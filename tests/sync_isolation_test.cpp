// tests/sync_isolation_test.cpp —— R1 回归：fsync 窗口不得挡住并发写者的入队
//
// 缺陷（R1，docs/m2-design.md 的修订记录）：`PersistentDBImpl::Sync()` 在**整个函数体**内持
// `commit_mu_`，其中包含 `log_->Sync()`（真实 fsync）；`RotateLog()` 的旧 log fsync 同样在
// `commit_mu_` 之下。⇒ 一次显式 `Sync()`（或一次 memtable 轮转）的整个 fsync 窗口内，所有
// `sync=false` 的并发写者都被挡在 `commit_mu_` 外：不能入队、也无法与后续批合并。
//
// 判据为什么是确定性的（不靠 sleep 猜时间）：
//   注入点是**唯一**的真实 fsync 出口 —— 被包装 Env 的 `WritableFile::Sync`（`WALWriter::Sync`
//   在 src/wal.cpp 里就是调它）。`FsyncGate` 把这次 fsync 人工持有 kInjectedFsyncUs，并让测试
//   精确知道「fsync 正在飞行」这一事实（`in_flight()`）。判据全部落在"窗口内返回 / 窗口内到达
//   commit_mu_"这两个可判定事实上；兜底超时只用于"绝不挂死"，不参与判据。
//
// 反向自检（防空绿）：
//   * 必须先证明注入点真的被进入（`entered() == 1`），否则"没被挡住"是恒真断言；
//   * 必须证明 `Sync()` 在窗口内还没有返回、且其耗时确实被注入拉长；
//   * 必须证明异步写者**写成功了**（返回 kOk 且可读），而不是"快速返回了一个错误"。
#include "test_harness.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "db.h"
#include "db_impl.h"
#include "env_slow_sync.h"
#include "memenv.h"

namespace lsm {
namespace test {
namespace {

using Clock = std::chrono::steady_clock;

// 注入的 fsync 窗口（人工持有）与异步写者的返回上界：10× 余量，避免把调度抖动当成判据。
constexpr uint64_t kInjectedFsyncUs = 2000ull * 1000ull;
constexpr uint64_t kAsyncWriteBoundUs = 200ull * 1000ull;
constexpr uint64_t kRotationFsyncUs = 1000ull * 1000ull;
constexpr uint64_t kQueueReachBoundUs = 200ull * 1000ull;
// 只用于"绝不挂死"的兜底（闸门自身到期也会放行，所以不会有真正的挂死）。
constexpr uint64_t kFallbackWaitMs = 20000;

uint64_t UsSince(Clock::time_point t0) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

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
std::string BigVal(size_t n) { return std::string(n, 'x'); }

struct Fixture {
  MemEnv mem;
  FsyncGate gate;
  SlowSyncEnv env{&mem, &gate};
};

PersistentDBImpl* OpenDB(SlowSyncEnv* env, const std::string& name, size_t write_buffer_size) {
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

// RAII：任何 ASSERT 提前返回都不会让 joinable 的线程析构（那会 std::terminate）。
class ThreadScope {
 public:
  void Add(std::thread* t) { threads_.push_back(t); }
  void JoinAll() {
    for (std::thread* t : threads_) {
      if (t->joinable()) t->join();
    }
    threads_.clear();
  }
  ~ThreadScope() { JoinAll(); }

 private:
  std::vector<std::thread*> threads_;
};

// 在 fsync 窗口内反复尝试「到达 commit_mu_ 并读队列长度」。
// 返回：第一次读到的 pending 数；若 bound_us 内始终为 0 则返回 0。
// *first_us 记录**第一次**调用的耗时 —— 窗口内被挡住时它会一直阻塞到闸门放行。
uint64_t ProbePendingWriters(PersistentDBImpl* db, uint64_t bound_us, uint64_t* first_us) {
  bool have_first = false;
  const Clock::time_point loop_t0 = Clock::now();
  for (;;) {
    const Clock::time_point call_t0 = Clock::now();
    const size_t n = db->pending_writers();
    if (!have_first) {
      have_first = true;
      *first_us = UsSince(call_t0);
    }
    if (n > 0) return static_cast<uint64_t>(n);
    if (UsSince(loop_t0) > bound_us) return 0;
  }
}

// ---------------------------------------------------------------------------
// 用例 1（R1 主判据）：显式 DB::Sync() 的 fsync 窗口内，异步写者必须能入队并返回。
// ---------------------------------------------------------------------------
TEST(SyncIsolation, AsyncWriteIsNotBlockedByExplicitSync) {
  Fixture fx;
  std::unique_ptr<PersistentDBImpl> db(OpenDB(&fx.env, "/iso1", 8u << 20));
  ASSERT_TRUE(db != nullptr);

  // 先让若干条写真正 ack，得到 Sync() 必须覆盖的水位下界（I32 方向：只能少报，绝不许多报）。
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
  const SequenceNumber acked_before = db->last_sequence();
  ASSERT_GT(acked_before, 0u);

  fx.gate.ArmForMicros(kInjectedFsyncUs);
  std::promise<Status> pa;
  std::future<Status> fa = pa.get_future();
  const Clock::time_point a_t0 = Clock::now();
  std::thread ta([&] { pa.set_value(db->Sync()); });
  ThreadScope ts;
  ts.Add(&ta);

  // ---- 反向自检：注入点真的被进入了 ----
  ASSERT_TRUE(fx.gate.WaitUntilInFlight(kFallbackWaitMs))
      << "注入的 fsync 闸门从未被进入 —— DB::Sync() 没有走到 WritableFile::Sync";
  ASSERT_EQ(1ull, fx.gate.entered());
  EXPECT_TRUE(fx.gate.in_flight()) << "注入的 fsync 应当仍在飞行";
  EXPECT_EQ(std::future_status::timeout, fa.wait_for(std::chrono::milliseconds(0)))
      << "Sync() 在注入的 fsync 仍在飞行时就返回了（窗口语义不成立）";

  // ---- 判据主体：fsync 窗口内的一次异步（sync=false）写 ----
  std::promise<Status> pb;
  std::future<Status> fb = pb.get_future();
  const Clock::time_point b_t0 = Clock::now();
  std::thread tb([&] { pb.set_value(db->Put(WriteOptions(), Key(100), Val(100))); });
  ts.Add(&tb);

  const std::future_status b_ready = fb.wait_for(std::chrono::milliseconds(kFallbackWaitMs));
  const uint64_t b_elapsed_us = UsSince(b_t0);
  // 关键结构性事实：异步写者返回时，注入的 fsync 是否仍在飞行。
  const bool b_returned_inside_window = fx.gate.in_flight();
  const bool a_still_pending =
      (fa.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

  // 正常路径**不**主动放行：让注入的 fsync 窗口自然到期（ArmForMicros 的持有时间）。
  // 否则"Sync() 必须在窗口结束后才返回"会退化成自证循环（窗口是我们自己在 B 返回后才结束的）。
  // Release() 只作为兜底逃生口：B 连兜底超时都没返回时才用。
  if (b_ready != std::future_status::ready) fx.gate.Release();
  ts.JoinAll();
  const uint64_t a_elapsed_us = UsSince(a_t0);
  const Status sync_status = fa.get();
  const Status put_status = fb.get();

  EXPECT_TRUE(b_returned_inside_window)
      << "R1：异步写者在注入的 fsync 窗口**之外**才返回（返回时闸门已放行）—— 被 commit_mu_ 挡住了";
  EXPECT_EQ(std::future_status::ready, b_ready)
      << "R1：异步写者在 " << kFallbackWaitMs << " ms 兜底超时内都没有返回";
  EXPECT_LT(b_elapsed_us, kAsyncWriteBoundUs)
      << "R1：注入的 fsync 窗口 = " << (kInjectedFsyncUs / 1000) << " ms，要求异步写者 < "
      << (kAsyncWriteBoundUs / 1000) << " ms 返回；实测 " << b_elapsed_us << " us";
  EXPECT_TRUE(a_still_pending) << "Sync() 不得在注入的 fsync 窗口内返回";

  // 反向自检（续）：注入确实把 Sync() 拉长了；两个写者都成功；水位语义仍成立。
  EXPECT_GE(a_elapsed_us, kInjectedFsyncUs * 9 / 10)
      << "Sync() 必须在注入窗口结束后才返回；实测 " << a_elapsed_us << " us";
  EXPECT_TRUE(sync_status.ok()) << sync_status.ToString();
  EXPECT_TRUE(put_status.ok()) << put_status.ToString();
  EXPECT_GE(db->durable_seq(), acked_before)
      << "Sync() 返回后，调用前已 ack 的写必须已 durable（I32 不得被弱化）";
  EXPECT_LE(db->durable_seq(), db->log_last_appended_seq()) << "水位不得超过已 Append 边界";
  std::string v;
  EXPECT_TRUE(db->Get(Key(100), &v).ok());
  EXPECT_EQ(Val(100), v);
}

// ---------------------------------------------------------------------------
// 用例 2（同类缺陷的第二个落点）：RotateLog() 的旧 log fsync 也不得持有 commit_mu_。
//
// 判据：在轮转的 fsync 窗口内，旁观者必须能**到达 commit_mu_**（`pending_writers()` 会取它），
// 且此时队列里必须**真的有**一个写者（证明"窗口内可入队"不是恒真）。
// 这里不断言"写者快速返回"——轮转期间 flusher 仍在飞，写者本来就要等本批结算，那与 R1 无关。
// ---------------------------------------------------------------------------
TEST(SyncIsolation, CommitQueueReachableDuringRotationFsync) {
  Fixture fx;
  // write_buffer_size 取小值：第二条写必然让 memtable 触顶 ⇒ 冻结 + 轮转（阶段 A' 的 fsync）。
  std::unique_ptr<PersistentDBImpl> db(OpenDB(&fx.env, "/iso2", 8192));
  ASSERT_TRUE(db != nullptr);
  ASSERT_TRUE(db->Put(WriteOptions(), Key(0), BigVal(4096)).ok());
  ASSERT_GT(db->log_last_appended_seq(), 0u);

  fx.gate.ArmForMicros(kRotationFsyncUs);
  std::promise<Status> pw;
  std::future<Status> fw = pw.get_future();
  std::thread tw([&] { pw.set_value(db->Put(WriteOptions(), Key(1), BigVal(8192))); });
  ThreadScope ts;
  ts.Add(&tw);

  ASSERT_TRUE(fx.gate.WaitUntilInFlight(kFallbackWaitMs))
      << "轮转的 fsync 没有走到 WritableFile::Sync（注入点未覆盖该代码路径）";
  ASSERT_EQ(1ull, fx.gate.entered());

  // 第二个写者：应当在轮转的 fsync 窗口内成功入队。
  std::promise<Status> p2;
  std::future<Status> f2 = p2.get_future();
  std::thread t2([&] { p2.set_value(db->Put(WriteOptions(), Key(2), Val(2))); });
  ts.Add(&t2);

  // 旁观者：窗口内到达 commit_mu_，并看到"队列里真的有人"。
  uint64_t probe_first_us = 0;
  std::promise<uint64_t> pp;
  std::future<uint64_t> fp = pp.get_future();
  std::thread tp([&] {
    pp.set_value(ProbePendingWriters(db.get(), kQueueReachBoundUs, &probe_first_us));
  });
  ts.Add(&tp);

  const std::future_status p_ready = fp.wait_for(std::chrono::milliseconds(kFallbackWaitMs));
  const bool probe_inside_window = fx.gate.in_flight();

  fx.gate.Release();
  ts.JoinAll();
  const uint64_t pending_observed = fp.get();
  const Status w1 = fw.get();
  const Status w2 = f2.get();

  EXPECT_EQ(std::future_status::ready, p_ready) << "旁观者在兜底超时内没有返回";
  EXPECT_TRUE(probe_inside_window)
      << "R1：旁观者在轮转的 fsync 窗口**之外**才拿到 commit_mu_ —— 被 commit_mu_ 挡住了";
  EXPECT_LT(probe_first_us, kQueueReachBoundUs)
      << "R1：轮转的注入窗口 = " << (kRotationFsyncUs / 1000) << " ms，要求窗口内取 commit_mu_ < "
      << (kQueueReachBoundUs / 1000) << " ms；实测 " << probe_first_us << " us";
  EXPECT_GE(pending_observed, 1ull)
      << "非空绿：轮转的 fsync 窗口内必须真的有写者进了队列（实测 " << pending_observed << "）";
  EXPECT_TRUE(w1.ok()) << w1.ToString();
  EXPECT_TRUE(w2.ok()) << w2.ToString();
  std::string v1;
  std::string v2;
  EXPECT_TRUE(db->Get(Key(1), &v1).ok());
  EXPECT_TRUE(db->Get(Key(2), &v2).ok());
  EXPECT_EQ(BigVal(8192), v1);
  EXPECT_EQ(Val(2), v2);
}

}  // namespace
}  // namespace test
}  // namespace lsm
