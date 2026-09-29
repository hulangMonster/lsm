// scripts/lsm_damage_test.cpp —— 损坏注入的端到端判据（docs/m2-design.md §9.2 的 B03/B04）
//
//   mode=tail    真实 WAL 上做**逐字节截断扫描**：每个「record 边界 ±3 字节」都试一遍，
//                要求 Open 必须成功，且恢复出的 key 集合恰好是某个前缀 [1..k]（不得多、不得少）。
//   mode=middle  在**中间**某条 record 的 payload 上翻一个字节：Open 必须返回 kCorruption，
//                且信息里能定位到文件（这正是"拒绝启动 vs 安全截断"的分界线，见 design §5.3）。
//
// 用法：lsm_damage_test <tail|middle> <workdir>
// 输出固定行格式，退出码非 0 表示判据不成立。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "db.h"
#include "filename.h"
#include "util/env.h"

using namespace lsm;

namespace {

constexpr int kRecords = 200;
constexpr int kMiddleRecord = 50;   // 用第 50 条做中间损坏（前面 49 条完好、后面 150 条完好）

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%08d", i);
  return buf;
}
std::string Val(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%08d", i);
  return buf;
}

bool FillDB(const std::string& dir) {
  DB* db = nullptr;
  if (!DB::Open(Options(), dir, &db).ok()) return false;
  WriteOptions wo;
  wo.sync = true;
  for (int i = 1; i <= kRecords; ++i) {
    if (!db->Put(wo, Key(i), Val(i)).ok()) return false;
  }
  const bool ok = db->Close().ok();
  delete db;
  return ok;
}

bool ReadFile(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char buf[8192];
  ssize_t n;
  out->clear();
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out->append(buf, static_cast<size_t>(n));
  ::close(fd);
  return n >= 0;
}

bool WriteFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
  return true;
}

// 恢复后统计 key 的出现情况：返回出现的最大连续前缀长度，*extra 记录"前缀之外还出现的 key 数"
int RecoveredPrefix(const std::string& dir, int* extra, Status* open_status) {
  DB* db = nullptr;
  *open_status = DB::Open(Options(), dir, &db);
  if (!open_status->ok()) return -1;
  int prefix = 0;
  int beyond = 0;
  std::string v;
  for (int i = 1; i <= kRecords; ++i) {
    const bool present = db->Get(Key(i), &v).ok();
    if (i == prefix + 1 && present) {
      ++prefix;
    } else if (present) {
      ++beyond;
    } else if (i <= prefix) {
      ++beyond;   // 前缀中间缺了一个 ⇒ 也算异常
    }
  }
  *extra = beyond;
  db->Close();
  delete db;
  return prefix;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: lsm_damage_test <tail|middle> <workdir>\n");
    return 2;
  }
  const std::string mode = argv[1];
  const std::string dir = argv[2];
  Env* env = Env::Default();

  if (!FillDB(dir)) {
    std::fprintf(stderr, "填充 DB 失败（%s）\n", dir.c_str());
    return 2;
  }
  const std::string log = LogFileName(dir, 1);
  uint64_t size = 0;
  if (!env->GetFileSize(log, &size).ok() || size == 0) {
    std::fprintf(stderr, "取不到 WAL 大小：%s\n", log.c_str());
    return 2;
  }
  if (size % static_cast<uint64_t>(kRecords) != 0) {
    std::fprintf(stderr, "record 大小不均匀（size=%llu），本工具需要等长 record\n",
                 static_cast<unsigned long long>(size));
    return 2;
  }
  const uint64_t rec = size / static_cast<uint64_t>(kRecords);

  std::string original;
  if (!ReadFile(log, &original)) return 2;

  if (mode == "tail") {
    int cases = 0;
    int ok = 0;
    int fail = 0;
    for (int b = 0; b <= kRecords; ++b) {
      const long long boundary = static_cast<long long>(b) * static_cast<long long>(rec);
      for (int d = -3; d <= 3; ++d) {
        const long long len = boundary + d;
        if (len < 0 || len > static_cast<long long>(size)) continue;
        ++cases;
        const std::string cut = original.substr(0, static_cast<size_t>(len));
        if (!WriteFile(log, cut)) {
          ++fail;
          continue;
        }
        // 期望：保留 end <= len 的完整 record ⇒ 前缀长度 = 完整 record 数
        const int expect = static_cast<int>(static_cast<long long>(len) / static_cast<long long>(rec));
        int extra = 0;
        Status st;
        const int got = RecoveredPrefix(dir, &extra, &st);
        const bool good = st.ok() && got == expect && extra == 0;
        if (good) {
          ++ok;
        } else {
          ++fail;
          std::printf("TAIL_FAIL len=%lld expect_prefix=%d got=%d extra=%d open=%s\n", len, expect,
                      got, extra, st.ToString().c_str());
        }
        // 复原，供下一轮使用
        WriteFile(log, original);
      }
    }
    std::printf("TAIL_CASES %d TAIL_OK %d TAIL_FAIL %d RECORD_BYTES %llu\n", cases, ok, fail,
                static_cast<unsigned long long>(rec));
    return fail == 0 ? 0 : 1;
  }

  if (mode == "middle") {
    // 翻转第 kMiddleRecord 条 record 的 payload 第一个字节（其后仍有大量完好 record）
    const size_t off = static_cast<size_t>(kMiddleRecord - 1) * static_cast<size_t>(rec) + 7;
    std::string damaged = original;
    damaged[off] = static_cast<char>(damaged[off] ^ 0x01);
    if (!WriteFile(log, damaged)) return 2;
    int extra = 0;
    Status st;
    const int got = RecoveredPrefix(dir, &extra, &st);
    const bool corrupted = st.IsCorruption();
    std::printf("MIDDLE_OPEN_CORRUPTION %d RECOVERED_PREFIX %d DETAIL %s\n", corrupted ? 1 : 0, got,
                st.ToString().c_str());
    // 复原，避免污染工作目录
    WriteFile(log, original);
    const bool locatable = st.ToString().find("000001.log") != std::string::npos;
    std::printf("MIDDLE_LOCATABLE %d\n", locatable ? 1 : 0);
    return (corrupted && locatable) ? 0 : 1;
  }

  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
