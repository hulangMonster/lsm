# M1 实测证据（docs/m1-evidence.md）

> 逐子里程碑追加。全部为**原始输出摘录**（命令 + 输出），未转述、未编造；失败也如实入档。

## #2 测试集缺陷：InternalKey.ParseMalformed 自相矛盾（修复前原始输出）

修复前运行 util 子集（命令见下方 M1.1 小节）得到：

```
[ RUN      ] InternalKey.ParseMalformed
/tmp/m11/util_only_test.cpp:990: Failure
Value of: ParseInternalKey(raw, &user_key, &seq, &type)
  Actual: true
Expected: false
type=0x2 必须拒绝

/tmp/m11/util_only_test.cpp:992: Failure
Expected equality of these values:
  "keep"
  user_key.ToString()
    Which is: "userkey"

/tmp/m11/util_only_test.cpp:993: Failure
Expected equality of these values:
  12345u
    Which is: 12345
  seq
    Which is: 562949953421354
```

矛盾点（同一份 util_test.cpp 内的两条互斥断言）：

```cpp
// A) InternalKey.BuildParseRoundTrip（要求接受）：seq = 2^56-1 → trailer b7 = 0xFF
const SequenceNumber seqs[] = {0, 1, 42, kMaxSequenceNumber};
ASSERT_TRUE(ParseInternalKey(internal_key, &parsed_user_key, &parsed_seq, &parsed_type));

// B) InternalKey.ParseMalformed（要求拒绝）：写到 raw[size-1] = b7 ∈ {…,0x80,0xFE,0xFF}
raw[raw.size() - 1] = static_cast<char>(t);
EXPECT_FALSE(ParseInternalKey(raw, &user_key, &seq, &type)) << "type=0x" << std::hex << t << " 必须拒绝";
```

实测被写坏的是 sequence（`seq = 562949953421354 = 42 + 0x02<<48`），不是 type。
修复：改为 protocol §6 规定的 type 字节下标 `raw[size - 8]`（与原文注释一致），仅此一处。

## M1.1 —— 工程骨架与 util 层

执行时间：2026-09-29T18:35:23+08:00

命令 1：liblsm 干净重建（-Wall -Wextra，目标 lsm）
$ rm -rf build && cmake -S . -B build && cmake --build build --target lsm -j8
-- Generating done
-- Build files have been written to: /home/tengyujie/lsm-kv/build
[ 50%] Building CXX object CMakeFiles/lsm.dir/src/util/crc32c.cpp.o
[ 50%] Building CXX object CMakeFiles/lsm.dir/src/util/status.cpp.o
[ 50%] Building CXX object CMakeFiles/lsm.dir/src/util/coding.cpp.o
[ 66%] Building CXX object CMakeFiles/lsm.dir/src/util/env_posix.cpp.o
[ 83%] Building CXX object CMakeFiles/lsm.dir/src/util/arena.cpp.o
[100%] Linking CXX static library liblsm.a
[100%] Built target lsm
warning 计数：0

命令 2：从冻结测试抽取 util 子集（断言一字不改）后编译运行
$ sed -n "1,198p" tests/test_harness.h > /tmp/m11/shim_harness.h
$ sed -n "310,362p" tests/test_harness.h >> /tmp/m11/shim_harness.h
$ sed -i /db.h/d 等三行（去掉指向 M1.2 头文件的 include）
$ awk 截到 TEST(InternalKey, LookupKeySemantics) 之前 > /tmp/m11/util_only_test.cpp
$ g++ -std=c++17 -Wall -Wextra -I src -I /tmp/m11 -I /usr/local/include /tmp/m11/util_only_test.cpp build/liblsm.a /usr/local/lib/libgtest.a /usr/local/lib/libgtest_main.a -lpthread -o /tmp/m11/util_only_tests
子集用例数：18
编译 warning 计数：0
$ /tmp/m11/util_only_tests
[       OK ] InternalKey.BuildParseRoundTrip (1 ms)
[ RUN      ] InternalKey.ParseMalformed
[       OK ] InternalKey.ParseMalformed (0 ms)
[ RUN      ] InternalKey.CompareOrder
[       OK ] InternalKey.CompareOrder (25 ms)
[----------] 3 tests from InternalKey (28 ms total)

[----------] Global test environment tear-down
[==========] 18 tests from 7 test suites ran. (67 ms total)
[  PASSED  ] 18 tests.

命令 3：修复测试下标后 InternalKey.ParseMalformed 单例复跑
$ /tmp/m11/util_only_tests --gtest_filter=InternalKey.ParseMalformed
[       OK ] InternalKey.ParseMalformed (0 ms)
[----------] 1 test from InternalKey (0 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (0 ms total)
[  PASSED  ] 1 test.

命令 4：M1.1 交付物（静态库成员）
$ ar t build/liblsm.a
status.cpp.o
coding.cpp.o
crc32c.cpp.o
arena.cpp.o
env_posix.cpp.o

## M1.2 —— 跳表 + MemTable + 内部 key 迭代器 + DB 内存实现

执行时间：2026-09-29T18:43:08+08:00

命令 1：bash scripts/lsm_build.sh（干净重建 + 0 warning 断言 + 全量用例）
$ bash scripts/lsm_build.sh
[ RUN      ] Stress.OneMillionKeysReconcile
[   INFO   ] Stress.OneMillionKeysReconcile: n=1000000 write_ms=16105 reconcile_ms=1295 ApproximateMemoryUsage=155998029 bytes
[       OK ] Stress.OneMillionKeysReconcile (19170 ms)
[ RUN      ] Stress.DeleteThirtyPercentReconcile
[   INFO   ] Stress.DeleteThirtyPercentReconcile: entries=390000 visible=210000 deleted=90000 elapsed_ms=5463 memtable_bytes=61318518
[       OK ] Stress.DeleteThirtyPercentReconcile (6049 ms)
[ RUN      ] Stress.SameKey100kTimes
[   INFO   ] Stress.SameKey100kTimes: versions=100000 elapsed_ms=250 memtable_bytes=14389159
[       OK ] Stress.SameKey100kTimes (251 ms)
[ RUN      ] Stress.AlignmentUnderSanitizers
[   INFO   ] Stress.AlignmentUnderSanitizers: build=plain（ASan/UBSan 结论见 build-asan 门禁）
[   INFO   ] Stress.AlignmentUnderSanitizers: allocations=2000 alignof(max_align_t)=16
[       OK ] Stress.AlignmentUnderSanitizers (26 ms)
[----------] 4 tests from Stress (25498 ms total)

[----------] Global test environment tear-down
[==========] 45 tests from 12 test suites ran. (26940 ms total)
[  PASSED  ] 45 tests.
[CHECK] 用例计数：
[==========] 45 tests from 12 test suites ran. (26940 ms total)
[  PASSED  ] 45 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）

命令 2：B 组（压力）实测数字
[   INFO   ] Stress.OneMillionKeysReconcile: n=1000000 write_ms=16105 reconcile_ms=1295 ApproximateMemoryUsage=155998029 bytes
[   INFO   ] Stress.DeleteThirtyPercentReconcile: entries=390000 visible=210000 deleted=90000 elapsed_ms=5463 memtable_bytes=61318518
[   INFO   ] Stress.SameKey100kTimes: versions=100000 elapsed_ms=250 memtable_bytes=14389159
[   INFO   ] Stress.AlignmentUnderSanitizers: build=plain（ASan/UBSan 结论见 build-asan 门禁）
[   INFO   ] Stress.AlignmentUnderSanitizers: allocations=2000 alignof(max_align_t)=16
