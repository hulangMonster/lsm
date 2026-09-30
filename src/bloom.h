// src/bloom.h —— filter block 的构建器/读取器 + 位级 Bloom 判定（M5.1 新增）
//
// 契约来源（逐条对应，禁止走样）：
//   docs/m5-design.md §3.2（filter block payload 位级布局）、§3.3（参数/哈希/假阳性率口径）、
//   §3.5（接口草案）、E2（`block_offset >> kFilterBaseLg` 的映射）、E3（filter CRC 始终校验）、
//   docs/protocol.md §12（M5.1 追加，与 §3 同文）。
//
// 位级格式（protocol §12.2）：
//   filter_block_payload := filter[0] ‖ … ‖ filter[n-1] ‖ offset[0..n-1](uint32 LE)
//                           ‖ array_offset(uint32 LE) ‖ n(uint32 LE)
//   filter[i]             := bitset_i ‖ k_i(1B)
#ifndef LSM_BLOOM_H_
#define LSM_BLOOM_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "filter_policy.h"

namespace lsm {

// §3.3/§12.3：LevelDB 口径的 32 位哈希（实现在 bloom.cpp 的匿名 namespace，不外泄 seed 常量）。
uint32_t BloomHash(const Slice& key);

// 自由函数（§3.5）：位级判定，供 FilterBlockReader 与测试直调；不依赖 FilterPolicy 实例。
//   len == 0 ⇒ false（该桶没有 key，安全） ；
//   len == 1 ⇒ true（只有 k 没有 bitset：保守判「可能存在」，§3.2 第 7 条）；
//   len >= 2 ⇒ `m = (len-1)*8`、`k = filter[len-1]` 的 Double Hashing 位检查。
bool BloomKeyMayMatch(const Slice& key, const Slice& filter);

// ---- 写入侧：单线程构建（L30/I50）----
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* policy);
  ~FilterBlockBuilder();
  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  // 在写一个数据块**之前**调用（block_offset = 该数据块起始文件偏移）。
  // 语义（E2）：target = block_offset >> kFilterBaseLg；
  //             while (target > filter_offsets_.size()) GenerateFilter();   // 逐桶补空过滤器
  void StartBlock(uint64_t block_offset);

  // 该数据块内每一条 entry 的 **user key** 调一次（含 tombstone；D2）。
  void AddKey(const Slice& user_key);

  // 收尾：若非空则 GenerateFilter()，再追加 offset[] ‖ array_offset ‖ n；返回 payload。
  // 返回的 Slice 指向 builder 内部的 result_ ⇒ 调用方必须在写块前保持本对象存活。
  Slice Finish();

  size_t NumFilters() const { return filter_offsets_.size(); }
  size_t CurrentSizeEstimate() const;

 private:
  void GenerateFilter();

  const FilterPolicy* policy_ = nullptr;
  std::string result_;                       // filter[0..n-1] 的拼接
  std::vector<std::string> keys_;            // 当前桶收集到的 key（GenerateFilter 后清空）
  std::vector<uint32_t> filter_offsets_;     // 每个 filter 在 result_ 中的绝对偏移
  bool finished_ = false;
};

// ---- 读取侧：构造后**只读**，多线程 KeyMayMatch 无需加锁（L30/I50）----
class FilterBlockReader {
 public:
  // 解析 payload（§3.2 的 8 条结构校验）。失败 ⇒ valid()==false，调用方降级为「可能存在」。
  // 不抛异常、不返回 Status —— filter 是 advisory（D3(b)/§3.4）。
  explicit FilterBlockReader(const Slice& contents);
  ~FilterBlockReader();
  FilterBlockReader(const FilterBlockReader&) = delete;
  FilterBlockReader& operator=(const FilterBlockReader&) = delete;

  bool valid() const { return valid_; }
  // block_offset = 将要读取的数据块起始偏移；返回 false = 否定（可跳过该数据块）。
  // 越界 / 不可用一律返回 true（§3.7 硬规则 3）。
  bool KeyMayMatch(uint64_t block_offset, const Slice& user_key) const;

  size_t NumFilters() const { return num_; }
  uint64_t filter_bytes() const { return contents_.size(); }

 private:
  std::string contents_;      // 拥有 payload 的生命周期（§3.5 的所有权要求）
  size_t num_ = 0;            // filter 个数
  size_t array_offset_ = 0;   // offset[0] 在 payload 中的偏移
  bool valid_ = false;
};

}  // namespace lsm

#endif  // LSM_BLOOM_H_
