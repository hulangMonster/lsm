#!/usr/bin/env bash
# B04：中间损坏必须拒绝启动且可定位（判据见 docs/m2-design.md §9.2 B04）
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d /tmp/lsm_mid_XXXXXX)
trap 'rm -rf "$W"' EXIT
echo "== lsm_corrupt_middle_test: dir=$W =="
./build/bin/lsm_damage_test middle "$W/db"
RC=$?
echo "退出码: $RC"
exit $RC
