# M5 前置复核与差异登记（docs/m5-prerequisites.md）

> 产出阶段：**M5.0**（`docs/m5-design.md` §11 的 M5.0；照 `docs/m4-design.md` §11 的 M4.0 先例）。
> 本文件**不写业务实现代码**；它只登记「设计草案 vs 落地实现」的逐条复核结果与差异。
> 纪律：`docs/m5-design.md`/`docs/m4-design.md`/`docs/m3-*.md`/`docs/m3-evidence.md` 一律不改；
> 差异一律以**落地实现**为准（`docs/m5-design.md` §11 M5.0 判据 3/6）。

---

## §0 前置复核（M5.0 的阻断条件与基线的原始证据）

### §0.0 可粘贴复核命令

以下命令**全部**在 VM `~/lsm-kv` 只读执行（M5.0 期间未跑任何构建/测试；构建与测试属 M5.1）：

```bash
cd ~/lsm-kv
git rev-parse HEAD; git status --porcelain; git tag; git log --oneline -5; git diff --stat
git show HEAD:src/compaction.cpp | grep -n "Status Compaction::Run"
git show HEAD:src/compaction.h  | grep -n "class Compaction\|static Status Run"
git show HEAD:src/db_impl.cpp | grep -n "Status PersistentDBImpl::GetInternal\|Status PersistentDBImpl::Write(\|EncodeGroup\|RunFlusher\|Compaction::Run"
git show HEAD:src/db_impl.h   | grep -n "struct Pending\|struct DbReadStats\|class PersistentDBImpl\|Status Write("
git show HEAD:src/version_set.h  | grep -n "class TableCache\|Status Get(\|NewIterator"
git show HEAD:src/version_set.cpp | grep -n "TableCache::Open\|TableCache::Get"
git show HEAD:src/sstable/table.h | grep -n "struct ReadStats\|class Table\|GetEntry\|ParseMetaIndexBlock\|static Status Open"
git show HEAD:src/sstable/table_builder.h | grep -n "class TableBuilder\|Status Add\|Status Finish"
git show HEAD:src/sstable/format.h | grep -n "kBlockTypeFilter\|kTableFormatVersion\|kFooterSize\|kBlockMinPayload"
git show HEAD:docs/protocol.md | grep -n "^### 9\.4\|^### 10\.6\|^### 10\.7\|^## 11"
git show HEAD:docs/m3-design.md | grep -n "BuiltinBloomFilter2\|kTableFormatVersion 不升\|M5 加 Bloom\|M5 的 filter"
git show HEAD:src/common.h | grep -n "struct Options\|bloom\|block_size\|verify_checksums\|max_open_files\|level0_file"
git show HEAD:src/db.h | grep -n "struct WriteOptions\|virtual Status"
git show HEAD:src/wal.h | grep -n "kMaxLogicalRecordSize\|class WALWriter"
git show HEAD:src/memtable.h | grep -n "WouldReject\|Status Add\|kFrozen"
git show HEAD:docs/m4-design.md | grep -n "M5 的顺延\|I47~I56\|L30~L35\|只允许在行尾追加\|M5 要加"
grep -n "run_gate_marked\|run_gate_m3_marked\|require-m3\|PARTIAL" scripts/lsm_gate.sh
grep -rn "FakeClock" tests src
grep -n "class CountingEnv" tests/sstable_counting_env.h
grep -n "class FaultyEnv"  tests/faulty_env.h
ls -la docs/m5-prerequisites.md docs/m5-bench.md 2>&1
ls -la scripts/bench_lsm.sh scripts/lsm_m5_unit_test.sh scripts/lsm_batch_crash_test.sh 2>&1
grep -h "TEST(" tests/*.cpp | wc -l
git show HEAD:src/db_impl.h | grep -n "AmplificationStats\|LevelStats\|FormatAmplLine"
git show HEAD:src/db_impl.cpp | grep -n "FormatAmplLine\|GetAmplificationStats\|GetLevelStats"
```

### §0.1 基线 rev / 工作区 / tag（**M4 已收口**）

| 项 | 命令 | 落地值 | 判据 |
|---|---|---|---|
| HEAD | `git rev-parse HEAD` | `fa7c328760f8b6c1ed6ff8d64c3974f127b2fb33`（短 `fa7c328`） | == 派工书基线 ✅ |
| 工作区 | `git status --porcelain` | **空**（无 `M src/db_impl.*`） | 空 ⇒ M4 已收口 ✅ |
| tag | `git tag` | `m1-memtable` / `m2-wal` / `m3-sstable` / `m4-compaction` | 四个 tag 全在 ✅ |
| diff | `git diff --stat` | **空** | 无未提交改动 ✅ |
| 最近 5 提交 | `git log --oneline -5` | `fa7c328`(M4 B01/B02 kill -9) → `d280a9a`(A31/A35) → `26a1a0d`(DBIter 跨 child) → `86d5c48`(181→182) → `5a5175a`(180→181) | 历史线性、M4.3 已提交 ✅ |
| 用例数 | `grep -h "TEST(" tests/*.cpp \| wc -l` | **185** | 与派工书一致；M5 只增不减 ✅ |

**结论（判据 1）**：M4 已收口 ⇒ **M5.1 可以开工**，`M5-design` §12.6 Q11 的阻断条件不成立。

**结论（判据 2）**：工作树干净 ⇒ 不进入「暂停等 M4.3」分支。

### §0.2 逐条复核结果（设计草案 §3.5/§3.6/§5/§9.2 引用的符号）

**① `Compaction::Run`（M5 基准要在真实 compaction 上跑）** — 落地签名（`src/compaction.h:70` 声明 / `src/compaction.cpp:201` 定义）：

```cpp
static Status Run(Env* env, TableCache* tc, const std::string& dbname, const CompactionInputs& in,
                  const Options& o, const Version& version, SequenceNumber smallest_snapshot,
                  const std::function<uint64_t()>& alloc_file_number,
                  const InternalKeyComparator& icmp, VersionEdit* edit, CompactionStats* stats,
                  std::string* why);
```

差异：设计草案写成 `(env, tc, dbname, in, options, *base, …)`；**落地是 `const Version& version` 而非 `Version*`**，
且多出 `alloc_file_number` / `icmp` / `stats` / `why` 四个参数。⇒ **以落地为准**（M5.1 不改它）。

**② 读路径层级化 + 组提交实际形态（M5 的接入点）**：

| 符号 | 落地位置 | 落地签名 |
|---|---|---|
| `PersistentDBImpl::Write` | `src/db_impl.cpp:146` / `src/db_impl.h:293` | `Status Write(ValueType type, const WriteOptions& options, const Slice& key, const Slice& value)` |
| `EncodeGroup` | `src/db_impl.cpp:228` | `static std::string EncodeGroup(SequenceNumber begin, const std::vector<Pending*>& members)` |
| `RunFlusher` | `src/db_impl.cpp:257` | `Status RunFlusher()` |
| `GetInternal` | `src/db_impl.cpp:464` | `Status GetInternal(const Slice& key, SequenceNumber snapshot, std::string* value, DbReadStats* delta)` |
| `MergeReadStats` | `src/db_impl.cpp:566` | `void MergeReadStats(const DbReadStats& delta)`（在 `mutex_` 下累加） |
| `Pending` | `src/db_impl.h:396` | `struct Pending { bool need_sync; bool done; Status status; SequenceNumber begin; ValueType type; std::string key; std::string value; size_t entry_bytes; }` |

**③ TableCache / Table / ReadStats / format 的实际签名**：

| 符号 | 落地位置 | 签名要点 |
|---|---|---|
| `class TableCache` | `src/version_set.h:195` | `Get(f, lookup_key, value, result, ReadStats* stats = nullptr, bool* opened = nullptr)`（声明 `:202`）；私有 `Open(number, smallest, largest, out, opened)`（声明 `:206`，定义 `version_set.cpp:897`） |
| `TableCache::Get` | `src/version_set.cpp:944` | 内部 `Open(...)` 后调 `table->GetEntry(lookup_key, value, result, stats)` |
| `struct ReadStats` | `src/sstable/table.h:42` | 8 列：`files_checked / key_range_skipped / index_blocks_read / data_blocks_read / blocks_read / bytes_read / crc_checked / crc_failed`（**无 filter 列** ⇒ M5.1 只追加） |
| `class Table` | `src/sstable/table.h:67` | `static Status Open(const TableOptions&, Env*, const std::string& filename, std::shared_ptr<Table>*, const std::string* known_smallest = nullptr, const std::string* known_largest = nullptr)`（**无 `ReadStats* open_stats`** ⇒ M5.1 追加默认参数） |
| `Table::GetEntry` | `src/sstable/table.h:87`，实现 `table.cpp:259` | `Status GetEntry(const Slice& lookup_key, std::string* value, TableGetResult* result, ReadStats* stats = nullptr) const` |
| `Table::ParseMetaIndexBlock` | `src/sstable/table.h:131`，实现 `table.cpp:143` | `Status ParseMetaIndexBlock(const Slice& payload)`（**当前把所有条目都算 unknown**） |
| `Table::ReadBlock` | `src/sstable/table.h:118`，实现 `table.cpp:199` | `Status ReadBlock(const BlockHandle&, BlockType, std::string*, ReadStats*) const`（CRC 受 `options_.verify_checksums` 控制） |
| `class TableBuilder` | `src/sstable/table_builder.h:27` | `Status Add(const Slice& key, const Slice& value)`（`:36`）、`Status Finish()`（`:40`）、私有 `WriteBlock(BlockType, const Slice&, BlockHandle*)`（`:57`） |
| `format.h` 常量 | `src/sstable/format.h` | `kBlockTypeData=0x01/Index=0x02/MetaIndex=0x03/Filter=0x04`（`:24-29`）；`kBlockOverhead=9`（`:33`）；`kBlockMinPayload=8`（`:35`）；`kFooterSize=44`（`:44`）；`kTableFormatVersion=1`（`:45`） |
| `TableOptions` | `src/sstable/table.h:39` | `using TableOptions = Options;`（M3.2 起的别名，M5.1 沿用） |

**④ `Options` / `DB` / `WAL` / `MemTable` 的实际形态**：

- `struct Options`（`src/common.h:285`）：有 `block_size=4096`（`:295`）、`verify_checksums=true`（`:296`）、`max_open_files=64`（`:298`）、
  M4 的 6 字段（`:305-310`）、`manifest_roll_bytes`（`:313`）；**无 `bloom_bits`**（`grep -n bloom src/common.h` 零命中）
  ⇒ 与设计判据 5 一致，M5.1 落地。
- `src/db.h`：`struct WriteOptions{ bool sync; }`（`:17`）；`virtual Status Put/Delete/Get/NewIterator/Sync/Close`（`:32-46`）；
  **无 `Write(const WriteOptions&, WriteBatch*)`** ⇒ 与设计判据 5 一致，M5.2 落地。
- `src/wal.h`：`kMaxLogicalRecordSize = 64u*1024*1024`（`:23`）；`class WALWriter`（`:48`）。
- `src/memtable.h`：`Status Add(SequenceNumber, ValueType, const Slice&, const Slice&)`（`:49`）；`bool WouldReject(size_t extra_bytes)`（`:64`）；`kFrozen`（`:48-49`）。

**⑤ 协议章节与 metaindex 预留**：

- `docs/protocol.md`：`### 9.4 WAL batch payload 编码`（`:185`）、`### 10.6 元数据块 payload`（`:317`，逐字写「M5 的 Bloom filter
  通过 `name = "filter.leveldb.BuiltinBloomFilter2"` 指向 `kBlockTypeFilter` 块；footer 布局不变、`kTableFormatVersion` 不升」）、
  `### 10.7 footer`（`:329`）、`## 11. MANIFEST / VersionEdit 编码（M4 定稿）`（`:379`，文件末尾为 §11.6）。
  ⇒ **追加 §12/§13 的编号空间可用**（M5.1 落 §12，M5.2 落 §13）。
  - 复核时 `docs/protocol.md` 的 **sha256 = `60282bb3ec3e6e4fb577f2bf7fc57943703144d554bd4c0544ad5b03cafc125f`**
    （追加 §12 前的基线，供 M5.1 的「只追加」证据）。
- `docs/m3-design.md` 的 metaindex 预留：`:471`（推荐预留，footer 零改动、版本不升）、`:656`（§3.4 元数据块与 M5 的 filter 预留）、
  `:664`（「M5 加 Bloom 时：metaindex 多一条 `"filter.leveldb.BuiltinBloomFilter2"` → filter 块的 handle」）、`:902`（filter 块类型）。

**⑥ M4 设计对 M5 的预留**：

- `docs/m4-design.md:345` / `:1951-1953` / `:2626`：**M5 顺延 `I47~I56` / `L30~L35`**；M4 的 X1~X8 刻意不占 I/L 号。
- `docs/m4-design.md:461` / `:2142`：`AMPL`/`LEVEL`/`FRONT` 三行「**只允许在行尾追加新列**（M5 要加 filter 列）」。
  - 落地核对：`FormatAmplLine` 存在于 `src/db_impl.cpp:1826`；`AmplificationStats`（`db_impl.h:96`）、`LevelStats`（`db_impl.h:90`）、
    `GetLevelStats`（`db_impl.cpp:1737`）、`GetAmplificationStats`（`db_impl.cpp:1749`）。
  - 现有列（逐字摘录）：`AMPL round_id=… user_logical_bytes=… entry_bytes=… flush_write_bytes=… compact_write_bytes=…
    write_amp_total=… write_amp_excl_compact=… read_files_checked=… read_index_blocks_read=… read_data_blocks_read=…
    read_bytes=… read_get_count=… read_amp_files_per_get=… space_sst_bytes=… space_manifest_bytes=… space_current_bytes=…
    space_log_bytes=… space_tmp_bytes=… space_amp=… space_amp_sst_only=… dropped_old_versions=… dropped_tombstones=…
    compaction_rounds=… compaction_round_p50_us=… compaction_round_max_us=… live_versions_max=…`
  - ⇒ 与 `M5-design §6.6` 的前缀列一致；**无 `space_filter_bytes`**（M5.3 的行尾追加点 = `FormatAmplLine` 的 `live_versions_max` 之后）。
    **E7 的阻断条件不成立**（M4.3 已提交）。

**⑦ 门禁机制与现有腿**：

- `scripts/lsm_gate.sh`：`run_gate`（`:39`）、`run_gate_marked`（`:60`，标记 AND 语义 + `@@` 分隔）、
  `run_gate_m3_marked`（`:85`，缺脚本 ⇒ `--require-m3` 时 FAIL / 默认 SKIP 并计入 `SKIPPED`）、`--require-m3`（`:29`）、
  `[PARTIAL]`（`:172-175`）。
- 现有腿数：`grep -c "^run_gate" scripts/lsm_gate.sh` = **21** = 3 个函数定义 + **18 条腿调用**
  ⇒ 与派工书「18 条腿」一致（`--rounds 100 --no-asan --require-m3`：M2 4 条 + M3 4 条 + M4 10 条 = 18；
  ASan 腿在 `--no-asan` 下不跑，TSan 腿默认不跑）。
- **`grep -c "require-m5" scripts/lsm_gate.sh` = 0** ⇒ `--require-m5` 与 M5 腿需 M5.1 新增；**既有 18 条腿一行不动**。

**⑧ 测试 seam（`FakeClock` 缺失必须登记）**：

- `grep -rn "FakeClock" tests src` = **0 命中** ⇒ 与 `M5-design E6` 一致；M5 的 A 组按 E6 用
  `MemEnv` + `CountingEnv` + `FaultyEnv`。
- `tests/memenv.h:29` `class MemEnv : public Env`；`tests/faulty_env.h:21` `class FaultyEnv : public Env`；
  `tests/sstable_counting_env.h:68` `class CountingEnv : public Env`。三个 seam 都存在。

**⑨ M5 产物是否已存在（应为空）**：`docs/m5-bench.md`、`scripts/bench_lsm.sh`、`scripts/lsm_m5_unit_test.sh`、
`scripts/lsm_batch_crash_test.sh` 复核时**全部不存在** ✅（`ls: cannot access …: No such file or directory`）；
`docs/m5-prerequisites.md` 即本文件（M5.0 产出）。

**⑩ `AMPL` 行的真实列名与插入点**：见 ⑥；**前缀列逐字一致，无差异需要更新 `M5-design §6.6` 的追加列清单**。

### §0.3 复核判据逐条结论（`M5-design §11 M5.0` 的 6 条）

| # | 判据 | 结论 |
|---|---|---|
| 1 | 工作区空 + `m4-compaction` tag 存在 ⇒ M5.1 可开工 | ✅ 成立 |
| 2 | 工作树仍脏 ⇒ 暂停 | ✅ 不适用（工作树干净） |
| 3 | `FormatAmplLine` 列名/位置与 §6.6 不一致 ⇒ 以落地为准并登记 | ✅ 前缀列一致；`space_filter_bytes` 待 M5.3 行尾追加（登记于 §2） |
| 4 | `FakeClock` 不存在 ⇒ 登记为可选测试辅助，A 组按 E6 | ✅ 确认不存在（登记于 §2 与 §8.3） |
| 5 | `Options` 无 `bloom_bits`、`DB` 无 `Write(WriteBatch*)` ⇒ 与 §5.2/§5.6 一致 | ✅ 确认；M5.1 落 `bloom_bits`，M5.2 落 `Write` |
| 6 | `Table::Open`/`GetEntry`/`TableBuilder::Add`/`Finish` 签名与 §3.6 草案不一致 ⇒ 以落地为准并登记 | ✅ 不一致（`Table::Open` 无 `open_stats`；`Add`/`Finish` 返回 `Status`）；M5.1 只**追加**默认参数、不改既有语义 |

---

## §2 差异登记（设计草案 ↔ 落地实现）

> 判据：`M5-design §11 M5.0` 的「**不一致以落地实现为准**」。下表逐条列出，供 M5.1~M5.3 引用。

| # | 设计草案的写法 | 落地实现（HEAD `fa7c328`） | 处置 |
|---|---|---|---|
| D-1 | §9.2/§11/§5 的所有 `src/*` 行号取自 `db5aa8f`（设计定稿时的稳定 rev），如 `DbReadStats` 标 `db_impl.h:98-113`、`Pending` 标 `db_impl.h:349`、`Write` 标 `db_impl.h:253`、`RunFlusher` 标 `db_impl.cpp:248` | 实际：`DbReadStats` `db_impl.h:119-131`、`Pending` `db_impl.h:396`、`Write` `db_impl.h:293`、`RunFlusher` `db_impl.cpp:257` | **行号全部以落地为准**（`M5-design §12.5` 第 11 条已预告）。接口语义无差异 |
| D-2 | §5.3 的 `Pending` 草案含 `entry_count` / `entries` / `user_bytes`（多 entry 承载） | 落地 `Pending`（`db_impl.h:396`）是**单 entry 形态**：`type` / `key` / `value` / `entry_bytes` | M5.2 按 §5.3 扩展（**只增字段、不改既有字段语义**）；M5.1 不动 |
| D-3 | §3.6 的 `Table::Open` 草案带 `ReadStats* open_stats = nullptr` | 落地 `Table::Open`（`table.h:77`）**无**该参数 | M5.1 **追加可选默认参数** `ReadStats* open_stats = nullptr`（默认参数保持 M3/M4 既有调用零改动） |
| D-4 | §3.6 假定 `TableBuilder::Add`/`Finish` 是「写入 + 返回状态」的形态 | 落地 `Status Add(...)` / `Status Finish()`（粘性 `status_`，`Finish` 幂等） | filter 的写入点插在 `Add`（`StartBlock`/`AddKey`）与 `Finish`（写 filter 块 + metaindex 注册）；**不改既有返回语义** |
| D-5 | §3.6 的 `ReadStats` 追加 7 个 filter 列 | 落地 `ReadStats`（`table.h:42`）无 filter 列 | M5.1 **只追加**这 7 列（既有 8 列语义逐字不变） |
| D-6 | §3.6 假定 `DbReadStats` 可直接加同名列 | 落地 `DbReadStats`（`db_impl.h:119`）无 filter 列 | M5.1 只追加 + `MergeReadStats`（`db_impl.cpp:566`）累加；`files_checked` 的既有递增点（`db_impl.cpp:530` 一带）不变 |
| D-7 | §9.2/§3.7 的「`TableCache::Open` 传递 `open_stats`，若需要」 | 落地 `TableCache::Open`（`version_set.cpp:897`）签名为 `(number, smallest, largest, out, opened)` | M5.1：`Table::Open` 的 `open_stats` 由 `TableCache::Open` 传 `nullptr`（缓存命中不重复读 filter）；`filter_blocks_read`/`filter_bytes_read` 只在**真的读了 filter 块**的路径上 +1，即 `Table::Open` 内部自己 += 到传入的 `open_stats`。`TableCache` 签名**不变** |
| D-8 | §2.1 D1 的 `k=7`（`round(10·ln2)`） | 常量尚未落地（`src/bloom.*` 不存在） | M5.1 按 §3.3 落地 `kFilterBaseLg=11`、`kBloomMinBits=64`、`kBloomMaxK=30`、`k=7` |
| D-9 | §7.5 / `M5-design §11 M5.1` 的证据命令引用 `--require-m5` 与 `scripts/lsm_m5_unit_test.sh` | `scripts/lsm_gate.sh` 无 `--require-m5`；脚本不存在 | M5.1 新增：脚本 + `run_gate_m5_marked`（照 `run_gate_m3_marked` 形态）+ `--require-m5`；**既有 18 条腿不动** |
| D-10 | §10.1 前置假设写进 `docs/m5-prerequisites.md` §8.3 | 本文件 | 见 §8.3（下方） |
| D-11 | `M5-design §9.1` 要求本文件「逐字附同一张 [指令原号 ↔ 落地号 ↔ 出处] 表」 | 本文件 | 见 §9（逐字复述 `M5-design §9.1` 的两张表） |
| D-12 | §3.5 草案把 `NewBuiltinBloomPolicy` / `FilterPolicy` 作为独立文件 | 尚未落地 | M5.1 落 `src/filter_policy.h`；哈希落点按 §3.3 的**替代落点**（`src/bloom.cpp` 内部匿名 namespace + `bloom.h` 暴露自由函数），**不新增 `src/util/hash.*`**（理由：`M5:120` 把它列为【可选扩展】；替代落点的位级结果与协议逐字一致，见 §3.3 末段） |
| D-13 | §3.7 step ②.5 的 `stats` 计数列名 | 落地 `ReadStats` 无这些列 | M5.1 用 §3.6 的 7 个精确列名（`filter_checked` / `filter_negative` / `filter_positive` / `filter_unavailable` / `filter_blocks_read` / `filter_bytes_read` / `data_blocks_skipped_by_filter`） |
| D-14 | §11 M5.1「必改」列 `src/version_set.{h,cpp}`（`TableCache::Open` 传递 `open_stats`，**若需要**） | 落地 `TableCache::Open` 在缓存未命中时调 `Table::Open`，`opened` 出参已表达「本次是否真的打开」 | **不需要改 `version_set.*`**（D-7 的落点分析）；M5.1 因此不动 `version_set.*`，减少波及面 |
| D-15 | §11 M5.1「必改」列 `src/db_impl.{h,cpp}`（`DbReadStats`/`MergeReadStats` 追加） | 落地 `DbReadStats`/`MergeReadStats` 在 `db_impl.{h,cpp}` | M5.1 追加 7 列 + 在 `GetInternal` 的 `tstats` 汇总处累加（`db_impl.cpp:530-536` 一带） |

---

## §8.3 A 组前置假设（`M5-design §10.1` 要求写进本文件）

1. filter 只覆盖该 SSTable **内**的 key 集合；一个 SSTable 内的 user key 可能有多版本（含 tombstone），
   filter 对**每条 entry 的 user key** 建位（D2），因此「有 tombstone 的 user key」也算存在性证据。
2. `Table::Open` 会读 footer/metaindex/index/**filter**（若存在）——**filter 不能省这些 IO，只能省数据块读**（E1）。
   A 组的「块读下降」断言一律只看 `data_blocks_read`，不看文件打开次数。
3. `MemEnv` 的 `GetFileSize` / `RenameFile` / `SyncDir` / `NewRandomAccessFile` 语义与 M3/M4 的 A 组假设一致
   （M5.1 的 filter 用例全部建在 `MemEnv` 上，零真实磁盘、零 flaky）。
4. `FakeClock` **不存在**（E6），M5 的 A 组不依赖它；需要确定性计时时用 `MemEnv` + `CountingEnv` + `FaultyEnv` + 显式事件断言。
5. `Options::bloom_bits` 默认 `10`（开）；**reader 是否解析旧 filter 与 `bloom_bits` 无关**（`M5-C7(c)`）——
   因此 `bloom_bits=0` 的库读 `bloom_bits=10` 写的文件时，filter 仍生效（A07 的反向用例）。
6. M5.1 的 A 组用例编号为 `M5-A01`~`M5-A10`（filter）与 `M5-A18`（多线程点查 + filter，TSan 腿）。
   `M5-A11`~`M5-A17` 属 M5.2（WriteBatch），**本阶段不落地**，亦不占用编号。

---

## §9 指令原号 ↔ 落地号 ↔ 出处（**逐字复述 `M5-design §9.1`**）

**M5 新增不变量**（沿用 `I1~I46`）：

| 指令原号 | 指令出处 | **落地号** | 一句话定义 | 与既有 I 的关系 |
|---|---|---|---|---|
| `I43` | `M5:85` | **`I47`** | Bloom 不得产生假阴性：对任一已写入且未被删除的 key，filter 必须判定「可能存在」；假阴性等价于读到错误结果 | **新增**；与 M3 的 `I25`（块类型/长度自检）互补，但 filter 是 advisory |
| `I44` | `M5:86` | **`I48`** | filter block 与其 SSTable 的数据块集合必须严格对应（数量、顺序、偏移自洽）；错位等同于假阴性 | **新增**；M3 的 `I25` 只管数据/索引块的 type/length，不含 filter 桶映射 |
| `I45` | `M5:87` | **`I49`** | filter 只能用于否定判断：读路径不得因 filter 的「肯定」结论跳过任何实际读取或校验 | **新增**；与 M3 的 `I24`（restart 结构）和 `I25` 共同构成「读路径必须真的读」 |
| `I46` | `M5:88` | **`I50`** | filter 的构建与读取是只读发布：filter 一旦随 SSTable 落盘即不可变 | **承接** M3 的 `I21`（SSTable 不可变）到 filter 块 |
| `I47` | `M5:89` | **`I51`** | WriteBatch 整批原子可见：崩溃恢复后不存在「批内部分条目可见」的状态 | **新增**；与 M2 的 `I14`（一条 record 一个原子单位）衔接 |
| `I48` | `M5:90` | **`I52`** | 批内 sequence 连续分配且与提交顺序一致；恢复后 `last_sequence` 覆盖批内最大值 | **收紧** M2 的 `I13`（重放顺序）到批粒度 |
| `I49` | `M5:91` | **`I53`** | 批的 WAL 记录完整承载整批（否则恢复出半批）；批记录损坏时的截断语义与 M2 的 `I14` 一致 | **承接** M2 的 `I14`/`I18` |
| `I50` | `M5:92` | **`I54`** | `sync=true` 时批的返回仍满足 durable-before-ack（M2 的 `I11` 在批粒度上继续成立） | **承接** M2 的 `I11`/`I16` |
| `I51` | `M5:93` | **`I55`** | 基准结果的参数必须完整打印，且与实际运行的配置一致；未打印参数的测量结果不得作为结论 | **新增**；与 M4 的 `I45`（统计可复现）同源 |
| `I52` | `M5:94` | **`I56`** | `bench_lsm.sh` 在数据不一致（如 `missing != 0`）时必须返回非零退出码；该性质必须被失败注入自测证明 | **新增**；与 M3 的 D9.6 防空绿同源 |

**M5 新增锁纪律**（沿用 `L1~L29`）：

| 指令原号 | 指令出处 | **落地号** | 一句话定义 | 与既有 L 的关系 |
|---|---|---|---|---|
| `L27` | `M5:97` | **`L30`** | filter 构建在单线程完成并发布；发布后只读，读路径无需加锁 | 承接 M3 的 `L15`（版本 `shared_ptr`）与 M4 的 `L23`（`Ref/Unref`）到 filter |
| `L28` | `M5:98` | **`L31`** | WriteBatch 对象的所有权与线程约束必须在接口注释中写明（谁拥有、能否跨线程传递、提交后是否可复用） | **新增**；与 M2 的 `L2`（单写者）并列 |
| `L29` | `M5:99` | **`L32`** | 批提交与组提交队列的锁序沿用 M2 的 `L8`（提交队列锁 → DB 锁），禁止反向 | **承接** M2 的 `L8`（`docs/m2-design.md:108`） |
| `L30` | `M5:100` | **`L33`** | 基准计时窗口内禁止做日志 IO（打印要么在窗口外，要么该轮关闭日志） | **新增**；延续 M2 的 `L7`（持锁零 IO）到基准 |
| `L31` | `M5:101` | **`L34`** | 读路径的 filter 命中/否证计数器必须用原子或线程局部后汇总，禁止无锁共享自增 | **收紧** M3 的 `L19`（读路径统计）到 filter 计数 |
| `L32` | `M5:102` | **`L35`** | 禁止持 DB 锁做基准统计的 IO（延续 `I17`/`L7`） | **承接** M3 的 `L18`（禁止持 DB 锁做 IO） |

**前序号段的换算（`M5:84`/`M5:96` 的旧号）**：

| 指令写法 | 实际冻结 | 换算说明 |
|---|---|---|
| 「M3 的 `I21~I30`」 | M3 实际 `I21~I34` | 前 10 个号相同，但 M3 另有 `I31~I34`；M5 的 `I43~I52` 不能建立在「M3 只到 I30」的假设上 |
| 「M4 的 `I31~I42`」 | M4 实际 `I35~I46` | M4-C1 已整体顺延 +4；M5 的起点因此是 `I47` |
| 「沿用 `L1~L26`」 | M4 实际到 `L29` | M4-C1 把 L19~L26 顺延为 L22~L29；M5 的起点因此是 `L30` |
| 「新增 `L27~L32`」 | 落地 `L30~L35` | 避免与 M4 的 `L27~L29` 撞号 |

---

## §10 本阶段（M5.0）的产出与未做清单

- **产出**：本文件（`docs/m5-prerequisites.md`）。**未提交**（提交由用户执行；M5.0 的提交信息见 `M5-design §11`）。
- **未做（诚实登记）**：
  1. M5.0 期间**未跑**任何构建/测试（§11 M5.0 只要求复核；构建与测试属 M5.1）。
  2. §0.2 的复核是**符号/常量/签名的存在性与形态**核对；不含语义正确性验证（那是各里程碑 A 组的职责）。
  3. `M5-design §9.2` 中 M5.2/M5.3 的落点（`Pending` 扩展、`AMPL` 行尾追加 `space_filter_bytes`）本阶段只登记、不落地。
