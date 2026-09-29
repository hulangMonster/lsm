// src/util/env.h —— 最小 Env（docs/m1-design.md §10）
//
// M1 用途：仅 tests/util_test.cpp（真实临时目录，覆盖 kIOError 路径）；M2 的 WAL 直接使用。
// M1 **不写**任何 WAL/SSTable 符号；RandomAccessFile 留到 M3（避免未使用接口）。
// 时间必须是 CLOCK_MONOTONIC：M2 崩溃恢复的时序判据不能受系统时间调整影响。
#ifndef LSM_UTIL_ENV_H_
#define LSM_UTIL_ENV_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace lsm {

// 进程级文件锁的句柄（M2 的 D10：LOCK 文件独占）。实现可以是 fd 包装；析构不自动解锁，
// 必须显式 UnlockFile（内核在进程崩溃时会释放 fcntl 锁，这正是选它而不是 flock/自旋文件的原因）。
class FileLock {
 public:
  virtual ~FileLock() = default;
};

class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(const Slice& data) = 0;   // 顺序追加；空 data 必须成功
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;                      // fsync（M2 的 commit 语义依赖它）
  virtual Status Close() = 0;
};

class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // 至多读 n 字节到 scratch，*result 指向 scratch 内的一段；到 EOF 返回空 Slice 且状态 OK。
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// M3 增补（design §11.1 必改清单）：**随机读**。SSTable 的块读取必须能按 (offset, size) 直接读，
// 而不是顺序扫到目标位置（读放大与"打开即扫全文件"都会毁掉 M3 的读路径）。
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  // 从 offset 读至多 n 字节到 scratch，*result 指向 scratch 内的一段；到 EOF 返回空 Slice 且状态 OK。
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

class Env {
 public:
  virtual ~Env() = default;

  static Env* Default();

  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) = 0;

  // M2 增补（design §5.7，只增不改）：以「追加」方式打开已存在的文件（不截断）。
  // WAL 恢复后继续追加、以及测试里对文件做字节级手术都依赖它。
  virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) = 0;

  virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) = 0;

  // M3 增补（design §11.1）：随机读打开。失败语义与 NewSequentialFile 一致（不存在 ⇒ kNotFound）。
  virtual Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) = 0;
  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;

  // M2 增补（design §5.7）：目录枚举（恢复时扫描 *.log）、删文件（DeleteFile 的别名语义）、
  // 截断（恢复时把最高编号 log 截到最后一条完整 record，I18 允许的唯一写）、进程级独占锁。
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  virtual Status Truncate(const std::string& fname, uint64_t size) = 0;
  virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;
  virtual Status UnlockFile(FileLock* lock) = 0;

  // M3 增补（design §3.7/§8.1）：**目录项** fsync。write+fsync+rename 之后若不 SyncDir，掉电时
  // 丢失的是"目录项"本身（文件内容在，名字没了）。META 的原子 rename 与 SSTable 注册都依赖它。
  virtual Status SyncDir(const std::string& dirname) = 0;

  virtual uint64_t NowMicros() = 0;
  virtual void SleepForMicros(uint64_t micros) = 0;
};

}  // namespace lsm

#endif  // LSM_UTIL_ENV_H_
