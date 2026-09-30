// src/compaction.cpp —— M4.2：选层/选文件/闭包/丢弃判据（docs/m4-design.md §3.5/§5.5/§6.3）
#include "compaction.h"

#include "merging_iterator.h"
#include "sstable/table.h"
#include "sstable/table_builder.h"
#include "util/env.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lsm {
namespace {

bool ParseUserRange(const FileMetaData& f, Slice* small, Slice* large) {
  SequenceNumber s = 0;
  ValueType t = kTypeValue;
  return ParseInternalKey(Slice(f.smallest), small, &s, &t) &&
         ParseInternalKey(Slice(f.largest), large, &s, &t);
}

bool RangesOverlap(const Slice& a_lo, const Slice& a_hi, const Slice& b_lo, const Slice& b_hi,
                   const Comparator* uc) {
  return uc->Compare(a_lo, b_hi) <= 0 && uc->Compare(b_lo, a_hi) <= 0;
}

}  // namespace

std::string Compaction::UserKeyOfInternal(const std::string& ikey) {
  Slice uk;
  SequenceNumber s = 0;
  ValueType t = kTypeValue;
  if (!ParseInternalKey(Slice(ikey), &uk, &s, &t)) return std::string();
  return uk.ToString();
}

uint64_t Compaction::MaxBytesForLevel(const Options& o, int level) {
  if (level <= 0) return 0;
  uint64_t bytes = o.max_bytes_for_level_base;
  for (int l = 2; l <= level; ++l) {
    const uint64_t mult = static_cast<uint64_t>(o.max_bytes_for_level_multiplier);
    if (bytes > UINT64_MAX / mult) return UINT64_MAX;
    bytes *= mult;
  }
  return bytes;
}

double Compaction::Score(const Version& v, const Options& o, int level) {
  if (level == 0) {
    const double trigger = static_cast<double>(o.level0_file_num_compaction_trigger);
    return static_cast<double>(v.level_files(0).size()) / trigger;
  }
  const uint64_t cap = MaxBytesForLevel(o, level);
  if (cap == 0) return 0.0;
  return static_cast<double>(v.total_bytes(level)) / static_cast<double>(cap);
}

int Compaction::PickLevel(const Version& v, const Options& o) {
  int best = -1;
  double best_score = 0.0;
  for (int l = 0; l < kNumLevels; ++l) {
    const double s = Score(v, o, l);
    if (s < 1.0) continue;
    if (best < 0 || s > best_score) {   // 平手（s == best_score）取层号小者
      best = l;
      best_score = s;
    }
  }
  return best;
}

bool Compaction::PickInputs(const Version& v, int level, PickStrategy s, const Options& o,
                            CompactionInputs* out, std::string* why) {
  if (out == nullptr || level < 0 || level + 1 >= kNumLevels) {
    if (why != nullptr) *why = "PickInputs: level 越界";
    return false;
  }
  const Comparator* uc = BytewiseComparator();
  const std::vector<FileMetaData>& upper = v.level_files(level);
  const std::vector<FileMetaData>& lower = v.level_files(level + 1);
  if (upper.empty()) {
    if (why != nullptr) *why = "PickInputs: 上层为空";
    return false;
  }
  (void)o;

  // ---- 选种子 ----
  size_t seed = 0;
  if (s == PickStrategy::kRoundRobin) {
    // A14：L0 取**文件号最小**者（L0 已按 number 降序 ⇒ 取末尾）；L1+ 取层内第一个。
    seed = (level == 0) ? upper.size() - 1 : 0;
  } else {
    // A15：kMinOverlap 取与下层重叠字节最小者；并列取文件号最小。
    uint64_t best_overlap = UINT64_MAX;
    for (size_t i = 0; i < upper.size(); ++i) {
      Slice ulo, uhi;
      if (!ParseUserRange(upper[i], &ulo, &uhi)) continue;
      uint64_t overlap = 0;
      for (const FileMetaData& lf : lower) {
        Slice llo, lhi;
        if (!ParseUserRange(lf, &llo, &lhi)) continue;
        if (RangesOverlap(ulo, uhi, llo, lhi, uc)) overlap += lf.file_size;
      }
      const bool better = (overlap < best_overlap) ||
                          (overlap == best_overlap && upper[i].number < upper[seed].number);
      if (i == 0 || better) {
        best_overlap = overlap;
        seed = i;
      }
    }
  }

  Slice begin_slice, end_slice;
  if (!ParseUserRange(upper[seed], &begin_slice, &end_slice)) {
    if (why != nullptr) *why = "PickInputs: 种子文件的 key range 畸形";
    return false;
  }
  std::string begin = begin_slice.ToString();
  std::string end = end_slice.ToString();

  std::vector<FileMetaData> chosen_upper;
  if (level == 0) {
    // X1：按 key range 重叠的传递闭包（反复扫描直到不再变化）。
    std::vector<bool> taken(upper.size(), false);
    taken[seed] = true;
    chosen_upper.push_back(upper[seed]);
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t i = 0; i < upper.size(); ++i) {
        if (taken[i]) continue;
        Slice flo, fhi;
        if (!ParseUserRange(upper[i], &flo, &fhi)) continue;
        if (RangesOverlap(Slice(begin), Slice(end), flo, fhi, uc)) {
          taken[i] = true;
          chosen_upper.push_back(upper[i]);
          if (uc->Compare(flo, Slice(begin)) < 0) begin = flo.ToString();
          if (uc->Compare(fhi, Slice(end)) > 0) end = fhi.ToString();
          changed = true;
        }
      }
    }
  } else {
    chosen_upper.push_back(upper[seed]);
  }

  // ---- 下层重叠集合（闭包：区间扩张后必须重扫，直到不再变化）----
  std::vector<FileMetaData> chosen_lower;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const FileMetaData& lf : lower) {
      bool already = false;
      for (const FileMetaData& c : chosen_lower) {
        if (c.number == lf.number) { already = true; break; }
      }
      if (already) continue;
      Slice llo, lhi;
      if (!ParseUserRange(lf, &llo, &lhi)) continue;
      if (RangesOverlap(Slice(begin), Slice(end), llo, lhi, uc)) {
        chosen_lower.push_back(lf);
        if (uc->Compare(llo, Slice(begin)) < 0) begin = llo.ToString();
        if (uc->Compare(lhi, Slice(end)) > 0) end = lhi.ToString();
        changed = true;
      }
    }
    // L1+ 的上层是单文件；但区间扩张后仍可能需要把上层相邻文件纳入（保守：只扩下层）。
  }

  out->level = level;
  out->inputs[0] = std::move(chosen_upper);
  out->inputs[1] = std::move(chosen_lower);
  out->begin_user_key = begin;
  out->end_user_key = end;
  return true;
}

bool Compaction::ShouldDrop(ValueType type, SequenceNumber seq, SequenceNumber last_seq_for_key,
                            SequenceNumber smallest_snapshot, bool base_level_for_key) {
  const bool drop_old_version = (last_seq_for_key <= smallest_snapshot);
  const bool drop_tombstone = (type == kTypeDeletion) && (seq <= smallest_snapshot) &&
                              base_level_for_key;
  return drop_old_version || drop_tombstone;   // 析取（A2/A26）
}

bool Compaction::IsBaseLevelForKey(const Version& v, const Slice& user_key, int level) {
  const Comparator* uc = BytewiseComparator();
  for (int l = level + 2; l < kNumLevels; ++l) {   // X6：从 level+2 起步
    for (const FileMetaData& f : v.level_files(l)) {
      Slice lo, hi;
      if (!ParseUserRange(f, &lo, &hi)) continue;
      if (uc->Compare(lo, user_key) <= 0 && uc->Compare(user_key, hi) <= 0) return false;
    }
  }
  return true;
}

Status Compaction::Run(Env* env, TableCache* tc, const std::string& dbname, const CompactionInputs& in,
                       const Options& o, const Version& version, SequenceNumber smallest_snapshot,
                       const std::function<uint64_t()>& alloc_file_number,
                       const InternalKeyComparator& icmp, VersionEdit* edit,
                       CompactionStats* stats, std::string* why) {
  if (env == nullptr || tc == nullptr || edit == nullptr || !alloc_file_number) {
    return Status::InvalidArgument("Compaction::Run: null 参数");
  }
  std::vector<Iterator*> kids;
  uint64_t bytes_read = 0;
  for (int which = 0; which < 2; ++which) {
    for (const FileMetaData& f : in.inputs[which]) {
      bytes_read += f.file_size;
      kids.push_back(tc->NewIterator(f, nullptr).release());
      edit->DeleteFile(in.level + which, f.number);   // X7：先 DeleteFile 后 AddFile
    }
  }
  const int n = static_cast<int>(kids.size());
  Iterator** arr = new Iterator*[n];
  for (int i = 0; i < n; ++i) arr[i] = kids[static_cast<size_t>(i)];
  MergingIterator merged(&icmp, arr, n);

  std::string current_user_key;
  SequenceNumber last_seq_for_key = kMaxSequenceNumber;
  bool base_level_for_key = true;
  uint64_t dropped_old = 0, dropped_tomb = 0, bytes_written = 0, out_files = 0;

  std::unique_ptr<WritableFile> out_file;
  std::unique_ptr<TableBuilder> builder;
  uint64_t out_number = 0;
  const auto finish_output = [&]() -> Status {
    if (builder == nullptr) return Status::OK();
    Status s = builder->Finish();
    if (s.ok()) s = out_file->Sync();                 // I39：输出先 durable
    const Status close_status = out_file->Close();
    if (s.ok() && !close_status.ok()) s = close_status;
    if (s.ok()) {
      s = env->RenameFile(TempFileName(dbname, out_number), TableFileName(dbname, out_number));
    }
    if (s.ok()) s = env->SyncDir(dbname);
    if (!s.ok()) {
      env->DeleteFile(TempFileName(dbname, out_number));
      builder.reset();
      out_file.reset();
      return s;
    }
    FileMetaData meta;
    meta.number = out_number;
    meta.file_size = builder->FileSize();
    meta.max_sequence = builder->MaxSequence();
    meta.smallest = builder->smallest();
    meta.largest = builder->largest();
    bytes_written += meta.file_size;
    ++out_files;
    edit->AddFile(in.level + 1, meta);                // 已 durable 才注册（I39）
    builder.reset();
    out_file.reset();
    return Status::OK();
  };

  Status loop_status = Status::OK();
  for (merged.SeekToFirst(); merged.Valid(); merged.Next()) {
    Slice uk;
    SequenceNumber seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(merged.key(), &uk, &seq, &type)) {
      loop_status = Status::Corruption("Compaction::Run: 归并输出存在畸形 internal key");
      break;
    }
    const std::string uk_str = uk.ToString();
    if (uk_str != current_user_key) {
      if (builder != nullptr && builder->FileSize() >= o.max_file_size) {
        const Status s = finish_output();             // X2：只在 user key 变化处切分
        if (!s.ok()) { loop_status = s; break; }
      }
      current_user_key = uk_str;
      last_seq_for_key = kMaxSequenceNumber;
      base_level_for_key = IsBaseLevelForKey(version, Slice(current_user_key), in.level);
    }
    const bool drop =
        ShouldDrop(type, seq, last_seq_for_key, smallest_snapshot, base_level_for_key);
    if (drop) {
      if (type == kTypeDeletion) {
        ++dropped_tomb;
      } else {
        ++dropped_old;
      }
    } else {
      if (builder == nullptr) {
        out_number = alloc_file_number();
        const std::string tmp = TempFileName(dbname, out_number);
        WritableFile* raw = nullptr;
        const Status s = env->NewWritableFile(tmp, &raw);
        if (!s.ok()) { loop_status = s; break; }
        out_file.reset(raw);
        builder.reset(new TableBuilder(o, out_file.get()));
      }
      const Status s = builder->Add(merged.key(), merged.value());
      if (!s.ok()) { loop_status = s; break; }
    }
    last_seq_for_key = seq;                            // X3：判定之后才更新
  }
  if (loop_status.ok()) loop_status = merged.status();
  if (loop_status.ok()) loop_status = finish_output();
  if (!loop_status.ok()) {
    if (why != nullptr) *why = loop_status.ToString();
    return loop_status;
  }

  if (stats != nullptr) {
    stats->input_files += in.inputs[0].size() + in.inputs[1].size();
    stats->output_files += out_files;
    stats->bytes_read += bytes_read;
    stats->bytes_written += bytes_written;
    stats->dropped_old_versions += dropped_old;
    stats->dropped_tombstones += dropped_tomb;
  }
  return Status::OK();
}

}  // namespace lsm
