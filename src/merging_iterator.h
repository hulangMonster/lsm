// src/merging_iterator.h —— N 路 internal-key 归并（docs/m3-design.md §7.3）
//
// 职责边界：**只**做归并。不做可见性判断、不跳 tombstone、不转 user key。
// child 的所有权归本类（析构 delete[] / delete）；要求所有 child 都支持双向迭代
// （M3 的 child 是 MemTable 内部迭代器与 Table::TableIterator，二者都实现 Iterator 全接口）。
#ifndef LSM_MERGING_ITERATOR_H_
#define LSM_MERGING_ITERATOR_H_

#include "common.h"

namespace lsm {

class MergingIterator : public Iterator {
 public:
  // children：所有权归本类（析构时 delete children_[i] 与 delete[] children_）。
  // icmp 不拥有；n >= 0。
  MergingIterator(const InternalKeyComparator* icmp, Iterator** children, int n);
  ~MergingIterator() override;
  MergingIterator(const MergingIterator&) = delete;
  MergingIterator& operator=(const MergingIterator&) = delete;

  bool Valid() const override { return current_ != nullptr; }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const Slice& target) override;
  void Next() override;
  void Prev() override;

  Slice key() const override { return current_ != nullptr ? current_->key() : Slice(); }
  Slice value() const override { return current_ != nullptr ? current_->value() : Slice(); }
  // 各 child status 的**第一个非 OK**（确定性顺序扫描）。
  Status status() const override;

 private:
  enum Direction { kForward, kReverse };
  void FindSmallest();
  void FindLargest();

  const InternalKeyComparator* const icmp_;
  Iterator** const children_;
  const int n_;
  Iterator* current_ = nullptr;
  Direction direction_ = kForward;
};

// 一个永远 Invalid、只携带 Status 的 child（TableCache 打开失败时用）。
// 所有权归调用方（与 MemTable::NewIterator 同纪律）。
Iterator* NewStatusIterator(const Status& status);

}  // namespace lsm

#endif  // LSM_MERGING_ITERATOR_H_
