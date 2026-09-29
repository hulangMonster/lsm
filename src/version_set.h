// src/version_set.h —— M3 的不可变 Version + 内存 TableCache + META 持久化（docs/m3-design.md §7.4/§8.1）
//
// M3.3 起 VersionSet 的 Recover/Persist 真正落地（§8.1/§10.8）：
//   Recover 读 META（或按 §10.9 的兼容规则重建），返回不可变 Version 与恢复计数；
//   Persist 把当前 Version 作为**全量快照**写成 META.tmp → fsync → rename → SyncDir。
// 迁移点（§D4/L17）：M4 换 MANIFEST+CURRENT 时只改这两个函数体，调用方不动。
#ifndef LSM_VERSION_SET_H_
#define LSM_VERSION_SET_H_

#include <cstdint>
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

// ---- 文件命名（docs/protocol.md §10.1）----
// 设计 P17 要求补进 src/filename.*，但 M3.2 的文件许可不含它；先落到本 TU，语义与 §10.1 一致。
std::string TableFileName(const std::string& dbname, uint64_t number);   // %06u.sst
std::string TempFileName(const std::string& dbname, uint64_t number);    // %06u.sst.tmp
bool ParseTableFileName(const std::string& fname, uint64_t* number);     // 精确后缀，拒绝 .sst.tmp
bool ParseTempFileName(const std::string& fname, uint64_t* number);      // %06u.sst.tmp

// 不可变版本（L15/I21）：构造后只读；注册 = 造新对象 + 原子替换 shared_ptr。
class Version {
 public:
  // files 会被按文件号**降序**排好（新→旧，§7.1 顺序规则 2）。
  Version(std::vector<FileMetaData> files, uint64_t log_number, uint64_t min_log_number_to_keep,
          uint64_t next_file_number);

  const std::vector<FileMetaData>& files() const { return files_; }
  uint64_t log_number() const { return log_number_; }
  uint64_t min_log_number_to_keep() const { return min_log_number_to_keep_; }
  uint64_t next_file_number() const { return next_file_number_; }
  SequenceNumber MaxSequenceInFiles() const;

 private:
  std::vector<FileMetaData> files_;
  const uint64_t log_number_;
  const uint64_t min_log_number_to_keep_;
  const uint64_t next_file_number_;
};

// M3 的"META 持久化"入口（§D4 的迁移点：M4 换成 MANIFEST 只改这个函数体）。
class VersionSet {
 public:
  VersionSet() = default;
  static std::shared_ptr<const Version> Empty(uint64_t log_number, uint64_t next_file_number);
  // 返回一个把 f 加入 base 后、按文件号降序的新版本（base 不变）。
  static std::shared_ptr<const Version> RegisterFile(const Version& base, const FileMetaData& f,
                                                     uint64_t log_number,
                                                     uint64_t next_file_number);
  // M3.3：注册时可显式给出重算后的 min_log_number_to_keep（I34 的单一落点）。
  static std::shared_ptr<const Version> RegisterFile(const Version& base, const FileMetaData& f,
                                                     uint64_t log_number,
                                                     uint64_t min_log_number_to_keep,
                                                     uint64_t next_file_number);

  // 语义级文件名（M4 换 MANIFEST+CURRENT 时只改这里；调用方不得拼接 "META" 字面量）。
  static std::string MetaFileName(const std::string& dbname);        // dbname + "/META"
  static std::string MetaTempFileName(const std::string& dbname);    // dbname + "/META.tmp"

  // M3-A38 的独立验证结果（§8.2 纪律 3）：每个已注册 .sst 全量扫描得到的真实 max_sequence
  // 必须等于 META 里记录的值，否则 kCorruption（把"统计写错"从静默错误变成显式失败）。
  struct RecoveryResult {
    bool meta_present = false;
    uint64_t sst_files_registered = 0;
    uint64_t sst_bytes_registered = 0;
    SequenceNumber max_sequence_in_files = 0;
    uint64_t unknown_metaindex_entries = 0;
  };

  // 读版本元数据（§8.3 步骤 ⑤；children 是目录枚举结果，避免二次扫描）。
  //   META 存在：解析 + 校验 comparator_name（不符 ⇒ kInvalidArgument，M3-A42）；
  //              每个注册文件必须存在（否则 kCorruption）；max_sequence 全量扫描复核。
  //   META 缺失：目录里有 *.sst ⇒ kCorruption（§10.9 安全阀，M3-A41）；否则空版本。
  static Status Recover(Env* env, const std::string& dbname, const Options& options,
                        const std::vector<std::string>& children,
                        std::shared_ptr<const Version>* out, RecoveryResult* info);

  // 把 v 作为**全量快照**持久化：META.tmp → fsync → rename(META) → SyncDir（L17）。
  // `edit` 被填成即将写入的快照（供调用方观测/测试）。
  static Status Persist(Env* env, const std::string& dbname, const Options& options,
                        const Version& v, VersionEdit* edit);
};

// 文件号 → shared_ptr<const Table> 的有界缓存（D8/I30/L19）。
// 淘汰时只释放 shared_ptr；Table 的析构（可能 close 句柄）发生在锁外。
class TableCache {
 public:
  TableCache(Env* env, std::string dbname, Options options, size_t capacity);
  ~TableCache();
  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // 打开/复用 f 对应的 Table，然后走三态 Get。*opened 表示本次是否真的打开了一个新 Table
  // （供读放大统计的 index_blocks_read 口径）。stats 透传给 Table::GetEntry（可为 nullptr）。
  Status Get(const FileMetaData& f, const Slice& lookup_key, std::string* value,
             TableGetResult* result, ReadStats* stats = nullptr, bool* opened = nullptr);

  // 供 DBIter 的 child；返回值持有 Table 的 shared_ptr（L19）。
  std::unique_ptr<Iterator> NewIterator(const FileMetaData& f, ReadStats* stats = nullptr);

  void Evict(uint64_t number);
  size_t size() const;

 private:
  // 取/开 Table：命中走缓存，未命中走 Table::Open（IO 在缓存锁外）。
  Status Open(uint64_t number, const std::string& smallest, const std::string& largest,
              std::shared_ptr<const Table>* out, bool* opened);

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
