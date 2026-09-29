// tests/recovery_test.cpp —— 恢复层确定性用例（docs/m2-design.md §9.1 的 A11~A19）
//
// 与 WAL 层用例同样的纪律：只测公开行为；涉及随机的必须固定种子（本文件的用例全部确定）；
// 任何"通过"都要能指向具体的断言。
#include "test_harness.h"

#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "filename.h"
#include "util/coding.h"
#include "util/env.h"
#include "wal.h"

namespace lsm {
namespace {

using test::TempDir;

WriteOptions Synced() {
  WriteOptions wo;
  wo.sync = true;
  return wo;
}

// 用 WALWriter 往指定编号的 log 里写一条 batch（payload 由调用方给，便于构造畸形输入）
void WriteRawRecord(Env* env, const std::string& path, const std::string& payload, bool append) {
  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(append).ok());
  ASSERT_TRUE(w.Append(Slice(payload)).ok());
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());
}

std::string EncodeBatch(SequenceNumber seq, ValueType type, const std::string& key,
                        const std::string& value) {
  std::string out;
  PutFixed64(&out, seq);
  PutFixed32(&out, 1);
  out.push_back(static_cast<char>(type));
  PutVarint32(&out, static_cast<uint32_t>(key.size()));
  out.append(key);
  if (type == kTypeValue) {
    PutVarint32(&out, static_cast<uint32_t>(value.size()));
    out.append(value);
  }
  return out;
}

std::string ReadWhole(Env* env, const std::string& path) {
  SequentialFile* raw = nullptr;
  EXPECT_TRUE(env->NewSequentialFile(path, &raw).ok());
  if (raw == nullptr) return std::string();
  std::unique_ptr<SequentialFile> f(raw);
  std::string out;
  char scratch[4096];
  while (true) {
    Slice piece;
    if (!f->Read(sizeof(scratch), &piece, scratch).ok() || piece.empty()) break;
    out.append(piece.data(), piece.size());
  }
  return out;
}

}  // namespace

// A11
TEST(Recovery, ReplayPutDeleteAndBatches) {
  TempDir dir("lsm_rec_");
  Options options;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
    ASSERT_TRUE(db->Put(Synced(), "a", "1").ok());
    ASSERT_TRUE(db->Put(Synced(), "b", "2").ok());
    ASSERT_TRUE(db->Delete(Synced(), "a").ok());
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
  std::string v;
  EXPECT_TRUE(db->Get("a", &v).IsNotFound()) << "tombstone 必须随重放生效";
  ASSERT_TRUE(db->Get("b", &v).ok());
  EXPECT_EQ("2", v);
  db->Close();
  delete db;
}

// A12：所有 *.log 都必须被扫描（I12）。注意：D7 的「seq <= last 则跳过」使最终状态对
// 「数值序 vs 字符串序」不敏感（两种顺序都会收敛到同一状态），所以本用例断言的是
// "全部 log 都被重放"与"高编号 log 里的新版本生效"；数值排序本身由构造保证，
// 而它的**可观测后果**在 A18（非最高编号 log 的残骸必须拒绝启动）。
TEST(Recovery, AllLogsAreReplayed) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  // 前提（I13）：sequence 必须与文件编号同序。给文件编号 1/2/10 依次分配 seq 1/2/3 ——
  // 若实现"只扫最高编号 log"，key a（在 000001.log）与 b（在 000002.log）就会丢，本用例即失败。
  WriteRawRecord(env, LogFileName(dir.path(), 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
  WriteRawRecord(env, LogFileName(dir.path(), 2), EncodeBatch(2, kTypeValue, "b", "B"), false);
  WriteRawRecord(env, LogFileName(dir.path(), 10), EncodeBatch(3, kTypeValue, "c", "C"), false);

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(Options(), dir.path(), &db).ok());
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ("A", v);
  ASSERT_TRUE(db->Get("b", &v).ok());
  EXPECT_EQ("B", v);
  ASSERT_TRUE(db->Get("c", &v).ok());
  EXPECT_EQ("C", v) << "编号 10 的 log 必须被重放";
  db->Close();
  delete db;
}

// A13
TEST(Recovery, SequenceRestoredAboveMaxReplayed) {
  TempDir dir("lsm_rec_");
  Options options;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(db->Put(Synced(), "x", std::to_string(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
  std::string v;
  ASSERT_TRUE(db->Get("x", &v).ok());
  EXPECT_EQ("4", v);
  // 恢复后新写的 sequence 必须**严格大于**任何已重放记录（I13），否则新值会被旧版本盖住
  ASSERT_TRUE(db->Put(Synced(), "x", "after-recovery").ok());
  ASSERT_TRUE(db->Get("x", &v).ok());
  EXPECT_EQ("after-recovery", v) << "恢复后的写入必须能覆盖重放出来的最新版本";
  db->Close();
  delete db;
}

// A14
TEST(Recovery, IdempotentAcrossRepeatedOpens) {
  TempDir dir("lsm_rec_");
  Options options;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
    ASSERT_TRUE(db->Put(Synced(), "k", "v").ok());
    db->Close();
    delete db;
  }
  Env* env = Env::Default();
  const std::string log = LogFileName(dir.path(), 1);
  const std::string before = ReadWhole(env, log);

  for (int round = 0; round < 3; ++round) {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok()) << "round=" << round;
    std::string v;
    ASSERT_TRUE(db->Get("k", &v).ok()) << "round=" << round;
    EXPECT_EQ("v", v);
    ASSERT_TRUE(db->Close().ok());
    delete db;
    EXPECT_EQ(before, ReadWhole(env, log)) << "无损坏时重复 Open 不得改动 WAL（I18 幂等），round=" << round;
  }
}

// A15
TEST(Recovery, TornTailDoesNotResurrectOlderValue) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  Options options;
  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok());
    ASSERT_TRUE(db->Put(Synced(), "k", "v1").ok());
    db->Close();
    delete db;
  }
  // 模拟撕裂尾：往 log 末尾追加半条 record（头完整、payload 不足）
  const std::string log = LogFileName(dir.path(), 1);
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env->NewAppendableFile(log, &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    const char half_header[7] = {1, 2, 3, 4, 100, 0, static_cast<char>(kFullType)};
    ASSERT_TRUE(f->Append(Slice(half_header, 7)).ok());
    ASSERT_TRUE(f->Append(Slice("short")).ok());     // 声称 100 字节，只给了 5 字节
    ASSERT_TRUE(f->Close().ok());
  }
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, dir.path(), &db).ok()) << "尾部残骸必须被安全截断后继续启动";
  std::string v;
  ASSERT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ("v1", v) << "不得返回半条 v2、也不得返回 NotFound（I14/I19）";
  // 截断后应能继续正常写
  ASSERT_TRUE(db->Put(Synced(), "k2", "v2").ok());
  db->Close();
  delete db;
}

// A16
TEST(Recovery, EmptyWalAndMissingDir) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  const std::string fresh = dir.File("brand_new");
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(Options(), fresh, &db).ok()) << "目录不存在必须自动创建";
  std::string v;
  EXPECT_TRUE(db->Get("nope", &v).IsNotFound());
  db->Close();
  delete db;

  // 目录里只有非 .log 文件
  const std::string only_junk = dir.File("junk_dir");
  ASSERT_TRUE(env->CreateDir(only_junk).ok());
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env->NewWritableFile(only_junk + "/README", &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    f->Close();
  }
  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(Options(), only_junk, &db2).ok());
  EXPECT_TRUE(db2->Get("nope", &v).IsNotFound());
  db2->Close();
  delete db2;
}

// A17
TEST(Recovery, MalformedBatchPayloadIsCorruption) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  const std::string log = LogFileName(dir.path(), 1);

  // count == 0
  std::string bad;
  PutFixed64(&bad, 1);
  PutFixed32(&bad, 0);
  WriteRawRecord(env, log, bad, false);
  {
    DB* db = nullptr;
    const Status s = DB::Open(Options(), dir.path(), &db);
    EXPECT_TRUE(s.IsCorruption()) << "count == 0 必须判损坏：" << s.ToString();
    EXPECT_EQ(nullptr, db);
  }

  // entry 解完后仍有剩余字节
  std::string bad2 = EncodeBatch(1, kTypeValue, "k", "v");
  bad2.append("trailing");
  WriteRawRecord(env, log, bad2, false);
  {
    DB* db = nullptr;
    const Status s = DB::Open(Options(), dir.path(), &db);
    EXPECT_TRUE(s.IsCorruption()) << "多余字节必须判损坏：" << s.ToString();
  }

  // key 为空
  std::string bad3;
  PutFixed64(&bad3, 1);
  PutFixed32(&bad3, 1);
  bad3.push_back(static_cast<char>(kTypeValue));
  PutVarint32(&bad3, 0);
  WriteRawRecord(env, log, bad3, false);
  {
    DB* db = nullptr;
    const Status s = DB::Open(Options(), dir.path(), &db);
    EXPECT_TRUE(s.IsCorruption()) << "空 key 必须判损坏：" << s.ToString();
  }
}

// A18
TEST(Recovery, NonHighestLogTailCorruptionRejected) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  WriteRawRecord(env, LogFileName(dir.path(), 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
  WriteRawRecord(env, LogFileName(dir.path(), 2), EncodeBatch(2, kTypeValue, "b", "B"), false);
  // 在**非最高编号**的 log 尾部追加残骸 ⇒ 必须拒绝启动（不能把它当残骸截掉，那会静默丢数据）
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env->NewAppendableFile(LogFileName(dir.path(), 1), &raw).ok());
    std::unique_ptr<WritableFile> f(raw);
    const char half[4] = {9, 9, 9, 9};
    ASSERT_TRUE(f->Append(Slice(half, 4)).ok());
    ASSERT_TRUE(f->Close().ok());
  }
  DB* db = nullptr;
  const Status s = DB::Open(Options(), dir.path(), &db);
  EXPECT_TRUE(s.IsCorruption()) << "非最高编号 log 的尾部残骸必须拒绝启动：" << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("000001.log")) << "错误信息必须能定位到文件：" << s.ToString();
}

// A19
TEST(Recovery, SameKeyManyVersionsReplaysLatest) {
  TempDir dir("lsm_rec_");
  Env* env = Env::Default();
  const std::string log = LogFileName(dir.path(), 1);
  WALWriter w(env, log);
  ASSERT_TRUE(w.Open(false).ok());
  const int kVersions = 1000;
  for (int i = 0; i < kVersions; ++i) {
    ASSERT_TRUE(w.Append(Slice(EncodeBatch(static_cast<SequenceNumber>(i + 1), kTypeValue, "same",
                                          "v" + std::to_string(i)))).ok());
  }
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(Options(), dir.path(), &db).ok());
  std::string v;
  ASSERT_TRUE(db->Get("same", &v).ok());
  EXPECT_EQ("v" + std::to_string(kVersions - 1), v) << "必须返回最大 sequence 的版本";
  std::unique_ptr<Iterator> it(db->NewIterator());
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  EXPECT_EQ(1u, n) << "用户视图里同一 user key 只能出现一次";
  db->Close();
  delete db;
}

}  // namespace lsm
