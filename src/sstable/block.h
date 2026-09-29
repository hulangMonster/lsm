// src/sstable/block.h —— 数据块/索引块的构建、读取与结构校验（M3.1 骨架）
//
// 契约来源：docs/m3-design.md §3.2（数据块布局与 restart 语义）、§3.3（索引块）、§3.6（块头/CRC）
//   不变量 I24（restart_offset 严格单调且指向 entry 起始）与 I25（类型/长度冗余自检）。
//
// `#2` 阶段只给声明；`#3`（M3.1）给实现。语义要点（实现不得走样）：
//   * 空块 payload **恰 8 字节**（restart_offset[0]=0 + restart_count=1）（§3.2）。
//   * 组内第一条 entry 的 shared 恒为 0；**组间不共享前缀**（新组首条 shared 恒 0）（§3.2）。
//   * 解码任何 entry 前必须**先校验再读**：shared <= 上一条 key 长度、
//     shared+non_shared <= 剩余字节、vlen <= 剩余字节、restart 点 shared 必须为 0（§3.2）。
//   * `block_size` 是**目标值不是硬上限**：判据是"加上这一条之后是否超过"，超了先封块（§3.2）。
#ifndef LSM_SSTABLE_BLOCK_H_
#define LSM_SSTABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "sstable/format.h"

namespace lsm {

// ---- §3.2 写入器 ----
class BlockBuilder {
 public:
  // restart_interval：数据块用 kRestartInterval(16)，索引块用 kIndexRestartInterval(1)。
  explicit BlockBuilder(int restart_interval);

  // 追加一条 entry。要求 key 相对上一条（**同一 restart 组内**）的公共前缀由内部计算。
  void Add(const Slice& key, const Slice& value);

  void Reset();
  bool empty() const;
  size_t NumRestarts() const;
  size_t NumEntries() const;

  // payload := entry* ‖ restart_offset[uint32 LE]* ‖ restart_count(uint32 LE)
  // 反复调用返回同一份内容（幂等）；空 builder 返回的 payload 恰 8 字节。
  Slice Finish();

  // 与 Finish().size() 必须相等（A01 断言）。
  size_t CurrentSizeEstimate() const;

  // §5.2/§3.2：**唯一的切块判据** —— 「再加上这一条之后」的 payload 字节数
  // （含本条 entry 的 varint 前缀、value，以及可能新增的 restart_offset 与 count 字段）。
  // TableBuilder::Add 只在它 > block_size 时封块；实现不得在别处复算该口径。
  size_t EstimatedSizeAfter(const Slice& key, const Slice& value) const;

 private:
  int restart_interval_;
  std::string buffer_;               // entry 区
  std::vector<uint32_t> restarts_;   // restart 点（entry 起始偏移），空块恒为 {0}
  std::string last_key_;             // 上一条 key（**已完整复原**，不是 delta）
  int counter_;                      // 本组已写几条
  size_t num_entries_;               // 已 Add 的 entry 总数（估计口径与 NumRestarts 无关）
  bool finished_;
  std::string finished_payload_;     // Finish() 的结果（幂等）
};

// ---- §3.2 只读访问器 ----
class BlockReader {
 public:
  // 解析 payload（**不含** header 与 crc）。任何结构违规 ⇒ kCorruption（不得越界读）。
  // icmp：非空时块内 Seek 一律走 InternalKeyComparator（design §5.3 的二分规则）；
  // 为空时退化为整条 key 的字节序（仅用于 M3.1 的纯格式层用例与不含 internal key 的块）。
  static Status Open(const Slice& payload, std::unique_ptr<BlockReader>* out,
                     const InternalKeyComparator* icmp = nullptr);

  Status SeekToFirst();
  Status SeekToLast();
  // 找到第一个 key >= target 的 entry；都不满足则 Valid()==false 且 status()==kOk（越过末尾）。
  Status Seek(const Slice& target);
  Status Next();
  Status Prev();

  bool Valid() const;
  Slice key() const;     // 完整 key（组内已按 shared/delta 复原）
  Slice value() const;
  Status status() const;

  size_t NumRestarts() const;
  uint32_t RestartOffset(size_t i) const;

 private:
  // 解出 off 处的一条 entry。group_start==true 表示该处是 restart 点（previous key 视为空，
  // 且要求 shared == 0）。prev_key 仅在 group_start==false 时使用。任何违规 ⇒ kCorruption。
  Status DecodeEntry(size_t off, bool group_start, const std::string& prev_key, std::string* key,
                     Slice* value, size_t* next) const;
  bool IsRestartOffset(size_t off) const;
  // 从 target 所在 restart 组起点顺序解码到 target（组内 prefix 状态由此恢复）。
  Status DecodeFromGroupStart(size_t target, std::string* key, Slice* value) const;
  // 解出 off 处 entry 并置为当前位置（off 必须是合法 entry 起点）。
  Status SetTo(size_t off);
  // cur 之前那条 entry 的起始偏移；cur 之前没有 entry 时返回 kNoEntry。
  static const size_t kNoEntry = static_cast<size_t>(-1);
  size_t PrevOffset(size_t cur) const;

  int CompareKey(const std::string& a, const std::string& b) const;

  Slice payload_;
  const InternalKeyComparator* icmp_ = nullptr;
  std::vector<uint32_t> restarts_;
  size_t entry_area_end_;    // entry 区的结束偏移（restart 数组起点）
  size_t entry_offset_;      // 当前 entry 的起始偏移
  std::string key_;          // 已复原的当前 key
  Slice value_;
  bool valid_;
  Status status_;
};

// ---- §3.6 + I24：块的**结构**校验（不碰 CRC；CRC 由读块函数负责）----
// 校验：payload >= kBlockMinPayload、restart_count >= 1、restart_offset[0] == 0、
// 严格单调递增、每个 restart_offset 落在 entry 边界上、最后一个 restart 之后到
// restart 数组之前的字节必须能被完整解析成 entry。失败返回 kCorruption（信息含精确偏移）。
Status ValidatePayload(const Slice& payload);

}  // namespace lsm

#endif  // LSM_SSTABLE_BLOCK_H_
