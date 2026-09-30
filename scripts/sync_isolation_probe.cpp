// scripts/sync_isolation_probe.cpp —— R1 的同轮同机量效探针（**不是门禁**）
//
// 场景：S 个 syncer 线程持续调用显式 `DB::Sync()`，W 个 writer 线程持续做异步
//       （默认 `sync=false`）写。这正是 R1 的现场：修复前 `Sync()` 在整个 fsync 窗口内持
//       `commit_mu_` ⇒ writer 的**入队**被挡住，Put 延迟被拉到"一次 fsync"的量级。
//
// 只走公共 DB 接口；不读任何内部统计，因此 before/after 的口径完全一致。
// 固定输出（同参数 before/after 逐字段对照）：
//   SYNCISO writers=.. syncers=.. writers_sync=.. value_size=.. write_buffer_size=.. puts=..
//           duration_ms=.. throughput_ops_per_s=.. put_p50_us=.. put_p99_us=.. put_max_us=..
//           sync_calls=.. sync_p50_us=.. sync_p99_us=.. puts_per_sync_call=..
//
// 纪律（docs/roadmap.md / docs/m5-design.md §6.5）：只报**同轮同机**对照事实，
// 不承诺任何对外吞吐倍数；单机/单块 ext4/VM 级。负结果也要如实打印。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "db.h"

namespace {

using Clock = std::chrono::steady_clock;

uint64_t UsSince(Clock::time_point t0) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

uint64_t Percentile(std::vector<uint64_t>* v, double p) {
  if (v->empty()) return 0;
  std::sort(v->begin(), v->end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(v->size() - 1) + 0.5);
  if (idx >= v->size()) idx = v->size() - 1;
  return (*v)[idx];
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%012" PRIu64, i);
  return std::string(buf);
}

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--writers W] [--syncers S] [--puts N] [--value-size B] [--sync 0|1]\n"
               "          [--write-buffer-size B] [--dir DIR]\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  int writers = 4;
  int syncers = 1;
  uint64_t puts_total = 20000;
  size_t value_size = 100;
  int writer_sync = 0;
  size_t write_buffer_size = 64u << 20;   // 大缓冲：不触发 flush/轮转，隔离出 R1 本身
  std::string dir;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (std::strcmp(a, "--writers") == 0 && v != nullptr) { writers = std::atoi(v); ++i; }
    else if (std::strcmp(a, "--syncers") == 0 && v != nullptr) { syncers = std::atoi(v); ++i; }
    else if (std::strcmp(a, "--puts") == 0 && v != nullptr) { puts_total = std::strtoull(v, nullptr, 10); ++i; }
    else if (std::strcmp(a, "--value-size") == 0 && v != nullptr) { value_size = static_cast<size_t>(std::atoi(v)); ++i; }
    else if (std::strcmp(a, "--sync") == 0 && v != nullptr) { writer_sync = std::atoi(v); ++i; }
    else if (std::strcmp(a, "--write-buffer-size") == 0 && v != nullptr) { write_buffer_size = static_cast<size_t>(std::strtoull(v, nullptr, 10)); ++i; }
    else if (std::strcmp(a, "--dir") == 0 && v != nullptr) { dir = v; ++i; }
    else { Usage(argv[0]); return 2; }
  }
  if (writers < 1 || syncers < 0 || puts_total == 0) { Usage(argv[0]); return 2; }

  std::string tmpl = "/tmp/lsm_synciso_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (dir.empty()) {
    const char* made = ::mkdtemp(buf.data());
    if (made == nullptr) { std::perror("mkdtemp"); return 1; }
    dir.assign(made);
  }

  lsm::Options options;
  options.write_buffer_size = write_buffer_size;
  lsm::DB* db = nullptr;
  lsm::Status s = lsm::DB::Open(options, dir, &db);
  if (!s.ok() || db == nullptr) {
    std::fprintf(stderr, "DB::Open(%s) failed: %s\n", dir.c_str(), s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<lsm::DB> guard(db);

  const std::string value(value_size, 'v');
  std::atomic<uint64_t> next_key{0};
  std::atomic<bool> writers_done{false};
  std::atomic<uint64_t> sync_calls{0};
  std::atomic<bool> failed{false};

  std::vector<std::vector<uint64_t>> put_lat(static_cast<size_t>(writers));
  std::vector<std::vector<uint64_t>> sync_lat(static_cast<size_t>(syncers));

  const Clock::time_point t0 = Clock::now();
  std::vector<std::thread> ts;
  for (int w = 0; w < writers; ++w) {
    ts.emplace_back([&, w] {
      const uint64_t per = puts_total / static_cast<uint64_t>(writers);
      put_lat[static_cast<size_t>(w)].reserve(static_cast<size_t>(per));
      lsm::WriteOptions wo;
      wo.sync = (writer_sync != 0);
      for (uint64_t i = 0; i < per; ++i) {
        const std::string k = Key(w * per + i);
        const Clock::time_point p0 = Clock::now();
        const lsm::Status ps = db->Put(wo, k, value);
        put_lat[static_cast<size_t>(w)].push_back(UsSince(p0));
        if (!ps.ok()) { failed.store(true); break; }
      }
    });
  }
  for (int sy = 0; sy < syncers; ++sy) {
    ts.emplace_back([&, sy] {
      while (!writers_done.load()) {
        const Clock::time_point s0 = Clock::now();
        const lsm::Status ss = db->Sync();
        sync_lat[static_cast<size_t>(sy)].push_back(UsSince(s0));
        sync_calls.fetch_add(1);
        if (!ss.ok()) { failed.store(true); break; }
      }
    });
  }

  // writers 先 join：它们的墙钟区间就是吞吐的分母。
  for (int w = 0; w < writers; ++w) ts[static_cast<size_t>(w)].join();
  const uint64_t duration_us = UsSince(t0);
  writers_done.store(true);
  for (size_t i = static_cast<size_t>(writers); i < ts.size(); ++i) ts[i].join();

  std::vector<uint64_t> all_put;
  for (const std::vector<uint64_t>& v : put_lat) all_put.insert(all_put.end(), v.begin(), v.end());
  std::vector<uint64_t> all_sync;
  for (const std::vector<uint64_t>& v : sync_lat) all_sync.insert(all_sync.end(), v.begin(), v.end());

  const uint64_t done_puts = static_cast<uint64_t>(all_put.size());
  const uint64_t sc = sync_calls.load();
  std::printf(
      "SYNCISO writers=%d syncers=%d writers_sync=%d value_size=%zu write_buffer_size=%zu puts=%" PRIu64
      " duration_ms=%" PRIu64 " throughput_ops_per_s=%.0f put_p50_us=%" PRIu64 " put_p99_us=%" PRIu64
      " put_max_us=%" PRIu64 " sync_calls=%" PRIu64 " sync_p50_us=%" PRIu64 " sync_p99_us=%" PRIu64
      " puts_per_sync_call=%.2f failed=%d\n",
      writers, syncers, writer_sync, value_size, write_buffer_size, done_puts,
      duration_us / 1000,
      duration_us == 0 ? 0.0 : (static_cast<double>(done_puts) * 1e6 / static_cast<double>(duration_us)),
      Percentile(&all_put, 0.50), Percentile(&all_put, 0.99),
      all_put.empty() ? static_cast<uint64_t>(0) : *std::max_element(all_put.begin(), all_put.end()),
      sc, Percentile(&all_sync, 0.50), Percentile(&all_sync, 0.99),
      sc == 0 ? 0.0 : static_cast<double>(done_puts) / static_cast<double>(sc),
      failed.load() ? 1 : 0);

  const lsm::Status cs = guard->Close();
  if (!cs.ok()) std::fprintf(stderr, "Close failed: %s\n", cs.ToString().c_str());
  if (dir.rfind("/tmp/lsm_synciso_", 0) == 0) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed: %s\n", dir.c_str());
  }
  return failed.load() ? 1 : 0;
}
