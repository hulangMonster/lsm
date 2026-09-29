// src/util/env_posix.cpp —— 最小 Env 的 POSIX 实现
//
// 错误口径（docs/m1-design.md §4.2 / design §10）：所有失败都必须带路径上下文返回 kIOError，
// 且失败时把出参置空（禁止半构造对象）。
#include "util/env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include <dirent.h>
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
  PosixWritableFile(const PosixWritableFile&) = delete;
  PosixWritableFile& operator=(const PosixWritableFile&) = delete;
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

// M3：pread 语义的随机读（不改变文件偏移，天然可并发；const 方法内不改任何状态）。
class PosixRandomAccessFile : public RandomAccessFile {
 public:
  PosixRandomAccessFile(int fd, std::string fname) : fd_(fd), fname_(std::move(fname)) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) ::close(fd_);
  }
  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    if (fd_ < 0) return Status::IOError("PosixRandomAccessFile: closed", fname_);
    const ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    if (r < 0) return Status::IOError(ErrnoMessage("pread", fname_));
    *result = Slice(scratch, static_cast<size_t>(r));
    return Status::OK();
  }

 private:
  int fd_;
  std::string fname_;
};

class PosixSequentialFile : public SequentialFile {
 public:
  explicit PosixSequentialFile(FILE* f) : file_(f) {}
  PosixSequentialFile(const PosixSequentialFile&) = delete;
  PosixSequentialFile& operator=(const PosixSequentialFile&) = delete;
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

// M2 的 D10：用 fcntl 写锁做进程级独占。崩溃时内核自动释放（不需要清理陈旧锁文件）。
class PosixFileLock : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override {
    if (fd_ >= 0) ::close(fd_);
  }
  int fd() const { return fd_; }

 private:
  int fd_;
};

class PosixEnv : public Env {
 public:
  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return Status::IOError(ErrnoMessage("NewAppendableFile", fname));
    PosixWritableFile* f = new (std::nothrow) PosixWritableFile(fd);
    if (f == nullptr) {
      ::close(fd);
      return Status::IOError("NewAppendableFile: out of memory", fname);
    }
    *result = f;
    return Status::OK();
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return Status::IOError(ErrnoMessage("GetChildren", dir));
    while (struct dirent* e = ::readdir(d)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      result->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) return Status::IOError(ErrnoMessage("RemoveFile", fname));
    return Status::OK();
  }

  Status Truncate(const std::string& fname, uint64_t size) override {
    if (::truncate(fname.c_str(), static_cast<off_t>(size)) != 0) {
      return Status::IOError(ErrnoMessage("Truncate", fname));
    }
    return Status::OK();
  }

  Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;
    const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return Status::IOError(ErrnoMessage("LockFile: open", fname));
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd, F_SETLK, &fl) != 0) {
      ::close(fd);
      return Status::IOError(ErrnoMessage("LockFile: already held by another process", fname));
    }
    *lock = new PosixFileLock(fd);
    return Status::OK();
  }

  Status UnlockFile(FileLock* lock) override {
    if (lock == nullptr) return Status::OK();
    PosixFileLock* l = static_cast<PosixFileLock*>(lock);
    const int fd = l->fd();
    if (fd >= 0) {
      struct flock fl;
      std::memset(&fl, 0, sizeof(fl));
      fl.l_type = F_UNLCK;
      fl.l_whence = SEEK_SET;
      ::fcntl(fd, F_SETLK, &fl);
    }
    delete l;
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    *result = nullptr;
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return Status::IOError(ErrnoMessage("NewWritableFile", fname));
    // nothrow + 失败时关掉 fd：否则 new 抛 bad_alloc 会把已打开的 fd 漏掉（#4 评审建议 6）
    PosixWritableFile* f = new (std::nothrow) PosixWritableFile(fd);
    if (f == nullptr) {
      ::close(fd);
      return Status::IOError("NewWritableFile: out of memory", fname);
    }
    *result = f;
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    *result = nullptr;
    const int fd = ::open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT) return Status::NotFound("NewRandomAccessFile", fname);
      return Status::IOError(ErrnoMessage("open", fname));
    }
    *result = new PosixRandomAccessFile(fd, fname);
    return Status::OK();
  }

  // M3：目录项 fsync（O_DIRECTORY 只在 Linux 有效；本仓库的运行环境是 Ubuntu 上的 ext4）。
  Status SyncDir(const std::string& dirname) override {
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return Status::IOError(ErrnoMessage("SyncDir: open", dirname));
    const int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) return Status::IOError(ErrnoMessage("SyncDir: fsync", dirname));
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname, SequentialFile** result) override {
    *result = nullptr;
    FILE* f = std::fopen(fname.c_str(), "r");
    if (f == nullptr) return Status::IOError(ErrnoMessage("NewSequentialFile", fname));
    PosixSequentialFile* file = new (std::nothrow) PosixSequentialFile(f);
    if (file == nullptr) {
      std::fclose(f);
      return Status::IOError("NewSequentialFile: out of memory", fname);
    }
    *result = file;
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

  Status DeleteFile(const std::string& fname) override { return RemoveFile(fname); }

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
