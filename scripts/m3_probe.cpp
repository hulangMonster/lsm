// scripts/m3_probe.cpp —— M3.3 四个门禁脚本共用的探针（docs/m3-design.md §10.2 的 B01/B03/B04/B05）
//
// 为什么把这些探针放在一个可执行文件里：四条腿共用同一套 Options/恢复/统计口径，
// 分散成 5 个二进制会让"口径漂移"无处不被发现。子命令：
//   flush-writer  <db> <sidecar> <write_buffer_size>
//       与 crash_writer 同一协议（先 Put(sync) 再 fsync sidecar），但用**小 write_buffer_size**
//       迫使每轮都发生 flush + 轮转；脚本在随机时刻 kill -9。
//   flush-recover <db> <sidecar> <write_buffer_size>
//       对账 sidecar；然后补写 filler 逼出 flush/轮转/回收，并打印一行汇总：
//       ROUND 1 ACKED a RECOVERED r MISSING m MISMATCH mm SST_FILES_TOTAL s
//       LOGS_DELETED_TOTAL d RECORDS_REPLAYED rp
//   restart       <db> <write_buffer_size> <n_puts>
//       写 n 条 + 强制把最后一个 memtable 也落盘 + Close + 重开；打印 RECORDS_REPLAYED <n>。
//   damage        <db> <cases>
//       建库后对唯一的 .sst 逐字节翻转，每次重开并验证全部 key；
//       打印 SST_DAMAGE_CASES <n> 与 SILENT_WRONG <k>（k 必须为 0）。
//   fd-leak       <db> <rounds>
//       多轮 flush + Get 后比较 /proc/self/fd 计数；打印 FD_GROWTH <delta>。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "db.h"
#include "db_impl.h"
#include "filename.h"
#include "util/env.h"

using namespace lsm;

namespace {

std::string Key(int i) {
  char b[32];
  std::snprintf(b, sizeof(b), "k%08d", i);
  return std::string(b);
}
std::string Val(int i) {
  char b[40];
  std::snprintf(b, sizeof(b), "v%08d", i);
  return std::string(b);
}

WriteOptions SyncOptions() {
  WriteOptions wo;
  wo.sync = true;
  return wo;
}

bool AppendLineAndSync(int fd, const std::string& line) {
  size_t off = 0;
  while (off < line.size()) {
    const ssize_t n = ::write(fd, line.data() + off, line.size() - off);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return ::fsync(fd) == 0;
}

Options MakeOptions(size_t write_buffer_size) {
  Options o;
  o.write_buffer_size = write_buffer_size;
  o.recycle_log_files = true;
  return o;
}

uint64_t CountSstFiles(Env* env, const std::string& dir) {
  std::vector<std::string> children;
  if (!env->GetChildren(dir, &children).ok()) return 0;
  uint64_t n = 0;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (ParseTableFileName(c, &number)) ++n;
  }
  return n;
}

bool WaitFlushIdle(PersistentDBImpl* impl, uint64_t min_completed, int spins) {
  for (int i = 0; i < spins; ++i) {
    const FlushStats fs = impl->GetFlushStats();
    if (fs.flushes_failed == 0 && fs.flushes_completed >= min_completed &&
        impl->immutables_size() == 0) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

int OpenOrFail(const Options& o, const std::string& dir, DB** db) {
  const Status s = DB::Open(o, dir, db);
  if (!s.ok()) {
    std::fprintf(stderr, "DB::Open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  return 0;
}

int cmd_flush_writer(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: m3_probe flush-writer <db> <sidecar> <write_buffer_size>\n");
    return 2;
  }
  const std::string db_dir = argv[2];
  const std::string sidecar = argv[3];
  const size_t wbs = static_cast<size_t>(std::strtoull(argv[4], nullptr, 10));
  const int ack_fd = ::open(sidecar.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (ack_fd < 0) {
    std::perror("open sidecar");
    return 2;
  }
  DB* db = nullptr;
  if (OpenOrFail(MakeOptions(wbs), db_dir, &db) != 0) return 2;
  WriteOptions wo;
  wo.sync = true;   // 门禁口径：每条都必须 durable-before-ack
  for (long long i = 1;; ++i) {
    const std::string k = Key(static_cast<int>(i));
    const std::string v = Val(static_cast<int>(i));
    const Status s = db->Put(wo, Slice(k), Slice(v));
    if (!s.ok()) {
      std::fprintf(stderr, "Put(%s) failed: %s\n", k.c_str(), s.ToString().c_str());
      return 1;
    }
    const std::string line = k + " " + v + "\n";
    if (!AppendLineAndSync(ack_fd, line)) {
      std::fprintf(stderr, "sidecar append failed\n");
      return 1;
    }
  }
}

int cmd_flush_recover(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: m3_probe flush-recover <db> <sidecar> <write_buffer_size>\n");
    return 2;
  }
  const std::string db_dir = argv[2];
  const std::string sidecar = argv[3];
  const size_t wbs = static_cast<size_t>(std::strtoull(argv[4], nullptr, 10));
  const Options o = MakeOptions(wbs);
  DB* db = nullptr;
  if (OpenOrFail(o, db_dir, &db) != 0) {
    std::printf(
        "ROUND 1 ACKED 0 RECOVERED 0 MISSING 1 MISMATCH 0 SST_FILES_TOTAL 0 "
        "LOGS_DELETED_TOTAL 0 RECORDS_REPLAYED 0\n");
    return 1;
  }
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const RecoveryStats rst = impl->GetRecoveryStats();

  long long acked = 0, recovered = 0, missing = 0, mismatch = 0;
  std::ifstream in(sidecar);
  std::string key, val;
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

  // 补写 filler：逼迫 flush + 轮转 + WAL 回收真的发生（否则 LOGS_DELETED_TOTAL 会是空绿的 0）。
  uint64_t deleted = rst.obsolete_logs_removed + impl->GetFlushStats().log_files_deleted;
  for (int i = 1; i <= 40000 && deleted == 0; ++i) {
    if (!db->Put(WriteOptions(), Key(1000000 + i), Val(i)).ok()) break;
    if ((i % 500) == 0) {
      const FlushStats fs = impl->GetFlushStats();
      deleted = rst.obsolete_logs_removed + fs.log_files_deleted;
    }
  }
  WaitFlushIdle(impl, impl->GetFlushStats().flushes_completed, 4000000);
  const FlushStats fs = impl->GetFlushStats();
  deleted = rst.obsolete_logs_removed + fs.log_files_deleted;
  Env* env = Env::Default();
  const uint64_t sst = CountSstFiles(env, db_dir);
  std::printf(
      "ROUND 1 ACKED %lld RECOVERED %lld MISSING %lld MISMATCH %lld SST_FILES_TOTAL %llu "
      "LOGS_DELETED_TOTAL %llu RECORDS_REPLAYED %llu\n",
      acked, recovered, missing, mismatch, static_cast<unsigned long long>(sst),
      static_cast<unsigned long long>(deleted),
      static_cast<unsigned long long>(rst.records_replayed));
  db->Close();
  delete db;
  return (missing == 0 && mismatch == 0) ? 0 : 1;
}

int cmd_restart(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: m3_probe restart <db> <write_buffer_size> <n_puts>\n");
    return 2;
  }
  const std::string db_dir = argv[2];
  const size_t wbs = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
  const int n = std::atoi(argv[4]);
  const Options o = MakeOptions(wbs);
  {
    DB* db = nullptr;
    if (OpenOrFail(o, db_dir, &db) != 0) return 2;
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= n; ++i) {
      const Status s = db->Put(SyncOptions(), Key(i), Val(i));
      if (!s.ok()) {
        std::fprintf(stderr, "Put failed: %s\n", s.ToString().c_str());
        return 1;
      }
    }
    // 把"最后一个 memtable"也落盘，并轮转出空的当前 log（B03 的强证据前提）。
    const Status f = impl->ForceFlushForTest();
    if (!f.ok()) {
      std::fprintf(stderr, "ForceFlushForTest failed: %s\n", f.ToString().c_str());
      return 1;
    }
    const Status c = db->Close();
    if (!c.ok()) {
      std::fprintf(stderr, "Close failed: %s\n", c.ToString().c_str());
      return 1;
    }
    delete db;
  }
  DB* db2 = nullptr;
  if (OpenOrFail(o, db_dir, &db2) != 0) return 2;
  PersistentDBImpl* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats st = impl2->GetRecoveryStats();
  long long readable = 0;
  bool bad = false;
  for (int i = 1; i <= n; ++i) {
    std::string got;
    const Status s = db2->Get(Key(i), &got);
    if (!s.ok() || got != Val(i)) {
      bad = true;
      break;
    }
    ++readable;
  }
  std::printf("RECORDS_REPLAYED %llu RESTART_KEYS_OK %lld/%d SST_FILES_REGISTERED %llu\n",
              static_cast<unsigned long long>(st.records_replayed), readable, n,
              static_cast<unsigned long long>(st.sst_files_registered));
  db2->Close();
  delete db2;
  if (bad || readable != n) return 1;
  return st.records_replayed == 0 ? 0 : 1;
}

// ---- B04：单字节翻转扫描（真实文件）----
bool ReadFileBytes(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}
bool WriteFileBytes(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

int cmd_damage(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: m3_probe damage <db> <cases>\n");
    return 2;
  }
  const std::string db_dir = argv[2];
  const int cases_cap = std::atoi(argv[3]);
  Env* env = Env::Default();
  const int kKeys = 40;
  // 建库：小 buffer + 强制落盘，产出至少一个已注册 .sst。
  {
    Options o;
    o.write_buffer_size = 8 * 1024;
    DB* db = nullptr;
    if (OpenOrFail(o, db_dir, &db) != 0) return 2;
    PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
    for (int i = 1; i <= kKeys; ++i) {
      const Status s = db->Put(SyncOptions(), Key(i), Val(i));
      if (!s.ok()) return 1;
    }
    if (!impl->ForceFlushForTest().ok()) return 1;
    db->Close();
    delete db;
  }
  std::vector<std::string> children;
  if (!env->GetChildren(db_dir, &children).ok()) return 1;
  std::string sst_path;
  for (const std::string& c : children) {
    uint64_t number = 0;
    if (ParseTableFileName(c, &number)) {
      sst_path = db_dir + "/" + c;   // 单文件（本用例只写一个 memtable）
      break;
    }
  }
  if (sst_path.empty()) {
    std::fprintf(stderr, "no .sst produced\n");
    return 1;
  }
  std::string original;
  if (!ReadFileBytes(sst_path, &original)) return 1;

  const int cap = cases_cap > 0 ? cases_cap : 1;
  const size_t stride = original.size() > static_cast<size_t>(cap)
                            ? original.size() / static_cast<size_t>(cap)
                            : 1;
  long long cases = 0;
  long long silent_wrong = 0;
  for (size_t off = 0; off < original.size() && cases < cap; off += stride) {
    std::string mutated = original;
    mutated[off] = static_cast<char>(mutated[off] ^ 0x5a);
    if (!WriteFileBytes(sst_path, mutated)) return 1;
    ++cases;
    Options o;
    o.write_buffer_size = 8 * 1024;
    DB* db = nullptr;
    const Status os = DB::Open(o, db_dir, &db);
    if (!os.ok()) {
      // Open 检出（footer/索引/metadata 扫描）—— 计为检出
    } else {
      bool detected = false;
      for (int i = 1; i <= kKeys && !detected; ++i) {
        std::string got;
        const Status gs = db->Get(Key(i), &got);
        if (gs.IsCorruption()) {
          detected = true;
        } else if (gs.ok() && got != Val(i)) {
          ++silent_wrong;   // 静默错值：最危险的失效模式
          detected = true;
        }
      }
      db->Close();
      delete db;
    }
    if (!WriteFileBytes(sst_path, original)) return 1;   // 复原，进入下一个字节
  }
  std::printf("SST_DAMAGE_CASES %lld SILENT_WRONG %lld\n", cases, silent_wrong);
  return (cases > 0 && silent_wrong == 0) ? 0 : 1;
}

// ---- B05：fd 计数 ----
int CountOpenFds() {
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) return -1;
  int n = 0;
  while (::readdir(d) != nullptr) ++n;
  ::closedir(d);
  return n;
}

int cmd_fd_leak(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: m3_probe fd-leak <db> <rounds>\n");
    return 2;
  }
  const std::string db_dir = argv[2];
  const int rounds = std::atoi(argv[3]);
  Options o;
  o.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  if (OpenOrFail(o, db_dir, &db) != 0) return 2;
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);
  const int baseline = CountOpenFds();
  if (baseline < 0) {
    std::fprintf(stderr, "/proc/self/fd not readable\n");
    return 2;
  }
  uint64_t completed = impl->GetFlushStats().flushes_completed;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < 400; ++i) {
      const Status s = db->Put(WriteOptions(), Key(r * 1000 + i), Val(i));
      if (!s.ok()) {
        std::fprintf(stderr, "Put failed: %s\n", s.ToString().c_str());
        return 1;
      }
    }
    // M4.3 判据修正：旧写法在 Put 之后取 completed+1，若本轮 flush 已完成，目标就变成
    // "还要再来一次 flush"，而下一轮 Put 之前不会再有写 => 谓词不可达（自旋挂死）。
    // 改为等"当前计数 + immutables_==0"（真正的 flush 空闲），不削弱句柄泄漏判据。
    completed = impl->GetFlushStats().flushes_completed;
    WaitFlushIdle(impl, completed, 2000000);
    for (int i = 0; i < 400; ++i) {
      std::string got;
      db->Get(Key(r * 1000 + i), &got);
    }
  }
  const int after = CountOpenFds();
  std::printf("FD_GROWTH %d FD_BASELINE %d FLUSHES_COMPLETED %llu\n", after - baseline, baseline,
              static_cast<unsigned long long>(impl->GetFlushStats().flushes_completed));
  db->Close();
  delete db;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: m3_probe <flush-writer|flush-recover|restart|damage|fd-leak> ...\n");
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "flush-writer") return cmd_flush_writer(argc, argv);
  if (cmd == "flush-recover") return cmd_flush_recover(argc, argv);
  if (cmd == "restart") return cmd_restart(argc, argv);
  if (cmd == "damage") return cmd_damage(argc, argv);
  if (cmd == "fd-leak") return cmd_fd_leak(argc, argv);
  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 2;
}
