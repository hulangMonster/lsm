// src/util/coding.h —— varint / fixed / length-prefixed（docs/protocol.md §2~§4，逐字实现）
#ifndef LSM_UTIL_CODING_H_
#define LSM_UTIL_CODING_H_

#include <cstdint>
#include <string>

#include "common.h"

namespace lsm {

// 固定宽度：小端落盘（protocol §3）。禁止 reinterpret_cast 到整型指针直接读写。
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
uint32_t DecodeFixed32(const char* ptr);
uint64_t DecodeFixed64(const char* ptr);

// LEB128（protocol §2）：7 bit/byte，最高位为续位；32 位最多 5 字节、64 位最多 10 字节。
void PutVarint32(std::string* dst, uint32_t v);
void PutVarint64(std::string* dst, uint64_t v);

// 裸缓冲编码：把 v 写进 dst，返回「编码末尾」指针（LevelDB 口径，供 MemTable 条目拼接复用）。
char* EncodeVarint32(char* dst, uint32_t v);
char* EncodeVarint64(char* dst, uint64_t v);

// 指针式解析：成功返回新位置，失败返回 nullptr（p 不前进）。
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

// Slice 前进式解析：成功时 input 前进、*value 写出；**失败时 input 与 *value 都不修改**。
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);

// varint 编码后的字节数（MemTable 条目长度前缀用；不写入，只算长度）。
int VarintLength(uint64_t v);

// protocol §4：varint32 长度前缀 + 原始字节
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

}  // namespace lsm

#endif  // LSM_UTIL_CODING_H_
