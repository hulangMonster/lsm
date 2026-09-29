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

}  // namespace lsm
