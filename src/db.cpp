// src/db.cpp —— DB::Open 与内存模式实现（M1 §9）；持久模式实现见 db_impl.cpp
#include "db.h"

#include <memory>
#include <mutex>

#include "db_impl.h"
#include "memtable.h"

namespace lsm {
namespace {

std::string ShortKey(const Slice& key) {
  const size_t n = key.size() < 64 ? key.size() : 64;
  return std::string(key.data(), n);
}

class MemoryDBImpl : public DB {
 public:
  MemoryDBImpl(const Options& options, const InternalKeyComparator& icmp)
      : options_(options),
        internal_comparator_(icmp),
        memtable_(new MemTable(internal_comparator_, options.write_buffer_size)) {}

  Status Put(const WriteOptions&, const Slice& key, const Slice& value) override {
    return Write(kTypeValue, key, value);
  }
  Status Delete(const WriteOptions&, const Slice& key) override {
    return Write(kTypeDeletion, key, Slice());
  }

  Status Get(const Slice& key, std::string* value) override {
    if (value == nullptr) return Status::InvalidArgument("MemoryDBImpl::Get: null value pointer");
    value->clear();
    std::lock_guard<std::mutex> l(mu_);
    const std::string lookup_key = BuildLookupKey(key, last_sequence_);
    switch (memtable_->Get(Slice(lookup_key), value)) {
      case MemTable::GetResult::kFound:
        return Status::OK();
      case MemTable::GetResult::kDeleted:
        value->clear();
        return Status::NotFound("MemoryDBImpl::Get: key is deleted", ShortKey(key));
      case MemTable::GetResult::kNotFound:
        return Status::NotFound("MemoryDBImpl::Get: key not found", ShortKey(key));
    }
    return Status::NotFound("MemoryDBImpl::Get: unreachable", ShortKey(key));
  }

  Iterator* NewIterator() override {
    std::lock_guard<std::mutex> l(mu_);
    return NewMemTableUserIterator(memtable_.get(), internal_comparator_);
  }

  Status Sync() override { return Status::OK(); }   // 内存模式没有需要刷盘的东西

  Status Close() override {
    std::lock_guard<std::mutex> l(mu_);
    closed_ = true;                                 // 幂等
    return Status::OK();
  }

 private:
  Status Write(ValueType type, const Slice& key, const Slice& value) {
    if (key.empty()) return Status::InvalidArgument("MemoryDBImpl::Put/Delete: empty user key");
    if (key.size() > kMaxUserKeySize) {
      return Status::InvalidArgument("MemoryDBImpl::Put/Delete: user key too large",
                                     std::to_string(key.size()));
    }
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return Status::IOError("MemoryDBImpl::Put/Delete: DB is closed");
    const Status s = memtable_->Add(last_sequence_ + 1, type, key, value);
    if (s.ok()) ++last_sequence_;
    return s;
  }

  Options options_;
  InternalKeyComparator internal_comparator_;
  std::unique_ptr<MemTable> memtable_;
  std::mutex mu_;
  SequenceNumber last_sequence_ = 0;
  bool closed_ = false;
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
  if (name.empty()) {
    *dbptr = new MemoryDBImpl(options, InternalKeyComparator(options.comparator));
    return Status::OK();
  }
  return PersistentDBImpl::RecoverAndOpen(options, name, dbptr);
}

}  // namespace lsm
