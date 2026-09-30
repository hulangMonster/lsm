#!/usr/bin/env bash
# scripts/lsm_m5_unit_test.sh —— M5.1 的正向标记腿（docs/m5-design.md §11 M5.1 的「判据/证据命令」）
#
# 判据（缺一即非零退出；门禁侧再用 run_gate_m5_marked 要求收尾汇总行里的标记全部出现）：
#   ① 过滤器选中的用例全部真跑且全绿（Filter.* 必须 >= 12 条：M5-A01~A10 + A18 + damage 扫描）；
#   ② `M5_FILTER_FALSE_NEGATIVE 0`（I47：零假阴性，**打印出来的计数**，不是「测试没失败」）；
#   ③ `M5_FILTER_SILENT_FALSE_NEGATIVE 0`（单字节翻转扫描不得产生静默假阴性）；
#   ④ 同轮开关门禁：`without >= 3` 且 `without / max(1, with) >= 3.0`（M5-C5 / §3.8）；
#   ⑤ `data_blocks_skipped_by_filter > 0`（§10.3 反空绿：filter 必须真的省了块读）；
#   ⑥ 扫描必须真的命中 filter 块并把它降级（`M5_FILTER_DAMAGE_FILTER_DEGRADED > 0`）；
#   ⑦ lsm_sstable 的依赖纪律：nm -C 里 db_impl/wal/version/memtable 符号计数 == 0（bloom 进纯格式层后仍成立）。
#
# 收尾汇总标记行（`run_gate_m5_marked` 读它）：
#   M5_TESTS_RAN <n>  M5_TESTS_FAILED 0  M5_FILTER_FALSE_NEGATIVE 0  M5_FILTER_SILENT_FALSE_NEGATIVE 0
#   M5_FILTER_BLOCK_READS_WITHOUT <n>  M5_FILTER_BLOCK_READS_WITH <n>  M5_FILTER_BLOCK_READ_RATIO <f>
#   M5_FILTER_BLOCKS_SKIPPED <n>  M5_FILTER_DAMAGE_CASES <n>  M5_FILTER_DAMAGE_FILTER_DEGRADED <n>
#   LSM_SSTABLE_FORBIDDEN 0  [FILTER_OK]  [FILTER_DAMAGE_OK]
#
# 注：设计 §11 的证据命令写 `--gtest_filter='Filter.*:Bloom.*:Table.*:SSTable.*'`；本仓库**没有**
#   `SSTable.*` 这个 suite（M3.1 的格式层 suite 是 `Block.*`/`Footer.*`），因此这里用
#   `Filter.*:Table.*:Block.*:Footer.*`（登记于报告）。`Bloom.*` 在本仓库同样是空集合（策略用例
#   统一放在 `Filter.*`），故不列。
set -uo pipefail
cd "$(dirname "$0")/.."

BIN="build/bin/lsm_tests"
FILTER='Filter.*:Table.*:Block.*:Footer.*'

if [ ! -x "$BIN" ]; then
  echo "[FAIL] $BIN 不存在（先跑 scripts/lsm_build.sh）"
  exit 1
fi

OUT="$(mktemp /tmp/lsm_m5_unit_XXXXXX.log)"
"$BIN" --gtest_filter="$FILTER" >"$OUT" 2>&1
rc=$?
cat "$OUT"
if [ "$rc" -ne 0 ]; then
  echo "[FAIL] M5.1 用例退出码 $rc（完整日志：$OUT）"
  exit 1
fi

RAN="$(grep -c '^\[       OK \]' "$OUT")"
RAN="${RAN:-0}"
FILTER_RAN="$(grep -c '^\[       OK \] Filter\.' "$OUT")"
FILTER_RAN="${FILTER_RAN:-0}"
if [ "$RAN" -lt 12 ]; then
  echo "[FAIL] 跑到的用例数 $RAN < 12；不允许把「没测到东西」当通过"
  exit 1
fi
if [ "$FILTER_RAN" -lt 12 ]; then
  echo "[FAIL] Filter.* 只跑到 $FILTER_RAN 条（要求 >= 12：M5-A01~A10 + A18 + damage 扫描）"
  exit 1
fi

pick() {   # pick <marker> -> 最后出现的值（测试会多次打印同一个标记，取最后一次即最终值）
  local v
  v="$(grep "^$1 " "$OUT" | tail -1 | awk '{print $2}')"
  if [ -z "$v" ]; then echo "MISSING"; else echo "$v"; fi
}

FALSE_NEG="$(pick M5_FILTER_FALSE_NEGATIVE)"
SILENT_FN="$(pick M5_FILTER_SILENT_FALSE_NEGATIVE)"
WITHOUT="$(pick M5_FILTER_BLOCK_READS_WITHOUT)"
WITH="$(pick M5_FILTER_BLOCK_READS_WITH)"
RATIO="$(pick M5_FILTER_BLOCK_READ_RATIO)"
SKIPPED="$(pick M5_FILTER_BLOCKS_SKIPPED)"
DAMAGE_CASES="$(pick M5_FILTER_DAMAGE_CASES)"
DAMAGE_DEG="$(pick M5_FILTER_DAMAGE_FILTER_DEGRADED)"
FPR_PPM="$(pick M5_FILTER_FPR_PPM)"

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

require_eq M5_FILTER_FALSE_NEGATIVE "$FALSE_NEG" 0
require_eq M5_FILTER_SILENT_FALSE_NEGATIVE "$SILENT_FN" 0
require_ge_int M5_FILTER_BLOCK_READS_WITHOUT "$WITHOUT" 3
require_ge_int M5_FILTER_BLOCKS_SKIPPED "$SKIPPED" 1
require_ge_int M5_FILTER_DAMAGE_CASES "$DAMAGE_CASES" 1
require_ge_int M5_FILTER_DAMAGE_FILTER_DEGRADED "$DAMAGE_DEG" 1

# 比值门禁：without / max(1, with) >= 3.0（M5-C5 / §3.8；只比数据块读**次数**，不比墙钟）
RATIO_OK=0
if [ "$WITHOUT" != "MISSING" ] && [ "$WITH" != "MISSING" ]; then
  RATIO_OK="$(awk -v a="$WITHOUT" -v b="$WITH" 'BEGIN{ d=(b<1?1:b); printf "%d", (a/d >= 3.0) ? 1 : 0 }')"
fi
if [ "$RATIO_OK" != "1" ]; then
  echo "[FAIL] 块读下降门禁未过：without=$WITHOUT with=$WITH ratio=$RATIO（要求 >= 3.0；不得放宽判据，应放大数据集）"
  fail=1
fi

# lsm_sstable 的依赖纪律（M3.1 §5.1 的机制；M5.1 把 bloom.cpp 也放进该静态库后必须仍然成立）
if ! cmake --build build --target lsm_sstable >/dev/null 2>&1; then
  echo "[FAIL] 构建 lsm_sstable 失败"
  exit 1
fi
FORBIDDEN="$(nm -C build/liblsm_sstable.a 2>/dev/null | grep -cE 'db_impl|wal|version|memtable')"
FORBIDDEN="${FORBIDDEN:-0}"
if [ "$FORBIDDEN" -ne 0 ]; then
  echo "[FAIL] lsm_sstable 越权依赖（db_impl/wal/version/memtable 符号 $FORBIDDEN 个）"
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "[FAIL] M5.1 判据未全部满足（完整日志：$OUT）"
  exit 1
fi

echo "M5_TESTS_RAN $RAN  M5_TESTS_FAILED 0  M5_FILTER_RAN $FILTER_RAN  M5_FILTER_FALSE_NEGATIVE $FALSE_NEG  M5_FILTER_SILENT_FALSE_NEGATIVE $SILENT_FN  M5_FILTER_BLOCK_READS_WITHOUT $WITHOUT  M5_FILTER_BLOCK_READS_WITH $WITH  M5_FILTER_BLOCK_READ_RATIO $RATIO  M5_FILTER_BLOCKS_SKIPPED $SKIPPED  M5_FILTER_DAMAGE_CASES $DAMAGE_CASES  M5_FILTER_DAMAGE_FILTER_DEGRADED $DAMAGE_DEG  M5_FILTER_FPR_PPM $FPR_PPM  LSM_SSTABLE_FORBIDDEN $FORBIDDEN  [FILTER_OK]  [FILTER_DAMAGE_OK]"
