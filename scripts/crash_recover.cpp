// scripts/crash_recover.cpp —— 崩溃对账的校验端（docs/m2-design.md §8.2）
//
// 判据（唯一可宣称 durable 的模式）：sidecar 里每一行都必须能在恢复后的 DB 里查到同名同值，
// 否则 missing/mismatch 计数 +1 并以退出码 1 结束。
// 复用 DB::Get 作为独立参照（而不是自己扫 WAL）：避免"恢复逻辑的 bug"与"对账逻辑的 bug"互相掩盖。
//
// 用法：crash_recover <db_dir> <sidecar>
// 输出：ROUND 0 ACKED <a> RECOVERED <r> MISSING <m> MISMATCH <mm> TRUNCATED_BYTES <t> OPEN_MS <ms>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "db.h"
#include "filename.h"
#include "util/env.h"

using namespace lsm;

namespace {

uint64_t LogSize(const std::string& dir) {
  uint64_t total = 0;
  Env* env = Env::Default();
  std::vector<std::string> children;
  if (!env->GetChildren(dir, &children).ok()) return 0;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (!ParseLogFileName(c, &n)) continue;
    uint64_t sz = 0;
    if (env->GetFileSize(dir + "/" + c, &sz).ok()) total += sz;
  }
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: crash_recover <db_dir> <sidecar>\n");
    return 2;
  }
  const std::string db_dir = argv[1];
  const std::string sidecar = argv[2];

  const uint64_t size_before = LogSize(db_dir);
  const auto t0 = std::chrono::steady_clock::now();
  DB* db = nullptr;
  const Status os = DB::Open(Options(), db_dir, &db);
  const auto t1 = std::chrono::steady_clock::now();
  const long long open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  if (!os.ok()) {
    std::fprintf(stderr, "DB::Open failed during recovery: %s\n", os.ToString().c_str());
    std::printf("ROUND 0 ACKED 0 RECOVERED 0 MISSING 1 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS %lld\n",
                open_ms);
    return 1;
  }
  const uint64_t size_after = LogSize(db_dir);
  const uint64_t truncated = size_before > size_after ? size_before - size_after : 0;

  std::ifstream in(sidecar);
  long long acked = 0;
  long long recovered = 0;
  long long missing = 0;
  long long mismatch = 0;
  std::string key;
  std::string val;
  while (in >> key >> val) {
    ++acked;
    std::string got;
    const Status gs = db->Get(Slice(key), &got);
    if (!gs.ok()) {
      ++missing;
    } else if (got != val) {
      ++mismatch;
    } else {
      ++recovered;
    }
  }
  std::printf(
      "ROUND 0 ACKED %lld RECOVERED %lld MISSING %lld MISMATCH %lld TRUNCATED_BYTES %llu OPEN_MS %lld\n",
      acked, recovered, missing, mismatch, static_cast<unsigned long long>(truncated), open_ms);
  db->Close();
  delete db;
  return (missing == 0 && mismatch == 0) ? 0 : 1;
}
