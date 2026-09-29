// tests/faulty_env.cpp —— 信封式故障注入 Env 的实现（见 faulty_env.h 的说明）
#include "faulty_env.h"

#include <algorithm>
#include <string>

namespace lsm {
namespace test {

Status FaultyEnv::FaultyWritableFile::Append(const Slice& data) {
  ++env_->write_calls_;
  if (env_->enospc_) {
    return Status::IOError("FaultyEnv: injected ENOSPC", "Append");
  }
  if (env_->short_write_ == 0) {
    return file_->Append(data);   // 未注入：直接转发（底层实现自己会处理部分写）
  }
  // 注入「底层一次只接受 N 字节」：切成 N 字节的块逐个写。底层 Append 是 all-or-nothing 语义，
  // 所以数据最终仍然完整——这条注入验证的是「写入路径不会被短写打乱」，而不是制造丢数据。
  size_t off = 0;
  while (off < data.size()) {
    const size_t n = std::min(env_->short_write_, data.size() - off);
    const Status s = file_->Append(Slice(data.data() + off, n));
    if (!s.ok()) return s;
    off += n;
  }
  return Status::OK();
}

Status FaultyEnv::FaultyWritableFile::Sync() {
  ++env_->sync_calls_;
  if (env_->sync_fail_after_ >= 0 && env_->sync_calls_ >= env_->sync_fail_after_) {
    return Status::IOError("FaultyEnv: injected fsync failure (call #" +
                               std::to_string(env_->sync_calls_) + ")",
                           "Sync");
  }
  return file_->Sync();
}

Status FaultyEnv::NewWritableFile(const std::string& fname, WritableFile** result) {
  WritableFile* raw = nullptr;
  const Status s = base_->NewWritableFile(fname, &raw);
  if (!s.ok()) {
    *result = nullptr;
    return s;
  }
  *result = new FaultyWritableFile(this, std::unique_ptr<WritableFile>(raw));
  return Status::OK();
}

Status FaultyEnv::NewAppendableFile(const std::string& fname, WritableFile** result) {
  WritableFile* raw = nullptr;
  const Status s = base_->NewAppendableFile(fname, &raw);
  if (!s.ok()) {
    *result = nullptr;
    return s;
  }
  *result = new FaultyWritableFile(this, std::unique_ptr<WritableFile>(raw));
  return Status::OK();
}

}  // namespace test
}  // namespace lsm
