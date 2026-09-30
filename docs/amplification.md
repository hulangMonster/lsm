# 放大口径与实测（docs/amplification.md）

> 口径来源：`docs/m4-design.md` §10.3（固定行格式）。三行都用 `KEY=VALUE` 空格分隔；
> **前缀列冻结、只允许行尾追加**（I45 的可复现性）。实测数字来自 `scripts/lsm_compaction_stress.sh`
> （驱动 = `scripts/lsm_ampl_probe`，真实磁盘 `/tmp/lsm_stress_*`，`--rounds 3 --keys 2000 --write-buffer-size 16384`）。

## 1. 口径定义

| 口径 | 分子 | 分母 | 窗口 |
|---|---|---|---|
| 写放大（主）`write_amp_total` | `flush_write_bytes + compact_write_bytes`（写进 `.sst`/MANIFEST 的字节，不含 WAL） | `user_logical_bytes = Σ(key.size+value.size)` | ALL / 单轮 |
| 写放大（对照）`write_amp_excl_compact` | `flush_write_bytes` | 同上 | ALL / 单轮 |
| 读放大 `read_amp_files_per_get` | `files_checked`（只数真的进了 `Table::Get` 的文件）+ 索引/数据块计数、字节 | `read_get_count` | ALL |
| 空间放大（主）`space_amp` | `sst + manifest + current + log + tmp`（含临时文件/孤儿残留） | `user_logical_bytes` | ALL 末态 |
| 空间放大（对照）`space_amp_sst_only` | `sst_bytes` | 同上 | ALL 末态 |
| 前台延迟 | `FRONT` 行的 `p50_us/p99_us/p999_us/max_us`（前台 Put 批结算 + Get 端到端） | — | ALL |
| compaction 单轮 | `AMPL.compaction_round_p50_us` = **多轮采样**的 p50（样本数 = `ROUND_SAMPLES`；不是单轮近似） | — | ALL |

## 2. 实测（round_robin，ALL）

```
AMPL round_id=ALL user_logical_bytes=120000 entry_bytes=216000 flush_write_bytes=150259 compact_write_bytes=1591095 write_amp_total=14.511283 write_amp_excl_compact=1.252158 read_files_checked=7869 read_index_blocks_read=27 read_data_blocks_read=0 read_bytes=27817250 read_get_count=8000 read_amp_files_per_get=0.983625 space_sst_bytes=56513 space_manifest_bytes=7661 space_current_bytes=2 space_log_bytes=4830 space_tmp_bytes=0 space_amp=0.575050 space_amp_sst_only=0.470942 dropped_old_versions=3671 dropped_tombstones=0 compaction_rounds=47 compaction_round_p50_us=40131 compaction_round_max_us=63892 live_versions_max=6
LEVEL round_id=ALL l0_files=3 l0_bytes=8049 l1_files=1 l1_bytes=48464 l2_files=0 l2_bytes=0 l3_files=0 l3_bytes=0 l4_files=0 l5_files=0 l6_files=0 total_sst_files=4 total_sst_bytes=56513 l0_score=0.750000 l1_score=0.004622 l2_score=0.000000 l3_score=0.000000 l4_score=0.000000 l5_score=0.000000 l6_score=0.000000
FRONT round_id=ALL ops=14000 get_ops=8000 p50_us=246 p99_us=1294 p999_us=30078 max_us=444
MISSING 0 MISMATCH 0 COMPACTION_ROUNDS_TOTAL 47 MANIFEST_ROLLS 1 MANIFEST_BYTES 7661 LIVE_VERSIONS_MAX 6 READ_FILES_CHECKED_P50 1 READ_FILES_CHECKED_MAX 1 READ_BASELINE_M3 59 FD_GROWTH 1 ROUND_SAMPLES 47
B05_OK 1 B06_OK 1 B07_OK 1
```

## 3. 读放大改善（B06 的判据）

- `READ_FILES_CHECKED_P50 = 1` ≤ 3、`READ_FILES_CHECKED_MAX = 1` ≤ 12；
- M3 基线的对照值 59（`docs/m3-evidence.md` 登记的 F≈59 全量扫 L0）⇒ 结构性改善成立；
- 与 A19 的上界（`trigger + 2 + (kNumLevels-1)`）一致：稳态下 L0 ≤ trigger、L1+ 每层至多查 1 个文件。

## 4. 已知偏差与未闭合

1. **写放大 14.5 偏高**：`--write-buffer-size 16384` 是刻意的小缓冲（快速触发 compaction），
   每轮 flush 的 L0 文件很小、compaction 47 轮反复重写；这不是稳态数字。
2. **空间放大 < 1**：`user_logical_bytes` 统计了 3 轮覆盖写的全部字节（120000），而末态只保留最新版本（56513）
   ⇒ 比值天然 < 1；口径本身正确（分子是末态、分母是累计逻辑写量）。
3. **`live_versions_max = 6` 高于设计 §10.2 B08 示例的"≤4"**：live 集合含 current + 若干正在被
   compaction/读路径持有的旧版本，`MaybeDeleteObsoleteFiles` 在每次安装后回收 refs==0 的条目。
   6 是**有界**的，但与"≤4"的示例阈值不一致 ⇒ **保留用例与数字、交用户裁决**（不删弱、不调门禁）。
4. **B01/B02（compaction 中途 kill -9 的进程级对账）未实现**：需要 `CompactionHook` 的四个注入点 +
   子进程驱动；本轮未做，门禁未接这两条腿。
5. 策略对照（B03）目前只打印 `STRATEGY` 行与两个策略各自的一行；**未给"结论作废/优劣"的定性行**。

## 5. 采样条件与"观测 vs 门禁"（用户裁决，2026-xx）

- `LIVE_VERSIONS_MAX = 6` 的采样条件：真实磁盘、`--rounds 3 --keys 2000 --write-buffer-size 16384`、
  `level0_file_num_compaction_trigger = 4`（默认）、单 compaction 线程 + 单 flush 线程、
  本轮共 47 轮 compaction。它是**观测值，不是门禁阈值**：设计 §10.2 B08 的示例"≤4"是说明性数字，
  门禁腿（M4-B08）只断言"该计数存在且被打印"。**结论：保留 6，不为凑 4 改代码**；
  若 `docs/m4-design.md` 把 4 写成硬阈值，应由设计侧补 R 记录降级为观测/告警级（本文件不擅自改设计）。
  6 的构成（当轮）：current 1 + 正在被 compaction/读路径持有的旧版本（`refs() > 0`）若干；
  `MaybeDeleteObsoleteFiles` 在每次安装后回收 `refs()==0` 的条目，故它**有界**。

- `FRONT` 行的 **Put 侧是近似口径**：一次组提交里只有"当选 flusher 的那个写者"真正跑了
  `RunFlusher`，其延迟被采样；同组其他写者的端到端延迟 ≥ 该值（它们多等了一个 commit_cv_ 唤醒）。
  因此 `FRONT.p50_us` 对 Put 而言是**下界近似**（误差方向：偏小），Get 侧是精确的端到端。
  行格式不为此单列字段（§10.3 冻结的前缀列），此说明即为口径披露。
