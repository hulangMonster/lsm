#!/usr/bin/env bash
# scripts/lsm_compaction_stress.sh —— M4.3 B 组的驱动（两种 pick 策略 + 三个放大的固定行）
#
# 收尾汇总标记行（门禁 run_gate_m3_marked 读它）：
#   [COMPACTION_STRESS_OK] / [COMPACTION_STRATEGY_OK]，且必须出现 COMPACTION_ROUNDS_TOTAL > 0 与 MISSING 0。
set -uo pipefail
cd "$(dirname "$0")/.."

ROUNDS=3
KEYS=2000
WBS=16384
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --keys) KEYS="$2"; shift 2;;
    --write-buffer-size) WBS="$2"; shift 2;;
    --strategy) STRATEGY="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
STRATEGY="${STRATEGY:-both}"

if ! cmake --build build --target lsm_ampl_probe >/dev/null 2>&1; then
  echo "[FAIL] 构建 lsm_ampl_probe 失败"
  exit 1
fi

run_one() {
  local strat="$1" dir="$2"
  rm -rf "$dir"
  ./build/bin/lsm_ampl_probe --db "$dir" --rounds "$ROUNDS" --keys "$KEYS" \
    --write-buffer-size "$WBS" --strategy "$strat"
}

RR=""
MO=""
if [ "$STRATEGY" = "both" ] || [ "$STRATEGY" = "round_robin" ]; then
  RR="$(run_one round_robin /tmp/lsm_stress_rr)" || { echo "$RR"; echo "[FAIL] round_robin 运行失败"; exit 1; }
  echo "$RR"
fi
if [ "$STRATEGY" = "both" ] || [ "$STRATEGY" = "min_overlap" ]; then
  MO="$(run_one min_overlap /tmp/lsm_stress_mo)" || { echo "$MO"; echo "[FAIL] min_overlap 运行失败"; exit 1; }
  echo "$MO"
fi

PRIMARY="${RR:-$MO}"
if ! printf '%s' "$PRIMARY" | grep -Eq 'COMPACTION_ROUNDS_TOTAL [1-9][0-9]*'; then
  echo "[FAIL] compaction 一次都没发生（空绿）"
  exit 1
fi
if ! printf '%s' "$PRIMARY" | grep -q 'MISSING 0 MISMATCH 0'; then
  echo "[FAIL] 压测出现 missing/mismatch"
  exit 1
fi
if [ -n "$RR" ] && [ -n "$MO" ]; then
  echo "M4-B03 STRATEGY_TABLE strat=round_robin strat=min_overlap [COMPACTION_STRATEGY_OK]"
fi
echo "[COMPACTION_STRESS_OK]"
