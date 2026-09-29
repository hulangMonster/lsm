#!/usr/bin/env bash
# scripts/lsm_fd_leak_test.sh —— M3-B05：文件句柄不泄漏（docs/m3-design.md §10.2，I30）
#
# 多轮 flush + Get 之后比较 /proc/self/fd 计数；相对基线的增量必须 <= table cache 容量 + 常数。
# 必打标记：FD_GROWTH <n>（相对基线的增量）。
#
#   --rounds N  轮数（默认 60）
set -u
cd "$(dirname "$0")/.."
ROUNDS=60
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
BIN=./build/bin
if [ ! -x "$BIN/lsm_m3_probe" ]; then
  echo "缺少 $BIN/lsm_m3_probe，请先构建（cmake --build build）" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_fd_leak_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
DB="$WORK/db"
mkdir -p "$DB"
echo "== lsm_fd_leak_test: rounds=$ROUNDS dir=$WORK =="
OUT="$("$BIN/lsm_m3_probe" fd-leak "$DB" "$ROUNDS" 2>"$WORK/err")"
RC=$?
echo "$OUT"
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] fd-leak 探针失败 RC=$RC" >&2
  sed 's/^/    /' "$WORK/err" >&2
  exit 1
fi
G=$(echo "$OUT" | sed -n 's/.*FD_GROWTH \([0-9-]*\).*/\1/p')
if [ -z "$G" ]; then
  echo "[FAIL] 输出缺少 FD_GROWTH 标记" >&2
  exit 1
fi
# 容差：table cache 容量（默认 64）+ 常数 16（I30 的上界口径）。
MAX=80
if [ "$G" -gt "$MAX" ]; then
  echo "[FAIL] FD_GROWTH $G > $MAX：句柄疑似泄漏（I30）" >&2
  exit 1
fi
exit 0
