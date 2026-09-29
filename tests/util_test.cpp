// tests/util_test.cpp —— M1 A 组（确定性）util 层用例
//
// 覆盖：Slice / Status / coding（fixed + varint + length-prefixed）/ CRC32C / Arena / 最小 Env /
//       内部 key 编解码与比较 / MemTableKeyComparator 全序。
// 用例名逐字对应 docs/m1-design.md §11 A 组（git 提交 #2 的对账口径）。
//
// 命名空间契约：全项目位于 namespace lsm（依据 design §13 的「undefined reference to lsm::...」）。
// 未冻结签名（design §4/§5 未列出，本文件即契约，M1.1/M1.2 必须匹配）：
//   - crc32c::Value(data,n) / crc32c::Extend(init_crc,data,n)：protocol §5 只给了语义（初始值 0xFFFFFFFF、
//     结果异或 0xFFFFFFFF、Extend 支持增量），命名按 LevelDB 惯例；
//   - GetVarint32/64 采用「Slice* input 前进式」签名，失败时 *value 不得被修改；
//   - GetLengthPrefixedSlice 失败时不得修改 *result；
//   - MemTableKeyComparator(const InternalKeyComparator*)：prerequisites §2 L6「只持不可变指针」，
//     比较对象是**整条 MemTable 条目**（protocol §7）。

#include "test_harness.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "util/coding.h"
#include "util/crc32c.h"
#include "util/env.h"

using namespace lsm;
using namespace lsm::test;

namespace {

// Status 码值已由 design §4.2 冻结（一次冻结，避免后续阶段改枚举）。
static_assert(Status::kOk == 0, "Status::kOk 必须为 0（design §4.2）");
static_assert(Status::kNotFound == 1, "Status::kNotFound 必须为 1（design §4.2）");
static_assert(Status::kCorruption == 2, "Status::kCorruption 必须为 2（design §4.2）");
static_assert(Status::kNotSupported == 3, "Status::kNotSupported 必须为 3（design §4.2）");
static_assert(Status::kInvalidArgument == 4, "Status::kInvalidArgument 必须为 4（design §4.2）");
static_assert(Status::kIOError == 5, "Status::kIOError 必须为 5（design §4.2）");
static_assert(Status::kFrozen == 6, "Status::kFrozen 必须为 6（design §4.2）");

struct StatusCase {
  Status status;
  Status::Code code;
  const char* name;
  bool (Status::*is_code)() const;
};

const StatusCase kStatusCases[] = {
    {Status::NotFound("m1", "m2"), Status::kNotFound, "NotFound", &Status::IsNotFound},
    {Status::Corruption("m1", "m2"), Status::kCorruption, "Corruption", &Status::IsCorruption},
    {Status::NotSupported("m1", "m2"), Status::kNotSupported, "NotSupported",
     &Status::IsNotSupported},
    {Status::InvalidArgument("m1", "m2"), Status::kInvalidArgument, "InvalidArgument",
     &Status::IsInvalidArgument},
    {Status::IOError("m1", "m2"), Status::kIOError, "IOError", &Status::IsIOError},
    {Status::Frozen("m1", "m2"), Status::kFrozen, "Frozen", &Status::IsFrozen},
};

// 读回整个文件：Read 可能少于请求量，循环到返回空 Slice（EOF 不是错误）。
// 返回 void + 出参：ASSERT_* 只能出现在 void 函数里。
void ReadWholeFile(Env* env, const std::string& fname, std::string* out) {
  SequentialFile* raw = nullptr;
  const Status s = env->NewSequentialFile(fname, &raw);
  ASSERT_TRUE(s.ok()) << "NewSequentialFile(" << fname << ") 失败：" << s.ToString();
  ASSERT_TRUE(raw != nullptr);
  std::unique_ptr<SequentialFile> file(raw);
  out->clear();
  char scratch[512];
  while (true) {
    Slice piece;
    const Status rs = file->Read(sizeof(scratch), &piece, scratch);
    ASSERT_TRUE(rs.ok()) << "Read 失败：" << rs.ToString();
    ASSERT_LE(piece.size(), sizeof(scratch)) << "Read 返回量超过请求量";
    if (piece.empty()) break;
    out->append(piece.data(), piece.size());
  }
}

// 比较器必须是严格全序：反对称 + 传递（protocol §6.1 要求；O(n^2)/O(n^3) 在 n<=24 时可接受）。
void ExpectStrictTotalOrder(const Comparator& cmp, const std::vector<std::string>& keys,
                            const char* what) {
  const size_t n = keys.size();
  ASSERT_GT(n, 0u);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_EQ(0, cmp.Compare(keys[i], keys[i])) << what << ": Compare(x,x) 必须为 0";
    for (size_t j = 0; j < n; ++j) {
      const int ij = cmp.Compare(keys[i], keys[j]);
      const int ji = cmp.Compare(keys[j], keys[i]);
      EXPECT_EQ((ij > 0) - (ij < 0), -((ji > 0) - (ji < 0)))
          << what << ": 反对称性被破坏，i=" << i << " j=" << j;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (cmp.Compare(keys[i], keys[j]) >= 0) continue;
      for (size_t k = 0; k < n; ++k) {
        if (cmp.Compare(keys[j], keys[k]) >= 0) continue;
        EXPECT_LT(cmp.Compare(keys[i], keys[k]), 0)
            << what << ": 传递性被破坏，i=" << i << " j=" << j << " k=" << k;
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Slice
// ---------------------------------------------------------------------------
TEST(Slice, CompareAndStartsWith) {
  const Slice empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(0u, empty.size());
  EXPECT_EQ(0, empty.compare(Slice()));

  EXPECT_EQ(0, Slice("abc").compare(Slice("abc")));
  EXPECT_LT(Slice("abc").compare(Slice("abd")), 0);
  EXPECT_GT(Slice("abd").compare(Slice("abc")), 0);
  EXPECT_LT(Slice("abc").compare(Slice("abca")), 0);  // 短者为小
  EXPECT_GT(Slice("abca").compare(Slice("abc")), 0);
  EXPECT_LT(empty.compare(Slice("a")), 0);            // 空 Slice 是任何非空的前缀 → 小
  EXPECT_GT(Slice("a").compare(empty), 0);

  // 逐字节**无符号**比较：'\x80' > 'a'。若实现按有符号 char 比较（x86 上 char 有符号），这条会失败。
  EXPECT_GT(Slice("\x80", 1).compare(Slice("a")), 0);
  EXPECT_LT(Slice("a").compare(Slice("\x80", 1)), 0);
  EXPECT_GT(Slice("\xFF", 1).compare(Slice("\x7F", 1)), 0);

  // 内嵌 NUL：内容由长度决定，不受 NUL 截断影响
  const char raw[] = {'a', '\0', 'b'};
  EXPECT_EQ(3u, Slice(raw, 3).size());
  EXPECT_EQ(0, Slice(raw, 3).compare(Slice(raw, 3)));
  EXPECT_NE(0, Slice(raw, 3).compare(Slice("a")));

  // std::string 来源
  const std::string s = "abc";
  EXPECT_EQ(0, Slice(s).compare(Slice("abc")));

  // operator[] / clear
  const Slice xyz("xyz");
  EXPECT_EQ('x', xyz[0]);
  EXPECT_EQ('z', xyz[2]);
  Slice mutable_view("xyz");
  mutable_view.clear();
  EXPECT_TRUE(mutable_view.empty());
  EXPECT_EQ(0u, mutable_view.size());

  // starts_with 边界
  EXPECT_TRUE(Slice("abcd").starts_with(Slice("abc")));
  EXPECT_TRUE(Slice("abcd").starts_with(Slice("abcd")));
  EXPECT_TRUE(Slice("abcd").starts_with(empty));
  EXPECT_TRUE(Slice("abcd").starts_with(Slice("")));
  EXPECT_TRUE(empty.starts_with(empty));
  EXPECT_FALSE(empty.starts_with(Slice("a")));
  EXPECT_FALSE(Slice("abc").starts_with(Slice("abcd")));
  EXPECT_FALSE(Slice("abc").starts_with(Slice("abd")));
  EXPECT_FALSE(Slice("abc").starts_with(Slice("b")));
}

TEST(Slice, ToString) {
  EXPECT_EQ("", Slice().ToString());
  EXPECT_EQ("", Slice("").ToString());
  EXPECT_EQ("abc", Slice("abc").ToString());

  const char raw[] = {'a', '\0', 'b'};
  const std::string with_nul = Slice(raw, 3).ToString();
  EXPECT_EQ(3u, with_nul.size());
  EXPECT_EQ(std::string(raw, 3), with_nul);

  // ToString 是唯一的显式拷贝出口（design §4.1 生命周期契约）：
  // 拷贝与源解耦；而视图本身跟随源 —— 这正是「跨调用边界必须 ToString」的原因。
  std::string src = "hello";
  const Slice view(src);
  const std::string taken = view.ToString();
  src.assign("HELLO");
  EXPECT_EQ("hello", taken);
  EXPECT_EQ("HELLO", view.ToString());
  EXPECT_EQ(5u, view.size());
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
TEST(Status, CodesAndToString) {
  const Status ok;
  EXPECT_TRUE(ok.ok());
  EXPECT_EQ(Status::kOk, ok.code());
  EXPECT_EQ("OK", ok.ToString());
  EXPECT_TRUE(Status::OK().ok());
  EXPECT_EQ(Status::kOk, Status::OK().code());
  EXPECT_FALSE(ok.IsNotFound());
  EXPECT_FALSE(ok.IsCorruption());
  EXPECT_FALSE(ok.IsNotSupported());
  EXPECT_FALSE(ok.IsInvalidArgument());
  EXPECT_FALSE(ok.IsIOError());
  EXPECT_FALSE(ok.IsFrozen());

  bool (Status::*others[])() const = {&Status::IsNotFound,   &Status::IsCorruption,
                                           &Status::IsNotSupported, &Status::IsInvalidArgument,
                                           &Status::IsIOError,     &Status::IsFrozen};
  for (const StatusCase& c : kStatusCases) {
    EXPECT_FALSE(c.status.ok()) << c.name;
    EXPECT_EQ(c.code, c.status.code()) << c.name;
    EXPECT_TRUE((c.status.*c.is_code)()) << c.name;
    for (bool (Status::*fn)() const : others) {
      if (fn == c.is_code) continue;
      EXPECT_FALSE((c.status.*fn)()) << c.name << ": 其它 IsXxx 必须为假";
    }
    const std::string text = c.status.ToString();
    EXPECT_NE(std::string::npos, text.find(c.name)) << "ToString 必须含码名：" << text;
    EXPECT_NE(std::string::npos, text.find("m1")) << "ToString 必须含 message：" << text;
    EXPECT_NE(std::string::npos, text.find("m2")) << "ToString 必须含 msg2：" << text;
    EXPECT_LT(text.find(c.name), text.find("m1")) << "顺序必须是 <CodeName>: <msg>：" << text;
    EXPECT_LT(text.find("m1"), text.find("m2")) << "顺序必须是 <msg>: <msg2>：" << text;
  }

  // 省略 msg2 与显式空 Slice 等价（否则日志里会出现多余的分隔符）
  EXPECT_EQ(Status::InvalidArgument("x").ToString(), Status::InvalidArgument("x", "").ToString());
  EXPECT_EQ(Status::Frozen("buf full").ToString(), Status::Frozen("buf full", Slice()).ToString());
  // 空 message 也必须可读
  EXPECT_FALSE(Status::IOError("").ToString().empty());
  EXPECT_EQ("Corruption: ", Status::Corruption("").ToString());
}

// ---------------------------------------------------------------------------
// coding：fixed / varint / length-prefixed（docs/protocol.md §2~§4）
// ---------------------------------------------------------------------------
TEST(Coding, Fixed32RoundTrip) {
  const uint32_t values[] = {0u,          1u,          127u,       128u,
                             255u,       256u,        65535u,     65536u,
                             0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};
  for (uint32_t v : values) {
    std::string dst;
    PutFixed32(&dst, v);
    ASSERT_EQ(4u, dst.size()) << "v=" << v;
    EXPECT_EQ(v, DecodeFixed32(dst.data())) << "v=" << v;
  }

  // 小端（protocol §3）：0x01020304 → 04 03 02 01
  std::string le;
  PutFixed32(&le, 0x01020304u);
  ASSERT_EQ(4u, le.size());
  EXPECT_EQ(0x04, static_cast<unsigned char>(le[0]));
  EXPECT_EQ(0x03, static_cast<unsigned char>(le[1]));
  EXPECT_EQ(0x02, static_cast<unsigned char>(le[2]));
  EXPECT_EQ(0x01, static_cast<unsigned char>(le[3]));

  // 追加语义：连续 Put 不得覆盖
  std::string two;
  PutFixed32(&two, 1u);
  PutFixed32(&two, 2u);
  ASSERT_EQ(8u, two.size());
  EXPECT_EQ(1u, DecodeFixed32(two.data()));
  EXPECT_EQ(2u, DecodeFixed32(two.data() + 4));
}

TEST(Coding, Fixed64RoundTrip) {
  const uint64_t values[] = {0ull,
                             1ull,
                             127ull,
                             128ull,
                             (1ull << 31) - 1,
                             1ull << 31,
                             0xffffffffull,
                             1ull << 32,
                             (1ull << 63) - 1,
                             1ull << 63,
                             0xfffffffffffffffeull,
                             0xffffffffffffffffull};
  for (uint64_t v : values) {
    std::string dst;
    PutFixed64(&dst, v);
    ASSERT_EQ(8u, dst.size()) << "v=" << v;
    EXPECT_EQ(v, DecodeFixed64(dst.data())) << "v=" << v;
  }

  // 小端（protocol §3）：0x0102030405060708
  std::string le;
  PutFixed64(&le, 0x0102030405060708ull);
  ASSERT_EQ(8u, le.size());
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(0x08 - i), static_cast<unsigned char>(le[static_cast<size_t>(i)]));
  }

  std::string two;
  PutFixed64(&two, 1ull);
  PutFixed64(&two, 2ull);
  ASSERT_EQ(16u, two.size());
  EXPECT_EQ(1ull, DecodeFixed64(two.data()));
  EXPECT_EQ(2ull, DecodeFixed64(two.data() + 8));
}

TEST(Coding, Varint32RoundTrip) {
  const uint32_t values[] = {0u,          1u,          2u,          127u,
                             128u,       300u,        16383u,      16384u,
                             (1u << 21) - 1, 1u << 21, (1u << 28) - 1, 1u << 28,
                             0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};
  const size_t expect_len[] = {1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5};
  ASSERT_EQ(sizeof(values) / sizeof(values[0]), sizeof(expect_len) / sizeof(expect_len[0]));

  size_t idx = 0;
  for (uint32_t v : values) {
    std::string dst;
    PutVarint32(&dst, v);
    ASSERT_LE(dst.size(), 5u) << "varint32 最多 5 字节（protocol §2），v=" << v;
    EXPECT_EQ(expect_len[idx], dst.size()) << "LEB128 编码长度不符，v=" << v;

    Slice input(dst);
    uint32_t got = 0;
    ASSERT_TRUE(GetVarint32(&input, &got)) << "解码失败，v=" << v;
    EXPECT_EQ(v, got);
    EXPECT_TRUE(input.empty()) << "解码后必须消费完，v=" << v;

    // char* 版本必须返回「编码末尾」指针（EncodeVarint32 的返回约定按 LevelDB）
    char buf[5];
    char* const end = EncodeVarint32(buf, v);
    EXPECT_EQ(dst.size(), static_cast<size_t>(end - buf)) << "v=" << v;
    EXPECT_EQ(dst, std::string(buf, static_cast<size_t>(end - buf))) << "v=" << v;
    ++idx;
  }

  // 已知向量（protocol §2 LEB128）
  std::string dst;
  PutVarint32(&dst, 0u);
  EXPECT_EQ(std::string("\x00", 1), dst);
  dst.clear();
  PutVarint32(&dst, 127u);
  EXPECT_EQ(std::string("\x7F", 1), dst);
  dst.clear();
  PutVarint32(&dst, 128u);
  EXPECT_EQ(std::string("\x80\x01", 2), dst);
  dst.clear();
  PutVarint32(&dst, 300u);
  EXPECT_EQ(std::string("\xAC\x02", 2), dst);
  dst.clear();
  PutVarint32(&dst, 0xffffffffu);
  EXPECT_EQ(std::string("\xFF\xFF\xFF\xFF\x0F", 5), dst);
}

TEST(Coding, Varint64RoundTrip) {
  const uint64_t values[] = {0ull,
                             1ull,
                             127ull,
                             128ull,
                             16383ull,
                             16384ull,
                             (1ull << 31) - 1,
                             1ull << 31,
                             0xffffffffull,
                             1ull << 32,
                             (1ull << 63) - 1,
                             1ull << 63,
                             0xfffffffffffffffeull,
                             0xffffffffffffffffull};
  for (uint64_t v : values) {
    std::string dst;
    PutVarint64(&dst, v);
    ASSERT_LE(dst.size(), 10u) << "varint64 最多 10 字节（protocol §2），v=" << v;

    Slice input(dst);
    uint64_t got = 0;
    ASSERT_TRUE(GetVarint64(&input, &got)) << "解码失败，v=" << v;
    EXPECT_EQ(v, got);
    EXPECT_TRUE(input.empty()) << "v=" << v;

    char buf[10];
    char* const end = EncodeVarint64(buf, v);
    EXPECT_EQ(dst.size(), static_cast<size_t>(end - buf)) << "v=" << v;
    EXPECT_EQ(dst, std::string(buf, static_cast<size_t>(end - buf))) << "v=" << v;
  }

  // 已知向量：2^64-1 → 9 个 0xFF + 0x01（10 字节）
  std::string dst;
  PutVarint64(&dst, 0xffffffffffffffffull);
  ASSERT_EQ(10u, dst.size());
  for (size_t i = 0; i < 9; ++i) {
    EXPECT_EQ(0xFF, static_cast<unsigned char>(dst[i]));
  }
  EXPECT_EQ(0x01, static_cast<unsigned char>(dst[9]));
  dst.clear();
  PutVarint64(&dst, 0ull);
  EXPECT_EQ(std::string("\x00", 1), dst);
}

TEST(Coding, VarintRejectsOverflowAndTruncation) {
  const uint32_t sentinel32 = 0xDEADBEEFu;
  const uint64_t sentinel64 = 0xDEADBEEFCAFEBABEull;

  // 上界 2^32-1 合法（第 5 字节高 4 位为 0）
  {
    const char buf[] = "\xFF\xFF\xFF\xFF\x0F";
    Slice s(buf, 5);
    uint32_t v = sentinel32;
    ASSERT_TRUE(GetVarint32(&s, &v));
    EXPECT_EQ(0xffffffffu, v);
  }
  // 第 5 字节高 4 位非 0 → 溢出，必须拒绝且不修改输出（protocol §2）
  {
    const int bad_last[] = {0x10, 0x1F, 0x20, 0x7F, 0x80, 0xFF};
    for (int last : bad_last) {
      const char buf[5] = {'\xFF', '\xFF', '\xFF', '\xFF', static_cast<char>(last)};
      Slice s(buf, 5);
      uint32_t v = sentinel32;
      EXPECT_FALSE(GetVarint32(&s, &v)) << "last=0x" << std::hex << last;
      EXPECT_EQ(sentinel32, v) << "解析失败时不得修改输出";
    }
  }
  // 截断：续位仍在但字节用尽
  {
    const char buf[5] = {'\x80', '\x80', '\x80', '\x80', '\x80'};
    for (int n = 0; n <= 4; ++n) {
      Slice s(buf, static_cast<size_t>(n));
      uint32_t v = sentinel32;
      EXPECT_FALSE(GetVarint32(&s, &v)) << "n=" << n;
      EXPECT_EQ(sentinel32, v) << "n=" << n;
    }
  }
  // 0x80 单独一个字节：需要续字节但已到末尾
  {
    const char buf[] = "\x80";
    Slice s(buf, 1);
    uint32_t v = sentinel32;
    EXPECT_FALSE(GetVarint32(&s, &v));
    EXPECT_EQ(sentinel32, v);
  }

  // varint64：10 字节全续位 → 既截断又溢出，必须拒绝
  {
    const char buf[10] = {'\x80', '\x80', '\x80', '\x80', '\x80',
                          '\x80', '\x80', '\x80', '\x80', '\x80'};
    Slice s(buf, 10);
    uint64_t v = sentinel64;
    EXPECT_FALSE(GetVarint64(&s, &v));
    EXPECT_EQ(sentinel64, v);
  }
  // varint64：11 字节（超长编码）必须拒绝
  {
    const char buf[11] = {'\x80', '\x80', '\x80', '\x80', '\x80', '\x80',
                          '\x80', '\x80', '\x80', '\x80', '\x01'};
    Slice s(buf, 11);
    uint64_t v = sentinel64;
    EXPECT_FALSE(GetVarint64(&s, &v));
    EXPECT_EQ(sentinel64, v);
  }
  // varint64：上界 2^64-1 合法，2^64-1 之外不可表示（10 字节 0xFF 无终止位 → 拒绝）
  {
    const char buf[10] = {'\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
                          '\xFF', '\xFF', '\xFF', '\xFF', '\xFF'};
    Slice s(buf, 10);
    uint64_t v = sentinel64;
    EXPECT_FALSE(GetVarint64(&s, &v));
    EXPECT_EQ(sentinel64, v);
  }
  {
    const char buf[10] = {'\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
                          '\xFF', '\xFF', '\xFF', '\xFF', '\x01'};
    Slice s(buf, 10);
    uint64_t v = 0;
    ASSERT_TRUE(GetVarint64(&s, &v));
    EXPECT_EQ(0xffffffffffffffffull, v);
  }
  // 截断的 varint64（只有 9 个续位字节）
  {
    const char buf[9] = {'\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80', '\x80'};
    Slice s(buf, 9);
    uint64_t v = sentinel64;
    EXPECT_FALSE(GetVarint64(&s, &v));
    EXPECT_EQ(sentinel64, v);
  }
}

TEST(Coding, LengthPrefixedRoundTrip) {
  // 空串：只有 varint32(0)
  {
    std::string dst;
    PutLengthPrefixedSlice(&dst, Slice(""));
    ASSERT_EQ(1u, dst.size());
    EXPECT_EQ(0x00, static_cast<unsigned char>(dst[0]));
    Slice input(dst);
    Slice got("keep");
    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_TRUE(got.empty());
    EXPECT_TRUE(input.empty());
  }
  // 常规 + 64 KiB（protocol §8：value 不单独设上限，64 KiB 是最容易踩界的量级）
  const std::string small = "hello, lsm";
  const std::string big(64 * 1024, 'x');
  const std::string cases[] = {small, big};
  for (const std::string& s : cases) {
    std::string dst;
    PutLengthPrefixedSlice(&dst, Slice(s));
    EXPECT_EQ(dst.size(), s.size() + (s.size() < 128 ? 1u : (s.size() < 16384 ? 2u : 3u)))
        << "布局必须是 varint32(len) || bytes，len=" << s.size();
    Slice input(dst);
    Slice got;
    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ(s, got.ToString());
    EXPECT_TRUE(input.empty());
  }
  // 手工拼字节校验（独立参照）：varint32(3) || "abc"
  {
    std::string manual;
    AppendVarint32Manual(&manual, 3u);
    manual.append("abc");
    std::string enc;
    PutLengthPrefixedSlice(&enc, Slice("abc"));
    EXPECT_EQ(manual, enc);
    // 连续两条：第二条必须从第一条之后开始
    PutLengthPrefixedSlice(&enc, Slice("de"));
    Slice input(enc);
    Slice a, b;
    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &a));
    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &b));
    EXPECT_EQ("abc", a.ToString());
    EXPECT_EQ("de", b.ToString());
    EXPECT_TRUE(input.empty());
  }
  // 长度越界：声明 0xFFFFFFFF 字节
  {
    std::string bad;
    AppendVarint32Manual(&bad, 0xffffffffu);
    bad.append("x");
    Slice input(bad);
    Slice got("keep");
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ("keep", got.ToString()) << "失败时不得修改输出";
  }
  // 截断：声明 10 字节只有 5 字节
  {
    std::string bad;
    AppendVarint32Manual(&bad, 10u);
    bad.append("12345");
    Slice input(bad);
    Slice got("keep");
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ("keep", got.ToString());
  }
  // 只有长度没有内容
  {
    std::string bad;
    AppendVarint32Manual(&bad, 1u);
    Slice input(bad);
    Slice got("keep");
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ("keep", got.ToString());
  }
  // 空输入
  {
    Slice input;
    Slice got("keep");
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ("keep", got.ToString());
  }
  // 长度前缀本身被截断（0x80 无后续字节）
  {
    const char buf[] = "\x80";
    Slice input(buf, 1);
    Slice got("keep");
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &got));
    EXPECT_EQ("keep", got.ToString());
  }
}

// ---------------------------------------------------------------------------
// CRC32C（docs/protocol.md §5）
// ---------------------------------------------------------------------------
TEST(CRC32C, KnownVectors) {
  EXPECT_EQ(0x00000000u, crc32c::Value("", 0));
  EXPECT_EQ(0xE3069283u, crc32c::Value("123456789", 9));

  const char* const fox = "The quick brown fox jumps over the lazy dog";
  EXPECT_EQ(0x22620404u, crc32c::Value(fox, std::strlen(fox)));

  // 必须 Castagnoli，而不是 IEEE CRC-32（0xCBF43926）—— 张冠李戴是实现层最常见的错
  EXPECT_NE(0xCBF43926u, crc32c::Value("123456789", 9));

  // Extend 增量：分片累加 == 整体计算（protocol §5 要求 Extend 支持增量）
  const uint32_t whole = crc32c::Value("123456789", 9);
  EXPECT_EQ(whole, crc32c::Extend(crc32c::Extend(crc32c::Value("", 0), "1234", 4), "56789", 5));
  uint32_t acc = 0;
  for (size_t i = 0; i < 9; ++i) {
    acc = crc32c::Extend(acc, "123456789" + i, 1);
  }
  EXPECT_EQ(whole, acc);
  // Extend(0, ...) 等价 Value(...)
  EXPECT_EQ(whole, crc32c::Extend(0u, "123456789", 9));
  // Extend 长度为 0 不得改变 CRC
  EXPECT_EQ(whole, crc32c::Extend(whole, "", 0));

  // 长度必须参与计算，且不同内容不同 CRC
  EXPECT_NE(crc32c::Value("a", 1), crc32c::Value("ab", 2));
  EXPECT_NE(crc32c::Value("ab", 2), crc32c::Value("ba", 2));
  EXPECT_EQ(crc32c::Value("ab", 2), crc32c::Value("abc", 2));  // 只看前 2 字节
}

TEST(CRC32C, SingleBitFlipDetected) {
  Rng rng(kHarnessSeed + 3);

  // 小数据：逐字节逐位全扫（1+2+3+7+15+64 = 92 字节 × 8 位 = 736 次翻位）
  const size_t lens[] = {1, 2, 3, 7, 15, 64};
  for (size_t len : lens) {
    const std::string data = RandomValue(&rng, static_cast<int>(len));
    ASSERT_EQ(len, data.size());
    const uint32_t base = crc32c::Value(data.data(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
      for (int b = 0; b < 8; ++b) {
        std::string flipped = data;
        flipped[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ (1u << b));
        const uint32_t got = crc32c::Value(flipped.data(), flipped.size());
        ASSERT_NE(base, got) << "len=" << len << " 第 " << i << " 字节第 " << b << " 位翻转未被检出";
      }
    }
  }

  // 4 KiB 缓冲：抽样翻位（全扫 32768 次在 A 组里无必要地慢），步长 37 与缓冲长度互质保证覆盖分散
  {
    const std::string data = RandomValue(&rng, 4096);
    const uint32_t base = crc32c::Value(data.data(), data.size());
    size_t checked = 0;
    for (size_t i = 0; i < data.size(); i += 37) {
      for (int b = 0; b < 8; ++b) {
        std::string flipped = data;
        flipped[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ (1u << b));
        ASSERT_NE(base, crc32c::Value(flipped.data(), flipped.size()))
            << "4096B 缓冲第 " << i << " 字节第 " << b << " 位翻转未被检出";
        ++checked;
      }
    }
    EXPECT_GE(checked, 800u);
  }
}

// ---------------------------------------------------------------------------
// Arena（design §6）
// ---------------------------------------------------------------------------
TEST(Arena, AlignmentAndUsage) {
  const size_t kAlign = alignof(std::max_align_t);
  Arena arena;
  EXPECT_EQ(0u, arena.MemoryUsage()) << "新建 Arena 尚未申请任何块（design §6：Reset 后归 0，即空态为 0）";

  // 含 > kBlockSize/4 (=1024) 的大块路径
  const size_t sizes[] = {1, 7, 8, 15, 16, 100, 511, 1023, 1024, 1025, 4095, 4096, 4097, 8192};
  std::vector<std::pair<char*, size_t>> allocs;
  size_t prev_usage = arena.MemoryUsage();
  size_t requested = 0;
  for (size_t n : sizes) {
    char* p = arena.Allocate(n);
    ASSERT_TRUE(p != nullptr) << "Allocate(" << n << ") 返回空指针";
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign)
        << "Allocate(" << n << ") 未按 alignof(max_align_t)=" << kAlign << " 对齐";
    std::memset(p, 0xAB, n);  // 越界写会被 ASan 抓到
    allocs.emplace_back(p, n);
    requested += n;
    EXPECT_GE(arena.MemoryUsage(), prev_usage) << "MemoryUsage 必须单调不减（design §6）";
    prev_usage = arena.MemoryUsage();
  }
  EXPECT_GE(arena.MemoryUsage(), requested) << "MemoryUsage 至少覆盖全部请求字节";

  // 分配区间互不重叠（重复交出同一块是最典型的 Arena bug）
  std::vector<std::pair<uintptr_t, size_t>> ranges;
  for (const std::pair<char*, size_t>& a : allocs) {
    ranges.emplace_back(reinterpret_cast<uintptr_t>(a.first), a.second);
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    EXPECT_LE(ranges[i - 1].first + ranges[i - 1].second, ranges[i].first)
        << "第 " << i << " 次分配与前一次重叠";
  }
  // 写入内容不得被后续分配改写
  for (const std::pair<char*, size_t>& a : allocs) {
    for (size_t i = 0; i < a.second; ++i) {
      ASSERT_EQ(static_cast<char>(0xAB), a.first[i]) << "已分配区域被后续分配覆盖";
    }
  }

  // 显式对齐分配
  const size_t aligns[] = {8, 16, 32, 64, 128, 256};
  const size_t aligned_sizes[] = {1, 17, 65, 129, 1000};
  for (size_t align : aligns) {
    for (size_t n : aligned_sizes) {
      char* p = arena.AllocateAligned(n, align);
      ASSERT_TRUE(p != nullptr);
      EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % align)
          << "AllocateAligned(" << n << ", " << align << ") 未满足请求对齐";
      std::memset(p, 0xCD, n);
      EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign);
    }
  }

  // 默认参数等价 alignof(max_align_t)
  {
    char* p = arena.AllocateAligned(33);
    ASSERT_TRUE(p != nullptr);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign);
  }

  // 大块单独分配：不吃小块剩余空间。1 个小分配后紧跟 1025 字节请求，
  // MemoryUsage 增量必须 >= 1025（更大的块单独申请，design §6）
  {
    Arena big_arena;
    big_arena.Allocate(1);
    const size_t before = big_arena.MemoryUsage();
    char* p = big_arena.Allocate(1025);  // > kBlockSize(4096)/4 = 1024 → 走大块路径
    ASSERT_TRUE(p != nullptr);
    std::memset(p, 0x5A, 1025);
    EXPECT_GE(big_arena.MemoryUsage() - before, 1025u);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign);
  }
}

TEST(Arena, Reset) {
  Arena arena;
  std::vector<std::pair<char*, size_t>> allocs;
  for (int i = 0; i < 100; ++i) {
    const size_t n = static_cast<size_t>(i) * 37 + 1;
    char* p = arena.Allocate(n);
    ASSERT_TRUE(p != nullptr);
    std::memset(p, static_cast<int>(i & 0x7F), n);
    allocs.emplace_back(p, n);
  }
  const size_t usage_before = arena.MemoryUsage();
  ASSERT_GT(usage_before, 0u) << "100 次分配后 MemoryUsage 必须 > 0";

  arena.Reset();
  EXPECT_EQ(0u, arena.MemoryUsage()) << "Reset 必须丢弃全部块（design §6）";
  // Reset 之后原指针全部失效（悬垂），此处刻意不再触碰 allocs 里的指针 ——
  // 一旦实现「只重置指针不释放」或「释放后仍被引用」，ASan 会在后续分配里抓到。

  char* p = arena.Allocate(64);
  ASSERT_TRUE(p != nullptr);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t));
  std::memset(p, 0x5A, 64);
  EXPECT_GT(arena.MemoryUsage(), 0u) << "Reset 之后必须能继续分配";

  // 反复 Reset 幂等
  for (int i = 0; i < 3; ++i) {
    arena.Allocate(4096);
    arena.Reset();
    EXPECT_EQ(0u, arena.MemoryUsage());
  }
  allocs.clear();
}

// ---------------------------------------------------------------------------
// 最小 Env（design §10）
// ---------------------------------------------------------------------------
TEST(Env, TimeMonotonic) {
  Env* env = Env::Default();
  ASSERT_TRUE(env != nullptr);

  const uint64_t t0 = env->NowMicros();
  uint64_t prev = t0;
  for (int i = 0; i < 1000; ++i) {
    const uint64_t now = env->NowMicros();
    EXPECT_GE(now, prev) << "NowMicros 必须单调（design §10 要求 CLOCK_MONOTONIC，非 CLOCK_REALTIME）";
    prev = now;
  }

  env->SleepForMicros(1000);  // 1 ms
  const uint64_t t1 = env->NowMicros();
  EXPECT_GE(t1, t0);
  EXPECT_GE(t1 - t0, 1000u) << "SleepForMicros(1000) 实测睡眠不足 1 ms";
  EXPECT_LT(t1 - t0, 10ull * 1000 * 1000) << "1 ms 睡眠不该出现秒级偏差（单位是不是微秒？）";

  // 睡眠 0 不得阻塞
  const uint64_t t2 = env->NowMicros();
  env->SleepForMicros(0);
  EXPECT_GE(env->NowMicros(), t2);
}

TEST(Env, FileRoundTrip) {
  Env* env = Env::Default();
  ASSERT_TRUE(env != nullptr);
  TempDir dir("lsm_env_");
  const std::string fname = dir.File("data.txt");
  EXPECT_FALSE(env->FileExists(fname));

  // ---- 失败路径必须返回 kIOError 且带路径上下文（design §4.2：M1 的 kIOError 产生点） ----
  {
    SequentialFile* raw = nullptr;
    const Status s = env->NewSequentialFile(fname, &raw);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsIOError()) << "打开不存在的文件必须是 kIOError：" << s.ToString();
    EXPECT_EQ(nullptr, raw);
    EXPECT_NE(std::string::npos, s.ToString().find(fname)) << "错误必须带路径上下文：" << s.ToString();
  }
  {
    uint64_t size = 12345;
    const Status s = env->GetFileSize(fname, &size);
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
    EXPECT_NE(std::string::npos, s.ToString().find(fname)) << s.ToString();
  }
  {
    const Status s = env->DeleteFile(fname);
    EXPECT_TRUE(s.IsIOError()) << s.ToString();
  }
  {
    const Status s = env->RenameFile(fname, dir.File("target.txt"));
    EXPECT_TRUE(s.IsIOError()) << "源文件不存在 → kIOError：" << s.ToString();
  }
  {
    WritableFile* raw = nullptr;
    const Status s = env->NewWritableFile(dir.SubDir("no_such_dir") + "/x.txt", &raw);
    EXPECT_TRUE(s.IsIOError()) << "父目录不存在 → kIOError：" << s.ToString();
    EXPECT_EQ(nullptr, raw);
  }

  // ---- 正向：Append（多次）→ Flush → Sync → Close ----
  const std::string part1("hello lsm env\n", 14);
  std::string payload = part1;
  payload.push_back('\0');                      // 内嵌 NUL 必须原样落盘/读回
  payload.append(1, static_cast<char>(0xFF));   // 非 ASCII 字节
  payload.append("tail");
  {
    WritableFile* raw = nullptr;
    const Status s = env->NewWritableFile(fname, &raw);
    ASSERT_TRUE(s.ok()) << s.ToString();
    ASSERT_TRUE(raw != nullptr);
    std::unique_ptr<WritableFile> file(raw);
    ASSERT_TRUE(file->Append(Slice(part1)).ok());
    ASSERT_TRUE(file->Append(Slice(payload.data() + part1.size(), payload.size() - part1.size())).ok());
    ASSERT_TRUE(file->Append(Slice("")).ok()) << "空 Append 必须成功";
    ASSERT_TRUE(file->Flush().ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }
  EXPECT_TRUE(env->FileExists(fname));

  uint64_t size = 0;
  ASSERT_TRUE(env->GetFileSize(fname, &size).ok());
  EXPECT_EQ(payload.size(), size) << "多次 Append 必须是顺序拼接";

  std::string got;
  ReadWholeFile(env, fname, &got);
  EXPECT_EQ(payload, got);
  EXPECT_EQ(payload.size(), got.size());

  // ---- Skip ----
  {
    SequentialFile* raw = nullptr;
    ASSERT_TRUE(env->NewSequentialFile(fname, &raw).ok());
    ASSERT_TRUE(raw != nullptr);
    std::unique_ptr<SequentialFile> file(raw);
    ASSERT_TRUE(file->Skip(6).ok());
    std::string rest;
    char scratch[64];
    while (true) {
      Slice piece;
      ASSERT_TRUE(file->Read(sizeof(scratch), &piece, scratch).ok());
      if (piece.empty()) break;
      rest.append(piece.data(), piece.size());
    }
    EXPECT_EQ(payload.substr(6), rest);
  }
  // ---- 读到 EOF：返回空 Slice 且状态 OK（空文件/EOF 不是错误） ----
  {
    SequentialFile* raw = nullptr;
    ASSERT_TRUE(env->NewSequentialFile(fname, &raw).ok());
    std::unique_ptr<SequentialFile> file(raw);
    ASSERT_TRUE(file->Skip(static_cast<uint64_t>(payload.size())).ok());
    Slice piece;
    char scratch[16];
    const Status s = file->Read(sizeof(scratch), &piece, scratch);
    EXPECT_TRUE(s.ok()) << s.ToString();
    EXPECT_TRUE(piece.empty());
  }

  // ---- Rename ----
  const std::string renamed = dir.File("renamed.txt");
  ASSERT_TRUE(env->RenameFile(fname, renamed).ok());
  EXPECT_FALSE(env->FileExists(fname));
  EXPECT_TRUE(env->FileExists(renamed));
  got.clear();
  ReadWholeFile(env, renamed, &got);
  EXPECT_EQ(payload, got);

  // ---- CreateDir + 写入子目录 ----
  const std::string sub = dir.SubDir("sub");
  ASSERT_TRUE(env->CreateDir(sub).ok());
  const std::string inside = sub + "/inner.txt";
  {
    WritableFile* raw = nullptr;
    ASSERT_TRUE(env->NewWritableFile(inside, &raw).ok());
    ASSERT_TRUE(raw != nullptr);
    std::unique_ptr<WritableFile> file(raw);
    ASSERT_TRUE(file->Append(Slice("inner")).ok());
    ASSERT_TRUE(file->Close().ok());
  }
  EXPECT_TRUE(env->FileExists(inside));
  got.clear();
  ReadWholeFile(env, inside, &got);
  EXPECT_EQ("inner", got);

  // ---- DeleteFile ----
  ASSERT_TRUE(env->DeleteFile(renamed).ok());
  EXPECT_FALSE(env->FileExists(renamed));
  {
    const Status s = env->DeleteFile(renamed);
    EXPECT_TRUE(s.IsIOError()) << "重复删除必须是 kIOError：" << s.ToString();
  }
}

// ---------------------------------------------------------------------------
// 内部 key（docs/protocol.md §6）
// ---------------------------------------------------------------------------
TEST(InternalKey, BuildParseRoundTrip) {
  static_assert(kInternalKeyTrailerSize == 8, "trailer 固定 8 字节（protocol §6）");
  static_assert(kInternalKeyMinSize == 8, "internal key 最小 8 字节（protocol §6）");
  EXPECT_EQ((1ull << 56) - 1, kMaxSequenceNumber);
  EXPECT_EQ(64u * 1024u, kMaxUserKeySize);

  Rng rng(kHarnessSeed + 11);
  // 上限含端点：kMaxUserKeySize（64 KiB）合法（prerequisites §5）
  const size_t lens[] = {1, 2, 7, 8, 9, 63, 64, 100, 4096, kMaxUserKeySize};
  const SequenceNumber seqs[] = {0, 1, 42, kMaxSequenceNumber};
  const ValueType types[] = {kTypeValue, kTypeDeletion};

  for (size_t len : lens) {
    const std::string user_key = RandomKey(&rng, static_cast<int>(len));
    ASSERT_EQ(len, user_key.size());
    for (SequenceNumber seq : seqs) {
      for (ValueType type : types) {
        const std::string internal_key = BuildInternalKey(user_key, seq, type);
        ASSERT_EQ(user_key.size() + kInternalKeyTrailerSize, internal_key.size());
        // 与手工拼字节（protocol §6 独立参照）逐字节一致
        EXPECT_EQ(ManualInternalKey(user_key, seq, type), internal_key)
            << "len=" << len << " seq=" << seq;
        // trailer 的小端字节序
        const uint64_t trailer = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
        EXPECT_EQ(((seq << 8) | static_cast<uint64_t>(static_cast<uint8_t>(type))), trailer);
        EXPECT_EQ(PackTrailer(seq, type), trailer);
        EXPECT_EQ(seq, ExtractSequence(trailer));
        EXPECT_EQ(static_cast<int>(type), static_cast<int>(ExtractValueType(trailer)));

        Slice parsed_user_key;
        SequenceNumber parsed_seq = kMaxSequenceNumber;
        ValueType parsed_type = kTypeDeletion;
        ASSERT_TRUE(ParseInternalKey(internal_key, &parsed_user_key, &parsed_seq, &parsed_type))
            << "len=" << len << " seq=" << seq;
        EXPECT_EQ(user_key, parsed_user_key.ToString());
        EXPECT_EQ(seq, parsed_seq);
        EXPECT_EQ(static_cast<int>(type), static_cast<int>(parsed_type));
        EXPECT_EQ(user_key, ExtractUserKey(internal_key).ToString());
      }
    }
  }

  // PackTrailer 位布局（protocol §6）：trailer = (sequence << 8) | type
  EXPECT_EQ(0x0000000000000000ull, PackTrailer(0, kTypeDeletion));
  EXPECT_EQ(0x0000000000000001ull, PackTrailer(0, kTypeValue));
  EXPECT_EQ(0x0000000000000100ull, PackTrailer(1, kTypeDeletion));
  EXPECT_EQ(0x0000000000000101ull, PackTrailer(1, kTypeValue));
  EXPECT_EQ(((kMaxSequenceNumber << 8) | 1ull), PackTrailer(kMaxSequenceNumber, kTypeValue));
  EXPECT_EQ(kMaxSequenceNumber, ExtractSequence(PackTrailer(kMaxSequenceNumber, kTypeValue)));
  EXPECT_EQ(kMaxSequenceNumber, ExtractSequence(PackTrailer(kMaxSequenceNumber, kTypeDeletion)));

  // lookup key（protocol §6.2）
  EXPECT_EQ(ManualLookupKey("abc", 7), BuildLookupKey("abc", 7));
  EXPECT_EQ(ManualInternalKey("abc", kMaxSequenceNumber, kTypeValue),
            BuildLookupKey("abc", kMaxSequenceNumber));
  EXPECT_EQ(ManualLookupKey("abc", 0), BuildLookupKey("abc", 0));
  EXPECT_EQ(11u, BuildLookupKey("abc", 0).size());
}

TEST(InternalKey, ParseMalformed) {
  Rng rng(kHarnessSeed + 13);
  // 长度 0..7：一律拒绝，且不得越界读（ASan/UBSan 兜底，prerequisites §6.2）
  for (size_t n = 0; n < 8; ++n) {
    const std::string raw = RandomValue(&rng, static_cast<int>(n));  // 任意字节，避免误判
    Slice user_key("keep");
    SequenceNumber seq = 12345;
    ValueType type = kTypeValue;
    EXPECT_FALSE(ParseInternalKey(raw, &user_key, &seq, &type)) << "长度 " << n << " 必须拒绝";
    EXPECT_EQ("keep", user_key.ToString()) << "失败时不得修改输出（n=" << n << "）";
    EXPECT_EQ(12345u, seq) << "失败时不得修改输出（n=" << n << "）";
    EXPECT_EQ(static_cast<int>(kTypeValue), static_cast<int>(type))
        << "失败时不得修改输出（n=" << n << "）";
  }
  // 长度 >= 8 但 type 非法（protocol §6：不在 {0,1} 内视为畸形）
  {
    const int bad_types[] = {2, 3, 0x10, 0x7F, 0x80, 0xFE, 0xFF};
    for (int t : bad_types) {
      std::string raw = ManualInternalKey("userkey", 42, kTypeValue);
      // trailer 是 8 字节**小端**：最低物理字节 = raw[size-8] 才是 type 字段（protocol §6）。
      // [#3 阶段修订] 原文写的是 raw[size-1]（最高字节，属 sequence）：
      //   该下标把 b7 改成 {2,3,0x10,0x7F,0x80,0xFE,0xFF}，而 seq=kMaxSequenceNumber(2^56-1)
      //   的 b7 恰好也是 0xFF 且被 InternalKey.BuildParseRoundTrip 要求必须解析成功 ——
      //   同一解析器无法既接受又拒绝 b7=0xFF，两条断言互斥。此处按本行注释的原意改成 type 字节。
      raw[raw.size() - 8] = static_cast<char>(t);
      Slice user_key("keep");
      SequenceNumber seq = 12345;
      ValueType type = kTypeValue;
      EXPECT_FALSE(ParseInternalKey(raw, &user_key, &seq, &type))
          << "type=0x" << std::hex << t << " 必须拒绝";
      EXPECT_EQ("keep", user_key.ToString());
      EXPECT_EQ(12345u, seq);
      EXPECT_EQ(static_cast<int>(kTypeValue), static_cast<int>(type));
    }
  }
  // 典型畸形：只有 trailer 没有 user key（8 字节）→ 结构合法（空 user key 的拒绝属上层
  // ValidateKey / MemTable::Add 的职责，protocol §6/§8），此处必须能解析出来
  {
    std::string raw = ManualInternalKey("", 7, kTypeValue);
    ASSERT_EQ(8u, raw.size());
    Slice user_key("keep");
    SequenceNumber seq = 0;
    ValueType type = kTypeDeletion;
    ASSERT_TRUE(ParseInternalKey(raw, &user_key, &seq, &type));
    EXPECT_TRUE(user_key.empty());
    EXPECT_EQ(7u, seq);
    EXPECT_EQ(static_cast<int>(kTypeValue), static_cast<int>(type));
    EXPECT_TRUE(ExtractUserKey(raw).empty());
  }
  // 非法 type 只在长度也非法时不越界：长度 8 的两种边界都覆盖
  {
    std::string raw = ManualInternalKey("", 0, kTypeDeletion);
    Slice user_key;
    SequenceNumber seq = 9;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(raw, &user_key, &seq, &type));
    EXPECT_TRUE(user_key.empty());
    EXPECT_EQ(0u, seq);
    EXPECT_EQ(static_cast<int>(kTypeDeletion), static_cast<int>(type));
  }
}

TEST(InternalKey, CompareOrder) {
  InternalKeyComparator cmp(BytewiseComparator());
  EXPECT_STREQ("leveldb.InternalKeyComparator", cmp.Name());
  EXPECT_EQ(BytewiseComparator(), cmp.user_comparator());
  EXPECT_STREQ("leveldb.BytewiseComparator", BytewiseComparator()->Name());

  const std::string a5 = BuildInternalKey("a", 5, kTypeValue);
  const std::string a3 = BuildInternalKey("a", 3, kTypeValue);
  const std::string a5_del = BuildInternalKey("a", 5, kTypeDeletion);
  const std::string b1 = BuildInternalKey("b", 1, kTypeValue);

  EXPECT_EQ(0, cmp.Compare(a5, a5));
  // 同 user key：sequence 降序（大 sequence 在前 = 更小）
  EXPECT_LT(cmp.Compare(a5, a3), 0);
  EXPECT_GT(cmp.Compare(a3, a5), 0);
  // user key 升序优先于 sequence
  EXPECT_LT(cmp.Compare(a5, b1), 0);
  EXPECT_GT(cmp.Compare(b1, a5), 0);
  EXPECT_LT(cmp.Compare(BuildInternalKey("a", kMaxSequenceNumber, kTypeValue),
                        BuildInternalKey("ab", 0, kTypeDeletion)),
            0);
  // 同 seq：kTypeValue(0x1) 在 kTypeDeletion(0x0) 之前 → 更小
  EXPECT_LT(cmp.Compare(a5, a5_del), 0);
  EXPECT_GT(cmp.Compare(a5_del, a5), 0);

  std::vector<std::string> keys;
  const SequenceNumber order_seqs[] = {0, 1, 255, 256, kMaxSequenceNumber};
  for (const char* user : {"", "a", "aa", "ab", "b", "\xFF\x80", "\xFF\xFF"}) {
    for (SequenceNumber seq : order_seqs) {
      for (ValueType type : {kTypeValue, kTypeDeletion}) {
        keys.push_back(BuildInternalKey(Slice(user), seq, type));
      }
    }
  }
  ExpectStrictTotalOrder(cmp, keys, "InternalKeyComparator");

  // 与 protocol §6.1 的参考实现逐对一致（含 user key 不同/相同两类）
  for (const std::string& x : keys) {
    for (const std::string& y : keys) {
      const int user_c = BytewiseComparator()->Compare(ExtractUserKey(x), ExtractUserKey(y));
      int expect;
      if (user_c != 0) {
        expect = user_c > 0 ? 1 : -1;
      } else {
        const uint64_t tx = DecodeFixed64(x.data() + x.size() - 8);
        const uint64_t ty = DecodeFixed64(y.data() + y.size() - 8);
        expect = tx > ty ? -1 : (tx < ty ? 1 : 0);
      }
      const int got = cmp.Compare(x, y);
      EXPECT_EQ(expect, (got > 0) - (got < 0));
    }
  }
}

TEST(InternalKey, LookupKeySemantics) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTable mem(icmp, kTestWriteBufferSize);

  // "k" 的 5 个版本（其中 seq=3 是 tombstone），外加一个更大的 user key "z"
  AddEntry(&mem, 1, kTypeValue, "k", "v1");
  AddEntry(&mem, 2, kTypeValue, "k", "v2");
  AddEntry(&mem, 3, kTypeDeletion, "k", "");
  AddEntry(&mem, 4, kTypeValue, "k", "v4");
  AddEntry(&mem, 5, kTypeValue, "k", "v5");
  AddEntry(&mem, 9, kTypeValue, "z", "zv");

  // Get(lookup_key)：Seek 命中即该快照下可见的最新版本（protocol §6.2）
  {
    std::string value;
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", kMaxSequenceNumber), &value));
    EXPECT_EQ("v5", value);
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 4), &value));
    EXPECT_EQ("v4", value);
    EXPECT_EQ(MemTable::GetResult::kDeleted, mem.Get(BuildLookupKey("k", 3), &value));
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 2), &value));
    EXPECT_EQ("v2", value);
    EXPECT_EQ(MemTable::GetResult::kFound, mem.Get(BuildLookupKey("k", 1), &value));
    EXPECT_EQ("v1", value);
    // snapshot=0：没有任何版本 <= 0
    EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("k", 0), &value));
    EXPECT_EQ(MemTable::GetResult::kNotFound, mem.Get(BuildLookupKey("none", kMaxSequenceNumber), &value));
  }

  // 用内部迭代器直接验证「lookup_key 落在该 user key 下 <= snapshot 的最大 sequence」
  std::unique_ptr<Iterator> it(mem.NewIterator());
  const SequenceNumber snapshots[] = {kMaxSequenceNumber, 5, 4, 3, 2, 1};
  const SequenceNumber expected[] = {5, 5, 4, 3, 2, 1};
  for (size_t i = 0; i < sizeof(snapshots) / sizeof(snapshots[0]); ++i) {
    it->Seek(BuildLookupKey("k", snapshots[i]));
    ASSERT_TRUE(it->Valid()) << "snapshot=" << snapshots[i] << " 必须命中";
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    ASSERT_TRUE(ParseInternalKey(it->key(), &user_key, &seq, &type));
    EXPECT_EQ("k", user_key.ToString());
    EXPECT_EQ(expected[i], seq) << "snapshot=" << snapshots[i] << " 命中版本错误";
  }
  // snapshot=0 时该 user key 全不可见 → 落到下一个更大的 user key
  it->Seek(BuildLookupKey("k", 0));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("z", ExtractUserKey(it->key()).ToString());

  // lookup key 与「该快照下的版本」的相对次序（protocol §6.2 的推理依据）
  EXPECT_EQ(0, icmp.Compare(BuildLookupKey("k", 4), BuildInternalKey("k", 4, kTypeValue)));
  EXPECT_LT(icmp.Compare(BuildLookupKey("k", 3), BuildInternalKey("k", 3, kTypeDeletion)), 0);
  // [#3 阶段修订] 原文写 EXPECT_LT(...)：与上一行（seq=4 的同型比较断言 == 0）互斥 ——
  // kValueTypeForSeek == kTypeValue 时，BuildLookupKey(k,s) 与 BuildInternalKey(k,s,kTypeValue)
  // 逐字节相同，同一比较器不可能对 s=4 给 0、对 s=3 给负数。按 protocol §6.2 的定义改为相等。
  EXPECT_EQ(0, icmp.Compare(BuildLookupKey("k", 3), BuildInternalKey("k", 3, kTypeValue)));
  EXPECT_GT(icmp.Compare(BuildLookupKey("k", 3), BuildInternalKey("k", 4, kTypeValue)), 0);
  EXPECT_GT(icmp.Compare(BuildLookupKey("k", 4), BuildInternalKey("k", 5, kTypeValue)), 0);
  // user key 不同时，lookup key 的构造不影响 user key 序
  EXPECT_LT(icmp.Compare(BuildLookupKey("a", kMaxSequenceNumber), BuildLookupKey("b", 0)), 0);
}

TEST(MemTableKeyComparator, TotalOrder) {
  InternalKeyComparator icmp(BytewiseComparator());
  MemTableKeyComparator mkc(&icmp);

  ASSERT_TRUE(mkc.Name() != nullptr);
  EXPECT_GT(std::strlen(mkc.Name()), 0u);

  // 比较对象是整条条目（protocol §7：先比 internal key，完全相同再比整条字节序）
  const char* const values[] = {"", "a", "b", "aa", "z"};
  std::vector<std::string> entries;
  const SequenceNumber mkc_seqs[] = {1, 2, 3};
  for (SequenceNumber seq : mkc_seqs) {
    for (ValueType type : {kTypeValue, kTypeDeletion}) {
      for (const char* v : values) {
        entries.push_back(ManualEntry("key", seq, type, Slice(v)));
        entries.push_back(ManualEntry("ke", seq, type, Slice(v)));
        entries.push_back(ManualEntry("key2", seq, type, Slice(v)));
      }
    }
  }
  ASSERT_GT(entries.size(), 20u);

  // 同一 internal key、不同 value 的条目不得判等（否则跳表会把它们合并，遍历丢条目）
  {
    const std::string e1 = ManualEntry("key", 7, kTypeValue, Slice("v1"));
    const std::string e2 = ManualEntry("key", 7, kTypeValue, Slice("v2"));
    EXPECT_NE(e1, e2);
    EXPECT_NE(0, mkc.Compare(e1, e2)) << "同 internal key 不同 value 必须有序（严格全序）";
    EXPECT_NE(0, mkc.Compare(e2, e1));
    EXPECT_EQ(0, mkc.Compare(e1, e1));
  }

  ExpectStrictTotalOrder(mkc, entries, "MemTableKeyComparator");

  // 端到端：把条目乱序插入跳表（注入 MemTableKeyComparator），遍历必须非降序且是原集合的一个排列
  {
    Arena arena;
    Skiplist list(&arena, &mkc);
    std::vector<std::string> shuffled = entries;
    Rng rng(kHarnessSeed + 17);
    for (size_t i = shuffled.size(); i > 1; --i) {
      const size_t j = rng.Uniform(static_cast<uint32_t>(i));
      std::swap(shuffled[i - 1], shuffled[j]);
    }
    for (const std::string& e : shuffled) {
      list.Insert(ArenaStoreSlice(&arena, e));
    }
    EXPECT_EQ(entries.size(), list.GetStats().node_count);

    std::vector<std::string> seen;
    std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      seen.push_back(it->key().ToString());
    }
    ASSERT_EQ(entries.size(), seen.size()) << "跳表遍历丢条目（严格全序被破坏会导致此现象）";
    for (size_t i = 1; i < seen.size(); ++i) {
      EXPECT_LE(mkc.Compare(seen[i - 1], seen[i]), 0) << "第 " << i << " 对逆序";
    }
    std::vector<std::string> a = entries;
    std::vector<std::string> b = seen;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    EXPECT_EQ(a, b) << "遍历结果不是原集合的排列";
  }
}
