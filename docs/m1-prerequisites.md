# M1 实现前置校验（docs/m1-prerequisites.md）

> `#1` 阶段产物。**本文件不写实现代码**；它把 `docs/m1-design.md` + `docs/protocol.md` 冻结成一组可校验的
> 不变量（I）、锁纪律（L）、风险、文件清单、边界全集、未定义行为清单与测试前置假设。
>
> 流程锁：`#1` 阶段发现缺陷必须回退 `#0` 修改设计，**禁止在校验阶段私自改实现方案**。

## 0. 校验范围与结论

- 校验对象：`docs/m1-design.md`（§1~§14）与 `docs/protocol.md`（§1~§8）。
- 结论：设计自检（design §14）通过；下面登记的不变量/纪律/风险/边界**逐条可测**，未发现需要回退 `#0` 的阻断缺陷。
- 两条已登记的取舍（Arena OOM 用 `abort()`、跳表固定 `next_[12]`）在 design §6/§7.3 写明备选与回退条件，`#4` 评审可推翻。

## 1. 不变量（I1~I10）——「谁保证 + 怎么验」

| # | 不变量 | 保证方 | 验证手段（M1 必须实测） |
|---|---|---|---|
| I1 | 内部 key `(user_key, sequence, type)` 唯一；sequence 单调不减 | `DBImpl::Write` 只在 `Add` 成功后才推进 `last_sequence_`；MemTable 内 key 含 sequence 故不重复 | `MemTable.*`、`Stress.SameKey100kTimes`；断言冻结被拒时 sequence 不推进 |
| I2 | 同一 user key 的版本按 sequence 降序排列；Get 命中 ≤ 快照的最大 sequence | `InternalKeyComparator`（trailer 降序）+ `BuildLookupKey` | `InternalKey.CompareOrder`、`InternalKey.LookupKeySemantics`、`MemTable.MultiVersionOrderInInternalIterator` |
| I3 | 已发布节点不可变：写指针 release、读指针 acquire，读路径无锁 | 跳表 `Insert` 只写新节点的 `next_` 后再发布；节点 key/value 不再改 | `Skiplist.*` 全量对账；`#4` 评审核对内存序；TSan 构建无竞争报告 |
| I4 | tombstone 屏蔽**更小** sequence 的同 user key 值，对更大 sequence 无效 | `MemTable::Get` 命中删除即返回 `kDeleted`；DB 层转 `kNotFound` | `MemTable.DeleteThenNotFound`、`MemTable.DeleteThenPutNewerStaysVisible` |
| I5 | 迭代器稳定性：已建立的位置与已返回的结果不受后续插入影响 | 跳表节点不可变 + 迭代器持节点指针（`Next/Prev` 沿 `next_` 走） | `MemTable.InsertDuringIteration` |
| I6 | 容量统计单调不减；达到上限后进入只读冻结，后续写入返回明确 Status（不静默丢弃） | `MemTable::Add` 写前判 + `Freeze()` + `Status::Frozen` | `MemTable.CapacityStatsMonotonic`、`MemTable.FreezeRejectsWriteWithFrozenStatus` |
| I7 | Slice 不逃逸来源对象生命周期；需要保留处必须 `ToString()` | 头文件生命周期契约 + 库内 Slice 一律指向 Arena | `Slice.ToString`；`#4` 评审逐处核对；ASan 兜底悬垂读 |
| I8 | 无异常路径：失败经 `Status` 返回；资源由 RAII 释放 | 全部公开 API 返回 `Status`；析构/`unique_ptr` 覆盖资源 | `Status.*`、`DB::Open` 失败路径（`*dbptr=nullptr`、无泄漏，ASan） |
| I9 | 内存安全与对齐：Arena 满足 `alignof(std::max_align_t)`；ASan/UBSan 零报告 | `Arena::Allocate` 对齐计算；跳表固定数组 | `Arena.AlignmentAndUsage`、`Stress.AlignmentUnderSanitizers` |
| I10 | 边界确定：空 user key/超长 key → `kInvalidArgument`；空 value 合法；重复 Put 覆盖 | `DBImpl::ValidateKey` + `MemTable::Add` 前置校验 | `MemTable.RejectsEmptyAndOversizedKey`、`MemTable.OverwriteNewestWins` |

## 2. 线程安全契约与锁纪律（L1~L6，M1 建立、后续沿用）

| # | 纪律 | M1 落地方式 | 验证 |
|---|---|---|---|
| L1 | 单写者 + 并发多读者；读者不加锁依赖 I3 | 跳表 `next_` 为 `std::atomic<Node*>`，读 acquire / 写 release | TSan 构建 + `#4` 评审核对内存序 |
| L2 | 容量统计与冻结状态由写者单方修改；读者只读到已发布值 | `frozen_` 用 `std::atomic<bool>`（release/acquire）；`entry_count_`/Arena 统计**不做原子化**，M1 声明其并发读为未定义（M5 再评估） | 代码评审；M1 不写并发读统计的测试 |
| L3 | 禁止持锁做 IO | M1 无锁、无 IO；契约先立，M2 直接适用 | 评审（M1 无 `mutex`/无文件 IO） |
| L4 | 迭代器不跨线程共享；与写并发的可见性规则显式定义 | design §4.4：向前遍历**可能**看到位置之后的新条目，已建立位置与已返回结果不变 | `MemTable.InsertDuringIteration`；文档对照 |
| L5 | 析构顺序：Arena 晚于所有跳表节点与迭代器；`MemTable` 晚于其迭代器 | `Arena` 由 `MemTable` 持有，`unique_ptr` 成员在 `skiplist_` 之后声明/析构；迭代器持裸引用，文档约定「迭代器不得晚于 MemTable/DB」 | ASan UAF 兜底；`#4` 评审核对成员声明顺序 |
| L6 | 比较器无状态且线程安全 | `Comparator` 纯虚 + 单例 `BytewiseComparator()`；`InternalKeyComparator`/`MemTableKeyComparator` 只持不可变指针 | 评审 + 单例测试 |

## 3. 风险清单（风险 → 触发场景 → 检测 → 缓解）

| 风险 | 触发场景 | 检测手段 | 缓解 |
|---|---|---|---|
| Slice 悬垂（最高频存储引擎 bug） | `Slice(std::string&)` 后源对象析构/被改；`Add` 后跨调用使用 | ASan + `Slice.ToString` 用例 + 评审逐处核对 | 生命周期契约写进头文件；库内 Slice 全部指向 Arena |
| 跳表随机层高越界 | `RandomHeight()` 返回 > `kMaxHeight`；数组下标越界 | `Skiplist.RandomLayerDistribution` 断言 `max height <= kMaxHeight`；ASan | 固定 `next_[kMaxHeight]` + 循环条件 `height < kMaxHeight` |
| 查找路径死循环 | `next_` 自环 / 比较器非严格弱序 | 10 万条对账（顺序+内容）+ 迭代器双向步进 | `MemTableKeyComparator` 定义为严格全序（protocol §7） |
| 内部 key 编解码不对称 / 字节序混用 | 手工拼装 trailer 用主机序；`DecodeFixed64` 误用 `reinterpret_cast` | `InternalKey.BuildParseRoundTrip`、`Coding.Fixed*RoundTrip`（小端断言） | protocol §1 禁止 reinterpret_cast；全部走 coding 函数 |
| 比较器与编码不一致导致多版本排序错乱 | 用 bytewise 比较 entry 而不是先比 internal key | `MemTable.MultiVersionOrderInInternalIterator` | `MemTableKeyComparator` 单点实现，测试用其作为唯一比较入口 |
| Arena 对齐不足导致 UB | `Allocate` 只按 8 字节对齐 / 指针算术对齐丢失 | `Arena.AlignmentAndUsage` 显式 `alignof` 断言；ASan/UBSan | 统一 `AllocateAligned` + 逐块对齐计算 |
| 迭代器失效（冻结/析构后继续用） | 迭代器活过 `MemTable` | ASan 兜底 + 文档约定（L4/L5） | 迭代器持裸引用并在文档写明所有权；M3 起换 `shared_ptr` 再评估 |
| 容量统计 64 位溢出 | 超大 value 累加 | 1 MiB value 用例 + `Stress.OneMillionKeysReconcile` 记录内存 | 统计用 `size_t`；Arena 内部 `uint64_t` 累加 |
| 无异常设计下错误被吞（返回 kOk 实际失败） | `Add` 冻结后仍返回 OK；Env 失败返回 OK | `MemTable.FreezeRejectsWriteWithFrozenStatus`、`Env.FileRoundTrip` 失败分支 | 每个失败路径必须构造带上下文的 `Status`；评审逐条核对 |
| CMake 目标混入测试代码 / sanitizer 目录互相污染 | 把 harness 写进 `src/`；在 `build` 里开 ASan | `scripts/lsm_build.sh` 干净重建 + 三目录独立；评审对照 design §3 | 目录纪律（`src/` 无测试代码、无 `#ifdef` 测试分支） |
| 探针条目缓冲越界 | `MemTable::Get` 栈缓冲 200 字节写超 | ASan + 64 KiB key 边界用例 | 超过 200 字节走 `std::string` 兜底 |
| 固定数组内存放大 | `next_[12]` × 8 B/节点 | `Stress.OneMillionKeysReconcile` 记录 `ApproximateMemoryUsage()` | 已在 design §7.3 记录；若实测不可接受回 `#0` |
| TSan 无法运行 | ASLR 导致 TSan 崩溃 | 门禁脚本用 `setarch $(uname -m) -R` | 已在 `scripts/lsm_build.sh` 固化 |

## 4. 文件清单

| 分类 | 文件 |
|---|---|
| **必须新增（实现）** | `CMakeLists.txt`、`src/common.h`、`src/util/status.cpp`、`src/util/coding.{h,cpp}`、`src/util/crc32c.{h,cpp}`、`src/util/arena.{h,cpp}`、`src/util/env.h`、`src/util/env_posix.cpp`、`src/skiplist.h`、`src/memtable.{h,cpp}`、`src/db.{h,cpp}` |
| **必须新增（测试/脚本）** | `tests/test_harness.h`、`tests/util_test.cpp`、`tests/memtable_test.cpp`、`scripts/lsm_build.sh` |
| **必须新增（文档/杂项）** | `.gitignore`、`docs/m1-tdd-red.log`（#2 原始 RED 输出）、`docs/m1-review.md`（#4 评审） |
| **必须修改** | 无（M1 是首阶段，`CMakeLists.txt` 属新增） |
| **禁止改动** | `~/raft-kv` 任何文件；本仓库不得出现 WAL/SSTable/flush/compaction/Bloom/WriteBatch/快照/组提交的任何符号或占位实现 |

**与指令原文的一处偏离（已在 #0 获批）**：Env 放 `src/util/env.h` + `src/util/env_posix.cpp`（原文写 `src/` 根），
理由：Env 属工具层，放根目录会破坏 `docs/roadmap.md` §0 的单向分层。

## 5. 边界 case 全集

| 输入/场景 | 期望 | 覆盖用例 |
|---|---|---|
| 空 user key | `kInvalidArgument`，不写入、不消耗 sequence | `MemTable.RejectsEmptyAndOversizedKey` |
| user key = 64 KiB | 合法（上限含端点） | `InternalKey.BuildParseRoundTrip` |
| user key = 64 KiB + 1 | `kInvalidArgument` | `MemTable.RejectsEmptyAndOversizedKey` |
| 空 value | 合法，Get 返回空串 | `MemTable.PutGetRoundTrip` |
| 1 MiB value | 合法，可完整读回 | `MemTable.PutGetRoundTrip` |
| 同一 key 重复 Put N 次 | Get 返回最后一次；用户视图只出现 1 个 user key | `MemTable.OverwriteNewestWins`、`Stress.SameKey100kTimes` |
| Delete 后 Get | `kDeleted`（DB 层 `kNotFound`） | `MemTable.DeleteThenNotFound` |
| Delete 后再 Put | 新值可见（sequence 更大） | `MemTable.DeleteThenPutNewerStaysVisible` |
| Delete 不存在的 key | `kOk`（写 tombstone） | `MemTable.DeleteThenNotFound`（前置） |
| Seek 到不存在的 key | 落在下一个更大的可见 user key | `MemTable.UserIteratorDedupAndTombstone` |
| Seek 到超过最大 key | `kPastEnd` 且 `!Valid()`；再 `Next` 仍 `!Valid()` | `MemTable.IteratorStateMachine` |
| 空 MemTable 的 Seek/SeekToFirst/SeekToLast/Next/Prev | 全部 `!Valid()`，无 UB | `MemTable.IteratorStateMachine` |
| `Prev()` 在 `kPastEnd` | 回到最后一条可见条目 | `MemTable.IteratorStateMachine` |
| `Prev()` 在 `kBeforeFirst` | 保持 `!Valid()` | `MemTable.IteratorStateMachine` |
| 遍历中插入当前位置之前的 key | 已建立位置与已返回结果不变 | `MemTable.InsertDuringIteration` |
| 写入导致超限 | 本次写返回 `kFrozen`，MemTable 变只读；后续写同样 `kFrozen` | `MemTable.FreezeRejectsWriteWithFrozenStatus` |
| 冻结后 Get/迭代 | 照常可用（旧值+已写入值） | `MemTable.FrozenStillReadable` |
| `DB::Open` 且 `name` 非空 | `kNotSupported`，`*dbptr == nullptr` | `DB` 用例（memtable_test 或 util_test） |
| `DB::Open` 且 `comparator == nullptr` / `write_buffer_size == 0` | `kInvalidArgument`，`*dbptr == nullptr` | 同上 |
| 畸形内部 key（长度 0~7 / type 非法） | `ParseInternalKey` 返回 `false`，不越界读 | `InternalKey.ParseMalformed`（ASan 兜底） |
| varint 溢出/截断 | 解析返回 `false`，不修改输出 | `Coding.VarintRejectsOverflowAndTruncation` |
| Env 打开不存在的文件 / 记录不存在的文件大小 | `kIOError`（带路径上下文） | `Env.FileRoundTrip` |
| 跳表重复 key | 多重集语义：均可见，`Contains` 为真 | `Skiplist.DuplicateKeys` |

## 6. 未定义行为清单（评审逐条禁止）

1. `reinterpret_cast` 到 `uint32_t*`/`uint64_t*` 读写多字节整数（主机序 + 严格别名）。
2. 解码畸形输入时越界读：`ExtractUserKey` 未校验 `size() >= 8`；varint/length-prefix 未校验剩余长度。
3. 未对齐访问（Arena 返回指针必须满足请求对齐）。
4. 悬垂 `Slice`：把指向临时/局部缓冲的 `Slice` 存进跳表或返回给调用方。
5. 迭代器晚于 `MemTable`/`DB` 释放后继续使用（UAF）。
6. `memcpy`/`std::memcmp` 传空指针（即使长度为 0）；统一「长度为 0 时早退」。
7. 有符号整数溢出或把无符号回绕用于比较。
8. 跳表发布路径不用 release/acquire（数据竞争 + 读到半初始化节点）。
9. 迭代器所有权重复释放（`NewIterator()` 的返回值必须由调用方 `delete`，库内不得再持有）。
10. 测试代码访问私有成员（验收只能通过公开行为与 `Stats` 诊断接口）。

## 7. 单元测试前置假设（测试必须显式依赖的假设）

1. `Get` 只查 MemTable —— M1 无 SSTable，故「重启后仍能读到」不属于 M1 判据。
2. M1 不验证并发写：测试单线程；TSan 门禁只证明「已有路径无数据竞争」，不构成并发写正确性证据（那属 M4/M5）。
3. 时间相关行为只经 `Env`；M1 仅在 `Env.TimeMonotonic` 用真实时钟（无 FakeClock）。
4. 磁盘只在 `Env.FileRoundTrip` 用真实临时目录；其余测试不依赖文件系统语义。
5. 跳表测试的 key 字节生命周期由测试自己的 Arena 保证（与生产路径一致）。
6. B 组压力测试必须显式传入足够大的 `write_buffer_size`，否则中途冻结会改变语义（这是有意的：`MemTable.Freeze*` 单独验冻结）。
7. 层高分布测试用固定种子（`Skiplist` 构造参数），结果可复现。
8. 迭代器终态用三态状态机判定（design §4.4），不假设 LevelDB 的「方向翻转」语义。

## 8. 验收命令清单（每子里程碑必须附原始输出）

```bash
# 干净重建 + 0 warning 断言 + 单测计数（scripts/lsm_build.sh 内部执行）
bash scripts/lsm_build.sh

# ASan（独立目录）
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests

# TSan（独立目录；本机需关 ASLR）
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j8 && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests
```

未跑不算过：任何「通过」结论必须粘贴上述命令的原始输出摘录（构建日志尾部、`[  PASSED  ] N tests.`、sanitizer 无报告行）。

## 9. 测试集缺陷登记（#3 实施阶段发现，已按最小改动修正）

### 9.1 `InternalKey.ParseMalformed` 的 type 字节下标写错（自相矛盾，必须修）

- **现象**：该用例构造 `ManualInternalKey("userkey", 42, kTypeValue)` 后，用 `raw[raw.size() - 1] = t` 写入
  「非法 type」并断言 `ParseInternalKey` 返回 false。
- **问题**：protocol §6 的 trailer 是 **8 字节小端**，type 占**最低物理字节**即 `raw[size-8]`；
  `raw[size-1]` 是最高字节 b7，属于 **sequence 字段**（bit 48..55）。
- **可证明的自相矛盾**：该用例要求 `b7 ∈ {2,3,0x10,0x7F,0x80,0xFE,0xFF}` 必须被拒；
  而同一份 `util_test.cpp` 的 `InternalKey.BuildParseRoundTrip` 用 `seq = kMaxSequenceNumber = 2^56-1`
  构造 internal key，其 trailer 的 `b7` 恰好 **= 0xFF**，并被断言必须解析成功。
  任何解析器都不可能同时接受与拒绝 `b7 = 0xFF`。（实测：修复前子集运行中该用例失败，
  报出 `seq = 71776119061217322 = 42 + 0xFF<<48`，即被写坏的是 sequence 而非 type。）
- **处理**：把下标改为 `raw[raw.size() - 8]`（与该行原有注释「trailer 最低物理字节 = type」的原意一致），
  并在测试里写明修订理由。**只改这一个下标，不动任何断言语义**；修订单独成一个提交，原始失败输出入档
  `docs/m1-evidence.md`。这不是「为了通过而改测试」：实现侧未因此放宽任何校验，
  protocol §6「type ∉ {0,1} 视为畸形」的行为保持不变。
- **影响面**：仅该用例；`ParseInternalKey` 的接口、语义与其它用例不受影响。
- **交叉复核**：#4 独立评审者手算验证了 `b7=0x02 ⇒ trailer=0x0200000000002A01 ⇒ seq=562949953421354`，
  与 `docs/m1-evidence.md` 里修复前的原始输出完全吻合，确认「改的是错的物理字节」而非「放松断言」。

### 9.2 `InternalKey.LookupKeySemantics` 尾部两条断言互斥

- 断言 A（原文 1132 行）：`Compare(BuildLookupKey("k",4), BuildInternalKey("k",4,kTypeValue)) == 0`
- 断言 B（原文 1134 行）：`Compare(BuildLookupKey("k",3), BuildInternalKey("k",3,kTypeValue)) < 0`
- 二者结构完全相同（只是 seq 不同）：`kValueTypeForSeek == kTypeValue` 时 `BuildLookupKey(k,s)` 与
  `BuildInternalKey(k,s,kTypeValue)` **逐字节相同**，同一比较器不可能对 s=4 返回 0、对 s=3 返回负数。
- **处理**：把 B 改为 `EXPECT_EQ(0, ...)`（protocol §6.2 的定义），保留覆盖、不削弱其它断言。
  注意 A 与 1133 行（对 `kTypeDeletion` 的 `< 0`）才是 protocol §6.2 真正依赖的性质：同 seq 时
  `kTypeValue` 排在 `kTypeDeletion` 之前，所以 lookup 键会落在删除标记上 → Get 返回 kDeleted。**这两条未改动。**

### 9.3 `MemTable.IteratorStateMachine` 对 seq=0 目标键的落点期望与降序规则互斥

- 断言（原文 660 行）：`Seek(BuildInternalKey("d", 0, kTypeDeletion))` 之后期望落在 `'d'`。
- 但 trailer 是**降序**比较（同一文件 `MultiVersionOrderInInternalIterator` 与
  `InternalKey.CompareOrder` 都用 `a5 < a3` 钉死）：seq=0 是最小 trailer，`("d",0,del)` 排在
  `d` 的**全部版本之后**。按「Seek 落在第一个 ≥ target 的条目」，落点必然是 `'e'`。
  同理 `Seek(BuildInternalKey("b",2,kTypeValue))` 落在 seq=2 的 b 上（该用例另一处断言，说明 Seek
  确实按 internal key 定位，不是按 user key）。
- **处理**：把期望改为 `'e'`，与该行原有注释「Seek 未命中 → 落在下一个更大的条目」的字面含义一致。

### 9.5 迭代器泄漏（`MultiVersionOrderInInternalIterator`，修复于 a7b0052）

- `CollectForward(mem.NewIterator())` / `CollectBackward(mem.NewIterator())` 把裸指针直接交给助手、
  从不 `delete`，违反 design §4.5 自己声明的「`NewIterator()` 返回值所有权归调用方」。
  ASan/LSan 报 `LeakSanitizer: 64 byte(s) leaked in 2 allocation(s)`。
- **处理**：改用 `unique_ptr` 接管（断言与遍历顺序未动）。此条由 #4 评审者指出「§9 漏登记」，现补记。

### 9.4 小结（#2 测试集的系统性偏差）

三处缺陷同源：写测试时把内部 key trailer 的**降序**当成了升序。实现侧未因此放宽任何校验；
三处修订都单独成提交并附证明，任何一处都可用 `git show` 复核。
