#!/usr/bin/env bash
# scripts/lsm_gate.sh —— M2 全部门禁的唯一入口（docs/m2-prerequisites.md §8 的可执行版本）
#
# 为什么需要它：M2 的验收由 6 条互相独立的门禁组成（干净重建 / ASan / TSan / 崩溃对账 /
# 截断扫描 / 中间损坏）。分散跑容易漏，评审者也难以确认「到底跑了哪些」。
# 本脚本把每条都跑一遍、逐条打印 PASS/FAIL，最后给汇总并以退出码反映结论。
#
#   --rounds N     崩溃对账轮数（默认 100）
#   --with-tsan    额外跑 TSan（默认关闭：1M 压力在 TSan 下约 6 分钟）
#   --no-asan      跳过 ASan
set -u
cd "$(dirname "$0")/.."
ROUNDS=100
WITH_TSAN=0
WITH_ASAN=1
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --with-tsan) WITH_TSAN=1; shift;;
    --no-asan) WITH_ASAN=0; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
LOG=$(mktemp -d /tmp/lsm_gate_XXXXXX)/gate.log
FAILED=0
declare -a RESULTS
run_gate() {
  local name="$1"; shift
  echo "=== [gate] $name ===" | tee -a "$LOG"
  if "$@" >>"$LOG" 2>&1; then
    RESULTS+=("PASS  $name")
    echo "--- [PASS] $name"
  else
    RESULTS+=("FAIL  $name")
    FAILED=1
    echo "--- [FAIL] $name（详见 $LOG）"
    tail -20 "$LOG"
  fi
}
echo "== lsm_gate: rounds=$ROUNDS asan=$WITH_ASAN tsan=$WITH_TSAN log=$LOG =="

run_gate "干净重建 + 0 warning + 全量用例" bash scripts/lsm_build.sh
if [ "$WITH_ASAN" = "1" ]; then
  run_gate "ASan 全量" bash -c 'cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-asan -j8 >/dev/null && ./build-asan/bin/lsm_tests'
fi
if [ "$WITH_TSAN" = "1" ]; then
  run_gate "TSan 全量（setarch 关 ASLR）" bash -c 'cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-tsan -j8 >/dev/null && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests'
fi
run_gate "崩溃对账（kill -9 x $ROUNDS，sync 模式）" bash scripts/lsm_crash_test.sh --rounds "$ROUNDS" --mode sync
run_gate "逐字节截断扫描（B03）" bash scripts/lsm_tail_truncate_test.sh
run_gate "中间损坏拒绝启动（B04）" bash scripts/lsm_corrupt_middle_test.sh

echo
echo "==== lsm_gate 汇总 ===="
for line in "${RESULTS[@]}"; do echo "$line"; done
if [ "$FAILED" -ne 0 ]; then
  echo "[FAIL] 有门禁未通过（完整日志：$LOG）"
  exit 1
fi
echo "[OK] 全部门禁通过"
