#!/usr/bin/env bash
# scripts/lsm_flush_crash_test.sh —— M3-B01：flush 崩溃对账（kill -9 循环，docs/m3-design.md §10.2）
#
# 与 M2 的 lsm_crash_test.sh 同一协议（先 Put(sync) 再 fsync sidecar），但用**小 write_buffer_size**
# 迫使每轮都发生 flush + WAL 轮转/回收。必打的收尾标记：
#   SST_FILES_TOTAL <n>（>0）  LOGS_DELETED_TOTAL <n>（>0）  MISSING_TOTAL 0  ROUNDS_OK <n>
#   末行 [FLUSH_CRASH_OK]
# 这三个正向计数是门禁 v2 用来防"退出码 0 但什么都没发生"的空绿（docs/m3-prerequisites §9 D9.6）。
#
#   --rounds N                轮数（默认 10；门禁传 100）
#   --write-buffer-size BYTES flush 触发阈值（默认 262144）
set -u
cd "$(dirname "$0")/.."
ROUNDS=10
WBS=262144
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --write-buffer-size) WBS="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
BIN=./build/bin
if [ ! -x "$BIN/lsm_m3_probe" ]; then
  echo "缺少 $BIN/lsm_m3_probe，请先构建（cmake --build build）" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_flush_crash_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
SST_TOTAL=0
LOGS_TOTAL=0
MISSING_TOTAL=0
MISMATCH_TOTAL=0
ACKED_TOTAL=0
REPLAYED_TOTAL=0
ROUNDS_OK=0
echo "== lsm_flush_crash_test: rounds=$ROUNDS write_buffer_size=$WBS dir=$WORK =="
for i in $(seq 1 "$ROUNDS"); do
  DB="$WORK/db$i"
  ACK="$WORK/ack$i.txt"
  mkdir -p "$DB"
  : > "$ACK"
  "$BIN/lsm_m3_probe" flush-writer "$DB" "$ACK" "$WBS" >/dev/null 2>"$WORK/w$i.err" &
  WPID=$!
  SLEEP_MS=$(( (RANDOM % 250) + 30 ))
  sleep "$(awk "BEGIN{printf \"%.3f\", $SLEEP_MS/1000}")"
  kill -9 "$WPID" 2>/dev/null
  wait "$WPID" 2>/dev/null
  OUT="$("$BIN/lsm_m3_probe" flush-recover "$DB" "$ACK" "$WBS" 2>"$WORK/r$i.err")"
  RC=$?
  LINE=$(echo "$OUT" | grep '^ROUND ' | head -1)
  if [ "$RC" -ne 0 ] || [ -z "$LINE" ]; then
    echo "ROUND_FAIL $i RC $RC（对账未产出 ROUND 行）" >&2
    sed 's/^/    /' "$WORK/r$i.err" >&2
    exit 1
  fi
  M=$(echo "$LINE" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
  MM=$(echo "$LINE" | sed -n 's/.*MISMATCH \([0-9]*\).*/\1/p')
  A=$(echo "$LINE" | sed -n 's/.*ACKED \([0-9]*\).*/\1/p')
  S=$(echo "$LINE" | sed -n 's/.*SST_FILES_TOTAL \([0-9]*\).*/\1/p')
  L=$(echo "$LINE" | sed -n 's/.*LOGS_DELETED_TOTAL \([0-9]*\).*/\1/p')
  RP=$(echo "$LINE" | sed -n 's/.*RECORDS_REPLAYED \([0-9]*\).*/\1/p')
  if [ -z "$M" ] || [ -z "$MM" ] || [ -z "$A" ] || [ -z "$S" ] || [ -z "$L" ] || [ -z "$RP" ]; then
    echo "ROUND_FAIL $i 无法解析汇总行：$LINE" >&2
    exit 1
  fi
  MISSING_TOTAL=$((MISSING_TOTAL + M))
  MISMATCH_TOTAL=$((MISMATCH_TOTAL + MM))
  ACKED_TOTAL=$((ACKED_TOTAL + A))
  SST_TOTAL=$((SST_TOTAL + S))
  LOGS_TOTAL=$((LOGS_TOTAL + L))
  REPLAYED_TOTAL=$((REPLAYED_TOTAL + RP))
  ROUNDS_OK=$((ROUNDS_OK + 1))
  printf 'ROUND %s %s\n' "$i" "$LINE"
done
echo "SST_FILES_TOTAL $SST_TOTAL"
echo "LOGS_DELETED_TOTAL $LOGS_TOTAL"
echo "MISSING_TOTAL $MISSING_TOTAL"
echo "MISMATCH_TOTAL $MISMATCH_TOTAL"
echo "ACKED_TOTAL $ACKED_TOTAL"
echo "RECORDS_REPLAYED_TOTAL $REPLAYED_TOTAL"
echo "ROUNDS_OK $ROUNDS_OK"
FAIL=0
if [ "$ROUNDS_OK" -ne "$ROUNDS" ]; then
  echo "[FAIL] 只有 $ROUNDS_OK/$ROUNDS 轮成功产出对账结果（门禁不完整）" >&2
  FAIL=1
fi
if [ "$ACKED_TOTAL" -eq 0 ]; then
  echo "[FAIL] ACKED_TOTAL 0：写者根本没写进数据，门禁无意义" >&2
  FAIL=1
fi
if [ "$MISSING_TOTAL" -ne 0 ] || [ "$MISMATCH_TOTAL" -ne 0 ]; then
  echo "[FAIL] 已 ack 的写出现丢失或值不一致" >&2
  FAIL=1
fi
if [ "$SST_TOTAL" -eq 0 ]; then
  echo "[FAIL] SST_FILES_TOTAL 0：一次 flush 都没发生（空绿）" >&2
  FAIL=1
fi
if [ "$LOGS_TOTAL" -eq 0 ]; then
  echo "[FAIL] LOGS_DELETED_TOTAL 0：WAL 回收未生效（空绿）" >&2
  FAIL=1
fi
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
echo "[FLUSH_CRASH_OK]"
exit 0
