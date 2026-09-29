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
