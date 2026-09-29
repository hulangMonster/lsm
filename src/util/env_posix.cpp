// src/util/env_posix.cpp —— 最小 Env 的 POSIX 实现
//
// 错误口径（docs/m1-design.md §4.2 / design §10）：所有失败都必须带路径上下文返回 kIOError，
// 且失败时把出参置空（禁止半构造对象）。
#include "util/env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace lsm {
namespace {

std::string ErrnoMessage(const char* what, const std::string& fname) {
  std::string msg(what);
  msg.append(": ");
  msg.append(fname);
  msg.append(": ");
  msg.append(std::strerror(errno));
  return msg;
}

class PosixWritableFile : public WritableFile {
 public:
  explicit PosixWritableFile(int fd) : fd_(fd) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  Status Append(const Slice& data) override {
    size_t written = 0;
    while (written < data.size()) {
      const ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
      if (n < 0) {
        if (errno == EINTR) continue;
        return Status::IOError("WritableFile::Append: write failed", std::strerror(errno));
      }
      written += static_cast<size_t>(n);
    }
    return Status::OK();
  }

  Status Flush() override { return Status::OK(); }   // 直接写 fd，无用户态缓冲

  Status Sync() override {
    if (::fsync(fd_) != 0) {
      return Status::IOError("WritableFile::Sync: fsync failed", std::strerror(errno));
    }
    return Status::OK();
  }

  Status Close() override {
    if (fd_ < 0) return Status::OK();
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      return Status::IOError("WritableFile::Close: close failed", std::strerror(errno));
    }
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixSequentialFile : public SequentialFile {
 public:
  explicit PosixSequentialFile(FILE* f) : file_(f) {}
  ~PosixSequentialFile() override {
    if (file_ != nullptr) std::fclose(file_);
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    const size_t r = std::fread(scratch, 1, n, file_);
    if (r < n && std::ferror(file_) != 0) {
      return Status::IOError("SequentialFile::Read: fread failed", std::strerror(errno));
    }
    *result = Slice(scratch, r);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (std::fseek(file_, static_cast<long>(n), SEEK_CUR) != 0) {
      return Status::IOError("SequentialFile::Skip: fseek failed", std::strerror(errno));
    }
    return Status::OK();
  }

 private:
  FILE* file_;
};

class PosixEnv : public Env {
 public:
  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return Status::IOError(ErrnoMessage("NewWritableFile", fname));
    *result = new PosixWritableFile(fd);
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    *result = nullptr;
    FILE* f = std::fopen(fname.c_str(), "r");
    if (f == nullptr) return Status::IOError(ErrnoMessage("NewSequentialFile", fname));
    *result = new PosixSequentialFile(f);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return ::access(fname.c_str(), F_OK) == 0;
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat sb;
    if (::stat(fname.c_str(), &sb) != 0) {
      return Status::IOError(ErrnoMessage("GetFileSize", fname));
    }
    *size = static_cast<uint64_t>(sb.st_size);
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) {
      return Status::IOError(ErrnoMessage("DeleteFile", fname));
    }
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return Status::IOError(ErrnoMessage("RenameFile", src + " -> " + target));
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0 && errno != EEXIST) {
      return Status::IOError(ErrnoMessage("CreateDir", dirname));
    }
    return Status::OK();
  }

  uint64_t NowMicros() override {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
  }

  void SleepForMicros(uint64_t micros) override {
    if (micros == 0) return;
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(micros / 1000000ull);
    ts.tv_nsec = static_cast<long>((micros % 1000000ull) * 1000ull);
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
      // 被信号打断后按剩余时间继续睡
    }
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv kInstance;   // C++11 magic static：进程内单例，线程安全
  return &kInstance;
}

}  // namespace lsm
