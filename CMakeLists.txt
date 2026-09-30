# lsm-kv 构建入口
#
# 阶段：#2 的 TDD 骨架已提交（RED 原始输出见 docs/m1-tdd-red.log）；本文件是 M1.1 的改造：
#   - lsm 从 INTERFACE 改成 STATIC，只列 M1.1 已实现的 util 层源文件；
#   - lsm_tests 的源列表自 #2 起不变（两个测试文件），因为 tests/util_test.cpp 与
#     tests/test_harness.h 本身就依赖 MemTable/Skiplist/MemTableKeyComparator
#     （用例 InternalKey.LookupKeySemantics、MemTableKeyComparator.TotalOrder），
#     所以 M1.1 阶段 lsm_tests 仍然链接不上 —— 这是预期的：
#     M1.1 的门禁是 liblsm 干净构建 0 warning（docs/m1-design.md §12 修订记录），
#     M1.2 补上 skiplist/memtable/db 后 lsm_tests 才首次全绿。
#
# 三构建目录互不共享缓存（docs/roadmap.md §3.5）：
#   build        Release
#   build-asan   -DENABLE_ASAN=ON
#   build-tsan   -DENABLE_TSAN=ON   （运行需 setarch $(uname -m) -R）

cmake_minimum_required(VERSION 3.16)
project(lsm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -g -O1)
  add_link_options(-fsanitize=address)
endif()

find_package(Threads REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 被测库（docs/m1-design.md §3 文件清单；每个文件显式列出，不用 GLOB）
add_library(lsm STATIC
  # M3.1 格式层（docs/m3-design.md §11.1：format / block / table_* 同属纯格式层）
  src/sstable/format.cpp
  src/sstable/block.cpp
  src/sstable/table_builder.cpp
  src/sstable/table.cpp
  src/util/status.cpp
  src/util/coding.cpp
  src/util/crc32c.cpp
  src/util/arena.cpp
  src/util/env_posix.cpp
  # M1.2：跳表是 header-only（src/skiplist.h），此处只列 .cpp
  src/memtable.cpp
  src/db.cpp
  # M2.1：WAL writer/reader（docs/m2-design.md §4）
  src/wal.cpp
  # M2.2：文件命名 + 持久化实现与恢复（docs/m2-design.md §5/§6/§7）
  src/filename.cpp
  src/db_impl.cpp
  # M3.2：版本/缓存 + 归并 + 用户视图迭代器（docs/m3-design.md §11.2 的「新增」列）
  src/version_edit.cpp
  src/version_set.cpp
  # M4.2：选层/选文件/闭包/丢弃判据（docs/m4-design.md §5.4/§5.5）
  src/compaction.cpp
  src/merging_iterator.cpp
  src/db_iter.cpp
)
target_include_directories(lsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm PUBLIC Threads::Threads)

# M3.1 的依赖纪律机制（docs/m3-design.md §5.1/§11.1）：纯格式层的独立静态库，只链 util。
# 越权依赖（db_impl/version_*/merging_iterator/db_iter/wal）会在这个目标上**链接失败**，
# 不靠人盯。`nm -C build/liblsm_sstable.a | grep -c 'db_impl\|wal\|version'` 期望 0。
add_library(lsm_sstable STATIC
  src/sstable/format.cpp
  src/sstable/block.cpp
  src/sstable/table_builder.cpp
  src/sstable/table.cpp
  src/util/status.cpp
  src/util/coding.cpp
  src/util/crc32c.cpp
)
target_include_directories(lsm_sstable PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm_sstable PUBLIC Threads::Threads)

# M4.1 的依赖纪律机制（docs/m4-design.md §1.4）：version_edit/version_set 只能依赖 util，
# 不得反向依赖 db_impl/wal/memtable。判据：nm -C build/liblsm_version.a | grep -cE 'db_impl|wal|memtable' ⇒ 0。
add_library(lsm_version STATIC
  src/version_edit.cpp
  src/version_set.cpp
  src/filename.cpp
  src/util/status.cpp
  src/util/coding.cpp
  src/util/crc32c.cpp
)
target_include_directories(lsm_version PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm_version PUBLIC Threads::Threads)

# GTest 是测试专用依赖：用 /usr/local 的既有安装，不做 FetchContent、不 vendor。
# 找不到必须硬失败：静默跳过测试目标等于把「没跑」伪装成「通过」（docs/roadmap.md §3.4）。
find_path(GTEST_INCLUDE_DIR gtest/gtest.h PATHS /usr/local/include)
find_library(GTEST_LIBRARY NAMES gtest PATHS /usr/local/lib)
find_library(GTEST_MAIN_LIBRARY NAMES gtest_main PATHS /usr/local/lib)

if(NOT GTEST_INCLUDE_DIR OR NOT GTEST_LIBRARY OR NOT GTEST_MAIN_LIBRARY)
  message(FATAL_ERROR
    "GTest not found under /usr/local "
    "(GTEST_INCLUDE_DIR='${GTEST_INCLUDE_DIR}' GTEST_LIBRARY='${GTEST_LIBRARY}' "
    "GTEST_MAIN_LIBRARY='${GTEST_MAIN_LIBRARY}').")
endif()

enable_testing()

add_executable(lsm_tests
  tests/sstable_format_test.cpp      # M3.1 格式层（Block/Footer）
  tests/sstable_table_test.cpp       # M3.1 Table/TableBuilder（A08 后 3 行、A09~A19）
  tests/util_test.cpp
  tests/memtable_test.cpp
  # M2：WAL 层用例（A01~A10）+ 故障注入 Env + 恢复层用例（A11~A19）+ 内存文件系统（崩溃语义 seam）
  tests/wal_test.cpp
  tests/faulty_env.cpp
  tests/recovery_test.cpp
  tests/memenv.cpp
  # M2.2e：掉电语义（MemEnv 崩溃注入）
  tests/crash_test.cpp
  # M3.2：flush / 读路径（A20~A30）与迭代器（A31~A34）
  tests/flush_test.cpp
  tests/iterator_test.cpp
  # M3.3：恢复/元数据/WAL 回收（A35~A54）+ 崩溃脚本用的 flush 探针
  tests/recovery_m3_test.cpp
  tests/crash_flush_test.cpp
  # M4.1：VersionEdit / MANIFEST / CURRENT（A01~A10/A39/A40/A42）
  tests/version_test.cpp
  # M4.2：选层/选文件/闭包/丢弃判据（A11~A16/A23/A26 的确定性部分）
  tests/compaction_test.cpp
  # M4.2：compaction 端到端 / 读路径层级化 / 延迟删除 / 独立 CRC
  tests/compaction_db_test.cpp
)
target_include_directories(lsm_tests PRIVATE
  ${GTEST_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(lsm_tests PRIVATE
  lsm
  ${GTEST_LIBRARY}
  ${GTEST_MAIN_LIBRARY}
  Threads::Threads
)

add_test(NAME lsm_unit COMMAND lsm_tests)

# M2.2：崩溃对账工具（docs/m2-design.md §8）—— 门禁脚本 scripts/lsm_crash_test.sh 调用
add_executable(lsm_crash_writer scripts/crash_writer.cpp)
target_link_libraries(lsm_crash_writer PRIVATE lsm)

add_executable(lsm_crash_recover scripts/crash_recover.cpp)
target_link_libraries(lsm_crash_recover PRIVATE lsm)

# M2.2c：损坏注入的端到端判据（B03 逐字节截断扫描 / B04 中间损坏）
add_executable(lsm_damage_test scripts/lsm_damage_test.cpp)
target_link_libraries(lsm_damage_test PRIVATE lsm)

# B07：单次提交延迟微基准（docs/m2-design.md §9.2/§9.3；M5 的性能分母）
add_executable(fsbench_commit_latency scripts/fsbench_commit_latency.cpp)

# M3.3：四个门禁脚本（B01/B03/B04/B05）共用的探针
add_executable(lsm_m3_probe scripts/m3_probe.cpp)
target_link_libraries(lsm_m3_probe PRIVATE lsm)

# M4.3：三个放大的实测探针（B 组驱动；真实磁盘）
add_executable(lsm_ampl_probe scripts/lsm_ampl_probe.cpp)
target_link_libraries(lsm_ampl_probe PRIVATE lsm)

# M4.3：B01/B02 的进程级崩溃驱动
add_executable(lsm_compaction_crash scripts/lsm_compaction_crash.cpp)
target_link_libraries(lsm_compaction_crash PRIVATE lsm)
target_link_libraries(lsm_ampl_probe PRIVATE lsm)
