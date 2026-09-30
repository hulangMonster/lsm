// src/db.cpp —— DB::Open 与内存模式实现（M1 §9）；持久模式实现见 db_impl.cpp
#include "db.h"

#include <memory>
#include <mutex>

#include "db_impl.h"
#include "memtable.h"
#include "write_batch.h"

namespace lsm {
namespace {

std::string ShortKey(const Slice& key) {
  const size_t n = key.size() < 64 ? key.size() : 64;
  return std::string(key.data(), n);
}

// M5.2：把 WriteBatch 解码成结构化条目（内存模式的批写路径用；预校验已在锁外完成）。
class BatchCollector : public WriteBatch::Handler {
 public:
  struct Entry {
    ValueType type = kTypeValue;
    std::string key;
    std::string value;
  };
  void Put(const Slice& key, const Slice& value) override {
    entries.push_back(Entry{kTypeValue, key.ToString(), value.ToString()});
  }
  void Delete(const Slice& key) override {
    entries.push_back(Entry{kTypeDeletion, key.ToString(), std::string()});
  }
  std::vector<Entry> entries;
};

class MemoryDBImpl : public DB {
 public:
  MemoryDBImpl(const Options& options, const InternalKeyComparator& icmp)
      : options_(options),
        internal_comparator_(icmp),
        memtable_(new MemTable(internal_comparator_, options.write_buffer_size)) {}

  Status Put(const WriteOptions&, const Slice& key, const Slice& value) override {
    return WriteEntry(kTypeValue, key, value);
  }
  Status Delete(const WriteOptions&, const Slice& key) override {
    return WriteEntry(kTypeDeletion, key, Slice());
  }

  // M5.2（docs/m5-design.md §5.2/§5.4）：内存模式的整批提交。与持久模式同形——
  // **先做无副作用预校验（锁外）**，再在锁内逐条 Add。I51（整批原子可见）在这里由
  // 「输入已全部校验 + Add 在输入合法时不会失败」共同保证。
  Status Write(const WriteOptions&, WriteBatch* updates) override {
    if (updates == nullptr) {
      return Status::InvalidArgument("MemoryDBImpl::Write: null WriteBatch");
    }
    uint32_t count = 0;
    size_t entry_bytes = 0;
    uint64_t user_bytes = 0;
    const Status vs = updates->Validate(&count, &entry_bytes, &user_bytes);
    if (!vs.ok()) return vs;
    // 批大小上限已由 Validate 统一校验（§13.3，单一真相源），此处不重复。
    // 锁外解码（纯内存、无 IO）：畸形 rep_ 在这里就被 Iterate 判为 kCorruption。
    BatchCollector c;
    const Status is = updates->Iterate(&c);
    if (!is.ok()) return is;
    if (c.entries.size() != count) {
      return Status::Corruption("MemoryDBImpl::Write", "Iterate 条数与 count 不一致");
    }
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return Status::IOError("MemoryDBImpl::Write: DB is closed");
    if (last_sequence_ + static_cast<SequenceNumber>(count) > kMaxSequenceNumber) {
      return Status::InvalidArgument("MemoryDBImpl::Write: sequence space exhausted");
    }
    for (const BatchCollector::Entry& e : c.entries) {
      // Validate 已保证 key 非空/不超限、type 合法；Add 的剩余拒绝条件只有「已冻结」，
      // 而内存模式的 MemTable 从不冻结 ⇒ 这里不会中途失败（失败会破坏 I51，故不吞掉）。
      const Status s = memtable_->Add(last_sequence_ + 1, e.type, e.key, e.value);
      if (!s.ok()) return s;
      ++last_sequence_;
    }
    return Status::OK();
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
  // M5.2（M5-R7）：改名 WriteEntry，避免与 Write(const WriteOptions&, WriteBatch*) 重载歧义。
  Status WriteEntry(ValueType type, const Slice& key, const Slice& value) {
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
  // M3 §8.5：block_size / max_open_files 的合法性在 Open 第一步一次性校验（输入校验不得触发
  // fail-stop；非法值返回 kInvalidArgument 且不写任何状态）。
  if (options.block_size < 512 || options.block_size > 1024 * 1024) {
    return Status::InvalidArgument("DB::Open: block_size must be within [512, 1 MiB]",
                                   std::to_string(options.block_size));
  }
  if (options.max_open_files == 0 || options.max_open_files > 1000000) {
    return Status::InvalidArgument("DB::Open: max_open_files out of range",
                                   std::to_string(options.max_open_files));
  }
  // M5.1（docs/m5-design.md §5.6）：bloom_bits ∈ {0} ∪ [1,64]；0 = 关闭（对照实验必须显式传 0
  // 并把该参数打印进结果文件头）。非法值在这里一次性拒绝，**不**写任何状态。
  if (options.bloom_bits < 0 || options.bloom_bits > 64) {
    return Status::InvalidArgument("DB::Open: bloom_bits must be within [0, 64]",
                                   std::to_string(options.bloom_bits));
  }
  if (name.empty()) {
    *dbptr = new MemoryDBImpl(options, InternalKeyComparator(options.comparator));
    return Status::OK();
  }
  return PersistentDBImpl::RecoverAndOpen(options, name, dbptr);
}

}  // namespace lsm
