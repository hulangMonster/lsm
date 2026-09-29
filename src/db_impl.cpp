// src/db_impl.cpp —— 恢复（§5）+ 过渡写路径（§6.2）+ sync/close 语义（§7）
#include "db_impl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "db_iter.h"
#include "filename.h"
#include "merging_iterator.h"
#include "sstable/table_builder.h"
#include "util/coding.h"

namespace lsm {
namespace {

namespace {
// A25 探针（I17 持锁零 IO 的可验证化）：持有 DB 互斥锁的线程置位该标记。
// 只做诊断，不改变加锁语义（内部仍是 std::lock_guard）。
thread_local bool g_db_mutex_held = false;
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
    const uint8_t t = static_cast<uint8_t>(input[0]);
    input = Slice(input.data() + 1, input.size() - 1);
    if (t != kTypeValue && t != kTypeDeletion) {
      *why = "entry type 非法：" + std::to_string(static_cast<int>(t));
      return false;
    }
    e.type = static_cast<ValueType>(t);
    uint32_t klen = 0;
    if (!GetVarint32(&input, &klen) || klen == 0 || klen > kMaxUserKeySize || klen > input.size()) {
      *why = "key_len 非法";
      return false;
    }
    e.key.assign(input.data(), klen);
    input = Slice(input.data() + klen, input.size() - klen);
    if (e.type == kTypeValue) {
      uint32_t vlen = 0;
      if (!GetVarint32(&input, &vlen) || vlen > input.size()) {
        *why = "value_len 非法";
        return false;
      }
      e.value.assign(input.data(), vlen);
      input = Slice(input.data() + vlen, input.size() - vlen);
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

Status PersistentDBImpl::Write(ValueType type, const WriteOptions& options, const Slice& key,
                              const Slice& value) {
  if (key.empty()) return Status::InvalidArgument("Put/Delete: empty user key");
  if (key.size() > kMaxUserKeySize) {
    return Status::InvalidArgument("Put/Delete: user key too large", std::to_string(key.size()));
  }

    // ---- M2.3 组提交（design §6.3）----
  // 每个写者入队后等待被结算；队首当选 flusher，把一批写者的条目**合并成一条 WAL record**
  // （一条 record = 一个 CRC = 一个原子单位，I15），并替整批做一次 fsync。
  Pending w;
  w.need_sync = options.sync;
  w.type = type;
  w.key.assign(key.data(), key.size());
  w.value.assign(value.data(), value.size());
  w.entry_bytes = 1 + static_cast<size_t>(VarintLength(w.key.size())) + w.key.size() +
                  (type == kTypeValue
                       ? static_cast<size_t>(VarintLength(w.value.size())) + w.value.size()
                       : 0);

  // 评审优化项：超过 WAL 单条 record 上限的输入必须在**入队之前**拒绝。
  // 否则批次会被接受、Append 返回 kInvalidArgument、RunFlusher 把它当持久化失败写进
  // bg_error_ ⇒ 整个库转粘性写只读（连 Close 都返回错误）。输入校验不该触发 fail-stop。
  if (w.entry_bytes + 16 > kMaxLogicalRecordSize) {
    return Status::InvalidArgument("Put/Delete: record exceeds kMaxLogicalRecordSize",
                                   std::to_string(w.entry_bytes + 16));
  }

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
  queue_.push_back(&w);
  while (!w.done) {
    // 谓词必须同时覆盖「我已经完成」与「**我能否接手当 flusher**」——只等"完成"会让最后一个写者
    // 睡满超时后才能当 flusher（raft-kv 的 P2a 丢唤醒复盘，L9/L10）。这里用 while + 无谓词 wait
    // 重查，语义等价且不会丢唤醒。
    if (!flusher_active_ && queue_.front() == &w) {
      flusher_active_ = true;
      l.unlock();
      const Status s = RunFlusher();
      l.lock();
      // D4 窗口放开时机：RunFlusher 已经「结算并唤醒组内成员」，此处才清 flusher_active_，
      // 紧接着唤醒新队首。顺序不可交换：先清标志会让下一批与本次收尾并发，多出一次排队中的 fsync。
      flusher_active_ = false;
      commit_cv_.notify_all();   // 交接（L10）；用 notify_all 是因为 Close() 也在这把 cv 上等
      if (!s.ok() && !w.done) return s;
      continue;
    }
    commit_cv_.wait(l);
  }
  return w.status;
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
  // §9.4 的 batch 编码：sequence(8B LE) || count(4B LE) || entry[0..count)
  std::string out;
  PutFixed64(&out, begin);
  PutFixed32(&out, static_cast<uint32_t>(members.size()));
  for (const Pending* p : members) {
    out.push_back(static_cast<char>(p->type));
    PutVarint32(&out, static_cast<uint32_t>(p->key.size()));
    out.append(p->key);
    if (p->type == kTypeValue) {
      PutVarint32(&out, static_cast<uint32_t>(p->value.size()));
      out.append(p->value);
    }
  }
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
  std::string payload;
  std::vector<Pending*> members;
  bool need_sync = false;
  SequenceNumber begin = 0;
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
      p->begin = begin + static_cast<SequenceNumber>(members.size());
      members.push_back(p);
      bytes += p->entry_bytes;
      need_sync = need_sync || p->need_sync;               // D3：sync 取组内 OR（优于 LevelDB 只看队首）
    }
    for (size_t i = 0; i < members.size(); ++i) queue_.pop_front();
    size_t footprint = 0;
    for (const Pending* p : members) footprint += p->entry_bytes + kMemTableNodeOverhead;
    if (closed_) {
      reject = Status::IOError("Put/Delete: DB is closed", dbname_);
    } else if (!bg_error_.ok()) {
      reject = bg_error_;
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
          bg_cv_.notify_all();                  // 唤醒后台线程（L13）
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
      }
      std::lock_guard<std::mutex> ql(commit_mu_);
      for (Pending* p : members) {
        p->status = rs;                                     // 整批同一个 Status（I16/I33）
        p->done = true;
      }
      commit_cv_.notify_all();
      return rs;
    }
  }

  // ---- 阶段 B（持 commit_mu_ → mutex_）：分配 sequence + 组 payload（§6.6.1）----
  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    DbMutexGuard ml(mutex_);
    begin = last_sequence_ + 1;
    for (size_t i = 0; i < members.size(); ++i) {
      members[i]->begin = begin + static_cast<SequenceNumber>(i);
    }
    last_sequence_ = begin + static_cast<SequenceNumber>(members.size()) - 1;
    payload = EncodeGroup(begin, members);
  }

  // ---- 阶段 C（锁外做 IO，I17/L7）：Append 永远做，fsync 只在组内有人要求时做（I11/D3）----
  Status s = log_->Append(Slice(payload));
  if (s.ok()) {
    // I32 修复：Append 返回 kOk 即字节已交给文件（fsync 只决定是否落到介质），所以这个边界
    // 只取决于 Append 的结果，与本次是否 fsync 无关。
    DbMutexGuard ml(mutex_);
    log_last_appended_seq_ = begin + static_cast<SequenceNumber>(members.size()) - 1;
  }
  if (s.ok() && need_sync) s = log_->Sync();
  if (s.ok()) {
    if (options_.commit_hook != nullptr) options_.commit_hook->OnAfterSyncBeforePublish();
    std::lock_guard<std::mutex> ql(commit_mu_);
    // 阻断项 2 的修复：**只有真的 fsync 过**才推进 durable 水位（design §7.1 用它论证 I11：
    // w.status.ok() ⟹ durable_seq_ >= w.end_seq）。原来无条件推进，会让只写不 fsync 的批
    // 也把水位抬高，这条推理链就断了（M3 若拿它当 durable 水位会被误导）。
    if (need_sync) {
      durable_seq_ = begin + static_cast<SequenceNumber>(members.size()) - 1;
    }
  } else {
    DbMutexGuard ml(mutex_);
    bg_error_ = s;                                         // 粘性：偏移已不可信（D11）；I16 传播给整批
  }

  if (s.ok()) {
    DbMutexGuard ml(mutex_);
    for (Pending* p : members) {
      const Status a = memtable_->Add(p->begin, p->type, p->key, p->value);
      if (!a.ok()) {
        bg_error_ = a;
        s = a;
        break;
      }
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
  return Write(kTypeValue, options, key, value);
}

Status PersistentDBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return Write(kTypeDeletion, options, key, Slice());
}

Status PersistentDBImpl::Get(const Slice& key, std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("PersistentDBImpl::Get: null value pointer");
  value->clear();
  DbReadStats delta;
  const Status s = GetInternal(key, value, &delta);
  MergeReadStats(delta);
  return s;
}

Status PersistentDBImpl::GetInternal(const Slice& key, std::string* value, DbReadStats* delta) {
  std::shared_ptr<const MemTable> mt;
  std::vector<std::shared_ptr<const MemTable>> imms;
  std::shared_ptr<const Version> ver;
  SequenceNumber snapshot = 0;
  {
    // L19：mutex_ 内只做「取 shared_ptr 引用 + 取快照」；所有 IO 与比较在锁外。
    DbMutexGuard l(mutex_);
    snapshot = last_sequence_;
    mt = memtable_;
    imms.reserve(immutables_.size());
    for (const std::shared_ptr<Immutable>& im : immutables_) imms.push_back(im->mem);
    ver = version_;
  }

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
  for (auto it = imms.rbegin(); it != imms.rend(); ++it) {   // 新→旧（deque 的 back 最新）
    r = lookup_mem(*it, HitLayer::kImmutable);
    if (r == 1) return Status::OK();
    if (r == -1) return Status::NotFound("PersistentDBImpl::Get: key is deleted in immutable");
  }

  if (ver != nullptr) {
    const Comparator* user_cmp = internal_comparator_.user_comparator();
    for (const FileMetaData& f : ver->files()) {   // 文件号降序 = 新→旧（§7.1 规则 2）
      Slice min_user;
      Slice max_user;
      SequenceNumber tmp_seq = 0;
      ValueType tmp_type = kTypeValue;
      if (!ParseInternalKey(Slice(f.smallest), &min_user, &tmp_seq, &tmp_type) ||
          !ParseInternalKey(Slice(f.largest), &max_user, &tmp_seq, &tmp_type)) {
        return Status::Corruption("PersistentDBImpl::Get", "file metadata key range is malformed");
      }
      if (user_cmp->Compare(key, min_user) < 0 || user_cmp->Compare(key, max_user) > 0) {
        ++delta->key_range_skipped;   // 零 IO（§5.4 ① / A19 的 DB 层口径）
        continue;
      }
      // M3-A30 的口径：files_checked 只数**真的进了 Table::GetEntry** 的文件；
      // 递增归属是 DB 层（不是 Table 层），避免双重计数。
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
  std::shared_ptr<const MemTable> mt;
  std::vector<std::shared_ptr<const MemTable>> imms;
  std::shared_ptr<const Version> ver;
  SequenceNumber snapshot = 0;
  {
    DbMutexGuard l(mutex_);
    snapshot = last_sequence_;
    mt = memtable_;
    imms.reserve(immutables_.size());
    for (const std::shared_ptr<Immutable>& im : immutables_) imms.push_back(im->mem);
    ver = version_;
  }

  // 迭代器必须持住每一个 MemTable / Version 的引用（L19/L21），否则迭代中途 flush 注册并
  // 释放 Arena 会造成 UAF（M3-A33 在 ASan 下钉住）。
  std::vector<std::shared_ptr<const void>> refs;
  refs.push_back(mt);
  for (const std::shared_ptr<const MemTable>& m : imms) refs.push_back(m);
  if (ver != nullptr) refs.push_back(ver);

  std::vector<Iterator*> kids_vec;
  kids_vec.push_back(mt->NewIterator());
  for (auto it = imms.rbegin(); it != imms.rend(); ++it) kids_vec.push_back((*it)->NewIterator());
  if (ver != nullptr) {
    for (const FileMetaData& f : ver->files()) {
      std::unique_ptr<Iterator> child = table_cache_->NewIterator(f, nullptr);
      kids_vec.push_back(child.release());
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
      enqueued = true;
      bg_cv_.notify_all();
    } else if (!log_sealed_) {
      // 空表：仍要把当前 log 轮转成空文件（"关库时当前 log 为空"的契约）。
      log_sealed_ = true;
      need_rotate_ = true;
    }
  }
  const Status rs = RotateLog();
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
    bg_cv_.wait(l, [this] { return bg_stop_ || !immutables_.empty(); });
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

  // ⑦a 内存版本替换（§6.3 步骤 ⑦ 的前半）：min_log_number_to_keep 由单一真相源重算（I34）。
  std::shared_ptr<const Version> snapshot;
  uint64_t roll_new_manifest = 0;
  {
    DbMutexGuard l(mutex_);
    if (!bg_error_.ok() || closed_) return;   // 与 Close/失败的竞态：不注册，数据仍在 WAL+内存
    if (version_ == nullptr) {
      version_ = VersionSet::Empty(log_number_ == 0 ? memtable_log_number_ : log_number_,
                                   next_file_number_);
    }
    const uint64_t min_keep = RecomputeMinLogNumberToKeepLocked(imm.get());
    version_ = VersionSet::RegisterFile(*version_, meta, log_number_, min_keep, next_file_number_);
    snapshot = version_;
    if (manifest_number_ == 0 || manifest_bytes_ > options_.manifest_roll_bytes) {
      roll_new_manifest = next_file_number_++;   // 模式 (a)：分配新 MANIFEST 编号
    }
  }

  // ⑦b META 持久化：META.tmp → fsync → rename(META) → SyncDir（§6.3 步骤 ⑦/L17）。
  // 这是 IO，必须在 DB 锁外做（L18）。
  VersionEdit edit;
  Status ms;
  edit.SetComparatorName(options_.comparator->Name());
  edit.SetLogNumber(snapshot->log_number());
  edit.SetMinLogNumberToKeep(snapshot->min_log_number_to_keep());
  edit.SetNextFileNumber(snapshot->next_file_number());
  edit.AddFile(0, meta);
  if (roll_new_manifest != 0) {
    const uint64_t old = manifest_number_;
    ms = VersionSet::WriteSnapshotManifest(EnvOf(), dbname_, roll_new_manifest, *snapshot, options_);
    if (ms.ok()) ms = VersionSet::WriteCurrentAtomic(EnvOf(), dbname_, roll_new_manifest);
    if (ms.ok()) {
      manifest_number_ = roll_new_manifest;
      manifest_edits_ = 1;
      ++manifest_rolls_;
      uint64_t size = 0;
      EnvOf()->GetFileSize(ManifestFileName(dbname_, manifest_number_), &size);
      manifest_bytes_ = size;
      if (old != 0 && old != manifest_number_) {
        EnvOf()->DeleteFile(ManifestFileName(dbname_, old));   // 旧 MANIFEST 只在切换后删
      }
    }
  } else {
    ms = VersionSet::AppendEdit(EnvOf(), dbname_, manifest_number_, edit, &manifest_bytes_);
    if (ms.ok()) ++manifest_edits_;
  }
  if (!ms.ok()) {
    DbMutexGuard l(mutex_);
    bg_error_ = ms;              // 粘性 fail-stop（§6.4 的"写/rename META"行）
    ++flush_stats_.flushes_failed;
    flush_stats_.last_error = ms.ToString();
    bg_cv_.notify_all();
    return;   // imm 留在 immutables_；内存版本已含该文件 ⇒ 本轮可读；WAL **未删**（I34）
  }

  // ⑦c 注册完成（META 已 durable）：弹出 imm + 计数。
  {
    DbMutexGuard l(mutex_);
    if (!immutables_.empty() && immutables_.front() == imm) immutables_.pop_front();
    ++flush_stats_.flushes_completed;
    flush_stats_.index_size_warn += index_warn;
    bg_cv_.notify_all();
  }

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
  db->recovery_stats_ = stats;
  db->file_lock_.reset(lock_guard.release());   // 所有权交给 DB（Close/析构时释放）
  db->closed_ = false;
  db->StartBackgroundThread();                  // L12：恢复成功后、发布 *dbptr 之前启动
  *dbptr = db.release();
  return Status::OK();
}

}  // namespace lsm
