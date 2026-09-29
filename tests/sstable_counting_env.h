// tests/sstable_counting_env.h —— 薄包装 Env：统计顺序文件打开次数、Read 调用次数与字节数。
//
// 契约来源：docs/m3-design.md §10.1 的 M3-A19（依赖假设「计数 Env」、判据
//   `ReadStats.blocks_read == 0` 且 `key_range_skipped == 1`）。
// 本文件只在 tests/ 下；**不**修改 src/（M3-A19 的 seam 由测试自备）。
#ifndef LSM_TESTS_SSTABLE_COUNTING_ENV_H_
#define LSM_TESTS_SSTABLE_COUNTING_ENV_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "util/env.h"

namespace lsm {
namespace test {

class CountingSequentialFile : public SequentialFile {
 public:
  CountingSequentialFile(std::unique_ptr<SequentialFile> inner, uint64_t* read_calls,
                         uint64_t* bytes_read, uint64_t* skip_calls)
      : inner_(std::move(inner)),
        read_calls_(read_calls),
        bytes_read_(bytes_read),
        skip_calls_(skip_calls) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    ++*read_calls_;
    const Status s = inner_->Read(n, result, scratch);
    if (s.ok()) *bytes_read_ += result->size();
    return s;
  }
  Status Skip(uint64_t n) override {
    ++*skip_calls_;
    return inner_->Skip(n);
  }

 private:
  std::unique_ptr<SequentialFile> inner_;
  uint64_t* read_calls_;
  uint64_t* bytes_read_;
  uint64_t* skip_calls_;
};

class CountingEnv : public Env {
 public:
  // R6-b：Env 新增纯虚函数后必须实现，否则本类变成抽象类（实测会编译失败）。
  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r) override {
    return base_->NewRandomAccessFile(f, r);
  }
  Status SyncDir(const std::string& d) override { return base_->SyncDir(d); }
  explicit CountingEnv(Env* base) : base_(base) {}

  void ResetCounters() {
    sequential_files_opened_ = 0;
    read_calls_ = 0;
    bytes_read_ = 0;
    skip_calls_ = 0;
  }
  uint64_t sequential_files_opened() const { return sequential_files_opened_; }
  uint64_t read_calls() const { return read_calls_; }
  uint64_t bytes_read() const { return bytes_read_; }
  uint64_t skip_calls() const { return skip_calls_; }

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    return base_->NewWritableFile(fname, result);
  }
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    return base_->NewAppendableFile(fname, result);
  }
  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    SequentialFile* inner = nullptr;
    const Status s = base_->NewSequentialFile(fname, &inner);
    if (!s.ok()) {
      *result = nullptr;
      return s;
    }
    ++sequential_files_opened_;
    *result = new CountingSequentialFile(std::unique_ptr<SequentialFile>(inner), &read_calls_,
                                         &bytes_read_, &skip_calls_);
    return Status::OK();
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
  Status LockFile(const std::string& fname, FileLock** lock) override {
    return base_->LockFile(fname, lock);
  }
  Status UnlockFile(FileLock* lock) override { return base_->UnlockFile(lock); }
  uint64_t NowMicros() override { return base_->NowMicros(); }
  void SleepForMicros(uint64_t micros) override { base_->SleepForMicros(micros); }

 private:
  Env* base_;
  uint64_t sequential_files_opened_ = 0;
  uint64_t read_calls_ = 0;
  uint64_t bytes_read_ = 0;
  uint64_t skip_calls_ = 0;
};

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_SSTABLE_COUNTING_ENV_H_
