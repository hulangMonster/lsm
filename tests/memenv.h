// tests/memenv.h —— 内存文件系统 Env（M2.2e 的崩溃语义 seam）
//
// 为什么必须有它（docs/m2-prerequisites.md §7.1）：M2 #0 实测证明 **kill -9 产生不了 torn record**
// （裸 write 逐条 100 轮全完好；带 stdio 缓冲 100 轮 95 轮撕裂）——进程级崩溃无法验证掉电语义。
// 真正能确定性验证「掉电 = 丢失未 fsync 的后缀 + 可能留下撕裂尾」的只有内存文件系统。
//
// 崩溃模型（诚实且可复现）：
//   每个文件维护 **fsync 水位** synced_size（= 最后一次 Sync 成功时的字节数）。
//   SimulateCrash() 对每个文件：先回滚到 synced_size（未 fsync 的后缀视为丢失），
//   再以概率 p 把「未 fsync 后缀的一个随机前缀」写回（模拟"部分字节恰好落盘" ⇒ 撕裂尾）。
//   ⇒ 要么干净回滚、要么尾部残缺，二者都不允许出现"半条 record 被当成有效"。
//   所有随机性由**固定种子**驱动（A 组零 flaky，种子可读、可打进 INFO）。
#ifndef LSM_TESTS_MEMENV_H_
#define LSM_TESTS_MEMENV_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "util/env.h"

namespace lsm {
namespace test {

class MemEnv : public Env {
 public:
  MemEnv() = default;
  ~MemEnv() override = default;
  MemEnv(const MemEnv&) = delete;
  MemEnv& operator=(const MemEnv&) = delete;

  // ---- Env 接口 ----
  Status NewWritableFile(const std::string& fname, WritableFile** result) override;
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override;
  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override;
  bool FileExists(const std::string& fname) override;
  Status GetFileSize(const std::string& fname, uint64_t* size) override;
  Status DeleteFile(const std::string& fname) override;
  Status RenameFile(const std::string& src, const std::string& target) override;
  Status CreateDir(const std::string& dirname) override;
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override;
  Status RemoveFile(const std::string& fname) override;
  Status Truncate(const std::string& fname, uint64_t size) override;
  Status LockFile(const std::string& fname, FileLock** lock) override;
  Status UnlockFile(FileLock* lock) override;
  uint64_t NowMicros() override { return now_micros_; }
  void SleepForMicros(uint64_t micros) override { now_micros_ += micros; }   // 假时钟：不真的睡

  // ---- 崩溃模拟（确定性的掉电语义）----
  void SetTearProbability(double p) { tear_probability_ = p; }   // [0,1]
  void SetSeed(uint64_t seed) { rng_state_ = seed == 0 ? 1 : seed; }
  uint64_t seed() const { return rng_state_; }
  uint32_t NextRandom();                                        // xorshift64*，确定性
  void SimulateCrash();                                         // 见文件头注释的崩溃模型

  // ---- 故障注入（与 FaultyEnv 同口径，便于 A27~A31 自包含）----
  void SetShortWrite(size_t n) { short_write_ = n; }
  size_t short_write() const { return short_write_; }   // 供文件类在注入模式下分块写
  void SetEnospc(bool v) { enospc_ = v; }
  void SetSyncFailureAfter(int n) { sync_fail_after_ = n; }
  void ClearSyncFailures() { sync_fail_after_ = -1; }
  int write_calls() const { return write_calls_; }
  int sync_calls() const { return sync_calls_; }

  // ---- 测试内省 ----
  std::string Contents(const std::string& fname) const;
  uint64_t SyncedSize(const std::string& fname) const;
  void SetContents(const std::string& fname, const std::string& data);   // 直接注入字节（构造畸形输入）

  // 由文件类回写；返回 false 表示注入的写失败（ENOSPC）
  bool AppendRaw(const std::string& fname, const char* data, size_t n, size_t* written);
  bool SyncRaw(const std::string& fname);

 private:
  struct File {
    std::string data;
    uint64_t synced_size = 0;
    bool exists = true;
  };
  File* Find(const std::string& fname);
  const File* Find(const std::string& fname) const;

  // 并发用例（I32 回归：在飞 Append 与 Sync 并发）要求内部串行化：真实 POSIX 文件的并发
  // write/fsync 由内核保证，内存替身没有内核，必须自己加锁。用 recursive_mutex 是因为
  // DeleteFile→RemoveFile、FileExists→Find 这类公开方法之间存在互相调用。
  mutable std::recursive_mutex mu_;
  std::map<std::string, File> files_;
  std::set<std::string> dirs_;
  std::set<std::string> locked_;
  std::map<std::string, std::unique_ptr<FileLock>> lock_handles_;

  uint64_t now_micros_ = 1000000;      // 假时钟起点
  uint64_t rng_state_ = 0x5EED2026ull; // 固定种子（A27/A28 必须可复现）
  double tear_probability_ = 0.0;

  size_t short_write_ = 0;
  bool enospc_ = false;
  int sync_fail_after_ = -1;
  int write_calls_ = 0;
  int sync_calls_ = 0;
};

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_MEMENV_H_
