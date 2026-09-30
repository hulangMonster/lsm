#!/usr/bin/env bash
# scripts/bench_lsm.sh —— M5.3 微基准门禁（docs/m5-design.md §6 / §7 的行为规格）
#
# 职责（§7.1）：解析参数 → 写结果文件头（MACHINE/PARAMS/BUILD/FSYNC_BASELINE）→ 调用
# `build/bin/bench_lsm`（四类负载 × 对照 × filter 开关，交替执行）→ **追加 M4.3 的三条诊断固定行**
# （`AMPL`/`LEVEL`/`FRONT`；`AMPL` 行尾用 `lsm_level_stats` 逐文件实测追加
#  `space_filter_bytes` / `space_sst_data_bytes`，即 §6.6 指定的落点）→ 解析固定 CELL 行 →
# 施加**只针对 LSM 腿**的对账门禁（§7.3 / M5-C6）→ 打印正向收尾标记（§7.2）。
#
# 硬门禁（退出码与标记，§7.3）：
#   * 参与对账：engine=lsm 的 missing / mismatch（**含 AMPL 子轮**的 LSM 对账）；任一非零 ⇒ rc=1；
#   * 不参与对账：raw_file / raw_pwrite / std_map（verify=na missing=na mismatch=na）；
#   * rc=0 还要求：LSM 格数 > 0、去重格数 >= --expected-min-cells、固定 token 齐全、
#     --repeats>=2 时 BENCH_REPRO_OK=1；**且**（未 --no-ampl 时）AMPL/LEVEL/FRONT 三行齐全、
#     `space_sst_bytes == space_filter_bytes + space_sst_data_bytes`（可复算）、
#     filter=on 时 `space_filter_bytes > 0`（M5-B04）；
#   * 参数错误 rc=2（与既有脚本约定一致）。
#
# AMPL/LEVEL/FRONT 的来源（登记于 docs/m5-evidence.md D-11）：这三行来自**独立子轮**
# `build/bin/lsm_ampl_probe`（它自带 DB 实例与工作负载），**不是**上面 CELL 格里的 DB 实例；
# 子轮参数逐项打印在 `AMPL_PARAMS` 行。禁止把子轮的写放大与 CELL 格的吞吐混成一句结论。
#
# 不可外推（§6.5，逐字写死）：单机/单块 ext4/VM/loopback 级；sync=false 只证进程级一致性；
#   绝对吞吐不承诺达到任何外部系统；`>=3x` 只针对「不存在 key 的数据块读次数」（由 M5-A10 承担）。
#
# 用法：
#   bash scripts/bench_lsm.sh [--dataset N] [--value-size B] [--key-dist seq|uniform]
#        [--batch K] [--pipeline P] [--sync 0|1] [--repeats R] [--warmup W] [--filter on|off]
#        [--engines all|lsm|raw|map] [--seed S] [--out FILE] [--require-lsm] [--inject-missing]
#        [--quick] [--expected-min-cells N] [--write-buffer-size B] [--repro-tol THR,P99]
#        [--ampl-keys N] [--ampl-rounds R] [--ampl-strategy round_robin|min_overlap] [--no-ampl]
set -uo pipefail
cd "$(dirname "$0")/.."

DATASET=100000
VALUE_SIZE=100
KEY_DIST=seq
BATCH=1
PIPELINE=1
SYNC=0
REPEATS=3
WARMUP=10000
FILTER=on
ENGINES=all
SEED=0x5EED2025
OUT=""
INJECT=0
QUICK=0
REQUIRE_LSM=0
EXPECTED_MIN_CELLS=12
WRITE_BUFFER_SIZE=4194304
REPRO_TOL="0.25,0.50"
AMPL_KEYS=""
AMPL_ROUNDS=1
AMPL_STRATEGY=round_robin
AMPL_WRITE_BUFFER_SIZE=""
NO_AMPL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --dataset) DATASET="$2"; shift 2;;
    --value-size) VALUE_SIZE="$2"; shift 2;;
    --key-dist) KEY_DIST="$2"; shift 2;;
    --batch) BATCH="$2"; shift 2;;
    --pipeline) PIPELINE="$2"; shift 2;;
    --sync) SYNC="$2"; shift 2;;
    --repeats) REPEATS="$2"; shift 2;;
    --warmup) WARMUP="$2"; shift 2;;
    --filter) FILTER="$2"; shift 2;;
    --engines) ENGINES="$2"; shift 2;;
    --seed) SEED="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --require-lsm) REQUIRE_LSM=1; shift;;
    --inject-missing) INJECT=1; shift;;
    --quick) QUICK=1; shift;;
    --expected-min-cells) EXPECTED_MIN_CELLS="$2"; shift 2;;
    --write-buffer-size) WRITE_BUFFER_SIZE="$2"; shift 2;;
    --repro-tol) REPRO_TOL="$2"; shift 2;;
    --ampl-keys) AMPL_KEYS="$2"; shift 2;;
    --ampl-rounds) AMPL_ROUNDS="$2"; shift 2;;
    --ampl-strategy) AMPL_STRATEGY="$2"; shift 2;;
    --ampl-write-buffer-size) AMPL_WRITE_BUFFER_SIZE="$2"; shift 2;;
    --no-ampl) NO_AMPL=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

case "$FILTER" in on) BLOOM_BITS=10;; off) BLOOM_BITS=0;; *) echo "--filter 只接受 on|off" >&2; exit 2;; esac
case "$SYNC" in 0|1) ;; *) echo "--sync 只接受 0|1" >&2; exit 2;; esac
case "$KEY_DIST" in seq|uniform) ;; *) echo "--key-dist 只接受 seq|uniform" >&2; exit 2;; esac
case "$ENGINES" in all|lsm|raw|map) ;; *) echo "--engines 只接受 all|lsm|raw|map" >&2; exit 2;; esac
case "$AMPL_STRATEGY" in round_robin|min_overlap) ;; *) echo "--ampl-strategy 只接受 round_robin|min_overlap" >&2; exit 2;; esac
[ -z "$AMPL_KEYS" ] && AMPL_KEYS="$DATASET"
[ -z "$AMPL_WRITE_BUFFER_SIZE" ] && AMPL_WRITE_BUFFER_SIZE="$WRITE_BUFFER_SIZE"
[ -z "$OUT" ] && OUT="/tmp/lsm_bench_$(date +%s).txt"

BIN=build/bin/bench_lsm
if [ ! -x "$BIN" ]; then
  echo "[FAIL] $BIN 不存在（先跑 scripts/lsm_build.sh）" >&2
  exit 1
fi

WORK=$(mktemp -d /tmp/lsm_bench_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/db" "$WORK/fsb"

# ---- 结果文件头（§6.3）----
NPROC="$(nproc 2>/dev/null || echo 0)"
read -r LOAD1 LOAD5 LOAD15 _ < /proc/loadavg 2>/dev/null || { LOAD1=0; LOAD5=0; LOAD15=0; }
KERNEL="$(uname -r)"
FS="$(findmnt -no FSTYPE -T "$WORK" 2>/dev/null || echo '?')"
MOUNT="$(findmnt -no TARGET -T "$WORK" 2>/dev/null || echo '?')"
REV="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then DIRTY=1; else DIRTY=0; fi
CXX="$(g++ --version 2>/dev/null | head -1 || echo unknown)"
FSYNC="NA"
if [ -x build/bin/fsbench_commit_latency ]; then
  FSYNC="$(./build/bin/fsbench_commit_latency "$WORK/fsb" 100 2>/dev/null \
    | sed -n 's/.*STRATEGY append+fsync.*MEDIAN_MS *\([0-9.]*\).*/\1/p' | head -1)"
  [ -z "$FSYNC" ] && FSYNC="NA"
fi

{
  echo "MACHINE nproc=$NPROC load1=$LOAD1 load5=$LOAD5 load15=$LOAD15 fs=$FS mount=$MOUNT kernel=$KERNEL"
  echo "PARAMS dataset=$DATASET value_size=$VALUE_SIZE key_dist=$KEY_DIST batch=$BATCH pipeline=$PIPELINE sync=$SYNC repeats=$REPEATS warmup=$WARMUP filter=$FILTER bloom_bits=$BLOOM_BITS seed=$SEED out=$OUT write_buffer_size=$WRITE_BUFFER_SIZE repro_tol=$REPRO_TOL"
  echo "AMPL_PARAMS ampl_keys=$AMPL_KEYS ampl_rounds=$AMPL_ROUNDS ampl_strategy=$AMPL_STRATEGY ampl_write_buffer_size=$AMPL_WRITE_BUFFER_SIZE ampl_source=build/bin/lsm_ampl_probe space_source=build/bin/lsm_level_stats"
  echo "BUILD rev=$REV dirty=$DIRTY build_type=Release cxx=$CXX"
  echo "FSYNC_BASELINE median_ms=$FSYNC source=scripts/fsbench_commit_latency.cpp"
} > "$OUT"
cat "$OUT"

ARGS=(--dataset "$DATASET" --value-size "$VALUE_SIZE" --key-dist "$KEY_DIST" --batch "$BATCH"
      --pipeline "$PIPELINE" --sync "$SYNC" --repeats "$REPEATS" --warmup "$WARMUP"
      --filter "$FILTER" --engines "$ENGINES" --seed "$SEED" --write-buffer-size "$WRITE_BUFFER_SIZE"
      --repro-tol "$REPRO_TOL" --dir "$WORK/db")
[ "$QUICK" = "1" ] && ARGS+=(--quick)
[ "$INJECT" = "1" ] && ARGS+=(--inject-missing)

RAW="$WORK/cells.raw"
"$BIN" "${ARGS[@]}" >"$RAW" 2>"$WORK/stderr"
BIN_RC=$?
cat "$RAW" >> "$OUT"
cat "$RAW"
if [ "$BIN_RC" -ne 0 ]; then
  echo "[FAIL] bench_lsm 退出码 $BIN_RC（stderr 尾部）" >&2
  tail -20 "$WORK/stderr" >&2
  exit 1
fi

# ---- 固定行解析（只统计；门禁口径见 --require-lsm / missing）----
CELL_ROWS="$(grep -c '^CELL ' "$RAW" || true)"
CELL_ROWS="${CELL_ROWS:-0}"
if [ "$CELL_ROWS" -eq 0 ]; then
  echo "[FAIL] 结果里没有任何 CELL 行（空绿）" >&2
  exit 1
fi
for tok in THROUGHPUT LATENCY P99 durability; do
  if ! grep -q "^CELL .*$tok=" "$RAW"; then
    echo "[FAIL] CELL 行缺少固定 token $tok=" >&2
    exit 1
  fi
done

DISTINCT="$(awk '/^CELL /{k=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="name"||a[1]=="engine"||a[1]=="filter"||a[1]=="batch"||a[1]=="pipeline"||a[1]=="sync") k=k"|"a[2]} if(!(k in seen)){seen[k]=1;n++}} END{print n+0}' "$RAW")"
LSM_DISTINCT="$(awk '/^CELL /{eng="";k=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="engine")eng=a[2]; if(a[1]=="name"||a[1]=="engine"||a[1]=="filter"||a[1]=="batch"||a[1]=="pipeline"||a[1]=="sync") k=k"|"a[2]} if(eng=="lsm" && !(k in seen)){seen[k]=1;n++}} END{print n+0}' "$RAW")"
MISSING="$(awk '/^CELL /{eng="";m=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="engine")eng=a[2]; if(a[1]=="missing")m=a[2]} if(eng=="lsm" && m ~ /^[0-9]+$/) s+=m} END{print s+0}' "$RAW")"
MISMATCH="$(awk '/^CELL /{eng="";m=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="engine")eng=a[2]; if(a[1]=="mismatch")m=a[2]} if(eng=="lsm" && m ~ /^[0-9]+$/) s+=m} END{print s+0}' "$RAW")"
UNRELIABLE="$(awk '/^CELL /{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="notes" && a[2] ~ /UNRELIABLE/) n++}} END{print n+0}' "$RAW")"
# 复现性判据只对 **engine=lsm** 的格生效：M5-C6 明确「硬门禁只施加在 LSM 腿」，raw/map 的
# verify=na 不参与退出码。非 LSM 格的 repro_ok 仍逐行打印，但不进 BENCH_REPRO_OK。
REPRO="$(awk '/^CELL /{eng=""; r=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="engine") eng=a[2]; if(a[1]=="repro_ok") r=a[2]} if(eng=="lsm"){ cells++; if(r=="0") bad=1 }} END{print bad?0:1}' "$RAW")"
REPRO_LSM_CELLS="$(awk '/^CELL /{eng=""; for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="engine") eng=a[2]} if(eng=="lsm") n++} END{print n+0}' "$RAW")"

# ---- PHASE 2（M5.3 追加）：M4.3 的三条诊断固定行 ----
# §6.6 的列（write_amp_* / space_amp* / read_filter_* / space_filter_bytes / space_sst_data_bytes）
# 全部从这三行取；`space_filter_bytes` 由 `lsm_level_stats` 逐文件 `Table::filter_bytes()` 求和后
# **追加在 AMPL 行尾**（不得用公式估算，也不得静默省略该列）。
AMPL_RC=0
AMPL_MISSING=0
AMPL_MISMATCH=0
SPACE_SST_BYTES=0
SPACE_FILTER_BYTES=0
SPACE_SST_DATA_BYTES=0
AMPL_OK=0
if [ "$NO_AMPL" != "1" ]; then
  AMPL_BIN=build/bin/lsm_ampl_probe
  STATS_BIN=build/bin/lsm_level_stats
  if [ ! -x "$AMPL_BIN" ] || [ ! -x "$STATS_BIN" ]; then
    echo "[FAIL] 缺 $AMPL_BIN 或 $STATS_BIN（AMPL/LEVEL/FRONT 三行无法产出；用 --no-ampl 显式跳过）" >&2
    exit 1
  fi
  mkdir -p "$WORK/ampl"
  "$AMPL_BIN" --db "$WORK/ampl" --rounds "$AMPL_ROUNDS" --keys "$AMPL_KEYS" \
      --write-buffer-size "$AMPL_WRITE_BUFFER_SIZE" --strategy "$AMPL_STRATEGY" \
      --bloom-bits "$BLOOM_BITS" \
      >"$WORK/ampl.raw" 2>"$WORK/ampl.err"
  AMPL_RC=$?
  if [ "$AMPL_RC" -ne 0 ]; then
    echo "[FAIL] lsm_ampl_probe 退出码 $AMPL_RC（stderr 尾部）" >&2
    tail -20 "$WORK/ampl.err" >&2
    exit 1
  fi
  AMPL_LINE="$(grep '^AMPL ' "$WORK/ampl.raw" | head -1)"
  LEVEL_LINE="$(grep '^LEVEL ' "$WORK/ampl.raw" | head -1)"
  FRONT_LINE="$(grep '^FRONT ' "$WORK/ampl.raw" | head -1)"
  if [ -z "$AMPL_LINE" ] || [ -z "$LEVEL_LINE" ] || [ -z "$FRONT_LINE" ]; then
    echo "[FAIL] lsm_ampl_probe 未产出 AMPL/LEVEL/FRONT 三行（原始输出见下）" >&2
    cat "$WORK/ampl.raw" >&2
    exit 1
  fi
  # AMPL 行尾追加 space_filter_bytes / space_sst_data_bytes（§6.6 的落点）。
  "$STATS_BIN" --db "$WORK/ampl" --ampl-line "$AMPL_LINE" >"$WORK/ampl.space" 2>"$WORK/ampl.err2"
  STATS_RC=$?
  if [ "$STATS_RC" -ne 0 ]; then
    echo "[FAIL] lsm_level_stats 退出码 $STATS_RC（stderr 尾部）" >&2
    tail -20 "$WORK/ampl.err2" >&2
    exit 1
  fi
  "$STATS_BIN" --db "$WORK/ampl" >"$WORK/space.raw" 2>/dev/null
  cat "$WORK/ampl.space" >> "$OUT"
  {
    grep '^LEVEL ' "$WORK/ampl.raw" | head -1
    grep '^FRONT ' "$WORK/ampl.raw" | head -1
    grep '^SPACE ' "$WORK/space.raw" | head -1
  } >> "$OUT"
  grep -E '^(AMPL|LEVEL|FRONT|SPACE) ' "$OUT" | tail -4

  AMPL_MISSING="$(sed -n 's/.*MISSING \([0-9]*\) MISMATCH.*/\1/p' "$WORK/ampl.raw" | head -1)"
  AMPL_MISMATCH="$(sed -n 's/.*MISSING [0-9]* MISMATCH \([0-9]*\).*/\1/p' "$WORK/ampl.raw" | head -1)"
  AMPL_MISSING="${AMPL_MISSING:-0}"
  AMPL_MISMATCH="${AMPL_MISMATCH:-0}"
  SPACE_SST_BYTES="$(grep '^SPACE ' "$WORK/space.raw" | sed -n 's/.*SPACE_SST_BYTES \([0-9]*\).*/\1/p' | head -1)"
  SPACE_FILTER_BYTES="$(grep '^SPACE ' "$WORK/space.raw" | sed -n 's/.*SPACE_FILTER_BYTES \([0-9]*\).*/\1/p' | head -1)"
  SPACE_SST_DATA_BYTES="$(grep '^SPACE ' "$WORK/space.raw" | sed -n 's/.*SPACE_SST_DATA_BYTES \([0-9]*\).*/\1/p' | head -1)"
  SPACE_SST_BYTES="${SPACE_SST_BYTES:-0}"
  SPACE_FILTER_BYTES="${SPACE_FILTER_BYTES:-0}"
  SPACE_SST_DATA_BYTES="${SPACE_SST_DATA_BYTES:-0}"
  AMPL_OK=1

  # M5-B04：`space_sst_bytes == space_filter_bytes + space_sst_data_bytes` 必须能复算；
  # 左边来自 impl 的 AmplificationStats，右边来自逐文件实测 ⇒ 这是一次**跨来源**核对。
  AMPL_SST="$(printf '%s' "$AMPL_LINE" | sed -n 's/.* space_sst_bytes=\([0-9]*\).*/\1/p')"
  AMPL_FILTER="$(sed -n 's/.*space_filter_bytes=\([0-9]*\).*/\1/p' "$WORK/ampl.space" | head -1)"
  AMPL_DATA="$(sed -n 's/.*space_sst_data_bytes=\([0-9]*\).*/\1/p' "$WORK/ampl.space" | head -1)"
  if [ "$AMPL_SST" != "$SPACE_SST_BYTES" ] || [ "$AMPL_FILTER" != "$SPACE_FILTER_BYTES" ] || \
     [ "$AMPL_DATA" != "$SPACE_SST_DATA_BYTES" ]; then
    echo "[FAIL] AMPL 追加列与逐文件实测不一致：ampl_sst=$AMPL_SST space_sst=$SPACE_SST_BYTES ampl_filter=$AMPL_FILTER space_filter=$SPACE_FILTER_BYTES ampl_data=$AMPL_DATA space_data=$SPACE_SST_DATA_BYTES" >&2
    exit 1
  fi
  if [ "$AMPL_SST" != "$((SPACE_FILTER_BYTES + SPACE_SST_DATA_BYTES))" ]; then
    echo "[FAIL] space_sst_bytes($AMPL_SST) != space_filter_bytes($SPACE_FILTER_BYTES) + space_sst_data_bytes($SPACE_SST_DATA_BYTES)" >&2
    exit 1
  fi
  if [ "$FILTER" = "on" ] && [ "$SPACE_FILTER_BYTES" -le 0 ]; then
    echo "[FAIL] --filter on 但 space_filter_bytes=$SPACE_FILTER_BYTES（filter 块一个字节都没写；M5-B04）" >&2
    exit 1
  fi
  # --filter off：AMPL 子轮必须真的不写 filter 块（否则 AMPL/space 行与 CELL 格的 filter 设置不一致）。
  # lsm_ampl_probe 的 --bloom-bits 使这一点可被机械核对（M5.3 追加；此前子轮恒为 bloom_bits=10）。
  if [ "$FILTER" = "off" ] && [ "$SPACE_FILTER_BYTES" -ne 0 ]; then
    echo "[FAIL] --filter off 但 space_filter_bytes=$SPACE_FILTER_BYTES（AMPL 子轮没按 off 跑；口径不一致）" >&2
    exit 1
  fi
  if [ "$AMPL_MISSING" -ne 0 ] || [ "$AMPL_MISMATCH" -ne 0 ]; then
    echo "[FAIL] AMPL 子轮（engine=lsm）对账失败：missing=$AMPL_MISSING mismatch=$AMPL_MISMATCH" >&2
    exit 1
  fi
fi

# LSM 腿的对账总量（CELL 格 + AMPL 子轮；**只**含 engine=lsm）。
MISSING_TOTAL=$((MISSING + AMPL_MISSING))
MISMATCH_TOTAL=$((MISMATCH + AMPL_MISMATCH))

if [ "$REPEATS" -ge 2 ] && [ "$REPRO" != "1" ]; then
  echo "[FAIL] BENCH_REPRO_OK 0：--repeats $REPEATS 下 engine=lsm 的格吞吐/P99 差异超过 --repro-tol $REPRO_TOL" >&2
  exit 1
fi
if [ "$LSM_DISTINCT" -eq 0 ]; then
  echo "[FAIL] 没有任何 engine=lsm 的格（空绿；--require-lsm / §7.3）" >&2
  exit 1
fi
if [ "$DISTINCT" -lt "$EXPECTED_MIN_CELLS" ]; then
  echo "[FAIL] 去重格数 $DISTINCT < $EXPECTED_MIN_CELLS（--expected-min-cells）" >&2
  exit 1
fi

RC=0
if [ "$MISSING_TOTAL" -ne 0 ]; then
  echo "[FAIL] LSM 腿 missing=$MISSING_TOTAL（只对账 engine=lsm；raw/map 的 verify=na 不参与）" >&2
  RC=1
fi
if [ "$MISMATCH_TOTAL" -ne 0 ]; then
  echo "[FAIL] LSM 腿 mismatch=$MISMATCH_TOTAL" >&2
  RC=1
fi

emit_summary() {
  echo "BENCH_CELLS_TOTAL $DISTINCT"
  echo "BENCH_CELL_ROWS_TOTAL $CELL_ROWS"
  echo "BENCH_LSM_CELLS_TOTAL $LSM_DISTINCT"
  echo "BENCH_MISSING_TOTAL $MISSING_TOTAL"
  echo "BENCH_MISMATCH_TOTAL $MISMATCH_TOTAL"
  echo "BENCH_UNRELIABLE_CELLS $UNRELIABLE"
  echo "BENCH_REPRO_OK $REPRO"
  echo "BENCH_REPRO_LSM_CELLS $REPRO_LSM_CELLS"
  echo "BENCH_AMPL_OK $AMPL_OK"
  echo "BENCH_AMPL_MISSING $AMPL_MISSING"
  echo "BENCH_AMPL_MISMATCH $AMPL_MISMATCH"
  echo "BENCH_SPACE_SST_BYTES $SPACE_SST_BYTES"
  echo "BENCH_SPACE_FILTER_BYTES $SPACE_FILTER_BYTES"
  echo "BENCH_SPACE_SST_DATA_BYTES $SPACE_SST_DATA_BYTES"
}

if [ "$INJECT" = "1" ]; then
  # §7.4：失败注入自测必须让脚本返回 1；若注入没生效（missing 仍为 0）这里返回 0，
  # 外层 bench_lsm_selftest.sh 会因 rc != 1 失败 —— 不允许把「没注入成功」当通过。
  {
    emit_summary
    echo "BENCH_INJECT_MISSING_RC $RC"
  } | tee -a "$OUT"
  exit "$RC"
fi

{
  emit_summary
  [ "$RC" -eq 0 ] && echo "[BENCH_LSM_OK]"
} | tee -a "$OUT"
exit "$RC"
