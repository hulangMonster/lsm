// tests/memtable_test.cpp —— M1 A 组（跳表 / MemTable / 迭代器 / 冻结）+ B 组（压力边界）
//
// 用例名逐字对应 docs/m1-design.md §11 的 Skiplist.* / MemTable.* / Stress.*；
// 另有 DB::Open 的边界用例（前置文档 m1-prerequisites.md §5 指定：name 非空 → kNotSupported，
// options 非法 → kInvalidArgument，失败路径 *dbptr 必须为 nullptr，不产生半构造对象）。
//
// 口径说明（本文件即契约，M1.2 必须匹配）：
//   - MemTable::NewIterator() 是**内部序**迭代器：key() 返回 internal key、value() 返回 value，
//     多版本全部可见、tombstone 也出现（design §4.4/§8）；
//   - DB::NewIterator() 是**用户视图**迭代器：只出每个 user key 的最新版本，tombstone 不可见，
//     key() 返回 user key（design §4.4/§9）；
//   - 迭代器三态状态机（kBeforeFirst / kValid / kPastEnd）按 design §4.4 判定表断言，
//     不假设 LevelDB 的「方向翻转」语义；
//   - Skiplist::Stats::level_histogram[i] = 恰好高度 i+1 的节点数；
//   - 压力用例的容量必须显式放大（prerequisites §7.6），冻结语义单独在 Freeze* 用例验证。

#include "test_harness.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "util/arena.h"
#include "util/coding.h"

using namespace lsm;
using namespace lsm::test;

namespace {

long long ElapsedMs(const std::chrono::steady_clock::time_point& a,
                    const std::chrono::steady_clock::time_point& b) {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
}

// 与 multimap 模型逐条对账（key + value 全量比对；模型 key 唯一，故顺序即全序）。
void ExpectMatchesModel(const std::string& what,
                        const std::multimap<std::string, std::string>& model,
                        const std::vector<std::pair<std::string, std::string>>& got) {
  ASSERT_EQ(model.size(), got.size()) << what << ": 条数不一致";
  std::multimap<std::string, std::string>::const_iterator m = model.begin();
  for (size_t i = 0; i < got.size(); ++i, ++m) {
    EXPECT_EQ(m->first, got[i].first) << what << ": 第 " << i << " 条 key 不一致";
    EXPECT_EQ(m->second, got[i].second) << what << ": 第 " << i << " 条 value 不一致";
  }
}

// 用 Rng 做 Fisher–Yates（不用 <random>：分布实现跨平台不一致会破坏可复现性）。
void Shuffle(std::vector<std::string>* v, Rng* rng) {
  for (size_t i = v->size(); i > 1; --i) {
    const size_t j = rng->Uniform(static_cast<uint32_t>(i));
    std::swap((*v)[i - 1], (*v)[j]);
  }
}

}  // namespace

// ===========================================================================
// Skiplist（design §7）
// ===========================================================================
TEST(Skiplist, InsertContainsWithStdMultimap) {
  const int kN = 100000;
  Rng rng(kHarnessSeed);
  Arena arena;  // 跳表内 Slice 的生命周期由测试自己的 Arena 保证（prerequisites §7.5）
  Skiplist list(&arena, BytewiseComparator());

  std::multimap<std::string, uint64_t> model;
  std::vector<std::string> keys;
  keys.reserve(static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    // 随机长度 8..16：覆盖不同长度前缀关系
    const std::string k = RandomKey(&rng, 8 + static_cast<int>(rng.Uniform(9)));
    keys.push_back(k);
    model.emplace(k, static_cast<uint64_t>(i));
    list.Insert(ArenaStoreSlice(&arena, k));
  }
  ASSERT_EQ(static_cast<size_t>(kN), model.size())
      << "随机 key 出现碰撞，用例前提（key 唯一）被破坏，请换种子或加长 key";
  ASSERT_EQ(static_cast<size_t>(kN), list.GetStats().node_count)
      << "节点数必须等于插入次数（design §7.4：重复 key 也各自建节点）";

  // 全量对账：顺序 + 内容
  ExpectSkiplistMatchesMultimap("10 万条随机顺序插入", model, list);

  // Contains 覆盖全部插入的 key
  for (const std::string& k : keys) {
    EXPECT_TRUE(list.Contains(Slice(k))) << "插入后 Contains 必须为真";
  }
  // 必然不存在的 key：前缀 'A'(0x41) < 'a'(0x61)，不可能与生成集合相交
  for (int i = 0; i < 200; ++i) {
    const std::string absent = "A" + RandomKey(&rng, 8);
    EXPECT_FALSE(list.Contains(Slice(absent))) << "未插入的 key 不得 Contains";
  }
  // 边界：空 key 未插入
  EXPECT_FALSE(list.Contains(Slice("")));
  // 首个/末个 key 必须命中
  EXPECT_TRUE(list.Contains(Slice(model.begin()->first)));
  EXPECT_TRUE(list.Contains(Slice(model.rbegin()->first)));
}

TEST(Skiplist, RandomLayerDistribution) {
  const size_t kN = 100000;
  Arena arena;
  Skiplist list(&arena, BytewiseComparator());  // 固定种子（默认 0x9E3779B9，design §7.1）
  for (size_t i = 0; i < kN; ++i) {
    list.Insert(ArenaStoreSlice(&arena, KeyFromIndex(i)));
  }

  const Skiplist::Stats st = list.GetStats();
  ASSERT_EQ(kN, st.node_count);

  // 直方图口径：level_histogram[i] = 恰好高度 i+1 的节点数
  size_t hist_sum = 0;
  size_t level_sum_from_hist = 0;
  size_t ge2 = 0;
  size_t ge3 = 0;
  int max_height = 0;
  for (int i = 0; i < Skiplist::kMaxHeight; ++i) {
    hist_sum += st.level_histogram[i];
    level_sum_from_hist += static_cast<size_t>(i + 1) * st.level_histogram[i];
    if (i >= 1) ge2 += st.level_histogram[i];
    if (i >= 2) ge3 += st.level_histogram[i];
    if (st.level_histogram[i] > 0) max_height = i + 1;
  }
  EXPECT_EQ(kN, hist_sum) << "层高直方图之和必须等于节点数";
  EXPECT_EQ(st.level_sum, level_sum_from_hist) << "level_sum 必须等于直方图加权重";
  EXPECT_LE(static_cast<size_t>(max_height), static_cast<size_t>(Skiplist::kMaxHeight))
      << "随机层高越界（maxHeight 与数组长度必须一致，prerequisites §3）";
  EXPECT_GE(max_height, 2) << "10 万条不可能全部落在 1 层";

  const double mean = static_cast<double>(st.level_sum) / static_cast<double>(st.node_count);
  const double p2 = static_cast<double>(ge2) / static_cast<double>(kN);
  const double p3 = static_cast<double>(ge3) / static_cast<double>(kN);
  // p = 1/4 → E[h] = 1/(1-p) = 4/3 ≈ 1.3333；P(h>=2) = 0.25；P(h>=3) = 0.0625（design §7.1）
  EXPECT_GE(mean, 1.28) << "实测 E[height] 偏低：mean=" << mean << "（期望 4/3）";
  EXPECT_LE(mean, 1.39) << "实测 E[height] 偏高：mean=" << mean << "（期望 4/3）";
  EXPECT_GE(p2, 0.23) << "P(h>=2)=" << p2;
  EXPECT_LE(p2, 0.27) << "P(h>=2)=" << p2;
  EXPECT_GE(p3, 0.055) << "P(h>=3)=" << p3;
  EXPECT_LE(p3, 0.07) << "P(h>=3)=" << p3;

  std::cout << "[   INFO   ] Skiplist.RandomLayerDistribution: node_count=" << st.node_count
            << " mean_height=" << mean << " P(h>=2)=" << p2 << " P(h>=3)=" << p3
            << " max_height=" << max_height << std::endl;
  RecordProperty("node_count", std::to_string(st.node_count));
  RecordProperty("mean_height_x10000", std::to_string(static_cast<long long>(mean * 10000)));
  RecordProperty("max_height", std::to_string(max_height));
}

TEST(Skiplist, IteratorForwardBackward) {
  const int kN = 5000;
  Rng rng(kHarnessSeed + 7);
  Arena arena;
  Skiplist list(&arena, BytewiseComparator());
  std::multimap<std::string, uint64_t> model;
  for (int i = 0; i < kN; ++i) {
    const std::string k = RandomKey(&rng, 10);
    model.emplace(k, static_cast<uint64_t>(i));
    list.Insert(ArenaStoreSlice(&arena, k));
  }

  const std::vector<std::string> fwd = SkiplistKeysForward(list);
  const std::vector<std::string> bwd = SkiplistKeysBackward(list);
  ASSERT_EQ(static_cast<size_t>(kN), fwd.size());
  ASSERT_EQ(fwd.size(), bwd.size());
  const std::vector<std::string> bwd_reversed(bwd.rbegin(), bwd.rend());
  EXPECT_EQ(fwd, bwd_reversed) << "SeekToFirst/Next 与 SeekToLast/Prev 必须互为逆序";
  ExpectSkiplistMatchesMultimap("双向遍历", model, list);

  std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
  const std::string first = fwd.front();
  const std::string last = fwd.back();
  EXPECT_LT(first, last);

  // Seek 落在「第一个 >= target」的条目；越界（无更大条目）→ !Valid
  const std::string probes[] = {std::string(""), std::string("a"), first, last,
                                std::string("m"), std::string("zzzzzzzzzzzzz")};
  for (const std::string& target : probes) {
    it->Seek(Slice(target));
    std::multimap<std::string, uint64_t>::const_iterator lb = model.lower_bound(target);
    if (lb == model.end()) {
      EXPECT_FALSE(it->Valid()) << "Seek(" << target << ") 无更大条目 → 必须 !Valid";
    } else {
      ASSERT_TRUE(it->Valid()) << "Seek(" << target << ") 必须命中 " << lb->first;
      EXPECT_EQ(lb->first, it->key().ToString()) << "Seek(" << target << ") 落点错误";
    }
  }
  // Seek 到超过最大 key → !Valid；再 Next 仍 !Valid
  std::string beyond = last;
  beyond.append(1, static_cast<char>(0xFF));
  ASSERT_GT(beyond, last);
  it->Seek(Slice(beyond));
  EXPECT_FALSE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  // SeekToLast 恢复
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(last, it->key().ToString());
  it->Next();
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(last, it->key().ToString());
  // 反向走到底：SeekToLast + Prev 序列是正向序列的逆序
  std::vector<std::string> back;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    back.push_back(it->key().ToString());
  }
  EXPECT_EQ(bwd, back);
  // 首元素再 Prev 不得越界（跳表迭代器只允许在 Valid 时 Prev；首元素 Prev 后必须 !Valid 且不崩）
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(first, it->key().ToString());
  // [#4 评审建议 5] 原文这段注释承诺「首元素再 Prev」，但代码里并没有 Prev —— 补齐，
  // 让注释承诺的边界真的被验证（首元素 Prev 后必须 !Valid，且再 Prev 不得崩）。
  it->Prev();
  EXPECT_FALSE(it->Valid());
  it->Prev();
  EXPECT_FALSE(it->Valid());
}

TEST(Skiplist, DuplicateKeys) {
  Arena arena;
  Skiplist list(&arena, BytewiseComparator());
  const int kDup = 5;
  for (int i = 0; i < kDup; ++i) {
    list.Insert(ArenaStoreSlice(&arena, "dup"));
  }
  list.Insert(ArenaStoreSlice(&arena, "aaa"));
  list.Insert(ArenaStoreSlice(&arena, "zzz"));

  // 多重集语义：重复 key 全部保留、Contains 为真（design §7.4）
  EXPECT_TRUE(list.Contains(Slice("dup")));
  EXPECT_FALSE(list.Contains(Slice("du")));
  EXPECT_FALSE(list.Contains(Slice("dupp")));
  const Skiplist::Stats st = list.GetStats();
  EXPECT_EQ(static_cast<size_t>(kDup) + 2u, st.node_count);

  const std::vector<std::string> fwd = SkiplistKeysForward(list);
  ASSERT_EQ(static_cast<size_t>(kDup) + 2u, fwd.size());
  EXPECT_EQ("aaa", fwd.front());
  EXPECT_EQ("zzz", fwd.back());
  EXPECT_EQ(static_cast<size_t>(kDup),
            static_cast<size_t>(std::count(fwd.begin(), fwd.end(), std::string("dup"))))
      << "重复条目必须全部可见";
  // 相同 key 在遍历中必须相邻
  for (size_t i = 2; i + 1 < fwd.size(); ++i) {
    EXPECT_LE(fwd[i - 1], fwd[i]);
  }
  EXPECT_EQ(fwd.size(), SkiplistKeysBackward(list).size());
  // [#4 评审建议 6] 原文只比条数：补上重复段内的 Prev 顺序断言（必须逐节点回退、不是跨过整段）
  {
    std::unique_ptr<Skiplist::Iterator> dup_it(list.NewIterator());
    std::vector<std::string> back;
    for (dup_it->SeekToLast(); dup_it->Valid(); dup_it->Prev()) {
      back.push_back(dup_it->key().ToString());
    }
    ASSERT_EQ(fwd.size(), back.size());
    EXPECT_EQ(std::vector<std::string>(fwd.rbegin(), fwd.rend()), back)
        << "反向遍历必须是正向遍历的严格逆序（含重复段内部）";
  }

  // MemTable 层不会出现重复：internal key 含 sequence（design §7.4）
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  for (int i = 1; i <= kDup; ++i) {
    AddEntry(&mem, static_cast<SequenceNumber>(i), kTypeValue, "dup", "v");
  }
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(static_cast<size_t>(kDup), entries.size());
  std::set<std::string> distinct;
  for (const InternalEntry& e : entries) {
    EXPECT_TRUE(distinct.insert(e.internal_key).second) << "internal key 必须唯一（I1）";
  }
}

// ===========================================================================
// MemTable（design §8）
// ===========================================================================
TEST(MemTable, PutGetRoundTrip) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  EXPECT_EQ(0u, mem.NumEntries());
  EXPECT_FALSE(mem.IsFrozen());
  EXPECT_EQ(&icmp, &mem.internal_comparator());
  // ApproximateMemoryUsage = arena_.MemoryUsage() + sizeof(MemTable)（design §8.3）
  EXPECT_GE(mem.ApproximateMemoryUsage(), sizeof(MemTable));

  AddEntry(&mem, 1, kTypeValue, "k1", "v1");
  std::string value;
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k1", kMaxSequenceNumber), &value));
  EXPECT_EQ("v1", value);

  // 未命中：只断言返回码（design §8.2 未规定 *value 内容，不对未定义行为做断言）
  {
    std::string untouched;
    EXPECT_EQ(MemTable::GetResult::kNotFound,
              mem.Get(BuildLookupKey("nope", kMaxSequenceNumber), &untouched));
  }
  // 前缀不算命中
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k11", kMaxSequenceNumber), &value));

  // 空 value 合法（protocol §8）
  AddEntry(&mem, 2, kTypeValue, "empty", "");
  {
    std::string v;
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("empty", kMaxSequenceNumber), &v));
    EXPECT_TRUE(v.empty());
  }
  // 1 MiB value 必须可用（protocol §8 / prerequisites §5）
  const std::string big(1024 * 1024, 'B');
  AddEntry(&mem, 3, kTypeValue, "big", big);
  {
    std::string v;
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("big", kMaxSequenceNumber), &v));
    EXPECT_EQ(big.size(), v.size());
    EXPECT_EQ(big, v);
  }
  EXPECT_EQ(3u, mem.NumEntries());
  EXPECT_FALSE(mem.IsFrozen());

  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(3u, entries.size());
  EXPECT_EQ("big", entries[0].user_key);
  EXPECT_EQ("empty", entries[1].user_key);
  EXPECT_EQ("k1", entries[2].user_key);
  EXPECT_EQ("v1", entries[2].value);
}

TEST(MemTable, OverwriteNewestWins) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  const int kVersions = 20;
  for (int i = 1; i <= kVersions; ++i) {
    AddEntry(&mem, static_cast<SequenceNumber>(i), kTypeValue, "k", "v" + std::to_string(i));
  }

  std::string value;
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
  EXPECT_EQ("v" + std::to_string(kVersions), value);
  // 读快照由调用方决定（protocol §6.2）：历史快照读到历史值
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 7), &value));
  EXPECT_EQ("v7", value);
  // snapshot=0：没有任何版本可见
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k", 0), &value));

  // 内部序：N 个版本，sequence 严格降序（I2）
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(static_cast<size_t>(kVersions), entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ("k", entries[i].user_key);
    EXPECT_EQ(static_cast<SequenceNumber>(kVersions - static_cast<int>(i)), entries[i].seq);
    EXPECT_EQ("v" + std::to_string(kVersions - static_cast<int>(i)), entries[i].value);
  }
  EXPECT_EQ(static_cast<size_t>(kVersions), mem.NumEntries());
}

TEST(MemTable, DeleteThenNotFound) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  AddEntry(&mem, 1, kTypeValue, "k", "v1");
  AddEntry(&mem, 2, kTypeValue, "k2", "v2");
  // Delete 不存在的 key 合法（写 tombstone，返回 OK，prerequisites §5）
  AddEntry(&mem, 3, kTypeDeletion, "ghost", "");
  AddEntry(&mem, 4, kTypeDeletion, "k", "");

  std::string value;
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
  EXPECT_TRUE(value.empty()) << "kDeleted 必须清空 value（design §8.2）";
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k2", kMaxSequenceNumber), &value));
  EXPECT_EQ("v2", value);
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("ghost", kMaxSequenceNumber), &value));
  // tombstone 屏蔽更小 sequence 的同 key 值（I4）：历史快照仍能看到删除前的值
  value.clear();
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 3), &value));
  EXPECT_EQ("v1", value);
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k", 4), &value));

  // tombstone 计入条目数（design §8.3）
  EXPECT_EQ(4u, mem.NumEntries());
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(4u, entries.size());
  EXPECT_EQ(static_cast<int>(kTypeDeletion), static_cast<int>(entries[0].type));  // ghost@3
  EXPECT_EQ("ghost", entries[0].user_key);
  EXPECT_TRUE(entries[0].value.empty());
  EXPECT_EQ(static_cast<int>(kTypeDeletion), static_cast<int>(entries[1].type));  // k@4
  EXPECT_EQ("k", entries[1].user_key);

  // DB 层：Delete 后 Get → kNotFound，且错误信息可定位
  std::unique_ptr<DB> db(OpenMemoryDB());
  ASSERT_TRUE(db != nullptr);
  ASSERT_TRUE(db->Put("dk", "dv").ok());
  std::string v;
  ASSERT_TRUE(db->Get("dk", &v).ok());
  EXPECT_EQ("dv", v);
  ASSERT_TRUE(db->Delete("dk").ok());
  const Status s = db->Get("dk", &v);
  EXPECT_TRUE(s.IsNotFound()) << "DB 层删除后必须是 kNotFound：" << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("NotFound")) << s.ToString();
  // 删除不存在的 key 在 DB 层也是 OK
  EXPECT_TRUE(db->Delete("never-existed").ok());
}

TEST(MemTable, DeleteThenPutNewerStaysVisible) {
  // 覆盖 m1-prerequisites.md §1 I4 / §5「Delete 后再 Put → 新值可见（sequence 更大）」
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  AddEntry(&mem, 1, kTypeValue, "k", "old");
  AddEntry(&mem, 2, kTypeDeletion, "k", "");

  std::string value;
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
  // tombstone 成功后仍可写入更大 sequence 的新值
  AddEntry(&mem, 3, kTypeValue, "k", "new");
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
  EXPECT_EQ("new", value);
  // 历史快照的可见性不受影响：snapshot=2 看 tombstone，snapshot=1 看旧值
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k", 2), &value));
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 1), &value));
  EXPECT_EQ("old", value);

  // DB 层同语义
  std::unique_ptr<DB> db(OpenMemoryDB());
  ASSERT_TRUE(db != nullptr);
  ASSERT_TRUE(db->Put("k", "old").ok());
  ASSERT_TRUE(db->Delete("k").ok());
  EXPECT_TRUE(db->Get("k", &value).IsNotFound());
  ASSERT_TRUE(db->Put("k", "new").ok());
  ASSERT_TRUE(db->Get("k", &value).ok());
  EXPECT_EQ("new", value);
}

TEST(MemTable, MultiVersionOrderInInternalIterator) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  // 乱序写入：user key 与 sequence 都不按序给
  AddEntry(&mem, 5, kTypeValue, "b", "b5");
  AddEntry(&mem, 1, kTypeValue, "a", "a1");
  AddEntry(&mem, 9, kTypeValue, "a", "a9");
  AddEntry(&mem, 3, kTypeDeletion, "a", "");
  AddEntry(&mem, 7, kTypeValue, "c", "c7");
  AddEntry(&mem, 2, kTypeValue, "b", "b2");

  // 期望内部序：user key 升序；同 key sequence 降序（I2 / protocol §6.1）
  const std::pair<const char*, SequenceNumber> want[] = {
      {"a", 9}, {"a", 3}, {"a", 1}, {"b", 5}, {"b", 2}, {"c", 7}};
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(sizeof(want) / sizeof(want[0]), entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ(std::string(want[i].first), entries[i].user_key) << "第 " << i << " 条 user key 次序错误";
    EXPECT_EQ(want[i].second, entries[i].seq) << "第 " << i << " 条 sequence 次序错误";
  }
  EXPECT_EQ(static_cast<int>(kTypeDeletion), static_cast<int>(entries[1].type));

  // 用 InternalKeyComparator 复核迭代顺序非降
  for (size_t i = 1; i < entries.size(); ++i) {
    EXPECT_LE(icmp.Compare(entries[i - 1].internal_key, entries[i].internal_key), 0)
        << "内部迭代器顺序与比较器不一致（比较器与编码必须单点一致，prerequisites §3）";
  }

  // Seek 的 target 是 internal key（design §4.4）
  std::unique_ptr<Iterator> it(mem.NewIterator());
  it->Seek(BuildInternalKey("b", 2, kTypeValue));
  ASSERT_TRUE(it->Valid());
  {
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(it->key(), &user_key, &seq, &type));
    EXPECT_EQ("b", user_key.ToString());
    EXPECT_EQ(2u, seq);
  }
  it->Seek(BuildLookupKey("b", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  {
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(it->key(), &user_key, &seq, &type));
    EXPECT_EQ("b", user_key.ToString());
    EXPECT_EQ(5u, seq);
  }
  // 反向遍历是正向遍历的逆序
  // [#3 阶段修订] 原文把 NewIterator() 的裸指针直接传给收集助手，从不 delete ——
  // 违反 design §4.5 自己声明的「返回值所有权归调用方」，ASan/LSan 门禁抓到 2 处泄漏（64 字节）。
  // 用 unique_ptr 接管，断言与遍历顺序一字未改。
  std::unique_ptr<Iterator> fwd_it(mem.NewIterator());
  std::unique_ptr<Iterator> bwd_it(mem.NewIterator());
  const std::vector<std::pair<std::string, std::string>> fwd = CollectForward(fwd_it.get());
  const std::vector<std::pair<std::string, std::string>> bwd = CollectBackward(bwd_it.get());
  const std::vector<std::pair<std::string, std::string>> bwd_reversed(bwd.rbegin(), bwd.rend());
  EXPECT_EQ(fwd, bwd_reversed);
  EXPECT_EQ(entries.size(), fwd.size());
}

TEST(MemTable, UserIteratorDedupAndTombstone) {
  // 用户视图来自 DB::NewIterator()（design §4.4/§9）
  std::unique_ptr<DB> db(OpenMemoryDB());
  ASSERT_TRUE(db != nullptr);
  ASSERT_TRUE(db->Put("b", "b1").ok());
  ASSERT_TRUE(db->Put("a", "a1").ok());
  ASSERT_TRUE(db->Put("b", "b2").ok());
  ASSERT_TRUE(db->Put("c", "c1").ok());
  ASSERT_TRUE(db->Delete("a").ok());
  ASSERT_TRUE(db->Put("d", "d1").ok());
  ASSERT_TRUE(db->Delete("zz").ok());
  ASSERT_TRUE(db->Put("a", "a3").ok());  // 删除后再写：新值可见（I4）

  std::map<std::string, std::string> model;
  model["a"] = "a3";
  model["b"] = "b2";
  model["c"] = "c1";
  model["d"] = "d1";

  std::unique_ptr<Iterator> it(db->NewIterator());
  // 同 key 多版本只出最新；tombstone 不出（zz 永不可见）
  ExpectSameAsStdMap("用户视图去重 + 跳 tombstone", model, it.get());

  // key() 必须是 user key（不含 8 字节 trailer）
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("a", it->key().ToString());
  EXPECT_EQ(1u, it->key().size());

  // Seek 落在 >= target 的第一个可见 user key
  const std::pair<const char*, const char*> seeks[] = {{"", "a"},    {"a", "a"},   {"a0", "b"},
                                                      {"b", "b"},   {"b2", "c"},  {"c", "c"},
                                                      {"cz", "d"},  {"d", "d"}};
  for (const std::pair<const char*, const char*>& sc : seeks) {
    it->Seek(Slice(sc.first));
    ASSERT_TRUE(it->Valid()) << "Seek(" << sc.first << ") 必须命中 " << sc.second;
    EXPECT_EQ(std::string(sc.second), it->key().ToString()) << "Seek(" << sc.first << ") 落点错误";
  }
  // Seek 超过最大 key → kPastEnd 且 !Valid；再 Next 仍 !Valid（design §4.4）
  it->Seek(Slice("e"));
  EXPECT_FALSE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  // Seek 到被 tombstone 屏蔽的 key（zz）→ !Valid
  it->Seek(Slice("zz"));
  EXPECT_FALSE(it->Valid());
  // Seek 到被 tombstone 屏蔽的 a，但 a 有更新的值 → 命中 a
  it->Seek(Slice("a"));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("a", it->key().ToString());
  EXPECT_EQ("a3", it->value().ToString());

  // SeekToLast + Prev 反向遍历
  std::vector<std::string> back;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    back.push_back(it->key().ToString());
  }
  EXPECT_EQ((std::vector<std::string>{"d", "c", "b", "a"}), back);
  // kBeforeFirst + Prev 保持 !Valid
  it->Prev();
  EXPECT_FALSE(it->Valid());
  // kPastEnd + Prev → 回到最后一条可见条目
  it->Seek(Slice("e"));
  ASSERT_FALSE(it->Valid());
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("d", it->key().ToString());

  // 空表：任何操作都不得 UB，终态 !Valid（prerequisites §5）
  std::unique_ptr<DB> empty_db(OpenMemoryDB());
  ASSERT_TRUE(empty_db != nullptr);
  std::unique_ptr<Iterator> eit(empty_db->NewIterator());
  EXPECT_FALSE(eit->Valid());
  for (int op = 0; op < 6; ++op) {
    switch (op) {
      case 0: eit->SeekToFirst(); break;
      case 1: eit->Next(); break;
      case 2: eit->Prev(); break;
      case 3: eit->SeekToLast(); break;
      case 4: eit->Seek(Slice("x")); break;
      default: eit->Seek(Slice("")); break;
    }
    EXPECT_FALSE(eit->Valid()) << "空表上 op=" << op << " 之后必须 !Valid";
  }
  EXPECT_TRUE(eit->status().ok());
}

TEST(MemTable, IteratorStateMachine) {
  InternalKeyComparator icmp(BytewiseComparator());

  // ---- 空表 ----
  {
    MemTable mem(icmp, kTestWriteBufferSize);
    std::unique_ptr<Iterator> it(mem.NewIterator());
    EXPECT_FALSE(it->Valid());
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    it->Next();
    EXPECT_FALSE(it->Valid());
    it->Prev();
    EXPECT_FALSE(it->Valid());
    it->SeekToLast();
    EXPECT_FALSE(it->Valid());
    it->Seek(BuildInternalKey("k", 1, kTypeValue));
    EXPECT_FALSE(it->Valid());
    it->Seek(BuildLookupKey("k", kMaxSequenceNumber));
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().ok());
  }

  // ---- 非空表：5 条 a..e ----
  MemTable mem(icmp, kTestWriteBufferSize);
  for (int i = 0; i < 5; ++i) {
    AddEntry(&mem, static_cast<SequenceNumber>(i + 1), kTypeValue,
             std::string(1, static_cast<char>('a' + i)), "v");
  }
  std::unique_ptr<Iterator> it(mem.NewIterator());

  // kBeforeFirst + Prev 保持 !Valid（nothing before first）
  EXPECT_FALSE(it->Valid());
  it->Prev();
  EXPECT_FALSE(it->Valid());
  it->Prev();
  EXPECT_FALSE(it->Valid());

  // SeekToFirst 起步
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('a', ExtractUserKey(it->key())[0]);

  // Next 走到底 → kPastEnd；再 Next 保持 !Valid
  size_t n = 0;
  for (; it->Valid(); it->Next()) {
    ++n;
  }
  EXPECT_EQ(5u, n);
  EXPECT_FALSE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());

  // kPastEnd + Prev → 回到最后一条
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('e', ExtractUserKey(it->key())[0]);

  // SeekToLast + Prev 走到底 → kBeforeFirst；再 Prev 保持 !Valid
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('e', ExtractUserKey(it->key())[0]);
  n = 0;
  for (; it->Valid(); it->Prev()) {
    ++n;
  }
  EXPECT_EQ(5u, n);
  EXPECT_FALSE(it->Valid());
  it->Prev();
  EXPECT_FALSE(it->Valid());

  // Seek 超过最大 key → kPastEnd；再 Next 仍 !Valid
  it->Seek(BuildInternalKey("zzz", kMaxSequenceNumber, kTypeValue));
  EXPECT_FALSE(it->Valid());
  it->Next();
  EXPECT_FALSE(it->Valid());
  // SeekToLast 之后 Seek 仍能恢复
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('e', ExtractUserKey(it->key())[0]);

  // Seek 未命中 → 落在下一个更大的条目
  it->Seek(BuildLookupKey("b", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('b', ExtractUserKey(it->key())[0]);
  it->Seek(BuildInternalKey("bb", kMaxSequenceNumber, kTypeValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('c', ExtractUserKey(it->key())[0]);
  // [#3 阶段修订] 原文期望落在 'd'。但 trailer 是**降序**（本文件 MultiVersionOrderInInternalIterator
  // 与 util_test InternalKey.CompareOrder 都用 a5 < a3 钉死了这一点）：seq=0 是最小 trailer，
  // 因此 target("d",0,del) 排在 d 的全部版本**之后**，按「第一个 >= target」应当落在 'e'，
  // 这也正是本行注释「Seek 未命中 → 落在下一个更大的条目」的字面含义。
  it->Seek(BuildInternalKey("d", 0, kTypeDeletion));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('e', ExtractUserKey(it->key())[0]);
  // Seek 到 0 位置之前
  it->Seek(BuildInternalKey("", kMaxSequenceNumber, kTypeValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('a', ExtractUserKey(it->key())[0]);
  // Seek 之后 Prev 回到更小的条目
  it->Seek(BuildLookupKey("d", kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('c', ExtractUserKey(it->key())[0]);
  it->Prev();
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ('a', ExtractUserKey(it->key())[0]);
  it->Prev();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(MemTable, InsertDuringIteration) {
  // I5：已建立的位置与已返回的结果不受后续插入影响（design §4.4 / prerequisites §1）
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  AddEntry(&mem, 1, kTypeValue, "c", "c1");
  AddEntry(&mem, 2, kTypeValue, "e", "e1");
  AddEntry(&mem, 3, kTypeValue, "g", "g1");

  std::unique_ptr<Iterator> it(mem.NewIterator());
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("c", ExtractUserKey(it->key()).ToString());

  // 迭代中插入「当前位置之前」的 key（a、b），以及一个更小的新版本
  AddEntry(&mem, 4, kTypeValue, "a", "a1");
  AddEntry(&mem, 5, kTypeValue, "b", "b1");
  AddEntry(&mem, 6, kTypeValue, "c", "c2");  // c 的新版本比当前迭代位置更小（sequence 更大）

  // 已建立的位置不变
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("c", ExtractUserKey(it->key()).ToString());

  // 继续前向遍历：不回头看到 a/b（跳表节点只增不删 + next_ 已建立，I3）
  std::vector<std::string> seen;
  for (; it->Valid(); it->Next()) {
    seen.push_back(ExtractUserKey(it->key()).ToString());
  }
  EXPECT_EQ((std::vector<std::string>{"c", "e", "g"}), seen);
  EXPECT_TRUE(it->status().ok());

  // 新建迭代器能看到全部 6 条（内部序，含 c 的两个版本）
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(6u, entries.size());
  size_t c_count = 0;
  for (const InternalEntry& e : entries) {
    if (e.user_key == "c") ++c_count;
  }
  EXPECT_EQ(2u, c_count);
}

TEST(MemTable, FreezeRejectsWriteWithFrozenStatus) {
  InternalKeyComparator icmp(BytewiseComparator());
  // 小容量：几次写入即触及上限（写前判，design §8.1）
  MemTable mem(icmp, 4096);
  EXPECT_FALSE(mem.IsFrozen());

  const std::string value(512, 'x');
  int accepted = 0;
  Status last;
  for (int i = 1; i <= 200; ++i) {
    last = mem.Add(static_cast<SequenceNumber>(i), kTypeValue, "key" + std::to_string(i), value);
    if (!last.ok()) break;
    ++accepted;
  }
  EXPECT_GT(accepted, 0) << "至少有一次写入应当成功";
  EXPECT_TRUE(last.IsFrozen()) << "超限必须返回 kFrozen，不得静默丢弃：" << last.ToString();
  EXPECT_TRUE(mem.IsFrozen());

  // 被拒这次不得写入：条目数与用量都不变
  const size_t entries_at_freeze = mem.NumEntries();
  const size_t usage_at_freeze = mem.ApproximateMemoryUsage();
  EXPECT_EQ(static_cast<size_t>(accepted), entries_at_freeze);
  Status again = mem.Add(1000, kTypeValue, "later", "v");
  EXPECT_TRUE(again.IsFrozen()) << again.ToString();
  EXPECT_EQ(entries_at_freeze, mem.NumEntries());
  EXPECT_EQ(usage_at_freeze, mem.ApproximateMemoryUsage());
  EXPECT_TRUE(mem.IsFrozen());
  // 冻结后 Get 不到被拒的 key
  std::string v;
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("later", kMaxSequenceNumber), &v));

  // Freeze 幂等：首次 true，其后 false（design §8.3）
  {
    MemTable m2(icmp, kTestWriteBufferSize);
    AddEntry(&m2, 1, kTypeValue, "k", "v");
    EXPECT_FALSE(m2.IsFrozen());
    EXPECT_TRUE(m2.Freeze());
    EXPECT_TRUE(m2.IsFrozen());
    EXPECT_FALSE(m2.Freeze());
    EXPECT_FALSE(m2.Freeze());
    // 冻结后写入被拒，但统计不变
    const size_t n = m2.NumEntries();
    const size_t u = m2.ApproximateMemoryUsage();
    EXPECT_TRUE(m2.Add(2, kTypeValue, "k2", "v2").IsFrozen());
    EXPECT_EQ(n, m2.NumEntries());
    EXPECT_EQ(u, m2.ApproximateMemoryUsage());
  }
}

TEST(MemTable, FrozenStillReadable) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, 2048);
  AddEntry(&mem, 1, kTypeValue, "k1", "v1");
  AddEntry(&mem, 2, kTypeValue, "k2", "v2");
  AddEntry(&mem, 3, kTypeDeletion, "k3", "");
  ASSERT_TRUE(mem.Freeze());
  ASSERT_TRUE(mem.IsFrozen());

  // 冻结后查询与迭代照常可用（design §8.3），含旧值、tombstone、未命中
  std::string v;
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k1", kMaxSequenceNumber), &v));
  EXPECT_EQ("v1", v);
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k2", kMaxSequenceNumber), &v));
  EXPECT_EQ("v2", v);
  EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k3", kMaxSequenceNumber), &v));
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k4", kMaxSequenceNumber), &v));
  // 历史快照照常
  EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k1", 1), &v));
  EXPECT_EQ("v1", v);

  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(3u, entries.size());
  EXPECT_EQ(3u, mem.NumEntries());
  const std::vector<std::pair<std::string, std::string>> visible = VisibleKeysFromInternal(&mem);
  ASSERT_EQ(2u, visible.size()) << "tombstone 不进用户视图";
  EXPECT_EQ("k1", visible[0].first);
  EXPECT_EQ("k2", visible[1].first);

  // 冻结后写入持续被拒，且不改变可读内容
  EXPECT_TRUE(mem.Add(4, kTypeValue, "k5", "v5").IsFrozen());
  EXPECT_EQ(3u, mem.NumEntries());
  EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k5", kMaxSequenceNumber), &v));
  EXPECT_EQ(3u, ReadInternalEntries(&mem).size());
}

TEST(MemTable, CapacityStatsMonotonic) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, 1u << 20);
  size_t prev_usage = mem.ApproximateMemoryUsage();
  size_t prev_entries = mem.NumEntries();
  EXPECT_EQ(0u, prev_entries);
  EXPECT_GE(prev_usage, sizeof(MemTable));

  const std::string value(100, 'v');
  for (int i = 1; i <= 200; ++i) {
    AddEntry(&mem, static_cast<SequenceNumber>(i), kTypeValue,
             "key" + std::to_string(1000 + i), value);
    EXPECT_EQ(prev_entries + 1, mem.NumEntries()) << "第 " << i << " 次写入后条目数应为 +1";
    EXPECT_GE(mem.ApproximateMemoryUsage(), prev_usage)
        << "第 " << i << " 次写入后 MemoryUsage 必须单调不减（I6）";
    prev_usage = mem.ApproximateMemoryUsage();
    prev_entries = mem.NumEntries();
  }

  // 被拒的写入（非法 key）不得改变统计（I6/I10）
  const size_t usage_before_reject = mem.ApproximateMemoryUsage();
  const size_t entries_before_reject = mem.NumEntries();
  EXPECT_TRUE(mem.Add(5000, kTypeValue, Slice(), "v").IsInvalidArgument());
  EXPECT_TRUE(mem.Add(5001, kTypeValue, std::string(kMaxUserKeySize + 1, 'k'), "v").IsInvalidArgument());
  EXPECT_EQ(usage_before_reject, mem.ApproximateMemoryUsage());
  EXPECT_EQ(entries_before_reject, mem.NumEntries());

  // tombstone 同样计入条目数与用量
  AddEntry(&mem, 5002, kTypeDeletion, "gone", "");
  EXPECT_EQ(entries_before_reject + 1, mem.NumEntries());
  EXPECT_GE(mem.ApproximateMemoryUsage(), usage_before_reject);

  // 只读统计：Get/迭代不得改变统计
  std::string v;
  mem.Get(BuildLookupKey("key1001", kMaxSequenceNumber), &v);
  EXPECT_EQ(entries_before_reject + 1, mem.NumEntries());
  EXPECT_EQ(mem.ApproximateMemoryUsage(), mem.ApproximateMemoryUsage());
}

TEST(MemTable, RejectsEmptyAndOversizedKey) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  const size_t entries0 = mem.NumEntries();
  const size_t usage0 = mem.ApproximateMemoryUsage();

  // 空 key → kInvalidArgument（protocol §8 / design §8.1），不写入、不改变统计
  {
    const Status s = mem.Add(1, kTypeValue, Slice(), "v");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
    EXPECT_NE(std::string::npos, s.ToString().find("InvalidArgument")) << s.ToString();
  }
  EXPECT_TRUE(mem.Add(2, kTypeDeletion, Slice(""), Slice()).IsInvalidArgument());
  EXPECT_TRUE(mem.Add(3, kTypeValue, Slice(""), std::string(kMaxUserKeySize, 'x')).IsInvalidArgument());
  EXPECT_EQ(entries0, mem.NumEntries());
  EXPECT_EQ(usage0, mem.ApproximateMemoryUsage());

  // 上限含端点：恰好 kMaxUserKeySize（64 KiB）合法
  const std::string max_key(kMaxUserKeySize, 'k');
  const Status ok_max = mem.Add(4, kTypeValue, max_key, "v");
  EXPECT_TRUE(ok_max.ok()) << "user key = 64 KiB 必须合法（上限含端点）：" << ok_max.ToString();
  EXPECT_EQ(entries0 + 1, mem.NumEntries());

  // kMaxUserKeySize + 1 → kInvalidArgument
  const std::string over_key(kMaxUserKeySize + 1, 'k');
  {
    const Status s = mem.Add(5, kTypeValue, over_key, "v");
    EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  }
  EXPECT_EQ(entries0 + 1, mem.NumEntries()) << "超长 key 不得写入";
  {
    std::string v;
    EXPECT_EQ(MemTable::GetResult::kNotFound,
              mem.Get(BuildLookupKey(over_key, kMaxSequenceNumber), &v))
        << "超长 key 从未写入";
  }
  // 恰好上限的 key 可读回
  {
    std::string v;
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey(max_key, kMaxSequenceNumber), &v));
    EXPECT_EQ("v", v);
  }

  // 空 value 合法（protocol §8）
  EXPECT_TRUE(mem.Add(6, kTypeValue, "emptyval", Slice()).ok());
  EXPECT_TRUE(mem.Add(7, kTypeValue, "emptyval2", "").ok());
  {
    std::string v("dirty");
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("emptyval", kMaxSequenceNumber), &v));
    EXPECT_TRUE(v.empty());
  }
  EXPECT_EQ(entries0 + 3, mem.NumEntries());

  // 空 key 与超长 key 的写入都不得阻塞后续合法写入（不是「一错全废」）
  EXPECT_TRUE(mem.Add(8, kTypeValue, "after", "ok").ok());
  {
    std::string v;
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("after", kMaxSequenceNumber), &v));
    EXPECT_EQ("ok", v);
  }
  EXPECT_FALSE(mem.IsFrozen());
}

// ===========================================================================
// DB 边界（m1-prerequisites.md §5；design §4.5/§9）
// ===========================================================================
TEST(DB, OpenMemoryMode) {
  // Options 默认值（design §4.3）
  const Options defaults;
  EXPECT_EQ(BytewiseComparator(), defaults.comparator);
  EXPECT_EQ(4u * 1024 * 1024, defaults.write_buffer_size);

  Options options;
  DB* db = nullptr;
  const Status s = DB::Open(options, "", &db);
  ASSERT_TRUE(s.ok()) << "空 name = 内存模式，必须成功：" << s.ToString();
  ASSERT_TRUE(db != nullptr);
  EXPECT_TRUE(db->Put("k", "v").ok());
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ("v", v);
  // 迭代器所有权归调用方（design §4.5）
  std::unique_ptr<Iterator> it(db->NewIterator());
  ASSERT_TRUE(it != nullptr);
  const std::vector<std::pair<std::string, std::string>> all = CollectForward(it.get());
  ASSERT_EQ(1u, all.size());
  EXPECT_EQ("k", all[0].first);
  EXPECT_EQ("v", all[0].second);
  delete db;
}

// [#2/M2 修订，登记于 docs/m2-prerequisites.md §9] M2 改变了 Open 的契约：
//   name 非空 从「仅内存模式 ⇒ kNotSupported」变为「持久模式（WAL + 崩溃恢复）」（docs/m2-design.md §5.1）。
//   这条断言与旧契约直接绑定，必须随契约更新；本测试的其它断言（空 name 成功、非法 options 拒绝）未动。
//   原测试名 OpenRejectsNonEmptyName 已不成立，改名为 OpenPersistentMode 并补上「重启后可恢复」。
TEST(DB, OpenPersistentMode) {
  TempDir dir("lsm_db_");
  Options options;
  DB* p = nullptr;
  ASSERT_TRUE(DB::Open(options, dir.path(), &p).ok()) << "非空 name ⇒ 持久模式必须成功";
  ASSERT_TRUE(p != nullptr);
  ASSERT_TRUE(p->Put("k", "v").ok());
  std::string v;
  ASSERT_TRUE(p->Get("k", &v).ok());
  EXPECT_EQ("v", v);
  ASSERT_TRUE(p->Close().ok());
  delete p;

  // 关掉再打开：数据必须从 WAL 恢复（M2 的核心能力）
  DB* q = nullptr;
  ASSERT_TRUE(DB::Open(options, dir.path(), &q).ok());
  ASSERT_TRUE(q != nullptr);
  const Status gs = q->Get("k", &v);
  ASSERT_TRUE(gs.ok()) << "重启后必须能从 WAL 恢复：" << gs.ToString();
  EXPECT_EQ("v", v);
  EXPECT_TRUE(q->Close().ok());
  EXPECT_TRUE(q->Close().ok()) << "Close 必须幂等";
  delete q;
}

TEST(DB, OpenRejectsInvalidOptions) {
  DB* p = nullptr;
  {
    Options options;
    options.comparator = nullptr;
    const Status s = DB::Open(options, "", &p);
    EXPECT_TRUE(s.IsInvalidArgument()) << "comparator == nullptr 必须是 kInvalidArgument：" << s.ToString();
    EXPECT_EQ(nullptr, p);
  }
  {
    Options options;
    options.write_buffer_size = 0;
    const Status s = DB::Open(options, "", &p);
    EXPECT_TRUE(s.IsInvalidArgument()) << "write_buffer_size == 0 必须是 kInvalidArgument：" << s.ToString();
    EXPECT_EQ(nullptr, p);
  }
  // 失败两次之后仍能成功打开（不得留下全局坏状态）
  {
    DB* ok = nullptr;
    ASSERT_TRUE(DB::Open(Options(), "", &ok).ok());
    ASSERT_TRUE(ok != nullptr);
    delete ok;
  }
}

TEST(DB, PutGetDeleteRoundTrip) {
  std::unique_ptr<DB> db(OpenMemoryDB());
  ASSERT_TRUE(db != nullptr);
  std::string v;

  // 未命中
  const Status miss = db->Get("nope", &v);
  EXPECT_TRUE(miss.IsNotFound()) << miss.ToString();
  EXPECT_NE(std::string::npos, miss.ToString().find("NotFound")) << miss.ToString();

  // 往返 + 覆盖
  EXPECT_TRUE(db->Put("a", "1").ok());
  EXPECT_TRUE(db->Put("b", "2").ok());
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ("1", v);
  EXPECT_TRUE(db->Put("a", "3").ok());
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ("3", v);

  // 删除 → NotFound；再次 Put 新值可见
  EXPECT_TRUE(db->Delete("a").ok());
  EXPECT_TRUE(db->Get("a", &v).IsNotFound());
  EXPECT_TRUE(db->Put("a", "4").ok());
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ("4", v);

  // 空 value 合法
  EXPECT_TRUE(db->Put("empty", "").ok());
  ASSERT_TRUE(db->Get("empty", &v).ok());
  EXPECT_TRUE(v.empty());

  // 空 key / 超长 key → kInvalidArgument（design §4.2 kInvalidArgument 产生点）
  EXPECT_TRUE(db->Put("", "x").IsInvalidArgument());
  EXPECT_TRUE(db->Delete("").IsInvalidArgument());
  EXPECT_TRUE(db->Put(std::string(kMaxUserKeySize + 1, 'k'), "x").IsInvalidArgument());
  EXPECT_TRUE(db->Delete(std::string(kMaxUserKeySize + 1, 'k')).IsInvalidArgument());
  // 空 key 查不到东西（不崩、不为 OK）
  EXPECT_FALSE(db->Get(Slice(), &v).ok());
  // 被拒的写入不得写入
  EXPECT_TRUE(db->Get("", &v).IsNotFound() || db->Get("", &v).IsInvalidArgument());
  EXPECT_TRUE(db->Get(std::string(kMaxUserKeySize + 1, 'k'), &v).IsNotFound());

  // 用户视图与模型一致
  std::map<std::string, std::string> model;
  model["a"] = "4";
  model["b"] = "2";
  model["empty"] = "";
  std::unique_ptr<Iterator> it(db->NewIterator());
  ExpectSameAsStdMap("DB::Put/Delete 往返后的用户视图", model, it.get());
}

TEST(DB, PutAfterFreezeReturnsFrozen) {
  // 小缓冲：几次写入即冻结。kFrozen 的产生点之一就是 DB::Put/Delete（design §4.2）
  std::unique_ptr<DB> db(OpenMemoryDB(4096));
  ASSERT_TRUE(db != nullptr);

  const std::string value(512, 'x');
  int accepted = 0;
  Status last;
  for (int i = 1; i <= 200; ++i) {
    last = db->Put("k" + std::to_string(i), value);
    if (!last.ok()) break;
    ++accepted;
  }
  EXPECT_GT(accepted, 0);
  EXPECT_TRUE(last.IsFrozen()) << "超限必须返回 kFrozen（不得静默丢弃）：" << last.ToString();

  // 冻结后查询/迭代照常可用
  std::string v;
  const Status first_get = db->Get("k1", &v);
  ASSERT_TRUE(first_get.ok()) << first_get.ToString();
  EXPECT_EQ(value, v);
  std::unique_ptr<Iterator> it(db->NewIterator());
  EXPECT_EQ(static_cast<size_t>(accepted), CollectForward(it.get()).size());

  // 后续写入持续被拒，且不得写入
  EXPECT_TRUE(db->Put("later", "v").IsFrozen());
  EXPECT_TRUE(db->Delete("k1").IsFrozen());
  EXPECT_TRUE(db->Get("later", &v).IsNotFound());
  ASSERT_TRUE(db->Get("k1", &v).ok()) << "被拒的 Delete 不得真的删掉 k1";
  EXPECT_EQ(value, v);
  std::unique_ptr<Iterator> it2(db->NewIterator());
  EXPECT_EQ(static_cast<size_t>(accepted), CollectForward(it2.get()).size());
}

// ===========================================================================
// B 组：压力与边界（design §11）
// ===========================================================================
TEST(Stress, OneMillionKeysReconcile) {
  const int kN = 1000000;
  Rng rng(kHarnessSeed + 31);
  InternalKeyComparator icmp(BytewiseComparator());
  // prerequisites §7.6：B 组必须显式给足容量，否则中途冻结会改变语义
  MemTable mem(icmp, 1u << 30);

  std::multimap<std::string, std::string> model;
  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    // RandomKey 提供随机前缀，KeyFromIndex 保证唯一（碰撞会让「全量对账」的前提失效）
    const std::string k = RandomKey(&rng, 8) + KeyFromIndex(static_cast<uint64_t>(i));
    const std::string v = RandomValue(&rng, 4 + static_cast<int>(rng.Uniform(13)));
    model.emplace(k, v);
    AddEntry(&mem, static_cast<SequenceNumber>(i + 1), kTypeValue, k, v);
  }
  const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
  const long long write_ms = ElapsedMs(t0, t1);

  ASSERT_EQ(static_cast<size_t>(kN), model.size()) << "key 必须唯一";
  ASSERT_EQ(static_cast<size_t>(kN), mem.NumEntries());
  EXPECT_FALSE(mem.IsFrozen()) << "容量必须足够，写入过程中不得冻结";

  // 全量对账（顺序 + 内容）：内部序 == 模型的 user key 升序
  const std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
  std::unique_ptr<Iterator> it(mem.NewIterator());
  std::multimap<std::string, std::string>::const_iterator m = model.begin();
  std::vector<char> seq_seen(static_cast<size_t>(kN) + 1, 0);
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++m) {
    ASSERT_TRUE(m != model.end()) << "第 " << n << " 条：模型已耗尽";
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(it->key(), &user_key, &seq, &type));
    EXPECT_EQ(m->first, user_key.ToString()) << "第 " << n << " 条 key 不一致（顺序错乱）";
    EXPECT_EQ(m->second, it->value().ToString()) << "第 " << n << " 条 value 不一致";
    EXPECT_EQ(static_cast<int>(kTypeValue), static_cast<int>(type));
    ASSERT_GE(seq, 1u);
    ASSERT_LE(seq, static_cast<SequenceNumber>(kN));
    EXPECT_EQ(0, static_cast<int>(seq_seen[static_cast<size_t>(seq)])) << "sequence 重复：" << seq;
    seq_seen[static_cast<size_t>(seq)] = 1;
    ++n;
  }
  EXPECT_TRUE(m == model.end()) << "迭代器少输出 " << model.size() - n << " 条";
  EXPECT_EQ(static_cast<size_t>(kN), n);
  for (int i = 1; i <= kN; ++i) {
    ASSERT_EQ(1, static_cast<int>(seq_seen[static_cast<size_t>(i)])) << "sequence " << i << " 缺失";
  }
  const std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now();
  const long long reconcile_ms = ElapsedMs(t2, t3);

  std::cout << "[   INFO   ] Stress.OneMillionKeysReconcile: n=" << n
            << " write_ms=" << write_ms << " reconcile_ms=" << reconcile_ms
            << " ApproximateMemoryUsage=" << mem.ApproximateMemoryUsage() << " bytes" << std::endl;
  RecordProperty("keys", std::to_string(n));
  RecordProperty("write_ms", std::to_string(write_ms));
  RecordProperty("reconcile_ms", std::to_string(reconcile_ms));
  RecordProperty("memtable_bytes", std::to_string(mem.ApproximateMemoryUsage()));
}

TEST(Stress, DeleteThirtyPercentReconcile) {
  const int kN = 300000;
  Rng rng(kHarnessSeed + 37);
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, 1u << 30);  // prerequisites §7.6

  std::multimap<std::string, std::string> model;
  std::vector<std::string> keys;
  keys.reserve(static_cast<size_t>(kN));
  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    const std::string k = RandomKey(&rng, 10) + KeyFromIndex(static_cast<uint64_t>(i));
    const std::string v = RandomValue(&rng, 8 + static_cast<int>(rng.Uniform(9)));
    model.emplace(k, v);
    keys.push_back(k);
    AddEntry(&mem, static_cast<SequenceNumber>(i + 1), kTypeValue, k, v);
  }

  // 随机删除 30%（tombstone 语义，design §7.4 决策 5）
  Shuffle(&keys, &rng);
  const size_t to_delete = keys.size() * 30 / 100;
  SequenceNumber seq = static_cast<SequenceNumber>(kN);
  for (size_t i = 0; i < to_delete; ++i) {
    ++seq;
    AddEntry(&mem, seq, kTypeDeletion, keys[i], "");
    model.erase(keys[i]);
  }

  // 统计口径（design §8.3）：tombstone 计入条目数，不计入可见 user key
  EXPECT_EQ(static_cast<size_t>(kN) + to_delete, mem.NumEntries())
      << "tombstone 必须计入 NumEntries";
  EXPECT_EQ(static_cast<size_t>(kN) - to_delete, model.size());
  EXPECT_EQ(static_cast<size_t>(kN), static_cast<size_t>(keys.size()));

  // 内部序全量复核：user key 非降、同 key sequence 严格降序、distinct user key == kN
  const std::vector<InternalEntry> entries = ReadInternalEntries(&mem);
  ASSERT_EQ(static_cast<size_t>(kN) + to_delete, entries.size());
  size_t groups = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i == 0 || entries[i].user_key != entries[i - 1].user_key) {
      ++groups;
    } else {
      EXPECT_GT(entries[i - 1].seq, entries[i].seq) << "同 key 多版本必须按 sequence 降序（I2）";
    }
    if (i > 0) {
      EXPECT_LE(entries[i - 1].user_key, entries[i].user_key) << "user key 必须升序";
    }
  }
  EXPECT_EQ(static_cast<size_t>(kN), groups) << "distinct user key 数必须等于写入次数";

  // 可见 key 对账（独立参照实现，不经过 db.cc 的 UserIterator）
  const std::vector<std::pair<std::string, std::string>> visible = VisibleKeysFromInternal(&mem);
  ExpectMatchesModel("删除 30% 后的可见 user key", model, visible);

  // Get 抽查：已删除 → kDeleted；未删除 → 模型里的值
  std::string v;
  size_t checked = 0;
  for (size_t i = 0; i < to_delete; i += (to_delete / 50 + 1)) {
    EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey(keys[i], kMaxSequenceNumber), &v))
        << "被删除的 key 必须返回 kDeleted";
    ++checked;
  }
  for (std::multimap<std::string, std::string>::const_iterator m = model.begin();
       m != model.end() && checked < 200; ++m) {
    ASSERT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey(m->first, kMaxSequenceNumber), &v));
    EXPECT_EQ(m->second, v);
    ++checked;
  }
  EXPECT_GT(checked, 0u);

  const long long elapsed_ms = ElapsedMs(t0, std::chrono::steady_clock::now());
  std::cout << "[   INFO   ] Stress.DeleteThirtyPercentReconcile: entries=" << mem.NumEntries()
            << " visible=" << visible.size() << " deleted=" << to_delete
            << " elapsed_ms=" << elapsed_ms
            << " memtable_bytes=" << mem.ApproximateMemoryUsage() << std::endl;
  RecordProperty("entries", std::to_string(mem.NumEntries()));
  RecordProperty("visible_keys", std::to_string(visible.size()));
  RecordProperty("deleted", std::to_string(to_delete));
  RecordProperty("elapsed_ms", std::to_string(elapsed_ms));
}

TEST(Stress, SameKey100kTimes) {
  const int kN = 100000;
  InternalKeyComparator icmp(BytewiseComparator());

  // ---- 内部视图：10 万条版本 ----
  MemTable mem(icmp, 1u << 30);  // prerequisites §7.6
  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  for (int i = 1; i <= kN; ++i) {
    AddEntry(&mem, static_cast<SequenceNumber>(i), kTypeValue, "same-key", "v" + std::to_string(i));
  }

  std::string value;
  ASSERT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("same-key", kMaxSequenceNumber), &value));
  EXPECT_EQ("v" + std::to_string(kN), value);
  EXPECT_EQ(static_cast<size_t>(kN), mem.NumEntries());
  EXPECT_FALSE(mem.IsFrozen());

  // 内部迭代：10 万条，sequence 从 kN 严格递减到 1
  std::unique_ptr<Iterator> it(mem.NewIterator());
  SequenceNumber expect_seq = static_cast<SequenceNumber>(kN);
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(it->key(), &user_key, &seq, &type));
    EXPECT_EQ("same-key", user_key.ToString());
    ASSERT_EQ(expect_seq, seq) << "第 " << n << " 条版本顺序错误";
    --expect_seq;
    ++n;
  }
  EXPECT_EQ(static_cast<size_t>(kN), n) << "内部迭代必须看到全部 10 万条版本";
  EXPECT_EQ(0u, expect_seq);

  // ---- 用户视图：只出现 1 个 user key ----
  std::unique_ptr<DB> db(OpenMemoryDB(1u << 30));
  ASSERT_TRUE(db != nullptr);
  for (int i = 1; i <= kN; ++i) {
    ASSERT_TRUE(db->Put("same-key", "v" + std::to_string(i)).ok());
  }
  std::unique_ptr<Iterator> uit(db->NewIterator());
  size_t user_keys = 0;
  for (uit->SeekToFirst(); uit->Valid(); uit->Next()) {
    EXPECT_EQ("same-key", uit->key().ToString());
    ++user_keys;
  }
  EXPECT_EQ(1u, user_keys) << "用户视图同一 user key 只能出现一次";
  uit->SeekToFirst();
  ASSERT_TRUE(uit->Valid());
  EXPECT_EQ("v" + std::to_string(kN), uit->value().ToString());
  uit->SeekToLast();
  ASSERT_TRUE(uit->Valid());
  EXPECT_EQ("same-key", uit->key().ToString());
  EXPECT_EQ("v" + std::to_string(kN), uit->value().ToString());
  {
    std::string v;
    ASSERT_TRUE(db->Get("same-key", &v).ok());
    EXPECT_EQ("v" + std::to_string(kN), v);
  }
  // 反向遍历（SeekToLast + Prev）同样只有 1 条
  size_t rev = 0;
  for (uit->SeekToLast(); uit->Valid(); uit->Prev()) {
    ++rev;
  }
  EXPECT_EQ(1u, rev);

  const long long elapsed_ms = ElapsedMs(t0, std::chrono::steady_clock::now());
  std::cout << "[   INFO   ] Stress.SameKey100kTimes: versions=" << n << " elapsed_ms=" << elapsed_ms
            << " memtable_bytes=" << mem.ApproximateMemoryUsage() << std::endl;
  RecordProperty("versions", std::to_string(n));
  RecordProperty("user_keys_in_view", std::to_string(user_keys));
  RecordProperty("elapsed_ms", std::to_string(elapsed_ms));
  RecordProperty("memtable_bytes", std::to_string(mem.ApproximateMemoryUsage()));
}

TEST(Stress, AlignmentUnderSanitizers) {
  // I9：对齐与内存安全。显式 alignof 断言在本用例强制执行；
  // ASan/UBSan 的结论由三目录门禁给出（docs/m1-prerequisites.md §8），此处把构建形态记录进用例属性。
#if defined(__SANITIZE_ADDRESS__)
  RecordProperty("sanitizer", "asan");
  std::cout << "[   INFO   ] Stress.AlignmentUnderSanitizers: build=asan" << std::endl;
#elif defined(__SANITIZE_THREAD__)
  RecordProperty("sanitizer", "tsan");
  std::cout << "[   INFO   ] Stress.AlignmentUnderSanitizers: build=tsan" << std::endl;
#else
  RecordProperty("sanitizer", "none");
  std::cout << "[   INFO   ] Stress.AlignmentUnderSanitizers: build=plain（ASan/UBSan 结论见 build-asan 门禁）"
            << std::endl;
#endif

  const size_t kAlign = alignof(std::max_align_t);
  Arena arena;
  Rng rng(kHarnessSeed + 41);

  struct Alloc {
    char* p;
    size_t n;
    unsigned char byte;
  };
  std::vector<Alloc> allocs;
  for (int i = 0; i < 2000; ++i) {
    const size_t n = 1 + rng.Uniform(300);
    const size_t align = (i % 4 == 0) ? 64u : ((i % 4 == 1) ? 32u : kAlign);
    char* p = arena.AllocateAligned(n, align);
    ASSERT_TRUE(p != nullptr);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % align)
        << "第 " << i << " 次分配 n=" << n << " align=" << align << " 未对齐";
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign);
    const unsigned char byte = static_cast<unsigned char>(i & 0xFF);
    std::memset(p, static_cast<int>(byte), n);  // 越界写会被 ASan 抓到
    Alloc a;
    a.p = p;
    a.n = n;
    a.byte = byte;
    allocs.push_back(a);
  }
  // 区间不重叠 + 内容未被后续分配破坏（越界读会被 ASan 抓到）
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
  for (const Alloc& a : allocs) {
    ranges.emplace_back(reinterpret_cast<uintptr_t>(a.p), reinterpret_cast<uintptr_t>(a.p + a.n));
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    EXPECT_LE(ranges[i - 1].second, ranges[i].first) << "第 " << i << " 段与前一段重叠";
  }
  for (const Alloc& a : allocs) {
    for (size_t i = 0; i < a.n; ++i) {
      ASSERT_EQ(static_cast<char>(a.byte), a.p[i]) << "已分配区域被后续分配破坏";
    }
  }

  // 显式 8/16/32/64/128/256 对齐（AllocateAligned 的前置条件是 align 为 2 的幂；
  // [#4 评审建议 5] 原文注释写「非 2 的幂请求不得崩」但 aligns[] 全是 2 的幂，注释已订正）
  const size_t aligns[] = {8, 16, 32, 64, 128, 256};
  for (size_t align : aligns) {
    char* p = arena.AllocateAligned(1000, align);
    ASSERT_TRUE(p != nullptr);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % align);
  }

  // 跳表节点与条目都来自 Arena：插入 + 遍历路径不得有对齐/越界问题
  {
    Arena sl_arena;
    Skiplist list(&sl_arena, BytewiseComparator());
    for (int i = 0; i < 5000; ++i) {
      list.Insert(ArenaStoreSlice(&sl_arena, KeyFromIndex(static_cast<uint64_t>(i))));
    }
    EXPECT_EQ(5000u, list.GetStats().node_count);
    size_t n = 0;
    std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      EXPECT_EQ(8u, it->key().size());
      ++n;
    }
    EXPECT_EQ(5000u, n);
  }

  // MemTable 路径：Arena + 跳表 + 内部迭代器（生命周期与对齐由 ASan 兜底）
  {
    InternalKeyComparator icmp(BytewiseComparator());
    MemTable mem(icmp, 1u << 20);
    for (int i = 0; i < 2000; ++i) {
      const std::string k = KeyFromIndex(static_cast<uint64_t>(i));
      const std::string v = RandomValue(&rng, 1 + static_cast<int>(rng.Uniform(40)));
      AddEntry(&mem, static_cast<SequenceNumber>(i + 1), kTypeValue, k, v);
    }
    EXPECT_EQ(2000u, mem.NumEntries());
    std::unique_ptr<Iterator> it(mem.NewIterator());
    size_t n = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const Slice v = it->value();
      // 逐个字节读回（越界读会被 ASan 抓到）
      for (size_t i = 0; i < v.size(); ++i) {
        ASSERT_EQ(static_cast<char>(static_cast<unsigned char>(v[i])),
                  static_cast<char>(static_cast<unsigned char>(v[i])));
      }
      ++n;
    }
    EXPECT_EQ(2000u, n);
  }

  RecordProperty("allocations", std::to_string(allocs.size()));
  std::cout << "[   INFO   ] Stress.AlignmentUnderSanitizers: allocations=" << allocs.size()
            << " alignof(max_align_t)=" << kAlign << std::endl;
}

// ===========================================================================
// [#4 评审阻断项回归] 自定义比较器一致性 / 畸形输入安全（design §4.3、protocol §6）
// ===========================================================================

namespace {

// 大小写不敏感：key 的「等价关系」与「字节相等」故意不一致，用来暴露
// 「排序用注入比较器、命中/判段却用逐字节」这类不一致缺陷。
// 不用 std::tolower，避免引入 <cctype> 与 locale 相关的未定义行为。
inline char FoldAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

class CaseInsensitiveComparator : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
      const unsigned char ca = static_cast<unsigned char>(FoldAscii(a[i]));
      const unsigned char cb = static_cast<unsigned char>(FoldAscii(b[i]));
      if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
  }
  const char* Name() const override { return "test.CaseInsensitiveComparator"; }
};

}  // namespace

TEST(MemTable, CustomComparatorEqualityIsHonored) {
  CaseInsensitiveComparator cmp;
  Options options;
  options.comparator = &cmp;
  DB* raw = nullptr;
  ASSERT_TRUE(DB::Open(options, "", &raw).ok());
  std::unique_ptr<DB> db(raw);

  ASSERT_TRUE(db->Put("Key", "v1").ok());
  std::string v;
  ASSERT_TRUE(db->Get("KEY", &v).ok()) << "等价 key（仅大小写不同）必须命中";
  EXPECT_EQ("v1", v);
  ASSERT_TRUE(db->Get("key", &v).ok());
  EXPECT_EQ("v1", v);

  // 等价 key 的再次写入 = 同一条逻辑 key 的新版本（覆盖语义按比较器判定）
  ASSERT_TRUE(db->Put("KEY", "v2").ok());
  ASSERT_TRUE(db->Get("Key", &v).ok());
  EXPECT_EQ("v2", v);

  // 用户视图：等价 key 只能出现一条（否则说明判段退化成了逐字节比较）
  std::unique_ptr<Iterator> it(db->NewIterator());
  const std::vector<std::pair<std::string, std::string>> fwd = CollectForward(it.get());
  ASSERT_EQ(1u, fwd.size()) << "等价 user key 必须去重成一条";
  EXPECT_EQ("v2", fwd[0].second);

  // 正反向视图条数必须一致（design §4.4）
  std::unique_ptr<Iterator> it2(db->NewIterator());
  const std::vector<std::pair<std::string, std::string>> bwd = CollectBackward(it2.get());
  EXPECT_EQ(fwd.size(), bwd.size()) << "正反向视图条数必须一致";

  // 再加两条不同 key：排序（按注入比较器）与去重必须同时成立
  ASSERT_TRUE(db->Put("Zz", "z").ok());
  ASSERT_TRUE(db->Put("aa", "a").ok());
  std::unique_ptr<Iterator> it3(db->NewIterator());
  const std::vector<std::pair<std::string, std::string>> all = CollectForward(it3.get());
  ASSERT_EQ(3u, all.size());
  EXPECT_EQ("aa", all[0].first);
  EXPECT_EQ("KEY", all[1].first) << "输出的是该逻辑 key 最新版本的字节形态";
  EXPECT_EQ("Zz", all[2].first);
  std::unique_ptr<Iterator> it4(db->NewIterator());
  EXPECT_EQ(all.size(), CollectBackward(it4.get()).size());
}

TEST(MemTable, MalformedInputDoesNotReadOutOfBounds) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);
  AddEntry(&mem, 1, kTypeValue, std::string(1000, 'x'), "v");

  // 1) lookup_key 少于 8 字节（漏拼 trailer 的调用方）：必须安全判未命中，不得触发 size_t 下溢
  std::string v;
  for (size_t n = 0; n < kInternalKeyMinSize; ++n) {
    const std::string bad(n, 'x');
    EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(Slice(bad), &v)) << "lookup_key size=" << n;
  }
  EXPECT_EQ(MemTable::GetResult::kFound,
            mem.Get(BuildLookupKey(std::string(1000, 'x'), kMaxSequenceNumber), &v));

  // 2) 条目编码自洽但 internal_key_size < 8：比较器必须拒绝解码并退化为整条字节序
  const std::string malformed =
      std::string(1, static_cast<char>(5)) + std::string(5, 'z') + std::string(1, '\x00');
  const std::string well_formed = ManualEntry("userkey", 7, kTypeValue, Slice("v"));
  MemTableKeyComparator mkc(&icmp);
  EXPECT_NE(0, mkc.Compare(malformed, well_formed));
  EXPECT_NE(0, mkc.Compare(well_formed, malformed));
  EXPECT_EQ(0, mkc.Compare(malformed, malformed));
  EXPECT_EQ(0, mkc.Compare(well_formed, well_formed));

  // 3) InternalKeyComparator 对畸形 internal key 同样不得越界（M3 会复用同一个比较器）
  const std::string good = BuildInternalKey("userkey", 1, kTypeValue);
  EXPECT_EQ(0, icmp.Compare(Slice("ab"), Slice("ab")));
  EXPECT_LT(icmp.Compare(Slice("ab"), Slice(good)), 0);
  EXPECT_GT(icmp.Compare(Slice(good), Slice("ab")), 0);
}
