// tests/recovery_m3_test.cpp —— M3.3 恢复/元数据/WAL 回收确定性用例（docs/m3-design.md §10.1：A35~A54）
//
// 纪律（沿用 m2-prerequisites §7 与 §15 R4）：
//   * 全部走 MemEnv（确定性，零 flaky）；跨线程时序靠 FlushHook 屏障，不靠 sleep 赌调度；
//   * 涉及 SyncDir 的结论只写"调用了 SyncDir 且顺序在 rename 之后、写元数据之前"，
//     **不**宣称掉电安全（MemEnv 不建模 dirent）；
//   * M3-A39/A40/A41 的断言写在**语义层**（"元数据损坏 ⇒ kCorruption"、"孤儿 *.sst.tmp /
//     未注册 *.sst ⇒ 不读入 + 计数 + 清理"、"元数据缺失但目录非空 ⇒ kCorruption；只有 .log
//     ⇒ 兼容打开"），元数据文件只用 VersionSet::MetaFileName() 定位，不把断言绑到 "META" 字面量
//     （M4 换 MANIFEST+CURRENT 时这些用例的语义不变）。
#include "test_harness.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db_impl.h"
#include "filename.h"
#include "memenv.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "util/coding.h"
#include "version_edit.h"
#include "version_set.h"
#include "wal.h"

namespace lsm {
namespace {

using test::MemEnv;

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

WriteOptions Synced() {
  WriteOptions wo;
  wo.sync = true;
  return wo;
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

void WriteRawRecord(Env* env, const std::string& path, const std::string& payload, bool append) {
  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(append).ok());
  ASSERT_TRUE(w.Append(Slice(payload)).ok());
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());
}

void WaitFlushIdle(PersistentDBImpl* impl, uint64_t min_completed) {
  for (int spin = 0; spin < 2000000; ++spin) {
    const FlushStats fs = impl->GetFlushStats();
    if (fs.flushes_failed == 0 && fs.flushes_completed >= min_completed &&
        impl->immutables_size() == 0) {
      return;
    }
    std::this_thread::yield();
  }
  ADD_FAILURE() << "等待 flush idle 超时";
}

void PutRange(DB* db, int from, int to) {
  for (int i = from; i <= to; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
}

uint64_t CountLogFiles(Env* env, const std::string& dir) {
  std::vector<std::string> children;
  if (!env->GetChildren(dir, &children).ok()) return 0;
  uint64_t n = 0;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (ParseLogFileName(c, &number)) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// 事件日志 Env（M3-A47）：记录 rename / syncdir / remove，供"删除晚于元数据 durable"的顺序断言
// ---------------------------------------------------------------------------
class EventEnv : public MemEnv {
 public:
  void Record(const std::string& e) {
    std::lock_guard<std::mutex> l(mu_);
    events_.push_back(e);
  }
  std::vector<std::string> Snapshot() const {
    std::lock_guard<std::mutex> l(mu_);
    return events_;
  }

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(fname, &inner);
    if (s.ok()) *result = new LogFile(inner, this, fname);
    return s;
  }
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(fname, &inner);
    if (s.ok()) *result = new LogFile(inner, this, fname);
    return s;
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    Record("rename:" + src + "->" + target);
    return MemEnv::RenameFile(src, target);
  }
  Status SyncDir(const std::string& dirname) override {
    Record("syncdir:" + dirname);
    return MemEnv::SyncDir(dirname);
  }
  Status RemoveFile(const std::string& fname) override {
    Record("remove:" + fname);
    return MemEnv::RemoveFile(fname);
  }

 private:
  class LogFile : public WritableFile {
   public:
    LogFile(WritableFile* inner, EventEnv* env, std::string name)
        : inner_(inner), env_(env), name_(std::move(name)) {}
    ~LogFile() override { delete inner_; }
    Status Append(const Slice& data) override { return inner_->Append(data); }
    Status Flush() override { return inner_->Flush(); }
    Status Sync() override {
      env_->Record("sync:" + name_);
      return inner_->Sync();
    }
    Status Close() override { return inner_->Close(); }

   private:
    WritableFile* inner_;
    EventEnv* env_;
    std::string name_;
  };

  mutable std::mutex mu_;
  std::vector<std::string> events_;
};

// ---------------------------------------------------------------------------
// 钩子：在指定次序的注册点阻塞后台线程（M3-A45/A51 的确定性屏障）
// ---------------------------------------------------------------------------
class GatedRegisterHook : public FlushHook {
 public:
  void OnBeforeRegister() override {
    const int n = ++calls_;
    if (block_at_.load(std::memory_order_acquire) == n) {
      entered_.store(true, std::memory_order_release);
      while (!release_.load(std::memory_order_acquire)) std::this_thread::yield();
    }
  }
  void SetBlockAt(int n) { block_at_.store(n, std::memory_order_release); }
  bool WaitEntered() {
    for (int i = 0; i < 2000000; ++i) {
      if (entered_.load(std::memory_order_acquire)) return true;
      std::this_thread::yield();
    }
    return false;
  }
  void Release() { release_.store(true, std::memory_order_release); }
  int calls() const { return calls_.load(); }

 private:
  std::atomic<int> calls_{0};
  std::atomic<int> block_at_{-1};
  std::atomic<bool> entered_{false};
  std::atomic<bool> release_{false};
};

class BlockOnWrittenHook : public FlushHook {
 public:
  void Arm() { armed_.store(true, std::memory_order_release); }   // 之前完全放行（先造出一次注册）
  void OnSSTableWritten() override {
    if (!armed_.load(std::memory_order_acquire)) return;
    entered_.store(true, std::memory_order_release);
    while (!release_.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  bool WaitEntered() {
    for (int i = 0; i < 2000000; ++i) {
      if (entered_.load(std::memory_order_acquire)) return true;
      std::this_thread::yield();
    }
    return false;
  }
  void Release() { release_.store(true, std::memory_order_release); }

 private:
  std::atomic<bool> armed_{false};
  std::atomic<bool> entered_{false};
  std::atomic<bool> release_{false};
};

// ---------------------------------------------------------------------------
// 失败注入 Env
// ---------------------------------------------------------------------------
class FailRemoveEnv : public MemEnv {
 public:
  bool fail_sst_removal = false;
  int failed = 0;
  Status RemoveFile(const std::string& fname) override {
    if (fail_sst_removal && fname.find(".sst") != std::string::npos) {
      ++failed;
      return Status::IOError("FailRemoveEnv: injected RemoveFile failure", fname);
    }
    return MemEnv::RemoveFile(fname);
  }
};

class OneShotSyncDirFailEnv : public MemEnv {
 public:
  std::atomic<bool> armed{false};
  std::atomic<int> failures{0};
  Status SyncDir(const std::string& dirname) override {
    if (armed.exchange(false)) {
      ++failures;
      return Status::IOError("OneShotSyncDirFailEnv: injected SyncDir failure", dirname);
    }
    return MemEnv::SyncDir(dirname);
  }
};

class NamedBytewiseComparator : public Comparator {
 public:
  explicit NamedBytewiseComparator(std::string name) : name_(std::move(name)) {}
  int Compare(const Slice& a, const Slice& b) const override { return a.compare(b); }
  const char* Name() const override { return name_.c_str(); }

 private:
  std::string name_;
};

// 用独立 MemEnv 造一个只含 "orphan-only" 的合法 SSTable，返回其字节（M3-A40 ②）。
std::string MakeOrphanSstableBytes() {
  MemEnv src;
  Options opts;
  opts.env = &src;
  WritableFile* raw = nullptr;
  EXPECT_TRUE(src.NewWritableFile("/orphan.sst", &raw).ok());
  if (raw == nullptr) return std::string();
  {
    std::unique_ptr<WritableFile> file(raw);
    TableBuilder builder(opts, file.get());
    EXPECT_TRUE(builder.Add(Slice(BuildInternalKey("orphan-only", 5, kTypeValue)), Slice("x")).ok());
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(file->Close().ok());
  }
  return src.Contents("/orphan.sst");
}

}  // namespace

// ===================== M3-A35 [落盘重启] =====================
TEST(Recover, SSTableOnly) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = true;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 400);
  ASSERT_TRUE(impl->ForceFlushForTest().ok()) << "把最后一个 memtable 也落盘（A35 的契约）";
  WaitFlushIdle(impl, 1);
  const FlushStats fs = impl->GetFlushStats();
  EXPECT_GE(fs.flushes_completed, 1u);
  ASSERT_TRUE(db->Close().ok());
  delete db;

  // 关库时：老 log 已被回收；重开只靠 SSTable。
  const uint64_t logs_before = CountLogFiles(&env, "/db");
  EXPECT_LE(logs_before, 2u) << "关库后非空老 log 应已回收（最多剩当前 log）";

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats st = impl2->GetRecoveryStats();
  EXPECT_TRUE(st.meta_present);
  EXPECT_GE(st.sst_files_registered, 1u);
  EXPECT_EQ(0u, st.records_replayed) << "重开不得依赖 WAL（老 log 已被回收）";
  for (int i = 1; i <= 400; ++i) {
    std::string v;
    ASSERT_TRUE(db2->Get(Key(i), &v).ok()) << "key " << i;
    EXPECT_EQ(Val(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===================== M3-A36 [落盘重启] =====================
TEST(Recover, SSTablePlusWalTail) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 400);
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  // flush 之后再写一批（不 flush）：K1 覆盖 SSTable 里的旧值，K2 只在 WAL 里。
  ASSERT_TRUE(db->Put(Synced(), Key(1), "newer-1").ok());
  ASSERT_TRUE(db->Put(Synced(), Key(100000), "tail-only").ok());
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats st = impl2->GetRecoveryStats();
  EXPECT_TRUE(st.meta_present);
  EXPECT_GE(st.sst_files_registered, 1u);
  EXPECT_GT(st.records_replayed, 0u) << "WAL 尾巴中比 SSTable 更新的部分必须被重放";
  std::string v;
  ASSERT_TRUE(db2->Get(Key(1), &v).ok());
  EXPECT_EQ("newer-1", v) << "新值必须覆盖 SSTable 里的旧值";
  ASSERT_TRUE(db2->Get(Key(100000), &v).ok());
  EXPECT_EQ("tail-only", v);
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===================== M3-A37（I31） =====================
TEST(Recover, LastSequenceUniqueSource) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  const int kN = 400;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, kN);
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  ASSERT_TRUE(db->Put(Synced(), Key(200000), "tail").ok());   // 只进 WAL 的第 kN+1 条
  ASSERT_TRUE(db->Close().ok());
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats st = impl2->GetRecoveryStats();
  EXPECT_EQ(static_cast<SequenceNumber>(kN + 1), st.last_sequence)
      << "last_sequence 必须等于 max(WAL 重放, 文件 max_sequence)";
  EXPECT_EQ(st.last_sequence, impl2->last_sequence());
  EXPECT_GT(st.last_sequence, st.max_sequence_in_files) << "WAL 尾巴比已注册文件的 sequence 更新";
  // 下一次写入分配的 sequence 必须严格更大（I13 的 M2 措辞不变）。
  ASSERT_TRUE(db2->Put(Synced(), "after", "x").ok());
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
  DB* db3 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db3).ok());
  EXPECT_EQ(static_cast<SequenceNumber>(kN + 2),
            static_cast<PersistentDBImpl*>(db3)->GetRecoveryStats().last_sequence);
  ASSERT_TRUE(db3->Close().ok());
  delete db3;
}

// ===================== M3-A38（I31） =====================
TEST(Recover, MetaMaxSequenceVerifiedAgainstFullScan) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  const int kN = 400;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, kN);
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  ASSERT_TRUE(db->Close().ok());
  delete db;

  const std::string meta_path = VersionSet::MetaFileName("/db");
  std::string contents = env.Contents(meta_path);
  ASSERT_FALSE(contents.empty());
  VersionEdit edit;
  std::string why;
  ASSERT_TRUE(edit.DecodeFrom(Slice(contents), &why)) << why;
  ASSERT_GE(edit.files().size(), 1u);
  // 全量扫描：打开每个注册文件算真实 max_sequence，与元数据比对（正常必须相等）。
  SequenceNumber scanned = 0;
  for (const FileMetaData& f : edit.files()) {
    std::shared_ptr<Table> t;
    ASSERT_TRUE(Table::Open(options, &env, TableFileName("/db", f.number), &t, &f.smallest,
                            &f.largest)
                    .ok());
    std::unique_ptr<Iterator> it = t->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      Slice uk;
      SequenceNumber seq = 0;
      ValueType type = kTypeValue;
      ASSERT_TRUE(ParseInternalKey(it->key(), &uk, &seq, &type));
      if (seq > scanned) scanned = seq;
    }
    EXPECT_TRUE(it->status().ok());
  }
  EXPECT_EQ(scanned, static_cast<SequenceNumber>(kN));
  // 手工拼字节参照（§12.5 第 7 条：不能只用同一个解码器自证）。
  const std::string manual = test::ManualInternalKey(Slice("manual-ref"), 42, kTypeValue);
  EXPECT_EQ(42u, DecodeTrailerLE(manual.data() + manual.size() - kInternalKeyTrailerSize) >> 8);
  for (const FileMetaData& f : edit.files()) {
    EXPECT_LE(f.max_sequence, static_cast<SequenceNumber>(kN));
    EXPECT_GE(f.max_sequence, 1u);
  }
  // 篡改第一条文件的 max_sequence ⇒ Open 必须 kCorruption（"统计写错"不得静默）。
  VersionEdit tampered;
  tampered.SetComparatorName(edit.comparator_name());
  tampered.SetLogNumber(edit.log_number());
  tampered.SetMinLogNumberToKeep(edit.min_log_number_to_keep());
  tampered.SetNextFileNumber(edit.next_file_number());
  bool first = true;
  for (const FileMetaData& f : edit.files()) {
    FileMetaData f2 = f;
    if (first) {
      f2.max_sequence += 1000000;
      first = false;
    }
    tampered.AddFile(f2);
  }
  std::string tampered_bytes;
  ASSERT_TRUE(tampered.EncodeTo(&tampered_bytes));
  env.SetContents(meta_path, tampered_bytes);
  DB* db2 = nullptr;
  const Status s = DB::Open(options, "/db", &db2);
  EXPECT_TRUE(s.IsCorruption()) << "max_sequence 与全量扫描不符必须拒绝启动：" << s.ToString();
  if (db2 != nullptr) {
    db2->Close();
    delete db2;
  }
}

// ===================== M3-A39（I27/§1.2 边界 4）：语义层 =====================
TEST(Recover, MetaCorruptRefused) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 400);
  WaitFlushIdle(impl, 1);
  ASSERT_TRUE(db->Close().ok());
  delete db;

  const std::string meta_path = VersionSet::MetaFileName("/db");
  const std::string good = env.Contents(meta_path);
  ASSERT_FALSE(good.empty());
  const auto expect_refused_without_repair = [&](const std::string& mutated, const char* what) {
    env.SetContents(meta_path, mutated);
    DB* reopened = nullptr;
    const Status s = DB::Open(options, "/db", &reopened);
    EXPECT_TRUE(s.IsCorruption()) << what << "：元数据损坏必须 kCorruption：" << s.ToString();
    if (reopened != nullptr) {
      reopened->Close();
      delete reopened;
    }
    EXPECT_EQ(mutated, env.Contents(meta_path)) << what << "：不得自动修复/重建元数据";
  };
  // ① magic 坏
  {
    std::string m = good;
    m[0] = 'X';
    expect_refused_without_repair(m, "magic");
  }
  // ② format_version 坏
  {
    std::string m = good;
    m[5] = static_cast<char>(7);
    expect_refused_without_repair(m, "format_version");
  }
  // ③ tail CRC 坏
  {
    std::string m = good;
    m[m.size() - 1] = static_cast<char>(m[m.size() - 1] ^ 0x5a);
    expect_refused_without_repair(m, "tail_crc");
  }
  // ④ 字段长度截断
  {
    std::string m = good.substr(0, good.size() - 1);
    expect_refused_without_repair(m, "truncated");
  }
  // 收尾：恢复原文件后必须能正常打开（证明失败只是"坏文件被拒"，不是把库搞坏）。
  env.SetContents(meta_path, good);
  DB* ok_db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &ok_db).ok());
  ASSERT_TRUE(ok_db->Close().ok());
  delete ok_db;
}

// ===================== M3-A40（I27）：孤儿处理，语义层 =====================
TEST(Recover, OrphanHandledPerInvariant27) {
  // ① 只有 *.sst.tmp ⇒ 不注册 + 被删 + 计数
  {
    MemEnv env;
    env.CreateDir("/db");
    env.SetContents("/db/000007.sst.tmp", "not-a-table");
    Options options;
    options.env = &env;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db)->GetRecoveryStats();
    EXPECT_EQ(1u, st.orphan_tmp_removed);
    EXPECT_FALSE(env.FileExists("/db/000007.sst.tmp"));
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
  // ② 未注册 .sst ⇒ 不读入 + 被删 + 计数
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    PutRange(db, 1, 400);
    WaitFlushIdle(impl, 1);
    ASSERT_TRUE(db->Close().ok());
    delete db;
    const std::string orphan = MakeOrphanSstableBytes();
    ASSERT_FALSE(orphan.empty());
    env.SetContents("/db/000099.sst", orphan);

    DB* db2 = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db2)->GetRecoveryStats();
    EXPECT_EQ(1u, st.orphan_sst_removed);
    EXPECT_FALSE(env.FileExists("/db/000099.sst"));
    std::string v;
    EXPECT_TRUE(db2->Get("orphan-only", &v).IsNotFound()) << "未注册的 .sst 绝不能被读入";
    ASSERT_TRUE(db2->Get(Key(1), &v).ok());
    EXPECT_EQ(Val(1), v) << "已注册数据必须仍可读";
    ASSERT_TRUE(db2->Close().ok());
    delete db2;
  }
  // ③ 清理失败（注入）⇒ Open 仍成功、计数、且该文件仍不被读入
  {
    FailRemoveEnv env;
    env.CreateDir("/db");
    env.SetContents("/db/000007.sst.tmp", "not-a-table");
    env.fail_sst_removal = true;
    Options options;
    options.env = &env;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok()) << "清理失败不得阻断 Open（读路径只走版本）";
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db)->GetRecoveryStats();
    EXPECT_EQ(0u, st.orphan_tmp_removed);
    EXPECT_GE(st.orphan_remove_failed, 1u);
    EXPECT_TRUE(env.FileExists("/db/000007.sst.tmp"));
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
}

// ===================== M3-A41（§10.9 安全阀）：语义层 =====================
TEST(Recover, MetaMissingWithSstRefused) {
  // ① 元数据缺失但目录里有 *.sst ⇒ kCorruption
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    PutRange(db, 1, 400);
    WaitFlushIdle(impl, 1);
    ASSERT_TRUE(db->Close().ok());
    delete db;
    ASSERT_TRUE(env.RemoveFile(VersionSet::MetaFileName("/db")).ok());
    DB* db2 = nullptr;
    const Status s = DB::Open(options, "/db", &db2);
    EXPECT_TRUE(s.IsCorruption()) << "元数据缺失但目录非空必须显式拒绝：" << s.ToString();
    EXPECT_NE(std::string::npos, s.ToString().find("sst")) << s.ToString();
    if (db2 != nullptr) {
      db2->Close();
      delete db2;
    }
  }
  // ② 元数据缺失且只有 .log ⇒ 兼容打开（M2 老库路径）
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 4 * 1024 * 1024;   // 大 buffer：不触发 flush，只有 WAL
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PutRange(db, 1, 50);
    ASSERT_TRUE(db->Sync().ok());
    ASSERT_TRUE(db->Close().ok());
    delete db;
    // 元数据本来就不存在；显式删除一次（语义：元数据缺失）。
    env.RemoveFile(VersionSet::MetaFileName("/db"));
    DB* db2 = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db2).ok()) << "只有 .log 的老库必须能打开";
    PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
    const RecoveryStats st = impl2->GetRecoveryStats();
    EXPECT_FALSE(st.meta_present);
    EXPECT_GT(st.records_replayed, 0u);
    for (int i = 1; i <= 50; ++i) {
      std::string v;
      ASSERT_TRUE(db2->Get(Key(i), &v).ok()) << "key " << i;
      EXPECT_EQ(Val(i), v);
    }
    ASSERT_TRUE(db2->Close().ok());
    delete db2;
  }
}

// ===================== M3-A42（D13 残留） =====================
TEST(Recover, ComparatorNameMismatchRefused) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 400);
  WaitFlushIdle(impl, 1);
  ASSERT_TRUE(db->Close().ok());
  delete db;

  NamedBytewiseComparator other("test.OtherComparator");
  Options options2 = options;
  options2.comparator = &other;
  DB* db2 = nullptr;
  const Status s = DB::Open(options2, "/db", &db2);
  EXPECT_TRUE(s.IsInvalidArgument()) << "comparator 不符必须 kInvalidArgument：" << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("test.OtherComparator")) << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("leveldb.BytewiseComparator")) << s.ToString();
  if (db2 != nullptr) {
    db2->Close();
    delete db2;
  }
}

// ===================== M3-A43 =====================
TEST(Recover, LogOrderIsNumericAscending) {
  MemEnv env;
  env.CreateDir("/db");
  // 字符串序会是 1 → 10 → 2；数值序必须是 1 → 2 → 10。sequence 也按数值序递增，
  // 否则"seq 非递增则跳过"会把后面的 log 全部跳过（那是 D7 的幂等规则，不是本用例要测的）。
  WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "same", "from01"), false);
  WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(2, kTypeValue, "only01", "x"), true);
  WriteRawRecord(&env, LogFileName("/db", 2), EncodeBatch(3, kTypeValue, "same", "from02"), false);
  WriteRawRecord(&env, LogFileName("/db", 10), EncodeBatch(4, kTypeValue, "same", "from10"), false);
  WriteRawRecord(&env, LogFileName("/db", 10), EncodeBatch(5, kTypeValue, "only10", "y"), true);
  Options options;
  options.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  const RecoveryStats st = static_cast<PersistentDBImpl*>(db)->GetRecoveryStats();
  EXPECT_EQ(3u, st.log_files);
  EXPECT_EQ(5u, st.records_replayed);
  std::string v;
  ASSERT_TRUE(db->Get("same", &v).ok());
  EXPECT_EQ("from10", v) << "同 key 的最终值必须来自编号最大的 log";
  ASSERT_TRUE(db->Get("only01", &v).ok());
  EXPECT_EQ("x", v);
  ASSERT_TRUE(db->Get("only10", &v).ok());
  EXPECT_EQ("y", v);
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===================== M3-A44 =====================
TEST(Recover, TruncationRulesUnchangedFromM2) {
  // 非最高编号 log 的尾部残骸 ⇒ kCorruption
  {
    MemEnv env;
    env.CreateDir("/db");
    WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
    WriteRawRecord(&env, LogFileName("/db", 2), EncodeBatch(2, kTypeValue, "b", "B"), false);
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewAppendableFile(LogFileName("/db", 1), &raw).ok());
    {
      std::unique_ptr<WritableFile> f(raw);
      ASSERT_TRUE(f->Append(Slice("abc")).ok());
      ASSERT_TRUE(f->Close().ok());
    }
    Options options;
    options.env = &env;
    DB* db = nullptr;
    const Status s = DB::Open(options, "/db", &db);
    EXPECT_TRUE(s.IsCorruption()) << "非最高编号 log 的残骸必须拒绝：" << s.ToString();
    if (db != nullptr) {
      db->Close();
      delete db;
    }
  }
  // 最高编号 log 的尾部残骸 ⇒ 截断 + 计数 + 可读
  {
    MemEnv env;
    env.CreateDir("/db");
    WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
    WriteRawRecord(&env, LogFileName("/db", 2), EncodeBatch(2, kTypeValue, "b", "B"), false);
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewAppendableFile(LogFileName("/db", 2), &raw).ok());
    {
      std::unique_ptr<WritableFile> f(raw);
      ASSERT_TRUE(f->Append(Slice("abc")).ok());
      ASSERT_TRUE(f->Close().ok());
    }
    Options options;
    options.env = &env;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db)->GetRecoveryStats();
    EXPECT_GT(st.tail_truncated_bytes, 0u);
    EXPECT_FALSE(st.truncation_note.empty());
    std::string v;
    ASSERT_TRUE(db->Get("a", &v).ok());
    EXPECT_EQ("A", v);
    ASSERT_TRUE(db->Get("b", &v).ok());
    EXPECT_EQ("B", v);
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
}

// ===================== M3-A45（I34） =====================
TEST(WalReclaim, WatermarkCriterion) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = true;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  EXPECT_EQ(1u, impl->min_log_number_to_keep());

  uint64_t last_min = 1;
  int written = 0;
  for (int i = 1; i <= 8000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
    written = i;
    const uint64_t m = impl->min_log_number_to_keep();
    EXPECT_GE(m, last_min) << "min_log_to_keep 必须单调不减";
    last_min = m;
    if (impl->GetFlushStats().flushes_completed >= 4) break;
  }
  // 把剩余 memtable 也落盘，消除"后台 flush 是否恰好追上来"的时序依赖（ASan 下曾偶发）。
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  WaitFlushIdle(impl, impl->GetFlushStats().flushes_completed);
  const FlushStats fs = impl->GetFlushStats();
  EXPECT_GE(fs.rotations, 1u) << "多次冻结必须发生 WAL 轮转";
  const uint64_t m = impl->min_log_number_to_keep();
  EXPECT_GE(m, 2u) << "注册完成后可回收水位必须前移";
  EXPECT_LE(m, impl->memtable_log_number()) << "pending 恒含 memtable_（§6.6.2）";
  // 可删集合 = {n < m}，且必须真的被删掉（I34 的安全性方向）。
  // 后台线程的 RecycleObsoleteLogs 在 pop imm 之后才执行，可能与本次扫描并发；
  // 所以这里轮询到"没有 log < m"或超时，避免把"回收正在飞"误判成"回收没生效"。
  bool all_recycled = false;
  uint64_t m_scan = impl->min_log_number_to_keep();
  for (int spin = 0; spin < 2000000 && !all_recycled; ++spin) {
    std::vector<std::string> children;
    ASSERT_TRUE(env.GetChildren("/db", &children).ok());
    all_recycled = true;
    for (const std::string& c : children) {
      uint64_t n = 0;
      if (!ParseLogFileName(c, &n)) continue;
      if (n < m_scan) {
        all_recycled = false;
        break;
      }
    }
    m_scan = impl->min_log_number_to_keep();   // 水位可能仍在单调前移
    if (!all_recycled) std::this_thread::yield();
  }
  EXPECT_TRUE(all_recycled) << "编号 < min_log_to_keep 的 log 必须已被回收";
  for (int i = 1; i <= written; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get(Key(i), &v).ok()) << "key " << i;
  }
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===================== M3-A46（I34 必要性形态） =====================
TEST(WalReclaim, RecoveredMemtableKeepsOldestLog) {
  MemEnv env;
  env.CreateDir("/db");
  WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
  WriteRawRecord(&env, LogFileName("/db", 2), EncodeBatch(2, kTypeValue, "b", "B"), false);
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = true;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  // §8.3 步骤 ⑩：恢复出的 memtable 必须记住**被重放 log 的最小编号**。
  EXPECT_EQ(1u, impl->memtable_log_number()) << "漏掉步骤 ⑩ 会直接丢数据（I34 必要项）";
  EXPECT_EQ(1u, impl->min_log_number_to_keep());
  EXPECT_TRUE(env.FileExists(LogFileName("/db", 1)));
  EXPECT_TRUE(env.FileExists(LogFileName("/db", 2)));
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ("A", v);
  ASSERT_TRUE(db->Get("b", &v).ok());
  EXPECT_EQ("B", v);

  // 立刻 flush + 注册：恢复数据必须已进 SSTable，之后才允许回收老 log；无论如何不得丢数据。
  ASSERT_TRUE(impl->ForceFlushForTest().ok()) << "把恢复出的 memtable 落盘并注册";
  ASSERT_TRUE(db->Close().ok());
  delete db;
  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  ASSERT_TRUE(db2->Get("a", &v).ok()) << "老 log 被回收后数据必须仍在（已注册进 SSTable）";
  EXPECT_EQ("A", v);
  ASSERT_TRUE(db2->Get("b", &v).ok());
  EXPECT_EQ("B", v);
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===================== M3-A47（I34） =====================
TEST(WalReclaim, DeleteOnlyAfterMetaDurable) {
  EventEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = true;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 2000);
  WaitFlushIdle(impl, 2);
  ASSERT_TRUE(db->Close().ok());
  delete db;

  const std::vector<std::string> events = env.Snapshot();
  // 找到第一条 *.log 的删除事件
  size_t remove_idx = events.size();
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].compare(0, 7, "remove:") == 0 &&
        events[i].find(".log") != std::string::npos) {
      remove_idx = i;
      break;
    }
  }
  ASSERT_LT(remove_idx, events.size()) << "本用例必须真的发生 WAL 回收（否则空绿）";
  // 该删除之前必须有元数据的 rename(META.tmp->META)，且 rename 之后、删除之前有 SyncDir。
  size_t meta_rename = events.size();
  for (size_t i = 0; i < remove_idx; ++i) {
    if (events[i].find("META.tmp") != std::string::npos &&
        events[i].compare(0, 7, "rename:") == 0) {
      meta_rename = i;
    }
  }
  ASSERT_LT(meta_rename, remove_idx) << "log 删除必须晚于元数据 rename（I34）";
  bool syncdir_between = false;
  for (size_t i = meta_rename + 1; i < remove_idx; ++i) {
    if (events[i].compare(0, 8, "syncdir:") == 0) syncdir_between = true;
  }
  EXPECT_TRUE(syncdir_between) << "log 删除必须晚于元数据的 SyncDir（I34/L17）";
}

// ===================== M3-A48 =====================
TEST(WalReclaim, DisabledByOption) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = false;   // 对照：一个 log 都不删
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutRange(db, 1, 2000);
  WaitFlushIdle(impl, 2);
  EXPECT_EQ(0u, impl->GetFlushStats().log_files_deleted) << "关闭回收时运行期也不得删 log";
  ASSERT_TRUE(db->Close().ok());
  delete db;
  EXPECT_GE(CountLogFiles(&env, "/db"), 2u) << "关闭回收时老 log 必须仍在";

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  const RecoveryStats st = static_cast<PersistentDBImpl*>(db2)->GetRecoveryStats();
  EXPECT_EQ(0u, st.obsolete_logs_removed) << "关闭回收时重开也不得删 log";
  for (int i = 1; i <= 2000; ++i) {
    std::string v;
    ASSERT_TRUE(db2->Get(Key(i), &v).ok()) << "key " << i;
    EXPECT_EQ(Val(i), v);
  }
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===================== M3-A49（I33） =====================
TEST(Rotation, BeforeSequenceAssignAndAtomicOnFailure) {
  OneShotSyncDirFailEnv env;
  env.CreateDir("/db");
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);

  env.armed.store(true);
  int failed_key = -1;
  SequenceNumber seq_before = 0;
  uint64_t log_before = 0;
  for (int i = 1; i <= 20000; ++i) {
    seq_before = impl->last_sequence();
    log_before = impl->log_number();
    const Status s = db->Put(WriteOptions(), Key(i), Val(i));
    if (!s.ok()) {
      EXPECT_TRUE(s.IsIOError()) << s.ToString();
      failed_key = i;
      break;
    }
  }
  ASSERT_GE(failed_key, 1) << "注入的轮转 SyncDir 失败必须让某一批被拒";
  EXPECT_EQ(0, env.failures.load() - 1) << "一次性注入应恰好消费一次";
  EXPECT_EQ(1u, impl->GetFlushStats().rotate_failed);
  EXPECT_EQ(seq_before, impl->last_sequence()) << "I33：轮转失败不得推进 sequence";
  EXPECT_EQ(log_before, impl->log_number()) << "I33：轮转失败不得换 log_";
  std::string v;
  EXPECT_TRUE(db->Get(Key(failed_key), &v).IsNotFound()) << "I33：被拒的批不得 Add 进 memtable";
  // 重试同一 key：轮转重试成功后必须成功，且 sequence 只前进一格。
  ASSERT_TRUE(db->Put(WriteOptions(), Key(failed_key), Val(failed_key)).ok());
  EXPECT_EQ(seq_before + 1, impl->last_sequence());
  ASSERT_TRUE(db->Get(Key(failed_key), &v).ok());
  EXPECT_EQ(Val(failed_key), v);
  EXPECT_GE(impl->GetFlushStats().rotations, 1u) << "重试后必须完成轮转";
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===================== M3-A50（I32 per-log） =====================
TEST(Sync, PerLogBoundaryAfterRotation) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 20000 && impl->GetFlushStats().rotations < 1; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
  ASSERT_GE(impl->GetFlushStats().rotations, 1u) << "必须发生轮转（否则本用例空测）";
  const SequenceNumber durable_before = impl->durable_seq();
  const uint64_t log_before = impl->log_number();
  ASSERT_TRUE(db->Sync().ok());
  EXPECT_LE(impl->durable_seq(), impl->log_last_appended_seq())
      << "I32/R1：不得发布到当前 log 未追加的部分";
  EXPECT_GE(impl->durable_seq(), durable_before) << "水位必须单调不减";
  EXPECT_EQ(log_before, impl->log_number());
  ASSERT_TRUE(db->Put(Synced(), Key(999999), Val(999999)).ok());
  EXPECT_EQ(impl->log_last_appended_seq(), impl->durable_seq());
  EXPECT_GE(impl->durable_seq(), durable_before);
  EXPECT_LE(impl->durable_seq(), impl->log_last_appended_seq());
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===================== M3-A51（§6.5） =====================
TEST(Close, AbandonsImmutablesWithCounter) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  BlockOnWrittenHook hook;
  options.flush_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  // 先让第一次 flush 成功（从而写出元数据），第二次 flush 才阻塞 ⇒ Close 时留下一个未注册的
  // immutable；重开时它是孤儿（被清理），数据从 WAL 重放。
  PutRange(db, 1, 400);
  WaitFlushIdle(impl, 1);
  ASSERT_TRUE(env.FileExists(VersionSet::MetaFileName("/db"))) << "必须先有一次成功注册";
  hook.Arm();
  // 写者放独立线程：后台线程被 hook 卡住后，写者会停在 WaitForImmutableCapacity 上，
  // 这正是 A51 要覆盖的"Close 时有未落盘 immutable"形态。
  std::atomic<bool> stop_writer{false};
  std::thread writer([&] {
    for (int i = 1000; i <= 200000 && !stop_writer.load(std::memory_order_acquire); ++i) {
      if (!db->Put(WriteOptions(), Key(i), Val(i)).ok()) break;
    }
  });
  ASSERT_TRUE(hook.WaitEntered()) << "后台 flush 未进入可阻塞的注入点";

  Status close_status;
  std::thread closer([&] { close_status = db->Close(); });
  for (int i = 0; i < 4000000 && !impl->closed_for_test(); ++i) std::this_thread::yield();
  ASSERT_TRUE(impl->closed_for_test()) << "Close 必须置 closed_";
  hook.Release();
  closer.join();
  stop_writer.store(true, std::memory_order_release);
  writer.join();

  EXPECT_TRUE(close_status.ok()) << close_status.ToString();
  const FlushStats fs = impl->GetFlushStats();
  EXPECT_GE(fs.immutables_abandoned, 1u) << "被放弃的 immutable 必须计数（不得静默）";
  EXPECT_GE(CountLogFiles(&env, "/db"), 1u) << "数据仍在 WAL，log 不得被删";
  delete db;

  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
  std::string v;
  ASSERT_TRUE(db2->Get(Key(1000), &v).ok()) << "被放弃的 immutable 数据必须能从 WAL 重放";
  EXPECT_EQ(Val(1000), v);
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ===================== M3-A52（L21） =====================
TEST(Close, DrainsBackgroundThread) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PutRange(db, 1, 400);
  ASSERT_TRUE(db->Close().ok());
  EXPECT_TRUE(db->Close().ok()) << "Close 必须幂等";
  const Status p = db->Put(WriteOptions(), "after-close", "x");
  EXPECT_FALSE(p.ok()) << "Close 后的写必须返回明确 Status";
  delete db;
}

// ===================== M3-A53（M2 教训 4） =====================
TEST(Options, InvalidRejectedWithoutFailStop) {
  {
    Options o;
    o.comparator = nullptr;
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(o, "/db", &db).IsInvalidArgument());
  }
  {
    Options o;
    o.write_buffer_size = 0;
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(o, "/db", &db).IsInvalidArgument());
  }
  for (size_t bs : {static_cast<size_t>(0), static_cast<size_t>(256),
                    static_cast<size_t>(2) * 1024 * 1024}) {
    Options o;
    o.block_size = bs;
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(o, "/db", &db).IsInvalidArgument()) << "block_size=" << bs;
  }
  {
    Options o;
    o.max_open_files = 0;
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(o, "/db", &db).IsInvalidArgument());
  }
  // 越界输入不得让库进入粘性写只读：先触发一次 kInvalidArgument 的 Put，随后普通 Put 必须 kOk。
  MemEnv env;
  Options o;
  o.env = &env;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  std::string huge(kMaxLogicalRecordSize + 1, 'x');
  EXPECT_TRUE(db->Put(WriteOptions(), "big", huge).IsInvalidArgument());
  ASSERT_TRUE(db->Put(WriteOptions(), "normal", "ok").ok()) << "输入校验不得触发 fail-stop";
  std::string v;
  ASSERT_TRUE(db->Get("normal", &v).ok());
  EXPECT_EQ("ok", v);
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ===================== M3-A54（"不得静默"纪律） =====================
TEST(Flush, StatsCountedNotSilent) {
  // ① 尾部截断 + ② 被跳过的 record：同一份 WAL 里制造两种情形。
  {
    MemEnv env;
    env.CreateDir("/db");
    WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "a", "A"), false);
    // 非递增 sequence 的 record（会被跳过并计数）
    WriteRawRecord(&env, LogFileName("/db", 1), EncodeBatch(1, kTypeValue, "stale", "x"), true);
    // 撕裂尾
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env.NewAppendableFile(LogFileName("/db", 1), &raw).ok());
    {
      std::unique_ptr<WritableFile> f(raw);
      ASSERT_TRUE(f->Append(Slice("abc")).ok());
      ASSERT_TRUE(f->Close().ok());
    }
    Options options;
    options.env = &env;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db)->GetRecoveryStats();
    EXPECT_GT(st.tail_truncated_bytes, 0u) << "截断必须计数";
    EXPECT_FALSE(st.truncation_note.empty());
    EXPECT_GE(st.records_skipped, 1u) << "跳过必须计数";
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
  // ③ 删 orphan + ④ 删 obsolete log：重开后两个计数都必须非零。
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    options.recycle_log_files = true;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    PutRange(db, 1, 2000);
    WaitFlushIdle(impl, 2);
    ASSERT_TRUE(db->Close().ok());
    delete db;
    // 人为制造：一个 *.sst.tmp 孤儿 + 一个编号低于可回收水位的 *.log。
    env.SetContents("/db/000777.sst.tmp", "junk");
    const uint64_t low = 1;
    if (!env.FileExists(LogFileName("/db", low))) {
      WriteRawRecord(&env, LogFileName("/db", low), EncodeBatch(999999, kTypeValue, "z", "z"),
                     false);
    }
    DB* db2 = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db2).ok());
    const RecoveryStats st = static_cast<PersistentDBImpl*>(db2)->GetRecoveryStats();
    EXPECT_GE(st.orphan_tmp_removed, 1u) << "orphan *.sst.tmp 必须计数";
    EXPECT_GE(st.obsolete_logs_removed, 1u) << "obsolete log 必须计数";
    ASSERT_TRUE(db2->Close().ok());
    delete db2;
  }
  // ⑤ index_size_warn：由 M3-A11 在 TableBuilder 层覆盖（FlushStats 只是逐字段转发
  //    builder.index_size_warn_count()）；此处只断言转发口径存在且初值为 0，避免造 65536 块的天价用例。
  {
    MemEnv env;
    Options options;
    options.env = &env;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    EXPECT_EQ(0u, static_cast<PersistentDBImpl*>(db)->GetFlushStats().index_size_warn);
    ASSERT_TRUE(db->Close().ok());
    delete db;
  }
}

}  // namespace lsm
