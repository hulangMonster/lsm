#!/usr/bin/env bash
# scripts/lsm_manifest_test.sh —— M4.1 的正向标记腿（docs/m4-design.md §10.2 M4-B11 的 A 组部分）
#
# 判据（缺一即非零退出，门禁侧再要求这些标记全部出现）：
#   ① 过滤器选中的 M4.1 用例全部真跑（OK 行数 >= 1，退出码 0）；
#   ② lsm_version 静态库的依赖纪律：nm -C 里 db_impl/wal/memtable 符号计数 == 0（§1.4）。
# 收尾汇总标记行（门禁 run_gate_m3_marked 读它）：
#   M4_TESTS_RAN <n>  M4_TESTS_FAILED 0  LSM_VERSION_FORBIDDEN 0  [MANIFEST_OK]
set -uo pipefail
cd "$(dirname "$0")/.."

BIN="build/bin/lsm_tests"
FILTER='VersionEdit.*:Manifest.*:Current.*:Migration.*:Install.FailureKeepsCurrentRecoverable:Compaction.*:CompactionDb.*:Level0.*:LevelN.*:PickLevel.*:PickFile.*:L0Inputs.*:BaseLevelForKey.*:Drop.*:ReadLevels.*:Delete.*'

if [ ! -x "$BIN" ]; then
  echo "[FAIL] $BIN 不存在（先跑 scripts/lsm_build.sh）"
  exit 1
fi

OUT="$(mktemp /tmp/lsm_manifest_XXXXXX.log)"
"$BIN" --gtest_filter="$FILTER" >"$OUT" 2>&1
rc=$?
cat "$OUT"
if [ "$rc" -ne 0 ]; then
  echo "[FAIL] M4.1 用例退出码 $rc（完整日志：$OUT）"
  exit 1
fi

RAN="$(grep -c '^\[       OK \]' "$OUT" || true)"
if [ -z "$RAN" ]; then RAN=0; fi
if [ "$RAN" -lt 1 ]; then
  echo "[FAIL] 没有跑到任何 M4.1 用例（RAN=$RAN）；不允许把"没测到东西"当通过"
  exit 1
fi

if ! cmake --build build --target lsm_version >/dev/null 2>&1; then
  echo "[FAIL] 构建 lsm_version 失败"
  exit 1
fi
FORBIDDEN="$(nm -C build/liblsm_version.a 2>/dev/null | grep -cE 'db_impl|wal|memtable' || true)"
if [ -z "$FORBIDDEN" ]; then FORBIDDEN=0; fi
if [ "$FORBIDDEN" -ne 0 ]; then
  echo "[FAIL] lsm_version 越权依赖（db_impl/wal/memtable 符号 $FORBIDDEN 个）"
  exit 1
fi

echo "M4_TESTS_RAN $RAN  M4_TESTS_FAILED 0  LSM_VERSION_FORBIDDEN $FORBIDDEN  [MANIFEST_OK]"
