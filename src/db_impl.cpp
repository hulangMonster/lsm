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

struct BatchEntry {
  ValueType type = kTypeValue;
  std::string key;
  std::string value;
};

// §9.4：payload := sequence(8B LE) || count(4B LE) || entry[0..count)
//        entry   := type(1B) || key_len(varint32) || key || [value_len(varint32) || value]
std::string EncodeBatch(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  std::string out;
  PutFixed64(&out, seq);
  PutFixed32(&out, 1);                                   // M2 的一次写 = 一个 entry 的 batch
  out.push_back(static_cast<char>(type));
  PutVarint32(&out, static_cast<uint32_t>(key.size()));
  if (!key.empty()) out.append(key.data(), key.size());
  if (type == kTypeValue) {
    PutVarint32(&out, static_cast<uint32_t>(value.size()));
    if (!value.empty()) out.append(value.data(), value.size());
  }
  return out;
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

  // M2.2 过渡形态：整条写路径串行化（每写一次 fsync），正确但低吞吐；M2.3 换成组提交。
  std::lock_guard<std::mutex> log_lock(log_mu_);
  SequenceNumber seq = 0;
  std::string batch;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::IOError("Put/Delete: DB is closed");
    if (!bg_error_.ok()) return bg_error_;                 // 粘性 fail-stop（D11）
    if (memtable_->IsFrozen()) {
      return Status::Frozen("Put/Delete: memtable is full (M2 无 flush)", dbname_);
    }
    seq = last_sequence_ + 1;
    batch = EncodeBatch(seq, type, key, value);
  }

  // 锁外做 IO（I17/L7）：Append 永远做，Sync 只在 sync=true 时做（I11）
  Status s = log_->Append(Slice(batch));
  if (s.ok() && options.sync) s = log_->Sync();
  if (!s.ok()) {
    std::lock_guard<std::mutex> l(mutex_);
    bg_error_ = s;                                         // 偏移已不可信 ⇒ 写只读
    return s;
  }

  {
    std::lock_guard<std::mutex> l(mutex_);
    const Status a = memtable_->Add(seq, type, key, value);
    if (!a.ok()) {
      bg_error_ = a;
      return a;
    }
    last_sequence_ = seq;
  }
  return Status::OK();
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

Status PersistentDBImpl::Sync() {
  std::lock_guard<std::mutex> log_lock(log_mu_);
  if (log_ == nullptr) return Status::IOError("PersistentDBImpl::Sync: WAL is not open", dbname_);
  if (!bg_error_.ok()) return bg_error_;
  return log_->Sync();
}

Status PersistentDBImpl::Close() {
  std::lock_guard<std::mutex> log_lock(log_mu_);
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (closed_) return Status::OK();                      // 幂等（A30）
    closed_ = true;
  }
  if (log_ == nullptr) return Status::OK();                // 尚未打开 WAL（恢复中途失败）⇒ 没有要刷的东西
  Status s = bg_error_.ok() ? log_->Sync() : bg_error_;    // Close 隐含 Sync（I20/A30）
  const Status c = log_->Close();
  return s.ok() ? c : s;
}

Status PersistentDBImpl::RecoverAndOpen(const Options& options, const std::string& name, DB** dbptr) {
  // 注入的 Env（A27~A31 的掉电语义测试用 MemEnv）；nullptr 时用真实 POSIX Env
  Env* env = options.env != nullptr ? options.env : Env::Default();
  if (!env->FileExists(name)) {
    const Status s = env->CreateDir(name);
    if (!s.ok()) return s;
  }

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
    for (const std::string& rec : p.records) total_payload += rec.size();
    plans.push_back(std::move(p));
  }

  // ---- 截断（I18 允许的唯一写）----
  for (const Plan& p : plans) {
    if (!p.truncate) continue;
    const std::string path = LogFileName(name, p.number);
    s = env->Truncate(path, p.truncate_at);
    if (!s.ok()) return s;
  }

  // ---- 第二遍：按 D12 放大容量后重放 ----
  const size_t cap = std::max(options.write_buffer_size,
                              static_cast<size_t>(total_payload) + kRecoverySlack);
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
  db->closed_ = false;
  *dbptr = db.release();
  return Status::OK();
}

}  // namespace lsm
