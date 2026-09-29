// tests/version_test.cpp —— M4.1：VersionEdit 编码 + MANIFEST/CURRENT + META 迁移 + 安装失败可恢复
//   （docs/m4-design.md §10.1 的 M4-A01~A10、A39、A40、A42；全部走 MemEnv/FaultyEnv，零真实磁盘）
//
// 纪律：
//   * 每条用例都用**语义层**判据（回放出的文件集合、CURRENT 的内容、计数），不依赖 sleep；
//   * A04 的期望字节**手工拼装**（不复用 VersionEdit::EncodePayloadTo），CRC 复用 crc32c::Value
//     （同一实现的 CRC 无法手工复算，属已知薄弱点，登记在 docs/m4-prerequisites.md）；
//   * A39 手工构造只有 META 的旧库，验证兼容读入 + 一次性迁移；A40 走默认 MANIFEST 路径。
#include "test_harness.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "db_impl.h"
#include "faulty_env.h"
#include "filename.h"
#include "memenv.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "version_edit.h"
#include "version_set.h"

namespace lsm {
namespace {

using test::FaultyEnv;
using test::MemEnv;

std::string Key(char c, int n = 1) { return std::string(static_cast<size_t>(n), c); }

FileMetaData MakeFile(uint64_t number, const std::string& smallest_user,
                      const std::string& largest_user, uint64_t size = 128,
                      SequenceNumber seq = 100) {
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.max_sequence = seq;
  f.smallest = BuildInternalKey(smallest_user, seq, kTypeValue);
  f.largest = BuildInternalKey(largest_user, 1, kTypeValue);
  return f;
}

bool SameFile(const FileMetaData& a, const FileMetaData& b) {
  return a.number == b.number && a.file_size == b.file_size &&
         a.max_sequence == b.max_sequence && a.smallest == b.smallest && a.largest == b.largest;
}

VersionEdit MakeSnapshot(uint64_t log_number, uint64_t next_file,
                         const std::vector<std::pair<int, FileMetaData>>& files) {
  VersionEdit e;
  e.SetComparatorName(BytewiseComparator()->Name());
  e.SetLogNumber(log_number);
  e.SetMinLogNumberToKeep(log_number == 0 ? 1 : log_number);
  e.SetNextFileNumber(next_file);
  for (const auto& p : files) e.AddFile(p.first, p.second);
  return e;
}

void WriteManifestRecords(MemEnv* env, const std::string& dbname, uint64_t number,
                          const std::vector<VersionEdit>& edits) {
  std::string all;
  for (const VersionEdit& e : edits) {
    std::string rec;
    ASSERT_TRUE(EncodeManifestRecord(e, &rec)) << e.DebugString();
    all += rec;
  }
  env->SetContents(ManifestFileName(dbname, number), all);
}

std::vector<uint64_t> NumbersOf(const std::vector<FileMetaData>& fs) {
  std::vector<uint64_t> out;
  for (const FileMetaData& f : fs) out.push_back(f.number);
  return out;
}

// ---- A01 ----
TEST(VersionEdit, RoundTripEmptyAndFull) {
  VersionEdit empty;
  std::string bytes;
  ASSERT_TRUE(empty.EncodePayloadTo(&bytes));
  EXPECT_TRUE(bytes.empty());
  VersionEdit empty_decoded;
  std::string why;
  ASSERT_TRUE(empty_decoded.DecodePayloadFrom(Slice(bytes), &why)) << why;
  EXPECT_TRUE(empty_decoded.empty());
  EXPECT_FALSE(empty_decoded.has_comparator());
  EXPECT_FALSE(empty_decoded.has_log_number());
  EXPECT_FALSE(empty_decoded.has_next_file_number());
  EXPECT_FALSE(empty_decoded.has_min_log_number_to_keep());

  VersionEdit e;
  e.SetComparatorName("leveldb.BytewiseComparator");
  e.SetLogNumber(7);
  e.SetNextFileNumber(42);
  e.SetMinLogNumberToKeep(3);
  e.AddFile(0, MakeFile(5, Key('a', 65536), Key('b', 65536)));
  e.AddFile(0, MakeFile(4, Key('m'), Key('m')));
  e.AddFile(1, MakeFile(9, Key('x'), Key('z')));
  e.DeleteFile(1, 8);

  ASSERT_TRUE(e.EncodePayloadTo(&bytes));
  EXPECT_FALSE(bytes.empty());
  VersionEdit d;
  ASSERT_TRUE(d.DecodePayloadFrom(Slice(bytes), &why)) << why;
  EXPECT_EQ(e.comparator_name(), d.comparator_name());
  EXPECT_EQ(e.log_number(), d.log_number());
  EXPECT_EQ(e.next_file_number(), d.next_file_number());
  EXPECT_EQ(e.min_log_number_to_keep(), d.min_log_number_to_keep());
  ASSERT_EQ(e.added_files().size(), d.added_files().size());
  for (size_t i = 0; i < e.added_files().size(); ++i) {
    EXPECT_EQ(e.added_files()[i].first, d.added_files()[i].first);
    EXPECT_TRUE(SameFile(e.added_files()[i].second, d.added_files()[i].second));
  }
  ASSERT_EQ(1u, d.deleted_files().size());
  EXPECT_EQ(1, d.deleted_files()[0].first);
  EXPECT_EQ(8u, d.deleted_files()[0].second);
  // 编码长度必须与"逐字段手工长度"一致（catches 多写/漏写 tag 的实现错）。
  const auto vlen = [](uint64_t v) {
    size_t n = 1;
    while (v >= 128) { v >>= 7; ++n; }
    return n;
  };
  size_t manual = 0;
  manual += 1 + vlen(e.comparator_name().size()) + e.comparator_name().size();   // tag1 + len + name
  manual += 1 + vlen(7);                                 // tag2 + varint(7)
  manual += 1 + vlen(42);                                // tag3 + varint(42)
  manual += 1 + vlen(3);                                 // tag4 + varint(3)
  manual += 1 + 1 + vlen(8);                             // tag5 + level + varint(8)
  for (const auto& a : e.added_files()) {
    manual += 1 + 1 + vlen(a.second.number) + vlen(a.second.file_size) +
              vlen(a.second.max_sequence);
    manual += vlen(a.second.smallest.size()) + a.second.smallest.size();
    manual += vlen(a.second.largest.size()) + a.second.largest.size();
  }
  EXPECT_EQ(manual, bytes.size());
}

// ---- A02 ----
TEST(VersionEdit, OptionalFieldsAbsent) {
  VersionEdit only_new;
  only_new.AddFile(0, MakeFile(3, Key('a'), Key('a')));
  std::string b1;
  ASSERT_TRUE(only_new.EncodePayloadTo(&b1));
  VersionEdit d1;
  std::string why;
  ASSERT_TRUE(d1.DecodePayloadFrom(Slice(b1), &why)) << why;
  EXPECT_FALSE(d1.has_comparator());
  EXPECT_FALSE(d1.has_log_number());
  EXPECT_FALSE(d1.has_next_file_number());
  EXPECT_FALSE(d1.has_min_log_number_to_keep());
  ASSERT_EQ(1u, d1.added_files().size());
  EXPECT_TRUE(d1.deleted_files().empty());

  VersionEdit only_del;
  only_del.DeleteFile(2, 11);
  std::string b2;
  ASSERT_TRUE(only_del.EncodePayloadTo(&b2));
  VersionEdit d2;
  ASSERT_TRUE(d2.DecodePayloadFrom(Slice(b2), &why)) << why;
  EXPECT_FALSE(d2.has_comparator());
  EXPECT_FALSE(d2.has_log_number());
  EXPECT_FALSE(d2.has_next_file_number());
  EXPECT_FALSE(d2.has_min_log_number_to_keep());
  EXPECT_TRUE(d2.added_files().empty());
  ASSERT_EQ(1u, d2.deleted_files().size());
  EXPECT_EQ(2, d2.deleted_files()[0].first);
  EXPECT_EQ(11u, d2.deleted_files()[0].second);
}

// ---- A03 ----
TEST(VersionEdit, MalformedRejected) {
  std::string why;
  const auto expect_payload_reject = [&](const std::string& bytes, const char* what) {
    VersionEdit d;
    EXPECT_FALSE(d.DecodePayloadFrom(Slice(bytes), &why)) << what;
    EXPECT_FALSE(why.empty()) << what;
  };
  expect_payload_reject(std::string(1, static_cast<char>(kTagLogNumber)), "tag2 无 varint");
  expect_payload_reject(std::string(1, static_cast<char>(0x7f)), "未知 tag");
  {
    VersionEdit e;
    e.AddFile(7, MakeFile(3, Key('a'), Key('a')));
    std::string b;
    ASSERT_TRUE(e.EncodePayloadTo(&b));
    expect_payload_reject(b, "level 越界");
  }
  {
    VersionEdit e;
    e.AddFile(0, MakeFile(3, Key('a'), Key('a')));
    e.AddFile(1, MakeFile(3, Key('b'), Key('b')));
    std::string b;
    ASSERT_TRUE(e.EncodePayloadTo(&b));
    expect_payload_reject(b, "number 重复");
  }
  {
    VersionEdit e;
    FileMetaData f = MakeFile(3, Key('a'), Key('a'));
    f.file_size = 0;
    e.AddFile(0, f);
    std::string b;
    ASSERT_TRUE(e.EncodePayloadTo(&b));
    expect_payload_reject(b, "file_size == 0");
  }
  {
    VersionEdit e;
    FileMetaData f = MakeFile(3, Key('a'), Key('a'));
    f.smallest = BuildInternalKey("z", 100, kTypeValue);
    f.largest = BuildInternalKey("a", 1, kTypeValue);
    e.AddFile(0, f);
    std::string b;
    ASSERT_TRUE(e.EncodePayloadTo(&b));
    expect_payload_reject(b, "smallest > largest");
  }
  {
    VersionEdit e;
    FileMetaData f = MakeFile(3, Key('a'), Key('a'));
    f.smallest = std::string(3, 'x');   // 短于 kInternalKeyMinSize
    e.AddFile(0, f);
    std::string b;
    ASSERT_TRUE(e.EncodePayloadTo(&b));
    expect_payload_reject(b, "非 internal key");
  }
  // record 层：length == 0 / 越界 / 未知 type / 尾部截断 / CRC 坏
  {
    std::string rec;
    PutFixed32(&rec, 0);
    rec.push_back(static_cast<char>(kManifestRecordTypeVersionEdit));
    PutFixed32(&rec, 0);
    size_t off = 0;
    VersionEdit out;
    EXPECT_EQ(ManifestReadStatus::kCorruption, ReadManifestRecord(Slice(rec), &off, &out, &why));
  }
  {
    std::string rec;
    PutFixed32(&rec, 0xffffffffu);
    rec.push_back(static_cast<char>(kManifestRecordTypeVersionEdit));
    PutFixed32(&rec, 0);
    size_t off = 0;
    VersionEdit out;
    EXPECT_EQ(ManifestReadStatus::kCorruption, ReadManifestRecord(Slice(rec), &off, &out, &why));
  }
  {
    VersionEdit e;
    e.AddFile(0, MakeFile(3, Key('a'), Key('a')));
    std::string rec;
    ASSERT_TRUE(EncodeManifestRecord(e, &rec));
    rec[4] = static_cast<char>(0x7e);   // type 未知
    size_t off = 0;
    VersionEdit out;
    EXPECT_EQ(ManifestReadStatus::kNotSupported, ReadManifestRecord(Slice(rec), &off, &out, &why));
  }
  {
    VersionEdit e;
    e.AddFile(0, MakeFile(3, Key('a'), Key('a')));
    std::string rec;
    ASSERT_TRUE(EncodeManifestRecord(e, &rec));
    const std::string torn = rec.substr(0, rec.size() - 2);
    size_t off = 0;
    VersionEdit out;
    EXPECT_EQ(ManifestReadStatus::kTailResidue, ReadManifestRecord(Slice(torn), &off, &out, &why));
  }
  {
    VersionEdit e;
    e.AddFile(0, MakeFile(3, Key('a'), Key('a')));
    std::string rec;
    ASSERT_TRUE(EncodeManifestRecord(e, &rec));
    rec[rec.size() - 1] = static_cast<char>(rec[rec.size() - 1] ^ 0x5a);
    size_t off = 0;
    VersionEdit out;
    EXPECT_EQ(ManifestReadStatus::kCorruption, ReadManifestRecord(Slice(rec), &off, &out, &why));
  }
}

// ---- A04：手工拼字节参照 ----
TEST(Manifest, RecordBytesMatchHandBuilt) {
  VersionEdit e;
  e.SetComparatorName("C");
  e.SetLogNumber(300);
  e.SetNextFileNumber(9);
  e.SetMinLogNumberToKeep(2);
  e.AddFile(1, MakeFile(4, "k", "k", 200, 300));
  std::string got;
  ASSERT_TRUE(EncodeManifestRecord(e, &got));

  // ---- 手工拼 payload（不复用 EncodePayloadTo）----
  const auto put_varint64 = [](std::string* d, uint64_t v) {
    while (v >= 128) { d->push_back(static_cast<char>((v & 0x7f) | 0x80)); v >>= 7; }
    d->push_back(static_cast<char>(v));
  };
  const auto put_len_slice = [&](std::string* d, const std::string& s) {
    d->push_back(static_cast<char>(s.size()));   // 都 < 128
    d->append(s);
  };
  std::string payload;
  payload.push_back(static_cast<char>(kTagComparator));
  put_len_slice(&payload, "C");
  payload.push_back(static_cast<char>(kTagLogNumber));
  put_varint64(&payload, 300);
  payload.push_back(static_cast<char>(kTagNextFileNumber));
  put_varint64(&payload, 9);
  payload.push_back(static_cast<char>(kTagMinLogNumberToKeep));
  put_varint64(&payload, 2);
  payload.push_back(static_cast<char>(kTagNewFile));
  payload.push_back(1);                   // level
  put_varint64(&payload, 4);
  put_varint64(&payload, 200);
  put_varint64(&payload, 300);
  put_len_slice(&payload, e.added_files()[0].second.smallest);
  put_len_slice(&payload, e.added_files()[0].second.largest);

  std::string expected;
  PutFixed32(&expected, static_cast<uint32_t>(payload.size()));
  expected.push_back(static_cast<char>(kManifestRecordTypeVersionEdit));
  expected.append(payload);
  PutFixed32(&expected, crc32c::Value(expected.data(), expected.size()));
  EXPECT_EQ(expected, got);
  EXPECT_EQ(payload.size() + 9u, got.size());
}

// ---- A05 ----
TEST(Manifest, ReplayFullSnapshot) {
  MemEnv env;
  ASSERT_TRUE(env.CreateDir("/db").ok());
  const FileMetaData f3 = MakeFile(3, Key('a'), Key('c'));
  const FileMetaData f5 = MakeFile(5, Key('p'), Key('q'));
  const FileMetaData f7 = MakeFile(7, "m", "n");
  VersionEdit snap = MakeSnapshot(/*log=*/11, /*next=*/20, {{0, f3}, {0, f5}, {1, f7}});
  VersionEdit inc1;
  inc1.DeleteFile(0, 3);
  VersionEdit inc2;
  inc2.AddFile(1, MakeFile(9, "x", "z"));
  WriteManifestRecords(&env, "/db", 1, {snap, inc1, inc2});
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 1).ok());

  std::shared_ptr<const Version> v;
  VersionSet::ManifestReplayResult info;
  const Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_TRUE(v != nullptr);
  EXPECT_EQ(3u, info.edits_replayed);
  EXPECT_EQ(3u, info.next_version_number);
  EXPECT_EQ(1u, info.manifest_number);
  EXPECT_TRUE(info.manifest_present);
  EXPECT_EQ(0u, info.tail_truncated_bytes);
  EXPECT_EQ(std::vector<uint64_t>({5}), NumbersOf(v->level_files(0)));
  EXPECT_EQ(std::vector<uint64_t>({7, 9}), NumbersOf(v->level_files(1)));
  EXPECT_EQ(11u, v->log_number());
  EXPECT_EQ(20u, v->next_file_number());
  EXPECT_GE(v->next_file_number(), info.max_directory_number + 1);
}

// ---- A06 ----
TEST(Manifest, TailTornTruncatedAndCounted) {
  MemEnv env;
  ASSERT_TRUE(env.CreateDir("/db").ok());
  const FileMetaData f3 = MakeFile(3, Key('a'), Key('c'));
  VersionEdit snap = MakeSnapshot(5, 20, {{0, f3}});
  VersionEdit inc;
  inc.AddFile(0, MakeFile(6, Key('d'), Key('e')));
  std::string r1, r2;
  ASSERT_TRUE(EncodeManifestRecord(snap, &r1));
  ASSERT_TRUE(EncodeManifestRecord(inc, &r2));
  env.SetContents(ManifestFileName("/db", 1), r1 + r2);
  // 尾部撕掉 3 字节 ⇒ 第二条 record 不完整
  ASSERT_TRUE(env.Truncate(ManifestFileName("/db", 1), r1.size() + r2.size() - 3).ok());
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 1).ok());

  std::shared_ptr<const Version> v;
  VersionSet::ManifestReplayResult info;
  Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(1u, info.edits_replayed);
  EXPECT_EQ(r2.size() - 3, info.tail_truncated_bytes);
  EXPECT_EQ(std::vector<uint64_t>({3}), NumbersOf(v->level_files(0)));
  EXPECT_EQ(r1.size(), env.Contents(ManifestFileName("/db", 1)).size());

  // 截断后追加必须仍可回放（证明截断真的落了盘）
  uint64_t bytes = 0;
  ASSERT_TRUE(VersionSet::AppendEdit(&env, "/db", 1, inc, &bytes).ok());
  std::shared_ptr<const Version> v2;
  VersionSet::ManifestReplayResult info2;
  s = VersionSet::RecoverManifest(&env, "/db", Options(), &v2, &info2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(2u, info2.edits_replayed);
  EXPECT_EQ(0u, info2.tail_truncated_bytes);
  EXPECT_EQ(std::vector<uint64_t>({6, 3}), NumbersOf(v2->level_files(0)));
}

// ---- A07 ----
TEST(Manifest, MiddleCorruptionRejected) {
  MemEnv env;
  ASSERT_TRUE(env.CreateDir("/db").ok());
  VersionEdit e1 = MakeSnapshot(5, 20, {{0, MakeFile(3, Key('a'), Key('a'))}});
  VersionEdit e2;
  e2.AddFile(0, MakeFile(6, Key('b'), Key('b')));
  std::string r1, r2;
  ASSERT_TRUE(EncodeManifestRecord(e1, &r1));
  ASSERT_TRUE(EncodeManifestRecord(e2, &r2));
  r1[r1.size() - 1] = static_cast<char>(r1[r1.size() - 1] ^ 0x5a);   // 第一条的 CRC 坏，其后仍有完整 record
  env.SetContents(ManifestFileName("/db", 1), r1 + r2);
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 1).ok());
  std::shared_ptr<const Version> v;
  VersionSet::ManifestReplayResult info;
  const Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(r1.size() + r2.size(), env.Contents(ManifestFileName("/db", 1)).size());   // 不截断
}

// ---- A08 ----
TEST(Current, AtomicSwitchOnInjectedCrash) {
  MemEnv env;
  ASSERT_TRUE(env.CreateDir("/db").ok());
  VersionEdit m1 = MakeSnapshot(5, 20, {{0, MakeFile(3, Key('a'), Key('a'))}});
  VersionEdit m2 = MakeSnapshot(5, 20, {{0, MakeFile(3, Key('a'), Key('a'))},
                                        {0, MakeFile(4, Key('b'), Key('b'))}});
  WriteManifestRecords(&env, "/db", 1, {m1});
  WriteManifestRecords(&env, "/db", 2, {m2});
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 1).ok());
  // 模拟"rename(CURRENT.tmp, CURRENT) 之前"崩溃：CURRENT.tmp 已写了新编号，CURRENT 仍是旧值。
  env.SetContents(CurrentTempFileName("/db"), "2\n");
  std::shared_ptr<const Version> v;
  VersionSet::ManifestReplayResult info;
  Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(1u, info.manifest_number);
  EXPECT_EQ(std::vector<uint64_t>({3}), NumbersOf(v->level_files(0)));

  // rename 之后：CURRENT 指向新 MANIFEST，两个状态都能完整打开。
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 2).ok());
  std::shared_ptr<const Version> v2;
  VersionSet::ManifestReplayResult info2;
  s = VersionSet::RecoverManifest(&env, "/db", Options(), &v2, &info2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(2u, info2.manifest_number);
  EXPECT_EQ(std::vector<uint64_t>({4, 3}), NumbersOf(v2->level_files(0)));
}

// ---- A09 ----
TEST(Current, MissingWithNonEmptyDirIsCorruption) {
  {
    MemEnv env;
    ASSERT_TRUE(env.CreateDir("/empty").ok());
    std::shared_ptr<const Version> v;
    VersionSet::ManifestReplayResult info;
    const Status s = VersionSet::RecoverManifest(&env, "/empty", Options(), &v, &info);
    EXPECT_TRUE(s.ok()) << s.ToString();
    EXPECT_FALSE(info.manifest_present);
  }
  {
    MemEnv env;
    ASSERT_TRUE(env.CreateDir("/db").ok());
    env.SetContents(ManifestFileName("/db", 1), "irrelevant");
    std::shared_ptr<const Version> v;
    VersionSet::ManifestReplayResult info;
    const Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  }
  {
    MemEnv env;
    ASSERT_TRUE(env.CreateDir("/db").ok());
    env.SetContents(TableFileName("/db", 5), "irrelevant");
    std::shared_ptr<const Version> v;
    VersionSet::ManifestReplayResult info;
    const Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  }
  {
    // CURRENT 缺失但 CURRENT.tmp 存在 ⇒ 安全阀（不得自动修复）
    MemEnv env;
    ASSERT_TRUE(env.CreateDir("/db").ok());
    env.SetContents(CurrentTempFileName("/db"), "1\n");
    std::shared_ptr<const Version> v;
    VersionSet::ManifestReplayResult info;
    const Status s = VersionSet::RecoverManifest(&env, "/db", Options(), &v, &info);
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  }
}

// ---- A10 ----
TEST(Current, ContentStrictlyValidated) {
  MemEnv env;
  ASSERT_TRUE(env.CreateDir("/db").ok());
  const char* bad[] = {"abc", "12", "1\n\n", "1x\n", "123456789012345678901\n"};
  for (const char* b : bad) {
    env.SetContents(CurrentFileName("/db"), b);
    uint64_t n = 0;
    const Status s = VersionSet::ReadCurrent(&env, "/db", &n);
    EXPECT_TRUE(s.IsCorruption()) << "内容 " << b << " 必须 kCorruption：" << s.ToString();
  }
  env.SetContents(CurrentFileName("/db"), "42\n");
  uint64_t n = 0;
  const Status s = VersionSet::ReadCurrent(&env, "/db", &n);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(42u, n);
}

// ---- A39 ----
// 旧库兼容读入 + 一次性迁移：构造一个**只有 META、没有 CURRENT/MANIFEST** 的 M3 形态库
// （用真实的 .sst；META 用 M3 的定宽编码写），首次 Open 必须迁移到 MANIFEST+CURRENT 并删除 META。
TEST(Migration, MetaToManifestOneShot) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 4096;
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  for (int i = 0; i < 200; ++i) db->Put("k" + std::to_string(i), "v" + std::to_string(i));
  ASSERT_TRUE(static_cast<PersistentDBImpl*>(db)->ForceFlushForTest().ok());
  ASSERT_TRUE(db->Close().ok());
  delete db;

  // 把活动 MANIFEST 回放成 Version，再以 META 定宽编码写出 ⇒ 得到一个"M3 旧库"。
  uint64_t n = 0;
  ASSERT_TRUE(VersionSet::ReadCurrent(&env, "/db", &n).ok());
  const std::string contents = env.Contents(ManifestFileName("/db", n));
  ASSERT_FALSE(contents.empty());
  std::shared_ptr<const Version> v = VersionSet::Empty(0, 1);
  {
    size_t off = 0;
    std::string why;
    while (off < contents.size()) {
      VersionEdit e;
      ASSERT_EQ(ManifestReadStatus::kOk, ReadManifestRecord(Slice(contents), &off, &e, &why)) << why;
      std::shared_ptr<const Version> next;
      ASSERT_TRUE(VersionSet::ApplyEdit(*v, e, &next, &why)) << why;
      v = next;
    }
  }
  const VersionEdit legacy = VersionSet::MakeSnapshotEdit(*v, o);
  std::string meta_bytes;
  ASSERT_TRUE(legacy.EncodeTo(&meta_bytes));
  env.SetContents(VersionSet::MetaFileName("/db"), meta_bytes);
  env.RemoveFile(CurrentFileName("/db"));
  env.RemoveFile(ManifestFileName("/db", n));
  ASSERT_FALSE(env.FileExists(CurrentFileName("/db")));
  ASSERT_TRUE(env.FileExists(VersionSet::MetaFileName("/db")));

  DB* db2 = nullptr;
  const Status s = DB::Open(o, "/db", &db2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  auto* impl2 = static_cast<PersistentDBImpl*>(db2);
  const RecoveryStats rs = impl2->GetRecoveryStats();
  EXPECT_EQ(1u, rs.meta_migrated);
  EXPECT_TRUE(rs.manifest_present);
  EXPECT_GT(rs.manifest_number, 0u);
  EXPECT_TRUE(env.FileExists(CurrentFileName("/db")));
  EXPECT_TRUE(env.FileExists(ManifestFileName("/db", rs.manifest_number)));
  EXPECT_FALSE(env.FileExists(VersionSet::MetaFileName("/db")));
  EXPECT_FALSE(env.FileExists(VersionSet::MetaTempFileName("/db")));
  std::string v5;
  ASSERT_TRUE(db2->Get("k5", &v5).ok());
  EXPECT_EQ("v5", v5);
  ASSERT_TRUE(db2->Close().ok());
  delete db2;

  DB* db3 = nullptr;
  const Status s3 = DB::Open(o, "/db", &db3);
  ASSERT_TRUE(s3.ok()) << s3.ToString();
  auto* impl3 = static_cast<PersistentDBImpl*>(db3);
  EXPECT_EQ(0u, impl3->GetRecoveryStats().meta_migrated);
  EXPECT_TRUE(impl3->GetRecoveryStats().manifest_present);
  std::string v7;
  ASSERT_TRUE(db3->Get("k7", &v7).ok());
  EXPECT_EQ("v7", v7);
  ASSERT_TRUE(db3->Close().ok());
  delete db3;
}

// ---- A40 ----
TEST(Manifest, RollWritesSnapshotAndKeepsNumberingMonotonic) {
  MemEnv env;
  Options o;
  o.env = &env;
  o.write_buffer_size = 1024;
  o.manifest_roll_bytes = 1;   // 每次 flush 后都越过阈值 ⇒ 走模式 (a)
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db).ok());
  for (int i = 0; i < 500; ++i) db->Put("a" + std::to_string(i), "v" + std::to_string(i));
  auto* impl = static_cast<PersistentDBImpl*>(db);
  ASSERT_TRUE(impl->ForceFlushForTest().ok());
  EXPECT_GE(impl->manifest_rolls(), 1u);
  const uint64_t next = impl->next_file_number();
  const uint64_t manifest_number = impl->manifest_number();
  EXPECT_GT(manifest_number, 0u);
  ASSERT_TRUE(db->Close().ok());
  delete db;

  // X5：所有族（.log/.sst/MANIFEST）的编号必须严格小于 next_file_number_。
  std::vector<std::string> children;
  ASSERT_TRUE(env.GetChildren("/db", &children).ok());
  size_t counted = 0;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseTableFileName(c, &n) || ParseTempFileName(c, &n) || ParseLogFileName(c, &n) ||
        ParseManifestFileName(c, &n) || ParseManifestTempFileName(c, &n)) {
      ++counted;
      EXPECT_LT(n, next) << "编号重用风险：" << c;
    }
  }
  EXPECT_GT(counted, 0u);
  // CURRENT 必须指向某个可回放的 MANIFEST
  uint64_t cur = 0;
  ASSERT_TRUE(VersionSet::ReadCurrent(&env, "/db", &cur).ok());
  EXPECT_TRUE(env.FileExists(ManifestFileName("/db", cur)));
  DB* db2 = nullptr;
  ASSERT_TRUE(DB::Open(o, "/db", &db2).ok());
  std::string v;
  ASSERT_TRUE(db2->Get("a10", &v).ok());
  EXPECT_EQ("v10", v);
  ASSERT_TRUE(db2->Close().ok());
  delete db2;
}

// ---- A42 ----
TEST(Install, FailureKeepsCurrentRecoverable) {
  MemEnv base;
  FaultyEnv env(&base);
  ASSERT_TRUE(env.CreateDir("/db").ok());
  const FileMetaData f3 = MakeFile(3, Key('a'), Key('c'));
  auto v0 = std::make_shared<const Version>(std::vector<FileMetaData>({f3}), 5, 1, 20);
  ASSERT_TRUE(VersionSet::WriteSnapshotManifest(&env, "/db", 1, *v0, Options()).ok());
  ASSERT_TRUE(VersionSet::WriteCurrentAtomic(&env, "/db", 1).ok());

  // 追加失败：Sync 注入失败 ⇒ AppendEdit 非 OK；CURRENT 仍指向 1。
  VersionEdit inc;
  inc.AddFile(0, MakeFile(6, Key('d'), Key('e')));
  env.SetSyncFailureAfter(1);
  uint64_t bytes = 0;
  const Status s = VersionSet::AppendEdit(&env, "/db", 1, inc, &bytes);
  EXPECT_FALSE(s.ok()) << s.ToString();
  env.ClearSyncFailures();
  uint64_t cur = 0;
  ASSERT_TRUE(VersionSet::ReadCurrent(&base, "/db", &cur).ok());
  EXPECT_EQ(1u, cur);
  {
    std::shared_ptr<const Version> v;
    VersionSet::ManifestReplayResult info;
    const Status rs = VersionSet::RecoverManifest(&base, "/db", Options(), &v, &info);
    EXPECT_TRUE(rs.ok()) << "CURRENT 指向的 MANIFEST 必须仍可完整回放：" << rs.ToString();
  }

  // 重建失败：Sync 注入失败 ⇒ WriteSnapshotManifest 非 OK；CURRENT 不被切换。
  env.SetSyncFailureAfter(1);
  std::shared_ptr<const Version> v1;
  std::string why;
  ASSERT_TRUE(VersionSet::ApplyEdit(*v0, inc, &v1, &why)) << why;
  const Status s2 = VersionSet::WriteSnapshotManifest(&env, "/db", 2, *v1, Options());
  EXPECT_FALSE(s2.ok()) << s2.ToString();
  env.ClearSyncFailures();
  ASSERT_TRUE(VersionSet::ReadCurrent(&base, "/db", &cur).ok());
  EXPECT_EQ(1u, cur);
  EXPECT_FALSE(base.FileExists(ManifestFileName("/db", 2)));
  std::shared_ptr<const Version> v2;
  VersionSet::ManifestReplayResult info2;
  const Status rs2 = VersionSet::RecoverManifest(&base, "/db", Options(), &v2, &info2);
  EXPECT_TRUE(rs2.ok()) << rs2.ToString();
  // 追加失败的形态是"record 已写出但 fsync 失败"⇒ 回放可能多一条 edit（§8.2 登记过这一固有性质）；
  // 判据是"旧文件仍在、CURRENT 可完整回放"，而不是"文件集合恰好不变"。
  const std::vector<uint64_t> files2 = NumbersOf(v2->level_files(0));
  EXPECT_NE(files2.end(), std::find(files2.begin(), files2.end(), 3u));
}

}  // namespace
}  // namespace lsm
