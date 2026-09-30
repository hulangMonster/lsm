# M5 设计（docs/m5-design.md）—— Bloom Filter + WriteBatch + 微基准与数据表

> 角色：M5「优化与微基准」里程碑 `#0` 设计起草者。
> 边界：**只写本文件**；VM `~/lsm-kv` 全程只读（`cat/sed/grep/ls/git log|status|show/diff --stat`），
> 未执行任何构建、测试、cmake、git 写操作；本机克隆 `D:\JLProject\lsm-kv` 只读。
> 基线：VM `HEAD = db5aa8f`（M4.2 核心已提交；M4.3 正在被另一个代理实现，工作树在探测期间为脏）。
> 用户已授权：`M5-C1`~`M5-C8` 一律按推荐执行；本文的「推荐」即最终裁决，除非用户明确改判。
> 引用纪律：每条结论可追溯到「文件名 + 行号」；代码行号一律取自 `git show db5aa8f:<path>` 的
> **稳定 rev**，未提交的 M4.3 工作树改动单独标注，并在 §11 的 M5.0 里用可粘贴命令复核。

---

## 0. 现状探测与原始证据（`#0` 的硬要求）

### 0.1 探测命令清单（全部只读；本轮实际执行）

下列命令**全部实际执行**（为可读性，把同一轮只读调用合并成一条命令族；`ssh` 均带 `-o BatchMode=yes`）；没有执行的（构建、测试、cmake、sanitizer、`git add/commit/checkout`）一律写明「未验证」。

```bash
# —— 本机（Git Bash，只读）——
pwd; ls -la /d/lsm; ls -la /d/lsm/mine/m4m5
wc -l /d/lsm/mine/m4m5/notes.md '/d/lsm/lsm-kv-开发指令/M5-优化与微基准.md' /d/lsm/mine/m4m5/m4-design.md
sed -n '1,201p' '/d/lsm/lsm-kv-开发指令/M5-优化与微基准.md'
sed -n '243,459p' /d/lsm/mine/m4m5/notes.md
grep -n 'M5' /d/lsm/mine/m4m5/notes.md | head -80
ls -la /d/JLProject/lsm-kv; cd /d/JLProject/lsm-kv && git rev-parse --short HEAD && git log --oneline -3
find /d/JLProject/lsm-kv -maxdepth 2 -type f -not -path './.git/*' | sort | head -100

# —— 虚拟机 ~/lsm-kv（ssh 只读；未跑任何构建/测试）——
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git rev-parse HEAD && git status --porcelain && git log --oneline -8 && git tag && git branch -a && git worktree list'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && ls -la docs scripts tests src src/sstable src/util && wc -l docs/*.md scripts/* src/**/*.h src/**/*.cpp'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && grep -n "^#\{1,4\} " docs/m3-design.md docs/m4-design.md docs/protocol.md docs/m3-evidence.md docs/m3-prerequisites.md docs/m4-prerequisites.md'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/sstable/format.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/sstable/table.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/sstable/table_builder.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/common.h | sed -n "260,343p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/db.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/wal.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/db_impl.h | cat -n'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/db_impl.cpp | sed -n "130,420p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/db_impl.cpp | sed -n "420,560p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/version_set.cpp | sed -n "890,975p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/sstable/table.cpp | sed -n "80,340p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/sstable/table_builder.cpp | sed -n "20,170p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git show db5aa8f:src/memtable.h | cat -n && git show db5aa8f:src/memtable.cpp | sed -n "144,200p"'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && cat -n scripts/lsm_gate.sh && cat -n scripts/lsm_manifest_test.sh && cat -n scripts/lsm_build.sh'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && cat -n tests/test_harness.h && cat -n CMakeLists.txt'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && sed -n "656,777p" docs/m3-design.md && sed -n "805,825p" docs/m3-design.md && sed -n "1631,1642p" docs/m3-design.md && sed -n "1993,2015p" docs/m3-design.md'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && sed -n "185,205p" docs/protocol.md && sed -n "281,378p" docs/protocol.md'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && sed -n "1358,1387p" docs/m4-design.md && sed -n "1716,1731p" docs/m4-design.md && sed -n "1926,1990p" docs/m4-design.md'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && sed -n "2139,2199p" docs/m4-design.md && sed -n "2310,2335p" docs/m4-design.md && sed -n "2393,2417p" docs/m4-design.md && sed -n "2581,2648p" docs/m4-design.md'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && uname -a; nproc; free -m; df -T /home; cat /proc/loadavg; cat /proc/sys/kernel/perf_event_paranoid; g++ --version; cmake --version; ls -la /usr/local/lib/libgtest*.a'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && grep -h "TEST(" tests/*.cpp | wc -l && ls -la build/bin build-asan/bin build-tsan/bin'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && grep -rn "FakeClock" tests src || true; grep -n "class CountingEnv" tests/sstable_counting_env.h; grep -n "class FaultyEnv" tests/faulty_env.h'
ssh -o BatchMode=yes ubuntu-vm 'cd ~/lsm-kv && git diff --stat && git diff --numstat'
# —— raft-kv 只读参照（本机克隆）——
cd /d/JLProject/raft-kv && ls -la scripts docs && grep -n '^#\{1,3\} ' docs/m5-bench.md | head -60 && sed -n '1,80p' scripts/bench_m5_ab.sh && sed -n '1,60p' scripts/fsbench_commit_latency.cpp
```

### 0.2 探测①：基线 rev / 工作区 / tag（**M4 尚未收口，必须登记**）

原始输出（`ssh -o BatchMode=yes ubuntu-vm` 只读）：

```
$ git rev-parse HEAD
db5aa8fcebdf381ce2c548903308d4cd51c2ba4f

$ git log --oneline -5
db5aa8f feat(m4): M4.2 核心 —— compaction 执行体 + 调度 + 读路径层级化 + L22~L29 锁纪律接线
4c5cc40 feat(m4): M4.0 复核 + M4.1 MANIFEST/CURRENT 元数据 + M4.2（部分：选择器与默认翻转）
43cdea3 feat(m3): M3.3 版本元数据持久化 + 启动恢复 + WAL 轮转/回收 + 崩溃脚本 + 全部门禁（M3 收口）
59adfe9 docs(m4): 冻结 M4 设计（分层 Compaction：MANIFEST/CURRENT + VersionEdit + 选层选文件 + 不变量改号）
d8513e3 feat(m3): M3.2 flush 路径 + 读路径 + MergingIterator/DBIter + 单后台线程（注册只在内存）

$ git tag
m1-memtable
m2-wal
m3-sstable
# 注意：**没有** m4-compaction tag ⇒ M4 未收口。

$ git status --porcelain          # 第一次探测（时刻 A）
（空）

$ git status --porcelain          # 稍后探测（时刻 B，同一只读会话内）
 M src/db_impl.cpp
 M src/db_impl.h

$ git diff --stat
 src/db_impl.cpp | 192 +++++++++++++++++++++++++++++++++++++++++++++++++++++++-
 src/db_impl.h   |  31 +++++++++
 2 files changed, 222 insertions(+), 1 deletion(-)

$ git worktree list
/home/tengyujie/lsm-kv  db5aa8f [main]
```

**事实**：
1. 提交层（committed layer）的 HEAD = `db5aa8f`；`m4-compaction` tag 不存在 ⇒ **M4 未收口**。
2. 工作树在探测期间被另一个代理改成**脏**：`src/db_impl.{h,cpp}` 有 **未提交** 的 M4.3 统计改动
   （`FormatAmplLine` / `GetLevelStats` / `GetAmplificationStats` / `FRONT` 采样等）。
3. 因此本文的代码行号一律用 `git show db5aa8f:<path>` 的**稳定行号**；M4.3 在途部分的接口一律标
   「**M5 开工前置复核**」，复核命令见 §11 的 M5.0。

### 0.3 探测②：M4 依赖物「已落地 / 在途 / 未落地」矩阵

| 依赖物 | 稳定 rev 的落地形态（`db5aa8f`） | 在途 / 未落地 | 对 M5 的影响 |
|---|---|---|---|
| `Version` 层级视图 | `src/version_set.h:36-76`（`level_files` / `AllFiles` / `Ref/Unref`）**已落地** | — | M5 的读路径接入点复用 `Version::level_files()` |
| `VersionSet` / MANIFEST / CURRENT | `src/version_set.h:83-192` + `git show db5aa8f:src/version_set.cpp` | M4.3 的快照/统计在途 | M5 不改 MANIFEST 格式 |
| `Compaction::Run` | `git show db5aa8f:src/compaction.cpp` 的 `Status Compaction::Run` 在**第 197 行** | M4.3 的脚本/B 组未落地 | M5 基准必须在「compaction 可能发生」的真实库上测；参数必须打印 compaction 触发参数 |
| `TableCache` | `src/version_set.h:195-224`；`version_set.cpp:897`（Open）/`944`（Get） | — | filter 随 `Table` 打开/缓存，**不另开 fd** |
| `Table::GetEntry` 三态 | `src/sstable/table.h:87`；`table.cpp:259` | — | M5 的 filter 否定判断接在 `GetEntry` 的「决定是否读数据块」处 |
| `ReadStats` | `src/sstable/table.h:42-54` | M4.3 不改字段语义 | M5 只**追加** filter 计数列 |
| `Options` | `src/common.h:285-314`（含 M3/M4 字段） | M4.3 可能在途调整 | M5 追加 `bloom_bits`，校验放 `DB::Open` |
| `Pending` 组提交结构 | `src/db_impl.h:349`（稳定 rev）；`db_impl.cpp:219` `EncodeGroup`；`:248` `RunFlusher` | M4.3 在 `RunFlusher` 里加统计采样 | M5 要扩 `Pending` 以承载批；**必须在 M5.0 复核 M4.3 落地后的形态** |
| `WALWriter::Append` | `src/wal.h:48-72`；`kMaxLogicalRecordSize=64 MiB`（`wal.h:23`） | — | 一次 WAL record 承载整批 |
| `DB::Put/Delete/Get` | `src/db.h:32-46`；**无 `WriteBatch`** | — | M5 新增 `DB::Write(WriteOptions, WriteBatch*)` |
| `MemoryDBImpl` | `src/db.cpp:18-82` | — | 它也实现 `DB`，M5 必须同步实现 `Write(WriteBatch*)` |
| `tests/test_harness.h` | `FakeClock` **零命中**（本轮 grep）；`CountingEnv` 在 `tests/sstable_counting_env.h:68`；`FaultyEnv` 在 `tests/faulty_env.h:21`；`MemEnv` 在 `tests/memenv.h` | `FakeClock` 仍不存在 | M5 的 A 组不得假装它存在；M5.1 可追加最小 `FakeClock` 并登记 |
| `scripts/lsm_gate.sh` | 已有 `run_gate_marked`（`:60`）与 `run_gate_m3_marked`（`:85`） | 无 M5 腿 | M5.3 追加 M5 腿 + 正向标记 |
| `docs/m5-design.md` / `docs/m5-prerequisites.md` | VM 无 | 本文即 `#0` 产出 | `#1` 产出 prerequisites |
| `docs/m4-evidence.md` / `docs/amplification.md` / `scripts/lsm_compaction_stress.sh` / `scripts/lsm_level_stats.cpp` | VM 均无（`ls` 实测 No such file） | M4.3 在途 | M5 的数据表不能假装 M4.3 已交付；M5.0 复核 |

### 0.4 探测③：既有契约的实际形态（原始输出摘录）

**metaindex 与 filter 预留**（`git show db5aa8f:src/sstable/format.h`）：

```
24: enum BlockType : uint8_t {
25:   kBlockTypeData = 0x01,
26:   kBlockTypeIndex = 0x02,
27:   kBlockTypeMetaIndex = 0x03,
28:   kBlockTypeFilter = 0x04,  // M5 预留；M3 不产出
29: };
44: constexpr size_t kFooterSize = 44;
45: constexpr uint32_t kTableFormatVersion = 1;
```

**protocol 冻结**（`docs/protocol.md`）：

```
185: ### 9.4 WAL batch payload 编码
186: 一条逻辑 record 的 payload 是**一个 batch**（M2 的一次写 = 一个 batch；组提交把整批合并为一条 record）：
188: payload := sequence(8B, LE) || count(4B, LE) || entry[0..count)
189: entry   := type(1B) || key_len(varint32) || key || [ value_len(varint32) || value ]
...
204: - 本编码与 M5 的 `WriteBatch` 落盘布局同构（M2 只作为 WAL 内部组织，**不提供公共 API**）。

317: ### 10.6 元数据块 payload
319: metaindex_payload := meta_entry* || restart_offset[uint32 LE] * n || n(uint32 LE)
320: meta_entry        := varint32(name_len) || name || handle(16B)
322: - `name` 是普通字符串（**不是** internal key）。M3 写**空表**（`n = 1`，`restart_offset[0] = 0`，无 entry）。
323: - M5 的 Bloom filter 通过 `name = "filter.leveldb.BuiltinBloomFilter2"` 指向 `kBlockTypeFilter` 块；
324:   **footer 布局不变、`kTableFormatVersion` 不升**。
325: - 读取方**必须容忍未知 `name`**（记录并计数，不报错、不影响其他块）。
```

**M3 设计对 M5 的预留**（`docs/m3-design.md:656-671`）：M3 写空 metaindex；M5 加 Bloom 时 metaindex 多一条
`"filter.leveldb.BuiltinBloomFilter2"` → filter 块 handle；**footer 零改动、`kTableFormatVersion` 不升**；
M3 的 reader 必须容忍未知 metaindex 条目且**计数上报** `unknown_metaindex_entries`。

**文件整体布局**（`docs/m3-design.md:672-697`）：`data block* → metaindex → index → footer(44B)`；
校验约束是 `metaindex.offset + metaindex.size <= index.offset` 且 `index.offset + index.size == file_size - 44`。
M5 的 filter 块必须插在 **data block* 与 metaindex 之间**，才能让 metaindex 引用它的 handle，且不破坏上述约束。

**footer**（`docs/m3-design.md:731-764`）：`magic(4) || version(4 LE) || index_handle(16) || metaindex_handle(16) || crc(4)`；
`version != 1 ⇒ kNotSupported`（不是 `kCorruption`）。⇒ M5 若升 `kTableFormatVersion`，所有 M3/M4 旧文件都会
`kNotSupported`，与 `M5:63` 的「旧格式文件（无 filter）的行为」直接冲突 ⇒ `M5-C3` 裁决：**不升**。

**读放大口径**（`docs/m3-design.md:1631-1641` + `src/sstable/table.h:42-54`）：`ReadStats` 只给原始计数，
字段 `files_checked / key_range_skipped / index_blocks_read / data_blocks_read / bytes_read / crc_checked / crc_failed`；
M4 在 `docs/m4-design.md:1716-1730` 追加 `hit_layer` 取值域，并明确「`bytes_read` 口径不变、`files_checked` 口径不变」。
M3 的 `M3-B08`（`docs/m3-design.md:2004`）固定输出行是
`FILES_CHECKED / KEY_RANGE_SKIPPED / INDEX_BLOCKS / DATA_BLOCKS / BYTES_READ / HIT_LAYER` 的 p50/p90。
⇒ M5 只能**追加** `filter_*` 列，不得改这些既有列的语义（`M5:182`、M4 的 `I45`）。

**M4 的放大固定行**（`docs/m4-design.md:2139-2184`）：`AMPL` 行已冻结前缀，**只允许行尾追加新列**，
并明写「M5 要加 fitler 列，`M5:182`；M5 只能追加」。⇒ M5 的所有 filter/bench 列都**追加在 `AMPL` 行尾**。

**组提交现状**（稳定 rev）：
- `src/db_impl.h:349` 的 `Pending` 只有单条 `type/key/value/entry_bytes`；
- `src/db_impl.cpp:219` 的 `EncodeGroup` 写 `begin(8) || count(4) || entry*`；
- `src/db_impl.cpp:248` 的 `RunFlusher` 取批、分配 `begin = last_sequence_ + 1`、按成员逐个分配 sequence、
  `last_sequence_ = begin + members.size() - 1`、锁外 `Append`/可选 `Sync`、回锁后逐条 `MemTable::Add`；
- `src/db_impl.cpp:160` 已对单条写做 `entry_bytes + 16 > kMaxLogicalRecordSize` 的**入队前**拒绝；
- `src/memtable.h:64-66` 的 `WouldReject(extra_bytes)` 与 `MemTable::Add` 的写前判**同源**；
- `src/memtable.cpp:151-182` 的 `Add` 失败只有：非法 key / 非法 seq / 非法 type / `WouldReject(0)` 触顶。
⇒ M5 的 WriteBatch 只需扩 `Pending` 为「多 entry + 总 footprint」，不需要改 WAL 物理格式。

**M2 的 fsync 固定输出格式**（`docs/m2-design.md` 的「9.3 微基准的固定输出格式（G6）」）：
`STRATEGY … N … MIN_MS … MEDIAN_MS … P90_MS … MAX_MS`，**必须**打印 `machine`/`load`/`fs`/`mount` 行；
M2 实测教训是「同一脚本、同一台 VM、不同时间点结果可差 3 倍」，且 M2 实测的 fsync 中位是 **2.3~2.9 ms**，
**不是**指令写的 8 ms。⇒ M5 的基准必须自己重测 fsync 成本，**不得继承 8 ms**（`M5:201`、`docs/m2-design.md` 的 Q13）。

**raft-kv 参照**（本机只读核对存在）：`scripts/fsbench_commit_latency.cpp`（2812 B）、
`scripts/bench_m5_ab.sh`（7158 B，头部注释逐字含「同一脚本内交替测量（M4→M5→M4→M5…）」、
「每次 fill 后必须 verify，missing != 0 则整轮作废」、「判据(M5.5 修订)… **同机比值口径**」）、
`docs/m5-bench.md`（26738 B，`##` 小节含 §3.6/§3.7/§3.9/§3.10/§3.11）。
M5 只借鉴其「同轮交替 + 固定行 + 非零退出 + 负结果入档」形状，不复制业务代码。

### 0.5 探测④：编号空间的实际占用（`M5-C1` 的事实基）

| 号段 | 实际冻结范围 | 出处 |
|---|---|---|
| M1 | `I1~I10` | `docs/m1-prerequisites.md` §1 |
| M2 | `I11~I20` | `docs/m2-prerequisites.md` §4 |
| M3 | **`I21~I34`**（不是指令写的 `I21~I30`） | `docs/m3-design.md:1876-1901` |
| M4 | **`I35~I46`**（不是指令写的 `I31~I42`） | `docs/m4-design.md:1926-1955` |
| M5 指令原号 | `I43~I52`（`M5:85-94`） | 与 M3 的 `I31~I34`、M4 的 `I35~I46` **撞号** |
| M1~M2 锁 | `L1~L12` | `docs/m1-prerequisites.md` §6、`docs/m2-design.md` |
| M3 锁 | **`L13~L21`** | `docs/m3-design.md:1902-1922` |
| M4 锁 | **`L22~L29`**（不是指令写的 `L19~L26`） | `docs/m4-design.md:1926-1955` |
| M5 指令原号 | `L27~L32`（`M5:97-102`） | 与 M4 的 `L27~L29` **撞号** |

⇒ `M5-C1` 裁决：**沿用 `I1~I46` / `L1~L29`；M5 新增 `I47~I56` / `L30~L35`**。
完整映射表见 §9.1；`docs/m5-prerequisites.md` 必须逐字附同一张表。

### 0.6 探测⑤：协议章节与「M3 预留、M5 才用」清单

`docs/protocol.md` 当前共 **469 行**、章节到 **§11（MANIFEST / VersionEdit，M4 定稿）**：

```
379: ## 11. MANIFEST / VersionEdit 编码（M4 定稿）
387: ### 11.1 文件命名与文件号空间（追加）
404: ### 11.2 MANIFEST record 帧格式
425: ### 11.3 VersionEdit 字段表
448: ### 11.4 全量快照 edit
456: ### 11.5 层内布局不变式（安装期校验）
463: ### 11.6 `META` 的迁移（一次性）
```

⇒ M5 追加 **§12（filter block 编码）** 与 **§13（WriteBatch 编码）**；patch 文本见 §4。
若 M4.3 又追加了协议章节，M5.0 必须重新顺延编号；本文以 `db5aa8f` 的 §11 结尾为基线。

「M3 预留、M5 才用」逐条核对：

| # | 预留物 | 稳定 rev 实测 | M5 的用法 |
|---|---|---|---|
| V1 | metaindex 空块 | `table_builder.cpp:126-133` 写空 metaindex 并注册 handle | M5 写一条 `filter.leveldb.BuiltinBloomFilter2` → filter handle |
| V2 | reader 容忍未知 metaindex 条目并计数 | `table.cpp:143-162`（`unknown_metaindex_entries_`） | M5 reader 认识已知 name；未知仍计数 |
| V3 | `kBlockTypeFilter = 0x04` | `format.h:28` 注释逐字「M5 预留；M3 不产出」 | M5 写出 `type=0x04` 的 filter 块 |
| V4 | WAL batch payload 与 WriteBatch 同构 | `protocol.md:185-204`；`db_impl.cpp:219-245` | M5 只加公共 `WriteBatch`/`DB::Write`，**不改位级格式** |
| V5 | `TableCache` | `version_set.h:195-224` | filter 随 `Table` 缓存；不另开 fd |
| V6 | `ReadStats` 原始计数 | `table.h:42-54` | M5 **追加** filter 列 |
| V7 | `Options::verify_checksums` | `common.h:296` | filter 块 CRC 的处置见 §3.7 |
| V8 | footer 不升版本 | `format.h:45`、`m3-design.md:656-671` | `M5-C3`：**不升** `kTableFormatVersion` |
| V9 | `MemTable::WouldReject` | `memtable.h:64-66` | WriteBatch 的整批容量预检复用同一函数 |
| V10 | `scripts/lsm_gate.sh` 的正向标记机制 | `lsm_gate.sh:60` `run_gate_marked`、`:85` `run_gate_m3_marked` | M5 追加 `run_gate_m5_marked`，防「空绿」 |

### 0.7 探测⑥：测试 / 门禁 / 构建资产现状

| 项 | 原始实测 |
|---|---|
| 全量用例数 | `grep -h "TEST(" tests/*.cpp \| wc -l` = **167** |
| 构建目录 | `build/`、`build-asan/`、`build-tsan/` 均存在，`ls build/bin` 有 `lsm_tests`、`lsm_crash_*`、`lsm_m3_probe` 等 |
| 门禁入口 | `scripts/lsm_gate.sh` 156 行；M2 腿 5 条 + M3 腿 4 条 + M4 腿 1 条（`lsm_manifest_test.sh`） |
| M4 腿标记 | `M4_TESTS_RAN [1-9][0-9]*@@M4_TESTS_FAILED 0@@LSM_VERSION_FORBIDDEN 0@@\[MANIFEST_OK\]` |
| `--require-m3` | 存在；缺脚本 ⇒ `SKIP` + 末行 `[PARTIAL]`（不是通过） |
| `FakeClock` | `tests/` 与 `src/` **零命中**（本轮 grep）；M4 的 A 组也未落地它 |
| 计数/故障注入 seam | `CountingEnv`（`tests/sstable_counting_env.h:68`）、`FaultyEnv`（`tests/faulty_env.h:21`）、`MemEnv`（`tests/memenv.h`） |
| ASan/TSan | 构建目录存在；本轮**未跑**（禁止构建/测试） |

### 0.8 未验证项（明确写出，不得当成已验证）

1. **未跑任何构建、单测、ASan、TSan、门禁脚本**（派工书禁止；另一个代理正在用 VM 仓库做 M4.3）。
2. **未实测 Bloom 的假阳性率 / 块读下降倍数**；本文只给判据与门禁形状，实测值由 M5.1 产生。
3. **未实测本机 fsync 成本**；M2 已实测 2.3~2.9 ms、指令写 8 ms，M5 必须重测，本文以「未验证（需 M5 #1 阶段实测）」标注所有性能数字。
4. **未验证 M4.3 的 `FormatAmplLine` 最终列名与插入点**（工作树未提交）；M5.0 必须复核。
5. **未验证 `tests/test_harness.h` 的 `FakeClock` 是否会在 M4.3 期间被补上**；M5 不依赖它。

### 0.9 并发事实（**必须登记**）

探测期间 VM 工作树从干净变为 ` M src/db_impl.cpp` + ` M src/db_impl.h`（`git diff --numstat` = `191 1` / `31 0`），
内容正是 M4.3 的 `AMPL/LEVEL/FRONT` 统计与采样。⇒ **M4.3 正被另一个代理实现**。
本文对 `src/db_impl.*` 的行号引用一律以 `db5aa8f` 为准；任何与 M4.3 在途改动相交的接口
（`Pending` / `RunFlusher` / `FormatAmplLine` / `AmplificationStats`）都在 §11 的 M5.0 里列了**可粘贴复核命令**，
复核不一致时以落地实现为准并登记差异。
后续探测（同一只读会话）观测到脏集合已扩大到 `CMakeLists.txt`、`tests/compaction_db_test.cpp`，并出现未跟踪新文件
`scripts/lsm_ampl_probe.cpp`（M4.3 的放大探针）——这进一步证明 M4.3 仍在途，M5.0 必须等它提交或用户授权后再开工。

---

## 1. 目标 / 非目标 / 与 M1~M4 的关系

### 1.1 目标（逐条对应 `M5:9-15` 的 6 条）

| # | 目标 | 本文的落地章节 | 验收判据 |
|---|---|---|---|
| G1 | Bloom Filter：每个 SSTable 一个 filter block（默认 10 bits/key、k≈7、Double Hashing），读路径在「决定是否读数据块」处做否定判断；filter 存入 metaindex，随格式版本兼容演进 | §3、§4 §12、§5 | `M5.1`：`filter_test` 全绿（含零假阴性 + 误判率实测 + 块读下降门禁） |
| G2 | WriteBatch：`DB::Write(batch)`、编码格式、整批原子可见、一次 WAL record 承载整批 + 一次 MemTable 提交组 | §5、§4 §13 | `M5.2`：`batch_test` 全绿 + `kill -9` 半批不可见 |
| G3 | 微基准：顺序写/随机写/随机读/顺序读四类负载，对照裸文件 `write+fsync`、裸文件 `pwrite` 随机、纯内存 `std::map`，产出「LSM 值得用在哪、不值得用在哪」的数据表 | §8 | `M5.3`：`bench_lsm.sh` 固定行 + 数据表 + 负结果入档 |
| G4 | 把 M2 的 fsync 成本基线与本阶段的吞吐/延迟串成一张成本-收益表 | §6.6 | 数据表含 fsync 成本列，且**本阶段重测**（不继承 8 ms） |
| G5 | 门禁脚本 `scripts/bench_lsm.sh`：固定输出行 `CELL / THROUGHPUT / LATENCY / P99`，数据不一致或 `missing != 0` 时退出码 1 | §7 | `M5.3`：脚本可复现 + 失败注入自测证明退出码 1 |
| G6 | 三个放大的最终口径与结论，明确列出 LSM 的劣势场景（不吹） | §6.6、§12.5 | `AMPL` 行追加 filter 列；负结果入档 |

### 1.2 非目标（硬边界，评审逐条对照）

以下**不做**，且每条给出「不做的理由 + 登记落点」：

1. **多线程并行 compaction / subcompaction**：`M5:25` 硬边界；M4 已把并发模型定成「单 compaction 线程 + `install_mu_` 串行化」（`docs/m4-design.md:442-485`、§15 R7），M5 不引入分片。
2. **mmap 读取**：`M5:26` 非目标（`M5-C8` 裁决：不做，只出「列风险 + 不做理由」登记段）。
   风险：崩溃一致性（mmap 页引用与 `unlink`/`rename` 的交互）、文件被删/截断后的 `SIGBUS`、地址空间与
   `mmap` 生命周期；且 M5 的验收只需要「数据块读取次数下降」，不依赖 mmap。
3. **压缩算法（Snappy/LZ4 等）**：`M5:26` 非目标（`M5-C8`：不做）。
   理由：会改动数据块 payload 格式与 CPU/IO 权衡，属于新的格式面；M5 只允许「filter 新增块」，
   不允许改 M3 的数据块/索引块格式（`M5:121`）。登记为 M6+ 候选。
4. **Column Family / 事务 / MVCC 增强**：`M5:26`。
5. **与 raft-kv 的对接**：`M5:26`，属 M6。
6. **块缓存（BlockCache）**：M4 已裁决「不引入」（`docs/m4-design.md:2581-2588`、§2.1 D8），
   并把块缓存列为 M5 候选（`docs/m4-design.md:2596-2603` R10）。M5 的裁决：**仍不引入**。
   理由：① M4 的 `TableCache` 已承担文件句柄缓存，块缓存是第二级缓存，引入独立的淘汰粒度、
   与 `verify_checksums` 的交互、命中率口径；② M5 的读放大改善由 **filter** 承担，若同时引入块缓存，
   `≥3×` 的块读下降会**归因混淆**（分不清是 filter 还是块缓存省下的）；③ 块缓存列为 M6+ 候选并登记。
7. **`ReadOptions` / 公共 `Snapshot*` API**：M4 已把快照 API 落在 `PersistentDBImpl`（`docs/m4-design.md:2589-2595` R9）；M5 不扩大公共 API。
8. **并行/异步 WAL、`O_DIRECT`、WAL 用户态缓冲**：M2 的硬约束（`wal.h:4-5`）；M5 不碰。
9. **为让数字好看而修改默认配置**：`M5:21` 禁止；M5 的 filter 默认 **开**，对照实验必须显式传参并打印（`M5-C7`）。

### 1.3 与 M1~M4 的关系

#### 1.3.1 逐字复用（不改一行）的既有契约

| 契约 | 出处 | M5 如何复用 |
|---|---|---|
| WAL batch payload 编码 | `protocol.md:185-204` | `WriteBatch` 的内存布局与落盘布局**逐字同构**；`DB::Write` 不改 payload 格式 |
| WAL 物理 record / 跨块切分 / CRC 覆盖面 | `protocol.md:144-184` | 不改；`WALWriter::Append` 一次收一整条 batch payload |
| 内部 key 编码 / 比较规则 | `protocol.md:54-94`、`src/common.h` | filter 按 **user key** 建；不碰 internal key 编码 |
| SSTable 数据块 / 索引块 / footer 格式 | `protocol.md:281-345`、`m3-design.md:582-764` | 不改；filter **只新增块**（`kBlockTypeFilter=0x04`） |
| metaindex 容器与「容忍未知 name」 | `protocol.md:317-328`、`table.cpp:143-162` | M5 往已验证的容器里塞一条记录 |
| `Version` / `MANIFEST` / `CURRENT` | `protocol.md:379-469`、`m4-design.md` §3 | 不改；M5 不新增 `VersionEdit` 字段 |
| 三个放大的前缀列 | `m4-design.md:2139-2184` | **只在行尾追加** filter 列 |
| `ReadStats` 既有列语义 | `table.h:42-54`、`m4-design.md:1716-1730` | 不改语义，只追加 `filter_*` 列 |
| `Options` 既有字段 | `common.h:285-314` | 追加 `bloom_bits`；既有字段校验不变 |
| `lsm_gate.sh` 的正向标记纪律 | `lsm_gate.sh:56-82`、`m3-prerequisites.md:716-729` D9.6 | M5 腿同样用 `run_gate_marked`，并加 M5 正向标记 |

#### 1.3.2 被扩展的接口（「只增不改」逐条列出）

| 既有物 | M5 的扩展 | 兼容性 |
|---|---|---|
| `src/common.h` 的 `Options` | 追加 `int bloom_bits = 10;`（`0` = 关闭） | 只增字段；旧聚合初始化若用 designated init 不受影响；`DB::Open` 追加校验 |
| `src/db.h` 的 `DB` | 追加纯虚 `virtual Status Write(const WriteOptions&, WriteBatch*) = 0;` | 所有 `DB` 实现必须补；`MemoryDBImpl` 与 `PersistentDBImpl` 都补 |
| `src/db_impl.h` 的私有 `Write(ValueType,...)` | **改名** `WriteEntry(ValueType,...)` | 避免与公共 `Write(WriteOptions, WriteBatch*)` 重载歧义（`M5-R7`） |
| `src/db_impl.h` 的 `Pending` | 追加 `entry_count` / `entries` / `user_bytes`；`entry_bytes` 语义扩展为「整批 entry 字节」 | 组提交内部结构，不落盘 |
| `src/sstable/table.h` 的 `ReadStats` | 追加 `filter_*` 计数列 | M3/M4 既有列语义不变 |
| `src/sstable/table.h` 的 `Table` | 追加 `filter_state()` / `KeyMayMatch()` / `filter_bytes()` | 只增成员函数 |
| `src/sstable/table_builder.h` 的 `TableBuilder` | 追加 `filter_bytes()`；构造时按 `options.bloom_bits` 建 filter | 只增 |
| `src/sstable/format.h` | 追加 `inline constexpr char kBuiltinBloomFilterName[]` 与 `kFilterBaseLg` | 只增常量；**不改** `kTableFormatVersion` |
| `src/db_impl.h` 的 `AmplificationStats` / `FormatAmplLine`（M4.3 在途） | 追加 filter/bench 列 | **只追加在行尾**；M5.0 复核 M4.3 落地形态 |
| `scripts/lsm_gate.sh` | 追加 `--require-m5` 与 M5 腿 | 既有腿不变 |

#### 1.3.3 M5 **不**解除的既有禁令

- 不改 M3 的数据块/索引块格式（`M5:121`、`m3-design.md:582-655`）。
- 不改 M4 的 compaction 正确性逻辑与 tombstone 丢弃条件（`M5:121`）。
- 不改 WAL record 格式（只追加批承载方式，`M5:121`；`M5-C4`）。
- 不改前四阶段既有测试的断言（`M5:130`）。
- 不改 raft-kv 仓库任何文件（`M5:121`）。

### 1.4 依赖方向（单向，禁止反向）

```
common.h
  ↑
util/{status,coding,crc32c,hash,arena,env}
  ↑
sstable/{format,block,filter_policy,bloom,table_builder,table}
  ↑
{version_edit, version_set, compaction, merging_iterator, db_iter}
  ↑
{memtable, wal, write_batch}
  ↑
db / db_impl
  ↑
bench/bench_lsm.cpp   （只通过公共 DB / Options 驱动，禁止为了基准暴露内部结构，`M5:166`）
```

三条硬规则：

1. `filter_policy.h` / `bloom.{h,cpp}` / `util/hash.{h,cpp}` **不得**反向依赖 `db_impl`、`version_set`、`wal`
   （`M5:164` 的「filter 与 batch 不得反向依赖 db_impl」）。
2. `write_batch.{h,cpp}` 只依赖 `common.h` / `util/coding.h`；**不得** include `db_impl.h` 或 `wal.h`。
3. `bench/bench_lsm.cpp` 只 include `db.h` / `common.h`（公共接口）；**不得** include `db_impl.h`、
   `version_set.h`、`sstable/*` 等内部头（`M5:166`）。
   ⇒ 块读计数器证据由 **`tests/filter_test.cpp`**（A 组，可直调 `PersistentDBImpl` 诊断）与
   `scripts/lsm_level_stats.cpp`（M4.3 的诊断入口）承担，**不**塞进基准程序。

### 1.5 不变量与锁纪律的映射预告

- M5 新增不变量：**`I47~I56`**（对应指令 `I43~I52`）。
- M5 新增锁纪律：**`L30~L35`**（对应指令 `L27~L32`）。
- 完整映射表与「谁保证 + 怎么验」见 §9；`docs/m5-prerequisites.md` 必须逐字附同一张表。
- M5 **不新增锁**：沿用 M2 的 `commit_mu_ → mutex_`（`docs/m2-design.md:108`）与 M4 的唯一全序
  `install_mu_ → deletion_mu_ → mutex_`（`docs/m4-design.md:1986-2016`）。

---

## 2. 开放决策记录（候选 → 取舍 → 推荐 → 影响面）

> 组织：§2.1 = `M5:59-74` 的 8 条指令决策（D1~D8）；§2.2 = `M5-C1`~`M5-C8` 的裁决；
> §2.3 = 本轮新发现的补充决策（E1~E8）。
> **用户已授权：一律按「推荐」执行**（§13 逐条复述）。

### 2.1 指令的 8 条开放决策

#### D1 Bloom 参数与粒度（`M5:60`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| bits/key 与 k | (a) 10 bits/key + `k=7`；(b) 10 bits/key + `k=6`（LevelDB 的 `0.69` 截断）；(c) 可配 | `k` 由公式 `k = round(bits_per_key · ln2)`：`bits_per_key=10` ⇒ `k=7`；实测误判率略优于 `k=6`；LevelDB 格式允许每个 filter 自带 k，读者以**存储的 k** 为准，故两种都能读 | **(a)**：`10 bits/key`、`k = round(bits_per_key · 0.693147)` 并 clamp 到 `[1,30]` ⇒ 默认 `k=7`；误判率理论上界 ≈ `(1-e^{-0.7})^7 ≈ 0.82%` |
| 哈希 | (a) 多个独立哈希；(b) Double Hashing `h_i = h1 + i·h2` | (a) 需要 k 次哈希，CPU 贵；(b) LevelDB `BuiltinBloomFilter2` 的语义，`h2` 取 `h1` 的循环移位，保证 `i` 与位数互质（`m` 取 8 的倍数，`h2` 为奇数） | **(b)** Double Hashing；公式与哈希算法见 §3.3/§4 §12 |
| 粒度 | (a) 每文件一个 filter（对所有 key 一个 bitset）；(b) 每数据块一个 filter；(c) 按文件偏移每 2 KiB 一个 bucket（LevelDB 的 `kFilterBaseLg=11`） | (a) 误判率随 key 数线性上升，且一次否定只能省「整个文件」；(b) 粒度取决于 `block_size`（默认 4096），与 `M5:61` 的「每 2 KB」不符，且块大小可配 ⇒ 覆盖粒度漂移；(c) 与 `M5:10` 的「每个 SSTable 一个 filter block」不冲突（**一个块、内部多 bitset**），且把查询映射到数据块所属的 2 KiB 桶，能直接省「一次数据块读」 | **(c)**：**每个 SSTable 恰好一个 filter block**，块内按 `kFilterBaseLg=11`（2 KiB）切成多个独立 bitset；这是 `M5-C2` 的裁决 |
| filter 编码 | (a) bitset + 尾部全局 k + 全局长度；(b) LevelDB `BuiltinBloomFilter2`：`filter[i] = bitset ‖ k(1B)`，尾部 `offset[] ‖ array_offset ‖ n` | (a) 只支持单一 k、无法表达逐桶不同位数；(b) 已被 `protocol.md:323` 的名字空间指名，且把 k 放在每个 filter 尾部，天然支持「桶内 key 数不同 ⇒ 位数不同」 | **(b)**；位级布局见 §4 §12 |

**影响面**：§3、§4 §12、§5.1、§9 的 `I47~I50`、§10 的 `M5-A01~A10`。
**负结果入档**：若 10 bits/key 在某负载下净收益为负（filter 体积/构建成本 > 省下的块读），
按 `M5:115` 与 `RM:61` 记入 `docs/m5-bench.md` 的负结果节，**不**改默认参数去硬凑。

#### D2 Bloom 与多版本 / tombstone 的交互（`M5:62`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| filter 按哪一级 key 建 | (a) internal key（含 sequence/type）；(b) **user key** | (a) 查询用 `lookup_key`（user_key ‖ snapshot trailer），同一 user key 的每个版本都会算成不同的 filter key ⇒ 对某个 snapshot 查旧版本时，filter 里只有新版本的 internal key ⇒ **假阴性（丢数据级）** | **(b) user key**；写入时对**每一条 entry（含 tombstone）**取 `ExtractUserKey(key)` 加入当前桶的 key 集合，集合语义（同 user key 去重不加也可以，重复只会提高误判率） |
| tombstone 是否进 filter | (a) 只加 value entry；(b) **加所有 entry（含 `kTypeDeletion`）** | (a) 若某文件只有某个 user key 的 tombstone，读路径可能因 filter 否定而跳过该文件，继续向更旧文件查到旧值 ⇒ **删除复活**（假阴性的一种）；(b) 强制读该文件才能看到 `kDeleted` | **(b)**；`I47` 的「已写入且未被删除的 key」包含「有 tombstone 的 key」——tombstone 也是该文件对该 user key 的「存在性证据」 |
| filter 与可见性的关系 | (a) filter 参与「是否存在」判断；(b) filter 只回答「这个文件是否**可能**含该 user key」 | (a) 会把 filter 变成读路径的第二真相源，破坏 `M1` 的可见性规则与 `M3` 的三态 `TableGetResult`；(b) 与 `M5:34`/`I49` 一致 | **(b)**；filter 只在「决定是否读这个数据块」这一处使用，命中 `kFound`/`kDeleted`/`kNotFound` 仍由正常读路径决定 |

**影响面**：§3.7、§9 的 `I47`/`I49`、§10 的 `M5-A03/A05`。

#### D3 filter 的持久化与版本兼容（`M5:63`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| filter 的挂接点 | (a) footer 新增 `filter_handle`；(b) metaindex 多一条 name → handle | (a) 改 footer 44 B 布局 ⇒ 与 M3 冻结的 `kFooterSize`/`kTableFormatVersion` 冲突，所有 M3 旧文件按 `version != 1` 判 `kNotSupported`；(b) M3 已写空 metaindex 并让 reader 容忍未知 name（`m3-design.md:656-671`、`table.cpp:143-162`），**footer 零改动** | **(b)** `name = "filter.leveldb.BuiltinBloomFilter2"` |
| 版本号 | (a) `kTableFormatVersion` 升到 2；(b) 保持 1 | (a) `format.h:70` 注释逐字「`version != 1 ⇒ kNotSupported`」⇒ 升版会让旧文件读不了，与 `M5:63` 的「旧格式文件（无 filter）的行为」冲突；(b) 用 metaindex 名字空间演进 | **(b)** `M5-C3`：**不升**；`footer` 与 `kTableFormatVersion` 逐字不变 |
| 旧文件（无 filter） | (a) 报错拒绝；(b) 照常读，filter 状态 = `kAbsent`，等价于「可能存在」 | (a) 违反 `M5:35` 降级纪律；(b) 与 `M3` 的 reader 行为一致 | **(b)** |
| filter 与数据块集合的对应校验 | (a) 不校验，信任 metaindex；(b) 在 `Table::Open` 校验 handle 边界、payload 结构、offset 数组自洽；不一致时**禁用 filter**并计数 | (a) 错位 = 假阴性；(b) 与 `I48` 一致；但**不得**因 filter 损坏让 `Table::Open` 失败（`M5:35`） | **(b)**：结构自洽则可用；任一不自洽 ⇒ `filter_state = kCorrupt`，`KeyMayMatch` 一律返回 true（可能存在），并计数 |

**影响面**：§3.4、§3.7、§4 §12、§9 的 `I48`、§10 的 `M5-A04/A06/A07/A08/A09`。

#### D4 WriteBatch 的编码与原子性（`M5:64`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 编码格式 | (a) 自定义；(b) `protocol.md:185-204` 的 `sequence ‖ count ‖ entry*` | (a) 会与 M2 的 WAL 解析器各写一套，违反「不得各写一套」（`protocol.md:3-4`）；(b) M2 已冻结、已有 `ParseBatch` 实现（`db_impl.cpp:51-113`）与恢复用例 | **(b)** 逐字同构 |
| WAL 承载 | (a) 一个 WriteBatch 一条 record；(b) 多 record + 批边界标记 | (b) 破坏「一条 record = 一个 CRC = 一个原子单位」（`docs/m2-design.md:151` 决策 A），且 M2 的解析器只认 batch payload；`M5-C4` 是**阻断级**裁决 | **(a)**：一个 WriteBatch 的全部 entry **必须落在同一条 WAL record 内**；组提交可把多个 WriteBatch 合并到同一条 record（每条 record 仍是一个 batch payload），但**禁止**把一个 WriteBatch 拆到两条 record |
| MemTable 提交组 | (a) 逐条 `Add`，失败就部分可见；(b) **整批容量预检 + 整批预校验 + 整批一次性插入** | (a) 半批可见，违反 `I51`；(b) 见 §5.4 的证明：`WouldReject(total_footprint)` 为 false ⇒ 后续每条 `Add` 的 `WouldReject(0)` 必为 false，且输入已预校验 ⇒ `Add` 不会失败 | **(b)** |
| 批大小上限 | (a) 只受 `kMaxBatchCount=1<<20`；(b) 只受 `kMaxLogicalRecordSize=64 MiB`；(c) 两者都受 + 明确的 policy 常量 | (a) 64 MiB 的 entry 字节仍可能配出超大 record；(b) 漏掉 count 上限；(c) 两道独立上限都写死，超限在**编码前**返回错误 | **(c)**：`WriteBatch::kMaxCount = 1<<20`（协议）；`WriteBatch::kMaxBytes = kMaxLogicalRecordSize`（协议）+ `ByteSize()+16 <= kMaxLogicalRecordSize`（与 `db_impl.cpp:160` 同形）；超限返回 `kInvalidArgument`，不得写到一半失败 |
| `count == 0` | 允许空批 / 拒绝 | `protocol.md:198` 明确 `count = 1 .. kMaxBatchCount` ⇒ 空批非法 | **拒绝**：`DB::Write` 返回 `kInvalidArgument`（`M5-R6`） |

**影响面**：§5、§4 §13、§9 的 `I51~I54`、§10 的 `M5-A11~A17`、`M5-B01`。

#### D5 WriteBatch 与 M2 组提交的协作（`M5:65`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 批内原子性与组窗口 | (a) 每个 WriteBatch 单独成组；(b) 与其它写者一起并入一个组 record | (a) 批大时 `kMaxGroupRecs`/`kMaxGroupBytes` 会限制吞吐，且失去组提交收益；(b) 一条 record 完整承载多个批的全部 entry，**每个批仍全部落在同一条 record 内**，崩溃时 record 级原子 ⇒ 每个批要么全可见要么全不可见；批内 sequence 连续仍成立 | **(b)**；`RunFlusher` 的 `EncodeGroup` 扩展为「总 count + 各成员 entry 拼接」，`Pending` 记 `entry_count`/`entries` |
| 批内 sequence | (a) 每条 entry 各分配一次（逐条加锁）；(b) 在组内一次性分配 `[begin, begin+total_count)`，批内按 entry 顺序连续 | (a) 锁竞争大、窗口不原子；(b) 与 `I52` 一致，且与恢复的 `ParseBatch`（`seq+i`）天然对齐 | **(b)** |
| `sync=true` | (a) 每批各 fsync；(b) 组内 `OR`，一次 fsync 覆盖整组 | (a) 吞吐退化；(b) 沿用 M2 `D3` 的组内 OR（`db_impl.cpp:281`），返回任一写者前 fsync 已完成 ⇒ 批粒度 durable-before-ack 成立 | **(b)** |
| 提交失败回滚 | (a) MemTable 逐条 undo；(b) **WAL 失败时不插入内存**；预检失败时整批拒绝；预检通过后插入不会失败 | (a) MemTable 无 undo 接口，且要改 M1 结构；(b) 见 §5.4 的证明 | **(b)** |
| 丢唤醒 | 沿用 M2 的 `commit_mu_` + `flusher_active_` + `commit_cv_`（`db_impl.cpp:166-215`） | 不动唤醒协议；批只是「更大的 Pending」 | **不改**；`M5-A14` 复用 M2 的无丢唤醒断言 |

**影响面**：§5.3/§5.4/§5.5、§9 的 `I51~I54`、§10 的 `M5-A12/A14/A16`。

#### D6 缓存与其它优化的取舍（`M5:66`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 块缓存 | (a) 引入块级 LRU；(b) 不引入 | 见 §1.2 第 6 条；M4 已不引入，M5 引入会混淆 filter 收益归因 | **(b)** 不引入；登记为 M6+ 候选 |
| table cache | (a) 新建 `src/table_cache.*`；(b) 复用 M3 的 `TableCache` | M4 已裁决复用（`m4-design.md:2581-2588`）；filter 随 `Table` 打开 | **(b)** 复用；不新增 fd |
| mmap | (a) 做；(b) 不做 | `M5:26` 非目标；风险见 §1.2 第 2 条 | **(b)** 不做；登记 |
| 压缩 | (a) 做；(b) 不做 | `M5:26` 非目标；会改数据块格式 | **(b)** 不做；登记 |
| 其它「看起来能用但没实现」的字段 | — | `D3:462` 的纪律 | M5 **不新增**未实现的优化字段；`Options::bloom_bits` 是唯一新增，且真实生效 |

**影响面**：§1.2、§12.4、§12.5；M5 的数据表不含块缓存行。

#### D7 基准方法学（`M5:67`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 数据集与 key 分布 | 顺序 key、均匀随机、热点（Zipf） | 三类都测会让矩阵爆炸；`M5:12` 只要求四类负载 | **固定两种 key 分布**：`seq`（固定宽度 8B 大端递增，`KeyFromIndex` 风格）与 `uniform`（固定种子均匀随机，读多写少时用同一集合的 shuffle 顺序）；热点分布登记为「未做」，不编造 |
| 数据集规模 | 小/中/大 | 规模必须能触发至少一轮 compaction，否则「LSM 值得用在哪」无从回答 | **默认 `dataset=100000`、`value_size=100B`、`write_buffer_size=1 MiB`**（可用参数改）；必须打印实际值 |
| 预热 | 无 / 固定条数 | 不预热会把 page cache 冷启动算进 LSM 劣势 | **预热 `warmup=10000` 次操作**（默认），预热结果丢弃；打印 `warmup` |
| 每格重复与统计 | 单次 / 多次取中位数 | 单次会被抖动支配 | **`repeats=3`（默认）取中位数**；同时打印 `min/median/max`，P99 取三次中位数的 P99（并打印三次原始值） |
| 随机读偏移生成 | 每次 `rand()` / 固定种子 shuffle | 不可复现的随机源会让结论不可复现 | **固定种子 PRNG + 固定序列**；打印 `seed`；与 `tests/test_harness.h` 的 Park–Miller 同源或用 `std::mt19937_64` 但**固定种子**并打印 |
| 同轮交替 A/B | 两轮分别跑 / 同一轮内交替 | 机器状态漂移（M2 实测同一脚本不同时间可差 3 倍） | **同一脚本、同一轮内交替**（如 `filter=0,filter=1,filter=0,filter=1,...`），照 `RKV scripts/bench_m5_ab.sh` 的头部注释形状 |
| 绑核与频率 | `taskset` 绑核 / 不绑 | 绑核在 VM 上可能失败；不绑会引入迁移噪声 | **默认不绑核**（VM 上 `taskset` 可能不可用）；若绑核则必须在头部打印 `cpu_affinity`；不绑核时打印 `loadavg` 并作为有效性判据 |
| 机器状态 | 声明 / 不声明 | M2 的教训 | **必须打印** `MACHINE nproc=... load1=... load5=... load15=... fs=... mount=...`；`load1 > 8`（8 vCPU）时把该轮标 `UNRELIABLE` 而不是静默采用 |
| 不可外推声明 | 写 / 不写 | `M5:199`、`RM:60` | **写死**：所有结论只对「本机、单块 ext4、loopback 级、该数据集/参数」成立；禁止外推为通用结论 |

**影响面**：§8、§7、§10 的 `M5-B02/B05`。

#### D8 结论表达与负结果（`M5:68`）

| 决策点 | 候选 | 取舍 | 推荐 |
|---|---|---|---|
| 数据表列 | 只放吞吐 / 延迟；或加放大与 filter 代价 | `M5:15`、`M5:182` 要求三个放大最终口径 + filter 体积代价 | 见 §6.6 的固定列 |
| 对比基准 | 只和裸文件比；或同时给「LSM 内 filter 开/关」同轮对照 | `M5:18` 要的是 filter 带来的块读下降；不同基线回答不同问题 | **两类都做**：① LSM vs 裸文件/`std::map`（回答「LSM 值得用在哪」）；② filter on/off 同轮（回答「Bloom 值不值」） |
| 负结果 | 只留成功数字 / 全部入档 | `M5:115`、`RM:61`；raft-kv 的 §3.9/§3.10/§3.11 是先例 | **全部入档**：数据 + 归因 + 是否作废 + 原文保留；作废行用 `NOT_APPLICABLE`/`INVALIDATED` 显式标记 |
| 选择性报告 | 只测 filter 命中场景 | `M5:68` 明确禁止 | 四类负载 × 三类对照 × filter on/off 的**固定矩阵**全部跑；未做的格子写 `NOT_APPLICABLE` + 理由，不得省略 |
| 「提升 N 倍」 | 写 / 不写 | `M5:199` | **禁止**写未实测的倍速；`≥3×` 只在 §3.8 限定的作用域内作为**实测计数比值**出现 |

**影响面**：§6.6、§10 的 `M5-B02/B10`、`docs/m5-bench.md`（`#1`/M5.3 产出）。

### 2.2 矛盾 / 歧义裁决（`M5-C1`~`M5-C8`）

#### C1（= `M5-C1` [编号]）新增 I/L 号与 M3/M4 冻结契约撞号

- **原指令**：`M5:84` 说维持「M3 的 `I21~I30`、M4 的 `I31~I42`」；`M5:96` 说「沿用 `L1~L26`，新增 `L27~L32`」。
- **实测**：M3 冻结的是 `I21~I34` / `L13~L21`（`docs/m3-design.md:1876-1922`）；M4 冻结的是 `I35~I46` / `L22~L29`
  （`docs/m4-design.md:1926-1985`）。指令写的 `M3 I21~I30 / L1~L26`、`M4 I31~I42` 都是**旧号**。
- **裁决（随 M4-C1 顺延）**：M5 新增 **`I47~I56`**、锁纪律 **`L30~L35`**；沿用 `I1~I46` / `L1~L29`。
- **理由**：若照抄，`M5:85` 的 `I43` 会与 M3 的 `I43`（不存在，但 M4 的 `I43` 存在）以及 M4 的 `I35~I46` 交叉；
  `L27~L32` 会与 M4 的 `L27~L29` 撞号。`docs/m4-design.md:1955` 末段已明确把 `I47~I56 / L30~L35` 留给 M5。
- **落地**：§9.1 的完整映射表；`docs/m5-prerequisites.md` 必须逐字附同一张表。

#### C2（= `M5-C2` [表述]）「每个 SSTable 一个 filter block」vs「块级 filter（每 2 KB）」

- **原指令**：`M5:10` 说「每个 SSTable 一个 filter block」；`M5:61` 说「块级 filter（每 2 KB 数据一个 filter）vs 文件级单 filter」。
- **裁决**：唯一解释 = **每个 SSTable 恰好一个 filter block**（metaindex 只能存一个 handle，`M5:10`/`M5:14` 要求进 metaindex）；
  该 filter block **内部**按 `kFilterBaseLg=11`（2 KiB）切成多个 bitset；`M5:61` 是「filter 覆盖粒度」的选择，不是「块类型」的选择。
- **理由**：`M5:18` 的判据是「**数据块**读取次数下降」⇒ 覆盖粒度必须能映射到数据块；LevelDB `BuiltinBloomFilter2`
  的 `kFilterBaseLg=11` 语义正好如此。`protocol.md:323` 已把名字空间定为 `filter.leveldb.BuiltinBloomFilter2`。
- **落地**：§3.2/§3.3、§4 §12。

#### C3（= `M5-C3` [表述]）「随格式版本兼容演进」vs「`kTableFormatVersion` 不升」

- **原指令**：`M5:10` 说「随格式版本兼容演进」；`M3` 冻结「footer 布局不变、`kTableFormatVersion` 不升」
  （`m3-design.md:656-671`、`format.h:45`）。
- **裁决**：**不升** `kTableFormatVersion`；「版本兼容演进」= 靠 metaindex 的**名字空间** `filter.leveldb.BuiltinBloomFilter2`。
- **理由**：`format.h:70` 注释逐字「`version != 1 ⇒ kNotSupported`」；升版会让所有 M3/M4 旧文件读不了，
  与 `M5:63` 的「旧格式文件（无 filter）的行为」冲突。M3 写空 metaindex 的整段论证就是为此（`m3-design.md:656-671`）。
- **落地**：§3.4、§4 §12。

#### C4（= `M5-C4` [阻断]）WriteBatch 的 WAL 承载方式

- **原指令**：`M5:64` 让 `#0` 在「一次 WAL record 承载整批」与「多条 record + 批边界标记」间选；
  `M5:11`/`M5:37` 又都写「一次 WAL record 承载整批」。
- **既有冻结**：`protocol.md:185-204` 已冻结「一条逻辑 record 的 payload 是一个 batch」；
  `protocol.md:204` 逐字「本编码与 M5 的 `WriteBatch` 落盘布局同构」；`db_impl.cpp:219-245` 的 `EncodeGroup` 已按整批实现。
- **裁决（阻断级）**：**必须**沿用「一个 batch = 一条 WAL record」；决策 4 只剩「批大小上限 / 超限处理」（见 D4/§5.6）。
  一个 `WriteBatch` 的全部 entry 必须落在同一条 record 内；组提交可以把多个 WriteBatch 合并到同一条 record，
  但**禁止**把单个 WriteBatch 拆到两条 record。
- **理由**：改承载方式同时违反 `M5:121` 与 M2 的既有解析器（`db_impl.cpp:51-113` 的 `ParseBatch`）。
- **落地**：§5.1/§5.3/§5.5、§4 §13。

#### C5（= `M5-C5` [表述]）`≥3×` 与「不写提升 N 倍」

- **原指令**：`M5:18` 说「例如 ≥3× 减少；具体阈值由 `#0` 定并写进设计文档」；`M5:199` 说「不写提升 N 倍这类未测数字」。
- **裁决**：允许把 `≥3×` 作为**门禁**，但必须同时满足：
  (a) **同一脚本、同一数据集、同一轮内开关 filter 的两列**（不是两次不同轮次）；
  (b) 只声明为「该数据集上、该查询模式下、**探测不存在的 key** 时」的实测**数据块读取次数**比值；
  (c) 阈值写死为 **3.0**，并在 `docs/m5-bench.md` 记录 `blocks_without_filter` 与 `blocks_with_filter` 两个原始计数；
  (d) 若实测 <3.0，按 `M5:199` 的纪律**先证伪/改口径**（例如说明该数据集下 LSM 层级已使 `files_checked` 很小），
      **禁止**为了让门禁变绿而改默认配置。
- **口径**：`ratio = blocks_read_without_filter / max(1, blocks_read_with_filter)`；若 `blocks_read_with_filter == 0`，
  则要求 `blocks_read_without_filter >= 3`；两者都必须来自同一次 `PersistentDBImpl` 会话的 `GetReadStats()` 增量。
- **落地**：§3.8、§9 的 `I47`、§10 的 `M5-A10`。

#### C6（= `M5-C6` [表述]）`bench_lsm.sh` 的 `missing != 0` 门禁 vs 无对账集合的对照

- **原指令**：`M5:14` 说「数据不一致或对账 `missing != 0` 时退出码 1」；`M5:12`/`M5:148` 的对照是
  「裸文件 `write+fsync` / 裸文件 `pwrite` 随机 / 纯内存 `std::map`」——这三者没有 sidecar 对账语义。
- **裁决**：`missing != 0` 硬门禁**只施加在 LSM 腿**。`bench_lsm.sh` 的固定行里必须有
  `engine=<lsm|raw_file|raw_pwrite|std_map>` 与 `verify=<lsm|na>` / `missing=<n|na>`；门禁只检查 `engine=lsm` 的行。
  裸文件与 `std::map` 腿只产出吞吐/延迟对照列，`verify=na`、`missing=na`，**不参与**对账。
  失败注入自测**只**构造一个 LSM 单元格的 `missing != 0`，并断言脚本返回 1。
- **理由**：否则门禁要么无法实现（裸文件无对账），要么被做成恒真（永远 0）——后者正是 `M5:113` 登记的风险。
- **落地**：§7.3/§7.4、§9 的 `I56`、§10 的 `M5-B02/B03`。

#### C7（= `M5-C7` [表述]）filter 默认开关与 `Options` 新字段

- **原指令**：`M5:10` 说「默认 10 bits/key」（= 默认开）；`M5:12`/`M5:21` 又要求对照与「默认配置不得修改」；
  `M5:119` 的【必须改】**未列** `src/common.h`，但实测 `Options` 没有任何 filter 字段（`common.h:285-314`）。
- **裁决**：
  (a) 在 `Options` 追加 **`int bloom_bits = 10;`**（`0` = 关闭；`1..64` 合法）。
      采用 `int` 而不是 `const FilterPolicy*`，以保持 `common.h` **零项目依赖**（`roadmap.md:22-26` 的依赖纪律；
      若用 `FilterPolicy*`，`common.h` 必须 include `filter_policy.h`，会形成 `common → filter_policy → common` 的环）。
  (b) **新文件默认写 filter（开）**；对照实验通过显式 `--bloom-bits 0`（或 `Options.bloom_bits=0`）关闭，
      并把该参数打印进结果文件头部（满足 `M5:36`）。
  (c) 旧文件无 filter 时按「照常读」降级（`M5:35`）；即使 `bloom_bits=0`，reader 也**仍会尝试解析** metaindex 里的 filter
      （因为旧/新文件可能带 filter；读 filter 不需要 `bloom_bits`，见 §3.5），结果正确。
- **理由**：`D1` 的 `Options` 在 M2/M3 已有「追加字段 + 登记」的先例（`common.h:291-298`）；`D3` §12.4 早就把
  「`Options::bloom_bits`」标注为 M4/M5 的内容。
- **落地**：§3.5、§3.6、§5.6、§9 的 `I50`、§10 的 `M5-A06/A07`。

#### C8（= `M5-C8` [表述]）mmap / 压缩是「已决定不做」还是「开放决策」

- **原指令**：`M5:26` 非目标说「mmap 读取（列方案但默认不做）、压缩算法（列出但默认不做）」；
  `M5:66`（决策 6）又把 mmap 的取舍列为开放决策，并允许「不做但必须写明理由」。
- **裁决**：以 `M5:26` 为准 —— **M5 不做 mmap、不做压缩**；决策 6 只产出「列风险 + 不做理由」的**登记段**（3 行以内），
  不产出方案对比表。`M5:78` 的自检按「未引入」核对。
- **理由**：非目标是硬边界（`roadmap.md:44`「每阶段的『非目标』段是硬边界，评审按此对照检查」）；
  两者一致时取更严的一侧。
- **落地**：§1.2 第 2/3 条、§12.4。

### 2.3 本轮新发现的歧义 / 补充决策（E1~E8）

> `docs/m3-design.md:1852-1866` 的 E1~E8 与 `docs/m4-design.md:772-791` 的 E1~E10 是同形先例：
> 指令未规定、但实现必然要碰的拍板项。E 号**不占** `I/L` 号空间。

#### E1 「读路径在打开文件前先做否定判断」的精确落点

- **歧义**：`M5:10` 逐字「读路径在打开文件前先做否定判断」；但 filter 的 handle 在 **metaindex** 里（`protocol.md:323`），
  不打开文件/不读 metaindex 就无法知道 filter 在哪。`M5:164` 允许接入点落在「决定是否打开文件/是否读块」之一。
- **唯一解释**：filter **不能省 SSTable 的 footer + metaindex + index 读**，只能省 **数据块读**；
  `M5:10` 的「打开文件前」在本文解释为「**读数据块之前**」，这正是 `M5:18` 的判据（数据块读取次数下降）。
- **代价与缓解**：`TableCache` 把 `shared_ptr<const Table>`（含 filter 字节）按文件号缓存（`version_set.h:195-224`），
  同一文件的重复点查不重复读 footer/metaindex/index/filter；filter 块**随 `Table::Open` 一次性读入**（`M5-R2`），
  不另开 fd、不单独读。
- **登记**：这条解释写入 §3.7，并在 `docs/m5-bench.md` 的「LSM 劣势场景」里如实写「filter 省的是数据块读，
  不是文件打开成本；文件数很多而点查很稀疏时，metaindex/index 的固定成本仍在」。
- **不做的替代**：若要把 filter handle 放进 `FileMetaData`/MANIFEST 以省 `Table::Open`，需要改 M4 冻结的
  `VersionEdit`/`FileMetaData`（`protocol.md:425-447`），超出 M5 的「只新增块」边界 ⇒ **不做**，登记。

#### E2 `block_size` 默认 4096 vs filter 粒度 2 KiB

- **歧义**：`M5:61` 写「每 2 KB 数据一个 filter」，但 `Options::block_size` 默认 4096（`common.h:295`、`m3-design.md:815`）。
- **唯一解释**：2 KiB 是 **filter 覆盖粒度**（`kFilterBaseLg=11`），4 KiB 是**数据块目标大小**；两者独立。
  filter 桶按「数据块起始偏移 `>> 11`」映射；一个 4 KiB 数据块的 key 全部落在其起始偏移所属的桶。
- **正确性**：`FilterBlockBuilder::StartBlock(block_offset)` 用
  `while (target > filter_offsets_.size()) GenerateFilter();`（LevelDB 的逐桶补空过滤器写法），
  保证 `filter_offsets_.size() > target`，从而 `KeyMayMatch` 的 `index = block_offset >> 11` 永远落在有效 filter 上；
  被跳过的空桶写 0 字节 filter，对应「该桶没有数据块」，返回 false 是安全的（不会误伤真实数据块）。
- **登记**：§3.3。

#### E3 filter 块 CRC 是否受 `Options::verify_checksums` 控制

- **歧义**：`M5:35` 要求 filter 损坏时降级照常读；`M3` 的 `Table::ReadBlock` 在 `verify_checksums=false` 时
  **跳过 payload CRC**（只保留结构校验，`table.cpp:199-247`、`m3-evidence.md:41-54` 未闭合项 7）。
- **唯一解释**：**filter 块的 CRC 始终校验**，不受 `Options::verify_checksums` 影响。
  理由：filter 的假阴性是丢数据级（`I47`），而它对 `verify_checksums=false` 的「省一次 CRC」收益极小
  （filter 块每文件只读一次）；若用户关掉校验就允许使用未校验的 filter 字节，一次 bit flip 就可能造成假阴性。
- **落地**：`Table::Open` 走一个私有的 `ReadBlockImpl(..., bool verify_crc)`，filter 调用 `verify_crc=true`；
  公开的 `ReadBlock(handle, expected, payload, stats)` 语义不变（仍按 `options_.verify_checksums`）。
  若 filter 块 CRC 不符 ⇒ **不报错**，置 `filter_state=kCorrupt`，`KeyMayMatch` 返回 true 并计数（`M5:35`）。
- **代价**：filter 块 CRC 计算量 = 一次 filter 块大小（10 bits/key 时约 1.25 B/key）；对 100 万 key 约 1.25 MB，
  只在文件打开时算一次；可忽略。
- **登记**：§3.7、§12.5 的已知薄弱点。

#### E4 filter 的「错位注入必须让测试失败」怎么证明测试有效

- **歧义**：`M5:138` 末句「错位注入必须导致测试失败（证明测试有效）」；但按 `M5:35`/`I48`，错位应当被**降级**处理
  （禁用 filter、照常读），此时读结果不会错 ⇒ 表面上测试不会失败。
- **唯一解释**：所谓「测试失败」指的是「**注入被检出**」：A 组用例必须断言
  `filter_state == kCorrupt` 或 `ReadStats.filter_unavailable > 0`（即注入确实生效、filter 未被使用），
  并同时断言读结果仍正确。若实现误用了错位 filter，`KeyMayMatch` 会对一个存在的 key 返回 false ⇒
  读路径跳过该文件 ⇒ `Get` 返回 `kNotFound`（或读到旧值）⇒ 结果断言失败。
- **构造**：`M5-A04` 构造一个 filter block，把 `offset[i]` 指向错误的 filter（或把 `array_offset` 改小），
  重新计算块 CRC 使其**结构上看起来合法但语义错位**（这比「CRC 坏」更强，能穿透 CRC 校验）；
  断言 `Table::Open` 检出 `filter_state=kCorrupt`（或至少 `KeyMayMatch` 未被使用）且 `Get` 仍返回正确值。
- **登记**：§10 的 `M5-A04`。

#### E5 `bench_lsm` 与被测库解耦 vs 块读计数器证据

- **歧义**：`M5:166` 要求基准程序「通过公共接口驱动，禁止为了基准暴露内部结构」；
  `M5:139` 又要求「用计数器断言读次数下降」。
- **唯一解释**：**计数器证据不放在 `bench_lsm`**。计数器证据由 `tests/filter_test.cpp`（A 组，可直接使用
  `PersistentDBImpl` 的诊断接口，这是本仓库既有的测试 seam，见 `docs/m3-prerequisites.md:686-702` D9.4）
  与 M4.3 的 `scripts/lsm_level_stats.cpp`（内部诊断工具）承担。`bench_lsm` 只输出
  `CELL/THROUGHPUT/LATENCY/P99` 与 `missing`，不 include 内部头。
- **落地**：§1.4、§3.8、§7.1。

#### E6 `FakeClock` 不存在

- **事实**：本轮 grep `tests/` 与 `src/` 对 `FakeClock` **零命中**（M4 的 A 组也没落地它；`notes.md:239` M5-R9 已登记）。
- **唯一解释**：M5 的 A 组**不得**假装 `FakeClock` 存在。A 组的确定性计时/事件序列改用：
  `MemEnv`（`tests/memenv.h`）+ `CountingEnv`（`tests/sstable_counting_env.h:68`）+ `FaultyEnv`（`tests/faulty_env.h:21`）
  + 显式的 `Env` 事件序列断言；M5 的基准用真实时间，且用「同轮交替 + 中位数 + 机器状态」对抗抖动。
- **可选**：若 M5.1 确实需要「把 `Env::NowMicros` 冻结」来做确定性微基准的单元测试，则在 `tests/test_harness.h`
  **追加**最小 `FakeClock`（只属于 tests/，不得进 src/），并在 `docs/m5-prerequisites.md` 登记为「M5 新增测试辅助」。
- **登记**：§10 的 A 组前置假设、§12.5。

#### E7 `AMPL` 行由 M4.3 在途实现，M5 的列追加点未冻结

- **事实**：`git show db5aa8f:src/db_impl.cpp` **没有** `FormatAmplLine`/`GetLevelStats`/`GetAmplificationStats`；
  工作树的未提交 diff 才加上它们。⇒ M5 的「在 `AMPL` 行尾追加 filter 列」目前**没有稳定落点**。
- **唯一解释**：M5.0 必须先复核 M4.3 落地后的 `AmplificationStats` 与 `FormatAmplLine`；M5 的追加规则是：
  **只允许行尾追加**（`m4-design.md:2139-2145`），列名固定为 §6.6 的列表；
  若 M4.3 的列名与本文不一致，以 M4.3 落地为准并在 `docs/m5-prerequisites.md` 登记差异。
- **阻断条件**：若 M5.0 时 M4.3 仍未提交，M5.1 的 filter 计数器可以只落在 `ReadStats`/`DbReadStats` 与测试里，
  **但** `AMPL` 行追加必须等 M4.3 提交后再做；M5.3 的基准数据表不得基于未提交的 M4.3 统计。
- **登记**：§11 M5.0、§12.5。

#### E8 基准的「同轮交替 A/B」到底交替什么

- **歧义**：`M5:67` 的「同一轮内交替跑 A/B」在 M5 里有两个 A/B：
  (a) **LSM vs 裸文件/内存 map**（回答 G3）；
  (b) **filter on vs off**（回答 `M5:18` 的 `≥3×`）。
- **唯一解释**：两者都用「同一轮内交替」：
  - (a) 在同一轮内按 `lsm → raw_file → std_map → lsm → …` 的顺序轮转，避免机器漂移只砸中某一类；
  - (b) filter on/off 在同一进程/同一脚本内交替 `bloom_bits=0` 与 `bloom_bits=10` 的两套 DB（同一数据集、同一查询集合），
    且 `≥3×` 只比较**数据块读计数**（不是墙钟吞吐），因为计数不受机器负载影响。
- **落地**：§6.5、§6.6、§10 的 `M5-A10/B02/B05`。

---

## 3. filter 位级格式、读路径接入与计数器

### 3.1 文件整体布局（filter 块的位置）

M3 的布局是 `data block* → metaindex → index → footer`（`m3-design.md:672-697`），校验约束是
`metaindex.offset + metaindex.size <= index.offset` 且 `index.offset + index.size == file_size - 44`。
M5 在 **data block* 与 metaindex 之间**插入恰好一个 filter 块：

```
偏移 0
  ┌──────────────────────────────────────────────┐
  │ data block 0                                 │
  │ …                                            │
  │ data block N-1                               │
  ├──────────────────────────────────────────────┤
  │ filter block (仅当 Options::bloom_bits > 0)   │  type = 0x04（kBlockTypeFilter）
  ├──────────────────────────────────────────────┤
  │ metaindex block                              │  M3 为空；M5 多一条 filter name → handle
  ├──────────────────────────────────────────────┤
  │ index block                                  │
  ├──────────────────────────────────────────────┤
  │ footer (44 B, 定长, 文件末尾)                 │
  └──────────────────────────────────────────────┘
```

- filter 块必须在 metaindex **之前**：metaindex 的 entry 要引用它的 handle。
- filter 块**不破坏** M3 的两条布局约束：`metaindex.offset + size <= index.offset` 仍成立；
  `index.offset + size == file_size - 44` 仍成立。
- filter 块**不是**数据块/索引块，M3 的 `Table::Open` 不会遍历它；M3 的 reader 只会在 metaindex 里
  看到一个未知 name，按 `table.cpp:143-162` 计数 `unknown_metaindex_entries_` 后照常读（`M5:63`）。
- **M5 追加的布局校验**（只在 filter handle 存在时）：`filter.offset + filter.size <= metaindex.offset`；
  `filter.size >= kBlockOverhead + kBlockMinPayload`；否则 `filter_state = kCorrupt`（降级，不报错）。
  这条比 M3 的校验更严，但不改 M3 的校验（新增块自有新增约束）。

### 3.2 filter block payload（LevelDB `BuiltinBloomFilter2` 语义）

```
filter_block_payload := filter[0] ‖ filter[1] ‖ … ‖ filter[n-1]
                        ‖ offset[0] ‖ … ‖ offset[n-1]      # 每个 uint32 LE，绝对偏移（相对 payload 起点）
                        ‖ array_offset                       # uint32 LE：offset[0] 的偏移
                        ‖ n                                  # uint32 LE：filter 个数

filter[i]             := bitset_i ‖ k_i(1B)
```

- `offset[i]` 是 `filter[i]` 在 payload 中的**绝对字节偏移**；`array_offset` 是 `offset[0]` 的偏移；
  `n` 是 filter 个数（4 B LE）。
- `filter[i]` 的尾部 1 字节是**该 filter 自己的 k**（不是全局 k）；bitset_i 长度为
  `ceil(m_i / 8)` 字节，`m_i` 是该桶的位数（8 的倍数）。
- **空 filter**：若某桶没有 key（例如数据块起始偏移跳过了若干 2 KiB 桶），`filter[i]` 长度 = 0
  （`offset[i] == offset[i+1]`）；reader 对长度 0 返回 false（该桶无数据块，安全）。
- `n` 可以为 0（空 filter block，payload 恰 8 B：`array_offset=0` + `n=0`）。但 `TableBuilder` 在
  `Options::bloom_bits == 0` 或 `num_entries_ == 0` 时**不写 filter 块**（metaindex 保持 M3 的空表），
  所以 8 B 的空 filter block 只可能由手工构造的测试产生。
- 块外壳仍是 M3 的 `header(5B: length(4B LE) ‖ type(1B)) ‖ payload ‖ crc32c(4B LE)`，
  CRC 覆盖 `length ‖ type ‖ payload`（`m3-design.md:698-730`、`protocol.md:248-280`）；
  `type = kBlockTypeFilter = 0x04`（`format.h:28`）。
- **`kBlockMinPayload = 8`** 对 filter 块同样适用；但 filter payload 可以恰好 8 B（n=0），
  所以结构校验不得假定「filter payload 一定含 bitset」。

**解析校验（结构自洽；任一不符 ⇒ `filter_state = kCorrupt`，不报错）**：

1. `payload.size() >= 8`；
2. `n = DecodeFixed32(end - 4)`，`array_offset = DecodeFixed32(end - 8)`；
3. `array_offset + 4 * n + 8 == payload.size()`（无 padding）；
4. 若 `n == 0`：`array_offset == 0 && payload.size() == 8`；
5. 若 `n > 0`：`offset[0] == 0`；`offset[i] <= offset[i+1]`；`offset[n-1] <= array_offset`；
6. 对每个 `i`：`begin = offset[i]`，`end_i = (i+1<n) ? offset[i+1] : array_offset`；要求 `begin <= end_i`；
7. `filter[i]` 的 `len = end_i - begin`：`len == 0` ⇒ 该桶无 key（false）；`len == 1` ⇒ 只有 k 没有 bitset
   ⇒ **视为损坏**（返回 true，计数 `filter_corrupt`），宁可不用也不冒假阴性风险；`len >= 2` ⇒ 正常。

> 第 7 条的 `len == 1` 处置是 M5 相对 LevelDB 的**安全收紧**：LevelDB 的 `len < 2 ⇒ false`，
> 但一个只有 k 字节的 filter 无法承载任何 bitset，若它对应有 key 的桶，就会产生假阴性。
> 本设计不用「len < 2 ⇒ false」的宽松写法；`len == 0` 只用于 builder 自己产生的空桶。

### 3.3 参数、哈希与假阳性率口径

**常量（新增，见 §4 §12）**：

| 常量 | 值 | 出处/理由 |
|---|---|---|
| `kBuiltinBloomFilterName` | `"filter.leveldb.BuiltinBloomFilter2"` | `protocol.md:323` 逐字冻结 |
| `kFilterBaseLg` | `11`（2 KiB） | `M5:61`、`M5-C2`、E2 |
| `kFilterBase` | `1u << kFilterBaseLg` | 与 `kFilterBaseLg` 配套 |
| `Options::bloom_bits` | `10`（默认；`0` = 关闭；`1..64` 合法） | `M5-C7`、`M5:10` |
| `kBloomMinBits` | `64` | LevelDB 口径：`bits = max(64, n * bits_per_key)`，避免小桶位数过少 |
| `kBloomMaxK` | `30` | LevelDB 口径的 clamp 上界 |

**k 的推导**：`k = max(1, min(30, round(bits_per_key · ln2)))`。
`bits_per_key = 10` ⇒ `round(6.93147) = 7`。与 `M5:10` 的「k ≈ 7」一致。
注意：`k` 存在**每个 filter 的尾部 1 字节**里，reader 使用**存储的 k**，因此即使将来默认 bits/key 改了，
旧文件仍可正确读取；这也是 `M5-C3` 不升版本号的另一个理由。

**bitset 大小**：对某桶的 `n` 个 key，`m = max(64, n * bits_per_key)`，`bytes = (m + 7) / 8`，
`m = bytes * 8`（向上取整到整字节，再回填为位数）。空桶不调用 `CreateFilter`（`offset` 只记 0 字节）。

**哈希（Double Hashing）**：

```
uint32_t Hash(const char* data, size_t n, uint32_t seed);     // LevelDB util/hash.cc 的 32 位哈希
uint32_t BloomHash(const Slice& key) = Hash(key.data(), key.size(), 0xbc9f1d34);

// CreateFilter(key, m, k):
h = BloomHash(key);
delta = (h >> 17) | (h << 15);        // 循环右移 17 位（保证 delta 非零且与 2 的幂互质）
for (i = 0; i < k; ++i) {
  bitpos = h % m;
  bitset[bitpos / 8] |= (1u << (bitpos % 8));   // 字节内 LSB-first
  h += delta;
}

// KeyMayMatch(key, filter):
len = filter.size(); if (len < 2) return /*按 §3.2 第 7 条：len==0 ⇒ false；len==1 ⇒ true+counter*/;
m = (len - 1) * 8; k = (uint8_t)filter[len - 1];
h = BloomHash(key); delta = (h >> 17) | (h << 15);
for (i = 0; i < k; ++i) {
  bitpos = h % m;
  if ((bitset[bitpos / 8] & (1u << (bitpos % 8))) == 0) return false;
  h += delta;
}
return true;
```

- 哈希算法逐字采用 LevelDB 的 `Hash(data, n, seed)`（`src/util/hash.{h,cpp}`，M5 新增的**可选扩展**；
  `M5:120` 把它列为【可选扩展】「哈希质量」）。选择它的理由：与名字空间 `BuiltinBloomFilter2` 语义一致，
  实现小、无第三方依赖、可复现。
- **如果 `#1` 复核后决定不新增 `src/util/hash.*`**：允许把 `BloomHash` 放在 `src/bloom.cpp` 的匿名 namespace，
  并在 `bloom.h` 暴露 `bool BloomKeyMayMatch(const Slice& key, const Slice& filter)` 供 `FilterBlockReader` 使用；
  协议仍逐字写明算法。两种落点都不得改变位级结果。

**假阳性率口径**：理论上界 `fpr ≤ (1 - e^{-k·n/m})^k`；`bits_per_key=10, k=7` 时约 `0.82%`。
A 组 `M5-A02` 用 `N=100000` 个插入 key、`M=100000` 个未插入 key 实测，判据：
- `false_negatives == 0`（硬断言，`I47`）；
- `measured_fpr <= 2.0%`（约 2.4× 理论上界，给实测抖动留余量；**必须打印实测值** `FILTER_FPR_PPM`，
  作为数据表 `filter_fpr_measured` 列）。
若实测 > 2%，先查哈希/位序/bits 回填是否实现错误，**不得**改默认参数去掩盖（`M5:21`）。

### 3.4 metaindex 注册与版本兼容

- 写入：`TableBuilder::Finish` 在写完最后一个数据块后：
  1. 若 `bloom_bits > 0` 且至少有一条 entry 且未因坏 internal key 禁用 filter：
     调用 `FilterBlockBuilder::Finish()` 得到 payload；
     `WriteBlock(kBlockTypeFilter, payload, &filter_handle)`；
  2. 构造 metaindex `BlockBuilder(kIndexRestartInterval)`；
     若 `filter_handle` 存在：`meta_builder.Add(Slice(kBuiltinBloomFilterName), handle_bytes)`；
     否则写空 metaindex（与 M3 逐字一致，`table_builder.cpp:126-133`）；
  3. 写 metaindex、索引块、footer（M3 顺序）。
- 读取：`Table::ParseMetaIndexBlock`（`table.cpp:143-162`）从「所有条目都算 unknown」改为：
  - `name == kBuiltinBloomFilterName`：解 16 B handle；**最多一条**；若重复 ⇒ `filter_state=kCorrupt`；
    读 filter 块（`ReadBlockImpl(..., verify_crc=true)`），解析 payload；成功 ⇒ `filter_state=kOk`；
    失败 ⇒ `filter_state=kCorrupt`，**不返回错误**，计数 `filter_corrupt`；
  - 其它 name：仍按 M3 计数 `unknown_metaindex_entries_`（M3 的 reader 看到 M5 的 filter name 也会走这条）。
- 版本兼容：
  - M5 写的文件：`footer.version` 仍为 `1`（`format.h:45` 不变）；M3 reader 只多计一个 unknown entry，读正常；
  - M3/M4 写的文件：metaindex 无 filter entry ⇒ `filter_state = kAbsent`；`KeyMayMatch` 返回 true（照常读）；
  - 若 `Options::bloom_bits == 0`：新文件不写 filter；但**仍解析**旧文件里的 filter（见 C7(c)）；
    reader 不需要 `bloom_bits` 就能解析 filter，因为 hash 与 bits_per_key 无关，k 从 filter 尾部读。
- **禁止**：不得在 `Table::Open` 因 filter 结构/CRC 问题返回 `kCorruption`/`kNotSupported`；
  不得把 filter 的肯定结论用于跳过实际读取或校验（`I49`）。

### 3.5 接口与签名草案（`src/filter_policy.h` / `src/bloom.{h,cpp}`）

> 以下为**签名草案**，不是实现代码；任何以代码为准的签名在 M5.0 必须复核（§11 M5.0 的命令清单）。

```cpp
// src/filter_policy.h —— 策略接口（M5 新增；只依赖 common.h）
class FilterPolicy {
 public:
  virtual ~FilterPolicy() = default;
  virtual const char* Name() const = 0;                 // "leveldb.BuiltinBloomFilter2"
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// 工厂：返回按 bits_per_key 参数化的内置 Bloom 策略（进程内静态/一次性对象均可）。
// bits_per_key ∈ [1,64]；调用方保证。
const FilterPolicy* NewBuiltinBloomPolicy(int bits_per_key);

// src/bloom.h
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* policy);
  ~FilterBlockBuilder();
  // 在写一个数据块之前调用（block_offset = 该数据块起始文件偏移）。
  // 语义：target = block_offset >> kFilterBaseLg；
  //       while (target > filter_offsets_.size()) GenerateFilter();   // 逐桶补空过滤器
  void StartBlock(uint64_t block_offset);
  // 该数据块内每一条 entry 的 user key 调一次（含 tombstone）。
  void AddKey(const Slice& user_key);
  // 收尾：若非空则 GenerateFilter()，再追加 offset[] ‖ array_offset ‖ n；返回 payload。
  // 只允许调用一次；返回 Slice 指向 builder 内部 result_，调用方在写块前使用。
  Slice Finish();
  size_t NumFilters() const;      // = filter_offsets_.size()
  size_t CurrentSizeEstimate() const;

 private:
  void GenerateFilter();
  const FilterPolicy* policy_;
  std::string result_;
  std::string start_;               // 当前桶的 key 集合（length-prefixed）
  std::vector<uint32_t> filter_offsets_;
};

class FilterBlockReader {
 public:
  // 解析 filter_block_payload；解析失败 ⇒ valid()==false（调用方降级为「可能存在」）。
  // 注意：不抛异常、不返回 Status；filter 是 advisory。
  explicit FilterBlockReader(const Slice& contents);
  ~FilterBlockReader();
  bool valid() const;
  // block_offset = 将要读取的数据块起始偏移；user_key = 查询的 user key。
  // 返回 false = 否定（可跳过该数据块）；true = 可能存在或不可用。
  bool KeyMayMatch(uint64_t block_offset, const Slice& user_key) const;
  size_t NumFilters() const;
  uint64_t filter_bytes() const;    // contents.size()

 private:
  std::string contents_;            // 拷贝或持有（见实现注）
  size_t num_ = 0;
  size_t array_offset_ = 0;
  bool valid_ = false;
};

// 自由函数：供 FilterBlockReader 与测试使用（算法见 §3.3；不依赖 FilterPolicy 实例）。
bool BloomKeyMayMatch(const Slice& key, const Slice& filter);
```

**线程与所有权**：

- `FilterPolicy` 无状态（除 bits_per_key），可用 `const FilterPolicy*` 共享；`NewBuiltinBloomPolicy` 返回的对象
  由调用方按实现约定持有（建议进程内按 bits 缓存或用 `std::unique_ptr` 持有）。
- `FilterBlockBuilder` 只在 `TableBuilder` 的**单线程**构建期使用（`I50`/`L30`）。
- `FilterBlockReader` 在 `Table::Open` 的**单线程**构造期创建，之后**只读**；`Table` 经 `TableCache` 的
  `shared_ptr<const Table>` 发布，多线程读 `KeyMayMatch` 无需加锁（`I50`/`L30`）。
- `FilterBlockReader` 的 `contents_` 必须**拥有** filter payload 的生命周期（`Table` 不保证文件句柄长寿；
  M3 的 `Table::ReadBlock` 返回 `std::string` payload）。若 `contents_` 用 `std::string` 拷贝，
  在大 filter 上多一次拷贝；这是可接受的（每文件一次）。若实现改为 `shared_ptr<std::string>` 亦可，
  但不得引入悬垂 `Slice`。

### 3.6 `TableBuilder` / `Table` 的接入点

**`TableBuilder`（`src/sstable/table_builder.h` / `.cpp`）**：

- 构造函数：若 `options_.bloom_bits > 0`，创建 `filter_policy_` 与 `filter_builder_`。
- `Add(key, value)`（`table_builder.cpp:26`）：在 `block_builder_.empty()` 为真时（新数据块的第一条 entry），
  调用 `filter_builder_->StartBlock(file_size_)`；随后对每条 entry：
  `if (ParseInternalKey(key, &user_key, ...)) filter_builder_->AddKey(user_key); else filter_disabled_ = true;`
  （解析失败 ⇒ 整个文件禁用 filter，见下）。
- `FlushBlock`（`table_builder.cpp:64`）：不改；filter 的 key 收集已在 `Add` 完成。
- `Finish`（`table_builder.cpp:114`）：在写 metaindex 之前插入 filter 写块步骤（§3.4）。
- `filter_bytes()`：返回已写 filter 块的 `handle.size`（含 9 B 外壳）；用于 A 组的体积代价断言。
- **坏 internal key 的防御**：若任一 `ParseInternalKey` 失败，置 `filter_disabled_ = true`；`Finish` 不写 filter 块，
  metaindex 保持空表，并计数 `filter_skipped_bad_internal_key`。这保证「输入损坏」时不会写出**不完整的 key 集合**
  （不完整 = 后续假阴性）。不改变 M3 的既有行为（M3 的 `TableBuilder` 本来也不校验 internal key 语义）。
- `Options::bloom_bits` 已由 `DB::Open` 校验（§5.6）；`TableBuilder` 不再重复校验（可加 `assert`）。

**`Table`（`src/sstable/table.h` / `.cpp`）**：

- `Table::Open` 签名追加可选 `ReadStats* open_stats = nullptr`（默认参数保持 M3 调用兼容）：
  ```cpp
  static Status Open(const TableOptions& options, Env* env, const std::string& filename,
                     std::shared_ptr<Table>* table,
                     const std::string* known_smallest = nullptr,
                     const std::string* known_largest = nullptr,
                     ReadStats* open_stats = nullptr);
  ```
  在 `ParseMetaIndexBlock` 读到已知 filter name 时，读 filter 块（私有 `ReadBlockImpl(..., verify_crc=true)`），
  构造 `FilterBlockReader`；成功/失败都**不返回错误**；`open_stats->filter_blocks_read++`、
  `open_stats->filter_bytes_read += payload.size()`（若 `open_stats != nullptr`）。
- 新增公开成员：
  ```cpp
  enum class FilterState { kAbsent, kOk, kCorrupt };
  FilterState filter_state() const;
  bool has_filter() const;                 // == (filter_state() == kOk)
  uint64_t filter_bytes() const;           // 0 if absent
  // 仅用于读路径「决定是否读数据块」：返回 false ⇒ 否定（可跳过）；true ⇒ 可能存在/不可用。
  bool KeyMayMatch(uint64_t block_offset, const Slice& user_key, ReadStats* stats) const;
  ```
- `ReadStats` 追加（**只追加，不改既有列语义**）：
  ```cpp
  uint64_t filter_checked = 0;         // KeyMayMatch 被调用的次数
  uint64_t filter_negative = 0;        // 返回 false 的次数
  uint64_t filter_positive = 0;        // 返回 true 的次数
  uint64_t filter_unavailable = 0;     // 本文件没有可用 filter（kAbsent/kCorrupt）时按 true 处理的次数
  uint64_t filter_blocks_read = 0;     // 读 filter 块的次数（只在 Table::Open 成功读到 filter 时 +1）
  uint64_t filter_bytes_read = 0;      // filter payload 字节数
  uint64_t data_blocks_skipped_by_filter = 0;  // 因 filter 否定而省掉的数据块读次数
  ```
- `DbReadStats`（`db_impl.h:98-113`，稳定 rev）追加同名/同义字段；`MergeReadStats`（`db_impl.cpp:533` 附近的
  稳定 rev 实现）把它们累加进 `read_stats_`。**注意**：`DbReadStats.files_checked` 的既有递增点不变
  （`db_impl.cpp:500` 稳定 rev：只对真的进了 `TableCache::Get` 的文件 +1）；被 filter 否定的文件仍被计入
  `files_checked`（因为已经进了 `GetEntry`），但 `data_blocks_read` 不增加。这正是 `M5:182` 要的
  「读放大是否计 filter 跳过的文件」的口径：**filter 跳过的是块，不是文件**；「考虑 filter」与「不考虑 filter」
  两列的差异体现在 `data_blocks_read` 与 `blocks_read`，不在 `files_checked`。

### 3.7 读路径接入与降级规则（唯一允许的否定判断点）

`Table::GetEntry`（`table.cpp:259`）的现有步骤是：
① key range 过滤 → ② 内存索引 `lower_bound` → ③ 读一个数据块 → ④ 块内 Seek → ⑤ user key 相等校验 →
⑥ tombstone/value/not-found 三态。

M5 在 **② 与 ③ 之间**插入 step ②.5：

```
② lower_bound 得到 it（将要读的数据块）
   if (it == end) return kNotFound;             // 原有行为：索引越界，无块可读
②.5 if (filter 可用) {
       bool maybe = filter_->KeyMayMatch(it->handle.offset, lookup_user);
       if (stats) {
         ++stats->filter_checked;
         if (!maybe) { ++stats->filter_negative; ++stats->data_blocks_skipped_by_filter; }
         else        { ++stats->filter_positive; }
       }
       if (!maybe) {
         *result = TableGetResult::kNotFound;     // 只允许的「否定」用途：告诉上层继续查更旧文件
         return Status::OK();
       }
     } else if (stats) {
       ++stats->filter_unavailable;
     }
③ 读数据块（原有行为，含 CRC/长度/type 校验）
```

**硬规则**：

1. filter 为 false ⇒ 只能返回 `kNotFound`（本文件没有该 user key），**禁止**返回 `kFound`/`kDeleted`，
   **禁止**影响 `result` 的其它取值。上层（`GetInternal`）会因此继续查更旧文件；若所有文件都返回 `kNotFound`，
   最终 `Get` 返回 `kNotFound`，与「不存在」语义一致。
2. filter 为 true ⇒ **必须**执行原有 step ③~⑥（读块、CRC、Seek、user key 校验、tombstone 判定）。
   任何「true 就直接返回存在/跳过校验」的实现是阻断性缺陷（`M5:34`、`I49`）。
3. filter 不可用（`kAbsent`/`kCorrupt`/索引越界/`len==1`）⇒ 一律按「可能存在」处理，执行 step ③~⑥；
   计数 `filter_unavailable`（每文件每次查询 +1），**不得**报错拒绝读（`M5:35`）。
4. **迭代器路径（`Table::NewIterator`）绝不使用 filter**：全量迭代必须读所有数据块。
5. **CRC 始终校验**：filter 块的读取走 `verify_crc=true`（E3）；数据块的 CRC 仍由 `Options::verify_checksums` 控制。
6. **降级不静默**：filter 不可用/损坏必须在 `ReadStats`/`DbReadStats` 计数，并由 `AMPL` 行/诊断工具上报；
   生产路径可以只计数不打印，但测试与证据必须能看到 `filter_unavailable`/`filter_corrupt`。

**`filter_state == kCorrupt` 的产生条件**（每文件一次，在 `Table::Open` 决定）：
- metaindex 有条目但 handle 越界/不满足 `filter.offset+size <= metaindex.offset`；
- filter 块 `ReadBlockImpl` 返回任何错误（含 CRC 不符、type 不符、length 自检不符）；
- payload 不满足 §3.2 的 8 条解析校验；
- 同一 name 出现多次。

**`filter_state == kAbsent` 的产生条件**：metaindex 无该 name；或 `Options::bloom_bits==0` 的新文件（本来就没写）。
两者在读路径行为上等价（都按可能存在处理），但计数上分开（`kAbsent` 不等于损坏）。

### 3.8 计数器口径与 `≥3×` 门禁

**计数来源**：`PersistentDBImpl::GetReadStats()`（`db_impl.h:255` 稳定 rev）返回累计 `DbReadStats`；
在一次 `Get` 前后取差，得到本次查询的增量。`Table::ReadStats` 的增量由 `GetInternal` 汇总。

**同轮开关对照的实现**：`M5-A10` 在**同一个测试进程**里建两个 `PersistentDBImpl`：
- DB-A：`Options.bloom_bits = 0`（不写 filter）；
- DB-B：`Options.bloom_bits = 10`（写 filter）。
两者用**同一数据集**（相同 key 集合与写入顺序）、**同一查询集合**（`Q` 个确定不存在的 key，
`M=100000`），查询顺序也在两 DB 间相同。分别取 `GetReadStats().data_blocks_read` 增量：

```
without = DB-A 的 data_blocks_read 增量
with    = DB-B 的 data_blocks_read 增量
ratio   = without / max(1, with)
gate    = (without >= 3) && (ratio >= 3.0)      // M5-C5 的 3.0 阈值
```

**必须打印**（供门禁 `grep` 正向标记）：

```
M5_FILTER_BLOCK_READS_WITHOUT <n>
M5_FILTER_BLOCK_READS_WITH <n>
M5_FILTER_BLOCK_READ_RATIO <f>
M5_FILTER_FALSE_NEGATIVE 0
M5_FILTER_FPR_PPM <n>
[FILTER_OK]
```

**作用域限制（写死在测试注释与 `docs/m5-bench.md`）**：
- 只对「该数据集、该点查模式、`Get` 不存在的 key」成立；
- 禁止外推为「Bloom 让读快 N 倍」；`ratio` 是**数据块读次数**的比值，不是墙钟延迟比值；
- 如果 `M` 太小或数据集太小导致 `without < 3`，该测试必须**失败**并提示放大数据集，**不得**放行。

---

## 4. `docs/protocol.md` 追加 §12/§13 的 patch 文本（**本阶段不落地**）

> 纪律：**只追加，不改 §1~§11 任何一行**。落地时机是 M5.1（§12）与 M5.2（§13）。
> 若 M4.3 又追加了协议章节，M5.0 必须重新顺延编号。

### 4.1 追加 §12：filter block 编码（M5 定稿）

```markdown
## 12. filter block 编码（M5 定稿）

### 12.1 常量与名字空间

| 常量 | 值 | 说明 |
|---|---|---|
| `kBuiltinBloomFilterName` | `"filter.leveldb.BuiltinBloomFilter2"` | metaindex 的 `name`；footer 不变、`kTableFormatVersion` 不升 |
| `kFilterBaseLg` | `11` | filter 覆盖粒度 = 2 KiB（`1 << 11`） |
| `kBlockTypeFilter` | `0x04` | §10.2 已预留的块类型；M5 开始产出 |
| `kBloomMinBits` | `64` | 单个 filter 的最小位数（向上取整到整字节） |
| `kBloomMaxK` | `30` | 单个 filter 的 k 上界 |

- filter **不是**独立文件，也不是新文件类型：它是 SSTable 内的一个块，块外壳与 §10.3 完全相同。
- 每个 SSTable **至多一个** filter 块；`name` 在 metaindex 里至多出现一次。
- 读方必须容忍未知 `name`（§10.6）；本节的 `name` 对 M3 的 reader 就是「未知」，只计数、不报错。

### 12.2 filter block 的 payload

```
filter_block_payload := filter[0] ‖ filter[1] ‖ … ‖ filter[n-1]
                        ‖ offset[0] ‖ … ‖ offset[n-1]      # uint32 LE × n
                        ‖ array_offset                       # uint32 LE
                        ‖ n                                  # uint32 LE

filter[i]             := bitset_i ‖ k_i(1B)
```

| 字段 | 编码 | 约束 |
|---|---|---|
| `filter[i]` | bitset 字节 + 1 B `k` | bitset 长度 = `ceil(m_i / 8)`；`m_i` 是 8 的倍数；`m_i = max(64, n_i * bits_per_key)`（`n_i` = 该桶 key 数） |
| `offset[i]` | 4 B LE | `filter[i]` 相对 payload 起点的绝对偏移；`offset[0] == 0`；非递减 |
| `array_offset` | 4 B LE | `offset[0]` 的偏移；`array_offset + 4*n + 8 == payload.size()` |
| `n` | 4 B LE | filter 个数；`n == 0` 时 `payload` 恰 8 B（`array_offset=0`、`n=0`） |

- **空 filter**：`filter[i]` 长度可以为 0（`offset[i] == offset[i+1]`），表示该 2 KiB 桶内没有 key；
  读方对长度 0 返回 `false`（该桶无数据块，安全）。
- **长度 1 的 filter**：只有 `k` 没有 bitset；读方必须视为不可用（按「可能存在」处理并计数），
  **不得**按「不存在」返回。理由：这属于结构异常，不能冒假阴性风险。
- bitset 的位序：`bitset[bitpos / 8] |= (1u << (bitpos % 8))`（字节内 LSB-first）。
- `k` 与 `bits` 一样是 **每个 filter 自己的**，不是全局的；读方必须使用存储的 `k`。

### 12.3 哈希与 Double Hashing

```
uint32_t Hash(const char* data, size_t n, uint32_t seed);   // LevelDB util/hash.cc 的 32 位哈希
BloomHash(key) = Hash(key.data(), key.size(), 0xbc9f1d34)

CreateFilter(keys, n, dst):
  bits = max(64, n * bits_per_key); bytes = (bits + 7) / 8; bits = bytes * 8;
  k = max(1, min(30, round(bits_per_key * 0.693147)));
  append bytes 个 0；append k 字节；
  for each key:
    h = BloomHash(key); delta = (h >> 17) | (h << 15);
    for i in [0, k): bitpos = h % bits; set bit; h += delta;

KeyMayMatch(key, filter):
  len = filter.size(); if (len < 2) return (len == 0 ? false : true);   # 见 12.2 的「长度 1」
  bits = (len - 1) * 8; k = (uint8_t)filter[len - 1];
  h = BloomHash(key); delta = (h >> 17) | (h << 15);
  for i in [0, k): bitpos = h % bits; if bit not set return false; h += delta;
  return true;
```

- Double Hashing 的正确性依赖 `delta ≠ 0` 且 `delta` 与 `2` 的幂互质；`(h >> 17) | (h << 15)` 是
  `h` 的循环右移 17 位，若结果为 0 则 `h` 本身为 0（此时 `BloomHash` 对空 key 也给非 0 种子结果），
  实现里应对 `delta == 0` 做一次保护（如 `delta = 0x9e3779b9`）。
- `bits_per_key` 的默认值由 `Options::bloom_bits` 决定（默认 10）；协议只规定编码，不规定默认值。

### 12.4 filter 与数据块的对应

- 写方在写一个数据块之前调用 `StartBlock(block_offset)`，其中 `block_offset` = 该数据块的文件起始偏移；
  该数据块内**每一条 entry**（含 tombstone）的 **user key** 加入当前桶。
- 读方对将要读取的数据块（由索引 `lower_bound` 选中）计算
  `index = block_offset >> kFilterBaseLg`，用 `filter[index]` 做否定判断。
- 对应关系必须满足：`index < n`；否则读方必须按「可能存在」处理（不得按不存在）。
- **错位等同于假阴性**：任何 offset/桶对应不自洽的 filter block 必须被读方判定为不可用，
  按「可能存在」处理并计数；不得用它做否定判断。

### 12.5 版本兼容

- M5 写 filter 时 **不升** `kTableFormatVersion`、**不改** footer：`version` 仍为 `1`。
- 旧文件（无 `filter.leveldb.BuiltinBloomFilter2` 条目）⇒ 读方按「无 filter」处理，照常读。
- 未来 filter 策略演进（例如 `BuiltinBloomFilter3`）通过 **metaindex 的新 name** 表达；
  读方对不认识的 name 只计数、不使用；旧 reader 对新 name 也只会计数（§10.6）。
- filter 块自身损坏（CRC/结构/错位）⇒ 读方**降级**为「无 filter」，**不得**让 SSTable 打开失败。
```

### 4.2 追加 §13：WriteBatch 编码（M5 定稿）

```markdown
## 13. WriteBatch 编码（M5 定稿）

### 13.1 batch payload

```
batch_payload := sequence(8B, LE) ‖ count(4B, LE) ‖ entry[0..count)
entry         := type(1B) ‖ key_len(varint32) ‖ key ‖ [ value_len(varint32) ‖ value ]
```

| 字段 | 编码 | 约束 |
|---|---|---|
| `sequence` | 8 B LE | 本 batch 第一条 entry 的 sequence；`sequence + count - 1 <= kMaxSequenceNumber` |
| `count` | 4 B LE | `1 .. kMaxBatchCount`（`kMaxBatchCount = 1 << 20`）；`count == 0` 非法 |
| `type` | 1 B | `0x0 = kTypeDeletion`（无 value 字段）、`0x1 = kTypeValue` |
| `key_len` / `key` | varint32 + 字节（§4） | `1 .. kMaxUserKeySize` |
| `value_len` / `value` | varint32 + 字节（§4） | 仅 `kTypeValue`；空 value 合法 |

- 第 `i` 条 entry 的 sequence = `sequence + i`。
- 解析必须**恰好消费完** payload：`count` 条 entry 解完后仍有剩余字节 ⇒ 损坏。
- 本编码与 §9.4 的 WAL batch payload **逐字相同**；M5 的 `WriteBatch` 提供公共 API，但**不改编码**。

### 13.2 一个 WriteBatch = 一条 WAL record

- `DB::Write(const WriteOptions&, WriteBatch*)` 的**一个调用**所携带的全部 entry 必须落在**同一条** WAL
  逻辑 record 的 payload 内；禁止把一个 `WriteBatch` 拆到两条 record。
- M2 的组提交可以把**多个**并发写者（含多个 `WriteBatch`）合并到同一条 record：此时 payload 仍是一个
  `batch_payload`，`count` = 组内所有 entry 总数，`sequence` = 组内第一条 entry 的 sequence；
  每个原 `WriteBatch` 的 entry 在 payload 内保持**连续且顺序不变**。
- 批内 sequence 连续：同一 `WriteBatch` 的第 `i` 条 entry 的 sequence = `batch 起始 sequence + i`。
- 崩溃恢复按 §9.4 的解析器逐条重放；一条 record 的完整性由 M2 的物理层保证（一个 record = 一个 CRC = 一个原子单位）。
- `sync = true` 的 durable-before-ack 按**组**生效：返回任一写者前完成 fsync；批粒度继承 M2 的 I11。

### 13.3 批大小上限与超限处理

- `WriteBatch::kMaxCount = 1 << 20`（= §9.4 的 `kMaxBatchCount`）。
- `WriteBatch::kMaxBytes = kMaxLogicalRecordSize`（= 64 MiB，`src/wal.h:23`）；
  `DB::Write` 在**编码/入队之前**校验 `ByteSize() + 16 <= kMaxLogicalRecordSize`（与单条写的既有校验同形，
  `src/db_impl.cpp:160` 稳定 rev）。
- 超限 ⇒ `DB::Write` 返回 `kInvalidArgument`，**不得**做任何 WAL/内存写入；
  `count == 0` 同样返回 `kInvalidArgument`。
- 上限是**协议层防御**；benchmark 与推荐用法应使用远小于上限的批（默认 `batch=1`，可配）。
```

---

## 5. WriteBatch 接口、提交路径与原子性

### 5.1 `src/write_batch.h` 接口草案

> 只依赖 `common.h` / `util/coding.h`；不依赖 `db_impl` / `wal` / `memtable`。

```cpp
class WriteBatch {
 public:
  static constexpr size_t kHeaderSize = 12;          // sequence(8) + count(4)
  static constexpr uint32_t kMaxCount = 1u << 20;    // protocol §9.4 / §13.3
  static constexpr size_t kMaxBytes = 64u * 1024 * 1024;  // kMaxLogicalRecordSize

  WriteBatch();
  ~WriteBatch();
  WriteBatch(const WriteBatch&) = delete;
  WriteBatch& operator=(const WriteBatch&) = delete;

  // 追加一条；不返回 Status（LevelDB 同形）。超限由 DB::Write 在提交前拒绝（§13.3）。
  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);
  void Clear();                                      // rep_ = 12 个 0 字节

  size_t Count() const;                              // batch 内的 entry 条数
  size_t ByteSize() const;                           // == rep_.size()，含 12B 头
  SequenceNumber Sequence() const;                   // 未提交时为 0
  void SetSequence(SequenceNumber seq);              // 仅供 DB::Write/测试；调用方不得依赖
  Slice Data() const;                                // 完整 batch 编码（含 12B 头）

  // 解析并逐条回调；成功返回 kOk；畸形返回 kCorruption；失败时 handler 可能已被部分调用，
  // 调用方（DB::Write）必须先用 Validate 之类的方式做「无副作用预校验」。
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

 private:
  std::string rep_;    // header(12) + entries；与 §13.1 逐字同构
};
```

**所有权 / 线程约束（写入头文件注释，满足 `L31`）**：

- `WriteBatch` 由**调用方拥有**；`DB` 不持有它的裸指针超过 `DB::Write` 的调用期。
- 同一个 `WriteBatch` **不得**在 `DB::Write` 进行期间被另一个线程修改或销毁；`WriteBatch` 内部**没有锁**。
- `DB::Write` 返回后，调用方可以复用（继续 `Put`）或 `Clear()`；`DB::Write` 不会保留对它的引用。
- `DB::Write` 会忽略调用方先前 `SetSequence` 的值，并在组提交里重新分配 sequence；
  `Sequence()` 在 `DB::Write` 返回后可反映本次批的起始 sequence（诊断用，不作为契约）。

### 5.2 `DB` 与实现的接口扩展

```cpp
// src/db.h
class DB {
  ...
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;
  ...
};
```

- `MemoryDBImpl`（`src/db.cpp:18-82`）与 `PersistentDBImpl`（`src/db_impl.h:115` 稳定 rev）都必须 override；
  否则 `DB` 的纯虚接口会让编译失败（这是有意的：不允许只改持久模式）。
- `PersistentDBImpl` 现有的私有 `Status Write(ValueType, const WriteOptions&, const Slice&, const Slice&)`
  （`db_impl.h:253` 稳定 rev）**改名** `WriteEntry(...)`；`Put`/`Delete` 调 `WriteEntry`。
  理由：`Write(const WriteOptions&, WriteBatch*)` 与 `Write(ValueType, const WriteOptions&, ...)` 同名时，
  `DB::Write(WriteOptions(), nullptr)` 之类的调用会产生重载歧义（`M5-R7`）。

### 5.3 持久模式的提交路径（对 M2 组提交的最小扩展）

**`Pending` 结构扩展**（`db_impl.h:349` 稳定 rev；M4.3 在途若改了它，以落地为准）：

```cpp
struct Pending {
  bool need_sync = false;
  bool done = false;
  Status status;
  SequenceNumber begin = 0;     // 本成员第一条 entry 的 sequence（组提交分配）
  uint32_t entry_count = 1;     // 单条写 = 1；WriteBatch = batch->Count()
  std::string entries;          // entry 编码拼接（不含 12B batch 头）
  size_t entry_bytes = 0;       // entries 的编码字节 + 每条 1B 的既有口径；见下
  uint64_t user_bytes = 0;      // sum(key.size + value.size)，用于放大统计
};
```

- 单条 `Put`/`Delete`：`entry_count=1`，`entries` = 一条 entry 的编码（`type ‖ klen ‖ key ‖ [vlen ‖ value]`），
  `entry_bytes` = `1 + VarintLength(key) + key + [VarintLength(value) + value]`（与现有 `Write` 的口径一致）。
- `WriteBatch`：`entry_count = updates->Count()`；`entries` = `updates->Data()` 去掉前 12 B；
  `entry_bytes` = 各 entry 的 `1 + VarintLength(key) + key + [VarintLength(value) + value]` 之和；
  `user_bytes` = Σ(key.size + value.size)。

**`EncodeGroup` 扩展**（`db_impl.cpp:219` 稳定 rev）：

```
total_count = Σ p->entry_count
out := PutFixed64(begin) ‖ PutFixed32(total_count) ‖ (for each member: p->entries)
```

**`RunFlusher` 的 sequence 分配**（`db_impl.cpp:248` 稳定 rev）：

```
begin = last_sequence_ + 1;
offset = 0;
for (p : members) { p->begin = begin + offset; offset += p->entry_count; }
total_count = offset;
if (begin + total_count - 1 > kMaxSequenceNumber) { 整批拒绝（kInvalidArgument），不写 WAL }
last_sequence_ = begin + total_count - 1;
smallest_snapshot_ = snapshots_.empty() ? last_sequence_ : *snapshots_.begin();
```

**内存提交**（同一 `DbMutexGuard` 内）：

```
footprint = Σ (p->entry_bytes + p->entry_count * kMemTableNodeOverhead);
if (memtable_->WouldReject(footprint)) { 冻结当前表（若非空）→ 建新表，容量 >= footprint + kMemTableNodeOverhead; }
for (p : members)
  for (j = 0; j < p->entry_count; ++j)
    memtable_->Add(p->begin + j, entry[j].type, entry[j].key, entry[j].value);
```

- `entry[j]` 从 `p->entries` 解析；解析在**取批时**或**提交时**完成，但必须在任何 `Add` 之前完成并校验。
- `put_ops_ += total_count`；`user_logical_bytes_ += Σ p->user_bytes`；`entry_bytes_ += Σ p->user_bytes + 16*total_count`
  （M4.3 在途的统计口径若不同，以落地为准，M5.0 复核）。
- `front_samples_us_`：一次 `DB::Write` 调用一个样本（批的端到端延迟）；`FRONT` 行的 `ops` 为 entry 数，
  故数据表必须同时打印 `batch` 参数，避免把「每批延迟」误读成「每条延迟」。

**失败传播**：`WAL Append`/`Sync` 失败 ⇒ 整组 `p->status = s`、`done = true`、`commit_cv_.notify_all()`，
**不执行**任何 `MemTable::Add`（现有 `RunFlusher` 已是这个顺序；`db_impl.cpp:381-391` 稳定 rev）。
`bg_error_` 粘性语义不变。

### 5.4 整批原子性的证明（`I51`）

**命题**：在 `M5-A12` 的提交路径下，一个 `WriteBatch` 的内存条目要么全部可见，要么全部不可见。

**证明**：

1. **输入预校验**：`DB::Write` 在入队前解析 `updates->Data()`，逐条校验
   `type ∈ {kTypeValue, kTypeDeletion}`、`1 <= key.size() <= kMaxUserKeySize`、`value` 长度合法、
   `count ∈ [1, kMaxCount]`、`ByteSize()+16 <= kMaxLogicalRecordSize`；任何失败返回错误，**不写 WAL、不碰内存**。
2. **容量预检**：`RunFlusher` 用 `footprint = Σ (entry_bytes + entry_count * kMemTableNodeOverhead)` 调
   `memtable_->WouldReject(footprint)`。若为 false，则当前 `ApproximateMemoryUsage() + footprint < write_buffer_size_`。
3. **逐条 `Add` 不可能触顶**：`MemTable::Add` 的写前判是 `WouldReject(0)`，即
   `ApproximateMemoryUsage() + 0 >= write_buffer_size_`。由于每次 `Add` 后
   `ApproximateMemoryUsage()` 的增量 ≤ `entry_bytes + 128`（见下），在全部 `Add` 完成前的任意时刻，
   累计用量 ≤ 初始用量 + footprint < `write_buffer_size_` ⇒ 每条 `Add` 的 `WouldReject(0)` 均为 false。
4. **增量上界**：`MemTable::Add` 的 Arena 交出的字节 = `encoded_len`（`memtable.cpp:179` 的 `arena_.Allocate(encoded_len)`）
   + `Skiplist::Insert` 的 `arena_->AllocateAligned(sizeof(Node))`（`skiplist.h:192`）。
   `encoded_len = VarintLength(key.size()+8) + key.size()+8 + VarintLength(value.size()) + value.size()`；
   本设计的 `entry_bytes = 1 + VarintLength(key.size()) + key.size() + [VarintLength(value.size())+value.size()]`。
   两者之差 = `VarintLength(key+8) - VarintLength(key) + 7 ≤ 8`。`sizeof(Node) = 16(Slice) + 4(int) + padding + 12*8`
   = 120（x86-64），`kMemTableNodeOverhead = 128` ⇒ `entry_bytes + 128 ≥ encoded_len + sizeof(Node)`。
   因此 `footprint` 是 `Arena::BytesAllocated()` 增量的**上界**。
5. **WAL 与内存的顺序**：`RunFlusher` 先 `Append`（可选 `Sync`），成功后才做 `Add`。
   若 WAL 失败，`Add` 一次都不执行 ⇒ 无半批。若 WAL 成功，则第 2~4 步保证所有 `Add` 成功 ⇒ 全批可见。
6. **恢复路径**：`RecoverAndOpen` 在**尚未发布**的 `PersistentDBImpl` 对象上重放；`ParseBatch` 对整条 record
   做「恰好消费完」校验，尾部截断的 record 根本不会被 `WALReader` 发射出来；若完整 record 的重放中
   `MemTable::Add` 失败，`Open` 返回 `kCorruption`，该对象被销毁 ⇒ 不存在「半批被外部看到」。
   `memtable.h:57` 的 `Freeze` 与 `kFrozen` 只影响写路径；恢复的容量公式（`db_impl.cpp:1264-1265` 稳定 rev）
   已按「编码字节 ×2 + 每条 ×2×128 + 1 MiB slack」放大，且重放路径在 Add 失败时直接失败关闭。

**结论**：`I51` 成立，且不依赖 MemTable 的 undo 接口，也不修改 M1 的 MemTable 结构。
**唯一残留风险**：`kMemTableNodeOverhead=128` 的数值前提是 x86-64 且 `alignof(max_align_t)=16`；
`M5-A13` 必须实测「`ApproximateMemoryUsage` 增量 ≤ Σ(entry_bytes+128)」并在失败时让测试失败；
若将来平台变化，调大该常量即可（不落盘、不影响格式）。

### 5.5 与 M2 组提交的协作细节

| 交互点 | M2 现状（稳定 rev） | M5 的扩展 |
|---|---|---|
| 锁序 | `commit_mu_ → mutex_`（`db_impl.h:391-395`、`db_impl.cpp:166-171`） | **不变**；批只让 `Pending` 更大 |
| 组批上限 | `kMaxGroupBytes=1 MiB`、`kMaxGroupRecs=64`（`db_impl.cpp:22-23`） | **不变**；单个大 `WriteBatch` 可独占一批（现有循环对首个成员不检查字节上限，`db_impl.cpp:273-283`） |
| `sync` | 组内 OR（`db_impl.cpp:281`） | **不变** |
| durable 水位 | 只有真 fsync 才推进 `durable_seq_`（`db_impl.cpp:371-378`） | **不变**；批的 `Write` 返回 ⟹ `durable_seq_ >= p->begin + p->entry_count - 1` |
| 丢唤醒 | `flusher_active_` + `commit_cv_` + `while (!w.done)`（`db_impl.cpp:183-214`） | **不变**；`M5-A14` 复用 M2 的无丢唤醒用例形状 |
| 失败传播 | 整批同一 `Status`（`db_impl.cpp:392-399`） | **不变**；批内所有 entry 同一个 `Status` |
| `front` 统计 | M4.3 在途的一次 `batch_t0` 采样 | M5 改为「一次 `Write` 调用一个样本」；`put_ops_` 改为 entry 数；M5.0 复核 M4.3 落地 |

### 5.6 `Options::bloom_bits` 的校验与打印

- `Options` 追加 `int bloom_bits = 10;`（`common.h:285` 稳定 rev 的 `struct Options` 内，放在 M4 字段之后）。
- `DB::Open` 第一步校验（与 `block_size`/`max_open_files` 同处，`db.cpp:95-104`）：
  `bloom_bits < 0 || bloom_bits > 64` ⇒ `kInvalidArgument`；`0` 合法（关闭）。
- **默认开**：`bloom_bits=10`；基准脚本的 filter-off 对照必须显式 `--bloom-bits 0` 并把
  `bloom_bits=0` 打印到结果文件头（`M5:21`/`M5:36`）。
- `TableBuilder`/`Table` 只读 `options_.bloom_bits` 决定**是否写** filter；reader 是否**读**旧 filter 与它无关（C7(c)）。
- 不在 `Options` 放 `const FilterPolicy*`：保持 `common.h` 零项目依赖（`roadmap.md:22-26`）。

---

## 6. 微基准与数据表

### 6.1 四类负载定义（`M5:12`）

| 负载 | 操作序列 | key 分布 | value 大小 | LSM 写入方式 | 对照 |
|---|---|---|---|---|---|
| `seq_write` | N 次 `Put` | 递增 8B BE（`KeyFromIndex` 风格） | `value_size` | `DB::Put`；`sync` 由参数控制 | 裸文件 `write+fsync` 追加；`std::map` |
| `rand_write` | N 次 `Put` | 均匀随机（固定种子） | `value_size` | `DB::Put` | 裸文件 `pwrite` 随机偏移 + `fdatasync`（按参数）；`std::map` |
| `rand_read` | M 次 `Get` | 已写入 key 的随机 shuffle 顺序 | — | `DB::Get` | 裸文件 `pread` 随机偏移；`std::map` |
| `seq_read` | M 次 `Get` | 递增顺序 | — | `DB::Get` | 裸文件顺序读；`std::map` |

- 每个 LSM 负载在计时窗口外先 `warmup` 次操作；窗口内只做被测操作。
- `sync=1` 时 LSM 走 `WriteOptions.sync=true`；`sync=0` 时 `sync=false`。**两次口径都跑**，
  数据表分开列，且明确写「`sync=false` 的 `missing 0` 只证明进程级一致性，不证明掉电安全」（M2 教训）。
- `rand_read`/`seq_read` 的 LSM 数据集必须在计时前完成写入并（可选）`Sync()`，保证读的是 SSTable/WAL 组合；
  `filter` 开关影响 `rand_read` 的块读计数与吞吐。

### 6.2 对照对象的公平性

- **裸文件 `write+fsync`**：`open(O_CREAT|O_WRONLY|O_APPEND)`，每次 `write(value_size + key_size)` + `fsync`；
  与 LSM 的 `sync=true` 对齐；不打印 `missing`（`verify=na`）。
- **裸文件 `pwrite` 随机**：`open(O_CREAT|O_RDWR)` + 预分配（`posix_fallocate`）+ 随机偏移 `pwrite` + `fdatasync`
  （与 raft-kv `fsbench_commit_latency.cpp` 的 prealloc 档同形，见本机只读核对）。
- **纯内存 `std::map`**：`std::map<std::string,std::string>` 的 `insert`/`find`；只测 CPU/内存路径，
  不涉及 IO；作为「LSM 的 CPU 与内存开销」参照。
- **公平性声明**：三类对照的持久性语义**不同**（裸文件档是纯 IO；`std::map` 完全不持久），
  数据表必须逐行写 `durability=<fsync|fdatasync|none>`；禁止把 `std::map` 的吞吐说成「LSM 应该达到的目标」。

### 6.3 参数矩阵与固定输出行

**参数（`bench_lsm.sh` 透传给 `bench_lsm`）**：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--dataset N` | `100000` | 写入 key 数 |
| `--value-size B` | `100` | value 字节数 |
| `--key-dist seq|uniform` | `seq` | key 分布 |
| `--batch K` | `1` | 每个 `DB::Write` 的 entry 数（`1` = 单条 `Put`） |
| `--pipeline P` | `1` | 并发写/读线程数（`1` = 单线程） |
| `--sync 0|1` | `0` | `WriteOptions.sync` |
| `--repeats R` | `3` | 每格重复次数，取中位数 |
| `--warmup W` | `10000` | 预热操作数 |
| `--filter on|off` | `on` | `on` ⇒ `bloom_bits=10`；`off` ⇒ `bloom_bits=0`（显式打印） |
| `--engines all|lsm|raw|map` | `all` | 对照选择 |
| `--seed S` | `0x5EED2025` | 随机源种子 |
| `--out FILE` | `/tmp/lsm_bench_<ts>.txt` | 原始结果文件 |
| `--inject-missing` | 关 | 失败注入自测（只对 LSM 格） |
| `--quick` | 关 | 缩小规模（用于 smoke；不得用于结论） |

**固定行格式（`M5:14` 的 `CELL / THROUGHPUT / LATENCY / P99`）**：

```
CELL name=<seq_write|rand_write|rand_read|seq_read> engine=<lsm|raw_file|raw_pwrite|std_map>
     dataset=<N> value_size=<B> key_dist=<seq|uniform> batch=<K> pipeline=<P> sync=<0|1>
     filter=<0|1> repeat=<i> THROUGHPUT=<ops_per_sec> LATENCY=<median_us> P99=<p99_us>
     p999_us=<us> min_us=<us> max_us=<us> verify=<lsm|na> missing=<n|na> durability=<fsync|fdatasync|none>
```

- `THROUGHPUT` 的单位：`ops/s`，`op` = 一次用户操作（`batch=K` 时 = 一个 entry）。
- `LATENCY` 是 `ops` 的中位延迟；`P99` 是 P99 延迟；两者单位 `us`。
- 行内**固定出现** `CELL`、`THROUGHPUT`、`LATENCY`、`P99` 四个 token（便于 `grep`/`awk`）。
- 不能跑的格子也**必须**打印：`THROUGHPUT=NA LATENCY=NA P99=NA notes=NOT_APPLICABLE reason=...`。
  禁止省略行（`M5:68` 的「不选择性报告」）。

**结果文件头部（`M5:167`）**：

```
MACHINE nproc=<n> load1=<f> load5=<f> load15=<f> fs=<ext4|...> mount=<src> kernel=<uname -r>
PARAMS dataset=<N> value_size=<B> key_dist=<...> batch=<K> pipeline=<P> sync=<0|1> repeats=<R>
       warmup=<W> filter=<0|1> bloom_bits=<0|10> seed=<hex> out=<path>
BUILD rev=<git rev-parse HEAD> dirty=<0|1> build_type=<Release|...> cxx=<g++ version>
FSYNC_BASELINE median_ms=<未验证，需 M5 #1 实测> source=<scripts/fsbench_commit_latency.cpp>
```

- `load1 > nproc` 时该文件的所有 `CELL` 行追加 `notes=UNRELIABLE(load)`；`lsm_gate.sh` 可把
  `UNRELIABLE` 计入失败或警告（M5.3 定，建议**失败**，避免把噪声当结论）。

### 6.4 预热、重复、中位数与机器状态

- 每格：`warmup` 次操作（丢弃）→ `R` 次测量（每轮重新注入同一操作序列）→ 取 `R` 次的中位数。
- `R=3` 时同时打印三次原始值（`raw1_us/raw2_us/raw3_us`），供复现性核对。
- 机器状态行在**运行前**打印一次，并在每格前重读 `loadavg`；`load1 > nproc` 标 `UNRELIABLE`。
- 关闭 CPU 频率抖动不可控；本设计不要求 `cpupower`/`taskset`，但要求把**不可控事实**写进 `docs/m5-bench.md`。
- `--quick` 只用于 smoke，输出必须带 `notes=QUICK`，不得进入结论表。

### 6.5 同轮交替 A/B 与不可外推声明

- **同轮交替**：`bench_lsm` 在一次进程运行内，按 `engine` 与 `filter` 维度交替执行（例如
  `lsm/filter=0 → lsm/filter=1 → raw_file → std_map → …`），而不是「先把 LSM 全跑完再跑对照」；
  这样机器状态漂移对两边的期望影响相近。
- **filter on/off 的块读对照**：不靠墙钟，靠 §3.8 的计数（不受负载影响）；墙钟吞吐也打印，但只作参考。
- **不可外推（写死在 `docs/m5-bench.md` 与脚本头）**：
  1. 单机、单块 ext4、VM、`loopback` 级场景；
  2. `sync=false` 的数字只说明「进程级一致性」；
  3. 数据集 `N`、`value_size`、`batch`、`pipeline` 具体值只在打印的参数下成立；
  4. 绝对吞吐不承诺达到任何外部系统的数字；
  5. `≥3×` 只针对「不存在 key 的数据块读次数」。

### 6.6 数据表列定义（含 filter 列与三个放大）

数据表（`docs/m5-bench.md`，M5.3 产出）固定列：

| 列 | 来源 | 口径 |
|---|---|---|
| `load` | 参数 | 四类负载 |
| `engine` | 参数 | `lsm` / `raw_file` / `raw_pwrite` / `std_map` |
| `filter` | 参数 | `0` / `1`（LSM 行） |
| `batch` / `pipeline` / `sync` | 参数 | 固定打印 |
| `throughput_ops_per_sec` | CELL 行 | 中位数 |
| `latency_p50_us` / `p99_us` | CELL 行 | 中位数 / P99 |
| `write_amp_total` / `write_amp_excl_compact` | `AMPL` 行 | M4.3 口径（`m4-design.md:2161-2172`） |
| `space_amp` / `space_amp_sst_only` | `AMPL` 行 | 含/不含临时文件与 MANIFEST |
| `space_filter_bytes` | 诊断工具（见下） | filter 块总字节；追加列 |
| `space_sst_data_bytes` | `space_sst_bytes - space_filter_bytes` | 追加列 |
| `filter_fpr_measured` | `M5-A02` | 实测假阳性率（ppm 或 %） |
| `read_data_blocks_without_filter` / `read_data_blocks_with_filter` | `M5-A10` | 原始计数 |
| `read_data_blocks_ratio` | 计算 | `without / max(1, with)` |
| `read_filter_checked` / `read_filter_negative` / `read_filter_positive` / `read_filter_unavailable` | `AMPL` 追加列 | M5 的计数器 |
| `negative_result` | 人工/脚本 | `NONE` / `INVALIDATED` / `NET_LOSS` + 说明 |

**`space_filter_bytes` 的落点**：`PersistentDBImpl::FormatAmplLine`（M4.3 在途）**不**为这一列去开文件；
由 `scripts/lsm_level_stats.cpp`（M4 R10 的诊断入口，`m4-design.md:2596-2603`）在逐文件打开时用
`Table::filter_bytes()` 求和，追加到 `AMPL` 行尾。若 M4.3 未交付该工具，M5.3 必须先补它；
**不得**用「公式估算」冒充实测，也不得静默省略该列（`M5:150`）。

**三个放大的最终口径**（不重新定义，只引用并加列）：

- 写放大（主）：`(flush_write_bytes + compact_write_bytes) / user_logical_bytes`；对照列 `flush/user`。
- 读放大：`files_checked / get_count`（M4 口径不变）；**新增**「考虑 filter / 不考虑 filter」的
  `data_blocks_read` 两列。
- 空间放大（主）：`(sst + manifest + current + log + tmp) / user_logical_bytes`；**新增** `space_filter_bytes`
  与 `space_sst_data_bytes`，使「filter 的代价」可从同一行复算。
- `AMPL` 行的每列都**只追加**在行尾；M5 不重命名/不重排 M4.3 的前缀列（`m4-design.md:2141-2145`）。

---

## 7. 门禁脚本 `scripts/bench_lsm.sh` 行为规格

### 7.1 参数与解耦

- `bench_lsm.sh` 是 **shell 包装**：解析参数、写结果文件头、按 §6.3 交替调用 `bench/bench_lsm`（C++ 可执行）、
  解析固定行、施加门禁、打印正向标记。
- `bench/bench_lsm.cpp` 只 include `db.h` / `common.h`；通过 `DB::Open` / `DB::Put` / `DB::Get` /
  `DB::Write(WriteBatch*)` 驱动（`M5:166`）。**不得**为了基准暴露 `PersistentDBImpl` 的统计接口。
- 参数表见 §6.3；额外参数：
  - `--require-lsm`：若没有任何 `engine=lsm` 的 CELL 行 ⇒ 直接失败（防空绿）。
  - `--inject-missing`：失败注入自测。
  - `--expected-min-cells N`（默认 12）：要求结果文件至少 N 条 `CELL` 行（防空绿）。

### 7.2 输出

- 结果文件头部：`MACHINE`、`PARAMS`、`BUILD`、`FSYNC_BASELINE` 四行（§6.3）。
- 每个 CELL 一行（§6.3 的固定行）。
- 收尾汇总行（供 `lsm_gate.sh` 的正向标记 AND 使用）：

```
BENCH_CELLS_TOTAL <n>
BENCH_LSM_CELLS_TOTAL <n>
BENCH_MISSING_TOTAL <n>
BENCH_MISMATCH_TOTAL <n>
BENCH_UNRELIABLE_CELLS <n>
BENCH_REPRO_OK <0|1>
[BENCH_LSM_OK]
```

- `--inject-missing` 时，收尾行改为 `BENCH_INJECT_MISSING_RC <rc>` 与 `[BENCH_INJECT_MISSING_OK]`
  （由外层 `bench_lsm_selftest.sh` 读取），且脚本返回 1。

### 7.3 退出码与对账范围（`M5-C6` 的落地）

- **参与对账**：`engine=lsm` 的行的 `missing` / `mismatch`。对每个写入的 key，读回值必须逐字节相等；
  `missing` = 读不到的 key 数；`mismatch` = 值不一致的 key 数。任一非零 ⇒ 退出码 1。
- **不参与对账**：`engine=raw_file`、`raw_pwrite`、`std_map` 的行，其 `verify=na`、`missing=na`；
  它们**不**影响退出码，但影响数据表（§6.6）。
- 脚本退出码：
  - `0`：所有 LSM 格 `missing == 0 && mismatch == 0`，且 `BENCH_LSM_CELLS_TOTAL > 0`，
    且 `BENCH_CELLS_TOTAL >= expected-min-cells`，且 `BENCH_REPRO_OK==1`（若 `--repeats >= 2`）；
  - `1`：任一 LSM 格对账失败，或没有任何 LSM 格（空绿），或复现性失败，或结果文件缺固定行；
  - `2`：参数错误（与现有脚本约定一致，如 `lsm_gate.sh:30` 的 `unknown arg`）。
- **数据不一致的定义**：
  1. `missing != 0`（LSM 格）；
  2. `mismatch != 0`（LSM 格）；
  3. 同一格 `--repeats R` 的中位数差异超过阈值（`BENCH_REPRO_OK 0`）：
     吞吐相对差 `> 25%` 或 P99 相对差 `> 50%`（阈值可用 `--repro-tol` 覆盖并打印）；
  4. `--require-lsm` 且 LSM 格数为 0。
- **注意**：`sync=false` 的 LSM 格在进程级 `kill -9` 之前不会丢数据；本脚本**不**做 `kill -9`（那是 `M5-B01`），
  但结果文件必须写明 `sync` 值，并在 `docs/m5-bench.md` 注明「`sync=false` 的 `missing 0` 不代表掉电安全」。

### 7.4 失败注入自测（`M5:22`/`M5:149`）

- `scripts/bench_lsm_selftest.sh`（新增）：
  1. 构造一个**确定会 mismatch** 的 LSM 场景：`bench_lsm --inject-missing` 在写完数据后，
     故意让某个 key 的读回值不匹配（例如注入 `INJECT_MISSING=1` 时，某个 cell 的 verify 把
     一个 key 视为 missing）；
  2. 断言 `bench_lsm.sh --inject-missing` 的退出码为 **1**；
  3. 断言结果文件里出现 `BENCH_MISSING_TOTAL [1-9][0-9]*`（不是 0）；
  4. 打印 `BENCH_INJECT_MISSING_RC 1` 与 `[BENCH_SELFTEST_OK]`；否则退出 1。
- `lsm_gate.sh` 的 M5 腿对这条自测要求正向标记：`BENCH_INJECT_MISSING_RC 1@@\[BENCH_SELFTEST_OK\]`
  ——**防空绿**：如果脚本永远返回 0，这个标记不会出现，门禁失败。

### 7.5 与 `scripts/lsm_gate.sh` 的挂接（**必须做**）

- `lsm_gate.sh` 追加：
  - `--require-m5`（与 `--require-m3` 同形：缺脚本 ⇒ `FAIL`；默认缺 ⇒ `SKIP` + `[PARTIAL]`）；
  - `run_gate_m5_marked`（可直接复用 `run_gate_m3_marked` 的形状，把 `REQUIRE_M3` 换成 `REQUIRE_M5`）。
- 新增 M5 腿（每条都用 `run_gate_marked` 的多标记 AND）：

| 腿 | 脚本 | 正向标记（`@@` = AND） |
|---|---|---|
| M5-A 单元 | `scripts/lsm_m5_unit_test.sh` | `M5_FILTER_TESTS_RAN [1-9][0-9]*` @@ `M5_FILTER_FALSE_NEGATIVE 0` @@ `M5_FILTER_BLOCK_READ_RATIO [3-9][0-9]*(\.[0-9]+)?` @@ `M5_BATCH_TESTS_RAN [1-9][0-9]*` @@ `M5_BATCH_TESTS_FAILED 0` @@ `\[FILTER_OK\]` @@ `\[BATCH_OK\]` |
| M5-B 批崩溃 | `scripts/lsm_batch_crash_test.sh` | `BATCH_KILL9_MISSING 0` @@ `BATCH_HALF_VISIBLE 0` @@ `\[BATCH_CRASH_OK\]` |
| M5-C 基准 | `scripts/bench_lsm.sh` | `BENCH_CELLS_TOTAL [1-9][0-9]*` @@ `BENCH_LSM_CELLS_TOTAL [1-9][0-9]*` @@ `BENCH_MISSING_TOTAL 0` @@ `\[BENCH_LSM_OK\]` |
| M5-D 基准自测 | `scripts/bench_lsm_selftest.sh` | `BENCH_INJECT_MISSING_RC 1` @@ `\[BENCH_SELFTEST_OK\]` |
| M5-E filter 损坏 | `scripts/lsm_filter_damage_test.sh` | `FILTER_DAMAGE_CASES [1-9][0-9]*` @@ `FILTER_SILENT_FALSE_NEGATIVE 0` @@ `\[FILTER_DAMAGE_OK\]` |

- `scripts/lsm_m5_unit_test.sh`（新增）：
  - 运行 `build/bin/lsm_tests --gtest_filter='Filter.*:Bloom.*:WriteBatch.*:Batch.*'`；
  - 从输出里提取并打印 `M5_FILTER_TESTS_RAN` / `M5_FILTER_FALSE_NEGATIVE` / `M5_FILTER_BLOCK_READ_RATIO` /
    `M5_BATCH_TESTS_RAN` / `M5_BATCH_TESTS_FAILED`；测试用 `std::cout` 打印这些机器可读行（不能只依赖 GTest 的
    `[ OK ]` 行数，因为那不能证明「零假阴性」）。
  - 与 `lsm_manifest_test.sh` 一样，脚本自身也要对「缺 `build/bin/lsm_tests`」「0 个 OK 行」做硬失败。
- `scripts/lsm_batch_crash_test.sh` + `scripts/batch_crash_writer.cpp`（新增，不改 M2 的 `crash_writer.cpp`）：
  - writer 以 `WriteBatch` 为单位写 N 个批，sidecar 每批记一个「批指纹」；
  - `kill -9` 在批写入中途；recover 后对账：每个批要么全部 key 可见且值正确，要么全部不可见；
    `BATCH_HALF_VISIBLE` 必须为 0；
  - 复用 M2 的 `kill -9` 只证**进程级一致性**的措辞纪律（`m3-design.md:2004-2013`），**禁止**写「掉电安全已证明」。
- `scripts/lsm_filter_damage_test.sh`（新增）：
  - 构造带 filter 的 DB，逐字节/逐位翻转 filter 块的某些字节（重算块 CRC 的错位注入与不重算 CRC 的两种都要），
    断言：`Open`/`Get` 不失败、结果正确、`FILTER_SILENT_FALSE_NEGATIVE == 0`；
  - 错位注入（重算 CRC）必须被检出为 `kCorrupt`/`filter_unavailable`，证明测试有效（E4）。
- **反「空绿」**（`m3-prerequisites.md:716-729` D9.6）：每条 M5 腿必须是「退出码 0 **且** 多条标记 AND」；
  缺脚本默认 `SKIP` + `[PARTIAL]`，`--require-m5` 时 `SKIP` 即 `FAIL`。

---

## 8. 结论表达、负结果与与 M4 的关系

### 8.1 数据表的结论形态

`docs/m5-bench.md`（M5.3 产出）必须包含：

1. **机器状态声明**（照 M2 的 `machine/load/fs/mount` 固定行，`docs/m2-design.md` 的 §9.3 G6）与
   `BUILD rev/dirty/build_type`；
2. **fsync 成本基线**：本阶段用 `scripts/fsbench_commit_latency.cpp` 重测（**不继承 8 ms**），
   给出 `MIN_MS / MEDIAN_MS / P90_MS / MAX_MS`；
3. **四类负载 × 三类对照**的 `CELL` 表（§6.6），filter on/off 两列；
4. **三个放大的最终口径**：`AMPL` 行 + `space_filter_bytes` + `filter_fpr_measured`；
5. **LSM 劣势场景**（至少 3 条，逐条有数据）：
   - 点查不存在 key 时，若文件数很多而 filter 关闭，读放大高；
   - `sync=true` 时写吞吐受 fsync 限制（fsync 成本列）；
   - 空间放大含 filter 后上升（`space_filter_bytes`/`sst_bytes`）；
   - `std::map` 在纯内存点查/写入上明显更快（CPU/内存路径 vs IO 路径）；
   - 长尾：`P99` 在 compaction 发生的一轮可能抬高（M4 的 compaction 统计列）。
6. **负结果**：见 §8.2。

### 8.2 负结果入档（`M5:115`、`RM:61`、`M5:68`）

- 每个「做了没用/得不偿失」的尝试必须入档：**数据 + 归因 + 是否作废 + 原文保留**。
- 本设计预设的负结果候选：
  1. **filter 在小数据集/单文件上净收益为负**：filter 体积 + 构建成本 > 省下的块读；
     `negative_result=NET_LOSS`，并给出 `filter_bytes` 与 `blocks_saved`；
  2. **`batch` 很大时吞吐不升反降**（组提交 `kMaxGroupBytes=1 MiB` 的边界、MemTable 容量放大、
     WAL record 变大）；记录 `batch=1/16/256` 的实测；
  3. **`sync=true` vs `sync=false` 的差距**（fsync 物理成本）；
  4. **块缓存被否决**：登记为 `NOT_APPLICABLE` + 理由（§1.2 第 6 条）；
  5. **mmap/压缩被否决**：登记为 `NOT_APPLICABLE` + 理由（§1.2 第 2/3 条）。
- 禁止：只留成功数字、把负结果删掉、用「未做」冒充「做了更差」。

### 8.3 与 M4 的关系

- **读路径接入点**：M5 的 filter 否定判断接在 `Table::GetEntry`（`table.cpp:259` 稳定 rev）的
  「② lower_bound 之后、③ 读数据块之前」，这正是 `M5:56`/`M5:164` 说的「决定是否读块」的位置；
  `GetInternal`（`db_impl.cpp:431` 稳定 rev）的层级顺序（L0 新→旧、L1+ 层内有序）**不变**。
- **放大口径**：M5 在 M4 的 `AMPL` 行**追加** filter 列；M4 的写放大分子 `compact_write_bytes`
  包含 compaction 输出文件（含 filter 块），因此 filter 的写代价自动进入写放大；M5 只需显式打印
  `space_filter_bytes` 与 `space_sst_data_bytes`，让空间代价可复算。
- **compaction 与 filter 的交互**：compaction 输出新 SSTable 时，`TableBuilder` 按当时的
  `Options::bloom_bits` 决定是否写 filter；若 M4.3 的 `Compaction::Run` 传入的是 `Options`（`compaction.h:70` 稳定 rev
  的 `const Options& o`），则 filter 会自动生效；M5.0 必须复核 `Compaction::Run` 是否把 `options.bloom_bits`
  透传到 `TableBuilder`（应该在 `o` 里）。这是 M5.1 的关键接线点。
- **`TableCache`**：filter 随 `Table` 打开/缓存（`version_set.cpp:897-951` 稳定 rev）；M5 不新建缓存。
- **统计行**：M4.3 在途的 `AmplificationStats`/`FormatAmplLine` 是追加点；M5.0 复核（E7）。

---

## 9. 不变量与锁纪律的增量（**I47~I56 / L30~L35**）

### 9.1 指令原号 ↔ 落地号 ↔ 出处（**完整映射表**）

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

### 9.2 不变量 `I47~I56`（谁保证 + 怎么验）

| # | 谁保证（落点） | 怎么验（用例/命令） |
|---|---|---|
| **I47** | `BuiltinBloomPolicy::CreateFilter` 对每条 entry 的 **user key**（含 tombstone）建 filter；`FilterBlockReader::KeyMayMatch` 只用存储的 bits/k；`Table::GetEntry` 只在 `maybe==false` 时返回 `kNotFound` | `M5-A02`（零假阴性 + 边界 key）、`M5-A03`（tombstone/多版本）、`M5-A10`（块读下降门禁）；`lsm_gate.sh` 的 `M5_FILTER_FALSE_NEGATIVE 0` 标记 |
| **I48** | `FilterBlockBuilder::StartBlock` 的逐桶补空过滤器；`FilterBlockReader` 的 §3.2 八条解析校验；`Table::Open` 对错位置 `kCorrupt` | `M5-A04`（结构自洽 + 错位注入被检出 + 结果仍正确）、`M5-A06`（metaindex 注册/缺失）、`M5-B09`（磁盘 filter 损坏） |
| **I49** | `Table::GetEntry` 的 step ②.5：`maybe==true` 必须走原 step ③~⑥；`Table::NewIterator` 完全不调用 filter | `M5-A05`（肯定/否定/不可用三态的块读计数断言）、代码评审逐条核对 |
| **I50** | `FilterBlockReader` 构造后只读；`Table` 经 `shared_ptr<const Table>` 发布；`filter_state`/`filter_bytes` 不在读路径修改 | `M5-A06`、`M5-B06`（TSan 全量）、`M5-A18`（多线程点查无 race） |
| **I51** | §5.4 的四步证明：预校验 → 容量预检 → WAL 先于内存 → 恢复对象未发布 | `M5-A11`~`M5-A13`（编码/边界/上界）、`M5-A15`（截断）、`M5-A12`（提交失败无部分可见）、`M5-B01`（`kill -9` 半批不可见） |
| **I52** | `RunFlusher` 在组内一次性分配 `[begin, begin+total_count)`；恢复用 `seq+i` 重放；`last_sequence_ = max(replay_last, MaxSequenceInFiles)` | `M5-A16`（批内 sequence 连续 + `last_sequence` 覆盖）、`M5-A15`（恢复后可见性） |
| **I53** | `EncodeGroup` 把整组 entry 拼成一条 `batch_payload`；`WALWriter::Append` 一条逻辑 record；`ParseBatch` 恰好消费完 | `M5-A11`/`M5-A15`、`M5-B01`；`protocol.md` §9.4/§13 逐字对照 |
| **I54** | 组内 `need_sync = OR`；只有真 fsync 才推进 `durable_seq_`（`db_impl.cpp:371-378` 稳定 rev） | `M5-A14`（`sync=true` 的故障注入：fsync 失败 ⇒ 批不可见且返回错误）、`M5-B01` |
| **I55** | `bench_lsm.sh` 的 `PARAMS`/`MACHINE`/`BUILD` 行；`bench_lsm` 把实际生效参数逐项打印 | `M5-B02`（固定行解析）、`M5-B05`（复现性）、代码评审核对「打印值 == 实际值」 |
| **I56** | `bench_lsm.sh` 的 `missing != 0` 检查 + `lsm_gate.sh` 的 `run_gate_m5_marked` | `M5-B03`（失败注入自测：退出码 1 + `BENCH_INJECT_MISSING_RC 1`）、`M5-B07`（门禁全绿时的正向标记） |

### 9.3 锁纪律 `L30~L35`（谁保证 + 怎么验）

| # | 纪律 | 落地方式 | 验证 |
|---|---|---|---|
| **L30** | filter 构建单线程、发布后只读 | `TableBuilder` 单线程持有 builder；`Table::Open` 单线程构造 reader；`Table` 发布后 `filter_` 只读；`TableCache` 的 `shared_ptr<const Table>`（`version_set.h:195-224` 稳定 rev） | `M5-A06`、`M5-B06`（TSan）、`M5-A18`；代码评审核对 `filter_` 无写路径 |
| **L31** | WriteBatch 所有权/线程约束写进头文件 | `src/write_batch.h` 的类注释逐条列出（§5.1） | `M5-A17`（复用/所有权用例）、代码评审逐条核对 |
| **L32** | 批提交锁序 = M2 的 `commit_mu_ → mutex_` | `RunFlusher` 的 `std::lock_guard<std::mutex> ql(commit_mu_)` + `DbMutexGuard ml(mutex_)`（`db_impl.cpp:266-268` 稳定 rev） | `M5-A14`（并发批 + 无丢唤醒）、TSan 全量、代码评审核对 |
| **L33** | 基准计时窗口内禁止日志 IO | `bench_lsm` 的所有打印在窗口外；结果写文件在窗口外；若某轮需要日志，则关闭该轮或不计入 | `M5-B02`（行时间戳/阶段标记）、代码评审 |
| **L34** | filter 计数器：线程局部 delta + 汇总 | `Table::KeyMayMatch` 只写调用方传入的 `ReadStats*`（线程局部）；`GetInternal` 的 `DbReadStats delta` 在函数尾部经 `MergeReadStats` 在 `mutex_` 下累加（`db_impl.cpp:533` 稳定 rev 的既有模式） | `M5-A10`（计数正确）、`M5-A18`（TSan 多线程点查）、代码评审核对无共享自增 |
| **L35** | 禁止持 DB 锁做基准统计 IO | `AmplificationStats` 的目录扫描在锁外（M4.3 在途的实现已如此）；`bench_lsm` 不在持锁路径读文件 | `M5-B06`（SpyEnv/A36 同形探针仍通过）、代码评审 |

### 9.4 全局锁序与「M5 不新增锁」声明

- M2：`commit_mu_ → mutex_`（`docs/m2-design.md:108`、`db_impl.h:391-395` 稳定 rev）。
- M4：`install_mu_ → deletion_mu_ → mutex_`（`docs/m4-design.md:1986-2016`；`db_impl.h:344-349` 稳定 rev）。
- M5：**不新增锁**；`WriteBatch` 没有内部锁；`FilterBlockReader` 只读；`Pending` 仍在 `commit_mu_` 下操作。
- M5 的唯一「新顺序」是 `RunFlusher` 内既有的「锁外 WAL IO → 回锁 → 锁内 Add」（`db_impl.cpp:360-391` 稳定 rev），
  不是新锁，只是把单条 entry 换成多条 entry；不得在持 `mutex_` 期间做任何 IO（`L35`/M2 的 `I17`）。

---

## 10. 测试矩阵

### 10.1 A 组（确定性：`MemEnv` + `CountingEnv` + `FaultyEnv` + 块读计数器；无真实磁盘、零 flaky）

> 前置假设（必须写进 `docs/m5-prerequisites.md` §8.3）：filter 只覆盖该 SSTable 内的 key 集合；
> `Table::Open` 会读 footer/metaindex/index/filter（因此 filter 不能省这些 IO，只能省数据块读，E1）；
> `MemEnv` 的 `GetFileSize`/`RenameFile`/`SyncDir` 语义与 M3 的 A 组假设一致。

| 编号 | 用例 | 判据 | 需要的 seam |
|---|---|---|---|
| **M5-A01** | Bloom 位图/编码往返：空 filter、单 key、多 key、`n` 很大（> 8192）、`k` 上下界、`bits` 非 8 倍数回填 | `CreateFilter`→`KeyMayMatch` 对所有插入 key 为 true；`len==0` 桶为 false；结构解析通过 | 直调 `bloom.h` |
| **M5-A02** | 误判率实测（`N=100000` 插入、`M=100000` 未插入） | `FILTER_FALSE_NEGATIVE 0`；`FILTER_FPR_PPM <= 20000`（2.0%）；打印实测 ppm | `bloom.h` + 固定 RNG |
| **M5-A03** | 假阴性专项：所有已插入 key（含 tombstone、同 user key 多版本、最小/最大/相邻 key） | 每个 key 都返回 true（零假阴性）；tombstone-only 文件的 user key 也在 filter 中 | `TableBuilder`/`Table` + `MemEnv` |
| **M5-A04** | filter 与数据块对应：正常构建 + 错位注入（重算 CRC 的 offset 错位 / 不重算 CRC 的 bit flip） | 正常：`filter_state=kOk`、`Get` 正确；错位：`filter_state=kCorrupt` 或 `filter_unavailable>0`，`Get` 仍正确；**注入必须被检出**（E4） | `TableBuilder`/`Table` + 手工字节手术 |
| **M5-A05** | filter 只用于否定：构造「肯定」与「否定」两种 filter 状态，统计 `data_blocks_read` | 否定 ⇒ 0 数据块读；肯定 ⇒ ≥1 数据块读且 CRC/type 校验仍执行；不可用 ⇒ ≥1 数据块读 | `CountingEnv` + `PersistentDBImpl::GetReadStats()` |
| **M5-A06** | metaindex 注册/缺失：`bloom_bits=10` 写 filter；`bloom_bits=0` 不写；M3 reader 只计数 unknown；filter name 重复 ⇒ `kCorrupt` | `filter_state`/`unknown_metaindex_entries`/`kTableFormatVersion==1` 逐条断言 | `Table` + `TableBuilder` |
| **M5-A07** | 旧文件（无 filter）降级：用 `bloom_bits=0` 建库后，用 `bloom_bits=10` 重开读 | 全部 key 可读；`filter_unavailable > 0`；无假阴性 | `PersistentDBImpl` + `MemEnv` |
| **M5-A08** | filter 损坏降级：filter handle 越界、块 type 错、CRC 坏、payload 结构坏 | `Table::Open` 成功；`filter_state=kCorrupt`；`Get` 结果正确；计数 `filter_corrupt` | `MemEnv` + 字节手术 |
| **M5-A09** | `verify_checksums=false` 与 filter：关数据块校验后，filter 块 CRC 仍校验（E3） | filter CRC 坏 ⇒ 降级不可用（不是使用坏 filter）；数据块 CRC 关掉后行为与 M3 一致 | `Table` + `Options` |
| **M5-A10** | `≥3×` 同轮开关对照：同一数据集、同一不存在 key 集合，`bloom_bits=0` vs `10` 的 `data_blocks_read` | `without >= 3` 且 `without / max(1, with) >= 3.0`；打印两个原始计数；**只限不存在 key** | `PersistentDBImpl::GetReadStats()` + 两个 `MemEnv` DB |
| **M5-A11** | WriteBatch 编码往返：空批（拒绝）、单条、多条、混合 Put/Delete、空 value、大 value、`count` 上下界 | `Iterate` 逐条一致；`ParseBatch` 恰好消费完；`DB::Write` 对 `count==0`/超限返回 `kInvalidArgument` | `write_batch.h` + `db_impl` |
| **M5-A12** | 批原子性（MemTable 层）：正常写整批可见；WAL Append 失败 / fsync 失败 ⇒ 全部不可见；容量边界（batch 总 footprint 刚超 `write_buffer_size`）⇒ 新表整批可见 | 无部分可见；`missing`/`mismatch` 均为 0；提交失败返回错误 | `FaultyEnv`（fsync 注入）+ `MemEnv` |
| **M5-A13** | 容量上界证明的实测：随机 key/value 尺寸，比较 `ApproximateMemoryUsage` 增量与 `Σ(entry_bytes+128)` | 增量 ≤ 估计上界；否则测试失败（§5.4 第 4 步） | `MemTable` 诊断 |
| **M5-A14** | 批 + 组提交：N 个并发批写；`sync=true` 与 `false` 两种 | fsync 次数 << 批数；等待者全部唤醒（无丢唤醒）；所有 entry 可见；`durable_seq_` 覆盖批内最大 sequence | `CommitHook` + `FaultyEnv` |
| **M5-A15** | WAL 批记录截断：对含多 entry 的 record 逐字节截断（0..len） | 截断后该批完全不可见；完整时全部可见；`ParseBatch` 不产生半批；`records_skipped` 计数 | `MemEnv` + `WALReader` |
| **M5-A16** | 恢复 sequence：批内连续、`last_sequence` 覆盖批内最大、重放后值与写入一致 | 逐条断言；`last_sequence == seq + count - 1`（无更大文件 sequence 时） | `MemEnv` + `RecoveryStats` |
| **M5-A17** | WriteBatch 所有权/复用：`DB::Write` 返回后修改/`Clear` 同一对象；`WriteBatch` 无内部锁（同一对象跨线程并发访问是 UB，文档写明） | 复用后再次提交结果正确；`Iterate` 对畸形 rep 返回 `kCorruption` | `write_batch.h` |
| **M5-A18** | 多线程点查 + filter（`pipeline>1`）：读同一 `TableCache` 缓存的 `Table` | TSan 干净；每个线程的 delta 计数汇总正确；无数据竞争（`L30`/`L34`） | `build-tsan` + `PersistentDBImpl` |

**A 组纪律**：

- 零断言 TEST 块 = 0；`DISABLED_`/`GTEST_SKIP`/`|| true` = 0；既有断言不得被删除（`m3-evidence.md:32-39` 的方法）。
- 每条「机制」用例必须有**反向自检**：例如 filter 否定用例要断言「如果 filter 被误用，结果会错」；
  `M5-A04` 的错位注入要断言「注入确实被检出」。
- 若 M5.1 追加 `FakeClock`，只放 `tests/test_harness.h`，不得进 `src/`（E6）。

### 10.2 B 组（真实磁盘 / 进程级，脚本驱动）

| 编号 | 名称（脚本/命令） | 依赖假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| **M5-B01** | `scripts/lsm_batch_crash_test.sh`（新增；`kill -9` 在批写入中途） | 真实目录、真实进程；`kill -9` 不丢 page cache（M2 §11.3）⇒ 只证明**进程级**一致性 | `BATCH_KILL9_MISSING 0` ∧ `BATCH_HALF_VISIBLE 0` ∧ `[BATCH_CRASH_OK]`；退出码 0 | 新的批 writer + sidecar 批指纹 + 固定行格式 |
| **M5-B02** | `scripts/bench_lsm.sh` 全流程（四类负载 × {LSM / 裸文件 / 内存 map} × filter on/off） | 真实目录；固定参数；预热/重复/中位数；机器状态打印 | 固定 `CELL/THROUGHPUT/LATENCY/P99` 行；LSM 格 `missing 0`；`BENCH_CELLS_TOTAL >= 12`；`BENCH_LSM_CELLS_TOTAL >= 4`；`[BENCH_LSM_OK]` | `bench_lsm` 可执行 + 参数解析 + 结果文件头 |
| **M5-B03** | `scripts/bench_lsm_selftest.sh`（门禁失败注入自测） | 构造一次 LSM 格 `missing != 0` | `bench_lsm.sh --inject-missing` 退出码 **1**；`BENCH_MISSING_TOTAL [1-9][0-9]*`；`BENCH_INJECT_MISSING_RC 1`；`[BENCH_SELFTEST_OK]` | `--inject-missing` |
| **M5-B04** | 写放大与空间占用（含 filter 体积） | 真实目录；`AMPL` 行 + `space_filter_bytes` | `space_filter_bytes > 0`（filter on）；`space_sst_data_bytes == space_sst_bytes - space_filter_bytes`；写放大两列可复算 | M4.3 的 `GetAmplificationStats`/`lsm_level_stats`（在途，M5.0 复核） |
| **M5-B05** | 复现性验证：同参数连跑两轮 + 交替顺序 | 机器状态打印；`load1 <= nproc` | 两轮中位数差在 `--repro-tol` 内（默认 25%/50%）；换轮次/交替顺序不改变结论方向；`BENCH_REPRO_OK 1` | 脚本重复执行 + 原始值打印 |
| **M5-B06** | ASan / TSan / 干净重建 0 warning + 全量用例 | `build`/`build-asan`/`build-tsan`；TSan 需 `setarch $(uname -m) -R` | `[ PASSED ] N tests`（N ≥ 167 + M5 新增）；ASan 零报告；TSan 零 race；`lsm_build.sh` warning 0 | 三构建目录 |
| **M5-B07** | 全部门禁收口：`lsm_gate.sh --require-m3 --require-m5 --with-tsan` | M3/M4/M5 腿都存在 | 全部 `PASS`；末行 `[OK]`（不是 `[PARTIAL]`）；M5 五条腿的正向标记全部出现 | `run_gate_m5_marked` + 新脚本 |
| **M5-B08** | 旧库兼容：M3/M4 写的无 filter 库用 M5 二进制读；M5 写的带 filter 库用 `bloom_bits=0` 读 | 真实目录 | 全部 key 可读；`filter_unavailable` 合理；无假阴性 | `PersistentDBImpl` + 参数 |
| **M5-B09** | 磁盘 filter 损坏：翻转 filter 块字节（含重算 CRC 的错位注入） | 真实文件 | 读结果正确；`FILTER_SILENT_FALSE_NEGATIVE 0`；错位被检出；`[FILTER_DAMAGE_OK]` | `lsm_filter_damage_test.sh` + 字节手术 |
| **M5-B10** | 负结果入档：filter 净损失、batch 大反而慢、`sync` 差距等 | 数据集与参数可复现 | 每条负结果有数据 + 归因 + 是否作废；`AMPL`/`CELL` 行原文保留 | `docs/m5-bench.md` |

**B 组的诚实性纪律**（沿用 `m3-design.md:2004-2013`）：

- 不得由「`sync=false` 的 `missing 0`」宣称 `sync=false` 有持久性（`kill -9` 不丢 page cache）；
- 不得由「`kill -9` 没产生 torn 文件」宣称「掉电安全」；掉电语义只能靠 `MemEnv` 回滚注入（A 组）承担；
- `kill -9` 系列证明的是**进程级一致性**，`MemEnv` 系列证明的是**掉电语义**，两者的结论必须分开写。

### 10.3 反「空绿」检查（逐条适用）

| 检查 | 方法 | M5 的期望 |
|---|---|---|
| 零断言 TEST 块 | `awk` 逐 `TEST(...){...}` 块扫描 `EXPECT_/ASSERT_` | **0 个** |
| 跳过/禁用/永真 | `grep -nE 'DISABLED_\|GTEST_SKIP\|\|\| true' tests/` | **0 处** |
| 既有断言是否被放宽 | `git diff -U0` 中被删除行里的 `EXPECT_/ASSERT_` 计数 | **0**（M5 不得改 M1~M4 既有测试断言，`M5:130`） |
| filter 测试是否真的零假阴性 | `M5_FILTER_FALSE_NEGATIVE 0` 必须是**打印出来的计数**，不是「测试没失败」 | 必须 |
| 块读下降是否真的同轮 | `M5-A10` 必须在同一进程、同一数据集、同一查询集合内比较两个 DB | 必须 |
| 基准是否真的跑了 LSM | `BENCH_LSM_CELLS_TOTAL > 0` + `BENCH_MISSING_TOTAL 0` | 必须 |
| 门禁失败注入是否真的失败 | `BENCH_INJECT_MISSING_RC 1` + `[BENCH_SELFTEST_OK]` | 必须 |
| 门禁是否只看退出码 | `lsm_gate.sh` 的 `run_gate_m5_marked` 多条标记 AND | 必须 |
| `--require-m5` 的 SKIP 语义 | 缺脚本 ⇒ `FAIL`；默认 ⇒ `SKIP` + `[PARTIAL]` | 必须 |
| 「filter 真的省了块读」 | `data_blocks_skipped_by_filter > 0`（`M5-A05/A10`） | 必须 |
| 「compaction 真的发生了」 | 若基准/回归声称覆盖 compaction，则 `COMPACTION_ROUNDS_TOTAL > 0`（M4 的正向标记，M4-B11） | 若涉及则必须 |

---

## 11. 子里程碑拆分（M5.0 → M5.1 → M5.2 → M5.3）

### M5.0 —— 开工前置复核（**不是功能提交**；`docs(m5):` 一次文档提交）

> 照 `docs/m4-design.md:2206-2238` 的 M4.0 先例。M5.0 的产出是
> `docs/m5-prerequisites.md` 的「§0 前置复核 + §2 差异登记」，**不写业务实现代码**。
> **阻断条件**：若 M4.3 仍未提交（`git status --porcelain` 非空且含 `src/db_impl.*`），
> M5.1 不得开工；M5.0 只能产出复核报告并等 M4.3 提交或用户明确授权。

**可粘贴复核命令（在 VM `~/lsm-kv`，只读）**：

```bash
cd ~/lsm-kv

# ① 基线 / tag / 工作区（M4 是否收口）
git rev-parse HEAD
git status --porcelain
git tag
git log --oneline -5
git diff --stat

# ② M4.2 执行体是否落地（M5 基准要在真实 compaction 上跑）
git show HEAD:src/compaction.cpp | grep -n 'Status Compaction::Run'
git show HEAD:src/compaction.h  | grep -n 'class Compaction\|static Status Run'

# ③ 读路径层级化 + 组提交实际形态（M5 的接入点）
git show HEAD:src/db_impl.cpp | grep -n 'Status PersistentDBImpl::GetInternal\|Status PersistentDBImpl::Write(ValueType\|EncodeGroup\|RunFlusher\|Compaction::Run'
git show HEAD:src/db_impl.h   | grep -n 'struct Pending\|struct DbReadStats\|class PersistentDBImpl\|Status Write(ValueType'

# ④ TableCache / Table / ReadStats / format 的实际签名
git show HEAD:src/version_set.h  | grep -n 'class TableCache\|Status Get(\|NewIterator'
git show HEAD:src/version_set.cpp | grep -n 'TableCache::Open\|TableCache::Get'
git show HEAD:src/sstable/table.h | grep -n 'struct ReadStats\|class Table\|GetEntry\|ParseMetaIndexBlock'
git show HEAD:src/sstable/table_builder.h | grep -n 'class TableBuilder\|Status Add\|Status Finish'
git show HEAD:src/sstable/format.h | grep -n 'kBlockTypeFilter\|kTableFormatVersion\|kFooterSize'

# ⑤ 协议章节（M5 追加 §12/§13 的前提）与 metaindex 预留
git show HEAD:docs/protocol.md | grep -n '^### 9\.4\|^### 10\.6\|^### 10\.7\|^## 11'
git show HEAD:docs/m3-design.md | grep -n 'BuiltinBloomFilter2\|kTableFormatVersion 不升\|M5 加 Bloom\|M5 的 filter'

# ⑥ Options / DB / WAL / MemTable 的实际形态
git show HEAD:src/common.h | grep -n 'struct Options\|bloom\|block_size\|verify_checksums\|max_open_files\|level0_file'
git show HEAD:src/db.h | grep -n 'struct WriteOptions\|virtual Status'
git show HEAD:src/wal.h | grep -n 'kMaxLogicalRecordSize\|class WALWriter'
git show HEAD:src/memtable.h | grep -n 'WouldReject\|Status Add\|kFrozen'

# ⑦ M4 设计对 M5 的预留（`AMPL` 行、I/L 顺延）
git show HEAD:docs/m4-design.md | grep -n 'M5 的顺延\|I47~I56\|L30~L35\|只允许在行尾追加\|M5 要加'
git show HEAD:docs/m4-design.md | sed -n '2139,2199p'
git show HEAD:docs/m4-design.md | sed -n '1926,1990p'

# ⑧ 门禁机制与现有腿
grep -n 'run_gate_marked\|run_gate_m3_marked\|require-m3\|PARTIAL' scripts/lsm_gate.sh
cat -n scripts/lsm_manifest_test.sh

# ⑨ 测试 seam 是否存在（`FakeClock` 缺失必须登记）
grep -rn 'FakeClock' tests src || true
grep -n 'class CountingEnv' tests/sstable_counting_env.h
grep -n 'class FaultyEnv' tests/faulty_env.h
ls -la tests/memenv.h tests/faulty_env.h tests/sstable_counting_env.h

# ⑩ M5 产物是否已存在（应为空）
ls -la docs/m5-design.md docs/m5-prerequisites.md docs/m5-bench.md 2>&1
ls -la scripts/bench_lsm.sh scripts/lsm_m5_unit_test.sh scripts/lsm_batch_crash_test.sh 2>&1

# ⑪ 测试总数（M5 不得让既有用例数下降）
grep -h "TEST(" tests/*.cpp | wc -l

# ⑫ 若 M4.3 已提交：复核 AMPL 行的真实列名与插入点
git show HEAD:src/db_impl.h | grep -n 'AmplificationStats\|LevelStats\|FormatAmplLine'
git show HEAD:src/db_impl.cpp | grep -n 'FormatAmplLine\|GetAmplificationStats\|GetLevelStats'
```

**复核判据**：

1. `git status --porcelain` 为空且 `m4-compaction` tag 存在 ⇒ M4 收口，M5.1 可以开工。
2. 若工作树仍脏（M4.3 在途）⇒ **暂停**，把差异写进 `docs/m5-prerequisites.md` §2，等 M4.3 提交/用户授权。
3. `FormatAmplLine` 的列名/位置与本文 §6.6 不一致 ⇒ 以落地实现为准，登记差异并更新本文的追加列清单。
4. `FakeClock` 仍不存在 ⇒ 登记为「M5 可选新增测试辅助」，A 组按 E6 用 `MemEnv`+`CountingEnv`+`FaultyEnv`。
5. `Options` 仍无 `bloom_bits`、`DB` 仍无 `Write(WriteBatch*)` ⇒ 与本文 §5.2/§5.6 一致，M5.1/M5.2 落地。
6. `Table::Open`/`Table::GetEntry`/`TableBuilder::Add`/`Finish` 的实际签名与本文 §3.6 草案不一致 ⇒
   以落地实现为准，更新 §3.6/§9.2 的「谁保证」列，并在 `docs/m5-prerequisites.md` §2 登记。

**M5.0 提交信息**（若 M4 已收口）：`docs(m5): M5.0 前置复核 + 差异登记（docs/m5-design.md §11）`。

### M5.1 —— Bloom Filter + filter block + metaindex + 读路径否定 + 计数器

| 项 | 内容 |
|---|---|
| 新增 | `src/filter_policy.h`、`src/bloom.{h,cpp}`、`src/util/hash.{h,cpp}`（可选，见 §3.3 的替代落点）、`tests/filter_test.cpp` |
| 必改 | `src/common.h`（`Options::bloom_bits`）、`src/sstable/format.h`（常量）、`src/sstable/table_builder.{h,cpp}`（写 filter）、`src/sstable/table.{h,cpp}`（读 filter + `KeyMayMatch` + `ReadStats` 追加）、`src/version_set.{h,cpp}`（`TableCache::Open` 传递 `open_stats`，若需要）、`src/db_impl.{h,cpp}`（`DbReadStats`/`MergeReadStats` 追加）、`CMakeLists.txt`（新源文件进 `lsm` 与 `lsm_sstable`；`lsm_tests` 加 `filter_test.cpp`）、`docs/protocol.md`（追加 §12） |
| 判据 | `filter_test` 全绿（`M5-A01~A10`，含**零假阴性**、误判率实测、`≥3×` 同轮门禁、旧文件降级、损坏降级）；ASan/TSan 干净；干净重建 0 warning；`lsm_gate.sh` 的 M5-A/M5-E 腿正向标记 |
| 提交信息 | `feat(m5): M5.1 Bloom filter + filter block + metaindex（docs/m5-design.md §3/§4 §12/§9.2 I47~I50）` |
| 证据命令 | 见下 |

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh
./build/bin/lsm_tests --gtest_filter='Filter.*:Bloom.*:Table.*:SSTable.*'
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests --gtest_filter='Filter.*:Bloom.*'
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j8 && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter='Filter.*:Bloom.*'
bash scripts/lsm_m5_unit_test.sh      # 期望 M5_FILTER_* 标记 + [FILTER_OK]
bash scripts/lsm_gate.sh --rounds 20 --no-asan --require-m3 --require-m5   # M5-A/M5-E 腿
grep -n 'kBuiltinBloomFilterName\|kFilterBaseLg' src/sstable/format.h docs/protocol.md
```

### M5.2 —— WriteBatch 编码 + `DB::Write` + 批提交 + WAL 一次写 + 崩溃原子性

| 项 | 内容 |
|---|---|
| 新增 | `src/write_batch.{h,cpp}`、`tests/batch_test.cpp`、`scripts/lsm_batch_crash_test.sh`、`scripts/batch_crash_writer.cpp`（新 writer；不改 M2 的 `crash_writer.cpp`） |
| 必改 | `src/db.h`（`Write(WriteOptions, WriteBatch*)`）、`src/db.cpp`（`MemoryDBImpl::Write`）、`src/db_impl.{h,cpp}`（`WriteEntry` 改名、`Pending` 扩展、`EncodeGroup`/`RunFlusher` 扩展）、`src/wal.{h,cpp}`（如需要，仅追加批承载辅助；不改物理格式）、`CMakeLists.txt`、`docs/protocol.md`（追加 §13） |
| 判据 | `batch_test` 全绿（`M5-A11~A17`）；`kill -9` 半批不可见（`M5-B01`）；`M5-A14` 无丢唤醒；`sync=true` 故障注入批不可见；ASan/TSan 干净；`lsm_gate.sh` 的 M5-B 腿正向标记 |
| 提交信息 | `feat(m5): M5.2 WriteBatch + 批提交 + WAL 一次写（docs/m5-design.md §5/§4 §13/§9.2 I51~I54）` |
| 证据命令 | 见下 |

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh
./build/bin/lsm_tests --gtest_filter='WriteBatch.*:Batch.*:Recovery.*'
bash scripts/lsm_batch_crash_test.sh --rounds 100 --batch-size 16 --write-buffer-size 262144
# 期望 BATCH_KILL9_MISSING 0 / BATCH_HALF_VISIBLE 0 / [BATCH_CRASH_OK]，退出码 0
bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3 --require-m5   # M5-B 腿
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests --gtest_filter='WriteBatch.*:Batch.*'
setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter='WriteBatch.*:Batch.*'   # 需先构建 build-tsan
```

### M5.3 —— 微基准 + 四类负载 × 三类对照 + 数据表 + `bench_lsm.sh` 门禁 + 负结果入档

| 项 | 内容 |
|---|---|
| 新增 | `bench/bench_lsm.cpp`、`scripts/bench_lsm.sh`、`scripts/bench_lsm_selftest.sh`、`scripts/lsm_filter_damage_test.sh`、`docs/m5-bench.md`、`scripts/lsm_level_stats.cpp`（若 M4.3 未交付则 M5 补，用于 `space_filter_bytes`） |
| 必改 | `CMakeLists.txt`（新 `bench_lsm` 目标；`lsm_tests` 不加 bench 源）、`src/db_impl.{h,cpp}`（`AMPL` 行**行尾追加** filter 列；M4.3 在途，M5.0 复核）、`scripts/lsm_gate.sh`（`--require-m5` + M5 五条腿）、`.gitignore`（bench 临时目录）、`docs/m5-bench.md`（结果 + 负结果） |
| 判据 | `bench_lsm.sh` 全流程可复现；四类负载 × 三类对照的固定 `CELL` 行；LSM 格 `missing 0`；失败注入自测退出码 1（`M5-B03`）；复现性 `BENCH_REPRO_OK 1`；`AMPL` 追加 filter 列可复算；负结果入档；ASan/TSan 干净；`tag m5-optimize` 并 push |
| 提交信息 | `feat(m5): M5.3 微基准与数据表 + bench_lsm 门禁 + 负结果入档（docs/m5-design.md §6/§7/§8）` |
| 证据命令 | 见下 |

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh
# ① fsync 成本基线（M5 自己重测，不继承 8 ms）
./build/bin/fsbench_commit_latency /tmp/lsm_fsb 500
# ② 四类负载 × 三类对照 × filter on/off
bash scripts/bench_lsm.sh --dataset 100000 --value-size 100 --batch 1 --pipeline 1 --sync 0 --filter on  --repeats 3 --out /tmp/lsm_bench_on.txt
bash scripts/bench_lsm.sh --dataset 100000 --value-size 100 --batch 1 --pipeline 1 --sync 0 --filter off --repeats 3 --out /tmp/lsm_bench_off.txt
grep -E '^(MACHINE|PARAMS|CELL|BENCH_)' /tmp/lsm_bench_on.txt | head -40
grep -c '^CELL ' /tmp/lsm_bench_on.txt
grep '^BENCH_LSM_CELLS_TOTAL' /tmp/lsm_bench_on.txt
grep '^BENCH_MISSING_TOTAL' /tmp/lsm_bench_on.txt
# ③ 失败注入自测（必须退出码 1）
bash scripts/bench_lsm_selftest.sh; echo "selftest_rc=$?"   # 期望 1 + [BENCH_SELFTEST_OK]
# ④ filter 损坏扫描
bash scripts/lsm_filter_damage_test.sh --cases 2000        # 期望 FILTER_SILENT_FALSE_NEGATIVE 0 + [FILTER_DAMAGE_OK]
# ⑤ 全部门禁收口（含 TSan）
bash scripts/lsm_gate.sh --rounds 100 --with-tsan --require-m3 --require-m5
# ⑥ 复现性 / 原始值
grep -E '^BENCH_REPRO_OK|^BENCH_CELLS_TOTAL|^BENCH_UNRELIABLE_CELLS' /tmp/lsm_bench_on.txt
# ⑦ 数据表与负结果
grep -n '^## ' docs/m5-bench.md
git add -A && git commit -m 'feat(m5): M5.3 ...' && git tag -a m5-optimize -m 'M5 优化与微基准（docs/m5-design.md）' && git push origin m5-optimize
```

**注意**：M5.3 的 `lsm_gate.sh` 默认不跑 M5 腿时仍打印 `[PARTIAL]`（`SKIP` 不是通过）；
`--require-m5` 必须让缺脚本变成硬失败。**tag 只能在全部判据（含 ASan/TSan/崩溃/基准门禁）都通过后打**（`M5:200`）。

---

## 12. 自检（占位符 / 内部矛盾 / 歧义 / 范围越界 / 已知薄弱点）

### 12.1 占位符

- 无 `TODO`/`TBD`/`XXX`/`FIXME`。
- 所有新增常量/字段有取值与语义：`Options::bloom_bits=10`、`kFilterBaseLg=11`、
  `kBuiltinBloomFilterName`、`WriteBatch::kMaxCount=1<<20`、`WriteBatch::kMaxBytes=64 MiB`、
  `kMemTableNodeOverhead=128`（沿用）、`BENCH_*` 标记、`M5_*` 标记。
- 未实测的数字一律写「未验证（需 M5 #1/M5.3 实测）」：fsync 成本、Bloom 误判率、块读下降倍数、吞吐/延迟/P99。
- 引用的行号来自 `git show db5aa8f:<path>` 或 `docs/*.md` 的稳定 rev；M4.3 未提交部分单独标注。

### 12.2 内部矛盾（逐条核对）

| 潜在矛盾 | 处置 |
|---|---|
| §1.1 G1 说「在打开文件前做否定判断」vs E1 说「不能省文件打开」 | **已在 E1 消解**：精确化为「读数据块之前」；`M5:18` 的判据就是数据块读次数；`M5:10` 的措辞按此解释并在 `docs/m5-bench.md` 如实写限制 |
| D1 的 `k=7` vs LevelDB `0.69` 截断得到 `k=6` | **不矛盾**：协议把 k 存在每个 filter 尾部，reader 用存储值；本设计取 `round` 以匹配 `M5:10` 的「k ≈ 7」；两套实现可互读 |
| §3.2 的「len==0 ⇒ false」vs 「空桶无数据块」 | **一致**：builder 只为「有 key 的桶」写非空 filter；空桶的 0 长度 filter 不会被真实数据块的 `index` 命中 |
| E3 的「filter CRC 始终校验」vs 用户设 `verify_checksums=false` 的预期 | **有意收紧**：filter 是丢数据级安全设施；只对 filter 块强制 CRC，不改数据块的 `verify_checksums` 语义；已在 §12.5 登记 |
| §5.4 的「预检后 Add 不会失败」vs `MemTable::Add` 仍可能因 `kFrozen` 失败 | **证明在 §5.4 第 2~4 步**：`footprint` 是 `BytesAllocated()` 增量上界 ⇒ 预检 false 蕴含每次 `WouldReject(0)` false；`M5-A13` 实测该上界 |
| D4 的 `WriteBatch::kMaxBytes=64 MiB` vs「避免大 batch 内存峰值」 | **折中**：协议上限保持 64 MiB（与既有单条写一致）；benchmark/推荐用法用 1 MiB 以下的批；内存峰值上界 = 3×批 + MemTable 容量，写进 §12.5 |
| §6.6 的 `space_filter_bytes` 由 `lsm_level_stats` 追加 vs `FormatAmplLine` 是 `PersistentDBImpl` 的方法 | **不矛盾**：`lsm_level_stats` 可以调用 `PersistentDBImpl::FormatAmplLine` 后再追加列，或者由它自己打开文件求和；两种落点都保持「行尾追加」纪律；E7 要求 M5.0 复核 |
| `M5:84` 的旧号 vs §9.1 的 `I47~I56` | **M5-C1 已裁决**；映射表给全 |
| `M5:96` 的 `L1~L26` vs M4 实际到 `L29` | **同上**；映射表给全 |
| `bench_lsm.sh` 要退出码 1 vs 对照腿无对账 | **M5-C6 已裁决**：只对 LSM 腿对账，`verify=na` 的腿不参与；失败注入只对 LSM 格 |
| `≥3×` 与「不写提升 N 倍」 | **M5-C5 已裁决**：`≥3×` 只作同轮开关的**块读计数**门禁，且只限不存在的 key；禁止外推 |
| 「filter 默认开」vs「默认配置不得为好看而改」 | **M5-C7 已裁决**：默认开是 `M5:10` 的规定；对照关闭必须显式传参并打印；两列都在同一脚本/同一轮内产生 |
| mmap/压缩「非目标」vs 决策 6「开放」 | **M5-C8 已裁决**：以非目标为准，不做；只登记理由 |

### 12.3 歧义（逐条消解，给出唯一解释）

| 歧义点 | 本文的唯一解释 |
|---|---|
| 「一个 filter block」的粒度 | 每个 SSTable 恰好一个 filter **块**；块内按 2 KiB 桶多个 bitset（E2） |
| 「filter 只允许产生否定」 | 只允许在 `Table::GetEntry` 的 ②.5 处、对「将要读的那个数据块」返回 false；false 只能映射为 `kNotFound`（本文件没有该 user key） |
| 「filter 说可能存在就直接返回」 | 一律视为阻断性缺陷；`I49` 的用例 `M5-A05` 强制「true ⇒ 必须读块」 |
| 「降级」 | `kAbsent`/`kCorrupt`/索引越界/len==1 一律按「可能存在」处理并计数；不报错、不跳过 |
| 「旧文件」 | metaindex 无 filter name 的 M3/M4 文件；`filter_state=kAbsent` |
| 「一次 WAL record 承载整批」 | 一个 `WriteBatch` 的全部 entry 必须在同一条 record 内；组提交可合并多个 `WriteBatch`，但不得拆一个批 |
| 「整批原子」 | 崩溃恢复后不存在半批；正常提交失败时不存在半批（§5.4） |
| 「批大小上限」 | `count ≤ 1<<20` 且 `ByteSize()+16 ≤ 64 MiB`；超限在编码/入队前拒绝 |
| 「同轮交替 A/B」 | LSM vs 对照、filter on vs off 都在同一脚本/同一轮内交替（E8） |
| 「机器状态」 | 必须打印 `MACHINE ... load1/5/15 fs mount`；`load1 > nproc` 标 `UNRELIABLE` |
| 「哪些 CELL 参与对账」 | 只有 `engine=lsm` 的 `missing/mismatch`；其它 `verify=na` |

### 12.4 范围越界（是否偷偷带了 M5 禁列 / M6 内容）

逐条对照 §1.2 与 `M5:25-26`：

| 出现的东西 | 为什么不是越界 |
|---|---|
| `Options::bloom_bits` | M5 的 filter 开关与参数（D1/C7）；不是 M4 的层级字段 |
| `WriteBatch` / `DB::Write` | M5 的目标 2（`M5:11`）；不是 M6 的 raft 适配 |
| `bench/bench_lsm.cpp` / `scripts/bench_lsm.sh` | M5 的目标 3/5（`M5:12`/`M5:14`） |
| `AMPL` 行追加 filter 列 | `docs/m4-design.md:2141-2145` 明写「只允许在行尾追加新列（M5 要加 fitler 列）」——是 M4 留下的扩展点，不是 M4 内容 |
| `space_filter_bytes` | `M5:150` 要求「量化 Bloom 的代价」 |
| `filter_fpr_measured` | `M5:136` 要求「记录实测值，作为数据表的一列」 |
| `tests/test_harness.h` 追加 `FakeClock`（可选） | `M5:128` 要求追加测试辅助；只进 tests/ |
| `scripts/lsm_batch_crash_test.sh` / `batch_crash_writer.cpp` | M5 的目标 2 验收（`M5:20`/`M5:147`）；不改 M2 的既有脚本 |

**明确声明：本设计不含** 多线程并行 compaction / subcompaction / mmap / 压缩 / 块缓存 / Column Family /
事务 / MVCC 增强 / raft-kv 对接 / 公共 `Snapshot*` API / `ReadOptions` 扩展 / M3 数据块与索引块格式变更 /
M4 的 compaction 正确性逻辑与 tombstone 丢弃条件变更 / `kTableFormatVersion` 升版。

### 12.5 已知薄弱点（主动暴露，供 `#4` 评审攻击）

1. **E1 的「filter 不能省文件打开」**：metaindex/index/footer 的读成本仍在；文件数很多而点查稀疏时，
   filter 只能省数据块读，不能省固定成本。已写进 §3.7/§8.1 的劣势场景。
2. **filter CRC 强制校验**：E3 的收紧会让 `verify_checksums=false` 的用户也付 filter CRC 成本；
   权衡是「filter 假阴性是丢数据级」；若 `#4` 认为不可接受，可改为「`verify_checksums=false` 时禁用 filter」，
   但必须在 `docs/m5-prerequisites.md` 登记并重跑 `M5-A09`。
3. **`WriteBatch` 的内存峰值**：一个 64 MiB 的批在 `WriteBatch` rep + `Pending.entries` + `EncodeGroup` payload
   上可能三倍放大（≈192 MiB），加 MemTable 容量；协议上限是 64 MiB，benchmark 默认远小于此；
   若 `#4` 要求更小，可把 `kMaxBytes` 调小（只改常量与协议 §13.3 的文字）。
4. **`kMemTableNodeOverhead=128` 的平台前提**：x86-64 + `alignof(max_align_t)=16`；`M5-A13` 实测上界；
   若平台变化，调大常量即可（不落盘）。
5. **`space_filter_bytes` 的落点未冻结**：M4.3 在途；E7 要求 M5.0 复核，若 M4.3 未交付则 M5.3 补
   `scripts/lsm_level_stats.cpp`；在落地前，该列只能标 `未验证`。
6. **`FakeClock` 不存在**：E6；M5 的 A 组不依赖它；若 M5.1 追加，只进 tests/ 并登记。
7. **基准的机器状态不可控**：VM 的 host 页缓存可能承接 fsync（M2 §11.2 的 2.3~2.9 ms vs 历史 8 ms）；
   M5 的结果文件必须打印机器状态，且 `docs/m5-bench.md` 必须声明「本机 fsync 是否真的到介质无法从 guest 验证」。
8. **`≥3×` 的数据集依赖**：若数据集太小/文件数太少，`without` 可能 <3；`M5-A10` 必须放大数据集而不是放行。
9. **compaction 抖动**：随机写基准可能把 compaction 的抖动算进吞吐/P99；数据表必须打印 compaction 参数
   （`level0_file_num_compaction_trigger` 等）并给出 `COMPACTION_ROUNDS`（若可观测）；不可观测时写「未验证」。
10. **`WriteBatch` 的 `Iterate` 语义**：`Iterate` 失败时可能已回调部分 entry；`DB::Write` 必须使用
    「无副作用预校验」而不是直接用 `Iterate` 写内存；§5.4 第 1 步已要求。
11. **M4.3 并发**：本文的所有 `src/db_impl.*` 行号取自 `db5aa8f`；M4.3 落地后行号与接口可能变；
    M5.0 必须重锚。这是流程风险，不是设计缺陷，但**必须**在 `docs/m5-prerequisites.md` §0/§2 登记。

### 12.6 需用户拍板清单（汇总）

| # | 问题 | 本文推荐 | 影响面 |
|---|---|---|---|
| Q1 | filter 粒度：一个文件一个 filter block（内部 2 KiB 桶）？ | **是**（`M5-C2`） | §3.2/§3.3 |
| Q2 | 不升 `kTableFormatVersion`，只靠 metaindex name？ | **是**（`M5-C3`） | §3.4/§4 §12 |
| Q3 | `WriteBatch` 沿用「一个 batch = 一条 WAL record」？ | **是**（`M5-C4`，阻断级） | §5/§4 §13 |
| Q4 | `≥3×` 只作同轮开关的块读计数门禁，且只限不存在的 key？ | **是**（`M5-C5`） | §3.8/§10 |
| Q5 | `missing != 0` 只施加在 LSM 腿？ | **是**（`M5-C6`） | §7.3/§10 |
| Q6 | `Options::bloom_bits` 默认 10（开），对照显式关闭并打印？ | **是**（`M5-C7`） | §5.6/§10 |
| Q7 | mmap/压缩不做，只登记理由？ | **是**（`M5-C8`） | §1.2/§12.4 |
| Q8 | 是否引入块缓存？ | **不引入**（§1.2 第 6 条） | §2.1 D6 |
| Q9 | `WriteBatch` 上限 = `count ≤ 1<<20` + `ByteSize()+16 ≤ 64 MiB`？ | **是** | §5.6/§4 §13.3 |
| Q10 | 新门禁腿是否挂进 `lsm_gate.sh` 并加正向标记？ | **是**（派工书补充要求） | §7.5/§10.3 |
| Q11 | M4.3 未收口时是否暂停 M5.1？ | **暂停**（M5.0 阻断条件） | §11 M5.0 |

---

## 13. 需用户拍板清单（标注「用户已授权按推荐执行」）

> **用户已授权：`M5-C1`~`M5-C8` 一律按推荐执行**；§12.6 的 Q1~Q11 全部按「本文推荐」执行。
> 下面逐条复述，便于用户在 `#0` 门后一次性确认。

1. **Q1/Q2/Q3**（filter 粒度、不升版本、一个 batch = 一条 record）：按 `M5-C2`/`M5-C3`/`M5-C4` 推荐执行。
2. **Q4/Q5**（`≥3×` 限作用域、`missing != 0` 只施加 LSM 腿）：按 `M5-C5`/`M5-C6` 推荐执行。
3. **Q6/Q7**（`bloom_bits` 默认 10、mmap/压缩不做）：按 `M5-C7`/`M5-C8` 推荐执行。
4. **Q8/Q9**（不做块缓存、`WriteBatch` 上限）：按 §1.2/§5.6/§4 §13.3 执行。
5. **Q10/Q11**（新门禁腿挂 `lsm_gate.sh` + 正向标记；M4.3 未收口则暂停 M5.1）：按 §7.5/§11 M5.0 执行。
6. **`docs/protocol.md` 追加 §12/§13**：按 §4 的 patch 文本落地；不改 §1~§11。
7. **`Options` 追加 `bloom_bits`、`DB` 追加 `Write(WriteBatch*)`**：按 §5.2/§5.6 落地；这是对 `M5:119`
   修改点清单的必要追加，登记进 `docs/m5-prerequisites.md` §2。
8. **`I47~I56` / `L30~L35` 编号**：按 §9.1 的映射表执行；`docs/m5-prerequisites.md` 附同一张表。
9. **`docs/m5-bench.md` 的负结果与「未验证」措辞**：按 §8.2/§12.5 执行；禁止把未实测数字写成结论。

---

## 14. 参考与引用

| 出处 | 用途 |
|---|---|
| `D:\lsm\lsm-kv-开发指令\M5-优化与微基准.md`（201 行） | M5 任务书；`#0` 的 8 条决策、验收判据、子里程碑 |
| `D:\lsm\mine\m4m5\notes.md` 的 M5 分节（243-459 行） | `M5-C1`~`M5-C8` 的事实基与推荐裁决 |
| `docs/m3-design.md` §3.4（656-671）、§3.5（672-697）、§3.7（731-764）、§7.4（1631-1641）、§9.1/§9.2（1876-1922）、§10.2 的 M3-B08（2004） | metaindex 预留、footer、读放大口径、M3 的 B 组固定行 |
| `docs/m4-design.md` §9.1（1926-1955）、§10.3（2139-2184）、§10.4（2185-2198）、§11 的 M4.3（2310-2335）、§12.4（2393-2416）、§15 R8/R10/R14（2581-2648） | I/L 顺延、`AMPL` 行只追加、反空绿、M4.3 的产物与 M5 的预留 |
| `docs/protocol.md` §9.4（185-204）、§10.3（248-280）、§10.6（317-328）、§10.7（329-345）、§11（379-469） | batch 编码、块外壳、metaindex、footer、M4 协议章节基线 |
| `docs/m2-design.md` 的 §9.3 G6 固定输出格式、§11.2/§11.3 的 fsync 与 kill -9 实测 | 基准输出格式、fsync 2.3~2.9 ms、kill -9 只证进程级一致性 |
| `docs/m3-evidence.md` §3（32-39）、§4（41-54） | 反空绿方法、M3 未闭合项（`verify_checksums` 的行为） |
| `docs/m3-prerequisites.md` §9 D9.6（716-729） | 正向标记缺口与防空绿 |
| `docs/m4-prerequisites.md` §7.3/§8.3 | M4.2 未做清单（M5 的前置事实） |
| `docs/roadmap.md` §0/§2/§3/§5（8-26、39-44、46-61、70-75） | 依赖纪律、阶段硬边界、工程约定、面试四问 |
| `src/sstable/format.h`（24-45） | `kBlockTypeFilter`、`kTableFormatVersion`、`kFooterSize` |
| `src/sstable/table.h`（42-110）、`src/sstable/table.cpp`（55-162、199-320） | `ReadStats`、`Table::Open`、metaindex 解析、`GetEntry` |
| `src/sstable/table_builder.h`/`.cpp`（26-160） | `TableBuilder::Add`/`FlushBlock`/`WriteBlock`/`Finish` |
| `src/version_set.h`（195-224）、`src/version_set.cpp`（897-953） | `TableCache` 的 `Open`/`Get`/`NewIterator` |
| `src/db.h`（17-46）、`src/db_impl.h`（98-115、253-265、344-407）、`src/db_impl.cpp`（137-412、431-548、1558） | `DB` 接口、`Pending`、`GetInternal`、`RunFlusher`、`Compaction::Run` |
| `src/memtable.h`（41-85）、`src/memtable.cpp`（144-182）、`src/skiplist.h`（28-42、192） | `WouldReject`/`Add` 的失败矩阵、节点大小上界 |
| `src/common.h`（285-343）、`src/wal.h`（20-31、48-72） | `Options`、`kMaxLogicalRecordSize`、`WALWriter::Append` |
| `scripts/lsm_gate.sh`（56-156）、`scripts/lsm_manifest_test.sh`（1-47）、`scripts/lsm_build.sh`（1-73） | 正向标记机制、M4 腿、构建/0 warning 口径 |
| `tests/test_harness.h`、`tests/sstable_counting_env.h`（68）、`tests/faulty_env.h`（21）、`tests/memenv.h` | 测试辅助与注入 seam |
| raft-kv `scripts/fsbench_commit_latency.cpp`、`scripts/bench_m5_ab.sh`、`docs/m5-bench.md` §3.6/§3.7/§3.9/§3.10/§3.11（本机只读） | 基准形状、同轮交替、比值口径、负结果写法 |
| `D:\lsm\mine\m4m5\m4-design.md`（2648 行） | M4 设计层权威（已冻结）；本文与它的 `§9.1`/`§10.3`/`§15` 对齐 |

---

## 15. 修订记录（相对指令原文的裁决与偏离）

### R1 —— 不变量与锁纪律编号全部顺延（`M5-C1`）

- **原指令**：`M5:84` 的「M3 的 `I21~I30`、M4 的 `I31~I42`」；`M5:96` 的「沿用 `L1~L26`，新增 `L27~L32`」。
- **实际**：M3 = `I21~I34` / `L13~L21`；M4 = `I35~I46` / `L22~L29`。
- **落地**：M5 = **`I47~I56` / `L30~L35`**；完整映射表见 §9.1；`docs/m5-prerequisites.md` 附同一张表。
- **依据**：`docs/m4-design.md:1926-1955` 的 M4-C1 顺延表；`docs/m4-design.md:1955` 末段明确把 `I47~I56 / L30~L35` 留给 M5。

### R2 —— filter 粒度：一个 SSTable 一个 filter block，内部按 2 KiB 桶（`M5-C2`）

- **原指令**：`M5:10` 的「每个 SSTable 一个 filter block」与 `M5:61` 的「块级 filter（每 2 KB）」并列。
- **落地**：一个 filter **块** + 内部多个 bitset；`kFilterBaseLg=11`；reader 用 `block_offset >> 11` 映射。
- **依据**：metaindex 只能存一个 handle；`M5:18` 的判据是数据块读次数；E2。

### R3 —— 不升 `kTableFormatVersion`，靠 metaindex name 演进（`M5-C3`）

- **原指令**：`M5:10` 的「随格式版本兼容演进」。
- **落地**：`version` 仍为 `1`；`name = "filter.leveldb.BuiltinBloomFilter2"`；footer 不变。
- **依据**：`format.h:45`/`:70`；`m3-design.md:656-671`；`protocol.md:323-324`。

### R4 —— WriteBatch 沿用「一个 batch = 一条 WAL record」（`M5-C4`，阻断级）

- **原指令**：`M5:64` 的「一次 WAL record vs 多条 record + 边界标记」。
- **落地**：必须一次 record；一个 `WriteBatch` 的全部 entry 必须在同一条 record 内；组提交可合并多个批。
- **依据**：`protocol.md:185-204`、`db_impl.cpp:219-245`（稳定 rev）、`M5:121`。

### R5 —— `≥3×` 的作用域与门禁形状（`M5-C5`）

- **原指令**：`M5:18` 的「例如 ≥3×」。
- **落地**：同轮开关、同一数据集、只限不存在的 key、只比**数据块读计数**、阈值 3.0；禁止外推。
- **依据**：`M5:199`、`RM:60`；§3.8。

### R6 —— `missing != 0` 只施加在 LSM 腿（`M5-C6`）

- **原指令**：`M5:14`/`M5:22` 的门禁 vs `M5:12`/`M5:148` 的无对账对照。
- **落地**：`engine=lsm` 才参与对账；其它 `verify=na`；失败注入只构造 LSM 格。
- **依据**：`M5:113` 的风险；§7.3。

### R7 —— `Options` 追加 `bloom_bits`，默认开（`M5-C7`）

- **原指令**：`M5:119` 的【必须改】**未列** `src/common.h`；`M5:10` 又要求默认 10 bits/key。
- **落地**：`int bloom_bits = 10;`（`0` = 关闭）；`DB::Open` 校验；对照显式关闭并打印；旧文件照常读。
- **依据**：`common.h:285-314`（稳定 rev）；`docs/m3-design.md:2117`（M3 已登记 `Options::bloom_bits` 属 M5）；
  `roadmap.md:22-26` 的依赖纪律（不用 `FilterPolicy*` 避免 `common → filter_policy` 环）。

### R8 —— mmap / 压缩不做（`M5-C8`）

- **原指令**：`M5:26` 非目标 vs `M5:66` 开放决策。
- **落地**：不做；只登记风险与理由；自检按「未引入」核对。
- **依据**：`roadmap.md:44`「非目标是硬边界」。

### R9 —— 新增「filter CRC 始终校验」（E3）

- **原指令未规定**：`M5:35` 只说降级照常读；`m3-design.md` 的 `verify_checksums` 只管数据块 CRC。
- **落地**：filter 块的读取强制 `verify_crc=true`；数据块仍按 `Options::verify_checksums`。
- **依据**：`I47` 的假阴性是丢数据级；filter 块每文件只读一次，成本可忽略。
- **风险**：见 §12.5 第 2 条；若 `#4` 反对，改为「`verify_checksums=false` 时禁用 filter」并重跑 `M5-A09`。

### R10 —— 新增 E1~E8 的补充决策

- **原指令未规定**：E1（否定判断的精确落点）、E2（`block_size` vs 2 KiB）、E3（filter CRC）、
  E4（错位注入如何证明测试有效）、E5（bench 与计数器解耦）、E6（`FakeClock` 缺失）、
  E7（`AMPL` 追加点未冻结）、E8（同轮交替的对象）。
- **落地**：按 E1~E8 的「唯一解释」执行；E 号不占 I/L 号空间（与 `docs/m4-design.md:2622-2628` R13 同形）。

### R11 —— `docs/protocol.md` 追加 §12/§13

- **原指令**：`M5:118` 要求追加 filter block 与 WriteBatch 编码章节，但**未给章节号**；`notes.md` 的 M5-8 当时
  实测「VM 侧尚未到 §10」，现在协议已到 §11（M4 定稿）。
- **落地**：追加 **§12（filter block）** 与 **§13（WriteBatch）**；patch 文本见 §4；只追加、不改 §1~§11。
- **依据**：`protocol.md:379-469` 的 §11 结尾；`M5:118`；`M3` 的 §4 patch 先例（`m3-design.md:777-782`）。

### R12 —— 基线重钉与「未落地」标注纪律

- **基线**：VM `HEAD = db5aa8f`；工作树在探测期间被 M4.3 改成脏（` M src/db_impl.{h,cpp}`）；**M4 未收口**
  （无 `m4-compaction` tag；`docs/m4-evidence.md`/`docs/amplification.md`/`scripts/lsm_compaction_stress.sh`/
  `scripts/lsm_level_stats.cpp` 均不存在）。
- **纪律**：本文的所有 `src/*` 行号取自 `git show db5aa8f:<path>`；M4.3 在途接口标「M5 开工前置复核」；
  M5.0（§11）给出可粘贴复核命令；未跑构建/测试的数字一律写「未验证（需 M5 #1 阶段实测）」。
- **依据**：`docs/m4-design.md:2629-2639` R14 的同形纪律。

### R13 —— 与 `notes.md` 的 `M5-R1`~`M5-R9` 的对应

| `notes.md` 风险 | 本文落点 |
|---|---|
| M5-R1 计数器并发 | `L34`、`Table::KeyMayMatch` 写线程局部 `ReadStats`、`MergeReadStats` 在 `mutex_` 下 |
| M5-R2 filter 读放大隐性成本 | `TableCache` 缓存 `Table`；filter 随 `Open` 一次性读入；`filter_blocks_read` 计数 |
| M5-R3 掉电 vs 进程级 | §10.2 的 B 组纪律；`kill -9` 只证进程级；`MemEnv` 证截断语义 |
| M5-R4 性能分母缺一半 | §3.8 同时打印 `without`/`with` 两个原始计数 |
| M5-R5 Bloom 体积代价 | §6.6 的 `space_filter_bytes`/`space_sst_data_bytes` |
| M5-R6 批大小上限 | §5.6/§4 §13.3；`count` 与 `ByteSize` 两道上限 |
| M5-R7 接口命名冲突 | §5.2：私有 `Write(ValueType,...)` 改名 `WriteEntry` |
| M5-R8 机器状态漂移 | §6.3/§6.4 的 `MACHINE`/`UNRELIABLE`/同轮交替 |
| M5-R9 `FakeClock` 不存在 | E6、§12.5 第 6 条 |

---

**（本文件结束。`#0` 硬闸门：设计定稿后必须暂停，等用户明确评审批准 `docs/m5-design.md` 后才进入 `#1`。）**

