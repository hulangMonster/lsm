// scripts/lsm_filter_damage.cpp —— M5.3 / M5-B09：真实磁盘上的 filter 块损坏扫描
//
// 契约来源：docs/m5-design.md §7.5（M5-E 腿）、§10.2 M5-B09、§3.2 的 8 条结构校验、E3/E4。
//
// 做两件事：
//   ① 逐字节翻转扫描（**不**重算 CRC）：任何翻转都不得造成「静默假阴性 / 静默错值」；
//      数据/索引/footer 区的翻转必须被检出（kCorruption），filter 区的翻转必须让 filter 降级。
//   ② 错位注入（**重算 CRC**）：把 filter payload 的 offset[0] 改成非法值并重算块 CRC，
//      使 CRC 通过但结构自洽性被破坏 —— 必须被检出为 kCorrupt（证明 CRC 不是唯一防线），
//      且读结果仍正确、零静默假阴性。
//
// 用法：lsm_filter_damage <workdir> [cases]
// 输出标记：FILTER_DAMAGE_CASES / FILTER_SILENT_FALSE_NEGATIVE / FILTER_SILENT_WRONG_VALUE /
//           FILTER_DAMAGE_DETECTED / FILTER_DAMAGE_FILTER_DEGRADED /
//           FILTER_MISALIGN_INJECTED / FILTER_MISALIGN_DETECTED / FILTER_MISALIGN_SILENT_FN /
//           [FILTER_DAMAGE_OK]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sstable/block.h"
#include "sstable/format.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/env.h"

using namespace lsm;

namespace {

std::string UserKey(int i) {
  char b[32];
  std::snprintf(b, sizeof(b), "k%06d", i);
  return std::string(b);
}

std::string Value(int i) { return std::string("v") + std::to_string(i) + std::string(20, 'd'); }

std::string LookupKey(const std::string& user) {
  return BuildInternalKey(user, kMaxSequenceNumber, kValueTypeForSeek);
}

// 小端就地写 32 位（coding.h 只提供「追加到 std::string」，这里需要改磁盘字节）。
void StoreFixed32(char* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xffu);
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  out->clear();
  char buf[8192];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out->append(buf, static_cast<size_t>(n));
  ::close(fd);
  return n >= 0;
}

bool WriteWholeFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const ssize_t w = ::write(fd, data.data(), data.size());
  ::close(fd);
  return w == static_cast<ssize_t>(data.size());
}

struct ScanCounts {
  uint64_t scanned = 0;
  uint64_t silent_false_negative = 0;
  uint64_t silent_wrong_value = 0;
  uint64_t detected = 0;
  uint64_t degraded = 0;
};

// 对一份（可能被恶意修改的）SST 字节做完整读校验：任一 key 读成不存在 = 静默假阴性。
void CheckReads(const Options& o, Env* env, const std::string& path, const std::vector<std::string>& users,
                const std::vector<std::string>& vals, const std::string& smallest,
                const std::string& largest, ScanCounts* c) {
  std::shared_ptr<Table> t;
  const Status open = Table::Open(o, env, path, &t, &smallest, &largest);
  if (!open.ok()) {
    ++c->detected;
    return;
  }
  if (t->filter_state() == Table::FilterState::kCorrupt) ++c->degraded;
  for (size_t i = 0; i < users.size(); ++i) {
    std::string v;
    TableGetResult r = TableGetResult::kNotFound;
    const Status g = t->GetEntry(Slice(LookupKey(users[i])), &v, &r, nullptr);
    if (g.IsCorruption()) {
      ++c->detected;
      return;
    }
    if (!g.ok()) {
      ++c->silent_wrong_value;
      return;
    }
    if (r == TableGetResult::kNotFound) {
      ++c->silent_false_negative;
      return;
    }
    if (r == TableGetResult::kFound && v != vals[i]) {
      ++c->silent_wrong_value;
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <workdir> [cases]\n", argv[0]);
    return 2;
  }
  const std::string workdir = argv[1];
  const uint64_t cases = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
  if (cases == 0) return 2;
  if (::mkdir(workdir.c_str(), 0755) != 0) {
    // 已存在也可继续（脚本用 mkdtemp 保证唯一）。
  }
  const std::string path = workdir + "/000001.sst";

  Env* env = Env::Default();
  Options o;
  o.bloom_bits = 10;
  o.block_size = 512;

  const int kKeys = 50;
  std::vector<std::string> users, vals, iks;
  for (int i = 0; i < kKeys; ++i) {
    users.push_back(UserKey(i));
    vals.push_back(Value(i));
    iks.push_back(BuildInternalKey(users.back(), 1, kTypeValue));
  }

  WritableFile* raw = nullptr;
  if (!env->NewWritableFile(path, &raw).ok()) {
    std::fprintf(stderr, "NewWritableFile failed: %s\n", path.c_str());
    return 1;
  }
  std::unique_ptr<WritableFile> wf(raw);
  TableBuilder builder(o, wf.get());
  for (size_t i = 0; i < iks.size(); ++i) {
    if (!builder.Add(Slice(iks[i]), Slice(vals[i])).ok()) {
      std::fprintf(stderr, "Add failed\n");
      return 1;
    }
  }
  if (!builder.Finish().ok() || !wf->Close().ok()) {
    std::fprintf(stderr, "Finish/Close failed\n");
    return 1;
  }
  if (builder.filter_bytes() == 0 || builder.NumDataBlocks() <= 1) {
    std::fprintf(stderr, "前置条件不成立：filter_bytes=%llu blocks=%llu\n",
                 static_cast<unsigned long long>(builder.filter_bytes()),
                 static_cast<unsigned long long>(builder.NumDataBlocks()));
    return 1;
  }

  std::string original;
  if (!ReadWholeFile(path, &original) || original.size() < 100) {
    std::fprintf(stderr, "读回 %s 失败\n", path.c_str());
    return 1;
  }
  std::shared_ptr<Table> probe;
  if (!Table::Open(o, env, path, &probe).ok()) {
    std::fprintf(stderr, "干净文件 Table::Open 失败\n");
    return 1;
  }
  const std::string smallest = probe->FirstInternalKey();
  const std::string largest = probe->LastInternalKey();
  probe.reset();

  // ---- ① 逐字节翻转扫描（采样 cases 个） ----
  ScanCounts scan;
  for (uint64_t k = 0; k < cases; ++k) {
    const size_t i = (cases >= original.size()) ? static_cast<size_t>(k % original.size())
                                                : static_cast<size_t>(k * original.size() / cases);
    std::string bad = original;
    bad[i] = static_cast<char>(static_cast<unsigned char>(bad[i]) ^ 0x01u);
    if (!WriteWholeFile(path, bad)) {
      std::fprintf(stderr, "写回失败\n");
      return 1;
    }
    ++scan.scanned;
    ScanCounts one;
    CheckReads(o, env, path, users, vals, smallest, largest, &one);
    // 一次翻转只允许归入一类（CheckReads 会在第一个异常处返回）。
    if (one.silent_false_negative) {
      ++scan.silent_false_negative;
    } else if (one.silent_wrong_value) {
      ++scan.silent_wrong_value;
    } else {
      scan.detected += one.detected;
      scan.degraded += one.degraded;
    }
  }
  if (!WriteWholeFile(path, original)) {
    std::fprintf(stderr, "恢复原文件失败\n");
    return 1;
  }

  // ---- ② 错位注入（重算 CRC）：把 filter payload 的 offset[0] 改成非法值 ----
  uint64_t misalign_injected = 0, misalign_detected = 0, misalign_silent_fn = 0;
  {
    std::string bad = original;
    // footer（最后 44B）→ metaindex handle → 找到 filter 条目 → filter payload。
    Footer footer;
    if (footer.DecodeFrom(Slice(bad.data() + bad.size() - kFooterSize, kFooterSize)).ok()) {
      const uint64_t mo = footer.metaindex_handle.offset;
      const uint64_t ms = footer.metaindex_handle.size;
      if (mo + ms <= bad.size() && ms > kBlockOverhead) {
        const Slice mp(bad.data() + mo + kBlockHeaderSize, static_cast<size_t>(ms - kBlockOverhead));
        std::unique_ptr<BlockReader> reader;
        if (BlockReader::Open(mp, &reader, nullptr).ok()) {
          reader->SeekToFirst();
          while (reader->Valid()) {
            if (reader->key().ToString() == kBuiltinBloomFilterName) {
              BlockHandle fh;
              size_t consumed = 0;
              if (fh.DecodeFrom(reader->value(), &consumed).ok() && fh.size > kBlockOverhead &&
                  fh.offset + fh.size <= bad.size()) {
                const size_t payload_off = static_cast<size_t>(fh.offset) + kBlockHeaderSize;
                const size_t payload_len = static_cast<size_t>(fh.size) - kBlockOverhead;
                if (payload_len >= 8) {
                  const uint32_t array_offset =
                      DecodeFixed32(bad.data() + payload_off + payload_len - 8);
                  if (array_offset + 8 <= payload_len && array_offset + 4 <= payload_len) {
                    // offset[0] 必须为 0；写成 array_offset + 1 ⇒ 结构校验必须拒绝。
                    StoreFixed32(&bad[payload_off + array_offset],
                                 static_cast<uint32_t>(array_offset + 1));
                    // 重算块 CRC：crc32c(length(4 LE) ‖ type(1) ‖ payload)。
                    const uint32_t crc =
                        crc32c::Value(bad.data() + fh.offset, kBlockHeaderSize + payload_len);
                    StoreFixed32(&bad[payload_off + payload_len], crc);
                    misalign_injected = 1;
                    if (WriteWholeFile(path, bad)) {
                      std::shared_ptr<Table> t;
                      const Status op = Table::Open(o, env, path, &t, &smallest, &largest);
                      if (!op.ok()) {
                        ++misalign_detected;
                      } else if (t->filter_state() == Table::FilterState::kCorrupt) {
                        ++misalign_detected;
                        for (size_t i = 0; i < users.size(); ++i) {
                          std::string v;
                          TableGetResult r = TableGetResult::kNotFound;
                          const Status g = t->GetEntry(Slice(LookupKey(users[i])), &v, &r, nullptr);
                          if (!g.ok() || r == TableGetResult::kNotFound) {
                            ++misalign_silent_fn;
                            break;
                          }
                          if (v != vals[i]) {
                            ++misalign_silent_fn;
                            break;
                          }
                        }
                      }
                    }
                  }
                }
              }
              break;
            }
            reader->Next();
          }
        }
      }
    }
    WriteWholeFile(path, original);
  }

  const bool ok = scan.silent_false_negative == 0 && scan.silent_wrong_value == 0 &&
                  scan.detected > 0 && scan.degraded > 0 && misalign_injected == 1 &&
                  misalign_detected == 1 && misalign_silent_fn == 0;

  std::printf("FILTER_DAMAGE_CASES %llu\n", static_cast<unsigned long long>(scan.scanned));
  std::printf("FILTER_SILENT_FALSE_NEGATIVE %llu\n",
              static_cast<unsigned long long>(scan.silent_false_negative));
  std::printf("FILTER_SILENT_WRONG_VALUE %llu\n",
              static_cast<unsigned long long>(scan.silent_wrong_value));
  std::printf("FILTER_DAMAGE_DETECTED %llu\n", static_cast<unsigned long long>(scan.detected));
  std::printf("FILTER_DAMAGE_FILTER_DEGRADED %llu\n", static_cast<unsigned long long>(scan.degraded));
  std::printf("FILTER_MISALIGN_INJECTED %llu\n", static_cast<unsigned long long>(misalign_injected));
  std::printf("FILTER_MISALIGN_DETECTED %llu\n", static_cast<unsigned long long>(misalign_detected));
  std::printf("FILTER_MISALIGN_SILENT_FN %llu\n", static_cast<unsigned long long>(misalign_silent_fn));
  if (ok) std::printf("[FILTER_DAMAGE_OK]\n");
  std::fflush(stdout);
  return ok ? 0 : 1;
}
