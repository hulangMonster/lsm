// src/version_edit.cpp —— META 的位级编解码（docs/m3-design.md §10.8）
#include "version_edit.h"

#include <cstring>
#include <utility>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {
namespace {

constexpr size_t kMaxMetaComparatorName = 4096;
constexpr size_t kMaxMetaKeyLen = 1024 * 1024;      // internal key 上界（含防御余量）
constexpr uint32_t kMaxMetaFileCount = 1u << 20;    // 防畸形 count 撑爆

}  // namespace

bool VersionEdit::EncodeTo(std::string* dst) const {
  if (dst == nullptr) return false;
  std::string out;
  out.append(kMetaMagic, 4);
  PutFixed32(&out, kMetaFormatVersion);
  PutLengthPrefixedSlice(&out, Slice(comparator_name_));
  PutFixed64(&out, log_number_);
  PutFixed64(&out, min_log_number_to_keep_);
  PutFixed64(&out, next_file_number_);
  PutFixed32(&out, static_cast<uint32_t>(files_.size()));
  for (const FileMetaData& f : files_) {
    PutFixed64(&out, f.number);
    PutFixed64(&out, f.file_size);
    PutFixed64(&out, f.max_sequence);
    PutLengthPrefixedSlice(&out, Slice(f.smallest));
    PutLengthPrefixedSlice(&out, Slice(f.largest));
  }
  PutFixed32(&out, crc32c::Value(out.data(), out.size()));
  dst->swap(out);
  return true;
}

bool VersionEdit::DecodeFrom(const Slice& src, std::string* why) {
  const auto fail = [why](const std::string& msg) {
    if (why != nullptr) *why = msg;
    return false;
  };
  if (src.size() < 4) return fail("META 短于 tail CRC（" + std::to_string(src.size()) + " 字节）");
  // ① tail CRC 先于任何字段解析（CRC 覆盖面 = header ‖ file*，§10.8）。
  const size_t body_len = src.size() - 4;
  const uint32_t want_crc = crc32c::Value(src.data(), body_len);
  const uint32_t got_crc = DecodeFixed32(src.data() + body_len);
  if (want_crc != got_crc) {
    return fail("META tail CRC 不符（期望 " + std::to_string(want_crc) + "，实得 " +
                std::to_string(got_crc) + "）");
  }

  Slice in(src.data(), body_len);
  if (in.size() < 4 || std::memcmp(in.data(), kMetaMagic, 4) != 0) {
    return fail("META magic 不符（期望 \"LSMM\"）");
  }
  in = Slice(in.data() + 4, in.size() - 4);
  if (in.size() < 4) return fail("META 缺少 format_version");
  const uint32_t version = DecodeFixed32(in.data());
  if (version != kMetaFormatVersion) {
    return fail("META format_version=" + std::to_string(version) + " 不受支持（期望 1）");
  }
  in = Slice(in.data() + 4, in.size() - 4);

  Slice comparator;
  if (!GetLengthPrefixedSlice(&in, &comparator)) return fail("META comparator_name 长度前缀非法");
  if (comparator.size() > kMaxMetaComparatorName) {
    return fail("META comparator_name 过长：" + std::to_string(comparator.size()));
  }
  if (in.size() < 28) return fail("META 缺少 log_number/min_log/next_file/file_count");
  const uint64_t log_number = DecodeFixed64(in.data());
  const uint64_t min_log = DecodeFixed64(in.data() + 8);
  const uint64_t next_file = DecodeFixed64(in.data() + 16);
  const uint32_t file_count = DecodeFixed32(in.data() + 24);
  in = Slice(in.data() + 28, in.size() - 28);
  if (file_count > kMaxMetaFileCount) {
    return fail("META file_count 越界：" + std::to_string(file_count));
  }
  if (min_log == 0) return fail("META min_log_number_to_keep == 0（必须 >= 1）");
  if (next_file == 0) return fail("META next_file_number == 0（必须 >= 1）");

  std::vector<FileMetaData> files;
  files.reserve(file_count);
  for (uint32_t i = 0; i < file_count; ++i) {
    if (in.size() < 24) return fail("META file[" + std::to_string(i) + "] 头不足 24 字节");
    FileMetaData f;
    f.number = DecodeFixed64(in.data());
    f.file_size = DecodeFixed64(in.data() + 8);
    f.max_sequence = DecodeFixed64(in.data() + 16);
    in = Slice(in.data() + 24, in.size() - 24);
    Slice smallest;
    Slice largest;
    if (!GetLengthPrefixedSlice(&in, &smallest)) {
      return fail("META file[" + std::to_string(i) + "].smallest 长度前缀非法");
    }
    if (!GetLengthPrefixedSlice(&in, &largest)) {
      return fail("META file[" + std::to_string(i) + "].largest 长度前缀非法");
    }
    if (smallest.size() < kInternalKeyMinSize || smallest.size() > kMaxMetaKeyLen ||
        largest.size() < kInternalKeyMinSize || largest.size() > kMaxMetaKeyLen) {
      return fail("META file[" + std::to_string(i) + "] 的 internal key 长度非法");
    }
    if (f.number == 0) return fail("META file[" + std::to_string(i) + "].number == 0");
    if (f.max_sequence > kMaxSequenceNumber) {
      return fail("META file[" + std::to_string(i) + "].max_sequence 超过 kMaxSequenceNumber");
    }
    // internal key 自身的 type 必须合法（§6）：畸形键不得进版本。
    Slice uk;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(smallest, &uk, &seq, &type) ||
        !ParseInternalKey(largest, &uk, &seq, &type)) {
      return fail("META file[" + std::to_string(i) + "] 的 internal key 畸形");
    }
    f.smallest.assign(smallest.data(), smallest.size());
    f.largest.assign(largest.data(), largest.size());
    files.push_back(std::move(f));
  }
  if (!in.empty()) {
    return fail("META 在 file* 之后仍有 " + std::to_string(in.size()) + " 字节剩余");
  }

  // 全部校验通过后才写入 *this（失败路径保证 *this 不变）。
  VersionEdit parsed;
  parsed.SetComparatorName(std::string(comparator.data(), comparator.size()));
  parsed.SetLogNumber(log_number);
  parsed.SetMinLogNumberToKeep(min_log);
  parsed.SetNextFileNumber(next_file);
  for (const FileMetaData& f : files) parsed.AddFile(f);
  *this = parsed;
  return true;
}

}  // namespace lsm
