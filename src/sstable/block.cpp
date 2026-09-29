// src/sstable/block.cpp —— 数据块/索引块的构建、读取与结构校验（M3.1）
//
// 契约：docs/m3-design.md §3.2（布局与 restart 语义）、§3.6（块头/CRC 由上层负责，本文件不管 CRC）、
// 不变量 I24（restart_offset 严格单调且落在 entry 边界）、I25（类型/长度冗余自检，见 §3.6）。
// 纪律：**任何 entry 在读取前先校验**（shared/non_shared/vlen 与剩余字节），不得先读后判。
#include "sstable/block.h"

#include <algorithm>
#include <utility>

#include "util/coding.h"

namespace lsm {

// Slice 未提供 remove_prefix（M1 已冻结该接口），块内解析统一用游标：
// 只有**校验通过**之后才推进游标，杜绝"先读后判"导致的越界。
namespace {

struct Cursor {
  const char* p;
  size_t n;
  bool GetVarint32(uint32_t* v) {
    const char* start = p;
    const char* q = GetVarint32Ptr(p, p + n, v);
    if (q == nullptr) return false;
    n -= static_cast<size_t>(q - start);
    p = q;
    return true;
  }
  void Skip(size_t k) { p += k; n -= k; }  // 仅在 k <= n 时调用
};

}  // namespace

// ============================ BlockBuilder ============================

BlockBuilder::BlockBuilder(int restart_interval)
    : restart_interval_(restart_interval > 0 ? restart_interval : 1),
      counter_(0),
      num_entries_(0),
      finished_(false) {
  // §3.2：第一条 entry 必然是 restart 点；空块的 restart 数组恒为 [0]（payload 恰 8 字节）。
  restarts_.push_back(0);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  uint32_t shared = 0;
  if (counter_ < restart_interval_ && !buffer_.empty()) {
    // 组内：与**上一条已复原的完整 key** 求最长公共前缀（不是与上一条的 delta）。
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
  } else {
    // 新 restart 组：即使与上一组末尾有公共前缀也**不共享**（§3.2 写死的语义）。
    if (!buffer_.empty()) restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const Slice delta(key.data() + shared, key.size() - shared);
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, static_cast<uint32_t>(delta.size()));
  buffer_.append(delta.data(), delta.size());
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++counter_;
  ++num_entries_;
  finished_ = false;
}

void BlockBuilder::Reset() {
  buffer_.clear();
  finished_payload_.clear();
  last_key_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  num_entries_ = 0;
  finished_ = false;
}

bool BlockBuilder::empty() const { return buffer_.empty(); }

size_t BlockBuilder::NumRestarts() const { return restarts_.size(); }

size_t BlockBuilder::NumEntries() const { return num_entries_; }

size_t BlockBuilder::CurrentSizeEstimate() const {
  // entry 区 + restart 数组（含 count 字段本身）
  return buffer_.size() + 4 * (restarts_.size() + 1);
}

size_t BlockBuilder::EstimatedSizeAfter(const Slice& key, const Slice& value) const {
  // 与 Add 使用**同一份** shared 计算与 restart 追加规则；这是切块判据的唯一实现点。
  uint32_t shared = 0;
  if (counter_ < restart_interval_ && !buffer_.empty()) {
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
  }
  const uint32_t non_shared = static_cast<uint32_t>(key.size() - shared);
  const uint64_t entry_bytes = static_cast<uint64_t>(VarintLength(shared)) +
                               static_cast<uint64_t>(VarintLength(non_shared)) +
                               static_cast<uint64_t>(non_shared) +
                               static_cast<uint64_t>(VarintLength(value.size())) +
                               static_cast<uint64_t>(value.size());
  size_t restarts = restarts_.size();
  if (counter_ >= restart_interval_ && !buffer_.empty()) ++restarts;   // 本条会开一个新 restart 组
  return buffer_.size() + static_cast<size_t>(entry_bytes) + 4 * (restarts + 1);
}

Slice BlockBuilder::Finish() {
  if (!finished_) {
    finished_payload_.assign(buffer_);
    for (uint32_t r : restarts_) PutFixed32(&finished_payload_, r);
    PutFixed32(&finished_payload_, static_cast<uint32_t>(restarts_.size()));
    finished_ = true;
  }
  return Slice(finished_payload_);
}

// ============================ 结构校验 ============================

Status ValidatePayload(const Slice& payload) {
  if (payload.size() < kBlockMinPayload) {
    return Status::Corruption("Block", "payload smaller than the 8-byte minimum");
  }
  const char* p = payload.data();
  const uint32_t count = DecodeFixed32(p + payload.size() - 4);
  if (count == 0) return Status::Corruption("Block", "restart_count == 0");
  const size_t array_bytes = 4 * (static_cast<size_t>(count) + 1);
  if (payload.size() < array_bytes) {
    return Status::Corruption("Block", "restart array does not fit in payload");
  }
  const size_t entry_end = payload.size() - array_bytes;

  std::vector<uint32_t> rs(count);
  for (uint32_t i = 0; i < count; ++i) rs[i] = DecodeFixed32(p + entry_end + 4 * i);
  if (rs[0] != 0) return Status::Corruption("Block", "restart_offset[0] != 0");
  for (uint32_t i = 1; i < count; ++i) {
    if (rs[i] <= rs[i - 1]) {
      return Status::Corruption("Block", "restart offsets are not strictly increasing");
    }
  }
  if (entry_end == 0) {
    // §3.2：空块的 restart 数组恰为 [0]（0 指向"entry 区末尾"，即没有任何 entry）。
    if (count != 1) return Status::Corruption("Block", "empty block must have exactly one restart");
    return Status::OK();
  }

  // 顺序解析 entry 区：每条 entry 必须完整合法，且每个 restart 偏移**恰好**落在 entry 起点。
  std::string prev;
  size_t off = 0;
  uint32_t seen = 0;
  while (off < entry_end) {
    const bool group_start = (seen < count && rs[seen] == off);
    if (group_start) {
      ++seen;
      prev.clear();   // restart 点重置 prefix 状态（§3.2）
    } else if (seen == 0) {
      return Status::Corruption("Block", "entry area does not start at a restart offset");
    }

    Cursor cur{p + off, entry_end - off};
    uint32_t shared = 0, non_shared = 0, vlen = 0;
    if (!cur.GetVarint32(&shared)) return Status::Corruption("Block", "truncated shared");
    if (!cur.GetVarint32(&non_shared)) return Status::Corruption("Block", "truncated non_shared");
    if (non_shared == 0) return Status::Corruption("Block", "non_shared == 0");
    if (group_start && shared != 0) {
      return Status::Corruption("Block", "restart point must have shared == 0");
    }
    if (shared > prev.size()) return Status::Corruption("Block", "shared exceeds previous key length");
    if (cur.n < non_shared) return Status::Corruption("Block", "key delta exceeds remaining bytes");
    std::string key = prev.substr(0, shared);
    key.append(cur.p, non_shared);
    cur.Skip(non_shared);
    if (!cur.GetVarint32(&vlen)) return Status::Corruption("Block", "truncated value_len");
    if (cur.n < vlen) return Status::Corruption("Block", "value exceeds remaining bytes");

    prev = key;
    off = (entry_end - cur.n) + vlen;
  }
  if (off != entry_end) return Status::Corruption("Block", "entry area not fully consumed");
  if (seen != count) {
    return Status::Corruption("Block", "restart offset does not point at an entry boundary");
  }
  return Status::OK();
}

// ============================ BlockReader ============================

Status BlockReader::Open(const Slice& payload, std::unique_ptr<BlockReader>* out) {
  if (out == nullptr) return Status::InvalidArgument("BlockReader::Open", "null out");
  const Status v = ValidatePayload(payload);
  if (!v.ok()) return v;

  std::unique_ptr<BlockReader> r(new BlockReader());
  r->payload_ = payload;
  const uint32_t count = DecodeFixed32(payload.data() + payload.size() - 4);
  r->entry_area_end_ = payload.size() - 4 * (static_cast<size_t>(count) + 1);
  r->restarts_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    r->restarts_.push_back(DecodeFixed32(payload.data() + r->entry_area_end_ + 4 * i));
  }
  r->entry_offset_ = 0;
  r->valid_ = false;
  r->status_ = Status::OK();
  *out = std::move(r);
  return Status::OK();
}

Status BlockReader::DecodeEntry(size_t off, bool group_start, const std::string& prev_key,
                               std::string* key, Slice* value, size_t* next) const {
  if (off >= entry_area_end_) {
    return Status::Corruption("BlockReader", "entry offset out of range");
  }
  Cursor cur{payload_.data() + off, entry_area_end_ - off};
  uint32_t shared = 0, non_shared = 0, vlen = 0;
  if (!cur.GetVarint32(&shared) || !cur.GetVarint32(&non_shared)) {
    return Status::Corruption("BlockReader", "truncated entry header");
  }
  if (non_shared == 0) return Status::Corruption("BlockReader", "non_shared == 0");
  if (group_start && shared != 0) {
    return Status::Corruption("BlockReader", "restart point must have shared == 0");
  }
  if (cur.n < non_shared) {
    return Status::Corruption("BlockReader", "key delta exceeds remaining bytes");
  }
  if (group_start) {
    key->assign(cur.p, non_shared);
  } else {
    if (shared > prev_key.size()) {
      return Status::Corruption("BlockReader", "shared exceeds previous key length");
    }
    key->assign(prev_key, 0, shared);
    key->append(cur.p, non_shared);
  }
  cur.Skip(non_shared);
  if (!cur.GetVarint32(&vlen)) return Status::Corruption("BlockReader", "truncated value_len");
  if (cur.n < vlen) return Status::Corruption("BlockReader", "value exceeds remaining bytes");
  *value = Slice(cur.p, vlen);
  *next = (entry_area_end_ - cur.n) + vlen;
  return Status::OK();
}

bool BlockReader::IsRestartOffset(size_t off) const {
  for (uint32_t r : restarts_) {
    if (r == off) return true;
  }
  return false;
}

Status BlockReader::DecodeFromGroupStart(size_t target, std::string* key, Slice* value) const {
  size_t group_start = 0;
  for (uint32_t r : restarts_) {
    if (r <= target) group_start = r;
    else break;
  }
  std::string prev;
  std::string k;
  Slice v;
  size_t off = group_start;
  while (off < entry_area_end_) {
    size_t next = 0;
    const Status s = DecodeEntry(off, off == group_start, prev, &k, &v, &next);
    if (!s.ok()) return s;
    if (off == target) {
      *key = k;
      *value = v;
      return Status::OK();
    }
    prev = k;
    off = next;
  }
  return Status::Corruption("BlockReader", "target offset is not an entry boundary");
}

Status BlockReader::SetTo(size_t off) {
  std::string k;
  Slice v;
  const Status s = DecodeFromGroupStart(off, &k, &v);
  if (!s.ok()) {
    status_ = s;
    valid_ = false;
    return s;
  }
  key_ = k;
  value_ = v;
  entry_offset_ = off;
  valid_ = true;
  status_ = Status::OK();
  return status_;
}

size_t BlockReader::PrevOffset(size_t cur) const {
  if (cur == 0) return kNoEntry;
  size_t group_start = 0;
  for (uint32_t r : restarts_) {
    if (r < cur) group_start = r;
    else break;
  }
  size_t off = group_start;
  size_t prev = kNoEntry;
  std::string prev_key, k;
  Slice v;
  while (off < cur) {
    size_t next = 0;
    const Status s = DecodeEntry(off, off == group_start, prev_key, &k, &v, &next);
    if (!s.ok()) return kNoEntry;
    prev = off;
    prev_key = k;
    off = next;
  }
  return off == cur ? prev : kNoEntry;
}

Status BlockReader::SeekToFirst() {
  valid_ = false;
  status_ = Status::OK();
  if (entry_area_end_ == 0) return status_;   // 空块：Valid() == false 且不是错误
  return SetTo(0);
}

Status BlockReader::SeekToLast() {
  valid_ = false;
  status_ = Status::OK();
  const size_t off = PrevOffset(entry_area_end_);
  if (off == kNoEntry) return status_;
  return SetTo(off);
}

Status BlockReader::Seek(const Slice& target) {
  valid_ = false;
  status_ = Status::OK();
  if (entry_area_end_ == 0) return status_;
  const std::string t = target.ToString();

  // 二分：找第一个「restart 点 key >= target」的组；目标可能落在它**前一组**内（§3.3 的 >= 语义）。
  size_t left = 0;
  size_t right = restarts_.size();
  while (left < right) {
    const size_t mid = left + (right - left) / 2;
    std::string k;
    Slice v;
    const Status s = DecodeFromGroupStart(restarts_[mid], &k, &v);
    if (!s.ok()) {
      status_ = s;
      return s;
    }
    if (k < t) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  const size_t group = (left == 0) ? 0 : left - 1;

  // 从该组起点线性扫到第一个 key >= target
  size_t off = restarts_[group];
  std::string prev, k;
  Slice v;
  while (off < entry_area_end_) {
    size_t next = 0;
    const Status s = DecodeEntry(off, off == restarts_[group], prev, &k, &v, &next);
    if (!s.ok()) {
      status_ = s;
      return s;
    }
    if (k >= t) {
      key_ = k;
      value_ = v;
      entry_offset_ = off;
      valid_ = true;
      return status_;
    }
    prev = k;
    off = next;
  }
  return status_;   // 越过末尾：Valid() == false，status 仍为 OK
}

Status BlockReader::Next() {
  if (!valid_) return status_;
  const size_t off = entry_offset_;
  const bool group_start = IsRestartOffset(off);
  std::string k;
  Slice v;
  size_t next = 0;
  const Status s = DecodeEntry(off, group_start, group_start ? std::string() : key_, &k, &v, &next);
  if (!s.ok()) {
    status_ = s;
    valid_ = false;
    return s;
  }
  if (next >= entry_area_end_) {
    valid_ = false;
    return status_;
  }
  return SetTo(next);
}

Status BlockReader::Prev() {
  if (!valid_) return status_;
  const size_t off = PrevOffset(entry_offset_);
  if (off == kNoEntry) {
    valid_ = false;
    return status_;
  }
  return SetTo(off);
}

bool BlockReader::Valid() const { return valid_; }
Slice BlockReader::key() const { return Slice(key_); }
Slice BlockReader::value() const { return value_; }
Status BlockReader::status() const { return status_; }
size_t BlockReader::NumRestarts() const { return restarts_.size(); }
uint32_t BlockReader::RestartOffset(size_t i) const { return restarts_[i]; }

}  // namespace lsm
