// src/write_batch.h —— 批写容器与 §13.1 编码（M5.2 新增）
//
// 契约来源（逐条对应，禁止走样）：
//   docs/m5-design.md §5.1（接口草案）、§5.4（整批原子性证明的第 1 步：入队前预校验）、
//   §4 §13（WriteBatch 编码，M5 定稿）、§13.3（批大小上限与超限处理）、
//   docs/protocol.md §13（M5.2 追加，与 §4 §13 同文）、§9.4（WAL batch payload 逐字同构）。
//
// 依赖纪律（M5-design §1.4 / §5.1）：本头文件**只**依赖 common.h；实现只额外用 util/coding.h。
// 不得依赖 db_impl / wal / memtable（否则「filter 与 batch 不得反向依赖 db_impl」被打破）。
//
// 所有权与线程约束（L31，逐字写进接口注释，评审逐条核对）：
//   * `WriteBatch` 由**调用方拥有**；`DB` 不持有它的裸指针超过 `DB::Write` 的调用期。
//   * 同一个 `WriteBatch` **不得**在 `DB::Write` 进行期间被另一个线程修改或销毁；
//     本类内部**没有锁**，跨线程并发访问同一对象是未定义行为（设计 §5.1 末段）。
//   * `DB::Write` 返回后，调用方可以继续 `Put`/`Clear` 复用该对象；`DB::Write` 不保留引用。
#ifndef LSM_WRITE_BATCH_H_
#define LSM_WRITE_BATCH_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "common.h"

namespace lsm {

// 一个 WriteBatch = §13.1 的 `batch_payload`；`Data()` 的字节与 WAL 里的批记录 payload 逐字相同。
class WriteBatch {
 public:
  static constexpr size_t kHeaderSize = 12;                 // sequence(8) + count(4)
  static constexpr uint32_t kMaxCount = 1u << 20;           // §13.3 / §9.4 的 kMaxBatchCount
  static constexpr size_t kMaxBytes = 64u * 1024 * 1024;    // §13.3 = kMaxLogicalRecordSize

  WriteBatch();     // rep_ = 12 个 0 字节（sequence=0、count=0）
  ~WriteBatch();
  WriteBatch(const WriteBatch&) = delete;
  WriteBatch& operator=(const WriteBatch&) = delete;

  // 追加一条；不返回 Status（与 LevelDB 同形，§5.1）。超限由 DB::Write 在提交前拒绝（§13.3）。
  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);
  void Clear();     // rep_ = 12 个 0 字节

  size_t Count() const;                              // batch 内的 entry 条数（读 12B 头）
  size_t ByteSize() const { return rep_.size(); }     // == rep_.size()，含 12B 头
  SequenceNumber Sequence() const;                    // 未提交时为 0
  // 仅供 DB::Write / 测试；调用方不得依赖（§5.1 末段：DB::Write 会重新分配 sequence）。
  void SetSequence(SequenceNumber seq);
  Slice Data() const { return Slice(rep_); }          // 完整 batch 编码（含 12B 头）

  // 逐条回调；成功返回 kOk；**结构性**畸形（截断 / varint 非法 / type 非法 / count 解完仍有剩余）
  // 返回 kCorruption；失败时 handler 可能已被部分调用（§5.1 的注释逐字要求）。
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };
  Status Iterate(Handler* handler) const;

  // 无副作用的提交前预校验（§5.4 第 1 步 / M5-A11）。`DB::Write` 必须在**入队与写 WAL 之前**
  // 调用它：输入校验失败不得触发 fail-stop（M2 的教训：坏输入曾被当成持久化失败写进 bg_error_）。
  //   * 结构性畸形 ⇒ kCorruption；
  //   * count == 0 / count > kMaxCount / 空 user key / key 超 kMaxUserKeySize /
  //     ByteSize()+16 > kMaxLogicalRecordSize ⇒ kInvalidArgument。
  // 成功时（出参非空）写出 count、Σ(entry 编码字节)、Σ(key.size + value.size)。
  Status Validate(uint32_t* count, size_t* entry_bytes, uint64_t* user_bytes) const;

  // **仅测试/诊断**：用未经校验的字节替换内部 rep_（M5-A17 的「Iterate 对畸形 rep 返回 kCorruption」
  // 需要一个能承载畸形输入的对象）。生产的构造路径只有 Put/Delete/Clear。
  explicit WriteBatch(const Slice& raw_data);

 private:
  std::string rep_;   // header(12) + entries；与 §13.1 逐字同构
};

}  // namespace lsm

#endif  // LSM_WRITE_BATCH_H_
