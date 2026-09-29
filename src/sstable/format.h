// src/sstable/format.h —— SSTable 位级格式：常量、块句柄、footer（M3.1 骨架）
//
// 契约来源（逐条对应，禁止各写一套）：
//   docs/m3-design.md §3.6（块头与 CRC 覆盖面）、§3.7（footer 44B）、§3.3（handle 16B）
//   docs/protocol.md §10（M3.1 追加，与 §3 同文）
//
// 本文件在 `#2` 阶段只提供**声明与常量**：实现留到 `#3`（M3.1）。因此 `#2` 的 RED 表现是
// 「用例可编译、链接失败」，与 M1 的 `#2` 骨架同形（见 docs/m1-tdd-red.log 的先例）。
#ifndef LSM_SSTABLE_FORMAT_H_
#define LSM_SSTABLE_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "common.h"

namespace lsm {

// ---- §3.6 块头/块尾 ----
// block_on_disk := header(5B) ‖ payload ‖ crc32c(4B LE)
// header        := length(4B LE) ‖ type(1B)
// crc           := crc32c( length ‖ type ‖ payload )      含长度字段（§3.6 的理由）
enum BlockType : uint8_t {
  kBlockTypeData = 0x01,
  kBlockTypeIndex = 0x02,
  kBlockTypeMetaIndex = 0x03,
  kBlockTypeFilter = 0x04,  // M5 预留；M3 不产出
};

constexpr size_t kBlockHeaderSize = 5;
constexpr size_t kBlockTrailerSize = 4;
constexpr size_t kBlockOverhead = kBlockHeaderSize + kBlockTrailerSize;  // 9
// 最小合法 payload：空块 = restart_offset[0]=0 (4B) + restart_count=1 (4B)（§3.2）
constexpr size_t kBlockMinPayload = 8;
// length 只用来「检出不等于」；读取长度一律以 handle.size 为权威（§3.6）
constexpr uint32_t kBlockLengthMismatchIsCorruption = 1;

// ---- §3.2 / §3.3 restart 间隔 ----
constexpr int kRestartInterval = 16;       // 数据块：每 16 条 entry 一个 restart 点
constexpr int kIndexRestartInterval = 1;   // 索引块：restart 间隔 1（精确二分）

// ---- §3.7 footer ----
constexpr size_t kFooterSize = 44;
constexpr uint32_t kTableFormatVersion = 1;
extern const char kTableMagic[4];  // 'L','S','M','1'

// ---- §3.3 块句柄：offset(8B LE) ‖ size(8B LE) ----
constexpr size_t kBlockHandleEncodedLength = 16;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;

  // 追加恰好 16 字节到 *dst。
  void EncodeTo(std::string* dst) const;
  // 从 input 解出 16 字节，*consumed 置 16；不足 16 字节 ⇒ kCorruption。
  Status DecodeFrom(const Slice& input, size_t* consumed);
};

// ---- §3.7 footer := magic(4) ‖ version(4 LE) ‖ index_handle(16) ‖ metaindex_handle(16) ‖ crc(4) ----
// footer_crc 覆盖前 40 字节（magic ‖ version ‖ 两个 handle）。
struct Footer {
  BlockHandle index_handle;
  BlockHandle metaindex_handle;

  // 追加恰好 kFooterSize(44) 字节到 *dst。
  void EncodeTo(std::string* dst) const;
  // 从 input 解出 footer。要求 input.size() >= kFooterSize；失败矩阵见 §3.7：
  //   magic 不符 ⇒ kCorruption；version != 1 ⇒ **kNotSupported**；footer_crc 不符 ⇒ kCorruption。
  // 注意：handle 越界 / metaindex 与 index 顺序 / index 是否紧贴 footer 需要 file_size，
  // 由 Table 层在打开文件时校验（M3.1 的 table 片，见 M3-A08 后三行）。
  Status DecodeFrom(const Slice& input);
};

}  // namespace lsm

#endif  // LSM_SSTABLE_FORMAT_H_
