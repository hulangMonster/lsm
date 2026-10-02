# M4 设计（docs/m4-design.md）—— 分层 Compaction

> 体例对齐 `docs/m3-design.md`（同章节号骨架：§0 探测 / §1 目标 / §2 开放决策 / §3 位级布局 /
> §4 protocol patch / §5 接口签名 / §6 执行路径 / §7 读路径 / §8 持久化与恢复 / §9 不变量与锁纪律 /
> §10 测试矩阵 / §11 子里程碑 / §12 自检 / §13 需拍板 / §14 参考 / §15 修订记录）。
>
> **本文件是 `#0` 的唯一产出**，是 `#1`（`docs/m4-prerequisites.md`）与 `#2`（测试先行）的唯一依据。
> 每条结论都可追溯"文件名 + 行号"；**未验证**的一律显式标注，不编造。
>
> 引用简写：`M4`=`D:\lsm\lsm-kv-开发指令\M4-分层Compaction.md`（214 行）；
> `D3`=`docs/m3-design.md`；`PR3`=`docs/m3-prerequisites.md`；`P`=`docs/protocol.md`；
> `RM`=`docs/roadmap.md`；`D2`=`docs/m2-design.md`；`NOTE`=`D:\lsm\mine\m4m5\notes.md`（459 行）。

---

## 0. 现状探测与原始证据（`#0` 的硬要求）

### 0.1 探测命令清单（全部只读；未执行任何构建/测试/git 写操作）

```bash
# —— 本机只读克隆 D:\JLProject\lsm-kv（Git Bash）——
cd /d/JLProject/lsm-kv
git log --oneline -3
git status --porcelain
git tag
for f in src/version_set.h src/version_set.cpp src/version_edit.h src/version_edit.cpp \
         src/compaction.h src/compaction.cpp src/merging_iterator.h src/merging_iterator.cpp \
         src/db_iter.h src/db_iter.cpp src/table_cache.h src/table_cache.cpp; do
  if [ -f "$f" ]; then echo "EXIST $f"; else echo "MISSING $f"; fi; done
ls tests/ scripts/ docs/
grep -n '^#\{1,4\} ' docs/m3-design.md
grep -c '^TEST' tests/*.cpp
grep -n 'struct Options' -A 40 src/common.h
cat -n src/filename.h ; cat -n src/filename.cpp
sed -n '1,90p' src/util/env.h
grep -n '^#\{1,3\} ' docs/protocol.md
grep -n 'add_library\|add_executable\|target_link_libraries\|add_test\|ENABLE_ASAN\|ENABLE_TSAN' CMakeLists.txt
grep -rn 'TableOptions\|block_size\|verify_checksums\|max_open_files\|recycle_log_files' src/ tests/
grep -n 'run_gate_marked\|_OK\]\|PARTIAL\|SKIP' scripts/lsm_gate.sh
grep -n 'ROUND\|ACKED\|MISSING\|MISMATCH' scripts/lsm_crash_test.sh
sed -n '1,40p' scripts/crash_writer.cpp ; sed -n '1,45p' scripts/crash_recover.cpp
grep -n 'class MemEnv\|SimulateCrash\|synced_size\|SyncDir\|RenameFile' tests/memenv.h
wc -l docs/m3-design.md docs/m3-evidence.md docs/m2-design.md docs/roadmap.md docs/protocol.md

# —— 虚拟机 ~/lsm-kv（ssh 只读；未跑 cmake/测试）——
ssh ubuntu-vm 'cd ~/lsm-kv && git log --oneline -3'
ssh ubuntu-vm 'cd ~/lsm-kv && git status --porcelain'
ssh ubuntu-vm 'cd ~/lsm-kv && git tag'
ssh ubuntu-vm 'cd ~/lsm-kv && for f in <上述同一清单 + src/db_impl.h src/filename.h src/common.h>; do ...; done'
ssh ubuntu-vm 'cd ~/lsm-kv && ls docs/ tests/ scripts/'
```

### 0.2 探测①：基线 rev / 工作区 / tag（**与 `NOTE` 侦察时点已发生漂移，必须登记**）

VM 原始输出：

```
=== HEAD ===
e56d0b7 feat(m3): M3.1 SSTable 格式层收口（table/index/metaindex + Env 补齐 + 门禁正向标记）
9154c5f fix(m3): 补上 src/sstable/block.h 的私有成员与辅助声明
ffec9eb feat(m3): M3.1 格式层实现（Block 构建/读取/校验 + BlockHandle/Footer 编解码）
=== STATUS ===
（空）
=== TAGS ===
m1-memtable
m2-wal
```

本机克隆原始输出（**与 VM 逐字一致**）：

```
e56d0b7 feat(m3): M3.1 SSTable 格式层收口（table/index/metaindex + Env 补齐 + 门禁正向标记）
（git status --porcelain 为空）
m1-memtable
m2-wal
```

**登记（漂移）**：`NOTE`（`notes.md:6-11`）记录的 VM 基线是 `9154c5f` **且工作区脏**（`M CMakeLists.txt`、
`M src/sstable/block.{h,cpp}`、`?? src/sstable/table.{h,cpp}` …）。本轮实测 VM 已是 **`e56d0b7` 且工作区干净**
⇒ M3.1 的未提交内容已被提交为 `e56d0b7`（`git log --stat` 实测该提交含 `src/sstable/table.{h,cpp}`、
`table_builder.{h,cpp}`、`tests/sstable_table_test.cpp`、`tests/sstable_counting_env.h`、
`docs/protocol.md`(§10 追加)、`scripts/lsm_gate.sh` v2 等 19 个文件）。
⇒ **本设计的一切基线引用以 `e56d0b7` 为准，不以 `NOTE` 的 `9154c5f` 为准。**

tag 仍只有 `m1-memtable` / `m2-wal`（**无 `m3-sstable`**）⇒ M3 未收口（与 `NOTE:131-132` 的判断一致）。

### 0.3 探测②：M4 依赖的 M3 产物存在性（实测矩阵）

| 路径 | VM `~/lsm-kv` | 本机 `D:\JLProject\lsm-kv` | 归属 |
|---|---|---|---|
| `src/version_set.h` / `.cpp` | **MISSING** | **MISSING** | M3.2 计划交付（`D3:2036`） |
| `src/version_edit.h` / `.cpp` | **MISSING** | **MISSING** | M3.2 计划交付（`D3:2036`） |
| `src/compaction.h` / `.cpp` | **MISSING** | **MISSING** | **M4 唯一全新模块**（`M4:34`、`M4:131`） |
| `src/merging_iterator.h` / `.cpp` | **MISSING** | **MISSING** | M3.2 计划交付（`D3:2036`） |
| `src/db_iter.h` / `.cpp` | **MISSING** | **MISSING** | M3.2 计划交付（`D3:2036`） |
| `src/table_cache.h` / `.cpp` | **MISSING** | **MISSING** | **不需要**：`TableCache` 类定义在 `version_set.h`（`D3:1687`、`D3:1863` E6） |
| `src/db_impl.h` / `.cpp` | EXIST | EXIST | M2 已交付，M3.2/M3.3 扩展中 |
| `src/filename.h` / `.cpp` | EXIST | EXIST | M2 已交付（仅 `.log` 命名，36 行） |
| `src/common.h` | EXIST | EXIST | M1 已交付（`Options` 只有 4 个字段，见 §0.4） |
| `docs/m3-design.md` / `m3-evidence.md` / `m3-prerequisites.md` | EXIST | EXIST | M3 设计/证据/前置 |
| `docs/m4-design.md` / `m4-prerequisites.md` / `amplification.md` | **不存在** | **不存在** | 本阶段产出（`M4:131`） |

`ls docs/` 原始输出：`m1-design.md m1-evidence.md m1-prerequisites.md m1-review.md m1-tdd-red.log
m2-design.md m2-evidence.md m2-prerequisites.md m2-review.md m2-tdd-red-i32.log m2-tdd-red.log
m3-design.md m3-evidence.md m3-prerequisites.md m3-tdd-red.log protocol.md roadmap.md`
⇒ **`docs/m4-*.md` 与 `docs/amplification.md` 确认不存在**（本阶段要建）。

**判定**：M4 `#0` 第一步（`M4:61-62`）点名要探查的 `version_set` / `merging_iterator` / `db_iter`
**在 `e56d0b7` 上全部不存在** ⇒ 本设计对它们的接口只能以 `D3` §8.1（`D3:1644-1705`）与 §7.3（`D3:1558-1607`）
的**已冻结签名草案**为基线，并逐处标注"未落地"。这是 `#1` 的**头号前置条件**。

### 0.4 探测③：既有契约的实际形态（原始输出）

**(a) `Options`（`src/common.h:241-250`，逐字）**

```cpp
struct Options {
  const Comparator* comparator = BytewiseComparator();
  size_t write_buffer_size = 4 * 1024 * 1024;   // 4 MiB
  // M2 增补（登记于 docs/m2-prerequisites.md §9 第 7 条）：注入 Env。nullptr = Env::Default()。
  Env* env = nullptr;
  // M2.3：组提交观察点（nullptr = 无观察者）。见 CommitHook 的注释。
  CommitHook* commit_hook = nullptr;
};
```
⇒ 实测**只有 4 个字段**：`comparator` / `write_buffer_size` / `env` / `commit_hook`。
**没有** `block_size` / `verify_checksums` / `max_open_files` / `recycle_log_files` / `flush_hook`
（`D3:2117` 与 `PR3:509` 要求的 M3 字段**尚未落地**），**也没有**任何 `max_bytes_for_level_*`（`M4` 需要的字段）。

**(b) `TableOptions`（`src/sstable/table.h:11-12, 36-39`，实测）**

```
src/sstable/table.h:11  //   §4/§405 要求 Options 增加 `block_size` / `verify_checksums`，但 §11.1 与本任务的文件许可
src/sstable/table.h:12  //   都明确 **不得改 src/common.h**。因此这里用 `TableOptions` 承载这两个字段，取值与语义逐字
src/sstable/table.h:37  struct TableOptions {
src/sstable/table.h:38    size_t block_size = 4096;        // 数据块 payload 的**目标值**，不是硬上限（§3.2）
src/sstable/table.h:39    bool verify_checksums = true;    // 默认开；关掉只跳过 payload CRC，结构校验永不跳过（§5.4）
```
⇒ **存在两份"块选项"落点**（`TableOptions` vs 设计与 `PR3:509` 要求的 `Options`）。
`docs/m3-evidence.md:42-44` §4 第 1 条已把它登记为未闭合项，并写明"**M3.2 决定**：把 `Options` 别名/嵌套到
`TableOptions`，或反向统一" ⇒ **M4 必须依赖该收敛的结果**（见 §2.3 A11 的影响面与 §12.5 薄弱点 6）。

**(c) `Env` 能力（`src/util/env.h`，实测）**：已有 `NewWritableFile` / `NewAppendableFile` /
`NewSequentialFile` / `NewRandomAccessFile`(`:58`) / `RandomAccessFile`(`:41-48`) / `FileExists` /
`GetFileSize` / `DeleteFile` / `RenameFile` / `CreateDir` / `GetChildren` / `RemoveFile` / `Truncate` /
`LockFile` / `UnlockFile` / **`SyncDir`(`:75`)** / `NowMicros` / `SleepForMicros`。
⇒ `D3` §11 M3.1 要求的三项增补（`RandomAccessFile` / `NewRandomAccessFile` / `SyncDir`）**已落地**
（由 `e56d0b7` 交付，`src/util/env.h:16` 的 `+16` 行）。
⇒ **M4 通常无需再补 `Env`**（与 `NOTE` 的判断一致；`M4:133` 的"可选扩展"不触发）。

**(d) `filename.{h,cpp}`（`src/filename.h:13-18`，逐字）**

```cpp
constexpr const char* kLogFileSuffix = ".log";
std::string MakeFileName(const std::string& dbname, const std::string& suffix);
std::string LogFileName(const std::string& dbname, uint64_t number);
bool ParseLogFileName(const std::string& fname, uint64_t* number);   // 只认 "%06u.log"
std::string LockFileName(const std::string& dbname);
```
⇒ 实测**只有 `.log` 一族**；`TableFileName` / `TempFileName` / `ParseTableFileName` / `MetaFileName`
（`PR3:509` 要求 M3 交付）**尚未落地**。M4 的 `MANIFEST-*` / `CURRENT` 命名**必须**与 M3 那批一起落地
（见 §3.1 与 §2.3 A11 的影响面）。

**(e) `docs/protocol.md` 章节（实测 `grep '^#\{1,3\} '`）**：`§1 基本类型:9` / `§2 Varint:18` /
`§3 Fixed:29` / `§4 Length-Prefixed:36` / `§5 CRC32C:43` / `§6 内部 key:54`（`§6.1:71`、`§6.2:86`）/
`§7 MemTable 条目编码:95` / `§8 边界与拒绝口径:126` / `§9 WAL record:139`（`§9.1:144`~`§9.4:185`）/
`§10 SSTable 编码:206`（`§10.1:212` ~ `§10.9:364`，共 377 行）。
⇒ **M4 的追加章节号 = §11**（`M4:44` 要求"追加章节"，未给号；`§1~§10` 已占用，且 `P:3-4` 明文
"M1 定稿后冻结，任何变更须回到 `#0` 设计阶段修订本文件"）。§11 的完整 patch 文本见本文 §4。

**(f) 文档行数（实测 `wc -l`）**：`m3-design.md` 2264 / `m3-evidence.md` 57 / `m2-design.md` 1386 /
`roadmap.md` 75 / `protocol.md` 377。

**(g) 测试与工具（实测）**：`tests/` = `crash_test.cpp`(16 个 `TEST`) `faulty_env.{h,cpp}`
`memenv.{h,cpp}` `memtable_test.cpp`(27) `recovery_test.cpp`(10) `sstable_counting_env.h`
`sstable_format_test.cpp`(8) `sstable_table_test.cpp`(11) `test_harness.h` `util_test.cpp`(20)
`wal_test.cpp`(10) ⇒ **静态计数 102 个 `TEST(...)`**。
`scripts/` = `crash_recover.cpp` `crash_writer.cpp` `fsbench_commit_latency.cpp` `lsm_build.sh`
`lsm_corrupt_middle_test.sh` `lsm_crash_test.sh` `lsm_damage_test.cpp` `lsm_gate.sh`
`lsm_recovery_stats.sh` `lsm_tail_truncate_test.sh`。

**(h) `CMakeLists.txt` 目标（实测）**：`add_library(lsm STATIC ...):46`、`add_library(lsm_sstable STATIC ...):72`
（**依赖纪律机制**：越权依赖会在此链接失败）、`add_executable(lsm_tests ...):99`、`add_test(NAME lsm_unit):123`、
`lsm_crash_writer:126`、`lsm_crash_recover:129`、`lsm_damage_test:133`、`fsbench_commit_latency:137`、
`option(ENABLE_TSAN):30`、`option(ENABLE_ASAN):31`。

**(i) 门禁与对账脚本机制（实测）**：`scripts/lsm_gate.sh:60` 的 `run_gate_marked` 实现"**多条正向标记 AND**"
判定（`:107` 干净重建腿的标记是 `\[  PASSED  \] [1-9][0-9]* tests`；`:109` ASan；`:115` TSan；
`:118` 崩溃对账；`:121` 截断扫描；`:122` 中间损坏），`:94-97` 缺脚本记 `SKIP`，`:147-148` 汇总 `[PARTIAL]`。
`scripts/lsm_crash_test.sh:51-70` 从 `lsm_crash_recover` 的 `^ROUND ` 行 `sed` 出 `ACKED/MISSING/MISMATCH`
并累加 `MISSING_TOTAL`；`:56` `RC != 0` ⇒ `ROUND_FAIL`。
`scripts/crash_writer.cpp:1-40` 的 sidecar 协议（**先 `Put(sync=true)` 返回 `kOk`，之后才写 sidecar 并 fsync**）
是 M4 压测必须逐字沿用的形状；`scripts/crash_recover.cpp:15` 的固定行格式 =
`ROUND 0 ACKED <a> RECOVERED <r> MISSING <m> MISMATCH <mm> TRUNCATED_BYTES <t> OPEN_MS <ms>`。
**实测**：`crash_writer.cpp` 与 `crash_recover.cpp` 只 include `db.h` / `filename.h` / `util/env.h`
⇒ **拿不到 `PersistentDBImpl` 的内部统计**（M4 的放大/层级统计需要一个新工具，见 §2.3 A14）。

**(j) 测试 seam（实测）**：`tests/memenv.h:8-9` "每个文件维护 **fsync 水位** `synced_size`；
`SimulateCrash()` 先回滚到 `synced_size`"；`tests/memenv.h:49-52` "内存 FS **没有目录项概念**…
`SyncDir` 只**计数**，供用例做**顺序断言**"；`tests/memenv.cpp` 实测有 `SyncDir` 实现（`e56d0b7` 的 `+40` 行）。
`tests/test_harness.h` 提供 `Rng/RandomKey/RandomValue`（固定种子 `kHarnessSeed = 0x5EED2025u`，`:57`）、
`ExpectSameAsStdMap` / `ExpectSkiplistMatchesMultimap` / `VisibleKeysFromInternal`（`:10-11`）、
`ManualInternalKey` / `ManualLookupKey` / `ManualEntry`（`:12-14`，**手工拼字节参照**纪律）、`TempDir`（`:15`）。
**实测全仓 `grep -rn FakeClock` 在 `src/` 与 `tests/` 零命中**（与 `NOTE:412` M5-R9 一致）⇒ `FakeClock` **需要新建**。

### 0.5 探测④：不变量/锁纪律号空间的实际占用（`M4-C1` 的事实基）

| 号段 | 实际内容 | 出处（实测） |
|---|---|---|
| I1~I10 / L1~L6 | M1 | `M4:45`、`D3:1876` 的说明行 |
| I11~I20 / L7~L12 | M2（`I11` durable-before-ack … `I20` 关闭语义；`L7`~`L12`） | `docs/m2-design.md:97-112`、`:868-873` |
| **I21~I34 / L13~L21** | **M3（含追加的 I31~I34 / L19~L21）** | `D3:1876-1916`；`PR3:415-463`；`PR3:824` 的 **V8**（"新增 I31~I34 与 L19~L21…状态：已批准"） |
| **M4 指令要求新增** | **I31~I42 / L19~L26** | `M4:92-104`、`M4:106-114` |

⇒ **逐字撞号的 4 对**（`D3:1890-1893` vs `M4:93-96`）与 **3 对**（`D3:1908-1910` vs `M4:107-109`）已实测确认，
与 `NOTE:138-149` 的描述完全一致。裁决见 §2.2 **A1**。

### 0.6 未验证项（明确写出，不得当成已验证）

1. **用例数 102 是静态计数**（`grep -c '^TEST'`），**不是本轮运行结果**——派工纪律禁止在本仓库跑构建/测试。
   仓库内已记录的同基线运行结果是 `docs/m3-evidence.md:26` 的
   `102 tests from 22 test suites ran.` / `[  PASSED  ] 102 tests.`（**引用，非本轮实测**）。
2. **VM 上的构建目录状态、编译器版本、磁盘/CPU 状态本轮未探测**（`PR3` §2 已记录，未复核）。
3. **`FakeClock` 是否需要、以何形态存在：未确定**（`src/` 与 `tests/` 全仓**零命中**；`D3:1958` 的 `M3-A25` 行末写的是 **`FakeClock`（MemEnv）**，而 `M4:141`/`M5:128` 都写"追加 `tests/test_harness.h` 的辅助" ⇒ 两处措辞不一致；`docs/m3-prerequisites.md` **全文未提** `FakeClock`（实测 `grep` 无命中）。裁决见 §11 的 M4.2（归属 `tests/test_harness.h` 的 `FakeClock` + `ClockEnv`）。
4. **`MANIFEST` 的最终命名与 `CURRENT` 的内容格式：指令未规定**（由本设计冻结，见 §3.1）。
5. **`M3.2`/`M3.3` 的实际落地形态未知**（`version_set.*` 等不存在）⇒ 本设计引用的 M3 侧签名全部来自
   `D3` 的**设计草案**，不是实测代码。
6. **`META` 取代方案对 M3.3 尚未编写的测试的影响只能"前置约束"，不能"实测"**（见 §2.2 A9）。
7. **块缓存不引入**（本设计的裁决）⇒ `M4:163` 的"可选块缓存命中率对比"**不适用**，无实测数字。

### 0.7 并发事实（**必须登记：M4 的开工前置此刻正被另一个代理实现**）

- 本轮的派工事实：`src/version_set.{h,cpp}`（内存版 `Version` + `TableCache`）、`src/version_edit.{h,cpp}`、
  `src/merging_iterator.{h,cpp}`、`src/db_iter.{h,cpp}` 是 **M3.2 的交付物，正由另一个代理实现**；
  **M3.3** 再把 `META` 的 `Persist`/`Recover` 加上去。M4 的实现者**不**碰这些文件（§2.2 A10 的白名单除外）。
- **后果 1（接口基线）**：本设计的 §5 是**凭空起草**的草案。`#0` 冻结之后、M4 写下第一行代码之前，
  必须执行 **§11 的 M4.0**（15 项"开工前置复核清单"，每项带可粘贴命令）：逐一校对落地后的**真实签名**，
  **不一致处以落地实现为准并登记差异**；若差异触及§6/§9 的机制（例如 `Version` 的不可变性、
  `TableCache` 的键、`VersionEdit` 的编解码形态），按 `M4:40` 的流程锁**回退 `#0`** 修订本文档。
- **后果 2（本设计的细节程度不因此降低）**：位级布局（§3.2/§3.3）、失败码与失败矩阵（§6.6/§8.2）、
  不变量与锁纪律（§9）、补充约束（§9.5）**全部写死**，不依赖"M3.2 大概会这么做"。
  凡引用 M3 侧的地方一律标 **【M3.2 承诺】** 并给出 `D3` 的行号出处（§5 各节）。
- **后果 3（B 组）**：M4 的 B 组**复用** M3 的崩溃脚本与工具 ⇒ 依赖 `M3-B01~B05` 的交付状态，
  见 §0.6 第 1 条与 §10.2 末段的六条可粘贴前置检查命令。

**因此 `#0` 的产出是一个"可立即执行的施工图"，但它带一个显式的**前置闸门**（M4.0）。**

---

## 1. 目标 / 非目标 / 与 M1~M3 的关系

### 1.1 目标（逐条对应 `M4:9-16` 的 7 条）

| # | 出处 | 目标 | 本设计的落地章节 |
|---|---|---|---|
| G1 | `M4:10` | 层级结构：L0 允许 key 范围重叠、**文件数阈值触发（默认 4）**；**L1 起始容量 10 MB、每层 ×10**；**L1+ 层内 key 范围不重叠** | §3.4、§3.5、§5.4 |
| G2 | `M4:11` | `Version` / `VersionEdit` / `VersionSet` 完整实现 + MANIFEST 记录与恢复 + CURRENT 原子切换 | §3.1~§3.4、§5.2/§5.3、§8.1~§8.4 |
| G3 | `M4:12` | Compaction 执行 L(n)→L(n+1)、选层与选文件、输入输出文件管理、**输出文件上限（默认 2 MB）**、key 范围裁剪 | §3.5、§5.4、§6.3/§6.4 |
| G4 | `M4:13` | 快照一致性：最小快照 sequence 决定 tombstone/旧版本何时可丢；compaction 期间前台读持 Version 引用（读写都不阻塞） | §5.5、§6.5、§7.2/§7.3、**§2.3 A13** |
| G5 | `M4:14` | 三个放大口径的可复现统计输出 | §5.7、§10.3 |
| G6 | `M4:15` | 崩溃一致性：compaction 中途 `kill -9` 后重启，版本一致、`missing 0` | §8.1~§8.5、§10.2 |
| G7 | `M4:16` | **可选**：块缓存（LRU）与 table cache | **§2.1 D8**（table cache 复用 M3；块缓存不引入） |

验收口径 7 条见 `M4:18-25`，逐条映射到 §10（测试矩阵）与 §10.3（统计口径）；其中 `M4:20` 的"量级分离"
必须落成可判定数字（§2.2 **A12**），`M4:24` 必须按 `M4-C8` 改写判据（§2.2 **A8**）。

### 1.2 非目标（硬边界，评审逐条对照）

逐字来自 `M4:27-28`：多线程并行 compaction、Bloom Filter（M5）、WriteBatch 对外接口（M5）、压缩算法、
与 raft-kv 对接（M6）；"**M4 只允许单个后台 compaction 线程；跨层并发 compaction、subcompaction 全部禁止**"。
与 `RM:42`（"M4：不得出现 Bloom/Batch"）、`RM:44`（"每阶段的『非目标』段是硬边界"）一致。

**本设计对"非目标"的精确化（避免用词歧义导致误判越界）**：

| 非目标 | 本设计的精确含义（唯一解释） |
|---|---|
| 多线程并行 compaction | **至多一个 compaction 线程、任一时刻至多一个进行中的 compaction**。M3 的**单后台 flush 线程**不属于"并行 compaction"，M4 继续使用它（§2.1 D5）。 |
| 跨层并发 compaction / subcompaction | 一次 compaction 只处理**一对相邻层** (n, n+1)；**禁止**把一次逻辑 compaction 拆成多个并行子任务。 |
| Bloom Filter / WriteBatch 对外接口 / 压缩算法 / mmap | 一个符号都不出现。 |
| 与 raft-kv 对接 | 不出现任何 raft 相关符号。 |

### 1.3 与 M1 / M2 / M3 的关系

#### 1.3.1 逐字复用（不改一行）的既有契约

| 契约 | 出处 | M4 如何用 |
|---|---|---|
| internal key 编码（`user_key ‖ trailer(8B LE)`）与比较规则（user key 升序 + trailer **降序**） | `P` §6（`P:54-94`）；`D3:1558-1584`；`M4:35` 的冻结清单 | compaction 的输入迭代器与输出顺序**逐字沿用**；`FileMetaData::smallest/largest` 是 internal key |
| `MergingIterator` 只归并、不做可见性判断；输出 internal key 全序 | `D3:1563-1581`（`M4:155` 要求保持） | compaction 的归并复用 M3 的 `MergingIterator`（child 数从 F 变成 输入文件数 + 0，构造相同） |
| `DBIter` 的可见性算法（`seq <= snapshot`、每 user key 只出最新可见版本、跳 tombstone） | `D3:1592-1607` | 不变；M4 只把 `snapshot` 的来源从 `last_sequence_` 扩展为可选快照 |
| `Table` / `TableBuilder` / `BlockBuilder` 的位级格式与"块内 restart 语义" | `D3:558-765`；`P` §10 | compaction 输出**必须**用 `TableBuilder`（禁止自写格式） |
| `Options::write_buffer_size` 的 M3 语义（目标值，不是硬上限） | `D3:1262-1269` | flush 产出的 L0 文件大小由它决定；M4 不改 |
| `TableCache`（文件号 → `shared_ptr<const Table>`，LRU，容量 `Options::max_open_files`） | `D3:1687`、`D3:1633`、I30（`D3:1889`） | compaction 的输入**必须**经 `TableCache` 取（§2.2 A4、§6.3） |
| `last_sequence_` 的唯一口径（`= max(WAL 重放最大值, 各已注册文件 max_sequence)`，**禁止**用注册时刻的 `last_sequence_`） | `D3:1707-1733`（I31 原文 `D3:1890`）、`P` §10.9 规则 5 | M4 的 `VersionEdit` **不引入** `kLastSequence`（§2.4 E4） |
| `RecoveryStats` / `FlushStats` 的"截断/跳过/丢弃必须计数 + 上报"纪律 | `D3:1783-1812`（`D3:1811` 的纪律行） | M4 的每条新丢弃路径必须加计数（§5.7、§8.6） |
| WAL 恢复的尾部语义（只有最高编号 log 允许截断；中间损坏 ⇒ `kCorruption`） | `P` §10.9 规则 4；`D3:1762-1771` | M4 的 MANIFEST 回放**逐字沿用同形状**（§8.4） |
| 依赖方向（`common.h → util → memtable → db`；`util` 不得 include `memtable.h`/`db.h`） | `RM:8-26` | `compaction.*` **只能**经 `DbImpl` 访问 MemTable/WAL/SSTable（`M4:77`） |

**前置事实（§0.3/§0.7，影响本节每一条"扩展"的落地时机）**：`src/version_set.*`、`src/version_edit.*`、
`src/merging_iterator.*`、`src/db_iter.*` **在 `e56d0b7` 上不存在**，它们是 **M3.2 的交付物（正由另一个代理实现）**。
⇒ 本节两条（`Version` / `VersionEdit` / `VersionSet`）标为 **【M3.2 承诺】的扩展**：M4 只在其**落地之后**
做扩展；落地后的**真实签名以实现为准**，差异用 **§11 的 M4.0** 复核清单逐项登记。

#### 1.3.2 被扩展的接口（"只增不改"逐条列出）

| 既有物 | M4 的扩展 | 为什么不是"改语义" |
|---|---|---|
| `Version`（`D3:1679-1685`） | 加 `level_files(int)` / `level_sizes` / `Ref()/Unref()` / `ValidateLevelLayout()`；保留 `files()` 作为 `level_files(0)` 的**别名** | `D3:511-512` 明写"M4 把它扩成 `level_files(int)` 时**不需要改** `files()` 的调用方"。M3 只有 L0 ⇒ `files()` 的语义自动等于 `level_files(0)` |
| `VersionEdit`（`D3:1659-1671`） | `AddFile(int level, const FileMetaData&)`；`DeleteFile(int level, uint64_t)`；`added_files()/deleted_files()`；`Clear()` | `D3:1658` 原文即"全量快照的内存表示 + 编解码，**没有**追加/差分语义"⇒ M4 给它**加上**差分语义（正是 `D3:506-508` 承诺的迁移点） |
| `VersionSet`（`D3:1691-1700`） | `Recover`/`Persist` 的函数体替换；加 `LogAndApply(VersionEdit*)`、`manifest_*`、`live_versions()` | `D3:1704-1705` 逐字："`Recover` 的签名里没有『META』字样…M4 改成 MANIFEST + VersionEdit 追加日志时，只改 `Recover`/`Persist` 的函数体" |
| `Options`（`src/common.h:241-250`） | 追加 M4 的 6 个字段（§5.6），并**收敛** M3 的 `block_size`/`verify_checksums`/`max_open_files`/`recycle_log_files`/`flush_hook` | `M4-C11` 的裁决（§2.2 A11）；`PR3:509` 已把 `src/common.h` 列入 M3 的【必须改】⇒ 追加是被预期的 |
| `filename.{h,cpp}`（`src/filename.h:13-18`） | 追加 `ManifestFileName` / `ManifestTempFileName` / `CurrentFileName` / `CurrentTempFileName` / `ParseManifestFileName` / `TableFileName` / `TempTableFileName` / `ParseTableFileName` / `ParseTempTableFileName` | `M4:132` 明列；且 `PR3:509` 已要求 M3 交付后 5 个 ⇒ M4 只追加 MANIFEST/CURRENT 那一族 |
| `DBImpl`（`src/db_impl.{h,cpp}`） | 追加 compaction 线程、`MaybeScheduleCompaction`、`MakeRoomForWrite` 的层级分支、Version 安装、延迟删除、统计入口 | `M4:34` + `M4:132`；白名单见 §2.2 A10 |
| `src/db.{h,cpp}` | **不动**（见 §2.3 A13/A14 的裁决） | `M4:132` 对 `db.*` 的改动是**条件性**的（"如需…则最小扩展"）；M3 已把统计入口放在 `PersistentDBImpl`（`D3:1799`）⇒ 条件不成立 |

#### 1.3.3 M4 **显式解除**的 M3 禁令（必须登记，否则实现者会以为越界）

| M3 的禁令 | 出处（实测原文） | M4 的处置 |
|---|---|---|
| "本设计**不使用**『层/L0/Ln』作为**实现**概念（M4 才引入）" | `D3:2102` §12.3 | **解除**：M4 起 `level` 是核心实现概念 |
| "代码里**不得**出现 `level`/`LevelFiles`/`L0_` 等符号（`#1` 登记为评审检查项）" | `D3:2112` §12.4 | **解除**：该禁令的作用域是"M3 的实现范围"；M4 必须引入（`M4:95` 把层内不重叠定义为"违反即拒绝安装"） |
| "M3 **不删**已注册的 `.sst`" | `D3:1911` L16 行末 | **解除并由 I42/I43（`M4:100-101`）接管**：M4 首次真删已注册 `.sst`，判据是"引用归零 + 延迟队列" |
| "M3 不删任何旧版本，tombstone 必须能屏蔽更旧版本" | `D3:1885`（I26） | **由 I40（`M4:98`）以"条件删"的形式取代**：M3 是"永不丢所以不会误丢"，M4 是"只在最底层无重叠时丢"（§9.2 的关系列） |
| "本设计**不引入** `CURRENT`；等价纪律落到 `META` 的 `写 .tmp → fsync → rename → SyncDir`" | `D3:1912`（L17） | **取代**：M4 引入 `CURRENT`，`META` 被 MANIFEST 取代（§2.2 A9、§3.6）；L17 的**实质**（原子切换、禁止原地覆写、rename 后必须 `SyncDir`）在 M4 由 L25/L28 承接 |

### 1.4 依赖方向（单向，禁止反向）

```
common.h  →  util  →  memtable  →  db            （RM:8-26，M1~M3 不变）
                                    ↑
                       sstable/format,block,table_builder,table   （M3.1，只链 util）
                                    ↑
                          version_edit  →  version_set  →  compaction  →  db_impl
```
- `compaction.*` **禁止** include `memtable.h` / `wal.h` / `db.h`；它只依赖 `version_set.h` /
  `sstable/table*.h` / `util/env.h`。对 MemTable/WAL 的访问**只能**经 `DbImpl` 提供的回调或参数（`M4:77`）。
- `version_edit.*` **禁止**依赖 `sstable/*`（它只需要 `Slice` + `util/coding.h` + `util/crc32c.h`）——
  这条用 CMake 独立目标机制化：新增 `add_library(lsm_version STATIC src/version_edit.cpp src/version_set.cpp ...)`，
  并断言 `nm -C build/liblsm_version.a | grep -cE 'db_impl|wal|memtable'` ⇒ **0**（形状照抄 `D3:2029` 的
  `lsm_sstable` 机制，`D3:72-82`）。
- `compaction.*` 不得直接调用 POSIX 系统调用，全部经 `Env`（`M4:177`）。

### 1.5 不变量与锁纪律的映射预告

M4 的**落地号** = `I35~I46` / `L22~L29`（`M4-C1` 裁决，§2.2 A1）；`M5` 顺延 `I47~I56` / `L30~L35`。
完整映射表见 **§9.1**；与 M3 既有 `I21~I34` / `L13~L21` 的"新增/收紧/取代"关系见 **§9.2 / §9.3**。

---

## 2. 开放决策记录（方案 → 取舍 → 推荐 → 影响面）

> 组织：§2.1 = `M4:66-74` 的 8 条指令决策（D1~D8）；§2.2 = `NOTE:134-212` 的 11 条矛盾/歧义裁决
> （A1~A11 = M4-C1~C11）+ 针对 `M4:20` 的补充裁决（A12）；§2.3 = **本轮新发现**的 3 条歧义（A13~A15）；
> §2.4 = 指令外必须拍板的补充决策（E1~E10）。
> **用户已授权：一律按"推荐"执行**（§13 逐条复述）。

### 2.1 指令的 8 条开放决策

#### D1 分层策略（`M4:67`）

| 方案 | 形态 | 取舍 |
|---|---|---|
| A：全 leveled（L0 也不重叠） | 不可能：flush 每次产出一个 L0 文件，其 key range 与已有 L0 文件天然重叠；要在 L0 也保持不重叠就得在 flush 里做归并 = 把 compaction 塞进 flush 路径，违反 `M4:34` 的禁入声明 |
| B：size-tiered（L1+ 重叠） | 写放大低、读放大高。**但与 `M4:10`（硬要求）和 `M4:95`（违反即**拒绝安装该 Version**）直接冲突** ⇒ 若允许 L1+ 重叠，该不变量自身失效，且 `M4:122` 的风险（"compaction 与 flush 并发破坏层级约束 ⇒ 读放大暴涨甚至读错"）变成常态。**否决** |
| **C：L0 tiered + L1+ leveled（指令推荐）** | L0 保持"文件号降序、逐个检查"（沿用 M3 的 I23/I38），L1+ 用"层内 key 范围严格互斥 + 二分定位"把一次点查的**文件数上界**从 O(全库) 降到 O(层数) |

**推荐 C。** 三种方案的**放大预期**（指令 `M4:67` 要求写出）：

| 方案 | 写放大预期 | 读放大预期 | 空间放大预期 |
|---|---|---|---|
| 全 leveled（不可实现，仅作参照） | 高（每层都可能重写） | 低（每层 ≤1 文件） | 低 |
| size-tiered（**被否决，负结果入档**） | 低（一次合并只碰同层若干个等大文件） | 高：一次点查要检查**每层全部文件**（层数 × 每层文件数），且层内重叠使"能否跳过"不可判定 ⇒ 期望 ≈ M3 的退化形态（`D3:2136`：F≈59 时一次最老 key ≈1.33 MB） | 高（重叠数据的多份副本长期共存；tombstone 无法在"非最底层无重叠"条件下丢弃 ⇒ `M4-C5` 的推理） |
| **C（推荐）** | 中：一次点查的 IO 由"L0 文件数 + 1 + 输出量"决定 | **低**：`files_checked ≤ level0_trigger + 2 + (kNumLevels − 1) = 4 + 2 + 6 = 12`（上界由 §3.4 的不变式与 `D3:1400` 的在途 flush 上界共同保证） | 中：L0 与 L1 的短暂重叠 + 最底层承担全部 |

**影响面**：§3.4（层级不变式）、§5.4（选层/选文件）、§6.3（输入闭包）、§9.2 的 I37/I38、§10.1 的
`M4-A16~A19`。**负结果入档**：size-tiered 的预期数字进 `docs/amplification.md`（`M4:23`、`RM:61`），
标注"**结论作废：该方案与 `M4:10`/`M4:95` 的硬约束不兼容，未实现**"（**不是**"实测更差"——
本设计**不假装**有一个未做的方案的实测数据）。

#### D2 触发条件与选层/选文件（`M4:68`）

**先消歧（`M4-C6` 的裁决，见 A6）**：`M4:10` 同时给出两条触发（L0 文件数默认 4；L1 起 10 MB 每层 ×10）
⇒ 两条**都生效**，`M4:68` 真正要定的是"**多层同时超限时选哪一层**"与"**层内选哪个文件**"。

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 触发判据 | (i) 只用 L0 文件数；(ii) 只用每层容量 | 二选一 ⇒ (i) 会让 L1+ 永远不被合并（容量无判据 ⇒ **违反 `M4:19` 要求同时给出 L0 文件数与各层字节数**）；(ii) 会让 L0 永不触发 | **两者都生效**：`score(0) = num_files(0) / level0_file_num_compaction_trigger`（**文件数**判据）；`score(l≥1) = total_bytes(l) / MaxBytesForLevel(l)`（**字节数**判据） |
| 选层打分 | (i) 取 `score` 最大且 `≥ 1` 的层；(ii) 固定 L0 优先（只要 L0 超限就压 L0） | (ii) 会让"L1 严重超限"被 L0 的常态触发永远压住 ⇒ L1 单调膨胀 | **(i)**；**平手（分数相等）时取层号最小者**（⇒ L0 优先，因为它通常是新数据、压它最划算）。打分函数与推导见 §3.5 |
| L0 层内选文件 | (a) **轮转**：取文件号**最小**（最旧）的未合并文件为种子；(b) **与下层重叠最小**：按与 L(n+1) 重叠字节数取最小；(c) seek 热度计数 | (c) 需要额外的读路径埋点与持久化，`M4:68` 明写"留作可选扩展"；且未实现的扩展**不能**拿去对照（`M4-C7`）。(a) 公平、实现最小、无饥饿；(b) IO 量与输出量更小，但**可能让冷文件被反复跳过**（`M4:127` 的风险"某些文件永远不被 compaction"） | **(a) 为默认**；**(b) 为对照策略**（运行时可选，见 §2.4 E9 的 `Options::compaction_pick_strategy`）。`M4:68` 的"是否会导致冷文件永不合并"由 **§10.2 `M4-B04`**（长压测下层级文件数与字节数随时间变化）承担探针职责 |
| L(n≥1) 层内选文件 | (a) **轮转**：从"上次 compaction 的结束位置（compact pointer）"之后取**第一个**文件；(b) 与下层重叠最小 | (a) 需要维护 compact pointer；M4 **不持久化**它（§2.4 E5），重启后从各层最左重新开始 ⇒ 只会多做一些 compaction，不影响正确性 | **(a) 为默认**（内存 compact pointer）；**(b) 为对照策略** |

**影响面**：§3.5（容量表与 score）、§5.4（`PickCompaction` 伪代码）、§10.1 的 `M4-A11~A15`、
§10.2 的 `M4-B03`（对照表）。

#### D3 合并输入输出、key 范围裁剪与丢弃判据（`M4:69`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 输入集 | (i) 一个上层文件 + 下层重叠文件集；(ii) 上层文件集 + 下层重叠集 | (ii) 是**必须**的：L0 允许重叠，任取一个文件会在"更旧的 L0 文件仍持有同 user key"时读到陈旧值（见 §6.3 的反例） | **(ii)**，且上层集合必须是**传递重叠闭包**（§2.4 **E1**） |
| 输出上限 / 滚动 | (i) 固定 2 MB；(ii) 不限制（一个大文件） | (ii) 会让 L1+ 的"重叠检测"退化为"整层一个文件"，且后续 compaction 的输入永远巨大 | **(i)**，`Options::max_file_size = 2 MiB`（`M4:12`）；**只约束 compaction 输出**，flush 输出仍一表一文件（§2.4 **E6**） |
| key 范围裁剪 | (i) 只处理输入集 key range 内的 key（迭代器天然做到）；(ii) 额外按"祖父层重叠"提前封块（LevelDB 的 `ShouldStopBefore`） | (ii) 能压低**未来**的 compaction 规模，但引入第三个层参与判据，且 `M4` 未要求 | **(i)**；**(ii) 显式不做并登记**（§2.4 **E7**） |
| 丢弃判据 | (i) `M4:37` 的**合取**写法；(ii) `M4:98-99`（I36/I37）的**分列**写法 | 合取 ⇒ (a) 非最底层的旧版本永远丢不掉 ⇒ 空间放大爆炸；(b) 把"快照保护"错当成 tombstone 的额外条件。见 **A2** | **(ii)**；**单独成函数**（`M4:178` 要求）+ 双向构造性用例（`M4-A22~A27`） |

**影响面**：§5.5（判据函数与真值表）、§6.3/§6.4、§9.2 的 I40/I41、§10.1 的 `M4-A16~A27`。

#### D4 快照与多版本（`M4:70`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 最小快照 sequence 的维护 | (i) 只保留一个"最小快照 sequence"标量，随快照集合增删重算；(ii) 不维护，永远用 `last_sequence_` | (ii) ⇒ 任何快照读都会在 compaction 后**读到错值**（`M4:118` 的风险），且 I41 无从成立 | **(i)**：快照集合按 `sequence` 有序（`std::multiset<SequenceNumber>`），`smallest_snapshot_ = 空 ? last_sequence_ : *begin()` |
| `last_sequence_` 的读取时机 | (i) compaction 开始时锁内取一次；(ii) 每条 entry 现取 | (ii) 会让同一次 compaction 的不同 entry 用不同快照边界 ⇒ 判据不可复现（违反 I45） | **(i)** |
| 快照句柄的生命周期与释放 | 见 **A13**（公共 `DB` 接口 vs 具体类接口） | | **A13 的推荐**：不改 `src/db.h`，落在 `PersistentDBImpl` |
| 无快照时的默认行为 | (i) `smallest_snapshot = last_sequence_`；(ii) `= 0`（最保守，一律不丢） | (ii) ⇒ 旧版本永不丢弃、空间放大爆炸；且与 `M4:70` 原文（"最小快照 = 当前 `last_sequence`，**旧版本可立即进入可丢弃判定**但 tombstones 仍受底层约束"）不符 | **(i)**，逐字落 `M4:70` |
| DBIter 的快照语义 | 沿用 `D3:1592-1607`，`snapshot` 由构造参数传入 | — | 不变 |

**影响面**：§5.5、§7.2/§7.3、§9.2 的 I41、§10.1 的 `M4-A24/A25/A28/A29/A30`。

#### D5 并发模型与锁纪律（`M4:71`）

| 方案 | 形态 | 取舍 |
|---|---|---|
| A：**flush 与 compaction 复用同一个后台线程**（`NOTE:219` M4-R1 的建议） | 一个线程、一个队列、flush 优先 | 简单、安装者唯一、无需 `install_mu_`。**但**：一次 compaction（读 4 个 4 MiB L0 文件 + 下层重叠 + 写输出 ≈ 数十~数百 ms）期间**没有任何线程**能消化 `immutables_` ⇒ `kMaxImmutableMemTables = 2`（`D3:1400`）被填满后**写者停等整个 compaction 时长** ⇒ `M4:20` 的"前台 P99 与 compaction 单轮耗时有**量级分离**"**结构上不成立** |
| **B（推荐）：M3 的单后台 flush 线程 + 新增单后台 compaction 线程（共 2 个线程），安装阶段用 `install_mu_` 串行化** | 两个执行体，但任一时刻**只有一个安装者** | ① 完全满足 `M4:28`（"只允许单个后台 **compaction** 线程"——它限制的是 compaction，不是已有的 flush 线程）；② 满足 `M4:112` 的**第二分支**（"或按设计文档的固定优先级串行化"）；③ 前台 P99 只受"短临界区 + 一次 flush 的 IO"影响，与 compaction 时长**解耦** ⇒ `M4:20` 可达成；④ 代价：多一个线程 + 一条"安装串行化"规则 + 安装期的 rebase 校验（§6.5） |
| C：compaction 分片（有界字节后回到队列） | 每次处理 ≤K 字节就释放线程让 flush 跑 | 需要"部分完成的 compaction"状态 ⇒ 实质上就是 subcompaction（`M4:28` **明文禁止**）。**否决** |

**推荐 B。** 配套的三条规则（写死，评审逐条对照）：

1. **固定优先级：flush 先**。compaction 线程在**进入安装阶段之前**必须在 `mutex_` 内观察到
   `immutables_.empty()`；否则先让路（等价于"先完成 flush 安装再开始 compaction"，`M4:112`）。
2. **安装串行化**：`install_mu_` 覆盖"写 MANIFEST record + fsync + 安装 Version"整段。
   持 `install_mu_` 期间**不持** `mutex_`（≠ DB 锁 ⇒ 不违反 `M4:111`/`M4:39`）。
3. **rebase 校验**：安装前必须校验"启动时捕获的输入文件集仍然全部存在于 `current_`"，
   并把 `VersionEdit` 应用到**当时最新的** `current_`（不是启动时的快照）；校验失败 ⇒ 放弃本次
   compaction（输出转孤儿、计数 `compaction_aborted`）并重新调度（§6.5 ⑤）。

**饥饿探针**（`NOTE:219` M4-R1 的建议）：A 组 `M4-A37` 用注入的"慢 compaction"（大输入 + `SlowingEnv`）
断言"compaction 进行中，flush 的入队→注册等待上界 ≤ 1 次 flush 的 IO 时间"。

**影响面**：§6.1（锁与状态表）、§6.2（两级后台工作）、§6.5（安装时序）、§6.7（关闭顺序）、
§9.3 的 L22/L25/L27/L29、§9.4（**全局锁序唯一表**）。

#### D6 MANIFEST 持久化与崩溃恢复（`M4:72`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 元数据形态 | (i) `MANIFEST-<n>` 追加日志 + `CURRENT` 原子切换；(ii) 启动扫目录重建 | (ii) **结构上无法区分"已注册文件"与"孤儿/残片"** ⇒ 违反 I39（`M4:97`）与 `M4:126` 的风险。`D3:501` 已对同形方案给出**决定性否决** | **(i)** |
| MANIFEST 的物理 record 格式 | (a) 复用 M2 的 WAL 物理 record（32 KiB 块 + padding 状态机）；(b) 复用 **SSTable 块外壳**同形（`P` §10.3：`length(4B LE) ‖ type(1B) ‖ payload ‖ crc(4B LE)`，CRC **含长度**）；(c) 自定义 | (a) 把 WAL 的"跨块切分 + 逻辑 record 64 MiB 上限 + 组提交语义"带进来，耦合面大；(c) 重新发明一遍"含长度的 CRC"纪律（`P:478-489` §9.3 已**否决**"间接推断"）。**(b)** 的形状已在 `P:248-280`（§10.3）冻结、已在 `M3-A08/A13/A17` 被测过、且**不需要任何跨块状态机**（`length` 是 4 B LE，单条 record 最大 4 GiB，远超全量快照的实际规模） | **(b)**，见 §3.2 与 §2.4 **E3** |
| MANIFEST 损坏/截断 | (i) 尾部残骸截断 + 计数；(ii) 拒绝启动；(iii) 自动修复 | (iii) 与 `P` §10.9 规则 1（"任一失败 ⇒ `kCorruption`，**不自动修复**"）冲突 | **(i)**：**只有最后一条 record** 允许截断到 `last_good_end`（形状逐字照 `P:371-377` §10.9 规则 4 与 `D3:1766`）；中间损坏 ⇒ `kCorruption` |
| 孤儿清理 | 见 §8.5 | — | 先 `durable → rename → SyncDir → 注册`（`M4:36`、`M4:97`），恢复期按 §8.5 的分类清理并**逐类计数** |
| 完整时序图 | `M4:72` 要求"**先落盘后注册**"的完整时序图 | — | §8.1（**两种模式**，见 A3） |

**影响面**：§3.1~§3.4、§4（protocol patch）、§8.1~§8.5、§9.2 的 I35/I36/I39/I43、§10.1 的 `M4-A04~A10/A39~A42`。

#### D7 放大度量口径与埋点（`M4:73`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 写放大是否含 compaction 自身的写 | (i) 含（`(flush + compaction 写)/用户逻辑字节`）；(ii) 不含（`flush 写/用户逻辑字节`） | 两者都被业界用过 ⇒ 争论本身无意义，**必须可复算** | **主列取 (i)**（LSM 的标准定义），**同时另给 (ii) 一列**，且分子**分项打印**（`flush_write_bytes` + `compact_write_bytes`）⇒ 从同一行可复算两种口径（`M4:73`、`M4:194`、`NOTE:222` M4-R4） |
| 统计窗口 | (i) 只做整段压测；(ii) 只做单次 compaction | 单轮口径回答"这次 compaction 划不划算"；整段口径回答"稳态放大是多少"；**两者都要**（`M4:83` 要求写死） | **两窗口同格式**：`round_id=C<k>`（单轮）与 `round_id=ALL`（整段），且整段行 = 单轮行各项之和（可复算 ⇒ I45） |
| 分母 | 用户逻辑字节（key+value）vs 写入的 entry 字节 | 含内部 key 开销会把"内部 key 编码的效率"混进放大 | **`user_logical_bytes` = Σ(key.size + value.size)**（不含内部 key、不含 WAL 头）；**另给 `entry_bytes` 一列** |
| 输出格式 | 见 §10.3 | `M4:22` 要求"固定列名与统计窗口，供脚本解析" | §10.3 的 `AMPL` / `LEVEL` / `FRONT` 三行；**只允许追加列**（M5 要加 filter 列，`M5:182`），前缀列一旦冻结不得改（I45 的可复现性） |
| 是否需要 status/日志命令 | — | — | **不加新公共 API**：入口落在 `PersistentDBImpl`（§2.3 A14），由 `scripts/lsm_level_stats.cpp` 打印 |

**影响面**：§5.7、§10.3、§9.2 的 I45、§10.1 的 `M4-A31`、§10.2 的 `M4-B03/B06/B07`。

#### D8 块缓存与 table cache（`M4:74`）

**table cache 部分（`M4-C4` 的裁决，见 A4）**：**复用** M3 已交付的 `TableCache`（`D3:1687`，
类定义在 `version_set.h`，容量 `Options::max_open_files`，LRU，`D3:1863` 的 E6 已把落点定在 `version_set.h`）。
**不新建** `src/table_cache.{h,cpp}`。理由：M3 的 I30（`D3:1889`）已把"文件句柄有上限"变成不变量；
再建一个 table cache 会破坏单一真相源并制造两套句柄预算。

**块缓存部分**：

| 方案 | 取舍 | 推荐 |
|---|---|---|
| A：M4 引入简单块级 LRU（键 = `(file_number, block_offset)`，容量按字节） | 能进一步压低"同一文件被多次点查"的 `data_blocks_read`。**代价**：① 块缓存把 LRU 的键从"文件号"变成"(文件号, 块偏移)"，是**一块独立的设计面**（淘汰粒度、块大小差异、与 `Table` 生命周期的耦合、`verify_checksums` 交互）；② `D3:552` 已把"块缓存"显式留给 M4/M5，而 M4 的 scope 已经包含整个分层 compaction；③ `M4:172` 的 M4.2 判据要的是"读放大有**改善证据**"，而该改善的**结构性**来源是分层本身（`files_checked` 从 59 降到 ≤12），不是块缓存 | **不引入** |
| B（推荐）：**M4 不引入块缓存** | 读放大的改善由分层提供（A 组可确定性断言上界，见 §10.1 `M4-A19` 与 §10.2 `M4-B06`） | **B** |

**登记（"未引入"必须写明理由 + 去向）**：`M4:163`（"可选块缓存/table cache 的命中率与读放大改善对比（若 M4 引入）"）
⇒ **前提不成立，故不适用**，在 `docs/amplification.md` 里显式写一行"`M4-B10`：未引入块缓存，不适用"
（**不得静默省略**，否则门禁清单看起来"少了一条"）。块缓存列为 **M5 的候选**并写入 `#1` 的风险/接续清单。

**影响面**：§7.4（读路径缓存口径，`D3:1633-1638` 不变）、§10.2 的 `M4-B06`、§12.4（范围越界核对）。

### 2.2 矛盾/歧义裁决（A1~A11 = `M4-C1`~`M4-C11`；A12 = `M4:20` 的补充裁决）

> **全部逐条采纳 `NOTE:437-449` 的推荐裁决**（用户已授权）。每条给"候选 → 取舍 → 推荐 → 影响面"。

#### A1（= `M4-C1` [编号]）新增 I/L 号与 M3 冻结契约双重占用

| 候选 | 取舍 |
|---|---|
| (i) 按 `M4:92-104`/`M4:106-114` 的字面号（I31~I42 / L19~L26） | **与已批准的 M3 冻结号逐字冲突**：`D3:1890` 的 I31"恢复水位的唯一口径" vs `M4:93` 的"I31 版本号严格单调递增"；`D3:1891` I32 vs `M4:94`；`D3:1892` I33 vs `M4:95`；`D3:1893` I34 vs `M4:96`；`D3:1908` L13 附近 vs `M4:107` L19 起 3 条。`PR3:824` 的 **V8** 已把 M3 的 I31~I34/L19~L21 正式收录并标注"已批准" ⇒ 改 M3 的代价更大，且 `M4:178` 要求"每条不变量在代码注释中标注编号，`#4` 逐条对照" ⇒ 双重占用会让"代码里的 I32 指哪条"无法判定 |
| **(ii) 按 `NOTE:147` 的裁决改号** | M4 新增不变量 = **I35~I46**；锁纪律 = **L22~L29**；M5 顺延 **I47~I56 / L30~L35**。`docs/m4-prerequisites.md` / `docs/m5-prerequisites.md` 各附"**指令原号 ↔ 落地号 ↔ 出处**"映射表 |

**推荐 (ii)**，并**兑现 `D3:1873` 的"编号可重排但不得丢条目"**：§9.1 给出完整映射表，
§9.5 说明本设计的**补充约束**（X1~X8）**不占用 I/L 号空间**（⇒ M5 的 I47~I56 保持可用）。

**影响面**：§9 全文、§10 全文的编号引用、§11 的每步判据措辞、`#1` 的 §4/§5。
**登记**：M5 的 `M5:84`/`M5:96` 已要求"M3 的 I21~I30 / 沿用 L1~L26" ⇒ 它以为"M3 只到 I30/L18"，
必须随本裁决一并顺延（`NOTE:351-355` M5-C1）。**本文件只登记，不改 M5 的任何文件。**

#### A2（= `M4-C2` [阻断]）tombstone/旧版本丢弃判据被写成"且"

| 候选 | 取舍 |
|---|---|
| (i) 按 `M4:37` 的合取（"只能在『无更旧版本』**且**『低于最小快照』时才可丢弃"） | 语义上等于"两条都要成立" ⇒ (a) **非最底层的旧版本永远丢不掉**（因为"最底层无重叠"对中间层恒假）⇒ 空间放大爆炸；(b) 把"快照保护"错当成 tombstone 的额外条件，掩盖"tombstone 可以在有快照时被丢"这一事实边界 |
| **(ii) 按 `M4:98-99`（I36/I37）分列，`M4:37` 只作摘要** | 与 `M4:69` 的分列写法（"必须逐条对应 I36/I37"）一致，与 `D3:1885`（I26）/`D3:1882`（I23）各自独立的既有写法一致；合取只在该 compaction 恰好覆盖最底层**且**无快照时才"碰巧等价" |

**推荐 (ii)。** 落地为**单独一个函数**（`M4:178` 要求）并在 §5.5 给出真值表：

```
drop_old_version := (last_sequence_for_key <= smallest_snapshot)                      // ← I41
drop_tombstone   := (ikey.type == kTypeDeletion)
                 && (ikey.sequence <= smallest_snapshot)                              // ← 可见性前提
                 && base_level_for_key                                               // ← I40
drop := drop_old_version || drop_tombstone                                           // ← 析取，不是合取
```
`base_level_for_key` 的精确定义（**这是本设计对 `M4:98` 的必要精确化**）：
`对 l ∈ [compaction 的输入层号 + 2, kNumLevels) 的每一层，该层中没有文件覆盖 ikey.user_key`。
**为什么从 `+2` 开始**：输入层 +1 的**全部重叠文件都是本次 compaction 的输入**，它们的内容会被归并进输出
⇒ 若把 +1 层也算进"更底层"，会**永远**返回 false（因为被压掉的旧值就在 +1 层）⇒ tombstone 永远丢不掉。
（LevelDB 的 `IsBaseLevelForKey` 用 `lvl = level_ + 2` 起步，同源理由。）

**影响面**：§5.5、§6.4、§9.2 的 I40/I41、§10.1 的 `M4-A22~A27`（其中 `M4-A26` 专测"析取而非合取"）。
**登记**：`M4:37` 的措辞**不得**被实现者当判据；`#1` 必须把 `M4:37` 标为"摘要"，把 I40/I41 标为"判据"。

#### A3（= `M4-C3` [表述]）MANIFEST"新建"与"追加"混在一句，CURRENT 切换时机未定

| 候选 | 取舍 |
|---|---|
| (i) 每次安装都"写新 MANIFEST → fsync → 追加 → fsync → 切 CURRENT" | 字面照 `M4:36` ⇒ 每次安装都做 2 次文件 fsync + `rename` + 2 次目录 fsync（`CURRENT.tmp` 的 rename 与 `SyncDir`）。在热路径上（每次 flush/compaction 安装）都付目录 fsync 的代价，且与 `D3:1912`（L17 加强版"rename 后必须 `SyncDir`"）结合后每层版本安装承担**两次**目录 fsync |
| **(ii) 拆成两个显式模式（`NOTE:164-167` 的裁决）** | (a) **新建/重建模式**（首次 `Open`、`CURRENT` 缺失、MANIFEST 超过阈值）；(b) **常规模式**（每次安装只向**当前** MANIFEST 追加 + `fsync`，**不碰 CURRENT**）。`M4:36` 的字面顺序理解为"(a) 的收尾 + (b) 在同一新文件上继续追加" |

**推荐 (ii)。** CURRENT 的语义（`M4:94` 的 I32）= "指向某个**完整持久化过**的 MANIFEST"——
只要 MANIFEST 是 append-only + 记录自带 CRC，追加就是安全的，不需要每次切指针。

模式 (a) 的完整顺序（**逐步不可交换**，§8.1 给时序图）：`写 MANIFEST-<n>.tmp → fsync → rename → SyncDir`;
`写 CURRENT.tmp → fsync → rename(CURRENT.tmp, CURRENT) → SyncDir`;然后才把旧 MANIFEST 入延迟删除队列。
模式 (b)：只 `append record + Sync()`。

**影响面**：§3.1、§8.1、§8.2、§9.3 的 L25/L28、§10.1 的 `M4-A08/A09/A10/A40`。
**登记**：`M4:190`（`#4` 评审项 3）逐字重复了字面顺序 ⇒ `#1` 必须把评审项 3 的对照对象改为"**按模式核对**"。

#### A4（= `M4-C4` [表述]）"可选引入 table cache"与 M3 已交付的 `TableCache` 重叠

| 候选 | 取舍 |
|---|---|
| (i) M4 新建 `src/table_cache.{h,cpp}`（照 `M4:133` 的【可选扩展】） | 与 M3 的 I30（`D3:1889`"文件句柄有上限且必须 RAII 释放"）形成**两套句柄预算** ⇒ 破坏单一真相源；且 `D3:1863`（E6）已把 `TableCache` 的落点定在 `version_set.h` |
| **(ii) 复用 M3 的 `TableCache`（`NOTE:173` 的裁决）** | `M4:74`/`M4:16` 的"可选"**只剩块缓存**一个决策 ⇒ 按 D8 的裁决 **不引入**（并写明理由 + 去向） |

**推荐 (ii)。** 强约束（写进 §6.3）：**compaction 的输入迭代器必须经 `TableCache` 拿
`shared_ptr<const Table>`，不得自行 `NewRandomAccessFile`**。理由：`D3:1889` 的 I30 把 fd 上界变成不变量；
compaction 同时持有"输入文件数 + 1 个输出 `WritableFile`"的句柄，若绕过缓存会与前台读共享同一预算并突破它。
`NOTE:220`（M4-R2）建议的 fd 计数断言落在 §10.2 `M4-B05`（形状照抄 `D3:1889` 的 `M3-B05`：`/proc/self/fd` 不增长）。

**影响面**：§6.3、§7.4、§9.2 的 I42、§10.2 的 `M4-B05`、§12.4（范围越界）。

#### A5（= `M4-C5` [阻断]）"L1+ 不重叠"是硬要求，但决策 1 把 size-tiered 列为候选

| 候选 | 取舍 |
|---|---|
| (i) 把 size-tiered 作为**可实现的实现分支**（`M4:67` 的字面候选） | `M4:95` 把重叠定义成"**拒绝安装该 Version**"的安装期校验 ⇒ 若允许 L1+ tiered，该不变量自身失效；且 `M4:122` 的风险（"compaction 与 flush 并发破坏层级约束 ⇒ 读放大暴涨甚至读错"）成为常态 |
| **(ii) L1+ 必须 leveled；size-tiered 只作"被否决方案"入档** | `M4:10`/`M4:95` 是硬约束、不可选（`NOTE:179` 的裁决） |

**推荐 (ii)。** 做法：在 `docs/amplification.md` 保留一行 size-tiered 的**预期**放大数据 +
"**结论作废：与 `M4:10`/`M4:95` 不兼容，未实现**"（`M4:23` 的负结果入档纪律要求"保留原文并标注结论作废"，
但**不得**伪造未做过的实测数字 ⇒ 明确标注为"预期值，非实测"）。
**不做**实现分支 ⇒ `Options` 里**不出现** `compaction_style` 之类的开关（避免 M1 §4.3 的
"看起来能用但没实现"的字段纪律，`D3:462`）。

**影响面**：§1.2、§3.4、§5.4、§9.2 的 I37、§10.2 的 `M4-B03`、§12.4。

#### A6（= `M4-C6` [表述]）"触发条件"的语义（L0 阈值 vs 每层容量）

**裁决（`NOTE:184-186`）**：两条触发**都成立**；`M4:68` 真正要定的是"**多层同时超限时选哪一层**"
与"**层内选哪个文件**"；文件 pick 按 `M4:68` 的推荐实现（轮转 或 与下层重叠最小），
**seek 热度列为可选扩展并在设计文档标注**（⇒ 本设计 §2.1 D2 与 §2.4 E8 标注"M4 不实现"）。

**理由（必须在设计文档里写清，否则 `#1` 会凭空补）**：`M4:19` 的判据要求同时给"L0 文件数**与**
各层字节数" ⇒ 两条判据都要可观测；二选一会让 L1+ 永远不被合并（容量无判据）或 L0 永不触发。
**影响面**：§3.5、§5.4。

#### A7（= `M4-C7` [表述]）"两种策略对照"指哪两种策略

**裁决（`NOTE:190`）**：对照落在**同一层级布局（L0 tiered + L1+ leveled）下的两种 pick/选层策略**：
**`轮转` vs `与下层重叠最小`**。分层布局**不参与**对照（A5 已定死层级布局为 leveled）。

**理由**：`M4:68` 明确说 seek 热度"留作可选扩展"；若把"未实现的扩展"拿去对照，等于 M4 要多做一份实现
才够两种策略，与 `M4:167`（"只实现让当前测试集通过的最小代码"）冲突。
**落地**：`Options::compaction_pick_strategy ∈ {kRoundRobin, kMinOverlap}`（§2.4 E9）⇒ **同一个二进制、同一脚本、
同一数据规模、同一机器**下交替运行（`M4:181` 要求），而不是编译两份。
**影响面**：§5.4、§10.2 的 `M4-B03`、`docs/amplification.md` 的固定列（`M4:206`）。

#### A8（= `M4-C8` [表述]）"重启后层级文件集合与崩溃前一致"未定义"崩溃前"

| 候选 | 取舍 |
|---|---|
| (i) 等于"崩溃瞬间**内存中**的文件集合" | 与 I39（`M4:97`："未落盘的文件号不得出现在任何 Version 中"）语义张力：崩溃瞬间可能正好有 compaction 输出未注册/未 fsync ⇒ 该判据**不可能**成立，会让正确的实现被判失败 |
| **(ii) 等于"崩溃前最后一个成功持久化的 Version"**（`NOTE:196` 的裁决） | 即 CURRENT → MANIFEST 可回放出的那个；未注册/半写输出被识别为**孤儿并计数清理**（`M4:126`、`M4:195`） |

**推荐 (ii)。** `M4:24` 的判据落地为：
`恢复后的层级文件集合 == CURRENT 指向的 MANIFEST 可回放出的集合` ∧ `未注册 .sst 与 .tmp 被清理且计数` ∧
`missing 0`（`M4:21`）。**影响面**：§8.3/§8.5、§10.2 的 `M4-B02`、§12.3（歧义消解）。

#### A9（= `M4-C9` [表述]）`META`（M3 已交付）在 M4 的处置

| 候选 | 取舍 |
|---|---|
| (i) `META` 与 MANIFEST **并存**（M4 双写） | **明令禁止**（`NOTE:201` 的裁决"不允许两套元数据在稳态并存"）。理由：`D3:2132-2135` 登记了 `META` 的缺陷（F 个文件时每次 flush 重写 ≈`12 + F×(28+2×key_len)` 字节；F=10000 ⇒ ≈700 KB），M4 正是它的解法；并存 = 保留缺陷 + 多一个损坏点 |
| (ii) **删除 `META` 路径**（不兼容读旧库） | 简单，但会**破坏 M3 既有测试的前提**：`D3:1886`（I27）的验证用例 `M3-A40/A41` 与 `P:346-363`（§10.8/§10.9，**已冻结的协议章节**）都以 `META` 为对象 |
| **(iii) MANIFEST+CURRENT 取代 `META`；首次 `Open` 兼容读取一次后**立即**改写为 MANIFEST 并删除 `META`** | 一次性迁移、稳态不并存；`D3:1704-1705` 已预留该迁移点（"只改 `Recover`/`Persist` 的函数体"） |

**推荐 (iii)**，迁移路径写死在 §3.6。
**影响面（必须显式登记，否则会撞 M3 的既有断言）**：

- `P` §10.8/§10.9（`P:346-363`）是**已冻结**的协议章节。本设计**不改它**，而是在 §4 的 §11 追加里
  写明"`META` 在 M4 起为**兼容读入格式**，不再是稳态写格式"⇒ 属于"追加说明"而不是"修改冻结章节"。
- **`M3-A39`/`M3-A40`/`M3-A41`（`D3:1886`、`D3:1888` 引用的用例）尚未落地**（`version_set.*` 不存在，
  `docs/m3-evidence.md:54` §4 第 9 条确认 `M3-B01~B05` 未交付）。⇒ **本设计把"这些用例的断言必须写成
  『以 `Recover`/`Persist` 的语义为准，不绑定 `META` 这个文件名』"登记为对 M3.3 的前置要求**（§13 Q3）。
  理由：`M4:134` 禁止改动 M1~M3 既有测试的断言 ⇒ **在 M3.3 尚未写下这些断言时裁决，代价为零；
  若先落地再改，就是违约**。这是本条裁决的**唯一正确时机**，必须现在拍板。
- 若用户否决 (iii) 而选 (ii)：M4 的 `Recover` 直接拒读 `META` ⇒ 必须在 M4.1 的提交里把
  `M3-A39~A41` 改写为 CURRENT/MANIFEST 形态，并**登记为对 `M4:134` 的显式例外**。

#### A10（= `M4-C10` [表述]）`M4:34` 的"只允许追加"清单与 `M4:132` 的追加点清单不一致

**裁决（`NOTE:207`）**：`M4:34` 是"**禁入**声明"（真正要禁的是"把 compaction 逻辑塞进 MemTable 或 SSTable
读路径" + 既有语义变更）；`M4:132` 是"**追加点**清单"；两者取**并集**作为允许的追加面，
并在 `docs/m4-prerequisites.md` §4 逐条列出"**许可追加符号**"白名单。

**白名单（本设计冻结，`#1` 照抄；`#4` 按此判定"越界与否"）**：

| 文件 | 许可追加的符号 |
|---|---|
| `src/db_impl.{h,cpp}` | `compaction_thread_` / `compact_cv_` / `compaction_pending_` / `compaction_scheduled_` / `MaybeScheduleCompaction()` / `BackgroundCompaction()` / `MakeRoomForWrite` 的**层级分支** / `InstallCompactionResults()` / `PendingDelete` 队列与 `MaybeDeleteObsoleteFiles()` / `manifest_file_` 与 `manifest_bytes_` / `GetLevelStats()` / `GetCompactionStats()` / `GetAmplificationStats()` / `GetManifestStats()` / `GetSnapshot()` / `ReleaseSnapshot()` / `GetAtSnapshot()` / `NewIteratorAtSnapshot()` / `smallest_snapshot_` 与快照集合 / `live_versions()` |
| `src/version_set.{h,cpp}` | `Version::level_files/Ref/Unref/ValidateLevelLayout/AllFiles` / `VersionSet::LogAndApply` / `manifest_*` / `live_versions_` / `Recover`+`Persist` 的**函数体**替换 |
| `src/version_edit.{h,cpp}` | `AddFile(int,...)` / `DeleteFile` / `Clear` / `added_files` / `deleted_files` / `DebugString` |
| `src/filename.{h,cpp}` | `ManifestFileName` / `ManifestTempFileName` / `CurrentFileName` / `CurrentTempFileName` / `ParseManifestFileName` |
| `src/common.h` | `Options` 的 6 个新字段（§5.6）+ `CompactionHook` 类型 |
| `-禁止-` | **禁止**在 `src/memtable.{h,cpp}` / `src/skiplist.h` / `src/wal.{h,cpp}` / `src/sstable/*` 的**语义**上做任何改动（`sstable/*` 只允许 M3.2 已承诺的"随机读 API + 计数 seam"那批，且必须已在 M3.2 落地，M4 不再动） |

**影响面**：§5.1（文件清单）、§6 全文、§12.4。

#### A11（= `M4-C11` [表述]）M4 需要新增 `Options` 字段，但 `M4:132` 未列 `src/common.h`

**裁决（`NOTE:212`）**：允许在 `src/common.h` 的 `Options` 追加最小字段集；`M4:134` 的禁止项只针对
"内部 key 编码与比较规则"，**不**禁止 `Options` 扩字段。字段表与默认值见 **§5.6**。
**理由**：`D1` 的 `Options` 在 M2/M3 已有"追加字段并登记"的先例（`src/common.h:244-249` 的 `env`/`commit_hook`
都带"登记于 docs/mX-prerequisites.md §9"注释）；且 `PR3:509` 已把 `src/common.h` 列入 M3 的【必须改】
（`block_size`/`verify_checksums`/`max_open_files`/`recycle_log_files`/`flush_hook`）。

**本设计的额外处置（`NOTE` 未覆盖，本轮新增裁决）**：M4 使用的 `block_size` / `verify_checksums`
必须来自**同一份 `Options`**。实测当前它们只存在于 `src/sstable/table.h:37-39` 的 `TableOptions`
（`docs/m3-evidence.md:42-44` §4 第 1 条已登记为未闭合项，并写明"M3.2 决定"）。⇒
**M4 的裁决**：M4 **依赖 M3.2 的收敛结果**；若 M3.2 交付时仍未收敛（`TableOptions` 独立存在），
则 M4.1 的提交里**必须**完成收敛（推荐方向：`Options` 持有 `block_size`/`verify_checksums`，
`TableOptions` 变成 `Options` 的**只读投影**（`Table::Open` 改为接受 `const Options&` 或从 `Options` 构造
`TableOptions`），并更新 `M3-A11/A15` 的构造口径 ⇒ **属于"跨里程碑接口收敛"，需要用户在 §13 Q4 确认**。
**影响面**：§5.6、§11 的 M4.1、§12.5 薄弱点 6。

#### A12（补充裁决，针对 `M4:20`）"量级分离"必须落成可判定数字

**裁决（`NOTE:449`）**：`M4:20` 的"前台 P99 与 compaction 单轮耗时有量级上的分离"落成
**可判定数字**，并**同时打印两者的分子/分母**。

**落地口径（精确到可比）**：

```
门禁数字（写死，写入 docs/amplification.md 与 #1 的验收清单）：
    p99_front_us * 10 <= p50_compaction_round_us          ← "P99 ≤ 单轮中位耗时的 1/10"
其中：
    p99_front_us          = FRONT 行的 p99_us（前台 Put/Get 的端到端延迟，含 sync=true 的 fsync）
    p50_compaction_round_us = AMPL 行 round_id=ALL 的 compaction_round_p50_us 换算成 us
    两个数字必须来自**同一脚本、同一轮、同一机器、同一数据规模**（M4:181）
必须同时打印的分母：
    FRONT ... ops=<n> sync_ops=<k>       ← P99 的分母 = 前台操作数（并区分 sync/非 sync）
    AMPL  ... compaction_rounds=<n>      ← 单轮中位数的分母 = 轮数
```
**为什么用"中位"而不是"最大"**：`M4:20` 要证的是"**常态不阻塞**"；单轮最大值会受一次超大 compaction
（例如 L2→L3）主导，用它做分母会把门禁变成不可判定。最大值另打一列 `compaction_round_max_ms` 供观察，
**不作为门禁**。
**若门禁不成立**：说明前台 P99 被 compaction 的**临界区**拖住（而不是被它自己的 fsync 拖住）⇒
按 §9.3 的 L25（锁内不写 MANIFEST）/L26（锁内零 IO）逐条核对；**这属于设计被证伪，按 `RM:61` 入档并回退 `#0`**
（`M4:40` 的流程锁要求回退设计，不得在校验阶段私改实现）。
**影响面**：§5.7、§10.3、§10.2 的 `M4-B07`、§11 的 M4.3 判据。

### 2.3 本轮新发现的歧义（`NOTE` 的 C1~C11 未覆盖；逐条给候选/取舍/推荐/影响面）

#### A13（新）快照句柄的 API 落点未定

**事实**：`M4:70`（决策 4）要求"快照集合按 sequence 排序取最小；**快照句柄的生命周期与释放**"；
`M4:13` 要求"最小快照 sequence 决定 tombstone 与旧版本何时可丢"。
但 `M4:132` 的【必须改】对 `src/db.{h,cpp}` 只写"**如需**对外暴露层级统计/放大统计查询入口则最小扩展"
——**没有**授权加快照 API；且 `PR3:52` §1.2 把"`ReadOptions::snapshot`/`Snapshot*` 公共 API"
列为**非目标**。⇒ 落点必须裁决。

| 候选 | 取舍 |
|---|---|
| (i) 在 `src/db.h` 加公共虚接口（`virtual const Snapshot* GetSnapshot()=0; virtual void ReleaseSnapshot(const Snapshot*)=0;` + `ReadOptions`） | 它**必须**同时被 M1 的内存模式 `DBImpl`（`src/db.cpp`）实现 ⇒ 要改 M1 的实现文件；若做成带默认实现的非纯虚函数，就引入"看起来能用但没实现"的接口（违反 `D3:462` 的字段纪律与 M1 §4.3）。而且 `M4:132` 没授权 |
| **(ii) 不改 `src/db.h`；快照 API 落在 `PersistentDBImpl`（具体类）上** | 沿用 `PR3:686-702` §9 **D9.4** 的既有先例（"既有测试直接依赖具体实现类 `PersistentDBImpl`（11 处）"）；`D3:1799`（`GetFlushStats`）与 `D3:1637`（`GetReadStats`）也都在具体类上。⇒ 零改动 `src/db.{h,cpp}`，零改动 M1 |

**推荐 (ii)**，接口形态（签名草案，`#1` 照抄）：

```cpp
// src/db_impl.h（仅在具体类上；**不动** src/db.h）
struct Snapshot { SequenceNumber sequence; };   // 值类型；句柄 = const Snapshot*，所有权归调用方

class PersistentDBImpl : public DB {
 public:
  // 快照（I41 的判据构造入口）。GetSnapshot 返回的句柄**必须** ReleaseSnapshot 释放。
  const Snapshot* GetSnapshot();                       // 加入快照集合，重算 smallest_snapshot_
  void ReleaseSnapshot(const Snapshot* s);             // 移出集合，重算 smallest_snapshot_
  // 快照读（单一真相源：两者都转调私有 GetInternal/NewIteratorInternal）
  Status GetAtSnapshot(const Snapshot* s, const Slice& key, std::string* value);
  Iterator* NewIteratorAtSnapshot(const Snapshot* s);
 private:
  Status GetInternal(SequenceNumber snapshot, const Slice& key, std::string* value);
  Iterator* NewIteratorInternal(SequenceNumber snapshot);
  std::multiset<SequenceNumber> snapshots_;            // mutex_
  SequenceNumber smallest_snapshot_ = 0;               // mutex_；空集时 = last_sequence_
};
```
`DB::Get`/`DB::NewIterator`（虚函数）在 `PersistentDBImpl` 的实现 = `GetInternal(snapshot_of(last_sequence_), ...)`
⇒ **不存在第二份读路径**（M2 教训①"判据只允许单一真相源"）。

**影响面**：§5.5、§7.2/§7.3、§9.2 的 I41、§10.1 的 `M4-A28/A29/A30`。
**登记（去向）**：公共 `DB::GetSnapshot()` / `ReadOptions::snapshot` 属 M5/M6 的接口扩展；
本设计**在 `#1` 的风险清单里写一行**"公共快照 API 未提供，`lsm-kv` 的外部使用者目前不能做快照读"。

#### A14（新）B 组驱动与统计入口的落点未定

**事实**：`M4:34` 的文件清单只列了 `scripts/lsm_compaction_stress.sh`（**没有** C++ 驱动）；
`M4:159` 要求固定输出行 `ROUND / ACKED / RECOVERED / MISSING`，`M4:170`（决策 7）要求"是否需要 status/日志命令"
必须明确。实测 `scripts/crash_writer.cpp:16`/`crash_recover.cpp:19` **只 include `db.h`/`filename.h`/`util/env.h`**
⇒ 拿不到 `PersistentDBImpl` 的层级/放大统计。

| 候选 | 取舍 |
|---|---|
| (i) 新增 `scripts/lsm_compaction_stress.cpp`（写压 + sidecar）+ `scripts/lsm_compaction_verify.cpp`（对账 + 统计打印） | 与 M2 的 `crash_writer`/`crash_recover` 重复一大段 sidecar 协议 ⇒ **两份 sidecar 协议会漂移**（M2 教训①） |
| **(ii) 复用 M2 的两个工具（`lsm_crash_writer`/`lsm_crash_recover`，带"小写缓冲强制 flush"的参数），另加一个只读的 `scripts/lsm_level_stats.cpp` 打印层级/放大统计** | `M4:159` 明写"复用 M2/M3 对账脚本"，`M4:181` 要求"同一脚本"；M2 的工具已支持 `--write-buffer-size`（`D3:1845` G5 用的就是这个参数）；新增脚本极薄 |

**推荐 (ii)**，具体：

| 产物 | 内容 | 改动性质 |
|---|---|---|
| `scripts/lsm_compaction_stress.sh`（`M4:34` 要求） | 每轮独立临时目录、**随机 kill 延迟但打印可复现种子**（`M4:180`/`M4:196`）、从 `ROUND` 行 `sed` 出四个计数、`missing != 0` ⇒ `exit 1`、末尾打印 `[COMPACTION_CRASH_OK]` 正向标记（形状照抄 `scripts/lsm_crash_test.sh:51-70` + `D3:1847` G7） | 新增 |
| `scripts/lsm_level_stats.cpp` + `add_executable(lsm_level_stats ...)` | 只读打开一个目录，打印 `LEVEL` / `AMPL` / `FRONT` 三行（§10.3 的固定格式）；**必须**经 `db_impl.h` 拿统计 | 新增（薄） |
| `lsm_crash_writer` / `lsm_crash_recover` | **不改**（复用） | 无 |
| `scripts/lsm_gate.sh` | 追加 M4 腿 + **正向标记 AND**（`M4:196`、`D3:1847`） | 追加 |

**影响面**：§10.2、§10.3、§11 的 M4.3、`CMakeLists.txt`。
**登记**：这是对 `M4:34` 文件清单的**必要追加**（`M4:182` 允许"`CMakeLists.txt` 最小改动"），
在 §15 的 R 记录里登记。

#### A15（新）`M4:172` 把"读放大改善证据"放在 M4.2 门禁，但 `M4:163` 把它放在 B 组

**事实**：`M4:172`（M4.2 的通过判据）含"**读放大统计有改善证据**"；`M4:163`（B 组）含
"长时间压测下的空间放大与文件数统计"。⇒ "改善证据"的**证据类型**未定（确定性 vs 实测）。

| 候选 | 取舍 |
|---|---|
| (i) 推迟到 M4.3 的 B 组才给 | 与 `M4:172` 的字面冲突：M4.2 的门禁过不了 |
| **(ii) M4.2 用 A 组确定性口径给"结构性改善"的证据，M4.3 用 B 组给"实测数字"** | 两者都满足：A 组可断言**上界**（`files_checked ≤ level0_trigger + 2 + (kNumLevels−1)`，由 §3.4 的不变式 + `D3:1400` 的在途 flush 上界保证），B 组给 p50/max 实测与 M3 基线（`D3:2136` 的 F≈59）的对照 |

**推荐 (ii)**：M4.2 判据 = `compaction_test` A 组全绿（含 `M4-A19` 的 `files_checked` 上界断言，**确定性**）；
M4.3 判据 = `M4-B06` 的实测行 + `M4-B03` 的对照表。
**影响面**：§10.1 的 `M4-A19`、§10.2 的 `M4-B06`、§11 的 M4.2/M4.3 判据。

### 2.4 指令外的补充决策（E1~E10；`D3:1852-1866` 的 E1~E8 是同形先例）

| # | 决策 | 方案与取舍 | 推荐 | 占 I/L 号? |
|---|---|---|---|---|
| **E1** | **L0 输入集是否必须做"传递重叠闭包"** | (a) 只取"与种子文件 key range 重叠"的文件（单趟）；(b) 取**传递闭包**（新加入的文件若扩大了 range，则重新扫描） | **(b)，且是硬要求**。反例（必须写进 `#1` 的风险清单）：L0 有 `A[n=1, a..b]`、`B[n=2, c..d]`、`C[n=3, b..c]`（桥接）。种子取 `A` ⇒ (a) 得 `{A, C}`；剩余 `B` 持有 `key=c` 的更**旧**值（`B` 是 n=2，`C` 是 n=3）⇒ 输出已把 `c` 的新值写进 L1，而读路径先查 L0 的 `B` ⇒ **读到陈旧值**（I44 被破坏）。传递闭包下 `{A,B,C}` 全进输入 ⇒ 无残留重叠文件 | 否（**§9.5 的 X1**） |
| **E2** | **compaction 输出文件的切分边界** | (a) 按 `max_file_size` 字节数任意切；(b) 只在 **user key 变化处**切 | **(b)，硬要求**。若同一 user key 的多个版本落到两个输出文件，两个文件的 key range 都覆盖该 user key ⇒ 违反 I37（层内互斥）；且层内没有"谁更新"的判据（`D3:1530` 已冻结"不用 max_sequence 判新旧"）⇒ 读路径无法判定 | 否（**X2**） |
| **E3** | **MANIFEST record 的物理帧** | 见 §2.1 D6 的 (a)(b)(c) | **(b)：`length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)`，CRC 覆盖 `length‖type‖payload`**（与 `P:248-280` §10.3 同形）。`type` 用 **MANIFEST 自己的枚举空间**（`kManifestRecordTypeVersionEdit = 0x01`），**不**复用 `src/sstable/format.h` 的 `BlockType`（避免触碰 M3 冻结的枚举） | 否 |
| **E4** | **`VersionEdit` 是否引入 `kLastSequence`（LevelDB 同名字段）** | (a) 引入，恢复时 `last_sequence_ = max(各 edit 的 last_sequence, 各文件 max_sequence)`；(b) **不引入** | **(b)**。理由：`D3:1724-1728`（§8.2 纪律 1）明文"**`META` 里不存『注册时刻的 `last_sequence_`』**"，并给出"会丢数据"的论证；`D3:1729-1731` 纪律 2 又要求"恢复期不做任何基于水位的跳过"。⇒ 引入该字段等于把已被否决的失效模式请回来。恢复水位由 **I31（`M3` 的，未改号）** 的 `max(WAL 重放最大值, 各文件 max_sequence)` 唯一确定，**不需要**任何 edit 级水位。字段**不定义**（不是"定义但不写"——避免"看起来能用"） | 否 |
| **E5** | **compact pointer 是否持久化** | (a) 持久化（写在 `VersionEdit` 里）；(b) 只在内存 | **(b)**。理由：持久化要求每次选层后额外写一条 edit（一次 fsync/次选层）⇒ 把 IO 加进热路径；不持久化的唯一代价是"重启后各层 compaction 从最左重新开始"⇒ 只多做一些 compaction，**不影响任何正确性判据**。同时**不在协议里定义**该 tag（E4 同纪律）。登记为 M5 的候选 | 否 |
| **E6** | **`max_file_size` 是否约束 flush 输出** | (a) 约束（flush 也按 2 MB 切分）；(b) 只约束 compaction 输出 | **(b)**。理由：(a) 会改 M3 的 flush 契约（一表一文件，`D3:1322` 步骤 ⑦）并让 L0 文件数翻倍（4 MB 写缓冲 ⇒ 2 个 L0 文件/次 flush）⇒ 直接改变 `level0_file_num_compaction_trigger = 4` 的**实际数据量**语义。⇒ `max_file_size` **只**用于 compaction 输出滚动（`M4:12` 的原文就是"Compaction 执行…输出文件大小上限"） | 否 |
| **E7** | **是否实现祖父层重叠限制（LevelDB 的 `ShouldStopBefore`）** | (a) 实现；(b) 不实现 | **(b)**，并登记。理由：它需要引入"第三层"参与输入选择（grandparents 来自 L(n+2)），`M4` 未要求；且 `M4:167` 要求"只实现让当前测试集通过的最小代码"。**代价必须写明**：缺少它时，一次 L(n)→L(n+1) 的输出可能与 L(n+2) 大量重叠 ⇒ 下一次 L(n+1)→L(n+2) 的输入偏大（**放大统计要能观测到这一点**，`M4-B04` 承担） | 否 |
| **E8** | **seek 热度选文件** | (a) 实现；(b) 不实现 | **(b)**，逐字落 `M4:68` 的"留作可选扩展并在设计文档标注"。**注意**：`M4:127` 把它列为风险（"统计错误 ⇒ 某些文件永远不被 compaction"）⇒ 不实现即**消除**该风险；但必须登记"因此 `M4-B03` 的对照只有两种策略（A7）" | 否 |
| **E9** | **两种 pick 策略的运行时开关落点** | (a) 编译期开关（两份二进制）；(b) `Options` 字段（枚举） | **(b)**：`Options::compaction_pick_strategy`（默认 `kRoundRobin`）。理由：`M4:181` 要求"同一脚本、同一数据规模、同一机器"⇒ 编译两份二进制会让"同一机器"的对照被构建差异污染；且脚本必须能**在同一轮内交替**（`RM:60` §3.7 的单机数字纪律 + `NOTE:411` M5-R8 的"同轮交替"教训） | 否（`Options` 字段，见 §5.6） |
| **E10** | **compaction 路径的确定性故障注入点** | (a) 只靠脚本的随机延迟 kill；(b) 新增 `CompactionHook`（形状照抄 `D3:1865` 的 `FlushHook`） | **(b)**。理由：`M4:160` 要求"compaction **中途** `kill -9` 的版本恢复"，`M4:180` 要求"kill 时机**随机但可复现**（种子打印）"——随机延迟在"写输出后 / rename 后 / 注册前"这三个窗口上的命中率极低且**不可复现**；`D3:1865`（E8）已为 flush 立了三个注入点（`OnSSTableWritten`/`OnBeforeRename`/`OnBeforeRegister`）。⇒ M4 新增 `class CompactionHook { OnInputsSelected(); OnOutputWritten(); OnOutputRenamed(); OnBeforeInstall(); }`，`Options::compaction_hook = nullptr`（生产恒 `nullptr`，只做观察，不含生产逻辑） | 否（`Options` 字段 + 新类型，见 §5.6） |

**E1~E10 全部不占用 I/L 号空间**；其中 E1/E2 被提升为**补充约束 X1/X2**并进入 §9.5
（因为它们承载"正确性"而非"工程取舍"，`#4` 评审必须逐条核对）。

---

## 3. MANIFEST / VersionEdit 的位级布局与版本模型

> **本节与 `docs/protocol.md` 追加的 §11 必须逐字一致**（§4 给出追加 patch 的完整文本）。
> 评审逐字对照检查（`M4:179`："VersionEdit 编解码必须与 `docs/protocol.md` 的追加章节完全一致，
> 编码实现与文档不得各说一套"）。
> 所有多字节整数**小端（LE）**，逐字节拼装/解析，**禁止** `reinterpret_cast` 到整型指针（`P:6-7`）。

### 3.1 目录布局与文件命名

```
<dbname>/
  LOCK                     进程级独占（M2 的 D10，不变）
  CURRENT                  指向当前 MANIFEST 的原子指针（M4 新增；ASCII 十进制编号 + '\n'）
  CURRENT.tmp              CURRENT 的写临时文件（rename 的源；**永不**被当作 CURRENT 读）
  MANIFEST-<n>             VersionEdit 追加日志（M4 新增；= 恢复的权威源）
  MANIFEST-<n>.tmp         MANIFEST 的写临时文件（**永不注册**）
  META / META.tmp          M3 的单快照元数据：**M4 起为兼容读入格式**（§3.6），稳态不写
  %06u.log                 WAL（M2）
  %06u.sst                 SSTable（已注册即不可变）
  %06u.sst.tmp             SSTable 的写临时文件（**永不注册**）
```

| 规则 | 内容 | 理由 |
|---|---|---|
| 编号格式 | `%06u` 十进制零填充（与 `.log`/`.sst` 同口径）；**解析一律按数值**，不靠字符串序 | `D3:577`；`D3:581` 的正则 |
| 编号空间 | **`.log` / `.sst` / `MANIFEST-<n>` 共享同一个单调递增的 `next_file_number`** | 沿用 `D3:578` 的"单一编号源"论证；**关键后果**：`Open` 时算 `next_file_number` 的目录扫描**必须**把 `MANIFEST-<n>` 的 `n` 计入（`D3:579` 的"权威值 = max(记录的 next_file_number, max(目录编号)+1)"），否则**会重用 MANIFEST 编号** ⇒ 覆盖掉正在被 CURRENT 指向的文件 |
| `CURRENT` | 内容 = `^[0-9]{1,20}\n$`（**纯十进制编号 + 恰好一个换行**）；读侧**严格校验**，不符 ⇒ `kCorruption` | 若允许多种写法（有无换行、带后缀、带目录），就会出现"两个都能解析"⇒ 不是"半个文件"防护；严格形态让"半个文件/被截断的 CURRENT"必然落 `kCorruption` |
| `MANIFEST-<n>` 命名 | 文件名带 `MANIFEST-` 前缀；**不参与** `.sst`/`.log` 的正则 | 三个族互不干扰：`^[0-9]{6}\.log$` / `^[0-9]{6}\.sst$` / `^[0-9]{6}\.sst\.tmp$` / `^MANIFEST-[0-9]{6}$` / `^MANIFEST-[0-9]{6}\.tmp$` |
| `ParseTableFileName` 必须拒绝 `.sst.tmp` | 后缀匹配必须**精确**比较 `.sst`，禁止 `starts_with` | `D3:582`（半个文件进版本 = 最危险的失效模式）；M4 保持不变 |
| `CURRENT.tmp` / `MANIFEST-<n>.tmp` | 前缀/后缀精确匹配；`ParseManifestFileName` **必须**拒绝 `MANIFEST-<n>.tmp`（同 `D3:582` 的理由） | 否则 `.tmp` 会被当成"可读的 MANIFEST" ⇒ 回放一个半写文件 |

**命名函数（追加到 `src/filename.{h,cpp}`）**：

```cpp
constexpr const char* kManifestFilePrefix = "MANIFEST-";
std::string ManifestFileName(const std::string& dbname, uint64_t number);      // MANIFEST-%06u
std::string ManifestTempFileName(const std::string& dbname, uint64_t number);  // MANIFEST-%06u.tmp
std::string CurrentFileName(const std::string& dbname);                        // CURRENT
std::string CurrentTempFileName(const std::string& dbname);                    // CURRENT.tmp
bool ParseManifestFileName(const std::string& fname, uint64_t* number);        // **拒绝** .tmp
// M3 承诺但尚未落地的（`PR3:509`）：TableFileName / TempTableFileName / ParseTableFileName /
// ParseTempTableFileName / MetaFileName / MetaTempFileName —— M4 复用，不重新定义
```

### 3.2 MANIFEST record 帧格式（`E3` 的落地）

```
manifest_on_disk := record*
record           := length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)
  length  = payload 的字节数（不含 length/type/crc 自身）        // 与 §10.3 的 `length` 同口径
  type    = 记录类型（M4 只定义 1 个值：0x01 = VersionEdit）
  crc     = crc32c( length(4B LE) ‖ type(1B) ‖ payload )        // **含长度**（P:478-489 §9.3 的纪律）
```

| 字段 | 字节数 | 字节序 | 取值 / 约束 |
|---|---|---|---|
| `length` | 4 | LE | `>= 1`（VersionEdit 至少要有内容）；`<= 64 MiB`（**软上界**，见下） |
| `type` | 1 | — | `0x01 = kManifestRecordTypeVersionEdit`；**其他值 ⇒ `kNotSupported`**（不是 `kCorruption`） |
| `payload` | `length` | — | §3.3 的 VersionEdit 编码 |
| `crc32c` | 4 | LE | 覆盖 `length ‖ type ‖ payload` |

**约束与理由**：

1. **不含长度的 CRC 一律否决**（`P:481` D3 方案 C 的同一理由："先用错长度取到错 payload，再『碰巧』CRC 失败"
   是间接推断）。
2. **`length <= kMaxManifestRecordBytes = 64 MiB`**：与 `WALWriter` 的 `kMaxLogicalRecordSize = 64 MiB`
   （`VM:src/wal.h:23`，`NOTE:372` 引用）取同一数量级。**为什么是软上界**：全量快照 edit 的规模
   ≈ `F × (2 + 8 + 8 + 8 + 2×key_len)`（见 `D3:2132` 的同形估算）⇒ F = 10 000、key 16 B 时 ≈ 680 KB，
   离 64 MiB 有三个数量级；**超出上界 ⇒ `kCorruption`**（不是静默截断），因为"要写这么大一条 record"必然是 bug。
3. **同一 MANIFEST 内 record 必须按写入顺序、连续、无填充**（没有 WAL 的跨块 padding 状态机）⇒
   reader 是"从头顺序读、每条自定界"的**无状态**循环。
4. **每条 record 的解码必须"全或无"**：先校验 `length`/`type`/`crc`，再整体解码到**临时 `VersionEdit`**，
   成功后才应用到版本视图（`VersionEdit::Clear()` + 逐字段）。**禁止**边解边应用（半条 record 生效 =
   `P` §9 的"半条 record 永不生效"（`docs/m2-design.md:100`，I14）在元数据侧的对应物）。

**与 SSTable 块外壳的"同形"是刻意的**：`P:248-280`（§10.3）已冻结这个形状，`M3-A08/A13/A17` 已测过
同形的失败矩阵（`handle.size` 是"读多少"的唯一真相源、先校验长度再 CRC）。⇒ A 组必须有一条
**手工拼字节参照**用例（`M4-A04`），把 record 的期望字节**独立于实现**地写出来
（沿用 `tests/test_harness.h:12-14` 的 `ManualInternalKey` 纪律："故意不复用 coding.h 与 common.h 的实现，
这样实现写错时测试能抓到，而不是『实现与测试同错』"）。

### 3.3 `VersionEdit` 字段表与编码

```
version_edit_payload := field*
field                := tag(varint32) ‖ value(tag 依赖)

tag 值（M4 只定义**实际使用**的 6 个；不留"看起来能用"的空 tag —— 同 E4/E5 的纪律）：
  1  kComparator        : len_prefixed_string             // comparator->Name()
  2  kLogNumber         : varint64                        // 当前 WAL 编号
  3  kNextFileNumber    : varint64                        // 分配器提示（权威值在 Open 时重算，§3.1）
  4  kMinLogNumberToKeep: varint64                        // WAL 回收水位（M3 的 I34，不变）
  5  kDeletedFile       : level(varint32) ‖ number(varint64)
  6  kNewFile           : level(varint32) ‖ number(varint64) ‖ file_size(varint64)
                          ‖ max_sequence(varint64) ‖ smallest(len_prefixed) ‖ largest(len_prefixed)
```

| tag | 字段 | 编码 | 必选? | 语义与约束 |
|---|---|---|---|---|
| 1 | `comparator_name` | `PutLengthPrefixedSlice` | 仅**全量快照** edit 必选 | 恢复时与 `options.comparator->Name()` 比较，不符 ⇒ `kInvalidArgument`（沿用 `D3:1748` 与 M2 的 D13 残留） |
| 2 | `log_number` | varint64 | 仅全量快照 edit 必选 | 当前 WAL 编号 |
| 3 | `next_file_number` | varint64 | 仅全量快照 edit 必选 | 提示值；权威值 = `max(它, max(目录编号)+1)`（`D3:579`） |
| 4 | `min_log_number_to_keep` | varint64 | 仅全量快照 edit 必选 | M3 的 I34 判据（`D3:1893`），M4 不改 |
| 5 | `deleted_file` | `level` + `number` | 增量 edit 用 | `level ∈ [0, kNumLevels)`；`(level, number)` 必须在**应用该 edit 时**的版本里存在，否则 `kCorruption`（`#1` 登记为不变量检查） |
| 6 | `new_file` | 见上 | 增量 edit 用 | `level ∈ [0, kNumLevels)`；`number` 不得重复；`file_size > 0`；`smallest`/`largest` 必须是**合法 internal key**（`ParseInternalKey` 成功且 `user_key` 非空）；`smallest <= largest`（按 `InternalKeyComparator`） |
| — | `last_sequence` | **不定义** | — | 见 `E4`：`D3:1724-1731` 已否决 edit 级水位 |
| — | `compact_pointer` | **不定义** | — | 见 `E5` |

**Varint64 的必要性**：`number` / `file_size` / `max_sequence` 用 varint64 而非定宽 8 B。
**取舍（说明，避免 `#4` 质疑"为什么和 `META` 不一致"）**：`META`（`P:346-363` §10.8）是**定宽**的
（`number(8B LE) ‖ file_size(8B LE) ‖ max_sequence(8B LE)`），它的设计目标是"单文件全量快照 + 一次解析"；
`MANIFEST` 是**追加日志**、每条 record 越短越好（写放大直接进 I45 的统计）。
两者**格式不同但不冲突**：`META` 是兼容读入格式（§3.6），MANIFEST 是稳态格式（§3.2）。
⇒ 这条差异必须写进 `#1` 的"格式对照表"，避免评审以为"两套编码说同一件事"。

**`level` 的宽度**：`varint32`（1 B for `level < 128`）。**`kNumLevels = 7`**（编译期常量，
**不入 `Options`**，理由与 `D3:1824` 的 `kRestartInterval` 同口径："避免可配出非法值"）。

**全量快照 edit 的构成（模式 (a) 的首条 record，也是 `META → MANIFEST` 迁移的载体）**：

```
全量快照 edit :=
  tag1 comparator_name
  tag2 log_number
  tag3 next_file_number
  tag4 min_log_number_to_keep
  tag6 new_file × (当前 Version 的**全部**文件，按 (level 升序, 层内序) 展开)
无 tag5（没有"删除"：快照从空版本开始应用）
```
⇒ 恢复 = **从空版本开始，按顺序应用每条 record**。因此**不需要**"这是快照"的标志位：
新建/重建 MANIFEST 的第一条就是"相对空版本的全量增量"，语义自洽（LevelDB 同）。

### 3.4 层级布局与层级不变式（I37/I38 的落地形态）

```
L0（tiered）：文件按 **number 降序**（新→旧）；**允许** key range 重叠；读路径**逐个检查**
L1..L6（leveled）：层内按 **smallest user key 升序**；**任何两个文件不得共享任何 user key**
```

**层内互斥的精确判据（**本设计对 `M4:95` 的收紧，必须登记**）**：

```
对层内相邻两文件 f_i（前）与 f_{i+1}（后），必须同时满足：
    (1) f_i.largest.user_key  <  f_{i+1}.smallest.user_key        // **严格**，不得相等
    (2) icmp.Compare(f_i.largest, f_{i+1}.smallest) < 0            // internal key 全序上的等价表述
```

**对 `M4:95` 的"允许端点相等但不得覆盖"的重释（收紧，登记为 §15 R5）**：

- internal key 序是 `user key 升序 + trailer 降序`（`P` §6.1，`D3:1509` 一带）。
  若 `f_i.largest.user_key == f_{i+1}.smallest.user_key = k`，设它们的 trailer 为 `s1`、`s2`
  （`f_i.largest` 是 `k` 在 `f_i` 中的**最小** sequence，`f_{i+1}.smallest` 是 `k` 在 `f_{i+1}` 中的**最大** sequence，
  由 `D3:1859`（M3 的 E2，`smallest`/`largest` 用 internal key）与 `P:355-357`（`largest` 的 trailer 反而最小）
  共同决定 ⇒ `s1 < s2`）⇒ `Compare((k,s1),(k,s2)) > 0` ⇒ **判据 (2) 必然失败**。
  ⇒ "端点相等"在 M4 的实现里**被收紧为"不得相等"**。
- **为什么必须收紧**：若两个文件都含 `k`，读路径在层内无法判定"谁更新"——层内**没有**"谁更新"的判据
  （`D3:1530` 已冻结"**不用** key range 或 `max_sequence` 决定谁新"）。⇒ 允许相等 = 读出陈旧值。
- **副作用（必须一并写清）**：输出侧的切分必须**只在 user key 变化处**发生（`E2`/`X2`），
  否则一次 compaction 就会产出违反 I37 的两个文件。

**安装期校验（`M4:95` 的"违反即拒绝安装该 Version"）**：

```cpp
// src/version_set.h
// 返回 false + 具体定位（层号 / 两个文件号 / 冲突的 user key）⇒ 调用方**拒绝安装该 Version**
bool ValidateLevelLayout(const LevelFiles& levels, const InternalKeyComparator& icmp, std::string* why);
```
调用点（**必须全覆盖**，`M4:191` 的评审项 4 要求"L1+ 重叠检测是否覆盖全部安装路径"）：

| 安装路径 | 是否校验 |
|---|---|
| `VersionSet::Recover` 回放完的最后一步 | **是**（违反 ⇒ `kCorruption`，不自动修复） |
| 每一次 `LogAndApply`（flush 安装 / compaction 安装 / 迁移安装） | **是**（违反 ⇒ `kCorruption` + fail-stop） |
| `META → MANIFEST` 迁移 | **是** |
| A 组的构造性检查（`M4-A17` 直接构造非法布局断言拒绝） | **是** |

### 3.5 容量约束表与触发打分（`M4:10`/`M4:19` 的落地）

| 层 | 容量 | 触发判据 | 备注 |
|---|---|---|---|
| L0 | **无字节上限** | `num_files(0) >= level0_file_num_compaction_trigger`（默认 **4**，`M4:10`） | 允许重叠；`score(0) = num_files(0) / trigger` |
| L1 | `max_bytes_for_level_base`（默认 **10 MB**，`M4:10`） | `bytes(1) >= MaxBytesForLevel(1)` | 不重叠 |
| L2..L6 | `MaxBytesForLevel(l) = MaxBytesForLevel(l−1) × max_bytes_for_level_multiplier`（默认 **10**） | 同上 | **L6 = 最底层**（`kNumLevels = 7`）；L6 超限时**不再向下**（由它承担空间放大），并计数上报 |

```
MaxBytesForLevel(l):
    l == 0                       -> 无意义（L0 用文件数判据；调用方不得对 l==0 调它）
    l == 1                       -> max_bytes_for_level_base
    l >= 2                       -> MaxBytesForLevel(l-1) * max_bytes_for_level_multiplier   // 整数乘，饱和到 uint64 上界
score(l):
    l == 0                       -> num_files(0) / level0_file_num_compaction_trigger        // 用**文件数**
    l >= 1                       -> total_bytes(l) / MaxBytesForLevel(l)                     // 用**字节数**
选层:
    best := argmax_{l ∈ [0, kNumLevels)} score(l)，要求 score(best) >= 1
    平手（分数相等）时取**层号最小者**（⇒ L0 优先）
    不存在 score >= 1 的层 ⇒ 本轮无 compaction
```

**为什么 L0 用文件数、L1+ 用字节数（必须写清，否则 `#4` 会问"为什么两套判据"）**：
`M4:10` 原文如此；且 L0 的文件大小由 `write_buffer_size` 决定（与"用户数据量"不成比例），
用字节数会让"小写缓冲"把 L0 阈值实际抬高；L1+ 的文件是 compaction 输出（大小上界 `max_file_size`），
字节数才是它们的真实规模。**两条判据各自单一真相源**（不存在"两处算同一件事"）。

**`int max_bytes_for_level_multiplier = 10`（整数而非浮点）的理由**：I45 要求"计数维度必须一致、
重跑得到相同统计行"。浮点乘法在不同编译器/优化级别下的舍入可能造成 `MaxBytesForLevel` 的第 15 位差异，
进而改变 `score(l) >= 1` 的判定边界 ⇒ **可复现性被破坏**。整数乘法 + 显式饱和是确定性的。

### 3.6 `META` 的迁移路径（`A9` 的落地；`M4-C9` 要求"二选一写死"）

**写死的选择 = (iii)：MANIFEST + CURRENT 取代 `META`；首次 `Open` 兼容读取一次后立即改写并删除 `META`。**

```
Open 的版本元数据恢复优先级（**唯一真相源**，`VersionSet::Recover` 的函数体）：

① if (FileExists(CURRENT)):
       num := ParseCurrent(CURRENT)                     // 严格 ^[0-9]{1,20}\n$，否则 kCorruption
       MF  := ManifestFileName(dbname, num)
       if (!FileExists(MF)) ⇒ kCorruption("CURRENT 指向不存在的 MANIFEST")   // ★ 不自动修复
       回放 MF 的全部 record（§8.3/§8.4）
       manifest_bytes_ := MF 的字节数；manifest_number_ := num
       append_offset_ := 回放结束时的 last_good_end
② else if (FileExists(META)):
       **兼容读入**（逐字沿用 P §10.8/§10.9 的解析与校验；失败 ⇒ kCorruption，不自动修复）
       得 version_0
       **立即一次性迁移**（本进程内、`Open` 的单线程阶段、L12/L26 保证无后台线程）：
            n := next_file_number_（分配）
            写 MANIFEST-<n>.tmp（首条 record = version_0 的全量快照 edit）→ fsync → rename → SyncDir
            写 CURRENT.tmp（内容 "<n>\n"）→ fsync → rename(CURRENT.tmp, CURRENT) → SyncDir
            **删除** META 与 META.tmp（`Open` 单线程 ⇒ 可直接 unlink；失败只计数不阻断，
            但**必须**计数 meta_delete_failed，并保证**下一次 Open 不会再走②**（因为 CURRENT 已存在））
           计数 meta_migrated = 1
③ else:
       if (目录中存在 *.sst 或 MANIFEST-*) ⇒ kCorruption("元数据丢失但目录非空")   // 安全阀，D3:1753 同形
       else ⇒ 空库：空 Version，next_file_number = 1

**稳态：`Persist` 只写 MANIFEST + CURRENT，永不写 META。**
```

**"不允许稳态并存"的可检查判据（写进 `#1` 的评审检查项）**：
M4 落地后全仓 `grep 'META'` 的命中**只允许**出现在 (a) `filename.{h,cpp}` 的函数名/字符串、
(b) `version_set.cpp` 的②这条迁移分支、(c) `P` §10.8/§10.9 的引用注释、(d) 测试里**构造旧库**的辅助代码。
**禁止**出现在任何写路径（`Persist` / `LogAndApply` / flush / compaction）里。

**对 M3.3 的前置要求（`A9` 的影响面，必须进 `#1` 与 §13）**：
`M3-A39/A40/A41`（`D3:1886`/`D3:1888` 引用的三条用例）的断言必须写成"**以 `Recover`/`Persist` 的语义为准，
不绑定 `META` 这个文件名**"（例如断言"元数据缺失但目录非空 ⇒ 拒绝启动"，而不是断言
`env->FileExists(dbname + "/META")`）。⇒ 这样 M4 的迁移**不需要**改 M3 的任何测试（`M4:134` 的禁令不被触碰）。

---

## 4. `docs/protocol.md` 追加 §11 的 patch 文本（**由 `#1`/M4.1 落地，本阶段不改 `protocol.md`**）

> `M4:44` 要求"**追加** MANIFEST/VersionEdit 编码章节与层级文件命名约定，**不得修改**内部 key 编码章节
> 与 WAL record 格式章节"。当前 `P` 实测 377 行、末章为 `§10`（`P:206`）⇒ 追加章节号 = **§11**。
> 下面是从 `P` 末尾**纯追加**的完整 patch 文本（`#1` 或 M4.1 的提交直接照抄；形状照 `D3:777-783` 的先例）。
> **它与本文 §3.1~§3.3 必须逐字一致**（`M4:179`）。

````markdown
## 11. MANIFEST / VersionEdit 编码（M4 定稿）

> 追加章节。§1~§10 为 M1/M2/M3 冻结内容，本节不得反向修改它们。
> 本节与 §9/§10 共用同一条 CRC 纪律：**CRC 覆盖面必须包含长度字段**（§9.3 的"有意差异"在此复用）。
> **`META`（§10.8）自本节起为兼容读入格式**：M4 起稳态元数据是 `CURRENT` + `MANIFEST-<n>`；
> `META` 仅在"首次打开 M3 旧库"时被读一次，随后立即迁移并删除（§11.6）。§10.8/§10.9 的字节布局与
> 拒绝口径**不变**，只追加"M4 不再写它"这一条事实。

### 11.1 文件命名与文件号空间（追加）

```
<dbname>/CURRENT              指向当前 MANIFEST 的原子指针（ASCII 十进制编号 + '\n'）
<dbname>/CURRENT.tmp          CURRENT 的写临时文件（**永不**被当作 CURRENT 读）
<dbname>/MANIFEST-<n>         VersionEdit 追加日志（恢复的权威源）
<dbname>/MANIFEST-<n>.tmp     MANIFEST 的写临时文件（**永不注册**）
```

- `.log`、`.sst`、`MANIFEST-<n>` **共享**一个单调递增的 `next_file_number`（沿用 §10.1 的规则）。
  ⇒ `Open` 时权威值 = `max(MANIFEST 里的 next_file_number, max(目录中所有族的编号) + 1)`；
  **目录扫描必须包含 `MANIFEST-<n>` 的 `n`**，否则会重用 MANIFEST 编号、覆盖 CURRENT 指向的文件。
- `CURRENT` 的内容 = `^[0-9]{1,20}\n$`（纯十进制编号 + 恰好一个换行）。读侧**严格校验**，
  不符 ⇒ `kCorruption`。`CURRENT` **不**参与编号分配。
- 正则：`MANIFEST` 用 `^MANIFEST-[0-9]{6}$`；临时文件 `^MANIFEST-[0-9]{6}\.tmp$`。
  `ParseManifestFileName` **必须拒绝** `MANIFEST-<n>.tmp`（后缀匹配精确，禁止前缀匹配，同 §10.1）。

### 11.2 MANIFEST record 帧格式

```
manifest_on_disk := record*
record           := length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)
  length  = payload 字节数（不含 length/type/crc 自身）
  type    = 记录类型；0x01 = kManifestRecordTypeVersionEdit（M4 只定义此一个值）
  crc     = crc32c( length(4B LE) ‖ type(1B) ‖ payload )        // **含长度**
```

| 字段 | 字节数 | 字节序 | 取值 / 约束 |
|---|---|---|---|
| `length` | 4 | LE | `1 .. 67108864`（64 MiB 软上界）；越界 ⇒ `kCorruption` |
| `type` | 1 | — | `0x01`；其他值 ⇒ `kNotSupported`（不是 `kCorruption`） |
| `payload` | `length` | — | §11.3 的 VersionEdit 编码 |
| `crc32c` | 4 | LE | 覆盖 `length ‖ type ‖ payload` |

- record 之间**没有**填充与对齐，reader 是无状态的"顺序读 + 每条自定界"循环。
- **每条 record 的解码必须"全或无"**：先校验 `length`/`type`/`crc`，再整体解码到临时 `VersionEdit`，
  成功后才应用。禁止边解边应用（半个 edit 生效 = §9 的"半条 record 永不生效"在元数据侧的对应物）。

### 11.3 VersionEdit 字段表

```
version_edit_payload := field*
field                := tag(varint32) ‖ value(tag 依赖)

1 kComparator          : len_prefixed_string
2 kLogNumber           : varint64
3 kNextFileNumber      : varint64
4 kMinLogNumberToKeep  : varint64
5 kDeletedFile         : level(varint32) ‖ number(varint64)
6 kNewFile             : level(varint32) ‖ number(varint64) ‖ file_size(varint64)
                         ‖ max_sequence(varint64) ‖ smallest(len_prefixed) ‖ largest(len_prefixed)
```

- `level ∈ [0, kNumLevels)`，`kNumLevels = 7`（编译期常量，不入 `Options`）。
- `smallest` / `largest` 是 **internal key**（§6），`smallest <= largest`（按 §6.1 的比较器）。
- **不定义** `last_sequence` 字段：唯一合法的恢复水位口径是 §9 的
  `max(WAL 重放最大值, 各已注册文件的 max_sequence)`（§10.8 的 `max_sequence` 字段语义不变）。
- **不定义** `compact_pointer` 字段：M4 的选文件轮转指针只在内存（重启后从各层最左重新开始）。
- 同一 edit 内**允许** `kDeletedFile` 与 `kNewFile` 混合（flush 的 edit 只有 `kNewFile`；
  compaction 的 edit 两者都有）。

### 11.4 全量快照 edit

```
全量快照 edit := kComparator ‖ kLogNumber ‖ kNextFileNumber ‖ kMinLogNumberToKeep ‖ kNewFile*
```
恢复 = 从**空版本**开始按顺序应用所有 record。⇒ 不需要"这是快照"的标志位：
新建/重建 MANIFEST 的首条 record 就是"相对空版本的全量增量"。

### 11.5 层内布局不变式（安装期校验）

- **L0**：允许 key range 重叠；文件按 `number` **降序**（新→旧）。读路径逐个检查，命中即返回。
- **L1..L6**：层内按 `smallest` 的 **user key 升序**；相邻两文件必须 `largest.user_key <
  smallest.user_key`（**严格**，不得共享任何 user key）。违反 ⇒ **拒绝安装该 Version**。
- 安装期校验必须覆盖**全部**安装路径：恢复回放的收尾、每一次 `LogAndApply`、`META` 迁移。

### 11.6 `META` 的迁移（一次性）

`Open` 的优先级：`CURRENT` 存在 ⇒ 走 MANIFEST；否则 `META` 存在 ⇒ 兼容读入（§10.8/§10.9 逐字解码），
随后**立即**写 `MANIFEST-<n>`（首条 = 全量快照 edit）→ `fsync` → 写 `CURRENT.tmp` → `fsync` →
`rename(CURRENT.tmp, CURRENT)` → `SyncDir`，然后删除 `META` / `META.tmp`。
两者都不存在时：目录中若有 `*.sst` 或 `MANIFEST-*` ⇒ `kCorruption`（"元数据丢失但目录非空"），
否则按空库处理。**稳态只写 MANIFEST + CURRENT，永不写 `META`。**
````

**落地纪律（`M4:44`）**：`protocol.md` 的追加必须是**纯追加**（`docs/m3-evidence.md:19` 的先例是
"204 → 377 行，**只追加**，sha256 前缀证明"）⇒ M4.1 的提交必须给出 `wc -l` 与 `sha256` 前缀证据，
证明 `§1~§10` 的字节未被改动。

---

## 5. 接口与数据结构（签名草案）

> **前置事实（必须记住）**：`src/version_edit.*` / `src/version_set.*` / `src/merging_iterator.*` /
> `src/db_iter.*` **在 `e56d0b7` 上不存在**（§0.3 实测），它们是 **M3.2 的交付物（此刻正由另一个代理实现）**。
> 本节的签名以此为基线**凭空起草**；M4 开工第一步（§11 的 **M4.0**）必须逐一复核真实签名并登记差异。
> 标记约定：**【M3.2 承诺】** = 来自 `D3` 的已冻结设计草案；**【M4 新增】** = 本设计新增。

### 5.1 文件与类型清单

| 文件 | 性质 | 依据 |
|---|---|---|
| `src/version_edit.{h,cpp}` | **扩展**【M3.2 承诺】+ 【M4 新增】Level 与差分 | `M4:131`、`D3:1659-1671` |
| `src/version_set.{h,cpp}` | **扩展**【M3.2 承诺】：`Version` / `VersionSet` / `TableCache` | `M4:131`、`D3:1675-1702` |
| `src/compaction.{h,cpp}` | **全新**【M4 新增】 | `M4:34`、`M4:131`（"M4 唯一全新模块"） |
| `src/db_impl.{h,cpp}` | **追加**（白名单见 §2.2 A10） | `M4:34`、`M4:132` |
| `src/filename.{h,cpp}` | **追加** MANIFEST/CURRENT 命名 | `M4:132` |
| `src/common.h` | **追加** 6 个 `Options` 字段 + `CompactionHook` | §2.2 A11、§5.6 |
| `tests/version_test.cpp`、`tests/compaction_test.cpp` | **新增** | `M4:34`、`M4:142` |
| `scripts/lsm_compaction_stress.sh`、`scripts/lsm_level_stats.cpp` | **新增** | `M4:34`、§2.3 A14 |
| `docs/m4-design.md` / `m4-prerequisites.md` / `amplification.md` / `m4-evidence.md` | **新增** | `M4:50`、`M4:131` |
| `tests/test_harness.h` | **追加**（含新建 `FakeClock`） | `M4:141`、§0.6 第 3 条 |

`TableCache` **不新建文件**（§2.2 A4）：类定义在 `version_set.h`（`D3:1687`、`D3:1863` E6）。

### 5.2 `VersionEdit`（【M4 新增】的形态）

```cpp
// src/version_edit.h
namespace lsm {

struct FileMetaData {                       // 【M3.2 承诺】逐字沿用 D3:1649-1656
  uint64_t number = 0;
  uint64_t file_size = 0;
  SequenceNumber max_sequence = 0;          // 文件内最大 sequence（**不是** largest 的 sequence）
  std::string smallest;                     // internal key
  std::string largest;                      // internal key
  uint64_t smallest_user_key_size() const;
};

class VersionEdit {
 public:
  void Clear();                                              // 【M4 新增】"全或无"解码的前提
  void SetComparatorName(const std::string& n);
  void SetLogNumber(uint64_t n);
  void SetNextFileNumber(uint64_t n);
  void SetMinLogNumberToKeep(uint64_t n);
  void AddFile(int level, const FileMetaData& f);            // 【M4 扩展】原 M3 无 level 参数
  void DeleteFile(int level, uint64_t number);               // 【M4 新增】
  const std::vector<std::pair<int, FileMetaData>>& added_files() const;   // 【M4 新增】
  const std::vector<std::pair<int, uint64_t>>& deleted_files() const;     // 【M4 新增】
  std::string DebugString() const;                           // 失败时定位用
  bool EncodeTo(std::string* dst) const;                     // 编码；失败返回 false（不修改 *dst 之外的任何状态）
  bool DecodeFrom(const Slice& src, std::string* why);       // 失败返回 false + why（含 tag 与偏移）
 private:
  bool has_comparator_ = false, has_log_number_ = false, has_next_file_number_ = false,
       has_min_log_number_to_keep_ = false;
  std::string comparator_;
  uint64_t log_number_ = 0, next_file_number_ = 0, min_log_number_to_keep_ = 0;
  std::vector<std::pair<int, FileMetaData>> added_;
  std::vector<std::pair<int, uint64_t>> deleted_;
};

}  // namespace lsm
```
**取代登记**：`D3:1665-1666` 的 `AddFile(const FileMetaData&)` / `files()` **被上式取代**。
代价为零的论证：`src/version_edit.*` 实测不存在（§0.3）⇒ M3 侧没有任何调用方会因此编译失败；
且 `D3:506-508` 明确要求 M4"把『写全量快照』换成『写一条全量 `VersionEdit`』"。

**判据（`M4:179`）**：`EncodeTo` / `DecodeFrom` 的字节必须与 §4 的 §11 逐字一致；
A 组 `M4-A01~A04` 覆盖往返、可选字段缺省、空 edit、超长 key range、手工拼字节参照、非法输入拒绝。

### 5.3 `Version` / `VersionSet`（【M3.2 承诺】+【M4 新增】）

```cpp
// src/version_set.h
namespace lsm {

class Version {                              // **不可变**（D3:1678，I21/L15 不变）
 public:
  int num_levels() const { return kNumLevels; }                       // = 7
  const std::vector<FileMetaData>& level_files(int level) const;      // 【M4 新增】L0 降序；L1+ 按 smallest 升序
  const std::vector<FileMetaData>& files() const;                     // 【兼容别名】== level_files(0)
  const std::vector<FileMetaData>& AllFiles() const;                  // 【M4 新增】跨层全量（恢复/孤儿判定用）
  uint64_t total_bytes(int level) const;                              // 【M4 新增】score 的分子
  uint64_t log_number() const;  uint64_t min_log_number_to_keep() const;
  SequenceNumber MaxSequenceInFiles() const;                          // 全层 max（I31）
  // ---- 【M4 新增】引用计数（I42/L23）：refs_ 为 std::atomic<int>；live 集合由 VersionSet 维护（受 mutex_ 保护）
  void Ref();  void Unref();  int refs() const;
 private:
  friend class VersionSet;
  VersionSet* vset_ = nullptr;   // 只为 Unref 时回报（weak 语义：Unref 到 0 时通知 vset 摘除）
  ...
};

class TableCache { /* 【M3.2 承诺】D3:1687：文件号 → shared_ptr<const Table>，LRU，容量 Options::max_open_files */ };

class VersionSet {
 public:
  // 【M3.2 承诺的签名，函数体被 M4 替换】（D3:1694-1695）
  Status Recover(Env* env, const std::string& dbname, const Options& options,
                 Version** out, RecoveryStats* stats);
  // 【M4 新增】持久化 + 安装的唯一入口（替换 M3 的 Persist）
  //   edit 由调用方在锁内组装；函数内部按 §8.1 的模式 (a)/(b) 落盘，再安装 Version。
  Status LogAndApply(VersionEdit* edit, std::mutex* install_mu, std::mutex* db_mutex,
                     Version** out_new_version);
  // 【M4 新增】MANIFEST 追加句柄与阈值（受 install_mu_ 保护）
  WritableFile* manifest_file() const;  uint64_t manifest_bytes() const;
  const std::set<const Version*>& live_versions() const;              // 受 mutex_ 保护；供延迟删除判定
};

}  // namespace lsm
```
**`files()` 的语义（必须写清，否则会成为一个真实的误读点）**：`D3:511-512` 承诺"M4 把 `Version` 扩成
`level_files(int)` 时**不需要改** `files()` 的调用方"。在 M3 的形态下 `files()` = 全部文件（且全部是 L0）；
在 M4 的形态下 `files()` = **L0 的文件**。⇒ M3 的读路径调用方（`D3:1517` 的
`for f in ver->files()`）语义**自动**变成"遍历 L0"，正是 M4 读路径要的第一步（§7.1）。
`#1` 必须把这条写进"语义收窄登记"，并在 `Version::files()` 的注释里标 `// == level_files(0)`。

**`Recover` 的签名**：`D3:1704-1705` 要求它"不暴露 META 是文件还是日志"⇒ M4 **不改签名**，
只改函数体（§3.6）。

### 5.4 `Compaction`：选层、选文件、输入闭包、输出滚动

```cpp
// src/compaction.h
namespace lsm {

enum class PickStrategy { kRoundRobin = 0, kMinOverlap = 1 };   // §2.1 D2 / §2.4 E9

struct CompactionInputs {
  int level = 0;                                  // 上层层号；输出层 = level + 1
  std::vector<FileMetaData> inputs[2];            // inputs[0] = 上层集合；inputs[1] = 下层重叠集合
  std::string begin_user_key, end_user_key;       // 闭包后的 user key 区间（**闭区间**）
  bool is_trivial_move = false;                   // 【M4 可选，默认 false】见下"不做"的说明
};

class Compaction {
 public:
  // 选层打分（§3.5）；返回 -1 表示本轮无 compaction
  static int PickLevel(const Version& v, const Options& o);
  // 选文件 + 闭包展开（§6.3）；返回 false ⇒ 该层无可选输入（例如该层为空）
  static bool PickInputs(const Version& v, int level, PickStrategy s, const Options& o,
                         CompactionInputs* out, std::string* why);
  // 唯一真相源的丢弃判据（M4:178 要求单独成函数；真值表见 §5.5）
  static bool ShouldDrop(const InternalKey& ikey, SequenceNumber last_sequence_for_key,
                         SequenceNumber smallest_snapshot, bool base_level_for_key);
  // 执行（锁外）：构造输入迭代器 → 归并 → 滚动输出 → 生成 VersionEdit
  Status Run(Env* env, TableCache* tc, const CompactionInputs& in, const Options& o,
             VersionEdit* edit, CompactionStats* stats, std::string* why);
};

}  // namespace lsm
```

**`is_trivial_move` 的处置（明确写"不做"）**：LevelDB 有一条"trivial move"优化（当上层只有一个文件、
下层无重叠时，**直接 `rename` 到下层**而不重写字节）。它能把写放大降到 0，但会：
① 让"compaction 一定产出新文件"这一结构性假设失效（`M4:97` 的"输出文件先 durable 再注册"仍成立，
但"注册的新文件是新写的"不成立）；② 让 A 组的输出文件内容断言复杂化；③ `M4` 未要求。
⇒ **M4 不实现**，`is_trivial_move` 恒 `false`；登记为 M5 的候选（与 `E7`/`E8` 同批）。
**建议保留这个字段名但不上报**——不，按 `D3:462` 的纪律（"避免看起来能用但没实现的字段"）⇒
**字段直接不出现**。上式的 `is_trivial_move` **从最终签名中删除**，此处保留说明仅用于登记"不做"。

### 5.5 丢弃判据函数（**单一真相源**，`M4:178`）

```
// 前置（每条 entry 在被判定前由调用方维护）：
//   last_sequence_for_key = "同一个 user key 上，之前已经处理过的**更新**版本的 sequence"
//                           （user key 变化时置为 kMaxSequenceNumber）
//   smallest_snapshot     = 本次 compaction 开始时锁内取一次的快照边界（§2.1 D4）
//   base_level_for_key    = Compaction::IsBaseLevelForKey(user_key, level)（见下）

ShouldDrop(ikey, last_sequence_for_key, smallest_snapshot, base_level_for_key):
    drop_old_version := (last_sequence_for_key <= smallest_snapshot)                  // I41
    drop_tombstone   := (ikey.type == kTypeDeletion)
                     && (ikey.sequence <= smallest_snapshot)                          // I41 的可见性前提
                     && base_level_for_key                                            // I40
    return drop_old_version || drop_tombstone                                          // **析取**（A2）
```

**真值表（`#2` 必须逐行构造用例；`M4-A22~A27`）**：

| 场景 | `last_seq_for_key <= smallest_snapshot` | `type` | `seq <= smallest_snapshot` | `base_level_for_key` | 丢弃? | 依据 |
|---|---|---|---|---|---|---|
| 有更新的可见版本（旧版本） | 真 | 任意 | （无关） | （无关） | **丢** | I41 |
| 最新的可见版本，且是值 | 假 | `kTypeValue` | 真 | 任意 | **留** | 它是该快照下的答案 |
| 最新的可见版本，且是 tombstone，底层**无**旧值 | 假 | `kTypeDeletion` | 真 | **真** | **丢** | I40 |
| 最新的可见版本，且是 tombstone，底层**有**旧值 | 假 | `kTypeDeletion` | 真 | **假** | **留** | I40（**`M4-A22`**） |
| tombstone 但 `seq > smallest_snapshot`（快照看得见它的"更旧"状态） | 假 | `kTypeDeletion` | **假** | 任意 | **留** | I41 的可见性前提 |
| 旧版本，但 `last_seq_for_key > smallest_snapshot`（更新的版本对最小快照不可见） | 假 | 任意 | 任意 | 任意 | **留** | I41 |

**`IsBaseLevelForKey(user_key, level)` 的精确定义（A2 已论证）**：

```
对 l ∈ [level + 2, kNumLevels) 的每一层：
    该层的文件里，若存在 f 使得 f.smallest.user_key <= user_key <= f.largest.user_key ⇒ 返回 false
返回 true
```
- **为什么从 `level + 2` 起步**：`level + 1` 的全部重叠文件都是本次 compaction 的**输入**，
  它们的内容会被归并进输出 ⇒ 把它们算进"更底层"会让 `base_level_for_key` 恒假 ⇒ tombstone 永不丢弃。
- **"输入集之外"的隐含条件**：由于 `PickInputs` 已把 `level + 1` 的全部重叠文件纳入输入（§6.3），
  `l >= level + 2` 的文件都是"输入集之外"的层。
- **实现效率**：归并迭代器输出的 user key 是**升序**的 ⇒ 每一层维护一个**单调前进的游标**
  （LevelDB 的 `level_ptrs_`），摊还 O(1)。**禁止**每判定一次就二分（会把 O(1) 变成 O(log F) 且破坏确定性）。

**计数纪律（`D3:1811` 的"丢弃必须计数"）**：`drop_old_version` 与 `drop_tombstone` 命中时
**必须分别**累加 `CompactionStats::dropped_old_versions` / `dropped_tombstones`
（两条路径分开计数 ⇒ 可以直接验证 A2 的"析取"是否被误实现成合取：若被误写成合取，
`dropped_old_versions` 在非最底层会**恒为 0**，A 组用例一眼可见）。

### 5.6 `Options` 增量字段表（`A11` + `E9` + `E10`）

| 字段 | 类型 | 默认值 | 出处 | 校验（`Open` 第一步，非法 ⇒ `kInvalidArgument`，**不** fail-stop） |
|---|---|---|---|---|
| `level0_file_num_compaction_trigger` | `int` | `4` | `M4:10` | `< 1` ⇒ `kInvalidArgument`（0 会让 L0 永不触发） |
| `max_bytes_for_level_base` | `uint64_t` | `10u << 20`（10 MB） | `M4:10` | `== 0` ⇒ `kInvalidArgument` |
| `max_bytes_for_level_multiplier` | `int` | `10` | `M4:10` | `< 2` ⇒ `kInvalidArgument`（=1 会让各层容量相同、score 判据退化） |
| `max_file_size` | `uint64_t` | `2u << 20`（2 MB） | `M4:12` | `== 0` 或 `< block_size` ⇒ `kInvalidArgument`（< 一块 ⇒ 每个输出文件装不下一条 entry 的边界情形） |
| `compaction_pick_strategy` | `PickStrategy`（枚举，底层 `int`） | `PickStrategy::kRoundRobin` | `M4:68`、§2.4 E9 | 枚举值越界 ⇒ `kInvalidArgument` |
| `compaction_hook` | `CompactionHook*` | `nullptr` | §2.4 E10（形状照 `D3:1865` E8） | 无（与 `commit_hook`/`flush_hook` 同纪律：生产恒 `nullptr`） |

**M3 已承诺但尚未落地的 5 个字段**（`PR3:509`，M4 **复用**而不是重复定义）：
`block_size`(4096) / `verify_checksums`(true) / `max_open_files`(64) / `recycle_log_files`(true) / `flush_hook`(nullptr)。

**`CompactionHook`（【M4 新增】类型，`src/common.h`）**：

```cpp
class CompactionHook {
 public:
  virtual ~CompactionHook() = default;
  virtual void OnInputsSelected(int level, size_t num_inputs) {}   // 选层/选文件之后
  virtual void OnOutputWritten(uint64_t file_number) {}            // 输出文件 write+fsync 之后、rename 之前
  virtual void OnOutputRenamed(uint64_t file_number) {}            // rename + SyncDir 之后、注册之前
  virtual void OnBeforeInstall() {}                                // 进入 install_mu_ 之前
  // 与 CommitHook / FlushHook 同纪律：只做观察，**不含**任何生产逻辑（生产恒 nullptr）。
};
```
四个注入点**逐字对应** `M4:160` 要考的"compaction 中途 kill -9"的四个窗口。

### 5.7 统计与诊断入口（三个放大 + 层级；全部落在 `PersistentDBImpl`）

```cpp
// src/db_impl.h（【M4 新增】；沿用 D3:1799 GetFlushStats / D3:1637 GetReadStats 的先例）
struct CompactionStats {
  uint64_t started = 0, completed = 0, failed = 0, aborted = 0;
  uint64_t rounds_by_level[kNumLevels] = {};          // 每层发生过的 compaction 轮数
  uint64_t pick_round_robin = 0, pick_min_overlap = 0;
  uint64_t input_files = 0, output_files = 0;
  uint64_t bytes_read = 0, bytes_written = 0;         // 单轮与累计都从这里出（窗口由调用方界定）
  uint64_t dropped_old_versions = 0, dropped_tombstones = 0;  // **分开计数**（§5.5 的纪律）
  uint64_t install_rebase_retries = 0;                // L25 的"回锁校验"被触发几次
  uint64_t round_micros_p50 = 0, round_micros_max = 0; // 单轮耗时（微秒）；门禁用 p50（A12）
  uint64_t level_full_events[kNumLevels] = {};        // 各层 score>=1 的次数
  std::string last_error;
};

struct LevelStats {                                    // GetLevelStats()
  uint64_t files[kNumLevels] = {}, bytes[kNumLevels] = {};
  double score[kNumLevels] = {};
};

struct AmplificationStats {                            // GetAmplificationStats()
  uint64_t user_logical_bytes = 0, entry_bytes = 0;
  uint64_t flush_write_bytes = 0, compact_write_bytes = 0;         // 写放大的**分项分子**
  uint64_t files_checked = 0, get_count = 0;                       // 读放大的分子与分母
  uint64_t index_blocks_read = 0, data_blocks_read = 0, bytes_read = 0;
  uint64_t sst_bytes = 0, manifest_bytes = 0, current_bytes = 0, log_bytes = 0, tmp_bytes = 0;
  uint64_t live_versions = 0, live_versions_max = 0;               // M4-R5 的"存活 Version 数有界"探针
};

struct ManifestStats {                                 // GetManifestStats()
  uint64_t number = 0, bytes = 0, edits = 0, rolls = 0;
  uint64_t replay_edits = 0, replay_truncated_bytes = 0;           // 上次 Open 的回放统计
};
```
**只读纪律**：四个 `Get*Stats()` 都是"诊断、不改行为"（`D3:553` 同纪律）；**禁止**在 `mutex_` 内做聚合
（统计字段用 `std::atomic<uint64_t>`（relaxed）或在锁内累加到非原子字段——M4 选择**锁内累加**
（非原子）用于与版本状态强相关的计数（如 `live_versions`），**锁外累加**（原子）用于纯计数
（如 `bytes_read`）。**判据**：A 组 `M4-A36` 的 SpyEnv 断言"持 DB 锁期间 IO 调用数 == 0"。

---

## 6. compaction 执行路径（锁纪律的落点）

### 6.1 状态与锁（在 M3 的两把锁 + 后台线程上做最小扩展）

| 状态 | 保护者 | 来源 | 说明 |
|---|---|---|---|
| `memtable_` / `immutables_` / `log_number_` / `log_sealed_` / `next_file_number_` / `version_` / `last_sequence_` / `bg_error_` / `closed_` / `file_lock_` | `mutex_` | 【M3.2 承诺】`D3:1213-1221` | 逐条沿用 |
| `log_` / `durable_seq_` / `log_last_appended_seq_` / `queue_` / `flusher_active_` / `commit_cv_` | `commit_mu_` | 【M3.2 承诺】`D3:1222-1223` | 沿用 |
| `bg_thread_` / `bg_cv_` / `bg_started_` / `bg_stop_` / `flush_stats_` | `mutex_` + `bg_cv_` | 【M3.2 承诺】`D3:1224` | flush 线程，**不改** |
| **`compaction_thread_` / `compact_cv_` / `compaction_pending_` / `compaction_scheduled_` / `compaction_stats_`** | **`mutex_` + `compact_cv_`** | 【M4 新增】 | 新增的 compaction 线程与它的唤醒/停止 |
| **`install_mu_`** | 自持 | 【M4 新增】 | **安装串行化**：覆盖"写 MANIFEST + fsync + 安装 Version"。**不是** DB 互斥锁 |
| **`manifest_file_` / `manifest_number_` / `manifest_bytes_`** | `install_mu_` | 【M4 新增】 | MANIFEST 的追加句柄与规模（阈值判据） |
| **`pending_delete_`** | **`deletion_mu_`** | 【M4 新增】 | 延迟删除队列（`M4:101`） |
| **`snapshots_` / `smallest_snapshot_`** | `mutex_` | 【M4 新增】 | 快照集合与最小快照（`M4:70`；落点在具体类，A13） |
| **`live_versions_`（在 `VersionSet` 内）** | `mutex_` | 【M4 新增】 | 存活 Version 集合，供"引用归零才删"判定（I42/I43） |
| `TableCache` 自持的 `tc_mu_` | 自持 | 【M3.2 承诺】`D3:1635` | **不**在 DB 锁保护范围内；淘汰的 `close` 在锁外 |

### 6.2 两级后台工作：串行化与优先级（`M4:71`/`M4:112`；§2.1 D5 的落地）

```
flush 线程（M3 已有，形状不变，D3:1379-1385）：
    [mutex_] while (!bg_stop_ && immutables_.empty() && !rotate_needed_) bg_cv_.wait(lk)
             取 immutables_.front()（拷贝 shared_ptr，不弹出）
    [锁外]   FlushImmutable(imm)（D3:1297-1338 的 ①②③④⑤⑥ → ⑦安装 → ⑨回收）
             ★ ⑦ 的"安装"在 M4 改为：install_mu_ → 组装 edit → LogAndApply 的 (b) 模式 → 释放 install_mu_

compaction 线程（M4 新增）：
    [mutex_] while (!bg_stop_ && !compaction_pending_) compact_cv_.wait(lk)
             if (bg_stop_) break
             compaction_pending_ = false; compaction_scheduled_ = true
             ver := version_（shared_ptr<const Version> 拷贝）；snap := last_sequence_（一次）
             smallest_snapshot_ 的当前值（一次）
    [锁外]   ① PickLevel + PickInputs（纯内存，基于 ver，不碰 DB 锁）
             ② 若 level 为 -1 ⇒ 回循环
             ③ **让路检查**：[mutex_] while (!immutables_.empty() && bg_error_.ok() && !bg_stop_)
                                  compact_cv_.wait(lk)
                            // 固定优先级：flush 先（M4:112 的第二分支）
             ④ Compaction::Run(...)（读输入 → 归并 → 写输出；全在锁外）
             ⑤ InstallCompactionResults(...)（§6.5）
             ⑥ [mutex_] compaction_scheduled_ = false；回到循环
```
**触发点（`MaybeScheduleCompaction`）**：

| 触发位置 | 依据 | 说明 |
|---|---|---|
| `MakeRoomForWrite` 的层级分支（写入路径取批时） | `M4:34`、`M4:132` | 与 M3 的冻结判定同一处（`D3:1233`）；只做"置 `compaction_pending_` + `notify`"，**不做选层**（选层在后台线程、锁外） |
| flush 安装成功之后 | 【M4 新增】 | flush 产出新 L0 文件 ⇒ L0 的 score 可能越 1 |
| compaction 安装成功之后 | 【M4 新增】 | 安装可能让下层 score 越 1（级联） |
| `Open` 完成之后（首次） | 【M4 新增】 | 崩溃恢复后可能已经有超限的层 |

**`compaction_pending_` 的合并语义**：多次触发只置一个布尔位（不是计数器）⇒ 不会堆积。
**判据（`M4:112`"不得同时修改 VersionSet"）**：`install_mu_` 的唯一性 + `LogAndApply` 内部断言
"同一时刻只有一个线程在 MANIFEST 追加"（用 `install_mu_` 的持有事实保证，代码注释标 L25/L27）。

### 6.3 选层 → 选文件 → 输入闭包 → 构造迭代器

```
PickLevel(v, o):                                        // §3.5 的打分
    best := -1; best_score := 0.0
    for l in [0, kNumLevels):
        s := score(l)                                    // L0 用文件数，L1+ 用字节数
        if (s >= 1.0 && (best == -1 || s > best_score)) { best := l; best_score := s; }
        // 注意 "s > best_score" 是**严格**大于 ⇒ 平手时保留**先遇到的（层号更小）** ⇒ L0 优先
    return best

PickInputs(v, level, strategy, o, out):                 // 选文件 + 闭包
    ① 选**种子**：
        level == 0:
            kRoundRobin  ⇒ 取 L0 中**文件号最小**的文件（= 最旧）              // §2.1 D2 的 (a)
            kMinOverlap  ⇒ 取"与 level+1 重叠字节数最小"的 L0 文件（并列时取文件号最小者，保证确定性）
        level >= 1:
            kRoundRobin  ⇒ 取 compact_pointer_[level] 之后的**第一个**文件；指针越过末尾 ⇒ 回到第一个
            kMinOverlap  ⇒ 取"与 level+1 重叠字节数最小"的文件（并列时取 smallest 最小的那个）
    ② **闭包展开（E1/X1，硬要求）**：把种子放进 inputs[0] 后，反复扫描 level 层，
        若某文件与 inputs[0] 当前的 user key 并集区间**相交**（闭区间判定，端点相等算相交）
        ⇒ 加入 inputs[0] 并**扩大区间**，然后**重新扫描**（必须重扫，直到不再变化）
    ③ 区间 = [min smallest.user_key, max largest.user_key]（闭区间）
    ④ inputs[1] := level+1 层中**所有与区间相交**的文件（无重叠 ⇒ 可用二分定位首尾，
        也可线性扫；M4 选**线性扫 + 早停**：L1+ 不重叠 ⇒ 遇到 `smallest > end` 即可停）
    ⑤ 自检：inputs[0] 与 inputs[1] 均非空？inputs[0] 非空即可（inputs[1] 允许为空）
    ⑥ 断言：level+1 层中**不存在**"与区间相交但不在 inputs[1] 里"的文件（构造性保证）
```
**第 ② 步的反例（必须写进 `#1` 的风险清单；`M4-A16` 专测）**：见 §2.4 **E1**。
**第 ④ 步的"早停"判据**：L1+ 层内按 `smallest` 升序 ⇒ 一旦 `f.smallest.user_key > end` 即停止扫描
（后续文件的区间必然更大）。**这不是优化，是可判定性**：它与 `ValidateLevelLayout` 保证的层内互斥
（§3.4）是同一事实的两个用法。

**构造输入迭代器（`M4:81` 的"构造输入迭代器"）**：

```
children := []
for f in inputs[0] ∪ inputs[1]:                         // 顺序无关（MergingIterator 按 internal key 全序归并）
    t := table_cache->Get(f.number)                     // **必须**经 TableCache（A4）；拿 shared_ptr<const Table>
    if (t == nullptr) ⇒ return kCorruption("compaction 输入文件打不开")
    children.push_back(t->NewIterator())                // 该迭代器自持 shared_ptr<const Table>（D3:1627）
merged := new MergingIterator(&icmp, children.data(), children.size())
```
**句柄预算（A4 的强约束）**：`children` 的数量 = `inputs[0].size() + inputs[1].size()`。
`TableCache` 的容量是 `max_open_files`（默认 64）；L0 的输入上界是 `trigger + 2 = 6`（`D3:1400` 的在途 flush），
`inputs[1]` 的上界由 §3.5 的容量比（下层容量 / `max_file_size`）决定，**必须**在 `#1` 里给出
"最坏输入文件数 ≤ `max_open_files` 的一半"的推导 + §10.2 `M4-B05` 的 fd 计数断言兜底。
若某次 compaction 的输入数超过容量，`TableCache` 会正常 LRU 淘汰（正确但不划算）——**不得**绕开缓存。

### 6.4 归并、输出滚动、key 范围裁剪与丢弃（`M4:81` 的"输出文件滚动"）

```
Compaction::Run(env, tc, in, o, edit, stats, why):
  ① 构造 merged（§6.3）
  ② builder := nullptr; current_output_size := 0; outputs := []
     last_user_key := <空>; last_sequence_for_key := kMaxSequenceNumber
  ③ for (merged->SeekToFirst(); merged->Valid(); merged->Next()):
        ikey := ParseInternalKey(merged->key())                     // 失败 ⇒ kCorruption
        if (ikey.user_key != last_user_key) {                       // ← **user key 变化**
            last_user_key := ikey.user_key
            last_sequence_for_key := kMaxSequenceNumber
            // E2/X2：**只在这里**允许封块（切分必须落在 user key 边界）
            if (builder != nullptr && current_output_size >= o.max_file_size) ⇒ FinishOutput()
        }
        base := IsBaseLevelForKey(ikey.user_key, in.level)          // 单调游标，摊还 O(1)
        if (ShouldDrop(ikey, last_sequence_for_key, smallest_snapshot, base)) {
            ++(ikey.type == kTypeDeletion ? stats->dropped_tombstones : stats->dropped_old_versions);
            continue;                                               // 丢弃；**不**推进 builder
        }
        if (builder == nullptr) ⇒ StartOutput()                     // 分配 num、建 TableBuilder、记录 min_key
        builder->Add(merged->key(), merged->value())                // **只用 M3 的 TableBuilder**（禁止自写格式）
        current_output_size := builder->EstimatedFileSize()
        last_sequence_for_key := ikey.sequence                      // ← 必须在 Add **之后**更新
  ④ FinishOutput()（若 builder != nullptr）
  ⑤ 生成 edit：对 outputs 逐个 AddFile(in.level + 1, meta)；对 inputs[0] ∪ inputs[1] 逐个 DeleteFile
     ★ **顺序**：同一个 edit 里"先 DeleteFile 再 AddFile"还是反过来？
       ⇒ 由 `LogAndApply` 的**应用语义**决定：**先删除后新增**（否则若新文件与旧文件编号相同会误删）。
       编号由 `next_file_number_` 单调分配 ⇒ 不可能相同，但**语义上仍定死"先删后加"**（可判定 > 不可能）
  ⑥ 自检（`M4:97` 的 I39）：**所有** outputs 的文件都已在 ②/③ 里 `Sync()` 成功（§8.1 的模式 (b) 前置）
```
**`last_sequence_for_key` 的更新时机是易错点**（必须写进 `#1` 的风险清单）：
若在丢弃判定**之前**更新，则"更新的版本被丢弃后，更旧的版本会看到 `last_seq_for_key` = 被丢那个的 seq"
⇒ 仍 <= snapshot ⇒ 也被丢 ⇒ **同一次 compaction 把某个 user key 的全部版本丢光** ⇒ 读不到数据。
⇒ 定死：**只在 entry 被真正写入（或明确保留）之后更新**。

**key 范围裁剪的落地**：`MergingIterator` 只在 `inputs` 的文件上推进 ⇒ 区间外的 key **结构上不可能**
进入输出（不需要额外的裁剪代码）。`E7`（不做祖父层限制）⇒ **不**引入第三层判据。

### 6.5 安装：锁内准备 → 锁外写 → 回锁校验 → 安装 → 延迟删除（L25 的落地）

```
InstallCompactionResults(ver_snap /* 启动时的版本快照 */, in, outputs, removed_numbers, out_edit):
 ① [mutex_]  让路检查：若 !immutables_.empty() ⇒ 等待（§6.2 ③；固定优先级 flush 先）
             校验：in.inputs 的**每一个**文件号仍存在于 version_（= 当时的 current_）
                   否则 ⇒ aborted++、丢弃 outputs（转孤儿）、`compact_cv_.notify`、返回（重新调度）
             组装 edit（§6.4 ⑤）；从 version_ 构造 new_version（应用 edit）
             `ValidateLevelLayout(new_version)` ⇒ 失败 ⇒ kCorruption + fail-stop（M4:95）
             取 `removed := (旧 version 的文件) − (new_version 的文件)`
 ② [install_mu_ 获取；释放 mutex_]                            // ★ 顺序：install_mu_ 在 mutex_ **之外**取
             若 manifest_bytes_ > kManifestRollBytes（默认 4 MiB）⇒ 走模式 (a)（§8.1）
             否则走模式 (b)：向 manifest_file_ 追加 record + `Sync()`
             失败 ⇒ fail-stop（bg_error_）、outputs 转孤儿、failed++（§6.6）
 ③ [mutex_]  重做校验：inputs 的每个文件号仍存在（②期间 flush 可能装了新版本，但只加 L0）⇒ 不可能失败；
             防御性检查失败 ⇒ aborted++、**不安装**、fail-stop（"结构性不可能"发生了 = bug）
             安装：old := version_; version_ = new_version（Ref 新；old->Unref()）
             更新统计（compaction_stats_、level 计数）
 ④ [install_mu_ 释放]
 ⑤ [deletion_mu_]  removed 入 pending_delete_（**不持 mutex_** —— 见 §9.4 的锁序纪律）
 ⑥ [锁外]  MaybeDeleteObsoleteFiles()（§8.5）
 ⑦ [mutex_] compaction_scheduled_ = false；MaybeScheduleCompaction()（级联：下层 score 可能越 1）
```
**为什么 `install_mu_` 在 `mutex_` 之外取**：`LogAndApply` 需要在**不持 DB 锁**的情况下写 MANIFEST
（`M4:111`/L26）。若先取 `mutex_` 再取 `install_mu_`，则锁序变成 `mutex_ → install_mu_`，
而 flush 的安装路径必须用同一顺序才不死锁 ⇒ 会出现"持 DB 锁等 install_mu_（可能正被另一个线程的
fsync 持有）"⇒ **把 IO 的等待时间塞进了临界区**。⇒ 定死：**先取 `install_mu_`，再在需要时取 `mutex_`**，
并在 `install_mu_` 覆盖段的中间**释放 `mutex_`**（②）。这条必须写进 §9.4 的唯一全序表。

**"回锁校验"（L25 的"回锁后校验状态再安装"）的两处校验**：
- ① 的校验是**真正起作用**的那处（compaction 期间可能有 flush 装过版本）；
- ③ 的校验是**防御性**的（②期间没有任何路径会删除文件——flush 只加 L0、compaction 只有一个线程）。
**若 ① 的校验失败**：不装、转孤儿、重新调度。**计数 `install_rebase_retries`**（`M4-A33` 断言它 > 0
且结果仍满足 I37）。

### 6.6 失败矩阵与 fail-stop（`M4:81` 的"每一步失败的处理"）

| 失败点 | 处理 | 磁盘/内存状态 | 计数 |
|---|---|---|---|
| 选层/选文件（纯内存）失败 | 回循环；**不**置 `bg_error_` | 无变化 | — |
| 输入文件打不开（`Table::Open` 失败） | 放弃本次；**不**置 `bg_error_`（可用性优先：一次打不开不等于库损坏） | 无变化 | `aborted++`、`last_error` |
| 输出 `WritableFile` 创建/写/`Sync` 失败 | 删 `.tmp`（尽力而为）⇒ 放弃本次；**不**置 `bg_error_` | 无变化 | `aborted++` |
| 输出 `rename` 失败 | 删 `.tmp` ⇒ 放弃 | 无变化 | `aborted++` |
| 输出 `SyncDir` 失败 | **fail-stop**（目录项不持久 ⇒ 无法判定磁盘状态） | `bg_error_` 粘性 | `failed++` |
| MANIFEST 追加/`Sync` 失败 | **fail-stop** + outputs 转孤儿 | `bg_error_`；`version_` **不回滚**（未安装） | `failed++` |
| `ValidateLevelLayout` 失败 | **fail-stop**（`kCorruption`） | `bg_error_` | `failed++` |
| `install_mu_` 覆盖段结束后的 ③ 校验失败 | **fail-stop**（"结构性不可能"发生了） | `bg_error_` | `failed++`、`aborted++` |
| 延迟删除的 `unlink` 失败 | **不** fail-stop（垃圾留在盘上 ⇒ 只影响空间放大） | 无变化 | `orphan/obsolete_remove_failed++` |

**fail-stop 的一致性论证（`M4:36`："任何一步失败都必须返回错误且保持 CURRENT 指向旧的可恢复状态"）**：
- 模式 (b) 的 ② 之前**没有**改动过 CURRENT；② 失败 ⇒ 磁盘上的最后一个完整 record 仍是上一次安装 ⇒
  重启后回放出的版本 = 上一次安装的版本 ⇒ **与内存里的 `version_` 一致**（因为 ② 失败时没有安装）。
- 模式 (a) 的 CURRENT 只在第 ⑨ 步切（§8.1）⇒ ⑨ 之前的任何失败都让 CURRENT 仍指向**旧** MANIFEST
  （旧的、完整的、可回放的）⇒ 满足 `M4:36`。**旧 MANIFEST 只在 ⑨ 之后才允许删除**（⑪）。
- **禁止**：任何"注册失败后回滚内存 `version_`"的尝试（`D3:1327` 已给出理由：META/MANIFEST 没写成 ⇒
  重启后新文件是孤儿 ⇒ 数据仍在原处；回滚内存反而会制造"内存与磁盘不一致"）。

### 6.7 关闭顺序（L29 的落地；`M4:114`）

```
Close()（在 M3 的 D3:1387-1398 之上扩展，**顺序不可交换**）：
  ① [commit_mu_ → mutex_] closed_ = true                     // 拒绝新写（M2 的 L11）
  ② 等在途组提交批结算：commit_cv_.wait(commit_mu_, !flusher_active_ && queue_.empty())
  ③ [mutex_] bg_stop_ = true; bg_cv_.notify_all(); compact_cv_.notify_all()
  ④ join **两个**线程：compaction_thread_ 先 join，再 bg_thread_（flush）
     ★ 为什么 compaction 先：flush 可能正在等 install_mu_，而 compaction 可能正持有它 ⇒
       先 join compaction 能让 install_mu_ 立即释放，避免 flush 线程多等一次；
       反过来（先 join flush）也不会死锁，但会多等一个 compaction 的 MANIFEST fsync。
  ⑤ [commit_mu_] log_->Sync()（M2 的 I20）→ log_->Close()
  ⑥ [install_mu_] manifest_file_->Sync() → Close()           // 【M4 新增】MANIFEST 句柄的收尾
  ⑦ 等待所有 Version 引用归零：live_versions_.empty()（除 current_ 自身）
  ⑧ [deletion_mu_] 处理 pending_delete_ 队列（§8.5 的清理）
  ⑨ 释放 LOCK（幂等路径也要释放，M2 已有）
  ★ **不**强制把 pending 的 compaction 跑完（与 M3 的 `Close()` 不强制 flush 同纪律，D3:1394）：
    被放弃的 compaction 的输出是**孤儿**（未注册）⇒ 重启后由 §8.5 清理并计数。
    **必须计数**：compaction_stats_.aborted（"丢弃必须计数"，D3:1811）。
```

---

## 7. 读路径与快照语义

### 7.1 层次查找顺序（`M4:96` 的 I38 + I37 的读侧用法）

```
Get(user_key):
  [mutex_]  snap :=（调用方指定的快照 ? s->sequence : last_sequence_）
            mt := memtable_；imms := immutables_（整条 deque 的 shared_ptr 拷贝，新→旧）
            ver := version_（shared_ptr<const Version> 拷贝；**在锁内 Ref()**）
  [锁外]    lookup_key := BuildLookupKey(user_key, snap)
    ① mt->Get(lookup_key)                       → kFound / kDeleted(⇒ NotFound) / kNotFound
    ② for imm in imms（back→front，新→旧）        → 同 ①；命中即终局
    ③ for f in ver->level_files(0)（**文件号降序**，即新→旧）：
           if (user_key < f.smallest.user_key || user_key > f.largest.user_key) continue;   // 零 IO
           TableCache::Get(f, lookup_key)       → 命中即终局（含 kDeleted ⇒ NotFound）
           ★ **不得**遇到"第一个 key 在范围内"的文件就停（D3:1531 的 I23 纪律）
    ④ for l in [1, kNumLevels):
           定位"可能覆盖 user_key"的**至多一个**文件（层内按 smallest 升序 ⇒ 二分或前向游标）
           if 不存在 ⇒ 该层无贡献（**可立即进入下一层**）
           TableCache::Get(f, lookup_key)       → 命中即终局
    ⑤ Status::NotFound
```
**I37 如何让第 ④ 步变成"至多一个文件"**：层内 user key 区间严格互斥（§3.4）⇒ 一个 user key 至多落在
一个文件里 ⇒ 每层最多一次 `Table::Get`。**这是分层的全部读放大收益**（对照 `D3:2136` 的 M3 基线 F≈59）。

**`files_checked` 的计数归属（`docs/m3-evidence.md:49` §4 第 4 条的未闭合项，M4 必须闭合）**：
由**读路径**在"真的调用了 `TableCache::Get` 之前"递增一次，**且只在 DB 层递增**
（`Table::Get` **不得**自己递增，否则双重计数）。判定：`M4-A19` 断言
`read_files_checked == 实际 Table::Get 调用数`（用 SpyEnv 的块读计数做独立参照）。

### 7.2 快照集合与最小快照（`M4:70` 的落地）

```
状态（受 mutex_）：std::multiset<SequenceNumber> snapshots_;  SequenceNumber smallest_snapshot_;

GetSnapshot():
  [mutex_]  s := new Snapshot{last_sequence_}; snapshots_.insert(s->sequence);
            smallest_snapshot_ = *snapshots_.begin();     // 重算
            return s;                                      // 所有权归调用方

ReleaseSnapshot(s):
  [mutex_]  在 snapshots_ 中 erase 一个等于 s->sequence 的元素（**必须按值删一个，不是按 key 删全部**）；
            smallest_snapshot_ = snapshots_.empty() ? last_sequence_ : *snapshots_.begin();
            delete s;

无快照时的默认行为（M4:70 原文）：smallest_snapshot_ == last_sequence_ ⇒ 旧版本可立即进入可丢弃判定；
            tombstone **仍**受 base_level_for_key 约束（I40）。
```
**`smallest_snapshot_` 是唯一真相源**：它在 (a) `GetSnapshot` / (b) `ReleaseSnapshot` / (c) 每一次
`last_sequence_` 前进（写入提交）时更新。**判据**：`M4-A29` 断言"释放最后一个快照后
`smallest_snapshot_ == last_sequence_`"，`M4-A30` 断言"无快照时旧版本被丢、tombstone 不因无快照而被丢"。

### 7.3 `DBIter` 的快照语义与生命周期

- `DBIter` 的 `snapshot` 由构造参数传入（`D3:1594` 已经如此）；M4 只是把它的**来源**从
  `last_sequence_` 扩展为"可选快照的 sequence"（§7.2）。
- **生命周期（L23 的落地）**：`DBIter` **必须**持住它访问的每一个 `Table`/`Version`/`MemTable`
  的引用（`D3:1625-1629` 已冻结）；M4 追加一条：**构造时对 `Version` 调 `Ref()`，析构时调 `Unref()`**
  （`M4:108` 要求"读路径在拿到 Version 时 Ref、在 DBIter 析构/查询结束时 Unref"）。
  `Get` 路径同理：在锁内 `Ref()`，返回前 `Unref()`（**包括所有提前返回的分支**—— `#1` 必须登记
  "早返回路径的 Unref" 为评审检查项，`M4-A34` 覆盖）。
- **与 compaction 的竞态**：读持住 `Version` ⇒ 该版本引用的文件不会被删（I42）⇒ 迭代器在 compaction
  并发下**始终**读到一个一致快照（`M4:192` 的评审项 5）。判据：`M4-A28`（A 组，ASan 下）+ TSan 全量。

### 7.4 读路径缓存与读放大的测量口径

- **table cache**：沿用 `D3:1633-1635`（键 = 文件号，值 = `shared_ptr<const Table>`，容量
  `Options::max_open_files`，LRU，淘汰的 `close` 在 `mutex_` 之外）。
- **无块缓存**（§2.1 D8 的裁决）。
- **读放大口径**：沿用 `D3:553` 的 `ReadStats` 字段定义（`files_checked` / `key_range_skipped` /
  `index_blocks_read` / `data_blocks_read` / `bytes_read` / `crc_checked` / `crc_failed` / `hit_layer`），
  **`hit_layer` 的取值域由 M4 扩展**：`{memtable, immutable, l0, l1, ..., l6, none}`
  （M3 只有 `{memtable, immutable, sstable}`；扩值域是**追加**，不破坏 M3 的既有断言）。
- **新增一条判据（I45 的可复现性）**：`bytes_read` 的口径**不变**（含块头/CRC/restart 数组的全部字节），
  `files_checked` 的口径**不变**（只数真的进了 `Table::Get` 的文件）⇒ M4 的读放大数字与 `D3:2136`
  的 M3 推算基线**可直接对比**（`M4-B06` 的对照列）。

---

## 8. 持久化、启动恢复与孤儿清理

### 8.1 持久化时序图（两模式；`M4:80`/`M4:190`；A3 的落地）

**模式 (a) 新建/重建**（触发：首次 `Open`、`CURRENT` 缺失、`META` 迁移、`manifest_bytes_ > kManifestRollBytes`）：

```
 ① [mutex_ 内]  n := next_file_number_（分配；只递增内存计数器）
 ② [锁外]       Env::NewWritableFile(MANIFEST-<n>.tmp)              失败 ⇒ fail-stop
 ③ [锁外]       写首条 record = **全量快照 edit**（当前 version_ 的全部文件 + comparator_name
                + log_number + next_file_number + min_log_number_to_keep）
 ④ [锁外]       file->Sync()                  ★ fsync（内容 durable）  失败 ⇒ 删 tmp ⇒ fail-stop
 ⑤ [锁外]       file->Close() → Env::RenameFile(tmp, MANIFEST-<n>)    失败 ⇒ 删 tmp ⇒ fail-stop
 ⑥ [锁外]       Env::SyncDir(dbname_)         ★ 目录项 durable            失败 ⇒ fail-stop
 ⑦ [锁外]       写 CURRENT.tmp：内容 "<n>\n" → Sync() → Close()          失败 ⇒ fail-stop
 ⑧ [锁外]       Env::RenameFile(CURRENT.tmp, CURRENT)  ★ **原子切换点**   失败 ⇒ fail-stop
 ⑨ [锁外]       Env::SyncDir(dbname_)         ★ rename 后必须 SyncDir（D3:2235 R4 的加强纪律）
 ⑩ [锁外]       manifest_file_ := 以 NewAppendableFile(MANIFEST-<n>) 重新打开（追加点 = EOF）
 ⑪ [deletion_mu_] 旧 MANIFEST-<old> 入 pending_delete_（**不**在锁内 unlink）
 ⑫ [锁外]       MaybeDeleteObsoleteFiles()（旧 MANIFEST 真正删除）
```
**顺序的不可交换性（逐条给反例）**：

| 若交换 | 后果 |
|---|---|
| ④ 与 ⑤ 互换（先 rename 后 fsync） | `MANIFEST-<n>` 可能存在但内容不全 ⇒ 若 ⑧ 已完成，CURRENT 指向一个**半写**的 MANIFEST ⇒ 恢复时尾部截断可能把**本该有的** record 判成残骸 |
| ⑥ 与 ⑧ 互换（先切 CURRENT 再 SyncDir） | `CURRENT` 的**目录项**不持久 ⇒ 掉电后 CURRENT 可能不存在或指向旧值，而 `MANIFEST-<n>` 的目录项已持久 ⇒ 最坏情况是"回退到旧版本"（若旧 MANIFEST 已删 ⇒ **无法恢复**）⇒ 这正是 `D3:2231-2236`（R4 的 `SyncDir` 措辞纪律）与 `D3:2127-2131`（§12.5 第 1 条）登记的缺口，**必须**由 ⑥/⑨ 两次 `SyncDir` 覆盖 |
| ⑪ 在 ⑨ 之前（先删旧 MANIFEST） | CURRENT 可能还指向旧 MANIFEST（⑧ 失败/未执行）⇒ 删掉唯一可恢复点。**`M4:36` 明文禁止** |

**模式 (b) 常规追加**（触发：每一次 flush / compaction 安装，且未越过重建阈值）：

```
 ① [install_mu_ → mutex_]  组装 edit；构造 new_version；ValidateLevelLayout；取 removed 集合
 ② [释放 mutex_，**仍持 install_mu_**] 写 record 到 manifest_file_（追加）→ file->Sync()
                                        manifest_bytes_ += record_bytes；manifest_edits_++
                                        失败 ⇒ fail-stop（磁盘上留下半条 record ⇒ 恢复时尾部截断）
 ③ [mutex_]  安装 version_; Ref 新 / Unref 旧；更新统计
 ④ [释放 install_mu_]
 ⑤ [deletion_mu_] removed 入 pending_delete_；[锁外] MaybeDeleteObsoleteFiles()
```
**`M4:36` 的字面顺序理解为"模式 (a) 的收尾 + 模式 (b) 在同一新文件上继续追加"**（A3）。
**CURRENT 只在模式 (a) 切换** ⇒ 热路径上**没有** `rename` 与目录 fsync（这是 A3 的收益）。

### 8.2 每一步失败的处理矩阵

见 §6.6（compaction 路径的失败矩阵）与下表（持久化专属）：

| 步骤 | 失败 ⇒ 行为 | CURRENT 的状态 | 可恢复性 |
|---|---|---|---|
| (a)② ~ (a)⑦ | 删 tmp（尽力而为）⇒ fail-stop | **仍指向旧 MANIFEST** | 完全可恢复（旧 MANIFEST 未被删，因为 (a)⑪ 未执行） |
| (a)⑧ rename 失败 | fail-stop | **仍指向旧 MANIFEST**（rename 是原子的：要么旧、要么新） | 完全可恢复 |
| (a)⑨ SyncDir 失败 | fail-stop | **可能是新值也可能回退到旧值**（目录项未 fsync）⇒ 只能"两个都试"？**不**：契约是"读 CURRENT 的当前内容" ⇒ 若读到旧编号，旧 MANIFEST 必须仍在盘上 ⇒ **禁止**在 (a)⑫ 之前删旧 MANIFEST | 可恢复 |
| (a)⑩ ~ (a)⑫ | fail-stop | 已指向新 MANIFEST | 可恢复（新 MANIFEST 完整） |
| (b)② | fail-stop；**不安装** | 不变 | 完全可恢复（磁盘上的最后一条完整 record 与内存 `version_` 一致） |
| (b)③ | fail-stop | 已追加成功但未安装 ⇒ 重启后会回放出**多一条** edit | **可恢复但语义上"多"了一次安装** ⇒ 这没关系（`LogAndApply` 是幂等的：重放该 edit 得到的 Version 与内存里的 `new_version` 相同）。**登记**：这是"先落盘后安装"的固有性质，`M4-A41` 覆盖 |
| 延迟删除的 unlink | 只计数 | 不变 | 只影响空间放大 |

### 8.3 启动恢复算法（`M4:72` 的"完整时序图"对应物）

```
Open 的版本元数据部分（**替换** D3:1744-1761 的 ⑤ 与 ⑥）：

 ⑤ VerInfo := VersionSet::Recover(...)          // 过滤在 §3.6 的 ①②③
    if (走① MANIFEST 回放):
        逐 record（顺序、自定界、全或无）：
            s := ReadRecord(&offset, &type, &payload, &why)
            if (读不出完整 record 或 crc 失败):
                若 offset 之后**还有**一个可解析的完整 record ⇒ kCorruption("中间损坏")
                否则 ⇒ **截断到 last_good_end**（只允许在末尾），记 manifest_tail_truncated_bytes
                        ★ **不修改文件**（M4 只读恢复；截断由下一次模式 (b) 的追加**覆盖式**自然发生？
                          —— 不：追加会写在残骸**之后**。⇒ **必须**在恢复期把文件截断到 last_good_end
                          （Env::Truncate，形状照 D3:1766 的"最高编号 log 允许截断"），并计数。
                          否则残骸会把新追加的 record 与它拼成一条不可能的 record ⇒ 下次恢复失败。）
            if (type != 0x01) ⇒ kNotSupported
            解码到临时 VersionEdit；失败 ⇒ kCorruption（含 tag 与偏移）
            应用到**版本视图**（先 DeleteFile 后 AddFile 的语义）
            每应用一条 ⇒ edits_replayed++
        语义校验（逐条，见 §3.3 的表）：
            level 越界 / number 重复 / DeleteFile 的目标不存在 / comparator 不符 / internal key 非法
            ⇒ kCorruption 或 kInvalidArgument（**不自动修复**）
        收尾：ValidateLevelLayout ⇒ 失败 ⇒ kCorruption
        manifest_bytes_ := 文件（截断后）的字节数；manifest_file_ := NewAppendableFile（追加点在末尾）
        ★ **`next_file_number_` 的权威值必须把 MANIFEST 编号计入**（§3.1 的规则）
    if (走② META 迁移): 见 §3.6（一次性改写 + 删除 META，计数 meta_migrated）
    if (走③ 空库): 空 Version
 ⑥ 孤儿清理（§8.5；**只清理可证明未被引用的**，逐类计数上报；失败只计数不阻断）
 ⑦ 重放 WAL：集合 = 步骤 ⑥ 之后目录里**实际存在**的 `*.log`，按编号**数值升序**（D3:1762-1771 逐字沿用）
 ⑧ last_sequence_ := max(max_replayed_seq, version->MaxSequenceInFiles())        // I31 不变
 ⑨ 当前 log：沿用 M3 的规则（D3:1773-1775）
 ⑩ 重放出来的数据留在 memtable_；其 log_number = min(被重放的 log 编号)（D3:1777，I34 的必要项）
 ⑪ smallest_snapshot_ := last_sequence_（无快照）
 ⑫ StartBackgroundThread()（**单数**：M3 的 flush 线程）→ 再启动 compaction 线程
    ★ 两个线程都在 `RecoverAndOpen` 成功返回**之前**启动（L12/L26 的形态：恢复期无后台线程）
 ⑬ 触发一次 `MaybeScheduleCompaction()`（恢复后可能已有超限层）
```

### 8.4 MANIFEST 损坏/截断分支（`M4:72` 要求"MANIFEST 损坏/截断的恢复策略"）

| 形态 | 判定 | 行为 | 计数 |
|---|---|---|---|
| `CURRENT` 不存在，`META` 也不存在，但有 `*.sst`/`MANIFEST-*` | `P` §10.9 规则 2 的同形安全阀 | `kCorruption`（**不自动修复**） | — |
| `CURRENT` 内容非法（非 `^[0-9]{1,20}\n$`） | 严格校验 | `kCorruption` | — |
| `CURRENT` 指向的 `MANIFEST-<n>` 不存在 | — | `kCorruption` | — |
| MANIFEST **尾部**半条 record / CRC 失败（其后无完整 record） | 沿 `P` §10.9 规则 4 的形状 | **截断到 `last_good_end`**（`Env::Truncate`）+ 继续 | `manifest_tail_truncated_bytes` |
| MANIFEST **中间**损坏（失败之后还有完整 record） | 同上规则的另一半 | `kCorruption` | — |
| record `type` 未知 | — | `kNotSupported` | `unknown_manifest_record_types` |
| record `length` 越界（0 或 > 64 MiB） | — | `kCorruption` | — |
| 语义校验失败（level 越界 / DeleteFile 目标不存在 / 层内重叠） | §3.3/§3.4 | `kCorruption` | — |
| comparator 不符 | 逐字沿用 `D3:1748` | `kInvalidArgument` | — |
| 回放条目数异常增长（软阈值） | 【M4 新增，可选】 | 只 **WARN + 计数**（`D3:2134` 的先例：`#1` 建议对 META 变胖加 WARN） | `manifest_replay_warn` |

### 8.5 孤儿文件判据与清理（`M4:126`/`M4:195`；`NOTE:224` M4-R7 的落地）

```
Open 步骤 ⑥ 的分类清理（**逐类计数**；失败只计数不阻断，D3:1756-1761 同纪律）：

 a) *.sst.tmp                              ⇒ 删除     // 构造上永不注册（D3:1757 逐字沿用）
 b) 编号 ∉ 回放出的 version 的 *.sst         ⇒ 删除     // **M4 新增来源**：compaction 输出在
                                                        ②(write+fsync)之后、③(注册)之前崩溃
 c) MANIFEST-<n>.tmp                       ⇒ 删除     // 【M4 新增】模式 (a) 的中断残片
 d) 编号 < min_log_number_to_keep 的 *.log  ⇒ 删除     // M3 的 I34（D3:1759），不变
 e) MANIFEST-<n> 且 n != CURRENT 指向的编号  ⇒ 删除     // 【M4 新增】被切换掉的旧 MANIFEST
                                                        （延迟删除队列在崩溃前未清空）
 f) META / META.tmp                        ⇒ 由 §3.6 的迁移分支处理（**不是**孤儿）
    ★ 但若 CURRENT 存在且 META 也在 ⇒ 判为"迁移的残留" ⇒ 删除 + 计数（不得静默留着）
 g) CURRENT.tmp                            ⇒ 删除     // 【M4 新增】模式 (a) 的中断残片
    ★ **例外**：若 CURRENT 不存在而 CURRENT.tmp 存在 ⇒ **不删**，走 kCorruption（安全阀，§8.4 第 1 行）

★ 读路径**必须容忍**孤儿存在（`M4:126` 的硬约束）：即使清理失败，读路径也**只**走 version_ 的文件
  ⇒ 孤儿既不会被读入，也不会让 Open 失败。清理只是"顺手把垃圾收掉"（D3:1760 逐字沿用）。

运行期清理（**延迟删除队列**，I43/L24）：
  MaybeDeleteObsoleteFiles()：
    [deletion_mu_]                                   // ← 先取队列锁（§9.4 的锁序）
      candidate := pending_delete_ 的全量快照（**不清空**）
      [mutex_]  live := ∪ { v->AllFiles().number : v ∈ live_versions_ } ∪ { current_->AllFiles() }
               deletable := candidate \ live
      [释放 mutex_]
      从 pending_delete_ 移除 deletable
    [释放 deletion_mu_]
    [锁外]  逐个 Env::DeleteFile + 计数（成功/失败/字节）
  ★ **绝不在持 mutex_ 时取 deletion_mu_**（M4:109 的锁序；§9.4 给唯一全序）
```
**"引用归零才删"的判据（I42）**：`live` 集合来自 `VersionSet::live_versions_`（受 `mutex_` 保护），
它的成员由 `Version::Ref()/Unref()` 维护（§5.3）。**为什么这是充分的**：读者只从 `current_` 拿版本
（在 `mutex_` 内 `Ref()`），而 `current_` 恒在 `live` 里；任何**更旧**的版本只有在仍被某个读者持有时
才留在 `live` 里 ⇒ `deletable` 里的文件号不被任何**当前可被读到**的版本引用。**判据**：`M4-A34`
（A 组：持版本引用期间调 `MaybeDeleteObsoleteFiles()`，断言文件仍在；释放后断言被删）。

### 8.6 `RecoveryStats` / `CompactionStats` 的增量字段（"不得静默"的落点）

`RecoveryStats`（M2 的字段**只增不改**；M3 的增量见 `D3:1789-1797`）：

| 字段 | 来源 | 新增? |
|---|---|---|
| `manifest_present` / `manifest_number` / `manifest_bytes` | §8.3 ⑤ | **是** |
| `edits_replayed` / `manifest_records_skipped` | §8.3 ⑤ | **是** |
| `manifest_tail_truncated_bytes` / `manifest_truncation_note` | §8.4 | **是** |
| `unknown_manifest_record_types` | §8.4 | **是** |
| `level_files[kNumLevels]` / `level_bytes[kNumLevels]` | §8.3 ⑤ 收尾 | **是** |
| `meta_migrated` / `meta_delete_failed` | §3.6 ② | **是** |
| `manifest_orphan_removed` / `manifest_tmp_removed` / `current_tmp_removed` | §8.5 c/e/g | **是** |
| `compaction_orphan_sst_removed` | §8.5 b（M4 的新孤儿来源） | **是** |
| （M2/M3 的全部既有字段） | — | 否（语义不变） |

`CompactionStats` / `LevelStats` / `AmplificationStats` / `ManifestStats` 见 §5.7。

**纪律（`D3:1811-1812` 的延续）**：任何一处**丢弃/跳过/截断/删除**都必须先在上述表里加计数。
`#1` 把它做成硬约束：**新增任何丢弃路径而不加计数 = 评审阻断项**。
M4 的新增丢弃路径共 5 条，逐条给计数落点：

| 丢弃路径 | 计数 |
|---|---|
| 丢旧版本 | `CompactionStats::dropped_old_versions` |
| 丢 tombstone | `CompactionStats::dropped_tombstones` |
| 放弃一次 compaction（输入打不开/写失败/校验失败） | `CompactionStats::aborted` |
| 清理孤儿 `.sst`（compaction 未注册输出） | `RecoveryStats::compaction_orphan_sst_removed` |
| 截断 MANIFEST 尾部残骸 | `RecoveryStats::manifest_tail_truncated_bytes` |

### 8.7 本节涉及的补充决策索引

本节依赖的指令外决策**全部**在 §2.4 给出（不重复）：`E1`（L0 传递重叠闭包，§6.3 ②）、
`E2`（输出按 user key 边界切分，§6.4 ③）、`E3`（MANIFEST 帧格式，§3.2）、`E4`（无 `kLastSequence`，§3.3）、
`E5`（compact pointer 不持久化，§5.4/§6.3）、`E6`（`max_file_size` 只约束 compaction 输出，§3.5）、
`E7`（不做祖父层限制，§6.4）、`E8`（不做 seek 热度，§2.1 D2）、`E9`（策略开关落 `Options`，§5.6）、
`E10`（`CompactionHook`，§5.6）。

---

## 9. 不变量与锁纪律的增量（**I35~I46 / L22~L29**）

> **改号依据**：§2.2 **A1**（= `M4-C1`）。M4 的**落地号** = M3 既有 `I21~I34 / L13~L21` 之后**追加**；
> 指令原号 `I31~I42 / L19~L26` **全部顺延 +4 / +3**。
> `#1` 写 `docs/m4-prerequisites.md` 时逐行抄"谁保证 / 怎么验"两列即可；`#4` 按"落点"列的章节号找到代码位置。

### 9.1 指令原号 ↔ 落地号 ↔ 出处（**完整映射表，一列都不许丢**）

| 指令原号 | **落地号** | 指令出处 | 一句话定义 | 与 M3 既有 I/L 的关系 |
|---|---|---|---|---|
| I31 | **I35** | `M4:93` | 版本号严格单调递增；`VersionSet` 分配的新版本号不得回退或复用 | **新增**（M3 无版本号概念：单快照 `META` 没有版本图） |
| I32 | **I36** | `M4:94` | `CURRENT` 始终指向某个完整持久化过的 MANIFEST；任何时刻读到的 CURRENT 目标都可被成功回放 | **取代** M3 的 L17（`D3:1912`："本设计不引入 CURRENT…等价纪律落到 `META` 的 rename"）——L17 的**实质**由 I36 + L25 + L28 承接 |
| I33 | **I37** | `M4:95` | L1 及以上层内 key 范围不重叠（按 user key 区间互斥）；违反即**拒绝安装该 Version** | **新增**；**顺带解除** `D3:2102`/`D3:2112` 的"M3 不得出现 level 概念"禁令（§1.3.3）。**本设计的收紧**：`M4:95` 的"允许端点相等"被收紧为"不得共享任何 user key"（§3.4、§15 R5） |
| I34 | **I38** | `M4:96` | L0 内文件 key 范围允许重叠 ⇒ 读路径必须按文件号**从新到旧**逐个检查，首个命中即返回 | **承接并收紧** M3 的 I23（`D3:1882`）：I23 在 L0 这一层**逐字继续成立**，M4 追加"L1+ 每层至多一个文件"的读侧用法 |
| I35 | **I39** | `M4:97` | compaction 输出文件必须先 durable（`fsync` 完成）再注册进版本；未落盘的文件号不得出现在任何 Version 中 | **泛化** M3 的 I22（`D3:1881`）：I22 是 flush 专属（`durable → rename → SyncDir → 注册`），I39 覆盖**任何**产出新 SSTable 的路径（flush 与 compaction 同判据） |
| I36 | **I40** | `M4:98` | tombstone 只在"该层及更底层不存在更旧版本"时才可丢弃；只要更底层可能还有同 user key 的旧值，tombstone 必须原样保留 | **取代** M3 的 I26（`D3:1885`）：I26 的前提是"M3 不删任何旧版本"⇒ M4 首次允许条件删除，I26 的"不得误丢"以**更严**的形式落在 I40 |
| I37 | **I41** | `M4:99` | 旧版本只在低于最小快照 sequence 时才可丢弃；存在快照时必须为该快照保留可见版本 | **新增**；它是 M3 的 I28（`D3:1887`：迭代器不得返回不可见版本）的**前提加强** |
| I38 | **I42** | `M4:100` | 被任何读（含进行中的迭代器与快照句柄）引用的文件不得删除，删除前必须确认引用计数归零 | **首次让 M3 的 L16 真的生效**（`D3:1911`：M3 不删已注册 `.sst` ⇒ L16 当时无适用对象）；M4 起 L16 有对象，判据由 I42/I43 承担 |
| I39 | **I43** | `M4:101` | 文件删除必须经延迟队列（记录待删文件号 + 触发时机），禁止在安装 Version 的临界区内同步删除 | **新增**（`M4:101`）；与 L24 的锁序配套 |
| I40 | **I44** | `M4:102` | compaction 不得改变任何 key 的可见结果：对任意给定快照，合并前后 `Get` 与 `DBIter` 的结果逐字节一致 | **新增**；它是 M3 的读路径用例组（`D3:1887-1888`，`M3-A27~A33`）在 M4 下的**保持条** |
| I41 | **I45** | `M4:103` | 三个放大的统计口径固定且可复现：同一份脚本、同一份数据、重跑得到相同统计行（允许时间维度波动，计数维度必须一致） | **新增**；M3 的 `ReadStats`（`D3:553`、`D3:2114`：M3 只给原始计数，"三个放大率的聚合与报告**那属 M4**"）是它的分子来源 |
| I42 | **I46** | `M4:104` | 恢复后的版本必须保证所有已 ack 数据仍然可见（与 M2 的 I11/I12 衔接），不得因 compaction 元数据丢失而漏读 | **收紧** M3 的 I31（`D3:1890`，恢复水位口径，**号不变**）与 I27（`D3:1886`，恢复 = 最后一次成功注册的状态）：M4 的"已注册文件集合"来源从 `META` 变成"CURRENT → MANIFEST 回放" |
| L19 | **L22** | `M4:107` | 后台 compaction 线程与前台写/读的交互：compaction 只持 Version 引用与必要的短临界区，不得长时间持有 DB mutex；前台写只在其需要安装新 Version 或触发调度时短暂加锁 | **扩展** M3 的 L13（`D3:1908`：后台 flush 与前台）到**两类**后台工作；M4 追加"compaction 的 IO 全程在锁外" |
| L20 | **L23** | `M4:108` | Version 引用计数的获取与释放点必须成对且明确：读路径在拿到 Version 时 `Ref`、在 DBIter 析构/查询结束时 `Unref`；compaction 安装新 Version 时对旧 Version `Unref` | **收紧** M3 的 L15（`D3:1910`：版本用 `shared_ptr` 持有、禁止原地修改）与 L19/L21（`D3:1914`/`D3:1916`）：M3 靠 `shared_ptr` 保内存安全，M4 需要**显式的 `Ref/Unref`** 才能判定"文件是否可删"（I42） |
| L21 | **L24** | `M4:109` | 延迟删除队列由独立互斥量保护；锁序固定为"延迟删除队列锁 → DB 锁"，禁止反向获取；队列出队与实际 `unlink` 不得持有 DB 锁 | **新增**；与 M3 的 L8（`docs/m2-design.md:108`："锁序固定 `commit_mu_ → mutex_`"）**并列**⇒ 必须给出唯一全序（§9.4） |
| L22 | **L25** | `M4:110` | DB mutex 与 MANIFEST 写的顺序：先在锁内准备 VersionEdit 与目标文件号，锁外写 MANIFEST/fsync，回锁后校验状态再安装 Version；禁止锁内 fsync | **收紧** M3 的 L18（`D3:1913`：禁止持 DB 锁做 IO）的具体形态；`D3:1913` 的"M2 的 `commit_mu_` 例外"在 M4 **不扩大**——`install_mu_` 是新引入的**非 DB 锁**，它的持锁期做 IO 不违反 L18 也不违反 L26 |
| L23 | **L26** | `M4:111` | 禁止持 DB 锁做 IO 的延续：SSTable 读写、MANIFEST 追加、目录 fsync、rename、unlink 全部在锁外 | **沿用并明确列举** M3 的 L18 的 M4 形态（新增 `MANIFEST 追加` 与 `unlink` 两项） |
| L24 | **L27** | `M4:112` | compaction 与 flush 的互斥关系：同一时刻只允许一个后台 compaction；flush 与 compaction 不得同时修改 VersionSet；**按设计文档的固定优先级串行化** | **取代** M3 的 L20（`D3:1915`：后台 flush 线程**只读** `log_number_`、永不取 `commit_mu_`）的**独占安装者**假设：M3 只有一个后台写者，M4 有两个 ⇒ 由 `install_mu_` + 固定优先级（flush 先）重建"同一时刻一个安装者"（§6.2） |
| L25 | **L28** | `M4:113` | CURRENT 切换与读路径的可见性依赖 `rename` 的原子性与目录 fsync；读路径打开文件必须能容忍"刚被切换掉的旧 MANIFEST 文件已被删除"之外的一切中间态 | **取代** M3 的 L17 的"不引入 CURRENT"（`D3:1912`），并把它的**加强版**（`D3:1913` 的"rename 之后必须 `SyncDir`"）保留为模式 (a) 的 ⑥/⑨ 两步 |
| L26 | **L29** | `M4:114` | shutdown 顺序：置停止标志 → 唤醒并等待后台 compaction 线程退出 → 等待所有 Version 引用归零 → 处理延迟删除队列 → 释放 VersionSet 与表缓存；关闭后不得再有线程访问已释放对象 | **扩展** M3 的 L11（`docs/m2-design.md:111`：拒绝新写 → 等在途批 → 再销毁）与 `D3:1387-1398` 的 Close 顺序到**两个**后台线程 + 延迟删除队列 |

**M5 的顺延（本文件只登记，不改 M5 的任何文件）**：`M5:96` 的"沿用 L1~L26，新增 L27~L32"
⇒ 落地为"沿用 `L1~L29`，新增 **L30~L35**"；`M5:84` 的"维持…M4 的 I31~I42"⇒ 落地为"维持 M4 的
**I35~I46**"；M5 的新增不变量为 **I47~I56**。⇒ **本设计的补充约束（§9.5 的 X1~X8）刻意不占用 I/L 号空间**，
正是为了让 M5 的号段保持可用。

### 9.2 不变量 I35~I46（谁保证 + 怎么验）

| # | 谁保证（落点） | 怎么验（用例） |
|---|---|---|
| **I35** | `VersionSet::LogAndApply` 的版本号分配（`src/version_set.cpp`）；`Open` 时 `next_version_number_ := 已回放的最大版本号 + 1`（**只从 MANIFEST 推**，不从内存推） | `M4-A40`（切换/重建后版本号仍单调）、`M4-A41`（重启后 `next_version_number_` 不回退）；**§4 §11.3 的协议里没有版本号字段** ⇒ 版本号必须**只**由"回放的 edit 条数/顺序"决定（`#1` 必须把这条写成不变量检查：`current_version_number_ == 已回放的安装型 edit 数`） |
| **I36** | §3.1 的 `CURRENT` 命名与严格内容校验（`ParseCurrent`）+ §8.1 模式 (a) 的 ⑦⑧⑨（`CURRENT.tmp` → `fsync` → `rename` → `SyncDir`）+ §8.5 g 的例外分支 | `M4-A08`（rename 前/后注入 ⇒ 恢复后 CURRENT 指向完整 MANIFEST）、`M4-A09`（CURRENT 缺失 + 目录非空 ⇒ 拒绝）、`M4-A10`（CURRENT 内容非严格形态 ⇒ `kCorruption`） |
| **I37** | `ValidateLevelLayout`（`src/version_set.{h,cpp}`）+ 输出侧只在 user key 边界切分（§6.4 ③、`E2`）+ 输入侧闭包（§6.3 ②、`E1`） | `M4-A17`（构造非法布局 ⇒ 拒绝安装；含"同一 user key 跨两文件"与"区间部分覆盖"两种构造）、`M4-A32`（输出滚动落在 user key 边界）、`M4-A33`（并发 flush 下的 rebase 仍满足 I37） |
| **I38** | §7.1 的顺序规则 ③（L0 按**文件号降序**、命中即终局、**不得**因 key 范围命中就停止遍历）+ `D3:1530` 的"不用 key range/max_sequence 判新旧" | `M4-A18`（构造重叠的多个 L0 文件，断言读到的值来自最新文件；含"tombstone 在旧文件、值在新文件"的两向子断言）、`M4-A19`（`files_checked` 的上界与 `hit_layer` 取值） |
| **I39** | §6.4 ⑥（输出的 `write+fsync` 在 `rename` 之前）+ §8.1 的模式 (b) ②（MANIFEST 追加在安装之前）+ §6.6 的失败矩阵 | `M4-A34`（SpyEnv 断言"注册发生时，输出的 `fsync` 与 `rename` 都已完成"——**一条用例三个子断言**，形状照 `D3:1881` 的 `M3-A22`）、`M4-B02`（进程级 kill -9 注入） |
| **I40** | §5.5 的 `ShouldDrop` 的 `drop_tombstone` 分支 + `IsBaseLevelForKey`（`l >= level + 2`、单调游标） | `M4-A22`（底层还有旧值 ⇒ tombstone **保留**）、`M4-A23`（最底层无重叠 ⇒ tombstone **可丢**）、`M4-A26`（**析取 vs 合取**的构造性对照）、`dropped_tombstones` 计数 > 0 |
| **I41** | §5.5 的 `drop_old_version`（`last_sequence_for_key <= smallest_snapshot`）+ §7.2 的 `smallest_snapshot_`（唯一真相源）+ §6.4 ③ 的 `last_sequence_for_key` **更新时机** | `M4-A24`（有快照且快照看得见旧版本 ⇒ **不丢**）、`M4-A25`（无快照 ⇒ 可丢）、`M4-A28`（快照内可见版本不被丢且读到正确值）、`M4-A29`（释放快照后 `smallest_snapshot_` 正确更新）、`dropped_old_versions` 计数 > 0 |
| **I42** | §5.3 的 `Version::Ref/Unref` + §5.3 的 `VersionSet::live_versions_` + §8.5 的 `MaybeDeleteObsoleteFiles` 的 `deletable := candidate \ live` | `M4-A34`（引用未归零 ⇒ 文件仍在；归零后 ⇒ 被删）、`M4-A28`（ASan 下迭代 + compaction）、`M4-B05`（fd 计数不增长） |
| **I43** | §6.5 ⑤（`removed` 在锁**外**入队）+ §8.5 的队列实现 + §6.7 ⑧ | `M4-A35`（断言"安装临界区内**没有**发生 `unlink`"，用 SpyEnv 的事件序列判定）、`M4-A38`（关闭时队列被处理） |
| **I44** | §5.5（判据）+ §6.4（归并顺序与输出）+ §7.1（读路径的层序）+ §7.3（迭代器持引用） | `M4-A20`（`std::map` 全量对账，含"同 user key 多 version、同 sequence 多 type"）、`M4-A21`（合并后仍满足 `MergingIterator` 的 internal key 全序契约）、`M4-A28` |
| **I45** | §10.3 的口径定义表 + §5.7 的统计字段 + §3.5 的**整数**容量乘（避免浮点舍入破坏可复现性） | `M4-A31`（手工构造已知输入，断言三个放大的分子/分母与输出行**逐字段一致**，含"合并自身的写是否计入"）、`M4-B03`（两种策略的固定列表）、`M4-B06`（读放大固定行） |
| **I46** | §8.3 的恢复算法（⑤ MANIFEST 回放 + ⑧ 水位口径）+ §3.6 的迁移分支 + `D3:1707-1733` 的 I31（**号不变**） | `M4-A41`（回放后所有已 ack 数据可见）、`M4-A39`（`META` 迁移后数据仍可见）、`M4-B01`/`M4-B02`（`missing 0`） |

### 9.3 锁纪律 L22~L29（谁保证 + 怎么验）

| # | 纪律 | 落地方式 | 验证 |
|---|---|---|---|
| **L22** | 后台 compaction 与前台读写的交互：compaction 只持 `shared_ptr<const Version>` 与短临界区；前台写只在"安装新 Version 或触发调度"时短暂加锁 | §6.1 的状态/锁表 + §6.2 的线程循环（③ 的让路检查是**唯一**的长等待点，且它在 `compact_cv_` 上睡而不是忙等）+ §6.5（② 在 `mutex_` 之外） | `M4-A36`（SpyEnv：持 `mutex_` 期间 IO 调用数 == 0；覆盖 compaction 全路径）、`M4-A37`（饥饿探针）、`M4-B07`（P99 门禁，A12） |
| **L23** | `Version` 的 `Ref/Unref` 成对且明确：读路径拿到即 `Ref`、`DBIter` 析构/查询结束即 `Unref`；安装新版本时对旧版本 `Unref` | §5.3 的 `Ref/Unref`（`refs_` 为 `std::atomic<int>`）+ §7.3（含**所有早返回路径**的 `Unref`）+ §6.5 ③（安装时 `old->Unref()`） | `M4-A34`（引用归零判据）、`M4-A28`（ASan 下并发）、`M4-B08`（`live_versions_max` 有界）、代码评审逐处核对（`#1` 登记为检查项） |
| **L24** | 延迟删除队列由 `deletion_mu_` 保护；锁序 `deletion_mu_ → mutex_`；出队与 `unlink` 不持 DB 锁 | §8.5 的 `MaybeDeleteObsoleteFiles`（**显式** `[deletion_mu_] → [mutex_]` 的嵌套）+ §9.4 的唯一全序 + §6.5 ⑤（入队**不持** `mutex_`） | `M4-A35`（事件序列里 `unlink` 不在安装临界区内）、TSan 全量、代码评审按 §9.4 逐处核对 |
| **L25** | 锁内准备 edit + 目标文件号 ⇒ 锁外写 MANIFEST/`fsync` ⇒ 回锁校验再安装；**禁止锁内 fsync** | §6.5 的 ①②③（② 在 `install_mu_` 下、**不持** `mutex_`）+ §6.1 的 `install_mu_` 行 + §9.4 的"先取 `install_mu_` 再取 `mutex_`" | `M4-A36`（SpyEnv）、`M4-A33`（rebase 校验被触发且结果满足 I37）、`M4-A42`（失败路径不回滚、CURRENT 仍可恢复） |
| **L26** | SSTable 读写、MANIFEST 追加、目录 `fsync`、`rename`、`unlink` 全部在锁外 | §6.3（`TableCache::Get` 在锁外）、§6.4（`TableBuilder` 在锁外）、§8.1（两模式的 IO 步全标"[锁外]"）、§8.5（`unlink` 在锁外） | `M4-A36`、`M4-A35`；**M2 的 `Locks.ZeroIoWhileHoldingDbMutex` 必须继续通过**（`D3:1913` 明文"不得回退"） |
| **L27** | 同一时刻至多一个 compaction；flush 与 compaction 不同时修改 VersionSet；**按固定优先级串行化（flush 先）** | §6.2（`install_mu_` 唯一性 + ③ 的让路检查）+ §6.5（`install_mu_` 覆盖"写 MANIFEST + 安装"整段） | `M4-A37`（`immutables_` 非空时 compaction 让路）、`M4-A33`（并发下仍满足 I37）、TSan 全量、代码评审核对"两个安装路径都取 `install_mu_`" |
| **L28** | `CURRENT` 切换的可见性依赖 `rename` 原子性 + 目录 `fsync`；读路径容忍"旧 MANIFEST 已被删除"之外的中间态 | §8.1 模式 (a) 的 ⑦（`CURRENT.tmp` 写 + `fsync`）⑧（`rename`）⑨（`SyncDir`）⑪⑫（旧 MANIFEST 只在 ⑨ 之后入队/删除）+ §8.5 g（`CURRENT` 缺失但 `CURRENT.tmp` 存在 ⇒ **不删**，判 `kCorruption`） | `M4-A08`（rename 前/后注入）、`M4-A09`、`M4-A10`；**措辞纪律**：只断言"调用了 `SyncDir` 且顺序位于 `rename` 之后"，**禁止**写"掉电安全已证明"（`D3:2235` R4） |
| **L29** | 关闭顺序：停止标志 → join compaction 线程 → join flush 线程 → 等引用归零 → 处理延迟删除队列 → 释放 `VersionSet`/表缓存；关闭后无线程访问已释放对象 | §6.7 的 ①~⑨（含"compaction 先 join"的理由）+ §6.7 末条（不强制跑完 pending compaction，但**必须计数**） | `M4-A38`（关闭路径：无 UAF、队列被处理、`aborted` 有计数）、ASan/TSan 全量 |

### 9.4 全局锁序（**唯一全序**；三条既有/新增锁序规则的综合）

```
唯一全序（只允许从左向右获取；任何反向获取都是 bug）：

        install_mu_   →   commit_mu_   →   deletion_mu_   →   mutex_

  禁止：
    - mutex_  → install_mu_        （⇒ 会在临界区内等 IO，违反 L26 的精神）
    - mutex_  → deletion_mu_       （⇒ 违反 L24 的字面锁序 "deletion_mu_ → mutex_"）
    - mutex_  → commit_mu_         （⇒ 违反 M2 的 L8 "commit_mu_ → mutex_"）
    - deletion_mu_ → commit_mu_    （无任何调用点需要它；禁止以防未来误用）

  TableCache 的 tc_mu_：**完全独立**，不得在持上述任一锁时获取；
                        TableCache::Get 会做 IO ⇒ 只能在 L26 允许的"锁外"调用（§6.3/§7.1）。

  bg_cv_ / compact_cv_ / commit_cv_：条件变量本身不参与全序；它们的 wait 必须释放对应互斥量（标准语义）。
```

| 既有/新增规则 | 原文出处 | 在全序中的位置 |
|---|---|---|
| `commit_mu_ → mutex_` | M2 的 L8（`docs/m2-design.md:108`） | 保留（M2/M3 的既有调用点不变） |
| `deletion_mu_ → mutex_` | M4 的 L24（`M4:109`） | 保留 |
| 「先取 `install_mu_`，再取 `mutex_`」 | 本设计（§6.5；`M4:110`/L25 的落地需要） | 新增，且必须在最左端 |
| 「`removed` 集合在锁内算出、**锁外**入队」 | 本设计（§6.5 ⑤ + §8.5） | **拆段纪律**：需要"先 DB 锁后队列锁"的场景一律拆成两段，而不是反向获取 |
| 「`bg_cv_`/`compact_cv_` 的 wait 不得持 `install_mu_`」 | 本设计（§6.2 ③ 在 `mutex_` 上 wait） | 防止"持安装锁睡觉" |

**为什么需要这张表**：M3 只有两条规则（`commit_mu_ → mutex_` + 后台线程只取 `mutex_`，`D3:1226-1229`）；
M4 引入 `install_mu_` 与 `deletion_mu_` ⇒ 若不给出全序，"`install_mu_` 与 `deletion_mu_` 谁在外"
这两种写法都能编译通过，但只有一种与 L24/L25 同时相容。**`#4` 评审按本表逐处核对调用点。**

### 9.5 补充约束 X1~X8（**不占 I/L 号空间**；`#4` 逐条核对）

> 这些约束承载"正确性"而非"工程取舍"，但**不能**占用 I 号（I47~I56 留给 M5，§9.1 末段）。
> 它们以"一个函数 / 一处断言"的形式落地，且都有 A 组用例。

| # | 约束 | 落点 | 用例 |
|---|---|---|---|
| **X1** | **L0 输入集必须是"按 key range 重叠的传递闭包"**（新加入的文件若扩大区间则重扫，直到不再变化） | §6.3 ② | `M4-A16`（桥接文件反例） |
| **X2** | **compaction 输出文件的切分必须落在 user key 变化处**（禁止同一 user key 的多个版本跨两个输出文件） | §6.4 ③ | `M4-A32` |
| **X3** | **`last_sequence_for_key` 只在 entry 被写入/保留之后更新**（否则会把一个 user key 的全部版本丢光） | §6.4 ③ | `M4-A27`（构造"三层版本 + 无快照"输入，断言至少保留最新版本） |
| **X4** | **每条 MANIFEST record 的解码必须"全或无"**（先校验 `length`/`type`/`crc`，再整体解码到临时 `VersionEdit`） | §3.2 约束 4 | `M4-A03`（中间损坏 / CRC 坏 / 截断三类） |
| **X5** | **`next_file_number_` 的权威值必须把 `MANIFEST-<n>` 的 `n` 计入**（否则重用编号 ⇒ 覆盖 CURRENT 指向的文件） | §3.1、§8.3 ⑤ | `M4-A40`（重建模式后编号仍严格大于所有族的最大编号） |
| **X6** | **`IsBaseLevelForKey` 从 `level + 2` 起步**（把 `level + 1` 排除，因为它全在输入集里） | §5.5 | `M4-A23`（若误从 `level + 1` 起步 ⇒ tombstone 永不丢弃 ⇒ 该用例的 `dropped_tombstones > 0` 断言失败） |
| **X7** | **同一个 edit 内的应用语义 = "先 DeleteFile 后 AddFile"**（顺序定死，即使编号不可能冲突） | §6.4 ⑤ | `M4-A20` 的子断言（构造一个"同层删一个加一个"的 edit，断言最终文件集合正确） |
| **X8** | **禁止对正被任何 Version 引用的文件号调用删除**（I42 的可判定形式：删除判定必须查 `live_versions_`，不得只看 `current_`） | §8.5 | `M4-A34`（持**旧**版本的引用 ⇒ 文件仍不可删——这条专测"只看 current_"的错误实现） |

---

## 10. 测试矩阵

> A 组 = 确定性（`MemEnv` + 注入 `Env` + **新建的 `FakeClock`**，无真实磁盘、无真实时间、无网络，零 flaky）。
> B 组 = 真实磁盘 / 进程级（脚本驱动）。
> 每条给：**依赖假设 / 通过判据 / 需要的 seam**；**来源列**区分"[指令]"（来自 `M4:147-163` 的必含清单）
> 与"[新增+理由]"（本设计补齐的，逐条给理由）。编号**按落地号**引用 I/L。

### 10.1 A 组（确定性）

| 编号 | 用例名（GTest） | 依赖假设 | 通过判据 | 需要的 seam | 来源 |
|---|---|---|---|---|---|
| M4-A01 | `VersionEdit.RoundTripEmptyAndFull` | 无 | 空 edit / 全字段 edit / 超长 key range（`kMaxUserKeySize` 与 1 B 两种）往返后**逐字段相等**；编码字节数与手工计算一致 | 无 | [指令] `M4:148` |
| M4-A02 | `VersionEdit.OptionalFieldsAbsent` | 无 | 只含 `kNewFile` 的增量 edit / 只含 `kDeletedFile` 的 edit 都能编码并解码；缺省字段的 `has_*` 为 false，且**不**被写成默认值 | 无 | [指令] `M4:148` |
| M4-A03 | `VersionEdit.MalformedRejected`（X4） | 无（ASan 下跑） | 截断（尾部半条）、CRC 损坏、`length == 0`、`length > 64 MiB`、未知 `type`（⇒ `kNotSupported`）、`level` 越界、`number` 重复、`smallest > largest`、非 internal key 的 `smallest` ⇒ 全部**拒绝**且 `why` 含 tag 与偏移；**ASan 无越界读** | 无 | [指令] `M4:148` + [新增+理由：`M4:148` 只说"损坏"，未要求"`kNotSupported` vs `kCorruption` 的区分"；沿用 `D3:2030` 的 `M3-A08` 纪律] |
| M4-A04 | `Manifest.RecordBytesMatchHandBuilt` | 无 | 对同一个 edit，`EncodeTo` 的输出与**手工拼字节**的期望值逐字节相等（CRC 也手工算） | `tests/test_harness.h` 的手工拼装辅助（**不复用** `coding.h`，沿用 `test_harness.h:12-14` 的纪律） | [新增+理由：防"实现与测试同错"——`M4:179` 要求编解码与文档一致，若测试也用 `EncodeTo` 算期望值，两边同错不会被发现；`D3:2146` 的 `M3-A38` 已有同形教训] |
| M4-A05 | `Manifest.ReplayFullSnapshot` | 无 | 一条全量快照 edit + N 条增量 edit 回放出的版本 = 期望的文件集合（逐层逐文件号）；`edits_replayed == N+1` | 无 | [指令] `M4:148` |
| M4-A06 | `Manifest.TailTornTruncatedAndCounted` | `MemEnv` 的 `Truncate` | 尾部半条 record ⇒ 截断到 `last_good_end`、`manifest_tail_truncated_bytes > 0`、回放出的版本 = 最后一条完整 record 的状态；**截断后追加一条新 record 再回放仍正确**（证明截断真的落了盘） | `MemEnv`（`Truncate`） | [指令] `M4:148` + [新增+理由：`M4:148` 只说"截断的恢复策略"，未要求"截断后追加仍正确"；这是 `D3` 的 `M3-A50` 同源教训（per-log 边界）在元数据侧的形态] |
| M4-A07 | `Manifest.MiddleCorruptionRejected` | 无 | 中间 record 的 CRC 损坏（其后还有完整 record）⇒ `kCorruption`；**不**截断、**不**静默跳过 | 无 | [指令] `M4:148` |
| M4-A08 | `Current.AtomicSwitchOnInjectedCrash` | `MemEnv` + `CompactionHook`/`FlushHook` 的注入点 | 在 `CURRENT.tmp` 的 rename **之前**/之后注入 ⇒ 恢复后 `CURRENT` 指向一个**完整**的 MANIFEST（前者指旧、后者指新），两种情况都能成功打开且数据不丢 | `MemEnv` + 注入点 | [指令] `M4:149` |
| M4-A09 | `Current.MissingWithNonEmptyDirIsCorruption` | 无 | `CURRENT` 缺失 且 目录中有 `*.sst` 或 `MANIFEST-*` ⇒ `kCorruption`；空目录 ⇒ 正常空库 | `MemEnv` | [新增+理由：`M4:84` 只要求"MANIFEST 损坏/截断分支"，未覆盖"`CURRENT` 整体缺失"；它是 `P:364-377` §10.9 规则 2 安全阀的直接迁移，缺了它"元数据丢失"会变成静默空库] |
| M4-A10 | `Current.ContentStrictlyValidated` | 无 | `CURRENT` 内容 = 非数字 / 超 20 位 / 缺换行 / 多换行 / 带后缀 ⇒ 全部 `kCorruption`；合法内容 ⇒ 成功 | `MemEnv`（手工写 `CURRENT`） | [新增+理由：`M4:121` 把"CURRENT 切换非原子"列为风险，但"内容非法"是**另一个**独立的失效模式（半写文件），`M4` 未要求用例] |
| M4-A11 | `Level0.FileCountThresholdTriggers` | 无 | 恰好 `trigger - 1` 个 L0 文件 ⇒ 不触发；恰好 `trigger` 个 ⇒ 触发一次 L0→L1；`PickLevel` 返回 0 | 层级文件集合构造器 | [指令] `M4:150`（`M4:136` 明写"L0 满阈值恰好等于 4"是必须列出的边界） |
| M4-A12 | `LevelN.CapacityThresholdTriggers` | 无 | 逐层构造字节数恰好 `< MaxBytesForLevel(l)` / 恰好 `==` / 略超 ⇒ 触发点正确；**L6 超限不再向下**（计数 +1，不产生 L7） | 同上 | [指令] `M4:150` |
| M4-A13 | `PickLevel.MaxScoreAndTieBreak` | 无 | L0 与 L2 同时 `score >= 1` ⇒ 选 `score` 大者；`score` 相等 ⇒ 选**层号小者**；无 `score >= 1` ⇒ 返回 -1 | 无 | [新增+理由：`M4:150` 只说"选层逻辑"，而 `M4-C6`/A6 的裁决把"多层同时超限选哪层"定为核心决策 ⇒ 必须有确定性用例；否则 `M4:191` 的评审项 4（"L0 阈值与每层容量判据是否与设计一致"）无判据] |
| M4-A14 | `PickFile.RoundRobinDeterministic` | 无 | `kRoundRobin`：L0 取**文件号最小**者作种子；L1+ 从 compact pointer 之后取第一个；连续两轮的输出序列**完全确定**（同一输入 ⇒ 同一序列） | 无 | [指令] `M4:150`（"pick 文件策略的确定性输出"） |
| M4-A15 | `PickFile.MinOverlapDeterministic` | 无 | `kMinOverlap`：重叠字节最小时取该文件；并列时取文件号最小（L0）/ `smallest` 最小（L1+）⇒ 并列也有**唯一**答案 | 无 | [指令] `M4:150` + [新增+理由：并列时的 tie-break 是"确定性输出"的必要条件，`M4` 未规定 ⇒ 本设计定死并测它] |
| M4-A16 | `L0Inputs.TransitiveOverlapClosure`（**X1**） | 无 | 构造 §2.4 E1 的桥接反例（`A[a..b]`/`B[c..d]`/`C[b..c]`）⇒ 断言 `inputs[0]` 含**全部三个**文件；且断言"闭包后 L0 中不存在与输入区间相交的残留文件" | 层级构造器 + `PickInputs` 可直调 | [新增+理由：这是分层的**正确性前提**（A16 的反例会读出陈旧值 ⇒ 破坏 I44）；`M4:150` 完全没提，`NOTE` 的 C1~C11 也没覆盖 ⇒ 本设计的**最重要新增用例**] |
| M4-A17 | `LevelLayout.OverlapRejectedOnInstall`（I37） | 无 | 构造三种非法布局：(a) 同一 user key 跨两个文件（端点相等）；(b) 区间部分覆盖；(c) 顺序错乱 ⇒ `ValidateLevelLayout` 全部返回 false 且 `why` 定位到层号+两个文件号+冲突 key；**安装被拒绝**（`version_` 不变） | 直调 `ValidateLevelLayout` + 一次安装尝试 | [指令] `M4:150` 的"层级容量约束" + `M4:95` + [新增+理由：`M4:95` 的"允许端点相等"必须被判据钉死（§3.4 收紧为"不得相等"），否则实现者会照字面实现出一个"允许等价"的校验] |
| M4-A18 | `Read.L0NewestFirst`（I38） | 无 | 三个 L0 文件持有同一 key 的三个版本 ⇒ 读到文件号最大者的值；"tombstone 在旧文件、值在新文件" ⇒ 读到新值；"tombstone 在新文件" ⇒ `NotFound` | MemEnv + 层级构造器 | [指令] `M4:151` |
| M4-A19 | `Read.LevelOrderAndFilesCheckedBound`（I45/A15） | 无 | `hit_layer` 取值正确（`memtable`/`immutable`/`l0`/`l1`…）；稳定态下 `files_checked <= level0_file_num_compaction_trigger + 2 + (kNumLevels - 1)`；`files_checked` == SpyEnv 观测到的 `TableCache::Get` 调用数（**单一真相源**，闭合 `docs/m3-evidence.md:49` 的未闭合项） | SpyEnv（计数）+ `ReadStats` | [新增+理由：M4.2 的门禁（`M4:172`）要求"读放大有改善证据"，而 A15 的裁决把它落在 A 组 ⇒ 必须有这条**确定性**的上界断言；同时它闭合了 M3 遗留的 `files_checked` 归属问题] |
| M4-A20 | `Merge.StdMapReconciliation`（I44） | 无 | 对"同 user key 多 version、同 sequence 多 type（`kTypeValue`/`kTypeDeletion`）、跨层重叠"的随机输入，compaction 后的 `Get`/`DBIter` 结果与 `std::map` 期望模型**逐字节一致**（全量对账） | `test_harness.h` 的 `std::map` 期望模型 | [指令] `M4:152` |
| M4-A21 | `Merge.InternalKeyOrderContract`（I44） | 无 | 输出文件内的 entry 序列在 `InternalKeyComparator` 下**严格升序**；跨输出文件按 user key 严格递增（X2） | `Table` 读路径 | [指令] `M4:155` |
| M4-A22 | `Drop.TombstoneKeptWhenLowerLevelHasOlder`（I40） | 无 | `L1→L2`，`L2` 另有持有同 key 旧值的文件（**不在**本次输入里）⇒ tombstone **保留**在输出中；`dropped_tombstones == 0` | 层级构造器 | [指令] `M4:153` |
| M4-A23 | `Drop.TombstoneDroppedAtBaseLevel`（I40/X6） | 无 | 输入覆盖到 `kNumLevels - 1`（或 `l >= level + 2` 的层全空）⇒ tombstone **被丢**；`dropped_tombstones > 0` | 同上 | [指令] `M4:153` |
| M4-A24 | `Drop.OldVersionKeptWhenSnapshotBelow`（I41） | 快照句柄可构造（A13 的 `GetSnapshot`） | 存在快照 `s`；`L1→L2` 的输入里有 `key@seq=10`(新) 与 `key@seq=5`(旧)，`s.sequence = 5` ⇒ `key@seq=10` 对 `s` 不可见 ⇒ **旧版本 `@5` 必须保留** | `GetSnapshot` | [指令] `M4:154` |
| M4-A25 | `Drop.OldVersionDroppedWithoutSnapshot`（I41） | 无 | 无快照 ⇒ `smallest_snapshot_ == last_sequence_` ⇒ 被更新的可见版本覆盖的旧版本**被丢**；`dropped_old_versions > 0` | 无 | [指令] `M4:154` |
| M4-A26 | `Drop.DecisionIsDisjunctionNotConjunction`（A2 的直接回归） | 无 | 两个子断言：(a) "底层有旧值 **且** 低于最小快照"的 tombstone ⇒ **保留**（若实现成合取会**丢**）；(b) "无快照 **且** 非最底层"的旧版本 ⇒ **被丢**（若实现成合取会**留**）。⇒ 一次用例把两种误实现都杀掉 | 层级构造器 + `dropped_*` 计数 | [新增+理由：`M4-C2` 是本阶段**最高优先级的红线**（`M4:37` 的合取写法与 `M4:98-99` 冲突）；`M4:153`/`M4:154` 的两条分列用例**各自单独**都可能在合取实现下通过（见 §5.5 真值表），必须有一条**专门对照**的用例] |
| M4-A27 | `Drop.KeepsNewestVersionForEveryKey`（**X3**） | 无 | 同一个 user key 在输入里有三条版本（`kTypeDeletion@30`、`kTypeValue@20`、`kTypeValue@10`），无快照、非最底层 ⇒ 断言输出里**恰好保留 `@30` 一条**（若 `last_sequence_for_key` 更新时机写错 ⇒ 三条全丢） | 无 | [新增+理由：§6.4 ③ 的更新时机是"会把一个 key 的全部版本丢光"的静默数据丢失点，`M4` 未点名；`X3` 的判据需要它] |
| M4-A28 | `Snapshot.VisibleVersionSurvivesCompaction`（I41/I42/I44） | `GetSnapshot` + ASan | 建立快照 ⇒ 写覆盖 ⇒ 触发 compaction ⇒ `GetAtSnapshot(s, key)` 仍返回**旧值**且 `NewIteratorAtSnapshot(s)` 的结果与 compaction 前逐字节一致 | `GetSnapshot`/`GetAtSnapshot`/`NewIteratorAtSnapshot` | [指令] `M4:154` |
| M4-A29 | `Snapshot.MinSequenceUpdatesOnRelease`（I41） | `GetSnapshot` | 取两个快照 `s1.seq < s2.seq` ⇒ `smallest_snapshot_ == s1.seq`；释放 `s1` ⇒ `== s2.seq`；释放 `s2` ⇒ `== last_sequence_`；**重复释放同一序列号的另一个句柄**不会误删（`multiset` 的"按值删一个"） | `GetSnapshot`/`ReleaseSnapshot` | [新增+理由：`M4:70` 要求"快照句柄的生命周期与释放"，但未给判据；`multiset` 的按值删除是易错点（按 key 删全部 ⇒ 另一个同 seq 的快照被静默失效）] |
| M4-A30 | `Snapshot.NoSnapshotDefaultBehavior`（I41） | 无 | 无快照时 `smallest_snapshot_ == last_sequence_`；旧版本可丢；tombstone **仍**受 `base_level_for_key` 约束（与 A22/A23 联合断言） | 无 | [指令] `M4:70` + [新增+理由：把"无快照"与"tombstone 仍受底层约束"的组合写成一条显式断言，防"无快照 ⇒ 什么都丢"] |
| M4-A31 | `Amplification.RowReproducibleAndSelfConsistent`（I45） | 无 | 手工构造"已知用户字节 + 已知 flush 写 + 已知 compact 写 + 已知文件字节"的输入 ⇒ 断言 `AMPL` 行的每个字段等于手算值，且 `write_amp_total == (flush+compact)/user`、`write_amp_excl_compact == flush/user`（**两种口径都能从同一行复算**）；同一输入重跑 ⇒ 行**逐字符相同** | 放大统计断言工具 | [指令] `M4:156` |
| M4-A32 | `Output.SplitOnlyOnUserKeyBoundary`（**X2**/I37） | 无 | 输入含"同一 user key 的 3 个版本"，令 `max_file_size` 极小（例如 1 B 的极端设置不可行 ⇒ 用 `max_file_size = block_size` = 512）⇒ 断言输出**只有一个**文件含该 user key（多版本不被拆散）；总输出文件数 ≥ 2（证明滚动真的发生） | 层级构造器 + 可配 `max_file_size` | [新增+理由：§6.4 ③ 的分切规则是 I37 的直接前提；`M4:150` 的"层级容量约束"不含它，但违反它会让**下一次**安装被 I37 拒绝 ⇒ 必须在第一次就测出来] |
| M4-A33 | `Install.RebaseOnConcurrentFlushKeepsI37`（L25/L27/I37） | 注入"compaction 期间让 flush 完成一次安装"的 seam（`CompactionHook::OnOutputWritten` 里驱动一次 flush） | 安装成功（`install_rebase_retries >= 1` 或 `>= 0` 但结果正确）；最终 `ValidateLevelLayout` 通过；被 flush 新加的 L0 文件**仍在** L0；compaction 的输入文件**全部**从原层消失；无文件被误删 | `CompactionHook` + `FlushHook` | [新增+理由：`M4:110`（L25）明写"回锁后校验状态再安装"，而 `M4:191` 的评审项 4 要求核对"L1+ 重叠检测覆盖全部安装路径"⇒ 必须有并发安装的用例；否则 L25 的回锁校验是死代码（`D3:487` 的先例："否则开关是死代码"）] |
| M4-A34 | `Delete.DeferredUntilRefsZero`（I42/I43/**X8**） | 无 | 持住**旧** `Version` 的引用（模拟一个正在迭代的读者）⇒ 触发一次删除 ⇒ 断言文件**仍在**（专测"只看 `current_`"的错误实现）；释放引用 ⇒ 再触发 ⇒ 文件被删且计数 +1 | 直调 `MaybeDeleteObsoleteFiles` + 可持版本引用的 seam | [指令] `M4:100`/`M4:101` + [新增+理由：`M4:189` 的评审项 2 明确要查"被引用文件是否绝不被删"，而"只看 current_"是最容易被写出的错误实现] |
| M4-A35 | `Delete.NoUnlinkInsideInstallCriticalSection`（I43/L24/L26） | SpyEnv（记录 `unlink`/`DeleteFile` 事件 + 线程） | 在安装临界区（`install_mu_` 持有期）内**没有** `DeleteFile` 调用；`unlink` 发生在 `install_mu_` 释放之后；`deletion_mu_` 的嵌套顺序为 `deletion_mu_ → mutex_`（事件序列可判定） | SpyEnv 事件序列 | [新增+理由：`M4:101` 明写"禁止在安装 Version 的临界区内同步删除"、`M4:109` 给定锁序 ⇒ 都需要可判定证据；`D3:2223-2229`（R3）的教训是"探针不加宽则用例属空绿"⇒ 见 §10.4] |
| M4-A36 | `Locks.ZeroIoWhileHoldingDbMutexOnCompactionPath`（L22/L25/L26） | SpyEnv（**必须加宽**：拦 `NewWritableFile`/`Append`/`Sync`/`RenameFile`/`CreateDir`/`SyncDir`/`GetFileSize`/`GetChildren`/`RemoveFile`/`Truncate`/`NewRandomAccessFile`/`RandomAccessFile::Read`）+ `DbMutexHeldOnThisThread()` | compaction 全路径（选层、读输入、写输出、追加 MANIFEST、`rename`、`SyncDir`、`unlink`）在持 `mutex_` 期间的 IO 调用数 == **0**；且每个注入点的计数 `> 0`（**反向自检**，防空绿） | SpyEnv 加宽 + `MuHeldGuard` | [指令] `M4:111`（L26）+ `D3:2227-2229` R3 的纪律（"未加宽探针之前，通过不得计入验收"） |
| M4-A37 | `Scheduling.FlushTakesPriorityOverCompaction`（L27；`NOTE:219` M4-R1 的饥饿探针） | 注入"慢 compaction"（大输入 + 慢 Env） | compaction 处于"准备安装"前的让路检查时，`immutables_` 非空 ⇒ compaction 等待；flush 完成安装后 compaction 才继续；**flush 的入队→注册等待上界 ≤ 1 次 flush 的 IO 时间**（用 `FakeClock` + 屏障确定性验证，不靠调度赌） | `FakeClock` + 慢 Env + 屏障 | [新增+理由：`M4:71`/`M4:112`（L27）把"避免 flush 被 compaction 饿死"列为必须写清的点，但 `M4` 未要求用例；`NOTE:219` 明确建议加"饥饿探针"] |
| M4-A38 | `Shutdown.JoinsBothThreadsAndDrainsQueue`（L29） | ASan | `Close()` 期间：两个线程都被 join；`pending_delete_` 被处理；`compaction_stats_.aborted` 有计数；关闭后无 UAF（ASan 干净）；重复 `Close()` 幂等 | 无 | [指令] `M4:114`（L26→L29）+ [新增+理由：`M4:114` 给了纪律但 `M4:147-156` 的 A 组清单没有关闭用例；`D3:1916` 的 `M3-A25`（停等路径不放锁死锁）是同类先例] |
| M4-A39 | `Migration.MetaToManifestOneShot`（A9/I46） | MemEnv（构造一个 M3 形态的库：`META` + `*.sst`） | 首次 `Open` 后：`CURRENT` 存在且指向可回放的 `MANIFEST-<n>`；`META`/`META.tmp` **不存在**；`meta_migrated == 1`；数据全部可见；**第二次 `Open` 不再走迁移分支**（`meta_migrated == 0`）；全仓 `grep 'META'` 的写路径命中 == 0（静态检查另做） | MemEnv + 手工构造 `META` | [新增+理由：`M4-C9` 要求"写清迁移路径且不允许稳态并存"⇒ 必须有"一次性"与"不并存"的可判定证据] |
| M4-A40 | `Manifest.RollWritesSnapshotAndKeepsNumberingMonotonic`（**X5**/I35） | 无（`kManifestRollBytes` 可通过测试专用 seam 调小） | 令 `manifest_bytes_ > 阈值` ⇒ 走模式 (a)：新 MANIFEST 的首条 record 是**全量快照**、旧 MANIFEST 被删、`CURRENT` 内容更新；**所有族**（`.log`/`.sst`/`MANIFEST`）的编号仍严格小于 `next_file_number_`（专测 X5 的"重用编号"错误） | 可配的 roll 阈值 seam | [新增+理由：`M4-C3` 的模式 (a) 是本设计的持久化核心，且 `NOTE:224`（M4-R6）要求"周期性写全量快照"的触发被规定 ⇒ 必须有用例；X5 的编号重用是"覆盖 CURRENT 指向的文件"这类灾难性 bug 的唯一防线] |
| M4-A41 | `Recovery.AckedDataVisibleAfterManifestReplay`（I46） | MemEnv（含 `SimulateCrash`） | 一批 `Put(sync=true)` 返回 `kOk` ⇒ 模拟崩溃（在若干个注入点）⇒ 重新 `Open` ⇒ 每个 key 的值逐字节正确；`edits_replayed > 0`；`missing == 0`（用 M2 的 sidecar 形状在 A 组做小规模版本） | MemEnv + 注入点 | [指令] `M4:104`（I42→I46）+ `M4:24` |
| M4-A42 | `Install.FailureKeepsCurrentRecoverable`（`M4:36`/`M4:190`） | FaultyEnv（注入 `Sync`/`RenameFile`/`SyncDir` 失败） | 在模式 (b) 的追加失败、模式 (a) 的②~⑨各步失败 ⇒ `Status` 非 `kOk`、`bg_error_` 粘性、**内存 `version_` 不回滚**、`CURRENT` 仍指向一个可完整回放的 MANIFEST（重新 `Open` 成功且数据不丢） | FaultyEnv 各注入点 | [指令] `M4:190`（评审项 3："每一步失败是否保持旧状态可恢复"）+ [新增+理由：`M4:190` 是评审要求而不是用例要求 ⇒ 无用例则评审只能"看代码"] |
| M4-A43 | `Orphan.CompactionOutputCleanedAndCounted`（`M4:126`/`M4:195`） | MemEnv（在 `OnOutputWritten` 之后注入崩溃） | 重启后：未注册的 `*.sst` 被删（`compaction_orphan_sst_removed >= 1`）；**不被注册进任何 Version**；数据仍可见（来自 WAL）；`.tmp` 也被清 | `CompactionHook` + MemEnv | [指令] `M4:160`（B 组的 A 组确定性对应物） |

**A 组条数：43**（其中 [指令] 20 条、[新增] 23 条，每条新增都给理由）。

### 10.2 B 组（真实磁盘 / 进程级，脚本驱动）

| 编号 | 用例 | 依赖假设 | 通过判据 | 来源 |
|---|---|---|---|---|
| M4-B01 | 全量压测（随机写 + 随机读 + 定期 `kill -9`）后 `missing 0` | M2/M3 的 `crash_writer`/`crash_recover` + **小写缓冲**（`--write-buffer-size 262144`，`D3:1845` G5） | `ROUNDS_OK == ROUNDS` ∧ `MISSING_TOTAL == 0` ∧ `MISMATCH_TOTAL == 0` ∧ `SST_FILES_TOTAL > 0` ∧ `LOGS_DELETED_TOTAL > 0` ∧ `COMPACTION_ROUNDS_TOTAL > 0`（**M4 新增的正向标记**：防"compaction 一次都没发生"的空绿） ∧ 末尾 `[COMPACTION_CRASH_OK]` | [指令] `M4:159`、`M4:21` |
| M4-B02 | compaction 中途 `kill -9` 的版本恢复 + 孤儿清理 | `CompactionHook` 的四个注入点 + 随机延迟（种子打印） | 重启后：`CURRENT → MANIFEST` 完整回放；层级文件集合 == **崩溃前最后一个成功持久化的 Version**（A8 的判据）；未注册输出被识别为孤儿并**计数清理**；`missing 0` | [指令] `M4:160`、`M4:24` |
| M4-B03 | 两种 pick 策略的对照实测与数据表 | 同一脚本、同一数据规模、**同一机器**、同一轮内交替（`--strategy` 切换） | 输出 `M4:206` 的固定列：`策略名 / 写放大 / 读放大 / 空间放大 / 中位延迟 / 备注`；两行都要有；**负结果行保留原文并标注"结论作废 + 作废原因"** | [指令] `M4:161`、`M4:23`、`M4:206` |
| M4-B04 | 长压测下的层级文件数与字节数随时间变化 | 长跑（≥ 10× 数据量） | `LEVEL` 行按 `round_id` 序列化，能看出：L0 在 `trigger` 附近振荡；L1+ 的字节数被压到容量附近而非单调增长；**无"冷文件永不合并"**（每个文件号最终都从 L0/L1 消失过——用 `grep` 文件号的出现/消失判定） | [指令] `M4:162` |
| M4-B05 | 句柄上限（compaction 与前台读共享 `TableCache` 预算） | `/proc/self/fd` 计数（形状照 `D3:1889` 的 `M3-B05`） | compaction 压测结束后 `/proc/self/fd` 计数**不增长**；且峰值 ≤ `max_open_files` + 常数 | [新增+理由：`NOTE:220`（M4-R2）登记的头号风险是"compaction 的输入 + 输出与前台读共享同一 fd 预算"；`D3:1889` 的 I30 已把 fd 上界变成不变量 ⇒ 必须有流水线级证据] |
| M4-B06 | 读放大改善证据（固定格式行） | 与 M3 基线同数据规模（100 万 key / 4 MiB 写缓冲，`D3:2136` 的 F≈59） | `files_checked_p50 <= 3` ∧ `files_checked_max <= level0_file_num_compaction_trigger + 2 + (kNumLevels - 1) = 12`；同一行同时打印 M3 基线的对照值（59）；**A15 的裁决**：M4.2 用 A 组的 `M4-A19` 先给结构性证据，本行给实测数字 | [指令] `M4:172`、`M4:163` |
| M4-B07 | 前台 P99 与 compaction 单轮耗时的量级分离 | A12 的门禁数字 | `p99_front_us * 10 <= p50_compaction_round_us`；**同一行**同时打印 `FRONT` 与 `AMPL` 的分母（A12 要求） | [指令] `M4:20` |
| M4-B08 | 存活 Version 数有界 | `AmplificationStats::live_versions_max` | 长压测下 `live_versions_max <= 常数`（例如 ≤ 4：`current_` + 一个 reader + 一个 compaction 快照 + 余量），且**不随轮数增长** | [新增+理由：`M4:128` 把"内存中版本对象无限增长…长压测下 OOM"列为风险，`NOTE:223`（M4-R5）建议加"存活 Version 数"计数；`M4:38` 的"只增不减"必须被解释为"不可就地修改"而非"永不析构"] |
| M4-B09 | MANIFEST 体积与回放时间随轮次的变化 + 重建阈值生效 | `ManifestStats` | `manifest_bytes` 随编辑数增长，但**每越过阈值就回落**（`rolls > 0`）；`replay_edits` 相应回落；单次 `Open` 的版本回放时间有界（打印 `OPEN_MS` 分项） | [新增+理由：`M4:72` 要求"恢复时间"的取舍被明确，`NOTE:224`（M4-R6）要求"周期性写全量快照"的触发被规定并计入统计 ⇒ 必须有实测行] |
| M4-B10 | 可选块缓存的命中率与读放大改善对比 | **不适用** | 显式输出一行 `M4-B10 NOT_APPLICABLE (block cache not introduced in M4; see docs/m4-design.md §2.1 D8)` | [指令] `M4:163`（**前提不成立**；不得静默省略，§2.1 D8） |
| M4-B11 | 门禁接线（正向标记 AND） | `scripts/lsm_gate.sh` | 追加 M4 腿；每条腿要求**多条标记 AND**；缺脚本 ⇒ `SKIP` + 汇总 `[PARTIAL]`（绝不空绿）；`--require-m4` 时 SKIP 判 FAIL | [新增+理由：`M4:196` 要求脚本自校验，`D3:1847` G7 与 `PR3:716`（D9.6）登记了"空绿"教训 ⇒ 新腿必须进同一套机制] |

**B 组条数：11**（[指令] 6 条、[新增] 4 条、[不适用] 1 条）。

**B 组对 M3 的前置依赖（§0.6 第 1 条；`#1` 必须写进"开工门"）**：`M4-B01/B02` **复用** M3 的崩溃脚本与工具
⇒ M3 的 `scripts/lsm_flush_crash_test.sh`、`tests/recovery_m3_test.cpp`、`M3-B01~B05` **必须先交付**
（`docs/m3-evidence.md:54` §4 第 9 条实测它们**尚未交付**）。**前置检查命令（可直接粘贴）**：

```bash
cd ~/lsm-kv
# ① M3 收口的硬门：tag 存在 + 工作区干净
git tag | grep -qx m3-sstable && echo "[M4-PRE-OK] tag m3-sstable 存在" || echo "[M4-PRE-FAIL] 缺 tag m3-sstable"
test -z "$(git status --porcelain)" && echo "[M4-PRE-OK] 工作区干净" || echo "[M4-PRE-FAIL] 工作区脏"
# ② M3 崩溃脚本与正向标记（M4-B01/B02 的直接前置）
test -f scripts/lsm_flush_crash_test.sh \
  && grep -c 'FLUSH_CRASH_OK' scripts/lsm_flush_crash_test.sh \
  || echo "[M4-PRE-FAIL] 缺 scripts/lsm_flush_crash_test.sh（M3.3 未交付）"
# ③ M3 的 A/B 组用例存在
test -f tests/recovery_m3_test.cpp && echo "[M4-PRE-OK] recovery_m3_test 存在" \
  || echo "[M4-PRE-FAIL] 缺 tests/recovery_m3_test.cpp"
# ④ 门禁里 M3 四腿不是 SKIP（--require-m3 ⇒ SKIP 即 FAIL）
bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3 | tee /tmp/m4pre.log
grep -q 'SKIP' /tmp/m4pre.log && echo "[M4-PRE-FAIL] 仍有 M3 腿被 SKIP" || echo "[M4-PRE-OK] M3 腿全部真跑"
grep -q '\[PARTIAL\]' /tmp/m4pre.log && echo "[M4-PRE-FAIL] 门禁汇总为 PARTIAL" || echo "[M4-PRE-OK] 门禁无 PARTIAL"
# ⑤ M3.2 的产物齐备（M4 的接口基线）
for f in src/version_set.h src/version_set.cpp src/version_edit.h src/version_edit.cpp \
         src/merging_iterator.h src/merging_iterator.cpp src/db_iter.h src/db_iter.cpp; do
  test -f "$f" || echo "[M4-PRE-FAIL] 缺 $f"
done
# ⑥ 全量用例数不得少于 M3 收口时的数字
./build/bin/lsm_tests --gtest_list_tests | grep -c '^  ' || true
```

### 10.3 三个放大的口径定义表（`M4:22`/`M4:83` 的落点；**固定行格式**）

**约定**：三行都用 `KEY=VALUE` 的空格分隔形式；**前缀列一旦冻结不得改**（I45 的可复现性），
**只允许在行尾追加新列**（M5 要加 fitler 列，`M5:182`；M5 只能追加）。

```
AMPL  round_id=<C<k>|ALL> strategy=<round_robin|min_overlap>
      user_logical_bytes=<B>  entry_bytes=<B>
      flush_write_bytes=<B>   compact_write_bytes=<B>
      write_amp_total=<f>            # (flush+compact)/user      ← 主列（含 compaction 自身）
      write_amp_excl_compact=<f>     # flush/user                ← 对照列
      read_files_checked=<n>  read_index_blocks_read=<n>  read_data_blocks_read=<n>  read_bytes=<B>
      read_get_count=<n>      read_amp_files_per_get=<f>          # files_checked / get_count
      space_sst_bytes=<B> space_manifest_bytes=<B> space_current_bytes=<B>
      space_log_bytes=<B> space_tmp_bytes=<B>
      space_amp=<f>                  # (sst+manifest+current+log+tmp)/user  ← 含临时文件与 MANIFEST
      space_amp_sst_only=<f>         # sst/user                              ← 对照列
      dropped_old_versions=<n> dropped_tombstones=<n>
      compaction_rounds=<n> compaction_round_p50_us=<n> compaction_round_max_us=<n>
      live_versions_max=<n>

LEVEL round_id=<C<k>|ALL>
      l0_files=<n> l0_bytes=<B> l1_files=<n> l1_bytes=<B> ... l6_files=<n> l6_bytes=<B>
      total_sst_files=<n> total_sst_bytes=<B>
      l0_score=<f> l1_score=<f> ... l6_score=<f>

FRONT round_id=<C<k>|ALL>
      ops=<n> sync_ops=<k> get_ops=<n>
      p50_us=<n> p99_us=<n> p999_us=<n> max_us=<n>
```

| 口径 | 分子 | 分母 | 统计窗口 | 依据 |
|---|---|---|---|---|
| **写放大（主）** | `flush_write_bytes + compact_write_bytes`（**写进 `.sst` 与 MANIFEST 的字节**；不含 WAL） | `user_logical_bytes` = Σ(`key.size + value.size`) | 双窗口：`C<k>`（单次 compaction 提交）与 `ALL`（整段压测） | `M4:73`（"必须明确写放大是否把合并自身的写算进去"）+ `M4:194`；`NOTE:222`（M4-R4）要求"同时打印分项，使两种口径都能从同一行复算" |
| **写放大（对照）** | `flush_write_bytes` | 同上 | 同上 | 同上 |
| **读放大** | `read_files_checked`（**只数真的进了 `Table::Get` 的文件**，被 key range 过滤掉的不算）+ `read_index_blocks_read` + `read_data_blocks_read`；`read_bytes` 含块头/CRC/restart 数组的**全部**字节 | `read_get_count`（点查次数） | `ALL`（读放大是累计量，单轮窗口对它无意义 ⇒ **明说**） | `D3:553`（M3 的 `ReadStats` 口径，**逐字不改**）；`D3:2114`（"三个放大率的聚合与报告那属 M4"） |
| **空间放大（主）** | `space_sst_bytes + space_manifest_bytes + space_current_bytes + space_log_bytes + space_tmp_bytes`（**含临时文件、MANIFEST、WAL、孤儿/残留**） | `user_logical_bytes` | `ALL` | `M4:14`（"磁盘占用 / 逻辑数据量"）+ `M4:194`（"空间放大是否包含临时文件"⇒ 主列**包含**，并给不含的对照列） |
| **空间放大（对照）** | `space_sst_bytes` | 同上 | `ALL` | 同上 |
| **中位延迟** | `FRONT` 行的 `p50_us`（前台操作端到端） | — | `ALL` | `M4:206` 的固定列 |

**窗口与可复算性（I45 的判据）**：
`ALL` 行的每个**计数类**字段必须等于其 `C<k>` 行对应字段之和（`space_*` 快照类字段除外，
它们是"末态快照"而非累计量）⇒ `M4-A31` 用它做自洽性断言。
`write_amp_total` / `space_amp` 这类**比值**由脚本从同一行的分项**复算**，**不单独存储**
（避免"存了一份可能被改的比值"）。

### 10.4 反"空绿"检查（沿用 `docs/m3-evidence.md:32-39` 的纪律，逐条适用）

| 检查 | 方法 | M4 的期望 |
|---|---|---|
| 零断言 TEST 块 | `awk` 逐 `TEST(...){...}` 块扫描 `EXPECT_/ASSERT_` | **0 个** |
| 跳过/禁用/永真 | `grep -nE 'DISABLED_\|GTEST_SKIP\|\|\| true' tests/` | **0 处** |
| 既有断言是否被放宽 | `git diff -U0` 中被删除行里的 `EXPECT_/ASSERT_` 计数 | **0**（M4 不得改 M1~M3 既有测试断言，`M4:134`） |
| 探针是否真的被用上 | SpyEnv/FaultyEnv 的每个注入点计数 `> 0`（**反向自检**） | 全部 `> 0`；未加宽的探针 ⇒ 相关用例**不得计入验收**（`D3:2227-2229` R3） |
| 门禁是否只看退出码 | `scripts/lsm_gate.sh` 的 `run_gate_marked`（多条标记 AND） | M4 腿同样接进去；`--require-m4` 可把 SKIP 变 FAIL |
| "compaction 真的发生了" | `COMPACTION_ROUNDS_TOTAL > 0` 的正向标记 | **必须**（否则 B01 可能只是 M3 门禁的复制品，`D3:1843` G3 的同源教训） |
| `grep 'META'` 的写路径命中 | `grep -n 'META' src/version_set.cpp src/db_impl.cpp` | 只允许迁移分支 + 注释（§3.6 的可检查判据） |

---

## 11. 子里程碑拆分（M4.0 → M4.1 → M4.2 → M4.3）

> 每步**一个提交**，提交信息引用本文档的章节号（`M4:211`）；只实现让**当前阶段测试集**通过的最小代码，
> **不提前实现** M5 及以后特性（`M4:167`、`RM:42`）。
> 每步结束必须给出**实测证据**（构建命令 + 原始输出摘录，`M4:183`、`RM:55`"未跑不算过"）。
> 每个阶段的**回归集是上一阶段的超集**（沿用 `D3:2053-2055` 的先例）。

### M4.0 —— 开工前置复核（**不是功能提交**；`docs(m4):` 一次文档提交）

> **为什么必须有这一步**：本设计的接口签名是**凭空起草**的——`src/version_set.*` / `src/version_edit.*` /
> `src/merging_iterator.*` / `src/db_iter.*` 在 `e56d0b7` 上**不存在**（§0.3 实测），它们是
> **M3.2 的交付物（此刻正由另一个代理实现）**，M3.3 再加 `META` 的 `Persist`/`Recover`。
> ⇒ 在写下第一行 M4 代码**之前**，必须先用**真实签名**校对本设计的草案。

| 项 | 内容 |
|---|---|
| 产物 | `docs/m4-prerequisites.md` 的 §0"接口基线复核"小节（`#1` 产出）+ 本文件 §15 追加一条 R 记录 |
| 复核清单（**逐项核对，不一致处以落地实现为准并登记差异**） | 见下表 |
| 判据 | 复核清单每一项都有"与草案一致 / 不一致（差异描述 + 处置）"的结论；**不得**留空项 |
| 证据命令 | 见下表右列（可直接粘贴执行） |
| 若发现重大不一致 | 按 `M4:40` 的流程锁**回退 `#0`** 修改本文档，**禁止**在校验阶段私改实现方案 |

| # | 复核对象 | 期望（本设计的草案） | 证据命令（粘贴执行） |
|---|---|---|---|
| C1 | `M3` 是否已收口 | tag `m3-sstable` 存在、工作区干净、`M3-B01~B05` 已交付 | §10.2 的六条前置检查命令 |
| C2 | `Version` 的真实形态 | `files()`（无参）+ `log_number()` + `min_log_number_to_keep()` + `MaxSequenceInFiles()`；**不可变**；构造后只读 | `grep -n 'class Version' -A 30 src/version_set.h` |
| C3 | `files()` 的排序 | **按文件号降序**（`D3:1681`） | `grep -n 'sort\|reverse\|Compare' src/version_set.cpp` |
| C4 | `TableCache` 的落点与键 | 在 `version_set.h`（不是独立文件）；键 = 文件号；容量 `Options::max_open_files` | `grep -rn 'class TableCache' src/` |
| C5 | `VersionEdit` 的真实签名 | `AddFile`/`files()`/`EncodeTo`/`DecodeFrom`/`SetLogNumber`/`SetMinLogNumberToKeep`/`SetNextFileNumber`/`SetComparatorName` | `grep -n 'class VersionEdit' -A 25 src/version_edit.h` |
| C6 | `VersionSet::Recover` 的签名 | **不暴露"META 是文件还是日志"**（`D3:1704-1705`）；参数含 `Version** out` 与 `RecoveryStats*` | `grep -n 'Recover\|Persist' src/version_set.h` |
| C7 | `filename.{h,cpp}` 的落地集合 | 至少含 `TableFileName`/`TempFileName`/`ParseTableFileName`/`MetaFileName`（`PR3:509`） | `grep -n '.' src/filename.h` |
| C8 | `Options` 的落地字段 | 至少含 `block_size`/`verify_checksums`/`max_open_files`/`recycle_log_files`/`flush_hook`（`PR3:509`） | `grep -n 'struct Options' -A 30 src/common.h` |
| C9 | `TableOptions` 是否已与 `Options` 收敛 | 若仍独立 ⇒ **M4.1 必须收敛**（§2.2 A11 的额外处置） | `grep -rn 'TableOptions\|block_size' src/ \| head -20` |
| C10 | `MergingIterator` / `DBIter` 的构造签名 | `MergingIterator(const InternalKeyComparator*, Iterator**, int)`；`DBIter(const InternalKeyComparator*, Iterator*, SequenceNumber)`（`D3:1570`/`D3:1594`） | `grep -n 'MergingIterator(\|DBIter(' src/merging_iterator.h src/db_iter.h` |
| C11 | flush 线程与 `FlushHook` 的真实形态 | 单后台线程 + `bg_cv_` + `flush_hook` 的三个注入点（`D3:1865` E8） | `grep -n 'bg_thread_\|bg_cv_\|FlushHook' src/db_impl.h src/common.h` |
| C12 | `META` 的落点行 | 记录 `grep -c 'META' src/version_set.cpp` 的**基线值**（M4 要改的就是它） | `grep -n 'META' src/version_set.cpp src/filename.cpp` |
| C13 | **`M3-A39/A40/A41` 的断言是否绑定了 `META` 文件名** | **必须不绑定**（否则 M4 的迁移会撞 `M4:134` 的禁令；§2.2 A9） | `grep -n 'META' tests/recovery_m3_test.cpp` |
| C14 | `FakeClock` 是否已存在 | **期望 0 命中**（`NOTE:412` M5-R9 实测；§0.6 第 3 条） | `grep -rn 'FakeClock' tests/ src/ \| wc -l` |
| C15 | 门禁 M3 腿的真实状态 | 非 SKIP、非 `[PARTIAL]` | §10.2 的检查命令 ④ |

### M4.1 —— `Version`/`VersionEdit`/`VersionSet`/`MANIFEST`/`CURRENT` + 启动恢复（含截断/损坏分支）

| 项 | 内容 |
|---|---|
| 新增 | `src/version_edit.{h,cpp}`（差分形态，§5.2）、`src/version_set.{h,cpp}` 的 MANIFEST 部分、`tests/version_test.cpp`、`docs/m4-prerequisites.md`（由 `#1` 产出）、`docs/protocol.md` **追加 §11**（§4 的 patch 文本照抄，**纯追加** + `wc -l` 与 `sha256` 前缀证据） |
| 必改 | `src/filename.{h,cpp}`（§3.1 的 5 个函数）、`src/db_impl.{h,cpp}`（`Recover` 接入 + `META` 迁移分支 + `manifest_*` 状态）、`src/common.h`（§5.6 的 6 个字段 + `CompactionHook` 类型）、`src/version_set.{h,cpp}`（`VersionSet::Recover`/`LogAndApply` 的函数体）、`CMakeLists.txt`（新增独立目标 `lsm_version`，§1.4 的零依赖机制）、`.gitignore`（`MANIFEST-*.tmp`、`CURRENT.tmp`、压测临时目录） |
| **不改** | `src/memtable.*`、`src/wal.*`、`src/skiplist.h`、`src/sstable/*`（除 `TableOptions` 收敛这一项，若 C9 判定需要）、M1~M3 的既有测试文件 |
| 判据 | `version_test` 全绿（`M4-A01~A10`、`M4-A39`、`M4-A40`、`M4-A42`）；CURRENT 原子切换/恢复用例（`M4-A08/A09/A10`）通过；**干净重建 0 warning**；M1~M3 的既有用例**数量不减少**且全绿 |
| 证据命令 | 见下 |
| 风险 | `CURRENT` 的严格校验与"半个文件"防护（`M4-A10`）/ `META` 迁移的"一次性"与"不并存"（`M4-A39`）/ X5 的编号重用（`M4-A40`）/ 逐字节截断后追加（`M4-A06`） |

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh                                          # 干净重建 + 0 warning + 全量
./build/bin/lsm_tests --gtest_filter='VersionEdit.*:Manifest.*:Current.*:Migration.*:Recovery.*'
./build/bin/lsm_tests                                              # 全量：M1~M3 既有用例不得减少
cmake --build build --target lsm_version
nm -C build/liblsm_version.a | grep -cE 'db_impl|wal|memtable'      # 期望 0（依赖纪律）
bash scripts/lsm_gate.sh --rounds 100 --no-asan                    # M2/M3 腿不得回退
git diff --stat docs/protocol.md && wc -l docs/protocol.md         # 纯追加的证据
```

### M4.2 —— 层级 + 选层/选文件 + compaction 执行 + 读路径层级化 + 丢弃判据

| 项 | 内容 |
|---|---|
| 新增 | `src/compaction.{h,cpp}`（§5.4/§5.5）、`tests/compaction_test.cpp`、`scripts/lsm_level_stats.cpp`、**`tests/test_harness.h` 的追加**（见下"FakeClock 的归属决策"） |
| 必改 | `src/version_set.{h,cpp}`（`Version::level_files`/`AllFiles`/`Ref`/`Unref`/`total_bytes`、`live_versions_`、`ValidateLevelLayout`）、`src/db_impl.{h,cpp}`（compaction 线程、`MaybeScheduleCompaction`、`MakeRoomForWrite` 的层级分支、`InstallCompactionResults`、延迟删除队列、`GetSnapshot`/`ReleaseSnapshot`/`GetAtSnapshot`/`NewIteratorAtSnapshot`、§5.7 的四个统计入口）、`src/common.h`（若 M4.1 未加完）、`CMakeLists.txt` |
| 判据 | `compaction_test` **A 组全绿**（`M4-A11~A38`、`M4-A41`、`M4-A43`）；含 tombstone/旧版本丢弃条件（`M4-A22~A27`）与 `std::map` 全量对账（`M4-A20`）；**读放大改善的结构性证据**（`M4-A19` 的上界断言，A15 的裁决）；干净重建 0 warning；ASan 干净；M3 门禁不回退 |
| 证据命令 | 见下 |
| 风险 | X1 的闭包（`M4-A16`）/ X2 的切分边界（`M4-A32`）/ X3 的更新时机（`M4-A27`）/ X6 的 `+2`（`M4-A23`）/ L25 的 rebase（`M4-A33`）/ L24 的锁序（`M4-A35`）/ L26 的持锁零 IO（`M4-A36`，**探针必须加宽**）/ L27 的优先级（`M4-A37`） |

**`FakeClock` 的归属决策（二选一，本设计选定并给理由）**：

| 候选 | 取舍 |
|---|---|
| (a) 作为 `MemEnv` 的成员 | `D3:1958` 的 `M3-A25` 措辞是"`FakeClock`（MemEnv）"。**问题**：`MemEnv` 的职责是"文件系统的崩溃语义 seam"（`D3:2129`、`D3:2241`），塞进时钟会承担**第二个正交职责**；且所有不关心时间的 `MemEnv` 用例（A01~A19 一大批）要多一个构造参数 |
| **(b)（推荐）在 `tests/test_harness.h` 内新建 `class FakeClock` + 一个 `Env` 装饰器 `ClockEnv`** | ① `M4:141` 与 `M5:128` 都写"追加 `tests/test_harness.h` 的辅助"，M5 要复用 ⇒ 与该纪律一致；② **零改动 `src/util/env.h`**：`Env::NowMicros()` 已是虚函数 ⇒ 用装饰器（`NowMicros()` 转调 `FakeClock`，其余全部转调内层 `Env`）即可；③ 被测代码仍**只**经 `Env` 取时间，不引入第二套时间来源（单一真相源） |

**形态草案**：
```cpp
// tests/test_harness.h（【M4 新增】）
class FakeClock {                       // 确定性时间：只前进，不回退
 public:
  uint64_t NowMicros() const { return now_us_; }
  void Advance(uint64_t us) { now_us_ += us; }
 private:
  uint64_t now_us_ = 0;
};
class ClockEnv : public Env {           // Env 装饰器：只改 NowMicros，其余全转调 inner
 public:
  ClockEnv(Env* inner, FakeClock* clock);
  uint64_t NowMicros() override { return clock_->NowMicros(); }
  void SleepForMicros(uint64_t us) override { clock_->Advance(us); }   // **不真睡**（零 flaky 的关键）
  /* 其余每个虚函数 ⇒ inner_->Xxx(...) */
};
```
**落地步骤（M4.2 的提交内）**：① 加 `FakeClock` + `ClockEnv`（`ClockEnv` 必须实现 `Env` 的**全部**虚函数；
`tests/faulty_env.h`/`memenv.h` 已提供"新增纯虚方法时同步补全"的先例，`e56d0b7` 的 `+4` 行即此）；
② `M4-A37` 用 `ClockEnv(MemEnv, &clock)` 做"慢 compaction"与"停等上界"的确定性验证；
③ 登记：`D3` 里"`FakeClock`（MemEnv）"的措辞与 (b) 不一致 ⇒ 写进 `#1` 的"措辞对齐登记"（**本轮不改 `m3-design.md`**）。

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh
./build/bin/lsm_tests --gtest_filter='Compaction.*:PickLevel.*:PickFile.*:L0Inputs.*:LevelLayout.*:Read.*:Merge.*:Drop.*:Snapshot.*:Amplification.*:Output.*:Install.*:Delete.*:Locks.*:Scheduling.*:Shutdown.*:Orphan.*'
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests
./build/bin/lsm_tests          # 全量：M1~M3 用例不得减少
bash scripts/lsm_gate.sh --rounds 100 --no-asan
```

### M4.3 —— 快照一致性收口 + 三个放大埋点 + 压力与崩溃脚本 + 策略对照实测与负结果入档

| 项 | 内容 |
|---|---|
| 新增 | `scripts/lsm_compaction_stress.sh`（`M4:34` 要求）、`docs/amplification.md`（`M4:50`/`M4:131`）、`docs/m4-evidence.md`、`tests/compaction_crash_test.cpp`（若 A 组之外还需要进程级用例） |
| 必改 | `scripts/lsm_gate.sh`（追加 M4 腿 + 正向标记 AND）、`CMakeLists.txt`（`lsm_level_stats` 目标）、`.gitignore`、`src/db_impl.{h,cpp}`（放大埋点的收口与 `AMPL`/`LEVEL`/`FRONT` 行的输出） |
| 判据 | **B 组全部**（`M4-B01~B11`）；`kill -9` 对账 `missing 0`（含 `COMPACTION_ROUNDS_TOTAL > 0` 的正向标记）；**三张放大统计表入档**；**策略负结果入档**（`M4-B10` 的 `NOT_APPLICABLE` 行 + D1 的 size-tiered "结论作废"行）；`M4-B07` 的 P99 门禁（A12）；TSan/ASan 干净；**tag `m4-compaction`** 并 push（`M4:211`） |
| 证据命令 | 见下 |
| 风险 | 门禁的"空绿"（`M4-B11`）/ `kill -9` 只证进程级一致性（§12.5 第 1 条）/ 策略对照的机器状态漂移（同轮交替，`NOTE:411` M5-R8 的教训）/ 放大口径的可复算性（`M4-A31` + I45） |

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh
bash scripts/lsm_compaction_stress.sh --rounds 100 --write-buffer-size 262144
setarch $(uname -m) -R bash scripts/lsm_compaction_stress.sh --rounds 100 --write-buffer-size 262144   # TSan 腿
bash scripts/lsm_compaction_stress.sh --rounds 20 --strategy round_robin  --emit-ampl > /tmp/amp_rr.txt
bash scripts/lsm_compaction_stress.sh --rounds 20 --strategy min_overlap  --emit-ampl > /tmp/amp_mo.txt
./build/bin/lsm_level_stats /tmp/lsm_stress_db | grep -E '^(LEVEL|AMPL|FRONT) '
bash scripts/lsm_gate.sh --rounds 100 --with-tsan --require-m4
grep -c 'MISSING 0' /tmp/amp_rr.txt ; grep -c '\[COMPACTION_CRASH_OK\]' /tmp/amp_rr.txt
git tag -a m4-compaction -m "M4 分层 Compaction（docs/m4-design.md）" && git push origin m4-compaction
```

---

## 12. 自检（占位符 / 内部矛盾 / 歧义 / 范围越界 / 已知薄弱点）

### 12.1 占位符

- 全文**无** `TODO` / `TBD` / `XXX` / `FIXME`。
- 出现的 `...` / `<...>` 全部在**伪代码省略**或**行格式占位**（§6、§10.3 的 `AMPL`/`LEVEL`/`FRONT` 行、
  §4 的 patch 文本）里，属结构性省略，逐处可核。
- 每个新增常量都有明确取值（§3.3 的 6 个 tag、§3.5 的 `kNumLevels = 7`、§5.6 的 6 个字段默认值、
  §3.2 的 `kMaxManifestRecordBytes = 64 MiB`、§6.5 的 `kManifestRollBytes = 4 MiB`）。
- 每个新增类型都有**完整签名或明确字段表**（`VersionEdit` / `Version` / `VersionSet` 的增量 /
  `Compaction` / `Snapshot` / `PickStrategy` / `CompactionHook` / `CompactionStats` / `LevelStats` /
  `AmplificationStats` / `ManifestStats` / `RecoveryStats` 的增量 / `FakeClock` / `ClockEnv`）。
- **未验证项一律显式标注**：§0.6 的 7 条、§2.2 A11 的 `TableOptions` 收敛（"未落地"）、
  §10.1 的"依赖假设"列、§12.5 的 9 个薄弱点、§13 的全部拍板项。

### 12.2 内部矛盾（逐条核对）

| 潜在矛盾 | 处置 |
|---|---|
| `M4:28` 说"只允许单个后台 compaction 线程"，§2.1 D5 用**两个**后台线程（M3 flush + M4 compaction） | **不矛盾**：`M4:28` 的原文限定对象是 "compaction"（"跨层并发 compaction、subcompaction 全部禁止"）。M3 已交付的单后台 **flush** 线程（`D3:1224`、`D3:1379-1385`）不是"并行 compaction"；且 `M4:34` 明文允许追加"后台线程"（单数/复数未限）。§1.2 已把这条写进非目标的精确化 |
| `M4:112`（L27）说"flush 与 compaction **不得同时**修改 VersionSet"，§6.2 让两个线程并发运行 | **不矛盾**：改的是"**修改 VersionSet 这一动作**由 `install_mu_` 串行化"，不是"两个线程不能同时存在"。§6.2 的规则 1/2 把它写死；`M4:112` 的括号分支"（或按设计文档的固定优先级串行化）"正是本设计选的分支 |
| §3.4 说层内"不得共享任何 user key"，而 `M4:95` 说"允许端点相等" | **这是一个显式的收紧，不是矛盾**：§3.4 给出了 internal key 全序下的推导（端点相等 ⇒ 判据 (2) 必假），并说明"允许相等 ⇒ 层内无法判定谁更新 ⇒ 读出陈旧值"。已登记为 **§15 R5**，并进 §13 Q13 供用户确认 |
| `M4:37` 的合取 vs §5.5 的析取 | **已在 §2.2 A2 显式裁决**：以 `M4:98-99`（分列）为唯一判据，`M4:37` 只作摘要。`#1` 必须把 `M4:37` 标为"摘要" |
| `M4:36` 的字面顺序（每次安装都"写新 MANIFEST → 切 CURRENT"）vs §8.1 的两模式 | **已在 §2.2 A3 显式裁决**：字面顺序 = 模式 (a) 的收尾 + 模式 (b) 在同一新文件上继续追加。`#1` 必须把 `M4:190` 的评审项 3 改成"按模式核对" |
| §0.4 说 `Options` 里**没有** `block_size`/`verify_checksums`，§5.6 又要求 M4"复用"它们 | **不矛盾**：§0.4 是**实测的当前状态**（`src/common.h:241-250` 只有 4 个字段），§5.6 是**M3 已承诺交付**（`PR3:509` 把 `src/common.h` 列入 M3 的【必须改】）⇒ M4 复用 M3.2/M3.3 的交付结果。"尚未落地"已在 §0.4(b)/§2.2 A11 标清 |
| §4 的 patch 说 `META` 是"兼容读入格式"，`P` §10.8/§10.9 是冻结章节 | **不矛盾**：`P` 的 §10.8/§10.9 **字节布局与拒绝口径一个字都不改**，追加的只是"M4 起稳态不再写它"这一条**事实说明**（在 §11 的追加章节里）⇒ 符合 `P:3-4` 的"任何变更须回到 `#0` 设计阶段修订本文件并说明影响面" |
| §6.6 说"② 失败时内存 `version_` 不回滚"，§8.2 说"③ 失败后重启会多回放一条 edit" | **不矛盾且互相印证**：两者都是"先落盘后安装"的固有性质。③ 的"多一条"是无害的（`LogAndApply` 幂等：重放该 edit 得到的 Version == 内存里的 `new_version`） |
| §7.1 的 `files_checked` 上界 12 与 §10.2 `M4-B06` 的 `files_checked_max <= 12` | **一致**（同一个数字，由 `level0_trigger + 2 + (kNumLevels-1) = 4+2+6 = 12` 推得）；"2" 的来源是 `D3:1400` 的 `kMaxImmutableMemTables = 2`（在途 flush 的文件上界） |
| §5.3 说 `Version::files()` 的语义在 M4 变成"L0 的文件"，`D3:511-512` 说"不需要改 `files()` 的调用方" | **不矛盾**：M3 只有 L0 ⇒ `files()` 在 M3 下**就是**全部文件；M4 下 `files()` = `level_files(0)` = L0 ⇒ M3 的读路径调用方语义**自动**变成"遍历 L0"，正是 M4 要的第一步。已登记为"语义收窄"（§5.3） |
| §2.1 D6 说 MANIFEST 用 SSTable 块外壳的**形状**，`E3` 说"不复用 `BlockType` 枚举" | **不矛盾**：复用的是**帧形状与 CRC 覆盖面**（`P:248-280`），不是 `src/sstable/format.h` 的枚举值 ⇒ 不触碰 M3 冻结的枚举 |

### 12.3 歧义（逐条消解，给出唯一解释）

| 歧义点 | 本设计的唯一解释 |
|---|---|
| "L1 起始容量 10 MB，每层 ×10" | 精确化为 `MaxBytesForLevel(1) = max_bytes_for_level_base`（默认 10 MiB = `10u << 20`），`MaxBytesForLevel(l) = MaxBytesForLevel(l-1) × multiplier`（整数，饱和）；**L6 是最底层**（`kNumLevels = 7`），超限不再向下（§3.5） |
| "L0 文件数阈值默认 4" | 精确化为 `num_files(0) >= level0_file_num_compaction_trigger` ⇒ 触发；**score(0) = num_files(0)/trigger**（§3.5） |
| "选层打分"（`M4:191`） | 精确化为 §3.5 的两条 `score` 公式 + `argmax` + **平手取层号小者**（§5.4 的 `PickLevel`） |
| "轮转" | 精确化为"L0 取**文件号最小**（最旧）者作种子；L1+ 取 compact pointer 之后的**第一个**文件；指针越过末尾回到第一个"（§6.3 ①）。**不是**"随机轮转"或"按层号轮转" |
| "与下层重叠最小" | 精确化为"与 `level+1` 层文件的 **user key 区间重叠的字节数之和**最小"；**并列时** L0 取文件号最小者、L1+ 取 `smallest` 最小者（⇒ 唯一答案，§6.3 ①） |
| "输入集" | 精确化为"上层 = **传递重叠闭包**（X1）；下层 = 与闭包区间相交的**全部**文件"（§6.3 ②④） |
| "key 范围裁剪"（`M4:12`） | 精确化为"`MergingIterator` 只在输入文件上推进 ⇒ 结构上不可能输出区间外的 key"；**不**引入祖父层判据（`E7`）（§6.4） |
| "输出文件大小上限默认 2 MB" | 精确化为"**compaction 输出**的滚动阈值 `max_file_size`；判据是 `current_output_size >= max_file_size` **且**当前 entry 的 user key 与上一条不同"（X2）；flush 输出**不受**它约束（`E6`） |
| "最小快照 sequence" | 精确化为 `smallest_snapshot_ = snapshots_.empty() ? last_sequence_ : *snapshots_.begin()`；compaction 开始时**一次**取值（§7.2、§2.1 D4） |
| "低于最小快照 sequence" | 精确化为 `last_sequence_for_key <= smallest_snapshot`（"有更新的可见版本"）与 `ikey.sequence <= smallest_snapshot`（"该版本本身对快照可见"）两个**不同**的判据，分别用于旧版本与 tombstone（§5.5） |
| "更底层不存在更旧版本"（`M4:98` 的"该层及更底层"） | 精确化为 `IsBaseLevelForKey` 检查 `l ∈ [level + 2, kNumLevels)`（**不含** `level + 1`，因为它全在输入集里）（§5.5、X6） |
| "装进 Version" / "注册" | 精确化为"`LogAndApply` 成功返回"：即 MANIFEST 的 record 已 `fsync` **且** 内存 `version_` 已替换（§6.5） |
| "持久化"（`M4:36`） | 精确化为"CURRENT 指向的 MANIFEST 里存在一条 CRC 正确的完整 record"；**不等于**"掉电安全已证明"（§9.3 L28 的措辞纪律） |
| "重启后层级文件集合与崩溃前一致"（`M4:24`） | 精确化为"== **崩溃前最后一个成功持久化的 Version**，且未注册/半写输出被识别为孤儿并计数清理"（A8） |
| "两种策略"（`M4:23`） | 精确化为"同一层级布局下的 `轮转` vs `与下层重叠最小`"（A7）；**不含**分层布局的对照（A5 已定死为 leveled） |
| "写放大" | 精确化为 §10.3 的**两列**（主列含 compaction 自身、对照列不含）+ 分子分项（§2.1 D7） |
| "空间放大" | 精确化为 §10.3 的**两列**（主列含临时文件/MANIFEST/WAL、对照列只含 `.sst`） |
| "量级上的分离"（`M4:20`） | 精确化为 `p99_front_us * 10 <= p50_compaction_round_us` 且两者同时打印分母（A12） |
| "快照句柄" | 精确化为 `const Snapshot*`（含 `SequenceNumber sequence`），由 `PersistentDBImpl::GetSnapshot()` 返回、`ReleaseSnapshot()` 释放（A13） |
| "孤儿文件" | 精确化为 §8.5 的 a~g 七类（其中 b/c/e/g 是 M4 新增来源） |
| "META 迁移" | 精确化为"首次 `Open` 兼容读入一次 → 写 MANIFEST-`<n>` → 切 CURRENT → 删除 META；**稳态不写 META**"（`A9`/§3.6） |
| "层"这个词（`M3` 曾禁用，`D3:2102`/`D3:2112`） | **M4 起解禁**（§1.3.3）；实现概念就是 L0..L6，`kNumLevels = 7` |

### 12.4 范围越界（是否偷偷带了 M5/M6 内容）

逐条对照 §1.2 的禁列与 `RM:42`：

| 出现的东西 | 为什么不是越界 |
|---|---|
| `Options::compaction_pick_strategy` / `compaction_hook` | 都是 **M4 自己的**开放决策（`M4:68` 的两种 pick 策略、`M4:160` 的确定性注入点）的落点，不含任何 M5 特性 |
| `CompactionStats::bytes_read/bytes_written` | 是 §10.3 三个放大的**分子来源**（`M4:14`/`M4:22` 明确要求），不是 M5 的微基准设施（M5 的 `bench_lsm.sh` 是**独立程序**，`M5:12`） |
| `AMPL` 行里的 `read_index_blocks_read` / `read_data_blocks_read` | 沿用 `D3:553` 的 M3 `ReadStats` 字段。**M5 的"考虑/不考虑 filter 两列"（`M5:182`）不在本设计里**；§10.3 只声明"允许在行尾追加列"（这是给 M5 留的**扩展点声明**，不是实现） |
| `GetSnapshot` / `ReleaseSnapshot` / `GetAtSnapshot` / `NewIteratorAtSnapshot` | M4 的 I41（`M4:99`）判据**必须有**快照才能构造（`M4:70` 明确要求"快照句柄的生命周期与释放"）。**公共 `DB::GetSnapshot()` / `ReadOptions::snapshot` 不在本设计内**（A13）⇒ M5/M6 才考虑对外暴露 |
| `Snapshot` 值类型 | 只含 `SequenceNumber sequence`；**没有** `ReadOptions` / `WriteOptions` 的任何扩展 |
| `ManifestStats::rolls` / `kManifestRollBytes` | `M4:72` 要求"MANIFEST 损坏/截断的恢复策略 + 恢复时间"的取舍必须给出 ⇒ 周期性重建是这个取舍的**落点**；`NOTE:224`（M4-R6）明确建议规定它 |
| `EncodeTo`/`DecodeFrom` 的 `varint64` 用法 | 复用 `P` §2 的既有原语；`M4:63` 明确要求"确定 VersionEdit/MANIFEST 如何复用既有 Fixed/Varint/CRC 原语与 LengthPrefixed 编码" |
| `X1~X8` 的编号空间 | **不占 I/L 号**，正是为了不侵入 M5 的 `I47~I56 / L30~L35`（§9.1 末段） |

**明确声明：本设计不含** Bloom filter（`M5` 的内容）/ `WriteBatch` 公共 API / 压缩算法 / mmap /
**块缓存**（`BlockCache`）/ 多线程并行 compaction / subcompaction / trivial move /
祖父层重叠限制（`ShouldStopBefore`）/ seek 热度选文件 / 公共 `Snapshot*` API / `DB::Flush()` /
`ReadOptions` 的任何扩展 / raft-kv 对接 / `META` 的长期保留。

**"提前实现"的核对**：本设计的每个"不做"都有**显式登记**（`E7`/`E8`/`M4-B10`/D8 的块缓存/§5.4 的 trivial move），
且都写在 `docs/amplification.md` 或 `#1` 的接续清单里 ⇒ 不会出现"看起来能用但没实现"的字段
（`D3:462` 的纪律）。

### 12.5 本设计的已知薄弱点（主动暴露，供 `#4` 评审攻击）

1. **`kill -9` 不能验证"掉电安全"**（沿用 `D3:2127-2131` 与 `D3:2235` R4）：`kill -9` 不丢 page cache
   ⇒ `M4-B01/B02` 的 `missing 0` 只证明**进程级**一致性。真正的掉电语义（尤其"`CURRENT` 的 `rename`
   之后、`SyncDir` 之前掉电"）**只能**靠 `MemEnv` 近似；而 **`MemEnv` 不建模目录项**（`tests/memenv.h:49-52`
   逐字："内存 FS 没有目录项概念…`SyncDir` 只**计数**"）⇒ 本设计的 L28 在 A 组里**只能**验证"调用顺序"，
   **不能**验证"掉了会怎样"。这条**必须**写进 `#1` 的风险清单，且措辞**禁止**升级为"掉电安全已证明"。
   **这是本设计最明显的证据强度缺口。**
2. **`MANIFEST` 回放时间随编辑数线性增长**：本设计靠 `kManifestRollBytes = 4 MiB` 的周期性
   全量快照把它压回去，但**该阈值是估的、不是测的**（4 MiB 对应约 6~7 万条 edit 的全量快照规模，
   按 `D3:2132` 的同形估算 ≈ `F × 60 B` ⇒ 4 MiB ≈ 7 万个文件 —— 这个数字**没有实测**）。
   `M4-B09` 会给出实测曲线；若曲线不可接受，`#1` 应调整阈值或给"每次成功 `Open` 之后强制重建"的策略。
3. **`TableOptions` 与 `Options` 的收敛未落地**（`docs/m3-evidence.md:42-44` §4 第 1 条；§0.4(b) 实测）：
   本设计的 §5.6 **假定** M3.2 会收敛。若 M3.2 交付时仍独立，M4.1 必须顺手收敛 ⇒ 会动
   `src/sstable/table.h`（**不在** `M4:132` 的【必须改】清单里）⇒ 需要 §13 Q4 的明确授权。
4. **`install_mu_` 的引入是对 `M4:110` 的"落地解释"**：`M4:110`（L25）只说"先在锁内准备…锁外写…
   回锁后校验状态再安装"，它**没有**说明"两个线程并发时如何保证 MANIFEST 的 record 顺序与版本图顺序一致"。
   本设计用 `install_mu_` 解决，但这是一个**新增的同步原语**（M3 只有两把锁 + `bg_cv_`）⇒ 增加了
   一个死锁面。§9.4 的唯一全序是防线，但"锁序表 vs 代码"的一致性只能靠评审逐处核对。
5. **`Version::files()` 的语义在 M4 收窄**（从"全部文件"变成"L0 的文件"）：虽然 §12.2 论证了它
   与 `D3:511-512` 相容，但这是一个**容易被误读**的接口。若 `#4` 认为风险偏高，替代方案是
   **删掉 `files()`** 并强制所有调用方用 `level_files(0)`（代价：改 M3.2/M3.3 的调用点，
   但那些文件此刻尚不存在 ⇒ 现在改代价最低）。
6. **`kNumLevels = 7` 与"L6 是最底层"把空间放大全部压在最底层**：数据量超过 `MaxBytesForLevel(6)`
   （10 MiB × 10^5 = 1 TiB）时，L6 会无界增长且**没有**"再向下"的判据。本设计只给"计数上报"。
   对本项目的目标规模（100 万 key ≈ 108 MiB）不构成问题，但**必须**在 `#1` 里写明这条边界。
7. **X1（传递闭包）的实现成本可能被低估**：闭包展开的最坏情况是 O(F²)（每次加入都重扫）。
   L0 的 F 上界是 `trigger + 2 = 6`（`D3:1400`）⇒ 实际成本可忽略（≤ 36 次比较）。**但**若未来
   `level0_file_num_compaction_trigger` 被调大到几百，这个 O(F²) 会变成真问题。⇒ `#1` 应登记
   `level0_file_num_compaction_trigger` 的**合理上界**（例如 ≤ 64）作为输入校验项。
8. **`IsBaseLevelForKey` 的单调游标依赖"归并输出严格升序的 user key"**：若 `MergingIterator` 的
   输出出现"同一 user key 交错"（不应发生，`D3:1563-1581` 已冻结全序契约），游标会**静默**给出
   错误的 base-level 判定 ⇒ tombstone 被误丢。⇒ 必须有一条**断言**（`M4-A21` 的子断言：
   "归并输出的 user key 单调不减"）把它从静默错误变成显式失败。
9. **`FakeClock` 只能验证"逻辑"，不能验证"真实时间"**：`M4-A37`（饥饿探针）与 `M4-B07`（P99 门禁）
   是**两条不同的腿**：前者用假时钟保证零 flaky 的逻辑正确性，后者用真实时钟给性能数字。
   ⇒ 门禁数字（A12）只有后者能给；若 VM 上负载漂移（`NOTE:411` M5-R8 的教训：M2 实测"同一脚本、
   同一台 VM、不同时间点，结果可以差 3 倍"），`M4-B07` 必须打印 `load` 并把超阈值的行标 `UNRELIABLE`
   而不是静默采用。

### 12.6 需用户拍板清单（汇总）

见 §13。

---

## 13. 需用户拍板清单

> **纪律**：每条都标注"**用户已授权按推荐执行**"并写清**推荐值**（派工书原文："用户已授权：一律按推荐执行"）。
> "若否决"列说明改了之后**哪些章节要重写**（便于评估代价）。

| # | 问题 | 推荐值 | 若否决的代价 |
|---|---|---|---|
| **Q1** | 新增 I/L 号与 M3 冻结契约撞号（§2.2 A1） | **M4 = `I35~I46` / `L22~L29`；M5 顺延 `I47~I56` / `L30~L35`**；`#1`/`#2` 各附"指令原号 ↔ 落地号"映射表。**用户已授权按推荐执行** | 照字面号 ⇒ 与 `D3:1890-1893`/`PR3:824`（V8，已批准）双重占用，`M4:178` 的"逐条对照"失效；§9、§10 全部重编号 |
| **Q2** | 分层布局与 size-tiered 的处置（A5） | **L0 tiered + L1+ leveled（硬约束）；size-tiered 只作"结论作废"的负结果入档，不做实现分支**。**已授权** | 允许 tiered ⇒ `M4:95` 的"违反即拒绝安装"自身失效；§3.4、§5.4、§9.2 的 I37 重写 |
| **Q3** ★ | `META` 在 M4 的处置 + **对 M3.3 的前置要求**（A9） | **MANIFEST+CURRENT 取代 `META`；首次 `Open` 兼容读入一次后立即改写并删除；稳态不并存**；并把"`M3-A39/A40/A41` 的断言不得绑定 `META` 文件名"作为**对 M3.3 的前置要求**（此刻 M3.3 尚未写这些测试 ⇒ 代价为零）。**已授权** | 选"删除 META 路径"⇒ 必须改 M3 的既有测试断言（撞 `M4:134`）；选"双写并存"⇒ 保留 `D3:2132` 的缺陷 + 多一个损坏点 |
| **Q4** ★ | `Options` 扩字段 + `TableOptions` 收敛（A11、§0.4(b)） | **在 `src/common.h` 追加 §5.6 的 6 个字段**；**若 M3.2 未收敛，M4.1 必须完成 `TableOptions → Options` 的收敛**（会动 `src/sstable/table.h`）。**已授权** | 不收敛 ⇒ M4 的 compaction 要用两份块选项（判据漂移）；不许动 `common.h` ⇒ `M4:10`/`M4:12` 的 4 个可配项无处落 |
| **Q5** ★ | 快照 API 的落点（A13） | **不改 `src/db.h`**；`Snapshot` + `GetSnapshot`/`ReleaseSnapshot`/`GetAtSnapshot`/`NewIteratorAtSnapshot` 全部落在 `PersistentDBImpl`（沿用 `PR3:686` D9.4 的先例）。**已授权** | 加公共虚接口 ⇒ 必须同时改 M1 的内存 `DBImpl`（`src/db.cpp`）⇒ 违反"最小改动 M1"；且 `M4:132` 未授权 `src/db.h` |
| **Q6** ★ | 并发模型：一个还是两个后台线程（D5） | **两个**（M3 的 flush 线程 + M4 的 compaction 线程），用 `install_mu_` 串行化安装、**flush 固定优先**；配"饥饿探针" `M4-A37`。**已授权** | 单线程 ⇒ `M4:20` 的"P99 与单轮耗时量级分离"**结构上不成立**（写者在 compaction 期间停等）；§6.2、§6.5、§9.3 的 L27、§9.4 全部重写 |
| **Q7** | 块缓存是否引入（D8 / A4 的另一半） | **不引入**；三层收益由分层提供；在 `docs/amplification.md` 写一行 `M4-B10 NOT_APPLICABLE`。**已授权** | 引入 ⇒ 多一块独立设计面（LRU 键、淘汰粒度、与 `Table` 生命周期耦合），M4 的 scope 与风险显著上升 |
| **Q8** | B 组驱动与统计入口的落点（A14） | **复用** M2 的 `crash_writer`/`crash_recover`（不改）+ **新增** `scripts/lsm_compaction_stress.sh` 与 `scripts/lsm_level_stats.cpp`；统计入口全部在 `PersistentDBImpl`（**不动** `src/db.h`）。**已授权** | 新增成套驱动 ⇒ 两份 sidecar 协议漂移（M2 教训①）；改 `src/db.h` ⇒ 越权 |
| **Q9** | 两种策略 = 哪两种（A7） | **`轮转`（默认） vs `与下层重叠最小`**，运行时经 `Options::compaction_pick_strategy` 切换；**不含** seek 热度、**不含**分层布局对照。**已授权** | 拿未实现的 seek 热度对照 ⇒ 违反 `M4:167`；拿分层布局对照 ⇒ 违反 A5 |
| **Q10** ★ | tombstone/旧版本丢弃判据（A2） | **I40 与 I41 分列、析取（`drop_old_version \|\| drop_tombstone`）；`M4:37` 只作摘要**；判据单独成函数 + 双向构造性用例（`M4-A22~A27`）。**已授权** | 按合取 ⇒ 非最底层的旧版本永远丢不掉（空间放大爆炸）+ tombstone 的条件被写错 ⇒ **静默数据损坏的最高优先级红线** |
| **Q11** | MANIFEST 持久化的两种模式（A3） | **模式 (a) 新建/重建（写 MANIFEST → fsync → CURRENT.tmp → fsync → rename → SyncDir）** + **模式 (b) 常规追加（只追加 + fsync，不碰 CURRENT）**；`M4:190` 的评审项 3 改为"按模式核对"。**已授权** | 每次安装都切 CURRENT ⇒ 热路径上两次目录 fsync |
| **Q12** ★ | "量级分离"的可判定数字（A12） | **`p99_front_us * 10 <= p50_compaction_round_us`**，且 `FRONT`/`AMPL` 两行必须同时打印各自的分母；最大值只观测、不作门禁。**已授权** | 不落数字 ⇒ `M4:20` 不可判定；用最大值作分母 ⇒ 会被一次超大 compaction 主导，门禁失效 |
| **Q13** | 层内"允许端点相等"的收紧（§3.4 / §15 R5） | **收紧为"相邻两文件不得共享任何 user key"**（`largest.user_key < smallest.user_key` 严格）；输出侧只在 user key 边界切分（X2）。**已授权** | 照字面"允许端点相等" ⇒ 同一 user key 落在两个文件里 ⇒ 层内无法判定谁更新（`D3:1530` 已冻结"不用 max_sequence 判新旧"）⇒ 读出陈旧值 |
| **Q14** | `VersionEdit` 取代 M3 的 `AddFile(FileMetaData)` / `files()` 签名（§5.2） | **取代**（`AddFile(int level, ...)` + `added_files()/deleted_files()` + `Clear()`）；代价为零（`src/version_edit.*` 实测不存在）。**已授权** | 保留旧签名 ⇒ 两套形态（"看起来能用"）或需要 M3 的调用方适配 |
| **Q15** | M4.2 的"读放大改善证据"是 A 组还是 B 组（A15） | **两条腿都要**：M4.2 用 A 组的**结构性上界断言**（`M4-A19`）、M4.3 用 B 组的**实测行**（`M4-B06`）。**已授权** | 只等 B 组 ⇒ `M4:172` 的 M4.2 门禁过不了；只有 A 组 ⇒ 无实测数字 |
| **Q16** | "不做"清单（trivial move / 祖父层限制 / seek 热度 / compact pointer 持久化 / 块缓存） | **全部不做**，逐条显式登记 + 去向（`E5`/`E7`/`E8`/§5.4/D8）。**已授权** | 做了会扩 scope 并违反 `M4:167`；不登记则被误认为"遗漏" |
| **Q17** | `M3-B01~B05` 的前置依赖（§10.2 末段、§0.6 第 1 条） | **M4 的 B 组以 M3 的 B 组交付为前提**；`#1` 必须把 §10.2 的六条**可粘贴**前置检查命令写进"开工门"，未过门不得宣称 M4.3 判据成立。**已授权** | 无前置检查 ⇒ M4 的 `missing 0` 可能只是 M3 门禁的复制品（`D3:1843` G3 的空绿教训） |

---

## 14. 参考与引用

| 出处 | 用途 |
|---|---|
| `M4-分层Compaction.md` §0~§5（214 行） | 本阶段任务书：7 目标（`:9-16`）、7 验收口径（`:18-25`）、非目标（`:27-28`）、7 条硬性约束（`:30-40`）、8 条开放决策（`:66-74`）、`#0` 的探查与产物要求（`:57-88`）、`#4` 评审的 9 项（`:187-196`）、固定列与负结果纪律（`:200-206`）、工程约定（`:208-214`） |
| `docs/m3-design.md`（2264 行） | **体例模板** + M4 的**全部既有契约**：§0 探测范式（`:13-323`）、§2 D1~D8（`:447-554`）、§3.1 命名（`:563-582`）、§6 flush/轮转（`:1209-1503`）、§7 读路径与 `MergingIterator`/`DBIter`（`:1504-1640`）、§8 版本元数据与恢复（`:1642-1866`，含 E1~E8 的 `:1852-1866`）、§9 不变量与锁纪律（`:1869-1920`）、§10 测试矩阵（`:1923-2013`）、§11 子里程碑（`:2015-2055`）、§12 自检（`:2059-2152`）、§13 需拍板（`:2155-2172`）、§14 参考（`:2174-2194`）、§15 修订记录（`:2196-2264`，含 **R4** 的 `SyncDir` 措辞纪律 `:2231-2236`、**R5** 基线重钉 `:2238-2242`、**R6/R6-e** 偏离登记 `:2244-2264`） |
| `docs/m3-evidence.md`（57 行） | M3.1 的实测判据（`:23-30`：`102 tests`、0 warning、ASan/TSan、零依赖计数）、`:41-54` §4 的 **9 条未闭合项**（尤其第 1 条 `TableOptions`、第 2 条随机读推迟、第 9 条 `M3-B01~B05` 未交付） |
| `docs/m3-prerequisites.md`（85 K 字节） | 契约纪律：`:52` §1.2 的非目标清单（"`ReadOptions::snapshot`/`Snapshot*` 公共 API"）、`:382` 的 P6（`FlushHook` 的必要性）、`:509` §7 的【必须改】清单（含 `src/common.h` 与 `filename.*`）、`:824` §11.4 的 **V8**（I31~I34/L19~L21 的收录与"已批准"状态）、`:612-740` §9 的测试套件缺陷登记（**D9.4 的"11 处测试直接依赖 `PersistentDBImpl`"**、**D9.5 的 `MemEnv` 不建模目录项**、**D9.6 的正向标记缺口**） |
| `docs/protocol.md`（377 行） | 复用 `§1`（LE）`:9`、`§2`（varint）`:18`、`§3`（Fixed）`:29`、`§4`（Length-Prefixed）`:36`、`§5`（CRC32C）`:43`、`§6`（内部 key 与降序比较）`:54`、`§9`（WAL record 与 CRC 覆盖面）`:139`、`§10`（SSTable）`:206`；**冻结声明在 `:3-4`**；`§4` 给出**追加 §11 的完整 patch 文本**（本文 §4）；`§10.3` 的块外壳（`:248-280`）是 MANIFEST record 的形状来源；`§10.8`/`§10.9`（`:346-377`）是 `META` 的兼容读格式与安全阀 |
| `docs/m2-design.md`（1386 行） | 上游契约：`I11~I20`（`:97-106`）尤其 **I11**（durable-before-ack）与 **I17**（持锁零 IO）、`L7~L12`（`:107-112`）尤其 **L8**（锁序 `commit_mu_ → mutex_`）与 **L11**（关闭顺序）、`§11.2` 的 fsync 实测、`§11.3` 的 `kill -9` 实测 |
| `docs/roadmap.md`（75 行） | 依赖方向（`:8-26`）、M4 的目标与判据（`:35`）、**阶段硬边界**（`:42`"M4：不得出现 Bloom/Batch"）、`非目标`是硬边界（`:44`）、工程约定（`:52` 提交粒度、`:54` tag、`:55` 未跑不算过、`:57` 三构建目录、`:59` 0 warning、`:60` 不吹、`:61` 负结果入档） |
| `D:\lsm\mine\m4m5\notes.md`（459 行） | 侦察笔记：M4 的要求/清单/判据整理（`:1-110`）、与 M3 的关系（`:111-132`）、**11 条矛盾与推荐裁决**（`:134-213`）、风险与建议（`:215-224`）、跨里程碑前置条件（`:427-435`）、推荐裁决摘要（`:437-449`） |
| `src/common.h:241-250` | `Options` 的实测形态（4 个字段） |
| `src/filename.{h,cpp}` | 命名与编号的实测形态（只有 `.log` 一族） |
| `src/util/env.h` | `Env` 的能力边界（`SyncDir`/`RandomAccessFile` **已落地**，`:41-48`/`:58`/`:75`） |
| `src/db_impl.h` | M2 的持久化实现形态（两把锁、`RecoveryStats`、`DbMutexHeldOnThisThread` 探针） |
| `src/db.h` | `DB` 的公共接口（M4 **不动**它，A13/A14） |
| `src/sstable/table.h:11-12,36-39` | `TableOptions` 的**实测偏离**（与 `Options` 未收敛） |
| `tests/memenv.{h,cpp}` | 崩溃语义的**唯一** seam（fsync 水位 + 固定种子撕裂）及其**目录项不建模**的缺口（`memenv.h:49-52`） |
| `tests/test_harness.h` | 固定种子生成器（`:57`）、`std::map` 对账助手（`:10-11`）、**手工拼字节参照**纪律（`:12-14`） |
| `scripts/lsm_crash_test.sh:51-70`、`scripts/crash_{writer,recover}.cpp`、`scripts/lsm_gate.sh:60-122` | B 组的固定行格式、sidecar 协议、正向标记 AND 判定的**实测形状**（M4 逐字沿用） |
| `CMakeLists.txt:46-140` | 目标清单与 `lsm_sstable` 的**零依赖机制**（M4 照做 `lsm_version`） |

---

## 15. 修订记录（相对指令原文的裁决与偏离）

> 本节记录本设计对 `M4-分层Compaction.md` 的**裁决与偏离**。任一条都不得被后续实现当作"设计原文"绕过。
> （`D3:2196-2199` 的同形声明。）

### R1 —— 不变量与锁纪律的编号全部顺延（`M4:92-114` → `I35~I46 / L22~L29`）

- **原指令**：新增 `I31~I42`、`L19~L26`。
- **实际**：与 M3 已批准的 `I31~I34 / L19~L21`（`D3:1890-1893`/`D3:1908-1910`、`PR3:824` V8）**双重占用**
  ⇒ 落地为 `I35~I46 / L22~L29`，M5 顺延 `I47~I56 / L30~L35`。
- **依据**：§2.2 A1（`M4-C1`）。**影响面**：§9.1 的映射表是唯一权威；`#1` 必须附同表。

### R2 —— 持久化纪律拆成两个显式模式（`M4:36`/`M4:190`）

- **原指令**："写新 MANIFEST 文件 → fsync → VersionEdit 追加 + fsync → CURRENT 原子切换（临时文件 +
  fsync + rename + fsync 目录）"，且 `M4:190` 逐字重复。
- **实际**：拆成**模式 (a) 新建/重建**与**模式 (b) 常规追加**（$8.1）；`M4:36` 的字面顺序解释为
  "(a) 的收尾 + (b) 在同一新文件上继续追加"；CURRENT **只在模式 (a) 切换**。
- **依据**：§2.2 A3（`M4-C3`）。**影响面**：`#1` 必须把 `M4:190` 的评审项 3 改为"按模式核对"。

### R3 —— `META` 被 MANIFEST+CURRENT 取代，且对 M3.3 提出前置要求

- **原指令**：全文（含 `M4:35` 的格式冻结清单）**没有**说明 `META` 的处置。
- **实际**：`MANIFEST + CURRENT` **取代** `META`；首次 `Open` 兼容读入一次后立即改写并删除；
  **不允许稳态并存**（§3.6）。
- **额外成果（必须拍板）**：把"`M3-A39/A40/A41` 的断言不得绑定 `META` 文件名"登记为**对 M3.3 的前置要求**
  —— 理由：这三条用例**尚未落地**（`docs/m3-evidence.md:54` §4 第 9 条），此刻裁决代价为零；
  若 M3.3 先写成绑定文件名，M4 的迁移就会撞 `M4:134`（"禁止改动 M1~M3 既有测试的断言"）。
- **依据**：§2.2 A9（`M4-C9`）+ §13 Q3。**影响面**：§3.6、§4 的 §11.6、§8.3、§10.1 的 `M4-A39`、§12.4。

### R4 —— `Options` 扩字段（含 `src/common.h`）与 `TableOptions` 的收敛

- **原指令**：`M4:132` 的【必须改】**未列** `src/common.h`；但 `M4:10`/`M4:12` 的 4 个可配项无处落。
- **实际**：允许追加 §5.6 的 6 个字段；并**额外**裁决"若 M3.2 未把 `TableOptions` 与 `Options` 收敛，
  M4.1 必须收敛"（会动 `src/sstable/table.h`）。
- **依据**：§2.2 A11（`M4-C11`）+ `docs/m3-evidence.md:42-44` §4 第 1 条 + §13 Q4。

### R5 —— L1+ 层内"允许端点相等"被**收紧**为"不得共享任何 user key"

- **原指令**：`M4:95`"按 user key 区间互斥，**允许端点相等但不得覆盖**"。
- **实际**：收紧为 `f_i.largest.user_key < f_{i+1}.smallest.user_key`（**严格**），
  并在 §3.4 给出 internal key 全序下的推导：端点相等 ⇒ 同一 user key 落在两个文件里 ⇒
  层内无法判定谁更新（`D3:1530` 已冻结"不用 key range/max_sequence 判新旧"）⇒ 读出陈旧值。
- **依据**：§3.4、§12.2、§13 Q13。**影响面**：§6.4 ③ 的切分规则（X2）与 `M4-A17`/`M4-A32` 是用例落点。

### R6 —— `VersionEdit` **不引入** `last_sequence` 与 `compact_pointer` 字段

- **原指令**：未提；`M4:63` 只要求"确定 VersionEdit/MANIFEST 如何复用既有原语"。
- **实际**：`last_sequence` **不定义**（`D3:1724-1731` 的 §8.2 纪律 1/2 已否决 edit 级水位，
  引入它等于把已被否决的"丢数据"模式请回来）；`compact_pointer` **不定义**（重启后从各层最左重新开始，
  只多做 compaction，不影响正确性）。
- **依据**：§2.4 E4/E5。**影响面**：§3.3 的 tag 表（只有 6 个 tag）、§4 的 §11.3。

### R7 —— 并发模型：**两个后台线程** + `install_mu_` 串行化（偏离 `NOTE:219` 的"同一线程"建议）

- **`NOTE` 的建议**（M4-R1）："复用 M3 的**同一个**后台线程与 `bg_cv_`，把 flush 与 compaction 排成一个队列
  且 flush 优先"。
- **实际偏离**：改为"**M3 的 flush 线程 + M4 的 compaction 线程**（两个），安装用 `install_mu_` 串行化，
  flush 固定优先"。
- **理由**：`NOTE` 自己的建议**承认**了饥饿风险但未给判据；若真的复用同一线程，一次 compaction 期间
  `immutables_` 无人消化 ⇒ `kMaxImmutableMemTables = 2`（`D3:1400`）被填满后写者停等**整个 compaction 时长**
  ⇒ **`M4:20` 的"P99 与单轮耗时量级分离"结构上不成立**。两个线程 + 短临界区安装是唯一能让 `M4:20`
  可达成且不违反 `M4:28`（"只允许单个后台 **compaction** 线程"）的形态。
- **依据**：§2.1 D5、§6.2、§9.3 的 L27、§9.4、§13 Q6。
- **本偏离的代价（诚实登记）**：新增一个同步原语（`install_mu_`）⇒ 增加一个死锁面（§12.5 第 4 条）。

### R8 —— 块缓存**不引入**；`TableCache` **复用** M3 的（不新建 `src/table_cache.*`）

- **原指令**：`M4:16`/`M4:74` 把"块缓存（LRU）与 table cache"写成**可选**；`M4:133` 列
  【可选扩展】`src/table_cache.{h,cpp}`。
- **实际**：`TableCache` 复用 M3（`D3:1687`，类在 `version_set.h`）；**块缓存不引入**，
  并在 `docs/amplification.md` 写一行 `M4-B10 NOT_APPLICABLE`（**不静默省略**）。
- **依据**：§2.2 A4（`M4-C4`）、§2.1 D8、§13 Q7。

### R9 —— 快照 API 落在 `PersistentDBImpl`，**不改** `src/db.{h,cpp}`

- **原指令**：`M4:70` 要求"快照句柄的生命周期与释放"；`M4:132` 对 `src/db.*` 只写"**如需**…则最小扩展"。
- **实际**：`Snapshot` 值类型 + 四个方法全部落在 `db_impl.h` 的 `PersistentDBImpl` 上；
  公共 `DB::GetSnapshot()` / `ReadOptions::snapshot` **不提供**（登记为去向）。
- **依据**：§2.3 A13、§12.4、§13 Q5、`PR3:52`（`Snapshot*` 公共 API 属非目标）+ `PR3:686`（D9.4 的先例）。

### R10 —— B 组驱动与统计入口的落点（**对 `M4:34` 文件清单的必要追加**）

- **原指令**：`M4:34` 只列 `scripts/lsm_compaction_stress.sh`，没列 C++ 驱动；`M4:170` 要求
  "是否需要 status/日志命令"被明确。
- **实际**：**复用** M2 的 `lsm_crash_writer`/`lsm_crash_recover`（不改）；**新增**
  `scripts/lsm_compaction_stress.sh` 与 `scripts/lsm_level_stats.cpp`；统计入口落在 `PersistentDBImpl`。
- **依据**：§2.3 A14、§10.2、§11 的 M4.2/M4.3、§13 Q8、`M4:182`（允许 `CMakeLists.txt` 最小改动）。

### R11 —— MANIFEST record 采用"SSTable 块外壳同形"的帧，而不是 WAL 的物理 record

- **原指令**：未规定；`M4:63` 只要求"复用既有 Fixed/Varint/CRC 原语与 LengthPrefixed 编码"。
- **实际**：`length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)`，CRC **含长度**；`type` 用
  MANIFEST 自己的枚举空间（**不**复用 `src/sstable/format.h` 的 `BlockType`）。
- **理由**：① 复用"含长度的 CRC"纪律（`P:478-489` §9.3 已否决"间接推断"）；
  ② 不引入 WAL 的 32 KiB 跨块状态机与逻辑 record 上限；③ 不触碰 M3 冻结的枚举。
- **依据**：§2.4 E3、§3.2、§4 的 §11.2。

### R12 —— `M4:20` 的"量级分离"落成数字；`M4:172` 的"读放大改善"落成两条腿

- **原指令**：`M4:20`（"量级上的分离"）、`M4:172`（M4.2 判据含"读放大统计有改善证据"）、
  `M4:163`（把它放在 B 组）。
- **实际**：① 门禁数字 = `p99_front_us * 10 <= p50_compaction_round_us` 且同时打印分母（A12）；
  ② 读放大的"改善证据"**两条腿**：M4.2 用 A 组的**结构性上界断言**（`M4-A19`），
  M4.3 用 B 组的**实测行**（`M4-B06`）（A15）。
- **依据**：§2.2 A12、§2.3 A15、§10.1 的 `M4-A19`、§10.2 的 `M4-B06/B07`、§13 Q12/Q15。

### R13 —— 补充约束 `X1~X8` 建立**独立的编号空间**（不占 I/L 号）

- **原指令**：无此概念。
- **实际**：把 8 条"承载正确性但不属于指令列举的不变量"的约束编为 `X1~X8`（§9.5），
  **刻意不占用 `I47+` / `L30+`**，以保证 M5 的号段（`I47~I56 / L30~L35`）仍然可用。
- **依据**：§9.1 末段、§9.5、§12.4。

### R14 —— 基线重钉与本设计的"未落地"标注纪律

- **`NOTE` 的基线**：VM `9154c5f` + 工作区脏（`notes.md:6-11`）。
- **实测基线**：VM 与本机克隆均为 **`e56d0b7` 且干净**（§0.2）⇒ 本设计的一切引用以 `e56d0b7` 为准。
- **未落地标注**：`src/version_set.*` / `version_edit.*` / `merging_iterator.*` / `db_iter.*` 与
  `M3-B01~B05` **全部尚未交付**（§0.3、`docs/m3-evidence.md:54`）⇒ 本设计对它们的接口一律标
  "【M3.2 承诺】/【未落地】"，并在 §11 设立 **M4.0** 的"开工前置复核清单"（15 项，含可粘贴命令）
  逐一以**真实签名**校对本设计的草案，不一致处以落地实现为准并登记差异（派工书的裁决）。
- **`SyncDir` 的措辞纪律**：沿用 `D3:2235`（R4）——**禁止**写"掉电安全已证明"，只写"调用了 `SyncDir`
  且顺序位于 `rename` 之后"（§9.3 的 L28、§12.5 第 1 条）。

### R15 —— 与 `M4-C12`/`M4-C13` 的编号约定（避免与 `NOTE` 的字段撞名）

- 本文件用 **`A1~A15`** 指代裁决条目（`A1~A11` = `NOTE` 的 `M4-C1~C11`；`A12` = `M4:20` 的补充裁决；
  `A13~A15` = 本轮新发现）；用 **`D1~D8`** 指代 `M4:67-74` 的 8 条指令决策；用 **`E1~E10`** 指代
  指令外的补充决策；用 **`X1~X8`** 指代补充约束。
- **不**沿用 `M4-C12`/`M4-C13` 的命名（`NOTE` 的编号空间只到 `C11` + 1 条补充裁决），
  以避免与 `NOTE` 的既有编号语义混淆。`#1` 若要引用本轮的新发现，用 `A13/A14/A15`。



### R16 —— `DB::Open` 的 WAL-only 安全重建回退（M6.10.1 修复）

- **原指令**：无；M4 的 §3.6 / `P` §11.6 原文规定“两者都不存在且目录非空 ⇒ `kCorruption`”。
- **实际**：`VersionSet::RecoverManifest()` 的该分支逐字不变；`DB::Open` 在它返回该错误后新增一个
  可证明安全的回退：当没有 `CURRENT` / `META` 及其 tmp，且所有 WAL 能从 sequence 1 连续解析并覆盖
  所有孤儿 SST 的 `max_sequence` 时，按空 Version + WAL 重放打开，并让未引用 SST / MANIFEST 走孤儿清理。
- **理由**：修复 `flush` 与 `Close` / `kill -9` 竞态在首个 flush 中留下“有 SST、无 CURRENT/MANIFEST”的
  不可恢复库；同时保留“WAL 不完整 / 不覆盖 SST 时必须拒绝”的安全阀。未采用“DB 创建即写 MANIFEST”或
  “flush 在 SST 前先写 MANIFEST”的方案，因为会改变既有磁盘可见时序 / `manifest_present` 契约并与
  M3/M4 的冻结用例冲突。
- **依据**：M6.10.1 最小复现与门禁；新增确定性用例
  `Flush.FirstFlushCloseWindowRecoversFromCompleteWal`。
- **影响面**：`src/db_impl.cpp` 恢复路径；`docs/protocol.md` §11.7（纯追加）；不改变 MANIFEST 记录格式、
  CURRENT 语义、`I32/I43/L24` 或既有 `kCorruption` 触发条件（仅在其之前增加证明安全的重建）。
