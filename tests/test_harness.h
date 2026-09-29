// tests/test_harness.h —— M1（#2 阶段）测试辅助设施
//
// 目录纪律（docs/m1-design.md §3 / docs/m1-prerequisites.md §3「CMake 目标混入测试代码」风险）：
//   本文件只属于 tests/，**不得**被 src/ 下任何文件 include；全部符号 inline，可被多个 TU 共用。
//
// 提供四类设施：
//   1) Rng / RandomKey / RandomValue：固定种子的可复现生成器。
//      刻意不用 <random>：分布实现跨平台/跨标准库不一致，会破坏「固定种子可复现」的验收口径
//      （与 design §7.1 跳表不用 <random> 同一条理由）。
//   2) 对账助手：ExpectSameAsStdMap（用户视图 vs std::map）、
//      ExpectSkiplistMatchesMultimap（跳表 vs std::multimap）、VisibleKeysFromInternal（用户视图独立参照）。
//   3) docs/protocol.md §6/§7 的**手工拼字节参照**：ManualInternalKey / ManualLookupKey / ManualEntry。
//      故意不复用 coding.h 与 common.h 的实现，这样 BuildInternalKey 写错时测试能抓到，
//      而不是「实现与测试同错」。
//   4) TempDir：临时目录 RAII（只有 Env.FileRoundTrip 依赖真实文件系统语义）。
//
// 未冻结签名（design §4/§5 未列出，本文件即契约，M1.1/M1.2 必须匹配）：
//   - 全项目位于 namespace lsm（依据 design §13 的「undefined reference to lsm::...」）；
//   - crc32c::Value(data,n) / crc32c::Extend(init_crc,data,n)（protocol §5 只给了语义，命名按 LevelDB 惯例）；
//   - MemTableKeyComparator(const InternalKeyComparator*)（prerequisites §2 L6「只持不可变指针」），
//     比较对象是**整条 MemTable 条目**（protocol §7）；
//   - Skiplist::Stats::level_histogram[i] = 恰好高度为 i+1 的节点数（本文件与 memtable_test 的层高用例即契约）；
//   - MemTable::NewIterator() 的 key() 返回 internal key、value() 返回 value（design §4.4 明确冻结）。

#ifndef LSM_TESTS_TEST_HARNESS_H_
#define LSM_TESTS_TEST_HARNESS_H_

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common.h"
#include "db.h"
#include "memtable.h"
#include "skiplist.h"
#include "util/arena.h"
#include "util/coding.h"

namespace lsm {
namespace test {

// 固定种子：所有随机用例只用这一个种子（及少量显式偏移），失败可复现。
constexpr uint32_t kHarnessSeed = 0x5EED2025u;

// A 组默认写缓冲：4 MiB，与 Options 默认值同口径（design §4.3）。
const size_t kTestWriteBufferSize = 4 * 1024 * 1024;

// ---------------------------------------------------------------------------
// 1) 可复现随机数（Park–Miller minimal standard，与 design §7.1 同源）
// ---------------------------------------------------------------------------
class Rng {
 public:
  explicit Rng(uint32_t seed = kHarnessSeed) : seed_(seed == 0 ? 1u : seed) {}

  uint32_t Next() {
    seed_ = static_cast<uint32_t>((static_cast<uint64_t>(seed_) * 16807ull) % 2147483647ull);
    return seed_;
  }

  // 未归一化的均匀抽取：[0, n)
  uint32_t Uniform(uint32_t n) { return n == 0 ? 0u : Next() % n; }

  // 偏斜长度：小值更常见，用于制造长短混合的 key/value（LevelDB harness 同风格）
  int Skewed(int max_log) {
    const uint32_t shift = Uniform(static_cast<uint32_t>(max_log) + 1u);
    return static_cast<int>(Uniform(1u << shift));
  }

 private:
  uint32_t seed_;
};

// 小写字母 key：失败时日志可肉眼比对。字节序比较与字符集无关（protocol §6.1 逐字节无符号）。
inline std::string RandomKey(Rng* rng, int len) {
  std::string k;
  k.reserve(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    k.push_back(static_cast<char>('a' + rng->Uniform(26)));
  }
  return k;
}

// 任意字节 value（含 '\0'）：std::string 不依赖 NUL 结尾，正好覆盖「按长度而非终止符」语义。
inline std::string RandomValue(Rng* rng, int len) {
  std::string v;
  v.reserve(static_cast<size_t>(len));
  for (int i = 0; i < len; ++i) {
    v.push_back(static_cast<char>(rng->Uniform(256)));
  }
  return v;
}

// 定宽 8 字节大端 key：字典序 == 数值序（用于保证压力用例的 key 唯一且可排序）。
inline std::string KeyFromIndex(uint64_t i) {
  std::string k(8, '\0');
  for (int b = 0; b < 8; ++b) {
    k[static_cast<size_t>(7 - b)] = static_cast<char>((i >> (8 * b)) & 0xffu);
  }
  return k;
}

// ---------------------------------------------------------------------------
// 2) protocol §6/§7 的手工拼字节参照（独立于实现）
// ---------------------------------------------------------------------------
inline void AppendVarint32Manual(std::string* dst, uint32_t v) {
  while (v >= 128u) {
    dst->push_back(static_cast<char>((v & 0x7fu) | 0x80u));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

// internal_key := user_key || trailer(8B 小端) ; trailer := (sequence << 8) | type
inline std::string ManualInternalKey(const Slice& user_key, SequenceNumber seq, ValueType type) {
  const uint64_t trailer = (seq << 8) | static_cast<uint64_t>(static_cast<uint8_t>(type));
  std::string out(user_key.data(), user_key.size());
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((trailer >> (8 * i)) & 0xffu));
  }
  return out;
}

// protocol §6.2：lookup_key := user_key || ((snapshot << 8) | kValueTypeForSeek)；
// kValueTypeForSeek == kTypeValue（protocol §1），此处直接写 kTypeValue，避免依赖未在 §4/§5 列出的常量。
inline std::string ManualLookupKey(const Slice& user_key, SequenceNumber snapshot) {
  return ManualInternalKey(user_key, snapshot, kTypeValue);
}

// entry := varint32(internal_key_size) || internal_key || varint32(value_size) || value（protocol §7）
inline std::string ManualEntry(const Slice& user_key, SequenceNumber seq, ValueType type,
                               const Slice& value) {
  const std::string internal_key = ManualInternalKey(user_key, seq, type);
  std::string out;
  AppendVarint32Manual(&out, static_cast<uint32_t>(internal_key.size()));
  out.append(internal_key);
  AppendVarint32Manual(&out, static_cast<uint32_t>(value.size()));
  out.append(value.data(), value.size());
  return out;
}

// 跳表/MemTable 内的 Slice 必须指向生命周期 >= 容器的内存（design §4.1 I7）：生产路径是 Arena，
// 测试路径用调用方自己持有的 Arena 复现同一约束（prerequisites §7.5）。
inline Slice ArenaStoreSlice(Arena* arena, const std::string& s) {
  const size_t n = s.empty() ? 1 : s.size();
  char* buf = arena->Allocate(n);
  if (!s.empty()) std::memcpy(buf, s.data(), s.size());
  return Slice(buf, s.size());
}

// ---------------------------------------------------------------------------
// 3) 对账助手
// ---------------------------------------------------------------------------
inline std::vector<std::pair<std::string, std::string>> CollectForward(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  return out;
}

inline std::vector<std::pair<std::string, std::string>> CollectBackward(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  return out;
}

// 用户视图对账：模型与迭代器必须给出完全相同的 (user key, value) 序列。
inline void ExpectSameAsStdMap(const std::string& what,
                               const std::map<std::string, std::string>& model, Iterator* it) {
  std::map<std::string, std::string>::const_iterator m = model.begin();
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_TRUE(m != model.end())
        << what << ": 迭代器多出第 " << n << " 条（模型只有 " << model.size() << " 条）";
    EXPECT_EQ(m->first, it->key().ToString()) << what << ": 第 " << n << " 条 key 不一致";
    EXPECT_EQ(m->second, it->value().ToString()) << what << ": 第 " << n << " 条 value 不一致";
    ++m;
    ++n;
  }
  EXPECT_TRUE(it->status().ok()) << what << ": 迭代结束状态异常：" << it->status().ToString();
  EXPECT_TRUE(m == model.end()) << what << ": 迭代器少输出 " << model.size() - n << " 条";
}

// 跳表多重集对账：只比 key 序列。
// 理由：相同 key 的多个节点是无法互相区分的（跳表只存 key），且 design §7.4 规定新插入者排在
// 相等 key 之前，而 std::multimap 是同 key 按插入序 —— 对「谁在前」没有可观测的判据，故不作断言。
inline void ExpectSkiplistMatchesMultimap(const std::string& what,
                                          const std::multimap<std::string, uint64_t>& model,
                                          const Skiplist& list) {
  std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
  std::multimap<std::string, uint64_t>::const_iterator m = model.begin();
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_TRUE(m != model.end())
        << what << ": 跳表多出第 " << n << " 条（模型只有 " << model.size() << " 条）";
    EXPECT_EQ(m->first, it->key().ToString()) << what << ": 第 " << n << " 条 key 次序/内容不一致";
    ++m;
    ++n;
  }
  EXPECT_TRUE(m == model.end()) << what << ": 跳表少输出 " << model.size() - n << " 条";
  EXPECT_EQ(model.size(), n) << what << ": 遍历条数与模型不一致";
}

inline std::vector<std::string> SkiplistKeysForward(const Skiplist& list) {
  std::vector<std::string> out;
  std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.push_back(it->key().ToString());
  }
  return out;
}

inline std::vector<std::string> SkiplistKeysBackward(const Skiplist& list) {
  std::vector<std::string> out;
  std::unique_ptr<Skiplist::Iterator> it(list.NewIterator());
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    out.push_back(it->key().ToString());
  }
  return out;
}

// ---------------------------------------------------------------------------
// 4) MemTable / DB 助手
// ---------------------------------------------------------------------------
struct InternalEntry {
  std::string internal_key;  // 完整 internal key（user_key || trailer）
  std::string user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  std::string value;
};

// 走 internal-order 迭代器读出全部条目（含多版本与 tombstone），供顺序/计数断言使用。
// 若解析失败直接 FAIL：迭代器给出的 key 必须是合法 internal key（design §4.4）。
inline std::vector<InternalEntry> ReadInternalEntries(MemTable* mem) {
  std::vector<InternalEntry> out;
  std::unique_ptr<Iterator> it(mem->NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    InternalEntry e;
    Slice user_key;
    if (!ParseInternalKey(it->key(), &user_key, &e.seq, &e.type)) {
      ADD_FAILURE() << "迭代器返回的 key 不是合法 internal key（size=" << it->key().size() << ")";
      break;
    }
    e.internal_key = it->key().ToString();
    e.user_key = user_key.ToString();
    e.value = it->value().ToString();
    out.push_back(e);
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return out;
}

// 用户视图的**独立参照**实现（design §4.4 的可见性定义，不经过 db.cc 的 UserIterator）：
// internal-order 遍历中，每个 user key 只取第一条（sequence 最大者）；若它是 tombstone 则该 key 不可见。
inline std::vector<std::pair<std::string, std::string>> VisibleKeysFromInternal(MemTable* mem) {
  std::vector<std::pair<std::string, std::string>> out;
  bool have_key = false;
  std::string last_user;
  for (const InternalEntry& e : ReadInternalEntries(mem)) {
    if (have_key && e.user_key == last_user) continue;  // 同一 user key 的旧版本
    have_key = true;
    last_user = e.user_key;
    if (e.type == kTypeDeletion) continue;  // 最新版本是 tombstone → 该 user key 不可见
    out.emplace_back(e.user_key, e.value);
  }
  return out;
}

inline void AddEntry(MemTable* mem, SequenceNumber seq, ValueType type, const Slice& key,
                     const Slice& value) {
  const Status s = mem->Add(seq, type, key, value);
  ASSERT_TRUE(s.ok()) << "MemTable::Add(seq=" << seq << ", key_size=" << key.size()
                      << ") 意外失败：" << s.ToString();
}

// DB 内存模式打开（M1 只有内存模式，design §9）；失败直接记失败并返回 nullptr。
// 返回裸指针：调用方用 std::unique_ptr<DB> 接管（Iterator 同理，design §4.5 要求调用方 delete）。
inline DB* OpenMemoryDB(size_t write_buffer_size = kTestWriteBufferSize,
                        const Comparator* user_comparator = nullptr) {
  Options options;
  if (user_comparator != nullptr) options.comparator = user_comparator;
  options.write_buffer_size = write_buffer_size;
  DB* db = nullptr;
  const Status s = DB::Open(options, "", &db);
  if (!s.ok() || db == nullptr) {
    ADD_FAILURE() << "DB::Open(内存模式) 失败：" << s.ToString();
    delete db;
    return nullptr;
  }
  return db;
}

// ---------------------------------------------------------------------------
// 5) 临时目录 RAII（只有 Env.FileRoundTrip 用真实文件系统语义，prerequisites §7.4）
// ---------------------------------------------------------------------------
class TempDir {
 public:
  explicit TempDir(const char* prefix = "lsm_test_") {
    const std::string tmpl = std::string("/tmp/") + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const char* made = ::mkdtemp(buf.data());
    if (made == nullptr) {
      std::fprintf(stderr, "TempDir: mkdtemp(%s) 失败：%s\n", tmpl.c_str(), std::strerror(errno));
      std::abort();
    }
    path_.assign(made);
  }

  ~TempDir() { RemoveRecursively(path_); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string File(const std::string& name) const { return path_ + "/" + name; }
  std::string SubDir(const std::string& name) const { return path_ + "/" + name; }

 private:
  static void RemoveRecursively(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d != nullptr) {
      while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        struct stat st;
        if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
          RemoveRecursively(full);
        } else {
          ::unlink(full.c_str());
        }
      }
      ::closedir(d);
    }
    ::rmdir(dir.c_str());
  }

  std::string path_;
};

}  // namespace test
}  // namespace lsm

#endif  // LSM_TESTS_TEST_HARNESS_H_
