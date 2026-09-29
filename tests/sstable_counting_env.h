// tests/sstable_counting_env.h —— 薄包装 Env：统计顺序/随机文件打开次数、Read 调用次数与字节数。
//
// 契约来源：docs/m3-design.md §10.1 的 M3-A19（依赖假设「计数 Env」、判据
//   `ReadStats.blocks_read == 0` 且 `key_range_skipped == 1`）。
// M3.2 / R6-e：Table 的块读取从 `NewSequentialFile + Skip + Read` 改为
//   `NewRandomAccessFile + RandomAccessFile::Read`，因此本 seam 的**对象**随之改成随机读计数；
//   断言强度不变（仍要求「范围外一次 IO 都不做、范围内必须真的打开并读块」）。
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

class CountingRandomAccessFile : public RandomAccessFile {
 public:
  CountingRandomAccessFile(std::unique_ptr<RandomAccessFile> inner, uint64_t* read_calls,
                           uint64_t* bytes_read)
      : inner_(std::move(inner)), read_calls_(read_calls), bytes_read_(bytes_read) {}

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    ++*read_calls_;
    const Status s = inner_->Read(offset, n, result, scratch);
    if (s.ok()) *bytes_read_ += result->size();
    return s;
  }

 private:
  std::unique_ptr<RandomAccessFile> inner_;
  uint64_t* read_calls_;
  uint64_t* bytes_read_;
};

class CountingEnv : public Env {
 public:
  explicit CountingEnv(Env* base) : base_(base) {}

  void ResetCounters() {
    sequential_files_opened_ = 0;
    read_calls_ = 0;
    bytes_read_ = 0;
    skip_calls_ = 0;
    random_access_files_opened_ = 0;
    random_read_calls_ = 0;
    random_bytes_read_ = 0;
  }
  uint64_t sequential_files_opened() const { return sequential_files_opened_; }
  uint64_t read_calls() const { return read_calls_; }
  uint64_t bytes_read() const { return bytes_read_; }
  uint64_t skip_calls() const { return skip_calls_; }
  // M3.2 / R6-e：随机读打开次数（Table::ReadExactFile 每次读块打开一个）。
  uint64_t random_access_files_opened() const { return random_access_files_opened_; }
  uint64_t random_read_calls() const { return random_read_calls_; }
  uint64_t random_bytes_read() const { return random_bytes_read_; }

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
  Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    RandomAccessFile* inner = nullptr;
    const Status s = base_->NewRandomAccessFile(fname, &inner);
    if (!s.ok()) {
      *result = nullptr;
      return s;
    }
    ++random_access_files_opened_;
    *result = new CountingRandomAccessFile(std::unique_ptr<RandomAccessFile>(inner),
                                           &random_read_calls_, &random_bytes_read_);
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
  Status SyncDir(const std::string& dirname) override { return base_->SyncDir(dirname); }
  uint64_t NowMicros() override { return base_->NowMicros(); }
  void SleepForMicros(uint64_t micros) override { base_->SleepForMicros(micros); }

 private:
  Env* base_;
  uint64_t sequential_files_opened_ = 0;
  uint64_t read_calls_ = 0;
  uint64_t bytes_read_ = 0;
  uint64_t skip_calls_ = 0;
  uint64_t random_access_files_opened_ = 0;
  uint64_t random_read_calls_ = 0;
  uint64_t random_bytes_read_ = 0;
};

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_SSTABLE_COUNTING_ENV_H_
