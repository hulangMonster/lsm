#!/usr/bin/env bash
# scripts/bench_lsm_selftest.sh —— M5.3 / M5-B03：bench_lsm.sh 的失败注入自测（docs/m5-design.md §7.4）
#
# 防空绿：如果 bench_lsm.sh 永远返回 0，本脚本会失败。判据：
#   ① `bench_lsm.sh --inject-missing` 的退出码必须是 1（不是 0、不是 2）；
#   ② 结果文件里必须出现非零的 `BENCH_MISSING_TOTAL`（真的把 missing 做出来了）；
#   ③ 必须打印 `BENCH_INJECT_MISSING_RC 1`。
# 全部满足才打印 `[BENCH_SELFTEST_OK]` 并退出 0。
#
# 用法：bash scripts/bench_lsm_selftest.sh
set -uo pipefail
cd "$(dirname "$0")/.."

WORK="$(mktemp -d /tmp/lsm_bench_selftest_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
OUT="$WORK/out.txt"

bash scripts/bench_lsm.sh --inject-missing \
  --dataset 2000 --value-size 100 --batch 1 --pipeline 1 --sync 0 --filter on \
  --repeats 1 --warmup 100 --engines lsm --expected-min-cells 4 \
  --write-buffer-size 262144 --out "$OUT" >"$WORK/stdout.txt" 2>&1
RC=$?
cat "$WORK/stdout.txt"
cat "$OUT"

if [ "$RC" -ne 1 ]; then
  echo "[FAIL] bench_lsm.sh --inject-missing 退出码 $RC（要求 1；0 或 2 都说明注入/参数路径有问题）" >&2
  exit 1
fi
if ! grep -Eq 'BENCH_MISSING_TOTAL [1-9][0-9]*' "$OUT"; then
  echo "[FAIL] 结果文件没有非零的 BENCH_MISSING_TOTAL（注入没生效）" >&2
  exit 1
fi
if ! grep -q 'BENCH_INJECT_MISSING_RC 1' "$OUT"; then
  echo "[FAIL] 缺少 BENCH_INJECT_MISSING_RC 1" >&2
  exit 1
fi

echo "BENCH_INJECT_MISSING_RC 1"
echo "[BENCH_SELFTEST_OK]"
