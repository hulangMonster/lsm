// src/version_edit.h —— M3 的「全量版本快照」内存表示（docs/m3-design.md §8.1）
//
// 说明（M3.2 的显式范围）：本阶段版本注册**只在内存**，不写不读 META。因此本文件只提供
// FileMetaData 与 VersionEdit 的**内存字段/访问器**；§8.1 里的 EncodeTo/DecodeFrom（§10.8 的
// 位级编码）按「不提前实现下一步」纪律留到 M3.3（那时 META 持久化与恢复同批落地）。
// 这不是占位符：VersionSet 的注册路径真的使用本类来组装下一版文件列表。
#ifndef LSM_VERSION_EDIT_H_
#define LSM_VERSION_EDIT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace lsm {

// 一个已注册 SSTable 的元数据（§8.1）。smallest/largest 是 internal key（E2 的裁决）。
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  SequenceNumber max_sequence = 0;   // 文件内最大 sequence（E3；不是 largest 的 sequence）
  std::string smallest;              // internal key
  std::string largest;               // internal key
};

// 全量快照的内存表示：**没有**追加/差分语义（§8.1）。
class VersionEdit {
 public:
  VersionEdit() = default;

  void SetLogNumber(uint64_t n) { log_number_ = n; }
  uint64_t log_number() const { return log_number_; }

  void SetMinLogNumberToKeep(uint64_t n) { min_log_number_to_keep_ = n; }
  uint64_t min_log_number_to_keep() const { return min_log_number_to_keep_; }

  void SetNextFileNumber(uint64_t n) { next_file_number_ = n; }
  uint64_t next_file_number() const { return next_file_number_; }

  void SetComparatorName(const std::string& n) { comparator_name_ = n; }
  const std::string& comparator_name() const { return comparator_name_; }

  void AddFile(const FileMetaData& f) { files_.push_back(f); }
  const std::vector<FileMetaData>& files() const { return files_; }

 private:
  uint64_t log_number_ = 0;
  uint64_t min_log_number_to_keep_ = 1;
  uint64_t next_file_number_ = 1;
  std::string comparator_name_;
  std::vector<FileMetaData> files_;
};

}  // namespace lsm

#endif  // LSM_VERSION_EDIT_H_
