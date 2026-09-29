// src/memtable.h —— MemTable（docs/m1-design.md §8）+ 条目编码比较器（protocol §7）
//
// 不变量（docs/m1-prerequisites.md §1）：
//   I1 internal key 唯一（含 sequence）；I2 同 user key 按 sequence 降序；
//   I3 已发布节点不可变；I4 tombstone 只屏蔽更小 sequence；I6 统计单调不减 + 冻结后写入返回 kFrozen。
// 依赖纪律：本文件只依赖 common.h / util/arena.h / skiplist.h，不得 include db.h（docs/roadmap.md §0）。
#ifndef LSM_MEMTABLE_H_
#define LSM_MEMTABLE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "common.h"
#include "skiplist.h"
#include "util/arena.h"

namespace lsm {

// 跳表里存的是**整条条目**（protocol §7）：varint32(len) | internal_key | varint32(vlen) | value。
// 该编码不是字节序保持的（长度前缀破坏排序），所以跳表必须注入本比较器：
//   先解码两侧 internal_key 按 protocol §6.1 比较；完全相等时再按整条条目字节序比较，
//   以保证严格全序（同 internal key 不同 value 的两个条目不得判等，否则跳表会丢条目）。
class MemTableKeyComparator : public Comparator {
 public:
  explicit MemTableKeyComparator(const InternalKeyComparator* internal_comparator)
      : internal_comparator_(internal_comparator) {}
  int Compare(const Slice& a, const Slice& b) const override;
  const char* Name() const override { return "leveldb.MemTableKeyComparator"; }

 private:
  const InternalKeyComparator* internal_comparator_;
};

class MemTable {
 public:
  enum class GetResult { kFound, kDeleted, kNotFound };

  // internal_comparator 不拥有：生命周期必须覆盖本 MemTable（L5 析构顺序）。
  MemTable(const InternalKeyComparator& internal_comparator, size_t write_buffer_size);
  ~MemTable() = default;
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // 失败一律带上下文返回 Status（I8）：
  //   kInvalidArgument —— 空 key / key 超过 kMaxUserKeySize / seq 越界 / type 非法（且不写入、统计不变）
  //   kFrozen          —— 已冻结或本次触顶（**写前判**，不静默丢弃，design §8.1）
  Status Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // lookup_key = user_key || trailer（protocol §6.2）；读快照由调用方决定。
  GetResult Get(const Slice& lookup_key, std::string* value) const;

  // 内部 key 序（多版本可见、含 tombstone）；调用方负责 delete（design §4.4）。
  Iterator* NewIterator() const;

  bool Freeze();                       // 幂等：true = 本次完成冻结
  bool IsFrozen() const { return frozen_.load(std::memory_order_acquire); }
  // 口径（design §8.3 修订）：Arena **累计交出的载荷字节** + sizeof(MemTable)。
  // 不用整块字节：块粒度在 write_buffer_size 与块大小同量级时，光头部节点就吃掉一个 4 KiB 块，
  // 会让小缓冲配置连一次写入都接受不了（与指令「Arena 已用字节 + 条目数」的口径一致）。
  size_t ApproximateMemoryUsage() const {
    return arena_.BytesAllocated() + sizeof(MemTable);
  }
  size_t NumEntries() const { return entry_count_; }   // 含 tombstone；单调不减
  const InternalKeyComparator& internal_comparator() const { return *internal_comparator_; }

 private:
  const InternalKeyComparator* internal_comparator_;
  const size_t write_buffer_size_;
  MemTableKeyComparator key_comparator_;
  Arena arena_;
  Skiplist skiplist_;
  size_t entry_count_ = 0;
  std::atomic<bool> frozen_{false};
};

}  // namespace lsm

#endif  // LSM_MEMTABLE_H_
