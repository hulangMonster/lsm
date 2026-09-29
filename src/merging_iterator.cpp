// src/merging_iterator.cpp —— M3.2 的 N 路归并（LevelDB 同法：线性扫最小/最大，D7/E7）。
#include "merging_iterator.h"

#include <cassert>

namespace lsm {
namespace {

class StatusIterator : public Iterator {
 public:
  explicit StatusIterator(const Status& s) : status_(s) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}
  void Next() override {}
  void Prev() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  const Status status_;
};

}  // namespace

Iterator* NewStatusIterator(const Status& status) { return new StatusIterator(status); }

MergingIterator::MergingIterator(const InternalKeyComparator* icmp, Iterator** children, int n)
    : icmp_(icmp), children_(children), n_(n) {
  assert(icmp != nullptr && children != nullptr && n >= 0);
}

MergingIterator::~MergingIterator() {
  for (int i = 0; i < n_; ++i) delete children_[i];
  delete[] children_;
}

void MergingIterator::FindSmallest() {
  Iterator* smallest = nullptr;
  for (int i = 0; i < n_; ++i) {
    Iterator* child = children_[i];
    if (!child->Valid()) continue;
    if (smallest == nullptr || icmp_->Compare(child->key(), smallest->key()) < 0) smallest = child;
  }
  current_ = smallest;
}

void MergingIterator::FindLargest() {
  Iterator* largest = nullptr;
  for (int i = 0; i < n_; ++i) {
    Iterator* child = children_[i];
    if (!child->Valid()) continue;
    if (largest == nullptr || icmp_->Compare(child->key(), largest->key()) > 0) largest = child;
  }
  current_ = largest;
}

Status MergingIterator::status() const {
  for (int i = 0; i < n_; ++i) {
    const Status s = children_[i]->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

void MergingIterator::SeekToFirst() {
  for (int i = 0; i < n_; ++i) children_[i]->SeekToFirst();
  FindSmallest();
  direction_ = kForward;
}

void MergingIterator::SeekToLast() {
  for (int i = 0; i < n_; ++i) children_[i]->SeekToLast();
  FindLargest();
  direction_ = kReverse;
}

void MergingIterator::Seek(const Slice& target) {
  for (int i = 0; i < n_; ++i) children_[i]->Seek(target);
  FindSmallest();
  direction_ = kForward;
}

void MergingIterator::Next() {
  assert(current_ != nullptr);
  // 把其他 child 定位到 key() 之后（LevelDB 的 merging_iterator.cc 同法）。
  if (direction_ != kForward) {
    for (int i = 0; i < n_; ++i) {
      Iterator* child = children_[i];
      if (child == current_) continue;
      child->Seek(current_->key());
      if (child->Valid() && icmp_->Compare(current_->key(), child->key()) == 0) child->Next();
    }
    direction_ = kForward;
  }
  current_->Next();
  FindSmallest();
}

void MergingIterator::Prev() {
  assert(current_ != nullptr);
  if (direction_ != kReverse) {
    for (int i = 0; i < n_; ++i) {
      Iterator* child = children_[i];
      if (child == current_) continue;
      child->Seek(current_->key());
      if (child->Valid()) {
        // child 位于第一个 >= key() 的条目；回退一条即 < key()。
        child->Prev();
      } else {
        child->SeekToLast();
      }
    }
    direction_ = kReverse;
  }
  current_->Prev();
  FindLargest();
}

}  // namespace lsm
