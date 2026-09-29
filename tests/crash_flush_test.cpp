// tests/crash_flush_test.cpp —— M3-B02 的确定性（MemEnv）对照：flush 三个注入点失败 + 掉电后恢复
//
// 进程级 kill -9 版本由 scripts/lsm_flush_crash_test.sh 覆盖；本文件用 MemEnv 精确制造
// "写文件后 / rename 前 / 注册前"三个时刻的 **flush 失败**（§6.4 的 fail-stop），等 DB 完全静默
// （Close 返回、无在飞写者/flusher）后再 SimulateCrash() 模拟掉电，然后重开验证：
//   ① 已 ack（sync=true）的写一条不少、值逐字节相等；
//   ② 未注册的 .sst 只会被当孤儿清理，绝不被读入；
//   ③ Open 不返回 Corruption（引用集 ⊆ 存在集），META 与目录内容自洽。
//
// 为什么掉电放在 Close 之后：在进程内并发地 SimulateCrash 会把"仍在飞的 Append/Sync"也回滚，
// 那不是掉电模型（真实掉电时进程已死），会让 sync=true 的已 ack 写被误判为丢失。顺序化之后，
// "sync=true 的写必须存活"才是可判定的命题。
#include "test_harness.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db_impl.h"
#include "memenv.h"
#include "filename.h"
#include "version_set.h"

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
bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

class CrashEnv : public MemEnv {
 public:
  enum class Point { kNone, kAfterWrite, kBeforeRename, kBeforeRegister };
  // 注入开关由后台 flush 线程写、由前台写路径读 ⇒ 必须原子（TSan 回归）。
  std::atomic<int> point{static_cast<int>(Point::kNone)};
  std::atomic<bool> armed{false};
  std::atomic<int> sst_sync_failures{0};
  std::atomic<int> sst_rename_failures{0};
  std::atomic<int> meta_sync_failures{0};

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(fname, &inner);
    if (s.ok()) *result = new CrashFile(inner, this, fname);
    return s;
  }
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(fname, &inner);
    if (s.ok()) *result = new CrashFile(inner, this, fname);
    return s;
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    if (armed.load(std::memory_order_acquire) &&
        point.load(std::memory_order_acquire) == static_cast<int>(Point::kBeforeRename) &&
        EndsWith(target, ".sst")) {
      sst_rename_failures.fetch_add(1, std::memory_order_relaxed);
      return Status::IOError("CrashEnv: injected rename failure", src);
    }
    return MemEnv::RenameFile(src, target);
  }

 private:
  class CrashFile : public WritableFile {
   public:
    CrashFile(WritableFile* inner, CrashEnv* env, std::string name)
        : inner_(inner), env_(env), name_(std::move(name)) {}
    ~CrashFile() override { delete inner_; }
    Status Append(const Slice& data) override { return inner_->Append(data); }
    Status Flush() override { return inner_->Flush(); }
    Status Sync() override {
      const int p = env_->point.load(std::memory_order_acquire);
      if (env_->armed.load(std::memory_order_acquire) &&
          p == static_cast<int>(Point::kAfterWrite) && EndsWith(name_, ".sst.tmp")) {
        env_->sst_sync_failures.fetch_add(1, std::memory_order_relaxed);
        return Status::IOError("CrashEnv: injected SST fsync failure", name_);
      }
      // M4：活动元数据 = CURRENT + MANIFEST；稳态追加写的是 MANIFEST-<n>（无 .tmp），
      // 重建写的是 MANIFEST-<n>.tmp / CURRENT.tmp。三者都算"元数据的 fsync"。
      const bool is_active_meta =
          name_.find("MANIFEST-") != std::string::npos ||
          (name_.size() >= 11 && name_.compare(name_.size() - 11, 11, "CURRENT.tmp") == 0);
      if (env_->armed.load(std::memory_order_acquire) &&
          p == static_cast<int>(Point::kBeforeRegister) && is_active_meta) {
        env_->meta_sync_failures.fetch_add(1, std::memory_order_relaxed);
        return Status::IOError("CrashEnv: injected active-metadata fsync failure", name_);
      }
      return inner_->Sync();
    }
    Status Close() override { return inner_->Close(); }

   private:
    WritableFile* inner_;
    CrashEnv* env_;
    std::string name_;
  };
};

class ArmHook : public FlushHook {
 public:
  ArmHook(CrashEnv* env, CrashEnv::Point point) : env_(env), point_(point) {}
  void Arm() { armed_.store(true, std::memory_order_release); }
  void OnSSTableWritten() override { ArmIf(CrashEnv::Point::kAfterWrite); }
  void OnBeforeRename() override { ArmIf(CrashEnv::Point::kBeforeRename); }
  void OnBeforeRegister() override { ArmIf(CrashEnv::Point::kBeforeRegister); }

 private:
  void ArmIf(CrashEnv::Point p) {
    if (!armed_.load(std::memory_order_acquire)) return;
    if (point_ != p) return;
    env_->point.store(static_cast<int>(p), std::memory_order_release);
    env_->armed.store(true, std::memory_order_release);   // 紧随其后的那一步 IO 失败
  }
  CrashEnv* env_;
  CrashEnv::Point point_;
  std::atomic<bool> armed_{false};
};

struct CrashResult {
  bool reopened = false;
  bool meta_present = false;
  int missing = 0;
  std::string first_missing_key;
  int orphan_sst = 0;
  int orphan_failed = 0;
};

void RunCase(CrashEnv::Point point, const char* what, bool expect_sst_orphan, CrashResult* out) {
  CrashEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  options.recycle_log_files = true;
  ArmHook hook(&env, point);
  options.flush_hook = &hook;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok()) << what;
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 200; ++i) {
    ASSERT_TRUE(db->Put(Synced(), Key(i), Val(i)).ok()) << what;
  }
  // 第一次 flush 成功（META 先存在，重开时不会撞上"元数据缺失但目录非空"的安全阀）。
  for (int spin = 0; spin < 4000000; ++spin) {
    const FlushStats fs = impl->GetFlushStats();
    if (fs.flushes_completed >= 1 && impl->immutables_size() == 0) break;
    std::this_thread::yield();
  }
  ASSERT_TRUE(env.FileExists(CurrentFileName("/db"))) << what << "：活动元数据必须先存在";

  hook.Arm();
  int last_ok = 200;
  for (int i = 201; i <= 600; ++i) {
    const Status s = db->Put(Synced(), Key(i), Val(i));
    if (!s.ok()) break;   // fail-stop 之后的写会失败：正是期望
    last_ok = i;
  }
  for (int spin = 0; spin < 4000000; ++spin) {
    if (impl->GetFlushStats().flushes_failed >= 1) break;
    std::this_thread::yield();
  }
  EXPECT_GE(impl->GetFlushStats().flushes_failed, 1u) << what << "：注入点未触发 fail-stop";
  const Status c = db->Close();
  EXPECT_FALSE(c.ok()) << what << "：失败后 Close 必须返回同一错误";
  delete db;

  env.SimulateCrash();   // 静默之后才模拟掉电（见文件头注释）

  DB* db2 = nullptr;
  if (!DB::Open(options, "/db", &db2).ok()) {
    ADD_FAILURE() << what << "：崩溃后必须能恢复（不得 kCorruption）";
    return;
  }
  out->reopened = true;
  PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats st = impl2->GetRecoveryStats();
  out->meta_present = st.meta_present;
  EXPECT_TRUE(st.meta_present) << what;
  for (int i = 1; i <= last_ok; ++i) {
    std::string v;
    const Status g = db2->Get(Key(i), &v);
    if (!g.ok()) {
      ++out->missing;
      if (out->first_missing_key.empty()) out->first_missing_key = Key(i);
      continue;
    }
    EXPECT_EQ(Val(i), v) << what << "：key " << i << " 静默错值";
  }
  out->orphan_sst = static_cast<int>(st.orphan_sst_removed);
  out->orphan_failed = static_cast<int>(st.orphan_remove_failed);
  if (expect_sst_orphan) {
    EXPECT_GE(st.orphan_sst_removed, 1u) << what << "：未注册 .sst 必须被当孤儿清理并计数";
  }
  EXPECT_EQ(0u, st.orphan_remove_failed) << what;
  ASSERT_TRUE(db2->Close().ok()) << what;
  delete db2;
}

}  // namespace

// B02 注入点 ①：写文件后（.sst.tmp 尚未 fsync）失败 + 掉电
TEST(CrashFlush, AfterSSTableWrittenThenPowerLoss) {
  CrashResult r;
  RunCase(CrashEnv::Point::kAfterWrite, "after-write", /*expect_sst_orphan=*/false, &r);
  EXPECT_TRUE(r.reopened) << "掉电后必须能重新打开";
  EXPECT_TRUE(r.meta_present) << "元数据必须仍存在";
  EXPECT_EQ(0, r.missing) << "已 ack 的写不得丢失；首个缺失：" << r.first_missing_key;
  EXPECT_EQ(0, r.orphan_failed);
}

// B02 注入点 ②：rename 之前失败 + 掉电
TEST(CrashFlush, BeforeRenameThenPowerLoss) {
  CrashResult r;
  RunCase(CrashEnv::Point::kBeforeRename, "before-rename", /*expect_sst_orphan=*/false, &r);
  EXPECT_TRUE(r.reopened) << "掉电后必须能重新打开";
  EXPECT_EQ(0, r.missing) << "已 ack 的写不得丢失；首个缺失：" << r.first_missing_key;
  EXPECT_EQ(0, r.orphan_failed);
}

// B02 注入点 ③：注册之前失败（.sst 已 rename，META 尚未更新）+ 掉电
TEST(CrashFlush, BeforeRegisterThenPowerLoss) {
  CrashResult r;
  RunCase(CrashEnv::Point::kBeforeRegister, "before-register", /*expect_sst_orphan=*/true, &r);
  EXPECT_TRUE(r.reopened) << "掉电后必须能重新打开";
  EXPECT_EQ(0, r.missing) << "已 ack 的写不得丢失；首个缺失：" << r.first_missing_key;
  EXPECT_GE(r.orphan_sst, 1) << "未注册 .sst 必须被清理并计数";
  EXPECT_EQ(0, r.orphan_failed);
}

}  // namespace lsm
