// src/db_impl.cpp —— 恢复（§5）+ 过渡写路径（§6.2）+ sync/close 语义（§7）
#include "db_impl.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "filename.h"
#include "util/coding.h"

namespace lsm {
namespace {

constexpr size_t kRecoverySlack = 1u * 1024 * 1024;      // D12：恢复容量的余量
constexpr uint32_t kMaxBatchCount = 1u << 20;            // §9.4：防畸形 count 撑爆
constexpr size_t kMaxGroupBytes = 1u * 1024 * 1024;      // D3：一批的字节上限（1 MiB）
constexpr size_t kMaxGroupRecs = 64;                     // D3：一批的写者数上限
// 每条 entry 在 MemTable 里的额外占用（跳表节点 ≈ sizeof(Node) + 分配 slop）。
// 用于把"WAL 侧字节估算"折算成"MemTable 容量占用"，避免两处口径漂移（M2 评审阻断项 1）。
constexpr size_t kMemTableNodeOverhead = 128;

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
// 用户视图迭代器（三态状态机，design §4.4）：与 M1 §9 的行为逐条一致
// ---------------------------------------------------------------------------
namespace {

class UserIterator : public Iterator {
 public:
  UserIterator(const MemTable* mem, const InternalKeyComparator& icmp)
      : internal_(mem->NewIterator()), user_comparator_(icmp.user_comparator()), state_(kBeforeFirst) {}
  ~UserIterator() override { delete internal_; }
  UserIterator(const UserIterator&) = delete;
  UserIterator& operator=(const UserIterator&) = delete;

  bool Valid() const override { return state_ == kValid; }

  void SeekToFirst() override {
    internal_->SeekToFirst();
    ScanForwardToVisible();
  }
  void SeekToLast() override {
    internal_->SeekToLast();
    if (!internal_->Valid()) {
      state_ = kBeforeFirst;
      return;
    }
    RewindToRunStart();
    ScanBackwardToVisible();
  }
  void Seek(const Slice& target) override {
    internal_->Seek(BuildLookupKey(target, kMaxSequenceNumber));
    ScanForwardToVisible();
  }
  void Next() override {
    if (state_ != kValid) return;
    SkipCurrentRunForwardWithKey(Slice(current_user_key_));
    ScanForwardToVisible();
  }
  void Prev() override {
    if (state_ == kBeforeFirst) return;
    if (state_ == kPastEnd) {
      SeekToLast();
      return;
    }
    RewindToRunStart();
    internal_->Prev();
    if (!internal_->Valid()) {
      state_ = kBeforeFirst;
      return;
    }
    RewindToRunStart();
    ScanBackwardToVisible();
  }

  Slice key() const override { return Slice(current_user_key_); }
  Slice value() const override { return internal_->value(); }
  Status status() const override { return Status::OK(); }

 private:
  enum State { kBeforeFirst, kValid, kPastEnd };

  static bool ParseEntry(const Slice& internal_key, Slice* user_key, ValueType* type) {
    SequenceNumber seq = 0;
    return ParseInternalKey(internal_key, user_key, &seq, type);
  }
  bool SameUserKey(const Slice& a, const Slice& b) const {
    return user_comparator_->Compare(a, b) == 0;
  }
  void RewindToRunStart() {
    Slice user_key;
    ValueType type = kTypeValue;
    if (!ParseEntry(internal_->key(), &user_key, &type)) return;
    internal_->Seek(BuildLookupKey(user_key, kMaxSequenceNumber));
  }
  void SkipCurrentRunForwardWithKey(const Slice& user_key) {
    while (internal_->Valid()) {
      Slice uk;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &uk, &type)) break;
      if (!SameUserKey(uk, user_key)) break;
      internal_->Next();
    }
  }
  void ScanForwardToVisible() {
    while (internal_->Valid()) {
      Slice user_key;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &user_key, &type)) {
        internal_->Next();
        continue;
      }
      if (type == kTypeValue) {
        SetValid(user_key);
        return;
      }
      SkipCurrentRunForwardWithKey(user_key);
    }
    state_ = kPastEnd;
  }
  void ScanBackwardToVisible() {
    while (internal_->Valid()) {
      Slice user_key;
      ValueType type = kTypeValue;
      if (!ParseEntry(internal_->key(), &user_key, &type)) {
        state_ = kPastEnd;
        return;
      }
      if (type == kTypeValue) {
        SetValid(user_key);
        return;
      }
      internal_->Prev();
      if (!internal_->Valid()) {
        state_ = kBeforeFirst;
        return;
      }
      RewindToRunStart();
    }
    state_ = kBeforeFirst;
  }
  void SetValid(const Slice& user_key) {
    current_user_key_.assign(user_key.data(), user_key.size());
    state_ = kValid;
  }

  Iterator* internal_;
  const Comparator* user_comparator_;
  State state_;
  std::string current_user_key_;
};

}  // namespace

Iterator* NewMemTableUserIterator(const MemTable* mem, const InternalKeyComparator& icmp) {
  return new UserIterator(mem, icmp);
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
      memtable_(new MemTable(internal_comparator_, memtable_capacity)) {}

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
  if (!bg_error_.ok()) return bg_error_;                   // 粘性 fail-stop（D11）
  {
    // L11/A31：**入队前**就拒绝关闭后的新写。若只在 RunFlusher 里判 closed_，
    // 关闭期间源源不断的新写者会不断把 flusher_active_ 置回 true，Close 等待 !flusher_active_
    // 就会活锁（实测：Close.RejectsNewWriters 挂住）。顺序必须是"先拒绝新写 → 再等在途批"。
    std::lock_guard<std::mutex> ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）
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

Status PersistentDBImpl::RunFlusher() {
  std::string payload;
  std::vector<Pending*> members;
  bool need_sync = false;
  SequenceNumber begin = 0;
  Status reject;

  // 取批**之前**的观察点：此处不持锁，其他写者仍可入队（A20 的确定性屏障）
  if (options_.commit_hook != nullptr) options_.commit_hook->OnBeforeGroupAssemble();

  {
    std::lock_guard<std::mutex> ql(commit_mu_);
    if (queue_.empty()) return Status::OK();               // 已被别的 flusher 处理
    std::lock_guard<std::mutex> ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）
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
      // 阻断项 1 的修复：判据必须与 MemTable::Add 同源，且按**整批**预估占用做预留校验。
      // 只查 IsFrozen() 会让"刚好触顶"的那批走接受路径（WAL 落盘 + 推进 sequence），
      // 随后 Add 返回 kFrozen ⇒ 被拒的写进了 WAL，重启后复活、同配置重开还会报 Corruption。
    } else if (memtable_->WouldReject(footprint)) {
      memtable_->Freeze();   // 触顶即冻结（与 Add 的行为一致）
      reject = Status::Frozen("Put/Delete: memtable is full (M2 无 flush)", dbname_);
    } else {
      last_sequence_ = begin + static_cast<SequenceNumber>(members.size()) - 1;
      payload = EncodeGroup(begin, members);
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

  // ---- 锁外做 IO（I17/L7）：Append 永远做，fsync 只在组内有人要求时做（I11/D3）----
  Status s = log_->Append(Slice(payload));
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
    std::lock_guard<std::mutex> ml(mutex_);
    bg_error_ = s;                                         // 粘性：偏移已不可信（D11）；I16 传播给整批
  }

  if (s.ok()) {
    std::lock_guard<std::mutex> ml(mutex_);
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
  std::lock_guard<std::mutex> l(mutex_);
  const std::string lookup_key = BuildLookupKey(key, last_sequence_);
  switch (memtable_->Get(Slice(lookup_key), value)) {
    case MemTable::GetResult::kFound:
      return Status::OK();
    case MemTable::GetResult::kDeleted:
      value->clear();
      return Status::NotFound("PersistentDBImpl::Get: key is deleted", key.size() < 64 ? key.ToString() : key.ToString().substr(0, 64));
    case MemTable::GetResult::kNotFound:
      return Status::NotFound("PersistentDBImpl::Get: key not found", key.size() < 64 ? key.ToString() : key.ToString().substr(0, 64));
  }
  return Status::NotFound("PersistentDBImpl::Get: unreachable", key.ToString());
}

Iterator* PersistentDBImpl::NewIterator() {
  std::lock_guard<std::mutex> l(mutex_);
  return NewMemTableUserIterator(memtable_.get(), internal_comparator_);
}

size_t PersistentDBImpl::pending_writers() {
  std::lock_guard<std::mutex> ql(commit_mu_);
  return queue_.size();
}

Status PersistentDBImpl::Sync() {
  // 与 flusher 的 fsync 串行；语义 = 把此前全部已返回 kOk 的写刷到磁盘（design §7.4）
  std::lock_guard<std::mutex> ql(commit_mu_);
  if (log_ == nullptr) return Status::IOError("PersistentDBImpl::Sync: WAL is not open", dbname_);
  if (!bg_error_.ok()) return bg_error_;
  const Status s = log_->Sync();
  if (s.ok()) {
    std::lock_guard<std::mutex> ml(mutex_);                // 锁序：commit_mu_ → mutex_（L8）
    durable_seq_ = last_sequence_;
  }
  return s;
}

Status PersistentDBImpl::Close() {
  std::unique_lock<std::mutex> ql(commit_mu_);
  bool already_closed = false;
  {
    std::lock_guard<std::mutex> l(mutex_);
    already_closed = closed_;
    closed_ = true;                                        // 幂等（A30）
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
  Status s = bg_error_.ok() ? log_->Sync() : bg_error_;    // Close 隐含 Sync（I20/A30）
  const Status c = log_->Close();
  if (file_lock_ != nullptr) EnvOf()->UnlockFile(file_lock_.release());   // D10：释放独占
  return s.ok() ? c : s;
}

Status PersistentDBImpl::RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr) {
  // 注入的 Env（A27~A31 的掉电语义测试用 MemEnv）；nullptr 时用真实 POSIX Env
  Env* env = options.env != nullptr ? options.env : Env::Default();
  if (!env->FileExists(name)) {
    const Status s = env->CreateDir(name);
    if (!s.ok()) return s;
  }

  // D10：进程级独占（评审阻断项 4 —— 用户已裁决纳入，但此前完全没接线）。
  // fcntl 写锁是**进程级**的：同一进程内二次 Open 不冲突，验证必须用双进程。
  FileLock* raw_lock = nullptr;
  const Status lock_status = env->LockFile(LockFileName(name), &raw_lock);
  if (!lock_status.ok()) return lock_status;      // 已被别的进程持有 ⇒ kIOError
  ScopedFileLock lock_guard(env, raw_lock);

  // ---- 第一遍：只扫描并规划（§5.2）----
  std::vector<std::string> children;
  Status s = env->GetChildren(name, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (const std::string& c : children) {
    uint64_t n = 0;
    if (ParseLogFileName(c, &n)) logs.push_back(n);
  }
  std::sort(logs.begin(), logs.end());
  const uint64_t hi = logs.empty() ? 0 : logs.back();

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
    s = env->Truncate(path, p.truncate_at);
    if (!s.ok()) return s;
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
  // 保守估计 MemTable 实际占用：编码字节 ×2（长度前缀/对齐/分配 slop）+ 每条节点开销 ×2 + 余量
  const size_t cap = std::max(
      options.write_buffer_size,
      static_cast<size_t>(total_payload) * 2 +
          static_cast<size_t>(total_entries) * kMemTableNodeOverhead * 2 + kRecoverySlack);
  std::unique_ptr<PersistentDBImpl> db(
      new PersistentDBImpl(options, InternalKeyComparator(options.comparator), name, cap));
  SequenceNumber last = 0;
  for (const Plan& p : plans) {
    for (const std::string& rec : p.records) {
      SequenceNumber seq = 0;
      std::vector<BatchEntry> entries;
      std::string why;
      if (!ParseBatch(Slice(rec), &seq, &entries, &why)) {
        return Status::Corruption("RecoverAndOpen: batch 解析失败", why);
      }
      if (seq <= last) continue;                     // D7 幂等 + 拒绝 sequence 回退
      for (size_t i = 0; i < entries.size(); ++i) {
        const Status a = db->memtable_->Add(seq + static_cast<SequenceNumber>(i), entries[i].type,
                                            entries[i].key, entries[i].value);
        if (!a.ok()) {
          return Status::Corruption("RecoverAndOpen: 重放 Add 失败", a.ToString());
        }
      }
      last = seq + static_cast<SequenceNumber>(entries.size()) - 1;
    }
  }
  db->last_sequence_ = last;

  // ---- 打开 log 继续追加 ----
  const uint64_t log_number = logs.empty() ? 1 : hi;
  const std::string hi_path = LogFileName(name, log_number);
  db->log_.reset(new WALWriter(env, hi_path));
  s = db->log_->Open(env->FileExists(hi_path));
  if (!s.ok()) return s;
  db->file_lock_.reset(lock_guard.release());   // 所有权交给 DB（Close/析构时释放）
  db->closed_ = false;
  *dbptr = db.release();
  return Status::OK();
}

}  // namespace lsm
