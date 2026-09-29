// tests/crash_test.cpp —— 掉电语义用例（docs/m2-design.md §9.1 的 A27~A30）
//
// 为什么这些用例只能用 MemEnv：M2 #0 实测证明 kill -9 打断不了一次 write()
// （裸 write 逐条 100 轮 TAIL_TORN 0），所以「掉电丢多少」只能靠内存文件系统按
// fsync 水位 + 固定种子撕裂来确定性复现（memenv.h 的崩溃模型）。
//
// 判据（设计 §9.1）：崩溃后 ① 每个出现的 key 的值必须**逐字节等于某次完整写入**（无半写）；
// ② 出现的 key 集合必须是写入序列的**前缀**；③ 已 fsync 过的写一条都不能少。
#include "test_harness.h"

#include <memory>
#include <string>
#include <vector>

#include "db.h"
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

}  // namespace lsm
