# lsm-kv 构建入口（M1 / #2 阶段：TDD 测试骨架）
#
# 为什么 #2 阶段 lsm 是 INTERFACE 库而不是 STATIC（docs/m1-design.md §13）：
#   - 若在此处列出尚不存在的实现源文件，配置期就会以 "Cannot find source file" 失败，
#     那只证明「CMake 写错了」，证明不了「测试已表达全部契约、实现为零」；
#   - INTERFACE 目标让配置成功、只编译测试，构建结果 = 测试 include 不到 src/ 下的头文件
#     （编译期 RED，原始输出见 docs/m1-tdd-red.log，保留不删）。
#   - 设计 §13 原文预期的是「链接期 undefined reference to lsm::...」；实测为更早的编译期
#     失败（common.h 等根本不存在），RED 更早暴露、结论一致：#2 阶段实现为零。
# M1.1 起把 lsm 改成 STATIC 并按 design §3 的文件清单逐个补源文件，RED 随之收敛。

cmake_minimum_required(VERSION 3.16)
project(lsm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# 内存/并发正确性门禁（M1.3 收口）。三构建目录互不共享缓存（docs/roadmap.md §3.5）。
# TSan 需要关闭 ASLR：setarch $(uname -m) -R ./build-tsan/bin/lsm_tests
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

# 被测库：M1 的全部实现落在 src/ 下（design §3）。此处故意不列任何源文件（见文件头说明）。
add_library(lsm INTERFACE)
target_include_directories(lsm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GTest 是测试专用依赖：用 /usr/local 的既有安装，不做 FetchContent、不 vendor（design §1.3 决策 9）。
# 找不到必须硬失败：静默跳过测试目标等于把「没跑」伪装成「通过」（docs/roadmap.md §3.4「未跑不算过」）。
find_path(GTEST_INCLUDE_DIR gtest/gtest.h PATHS /usr/local/include)
find_library(GTEST_LIBRARY NAMES gtest PATHS /usr/local/lib)
find_library(GTEST_MAIN_LIBRARY NAMES gtest_main PATHS /usr/local/lib)

if(NOT GTEST_INCLUDE_DIR OR NOT GTEST_LIBRARY OR NOT GTEST_MAIN_LIBRARY)
  message(FATAL_ERROR
    "GTest not found under /usr/local "
    "(GTEST_INCLUDE_DIR='${GTEST_INCLUDE_DIR}' GTEST_LIBRARY='${GTEST_LIBRARY}' "
    "GTEST_MAIN_LIBRARY='${GTEST_MAIN_LIBRARY}'). "
    "安装后重试：sudo apt install libgtest-dev && 在 /usr/local 安装 gtest/gtest_main 静态库。")
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
