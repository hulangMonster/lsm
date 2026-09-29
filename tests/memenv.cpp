// tests/memenv.cpp —— MemEnv 实现（崩溃模型见 memenv.h 头注释）
#include "memenv.h"

#include <algorithm>
#include <cstring>

namespace lsm {
namespace test {
namespace {

class MemFileLock : public FileLock {
 public:
  explicit MemFileLock(std::string name) : name_(std::move(name)) {}
  const std::string& name() const { return name_; }

 private:
  std::string name_;
};

class MemWritableFile : public WritableFile {
 public:
  MemWritableFile(MemEnv* env, std::string name) : env_(env), name_(std::move(name)) {}
  Status Append(const Slice& data) override {
    if (data.empty()) return Status::OK();
    size_t off = 0;
    const size_t chunk = env_->short_write() == 0 ? data.size() : env_->short_write();
    while (off < data.size()) {
      const size_t n = std::min(chunk, data.size() - off);
      size_t written = 0;
      if (!env_->AppendRaw(name_, data.data() + off, n, &written)) {
        return Status::IOError("MemEnv: injected ENOSPC", name_);
      }
      if (written == 0) return Status::IOError("MemEnv: no progress writing", name_);
      off += written;
    }
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override {
    if (!env_->SyncRaw(name_)) {
      return Status::IOError("MemEnv: injected fsync failure", name_);
    }
    return Status::OK();
  }
  Status Close() override { return Status::OK(); }

 private:
  MemEnv* const env_;
  const std::string name_;
};

class MemSequentialFile : public SequentialFile {
 public:
  MemSequentialFile(std::string data) : data_(std::move(data)) {}
  Status Read(size_t n, Slice* result, char* scratch) override {
    const size_t avail = data_.size() - pos_;
    const size_t take = std::min(n, avail);
    if (take > 0) std::memcpy(scratch, data_.data() + pos_, take);
    pos_ += take;
    *result = Slice(scratch, take);
    return Status::OK();
  }
  Status Skip(uint64_t n) override {
    pos_ = std::min(data_.size(), pos_ + static_cast<size_t>(n));
    return Status::OK();
  }

 private:
  std::string data_;
  size_t pos_ = 0;
};

}  // namespace

MemEnv::File* MemEnv::Find(const std::string& fname) {
  auto it = files_.find(fname);
  if (it == files_.end() || !it->second.exists) return nullptr;
  return &it->second;
}

const MemEnv::File* MemEnv::Find(const std::string& fname) const {
  auto it = files_.find(fname);
  if (it == files_.end() || !it->second.exists) return nullptr;
  return &it->second;
}

uint32_t MemEnv::NextRandom() {
  // xorshift64*：确定性、无 <random>（跨平台/跨标准库可复现）
  uint64_t x = rng_state_;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_state_ = x;
  return static_cast<uint32_t>((x * 2685821657736338717ull) >> 32);
}

bool MemEnv::AppendRaw(const std::string& fname, const char* data, size_t n, size_t* written) {
  ++write_calls_;
  if (enospc_) {
    *written = 0;
    return false;
  }
  File& f = files_[fname];
  f.exists = true;
  f.data.append(data, n);
  *written = n;
  return true;
}

bool MemEnv::SyncRaw(const std::string& fname) {
  ++sync_calls_;
  if (sync_fail_after_ >= 0 && sync_calls_ >= sync_fail_after_) return false;
  File* f = Find(fname);
  if (f == nullptr) return false;
  f->synced_size = f->data.size();
  return true;
}

void MemEnv::SimulateCrash() {
  for (auto& kv : files_) {
    File& f = kv.second;
    if (!f.exists) continue;
    const std::string unsynced = f.data.substr(std::min<size_t>(f.synced_size, f.data.size()));
    f.data.resize(std::min<size_t>(f.synced_size, f.data.size()));   // 先回滚：未 fsync 的后缀视为丢失
    if (!unsynced.empty() && tear_probability_ > 0.0) {
      const bool tear = (NextRandom() % 1000u) < static_cast<uint32_t>(tear_probability_ * 1000.0);
      if (tear) {
        const size_t keep = NextRandom() % (unsynced.size() + 1);     // 0..全部：撕裂位置随机
        f.data.append(unsynced.data(), keep);
      }
    }
  }
}

std::string MemEnv::Contents(const std::string& fname) const {
  const File* f = Find(fname);
  return f == nullptr ? std::string() : f->data;
}

uint64_t MemEnv::SyncedSize(const std::string& fname) const {
  const File* f = Find(fname);
  return f == nullptr ? 0 : f->synced_size;
}

void MemEnv::SetContents(const std::string& fname, const std::string& data) {
  File& f = files_[fname];
  f.exists = true;
  f.data = data;
  f.synced_size = data.size();
}

Status MemEnv::NewWritableFile(const std::string& fname, WritableFile** result) {
  *result = nullptr;
  File& f = files_[fname];
  f.exists = true;
  f.data.clear();
  f.synced_size = 0;
  *result = new MemWritableFile(this, fname);
  return Status::OK();
}

Status MemEnv::NewAppendableFile(const std::string& fname, WritableFile** result) {
  *result = nullptr;
  File& f = files_[fname];
  f.exists = true;
  *result = new MemWritableFile(this, fname);
  return Status::OK();
}

Status MemEnv::NewSequentialFile(const std::string& fname, SequentialFile** result) {
  *result = nullptr;
  const File* f = Find(fname);
  if (f == nullptr) return Status::IOError("MemEnv::NewSequentialFile: not found", fname);
  *result = new MemSequentialFile(f->data);
  return Status::OK();
}

bool MemEnv::FileExists(const std::string& fname) { return Find(fname) != nullptr; }

Status MemEnv::GetFileSize(const std::string& fname, uint64_t* size) {
  const File* f = Find(fname);
  if (f == nullptr) return Status::IOError("MemEnv::GetFileSize: not found", fname);
  *size = f->data.size();
  return Status::OK();
}

Status MemEnv::DeleteFile(const std::string& fname) { return RemoveFile(fname); }

Status MemEnv::RemoveFile(const std::string& fname) {
  auto it = files_.find(fname);
  if (it == files_.end() || !it->second.exists) {
    return Status::IOError("MemEnv::RemoveFile: not found", fname);
  }
  it->second.exists = false;
  return Status::OK();
}

Status MemEnv::RenameFile(const std::string& src, const std::string& target) {
  const File* s = Find(src);
  if (s == nullptr) return Status::IOError("MemEnv::RenameFile: not found", src);
  File moved = *s;
  files_[src].exists = false;
  files_[target] = moved;
  return Status::OK();
}

Status MemEnv::CreateDir(const std::string& dirname) {
  dirs_.insert(dirname);
  return Status::OK();
}

Status MemEnv::GetChildren(const std::string& dir, std::vector<std::string>* result) {
  result->clear();
  const std::string prefix = dir.empty() ? std::string() : dir + "/";
  for (const auto& kv : files_) {
    if (!kv.second.exists) continue;
    const std::string& name = kv.first;
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = name.substr(prefix.size());
    if (rest.find('/') != std::string::npos) continue;   // 只列直接子项
    result->push_back(rest);
  }
  std::sort(result->begin(), result->end());
  return Status::OK();
}

Status MemEnv::Truncate(const std::string& fname, uint64_t size) {
  File* f = Find(fname);
  if (f == nullptr) return Status::IOError("MemEnv::Truncate: not found", fname);
  if (size < f->data.size()) {
    f->data.resize(static_cast<size_t>(size));
  } else {
    f->data.resize(static_cast<size_t>(size), '\0');
  }
  f->synced_size = std::min<uint64_t>(f->synced_size, f->data.size());
  return Status::OK();
}

Status MemEnv::LockFile(const std::string& fname, FileLock** lock) {
  *lock = nullptr;
  if (locked_.count(fname) != 0) {
    return Status::IOError("MemEnv::LockFile: already held", fname);
  }
  locked_.insert(fname);
  auto handle = std::unique_ptr<FileLock>(new MemFileLock(fname));
  *lock = handle.get();
  lock_handles_[fname] = std::move(handle);
  return Status::OK();
}

Status MemEnv::UnlockFile(FileLock* lock) {
  if (lock == nullptr) return Status::OK();
  const std::string name = static_cast<MemFileLock*>(lock)->name();
  locked_.erase(name);
  lock_handles_.erase(name);
  return Status::OK();
}

}  // namespace test
}  // namespace lsm
