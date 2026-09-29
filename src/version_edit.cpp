// src/version_edit.cpp —— META（M3，定宽全量快照）与 MANIFEST（M4，varint 差分）两套编解码。
//   docs/m3-design.md §10.8、docs/m4-design.md §3.2/§3.3、docs/protocol.md §11.2/§11.3
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
constexpr size_t kMaxEditComparatorName = 4096;
constexpr size_t kMaxEditKeyLen = 1024 * 1024;
constexpr size_t kMaxEditFieldCount = 1u << 22;     // 防畸形 tag 流撑爆

bool ValidInternalKey(const Slice& key) {
  Slice uk;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  if (key.size() < kInternalKeyMinSize || key.size() > kMaxEditKeyLen) return false;
  if (!ParseInternalKey(key, &uk, &seq, &type)) return false;
  return !uk.empty();
}

const InternalKeyComparator& EditIcmp() {
  static const InternalKeyComparator kIcmp(BytewiseComparator());
  return kIcmp;
}

std::string OffsetWhy(const std::string& msg, size_t offset) {
  return msg + " @offset=" + std::to_string(offset);
}

}  // namespace

uint64_t FileMetaData::smallest_user_key_size() const {
  Slice uk;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  if (!ParseInternalKey(Slice(smallest), &uk, &seq, &type)) return 0;
  return uk.size();
}

void VersionEdit::Clear() {
  has_comparator_ = false;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_min_log_number_to_keep_ = false;
  comparator_name_.clear();
  log_number_ = 0;
  min_log_number_to_keep_ = 1;
  next_file_number_ = 1;
  added_.clear();
  deleted_.clear();
  flat_files_.clear();
}

void VersionEdit::AddFile(int level, const FileMetaData& f) {
  added_.emplace_back(level, f);
  flat_files_.push_back(f);
}

std::string VersionEdit::DebugString() const {
  std::string out = "VersionEdit{";
  if (has_comparator_) out += " comparator=" + comparator_name_;
  if (has_log_number_) out += " log=" + std::to_string(log_number_);
  if (has_next_file_number_) out += " next=" + std::to_string(next_file_number_);
  if (has_min_log_number_to_keep_) out += " min_log=" + std::to_string(min_log_number_to_keep_);
  out += " deleted=" + std::to_string(deleted_.size());
  out += " added=" + std::to_string(added_.size());
  out += " }";
  return out;
}

// ---------------------------------------------------------------------------
// M3：META 全量快照（定宽；兼容读入格式，逐字节不改）
// ---------------------------------------------------------------------------
bool VersionEdit::EncodeTo(std::string* dst) const {
  if (dst == nullptr) return false;
  std::string out;
  out.append(kMetaMagic, 4);
  PutFixed32(&out, kMetaFormatVersion);
  PutLengthPrefixedSlice(&out, Slice(comparator_name_));
  PutFixed64(&out, log_number_);
  PutFixed64(&out, min_log_number_to_keep_);
  PutFixed64(&out, next_file_number_);
  PutFixed32(&out, static_cast<uint32_t>(flat_files_.size()));
  for (const FileMetaData& f : flat_files_) {
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

  VersionEdit parsed;
  parsed.SetComparatorName(std::string(comparator.data(), comparator.size()));
  parsed.SetLogNumber(log_number);
  parsed.SetMinLogNumberToKeep(min_log);
  parsed.SetNextFileNumber(next_file);
  for (const FileMetaData& f : files) parsed.AddFile(f);
  *this = parsed;
  return true;
}

// ---------------------------------------------------------------------------
// M4：VersionEdit 差分 payload（varint tag 流，docs/protocol.md §11.3）
// ---------------------------------------------------------------------------
bool VersionEdit::EncodePayloadTo(std::string* dst) const {
  if (dst == nullptr) return false;
  std::string out;
  if (has_comparator_) {
    PutVarint32(&out, kTagComparator);
    PutLengthPrefixedSlice(&out, Slice(comparator_name_));
  }
  if (has_log_number_) {
    PutVarint32(&out, kTagLogNumber);
    PutVarint64(&out, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(&out, kTagNextFileNumber);
    PutVarint64(&out, next_file_number_);
  }
  if (has_min_log_number_to_keep_) {
    PutVarint32(&out, kTagMinLogNumberToKeep);
    PutVarint64(&out, min_log_number_to_keep_);
  }
  for (const auto& d : deleted_) {
    PutVarint32(&out, kTagDeletedFile);
    PutVarint32(&out, static_cast<uint32_t>(d.first));
    PutVarint64(&out, d.second);
  }
  for (const auto& a : added_) {
    PutVarint32(&out, kTagNewFile);
    PutVarint32(&out, static_cast<uint32_t>(a.first));
    PutVarint64(&out, a.second.number);
    PutVarint64(&out, a.second.file_size);
    PutVarint64(&out, a.second.max_sequence);
    PutLengthPrefixedSlice(&out, Slice(a.second.smallest));
    PutLengthPrefixedSlice(&out, Slice(a.second.largest));
  }
  dst->swap(out);
  return true;
}

bool VersionEdit::DecodePayloadFrom(const Slice& src, std::string* why) {
  const auto fail = [why](const std::string& msg) {
    if (why != nullptr) *why = msg;
    return false;
  };
  Slice in = src;
  VersionEdit parsed;
  size_t fields = 0;
  while (!in.empty()) {
    const size_t tag_offset = src.size() - in.size();
    uint32_t tag = 0;
    if (!GetVarint32(&in, &tag)) {
      return fail(OffsetWhy("VersionEdit tag 的 varint32 截断", tag_offset));
    }
    if (++fields > kMaxEditFieldCount) return fail("VersionEdit 字段数越界");
    switch (tag) {
      case kTagComparator: {
        Slice name;
        if (!GetLengthPrefixedSlice(&in, &name)) {
          return fail(OffsetWhy("tag=1 comparator 的长度前缀非法", tag_offset));
        }
        if (name.size() > kMaxEditComparatorName) return fail("comparator 名称过长");
        parsed.SetComparatorName(std::string(name.data(), name.size()));
        break;
      }
      case kTagLogNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&in, &v)) return fail(OffsetWhy("tag=2 log_number 的 varint64 截断", tag_offset));
        parsed.SetLogNumber(v);
        break;
      }
      case kTagNextFileNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&in, &v)) return fail(OffsetWhy("tag=3 next_file_number 的 varint64 截断", tag_offset));
        if (v == 0) return fail(OffsetWhy("tag=3 next_file_number == 0", tag_offset));
        parsed.SetNextFileNumber(v);
        break;
      }
      case kTagMinLogNumberToKeep: {
        uint64_t v = 0;
        if (!GetVarint64(&in, &v)) return fail(OffsetWhy("tag=4 min_log_number_to_keep 的 varint64 截断", tag_offset));
        if (v == 0) return fail(OffsetWhy("tag=4 min_log_number_to_keep == 0", tag_offset));
        parsed.SetMinLogNumberToKeep(v);
        break;
      }
      case kTagDeletedFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        if (!GetVarint32(&in, &level)) return fail(OffsetWhy("tag=5 level 的 varint32 截断", tag_offset));
        if (!GetVarint64(&in, &number)) return fail(OffsetWhy("tag=5 number 的 varint64 截断", tag_offset));
        if (level >= static_cast<uint32_t>(kNumLevels)) {
          return fail(OffsetWhy("tag=5 level 越界：" + std::to_string(level), tag_offset));
        }
        if (number == 0) return fail(OffsetWhy("tag=5 number == 0", tag_offset));
        parsed.DeleteFile(static_cast<int>(level), number);
        break;
      }
      case kTagNewFile: {
        uint32_t level = 0;
        uint64_t number = 0, file_size = 0, max_sequence = 0;
        Slice smallest, largest;
        if (!GetVarint32(&in, &level)) return fail(OffsetWhy("tag=6 level 的 varint32 截断", tag_offset));
        if (!GetVarint64(&in, &number)) return fail(OffsetWhy("tag=6 number 的 varint64 截断", tag_offset));
        if (!GetVarint64(&in, &file_size)) return fail(OffsetWhy("tag=6 file_size 的 varint64 截断", tag_offset));
        if (!GetVarint64(&in, &max_sequence)) return fail(OffsetWhy("tag=6 max_sequence 的 varint64 截断", tag_offset));
        if (!GetLengthPrefixedSlice(&in, &smallest)) return fail(OffsetWhy("tag=6 smallest 的长度前缀非法", tag_offset));
        if (!GetLengthPrefixedSlice(&in, &largest)) return fail(OffsetWhy("tag=6 largest 的长度前缀非法", tag_offset));
        if (level >= static_cast<uint32_t>(kNumLevels)) {
          return fail(OffsetWhy("tag=6 level 越界：" + std::to_string(level), tag_offset));
        }
        if (number == 0) return fail(OffsetWhy("tag=6 number == 0", tag_offset));
        if (file_size == 0) return fail(OffsetWhy("tag=6 file_size == 0", tag_offset));
        if (max_sequence > kMaxSequenceNumber) {
          return fail(OffsetWhy("tag=6 max_sequence 超过 kMaxSequenceNumber", tag_offset));
        }
        if (!ValidInternalKey(smallest)) return fail(OffsetWhy("tag=6 smallest 不是合法 internal key", tag_offset));
        if (!ValidInternalKey(largest)) return fail(OffsetWhy("tag=6 largest 不是合法 internal key", tag_offset));
        if (EditIcmp().Compare(smallest, largest) > 0) {
          return fail(OffsetWhy("tag=6 smallest > largest", tag_offset));
        }
        FileMetaData f;
        f.number = number;
        f.file_size = file_size;
        f.max_sequence = max_sequence;
        f.smallest.assign(smallest.data(), smallest.size());
        f.largest.assign(largest.data(), largest.size());
        for (const auto& a : parsed.added_files()) {
          if (a.second.number == number) {
            return fail(OffsetWhy("tag=6 number 重复：" + std::to_string(number), tag_offset));
          }
        }
        parsed.AddFile(static_cast<int>(level), f);
        break;
      }
      default:
        return fail(OffsetWhy("VersionEdit 未知 tag=" + std::to_string(tag), tag_offset));
    }
  }
  *this = parsed;
  return true;
}

// ---------------------------------------------------------------------------
// M4：MANIFEST record 帧（length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)，CRC 含长度）
// ---------------------------------------------------------------------------
bool EncodeManifestRecord(const VersionEdit& edit, std::string* dst) {
  if (dst == nullptr) return false;
  std::string payload;
  if (!edit.EncodePayloadTo(&payload)) return false;
  if (payload.empty()) return false;
  if (payload.size() > kMaxManifestRecordBytes) return false;
  std::string out;
  out.reserve(payload.size() + 9);
  PutFixed32(&out, static_cast<uint32_t>(payload.size()));
  out.push_back(static_cast<char>(kManifestRecordTypeVersionEdit));
  out.append(payload);
  PutFixed32(&out, crc32c::Value(out.data(), out.size()));
  dst->swap(out);
  return true;
}

ManifestReadStatus ReadManifestRecord(const Slice& buf, size_t* offset, VersionEdit* out,
                                      std::string* why) {
  const auto fail = [why](const std::string& msg) {
    if (why != nullptr) *why = msg;
    return ManifestReadStatus::kCorruption;
  };
  const size_t off = *offset;
  if (buf.size() - off < 5) return ManifestReadStatus::kTailResidue;
  const uint32_t length = DecodeFixed32(buf.data() + off);
  if (length == 0) return fail("MANIFEST record length == 0");
  if (length > kMaxManifestRecordBytes) {
    return fail("MANIFEST record length 越界：" + std::to_string(length));
  }
  const size_t total = 4u + 1u + static_cast<size_t>(length) + 4u;
  if (buf.size() - off < total) return ManifestReadStatus::kTailResidue;
  const uint8_t type = static_cast<uint8_t>(buf.data()[off + 4]);
  if (type != kManifestRecordTypeVersionEdit) {
    if (why != nullptr) *why = "MANIFEST record type 未知：" + std::to_string(type);
    return ManifestReadStatus::kNotSupported;
  }
  const uint32_t want = crc32c::Value(buf.data() + off, 4u + 1u + length);
  const uint32_t got = DecodeFixed32(buf.data() + off + 4u + 1u + length);
  if (want != got) return fail("MANIFEST record CRC 不符 @offset=" + std::to_string(off));
  VersionEdit edit;
  std::string decode_why;
  if (!edit.DecodePayloadFrom(Slice(buf.data() + off + 5, length), &decode_why)) {
    return fail("MANIFEST record payload 解码失败：" + decode_why);
  }
  *out = edit;
  *offset = off + total;
  return ManifestReadStatus::kOk;
}

}  // namespace lsm
