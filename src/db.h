// src/db.h —— 对外接口（docs/m1-design.md §4.5/§9）。M1 只有内存模式。
//
// M1 不引入 WriteOptions/ReadOptions：sync/snapshot 在 M1 无实现，写进去就是
// 「看起来能用但没实现」的假字段（design §1.4）。M2/M3 以新增重载的形式补，不破坏本契约。
#ifndef LSM_DB_H_
#define LSM_DB_H_

#include <string>

#include "common.h"

namespace lsm {

class DB {
 public:
  // name 为空串 = M1 内存模式；非空 = kNotSupported（持久化在 M2 引入）。
  // 任何失败路径都必须 *dbptr = nullptr，且不产生半构造对象（I8）。
  static Status Open(const Options& options, const std::string& name, DB** dbptr);

  DB() = default;
  virtual ~DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const Slice& key) = 0;
  virtual Status Get(const Slice& key, std::string* value) = 0;

  // 用户视图：user key 升序、每个 key 只出最新可见版本、跳过 tombstone。
  // 返回值所有权归调用方（design §4.5）。
  virtual Iterator* NewIterator() = 0;
};

}  // namespace lsm

#endif  // LSM_DB_H_
