// tests/compaction_db_test.cpp —— M4.2：compaction 端到端/读路径层级化/延迟删除/失败恢复/独立 CRC
#include "test_harness.h"

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db_impl.h"
#include "filename.h"
#include "memenv.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "version_edit.h"
#include "version_set.h"

namespace lsm {
namespace {

using test::MemEnv;

std::string K(int i) {
  char b[32];
  std::snprintf(b, sizeof(b), "k%06d", i);
  return std::string(b);
}
std::string V(int i) {
  char b[32];
  std::snprintf(b, sizeof(b), "v%06d", i);
  return std::string(b);
}

bool WaitForCompaction(PersistentDBImpl* impl, uint64_t target, int spins = 4000000) {
  for (int i = 0; i < spins; ++i) {
    if (impl->GetCompactionStats().completed >= target) return true;
    std::this_thread::yield();
  }
  return false;
}

// A04 的**独立** CRC32C（Castagnoli，反射多项式 0x82F63B78，init/final 取反）。
// 刻意不复用被测的 util/crc32c，避免"实现与测试同错"。
uint32_t Crc32cIndependent(const std::string& s) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < s.size(); ++i) {
    crc ^= static_cast<unsigned char>(s[i]);
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

}  // namespace

// ---- A04 补强：record 帧的 CRC 用独立实现复算 ----
TEST(VersionEdit, CrcIndependentlyRecomputed) {
  VersionEdit e;
  e.SetComparatorName("C");
  e.SetLogNumber(300);
  e.SetNextFileNumber(9);
  e.SetMinLogNumberToKeep(2);
  FileMetaData f;
  f.number = 4;
  f.file_size = 200;
  f.max_sequence = 300;
  f.smallest = BuildInternalKey("k", 300, kTypeValue);
  f.largest = BuildInternalKey("k", 1, kTypeValue);
  e.AddFile(1, f);

  std::string payload;
  ASSERT_TRUE(e.EncodePayloadTo(&payload));
  std::string prefix;
  PutFixed32(&prefix, static_cast<uint32_t>(payload.size()));
  prefix.push_back(static_cast<char>(kManifestRecordTypeVersionEdit));
  prefix.append(payload);

  const uint32_t mine = Crc32cIndependent(prefix);
  const uint32_t impl = crc32c::Value(prefix.data(), prefix.size());
  EXPECT_EQ(mine, impl) << "独立 CRC32C 实现必须与被测实现一致";

  std::string full;
  ASSERT_TRUE(EncodeManifestRecord(e, &full));
  const uint32_t tail = DecodeFixed32(full.data() + full.size() - 4);
  EXPECT_EQ(mine, tail) << "record 尾部 CRC 必须等于独立复算值";
}

// ---- compaction 端到端：L0 -> L1，数据不丢，层号真的变了 ----
TEST(CompactionDb, EndToEndMovesL0ToL1AndKeepsData) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  const int kN = 300;
  // 分 3 批写 + 强制 flush ⇒ 至少 3 个 L0 文件，稳定越过 trigger=2。
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 100; ++i) {
      const int idx = round * 100 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  EXPECT_TRUE(WaitForCompaction(impl, 1)) << "compaction 必须真的发生过（正向标记）";

  const CompactionStats cs = impl->GetCompactionStats();
  EXPECT_GE(cs.completed, 1u);
  EXPECT_GE(cs.input_files, 1u);
  EXPECT_GE(cs.output_files, 1u);
  EXPECT_GE(impl->files_at_level(1), 1u) << "compaction 输出必须落到 L1+";

  std::string v;
  for (int i = 1; i <= kN; ++i) {
    ASSERT_TRUE(db->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v) << "key " << i;
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok());
  for (int i = 1; i <= kN; ++i) {
    ASSERT_TRUE(db2->Get(K(i), &v).ok()) << "重开后 key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ---- 读路径层级化：L0 覆盖值、tombstone 屏蔽下层、range 过滤不计 files_checked ----
TEST(ReadLevels, NewestWinsAndRangeFilterNotCounted) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 80; ++i) {
      const int idx = round * 80 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  ASSERT_TRUE(WaitForCompaction(impl, 1)) << "compaction 必须真的发生过";

  // L0 的新值必须覆盖 L1 的旧值
  ASSERT_TRUE(db->Put(K(7), "newer").ok());
  std::string v;
  ASSERT_TRUE(db->Get(K(7), &v).ok());
  EXPECT_EQ("newer", v);
  // L0 的 tombstone 必须屏蔽 L1 的旧值
  ASSERT_TRUE(db->Delete(K(8)).ok());
  EXPECT_TRUE(db->Get(K(8), &v).IsNotFound());
  // 范围外的 key：不得把任何文件算进 files_checked
  const DbReadStats before = impl->GetReadStats();
  EXPECT_TRUE(db->Get("zzzzzz", &v).IsNotFound());
  const DbReadStats after = impl->GetReadStats();
  EXPECT_EQ(0u, after.files_checked - before.files_checked)
      << "被 key range 过滤掉的文件不得计入 files_checked";
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ---- 延迟删除：被 live Version 引用的文件绝不删；引用释放后才删（I42/I43/X8）----
TEST(Delete, DeferredUntilRefsZero) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  impl->SetCompactionAutoForTest(false);   // 用 RunOneCompactionForTest 做确定性单轮
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 60; ++i) {
      const int idx = round * 60 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  std::shared_ptr<const Version> held = impl->RefCurrentVersionForTest();
  ASSERT_TRUE(held != nullptr);
  std::set<uint64_t> before;
  for (const FileMetaData& f : held->AllFiles()) before.insert(f.number);
  ASSERT_GE(before.size(), 2u) << "需要至少 2 个 L0 输入";

  impl->RunOneCompactionForTest();   // 同步一轮：输入被"取代"并进延迟删除队列
  auto cur = impl->RefCurrentVersionForTest();
  ASSERT_TRUE(cur != nullptr);
  std::set<uint64_t> after;
  for (const FileMetaData& f : cur->AllFiles()) after.insert(f.number);
  std::vector<uint64_t> removed;
  for (uint64_t n : before) {
    if (after.count(n) == 0) removed.push_back(n);
  }
  ASSERT_FALSE(removed.empty()) << "本轮必须真的移除了输入文件";
  for (uint64_t n : removed) {
    EXPECT_TRUE(env.FileExists(TableFileName("/db", n)))
        << "仍被 held Version 引用 ⇒ 绝不删（X8：只看 current_ 的实现会失败）";
  }
  EXPECT_GE(impl->pending_delete_size(), removed.size());

  held.reset();   // 引用归零 → 下一次 MaybeDeleteObsoleteFiles 才允许删
  cur.reset();
  impl->MaybeDeleteObsoleteFilesForTest();
  for (uint64_t n : removed) {
    EXPECT_FALSE(env.FileExists(TableFileName("/db", n))) << "引用归零后必须删";
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A08 补强：在 CURRENT 的 rename 上**字面注入**失败 ----
class CurrentRenameFailEnv : public MemEnv {
 public:
  bool fail_current_rename = false;
  Status RenameFile(const std::string& src, const std::string& target) override {
    if (fail_current_rename && target.size() >= 7 &&
        target.compare(target.size() - 7, 7, "CURRENT") == 0) {
      fail_current_rename = false;   // one-shot：只失败一次
      return Status::IOError("injected CURRENT rename failure", target);
    }
    return MemEnv::RenameFile(src, target);
  }
};

TEST(Current, InjectedRenameFailureKeepsOldCurrent) {
  CurrentRenameFailEnv env;
  Options o;
  o.env = &env;
  // 大 write_buffer：Put 期间不触发后台自动 flush（避免注入点被抢先消费）；
  // roll_bytes=1：每次**显式** ForceFlushForTest 都走模式 (a)，必然发生 CURRENT 切换。
  o.write_buffer_size = 1u << 20;
  o.manifest_roll_bytes = 1;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 50; ++i) ASSERT_TRUE(db->Put(K(i), V(i)).ok());
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  uint64_t first = 0;
  ASSERT_TRUE(VersionSet::ReadCurrent(&env, "/db", &first).ok());
  ASSERT_TRUE(env.FileExists(ManifestFileName("/db", first)));

  // rename(CURRENT.tmp, CURRENT) **之前**注入失败：新 MANIFEST 已写出，但 CURRENT 不得被切换。
  // 先写完待注册的数据，再**武装**注入点（避免后台自动 flush 抢先消费掉这一次失败）。
  impl->SetCompactionAutoForTest(false);   // 本用例只考 CURRENT 切换，不让 compaction 的 roll 抢注入点
  for (int i = 51; i <= 100; ++i) ASSERT_TRUE(db->Put(K(i), V(i)).ok());
  ASSERT_TRUE(db->Put(K(101), V(101)).ok());   // 确保 memtable 非空（否则 ForceFlush 无操作）
  env.fail_current_rename = true;
  EXPECT_FALSE(impl->ForceFlushForTest().ok()) << "注入的 CURRENT rename 失败必须让本次注册失败";
  EXPECT_GE(impl->GetFlushStats().flushes_failed, 1u) << "注入的 rename 失败必须变成 fail-stop 计数";
  uint64_t cur = 0;
  ASSERT_TRUE(VersionSet::ReadCurrent(&env, "/db", &cur).ok());
  EXPECT_EQ(first, cur) << "rename 失败 ⇒ CURRENT 必须仍指向旧的、完整的 MANIFEST";
  ASSERT_TRUE(env.FileExists(ManifestFileName("/db", cur)));
  (void)db->Close();   // 粘性 bg_error ⇒ Close 返回该错误
  delete db;

  // 重开：CURRENT 指向的旧 MANIFEST 必须可完整回放，已 ack 的数据不得丢（WAL 重放补齐）。
  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok());
  std::string v;
  for (int i = 1; i <= 101; ++i) {
    ASSERT_TRUE(db2->Get(K(i), &v).ok()) << "重开后 key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ---- A17：安装期层级校验必须拒绝非法布局 ----
TEST(LevelLayout, OverlapRejectedOnInstall) {
  const InternalKeyComparator icmp(BytewiseComparator());
  auto mk = [](uint64_t n, const std::string& lo, const std::string& hi) {
    FileMetaData f;
    f.number = n;
    f.file_size = 100;
    f.max_sequence = 100;
    f.smallest = BuildInternalKey(lo, 100, kTypeValue);
    f.largest = BuildInternalKey(hi, 1, kTypeValue);
    return f;
  };
  std::string why;
  {   // (a) 同一 user key 跨两个文件（端点相等）
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {mk(1, "a", "m"), mk(2, "m", "z")};
    EXPECT_FALSE(ValidateLevelLayout(l, icmp, &why)) << "端点相等必须拒绝（R5 的收紧）";
    EXPECT_NE(std::string::npos, why.find("level 1")) << why;
  }
  {   // (b) 区间部分覆盖
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {mk(1, "a", "n"), mk(2, "m", "z")};
    EXPECT_FALSE(ValidateLevelLayout(l, icmp, &why));
  }
  {   // (c) 顺序错乱
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {mk(1, "m", "n"), mk(2, "a", "b")};
    EXPECT_FALSE(ValidateLevelLayout(l, icmp, &why));
  }
  {   // 合法布局必须通过
    std::vector<std::vector<FileMetaData>> l(kNumLevels);
    l[1] = {mk(1, "a", "b"), mk(2, "c", "d")};
    EXPECT_TRUE(ValidateLevelLayout(l, icmp, &why)) << why;
  }
}

// ---- A28/A29：快照可见性与最小快照 ----
TEST(Snapshot, VisibleVersionSurvivesCompactionAndSmallestUpdates) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  impl->SetCompactionAutoForTest(false);
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 60; ++i) {
      const int idx = round * 60 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  auto* s = impl->GetSnapshot();
  ASSERT_TRUE(s != nullptr);
  const SequenceNumber seq_at_snap = s->sequence;
  ASSERT_TRUE(db->Put(K(1), "after-snapshot").ok());
  impl->RunOneCompactionForTest();

  std::string v;
  ASSERT_TRUE(impl->GetAtSnapshot(s, K(1), &v).ok());
  EXPECT_EQ(V(1), v) << "A28：快照内可见的旧版本不得被 compaction 丢掉";
  ASSERT_TRUE(db->Get(K(1), &v).ok());
  EXPECT_EQ("after-snapshot", v) << "最新读必须看到新值";

  EXPECT_EQ(seq_at_snap, impl->smallest_snapshot());
  auto* s2 = impl->GetSnapshot();
  ASSERT_TRUE(s2 != nullptr);
  EXPECT_EQ(seq_at_snap, impl->smallest_snapshot());
  impl->ReleaseSnapshot(s);
  EXPECT_EQ(s2->sequence, impl->smallest_snapshot());
  impl->ReleaseSnapshot(s2);
  EXPECT_EQ(impl->last_sequence(), impl->smallest_snapshot());
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A41：已 ack 数据在 MANIFEST 回放后仍可见 ----
TEST(Recovery, AckedDataVisibleAfterManifestReplay) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 200; ++i) {
    WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Put(wo, K(i), V(i)).ok());
  }
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok());
  auto* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats rs = impl2->GetRecoveryStats();
  EXPECT_TRUE(rs.manifest_present);
  EXPECT_GE(rs.manifest_edits_replayed, 1u);
  std::string v;
  for (int i = 1; i <= 200; ++i) {
    ASSERT_TRUE(db2->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ---- compaction 失败矩阵：输出写/rename 失败 ⇒ 旧版本完好、无半成品注册、CURRENT 可回放 ----
class CompactionFailEnv : public MemEnv {
 public:
  bool fail_output_write = false;
  bool fail_output_rename = false;
  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    if (fail_output_write && fname.find(".sst.tmp") != std::string::npos) {
      return Status::IOError("injected output write failure", fname);
    }
    return MemEnv::NewWritableFile(fname, result);
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    if (fail_output_rename && target.find(".sst") != std::string::npos &&
        target.find(".tmp") == std::string::npos) {
      return Status::IOError("injected output rename failure", target);
    }
    return MemEnv::RenameFile(src, target);
  }
};

static void RunFailureCase(bool fail_write, bool fail_rename) {
  CompactionFailEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  impl->SetCompactionAutoForTest(false);
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 60; ++i) {
      const int idx = round * 60 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  const uint64_t files_before = impl->files_at_level(0);
  const CompactionStats before = impl->GetCompactionStats();
  env.fail_output_write = fail_write;
  env.fail_output_rename = fail_rename;
  impl->RunOneCompactionForTest();
  env.fail_output_write = false;
  env.fail_output_rename = false;

  const CompactionStats after = impl->GetCompactionStats();
  EXPECT_GE(after.failed - before.failed, 1u) << "失败必须计数";
  EXPECT_EQ(files_before, impl->files_at_level(0)) << "失败不得安装新版本（输入仍在 L0）";
  EXPECT_EQ(0u, impl->files_at_level(1)) << "不得有半成品被注册";
  std::string v;
  for (int i = 1; i <= 180; ++i) {
    ASSERT_TRUE(db->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok()) << "CURRENT 必须仍指向可回放的 MANIFEST";
  for (int i = 1; i <= 180; ++i) {
    ASSERT_TRUE(db2->Get(K(i), &v).ok()) << "重开后 key " << i;
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

TEST(CompactionFail, OutputWriteFailureKeepsOldVersion) {
  RunFailureCase(true, false);
  EXPECT_FALSE(::testing::Test::HasFailure()) << "WriteFailure 用例内不得有失败断言";
}
TEST(CompactionFail, OutputRenameFailureKeepsOldVersion) {
  RunFailureCase(false, true);
  EXPECT_FALSE(::testing::Test::HasFailure()) << "RenameFailure 用例内不得有失败断言";
}

}  // namespace lsm
