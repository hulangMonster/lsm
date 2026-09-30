// scripts/lsm_ampl_probe.cpp —— M4.3：三个放大的实测探针（真实磁盘；B 组的驱动）
// 用法：lsm_ampl_probe --db DIR --rounds N --keys K --write-buffer-size B
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "db_impl.h"
#include "util/env.h"

using namespace lsm;

namespace {
uint64_t CountFds() {
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) return 0;
  uint64_t n = 0;
  while (::readdir(d) != nullptr) ++n;
  ::closedir(d);
  return n;
}
std::string Key(int i) {
  char b[32];
  std::snprintf(b, sizeof(b), "k%08d", i);
  return std::string(b);
}
std::string Val(int i, int round) {
  char b[64];
  std::snprintf(b, sizeof(b), "v%08d-%d", i, round);
  return std::string(b);
}
}  // namespace

int main(int argc, char** argv) {
  std::string dbdir = "/tmp/lsm_ampl_db";
  int rounds = 3;
  int keys = 2000;
  size_t wbs = 256 * 1024;
  std::string strategy = "round_robin";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) dbdir = argv[++i];
    else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--keys") == 0 && i + 1 < argc) keys = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--write-buffer-size") == 0 && i + 1 < argc)
      wbs = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    else if (std::strcmp(argv[i], "--strategy") == 0 && i + 1 < argc)
      strategy = argv[++i];
  }

  Options o;
  o.write_buffer_size = wbs;
  o.compaction_pick_strategy =
      (strategy == "min_overlap") ? PickStrategy::kMinOverlap : PickStrategy::kRoundRobin;
  DB* db = nullptr;
  const Status s = DB::Open(o, dbdir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "OPEN_FAIL %s\n", s.ToString().c_str());
    return 1;
  }
  auto* impl = static_cast<PersistentDBImpl*>(db);
  const uint64_t fd_before = CountFds();

  int missing = 0, mismatch = 0;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 1; i <= keys; ++i) {
      if (!db->Put(Key(i), Val(i, r)).ok()) {
        std::fprintf(stderr, "PUT_FAIL key=%d round=%d\n", i, r);
        return 1;
      }
    }
    std::string v;
    for (int i = 1; i <= keys; ++i) {
      const Status g = db->Get(Key(i), &v);
      if (!g.ok()) {
        ++missing;
      } else if (v != Val(i, r)) {
        ++mismatch;
      }
    }
  }

  // B06：逐次 Get 的 files_checked 增量（p50 / max）
  std::vector<uint64_t> checked;
  std::string v;
  for (int i = 1; i <= keys; ++i) {
    const DbReadStats b = impl->GetReadStats();
    db->Get(Key(i), &v);
    const DbReadStats a = impl->GetReadStats();
    checked.push_back(a.files_checked - b.files_checked);
  }
  std::sort(checked.begin(), checked.end());
  const uint64_t checked_p50 = checked.empty() ? 0 : checked[checked.size() / 2];
  const uint64_t checked_max = checked.empty() ? 0 : checked.back();

  const uint64_t fd_after = CountFds();
  const uint64_t fd_growth = fd_after > fd_before ? fd_after - fd_before : 0;

  const ManifestStats ms = impl->GetManifestStats();
  const CompactionStats cs = impl->GetCompactionStats();
  const AmplificationStats a = impl->GetAmplificationStats();
  const std::string front = impl->FormatFrontLine("ALL");
  unsigned long long p99 = 0;
  {
    const char* pos = std::strstr(front.c_str(), "p99_us=");
    if (pos != nullptr) std::sscanf(pos, "p99_us=%llu", &p99);
  }
  const bool b05_ok = (fd_growth <= 8);
  const bool b06_ok = (checked_p50 <= 3) && (checked_max <= 12);
  const bool b07_ok = (a.compaction_round_p50_us > 0) && (p99 * 10 <= a.compaction_round_p50_us);

  std::printf("STRATEGY %s\n", strategy.c_str());
  std::printf("%s\n", impl->FormatAmplLine("ALL").c_str());
  std::printf("%s\n", impl->FormatLevelLine("ALL").c_str());
  std::printf("%s\n", front.c_str());
  std::printf("MISSING %d MISMATCH %d COMPACTION_ROUNDS_TOTAL %llu MANIFEST_ROLLS %llu "
              "MANIFEST_BYTES %llu LIVE_VERSIONS_MAX %llu READ_FILES_CHECKED_P50 %llu "
              "READ_FILES_CHECKED_MAX %llu READ_BASELINE_M3 59 FD_GROWTH %llu ROUND_SAMPLES %llu\n",
              missing, mismatch, static_cast<unsigned long long>(cs.completed),
              static_cast<unsigned long long>(ms.rolls),
              static_cast<unsigned long long>(ms.bytes),
              static_cast<unsigned long long>(a.live_versions_max),
              static_cast<unsigned long long>(checked_p50),
              static_cast<unsigned long long>(checked_max),
              static_cast<unsigned long long>(fd_growth),
              static_cast<unsigned long long>(a.round_samples));
  std::printf("B05_OK %d B06_OK %d B07_OK %d\n", b05_ok ? 1 : 0, b06_ok ? 1 : 0, b07_ok ? 1 : 0);
  std::printf("M4-B10 NOT_APPLICABLE (block cache not introduced in M4; see docs/m4-design.md 2.1 D8)\n");

  const Status c = db->Close();
  delete db;
  if (!c.ok()) {
    std::fprintf(stderr, "CLOSE_FAIL %s\n", c.ToString().c_str());
    return 1;
  }
  if (missing != 0 || mismatch != 0) return 1;
  return 0;
}
