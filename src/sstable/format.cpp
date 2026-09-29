// src/sstable/format.cpp —— §3.3 块句柄与 §3.7 footer 的编解码（M3.1）
#include "sstable/format.h"

#include <cstring>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

const char kTableMagic[4] = {'L', 'S', 'M', '1'};

void BlockHandle::EncodeTo(std::string* dst) const {
  PutFixed64(dst, offset);
  PutFixed64(dst, size);
}

Status BlockHandle::DecodeFrom(const Slice& input, size_t* consumed) {
  if (input.size() < kBlockHandleEncodedLength) {
    return Status::Corruption("BlockHandle", "truncated: need 16 bytes");
  }
  offset = DecodeFixed64(input.data());
  size = DecodeFixed64(input.data() + 8);
  if (consumed != nullptr) *consumed = kBlockHandleEncodedLength;
  return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  dst->append(kTableMagic, sizeof(kTableMagic));
  PutFixed32(dst, kTableFormatVersion);
  index_handle.EncodeTo(dst);
  metaindex_handle.EncodeTo(dst);
  // footer_crc 覆盖前 40 字节（magic ‖ version ‖ 两个 handle）；§3.7 说明了它为什么必要：
  // index_handle 损坏 = 随机跳读，而"凑巧合法"的概率极低但不是零，故必须确定性检出。
  PutFixed32(dst, crc32c::Value(dst->data() + start, kFooterSize - 4));
}

Status Footer::DecodeFrom(const Slice& input) {
  if (input.size() < kFooterSize) {
    return Status::Corruption("Footer", "file too small to be an SSTable");
  }
  const char* p = input.data();
  // 检查顺序即 §3.7 的失败矩阵：magic → footer_crc → version。
  // version 放在 CRC 之后：一个由更新引擎写出的合法文件必然带**正确**的 CRC，
  // 所以"CRC 通过但 version 不认识"才是真正的前向兼容信号（kNotSupported）。
  if (std::memcmp(p, kTableMagic, sizeof(kTableMagic)) != 0) {
    return Status::Corruption("Footer", "bad magic");
  }
  const uint32_t stored_crc = DecodeFixed32(p + kFooterSize - 4);
  if (stored_crc != crc32c::Value(p, kFooterSize - 4)) {
    return Status::Corruption("Footer", "footer crc mismatch");
  }
  const uint32_t version = DecodeFixed32(p + 4);
  if (version != kTableFormatVersion) {
    return Status::NotSupported("Footer", "table format version is newer than this engine");
  }
  size_t consumed = 0;
  Status s = index_handle.DecodeFrom(Slice(p + 8, kBlockHandleEncodedLength), &consumed);
  if (!s.ok()) return s;
  s = metaindex_handle.DecodeFrom(Slice(p + 24, kBlockHandleEncodedLength), &consumed);
  if (!s.ok()) return s;
  return Status::OK();
}

}  // namespace lsm
