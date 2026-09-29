# M1 设计（docs/m1-design.md）—— 工程骨架 + KV 接口 + MemTable（跳表）

> 状态：**#0 设计定稿（已获用户批准）**。冻结范围：`docs/protocol.md`（位级编码）+ 本文件 §4 的全部对外签名 +
> §7 的跳表参数 + §8 的 MemTable 语义与统计口径 + §9 的 DB 语义。M2~M5 不得私自变更，确需变更须回 `#0` 修订并说明影响面。
>
> 决策记录：9 项开放决策的「方案 → 取舍 → 推荐」见 §1.3；本文件是那 9 项决策的落地文本。

## 1. 目标、非目标与决策记录

### 1.1 目标

1. 可构建、可测试的工程骨架：CMake 双目标 `lsm`（静态库）+ `lsm_tests`（GTest 可执行），三构建目录（`build` / `build-asan` / `build-tsan`）。
2. 全项目基础类型与对外接口：`Slice` / `Status` / `Options` / `Comparator` / `Iterator` / `DB`。
3. 跳表 + MemTable：内部 key 编码（user_key | sequence | type）、多版本、tombstone、容量控制与只读冻结。
4. 迭代器语义：MemTable 层按内部 key 序（user key 升序、同 key sequence 降序）；DB 层按 user key 序（每个 key 只出最新可见版本）。

### 1.2 非目标（M1 硬边界，评审逐条对照）

WAL 与任何形式的持久化、SSTable、flush、compaction、Bloom Filter、WriteBatch、快照与多版本读视图、
块缓存、并发写多线程与组提交、压缩算法、崩溃恢复、与 raft-kv 的对接。
**禁止**出现上述任何符号或占位实现（stub 也不行）。

### 1.3 9 项开放决策（已批准）

| # | 决策 | 方案与取舍 | 采纳 |
|---|---|---|---|
| 1 | 对外接口 | `DB` 类 + `Options` + `DB::Open(options, name, &db)`；`name` 保留（M2 用），M1 内存模式：空串=内存模式，非空=`kNotSupported` | 采纳；签名一次冻结，M2 不改 |
| 2 | Key/Value 表示 | `Slice`(ptr+len 视图) vs `std::string_view` vs `std::string` | 采纳 `Slice` + 显式 `ToString()`；`string_view` 不可控生命周期语义、`std::string` 有拷贝 |
| 3 | 内部 key 编码 | 固定 8 字节尾（seq 7B + type 1B）vs `struct` vs varint | 采纳**固定宽度 + `InternalKey` 自由函数**；varint 省空间但把比较/解码/前缀 Seek 的出错面放大 |
| 4 | 跳表 | p=1/4、maxHeight=12、随机层高（固定种子）；`std::atomic<Node*>` acquire/release；节点 Arena 分配 + 固定 `next_[12]` 数组 | 采纳；固定数组成员零 UB（柔性数组省内存但有未定义行为争议），MemTable 容量上限 4 MiB，4× 指针开销可接受 |
| 5 | 删除 | tombstone（`kTypeDeletion` + Get→NotFound）vs 物理删除 | 采纳 tombstone；I3 已发布节点不可变，且能否真删只有 compaction 知道 |
| 6 | 容量控制 | 近似统计（Arena 用量+条目数）vs 精确记账；写前判；专用码 `kFrozen` | 采纳近似 + 写前判 + `kFrozen`；冻结后查询/迭代照常可用 |
| 7 | 迭代器 | 内部序迭代器（多版本）+ DB 用户视图迭代器（去重+跳 tombstone）；持裸引用，状态机显式 | 采纳；`SeekToLast/Prev` 纳入 M1 |
| 8 | Arena | M1 就引入（4 KiB 块、不单独 free、`max_align_t` 对齐、统计、Reset） | 采纳；跳表/MemTable 内禁止裸 new/delete |
| 9 | 测试 seam | 最小 `Env`（时间+文件）M1 落地 vs M2 再抽；GTest 引入方式 | 采纳 M1 落地最小 `Env`（指令文件清单要求 + `util_test` 真实覆盖）；GTest 用 `/usr/local` 既有安装，不用 FetchContent |

### 1.4 对原文两处不一致的澄清（#1 校验需对照）

- **依赖方向**：原文「util → common → memtable → db」与「`Slice` 在 common.h、却被 util/coding 使用」互斥。
  本仓库按**单向分层**落地：`common.h`（最底层类型契约）→ `util` → `memtable` → `db`，见 `docs/roadmap.md` §0。
- **M1 不引入 `WriteOptions`/`ReadOptions`**：否则 `sync`/`snapshot` 就是「看起来能用但没实现」的字段。
  M2 加 sync 时以**新增重载**形式补（`Put(const WriteOptions&, ...)`），不破坏 M1 契约。M3 加 snapshot 同理。

## 2. 与 M2~M6 的关系

| 本阶段产物 | 被谁复用 | 契约稳定性 |
|---|---|---|
| `Slice` / `Status` / `Comparator` / `Options` | 全部后续阶段 | 冻结 |
| `docs/protocol.md`（varint/fixed/length-prefix/CRC32C/内部 key/条目编码） | M2 WAL record、M3 SSTable block | 冻结，逐字复用 |
| `Arena` | M2 写缓冲、M3 SSTable 构建 | 冻结（可扩展接口） |
| `Skiplist` / `MemTable` | M3 flush（`MemTable::NewIterator()` 的内部序） | 冻结 |
| `DB` 接口 | M2 持久化、M3 读路径、M4 compaction | 只允许**增补**（重载/新虚函数），不得改语义 |
| `Env` | M2 WAL 文件 IO、M3 SSTable 文件 IO | 只允许增补（如 M3 的 `RandomAccessFile`） |

## 3. 文件清单

| 文件 | 类型 | 内容 |
|---|---|---|
| `CMakeLists.txt` | 新增 | `lsm`（STATIC）/ `lsm_tests`；`-Wall -Wextra`；`ENABLE_ASAN`/`ENABLE_TSAN` 选项；GTest 从 `/usr/local` 查找 |
| `src/common.h` | 新增 | `Slice`/`Status`/`SequenceNumber`/`ValueType`/`Comparator`/`Options`/内部 key 编解码自由函数 |
| `src/util/status.cpp` | 新增 | `Status` 工厂与 `ToString` |
| `src/util/coding.{h,cpp}` | 新增 | Fixed32/64、Varint32/64、LengthPrefixed（§protocol 2~4） |
| `src/util/crc32c.{h,cpp}` | 新增 | CRC32C（表驱动，§protocol 5） |
| `src/util/arena.{h,cpp}` | 新增 | Arena（§6） |
| `src/util/env.h` + `src/util/env_posix.cpp` | 新增 | 最小 Env（§10）。注意：指令文件清单写 `src/env.h`/`src/env_posix.cpp`，本设计把它们归入 `util/`（Env 属工具层，放 `src/` 根会破坏 §roadmap 的单向分层） |
| `src/skiplist.h` | 新增 | 跳表（§7） |
| `src/memtable.{h,cpp}` | 新增 | MemTable + 内部序迭代器（§8） |
| `src/db.{h,cpp}` | 新增 | `DB` 接口 + M1 内存实现 + 用户视图迭代器（§9） |
| `tests/test_harness.h` | 新增 | RandomKey/RandomValue、`ExpectSameAsStdMap`、InternalKey 拼装助手、临时目录助手 |
| `tests/util_test.cpp` | 新增 | A 组：Slice/Status/coding/crc32c/Arena/Env/InternalKey |
| `tests/memtable_test.cpp` | 新增 | A 组：跳表/MemTable/迭代器/冻结 + B 组压力边界 |
| `scripts/lsm_build.sh` | 新增 | 干净重建 + 断言 0 warning + 跑 `lsm_tests` 并打印计数 |
| `docs/roadmap.md`, `docs/protocol.md`, `docs/m1-design.md`, `docs/m1-prerequisites.md` | 新增 | 本阶段文档 |
| `docs/m1-tdd-red.log` | 新增 | #2 阶段的 RED 原始输出 |
| `.gitignore` | 新增 | `build*/`、`data/`、`*.o`、`*.a`、`.cache/` |

禁止改动：`~/raft-kv` 任何文件。

## 4. 对外接口签名

### 4.1 `Slice`（src/common.h）

```cpp
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}      // NUL 结尾便捷构造
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  const char* data() const;  size_t size() const;  bool empty() const;
  char operator[](size_t n) const;
  void clear();
  int compare(const Slice& b) const;     // 逐字节无符号，短者为小
  bool starts_with(const Slice& x) const;
  std::string ToString() const;          // 唯一的显式拷贝出口
 private:
  const char* data_;  size_t size_;
};
```

**生命周期契约（写进头文件注释，测试覆盖 `ToString`）**：
- `Slice` 是视图，不持有内存；仅在「来源对象存活且未被修改」的窗口内有效。
- 跨越调用边界（返回值、容器元素、异步任务）必须 `ToString()`。
- 库内部（`MemTable`/跳表/迭代器）持有的 `Slice` 一律指向 **Arena 内存**，其生命周期 = `MemTable` 生命周期。

### 4.2 `Status`

```cpp
class Status {
 public:
  enum Code { kOk = 0, kNotFound = 1, kCorruption = 2, kNotSupported = 3,
               kInvalidArgument = 4, kIOError = 5, kFrozen = 6 };
  Status();                                     // kOk
  static Status OK();
  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice());
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice());
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice());
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice());
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice());
  static Status Frozen(const Slice& msg, const Slice& msg2 = Slice());
  bool ok() const;
  bool IsNotFound() const;  bool IsCorruption() const;   bool IsNotSupported() const;
  bool IsInvalidArgument() const; bool IsIOError() const; bool IsFrozen() const;
  Code code() const;
  std::string ToString() const;                 // "OK" / "<CodeName>: <msg>[: <msg2>]"
 private:
  Code code_;  std::string msg_;
};
```

M1 可产生性（防止「声明了但没人产生」的悬空码）：

| 码 | M1 产生点 |
|---|---|
| `kOk` | 全部成功路径 |
| `kNotFound` | `DB::Get` 未命中 / 命中 tombstone |
| `kInvalidArgument` | 空 key、超长 key、`Open` 参数非法 |
| `kNotSupported` | `DB::Open` 且 `name` 非空（M1 仅内存模式） |
| `kFrozen` | `MemTable::Add` / `DB::Put`/`Delete` 在冻结后 |
| `kIOError` | `Env` 文件操作失败（`util_test` 覆盖） |
| `kCorruption` | M1 不产生：预留给 M2/M3 的 CRC 与格式校验（枚举一次冻结，避免后续改枚举） |

### 4.3 `Comparator` / `Options` / 命名空间

**全部对外符号位于 `namespace lsm`**（`tests/test_harness.h` 已按此 include 与限定名书写，属 #2 冻结的事实契约）。

```cpp
class Comparator {
 public:
  virtual ~Comparator() = default;
  virtual int Compare(const Slice& a, const Slice& b) const = 0;
  virtual const char* Name() const = 0;
};
const Comparator* BytewiseComparator();     // 进程级单例，无状态、线程安全（L6）

struct Options {
  const Comparator* comparator = BytewiseComparator();
  size_t write_buffer_size = 4 * 1024 * 1024;   // 4 MiB
};
```

**M1 的 `Options` 只有这两个字段**；任何「看起来能用但没实现」的字段一律不加（评审项 §4.1）。

### 4.4 `Iterator`（src/common.h）

```cpp
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  virtual void Seek(const Slice& target) = 0;   // target = user key
  virtual void Next() = 0;
  virtual void Prev() = 0;
  virtual Slice key() const = 0;                // 用户视图：user key；内部序迭代器：internal key
  virtual Slice value() const = 0;
  virtual Status status() const = 0;
};
```

用户视图迭代器状态机（`DB::NewIterator()` 出去的迭代器；M1 实现于 `db.cc`）：

| 状态 | 进入方式 | `Next()` | `Prev()` | `Valid()` |
|---|---|---|---|---|
| `kBeforeFirst` | 构造后 / `SeekToFirst` 空表 / `Seek` 无结果 | `SeekToFirst()` | 保持 `kBeforeFirst`（nothing before first） | false |
| `kValid` | 命中可见条目 | 移到下一个可见 user key | 移到上一个可见 user key | true |
| `kPastEnd` | `Next()` 越过最后一条 | 保持 `kPastEnd` | `SeekToLast()` 语义（回到最后一条可见条目） | false |

- `Seek(target)`：定位到 **≥ target 的第一个可见 user key**；无则 `kPastEnd` 且 `!Valid()`。
- `SeekToLast()`：最后一条可见 user key；空表 → `kBeforeFirst` 且 `!Valid()`。
- 空表上任何 Seek/Next/Prev 都不得 UB；终态是 `!Valid()`。
- `key()`/`value()` 仅在 `Valid()` 时可调用（前置条件，头文件注释写明）。
- 可见性定义：每个 user key 只输出**其最新版本**；若最新版本是 tombstone，则该 user key 不可见。
- 并发写下的可见性（I5）：向前遍历**可能**看到「当前位置之后」新插入的条目（不保证不看到），
  但**已建立的位置与已返回的结果不受影响**。这条是 M1 迭代器测试的判据。
- 内部序迭代器（`MemTable::NewIterator()`）语义：按内部 key 序（§protocol 6.1），多版本全部可见，
  `key()` 返回 internal key，`Seek(target)` 的 target 是 **internal key**，状态机相同但无去重/跳 tombstone。

### 4.5 `DB`（src/db.h）

```cpp
class DB {
 public:
  static Status Open(const Options& options, const std::string& name, DB** dbptr);
  DB() = default;
  virtual ~DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const Slice& key) = 0;
  virtual Status Get(const Slice& key, std::string* value) = 0;
  virtual Iterator* NewIterator() = 0;    // 调用方负责 delete
};
```

`Open` 语义：
- `name` 为空串 → 内存模式，成功时 `*dbptr != nullptr`。
- `name` 非空 → `Status::NotSupported("DB::Open: M1 supports memory mode only (name must be empty)")`，`*dbptr = nullptr`，**不产生半构造对象**。
- `options.comparator == nullptr` 或 `options.write_buffer_size == 0` → `kInvalidArgument`。
- 失败路径一律不泄漏、不返回半构造对象（RAII，评审项 §4.7）。

## 5. 内部 key 编码与比较

完整位级布局、编解码、比较规则、畸形输入判定见 `docs/protocol.md` §6（**逐字实现，不得各写一套**）。
M1 需要落地的自由函数（`src/common.h`）：

```cpp
using SequenceNumber = uint64_t;
enum ValueType : uint8_t { kTypeDeletion = 0x0, kTypeValue = 0x1 };
constexpr SequenceNumber kMaxSequenceNumber = (1ULL << 56) - 1;
constexpr size_t kInternalKeyTrailerSize = 8;
constexpr size_t kInternalKeyMinSize = 8;
constexpr size_t kMaxUserKeySize = 64 * 1024;

uint64_t PackTrailer(SequenceNumber seq, ValueType type);
SequenceNumber ExtractSequence(uint64_t trailer);
ValueType ExtractValueType(uint64_t trailer);
Slice ExtractUserKey(const Slice& internal_key);              // 前置条件：size() >= 8
bool ParseInternalKey(const Slice& internal_key, Slice* user_key,
                      SequenceNumber* seq, ValueType* type);   // 畸形 → false，不修改输出
std::string BuildInternalKey(const Slice& user_key, SequenceNumber seq, ValueType type);
std::string BuildLookupKey(const Slice& user_key, SequenceNumber snapshot);   // protocol §6.2

class InternalKeyComparator : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* user_comparator);
  int Compare(const Slice& a, const Slice& b) const override;
  const char* Name() const override;               // "leveldb.InternalKeyComparator"（借 LevelDB 名以便辨识）
  const Comparator* user_comparator() const;
};
```

## 6. Arena（src/util/arena.{h,cpp}）

```cpp
class Arena {
 public:
  Arena();
  ~Arena();
  Arena(const Arena&) = delete;  Arena& operator=(const Arena&) = delete;
  char* Allocate(size_t bytes);                    // 对齐 alignof(std::max_align_t)
  char* AllocateAligned(size_t bytes, size_t align = alignof(std::max_align_t));
  size_t MemoryUsage() const;                      // 已申请块字节总和（含块内未用尾部）
  void Reset();                                    // 丢弃全部块，MemoryUsage() 归 0
 private:
  static constexpr size_t kBlockSize = 4096;
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);
  char* alloc_ptr_ = nullptr; size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;  size_t memory_usage_ = 0;
};
```

- 大块（`bytes > kBlockSize/4`）单独分配「恰好足够」的块，不吃小块的剩余空间。
- **分配失败处理（显式决策）**：`Allocate*` 在 `operator new` 失败时打印可定位信息到 `stderr` 并 `std::abort()`。
  理由：本项目不用异常做控制流；OOM 属不可恢复环境错误，fail-fast 优于把 `nullptr` 检查扩散到整条跳表插入路径
  （LevelDB 的 `new` 抛 `std::bad_alloc` 同样以终止收场）。备选方案（`Allocate` 返回 `nullptr` + `Status` 逐层上抛）
  记录在此，若评审判定为阻断项则回 `#0` 修订。
- 统计口径：`MemoryUsage()` 在 `Reset()` 之前**单调不减**；`Reset` 后归零。
- 线程安全：M1 不做线程安全 Arena（单写者，L1/L2）。

## 7. 跳表（src/skiplist.h）

```cpp
class Skiplist {
 public:
  static constexpr int kMaxHeight = 12;          // 支持 4^12 ≈ 1.6e7 条不退化
  static constexpr uint32_t kBranching = 4;      // p = 1/4

  Skiplist(Arena* arena, const Comparator* cmp, uint32_t rng_seed = 0x9E3779B9u);

  void Insert(const Slice& key);   // 前置条件：key 字节的生命周期 ≥ Skiplist（生产路径 = Arena）
  bool Contains(const Slice& key) const;
  class Iterator {                 // 双向；Seek/SeekToFirst/Next/Prev/SeekToLast
   public:
    bool Valid() const; Slice key() const;
    void SeekToFirst(); void SeekToLast(); void Seek(const Slice& target);
    void Next(); void Prev();
   private:
    const Skiplist* list_;  const Node* node_;
  };
  Iterator* NewIterator() const;   // 调用方 delete
  struct Stats { size_t node_count; size_t level_sum; size_t level_histogram[kMaxHeight]; };
  Stats GetStats() const;          // O(n) 诊断接口：供层高分布验收测试（不影响写路径）
 private:
  struct Node {
    Slice key_;
    std::atomic<Node*> next_[kMaxHeight];        // 固定数组：零 UB，代价见 §7.3
  };
  bool Equal(const Slice& a, const Slice& b) const;
  int Compare(const Slice& a, const Slice& b) const;
  int RandomHeight();
  Node* NewNode(const Slice& key, int height);   // Arena 分配 + placement new
  ...
  Arena* const arena_;  const Comparator* const compare_;  Node* const head_;
  std::atomic<int> max_height_{1};  Random rnd_;
};
```

### 7.1 随机层高（可复现）

```
RandomHeight():
  height = 1
  while (height < kMaxHeight && rnd_.Next() % kBranching == 0) height++
  return height
```

`Random` 是 skiplist.h 内的私有 Park–Miller（minimal standard）PRNG：
`seed = seed * 16807 % (2^31-1)`，初值由构造参数给出（默认 `0x9E3779B9`，0/2147483647 归一到 1）。
**不用 `<random>`**：分布实现跨平台不一致会破坏「固定种子可复现」的验收口径。
期望：`E[height] = 1/(1-p) = 4/3 ≈ 1.333`；`P(height ≥ k) = (1/4)^(k-1)`。

### 7.2 路径

- `Insert`：`FindGreaterOrEqual(key, prev[])` 求每层前驱 → 取 `RandomHeight()` → 建节点 →
  **先写 `next_[i]`，再 release 发布到 `prev[i]->next_[i]`**（自底向上），最后按需更新 `max_height_`。
- `Contains`：从 `max_height_-1` 层起，`while (next && Compare(next->key, key) < 0) x = next`。
- 迭代器 `Next/Prev`：`Prev` 用「逐层向前退到 < target 的最大节点」实现，O(log n) 摊销。
- 节点只增不删（tombstone 是数据不是删除）；内存回收 = Arena 整体释放。

### 7.3 布局取舍（显式记录）

| 方案 | 内存（每节点指针开销） | 风险 |
|---|---|---|
| 固定 `next_[kMaxHeight]`（**采纳**） | 96 B | 无 UB；MemTable 上限 4 MiB 时约 30 k 节点 → ~3 MB 指针，可接受 |
| 柔性数组 `next_[1]` + 按层高分配（LevelDB 式） | ~11 B（E[h]=1.33） | 省 4×，但 `next_[i>0]` 是标准未定义行为（GCC/Clang 惯用法），评审面变大 |

采纳固定数组：M1 的验收含「maxHeight 与数组长度一致性」，固定数组让 `height <= kMaxHeight` 可直接断言。
若后续（M4/M5）实测内存成为瓶颈，再回 `#0` 评估柔性数组。

### 7.4 重复 key 语义

- `Skiplist` 允许相等 key 并存（**多重集语义**）：`Insert` 把新节点插到所有相等 key 之前，`Contains` 为真，
  遍历会看到多条。测试用 `std::multimap` 对账。
- `MemTable` 层因内部 key 含 sequence（唯一）而**永不出现重复**，故多重集语义不会泄漏到 DB 语义。

### 7.5 并发契约

M1 允许**单写者 + 并发多读者**：节点一经 release 发布即不可变（I3），读者用 acquire 读 `next_`，无需加锁（L1）。
跳表**不支持**并发写（M1 无此需求）。

## 8. MemTable（src/memtable.{h,cpp}）

```cpp
class MemTable {
 public:
  enum class GetResult { kFound, kDeleted, kNotFound };

  MemTable(const InternalKeyComparator& internal_comparator, size_t write_buffer_size);
  ~MemTable();
  MemTable(const MemTable&) = delete;  MemTable& operator=(const MemTable&) = delete;

  Status Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);
  GetResult Get(const Slice& lookup_key, std::string* value) const;
  Iterator* NewIterator() const;                  // 内部 key 序，多版本可见
  bool Freeze();                                  // 幂等：true = 本次完成冻结
  bool IsFrozen() const;
  size_t ApproximateMemoryUsage() const;
  size_t NumEntries() const;                      // 含 tombstone
  const InternalKeyComparator& internal_comparator() const;
};
```

### 8.1 `Add`

1. 前置校验：`key` 非空且 `size() <= kMaxUserKeySize`；`seq <= kMaxSequenceNumber`；`type ∈ {0,1}`。
   违规 → `Status::InvalidArgument(...)`，**不写入、不消耗 sequence**。
2. 容量（**写前判**）：若 `IsFrozen() || ApproximateMemoryUsage() >= write_buffer_size_`
   → `Freeze()` 并返回 `Status::Frozen("MemTable: write buffer full; frozen")`（**不静默丢弃**）。
   注意：判据在插入前评估，因此最后一次成功写入可以让用量略微超过上限（LevelDB 同口径）。
3. 编码成条目（`docs/protocol.md` §7），Arena 分配，跳表 `Insert`，`entry_count_++`。
4. 成功返回 `Status::OK()`。

无 IO、无系统时间、无全局状态、无锁；全部外部依赖经构造参数注入（为 M2 崩溃测试铺路）。

### 8.2 `Get`

- `lookup_key` = user_key + 8 字节 trailer（读快照由调用方决定，见 §protocol 6.2）。
- 把 `lookup_key` 包成探针条目（`varint32(len) | lookup_key | varint32(0)`）后 `Skiplist::Seek`：
  - 未命中或命中条目的 user key ≠ 目标 → `kNotFound`
  - 命中 `kTypeDeletion` → `kDeleted`（`value->clear()`）
  - 命中 `kTypeValue` → `*value = value bytes`，`kFound`
- 探针缓冲：≤200 字节走栈缓冲，超过走 `std::string` 兜底（避免每次 Get 都堆分配；LevelDB `LookupKey` 同口径）。

### 8.3 容量、冻结与统计

- `write_buffer_size` 由 `Options` 传入，MemTable 独占持有。
- `Freeze()`：`frozen_.store(true, std::memory_order_release)`；幂等；已冻结返回 `false`。
- `IsFrozen()`：`frozen_.load(std::memory_order_acquire)`。
- **冻结后查询与迭代照常可用**（不可写、可读）。
- `ApproximateMemoryUsage()` = `arena_.MemoryUsage() + sizeof(MemTable)`；MemTable 生命周期内**单调不减**。
- `NumEntries()` = 成功 `Add` 的条目数（含 tombstone）；单调不减。
- 不提供「user key 去重计数」：需要额外结构，M1 不引入（避免未实现语义的统计字段）。

## 9. DB 的 M1 内存实现（src/db.{h,cpp}）

```cpp
class DBImpl : public DB {
  Options options_;
  InternalKeyComparator internal_comparator_;
  std::unique_ptr<MemTable> memtable_;
  SequenceNumber last_sequence_ = 0;

  Status Write(ValueType type, const Slice& key, const Slice& value);
  static Status ValidateKey(const Slice& key);
};
```

- `Put` / `Delete` → `Write(...)`：
  ```
  ValidateKey(key)（空/超长 → kInvalidArgument）
  s = memtable_->Add(last_sequence_ + 1, type, key, value)
  if (s.ok()) last_sequence_++        // 冻结被拒时**不消耗** sequence（I1 单调不减）
  return s
  ```
- `Get`：`BuildLookupKey(key, last_sequence_)` → `MemTable::Get`。
  `kFound` → `kOk` 并写 `*value`；`kDeleted` → `NotFound("Get: key is deleted", key)`；`kNotFound` → `NotFound("Get: key not found", key)`。
  错误 message 里的 key 截断到 64 字节（可定位且不放大日志）。
- `NewIterator()` → `class UserIterator : public Iterator`（`db.cc` 内文件局部类）：
  包装 `memtable_->NewIterator()`（拥有并负责 delete），实现 §4.4 的状态机；跳过同 user key 的旧版本与 tombstone。
  - `Seek`：内部 `Seek(BuildInternalKey(target, kMaxSequenceNumber, kValueTypeForSeek))` → 前向找第一个可见条目。
  - `Prev`：先跨过当前 user key 的所有版本（`Prev` 直到 user key 变化），若落点是 tombstone 则继续跨过该 user key。
  - M1 只有一层 MemTable，**不含合并逻辑**；M3 用 MergingIterator/DBIter 替换实现，用户可见语义不变。
- 生命周期：迭代器持 `const MemTable*` 裸引用；`MemTable` 必须先于迭代器释放（L5）。
  `DBImpl` 析构顺序：先销毁内存表还是先要求调用方销毁迭代器——由文档约定「迭代器不得晚于 DB 存活」（L4/L5，测试覆盖 UAF 场景用 ASan 兜底）。

## 10. 最小 Env（src/util/env.h + env_posix.cpp）

```cpp
class WritableFile { public: virtual ~WritableFile(); virtual Status Append(const Slice&) = 0;
  virtual Status Flush() = 0; virtual Status Sync() = 0; virtual Status Close() = 0; };
class SequentialFile { public: virtual ~SequentialFile();
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0; virtual Status Skip(uint64_t n) = 0; };
class Env {
 public:
  virtual ~Env();  static Env* Default();
  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) = 0;
  virtual Status NewSequentialFile(const std::string& fname, SequentialFile** result) = 0;
  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual uint64_t NowMicros() = 0;
  virtual void SleepForMicros(uint64_t micros) = 0;
};
```

- M1 用途：仅 `util_test`（真实临时目录，覆盖 `kIOError` 路径）；M2 的 WAL 直接使用。
  它是指令文件清单要求的落地项，且被测试真实覆盖，不是死代码。
- M1 **不写**任何 WAL/SSTable 相关符号；`RandomAccessFile` 留到 M3（避免未使用接口）。
- 时间用 `clock_gettime(CLOCK_MONOTONIC)`（M2 崩溃恢复的时序判据依赖单调时钟，不用 `CLOCK_REALTIME`）。

## 11. 测试矩阵

### A 组（确定性：无真实磁盘**语义**依赖、无真实时间依赖、无网络、零 flaky）

`tests/util_test.cpp`

| 用例 | 断言要点 |
|---|---|
| `Slice.CompareAndStartsWith` | 逐字节无符号序、短者为小、空 Slice、`starts_with` 边界 |
| `Slice.ToString` | 内容一致；空 Slice → `""` |
| `Status.CodesAndToString` | 各工厂的 `code()`/`IsXxx()`/`ToString()` 可读且含 message |
| `Coding.Fixed32/Fixed64RoundTrip` | 0 / 1 / 127 / 128 / 2^31-1 / 2^32-1 / 2^63 / 2^64-1 往返一致；字节序为小端 |
| `Coding.Varint32/Varint64RoundTrip` | protocol §2 全部边界向量 |
| `Coding.VarintRejectsOverflowAndTruncation` | 溢出/截断返回 false，且不修改输出 |
| `Coding.LengthPrefixedRoundTrip` | 空串、64 KiB；长度越界/截断 → false |
| `CRC32C.KnownVectors` | `""`→0、`"123456789"`→0xE3069283、fox 句→0x22620404 |
| `CRC32C.SingleBitFlipDetected` | 对多组数据逐位置翻一位，CRC 必须变化 |
| `Arena.AlignmentAndUsage` | 每次分配 `% alignof(max_align_t) == 0`；`MemoryUsage` 单调不减；大块分配正确 |
| `Arena.Reset` | Reset 后 `MemoryUsage() == 0`，可继续分配 |
| `Env.TimeMonotonic` | `NowMicros` 单调；`SleepForMicros(1ms)` 实测 ≥ 1ms（不依赖真实时间语义以外的假设） |
| `Env.FileRoundTrip` | 临时目录：新建→Append→Sync→Close→顺序读回→文件大小→Rename→Delete；不存在文件 → `kIOError` |
| `InternalKey.BuildParseRoundTrip` | 各种长度 key + seq 上界/0 + 两种 type |
| `InternalKey.ParseMalformed` | 长度 0..7 → false 且不越界；type 非法 → false（ASan 兜底） |
| `InternalKey.CompareOrder` | user key 升序；同 key sequence 降序；同 seq type 降序 |
| `InternalKey.LookupKeySemantics` | `Seek(lookup)` 落在该 user key 最新版本（用 §protocol 6.2 的口径验证） |
| `MemTableKeyComparator.TotalOrder` | 同一 internal key 不同 value 的条目也构成严格全序（反对称+传递性抽样检查） |

`tests/memtable_test.cpp`（跳表 + MemTable）

| 用例 | 断言要点 |
|---|---|
| `Skiplist.InsertContainsWithStdMultimap` | 随机 10 万条（随机顺序插入）全量遍历与 `std::multimap` 对账：顺序与内容完全一致 |
| `Skiplist.RandomLayerDistribution` | 固定种子；`Stats` 实测 `E[height] ∈ [1.28,1.39]`，`P(h≥2) ∈ [0.23,0.27]`，`P(h≥3) ∈ [0.055,0.07]`，`max height <= kMaxHeight` |
| `Skiplist.IteratorForwardBackward` | `SeekToFirst/Next` 与 `SeekToLast/Prev` 步进序列互为逆序；`Seek` 到不存在 key 落在下一个更大条目 |
| `Skiplist.DuplicateKeys` | 多重集语义：重复插入后遍历可见重复条目，`Contains` 为真 |
| `MemTable.PutGetRoundTrip` | 单条/多条往返；空 value 合法；1 MiB value 可读回 |
| `MemTable.OverwriteNewestWins` | 同 key 多次 Put，Get 返回最新 |
| `MemTable.DeleteThenNotFound` | Delete 后 Get = `kDeleted`（DB 层 `kNotFound`）；再次 Put 后新值可见 |
| `MemTable.MultiVersionOrderInInternalIterator` | 同 key 多版本按 sequence 降序；user key 升序 |
| `MemTable.UserIteratorDedupAndTombstone` | 用户视图：同 key 多版本只出最新；tombstone 不出；`Seek` 落在 ≥ target 的第一个可见 key |
| `MemTable.IteratorStateMachine` | 空表 / `SeekToFirst`+`Prev` / `Next` 越界后 `Prev` 回末条 / 越界后 `Next` 保持 `!Valid` |
| `MemTable.InsertDuringIteration` | 迭代中插入「当前位置之前」的 key：已建立位置与已返回结果不变（I5） |
| `MemTable.FreezeRejectsWriteWithFrozenStatus` | 写超限 → 后续 `Add` 返回 `kFrozen` 且不写入；`IsFrozen()` 为真 |
| `MemTable.FrozenStillReadable` | 冻结后 `Get`/迭代照常可用（覆盖旧值与新值） |
| `MemTable.CapacityStatsMonotonic` | `ApproximateMemoryUsage()`/`NumEntries()` 单调不减；`Add` 被拒时二者不变 |
| `MemTable.RejectsEmptyAndOversizedKey` | 空 key、>64 KiB key → `kInvalidArgument`，且统计与 sequence 不变 |

### B 组（压力/边界，进程内放大规模）

| 用例 | 断言要点 | 记录 |
|---|---|---|
| `Stress.OneMillionKeysReconcile` | 100 万条随机 key 写入后全量对账（`std::multimap`） | 耗时、`ApproximateMemoryUsage()` |
| `Stress.DeleteThirtyPercentReconcile` | 随机删除 30% 后对账（tombstone 计入条目数，不计入可见 key） | 条目数与可见 key 数 |
| `Stress.SameKey100kTimes` | 同一 key 写 10 万次：Get 返回最新；内部迭代 10 万条；用户视图只出现 1 个 user key | 耗时、内存 |
| `Stress.AlignmentUnderSanitizers` | 显式 `alignof` 断言 + ASan/UBSan 下无对齐/越界报告（由门禁脚本执行） | sanitizer 结论 |

## 12. 子里程碑拆分与提交计划

| 阶段 | 内容 | 通过判据 | 提交信息（引用章节） |
|---|---|---|---|
| M1.1 | `CMakeLists.txt`（双目标 + ASan/TSan 选项）、`common.h`、`status`、`coding`、`crc32c`、`arena`、最小 `Env`、`.gitignore`、`scripts/lsm_build.sh` | `util_test` 全绿；干净重建 0 warning | `m1.1: 工程骨架与 util 层（docs/m1-design.md §4/§6/§10）` |
| M1.2 | `skiplist.h`、`memtable.{h,cpp}`、内部 key 迭代器、`db.{h,cpp}`（内存实现 + 用户视图迭代器） | `memtable_test` A 组全绿（含 10 万条对账） | `m1.2: 跳表 + MemTable + 迭代器（docs/m1-design.md §5/§7/§8/§9）` |
| M1.3 | B 组压力补齐、迭代器语义打磨、门禁收口（ASan + TSan + 0 warning） | 全部测试绿 + ASan/TSan 干净 + tag `m1-memtable` | `m1.3: 压力边界与门禁收口（docs/m1-design.md §11）` |
| #4 评审 | 独立评审 → 阻断项修复 + 补回归 + 重跑全部验收 | 无阻断项 | `m1.4: 评审阻断项修复（docs/m1-review.md §…）` |

文档提交：`docs(m1): 冻结 M1 设计（design/protocol/roadmap/prerequisites）`（#0+#1 产物一个提交）。
每个提交 push 到 `origin`（`git@github.com:hulangMonster/lsm.git`），M1 收口后 `git tag m1-memtable && git push origin m1-memtable`。

**修订记录（#1 回退 #0，2026-09-29）—— M1.1 的验证口径**：本表初稿写「M1.1 判据 = `util_test` 全绿 + 干净重建 0 warning」，
但 `lsm_tests` 是**单一可执行文件**，不存在「部分链接」：M1.1 只有 util 层时若把 `tests/memtable_test.cpp` 一并编入，
整个二进制都链接失败，`util_test` 也跑不起来（该失败正是 `docs/m1-tdd-red.log` 记录的现象）。
落地口径（不改指令要求的「lsm 静态库 + lsm_tests 测试可执行文件」目标布局）：

- **M1.1**：`CMakeLists.txt` 里 `lsm_tests` 只含 `tests/util_test.cpp`（CMakeLists 注释写明 M1.2 会加回），
  因此 `lsm` 改成 STATIC 并只列 `src/util/*.cpp`；门禁 = `bash scripts/lsm_build.sh` 干净重建 0 warning + `util_test` 全绿。
- **M1.2**：`lsm_tests` 加回 `tests/memtable_test.cpp`，`lsm` 列全 `src/**`；门禁 = A 组全绿。
- `#2` 的 RED 证据不受影响（那一版 CMakeLists 两个测试文件都在，原始输出已留档）。

## 13. #2 阶段 RED 策略（为什么这样能拿到真实 RED）

**修订记录（#1 阶段回退 #0，2026-09-29）**：本节初稿写「`lsm` 用 INTERFACE 目标 → 得到链接期 `undefined reference`」。
`#1` 校验时发现该路径不可达：测试文件要 `#include "common.h"` 等头文件，而 `#2` 按指令「不新增任何实现文件」，
头文件尚不存在 → RED 发生在**编译期**（`fatal error: common.h: No such file or directory`），不可能走到链接期。
改为如实分两段留档，**影响面仅限 #2 的证据形式，不动任何接口/不变量/测试矩阵**：

- **#2（编译期 RED）**：`CMakeLists.txt` 里 `lsm` 用 `add_library(lsm INTERFACE)`（无源文件），`lsm_tests` 正常配置；
  构建在编译测试文件时失败于缺失头文件。原始输出落 `docs/m1-tdd-red.log` 的 `## #2 编译期 RED` 小节。
  证据含义：**测试集已完整表达契约，实现为零**。
- **M1.1（链接期 RED）**：头文件与部分 util 实现落地后，首次构建出现链接期 `undefined reference to lsm::...`，
  逐个对应尚未实现的符号。原始输出追加进同一文件（`## M1.1 链接期 RED` 小节），RED 随子里程碑逐步收敛。
- 不在 #2 阶段为了凑链接期错误而预写头文件——那会把「测试先行」变成「实现先行」。

## 14. 自检（占位符 / 内部矛盾 / 歧义 / 范围越界）

- **占位符**：无 TODO/stub；§4.2 已逐码标注 M1 产生点；`Options` 只有两个字段；`Env` 只含会被 `util_test` 覆盖的接口。
- **内部矛盾**：已澄清两处（§1.4 依赖方向、M1 不含 Options 化的 Put/Get）。`docs/roadmap.md` §0 与本文件一致。
- **歧义**：迭代器三态状态机（§4.4）、冻结语义（§8.3）、重复 key 语义（§7.4）、空表行为（§4.4）都已给出判定表。
- **范围越界**：M1 无 WAL/SSTable/flush/compaction/Bloom/Batch/快照/组提交；用户视图迭代器是单 MemTable 的薄封装，
  不含合并逻辑，M3 替换为 DBIter 时用户语义不变 —— 这不是提前实现 M3，而是 M1 验收（「遍历只出现一个 user key」）所必需。
- **已知取舍待评审**：Arena OOM 用 `abort()`（§6）；跳表固定 `next_[12]`（§7.3）。（两条都已写明备选方案与回退条件。）
