// src/filter_policy.h —— filter 策略接口（M5.1 新增；只依赖 common.h）
//
// 契约来源：docs/m5-design.md §3.5（签名草案）、§3.3（参数与哈希）、§4 §12（protocol 追加章节）。
// 依赖纪律（roadmap §2、M5-C7）：本头文件**只**依赖 common.h；`Options` 不得持有
// `const FilterPolicy*`（否则 common.h → filter_policy.h 成环），只持有 `int bloom_bits`。
#ifndef LSM_FILTER_POLICY_H_
#define LSM_FILTER_POLICY_H_

#include <string>

#include "common.h"

namespace lsm {

// 策略接口：无状态（除参数），可跨线程共享。
// 线程约束（M5-design §3.5 / L30）：实例一经创建即**只读**；`CreateFilter`/`KeyMayMatch` 不修改自身。
class FilterPolicy {
 public:
  virtual ~FilterPolicy() = default;
  // 策略名；内置 Bloom 返回 "leveldb.BuiltinBloomFilter2"（与 protocol §12.1 的名字空间同源）。
  virtual const char* Name() const = 0;
  // 把 n 个 key 的位图**追加**到 *dst（LevelDB 语义：`bitset ‖ k(1B)`，不覆盖已有内容）。
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;
  // 返回 false ⇒ 该 key **一定不在** filter 覆盖的集合里；true ⇒ 可能存在（或 filter 不可用）。
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// 工厂：返回按 bits_per_key 参数化的内置 Bloom 策略。bits_per_key ∈ [1,64]；越界返回 nullptr。
// 返回的对象由实现按 bits 缓存（进程内静态、magic static 初始化 ⇒ 线程安全），调用方**不得** delete。
const FilterPolicy* NewBuiltinBloomPolicy(int bits_per_key);

}  // namespace lsm

#endif  // LSM_FILTER_POLICY_H_
