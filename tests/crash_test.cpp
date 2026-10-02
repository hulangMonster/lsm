// tests/crash_test.cpp —— 掉电语义用例（docs/m2-design.md §9.1 的 A27~A30）
//
// 为什么这些用例只能用 MemEnv：M2 #0 实测证明 kill -9 打断不了一次 write()
// （裸 write 逐条 100 轮 TAIL_TORN 0），所以「掉电丢多少」只能靠内存文件系统按
// fsync 水位 + 固定种子撕裂来确定性复现（memenv.h 的崩溃模型）。
//
// 判据（设计 §9.1）：崩溃后 ① 每个出现的 key 的值必须**逐字节等于某次完整写入**（无半写）；
// ② 出现的 key 集合必须是写入序列的**前缀**；③ 已 fsync 过的写一条都不能少。
#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "db_impl.h"   // durable_seq() 诊断接口在具体实现上（不进 DB 公共接口）
#include "memenv.h"
#include "util/coding.h"
#include "wal.h"

namespace lsm {
namespace {

using test::MemEnv;

const char* kDBName = "/db";

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%06d", i);
  return buf;
}
std::string Val(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%06d", i);
  return buf;
}

// 单条 entry 的 batch payload（与 db_impl.cpp 的 EncodeBatch 同格式，§9.4）
std::string Batch(SequenceNumber seq, const std::string& key, const std::string& val) {
  std::string out;
  PutFixed64(&out, seq);
  PutFixed32(&out, 1);
  out.push_back(static_cast<char>(kTypeValue));
  PutVarint32(&out, static_cast<uint32_t>(key.size()));
  out.append(key);
  PutVarint32(&out, static_cast<uint32_t>(val.size()));
  out.append(val);
  return out;
}

// 返回恢复后「前缀长度」；若某 key 出现但值不完整/不对，返回 -1（半写 ⇒ 直接判失败）
int CheckedPrefix(DB* db, int upto) {
  int prefix = 0;
  for (int i = 1; i <= upto; ++i) {
    std::string got;
    const Status s = db->Get(Key(i), &got);
    if (!s.ok()) {
      if (s.IsNotFound()) break;      // 前缀到此为止
      return -1;
    }
    if (got != Val(i)) return -1;     // 半写 / 值错乱
    ++prefix;
  }
  return prefix;
}

}  // namespace

// A27：未 fsync 的后缀可以丢，但绝不能出现半条值；已 fsync 的前缀一条都不能少
TEST(CrashSim, UnsyncedSuffixLostNoTornValue) {
  const uint64_t seeds[] = {0x5EED2026ull, 0x1ull, 0xDEADBEEFull};
  const double tears[] = {0.0, 0.3, 1.0};
  const int kSynced = 50;
  const int kUnsynced = 50;
  for (uint64_t seed : seeds) {
    for (double tear : tears) {
      MemEnv env;
      env.SetSeed(seed);
      env.SetTearProbability(tear);
      const std::string log = std::string(kDBName) + "/000001.log";
      {
        WALWriter w(&env, log);
        ASSERT_TRUE(w.Open(false).ok()) << "seed=" << seed << " tear=" << tear;
        for (int i = 1; i <= kSynced; ++i) {
          ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
        }
        ASSERT_TRUE(w.Sync().ok());
        for (int i = kSynced + 1; i <= kSynced + kUnsynced; ++i) {
          ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
        }
        // 注意：**不** Sync（这些就是"未 durable"的部分）
      }
      env.SimulateCrash();

      Options options;
      options.env = &env;
      DB* db = nullptr;
      const Status s = DB::Open(options, kDBName, &db);
      ASSERT_TRUE(s.ok()) << "崩溃后必须能启动（尾部残缺要能安全截断）：" << s.ToString()
                         << " seed=" << seed << " tear=" << tear;
      const int prefix = CheckedPrefix(db, kSynced + kUnsynced);
      EXPECT_GE(prefix, kSynced) << "已 fsync 的 " << kSynced << " 条一条都不能少（seed=" << seed
                                << " tear=" << tear << "）";
      EXPECT_LE(prefix, kSynced + kUnsynced);
      db->Close();
      delete db;
    }
  }
}

// A28：同一目录连续 50 轮「写 → 掉电 → 恢复」，每轮都要满足 A27 的三条
TEST(CrashSim, RepeatedCrashRecoverLoop50) {
  MemEnv env;
  env.SetSeed(0x5EED2026ull);
  env.SetTearProbability(0.5);
  Options options;
  options.env = &env;

  int seq = 0;
  int synced_upto = 0;
  for (int round = 1; round <= 50; ++round) {
    {
      DB* db = nullptr;
      ASSERT_TRUE(DB::Open(options, kDBName, &db).ok()) << "round=" << round;
      // 恢复出来的前缀必须覆盖上一轮已 fsync 的部分
      const int prefix = CheckedPrefix(db, seq);
      if (prefix < 0) {
        FAIL() << "round=" << round << " 出现半写值";
      }
      EXPECT_GE(prefix, synced_upto) << "round=" << round << " 已 fsync 的数据丢失";
      seq = prefix;   // 未 durable 的部分被丢弃 ⇒ 从恢复出的前缀继续写（模拟真实重试）

      WriteOptions wo;
      wo.sync = (round % 3 == 0);   // 每 3 轮 fsync 一次，制造"部分 durable"的历史
      for (int i = 0; i < 5; ++i) {
        ++seq;
        const Status s = db->Put(wo, Key(seq), Val(seq));
        ASSERT_TRUE(s.ok()) << "round=" << round << " seq=" << seq << " " << s.ToString();
      }
      if (wo.sync) synced_upto = seq;
      // 模拟掉电：**不调用 Close**（Close 会 fsync，那就不是掉电了）
      env.SimulateCrash();
      delete db;   // 此时 Close 只会 Sync 已被回滚的文件，不改变崩溃语义
    }
  }
  // 收尾：恢复后 synced_upto 之前的数据必须都在
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  EXPECT_GE(CheckedPrefix(db, seq), synced_upto);
  db->Close();
  delete db;
}

// A29：DB::Sync() 必须覆盖此前全部写入
TEST(Sync, SyncCoversAllPriorWrites) {
  MemEnv env;
  env.SetSeed(0x5EED2026ull);
  env.SetTearProbability(1.0);   // 最恶劣：任何未 fsync 的字节都可能只落一半
  Options options;
  options.env = &env;
  const int kN = 100;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
    WriteOptions wo;   // sync = false
    for (int i = 1; i <= kN; ++i) {
      ASSERT_TRUE(db->Put(wo, Key(i), Val(i)).ok());
    }
    ASSERT_TRUE(db->Sync().ok()) << "Sync 必须成功";
    env.SimulateCrash();
    db->Close();
    delete db;
  }
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  EXPECT_EQ(kN, CheckedPrefix(db, kN)) << "Sync 之后掉电，全部写入都必须在";
  db->Close();
  delete db;
}

// A30：Close() 隐含 Sync（关闭后掉电，已返回 kOk 的写必须都在）；Close 幂等
TEST(Sync, CloseIsDurable) {
  MemEnv env;
  env.SetSeed(0x5EED2026ull);
  env.SetTearProbability(1.0);
  Options options;
  options.env = &env;
  const int kN = 100;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
    WriteOptions wo;
    for (int i = 1; i <= kN; ++i) {
      ASSERT_TRUE(db->Put(wo, Key(i), Val(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
    EXPECT_TRUE(db->Close().ok()) << "Close 必须幂等";
    env.SimulateCrash();
    delete db;
  }
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  EXPECT_EQ(kN, CheckedPrefix(db, kN)) << "Close 之后掉电，全部写入都必须在";
  db->Close();
  delete db;
}


// A31：Close() 与并发写者的交互（I20/L11）——Close 期间不得 UAF、不得死锁；
// 并发写只能拿到明确的 Status；Close 返回之后不得再接受写入。
TEST(Close, RejectsNewWriters) {
  MemEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());

  std::atomic<bool> stop{false};
  std::atomic<int> ok_count{0};
  std::atomic<int> err_count{0};
  std::atomic<int> unexpected{0};
  auto worker = [&](int id) {
    WriteOptions wo;
    long long i = 0;
    while (!stop.load()) {
      char key[32];
      std::snprintf(key, sizeof(key), "t%d-%06lld", id, i++);
      const Status s = db->Put(wo, Slice(key), Slice("v"));
      if (s.ok()) {
        ++ok_count;
      } else {
        ++err_count;
        if (!(s.IsIOError() || s.IsFrozen() || s.IsInvalidArgument())) ++unexpected;
        break;   // 被拒之后退出（DB 转写只读）
      }
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back(worker, i);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_TRUE(db->Close().ok());      // 与 4 个写者并发
  stop.store(true);
  for (std::thread& t : threads) t.join();

  EXPECT_GT(ok_count.load(), 0) << "Close 之前应当有成功的写入";
  EXPECT_EQ(0, unexpected.load()) << "并发写只能拿到明确的错误 Status（IOError/Frozen/InvalidArgument）";
  EXPECT_GT(err_count.load(), 0) << "Close 之后仍在跑的写者必须被明确拒绝";

  // Close 返回之后的新写入必须被拒，且不得真的写进去
  const Status after = db->Put(WriteOptions(), "after-close", "v");
  EXPECT_FALSE(after.ok()) << "Close 之后不得再接受写入：" << after.ToString();
  std::string v;
  EXPECT_TRUE(db->Get("after-close", &v).IsNotFound());
  EXPECT_TRUE(db->Close().ok()) << "Close 必须幂等";
  delete db;
}


// ==== M2.3(b)：组提交专项用例（A20b/A21/A23/A24）====

namespace {

// 记录 flusher 内部的观察值（A24 用它证明「水位在 fsync 之前不会发布」）
class RecordingHook : public CommitHook {
 public:
  void OnGroupTaken() override { ++groups_taken; }
  void OnAfterSyncBeforePublish() override {
    ++sync_hooks;
    durable_at_hook = static_cast<PersistentDBImpl*>(db)->durable_seq();   // 此刻必须还没发布新水位
  }
  std::atomic<int> groups_taken{0};
  std::atomic<int> sync_hooks{0};
  std::atomic<SequenceNumber> durable_at_hook{0};
  DB* db = nullptr;
};

}  // namespace

// A24：fsync 已返回、水位尚未发布 —— 窗口的最后一步（清 flusher_active_）在更后面
TEST(GroupCommit, WindowNotOpenedEarly) {
  MemEnv env;
  RecordingHook hook;
  Options options;
  options.env = &env;
  options.commit_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  hook.db = db;
  WriteOptions wo;
  wo.sync = true;
  for (int i = 1; i <= 20; ++i) {
    ASSERT_TRUE(db->Put(wo, Key(i), Val(i)).ok());
  }
  EXPECT_GT(hook.sync_hooks.load(), 0) << "至少有一次 flusher 走到 fsync 之后的观察点";
  EXPECT_GT(hook.groups_taken.load(), 0);
  // 观察点上水位必须尚未包含本批（本批的 end_seq 会被发布在 hook 之后）
  EXPECT_LT(hook.durable_at_hook.load(), static_cast<PersistentDBImpl*>(db)->durable_seq())
      << "水位必须在 OnAfterSyncBeforePublish **之后**才发布（D4 窗口放开时机）";
  db->Close();
  delete db;
}

// A21：不得丢唤醒 —— 所有写者必须在有限时间内被结算（超时即 FAIL，不是「慢」）
TEST(GroupCommit, NoLostWakeup) {
  MemEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kWriters = 32;
  std::atomic<int> finished{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> ts;
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      WriteOptions wo;
      wo.sync = (w % 4 == 0);
      const Status s = db->Put(wo, Key(w + 1), Val(w + 1));
      if (s.ok()) ++finished; else ++failed;
    });
  }
  for (std::thread& t : ts) t.join();   // 真丢唤醒会在这里挂死 ⇒ 由测试超时/门禁暴露
  EXPECT_EQ(kWriters, finished.load() + failed.load()) << "有写者没有被结算（丢唤醒）";
  EXPECT_EQ(0, failed.load());
  // 已 ack 的写必须能读回（组提交不得丢数据）
  for (int i = 1; i <= kWriters; ++i) {
    std::string v;
    EXPECT_TRUE(db->Get(Key(i), &v).ok()) << "i=" << i;
  }
  db->Close();
  delete db;
}

// A23：fsync 失败必须传播给该批**所有**等待者，且没有任何人拿到 kOk（I16）
TEST(GroupCommit, FailurePropagatesToAllWaiters) {
  MemEnv env;
  env.SetSeed(0x5EED2026ull);
  env.SetSyncFailureAfter(1);   // 第一次 fsync 就失败
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kWriters = 16;
  std::atomic<int> ok_count{0};
  std::atomic<int> err_count{0};
  std::atomic<int> io_errors{0};
  std::vector<std::thread> ts;
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      WriteOptions wo;
      wo.sync = true;   // 要求 durable ⇒ 必然经过 fsync ⇒ 必然失败
      const Status s = db->Put(wo, Key(w + 1), Val(w + 1));
      if (s.ok()) ++ok_count; else { ++err_count; if (s.IsIOError()) ++io_errors; }
    });
  }
  for (std::thread& t : ts) t.join();
  EXPECT_EQ(0, ok_count.load()) << "fsync 失败时绝不允许有人拿到 kOk（I16）";
  EXPECT_EQ(kWriters, err_count.load());
  EXPECT_EQ(kWriters, io_errors.load()) << "应当全部是同一类错误（kIOError）";
  // 粘性：失败之后的写也立刻返回错误（写只读，D11）
  EXPECT_FALSE(db->Put(WriteOptions(), "after-failure", "v").ok());
  db->Close();
  delete db;
}

// A20b：真实并发下的统计（不作硬门禁，只记录比值；确定性判据见设计 §9.1 的 A20 修订）
TEST(GroupCommit, BatchingReducesFsyncCount) {
  MemEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kWriters = 32;
  std::vector<std::thread> ts;
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      WriteOptions wo;
      wo.sync = true;
      EXPECT_TRUE(db->Put(wo, Key(w + 1), Val(w + 1)).ok());
    });
  }
  for (std::thread& t : ts) t.join();
  const int fsyncs = env.sync_calls();
  std::printf("[   INFO   ] GroupCommit.BatchingReducesFsyncCount: writers=%d fsync_calls=%d ratio=%.3f\n",
              kWriters, fsyncs, static_cast<double>(fsyncs) / kWriters);
  // 本用例**只登记不设门禁**（这正是 #1 阶段对 A20 判据的修订精神：不作赌调度的硬断言）：
  // MemEnv 的 fsync 瞬时完成、写者之间没有重叠窗口 ⇒ ratio ≈ 1.0 是**预期**结果，
  // 不能据此判定"组提交无效"。真实合并效果必须由(a) 真实磁盘的 B 组脚本（fsync 中位 2.6ms）
  // 或 (b) 确定性屏障构造（设计 §9.1 的 A20，断言"本批含 N 个写者且 fsync 次数 == 1"）来证明。
  EXPECT_GT(fsyncs, 0) << "至少要有一次 fsync";
  db->Close();
  delete db;
}


// A20（确定性版，按 #1 阶段对原判据的修订）：用 CommitHook 屏障让 N 个写者全部入队后再放行组装 ——
// 断言「本批确实含 N 个写者」且「本批只做 1 次 fsync」。这不依赖调度，因此不是赌 flaky。
TEST(GroupCommit, NWritersOneFsyncDeterministic) {
  MemEnv env;
  class BarrierHook : public CommitHook {
   public:
    void OnBeforeGroupAssemble() override {
      if (!entered.exchange(true)) {                  // 只有首个 flusher 需要等整批就位
        while (!released.load()) std::this_thread::yield();
        first_depth.store(db->pending_writers());     // 放行瞬间队里有多少写者 ⇒ 本批成员数
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
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  hook.db = static_cast<PersistentDBImpl*>(db);

  const int kWriters = 64;
  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      WriteOptions wo;
      wo.sync = true;
      if (db->Put(wo, Key(w + 1), Val(w + 1)).ok()) ++ok_count;
    });
  }
  // 轮询队列深度等整批就位（有上限，避免实现坏掉时挂死）：此刻 flusher 正卡在屏障里、不持锁
  for (int spin = 0; spin < 200000; ++spin) {
    if (static_cast<PersistentDBImpl*>(db)->pending_writers() >= static_cast<size_t>(kWriters)) break;
    std::this_thread::yield();
  }
  hook.released.store(true);
  for (std::thread& t : ts) t.join();

  EXPECT_EQ(static_cast<size_t>(kWriters), hook.first_depth.load())
      << "确定性屏障下首批必须含全部写者（实测首批 " << hook.first_depth.load() << " 个）";
  EXPECT_EQ(kWriters, ok_count.load());
  EXPECT_EQ(1, env.sync_calls()) << "整批只应做一次 fsync（实测 " << env.sync_calls() << " 次）";
  // 一次 fsync 覆盖整批 ⇒ 水位必须一步跳到 kWriters
  EXPECT_EQ(static_cast<SequenceNumber>(kWriters), static_cast<PersistentDBImpl*>(db)->durable_seq());
  db->Close();
  delete db;
}


// ==== 评审回归（阻断项 1/2/6）====

// M3 契约变更（docs/m3-design.md §15 R2）：容量不足不再返回 kFrozen，而是「冻结 + 后台 flush
// + 写者停等」。本用例按新契约收窄为：自动落盘必须真的发生、写全部 kOk、数据可读。
// 为什么不能保留重开断言：M3.2 的显式过渡妥协②——Open 时目录里存在 *.sst 一律 kCorruption
// （SSTable 恢复属 M3.3），因此本阶段不重开已 flush 的库。
TEST(DB, PutBlocksUntilFlush) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 64 * 1024;   // 强制在 2000 条内触发多次 flush
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kCount = 2000;
  for (int i = 1; i <= kCount; ++i) {
    const Status s = db->Put(WriteOptions(), Key(i), Val(i));
    ASSERT_TRUE(s.ok()) << "M3 起容量不足必须靠 flush 消化，不得返回 kFrozen：" << s.ToString();
  }
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  for (int spin = 0; spin < 200000; ++spin) {
    if (impl->GetFlushStats().flushes_completed >= 1 && impl->immutables_size() == 0) break;
    std::this_thread::yield();
  }
  const FlushStats stats = impl->GetFlushStats();
  EXPECT_GE(stats.flushes_completed, 1u) << "小 write_buffer_size 下必须真的发生 flush";
  std::string v;
  ASSERT_TRUE(db->Get(Key(1), &v).ok()) << "落盘后的 key 必须仍可读";
  EXPECT_EQ(Val(1), v);
  ASSERT_TRUE(db->Get(Key(kCount), &v).ok());
  EXPECT_EQ(Val(kCount), v);
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// 阻断项 2：没有 fsync 时 durable 水位不得前进（design §7.1 的结构证据）
TEST(GroupCommit, DurableSeqOnlyAdvancesOnSync) {
  MemEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  WriteOptions unsynced;                      // sync = false
  for (int i = 1; i <= 8; ++i) ASSERT_TRUE(db->Put(unsynced, Key(i), Val(i)).ok());
  EXPECT_EQ(0u, static_cast<PersistentDBImpl*>(db)->durable_seq())
      << "只写不 fsync 时 durable 水位必须留在 0";
  WriteOptions synced;
  synced.sync = true;
  ASSERT_TRUE(db->Put(synced, Key(9), Val(9)).ok());
  EXPECT_GE(static_cast<PersistentDBImpl*>(db)->durable_seq(), 9u)
      << "有 fsync 的批必须把水位推上去";
  db->Close();
  delete db;
}

// 阻断项 6：Close 必须等到队列排空（>kMaxGroupRecs 才可能把写者留在批边界之外）
TEST(Close, WaitsForDrainedQueue) {
  MemEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kWriters = 100;                   // > kMaxGroupRecs(64) ⇒ 必然有写者被留在队列里
  std::vector<std::thread> ts;
  std::atomic<int> done{0};
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      db->Put(WriteOptions(), Key(w + 1), Val(w + 1));
      ++done;
    });
  }
  // 等到所有写者都返回（Close 之前不得留下未结算成员）
  while (done.load() < kWriters) std::this_thread::yield();
  ASSERT_TRUE(db->Close().ok());
  EXPECT_EQ(0u, static_cast<PersistentDBImpl*>(db)->pending_writers())
      << "Close 返回时队列必须已排空（否则 Close 后仍可能有写者进 RunFlusher）";
  for (std::thread& t : ts) t.join();
  delete db;
}


// A22：混合同批 —— 只要组内有一个人要求 sync，整批就必须 fsync（D3：sync 取组内 OR）。
// 用确定性屏障把「队首是 sync=false、组内混有 sync=true」这个最坏情形钉死：
// 若实现只看队首（LevelDB 的行为），本批不会 fsync ⇒ sync_calls()==0、durable_seq()==0 ⇒ 用例红。
TEST(GroupCommit, MixedSyncPropagates) {
  MemEnv env;
  class BarrierHook : public CommitHook {
   public:
    void OnBeforeGroupAssemble() override {
      if (!entered.exchange(true)) {
        while (!released.load()) std::this_thread::yield();
      }
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
  } hook;
  Options options;
  options.env = &env;
  options.commit_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());

  const int kWriters = 64;
  std::atomic<int> ok_count{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> ts;
  auto spawn = [&](int w) {
    ts.emplace_back([&, w]() {
      WriteOptions wo;
      wo.sync = (w % 4 == 0);          // 局面：队首 sync=false，组内混有 sync=true
      const Status s = db->Put(wo, Key(w + 1), Val(w + 1));
      if (s.ok()) ++ok_count; else ++failed;
    });
  };
  spawn(0);                            // 队首 = sync=false（最坏情形）
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  for (int w = 1; w < kWriters; ++w) spawn(w);
  for (int spin = 0; spin < 200000; ++spin) {
    if (static_cast<PersistentDBImpl*>(db)->pending_writers() >= static_cast<size_t>(kWriters)) break;
    std::this_thread::yield();
  }
  hook.released.store(true);
  for (std::thread& t : ts) t.join();

  EXPECT_EQ(kWriters, ok_count.load());
  EXPECT_EQ(0, failed.load());
  EXPECT_EQ(1, env.sync_calls())
      << "混合同批必须（且只）做一次 fsync：sync 取组内 OR，不能只看队首";
  EXPECT_EQ(static_cast<SequenceNumber>(kWriters), static_cast<PersistentDBImpl*>(db)->durable_seq())
      << "sync=true 的写者返回后，durable 水位必须覆盖整批末尾";
  db->Close();
  delete db;
}


// A25：I17「持锁零 IO」的探针式验证 —— 包一层 Env，在 Append/Sync 时断言此刻未持 DB 互斥锁
namespace {
class SpyFile : public WritableFile {
 public:
  SpyFile(WritableFile* inner, std::atomic<int>* violations)
      : inner_(inner), violations_(violations) {}
  ~SpyFile() override { delete inner_; }
  Status Append(const Slice& data) override {
    if (DbMutexHeldOnThisThread()) ++(*violations_);
    return inner_->Append(data);
  }
  Status Flush() override { return inner_->Flush(); }
  Status Sync() override {
    if (DbMutexHeldOnThisThread()) ++(*violations_);
    return inner_->Sync();
  }
  Status Close() override { return inner_->Close(); }

 private:
  WritableFile* inner_;
  std::atomic<int>* violations_;
};

class SpyEnv : public MemEnv {
 public:
  Status NewWritableFile(const std::string& f, WritableFile** r) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(f, &inner);
    if (s.ok()) *r = new SpyFile(inner, &violations);
    return s;
  }
  Status NewAppendableFile(const std::string& f, WritableFile** r) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(f, &inner);
    if (s.ok()) *r = new SpyFile(inner, &violations);
    return s;
  }
  std::atomic<int> violations{0};
};
}  // namespace

TEST(Locks, ZeroIoWhileHoldingDbMutex) {
  SpyEnv env;
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  const int kWriters = 8;
  const int kPerWriter = 200;
  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      for (int i = 0; i < kPerWriter; ++i) {
        WriteOptions wo;
        wo.sync = (i % 10 == 0);
        if (db->Put(wo, Key(w * kPerWriter + i + 1), Val(w * kPerWriter + i + 1)).ok()) {
          ++ok_count;
        }
        std::string v;
        db->Get(Key(w * kPerWriter + i + 1), &v);   // 顺带压一下读路径
        if (i % 50 == 0) {
          std::unique_ptr<Iterator> it(db->NewIterator());
          it->SeekToFirst();
        }
      }
    });
  }
  for (std::thread& t : ts) t.join();
  EXPECT_EQ(kWriters * kPerWriter, ok_count.load());
  EXPECT_EQ(0, env.violations.load())
      << "I17：持 DB 互斥锁期间发生了 WAL 的 write/fsync（" << env.violations.load() << " 次）";
  db->Close();
  delete db;
}

// ==== I32 回归（M2 补丁）：Sync() 不得把水位发布到「已分配 sequence、尚未 Append」的在飞批次 ====
//
// 窗口：RunFlusher 在锁内取批时就把 last_sequence_ 推进到本批末尾（:393），而 log_->Append 是锁外
// 做的（:411）。并发 Sync() 若在两者之间拿到 commit_mu_，就会 fsync 一份**不含本批**的日志，却把
// 水位发布到含本批 ⇒ Sync() 返回 kOk 而该批字节并未落盘，违反 db.h 对 Sync() 的「返回即 durable」契约。
// 注入点：OnGroupTaken（批次已取、sequence 已推进、Append 尚未执行）把该窗口确定化，不靠时序碰运气。
namespace {

// 为什么用原子布尔自旋而不是 mutex+condition_variable：本用例会让 flusher 线程停在
// OnGroupTaken 里（它正处在 Write→RunFlusher 的调用栈上），而主线程同时在 Sync()/Release()
// 里访问同一对象。condvar 版本在 TSan 下实测报 double lock + 2 条 data race（见
// docs/m2-evidence.md §TSan）。原子布尔 + 有限自旋无锁竞争、无 TSan 噪声，判定依然确定：
// taken_ 置位严格早于 Append，released_ 只由主线程单点置位。
class BlockingTakeHook : public CommitHook {
 public:
  void OnGroupTaken() override {
    taken_.store(true, std::memory_order_release);
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  bool WaitUntilTaken(std::chrono::milliseconds timeout) {
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (!taken_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  }
  void Release() { released_.store(true, std::memory_order_release); }

 private:
  std::atomic<bool> taken_{false};
  std::atomic<bool> released_{false};
};

}  // namespace

TEST(GroupCommit, SyncDoesNotClaimInFlightBatch) {
  MemEnv env;
  BlockingTakeHook hook;
  Options options;
  options.env = &env;
  options.commit_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, kDBName, &db).ok());
  auto* impl = static_cast<PersistentDBImpl*>(db);

  Status writer_status;
  std::thread writer([&] { writer_status = db->Put(WriteOptions(), Key(1), Val(1)); });

  ASSERT_TRUE(hook.WaitUntilTaken(std::chrono::milliseconds(5000)))
      << "flusher 未在 5s 内走到 OnGroupTaken（屏障失效，用例无法判定）";
  // 此刻本批的 sequence 已推进、字节尚未 Append ⇒ 本次 fsync 覆盖不到本批 ⇒ 水位不得包含它。
  const Status early = db->Sync();
  const SequenceNumber early_durable = impl->durable_seq();
  hook.Release();
  writer.join();
  ASSERT_TRUE(writer_status.ok()) << writer_status.ToString();

  const Status late = db->Sync();   // 本批已 Append，这次 fsync 确实覆盖它
  const SequenceNumber late_durable = impl->durable_seq();
  EXPECT_TRUE(early.ok()) << early.ToString();
  EXPECT_TRUE(late.ok()) << late.ToString();
  EXPECT_LT(early_durable, late_durable)
      << "I32：在飞批次尚未 Append，Sync() 却把水位发布到了它（early=" << early_durable
      << " late=" << late_durable << "）—— Sync() 返回 kOk 却在声称未落盘的记录已 durable";
  EXPECT_GT(late_durable, 0u) << "修复不得以「永不发布水位」的方式通过";
  db->Close();
  delete db;
}

// M6.10.5 (1)：A27 只断言了「未 fsync 的后缀**可以**丢」（prefix ∈ [kSynced, kSynced+kUnsynced]），
// 但从未断言它**真的丢了** —— 如果 SimulateCrash() 退化成 no-op，A27 依然会绿（空绿）。
// 本用例把「注入确实生效」本身变成判据，分三层：
//   ① 前置：场景必须成立（文件里真的存在一段未 fsync 的后缀）；
//   ② 介质层（正向标记）：掉电后当前 WAL 的字节数必须**回落到该文件的 fsync 水位**；
//   ③ 恢复层：能正常启动（不 Corruption），恢复出的前缀**恰好**等于已 fsync 的条数 ——
//      未 fsync 的 key 一条都不得"复活"到 durable 点之上（I11：sync=true 才是 durable-before-ack），
//      也不得出现半条值（CheckedPrefix 返回 -1 即判失败）。
// tear=0 是刻意的：把「回滚是否生效」与「撕裂尾如何截断」两件事分开验证。
//
// 边界（必须写明，不许外推）：MemEnv 没有目录项语义（见 tests/memenv.h 的注释：
// "RenameFile 一次赋值即永久，SimulateCrash 不碰 dirs_"），所以本用例覆盖的是**文件数据**层面的
// 掉电语义；dirent/rename 的掉电顺序仍无法在 MemEnv 建模（docs/m3-design.md §12.5 / P7）。
TEST(CrashSim, UnsyncedWalTailIsActuallyDiscarded) {
  MemEnv env;
  env.SetSeed(0x5EED2026ull);
  env.SetTearProbability(0.0);   // 干净回滚：未 fsync 的后缀整段丢失，不掺撕裂
  const std::string log = std::string(kDBName) + "/000001.log";
  const int kSynced = 40;
  const int kUnsynced = 60;
  uint64_t synced_bytes = 0;
  uint64_t full_bytes = 0;
  {
    WALWriter w(&env, log);
    ASSERT_TRUE(w.Open(false).ok());
    for (int i = 1; i <= kSynced; ++i) {
      ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
    }
    ASSERT_TRUE(w.Sync().ok());
    synced_bytes = env.SyncedSize(log);
    for (int i = kSynced + 1; i <= kSynced + kUnsynced; ++i) {
      ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
    }
    // 注意：**不** Sync。这些就是"已 ack 但未 durable"的部分。
  }
  full_bytes = env.Contents(log).size();

  // ① 前置：场景本身必须成立。
  ASSERT_GT(synced_bytes, 0u);
  ASSERT_GT(full_bytes, synced_bytes)
      << "前置条件不成立：未 fsync 的后缀并没有真的写进文件（后面全是空绿）";

  env.SimulateCrash();

  // ② 介质层正向标记：未 fsync 的后缀真的被丢掉了。
  EXPECT_EQ(env.Contents(log).size(), synced_bytes)
      << "SimulateCrash 必须把文件回滚到 fsync 水位，否则本用例证明不了任何事";
  EXPECT_EQ(env.SyncedSize(log), synced_bytes);

  // ③ 恢复层：不 Corruption、前缀恰好 = 已 fsync 的条数、被丢的 key 不得复活。
  Options options;
  options.env = &env;
  DB* db = nullptr;
  const Status s = DB::Open(options, kDBName, &db);
  ASSERT_TRUE(s.ok()) << "掉电后必须能启动（尾部丢失要能安全截断）：" << s.ToString();
  const int recovered = CheckedPrefix(db, kSynced + kUnsynced);
  EXPECT_EQ(recovered, kSynced)
      << "已 fsync 的 " << kSynced << " 条必须一条不少，未 fsync 的 " << kUnsynced
      << " 条必须一条不多";
  // 显式「丢失计数」正向标记：tear=0 是干净回滚 ⇒ 未 fsync 的 kUnsynced 条必须**全部**消失。
  //   少丢 ⇒ 注入没生效（空绿）；多丢 ⇒ 把已 durable 的也截掉了（违反 I11，等于静默截断到更早）。
  if (recovered >= 0) {
    const int lost = (kSynced + kUnsynced) - recovered;
    EXPECT_EQ(lost, kUnsynced)
        << "干净回滚下的丢失计数必须恰好等于未 fsync 的条数（实际 lost=" << lost << "）";
  }
  for (int i = kSynced + 1; i <= kSynced + kUnsynced; ++i) {
    std::string got;
    EXPECT_TRUE(db->Get(Key(i), &got).IsNotFound())
        << Key(i) << " 未 fsync 却在掉电后存活（等于谎报 durable）";
  }
  db->Close();
  delete db;
}

// M6.10.5 (1) 之二：跨 (seed, tear) 的**聚合**正向标记。
// 单看某一个 seed 可能恰好"什么都没丢"（那一条就一直绿着），所以这里要求：
//   * 每个组合都不得出现半条值（CheckedPrefix != -1）、已 fsync 前缀一条不少；
//   * **至少有一个组合真的丢了未 fsync 的记录** —— 否则说明注入压根没生效。
TEST(CrashSim, CrashInjectionEffectiveAcrossSeeds) {
  const uint64_t seeds[] = {0x5EED2026ull, 0x1ull, 0xDEADBEEFull};
  const double tears[] = {0.0, 0.5, 1.0};
  const int kSynced = 40;
  const int kUnsynced = 60;
  int lost_some = 0;
  int lost_total = 0;
  int combos = 0;
  for (uint64_t seed : seeds) {
    for (double tear : tears) {
      MemEnv env;
      env.SetSeed(seed);
      env.SetTearProbability(tear);
      const std::string log = std::string(kDBName) + "/000001.log";
      {
        WALWriter w(&env, log);
        ASSERT_TRUE(w.Open(false).ok()) << "seed=" << seed << " tear=" << tear;
        for (int i = 1; i <= kSynced; ++i) {
          ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
        }
        ASSERT_TRUE(w.Sync().ok());
        for (int i = kSynced + 1; i <= kSynced + kUnsynced; ++i) {
          ASSERT_TRUE(w.Append(Slice(Batch(i, Key(i), Val(i)))).ok());
        }
      }
      env.SimulateCrash();

      Options options;
      options.env = &env;
      DB* db = nullptr;
      const Status s = DB::Open(options, kDBName, &db);
      ASSERT_TRUE(s.ok()) << "崩溃后必须能启动：seed=" << seed << " tear=" << tear << " "
                         << s.ToString();
      const int prefix = CheckedPrefix(db, kSynced + kUnsynced);
      EXPECT_NE(prefix, -1) << "出现半条值 / 值错乱：seed=" << seed << " tear=" << tear;
      EXPECT_GE(prefix, kSynced) << "已 fsync 的前缀丢失：seed=" << seed << " tear=" << tear;
      EXPECT_LE(prefix, kSynced + kUnsynced);
      if (prefix >= 0 && prefix < kSynced + kUnsynced) {
        ++lost_some;
        lost_total += (kSynced + kUnsynced) - prefix;
      }
      ++combos;
      db->Close();
      delete db;
    }
  }
  EXPECT_EQ(combos, 9);
  EXPECT_GT(lost_some, 0)
      << "9 个 (seed, tear) 组合里一次都没丢过未 fsync 的记录 ⇒ 注入没生效，用例是空绿";
  EXPECT_GT(lost_total, 0) << "聚合丢失计数必须 > 0（与上面的逐组合计数同口径）";
}

}  // namespace lsm
