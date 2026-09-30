// src/wal.cpp —— docs/m2-design.md §4 的逐字实现
#include "wal.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {
namespace {

constexpr uint32_t kMaxResyncProbeCandidates = 4096;              // 重同步探针的 CRC 计算上限
constexpr size_t kMaxResyncProbeBytes = 8u * 1024 * 1024;        // 重同步探针的扫描字节上限
constexpr size_t kZeroSentinelLen = 7;

// 头字段合法性（先于 CRC —— 畸形 length 绝不能进 CRC/越界读路径）
bool DecodeHeader(const char* p, uint32_t* crc, uint32_t* len, uint8_t* type) {
  *crc = DecodeFixed32(p);
  *len = static_cast<uint32_t>(static_cast<unsigned char>(p[4])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[5])) << 8);
  *type = static_cast<uint8_t>(p[6]);
  const bool type_ok =
      (*type == kFullType || *type == kFirstType || *type == kMiddleType || *type == kLastType);
  if (!type_ok) return false;
  if (*len == 0 || *len > kWALMaxPayload) return false;
  return true;
}

bool IsZeroSentinel(const char* p) {
  return DecodeFixed32(p) == 0 && p[4] == 0 && p[5] == 0 && p[6] == 0;
}

uint32_t FragmentCRC(const char* prefix3, const char* payload, size_t n) {
  return crc32c::Extend(crc32c::Value(prefix3, 3), payload, n);
}

}  // namespace

bool WALHasValidFragmentAfter(Env* env, const std::string& fname, uint64_t offset) {
  SequentialFile* raw = nullptr;
  if (!env->NewSequentialFile(fname, &raw).ok() || raw == nullptr) return false;
  std::unique_ptr<SequentialFile> file(raw);
  if (offset > 0 && !file->Skip(offset).ok()) return false;

  std::string rest;
  char scratch[65536];
  while (rest.size() < kMaxResyncProbeBytes) {
    Slice piece;
    if (!file->Read(sizeof(scratch), &piece, scratch).ok() || piece.empty()) break;
    rest.append(piece.data(), piece.size());
  }
  const size_t n = rest.size();
  if (n < kWALHeaderSize) return false;

  uint32_t candidates = 0;
  for (size_t pos = 0; pos + kWALHeaderSize <= n && candidates < kMaxResyncProbeCandidates; ++pos) {
    uint32_t crc = 0;
    uint32_t len = 0;
    uint8_t type = 0;
    if (!DecodeHeader(rest.data() + pos, &crc, &len, &type)) continue;
    if (pos + kWALHeaderSize + len > n) continue;
    ++candidates;
    if (FragmentCRC(rest.data() + pos + 4, rest.data() + pos + kWALHeaderSize, len) == crc) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// WALWriter
// ---------------------------------------------------------------------------
WALWriter::WALWriter(Env* env, std::string fname) : env_(env), fname_(std::move(fname)) {}

WALWriter::~WALWriter() {
  if (!closed_ && file_ != nullptr) file_->Close();   // 尽力而为；正常路径必须显式 Close
}

Status WALWriter::Open(bool append) {
  WritableFile* raw = nullptr;
  Status s;
  if (append) {
    s = env_->NewAppendableFile(fname_, &raw);
  } else {
    s = env_->NewWritableFile(fname_, &raw);
  }
  if (!s.ok()) return s;
  if (raw == nullptr) return Status::IOError("WALWriter::Open: env returned a null file", fname_);
  file_.reset(raw);
  closed_ = false;
  if (append) {
    uint64_t size = 0;
    const Status gs = env_->GetFileSize(fname_, &size);
    file_size_ = gs.ok() ? size : 0;
    block_offset_ = file_size_ % kWALBlockSize;
  } else {
    file_size_ = 0;
    block_offset_ = 0;
  }
  return Status::OK();
}

Status WALWriter::Append(const Slice& record) {
  // R1：只取 io_mu_（**不**取 sync_mu_）—— Append 绝不允许排在在飞的 fsync 后面。
  std::lock_guard<std::mutex> l(io_mu_);
  if (!error_.ok()) return error_;                     // 粘性 fail-stop（D11）
  if (closed_ || file_ == nullptr) {
    return Status::IOError("WALWriter::Append: writer is not open", fname_);
  }
  if (record.empty()) {
    return Status::InvalidArgument("WALWriter::Append: empty record (每个片段都必须 length >= 1)");
  }
  if (record.size() > kMaxLogicalRecordSize) {
    return Status::InvalidArgument("WALWriter::Append: record too large", std::to_string(record.size()));
  }

  // 不修改 M1 冻结的 Slice（它没有 remove_prefix），用指针 + 剩余长度推进
  const char* ptr = record.data();
  size_t left = record.size();
  bool begin = true;
  while (true) {
    // 修订（#1 回退 #0，docs/m2-design.md §4.2）：剩余 <= 7 时也必须补 padding。
    // 若只剩 7 字节，头之后 avail == 0 ⇒ 会写出 length == 0 的非法片段，而 reader 规定 len == 0 即 PARSE_FAIL。
    if (kWALBlockSize - block_offset_ <= kWALHeaderSize) {
      const size_t pad = kWALBlockSize - static_cast<size_t>(block_offset_);
      if (pad > 0) {
        char zeros[kWALHeaderSize] = {0, 0, 0, 0, 0, 0, 0};   // pad <= 6
        const Status s = file_->Append(Slice(zeros, pad));
        if (!s.ok()) {
          error_ = s;
          return s;
        }
        file_size_ += pad;
      }
      block_offset_ = 0;
    }

    const size_t avail = kWALBlockSize - static_cast<size_t>(block_offset_) - kWALHeaderSize;
    const size_t frag = (left < avail) ? left : avail;
    const bool end = (left == frag);
    const uint8_t type = begin ? (end ? kFullType : kFirstType) : (end ? kLastType : kMiddleType);

    char header[kWALHeaderSize];
    header[4] = static_cast<char>(frag & 0xffu);
    header[5] = static_cast<char>((frag >> 8) & 0xffu);
    header[6] = static_cast<char>(type);
    // CRC 覆盖面 = length(2) || type(1) || payload（design §4.4 / D2：比 LevelDB 多覆盖 length）
    const uint32_t crc = FragmentCRC(header + 4, ptr, frag);
    const char crc_bytes[4] = {static_cast<char>(crc & 0xffu), static_cast<char>((crc >> 8) & 0xffu),
                               static_cast<char>((crc >> 16) & 0xffu),
                               static_cast<char>((crc >> 24) & 0xffu)};
    std::memcpy(header, crc_bytes, 4);

    Status s = file_->Append(Slice(header, kWALHeaderSize));
    if (s.ok() && frag > 0) s = file_->Append(Slice(ptr, frag));
    if (!s.ok()) {
      error_ = s;
      return s;
    }
    file_size_ += kWALHeaderSize + frag;
    block_offset_ += kWALHeaderSize + frag;
    ptr += frag;
    left -= frag;
    begin = false;
    if (end) break;
  }
  return Status::OK();
}

Status WALWriter::Sync() {
  // R1：sync_mu_ 覆盖真正的 fsync（并保证与 Close 的关文件互斥）；io_mu_ 只在读状态/写粘性
  // 错误时短暂持有 ⇒ 并发的 Append 不会排在 fsync 后面（它只取 io_mu_）。
  // 锁序：sync_mu_ → io_mu_（本类内部唯一顺序，见 wal.h 的注释）。
  std::lock_guard<std::mutex> sl(sync_mu_);
  {
    std::lock_guard<std::mutex> l(io_mu_);
    if (!error_.ok()) return error_;
    if (closed_ || file_ == nullptr) {
      return Status::IOError("WALWriter::Sync: writer is not open", fname_);
    }
  }
  const Status s = file_->Sync();      // 持 sync_mu_，**不**持 io_mu_
  if (!s.ok()) {
    std::lock_guard<std::mutex> l(io_mu_);
    error_ = s;   // 粘性：偏移已不可信，绝不允许后续静默恢复
  }
  return s;
}

Status WALWriter::Close() {
  // 与 fsync 互斥：绝不在 in-flight fsync 期间关掉同一个文件句柄。
  std::lock_guard<std::mutex> sl(sync_mu_);
  std::lock_guard<std::mutex> l(io_mu_);
  if (closed_) return Status::OK();
  closed_ = true;
  if (file_ == nullptr) return Status::OK();
  return file_->Close();
}

// ---------------------------------------------------------------------------
// WALReader
// ---------------------------------------------------------------------------
WALReader::WALReader(Env* env, std::string fname) : env_(env), fname_(std::move(fname)) {}

WALReader::~WALReader() = default;

Status WALReader::ReadAll(const EmitFn& emit, WALScanResult* result) {
  *result = WALScanResult();
  SequentialFile* raw = nullptr;
  Status s = env_->NewSequentialFile(fname_, &raw);
  if (!s.ok()) return s;
  if (raw == nullptr) return Status::IOError("WALReader::ReadAll: env returned a null file", fname_);
  std::unique_ptr<SequentialFile> file(raw);

  std::vector<char> block(kWALBlockSize);
  uint64_t offset = 0;          // 当前块起始偏移
  uint64_t last_good_end = 0;   // 最后一条完整 record 的结束偏移
  bool in_frag = false;
  std::string frag_buf;

  auto finish = [&](WALScanVerdict verdict, uint64_t failure_off, const std::string& detail) -> Status {
    result->verdict = verdict;
    result->last_good_end = last_good_end;
    result->failure_offset = failure_off;
    result->detail = detail;
    if (verdict == WALScanVerdict::kParseFail) {
      result->valid_record_after_failure = WALHasValidFragmentAfter(env_, fname_, failure_off + 1);
    }
    return Status::OK();
  };

  while (true) {
    size_t got = 0;
    while (got < kWALBlockSize) {
      Slice piece;
      char scratch[4096];
      const size_t want = std::min(sizeof(scratch), kWALBlockSize - got);
      s = file->Read(want, &piece, scratch);
      if (!s.ok()) return s;
      if (piece.empty()) break;                  // EOF
      std::memcpy(block.data() + got, piece.data(), piece.size());
      got += piece.size();
    }
    const bool full_block = (got == kWALBlockSize);
    size_t p = 0;
    while (p < got) {
      const uint64_t abs = offset + p;
      if (p + kWALHeaderSize > got) {
        if (full_block) { p = got; break; }      // 块尾不足一个头：合法 padding 区
        return finish(WALScanVerdict::kTailResidue, abs, "块尾字节不足一个 record 头");
      }
      const char* h = block.data() + p;
      if (IsZeroSentinel(h)) {
        if (full_block) { p = got; break; }      // 合法 padding 哨兵 → 跳到块尾
        return finish(WALScanVerdict::kTailResidue, abs, "padding 哨兵之后文件结束");
      }
      uint32_t crc = 0;
      uint32_t len = 0;
      uint8_t type = 0;
      if (!DecodeHeader(h, &crc, &len, &type)) {
        return finish(WALScanVerdict::kParseFail, abs, "非法 record 头（type 不在 1..4 或 length 越界）");
      }
      if (p + kWALHeaderSize + len > got) {
        if (full_block) {
          return finish(WALScanVerdict::kParseFail, abs, "满块内 payload 越界（length 与块边界不一致）");
        }
        return finish(WALScanVerdict::kTailResidue, abs, "payload 被截断");
      }
      if (FragmentCRC(h + 4, h + kWALHeaderSize, len) != crc) {
        return finish(WALScanVerdict::kParseFail, abs, "CRC 不符");
      }
      if (type == kFullType) {
        if (in_frag) return finish(WALScanVerdict::kParseFail, abs, "kFullType 出现在未完成的多片段 record 内");
        emit(Slice(h + kWALHeaderSize, len));
        last_good_end = abs + kWALHeaderSize + len;
      } else if (type == kFirstType) {
        if (in_frag) return finish(WALScanVerdict::kParseFail, abs, "kFirstType 出现在未完成的多片段 record 内");
        in_frag = true;
        frag_buf.assign(h + kWALHeaderSize, len);
      } else {
        if (!in_frag) {
          return finish(WALScanVerdict::kParseFail, abs,
                        type == kMiddleType ? "kMiddleType 缺少 kFirstType" : "kLastType 缺少 kFirstType");
        }
        if (frag_buf.size() + len > kMaxLogicalRecordSize) {
          return finish(WALScanVerdict::kParseFail, abs, "重组缓冲超过 kMaxLogicalRecordSize");
        }
        frag_buf.append(h + kWALHeaderSize, len);
        if (type == kLastType) {
          emit(Slice(frag_buf));
          in_frag = false;
          frag_buf.clear();
          last_good_end = abs + kWALHeaderSize + len;
        }
      }
      p += kWALHeaderSize + len;
    }
    offset += got;
    if (!full_block) break;                      // EOF
  }

  if (in_frag) return finish(WALScanVerdict::kTailResidue, offset, "文件在跨块 record 中间结束");
  return finish(WALScanVerdict::kClean, 0, "");
}

}  // namespace lsm
