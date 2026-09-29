// src/util/status.cpp —— Status 工厂与 ToString（docs/m1-design.md §4.2）
#include "common.h"

namespace lsm {

const char* Status::CodeName(Code code) {
  switch (code) {
    case kOk: return "OK";
    case kNotFound: return "NotFound";
    case kCorruption: return "Corruption";
    case kNotSupported: return "NotSupported";
    case kInvalidArgument: return "InvalidArgument";
    case kIOError: return "IOError";
    case kFrozen: return "Frozen";
  }
  return "Unknown";
}

// 口径：<CodeName>: <msg>，仅当 msg2 非空时追加 ": <msg2>"。
// 空 msg2 与省略 msg2 必须等价（否则日志里会多出分隔符），空 msg 也必须可读。
Status::Status(Code code, const Slice& msg, const Slice& msg2) : code_(code) {
  msg_.assign(msg.data(), msg.size());
  if (!msg2.empty()) {
    msg_.append(": ");
    msg_.append(msg2.data(), msg2.size());
  }
}

std::string Status::ToString() const {
  if (code_ == kOk) return "OK";
  std::string out(CodeName(code_));
  out.append(": ");
  out.append(msg_);
  return out;
}

#define LSM_STATUS_FACTORY(Name, Code)                                        \
  Status Status::Name(const Slice& msg, const Slice& msg2) {                  \
    return Status(Code, msg, msg2);                                           \
  }

LSM_STATUS_FACTORY(NotFound, kNotFound)
LSM_STATUS_FACTORY(Corruption, kCorruption)
LSM_STATUS_FACTORY(NotSupported, kNotSupported)
LSM_STATUS_FACTORY(InvalidArgument, kInvalidArgument)
LSM_STATUS_FACTORY(IOError, kIOError)
LSM_STATUS_FACTORY(Frozen, kFrozen)
#undef LSM_STATUS_FACTORY

}  // namespace lsm
