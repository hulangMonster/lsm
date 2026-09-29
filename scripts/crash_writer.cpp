// scripts/crash_writer.cpp —— 崩溃对账的写入端（docs/m2-design.md §8.1）
//
// 协议（顺序不可颠倒，否则会产生假 missing）：
//   1) Put(WriteOptions{sync=true}) 返回 kOk —— 此刻该记录已 durable（I11）
//   2) **之后**才把 "key value" 追加到 sidecar，并逐行 fsync
//   ⇒ 被 kill -9 时，sidecar 里出现的每一行都必然是「已 ack 且已 durable」的写。
//     反过来（先记 sidecar 再 Put）会把崩溃窗口内的未完成写记成已 ack ⇒ 假 missing。
//
// 用法：crash_writer <db_dir> <sidecar>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "db.h"

using namespace lsm;

namespace {

bool AppendLineAndSync(int fd, const std::string& line) {
  size_t off = 0;
  while (off < line.size()) {
    const ssize_t n = ::write(fd, line.data() + off, line.size() - off);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return ::fsync(fd) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: crash_writer <db_dir> <sidecar>\n");
    return 2;
  }
  const std::string db_dir = argv[1];
  const std::string sidecar = argv[2];

  const int ack_fd = ::open(sidecar.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (ack_fd < 0) {
    std::perror("open sidecar");
    return 2;
  }

  DB* db = nullptr;
  const Status os = DB::Open(Options(), db_dir, &db);
  if (!os.ok()) {
    std::fprintf(stderr, "DB::Open failed: %s\n", os.ToString().c_str());
    return 2;
  }

  WriteOptions wo;
  wo.sync = true;   // 门禁口径：每条都必须 durable-before-ack
  for (long long i = 1;; ++i) {
    char key[32];
    char val[64];
    std::snprintf(key, sizeof(key), "k%08lld", i);
    std::snprintf(val, sizeof(val), "v%08lld", i);
    const Status s = db->Put(wo, Slice(key), Slice(val));
    if (!s.ok()) {
      std::fprintf(stderr, "Put(%s) failed: %s\n", key, s.ToString().c_str());
      return 1;
    }
    const std::string line = std::string(key) + " " + val + "\n";
    if (!AppendLineAndSync(ack_fd, line)) {
      std::fprintf(stderr, "sidecar append failed\n");
      return 1;
    }
  }
}
