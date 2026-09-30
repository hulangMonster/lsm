// src/db_impl.cpp —— 恢复（§5）+ 过渡写路径（§6.2）+ sync/close 语义（§7）
#include "db_impl.h"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "db_iter.h"
#include "filename.h"
#include "merging_iterator.h"
#include "sstable/table_builder.h"
#include "util/coding.h"
#include "write_batch.h"

namespace lsm {
namespace {


namespace {
// A25 探针（I17 持锁零 IO 的可验证化）：持有 DB 互斥锁的线程置位该标记。
// 只做诊断，不改变加锁语义（内部仍是 std::lock_guard）。
thread_local bool g_db_mutex_held = false;
thread_local bool g_install_mu_held = false;

struct InstallMuGuard {
  InstallMuGuard() { g_install_mu_held = true; }
  ~InstallMuGuard() { g_install_mu_held = false; }
};

struct DbMutexGuard {
  explicit DbMutexGuard(std::mutex& m) : lk(m) { g_db_mutex_held = true; }
  ~DbMutexGuard() { g_db_mutex_held = false; }
  std::lock_guard<std::mutex> lk;
};
}  // namespace

constexpr size_t kRecoverySlack = 1u * 1024 * 1024;      // D12：恢复容量的余量
constexpr uint32_t kMaxBatchCount = 1u << 20;            // §9.4：防畸形 count 撑爆
constexpr size_t kMaxGroupBytes = 1u * 1024 * 1024;      // D3：一批的字节上限（1 MiB）
constexpr size_t kMaxGroupRecs = 64;                     // D3：一批的写者数上限
// M3 §6.5：未落盘 immutable 的上限。达到该值后写者在取批前停等后台 flush（不是 kFrozen）。
constexpr size_t kMaxImmutableMemTables = 2;
// 每条 entry 在 MemTable 里的额外占用（跳表节点 ≈ sizeof(Node) + 分配 slop）。
// 用于把"WAL 侧字节估算"折算成"MemTable 容量占用"，避免两处口径漂移（M2 评审阻断项 1）。
constexpr size_t kMemTableNodeOverhead = 128;
// 截断量超过这个阈值就打 WARN（design §5.3/§5.4 承诺"截断与被跳过的 record 必须计数上报"）
constexpr uint64_t kMaxTailCorruptWarnBytes = 2 * 1024 * 1024;   // 2 MiB

struct BatchEntry {
  ValueType type = kTypeValue;
  std::string key;
  std::string value;
};

// M5.2：解一条 entry（§9.4 / §13.1 的 entry 编码）。成功时 input 前进，失败时**不修改** input。
// 单条写路径（Pending::entries）与 WAL 恢复路径（ParseBatch）共用这一份实现，避免两处口径漂移。
bool ParseOneEntry(Slice* input, BatchEntry* e) {
  Slice in = *input;
  if (in.empty()) return false;
  const uint8_t t = static_cast<uint8_t>(in[0]);
  in = Slice(in.data() + 1, in.size() - 1);
  if (t != kTypeValue && t != kTypeDeletion) return false;
  e->type = static_cast<ValueType>(t);
  uint32_t klen = 0;
  if (!GetVarint32(&in, &klen) || klen == 0 || klen > kMaxUserKeySize || klen > in.size()) {
    return false;
  }
  e->key.assign(in.data(), klen);
  in = Slice(in.data() + klen, in.size() - klen);
  e->value.clear();
  if (e->type == kTypeValue) {
    uint32_t vlen = 0;
    if (!GetVarint32(&in, &vlen) || vlen > in.size()) return false;
    e->value.assign(in.data(), vlen);
    in = Slice(in.data() + vlen, in.size() - vlen);
  }
  *input = in;
  return true;
}

// §9.4 的解析：必须**恰好消费完** payload，count 条解完还有剩余字节即损坏
bool ParseBatch(const Slice& payload, SequenceNumber* seq, std::vector<BatchEntry>* entries,
                std::string* why) {
  entries->clear();
  Slice input = payload;
  if (input.size() < 12) {
    *why = "batch 头不足 12 字节";
    return false;
  }
  const SequenceNumber s = DecodeFixed64(input.data());
  const uint32_t count = DecodeFixed32(input.data() + 8);
  input = Slice(input.data() + 12, input.size() - 12);
  if (count == 0 || count > kMaxBatchCount) {
    *why = "count 越界：" + std::to_string(count);
    return false;
  }
  if (s + count - 1 > kMaxSequenceNumber) {
    *why = "sequence + count - 1 超过 kMaxSequenceNumber";
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (input.empty()) {
      *why = "entry 数量少于 count";
      return false;
    }
    BatchEntry e;
    if (!ParseOneEntry(&input, &e)) {
      *why = "entry " + std::to_string(i) + " 解析失败（type / key_len / value_len 非法）";
      return false;
    }
    entries->push_back(std::move(e));
  }
  if (!input.empty()) {
    *why = "entry 解完后仍有 " + std::to_string(input.size()) + " 字节剩余";
    return false;
  }
  *seq = s;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 用户视图迭代器：M3 起由 DBIter + 单 child MergingIterator 提供（§7.3），
// 避免 M1 的 UserIterator 与 M3 的 DBIter 两份可见性实现漂移。
// ---------------------------------------------------------------------------
Iterator* NewMemTableUserIterator(const MemTable* mem, const InternalKeyComparator& icmp) {
  Iterator** kids = new Iterator*[1];
  kids[0] = mem->NewIterator();
  return new DBIter(&icmp, new MergingIterator(&icmp, kids, 1), kMaxSequenceNumber);
}

// ---------------------------------------------------------------------------
// PersistentDBImpl
// ---------------------------------------------------------------------------
// memtable_capacity 由恢复期按 D12 决定（max(options.write_buffer_size, WAL 实测字节数 + slack)）
PersistentDBImpl::PersistentDBImpl(const Options& options, const InternalKeyComparator& icmp,
                                   std::string dbname, size_t memtable_capacity)
    : options_(options),
      internal_comparator_(icmp),
      dbname_(std::move(dbname)),
      memtable_(std::make_shared<MemTable>(internal_comparator_, memtable_capacity)) {
  table_cache_.reset(new TableCache(EnvOf(), dbname_, options_, options_.max_open_files));
}

PersistentDBImpl::~PersistentDBImpl() {
  if (!closed_) Close();
}

Status PersistentDBImpl::SubmitPending(Pending* w) {
  // 评审优化项（M2 起的既有语义，逐字保留）：超过 WAL 单条 record 上限的输入必须在**入队之前**
  // 拒绝。否则批次会被接受、Append 返回 kInvalidArgument、RunFlusher 把它当持久化失败写进
  // bg_error_ ⇒ 整个库转粘性写只读（连 Close 都返回错误）。输入校验不该触发 fail-stop。
  if (w->entry_bytes + 16 > kMaxLogicalRecordSize) {
    return Status::InvalidArgument("Put/Delete: record exceeds kMaxLogicalRecordSize",
                                   std::to_string(w->entry_bytes + 16));
  }

  // ---- M2.3 组提交（design §6.3）----
  // 每个写者入队后等待被结算；队首当选 flusher，把一批写者的条目**合并成一条 WAL record**
  // （一条 record = 一个 CRC = 一个原子单位，I15），并替整批做一次 fsync。
  std::unique_lock<std::mutex> l(commit_mu_);
  {
    // L11/A31：**入队前**就拒绝关闭后的新写。若只在 RunFlusher 里判 closed_，
    // 关闭期间源源不断的新写者会不断把 flusher_active_ 置回 true，Close 等待 !flusher_active_
    // 就会活锁（实测：Close.RejectsNewWriters 挂住）。顺序必须是"先拒绝新写 → 再等在途批"。
    DbMutexGuard ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）
    // M2-I35（TSan 实测）：bg_error_ 的读写必须**同受 mutex_**（db_impl.h 的锁声明）。
    // 原来这一行只在 commit_mu_ 下读，与 RunFlusher() 里持 mutex_ 的 `bg_error_ = s` 构成
    // data race：Status 的 code 与 message 可能来自不同错误，最坏是漏掉粘性 fail-stop。
    if (!bg_error_.ok()) return bg_error_;   // 粘性 fail-stop（D11）
    if (closed_) return Status::IOError("Put/Delete: DB is closed", dbname_);
  }
  queue_.push_back(w);
  while (!w->done) {
    // 谓词必须同时覆盖「我已经完成」与「**我能否接手当 flusher**」——只等"完成"会让最后一个写者
    // 睡满超时后才能当 flusher（raft-kv 的 P2a 丢唤醒复盘，L9/L10）。这里用 while + 无谓词 wait
    // 重查，语义等价且不会丢唤醒。
    if (!flusher_active_ && queue_.front() == w) {
      flusher_active_ = true;
      l.unlock();
      const Status s = RunFlusher();
      l.lock();
      // D4 窗口放开时机：RunFlusher 已经「结算并唤醒组内成员」，此处才清 flusher_active_，
      // 紧接着唤醒新队首。顺序不可交换：先清标志会让下一批与本次收尾并发，多出一次排队中的 fsync。
      flusher_active_ = false;
      commit_cv_.notify_all();   // 交接（L10）；用 notify_all 是因为 Close() 也在这把 cv 上等
      if (!s.ok() && !w->done) return s;
      continue;
    }
    commit_cv_.wait(l);
  }
  return w->status;
}

Status PersistentDBImpl::WriteEntry(ValueType type, const WriteOptions& options, const Slice& key,
                                    const Slice& value) {
  if (key.empty()) return Status::InvalidArgument("Put/Delete: empty user key");
  if (key.size() > kMaxUserKeySize) {
    return Status::InvalidArgument("Put/Delete: user key too large", std::to_string(key.size()));
  }
  // M5.2（§5.3）：Pending 统一承载「entry 编码串」；单条写就是 entry_count == 1 的特例，
  // 编码口径与 M2/M4 落地实现逐字相同（1B type + varint + key [+ varint + value]）。
  Pending w;
  w.need_sync = options.sync;
  w.entry_count = 1;
  w.user_bytes = key.size() + (type == kTypeValue ? value.size() : 0);
  w.entries.reserve(1 + 10 + key.size() + (type == kTypeValue ? 10 + value.size() : 0));
  w.entries.push_back(static_cast<char>(type));
  PutVarint32(&w.entries, static_cast<uint32_t>(key.size()));
  w.entries.append(key.data(), key.size());
  if (type == kTypeValue) {
    PutVarint32(&w.entries, static_cast<uint32_t>(value.size()));
    w.entries.append(value.data(), value.size());
  }
  w.entry_bytes = w.entries.size();
  return SubmitPending(&w);
}

Status PersistentDBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  if (updates == nullptr) {
    return Status::InvalidArgument("Write: null WriteBatch");
  }
  // §5.4 第 1 步：**入队前**的无副作用预校验。任何失败都不得写 WAL、不得碰内存、
  // 不得触发 fail-stop（bg_error_ 粘性会让整库转只读）。
  uint32_t count = 0;
  size_t entry_bytes = 0;
  uint64_t user_bytes = 0;
  const Status vs = updates->Validate(&count, &entry_bytes, &user_bytes);
  if (!vs.ok()) return vs;

  const Slice data = updates->Data();
  Pending w;
  w.need_sync = options.sync;
  w.entry_count = count;
  w.entry_bytes = entry_bytes;
  w.user_bytes = user_bytes;
  // entries = batch_payload 去掉 12B 头；EncodeGroup 只补 12B 头（§13.2：一个 WriteBatch 的
  // entry 在 payload 内保持连续且顺序不变）。
  w.entries.assign(data.data() + WriteBatch::kHeaderSize, data.size() - WriteBatch::kHeaderSize);
  return SubmitPending(&w);
}

namespace {
// RAII：恢复失败等任何提前返回路径都必须释放 LOCK，否则同一进程内重试会被自己的锁挡住
class ScopedFileLock {
 public:
  ScopedFileLock(Env* env, FileLock* lock) : env_(env), lock_(lock) {}
  ~ScopedFileLock() {
    if (lock_ != nullptr) env_->UnlockFile(lock_);
  }
  FileLock* release() {
    FileLock* l = lock_;
    lock_ = nullptr;
    return l;
  }

 private:
  Env* const env_;
  FileLock* lock_;
};
}  // namespace

std::string PersistentDBImpl::EncodeGroup(SequenceNumber begin,
                                          const std::vector<Pending*>& members) {
  // §9.4 / §13.2 的 batch 编码：sequence(8B LE) || count(4B LE) || entry[0..count)
  // M5.2：count = 组内**所有 entry** 的总数（不再等于写者数）；每个成员的 entries 连续且有序。
  uint32_t total_count = 0;
  for (const Pending* p : members) total_count += p->entry_count;
  std::string out;
  PutFixed64(&out, begin);
  PutFixed32(&out, total_count);
  for (const Pending* p : members) out.append(p->entries);
  return out;
}

void PersistentDBImpl::WaitForImmutableCapacity() {
  std::unique_lock<std::mutex> l(mutex_);
  // 谓词必须覆盖三个退出条件（docs/m3-design.md §6.5）：容量让出、bg_error_、closed_。
  while (immutables_.size() >= kMaxImmutableMemTables && bg_error_.ok() && !closed_) {
    ++flush_stats_.stall_events;
    const uint64_t t0 = EnvOf()->NowMicros();
    bg_cv_.wait(l);
    flush_stats_.stall_micros += EnvOf()->NowMicros() - t0;
  }
}

Status PersistentDBImpl::RunFlusher() {
  const uint64_t batch_t0 = EnvOf()->NowMicros();
  std::string payload;
  std::vector<Pending*> members;
  bool need_sync = false;
  SequenceNumber begin = 0;
  // M5.2：本批覆盖的 **entry 总数**（= Σ p->entry_count）。phase C 的 durable 水位与
  // log_last_appended_seq 必须用它，而不是写者数（M5-A14 实测的 I52/I54 回归）。
  uint32_t total_count = 0;
  Status reject;

  // 取批**之前**的观察点：此处不持锁，其他写者仍可入队（A20 的确定性屏障）
  if (options_.commit_hook != nullptr) options_.commit_hook->OnBeforeGroupAssemble();

  // M3 §6.5 的写者停等：在取批前做，且**不持 commit_mu_** —— 这样 Close() 仍能拿到
  // commit_mu_ 置 closed_ 并 notify bg_cv_，不会与停等形成死锁（A25 的 ③/④）。
  WaitForImmutableCapacity();

  bool rotate_needed = false;
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    if (queue_.empty()) return Status::OK();               // 已被别的 flusher 处理
    DbMutexGuard ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）
    // **先取批，再决定拒绝**：无论后面是否拒绝，都必须把成员从队列摘出来并在下面结算。
    // 否则被拒的队首会永远留在队列里，后面的写者永远等不到自己成为队首（实测：A31 挂住）。
    size_t bytes = 0;
    begin = last_sequence_ + 1;
    for (Pending* p : queue_) {
      if (!members.empty() &&
          (members.size() >= kMaxGroupRecs || bytes + p->entry_bytes > kMaxGroupBytes)) {
        break;                                             // 单个超大 value 会独占一批，不饿死别人
      }
      // M5.2（I52）：sequence 按 **entry** 数推进，不再是「按写者数」。
      p->begin = begin + static_cast<SequenceNumber>(total_count);
      members.push_back(p);
      bytes += p->entry_bytes;
      total_count += p->entry_count;
      need_sync = need_sync || p->need_sync;               // D3：sync 取组内 OR（优于 LevelDB 只看队首）
    }
    for (size_t i = 0; i < members.size(); ++i) queue_.pop_front();
    // §5.4 第 2 步：容量预检的 footprint 按 entry 数折算节点开销（entry_count == 1 时与 M4.3 逐字相同）。
    size_t footprint = 0;
    for (const Pending* p : members) {
      footprint += p->entry_bytes + static_cast<size_t>(p->entry_count) * kMemTableNodeOverhead;
    }
    if (closed_) {
      reject = Status::IOError("Put/Delete: DB is closed", dbname_);
    } else if (!bg_error_.ok()) {
      reject = bg_error_;
    } else if (begin + static_cast<SequenceNumber>(total_count) - 1 > kMaxSequenceNumber) {
      // §5.3：sequence 空间耗尽 ⇒ 整批拒绝（kInvalidArgument），**不写 WAL、不碰内存**。
      reject = Status::InvalidArgument("RunFlusher", "sequence space exhausted");
    } else {
      // §6.2：容量不足**不再**是写的失败原因。冻结在锁内、纯内存；旧表进 immutables_ 时
      // **保留它自己的 log_number**（§6.6.2），新表容量由 NewTableCapacity 保证本批不可能 kFrozen。
      const size_t new_cap =
          std::max(options_.write_buffer_size, footprint + kMemTableNodeOverhead);
      if (memtable_->WouldReject(footprint)) {
        if (memtable_->NumEntries() > 0) {
          std::shared_ptr<Immutable> imm(new Immutable());
          imm->mem = memtable_;
          imm->log_number = memtable_log_number_;
          immutables_.push_back(imm);
          memtable_ = std::make_shared<MemTable>(internal_comparator_, new_cap);
          memtable_log_number_ = log_number_;   // 轮转前暂记；A' 轮转成功后更新为新 log（§6.6.2）
          log_sealed_ = true;                   // 当前 log 封口，本批起必须轮转
          need_rotate_ = true;
          rotate_in_progress_ = true;           // 轮转完成前不许 flush：min_log_to_keep 需要新 log 号
          // 注意：**不在这里 notify** —— 见阶段 A' 之后；否则后台线程会用旧 log 号算 min_keep，
          // 导致刚被 flush 覆盖的旧 log 逃过回收（M3-A35 实测偶发 records_replayed > 0）。
        } else {
          // 空表：不必制造空 SSTable，直接换一个容量足够的新表（当前 log 未封口，无需轮转）。
          memtable_ = std::make_shared<MemTable>(internal_comparator_, new_cap);
          memtable_log_number_ = log_number_;
        }
      }
      rotate_needed = log_sealed_ || need_rotate_;
    }
  }

  if (!reject.ok()) {
    std::lock_guard<std::mutex> ql(commit_mu_);
    for (Pending* p : members) {
      p->status = reject;
      p->done = true;
    }
    commit_cv_.notify_all();
    return reject;
  }

  if (options_.commit_hook != nullptr) options_.commit_hook->OnGroupTaken();

  // ---- 阶段 A'（§6.6.1）：轮转在**分配 sequence 之前**；失败 ⇒ 整批拒绝、不置 bg_error_（I33）----
  if (rotate_needed) {
    const Status rs = RotateLog();
    if (!rs.ok()) {
      {
        DbMutexGuard ml(mutex_);
        ++flush_stats_.rotate_failed;
        flush_stats_.last_error = rs.ToString();
        rotate_in_progress_ = false;                        // 轮转失败也必须解封后台线程（imm 仍需 flush）
        bg_cv_.notify_all();
      }
      std::lock_guard<std::mutex> ql(commit_mu_);
      for (Pending* p : members) {
        p->status = rs;                                     // 整批同一个 Status（I16/I33）
        p->done = true;
      }
      commit_cv_.notify_all();
      return rs;
    }
    // 轮转成功后：new log 号已写进 log_number_/memtable_log_number_，此时才允许后台 flush。
    DbMutexGuard ml(mutex_);
    rotate_in_progress_ = false;
    bg_cv_.notify_all();
  }

  // ---- 阶段 B（持 commit_mu_ → mutex_）：分配 sequence + 组 payload（§6.6.1）----
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    DbMutexGuard ml(mutex_);
    begin = last_sequence_ + 1;
    // M5.2（I52）：组内 sequence 按 entry 连续分配；一个 WriteBatch 的第 j 条 = p->begin + j。
    uint32_t offset = 0;
    for (Pending* p : members) {
      p->begin = begin + static_cast<SequenceNumber>(offset);
      offset += p->entry_count;
    }
    last_sequence_ = begin + static_cast<SequenceNumber>(offset) - 1;
    if (snapshots_.empty()) smallest_snapshot_ = last_sequence_;
    // M4.3：写放大分子（user_logical）与前台延迟样本（FRONT 行）。
    // M5.2：口径按 **entry** 计；单条写时（entry_count == 1、user_bytes == key+value）
    // 与 M4.3 的落地公式逐字相同，既有 AMPL 数字不变。
    for (const Pending* p : members) {
      user_logical_bytes_ += p->user_bytes;
      entry_bytes_ += p->user_bytes + 16ull * static_cast<uint64_t>(p->entry_count);
    }
    put_ops_ += offset;
    front_samples_us_.push_back(EnvOf()->NowMicros() - batch_t0);
    if (front_samples_us_.size() > 20000) front_samples_us_.erase(front_samples_us_.begin());
    payload = EncodeGroup(begin, members);
  }

  // ---- 阶段 C（锁外做 IO，I17/L7）：Append 永远做，fsync 只在组内有人要求时做（I11/D3）----
  Status s = log_->Append(Slice(payload));
  if (s.ok()) {
    // I32 修复：Append 返回 kOk 即字节已交给文件（fsync 只决定是否落到介质），所以这个边界
    // 只取决于 Append 的结果，与本次是否 fsync 无关。
    DbMutexGuard ml(mutex_);
    log_last_appended_seq_ = begin + static_cast<SequenceNumber>(total_count) - 1;
  }
  if (s.ok() && need_sync) s = log_->Sync();
  if (s.ok()) {
    if (options_.commit_hook != nullptr) options_.commit_hook->OnAfterSyncBeforePublish();
    std::lock_guard<std::mutex> ql(commit_mu_);
    // 阻断项 2 的修复：**只有真的 fsync 过**才推进 durable 水位（design §7.1 用它论证 I11：
    // w.status.ok() ⟹ durable_seq_ >= w.end_seq）。原来无条件推进，会让只写不 fsync 的批
    // 也把水位抬高，这条推理链就断了（M3 若拿它当 durable 水位会被误导）。
    if (need_sync) {
      durable_seq_ = begin + static_cast<SequenceNumber>(total_count) - 1;
    }
  } else {
    DbMutexGuard ml(mutex_);
    bg_error_ = s;                                         // 粘性：偏移已不可信（D11）；I16 传播给整批
  }

  if (s.ok()) {
    DbMutexGuard ml(mutex_);
    for (Pending* p : members) {
      // §5.4 第 3~4 步：容量预检已在取批时用 footprint 做过，逐条 Add 不可能触顶（kFrozen）。
      // entries 已在入队前由 WriteBatch::Validate 校验过，这里的解析不会失败（失败即 bg_error_）。
      Slice input(p->entries);
      for (uint32_t j = 0; j < p->entry_count; ++j) {
        BatchEntry e;
        if (!ParseOneEntry(&input, &e)) {
          const Status bad = Status::Corruption("RunFlusher", "batch entry 解析失败");
          bg_error_ = bad;
          s = bad;
          break;
        }
        const Status a =
            memtable_->Add(p->begin + static_cast<SequenceNumber>(j), e.type, e.key, e.value);
        if (!a.ok()) {
          bg_error_ = a;
          s = a;
          break;
        }
      }
      if (!s.ok()) break;
    }
  }

  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    for (Pending* p : members) {
      p->status = s;                                       // 同一批拿到**同一个** Status（I16）
      p->done = true;
    }
    commit_cv_.notify_all();
  }
  return s;
}

Status PersistentDBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  return WriteEntry(kTypeValue, options, key, value);
}

Status PersistentDBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return WriteEntry(kTypeDeletion, options, key, Slice());
}

Status PersistentDBImpl::Get(const Slice& key, std::string* value) {
  return GetAtSnapshot(nullptr, key, value);
}

Status PersistentDBImpl::GetAtSnapshot(const Snapshot* snapshot, const Slice& key,
                                       std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("PersistentDBImpl::Get: null value pointer");
  value->clear();
  SequenceNumber seq = 0;
  {
    DbMutexGuard l(mutex_);
    seq = (snapshot != nullptr) ? snapshot->sequence : last_sequence_;
  }
  DbReadStats delta;
  const uint64_t op_t0 = EnvOf()->NowMicros();
  const Status s = GetInternal(key, seq, value, &delta);
  MergeReadStats(delta);
  {
    DbMutexGuard l(mutex_);
    ++get_count_;
    front_samples_us_.push_back(EnvOf()->NowMicros() - op_t0);
    if (front_samples_us_.size() > 20000) front_samples_us_.erase(front_samples_us_.begin());
  }
  return s;
}

Status PersistentDBImpl::GetInternal(const Slice& key, SequenceNumber snapshot,
                                       std::string* value, DbReadStats* delta) {
  std::shared_ptr<const MemTable> mt;
  std::vector<std::shared_ptr<const MemTable>> imms;
  std::shared_ptr<const Version> ver;
  {
    DbMutexGuard l(mutex_);
    mt = memtable_;
    imms.reserve(immutables_.size());
    for (const std::shared_ptr<Immutable>& im : immutables_) imms.push_back(im->mem);
    ver = version_;
    if (ver != nullptr) ver->Ref();   // I42/L23：读路径拿到版本即 Ref
  }
  struct VerRef {
    const Version* v = nullptr;
    ~VerRef() {
      if (v != nullptr) v->Unref();
    }
  } ver_ref{ver.get()};

  const std::string lookup_key = BuildLookupKey(key, snapshot);
  const auto lookup_mem = [&](const std::shared_ptr<const MemTable>& m, HitLayer layer) -> int {
    std::string tmp;
    switch (m->Get(Slice(lookup_key), &tmp)) {
      case MemTable::GetResult::kFound:
        *value = std::move(tmp);
        delta->hit_layer = layer;
        return 1;
      case MemTable::GetResult::kDeleted:
        delta->hit_layer = layer;
        return -1;
      case MemTable::GetResult::kNotFound:
        return 0;
    }
    return 0;
  };

  int r = lookup_mem(mt, HitLayer::kMemTable);
  if (r == 1) return Status::OK();
  if (r == -1) return Status::NotFound("PersistentDBImpl::Get: key is deleted in memtable");
  for (auto it = imms.rbegin(); it != imms.rend(); ++it) {
    r = lookup_mem(*it, HitLayer::kImmutable);
    if (r == 1) return Status::OK();
    if (r == -1) return Status::NotFound("PersistentDBImpl::Get: key is deleted in immutable");
  }

  if (ver != nullptr) {
    const Comparator* user_cmp = internal_comparator_.user_comparator();
    // §7.1：L0 逐个检查（新→旧，命中即终局）；L1+ 层内按 smallest 升序，可用 key range 过滤。
    for (int level = 0; level < kNumLevels; ++level) {
      const std::vector<FileMetaData>& files = ver->level_files(level);
      for (const FileMetaData& f : files) {
        Slice min_user;
        Slice max_user;
        SequenceNumber tmp_seq = 0;
        ValueType tmp_type = kTypeValue;
        if (!ParseInternalKey(Slice(f.smallest), &min_user, &tmp_seq, &tmp_type) ||
            !ParseInternalKey(Slice(f.largest), &max_user, &tmp_seq, &tmp_type)) {
          return Status::Corruption("PersistentDBImpl::Get", "file metadata key range is malformed");
        }
        if (user_cmp->Compare(key, min_user) < 0) {
          ++delta->key_range_skipped;
          if (level > 0) break;   // L1+ 层内有序：后面的文件只会更大
          continue;
        }
        if (user_cmp->Compare(key, max_user) > 0) {
          ++delta->key_range_skipped;
          continue;
        }
        ++delta->files_checked;
        std::string file_value;
        TableGetResult tr = TableGetResult::kNotFound;
        ReadStats tstats;
        bool opened = false;
        const Status fs =
            table_cache_->Get(f, Slice(lookup_key), &file_value, &tr, &tstats, &opened);
        if (opened) ++delta->index_blocks_read;
        delta->data_blocks_read += tstats.data_blocks_read;
        delta->blocks_read += tstats.blocks_read;
        delta->bytes_read += tstats.bytes_read;
        delta->crc_checked += tstats.crc_checked;
        delta->crc_failed += tstats.crc_failed;
        // M5.1（§3.6/L34）：filter 的 8 个计数从**线程局部**的 tstats 汇总进本次 Get 的 delta；
        // delta 在 Get 尾部经 MergeReadStats 在 mutex_ 下累加 ⇒ 读路径无共享自增。
        delta->filter_checked += tstats.filter_checked;
        delta->filter_negative += tstats.filter_negative;
        delta->filter_positive += tstats.filter_positive;
        delta->filter_unavailable += tstats.filter_unavailable;
        delta->filter_blocks_read += tstats.filter_blocks_read;
        delta->filter_bytes_read += tstats.filter_bytes_read;
        delta->data_blocks_skipped_by_filter += tstats.data_blocks_skipped_by_filter;
        delta->filter_corrupt += tstats.filter_corrupt;
        if (!fs.ok()) return fs;
        if (tr == TableGetResult::kFound) {
          *value = std::move(file_value);
          delta->hit_layer = HitLayer::kSSTable;
          return Status::OK();
        }
        if (tr == TableGetResult::kDeleted) {
          delta->hit_layer = HitLayer::kSSTable;
          return Status::NotFound("PersistentDBImpl::Get: key is deleted in sstable");
        }
        if (level == 0) continue;   // L0 允许重叠：必须逐个检查
        break;                      // L1+ 层内互斥：查完这一个文件即可
      }
    }
  }

  delta->hit_layer = HitLayer::kNone;
  return Status::NotFound("PersistentDBImpl::Get: key not found");
}

void PersistentDBImpl::MergeReadStats(const DbReadStats& delta) {
  DbMutexGuard l(mutex_);
  read_stats_.files_checked += delta.files_checked;
  read_stats_.key_range_skipped += delta.key_range_skipped;
  read_stats_.index_blocks_read += delta.index_blocks_read;
  read_stats_.data_blocks_read += delta.data_blocks_read;
  read_stats_.blocks_read += delta.blocks_read;
  read_stats_.bytes_read += delta.bytes_read;
  read_stats_.crc_checked += delta.crc_checked;
  read_stats_.crc_failed += delta.crc_failed;
  // M5.1（L34）：filter 计数在线程局部 delta 里累好后，只在这里（持 mutex_）落进全局 read_stats_。
  read_stats_.filter_checked += delta.filter_checked;
  read_stats_.filter_negative += delta.filter_negative;
  read_stats_.filter_positive += delta.filter_positive;
  read_stats_.filter_unavailable += delta.filter_unavailable;
  read_stats_.filter_blocks_read += delta.filter_blocks_read;
  read_stats_.filter_bytes_read += delta.filter_bytes_read;
  read_stats_.data_blocks_skipped_by_filter += delta.data_blocks_skipped_by_filter;
  read_stats_.filter_corrupt += delta.filter_corrupt;
  last_hit_layer_ = delta.hit_layer;
}

DbReadStats PersistentDBImpl::GetReadStats() const {
  DbMutexGuard l(mutex_);
  DbReadStats out = read_stats_;
  out.hit_layer = last_hit_layer_;
  return out;
}

FlushStats PersistentDBImpl::GetFlushStats() const {
  DbMutexGuard l(mutex_);
  return flush_stats_;
}

size_t PersistentDBImpl::immutables_size() const {
  DbMutexGuard l(mutex_);
  return immutables_.size();
}

Iterator* PersistentDBImpl::NewIterator() {
  SequenceNumber snapshot = 0;
  {
    DbMutexGuard l(mutex_);
    snapshot = last_sequence_;
  }
  return BuildIterator(snapshot);
}

Iterator* PersistentDBImpl::NewIteratorAtSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return NewIterator();
  return BuildIterator(snapshot->sequence);
}

Iterator* PersistentDBImpl::BuildIterator(SequenceNumber snapshot) {
  std::shared_ptr<const MemTable> mt;
  std::vector<std::shared_ptr<const MemTable>> imms;
  std::shared_ptr<const Version> ver;
  {
    DbMutexGuard l(mutex_);
    mt = memtable_;
    imms.reserve(immutables_.size());
    for (const std::shared_ptr<Immutable>& im : immutables_) imms.push_back(im->mem);
    ver = version_;
    if (ver != nullptr) ver->Ref();
  }
  // 迭代器必须同时：(a) 持住 Version 的内存（owning shared_ptr），(b) 在析构时 Unref，
  // 且 Unref 必须在 own 释放**之前**执行（先 Unref 再析构成员）——否则会 use-after-free。
  struct VersionRefHolder {
    std::shared_ptr<const Version> v;
    ~VersionRefHolder() {
      if (v != nullptr) v->Unref();
    }
  };

  std::vector<std::shared_ptr<const void>> refs;
  refs.push_back(mt);
  for (const std::shared_ptr<const MemTable>& m : imms) refs.push_back(m);
  if (ver != nullptr) {
    refs.push_back(std::shared_ptr<const void>(new VersionRefHolder{ver}));
  }

  std::vector<Iterator*> kids_vec;
  kids_vec.push_back(mt->NewIterator());
  for (auto it = imms.rbegin(); it != imms.rend(); ++it) kids_vec.push_back((*it)->NewIterator());
  if (ver != nullptr) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (const FileMetaData& f : ver->level_files(level)) {
        std::unique_ptr<Iterator> child = table_cache_->NewIterator(f, nullptr);
        kids_vec.push_back(child.release());
      }
    }
  }
  const int n = static_cast<int>(kids_vec.size());
  Iterator** kids = new Iterator*[n];
  for (int i = 0; i < n; ++i) kids[i] = kids_vec[static_cast<size_t>(i)];
  MergingIterator* merged = new MergingIterator(&internal_comparator_, kids, n);
  return new DBIter(&internal_comparator_, merged, snapshot, std::move(refs));
}

void PersistentDBImpl::RunHoldingDbMutexForTest(const std::function<void()>& fn) {
  DbMutexGuard l(mutex_);
  fn();
}

Status PersistentDBImpl::ForceFlushForTest() {
  uint64_t target = 0;
  bool enqueued = false;
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    DbMutexGuard ml(mutex_);
    if (!bg_error_.ok()) return bg_error_;
    if (closed_) return Status::IOError("ForceFlushForTest: DB is closed", dbname_);
    target = flush_stats_.flushes_completed + 1;
    if (memtable_->NumEntries() > 0) {
      std::shared_ptr<Immutable> imm(new Immutable());
      imm->mem = memtable_;
      imm->log_number = memtable_log_number_;
      immutables_.push_back(imm);
      memtable_ = std::make_shared<MemTable>(internal_comparator_, options_.write_buffer_size);
      memtable_log_number_ = log_number_;
      log_sealed_ = true;
      need_rotate_ = true;
      rotate_in_progress_ = true;   // 轮转完成前不许 flush（与 RunFlusher 同一时序纪律）
      enqueued = true;
    } else if (!log_sealed_) {
      // 空表：仍要把当前 log 轮转成空文件（"关库时当前 log 为空"的契约）。
      log_sealed_ = true;
      need_rotate_ = true;
    }
  }
  const Status rs = RotateLog();
  {
    DbMutexGuard ml(mutex_);
    rotate_in_progress_ = false;
    if (enqueued || rs.ok()) bg_cv_.notify_all();
  }
  if (!rs.ok()) return rs;
  if (!enqueued) return Status::OK();
  for (int i = 0; i < 8000000; ++i) {
    {
      DbMutexGuard ml(mutex_);
      if (!bg_error_.ok()) return bg_error_;
      if (flush_stats_.flushes_completed >= target && immutables_.empty()) return Status::OK();
    }
    std::this_thread::yield();
  }
  return Status::IOError("ForceFlushForTest: timeout waiting for flush");
}

// A25 探针的访问器：必须定义在 namespace lsm 正体（外部链接），与 db_impl.h 的声明匹配
bool DbMutexHeldOnThisThread() { return g_db_mutex_held; }

bool InstallMuHeldOnThisThread() { return g_install_mu_held; }

size_t PersistentDBImpl::pending_writers() {
  std::lock_guard<std::mutex> ql(commit_mu_);
  return queue_.size();
}

Status PersistentDBImpl::Sync() {
  // 与 flusher 的 fsync 串行；语义 = 把此前全部已返回 kOk 的写刷到磁盘（design §7.4）
  std::lock_guard<std::mutex> ql(commit_mu_);
  if (log_ == nullptr) return Status::IOError("PersistentDBImpl::Sync: WAL is not open", dbname_);
  {
    DbMutexGuard ml(mutex_);                // M2-I35：bg_error_ 只在 mutex_ 下读
    if (!bg_error_.ok()) return bg_error_;
  }
  // I32 修复：**先**取「已实际追加的边界」，**再** fsync。fsync 只能覆盖调用之前已写进文件的
  // 字节，所以水位只能发布到这个快照；并发在飞批次（sequence 已分配、Append 尚未执行）不得被
  // 声称 durable。代价是可能少报（保守），绝不许多报 —— 契约仍然成立：任何在本次 Sync 之前
  // 已返回 kOk 的写，其 Append 必然早于本次快照点，因而被这次 fsync 覆盖。
  SequenceNumber appended_before_fsync = 0;
  {
    DbMutexGuard ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）；持锁期间不做 IO（I17）
    appended_before_fsync = log_last_appended_seq_;
  }
  const Status s = log_->Sync();
  if (s.ok()) {
    DbMutexGuard ml(mutex_);
    // 单调发布：flusher 的水位发布与本次同受 commit_mu_ 串行，且 appended_seq_ 单调不减。
    if (appended_before_fsync > durable_seq_) durable_seq_ = appended_before_fsync;
  }
  return s;
}

void PersistentDBImpl::StartBackgroundThread() {
  {
    DbMutexGuard l(mutex_);
    if (bg_started_) return;
    bg_started_ = true;
    bg_stop_ = false;
  }
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
}

void PersistentDBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    bg_cv_.wait(l, [this] {
      return bg_stop_ || (!immutables_.empty() && !rotate_in_progress_);
    });
    // §6.5：Close() 置 bg_stop_ 后必须**放弃**剩余 immutables 并退出（数据仍在 WAL + 内存，
    // 由 Close 计数 immutables_abandoned）；不能继续循环，否则会在 closed_ 下空转、join 挂死。
    if (bg_stop_) break;
    if (immutables_.empty()) continue;
    std::shared_ptr<Immutable> imm = immutables_.front();   // 不弹出；成功注册后才 pop（§6.3）
    l.unlock();
    FlushImmutable(imm);
    l.lock();
    if (!bg_error_.ok()) {
      // fail-stop 是粘性的：不再重试（否则忙等），等 Close() 置 bg_stop_ 后退出。
      bg_cv_.wait(l, [this] { return bg_stop_; });
      break;
    }
  }
}

uint64_t PersistentDBImpl::RecomputeMinLogNumberToKeepLocked(const Immutable* exclude) const {
  // §6.6.2 的单一真相源：pending = {memtable_} ∪ immutables_（exclude = 正在注册的那张表）。
  uint64_t m = memtable_log_number_;
  for (const std::shared_ptr<Immutable>& im : immutables_) {
    if (im.get() == exclude) continue;
    if (im->log_number < m) m = im->log_number;
  }
  return m == 0 ? 1 : m;
}

Status PersistentDBImpl::CreateEmptyLogFile(uint64_t number) {
  const std::string path = LogFileName(dbname_, number);
  WritableFile* raw = nullptr;
  Status s = EnvOf()->NewWritableFile(path, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Close();
  }
  if (!s.ok()) return s;
  return EnvOf()->SyncDir(dbname_);   // §6.6.1 阶段 A'：目录项 durable（R4：只证明调用顺序）
}

Status PersistentDBImpl::RotateLog() {
  // 前置：只有当前 flusher 会走到这里（L20），且它已把当前 log 封口（log_sealed_）。
  uint64_t new_number = 0;
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    SequenceNumber appended = 0;
    {
      DbMutexGuard ml(mutex_);
      appended = log_last_appended_seq_;
      // §3.1：`.log` 与 `.sst` **共享**一个单调递增的 next_file_number ⇒ 轮转也必须从
      // 这个分配器取号（不能写死 log_number_+1：恢复后 next 可能已经更大，写死会重用编号）。
      new_number = next_file_number_;
      if (new_number <= log_number_) new_number = log_number_ + 1;
      next_file_number_ = new_number + 1;
    }
    // I32 的 per-log 边界（M3-A50）：先把旧 log 的已 Append 字节落盘，再发布水位、再换文件。
    // 否则 Sync() 在新 log 上无法覆盖旧 log 的未 fsync 字节，就会多报 durable。
    const Status s = log_->Sync();
    if (!s.ok()) return s;
    if (appended > durable_seq_) durable_seq_ = appended;
  }
  // 锁外建文件（§6.6.1 阶段 A' 的 IO 窗口）。
  Status s = CreateEmptyLogFile(new_number);
  if (!s.ok()) return s;
  std::unique_ptr<WALWriter> fresh(new WALWriter(EnvOf(), LogFileName(dbname_, new_number)));
  s = fresh->Open(false);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    log_ = std::move(fresh);
    DbMutexGuard ml(mutex_);
    log_number_ = new_number;
    // §6.6.2 的**精确**口径：当前 memtable 尚未写过任何字节（封口时已把旧表移入 immutables_），
    // 它的第一批写入必然落在新 log ⇒ 记录的"最早写入所在 log"必须更新为新编号。
    // 旧实现保留轮转前的编号（设计原文的"保守"说法）会让"刚被 flush 覆盖的那个 log"
    // 永远 >= min_log_to_keep ⇒ 永远删不掉 ⇒ 重开必然重放（M3-A35 红、WAL 永不回收）。
    memtable_log_number_ = new_number;
    log_sealed_ = false;
    need_rotate_ = false;
    ++flush_stats_.rotations;
  }
  return Status::OK();
}

void PersistentDBImpl::RecycleObsoleteLogs() {
  if (!options_.recycle_log_files) return;   // M3-A48：关闭时一个 *.log 都不删
  uint64_t min_keep = 1;
  uint64_t current = 0;
  {
    DbMutexGuard l(mutex_);
    min_keep = version_ == nullptr ? 1 : version_->min_log_number_to_keep();
    current = log_number_;
  }
  std::vector<std::string> children;
  if (!EnvOf()->GetChildren(dbname_, &children).ok()) return;
  uint64_t removed = 0;
  uint64_t bytes = 0;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (!ParseLogFileName(c, &n)) continue;
    if (n == 0 || n >= min_keep || n == current) continue;   // 严格小于才可删；当前 log 永不删
    const std::string path = LogFileName(dbname_, n);
    uint64_t size = 0;
    EnvOf()->GetFileSize(path, &size);
    if (EnvOf()->RemoveFile(path).ok()) {
      ++removed;
      bytes += size;
    }
  }
  if (removed > 0) {
    DbMutexGuard l(mutex_);
    flush_stats_.log_files_deleted += removed;
    flush_stats_.log_bytes_deleted += bytes;
  }
}

void PersistentDBImpl::FlushImmutable(const std::shared_ptr<Immutable>& imm) {
  uint64_t number = 0;
  {
    DbMutexGuard l(mutex_);
    if (!bg_error_.ok() || closed_) return;
    ++flush_stats_.flushes_started;
    number = next_file_number_++;
  }
  const std::string tmp = TempFileName(dbname_, number);
  const std::string final = TableFileName(dbname_, number);

  Status s = Status::OK();
  uint64_t index_warn = 0;
  FileMetaData meta;
  meta.number = number;
  {
    WritableFile* raw = nullptr;
    s = EnvOf()->NewWritableFile(tmp, &raw);
    if (s.ok()) {
      std::unique_ptr<WritableFile> file(raw);
      TableBuilder builder(options_, file.get());
      std::unique_ptr<Iterator> it(imm->mem->NewIterator());
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        s = builder.Add(it->key(), it->value());
        if (!s.ok()) break;
      }
      if (s.ok() && !it->status().ok()) s = it->status();
      if (s.ok()) s = builder.Finish();
      if (s.ok()) {
        if (options_.flush_hook != nullptr) options_.flush_hook->OnSSTableWritten();
        s = file->Sync();   // ★ 步骤 ④ 的 fsync（I22）
      }
      const Status close_status = file->Close();
      if (s.ok() && !close_status.ok()) s = close_status;
      if (s.ok()) {
        if (options_.flush_hook != nullptr) options_.flush_hook->OnBeforeRename();
        s = EnvOf()->RenameFile(tmp, final);   // ★ 步骤 ⑤
      }
      if (s.ok()) s = EnvOf()->SyncDir(dbname_);   // ★ 步骤 ⑥（R4：只证明调用顺序）
      if (s.ok()) {
        meta.file_size = builder.FileSize();
        meta.max_sequence = builder.MaxSequence();
        meta.smallest = builder.smallest();
        meta.largest = builder.largest();
        index_warn = builder.index_size_warn_count();
      }
    }
  }

  if (!s.ok()) {
    EnvOf()->DeleteFile(tmp);   // 未注册的 tmp 尽力删除；final 若存在则是未注册孤儿（M3.3 清理）
    DbMutexGuard l(mutex_);
    bg_error_ = s;              // 粘性 fail-stop（§6.4）
    ++flush_stats_.flushes_failed;
    flush_stats_.last_error = s.ToString();
    bg_cv_.notify_all();
    return;
  }

  if (options_.flush_hook != nullptr) options_.flush_hook->OnBeforeRegister();

  // ⑦ min_log_to_keep（单一真相源）→ VersionEdit → LogAndApply（先落盘后安装，§8.1）。
  VersionEdit edit;
  {
    DbMutexGuard l(mutex_);
    if (!bg_error_.ok() || closed_) return;   // 与 Close/失败的竞态：不注册，数据仍在 WAL+内存
    const uint64_t min_keep = RecomputeMinLogNumberToKeepLocked(imm.get());
    edit.SetComparatorName(options_.comparator->Name());
    edit.SetLogNumber(log_number_);
    edit.SetMinLogNumberToKeep(min_keep);
    edit.SetNextFileNumber(next_file_number_);
    edit.AddFile(0, meta);
  }
  std::shared_ptr<const Version> snapshot;
  const Status ms = LogAndApply(edit, nullptr, &snapshot);
  if (!ms.ok()) {
    DbMutexGuard l(mutex_);
    bg_error_ = ms;              // 粘性 fail-stop（§6.6 的"写/rename MANIFEST"行）
    ++flush_stats_.flushes_failed;
    flush_stats_.last_error = ms.ToString();
    bg_cv_.notify_all();
    return;   // imm 留在 immutables_；WAL **未删**（I34）；没有半成品被注册
  }
  (void)snapshot;

  // ⑦c 注册完成（MANIFEST 已 durable）：弹出 imm + 计数。
  {
    DbMutexGuard l(mutex_);
    if (!immutables_.empty() && immutables_.front() == imm) immutables_.pop_front();
    ++flush_stats_.flushes_completed;
    flush_stats_.index_size_warn += index_warn;
    flush_write_bytes_ += meta.file_size;
    bg_cv_.notify_all();
    compact_cv_.notify_all();   // immutables_ 变为空 ⇒ 唤醒 compaction（谓词含 immutables_.empty()）
  }

  MaybeScheduleCompaction();   // 层级分支：flush 完成后才考虑 compaction（flush 优先，L27）
  // ⑨ WAL 回收（§6.3/§6.6.2）：**只在 META 的 rename + SyncDir 之后**执行（I34）。
  RecycleObsoleteLogs();
}

Status PersistentDBImpl::Close() {
  std::unique_lock<std::mutex> ql(commit_mu_);
  bool already_closed = false;
  {
    DbMutexGuard l(mutex_);
    already_closed = closed_;
    closed_ = true;                                        // 幂等（A30）
    // §6.5：立即唤醒可能停在 WaitForImmutableCapacity 的写者（谓词含 !closed_）；
    // 否则 Close 会卡在 commit_cv_ 上等 flusher 结束，而 flusher 正等这个通知 ⇒ 死锁。
    bg_cv_.notify_all();
  }
  if (already_closed) {
    // 幂等路径也必须确保 LOCK 已释放（恢复期失败可能留下未释放的锁）
    if (file_lock_ != nullptr) EnvOf()->UnlockFile(file_lock_.release());
    return Status::OK();
  }
  // L11/A31 + 阻断项 6：必须等「没有在途 flusher」**且「队列已排空」**。
  // 只看 !flusher_active_ 会漏掉"批边界之外被留在队列里的写者"——它随后仍会成为 flusher，
  // 若调用方按惯例 Close() 后立刻 delete db，就会落在已析构对象上（UAF 窗口）。
  commit_cv_.wait(ql, [this] { return !flusher_active_ && queue_.empty(); });
  if (log_ == nullptr) {                                   // 尚未打开 WAL（恢复中途失败）
    if (file_lock_ != nullptr) EnvOf()->UnlockFile(file_lock_.release());
    return Status::OK();
  }
  // M4.2/L29：先停 compaction 线程并 join，再停 flush 线程（compaction 先 join）。
  {
    DbMutexGuard ml(mutex_);
    compact_stop_ = true;
    compact_cv_.notify_all();
  }
  if (compaction_thread_.joinable()) compaction_thread_.join();
  // §6.5 的关闭顺序：置 bg_stop_ → join（必须在关 log/释放对象之前）。
  {
    DbMutexGuard ml(mutex_);
    bg_stop_ = true;
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();
  {
    DbMutexGuard ml(mutex_);
    flush_stats_.immutables_abandoned += immutables_.size();   // 放弃必须计数（§6.4 表末行）
  }
  MaybeDeleteObsoleteFiles();   // L29：关闭时处理延迟删除队列（失败只计数）
  Status sticky;                                        // M2-I35：bg_error_ 只在 mutex_ 下读
  {
    DbMutexGuard ml(mutex_);
    sticky = bg_error_;
  }
  Status s = sticky.ok() ? log_->Sync() : sticky;        // Close 隐含 Sync（I20/A30）
  const Status c = log_->Close();
  if (file_lock_ != nullptr) EnvOf()->UnlockFile(file_lock_.release());   // D10：释放独占
  return s.ok() ? c : s;
}

Status PersistentDBImpl::RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr) {
  // ---- §8.5 ①：Options 合法性校验。全部 kInvalidArgument，**不**置 bg_error_（M2 教训 4）----
  if (options.comparator == nullptr) {
    return Status::InvalidArgument("DB::Open: comparator == nullptr");
  }
  if (options.write_buffer_size == 0) {
    return Status::InvalidArgument("DB::Open: write_buffer_size == 0");
  }
  if (options.block_size < 512 || options.block_size > 1024 * 1024) {
    return Status::InvalidArgument("DB::Open: block_size 越界", std::to_string(options.block_size));
  }
  if (options.max_open_files == 0 || options.max_open_files > 1000000) {
    return Status::InvalidArgument("DB::Open: max_open_files 越界",
                                   std::to_string(options.max_open_files));
  }
  // M4（§5.6）：6 个新字段的合法性校验，非法 ⇒ kInvalidArgument（**不** fail-stop）。
  if (options.level0_file_num_compaction_trigger < 1) {
    return Status::InvalidArgument("DB::Open: level0_file_num_compaction_trigger < 1");
  }
  if (options.max_bytes_for_level_base == 0) {
    return Status::InvalidArgument("DB::Open: max_bytes_for_level_base == 0");
  }
  if (options.max_bytes_for_level_multiplier < 2) {
    return Status::InvalidArgument("DB::Open: max_bytes_for_level_multiplier < 2");
  }
  if (options.max_file_size == 0 || options.max_file_size < options.block_size) {
    return Status::InvalidArgument("DB::Open: max_file_size 非法（0 或 < block_size）");
  }

  // 注入的 Env（掉电语义测试用 MemEnv）；nullptr 时用真实 POSIX Env
  Env* env = options.env != nullptr ? options.env : Env::Default();
  if (!env->FileExists(name)) {
    const Status s = env->CreateDir(name);
    if (!s.ok()) return s;
    // §8.3 ②：新建目录后 SyncDir 其父目录（目录项 durable；R4：只证明调用顺序，不宣称掉电安全）。
    const size_t slash = name.find_last_of('/');
    std::string parent = ".";
    if (slash == 0) parent = "/";
    else if (slash != std::string::npos) parent = name.substr(0, slash);
    (void)env->SyncDir(parent);   // 失败只影响掉电语义（MemEnv 不建模目录项），不阻断 Open
  }

  // D10：进程级独占（评审阻断项 4 —— 用户已裁决纳入，但此前完全没接线）。
  // fcntl 写锁是**进程级**的：同一进程内二次 Open 不冲突，验证必须用双进程。
  FileLock* raw_lock = nullptr;
  const Status lock_status = env->LockFile(LockFileName(name), &raw_lock);
  if (!lock_status.ok()) return lock_status;      // 已被别的进程持有 ⇒ kIOError
  ScopedFileLock lock_guard(env, raw_lock);

  // ---- §8.3 ④⑤：版本元数据（META 或 §10.9 的兼容规则）----
  std::vector<std::string> children;
  Status s = env->GetChildren(name, &children);
  if (!s.ok()) return s;

  std::shared_ptr<const Version> version;
  RecoveryStats stats;
  VersionSet::ManifestReplayResult manifest_result;
  // M4（§3.6）：稳态元数据 = CURRENT + MANIFEST；
  // 只有"没有 CURRENT/MANIFEST、只有旧 META"时才做一次兼容读入 + 迁移（META 绝不作为稳态写入目标）。
  s = VersionSet::RecoverManifest(env, name, options, &version, &manifest_result);
  if (!s.ok()) return s;
  stats.manifest_present = manifest_result.manifest_present;
  stats.migrated_from_meta = manifest_result.migrated_from_meta;
  stats.manifest_number = manifest_result.manifest_number;
  stats.manifest_bytes = manifest_result.manifest_bytes;
  stats.manifest_edits_replayed = manifest_result.edits_replayed;
  stats.manifest_tail_truncated_bytes = manifest_result.tail_truncated_bytes;
  stats.unknown_manifest_record_types = manifest_result.unknown_record_types;
  stats.manifest_truncation_note = manifest_result.truncation_note;
  stats.meta_migrated = manifest_result.migrated_from_meta ? 1 : 0;
  stats.meta_delete_failed = manifest_result.meta_delete_failed;
  // meta_present 的语义（M2/M3 字段名保留）= "活动版本元数据存在"（M4 起即 manifest_present）。
  stats.meta_present = manifest_result.manifest_present;
  {
    // 活动元数据的语义校验（保留 M3-A38 的覆盖）：注册文件必须存在、file_size 与磁盘一致、
    // max_sequence 与全量扫描一致；不符 ⇒ kCorruption（不自动修复）。
    VersionSet::RecoveryResult vr;
    s = VersionSet::VerifyRegisteredFiles(env, name, options, *version, &vr);
    if (!s.ok()) return s;
    stats.sst_files_registered = vr.sst_files_registered;
    stats.sst_bytes_registered = vr.sst_bytes_registered;
    stats.max_sequence_in_files = vr.max_sequence_in_files;
    stats.unknown_metaindex_entries = vr.unknown_metaindex_entries;
  }

  // ---- §8.3 ⑥：孤儿清理（只清理可证明未被引用的；失败只计数不阻断）----
  // 判据全部写在**语义层**（*.sst.tmp / 未注册 *.sst / 编号 < min_log_to_keep 的 *.log），
  // 不绑定任何具体元数据文件名（M4 换 MANIFEST+CURRENT 时本段不变）。
  {
    std::set<uint64_t> registered;
    for (const FileMetaData& f : version->AllFiles()) registered.insert(f.number);
    const uint64_t min_keep = version->min_log_number_to_keep();
    const uint64_t cur_log = version->log_number();
    // 元数据临时文件永不权威：残留即删（它可能来自"写 META.tmp 之后、rename 之前"的崩溃）。
    // ⑥f：CURRENT/MANIFEST 已是权威 ⇒ 残留的 META / META.tmp 是迁移残片，删除并计数。
    if (env->FileExists(VersionSet::MetaTempFileName(name))) {
      if (env->RemoveFile(VersionSet::MetaTempFileName(name)).ok()) {
        ++stats.meta_delete_failed;   // 兼容字段：META 系删除计数（M4 语义 = 迁移残片清理）
      }
    }
    if (env->FileExists(VersionSet::MetaFileName(name))) {
      if (env->RemoveFile(VersionSet::MetaFileName(name)).ok()) {
        ++stats.meta_delete_failed;
      }
    }
    const auto remove_orphan = [&](const std::string& path, bool tmp_kind) {
      uint64_t size = 0;
      env->GetFileSize(path, &size);
      if (env->RemoveFile(path).ok()) {
        if (tmp_kind) {
          ++stats.orphan_tmp_removed;
        } else {
          ++stats.orphan_sst_removed;
        }
        stats.orphan_bytes_removed += size;
      } else {
        ++stats.orphan_remove_failed;
      }
    };
    const auto remove_counted = [&](const std::string& path, uint64_t* counter) {
      uint64_t size = 0;
      env->GetFileSize(path, &size);
      if (env->RemoveFile(path).ok()) {
        ++(*counter);
        stats.orphan_bytes_removed += size;
      } else {
        ++stats.orphan_remove_failed;
      }
    };
    for (const std::string& c : children) {
      uint64_t n = 0;
      if (ParseManifestTempFileName(c, &n)) {
        remove_counted(name + "/" + c, &stats.manifest_tmp_removed);   // ⑥c：模式 (a) 残片
        continue;
      }
      if (ParseManifestFileName(c, &n)) {
        if (n != manifest_result.manifest_number) {
          remove_counted(name + "/" + c, &stats.manifest_orphan_removed);   // ⑥e：被切换掉的旧 MANIFEST
        }
        continue;
      }
      if (c == "CURRENT.tmp") {
        remove_counted(name + "/" + c, &stats.current_tmp_removed);   // ⑥g：CURRENT 已存在 ⇒ 残片可删
        continue;
      }
      if (ParseTempFileName(c, &n)) {
        remove_orphan(name + "/" + c, true);        // ⑥a：*.sst.tmp 构造上永不注册
        continue;
      }
      if (ParseTableFileName(c, &n)) {
        if (registered.count(n) == 0) remove_orphan(name + "/" + c, false);   // ⑥b：未注册 *.sst
        continue;
      }
      if (ParseLogFileName(c, &n)) {
        if (!options.recycle_log_files) continue;   // ⑥c：受 recycle_log_files 控制（M3-A48）
        if (n == 0 || n >= min_keep) continue;      // 严格小于才可删（§6.6.2）
        if (n == cur_log) continue;                 // 当前 log 永不删
        uint64_t size = 0;
        env->GetFileSize(LogFileName(name, n), &size);
        if (env->RemoveFile(LogFileName(name, n)).ok()) {
          ++stats.obsolete_logs_removed;
          stats.obsolete_log_bytes_removed += size;
        } else {
          ++stats.orphan_remove_failed;
        }
      }
    }
  }

  // ---- §8.3 ⑦：重放集合 = 清理之后目录里**实际存在**的 *.log，按编号数值升序 ----
  children.clear();
  s = env->GetChildren(name, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseLogFileName(c, &n)) logs.push_back(n);
  }
  std::sort(logs.begin(), logs.end());
  const uint64_t hi = logs.empty() ? 0 : logs.back();
  stats.log_files = logs.size();

  // ---- 第一遍：只扫描并规划（§8.2），判定"尾部残骸 vs 中间损坏"（M2 §5.3 逐字）----
  struct Plan {
    uint64_t number = 0;
    std::vector<std::string> records;
    bool truncate = false;
    uint64_t truncate_at = 0;
  };
  std::vector<Plan> plans;
  uint64_t total_payload = 0;
  uint64_t total_entries = 0;

  for (uint64_t n : logs) {
    const std::string path = LogFileName(name, n);
    Plan p;
    p.number = n;
    WALReader reader(env, path);
    WALScanResult r;
    s = reader.ReadAll([&p](const Slice& rec) { p.records.push_back(rec.ToString()); }, &r);
    if (!s.ok()) return s;
    switch (r.verdict) {
      case WALScanVerdict::kClean:
        break;
      case WALScanVerdict::kTailResidue:
        if (n != hi) {
          return Status::Corruption("RecoverAndOpen: 非最高编号 log 的尾部残骸",
                                    path + " @" + std::to_string(r.failure_offset));
        }
        p.truncate = true;
        p.truncate_at = r.last_good_end;
        break;
      case WALScanVerdict::kParseFail:
        if (r.valid_record_after_failure) {
          return Status::Corruption("RecoverAndOpen: log 中间损坏（其后仍有完好 record）",
                                    path + " @" + std::to_string(r.failure_offset) + " " + r.detail);
        }
        if (n != hi) {
          return Status::Corruption("RecoverAndOpen: 非最高编号 log 的不可判定损坏",
                                    path + " @" + std::to_string(r.failure_offset));
        }
        p.truncate = true;
        p.truncate_at = r.last_good_end;
        break;
    }
    // 评审优化项（硬伤）：容量公式原来只按 WAL payload 估，漏了 MemTable 的每条目开销
    // （条目编码 + 跳表节点 ≈ +128~190 B/条，小 value 下放大可达 4.5×）——
    // 后果实测：用 64 MiB 写满、再用 1 MiB 重开 ⇒ 重放中途 kFrozen ⇒ Open 返回 Corruption。
    // 因此在扫描阶段就把每条 batch 解一遍、统计条目数（顺带把畸形 batch 提前判掉）。
    uint64_t entries = 0;
    for (const std::string& rec : p.records) {
      SequenceNumber rec_seq = 0;
      std::vector<BatchEntry> es;
      std::string why;
      if (!ParseBatch(Slice(rec), &rec_seq, &es, &why)) {
        return Status::Corruption("RecoverAndOpen: batch 解析失败", why);
      }
      entries += es.size();
      total_payload += rec.size();
    }
    total_entries += entries;
    plans.push_back(std::move(p));
  }

  // ---- 截断（I18 允许的唯一写）----
  for (const Plan& p : plans) {
    if (!p.truncate) continue;
    const std::string path = LogFileName(name, p.number);
    uint64_t before_size = 0;
    if (env->GetFileSize(path, &before_size).ok() && before_size > p.truncate_at) {
      stats.tail_truncated_bytes += before_size - p.truncate_at;
    }
    s = env->Truncate(path, p.truncate_at);
    if (!s.ok()) return s;
    if (stats.truncation_note.empty()) {
      stats.truncation_note = path + " @ " + std::to_string(p.truncate_at);
    }
    // 设计 §5.2 要求截断后 ReopenAndSync（评审阻断项 5 的代码级偏差）：
    // 不 fsync 的话掉电后 truncate 的元数据可能回滚，而进程已用 O_APPEND 在截断点之后
    // 追加了新 record ⇒ 老尾部字节"复活"并夹在新数据之前 ⇒ 恢复时正好撞上
    // §5.3 的"其后存在完好 record ⇒ 中间损坏 ⇒ 拒绝启动"。
    WritableFile* tf = nullptr;
    s = env->NewAppendableFile(path, &tf);
    if (!s.ok()) return s;
    {
      std::unique_ptr<WritableFile> guard(tf);
      s = guard->Sync();
      if (!s.ok()) return s;
      s = guard->Close();
      if (!s.ok()) return s;
    }
  }

  // ---- 第二遍：按 D12 放大容量后重放 ----
  // 保守估计 MemTable 实际占用：编码字节 ×2（长度前缀/对齐/分配 slop）+ 每条节点开销 ×2 + 余量。
  // 没有重放到任何条目时（新库 / 只有空 log）必须回到 write_buffer_size，
  // 否则 kRecoverySlack(1 MiB) 会把小 write_buffer_size 的 flush 触发点整个盖住。
  const size_t recovered_cap =
      static_cast<size_t>(total_payload) * 2 +
      static_cast<size_t>(total_entries) * kMemTableNodeOverhead * 2 + kRecoverySlack;
  const size_t cap = (total_entries == 0)
                         ? options.write_buffer_size
                         : std::max(options.write_buffer_size, recovered_cap);
  std::unique_ptr<PersistentDBImpl> db(
      new PersistentDBImpl(options, InternalKeyComparator(options.comparator), name, cap));
  SequenceNumber replay_last = 0;
  for (const Plan& p : plans) {
    for (const std::string& rec : p.records) {
      SequenceNumber seq = 0;
      std::vector<BatchEntry> entries;
      std::string why;
      if (!ParseBatch(Slice(rec), &seq, &entries, &why)) {
        return Status::Corruption("RecoverAndOpen: batch 解析失败", why);
      }
      if (seq <= replay_last) {
        // §5.4 承诺：跳过必须**计数上报**，不得静默（评审优化项）
        ++stats.records_skipped;
        continue;                                      // D7 幂等 + 拒绝 sequence 回退
      }
      ++stats.records_replayed;
      stats.entries_replayed += entries.size();
      for (size_t i = 0; i < entries.size(); ++i) {
        const Status a = db->memtable_->Add(seq + static_cast<SequenceNumber>(i), entries[i].type,
                                            entries[i].key, entries[i].value);
        if (!a.ok()) {
          return Status::Corruption("RecoverAndOpen: 重放 Add 失败", a.ToString());
        }
      }
      replay_last = seq + static_cast<SequenceNumber>(entries.size()) - 1;
    }
  }
  // §8.3 ⑧ / I31 的唯一口径：last_sequence_ = max(WAL 重放最大值, 各已注册文件的 max_sequence)。
  const SequenceNumber last = std::max(replay_last, version->MaxSequenceInFiles());
  db->last_sequence_ = last;
  db->log_last_appended_seq_ = last;
  stats.last_sequence = last;
  if (stats.tail_truncated_bytes > kMaxTailCorruptWarnBytes) {
    std::fprintf(stderr,
                 "[WARN] recovery: truncated %llu bytes of tail corruption at %s\n",
                 static_cast<unsigned long long>(stats.tail_truncated_bytes),
                 stats.truncation_note.c_str());
  }
  if (stats.records_skipped > 0) {
    std::fprintf(stderr, "[WARN] recovery: skipped %llu record(s) with non-increasing sequence\n",
                 static_cast<unsigned long long>(stats.records_skipped));
  }

  // ---- §8.3 ⑨：当前 log ----
  // 元数据没有编号（M2 老库 / 元数据缺失）时：有 log 就沿用**最高编号**那个（与 M2 逐字兼容，
  // 现有 M2 恢复用例依赖"最高编号 log 才允许尾部截断"）；一个 log 都没有才分配 next_file_number。
  uint64_t log_number = version->log_number();
  if (log_number == 0) {
    log_number = logs.empty() ? std::max<uint64_t>(1, version->next_file_number()) : hi;
  }
  const std::string current_path = LogFileName(name, log_number);
  if (!env->FileExists(current_path)) {
    WritableFile* nf = nullptr;
    s = env->NewWritableFile(current_path, &nf);
    if (!s.ok()) return s;
    {
      std::unique_ptr<WritableFile> guard(nf);
      s = guard->Close();
    }
    if (!s.ok()) return s;
    s = env->SyncDir(name);
    if (!s.ok()) return s;
    ++stats.current_log_recreated;
  }
  db->log_.reset(new WALWriter(env, current_path));
  s = db->log_->Open(true);
  if (!s.ok()) return s;
  db->log_number_ = log_number;
  // §8.3 步骤 ⑩：恢复出的 memtable 的 log_number = 被重放 log 的最小编号；无重放则 = 当前 log。
  // 漏掉这一条会直接丢数据（I34 的必要性方向，M3-A46）。
  db->memtable_log_number_ = logs.empty() ? log_number : logs.front();
  db->next_file_number_ =
      std::max<uint64_t>(1, std::max<uint64_t>(version->next_file_number(), log_number + 1));
  // 当前 log 是"元数据没有编号"时的分配结果 ⇒ 用一个带显式 log_number 的新 Version 替换
  // （Version 不可变，只能整体重建；M4 换 MANIFEST 后这一步由 VersionSet 内部完成）。
  db->version_ = std::make_shared<const Version>(version->level_files_all(), log_number,
                                                 version->min_log_number_to_keep(),
                                                 db->next_file_number_);
  db->manifest_number_ = manifest_result.manifest_number;
  db->manifest_bytes_ = manifest_result.manifest_bytes;
  db->version_->SetUnrefHook([dbp = db.get()](const Version* v) { dbp->OnVersionUnref(v); });
  db->live_versions_.push_back(db->version_);
  db->smallest_snapshot_ = last;
  if (manifest_result.manifest_present) {
    db->manifest_.reset(new ManifestStore(env, name, options.manifest_roll_bytes));
    const Status os = db->manifest_->OpenAppend(manifest_result.manifest_number,
                                                manifest_result.manifest_bytes);
    if (!os.ok()) return os;
  }
  db->recovery_stats_ = stats;
  db->file_lock_.reset(lock_guard.release());   // 所有权交给 DB（Close/析构时释放）
  db->closed_ = false;
  db->StartBackgroundThread();                  // L12：恢复成功后、发布 *dbptr 之前启动
  db->StartCompactionThread();
  *dbptr = db.release();
  return Status::OK();
}

// ---------------------------------------------------------------------------
// M4.2：有状态 LogAndApply / 延迟删除 / live versions / compaction 线程
// ---------------------------------------------------------------------------

uint64_t PersistentDBImpl::AllocateFileNumber() {
  DbMutexGuard l(mutex_);
  return next_file_number_++;
}

void PersistentDBImpl::OnVersionUnref(const Version* /*v*/) {
  // 延迟清理：Unref 可能在"最后一个 shared_ptr 的析构"里被调用，此时若销毁拥有者会 UAF。
  // 因此只记账（refs 已归零），真正的摘除 + 释放放在 MaybeDeleteObsoleteFiles（锁内、安全点）。
}

void PersistentDBImpl::InstallNewVersionLocked(std::shared_ptr<const Version> newv) {
  newv->SetUnrefHook([this](const Version* v) { OnVersionUnref(v); });
  version_ = newv;
  live_versions_.push_back(std::move(newv));
  if (live_versions_.size() > live_versions_max_) live_versions_max_ = live_versions_.size();
}

void PersistentDBImpl::EnqueueObsoleteSST(uint64_t number) {
  std::lock_guard<std::mutex> l(deletion_mu_);
  pending_delete_sst_.insert(number);
}

void PersistentDBImpl::EnqueueObsoleteManifest(uint64_t number) {
  std::lock_guard<std::mutex> l(deletion_mu_);
  pending_delete_manifest_.insert(number);
}

void PersistentDBImpl::MaybeDeleteObsoleteFiles() {
  std::vector<uint64_t> ssts;
  std::vector<uint64_t> mans;
  {
    std::lock_guard<std::mutex> dl(deletion_mu_);   // 锁序：deletion_mu_ -> mutex_（9.4）
    if (pending_delete_sst_.empty() && pending_delete_manifest_.empty()) return;
    std::set<uint64_t> live;
    uint64_t current_manifest = 0;
    {
      DbMutexGuard ml(mutex_);
      // 安全点：先摘除 refs 已归零、且不是 current 的版本（内存与"待删文件"同时在此刻释放），
      // 再据此计算 live 集合 —— 否则刚归零的版本会把文件多留一轮。
      for (auto it = live_versions_.begin(); it != live_versions_.end();) {
        if (it->get() != version_.get() && (*it)->refs() == 0) {
          it = live_versions_.erase(it);
        } else {
          ++it;
        }
      }
      for (const std::shared_ptr<const Version>& v : live_versions_) {
        for (const FileMetaData& f : v->AllFiles()) live.insert(f.number);
      }
      if (version_ != nullptr) {
        for (const FileMetaData& f : version_->AllFiles()) live.insert(f.number);
      }
      // 只读受 mutex_ 保护的 manifest_number_（不在 install_mu_ 下读 ManifestStore 内部状态，避免与 RollAndOpen 竞态）
      current_manifest = manifest_number_;
    }
    for (auto it = pending_delete_sst_.begin(); it != pending_delete_sst_.end();) {
      if (live.count(*it) != 0) {
        ++it;   // 仍被某个 live Version 引用 => 绝不删（I42/X8）
      } else {
        ssts.push_back(*it);
        it = pending_delete_sst_.erase(it);
      }
    }
    for (auto it = pending_delete_manifest_.begin(); it != pending_delete_manifest_.end();) {
      if (*it == current_manifest) {
        ++it;   // CURRENT 仍指向它 => 不删
      } else {
        mans.push_back(*it);
        it = pending_delete_manifest_.erase(it);
      }
    }
  }
  for (uint64_t n : ssts) {
    if (table_cache_ != nullptr) table_cache_->Evict(n);
    EnvOf()->DeleteFile(TableFileName(dbname_, n));   // 锁外 unlink（L24/L26）
  }
  for (uint64_t n : mans) {
    EnvOf()->DeleteFile(ManifestFileName(dbname_, n));
  }
}

void PersistentDBImpl::MaybeScheduleCompaction() {
  DbMutexGuard l(mutex_);
  if (!compaction_started_ || compact_stop_ || closed_) return;
  if (!compaction_auto_) return;
  compaction_pending_ = true;
  compact_cv_.notify_all();   // 即使 pending 已置位也要 notify：谓词还要求 immutables_ 为空
}

void PersistentDBImpl::StartCompactionThread() {
  {
    DbMutexGuard l(mutex_);
    if (compaction_started_) return;
    compaction_started_ = true;
    compact_stop_ = false;
  }
  compaction_thread_ = std::thread([this] { BackgroundCompactionLoop(); });
}

Status PersistentDBImpl::LogAndApply(const VersionEdit& edit,
                                     const std::shared_ptr<const Version>& base,
                                     std::shared_ptr<const Version>* out_new) {
  {
  std::lock_guard<std::mutex> il(install_mu_);
  InstallMuGuard install_guard;   // 仅诊断：A35 的安装临界区探针
  // 9.4 全序最左端：安装串行化
  std::shared_ptr<const Version> newv;
  std::shared_ptr<const Version> prev;
  std::string why;
  {
    DbMutexGuard ml(mutex_);
    if (version_ == nullptr) return Status::IOError("LogAndApply: version_ == nullptr");
    if (base != nullptr && base.get() != version_.get()) {
      ++compaction_stats_.install_rebase_retries;   // L25：回锁后状态已变 ⇒ 以当前 version_ 重放
    }
    std::shared_ptr<const Version> tmp;
    if (!VersionSet::ApplyEdit(*version_, edit, &tmp, &why)) {
      return Status::Corruption("LogAndApply: 安装期层级校验失败", why);
    }
    // next_file_number_ 的权威值 = 当前值（并发分配只增不减，绝不能用 stale 的 edit 值回退）。
    newv = std::make_shared<const Version>(tmp->level_files_all(), tmp->log_number(),
                                           tmp->min_log_number_to_keep(), next_file_number_);
    prev = version_;
  }

  if (manifest_ == nullptr) {
    manifest_.reset(new ManifestStore(EnvOf(), dbname_, options_.manifest_roll_bytes));
  }
  Status s;
  uint64_t old_manifest = 0;
  if (!manifest_->open() || manifest_->ShouldRoll()) {
    const uint64_t n = AllocateFileNumber();
    s = manifest_->RollAndOpen(*newv, options_, n, &old_manifest);
  } else {
    s = manifest_->Append(edit);
  }
  if (!s.ok()) return s;   // 先落盘后安装：失败 => version_ 不变、无半成品被注册

  std::set<uint64_t> removed;
  {
    std::lock_guard<std::mutex> dl(deletion_mu_);   // 锁序：deletion_mu_ -> mutex_
    {
      DbMutexGuard ml(mutex_);
      prev = version_;
      for (const FileMetaData& f : prev->AllFiles()) removed.insert(f.number);
      for (const FileMetaData& f : newv->AllFiles()) removed.erase(f.number);
      InstallNewVersionLocked(newv);
      manifest_number_ = manifest_->number();
      manifest_bytes_ = manifest_->bytes();
      manifest_edits_ = manifest_->edits();
      manifest_rolls_ = manifest_->rolls();
    }
    for (uint64_t n : removed) pending_delete_sst_.insert(n);
    if (old_manifest != 0) pending_delete_manifest_.insert(old_manifest);
  }
  if (prev != nullptr && prev.get() != newv.get()) prev->Unref();   // 锁外 Unref（hook 会取 mutex_）
  if (out_new != nullptr) *out_new = newv;
  }   // 释放 install_mu_
  MaybeDeleteObsoleteFiles();   // L24/I43：unlink 必须在安装临界区之外（A35）
  return Status::OK();
}

void PersistentDBImpl::CompactOnce() {
  CompactionInputs in;
  std::shared_ptr<const Version> base;
  SequenceNumber smallest = 0;
  int level = -1;
  const PickStrategy strategy = options_.compaction_pick_strategy;
  {
    DbMutexGuard l(mutex_);
    compaction_pending_ = false;
    if (!bg_error_.ok() || closed_ || version_ == nullptr) return;
    level = Compaction::PickLevel(*version_, options_);
    if (level < 0) return;
    std::string why;
    if (!Compaction::PickInputs(*version_, level, strategy, options_, &in, &why)) return;
    base = version_;
    base->Ref();
    smallest = smallest_snapshot_;
    ++compaction_stats_.started;
    ++compaction_stats_.rounds_by_level[level];
    if (strategy == PickStrategy::kRoundRobin) {
      ++compaction_stats_.pick_round_robin;
    } else {
      ++compaction_stats_.pick_min_overlap;
    }
  }

  const uint64_t t0 = EnvOf()->NowMicros();
  VersionEdit edit;
  CompactionStats round;
  std::string why;
  const Status run_status =
      Compaction::Run(EnvOf(), table_cache_.get(), dbname_, in, options_, *base, smallest,
                      [this] { return AllocateFileNumber(); }, internal_comparator_, &edit, &round,
                      &why);
  Status install_status = Status::OK();
  if (run_status.ok()) {
    std::shared_ptr<const Version> newv;
    install_status = LogAndApply(edit, base, &newv);
  }
  const uint64_t dt = EnvOf()->NowMicros() - t0;
  {
    DbMutexGuard l(mutex_);
    compaction_stats_.input_files += round.input_files;
    compaction_stats_.output_files += round.output_files;
    compaction_stats_.bytes_read += round.bytes_read;
    compaction_stats_.bytes_written += round.bytes_written;
    compaction_stats_.dropped_old_versions += round.dropped_old_versions;
    compaction_stats_.dropped_tombstones += round.dropped_tombstones;
    if (dt > compaction_stats_.round_micros_max) compaction_stats_.round_micros_max = dt;
    round_samples_us_.push_back(dt);   // 多轮采样；p50 在 getter 里按样本算，不用单轮近似
    if (run_status.ok() && install_status.ok()) {
      ++compaction_stats_.completed;
    } else {
      ++compaction_stats_.failed;
      ++compaction_stats_.aborted;   // 放弃一轮必须计数（8.6）
      compaction_stats_.last_error =
          run_status.ok() ? install_status.ToString() : run_status.ToString();
    }
  }
  base->Unref();
}

void PersistentDBImpl::BackgroundCompactionLoop() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    // flush 优先：只要 immutables_ 非空就让路（L27 的固定优先级）。
    compact_cv_.wait(l, [this] {
      return compact_stop_ || (compaction_pending_ && immutables_.empty());
    });
    if (compact_stop_) break;
    if (!compaction_pending_) continue;
    l.unlock();
    CompactOnce();
    l.lock();
    if (!bg_error_.ok()) {
      compact_cv_.wait(l, [this] { return compact_stop_; });
      break;
    }
  }
}

ManifestStats PersistentDBImpl::GetManifestStats() const {
  ManifestStats out;
  {
    DbMutexGuard l(mutex_);
    out.number = manifest_number_;
    out.bytes = manifest_bytes_;
    out.edits = manifest_edits_;
    out.rolls = manifest_rolls_;
    out.replay_edits = recovery_stats_.manifest_edits_replayed;
    out.replay_truncated_bytes = recovery_stats_.manifest_tail_truncated_bytes;
  }
  return out;
}

CompactionStats PersistentDBImpl::GetCompactionStats() const {
  DbMutexGuard l(mutex_);
  return compaction_stats_;
}

const PersistentDBImpl::Snapshot* PersistentDBImpl::GetSnapshot() {
  DbMutexGuard l(mutex_);
  std::unique_ptr<Snapshot> h(new Snapshot());
  h->sequence = last_sequence_;
  const Snapshot* p = h.get();
  snapshot_handles_.push_back(std::move(h));
  snapshots_.insert(p->sequence);
  smallest_snapshot_ = snapshots_.empty() ? last_sequence_ : *snapshots_.begin();
  return p;
}

void PersistentDBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  DbMutexGuard l(mutex_);
  for (auto it = snapshot_handles_.begin(); it != snapshot_handles_.end(); ++it) {
    if (it->get() == snapshot) {
      const auto sit = snapshots_.find(snapshot->sequence);
      if (sit != snapshots_.end()) snapshots_.erase(sit);   // 按值删一个（A29）
      snapshot_handles_.erase(it);
      break;
    }
  }
  smallest_snapshot_ = snapshots_.empty() ? last_sequence_ : *snapshots_.begin();
}

uint64_t PersistentDBImpl::files_at_level(int level) const {
  DbMutexGuard l(mutex_);
  if (version_ == nullptr) return 0;
  return version_->level_files(level).size();
}

uint64_t PersistentDBImpl::bytes_at_level(int level) const {
  DbMutexGuard l(mutex_);
  if (version_ == nullptr) return 0;
  return version_->total_bytes(level);
}

std::shared_ptr<const Version> PersistentDBImpl::RefCurrentVersionForTest() {
  DbMutexGuard l(mutex_);
  if (version_ == nullptr) return nullptr;
  version_->Ref();
  const Version* p = version_.get();
  return std::shared_ptr<const Version>(p, [](const Version* v) { v->Unref(); });
}


// ---------------------------------------------------------------------------
// M4.3：层级统计 / 三个放大口径 / 固定行格式（docs/m4-design.md §10.3）
// ---------------------------------------------------------------------------

namespace {
uint64_t Percentile(const std::vector<uint64_t>& v, int pct) {
  if (v.empty()) return 0;
  std::vector<uint64_t> c = v;
  std::sort(c.begin(), c.end());
  size_t idx = static_cast<size_t>(pct) * (c.size() - 1) / 100;
  if (idx >= c.size()) idx = c.size() - 1;   // p999 的分母是 1000，必须夹取
  return c[idx];
}
}  // namespace

LevelStats PersistentDBImpl::GetLevelStats() const {
  DbMutexGuard l(mutex_);
  LevelStats out;
  if (version_ == nullptr) return out;
  for (int l = 0; l < kNumLevels; ++l) {
    out.files[l] = version_->level_files(l).size();
    out.bytes[l] = version_->total_bytes(l);
    out.score[l] = Compaction::Score(*version_, options_, l);
  }
  return out;
}

AmplificationStats PersistentDBImpl::GetAmplificationStats() const {
  AmplificationStats out;
  {
    DbMutexGuard l(mutex_);
    out.user_logical_bytes = user_logical_bytes_;
    out.entry_bytes = entry_bytes_;
    out.flush_write_bytes = flush_write_bytes_;
    out.get_count = get_count_;
    out.files_checked = read_stats_.files_checked;
    out.index_blocks_read = read_stats_.index_blocks_read;
    out.data_blocks_read = read_stats_.data_blocks_read;
    out.bytes_read = read_stats_.bytes_read;
    out.manifest_bytes = manifest_bytes_;
    out.compaction_rounds = compaction_stats_.completed;
    out.compact_write_bytes = compaction_stats_.bytes_written;
    out.dropped_old_versions = compaction_stats_.dropped_old_versions;
    out.dropped_tombstones = compaction_stats_.dropped_tombstones;
    out.round_samples = round_samples_us_.size();
    out.compaction_round_p50_us = Percentile(round_samples_us_, 50);
    out.compaction_round_max_us = compaction_stats_.round_micros_max;
    out.live_versions = live_versions_.size();
    out.live_versions_max = live_versions_max_;
    // M5.3（§6.6）：读路径的 filter 计数器（线程局部 delta 汇总后的口径，L34）。
    out.filter_checked = read_stats_.filter_checked;
    out.filter_negative = read_stats_.filter_negative;
    out.filter_positive = read_stats_.filter_positive;
    out.filter_unavailable = read_stats_.filter_unavailable;
    out.data_blocks_skipped_by_filter = read_stats_.data_blocks_skipped_by_filter;
    if (version_ != nullptr) {
      for (const FileMetaData& f : version_->AllFiles()) out.sst_bytes += f.file_size;
    }
  }
  // 目录扫描在锁外（L26；A36 的探针会把持锁 IO 当违规）。
  std::vector<std::string> children;
  if (EnvOf()->GetChildren(dbname_, &children).ok()) {
    for (const std::string& c : children) {
      uint64_t n = 0;
      uint64_t size = 0;
      std::string path = dbname_ + "/" + c;
      if (c == "CURRENT") {
        EnvOf()->GetFileSize(path, &size);
        out.current_bytes = size;
      } else if (ParseLogFileName(c, &n)) {
        EnvOf()->GetFileSize(path, &size);
        out.log_bytes += size;
      } else if (ParseTempFileName(c, &n) || ParseManifestTempFileName(c, &n) ||
                 c == "CURRENT.tmp") {
        EnvOf()->GetFileSize(path, &size);
        out.tmp_bytes += size;
      }
    }
  }
  return out;
}

std::string PersistentDBImpl::FormatLevelLine(const std::string& round_id) const {
  const LevelStats ls = GetLevelStats();
  std::string out = "LEVEL round_id=" + round_id;
  uint64_t total_files = 0, total_bytes = 0;
  for (int l = 0; l < kNumLevels; ++l) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), " l%d_files=%llu l%d_bytes=%llu", l,
                  static_cast<unsigned long long>(ls.files[l]), l,
                  static_cast<unsigned long long>(ls.bytes[l]));
    out += buf;
    total_files += ls.files[l];
    total_bytes += ls.bytes[l];
  }
  {
    char buf[128];
    std::snprintf(buf, sizeof(buf), " total_sst_files=%llu total_sst_bytes=%llu",
                  static_cast<unsigned long long>(total_files),
                  static_cast<unsigned long long>(total_bytes));
    out += buf;
  }
  for (int l = 0; l < kNumLevels; ++l) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " l%d_score=%.6f", l, ls.score[l]);
    out += buf;
  }
  return out;
}

std::string PersistentDBImpl::FormatAmplLine(const std::string& round_id) const {
  const AmplificationStats a = GetAmplificationStats();
  const double user = static_cast<double>(a.user_logical_bytes);
  const double wa_total =
      user > 0 ? static_cast<double>(a.flush_write_bytes + a.compact_write_bytes) / user : 0.0;
  const double wa_excl = user > 0 ? static_cast<double>(a.flush_write_bytes) / user : 0.0;
  const double read_amp =
      a.get_count > 0 ? static_cast<double>(a.files_checked) / static_cast<double>(a.get_count) : 0.0;
  const double space_amp =
      user > 0 ? static_cast<double>(a.sst_bytes + a.manifest_bytes + a.current_bytes + a.log_bytes +
                                     a.tmp_bytes) /
                     user
               : 0.0;
  const double space_sst = user > 0 ? static_cast<double>(a.sst_bytes) / user : 0.0;
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "AMPL round_id=%s user_logical_bytes=%llu entry_bytes=%llu flush_write_bytes=%llu "
                "compact_write_bytes=%llu write_amp_total=%.6f write_amp_excl_compact=%.6f "
                "read_files_checked=%llu read_index_blocks_read=%llu read_data_blocks_read=%llu "
                "read_bytes=%llu read_get_count=%llu read_amp_files_per_get=%.6f "
                "space_sst_bytes=%llu space_manifest_bytes=%llu space_current_bytes=%llu "
                "space_log_bytes=%llu space_tmp_bytes=%llu space_amp=%.6f space_amp_sst_only=%.6f "
                "dropped_old_versions=%llu dropped_tombstones=%llu compaction_rounds=%llu "
                "compaction_round_p50_us=%llu compaction_round_max_us=%llu live_versions_max=%llu "
                "read_filter_checked=%llu read_filter_negative=%llu read_filter_positive=%llu "
                "read_filter_unavailable=%llu read_data_blocks_skipped_by_filter=%llu",
                round_id.c_str(), static_cast<unsigned long long>(a.user_logical_bytes),
                static_cast<unsigned long long>(a.entry_bytes),
                static_cast<unsigned long long>(a.flush_write_bytes),
                static_cast<unsigned long long>(a.compact_write_bytes), wa_total, wa_excl,
                static_cast<unsigned long long>(a.files_checked),
                static_cast<unsigned long long>(a.index_blocks_read),
                static_cast<unsigned long long>(a.data_blocks_read),
                static_cast<unsigned long long>(a.bytes_read),
                static_cast<unsigned long long>(a.get_count), read_amp,
                static_cast<unsigned long long>(a.sst_bytes),
                static_cast<unsigned long long>(a.manifest_bytes),
                static_cast<unsigned long long>(a.current_bytes),
                static_cast<unsigned long long>(a.log_bytes),
                static_cast<unsigned long long>(a.tmp_bytes), space_amp, space_sst,
                static_cast<unsigned long long>(a.dropped_old_versions),
                static_cast<unsigned long long>(a.dropped_tombstones),
                static_cast<unsigned long long>(a.compaction_rounds),
                static_cast<unsigned long long>(a.compaction_round_p50_us),
                static_cast<unsigned long long>(a.compaction_round_max_us),
                static_cast<unsigned long long>(a.live_versions_max),
                static_cast<unsigned long long>(a.filter_checked),
                static_cast<unsigned long long>(a.filter_negative),
                static_cast<unsigned long long>(a.filter_positive),
                static_cast<unsigned long long>(a.filter_unavailable),
                static_cast<unsigned long long>(a.data_blocks_skipped_by_filter));
  return std::string(buf);
}

std::string PersistentDBImpl::FormatFrontLine(const std::string& round_id) const {
  std::vector<uint64_t> samples;
  uint64_t get_count = 0;
  {
    DbMutexGuard l(mutex_);
    samples = front_samples_us_;
    get_count = get_count_;
  }
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "FRONT round_id=%s ops=%llu get_ops=%llu p50_us=%llu p99_us=%llu p999_us=%llu "
                "max_us=%llu",
                round_id.c_str(), static_cast<unsigned long long>(put_ops_ + get_count),
                static_cast<unsigned long long>(get_count), static_cast<unsigned long long>(Percentile(samples, 50)),
                static_cast<unsigned long long>(Percentile(samples, 99)),
                static_cast<unsigned long long>(Percentile(samples, 999)),
                static_cast<unsigned long long>(samples.empty() ? 0 : samples.back()));
  return std::string(buf);
}


}  // namespace lsm
