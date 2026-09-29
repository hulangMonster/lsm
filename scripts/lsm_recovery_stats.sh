#!/usr/bin/env bash
# scripts/lsm_recovery_stats.sh —— 恢复代价基线（docs/m2-design.md §9.2 B05）
#
# 口径：用 crash_writer 写一段时间（sync=true）后 kill -9，再量「WAL 字节数 → Open 耗时」。
# 这是 M3 的对照基线：M3 起恢复要叠加 SSTable 读取，本表的数字是「只有 WAL 时」的下界。
# 输出固定行格式：SIZE_BYTES <n> KEYS <k> OPEN_MS <ms>
set -u
cd "$(dirname "$0")/.."
BIN=./build/bin
if [ ! -x "$BIN/lsm_crash_writer" ] || [ ! -x "$BIN/lsm_crash_recover" ]; then
  echo "缺少崩溃工具，请先构建" >&2; exit 2
fi
WORK=$(mktemp -d /tmp/lsm_stats_XXXXXX)
trap 'rm -rf "$WORK"' EXIT
echo "== lsm_recovery_stats: dir=$WORK =="
for SECS in 0.2 0.6 1.5; do
  DB="$WORK/db_$SECS"; ACK="$WORK/ack_$SECS.txt"
  mkdir -p "$DB"; : > "$ACK"
  "$BIN/lsm_crash_writer" "$DB" "$ACK" >/dev/null 2>&1 &
  WPID=$!
  sleep "$SECS"
  kill -9 "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null
  SIZE=$(du -sb "$DB" 2>/dev/null | awk '{print $1}')
  KEYS=$(wc -l < "$ACK")
  OUT=$("$BIN/lsm_crash_recover" "$DB" "$ACK" 2>/dev/null | grep '^ROUND ' | head -1)
  OPEN_MS=$(echo "$OUT" | sed -n 's/.*OPEN_MS \([0-9]*\).*/\1/p')
  MISS=$(echo "$OUT" | sed -n 's/.*MISSING \([0-9]*\).*/\1/p')
  printf 'SIZE_BYTES %s KEYS %s OPEN_MS %s MISSING %s\n' "${SIZE:-0}" "$KEYS" "${OPEN_MS:-?}" "${MISS:-?}"
done
echo "NOTE 本表是「只有 WAL」时的恢复下界；M3 引入 SSTable 后必须重测并对照。"
echo "NOTE 本量级（十几 KB WAL、几百条记录）的恢复低于 OPEN_MS 的 1ms 分辨率 ⇒ 该列显示 0 属正常；"
echo "     M3 对照时必须用更大的 WAL（并考虑把耗时口径细化到微秒），否则该列没有分辨力。"
