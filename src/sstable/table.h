// src/sstable/table.h —— 只读 SSTable：footer/索引/metaindex 解析、块读取与 Get/迭代（M3.1）
//
// 契约来源（逐条对应，禁止各写一套）：
//   docs/m3-design.md §3.3（索引项 = 数据块最后一条 internal key；索引 restart_interval = 1）
//   docs/m3-design.md §3.4（metaindex；未知条目只计数不报错）
//   docs/m3-design.md §3.5（文件布局与顺序约束）、§3.6（handle.size 权威 + length 自检 + type 匹配）
//   docs/m3-design.md §3.7（footer 44B 与失败矩阵）、§5.4（Open/Get 的块读取路径）
//   docs/m3-design.md §10.1 的 M3-A08 后 3 行、M3-A09~M3-A19
//   docs/m5-design.md §3.4/§3.6/§3.7（M5.1：metaindex 里的 filter name、读 filter、KeyMayMatch、
//     GetEntry 的 step ②.5、ReadStats 追加列）、E3（filter 块 CRC 始终校验）、§4 §12（protocol 追加）
//
// 与设计的一处显式偏离（已登记，等待裁决）：
//   §4/§405 要求 Options 增加 `block_size` / `verify_checksums`，但 §11.1 与本任务的文件许可
//   都明确 **不得改 src/common.h**。因此这里用 `TableOptions` 承载这两个字段，取值与语义逐字
//   一致（默认 4096 / true）；`TableBuilder` 与 `Table` 共用同一个结构。
//
// 另一个显式偏离：设计 §11.1 要求给 `src/util/env.h` 增加 `RandomAccessFile`，但本任务的文件
//   许可不包含 env.h。为不改动冻结接口，本实现用 `Env::NewSequentialFile + Skip + Read`
//   实现按偏移读块（每次读块打开一次顺序文件）。语义与 RandomAccessFile 相同；代价是句柄 churn，
//   已登记为 M3.2 用 TableCache/RandomAccessFile 替换的替换点。
#ifndef LSM_SSTABLE_TABLE_H_
#define LSM_SSTABLE_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bloom.h"
#include "common.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "util/env.h"

namespace lsm {

// ---- 读路径选项 ----
// M3.2：按设计把 block_size / verify_checksums 统一进 common.h 的 Options（§8.5）；
// 这里保留 TableOptions 作为**别名**，使 M3.1 的既有用例与 TableBuilder/Table 的签名零改动。
using TableOptions = Options;

// ---- D8/A19 的读放大口径：全部是原始计数，不做聚合 ----
struct ReadStats {
  uint64_t files_checked = 0;        // 真的进了 Table::Get 的文件数（本类内部为 1）
  uint64_t key_range_skipped = 0;    // 因 key range 过滤而零 IO 直接返回的次数
  uint64_t index_blocks_read = 0;    // 读索引块的次数（M3.1 的 Open 一次）
  uint64_t data_blocks_read = 0;     // 读数据块的次数
  uint64_t blocks_read = 0;          // 上述两者之和（A19 断言范围外 key 为 0）
  uint64_t bytes_read = 0;           // 含块头/CRC/restart 数组的**全部**字节
  uint64_t crc_checked = 0;          // 真正算过 CRC 的次数
  uint64_t crc_failed = 0;           // CRC 不匹配的次数

  // ---- M5.1 追加（docs/m5-design.md §3.6 的 7 列 + §3.7/§10.1 M5-A08 要求的 filter_corrupt 计数）----
  // 只追加、不改既有 8 列的语义（D-5）。
  uint64_t filter_checked = 0;                  // KeyMayMatch 被调用的次数
  uint64_t filter_negative = 0;                 // KeyMayMatch 返回 false 的次数
  uint64_t filter_positive = 0;                 // KeyMayMatch 返回 true 的次数
  uint64_t filter_unavailable = 0;              // 本文件没有可用 filter（kAbsent/kCorrupt）时按 true 处理的次数
  uint64_t filter_blocks_read = 0;              // 读 filter 块的次数（只在 Table::Open 成功读到 filter 时 +1）
  uint64_t filter_bytes_read = 0;               // filter payload 字节数
  uint64_t data_blocks_skipped_by_filter = 0;   // 因 filter 否定而省掉的数据块读次数
  uint64_t filter_corrupt = 0;                  // filter 存在但结构/CRC 不自洽、被迫禁用的文件数

  void Clear() { *this = ReadStats(); }
};

// M3.2 的三态结果（设计 §5.4 的 ⑥：tombstone 与「不存在」必须能区分，
// 否则 DB 层在 L0 按新→旧查文件时会把「本文件里的 tombstone」误当成「本文件没有该 key」
// 而继续向更旧文件查，造成删除复活）。链路上层（db_impl/TableCache）只使用 GetEntry；
// Get() 保留 M3.1 的 Status 语义（两者都映射为 kNotFound）以保持格式层契约稳定。
enum class TableGetResult {
  kFound,
  kDeleted,
  kNotFound,
};

// Table 是只读视图：不拥有文件名与 Env（Env 必须比 Table 长寿）；迭代器通过
// shared_from_this 持住 Table，所以 Table 必须用 std::shared_ptr 创建。
class Table : public std::enable_shared_from_this<Table> {
 public:
  // ---- M5.1（docs/m5-design.md §3.6/§3.7）----
  // filter 的三态：kAbsent（metaindex 无该 name：M3/M4 旧文件，或 bloom_bits==0 的新文件）、
  // kOk（读到且结构自洽）、kCorrupt（存在但 handle 越界 / 读块失败 / payload 不自洽 / 重复注册）。
  // 三态在读路径上等价（都按「可能存在」处理）；分开只为可观测性，kAbsent 不等于损坏。
  enum class FilterState { kAbsent, kOk, kCorrupt };

  // 打开：读末尾 44B footer → 校验（§3.7 失败矩阵）→ 读 metaindex → 读 index →
  // 读第一个数据块取最小 key（供 D8/A19 的零 IO key range 过滤）。
  // 失败：footer/块结构/CRC 问题 ⇒ kCorruption；version != 1 ⇒ kNotSupported；
  //       IO 失败 ⇒ kIOError；参数非法 ⇒ kInvalidArgument。
  // known_smallest / known_largest 非空时，Open 直接采用调用方给出的 key range
  // （M3.2 的 Version/FileMetaData 已持有 TableBuilder 统计出的 internal key），
  // 从而跳过「预读第一个数据块取 smallest_」这一步（docs/m3-evidence §4 未闭合项 5）。
  // 传 nullptr（默认）保持 M3.1 行为：预读首块。空表可传两个非空但为空串的指针。
  //
  // M5.1 追加：open_stats（默认 nullptr）——传入时，成功读到 filter 块的次数/字节数与
  // 「filter 存在但损坏」的文件数累加进来。**默认参数保证 M3/M4 既有调用零改动**（D-3）。
  // 注意：filter 的任何结构/CRC 问题都**不**让 Open 失败（M5:35 的降级纪律）。
  static Status Open(const TableOptions& options, Env* env, const std::string& filename,
                     std::shared_ptr<Table>* table, const std::string* known_smallest = nullptr,
                     const std::string* known_largest = nullptr, ReadStats* open_stats = nullptr);

  ~Table();

  // M3.2 的三态入口：返回值仍用 Status 传结构/IO/CRC 错误，命中种类走 *result。
  // ① key range 过滤（零 IO，key_range_skipped++）② 内存索引 lower_bound
  // ②.5 filter 否定判断（M5.1；**唯一**允许的否定点，negative ⇒ 直接 kNotFound）
  // ③ 读一个数据块（blocks_read++）④ 块内 Seek ⑤ user key 相等校验
  // ⑥ tombstone ⇒ kDeleted；值 ⇒ kFound；其余 ⇒ kNotFound。
  Status GetEntry(const Slice& lookup_key, std::string* value, TableGetResult* result,
                  ReadStats* stats = nullptr) const;
  // M3.1 的兼容包装：kFound ⇒ kOk，kDeleted/kNotFound ⇒ kNotFound（信息里保留 tombstone 字样）。
  Status Get(const Slice& lookup_key, std::string* value, ReadStats* stats = nullptr) const;

  // 内部 key 迭代器（归并/DBIter 的 child）；迭代器持住 Table 的 shared_ptr。
  // **绝不使用 filter**（M5-design §3.7 硬规则 4：全量迭代必须读所有数据块）。
  std::unique_ptr<Iterator> NewIterator(ReadStats* stats = nullptr) const;
  // 便利入口：等价于 NewIterator() 后 Seek(target)；返回的迭代器可能 Invalid()。
  std::unique_ptr<Iterator> Seek(const Slice& target, ReadStats* stats = nullptr) const;

  // 该文件最后一条 internal key（= 最后一个索引项的 key）；空表返回空串。
  const std::string& LastInternalKey() const { return largest_; }
  // 第一个数据块的第一条 internal key；空表返回空串。供 key range 过滤（§5.4 ①）。
  const std::string& FirstInternalKey() const { return smallest_; }

  uint64_t file_size() const { return file_size_; }
  uint64_t NumDataBlocks() const { return index_entries_.size(); }
  const std::string& filename() const { return filename_; }
  const Footer& footer() const { return footer_; }
  bool verify_checksums() const { return options_.verify_checksums; }

  // metaindex 里未识别的条目数（§3.4：只记录、计数、不报错）。
  uint64_t unknown_metaindex_entries() const { return unknown_metaindex_entries_; }
  const std::vector<std::string>& unknown_metaindex_names() const { return unknown_metaindex_names_; }

  const InternalKeyComparator& internal_comparator() const { return icmp_; }

  // ---- M5.1：filter 的只读视图（构造后不变；L30/I50）----
  FilterState filter_state() const { return filter_state_; }
  bool has_filter() const { return filter_state_ == FilterState::kOk; }
  uint64_t filter_bytes() const { return filter_payload_bytes_; }
  // 只用于读路径「决定是否读数据块」：false ⇒ 否定（可跳过）；true ⇒ 可能存在/不可用。
  // 计数写调用方传入的线程局部 ReadStats（L34），本对象不持有任何可变计数器。
  bool KeyMayMatch(uint64_t block_offset, const Slice& user_key, ReadStats* stats) const;

  // 读一个块：按 handle.size 读（**唯一**的读取长度权威）→ length 自检 → type 匹配 →
  // （可选）CRC → length 上界自检。结构校验在关掉 verify_checksums 时**仍然生效**（A15）。
  // 公开是因为 A12/A13/A14/A17 需要逐字节手术后台直接观察"读了多少/算了没有"；
  // 正常读路径只由 Get/迭代器调用。
  Status ReadBlock(const BlockHandle& handle, BlockType expected, std::string* payload,
                   ReadStats* stats = nullptr) const;

 private:
  struct IndexEntry {
    std::string key;   // 数据块最后一条 internal key
    BlockHandle handle;
  };

  Table() = default;

  Status ReadAt(uint64_t offset, size_t n, std::string* out) const;
  // M5.1（E3）：verify_crc 显式传入，filter 块走 verify_crc=true（不受 verify_checksums 影响），
  // 数据块仍走 options_.verify_checksums。其余语义与公开 ReadBlock 逐字相同。
  Status ReadBlockImpl(const BlockHandle& handle, BlockType expected, std::string* payload,
                       ReadStats* stats, bool verify_crc) const;
  Status ParseIndexBlock(const Slice& payload);
  Status ParseMetaIndexBlock(const Slice& payload);
  // M5.1：metaindex 解析出 filter handle 之后，读 filter 块并构造 FilterBlockReader（降级不报错）。
  void LoadFilterBlock(ReadStats* open_stats);

  TableOptions options_;
  Env* env_ = nullptr;                    // 不拥有（调用方保证长寿）
  std::string filename_;
  uint64_t file_size_ = 0;
  Footer footer_;
  std::vector<IndexEntry> index_entries_;
  std::string smallest_;                   // 第一个数据块首条 internal key
  std::string largest_;                    // 最后一个索引项 key
  uint64_t unknown_metaindex_entries_ = 0;
  std::vector<std::string> unknown_metaindex_names_;
  InternalKeyComparator icmp_{BytewiseComparator()};

  // ---- M5.1：filter 状态（Open 期一次性决定，之后只读）----
  FilterState filter_state_ = FilterState::kAbsent;
  bool has_filter_handle_ = false;
  bool filter_handle_seen_ = false;        // 用于检出「同名重复注册」
  BlockHandle filter_handle_;
  uint64_t filter_payload_bytes_ = 0;
  std::unique_ptr<FilterBlockReader> filter_;

  // 迭代器需要 index_entries_ / ReadBlock / options_ / smallest_ 等私有成员。
  class TableIterator;
  friend class TableIterator;
};

}  // namespace lsm

#endif  // LSM_SSTABLE_TABLE_H_
