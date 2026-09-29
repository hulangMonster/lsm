// src/db_iter.h —— 用户视图迭代器（docs/m3-design.md §7.3）
//
// 只做：(a) sequence 可见性 seq <= snapshot、(b) 每个 user key 只出最新可见版本、
// (c) 跳过 tombstone、(d) internal→user key、(e) 三态状态机 kBeforeFirst/kValid/kPastEnd。
// child 是一个 internal key 序的迭代器（M3 传 MergingIterator；M1 的内存模式传单 child 的
// MergingIterator）；child 的所有权归本类。
#ifndef LSM_DB_ITER_H_
#define LSM_DB_ITER_H_

#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace lsm {

class DBIter : public Iterator {
 public:
  // keep_alive：本迭代器必须保活的 MemTable / Version 等 shared_ptr（L19/L21）。
  // 每个 Table child 自己持 Table 的 shared_ptr；这里额外兜住 MemTable 与 Version。
  DBIter(const InternalKeyComparator* icmp, Iterator* internal, SequenceNumber snapshot,
         std::vector<std::shared_ptr<const void>> keep_alive = {});
  ~DBIter() override;
  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  bool Valid() const override { return state_ == kValid; }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const Slice& target) override;
  void Next() override;
  void Prev() override;

  Slice key() const override { return Slice(current_user_key_); }
  Slice value() const override { return internal_->value(); }
  Status status() const override { return internal_->status(); }

 private:
  enum State { kBeforeFirst, kValid, kPastEnd };

  void RewindToRunStart();
  bool MoveToPreviousUserKeyOf(const std::string& user_key);
  void SkipCurrentRunForwardWithKey(const Slice& user_key);
  void ScanForwardToVisible();
  void ScanBackwardToVisible();
  void SetValid(const Slice& user_key);

  const InternalKeyComparator* const icmp_;
  Iterator* const internal_;
  const SequenceNumber snapshot_;
  std::vector<std::shared_ptr<const void>> keep_alive_;
  State state_ = kBeforeFirst;
  std::string current_user_key_;
};

}  // namespace lsm

#endif  // LSM_DB_ITER_H_
