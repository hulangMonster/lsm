// src/db_impl.h —— 持久化实现（docs/m2-design.md §5/§6/§7 + docs/m3-design.md §6/§7/§8）
//
// M3.3：完整启动恢复（元数据持久化 + 孤儿清理 + WAL 轮转/回收），删除 M3.2 的 5 行 *.sst guard。
#ifndef LSM_DB_IMPL_H_
#define LSM_DB_IMPL_H_

#include <condition_variable>
#include <cstdint>
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
// M3.3 只增不改（§8.4 的计数纪律：任何丢弃/跳过/截断/删除都必须有一个计数落点）。
struct RecoveryStats {
  uint64_t log_files = 0;               // 扫描到的 *.log 数
  uint64_t records_replayed = 0;        // 实际重放的 batch 数
  uint64_t entries_replayed = 0;        // 重放的条目数
  uint64_t records_skipped = 0;         // 因 seq <= last 被跳过（幂等 / 拒绝 sequence 回退）
  uint64_t tail_truncated_bytes = 0;    // 尾部截断掉的字节数（>0 表示发生过残骸截断）
  SequenceNumber last_sequence = 0;     // 恢复后的 last_sequence_
  std::string truncation_note;          // 截断原因（可定位）
  // ---- M3.3 新增（§8.4）----
  bool meta_present = false;            // 步骤 ⑤：**活动**版本元数据是否存在（M4 起 = manifest_present；字段名保留）
  uint64_t sst_files_registered = 0;    // 步骤 ⑤：版本里的文件数
  uint64_t sst_bytes_registered = 0;    // 步骤 ⑤：版本里各文件 file_size 之和
  uint64_t orphan_tmp_removed = 0;      // 步骤 ⑥a
  uint64_t orphan_sst_removed = 0;      // 步骤 ⑥b
  uint64_t orphan_bytes_removed = 0;    // 步骤 ⑥a+b 删掉的字节数
  uint64_t obsolete_logs_removed = 0;   // 步骤 ⑥c（受 recycle_log_files 控制）
  uint64_t obsolete_log_bytes_removed = 0;
  uint64_t orphan_remove_failed = 0;    // 步骤 ⑥ 任一删除失败（只计数不阻断）
  SequenceNumber max_sequence_in_files = 0;   // 步骤 ⑧（M3-A38 的比对对象）
  uint64_t current_log_recreated = 0;   // 步骤 ⑨：当前 log 缺失而重建（正常情况下不该发生）
  uint64_t unknown_metaindex_entries = 0;  // 读 .sst 时的未知 metaindex 条目（§3.4）
  // ---- M4.1/M4.2 新增（docs/m4-design.md §8.6）：活动元数据 = CURRENT + MANIFEST ----
  bool manifest_present = false;
  bool migrated_from_meta = false;
  uint64_t manifest_number = 0;
  uint64_t manifest_bytes = 0;
  uint64_t manifest_edits_replayed = 0;
  uint64_t manifest_tail_truncated_bytes = 0;
  uint64_t unknown_manifest_record_types = 0;
  uint64_t meta_migrated = 0;
  uint64_t meta_delete_failed = 0;
  uint64_t manifest_orphan_removed = 0;
  uint64_t manifest_tmp_removed = 0;
  uint64_t current_tmp_removed = 0;
  std::string manifest_truncation_note;
};

// M3 的 flush 统计（docs/m3-design.md §8.4；"丢弃/失败必须计数"的单一落点）。
struct FlushStats {
  uint64_t flushes_started = 0;
  uint64_t flushes_completed = 0;
  uint64_t flushes_failed = 0;
  uint64_t immutables_abandoned = 0;   // Close() 时仍未落盘的 immutable 数（§6.5）
  uint64_t stall_events = 0;           // 写者因 kMaxImmutableMemTables 停等的次数
  uint64_t stall_micros = 0;
  uint64_t rotations = 0;              // WAL 轮转成功次数（§6.6.1）
  uint64_t rotate_failed = 0;          // WAL 轮转失败次数（整批拒绝，不置 bg_error_）
  uint64_t log_files_deleted = 0;      // WAL 回收实际删掉的文件数（§6.6.2）
  uint64_t log_bytes_deleted = 0;      // WAL 回收实际删掉的字节数
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

// 持久化 DB：WAL + MemTable + 内存 Version（M3.3：注册写 META，恢复从 META 重建）
class PersistentDBImpl : public DB {
 public:
  ~PersistentDBImpl() override;

  Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;
  Status Get(const Slice& key, std::string* value) override;
  Iterator* NewIterator() override;
  Status Sync() override;
  Status Close() override;

  // 诊断（测试与证据用）：已 fsync 覆盖到的最大 sequence（受 commit_mu_ 保护，M2 起如此）
  SequenceNumber durable_seq() const { return durable_seq_; }
  // 诊断：已真正 Append 进**当前 log** 的最大 sequence（I32 的 per-log 边界，§15 R1）
  SequenceNumber log_last_appended_seq() const {
    std::lock_guard<std::mutex> l(mutex_);
    return log_last_appended_seq_;
  }
  // 诊断：恢复/写入后的 last_sequence_（I31 的唯一口径）
  SequenceNumber last_sequence() const {
    std::lock_guard<std::mutex> l(mutex_);
    return last_sequence_;
  }
  // 诊断：当前 WAL 编号 / 当前 memtable 的 log_number（I34 回收判据的输入）
  uint64_t log_number() const {
    std::lock_guard<std::mutex> l(mutex_);
    return log_number_;
  }
  uint64_t memtable_log_number() const {
    std::lock_guard<std::mutex> l(mutex_);
    return memtable_log_number_;
  }
  uint64_t next_file_number() const {
    std::lock_guard<std::mutex> l(mutex_);
    return next_file_number_;
  }
  // M4.1 诊断：取代路径的 MANIFEST 编号/字节数/编辑数/重建次数（受 mutex_ 保护）。
  uint64_t manifest_number() const {
    std::lock_guard<std::mutex> l(mutex_);
    return manifest_number_;
  }
  uint64_t manifest_bytes() const {
    std::lock_guard<std::mutex> l(mutex_);
    return manifest_bytes_;
  }
  uint64_t manifest_edits() const {
    std::lock_guard<std::mutex> l(mutex_);
    return manifest_edits_;
  }
  uint64_t manifest_rolls() const {
    std::lock_guard<std::mutex> l(mutex_);
    return manifest_rolls_;
  }
  // 诊断：当前版本里的 min_log_number_to_keep（§6.6.2 的 I34 判据值）
  uint64_t min_log_number_to_keep() const {
    std::lock_guard<std::mutex> l(mutex_);
    return version_ == nullptr ? 1 : version_->min_log_number_to_keep();
  }
  // 诊断：当前版本注册的文件号（降序）
  std::vector<uint64_t> registered_file_numbers() const {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<uint64_t> out;
    if (version_ != nullptr) {
      for (const FileMetaData& f : version_->files()) out.push_back(f.number);
    }
    return out;
  }
  // 诊断：当前等待结算的写者数（A20 的确定性屏障靠轮询它来等"整批就位"）
  size_t pending_writers();
  // 诊断：Close 是否已置 closed_（M3-A51 用它把"Close 已开始"与"放行被阻的 flush"定序）
  bool closed_for_test() const {
    std::lock_guard<std::mutex> l(mutex_);
    return closed_;
  }
  // 恢复报告（只读；由 RecoverAndOpen 在恢复期间填好）
  RecoveryStats GetRecoveryStats() const { return recovery_stats_; }

  // M3.2 诊断（只读；docs/m3-design.md §8.4/§D8）
  FlushStats GetFlushStats() const;
  DbReadStats GetReadStats() const;
  size_t immutables_size() const;

  // 仅测试的反向自检 seam：在持有 DB 互斥锁的临界区内执行 fn，
  // 用于证明 SpyEnv 的「持锁零 IO」探针真的会报警（M3-A23 的防空绿要求，docs/m3-prerequisites §9.2）。
  void RunHoldingDbMutexForTest(const std::function<void()>& fn);

  // 仅测试的 flush seam（M3-A35/A36 的"关库时当前 log 为空、老 log 已回收"契约）：
  // 把当前 memtable 冻结并等待后台注册完成，再把当前 log 轮转成**空**文件。
  // 设计 §8.7 E4 不新增公共 DB::Flush()；小 write_buffer_size 也无法冲刷"最后一个 memtable"
  // （触发冻结的那一批必然落进新 memtable/新 log），所以这条契约需要这个最小 seam。
  // 生产路径不调用它；它不写任何用户数据。
  Status ForceFlushForTest();

 private:
  friend Status DB::Open(const Options&, const std::string&, DB**);

  PersistentDBImpl(const Options& options, const InternalKeyComparator& icmp, std::string dbname,
                   size_t memtable_capacity);

  // design §5.2/§8.3：两遍扫描（先规划 + 截断，再按 D12 的容量重放）
  static Status RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr);

  Status Write(ValueType type, const WriteOptions& options, const Slice& key, const Slice& value);

  struct Pending;   // 定义在下方（组提交成员）；此处先声明以便 EncodeGroup 的签名可见

  Status RunFlusher();                                   // 队首：组批 → 冻结 → 写 WAL → 结算
  static std::string EncodeGroup(SequenceNumber begin, const std::vector<Pending*>& members);

  // M3.2 flush 状态机（§6.2/§6.3）与读路径串联（§7.1）。
  void StartBackgroundThread();
  void BackgroundLoop();
  void FlushImmutable(const std::shared_ptr<struct Immutable>& imm);
  void WaitForImmutableCapacity();
  Status GetInternal(const Slice& key, std::string* value, DbReadStats* delta);
  void MergeReadStats(const DbReadStats& delta);

  // ---- M3.3（§6.3 步骤 ⑦/⑨、§6.6）----
  // 需要 mutex_：min({memtable_} ∪ immutables_ 中除 exclude 之外的表)（§6.6.2 的单一真相源）。
  uint64_t RecomputeMinLogNumberToKeepLocked(const struct Immutable* exclude) const;
  // 删除编号 < min_log_to_keep 的 *.log（在 META durable 之后调用；锁外做 IO）。
  void RecycleObsoleteLogs();
  // 轮转：创建编号 number 的空 log 文件（NewWritableFile + Close + SyncDir）。
  Status CreateEmptyLogFile(uint64_t number);
  // 轮转：fsync 旧 log → 发布 durable → 建新 log（编号 +1）→ 原子换 log_/log_number_。
  Status RotateLog();

  const Options options_;
  const InternalKeyComparator internal_comparator_;
  const std::string dbname_;
  std::shared_ptr<MemTable> memtable_;
  std::shared_ptr<const Version> version_;
  std::deque<std::shared_ptr<struct Immutable>> immutables_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<WALWriter> log_;
  std::unique_ptr<FileLock> file_lock_;

  // M3：文件号空间与当前 log（§6.1 的状态表；log_number_ 只由当前 flusher 修改，L20）
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 1;
  bool log_sealed_ = false;       // 当前 log 已封口（冻结时置位，轮转完成后清）
  bool need_rotate_ = false;      // 待轮转（阶段 A 置位，阶段 A' 做 IO）
  // §6.6.2 / §8.3 步骤 ⑩：当前 memtable 的**最早写入所在 log**。恢复时 = 被重放 log 的最小编号；
  // 冻结时旧表保留它自己的值，新表取当时的 log_number_（轮转后不更新 ⇒ 保守，绝不漏删）。
  uint64_t memtable_log_number_ = 1;

  // ---- M4.1/M4.2：MANIFEST/CURRENT（稳态元数据）----
  uint64_t manifest_number_ = 0;   // 0 = 尚无 MANIFEST（首次 flush 走模式 (a)）
  uint64_t manifest_bytes_ = 0;
  uint64_t manifest_edits_ = 0;
  uint64_t manifest_rolls_ = 0;

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
  // I32 修复（§15 R1）：**已真正 Append 进当前 log** 的最大 sequence（受 mutex_ 保护）。
  // 水位只按它发布，不按 last_sequence_ —— 后者在锁内取批时就推进了，而 Append 是锁外做的。
  // per-log 语义：轮转前先 fsync 旧 log 并发布水位，再换文件，故它是"当前 log 的已追加边界"。
  SequenceNumber log_last_appended_seq_ = 0;
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
