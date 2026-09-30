# M5 实现证据（M5.1 + M5.2 + M5.3 收口版）

> 产出阶段：**M5.1**（Bloom filter + filter block + metaindex + 读路径否定 + 计数器）、
> **M5.2**（WriteBatch 编码 + `DB::Write` + 批提交 + WAL 一次写 + 崩溃原子性）、
> **M5.3**（微基准 + 四类负载 × 三类对照 + 数据表 + `bench_lsm.sh` 门禁 + 负结果入档 + 收口）。
> 基线：`main` HEAD **`99c417f`**（`feat(m5): M5.1 Bloom filter + M5.2 WriteBatch`；M1~M4 收口，
> tag `m1-memtable`/`m2-wal`/`m3-sstable`/`m4-compaction`）。M5.3 的改动**未提交**，由用户提交。
> 本文件**只登记证据与差异**；不改 `docs/m5-design.md`/`docs/m4-design.md`/`docs/m3-*.md`/`docs/m3-evidence.md`，
> `docs/protocol.md` 只做**纯追加**（§12/§13，证据见 §5）；`docs/amplification.md` 追加了 §6（`data_blocks_read`
> 语义修正的登记，见 §6 D-8）。
>
> 契约来源：`docs/m5-design.md` §11 的 M5.1/M5.2/M5.3、§2 的裁决（D1~D8 / E1~E8）、§3（filter 位级格式与读路径接入）、
> §4 §12/§13（协议追加文本）、§5（WriteBatch 与提交路径）、§6/§7/§8（微基准、门禁脚本、结论与负结果）、
> §9（`I47~I56` / `L30~L35`）、§10（测试矩阵）；`docs/protocol.md` §9.4（M2 冻结的「一个 batch = 一条 WAL record」）、
> §12（M5.1 追加）、§13（M5.2 追加）。M5.3 的数据表与负结果原文见 **`docs/m5-bench.md`**。

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

## §2.3 M5.3 判据逐条（`docs/m5-design.md` §11 M5.3 / §6 / §7 / §8 / §10.2）

| # | 判据（§11 M5.3 / §10.2） | 落地位置 | 证据（命令 / 计数行） | 状态 |
|---|---|---|---|---|
| 1 | `scripts/bench_lsm.sh` 全流程可复现（四类负载 × 三类对照） | `scripts/bench_lsm.sh` + `bench/bench_lsm.cpp` | `M5-C` 门禁腿：`BENCH_CELLS_TOTAL 12`（去重格）/`BENCH_LSM_CELLS_TOTAL 4`/`BENCH_MISSING_TOTAL 0`/`BENCH_MISMATCH_TOTAL 0`/`BENCH_REPRO_OK 1`/`BENCH_REPRO_LSM_CELLS 4`/`[BENCH_LSM_OK]` | ✅ |
| 2 | 固定 `CELL/THROUGHPUT/LATENCY/P99` 行；`AMPL`/`LEVEL`/`FRONT` 三行齐全 | 同上 + `lsm_ampl_probe` | `gate.log` 的 M5-C 段：12 条 `CELL` + `^AMPL round_id=ALL … space_filter_bytes=…`/`^LEVEL …`/`^FRONT … p99_us=…`（门禁用正则 AND） | ✅ |
| 3 | 硬门禁**只**施加在 LSM 腿（M5-C6） | `bench_lsm.sh` 的 missing/mismatch 求和只取 `engine=lsm` | §3.2 的 M5-C 腿标记；`raw_*`/`std_map` 的 `verify=na` 不进退出码 | ✅ |
| 4 | `AMPL` 行尾追加 `space_filter_bytes` / `space_sst_data_bytes`（§6.6） | `FormatAmplLine`（read_filter_* 四列）+ `scripts/lsm_level_stats.cpp`（逐文件 `Table::filter_bytes()` 求和后追加） | `SPACE_SST_BYTES == SPACE_FILTER_BYTES + SPACE_SST_DATA_BYTES`（on 2138613 = 109401 + 2029212）；`BENCH_SPACE_FILTER_BYTES 25113`（门禁腿） | ✅ |
| 5 | `>=3x` **只**作同轮开关对照、**只限不存在 key**、禁止外推 | `Filter.BlockReadReductionAtLeastThreeTimes`（M5-A10） | `M5_FILTER_BLOCK_READS_WITHOUT 2000` / `WITH 18` / `RATIO 111.111111` / `BLOCKS_SKIPPED 1982`（同进程、同数据集、同查询集合） | ✅ |
| 6 | 失败注入自测：`missing != 0 ⇒ rc=1`（M5-B03/`I56`） | `scripts/bench_lsm_selftest.sh` 驱动 `bench_lsm.sh --inject-missing` | `M5-D` 腿：`BENCH_INJECT_MISSING_RC 1` + `[BENCH_SELFTEST_OK]`（且 `BENCH_MISSING_TOTAL 1`） | ✅ |
| 7 | 磁盘 filter 损坏扫描（M5-B09/E4）：零静默假阴性 + 重算 CRC 的错位注入被检出 | `scripts/lsm_filter_damage.cpp` + `lsm_filter_damage_test.sh` | `M5-E` 腿：`FILTER_DAMAGE_CASES 2000`、`FILTER_SILENT_FALSE_NEGATIVE 0`、`FILTER_SILENT_WRONG_VALUE 0`、`FILTER_DAMAGE_DETECTED 1923`、`FILTER_DAMAGE_FILTER_DEGRADED 77`、`FILTER_MISALIGN_DETECTED 1`、`[FILTER_DAMAGE_OK]` | ✅ |
| 8 | 复现性（M5-B05）：同参数两轮中位数在 `--repro-tol` 内 | `bench_lsm.sh` 的 `BENCH_REPRO_OK`（按 M5-C6 只对 `engine=lsm`） | 门禁腿 **连跑 3 次**：3/3 `rc=0`、`BENCH_REPRO_OK 1`、`BENCH_REPRO_LSM_CELLS 4`；100k 文档轮 4 个 LSM 格 `repro_ok=1` | ✅ LSM 腿；非 LSM 格的 `repro_ok=0` 仍打印但不进退出码（§7 第 2 条） |
| 9 | 负结果入档（§8.2）：data + 归因 + 原文保留 | `docs/m5-bench.md` §8（N1~N9） | N1 已存在 key 无吞吐收益、N2 filter 空间/写代价 +5.40% 且 `NET_LOSS`、N4 "batch 大反而慢"未被支持、N5 fsync 漂移、N7 `compaction_rounds=0` 非稳态 | ✅ |
| 10 | 三构建 + 门禁收口（M5-B06/B07） | 见 §3.1/§3.2 | Release 205/205 + 0 warning；**ASan 全量 205/205、0 sanitizer 报告**；**TSan 全量 205/205、0 race**；`--no-asan` 门禁 **24/24 PASS**；**`--with-tsan` 完整门禁 26/26 PASS + `[OK]`（M5-B07）** | ✅ |
| 11 | `data_blocks_read` 语义修正登记（用户裁决 #1） | `src/sstable/table.cpp`（M5.1）+ `docs/amplification.md` §6 + §6 D-8 | 修正前 M4 行 `read_data_blocks_read=0`（原文保留）；修正后 `bench_lsm` on/off 行 `read_data_blocks_read=167760`（非零，且读结果不变） | ✅ |

**M5.3 的一处设计收窄（登记，不是放宽）**：§6.3 的 `repeat=<i>` 落地为每格一行的
`repeat=<R>`（R = 重复次数），并在同一行追加 `raw1_us/raw2_us/raw3_us` 三次原始中位延迟；
理由：`--expected-min-cells` 与 §6.6 的数据表都以"格"为单位，逐 repeat 成行会让
`BENCH_CELLS_TOTAL` 与格数脱钩。审计方若按"逐 repeat 一行"解析需改（登记于 §6 D-11）。

**M5.3 的两处驱动修正（登记于 §6 D-12/D-13）**：① 读负载**不**按 `batch` 折算（首版把读也按 batch
折算，导致 batch=16/256 的读吞吐被放大 16/256 倍；已修，并用 batch 1/16/256 的对照复测）；
② 复现性判据只对 `engine=lsm` 生效（依据 M5-C6）。

---



## §3 三构建与门禁原始计数行

### 3.1 三构建（**刚重建后测**）

| 构建 | 命令 | 原始计数行 |
|---|---|---|
| Release（干净重建） | `bash scripts/lsm_gate.sh --rounds 100 --with-tsan --require-m3 --require-m5`（第一腿 `scripts/lsm_build.sh`，`rm -rf build` 全量重建） | `[CHECK] warning 计数 = 0（要求 0）`；`[==========] 205 tests from 56 test suites ran. (71061 ms total)`；`[  PASSED  ] 205 tests.` |
| ASan（全量，门禁内） | 同上门禁的 `ASan 全量` 腿（`cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests`） | `[==========] 205 tests from 56 test suites ran. (440999 ms total)`；`[  PASSED  ] 205 tests.`；`grep -c AddressSanitizer = 0` |
| TSan（全量，门禁内；A10 缩规模） | 同上门禁的 `TSan 全量` 腿（`cmake -S . -B build-tsan -DENABLE_TSAN=ON … && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests`） | `[==========] 205 tests from 56 test suites ran. (938829 ms total)`；`[  PASSED  ] 205 tests.`；`grep -c "WARNING: ThreadSanitizer" = 0` |
| TSan（并发面，独立跑） | `setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter='Filter.MultiThreadedGetNoRace:WriteBatch.*:Batch.*'` | `[  PASSED  ] 9 tests.`；`TSAN_FOCUS_ELAPSED 32.27`；`FOCUS_RC=0`；0 race |
| TSan（M5-A10 缩规模等价输入，**用户裁决 ①**） | `setarch … --gtest_filter='Filter.BlockReadReductionAtLeastThreeTimes'`（TSan 下 1500 key/300 查询） | `M5_FILTER_BLOCK_READS_WITHOUT 300` / `WITH 3` / `RATIO 100.000000` / `BLOCKS_SKIPPED 297` / `FALSE_NEGATIVE 0`；`[  PASSED  ] 1 test.`；`TSAN_SCALED_ELAPSED 16.69`；`SCALED_RC=0` |
| TSan（M5-A10 **全尺寸** 20000 key，**不做**，见 §7 第 6 条） | 同上前一版（`timeout 2400`，未缩规模） | M5.1 阶段实测 **25 分钟仍在 `[ RUN ]`**（CPU 100%、非死锁）⇒ 记为**未验证/不做**，由 Release 全尺寸与 ASan 全尺寸覆盖 |

**ASan 与 TSan 都是门禁内的全量 205/205**（不只是 M5 子集）：ASan 0 条 sanitizer 报告（441.0 s）、
TSan 0 条 `WARNING: ThreadSanitizer`（938.8 s）；独立跑的 TSan 并发面 9/9（32.3 s）与缩规模 A10
（16.7 s）作为交叉证据。**Release 的 0 warning 由第一腿硬断言（71061 ms / 205 PASSED）。**

**TSan 未闭合项的结论**：取用户裁决的 **①「缩小规模、判据不降」**——M5-A10 在 TSan 下用
`#if defined(__SANITIZE_THREAD__)` 的 1500 key/300 查询输入（`WITHOUT 300`/`WITH 3`/`RATIO 100.0`/
`SKIPPED 297`/`FALSE_NEGATIVE 0`，断言原样：`without>=3`、`ratio>=3.0`、`skipped>0`、
`with==positive+unavailable`、`positive<queries/10`）。**TSan 全量 205/205 PASS、0 条 race**
（949 s），因此本轮**主张**「TSan 全量干净」，但**明确限定**：A10 那一条是缩规模等价输入；
**全尺寸 A10 在 TSan 下不做**（实测 >25 分钟/条，非死锁），由 Release 全尺寸
（`WITHOUT 2000`/`WITH 18`/`RATIO 111.1`）与 ASan 全尺寸覆盖。

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
PASS  M5-C 基准（四类负载 × 三类对照 + AMPL/LEVEL/FRONT 追加列，硬门禁只在 LSM 腿）
PASS  M5-D 基准失败注入自测（missing!=0 ⇒ rc=1）
PASS  M5-E filter 磁盘损坏扫描（零静默假阴性）
[OK] 全部门禁通过
```

**（`--no-asan` 下 ASan 腿不跑，ASan 全量见 §3.1；`--with-tsan` 未开，见 §7 第 6 条。
24 条腿全部 PASS、末行 `[OK]`；无 `[PARTIAL]`、无 SKIP。）**

关键原始计数行（逐字摘自该次门禁的 `gate.log`）：

```
TOTAL_ROUNDS 100 ROUNDS_OK 100 ACKED_TOTAL 1179 MISSING_TOTAL 0 MISMATCH_TOTAL 0
TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0 RECORD_BYTES 40
MIDDLE_OPEN_CORRUPTION 1 RECOVERED_PREFIX -1 DETAIL Corruption: RecoverAndOpen: log 中间损坏（其后仍有完好 record）: /tmp/lsm_mid_7P7nwt/db/000001.log @1960 CRC 不符
SST_FILES_TOTAL 100
RECORDS_REPLAYED_TOTAL 1236
RECORDS_REPLAYED 0 RESTART_KEYS_OK 2000/2000 SST_FILES_REGISTERED 2
FD_GROWTH 1 FD_BASELINE 8 FLUSHES_COMPLETED 452
COMPACTION_ROUNDS_TOTAL 50 SST_FILES_TOTAL 49 MISSING_TOTAL 0 MISMATCH_TOTAL 0 ROUNDS_OK 30
INJECT_POINTS_OK 4 MISSING_TOTAL 0 REF_MISSING_TOTAL 0 OPEN_CORRUPTION_TOTAL 0 ORPHAN_REMOVED_TOTAL 3
M5_TESTS_RAN 32  M5_TESTS_FAILED 0  M5_FILTER_RAN 12  M5_FILTER_FALSE_NEGATIVE 0  M5_FILTER_SILENT_FALSE_NEGATIVE 0  M5_FILTER_BLOCK_READS_WITHOUT 2000  M5_FILTER_BLOCK_READS_WITH 18  M5_FILTER_BLOCK_READ_RATIO 111.111111  M5_FILTER_BLOCKS_SKIPPED 1982  M5_FILTER_DAMAGE_CASES 2471  M5_FILTER_DAMAGE_FILTER_DEGRADED 85  M5_FILTER_FPR_PPM 8310  LSM_SSTABLE_FORBIDDEN 0  [FILTER_OK]  [FILTER_DAMAGE_OK]
M5_BATCH_TESTS_RAN 8  M5_BATCH_TESTS_FAILED 0  M5_BATCH_WRITEBATCH_RAN 3  M5_BATCH_ROUNDTRIP_ENTRIES 9004  M5_BATCH_PARTIAL_VISIBLE 0  M5_BATCH_HALF_VISIBLE 0  M5_BATCH_CRASH_HALF_VISIBLE 0  M5_BATCH_CRASH_CASES 8  M5_BATCH_LOST_WAKEUPS 0  M5_BATCH_GROUP_FSYNCS 1  M5_BATCH_CONCURRENT_WRITERS 32  M5_BATCH_TRUNCATE_CASES 8  M5_BATCH_RECOVERY_RECORDS 3  M5_BATCH_RECOVERY_ENTRIES 10  M5_BATCH_ONE_RECORD_PER_BATCH 1  M5_BATCH_SEQ_CONTIGUOUS 1  M5_BATCH_LAST_SEQ_COVERS 1  LSM_BATCH_FORBIDDEN 0  [BATCH_OK]
BATCH_KILL9_ROUNDS 100 ROUNDS_OK 100 BATCH_ACKED 1018 BATCHES_SEEN 1066 BATCH_KILL9_MISSING 0 BATCH_MISMATCH 0 BATCH_HALF_VISIBLE 0
[BATCH_CRASH_OK] rounds=100 batch_size=16 sync=1 missing=0 mismatch=0 half=0
```

**M5.3 三条腿各自的标记行（逐字摘自同一次 `gate.log`）**：

```
# M5-C（脚本 scripts/bench_lsm.sh；--dataset 20000 --repeats 2 --warmup 1000
#       --write-buffer-size 16777216 --ampl-write-buffer-size 262144 --filter on）
BENCH_CELLS_TOTAL 12
BENCH_CELL_ROWS_TOTAL 12
BENCH_LSM_CELLS_TOTAL 4
BENCH_MISSING_TOTAL 0
BENCH_MISMATCH_TOTAL 0
BENCH_UNRELIABLE_CELLS 0
BENCH_REPRO_OK 1
BENCH_REPRO_LSM_CELLS 4
BENCH_AMPL_OK 1
BENCH_AMPL_MISSING 0
BENCH_AMPL_MISMATCH 0
BENCH_SPACE_SST_BYTES 491409
BENCH_SPACE_FILTER_BYTES 25113
BENCH_SPACE_SST_DATA_BYTES 466296
[BENCH_LSM_OK]

# M5-D（scripts/bench_lsm_selftest.sh）
BENCH_INJECT_MISSING_RC 1
[BENCH_SELFTEST_OK]

# M5-E（scripts/lsm_filter_damage_test.sh --cases 2000）
FILTER_DAMAGE_CASES 2000
FILTER_SILENT_FALSE_NEGATIVE 0
FILTER_SILENT_WRONG_VALUE 0
FILTER_DAMAGE_DETECTED 1923
FILTER_DAMAGE_FILTER_DEGRADED 77
FILTER_MISALIGN_INJECTED 1
FILTER_MISALIGN_DETECTED 1
FILTER_MISALIGN_SILENT_FN 0
[FILTER_DAMAGE_OK]
```

**L18 探针（`I17`/`L7` 持锁零 IO）**：`./build/bin/lsm_tests --gtest_filter='Flush.NoIoWhileHoldingDbMutex'`
⇒ `[==========] 1 test from 1 test suite ran. (35 ms total)` / `[  PASSED  ] 1 test.` ✅

**M2 高危区回归**：`TOTAL_ROUNDS 100 ROUNDS_OK 100 … MISSING_TOTAL 0 MISMATCH_TOTAL 0`（组提交语义未回退）；
`M5_BATCH_GROUP_FSYNCS 1`（32 并发批仍只 1 次 fsync，D3 的组批上限未被批路径破坏）。

**腿数与"不删不弱化"**：`grep -c '^run_gate' scripts/lsm_gate.sh` = **28**（4 个定义 + 24 条腿）。
24 条腿 = **21 条既有**（18 条 M2/M3/M4 + M5.1 的 M5-B11 + M5.2 的 M5-B12/M5-B01）+ **3 条 M5.3**
（M5-C/M5-D/M5-E）。既有腿的标记/脚本一行未删；新增腿全部走 `run_gate_m5_marked`
（多标记 AND、缺脚本 `SKIP`+`[PARTIAL]`、`--require-m5` 时 `SKIP` 即 `FAIL`）。

### 3.3 M5-B07：`--with-tsan` 完整门禁（**已跑，26/26 PASS**）

```
$ bash scripts/lsm_gate.sh --rounds 100 --with-tsan --require-m3 --require-m5
==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  ASan 全量
PASS  TSan 全量（setarch 关 ASLR）
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
PASS  M5-C 基准（四类负载 × 三类对照 + AMPL/LEVEL/FRONT 追加列，硬门禁只在 LSM 腿）
PASS  M5-D 基准失败注入自测（missing!=0 ⇒ rc=1）
PASS  M5-E filter 磁盘损坏扫描（零静默假阴性）
[OK] 全部门禁通过
GATE_RC=0
```

⇒ 26 条腿全部 PASS、末行 `[OK]`（无 `[PARTIAL]`、无 SKIP）：**ASan 全量与 TSan 全量都在门禁内
真跑并通过**。TSan 的 A10 一条走缩规模等价输入（D-14），这是该门禁能跑完的唯一偏离，已在 §3.1/§7 登记。

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
| D-8 | §11 M5.1「必改」把「fix `data_blocks_read` 从不递增」列为本阶段工作 | M5.1 在 `Table::ReadBlockImpl` 里为 `expected == kBlockTypeData` 递增 `data_blocks_read`；M5.3 登记于 `docs/amplification.md` **§6** | **用户已裁决批准**：这是对既有诊断列语义的修正，**只影响计数、不影响任何读结果/校验语义**（不改 CRC/长度/type、不改 `blocks_read` 口径）。M4 的 `read_data_blocks_read=0` 原文保留在 `amplification.md` §2；M5.3 的 `>=3x` 判据定义在该列上（M5-A10），因此必须先修好该列 |
| D-9 | §3.6 的 `ReadStats` 追加 7 列 | 落地追加 **8** 列（多 `filter_corrupt`） | M5-A08 要求「计数 `filter_corrupt`」而 7 列里没有该名字；只追加、不改既有 8 列 |
| D-10 | §5.3「`front_samples_us_`：一次 `DB::Write` 调用一个样本」 | **未改**：仍是「一个**组**一个样本」（M4.3 的落地形态） | 该项影响的是 M5.3 的数据表口径（`FRONT` 行的含义），M5.1/M5.2 不改以免动既有 `AMPL`/`FRONT` 断言；**留给 M5.3 处理并登记** |
| D-11 | §6.3 的固定 CELL 行含 `repeat=<i>` | 落地为每格**一行**、`repeat=<R>`（R = 重复次数），同一行追加 `raw1_us/raw2_us/raw3_us` | 理由：`--expected-min-cells` 与 §6.6 的数据表都以"格"为单位；逐 repeat 成行会让 `BENCH_CELLS_TOTAL` 与格数脱钩。原始值仍逐次打印（§6.4 的复现性核对不受影响）。已登记于 `docs/m5-bench.md` §8 |
| D-12 | §6.1/§6.3 的 `--batch K` 作用范围未写清 | **读负载不按 batch 折算**：一个 Get = 一个 op；只有写负载按 batch 成组 | 首版驱动误把读也按 batch 折算 ⇒ batch=16/256 的读吞吐被放大 16/256 倍（自检发现）；修正后用 batch 1/16/256 复测，读吞吐 1787~1933 ops/s 与 batch 无关。登记于 `docs/m5-bench.md` §6/N4 |
| D-13 | §7.3 的「同一格中位数差异超阈 ⇒ `BENCH_REPRO_OK 0`」未限定 engine | 复现性判据**只对 `engine=lsm`** 生效（并新增 `BENCH_REPRO_LSM_CELLS` 正向计数）；非 LSM 格仍逐行打印 `repro_ok` | 依据 M5-C6「硬门禁只施加在 LSM 腿」与原 §7.3「raw/map 不影响退出码」；`--filter off` 那轮唯一的 `repro_ok=0` 格是 `seq_write/raw_file`（非 LSM）。已登记于 `docs/m5-bench.md` §3/N3/N9 |
| D-14 | §10.2 M5-B06/§11 M5.3「ASan/TSan 干净」；`Filter.BlockReadReductionAtLeastThreeTimes` 全尺寸在 TSan 下未闭合 | TSan 下用**缩小规模的等价输入**（`#if defined(__SANITIZE_THREAD__)`：1500 key/300 查询；Release/ASan 仍 20000/2000），断言与判据**不降**（`without>=3`、`ratio>=3.0`、`skipped>0`、口径自洽、误判率<10%） | 全尺寸在 TSan 下实测 >25 分钟未结束（非死锁、CPU 100%）；用户裁决二选一，取**①缩小规模等价覆盖 + 明说全尺寸不做**。见 §7 第 6 条与 §3.1 的实测数字 |
| D-15 | §6.6 要求 `space_filter_bytes` 与 CELL 格同源；§7.5 的 M5-C 腿只给一个 `bench_lsm.sh` | ① `lsm_ampl_probe` 追加 `--bloom-bits`（子轮与 `--filter` 一致）；② `bench_lsm.sh` 追加 `--ampl-write-buffer-size`，M5-C 门禁腿 CELL 用 16 MiB（稳定命中 MemTable）、AMPL 子轮用 256 KiB（强制产生 SST） | 首版 AMPL 子轮恒用 `bloom_bits=10` ⇒ `--filter off` 的 `space_filter_bytes` 仍是 109401（口径不一致）；首版门禁腿 20000-key/256 KiB 的读计时窗口横跨 MemTable→SST 切换 ⇒ 复现性误报。两处都已修并加硬校验（off ⇒ `space_filter_bytes==0`）。登记于 `docs/m5-bench.md` §4/N6/N9 |
| D-16 | §5.3「一次 `DB::Write` 调用一个样本」（D-10 留给 M5.3） | **M5.3 仍不改** `front_samples_us_` 的采样粒度；在 `docs/m5-bench.md` §4 披露 `FRONT` 的 Put 侧是**下界近似** | 改采样粒度会动 M4.3 既有 `FRONT` 断言与 `AMPL` 行口径，收益只是让数字更"准"；M5.3 的判据不建立在 Put 侧 FRONT 上（四类负载的写延迟由 CELL 行给出）。口径披露见 `docs/amplification.md` §5 与 `docs/m5-bench.md` |

---

## §7 未做 / 未验证清单（诚实登记）

1. **git 状态**：M5.1/M5.2 已由用户在 `99c417f` 提交（tag 仍只有 m1~m4）；**M5.3 的全部改动未提交**
   （清单见 §8），提交与打 tag 由用户执行。本阶段只做只读 git 查询。
2. **M5-B05 复现性的口径收窄与一次真实失败**：`--filter off` 的 100k 轮在**修正前**脚本下
   `rc=1`（`BENCH_REPRO_OK 0`），唯一超阈的格是**非 LSM** 的 `seq_write/raw_file`（三轮中位数差 >25%）；
   4 个 LSM 格 `repro_ok=1`、`missing/mismatch=0`。M5.3 按 M5-C6 把复现性判据收窄到 `engine=lsm`
   （D-13），并在修正后的门禁腿上**连跑 3 次**：3/3 `rc=0`、`BENCH_REPRO_OK 1`、`BENCH_REPRO_LSM_CELLS 4`。
   **不主张**「100k off 轮在旧脚本下也全绿」——旧结果原文保留在 `docs/m5-bench.md` §3/N3。
3. **`Options::bloom_bits` 的"关闭对照"结论范围**：块读计数层有同轮对照（`M5-A10`：2000 vs 18，111×）；
   CELL 层的吞吐对照显示**已存在 key 的点查没有 filter 收益**（`docs/m5-bench.md` N1，rand_read 1782 vs 1706）。
   **不主张**任何"提升 N 倍"的性能结论；`>=3x` 只限"不存在 key 的数据块读次数"。
4. **假阳性率只测了三种分布**（随机、`even_keys` 结构化、`bucket` 型）；`M5-A02` 的
   `M5_FILTER_FPR_STRUCTURED_PPM`/`..._BUCKET_PPM` 是负结果性质的对照，**未**标定 zipf/热点分布
   （四类负载里也没有 zipf）。
5. **`kill -9` 系列只证明进程级一致性**：M5-B01 的 `BATCH_KILL9_MISSING 0` 不构成掉电安全证据
   （kill -9 不丢 page cache）；掉电语义由 `Batch.AtomicVisibilityUnderAppendAndSyncFailure` 的
   `MemEnv` 撕裂模型承担（8 个固定种子）。两者结论**分开写**，未混用。
6. **TSan 未闭合项的明确结论（用户裁决 ②→取 ①）**：
   - **取 ①「缩小规模但判据不降的等价覆盖」**：`Filter.BlockReadReductionAtLeastThreeTimes` 在
     TSan 下用 `#if defined(__SANITIZE_THREAD__)` 的 1500 key / 300 查询输入，**断言原样保留**
     （`without>=3`、`without/max(1,with)>=3.0`、`skipped>0`、`with==positive+unavailable`、
     `positive<queries/10`）；Release/ASan 仍是 20000/2000。D-14。
   - **已跑并通过（本轮实测，见 §3.1）**：TSan 并发面
     `Filter.MultiThreadedGetNoRace` + `WriteBatch.*` + `Batch.*` = 9/9 PASS、0 条
     `WARNING: ThreadSanitizer`（32.27 s）；TSan 下的缩规模 `BlockReadReduction` =
     `WITHOUT 300`/`WITH 3`/`RATIO 100.0`/`SKIPPED 297`/`FN 0`、PASS（16.69 s）。
   - **TSan 全量 205 条已跑并通过**：`[ PASSED ] 205 tests`（949.24 s，`FULL_RC=0`），
     `grep -c "WARNING: ThreadSanitizer" = 0` ⇒ 本轮**主张**「TSan 全量干净」，
     **限定**：其中 A10 一条是缩规模等价输入（见上）。
   - **全尺寸 A10 在 TSan 下不做**：实测 20000 key × 2 库 + 4000 点查在 TSan 下单条 **>25 分钟未结束**
     （`ps` 采样 `%CPU 101`、`utime+stime` 持续增长 ⇒ 在跑不是死锁）；**全尺寸由 Release
     （2000/18/111.1）与 ASan 全尺寸覆盖**。
   - **`--with-tsan` 全量门禁（M5-B07）已跑并通过**：`lsm_gate.sh --rounds 100 --with-tsan
     --require-m3 --require-m5` ⇒ **26/26 PASS + `[OK]`**（含 `ASan 全量`、`TSan 全量` 两条腿），
     见 §3.3。唯一限定：TSan 里的 A10 一条是缩规模等价输入（上一条）。
7. **ASan 的范围**：见 §3.1 的实测行（本轮）。
8. **`M5-A12` 的"fsync 失败 ⇒ 崩溃重开后不可见"没有断言**：这是刻意的——Append 已把字节交给文件
   （page cache），后续**成功的** fsync 或 Close 会把它变 durable，因此"崩溃重开后一定不可见"是**错的**
   断言。落地只断言「Write 返回错误 + 同一进程内整批不可见」（I51 的真实含义），并把"Append 失败"的
   强断言（字节根本没写进去）单独放在 ①。**登记为口径收窄，供评审裁决**。
9. **未新增 `FakeClock`**（E6 允许不新增）；M5 的 A 组全部用 `MemEnv` 的假时钟与 `CommitHook` 屏障，
   确定性用例零 `sleep`、零重试。
10. **`M5-A14` 的"sync=false 分支"未单独成例**：并发批用例只跑 `sync=true`（I54 的判据所在）；
    `sync=false` 的整批可见性由 `Batch.AtomicVisibilityUnderAppendAndSyncFailure` 的容量/掉电分支覆盖。
    **登记为与 §10.1 表格的差异**（表格写"`sync=true` 与 `false` 两种"）。
11. **绝对时间的负载复核已完成**：fsync（idle 500 次，load1=1.32）median 8.084 ms 与同轮 7.733/7.943 ms
    一致；`FRONT` on/off 的 p50 46/48→48/47 us。CELL 的绝对吞吐仍来自 load1≈2.96 的那一轮，已在
    `docs/m5-bench.md` §1 标注**不与空闲机器/外部系统比较**；相对量不受影响。
12. **`docs/m5-prerequisites.md` 未改动**（保留前一执行者的原文），只在 §0 上表里做了复核与一处行号收窄说明。
13. **未做**：mmap / 压缩 / 块缓存（M4-B10 已登记 NOT_APPLICABLE）；zipf/热点分布标定；K≥1024 或接近
    `kMaxGroupBytes=1 MiB` 的 batch 边界；`--pipeline>1` 在四类负载上的对照（驱动支持，但本轮文档轮的
    `pipeline=1`）；compaction 发生轮的稳态写放大/长尾（本轮 `compaction_rounds=0`，见 m5-bench N7）。

---

## §8 改动文件清单

`main` HEAD = **`99c417f`**。§8.1/§8.2/§8.3 是 **M5.1/M5.2 的历史清单**（已随 `99c417f` 提交，
按当时的 `fa7c328` 基线列出，保留作溯源）；**§8.4 是 M5.3 的未提交清单**。

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

### 8.4 只属 M5.3（微基准 + 收口；**未提交**）

| 文件 | 变更 |
|---|---|
| `bench/bench_lsm.cpp` | 新增：四类负载 × 三类对照的驱动（只 include `common.h`/`db.h`/`write_batch.h`，只走公共 DB 接口；固定 CELL 行） |
| `scripts/bench_lsm.sh` | 新增：头行/参数/交替调用/CELL 解析/**只对 LSM 的** missing/mismatch 门禁/收尾标记；`--ampl-*` 与 `lsm_level_stats` 子轮（D-15） |
| `scripts/lsm_level_stats.cpp` | 新增：逐文件 `Table::filter_bytes()` 求和，把 `space_filter_bytes`/`space_sst_data_bytes` 追加到 `AMPL` 行尾 |
| `scripts/bench_lsm_selftest.sh` | 新增：失败注入自测（`--inject-missing` ⇒ rc=1 + `[BENCH_SELFTEST_OK]`） |
| `scripts/lsm_filter_damage.cpp` | 新增：真实磁盘 filter 损坏扫描（逐字节翻转 + 重算 CRC 的错位注入） |
| `scripts/lsm_filter_damage_test.sh` | 新增：M5-E 腿的包装与标记校验 |
| `src/db_impl.{h,cpp}` | `AmplificationStats` 追加 5 个 filter 读计数；`FormatAmplLine` 行尾**只追加** `read_filter_*` 四列 + `read_data_blocks_skipped_by_filter` |
| `tests/filter_test.cpp` | M5-A10 追加 TSan 缩规模等价输入（D-14；Release/ASan 不变） |
| `scripts/lsm_ampl_probe.cpp` | 追加 `--bloom-bits`（AMPL 子轮与 `--filter` 一致，D-15） |
| `scripts/lsm_gate.sh` | M5-C/M5-D/M5-E 三条腿（`run_gate_m5_marked`，脚本打印收尾汇总标记行）+ M5-C 的 16 MiB/256 KiB 参数（D-15） |
| `CMakeLists.txt` | 新目标 `bench_lsm` / `lsm_level_stats` / `lsm_filter_damage` |
| `.gitignore` | `bench/tmp/`、`bench/*.out`、`bench/*.txt`、`bench_lsm_*.txt` |
| `docs/m5-bench.md` | 新增：M5.3 数据表 + fsync 基线 + 四类负载 × 三类对照 + filter 代价 + LSM 劣势 + 负结果 N1~N9 |
| `docs/m5-evidence.md` | 本文件：收口（§2.3 / §3 / §6 D-8..D-16 / §7 / §8.4） |
| `docs/amplification.md` | **追加 §6**：`data_blocks_read` 语义修正的登记（只影响计数，不影响读结果） |

> M5.3 未新增/未修改 M1~M4 的**既有测试断言**、未改 `docs/m5-design.md`/`docs/m4-design.md`/`docs/m3-*.md`；
> `docs/protocol.md` 在 M5.3 未改动（§12/§13 是 M5.1/M5.2 追加）。
