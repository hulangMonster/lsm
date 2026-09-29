// src/version_edit.h —— M3 的「全量版本快照」内存表示 + 编解码（docs/m3-design.md §8.1/§10.8）
//
// M3.3 起本文件承担 META 的位级编码（§10.8）：
//   META := magic("LSMM",4B) ‖ format_version(4B LE=1) ‖ comparator_name(len-prefixed)
//           ‖ log_number(8B LE) ‖ min_log_number_to_keep(8B LE) ‖ next_file_number(8B LE)
//           ‖ file_count(4B LE) ‖ file* ‖ crc32c(4B LE, 覆盖前面全部字节)
//   file := number(8B LE) ‖ file_size(8B LE) ‖ max_sequence(8B LE)
//           ‖ smallest(len-prefixed internal key) ‖ largest(len-prefixed internal key)
//
// 纪律（§10.8 / M3-A39）：magic/version/长度/CRC 任一不符 ⇒ DecodeFrom 返回 false + why，
// **不修改 *this 的任何已有字段**（调用方映射为 kCorruption；comparator 不符由 VersionSet 映射
// 为 kInvalidArgument，见 §8.3 步骤 ⑤ 与 M3-A42）。
#ifndef LSM_VERSION_EDIT_H_
#define LSM_VERSION_EDIT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace lsm {

// META 的 magic 与格式版本（§10.2）。与 SSTable 的 "LSM1" 故意不同，防止两类文件互认。
constexpr char kMetaMagic[4] = {'L', 'S', 'M', 'M'};
constexpr uint32_t kMetaFormatVersion = 1;

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
  void ClearFiles() { files_.clear(); }

  // §10.8 的编码。返回 false 只在参数异常（正常内存对象恒成功）。
  bool EncodeTo(std::string* dst) const;

  // 解析并**校验**；失败返回 false 且 *why 给出精确字段/偏移；失败时 *this 不变。
  bool DecodeFrom(const Slice& src, std::string* why);

 private:
  uint64_t log_number_ = 0;
  uint64_t min_log_number_to_keep_ = 1;
  uint64_t next_file_number_ = 1;
  std::string comparator_name_;
  std::vector<FileMetaData> files_;
};

}  // namespace lsm

#endif  // LSM_VERSION_EDIT_H_
