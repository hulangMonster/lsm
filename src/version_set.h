// src/version_set.h —— M3 的不可变 Version + 内存 TableCache + META 持久化；
//                     M4 的层级 Version、MANIFEST/CURRENT 回放与安装（docs/m4-design.md §3/§5.3/§8）
//
// M3.3：Recover 读 META，Persist 写 META（全量快照）。M4.1 追加：
//   * Version 的层级视图（files() 恒等于 level_files(0)，M3 调用方语义不变）；
//   * VersionEdit 的应用（X7：先 DeleteFile 后 AddFile）与安装期 ValidateLevelLayout；
//   * MANIFEST record 的追加/全量快照重建、CURRENT 的严格解析与原子切换、META→MANIFEST 一次性迁移。
// 稳态元数据 = CURRENT + MANIFEST；只有"没有 CURRENT/MANIFEST、只有旧 META"时才做一次兼容读入 + 迁移。
#ifndef LSM_VERSION_SET_H_
#define LSM_VERSION_SET_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "sstable/table.h"
#include "util/env.h"
#include "version_edit.h"

namespace lsm {

// ---- 文件命名（docs/protocol.md §10.1；M3.2 落点，M4 不迁移以免改 M3 测试的 include）----
std::string TableFileName(const std::string& dbname, uint64_t number);   // %06u.sst
std::string TempFileName(const std::string& dbname, uint64_t number);    // %06u.sst.tmp
bool ParseTableFileName(const std::string& fname, uint64_t* number);     // 精确后缀，拒绝 .sst.tmp
bool ParseTempFileName(const std::string& fname, uint64_t* number);      // %06u.sst.tmp

// 不可变版本（L15/I21）：构造后只读；注册 = 造新对象 + 原子替换 shared_ptr。
// M4 起内部是层级布局；files() 保持"L0 的文件"这一兼容语义（§5.3 的语义收窄登记）。
class Version {
 public:
  // 兼容构造（M3）：全部文件放进 L0，按文件号降序。
  Version(std::vector<FileMetaData> files, uint64_t log_number, uint64_t min_log_number_to_keep,
          uint64_t next_file_number);
  // M4：显式层级构造（levels 可乱序；构造时归一化）。
  Version(std::vector<std::vector<FileMetaData>> levels, uint64_t log_number,
          uint64_t min_log_number_to_keep, uint64_t next_file_number);

  int num_levels() const { return kNumLevels; }
  // L0 按文件号降序（新→旧）；L1+ 按 smallest 的 user key 升序。
  const std::vector<FileMetaData>& level_files(int level) const;
  const std::vector<std::vector<FileMetaData>>& level_files_all() const { return level_files_; }
  // 【兼容别名】== level_files(0)；M3 的库全部是 L0。
  const std::vector<FileMetaData>& files() const { return level_files_[0]; }
  // 跨层全量（恢复/孤儿判定/统计用）。
  const std::vector<FileMetaData>& AllFiles() const { return all_files_; }
  uint64_t total_bytes(int level) const;
  uint64_t log_number() const { return log_number_; }
  uint64_t min_log_number_to_keep() const { return min_log_number_to_keep_; }
  uint64_t next_file_number() const { return next_file_number_; }
  SequenceNumber MaxSequenceInFiles() const;

  // 【M4 新增】引用计数（I42/L23）：live 集合由 DB 维护。
  // Ref/Unref 成对：读路径拿到即 Ref、用完/迭代器析构即 Unref；安装新版本时对旧版本 Unref。
  // 最后一个引用释放时调用 unref_hook_（DB 用它把版本从 live_versions_ 摘除）。
  void Ref() const { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() const;
  int refs() const { return refs_.load(std::memory_order_relaxed); }
  void SetUnrefHook(std::function<void(const Version*)> h) const { unref_hook_ = std::move(h); }

 private:
  void Normalize();
  std::vector<std::vector<FileMetaData>> level_files_;
  std::vector<FileMetaData> all_files_;
  const uint64_t log_number_;
  const uint64_t min_log_number_to_keep_;
  const uint64_t next_file_number_;
  mutable std::atomic<int> refs_{1};
  mutable std::function<void(const Version*)> unref_hook_;
};

// 安装期层级校验（I37，§3.4）。返回 false + why（层号 / 两个文件号 / 冲突 key）。
// 调用点：Recover 收尾、META 迁移、每一次 ApplyEdit/安装。
bool ValidateLevelLayout(const std::vector<std::vector<FileMetaData>>& levels,
                         const InternalKeyComparator& icmp, std::string* why);

// M3 的"META 持久化"入口 + M4 的 MANIFEST/CURRENT 入口。
class VersionSet {
 public:
  VersionSet() = default;
  static std::shared_ptr<const Version> Empty(uint64_t log_number, uint64_t next_file_number);
  static std::shared_ptr<const Version> RegisterFile(const Version& base, const FileMetaData& f,
                                                     uint64_t log_number,
                                                     uint64_t next_file_number);
  static std::shared_ptr<const Version> RegisterFile(const Version& base, const FileMetaData& f,
                                                     uint64_t log_number,
                                                     uint64_t min_log_number_to_keep,
                                                     uint64_t next_file_number);

  // 语义级文件名（M4 的取代路径只改这里；调用方不得拼接 "META" 字面量）。
  static std::string MetaFileName(const std::string& dbname);        // dbname + "/META"
  static std::string MetaTempFileName(const std::string& dbname);    // dbname + "/META.tmp"

  // M3-A38 的独立验证结果（§8.2 纪律 3）。
  struct RecoveryResult {
    bool meta_present = false;
    uint64_t sst_files_registered = 0;
    uint64_t sst_bytes_registered = 0;
    SequenceNumber max_sequence_in_files = 0;
    uint64_t unknown_metaindex_entries = 0;
  };

  // ---- M3 的 META 路径（逐字保留）----
  static Status Recover(Env* env, const std::string& dbname, const Options& options,
                        const std::vector<std::string>& children,
                        std::shared_ptr<const Version>* out, RecoveryResult* info);
  // M4.2：活动元数据回放后的**语义校验**（M3-A38 的覆盖保留）：注册文件必须存在、file_size 与磁盘
  // 一致、max_sequence 与全量扫描一致；不符 ⇒ kCorruption（不自动修复）。
  static Status VerifyRegisteredFiles(Env* env, const std::string& dbname, const Options& options,
                                      const Version& v, RecoveryResult* info);
  static Status Persist(Env* env, const std::string& dbname, const Options& options,
                        const Version& v, VersionEdit* edit);

  // ---- M4.1：MANIFEST / CURRENT ----
  struct ManifestReplayResult {
    bool manifest_present = false;
    bool migrated_from_meta = false;
    uint64_t manifest_number = 0;
    uint64_t manifest_bytes = 0;
    uint64_t edits_replayed = 0;
    uint64_t tail_truncated_bytes = 0;
    uint64_t unknown_record_types = 0;
    uint64_t next_version_number = 0;      // 已回放的安装型 edit 数（I35）
    uint64_t max_directory_number = 0;     // .log/.sst/MANIFEST 各族的最大编号（X5）
    uint64_t meta_delete_failed = 0;
    std::string truncation_note;
  };

  // 目录中所有族（.log / .sst / .sst.tmp / MANIFEST-<n> / MANIFEST-<n>.tmp）的最大编号。
  static uint64_t DirectoryMaxNumber(const std::vector<std::string>& children);

  // Open 的版本元数据恢复（§3.6 的优先级 ①CURRENT ②META 迁移 ③空库）。
  static Status RecoverManifest(Env* env, const std::string& dbname, const Options& options,
                                std::shared_ptr<const Version>* out, ManifestReplayResult* info);

  // CURRENT 的严格读写（§3.1）：内容 = ^[0-9]{1,20}\n$；写 = tmp→fsync→rename→SyncDir。
  static Status ReadCurrent(Env* env, const std::string& dbname, uint64_t* number);
  static Status WriteCurrentAtomic(Env* env, const std::string& dbname, uint64_t number);

  // 模式 (a)：写 MANIFEST-<number>（首条 = 全量快照）→ fsync → rename → SyncDir（不含 CURRENT）。
  static Status WriteSnapshotManifest(Env* env, const std::string& dbname, uint64_t number,
                                      const Version& v, const Options& options);

  // 模式 (b)：向 MANIFEST-<number> 追加一条 record → fsync；*manifest_bytes 回填新文件大小。
  static Status AppendEdit(Env* env, const std::string& dbname, uint64_t manifest_number,
                           const VersionEdit& edit, uint64_t* manifest_bytes);

  // 构造全量快照 edit（tag1..4 + 全部层级文件）。
  static VersionEdit MakeSnapshotEdit(const Version& v, const Options& options);

  // 应用一条 edit 得到新 Version（X7：先 DeleteFile 后 AddFile）；安装前 ValidateLevelLayout。
  static bool ApplyEdit(const Version& base, const VersionEdit& edit,
                        std::shared_ptr<const Version>* out, std::string* why);
};

// 【M4.2】有状态的 MANIFEST 追加句柄 + 模式 (a) 重建（§8.1）。
//   * Append    = 模式 (b)：向当前 MANIFEST 追加一条 record 并 fsync；
//   * RollAndOpen = 模式 (a)：写新 MANIFEST 的全量快照 → fsync → rename → SyncDir →
//                   写 CURRENT.tmp → fsync → rename(CURRENT) → SyncDir → 以新 MANIFEST 重开追加句柄。
//   旧 MANIFEST 的删除**不由本类执行**（返回 old_number，由调用方进延迟删除队列，L24/I43）。
class ManifestStore {
 public:
  ManifestStore(Env* env, std::string dbname, uint64_t roll_bytes);
  ~ManifestStore();
  ManifestStore(const ManifestStore&) = delete;
  ManifestStore& operator=(const ManifestStore&) = delete;

  Status OpenAppend(uint64_t number, uint64_t bytes);   // 恢复后打开追加点（追加点 = EOF）
  Status Append(const VersionEdit& edit);               // 模式 (b)
  Status RollAndOpen(const Version& snapshot, const Options& options, uint64_t new_number,
                     uint64_t* old_number);             // 模式 (a)
  bool open() const { return file_ != nullptr; }
  bool ShouldRoll() const { return number_ == 0 || bytes_ > roll_bytes_; }
  WritableFile* manifest_file() const { return file_.get(); }
  uint64_t number() const { return number_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t edits() const { return edits_; }
  uint64_t rolls() const { return rolls_; }

 private:
  Env* const env_;
  const std::string dbname_;
  const uint64_t roll_bytes_;
  uint64_t number_ = 0, bytes_ = 0, edits_ = 0, rolls_ = 0;
  std::unique_ptr<WritableFile> file_;
};

// 文件号 → shared_ptr<const Table> 的有界缓存（D8/I30/L19）。
class TableCache {
 public:
  TableCache(Env* env, std::string dbname, Options options, size_t capacity);
  ~TableCache();
  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  Status Get(const FileMetaData& f, const Slice& lookup_key, std::string* value,
             TableGetResult* result, ReadStats* stats = nullptr, bool* opened = nullptr);
  std::unique_ptr<Iterator> NewIterator(const FileMetaData& f, ReadStats* stats = nullptr);
  void Evict(uint64_t number);
  size_t size() const;

 private:
  // M5.1（docs/m5-design.md §11 M5.1 的「TableCache::Open 传递 open_stats，若需要」）：
  // open_stats 只在**缓存未命中、真的打开文件**时透传给 Table::Open，用于累计
  // filter_blocks_read / filter_bytes_read / filter_corrupt；缓存命中不重复读 filter，因此不计数。
  // 默认参数保证既有调用点零改动。
  Status Open(uint64_t number, const std::string& smallest, const std::string& largest,
              std::shared_ptr<const Table>* out, bool* opened, ReadStats* open_stats = nullptr);

  struct Entry {
    std::shared_ptr<const Table> table;
    uint64_t last_use = 0;
  };

  Env* const env_;
  const std::string dbname_;
  const Options options_;
  const size_t capacity_;
  mutable std::mutex mu_;
  std::map<uint64_t, Entry> cache_;
  uint64_t counter_ = 0;
};

}  // namespace lsm

#endif  // LSM_VERSION_SET_H_
