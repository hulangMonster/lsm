// src/version_set.cpp —— M3.2/M3.3：文件命名、不可变 Version、内存 TableCache、META 持久化。
//                     M4.1：层级 Version、ValidateLevelLayout、MANIFEST/CURRENT 回放与迁移、ApplyEdit。
#include "version_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <utility>

#include "filename.h"
#include "util/coding.h"
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
// MANIFEST 是追加日志，回放要整读；给 256 MiB 防御上界（正常由 roll 阈值控制）。
constexpr size_t kMaxManifestFileBytes = 256u * 1024 * 1024;

Status ReadWholeFileLimit(Env* env, const std::string& path, std::string* out, size_t limit) {
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
    if (out->size() > limit) {
      return Status::Corruption("ReadWholeFile: 文件超过上限", path);
    }
  }
  return Status::OK();
}

Status ReadWholeFile(Env* env, const std::string& path, std::string* out) {
  return ReadWholeFileLimit(env, path, out, kMaxMetaFileBytes);
}

// M3-A38（§8.2 纪律 3）：打开已注册的 .sst 全量扫一遍，独立算出真实 max_sequence。
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

bool UserKeyOf(const std::string& ikey, Slice* uk) {
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  return ParseInternalKey(Slice(ikey), uk, &seq, &type);
}

bool KeysValid(const FileMetaData& f, const InternalKeyComparator& icmp, std::string* why) {
  Slice su, lu;
  if (!UserKeyOf(f.smallest, &su) || !UserKeyOf(f.largest, &lu)) {
    if (why != nullptr) *why = "文件 " + std::to_string(f.number) + " 的 smallest/largest 不是合法 internal key";
    return false;
  }
  if (su.empty() || lu.empty()) {
    if (why != nullptr) *why = "文件 " + std::to_string(f.number) + " 的 user key 为空";
    return false;
  }
  if (icmp.Compare(Slice(f.smallest), Slice(f.largest)) > 0) {
    if (why != nullptr) *why = "文件 " + std::to_string(f.number) + " 的 smallest > largest";
    return false;
  }
  return true;
}

// 中间损坏判定：失败点之后是否还藏着一个完整可解析的 record（§8.4）。
bool HasValidRecordAfter(const std::string& buf, size_t from) {
  const Slice s(buf);
  for (size_t j = from + 1; j + 5 <= buf.size(); ++j) {
    size_t off = j;
    VersionEdit tmp;
    std::string why;
    if (ReadManifestRecord(s, &off, &tmp, &why) == ManifestReadStatus::kOk) return true;
    if (j - from > 4096) break;   // 有界扫描：正常 record 就在失败点附近
  }
  return false;
}

// 把一条 edit 应用到 level 视图（X7：先 Delete 后 Add）。失败返回 false + why。
bool ApplyEditToLevels(std::vector<std::vector<FileMetaData>>* levels, const VersionEdit& edit,
                       std::string* why) {
  for (const auto& d : edit.deleted_files()) {
    const int level = d.first;
    if (level < 0 || level >= kNumLevels) {
      if (why != nullptr) *why = "DeleteFile level 越界";
      return false;
    }
    auto& fs = (*levels)[static_cast<size_t>(level)];
    auto it = std::find_if(fs.begin(), fs.end(),
                           [&](const FileMetaData& f) { return f.number == d.second; });
    if (it == fs.end()) {
      if (why != nullptr) {
        *why = "DeleteFile 目标不存在：level=" + std::to_string(level) +
               " number=" + std::to_string(d.second);
      }
      return false;
    }
    fs.erase(it);
  }
  for (const auto& a : edit.added_files()) {
    const int level = a.first;
    if (level < 0 || level >= kNumLevels) {
      if (why != nullptr) *why = "AddFile level 越界";
      return false;
    }
    for (int l = 0; l < kNumLevels; ++l) {
      for (const FileMetaData& f : (*levels)[static_cast<size_t>(l)]) {
        if (f.number == a.second.number) {
          if (why != nullptr) {
            *why = "AddFile number 重复：" + std::to_string(a.second.number);
          }
          return false;
        }
      }
    }
    (*levels)[static_cast<size_t>(level)].push_back(a.second);
  }
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
  if (ParseTempFileName(fname, number)) return false;
  return ParseNumberSuffix(fname, ".sst", number);
}

bool ParseTempFileName(const std::string& fname, uint64_t* number) {
  return ParseNumberSuffix(fname, ".sst.tmp", number);
}

// ---------------------------------------------------------------------------
// Version（M4：层级视图；files() == level_files(0)）
// ---------------------------------------------------------------------------

Version::Version(std::vector<FileMetaData> files, uint64_t log_number,
                 uint64_t min_log_number_to_keep, uint64_t next_file_number)
    : log_number_(log_number),
      min_log_number_to_keep_(min_log_number_to_keep),
      next_file_number_(next_file_number) {
  level_files_.resize(kNumLevels);
  level_files_[0] = std::move(files);
  Normalize();
}

Version::Version(std::vector<std::vector<FileMetaData>> levels, uint64_t log_number,
                 uint64_t min_log_number_to_keep, uint64_t next_file_number)
    : level_files_(std::move(levels)),
      log_number_(log_number),
      min_log_number_to_keep_(min_log_number_to_keep),
      next_file_number_(next_file_number) {
  Normalize();
}

void Version::Normalize() {
  level_files_.resize(kNumLevels);
  std::sort(level_files_[0].begin(), level_files_[0].end(),
            [](const FileMetaData& a, const FileMetaData& b) { return a.number > b.number; });
  const InternalKeyComparator icmp(BytewiseComparator());
  for (int l = 1; l < kNumLevels; ++l) {
    std::sort(level_files_[static_cast<size_t>(l)].begin(),
              level_files_[static_cast<size_t>(l)].end(),
              [&icmp](const FileMetaData& a, const FileMetaData& b) {
                Slice au, bu;
                if (UserKeyOf(a.smallest, &au) && UserKeyOf(b.smallest, &bu)) {
                  const int c = au.compare(bu);
                  if (c != 0) return c < 0;
                }
                return icmp.Compare(Slice(a.smallest), Slice(b.smallest)) < 0;
              });
  }
  all_files_.clear();
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FileMetaData& f : level_files_[static_cast<size_t>(l)]) all_files_.push_back(f);
  }
}

const std::vector<FileMetaData>& Version::level_files(int level) const {
  static const std::vector<FileMetaData> kEmpty;
  if (level < 0 || level >= kNumLevels) return kEmpty;
  return level_files_[static_cast<size_t>(level)];
}

uint64_t Version::total_bytes(int level) const {
  uint64_t sum = 0;
  for (const FileMetaData& f : level_files(level)) sum += f.file_size;
  return sum;
}

SequenceNumber Version::MaxSequenceInFiles() const {
  SequenceNumber m = 0;
  for (const FileMetaData& f : all_files_) {
    if (f.max_sequence > m) m = f.max_sequence;
  }
  return m;
}

bool ValidateLevelLayout(const std::vector<std::vector<FileMetaData>>& levels,
                         const InternalKeyComparator& icmp, std::string* why) {
  if (levels.size() > static_cast<size_t>(kNumLevels)) {
    if (why != nullptr) *why = "层数超过 kNumLevels";
    return false;
  }
  std::map<uint64_t, int> seen;
  for (size_t l = 0; l < levels.size(); ++l) {
    const auto& fs = levels[l];
    for (const FileMetaData& f : fs) {
      if (f.number == 0) {
        if (why != nullptr) *why = "level " + std::to_string(l) + " 存在 number == 0 的文件";
        return false;
      }
      if (f.file_size == 0) {
        if (why != nullptr) *why = "文件 " + std::to_string(f.number) + " 的 file_size == 0";
        return false;
      }
      if (!KeysValid(f, icmp, why)) return false;
      auto ins = seen.emplace(f.number, static_cast<int>(l));
      if (!ins.second) {
        if (why != nullptr) {
          *why = "文件号重复：" + std::to_string(f.number) + "（level " +
                 std::to_string(ins.first->second) + " 与 level " + std::to_string(l) + "）";
        }
        return false;
      }
    }
    if (l == 0) {
      for (size_t i = 1; i < fs.size(); ++i) {
        if (!(fs[i - 1].number > fs[i].number)) {
          if (why != nullptr) {
            *why = "L0 未按文件号降序：文件 " + std::to_string(fs[i - 1].number) + " 与 " +
                   std::to_string(fs[i].number);
          }
          return false;
        }
      }
      continue;
    }
    for (size_t i = 1; i < fs.size(); ++i) {
      const FileMetaData& prev = fs[i - 1];
      const FileMetaData& cur = fs[i];
      Slice pu, cu;
      const bool pu_ok = UserKeyOf(prev.largest, &pu);
      const bool cu_ok = UserKeyOf(cur.smallest, &cu);
      if (!pu_ok || !cu_ok) {
        if (why != nullptr) *why = "层内 internal key 畸形";
        return false;
      }
      if (!(pu.compare(cu) < 0)) {
        if (why != nullptr) {
          *why = "level " + std::to_string(l) + " 层内 user key 不严格递增：文件 " +
                 std::to_string(prev.number) + " 与 " + std::to_string(cur.number) +
                 " 共享 user key";
        }
        return false;
      }
      if (icmp.Compare(Slice(prev.largest), Slice(cur.smallest)) >= 0) {
        if (why != nullptr) {
          *why = "level " + std::to_string(l) + " 层内区间重叠：文件 " +
                 std::to_string(prev.number) + " 与 " + std::to_string(cur.number);
        }
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// VersionSet：M3 的 META 路径 + M4 的 MANIFEST/CURRENT 路径
// ---------------------------------------------------------------------------

std::shared_ptr<const Version> VersionSet::Empty(uint64_t log_number, uint64_t next_file_number) {
  return std::make_shared<const Version>(std::vector<FileMetaData>(), log_number,
                                         log_number == 0 ? 1 : log_number, next_file_number);
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
    if (has_sst) {
      return Status::Corruption("VersionSet::Recover: 版本元数据缺失但目录里存在 *.sst", dbname);
    }
    const uint64_t next = std::max<uint64_t>(1, max_number + 1);
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
  {
    const Version probe(edit.files(), edit.log_number(), edit.min_log_number_to_keep(),
                        edit.next_file_number());
    RecoveryResult vr;
    s = VerifyRegisteredFiles(env, dbname, options, probe, &vr);
    if (!s.ok()) return s;
    r.sst_files_registered = vr.sst_files_registered;
    r.sst_bytes_registered = vr.sst_bytes_registered;
    r.max_sequence_in_files = vr.max_sequence_in_files;
    r.unknown_metaindex_entries = vr.unknown_metaindex_entries;
  }

  const uint64_t next = std::max<uint64_t>(1, std::max(edit.next_file_number(), max_number + 1));
  *out = std::make_shared<const Version>(edit.files(), edit.log_number(),
                                         edit.min_log_number_to_keep(), next);
  if (info != nullptr) *info = r;
  return Status::OK();
}

Status VersionSet::VerifyRegisteredFiles(Env* env, const std::string& dbname,
                                         const Options& options, const Version& v,
                                         RecoveryResult* info) {
  if (info != nullptr) *info = RecoveryResult();
  RecoveryResult r;
  SequenceNumber max_seq = 0;
  for (const FileMetaData& f : v.AllFiles()) {
    const std::string path = TableFileName(dbname, f.number);
    if (!env->FileExists(path)) {
      return Status::Corruption("VersionSet::VerifyRegisteredFiles: 元数据引用了不存在的 SSTable",
                                path);
    }
    uint64_t size_on_disk = 0;
    Status s = env->GetFileSize(path, &size_on_disk);
    if (!s.ok()) return s;
    if (size_on_disk != f.file_size) {
      return Status::Corruption("VersionSet::VerifyRegisteredFiles: file_size 与磁盘不符",
                                path + " disk=" + std::to_string(size_on_disk) +
                                    " meta=" + std::to_string(f.file_size));
    }
    SequenceNumber actual = 0;
    s = ScanFileMaxSequence(env, options, path, f, &actual, &r.unknown_metaindex_entries);
    if (!s.ok()) return s;
    if (actual != f.max_sequence) {
      return Status::Corruption("VersionSet::VerifyRegisteredFiles: max_sequence 与全量扫描不符",
                                path + " scan=" + std::to_string(actual) +
                                    " meta=" + std::to_string(f.max_sequence));
    }
    if (f.max_sequence > max_seq) max_seq = f.max_sequence;
    ++r.sst_files_registered;
    r.sst_bytes_registered += f.file_size;
  }
  r.max_sequence_in_files = max_seq;
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
    env->DeleteFile(tmp);
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

// ---------------------------------------------------------------------------
// M4.1：MANIFEST / CURRENT
// ---------------------------------------------------------------------------

uint64_t VersionSet::DirectoryMaxNumber(const std::vector<std::string>& children) {
  uint64_t max_number = 0;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseTableFileName(c, &n) || ParseTempFileName(c, &n) || ParseLogFileName(c, &n) ||
        ParseManifestFileName(c, &n) || ParseManifestTempFileName(c, &n)) {
      if (n > max_number) max_number = n;
    }
  }
  return max_number;
}

Status VersionSet::ReadCurrent(Env* env, const std::string& dbname, uint64_t* number) {
  std::string contents;
  Status s = ReadWholeFileLimit(env, CurrentFileName(dbname), &contents, 64);
  if (!s.ok()) return s;
  if (!ParseCurrentContents(contents, number)) {
    return Status::Corruption("VersionSet::ReadCurrent: CURRENT 内容非法（要求 ^[0-9]{1,20}\\n$）",
                              contents);
  }
  return Status::OK();
}

Status VersionSet::WriteCurrentAtomic(Env* env, const std::string& dbname, uint64_t number) {
  const std::string tmp = CurrentTempFileName(dbname);
  const std::string final_name = CurrentFileName(dbname);
  std::string contents = std::to_string(number) + "\n";
  WritableFile* raw = nullptr;
  Status s = env->NewWritableFile(tmp, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Append(Slice(contents));
    if (s.ok()) s = file->Sync();
    const Status close_status = file->Close();
    if (s.ok() && !close_status.ok()) s = close_status;
  }
  if (!s.ok()) {
    env->DeleteFile(tmp);
    return s;
  }
  s = env->RenameFile(tmp, final_name);
  if (!s.ok()) {
    env->DeleteFile(tmp);
    return s;
  }
  return env->SyncDir(dbname);
}

VersionEdit VersionSet::MakeSnapshotEdit(const Version& v, const Options& options) {
  VersionEdit e;
  e.SetComparatorName(options.comparator->Name());
  e.SetLogNumber(v.log_number());
  e.SetMinLogNumberToKeep(v.min_log_number_to_keep());
  e.SetNextFileNumber(v.next_file_number());
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FileMetaData& f : v.level_files(l)) e.AddFile(l, f);
  }
  return e;
}

Status VersionSet::WriteSnapshotManifest(Env* env, const std::string& dbname, uint64_t number,
                                         const Version& v, const Options& options) {
  const VersionEdit e = MakeSnapshotEdit(v, options);
  std::string record;
  if (!EncodeManifestRecord(e, &record)) {
    return Status::Corruption("VersionSet::WriteSnapshotManifest: 编码失败");
  }
  const std::string tmp = ManifestTempFileName(dbname, number);
  const std::string final_name = ManifestFileName(dbname, number);
  WritableFile* raw = nullptr;
  Status s = env->NewWritableFile(tmp, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Append(Slice(record));
    if (s.ok()) s = file->Sync();
    const Status close_status = file->Close();
    if (s.ok() && !close_status.ok()) s = close_status;
  }
  if (!s.ok()) {
    env->DeleteFile(tmp);
    return s;
  }
  s = env->RenameFile(tmp, final_name);
  if (!s.ok()) {
    env->DeleteFile(tmp);
    return s;
  }
  return env->SyncDir(dbname);
}

Status VersionSet::AppendEdit(Env* env, const std::string& dbname, uint64_t manifest_number,
                              const VersionEdit& edit, uint64_t* manifest_bytes) {
  std::string record;
  if (!EncodeManifestRecord(edit, &record)) {
    return Status::Corruption("VersionSet::AppendEdit: 编码失败");
  }
  const std::string path = ManifestFileName(dbname, manifest_number);
  WritableFile* raw = nullptr;
  Status s = env->NewAppendableFile(path, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Append(Slice(record));
    if (s.ok()) s = file->Sync();
    const Status close_status = file->Close();
    if (s.ok() && !close_status.ok()) s = close_status;
  }
  if (!s.ok()) return s;
  if (manifest_bytes != nullptr) {
    uint64_t size = 0;
    s = env->GetFileSize(path, &size);
    if (!s.ok()) return s;
    *manifest_bytes = size;
  }
  return Status::OK();
}

bool VersionSet::ApplyEdit(const Version& base, const VersionEdit& edit,
                           std::shared_ptr<const Version>* out, std::string* why) {
  if (out == nullptr) return false;
  *out = nullptr;
  std::vector<std::vector<FileMetaData>> levels(kNumLevels);
  for (int l = 0; l < kNumLevels; ++l) levels[static_cast<size_t>(l)] = base.level_files(l);
  if (!ApplyEditToLevels(&levels, edit, why)) return false;
  const uint64_t log = edit.has_log_number() ? edit.log_number() : base.log_number();
  const uint64_t min_log = edit.has_min_log_number_to_keep() ? edit.min_log_number_to_keep()
                                                             : base.min_log_number_to_keep();
  const uint64_t next = edit.has_next_file_number() ? edit.next_file_number()
                                                    : base.next_file_number();
  const InternalKeyComparator icmp(BytewiseComparator());
  std::string vwhy;
  auto candidate = std::make_shared<const Version>(levels, log, min_log, next);
  if (!ValidateLevelLayout(candidate->level_files_all(), icmp, &vwhy)) {
    if (why != nullptr) *why = "安装期层级校验失败：" + vwhy;
    return false;
  }
  *out = candidate;
  return true;
}

Status VersionSet::RecoverManifest(Env* env, const std::string& dbname, const Options& options,
                                   std::shared_ptr<const Version>* out,
                                   ManifestReplayResult* info) {
  if (out == nullptr) return Status::InvalidArgument("VersionSet::RecoverManifest: null out");
  *out = nullptr;
  if (info != nullptr) *info = ManifestReplayResult();

  std::vector<std::string> children;
  Status s = env->GetChildren(dbname, &children);
  if (!s.ok()) return s;
  const uint64_t max_dir = DirectoryMaxNumber(children);
  if (info != nullptr) info->max_directory_number = max_dir;

  // ① CURRENT 存在 ⇒ 回放 MANIFEST（权威源）。
  if (env->FileExists(CurrentFileName(dbname))) {
    uint64_t number = 0;
    s = ReadCurrent(env, dbname, &number);
    if (!s.ok()) return s;
    const std::string path = ManifestFileName(dbname, number);
    if (!env->FileExists(path)) {
      return Status::Corruption("VersionSet::RecoverManifest: CURRENT 指向不存在的 MANIFEST", path);
    }
    std::string buf;
    s = ReadWholeFileLimit(env, path, &buf, kMaxManifestFileBytes);
    if (!s.ok()) return s;

    std::vector<std::vector<FileMetaData>> levels(kNumLevels);
    uint64_t log_number = 0, min_log = 1, next_file = 1;
    size_t offset = 0;
    uint64_t edits = 0;
    uint64_t truncated = 0;
    std::string why;
    bool stop = false;
    while (!stop && offset < buf.size()) {
      VersionEdit edit;
      const size_t before = offset;
      const ManifestReadStatus st = ReadManifestRecord(Slice(buf), &offset, &edit, &why);
      if (st == ManifestReadStatus::kOk) {
        if (edit.has_comparator() && edit.comparator_name() != options.comparator->Name()) {
          return Status::InvalidArgument("VersionSet::RecoverManifest: comparator 名称不符",
                                         edit.comparator_name() + " != " +
                                             options.comparator->Name());
        }
        std::string awhy;
        if (!ApplyEditToLevels(&levels, edit, &awhy)) {
          return Status::Corruption("VersionSet::RecoverManifest: 回放应用失败", awhy);
        }
        if (edit.has_log_number()) log_number = edit.log_number();
        if (edit.has_min_log_number_to_keep()) min_log = edit.min_log_number_to_keep();
        if (edit.has_next_file_number()) next_file = edit.next_file_number();
        ++edits;
        continue;
      }
      if (st == ManifestReadStatus::kNotSupported) {
        if (info != nullptr) info->unknown_record_types += 1;
        return Status::NotSupported("VersionSet::RecoverManifest: 未知 record type", why);
      }
      if (st == ManifestReadStatus::kTailResidue) {
        offset = before;
        stop = true;
        break;
      }
      // kCorruption：长度越界/CRC 不符/payload 非法。
      const bool length_insane =
          (buf.size() - before >= 5) &&
          (DecodeFixed32(buf.data() + before) == 0 ||
           DecodeFixed32(buf.data() + before) > kMaxManifestRecordBytes);
      if (length_insane || HasValidRecordAfter(buf, before)) {
        return Status::Corruption("VersionSet::RecoverManifest: MANIFEST 中间损坏（不截断）", why);
      }
      offset = before;
      stop = true;
      break;
    }
    if (stop) truncated = buf.size() - offset;
    (void)truncated;

    // 语义收尾：层级校验 + 权威 next_file_number（X5：必须把 MANIFEST 编号计入）。
    const InternalKeyComparator icmp(BytewiseComparator());
    auto candidate = std::make_shared<const Version>(levels, log_number, min_log == 0 ? 1 : min_log,
                                                     next_file);
    std::string vwhy;
    if (!ValidateLevelLayout(candidate->level_files_all(), icmp, &vwhy)) {
      return Status::Corruption("VersionSet::RecoverManifest: 层内布局非法（不自动修复）", vwhy);
    }
    const uint64_t auth_next =
        std::max<uint64_t>(1, std::max(std::max(next_file, number + 1), max_dir + 1));
    *out = std::make_shared<const Version>(levels, log_number, min_log == 0 ? 1 : min_log, auth_next);

    // 尾部残骸：截断到 last_good_end（否则下一次追加会与残骸拼成不可能的 record）。
    uint64_t tail_bytes = 0;
    if (offset < buf.size()) {
      tail_bytes = buf.size() - offset;
      s = env->Truncate(path, offset);
      if (!s.ok()) return s;
      WritableFile* tf = nullptr;
      s = env->NewAppendableFile(path, &tf);
      if (!s.ok()) return s;
      {
        std::unique_ptr<WritableFile> guard(tf);
        s = guard->Sync();
        if (s.ok()) s = guard->Close();
      }
      if (!s.ok()) return s;
    }
    if (info != nullptr) {
      info->manifest_present = true;
      info->manifest_number = number;
      info->manifest_bytes = offset;
      info->edits_replayed = edits;
      info->tail_truncated_bytes = tail_bytes;
      info->next_version_number = edits;
      info->max_directory_number = std::max(max_dir, number);
    }
    return Status::OK();
  }

  // ② META 存在 ⇒ 兼容读入一次 + 一次性迁移（§3.6）。
  if (env->FileExists(MetaFileName(dbname))) {
    std::shared_ptr<const Version> legacy;
    RecoveryResult rr;
    s = Recover(env, dbname, options, children, &legacy, &rr);
    if (!s.ok()) return s;
    const uint64_t n = std::max<uint64_t>(1, max_dir + 1);
    s = WriteSnapshotManifest(env, dbname, n, *legacy, options);
    if (!s.ok()) return s;
    s = WriteCurrentAtomic(env, dbname, n);
    if (!s.ok()) return s;
    uint64_t meta_delete_failed = 0;
    if (env->FileExists(MetaFileName(dbname))) {
      if (!env->DeleteFile(MetaFileName(dbname)).ok()) ++meta_delete_failed;
    }
    if (env->FileExists(MetaTempFileName(dbname))) {
      if (!env->DeleteFile(MetaTempFileName(dbname)).ok()) ++meta_delete_failed;
    }
    uint64_t bytes = 0;
    env->GetFileSize(ManifestFileName(dbname, n), &bytes);
    const uint64_t auth_next =
        std::max<uint64_t>(1, std::max(std::max(legacy->next_file_number(), n + 1), max_dir + 1));
    *out = std::make_shared<const Version>(legacy->level_files_all(), legacy->log_number(),
                                           legacy->min_log_number_to_keep(), auth_next);
    if (info != nullptr) {
      info->manifest_present = true;
      info->migrated_from_meta = true;
      info->manifest_number = n;
      info->manifest_bytes = bytes;
      info->edits_replayed = 1;
      info->next_version_number = 1;
      info->max_directory_number = std::max(max_dir, n);
      info->meta_delete_failed = meta_delete_failed;
    }
    return Status::OK();
  }

  // ③ 空库（但 CURRENT.tmp 残留 ⇒ 安全阀拒绝；目录里有 *.sst/MANIFEST-* ⇒ 元数据丢失）。
  if (env->FileExists(CurrentTempFileName(dbname))) {
    return Status::Corruption(
        "VersionSet::RecoverManifest: CURRENT 缺失但 CURRENT.tmp 存在（不得自动修复）", dbname);
  }
  bool nonempty = false;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseTableFileName(c, &n) || ParseManifestFileName(c, &n)) {
      nonempty = true;
      break;
    }
  }
  if (nonempty) {
    return Status::Corruption(
        "VersionSet::RecoverManifest: 元数据缺失但目录非空（*.sst 或 MANIFEST-*）", dbname);
  }
  *out = std::make_shared<const Version>(std::vector<FileMetaData>(), /*log_number=*/0,
                                         /*min_log_number_to_keep=*/1,
                                         std::max<uint64_t>(1, max_dir + 1));
  return Status::OK();
}

// ---------------------------------------------------------------------------
// TableCache（不改）
// ---------------------------------------------------------------------------

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
