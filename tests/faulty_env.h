// tests/faulty_env.h —— 信封式故障注入 Env（M2.1 的测试 seam）
//
// 为什么是「信封」而不是完整 MemEnv：#1 的 m2-prerequisites.md 把 MemEnv（内存文件系统 + fsync 水位 +
// 崩溃回滚 + 固定种子撕裂）划给 M2.2——只有崩溃语义用例（A27/A28/CrashSim.*）需要它；
// M2.1 的 A09/A10 只需要「短写 / ENOSPC / fsync 失败」三种注入，用包装真实文件的信封即可，
// 这样 M2.1 的验证面不必等内存文件系统就位。这条拆分已登记在 docs/m2-prerequisites.md §4。
//
// 注意：本类只做注入与计数，不改语义——转发接口时一律交给底层 Env。
#ifndef LSM_TESTS_FAULTY_ENV_H_
#define LSM_TESTS_FAULTY_ENV_H_

#include <memory>
#include <string>
#include <vector>

#include "util/env.h"

namespace lsm {
namespace test {

class FaultyEnv : public Env {
 public:
  explicit FaultyEnv(Env* base) : base_(base) {}

  // 每次 write 最多落 n 字节；0 = 关闭注入（默认）
  void SetShortWrite(size_t n) { short_write_ = n; }
  // 磁盘满：write 返回 -1/ENOSPC
  void SetEnospc(bool v) { enospc_ = v; }
  // 第 n 次 Sync 调用（1-based）开始失败；-1 = 关闭注入（默认）
  void SetSyncFailureAfter(int n) { sync_fail_after_ = n; }
  void ClearSyncFailures() { sync_fail_after_ = -1; }

  int write_calls() const { return write_calls_; }
  int sync_calls() const { return sync_calls_; }

  Status NewWritableFile(const std::string& fname, WritableFile** result) override;
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override;
  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    return base_->NewSequentialFile(fname, result);
  }
  bool FileExists(const std::string& fname) override { return base_->FileExists(fname); }
  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return base_->GetFileSize(fname, size);
  }
  Status DeleteFile(const std::string& fname) override { return base_->DeleteFile(fname); }
  Status RenameFile(const std::string& s, const std::string& t) override {
    return base_->RenameFile(s, t);
  }
  Status CreateDir(const std::string& d) override { return base_->CreateDir(d); }
  Status GetChildren(const std::string& d, std::vector<std::string>* r) override {
    return base_->GetChildren(d, r);
  }
  Status RemoveFile(const std::string& f) override { return base_->RemoveFile(f); }
  Status Truncate(const std::string& f, uint64_t n) override { return base_->Truncate(f, n); }
  Status LockFile(const std::string& f, FileLock** l) override { return base_->LockFile(f, l); }
  Status UnlockFile(FileLock* l) override { return base_->UnlockFile(l); }
  uint64_t NowMicros() override { return base_->NowMicros(); }
  void SleepForMicros(uint64_t us) override { base_->SleepForMicros(us); }

 private:
  class FaultyWritableFile : public WritableFile {
   public:
    FaultyWritableFile(FaultyEnv* env, std::unique_ptr<WritableFile> file)
        : env_(env), file_(std::move(file)) {}
    Status Append(const Slice& data) override;
    Status Flush() override { return file_->Flush(); }
    Status Sync() override;
    Status Close() override { return file_->Close(); }

   private:
    FaultyEnv* const env_;
    std::unique_ptr<WritableFile> file_;
  };

  Env* const base_;
  size_t short_write_ = 0;
  bool enospc_ = false;
  int sync_fail_after_ = -1;
  int write_calls_ = 0;
  int sync_calls_ = 0;
};

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_FAULTY_ENV_H_
