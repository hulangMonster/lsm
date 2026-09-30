#!/usr/bin/env bash
# scripts/lsm_gate.sh —— M2+M3 全部门禁的唯一入口（docs/m2-prerequisites.md §8 + docs/m3-design.md §11.3）
#
# 为什么需要它：M2 的验收由多条互相独立的门禁组成（干净重建 / ASan / TSan / 崩溃对账 /
# 截断扫描 / 中间损坏）。分散跑容易漏，评审者也难以确认「到底跑了哪些」。
#
# 本版（M3 起）新增的核心机制：**正向标记判定**（run_gate_marked）。
# 起因是 docs/m3-prerequisites.md §9 的 D9.6：旧版 run_gate 只看退出码，而"什么都没发生也算成功"
# 会让门禁变成**空绿**（M2 已经踩过一次：`$((...+M))` 在 M 为空时静默跳过检查）。
#   ⇒ 凡是有"必须真的发生过某件事"语义的腿，都必须改为：命令成功 **且** 输出里出现预期标记。
#   每条腿用**独立日志文件**判定，避免标记在上一条腿的输出里被误匹配。
#   M3 的腿在脚本尚未交付时打印 [SKIP] 并计入"未验证"；加 --require-m3 时 SKIP 直接判 FAIL。
#
#   --rounds N     崩溃对账轮数（默认 100）
#   --with-tsan    额外跑 TSan（默认关闭：全量在 TSan 下约 6 分钟）
#   --no-asan      跳过 ASan
#   --require-m3   要求 M3 的腿**必须存在且通过**（缺一即 FAIL）；默认缺失记为 SKIP
#   --require-m5   要求 M5 的腿**必须存在且通过**（缺一即 FAIL）；默认缺失记为 SKIP（M5.1 追加）
set -u
cd "$(dirname "$0")/.."
ROUNDS=100
WITH_TSAN=0
WITH_ASAN=1
REQUIRE_M3=0
REQUIRE_M5=0
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --with-tsan) WITH_TSAN=1; shift;;
    --no-asan) WITH_ASAN=0; shift;;
    --require-m3) REQUIRE_M3=1; shift;;
    --require-m5) REQUIRE_M5=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
LOGDIR=$(mktemp -d /tmp/lsm_gate_XXXXXX)
LOG="$LOGDIR/gate.log"
FAILED=0
SKIPPED=0
declare -a RESULTS

run_gate() {
  local name="$1"; shift
  echo "=== [gate] $name ===" | tee -a "$LOG"
  local leg="$LOGDIR/leg_$(echo "$name" | tr -c 'A-Za-z0-9' '_').log"
  if "$@" >"$leg" 2>&1; then
    cat "$leg" >>"$LOG"
    RESULTS+=("PASS  $name")
    echo "--- [PASS] $name"
  else
    cat "$leg" >>"$LOG"
    RESULTS+=("FAIL  $name")
    FAILED=1
    echo "--- [FAIL] $name（详见 $LOG）"
    tail -20 "$leg"
  fi
}

# 正向标记版：命令成功 **且** 这条腿自己的输出里匹配到全部标记（标记之间用 @@ 分隔，AND 语义）。
# 为什么必须 AND：`MISSING 0` 这种标记会在"100 轮里坏 1 轮"时命中别的好轮次 ⇒ 假通过。
# 因此一律用**收尾汇总行**里的总量标记（如 `MISSING_TOTAL 0 MISMATCH_TOTAL 0`），并配一个正向计数
# （如 `ROUNDS_OK 1[0-9]*`）确保"真的跑了"。
run_gate_marked() {
  local name="$1"; local marker_spec="$2"; shift 2
  echo "=== [gate] $name （要求标记 /$marker_spec/）===" | tee -a "$LOG"
  local leg="$LOGDIR/marked_$(echo "$name" | tr -c 'A-Za-z0-9' '_').log"
  local ok=1
  "$@" >"$leg" 2>&1 || ok=0
  local m
  while IFS= read -r m; do
    [ -z "$m" ] && continue
    grep -Eq "$m" "$leg" || { ok=0; echo "--- [gate] 缺少标记 /$m/" | tee -a "$LOG"; }
  done < <(printf '%s' "$marker_spec" | tr '@' '\n' | grep -v '^$')
  if [ "$ok" = "1" ]; then
    cat "$leg" >>"$LOG"
    RESULTS+=("PASS  $name")
    echo "--- [PASS] $name"
  else
    cat "$leg" >>"$LOG"
    RESULTS+=("FAIL  $name（退出码或正向标记 /$marker_spec/ 不满足）")
    FAILED=1
    echo "--- [FAIL] $name（正向标记未全部出现或命令失败，详见 $LOG）"
    tail -20 "$leg"
  fi
}

# M3 的腿：脚本尚未交付时记 SKIP（绝不静默变绿）；--require-m3 时记 FAIL。
run_gate_m3_marked() {
  local name="$1"; local script="$2"; local marker="$3"; shift 3
  if [ ! -f "$script" ]; then
    if [ "$REQUIRE_M3" = "1" ]; then
      RESULTS+=("FAIL  $name（缺 $script）")
      FAILED=1
      echo "=== [gate] $name ===
--- [FAIL] $name：$script 不存在（--require-m3 要求必须存在）"
    else
      RESULTS+=("SKIP  $name（$script 尚未交付）")
      SKIPPED=$((SKIPPED + 1))
      echo "=== [gate] $name ===
--- [SKIP] $name：$script 尚未交付（M3.3）；加 --require-m3 可把它变成硬失败"
    fi
    return
  fi
  run_gate_marked "$name" "$marker" bash "$script" "$@"
}

# M5 的腿（M5.1 追加）：与 run_gate_m3_marked 同形 —— 脚本缺失时默认记 SKIP（绝不静默变绿），
# 加 --require-m5 时记 FAIL。**既有 M2/M3/M4 的 18 条腿一行未动。**
run_gate_m5_marked() {
  local name="$1"; local script="$2"; local marker="$3"; shift 3
  if [ ! -f "$script" ]; then
    if [ "$REQUIRE_M5" = "1" ]; then
      RESULTS+=("FAIL  $name（缺 $script）")
      FAILED=1
      echo "=== [gate] $name ===
--- [FAIL] $name：$script 不存在（--require-m5 要求必须存在）"
    else
      RESULTS+=("SKIP  $name（$script 尚未交付）")
      SKIPPED=$((SKIPPED + 1))
      echo "=== [gate] $name ===
--- [SKIP] $name：$script 尚未交付；加 --require-m5 可把它变成硬失败"
    fi
    return
  fi
  run_gate_marked "$name" "$marker" bash "$script" "$@"
}

echo "== lsm_gate: rounds=$ROUNDS asan=$WITH_ASAN tsan=$WITH_TSAN require_m3=$REQUIRE_M3 require_m5=$REQUIRE_M5 log=$LOG =="

# ---------------- M2 腿（不得退化；全部改为"退出码 + 正向标记"） ----------------
run_gate_marked "干净重建 + 0 warning + 全量用例" '\[  PASSED  \] [1-9][0-9]* tests' bash scripts/lsm_build.sh
if [ "$WITH_ASAN" = "1" ]; then
  run_gate_marked "ASan 全量" '\[  PASSED  \] [1-9][0-9]* tests' bash -c \
    'cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-asan -j8 >/dev/null && ./build-asan/bin/lsm_tests'
fi
if [ "$WITH_TSAN" = "1" ]; then
  # TSan 的判据是"零 race"，属于**反向**判据：退出码为 0 且报告数为 0；这里用退出码，
  # 报告数由 verify 脚本单独 grep（见 scripts/verify_m3_1.sh 的 F 段）。
  run_gate_marked "TSan 全量（setarch 关 ASLR）" '\[  PASSED  \] [1-9][0-9]* tests' bash -c \
    'cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-tsan -j8 >/dev/null && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests'
fi
run_gate_marked "崩溃对账（kill -9 x $ROUNDS，sync 模式）" \
  'MISSING_TOTAL 0 MISMATCH_TOTAL 0@@ROUNDS_OK [1-9][0-9]*' \
  bash scripts/lsm_crash_test.sh --rounds "$ROUNDS" --mode sync
run_gate_marked "逐字节截断扫描（B03）" 'TAIL_OK [1-9][0-9]* TAIL_FAIL 0' bash scripts/lsm_tail_truncate_test.sh
run_gate_marked "中间损坏拒绝启动（B04）" 'MIDDLE_OPEN_CORRUPTION [1-9]' bash scripts/lsm_corrupt_middle_test.sh

# ---------------- M3 腿（docs/m3-design.md §10.2 的 B01/B03/B04/B05） ----------------
# ① B01：flush 崩溃对账。必须证明"真的发生过 flush（SST 文件 > 0）且真的删过 log"，
#    否则就是空绿 —— 这正是 M2 评审阻断项 ③ 的同类形态。
run_gate_m3_marked "M3-B01 flush 崩溃对账（kill -9 x $ROUNDS）" scripts/lsm_flush_crash_test.sh \
  'SST_FILES_TOTAL [1-9][0-9]*@@LOGS_DELETED_TOTAL [1-9][0-9]*@@MISSING_TOTAL 0@@\[FLUSH_CRASH_OK\]' \
  --rounds "$ROUNDS" --write-buffer-size 262144
# ② B03：落盘重启（仅靠 SSTable 可读）
run_gate_m3_marked "M3-B03 落盘重启（records_replayed == 0）" scripts/lsm_flush_restart_test.sh \
  'RECORDS_REPLAYED 0@@\[FLUSH_RESTART_OK\]' --write-buffer-size 262144
# ③ B04：SSTable 单字节翻转扫描（零静默错值）
run_gate_m3_marked "M3-B04 SSTable 损坏扫描（零静默错值）" scripts/lsm_sst_damage_test.sh \
  'SST_DAMAGE_CASES [1-9][0-9]*@@SILENT_WRONG 0' --cases 2000
# ④ B05：文件句柄不泄漏
run_gate_m3_marked "M3-B05 句柄计数不增长" scripts/lsm_fd_leak_test.sh \
  'FD_GROWTH [0-9]+'

# ---------------- M4 腿（docs/m4-design.md §10.2 M4-B11）----------------
# M4.1 的正向标记腿：A 组 M4.1 用例全绿 + lsm_version 零越权依赖（缺脚本 ⇒ SKIP + [PARTIAL]）。
run_gate_m3_marked "M4-B11 MANIFEST/VersionEdit A 组 + 零依赖" scripts/lsm_manifest_test.sh \
  'M4_TESTS_RAN [1-9][0-9]*@@M4_TESTS_FAILED 0@@LSM_VERSION_FORBIDDEN 0@@\[MANIFEST_OK\]'

# ---------------- M4 腿（M4.3 B03/B05/B06/B07/B08/B09/B10）----------------
run_gate_m3_marked "M4-B03 两种 pick 策略对照" scripts/lsm_compaction_stress.sh \
  'M4-B03 STRATEGY_TABLE@@\[COMPACTION_STRATEGY_OK\]@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B05 句柄上限（fd 不增长）" scripts/lsm_compaction_stress.sh \
  'B05_OK 1@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B06 读放大改善（p50<=3 max<=12）" scripts/lsm_compaction_stress.sh \
  'READ_FILES_CHECKED_P50 [0-3]@@READ_FILES_CHECKED_MAX ([0-9]|1[0-2])@@B06_OK 1@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B07 前台 P99 与单轮 P50 量级分离" scripts/lsm_compaction_stress.sh \
  'B07_OK 1@@ROUND_SAMPLES [1-9][0-9]*@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B08 存活 Version 数有界（计数存在）" scripts/lsm_compaction_stress.sh \
  'LIVE_VERSIONS_MAX [0-9]+@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B09 MANIFEST 体积/重建计数" scripts/lsm_compaction_stress.sh \
  'MANIFEST_BYTES [0-9]+@@MANIFEST_ROLLS [0-9]+@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B10 块缓存 NOT_APPLICABLE" scripts/lsm_compaction_stress.sh \
  'M4-B10 NOT_APPLICABLE@@\[COMPACTION_STRESS_OK\]'
run_gate_m3_marked "M4-B01 compaction 中途 kill -9 对账" scripts/lsm_compaction_crash_test.sh \
  'COMPACTION_ROUNDS_TOTAL [1-9][0-9]*@@SST_FILES_TOTAL [1-9][0-9]*@@MISSING_TOTAL 0@@MISMATCH_TOTAL 0@@ROUNDS_OK [1-9][0-9]*@@\[COMPACTION_CRASH_OK\]' --mode b01
run_gate_m3_marked "M4-B02 四注入点 raise(SIGKILL) 对账" scripts/lsm_compaction_crash_test.sh \
  'INJECT_POINTS_OK 4@@MISSING_TOTAL 0@@REF_MISSING_TOTAL 0@@OPEN_CORRUPTION_TOTAL 0@@\[COMPACTION_INJECT_OK\]' --mode b02

# ---------------- M5 腿（docs/m5-design.md §7.5 / §10.3；M5.1 追加）----------------
# M5.1：Bloom filter + filter block + metaindex + 读路径否定 + 计数器 + 单字节翻转扫描。
# 标记全部取自 run_gate_m5_marked 的**收尾汇总行**（AND 语义），且含正向计数（M5_TESTS_RAN）防空绿。
run_gate_m5_marked "M5-B11 filter 单元 + 块读下降 + 零假阴性 + 依赖纪律" scripts/lsm_m5_unit_test.sh \
  'M5_TESTS_FAILED 0@@M5_FILTER_RAN [1-9][0-9]*@@M5_FILTER_FALSE_NEGATIVE 0@@M5_FILTER_SILENT_FALSE_NEGATIVE 0@@M5_FILTER_BLOCK_READS_WITHOUT [1-9][0-9]*@@M5_FILTER_BLOCKS_SKIPPED [1-9][0-9]*@@M5_FILTER_DAMAGE_CASES [1-9][0-9]*@@M5_FILTER_DAMAGE_FILTER_DEGRADED [1-9][0-9]*@@LSM_SSTABLE_FORBIDDEN 0@@\[FILTER_OK\]@@\[FILTER_DAMAGE_OK\]'

# M5.2（docs/m5-design.md §11 M5.2 / §10.1 的 M5-A11~A17）：
# WriteBatch 编码 + 批提交 + WAL 一次写 + 崩溃原子性 + 依赖纪律。
run_gate_m5_marked "M5-B12 WriteBatch 单元 + 整批原子 + WAL 一次写 + 依赖纪律" scripts/lsm_batch_unit_test.sh \
  'M5_BATCH_TESTS_FAILED 0@@M5_BATCH_TESTS_RAN [1-9][0-9]*@@M5_BATCH_PARTIAL_VISIBLE 0@@M5_BATCH_HALF_VISIBLE 0@@M5_BATCH_CRASH_HALF_VISIBLE 0@@M5_BATCH_LOST_WAKEUPS 0@@M5_BATCH_ONE_RECORD_PER_BATCH 1@@M5_BATCH_TRUNCATE_CASES [1-9][0-9]*@@M5_BATCH_CONCURRENT_WRITERS [1-9][0-9]*@@M5_BATCH_GROUP_FSYNCS [1-9][0-9]*@@M5_BATCH_RECOVERY_RECORDS [1-9][0-9]*@@LSM_BATCH_FORBIDDEN 0@@\[BATCH_OK\]'

# M5-B01（docs/m5-design.md §10.2）：kill -9 落在批写入中途 —— 已 ack 的批不得丢、不得出现半批。
# 只证明**进程级**一致性（kill -9 不丢 page cache）；掉电语义由 M5-B12 的 MemEnv 用例承担。
run_gate_m5_marked "M5-B01 批崩溃对账（kill -9 批写入中途）" scripts/lsm_batch_crash_test.sh \
  'BATCH_KILL9_MISSING 0@@BATCH_MISMATCH 0@@BATCH_HALF_VISIBLE 0@@BATCH_KILL9_ROUNDS [1-9][0-9]*@@BATCH_ACKED [1-9][0-9]*@@BATCHES_SEEN [1-9][0-9]*@@\[BATCH_CRASH_OK\]' \
  --rounds "$ROUNDS" --batch-size 16 --write-buffer-size 262144

# ---------------- M5.3 腿（docs/m5-design.md §7.5 的 M5-C/M5-D/M5-E；M5.3 追加）----------------
# M5-C：四类负载 × 三类对照的固定 CELL 行 + **只对 LSM 格**的 missing/mismatch 对账。
# 硬门禁只施加在 LSM 腿（M5-C6）：raw_file/raw_pwrite/std_map 的 verify=na 不参与退出码，
# 复现性判据也只对 engine=lsm 的格生效（BENCH_REPRO_LSM_CELLS 是非零的正向计数）。
# CELL 的 write_buffer_size 取 16 MiB：20000-key 数据集稳定落在 MemTable 内，避免计时窗口
# 横跨「后台 flush 完成前后」的 MemTable→SST 切换（那会让复现性判据误报，见 docs/m5-bench.md §8）；
# AMPL 子轮单独用 256 KiB 强制产生 SST（space_filter_bytes 必须 >0，M5-B04）。
run_gate_m5_marked "M5-C 基准（四类负载 × 三类对照 + AMPL/LEVEL/FRONT 追加列，硬门禁只在 LSM 腿）" scripts/bench_lsm.sh \
  'BENCH_CELLS_TOTAL [1-9][0-9]*@@BENCH_LSM_CELLS_TOTAL [1-9][0-9]*@@BENCH_MISSING_TOTAL 0@@BENCH_MISMATCH_TOTAL 0@@BENCH_REPRO_OK 1@@BENCH_REPRO_LSM_CELLS [1-9][0-9]*@@BENCH_AMPL_OK 1@@BENCH_AMPL_MISSING 0@@BENCH_SPACE_FILTER_BYTES [1-9][0-9]*@@BENCH_SPACE_SST_DATA_BYTES [0-9]+@@^AMPL round_id=ALL .*space_filter_bytes=[0-9]+ space_sst_data_bytes=[0-9]+@@^LEVEL round_id=ALL .*total_sst_files=[0-9]+@@^FRONT round_id=ALL .*p99_us=[0-9]+@@\[BENCH_LSM_OK\]' \
  --dataset 20000 --value-size 100 --batch 1 --pipeline 1 --sync 0 --filter on \
  --repeats 2 --warmup 1000 --write-buffer-size 16777216 --ampl-write-buffer-size 262144 \
  --out /tmp/lsm_bench_gate.txt

# M5-D：失败注入自测 —— bench_lsm.sh 在 missing != 0 时必须返回 1（防空绿）。
run_gate_m5_marked "M5-D 基准失败注入自测（missing!=0 ⇒ rc=1）" scripts/bench_lsm_selftest.sh \
  'BENCH_INJECT_MISSING_RC 1@@\[BENCH_SELFTEST_OK\]'

# M5-E：真实磁盘 filter 损坏扫描（含重算 CRC 的错位注入，E4 的「CRC 不是唯一防线」）。
run_gate_m5_marked "M5-E filter 磁盘损坏扫描（零静默假阴性）" scripts/lsm_filter_damage_test.sh \
  'FILTER_DAMAGE_CASES [1-9][0-9]*@@FILTER_SILENT_FALSE_NEGATIVE 0@@FILTER_SILENT_WRONG_VALUE 0@@FILTER_MISALIGN_DETECTED 1@@\[FILTER_DAMAGE_OK\]' \
  --cases 2000

echo
echo "==== lsm_gate 汇总 ===="
for line in "${RESULTS[@]}"; do echo "$line"; done
if [ "$FAILED" -ne 0 ]; then
  echo "[FAIL] 有门禁未通过（完整日志：$LOG）"
  exit 1
fi
if [ "$SKIPPED" -ne 0 ]; then
  echo "[PARTIAL] 全部已运行的腿通过，但有 $SKIPPED 条 M3/M5 腿因脚本未交付被跳过（未验证，不是通过）"
  exit 0
fi
echo "[OK] 全部门禁通过"
