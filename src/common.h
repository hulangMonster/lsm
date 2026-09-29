// src/common.h —— M1 全项目类型契约（docs/m1-design.md §4/§5，docs/protocol.md §6）
//
// 依赖纪律（docs/roadmap.md §0）：本头**只依赖标准库**，不 include 任何项目头。
// 因此内部 key 的小端读写在这里自带实现，而不复用 util/coding.h：
//   common.h ← util/coding.h 是既定方向，反向 include 会形成 common.h ↔ coding.h 循环
//   （先被包含的那一侧必然看到对方尚未声明的符号）。
// 两份小端实现的一致性由测试保证：InternalKey.BuildParseRoundTrip 与 Coding.Fixed*RoundTrip
// 都对照 protocol §2/§6 的手工拼字节参照（tests/test_harness.h 的 ManualInternalKey 等）。
//
// 生命周期契约（design §4.1 / I7）：Slice 是**视图**，不持有内存，仅在来源对象存活且未被修改
// 的窗口内有效；跨越调用边界（返回值、容器元素、异步）必须 ToString()。库内 Slice 一律指向 Arena。
#ifndef LSM_COMMON_H_
#define LSM_COMMON_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace lsm {

class Env;   // M2：Options::env 注入（定义见 util/env.h；此处只前向声明，保持 common.h 零项目依赖）

// M2：组提交的可注入观察点（生产为 nullptr）。测试用它构造确定性时序，而不是靠 sleep 赌调度：
//   - OnGroupTaken：队首已选出 flusher、批已组装但**尚未做 IO** —— A20 在这里让整批写者就位
//   - OnAfterSyncBeforePublish：fsync 已返回、durable 水位**尚未发布** —— A24 在这里断言窗口没被提前放开
class CommitHook {
 public:
  virtual ~CommitHook() = default;
  // 队首当选 flusher 之后、**取批之前**调用（此时不持任何锁 ⇒ 其他写者可自由入队）。
  // A20 用它造确定性屏障：让 N 个写者全部入队后再放行组装，从而断言"本批确实含 N 个写者"。
  virtual void OnBeforeGroupAssemble() {}
  virtual void OnGroupTaken() {}
  virtual void OnAfterSyncBeforePublish() {}
};

// ---------------------------------------------------------------------------
// Slice：ptr + len 视图
// ---------------------------------------------------------------------------
class Slice {
 public:
  // 空 Slice 指向字面量 "" 而非 nullptr：ToString()/memcmp(…,0) 在空视图上也必须是良定义的。
  Slice() : data_(""), size_(0) {}
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}   // s 必须在 Slice 存活期内有效
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}  // 不持有 s

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](size_t n) const { return data_[n]; }
  void clear() { data_ = ""; size_ = 0; }

  // 逐字节**无符号**比较（protocol §6.1）：先比公共前缀，再短者为小。
  // 不能直接用 memcmp 的返回值大小，只能用符号（测试用 EXPECT_LT/GT 与符号归一化）。
  int compare(const Slice& b) const {
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = 0;
    if (min_len > 0) r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) r = -1;
      else if (size_ > b.size_) r = 1;
    }
    return r;
  }

  bool starts_with(const Slice& x) const {
    return size_ >= x.size_ && (x.size_ == 0 || std::memcmp(data_, x.data_, x.size_) == 0);
  }

  std::string ToString() const { return std::string(data_, size_); }

 private:
  const char* data_;
  size_t size_;
};

// ---------------------------------------------------------------------------
// Status（design §4.2；码值一次冻结，后续阶段不得改枚举）
// ---------------------------------------------------------------------------
class Status {
 public:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5,
    kFrozen = 6,
  };

  Status() : code_(kOk) {}
  static Status OK() { return Status(); }

  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice());
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice());
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice());
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice());
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice());
  static Status Frozen(const Slice& msg, const Slice& msg2 = Slice());

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsNotSupported() const { return code_ == kNotSupported; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsFrozen() const { return code_ == kFrozen; }

  Code code() const { return code_; }
  std::string ToString() const;

 private:
  Status(Code code, const Slice& msg, const Slice& msg2);
  static const char* CodeName(Code code);

  Code code_;
  std::string msg_;   // 只在失败路径分配（成功路径零堆分配）
};

// ---------------------------------------------------------------------------
// 序列号 / 值类型 / 内部 key 常量（protocol §1/§6）
// ---------------------------------------------------------------------------
using SequenceNumber = uint64_t;

enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

constexpr SequenceNumber kMaxSequenceNumber = (1ULL << 56) - 1;
constexpr ValueType kValueTypeForSeek = kTypeValue;
constexpr size_t kInternalKeyTrailerSize = 8;
constexpr size_t kInternalKeyMinSize = 8;
constexpr size_t kMaxUserKeySize = 64 * 1024;

// trailer := (sequence << 8) | type；物理上以 8 字节小端落进 internal key。
inline uint64_t PackTrailer(SequenceNumber seq, ValueType type) {
  return (seq << 8) | static_cast<uint64_t>(static_cast<uint8_t>(type));
}
inline SequenceNumber ExtractSequence(uint64_t trailer) { return trailer >> 8; }
inline ValueType ExtractValueType(uint64_t trailer) {
  return static_cast<ValueType>(trailer & 0xffu);
}

inline void PutTrailerLE(char* dst, uint64_t trailer) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((trailer >> (8 * i)) & 0xffu);
}
inline uint64_t DecodeTrailerLE(const char* src) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(src[i]);
  return v;
}

// 前置条件：internal_key.size() >= kInternalKeyMinSize（调用方保证；解析入口请用 ParseInternalKey）
inline Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - kInternalKeyTrailerSize);
}

// 畸形输入（长度 < 8 或 type 不在 {0,1}）返回 false，且**不修改任何输出**（protocol §6）。
inline bool ParseInternalKey(const Slice& internal_key, Slice* user_key, SequenceNumber* seq,
                             ValueType* type) {
  if (internal_key.size() < kInternalKeyMinSize) return false;
  const uint64_t trailer =
      DecodeTrailerLE(internal_key.data() + internal_key.size() - kInternalKeyTrailerSize);
  const ValueType t = ExtractValueType(trailer);
  if (t != kTypeValue && t != kTypeDeletion) return false;
  *user_key = Slice(internal_key.data(), internal_key.size() - kInternalKeyTrailerSize);
  *seq = ExtractSequence(trailer);
  *type = t;
  return true;
}

inline std::string BuildInternalKey(const Slice& user_key, SequenceNumber seq, ValueType type) {
  std::string out;
  out.reserve(user_key.size() + kInternalKeyTrailerSize);
  if (!user_key.empty()) out.assign(user_key.data(), user_key.size());
  char trailer[kInternalKeyTrailerSize];
  PutTrailerLE(trailer, PackTrailer(seq, type));
  out.append(trailer, kInternalKeyTrailerSize);
  return out;
}

// protocol §6.2：读快照 s 下的「最大可能内部 key」，Seek 命中即该快照可见的最新版本。
inline std::string BuildLookupKey(const Slice& user_key, SequenceNumber snapshot) {
  return BuildInternalKey(user_key, snapshot, kValueTypeForSeek);
}

// ---------------------------------------------------------------------------
// Comparator（L6：必须无状态且线程安全）
// ---------------------------------------------------------------------------
class Comparator {
 public:
  virtual ~Comparator() = default;
  virtual int Compare(const Slice& a, const Slice& b) const = 0;
  virtual const char* Name() const = 0;
};

class BytewiseComparatorImpl : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override { return a.compare(b); }
  const char* Name() const override { return "leveldb.BytewiseComparator"; }
};

// 进程级单例：inline 函数里的 static 在 C++17 下跨 TU 唯一，无需单独的 .cpp。
inline const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl kInstance;
  return &kInstance;
}

// 内部 key 比较（protocol §6.1）：user key 升序 → trailer 降序（sequence 大者在前）。
class InternalKeyComparator : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* user_comparator)
      : user_comparator_(user_comparator) {}

  int Compare(const Slice& a, const Slice& b) const override {
    // 防御：畸形 internal key（< kInternalKeyMinSize）不得进入 ExtractUserKey（size_t 下溢 + 越界读）。
    // 库内不产生畸形键，但 M3 的 block 解析会复用同一个比较器、输入来自磁盘。退化为整条字节序，
    // 代价是畸形键参与排序时顺序未定义 —— 这属于「输入已损坏」的范畴，安全性优先。
    if (a.size() < kInternalKeyMinSize || b.size() < kInternalKeyMinSize) return a.compare(b);
    const int c = user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
    if (c != 0) return c;
    const uint64_t ta = DecodeTrailerLE(a.data() + a.size() - kInternalKeyTrailerSize);
    const uint64_t tb = DecodeTrailerLE(b.data() + b.size() - kInternalKeyTrailerSize);
    if (ta > tb) return -1;   // trailer 降序
    if (ta < tb) return 1;
    return 0;
  }
  const char* Name() const override { return "leveldb.InternalKeyComparator"; }
  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;
};

// ---------------------------------------------------------------------------
// M3：flush 路径的注入点（docs/m3-design.md §8.7 E8）。
// 与 CommitHook 同纪律：生产恒为 nullptr，测试用它把「落盘三步」的时序确定化，
// 不靠 sleep 赌调度。三个点逐字对应 M3-B02 的三个进程级 kill -9 注入点。
// ---------------------------------------------------------------------------
class FlushHook {
 public:
  virtual ~FlushHook() = default;
  // TableBuilder 已把 footer 写进 .sst.tmp（内容完整、尚未 fsync）。
  virtual void OnSSTableWritten() {}
  // rename(.sst.tmp -> .sst) 之前。
  virtual void OnBeforeRename() {}
  // SyncDir 已完成、版本注册（内存替换）尚未发生之前。
  virtual void OnBeforeRegister() {}
};

// ---------------------------------------------------------------------------
// Options（M1 定义 + M2/M3 只增不改；docs/m3-design.md §8.5/§8.7、M3.2 交付表）
//   * block_size / verify_checksums 是 SSTable 读写的格式层参数（M3.1 曾以 TableOptions
//     承载；M3.2 按设计统一回 Options，TableOptions 保留为别名以免动 M3.1 既有用例）。
//   * max_open_files 是 TableCache 的容量（§D8 的默认值 64）。
//   * flush_hook 见 FlushHook。
//   * 仍然禁止出现「看起来能用但没实现」的字段。
// ---------------------------------------------------------------------------
struct Options {
  const Comparator* comparator = BytewiseComparator();
  size_t write_buffer_size = 4 * 1024 * 1024;   // 4 MiB（目标值，见 design §1.3.2/E5）
  // M2 增补（登记于 docs/m2-prerequisites.md §9 第 7 条）：注入 Env。nullptr = Env::Default()。
  // 为什么需要：A27~A31 的掉电语义必须用 MemEnv（内存文件系统 + fsync 水位 + 固定种子撕裂）
  // 才能确定性验证，而 M2 的恢复路径原来硬编码 Env::Default()，测试无法注入。
  Env* env = nullptr;
  // M2.3：组提交观察点（nullptr = 无观察者）。见 CommitHook 的注释。
  CommitHook* commit_hook = nullptr;
  // M3 增补（docs/m3-design.md §8.5）：block_size 的目标值/合法区间 [512, 1 MiB]。
  size_t block_size = 4096;
  bool verify_checksums = true;
  // M3 增补（§D8）：TableCache 打开文件句柄的上界；0/过大的非法值在 DB::Open 拒绝。
  size_t max_open_files = 64;
  // M3 增补（§8.7 E8）：flush 路径观察点。
  FlushHook* flush_hook = nullptr;
};

// ---------------------------------------------------------------------------
// Iterator（design §4.4）：用户视图三态状态机 kBeforeFirst / kValid / kPastEnd
// ---------------------------------------------------------------------------
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  virtual void Seek(const Slice& target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;

  // 仅在 Valid() 时可调用（前置条件）。
  // 有效期契约（#4 评审建议 1；design §4.1 的 I7）：
  //   - MemTable 内部序迭代器：两者都指向 Arena，有效期 = MemTable 存活期；
  //   - DB 用户视图迭代器：value() 指向 Arena；**key() 指向迭代器内部的 user key 缓存，
  //     下一次定位调用（Seek*/SeekToFirst/SeekToLast/Next/Prev）后即失效**。
  //   需要跨调用保留一律 ToString()。
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  virtual Status status() const = 0;
};

}  // namespace lsm

#endif  // LSM_COMMON_H_
