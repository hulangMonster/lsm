// tests/compaction_db_test.cpp —— M4.2：compaction 端到端/读路径层级化/延迟删除/失败恢复/独立 CRC
#include "test_harness.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "db_impl.h"
#include "filename.h"
#include "memenv.h"
#include "db_iter.h"
#include "memtable.h"
#include "merging_iterator.h"
#include "sstable/table_builder.h"
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
  int fail_sync_dir_remaining = 0;   // A43：rename 之后再让 SyncDir 失败 == 留下未注册的 .sst
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
  Status SyncDir(const std::string& d) override {
    if (fail_sync_dir_remaining > 0) {
      --fail_sync_dir_remaining;
      return Status::IOError("injected SyncDir failure", d);
    }
    return MemEnv::SyncDir(d);
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

// ================= M4.3 批 (i)：A36 加宽锁探针 + A37 flush 优先 =================

// ---- A36：compaction 全路径的「持 DB 互斥锁期间 IO 调用数 == 0」+ 反向自检 ----
class CompactionSpyEnv : public MemEnv {
 public:
  std::atomic<uint64_t> violations{0};
  std::atomic<uint64_t> append_calls{0}, sync_calls{0}, rename_calls{0}, sync_dir_calls{0};
  std::atomic<uint64_t> get_file_size_calls{0}, get_children_calls{0}, remove_file_calls{0};
  std::atomic<uint64_t> truncate_calls{0}, block_read_calls{0}, new_file_calls{0};

  Status NewWritableFile(const std::string& f, WritableFile** r) override {
    ++new_file_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(f, &inner);
    if (s.ok()) *r = new SpyFile(inner, this);
    return s;
  }
  Status NewAppendableFile(const std::string& f, WritableFile** r) override {
    ++new_file_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(f, &inner);
    if (s.ok()) *r = new SpyFile(inner, this);
    return s;
  }
  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r) override {
    if (DbMutexHeldOnThisThread()) ++violations;
    RandomAccessFile* inner = nullptr;
    const Status s = MemEnv::NewRandomAccessFile(f, &inner);
    if (s.ok()) *r = new SpyRandom(inner, this);
    return s;
  }
  Status RenameFile(const std::string& a, const std::string& b) override {
    ++rename_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::RenameFile(a, b);
  }
  Status SyncDir(const std::string& d) override {
    ++sync_dir_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::SyncDir(d);
  }
  Status GetFileSize(const std::string& f, uint64_t* n) override {
    ++get_file_size_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::GetFileSize(f, n);
  }
  Status GetChildren(const std::string& d, std::vector<std::string>* r) override {
    ++get_children_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::GetChildren(d, r);
  }
  Status RemoveFile(const std::string& f) override {
    ++remove_file_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::RemoveFile(f);
  }
  Status Truncate(const std::string& f, uint64_t n) override {
    ++truncate_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::Truncate(f, n);
  }

 private:
  class SpyFile : public WritableFile {
   public:
    SpyFile(WritableFile* inner, CompactionSpyEnv* env) : inner_(inner), env_(env) {}
    ~SpyFile() override { delete inner_; }
    Status Append(const Slice& d) override {
      ++env_->append_calls;
      if (DbMutexHeldOnThisThread()) ++env_->violations;
      return inner_->Append(d);
    }
    Status Flush() override { return inner_->Flush(); }
    Status Sync() override {
      ++env_->sync_calls;
      if (DbMutexHeldOnThisThread()) ++env_->violations;
      return inner_->Sync();
    }
    Status Close() override { return inner_->Close(); }

   private:
    WritableFile* inner_;
    CompactionSpyEnv* env_;
  };
  class SpyRandom : public RandomAccessFile {
   public:
    SpyRandom(RandomAccessFile* inner, CompactionSpyEnv* env) : inner_(inner), env_(env) {}
    ~SpyRandom() override { delete inner_; }
    Status Read(uint64_t off, size_t n, Slice* out, char* scratch) const override {
      ++env_->block_read_calls;
      if (DbMutexHeldOnThisThread()) ++env_->violations;
      return inner_->Read(off, n, out, scratch);
    }

   private:
    RandomAccessFile* inner_;
    CompactionSpyEnv* env_;
  };
};

TEST(Locks, ZeroIoWhileHoldingDbMutexOnCompactionPath) {
  CompactionSpyEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  o.manifest_roll_bytes = 1;   // 强制 MANIFEST 重建，覆盖 rename/SyncDir/RemoveFile
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
  ASSERT_TRUE(WaitForCompaction(impl, 1));
  std::string v;
  for (int i = 1; i <= 240; ++i) ASSERT_TRUE(db->Get(K(i), &v).ok());
  (void)impl->GetAmplificationStats();   // GetChildren/GetFileSize（锁外）
  impl->MaybeDeleteObsoleteFilesForTest();

  EXPECT_EQ(0u, env.violations.load()) << "L26：持 DB 互斥锁期间发生了 IO";
  EXPECT_GE(env.append_calls.load(), 1u);
  EXPECT_GE(env.sync_calls.load(), 1u);
  EXPECT_GE(env.rename_calls.load(), 1u);
  EXPECT_GE(env.sync_dir_calls.load(), 1u);
  EXPECT_GE(env.get_file_size_calls.load(), 1u);
  EXPECT_GE(env.get_children_calls.load(), 1u);
  EXPECT_GE(env.remove_file_calls.load(), 1u);
  EXPECT_GE(env.block_read_calls.load(), 1u);

  // 反向自检：持锁时逐项操作，探针必须**每一项**都报警（证明它真的在数）。
  const uint64_t before = env.violations.load();
  const uint64_t ra = env.rename_calls.load(), sa = env.sync_dir_calls.load();
  const uint64_t ga = env.get_file_size_calls.load(), ca = env.get_children_calls.load();
  const uint64_t ma = env.remove_file_calls.load(), ta = env.truncate_calls.load();
  const uint64_t aa = env.append_calls.load(), ya = env.sync_calls.load();
  const uint64_t ba = env.block_read_calls.load();
  impl->RunHoldingDbMutexForTest([&] {
    env.RenameFile("/db/a", "/db/b");
    env.SyncDir("/db");
    uint64_t sz = 0;
    env.GetFileSize("/db/CURRENT", &sz);
    std::vector<std::string> ch;
    env.GetChildren("/db", &ch);
    env.RemoveFile("/db/nonexistent");
    env.Truncate("/db/nonexistent", 0);
    WritableFile* f = nullptr;
    env.NewWritableFile("/db/probe.tmp", &f);
    if (f != nullptr) {
      f->Append(Slice("x"));
      f->Sync();
      f->Close();
      delete f;
    }
    RandomAccessFile* r = nullptr;
    env.NewRandomAccessFile("/db/CURRENT", &r);
    if (r != nullptr) {
      Slice out;
      char scratch[16];
      r->Read(0, 1, &out, scratch);
      delete r;
    }
  });
  EXPECT_GE(env.violations.load() - before, 9u) << "反向自检：每个注入点都必须报警";
  EXPECT_EQ(ra + 1, env.rename_calls.load());
  EXPECT_EQ(sa + 1, env.sync_dir_calls.load());
  EXPECT_EQ(ga + 1, env.get_file_size_calls.load());
  EXPECT_EQ(ca + 1, env.get_children_calls.load());
  EXPECT_EQ(ma + 1, env.remove_file_calls.load());
  EXPECT_EQ(ta + 1, env.truncate_calls.load());
  EXPECT_GE(env.append_calls.load(), aa + 1);
  EXPECT_GE(env.sync_calls.load(), ya + 1);
  EXPECT_GE(env.block_read_calls.load(), ba + 1);
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A37：flush 优先于 compaction（L27）——compaction 在"写输出后"被挡住，flush 仍必须完成 ----
class BlockingCompactionHook : public CompactionHook {
 public:
  void OnOutputWritten(uint64_t) override {
    std::unique_lock<std::mutex> l(mu_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(l, [this] { return released_; });
  }
  bool WaitEntered(int spins = 4000000) {
    for (int i = 0; i < spins; ++i) {
      {
        std::lock_guard<std::mutex> l(mu_);
        if (entered_) return true;
      }
      std::this_thread::yield();
    }
    return false;
  }
  void Release() {
    std::lock_guard<std::mutex> l(mu_);
    released_ = true;
    cv_.notify_all();
  }
  bool entered() const {
    std::lock_guard<std::mutex> l(mu_);
    return entered_;
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
};

TEST(Scheduling, FlushTakesPriorityOverCompaction) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  BlockingCompactionHook hook;
  o.compaction_hook = &hook;
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
  ASSERT_TRUE(hook.WaitEntered()) << "compaction 必须真的跑到输出注入点（否则空绿）";
  // compaction 卡在 OnOutputWritten 时，flush 必须仍能完成（L27 的固定优先级）。
  const uint64_t fl_before = impl->GetFlushStats().flushes_completed;
  for (int i = 241; i <= 320; ++i) ASSERT_TRUE(db->Put(K(i), V(i)).ok());
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  EXPECT_GT(impl->GetFlushStats().flushes_completed, fl_before)
      << "compaction 阻塞期间 flush 仍必须推进";
  EXPECT_TRUE(hook.entered());
  hook.Release();
  ASSERT_TRUE(WaitForCompaction(impl, 1));
  std::string v;
  for (int i = 1; i <= 320; ++i) {
    ASSERT_TRUE(db->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ================= M4.3 批 (ii)：A18/A20/A21/A31/A33/A38/A43 =================

// ---- A18：L0 内允许重叠，读必须按文件号新->旧逐个检查 ----
TEST(Read, L0NewestFirst) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 1000;   // 只测 L0 语义
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  for (int r = 1; r <= 3; ++r) {
    ASSERT_TRUE(db->Put(K(1), "v" + std::to_string(r)).ok());
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  std::string v;
  ASSERT_TRUE(db->Get(K(1), &v).ok());
  EXPECT_EQ("v3", v) << "必须读到文件号最大者的值";
  // tombstone 在新文件、值在旧文件
  ASSERT_TRUE(db->Delete(K(1)).ok());
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  EXPECT_TRUE(db->Get(K(1), &v).IsNotFound()) << "新文件的 tombstone 必须屏蔽旧文件的值";
  // 值在新文件、tombstone 在旧文件
  ASSERT_TRUE(db->Put(K(1), "v5").ok());
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  ASSERT_TRUE(db->Get(K(1), &v).ok());
  EXPECT_EQ("v5", v) << "新文件的值必须覆盖旧文件的 tombstone";
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A20：compaction 前后 Get/DBIter 与 std::map 全量对账 ----
TEST(Merge, StdMapReconciliation) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  impl->SetCompactionAutoForTest(false);   // A20 是确定性对账：compaction 用显式单轮驱动，避免并发安装
  test::Rng rng(12345);
  std::map<std::string, std::string> expect;
  for (int r = 0; r < 3; ++r) {
    for (int i = 0; i < 150; ++i) {
      const std::string k = "k" + std::to_string(rng.Uniform(200));
      if (rng.Uniform(5) == 0) {
        ASSERT_TRUE(db->Delete(k).ok());
        expect.erase(k);
      } else {
        const std::string val = "v" + std::to_string(rng.Next());
        ASSERT_TRUE(db->Put(k, val).ok());
        expect[k] = val;
      }
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  // 显式跑到没有可压的层为止（确定性；仍要求真的发生过 compaction）
  for (int k = 0; k < 32; ++k) impl->RunOneCompactionForTest();
  ASSERT_GE(impl->GetCompactionStats().completed, 1u) << "本用例必须真的发生 compaction";
  EXPECT_TRUE(impl->GetCompactionStats().last_error.empty());
  for (int i = 0; i < 200; ++i) {
    const std::string k = "k" + std::to_string(i);
    std::string v;
    const Status g = db->Get(k, &v);
    const auto it = expect.find(k);
    if (it == expect.end()) {
      EXPECT_TRUE(g.IsNotFound()) << k;
    } else {
      ASSERT_TRUE(g.ok()) << k;
      EXPECT_EQ(it->second, v) << k;
    }
  }
  std::unique_ptr<Iterator> it(db->NewIterator());
  std::map<std::string, std::string> got;
  for (it->SeekToFirst(); it->Valid(); it->Next()) got[it->key().ToString()] = it->value().ToString();
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(expect.size(), got.size());
  auto a = expect.begin();
  auto b = got.begin();
  for (; a != expect.end() && b != got.end(); ++a, ++b) {
    EXPECT_EQ(a->first, b->first);
    EXPECT_EQ(a->second, b->second);
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A21：输出文件内部 internal key 严格升序 + 跨文件 user key 严格递增 ----
TEST(Merge, InternalKeyOrderContract) {
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
  ASSERT_TRUE(WaitForCompaction(impl, 1));
  // 后台 compaction 可能在第一轮完成后立刻继续；持住当前 Version 引用，防止第二轮
  // 安装后回收其文件，避免 Table::Open 打在已 unlink 的路径上。断言前释放，保证
  // Unref hook 在 DB 仍存活时执行。
  std::shared_ptr<const Version> version = impl->RefCurrentVersionForTest();
  ASSERT_TRUE(version != nullptr);
  const InternalKeyComparator icmp(BytewiseComparator());
  for (int level = 0; level < kNumLevels; ++level) {
    const std::vector<FileMetaData>& files = version->level_files(level);
    std::string prev_largest_user;
    for (const FileMetaData& f : files) {
      std::shared_ptr<Table> t;
      ASSERT_TRUE(Table::Open(o, &env, TableFileName("/db", f.number), &t, &f.smallest, &f.largest).ok());
      std::unique_ptr<Iterator> it(t->NewIterator());
      std::string prev;
      bool first = true;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string cur = it->key().ToString();
        if (!first) { EXPECT_LT(icmp.Compare(Slice(prev), Slice(cur)), 0) << "文件 " << f.number << " 内序错乱"; }
        prev = cur;
        first = false;
      }
      EXPECT_TRUE(it->status().ok());
      if (level >= 1) {
        const std::string lo = Compaction::UserKeyOfInternal(f.smallest);
        if (!prev_largest_user.empty()) { EXPECT_LT(prev_largest_user, lo) << "跨文件 user key 必须严格递增"; }
        prev_largest_user = Compaction::UserKeyOfInternal(f.largest);
      }
    }
  }
  version.reset();
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A33：构造性证明「锁内以当前 version_ 重放 edit」（陈旧 base 不丢并发 flush 的文件）----
TEST(Install, RebaseOnConcurrentFlushKeepsI37) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 1000;   // 关闭自动 compaction，手工构造 rebase
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  auto put_batch = [&](int base, int n) {
    for (int i = 0; i < n; ++i) ASSERT_TRUE(db->Put(K(base + i), V(base + i)).ok());
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  };
  put_batch(1, 20);      // L0 f1
  put_batch(21, 20);     // L0 f2
  std::shared_ptr<const Version> stale = impl->RefCurrentVersionForTest();
  ASSERT_TRUE(stale != nullptr);
  ASSERT_GE(stale->level_files(0).size(), 2u);
  put_batch(41, 20);     // 并发 flush：L0 f3（version_ 前进，stale 落后）
  auto cur = impl->RefCurrentVersionForTest();
  ASSERT_TRUE(cur != nullptr);
  ASSERT_EQ(stale->level_files(0).size() + 1, cur->level_files(0).size());

  std::set<uint64_t> concurrent_added;
  {
    std::set<uint64_t> stale_nums;
    for (const FileMetaData& f : stale->level_files(0)) stale_nums.insert(f.number);
    for (const FileMetaData& f : cur->level_files(0)) {
      if (stale_nums.count(f.number) == 0) concurrent_added.insert(f.number);
    }
  }
  ASSERT_FALSE(concurrent_added.empty());
  const uint64_t before_retries = impl->GetCompactionStats().install_rebase_retries;
  VersionEdit edit;
  for (const FileMetaData& f : stale->level_files(0)) edit.DeleteFile(0, f.number);
  FileMetaData synth;
  synth.number = 999999;
  synth.file_size = 100;
  synth.max_sequence = 1;
  synth.smallest = BuildInternalKey("zzz-a", 1, kTypeValue);
  synth.largest = BuildInternalKey("zzz-b", 1, kTypeValue);
  edit.AddFile(1, synth);
  std::shared_ptr<const Version> out;
  const Status s = impl->LogAndApplyForTest(edit, stale, &out);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_GT(impl->GetCompactionStats().install_rebase_retries, before_retries)
      << "陈旧 base 必须触发 rebase 计数";
  ASSERT_TRUE(out != nullptr);
  ASSERT_EQ(concurrent_added.size(), out->level_files(0).size())
      << "并发 flush 新增的 L0 文件必须原样保留（陈旧 base 不得覆盖）";
  for (const FileMetaData& f : out->level_files(0)) {
    EXPECT_EQ(1u, concurrent_added.count(f.number)) << "只允许并发新增的文件留在 L0";
  }
  EXPECT_EQ(1u, out->level_files(1).size());
  EXPECT_EQ(cur->AllFiles().size() - stale->level_files(0).size() + 1u, out->AllFiles().size())
      << "总文件数 = 并发后的全量 - 输入 + 新增 L1（无丢失）";
  stale.reset();
  cur.reset();
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ---- A38：Close 幂等 + 两个后台线程 join + 延迟删除队列被处理 ----
TEST(Shutdown, JoinsBothThreadsAndDrainsQueue) {
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
  ASSERT_TRUE(WaitForCompaction(impl, 1));
  ASSERT_TRUE(db->Close().ok()) << "第一次 Close 必须成功";
  EXPECT_EQ(0u, impl->pending_delete_size()) << "Close 必须处理延迟删除队列";
  ASSERT_TRUE(db->Close().ok()) << "重复 Close 必须幂等";
  delete db;
}

// ---- A43：compaction 中途失败的未注册输出（.sst.tmp）必须被当孤儿清理并计数 ----
TEST(Orphan, CompactionOutputCleanedAndCounted) {
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
  std::set<uint64_t> registered;
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FileMetaData& f : impl->LevelFilesForTest(l)) registered.insert(f.number);
  }
  // rename 成功、随后 SyncDir 失败 ⇒ 磁盘上留下**未注册的 .sst**（A43 的孤儿来源）
  env.fail_sync_dir_remaining = 1;
  impl->RunOneCompactionForTest();
  std::vector<std::string> children;
  ASSERT_TRUE(env.GetChildren("/db", &children).ok());
  size_t orphan = 0;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseTableFileName(c, &n) && registered.count(n) == 0) ++orphan;
  }
  EXPECT_GE(orphan, 1u) << "compaction 失败必须留下未注册的 .sst（本用例的前提）";
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok());
  const RecoveryStats st = static_cast<PersistentDBImpl*>(db2)->GetRecoveryStats();
  EXPECT_GE(st.orphan_sst_removed, 1u) << "未注册输出必须被当孤儿清理并计数";
  std::string v;
  for (int i = 1; i <= 180; ++i) {
    ASSERT_TRUE(db2->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===== A20-并发：后台 compaction 在迭代期间完成安装（确定性屏障；不使用 sleep）=====
class PauseBeforeInstallHook : public CompactionHook {
 public:
  void OnBeforeInstall() override {
    std::unique_lock<std::mutex> l(mu_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(l, [this] { return released_; });
  }
  bool WaitEntered(int spins = 8000000) {
    for (int i = 0; i < spins; ++i) {
      { std::lock_guard<std::mutex> l(mu_); if (entered_) return true; }
      std::this_thread::yield();
    }
    return false;
  }
  void Release() {
    std::lock_guard<std::mutex> l(mu_);
    released_ = true;
    cv_.notify_all();
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
};

TEST(Merge, ConcurrentIteratorVsBackgroundCompaction) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  PauseBeforeInstallHook hook;
  o.compaction_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  std::map<std::string, std::string> expect;
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 80; ++i) {
      const int idx = round * 80 + i;
      ASSERT_TRUE(db->Put(K(idx), V(idx)).ok());
      expect[K(idx)] = V(idx);
    }
    ASSERT_TRUE(impl->ForceFlushForTest().ok());
  }
  ASSERT_TRUE(hook.WaitEntered()) << "compaction 必须在 install 前被挡住（否则本用例空绿）";
  std::unique_ptr<Iterator> it(db->NewIterator());   // 持住安装前的 Version
  std::shared_ptr<const Version> held = impl->RefCurrentVersionForTest();
  hook.Release();
  ASSERT_TRUE(WaitForCompaction(impl, 1)) << "安装必须在迭代存活期间完成";
  EXPECT_GT(impl->GetCompactionStats().install_rebase_retries + impl->GetCompactionStats().completed, 0u);
  // 迭代期间 version_ 已换出：结果仍必须与期望逐字节一致
  std::map<std::string, std::string> got;
  std::vector<std::string> dup;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string k = it->key().ToString();
    const std::string v = it->value().ToString();
    auto ins = got.emplace(k, v);
    if (!ins.second) dup.push_back(k);
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_TRUE(dup.empty()) << "迭代器对同一 user key 输出了多次：" << (dup.empty() ? "" : dup[0]);
  EXPECT_EQ(expect.size(), got.size());
  auto a = expect.begin();
  auto b = got.begin();
  for (; a != expect.end() && b != got.end(); ++a, ++b) {
    EXPECT_EQ(a->first, b->first);
    EXPECT_EQ(a->second, b->second);
  }
  std::string v;
  for (int i = 1; i <= 240; ++i) {
    ASSERT_TRUE(db->Get(K(i), &v).ok()) << "key " << i;
    EXPECT_EQ(V(i), v);
  }
  held.reset();
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===== A20-RED 候选：2 个内存表 + 1 个文件，同 user key 的 tombstone 与旧值混合 =====
// 构造不依赖 DB 时序；断言 全量扫描 / Seek / 语义真值 三者一致（tombstone 必须屏蔽旧值）。
namespace {
std::shared_ptr<Table> MakeTableFile(MemEnv* env, const std::string& path, const std::string& user,
                                     SequenceNumber seq, const std::string& value) {
  Options o;
  WritableFile* raw = nullptr;
  EXPECT_TRUE(env->NewWritableFile(path, &raw).ok());
  if (raw == nullptr) return nullptr;
  {
    std::unique_ptr<WritableFile> f(raw);
    TableBuilder b(o, f.get());
    EXPECT_TRUE(b.Add(Slice(BuildInternalKey(user, seq, kTypeValue)), Slice(value)).ok());
    EXPECT_TRUE(b.Finish().ok());
    EXPECT_TRUE(f->Close().ok());
  }
  std::shared_ptr<Table> t;
  EXPECT_TRUE(Table::Open(o, env, path, &t).ok());
  return t;
}
}  // namespace

TEST(Merge, MixedMemTablesAndFileTombstoneVisibility) {
  const InternalKeyComparator icmp(BytewiseComparator());
  const std::string key = "mix-key";
  MemEnv env;
  auto make_it = [&](std::vector<Iterator*> kids) -> Iterator* {
    Iterator** arr = new Iterator*[kids.size()];
    for (size_t i = 0; i < kids.size(); ++i) arr[i] = kids[i];
    MergingIterator* m = new MergingIterator(&icmp, arr, static_cast<int>(kids.size()));
    return new DBIter(&icmp, m, 100, {});
  };
  // 方向 0：旧值在文件、tombstone 在内存（两个内存表 + 一个文件）
  MemTable m1(icmp, 1u << 20);
  MemTable m2(icmp, 1u << 20);
  ASSERT_TRUE(m1.Add(10, kTypeValue, key, "old").ok());
  ASSERT_TRUE(m2.Add(20, kTypeDeletion, key, "").ok());
  std::shared_ptr<Table> tf = MakeTableFile(&env, "/f0.sst", key, 5, "oldest");
  ASSERT_TRUE(tf != nullptr);
  {
    std::unique_ptr<Iterator> it(make_it({m1.NewIterator(), m2.NewIterator(), tf->NewIterator().release()}));
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid()) << "方向0 全量扫描：内存 tombstone 必须屏蔽文件旧值";
  }
  {
    std::unique_ptr<Iterator> it(make_it({m1.NewIterator(), m2.NewIterator(), tf->NewIterator().release()}));
    it->Seek(Slice(key));
    EXPECT_FALSE(it->Valid()) << "方向0 Seek：内存 tombstone 必须屏蔽文件旧值";
  }
  // 方向 1：tombstone 在文件、旧值在两个内存表
  MemTable m3(icmp, 1u << 20);
  MemTable m4(icmp, 1u << 20);
  ASSERT_TRUE(m3.Add(10, kTypeValue, key, "old").ok());
  ASSERT_TRUE(m4.Add(5, kTypeValue, key, "oldest").ok());
  {
    Options o;
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewWritableFile("/f1.sst", &raw).ok());
    {
      std::unique_ptr<WritableFile> f(raw);
      TableBuilder b(o, f.get());
      ASSERT_TRUE(b.Add(Slice(BuildInternalKey(key, 20, kTypeDeletion)), Slice()).ok());
      ASSERT_TRUE(b.Finish().ok());
      ASSERT_TRUE(f->Close().ok());
    }
  }
  std::shared_ptr<Table> tt;
  {
    Options o;
    ASSERT_TRUE(Table::Open(o, &env, "/f1.sst", &tt).ok());
  }
  ASSERT_TRUE(tt != nullptr);
  {
    std::unique_ptr<Iterator> it(make_it({m3.NewIterator(), m4.NewIterator(), tt->NewIterator().release()}));
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid()) << "方向1 全量扫描：文件 tombstone 必须屏蔽内存旧值";
  }
  {
    std::unique_ptr<Iterator> it(make_it({m3.NewIterator(), m4.NewIterator(), tt->NewIterator().release()}));
    it->Seek(Slice(key));
    EXPECT_FALSE(it->Valid()) << "方向1 Seek：文件 tombstone 必须屏蔽内存旧值";
  }
}

// ===== A20 最小确定性 RED：child 的 key() 视图会被它自己的 Next() 覆盖 =====
// 真实对应：TableIterator 跨块时旧块缓冲被替换 ⇒ DBIter 传给 skip 循环的 user_key 视图失效。
class VolatileKeyIterator : public Iterator {
 public:
  bool Valid() const override { return valid_; }
  void SeekToFirst() override {
    key_.assign(BuildInternalKey("m", 20, kTypeDeletion));   // 墓碑：m@20
    valid_ = true;
  }
  void SeekToLast() override { SeekToFirst(); }
  void Seek(const Slice&) override { SeekToFirst(); }
  void Next() override {
    key_.assign(BuildInternalKey("z", 1, kTypeValue));        // 覆盖同一缓冲 + 变为 Invalid
    valid_ = false;
  }
  void Prev() override { valid_ = false; }
  Slice key() const override { return Slice(key_); }
  Slice value() const override { return Slice(key_); }
  Status status() const override { return Status::OK(); }

 private:
  std::string key_;
  bool valid_ = false;
};

TEST(Merge, SkipAcrossChildrenWithVolatileChildKeyView) {
  const InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, 1u << 20);
  ASSERT_TRUE(mem.Add(10, kTypeValue, "m", "old-value").ok());
  std::vector<Iterator*> kids;
  kids.push_back(new VolatileKeyIterator());     // m@20 tombstone（key 视图会被 Next() 覆盖）
  kids.push_back(mem.NewIterator());             // m@10 value（跨 child）
  Iterator** arr = new Iterator*[kids.size()];
  for (size_t i = 0; i < kids.size(); ++i) arr[i] = kids[i];
  MergingIterator* merged = new MergingIterator(&icmp, arr, static_cast<int>(kids.size()));
  std::unique_ptr<Iterator> it(new DBIter(&icmp, merged, 100, {}));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid()) << "RED：跨 child 的整段跳过必须消费 m@10，不得 emit";
  EXPECT_TRUE(it->status().ok());
}

// ===== A31：放大行可复现 + 自洽（同一输入两次运行逐字段相等；两种口径从同一行复算）=====
namespace {
struct AmpRun {
  AmplificationStats stats;
  std::string line;
};
AmpRun RunAmpWorkload() {
  AmpRun out;
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  o.level0_file_num_compaction_trigger = 2;
  DB* db = nullptr;
  EXPECT_TRUE(DB::Open(o, "/db", &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);
  impl->SetCompactionAutoForTest(false);
  for (int round = 0; round < 3; ++round) {
    for (int i = 1; i <= 60; ++i) {
      const int idx = round * 60 + i;
      EXPECT_TRUE(db->Put(K(idx), V(idx)).ok());
    }
    EXPECT_TRUE(impl->ForceFlushForTest().ok());
  }
  for (int k = 0; k < 32; ++k) impl->RunOneCompactionForTest();
  std::string v;
  for (int i = 1; i <= 180; ++i) EXPECT_TRUE(db->Get(K(i), &v).ok());
  out.stats = impl->GetAmplificationStats();
  out.line = impl->FormatAmplLine("ALL");
  EXPECT_TRUE(db->Close().ok());
  delete db;
  return out;
}
}  // namespace

TEST(Amplification, RowReproducibleAndSelfConsistent) {
  const AmpRun a = RunAmpWorkload();
  const AmpRun b = RunAmpWorkload();
  EXPECT_GT(a.stats.user_logical_bytes, 0u);
  EXPECT_GT(a.stats.compaction_rounds, 0u) << "必须真的发生过 compaction";
  // 同输入重跑：计数类字段逐字段相等（时间维度不参与）
  EXPECT_EQ(a.stats.user_logical_bytes, b.stats.user_logical_bytes);
  EXPECT_EQ(a.stats.entry_bytes, b.stats.entry_bytes);
  EXPECT_EQ(a.stats.flush_write_bytes, b.stats.flush_write_bytes);
  EXPECT_EQ(a.stats.compact_write_bytes, b.stats.compact_write_bytes);
  EXPECT_EQ(a.stats.get_count, b.stats.get_count);
  EXPECT_EQ(a.stats.files_checked, b.stats.files_checked);
  EXPECT_EQ(a.stats.compaction_rounds, b.stats.compaction_rounds);
  EXPECT_EQ(a.stats.dropped_old_versions, b.stats.dropped_old_versions);
  EXPECT_EQ(a.stats.sst_bytes, b.stats.sst_bytes);
  EXPECT_EQ(a.stats.manifest_bytes, b.stats.manifest_bytes);
  EXPECT_EQ(a.stats.live_versions_max, b.stats.live_versions_max);
  // 自洽：从同一行的分母/分子复算两种写放大口径
  const double wa_total =
      static_cast<double>(a.stats.flush_write_bytes + a.stats.compact_write_bytes) /
      static_cast<double>(a.stats.user_logical_bytes);
  const double wa_excl =
      static_cast<double>(a.stats.flush_write_bytes) / static_cast<double>(a.stats.user_logical_bytes);
  double line_total = -1.0, line_excl = -1.0, line_read = -1.0;
  {
    const char* p1 = std::strstr(a.line.c_str(), "write_amp_total=");
    ASSERT_TRUE(p1 != nullptr);
    std::sscanf(p1, "write_amp_total=%lf", &line_total);
    const char* p2 = std::strstr(a.line.c_str(), "write_amp_excl_compact=");
    ASSERT_TRUE(p2 != nullptr);
    std::sscanf(p2, "write_amp_excl_compact=%lf", &line_excl);
    const char* p3 = std::strstr(a.line.c_str(), "read_amp_files_per_get=");
    ASSERT_TRUE(p3 != nullptr);
    std::sscanf(p3, "read_amp_files_per_get=%lf", &line_read);
  }
  EXPECT_NEAR(wa_total, line_total, 1e-6);   // 行内固定 %.6f 精度
  EXPECT_NEAR(wa_excl, line_excl, 1e-6);
  EXPECT_NEAR(static_cast<double>(a.stats.files_checked) / static_cast<double>(a.stats.get_count),
              line_read, 1e-6);
  // 同一输入重跑：行里除时间字段外必须逐字符一致
  const auto strip = [](std::string l) {
    for (const char* tag : {"compaction_round_p50_us=", "compaction_round_max_us="}) {
      const size_t pos = l.find(tag);
      if (pos != std::string::npos) {
        const size_t end = l.find(' ', pos);
        l.erase(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
      }
    }
    return l;
  };
  EXPECT_EQ(strip(a.line), strip(b.line)) << a.line << "\nvs\n" << b.line;
}

// ===== A35：安装临界区内无 unlink；删除晚于安装；只删已无 live 引用的文件 =====
class EventLogEnv : public MemEnv {
 public:
  std::atomic<uint64_t> deletes{0}, deletes_under_install{0}, deletes_under_db_mutex{0};
  std::mutex mu_;
  std::vector<uint64_t> deleted_ssts;

  Status DeleteFile(const std::string& fname) override {
    ++deletes;
    if (InstallMuHeldOnThisThread()) ++deletes_under_install;
    if (DbMutexHeldOnThisThread()) ++deletes_under_db_mutex;
    uint64_t n = 0;
    if (ParseTableFileName(fname, &n)) {
      std::lock_guard<std::mutex> l(mu_);
      deleted_ssts.push_back(n);
    }
    return MemEnv::DeleteFile(fname);
  }
};

TEST(Delete, NoUnlinkInsideInstallCriticalSection) {
  EventLogEnv env;
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
  ASSERT_TRUE(WaitForCompaction(impl, 1));
  impl->MaybeDeleteObsoleteFilesForTest();

  EXPECT_EQ(0u, env.deletes_under_install.load())
      << "I43/L24：安装临界区内不得发生 unlink";
  EXPECT_EQ(0u, env.deletes_under_db_mutex.load())
      << "L24/L26：unlink 不得持 DB 互斥锁";
  EXPECT_GE(env.deletes.load(), 1u) << "本用例必须真的有删除（否则空绿）";

  // 被删的 sst 必须已不在当前 Version 里（删除晚于安装）
  std::set<uint64_t> live;
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FileMetaData& f : impl->LevelFilesForTest(l)) live.insert(f.number);
  }
  std::lock_guard<std::mutex> l(env.mu_);
  for (uint64_t n : env.deleted_ssts) {
    EXPECT_EQ(0u, live.count(n)) << "被删文件 " << n << " 仍在当前 Version 里（删除早于安装）";
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

}  // namespace lsm
