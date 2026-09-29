// src/memtable.cpp
#include "memtable.h"

#include <cstring>

#include "util/coding.h"

namespace lsm {
namespace {

bool DecodeEntryValue(const Slice& entry, Slice* value) {
  Slice input = entry;
  uint32_t len = 0;
  if (!GetVarint32(&input, &len)) return false;
  if (len < kInternalKeyMinSize || len > input.size()) return false;
  input = Slice(input.data() + len, input.size() - len);
  if (!GetVarint32(&input, &len) || len > input.size()) return false;
  *value = Slice(input.data(), len);
  return true;
}

// 从条目里解出 internal_key（protocol §7）。输入必须是本类写入的合法编码。
bool DecodeEntryInternalKey(const Slice& entry, Slice* internal_key) {
  Slice input = entry;
  uint32_t len = 0;
  if (!GetVarint32(&input, &len)) return false;
  // protocol §6 的 MUST：解码前必须校验长度 >= kInternalKeyMinSize。
  // 少了这一条，长度 5 的 internal_key 会让 ExtractUserKey 的 size_t 下溢成 ~2^64，
  // 进而在 Slice::compare 里越界读（#4 评审阻断项 2 的 ASan 复现）。
  if (len < kInternalKeyMinSize || len > input.size()) return false;
  *internal_key = Slice(input.data(), len);
  return true;
}

// 探针条目 varint32(len) | internal_key | varint32(0)，用于在跳表里按 internal key 定位。
// 小 key 走栈缓冲（避免每次 Get 都堆分配），大 key 走 std::string 兜底（LevelDB LookupKey 同口径）。
class ProbeEntry {
 public:
  explicit ProbeEntry(const Slice& internal_key) {
    const size_t needed =
        static_cast<size_t>(VarintLength(internal_key.size())) + internal_key.size() + 1;
    char* buf = nullptr;
    if (needed <= sizeof(space_)) {
      buf = space_;
    } else {
      heap_.resize(needed);
      buf = &heap_[0];
    }
    char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key.size()));
    if (!internal_key.empty()) std::memcpy(p, internal_key.data(), internal_key.size());
    p += internal_key.size();
    p = EncodeVarint32(p, 0);
    slice_ = Slice(buf, static_cast<size_t>(p - buf));
  }
  ProbeEntry(const ProbeEntry&) = delete;
  ProbeEntry& operator=(const ProbeEntry&) = delete;
  const Slice& slice() const { return slice_; }

 private:
  char space_[200];
  std::string heap_;
  Slice slice_;
};

// 内部序迭代器：把跳表条目解成 (internal key, value)（design §4.4）。
class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(const Skiplist* list) : iter_(list), state_(kBeforeFirst) {}

  // 三态状态机与用户视图同构（design §4.4）：内部序迭代器也要满足
  // 「kPastEnd + Prev 回到最后一条」「kBeforeFirst + Prev 保持 !Valid」「Next 越界后不再变化」。
  bool Valid() const override { return state_ == kValid; }
  void SeekToFirst() override {
    iter_.SeekToFirst();
    state_ = iter_.Valid() ? kValid : kPastEnd;
  }
  void SeekToLast() override {
    iter_.SeekToLast();
    state_ = iter_.Valid() ? kValid : kBeforeFirst;
  }
  void Seek(const Slice& target) override {
    const ProbeEntry probe(target);
    iter_.Seek(probe.slice());
    state_ = iter_.Valid() ? kValid : kPastEnd;
  }
  void Next() override {
    if (state_ != kValid) return;
    iter_.Next();
    if (!iter_.Valid()) state_ = kPastEnd;
  }
  void Prev() override {
    if (state_ == kBeforeFirst) return;
    if (state_ == kPastEnd) {
      iter_.SeekToLast();
      state_ = iter_.Valid() ? kValid : kBeforeFirst;
      return;
    }
    iter_.Prev();
    if (!iter_.Valid()) state_ = kBeforeFirst;
  }

  Slice key() const override {
    Slice internal_key;
    if (!DecodeEntryInternalKey(iter_.key(), &internal_key)) return Slice();
    return internal_key;
  }
  Slice value() const override {
    Slice input = iter_.key();
    uint32_t len = 0;
    if (!GetVarint32(&input, &len) || len > input.size()) return Slice();
    input = Slice(input.data() + len, input.size() - len);
    if (!GetVarint32(&input, &len) || len > input.size()) return Slice();
    return Slice(input.data(), len);
  }
  Status status() const override { return Status::OK(); }

 private:
  enum State { kBeforeFirst, kValid, kPastEnd };
  Skiplist::Iterator iter_;
  State state_;
};

}  // namespace

int MemTableKeyComparator::Compare(const Slice& a, const Slice& b) const {
  Slice ia;
  Slice ib;
  if (DecodeEntryInternalKey(a, &ia) && DecodeEntryInternalKey(b, &ib)) {
    const int c = internal_comparator_->Compare(ia, ib);
    if (c != 0) return c;
    // internal key 按比较器等价（注意：**不一定字节相同** —— 自定义比较器下 "Key"/"key" 等价）。
    // 此时只用 **value 段** 定序，绝不能用「整条条目的字节序」：探针条目（空 value）的 user key
    // 字节形态可能与真实条目不同，整条字节序会把逻辑相等的真实条目排到探针之前，
    // 于是 Seek 跳过它、Get 落空（#4 评审阻断项 1 的更深一层根因）。
    // 用 value 定序还能保证探针（空 value）≤ 同 internal key 的任何真实条目，Seek 必能命中。
    Slice va;
    Slice vb;
    if (DecodeEntryValue(a, &va) && DecodeEntryValue(b, &vb)) return va.compare(vb);
  }
  // 任一侧畸形：退化为整条字节序。只用于保证内存安全与全序，正常路径不会走到。
  return a.compare(b);
}

MemTable::MemTable(const InternalKeyComparator& internal_comparator, size_t write_buffer_size)
    : internal_comparator_(&internal_comparator),
      write_buffer_size_(write_buffer_size),
      key_comparator_(&internal_comparator),
      arena_(),
      skiplist_(&arena_, &key_comparator_) {}

Status MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  if (key.empty()) {
    return Status::InvalidArgument("MemTable::Add: empty user key");
  }
  if (key.size() > kMaxUserKeySize) {
    return Status::InvalidArgument("MemTable::Add: user key too large", std::to_string(key.size()));
  }
  if (seq > kMaxSequenceNumber) {
    return Status::InvalidArgument("MemTable::Add: sequence out of range", std::to_string(seq));
  }
  if (type != kTypeValue && type != kTypeDeletion) {
    return Status::InvalidArgument("MemTable::Add: bad value type",
                                   std::to_string(static_cast<int>(type)));
  }

  // 写前判（design §8.1）：先看是否已冻结或本次是否触顶；触顶则冻结并返回 kFrozen。
  // 因此最后一次成功写入可以让用量略微超过上限（与 LevelDB 同口径），而被拒这次绝不写入。
  if (IsFrozen() || ApproximateMemoryUsage() >= write_buffer_size_) {
    Freeze();
    return Status::Frozen("MemTable: write buffer full, memtable is read-only",
                          std::to_string(write_buffer_size_));
  }

  const size_t internal_key_size = key.size() + kInternalKeyTrailerSize;
  const size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) +
                             internal_key_size + static_cast<size_t>(VarintLength(value.size())) +
                             value.size();
  char* const buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  PutTrailerLE(p, PackTrailer(seq, type));
  p += kInternalKeyTrailerSize;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  if (!value.empty()) std::memcpy(p, value.data(), value.size());

  skiplist_.Insert(Slice(buf, encoded_len));
  ++entry_count_;
  return Status::OK();
}

MemTable::GetResult MemTable::Get(const Slice& lookup_key, std::string* value) const {
  // 调用方给的不是合法 lookup key（user_key || trailer，至少 8 字节）时直接判未命中，
  // 而不是把控制流送进会做 size_t 下溢的路径（protocol §6 / #4 评审阻断项 2）。
  if (lookup_key.size() < kInternalKeyMinSize) {
    if (value != nullptr) value->clear();
    return GetResult::kNotFound;
  }
  const ProbeEntry probe(lookup_key);
  Skiplist::Iterator it(&skiplist_);
  it.Seek(probe.slice());
  if (!it.Valid()) return GetResult::kNotFound;

  Slice internal_key;
  if (!DecodeEntryInternalKey(it.key(), &internal_key)) return GetResult::kNotFound;
  Slice user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  if (!ParseInternalKey(internal_key, &user_key, &seq, &type)) return GetResult::kNotFound;
  // 等价判定必须走注入的 user comparator，不能退化成逐字节比较：
  // 否则「排序说相等、Get 说不等」——自定义比较器（如大小写不敏感）下 Get 会命中不了
  // （#4 评审阻断项 1 的复现）。protocol §6.1 里的「字节序」是默认 BytewiseComparator 下的特例。
  if (internal_comparator_->user_comparator()->Compare(user_key, ExtractUserKey(lookup_key)) != 0) {
    return GetResult::kNotFound;
  }

  if (type == kTypeDeletion) {
    if (value != nullptr) value->clear();
    return GetResult::kDeleted;
  }

  Slice input = it.key();
  uint32_t len = 0;
  if (!GetVarint32(&input, &len) || len > input.size()) return GetResult::kNotFound;
  input = Slice(input.data() + len, input.size() - len);
  if (!GetVarint32(&input, &len) || len > input.size()) return GetResult::kNotFound;
  if (value != nullptr) value->assign(input.data(), len);
  return GetResult::kFound;
}

Iterator* MemTable::NewIterator() const { return new MemTableIterator(&skiplist_); }

bool MemTable::Freeze() {
  if (frozen_.load(std::memory_order_acquire)) return false;
  frozen_.store(true, std::memory_order_release);
  return true;
}

}  // namespace lsm
