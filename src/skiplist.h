// src/skiplist.h —— 跳表（docs/m1-design.md §7）
//
// 设计要点（都是"为什么"）：
//  - 固定 next_[kMaxHeight] 数组而非柔性数组：零 UB（design §7.3 的取舍，MemTable 上限 4 MiB
//    时指针开销可接受），并让「maxHeight 与数组长度一致」这条验收判据可以直接断言。
//  - next_ 用 std::atomic<Node*>，读 acquire / 写 release：发布语义支撑 L1「单写者 + 读不加锁」
//    （I3：节点一经发布，其 key 与层指针不再修改）。
//  - 允许相等 key 并存（多重集语义，design §7.4）；MemTable 层的 internal key 含 sequence，不会重复。
//  - 随机源是自带的 Park–Miller PRNG（固定种子可复现），不用 <random>：分布实现跨平台不一致
//    会破坏「固定种子可复现」的验收口径（design §7.1）。
//
// 前置条件（调用方保证）：Insert 的 key 字节生命周期 ≥ 本跳表（生产路径是 Arena）。
#ifndef LSM_SKIPLIST_H_
#define LSM_SKIPLIST_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "common.h"
#include "util/arena.h"

namespace lsm {

class Skiplist {
 public:
  static constexpr int kMaxHeight = 12;        // 支持 4^12 ≈ 1.6e7 条不退化
  static constexpr uint32_t kBranching = 4;    // p = 1/4

  struct Stats {
    size_t node_count = 0;
    size_t level_sum = 0;
    size_t level_histogram[kMaxHeight] = {0};  // level_histogram[i] = 恰好高度 i+1 的节点数
  };

 private:
  struct Node {
    Slice key_;
    int height_ = 1;
    std::atomic<Node*> next_[kMaxHeight];
  };

 public:
  Skiplist(Arena* arena, const Comparator* cmp, uint32_t rng_seed = 0x9E3779B9u)
      : arena_(arena), compare_(cmp), rnd_(rng_seed) {
    head_ = NewNode(Slice(), kMaxHeight);
  }

  ~Skiplist() = default;   // 节点由 Arena 拥有，不逐个释放（design §7.2）
  Skiplist(const Skiplist&) = delete;
  Skiplist& operator=(const Skiplist&) = delete;

  void Insert(const Slice& key);
  bool Contains(const Slice& key) const;

  // O(n) 诊断接口：供层高分布验收（design §11）与内存口径使用，不参与写路径。
  Stats GetStats() const {
    Stats st;
    for (const Node* x = head_->next_[0].load(std::memory_order_acquire); x != nullptr;
         x = x->next_[0].load(std::memory_order_acquire)) {
      ++st.node_count;
      st.level_sum += static_cast<size_t>(x->height_);
      st.level_histogram[x->height_ - 1]++;
    }
    return st;
  }

  int MaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  class Iterator {
   public:
    explicit Iterator(const Skiplist* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    Slice key() const { return node_->key_; }

    void SeekToFirst() { node_ = list_->head_->next_[0].load(std::memory_order_acquire); }

    void SeekToLast() {
      const Node* x = list_->head_;
      for (int level = list_->MaxHeight() - 1; level >= 0;) {
        const Node* next = x->next_[level].load(std::memory_order_acquire);
        if (next != nullptr) {
          x = next;
        } else if (level == 0) {
          break;
        } else {
          --level;
        }
      }
      node_ = (x == list_->head_) ? nullptr : x;
    }

    // 定位到第一个 >= target 的节点；无 → !Valid。
    void Seek(const Slice& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void Next() {
      if (node_ == nullptr) return;
      node_ = node_->next_[0].load(std::memory_order_acquire);
    }

    // 立刻前驱（列表序意义上的前一个节点），不是「严格小于的最后一个」：
    // 相等 key 的重复段必须能被逐个走完，否则「正反遍历互为逆序 / 条数相等」的验收不成立。
    void Prev();

   private:
    const Skiplist* list_;
    const Node* node_;
  };

  Iterator* NewIterator() const { return new Iterator(this); }

 private:
  Node* NewNode(const Slice& key, int height);
  int RandomHeight();
  int Compare(const Slice& a, const Slice& b) const { return compare_->Compare(a, b); }
  Node* FindGreaterOrEqual(const Slice& key, Node** prev) const;
  const Node* FindLastLessThan(const Slice& key) const;   // 返回 head_ 表示没有

  // Park–Miller minimal standard（design §7.1）
  class Random {
   public:
    explicit Random(uint32_t seed) : seed_(seed & 0x7fffffffu) {
      if (seed_ == 0 || seed_ == 2147483647u) seed_ = 1;
    }
    uint32_t Next() {
      static constexpr uint64_t kM = 2147483647ull;
      static constexpr uint64_t kA = 16807ull;
      const uint64_t product = static_cast<uint64_t>(seed_) * kA;
      seed_ = static_cast<uint32_t>((product >> 31) + (product & kM));
      if (seed_ > kM) seed_ -= static_cast<uint32_t>(kM);
      return seed_;
    }

   private:
    uint32_t seed_;
  };

  Arena* const arena_;
  const Comparator* const compare_;
  Random rnd_;
  Node* head_ = nullptr;
  std::atomic<int> max_height_{1};
};

inline void Skiplist::Insert(const Slice& key) {
  Node* prev[kMaxHeight];
  for (int i = 0; i < kMaxHeight; ++i) prev[i] = head_;
  FindGreaterOrEqual(key, prev);

  const int height = RandomHeight();
  const int cur_max = max_height_.load(std::memory_order_relaxed);
  if (height > cur_max) {
    for (int i = cur_max; i < height; ++i) prev[i] = head_;
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* const x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // 先写新节点自己的 next_，再 release 发布到前驱：读者 acquire 之后能完整看到新节点（I3/L1）
    x->next_[i].store(prev[i]->next_[i].load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    prev[i]->next_[i].store(x, std::memory_order_release);
  }
}

inline bool Skiplist::Contains(const Slice& key) const {
  const Node* const x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Compare(x->key_, key) == 0;
}

inline void Skiplist::Iterator::Prev() {
  if (node_ == nullptr) return;
  const Node* const target = node_;
  const Slice key = target->key_;
  const Node* const first_ge = list_->FindGreaterOrEqual(key, nullptr);
  if (first_ge == target) {
    const Node* const less = list_->FindLastLessThan(key);
    node_ = (less == list_->head_) ? nullptr : less;
    return;
  }
  // target 落在相等 key 的重复段内部：从该段第一个节点顺序走到 target 的前驱
  const Node* x = first_ge;
  while (x != nullptr && x->next_[0].load(std::memory_order_acquire) != target) {
    x = x->next_[0].load(std::memory_order_acquire);
  }
  node_ = x;
}

inline Skiplist::Node* Skiplist::NewNode(const Slice& key, int height) {
  char* const mem = arena_->AllocateAligned(sizeof(Node), alignof(Node));
  Node* const n = new (mem) Node();
  n->key_ = key;
  n->height_ = height;
  for (int i = 0; i < kMaxHeight; ++i) {
    n->next_[i].store(nullptr, std::memory_order_relaxed);
  }
  return n;
}

inline int Skiplist::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && rnd_.Next() % kBranching == 0) ++height;
  return height;
}

inline Skiplist::Node* Skiplist::FindGreaterOrEqual(const Slice& key, Node** prev) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* const next = x->next_[level].load(std::memory_order_acquire);
    if (next != nullptr && Compare(next->key_, key) < 0) {
      x = next;
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;
    }
  }
}

inline const Skiplist::Node* Skiplist::FindLastLessThan(const Slice& key) const {
  const Node* x = head_;
  for (int level = max_height_.load(std::memory_order_relaxed) - 1; level >= 0;) {
    const Node* const next = x->next_[level].load(std::memory_order_acquire);
    if (next != nullptr && Compare(next->key_, key) < 0) {
      x = next;
    } else if (level == 0) {
      break;
    } else {
      --level;
    }
  }
  return x;
}

}  // namespace lsm

#endif  // LSM_SKIPLIST_H_
