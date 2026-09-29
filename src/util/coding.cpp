// src/util/coding.cpp —— docs/protocol.md §2~§4 的逐字实现
#include "util/coding.h"

#include <cstddef>

namespace lsm {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
  dst->append(buf, 4);
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
  dst->append(buf, 8);
}

uint32_t DecodeFixed32(const char* ptr) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(ptr[0]))) |
         (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(ptr[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(ptr[3])) << 24);
}

uint64_t DecodeFixed64(const char* ptr) {
  uint64_t lo = DecodeFixed32(ptr);
  uint64_t hi = DecodeFixed32(ptr + 4);
  return lo | (hi << 32);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  int n = 0;
  while (v >= 128u) {
    buf[n++] = static_cast<char>((v & 0x7fu) | 0x80u);
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(n));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  int n = 0;
  while (v >= 128u) {
    buf[n++] = static_cast<char>(static_cast<uint8_t>(v) | 0x80u);
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, static_cast<size_t>(n));
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128u) {
    v >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint32(char* dst, uint32_t v) {
  unsigned char* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 128u) {
    *p++ = static_cast<unsigned char>((v & 0x7fu) | 0x80u);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  unsigned char* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 128u) {
    *p++ = static_cast<unsigned char>((v & 0x7fu) | 0x80u);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = static_cast<unsigned char>(*p);
    ++p;
    if ((byte & 0x80u) != 0) {
      result |= (byte & 0x7fu) << shift;
    } else {
      // 第 5 字节只能携带低 4 位，否则溢出 2^32-1（protocol §2：必须拒绝）
      if (shift == 28 && (byte & 0x7fu) > 0x0fu) return nullptr;
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = static_cast<unsigned char>(*p);
    ++p;
    if ((byte & 0x80u) != 0) {
      result |= (byte & 0x7fu) << shift;
    } else {
      // 第 10 字节只能携带 1 位，否则溢出 2^64-1
      if (shift == 63 && byte > 0x01u) return nullptr;
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  uint32_t parsed = 0;
  const char* end = GetVarint32Ptr(input->data(), input->data() + input->size(), &parsed);
  if (end == nullptr) return false;   // 失败：input 与 *value 都不动
  *value = parsed;
  *input = Slice(end, static_cast<size_t>(input->data() + input->size() - end));
  return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  uint64_t parsed = 0;
  const char* end = GetVarint64Ptr(input->data(), input->data() + input->size(), &parsed);
  if (end == nullptr) return false;
  *value = parsed;
  *input = Slice(end, static_cast<size_t>(input->data() + input->size() - end));
  return true;
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  if (!value.empty()) dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  Slice rest = *input;                 // 在副本上推进：失败时 input/result 均不动
  if (!GetVarint32(&rest, &len)) return false;
  if (len > rest.size()) return false;
  *result = Slice(rest.data(), len);
  *input = Slice(rest.data() + len, rest.size() - len);
  return true;
}

}  // namespace lsm
