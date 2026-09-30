// src/sstable/table_builder.cpp —— SSTable 顺序写入（M3.1；M5.1 追加 filter 块）
//
// 契约：docs/m3-design.md §3.2（block_size 是目标值）、§3.3（索引项/索引 restart=1）、
//       §3.4（空 metaindex 必须注册 handle）、§3.5（写出顺序）、§3.6（CRC 覆盖 length‖type‖payload）、
//       §5.4（唯一判据 EstimatedSizeAfter）、§3.3 的 index_size_warn 登记阈值。
//       docs/m5-design.md §3.1（filter 块在 metaindex 之前）、§3.4（Finish 的注册顺序）、
//       §3.6（Add 的 StartBlock/AddKey 接入点、坏 internal key 的防御）、protocol §12。
#include "sstable/table_builder.h"

#include <cstdio>
#include <cstring>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

namespace {
// §3.3：block_count > 65536（单层索引 > ~2.5MB）⇒ WARN + 计数，不阻断。
constexpr uint64_t kIndexSizeWarnBlockCount = 65536;
}  // namespace

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      block_builder_(kRestartInterval),
      index_builder_(kIndexRestartInterval) {
  // M5.1（§3.6）：bloom_bits ∈ [1,64] 时启用 filter；0 表示关闭（新文件不写 filter 块）。
  // 合法性由 DB::Open 第一步校验（§5.6）；这里只做「取不到策略就禁用」的兜底。
  if (options_.bloom_bits > 0) {
    filter_policy_ = NewBuiltinBloomPolicy(options_.bloom_bits);
    if (filter_policy_ != nullptr) {
      filter_builder_.reset(new FilterBlockBuilder(filter_policy_));
    }
  }
}

Status TableBuilder::Add(const Slice& key, const Slice& value) {
  if (!status_.ok()) return status_;
  if (file_ == nullptr) {
    status_ = Status::InvalidArgument("TableBuilder::Add", "null WritableFile");
    return status_;
  }
  if (has_last_key_ && icmp_.Compare(key, Slice(largest_)) <= 0) {
    // §5.4：违反严格递增 ⇒ kInvalidArgument，**不**把该条写进文件（也不封块）。
    status_ = Status::InvalidArgument("TableBuilder::Add", "keys must be strictly increasing");
    return status_;
  }

  // §5.4：**唯一的切块判据** —— 加上这一条之后是否超过 block_size；超了先封块。
  // 因此单条 entry 可以超过 block_size（block_size 是目标值，不是硬上限）。
  if (!block_builder_.empty() &&
      block_builder_.EstimatedSizeAfter(key, value) > options_.block_size) {
    const Status s = FlushBlock();
    if (!s.ok()) return s;
  }

  // M5.1（§3.6/E2）：这是**新数据块的第一条 entry**（block_builder_.empty() 为真），
  // 此刻 file_size_ 就是该数据块的起始文件偏移 ⇒ 用它划定 filter 的覆盖桶。
  if (block_builder_.empty() && filter_builder_ != nullptr) {
    filter_builder_->StartBlock(file_size_);
  }

  if (!has_last_key_) {
    smallest_.assign(key.data(), key.size());
    has_last_key_ = true;
  }
  block_builder_.Add(key, value);
  last_key_in_block_.assign(key.data(), key.size());
  largest_.assign(key.data(), key.size());
  ++num_entries_;

  Slice user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  if (ParseInternalKey(key, &user_key, &seq, &type)) {
    if (seq > max_sequence_) max_sequence_ = seq;
    // M5.1（D2）：对**每一条 entry 的 user key**（含 tombstone）加入当前桶。
    if (filter_builder_ != nullptr && !filter_disabled_) {
      filter_builder_->AddKey(user_key);
    }
  } else {
    // M5.1（§3.6）：输入损坏 ⇒ 整文件禁用 filter。否则写出的是**不完整的 key 集合**，
    // 后续会对合法 key 产生假阴性（丢数据级）。不改变 M3 的既有行为（M3 也不校验语义）。
    filter_disabled_ = true;
    ++filter_skipped_bad_internal_key_;
  }
  return status_;
}

Status TableBuilder::FlushBlock() {
  if (!status_.ok()) return status_;
  if (block_builder_.empty()) return status_;   // 空表不写数据块（§3.2 末段的例外说明）
  if (last_key_in_block_.empty()) {
    status_ = Status::Corruption("TableBuilder::FlushBlock", "missing last key for index entry");
    return status_;
  }

  const Slice payload = block_builder_.Finish();
  BlockHandle handle;
  Status s = WriteBlock(kBlockTypeData, payload, &handle);
  if (!s.ok()) return s;
  if (payload.size() > max_block_size_) max_block_size_ = payload.size();

  std::string handle_bytes;
  handle.EncodeTo(&handle_bytes);
  // §3.3：索引项 key = **该数据块内最后一条** internal key。
  index_builder_.Add(Slice(last_key_in_block_), Slice(handle_bytes));

  block_builder_.Reset();
  last_key_in_block_.clear();
  ++num_data_blocks_;
  return status_;
}

Status TableBuilder::WriteBlock(BlockType type, const Slice& payload, BlockHandle* handle) {
  if (!status_.ok()) return status_;
  if (payload.size() > 0xffffffffull) {
    status_ = Status::InvalidArgument("TableBuilder::WriteBlock", "payload exceeds uint32 length");
    return status_;
  }
  std::string buf;
  buf.reserve(kBlockOverhead + payload.size());
  PutFixed32(&buf, static_cast<uint32_t>(payload.size()));
  buf.push_back(static_cast<char>(type));
  buf.append(payload.data(), payload.size());
  // §3.6：CRC 覆盖 length(4B LE) ‖ type(1B) ‖ payload 的全部字节。
  PutFixed32(&buf, crc32c::Value(buf.data(), buf.size()));

  handle->offset = file_size_;
  handle->size = buf.size();
  const Status s = file_->Append(Slice(buf));
  if (!s.ok()) {
    status_ = s;
    return status_;
  }
  file_size_ += buf.size();
  return status_;
}

Status TableBuilder::Finish() {
  if (!status_.ok()) return status_;
  if (finished_) return status_;
  if (file_ == nullptr) {
    status_ = Status::InvalidArgument("TableBuilder::Finish", "null WritableFile");
    return status_;
  }

  // ① 封最后一个数据块（若一个都没有则跳过：空表不写数据块）
  Status s = FlushBlock();
  if (!s.ok()) return s;

  // ② M5.1（§3.4 步骤 1）：写 filter 块。条件：启用 filter 且至少有 1 条 entry 且未被禁用。
  //    num_entries_ == 0 ⇒ metaindex 与 M3 逐字一致（空表），不产生 8B 的空 filter 块。
  BlockHandle filter_handle;
  bool has_filter = false;
  if (filter_builder_ != nullptr && !filter_disabled_ && num_entries_ > 0) {
    const Slice filter_payload = filter_builder_->Finish();
    s = WriteBlock(kBlockTypeFilter, filter_payload, &filter_handle);
    if (!s.ok()) return s;
    filter_bytes_ = filter_handle.size;   // 含 9B 外壳（§3.6）
    has_filter = true;
  }

  // ③ metaindex（§3.4 步骤 2）：M3 写空块（payload 恰 8B）；M5 多一条 filter name → handle。
  {
    BlockBuilder meta_builder(kIndexRestartInterval);
    if (has_filter) {
      std::string handle_bytes;
      filter_handle.EncodeTo(&handle_bytes);
      meta_builder.Add(Slice(kBuiltinBloomFilterName), Slice(handle_bytes));
    }
    const Slice meta_payload = meta_builder.Finish();
    BlockHandle meta_handle;
    s = WriteBlock(kBlockTypeMetaIndex, meta_payload, &meta_handle);
    if (!s.ok()) return s;
    footer_.metaindex_handle = meta_handle;
  }

  // ④ 索引块（restart_interval = 1，§3.3）
  {
    const Slice index_payload = index_builder_.Finish();
    BlockHandle index_handle;
    s = WriteBlock(kBlockTypeIndex, index_payload, &index_handle);
    if (!s.ok()) return s;
    footer_.index_handle = index_handle;
  }

  // ⑤ footer（44B，文件末尾）
  std::string footer_bytes;
  footer_.EncodeTo(&footer_bytes);
  s = file_->Append(Slice(footer_bytes));
  if (!s.ok()) {
    status_ = s;
    return status_;
  }
  file_size_ += footer_bytes.size();

  // §3.3：block_count > 65536 ⇒ WARN 并计数，**不阻断**（提醒 M4 上两级索引）。
  if (num_data_blocks_ > kIndexSizeWarnBlockCount) {
    ++index_size_warn_count_;
    std::fprintf(stderr,
                 "WARN: sstable index_size_warn: block_count=%llu > %llu "
                 "(single-level index > ~2.5MB); two-level index is an M4 item\n",
                 static_cast<unsigned long long>(num_data_blocks_),
                 static_cast<unsigned long long>(kIndexSizeWarnBlockCount));
  }

  finished_ = true;
  return status_;
}

}  // namespace lsm
