// src/util/env.h —— 最小 Env（docs/m1-design.md §10）
//
// M1 用途：仅 tests/util_test.cpp（真实临时目录，覆盖 kIOError 路径）；M2 的 WAL 直接使用。
// M1 **不写**任何 WAL/SSTable 符号；RandomAccessFile 留到 M3（避免未使用接口）。
// 时间必须是 CLOCK_MONOTONIC：M2 崩溃恢复的时序判据不能受系统时间调整影响。
#ifndef LSM_UTIL_ENV_H_
#define LSM_UTIL_ENV_H_

#include <cstdint>
#include <string>

#include "common.h"

namespace lsm {

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

class Env {
 public:
  virtual ~Env() = default;

  static Env* Default();

  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) = 0;
  virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) = 0;
  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;

  virtual uint64_t NowMicros() = 0;
  virtual void SleepForMicros(uint64_t micros) = 0;
};

}  // namespace lsm

#endif  // LSM_UTIL_ENV_H_
