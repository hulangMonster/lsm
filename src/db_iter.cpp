// src/db_iter.cpp —— M3.2 的用户视图迭代器（算法与 M1 的 UserIterator 同源，增加 snapshot 过滤）。
#include "db_iter.h"

#include <utility>

namespace lsm {
namespace {

bool ParseEntry(const Slice& internal_key, Slice* user_key, SequenceNumber* seq, ValueType* type) {
  return ParseInternalKey(internal_key, user_key, seq, type);
}

}  // namespace

DBIter::DBIter(const InternalKeyComparator* icmp, Iterator* internal, SequenceNumber snapshot,
               std::vector<std::shared_ptr<const void>> keep_alive)
    : icmp_(icmp),
      internal_(internal),
      snapshot_(snapshot),
      keep_alive_(std::move(keep_alive)) {}

DBIter::~DBIter() { delete internal_; }

void DBIter::SetValid(const Slice& user_key) {
  current_user_key_.assign(user_key.data(), user_key.size());
  state_ = kValid;
}

void DBIter::RewindToRunStart() {
  if (!internal_->Valid()) return;
  Slice user_key;
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  if (!ParseEntry(internal_->key(), &user_key, &seq, &type)) return;
  // run start = 该 user key 的**最新版本**（与可见性无关）；用 kMaxSequenceNumber 定位，
  // 因此后续 Prev() 才会落到「前一个 user key」而不是同 key 的更旧版本。
  internal_->Seek(BuildLookupKey(user_key, kMaxSequenceNumber));
}

bool DBIter::MoveToPreviousUserKeyOf(const std::string& user_key) {
  internal_->Seek(BuildLookupKey(Slice(user_key), kMaxSequenceNumber));
  if (!internal_->Valid()) return false;
  internal_->Prev();
  if (!internal_->Valid()) return false;
  RewindToRunStart();
  return internal_->Valid();
}

void DBIter::SkipCurrentRunForwardWithKey(const Slice& user_key) {
  while (internal_->Valid()) {
    Slice uk;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseEntry(internal_->key(), &uk, &seq, &type)) break;
    if (icmp_->user_comparator()->Compare(uk, user_key) != 0) break;
    internal_->Next();
  }
}

void DBIter::SeekToFirst() {
  internal_->SeekToFirst();
  ScanForwardToVisible();
}

void DBIter::Seek(const Slice& target) {
  internal_->Seek(BuildLookupKey(target, snapshot_));
  ScanForwardToVisible();
}

void DBIter::SeekToLast() {
  internal_->SeekToLast();
  if (!internal_->Valid()) {
    state_ = kBeforeFirst;
    return;
  }
  RewindToRunStart();
  ScanBackwardToVisible();
}

void DBIter::Next() {
  if (state_ != kValid) return;
  SkipCurrentRunForwardWithKey(Slice(current_user_key_));
  ScanForwardToVisible();
}

void DBIter::Prev() {
  if (state_ == kBeforeFirst) return;
  if (state_ == kPastEnd) {
    SeekToLast();
    return;
  }
  if (!MoveToPreviousUserKeyOf(current_user_key_)) {
    state_ = kBeforeFirst;
    return;
  }
  ScanBackwardToVisible();
}

void DBIter::ScanForwardToVisible() {
  while (internal_->Valid()) {
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseEntry(internal_->key(), &user_key, &seq, &type)) {
      internal_->Next();
      continue;
    }
    if (seq > snapshot_) {   // 不可见版本；同 key 的更旧版本仍可能可见
      internal_->Next();
      continue;
    }
    if (type == kTypeValue) {
      SetValid(user_key);
      return;
    }
    // 可见 tombstone 屏蔽该 user key 的所有更旧版本。
    // 【I7 修复】user_key 是指向 internal_->key() 缓冲区的 Slice；SkipCurrentRunForwardWithKey 内部会反复
    // 调用 internal_->Next()，缓冲区随即被复用 ⇒ 传进去的 Slice 会悬垂，比较结果随机变成"不同 user key"
    // ⇒ 循环提前 break ⇒ 同一 user key 在其它 child 里的更旧版本逃过跳过，被外层当成可见值 emit
    // （表现为"全量扫描复活已删除的旧值"，而 Get/Seek 正确）。这里必须先复制成拥有型字符串。
    const std::string run_key(user_key.data(), user_key.size());
    SkipCurrentRunForwardWithKey(Slice(run_key));
  }
  state_ = kPastEnd;
}

void DBIter::ScanBackwardToVisible() {
  while (internal_->Valid()) {
    Slice user_key;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseEntry(internal_->key(), &user_key, &seq, &type)) {
      state_ = kPastEnd;
      return;
    }
    const std::string user = user_key.ToString();
    if (seq > snapshot_) {
      // 当前位置是某 user key 的 run start（kMaxSequenceNumber 定位）或 run 内更旧的版本。
      // 在 run 内向后（Next 方向 = seq 递减）找第一条可见版本。
      bool found = false;
      while (internal_->Valid()) {
        Slice u2;
        SequenceNumber s2 = 0;
        ValueType t2 = kTypeValue;
        if (!ParseEntry(internal_->key(), &u2, &s2, &t2)) {
          state_ = kPastEnd;
          return;
        }
        if (icmp_->user_comparator()->Compare(u2, Slice(user)) != 0) break;
        if (s2 <= snapshot_) {
          found = true;
          type = t2;
          break;
        }
        internal_->Next();
      }
      if (!found) {
        if (!MoveToPreviousUserKeyOf(user)) {
          state_ = kBeforeFirst;
          return;
        }
        continue;
      }
      if (type == kTypeValue) {
        SetValid(user);
        return;
      }
      if (!MoveToPreviousUserKeyOf(user)) {
        state_ = kBeforeFirst;
        return;
      }
      continue;
    }
    if (type == kTypeValue) {
      SetValid(user_key);
      return;
    }
    if (!MoveToPreviousUserKeyOf(user)) {
      state_ = kBeforeFirst;
      return;
    }
  }
  state_ = kBeforeFirst;
}

}  // namespace lsm
