// scripts/batch_crash_recover.cpp —— M5.2 批崩溃对账的核对端（docs/m5-design.md §10.2 的 M5-B01）
//
// 判据（三条都要为 0/成立）：
//   ① 已 ack 的批必须**整批**可见且值逐字节一致  ⇒ MISSING 0 / MISMATCH 0
//   ② 任何批都不得出现「0 < 可见条数 < batch_size」 ⇒ HALF 0（I51 的崩溃侧证据）
//   ③ 至少要看到过数据（ACKED > 0 且 BATCHES_SEEN > 0），否则门禁是空绿（由脚本判）
//
// 输出固定行：ROUND ACKED <n> MISSING <n> MISMATCH <n> HALF <n> BATCHES_SEEN <n>
//
// 用法：batch_crash_recover <db_dir> <sidecar> <batch_size>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "db.h"

using namespace lsm;

namespace {

std::string KeyOf(long long batch, int i) {
  char key[48];
  std::snprintf(key, sizeof(key), "b%08lld_%04d", batch, i);
  return std::string(key);
}
std::string ValOf(long long batch, int i) {
  char val[64];
  std::snprintf(val, sizeof(val), "v%08lld_%04d", batch, i);
  return std::string(val);
}

// "b%08lld_%04d" ⇒ (batch id, index)；不是这个形状就返回 false。
bool ParseBatchKey(const std::string& key, long long* batch, int* index) {
  if (key.size() != 14 || key[0] != 'b' || key[9] != '_') return false;
  long long b = 0;
  for (int i = 1; i <= 8; ++i) {
    if (key[i] < '0' || key[i] > '9') return false;
    b = b * 10 + (key[i] - '0');
  }
  int idx = 0;
  for (int i = 10; i < 14; ++i) {
    if (key[i] < '0' || key[i] > '9') return false;
    idx = idx * 10 + (key[i] - '0');
  }
  *batch = b;
  *index = idx;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: batch_crash_recover <db_dir> <sidecar> <batch_size>\n");
    return 2;
  }
  const std::string db_dir = argv[1];
  const std::string sidecar = argv[2];
  const int batch_size = std::atoi(argv[3]);

  std::vector<std::pair<long long, int>> acked;
  {
    std::FILE* f = std::fopen(sidecar.c_str(), "r");
    if (f == nullptr) {
      std::fprintf(stderr, "打不开 sidecar: %s\n", sidecar.c_str());
      return 2;
    }
    long long id = 0;
    int cnt = 0;
    while (std::fscanf(f, "%lld %d", &id, &cnt) == 2) acked.emplace_back(id, cnt);
    std::fclose(f);
  }

  DB* raws = nullptr;
  const Status os = DB::Open(Options(), db_dir, &raws);
  if (!os.ok()) {
    std::fprintf(stderr, "DB::Open failed: %s\n", os.ToString().c_str());
    return 2;
  }
  std::unique_ptr<DB> db(raws);

  // ① 已 ack 的批必须整批可见且值逐字节一致
  unsigned long long missing = 0;
  unsigned long long mismatch = 0;
  for (const auto& kv : acked) {
    for (int i = 0; i < kv.second; ++i) {
      std::string v;
      if (!db->Get(KeyOf(kv.first, i), &v).ok()) {
        ++missing;
      } else if (v != ValOf(kv.first, i)) {
        ++mismatch;
      }
    }
  }

  // ② 扫描整库：按批 id 统计可见条数（检出半批）并复核值
  std::map<long long, int> seen;
  {
    std::unique_ptr<Iterator> it(db->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      long long b = 0;
      int idx = 0;
      const std::string key = it->key().ToString();
      if (!ParseBatchKey(key, &b, &idx)) continue;
      ++seen[b];
      if (it->value().ToString() != ValOf(b, idx)) ++mismatch;
    }
  }
  unsigned long long half = 0;
  for (const auto& kv : seen) {
    if (kv.second > 0 && kv.second < batch_size) ++half;
  }

  std::printf("ROUND ACKED %llu MISSING %llu MISMATCH %llu HALF %llu BATCHES_SEEN %llu\n",
              static_cast<unsigned long long>(acked.size()), missing, mismatch, half,
              static_cast<unsigned long long>(seen.size()));
  return (missing == 0 && mismatch == 0 && half == 0) ? 0 : 1;
}
