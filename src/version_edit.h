// src/version_edit.h —— 版本编辑：M3 的「全量快照（META）」+ M4 的「差分（MANIFEST）」
//                        （docs/m3-design.md §8.1/§10.8、docs/m4-design.md §3.3/§5.2、docs/protocol.md §11.3）
#ifndef LSM_VERSION_EDIT_H_
#define LSM_VERSION_EDIT_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common.h"

namespace lsm {

// META 的 magic 与格式版本（§10.2）。与 SSTable 的 "LSM1" 故意不同，防止两类文件互认。
constexpr char kMetaMagic[4] = {'L', 'S', 'M', 'M'};
constexpr uint32_t kMetaFormatVersion = 1;

// MANIFEST record 的类型与上界（§3.2）。
constexpr uint8_t kManifestRecordTypeVersionEdit = 0x01;
constexpr uint64_t kMaxManifestRecordBytes = 64ull * 1024 * 1024;   // 64 MiB 软上界

// VersionEdit payload 的 tag 值（§3.3 只定义实际使用的 6 个）。
enum VersionEditTag : uint32_t {
  kTagComparator = 1,
  kTagLogNumber = 2,
  kTagNextFileNumber = 3,
  kTagMinLogNumberToKeep = 4,
  kTagDeletedFile = 5,
  kTagNewFile = 6,
};

// 一个已注册 SSTable 的元数据（§8.1）。smallest/largest 是 internal key（E2 的裁决）。
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  SequenceNumber max_sequence = 0;   // 文件内最大 sequence（E3；不是 largest 的 sequence）
  std::string smallest;              // internal key
  std::string largest;               // internal key

  uint64_t smallest_user_key_size() const;
};

// MANIFEST record 读取的三态（§8.4）：尾部残骸可截断；中间损坏拒绝；未知 type ⇒ kNotSupported。
enum class ManifestReadStatus {
  kOk = 0,
  kTailResidue = 1,     // 读不出完整 record（尾部半条）⇒ 调用方截断到 last_good_end
  kCorruption = 2,      // 长度/CRC/字段非法
  kNotSupported = 3,    // record type 未知
};

// 版本编辑：M3 的全量快照 + M4 的差分语义（X7：应用时**先 DeleteFile 后 AddFile**）。
class VersionEdit {
 public:
  VersionEdit() = default;

  // 【M4 新增】"全或无"解码的前提（§5.2）。
  void Clear();

  void SetLogNumber(uint64_t n) { log_number_ = n; has_log_number_ = true; }
  uint64_t log_number() const { return log_number_; }
  bool has_log_number() const { return has_log_number_; }

  void SetMinLogNumberToKeep(uint64_t n) { min_log_number_to_keep_ = n; has_min_log_number_to_keep_ = true; }
  uint64_t min_log_number_to_keep() const { return min_log_number_to_keep_; }
  bool has_min_log_number_to_keep() const { return has_min_log_number_to_keep_; }

  void SetNextFileNumber(uint64_t n) { next_file_number_ = n; has_next_file_number_ = true; }
  uint64_t next_file_number() const { return next_file_number_; }
  bool has_next_file_number() const { return has_next_file_number_; }

  void SetComparatorName(const std::string& n) { comparator_name_ = n; has_comparator_ = true; }
  const std::string& comparator_name() const { return comparator_name_; }
  bool has_comparator() const { return has_comparator_; }

  // 兼容别名（M3）：等价于 AddFile(0, f)。
  void AddFile(const FileMetaData& f) { AddFile(0, f); }
  // 【M4 扩展】带 level 的注册（§5.2）。
  void AddFile(int level, const FileMetaData& f);
  // 【M4 新增】删除某个 (level, number) 的注册。
  void DeleteFile(int level, uint64_t number) { deleted_.emplace_back(level, number); }

  // 兼容别名（M3）：返回**全部**新增文件（跨层、按加入顺序）；M3 的库全部是 L0。
  const std::vector<FileMetaData>& files() const { return flat_files_; }
  // 【M4 新增】带 level 的视图与删除集合。
  const std::vector<std::pair<int, FileMetaData>>& added_files() const { return added_; }
  const std::vector<std::pair<int, uint64_t>>& deleted_files() const { return deleted_; }

  bool empty() const {
    return !has_comparator_ && !has_log_number_ && !has_next_file_number_ &&
           !has_min_log_number_to_keep_ && added_.empty() && deleted_.empty();
  }

  std::string DebugString() const;

  // ---- M3 的 META 全量快照编码（**逐字节不改**；兼容读入格式）----
  bool EncodeTo(std::string* dst) const;
  bool DecodeFrom(const Slice& src, std::string* why);

  // ---- M4 的 VersionEdit 差分 payload 编码（docs/protocol.md §11.3）----
  bool EncodePayloadTo(std::string* dst) const;
  bool DecodePayloadFrom(const Slice& src, std::string* why);

 private:
  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_min_log_number_to_keep_ = false;
  std::string comparator_name_;
  uint64_t log_number_ = 0;
  uint64_t min_log_number_to_keep_ = 1;
  uint64_t next_file_number_ = 1;
  std::vector<std::pair<int, FileMetaData>> added_;
  std::vector<std::pair<int, uint64_t>> deleted_;
  std::vector<FileMetaData> flat_files_;   // files() 的兼容视图
};

// 把 payload 包成 §3.2 的 record 帧。payload 超 64 MiB 上界或为空 ⇒ false。
bool EncodeManifestRecord(const VersionEdit& edit, std::string* dst);

// 从 buf 的 *offset 处读一条 record；成功时 *offset 前进到 record 末尾。
// kTailResidue/kCorruption/kNotSupported 时 *offset 不变（调用方决定截断或拒绝）。
ManifestReadStatus ReadManifestRecord(const Slice& buf, size_t* offset, VersionEdit* out,
                                      std::string* why);

}  // namespace lsm

#endif  // LSM_VERSION_EDIT_H_
