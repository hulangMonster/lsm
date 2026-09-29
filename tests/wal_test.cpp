// tests/wal_test.cpp —— M2.1 的 WAL 层用例（docs/m2-design.md §9.1 的 A01~A10）
//
// 口径说明（#1 阶段回退 #0 的修订，见 docs/m2-prerequisites.md §9）：
//   * A07/A08 在 WAL 层断言 kParseFail + `valid_record_after_failure`，把"尾部截断 vs 中间损坏"的
//     §5.3 判定留给 M2.2 的 Recovery（WAL 层只提供判定所需的事实，不替上层决定）。
//   * A03 的 block_offset 范围扩到含 32761（剩余正好 7 字节）：那是 writer 会写出 length=0 片段的边界，
//     原设计的 32762..32767 漏掉了它。
#include "test_harness.h"

#include <memory>
#include <string>
#include <vector>

#include "faulty_env.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/env.h"
#include "wal.h"

namespace lsm {
namespace {

using test::FaultyEnv;
using test::TempDir;

std::string Repeat(char c, size_t n) { return std::string(n, c); }

// 读回整个文件（测试侧独立实现，不经过 WALReader）
std::string ReadFile(Env* env, const std::string& path) {
  SequentialFile* raw = nullptr;
  EXPECT_TRUE(env->NewSequentialFile(path, &raw).ok()) << path;
  if (raw == nullptr) return std::string();
  std::unique_ptr<SequentialFile> f(raw);
  std::string out;
  char scratch[4096];
  while (true) {
    Slice piece;
    const Status s = f->Read(sizeof(scratch), &piece, scratch);
    EXPECT_TRUE(s.ok()) << s.ToString();
    if (!s.ok() || piece.empty()) break;
    out.append(piece.data(), piece.size());
  }
  return out;
}

void WriteFile(Env* env, const std::string& path, const std::string& data) {
  WritableFile* raw = nullptr;
  ASSERT_TRUE(env->NewWritableFile(path, &raw).ok());
  ASSERT_TRUE(raw != nullptr);
  std::unique_ptr<WritableFile> f(raw);
  EXPECT_TRUE(f->Append(Slice(data)).ok());
  EXPECT_TRUE(f->Close().ok());
}

struct ScanOut {
  Status status;
  std::vector<std::string> records;
  WALScanResult result;
};

// 扫描一个 WAL 文件：把记录收集出来（kParseFail/kTailResidue 时收集到的仍是"完好前缀"）
ScanOut Scan(Env* env, const std::string& path) {
  ScanOut out;
  WALReader reader(env, path);
  out.status = reader.ReadAll([&out](const Slice& rec) { out.records.push_back(rec.ToString()); },
                              &out.result);
  return out;
}

}  // namespace

// A01
TEST(WAL, WriteReadRoundTrip) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("000001.log");

  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(/*append=*/false).ok());
  // 空 payload 必须被拒绝（§4.2：一条逻辑 record 的每个片段都必须 length >= 1）
  EXPECT_TRUE(w.Append(Slice()).IsInvalidArgument()) << "空 record 必须被拒绝";

  const size_t sizes[] = {1, 10, 100, 1000, 5000};
  std::vector<std::string> want;
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const std::string rec = Repeat(static_cast<char>('a' + i), sizes[i]);
    ASSERT_TRUE(w.Append(Slice(rec)).ok()) << "size=" << sizes[i];
    want.push_back(rec);
  }
  ASSERT_TRUE(w.Sync().ok());
  const uint64_t size_before_close = w.file_size();
  ASSERT_TRUE(w.Close().ok());

  const ScanOut got = Scan(env, path);
  ASSERT_TRUE(got.status.ok()) << got.status.ToString();
  EXPECT_EQ(want, got.records);
  EXPECT_EQ(WALScanVerdict::kClean, got.result.verdict);
  EXPECT_EQ(size_before_close, got.result.last_good_end) << "CLEAN 时必须恰好消费到文件尾";
}

// A02
TEST(WAL, LargeRecordCrossBlockSplit) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("000001.log");

  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(false).ok());
  EXPECT_TRUE(w.Append(Slice()).IsInvalidArgument());

  struct Case {
    size_t size;
    uint8_t first_type;
  };
  const Case cases[] = {
      {1, kFullType},
      {kWALMaxPayload, kFullType},                  // 32761：正好一块payload
      {kWALMaxPayload + 1, kFirstType},             // 跨两块
      {2 * kWALMaxPayload, kFirstType},             // 跨三块
  };
  for (const Case& c : cases) {
    WALWriter one(env, dir.File("one.log"));
    ASSERT_TRUE(one.Open(false).ok());
    const std::string rec = Repeat('x', c.size);
    ASSERT_TRUE(one.Append(Slice(rec)).ok()) << "size=" << c.size;
    ASSERT_TRUE(one.Sync().ok());
    ASSERT_TRUE(one.Close().ok());

    const ScanOut got = Scan(env, dir.File("one.log"));
    ASSERT_EQ(1u, got.records.size()) << "size=" << c.size;
    EXPECT_EQ(rec, got.records[0]) << "size=" << c.size;
    EXPECT_EQ(WALScanVerdict::kClean, got.result.verdict) << "size=" << c.size;

    const std::string raw = ReadFile(env, dir.File("one.log"));
    ASSERT_GE(raw.size(), kWALHeaderSize);
    EXPECT_EQ(static_cast<int>(c.first_type), static_cast<int>(static_cast<unsigned char>(raw[6])))
        << "首片段类型不符，size=" << c.size;
  }
}

// A03（含 #1 修订的 32761 边界）
TEST(WAL, BlockTailPadding) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("000001.log");

  // 让 block_offset 恰好落在 32761..32767：每个尺寸都造一次「下一条 record 触发 padding」的局面
  for (size_t leftover = kWALHeaderSize; leftover < kWALHeaderSize + 6; ++leftover) {
    WALWriter w(env, path);
    ASSERT_TRUE(w.Open(false).ok());
    // 第一条：header(7) + payload 使 block_offset = kWALBlockSize - leftover
    const size_t first_payload = kWALBlockSize - leftover - kWALHeaderSize;
    ASSERT_TRUE(w.Append(Slice(Repeat('A', first_payload))).ok());
    EXPECT_EQ(kWALBlockSize - leftover, w.block_offset()) << "leftover=" << leftover;
    // 第二条：必须补 padding 后另起一块，绝不能写出 length == 0 的片段
    const std::string second = Repeat('B', 100);
    ASSERT_TRUE(w.Append(Slice(second)).ok()) << "leftover=" << leftover;
    ASSERT_TRUE(w.Sync().ok());
    ASSERT_TRUE(w.Close().ok());

    const std::string raw = ReadFile(env, path);
    // 逐片段检查：任何片段的 length 都必须 >= 1（padding 只能是全 0 哨兵）
    size_t off = 0;
    int fragments = 0;
    while (off + kWALHeaderSize <= raw.size()) {
      const std::string body = raw.substr(off, kWALHeaderSize);
      const uint32_t crc = DecodeFixed32(body.data());
      const uint32_t len = static_cast<uint32_t>(static_cast<unsigned char>(body[4])) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(body[5])) << 8);
      const int type = static_cast<unsigned char>(body[6]);
      if (crc == 0 && len == 0 && type == 0) break;   // padding 起点
      EXPECT_GE(len, 1u) << "leftover=" << leftover << " 处出现了 length=0 的片段";
      EXPECT_LE(len, kWALMaxPayload);
      off += kWALHeaderSize + len;
      ++fragments;
    }
    EXPECT_GE(fragments, 2) << "leftover=" << leftover;

    const ScanOut got = Scan(env, path);
    ASSERT_EQ(2u, got.records.size()) << "leftover=" << leftover;
    EXPECT_EQ(Repeat('A', first_payload), got.records[0]);
    EXPECT_EQ(second, got.records[1]);
    EXPECT_EQ(WALScanVerdict::kClean, got.result.verdict) << "leftover=" << leftover;
  }
}

// A04
TEST(WAL, CrcDetectsSingleByteFlip) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("000001.log");
  const std::string scratch_path = dir.File("mutated.log");

  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(false).ok());
  std::vector<std::string> want;
  for (int i = 0; i < 4; ++i) {
    want.push_back(Repeat(static_cast<char>('0' + i), 64));
    ASSERT_TRUE(w.Append(Slice(want.back())).ok());
  }
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());
  const std::string base = ReadFile(env, path);
  ASSERT_FALSE(base.empty());

  // 覆盖 header 的四个字段区（crc/len/type）与 payload；每个采样点翻转每一位
  std::vector<size_t> positions;
  for (size_t i = 0; i < 7; ++i) positions.push_back(i);          // 第一条的 header
  for (size_t i = 7; i < 20; ++i) positions.push_back(i);          // 第一条的 payload 头部
  positions.push_back(base.size() / 2);
  positions.push_back(base.size() - 1);

  for (size_t pos : positions) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string mutated = base;
      mutated[pos] = static_cast<char>(static_cast<unsigned char>(mutated[pos]) ^ (1u << bit));
      WriteFile(env, scratch_path, mutated);
      const ScanOut got = Scan(env, scratch_path);
      // 判据：不得"看起来完全正常"——要么 verdict 非 CLEAN，要么记录内容已变
      const bool looks_clean = got.status.ok() && got.result.verdict == WALScanVerdict::kClean &&
                               got.records == want;
      EXPECT_FALSE(looks_clean) << "pos=" << pos << " bit=" << bit << " 单比特翻转未被检出";
    }
  }
}

// A05
TEST(WAL, RejectsIllegalTypeAndLength) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("bad.log");

  // 手工拼一个片段：CRC 覆盖面 = length(2) || type(1) || payload（docs/protocol.md §9.3）
  auto append_header = [&](const std::string& p, uint32_t len, int type, const std::string& payload) {
    std::string out = ReadFile(env, p);
    const char lenb[2] = {static_cast<char>(len & 0xff), static_cast<char>((len >> 8) & 0xff)};
    const char prefix[3] = {lenb[0], lenb[1], static_cast<char>(type)};
    const uint32_t crc = crc32c::Extend(crc32c::Value(prefix, 3), payload.data(), payload.size());
    PutFixed32(&out, crc);
    out.append(lenb, 2);
    out.push_back(static_cast<char>(type));
    out.append(payload);
    WriteFile(env, p, out);
  };

  const int bad_types[] = {0, 5, 0x7f, 0xff};
  for (int t : bad_types) {
    WriteFile(env, path, std::string());
    append_header(path, 4, t, "abcd");
    const ScanOut got = Scan(env, path);
    EXPECT_EQ(WALScanVerdict::kParseFail, got.result.verdict) << "type=" << t;
    EXPECT_TRUE(got.records.empty()) << "type=" << t;
  }
  {
    WriteFile(env, path, std::string());
    append_header(path, 0, kFullType, "");          // length == 0 非法
    EXPECT_EQ(WALScanVerdict::kParseFail, Scan(env, path).result.verdict);
  }
  {
    WriteFile(env, path, std::string());
    append_header(path, static_cast<uint32_t>(kWALMaxPayload) + 1, kFullType, Repeat('z', 8));
    EXPECT_EQ(WALScanVerdict::kParseFail, Scan(env, path).result.verdict);
  }
}

// A06
TEST(WAL, TruncatedTailIsCut) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string full = dir.File("full.log");
  const std::string cut = dir.File("cut.log");

  WALWriter w(env, full);
  ASSERT_TRUE(w.Open(false).ok());
  std::vector<std::string> want;
  for (int i = 0; i < 3; ++i) {
    want.push_back(Repeat(static_cast<char>('a' + i), 100 * static_cast<size_t>(i + 1)));
    ASSERT_TRUE(w.Append(Slice(want.back())).ok());
  }
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());
  const std::string base = ReadFile(env, full);
  ASSERT_GT(base.size(), 400u);

  // record 边界（由编码规则推出：header 7 + payload）
  std::vector<size_t> bounds;
  size_t off = 0;
  for (size_t i = 0; i < want.size(); ++i) {
    off += kWALHeaderSize + want[i].size();
    bounds.push_back(off);
  }
  ASSERT_EQ(base.size(), bounds.back());

  for (size_t len = 0; len <= base.size(); ++len) {
    WriteFile(env, cut, base.substr(0, len));
    const ScanOut got = Scan(env, cut);
    ASSERT_TRUE(got.status.ok()) << "len=" << len << " " << got.status.ToString();
    ASSERT_TRUE(got.result.verdict == WALScanVerdict::kClean ||
                got.result.verdict == WALScanVerdict::kTailResidue)
        << "len=" << len;
    // 截断点要么正好在 record 边界（CLEAN），要么必须回退到某个边界且只保留完好前缀
    size_t expect_boundary = 0;
    size_t expect_count = 0;
    for (size_t i = 0; i < bounds.size(); ++i) {
      if (bounds[i] <= len) {
        expect_boundary = bounds[i];
        expect_count = i + 1;
      }
    }
    EXPECT_EQ(expect_boundary, got.result.last_good_end) << "len=" << len;
    EXPECT_EQ(expect_count, got.records.size()) << "len=" << len;
    for (size_t i = 0; i < got.records.size(); ++i) {
      EXPECT_EQ(want[i], got.records[i]) << "len=" << len;
    }
    if (len == base.size()) EXPECT_EQ(WALScanVerdict::kClean, got.result.verdict);
  }
}

// A07
TEST(WAL, MiddleCorruptionRejected) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("mid.log");

  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(false).ok());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(w.Append(Slice(Repeat('m', 64))).ok());
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());

  std::string mutated = ReadFile(env, path);
  mutated[kWALHeaderSize + 3] = static_cast<char>(mutated[kWALHeaderSize + 3] ^ 0x01);  // 第一条 payload
  WriteFile(env, path, mutated);

  const ScanOut got = Scan(env, path);
  EXPECT_EQ(WALScanVerdict::kParseFail, got.result.verdict);
  EXPECT_TRUE(got.result.valid_record_after_failure) << "其后仍有完好 record ⇒ 属中间损坏（必须拒绝启动）";
  EXPECT_NE(0u, got.result.failure_offset);
  EXPECT_FALSE(got.result.detail.empty()) << "必须带可定位信息";
}

// A08
TEST(WAL, TailCorruptionWithNoValidRecordAfterIsCut) {
  TempDir dir("lsm_wal_");
  Env* env = Env::Default();
  const std::string path = dir.File("tail.log");

  WALWriter w(env, path);
  ASSERT_TRUE(w.Open(false).ok());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(w.Append(Slice(Repeat('t', 64))).ok());
  ASSERT_TRUE(w.Sync().ok());
  ASSERT_TRUE(w.Close().ok());

  const size_t third_off = 2 * (kWALHeaderSize + 64);
  std::string mutated = ReadFile(env, path);
  mutated[third_off + kWALHeaderSize] = static_cast<char>(mutated[third_off + kWALHeaderSize] ^ 0x80);
  WriteFile(env, path, mutated);

  const ScanOut got = Scan(env, path);
  EXPECT_EQ(WALScanVerdict::kParseFail, got.result.verdict);
  EXPECT_FALSE(got.result.valid_record_after_failure) << "其后没有完好 record ⇒ 属尾部损坏（可安全截断）";
  EXPECT_EQ(third_off, got.result.last_good_end) << "只能保留前两条";
  EXPECT_EQ(2u, got.records.size());
}

// A09
TEST(WAL, ShortWriteIsRetriedThenFails) {
  TempDir dir("lsm_wal_");
  FaultyEnv env(Env::Default());
  const std::string path = dir.File("short.log");

  env.SetShortWrite(1);   // 底层每次只接受 1 字节
  {
    WALWriter w(&env, path);
    ASSERT_TRUE(w.Open(false).ok());
    const std::string rec = Repeat('s', 500);
    ASSERT_TRUE(w.Append(Slice(rec)).ok()) << "短写必须被正确重试/处理，而不是丢失数据";
    ASSERT_TRUE(w.Sync().ok());
    ASSERT_TRUE(w.Close().ok());
    const ScanOut got = Scan(&env, path);
    ASSERT_EQ(1u, got.records.size());
    EXPECT_EQ(rec, got.records[0]);
  }
  env.SetShortWrite(0);
  {
    env.SetEnospc(true);
    WALWriter w(&env, dir.File("enospc.log"));
    ASSERT_TRUE(w.Open(false).ok());
    const Status s = w.Append(Slice(Repeat('e', 100)));
    EXPECT_FALSE(s.ok()) << "ENOSPC 下绝不返回 kOk";
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
  }
}

// A10
TEST(WAL, FsyncFailurePropagates) {
  TempDir dir("lsm_wal_");
  FaultyEnv env(Env::Default());
  const std::string path = dir.File("fsync.log");

  WALWriter w(&env, path);
  ASSERT_TRUE(w.Open(false).ok());
  ASSERT_TRUE(w.Append(Slice(Repeat('f', 50))).ok());
  env.SetSyncFailureAfter(1);                       // 下一次 Sync 开始失败
  const Status s = w.Sync();
  EXPECT_FALSE(s.ok()) << "fsync 失败绝不能返回 kOk";
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  // 失败后必须粘性：不静默恢复
  const Status again = w.Sync();
  EXPECT_FALSE(again.ok()) << "fsync 失败必须是粘性的（fail-stop）";
  EXPECT_GE(env.sync_calls(), 2);
  w.Close();
}

}  // namespace lsm
