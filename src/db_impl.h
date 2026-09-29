// src/db_impl.h —— 持久化实现（docs/m2-design.md §5/§6/§7 + docs/m3-design.md §6/§7）
//
// M3.2 的过渡形态（design §11.2）：flush 路径 + 读路径串联 + MergingIterator/DBIter +
// 单后台线程；版本注册**只在内存**（不写不读 META），Open 时若目录里存在 *.sst 一律拒绝。
#ifndef LSM_DB_IMPL_H_
#define LSM_DB_IMPL_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "db.h"
#include "memtable.h"
#include "version_set.h"
#include "wal.h"

namespace lsm {

// A25 探针（I17「持锁零 IO」的可验证化）：本线程当前是否持有 DB 互斥锁。
// 只做诊断，不改变加锁语义；测试用它包一层 Env，在 Append/Sync/rename/块读时断言此刻未持锁。
bool DbMutexHeldOnThisThread();

// M2 的恢复报告（design §8.2 明写"这个接口 M2 就要有"；A13 的判据含"可读"）。
struct RecoveryStats {
  uint64_t log_files = 0;               // 扫描到的 *.log 数
  uint64_t records_replayed = 0;        // 实际重放的 batch 数
  uint64_t entries_replayed = 0;        // 重放的条目数
  uint64_t records_skipped = 0;         // 因 seq <= last 被跳过（幂等 / 拒绝 sequence 回退）
  uint64_t tail_truncated_bytes = 0;    // 尾部截断掉的字节数（>0 表示发生过残骸截断）
  SequenceNumber last_sequence = 0;     // 恢复后的 last_sequence_
  std::string truncation_note;          // 截断原因（可定位）
};

// M3 的 flush 统计（docs/m3-design.md §8.4；"丢弃/失败必须计数"的单一落点）。
struct FlushStats {
  uint64_t flushes_started = 0;
  uint64_t flushes_completed = 0;
  uint64_t flushes_failed = 0;
  uint64_t immutables_abandoned = 0;   // Close() 时仍未落盘的 immutable 数（§6.5）
  uint64_t stall_events = 0;           // 写者因 kMaxImmutableMemTables 停等的次数
  uint64_t stall_micros = 0;
  uint64_t index_size_warn = 0;        // TableBuilder 的索引超阈值计数（§3.3）
  std::string last_error;              // 最近一次 flush 失败的可读 Status
};

// 读路径命中位置（§D8；A30 断言 "hit_layer == none"）。
enum class HitLayer { kNone, kMemTable, kImmutable, kSSTable };

// DB 层读放大口径（§D8）。与格式层的 lsm::ReadStats **分开命名**，避免与 Table 的
// 「单文件统计」混为一谈；files_checked 的递增归 DB 层（见 M3.2 报告第 4 条）。
struct DbReadStats {
  uint64_t files_checked = 0;        // 真的进了 Table::GetEntry 的文件数
  uint64_t key_range_skipped = 0;    // 被 key range 零 IO 过滤掉的文件数
  uint64_t index_blocks_read = 0;    // 本次真正打开（首次载入索引）的文件数
  uint64_t data_blocks_read = 0;
  uint64_t blocks_read = 0;
  uint64_t bytes_read = 0;
  uint64_t crc_checked = 0;
  uint64_t crc_failed = 0;
  HitLayer hit_layer = HitLayer::kNone;   // 最近一次 Get 的命中层
};

// 用户视图迭代器（三态状态机，design §4.4）：内存模式与持久模式共用，避免两份实现漂移。
// 返回值所有权归调用方。
Iterator* NewMemTableUserIterator(const MemTable* mem, const InternalKeyComparator& icmp);

// 持久化 DB：WAL + MemTable + 内存 Version（M3.2 过渡形态：注册不落 META）
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

  // M3.2 诊断（只读；docs/m3-design.md §8.4/§D8）
  FlushStats GetFlushStats() const;
  DbReadStats GetReadStats() const;
  size_t immutables_size() const;

  // 仅测试的反向自检 seam：在持有 DB 互斥锁的临界区内执行 fn，
  // 用于证明 SpyEnv 的「持锁零 IO」探针真的会报警（M3-A23 的防空绿要求，docs/m3-prerequisites §9.2）。
  void RunHoldingDbMutexForTest(const std::function<void()>& fn);

 private:
  friend Status DB::Open(const Options&, const std::string&, DB**);

  PersistentDBImpl(const Options& options, const InternalKeyComparator& icmp, std::string dbname,
                   size_t memtable_capacity);

  // design §5.2：两遍扫描（先规划 + 截断，再按 D12 的容量重放）
  static Status RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr);

  Status Write(ValueType type, const WriteOptions& options, const Slice& key, const Slice& value);

  struct Pending;   // 定义在下方（组提交成员）；此处先声明以便 EncodeGroup 的签名可见

  Status RunFlusher();                                   // 队首：组批 → 冻结 → 写 WAL → 结算
  static std::string EncodeGroup(SequenceNumber begin, const std::vector<Pending*>& members);

  // M3.2 flush 状态机（§6.2/§6.3 去掉 META 步骤）与读路径串联（§7.1）。
  void StartBackgroundThread();
  void BackgroundLoop();
  void FlushImmutable(const std::shared_ptr<struct Immutable>& imm);
  void WaitForImmutableCapacity();
  Status GetInternal(const Slice& key, std::string* value, DbReadStats* delta);
  void MergeReadStats(const DbReadStats& delta);

  const Options options_;
  const InternalKeyComparator internal_comparator_;
  const std::string dbname_;
  std::shared_ptr<MemTable> memtable_;
  std::shared_ptr<const Version> version_;
  std::deque<std::shared_ptr<struct Immutable>> immutables_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<WALWriter> log_;
  std::unique_ptr<FileLock> file_lock_;

  // M3：文件号空间与当前 log（M3.2 不轮转，log_number_ 不变；log_sealed_ 仅为对齐 §6.1 锁表）
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 1;
  bool log_sealed_ = false;

  // M3：单后台 flush 线程（§6.5/L21：它只取 mutex_，永不碰 commit_mu_）
  std::thread bg_thread_;
  std::condition_variable bg_cv_;
  bool bg_started_ = false;
  bool bg_stop_ = false;

  FlushStats flush_stats_;
  DbReadStats read_stats_;
  HitLayer last_hit_layer_ = HitLayer::kNone;

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

  mutable std::mutex mutex_;          // 保护内存状态（memtable_/immutables_/version_/last_sequence_/...）
  std::mutex commit_mu_;              // 保护组提交队列与 flusher 状态（L8 的第一把锁）
  std::condition_variable commit_cv_; // 谓词：w.done || (!flusher_active_ && queue_.front() == &w)
  std::deque<Pending*> queue_;
  bool flusher_active_ = false;
  SequenceNumber last_sequence_ = 0;  // 受 mutex_ 保护
  RecoveryStats recovery_stats_;      // 恢复期填好，之后只读
  SequenceNumber durable_seq_ = 0;    // 已 fsync 覆盖到的最大 sequence（受 commit_mu_ 保护）
  // I32 修复：**已真正 Append 进 log 的**最大 sequence（受 mutex_ 保护）。水位只按它发布，
  // 不按 last_sequence_ —— 后者在锁内取批时就推进了，而 Append 是锁外做的。
  SequenceNumber appended_seq_ = 0;
  Status bg_error_;
  // 初始为 true：恢复中途失败时对象会被 unique_ptr 析构，此时 log_ 尚未打开 ——
  // 若不这样，析构会走到 Close() 里对 nullptr 的 log_ 取 Sync（实测段错误，见 docs/m2-evidence.md）
  bool closed_ = true;
};

// 内存中的 immutable MemTable（§6.1）：log_number 记录它的最早写入所在 log（M3.3 的 WAL 回收判据）。
struct Immutable {
  std::shared_ptr<MemTable> mem;
  uint64_t log_number = 0;
};

}  // namespace lsm

#endif  // LSM_DB_IMPL_H_
