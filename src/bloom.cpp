// src/bloom.cpp —— filter block 的位级实现（M5.1）
//
// 契约：docs/m5-design.md §3.2（payload 布局与 8 条解析校验）、§3.3（哈希/参数/假阳性率）、
//       E2（逐桶补空过滤器）、§3.7 硬规则 3（不可用一律按「可能存在」）、
//       docs/protocol.md §12（M5.1 追加）。
#include "bloom.h"

#include "sstable/format.h"   // kFilterBaseLg / kBloomMinBits / kBloomMaxK（§3.3 的常量表）
#include "util/coding.h"

namespace lsm {

namespace {

// protocol §12.3 / §3.3：LevelDB util/hash.cc 的 32 位哈希，逐字实现（seed = 0xbc9f1d34）。
uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  // 4 字节一组混合。
  while (data + 4 <= limit) {
    h += DecodeFixed32(data);
    data += 4;
    h *= m;
    h ^= (h >> 16);
  }

  // 尾部 1~3 字节。
  switch (static_cast<size_t>(limit - data)) {
    case 3:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16;
      // fallthrough
    case 2:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8;
      // fallthrough
    case 1:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;
  }
  return h;
}

// §3.3：k = max(1, min(kBloomMaxK, round(bits_per_key · ln2)))。
// 用整数运算表达 round(x·0.693147)：`(bits*693147 + 500000) / 1000000`。
int BloomKFor(int bits_per_key) {
  long long num = static_cast<long long>(bits_per_key) * 693147 + 500000;
  int k = static_cast<int>(num / 1000000);
  if (k < 1) k = 1;
  if (k > kBloomMaxK) k = kBloomMaxK;
  return k;
}

// 内置 Bloom 策略（无状态：只持有 bits_per_key 与 k）。
class BuiltinBloomPolicy : public FilterPolicy {
 public:
  BuiltinBloomPolicy(int bits_per_key, const char* name)
      : bits_per_key_(bits_per_key), k_(BloomKFor(bits_per_key)), name_(name) {}

  const char* Name() const override { return name_; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    if (n <= 0) return;
    size_t bits = static_cast<size_t>(n) * static_cast<size_t>(bits_per_key_);
    if (bits < kBloomMinBits) bits = kBloomMinBits;
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;   // 回填为整字节的位数（§3.3「m = bytes*8」）

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_));
    char* array = &(*dst)[init_size];

    for (int i = 0; i < n; ++i) {
      uint32_t h = BloomHash(keys[i]);
      // Double Hashing：delta = (h >> 17) | (h << 15)（循环右移 17 位）。
      // protocol §12.3：delta == 0 时退化成「k 次探测同一个位」，必须做一次保护
      // （写方与读方用**同一个**常数，保证位级结果一致）。
      uint32_t delta = (h >> 17) | (h << 15);
      if (delta == 0) delta = 0x9e3779b9u;
      for (int j = 0; j < k_; ++j) {
        const uint32_t bitpos = h % static_cast<uint32_t>(bits);
        array[bitpos / 8] = static_cast<char>(static_cast<unsigned char>(array[bitpos / 8]) |
                                             (1u << (bitpos % 8)));
        h += delta;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
    return BloomKeyMayMatch(key, filter);
  }

 private:
  int bits_per_key_;
  int k_;
  const char* name_;
};

const char kBuiltinBloomPolicyName[] = "leveldb.BuiltinBloomFilter2";

// 进程内按 bits 缓存（magic static ⇒ C++11 起线程安全初始化；对象建成后只读，L30）。
class BloomPolicyTable {
 public:
  BloomPolicyTable() {
    for (int b = 0; b <= 64; ++b) {
      policies_[b].reset(new BuiltinBloomPolicy(b, kBuiltinBloomPolicyName));
    }
  }
  const FilterPolicy* Get(int b) const { return policies_[b].get(); }

 private:
  std::unique_ptr<FilterPolicy> policies_[65];
};

const BloomPolicyTable& Policies() {
  static const BloomPolicyTable table;
  return table;
}

}  // namespace

uint32_t BloomHash(const Slice& key) { return Hash(key.data(), key.size(), 0xbc9f1d34); }

bool BloomKeyMayMatch(const Slice& key, const Slice& filter) {
  const size_t len = filter.size();
  if (len < 2) {
    // §3.3：len == 0 ⇒ false（空桶没有 key）；len == 1 ⇒ true（只有 k，保守判「可能存在」）。
    return len == 1;
  }
  const size_t bits = (len - 1) * 8;
  const uint8_t k = static_cast<uint8_t>(filter[len - 1]);
  const char* array = filter.data();
  // k 的合法上界是 kBloomMaxK（§3.3）；超出说明是畸形/未知编码 ⇒ 保守判「可能存在」
  // （与 LevelDB 的 `k > 30 ⇒ true` 同形，避免用未校验的 k 做长循环）。
  if (k > kBloomMaxK) return true;

  uint32_t h = BloomHash(key);
  // 与写方**逐字相同**的 delta 保护（protocol §12.3）。
  uint32_t delta = (h >> 17) | (h << 15);
  if (delta == 0) delta = 0x9e3779b9u;
  for (uint8_t j = 0; j < k; ++j) {
    const uint32_t bitpos = h % static_cast<uint32_t>(bits);
    const unsigned char byte = static_cast<unsigned char>(array[bitpos / 8]);
    if ((byte & (1u << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

const FilterPolicy* NewBuiltinBloomPolicy(int bits_per_key) {
  if (bits_per_key < 1 || bits_per_key > 64) return nullptr;
  return Policies().Get(bits_per_key);
}

// ============================== FilterBlockBuilder ==============================

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy) : policy_(policy) {}

FilterBlockBuilder::~FilterBlockBuilder() = default;

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // E2：target = block_offset >> kFilterBaseLg；逐桶补空过滤器，保证
  // filter_offsets_.size() > target ⇒ reader 的 index 永远落在有效 filter 上。
  const uint64_t target = block_offset >> kFilterBaseLg;
  while (target > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& user_key) { keys_.emplace_back(user_key.ToString()); }

void FilterBlockBuilder::GenerateFilter() {
  // 先记偏移，再写内容 ⇒ 空桶的 offset[i] == offset[i+1]（0 字节 filter，§3.2）。
  filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
  if (keys_.empty() || policy_ == nullptr) return;

  std::vector<Slice> slices;
  slices.reserve(keys_.size());
  for (const std::string& s : keys_) slices.emplace_back(s);
  policy_->CreateFilter(slices.data(), static_cast<int>(slices.size()), &result_);
  keys_.clear();
}

Slice FilterBlockBuilder::Finish() {
  if (finished_) return Slice();
  if (!keys_.empty()) GenerateFilter();

  const uint32_t array_offset = static_cast<uint32_t>(result_.size());
  for (uint32_t off : filter_offsets_) PutFixed32(&result_, off);
  PutFixed32(&result_, array_offset);
  PutFixed32(&result_, static_cast<uint32_t>(filter_offsets_.size()));
  finished_ = true;
  keys_.clear();
  return Slice(result_);
}

size_t FilterBlockBuilder::CurrentSizeEstimate() const {
  return result_.size() + filter_offsets_.size() * sizeof(uint32_t) + 2 * sizeof(uint32_t);
}

// ============================== FilterBlockReader ==============================

FilterBlockReader::FilterBlockReader(const Slice& contents)
    : contents_(contents.data(), contents.size()) {
  const size_t size = contents_.size();
  if (size < 8) return;   // 校验 1
  const char* p = contents_.data();
  const uint32_t n = DecodeFixed32(p + size - 4);
  const uint32_t array_offset = DecodeFixed32(p + size - 8);

  // 校验 3：array_offset + 4*n + 8 == payload.size()（无 padding）。
  if (static_cast<uint64_t>(array_offset) + 4ull * static_cast<uint64_t>(n) + 8ull !=
      static_cast<uint64_t>(size)) {
    return;
  }
  if (n == 0) {
    // 校验 4：空 filter block ⇒ array_offset == 0 && payload.size() == 8。
    if (array_offset != 0) return;
    num_ = 0;
    array_offset_ = 0;
    valid_ = true;
    return;
  }
  if (static_cast<uint64_t>(array_offset) + 4ull * static_cast<uint64_t>(n) >
      static_cast<uint64_t>(size)) {
    return;
  }

  const char* off = p + array_offset;
  // 校验 5：offset[0] == 0；单调不减；offset[n-1] <= array_offset。
  uint32_t prev = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t cur = DecodeFixed32(off + 4 * i);
    if (i == 0) {
      if (cur != 0) return;
    } else if (cur < prev) {
      return;
    }
    prev = cur;
  }
  if (prev > array_offset) return;

  // 校验 6/7：每个 filter 的 [begin, end) 自洽；len == 1 视为损坏（保守：整块禁用）。
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t begin = DecodeFixed32(off + 4 * i);
    const uint32_t end =
        (i + 1 < n) ? DecodeFixed32(off + 4 * (i + 1)) : array_offset;
    if (begin > end) return;
    if (end - begin == 1) return;   // 只有 k 没有 bitset ⇒ kCorrupt
  }

  num_ = n;
  array_offset_ = array_offset;
  valid_ = true;
}

FilterBlockReader::~FilterBlockReader() = default;

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& user_key) const {
  if (!valid_) return true;   // 不可用 ⇒ 按「可能存在」（§3.7 硬规则 3）
  const uint64_t index = block_offset >> kFilterBaseLg;
  if (index >= num_) return true;   // 越界（畸形文件）⇒ 保守
  const char* off = contents_.data() + array_offset_;
  const uint32_t begin = DecodeFixed32(off + 4 * index);
  const uint32_t end = (index + 1 < num_) ? DecodeFixed32(off + 4 * (index + 1))
                                          : static_cast<uint32_t>(array_offset_);
  if (begin > end) return true;   // 构造期已校验；防御性兜底
  return BloomKeyMayMatch(user_key,
                          Slice(contents_.data() + begin, static_cast<size_t>(end - begin)));
}

}  // namespace lsm
