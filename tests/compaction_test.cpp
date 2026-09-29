// tests/compaction_test.cpp —— M4.2：选层/选文件/闭包/丢弃判据（A11~A16/A23/A26 的确定性部分）
#include "test_harness.h"

#include <algorithm>
#include <string>
#include <vector>

#include "compaction.h"
#include "version_set.h"

namespace lsm {
namespace {

FileMetaData F(uint64_t number, const std::string& lo, const std::string& hi,
               uint64_t size = 1000, SequenceNumber seq = 100) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.max_sequence = seq;
  f.smallest = BuildInternalKey(lo, seq, kTypeValue);
  f.largest = BuildInternalKey(hi, 1, kTypeValue);
  return f;
}

Version V(std::vector<std::vector<FileMetaData>> levels) { return Version(levels, 10, 1, 999); }

std::vector<uint64_t> Numbers(const std::vector<FileMetaData>& fs) {
  std::vector<uint64_t> out;
  for (const FileMetaData& f : fs) out.push_back(f.number);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// ---- A11 ----
TEST(Level0, FileCountThresholdTriggers) {
  Options o;
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(3, "a", "b"), F(2, "c", "d"), F(1, "e", "f")};
    Version v = V(l);
    EXPECT_EQ(-1, Compaction::PickLevel(v, o)) << "3 个 L0（< trigger=4）不得触发";
  }
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(4, "a", "b"), F(3, "c", "d"), F(2, "e", "f"), F(1, "g", "h")};
    Version v = V(l);
    EXPECT_EQ(0, Compaction::PickLevel(v, o)) << "4 个 L0（== trigger）必须触发";
    EXPECT_DOUBLE_EQ(1.0, Compaction::Score(v, o, 0));
  }
}

// ---- A12 ----
TEST(LevelN, CapacityThresholdTriggers) {
  Options o;
  const uint64_t cap1 = Compaction::MaxBytesForLevel(o, 1);
  EXPECT_EQ(o.max_bytes_for_level_base, cap1);
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {F(1, "a", "b", cap1 - 1)};
    EXPECT_EQ(-1, Compaction::PickLevel(V(l), o)) << "恰好 < 容量不得触发";
  }
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {F(1, "a", "b", cap1)};
    EXPECT_EQ(1, Compaction::PickLevel(V(l), o)) << "恰好 == 容量必须触发";
  }
  {
    // L6 超限不再向下：PickLevel 只能返回 6，不得产生 L7 的概念。
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[6] = {F(1, "a", "b", Compaction::MaxBytesForLevel(o, 6))};
    EXPECT_EQ(6, Compaction::PickLevel(V(l), o));
  }
}

// ---- A13 ----
TEST(PickLevel, MaxScoreAndTieBreak) {
  Options o;
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(4, "a", "a"), F(3, "b", "b"), F(2, "c", "c"), F(1, "d", "d")};   // score 1.0
    l[1] = {F(9, "m", "n", 2 * o.max_bytes_for_level_base)};                    // score 2.0
    EXPECT_EQ(1, Compaction::PickLevel(V(l), o)) << "必须选 score 大者";
  }
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(4, "a", "a"), F(3, "b", "b"), F(2, "c", "c"), F(1, "d", "d")};   // 1.0
    l[1] = {F(9, "m", "n", o.max_bytes_for_level_base)};                        // 1.0
    EXPECT_EQ(0, Compaction::PickLevel(V(l), o)) << "平手取层号小者（L0 优先）";
  }
}

// ---- A14 ----
TEST(PickFile, RoundRobinDeterministic) {
  Options o;
  std::vector<std::vector<FileMetaData>> l(kNumLevels);
  l[0] = {F(5, "a", "b"), F(3, "c", "d"), F(1, "e", "f")};
  Version v = V(l);
  CompactionInputs a, b;
  std::string why;
  ASSERT_TRUE(Compaction::PickInputs(v, 0, PickStrategy::kRoundRobin, o, &a, &why)) << why;
  ASSERT_TRUE(Compaction::PickInputs(v, 0, PickStrategy::kRoundRobin, o, &b, &why)) << why;
  ASSERT_FALSE(a.inputs[0].empty());
  EXPECT_NE(a.inputs[0].end(),
            std::find_if(a.inputs[0].begin(), a.inputs[0].end(),
                         [](const FileMetaData& f) { return f.number == 1; }))
      << "L0 必须取文件号最小者作种子";
  EXPECT_EQ(Numbers(a.inputs[0]), Numbers(b.inputs[0])) << "同一输入必须给出同一序列";
}

// ---- A15 ----
TEST(PickFile, MinOverlapDeterministic) {
  Options o;
  std::string why;
  {
    // 重叠字节相同 ⇒ tie-break 取文件号最小者（3），下层 = 与之重叠的 L1 文件。
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(5, "a", "b"), F(3, "m", "n")};
    l[1] = {F(4, "a", "b"), F(9, "m", "n")};
    Version v = V(l);
    CompactionInputs in;
    ASSERT_TRUE(Compaction::PickInputs(v, 0, PickStrategy::kMinOverlap, o, &in, &why)) << why;
    ASSERT_EQ(1u, in.inputs[0].size());
    EXPECT_EQ(3u, in.inputs[0][0].number) << "并列时取文件号最小（A15 的 tie-break）";
    ASSERT_EQ(1u, in.inputs[1].size());
    EXPECT_EQ(9u, in.inputs[1][0].number);
  }
  {
    // 重叠最小（0）者优先 ⇒ 选文件 3，下层为空（没有与 m..n 重叠的 L1 文件）。
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[0] = {F(5, "a", "b"), F(3, "m", "n")};
    l[1] = {F(4, "a", "b")};
    Version v = V(l);
    CompactionInputs in;
    ASSERT_TRUE(Compaction::PickInputs(v, 0, PickStrategy::kMinOverlap, o, &in, &why)) << why;
    ASSERT_EQ(1u, in.inputs[0].size());
    EXPECT_EQ(3u, in.inputs[0][0].number) << "重叠最小者优先";
    EXPECT_TRUE(in.inputs[1].empty());
  }
}

// ---- A16（X1）----
TEST(L0Inputs, TransitiveOverlapClosure) {
  Options o;
  std::vector<std::vector<FileMetaData>> l(kNumLevels);
  // 桥接反例：A[a..b] / B[c..d] / C[b..c]；L0 内部按 number 降序 ⇒ {A=3, B=2, C=1}
  l[0] = {F(3, "a", "b"), F(2, "c", "d"), F(1, "b", "c")};
  Version v = V(l);
  CompactionInputs in;
  std::string why;
  ASSERT_TRUE(Compaction::PickInputs(v, 0, PickStrategy::kRoundRobin, o, &in, &why)) << why;
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), Numbers(in.inputs[0]))
      << "闭包必须含全部三个文件（X1）";
  EXPECT_EQ("a", in.begin_user_key);
  EXPECT_EQ("d", in.end_user_key);
  // 闭包后 L0 中不得再存在与输入区间相交的残留文件
  const std::vector<uint64_t> chosen = Numbers(in.inputs[0]);
  for (const FileMetaData& f : v.level_files(0)) {
    if (std::find(chosen.begin(), chosen.end(), f.number) != chosen.end()) continue;
    const std::string flo = Compaction::UserKeyOfInternal(f.smallest);
    const std::string fhi = Compaction::UserKeyOfInternal(f.largest);
    const bool disjoint = fhi < in.begin_user_key || flo > in.end_user_key;
    EXPECT_TRUE(disjoint) << "残留文件 " << f.number << " 与输入区间相交但不在闭包里";
  }
}

// ---- A23/X6 ----
TEST(BaseLevelForKey, StartsAtLevelPlusTwo) {
  // X6：IsBaseLevelForKey(level) 只看 l ∈ [level+2, kNumLevels)。
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[2] = {F(1, "a", "b")};   // 对 level=1 而言 level+1=2 是本次输入层 ⇒ 不得算更底层
    Version v = V(l);
    EXPECT_TRUE(Compaction::IsBaseLevelForKey(v, Slice("a"), 1))
        << "level+1 的文件是本次输入，不得算作更底层";
  }
  {
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[3] = {F(1, "a", "b")};   // level+2：真正的更底层
    Version v = V(l);
    EXPECT_FALSE(Compaction::IsBaseLevelForKey(v, Slice("a"), 1))
        << "从 level+2 起步 ⇒ L3 的覆盖必须被看到";
  }
}

// ---- A26（析取 vs 合取的真值表）----
TEST(Drop, DecisionIsDisjunctionNotConjunction) {
  // 有更新的可见版本（last <= smallest）⇒ 旧版本必须丢（任意 type/base）
  EXPECT_TRUE(Compaction::ShouldDrop(kTypeValue, 10, 10, 10, false));
  EXPECT_TRUE(Compaction::ShouldDrop(kTypeValue, 10, 5, 10, false));
  // 最新可见版本且是 tombstone，底层无旧值 ⇒ 丢
  EXPECT_TRUE(Compaction::ShouldDrop(kTypeDeletion, 10, kMaxSequenceNumber, 10, true));
  // 最新可见版本且是 tombstone，底层有旧值 ⇒ 保留（若实现成合取会丢）
  EXPECT_FALSE(Compaction::ShouldDrop(kTypeDeletion, 10, kMaxSequenceNumber, 10, false));
  // tombstone 但 seq > smallest_snapshot ⇒ 必须保留（可见性前提）
  EXPECT_FALSE(Compaction::ShouldDrop(kTypeDeletion, 11, kMaxSequenceNumber, 10, true));
  // 最新可见版本且是值 ⇒ 必须保留
  EXPECT_FALSE(Compaction::ShouldDrop(kTypeValue, 10, kMaxSequenceNumber, 10, true));
  // 旧版本但更新的版本对最小快照不可见（last > smallest）⇒ 必须保留
  EXPECT_FALSE(Compaction::ShouldDrop(kTypeValue, 5, 20, 10, false));
}

}  // namespace lsm
