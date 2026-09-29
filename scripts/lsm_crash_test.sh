#!/usr/bin/env bash
# scripts/lsm_crash_test.sh —— kill -9 崩溃对账门禁（docs/m2-design.md §8.3）
#
#   --rounds N        轮数（默认 100）
#   --mode sync       writer 用 sync=true（唯一可宣称 durable 的模式，缺省）
#   --mode nosync     只验证「无半写/无乱序/无旧值覆盖」，**不构成 durable 证据**
#   --ack-sync-every N  放宽 sidecar 的 fsync 粒度（会在输出里显式标注强度被放宽）
# 退出码：missing != 0 或 mismatch != 0 ⇒ 1
set -u
cd "$(dirname "$0")/.."
ROUNDS=100
MODE=sync
ACK_SYNC_EVERY=1
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --mode) MODE="$2"; shift 2;;
    --ack-sync-every) ACK_SYNC_EVERY="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
if [ "$ACK_SYNC_EVERY" != "1" ]; then
  echo "WARN: 本次对账强度被放宽（--ack-sync-every=$ACK_SYNC_EVERY），sidecar 不再逐行 fsync" >&2
fi
BIN=./build/bin
if [ ! -x "$BIN/lsm_crash_writer" ] || [ ! -x "$BIN/lsm_crash_recover" ]; then
  echo "缺少 $BIN/lsm_crash_writer 或 lsm_crash_recover，请先构建" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_crash_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
MISSING_TOTAL=0
MISMATCH_TOTAL=0
echo "== lsm_crash_test: rounds=$ROUNDS mode=$MODE ack_sync_every=$ACK_SYNC_EVERY dir=$WORK =="
for i in $(seq 1 "$ROUNDS"); do
  DB="$WORK/db$i"
  ACK="$WORK/ack$i.txt"
  mkdir -p "$DB"
  : > "$ACK"
  "$BIN/lsm_crash_writer" "$DB" "$ACK" >/dev/null 2>"$WORK/w$i.err" &
  WPID=$!
  # 随机时刻杀：让 kill -9 落在写进行中的不同阶段
  SLEEP_MS=$(( (RANDOM % 250) + 30 ))
  sleep "$(awk "BEGIN{printf \"%.3f\", $SLEEP_MS/1000}")"
  kill -9 "$WPID" 2>/dev/null
  wait "$WPID" 2>/dev/null
  OUT=$("$BIN/lsm_crash_recover" "$DB" "$ACK" 2>"$WORK/r$i.err")
  RC=$?
  LINE=$(echo "$OUT" | grep '^ROUND ' | head -1)
  M=$(echo "$LINE" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
  MM=$(echo "$LINE" | sed -n 's/.*MISMATCH \([0-9]*\).*/\1/p')
  MISSING_TOTAL=$((MISSING_TOTAL + M))
  MISMATCH_TOTAL=$((MISMATCH_TOTAL + MM))
  printf 'ROUND %s ACKED_RECOVERED_MISSING_MISMATCH %s RC %s\n' "$i" "$LINE" "$RC"
  if [ "$RC" -ne 0 ]; then
    echo "  [FAIL] recover 退出码非 0；stderr:" >&2
    sed 's/^/    /' "$WORK/r$i.err" >&2
  fi
done
echo "TOTAL_ROUNDS $ROUNDS MISSING_TOTAL $MISSING_TOTAL MISMATCH_TOTAL $MISMATCH_TOTAL"
if [ "$MISSING_TOTAL" -ne 0 ] || [ "$MISMATCH_TOTAL" -ne 0 ]; then
  echo "[FAIL] 已 ack 的写出现丢失或值不一致" >&2
  exit 1
fi
echo "[OK] 全部 $ROUNDS 轮 missing 0 / mismatch 0"
