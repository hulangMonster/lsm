// src/version_set.cpp —— M3.2：文件命名、不可变 Version、内存 TableCache。
#include "version_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "merging_iterator.h"

namespace lsm {
namespace {

std::string MakeFileName(const std::string& dbname, const std::string& suffix) {
  return dbname.empty() ? suffix : dbname + "/" + suffix;
}

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
                                         /*min_log_number_to_keep=*/log_number, next_file_number);
}

std::shared_ptr<const Version> VersionSet::RegisterFile(const Version& base, const FileMetaData& f,
                                                        uint64_t log_number,
                                                        uint64_t next_file_number) {
  std::vector<FileMetaData> files = base.files();
  files.push_back(f);
  return std::make_shared<const Version>(std::move(files), log_number, log_number,
                                         next_file_number);
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
