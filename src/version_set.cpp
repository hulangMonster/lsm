// src/version_set.cpp —— M3.2/M3.3：文件命名、不可变 Version、内存 TableCache、META 持久化。
#include "version_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "filename.h"
#include "merging_iterator.h"
#include "util/env.h"

namespace lsm {
namespace {

bool ParseNumberSuffix(const std::string& fname, const char* suffix, uint64_t* number) {
  const size_t suffix_len = std::strlen(suffix);
  if (fname.size() <= suffix_len) return false;
  if (fname.compare(fname.size() - suffix_len, suffix_len, suffix) != 0) return false;
  uint64_t n = 0;
  const size_t digits = fname.size() - suffix_len;
  for (size_t i = 0; i < digits; ++i) {
    const char c = fname[i];
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

// META 是单文件全量快照；64 MiB 上限只是防御性的（§12.5 的已知薄弱点：文件数很大时它会变胖）。
constexpr size_t kMaxMetaFileBytes = 64u * 1024 * 1024;

Status ReadWholeFile(Env* env, const std::string& path, std::string* out) {
  out->clear();
  SequentialFile* raw = nullptr;
  Status s = env->NewSequentialFile(path, &raw);
  if (!s.ok()) return s;
  if (raw == nullptr) return Status::IOError("ReadWholeFile: env returned null file", path);
  std::unique_ptr<SequentialFile> file(raw);
  char scratch[65536];
  while (true) {
    Slice piece;
    s = file->Read(sizeof(scratch), &piece, scratch);
    if (!s.ok()) return s;
    if (piece.empty()) break;
    out->append(piece.data(), piece.size());
    if (out->size() > kMaxMetaFileBytes) {
      return Status::Corruption("ReadWholeFile: 文件超过 64 MiB 上限", path);
    }
  }
  return Status::OK();
}

// M3-A38（§8.2 纪律 3）：打开已注册的 .sst 全量扫一遍，独立算出真实 max_sequence。
// 注意：这条与 META 的写入方共用同一个块解码器，属 §12.5 第 7 条登记的"自证风险"；
// 因此 A38 另配"手工拼字节参照"的子断言（见 tests/recovery_m3_test.cpp）。
Status ScanFileMaxSequence(Env* env, const Options& options, const std::string& path,
                           const FileMetaData& f, SequenceNumber* actual,
                           uint64_t* unknown_metaindex_entries) {
  std::shared_ptr<Table> table;
  Status s = Table::Open(options, env, path, &table, &f.smallest, &f.largest);
  if (!s.ok()) return s;
  if (unknown_metaindex_entries != nullptr) {
    *unknown_metaindex_entries += table->unknown_metaindex_entries();
  }
  SequenceNumber m = 0;
  std::unique_ptr<Iterator> it = table->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    Slice uk;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(it->key(), &uk, &seq, &type)) {
      return Status::Corruption("ScanFileMaxSequence: 文件里存在畸形 internal key", path);
    }
    if (seq > m) m = seq;
  }
  if (!it->status().ok()) return it->status();
  *actual = m;
  return Status::OK();
}

}  // namespace

std::string TableFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.sst", static_cast<unsigned long long>(number));
  return MakeFileName(dbname, buf);
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.sst.tmp", static_cast<unsigned long long>(number));
  return MakeFileName(dbname, buf);
}

bool ParseTableFileName(const std::string& fname, uint64_t* number) {
  // 精确后缀：解析顺序不能让 "%06u.sst.tmp" 命中 ".sst"。
  if (ParseTempFileName(fname, number)) return false;
  return ParseNumberSuffix(fname, ".sst", number);
}

bool ParseTempFileName(const std::string& fname, uint64_t* number) {
  return ParseNumberSuffix(fname, ".sst.tmp", number);
}

Version::Version(std::vector<FileMetaData> files, uint64_t log_number,
                 uint64_t min_log_number_to_keep, uint64_t next_file_number)
    : files_(std::move(files)),
      log_number_(log_number),
      min_log_number_to_keep_(min_log_number_to_keep),
      next_file_number_(next_file_number) {
  std::sort(files_.begin(), files_.end(),
            [](const FileMetaData& a, const FileMetaData& b) { return a.number > b.number; });
}

SequenceNumber Version::MaxSequenceInFiles() const {
  SequenceNumber m = 0;
  for (const FileMetaData& f : files_) {
    if (f.max_sequence > m) m = f.max_sequence;
  }
  return m;
}

std::shared_ptr<const Version> VersionSet::Empty(uint64_t log_number, uint64_t next_file_number) {
  return std::make_shared<const Version>(std::vector<FileMetaData>(), log_number,
                                         /*min_log_number_to_keep=*/log_number == 0 ? 1 : log_number,
                                         next_file_number);
}

std::shared_ptr<const Version> VersionSet::RegisterFile(const Version& base, const FileMetaData& f,
                                                        uint64_t log_number,
                                                        uint64_t next_file_number) {
  return RegisterFile(base, f, log_number, log_number, next_file_number);
}

std::shared_ptr<const Version> VersionSet::RegisterFile(const Version& base, const FileMetaData& f,
                                                        uint64_t log_number,
                                                        uint64_t min_log_number_to_keep,
                                                        uint64_t next_file_number) {
  std::vector<FileMetaData> files = base.files();
  files.push_back(f);
  return std::make_shared<const Version>(std::move(files), log_number, min_log_number_to_keep,
                                         next_file_number);
}

std::string VersionSet::MetaFileName(const std::string& dbname) {
  return MakeFileName(dbname, "META");
}

std::string VersionSet::MetaTempFileName(const std::string& dbname) {
  return MakeFileName(dbname, "META.tmp");
}

Status VersionSet::Recover(Env* env, const std::string& dbname, const Options& options,
                           const std::vector<std::string>& children,
                           std::shared_ptr<const Version>* out, RecoveryResult* info) {
  if (out == nullptr) return Status::InvalidArgument("VersionSet::Recover: null out");
  *out = nullptr;
  if (info != nullptr) *info = RecoveryResult();

  // 目录里出现的最大编号（.log / .sst 共享编号空间，§3.1）。
  uint64_t max_number = 0;
  bool has_sst = false;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseTableFileName(c, &n)) {
      has_sst = true;
      if (n > max_number) max_number = n;
    } else if (ParseLogFileName(c, &n)) {
      if (n > max_number) max_number = n;
    }
  }

  const std::string meta_path = MetaFileName(dbname);
  if (!env->FileExists(meta_path)) {
    // §10.9 规则 2 的安全阀：元数据缺失但目录里有已注册的 SSTable ⇒ 显式拒绝（把最坏情况从
    // 静默丢数据变成 kCorruption）。这里的判据是**语义**的（"存在 *.sst"），不绑文件名。
    if (has_sst) {
      return Status::Corruption("VersionSet::Recover: 版本元数据缺失但目录里存在 *.sst", dbname);
    }
    const uint64_t next = std::max<uint64_t>(1, max_number + 1);
    // log_number = 0 表示"未分配"，由调用方 §8.3 步骤 ⑨ 分配；min_log_number_to_keep = 1。
    *out = std::make_shared<const Version>(std::vector<FileMetaData>(), /*log_number=*/0,
                                           /*min_log_number_to_keep=*/1, next);
    return Status::OK();
  }

  std::string contents;
  Status s = ReadWholeFile(env, meta_path, &contents);
  if (!s.ok()) return s;
  VersionEdit edit;
  std::string why;
  if (!edit.DecodeFrom(Slice(contents), &why)) {
    return Status::Corruption("VersionSet::Recover: 元数据解析失败（不自动修复）", why);
  }
  if (edit.comparator_name() != options.comparator->Name()) {
    return Status::InvalidArgument("VersionSet::Recover: comparator 名称不符",
                                   edit.comparator_name() + " != " + options.comparator->Name());
  }

  RecoveryResult r;
  r.meta_present = true;
  SequenceNumber max_seq = 0;
  for (const FileMetaData& f : edit.files()) {
    const std::string path = TableFileName(dbname, f.number);
    if (!env->FileExists(path)) {
      return Status::Corruption("VersionSet::Recover: 元数据引用了不存在的 SSTable", path);
    }
    uint64_t size_on_disk = 0;
    s = env->GetFileSize(path, &size_on_disk);
    if (!s.ok()) return s;
    if (size_on_disk != f.file_size) {
      return Status::Corruption("VersionSet::Recover: 元数据 file_size 与磁盘不符",
                                path + " disk=" + std::to_string(size_on_disk) +
                                    " meta=" + std::to_string(f.file_size));
    }
    SequenceNumber actual = 0;
    s = ScanFileMaxSequence(env, options, path, f, &actual, &r.unknown_metaindex_entries);
    if (!s.ok()) return s;
    if (actual != f.max_sequence) {
      return Status::Corruption(
          "VersionSet::Recover: 元数据 max_sequence 与全量扫描不符",
          path + " scan=" + std::to_string(actual) + " meta=" + std::to_string(f.max_sequence));
    }
    if (f.max_sequence > max_seq) max_seq = f.max_sequence;
    ++r.sst_files_registered;
    r.sst_bytes_registered += f.file_size;
  }
  r.max_sequence_in_files = max_seq;

  const uint64_t next = std::max<uint64_t>(1, std::max(edit.next_file_number(), max_number + 1));
  *out = std::make_shared<const Version>(edit.files(), edit.log_number(),
                                         edit.min_log_number_to_keep(), next);
  if (info != nullptr) *info = r;
  return Status::OK();
}

Status VersionSet::Persist(Env* env, const std::string& dbname, const Options& options,
                           const Version& v, VersionEdit* edit) {
  VersionEdit e;
  e.SetComparatorName(options.comparator->Name());
  e.SetLogNumber(v.log_number());
  e.SetMinLogNumberToKeep(v.min_log_number_to_keep());
  e.SetNextFileNumber(v.next_file_number());
  for (const FileMetaData& f : v.files()) e.AddFile(f);

  std::string buf;
  if (!e.EncodeTo(&buf)) return Status::Corruption("VersionSet::Persist: 编码失败");

  const std::string tmp = MetaTempFileName(dbname);
  const std::string final_name = MetaFileName(dbname);
  WritableFile* raw = nullptr;
  Status s = env->NewWritableFile(tmp, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Append(Slice(buf));
    if (s.ok()) s = file->Sync();
    const Status close_status = file->Close();
    if (s.ok() && !close_status.ok()) s = close_status;
  }
  if (!s.ok()) {
    env->DeleteFile(tmp);   // 尽力而为；残留的 META.tmp 在下一次 Open 被清理
    return s;
  }
  s = env->RenameFile(tmp, final_name);
  if (!s.ok()) {
    env->DeleteFile(tmp);
    return s;
  }
  s = env->SyncDir(dbname);
  if (edit != nullptr) *edit = e;
  return s;
}

TableCache::TableCache(Env* env, std::string dbname, Options options, size_t capacity)
    : env_(env), dbname_(std::move(dbname)), options_(options), capacity_(capacity) {}

TableCache::~TableCache() = default;

Status TableCache::Open(uint64_t number, const std::string& smallest, const std::string& largest,
                        std::shared_ptr<const Table>* out, bool* opened) {
  *out = nullptr;
  if (opened != nullptr) *opened = false;

  {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = cache_.find(number);
    if (it != cache_.end()) {
      it->second.last_use = ++counter_;
      *out = it->second.table;
      return Status::OK();
    }
  }

  // 未命中：在缓存锁外做 IO（L19）。重复打开同一文件的竞态由插入时的二次检查兜底。
  std::shared_ptr<Table> table;
  const Status s = Table::Open(options_, env_, TableFileName(dbname_, number), &table, &smallest,
                               &largest);
  if (!s.ok()) return s;
  if (opened != nullptr) *opened = true;

  std::shared_ptr<const Table> evicted;
  {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = cache_.find(number);
    if (it != cache_.end()) {
      it->second.last_use = ++counter_;
      *out = it->second.table;
      return Status::OK();
    }
    Entry e;
    e.table = table;
    e.last_use = ++counter_;
    cache_.emplace(number, std::move(e));
    while (cache_.size() > capacity_) {
      auto victim = cache_.begin();
      for (auto it2 = cache_.begin(); it2 != cache_.end(); ++it2) {
        if (it2->second.last_use < victim->second.last_use) victim = it2;
      }
      evicted = victim->second.table;
      cache_.erase(victim);
    }
    *out = table;
  }
  // evicted 在此析构（可能在锁外 close 句柄）；不要把它带回调用方。
  return Status::OK();
}

Status TableCache::Get(const FileMetaData& f, const Slice& lookup_key, std::string* value,
                       TableGetResult* result, ReadStats* stats, bool* opened) {
  if (result == nullptr) return Status::InvalidArgument("TableCache::Get", "null result");
  std::shared_ptr<const Table> table;
  Status s = Open(f.number, f.smallest, f.largest, &table, opened);
  if (!s.ok()) return s;
  return table->GetEntry(lookup_key, value, result, stats);
}

std::unique_ptr<Iterator> TableCache::NewIterator(const FileMetaData& f, ReadStats* stats) {
  std::shared_ptr<const Table> table;
  Status s = Open(f.number, f.smallest, f.largest, &table, nullptr);
  if (!s.ok()) {
    // 与设计 §7.3 的「status() 取第一个非 OK」一致：返回一个只带错误的 child，
    // 让 DBIter/MergingIterator 能把错误报给调用方，而不是静默丢文件。
    return std::unique_ptr<Iterator>(NewStatusIterator(s));
  }
  return table->NewIterator(stats);
}

void TableCache::Evict(uint64_t number) {
  std::shared_ptr<const Table> dropped;
  {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = cache_.find(number);
    if (it == cache_.end()) return;
    dropped = it->second.table;
    cache_.erase(it);
  }
}

size_t TableCache::size() const {
  std::lock_guard<std::mutex> l(mu_);
  return cache_.size();
}

}  // namespace lsm
