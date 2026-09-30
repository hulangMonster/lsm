// src/sstable/table.cpp —— 只读 SSTable 的打开、块读取、Get 与迭代（M3.1；M5.1 追加 filter）
//
// 契约：docs/m3-design.md §3.3/§3.4/§3.5/§3.6/§3.7、§5.4 的块读取 5 步与 Get 路径。
//       docs/m5-design.md §3.4（metaindex 注册与版本兼容）、§3.6（ReadStats 追加）、
//       §3.7（读路径接入与降级规则；②.5 是**唯一**允许的否定判断点）、E3（filter CRC 始终校验）。
// 用例编号（§10.1）：M3-A08 后 3 行、M3-A09~A19；M5-A04/A05/A06/A07/A08/A09。
//
// 关键判据（写死，评审逐条核对）：
//   * 读多少字节**只**看 handle.size；块内 length 只用于"必须等于 handle.size-9"的自检
//     （小于时在算 CRC 之前检出；大于时若开了校验由 CRC 检出，关掉校验后由长度上界自检检出）。
//   * type 必须等于调用方期望的类型，否则 kCorruption（I25 / M3-A16）。
//   * 关掉 verify_checksums 只跳过 payload CRC；length/length 上界/type/结构校验**永不跳过**（A15）。
//   * §3.5 的越界/顺序/紧贴 footer 约束在 Open 时逐条检查（M3-A08 后 3 行）。
//   * **M5.1**：filter 的任何结构/CRC 问题都**不**让 Open 失败；只把 filter_state 置 kCorrupt 降级（I49/§3.7）。
//   * **M5.1**：filter 的「肯定」结论**不得**跳过 step ③~⑥ 的任何读取或校验（I49）。
//   * **M5.1**：filter 块 CRC **始终**校验（E3），不受 Options::verify_checksums 影响。
#include "sstable/table.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

namespace {

// 按 (offset, n) 读文件。M3.2 / R6-e：改用 Env::NewRandomAccessFile +
// RandomAccessFile::Read（pread 语义），不再「打开顺序文件再 Skip」。
// 说明：本函数每次调用打开一个只读句柄；Table 本身不持有句柄，这保证 M3-A19 的
// 「范围内 key 必须真的打开随机读文件、范围外一次都不打开」计数断言仍然可观测。
Status ReadExactFile(Env* env, const std::string& filename, uint64_t offset, size_t n,
                     std::string* out) {
  out->clear();
  RandomAccessFile* raw = nullptr;
  Status s = env->NewRandomAccessFile(filename, &raw);
  if (!s.ok()) return s;
  std::unique_ptr<RandomAccessFile> file(raw);
  out->resize(n);
  size_t got = 0;
  while (got < n) {
    Slice piece;
    s = file->Read(offset + got, n - got, &piece, &(*out)[got]);
    if (!s.ok()) return s;
    if (piece.size() == 0) break;   // EOF
    got += piece.size();
  }
  if (got != n) {
    out->resize(got);
    return Status::Corruption("Table::ReadAt", "short read (file truncated?)");
  }
  return Status::OK();
}

// M5.1：metaindex 里 filter 条目的名字比对（§3.4/protocol §12.1）。
bool IsBuiltinBloomFilterName(const Slice& name) {
  const size_t n = std::strlen(kBuiltinBloomFilterName);
  return name.size() == n && std::memcmp(name.data(), kBuiltinBloomFilterName, n) == 0;
}

}  // namespace

Table::~Table() = default;

Status Table::Open(const TableOptions& options, Env* env, const std::string& filename,
                   std::shared_ptr<Table>* out, const std::string* known_smallest,
                   const std::string* known_largest, ReadStats* open_stats) {
  if (env == nullptr || out == nullptr) {
    return Status::InvalidArgument("Table::Open", "null env or null out");
  }
  uint64_t file_size = 0;
  Status s = env->GetFileSize(filename, &file_size);
  if (!s.ok()) return s;
  if (file_size < kFooterSize) {
    return Status::Corruption("Table::Open", "file too small to be an SSTable");
  }

  std::string footer_bytes;
  s = ReadExactFile(env, filename, file_size - kFooterSize, kFooterSize, &footer_bytes);
  if (!s.ok()) return s;
  Footer footer;
  s = footer.DecodeFrom(Slice(footer_bytes));
  if (!s.ok()) return s;

  // §3.5/§3.7：footer 区域被排除在块区域之外；两个 handle 都必须落在块区域内，
  // 且 metaindex 必须整体在 index 之前、index 必须紧贴 footer。
  const uint64_t block_region_end = file_size - kFooterSize;
  const auto in_bounds = [block_region_end](const BlockHandle& h) {
    return h.offset <= block_region_end && h.size <= block_region_end - h.offset;
  };
  if (!in_bounds(footer.index_handle) || !in_bounds(footer.metaindex_handle)) {
    return Status::Corruption("Table::Open", "block handle out of file bounds");
  }
  if (footer.metaindex_handle.offset > footer.index_handle.offset ||
      footer.metaindex_handle.size >
          footer.index_handle.offset - footer.metaindex_handle.offset) {
    return Status::Corruption("Table::Open", "metaindex.offset+size > index.offset");
  }
  if (footer.index_handle.size != block_region_end - footer.index_handle.offset) {
    return Status::Corruption("Table::Open", "index handle does not abut footer");
  }

  std::shared_ptr<Table> t(new Table());
  t->options_ = options;
  t->env_ = env;
  t->filename_ = filename;
  t->file_size_ = file_size;
  t->footer_ = footer;

  // ② metaindex（§3.4）：M3 为空块；未知条目只记录并计数，**不报错**。
  //    M5.1：已知 name（filter.leveldb.BuiltinBloomFilter2）在这里被**识别**（不再计 unknown）。
  std::string meta_payload;
  s = t->ReadBlock(footer.metaindex_handle, kBlockTypeMetaIndex, &meta_payload, nullptr);
  if (!s.ok()) return s;
  s = t->ParseMetaIndexBlock(Slice(meta_payload));
  if (!s.ok()) return s;

  // ②.5 M5.1：读 filter 块并构造只读 reader。**任何失败都只降级**（M5:35/I49），不返回错误。
  t->LoadFilterBlock(open_stats);

  // ③ index（§3.3）：索引已在内存 ⇒ 后续 Get 的定位零 IO。
  std::string index_payload;
  s = t->ReadBlock(footer.index_handle, kBlockTypeIndex, &index_payload, nullptr);
  if (!s.ok()) return s;
  s = t->ParseIndexBlock(Slice(index_payload));
  if (!s.ok()) return s;

  // ④ key range：M3.2 的 Version 已从 TableBuilder 带了 smallest/largest，调用方给出时
  //    直接采用（不预读首块）；否则退到 M3.1 的「预读第一个数据块取 smallest_」。
  if (known_smallest != nullptr && known_largest != nullptr) {
    t->smallest_ = *known_smallest;
    t->largest_ = *known_largest;
  } else {
    if (!t->index_entries_.empty()) {
      std::string first_payload;
      s = t->ReadBlock(t->index_entries_.front().handle, kBlockTypeData, &first_payload, nullptr);
      if (!s.ok()) return s;
      std::unique_ptr<BlockReader> reader;
      s = BlockReader::Open(Slice(first_payload), &reader, &t->icmp_);
      if (!s.ok()) return s;
      s = reader->SeekToFirst();
      if (!s.ok()) return s;
      if (!reader->Valid()) {
        return Status::Corruption("Table::Open", "first data block has no entries");
      }
      t->smallest_ = reader->key().ToString();
    }

    // ⑤ 最大 internal key = 最后一个索引项的 key（索引项 key 是该块最后一条 key，§3.3）。
    t->largest_ = t->index_entries_.empty() ? std::string() : t->index_entries_.back().key;
  }

  *out = t;
  return Status::OK();
}

void Table::LoadFilterBlock(ReadStats* open_stats) {
  const auto mark_corrupt = [&]() {
    filter_state_ = FilterState::kCorrupt;
    filter_.reset();
    filter_payload_bytes_ = 0;
    if (open_stats != nullptr) ++open_stats->filter_corrupt;
  };

  // metaindex 里同名重复注册 ⇒ 已在 ParseMetaIndexBlock 里记为 kCorrupt。
  if (filter_state_ == FilterState::kCorrupt) {
    mark_corrupt();
    return;
  }
  if (!has_filter_handle_) {
    filter_state_ = FilterState::kAbsent;
    return;
  }
  filter_state_ = FilterState::kAbsent;   // 先假定失败；成功路径会改成 kOk

  // §3.1 的 M5 追加布局校验：filter.offset + filter.size <= metaindex.offset。
  if (filter_handle_.offset > footer_.metaindex_handle.offset ||
      filter_handle_.size > footer_.metaindex_handle.offset - filter_handle_.offset) {
    mark_corrupt();
    return;
  }

  // E3：filter 块 CRC **始终**校验（不受 Options::verify_checksums 影响）。
  std::string payload;
  if (!ReadBlockImpl(filter_handle_, kBlockTypeFilter, &payload, nullptr, true).ok()) {
    mark_corrupt();
    return;
  }

  std::unique_ptr<FilterBlockReader> reader(new FilterBlockReader(Slice(payload)));
  if (!reader->valid()) {
    mark_corrupt();
    return;
  }

  filter_payload_bytes_ = payload.size();
  filter_ = std::move(reader);
  filter_state_ = FilterState::kOk;
  if (open_stats != nullptr) {
    ++open_stats->filter_blocks_read;
    open_stats->filter_bytes_read += filter_payload_bytes_;
  }
}

Status Table::ParseMetaIndexBlock(const Slice& payload) {
  std::unique_ptr<BlockReader> reader;
  Status s = BlockReader::Open(payload, &reader, &icmp_);
  if (!s.ok()) return s;
  for (s = reader->SeekToFirst(); reader->Valid(); s = reader->Next()) {
    if (!s.ok()) return s;
    const Slice name = reader->key();
    const Slice handle_bytes = reader->value();
    if (handle_bytes.size() != kBlockHandleEncodedLength) {
      return Status::Corruption("Table::ParseMetaIndexBlock", "meta handle must be 16 bytes");
    }
    BlockHandle handle;
    size_t consumed = 0;
    const Status hs = handle.DecodeFrom(handle_bytes, &consumed);
    if (!hs.ok()) return hs;

    // M5.1（§3.4）：唯一已知的 name 是内置 Bloom filter；**最多一条**，重复 ⇒ filter 降级为 kCorrupt。
    if (IsBuiltinBloomFilterName(name)) {
      if (filter_handle_seen_) {
        filter_state_ = FilterState::kCorrupt;
        has_filter_handle_ = false;
      } else {
        filter_handle_seen_ = true;
        filter_handle_ = handle;
        has_filter_handle_ = true;
      }
      continue;
    }

    // M3 没有任何已知名字；M5 的其它名字对 M3 reader 也是"未知"。§3.4 要求只计数不报错。
    ++unknown_metaindex_entries_;
    unknown_metaindex_names_.push_back(name.ToString());
  }
  return s;
}

Status Table::ParseIndexBlock(const Slice& payload) {
  std::unique_ptr<BlockReader> reader;
  Status s = BlockReader::Open(payload, &reader, &icmp_);
  if (!s.ok()) return s;
  index_entries_.clear();
  for (s = reader->SeekToFirst(); reader->Valid(); s = reader->Next()) {
    if (!s.ok()) return s;
    const Slice key = reader->key();
    if (key.size() < kInternalKeyMinSize) {
      return Status::Corruption("Table::ParseIndexBlock", "index key shorter than 8 bytes");
    }
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(key, &user_key, &seq, &type)) {
      return Status::Corruption("Table::ParseIndexBlock", "index key is not a valid internal key");
    }
    const Slice value = reader->value();
    if (value.size() != kBlockHandleEncodedLength) {
      return Status::Corruption("Table::ParseIndexBlock", "index handle must be 16 bytes");
    }
    IndexEntry entry;
    entry.key = key.ToString();
    size_t consumed = 0;
    const Status hs = entry.handle.DecodeFrom(value, &consumed);
    if (!hs.ok()) return hs;
    index_entries_.push_back(std::move(entry));
  }
  return s;
}

Status Table::ReadAt(uint64_t offset, size_t n, std::string* out) const {
  return ReadExactFile(env_, filename_, offset, n, out);
}

Status Table::ReadBlock(const BlockHandle& handle, BlockType expected, std::string* payload,
                        ReadStats* stats) const {
  return ReadBlockImpl(handle, expected, payload, stats, options_.verify_checksums);
}

Status Table::ReadBlockImpl(const BlockHandle& handle, BlockType expected, std::string* payload,
                            ReadStats* stats, bool verify_crc) const {
  if (payload == nullptr) return Status::InvalidArgument("Table::ReadBlock", "null payload");
  payload->clear();
  if (handle.size < kBlockOverhead + kBlockMinPayload) {
    return Status::Corruption("Table::ReadBlock", "handle.size smaller than the minimum block");
  }
  if (handle.offset > file_size_ || handle.size > file_size_ - handle.offset) {
    return Status::Corruption("Table::ReadBlock", "block handle out of file bounds");
  }
  if (handle.offset + handle.size > file_size_ - kFooterSize) {
    return Status::Corruption("Table::ReadBlock", "block handle overlaps the footer region");
  }

  std::string buf;
  Status s = ReadAt(handle.offset, static_cast<size_t>(handle.size), &buf);
  if (!s.ok()) return s;
  if (stats != nullptr) {
    ++stats->blocks_read;
    stats->bytes_read += handle.size;
    // M5.1 修复（登记于报告）：`ReadStats::data_blocks_read` 在 M3/M4 里**从未被递增**
    // （`docs/amplification.md` 的 `read_data_blocks_read=0` 是直接证据），而 M5 的 §3.8 门禁
    // 明确定义在 `data_blocks_read` 上。这里在真正读到数据块时 +1，使该口径与 §3.6/D3:553 一致。
    // 只影响诊断计数，不改变任何读结果或块校验语义。
    if (expected == kBlockTypeData) ++stats->data_blocks_read;
  }

  const uint32_t length = DecodeFixed32(buf.data());
  const uint8_t type = static_cast<uint8_t>(buf[kBlockHeaderSize - 1]);
  const uint64_t expected_len = handle.size - kBlockOverhead;

  // §3.6：length 只用于"必须等于 handle.size-9"的自检。变小时在算 CRC 之前就检出
  // （A13 的第一条断言：不依赖 CRC）；变大时留给 CRC（若开启）与末尾的长度上界自检。
  if (static_cast<uint64_t>(length) < expected_len) {
    return Status::Corruption("Table::ReadBlock", "block length < handle.size (length self-check)");
  }
  if (type != static_cast<uint8_t>(expected)) {
    return Status::Corruption("Table::ReadBlock", "block type mismatch");
  }
  if (verify_crc) {
    if (stats != nullptr) ++stats->crc_checked;
    const uint32_t stored = DecodeFixed32(buf.data() + buf.size() - kBlockTrailerSize);
    const uint32_t actual = crc32c::Value(buf.data(), buf.size() - kBlockTrailerSize);
    if (stored != actual) {
      if (stats != nullptr) ++stats->crc_failed;
      return Status::Corruption("Table::ReadBlock", "block crc mismatch");
    }
  }
  if (static_cast<uint64_t>(length) > expected_len) {
    return Status::Corruption("Table::ReadBlock", "block length > handle.size (length self-check)");
  }

  *payload = buf.substr(kBlockHeaderSize, static_cast<size_t>(length));
  return Status::OK();
}

bool Table::KeyMayMatch(uint64_t block_offset, const Slice& user_key, ReadStats* stats) const {
  if (filter_state_ != FilterState::kOk || filter_ == nullptr) {
    // §3.7 硬规则 3：kAbsent / kCorrupt ⇒ 一律按「可能存在」处理，并计数（降级不静默）。
    if (stats != nullptr) ++stats->filter_unavailable;
    return true;
  }
  const bool maybe = filter_->KeyMayMatch(block_offset, user_key);
  if (stats != nullptr) {
    ++stats->filter_checked;
    if (maybe) {
      ++stats->filter_positive;
    } else {
      ++stats->filter_negative;
      ++stats->data_blocks_skipped_by_filter;
    }
  }
  return maybe;
}

Status Table::Get(const Slice& lookup_key, std::string* value, ReadStats* stats) const {
  TableGetResult r = TableGetResult::kNotFound;
  const Status s = GetEntry(lookup_key, value, &r, stats);
  if (!s.ok()) return s;
  if (r == TableGetResult::kFound) return Status::OK();
  if (r == TableGetResult::kDeleted) return Status::NotFound("Table::Get", "tombstone");
  return Status::NotFound("Table::Get", "not found");
}

Status Table::GetEntry(const Slice& lookup_key, std::string* value, TableGetResult* result,
                       ReadStats* stats) const {
  if (value == nullptr || result == nullptr) {
    return Status::InvalidArgument("Table::GetEntry", "null value or result");
  }
  *result = TableGetResult::kNotFound;
  Slice lookup_user;
  SequenceNumber lookup_seq = 0;
  ValueType lookup_type = kTypeValue;
  if (!ParseInternalKey(lookup_key, &lookup_user, &lookup_seq, &lookup_type)) {
    return Status::InvalidArgument("Table::GetEntry", "malformed lookup key");
  }

  // ① key range 过滤：**零 IO**（key range 来自 Open 时载入的 first/last internal key）。
  if (index_entries_.empty()) {
    if (stats != nullptr) ++stats->key_range_skipped;
    return Status::OK();
  }
  Slice min_user, max_user;
  SequenceNumber tmp_seq = 0;
  ValueType tmp_type = kTypeValue;
  if (!ParseInternalKey(Slice(smallest_), &min_user, &tmp_seq, &tmp_type) ||
      !ParseInternalKey(Slice(largest_), &max_user, &tmp_seq, &tmp_type)) {
    return Status::Corruption("Table::GetEntry", "cached key range is not a valid internal key");
  }
  const Comparator* user_cmp = icmp_.user_comparator();
  if (user_cmp->Compare(lookup_user, min_user) < 0 || user_cmp->Compare(lookup_user, max_user) > 0) {
    if (stats != nullptr) ++stats->key_range_skipped;
    return Status::OK();
  }

  // ② 内存索引上找第一个 key >= lookup_key 的索引项（§3.3 的 >= 语义）。
  const auto it = std::lower_bound(
      index_entries_.begin(), index_entries_.end(), lookup_key,
      [this](const IndexEntry& e, const Slice& target) {
        return icmp_.Compare(Slice(e.key), target) < 0;
      });
  if (it == index_entries_.end()) return Status::OK();

  // ②.5 M5.1（§3.7）：filter 的**唯一**允许用途 —— 对「将要读的那个数据块」做否定判断。
  //   false ⇒ 本文件一定没有该 user key ⇒ 只允许映射为 kNotFound（上层继续查更旧文件）。
  //   true  ⇒ **必须**执行原有 step ③~⑥（I49：禁止用「肯定」跳过读取或校验）。
  if (!KeyMayMatch(it->handle.offset, lookup_user, stats)) {
    *result = TableGetResult::kNotFound;
    return Status::OK();
  }

  // ③ 一次数据块读（statistics 的唯一数据块来源）。
  std::string payload;
  Status s = ReadBlock(it->handle, kBlockTypeData, &payload, stats);
  if (!s.ok()) return s;

  // ④ 块内 Seek + ⑤ user key 相等校验（走 user_comparator，不得退化成逐字节比较）。
  std::unique_ptr<BlockReader> reader;
  s = BlockReader::Open(Slice(payload), &reader, &icmp_);
  if (!s.ok()) return s;
  s = reader->Seek(lookup_key);
  if (!s.ok()) return s;
  if (!reader->Valid()) return Status::OK();

  Slice found_user;
  SequenceNumber found_seq = 0;
  ValueType found_type = kTypeValue;
  if (!ParseInternalKey(reader->key(), &found_user, &found_seq, &found_type)) {
    return Status::Corruption("Table::GetEntry", "data block key is not a valid internal key");
  }
  if (user_cmp->Compare(found_user, lookup_user) != 0) return Status::OK();
  if (found_type == kTypeDeletion) {
    *result = TableGetResult::kDeleted;
    return Status::OK();
  }
  *value = reader->value().ToString();
  *result = TableGetResult::kFound;
  return Status::OK();
}

// ============================ 迭代器（内部 key 视图） ============================
//
// M5.1 硬规则（§3.7 第 4 条）：**迭代器路径绝不使用 filter** —— 全量迭代必须读所有数据块。
// 下面的 LoadBlock 直接走 ReadBlock(kBlockTypeData)，不经 KeyMayMatch。

class Table::TableIterator : public Iterator {
 public:
  TableIterator(std::shared_ptr<const Table> table, ReadStats* stats)
      : table_(std::move(table)), stats_(stats) {}

  bool Valid() const override { return valid_; }

  Slice key() const override { return valid_ && reader_ != nullptr ? reader_->key() : Slice(); }
  Slice value() const override {
    return valid_ && reader_ != nullptr ? reader_->value() : Slice();
  }
  Status status() const override { return status_; }

  void SeekToFirst() override {
    status_ = Status::OK();
    valid_ = false;
    if (table_->index_entries_.empty()) return;
    if (!LoadBlock(0)) return;
    status_ = reader_->SeekToFirst();
    valid_ = status_.ok() && reader_->Valid();
  }

  void SeekToLast() override {
    status_ = Status::OK();
    valid_ = false;
    if (table_->index_entries_.empty()) return;
    if (!LoadBlock(table_->index_entries_.size() - 1)) return;
    status_ = reader_->SeekToLast();
    valid_ = status_.ok() && reader_->Valid();
  }

  void Seek(const Slice& target) override {
    status_ = Status::OK();
    valid_ = false;
    const size_t n = table_->index_entries_.size();
    if (n == 0) return;
    size_t i = 0;
    while (i < n && table_->icmp_.Compare(Slice(table_->index_entries_[i].key), target) < 0) ++i;
    if (i == n) return;   // 目标在最后一个数据块之后：越过末尾
    if (!LoadBlock(i)) return;
    status_ = reader_->Seek(target);
    if (!status_.ok() || reader_->Valid()) {
      valid_ = status_.ok() && reader_->Valid();
      return;
    }
    // 索引项的 key >= target（它是该块最后一条 key），正常情况下不会走到这里；
    // 防御性地继续向后扫描，避免任何"索引语义走样"变成静默丢 key。
    for (size_t j = i + 1; j < n; ++j) {
      if (!LoadBlock(j)) return;
      status_ = reader_->SeekToFirst();
      if (!status_.ok()) return;
      if (reader_->Valid()) {
        valid_ = true;
        return;
      }
    }
  }

  void Next() override {
    if (!valid_ || reader_ == nullptr) return;
    status_ = reader_->Next();
    if (!status_.ok()) {
      valid_ = false;
      return;
    }
    if (reader_->Valid()) {
      valid_ = true;
      return;
    }
    for (size_t j = index_pos_ + 1; j < table_->index_entries_.size(); ++j) {
      if (!LoadBlock(j)) return;
      status_ = reader_->SeekToFirst();
      if (!status_.ok()) return;
      if (reader_->Valid()) {
        valid_ = true;
        return;
      }
    }
    valid_ = false;
  }

  void Prev() override {
    if (!valid_ || reader_ == nullptr) return;
    status_ = reader_->Prev();
    if (!status_.ok()) {
      valid_ = false;
      return;
    }
    if (reader_->Valid()) {
      valid_ = true;
      return;
    }
    size_t j = index_pos_;
    while (j > 0) {
      --j;
      if (!LoadBlock(j)) return;
      status_ = reader_->SeekToLast();
      if (!status_.ok()) return;
      if (reader_->Valid()) {
        valid_ = true;
        return;
      }
    }
    valid_ = false;
  }

 private:
  bool LoadBlock(size_t i) {
    payload_.clear();
    reader_.reset();
    valid_ = false;
    if (i >= table_->index_entries_.size()) return false;
    Status s = table_->ReadBlock(table_->index_entries_[i].handle, kBlockTypeData, &payload_, stats_);
    if (!s.ok()) {
      status_ = s;
      return false;
    }
    s = BlockReader::Open(Slice(payload_), &reader_, &table_->icmp_);
    if (!s.ok()) {
      status_ = s;
      return false;
    }
    index_pos_ = i;
    return true;
  }

  std::shared_ptr<const Table> table_;
  ReadStats* stats_ = nullptr;
  size_t index_pos_ = 0;
  std::string payload_;                 // 当前块 payload 的所有者（reader_ 引用它）
  std::unique_ptr<BlockReader> reader_;
  bool valid_ = false;
  Status status_;
};

std::unique_ptr<Iterator> Table::NewIterator(ReadStats* stats) const {
  return std::unique_ptr<Iterator>(new TableIterator(shared_from_this(), stats));
}

std::unique_ptr<Iterator> Table::Seek(const Slice& target, ReadStats* stats) const {
  std::unique_ptr<Iterator> it = NewIterator(stats);
  it->Seek(target);
  return it;
}

}  // namespace lsm
