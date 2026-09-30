// scripts/lsm_compaction_crash.cpp —— M4.3 B01/B02 的子进程驱动（真实磁盘 + raise(SIGKILL)）
// writer: 顺序 Put(sync=true) 并把已 ack 的最大 index 追加到 checkpoint；CompactionHook 在每个注入点可自杀。
// verify: 重开对账 —— OPEN_OK / MISSING / MISMATCH / REF_MISSING（引用集 ⊆ 存在集）/ 孤儿计数。
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "db_impl.h"
#include "filename.h"
#include "util/env.h"
#include "version_set.h"

using namespace lsm;

namespace {

std::string K(int i) { char b[32]; std::snprintf(b, sizeof(b), "k%08d", i); return std::string(b); }
std::string V(int i) { char b[32]; std::snprintf(b, sizeof(b), "v%08d", i); return std::string(b); }

class KillHook : public CompactionHook {
 public:
  KillHook(int point, std::string hit_file, std::string rounds_file)
      : point_(point), hit_file_(std::move(hit_file)), rounds_file_(std::move(rounds_file)) {}
  void OnInputsSelected(int, size_t) override {
    ++rounds_;
    FILE* f = std::fopen(rounds_file_.c_str(), "a");
    if (f != nullptr) {
      std::fprintf(f, "COMPACTION_ROUND %llu\n", static_cast<unsigned long long>(rounds_));
      std::fclose(f);
    }
    Maybe(0);
  }
  void OnOutputWritten(uint64_t) override { Maybe(1); }
  void OnOutputRenamed(uint64_t) override { Maybe(2); }
  void OnBeforeInstall() override { Maybe(3); }

 private:
  void Maybe(int p) {
    if (p != point_) return;
    if (killed_.exchange(true)) return;
    FILE* f = std::fopen(hit_file_.c_str(), "w");
    if (f != nullptr) {
      std::fprintf(f, "POINT_HIT %d\n", p);
      std::fclose(f);
    }
    std::fflush(nullptr);
    std::raise(SIGKILL);
  }
  const int point_;
  const std::string hit_file_;
  const std::string rounds_file_;
  std::atomic<uint64_t> rounds_{0};
  std::atomic<bool> killed_{false};
};

int RunWriter(int argc, char** argv) {
  std::string dbdir = "/tmp/lsm_crash_db";
  int point = -1;
  int keys = 4000;
  size_t wbs = 16 * 1024;
  std::string ckpt = "/tmp/lsm_crash_ckpt";
  std::string hit = "/tmp/lsm_crash_hit";
  std::string rounds_file = "/tmp/lsm_crash_rounds";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--db") && i + 1 < argc) dbdir = argv[++i];
    else if (!std::strcmp(argv[i], "--point") && i + 1 < argc) point = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) keys = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--write-buffer-size") && i + 1 < argc)
      wbs = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    else if (!std::strcmp(argv[i], "--ckpt") && i + 1 < argc) ckpt = argv[++i];
    else if (!std::strcmp(argv[i], "--hit") && i + 1 < argc) hit = argv[++i];
    else if (!std::strcmp(argv[i], "--rounds-file") && i + 1 < argc) rounds_file = argv[++i];
  }
  Options o;
  o.write_buffer_size = wbs;
  o.level0_file_num_compaction_trigger = 2;
  KillHook hook(point, hit, rounds_file);
  o.compaction_hook = &hook;
  DB* db = nullptr;
  const Status s = DB::Open(o, dbdir, &db);
  if (!s.ok()) { std::fprintf(stderr, "OPEN_FAIL %s\n", s.ToString().c_str()); return 2; }
  FILE* ck = std::fopen(ckpt.c_str(), "a");
  WriteOptions wo;
  wo.sync = false;   // 每 64 条做一次 db->Sync()，checkpoint 只记录**已 sync** 的点（缺失判据才成立）
  for (int i = 1; i <= keys; ++i) {
    if (!db->Put(wo, K(i), V(i)).ok()) break;
    if (i % 64 == 0) {
      if (!db->Sync().ok()) break;
      if (ck != nullptr) {
        std::fprintf(ck, "I %d\n", i);
        std::fflush(ck);
      }
    }
  }
  if (ck != nullptr) std::fclose(ck);
  db->Close();
  delete db;
  return 0;
}

int RunVerify(int argc, char** argv) {
  std::string dbdir = "/tmp/lsm_crash_db";
  std::string ckpt = "/tmp/lsm_crash_ckpt";
  std::string rounds_file = "/tmp/lsm_crash_rounds";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--db") && i + 1 < argc) dbdir = argv[++i];
    else if (!std::strcmp(argv[i], "--ckpt") && i + 1 < argc) ckpt = argv[++i];
    else if (!std::strcmp(argv[i], "--rounds-file") && i + 1 < argc) rounds_file = argv[++i];
  }
  int last_acked = 0;
  {
    FILE* f = std::fopen(ckpt.c_str(), "r");
    if (f != nullptr) {
      char line[128];
      while (std::fgets(line, sizeof(line), f) != nullptr) {
        int v = 0;
        if (std::sscanf(line, "I %d", &v) == 1 && v > last_acked) last_acked = v;
      }
      std::fclose(f);
    }
  }
  uint64_t rounds = 0;
  {
    FILE* f = std::fopen(rounds_file.c_str(), "r");
    if (f != nullptr) {
      char line[128];
      while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "COMPACTION_ROUND ", 17) == 0) ++rounds;
      }
      std::fclose(f);
    }
  }
  Options o;
  DB* db = nullptr;
  const Status s = DB::Open(o, dbdir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "OPEN_FAIL %s\n", s.ToString().c_str());
    std::printf("OPEN_OK 0 MISSING %d MISMATCH %d REF_MISSING %d ORPHAN_SST 0 ORPHAN_TMP 0 "
                "COMPACTION_ROUNDS %llu SST_FILES_TOTAL 0\n",
                last_acked, 0, 0, static_cast<unsigned long long>(rounds));
    return 1;
  }
  auto* impl = static_cast<PersistentDBImpl*>(db);
  int missing = 0, mismatch = 0;
  std::string v;
  for (int i = 1; i <= last_acked; ++i) {
    const Status g = db->Get(K(i), &v);
    if (!g.ok()) ++missing;
    else if (v != V(i)) ++mismatch;
  }
  // 引用集 ⊆ 存在集
  Env* env = Env::Default();
  int ref_missing = 0;
  const std::vector<uint64_t> reg = impl->registered_file_numbers();
  for (uint64_t n : reg) {
    if (!env->FileExists(TableFileName(dbdir, n))) ++ref_missing;
  }
  const RecoveryStats st = impl->GetRecoveryStats();
  const size_t sst_total = reg.size();
  std::printf("OPEN_OK 1 MISSING %d MISMATCH %d REF_MISSING %d ORPHAN_SST %llu ORPHAN_TMP %llu "
              "COMPACTION_ROUNDS %llu SST_FILES_TOTAL %zu\n",
              missing, mismatch, ref_missing,
              static_cast<unsigned long long>(st.orphan_sst_removed),
              static_cast<unsigned long long>(st.orphan_tmp_removed),
              static_cast<unsigned long long>(rounds), sst_total);
  db->Close();
  delete db;
  return (missing == 0 && mismatch == 0 && ref_missing == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: lsm_compaction_crash <writer|verify> ...\n");
    return 2;
  }
  if (!std::strcmp(argv[1], "writer")) return RunWriter(argc - 1, argv + 1);
  if (!std::strcmp(argv[1], "verify")) return RunVerify(argc - 1, argv + 1);
  std::fprintf(stderr, "unknown mode %s\n", argv[1]);
  return 2;
}
