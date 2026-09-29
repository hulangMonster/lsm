// src/db_impl.h —— 持久化实现（docs/m2-design.md §5/§6/§7）+ 共用的用户视图迭代器
//
// M2.2 的写路径是**过渡形态**（design §6.2）：单写者串行、每写一次 fsync —— 正确但低吞吐；
// M2.3 把它换成组提交，Write 的签名/返回值/不变量都不变（这是"每步一个判据"的关键）。
#ifndef LSM_DB_IMPL_H_
#define LSM_DB_IMPL_H_

#include <memory>
#include <mutex>
#include <string>

#include "common.h"
#include "db.h"
#include "memtable.h"
#include "wal.h"

namespace lsm {

// 用户视图迭代器（三态状态机，design §4.4）：内存模式与持久模式共用，避免两份实现漂移。
// 返回值所有权归调用方。
Iterator* NewMemTableUserIterator(const MemTable* mem, const InternalKeyComparator& icmp);

// 持久化 DB：WAL + MemTable（M2 无 SSTable/flush ⇒ WAL 是唯一真相源）
class PersistentDBImpl : public DB {
 public:
  ~PersistentDBImpl() override;

  Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;
  Status Get(const Slice& key, std::string* value) override;
  Iterator* NewIterator() override;
  Status Sync() override;
  Status Close() override;

 private:
  friend Status DB::Open(const Options&, const std::string&, DB**);

  PersistentDBImpl(const Options& options, const InternalKeyComparator& icmp, std::string dbname,
                   size_t memtable_capacity);

  // design §5.2：两遍扫描（先规划 + 截断，再按 D12 的容量重放）
  static Status RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr);

  Status Write(ValueType type, const WriteOptions& options, const Slice& key, const Slice& value);

  const Options options_;
  const InternalKeyComparator internal_comparator_;
  const std::string dbname_;
  std::unique_ptr<MemTable> memtable_;
  std::unique_ptr<WALWriter> log_;

  std::mutex mutex_;         // 只保护内存状态（memtable_ / last_sequence_ / closed_ / bg_error_）
  std::mutex log_mu_;        // 串行化整条写路径（M2.2 过渡形态）；**持它期间不做 DB 锁内的 IO**
  SequenceNumber last_sequence_ = 0;
  Status bg_error_;
  bool closed_ = false;
};

}  // namespace lsm

#endif  // LSM_DB_IMPL_H_
