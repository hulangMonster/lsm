// src/filename.cpp
#include "filename.h"

#include <cstdio>
#include <cstring>

namespace lsm {

std::string MakeFileName(const std::string& dbname, const std::string& suffix) {
  return dbname.empty() ? suffix : dbname + "/" + suffix;
}

std::string LogFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.log", static_cast<unsigned long long>(number));
  return MakeFileName(dbname, buf);
}

bool ParseLogFileName(const std::string& fname, uint64_t* number) {
  const size_t suffix_len = std::strlen(kLogFileSuffix);
  if (fname.size() <= suffix_len) return false;
  if (fname.compare(fname.size() - suffix_len, suffix_len, kLogFileSuffix) != 0) return false;
  uint64_t n = 0;
  const size_t digits = fname.size() - suffix_len;
  for (size_t i = 0; i < digits; ++i) {
    const char c = fname[i];
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

std::string LockFileName(const std::string& dbname) { return MakeFileName(dbname, "LOCK"); }

}  // namespace lsm
