// src/util/arena.h —— 块式 Arena（docs/m1-design.md §6）
//
// 为什么 M1 就引入（design §1.3 决策 8）：跳表节点与条目字节的生命周期与 MemTable 绑定，
// 一次性回收即可避免逐节点 free 与碎片，也让「已发布节点不可变」（I3）有明确的所有权边界。
//
// 对齐契约（I9）：Allocate 与 AllocateAligned **都**返回 alignof(std::max_align_t) 对齐的内存
// （冻结测试 Arena.AlignmentAndUsage 对每次 Allocate 都断言 % alignof(max_align_t) == 0）。
//
// 线程安全：**不是**线程安全的（L1/L2：单写者；M1 不做并发分配）。
#ifndef LSM_UTIL_ARENA_H_
#define LSM_UTIL_ARENA_H_

#include <cstddef>
#include <vector>

namespace lsm {

class Arena {
 public:
  static constexpr size_t kBlockSize = 4096;

  Arena();
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // 返回 alignof(std::max_align_t) 对齐的内存。bytes == 0 视为请求 1 字节（返回可用指针）。
  char* Allocate(size_t bytes);

  // align 必须是 2 的幂（默认 alignof(std::max_align_t)）；返回满足该对齐的内存。
  char* AllocateAligned(size_t bytes, size_t align = alignof(std::max_align_t));

  // 已申请块字节总和（含块内未用尾部与每块的 vector 指针开销）。Reset 前单调不减。
  size_t MemoryUsage() const { return memory_usage_; }

  // 丢弃全部块（design §6）；调用后 MemoryUsage() == 0，可继续分配。Reset 前交出的指针全部失效。
  void Reset();

 private:
  // 当前块剩余空间不够（或对齐不满足）时走这里：新开一块并返回其中对齐后的位置。
  char* AllocateFallbackAligned(size_t bytes, size_t align);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  size_t memory_usage_ = 0;
};

}  // namespace lsm

#endif  // LSM_UTIL_ARENA_H_
