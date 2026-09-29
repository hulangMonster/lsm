#!/usr/bin/env bash
# B03：真实 WAL 上的逐字节截断扫描（判据见 docs/m2-design.md §9.2 B03）
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d /tmp/lsm_tail_XXXXXX)
trap 'rm -rf "$W"' EXIT
echo "== lsm_tail_truncate_test: dir=$W =="
./build/bin/lsm_damage_test tail "$W/db"
RC=$?
echo "退出码: $RC"
exit $RC
