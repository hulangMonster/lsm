// src/sstable/table_builder.h —— SSTable 顺序写入器（M3.1）
//
// 契约来源（逐条对应，禁止走样）：
//   docs/m3-design.md §3.5（写出顺序 data block* → metaindex → index → footer，顺序是格式的一部分）
//   docs/m3-design.md §3.3（索引项 key = 数据块**最后一条** internal key；索引 restart_interval = 1）
//   docs/m3-design.md §3.4（M3 写**空 metaindex**（8B payload）并在 footer 注册其 handle）
//   docs/m3-design.md §3.6（块头/CRC：crc32c(length ‖ type ‖ payload)）
//   docs/m3-design.md §3.2/§5.4（block_size 是目标值；唯一切块判据是 EstimatedSizeAfter > block_size）
//   docs/m3-design.md §10.1 的 M3-A09~A11
//
// 本类**不**做 fsync、不 rename、不注册（那是 flush 线程的职责，I22）；
// 前置条件：Add 的 key 严格递增（违反 ⇒ kInvalidArgument，且**不**把该条写进文件）。
#ifndef LSM_SSTABLE_TABLE_BUILDER_H_
#define LSM_SSTABLE_TABLE_BUILDER_H_

#include <cstdint>
#include <string>

#include "common.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "sstable/table.h"   // TableOptions（见 table.h 顶部的显式偏离说明）
#include "util/env.h"

namespace lsm {

class TableBuilder {
 public:
  // file 由调用方打开（典型为 `%06u.sst.tmp`）；本类不拥有、不 Close。
  TableBuilder(const TableOptions& options, WritableFile* file);
  ~TableBuilder() = default;
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 追加一条 internal key/value。违反严格递增 ⇒ kInvalidArgument（粘性，不写该条）。
  Status Add(const Slice& key, const Slice& value);

  // 收尾：封最后一个数据块 → 写空 metaindex → 写索引块 → 写 footer（顺序不可交换，§3.5）。
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
};

}  // namespace lsm

#endif  // LSM_SSTABLE_TABLE_BUILDER_H_
