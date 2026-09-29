// src/db.cpp —— M1 内存实现 + 用户视图迭代器（docs/m1-design.md §9）
#include "db.h"

#include <memory>

#include "memtable.h"

namespace lsm {
namespace {

std::string TruncatedKey(const Slice& key) {
  const size_t n = key.size() < 64 ? key.size() : 64;
  return std::string(key.data(), n);
}

// 用户视图迭代器（design §4.4 的三态状态机）。
// 内部序是「user key 升序 + 同 key sequence 降序」，因此每个 user key 的第一个条目就是它的最新版本；
// 前向定位永远落在 run 起点，所以「最新版本」不需要额外判断，只需要跳过 tombstone 段。
class UserIterator : public Iterator {
 public:
  explicit UserIterator(const MemTable* mem)
      : internal_(mem->NewIterator()), state_(kBeforeFirst) {}
  ~UserIterator() override { delete internal_; }
  UserIterator(const UserIterator&) = delete;
  UserIterator& operator=(const UserIterator&) = delete;

  bool Valid() const override { return state_ == kValid; }

  void SeekToFirst() override {
    internal_->SeekToFirst();
    ScanForwardToVisible();
  }

  void SeekToLast() override {
    internal_->SeekToLast();
    if (!internal_->Valid()) {
      state_ = kBeforeFirst;
      return;
    }
    RewindToRunStart();
    ScanBackwardToVisible();
  }

  void Seek(const Slice& target) override {
    internal_->Seek(BuildLookupKey(target, kMaxSequenceNumber));
    ScanForwardToVisible();
  }

  void Next() override {
    if (state_ != kValid) return;   // kBeforeFirst / kPastEnd 上 Next 保持 !Valid
    SkipCurrentRunForwardWithKey(Slice(current_user_key_));
    ScanForwardToVisible();
  }

  void Prev() override {
    if (state_ == kBeforeFirst) return;        // nothing before first
    if (state_ == kPastEnd) { SeekToLast(); return; }
    RewindToRunStart();                        // 回到当前 run 的最新版本
    internal_->Prev();                         // 跨到前一个 user key 的最后一个版本
    if (!internal_->Valid()) {
      state_ = kBeforeFirst;
      return;
    }
    RewindToRunStart();
    ScanBackwardToVisible();
  }

  Slice key() const override { return Slice(current_user_key_); }
  Slice value() const override { return internal_->value(); }
  Status status() const override { return Status::OK(); }

 private:
  enum State { kBeforeFirst, kValid, kPastEnd };

  static bool ParseEntry(const Slice& internal_key, Slice* user_key, ValueType* type) {
    SequenceNumber seq = 0;
    return ParseInternalKey(internal_key, user_key, &seq, type);
  }

  // 把 internal_ 从 run 内任意位置挪到该 run 的最新版本（run 的起点）
  void RewindToRunStart() {
    Slice user_key;
    ValueType type = kTypeValue;
    if (!ParseEntry(internal_->key(), &user_key, &type)) return;
    internal_->Seek(BuildLookupKey(user_key, kMaxSequenceNumber));
  }

  // 前置：internal_ 停在 run 的最新版本；结果：停在第一个可见（非 tombstone）的 run 起点，或 kPastEnd
  void ScanForwardToVisible() {
    while (internal_->Valid()) {
      Slice user_key;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &user_key, &type)) {
        internal_->Next();
        continue;
      }
      if (type == kTypeValue) {
        SetValid(user_key);
        return;
      }
      SkipCurrentRunForwardWithKey(user_key);   // tombstone：跳过该 user key 的全部版本
    }
    state_ = kPastEnd;
  }

  // 后向：只可能从「前一个 user key 的最新版本」继续往前（方向与内部序相反）
  void ScanBackwardToVisible() {
    while (internal_->Valid()) {
      Slice user_key;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &user_key, &type)) {
        state_ = kPastEnd;
        return;
      }
      if (type == kTypeValue) {
        SetValid(user_key);
        return;
      }
      internal_->Prev();             // 跨到前一个 user key 的最后一个版本
      if (!internal_->Valid()) {
        state_ = kBeforeFirst;
        return;
      }
      RewindToRunStart();
    }
    state_ = kBeforeFirst;
  }

  void SkipCurrentRunForwardWithKey(const Slice& user_key) {
    while (internal_->Valid()) {
      Slice uk;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &uk, &type)) break;
      if (uk.compare(user_key) != 0) break;
      internal_->Next();
    }
  }

  void SetValid(const Slice& user_key) {
    current_user_key_.assign(user_key.data(), user_key.size());
    state_ = kValid;
  }

  Iterator* internal_;
  State state_;
  std::string current_user_key_;
};

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const InternalKeyComparator& icmp)
      : options_(options),
        internal_comparator_(icmp),
        memtable_(new MemTable(internal_comparator_, options.write_buffer_size)) {}

  Status Put(const Slice& key, const Slice& value) override { return Write(kTypeValue, key, value); }
  Status Delete(const Slice& key) override { return Write(kTypeDeletion, key, Slice()); }

  Status Get(const Slice& key, std::string* value) override {
    if (value == nullptr) return Status::InvalidArgument("DBImpl::Get: null value pointer");
    value->clear();
    const std::string lookup_key = BuildLookupKey(key, last_sequence_);
    switch (memtable_->Get(Slice(lookup_key), value)) {
      case MemTable::GetResult::kFound:
        return Status::OK();
      case MemTable::GetResult::kDeleted:
        value->clear();
        return Status::NotFound("DBImpl::Get: key is deleted", TruncatedKey(key));
      case MemTable::GetResult::kNotFound:
        return Status::NotFound("DBImpl::Get: key not found", TruncatedKey(key));
    }
    return Status::NotFound("DBImpl::Get: unreachable", TruncatedKey(key));
  }

  Iterator* NewIterator() override { return new UserIterator(memtable_.get()); }

 private:
  Status Write(ValueType type, const Slice& key, const Slice& value) {
    if (key.empty()) return Status::InvalidArgument("DBImpl::Put/Delete: empty user key");
    if (key.size() > kMaxUserKeySize) {
      return Status::InvalidArgument("DBImpl::Put/Delete: user key too large",
                                     std::to_string(key.size()));
    }
    const Status s = memtable_->Add(last_sequence_ + 1, type, key, value);
    if (s.ok()) ++last_sequence_;   // 被拒（如冻结）时不消耗 sequence：I1 单调不减
    return s;
  }

  Options options_;
  InternalKeyComparator internal_comparator_;
  std::unique_ptr<MemTable> memtable_;   // 声明在 internal_comparator_ 之后 → 先析构（L5）
  SequenceNumber last_sequence_ = 0;
};

}  // namespace

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  if (dbptr == nullptr) return Status::InvalidArgument("DB::Open: null dbptr");
  *dbptr = nullptr;
  if (options.comparator == nullptr) {
    return Status::InvalidArgument("DB::Open: comparator must not be null");
  }
  if (options.write_buffer_size == 0) {
    return Status::InvalidArgument("DB::Open: write_buffer_size must be greater than 0");
  }
  if (!name.empty()) {
    return Status::NotSupported("DB::Open: M1 supports memory mode only (name must be empty)", name);
  }
  const InternalKeyComparator internal_comparator(options.comparator);
  *dbptr = new DBImpl(options, internal_comparator);
  return Status::OK();
}

}  // namespace lsm
