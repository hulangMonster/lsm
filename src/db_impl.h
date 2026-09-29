// src/db_impl.h —— 持久化实现（docs/m2-design.md §5/§6/§7）+ 共用的用户视图迭代器
//
// M2.2 的写路径是**过渡形态**（design §6.2）：单写者串行、每写一次 fsync —— 正确但低吞吐；
// M2.3 把它换成组提交，Write 的签名/返回值/不变量都不变（这是"每步一个判据"的关键）。
#ifndef LSM_DB_IMPL_H_
#define LSM_DB_IMPL_H_

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "db.h"
#include "memtable.h"
#include "wal.h"

namespace lsm {

// M2 的恢复报告（design §8.2 明写"这个接口 M2 就要有"；A13 的判据含"可读"）。
// 它同时承载 §5.3/§5.4 承诺的"截断量与被跳过的 record 必须**计数上报**，不得静默"。
struct RecoveryStats {
  uint64_t log_files = 0;               // 扫描到的 *.log 数
  uint64_t records_replayed = 0;        // 实际重放的 batch 数
  uint64_t entries_replayed = 0;        // 重放的条目数
  uint64_t records_skipped = 0;         // 因 seq <= last 被跳过（幂等 / 拒绝 sequence 回退）
  uint64_t tail_truncated_bytes = 0;    // 尾部截断掉的字节数（>0 表示发生过残骸截断）
  SequenceNumber last_sequence = 0;     // 恢复后的 last_sequence_
  std::string truncation_note;          // 截断原因（可定位）
};

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

  // 诊断（测试与证据用）：已 fsync 覆盖到的最大 sequence
  SequenceNumber durable_seq() const { return durable_seq_; }
  // 诊断：当前等待结算的写者数（A20 的确定性屏障靠轮询它来等"整批就位"）
  size_t pending_writers();
  // 恢复报告（只读；由 RecoverAndOpen 在恢复期间填好）
  RecoveryStats GetRecoveryStats() const { return recovery_stats_; }

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
  // D10：进程级独占锁（Close/析构时释放）。缺了它，两个进程同时 Open 同一目录会同时
  // Append 同一 WAL ⇒ record 交错 ⇒ 恢复报中间损坏（设计点名的最难排查路径）。
  std::unique_ptr<FileLock> file_lock_;

  Env* EnvOf() const { return options_.env != nullptr ? options_.env : Env::Default(); }

  // 组提交（design §6.3）。锁序（L8）：commit_mu_ → mutex_；且**两把锁都不跨 IO**（I17）。
  struct Pending {
    bool need_sync = false;
    bool done = false;
    Status status;
    SequenceNumber begin = 0;    // 本写者拿到的起始 sequence（组内连续）
    ValueType type = kTypeValue;
    std::string key;
    std::string value;
    size_t entry_bytes = 0;
  };

  Status RunFlusher();                                   // 队首：组批 → 写 WAL →（必要时）fsync → 发布水位 → 结算并唤醒
  static std::string EncodeGroup(SequenceNumber begin, const std::vector<Pending*>& members);

  std::mutex mutex_;                  // 只保护内存状态（memtable_ / last_sequence_ / closed_ / bg_error_）
  std::mutex commit_mu_;              // 保护组提交队列与 flusher 状态（L8 的第一把锁）
  std::condition_variable commit_cv_; // 谓词：w.done || (!flusher_active_ && queue_.front() == &w)
  std::deque<Pending*> queue_;
  bool flusher_active_ = false;
  SequenceNumber last_sequence_ = 0;  // 受 mutex_ 保护
  RecoveryStats recovery_stats_;      // 恢复期填好，之后只读
  SequenceNumber durable_seq_ = 0;    // 已 fsync 覆盖到的最大 sequence（受 commit_mu_ 保护）
  Status bg_error_;
  // 初始为 true：恢复中途失败时对象会被 unique_ptr 析构，此时 log_ 尚未打开 ——
  // 若不这样，析构会走到 Close() 里对 nullptr 的 log_ 取 Sync（实测段错误，见 docs/m2-evidence.md）
  bool closed_ = true;
};

}  // namespace lsm

#endif  // LSM_DB_IMPL_H_
