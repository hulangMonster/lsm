// tests/filter_test.cpp —— M5.1 Bloom filter / filter block / metaindex / 读路径否定 / 计数器
//
// 契约来源：docs/m5-design.md §3（filter 位级格式、读路径接入与计数器）、§4 §12（protocol 追加）、
//           §9.2 的 I47~I50、§10.1 的 M5-A01~A10 与 M5-A18。
//
// A 组纪律（§10.1 末段 + §10.3 反空绿）：
//   * 零断言 TEST 块 = 0；禁用 DISABLED_/GTEST_SKIP/`|| true`；
//   * 每条「机制」用例都有**反向自检**：A04 的错位注入必须被检出（filter_state==kCorrupt 或
//     filter_unavailable>0），A05 必须有「肯定 ⇒ 一定读块」的对照，A10 必须打印两个原始计数；
//   * 全部建在 MemEnv 上（零真实磁盘、零 flaky）；固定种子 RNG。
#include "test_harness.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bloom.h"
#include "db_impl.h"
#include "filter_policy.h"
#include "memenv.h"
#include "sstable/format.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {
namespace test {
namespace {

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%06d", i);
  return std::string(buf);
}
std::string Val(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%06d", i);
  return std::string(buf);
}

std::string IKey(const std::string& user, SequenceNumber seq, ValueType type) {
  return ManualInternalKey(user, seq, type);
}

// internal key 序：user key 升序，同一 user key 按 sequence **降序**（protocol §6）。
bool InternalKeyLess(const std::string& l, const std::string& r) {
  Slice lu, ru;
  SequenceNumber ls = 0, rs = 0;
  ValueType lt = kTypeValue, rt = kTypeValue;
  const bool lok = ParseInternalKey(Slice(l), &lu, &ls, &lt);
  const bool rok = ParseInternalKey(Slice(r), &ru, &rs, &rt);
  if (!lok || !rok) return l < r;
  const int c = lu.compare(ru);
  if (c != 0) return c < 0;
  return ls > rs;
}

// ---------------------------------------------------------------------------
// 手工 SSTable 构造器：M5-A04/A06/A08/A09 需要「结构上看似合法的畸形 metaindex / filter 块」，
// TableBuilder 只产出良构文件 ⇒ 这几条用例必须自己拼字节。
// ---------------------------------------------------------------------------
BlockHandle AppendRawBlock(std::string* out, uint8_t type, const Slice& payload) {
  BlockHandle h;
  h.offset = out->size();
  std::string buf;
  PutFixed32(&buf, static_cast<uint32_t>(payload.size()));
  buf.push_back(static_cast<char>(type));
  buf.append(payload.data(), payload.size());
  PutFixed32(&buf, crc32c::Value(buf.data(), buf.size()));
  out->append(buf);
  h.size = buf.size();
  return h;
}

void FixBlockCrc(std::string* bytes, const BlockHandle& h) {
  const uint32_t crc =
      crc32c::Value(bytes->data() + h.offset, static_cast<size_t>(h.size) - kBlockTrailerSize);
  const size_t off = static_cast<size_t>(h.offset + h.size - kBlockTrailerSize);
  for (int i = 0; i < 4; ++i) {
    (*bytes)[off + static_cast<size_t>(i)] = static_cast<char>((crc >> (8 * i)) & 0xffu);
  }
}

std::string HandleBytes(const BlockHandle& h) {
  std::string b;
  h.EncodeTo(&b);
  return b;
}

struct RawTable {
  std::string bytes;
  std::vector<BlockHandle> data_handles;
  BlockHandle filter_handle;
  BlockHandle metaindex_handle;
  BlockHandle index_handle;
  bool has_filter = false;
  std::string smallest;   // 第一条 internal key
  std::string largest;    // 最后一条 internal key

  void Build(const std::vector<std::pair<std::string, std::string>>& data_kv, size_t block_size,
             const std::string* filter_payload, uint8_t filter_block_type,
             const BlockHandle* filter_handle_override, bool dup_filter_entry,
             const std::vector<std::pair<std::string, std::string>>& meta_extra) {
    bytes.clear();
    data_handles.clear();
    has_filter = false;
    std::vector<std::string> last_keys;
    std::vector<BlockHandle> handles;

    const size_t n = data_kv.size();
    BlockBuilder data(kRestartInterval);
    size_t est = 0;
    for (size_t i = 0; i < n; ++i) {
      if (!data.empty() && est + data_kv[i].first.size() + data_kv[i].second.size() > block_size) {
        handles.push_back(AppendRawBlock(&bytes, kBlockTypeData, data.Finish()));
        last_keys.push_back(data_kv[i - 1].first);
        data.Reset();
        est = 0;
      }
      data.Add(Slice(data_kv[i].first), Slice(data_kv[i].second));
      est += data_kv[i].first.size() + data_kv[i].second.size() + 8;
    }
    if (!data.empty()) {
      handles.push_back(AppendRawBlock(&bytes, kBlockTypeData, data.Finish()));
      last_keys.push_back(data_kv[n - 1].first);
    }
    data_handles = handles;
    if (n > 0) {
      smallest = data_kv.front().first;
      largest = data_kv.back().first;
    }

    if (filter_payload != nullptr) {
      filter_handle = AppendRawBlock(&bytes, filter_block_type, Slice(*filter_payload));
      has_filter = true;
    }

    BlockHandle meta_filter_handle = filter_handle;
    if (filter_handle_override != nullptr) meta_filter_handle = *filter_handle_override;
    BlockBuilder meta(kIndexRestartInterval);
    if (filter_payload != nullptr || filter_handle_override != nullptr) {
      meta.Add(Slice(kBuiltinBloomFilterName), Slice(HandleBytes(meta_filter_handle)));
      if (dup_filter_entry) {
        meta.Add(Slice(kBuiltinBloomFilterName), Slice(HandleBytes(meta_filter_handle)));
      }
    }
    for (const auto& e : meta_extra) meta.Add(Slice(e.first), Slice(e.second));
    metaindex_handle = AppendRawBlock(&bytes, kBlockTypeMetaIndex, meta.Finish());

    BlockBuilder index(kIndexRestartInterval);
    for (size_t i = 0; i < handles.size(); ++i) {
      index.Add(Slice(last_keys[i]), Slice(HandleBytes(handles[i])));
    }
    index_handle = AppendRawBlock(&bytes, kBlockTypeIndex, index.Finish());

    Footer f;
    f.index_handle = index_handle;
    f.metaindex_handle = metaindex_handle;
    f.EncodeTo(&bytes);
  }

  // filter 块的 payload（不含 5B 头与 4B CRC）。
  std::string FilterPayload() const {
    return bytes.substr(static_cast<size_t>(filter_handle.offset) + kBlockHeaderSize,
                        static_cast<size_t>(filter_handle.size) - kBlockOverhead);
  }
  void SetFilterPayload(const std::string& p, bool fix_crc) {
    const size_t off = static_cast<size_t>(filter_handle.offset) + kBlockHeaderSize;
    std::memcpy(&bytes[off], p.data(), p.size());
    if (fix_crc) FixBlockCrc(&bytes, filter_handle);
  }
};

uint32_t PayloadN(const std::string& p) { return DecodeFixed32(p.data() + p.size() - 4); }
uint32_t PayloadArrayOffset(const std::string& p) {
  return DecodeFixed32(p.data() + p.size() - 8);
}
uint32_t PayloadOffset(const std::string& p, size_t i) {
  return DecodeFixed32(p.data() + PayloadArrayOffset(p) + 4 * i);
}
void PutU32At(std::string* s, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    (*s)[off + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
  }
}
void PutPayloadOffset(std::string* p, size_t i, uint32_t v) {
  PutU32At(p, PayloadArrayOffset(*p) + 4 * i, v);
}

// 用 FilterBlockBuilder 造一个「覆盖 5 个桶」的良构 filter payload。
std::string MakeMultiBucketFilterPayload(const std::vector<std::vector<std::string>>& buckets,
                                         const FilterPolicy* policy) {
  FilterBlockBuilder b(policy);
  b.StartBlock(0);
  for (size_t i = 0; i < buckets.size(); ++i) {
    b.StartBlock(static_cast<uint64_t>(kFilterBase) * (i + 1));
    for (const std::string& k : buckets[i]) b.AddKey(Slice(k));
  }
  return b.Finish().ToString();
}

// ---------------------------------------------------------------------------
// DB 级帮助（A05/A07/A10/A18）
// ---------------------------------------------------------------------------
PersistentDBImpl* OpenPersistent(MemEnv* env, size_t write_buffer_size, int bloom_bits,
                                 const std::string& name, size_t block_size = 4096) {
  Options options;
  options.env = env;
  options.write_buffer_size = write_buffer_size;
  options.block_size = block_size;
  options.bloom_bits = bloom_bits;
  DB* db = nullptr;
  const Status s = DB::Open(options, name, &db);
  if (!s.ok() || db == nullptr) {
    ADD_FAILURE() << "DB::Open(" << name << ") 失败：" << s.ToString();
    delete db;
    return nullptr;
  }
  return static_cast<PersistentDBImpl*>(db);
}

uint64_t DeltaOf(uint64_t after, uint64_t before) { return after - before; }

}  // namespace

// ===========================================================================
// M5-A01 Bloom 位图/编码往返（空 filter、单 key、多 key、n 很大、k 上下界、位数回填）
// ===========================================================================
TEST(Filter, BloomEncodingRoundTrip) {
  // --- 自由函数：空 filter / 只有 k 的 filter 的边界语义（§3.3）---
  EXPECT_FALSE(BloomKeyMayMatch(Slice("x"), Slice())) << "len==0 ⇒ false（空桶没有 key）";
  {
    const char only_k[1] = {7};
    EXPECT_TRUE(BloomKeyMayMatch(Slice("x"), Slice(only_k, 1)))
        << "len==1 ⇒ true（只有 k 没有 bitset，保守判可能存在）";
  }

  struct Case {
    int bits_per_key;
    int n;
    int expect_k;
  };
  const Case cases[] = {
      {1, 1, 1},       // k 下界：round(1·ln2)=1
      {10, 1, 7},      // 默认参数（M5:10 的 k≈7）
      {64, 4096, 30},  // k 上界：round(64·ln2)=44 ⇒ clamp 到 30
      {10, 5000, 7},   // n 很大（> 8192 bits）
      {10, 100, 7},
  };
  for (const Case& c : cases) {
    const FilterPolicy* p = NewBuiltinBloomPolicy(c.bits_per_key);
    ASSERT_NE(nullptr, p) << "bits_per_key=" << c.bits_per_key;
    std::vector<std::string> keys;
    std::vector<Slice> slices;
    keys.reserve(static_cast<size_t>(c.n));
    for (int i = 0; i < c.n; ++i) {
      keys.push_back("key-" + std::to_string(c.bits_per_key) + "-" + std::to_string(i));
    }
    for (const std::string& k : keys) slices.emplace_back(k);

    std::string filter;
    p->CreateFilter(slices.data(), c.n, &filter);
    ASSERT_GE(filter.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(filter[filter.size() - 1]), c.expect_k)
        << "bits=" << c.bits_per_key << " n=" << c.n << " 的 k 应与 round(bits·ln2) 一致";
    const size_t bits = (filter.size() - 1) * 8;
    EXPECT_GE(bits, kBloomMinBits) << "位数下限 kBloomMinBits=64（§3.3）";
    EXPECT_GE(bits, static_cast<size_t>(c.n) * static_cast<size_t>(c.bits_per_key));

    for (const std::string& k : keys) {
      EXPECT_TRUE(BloomKeyMayMatch(Slice(k), Slice(filter))) << "假阴性：" << k;
    }
  }

  // --- 非法 bits_per_key ⇒ nullptr（工厂的输入契约）---
  EXPECT_EQ(nullptr, NewBuiltinBloomPolicy(0));
  EXPECT_EQ(nullptr, NewBuiltinBloomPolicy(65));
  EXPECT_EQ(nullptr, NewBuiltinBloomPolicy(-1));

  // --- n == 0：CreateFilter 不改 dst（空桶由 builder 以「0 字节 filter」表达）---
  {
    const FilterPolicy* p = NewBuiltinBloomPolicy(10);
    std::string dst;
    p->CreateFilter(nullptr, 0, &dst);
    EXPECT_TRUE(dst.empty());
  }

  // --- FilterBlockBuilder → FilterBlockReader 的结构往返 ---
  {
    const FilterPolicy* p = NewBuiltinBloomPolicy(10);
    const std::string payload = MakeMultiBucketFilterPayload({{"alpha", "beta"}, {"gamma"}}, p);
    const uint32_t n = PayloadN(payload);
    EXPECT_GE(n, 3u) << "两个 StartBlock + Finish ⇒ 至少 3 个桶";
    EXPECT_EQ(static_cast<size_t>(PayloadArrayOffset(payload)) + 4u * n + 8u, payload.size())
        << "§3.2 校验 3：array_offset + 4n + 8 == payload.size()";
    EXPECT_EQ(PayloadOffset(payload, 0), 0u) << "offset[0] 必须为 0（§3.2 校验 5）";

    FilterBlockReader r{Slice(payload)};
    ASSERT_TRUE(r.valid()) << "builder 产出的 payload 必须自洽";
    EXPECT_EQ(r.NumFilters(), n);
    EXPECT_EQ(r.filter_bytes(), payload.size());
    // 桶映射（E2）：StartBlock(1×kFilterBase) 之后的 key 落在桶 1；StartBlock(2×kFilterBase)
    // 之后的 key 落在桶 2；桶 0 是**被跳过的空桶**（数据文件的第一个数据块在偏移 0 时才用它）。
    EXPECT_TRUE(r.KeyMayMatch(static_cast<uint64_t>(kFilterBase), Slice("alpha")));
    EXPECT_TRUE(r.KeyMayMatch(static_cast<uint64_t>(kFilterBase), Slice("beta")));
    EXPECT_TRUE(r.KeyMayMatch(static_cast<uint64_t>(kFilterBase) * 2, Slice("gamma")));
    EXPECT_FALSE(r.KeyMayMatch(static_cast<uint64_t>(kFilterBase), Slice("gamma")))
        << "跨桶不得命中（桶 1 没有 gamma）";
    // 被跳过的空桶返回 false（该桶没有数据块，安全）。
    EXPECT_FALSE(r.KeyMayMatch(0, Slice("alpha"))) << "空桶必须否定（§3.2「len == 0 ⇒ false」）";
    // 越界 index ⇒ 保守返回 true（§3.7 硬规则 3）。
    EXPECT_TRUE(r.KeyMayMatch(static_cast<uint64_t>(kFilterBase) * (n + 100), Slice("alpha")));
  }
}

// ===========================================================================
// M5-A02 误判率实测（N=100000 插入、M=100000 未插入）
// ===========================================================================
TEST(Filter, FalsePositiveRateMeasured) {
  const int N = 100000;
  const int M = 100000;
  const FilterPolicy* p = NewBuiltinBloomPolicy(10);
  ASSERT_NE(nullptr, p);

  std::vector<std::string> inserted;
  std::vector<Slice> slices;
  inserted.reserve(N);
  slices.reserve(N);
  for (int i = 0; i < N; ++i) inserted.push_back(KeyFromIndex(static_cast<uint64_t>(i)));
  for (const std::string& k : inserted) slices.emplace_back(k);

  std::string filter;
  p->CreateFilter(slices.data(), N, &filter);
  ASSERT_GT(filter.size(), 0u);

  uint64_t false_negatives = 0;
  for (const std::string& k : inserted) {
    if (!BloomKeyMayMatch(Slice(k), Slice(filter))) ++false_negatives;
  }

  uint64_t false_positives = 0;
  for (int i = 0; i < M; ++i) {
    const std::string k = KeyFromIndex(static_cast<uint64_t>(i) + (1ull << 40));
    if (BloomKeyMayMatch(Slice(k), Slice(filter))) ++false_positives;
  }
  const double fpr = static_cast<double>(false_positives) / static_cast<double>(M);
  const uint64_t ppm = static_cast<uint64_t>(fpr * 1000000.0 + 0.5);

  std::printf("M5_FILTER_FALSE_NEGATIVE %llu\n", static_cast<unsigned long long>(false_negatives));
  std::printf("M5_FILTER_FPR_PPM %llu\n", static_cast<unsigned long long>(ppm));
  std::printf("FILTER_FPR_MEASURED_PPM %llu (inserted=%d probes=%d)\n",
              static_cast<unsigned long long>(ppm), N, M);
  std::fflush(stdout);

  EXPECT_EQ(false_negatives, 0u) << "Bloom 不得产生假阴性（I47）";
  EXPECT_LE(ppm, 20000u) << "实测误判率 " << ppm << "ppm 超过 2.0% 的上限（§3.3）";
  EXPECT_GT(false_positives, 0u) << "0 个误判说明哈希/位序实现可疑（不是「更好」，是「没在测」）";

  // ---- 附：**结构性 key 分布**下的误判率实测（负结果，登记用；不改默认参数，M5:21）----
  // 若数据集只含 8B 大端 key 的**偶数**、探测只取同区间的**奇数**（key 之间只差最低位），
  // protocol §12.3 规定的 LevelDB 哈希 + Double Hashing 会退化：相邻 key 的 7 个探测位高度重合。
  // 退化在小 filter（≈一个数据块的 37 个 key、m=384 bits）上**远比**大 filter 严重，
  // 因为 `delta = (h>>17)|(h<<15)` 与 m 的公因子会把 7 次探测压到很少的几个位上。
  // 这是**算法在特定 key 分布下的已知弱点**，不是实现偏离（D1 要求逐字采用该哈希；
  // M5:21 禁止为了门禁好看去改默认参数）。
  {
    const FilterPolicy* pol = NewBuiltinBloomPolicy(10);
    // (a) 大 filter：N=100000 偶数 key，探测同区间的 100000 个奇数 key。
    const int struct_n = 100000;
    std::vector<std::string> bk;
    std::vector<Slice> bsl;
    bk.reserve(struct_n);
    for (int i = 0; i < struct_n; ++i) bk.push_back(KeyFromIndex(static_cast<uint64_t>(2 * i)));
    for (const std::string& k : bk) bsl.emplace_back(k);
    std::string bf;
    pol->CreateFilter(bsl.data(), struct_n, &bf);
    uint64_t fp_big = 0;
    for (int i = 0; i < struct_n; ++i) {
      const std::string q = KeyFromIndex(static_cast<uint64_t>(2 * i + 1));
      if (BloomKeyMayMatch(Slice(q), Slice(bf))) ++fp_big;
    }
    uint64_t fn_big = 0;
    for (const std::string& k : bk) {
      if (!BloomKeyMayMatch(Slice(k), Slice(bf))) ++fn_big;
    }
    // (b) 桶级 filter：每个桶 37 个偶数 key（≈一个 4 KiB 数据块），探测该桶区间内的奇数 key。
    const int kBucketKeys = 37;
    const int kBuckets = 2000;
    uint64_t fp_bucket = 0;
    uint64_t fn_bucket = 0;
    for (int b = 0; b < kBuckets; ++b) {
      std::vector<std::string> kb;
      std::vector<Slice> ks;
      const uint64_t base = static_cast<uint64_t>(b) * static_cast<uint64_t>(kBucketKeys);
      for (int j = 0; j < kBucketKeys; ++j) {
        kb.push_back(KeyFromIndex(2 * (base + static_cast<uint64_t>(j))));
      }
      for (const std::string& k : kb) ks.emplace_back(k);
      std::string f;
      pol->CreateFilter(ks.data(), kBucketKeys, &f);
      for (const std::string& k : kb) {
        if (!BloomKeyMayMatch(Slice(k), Slice(f))) ++fn_bucket;
      }
      for (int j = 0; j < kBucketKeys; ++j) {
        const std::string q = KeyFromIndex(2 * (base + static_cast<uint64_t>(j)) + 1);
        if (BloomKeyMayMatch(Slice(q), Slice(f))) ++fp_bucket;
      }
    }
    const uint64_t ppm_big = fp_big * 1000000ull / static_cast<uint64_t>(struct_n);
    const uint64_t ppm_bucket =
        fp_bucket * 1000000ull / (static_cast<uint64_t>(kBuckets) * kBucketKeys);
    std::printf("M5_FILTER_FPR_STRUCTURED_PPM %llu (even_keys=%d odd_probes=%d)\n",
                static_cast<unsigned long long>(ppm_big), struct_n, struct_n);
    std::printf("M5_FILTER_FPR_STRUCTURED_BUCKET_PPM %llu (bucket_keys=%d buckets=%d)\n",
                static_cast<unsigned long long>(ppm_bucket), kBucketKeys, kBuckets);
    std::printf("FILTER_FPR_STRUCTURED_MEASURED_PPM %llu\n",
                static_cast<unsigned long long>(ppm_big));
    std::printf("FILTER_FPR_STRUCTURED_BUCKET_MEASURED_PPM %llu\n",
                static_cast<unsigned long long>(ppm_bucket));
    std::fflush(stdout);
    // 硬断言只有一条：**零假阴性**仍然成立（误判率高不等于丢数据）。I47 不受 key 分布影响。
    EXPECT_EQ(fn_big, 0u) << "结构性分布（大 filter）下也不得出现假阴性";
    EXPECT_EQ(fn_bucket, 0u) << "结构性分布（桶级 filter）下也不得出现假阴性";
  }
}

// ===========================================================================
// M5-A03 假阴性专项：tombstone、同 user key 多版本、最小/最大/相邻 key
// ===========================================================================
TEST(Filter, NoFalseNegativeOnSSTable) {
  MemEnv env;
  std::vector<std::pair<std::string, std::string>> kv;
  kv.emplace_back(IKey("a", 5, kTypeDeletion), std::string());   // 只有 tombstone 的 user key
  kv.emplace_back(IKey("b", 9, kTypeValue), "b3");
  kv.emplace_back(IKey("b", 8, kTypeValue), "b2");
  kv.emplace_back(IKey("b", 7, kTypeValue), "b1");
  kv.emplace_back(IKey("c", 2, kTypeValue), "c2");
  kv.emplace_back(IKey("c", 1, kTypeValue), "c1");
  for (int i = 0; i < 200; ++i) {
    kv.emplace_back(IKey("d" + std::string(static_cast<size_t>(i % 3), 'x') + std::to_string(i), 1,
                         kTypeValue),
                    "v" + std::to_string(i));
  }
  std::sort(kv.begin(), kv.end(),
            [](const std::pair<std::string, std::string>& l,
               const std::pair<std::string, std::string>& r) {
              return InternalKeyLess(l.first, r.first);
            });
  for (size_t i = 1; i < kv.size(); ++i) {
    ASSERT_TRUE(InternalKeyLess(kv[i - 1].first, kv[i].first)) << "测试数据必须严格递增：i=" << i;
  }

  Options opts;
  opts.env = &env;
  opts.block_size = 256;   // 强制多个数据块 ⇒ 覆盖多个 filter 桶
  opts.bloom_bits = 10;
  WritableFile* raw = nullptr;
  ASSERT_TRUE(env.NewWritableFile("fnsst.sst", &raw).ok());
  std::unique_ptr<WritableFile> file(raw);
  TableBuilder builder(opts, file.get());
  for (const auto& e : kv) ASSERT_TRUE(builder.Add(Slice(e.first), Slice(e.second)).ok());
  ASSERT_TRUE(builder.Finish().ok());
  ASSERT_TRUE(file->Close().ok());
  ASSERT_GT(builder.NumDataBlocks(), 1u) << "A03 需要多数据块以覆盖多个 filter 桶";
  ASSERT_GT(builder.filter_bytes(), 0u) << "本表必须写出 filter 块";

  std::shared_ptr<Table> t;
  ReadStats open_stats;
  ASSERT_TRUE(Table::Open(opts, &env, "fnsst.sst", &t, nullptr, nullptr, &open_stats).ok());
  EXPECT_EQ(Table::FilterState::kOk, t->filter_state());
  EXPECT_EQ(open_stats.filter_blocks_read, 1u);
  EXPECT_EQ(open_stats.filter_corrupt, 0u);
  EXPECT_GT(open_stats.filter_bytes_read, 0u);
  EXPECT_EQ(t->unknown_metaindex_entries(), 0u) << "filter name 已被识别，不得再计入 unknown";

  // ① tombstone-only 的 user key：必须是 kDeleted（若 filter 假阴性 ⇒ 变成 kNotFound ⇒ 用例失败）。
  {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("a", kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kDeleted, r) << "tombstone-only 的 user key 必须在 filter 里（D2）";
    EXPECT_EQ(rs.filter_checked, 1u);
  }
  // ② 所有已写入的 user key 都不得被 filter 否定（零假阴性）。
  std::vector<std::string> users = {"a", "b", "c"};
  for (int i = 0; i < 200; ++i) {
    users.push_back("d" + std::string(static_cast<size_t>(i % 3), 'x') + std::to_string(i));
  }
  uint64_t checked = 0;
  for (const std::string& u : users) {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey(u, kMaxSequenceNumber)), &v, &r, &rs).ok())
        << "user=" << u;
    EXPECT_NE(TableGetResult::kNotFound, r) << "假阴性（filter 把存在的 key 否定了）：user=" << u;
    checked += rs.filter_checked;
  }
  EXPECT_EQ(checked, users.size()) << "filter 必须对每个 user key 真的被查询一次（防空绿）";

  // ③ 反向自检：确实**不存在**但在 key range 内的 key 必须被 filter 否定（证明 filter 真的在否定）。
  {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("b~absent", kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kNotFound, r);
    EXPECT_EQ(rs.filter_negative, 1u) << "在范围内的不存在 key 必须被 filter 否定";
    EXPECT_EQ(rs.data_blocks_read, 0u);
  }
}

// ===========================================================================
// M5-A04 filter 与数据块的对应 + 错位注入必须被检出（E4）
// ===========================================================================
TEST(Filter, MisalignedOffsetInjectionDetected) {
  MemEnv env;
  Options opts;
  opts.env = &env;
  opts.bloom_bits = 10;
  opts.block_size = 512;

  // 200 个 entry ⇒ 多个数据块，起始偏移跨越多个 2 KiB 桶（E2 的映射才有意义）。
  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < 200; ++i) {
    kv.emplace_back(IKey(Key(i), 1, kTypeValue), Val(i) + std::string(30, 'q'));
  }
  WritableFile* raw = nullptr;
  ASSERT_TRUE(env.NewWritableFile("a04.sst", &raw).ok());
  std::unique_ptr<WritableFile> wf(raw);
  TableBuilder builder(opts, wf.get());
  for (const auto& e : kv) ASSERT_TRUE(builder.Add(Slice(e.first), Slice(e.second)).ok());
  ASSERT_TRUE(builder.Finish().ok());
  ASSERT_TRUE(wf->Close().ok());
  ASSERT_GT(builder.NumDataBlocks(), 1u);
  const uint64_t filter_block_size = builder.filter_bytes();
  ASSERT_GT(filter_block_size, 0u);

  // filter 块紧贴 metaindex 之前（§3.1）⇒ 由 metaindex handle 反推它的 handle。
  std::shared_ptr<Table> t0;
  ASSERT_TRUE(Table::Open(opts, &env, "a04.sst", &t0).ok());
  ASSERT_EQ(Table::FilterState::kOk, t0->filter_state());
  const uint64_t meta_off = t0->footer().metaindex_handle.offset;
  ASSERT_GE(meta_off, filter_block_size);
  BlockHandle fh;
  fh.offset = meta_off - filter_block_size;
  fh.size = filter_block_size;
  const std::string base = env.Contents("a04.sst");
  const uint32_t n = PayloadN(
      base.substr(static_cast<size_t>(fh.offset) + kBlockHeaderSize,
                  static_cast<size_t>(fh.size) - kBlockOverhead));
  ASSERT_GE(n, 2u) << "本数据集必须覆盖 >=2 个 filter 桶";

  auto read_payload = [&](const std::string& bytes) {
    return bytes.substr(static_cast<size_t>(fh.offset) + kBlockHeaderSize,
                        static_cast<size_t>(fh.size) - kBlockOverhead);
  };
  auto writeback = [&](const std::string& name, const std::string& new_payload, bool fix_crc) {
    std::string bytes = base;
    std::memcpy(&bytes[static_cast<size_t>(fh.offset) + kBlockHeaderSize], new_payload.data(),
                new_payload.size());
    if (fix_crc) FixBlockCrc(&bytes, fh);
    env.SetContents(name, bytes);
  };
  auto check_all = [&](const std::string& name, bool expect_corrupt) {
    std::shared_ptr<Table> t;
    ReadStats os;
    ASSERT_TRUE(Table::Open(opts, &env, name, &t, nullptr, nullptr, &os).ok())
        << name << "：filter 不自洽**不得**让 Open 失败（I49/§3.7）";
    EXPECT_EQ(expect_corrupt ? Table::FilterState::kCorrupt : Table::FilterState::kOk,
              t->filter_state())
        << name;
    if (expect_corrupt) {
      EXPECT_EQ(os.filter_corrupt, 1u) << name;
      EXPECT_EQ(os.filter_blocks_read, 0u) << name << "：损坏的 filter 不得计入「读到 filter」";
    } else {
      EXPECT_EQ(os.filter_corrupt, 0u) << name;
      EXPECT_EQ(os.filter_blocks_read, 1u) << name;
    }
    for (const auto& e : kv) {
      Slice user;
      SequenceNumber s = 0;
      ValueType ty = kTypeValue;
      ASSERT_TRUE(ParseInternalKey(Slice(e.first), &user, &s, &ty));
      ReadStats rs;
      std::string v;
      TableGetResult r = TableGetResult::kNotFound;
      ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey(user.ToString(), kMaxSequenceNumber)), &v, &r,
                              &rs)
                      .ok())
          << name;
      EXPECT_EQ(TableGetResult::kFound, r) << name;
      EXPECT_EQ(e.second, v) << name;
      if (expect_corrupt) {
        EXPECT_EQ(rs.filter_unavailable, 1u) << name << "：降级必须计数（§3.7 硬规则 6）";
      } else {
        EXPECT_EQ(rs.filter_checked, 1u) << name;
        EXPECT_EQ(rs.filter_positive, 1u) << name << "：肯定结论必须**真的读块**（I49）";
        EXPECT_EQ(rs.data_blocks_read, 1u) << name;
      }
    }
    {
      ReadStats rs;
      std::string v;
      TableGetResult r = TableGetResult::kFound;
      ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("k000100~absent", kMaxSequenceNumber)), &v, &r,
                              &rs)
                      .ok())
          << name;
      EXPECT_EQ(TableGetResult::kNotFound, r) << name;
      if (expect_corrupt) {
        EXPECT_EQ(rs.filter_unavailable, 1u) << name;
      } else {
        EXPECT_EQ(rs.filter_negative, 1u) << name << "：范围内容不存在 key 必须被否定";
        EXPECT_EQ(rs.data_blocks_read, 0u) << name;
      }
    }
  };

  check_all("a04.sst", /*expect_corrupt=*/false);

  // --- 注入 I：offset[n-1] 指到 array_offset 之后（真实错位），**重算 CRC** ⇒ 穿透 CRC 校验 ---
  {
    std::string p = read_payload(base);
    PutPayloadOffset(&p, n - 1, PayloadArrayOffset(p) + 4);
    writeback("misalign.sst", p, /*fix_crc=*/true);
    check_all("misalign.sst", /*expect_corrupt=*/true);
  }
  // --- 注入 II：array_offset 改小（§3.2 校验 3 的原始例子），同样重算 CRC ---
  {
    std::string p = read_payload(base);
    PutU32At(&p, p.size() - 8, PayloadArrayOffset(p) - 4);
    writeback("misalign2.sst", p, /*fix_crc=*/true);
    check_all("misalign2.sst", /*expect_corrupt=*/true);
  }
  // --- 注入 III：不重算 CRC 的 bit flip ⇒ CRC 检出 ---
  {
    std::string p = read_payload(base);
    p[0] = static_cast<char>(static_cast<unsigned char>(p[0]) ^ 0x01u);
    writeback("flip.sst", p, /*fix_crc=*/false);
    check_all("flip.sst", /*expect_corrupt=*/true);
  }

  // --- 反向自检：证明「检测器有效」——错的 filter 确实会让位判定失败（确定性构造）---
  {
    std::string empty_bitset(8, '\0');             // 全零 bitset
    empty_bitset.push_back(static_cast<char>(7));  // k = 7
    EXPECT_FALSE(BloomKeyMayMatch(Slice("alpha"), Slice(empty_bitset)))
        << "若实现误用错位/空的 filter，KeyMayMatch 会对存在的 key 返回 false ⇒ 上面的结果断言才有意义（E4）";
    // 同轮对照：同一个 key 在**单个真实 filter**上必须为 true（排除「KeyMayMatch 恒为 false」）。
    const FilterPolicy* pol = NewBuiltinBloomPolicy(10);
    const std::string alpha = "alpha";
    const Slice one_key(alpha);
    std::string one_filter;
    pol->CreateFilter(&one_key, 1, &one_filter);
    ASSERT_GE(one_filter.size(), 2u);
    EXPECT_TRUE(BloomKeyMayMatch(Slice("alpha"), Slice(one_filter)));
  }
}

// ===========================================================================
// M5-A05 filter 只用于否定（肯定/否定/不可用三态的块读计数）
// ===========================================================================
TEST(Filter, NegationOnlySkipsDataBlocks) {
  MemEnv env;
  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < 64; ++i) {
    kv.emplace_back(IKey(Key(i), 1, kTypeValue), Val(i) + std::string(40, 'x'));
  }

  Options on;
  on.env = &env;
  on.bloom_bits = 10;
  on.block_size = 256;
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("on.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(on, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    ASSERT_GT(b.NumDataBlocks(), 1u);
  }
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(on, &env, "on.sst", &t).ok());
  ASSERT_EQ(Table::FilterState::kOk, t->filter_state());

  // ① 存在 key：filter 肯定 ⇒ **必须**读数据块（I49 的硬断言）。
  {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey(Key(10), kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kFound, r);
    EXPECT_EQ(rs.filter_checked, 1u);
    EXPECT_EQ(rs.filter_positive, 1u);
    EXPECT_EQ(rs.filter_negative, 0u);
    EXPECT_EQ(rs.data_blocks_read, 1u) << "filter 报「可能存在」时禁止跳过读取（阻断性缺陷）";
    EXPECT_EQ(rs.data_blocks_skipped_by_filter, 0u);
  }
  // ② 不存在的 key（在 key range 内）：filter 否定 ⇒ 0 数据块读。
  {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("k000031~absent", kMaxSequenceNumber)), &v, &r,
                            &rs)
                    .ok());
    EXPECT_EQ(TableGetResult::kNotFound, r);
    EXPECT_EQ(rs.filter_checked, 1u);
    EXPECT_EQ(rs.filter_negative, 1u);
    EXPECT_EQ(rs.data_blocks_read, 0u) << "filter 否定必须省掉数据块读";
    EXPECT_EQ(rs.data_blocks_skipped_by_filter, 1u);
  }
  // ③ 迭代器路径**绝不**使用 filter（§3.7 硬规则 4）。
  {
    ReadStats rs;
    std::unique_ptr<Iterator> it = t->NewIterator(&rs);
    size_t cnt = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++cnt;
    EXPECT_TRUE(it->status().ok());
    EXPECT_EQ(cnt, kv.size());
    EXPECT_EQ(rs.filter_checked, 0u) << "全量迭代必须读所有数据块，禁止使用 filter";
    EXPECT_GT(rs.data_blocks_read, 0u);
  }

  // ④ 无 filter 的文件：filter_unavailable 计数 + 必须读块。
  {
    Options off = on;
    off.bloom_bits = 0;
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("off.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(off, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_EQ(b.filter_bytes(), 0u);

    std::shared_ptr<Table> t2;
    ASSERT_TRUE(Table::Open(off, &env, "off.sst", &t2).ok());
    EXPECT_EQ(Table::FilterState::kAbsent, t2->filter_state());
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kFound;
    ASSERT_TRUE(t2->GetEntry(Slice(ManualLookupKey("k000031~absent", kMaxSequenceNumber)), &v, &r,
                             &rs)
                    .ok());
    EXPECT_EQ(TableGetResult::kNotFound, r);
    EXPECT_EQ(rs.filter_unavailable, 1u);
    EXPECT_EQ(rs.filter_checked, 0u);
    EXPECT_EQ(rs.data_blocks_read, 1u) << "不可用 ⇒ 必须按「可能存在」处理并真的读块";
  }

  // ⑤ DB 级（design 指定的 seam）：GetReadStats 上能看到 filter 真的省了块读。
  {
    MemEnv denv;
    PersistentDBImpl* impl = OpenPersistent(&denv, 8 * 1024 * 1024, 10, "/dbf", 512);
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i) + std::string(60, 'y')).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
    ASSERT_EQ(impl->registered_file_numbers().size(), 1u);
    const DbReadStats b = impl->GetReadStats();
    std::string v;
    ASSERT_TRUE(db->Get("k000101~absent", &v).IsNotFound());
    const DbReadStats a = impl->GetReadStats();
    EXPECT_GT(DeltaOf(a.filter_checked, b.filter_checked), 0u) << "filter 必须真的被查询";
    EXPECT_GT(DeltaOf(a.data_blocks_skipped_by_filter, b.data_blocks_skipped_by_filter), 0u)
        << "data_blocks_skipped_by_filter 必须真的增长（§10.3 反空绿）";
    EXPECT_EQ(DeltaOf(a.data_blocks_read, b.data_blocks_read), 0u)
        << "被 filter 否定的查询不得读数据块";
    ASSERT_TRUE(db->Get(Key(101), &v).ok()) << "存在 key 仍必须可读（零假阴性）";
    EXPECT_EQ(Val(101) + std::string(60, 'y'), v);
  }
}

// ===========================================================================
// M5-A06 metaindex 注册/缺失 + 不升版本 + 同名重复 ⇒ kCorrupt
// ===========================================================================
TEST(Filter, MetaIndexRegistrationAndFormatVersion) {
  MemEnv env;
  Options on;
  on.env = &env;
  on.bloom_bits = 10;
  Options off = on;
  off.bloom_bits = 0;

  const std::vector<std::pair<std::string, std::string>> kv = {
      {IKey("a", 1, kTypeValue), "A"},
      {IKey("b", 1, kTypeValue), "B"},
  };

  // ① bloom_bits=10：写出 filter 块 + metaindex 注册。
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("on.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(on, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_GT(b.filter_bytes(), 0u);
    EXPECT_GT(b.filter_num_filters(), 0u);

    std::shared_ptr<Table> t;
    ReadStats os;
    ASSERT_TRUE(Table::Open(on, &env, "on.sst", &t, nullptr, nullptr, &os).ok());
    EXPECT_EQ(Table::FilterState::kOk, t->filter_state());
    EXPECT_TRUE(t->has_filter());
    EXPECT_GT(t->filter_bytes(), 0u);
    EXPECT_EQ(t->unknown_metaindex_entries(), 0u);
    EXPECT_TRUE(t->unknown_metaindex_names().empty());
    EXPECT_EQ(os.filter_blocks_read, 1u);
    EXPECT_EQ(os.filter_bytes_read, t->filter_bytes());
    // 不升 kTableFormatVersion：footer.version 仍为 1（M5-C3）。
    const std::string bytes = env.Contents("on.sst");
    ASSERT_GE(bytes.size(), kFooterSize);
    EXPECT_EQ(DecodeFixed32(bytes.data() + bytes.size() - kFooterSize + 4), 1u);
    EXPECT_EQ(kTableFormatVersion, 1u);
  }
  // ② bloom_bits=0：不写 filter 块，metaindex 保持 M3 的空表。
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("off.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(off, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_EQ(b.filter_bytes(), 0u);
    EXPECT_EQ(b.filter_num_filters(), 0u);
    std::shared_ptr<Table> t;
    ReadStats os;
    ASSERT_TRUE(Table::Open(off, &env, "off.sst", &t, nullptr, nullptr, &os).ok());
    EXPECT_EQ(Table::FilterState::kAbsent, t->filter_state());
    EXPECT_FALSE(t->has_filter());
    EXPECT_EQ(t->filter_bytes(), 0u);
    EXPECT_EQ(t->unknown_metaindex_entries(), 0u);
    EXPECT_EQ(os.filter_blocks_read, 0u);
  }
  // ③ 空表（num_entries==0）：不写 filter 块（§3.2 末段）。
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("empty.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(on, f.get());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_EQ(b.filter_bytes(), 0u) << "空表不写 filter（metaindex 与 M3 逐字一致）";
    std::shared_ptr<Table> t;
    ASSERT_TRUE(Table::Open(on, &env, "empty.sst", &t).ok());
    EXPECT_EQ(Table::FilterState::kAbsent, t->filter_state());
  }
  // ④ 同名重复注册 ⇒ kCorrupt（§3.4「最多一条」），Open 仍成功、读结果仍正确。
  {
    MemEnv e2;
    const FilterPolicy* p = NewBuiltinBloomPolicy(10);
    FilterBlockBuilder fb(p);
    fb.StartBlock(0);
    fb.AddKey(Slice("a"));
    const std::string pl = fb.Finish().ToString();
    RawTable rt;
    rt.Build(kv, 4096, &pl, kBlockTypeFilter, nullptr, /*dup_filter_entry=*/true, {});
    e2.SetContents("dup.sst", rt.bytes);
    std::shared_ptr<Table> t;
    ReadStats os;
    ASSERT_TRUE(Table::Open(on, &e2, "dup.sst", &t, nullptr, nullptr, &os).ok());
    EXPECT_EQ(Table::FilterState::kCorrupt, t->filter_state());
    EXPECT_EQ(os.filter_corrupt, 1u);
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("a", kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kFound, r);
    EXPECT_EQ("A", v);
  }
  // ⑤ filter name 以外的未知 metaindex name 仍走 M3 的「只计数不报错」。
  {
    MemEnv e3;
    BlockHandle dummy;
    dummy.offset = 0;
    dummy.size = 16;
    RawTable rt;
    rt.Build(kv, 4096, nullptr, kBlockTypeFilter, nullptr, false,
             {{"some.unknown.meta", HandleBytes(dummy)}});
    e3.SetContents("unknown.sst", rt.bytes);
    std::shared_ptr<Table> t;
    ASSERT_TRUE(Table::Open(on, &e3, "unknown.sst", &t).ok());
    EXPECT_EQ(t->unknown_metaindex_entries(), 1u);
    ASSERT_EQ(t->unknown_metaindex_names().size(), 1u);
    EXPECT_EQ(t->unknown_metaindex_names()[0], "some.unknown.meta");
    EXPECT_EQ(Table::FilterState::kAbsent, t->filter_state());
  }
}

// ===========================================================================
// M5-A07 旧文件（无 filter）降级 + 反向（bloom_bits=0 也读旧 filter）
// ===========================================================================
TEST(Filter, OldFileWithoutFilterDegrades) {
  MemEnv env;
  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < 32; ++i) kv.emplace_back(IKey(Key(i), 1, kTypeValue), Val(i));
  Options off;
  off.env = &env;
  off.bloom_bits = 0;
  off.block_size = 256;
  Options on = off;
  on.bloom_bits = 10;

  // ① 表级：bloom_bits=0 建，bloom_bits=10 开 ⇒ kAbsent，全部 key 可读，filter_unavailable 计数。
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("old.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(off, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_EQ(b.filter_bytes(), 0u);
  }
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(on, &env, "old.sst", &t).ok());
  EXPECT_EQ(Table::FilterState::kAbsent, t->filter_state());
  for (int i = 0; i < 32; ++i) {
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey(Key(i), kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kFound, r) << "旧文件必须照常读（M5:35 降级纪律）";
    EXPECT_EQ(Val(i), v);
    EXPECT_GT(rs.filter_unavailable, 0u) << "降级不得静默（§3.7 硬规则 6）";
    EXPECT_EQ(rs.filter_checked, 0u);
  }

  // ② 反向（C7(c)）：bloom_bits=10 建的表用 bloom_bits=0 打开 ⇒ filter 仍被解析并使用。
  {
    MemEnv env2;
    Options on2 = on;
    on2.env = &env2;
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env2.NewWritableFile("newer.sst", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(on2, f.get());
    for (const auto& e : kv) ASSERT_TRUE(b.Add(Slice(e.first), Slice(e.second)).ok());
    ASSERT_TRUE(b.Finish().ok());
    ASSERT_TRUE(f->Close().ok());
    EXPECT_GT(b.filter_bytes(), 0u);

    Options reader = on2;
    reader.bloom_bits = 0;   // 读侧关闭：必须**仍然**解析文件里的 filter
    std::shared_ptr<Table> t2;
    ASSERT_TRUE(Table::Open(reader, &env2, "newer.sst", &t2).ok());
    EXPECT_EQ(Table::FilterState::kOk, t2->filter_state())
        << "reader 是否解析 filter 与 Options::bloom_bits 无关（M5-C7(c)）";
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kFound;
    ASSERT_TRUE(t2->GetEntry(Slice(ManualLookupKey("k000010~absent", kMaxSequenceNumber)), &v, &r,
                             &rs)
                    .ok());
    EXPECT_EQ(TableGetResult::kNotFound, r);
    EXPECT_EQ(rs.filter_checked, 1u) << "bloom_bits=0 的 reader 仍必须使用文件里的 filter";
    EXPECT_EQ(rs.data_blocks_read, 0u);
  }

  // ③ DB 级：bloom_bits=0 建的库，用 bloom_bits=10 重开仍能读全部 key。
  {
    MemEnv denv;
    {
      PersistentDBImpl* impl = OpenPersistent(&denv, 8 * 1024 * 1024, 0, "/db7");
      ASSERT_NE(nullptr, impl);
      std::unique_ptr<DB> db(impl);
      for (int i = 0; i < 100; ++i) ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
      ASSERT_TRUE(impl->ForceFlushForTest().ok());
    }
    PersistentDBImpl* impl = OpenPersistent(&denv, 8 * 1024 * 1024, 10, "/db7");
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    for (int i = 0; i < 100; ++i) {
      std::string v;
      ASSERT_TRUE(db->Get(Key(i), &v).ok()) << "旧库（无 filter）必须全部可读：key=" << Key(i);
      EXPECT_EQ(Val(i), v);
    }
    std::string v;
    ASSERT_TRUE(db->Get("k000050~absent", &v).IsNotFound());
    EXPECT_GT(impl->GetReadStats().filter_unavailable, 0u);
  }
}

// ===========================================================================
// M5-A08 filter 损坏降级（handle 越界 / 过短 / type 错 / CRC 坏 / payload 结构坏）
// ===========================================================================
TEST(Filter, CorruptFilterDegradesButReadsCorrect) {
  const std::vector<std::pair<std::string, std::string>> kv = {
      {IKey("a", 1, kTypeValue), "A"},
      {IKey("b", 1, kTypeValue), "B"},
      {IKey("c", 1, kTypeValue), "C"},
  };
  const FilterPolicy* p = NewBuiltinBloomPolicy(10);
  FilterBlockBuilder fb(p);
  fb.StartBlock(0);
  fb.AddKey(Slice("a"));
  fb.AddKey(Slice("b"));
  fb.AddKey(Slice("c"));
  const std::string good = fb.Finish().ToString();
  ASSERT_GT(PayloadN(good), 0u);

  struct Case {
    std::string name;
    std::string payload;          // 非空 ⇒ 用这个畸形 payload 建表
    uint8_t block_type = kBlockTypeFilter;
    bool override_handle = false;
    BlockHandle handle_override;
  };

  std::string off0_nonzero = good;
  PutPayloadOffset(&off0_nonzero, 0, 4);   // 校验 5：offset[0] 必须为 0
  std::string len1 = MakeMultiBucketFilterPayload({{"alpha"}, {"beta"}}, p);
  ASSERT_GE(PayloadN(len1), 2u);
  PutPayloadOffset(&len1, 1, PayloadArrayOffset(len1) - 1);   // 制造一个 len==1 的桶
  std::string n_zero_bad(8, '\0');
  n_zero_bad[0] = 1;   // n==0 但 array_offset != 0 ⇒ 校验 4 失败
  std::string garbage(16, static_cast<char>(0xff));

  std::vector<Case> cases;
  cases.push_back({"offset0_nonzero", off0_nonzero, kBlockTypeFilter, false, {}});
  cases.push_back({"len1_bucket", len1, kBlockTypeFilter, false, {}});
  cases.push_back({"n_zero_bad_array_offset", n_zero_bad, kBlockTypeFilter, false, {}});
  cases.push_back({"garbage_payload", garbage, kBlockTypeFilter, false, {}});
  cases.push_back({"wrong_block_type", good, kBlockTypeData, false, {}});
  {
    Case c;
    c.name = "handle_out_of_bounds";
    c.payload = good;
    c.block_type = kBlockTypeFilter;
    c.override_handle = true;
    c.handle_override.offset = 1000000;   // > metaindex.offset ⇒ 布局校验失败
    c.handle_override.size = 32;
    cases.push_back(c);
  }
  {
    Case c;
    c.name = "handle_too_short";
    c.payload = good;
    c.block_type = kBlockTypeFilter;
    c.override_handle = true;
    c.handle_override.offset = 0;
    c.handle_override.size = 10;   // < kBlockOverhead + kBlockMinPayload = 17
    cases.push_back(c);
  }

  for (const Case& c : cases) {
    MemEnv env;
    Options o;
    o.bloom_bits = 10;
    o.env = &env;
    RawTable rt;
    const BlockHandle* ov = c.override_handle ? &c.handle_override : nullptr;
    rt.Build(kv, 4096, &c.payload, c.block_type, ov, false, {});
    env.SetContents("c.sst", rt.bytes);

    std::shared_ptr<Table> t;
    ReadStats os;
    const Status s = Table::Open(o, &env, "c.sst", &t, nullptr, nullptr, &os);
    ASSERT_TRUE(s.ok()) << "case " << c.name << "：filter 损坏**不得**让 Open 失败（M5:35）："
                        << s.ToString();
    EXPECT_EQ(Table::FilterState::kCorrupt, t->filter_state()) << "case " << c.name;
    EXPECT_EQ(os.filter_corrupt, 1u) << "case " << c.name << "（计数 filter_corrupt）";
    for (const auto& e : kv) {
      Slice user;
      SequenceNumber sq = 0;
      ValueType ty = kTypeValue;
      ASSERT_TRUE(ParseInternalKey(Slice(e.first), &user, &sq, &ty));
      ReadStats rs;
      std::string v;
      TableGetResult r = TableGetResult::kNotFound;
      ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey(user.ToString(), kMaxSequenceNumber)), &v, &r,
                              &rs)
                      .ok())
          << "case " << c.name;
      EXPECT_EQ(TableGetResult::kFound, r) << "case " << c.name;
      EXPECT_EQ(e.second, v) << "case " << c.name;
      EXPECT_EQ(rs.filter_unavailable, 1u) << "case " << c.name;
    }
  }
}

// ===========================================================================
// M5-B09（确定性内核）：对**带 filter 的完整文件**做单字节翻转扫描
//   判据：① 任何翻转都不得产生「静默假阴性」（存在的 key 被读成 kNotFound）；
//        ② 任何翻转都不得产生「静默错值」；
//        ③ 扫描必须真的命中 filter 块并被降级（否则本用例是空绿）。
//   这条同时证明 E3（filter 块 CRC 始终校验）与 I49（否定只用于 skip、肯定必须读）联合有效。
// ===========================================================================
TEST(Filter, FilterDamageScanNoSilentWrongValue) {
  MemEnv env;
  Options opts;
  opts.env = &env;
  opts.bloom_bits = 10;
  opts.block_size = 512;
  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < 50; ++i) {
    kv.emplace_back(IKey(Key(i), 1, kTypeValue), Val(i) + std::string(20, 'd'));
  }
  WritableFile* raw = nullptr;
  ASSERT_TRUE(env.NewWritableFile("dmg.sst", &raw).ok());
  std::unique_ptr<WritableFile> wf(raw);
  TableBuilder builder(opts, wf.get());
  for (const auto& e : kv) ASSERT_TRUE(builder.Add(Slice(e.first), Slice(e.second)).ok());
  ASSERT_TRUE(builder.Finish().ok());
  ASSERT_TRUE(wf->Close().ok());
  ASSERT_GT(builder.filter_bytes(), 0u);
  ASSERT_GT(builder.NumDataBlocks(), 1u);

  std::shared_ptr<Table> probe;
  ASSERT_TRUE(Table::Open(opts, &env, "dmg.sst", &probe).ok());
  const std::string smallest = probe->FirstInternalKey();
  const std::string largest = probe->LastInternalKey();
  probe.reset();

  const std::string original = env.Contents("dmg.sst");
  ASSERT_GT(original.size(), 100u);

  uint64_t silent_false_negative = 0;
  uint64_t silent_wrong_value = 0;
  uint64_t detected_corruption = 0;
  uint64_t filter_degraded = 0;
  uint64_t scanned = 0;

  for (size_t i = 0; i < original.size(); ++i) {
    std::string bad = original;
    bad[i] = static_cast<char>(static_cast<unsigned char>(bad[i]) ^ 0x01u);
    env.SetContents("dmg.sst", bad);
    ++scanned;

    std::shared_ptr<Table> t;
    const Status open = Table::Open(opts, &env, "dmg.sst", &t, &smallest, &largest);
    if (!open.ok()) {
      ++detected_corruption;
      continue;
    }
    if (t->filter_state() == Table::FilterState::kCorrupt) ++filter_degraded;
    for (const auto& e : kv) {
      Slice user;
      SequenceNumber sq = 0;
      ValueType ty = kTypeValue;
      ASSERT_TRUE(ParseInternalKey(Slice(e.first), &user, &sq, &ty));
      ReadStats rs;
      std::string v;
      TableGetResult r = TableGetResult::kNotFound;
      const Status g =
          t->GetEntry(Slice(ManualLookupKey(user.ToString(), kMaxSequenceNumber)), &v, &r, &rs);
      if (g.IsCorruption()) {
        ++detected_corruption;
        break;
      }
      if (!g.ok()) {
        ++silent_wrong_value;   // 非 Corruption 的错误也算「没检出」
        break;
      }
      if (r == TableGetResult::kNotFound) {
        ++silent_false_negative;   // 存在 key 被读成不存在 ⇒ 丢数据级
        break;
      }
      if (r == TableGetResult::kFound && v != e.second) {
        ++silent_wrong_value;
        break;
      }
    }
  }
  env.SetContents("dmg.sst", original);

  std::printf("M5_FILTER_DAMAGE_CASES %llu\n", static_cast<unsigned long long>(scanned));
  std::printf("M5_FILTER_SILENT_FALSE_NEGATIVE %llu\n",
              static_cast<unsigned long long>(silent_false_negative));
  std::printf("M5_FILTER_SILENT_WRONG_VALUE %llu\n",
              static_cast<unsigned long long>(silent_wrong_value));
  std::printf("M5_FILTER_DAMAGE_DETECTED %llu\n",
              static_cast<unsigned long long>(detected_corruption));
  std::printf("M5_FILTER_DAMAGE_FILTER_DEGRADED %llu\n",
              static_cast<unsigned long long>(filter_degraded));
  std::printf("[FILTER_DAMAGE_OK]\n");
  std::fflush(stdout);

  EXPECT_EQ(silent_false_negative, 0u) << "任何单字节翻转都不得造成静默假阴性（I47/E3）";
  EXPECT_EQ(silent_wrong_value, 0u) << "任何单字节翻转都不得造成静默错值";
  EXPECT_GT(detected_corruption, 0u) << "数据块/索引/footer 区域的翻转必须被检出（防空绿）";
  EXPECT_GT(filter_degraded, 0u) << "扫描必须真的命中 filter 块并把它降级（防空绿）";
}

// ===========================================================================
// M5-A09 verify_checksums=false 与 filter：filter 块 CRC **始终**校验（E3）
// ===========================================================================
TEST(Filter, FilterCrcAlwaysVerified) {
  const std::vector<std::pair<std::string, std::string>> kv = {
      {IKey("a", 1, kTypeValue), "A"},
      {IKey("b", 1, kTypeValue), "B"},
  };
  const FilterPolicy* p = NewBuiltinBloomPolicy(10);
  FilterBlockBuilder fb(p);
  fb.StartBlock(0);
  fb.AddKey(Slice("a"));
  fb.AddKey(Slice("b"));
  const std::string good = fb.Finish().ToString();

  for (int verify = 0; verify <= 1; ++verify) {
    MemEnv env;
    Options o;
    o.bloom_bits = 10;
    o.verify_checksums = (verify == 1);
    o.env = &env;
    RawTable rt;
    rt.Build(kv, 4096, &good, kBlockTypeFilter, nullptr, false, {});
    // 只翻转 filter 块 payload 的一个 bit，**不**重算 CRC。
    const size_t poff = static_cast<size_t>(rt.filter_handle.offset) + kBlockHeaderSize;
    rt.bytes[poff] = static_cast<char>(static_cast<unsigned char>(rt.bytes[poff]) ^ 0x01u);
    env.SetContents("v.sst", rt.bytes);

    std::shared_ptr<Table> t;
    ReadStats os;
    ASSERT_TRUE(Table::Open(o, &env, "v.sst", &t, nullptr, nullptr, &os).ok())
        << "verify_checksums=" << verify;
    EXPECT_EQ(Table::FilterState::kCorrupt, t->filter_state())
        << "E3：filter 块 CRC 必须**始终**校验（verify_checksums=" << verify << "）";
    EXPECT_EQ(os.filter_corrupt, 1u) << "verify_checksums=" << verify;
    ReadStats rs;
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    ASSERT_TRUE(t->GetEntry(Slice(ManualLookupKey("b", kMaxSequenceNumber)), &v, &r, &rs).ok());
    EXPECT_EQ(TableGetResult::kFound, r);
    EXPECT_EQ("B", v);
  }

  // 反向对照：**数据块** CRC 仍受 verify_checksums 控制（M3 语义不被 M5 改动）。
  // 手法：翻转第一个数据块的 CRC 尾字段（payload 不变 ⇒ 结构校验仍通过）。
  {
    const std::vector<std::pair<std::string, std::string>> big = {
        {IKey("aa", 1, kTypeValue), std::string(64, 'z')},
        {IKey("bb", 1, kTypeValue), std::string(64, 'z')},
    };
    for (int verify = 0; verify <= 1; ++verify) {
      MemEnv env;
      Options o;
      o.bloom_bits = 0;   // 本对照只关心数据块 CRC
      o.verify_checksums = (verify == 1);
      o.env = &env;
      RawTable rt;
      rt.Build(big, 4096, nullptr, kBlockTypeFilter, nullptr, false, {});
      ASSERT_EQ(rt.data_handles.size(), 1u);
      const BlockHandle dh = rt.data_handles[0];
      const size_t crc_pos = static_cast<size_t>(dh.offset + dh.size - 1);
      rt.bytes[crc_pos] = static_cast<char>(static_cast<unsigned char>(rt.bytes[crc_pos]) ^ 0x01u);
      env.SetContents("d.sst", rt.bytes);

      std::shared_ptr<Table> t;
      // 传 known_smallest/largest 以跳过 Open 里的「预读首块」（否则 Open 自己就会撞上坏 CRC）。
      ASSERT_TRUE(Table::Open(o, &env, "d.sst", &t, &rt.smallest, &rt.largest).ok())
          << "verify_checksums=" << verify;
      ReadStats rs;
      std::string v;
      TableGetResult r = TableGetResult::kNotFound;
      const Status g = t->GetEntry(Slice(ManualLookupKey("aa", kMaxSequenceNumber)), &v, &r, &rs);
      if (verify == 1) {
        EXPECT_TRUE(g.IsCorruption()) << "verify_checksums=true ⇒ 数据块 CRC 必须检出";
        EXPECT_EQ(rs.crc_failed, 1u);
      } else {
        EXPECT_TRUE(g.ok()) << "verify_checksums=false ⇒ 数据块 CRC 不参与判定（M3 语义）";
        EXPECT_EQ(rs.crc_checked, 0u) << "关掉校验时不得统计 crc_checked";
        EXPECT_EQ(TableGetResult::kFound, r);
        EXPECT_EQ(std::string(64, 'z'), v);
      }
    }
  }
}

// ===========================================================================
// M5-A10 ≥3× 同轮开关对照：同一数据集、同一不存在 key 集合，bloom_bits=0 vs 10
// ===========================================================================
TEST(Filter, BlockReadReductionAtLeastThreeTimes) {
  const int kDataset = 20000;
  const int kQueries = 2000;

  MemEnv env_off;
  MemEnv env_on;
  PersistentDBImpl* off = OpenPersistent(&env_off, 8 * 1024 * 1024, 0, "/dboff", 4096);
  ASSERT_NE(nullptr, off);
  std::unique_ptr<DB> db_off(off);
  PersistentDBImpl* on = OpenPersistent(&env_on, 8 * 1024 * 1024, 10, "/dbon", 4096);
  ASSERT_NE(nullptr, on);
  std::unique_ptr<DB> db_on(on);

  // 数据集：固定种子的 16 字节 key（D7 的 `uniform` 分布），**刻意不用**「只差最低字节」的
  // 8B 递增 key —— 后者会让 D1 指定的 LevelDB 哈希退化（见 M5-A02 的
  // `M5_FILTER_FPR_STRUCTURED_PPM` 与报告里的负结果登记），那样测的是病态分布而不是 filter 本身。
  Rng rng(kHarnessSeed);
  std::vector<std::string> keys;
  keys.reserve(kDataset);
  for (int i = 0; i < kDataset; ++i) keys.push_back(RandomKey(&rng, 16));
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  ASSERT_GT(keys.size(), static_cast<size_t>(kQueries));

  const std::string value(100, 'v');
  for (const std::string& k : keys) {
    ASSERT_TRUE(db_off->Put(WriteOptions(), k, value).ok());
    ASSERT_TRUE(db_on->Put(WriteOptions(), k, value).ok());
  }
  ASSERT_TRUE(off->ForceFlushForTest().ok());
  ASSERT_TRUE(on->ForceFlushForTest().ok());
  ASSERT_EQ(off->registered_file_numbers().size(), 1u);
  ASSERT_EQ(on->registered_file_numbers().size(), 1u);

  // 同一查询集合、同一顺序：`key ‖ 'z'` 严格落在该 key 与其后继之间 ⇒ 在 key range 内但不存在。
  std::vector<std::string> queries;
  queries.reserve(kQueries);
  for (int i = 0; i < kQueries; ++i) {
    const size_t j = static_cast<size_t>(i) * (keys.size() - 1) / static_cast<size_t>(kQueries);
    queries.push_back(keys[j] + "z");
  }

  const DbReadStats before_off = off->GetReadStats();
  const DbReadStats before_on = on->GetReadStats();
  uint64_t false_negatives = 0;
  {
    std::string v;
    for (const std::string& q : queries) {
      if (!db_off->Get(q, &v).IsNotFound()) ++false_negatives;
    }
  }
  for (const std::string& q : queries) {
    std::string v;
    if (!db_on->Get(q, &v).IsNotFound()) ++false_negatives;
  }
  const DbReadStats after_off = off->GetReadStats();
  const DbReadStats after_on = on->GetReadStats();

  const uint64_t without = DeltaOf(after_off.data_blocks_read, before_off.data_blocks_read);
  const uint64_t with = DeltaOf(after_on.data_blocks_read, before_on.data_blocks_read);
  const double ratio =
      static_cast<double>(without) / static_cast<double>(std::max<uint64_t>(1, with));
  const uint64_t skipped =
      DeltaOf(after_on.data_blocks_skipped_by_filter, before_on.data_blocks_skipped_by_filter);

  std::printf("M5_FILTER_BLOCK_READS_WITHOUT %llu\n", static_cast<unsigned long long>(without));
  std::printf("M5_FILTER_BLOCK_READS_WITH %llu\n", static_cast<unsigned long long>(with));
  std::printf("M5_FILTER_BLOCK_READ_RATIO %.6f\n", ratio);
  std::printf("M5_FILTER_FALSE_NEGATIVE %llu\n", static_cast<unsigned long long>(false_negatives));
  std::printf("M5_FILTER_QUERIES %d\n", kQueries);
  std::printf("M5_FILTER_BLOCKS_SKIPPED %llu\n", static_cast<unsigned long long>(skipped));
  const uint64_t on_checked = DeltaOf(after_on.filter_checked, before_on.filter_checked);
  const uint64_t on_positive = DeltaOf(after_on.filter_positive, before_on.filter_positive);
  const uint64_t on_unavailable =
      DeltaOf(after_on.filter_unavailable, before_on.filter_unavailable);
  const uint64_t on_fpr_ppm =
      on_checked == 0 ? 0 : static_cast<uint64_t>(on_positive) * 1000000ull / on_checked;
  std::printf("M5_FILTER_ON_CHECKED %llu POSITIVE %llu UNAVAILABLE %llu POSITIVE_PPM %llu\n",
              static_cast<unsigned long long>(on_checked),
              static_cast<unsigned long long>(on_positive),
              static_cast<unsigned long long>(on_unavailable),
              static_cast<unsigned long long>(on_fpr_ppm));
  std::fflush(stdout);

  EXPECT_EQ(false_negatives, 0u) << "两个 DB 都必须零假阴性（全部查询都必须 kNotFound）";
  EXPECT_GE(without, 3u) << "without < 3：数据集/文件数太小，必须放大数据集，**不得**放行（§3.8）";
  EXPECT_GE(ratio, 3.0) << "同轮开关的块读次数比值必须 >= 3.0（M5-C5）";
  EXPECT_GT(skipped, 0u) << "filter 必须真的省了块读（§10.3 反空绿）";
  // 带 filter 一侧只允许剩下**误判**（每个空查询读到的块 = 该次 filter 报「可能存在」）：
  EXPECT_EQ(with, on_positive + on_unavailable)
      << "带 filter 时读到的数据块数必须恰好等于 filter 的肯定/不可用次数（口径自洽）";
  EXPECT_LT(on_positive, static_cast<uint64_t>(kQueries) / 10)
      << "代表分布上的误判率不应超过 10%（理论 ~0.82%；见 M5-A02 的结构性分布负结果）";

  std::printf("[FILTER_OK]\n");
  std::fflush(stdout);
}

// ===========================================================================
// M5-A18 多线程点查 + filter（读同一 TableCache 缓存的 Table）+ 计数不丢
// ===========================================================================
TEST(Filter, MultiThreadedGetNoRace) {
  const int kKeys = 500;
  const int kThreads = 4;
  const int kPerThread = 200;

  MemEnv env;
  PersistentDBImpl* impl = OpenPersistent(&env, 8 * 1024 * 1024, 10, "/dbmt", 512);
  ASSERT_NE(nullptr, impl);
  std::unique_ptr<DB> db(impl);
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i) + std::string(80, 'm')).ok());
  }
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  ASSERT_EQ(impl->registered_file_numbers().size(), 1u)
      << "本用例要求恰好 1 个 SST ⇒ filter_checked 的期望值才可精确表达";

  const DbReadStats before = impl->GetReadStats();
  std::atomic<int> ok_count{0};
  std::atomic<int> bad_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      std::string v;
      for (int j = 0; j < kPerThread; ++j) {
        const int idx = (t * 131 + j * 7) % kKeys;
        if (db->Get(Key(idx), &v).ok() && v == Val(idx) + std::string(80, 'm')) {
          ++ok_count;
        } else {
          ++bad_count;
        }
      }
    });
  }
  for (std::thread& th : threads) th.join();

  EXPECT_EQ(bad_count.load(), 0) << "并发点查必须全部读到正确值（零假阴性）";
  EXPECT_EQ(ok_count.load(), kThreads * kPerThread);
  const DbReadStats after = impl->GetReadStats();
  EXPECT_EQ(DeltaOf(after.filter_checked, before.filter_checked),
            static_cast<uint64_t>(kThreads * kPerThread))
      << "L34：并发下 filter 计数不得丢失（线程局部 delta + 持锁汇总）";
  EXPECT_EQ(DeltaOf(after.filter_negative, before.filter_negative), 0u)
      << "存在的 key 不可能被 filter 否定（否则就是假阴性）";
  EXPECT_EQ(DeltaOf(after.filter_positive, before.filter_positive),
            static_cast<uint64_t>(kThreads * kPerThread));
}

}  // namespace test
}  // namespace lsm
