// scripts/lsm_level_stats.cpp —— M5.3：逐文件量出 filter 块体积，追加到 AMPL 行尾
// （docs/m5-design.md §6.6「space_filter_bytes 的落点」/ §11 M5.3 的「新增」列）
//
// 为什么不让 PersistentDBImpl::FormatAmplLine 自己算：§6.6 明确要求这一列**不为它去开文件**，
// 而是由本诊断工具在逐文件打开时用 `Table::filter_bytes()` 求和，追加到 AMPL 行尾。这样
// FormatAmplLine 的既有前缀列与开销一行不变，filter 的**空间代价**可复算。
//
// 用法：
//   lsm_level_stats --db DIR [--round-id ID]
//   lsm_level_stats --db DIR --ampl-line "$(lsm_ampl_probe ... | grep '^AMPL')"
//
// 输出（固定行）：
//   SPACE_SST_FILES <n> SPACE_SST_BYTES <b> SPACE_FILTER_BYTES <f> SPACE_SST_DATA_BYTES <d>
//   SPACE_FILTER_FILES_OK <k> SPACE_FILTER_FILES_ABSENT <a> SPACE_FILTER_FILES_CORRUPT <c>
// 若给了 --ampl-line，则只打印「原行 + space_filter_bytes=<f> space_sst_data_bytes=<d>」。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sstable/table.h"
#include "util/env.h"
#include "version_set.h"

using namespace lsm;

namespace {

int Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --db DIR [--round-id ID] [--ampl-line \"AMPL ...\"]\n",
               argv0);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dbdir;
  std::string round_id = "ALL";
  std::string ampl_line;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
      dbdir = argv[++i];
    } else if (std::strcmp(argv[i], "--round-id") == 0 && i + 1 < argc) {
      round_id = argv[++i];
    } else if (std::strcmp(argv[i], "--ampl-line") == 0 && i + 1 < argc) {
      ampl_line = argv[++i];
    } else {
      return Usage(argv[0]);
    }
  }
  if (dbdir.empty()) return Usage(argv[0]);

  Env* env = Env::Default();
  Options o;
  std::vector<std::string> children;
  const Status ls = env->GetChildren(dbdir, &children);
  if (!ls.ok()) {
    std::fprintf(stderr, "GetChildren(%s) failed\n", dbdir.c_str());
    return 2;
  }

  uint64_t files = 0, sst_bytes = 0, filter_bytes = 0;
  uint64_t ok_files = 0, absent_files = 0, corrupt_files = 0;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (!ParseTableFileName(c, &number)) continue;
    const std::string path = dbdir + "/" + c;
    uint64_t size = 0;
    if (!env->GetFileSize(path, &size).ok()) continue;
    std::shared_ptr<Table> t;
    const Status s = Table::Open(o, env, path, &t);
    if (!s.ok() || t == nullptr) {
      // 打不开的文件只计入 sst_bytes，不臆造成 filter 体积（§6.6：不得用公式估算冒充）。
      ++files;
      sst_bytes += size;
      ++corrupt_files;
      continue;
    }
    ++files;
    sst_bytes += size;
    filter_bytes += t->filter_bytes();
    switch (t->filter_state()) {
      case Table::FilterState::kOk: ++ok_files; break;
      case Table::FilterState::kAbsent: ++absent_files; break;
      case Table::FilterState::kCorrupt: ++corrupt_files; break;
    }
  }
  const uint64_t sst_data_bytes = sst_bytes >= filter_bytes ? sst_bytes - filter_bytes : 0;

  if (!ampl_line.empty()) {
    std::printf("%s space_filter_bytes=%llu space_sst_data_bytes=%llu\n", ampl_line.c_str(),
                static_cast<unsigned long long>(filter_bytes),
                static_cast<unsigned long long>(sst_data_bytes));
  } else {
    std::printf("SPACE round_id=%s SPACE_SST_FILES %llu SPACE_SST_BYTES %llu SPACE_FILTER_BYTES %llu "
                "SPACE_SST_DATA_BYTES %llu SPACE_FILTER_FILES_OK %llu SPACE_FILTER_FILES_ABSENT %llu "
                "SPACE_FILTER_FILES_CORRUPT %llu\n",
                round_id.c_str(), static_cast<unsigned long long>(files),
                static_cast<unsigned long long>(sst_bytes),
                static_cast<unsigned long long>(filter_bytes),
                static_cast<unsigned long long>(sst_data_bytes),
                static_cast<unsigned long long>(ok_files),
                static_cast<unsigned long long>(absent_files),
                static_cast<unsigned long long>(corrupt_files));
  }
  std::fflush(stdout);
  return 0;
}
