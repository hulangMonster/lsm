#!/usr/bin/env bash
# scripts/lsm_build.sh —— lsm-kv 唯一门禁脚本（docs/m1-design.md §3 / docs/m1-prerequisites.md §8）
#
# 做四件事，任何一步失败即非零退出：
#   1) 干净重建（rm -rf build，避免旧目标被复用的假通过）
#   2) cmake 配置 + 编译（-Wall -Wextra），完整输出 tee 到 build/build.log
#   3) 断言 warning 计数为 0
#   4) 运行 build/bin/lsm_tests 并打印用例计数
#
# 用法：bash scripts/lsm_build.sh
#      BUILD_DIR=build-asan bash scripts/lsm_build.sh   # 也可用于 sanitizer 目录
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD_DIR="${BUILD_DIR:-build}"
LOG="${BUILD_DIR}/build.log"
JOBS="${JOBS:-$(nproc)}"

echo "== lsm_build: root=${ROOT} build_dir=${BUILD_DIR} jobs=${JOBS} =="

# ---- 1) 干净重建 ----
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

rc=0
{
  echo "===== [$(date -Is)] cmake 配置（-DCMAKE_BUILD_TYPE=Release） ====="
  cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
} > "${LOG}" 2>&1 || rc=$?
if [ "${rc}" -ne 0 ]; then
  echo "[FAIL] cmake 配置失败（rc=${rc}），尾部输出：" >&2
  tail -40 "${LOG}" >&2
  echo "[INFO] 完整日志：${LOG}" >&2
  exit "${rc}"
fi

{
  echo "===== [$(date -Is)] cmake 构建（-j${JOBS}，make 后端） ====="
  cmake --build "${BUILD_DIR}" -j"${JOBS}"
} >> "${LOG}" 2>&1 || rc=$?
if [ "${rc}" -ne 0 ]; then
  echo "[FAIL] 构建失败（rc=${rc}），尾部 80 行输出：" >&2
  tail -80 "${LOG}" >&2
  echo "[INFO] 完整日志：${LOG}" >&2
  exit "${rc}"
fi

# ---- 3) 0 warning 断言 ----
# 注意：grep -c 无匹配时退出码为 1（stdout 仍打印 0），必须用 || true 兜住，
# 否则 set -e/-o pipefail 会把「0 个 warning」当成脚本失败。
WARN_COUNT="$(grep -c -- "warning:" "${LOG}" || true)"
WARN_COUNT="${WARN_COUNT:-0}"
echo "[CHECK] warning 计数 = ${WARN_COUNT}（要求 0）"
if [ "${WARN_COUNT}" -ne 0 ]; then
  echo "[FAIL] 存在 ${WARN_COUNT} 条 warning：" >&2
  grep -n -- "warning:" "${LOG}" | head -20 >&2
  exit 1
fi

# ---- 4) 跑单测并打印计数 ----
echo "===== [$(date -Is)] 运行 lsm_tests =====" | tee -a "${LOG}"
if ! "${ROOT}/${BUILD_DIR}/bin/lsm_tests" 2>&1 | tee -a "${LOG}"; then
  echo "[FAIL] lsm_tests 退出码非 0（见上）" >&2
  echo "[INFO] 完整日志：${LOG}" >&2
  exit 1
fi

echo "[CHECK] 用例计数："
grep -E "^\[==========\] [0-9]+ tests? from .* ran\." "${LOG}" || true
grep -E "^\[  (PASSED|FAILED)  \]" "${LOG}" || true

echo "[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：${LOG}）"
