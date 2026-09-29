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

## M2.2b —— 恢复层确定性用例（A11~A19）与一个由它们抓到的真 bug

执行时间：2026-09-29T20:15:12+08:00

命令 1：bash scripts/lsm_build.sh（干净重建 + 0 warning + 全量）
$ bash scripts/lsm_build.sh
[  PASSED  ] 66 tests.
[CHECK] 用例计数：
[==========] 66 tests from 14 test suites ran. (28777 ms total)
[  PASSED  ] 66 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

命令 2：恢复层用例 A11~A19
$ ./build/bin/lsm_tests --gtest_filter=Recovery.*
[       OK ] Recovery.TornTailDoesNotResurrectOlderValue (4 ms)
[ RUN      ] Recovery.EmptyWalAndMissingDir
[       OK ] Recovery.EmptyWalAndMissingDir (2 ms)
[ RUN      ] Recovery.MalformedBatchPayloadIsCorruption
[       OK ] Recovery.MalformedBatchPayloadIsCorruption (5 ms)
[ RUN      ] Recovery.NonHighestLogTailCorruptionRejected
[       OK ] Recovery.NonHighestLogTailCorruptionRejected (3 ms)
[ RUN      ] Recovery.SameKeyManyVersionsReplaysLatest
[       OK ] Recovery.SameKeyManyVersionsReplaysLatest (9 ms)
[----------] 9 tests from Recovery (68 ms total)

[----------] Global test environment tear-down
[==========] 9 tests from 1 test suite ran. (68 ms total)
[  PASSED  ] 9 tests.

## 由 A17（畸形 batch）抓到的真 bug：恢复中途失败时析构崩溃

修复前（原始输出）：
$ timeout 60 ./build/bin/lsm_tests --gtest_filter=Recovery.MalformedBatchPayloadIsCorruption
timeout: the monitored command dumped core    # 退出码 134（SIGSEGV）

根因：RecoverAndOpen 在重放阶段构造了 PersistentDBImpl，畸形 batch 让解析失败并 return；
unique_ptr 析构 → Close() → 此时 log_ 仍是 nullptr（WAL 在重放的**后面**才打开）→ 对 nullptr 取 Sync 段错误。
修复：closed_ 初值改为 true（未完全打开就不做关闭动作）+ Sync/Close 对 log_ == nullptr 做显式判断。
影响面：任何「恢复中途失败」的路径（畸形 batch、重放 Add 失败）都会踩到，属阻断级。

## M2.2c —— 损坏注入的端到端判据（B03 逐字节截断扫描 / B04 中间损坏）

执行时间：2026-09-29T20:17:39+08:00

命令 1：B03 逐字节截断扫描（每条 record 边界 ±3 字节，共  个截断点）
$ bash scripts/lsm_tail_truncate_test.sh
== lsm_tail_truncate_test: dir=/tmp/lsm_tail_odWZQm ==
TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0 RECORD_BYTES 40
退出码: 0
  判据：Open 必须成功；恢复出的 key 集合必须恰好是前缀 [1..k]（k 由截断长度决定），不得多也不得少。

命令 2：B04 中间损坏（翻转第 50 条 record 的 payload 首字节，其后仍有 150 条完好 record）
$ bash scripts/lsm_corrupt_middle_test.sh
== lsm_corrupt_middle_test: dir=/tmp/lsm_mid_ZYezpS ==
MIDDLE_OPEN_CORRUPTION 1 RECOVERED_PREFIX -1 DETAIL Corruption: RecoverAndOpen: log 中间损坏（其后仍有完好 record）: /tmp/lsm_mid_ZYezpS/db/000001.log @1960 CRC 不符
MIDDLE_LOCATABLE 1
退出码: 0
  判据：必须返回 kCorruption（拒绝启动）、错误信息含文件名与字节偏移（可定位）——
  这正是「尾部残骸可安全截断」与「中间损坏必须拒绝」的分界线（design §5.3）。

## M2.3 前置 —— ASan 门禁复跑（覆盖 M2.2 新增的 9 条恢复用例）

执行时间：2026-09-29T20:21:36+08:00

$ cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests
ASan 构建 warning 计数：0
ASan 运行退出码：0
[----------] Global test environment tear-down
[==========] 66 tests from 14 test suites ran. (81366 ms total)
[  PASSED  ] 66 tests.
AddressSanitizer/LeakSanitizer/runtime error 报告出现次数：0

## B07 —— 单次提交延迟微基准（docs/m2-design.md §9.2/§9.3）

执行时间：2026-09-29T20:22:34+08:00

$ mkdir -p /tmp/fsb && ./build/bin/fsbench_commit_latency /tmp/fsb 300
== fsbench: 单次提交延迟（4096B 追加 + flush，N=300，目录 /tmp/fsb）==
machine 8 cores  load 0.68 1.18 1.19  fs see below  mount /tmp/fsb
NOTE 文件系统 ext4 挂载点 / 记录 4096B（预分配档先 posix_fallocate(64 MiB)）
STRATEGY append+fsync           N  300  MIN_MS   0.782  MEDIAN_MS   3.204  P90_MS   3.906  MAX_MS  11.551
STRATEGY append+fdatasync       N  300  MIN_MS   0.890  MEDIAN_MS   3.100  P90_MS   3.727  MAX_MS   5.531
STRATEGY prealloc+fsync         N  300  MIN_MS   0.749  MEDIAN_MS   2.918  P90_MS   3.652  MAX_MS   4.446
STRATEGY O_DIRECT+fsync         N  300  MIN_MS   0.716  MEDIAN_MS   2.819  P90_MS   3.332  MAX_MS   5.151
STRATEGY create+fsync+dirfsync  N  300  MIN_MS   0.717  MEDIAN_MS   3.305  P90_MS   3.973  MAX_MS   7.428
NOTE 每次提交 = 1 条 4096B 记录；本表的 MEDIAN_MS 是 M5 计算组提交收益的分母

说明：本表的 MEDIAN_MS 是 M5 计算组提交收益的分母；
      与 M2 #0 的独立探测（append+fsync p50 2.556ms）互相印证，且与历史记录的「约 8ms」差约 3 倍。

补充（同一脚本、同一机器、两次运行的差异，如实记录）：
  第一次 N=300：append+fsync MEDIAN_MS 2.491 / O_DIRECT+fsync 1.655
  第二次 N=300：append+fsync MEDIAN_MS 3.204 / O_DIRECT+fsync 2.819
  ⇒ 两次相差约 29%。这正是本工具必须打印 machine/load/fs/mount 的原因：
    同一个脚本、同一台机器、不同时间点结果可以差 3 倍，不记环境就无法解释差异（design §9.3 的经验）。
  ⇒ 结论口径：M5 引用分母时必须用**同轮同环境**的实测值，不得跨时间点挪用。



执行时间：2026-09-29T20:24:53+08:00
HEAD：21702a8 m2.2d: B07 单次提交延迟微基准（docs/m2-design.md §9.2/§9.3）

命令 1：bash scripts/lsm_build.sh（干净重建，含 B07 新目标）
$ bash scripts/lsm_build.sh
[CHECK] 用例计数：
[==========] 66 tests from 14 test suites ran. (28207 ms total)
[  PASSED  ] 66 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

命令 2：三条真实门禁的冒烟复核
$ bash scripts/lsm_crash_test.sh --rounds 20
TOTAL_ROUNDS 20 MISSING_TOTAL 0 MISMATCH_TOTAL 0
[OK] 全部 20 轮 missing 0 / mismatch 0
$ bash scripts/lsm_tail_truncate_test.sh
TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0 RECORD_BYTES 40
退出码: 0
$ bash scripts/lsm_corrupt_middle_test.sh
MIDDLE_OPEN_CORRUPTION 1 RECOVERED_PREFIX -1 DETAIL Corruption: RecoverAndOpen: log 中间损坏（其后仍有完好 record）: /tmp/lsm_mid_BpTgtJ/db/000001.log @1960 CRC 不符
MIDDLE_LOCATABLE 1
退出码: 0

构建产物：lsm_tests / lsm_crash_writer / lsm_crash_recover / lsm_damage_test / fsbench_commit_latency
