// scripts/batch_crash_writer.cpp —— M5.2 批崩溃对账的写入端（docs/m5-design.md §10.2 的 M5-B01）
//
// 协议（顺序不可颠倒，否则会产生假 missing）：
//   1) 先构造一整批 `batch_size` 条 entry 的 WriteBatch；
//   2) `db->Write(WriteOptions{sync=true}, &batch)` 返回 kOk —— 此刻**整批**已 durable（I51/I54）；
//   3) **之后**才把 `<batch_id> <count>` 追加到 sidecar，并逐行 fsync。
//   ⇒ 被 kill -9 时，sidecar 里出现的每一行都必然是「已 ack 且已 durable 的整批」。
//     反过来（先记 sidecar 再 Write）会把崩溃窗口内未完成的批记成已 ack ⇒ 假 missing。
//
// 一个 WriteBatch = 一条 WAL record（§13.2），所以「批」就是崩溃原子性的单位；
// 本工具与 lsm_batch_crash_test.sh 共同验证「半批不可见」。
//
// 用法：batch_crash_writer <db_dir> <sidecar> <batch_size> <sync 0|1> [write_buffer_size]
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "db.h"
#include "write_batch.h"

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
  if (argc < 5) {
    std::fprintf(stderr, "usage: batch_crash_writer <db_dir> <sidecar> <batch_size> <sync 0|1> "
                         "[write_buffer_size]\n");
    return 2;
  }
  const std::string db_dir = argv[1];
  const std::string sidecar = argv[2];
  const int batch_size = std::atoi(argv[3]);
  const bool sync = std::atoi(argv[4]) != 0;
  const size_t write_buffer_size =
      argc >= 6 ? static_cast<size_t>(std::strtoull(argv[5], nullptr, 10)) : 0;
  if (batch_size < 2) {
    std::fprintf(stderr, "batch_size 必须 >= 2（否则「半批」无意义）\n");
    return 2;
  }

  const int ack_fd = ::open(sidecar.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (ack_fd < 0) {
    std::perror("open sidecar");
    return 2;
  }

  Options options;
  if (write_buffer_size != 0) options.write_buffer_size = write_buffer_size;
  DB* db = nullptr;
  const Status os = DB::Open(options, db_dir, &db);
  if (!os.ok()) {
    std::fprintf(stderr, "DB::Open failed: %s\n", os.ToString().c_str());
    return 2;
  }
  std::fprintf(stderr, "batch_crash_writer: batch_size=%d sync=%d write_buffer_size=%llu\n",
               batch_size, sync ? 1 : 0,
               static_cast<unsigned long long>(options.write_buffer_size));

  WriteOptions wo;
  wo.sync = sync;
  for (long long b = 1;; ++b) {
    WriteBatch batch;
    for (int i = 0; i < batch_size; ++i) {
      char key[48];
      char val[64];
      std::snprintf(key, sizeof(key), "b%08lld_%04d", b, i);
      std::snprintf(val, sizeof(val), "v%08lld_%04d", b, i);
      batch.Put(Slice(key), Slice(val));
    }
    const Status s = db->Write(wo, &batch);
    if (!s.ok()) {
      std::fprintf(stderr, "Write(batch %lld) failed: %s\n", b, s.ToString().c_str());
      return 1;
    }
    char line[64];
    std::snprintf(line, sizeof(line), "%lld %d\n", b, batch_size);
    if (!AppendLineAndSync(ack_fd, line)) {
      std::fprintf(stderr, "sidecar append failed\n");
      return 1;
    }
  }
}
