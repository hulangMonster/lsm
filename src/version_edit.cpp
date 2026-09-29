// src/version_edit.cpp —— M3.2：VersionEdit 目前是纯内存的全量快照（见头文件的范围说明）。
#include "version_edit.h"

namespace lsm {

// 目前所有访问器都在头文件内实现；保留本 TU 是因为交付物要求 version_edit.{h,cpp} 成对，
// 且 M3.3 的 EncodeTo/DecodeFrom（§10.8）会落到这里。不放置无实现声明的占位符。
}  // namespace lsm
