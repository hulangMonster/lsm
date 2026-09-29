// tests/sstable_format_test.cpp —— M3.1 格式层用例（`#2` 阶段：RED）
//
// 契约：docs/m3-design.md §3.2（数据块）、§3.3（索引块）、§3.6（块头/CRC）、§3.7（footer 44B）；
// 用例编号与设计 §10.1 **逐字对应**（A01~A08），便于 `#4` 评审按编号核对。
//
// 本片覆盖**不需要 Env / 文件**的部分与 A08 的 Table 打开路径：
//   Block：A01~A06      Footer：A07 + A08 的全部（前 4 行在 Footer::DecodeFrom，
//   后 3 行 handle 越界 / metaindex 与 index 顺序 / index 未紧贴 footer 在 Table::Open）。
//
// `#2` 的 RED 形态：声明齐备、实现未写 ⇒ 用例可编译、**链接失败**（与 M1 `#2` 的骨架同形，
// 先例见 docs/m1-tdd-red.log）。`#3`（M3.1）补上实现后本文件转绿。
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
#include "util/crc32c.h"

namespace lsm {
namespace {

using test::AppendVarint32Manual;

// ---------------------------------------------------------------------------
// 手工拼字节的独立参照：**不调用被测实现**（与 test_harness 的 Manual* 同一纪律）。
// 这样实现把 shared/non_shared 编错时，测试能抓到，而不是"实现与测试同错"。
// ---------------------------------------------------------------------------
void AppendFixed32(std::string* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) dst->push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}

uint32_t Fixed32At(const std::string& s, size_t off) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) {
    v = (v << 8) | static_cast<uint8_t>(s[off + static_cast<size_t>(i)]);
  }
  return v;
}

void SetFixed32(std::string* s, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    (*s)[off + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
  }
}

// entry := varint32(shared) ‖ varint32(non_shared) ‖ delta ‖ varint32(vlen) ‖ value
void AppendRawEntry(std::string* dst, uint32_t shared, const Slice& delta, const Slice& value) {
  AppendVarint32Manual(dst, shared);
  AppendVarint32Manual(dst, static_cast<uint32_t>(delta.size()));
  dst->append(delta.data(), delta.size());
  AppendVarint32Manual(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

std::string MakeRawPayload(const std::vector<std::string>& entries,
                           const std::vector<uint32_t>& restarts) {
  std::string p;
  for (const std::string& e : entries) p.append(e);
  for (uint32_t r : restarts) AppendFixed32(&p, r);
  AppendFixed32(&p, static_cast<uint32_t>(restarts.size()));
  return p;
}

// restart 数组的起始偏移 = 末尾 4*(count+1) 字节之外
size_t RestartArrayOffset(const std::string& payload) {
  const uint32_t n = Fixed32At(payload, payload.size() - 4);
  return payload.size() - 4 - 4 * static_cast<size_t>(n);
}

struct RawHeader {
  uint32_t shared = 0;
  uint32_t non_shared = 0;
  size_t key_off = 0;
  uint32_t value_len = 0;
  size_t next = 0;
};

// 手工解一条 entry 的头（字节级断言用，例如"组首条 shared == 0 且 key 完整"）。
bool ParseRawHeader(const std::string& p, size_t off, RawHeader* h) {
  size_t i = off;
  const auto read_varint = [&](uint32_t* v) -> bool {
    uint32_t r = 0;
    int sh = 0;
    while (true) {
      if (i >= p.size() || sh > 28) return false;
      const uint8_t b = static_cast<uint8_t>(p[i++]);
      r |= static_cast<uint32_t>(b & 0x7fu) << sh;
      if ((b & 0x80u) == 0) break;
      sh += 7;
    }
    *v = r;
    return true;
  };
  if (!read_varint(&h->shared)) return false;
  if (!read_varint(&h->non_shared)) return false;
  h->key_off = i;
  if (i + h->non_shared > p.size()) return false;
  i += h->non_shared;
  if (!read_varint(&h->value_len)) return false;
  if (i + h->value_len > p.size()) return false;
  h->next = i + h->value_len;
  return true;
}

// 用户 key（定宽递增，字面可读）；internal key 一律走 test_harness 的手工参照。
std::string Uk(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%04d", i);
  return std::string(buf);
}

std::string Ik(int i) {
  return test::ManualInternalKey(Slice(Uk(i)), static_cast<SequenceNumber>(i + 1), kTypeValue);
}

std::string Val(int i) { return "v" + std::to_string(i); }

// 从 reader 当前 key 解出 user key（用 test_harness 的手工参照，不依赖被测解析）
std::string UserKeyOf(const BlockReader& r) {
  Slice user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  EXPECT_TRUE(ParseInternalKey(r.key(), &user_key, &seq, &type))
      << "块内 key 必须是合法 internal key（size=" << r.key().size() << "）";
  return user_key.ToString();
}

void BuildBlock(int n, BlockBuilder* b, std::vector<std::pair<std::string, std::string>>* model) {
  for (int i = 0; i < n; ++i) {
    b->Add(Slice(Ik(i)), Slice(Val(i)));
    model->emplace_back(Ik(i), Val(i));
  }
}

}  // namespace

// ===== M3-A01 =====
TEST(Block, RoundTripEmptySingleMany) {
  // 空块：payload 恰为 8 字节，且形如 restart_offset[0]=0 ‖ restart_count=1（§3.2）
  {
    BlockBuilder b(kRestartInterval);
    const Slice payload = b.Finish();
    EXPECT_EQ(kBlockMinPayload, payload.size()) << "空块 payload 必须恰为 8 字节";
    const std::string p = payload.ToString();
    ASSERT_EQ(8u, p.size());
    EXPECT_EQ(0u, Fixed32At(p, 0)) << "restart_offset[0] 必须为 0";
    EXPECT_EQ(1u, Fixed32At(p, 4)) << "空块的 restart_count 必须为 1（数组恒为 [0]）";
    EXPECT_EQ(payload.size(), b.CurrentSizeEstimate());
    EXPECT_TRUE(b.empty());
    std::unique_ptr<BlockReader> r;
    ASSERT_TRUE(BlockReader::Open(payload, &r).ok());
    ASSERT_TRUE(r->SeekToFirst().ok());
    EXPECT_FALSE(r->Valid()) << "空块不得产出任何 entry";
    EXPECT_TRUE(r->status().ok());
    EXPECT_TRUE(ValidatePayload(payload).ok()) << "空块是合法块";
  }

  // 单条 / 多条往返：key/value 逐字节相等，且 CurrentSizeEstimate() == Finish().size()
  for (const int n : {1, 17, 200}) {
    BlockBuilder b(kRestartInterval);
    std::vector<std::pair<std::string, std::string>> model;
    BuildBlock(n, &b, &model);
    EXPECT_EQ(b.Finish().size(), b.CurrentSizeEstimate()) << "n=" << n;
    EXPECT_EQ(b.Finish().ToString(), b.Finish().ToString()) << "n=" << n << "：Finish() 必须幂等";

    std::unique_ptr<BlockReader> r;
    ASSERT_TRUE(BlockReader::Open(b.Finish(), &r).ok()) << "n=" << n;
    size_t i = 0;
    for (r->SeekToFirst(); r->Valid(); r->Next(), ++i) {
      ASSERT_LT(i, model.size()) << "n=" << n << "：迭代器多出第 " << i << " 条";
      EXPECT_EQ(model[i].first, r->key().ToString()) << "n=" << n << " i=" << i;
      EXPECT_EQ(model[i].second, r->value().ToString()) << "n=" << n << " i=" << i;
    }
    EXPECT_TRUE(r->status().ok()) << "n=" << n << "：" << r->status().ToString();
    EXPECT_EQ(model.size(), i) << "n=" << n << "：迭代器少输出";
  }
}

// ===== M3-A02 =====
TEST(Block, PrefixCompressionAndRestartGroups) {
  const int kEntries = 40;   // 16 条一组 ⇒ restart 点 0 / 16 / 32，共 3 个
  BlockBuilder b(kRestartInterval);
  std::vector<std::pair<std::string, std::string>> model;
  BuildBlock(kEntries, &b, &model);
  const Slice fin = b.Finish();
  const std::string p = fin.ToString();

  EXPECT_EQ(3u, b.NumRestarts()) << "40 条 / 每 16 条一个 restart 点 ⇒ 恰 3 个";
  const size_t ra = RestartArrayOffset(p);
  ASSERT_GE(ra, 1u);
  EXPECT_EQ(0u, Fixed32At(p, ra)) << "restart_offset[0] 必须为 0";
  for (size_t g = 0; g < 3; ++g) {
    const uint32_t off = Fixed32At(p, ra + 4 * g);
    RawHeader h;
    ASSERT_TRUE(ParseRawHeader(p, off, &h)) << "组 " << g << " 的 offset 必须指向一条完整 entry";
    EXPECT_EQ(0u, h.shared) << "组 " << g << " 首条必须 shared == 0（组间不共享前缀）";
    EXPECT_EQ(model[g * kRestartInterval].first, p.substr(h.key_off, h.non_shared))
        << "组 " << g << " 首条必须存完整 key（key_delta == key）";
  }

  // 组内（第 2 条）必须发生前缀压缩 ⇒ shared > 0
  {
    RawHeader h0, h1;
    const uint32_t o0 = Fixed32At(p, ra);
    ASSERT_TRUE(ParseRawHeader(p, o0, &h0));
    ASSERT_TRUE(ParseRawHeader(p, h0.next, &h1));
    EXPECT_EQ(0u, h0.shared);
    EXPECT_GT(h1.shared, 0u) << "组内第 2 条必须与前一条共享前缀（否则前缀压缩没发生）";
    EXPECT_LT(h1.shared, model[0].first.size()) << "shared 不得等于整条 key 长度（non_shared >= 1）";
  }

  // 全量解码：键值逐字节相等（证明压缩/复原正确，含组边界跨组不共享）
  std::unique_ptr<BlockReader> r;
  ASSERT_TRUE(BlockReader::Open(fin, &r).ok());
  size_t i = 0;
  for (r->SeekToFirst(); r->Valid(); r->Next(), ++i) {
    ASSERT_LT(i, model.size());
    EXPECT_EQ(model[i].first, r->key().ToString()) << "i=" << i;
    EXPECT_EQ(model[i].second, r->value().ToString()) << "i=" << i;
  }
  EXPECT_EQ(model.size(), i);
  EXPECT_TRUE(r->status().ok());
}

// ===== M3-A03 =====
TEST(Block, RestartOffsetsMonotonicAndAligned) {
  BlockBuilder b(kRestartInterval);
  std::vector<std::pair<std::string, std::string>> model;
  BuildBlock(40, &b, &model);
  const std::string good = b.Finish().ToString();
  const size_t ra = RestartArrayOffset(good);
  ASSERT_EQ(3u, Fixed32At(good, ra + 12));

  EXPECT_TRUE(ValidatePayload(Slice(good)).ok())
      << "正常块必须通过结构校验：" << ValidatePayload(Slice(good)).ToString();

  {
    std::unique_ptr<BlockReader> r;
    ASSERT_TRUE(BlockReader::Open(Slice(good), &r).ok());
    EXPECT_EQ(0u, r->RestartOffset(0));
    EXPECT_LT(r->RestartOffset(0), r->RestartOffset(1));
    EXPECT_LT(r->RestartOffset(1), r->RestartOffset(2));
  }

  // 非单调（第 2 个 restart 与前一个相等）
  {
    std::string bad = good;
    SetFixed32(&bad, ra + 4, Fixed32At(bad, ra));
    const Status s = ValidatePayload(Slice(bad));
    EXPECT_EQ(Status::kCorruption, s.code()) << "restart_offset 非严格递增必须判 kCorruption：" << s.ToString();
  }
  // restart_offset[0] != 0
  {
    std::string bad = good;
    SetFixed32(&bad, ra, 1);
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(bad)).code()) << "restart_offset[0] 必须为 0";
  }
  // restart 指向 entry 内部（非边界）
  {
    std::string bad = good;
    SetFixed32(&bad, ra + 4, Fixed32At(bad, ra + 4) + 1);
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(bad)).code())
        << "restart_offset 必须落在 entry 起始边界上";
  }
  // restart_count == 0（§3.2 要求 >= 1）
  {
    std::string bad = good;
    SetFixed32(&bad, bad.size() - 4, 0);
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(bad)).code()) << "restart_count 必须 >= 1";
  }
  // payload 小于最小块
  EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(good.substr(0, kBlockMinPayload - 1))).code());
}

// ===== M3-A04 =====
TEST(Block, SeekSemantics) {
  // 空块：任何 Seek 都 Invalid（且状态仍为 OK，不是错误）
  {
    BlockBuilder e(kRestartInterval);
    std::unique_ptr<BlockReader> r;
    ASSERT_TRUE(BlockReader::Open(e.Finish(), &r).ok());
    ASSERT_TRUE(r->Seek(Slice(Ik(1))).ok());
    EXPECT_FALSE(r->Valid());
    EXPECT_TRUE(r->status().ok());
  }

  BlockBuilder b(kRestartInterval);
  std::vector<std::pair<std::string, std::string>> model;
  BuildBlock(40, &b, &model);
  std::unique_ptr<BlockReader> r;
  ASSERT_TRUE(BlockReader::Open(b.Finish(), &r).ok());

  // 精确命中
  ASSERT_TRUE(r->Seek(Slice(Ik(20))).ok());
  ASSERT_TRUE(r->Valid());
  EXPECT_EQ(Uk(20), UserKeyOf(*r));
  EXPECT_EQ(Val(20), r->value().ToString());

  // 落在两条之间 ⇒ 必须落在**后**一条（第一个 key >= target）
  ASSERT_TRUE(r->Seek(Slice(std::string("key00195"))).ok());
  ASSERT_TRUE(r->Valid()) << "介于 key0019 与 key0020 之间必须落在后一条";
  EXPECT_EQ(Uk(20), UserKeyOf(*r));

  // 组边界处 Seek（第 16 条是重启点）
  ASSERT_TRUE(r->Seek(Slice(Ik(16))).ok());
  ASSERT_TRUE(r->Valid());
  EXPECT_EQ(Uk(16), UserKeyOf(*r));
  ASSERT_TRUE(r->Prev().ok());
  ASSERT_TRUE(r->Valid());
  EXPECT_EQ(Uk(15), UserKeyOf(*r)) << "Prev 必须能跨组回退";

  // 越过末尾 ⇒ Invalid
  ASSERT_TRUE(r->Seek(Slice(std::string("zzzz"))).ok());
  EXPECT_FALSE(r->Valid()) << "Seek 到最后一个 key 之后必须越过末尾";
  EXPECT_TRUE(r->status().ok());

  // 第一与最后
  ASSERT_TRUE(r->SeekToFirst().ok());
  ASSERT_TRUE(r->Valid());
  EXPECT_EQ(Uk(0), UserKeyOf(*r));
  ASSERT_TRUE(r->SeekToLast().ok());
  ASSERT_TRUE(r->Valid());
  EXPECT_EQ(Uk(39), UserKeyOf(*r));
}

// ===== M3-A05 =====
TEST(Block, MalformedEntryRejectedWithoutOOB) {
  // 每条畸形 payload 只留一个违规点，其余字段合法。
  const std::string k0 = Ik(0);
  const std::string k1 = Ik(1);

  // (1) shared > 上一条 key 长度
  {
    std::string e0, e1;
    AppendRawEntry(&e0, 0, Slice(k0), Slice("v"));
    AppendRawEntry(&e1, static_cast<uint32_t>(k0.size() + 7), Slice("x"), Slice("v"));
    const std::string p = MakeRawPayload({e0, e1}, {0});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p)).code()) << "shared 超过上一条 key 长度";
    std::unique_ptr<BlockReader> r;
    const Status s = BlockReader::Open(Slice(p), &r);
    EXPECT_EQ(Status::kCorruption, s.code()) << "Open 也必须拒绝：" << s.ToString();
  }

  // (2) shared + non_shared > 剩余字节（必须先校验再读，不得越界）
  {
    std::string e0;
    AppendRawEntry(&e0, 0, Slice(k0), Slice("v"));
    std::string p = MakeRawPayload({e0}, {0});
    // 把最后一条 entry 的 non_shared 改大；这里直接构造一条"大头"entry
    std::string e_bad;
    AppendVarint32Manual(&e_bad, 0);
    AppendVarint32Manual(&e_bad, 4096);   // non_shared 远超剩余字节
    e_bad.append("abc");
    const std::string p2 = MakeRawPayload({e_bad}, {0});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p2)).code()) << "non_shared 超过剩余字节";
    EXPECT_TRUE(ValidatePayload(Slice(p)).ok()) << "对照组：同形状的合法单条 entry 必须通过校验";
  }

  // (3) value_len > 剩余字节
  {
    std::string e;
    AppendVarint32Manual(&e, 0);
    AppendVarint32Manual(&e, static_cast<uint32_t>(k0.size()));
    e.append(k0);
    AppendVarint32Manual(&e, 1024);   // vlen 远超剩余
    e.append("v");
    const std::string p = MakeRawPayload({e}, {0});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p)).code()) << "value_len 超过剩余字节";
  }

  // (4) non_shared == 0（§3.2 要求 >= 1）
  {
    std::string e;
    AppendRawEntry(&e, 0, Slice(""), Slice("v"));
    const std::string p = MakeRawPayload({e}, {0});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p)).code()) << "non_shared 必须 >= 1";
  }

  // (5) restart 点 shared != 0
  {
    std::string e;
    AppendRawEntry(&e, 3, Slice(k1), Slice("v"));
    const std::string p = MakeRawPayload({e}, {0});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p)).code()) << "restart 点 shared 必须为 0";
  }

  // (6) restart_offset 指向 payload 之外
  {
    std::string e;
    AppendRawEntry(&e, 0, Slice(k0), Slice("v"));
    const std::string p = MakeRawPayload({e}, {0xffffffffu});
    EXPECT_EQ(Status::kCorruption, ValidatePayload(Slice(p)).code()) << "restart_offset 越界必须拒绝";
  }
}

// ===== M3-A06 =====
TEST(Block, IteratorBidirectional) {
  const int kEntries = 40;
  BlockBuilder b(kRestartInterval);
  std::vector<std::pair<std::string, std::string>> model;
  BuildBlock(kEntries, &b, &model);

  std::unique_ptr<BlockReader> fwd, bwd;
  ASSERT_TRUE(BlockReader::Open(b.Finish(), &fwd).ok());
  ASSERT_TRUE(BlockReader::Open(b.Finish(), &bwd).ok());

  std::vector<std::string> forward, backward;
  for (fwd->SeekToFirst(); fwd->Valid(); fwd->Next()) forward.push_back(fwd->key().ToString());
  for (bwd->SeekToLast(); bwd->Valid(); bwd->Prev()) backward.push_back(bwd->key().ToString());
  EXPECT_TRUE(fwd->status().ok());
  EXPECT_TRUE(bwd->status().ok());

  ASSERT_EQ(static_cast<size_t>(kEntries), forward.size());
  ASSERT_EQ(forward.size(), backward.size());
  for (size_t i = 0; i < forward.size(); ++i) {
    EXPECT_EQ(forward[i], backward[backward.size() - 1 - i])
        << "正反向序列必须互为逆序（i=" << i << "）";
  }
}

// ===== M3-A07 =====
TEST(Footer, RoundTrip) {
  EXPECT_EQ(44u, kFooterSize) << "§3.7：footer 定长 44 B";
  EXPECT_EQ(16u, kBlockHandleEncodedLength);

  Footer f;
  f.index_handle.offset = 123456789ull;
  f.index_handle.size = 4242ull;
  f.metaindex_handle.offset = 777ull;
  f.metaindex_handle.size = 18ull;

  std::string enc;
  f.EncodeTo(&enc);
  ASSERT_EQ(kFooterSize, enc.size()) << "EncodeTo 必须恰好产出 44 字节";

  Footer g;
  ASSERT_TRUE(g.DecodeFrom(Slice(enc)).ok());
  EXPECT_EQ(f.index_handle.offset, g.index_handle.offset);
  EXPECT_EQ(f.index_handle.size, g.index_handle.size);
  EXPECT_EQ(f.metaindex_handle.offset, g.metaindex_handle.offset);
  EXPECT_EQ(f.metaindex_handle.size, g.metaindex_handle.size);

  // BlockHandle 单独往返：恰 16 字节，不足 16 字节 ⇒ kCorruption
  std::string h;
  BlockHandle bh;
  bh.offset = 99;
  bh.size = 3;
  bh.EncodeTo(&h);
  ASSERT_EQ(kBlockHandleEncodedLength, h.size());
  size_t consumed = 0;
  BlockHandle bh2;
  ASSERT_TRUE(bh2.DecodeFrom(Slice(h), &consumed).ok());
  EXPECT_EQ(kBlockHandleEncodedLength, consumed);
  EXPECT_EQ(99u, bh2.offset);
  EXPECT_EQ(3u, bh2.size);
  EXPECT_EQ(Status::kCorruption, bh2.DecodeFrom(Slice(h.substr(0, 15)), &consumed).code());
}

// ===== M3-A08（前 4 行；后 3 行需要 file_size，随 Table 片补齐）=====
TEST(Footer, RejectMatrix) {
  Footer f;
  f.index_handle.offset = 100;
  f.index_handle.size = 50;
  f.metaindex_handle.offset = 10;
  f.metaindex_handle.size = 19;
  std::string enc;
  f.EncodeTo(&enc);
  ASSERT_EQ(kFooterSize, enc.size());

  Footer out;
  // ① 文件长度 < 44
  {
    const Status s = out.DecodeFrom(Slice(enc.substr(0, kFooterSize - 1)));
    EXPECT_EQ(Status::kCorruption, s.code()) << "长度不足必须 kCorruption：" << s.ToString();
  }
  // ② magic 不符
  {
    std::string bad = enc;
    bad[0] = static_cast<char>(bad[0] ^ 0x01);
    EXPECT_EQ(Status::kCorruption, out.DecodeFrom(Slice(bad)).code()) << "magic 必须校验";
  }
  // ③ version = 2 ⇒ kNotSupported（是"更新的引擎写的合法文件"，不是损坏）
  {
    std::string bad = enc;
    SetFixed32(&bad, 4, kTableFormatVersion + 1);
    // 关键：更新引擎写出的合法文件带**正确**的 footer_crc，故必须一起重算 ——
    // 否则测到的是"CRC 坏"（kCorruption），而不是"version 不认识"（kNotSupported）。
    SetFixed32(&bad, kFooterSize - 4, crc32c::Value(bad.data(), kFooterSize - 4));
    EXPECT_EQ(Status::kNotSupported, out.DecodeFrom(Slice(bad)).code())
        << "version != 1 且 CRC 正确 ⇒ 必须是 kNotSupported（前向兼容信号），不得报 kCorruption";
  }
  // ④ footer_crc 不符
  {
    std::string bad = enc;
    bad[kFooterSize - 1] = static_cast<char>(bad[kFooterSize - 1] ^ 0x01);
    EXPECT_EQ(Status::kCorruption, out.DecodeFrom(Slice(bad)).code()) << "footer_crc 必须校验";
  }
  // ⑤⑥⑦ handle 越界 / metaindex 与 index 顺序 / index 未紧贴 footer：
  //        需要 file_size，属于 Table 打开路径（§3.7 失败矩阵后三行）。
  {
    const uint64_t kPrefixLen = 56;   // file_size = 100 ⇒ 块区域 = 56，footer 紧贴文件尾
    const auto reject = [&](const Footer& footer, const char* what) {
      test::MemEnv env;
      std::string file(kPrefixLen, '\0');
      footer.EncodeTo(&file);
      env.SetContents("reject.sst", file);
      std::shared_ptr<Table> t;
      const Status s = Table::Open(TableOptions(), &env, "reject.sst", &t);
      EXPECT_EQ(Status::kCorruption, s.code()) << what << "：" << s.ToString();
    };

    // ⑤ handle 越界（offset+size > file_size-44）
    {
      Footer bad;
      bad.index_handle.offset = kPrefixLen + 1000;
      bad.index_handle.size = 16;
      bad.metaindex_handle.offset = 0;
      bad.metaindex_handle.size = 16;
      reject(bad, "handle 越界必须 kCorruption");
    }
    // ⑥ metaindex.offset+size > index.offset（顺序约束）
    {
      Footer bad;
      bad.index_handle.offset = 0;
      bad.index_handle.size = kPrefixLen;
      bad.metaindex_handle.offset = 10;
      bad.metaindex_handle.size = 20;
      reject(bad, "metaindex.offset+size > index.offset 必须 kCorruption");
    }
    // ⑦ index.offset+size != file_size-44（索引必须紧贴 footer）
    {
      Footer bad;
      bad.index_handle.offset = 0;
      bad.index_handle.size = kPrefixLen - 1;
      bad.metaindex_handle.offset = 0;
      bad.metaindex_handle.size = 0;
      reject(bad, "index.offset+size != file_size-44 必须 kCorruption");
    }
  }
}

}  // namespace lsm
