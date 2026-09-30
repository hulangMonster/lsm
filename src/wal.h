// src/wal.h —— 预写日志（docs/m2-design.md §4，位级格式见 design §4.1~§4.5 与 protocol.md §9）
//
// 两条硬约束（docs/m2-prerequisites.md §6）：
//   1) WAL 路径**禁止任何用户态缓冲**：M2 #0 实测「裸 write 逐条 100 轮 TAIL_TORN 0，带 4 KiB stdio 缓冲 100 轮 TORN 95」
//      —— 真实撕裂来自用户态缓冲边界，而 kill -9 打断不了一次 write()。因此每次 Append 直接落到 Env 的 WritableFile。
//   2) 解码前必须校验 length 合法性，再算 CRC（顺序不能反；m1-review 阻断项 2 的同源风险）。
#ifndef LSM_WAL_H_
#define LSM_WAL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "common.h"
#include "util/env.h"

namespace lsm {

constexpr size_t kWALBlockSize = 32768;                          // 32 KiB
constexpr size_t kWALHeaderSize = 7;                             // crc(4) + length(2) + type(1)
constexpr size_t kWALMaxPayload = kWALBlockSize - kWALHeaderSize;  // 32761
constexpr size_t kMaxLogicalRecordSize = 64u * 1024 * 1024;      // 重组上限（§4.3）

enum RecordType : uint8_t {
  kZeroType = 0x00,   // 仅 padding 哨兵（crc == 0 && length == 0 && type == 0）
  kFullType = 0x01,
  kFirstType = 0x02,
  kMiddleType = 0x03,
  kLastType = 0x04,
};

enum class WALScanVerdict {
  kClean,        // 文件恰好结束在 record 边界
  kTailResidue,  // 尾部字节不足（文件在 header/record 中间结束）——可安全截断
  kParseFail,    // 结构性损坏（非法 type/length、CRC 不符、类型状态机不自洽）
};

struct WALScanResult {
  WALScanVerdict verdict = WALScanVerdict::kClean;
  uint64_t last_good_end = 0;               // 最后一条**完整** record 的结束偏移（＝截断点）
  uint64_t failure_offset = 0;              // 判定失败处的偏移
  bool valid_record_after_failure = false;  // 失败点之后是否仍能找到结构完好的片段
  std::string detail;                       // 可定位信息（供 kCorruption 的 Status 组装）
};

// 写端：一条逻辑 record = 一个原子单位（组提交的批就编码成一条 record，design §4.7/D3）
class WALWriter {
 public:
  WALWriter(Env* env, std::string fname);
  ~WALWriter();
  WALWriter(const WALWriter&) = delete;
  WALWriter& operator=(const WALWriter&) = delete;

  // append=false：截断新建；append=true：追加打开（恢复后继续写），block_offset 由现有大小推出。
  Status Open(bool append);
  Status Append(const Slice& record);
  Status Sync();
  Status Close();

  uint64_t file_size() const { return file_size_; }
  uint64_t block_offset() const { return block_offset_; }

 private:
  Env* const env_;
  const std::string fname_;
  std::unique_ptr<WritableFile> file_;
  uint64_t file_size_ = 0;
  uint64_t block_offset_ = 0;
  Status error_;        // 粘性错误（D11 fail-stop：短写/fsync 失败后文件偏移已不可信）
  bool closed_ = true;

  // ---- R1：WALWriter 的内部锁（**严格叶子**，不参与 DB 的全局锁序）----
  // 为什么需要：R1 把 DB::Sync()/RotateLog() 的 fsync 移出 commit_mu_ 之后，DB::Sync() 的 fsync
  // 与 flusher 的 Append/fsync 可以真的并发（既有代码里这两条路径本来就已能在 error_ 上撞车）。
  // 为什么是**两把**而不是一把：一把叶子锁若覆盖整个 Sync()，就会让 Append 排在 fsync 后面 ——
  // 那只是把 R1 的缺陷从 DB 层搬到文件层（sync=false 的写仍被 fsync 窗口挡住；实测可复现 RED）。
  //   io_mu_  ：只保护对象自身的可变状态（error_/closed_/file_/偏移），临界区短、**不含 fsync**；
  //   sync_mu_：只串行化**同一文件句柄上的 fsync**（并覆盖 Close 的关文件），临界区长、可阻塞。
  // 本类内部锁序（唯一）：sync_mu_ → io_mu_；Append 只取 io_mu_。
  // 与 DB 锁的关系：**所有**调用点都不得持 DB 锁（install_mu_/commit_mu_/deletion_mu_/mutex_）
  // 进入本类的方法（src/db_impl.cpp 的 5 处调用点逐一核对），而本类也绝不获取任何 DB 锁、
  // 不回调 DB ⇒ 不存在反向边，故它接在全局全序的最右端且构不成环（详见 docs 修订记录）。
  mutable std::mutex io_mu_;
  mutable std::mutex sync_mu_;
};

// 读端：顺序扫描并重组；结构性损坏不走 Status，而是通过 *result 报告事实，
// 「尾部截断 vs 中间损坏」的 §5.3 分类由 M2.2 的 Recovery 依据本结果施加。
class WALReader {
 public:
  using EmitFn = std::function<void(const Slice&)>;

  WALReader(Env* env, std::string fname);
  ~WALReader();
  WALReader(const WALReader&) = delete;
  WALReader& operator=(const WALReader&) = delete;

  Status ReadAll(const EmitFn& emit, WALScanResult* result);

 private:
  Env* const env_;
  const std::string fname_;
};

// 从 offset 起是否存在结构完好的片段（重同步判定的事实来源；实现有候选数与字节上限，见 .cpp）
bool WALHasValidFragmentAfter(Env* env, const std::string& fname, uint64_t offset);

}  // namespace lsm

#endif  // LSM_WAL_H_
