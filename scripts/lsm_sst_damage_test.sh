#!/usr/bin/env bash
# scripts/lsm_sst_damage_test.sh —— M3-B04：SSTable 单字节翻转扫描（docs/m3-design.md §10.2）
#
# 沿用 M2 B03 的逐字节方法论：对已注册 .sst 的每个采样字节翻转一次，重开后 Open 或首次读块
# **必须**返回 kCorruption；任何"静默返回错值"都计入 SILENT_WRONG（判 FAIL）。
# 必打标记：SST_DAMAGE_CASES <n>（>0）与 SILENT_WRONG 0。
#
#   --cases N  采样翻转次数上限（默认 2000；门禁传 2000）
set -u
cd "$(dirname "$0")/.."
CASES=2000
while [ $# -gt 0 ]; do
  case "$1" in
    --cases) CASES="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
BIN=./build/bin
if [ ! -x "$BIN/lsm_m3_probe" ]; then
  echo "缺少 $BIN/lsm_m3_probe，请先构建（cmake --build build）" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_sst_damage_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
DB="$WORK/db"
mkdir -p "$DB"
echo "== lsm_sst_damage_test: cases=$CASES dir=$WORK =="
OUT="$("$BIN/lsm_m3_probe" damage "$DB" "$CASES" 2>"$WORK/err")"
RC=$?
echo "$OUT"
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] damage 探针失败 RC=$RC" >&2
  sed 's/^/    /' "$WORK/err" >&2
  exit 1
fi
C=$(echo "$OUT" | sed -n 's/.*SST_DAMAGE_CASES \([0-9]*\).*/\1/p')
S=$(echo "$OUT" | sed -n 's/.*SILENT_WRONG \([0-9]*\).*/\1/p')
if [ -z "$C" ] || [ -z "$S" ]; then
  echo "[FAIL] 输出缺少 SST_DAMAGE_CASES / SILENT_WRONG 标记" >&2
  exit 1
fi
if [ "$C" -le 0 ]; then
  echo "[FAIL] SST_DAMAGE_CASES 0：一个字节都没翻转（空绿）" >&2
  exit 1
fi
if [ "$S" -ne 0 ]; then
  echo "[FAIL] SILENT_WRONG $S：存在静默返回错值" >&2
  exit 1
fi
exit 0
