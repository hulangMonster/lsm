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

// ---- M4 追加：MANIFEST / CURRENT（docs/m4-design.md §3.1）----

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s%06llu", kManifestFilePrefix,
                static_cast<unsigned long long>(number));
  return MakeFileName(dbname, buf);
}

std::string ManifestTempFileName(const std::string& dbname, uint64_t number) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s%06llu.tmp", kManifestFilePrefix,
                static_cast<unsigned long long>(number));
  return MakeFileName(dbname, buf);
}

std::string CurrentFileName(const std::string& dbname) { return MakeFileName(dbname, "CURRENT"); }

std::string CurrentTempFileName(const std::string& dbname) {
  return MakeFileName(dbname, "CURRENT.tmp");
}

namespace {

// 精确解析 "PREFIX<digits>"；允许任意位数的十进制（%06u 只是最小宽度，
// n >= 1000000 时是 7 位 —— 设计 §3.1 的正则 [0-9]{6} 字面会拒绝合法编号，这里按数值语义放宽，
// 差异登记在 docs/m4-prerequisites.md）。
bool ParsePrefixedNumber(const std::string& fname, const char* prefix, uint64_t* number) {
  const size_t plen = std::strlen(prefix);
  if (fname.size() <= plen) return false;
  if (fname.compare(0, plen, prefix) != 0) return false;
  const size_t digits = fname.size() - plen;
  if (digits == 0 || digits > 20) return false;
  uint64_t n = 0;
  for (size_t i = plen; i < fname.size(); ++i) {
    const char c = fname[i];
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

}  // namespace

bool ParseManifestTempFileName(const std::string& fname, uint64_t* number) {
  if (fname.size() <= 4) return false;
  if (fname.compare(fname.size() - 4, 4, ".tmp") != 0) return false;
  return ParsePrefixedNumber(fname.substr(0, fname.size() - 4), kManifestFilePrefix, number);
}

bool ParseManifestFileName(const std::string& fname, uint64_t* number) {
  if (ParseManifestTempFileName(fname, number)) return false;   // 先排除 .tmp（精确匹配）
  return ParsePrefixedNumber(fname, kManifestFilePrefix, number);
}

bool ParseCurrentContents(const std::string& contents, uint64_t* number) {
  if (contents.size() < 2 || contents.size() > 21) return false;   // 1..20 位数字 + '\n'
  if (contents.back() != '\n') return false;
  uint64_t n = 0;
  for (size_t i = 0; i + 1 < contents.size(); ++i) {
    const char c = contents[i];
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

}  // namespace lsm
