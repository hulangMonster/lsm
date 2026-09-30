#!/usr/bin/env bash
# scripts/lsm_compaction_crash_test.sh —— M4.3 B01/B02：真实进程在 compaction 中途 kill -9 的对账
set -uo pipefail
cd "$(dirname "$0")/.."
MODE=b01
ROUNDS=30
KEYS=3000
WBS=16384
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="$2"; shift 2;;
    --rounds) ROUNDS="$2"; shift 2;;
    --keys) KEYS="$2"; shift 2;;
    --write-buffer-size) WBS="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
if ! cmake --build build --target lsm_compaction_crash >/dev/null 2>&1; then
  echo "[FAIL] 构建 lsm_compaction_crash 失败"; exit 1
fi
BIN=./build/bin/lsm_compaction_crash

if [ "$MODE" = "b01" ]; then
  MISSING_TOTAL=0; MISMATCH_TOTAL=0; ROUNDS_OK=0; COMPACTION_ROUNDS_TOTAL=0; SST_TOTAL=0
  for r in $(seq 1 "$ROUNDS"); do
    DB=/tmp/lsm_crash_db; CK=/tmp/lsm_crash_ckpt; RF=/tmp/lsm_crash_rounds; HIT=/tmp/lsm_crash_hit
    rm -rf "$DB"; rm -f "$CK" "$RF" "$HIT"
    "$BIN" writer --db "$DB" --keys "$KEYS" --write-buffer-size "$WBS" --ckpt "$CK" --rounds-file "$RF" --hit "$HIT" >/dev/null 2>&1 &
    PID=$!
    sleep 0.3
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    OUT="$("$BIN" verify --db "$DB" --ckpt "$CK" --rounds-file "$RF" 2>/dev/null)"
    RC=$?
    M=$(printf '%s' "$OUT" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
    MM=$(printf '%s' "$OUT" | sed -n 's/.*MISMATCH \([0-9]*\).*/\1/p')
    CR=$(printf '%s' "$OUT" | sed -n 's/.*COMPACTION_ROUNDS \([0-9]*\).*/\1/p')
    SST=$(printf '%s' "$OUT" | sed -n 's/.*SST_FILES_TOTAL \([0-9]*\).*/\1/p')
    MISSING_TOTAL=$((MISSING_TOTAL + ${M:-0}))
    MISMATCH_TOTAL=$((MISMATCH_TOTAL + ${MM:-0}))
    COMPACTION_ROUNDS_TOTAL=$((COMPACTION_ROUNDS_TOTAL + ${CR:-0}))
    SST_TOTAL=$((SST_TOTAL + ${SST:-0}))
    [ "$RC" = "0" ] && ROUNDS_OK=$((ROUNDS_OK + 1))
  done
  echo "COMPACTION_ROUNDS_TOTAL $COMPACTION_ROUNDS_TOTAL SST_FILES_TOTAL $SST_TOTAL MISSING_TOTAL $MISSING_TOTAL MISMATCH_TOTAL $MISMATCH_TOTAL ROUNDS_OK $ROUNDS_OK"
  if [ "$COMPACTION_ROUNDS_TOTAL" -lt 1 ] || [ "$SST_TOTAL" -lt 1 ] || [ "$MISSING_TOTAL" -ne 0 ] || [ "$MISMATCH_TOTAL" -ne 0 ] || [ "$ROUNDS_OK" -lt 1 ]; then
    echo "[FAIL] B01 判据不满足"; exit 1
  fi
  echo "[COMPACTION_CRASH_OK]"
  exit 0
fi

if [ "$MODE" = "b02" ]; then
  POINTS_OK=0; MISSING_TOTAL=0; REF_MISSING_TOTAL=0; OPEN_CORRUPTION_TOTAL=0; ORPHAN_TOTAL=0
  for p in 0 1 2 3; do
    DB=/tmp/lsm_inject_db; CK=/tmp/lsm_inject_ckpt; RF=/tmp/lsm_inject_rounds; HIT=/tmp/lsm_inject_hit
    rm -rf "$DB"; rm -f "$CK" "$RF" "$HIT"
    "$BIN" writer --db "$DB" --point "$p" --keys "$KEYS" --write-buffer-size "$WBS" --ckpt "$CK" --rounds-file "$RF" --hit "$HIT" >/dev/null 2>&1
    if [ -f "$HIT" ] && grep -q "POINT_HIT $p" "$HIT"; then POINTS_OK=$((POINTS_OK + 1)); fi
    OUT="$("$BIN" verify --db "$DB" --ckpt "$CK" --rounds-file "$RF" 2>/dev/null)"
    RC=$?
    echo "POINT $p verify_rc=$RC :: $OUT"
    case "$OUT" in *"OPEN_OK 1"*) : ;; *) OPEN_CORRUPTION_TOTAL=$((OPEN_CORRUPTION_TOTAL + 1));; esac
    M=$(printf '%s' "$OUT" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
    RM=$(printf '%s' "$OUT" | sed -n 's/.*REF_MISSING \([0-9]*\).*/\1/p')
    OS=$(printf '%s' "$OUT" | sed -n 's/.*ORPHAN_SST \([0-9]*\).*/\1/p')
    OT=$(printf '%s' "$OUT" | sed -n 's/.*ORPHAN_TMP \([0-9]*\).*/\1/p')
    MISSING_TOTAL=$((MISSING_TOTAL + ${M:-0}))
    REF_MISSING_TOTAL=$((REF_MISSING_TOTAL + ${RM:-0}))
    ORPHAN_TOTAL=$((ORPHAN_TOTAL + ${OS:-0} + ${OT:-0}))
  done
  echo "INJECT_POINTS_OK $POINTS_OK MISSING_TOTAL $MISSING_TOTAL REF_MISSING_TOTAL $REF_MISSING_TOTAL OPEN_CORRUPTION_TOTAL $OPEN_CORRUPTION_TOTAL ORPHAN_REMOVED_TOTAL $ORPHAN_TOTAL"
  if [ "$POINTS_OK" -ne 4 ] || [ "$MISSING_TOTAL" -ne 0 ] || [ "$REF_MISSING_TOTAL" -ne 0 ] || [ "$OPEN_CORRUPTION_TOTAL" -ne 0 ]; then
    echo "[FAIL] B02 判据不满足"; exit 1
  fi
  echo "[COMPACTION_INJECT_OK]"
  exit 0
fi
echo "unknown mode $MODE" >&2; exit 2
