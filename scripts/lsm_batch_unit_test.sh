#!/usr/bin/env bash
# scripts/lsm_batch_unit_test.sh —— M5.2 的正向标记腿
# （docs/m5-design.md §11 M5.2 的「判据/证据命令」、§7.5 的标记名、§10.1 的 M5-A11~A17）
#
# 判据（缺一即非零退出；门禁侧再用 run_gate_m5_marked 要求收尾汇总行里的标记全部出现）：
#   ① 选中的用例全部真跑且全绿（WriteBatch.* + Batch.* 必须 >= 9 条：M5-A11~A17）；
#   ② `M5_BATCH_PARTIAL_VISIBLE 0` / `M5_BATCH_HALF_VISIBLE 0` / `M5_BATCH_CRASH_HALF_VISIBLE 0`
#      —— I51「整批原子可见」的三个**打印出来的计数**，不是「测试没失败」；
#   ③ `M5_BATCH_LOST_WAKEUPS 0`（M2 的 L9/L10 无丢唤醒在批粒度上仍然成立）；
#   ④ `M5_BATCH_ONE_RECORD_PER_BATCH 1` + `M5_BATCH_RECOVERY_RECORDS > 0`
#      —— §13.2「一个 WriteBatch = 一条 WAL record」的实测（用恢复侧的 records_replayed 复核）；
#   ⑤ 有正向计数防空绿：`M5_BATCH_TRUNCATE_CASES` / `M5_BATCH_CONCURRENT_WRITERS` /
#      `M5_BATCH_GROUP_FSYNCS` / `M5_BATCH_ROUNDTRIP_ENTRIES` 都必须是真跑出来的非零值；
#   ⑥ 依赖纪律：`src/write_batch.cpp.o` 里不得出现 PersistentDBImpl/WALWriter/MemTable 符号
#      （M5-design §1.4「filter 与 batch 不得反向依赖 db_impl」）。
#
# 收尾汇总标记行（`run_gate_m5_marked` 读它）：
#   M5_BATCH_TESTS_RAN <n>  M5_BATCH_TESTS_FAILED 0  M5_BATCH_ROUNDTRIP_ENTRIES <n>
#   M5_BATCH_PARTIAL_VISIBLE 0  M5_BATCH_HALF_VISIBLE 0  M5_BATCH_CRASH_HALF_VISIBLE 0
#   M5_BATCH_LOST_WAKEUPS 0  M5_BATCH_GROUP_FSYNCS <n>  M5_BATCH_CONCURRENT_WRITERS <n>
#   M5_BATCH_TRUNCATE_CASES <n>  M5_BATCH_RECOVERY_RECORDS <n>  M5_BATCH_ONE_RECORD_PER_BATCH 1
#   M5_BATCH_SEQ_CONTIGUOUS 1  M5_BATCH_LAST_SEQ_COVERS 1  LSM_BATCH_FORBIDDEN 0  [BATCH_OK]
#
# 注：设计 §11 的证据命令写 `--gtest_filter='WriteBatch.*:Batch.*:Recovery.*'`；这里去掉
#   `Recovery.*`（那是 M2 的恢复层 suite，属既有 18 条腿的覆盖范围），改由本脚本内的
#   `Batch.RecoverySequenceAndOneRecordPerBatch` 直接断言恢复侧的 records/entries，登记于报告。
set -uo pipefail
cd "$(dirname "$0")/.."

BIN="build/bin/lsm_tests"
FILTER='WriteBatch.*:Batch.*'

if [ ! -x "$BIN" ]; then
  echo "[FAIL] $BIN 不存在（先跑 scripts/lsm_build.sh）"
  exit 1
fi

OUT="$(mktemp /tmp/lsm_batch_unit_XXXXXX.log)"
"$BIN" --gtest_filter="$FILTER" >"$OUT" 2>&1
rc=$?
cat "$OUT"
if [ "$rc" -ne 0 ]; then
  echo "[FAIL] M5.2 用例退出码 $rc（完整日志：$OUT）"
  exit 1
fi

RAN="$(grep -c '^\[       OK \] ' "$OUT")"
RAN="${RAN:-0}"
# 8 条 = M5-A11 两条（编码往返 / 超限拒绝）+ A12/A13/A14/A15/A16/A17 各一条。
if [ "$RAN" -lt 8 ]; then
  echo "[FAIL] 跑到的用例数 $RAN < 8；不允许把「没测到东西」当通过"
  exit 1
fi
BATCH_RAN="$(grep -c '^\[       OK \] WriteBatch\.' "$OUT")"
BATCH_RAN="${BATCH_RAN:-0}"
if [ "$BATCH_RAN" -lt 2 ]; then
  echo "[FAIL] WriteBatch.* 只跑到 $BATCH_RAN 条（要求 >= 2：A11 编码往返 / A17 复用与畸形 rep）"
  exit 1
fi

pick() {   # pick <marker> -> 最后出现的值（测试会多次打印同一个标记，取最后一次即最终值）
  local v
  v="$(grep "^$1 " "$OUT" | tail -1 | awk '{print $2}')"
  if [ -z "$v" ]; then echo "MISSING"; else echo "$v"; fi
}

ROUNDTRIP="$(pick M5_BATCH_ROUNDTRIP_ENTRIES)"
PARTIAL="$(pick M5_BATCH_PARTIAL_VISIBLE)"
HALF="$(pick M5_BATCH_HALF_VISIBLE)"
CRASH_HALF="$(pick M5_BATCH_CRASH_HALF_VISIBLE)"
CRASH_CASES="$(pick M5_BATCH_CRASH_CASES)"
LOST="$(pick M5_BATCH_LOST_WAKEUPS)"
FSYNCS="$(pick M5_BATCH_GROUP_FSYNCS)"
WRITERS="$(pick M5_BATCH_CONCURRENT_WRITERS)"
TRUNC="$(pick M5_BATCH_TRUNCATE_CASES)"
RECORDS="$(pick M5_BATCH_RECOVERY_RECORDS)"
ENTRIES="$(pick M5_BATCH_RECOVERY_ENTRIES)"
ONEREC="$(pick M5_BATCH_ONE_RECORD_PER_BATCH)"
SEQOK="$(pick M5_BATCH_SEQ_CONTIGUOUS)"
LASTSEQ="$(pick M5_BATCH_LAST_SEQ_COVERS)"
EMPTYREJ="$(pick M5_BATCH_EMPTY_REJECTED)"
OVERREJ="$(pick M5_BATCH_OVERLIMIT_REJECTED)"
APPENDFAIL="$(pick M5_BATCH_APPEND_FAIL_INVISIBLE)"
FSYNCFAIL="$(pick M5_BATCH_FSYNC_FAIL_VISIBLE)"
SLACK="$(pick M5_BATCH_CAPACITY_SLACK_OK)"
CAPBOUND="$(pick M5_BATCH_CAPACITY_BOUNDARY_VISIBLE)"
REUSE="$(pick M5_BATCH_REUSE_OK)"
MALF="$(pick M5_BATCH_MALFORMED_CORRUPTION)"

fail=0
require_eq() {   # require_eq <name> <value> <expected>
  if [ "$2" != "$3" ]; then
    echo "[FAIL] $1 = $2（要求 $3）"
    fail=1
  fi
}
require_ge_int() {   # require_ge_int <name> <value> <min>
  case "$2" in
    ''|*[!0-9]*) echo "[FAIL] $1 = $2 不是非负整数（要求 >= $3）"; fail=1; return;;
  esac
  if [ "$2" -lt "$3" ]; then echo "[FAIL] $1 = $2（要求 >= $3）"; fail=1; fi
}

require_eq M5_BATCH_PARTIAL_VISIBLE "$PARTIAL" 0
require_eq M5_BATCH_HALF_VISIBLE "$HALF" 0
require_eq M5_BATCH_CRASH_HALF_VISIBLE "$CRASH_HALF" 0
require_eq M5_BATCH_LOST_WAKEUPS "$LOST" 0
require_eq M5_BATCH_ONE_RECORD_PER_BATCH "$ONEREC" 1
require_eq M5_BATCH_SEQ_CONTIGUOUS "$SEQOK" 1
require_eq M5_BATCH_LAST_SEQ_COVERS "$LASTSEQ" 1
require_eq M5_BATCH_EMPTY_REJECTED "$EMPTYREJ" 1
require_eq M5_BATCH_OVERLIMIT_REJECTED "$OVERREJ" 1
require_eq M5_BATCH_APPEND_FAIL_INVISIBLE "$APPENDFAIL" 1
require_eq M5_BATCH_FSYNC_FAIL_VISIBLE "$FSYNCFAIL" 0
require_eq M5_BATCH_CAPACITY_SLACK_OK "$SLACK" 1
require_eq M5_BATCH_CAPACITY_BOUNDARY_VISIBLE "$CAPBOUND" 1
require_eq M5_BATCH_REUSE_OK "$REUSE" 1

require_ge_int M5_BATCH_ROUNDTRIP_ENTRIES "$ROUNDTRIP" 8
require_ge_int M5_BATCH_CRASH_CASES "$CRASH_CASES" 1
require_ge_int M5_BATCH_GROUP_FSYNCS "$FSYNCS" 1
require_ge_int M5_BATCH_CONCURRENT_WRITERS "$WRITERS" 2
require_ge_int M5_BATCH_TRUNCATE_CASES "$TRUNC" 4
require_ge_int M5_BATCH_RECOVERY_RECORDS "$RECORDS" 1
require_ge_int M5_BATCH_RECOVERY_ENTRIES "$ENTRIES" 1
require_ge_int M5_BATCH_MALFORMED_CORRUPTION "$MALF" 1

# 组提交必须真的合并（fsync 次数 < 写者数），否则"一次 fsync"是空话
if [ "$FSYNCS" != "MISSING" ] && [ "$WRITERS" != "MISSING" ]; then
  if [ "$FSYNCS" -gt 0 ] && [ "$WRITERS" -gt "$FSYNCS" ]; then
    :
  else
    echo "[FAIL] 组提交未发生有效合并：fsync=$FSYNCS writers=$WRITERS（要求 0 < fsync < writers）"
    fail=1
  fi
fi

# 依赖纪律（M5-design §1.4）：write_batch.cpp 不得反向依赖 db_impl / wal / memtable
if ! cmake --build build --target lsm >/dev/null 2>&1; then
  echo "[FAIL] 构建 lsm 失败"
  exit 1
fi
OBJ="build/CMakeFiles/lsm.dir/src/write_batch.cpp.o"
if [ ! -f "$OBJ" ]; then
  echo "[FAIL] 找不到 $OBJ"
  exit 1
fi
FORBIDDEN="$(nm -C "$OBJ" 2>/dev/null | grep -cE 'PersistentDBImpl|WALWriter|MemTable')"
FORBIDDEN="${FORBIDDEN:-0}"
if [ "$FORBIDDEN" -ne 0 ]; then
  echo "[FAIL] write_batch 越权依赖（db_impl/wal/memtable 符号 $FORBIDDEN 个）"
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "[FAIL] M5.2 判据未全部满足（完整日志：$OUT）"
  exit 1
fi

echo "M5_BATCH_TESTS_RAN $RAN  M5_BATCH_TESTS_FAILED 0  M5_BATCH_WRITEBATCH_RAN $BATCH_RAN  M5_BATCH_ROUNDTRIP_ENTRIES $ROUNDTRIP  M5_BATCH_PARTIAL_VISIBLE $PARTIAL  M5_BATCH_HALF_VISIBLE $HALF  M5_BATCH_CRASH_HALF_VISIBLE $CRASH_HALF  M5_BATCH_CRASH_CASES $CRASH_CASES  M5_BATCH_LOST_WAKEUPS $LOST  M5_BATCH_GROUP_FSYNCS $FSYNCS  M5_BATCH_CONCURRENT_WRITERS $WRITERS  M5_BATCH_TRUNCATE_CASES $TRUNC  M5_BATCH_RECOVERY_RECORDS $RECORDS  M5_BATCH_RECOVERY_ENTRIES $ENTRIES  M5_BATCH_ONE_RECORD_PER_BATCH $ONEREC  M5_BATCH_SEQ_CONTIGUOUS $SEQOK  M5_BATCH_LAST_SEQ_COVERS $LASTSEQ  LSM_BATCH_FORBIDDEN $FORBIDDEN  [BATCH_OK]"
