// src/sstable/table_builder.cpp —— SSTable 顺序写入（M3.1）
//
// 契约：docs/m3-design.md §3.2（block_size 是目标值）、§3.3（索引项/索引 restart=1）、
//       §3.4（空 metaindex 必须注册 handle）、§3.5（写出顺序）、§3.6（CRC 覆盖 length‖type‖payload）、
//       §5.4（唯一判据 EstimatedSizeAfter）、§3.3 的 index_size_warn 登记阈值。
#include "sstable/table_builder.h"

#include <cstdio>

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
      index_builder_(kIndexRestartInterval) {}

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
  if (ParseInternalKey(key, &user_key, &seq, &type) && seq > max_sequence_) {
    max_sequence_ = seq;
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

  // ② metaindex：M3 写空块（payload 恰 8B），但必须在 footer 注册 handle（§3.4）。
  {
    BlockBuilder meta_builder(kIndexRestartInterval);
    const Slice meta_payload = meta_builder.Finish();
    BlockHandle meta_handle;
    s = WriteBlock(kBlockTypeMetaIndex, meta_payload, &meta_handle);
    if (!s.ok()) return s;
    footer_.metaindex_handle = meta_handle;
  }

  // ③ 索引块（restart_interval = 1，§3.3）
  {
    const Slice index_payload = index_builder_.Finish();
    BlockHandle index_handle;
    s = WriteBlock(kBlockTypeIndex, index_payload, &index_handle);
    if (!s.ok()) return s;
    footer_.index_handle = index_handle;
  }

  // ④ footer（44B，文件末尾）
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
