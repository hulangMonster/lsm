# M2 实测证据（docs/m2-evidence.md）

> 逐子里程碑追加；全部为**原始输出摘录**（命令 + 输出），未转述、未编造。

## M2.1 —— WAL record 格式与跨块切分（docs/m2-design.md §4）

执行时间：2026-09-29T20:01:53+08:00

命令 1：bash scripts/lsm_build.sh（干净重建 + 0 warning 断言 + 全量用例）
$ bash scripts/lsm_build.sh
[==========] 57 tests from 13 test suites ran. (26329 ms total)
[  PASSED  ] 57 tests.
[CHECK] 用例计数：
[==========] 57 tests from 13 test suites ran. (26329 ms total)
[  PASSED  ] 57 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

命令 2：WAL 层用例 A01~A10
$ ./build/bin/lsm_tests --gtest_filter=WAL.*
[       OK ] WAL.CrcDetectsSingleByteFlip (277 ms)
[ RUN      ] WAL.RejectsIllegalTypeAndLength
[       OK ] WAL.RejectsIllegalTypeAndLength (9 ms)
[ RUN      ] WAL.TruncatedTailIsCut
[       OK ] WAL.TruncatedTailIsCut (851 ms)
[ RUN      ] WAL.MiddleCorruptionRejected
[       OK ] WAL.MiddleCorruptionRejected (6 ms)
[ RUN      ] WAL.TailCorruptionWithNoValidRecordAfterIsCut
[       OK ] WAL.TailCorruptionWithNoValidRecordAfterIsCut (2 ms)
[ RUN      ] WAL.ShortWriteIsRetriedThenFails
[       OK ] WAL.ShortWriteIsRetriedThenFails (1 ms)
[ RUN      ] WAL.FsyncFailurePropagates
[       OK ] WAL.FsyncFailurePropagates (0 ms)
[----------] 10 tests from WAL (1193 ms total)

[----------] Global test environment tear-down
[==========] 10 tests from 1 test suite ran. (1193 ms total)
[  PASSED  ] 10 tests.

命令 3：ASan（独立 build-asan 目录）
$ cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests
ASan 构建 warning 计数：0
ASan 运行退出码：0
[----------] Global test environment tear-down
[==========] 57 tests from 13 test suites ran. (75577 ms total)
[  PASSED  ] 57 tests.
AddressSanitizer/LeakSanitizer 报告出现次数：0

命令 4：跨块切分的真实片段布局（A02 的四组边界，直接看文件的 type 字节）
$ ./build/bin/lsm_tests --gtest_filter=WAL.LargeRecordCrossBlockSplit

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (10 ms total)
[  PASSED  ] 1 test.

命令 5：padding 边界（含 #1 回退修订的 block_offset = 32761，A03）
$ ./build/bin/lsm_tests --gtest_filter=WAL.BlockTailPadding

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (26 ms total)
[  PASSED  ] 1 test.

## M2.2 —— DB::Open 崩溃恢复与 kill -9 对账门禁（docs/m2-design.md §5/§7/§8）

执行时间：2026-09-29T20:10:25+08:00

命令 1：bash scripts/lsm_build.sh（干净重建 + 0 warning 断言 + 全量 57 例，含 M1 的 47 例不回归）
$ bash scripts/lsm_build.sh
[==========] 57 tests from 13 test suites ran. (24180 ms total)
[  PASSED  ] 57 tests.
[CHECK] 用例计数：
[==========] 57 tests from 13 test suites ran. (24180 ms total)
[  PASSED  ] 57 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

命令 2：kill -9 崩溃对账门禁（唯一可宣称 durable 的模式）
$ bash scripts/lsm_crash_test.sh --rounds 100 --mode sync
退出码：0
--- 前 4 轮 ---
== lsm_crash_test: rounds=100 mode=sync ack_sync_every=1 dir=/tmp/lsm_crash_87MHQu ==
ROUND 1 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 37 RECOVERED 37 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
ROUND 2 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 39 RECOVERED 39 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
ROUND 3 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 16 RECOVERED 16 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
ROUND 4 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 65 RECOVERED 65 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
--- 末 3 行 ---
ROUND 100 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 31 RECOVERED 31 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
TOTAL_ROUNDS 100 MISSING_TOTAL 0 MISMATCH_TOTAL 0
[OK] 全部 100 轮 missing 0 / mismatch 0
--- 每轮 ack 数（min / max，说明 kill -9 落在写进行中的不同阶段）---
4
73
--- 100/100 轮均为 MISSING 0 MISMATCH 0 的行数 ---
100
