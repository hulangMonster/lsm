// tests/sstable_table_test.cpp —— M3.1 Table/TableBuilder 用例（A08 后 3 行、A09~A19）
//
// 契约：docs/m3-design.md §3.2~§3.7、§5.4、§10.1 的 M3-A08 后 3 行、M3-A09~M3-A19。
// 用例名与 §10.1 **逐字一致**（A08 后 3 行并入 sstable_format_test.cpp 的 Footer.RejectMatrix，
// 因为 GTest 不允许同一个 suite+test 名出现两次；其余在本文件）。
//
// 纪律：
//   * 每个失败判据都写死 kCorruption / kNotSupported / kNotFound，不放宽断言；
//   * A12/A13/A14 是逐字节翻转扫描，用 MemEnv::SetContents 做字节手术；
//   * A19 用 tests/sstable_counting_env.h 的计数 Env 证明"零 IO"，并有"范围内必须读块"的对照。
#include "test_harness.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "memenv.h"
#include "sstable/block.h"
#include "sstable/format.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "sstable_counting_env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {
namespace {

using test::MemEnv;

// ---------------------------------------------------------------------------
// 手工字节工具（不调用被测实现，避免"实现与测试同错"）
// ---------------------------------------------------------------------------
void AppendFixed32(std::string* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) dst->push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}
uint32_t Fixed32At(const std::string& s, size_t off) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(s[off + static_cast<size_t>(i)]);
  return v;
}
void SetFixed32(std::string* s, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    (*s)[off + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
  }
}
void SetFixed64(std::string* s, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    (*s)[off + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
  }
}
void FixBlockCrc(std::string* data, uint64_t offset, uint64_t size) {
  const uint32_t crc = crc32c::Value(data->data() + offset,
                                     static_cast<size_t>(size - kBlockTrailerSize));
  SetFixed32(data, static_cast<size_t>(offset + size - kBlockTrailerSize), crc);
}
// 手工写一个块（含 5B 头 + 4B CRC），与 §3.6 逐字一致。
void WriteRawBlock(std::string* file, uint8_t type, const std::string& payload, BlockHandle* h) {
  h->offset = file->size();
  std::string buf;
  AppendFixed32(&buf, static_cast<uint32_t>(payload.size()));
  buf.push_back(static_cast<char>(type));
  buf.append(payload);
  AppendFixed32(&buf, crc32c::Value(buf.data(), buf.size()));
  h->size = buf.size();
  file->append(buf);
}

// 手工读 varint32（用于定位索引项里的 handle 起始偏移）。
bool ReadVarint32At(const std::string& s, size_t* p, uint32_t* v) {
  uint32_t r = 0;
  int sh = 0;
  while (true) {
    if (*p >= s.size() || sh > 28) return false;
    const uint8_t b = static_cast<uint8_t>(s[(*p)++]);
    r |= static_cast<uint32_t>(b & 0x7fu) << sh;
    if ((b & 0x80u) == 0) break;
    sh += 7;
  }
  *v = r;
  return true;
}
// 第一个索引项的 handle(16B) 在**文件里的**起始偏移。
// 索引项在块内的编码与普通 entry 同形：shared(=0) ‖ non_shared ‖ key_delta ‖ vlen(=16) ‖ handle。
size_t FirstIndexHandleFileOffset(const std::string& file, const BlockHandle& index_handle) {
  size_t p = static_cast<size_t>(index_handle.offset) + kBlockHeaderSize;
  uint32_t shared = 0, non_shared = 0, vlen = 0;
  if (!ReadVarint32At(file, &p, &shared)) return 0;
  if (!ReadVarint32At(file, &p, &non_shared)) return 0;
  p += non_shared;
  if (!ReadVarint32At(file, &p, &vlen)) return 0;
  return p;
}
// 第一个数据块里第一条 entry 的 value 起始偏移（用于 A15 只损坏 value 字节）。
size_t FirstValueFileOffset(const std::string& file, uint64_t block_offset) {
  size_t p = static_cast<size_t>(block_offset) + kBlockHeaderSize;
  uint32_t shared = 0, non_shared = 0, vlen = 0;
  if (!ReadVarint32At(file, &p, &shared)) return 0;
  if (!ReadVarint32At(file, &p, &non_shared)) return 0;
  p += non_shared;
  if (!ReadVarint32At(file, &p, &vlen)) return 0;
  return p;
}

// ---------------------------------------------------------------------------
// 模型数据与建表助手
// ---------------------------------------------------------------------------
std::string Uk(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%04d", i);
  return std::string(buf);
}
std::string Ik(int i) {
  return test::ManualInternalKey(Slice(Uk(i)), static_cast<SequenceNumber>(i + 1), kTypeValue);
}
std::string Val(int i) { return "v" + std::to_string(i); }
std::string IkIndexed(uint64_t i) {
  return test::ManualInternalKey(Slice(test::KeyFromIndex(i)), static_cast<SequenceNumber>(i + 1),
                                 kTypeValue);
}
std::vector<std::pair<std::string, std::string>> MakeEntries(int n) {
  std::vector<std::pair<std::string, std::string>> v;
  v.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) v.emplace_back(Ik(i), Val(i));
  return v;
}

struct BuildInfo {
  uint64_t file_size = 0;
  uint64_t num_blocks = 0;
  uint64_t max_block = 0;
  uint64_t warn = 0;
};

Status BuildTable(MemEnv* env, const std::string& fname, const TableOptions& opts,
                  const std::vector<std::pair<std::string, std::string>>& kv, BuildInfo* info) {
  WritableFile* raw = nullptr;
  Status s = env->NewWritableFile(fname, &raw);
  if (!s.ok()) return s;
  std::unique_ptr<WritableFile> file(raw);
  TableBuilder builder(opts, file.get());
  for (const auto& p : kv) {
    s = builder.Add(Slice(p.first), Slice(p.second));
    if (!s.ok()) break;
  }
  if (s.ok()) s = builder.Finish();
  const Status close = file->Close();
  if (info != nullptr) {
    info->file_size = builder.FileSize();
    info->num_blocks = builder.NumDataBlocks();
    info->max_block = builder.max_block_size();
    info->warn = builder.index_size_warn_count();
  }
  if (!s.ok()) return s;
  return close;
}

Status OpenTable(Env* env, const std::string& fname, const TableOptions& opts,
                 std::shared_ptr<Table>* out) {
  return Table::Open(opts, env, fname, out);
}

void ExpectAllEntries(const std::shared_ptr<Table>& t,
                      const std::vector<std::pair<std::string, std::string>>& model) {
  std::unique_ptr<Iterator> it = t->NewIterator();
  size_t i = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++i) {
    ASSERT_LT(i, model.size());
    EXPECT_EQ(model[i].first, it->key().ToString()) << "i=" << i;
    EXPECT_EQ(model[i].second, it->value().ToString()) << "i=" << i;
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(model.size(), i);

  std::unique_ptr<Iterator> rit = t->NewIterator();
  size_t j = model.size();
  for (rit->SeekToLast(); rit->Valid(); rit->Prev()) {
    ASSERT_GT(j, 0u);
    --j;
    EXPECT_EQ(model[j].first, rit->key().ToString()) << "j=" << j;
    EXPECT_EQ(model[j].second, rit->value().ToString()) << "j=" << j;
  }
  EXPECT_TRUE(rit->status().ok());
  EXPECT_EQ(0u, j);
}

// 打开成功则 Get；Open 或 Get 任一返回 kCorruption 即算检出；若两者都成功则显式失败。
void ExpectCorruptionDetected(MemEnv* env, const std::string& fname, const TableOptions& opts,
                              const Slice& lookup) {
  std::shared_ptr<Table> t;
  const Status open = Table::Open(opts, env, fname, &t);
  bool detected = open.IsCorruption();
  if (open.ok()) {
    std::string v;
    ReadStats rs;
    const Status g = t->Get(lookup, &v, &rs);
    detected = g.IsCorruption();
    EXPECT_FALSE(g.ok()) << "静默返回错值：Open 成功且 Get 返回 " << g.ToString();
  }
  EXPECT_TRUE(detected) << "未检出：Open=" << open.ToString();
}

}  // namespace

// ===== M3-A09 =====
TEST(Table, EmptyAndBoundarySizes) {
  MemEnv env;
  const TableOptions opts;

  // 0 条：空表必须能打开；任何 key 都是 NotFound；迭代器 Invalid。
  {
    BuildInfo info;
    ASSERT_TRUE(BuildTable(&env, "empty.sst", opts, {}, &info).ok());
    EXPECT_EQ(0u, info.num_blocks);
    std::shared_ptr<Table> t;
    const Status s = OpenTable(&env, "empty.sst", opts, &t);
    ASSERT_TRUE(s.ok()) << s.ToString();
    EXPECT_EQ(0u, t->NumDataBlocks());
    EXPECT_TRUE(t->LastInternalKey().empty());
    std::string v;
    ReadStats rs;
    const Status g = t->Get(Slice(Ik(0)), &v, &rs);
    EXPECT_EQ(Status::kNotFound, g.code()) << g.ToString();
    std::unique_ptr<Iterator> it = t->NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().ok());
    it->SeekToLast();
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().ok());
  }

  // 1 条：能读回；LastInternalKey == 该条；Seek 便利入口命中。
  {
    BuildInfo info;
    ASSERT_TRUE(BuildTable(&env, "one.sst", opts, MakeEntries(1), &info).ok());
    std::shared_ptr<Table> t;
    ASSERT_TRUE(OpenTable(&env, "one.sst", opts, &t).ok());
    std::string v;
    ReadStats rs;
    ASSERT_TRUE(t->Get(Slice(Ik(0)), &v, &rs).ok());
    EXPECT_EQ(Val(0), v);
    EXPECT_EQ(Ik(0), t->LastInternalKey());
    std::unique_ptr<Iterator> it = t->Seek(Slice(Ik(0)));
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(Ik(0), it->key().ToString());
    it->Next();
    EXPECT_FALSE(it->Valid());
  }

  // 恰好落在 block_size 边界 / 1 字节之差：
  // 先用 EstimatedSizeAfter（唯一判据）求 10 条的 payload 大小，再以它 ±1 作为 block_size。
  const std::vector<std::pair<std::string, std::string>> entries = MakeEntries(10);
  size_t boundary = 0;
  {
    BlockBuilder probe(kRestartInterval);
    for (const auto& p : entries) {
      boundary = probe.EstimatedSizeAfter(Slice(p.first), Slice(p.second));
      probe.Add(Slice(p.first), Slice(p.second));
    }
    EXPECT_EQ(boundary, probe.Finish().size()) << "EstimatedSizeAfter 必须与 Finish() 同口径";
  }
  ASSERT_GT(boundary, 1u);

  {
    TableOptions exact = opts;
    exact.block_size = boundary;
    BuildInfo info;
    ASSERT_TRUE(BuildTable(&env, "exact.sst", exact, entries, &info).ok());
    EXPECT_EQ(1u, info.num_blocks) << "恰好等于 block_size 不得触发封块（判据是严格 >）";
    std::shared_ptr<Table> t;
    ASSERT_TRUE(OpenTable(&env, "exact.sst", exact, &t).ok());
    EXPECT_EQ(1u, t->NumDataBlocks());
    ExpectAllEntries(t, entries);
  }
  {
    TableOptions less = opts;
    less.block_size = boundary - 1;
    BuildInfo info;
    ASSERT_TRUE(BuildTable(&env, "less.sst", less, entries, &info).ok());
    EXPECT_EQ(2u, info.num_blocks) << "少 1 字节必须多封一个块（说明边界判据生效）";
    std::shared_ptr<Table> t;
    ASSERT_TRUE(OpenTable(&env, "less.sst", less, &t).ok());
    EXPECT_EQ(2u, t->NumDataBlocks());
    ExpectAllEntries(t, entries);
  }
}

// ===== M3-A10 =====
TEST(Table, CrossBlockLargeValue) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 4096;
  const size_t big_len = 3 * opts.block_size;
  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back(Ik(0), std::string(big_len, 'A'));
  for (int i = 1; i <= 40; ++i) entries.emplace_back(Ik(i), Val(i));

  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "big.sst", opts, entries, &info).ok());
  EXPECT_GT(info.max_block, static_cast<uint64_t>(opts.block_size))
      << "单条 entry 大于 block_size 必须被允许：max_block 必须 > block_size（目标值不是硬上限）";
  EXPECT_GE(info.num_blocks, 2u);

  std::shared_ptr<Table> t;
  ASSERT_TRUE(OpenTable(&env, "big.sst", opts, &t).ok());
  std::string v;
  ReadStats rs;
  ASSERT_TRUE(t->Get(Slice(Ik(0)), &v, &rs).ok());
  EXPECT_EQ(big_len, v.size());
  EXPECT_EQ(std::string(big_len, 'A'), v);
  ExpectAllEntries(t, entries);
}

// ===== M3-A11 =====
TEST(Table, LargeNumberOfBlocksIndexWarn) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 1;   // 每条 entry 单独成块（本格式层不做 Options 区间校验，见报告）
  const uint64_t kEntries = 65538;   // ⇒ 65538 个数据块 > 65536

  WritableFile* raw = nullptr;
  ASSERT_TRUE(env.NewWritableFile("many.sst", &raw).ok());
  std::unique_ptr<WritableFile> file(raw);
  TableBuilder builder(opts, file.get());
  for (uint64_t i = 0; i < kEntries; ++i) {
    const std::string key = IkIndexed(i);
    const Status s = builder.Add(Slice(key), Slice("v"));
    ASSERT_TRUE(s.ok()) << "i=" << i << " " << s.ToString();
  }
  const Status fin = builder.Finish();
  EXPECT_TRUE(fin.ok()) << fin.ToString();
  EXPECT_TRUE(builder.status().ok()) << "index_size_warn 必须只计数，不得阻断 Finish";
  EXPECT_GT(builder.NumDataBlocks(), 65536u);
  EXPECT_EQ(1u, builder.index_size_warn_count()) << "block_count > 65536 ⇒ 计数递增";
  EXPECT_TRUE(file->Close().ok());
}

// ===== M3-A12 =====
TEST(Table, CrcDetectsSingleByteFlipInDataBlock) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 65536;   // 单块，把翻转扫描限制在一个数据块内
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 300; ++i) entries.emplace_back(Ik(i), Val(i));
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "flip_data.sst", opts, entries, &info).ok());

  const std::string original = env.Contents("flip_data.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());
  ASSERT_GT(footer.metaindex_handle.offset, 0u);
  const size_t payload_begin = kBlockHeaderSize;
  const size_t payload_end = static_cast<size_t>(footer.metaindex_handle.offset) - kBlockTrailerSize;
  ASSERT_GT(payload_end, payload_begin + 1000u) << "扫描样本必须 >= 1000 字节";

  int flips = 0;
  for (size_t i = payload_begin; i < payload_end; ++i) {
    std::string bad = original;
    bad[i] = static_cast<char>(bad[i] ^ 0x01);
    env.SetContents("flip_data.sst", bad);

    std::shared_ptr<Table> t;
    const Status open = Table::Open(opts, &env, "flip_data.sst", &t);
    bool detected = open.IsCorruption();
    if (open.ok()) {
      std::string v;
      ReadStats rs;
      const Status g = t->Get(Slice(Ik(0)), &v, &rs);
      detected = g.IsCorruption();
      EXPECT_FALSE(g.ok()) << "静默返回错值：i=" << i;
    }
    EXPECT_TRUE(detected) << "未检出单字节翻转 i=" << i << "：Open=" << open.ToString();
    ++flips;
  }
  env.SetContents("flip_data.sst", original);
  std::fprintf(stderr, "FLIPS_DONE_A12=%d\n", flips);
  EXPECT_GE(flips, 1000);
}

// ===== M3-A13 =====
TEST(Table, CrcDetectsFlipInHeaderLengthAndTrailer) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 65536;
  std::vector<std::pair<std::string, std::string>> entries = MakeEntries(3);
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "flip_header.sst", opts, entries, &info).ok());

  const std::string original = env.Contents("flip_header.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());
  const uint64_t data_size = footer.metaindex_handle.offset;   // 单数据块 ⇒ 块尾即 metaindex 起点
  const uint32_t orig_len = Fixed32At(original, 0);
  BlockHandle data_handle;
  data_handle.offset = 0;
  data_handle.size = data_size;

  // 先在**未损坏**的文件上打开一个 Table，之后用 SetContents 做字节手术，再用它直接 ReadBlock。
  std::shared_ptr<Table> table;
  ASSERT_TRUE(Table::Open(opts, &env, "flip_header.sst", &table).ok());

  // length 字段的 4 个字节：显式变小（必须由 handle.size 自检先于 CRC 检出）+ 逐字节翻转。
  int flips = 0;
  for (int b = 0; b < 4; ++b) {
    {
      std::string bad = original;
      SetFixed32(&bad, 0, orig_len - 1);
      env.SetContents("flip_header.sst", bad);
      ReadStats rs;
      std::string payload;
      const Status s = table->ReadBlock(data_handle, kBlockTypeData, &payload, &rs);
      EXPECT_TRUE(s.IsCorruption()) << "length 变小必须 kCorruption (byte " << b << "): " << s.ToString();
      EXPECT_EQ(0u, rs.crc_checked) << "length 变小时必须在算 CRC 之前由 handle.size 不等检出";
      ++flips;
    }
    {
      std::string bad = original;
      bad[static_cast<size_t>(b)] = static_cast<char>(bad[static_cast<size_t>(b)] ^ 0x01);
      env.SetContents("flip_header.sst", bad);
      ReadStats rs;
      std::string payload;
      const Status s = table->ReadBlock(data_handle, kBlockTypeData, &payload, &rs);
      EXPECT_TRUE(s.IsCorruption()) << "翻转 length 字节 " << b << " 必须检出：" << s.ToString();
      ++flips;
    }
  }

  // length 变大：必须走到 CRC（开校验时由 CRC 检出）。
  {
    std::string bad = original;
    SetFixed32(&bad, 0, orig_len + 1);
    env.SetContents("flip_header.sst", bad);
    ReadStats rs;
    std::string payload;
    const Status s = table->ReadBlock(data_handle, kBlockTypeData, &payload, &rs);
    EXPECT_TRUE(s.IsCorruption()) << "length 变大必须 kCorruption：" << s.ToString();
    EXPECT_EQ(1u, rs.crc_checked) << "length 变大必须走到 CRC（由 CRC 检出）";
    EXPECT_EQ(1u, rs.crc_failed);
    ++flips;
  }

  // type 字节。
  {
    std::string bad = original;
    bad[kBlockHeaderSize - 1] = static_cast<char>(bad[kBlockHeaderSize - 1] ^ 0x01);
    env.SetContents("flip_header.sst", bad);
    ReadStats rs;
    std::string payload;
    const Status s = table->ReadBlock(data_handle, kBlockTypeData, &payload, &rs);
    EXPECT_TRUE(s.IsCorruption()) << "type 字节翻转必须检出：" << s.ToString();
    ++flips;
  }

  // 块尾 CRC 字节。
  {
    std::string bad = original;
    const size_t crc_off = static_cast<size_t>(data_size - kBlockTrailerSize);
    bad[crc_off] = static_cast<char>(bad[crc_off] ^ 0x01);
    env.SetContents("flip_header.sst", bad);
    ReadStats rs;
    std::string payload;
    const Status s = table->ReadBlock(data_handle, kBlockTypeData, &payload, &rs);
    EXPECT_TRUE(s.IsCorruption()) << "块尾 CRC 翻转必须检出：" << s.ToString();
    EXPECT_EQ(1u, rs.crc_failed);
    ++flips;
  }

  env.SetContents("flip_header.sst", original);
  std::fprintf(stderr, "FLIPS_DONE_A13=%d\n", flips);
}

// ===== M3-A14 =====
TEST(Table, CrcDetectsFlipInIndexAndFooter) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 65536;
  std::vector<std::pair<std::string, std::string>> entries = MakeEntries(3);
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "flip_index.sst", opts, entries, &info).ok());

  const std::string original = env.Contents("flip_index.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());

  // 索引块 payload：逐字节翻转（样本很小，全部扫）。
  const size_t idx_begin = static_cast<size_t>(footer.index_handle.offset) + kBlockHeaderSize;
  const size_t idx_end =
      static_cast<size_t>(footer.index_handle.offset + footer.index_handle.size) - kBlockTrailerSize;
  ASSERT_GT(idx_end, idx_begin);
  int flips = 0;
  for (size_t i = idx_begin; i < idx_end; ++i) {
    std::string bad = original;
    bad[i] = static_cast<char>(bad[i] ^ 0x01);
    env.SetContents("flip_index.sst", bad);
    ExpectCorruptionDetected(&env, "flip_index.sst", opts, Slice(Ik(0)));
    ++flips;
  }

  // index_handle / metaindex_handle / footer_crc 各翻转 1 字节。
  {
    std::string bad = original;
    const size_t off = original.size() - kFooterSize + 8;   // index_handle.offset 首字节
    bad[off] = static_cast<char>(bad[off] ^ 0x01);
    env.SetContents("flip_index.sst", bad);
    ExpectCorruptionDetected(&env, "flip_index.sst", opts, Slice(Ik(0)));
  }
  {
    std::string bad = original;
    const size_t off = original.size() - kFooterSize + 24;   // metaindex_handle.offset 首字节
    bad[off] = static_cast<char>(bad[off] ^ 0x01);
    env.SetContents("flip_index.sst", bad);
    ExpectCorruptionDetected(&env, "flip_index.sst", opts, Slice(Ik(0)));
  }
  {
    std::string bad = original;
    bad[original.size() - 1] = static_cast<char>(bad[original.size() - 1] ^ 0x01);
    env.SetContents("flip_index.sst", bad);
    ExpectCorruptionDetected(&env, "flip_index.sst", opts, Slice(Ik(0)));
  }

  env.SetContents("flip_index.sst", original);
  std::fprintf(stderr, "FLIPS_DONE_A14_INDEX=%d\n", flips);
  std::fprintf(stderr, "FLIPS_DONE_A14_TOTAL=%d\n", flips + 3);
}

// ===== M3-A15 =====
TEST(Table, VerifyChecksumsOffIsActuallyOff) {
  MemEnv env;
  TableOptions write_opts;
  write_opts.block_size = 65536;
  const std::string value(64, 'A');
  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back(Ik(0), value);
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "verify.sst", write_opts, entries, &info).ok());

  const std::string original = env.Contents("verify.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());

  const size_t value_off = FirstValueFileOffset(original, 0);
  ASSERT_GT(value_off, 0u);
  const size_t last_value_byte = value_off + value.size() - 1;

  TableOptions off = write_opts;
  off.verify_checksums = false;
  TableOptions on = write_opts;
  on.verify_checksums = true;

  // ② 只损坏 payload 的 value 字节 ⇒ 关掉校验后不再报错，且读到的就是被改坏的值。
  std::string corrupted_value = value;
  corrupted_value.back() = static_cast<char>(corrupted_value.back() ^ 0x01);
  std::string bad = original;
  bad[last_value_byte] = static_cast<char>(bad[last_value_byte] ^ 0x01);
  env.SetContents("verify.sst", bad);

  {
    std::shared_ptr<Table> t;
    const Status open = Table::Open(off, &env, "verify.sst", &t);
    ASSERT_TRUE(open.ok()) << "关掉校验后结构仍合法，必须能打开：" << open.ToString();
    std::string v;
    ReadStats rs;
    const Status g = t->Get(Slice(Ik(0)), &v, &rs);
    ASSERT_TRUE(g.ok()) << g.ToString();
    EXPECT_EQ(corrupted_value, v) << "关掉校验后必须读到被改坏的值（证明开关不是死代码）";
    EXPECT_EQ(0u, rs.crc_checked) << "关掉校验后不得再算 CRC";
  }
  {
    std::shared_ptr<Table> t;
    const Status open = Table::Open(on, &env, "verify.sst", &t);
    EXPECT_TRUE(open.IsCorruption()) << "开着校验必须 kCorruption：" << open.ToString();
  }

  // ① 结构校验仍然生效：畸形 length 在关掉校验时仍必须 kCorruption。
  {
    std::string bad_len = original;
    const uint32_t orig_len = Fixed32At(original, 0);
    SetFixed32(&bad_len, 0, orig_len - 1);
    env.SetContents("verify.sst", bad_len);
    std::shared_ptr<Table> t;
    const Status open = Table::Open(off, &env, "verify.sst", &t);
    EXPECT_TRUE(open.IsCorruption()) << "关掉 CRC 后 length 自检仍必须生效：" << open.ToString();
  }
  // ① 结构校验仍然生效：type 不匹配在关掉校验时仍必须 kCorruption（I25）。
  {
    std::string bad_type = original;
    bad_type[kBlockHeaderSize - 1] = static_cast<char>(bad_type[kBlockHeaderSize - 1] ^ 0x01);
    env.SetContents("verify.sst", bad_type);
    std::shared_ptr<Table> t;
    const Status open = Table::Open(off, &env, "verify.sst", &t);
    EXPECT_TRUE(open.IsCorruption()) << "关掉 CRC 后 type 匹配仍必须生效：" << open.ToString();
  }

  env.SetContents("verify.sst", original);
}

// ===== M3-A16 =====
TEST(Table, IndexHandleTypeMismatchRejected) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 65536;
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "type_mismatch.sst", opts, MakeEntries(1), &info).ok());
  const std::string original = env.Contents("type_mismatch.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());

  const size_t handle_off = FirstIndexHandleFileOffset(original, footer.index_handle);
  ASSERT_GT(handle_off, 0u);

  // 把第一个索引项的 handle 指向 metaindex 块（data ←→ metaindex），并修好索引块 CRC
  // 使 Open 能读到索引；这样失败点就只剩"期望 data、实际 metaindex"的类型不符。
  std::string bad = original;
  SetFixed64(&bad, handle_off, footer.metaindex_handle.offset);
  SetFixed64(&bad, handle_off + 8, footer.metaindex_handle.size);
  FixBlockCrc(&bad, footer.index_handle.offset, footer.index_handle.size);
  env.SetContents("type_mismatch.sst", bad);

  std::shared_ptr<Table> t;
  const Status s = Table::Open(opts, &env, "type_mismatch.sst", &t);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("type"))
      << "必须是类型不符导致的 kCorruption：" << s.ToString();

  env.SetContents("type_mismatch.sst", original);
}

// ===== M3-A17 =====
TEST(Table, HandleSizeVsBlockLengthMismatch) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 65536;
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "size_mismatch.sst", opts, MakeEntries(1), &info).ok());
  const std::string original = env.Contents("size_mismatch.sst");
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(Slice(original.data() + original.size() - kFooterSize, kFooterSize)).ok());

  const size_t handle_off = FirstIndexHandleFileOffset(original, footer.index_handle);
  ASSERT_GT(handle_off, 0u);
  const uint64_t data_size = footer.metaindex_handle.offset;   // 单数据块的总字节数

  for (int delta : {-1, 1}) {
    std::string bad = original;
    SetFixed64(&bad, handle_off + 8, static_cast<uint64_t>(static_cast<int64_t>(data_size) + delta));
    FixBlockCrc(&bad, footer.index_handle.offset, footer.index_handle.size);
    env.SetContents("size_mismatch.sst", bad);

    std::shared_ptr<Table> t;
    const Status s = Table::Open(opts, &env, "size_mismatch.sst", &t);
    EXPECT_TRUE(s.IsCorruption()) << "handle.size " << delta << " 必须 kCorruption：" << s.ToString();
    // 不发生越界读由 ASan 全量运行保证（A17 的判据之一）。
  }

  env.SetContents("size_mismatch.sst", original);
}

// ===== M3-A18 =====
TEST(Table, MetaIndexEmptyBlockParses) {
  MemEnv env;
  const TableOptions opts;

  // M3 正常写出的 metaindex：0 条、type == kBlockTypeMetaIndex。
  {
    BuildInfo info;
    ASSERT_TRUE(BuildTable(&env, "meta_empty.sst", opts, MakeEntries(2), &info).ok());
    const std::string bytes = env.Contents("meta_empty.sst");
    Footer footer;
    ASSERT_TRUE(footer.DecodeFrom(Slice(bytes.data() + bytes.size() - kFooterSize, kFooterSize)).ok());
    ASSERT_GE(footer.metaindex_handle.size, kBlockOverhead + kBlockMinPayload);
    const uint8_t type = static_cast<uint8_t>(bytes[footer.metaindex_handle.offset + kBlockHeaderSize - 1]);
    EXPECT_EQ(static_cast<uint8_t>(kBlockTypeMetaIndex), type);

    std::shared_ptr<Table> t;
    ASSERT_TRUE(Table::Open(opts, &env, "meta_empty.sst", &t).ok());
    EXPECT_EQ(0u, t->unknown_metaindex_entries()) << "M3 的 metaindex 恰为 0 条";
  }

  // 人为塞一条未知 name：必须只计数 unknown_metaindex_entries，不得报错（M5 预留）。
  {
    const std::string unknown_name = "filter.leveldb.BuiltinBloomFilter2";
    std::string file;
    BlockHandle data_handle;
    {
      BlockBuilder bb(kRestartInterval);
      bb.Add(Slice(Ik(0)), Slice(Val(0)));
      WriteRawBlock(&file, kBlockTypeData, bb.Finish().ToString(), &data_handle);
    }
    BlockHandle meta_handle;
    {
      BlockBuilder mb(kIndexRestartInterval);
      std::string handle_bytes;
      data_handle.EncodeTo(&handle_bytes);
      mb.Add(Slice(unknown_name), Slice(handle_bytes));
      WriteRawBlock(&file, kBlockTypeMetaIndex, mb.Finish().ToString(), &meta_handle);
    }
    BlockHandle index_handle;
    {
      BlockBuilder ib(kIndexRestartInterval);
      std::string handle_bytes;
      data_handle.EncodeTo(&handle_bytes);
      ib.Add(Slice(Ik(0)), Slice(handle_bytes));
      WriteRawBlock(&file, kBlockTypeIndex, ib.Finish().ToString(), &index_handle);
    }
    Footer footer;
    footer.index_handle = index_handle;
    footer.metaindex_handle = meta_handle;
    footer.EncodeTo(&file);
    env.SetContents("meta_unknown.sst", file);

    std::shared_ptr<Table> t;
    const Status s = Table::Open(opts, &env, "meta_unknown.sst", &t);
    ASSERT_TRUE(s.ok()) << "未知 metaindex 条目不得让 Open 报错：" << s.ToString();
    EXPECT_EQ(1u, t->unknown_metaindex_entries());
    ASSERT_EQ(1u, t->unknown_metaindex_names().size());
    EXPECT_EQ(unknown_name, t->unknown_metaindex_names()[0]);
    std::string v;
    ReadStats rs;
    const Status g = t->Get(Slice(Ik(0)), &v, &rs);
    ASSERT_TRUE(g.ok()) << g.ToString();
    EXPECT_EQ(Val(0), v);
  }
}

// ===== M3-A19 =====
TEST(Table, KeyRangeFilterDoesZeroIo) {
  MemEnv mem;
  TableOptions opts;
  opts.block_size = 4096;
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&mem, "range.sst", opts, MakeEntries(21), &info).ok());

  test::CountingEnv counting(&mem);
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(opts, &counting, "range.sst", &t).ok());

  // 范围外 key：kNotFound，blocks_read == 0 且 key_range_skipped == 1。
  counting.ResetCounters();
  ReadStats stats;
  std::string v;
  const Status g = t->Get(Slice(Ik(1000)), &v, &stats);
  EXPECT_EQ(Status::kNotFound, g.code()) << g.ToString();
  EXPECT_EQ(0u, stats.blocks_read);
  EXPECT_EQ(1u, stats.key_range_skipped);
  EXPECT_EQ(0u, counting.random_access_files_opened()) << "范围外 key 不得打开任何随机读文件";
  EXPECT_EQ(0u, counting.random_read_calls()) << "范围外 key 不得发生任何随机 Read";

  // 对照：范围内的 key 必须真的读块（否则"零 IO"可能只是因为根本没实现过滤）。
  counting.ResetCounters();
  ReadStats stats2;
  const Status g2 = t->Get(Slice(Ik(5)), &v, &stats2);
  ASSERT_TRUE(g2.ok()) << g2.ToString();
  EXPECT_EQ(Val(5), v);
  EXPECT_GE(stats2.blocks_read, 1u);
  // R6-e：seam 的对象由顺序文件改为随机读文件，强度不变（必须真的打开并读一次块）。
  EXPECT_GE(counting.random_access_files_opened(), 1u);
  EXPECT_GE(counting.random_read_calls(), 1u);
}

// ==== M3.3 回归（M3.2 实缺陷）：块内 Seek 必须走 InternalKeyComparator ====
// 背景：M3.2 发现并修复了 BlockReader::Seek 用逐字节比较而非 InternalKeyComparator 的缺陷
// （违反 §5.3）；它之所以逃过 M3.1 的审计，是因为当时的块级用例拿**裸 user key** 当 target
// ——裸 user key 的字节序恰好与"user 升序"一致，掩盖了 trailer 降序那半条规则。
// 本用例用 BuildLookupKey(user, snapshot)（snapshot **小于**文件内 sequence）当 target：
//   internal key 序 = user 升序 + trailer 降序 ⇒ "k"@10 < "k"@5 < "k"@2；
//   snapshot=5 必须命中 seq=2 的版本（v2）。若退回逐字节比较，"k"@2 的 trailer 字节 0x02…
//   小于 "k"@10 的 0x0a… ⇒ Seek 会命中 v10 ⇒ 本用例红。这正是同类回归的挡板。
TEST(Table, SeekWithSnapshotLookupKeyUsesInternalKeyComparator) {
  MemEnv env;
  TableOptions opts;
  opts.block_size = 4096;
  std::vector<std::pair<std::string, std::string>> kv;
  kv.emplace_back(BuildInternalKey("k", 10, kTypeValue), "v10");
  kv.emplace_back(BuildInternalKey("k", 2, kTypeValue), "v2");
  kv.emplace_back(BuildInternalKey("z", 1, kTypeValue), "vz");
  BuildInfo info;
  ASSERT_TRUE(BuildTable(&env, "snap.sst", opts, kv, &info).ok());
  std::shared_ptr<Table> t;
  ASSERT_TRUE(OpenTable(&env, "snap.sst", opts, &t).ok());

  std::string v;
  ReadStats rs;
  // 直接钉住"为什么逐字节比较是错的"：internal key 序是 user 升序 + trailer 降序，
  // 而同样的两条 key 的逐字节序恰好相反（trailer 的 LE 首字节 0x0a > 0x02）。
  {
    const InternalKeyComparator icmp(BytewiseComparator());
    const std::string k10 = BuildInternalKey("k", 10, kTypeValue);
    const std::string k2 = BuildInternalKey("k", 2, kTypeValue);
    EXPECT_LT(icmp.Compare(Slice(k10), Slice(k2)), 0) << "internal key 序：seq 大的在前";
    EXPECT_GT(Slice(k10).compare(Slice(k2)), 0)
        << "逐字节序与 internal key 序相反——这正是缺陷能逃过 M3.1 审计的原因";
  }
  ASSERT_TRUE(t->Get(Slice(BuildLookupKey("k", 5)), &v, &rs).ok())
      << "snapshot=5 必须命中 seq<=5 的版本";
  EXPECT_EQ("v2", v);
  v.clear();
  ASSERT_TRUE(t->Get(Slice(BuildLookupKey("k", 10)), &v, &rs).ok());
  EXPECT_EQ("v10", v);
  v.clear();
  EXPECT_TRUE(t->Get(Slice(BuildLookupKey("k", 1)), &v, &rs).IsNotFound())
      << "snapshot=1 时两个版本都不可见";
}

}  // namespace lsm
