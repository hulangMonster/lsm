// tests/batch_test.cpp —— M5.2 WriteBatch 编码 / 批提交 / WAL 一次写 / 崩溃原子性
//
// 契约来源：docs/m5-design.md §5（WriteBatch 接口、提交路径与原子性）、§4 §13（编码）、
//           §9.2 的 I51~I54、§10.1 的 M5-A11~A17、docs/protocol.md §13。
//
// A 组纪律（§10.1 末段 + §10.3 反空绿）：
//   * 零断言 TEST 块 = 0；禁用 DISABLED_/GTEST_SKIP/`|| true`；
//   * 每条「机制」用例都有**反向自检**：原子性用例必须先证「同样的批在无故障时整批可见」，
//     否则「0 条可见」会退化成恒真；
//   * 全部建在 MemEnv 上（零真实磁盘、零 flaky）；并发用例用 CommitHook 屏障而非 sleep。
#include "test_harness.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "db.h"
#include "db_impl.h"
#include "filename.h"
#include "memenv.h"
#include "memtable.h"
#include "util/coding.h"
#include "wal.h"
#include "write_batch.h"

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
std::string BatchKey(int batch, int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "b%04d_%04d", batch, i);
  return std::string(buf);
}

// 把 WriteBatch 解码成结构化条目（断言用）。
class Collector : public WriteBatch::Handler {
 public:
  struct Entry {
    ValueType type = kTypeValue;
    std::string key;
    std::string value;
  };
  void Put(const Slice& key, const Slice& value) override {
    entries.push_back(Entry{kTypeValue, key.ToString(), value.ToString()});
  }
  void Delete(const Slice& key) override {
    entries.push_back(Entry{kTypeDeletion, key.ToString(), std::string()});
  }
  std::vector<Entry> entries;
};

PersistentDBImpl* OpenPersistent(MemEnv* env, const std::string& name,
                                 size_t write_buffer_size = 8u * 1024 * 1024) {
  Options options;
  options.env = env;
  options.write_buffer_size = write_buffer_size;
  DB* db = nullptr;
  const Status s = DB::Open(options, name, &db);
  if (!s.ok() || db == nullptr) {
    ADD_FAILURE() << "DB::Open(" << name << ") 失败：" << s.ToString();
    delete db;
    return nullptr;
  }
  return static_cast<PersistentDBImpl*>(db);
}

// 目录里内容最长的那个 *.log（写完一批后记录都在它里面）。
std::string MainLogFile(Env* env, const std::string& dir) {
  std::vector<std::string> children;
  if (!env->GetChildren(dir, &children).ok()) return std::string();
  std::string best;
  uint64_t best_size = 0;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (!ParseLogFileName(c, &number)) continue;
    uint64_t size = 0;
    if (!env->GetFileSize(dir + "/" + c, &size).ok()) continue;
    if (size >= best_size) {
      best_size = size;
      best = c;
    }
  }
  return best;
}

int VisibleCount(DB* db, int n) {
  int visible = 0;
  for (int i = 0; i < n; ++i) {
    std::string v;
    if (db->Get(Key(i), &v).ok()) ++visible;
  }
  return visible;
}

// 半批检测：返回「0 < 可见条数 < per_batch」的批数（I51 的违例计数，必须恒为 0）。
int HalfVisibleBatches(DB* db, int batches, int per_batch) {
  int half = 0;
  for (int b = 0; b < batches; ++b) {
    int visible = 0;
    for (int i = 0; i < per_batch; ++i) {
      std::string v;
      if (db->Get(BatchKey(b, i), &v).ok()) ++visible;
    }
    if (visible > 0 && visible < per_batch) ++half;
  }
  return half;
}

}  // namespace

// ===========================================================================
// M5-A11 WriteBatch 编码往返（空批、单条、多条、混合、空 value、大 value、count 上下界）
// ===========================================================================
TEST(WriteBatch, EncodingRoundTrip) {
  // --- ① 空批：12B 头、count 0、sequence 0；Validate 必须拒绝 ---
  WriteBatch empty;
  EXPECT_EQ(0u, empty.Count());
  EXPECT_EQ(WriteBatch::kHeaderSize, empty.ByteSize());
  EXPECT_EQ(0u, empty.Sequence());
  EXPECT_EQ(WriteBatch::kHeaderSize, empty.Data().size());
  {
    uint32_t c = 123;
    size_t eb = 123;
    uint64_t ub = 123;
    const Status s = empty.Validate(&c, &eb, &ub);
    EXPECT_TRUE(s.IsInvalidArgument()) << "空批必须被拒绝：" << s.ToString();
  }
  // 空批的 Iterate 是 kCorruption（count == 0 对解析器而言就是畸形）——与 Validate 的
  // kInvalidArgument 分工不同（§5.1：Iterate 只管结构；§13.3：DB::Write 管策略）。
  {
    Collector col;
    EXPECT_TRUE(empty.Iterate(&col).IsCorruption());
  }

  // --- ② 混合批：Put / 空 value / Delete / 大 value ---
  WriteBatch b;
  b.Put(Slice("a"), Slice("A"));
  EXPECT_EQ(1u, b.Count());
  b.Put(Slice("b"), Slice(""));
  b.Delete(Slice("c"));
  const std::string big(9000, 'x');
  b.Put(Slice("d"), Slice(big));
  EXPECT_EQ(4u, b.Count());

  // 12B 头逐字节检查：sequence(8B LE) ‖ count(4B LE)
  {
    const Slice data = b.Data();
    EXPECT_EQ(0u, DecodeFixed64(data.data()));
    EXPECT_EQ(4u, DecodeFixed32(data.data() + 8));
    b.SetSequence(77);
    EXPECT_EQ(77u, b.Sequence());
    EXPECT_EQ(77u, DecodeFixed64(b.Data().data()));
    EXPECT_EQ(4u, DecodeFixed32(b.Data().data() + 8)) << "SetSequence 不得动 count";
  }

  // 逐条往返（Iterate）
  {
    Collector col;
    ASSERT_TRUE(b.Iterate(&col).ok());
    ASSERT_EQ(4u, col.entries.size());
    EXPECT_EQ(kTypeValue, col.entries[0].type);
    EXPECT_EQ("a", col.entries[0].key);
    EXPECT_EQ("A", col.entries[0].value);
    EXPECT_EQ("b", col.entries[1].key);
    EXPECT_EQ("", col.entries[1].value) << "空 value 必须能往返";
    EXPECT_EQ(kTypeDeletion, col.entries[2].type);
    EXPECT_EQ("c", col.entries[2].key);
    EXPECT_EQ("", col.entries[2].value) << "tombstone 不带 value 字段";
    EXPECT_EQ("d", col.entries[3].key);
    EXPECT_EQ(big, col.entries[3].value);
  }

  // entry_bytes 口径 = Σ(1 + varint(klen) + klen [+ varint(vlen) + vlen])
  {
    uint32_t c = 0;
    size_t eb = 0;
    uint64_t ub = 0;
    ASSERT_TRUE(b.Validate(&c, &eb, &ub).ok());
    EXPECT_EQ(4u, c);
    EXPECT_EQ(b.ByteSize() - WriteBatch::kHeaderSize, eb);
    // a(1)+A(1) + b(1)+""(0) + c(1) + d(1)+9000 = 9005
    EXPECT_EQ(9005u, ub) << "Σ(key.size+value.size) = 1+0+1+9000+1";
  }

  // --- ③ count 很大（> 8192）：count 字段与逐条解析都必须正确 ---
  WriteBatch many;
  const int kMany = 9000;
  for (int i = 0; i < kMany; ++i) many.Put(Slice(Key(i)), Slice(Val(i)));
  EXPECT_EQ(static_cast<size_t>(kMany), many.Count());
  {
    Collector col;
    ASSERT_TRUE(many.Iterate(&col).ok());
    ASSERT_EQ(static_cast<size_t>(kMany), col.entries.size());
    EXPECT_EQ(Key(0), col.entries[0].key);
    EXPECT_EQ(Key(kMany - 1), col.entries[kMany - 1].key);
    uint32_t c = 0;
    size_t eb = 0;
    uint64_t ub = 0;
    ASSERT_TRUE(many.Validate(&c, &eb, &ub).ok());
    EXPECT_EQ(static_cast<uint32_t>(kMany), c);
    EXPECT_EQ(many.ByteSize() - WriteBatch::kHeaderSize, eb);
    std::printf("M5_BATCH_ROUNDTRIP_ENTRIES %llu\n",
                static_cast<unsigned long long>(4 + kMany));
    std::printf("M5_BATCH_ENTRY_BYTES %llu\n", static_cast<unsigned long long>(eb));
  }

  // --- ④ count 上界：kMaxCount + 1 非法（Validate ⇒ kInvalidArgument；Iterate ⇒ kCorruption）---
  {
    std::string raw(WriteBatch::kHeaderSize, 0);
    std::string head;
    PutFixed64(&head, 1);
    raw.replace(0, 8, head);
    std::string cnt;
    PutFixed32(&cnt, WriteBatch::kMaxCount + 1);
    raw.replace(8, 4, cnt);
    WriteBatch over{Slice(raw)};
    EXPECT_EQ(static_cast<size_t>(WriteBatch::kMaxCount + 1), over.Count());
    uint32_t c = 0;
    size_t eb = 0;
    uint64_t ub = 0;
    EXPECT_TRUE(over.Validate(&c, &eb, &ub).IsInvalidArgument())
        << "count > kMaxCount 必须被拒绝";
    Collector col;
    EXPECT_TRUE(over.Iterate(&col).IsCorruption());
  }

  // --- ⑤ DB::Write 对 count == 0 / nullptr 返回 kInvalidArgument（内存模式 + 持久模式）---
  {
    DB* mem = nullptr;
    ASSERT_TRUE(DB::Open(Options(), "", &mem).ok());
    EXPECT_TRUE(mem->Write(WriteOptions(), &empty).IsInvalidArgument());
    EXPECT_TRUE(mem->Write(WriteOptions(), nullptr).IsInvalidArgument());
    delete mem;
  }
  MemEnv env;
  PersistentDBImpl* impl = OpenPersistent(&env, "/dbrt");
  ASSERT_NE(nullptr, impl);
  std::unique_ptr<DB> db(impl);
  EXPECT_TRUE(db->Write(WriteOptions(), &empty).IsInvalidArgument());
  EXPECT_TRUE(db->Write(WriteOptions(), nullptr).IsInvalidArgument());
  EXPECT_EQ(0u, impl->last_sequence()) << "被拒的批不得消耗 sequence";
  {
    const std::string logname = MainLogFile(&env, "/dbrt");
    ASSERT_FALSE(logname.empty()) << "DB::Open 必须已经建好当前 WAL 文件";
    EXPECT_EQ(0u, env.Contents("/dbrt/" + logname).size()) << "被拒的批不得写 WAL";
  }

  // 正常批写：整批可见、sequence 按 entry 数前进（I52）、值逐条一致
  ASSERT_TRUE(db->Write(WriteOptions(), &b).ok());
  EXPECT_EQ(4u, impl->last_sequence()) << "批内 sequence 必须按 entry 数前进";
  {
    std::string v;
    ASSERT_TRUE(db->Get("a", &v).ok());
    EXPECT_EQ("A", v);
    ASSERT_TRUE(db->Get("b", &v).ok());
    EXPECT_EQ("", v);
    ASSERT_TRUE(db->Get("c", &v).IsNotFound()) << "tombstone 遮蔽（本批里没有更早的值）";
    ASSERT_TRUE(db->Get("d", &v).ok());
    EXPECT_EQ(big, v);
  }
  std::printf("M5_BATCH_EMPTY_REJECTED 1\n");
}

// ===========================================================================
// M5-A11（超限）§13.3：ByteSize()+16 > kMaxLogicalRecordSize ⇒ kInvalidArgument 且不写 WAL
// ===========================================================================
TEST(WriteBatch, OverLimitRejectedBeforeAnyWrite) {
  WriteBatch b;
  b.Put(Slice("k"), Slice(std::string(WriteBatch::kMaxBytes, 'x')));
  EXPECT_GT(b.ByteSize(), WriteBatch::kMaxBytes);
  {
    uint32_t c = 0;
    size_t eb = 0;
    uint64_t ub = 0;
    const Status s = b.Validate(&c, &eb, &ub);
    EXPECT_TRUE(s.IsInvalidArgument()) << "超限批必须被拒绝：" << s.ToString();
  }

  MemEnv env;
  PersistentDBImpl* impl = OpenPersistent(&env, "/dbov");
  ASSERT_NE(nullptr, impl);
  std::unique_ptr<DB> db(impl);
  const Status s = db->Write(WriteOptions(), &b);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(0u, impl->last_sequence());
  {
    const std::string logname = MainLogFile(&env, "/dbov");
    ASSERT_FALSE(logname.empty());
    EXPECT_EQ(0u, env.Contents("/dbov/" + logname).size()) << "超限批不得写 WAL";
  }

  DB* mem = nullptr;
  ASSERT_TRUE(DB::Open(Options(), "", &mem).ok());
  EXPECT_TRUE(mem->Write(WriteOptions(), &b).IsInvalidArgument());
  delete mem;
  std::printf("M5_BATCH_OVERLIMIT_REJECTED 1\n");
  std::printf("M5_BATCH_OVERLIMIT_BYTES %llu\n", static_cast<unsigned long long>(b.ByteSize()));
}

// ===========================================================================
// M5-A13 容量上界实测：ApproximateMemoryUsage 增量 ≤ Σ(entry_bytes + kMemTableNodeOverhead)
// ===========================================================================
TEST(Batch, CapacityBoundUpperBoundMeasured) {
  const size_t kNodeOverhead = 128;   // = db_impl.cpp 的 kMemTableNodeOverhead（§5.4 第 4 步）
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, 64u * 1024 * 1024);

  WriteBatch b;
  Rng rng(0x5EED2026u);
  const int kEntries = 500;
  for (int i = 0; i < kEntries; ++i) {
    const std::string k = RandomKey(&rng, 1 + (i % 37));
    const std::string v = RandomValue(&rng, i % 53);
    b.Put(Slice(k), Slice(v));
  }
  uint32_t c = 0;
  size_t eb = 0;
  uint64_t ub = 0;
  ASSERT_TRUE(b.Validate(&c, &eb, &ub).ok());
  ASSERT_EQ(static_cast<uint32_t>(kEntries), c);
  EXPECT_EQ(b.ByteSize() - WriteBatch::kHeaderSize, eb)
      << "entry_bytes 必须精确等于 entries 的编码字节数";

  Collector col;
  ASSERT_TRUE(b.Iterate(&col).ok());
  ASSERT_EQ(static_cast<size_t>(kEntries), col.entries.size());

  const size_t before = mem.ApproximateMemoryUsage();
  SequenceNumber seq = 1;
  for (const Collector::Entry& e : col.entries) {
    ASSERT_TRUE(mem.Add(seq++, e.type, e.key, e.value).ok());
  }
  const size_t after = mem.ApproximateMemoryUsage();
  const size_t delta = after - before;
  const uint64_t bound = static_cast<uint64_t>(eb) + static_cast<uint64_t>(kNodeOverhead) * c;
  EXPECT_GT(delta, 0u) << "写入必须真的占用内存（否则本用例无意义）";
  EXPECT_LE(delta, bound) << "增量 " << delta << " 必须 ≤ 上界 " << bound
                          << "（§5.4 的 footprint 是 Arena 增量的上界）";
  std::printf("M5_BATCH_MEMTABLE_DELTA %llu\n", static_cast<unsigned long long>(delta));
  std::printf("M5_BATCH_MEMTABLE_BOUND %llu\n", static_cast<unsigned long long>(bound));
  std::printf("M5_BATCH_CAPACITY_SLACK_OK 1\n");
}

// ===========================================================================
// M5-A12 批原子性：Append 失败 / fsync 失败 ⇒ 整批不可见；容量边界 ⇒ 新表整批可见
// ===========================================================================
TEST(Batch, AtomicVisibilityUnderAppendAndSyncFailure) {
  const int kN = 8;

  // --- ⓪ 反向自检（先证「无故障 ⇒ 整批可见」，否则后面的「0 条可见」恒真）---
  {
    MemEnv env_ok;
    PersistentDBImpl* ok = OpenPersistent(&env_ok, "/dba0");
    ASSERT_NE(nullptr, ok);
    std::unique_ptr<DB> db(ok);
    WriteBatch b;
    for (int i = 0; i < kN; ++i) b.Put(Slice(Key(i)), Slice(Val(i)));
    WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Write(wo, &b).ok());
    EXPECT_EQ(kN, VisibleCount(db.get(), kN)) << "反向自检：无故障时同一批必须整批可见";
  }

  // --- ① Append 失败（ENOSPC）：Write 返回错误、整批不可见、崩溃重开后仍完全不可见 ---
  {
    MemEnv env;
    PersistentDBImpl* impl = OpenPersistent(&env, "/dba1");
    ASSERT_NE(nullptr, impl);
    {
      std::unique_ptr<DB> db(impl);
      env.SetEnospc(true);
      WriteBatch b;
      for (int i = 0; i < kN; ++i) b.Put(Slice(Key(i)), Slice(Val(i)));
      WriteOptions wo;
      wo.sync = true;
      const Status s = db->Write(wo, &b);
      EXPECT_FALSE(s.ok()) << "Append 失败必须返回错误（否则「0 可见」是恒真）";
      EXPECT_EQ(0, VisibleCount(db.get(), kN))
          << "Append 失败的批在内存里必须整批不可见（I51）";
      env.SetEnospc(false);
      // 不 ASSERT Close().ok()：bg_error_ 是粘性 fail-stop（D11），Close 返回同一个错误是预期行为。
      db->Close();
    }
    env.SimulateCrash();
    PersistentDBImpl* impl2 = OpenPersistent(&env, "/dba1");
    ASSERT_NE(nullptr, impl2);
    std::unique_ptr<DB> db2(impl2);
    EXPECT_EQ(0, VisibleCount(db2.get(), kN)) << "Append 失败的批崩溃重开后必须完全不可见";
    EXPECT_EQ(0u, impl2->GetRecoveryStats().entries_replayed);
    std::printf("M5_BATCH_APPEND_FAIL_INVISIBLE 1\n");
  }

  // --- ② fsync 失败：Write 返回错误、整批在内存里不可见 ---
  {
    MemEnv env;
    PersistentDBImpl* impl = OpenPersistent(&env, "/dba2");
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    env.SetSyncFailureAfter(1);   // 下一次 Sync 开始失败
    WriteBatch b;
    for (int i = 0; i < kN; ++i) b.Put(Slice(Key(i)), Slice(Val(i)));
    WriteOptions wo;
    wo.sync = true;
    const Status s = db->Write(wo, &b);
    EXPECT_FALSE(s.ok()) << "sync=true 时 fsync 失败必须返回错误";
    EXPECT_EQ(0, VisibleCount(db.get(), kN)) << "fsync 失败的批必须整批不可见（I51）";
    env.ClearSyncFailures();
    std::printf("M5_BATCH_FSYNC_FAIL_VISIBLE 0\n");
  }

  // --- ③ 掉电（MemEnv 撕裂模型，固定种子）：批要么全在、要么全不在 —— 半批必须为 0 ---
  {
    const int kBatches = 6;
    const int kPer = 5;
    int half_total = 0;
    int cases = 0;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
      MemEnv env;
      env.SetSeed(seed);
      env.SetTearProbability(0.5);
      {
        PersistentDBImpl* impl = OpenPersistent(&env, "/dba3");
        ASSERT_NE(nullptr, impl);
        std::unique_ptr<DB> db(impl);
        for (int bi = 0; bi < kBatches; ++bi) {
          WriteBatch b;
          for (int i = 0; i < kPer; ++i) {
            b.Put(Slice(BatchKey(bi, i)), Slice(Val(bi * 100 + i)));
          }
          ASSERT_TRUE(db->Write(WriteOptions(), &b).ok());
        }
        env.SimulateCrash();   // 掉电：未 fsync 的后缀丢失 + 可能撕裂
      }
      PersistentDBImpl* impl2 = OpenPersistent(&env, "/dba3");
      ASSERT_NE(nullptr, impl2);
      std::unique_ptr<DB> db2(impl2);
      half_total += HalfVisibleBatches(db2.get(), kBatches, kPer);
      ++cases;
    }
    EXPECT_EQ(0, half_total) << "掉电后不得出现半批（I51）";
    EXPECT_EQ(8, cases);
    std::printf("M5_BATCH_CRASH_CASES %d\n", cases);
    std::printf("M5_BATCH_CRASH_HALF_VISIBLE %d\n", half_total);
  }

  // --- ④ 容量边界：整批 footprint 刚超 write_buffer_size ⇒ 换新表，整批仍可见 ---
  {
    const int kBatches = 6;
    const int kPer = 16;
    MemEnv env;
    PersistentDBImpl* impl = OpenPersistent(&env, "/dba4", 8192);
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    const std::string val(160, 'z');   // 16 × ~170B ≈ 2.7 KiB/批
    for (int bi = 0; bi < kBatches; ++bi) {
      WriteBatch b;
      for (int i = 0; i < kPer; ++i) b.Put(Slice(BatchKey(bi, i)), Slice(val));
      const Status s = db->Write(WriteOptions(), &b);
      ASSERT_TRUE(s.ok()) << "容量不足必须靠冻结消化，不得返回 kFrozen：" << s.ToString();
    }
    EXPECT_EQ(0, HalfVisibleBatches(db.get(), kBatches, kPer));
    for (int bi = 0; bi < kBatches; ++bi) {
      for (int i = 0; i < kPer; ++i) {
        std::string v;
        ASSERT_TRUE(db->Get(BatchKey(bi, i), &v).ok()) << "key " << BatchKey(bi, i);
        EXPECT_EQ(val, v);
      }
    }
    EXPECT_EQ(static_cast<SequenceNumber>(kBatches * kPer), impl->last_sequence());
    std::printf("M5_BATCH_CAPACITY_BOUNDARY_VISIBLE 1\n");
  }
  std::printf("M5_BATCH_PARTIAL_VISIBLE 0\n");
}

// ===========================================================================
// M5-A14 批 + 组提交：确定性屏障下 N 个并发批 = 1 次 fsync，无丢唤醒，水位覆盖整批
// ===========================================================================
TEST(Batch, GroupCommitOneFsyncForConcurrentBatches) {
  const int kWriters = 32;
  const int kPer = 4;
  MemEnv env;
  class BarrierHook : public CommitHook {
   public:
    void OnBeforeGroupAssemble() override {
      if (!entered.exchange(true)) {
        while (!released.load()) std::this_thread::yield();
        first_depth.store(db->pending_writers());
      }
    }
    std::atomic<bool> entered{false};
    std::atomic<size_t> first_depth{0};
    std::atomic<bool> released{false};
    PersistentDBImpl* db = nullptr;
  } hook;
  Options options;
  options.env = &env;
  options.commit_hook = &hook;
  DB* raw = nullptr;
  ASSERT_TRUE(DB::Open(options, "/dbg", &raw).ok());
  std::unique_ptr<DB> db(raw);
  hook.db = static_cast<PersistentDBImpl*>(raw);

  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      WriteBatch b;
      for (int i = 0; i < kPer; ++i) {
        b.Put(Slice(Key(w * kPer + i)), Slice(Val(w * kPer + i)));
      }
      WriteOptions wo;
      wo.sync = true;
      if (db->Write(wo, &b).ok()) ++ok_count;
    });
  }
  for (int spin = 0; spin < 200000; ++spin) {
    if (static_cast<PersistentDBImpl*>(raw)->pending_writers() >= static_cast<size_t>(kWriters)) {
      break;
    }
    std::this_thread::yield();
  }
  hook.released.store(true);
  for (std::thread& t : ts) t.join();

  EXPECT_EQ(kWriters, hook.first_depth.load())
      << "确定性屏障下首批必须含全部 " << kWriters << " 个批（实测 " << hook.first_depth.load()
      << "）";
  EXPECT_EQ(kWriters, ok_count.load()) << "全部写者都必须被唤醒并结算（无丢唤醒，L9/L10）";
  EXPECT_EQ(1, env.sync_calls()) << "整批只应做一次 fsync（实测 " << env.sync_calls() << " 次）";
  EXPECT_EQ(static_cast<SequenceNumber>(kWriters * kPer),
            static_cast<PersistentDBImpl*>(raw)->durable_seq())
      << "一次 fsync 覆盖整批 ⇒ durable 水位必须一步跳到 entry 总数（I54）";
  EXPECT_EQ(static_cast<SequenceNumber>(kWriters * kPer),
            static_cast<PersistentDBImpl*>(raw)->last_sequence());
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPer; ++i) {
      const std::string k = Key(w * kPer + i);
      std::string v;
      ASSERT_TRUE(db->Get(k, &v).ok()) << "key " << k;
      EXPECT_EQ(Val(w * kPer + i), v);
    }
  }
  std::printf("M5_BATCH_CONCURRENT_WRITERS %d\n", kWriters);
  std::printf("M5_BATCH_CONCURRENT_ENTRIES %d\n", kWriters * kPer);
  std::printf("M5_BATCH_GROUP_FSYNCS %d\n", env.sync_calls());
  std::printf("M5_BATCH_LOST_WAKEUPS %d\n", kWriters - ok_count.load());
  std::printf("M5_BATCH_DURABLE_COVERED 1\n");
  ASSERT_TRUE(db->Close().ok());
}

// ===========================================================================
// M5-A15 WAL 批记录逐字节截断：截断后该批完全不可见，完整时全部可见（不得出现半批）
// ===========================================================================
TEST(Batch, WalRecordTruncationNeverExposesHalfBatch) {
  const int kN = 6;
  MemEnv src;
  std::vector<std::string> children;
  {
    PersistentDBImpl* impl = OpenPersistent(&src, "/dbt");
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    WriteBatch b;
    for (int i = 0; i < kN; ++i) b.Put(Slice(Key(i)), Slice(Val(i)));
    WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Write(wo, &b).ok());
    ASSERT_TRUE(db->Close().ok());
  }
  ASSERT_TRUE(src.GetChildren("/dbt", &children).ok());
  const std::string logname = MainLogFile(&src, "/dbt");
  ASSERT_FALSE(logname.empty());
  const std::string logbytes = src.Contents("/dbt/" + logname);
  ASSERT_GT(logbytes.size(), 0u) << "必须真的写出了 WAL 记录";

  std::vector<size_t> cuts;
  cuts.push_back(0);
  cuts.push_back(1);
  cuts.push_back(6);
  cuts.push_back(7);
  cuts.push_back(8);
  cuts.push_back(logbytes.size() / 2);
  cuts.push_back(logbytes.size() - 1);
  cuts.push_back(logbytes.size());   // 完整

  int half = 0;
  int cases = 0;
  int full_visible_cases = 0;
  for (size_t cut : cuts) {
    MemEnv e2;
    ASSERT_TRUE(e2.CreateDir("/dbt").ok());
    for (const std::string& c : children) {
      if (c == logname) continue;
      e2.SetContents("/dbt/" + c, src.Contents("/dbt/" + c));
    }
    e2.SetContents("/dbt/" + logname, logbytes.substr(0, cut));
    Options options;
    options.env = &e2;
    DB* raw = nullptr;
    const Status s = DB::Open(options, "/dbt", &raw);
    ASSERT_TRUE(s.ok()) << "截断到 " << cut << " 字节必须仍可打开（尾部残骸 ⇒ 安全截断）："
                        << s.ToString();
    std::unique_ptr<DB> db(raw);
    const int visible = VisibleCount(db.get(), kN);
    if (visible != 0 && visible != kN) ++half;
    if (cut == logbytes.size()) {
      EXPECT_EQ(kN, visible) << "完整记录必须整批可见";
      if (visible == kN) ++full_visible_cases;
    } else {
      EXPECT_EQ(0, visible) << "截断到 " << cut << " 字节后该批必须完全不可见（I53）";
    }
    ++cases;
  }
  EXPECT_EQ(0, half) << "任何截断点都不得产生半批";
  EXPECT_EQ(1, full_visible_cases) << "必须至少有一个「完整 ⇒ 全可见」的对照，否则本用例是空绿";
  std::printf("M5_BATCH_TRUNCATE_CASES %d\n", cases);
  std::printf("M5_BATCH_HALF_VISIBLE %d\n", half);
  std::printf("M5_BATCH_TRUNCATE_FULL_LOG_BYTES %llu\n",
              static_cast<unsigned long long>(logbytes.size()));
}

// ===========================================================================
// M5-A16 恢复：批内 sequence 连续、last_sequence 覆盖批内最大、一个 batch = 一条 WAL record
// ===========================================================================
TEST(Batch, RecoverySequenceAndOneRecordPerBatch) {
  const int sizes[3] = {3, 5, 2};
  const int kTotal = 10;
  MemEnv env;
  {
    PersistentDBImpl* impl = OpenPersistent(&env, "/dbr");
    ASSERT_NE(nullptr, impl);
    std::unique_ptr<DB> db(impl);
    SequenceNumber expect_seq = 0;
    for (int bi = 0; bi < 3; ++bi) {
      WriteBatch b;
      for (int i = 0; i < sizes[bi]; ++i) {
        const int id = bi * 100 + i;
        b.Put(Slice(Key(id)), Slice(Val(id)));
      }
      ASSERT_TRUE(db->Write(WriteOptions(), &b).ok()) << "批 " << bi;
      expect_seq += static_cast<SequenceNumber>(sizes[bi]);
      // I52：批内 sequence 连续，last_sequence 覆盖批内最大值
      EXPECT_EQ(expect_seq, impl->last_sequence()) << "批 " << bi;
    }
    EXPECT_EQ(static_cast<SequenceNumber>(kTotal), impl->last_sequence());
    // I53 / §13.2：3 次 DB::Write ⇒ 恰好 3 条 WAL record（在恢复侧用 records_replayed 复核）
    ASSERT_TRUE(db->Close().ok());
  }

  PersistentDBImpl* impl2 = OpenPersistent(&env, "/dbr");
  ASSERT_NE(nullptr, impl2);
  std::unique_ptr<DB> db2(impl2);
  const RecoveryStats rs = impl2->GetRecoveryStats();
  EXPECT_EQ(3u, rs.records_replayed) << "一个 WriteBatch 必须落在恰好一条 WAL record 里";
  EXPECT_EQ(static_cast<uint64_t>(kTotal), rs.entries_replayed);
  EXPECT_EQ(static_cast<SequenceNumber>(kTotal), impl2->last_sequence());
  EXPECT_EQ(static_cast<SequenceNumber>(kTotal), rs.last_sequence);
  for (int bi = 0; bi < 3; ++bi) {
    for (int i = 0; i < sizes[bi]; ++i) {
      const int id = bi * 100 + i;
      std::string v;
      ASSERT_TRUE(db2->Get(Key(id), &v).ok()) << "key " << Key(id);
      EXPECT_EQ(Val(id), v);
    }
  }
  std::string v;
  EXPECT_TRUE(db2->Get(Key(301), &v).IsNotFound()) << "未写入的 key 不得凭空出现";
  std::printf("M5_BATCH_RECOVERY_RECORDS %llu\n",
              static_cast<unsigned long long>(rs.records_replayed));
  std::printf("M5_BATCH_RECOVERY_ENTRIES %llu\n",
              static_cast<unsigned long long>(rs.entries_replayed));
  std::printf("M5_BATCH_SEQ_CONTIGUOUS 1\n");
  std::printf("M5_BATCH_LAST_SEQ_COVERS 1\n");
  std::printf("M5_BATCH_ONE_RECORD_PER_BATCH 1\n");
}

// ===========================================================================
// M5-A17 所有权 / 复用 / 畸形 rep
// ===========================================================================
TEST(WriteBatch, OwnershipReuseAndMalformedRep) {
  MemEnv env;
  PersistentDBImpl* impl = OpenPersistent(&env, "/dbu");
  ASSERT_NE(nullptr, impl);
  std::unique_ptr<DB> db(impl);

  // ① 提交后复用同一对象
  WriteBatch b;
  b.Put(Slice("x"), Slice("X"));
  ASSERT_TRUE(db->Write(WriteOptions(), &b).ok());
  b.Clear();
  EXPECT_EQ(0u, b.Count());
  EXPECT_EQ(WriteBatch::kHeaderSize, b.ByteSize());
  b.Put(Slice("y"), Slice("Y"));
  ASSERT_TRUE(db->Write(WriteOptions(), &b).ok());
  {
    std::string v;
    ASSERT_TRUE(db->Get("x", &v).ok());
    EXPECT_EQ("X", v);
    ASSERT_TRUE(db->Get("y", &v).ok());
    EXPECT_EQ("Y", v);
  }

  // ② DB::Write 返回后修改同一对象不得影响已提交的数据
  WriteBatch c;
  c.Put(Slice("z"), Slice("Z1"));
  ASSERT_TRUE(db->Write(WriteOptions(), &c).ok());
  c.Clear();
  c.Put(Slice("z"), Slice("Z2"));   // 未提交
  {
    std::string v;
    ASSERT_TRUE(db->Get("z", &v).ok());
    EXPECT_EQ("Z1", v) << "DB::Write 不得保留对 WriteBatch 的引用（L31）";
  }

  // ③ DB::Write 忽略调用方的 SetSequence，在组提交里重新分配
  WriteBatch d;
  d.Put(Slice("w"), Slice("W"));
  d.SetSequence(999999);
  const SequenceNumber before = impl->last_sequence();
  ASSERT_TRUE(db->Write(WriteOptions(), &d).ok());
  EXPECT_EQ(before + 1, impl->last_sequence()) << "批的 sequence 由组提交分配，不看 SetSequence";

  // ④ 畸形 rep ⇒ Iterate 返回 kCorruption；DB::Write 也必须拒绝且不消耗 sequence
  int malformed = 0;
  {
    // (a) 头不足 12 字节
    WriteBatch m{Slice("short")};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  {
    // (b) count == 0
    const std::string raw(WriteBatch::kHeaderSize, 0);
    WriteBatch m{Slice(raw)};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  {
    // (c) count 超过 kMaxCount
    std::string raw(WriteBatch::kHeaderSize, 0);
    std::string cnt;
    PutFixed32(&cnt, WriteBatch::kMaxCount + 1);
    raw.replace(8, 4, cnt);
    WriteBatch m{Slice(raw)};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  {
    // (d) entry 截断：声称 2 条只给 1 条
    WriteBatch good;
    good.Put(Slice("p"), Slice("P"));
    std::string raw = good.Data().ToString();
    std::string cnt;
    PutFixed32(&cnt, 2);
    raw.replace(8, 4, cnt);
    WriteBatch m{Slice(raw)};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  {
    // (e) entry 解完后仍有剩余字节
    WriteBatch good;
    good.Put(Slice("q"), Slice("Q"));
    std::string raw = good.Data().ToString();
    raw.append("trailing");
    WriteBatch m{Slice(raw)};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  {
    // (f) 非法 type 字节
    std::string raw(WriteBatch::kHeaderSize, 0);
    std::string cnt;
    PutFixed32(&cnt, 1);
    raw.replace(8, 4, cnt);
    raw.push_back(static_cast<char>(0x7f));
    PutVarint32(&raw, 1);
    raw.push_back('k');
    WriteBatch m{Slice(raw)};
    Collector col;
    EXPECT_TRUE(m.Iterate(&col).IsCorruption());
    ++malformed;
  }
  EXPECT_EQ(6, malformed);
  {
    // DB::Write 对畸形批必须拒绝，且不得消耗 sequence
    std::string raw(WriteBatch::kHeaderSize, 0);
    std::string cnt;
    PutFixed32(&cnt, 3);   // 声称 3 条，一条都没有
    raw.replace(8, 4, cnt);
    WriteBatch m{Slice(raw)};
    const SequenceNumber seq_before = impl->last_sequence();
    const Status s = db->Write(WriteOptions(), &m);
    EXPECT_FALSE(s.ok()) << "畸形批必须被拒绝";
    EXPECT_EQ(seq_before, impl->last_sequence());
  }
  std::printf("M5_BATCH_REUSE_OK 1\n");
  std::printf("M5_BATCH_MALFORMED_CORRUPTION %d\n", malformed);
  ASSERT_TRUE(db->Close().ok());
}

}  // namespace test
}  // namespace lsm
