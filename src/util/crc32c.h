// src/util/crc32c.h —— CRC32C（Castagnoli），docs/protocol.md §5
//
// M1 只做纯函数实现与向量测试；M2 起用于 WAL record 校验，M3 起用于 SSTable block/footer 校验。
#ifndef LSM_UTIL_CRC32C_H_
#define LSM_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace lsm {
namespace crc32c {

// 语义：初始值 0xFFFFFFFF、结果异或 0xFFFFFFFF（即 crc32c(0, data)）。
uint32_t Value(const char* data, size_t n);

// 增量：Extend(Value(a), b, m) == Value(a ++ b)。参数是「已完成最终异或的 crc」。
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

}  // namespace crc32c
}  // namespace lsm

#endif  // LSM_UTIL_CRC32C_H_
