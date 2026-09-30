# M5.1 + M5.2 实现证据（`docs/m5-evidence.md`）

> 产出阶段：**M5.1**（Bloom filter + filter block + metaindex + 读路径否定 + 计数器）与
> **M5.2**（WriteBatch 编码 + `DB::Write` + 批提交 + WAL 一次写 + 崩溃原子性）。
> 基线：`main` HEAD `fa7c328`（M1~M4 收口，tag `m1-memtable`/`m2-wal`/`m3-sstable`/`m4-compaction`）。
> 本文件**只登记证据与差异**；不改 `docs/m5-design.md`/`docs/m4-design.md`/`docs/m3-*.md`/`docs/m3-evidence.md`，
> `docs/protocol.md` 只做**纯追加**（§12/§13，证据见 §5）。
>
> 契约来源：`docs/m5-design.md` §11 的 M5.1/M5.2、§2 的裁决（D1~D8 / E1~E8）、§3（filter 位级格式与读路径接入）、
> §4 §12/§13（协议追加文本）、§5（WriteBatch 与提交路径）、§9（`I47~I56` / `L30~L35`）、§10（测试矩阵）；
> `docs/protocol.md` §9.4（M2 冻结的「一个 batch = 一条 WAL record」）、§12（M5.1 追加）、§13（M5.2 追加）。

---

## §0 M5.0 复核的复核（对 `docs/m5-prerequisites.md` 的再核对）

`docs/m5-prerequisites.md` 由前一执行者在 M5.0 产出（**未提交**）。本阶段逐条复核其结论：

| # | M5.0 的结论 | 本阶段实测 | 结论是否成立 |
|---|---|---|---|
| 1 | HEAD == `fa7c328`，四个 tag 全在 ⇒ M5.1 可开工 | `git rev-parse HEAD` == `fa7c328`；`git tag` 含四个 tag | ✅ 成立 |
| 2 | M5.0 时工作树干净 | M5.0 之后工作树被 M5.1/M5.2 的改动占据（见 §8），与 M5.0 时点不矛盾 | ✅ 成立 |
| 3 | `Options` 无 `bloom_bits` / `DB` 无 `Write(WriteBatch*)` | M5.1 落 `Options::bloom_bits`（`src/common.h`）；M5.2 落 `DB::Write(WriteOptions, WriteBatch*)`（`src/db.h`） | ✅ 成立，已落地 |
| 4 | `FakeClock` 不存在 ⇒ A 组按 E6 用 `MemEnv`/`CountingEnv`/`FaultyEnv` | M5.1/M5.2 的 A 组**全部**建在 `MemEnv` 上；`FakeClock` 仍未引入（`grep -rn FakeClock tests src` = 0） | ✅ 成立 |
| 5 | `Table::Open`/`Add`/`Finish` 签名与设计草案不一致 ⇒ 以落地为准 | M5.1 只**追加**默认参数 `ReadStats* open_stats = nullptr`；`Add`/`Finish` 仍返回 `Status`（粘性 `status_`） | ✅ 成立 |
| 6 | `FormatAmplLine` 的前缀列逐字一致，`space_filter_bytes` 待 M5.3 行尾追加 | M5.1/M5.2 **未动** `FormatAmplLine`（属于 M5.3）；`AMPL` 既有列语义不变（§6 D-6） | ✅ 成立 |
| 7 | `scripts/lsm_gate.sh` 无 `--require-m5`、M5 腿不存在 ⇒ M5.1 补 | M5.1 补 `run_gate_m5_marked` + `--require-m5` + M5-B11 腿；M5.2 补 M5-B12/M5-B01 两条腿（§3） | ✅ 成立，已落地 |
| 8 | M5 产物（`docs/m5-bench.md` 等）不存在 | 仍不存在（属 M5.3）；`scripts/lsm_batch_crash_test.sh` 等由 M5.2 新建 | ✅ 成立 |

**M5.0 判据 3 的一处措辞收紧**：`docs/m5-prerequisites.md` §0.2 ⑥ 逐字写 `Table::Open`（`table.h:77`），
落地实际在 `table.h:94` 一带（M5.1 追加默认参数后行号后移）。**行号以落地为准**（该文件 §2 D-1 已预告）；
语义无差异。

---

## §1 M5.1 判据逐条（`docs/m5-design.md` §11 M5.1 的判据列）

### 1.1 设计判据 ↔ 实测

| # | 判据（§11 M5.1） | 落地位置 | 证据（用例 / 计数行） | 状态 |
|---|---|---|---|---|
| 1 | **每个 SSTable 恰好一个 filter block**，块内按 `kFilterBaseLg=11`（2 KiB）切成多个独立 bitset | `TableBuilder::Finish` 写一次 `kBlockTypeFilter`；`FilterBlockBuilder::StartBlock(block_offset)` 用 `block_offset >> 11` 分桶 | `Filter.MetaIndexRegistrationAndFormatVersion` 断言 `b.filter_bytes() > 0` 且 `b.filter_num_filters() > 0`；`Filter.BloomEncodingRoundTrip` 直调 builder 逐桶校验 | ✅ |
| 2 | filter 写进 **metaindex**、名字空间 `filter.leveldb.BuiltinBloomFilter2` | `src/sstable/format.h` 的 `kBuiltinBloomFilterName`；`TableBuilder::Finish` 步骤 ③ 注册；`Table::ParseMetaIndexBlock` 用 `IsBuiltinBloomFilterName` 识别 | `Filter.MetaIndexRegistrationAndFormatVersion` ①：`t->filter_state() == kOk`、`unknown_metaindex_entries() == 0`；⑤：其它 name 仍 `unknown_metaindex_entries() == 1` | ✅ |
| 3 | **不升** `kTableFormatVersion`、footer 布局不变 | `format.h` 的 `kTableFormatVersion == 1` 未改；`TableBuilder` 只多写一个块 | `Filter.MetaIndexRegistrationAndFormatVersion` ① 逐字节断言 footer 的 `version == 1` 且 `kTableFormatVersion == 1` | ✅ |
| 4 | `Options::bloom_bits = 10`（默认开）；`0` = 关闭；非法值在 `DB::Open` 第一步拒绝 | `src/common.h`；`src/db.cpp` 的 `DB::Open` 校验 `[0,64]` | `Filter.MetaIndexRegistrationAndFormatVersion` ②（`bloom_bits=0` ⇒ 不写块、`filter_state()==kAbsent`）；③（空表不写块） | ✅ |
| 5 | 旧文件（无 filter）**照常读**（降级），且降级不静默 | `Table::LoadFilterBlock` 把 `kAbsent`/`kCorrupt` 都按「可能存在」；`Table::KeyMayMatch` 计 `filter_unavailable` | `Filter.OldFileWithoutFilterDegrades` ①②③：32/100 个 key 全可读、`filter_unavailable > 0`、`filter_checked == 0` | ✅ |
| 6 | 读路径**只**用 filter 做否定判断（肯定结论不得跳过读取或校验） | `Table::GetEntry` 的 step ②.5；`Table::NewIterator` 完全不调用 filter | `Filter.NegationOnlySkipsDataBlocks`（否定 ⇒ 0 数据块读；肯定 ⇒ ≥1 且 CRC/type 校验仍执行；不可用 ⇒ ≥1） | ✅ |
| 7 | filter 块 CRC **始终**校验（E3），不受 `verify_checksums` 影响 | `Table::ReadBlockImpl(..., verify_crc)`；`LoadFilterBlock` 传 `true` | `Filter.FilterCrcAlwaysVerified` | ✅ |
| 8 | filter 损坏/错位**必须被检出**并降级（E4），读结果仍正确 | `LoadFilterBlock` 的 4 类校验 + `FilterBlockReader` 的 8 条结构校验 | `Filter.MisalignedOffsetInjectionDetected`、`Filter.CorruptFilterDegradesButReadsCorrect`、`Filter.FilterDamageScanNoSilentWrongValue` | ✅ |
| 9 | 计数器按 `L34` 用线程局部 delta 汇总，禁止无锁共享自增 | `ReadStats` 追加 8 列；`Table::KeyMayMatch(…, ReadStats*)` 写调用方传入的 delta；`MergeReadStats` 持 `mutex_` 落库 | `Filter.MultiThreadedGetNoRace`（A18，TSan 腿）；`MergeReadStats` 逐列累加（8 列全在） | ✅ |
| 10 | 块读下降（同轮 on/off 对照，`≥3×`，只限**不存在**的 key） | `Options::bloom_bits` 透传到 `TableBuilder` | `Filter.BlockReadReductionAtLeastThreeTimes`：`M5_FILTER_BLOCK_READS_WITHOUT 2000` / `WITH 18` / `RATIO 111.111111` | ✅ |

### 1.2 M5.1 的 A 组（`M5-A01~A10 + A18`）映射

| 编号 | 用例 | 备注 |
|---|---|---|
| M5-A01 | `Filter.BloomEncodingRoundTrip` | 空 filter / 单 key / 多 key / `n>8192` / `k` 上下界 / bits 非 8 倍数回填 |
| M5-A02 | `Filter.FalsePositiveRateMeasured` | `N=100000` 插入 + `M=100000` 未插入 ⇒ 实测 `M5_FILTER_FPR_PPM 8310`（0.831%）≤ 2.0% 判据；并打印两种**结构性分布**的对照（`M5_FILTER_FPR_STRUCTURED_PPM` / `..._BUCKET_PPM`）作为负结果登记 |
| M5-A03 | `Filter.NoFalseNegativeOnSSTable` | tombstone / 同 user key 多版本 / 最小-最大-相邻 key 全覆盖，零假阴性 |
| M5-A04 | `Filter.MisalignedOffsetInjectionDetected` | 错位注入（重算 CRC 的 offset 错位 / 不重算 CRC 的 bit flip）**必须被检出** |
| M5-A05 | `Filter.NegationOnlySkipsDataBlocks` | 肯定/否定/不可用三态的块读计数断言 |
| M5-A06 | `Filter.MetaIndexRegistrationAndFormatVersion` | 注册/缺失/空表/同名重复 `kCorrupt`/未知 name 只计数 |
| M5-A07 | `Filter.OldFileWithoutFilterDegrades` | 旧库降级 + 反向（`bloom_bits=0` 的 reader 仍解析旧 filter，`M5-C7(c)`） |
| M5-A08 | `Filter.CorruptFilterDegradesButReadsCorrect` | handle 越界 / 过短 / type 错 / CRC 坏 / payload 结构坏 |
| M5-A09 | `Filter.FilterCrcAlwaysVerified` | `verify_checksums=false` 时 filter CRC 仍校验 |
| M5-A10 | `Filter.BlockReadReductionAtLeastThreeTimes` | 同轮、同数据集、同不存在 key 集合的 on/off 对照 |
| M5-A18 | `Filter.MultiThreadedGetNoRace` | 多线程点查同一 `TableCache` 缓存的 `Table`（TSan 腿） |
| （附）| `Filter.FilterDamageScanNoSilentWrongValue` | 单字节翻转扫描（`M5_FILTER_DAMAGE_CASES 2471`、`..._FILTER_DEGRADED 85`、静默假阴性 0） |

**`Bloom.*` suite 为空集合**（策略用例统一放在 `Filter.*`），`SSTable.*` 在本仓库不存在（M3.1 的格式层 suite 是
`Block.*` / `Footer.*`）。设计 §11 的证据命令写 `--gtest_filter='Filter.*:Bloom.*:Table.*:SSTable.*'`；
`scripts/lsm_m5_unit_test.sh` 实际用 `Filter.*:Table.*:Block.*:Footer.*` 并**额外要求 `Filter.* >= 12`**
（脚本头已逐字注明）。这是**收紧**（既跑过滤层，也跑 M3 格式层回归），不是放宽。

---

## §2 M5.2 判据逐条（`docs/m5-design.md` §11 M5.2 的判据列）

### 2.1 设计判据 ↔ 实测

| # | 判据（§11 M5.2 / §5） | 落地位置 | 证据 | 状态 |
|---|---|---|---|---|
| 1 | `src/write_batch.{h,cpp}`；`Data()` 与 §13.1 的 `batch_payload` 逐字同构 | `src/write_batch.h` / `src/write_batch.cpp` | `WriteBatch.EncodingRoundTrip`：12B 头逐字节 + 4 条混合 entry 往返 + 9000 条大 count | ✅ |
| 2 | `DB::Write(WriteOptions, WriteBatch*)` 在两个实现上都落地 | `src/db.h`（纯虚）、`src/db_impl.{h,cpp}`、`src/db.cpp`（`MemoryDBImpl`） | 两个实现都构成，无抽象类残留（否则编译失败） | ✅ |
| 3 | **一个 batch = 一条 WAL record**（§13.2 / I53，不得拆 record） | `EncodeGroup` 把整组 entry 拼进**一条** payload；`SubmitPending` 只入队一次 | `Batch.RecoverySequenceAndOneRecordPerBatch`：3 次 `DB::Write` ⇒ 恢复侧 `records_replayed == 3`、`entries_replayed == 10`（**恢复器复核**，不是自己数自己） | ✅ |
| 4 | 批内 sequence 连续、`last_sequence` 覆盖批内最大（I52） | `RunFlusher` 阶段 B 的 `offset` 累加；`SubmitPending` 不做分配 | `Batch.RecoverySequenceAndOneRecordPerBatch`（批 3/5/2 后 `last_sequence()` 逐步 3→8→10；恢复后一致） | ✅ |
| 5 | 整批原子可见（I51）：无「批内部分条目可见」 | 预校验（`Validate`）→ 容量预检（`footprint`）→ WAL 先于内存 → 恢复对象未发布 | `Batch.AtomicVisibilityUnderAppendAndSyncFailure`：`M5_BATCH_PARTIAL_VISIBLE 0`、`M5_BATCH_HALF_VISIBLE 0`、`M5_BATCH_CRASH_HALF_VISIBLE 0`（8 个掉电种子 × 6 批） | ✅ |
| 6 | `sync=true` 的 durable-before-ack 在批粒度继续成立（I54） | `RunFlusher` 阶段 C 的 `need_sync` = 组内 OR；只有真 fsync 才推进 `durable_seq_` | `Batch.GroupCommitOneFsyncForConcurrentBatches`：32 个并发批（128 entry）⇒ `M5_BATCH_GROUP_FSYNCS 1`、`durable_seq() == 128`、`last_sequence() == 128` | ✅ |
| 7 | 无丢唤醒（承接 M2 的 L9/L10；`M5-A14` 复用 A20 的形状） | `SubmitPending` 的 `while (!w->done)` + `flusher_active_` 交接（**协议一行未改**） | 同一用例：`M5_BATCH_LOST_WAKEUPS 0`（32/32 写者全部返回） | ✅ |
| 8 | WAL 批记录截断 ⇒ 该批**完全不可见**，不得产生半批（I53） | 物理层一个 record = 一个 CRC（M2 未改） | `Batch.WalRecordTruncationNeverExposesHalfBatch`：`M5_BATCH_TRUNCATE_CASES 8`、`M5_BATCH_HALF_VISIBLE 0`、完整记录 ⇒ 6/6 可见 | ✅ |
| 9 | 容量上界：`ApproximateMemoryUsage` 增量 ≤ `Σ(entry_bytes+128)`（§5.4 第 4 步） | `RunFlusher` 用同源 `footprint` 调 `WouldReject` | `Batch.CapacityBoundUpperBoundMeasured`：`DELTA 86984` ≤ `BOUND 87484`（500 条随机尺寸 entry） | ✅ |
| 10 | 容量边界（整批 footprint 刚超 `write_buffer_size`）⇒ 整批可见，不得 `kFrozen` | 既有 M3 冻结逻辑 + 按 entry 折算的 `footprint` | 同一用例 ④：6 批 × 16 条 / `write_buffer_size=8192`，零 `kFrozen`、半批 0 | ✅ |
| 11 | 输入预校验在**入队/写 WAL 之前**：`count==0` / 超限 / 畸形 ⇒ `kInvalidArgument`/`kCorruption`，且不写 WAL、不碰内存 | `WriteBatch::Validate`（§5.4 第 1 步）+ `DB::Write` | `WriteBatch.EncodingRoundTrip` ⑤、`WriteBatch.OverLimitRejectedBeforeAnyWrite`（WAL 文件 0 字节）、`WriteBatch.OwnershipReuseAndMalformedRep` ④ | ✅ |
| 12 | 所有权/复用（L31）：`DB::Write` 返回后可复用/`Clear`；不保留引用 | `src/write_batch.h` 头注释 + `DB::Write` 拷贝 `entries` | `WriteBatch.OwnershipReuseAndMalformedRep` ①②③（改回同一对象不影响已提交数据；`SetSequence` 被忽略） | ✅ |
| 13 | `kill -9` 落在批写入中途：已 ack 的批不得丢、不得半批（M5-B01） | `scripts/batch_crash_writer.cpp` + `batch_crash_recover.cpp` + `lsm_batch_crash_test.sh` | §3.2 的 `BATCH_KILL9_MISSING 0` / `BATCH_HALF_VISIBLE 0` / `[BATCH_CRASH_OK]` | ✅ |
| 14 | `docs/protocol.md` 追加 §13（只追加） | §5 | 前缀 sha256 不变 | ✅ |

### 2.2 M5.2 的 A 组（`M5-A11~A17`）映射

| 编号 | 用例 | 备注 |
|---|---|---|
| M5-A11 | `WriteBatch.EncodingRoundTrip` + `WriteBatch.OverLimitRejectedBeforeAnyWrite` | 空批拒绝 / 单条 / 多条 / 混合 / 空 value / 9000 字节大 value / `count` 上下界 / 超限不写 WAL |
| M5-A12 | `Batch.AtomicVisibilityUnderAppendAndSyncFailure` | Append(ENOSPC) 失败 ⇒ 不可见且崩溃重开仍不可见；fsync 失败 ⇒ 不可见；掉电 8 种子半批 0；容量边界整批可见 |
| M5-A13 | `Batch.CapacityBoundUpperBoundMeasured` | 实测增量 ≤ 上界 |
| M5-A14 | `Batch.GroupCommitOneFsyncForConcurrentBatches` | 确定性屏障：32 并发批 ⇒ 1 次 fsync、无丢唤醒、水位覆盖整批 |
| M5-A15 | `Batch.WalRecordTruncationNeverExposesHalfBatch` | 8 个截断点（含 0 / 1 / 6 / 7 / 8 / 半长 / 末字节 / 完整） |
| M5-A16 | `Batch.RecoverySequenceAndOneRecordPerBatch` | 批内连续 + `last_sequence` 覆盖 + `records_replayed == 批数`（I53 的直接证据） |
| M5-A17 | `WriteBatch.OwnershipReuseAndMalformedRep` | 复用 / 无引用残留 / `SetSequence` 被忽略 / 6 类畸形 rep ⇒ `kCorruption` |

**反向自检（§10.1 末段）**：`Batch.AtomicVisibilityUnderAppendAndSyncFailure` ⓪ 先证「同样的批在**无故障**时
整批可见（8/8）」，因此后面的「0 条可见」不可能是恒真；`Batch.WalRecordTruncationNeverExposesHalfBatch`
要求 `full_visible_cases == 1`（完整 ⇒ 6/6 可见），否则该用例判空绿。

---

## §3 三构建与门禁原始计数行

### 3.1 三构建

| 构建 | 命令 | 原始计数行 |
|---|---|---|
| Release（干净重建） | `bash scripts/lsm_build.sh` | `[CHECK] warning 计数 = 0（要求 0）`；`[==========] 205 tests from 56 test suites ran. (75648 ms total)`；`[  PASSED  ] 205 tests.` |
| ASan | `cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8` | `asan_build_rc=0  warnings=0`；`[==========] 20 tests from 3 test suites ran. (192416 ms total)`；`[  PASSED  ] 20 tests.`；`AddressSanitizer 报告数 = 0` |
| TSan | `cmake -S . -B build-tsan -DENABLE_TSAN=ON … && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter='Filter.MultiThreadedGetNoRace:WriteBatch.*:Batch.*'` | `tsan_build_rc=0  warnings=0`；`[==========] 9 tests from 3 test suites ran. (24078 ms total)`；`[  PASSED  ] 9 tests.`；`ThreadSanitizer 报告数 = 0` |
| TSan（单线程重负载，**未完成**，见 §7 第 6 条） | `--gtest_filter='Filter.BlockReadReductionAtLeastThreeTimes'`（`timeout 2400`） | 启动后 **25 分钟仍在 `[ RUN ]` 状态**（无 `[ OK ]`、无 race 报告、无失败）；`timeout 2400` 到点被强杀。**不计入「通过」**，也不计为「失败」——记为**未验证** |

ASan 的过滤器与设计 §11 的证据命令一致（`Filter.*:Bloom.*`），并加上 M5.2 的 `WriteBatch.*:Batch.*`
（20 条 = filter 12 + batch 8）。

TSan 这里只跑**并发面**：`Filter.MultiThreadedGetNoRace`（A18 多线程点查同一 `TableCache`）
+ `WriteBatch.*`/`Batch.*` 全部（含 A14 的 32 线程并发组提交）。**单线程** filter 用例在 Release 与 ASan
下覆盖——TSan 的用途是数据竞争，单线程用例加 TSan 只增加时间不增加信息量，且
`Filter.BlockReadReductionAtLeastThreeTimes`（20000 key × 2 库的写入 + 4000 次点查）在 TSan 下
**单条超过 17 分钟仍未结束**（实测 CPU 100% 持续占用、非死锁）。该条的 TSan 结果单列上表，
**偏离登记见 §7 第 6 条**。

### 3.2 门禁（`scripts/lsm_gate.sh`）

```
$ bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3 --require-m5
==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  崩溃对账（kill -9 x 100，sync 模式）
PASS  逐字节截断扫描（B03）
PASS  中间损坏拒绝启动（B04）
PASS  M3-B01 flush 崩溃对账（kill -9 x 100）
PASS  M3-B03 落盘重启（records_replayed == 0）
PASS  M3-B04 SSTable 损坏扫描（零静默错值）
PASS  M3-B05 句柄计数不增长
PASS  M4-B11 MANIFEST/VersionEdit A 组 + 零依赖
PASS  M4-B03 两种 pick 策略对照
PASS  M4-B05 句柄上限（fd 不增长）
PASS  M4-B06 读放大改善（p50<=3 max<=12）
PASS  M4-B07 前台 P99 与单轮 P50 量级分离
PASS  M4-B08 存活 Version 数有界（计数存在）
PASS  M4-B09 MANIFEST 体积/重建计数
PASS  M4-B10 块缓存 NOT_APPLICABLE
PASS  M4-B01 compaction 中途 kill -9 对账
PASS  M4-B02 四注入点 raise(SIGKILL) 对账
PASS  M5-B11 filter 单元 + 块读下降 + 零假阴性 + 依赖纪律
PASS  M5-B12 WriteBatch 单元 + 整批原子 + WAL 一次写 + 依赖纪律
PASS  M5-B01 批崩溃对账（kill -9 批写入中途）
[OK] 全部门禁通过
```

**（`--no-asan` 下 ASan 腿不跑；`--with-tsan` 未开，见 §7 第 6 条。既有 18 腿每条都 PASS。）**

关键原始计数行（逐字摘自该次门禁的 `gate.log`）：

```
TOTAL_ROUNDS 100 ROUNDS_OK 100 ACKED_TOTAL 1894 MISSING_TOTAL 0 MISMATCH_TOTAL 0
SST_FILES_TOTAL 100
RECORDS_REPLAYED_TOTAL 1708
RECORDS_REPLAYED 0 RESTART_KEYS_OK 2000/2000 SST_FILES_REGISTERED 2
TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0 RECORD_BYTES 40
MIDDLE_OPEN_CORRUPTION 1
FD_GROWTH 1 FD_BASELINE 8 FLUSHES_COMPLETED 452
COMPACTION_ROUNDS_TOTAL 61 SST_FILES_TOTAL 40 MISSING_TOTAL 0 MISMATCH_TOTAL 0 ROUNDS_OK 30
INJECT_POINTS_OK 4 MISSING_TOTAL 0 REF_MISSING_TOTAL 0 OPEN_CORRUPTION_TOTAL 0 ORPHAN_REMOVED_TOTAL 3
M5_TESTS_RAN 32  M5_TESTS_FAILED 0  M5_FILTER_RAN 12  M5_FILTER_FALSE_NEGATIVE 0  M5_FILTER_SILENT_FALSE_NEGATIVE 0
  M5_FILTER_BLOCK_READS_WITHOUT 2000  M5_FILTER_BLOCK_READS_WITH 18  M5_FILTER_BLOCK_READ_RATIO 111.111111
  M5_FILTER_BLOCKS_SKIPPED 1982  M5_FILTER_DAMAGE_CASES 2471  M5_FILTER_DAMAGE_FILTER_DEGRADED 85
  M5_FILTER_FPR_PPM 8310  LSM_SSTABLE_FORBIDDEN 0  [FILTER_OK]  [FILTER_DAMAGE_OK]
M5_BATCH_TESTS_RAN 8  M5_BATCH_TESTS_FAILED 0  M5_BATCH_WRITEBATCH_RAN 3  M5_BATCH_ROUNDTRIP_ENTRIES 9004
  M5_BATCH_PARTIAL_VISIBLE 0  M5_BATCH_HALF_VISIBLE 0  M5_BATCH_CRASH_HALF_VISIBLE 0  M5_BATCH_CRASH_CASES 8
  M5_BATCH_LOST_WAKEUPS 0  M5_BATCH_GROUP_FSYNCS 1  M5_BATCH_CONCURRENT_WRITERS 32  M5_BATCH_TRUNCATE_CASES 8
  M5_BATCH_RECOVERY_RECORDS 3  M5_BATCH_RECOVERY_ENTRIES 10  M5_BATCH_ONE_RECORD_PER_BATCH 1
  M5_BATCH_SEQ_CONTIGUOUS 1  M5_BATCH_LAST_SEQ_COVERS 1  LSM_BATCH_FORBIDDEN 0  [BATCH_OK]
BATCH_KILL9_ROUNDS 100 ROUNDS_OK 100 BATCH_ACKED 1086 BATCHES_SEEN 1139 BATCH_KILL9_MISSING 0
  BATCH_MISMATCH 0 BATCH_HALF_VISIBLE 0
[BATCH_CRASH_OK] rounds=100 batch_size=16 sync=1 missing=0 mismatch=0 half=0
```

**L18 探针（`I17`/`L7` 持锁零 IO）**：`./build/bin/lsm_tests --gtest_filter='Flush.NoIoWhileHoldingDbMutex'`
⇒ `[==========] 1 test from 1 test suite ran. (35 ms total)` / `[  PASSED  ] 1 test.` ✅

**M2 高危区回归**：`TOTAL_ROUNDS 100 ROUNDS_OK 100 … MISSING_TOTAL 0 MISMATCH_TOTAL 0`（组提交语义未回退）；
`M5_BATCH_GROUP_FSYNCS 1`（32 并发批仍只 1 次 fsync，D3 的组批上限未被批路径破坏）。

**既有 18 条腿一行未动**：`git diff scripts/lsm_gate.sh` 的删除行只有 2 行，且都不是腿的定义
（1 行是启动横幅 `== lsm_gate: rounds=… ==`，1 行是 `[PARTIAL]` 的提示文案）；`grep -c '^run_gate'`
从 21（3 定义 + 18 腿）变成 25（4 定义 + 21 腿 = 18 既有 + 3 条 M5 腿）。
新增的 3 条腿：M5-B11（M5.1）、M5-B12（M5.2 单元）、M5-B01（M5.2 崩溃）。

---

## §4 反空绿检查（`docs/m5-design.md` §10.3 逐条）

| 检查 | 方法 | 实测 | 期望 | 状态 |
|---|---|---|---|---|
| 零断言 TEST 块 | `awk` 逐 `TEST(...){...}` 块扫描 `EXPECT_/ASSERT_` | `0` | 0 | ✅ |
| 跳过/禁用/永真 | `grep -rnE 'DISABLED_\|GTEST_SKIP\|\|\| true' tests/`（**排除注释行**后） | `0` | 0 | ✅ |
| 既有断言是否被放宽 | `git diff -U0 -- tests/` 中**被删除行**里的 `EXPECT_/ASSERT_` 计数 | `0` | 0 | ✅ |
| filter 测试是否真的零假阴性 | `M5_FILTER_FALSE_NEGATIVE` 必须是打印出来的计数 | `0`（打印） | 0 | ✅ |
| filter 是否真的省了块读 | `data_blocks_skipped_by_filter > 0` | `M5_FILTER_BLOCKS_SKIPPED 1982` | > 0 | ✅ |
| 块读下降是否真的同轮 | 同一进程、同一数据集、同一查询集合 | `WITHOUT 2000` / `WITH 18`（同轮） | 同轮 | ✅ |
| 批是否真的整批原子 | 打印出来的违例计数 | `M5_BATCH_PARTIAL_VISIBLE 0` / `M5_BATCH_HALF_VISIBLE 0` / `M5_BATCH_CRASH_HALF_VISIBLE 0` | 0 | ✅ |
| 「一个 batch = 一条 WAL record」是否只是自证 | 用**恢复器**的 `records_replayed` 复核（不是自己数自己） | `3 次 Write ⇒ records_replayed 3 / entries_replayed 10` | 相等 | ✅ |
| 门禁是否只看退出码 | `run_gate_m5_marked` 多条标记 AND | 3 条 M5 腿各 7~13 条标记 | AND | ✅ |
| `--require-m5` 的 SKIP 语义 | 缺脚本 ⇒ `FAIL`；默认 ⇒ `SKIP` + `[PARTIAL]` | 脚本齐全时全 `PASS`，末行 `[OK]` | 必须 | ✅ |
| 用例总数 | `grep -h 'TEST(' tests/*.cpp \| wc -l` | `205`（基线 185 + M5.1 12 + M5.2 8） | ≥ 185 | ✅ |

---

## §5 `docs/protocol.md` 的「只追加」证据

| 项 | 值 |
|---|---|
| 追加前（= `HEAD:docs/protocol.md`）行数 | `469` |
| 追加前整文件 sha256 | `60282bb3ec3e6e4fb577f2bf7fc57943703144d554bd4c0544ad5b03cafc125f` |
| M5.1 追加 §12 后的行数 / sha256 | `555` / `49a65d5c87d56e252ae8c83fce2962367637160ff605c7bcc25c4eec5607b0f9` |
| M5.2 追加 §13 后的行数 / sha256 | `597` / `a54d59873ef6ed53140ba9bf70f5d75bd2ba84b8c2d183896c4ccd48607c4967` |
| `head -n 555 docs/protocol.md \| sha256sum` | `49a65d5c…5607b0f9` == 追加 §13 之前的整文件 sha256 ✅ |
| `head -n 469 docs/protocol.md \| sha256sum` | `60282bb3…cafc125f` == 追加 §12 之前的整文件 sha256 ✅ |
| `diff -q <追加前副本> <新文件前 N 行>` | 无差异（`PURE_APPEND_OK`） |
| 新增章节 | `## 12. filter block 编码（M5 定稿）`（`471`）、`## 13. WriteBatch 编码（M5 定稿）`（`557`） |

§12 与 §13 的正文逐字取自 `docs/m5-design.md` §4.1 / §4.2 给出的 patch 文本（含 12.1~12.5、13.1~13.3）。

---

## §6 差异登记（实现 ↔ 设计；供评审逐条裁决）

| # | 设计写法 | 落地实现 | 处置 / 理由 |
|---|---|---|---|
| D-1 | §7.5 的标记名：`M5_FILTER_TESTS_RAN` / `M5_FILTER_BLOCK_READ_RATIO` / `M5_BATCH_TESTS_RAN` / `[BATCH_OK]` | M5.1 用 `M5_TESTS_RAN` / `M5_FILTER_RAN` / `M5_FILTER_BLOCK_READ_RATIO` / `[FILTER_OK]`；M5.2 用 `M5_BATCH_TESTS_RAN` / `M5_BATCH_TESTS_FAILED` / `[BATCH_OK]` | 标记名以落地为准；**语义等价且更严**（M5.1 额外要求 `Filter.* >= 12`、单字节翻转扫描的降级计数）。已在两条脚本头逐字登记 |
| D-2 | §7.5 把 A 组与 B 组的标记都挂在 `scripts/lsm_m5_unit_test.sh` 一个脚本上 | 落地拆成两个：`lsm_m5_unit_test.sh`（filter，M5.1）与 `lsm_batch_unit_test.sh`（batch，M5.2）；崩溃侧 `lsm_batch_crash_test.sh` | **理由：可提交性**——用户要求"尝试拆成两个提交"，若 M5.1 的脚本要求 M5.2 的标记，M5.1 单独提交时该腿必红。拆分后两段各自可独立跑绿 |
| D-3 | §11 M5.2「必改」列含 `src/wal.{h,cpp}`（如需要，仅追加批承载辅助） | **未改** `src/wal.*` | 一个 record 的承载方式已经足够（`EncodeGroup` 拼一条 payload）；`§13.2` 要求"不改编码"，改 wal 反而增加风险 |
| D-4 | §5.1 的 `WriteBatch` 草案没有"原始字节构造"入口 | 追加 `explicit WriteBatch(const Slice& raw_data)`（注释标注**仅测试/诊断**） | M5-A17 要求「`Iterate` 对畸形 rep 返回 `kCorruption`」，需要一个能承载畸形输入的对象；生产的构造路径仍只有 `Put`/`Delete`/`Clear` |
| D-5 | §5.1 的 `Validate` 只是"Validate 之类的方式" | 落地为 `Status Validate(uint32_t*, size_t*, uint64_t*) const`（同时给出 `entry_bytes` / `user_bytes` 统计） | 让「预校验」与「提交时的统计口径」共用同一次解析，避免两处漂移（§5.4 第 1 步 + M4.3 的统计口径） |
| D-6 | §5.3 逐字写 `entry_bytes_ += Σ p->user_bytes + 16*total_count` | 落地为 `p->user_bytes + 16ull * p->entry_count` 后求和 | 与设计等价（`Σ16*count` 可分配）；单条写代入后与 M4.3 的落地公式 `key+value+16` **逐字相同** ⇒ 既有 `AMPL` 数字不变 |
| D-7 | §13.3 把批大小上限的位置写成 `src/db_impl.cpp:160`（单条写的既有校验处） | 上限校验落在 `WriteBatch::Validate`（`src/write_batch.cpp`），由 `DB::Write` 在入队前调用 | `docs/protocol.md` §13 保持设计给的 patch 文本**逐字**（协议描述的是契约："在编码/入队之前校验"，落地为真）；实现位置登记在此。单一真相源 ⇒ 内存模式与持久模式口径一致 |
| D-8 | §11 M5.1「必改」把「fix `data_blocks_read` 从不递增」列为本阶段工作 | 前一执行者在 `Table::ReadBlockImpl` 里为 `expected == kBlockTypeData` 递增 `data_blocks_read` | 只影响诊断计数，不改任何读结果/校验语义；M4 的 `docs/amplification.md` 里 `read_data_blocks_read=0` 是修复前的原始证据。**这是既有列语义的一处修正**，需用户知悉 |
| D-9 | §3.6 的 `ReadStats` 追加 7 列 | 落地追加 **8** 列（多 `filter_corrupt`） | M5-A08 要求「计数 `filter_corrupt`」而 7 列里没有该名字；只追加、不改既有 8 列 |
| D-10 | §5.3「`front_samples_us_`：一次 `DB::Write` 调用一个样本」 | **未改**：仍是「一个**组**一个样本」（M4.3 的落地形态） | 该项影响的是 M5.3 的数据表口径（`FRONT` 行的含义），M5.1/M5.2 不改以免动既有 `AMPL`/`FRONT` 断言；**留给 M5.3 处理并登记** |

---

## §7 未做 / 未验证清单（诚实登记）

1. **未提交任何 git 变更**（按派工书纪律：提交由用户执行；本阶段只做只读 git 查询）。
2. **未跑 M5.3 的任何内容**：`bench/bench_lsm.cpp`、`scripts/bench_lsm.sh`、`bench_lsm_selftest.sh`、
   `lsm_filter_damage_test.sh`、`docs/m5-bench.md`、`AMPL` 行尾追加 `space_filter_bytes` —— 全部属 M5.3。
3. **`Options::bloom_bits` 的"关闭对照"只做到单元/块读计数层**（`M5-A10` 的 2000 vs 18 数据块读），
   **没有**做 M5.3 的吞吐/延迟对照；因此**不主张**任何"提升 N 倍"的性能结论（`M5:199`）。
4. **假阳性率只测了三种分布**（随机、`even_keys` 结构化、`bucket` 型）；
   `M5-A02` 打印的 `M5_FILTER_FPR_STRUCTURED_PPM` / `..._BUCKET_PPM` 是**负结果**性质的对照，
   **未**在全量工作负载上标定 zipf/热点分布（M5.3 的四类负载里也没有 zipf）。
5. **`kill -9` 系列只证明进程级一致性**：M5-B01 的 `BATCH_KILL9_MISSING 0` 不构成掉电安全证据
   （kill -9 不丢 page cache）；掉电语义由 `Batch.AtomicVisibilityUnderAppendAndSyncFailure` 的
   `MemEnv` 撕裂模型承担（8 个固定种子）。两者的结论**分开写**，未混用。
6. **TSan 只跑了 M5 的并发面**，且**未**跑 `--with-tsan` 全量门禁（M5-B07 的形状）：
   - 已跑并通过：`Filter.MultiThreadedGetNoRace`（A18）+ `WriteBatch.*` + `Batch.*`（含 A14 的 32 线程并发组提交）
     ⇒ **9/9 PASS，0 条 `WARNING: ThreadSanitizer`**（24.1 s）。
   - **未跑完**：`Filter.BlockReadReductionAtLeastThreeTimes`（单线程、20000 key × 2 库 + 4000 次点查）。
     实测在 TSan 下 **17 分钟未结束**（`ps` 采样：`%CPU 101`、10 s 内 `utime+stime` 增长 1007 ticks ⇒ 在跑不是死锁），
     逐层重跑的成本不允许。该条在 **Release**（`M5_FILTER_BLOCK_READS_WITHOUT 2000` / `WITH 18`）与
     **ASan**（`20/20 PASS`，其中含本条）下都已通过。
   - 因此 **不主张**「M5.1 全量 TSan 干净」，只主张「M5 的并发面 TSan 干净」。这是**本阶段最大的未闭合项**，
     建议由用户决定是否在 `--with-tsan` 全量门禁里单独跑一次（预计 ≥ 20 分钟/条）。
7. **`M5-A12` 的"fsync 失败 ⇒ 崩溃重开后不可见"没有断言**：这是刻意的——Append 已把字节交给文件
   （page cache），后续**成功的** fsync 或 Close 会把它变 durable，因此"崩溃重开后一定不可见"是**错的**
   断言。落地只断言「Write 返回错误 + 同一进程内整批不可见」（I51 的真实含义），并把"Append 失败"的
   强断言（字节根本没写进去）单独放在 ①。**登记为口径收窄，供评审裁决**。
8. **未新增 `FakeClock`**（E6 允许不新增）；M5 的 A 组全部用 `MemEnv` 的假时钟与 `CommitHook` 屏障，
   确定性用例零 `sleep`、零重试。
9. **`M5-A14` 的"sync=false 分支"未单独成例**：并发批用例只跑 `sync=true`（I54 的判据所在）；
   `sync=false` 的整批可见性由 `Batch.AtomicVisibilityUnderAppendAndSyncFailure` 的容量/掉电分支覆盖。
   **登记为与 §10.1 表格的差异**（表格写"`sync=true` 与 `false` 两种"）。
10. **`docs/m5-prerequisites.md` 未改动**（保留前一执行者的原文），只在 §0 上表里做了复核与一处行号收窄说明。
11. **本文件本身未提交**；上一阶段的 `docs/m5-prerequisites.md` 也仍**未提交**。

---

## §8 改动文件清单（按段拆分，供用户尝试分成两个提交）

`main` HEAD = `fa7c328`；以下全部为**未提交**的工作树改动。

### 8.1 只属 M5.1（Bloom filter）

| 文件 | 变更 |
|---|---|
| `src/filter_policy.h` | 新增：`FilterPolicy` 接口 + `NewBuiltinBloomPolicy` |
| `src/bloom.h` | 新增：`BloomHash` / `BloomKeyMayMatch` / `FilterBlockBuilder` / `FilterBlockReader` |
| `src/bloom.cpp` | 新增：LevelDB 口径 32 位哈希 + Double Hashing + filter block 位级编解码 |
| `src/sstable/table.{h,cpp}` | `FilterState` 三态 / `LoadFilterBlock` / `KeyMayMatch` / `ReadBlockImpl(verify_crc)` / `ReadStats` 追加 8 列 / `GetEntry` 的 step ②.5 |
| `src/sstable/table_builder.{h,cpp}` | 写 filter 块 + metaindex 注册 + `StartBlock`/`AddKey` 接入点 + 坏 internal key 防御 |
| `src/sstable/format.h` | `kBuiltinBloomFilterName` / `kFilterBaseLg` / `kBloomMinBits` / `kBloomMaxK` / `kBloomBitsMax` |
| `src/version_set.{h,cpp}` | `unknown_metaindex_entries` 的传递（**未**改 `TableCache::Open` 签名，见 D-14 的既有分析） |
| `src/db_impl.{h,cpp}` | `DbReadStats` 追加 8 列 + `MergeReadStats` 逐列累加 |
| `src/common.h` | `Options::bloom_bits = 10` |
| `src/db.cpp` | `DB::Open` 对 `bloom_bits` 的第一步校验 |
| `tests/filter_test.cpp` | 新增：12 条（M5-A01~A10 + A18 + damage 扫描） |
| `tests/sstable_table_test.cpp` | 追加 filter 相关断言 |
| `scripts/lsm_m5_unit_test.sh` | 新增：M5.1 的正向标记腿 |
| `docs/protocol.md` | **纯追加** §12 |
| `docs/m5-prerequisites.md` | 新增（M5.0 产出，前一执行者） |

### 8.2 只属 M5.2（WriteBatch）

| 文件 | 变更 |
|---|---|
| `src/write_batch.h` | 新增：`WriteBatch` 接口（含所有权/线程约束注释，L31） |
| `src/write_batch.cpp` | 新增：§13.1 编码/解析 + `Validate` |
| `src/db.h` | 新增纯虚 `Write(const WriteOptions&, WriteBatch*)` + `class WriteBatch;` 前向声明 |
| `tests/batch_test.cpp` | 新增：8 条（M5-A11~A17） |
| `scripts/lsm_batch_unit_test.sh` | 新增：M5.2 单元腿 |
| `scripts/lsm_batch_crash_test.sh` | 新增：M5-B01 崩溃对账腿 |
| `scripts/batch_crash_writer.cpp` | 新增：批 writer（sidecar 批指纹） |
| `scripts/batch_crash_recover.cpp` | 新增：批核对端（MISSING/MISMATCH/HALF/BATCHES_SEEN） |
| `docs/protocol.md` | **纯追加** §13 |

### 8.3 跨两段（无法干净拆分 —— 请合并提交或按标注拆）

| 文件 | M5.1 部分 | M5.2 部分 |
|---|---|---|
| `src/db.cpp` | `DB::Open` 的 `bloom_bits` 校验 | `MemoryDBImpl::Write(batch)` + `Write`→`WriteEntry` 改名 |
| `src/db_impl.h` | `DbReadStats` 追加 filter 列 | `Write` override / `Pending` 扩展 / `WriteEntry` / `SubmitPending` |
| `src/db_impl.cpp` | `MergeReadStats` 追加 filter 列 | `ParseOneEntry` / `Write` / `WriteEntry` / `SubmitPending` / `EncodeGroup` / `RunFlusher` |
| `CMakeLists.txt` | `src/bloom.cpp`（`lsm` + `lsm_sstable`）、`tests/filter_test.cpp` | `src/write_batch.cpp`、`tests/batch_test.cpp`、两个批崩溃目标 |
| `scripts/lsm_gate.sh` | `run_gate_m5_marked` + `--require-m5` + M5-B11 腿 | M5-B12 腿 + M5-B01 腿 |
| `docs/protocol.md` | §12 | §13 |

> 说明：`src/db_impl.{h,cpp}` 的两段改动在**同一函数邻域**（`Pending` / 组提交 / 合并统计），
> 无法用文件粒度拆开；若用户要两提交，建议：**提交 1 = M5.1 全部 + 8.3 的 M5.1 部分**，
> **提交 2 = M5.2 全部**，其中 `db_impl.{h,cpp}` 的两处改动需按上表手工分离（M5.1 的部分只有
> `DbReadStats` 的 8 列与 `MergeReadStats` 的 8 行累加，其余都属 M5.2）。
