// src/version_set.h —— M3 的不可变 Version + 内存 TableCache（docs/m3-design.md §7.4/§8.1）
//
// M3.2 范围：注册**只在内存**（不写 META）。VersionSet 的 META Recover/Persist 留到 M3.3，
// 因此本文件只保留当前阶段真正被使用的版本构建与缓存能力。
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

// M3.2 的内存注册入口；M3.3 的 META 持久化接在这里（不改调用方）。
class VersionSet {
 public:
  VersionSet() = default;
  static std::shared_ptr<const Version> Empty(uint64_t log_number, uint64_t next_file_number);
  // 返回一个把 f 加入 base 后、按文件号降序的新版本（base 不变）。
  static std::shared_ptr<const Version> RegisterFile(const Version& base, const FileMetaData& f,
                                                     uint64_t log_number,
                                                     uint64_t next_file_number);
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
