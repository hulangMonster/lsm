// src/compaction.cpp —— M4.2：选层/选文件/闭包/丢弃判据（docs/m4-design.md §3.5/§5.5/§6.3）
#include "compaction.h"

#include <algorithm>
#include <cstdint>
#include <string>

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

  // ---- 下层重叠集合 ----
  std::vector<FileMetaData> chosen_lower;
  for (const FileMetaData& lf : lower) {
    Slice llo, lhi;
    if (!ParseUserRange(lf, &llo, &lhi)) continue;
    if (RangesOverlap(Slice(begin), Slice(end), llo, lhi, uc)) chosen_lower.push_back(lf);
  }
  if (!chosen_lower.empty()) {
    std::string b2 = begin, e2 = end;
    for (const FileMetaData& lf : chosen_lower) {
      Slice llo, lhi;
      if (!ParseUserRange(lf, &llo, &lhi)) continue;
      if (uc->Compare(llo, Slice(b2)) < 0) b2 = llo.ToString();
      if (uc->Compare(lhi, Slice(e2)) > 0) e2 = lhi.ToString();
    }
    begin = b2;
    end = e2;
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

}  // namespace lsm
