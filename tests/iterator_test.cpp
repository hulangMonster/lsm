// tests/iterator_test.cpp —— M3.2 归并/用户视图迭代器用例（docs/m3-design.md §10.1：A31~A34）
#include "test_harness.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "db_impl.h"
#include "db_iter.h"
#include "memenv.h"
#include "merging_iterator.h"

namespace lsm {
namespace test {
namespace {

using Entry = std::tuple<std::string, SequenceNumber, ValueType, std::string>;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%06d", i);
  return std::string(buf);
}
std::string Val(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%06d", i);
  return std::string(buf);
}

std::unique_ptr<MemTable> MakeMem(const InternalKeyComparator& icmp,
                                  const std::vector<Entry>& entries) {
  std::unique_ptr<MemTable> mem(new MemTable(icmp, 1u << 20));
  for (const Entry& e : entries) {
    const Status s = mem->Add(std::get<1>(e), std::get<2>(e), std::get<0>(e), std::get<3>(e));
    EXPECT_TRUE(s.ok()) << s.ToString();
  }
  return mem;
}

std::vector<std::pair<std::string, std::string>> SortedInternal(
    const InternalKeyComparator& icmp, const std::vector<Entry>& entries) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const Entry& e : entries) {
    out.emplace_back(BuildInternalKey(std::get<0>(e), std::get<1>(e), std::get<2>(e)),
                     std::get<3>(e));
  }
  std::sort(out.begin(), out.end(), [&icmp](const std::pair<std::string, std::string>& a,
                                            const std::pair<std::string, std::string>& b) {
    return icmp.Compare(Slice(a.first), Slice(b.first)) < 0;
  });
  return out;
}

std::vector<std::pair<std::string, std::string>> CollectMerged(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> CollectMergedBackward(Iterator* it) {
  std::vector<std::pair<std::string, std::string>> out;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  return out;
}

}  // namespace

// ==== M3-A31 ====
TEST(Iter, MergingIteratorOrder) {
  const InternalKeyComparator icmp(BytewiseComparator());
  const std::vector<Entry> e0 = {{"a", 1, kTypeValue, "va1"}, {"c", 3, kTypeValue, "vc3"}};
  const std::vector<Entry> e1 = {{"b", 2, kTypeValue, "vb2"}, {"c", 4, kTypeValue, "vc4"}};
  const std::vector<Entry> e2;
  std::unique_ptr<MemTable> m0 = MakeMem(icmp, e0);
  std::unique_ptr<MemTable> m1 = MakeMem(icmp, e1);
  std::unique_ptr<MemTable> m2 = MakeMem(icmp, e2);

  Iterator** kids = new Iterator*[3];
  kids[0] = m0->NewIterator();
  kids[1] = m1->NewIterator();
  kids[2] = m2->NewIterator();
  MergingIterator merged(&icmp, kids, 3);

  std::vector<Entry> all = e0;
  all.insert(all.end(), e1.begin(), e1.end());
  const std::vector<std::pair<std::string, std::string>> model = SortedInternal(icmp, all);
  const std::vector<std::pair<std::string, std::string>> fwd = CollectMerged(&merged);
  ASSERT_EQ(model.size(), fwd.size());
  for (size_t i = 0; i < model.size(); ++i) {
    EXPECT_EQ(model[i].first, fwd[i].first) << "i=" << i;
    EXPECT_EQ(model[i].second, fwd[i].second) << "i=" << i;
  }
  std::vector<std::pair<std::string, std::string>> model_rev = model;
  std::reverse(model_rev.begin(), model_rev.end());
  EXPECT_EQ(model_rev, CollectMergedBackward(&merged));
  EXPECT_TRUE(merged.status().ok());

  // 空 child 集合：Valid()==false，任何 Seek 都不得崩。
  Iterator** none = new Iterator*[0];
  MergingIterator empty(&icmp, none, 0);
  EXPECT_FALSE(empty.Valid());
  empty.SeekToFirst();
  EXPECT_FALSE(empty.Valid());
  empty.SeekToLast();
  EXPECT_FALSE(empty.Valid());
  empty.Seek(Slice("x"));
  EXPECT_FALSE(empty.Valid());
  EXPECT_TRUE(empty.status().ok());

  // status() 取第一个非 OK：把错误 child 放在前面，必须报它。
  Iterator** mix = new Iterator*[2];
  mix[0] = NewStatusIterator(Status::Corruption("child0 corruption"));
  mix[1] = m2->NewIterator();
  MergingIterator bad(&icmp, mix, 2);
  bad.SeekToFirst();
  EXPECT_TRUE(bad.status().IsCorruption()) << bad.status().ToString();
}

// ==== M3-A32 ====
TEST(Iter, DBIterVisibilityAndTombstoneSkip) {
  const InternalKeyComparator icmp(BytewiseComparator());
  const std::vector<Entry> entries = {
      {"a", 10, kTypeValue, "a10"}, {"a", 5, kTypeValue, "a5"},
      {"b", 9, kTypeValue, "b9"},   {"b", 4, kTypeDeletion, ""},
      {"b", 2, kTypeValue, "b2"},   {"c", 6, kTypeValue, "c6"}};
  std::unique_ptr<MemTable> mem = MakeMem(icmp, entries);
  const auto make = [&mem, &icmp](SequenceNumber snap) -> Iterator* {
    Iterator** kids = new Iterator*[1];
    kids[0] = mem->NewIterator();
    return new DBIter(&icmp, new MergingIterator(&icmp, kids, 1), snap);
  };

  // snapshot 8：a10 不可见（10>8）⇒ a5；b4 的 tombstone 可见 ⇒ b 整条屏蔽；c6 可见。
  std::unique_ptr<Iterator> it(make(8));
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("a", it->key().ToString());
  EXPECT_EQ("a5", it->value().ToString());
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("c", it->key().ToString());
  EXPECT_EQ("c6", it->value().ToString());
  it->Next();
  EXPECT_FALSE(it->Valid());

  it->Seek("b");
  ASSERT_TRUE(it->Valid()) << "tombstone 之后 Seek 必须落在下一条可见 key";
  EXPECT_EQ("c", it->key().ToString());

  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("c", it->key().ToString());
  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("a", it->key().ToString());
  EXPECT_EQ("a5", it->value().ToString());
  it->Prev();
  EXPECT_FALSE(it->Valid());

  // snapshot 3：b4 的 tombstone 不在快照内 ⇒ b2 仍可见；a/c 只有更旧/更新的版本不可见。
  std::unique_ptr<Iterator> it3(make(3));
  it3->SeekToFirst();
  ASSERT_TRUE(it3->Valid());
  EXPECT_EQ("b", it3->key().ToString());
  EXPECT_EQ("b2", it3->value().ToString());
  it3->Next();
  EXPECT_FALSE(it3->Valid());
  it3->SeekToLast();
  ASSERT_TRUE(it3->Valid());
  EXPECT_EQ("b", it3->key().ToString());
  EXPECT_EQ("b2", it3->value().ToString());
}

// ==== M3-A33 ====
TEST(Iter, DBIterStableAcrossFlush) {
  MemEnv env;
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 * 1024;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(options, "/db", &db).ok());
  PersistentDBImpl* impl = static_cast<PersistentDBImpl*>(db);

  std::map<std::string, std::string> model;
  for (int i = 1; i <= 400; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
    model[Key(i)] = Val(i);
  }
  std::unique_ptr<Iterator> fwd(db->NewIterator());
  std::unique_ptr<Iterator> bwd(db->NewIterator());

  // 迭代中途触发 flush + 注册：迭代器必须持有旧快照（MemTable/Version 的 shared_ptr）。
  for (int i = 401; i <= 800; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
  }
  for (int spin = 0; spin < 400000; ++spin) {
    if (impl->GetFlushStats().flushes_completed >= 1 && impl->immutables_size() == 0) break;
    std::this_thread::yield();
  }
  EXPECT_GE(impl->GetFlushStats().flushes_completed, 1u);

  ExpectSameAsStdMap("stable-fwd", model, fwd.get());
  std::vector<std::pair<std::string, std::string>> expected_rev(model.rbegin(), model.rend());
  EXPECT_EQ(expected_rev, CollectBackward(bwd.get()));
  EXPECT_TRUE(db->Close().ok());
  delete db;
}

// ==== M3-A34 ====
TEST(Iter, UserIteratorUnchangedForMemoryMode) {
  std::unique_ptr<DB> db(OpenMemoryDB());
  ASSERT_TRUE(db != nullptr);
  const int kCount = 300;
  std::map<std::string, std::string> model;
  for (int i = 1; i <= kCount; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Val(i)).ok());
    model[Key(i)] = Val(i);
  }
  for (int i = 1; i <= kCount; i += 7) {
    ASSERT_TRUE(db->Delete(Key(i)).ok());
    model.erase(Key(i));
  }
  for (int i = 1; i <= kCount; i += 11) {
    const std::string nv = Val(i) + "-overwritten";
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), nv).ok());
    model[Key(i)] = nv;
  }

  std::unique_ptr<Iterator> it(db->NewIterator());
  ExpectSameAsStdMap("memory-forward", model, it.get());
  std::unique_ptr<Iterator> rit(db->NewIterator());
  std::vector<std::pair<std::string, std::string>> expected_rev(model.rbegin(), model.rend());
  EXPECT_EQ(expected_rev, CollectBackward(rit.get()));

  // Seek 落在下一条可见 key；Prev 回到上一条。
  std::unique_ptr<Iterator> sit(db->NewIterator());
  const std::string target = Key(kCount / 2);
  sit->Seek(target);
  auto mit = model.lower_bound(target);
  if (mit == model.end()) {
    EXPECT_FALSE(sit->Valid());
  } else {
    ASSERT_TRUE(sit->Valid());
    EXPECT_EQ(mit->first, sit->key().ToString());
    EXPECT_EQ(mit->second, sit->value().ToString());
    if (mit != model.begin()) {
      auto prev = mit;
      --prev;
      sit->Prev();
      ASSERT_TRUE(sit->Valid());
      EXPECT_EQ(prev->first, sit->key().ToString());
    }
  }
}

}  // namespace test
}  // namespace lsm
