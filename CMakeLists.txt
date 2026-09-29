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
  src/util/status.cpp
  src/util/coding.cpp
  src/util/crc32c.cpp
  src/util/arena.cpp
  src/util/env_posix.cpp
  # M1.2：跳表是 header-only（src/skiplist.h），此处只列 .cpp
  src/memtable.cpp
  src/db.cpp
)
target_include_directories(lsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsm PUBLIC Threads::Threads)

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
  tests/util_test.cpp
  tests/memtable_test.cpp
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
