#!/usr/bin/env bash
# scripts/lsm_filter_damage_test.sh —— M5.3 / M5-B09：真实磁盘上的 filter 块损坏扫描
# （docs/m5-design.md §7.5 的 M5-E 腿 / §10.2 M5-B09）
#
# 判据（缺一即非零退出）：
#   * FILTER_DAMAGE_CASES > 0（真的翻转了字节，防空绿）；
#   * FILTER_SILENT_FALSE_NEGATIVE == 0 且 FILTER_SILENT_WRONG_VALUE == 0（零静默错值）；
#   * FILTER_DAMAGE_DETECTED > 0 且 FILTER_DAMAGE_FILTER_DEGRADED > 0（损坏真的被检出/降级）；
#   * 重算 CRC 的错位注入：FILTER_MISALIGN_INJECTED == 1、FILTER_MISALIGN_DETECTED == 1、
#     FILTER_MISALIGN_SILENT_FN == 0（CRC 不是唯一防线，E4）。
#
# 用法：bash scripts/lsm_filter_damage_test.sh [--cases 2000]
set -u
cd "$(dirname "$0")/.."
CASES=2000
while [ $# -gt 0 ]; do
  case "$1" in
    --cases) CASES="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
BIN=./build/bin/lsm_filter_damage
if [ ! -x "$BIN" ]; then
  echo "缺少 $BIN，请先构建（cmake --build build）" >&2
  exit 2
fi
WORK="$(mktemp -d /tmp/lsm_filter_damage_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/db"
echo "== lsm_filter_damage_test: cases=$CASES dir=$WORK =="
OUT="$("$BIN" "$WORK/db" "$CASES" 2>"$WORK/err")"
RC=$?
echo "$OUT"
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] 损坏扫描探针退出码 $RC" >&2
  sed 's/^/    /' "$WORK/err" >&2
  exit 1
fi

pick() { echo "$OUT" | sed -n "s/.*$1 \([0-9]*\).*/\1/p" | head -1; }
C="$(pick FILTER_DAMAGE_CASES)"
SFN="$(pick FILTER_SILENT_FALSE_NEGATIVE)"
SWV="$(pick FILTER_SILENT_WRONG_VALUE)"
DET="$(pick FILTER_DAMAGE_DETECTED)"
DEG="$(pick FILTER_DAMAGE_FILTER_DEGRADED)"
MI="$(pick FILTER_MISALIGN_INJECTED)"
MD="$(pick FILTER_MISALIGN_DETECTED)"
MFN="$(pick FILTER_MISALIGN_SILENT_FN)"

fail=0
for name in C SFN SWV DET DEG MI MD MFN; do
  eval "v=\$$name"
  case "$v" in ''|*[!0-9]*) echo "[FAIL] 标记 $name 缺失/非数字（输出不完整）" >&2; fail=1;; esac
done
if [ "$fail" -eq 0 ]; then
  [ "$C" -gt 0 ] || { echo "[FAIL] FILTER_DAMAGE_CASES 0：一个字节都没翻转（空绿）" >&2; fail=1; }
  [ "$SFN" -eq 0 ] || { echo "[FAIL] FILTER_SILENT_FALSE_NEGATIVE $SFN" >&2; fail=1; }
  [ "$SWV" -eq 0 ] || { echo "[FAIL] FILTER_SILENT_WRONG_VALUE $SWV" >&2; fail=1; }
  [ "$DET" -gt 0 ] || { echo "[FAIL] FILTER_DAMAGE_DETECTED 0：损坏没被检出" >&2; fail=1; }
  [ "$DEG" -gt 0 ] || { echo "[FAIL] FILTER_DAMAGE_FILTER_DEGRADED 0：没命中 filter 块" >&2; fail=1; }
  [ "$MI" -eq 1 ] || { echo "[FAIL] FILTER_MISALIGN_INJECTED $MI（错位注入没做）" >&2; fail=1; }
  [ "$MD" -eq 1 ] || { echo "[FAIL] FILTER_MISALIGN_DETECTED $MD（重算 CRC 的错位注入未被检出）" >&2; fail=1; }
  [ "$MFN" -eq 0 ] || { echo "[FAIL] FILTER_MISALIGN_SILENT_FN $MFN" >&2; fail=1; }
fi
if [ "$fail" -ne 0 ] || ! echo "$OUT" | grep -q '\[FILTER_DAMAGE_OK\]'; then
  echo "[FAIL] M5-E 判据未全部满足" >&2
  exit 1
fi
exit 0
