// src/util/arena.cpp
#include "util/arena.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace lsm {
namespace {
constexpr size_t kMaxAlign = alignof(std::max_align_t);
}  // namespace

Arena::Arena() = default;

Arena::~Arena() {
  for (char* block : blocks_) delete[] block;
}

char* Arena::Allocate(size_t bytes) {
  // 空请求也交出可用指针，避免调用方对 nullptr 做额外分支；统一走对齐路径。
  return AllocateAligned(bytes == 0 ? 1 : bytes, kMaxAlign);
}

char* Arena::AllocateAligned(size_t bytes, size_t align) {
  if (bytes == 0) bytes = 1;
  bytes_allocated_ += bytes;
  // 结果必须同时满足请求对齐与 alignof(std::max_align_t)（I9；Arena.AlignmentAndUsage 对两者都断言）。
  if (align < kMaxAlign) align = kMaxAlign;
  // 大块**恒**单独分配：不吃当前小块的剩余空间（design §6 的原话）。
  // 若先看剩余空间再看大小，1 字节小分配之后的 1025 字节请求会落进同一个 4 KiB 块，
  // 与 design §6 的口径和冻结测试的断言（增量 ≥ 请求字节数）都不符。
  if (bytes > kBlockSize / 4) return AllocateFallbackAligned(bytes, align);
  // align 为 2 的幂（头文件前置条件）：用位与代替取模。
  const size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  const size_t slop = (current_mod == 0) ? 0 : (align - current_mod);
  const size_t needed = bytes + slop;
  if (needed <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
    return result;
  }
  return AllocateFallbackAligned(bytes, align);
}

char* Arena::AllocateFallbackAligned(size_t bytes, size_t align) {
  // 大块（> kBlockSize/4）单独申请「恰好够 + 对齐余量」的块，不吃小块的剩余空间（design §6）。
  const size_t block = (bytes > kBlockSize / 4) ? (bytes + align) : (kBlockSize + align);
  char* raw = AllocateNewBlock(block);
  const size_t mod = reinterpret_cast<uintptr_t>(raw) & (align - 1);
  const size_t slop = (mod == 0) ? 0 : (align - mod);
  char* result = raw + slop;
  alloc_ptr_ = result + bytes;
  alloc_bytes_remaining_ = block - slop - bytes;
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  // operator new[] 保证 alignof(std::max_align_t) 对齐。
  // 分配失败按 design §6 的显式决策 fail-fast：打印可定位信息后 abort。
  // 用 nothrow 变体是为了不让 std::bad_alloc 逃逸成无信息的 std::terminate
  //（#4 评审建议 4：原文的决策在实现里没兑现）。
  char* result = new (std::nothrow) char[block_bytes];
  if (result == nullptr) {
    std::fprintf(stderr, "lsm::Arena: out of memory allocating %zu bytes (design §6: fail-fast)\n",
                 block_bytes);
    std::abort();
  }
  blocks_.push_back(result);
  memory_usage_ += block_bytes + sizeof(char*);
  return result;
}

void Arena::Reset() {
  for (char* block : blocks_) delete[] block;
  blocks_.clear();
  blocks_.shrink_to_fit();
  alloc_ptr_ = nullptr;
  alloc_bytes_remaining_ = 0;
  memory_usage_ = 0;
  bytes_allocated_ = 0;
}

}  // namespace lsm
