// scripts/fsbench_commit_latency.cpp —— 单次提交延迟微基准（docs/m2-design.md §9.2 B07 / §9.3 固定输出格式）
//
// 为什么必须有它：M5 的一切"组提交/N 倍"结论都要除以本机的一次 fsync 成本，
// 而 M2 #0 实测发现历史记录里的「约 8 ms」在本机不成立（实测 2.2~3.1 ms，差 3 倍）。
// 因此本工具把**环境指纹**（核数/负载/文件系统/挂载点）与五个策略一起打出来——
// 同一个脚本、同一台机器、不同时间点结果可以差 3 倍，不记环境就无法解释差异。
//
// 用法：fsbench_commit_latency <dir> [N]      （N 默认 500）
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr size_t kRecordBytes = 4096;   // 每次提交 = 1 条 4 KiB 记录

double NowMs() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Stats {
  double min_ms = 0;
  double median_ms = 0;
  double p90_ms = 0;
  double max_ms = 0;
};

Stats Summarize(std::vector<double>* lat) {
  Stats s;
  if (lat->empty()) return s;
  std::sort(lat->begin(), lat->end());
  s.min_ms = lat->front();
  s.max_ms = lat->back();
  s.median_ms = (*lat)[lat->size() / 2];
  s.p90_ms = (*lat)[static_cast<size_t>(lat->size() * 0.9)];
  return s;
}

void Report(const char* strategy, int n, const Stats& s) {
  std::printf("STRATEGY %-22s N %4d  MIN_MS %7.3f  MEDIAN_MS %7.3f  P90_MS %7.3f  MAX_MS %7.3f\n",
              strategy, n, s.min_ms, s.median_ms, s.p90_ms, s.max_ms);
}

std::string ReadFileTrim(const char* path) {
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) return "?";
  char buf[512];
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) return "?";
  buf[n] = 0;
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: fsbench_commit_latency <dir> [N]\n");
    return 2;
  }
  const std::string dir = argv[1];
  const int n = argc > 2 ? std::atoi(argv[2]) : 500;
  if (n <= 0) return 2;

  std::printf("== fsbench: 单次提交延迟（%zuB 追加 + flush，N=%d，目录 %s）==\n", kRecordBytes, n,
              dir.c_str());
  std::printf("machine %ld cores  load %s %s %s  fs %s  mount %s\n",
              ::sysconf(_SC_NPROCESSORS_ONLN), ReadFileTrim("/proc/loadavg").substr(0, 4).c_str(),
              ReadFileTrim("/proc/loadavg").substr(5, 4).c_str(),
              ReadFileTrim("/proc/loadavg").substr(10, 4).c_str(),
              "see below", dir.c_str());

  {
    // 文件系统与挂载点：用 statvfs 拿不到名字，这里直接读 /proc/mounts 找最长前缀匹配
    FILE* f = std::fopen("/proc/mounts", "r");
    std::string best_fs = "?";
    std::string best_mp = "";
    if (f != nullptr) {
      char line[1024];
      while (std::fgets(line, sizeof(line), f) != nullptr) {
        char dev[256];
        char mp[256];
        char fs[64];
        if (std::sscanf(line, "%255s %255s %63s", dev, mp, fs) != 3) continue;
        const std::string m(mp);
        if (dir.compare(0, m.size(), m) != 0) continue;
        if (m.size() >= best_mp.size()) {   // 最长前缀匹配（/tmp 比 / 更具体）
          best_fs = fs;
          best_mp = m;
        }
      }
      std::fclose(f);
    }
    if (best_mp.empty()) best_mp = "?";
    std::printf("NOTE 文件系统 %s 挂载点 %s 记录 %zuB（预分配档先 posix_fallocate(64 MiB)）\n",
                best_fs.c_str(), best_mp.c_str(), kRecordBytes);
  }

  std::vector<char> buf(kRecordBytes, 'x');
  std::vector<double> lat;

  // 1) append + fsync
  lat.clear();
  {
    const std::string path = dir + "/fsb_append.dat";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      std::perror("open");
      return 2;
    }
    for (int i = 0; i < n; ++i) {
      const double t0 = NowMs();
      if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) {
        std::perror("write");
        return 2;
      }
      if (::fsync(fd) != 0) {
        std::perror("fsync");
        return 2;
      }
      lat.push_back(NowMs() - t0);
    }
    ::close(fd);
    Report("append+fsync", n, Summarize(&lat));
  }

  // 2) append + fdatasync
  lat.clear();
  {
    const std::string path = dir + "/fsb_fdata.dat";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 2;
    for (int i = 0; i < n; ++i) {
      const double t0 = NowMs();
      if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) return 2;
      if (::fdatasync(fd) != 0) return 2;
      lat.push_back(NowMs() - t0);
    }
    ::close(fd);
    Report("append+fdatasync", n, Summarize(&lat));
  }

  // 3) prealloc + fsync
  lat.clear();
  {
    const std::string path = dir + "/fsb_prealloc.dat";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 2;
    if (::posix_fallocate(fd, 0, 64 * 1024 * 1024) != 0) {
      std::fprintf(stderr, "posix_fallocate 失败（跳过该档）\n");
    } else {
      for (int i = 0; i < n; ++i) {
        const double t0 = NowMs();
        if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) return 2;
        if (::fsync(fd) != 0) return 2;
        lat.push_back(NowMs() - t0);
      }
      Report("prealloc+fsync", n, Summarize(&lat));
    }
    ::close(fd);
  }

  // 4) O_DIRECT + fsync
  lat.clear();
  {
    const std::string path = dir + "/fsb_direct.dat";
    void* aligned = nullptr;
    if (::posix_memalign(&aligned, 4096, kRecordBytes) == 0) {
      std::memset(aligned, 'x', kRecordBytes);
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
      if (fd >= 0) {
        for (int i = 0; i < n; ++i) {
          const double t0 = NowMs();
          if (::write(fd, aligned, kRecordBytes) != static_cast<ssize_t>(kRecordBytes)) break;
          if (::fsync(fd) != 0) break;
          lat.push_back(NowMs() - t0);
        }
        ::close(fd);
        if (static_cast<int>(lat.size()) == n) Report("O_DIRECT+fsync", n, Summarize(&lat));
        else std::printf("STRATEGY %-22s SKIPPED（该文件系统不支持或写失败）\n", "O_DIRECT+fsync");
      }
      std::free(aligned);
    }
  }

  // 5) create + fsync + dir fsync
  lat.clear();
  {
    for (int i = 0; i < n; ++i) {
      const double t0 = NowMs();
      const std::string path = dir + "/fsb_create_" + std::to_string(i) + ".dat";
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) return 2;
      if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) return 2;
      if (::fsync(fd) != 0) return 2;
      ::close(fd);
      lat.push_back(NowMs() - t0);
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
    Report("create+fsync+dirfsync", n, Summarize(&lat));
  }

  std::printf("NOTE 每次提交 = 1 条 %zuB 记录；本表的 MEDIAN_MS 是 M5 计算组提交收益的分母\n",
              kRecordBytes);
  return 0;
}
