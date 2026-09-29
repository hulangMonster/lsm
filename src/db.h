// src/db.h —— 对外接口（docs/m1-design.md §4.5/§9；M2 增补见 docs/m2-design.md §5/§7）
//
// M2 的契约变化（登记于 docs/m2-prerequisites.md §9）：`Open(options, name, dbptr)` 的 name
// **非空**从 M1 的"仅内存模式 ⇒ kNotSupported"变成"持久模式（WAL + 恢复）"。
// 因此 M1 的 DB.OpenRejectsNonEmptyName 断言随契约更新（只改与该契约直接相关的那一条）。
#ifndef LSM_DB_H_
#define LSM_DB_H_

#include <string>

#include "common.h"

namespace lsm {

// 每条写的持久性要求（design §7）：true = durable-before-ack（返回即已 fsync）；
// false = 只 write 不 fsync（崩溃可能丢最近的写，但绝不允许半条/乱序/旧值覆盖新值）。
struct WriteOptions {
  bool sync = false;
};

class DB {
 public:
  // name 为空串 = 内存模式（M1 语义）；非空 = 持久模式：目录不存在则创建，
  // 并做 WAL 恢复（design §5）。任何失败路径都必须 *dbptr = nullptr，且不产生半构造对象。
  static Status Open(const Options& options, const std::string& name, DB** dbptr);

  DB() = default;
  virtual ~DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
  // 便捷重载：等价于 WriteOptions()（sync=false）
  Status Put(const Slice& key, const Slice& value) { return Put(WriteOptions(), key, value); }
  Status Delete(const Slice& key) { return Delete(WriteOptions(), key); }

  virtual Status Get(const Slice& key, std::string* value) = 0;

  // 用户视图：user key 升序、每个 key 只出最新可见版本、跳过 tombstone。返回值所有权归调用方。
  virtual Iterator* NewIterator() = 0;

  // design §7：把此前**所有**已返回 kOk 的写入刷到磁盘（返回即全部 durable）；幂等。
  virtual Status Sync() = 0;
  // design §6.5/I20：拒绝新写 → 等在途写完成 → Sync → 关闭句柄；幂等；析构等价于 Close。
  virtual Status Close() = 0;
};

}  // namespace lsm

#endif  // LSM_DB_H_
