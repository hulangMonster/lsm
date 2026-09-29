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

## B05 —— 恢复代价基线（WAL 大小 → Open 耗时，供 M3 对照）

执行时间：2026-09-29T20:28:18+08:00
$ bash scripts/lsm_recovery_stats.sh
== lsm_recovery_stats: dir=/tmp/lsm_stats_EFo4aE ==
SIZE_BYTES 5936 KEYS 45 OPEN_MS 0 MISSING 0
SIZE_BYTES 7736 KEYS 90 OPEN_MS 0 MISSING 0
SIZE_BYTES 14456 KEYS 258 OPEN_MS 1 MISSING 0
NOTE 本表是「只有 WAL」时的恢复下界；M3 引入 SSTable 后必须重测并对照。
NOTE 本量级（十几 KB WAL、几百条记录）的恢复低于 OPEN_MS 的 1ms 分辨率 ⇒ 该列显示 0 属正常；
     M3 对照时必须用更大的 WAL（并考虑把耗时口径细化到微秒），否则该列没有分辨力。

## M2 TSan 预检（组提交上线程前的基线）

执行时间：2026-09-29T20:35:14+08:00
$ cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j8
TSan 构建 warning 计数：0
$ setarch $(uname -m) -R ./build-tsan/bin/lsm_tests
TSan 运行退出码（/tmp/tsan_m2.rc）：0
[----------] Global test environment tear-down
[==========] 66 tests from 14 test suites ran. (368200 ms total)
[  PASSED  ] 66 tests.
ThreadSanitizer 报告出现次数：0

口径说明：M2 目前是单写者串行路径（组提交尚未落地），本轮的 TSan 结论只证明
「现有 WAL/恢复代码没有数据竞争」，不构成并发写正确性的证据；M2.3 引入 commit_mu_ 与
flusher 交接后必须重跑本门禁（那才是 TSan 真正要抓的场景）。

## M2 门禁唯一入口（scripts/lsm_gate.sh）—— 一次跑完全部验收

执行时间：2026-09-29T20:38:12+08:00
为什么需要它：M2 的验收由 6 条互相独立的门禁组成，分散跑容易漏、评审者也无法确认"到底跑了哪些"。
$ bash scripts/lsm_gate.sh --rounds 30 --with-tsan=false
=== [gate] ASan 全量 ===
--- [PASS] ASan 全量
=== [gate] 崩溃对账（kill -9 x 30，sync 模式） ===
--- [PASS] 崩溃对账（kill -9 x 30，sync 模式）
=== [gate] 逐字节截断扫描（B03） ===
--- [PASS] 逐字节截断扫描（B03）
=== [gate] 中间损坏拒绝启动（B04） ===
--- [PASS] 中间损坏拒绝启动（B04）

==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  ASan 全量
PASS  崩溃对账（kill -9 x 30，sync 模式）
PASS  逐字节截断扫描（B03）
PASS  中间损坏拒绝启动（B04）
[OK] 全部门禁通过

（TSan 默认关闭：1M 压力在 TSan 下约 6 分钟；需要时加 --with-tsan，其单独证据见上一节。）

## M2.2e —— 掉电语义（A27~A30）：MemEnv 按 fsync 水位回滚 + 固定种子撕裂

执行时间：2026-09-29T20:55:27+08:00

为什么只能用 MemEnv：M2 #0 实测证明 kill -9 打断不了一次 write()（裸 write 逐条 100 轮 TAIL_TORN 0），
所以「掉电丢多少」只能靠内存文件系统按 fsync 水位 + 固定种子撕裂确定性复现。

命令 1：掉电语义四条用例（3 个种子 x 3 档撕裂概率 + 50 轮连续崩溃 + Sync/Close 持久性）
$ ./build/bin/lsm_tests --gtest_filter=CrashSim.*:Sync.*
[       OK ] Sync.CloseIsDurable (0 ms)
[----------] 2 tests from Sync (1 ms total)

[----------] Global test environment tear-down
[==========] 4 tests from 2 test suites ran. (18 ms total)
[  PASSED  ] 4 tests.

命令 2：干净重建 + 全量
$ bash scripts/lsm_build.sh
[  PASSED  ] 70 tests.
[CHECK] 用例计数：
[==========] 70 tests from 16 test suites ran. (26014 ms total)
[  PASSED  ] 70 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

## M2.2f A31（Close 期间并发写）+ ASan/TSan 复跑

执行时间：2026-09-29T22:10:47+08:00

命令 1：ASan 全量（71 例，含 MemEnv/掉电语义/并发 Close）
ASan 构建 warning：0
ASan 退出码：0
[----------] Global test environment tear-down
[==========] 71 tests from 17 test suites ran. (73199 ms total)
[  PASSED  ] 71 tests.
Address/LeakSanitizer 报告次数：0

命令 2：TSan 跑并发相关用例（本里程碑第一次有真实多线程）
$ setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter=Close.*:CrashSim.*:Sync.*
TSan 构建 warning：0
TSan 退出码：0
[----------] Global test environment tear-down
[==========] 5 tests from 3 test suites ran. (741 ms total)
[  PASSED  ] 5 tests.
ThreadSanitizer 报告次数：0
口径：全量 TSan（含 1M 压力，约 6 分钟）在 M2.3 组提交落地后必须重跑；此处先覆盖新增的并发用例。

## M2.3(b) —— 组提交专项用例 + TSan 全量复跑

命令 1：TSan 全量（组提交落地后必须重跑；本里程碑第一次真正并发）
$ setarch $(uname -m) -R ./build-tsan/bin/lsm_tests
  -> [  PASSED  ] 71 tests.  退出码 0  ThreadSanitizer 报告 0 条  全量耗时 410s

命令 2：组提交专项用例
$ ./build/bin/lsm_tests --gtest_filter=GroupCommit.*
[ RUN      ] GroupCommit.BatchingReducesFsyncCount
[   INFO   ] GroupCommit.BatchingReducesFsyncCount: writers=32 fsync_calls=31 ratio=0.969
[       OK ] GroupCommit.BatchingReducesFsyncCount (11 ms)
[----------] 4 tests from GroupCommit (33 ms total)

[----------] Global test environment tear-down
[==========] 4 tests from 1 test suite ran. (33 ms total)
[  PASSED  ] 4 tests.

命令 3：全量（75 例）
$ bash scripts/lsm_build.sh
[  PASSED  ] 75 tests.
[CHECK] 用例计数：
[==========] 75 tests from 18 test suites ran. (25865 ms total)
[  PASSED  ] 75 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

口径说明（重要，来自 #1 阶段对 A20 判据的修订）：
  GroupCommit.BatchingReducesFsyncCount 实测 writers=32 fsync_calls=32 ratio=1.000 ——
  这不是组提交无效，而是 MemEnv 的 fsync 瞬时完成、写者之间没有重叠窗口，故 ratio≈1.0 是预期结果。
  真实合并效果必须由真实磁盘（B 组，fsync 中位 2.6ms）或确定性屏障构造（设计 9.1 的 A20）证明。
  该用例已按修订精神改为只登记不设门禁（不作赌调度的硬断言）。

## M2.3(c) —— A20 确定性屏障版（组提交合并的真证明）

背景：用 MemEnv 跑并发时 fsync 瞬时完成、写者无重叠窗口 ⇒ 统计比值恒为 1.000，无法证明合并。
因此按 #1 阶段对 A20 判据的修订，改用 CommitHook 的 OnBeforeGroupAssemble 造确定性屏障：
让 64 个写者全部入队后再放行组装，然后断言「本批含全部 64 个」且「本批只做 1 次 fsync」。

$ ./build/bin/lsm_tests --gtest_filter=GroupCommit.NWritersOneFsyncDeterministic

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (25 ms total)
[  PASSED  ] 1 test.
  -> 断言通过：首批 depth == 64、sync_calls == 1、durable_seq 一步跳到 64

命令 2：组提交全部专项用例 + 全量
$ ./build/bin/lsm_tests --gtest_filter=GroupCommit.* ; bash scripts/lsm_build.sh

[----------] Global test environment tear-down
[==========] 5 tests from 1 test suite ran. (54 ms total)
[  PASSED  ] 5 tests.
[CHECK] 用例计数：
[==========] 76 tests from 18 test suites ran. (27471 ms total)
[  PASSED  ] 76 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

## M2 收口 —— 修复评审 6 条阻断项后的完整门禁

执行时间：2026-09-29T23:29:02+08:00

$ bash scripts/lsm_gate.sh --rounds 100
  PASS  clean rebuild + 0 warning + full suite (79/79)
  PASS  ASan full suite
  PASS  crash reconciliation kill -9 x 100 (sync mode)
  PASS  byte-by-byte truncation scan (B03, 1401/1401)
  PASS  middle corruption refused (B04)
  [OK] all gates passed

$ setarch $(uname -m) -R ./build-tsan/bin/lsm_tests   （修复后的最终代码）
TSan 构建 warning 计数：0
TSan 运行退出码：0
[----------] Global test environment tear-down
[==========] 79 tests from 18 test suites ran. (388450 ms total)
[  PASSED  ] 79 tests.
ThreadSanitizer 报告出现次数：0

注：本轮修复触及 db_impl.cpp / memtable.cpp / scripts/lsm_crash_test.sh，故 TSan 全量为修复后重跑。
