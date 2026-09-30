// src/write_batch.cpp —— §13.1 编码/解析（M5.2）
//
// 契约：docs/m5-design.md §5.1（接口）、§5.4（预校验）、§4 §13（编码）、docs/protocol.md §13。
// 编码与 M2 的 WAL batch payload（`src/db_impl.cpp` 的 `ParseBatch`）**逐字相同**：M5 只把它
// 提到公共 API，**不改编码**（§13.1 末条）。
#include "write_batch.h"

#include "util/coding.h"

namespace lsm {

namespace {

// 条目的编码前缀：type(1B) ‖ key_len(varint32) ‖ key [‖ value_len(varint32) ‖ value]。
void AppendEntry(std::string* dst, ValueType type, const Slice& key, const Slice& value) {
  dst->push_back(static_cast<char>(type));
  PutVarint32(dst, static_cast<uint32_t>(key.size()));
  dst->append(key.data(), key.size());
  if (type == kTypeValue) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
  }
}

// 解一条 entry；失败返回 false 且不写出任何结果。
bool ParseEntry(Slice* input, ValueType* type, Slice* key, Slice* value) {
  if (input->empty()) return false;
  const uint8_t t = static_cast<uint8_t>((*input)[0]);
  if (t != kTypeValue && t != kTypeDeletion) return false;
  Slice rest(input->data() + 1, input->size() - 1);
  uint32_t klen = 0;
  if (!GetVarint32(&rest, &klen)) return false;
  if (klen == 0 || klen > kMaxUserKeySize || klen > rest.size()) return false;
  const Slice k(rest.data(), klen);
  rest = Slice(rest.data() + klen, rest.size() - klen);
  Slice v;
  if (t == kTypeValue) {
    uint32_t vlen = 0;
    if (!GetVarint32(&rest, &vlen)) return false;
    if (vlen > rest.size()) return false;
    v = Slice(rest.data(), vlen);
    rest = Slice(rest.data() + vlen, rest.size() - vlen);
  }
  *type = static_cast<ValueType>(t);
  *key = k;
  *value = v;
  *input = rest;
  return true;
}

// 原地覆写 count 字段（rep_[8..12)）：长度不变，因此不破坏已追加的 entry。
void OverwriteCount(std::string* rep, uint32_t count) {
  std::string enc;
  PutFixed32(&enc, count);
  rep->replace(8, 4, enc);
}

}  // namespace

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::WriteBatch(const Slice& raw_data) : rep_(raw_data.data(), raw_data.size()) {}

WriteBatch::~WriteBatch() = default;

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeaderSize, 0);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
  // count 到顶后不再增长（真正的拒绝由 DB::Write 的 Validate 做，§13.3）；
  // 这里只保证 count 永远落在 [0, kMaxCount] 内。
  const uint32_t count = static_cast<uint32_t>(Count());
  if (count >= kMaxCount) return;
  OverwriteCount(&rep_, count + 1);
  AppendEntry(&rep_, kTypeValue, key, value);
}

void WriteBatch::Delete(const Slice& key) {
  const uint32_t count = static_cast<uint32_t>(Count());
  if (count >= kMaxCount) return;
  OverwriteCount(&rep_, count + 1);
  AppendEntry(&rep_, kTypeDeletion, key, Slice());
}

size_t WriteBatch::Count() const {
  if (rep_.size() < kHeaderSize) return 0;
  return DecodeFixed32(rep_.data() + 8);
}

SequenceNumber WriteBatch::Sequence() const {
  if (rep_.size() < kHeaderSize) return 0;
  return DecodeFixed64(rep_.data());
}

void WriteBatch::SetSequence(SequenceNumber seq) {
  if (rep_.size() < kHeaderSize) rep_.resize(kHeaderSize, 0);
  std::string enc;
  PutFixed64(&enc, seq);
  rep_.replace(0, 8, enc);
}

Status WriteBatch::Iterate(Handler* handler) const {
  if (handler == nullptr) return Status::InvalidArgument("WriteBatch::Iterate: null handler");
  Slice input(rep_);
  if (input.size() < kHeaderSize) {
    return Status::Corruption("WriteBatch::Iterate", "rep_ 不足 12 字节头");
  }
  const uint32_t count = DecodeFixed32(input.data() + 8);
  input = Slice(input.data() + kHeaderSize, input.size() - kHeaderSize);
  if (count == 0 || count > kMaxCount) {
    return Status::Corruption("WriteBatch::Iterate", "count 越界：" + std::to_string(count));
  }
  for (uint32_t i = 0; i < count; ++i) {
    ValueType type = kTypeValue;
    Slice key;
    Slice value;
    if (!ParseEntry(&input, &type, &key, &value)) {
      return Status::Corruption("WriteBatch::Iterate", "entry " + std::to_string(i) + " 解析失败");
    }
    if (type == kTypeValue) {
      handler->Put(key, value);
    } else {
      handler->Delete(key);
    }
  }
  if (!input.empty()) {
    return Status::Corruption("WriteBatch::Iterate",
                              "entry 解完后仍有 " + std::to_string(input.size()) + " 字节剩余");
  }
  return Status::OK();
}

Status WriteBatch::Validate(uint32_t* count, size_t* entry_bytes, uint64_t* user_bytes) const {
  Slice input(rep_);
  if (input.size() < kHeaderSize) {
    return Status::Corruption("WriteBatch::Validate", "rep_ 不足 12 字节头");
  }
  const SequenceNumber seq = DecodeFixed64(input.data());
  const uint32_t c = DecodeFixed32(input.data() + 8);
  input = Slice(input.data() + kHeaderSize, input.size() - kHeaderSize);
  if (c == 0) {
    return Status::InvalidArgument("WriteBatch::Validate", "count == 0（空批不允许提交）");
  }
  if (c > kMaxCount) {
    return Status::InvalidArgument("WriteBatch::Validate",
                                   "count 超过 kMaxCount：" + std::to_string(c));
  }
  if (seq + static_cast<SequenceNumber>(c) - 1 > kMaxSequenceNumber) {
    return Status::InvalidArgument("WriteBatch::Validate",
                                   "sequence + count - 1 超过 kMaxSequenceNumber");
  }
  // §13.3：`ByteSize() + 16 <= kMaxLogicalRecordSize`（与单条写的既有校验同形）。
  // 放在**解析之前**：整批超限时不需要先走一遍解码。
  if (rep_.size() + 16 > kMaxBytes) {
    return Status::InvalidArgument("WriteBatch::Validate",
                                   "batch 超过 kMaxLogicalRecordSize：" +
                                       std::to_string(rep_.size() + 16));
  }
  // entry_bytes = Σ(该条 entry 的编码字节)：用「解析前剩余 - 解析后剩余」直接量出，
  // 与单条写的既有口径（1 + varint(klen) + klen + [varint(vlen) + vlen]）逐字一致。
  Slice rest = input;
  size_t bytes = 0;
  uint64_t user = 0;
  for (uint32_t i = 0; i < c; ++i) {
    const size_t before = rest.size();
    ValueType type = kTypeValue;
    Slice key;
    Slice value;
    if (!ParseEntry(&rest, &type, &key, &value)) {
      return Status::Corruption("WriteBatch::Validate", "entry " + std::to_string(i) + " 解析失败");
    }
    bytes += before - rest.size();
    user += key.size() + value.size();
  }
  if (!rest.empty()) {
    return Status::Corruption("WriteBatch::Validate",
                              "entry 解完后仍有 " + std::to_string(rest.size()) + " 字节剩余");
  }
  if (count != nullptr) *count = c;
  if (entry_bytes != nullptr) *entry_bytes = bytes;
  if (user_bytes != nullptr) *user_bytes = user;
  return Status::OK();
}

}  // namespace lsm
