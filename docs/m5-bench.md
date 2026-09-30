# M5.3 微基准与数据表（docs/m5-bench.md）

> 产出阶段：**M5.3**（微基准 + 四类负载 × 三类对照 + 数据表 + `bench_lsm.sh` 门禁 + 负结果入档）。
> 契约来源：`docs/m5-design.md` §6（微基准与数据表）、§7（`bench_lsm.sh` 行为规格）、§8（结论表达、
> 负结果、与 M4 的关系）、§10.2 的 M5-B02/M5-B04/M5-B05/M5-B10、§11 M5.3。
> 驱动：`bench/bench_lsm.cpp`（只走公共 `DB` 接口）+ `scripts/bench_lsm.sh`（头行/对账/门禁/收尾标记）
> + `scripts/lsm_ampl_probe`（M4.3，产 `AMPL`/`LEVEL`/`FRONT`）+ `scripts/lsm_level_stats`（M5.3 新增，
> 逐文件实测 `space_filter_bytes` 并**追加在 `AMPL` 行尾**）。
>
> **不可外推（§6.5，逐字写死）**：① 单机、单块 ext4、VM、loopback 级场景；② `sync=false` 的数字只说明
> 「进程级一致性」，**不证明掉电安全**；③ 数据集/`value_size`/`batch`/`pipeline` 只在这些具体参数下成立；
> ④ 绝对吞吐不承诺达到任何外部系统的数字；⑤ `>=3x` 只针对「**不存在** key 的数据块读次数」。

---

## 1. 机器状态与构建（原始头行）

```
MACHINE nproc=8 load1=2.96 load5=3.49 load15=3.11 fs=ext4 mount=/ kernel=6.8.0-138-generic
PARAMS dataset=100000 value_size=100 key_dist=seq batch=1 pipeline=1 sync=0 repeats=3 warmup=10000 filter=on bloom_bits=10 seed=0x5EED2025 write_buffer_size=4194304 repro_tol=0.25,0.50
AMPL_PARAMS ampl_keys=100000 ampl_rounds=1 ampl_strategy=round_robin ampl_source=build/bin/lsm_ampl_probe space_source=build/bin/lsm_level_stats
BUILD rev=99c417fd06b2e4231e1e2118c1e89eccf64d92ac dirty=1 build_type=Release cxx=g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0
FSYNC_BASELINE median_ms=7.943 source=scripts/fsbench_commit_latency.cpp
```

- `dirty=1` 是**如实**打印：本次数据是在 M5.3 工作树（未提交）上跑的；`rev` 指 M5.1/M5.2 的提交基线。
- 数据文件：`/tmp/lsm_bench_on.txt`（filter=on）、`/tmp/lsm_bench_off.txt`（filter=off）。
  **两个结果文件与本文档同轮冻结**；`LSM` 格 `missing/mismatch` 全 0。
- **绝对时间的负载复核（M5.3 追加）**：上表 `MACHINE load1=2.96` 是 CELL 轮的头行；§2 的 fsync
  基线与 §4 的 `FRONT` 行已在**串行、无并发重活**下重测（重测时 `load1=1.32`，见 §2/§4 的
  "idle 复测"），两者与同轮值一致（fsync median 7.733 → 8.084 ms；`FRONT` on p50 46→48 us）。
  CELL 的绝对吞吐仍来自 `load1≈3` 的那一轮：**相对量（on/off、engine 之间）可用，绝对吞吐只作
  同一轮内的排序参考**，不与空闲机器或外部系统比较（§6.5 的不可外推）。
- CPU 频率抖动不可控、未用 `cpupower`/`taskset`：这是**不可控事实**，不改判据（§6.4）。

## 2. fsync 成本基线（M5 自己重测，不继承 8 ms）

**同轮**（`bench_lsm.sh` 头行，100 次）：`FSYNC_BASELINE median_ms=7.943`。
**idle 复测**（500 次，串行无并发，`load1=1.32`）：

```
STRATEGY append+fsync           N  500  MIN_MS   1.923  MEDIAN_MS   8.084  P90_MS  11.136  MAX_MS  22.436
STRATEGY append+fdatasync       N  500  MIN_MS   2.597  MEDIAN_MS   8.367  P90_MS  11.727  MAX_MS  19.758
STRATEGY prealloc+fsync         N  500  MIN_MS   2.811  MEDIAN_MS   8.786  P90_MS  11.513  MAX_MS  16.333
STRATEGY O_DIRECT+fsync         N  500  MIN_MS   2.597  MEDIAN_MS   9.178  P90_MS  11.815  MAX_MS  22.592
STRATEGY create+fsync+dirfsync  N  500  MIN_MS   3.399  MEDIAN_MS  10.653  P90_MS  13.804  MAX_MS  86.504
```

- 本机**idle 复测** `append+fsync` **median = 8.084 ms**，与同轮 7.733/7.943 ms 一致（差 <5%）；
  `sync=true` 的 LSM 写吞吐实测 125~135 ops/s，`1/125 s ≈ 8.0 ms`，与基线自洽。**这不是并发负载的产物**
  （复测时 load1=1.32，且无其他重活）。
- **负结果 N5（修订）**：M2/M3 在**同一台 VM 的另一个时间点**实测过 2.5~3.2 ms
  （`docs/m2-evidence.md`/`m3-evidence.md`），本轮 idle 复测是 8.084 ms —— 差 ~2.5×。
  两者都是真的；**fsync 成本不是常数**（宿主/VM/文件系统状态随时间漂移），因此所有
  "组提交收益 N 倍"的结论都必须除以**当次**实测值，不能用「指令里的 fsync≈8ms」或 M2 的 2.5ms 互相替代。

## 3. 四类负载 × 三类对照（固定 CELL 行；filter=on / off 两列）

口径：`THROUGHPUT` = ops/s，**op = 一个 entry**；`LATENCY` = 所有调用的中位延迟（us），`P99` 同理；
`batch=1` 时 `latency_unit=op`（= 一次 Put/Get），`batch>1` 时写侧变 `call`（见 §6/负结果 N4）。
三类对照的**持久性语义不同**（`durability` 列）：裸文件档是 `fsync`/`fdatasync`/`none`，`std_map` 恒
`none`（完全不持久）。**禁止**把 `std::map` 的吞吐说成「LSM 应该达到的目标」（§6.2）。

| load | engine | filter | throughput (ops/s) | p50 (us) | p99 (us) | durability | verify |
|---|---|---|---|---|---|---|---|
| seq_write | lsm | on | 2307.9 | 264 | 1250 | none | lsm (missing 0) |
| seq_write | lsm | off | 2394.7 | 256 | 1255 | none | lsm (missing 0) |
| seq_write | raw_file | — | 5655.4 (on) / 6237.9 (off) | 86 / 64 | 626 / 624 | none | na |
| seq_write | std_map | — | 8414.7 (on) / 8470.5 (off) | 38 | 514 / 536 | none | na |
| rand_write | lsm | on | 2163.4 | 268 | 1620 | none | lsm (missing 0) |
| rand_write | lsm | off | 2119.2 | 274 | 1683 | none | lsm (missing 0) |
| rand_write | raw_pwrite | — | 4933.7 (on) / 5302.3 (off) | 88 / 91 | 836 / 600 | none | na |
| rand_write | std_map | — | 8794.1 (on) / 8087.3 (off) | 38 | 510 / 616 | none | na |
| rand_read | lsm | on | 1782.2 | 413 | 1725 | none | lsm (missing 0) |
| rand_read | lsm | off | 1705.5 | 418 | 2020 | none | lsm (missing 0) |
| rand_read | raw_pwrite | — | 5438.4 (on) / 5582.8 (off) | 86 | 728 / 591 | none | na |
| rand_read | std_map | — | 9436.2 (on) / 8855.7 (off) | 35 / 37 | 463 / 444 | none | na |
| seq_read | lsm | on | 1952.9 | 359 | 1655 | none | lsm (missing 0) |
| seq_read | lsm | off | 1999.9 | 376 | 1543 | none | lsm (missing 0) |
| seq_read | raw_file | — | 5728.0 (on) / 5612.2 (off) | 84 / 85 | 611 / 613 | none | na |
| seq_read | std_map | — | 9351.3 (on) / 8461.9 (off) | 37 | 469 / 458 | none | na |

- **`missing=0 / mismatch=0` 只对 `engine=lsm` 成立**（M5-C6）；`raw_*`/`std_map` 是 `verify=na`，
  **不参与**退出码，也不参与「数据一致」判定。
- LSM 格的 `raw1_us/raw2_us/raw3_us`（filter=on）：`seq_write 283/253/258`、`rand_write 268/266/272`、
  `rand_read 410/423/405`、`seq_read 364/357/358` —— 三次离散度 ≤5%，读格没有 N9 的 MemTable→SST 双峰。
- filter on/off 的写侧差异（2308 vs 2395 等）在 VM 噪声内；filter 不改写路径语义，差值不作结论。
- **负结果 N1**：filter 对**已存在 key** 的点查几乎没有吞吐收益（`rand_read` 1782 vs 1706，
  `seq_read` 1953 vs 2000，方向都不稳定）——这与 §6.1 一致：filter **只做否定**，只有"不存在的 key"
  才省数据块读（见 §5）。**不得**用这两列宣称 filter 提升点查吞吐。
- **负结果 N3（复现性）**：`--filter off` 的那一轮在**修正前**的脚本下 `bench_lsm.sh` 返回 **1**：
  `BENCH_REPRO_OK 0`，原因是**非 LSM 格** `seq_write/raw_file` 三轮中位数差 >25%（`repro_ok=0`）；
  同一文件里 4 个 LSM 格 `repro_ok=1`、`missing/mismatch=0`。M5.3 已把复现性判据收窄到
  `engine=lsm`（依据 M5-C6「硬门禁只施加在 LSM 腿」；非 LSM 格仍逐行打印 `repro_ok`，但不进退出码）
  ⇒ 按修正后的脚本，该轮应为 `rc=0`。**本表的数据行未变**；这条负结果保留，用来记录修正前的口径。

## 4. 三个放大 + filter 代价（`AMPL` 行 + `space_filter_bytes`）

`AMPL` 行的 **前缀列由 M4.3 冻结**，M5.3 只在**行尾追加** `read_filter_*` 与
`space_filter_bytes`/`space_sst_data_bytes`（后者由 `lsm_level_stats` 逐文件 `Table::filter_bytes()`
求和后追加，**不是**公式估算，§6.6）。

filter=on（`compaction_rounds=0`，见负结果 N7）：

```
AMPL round_id=ALL user_logical_bytes=2000000 entry_bytes=3600000 flush_write_bytes=2138613 compact_write_bytes=0 write_amp_total=1.069306 write_amp_excl_compact=1.069306 read_files_checked=167760 read_index_blocks_read=3 read_data_blocks_read=167760 read_bytes=686064528 read_get_count=200000 read_amp_files_per_get=0.838800 space_sst_bytes=2138613 space_manifest_bytes=264 space_current_bytes=2 space_log_bytes=677180 space_tmp_bytes=0 space_amp=1.408030 space_amp_sst_only=1.069306 dropped_old_versions=0 dropped_tombstones=0 compaction_rounds=0 compaction_round_p50_us=0 compaction_round_max_us=0 live_versions_max=4 read_filter_checked=167760 read_filter_negative=0 read_filter_positive=167760 read_filter_unavailable=0 read_data_blocks_skipped_by_filter=0 space_filter_bytes=109401 space_sst_data_bytes=2029212
LEVEL round_id=ALL ... total_sst_files=3 total_sst_bytes=2138613 ...
FRONT round_id=ALL ops=300000 get_ops=200000 p50_us=48 p99_us=756 p999_us=8018 max_us=46
SPACE round_id=ALL SPACE_SST_FILES 3 SPACE_SST_BYTES 2138613 SPACE_FILTER_BYTES 109401 SPACE_SST_DATA_BYTES 2029212 SPACE_FILTER_FILES_OK 3 SPACE_FILTER_FILES_ABSENT 0 SPACE_FILTER_FILES_CORRUPT 0
```

（以上 `FRONT` 行为 **idle 复测**：串行、无并发重活、`load1=1.32`；AMPL 行的 `compaction_rounds=0`
⇒ 其与时间无关的列与同轮一致，`FRONT` 的 p50/p99 与同轮差 ≤5%。）

filter=off（**修正后**重跑的子轮：`lsm_ampl_probe --bloom-bits 0`；见负结果 N6 的登记）：

```
AMPL round_id=ALL user_logical_bytes=2000000 entry_bytes=3600000 flush_write_bytes=2029026 compact_write_bytes=0 write_amp_total=1.014513 write_amp_excl_compact=1.014513 read_files_checked=167760 read_index_blocks_read=3 read_data_blocks_read=167760 read_bytes=686064528 read_get_count=200000 read_amp_files_per_get=0.838800 space_sst_bytes=2029026 space_manifest_bytes=264 space_current_bytes=2 space_log_bytes=677180 space_tmp_bytes=0 space_amp=1.353236 space_amp_sst_only=1.014513 dropped_old_versions=0 dropped_tombstones=0 compaction_rounds=0 compaction_round_p50_us=0 compaction_round_max_us=0 live_versions_max=4 read_filter_checked=0 read_filter_negative=0 read_filter_positive=0 read_filter_unavailable=167760 read_data_blocks_skipped_by_filter=0 space_filter_bytes=0 space_sst_data_bytes=2029026
LEVEL round_id=ALL ... total_sst_files=3 total_sst_bytes=2029026 ...
FRONT round_id=ALL ops=300000 get_ops=200000 p50_us=47 p99_us=1000 p999_us=11347 max_us=49
SPACE round_id=ALL SPACE_SST_FILES 3 SPACE_SST_BYTES 2029026 SPACE_FILTER_BYTES 0 SPACE_SST_DATA_BYTES 2029026 SPACE_FILTER_FILES_OK 0 SPACE_FILTER_FILES_ABSENT 3 SPACE_FILTER_FILES_CORRUPT 0
```

（同样为 idle 复测；`FRONT` p50 47 us vs 同轮 48 us。）

- **可复算（M5-B04）**：`space_sst_bytes == space_filter_bytes + space_sst_data_bytes`
  （on：2138613 = 109401 + 2029212；off：2029026 = 0 + 2029026），且这份等式是**跨来源核对**：
  左边来自 `AmplificationStats`（内存统计），右边来自逐文件实测。
- **filter 的空间/写代价（负结果 N2）**：SST 字节 +109587（**+5.40%**），写放大 1.014513 → 1.069306
  （+5.4%）。在"只读已存在 key"的负载下 `read_filter_negative=0`、`data_blocks_skipped_by_filter=0`
  ⇒ 省下的块读为 0，而 filter 体积与构建成本都有 ⇒ 该负载下 `negative_result=NET_LOSS`。
- 假阳性率（M5-A02，同轮打印）：`M5_FILTER_FPR_PPM 8310`（0.831%）≤ 2.0% 判据；结构化分布
  （`even_keys`/`bucket`）的对照值见 §8 未闭合项（负结果性质）。
- **`FRONT` 的时间口径（D-16，沿用 M4）**：Put 侧只有"当选 flusher 的那个写者"被采样
  （`front_samples_us_` 仍是**一个组一个样本**，M5.3 未改），因此 `FRONT.p50_us` 对 Put 是**下界近似**；
  本表不使用 Put 侧 FRONT 作结论（四类负载的写延迟以 CELL 行为准），Get 侧是精确端到端。
- **`AMPL` 子轮与 CELL 格是两个不同的 DB 实例**（`AMPL_PARAMS` 行声明），禁止把子轮的写放大与
  CELL 格吞吐拼成一句结论；子轮默认 `--rounds 1`。

## 5. 「>=3x」的同轮开关对照（只限不存在的 key）

`Filter.BlockReadReductionAtLeastThreeTimes`（M5-A10）在**同一进程、同一数据集、同一不存在 key 集合**
内比较 `bloom_bits=0` vs `10` 的 `data_blocks_read`（`read_data_blocks_read`，M5.1 修正后才有意义，见
`docs/amplification.md` §6）：

```
M5_FILTER_BLOCK_READS_WITHOUT 2000
M5_FILTER_BLOCK_READS_WITH 18
M5_FILTER_BLOCK_READ_RATIO 111.111111
M5_FILTER_BLOCKS_SKIPPED 1982
M5_FILTER_FALSE_NEGATIVE 0
```

- 判据 `without >= 3` 且 `without/max(1,with) >= 3.0` ⇒ 111.1×，**通过**。
- **作用域**：只针对"不存在的 key 的数据块读次数"。**禁止**把它外推成"读吞吐提升 111×"或
  "点查提升 N 倍"——同一份 filter 对已存在的 key 不省块读（§3 负结果 N1）。`bloom_bits=0` 的对照
  数据即上表的 `WITHOUT` 一列，原文保留。

## 6. 参数矩阵的补充实测（sync / batch）

`sync`（LSM-only，N=2000，repeats=1，`--no-ampl`）：

| load | sync=0 (ops/s) | sync=1 (ops/s) | 比值 |
|---|---|---|---|
| seq_write | 2116.9 | 125.3 | 16.9× |
| rand_write | 2016.4 | 134.6 | 15.0× |
| rand_read | 3650.8 | 3681.8 | 1.00× |
| seq_read | 3609.5 | 3241.2 | 1.11× |

⇒ **负结果 N5 的落地**：`sync=true` 的写吞吐被 fsync 物理成本限制（125~135 ops/s ↔ 8.08 ms/次，idle 复测），
读不受影响；`sync=false` 的 `missing 0` **只**证进程级一致性（kill -9 不丢 page cache），**不证掉电安全**。

`batch`（LSM-only，N=20000，repeats=1，sync=0，`--no-ampl`；写侧 `latency_unit=call`）：

| load | K=1 (ops/s) | K=16 (ops/s) | K=256 (ops/s) |
|---|---|---|---|
| seq_write | 2456.1 | 29575.8 | 129902.2 |
| rand_write | 2504.0 | 24537.0 | 91305.0 |
| rand_read | 1786.6 | 1890.5 | 1807.6 |
| seq_read | 2043.6 | 1781.0 | 1932.7 |

- **负结果 N4**：设计 §8.2 的候选 2 预期"batch 很大时吞吐不升反降"，本机在 K≤256、N=20000 上**没有**
  观察到下降，反而单调上升（写侧 max ~53×）。**如实登记为"预期未被实测支持"**；限制：K>1 时
  `LATENCY/P99` 是**每次 `DB::Write` 调用**的（`latency_unit=call`），只有 entries/s 与 K=1 可比；
  未做 K≥1024 或接近 `kMaxGroupBytes=1 MiB` 的边界。
- 读侧 K 不影响（读恒为一次 Get 一个 op；M5.3 修掉了首版"读也被按 batch 折算"的驱动缺陷，
  见 §8 未闭合项 4 的登记）。

## 7. LSM 的劣势场景（≥3 条，逐条有数据）

1. **点查不存在的 key、filter 关闭**：块读次数 2000 vs 18（111×，M5-A10）。这是 LSM 在"负查询"上
   最贵的场景，也是 filter 唯一的收益来源（§5）。
2. **`sync=true` 的写吞吐受 fsync 限制**：125~135 ops/s vs `sync=false` 2016~2117 ops/s（15~17×）；
   fsync median 8.084 ms（idle 复测，§2）。写放大在 sync=0 下是 1.07，但延迟被物理 fsync 支配。
3. **空间放大含 filter 后上升**：SST 字节 +5.40%（+109587 B）；`space_amp` 1.353 → 1.408。
4. **`std::map` 在纯内存路径明显更快且完全不持久**：seq_write 8415 vs LSM 2308（3.6×）、
   rand_read 9436 vs LSM 1782（5.3×）；`durability=none`，**不是** LSM 的目标（§6.2 公平性声明）。
5. **长尾**：LSM 读 P99 1543~2020 us vs raw 档 591~728 us（~2.7×）。compaction 发生轮的长尾
   **未覆盖**（本轮 `compaction_rounds=0`，见负结果 N7）。

## 8. 负结果与未闭合（原文保留）

- **N1** filter 对已存在 key 的点查无吞吐收益（数据见 §3）。
- **N2** filter 的空间/写代价 +5.40%，而"只读已存在 key"负载下省下的块读为 0 ⇒ `NET_LOSS`（§4）。
- **N3** `--filter off` 全流程 `rc=1`（非 LSM 格 `seq_write/raw_file` 复现性超阈）；LSM 格全绿 ≠ 整轮全绿（§3）。
- **N4** 设计预期的"batch 大反而慢"未被实测支持（K≤256 单调上升）；K>1 的延迟单位是 call（§6）。
- **N5** fsync 成本随机器/时间漂移：本轮 idle 复测 median 8.084 ms（同轮 7.733/7.943 ms）vs M2/M3 的
  2.5~3.2 ms；已确认不是并发负载造成（复测 load1=1.32），但**不得当常数**（§2）。
- **N6** AMPL 子轮与 CELL 格是**两个不同的 DB 实例**（脚本用 `AMPL_PARAMS` 行声明）；子轮默认
  `--rounds 1` 且只读已存在 key ⇒ `read_filter_negative=0`。首版 `bench_lsm.sh` 的子轮**恒用
  `bloom_bits=10`**、与 `--filter off` 不一致；M5.3 已给 `lsm_ampl_probe` 加 `--bloom-bits` 并由
  `bench_lsm.sh` 透传（filter=off 时 `space_filter_bytes` 必须为 0，脚本已加硬校验），off 的
  `AMPL/SPACE` 行按修正后重跑（§4 下半段）。**`/tmp/lsm_bench_off.txt` 里那一行 AMPL 是修正前的
  旧口径，已被本节的修正行取代**——原文保留、不删除。
- **N7** `compaction_rounds=0`：本轮 L0 只有 3 个 SST（`level0_file_num_compaction_trigger=4`），
  **没有发生 compaction** ⇒ 写放大 1.07 与 `space_amp` 1.41 都**不是稳态**；设计 §8.1 的
  "compaction 长尾/稳态写放大"未覆盖。
- **N8** `has_filter=off` 时的 `read_filter_unavailable=167760` 是"按可能存在处理"的计数（不是错误）；
  该列在 off 下**必然**等于 `read_get_count` 量级，解读时不要当成损坏。
- **N9（门禁腿的计时窗口与复现性口径）**：`bench_lsm.sh` 的首版在 20000-key + 256 KiB 写缓冲下，
  读格的计时窗口横跨了「后台 flush/compaction 完成前后」的 MemTable→SST 切换 ⇒ 同一格两轮
  `raw1_us/raw2_us` 出现 122↔522 的双峰，`BENCH_REPRO_OK` 误报 0。处置（**不降判据**）：
  ① M5-C 门禁腿把 **CELL** 的 `write_buffer_size` 取 16 MiB（20000-key 稳定落在 MemTable 内），
  **AMPL 子轮**单独 `--ampl-write-buffer-size 262144` 以强制产生 SST（`space_filter_bytes>0`）；
  ② 复现性判据按 M5-C6 收窄到 `engine=lsm`，并新增 `BENCH_REPRO_LSM_CELLS` 正向计数；
  ③ 连跑 3 次同参数门禁腿：3/3 `rc=0`、`BENCH_REPRO_OK 1`、`BENCH_REPRO_LSM_CELLS 4`（零 flaky）。
  100k 的文档数据未受影响（该规模下 LSM 读 `raw1/2/3` 离散度 ≤5%，见 §3 表下的原始值）。

## 9. 证据命令（可复现）

```bash
cd ~/lsm-kv
bash scripts/lsm_build.sh                                   # 干净重建 + 0 warning + 205/205
# idle 复测（串行、无并发重活；本轮 load1=1.32）：
./build/bin/fsbench_commit_latency /tmp/lsm_fsb_idle 500     # §2（median 8.084 ms）
./build/bin/lsm_ampl_probe --db /tmp/lsm_m53_ampl_on_idle  --rounds 1 --keys 100000 \
     --write-buffer-size 4194304 --strategy round_robin --bloom-bits 10   # §4 的 FRONT/AMPL（on）
./build/bin/lsm_ampl_probe --db /tmp/lsm_m53_ampl_off_idle --rounds 1 --keys 100000 \
     --write-buffer-size 4194304 --strategy round_robin --bloom-bits 0    # §4 的 FRONT/AMPL（off）
./build/bin/lsm_level_stats --db /tmp/lsm_m53_ampl_on_idle   # space_filter_bytes 追加列
# 文档轮（§3 的 CELL 绝对吞吐来自这一轮；头行 load1=2.96）：
bash scripts/bench_lsm.sh --dataset 100000 --value-size 100 --batch 1 --pipeline 1 --sync 0 \
     --filter on  --repeats 3 --out /tmp/lsm_bench_on.txt   # §3/§4（rc=0）
bash scripts/bench_lsm.sh --dataset 100000 --value-size 100 --batch 1 --pipeline 1 --sync 0 \
     --filter off --repeats 3 --out /tmp/lsm_bench_off.txt  # §3/§4（修正前脚本 rc=1，见 N3）
bash scripts/bench_lsm_selftest.sh                          # 失败注入自测 rc=1 + [BENCH_SELFTEST_OK]
bash scripts/lsm_filter_damage_test.sh --cases 2000         # 磁盘 filter 损坏扫描
./build/bin/lsm_tests --gtest_filter='Filter.BlockReadReductionAtLeastThreeTimes'   # §5
# 门禁腿的形状（M5-C 的 CELL 用 16 MiB，AMPL 子轮用 256 KiB；见 N9）：
bash scripts/bench_lsm.sh --dataset 20000 --value-size 100 --batch 1 --pipeline 1 --sync 0 \
     --filter on --repeats 2 --warmup 1000 --write-buffer-size 16777216 \
     --ampl-write-buffer-size 262144 --out /tmp/lsm_bench_gate.txt
```
