#!/usr/bin/env bash
# scripts/lsm_flush_restart_test.sh —— M3-B03：落盘重启（仅靠 SSTable 可读，docs/m3-design.md §10.2）
#
# 写 N 条 → 强制把最后一个 memtable 也落盘（否则"当前 log 非空"必然重放）→ Close → 重开。
# 判据：重开后 records_replayed == 0（老 log 已被回收，数据全部来自已注册 SSTable）。
# 必打标记：RECORDS_REPLAYED 0 与末行 [FLUSH_RESTART_OK]。
#
#   --write-buffer-size BYTES  flush 触发阈值（默认 262144）
#   --keys N                   写入条数（默认 2000）
set -u
cd "$(dirname "$0")/.."
WBS=262144
KEYS=2000
while [ $# -gt 0 ]; do
  case "$1" in
    --write-buffer-size) WBS="$2"; shift 2;;
    --keys) KEYS="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
BIN=./build/bin
if [ ! -x "$BIN/lsm_m3_probe" ]; then
  echo "缺少 $BIN/lsm_m3_probe，请先构建（cmake --build build）" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_flush_restart_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
DB="$WORK/db"
mkdir -p "$DB"
echo "== lsm_flush_restart_test: write_buffer_size=$WBS keys=$KEYS dir=$WORK =="
OUT="$("$BIN/lsm_m3_probe" restart "$DB" "$WBS" "$KEYS" 2>"$WORK/err")"
RC=$?
echo "$OUT"
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] restart 探针失败 RC=$RC" >&2
  sed 's/^/    /' "$WORK/err" >&2
  exit 1
fi
RP=$(echo "$OUT" | sed -n 's/.*RECORDS_REPLAYED \([0-9]*\).*/\1/p')
if [ -z "$RP" ]; then
  echo "[FAIL] 输出缺少 RECORDS_REPLAYED 标记" >&2
  exit 1
fi
if [ "$RP" -ne 0 ]; then
  echo "[FAIL] RECORDS_REPLAYED $RP != 0：数据并非全部来自已注册 SSTable" >&2
  exit 1
fi
echo "[FLUSH_RESTART_OK]"
exit 0
