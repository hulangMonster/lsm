// bench/bench_lsm.cpp —— M5.3 微基准驱动（docs/m5-design.md §6 / §11 M5.3）
//
// 契约来源（逐条对应，禁止走样）：
//   docs/m5-design.md §6.1（四类负载定义）、§6.2（对照对象与 durability 口径）、
//   §6.3（参数矩阵与固定 CELL 行）、§6.4（预热/重复/中位数/raw1..raw3）、
//   §6.5（同轮交替与不可外推）、§6.6（数据表列）、§7.1（只 include db.h/common.h，只走公共 DB 接口）。
//
// 本进程只做一件事：按 (load, engine) 交替执行所有格子，把**固定格式的 CELL 行**打到 stdout；
// 头部（MACHINE/PARAMS/BUILD/FSYNC_BASELINE）、对账门禁、退出码、BENCH_* 收尾标记由
// `scripts/bench_lsm.sh` 负责（§7.1/§7.2/§7.3）。
//
// 硬门禁范围（M5-C6）：只有 engine=lsm 的行参与 missing/mismatch 对账；raw_file/raw_pwrite/
// std_map 一律 verify=na missing=na mismatch=na，不参与退出码。
//
// 不可外推（逐字写死，§6.5）：
//   1) 单机、单块 ext4、VM、loopback 级场景；
//   2) sync=false 的数字只说明「进程级一致性」，不证明掉电安全；
//   3) 具体 N / value_size / batch / pipeline 下才成立；
//   4) 不承诺达到任何外部系统的绝对吞吐；
//   5) `>=3x` 只针对「不存在 key 的数据块读次数」（由 M5-A10 的同轮开关对照承担，不在本文件）。
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "db.h"
#include "write_batch.h"

using namespace lsm;

namespace {

// ---------------------------------------------------------------------------
// 参数（docs/m5-design.md §6.3；未列出的 --dir/--write-buffer-size/--repro-tol
// 是本实现的驱动参数，由 bench_lsm.sh 传入并逐项登记在 PARAMS 头行）。
// ---------------------------------------------------------------------------
struct Params {
  uint64_t dataset = 100000;
  size_t value_size = 100;
  std::string key_dist = "seq";
  size_t batch = 1;
  int pipeline = 1;
  int sync = 0;
  int repeats = 3;
  uint64_t warmup = 10000;
  bool filter_on = true;
  std::string engines = "all";
  uint64_t seed = 0x5EED2025ull;
  bool inject_missing = false;
  bool quick = false;
  size_t write_buffer_size = 4u * 1024 * 1024;
  double repro_thr = 0.25;
  double repro_p99 = 0.50;
  std::string dir = "/tmp/lsm_bench_run";
};

uint64_t NowUs() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

// POSIX 的 write/pread/pwrite/ftruncate/fsync/fdatasync 带 warn_unused_result；
// 基准的原始 IO 档允许失败（它不是被测语义），这里显式消费返回值以满足 0 warning 门禁。
template <typename T>
inline void Consume(T) {}

// 固定种子的 xorshift64：跨进程/跨轮可复现（§6.4 的「固定 RNG」要求）。
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

std::string KeyFromIndex(uint64_t i) {
  char b[8];
  for (int k = 0; k < 8; ++k) b[k] = static_cast<char>((i >> (8 * (7 - k))) & 0xffu);
  return std::string(b, 8);
}

std::string RandomKey(Rng* rng) { return KeyFromIndex(rng->Next()); }

std::string MakeValue(uint64_t i, size_t n) {
  std::string v(n, 'v');
  for (int k = 0; k < 8 && static_cast<size_t>(k) < n; ++k) {
    v[static_cast<size_t>(k)] = static_cast<char>((i >> (8 * (7 - k))) & 0xffu);
  }
  return v;
}

// permille ∈ [0,1000]：P99 用 990、P99.9 用 999（**不是**把 999 当百分比，那会越界读）。
uint64_t PercentilePermille(std::vector<uint64_t>* v, int permille) {
  if (v->empty()) return 0;
  const size_t idx = static_cast<size_t>(permille) * (v->size() - 1) / 1000;
  return (*v)[idx];
}

double MedOf(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// 通用并行执行：calls 个「调用」，P 个线程，每个调用把耗时（us）追加进本线程的向量。
template <typename Fn>
std::vector<uint64_t> RunParallel(size_t calls, int threads, Fn fn) {
  const int nthreads = threads < 1 ? 1 : threads;
  std::vector<std::vector<uint64_t>> per(static_cast<size_t>(nthreads));
  if (nthreads == 1) {
    for (size_t i = 0; i < calls; ++i) fn(i, &per[0]);
  } else {
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(nthreads));
    for (int t = 0; t < nthreads; ++t) {
      ts.emplace_back([&, t]() {
        size_t i;
        while ((i = next.fetch_add(1)) < calls) fn(i, &per[static_cast<size_t>(t)]);
      });
    }
    for (std::thread& th : ts) th.join();
  }
  std::vector<uint64_t> out;
  for (std::vector<uint64_t>& v : per) out.insert(out.end(), v.begin(), v.end());
  std::sort(out.begin(), out.end());
  return out;
}

double Load1() {
  FILE* f = std::fopen("/proc/loadavg", "r");
  if (f == nullptr) return 0.0;
  double l1 = 0.0;
  if (std::fscanf(f, "%lf", &l1) != 1) l1 = 0.0;
  std::fclose(f);
  return l1;
}

int Nproc() {
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<int>(n) : 1;
}

// ---------------------------------------------------------------------------
// 单格结果（每次 repeat 的吞吐/延迟 + 对账计数）
// ---------------------------------------------------------------------------
struct CellOutcome {
  std::vector<double> throughput;        // 每次 repeat 的 ops/s
  std::vector<uint64_t> lat_p50;         // 每次 repeat 的中位延迟 us
  std::vector<uint64_t> lat_p99;         // 每次 repeat 的 P99 us
  std::vector<uint64_t> lat_all;         // 所有 repeat 的调用耗时池（P50/P99/P999/min/max）
  uint64_t missing = 0;
  uint64_t mismatch = 0;
};

bool ReproOk(const CellOutcome& o, const Params& p) {
  if (p.repeats < 2 || o.throughput.size() < 2) return true;
  double tmin = o.throughput[0], tmax = o.throughput[0];
  uint64_t pmin = o.lat_p99[0], pmax = o.lat_p99[0];
  for (double t : o.throughput) {
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  for (uint64_t v : o.lat_p99) {
    pmin = std::min(pmin, v);
    pmax = std::max(pmax, v);
  }
  const double thr_rel = tmax > 0 ? (tmax - tmin) / tmax : 0.0;
  const double p99_rel = pmax > 0 ? static_cast<double>(pmax - pmin) / static_cast<double>(pmax) : 0.0;
  return thr_rel <= p.repro_thr && p99_rel <= p.repro_p99;
}

void PrintCell(const Params& p, const char* load, const char* engine, const CellOutcome& o,
               const std::string& verify, const std::string& missing, const std::string& mismatch,
               const std::string& durability, const std::string& notes, const char* latency_unit) {
  const double thr = MedOf(o.throughput);
  std::vector<uint64_t> lat = o.lat_all;
  std::sort(lat.begin(), lat.end());
  const uint64_t p50 = PercentilePermille(&lat, 500);
  const uint64_t p99 = PercentilePermille(&lat, 990);
  const uint64_t p999 = PercentilePermille(&lat, 999);
  const uint64_t mn = lat.empty() ? 0 : lat.front();
  const uint64_t mx = lat.empty() ? 0 : lat.back();
  const uint64_t raw1 = o.lat_p50.size() > 0 ? o.lat_p50[0] : 0;
  const uint64_t raw2 = o.lat_p50.size() > 1 ? o.lat_p50[1] : 0;
  const uint64_t raw3 = o.lat_p50.size() > 2 ? o.lat_p50[2] : 0;
  const int repro = ReproOk(o, p) ? 1 : 0;
  std::printf(
      "CELL name=%s engine=%s dataset=%llu value_size=%zu key_dist=%s batch=%zu pipeline=%d "
      "sync=%d filter=%d repeat=%d THROUGHPUT=%.3f LATENCY=%llu P99=%llu p999_us=%llu "
      "min_us=%llu max_us=%llu latency_unit=%s verify=%s missing=%s mismatch=%s "
      "durability=%s raw1_us=%llu raw2_us=%llu raw3_us=%llu repro_ok=%d notes=%s\n",
      load, engine, static_cast<unsigned long long>(p.dataset), p.value_size, p.key_dist.c_str(),
      p.batch, p.pipeline, p.sync, p.filter_on ? 1 : 0, p.repeats, thr,
      static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p99),
      static_cast<unsigned long long>(p999), static_cast<unsigned long long>(mn),
      static_cast<unsigned long long>(mx), latency_unit, verify.c_str(),
      missing.c_str(), mismatch.c_str(), durability.c_str(),
      static_cast<unsigned long long>(raw1), static_cast<unsigned long long>(raw2),
      static_cast<unsigned long long>(raw3), repro,
      notes.empty() ? "NONE" : notes.c_str());
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 数据集
// ---------------------------------------------------------------------------
struct Dataset {
  std::vector<std::string> keys;        // 按 key_dist 生成
  std::vector<std::string> values;      // values[i] 对应 keys[i]
  std::vector<uint32_t> order;          // 固定种子的随机访问顺序（rand_* 用）
  size_t record_size = 0;               // key.size() + value.size()
};

void BuildDataset(const Params& p, Dataset* d) {
  Rng rng(p.seed);
  d->keys.reserve(static_cast<size_t>(p.dataset));
  for (uint64_t i = 0; i < p.dataset; ++i) {
    d->keys.push_back(p.key_dist == "uniform" ? RandomKey(&rng) : KeyFromIndex(i));
    d->values.push_back(MakeValue(i, p.value_size));
  }
  if (!d->keys.empty()) d->record_size = d->keys[0].size() + d->values[0].size();
  d->order.resize(d->keys.size());
  for (size_t i = 0; i < d->order.size(); ++i) d->order[i] = static_cast<uint32_t>(i);
  Rng shuffle_rng(p.seed ^ 0x5bd1e995ull);
  for (size_t i = d->order.size(); i > 1; --i) {
    const size_t j = static_cast<size_t>(shuffle_rng.Next() % i);
    std::swap(d->order[i - 1], d->order[j]);
  }
}

bool EnsureDir(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

// §6.4：每格前重读 loadavg；load1 > nproc ⇒ 该格标 UNRELIABLE(load)（不静默）。
std::string NotesFor(const Params& p) {
  const double l1 = Load1();
  const bool unreliable = l1 > static_cast<double>(Nproc());
  std::string notes;
  if (p.quick) notes = "QUICK";
  if (unreliable) {
    if (!notes.empty()) notes += ",";
    notes += "UNRELIABLE(load)";
  }
  return notes;
}

// ---------------------------------------------------------------------------
// LSM 格子
// ---------------------------------------------------------------------------
Options LsmOptions(const Params& p) {
  Options o;
  o.write_buffer_size = p.write_buffer_size;
  o.bloom_bits = p.filter_on ? 10 : 0;
  return o;
}

enum class Load { kSeqWrite, kRandWrite, kRandRead, kSeqRead };

const char* LoadName(Load l) {
  switch (l) {
    case Load::kSeqWrite: return "seq_write";
    case Load::kRandWrite: return "rand_write";
    case Load::kRandRead: return "rand_read";
    case Load::kSeqRead: return "seq_read";
  }
  return "?";
}

// 一次 LSM 调用：返回该次调用的耗时（us）。batch>1 时一个调用 = 一个 WriteBatch（K 条 entry）。
uint64_t LsmCall(DB* db, const Params& p, Load load, const Dataset& d, size_t call) {
  const WriteOptions wo{p.sync != 0};
  const uint64_t t0 = NowUs();
  if (load == Load::kSeqWrite || load == Load::kRandWrite) {
    if (p.batch <= 1) {
      const size_t idx = (load == Load::kSeqWrite) ? call : d.order[call];
      db->Put(wo, d.keys[idx], d.values[idx]);
    } else {
      WriteBatch b;
      for (size_t k = 0; k < p.batch; ++k) {
        const size_t pos = call * p.batch + k;
        if (pos >= d.keys.size()) break;
        const size_t idx = (load == Load::kSeqWrite) ? pos : d.order[pos];
        b.Put(d.keys[idx], d.values[idx]);
      }
      db->Write(wo, &b);
    }
  } else {
    std::string v;
    const size_t idx = (load == Load::kSeqRead) ? call : d.order[call];
    db->Get(d.keys[idx], &v);
  }
  return NowUs() - t0;
}

CellOutcome RunLsmCell(const Params& p, Load load, const Dataset& d) {
  CellOutcome out;
  const bool is_write = load == Load::kSeqWrite || load == Load::kRandWrite;
  // 只有**写**负载按 batch 成组；读负载恒为「一次 Get 一个 op」（batch 对读无意义）。
  const size_t calls = (!is_write || p.batch <= 1)
                           ? d.keys.size()
                           : (d.keys.size() + p.batch - 1) / p.batch;
  const uint64_t entries = static_cast<uint64_t>(d.keys.size());

  const std::string dir = p.dir + "/" + LoadName(load) + "_lsm";
  if (!EnsureDir(p.dir)) {
    std::fprintf(stderr, "mkdir %s failed\n", p.dir.c_str());
    std::exit(2);
  }
  Options o = LsmOptions(p);
  DB* db = nullptr;
  if (!DB::Open(o, dir, &db).ok()) {
    std::fprintf(stderr, "OPEN_FAIL %s\n", dir.c_str());
    std::exit(1);
  }

  // 读负载：计时窗口外先把数据集写好（§6.1：LSM 数据集必须在计时前完成写入）。
  if (!is_write) {
    for (size_t i = 0; i < d.keys.size(); ++i) {
      if (!db->Put(WriteOptions(), d.keys[i], d.values[i]).ok()) {
        std::fprintf(stderr, "SETUP_PUT_FAIL\n");
        std::exit(1);
      }
    }
    db->Sync();
  }

  for (int r = 0; r < p.repeats; ++r) {
    // 预热（§6.4）：窗口外丢弃。
    for (uint64_t w = 0; w < p.warmup; ++w) {
      const size_t idx = static_cast<size_t>(w % calls);
      LsmCall(db, p, load, d, idx);
    }
    const uint64_t t0 = NowUs();
    std::vector<uint64_t> lat = RunParallel(calls, p.pipeline, [&](size_t i, std::vector<uint64_t>* sink) {
      sink->push_back(LsmCall(db, p, load, d, i));
    });
    const uint64_t dt = NowUs() - t0;
    const double secs = dt > 0 ? static_cast<double>(dt) / 1e6 : 1e-6;
    out.throughput.push_back(static_cast<double>(entries) / secs);
    std::vector<uint64_t> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    out.lat_p50.push_back(PercentilePermille(&sorted, 500));
    out.lat_p99.push_back(PercentilePermille(&sorted, 990));
    out.lat_all.insert(out.lat_all.end(), lat.begin(), lat.end());
  }

  // 对账（只在 LSM 格；窗口外）：每个写入/读取的 key 必须逐字节读回相等。
  for (size_t i = 0; i < d.keys.size(); ++i) {
    std::string v;
    const Status s = db->Get(d.keys[i], &v);
    if (!s.ok()) {
      ++out.missing;
    } else if (v != d.values[i]) {
      ++out.mismatch;
    }
  }
  db->Close();
  delete db;
  return out;
}

// ---------------------------------------------------------------------------
// raw_file：seq_write = append+fsync；seq_read = 顺序 pread
// ---------------------------------------------------------------------------
CellOutcome RunRawFileCell(const Params& p, Load load, const Dataset& d) {
  CellOutcome out;
  const bool is_write = load == Load::kSeqWrite;
  const std::string dir = p.dir + "/raw_file_" + std::string(LoadName(load));
  if (!EnsureDir(p.dir) || !EnsureDir(dir)) {
    std::fprintf(stderr, "mkdir %s failed\n", dir.c_str());
    std::exit(2);
  }
  const std::string path = dir + "/raw.dat";
  std::vector<std::string> recs(d.keys.size());
  for (size_t i = 0; i < recs.size(); ++i) recs[i] = d.keys[i] + d.values[i];

  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | (is_write ? O_APPEND : 0), 0644);
  if (fd < 0) {
    std::fprintf(stderr, "open %s failed\n", path.c_str());
    std::exit(2);
  }
  std::vector<uint64_t> offs(d.keys.size() + 1, 0);
  if (!is_write) {
    // 读负载：窗口外顺序写好数据集并记偏移。
    uint64_t off = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
      if (::pwrite(fd, recs[i].data(), recs[i].size(), off) != static_cast<ssize_t>(recs[i].size())) {
        std::fprintf(stderr, "pwrite setup failed\n");
        std::exit(2);
      }
      offs[i] = off;
      off += recs[i].size();
    }
    offs[recs.size()] = off;
    ::fsync(fd);
  }

  for (int r = 0; r < p.repeats; ++r) {
    for (uint64_t w = 0; w < p.warmup; ++w) {
      const size_t i = static_cast<size_t>(w % d.keys.size());
      if (is_write) {
        Consume(::write(fd, recs[i].data(), recs[i].size()));
      } else {
        std::vector<char> buf(recs[i].size());
        Consume(::pread(fd, buf.data(), buf.size(), offs[i]));
      }
    }
    if (is_write) Consume(::ftruncate(fd, 0));
    const uint64_t t0 = NowUs();
    std::vector<uint64_t> lat =
        RunParallel(d.keys.size(), p.pipeline, [&](size_t i, std::vector<uint64_t>* sink) {
          const uint64_t a = NowUs();
          const std::string& rec = recs[i];
          if (is_write) {
            Consume(::write(fd, rec.data(), rec.size()));
            if (p.sync != 0) Consume(::fsync(fd));
          } else {
            std::vector<char> buf(rec.size());
            Consume(::pread(fd, buf.data(), buf.size(), offs[i]));
          }
          sink->push_back(NowUs() - a);
        });
    const uint64_t dt = NowUs() - t0;
    const double secs = dt > 0 ? static_cast<double>(dt) / 1e6 : 1e-6;
    out.throughput.push_back(static_cast<double>(d.keys.size()) / secs);
    std::vector<uint64_t> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    out.lat_p50.push_back(PercentilePermille(&sorted, 500));
    out.lat_p99.push_back(PercentilePermille(&sorted, 990));
    out.lat_all.insert(out.lat_all.end(), lat.begin(), lat.end());
  }
  ::close(fd);
  return out;
}

// ---------------------------------------------------------------------------
// raw_pwrite：rand_write = 随机偏移 pwrite + fdatasync；rand_read = 随机偏移 pread
// ---------------------------------------------------------------------------
CellOutcome RunRawPwriteCell(const Params& p, Load load, const Dataset& d) {
  CellOutcome out;
  const bool is_write = load == Load::kRandWrite;
  const std::string dir = p.dir + "/raw_pwrite_" + std::string(LoadName(load));
  if (!EnsureDir(p.dir) || !EnsureDir(dir)) {
    std::fprintf(stderr, "mkdir %s failed\n", dir.c_str());
    std::exit(2);
  }
  const std::string path = dir + "/raw.dat";
  std::vector<std::string> recs(d.keys.size());
  for (size_t i = 0; i < recs.size(); ++i) recs[i] = d.keys[i] + d.values[i];

  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    std::fprintf(stderr, "open %s failed\n", path.c_str());
    std::exit(2);
  }
  const off_t total = static_cast<off_t>(recs.size() * d.record_size);
  if (::posix_fallocate(fd, 0, total) != 0) {
    std::fprintf(stderr, "posix_fallocate failed\n");
    ::close(fd);
    std::exit(2);
  }
  if (!is_write) {
    for (size_t i = 0; i < recs.size(); ++i) {
      const off_t off = static_cast<off_t>(i * d.record_size);
      if (::pwrite(fd, recs[i].data(), recs[i].size(), off) != static_cast<ssize_t>(recs[i].size())) {
        std::fprintf(stderr, "pwrite setup failed\n");
        std::exit(2);
      }
    }
    ::fsync(fd);
  }

  for (int r = 0; r < p.repeats; ++r) {
    for (uint64_t w = 0; w < p.warmup; ++w) {
      const size_t i = d.order[static_cast<size_t>(w % d.order.size())];
      const off_t off = static_cast<off_t>(i * d.record_size);
      if (is_write) {
        Consume(::pwrite(fd, recs[i].data(), recs[i].size(), off));
      } else {
        std::vector<char> buf(recs[i].size());
        Consume(::pread(fd, buf.data(), buf.size(), off));
      }
    }
    const uint64_t t0 = NowUs();
    std::vector<uint64_t> lat =
        RunParallel(d.order.size(), p.pipeline, [&](size_t i, std::vector<uint64_t>* sink) {
          const size_t idx = d.order[i];
          const off_t off = static_cast<off_t>(idx * d.record_size);
          const uint64_t a = NowUs();
          if (is_write) {
            Consume(::pwrite(fd, recs[idx].data(), recs[idx].size(), off));
            if (p.sync != 0) Consume(::fdatasync(fd));
          } else {
            std::vector<char> buf(recs[idx].size());
            Consume(::pread(fd, buf.data(), buf.size(), off));
          }
          sink->push_back(NowUs() - a);
        });
    const uint64_t dt = NowUs() - t0;
    const double secs = dt > 0 ? static_cast<double>(dt) / 1e6 : 1e-6;
    out.throughput.push_back(static_cast<double>(d.order.size()) / secs);
    std::vector<uint64_t> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    out.lat_p50.push_back(PercentilePermille(&sorted, 500));
    out.lat_p99.push_back(PercentilePermille(&sorted, 990));
    out.lat_all.insert(out.lat_all.end(), lat.begin(), lat.end());
  }
  ::close(fd);
  return out;
}

// ---------------------------------------------------------------------------
// std_map：纯内存 CPU 路径（完全不持久；§6.2 的公平性声明）
// ---------------------------------------------------------------------------
CellOutcome RunStdMapCell(const Params& p, Load load, const Dataset& d) {
  CellOutcome out;
  const bool is_write = load == Load::kSeqWrite || load == Load::kRandWrite;
  std::map<std::string, std::string> m;
  std::mutex mu;
  if (is_write) {
    for (size_t i = 0; i < d.keys.size(); ++i) m[d.keys[i]] = d.values[i];
  }

  for (int r = 0; r < p.repeats; ++r) {
    for (uint64_t w = 0; w < p.warmup; ++w) {
      const size_t i = static_cast<size_t>(w % d.keys.size());
      const std::string& k = d.keys[i];
      std::lock_guard<std::mutex> l(mu);
      if (is_write) {
        m[k] = d.values[i];
      } else {
        auto it = m.find(k);
        (void)it;
      }
    }
    const uint64_t t0 = NowUs();
    std::vector<uint64_t> lat =
        RunParallel(d.keys.size(), p.pipeline, [&](size_t i, std::vector<uint64_t>* sink) {
          const std::string& k = d.keys[i];
          const uint64_t a = NowUs();
          if (is_write) {
            std::lock_guard<std::mutex> l(mu);
            m[k] = d.values[i];
          } else {
            std::lock_guard<std::mutex> l(mu);
            auto it = m.find(k);
            (void)it;
          }
          sink->push_back(NowUs() - a);
        });
    const uint64_t dt = NowUs() - t0;
    const double secs = dt > 0 ? static_cast<double>(dt) / 1e6 : 1e-6;
    out.throughput.push_back(static_cast<double>(d.keys.size()) / secs);
    std::vector<uint64_t> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    out.lat_p50.push_back(PercentilePermille(&sorted, 500));
    out.lat_p99.push_back(PercentilePermille(&sorted, 990));
    out.lat_all.insert(out.lat_all.end(), lat.begin(), lat.end());
  }
  return out;
}

const char* RawEngineFor(Load load) {
  return (load == Load::kRandWrite || load == Load::kRandRead) ? "raw_pwrite" : "raw_file";
}

CellOutcome RunRawCell(const Params& p, Load load, const Dataset& d) {
  if (load == Load::kRandWrite || load == Load::kRandRead) return RunRawPwriteCell(p, load, d);
  return RunRawFileCell(p, load, d);
}

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------
bool ParseU64(const char* s, uint64_t* out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 0);
  if (end == s || *end != '\0') return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

int Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--dataset N] [--value-size B] [--key-dist seq|uniform] [--batch K]\n"
               "          [--pipeline P] [--sync 0|1] [--repeats R] [--warmup W] [--filter on|off]\n"
               "          [--engines all|lsm|raw|map] [--seed S] [--inject-missing] [--quick]\n"
               "          [--write-buffer-size B] [--repro-tol THR,P99] [--dir DIR]\n",
               argv0);
  return 2;
}

int ParseArgs(int argc, char** argv, Params* p) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&](const char** dst) {
      if (i + 1 >= argc) return false;
      *dst = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (std::strcmp(a, "--dataset") == 0) {
      if (!next(&v) || !ParseU64(v, &p->dataset)) return Usage(argv[0]);
    } else if (std::strcmp(a, "--value-size") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n)) return Usage(argv[0]);
      p->value_size = static_cast<size_t>(n);
    } else if (std::strcmp(a, "--key-dist") == 0) {
      if (!next(&v)) return Usage(argv[0]);
      p->key_dist = v;
    } else if (std::strcmp(a, "--batch") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n) || n == 0) return Usage(argv[0]);
      p->batch = static_cast<size_t>(n);
    } else if (std::strcmp(a, "--pipeline") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n) || n == 0) return Usage(argv[0]);
      p->pipeline = static_cast<int>(n);
    } else if (std::strcmp(a, "--sync") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n) || n > 1) return Usage(argv[0]);
      p->sync = static_cast<int>(n);
    } else if (std::strcmp(a, "--repeats") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n) || n == 0) return Usage(argv[0]);
      p->repeats = static_cast<int>(n);
    } else if (std::strcmp(a, "--warmup") == 0) {
      if (!next(&v) || !ParseU64(v, &p->warmup)) return Usage(argv[0]);
    } else if (std::strcmp(a, "--filter") == 0) {
      if (!next(&v)) return Usage(argv[0]);
      if (std::strcmp(v, "on") == 0) p->filter_on = true;
      else if (std::strcmp(v, "off") == 0) p->filter_on = false;
      else return Usage(argv[0]);
    } else if (std::strcmp(a, "--engines") == 0) {
      if (!next(&v)) return Usage(argv[0]);
      p->engines = v;
    } else if (std::strcmp(a, "--seed") == 0) {
      if (!next(&v) || !ParseU64(v, &p->seed)) return Usage(argv[0]);
    } else if (std::strcmp(a, "--inject-missing") == 0) {
      p->inject_missing = true;
    } else if (std::strcmp(a, "--quick") == 0) {
      p->quick = true;
    } else if (std::strcmp(a, "--write-buffer-size") == 0) {
      uint64_t n = 0;
      if (!next(&v) || !ParseU64(v, &n) || n == 0) return Usage(argv[0]);
      p->write_buffer_size = static_cast<size_t>(n);
    } else if (std::strcmp(a, "--repro-tol") == 0) {
      if (!next(&v)) return Usage(argv[0]);
      double a1 = 0.0, a2 = 0.0;
      if (std::sscanf(v, "%lf,%lf", &a1, &a2) != 2) return Usage(argv[0]);
      p->repro_thr = a1;
      p->repro_p99 = a2;
    } else if (std::strcmp(a, "--dir") == 0) {
      if (!next(&v)) return Usage(argv[0]);
      p->dir = v;
    } else {
      return Usage(argv[0]);
    }
  }
  if (p->key_dist != "seq" && p->key_dist != "uniform") return Usage(argv[0]);
  if (p->engines != "all" && p->engines != "lsm" && p->engines != "raw" && p->engines != "map") {
    return Usage(argv[0]);
  }
  if (p->quick) {
    if (p->dataset > 5000) p->dataset = 5000;
    if (p->warmup > 500) p->warmup = 500;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Params p;
  const int rc = ParseArgs(argc, argv, &p);
  if (rc != 0) return rc;

  Dataset d;
  BuildDataset(p, &d);
  if (d.keys.empty()) {
    std::fprintf(stderr, "empty dataset\n");
    return 2;
  }

  const bool want_lsm = p.engines == "all" || p.engines == "lsm";
  const bool want_raw = p.engines == "all" || p.engines == "raw";
  const bool want_map = p.engines == "all" || p.engines == "map";

  const Load loads[4] = {Load::kSeqWrite, Load::kRandWrite, Load::kRandRead, Load::kSeqRead};
  bool first_lsm_seen = false;
  for (Load load : loads) {
    const std::string notes = NotesFor(p);
    const bool write_load = load == Load::kSeqWrite || load == Load::kRandWrite;
    const char* lat_unit = (write_load && p.batch > 1) ? "call" : "op";
    // 同轮交替（§6.5）：同一次进程运行内按 engine 维度交替执行。
    if (want_lsm) {
      CellOutcome o = RunLsmCell(p, load, d);
      if (p.inject_missing && !first_lsm_seen) {
        o.missing += 1;   // M5-B03 的失败注入：只对 LSM 格
        first_lsm_seen = true;
      }
      const std::string miss = std::to_string(o.missing);
      const std::string mism = std::to_string(o.mismatch);
      PrintCell(p, LoadName(load), "lsm", o, "lsm", miss, mism, p.sync != 0 ? "fsync" : "none", notes,
                lat_unit);
    }
    if (want_raw) {
      const char* eng = RawEngineFor(load);
      CellOutcome o = RunRawCell(p, load, d);
      const std::string dur = p.sync == 0 ? "none" : (std::strcmp(eng, "raw_pwrite") == 0 ? "fdatasync" : "fsync");
      PrintCell(p, LoadName(load), eng, o, "na", "na", "na", dur, notes, "op");
    }
    if (want_map) {
      CellOutcome o = RunStdMapCell(p, load, d);
      PrintCell(p, LoadName(load), "std_map", o, "na", "na", "na", "none", notes, "op");
    }
  }
  std::fflush(stdout);
  return 0;
}
