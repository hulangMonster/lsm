// tests/env_slow_sync.h —— 「fsync 闸门」信封式 Env（R1 的确定性注入 seam）
//
// 用途（docs/m2-design.md §15/§14 的 R1）：把一次真实的 fsync 人工**持有一段时间**，并让测试
// 精确知道「fsync 正在飞行」这一窗口，从而把「持锁期间不做 IO / 入队不被 fsync 挡住」这类判据
// 从"靠 sleep 猜时序"变成"靠原子状态判定事实"。
//
// 注入点为什么选 WritableFile::Sync：
//   PersistentDBImpl::Sync() → WALWriter::Sync() → WritableFile::Sync()（src/wal.cpp 的
//   `file_->Sync()`）是**唯一**的真实 fsync 出口；本包装器不改变任何其他语义，只在这一个方法上
//   插入闸门 ⇒ 注入必然覆盖 DB::Sync()/RotateLog()/组提交 fsync 的真实代码路径。
//
// 纪律：本类只做阻塞与计数，转发接口一律交给底层 Env；不含任何生产逻辑，只属于 tests/。
#ifndef LSM_TESTS_ENV_SLOW_SYNC_H_
#define LSM_TESTS_ENV_SLOW_SYNC_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "util/env.h"

namespace lsm {
namespace test {

// 可独立于 Env 复用的 fsync 闸门。
// 状态机：Disarmed →(ArmForMicros)→ Armed →(第一个 Enter)→ InFlight →(Release 或到期)→ Disarmed
//
// 实现纪律（TSan 证据驱动）：本闸门**只用原子变量**，不持任何 mutex/condition_variable。
// 起因：v1 用 mutex + condition_variable 实现时，TSan 在**栈复用的测试 seam** 上给出
// "double lock of a mutex … Mutex … is already destroyed" 与随之而来的 data race（原始输出：
// `SyncIsolation.*` 的 TSan 跑出 6 条报告，全部指向本文件；同一跑法的 src/ 侧 0 条）。
// 判据本身不依赖任何内部同步原语：所有判据读的都是下面的原子量（in_flight()/entered()），
// 等待只是**机制**（有界轮询 + 兜底到期），不参与判据。
class FsyncGate {
 public:
  // 布防：**下一次**进入 Sync 的线程会停在闸门内，直到 Release() 被调用或 hold_micros 到期。
  // 一次布防只拦截一次进入（后续 fsync 不阻塞），这样"注入一次慢 fsync"的语义是确定的。
  void ArmForMicros(uint64_t hold_micros) {
    armed_.store(true, std::memory_order_seq_cst);
    released_.store(false, std::memory_order_seq_cst);
    in_flight_.store(false, std::memory_order_seq_cst);
    hold_micros_.store(hold_micros, std::memory_order_seq_cst);
    entered_.store(0, std::memory_order_seq_cst);
    finished_.store(0, std::memory_order_seq_cst);
  }

  // 由被包装的 WritableFile::Sync 调用；返回即表示"这次 fsync 可以真正执行了"。
  void Enter() {
    if (!armed_.exchange(false, std::memory_order_seq_cst)) return;   // 只拦截第一个进入者
    in_flight_.store(true, std::memory_order_seq_cst);
    entered_.fetch_add(1, std::memory_order_seq_cst);
    const Clock::time_point deadline =
        Clock::now() + std::chrono::microseconds(hold_micros_.load(std::memory_order_seq_cst));
    // 兜底：到期自动放行 ⇒ 任何用例都不可能因为闸门而挂死（判据本身不依赖这个兜底）。
    // 轮询只是"检测放行"的机制；50 us 的粒度远小于任何判据的时限（>=200 ms）。
    while (!released_.load(std::memory_order_seq_cst)) {
      if (Clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    in_flight_.store(false, std::memory_order_seq_cst);
    finished_.fetch_add(1, std::memory_order_seq_cst);
  }

  // 等待"有线程卡在闸门里"。返回 false = 超时（注入点从未被进入 ⇒ 判据会退化成恒真，必须报错）。
  bool WaitUntilInFlight(uint64_t timeout_ms) {
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (in_flight_.load(std::memory_order_seq_cst) ||
          finished_.load(std::memory_order_seq_cst) > 0) {
        return true;
      }
      if (Clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return in_flight_.load(std::memory_order_seq_cst) ||
           finished_.load(std::memory_order_seq_cst) > 0;
  }

  // 放行（幂等）。ArmForMicros 之前调用无副作用。
  void Release() {
    released_.store(true, std::memory_order_seq_cst);
    armed_.store(false, std::memory_order_seq_cst);
  }

  bool in_flight() const { return in_flight_.load(std::memory_order_seq_cst); }
  uint64_t entered() const { return entered_.load(std::memory_order_seq_cst); }
  uint64_t finished() const { return finished_.load(std::memory_order_seq_cst); }
  uint64_t hold_micros() const { return hold_micros_.load(std::memory_order_seq_cst); }

 private:
  using Clock = std::chrono::steady_clock;
  std::atomic<bool> armed_{false};
  std::atomic<bool> in_flight_{false};
  std::atomic<bool> released_{false};
  std::atomic<uint64_t> hold_micros_{0};
  std::atomic<uint64_t> entered_{0};
  std::atomic<uint64_t> finished_{0};
};

// 信封式 Env：只把 WritableFile::Sync 接到闸门上，其余一律转发。
class SlowSyncEnv : public Env {
 public:
  SlowSyncEnv(Env* base, FsyncGate* gate) : base_(base), gate_(gate) {}

  Status NewWritableFile(const std::string& fname, WritableFile** result) override;
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override;
  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    return base_->NewSequentialFile(fname, result);
  }
  Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    return base_->NewRandomAccessFile(fname, result);
  }
  bool FileExists(const std::string& fname) override { return base_->FileExists(fname); }
  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return base_->GetFileSize(fname, size);
  }
  Status DeleteFile(const std::string& fname) override { return base_->DeleteFile(fname); }
  Status RenameFile(const std::string& src, const std::string& target) override {
    return base_->RenameFile(src, target);
  }
  Status CreateDir(const std::string& dirname) override { return base_->CreateDir(dirname); }
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return base_->GetChildren(dir, result);
  }
  Status RemoveFile(const std::string& fname) override { return base_->RemoveFile(fname); }
  Status Truncate(const std::string& fname, uint64_t size) override {
    return base_->Truncate(fname, size);
  }
  Status SyncDir(const std::string& dirname) override { return base_->SyncDir(dirname); }
  Status LockFile(const std::string& fname, FileLock** lock) override {
    return base_->LockFile(fname, lock);
  }
  Status UnlockFile(FileLock* lock) override { return base_->UnlockFile(lock); }
  uint64_t NowMicros() override { return base_->NowMicros(); }
  void SleepForMicros(uint64_t micros) override { base_->SleepForMicros(micros); }

 private:
  class GatedWritableFile : public WritableFile {
   public:
    GatedWritableFile(FsyncGate* gate, std::unique_ptr<WritableFile> file)
        : gate_(gate), file_(std::move(file)) {}
    Status Append(const Slice& data) override { return file_->Append(data); }
    Status Flush() override { return file_->Flush(); }
    // 唯一的注入点：先停在闸门（模拟慢 fsync 的窗口），再真正 fsync。
    Status Sync() override {
      gate_->Enter();
      return file_->Sync();
    }
    Status Close() override { return file_->Close(); }

   private:
    FsyncGate* const gate_;
    std::unique_ptr<WritableFile> file_;
  };

  Env* const base_;
  FsyncGate* const gate_;
};

inline Status SlowSyncEnv::NewWritableFile(const std::string& fname, WritableFile** result) {
  WritableFile* raw = nullptr;
  const Status s = base_->NewWritableFile(fname, &raw);
  if (!s.ok()) return s;
  *result = new GatedWritableFile(gate_, std::unique_ptr<WritableFile>(raw));
  return Status::OK();
}

inline Status SlowSyncEnv::NewAppendableFile(const std::string& fname, WritableFile** result) {
  WritableFile* raw = nullptr;
  const Status s = base_->NewAppendableFile(fname, &raw);
  if (!s.ok()) return s;
  *result = new GatedWritableFile(gate_, std::unique_ptr<WritableFile>(raw));
  return Status::OK();
}

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_ENV_SLOW_SYNC_H_
