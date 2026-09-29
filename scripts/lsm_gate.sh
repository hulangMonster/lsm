#!/usr/bin/env bash
# scripts/lsm_gate.sh —— M2+M3 全部门禁的唯一入口（docs/m2-prerequisites.md §8 + docs/m3-design.md §11.3）
#
# 为什么需要它：M2 的验收由多条互相独立的门禁组成（干净重建 / ASan / TSan / 崩溃对账 /
# 截断扫描 / 中间损坏）。分散跑容易漏，评审者也难以确认「到底跑了哪些」。
#
# 本版（M3 起）新增的核心机制：**正向标记判定**（run_gate_marked）。
# 起因是 docs/m3-prerequisites.md §9 的 D9.6：旧版 run_gate 只看退出码，而"什么都没发生也算成功"
# 会让门禁变成**空绿**（M2 已经踩过一次：`$((...+M))` 在 M 为空时静默跳过检查）。
#   ⇒ 凡是有"必须真的发生过某件事"语义的腿，都必须改为：命令成功 **且** 输出里出现预期标记。
#   每条腿用**独立日志文件**判定，避免标记在上一条腿的输出里被误匹配。
#   M3 的腿在脚本尚未交付时打印 [SKIP] 并计入"未验证"；加 --require-m3 时 SKIP 直接判 FAIL。
#
#   --rounds N     崩溃对账轮数（默认 100）
#   --with-tsan    额外跑 TSan（默认关闭：全量在 TSan 下约 6 分钟）
#   --no-asan      跳过 ASan
#   --require-m3   要求 M3 的腿**必须存在且通过**（缺一即 FAIL）；默认缺失记为 SKIP
set -u
cd "$(dirname "$0")/.."
ROUNDS=100
WITH_TSAN=0
WITH_ASAN=1
REQUIRE_M3=0
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --with-tsan) WITH_TSAN=1; shift;;
    --no-asan) WITH_ASAN=0; shift;;
    --require-m3) REQUIRE_M3=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
LOGDIR=$(mktemp -d /tmp/lsm_gate_XXXXXX)
LOG="$LOGDIR/gate.log"
FAILED=0
SKIPPED=0
declare -a RESULTS

run_gate() {
  local name="$1"; shift
  echo "=== [gate] $name ===" | tee -a "$LOG"
  local leg="$LOGDIR/leg_$(echo "$name" | tr -c 'A-Za-z0-9' '_').log"
  if "$@" >"$leg" 2>&1; then
    cat "$leg" >>"$LOG"
    RESULTS+=("PASS  $name")
    echo "--- [PASS] $name"
  else
    cat "$leg" >>"$LOG"
    RESULTS+=("FAIL  $name")
    FAILED=1
    echo "--- [FAIL] $name（详见 $LOG）"
    tail -20 "$leg"
  fi
}

# 正向标记版：命令成功 **且** 这条腿自己的输出里匹配到全部标记（标记之间用 @@ 分隔，AND 语义）。
# 为什么必须 AND：`MISSING 0` 这种标记会在"100 轮里坏 1 轮"时命中别的好轮次 ⇒ 假通过。
# 因此一律用**收尾汇总行**里的总量标记（如 `MISSING_TOTAL 0 MISMATCH_TOTAL 0`），并配一个正向计数
# （如 `ROUNDS_OK 1[0-9]*`）确保"真的跑了"。
run_gate_marked() {
  local name="$1"; local marker_spec="$2"; shift 2
  echo "=== [gate] $name （要求标记 /$marker_spec/）===" | tee -a "$LOG"
  local leg="$LOGDIR/marked_$(echo "$name" | tr -c 'A-Za-z0-9' '_').log"
  local ok=1
  "$@" >"$leg" 2>&1 || ok=0
  local m
  while IFS= read -r m; do
    [ -z "$m" ] && continue
    grep -Eq "$m" "$leg" || { ok=0; echo "--- [gate] 缺少标记 /$m/" | tee -a "$LOG"; }
  done < <(printf '%s' "$marker_spec" | tr '@' '\n' | grep -v '^$')
  if [ "$ok" = "1" ]; then
    cat "$leg" >>"$LOG"
    RESULTS+=("PASS  $name")
    echo "--- [PASS] $name"
  else
    cat "$leg" >>"$LOG"
    RESULTS+=("FAIL  $name（退出码或正向标记 /$marker_spec/ 不满足）")
    FAILED=1
    echo "--- [FAIL] $name（正向标记未全部出现或命令失败，详见 $LOG）"
    tail -20 "$leg"
  fi
}

# M3 的腿：脚本尚未交付时记 SKIP（绝不静默变绿）；--require-m3 时记 FAIL。
run_gate_m3_marked() {
  local name="$1"; local script="$2"; local marker="$3"; shift 3
  if [ ! -f "$script" ]; then
    if [ "$REQUIRE_M3" = "1" ]; then
      RESULTS+=("FAIL  $name（缺 $script）")
      FAILED=1
      echo "=== [gate] $name ===
--- [FAIL] $name：$script 不存在（--require-m3 要求必须存在）"
    else
      RESULTS+=("SKIP  $name（$script 尚未交付）")
      SKIPPED=$((SKIPPED + 1))
      echo "=== [gate] $name ===
--- [SKIP] $name：$script 尚未交付（M3.3）；加 --require-m3 可把它变成硬失败"
    fi
    return
  fi
  run_gate_marked "$name" "$marker" bash "$script" "$@"
}

echo "== lsm_gate: rounds=$ROUNDS asan=$WITH_ASAN tsan=$WITH_TSAN require_m3=$REQUIRE_M3 log=$LOG =="

# ---------------- M2 腿（不得退化；全部改为"退出码 + 正向标记"） ----------------
run_gate_marked "干净重建 + 0 warning + 全量用例" '\[  PASSED  \] [1-9][0-9]* tests' bash scripts/lsm_build.sh
if [ "$WITH_ASAN" = "1" ]; then
  run_gate_marked "ASan 全量" '\[  PASSED  \] [1-9][0-9]* tests' bash -c \
    'cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-asan -j8 >/dev/null && ./build-asan/bin/lsm_tests'
fi
if [ "$WITH_TSAN" = "1" ]; then
  # TSan 的判据是"零 race"，属于**反向**判据：退出码为 0 且报告数为 0；这里用退出码，
  # 报告数由 verify 脚本单独 grep（见 scripts/verify_m3_1.sh 的 F 段）。
  run_gate_marked "TSan 全量（setarch 关 ASLR）" '\[  PASSED  \] [1-9][0-9]* tests' bash -c \
    'cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build build-tsan -j8 >/dev/null && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests'
fi
run_gate_marked "崩溃对账（kill -9 x $ROUNDS，sync 模式）" \
  'MISSING_TOTAL 0 MISMATCH_TOTAL 0@@ROUNDS_OK [1-9][0-9]*' \
  bash scripts/lsm_crash_test.sh --rounds "$ROUNDS" --mode sync
run_gate_marked "逐字节截断扫描（B03）" 'TAIL_OK [1-9][0-9]* TAIL_FAIL 0' bash scripts/lsm_tail_truncate_test.sh
run_gate_marked "中间损坏拒绝启动（B04）" 'MIDDLE_OPEN_CORRUPTION [1-9]' bash scripts/lsm_corrupt_middle_test.sh

# ---------------- M3 腿（docs/m3-design.md §10.2 的 B01/B03/B04/B05） ----------------
# ① B01：flush 崩溃对账。必须证明"真的发生过 flush（SST 文件 > 0）且真的删过 log"，
#    否则就是空绿 —— 这正是 M2 评审阻断项 ③ 的同类形态。
run_gate_m3_marked "M3-B01 flush 崩溃对账（kill -9 x $ROUNDS）" scripts/lsm_flush_crash_test.sh \
  'SST_FILES_TOTAL [1-9][0-9]*@@LOGS_DELETED_TOTAL [1-9][0-9]*@@MISSING_TOTAL 0@@\[FLUSH_CRASH_OK\]' \
  --rounds "$ROUNDS" --write-buffer-size 262144
# ② B03：落盘重启（仅靠 SSTable 可读）
run_gate_m3_marked "M3-B03 落盘重启（records_replayed == 0）" scripts/lsm_flush_restart_test.sh \
  'RECORDS_REPLAYED 0@@\[FLUSH_RESTART_OK\]' --write-buffer-size 262144
# ③ B04：SSTable 单字节翻转扫描（零静默错值）
run_gate_m3_marked "M3-B04 SSTable 损坏扫描（零静默错值）" scripts/lsm_sst_damage_test.sh \
  'SST_DAMAGE_CASES [1-9][0-9]*@@SILENT_WRONG 0' --cases 2000
# ④ B05：文件句柄不泄漏
run_gate_m3_marked "M3-B05 句柄计数不增长" scripts/lsm_fd_leak_test.sh \
  'FD_GROWTH [0-9]+'

echo
echo "==== lsm_gate 汇总 ===="
for line in "${RESULTS[@]}"; do echo "$line"; done
if [ "$FAILED" -ne 0 ]; then
  echo "[FAIL] 有门禁未通过（完整日志：$LOG）"
  exit 1
fi
if [ "$SKIPPED" -ne 0 ]; then
  echo "[PARTIAL] 全部已运行的腿通过，但有 $SKIPPED 条 M3 腿因脚本未交付被跳过（未验证，不是通过）"
  exit 0
fi
echo "[OK] 全部门禁通过"
