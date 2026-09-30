#!/usr/bin/env bash
# scripts/lsm_batch_crash_test.sh —— M5-B01：kill -9 落在**批写入中途**的原子性对账
# （docs/m5-design.md §10.2 的 M5-B01、§9.2 的 I51/I53）
#
#   --rounds N              轮数（默认 100）
#   --batch-size N          每批 entry 数（默认 16；必须 >= 2）
#   --write-buffer-size N   写缓冲（默认 262144，制造真实的冻结/轮转交错）
#   --nosync                writer 用 sync=false —— 只验证「无半批」，**不构成 durable 证据**
#
# 判据（缺一即非零退出）：
#   BATCH_KILL9_MISSING 0  ∧  BATCH_MISMATCH 0  ∧  BATCH_HALF_VISIBLE 0  ∧  ROUNDS_OK == ROUNDS
#   且 BATCH_ACKED > 0（否则是空绿），末行打印 [BATCH_CRASH_OK]。
#
# 诚实性边界（§10.2 的 B 组纪律）：kill -9 不丢 page cache，本脚本证明的是**进程级一致性**；
# 掉电语义（未 fsync 后缀丢失 + 撕裂）由 A 组的 MemEnv 用例承担，两者结论不得混写。
set -u
cd "$(dirname "$0")/.."
ROUNDS=100
BATCH_SIZE=16
WRITE_BUFFER=262144
SYNC=1
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --batch-size) BATCH_SIZE="$2"; shift 2;;
    --write-buffer-size) WRITE_BUFFER="$2"; shift 2;;
    --nosync) SYNC=0; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
if [ "$BATCH_SIZE" -lt 2 ]; then
  echo "batch-size 必须 >= 2（否则「半批」无意义）" >&2
  exit 2
fi
if [ "$SYNC" != "1" ]; then
  echo "WARN: --nosync 只验证「无半批/无乱序」，**不构成 durable 证据**" >&2
fi
BIN=./build/bin
if [ ! -x "$BIN/lsm_batch_crash_writer" ] || [ ! -x "$BIN/lsm_batch_crash_recover" ]; then
  echo "缺少 $BIN/lsm_batch_crash_writer 或 lsm_batch_crash_recover，请先构建" >&2
  exit 2
fi
WORK=$(mktemp -d /tmp/lsm_batch_crash_XXXXXX)
trap 'rm -rf "$WORK"' EXIT

MISSING=0
MISMATCH=0
HALF=0
ACKED=0
SEEN=0
ROUNDS_OK=0
echo "== lsm_batch_crash_test: rounds=$ROUNDS batch_size=$BATCH_SIZE sync=$SYNC write_buffer=$WRITE_BUFFER dir=$WORK =="
for i in $(seq 1 "$ROUNDS"); do
  DB="$WORK/db$i"
  ACK="$WORK/ack$i.txt"
  mkdir -p "$DB"
  : > "$ACK"
  "$BIN/lsm_batch_crash_writer" "$DB" "$ACK" "$BATCH_SIZE" "$SYNC" "$WRITE_BUFFER" \
    >/dev/null 2>"$WORK/w$i.err" &
  WPID=$!
  # 随机时刻杀：让 kill -9 落在批写入/组提交的不同阶段
  SLEEP_MS=$(( (RANDOM % 250) + 30 ))
  sleep "$(awk "BEGIN{printf \"%.3f\", $SLEEP_MS/1000}")"
  kill -9 "$WPID" 2>/dev/null
  wait "$WPID" 2>/dev/null
  OUT="$("$BIN/lsm_batch_crash_recover" "$DB" "$ACK" "$BATCH_SIZE" 2>"$WORK/r$i.err")"
  RC=$?
  LINE=$(echo "$OUT" | grep '^ROUND ' | head -1)
  if [ -z "$LINE" ]; then
    echo "ROUND_FAIL $i RC $RC（对账工具未产出 ROUND 行）" >&2
    sed 's/^/    /' "$WORK/r$i.err" >&2
    exit 1
  fi
  M=$(echo "$LINE" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
  MM=$(echo "$LINE" | sed -n 's/.*MISMATCH \([0-9]*\).*/\1/p')
  H=$(echo "$LINE" | sed -n 's/.*HALF \([0-9]*\).*/\1/p')
  A=$(echo "$LINE" | sed -n 's/.*ACKED \([0-9]*\).*/\1/p')
  S=$(echo "$LINE" | sed -n 's/.*BATCHES_SEEN \([0-9]*\).*/\1/p')
  if [ -z "$M" ] || [ -z "$MM" ] || [ -z "$H" ] || [ -z "$A" ] || [ -z "$S" ]; then
    echo "ROUND_FAIL $i 无法从 ROUND 行解析计数：$LINE" >&2
    exit 1
  fi
  MISSING=$((MISSING + M))
  MISMATCH=$((MISMATCH + MM))
  HALF=$((HALF + H))
  ACKED=$((ACKED + A))
  SEEN=$((SEEN + S))
  ROUNDS_OK=$((ROUNDS_OK + 1))
  if [ "$RC" -ne 0 ]; then
    echo "  [FAIL] 第 $i 轮对账未过：$LINE" >&2
    sed 's/^/    /' "$WORK/r$i.err" >&2
  fi
  printf 'ROUND %s %s RC %s\n' "$i" "$LINE" "$RC"
done

echo "BATCH_KILL9_ROUNDS $ROUNDS ROUNDS_OK $ROUNDS_OK BATCH_ACKED $ACKED BATCHES_SEEN $SEEN BATCH_KILL9_MISSING $MISSING BATCH_MISMATCH $MISMATCH BATCH_HALF_VISIBLE $HALF"
FAILED=0
if [ "$ROUNDS_OK" -ne "$ROUNDS" ]; then
  echo "[FAIL] 只有 $ROUNDS_OK/$ROUNDS 轮成功产出对账结果（门禁不完整）" >&2
  FAILED=1
fi
if [ "$ACKED" -eq 0 ] || [ "$SEEN" -eq 0 ]; then
  echo "[FAIL] ACKED=$ACKED SEEN=$SEEN：写者几乎没写进数据，门禁是空绿" >&2
  FAILED=1
fi
if [ "$MISSING" -ne 0 ]; then
  echo "[FAIL] 已 ack 的批出现丢失（$MISSING 条 entry）—— 违反 I51/I54" >&2
  FAILED=1
fi
if [ "$MISMATCH" -ne 0 ]; then
  echo "[FAIL] 值与写入不一致（$MISMATCH 条）" >&2
  FAILED=1
fi
if [ "$HALF" -ne 0 ]; then
  echo "[FAIL] 出现半批（$HALF 个批只可见一部分）—— 违反 I51/§13.2" >&2
  FAILED=1
fi
if [ "$FAILED" -ne 0 ]; then
  exit 1
fi
echo "[BATCH_CRASH_OK] rounds=$ROUNDS batch_size=$BATCH_SIZE sync=$SYNC missing=0 mismatch=0 half=0"
