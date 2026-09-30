// src/sstable/table_builder.h —— SSTable 顺序写入器（M3.1；M5.1 追加 filter 块）
//
// 契约来源（逐条对应，禁止走样）：
//   docs/m3-design.md §3.5（写出顺序 data block* → metaindex → index → footer，顺序是格式的一部分）
//   docs/m3-design.md §3.3（索引项 key = 数据块**最后一条** internal key；索引 restart_interval = 1）
//   docs/m3-design.md §3.4（M3 写**空 metaindex**（8B payload）并在 footer 注册其 handle）
//   docs/m3-design.md §3.6（块头/CRC：crc32c(length ‖ type ‖ payload)）
//   docs/m3-design.md §3.2/§5.4（block_size 是目标值；唯一切块判据是 EstimatedSizeAfter > block_size）
//   docs/m3-design.md §10.1 的 M3-A09~A11
//   docs/m5-design.md §3.4（Finish 里写 filter 块 + metaindex 注册）、§3.6（Add 里的接入点与
//     「坏 internal key ⇒ 整文件禁用 filter」的防御）、E2（StartBlock 的 block_offset 语义）
//
// M5.1 的写出顺序变为 `data block* → filter block（可选）→ metaindex → index → footer`：
// filter 块必须在 metaindex **之前**（metaindex 的 entry 要引用它的 handle），
// 且不破坏 M3 的两条布局约束（§3.1）。
//
// 本类**不**做 fsync、不 rename、不注册（那是 flush 线程的职责，I22）；
// 前置条件：Add 的 key 严格递增（违反 ⇒ kInvalidArgument，且**不**把该条写进文件）。
#ifndef LSM_SSTABLE_TABLE_BUILDER_H_
#define LSM_SSTABLE_TABLE_BUILDER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "bloom.h"
#include "common.h"
#include "filter_policy.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "sstable/table.h"   // TableOptions（见 table.h 顶部的显式偏离说明）
#include "util/env.h"

namespace lsm {

class TableBuilder {
 public:
  // file 由调用方打开（典型为 `%06u.sst.tmp`）；本类不拥有、不 Close。
  // M5.1：options.bloom_bits > 0 时在构造期创建 filter 策略与 FilterBlockBuilder（单线程，L30）。
  TableBuilder(const TableOptions& options, WritableFile* file);
  ~TableBuilder() = default;
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 追加一条 internal key/value。违反严格递增 ⇒ kInvalidArgument（粘性，不写该条）。
  Status Add(const Slice& key, const Slice& value);

  // 收尾：封最后一个数据块 → 写 filter 块（若启用且未被禁用）→ 写 metaindex → 写索引块 → 写 footer。
  // 不 Sync；可重复调用（幂等）。
  Status Finish();

  Status status() const { return status_; }   // 粘性
  uint64_t FileSize() const { return file_size_; }
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t NumDataBlocks() const { return num_data_blocks_; }
  SequenceNumber MaxSequence() const { return max_sequence_; }
  const std::string& smallest() const { return smallest_; }
  const std::string& largest() const { return largest_; }

  // A10：本表所有数据块 payload 的最大字节数（证明 block_size 不是硬上限）。
  uint64_t max_block_size() const { return max_block_size_; }
  // A11：block_count > 65536 时 Finish 里递增；**不阻断**（§3.3 的登记阈值）。
  uint64_t index_size_warn_count() const { return index_size_warn_count_; }

  // ---- M5.1 诊断（M5-A06 的体积代价断言用）----
  // 已写 filter 块的 handle.size（**含 9B 外壳**）；未写 filter 时为 0（§3.6）。
  uint64_t filter_bytes() const { return filter_bytes_; }
  // 本表是否真的写出了一个 filter 块。
  bool has_filter_block() const { return filter_bytes_ != 0; }
  // filter 里 filter（bitset）的个数 = filter_offsets_.size()（未启用时为 0）。
  size_t filter_num_filters() const {
    return filter_builder_ == nullptr ? 0 : filter_builder_->NumFilters();
  }
  // 防御计数：Add 时遇到无法解析的 internal key ⇒ 整文件禁用 filter（§3.6）。
  uint64_t filter_skipped_bad_internal_key() const { return filter_skipped_bad_internal_key_; }
  bool filter_disabled() const { return filter_disabled_; }

 private:
  Status FlushBlock();   // 把当前 BlockBuilder 的 payload 加外壳写出，并注册索引项
  Status WriteBlock(BlockType type, const Slice& payload, BlockHandle* handle);

  TableOptions options_;
  WritableFile* file_ = nullptr;   // 不拥有
  InternalKeyComparator icmp_{BytewiseComparator()};

  BlockBuilder block_builder_;
  BlockBuilder index_builder_;
  Footer footer_;                    // index_handle / metaindex_handle（§3.7）
  std::string last_key_in_block_;   // 当前数据块最后一条 internal key（索引项 key）
  bool has_last_key_ = false;       // 用于严格递增检查（跨块也成立）

  Status status_;
  bool finished_ = false;
  uint64_t file_size_ = 0;
  uint64_t num_entries_ = 0;
  uint64_t num_data_blocks_ = 0;
  SequenceNumber max_sequence_ = 0;
  std::string smallest_;
  std::string largest_;
  uint64_t max_block_size_ = 0;
  uint64_t index_size_warn_count_ = 0;

  // ---- M5.1（§3.6）：filter 的写入侧状态（只在单线程构建期使用）----
  const FilterPolicy* filter_policy_ = nullptr;
  std::unique_ptr<FilterBlockBuilder> filter_builder_;
  bool filter_disabled_ = false;              // 坏 internal key ⇒ 整文件不写 filter
  uint64_t filter_skipped_bad_internal_key_ = 0;
  uint64_t filter_bytes_ = 0;                 // 含 9B 外壳
};

}  // namespace lsm

#endif  // LSM_SSTABLE_TABLE_BUILDER_H_
