// tests/flush_test.cpp —— M3.2 flush 路径与读路径串联用例（docs/m3-design.md §10.1：A20~A30）
#include "test_harness.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db_impl.h"
#include "memenv.h"
#include "sstable/table.h"
#include "version_set.h"

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

void WaitForFlushIdle(PersistentDBImpl* impl, uint64_t min_completed) {
  for (int spin = 0; spin < 800000; ++spin) {
    const FlushStats fs = impl->GetFlushStats();
    if (fs.flushes_failed == 0 && fs.flushes_completed >= min_completed &&
        impl->immutables_size() == 0) {
      return;
    }
    std::this_thread::yield();
  }
  ADD_FAILURE() << "等待 flush idle 超时";
}

void ForceOneFlush(DB* db, PersistentDBImpl* impl, int filler_base) {
  const uint64_t before = impl->GetFlushStats().flushes_completed;
  // 写足够多的 filler，保证**当前 memtable**（含目标批）一定被冻结并落盘；不能一看到
  // flushes_completed 增长就停——那可能是更早的 immutable 完成，而目标批还没冻结。
  for (int i = 0; i < 2000; ++i) {
    const Status s = db->Put(WriteOptions(), Key(filler_base + i), Val(filler_base + i));
    if (!s.ok()) {
      ADD_FAILURE() << "filler Put 失败：" << s.ToString();
      return;
    }
  }
  WaitForFlushIdle(impl, before + 1);
}

void PutThenForceFlush(DB* db, PersistentDBImpl* impl, const std::string& key,
                       const std::string& value, int filler_base) {
  const Status s = db->Put(WriteOptions(), key, value);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ForceOneFlush(db, impl, filler_base);
}

uint64_t CountDeletionsInSstFiles(Env* env, const std::string& dir) {
  std::vector<std::string> children;
  const Status cs = env->GetChildren(dir, &children);
  if (!cs.ok()) {
    ADD_FAILURE() << "GetChildren 失败：" << cs.ToString();
    return 0;
  }
  uint64_t count = 0;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (!ParseTableFileName(c, &number)) continue;
    std::shared_ptr<Table> t;
    const Status os = Table::Open(Options(), env, dir + "/" + c, &t);
    if (!os.ok()) {
      ADD_FAILURE() << "打开 " << c << " 失败：" << os.ToString();
      continue;
    }
    std::unique_ptr<Iterator> it = t->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      Slice user_key;
      SequenceNumber seq = 0;
      ValueType type = kTypeValue;
      if (!ParseInternalKey(it->key(), &user_key, &seq, &type)) continue;
      if (type == kTypeDeletion) ++count;
    }
  }
  return count;
}

class EventLogEnv : public MemEnv {
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

 private:
  class LogFile : public WritableFile {
   public:
    LogFile(WritableFile* inner, EventLogEnv* env, std::string name)
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
    EventLogEnv* env_;
    std::string name_;
  };

  mutable std::mutex mu_;
  std::vector<std::string> events_;
};

class RecordingFlushHook : public FlushHook {
 public:
  explicit RecordingFlushHook(EventLogEnv* env) : env_(env) {}
  void OnSSTableWritten() override { env_->Record("hook:written"); }
  void OnBeforeRename() override { env_->Record("hook:rename"); }
  void OnBeforeRegister() override { env_->Record("hook:register"); }

 private:
  EventLogEnv* env_;
};

class WidenedSpyEnv : public MemEnv {
 public:
  std::atomic<uint64_t> violations{0};
  std::atomic<uint64_t> append_calls{0};
  std::atomic<uint64_t> sync_calls{0};
  std::atomic<uint64_t> rename_calls{0};
  std::atomic<uint64_t> sync_dir_calls{0};
  std::atomic<uint64_t> get_file_size_calls{0};
  std::atomic<uint64_t> get_children_calls{0};
  std::atomic<uint64_t> remove_file_calls{0};
  std::atomic<uint64_t> truncate_calls{0};
  std::atomic<uint64_t> block_read_calls{0};

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(fname, &inner);
    if (s.ok()) *result = new SpyFile(inner, this);
    return s;
  }
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(fname, &inner);
    if (s.ok()) *result = new SpyFile(inner, this);
    return s;
  }
  Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    RandomAccessFile* inner = nullptr;
    const Status s = MemEnv::NewRandomAccessFile(fname, &inner);
    if (s.ok()) *result = new SpyRandomFile(inner, this);
    return s;
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    ++rename_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::RenameFile(src, target);
  }
  Status SyncDir(const std::string& dirname) override {
    ++sync_dir_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::SyncDir(dirname);
  }
  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    ++get_file_size_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::GetFileSize(fname, size);
  }
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    ++get_children_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::GetChildren(dir, result);
  }
  Status RemoveFile(const std::string& fname) override {
    ++remove_file_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::RemoveFile(fname);
  }
  Status Truncate(const std::string& fname, uint64_t size) override {
    ++truncate_calls;
    if (DbMutexHeldOnThisThread()) ++violations;
    return MemEnv::Truncate(fname, size);
  }

 private:
  class SpyFile : public WritableFile {
   public:
    SpyFile(WritableFile* inner, WidenedSpyEnv* env) : inner_(inner), env_(env) {}
    ~SpyFile() override { delete inner_; }
    Status Append(const Slice& data) override {
      ++env_->append_calls;
      if (DbMutexHeldOnThisThread()) ++env_->violations;
      return inner_->Append(data);
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
    WidenedSpyEnv* env_;
  };

  class SpyRandomFile : public RandomAccessFile {
   public:
    SpyRandomFile(RandomAccessFile* inner, WidenedSpyEnv* env) : inner_(inner), env_(env) {}
    ~SpyRandomFile() override { delete inner_; }
    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
      ++env_->block_read_calls;
      if (DbMutexHeldOnThisThread()) ++env_->violations;
      return inner_->Read(offset, n, result, scratch);
    }

   private:
    RandomAccessFile* inner_;
    WidenedSpyEnv* env_;
  };
};

class FlushFailingEnv : public MemEnv {
 public:
  enum class FailAt { kNone, kWrite, kSync, kRename, kSyncDir };
  FailAt fail = FailAt::kNone;

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewWritableFile(fname, &inner);
    if (s.ok()) *result = new FailFile(inner, this, fname);
    return s;
  }
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    WritableFile* inner = nullptr;
    const Status s = MemEnv::NewAppendableFile(fname, &inner);
    if (s.ok()) *result = new FailFile(inner, this, fname);
    return s;
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    if (fail == FailAt::kRename && target.find(".sst") != std::string::npos) {
      return Status::IOError("FlushFailingEnv: injected rename failure", src);
    }
    if (target.size() >= 4 && target.compare(target.size() - 4, 4, ".sst") == 0) {
      sst_renamed_ = true;   // M3.3：只有 SSTable 已 rename 之后才注入 SyncDir 失败
    }
    return MemEnv::RenameFile(src, target);
  }
  Status SyncDir(const std::string& dirname) override {
    // M3.3 起轮转也会调 SyncDir；本注入点只针对 flush 的"rename 之后、注册之前"那一次
    // （§6.3 步骤 ⑥），否则轮转的 SyncDir 会先失败、flush 根本不会发生（A24 会空测）。
    if (fail == FailAt::kSyncDir && sst_renamed_) {
      return Status::IOError("FlushFailingEnv: injected SyncDir failure", dirname);
    }
    return MemEnv::SyncDir(dirname);
  }

 private:
  static bool IsSstTmp(const std::string& name) {
    return name.size() >= 8 && name.compare(name.size() - 8, 8, ".sst.tmp") == 0;
  }

  bool sst_renamed_ = false;

  class FailFile : public WritableFile {
   public:
    FailFile(WritableFile* inner, FlushFailingEnv* env, std::string name)
        : inner_(inner), env_(env), name_(std::move(name)) {}
    ~FailFile() override { delete inner_; }
    Status Append(const Slice& data) override {
      if (env_->fail == FailAt::kWrite && IsSstTmp(name_)) {
        return Status::IOError("FlushFailingEnv: injected write failure", name_);
      }
      return inner_->Append(data);
    }
    Status Flush() override { return inner_->Flush(); }
    Status Sync() override {
      if (env_->fail == FailAt::kSync && IsSstTmp(name_)) {
        return Status::IOError("FlushFailingEnv: injected fsync failure", name_);
      }
      return inner_->Sync();
    }
    Status Close() override { return inner_->Close(); }

   private:
    WritableFile* inner_;
    FlushFailingEnv* env_;
    std::string name_;
  };
};

class BlockingFlushHook : public FlushHook {
 public:
  void OnBeforeRename() override {
    entered_.store(true, std::memory_order_release);
    while (!released_.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  bool WaitUntilEntered() {
    for (int i = 0; i < 400000; ++i) {
      if (entered_.load(std::memory_order_acquire)) return true;
      std::this_thread::yield();
    }
    return false;
  }
  void Release() { released_.store(true, std::memory_order_release); }

 private:
  std::atomic<bool> entered_{false};
  std::atomic<bool> released_{false};
};

}  // namespace

// ==== M3-A20 ====
TEST(Flush, AutoFlushOnFullBufferNeverFrozen) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const int kCount = 400;
  for (int i = 1; i <= kCount; ++i) {
    const Status s = db->Put(WriteOptions(), Key(i), Val(i));
    ASSERT_TRUE(s.ok()) << "M3 起容量不足必须靠 flush 消化，不得返回 kFrozen：" << s.ToString();
  }
  WaitForFlushIdle(impl, 1);
  EXPECT_GE(impl->GetFlushStats().flushes_completed, 1u);
  for (int i = 1; i <= kCount; ++i) {
    std::string v;
    const Status g = db->Get(Key(i), &v);
    ASSERT_TRUE(g.ok()) << "key " << i << " 不可读：" << g.ToString();
    EXPECT_EQ(Val(i), v);
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A21 ====
TEST(Flush, SingleEntryLargerThanWriteBufferAccepted) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 4 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const std::string big(64 * 1024, 'x');
  ASSERT_TRUE(db->Put(WriteOptions(), "big", big).ok());
  std::string v;
  ASSERT_TRUE(db->Get("big", &v).ok());
  EXPECT_EQ(big, v);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok()) << "防「粘性写只读」复活";
  }
  EXPECT_TRUE(impl->GetFlushStats().last_error.empty());
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A22 ====
TEST(Flush, OrderDurableRenameSyncDirRegister) {
  EventLogEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  RecordingFlushHook hook(&env);
  options.flush_hook = &hook;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 400; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
  WaitForFlushIdle(impl, 1);

  const std::vector<std::string> events = env.Snapshot();
  const auto find_event = [&events](const std::string& needle) {
    return std::find(events.begin(), events.end(), needle);
  };
  const auto find_prefix = [&events](const std::string& prefix) {
    return std::find_if(events.begin(), events.end(), [&prefix](const std::string& e) {
      return e.compare(0, prefix.size(), prefix) == 0;
    });
  };

  const auto written = find_event("hook:written");
  const auto sst_sync = std::find_if(events.begin(), events.end(), [](const std::string& e) {
    return e.compare(0, 5, "sync:") == 0 && e.find(".sst.tmp") != std::string::npos;
  });
  const auto rename_hook = find_event("hook:rename");
  const auto rename = find_prefix("rename:/db/");
  // M3.3 起轮转也会 SyncDir（在 SSTable rename 之前）；这里必须取 **rename 之后**的那一次，
  // 否则测的是轮转的 SyncDir 而不是 §6.3 步骤 ⑥。
  const auto syncdir = std::find_if(rename, events.end(), [](const std::string& e) {
    return e.compare(0, 8, "syncdir:") == 0;
  });
  const auto reg = find_event("hook:register");

  ASSERT_NE(events.end(), written) << "OnSSTableWritten 未被调用";
  ASSERT_NE(events.end(), sst_sync) << "没有看到 .sst.tmp 的 fsync";
  ASSERT_NE(events.end(), rename_hook) << "OnBeforeRename 未被调用";
  ASSERT_NE(events.end(), rename) << "没有看到 rename";
  ASSERT_NE(events.end(), syncdir) << "没有看到 SyncDir";
  ASSERT_NE(events.end(), reg) << "OnBeforeRegister 未被调用";

  ASSERT_NE(std::string::npos, sst_sync->find(".sst.tmp"));
  ASSERT_NE(std::string::npos, rename->find(".sst.tmp"));
  EXPECT_LT(written, sst_sync) << "fsync 必须晚于「文件写完」";
  EXPECT_LT(sst_sync, rename_hook) << "fsync 必须早于「rename 前」";
  EXPECT_LT(rename_hook, rename) << "OnBeforeRename 必须早于 rename";
  EXPECT_LT(rename, syncdir) << "rename 必须早于 SyncDir";
  EXPECT_LT(syncdir, reg) << "SyncDir 必须早于注册";

  for (const std::string& e : events) {
    if (e.compare(0, 5, "sync:") == 0) {
      const bool is_tmp = e.find(".sst.tmp") != std::string::npos;
      const bool is_sst = e.find(".sst") != std::string::npos;
      if (is_sst) {
        EXPECT_TRUE(is_tmp) << "注册后的 .sst 不得再被 fsync：" << e;
      }
    }
  }
  // M4：注册 = 活动元数据的原子发布（CURRENT + MANIFEST；对象=活动元数据实现）。
  // 断言的是**更强**的顺序链：SSTable rename → OnBeforeRegister → 元数据 tmp 写 → fsync → rename → SyncDir。
  const auto meta_sync = std::find_if(events.begin(), events.end(), [](const std::string& e) {
    return e.compare(0, 5, "sync:") == 0 && e.find("MANIFEST-") != std::string::npos &&
           e.find(".tmp") != std::string::npos;
  });
  const auto meta_rename = std::find_if(events.begin(), events.end(), [](const std::string& e) {
    return e.compare(0, 7, "rename:") == 0 && e.find("MANIFEST-") != std::string::npos &&
           e.find(".tmp") != std::string::npos;
  });
  ASSERT_NE(events.end(), meta_sync) << "MANIFEST-<n>.tmp 必须先 fsync（L17）";
  ASSERT_NE(events.end(), meta_rename) << "MANIFEST 必须原子 rename（L17）";
  EXPECT_LT(rename, meta_sync) << "SSTable rename 必须早于 MANIFEST 的写";
  EXPECT_LT(reg, meta_sync) << "MANIFEST 的写必须晚于「注册前」注入点";
  EXPECT_LT(meta_sync, meta_rename) << "MANIFEST-<n>.tmp 必须 fsync 后才 rename";
  const auto meta_syncdir = std::find_if(meta_rename, events.end(), [](const std::string& e) {
    return e.compare(0, 8, "syncdir:") == 0;
  });
  ASSERT_NE(events.end(), meta_syncdir) << "MANIFEST rename 之后必须 SyncDir";
  // M3.3：后台线程在注册成功后会做 WAL 回收（RecycleObsoleteLogs）并访问 Env；
  // 本用例必须显式 Close/delete，否则 Env 先析构而后台线程仍在使用它（TSan 实测 data race）。
  ASSERT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A23 ====
TEST(Flush, NoIoWhileHoldingDbMutex) {
  WidenedSpyEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  for (int i = 1; i <= 400; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
  WaitForFlushIdle(impl, 1);
  for (int i = 1; i <= 400; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get(Key(i), &v).ok());
  }

  EXPECT_EQ(0u, env.violations.load()) << "I17/L18：持 DB 互斥锁期间发生了 IO";
  EXPECT_GE(env.append_calls.load(), 1u);
  EXPECT_GE(env.sync_calls.load(), 1u);
  EXPECT_GE(env.rename_calls.load(), 1u);
  EXPECT_GE(env.sync_dir_calls.load(), 1u);
  EXPECT_GE(env.block_read_calls.load(), 1u);

  const uint64_t before = env.violations.load();
  impl->RunHoldingDbMutexForTest([&env] { env.RenameFile("/db/inside", "/db/inside2"); });
  EXPECT_EQ(before + 1, env.violations.load()) << "SpyEnv 必须在持锁 IO 时报警（防「通过但什么都没测」）";

  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A24 ====
TEST(Flush, FailureIsFailStopButReadable) {
  auto run_case = [](FlushFailingEnv::FailAt mode, const char* what) {
    FlushFailingEnv env;
    env.fail = mode;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok()) << what;
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= 1000; ++i) {
      const Status s = db->Put(WriteOptions(), Key(i), Val(i));
      if (!s.ok()) break;
    }
    for (int spin = 0; spin < 400000; ++spin) {
      if (impl->GetFlushStats().flushes_failed >= 1) break;
      std::this_thread::yield();
    }
    const FlushStats fs = impl->GetFlushStats();
    ASSERT_EQ(1u, fs.flushes_failed) << what << "：注入点未触发 flush 失败";
    EXPECT_FALSE(fs.last_error.empty()) << what;

    const Status p = db->Put(WriteOptions(), Key(999999), Val(999999));
    EXPECT_FALSE(p.ok()) << what << "：失败后 Put 必须仍失败";
    EXPECT_EQ(fs.last_error, p.ToString()) << what << "：Put 必须返回同一错误";

    std::string v;
    const Status g = db->Get(Key(1), &v);
    EXPECT_TRUE(g.ok()) << what << "：失败后内存中的值必须仍可读：" << g.ToString();

    std::vector<std::string> children;
    ASSERT_TRUE(env.GetChildren("/db", &children).ok());
    uint64_t logs = 0;
    for (const std::string& c : children) {
      if (c.size() >= 4 && c.compare(c.size() - 4, 4, ".log") == 0) ++logs;
    }
    EXPECT_GE(logs, 1u) << what << "：flush 失败不得删 WAL";

    const Status c = db->Close();
    EXPECT_EQ(fs.last_error, c.ToString()) << what << "：Close 必须返回同一错误";
    delete db;
  };

  run_case(FlushFailingEnv::FailAt::kWrite, "write");
  run_case(FlushFailingEnv::FailAt::kSync, "fsync");
  run_case(FlushFailingEnv::FailAt::kRename, "rename");
  run_case(FlushFailingEnv::FailAt::kSyncDir, "SyncDir");
}

// ==== M3-A25 ====
TEST(Flush, ImmutableStallReleasesOnAllConditions) {
  // ①/②/⑤：写者停等 + 后台完成后被唤醒 + stall_events。
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    BlockingFlushHook hook;
    options.flush_hook = &hook;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= 200000 && impl->immutables_size() < 2; ++i) {
      const Status s = db->Put(WriteOptions(), Key(i), Val(i));
      ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_GE(impl->immutables_size(), 2u) << "未能造出 2 个未落盘 immutable";
    ASSERT_TRUE(hook.WaitUntilEntered()) << "后台 flush 未进入可阻塞的注入点";

    std::atomic<bool> done{false};
    std::atomic<int> code{-1};
    std::thread writer([&] {
      const Status s = db->Put(WriteOptions(), Key(999001), Val(999001));
      code.store(static_cast<int>(s.code()), std::memory_order_release);
      done.store(true, std::memory_order_release);
    });
    for (int spin = 0; spin < 400000; ++spin) {
      if (impl->GetFlushStats().stall_events >= 1) break;
      std::this_thread::yield();
    }
    EXPECT_GE(impl->GetFlushStats().stall_events, 1u) << "写者必须真的停等";
    EXPECT_FALSE(done.load()) << "后台未完成前写者不得返回";
    hook.Release();
    writer.join();
    EXPECT_EQ(static_cast<int>(Status::kOk), code.load());
    EXPECT_TRUE(db->Close().ok());
    delete db;
  }

  // ③ bg_error_ 置位 ⇒ 停等立即解除。
  {
    FlushFailingEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    BlockingFlushHook hook;
    options.flush_hook = &hook;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= 200000 && impl->immutables_size() < 2; ++i) {
      const Status s = db->Put(WriteOptions(), Key(i), Val(i));
      ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_GE(impl->immutables_size(), 2u);
    ASSERT_TRUE(hook.WaitUntilEntered());
    std::atomic<bool> done{false};
    std::atomic<int> code{-1};
    std::thread writer([&] {
      const Status s = db->Put(WriteOptions(), Key(999002), Val(999002));
      code.store(static_cast<int>(s.code()), std::memory_order_release);
      done.store(true, std::memory_order_release);
    });
    for (int spin = 0; spin < 400000; ++spin) {
      if (impl->GetFlushStats().stall_events >= 1) break;
      std::this_thread::yield();
    }
    EXPECT_GE(impl->GetFlushStats().stall_events, 1u);
    env.fail = FlushFailingEnv::FailAt::kRename;
    hook.Release();
    writer.join();
    EXPECT_TRUE(done.load()) << "bg_error_ 置位后写者必须被唤醒";
    EXPECT_NE(static_cast<int>(Status::kOk), code.load()) << "写者必须看到 bg_error_";
    EXPECT_GE(impl->GetFlushStats().flushes_failed, 1u);
    db->Close();
    delete db;
  }

  // ④ closed_ 置位 ⇒ 停等立即解除。
  {
    MemEnv env;
    Options options;
    options.env = &env;
    options.write_buffer_size = 8 * 1024;
    BlockingFlushHook hook;
    options.flush_hook = &hook;
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= 200000 && impl->immutables_size() < 2; ++i) {
      const Status s = db->Put(WriteOptions(), Key(i), Val(i));
      ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_GE(impl->immutables_size(), 2u);
    ASSERT_TRUE(hook.WaitUntilEntered());
    std::atomic<bool> done{false};
    std::atomic<int> code{-1};
    std::thread writer([&] {
      const Status s = db->Put(WriteOptions(), Key(999003), Val(999003));
      code.store(static_cast<int>(s.code()), std::memory_order_release);
      done.store(true, std::memory_order_release);
    });
    for (int spin = 0; spin < 400000; ++spin) {
      if (impl->GetFlushStats().stall_events >= 1) break;
      std::this_thread::yield();
    }
    EXPECT_GE(impl->GetFlushStats().stall_events, 1u);
    std::atomic<int> close_code{-1};
    std::thread closer([&] { close_code.store(static_cast<int>(db->Close().code()), std::memory_order_release); });
    for (int spin = 0; spin < 400000; ++spin) {
      if (done.load()) break;
      std::this_thread::yield();
    }
    EXPECT_TRUE(done.load()) << "closed_ 置位后停等必须立即解除";
    EXPECT_EQ(static_cast<int>(Status::kIOError), code.load()) << "closed_ 唤醒后写者应返回 IOError";
    hook.Release();
    closer.join();
    writer.join();
    EXPECT_EQ(static_cast<int>(Status::kOk), close_code.load());
    delete db;
  }
}

// ==== M3-A26 ====
TEST(Flush, TombstoneCountPreserved) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  // M4.2：本用例测的是 **flush** 不得丢 tombstone；把 compaction 触发阈值调高，隔离 compaction
  // 的合法丢弃（I40）对"文件里 tombstone 条数"的影响（断言强度不变）。
  options.level0_file_num_compaction_trigger = 1000;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const int kCount = 400;
  int deletes = 0;
  for (int i = 1; i <= kCount; ++i) {
    if (i % 5 == 0) {
      ASSERT_TRUE(db->Delete(Key(i)).ok());
      ++deletes;
    } else {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
    }
  }
  ForceOneFlush(db, impl, 100000);
  WaitForFlushIdle(impl, 1);
  EXPECT_EQ(static_cast<uint64_t>(deletes), CountDeletionsInSstFiles(&env, "/db"))
      << "flush 不得丢 tombstone（冻结时 " << deletes << " 条）";
  std::string v;
  for (int i = 5; i <= kCount; i += 5) {
    EXPECT_TRUE(db->Get(Key(i), &v).IsNotFound()) << "tombstone 必须屏蔽旧值：key " << i;
  }
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A27 ====
TEST(Read, CoverageMemTableOverSSTable) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutThenForceFlush(db, impl, "k", "old", 1000);
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "new").ok());
  std::string v;
  const Status g = db->Get("k", &v);
  ASSERT_TRUE(g.ok()) << g.ToString();
  EXPECT_EQ("new", v);
  EXPECT_EQ(HitLayer::kMemTable, impl->GetReadStats().hit_layer) << "必须命中 MemTable 而不是 SSTable";
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A28 ====
TEST(Read, TombstoneShadowsSSTableOldValue) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutThenForceFlush(db, impl, "k", "old", 1000);
  ASSERT_TRUE(db->Delete("k").ok());
  std::string v;
  const Status g = db->Get("k", &v);
  EXPECT_EQ(Status::kNotFound, g.code()) << "tombstone 不得复活旧值：" << g.ToString();
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A29 ====
TEST(Read, NewerSSTableTombstoneShadowsOlderFile) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  PutThenForceFlush(db, impl, "k", "old", 1000);
  ASSERT_TRUE(db->Delete("k").ok());
  ForceOneFlush(db, impl, 2000);
  std::string v;
  const Status g = db->Get("k", &v);
  EXPECT_EQ(Status::kNotFound, g.code()) << "新文件的 tombstone 必须屏蔽旧文件的值：" << g.ToString();
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A30 ====
TEST(Read, NewestWinsAcrossThreeFiles) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  // M4.2：本用例测的是 **L0 三文件** 的"逐个检查、不得因 range 提前返回"；
  // 调高 compaction 阈值把三个文件留在 L0（M4 的 L1+ 等效用例见 tests/compaction_test.cpp）。
  options.level0_file_num_compaction_trigger = 1000;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const auto write_and_flush = [&](const std::vector<std::pair<std::string, std::string>>& kvs,
                                   int filler_base) {
    for (const auto& kv : kvs) {
      ASSERT_TRUE(db->Put(WriteOptions(), kv.first, kv.second).ok());
    }
    ForceOneFlush(db, impl, filler_base);
  };
  // 三个文件的 key range 都是 ["a","z"]（同 key "k" 三版；"m" 只在最旧文件里）。
  write_and_flush({{"a", "a1"}, {"k", "v1"}, {"m", "m1"}, {"z", "z1"}}, 1000);
  write_and_flush({{"a", "a2"}, {"k", "v2"}, {"z", "z2"}}, 2000);
  write_and_flush({{"a", "a3"}, {"k", "v3"}, {"z", "z3"}}, 3000);

  std::string v;
  const Status g = db->Get("k", &v);
  ASSERT_TRUE(g.ok()) << g.ToString();
  EXPECT_EQ("v3", v) << "必须读到序号最大的可见版本";

  // "m" 位于三个文件的范围内但只在最旧文件里 ⇒ 必须逐个检查三个文件才命中。
  const DbReadStats before_hit = impl->GetReadStats();
  const Status gm = db->Get("m", &v);
  const DbReadStats after_hit = impl->GetReadStats();
  ASSERT_TRUE(gm.ok()) << gm.ToString();
  EXPECT_EQ("m1", v);
  EXPECT_GE(after_hit.files_checked - before_hit.files_checked, 3u)
      << "不得因 key range 提前返回";

  // 全部未命中：范围内但不存在的 key ⇒ kNotFound 且 hit_layer == none。
  const std::string missing = "n";
  const DbReadStats before_miss = impl->GetReadStats();
  const Status gmiss = db->Get(missing, &v);
  const DbReadStats after_miss = impl->GetReadStats();
  EXPECT_EQ(Status::kNotFound, gmiss.code()) << gmiss.ToString();
  EXPECT_EQ(HitLayer::kNone, after_miss.hit_layer);
  EXPECT_GE(after_miss.files_checked - before_miss.files_checked, 3u)
      << "范围内未命中必须逐个检查过三个文件";
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

}  // namespace test
}  // namespace lsm
