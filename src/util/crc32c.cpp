// src/util/crc32c.cpp —— 表驱动 CRC32C（反射多项式 0x82F63B78，docs/protocol.md §5）
#include "util/crc32c.h"

namespace lsm {
namespace crc32c {
namespace {

struct Crc32cTable {
  uint32_t t[256];
  Crc32cTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
  }
};

// C++11 magic static：首次使用时构造一次，线程安全。
const Crc32cTable& Table() {
  static const Crc32cTable table;
  return table;
}

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  const Crc32cTable& table = Table();
  uint32_t l = init_crc ^ 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t idx = (l ^ static_cast<unsigned char>(data[i])) & 0xffu;
    l = table.t[idx] ^ (l >> 8);
  }
  return l ^ 0xffffffffu;
}

uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

}  // namespace crc32c
}  // namespace lsm
