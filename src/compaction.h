// src/compaction.h —— M4.2：选层/选文件/输入闭包/丢弃判据（docs/m4-design.md §3.5/§5.4/§5.5/§6.3）
//
// 本阶段（M4.2 第一批）落地**纯选择与判据**部分（无 IO、可直调、A 组确定性）；
// 执行体（Run/输出滚动/安装）与 compaction 线程见 docs/m4-prerequisites.md 的未做清单。
#ifndef LSM_COMPACTION_H_
#define LSM_COMPACTION_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "version_edit.h"
#include "version_set.h"

namespace lsm {

// §5.4 的策略枚举（complete definition；common.h 的 opaque 声明在这里落地）。
enum class PickStrategy : int { kRoundRobin = 0, kMinOverlap = 1 };

// §5.4：一次 compaction 的输入闭包（begin/end 是**闭区间** user key）。
struct CompactionInputs {
  int level = 0;                            // 上层层号；输出层 = level + 1
  std::vector<FileMetaData> inputs[2];      // inputs[0] = 上层；inputs[1] = 下层重叠集合
  std::string begin_user_key;
  std::string end_user_key;
};

// §5.7：compaction 统计（compaction 线程内累加；失败/丢弃必须计数）。
struct CompactionStats {
  uint64_t started = 0, completed = 0, failed = 0, aborted = 0;
  uint64_t rounds_by_level[kNumLevels] = {};
  uint64_t pick_round_robin = 0, pick_min_overlap = 0;
  uint64_t input_files = 0, output_files = 0;
  uint64_t bytes_read = 0, bytes_written = 0;
  uint64_t dropped_old_versions = 0, dropped_tombstones = 0;
  uint64_t install_rebase_retries = 0;
  uint64_t round_micros_p50 = 0, round_micros_max = 0;
  uint64_t level_full_events[kNumLevels] = {};
  std::string last_error;
};

class TableCache;

class Compaction {
 public:
  // §3.5：MaxBytesForLevel(l)；l == 0 未定义（返回 0，调用方不得用）。
  static uint64_t MaxBytesForLevel(const Options& o, int level);
  // §3.5：score(l)；l == 0 用文件数，l >= 1 用字节数。
  static double Score(const Version& v, const Options& o, int level);
  // §3.5：选层；< 0 表示本轮无 compaction。平手取层号小者。
  static int PickLevel(const Version& v, const Options& o);

  // §6.3：选文件 + 输入闭包（L0 传递闭包 X1；L1+ 单文件 + 下层重叠）。
  static bool PickInputs(const Version& v, int level, PickStrategy s, const Options& o,
                         CompactionInputs* out, std::string* why);

  // §5.5：单一真相源的丢弃判据（**析取**，A2/A26）。
  static bool ShouldDrop(ValueType type, SequenceNumber seq, SequenceNumber last_seq_for_key,
                         SequenceNumber smallest_snapshot, bool base_level_for_key);
  // §5.5/X6：l ∈ [level+2, kNumLevels) 是否有文件覆盖 user_key。
  static bool IsBaseLevelForKey(const Version& v, const Slice& user_key, int level);

  static std::string UserKeyOfInternal(const std::string& ikey);

  // §6.4：执行（锁外）：多路归并 → 只在 user key 变化处滚动输出（X2）→ 每个输出 write+fsync+rename
  // 之后才写进 *edit（I39）；edit 里先 DeleteFile(输入) 后 AddFile(输出层, 输出)（X7）。
  // 丢弃判据真正被调用（ShouldDrop + IsBaseLevelForKey，I40/I41/X6）。
  static Status Run(Env* env, TableCache* tc, const std::string& dbname, const CompactionInputs& in,
                    const Options& o, const Version& version, SequenceNumber smallest_snapshot,
                    const std::function<uint64_t()>& alloc_file_number,
                    const InternalKeyComparator& icmp, VersionEdit* edit, CompactionStats* stats,
                    std::string* why);
};

}  // namespace lsm

#endif  // LSM_COMPACTION_H_
