// src/sstable/table.h —— 只读 SSTable：footer/索引/metaindex 解析、块读取与 Get/迭代（M3.1）
//
// 契约来源（逐条对应，禁止各写一套）：
//   docs/m3-design.md §3.3（索引项 = 数据块最后一条 internal key；索引 restart_interval = 1）
//   docs/m3-design.md §3.4（metaindex；未知条目只计数不报错）
//   docs/m3-design.md §3.5（文件布局与顺序约束）、§3.6（handle.size 权威 + length 自检 + type 匹配）
//   docs/m3-design.md §3.7（footer 44B 与失败矩阵）、§5.4（Open/Get 的块读取路径）
//   docs/m3-design.md §10.1 的 M3-A08 后 3 行、M3-A09~M3-A19
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

#include "common.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "util/env.h"

namespace lsm {

// ---- 读路径选项（语义与设计 §4 的 Options::block_size / verify_checksums 一致）----
struct TableOptions {
  size_t block_size = 4096;        // 数据块 payload 的**目标值**，不是硬上限（§3.2）
  bool verify_checksums = true;    // 默认开；关掉只跳过 payload CRC，结构校验永不跳过（§5.4）
};

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

  void Clear() { *this = ReadStats(); }
};

// Table 是只读视图：不拥有文件名与 Env（Env 必须比 Table 长寿）；迭代器通过
// shared_from_this 持住 Table，所以 Table 必须用 std::shared_ptr 创建。
class Table : public std::enable_shared_from_this<Table> {
 public:
  // 打开：读末尾 44B footer → 校验（§3.7 失败矩阵）→ 读 metaindex → 读 index →
  // 读第一个数据块取最小 key（供 D8/A19 的零 IO key range 过滤）。
  // 失败：footer/块结构/CRC 问题 ⇒ kCorruption；version != 1 ⇒ kNotSupported；
  //       IO 失败 ⇒ kIOError；参数非法 ⇒ kInvalidArgument。
  static Status Open(const TableOptions& options, Env* env, const std::string& filename,
                     std::shared_ptr<Table>* table);

  ~Table();

  // §5.4 的 Get 路径：① key range 过滤（零 IO，key_range_skipped++）
  //                      ② 内存索引上 lower_bound（第一个 key >= lookup_key）
  //                      ③ 读一个数据块（blocks_read++）
  //                      ④ 块内 Seek ⑤ user key 相等校验 ⑥ tombstone/miss ⇒ kNotFound
  // 注意：本格式层的 kNotFound 同时覆盖"不存在"与"tombstone"；M3.2 的 DBIter 需要三态时
  // 会在上层再区分（不改变本函数的块读取与校验纪律）。
  Status Get(const Slice& lookup_key, std::string* value, ReadStats* stats = nullptr) const;

  // 内部 key 迭代器（归并/DBIter 的 child）；迭代器持住 Table 的 shared_ptr。
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
  Status ParseIndexBlock(const Slice& payload);
  Status ParseMetaIndexBlock(const Slice& payload);

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

  // 迭代器需要 index_entries_ / ReadBlock / options_ / smallest_ 等私有成员。
  class TableIterator;
  friend class TableIterator;
};

}  // namespace lsm

#endif  // LSM_SSTABLE_TABLE_H_
