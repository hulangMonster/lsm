# M1 独立评审报告与处置（docs/m1-review.md）

> 评审对象：虚拟机 `~/lsm-kv`（= 本机只读克隆 `D:\JLProject\lsm-kv`），HEAD `a7b0052`。
> 评审方式：**独立 subagent**（#4 独立评审专家，与作者不同上下文），被要求攻击作者自曝的 5 个风险点，
> 并自带对抗性探针（`review_probe1/2/3.cpp`，编译运行于 VM `/tmp/rev/`）。评审期间仓库未做任何修改。
> 本文件由作者转写评审结论 + 记录处置；评审者的关键原文与原始输出照录，未做美化。

## 1. 阻断项（2 条，均已修复 + 补回归 + 全量重验）

### 阻断项 1：注入的 user comparator 只参与排序，不参与「相等/同段」判定

**评审原文（摘）**：
> 排序走 `InternalKeyComparator`→`user_comparator_->Compare`，但 `MemTable::Get` 用
> `user_key.compare(ExtractUserKey(lookup_key))`（`Slice::compare`＝逐字节）判命中，
> `UserIterator::SkipCurrentRunForwardWithKey` 用 `uk.compare(user_key)` 判「同 user key 重复段」。
> 实测（注入大小写不敏感比较器）：`Put("Key","v")` 后 `Get("KEY")` → **NotFound**；
> 再 `Put("KEY","w")` 后用户视图输出 **2 条 `[KEY] [Key]`，而反向遍历只输出 1 条**。

**作者补充（回归用例暴露的更深根因）**：不止"相等判定"。`MemTableKeyComparator` 在 internal key 等价时
退化为**整条条目字节序**比较，而探针条目的 user key 字节形态可能与真实条目不同
（探针用调用方给的 `"key"`，条目里存的是 `"Key"`）→ 逻辑相等的真实条目被排到探针**之前**，
`Seek` 跳过它。实测表现为「`Get("KEY")` 命中、`Get("key")` 落空」这种"同一条 key 两种写法结果不同"。

**修复**：
- `src/memtable.cpp`：`MemTable::Get` 的等价判定改走 `internal_comparator_->user_comparator()->Compare(...) == 0`；
  `MemTableKeyComparator::Compare` 在 internal key 等价时**只比 value 段**（不再比整条条目字节序）——
  这样探针（空 value）必然 ≤ 同 internal key 的真实条目，Seek 必能命中；同时保持严格弱序。
- `src/db.cpp`：`UserIterator` 引入 `SameUserKey()`，同段判定统一走注入的比较器。
- `docs/protocol.md` §7 同步修订比较器口径（原文只写了"整条字节序"）。
- **回归**：`MemTable.CustomComparatorEqualityIsHonored`（大小写不敏感比较器：跨大小写命中、等价 key 覆盖、
  用户视图只出 1 条、正反向条数一致、排序与去重并存）。

### 阻断项 2：protocol §6 的 MUST「解码前校验 size >= 8」在解码点未落实 → size_t 下溢越界读

**评审原文（摘）**：
> `DecodeEntryInternalKey` 只查 `len > input.size()`；长度 < 8 时 `ExtractUserKey` 的 `size()-8` 按
> `size_t` 下溢成 ~2^64，`Slice::compare` 变成任意长度 memcmp。两条 ASan 复现：
> ① 表内有 1000 字节 key 时 `mem.Get(Slice("ab"), &v)`；② `MemTableKeyComparator::Compare` 收到
> `internal_key_size=5` 的畸形态条目 —— 均报 `AddressSanitizer: stack-buffer-overflow ... READ of size 1000`。
> 诚实口径：M1 从 `DB::*` 触达不到，但这是冻结协议的 MUST，且 protocol §7 明说 M3 block entry 复用同一编码。

**修复**：
- `src/memtable.cpp`：`DecodeEntryInternalKey` 增加 `len < kInternalKeyMinSize` 拒绝；
  `MemTable::Get` 入口对 `lookup_key.size() < kInternalKeyMinSize` 直接返回 `kNotFound`。
- `src/common.h`：`InternalKeyComparator::Compare` 对 `size() < 8` 的畸形 internal key 退化为字节序比较
  （M3 的 block 解析会复用同一个比较器，输入来自磁盘）。
- **回归**：`MemTable.MalformedInputDoesNotReadOutOfBounds`（8 种短 lookup key、`internal_key_size=5` 的
  畸形态条目、畸形 internal key 进比较器；ASan 下必须无报告）。

## 2. 优化建议处置（7 条）

| # | 评审建议 | 处置 |
|---|---|---|
| 1 | `UserIterator::key()` 返回内部可变成员的 Slice，与 design §4.1「库内 Slice 一律指向 Arena」矛盾，且接口未写有效期 | **采纳**：`common.h` 的 `Iterator::key()/value()` 写明有效期契约（用户视图 key 在下次定位调用后失效，跨调用必须 `ToString()`）；design §4.4 同步 |
| 2 | `docs/m1-evidence.md` 缺 `Skiplist.RandomLayerDistribution` 数字 | **采纳**：补档（并给出 Release 与 ASan 两个构建输出一致的证据） |
| 3 | design §4.4 状态表把「Seek 无结果 / SeekToFirst 空表」写成 `kBeforeFirst`，实现与测试都是 `kPastEnd` | **采纳**：改表（实现对、表错），并注明这是 M3 替换 DBIter 时的不变契约 |
| 4 | `arena.cpp` 的 `new char[]` 与 design §6 的「stderr 定位 + abort」决策不符 | **采纳**：改 `new (std::nothrow)`，失败打印可定位信息后 `abort()`，决策与实现一致 |
| 5 | 注释承诺多于代码（`memtable_test.cpp` 首元素 Prev、非 2 的幂对齐） | **采纳**：补上真正的 `Prev` 断言；订正对齐注释（`AllocateAligned` 的前置条件就是 2 的幂） |
| 6 | 测试盲点：`Arena::Reset` 未断言 `BytesAllocated()`、重复段 Prev 顺序无断言、Env 文件类未禁拷贝、`NewWritableFile` 失败漏 fd | **采纳**：逐条补断言/补实现（禁拷贝 + `nothrow` 失败关 fd） |
| 7 | 两份小端实现（common.h vs coding.h）的取舍；建议把 LE 原语放 common.h、coding.h 转发 | **记录不改**：当前有 `util_test.cpp` 用 `DecodeFixed64` 交叉核对 common.h 的 trailer 字节序，漂移会被抓到；为冻结期最小改动，列入 M2 前的可选重构 |

## 3. 评审者逐条结论（原文摘录）

1) 接口设计：有疑虑（已随阻断项 1/2 与建议 1 处理）。Status 定位信息足够；Options 两字段都在用。
2) 内部 key 编码：通过（除阻断项 2 的解码点）。编解码与 protocol §6/§6.2 逐字一致；无主机序混用；`ParseInternalKey` 自身安全。
3) 跳表内存安全：通过。`kMaxHeight` 与数组长度一致、循环不变式在界内、placement new 后显式置空 12 个 atomic、节点随 Arena 回收。
4) 随机层高：通过（证据原未入档，已补）。固定种子 + 自带 Park–Miller ⇒ 跨构建/优化级别可复现。
5) 迭代器：通过（一处文档不一致，已改表）。两个三态实现无漏态、无死循环。
6) Arena 与析构：通过（OOM 口径已修）。4000 组随机 (bytes, align) 实测对齐/不重叠/内容不被覆盖全通过。
7) 无异常下的资源安全：通过。`DB::Open` 先置空出参再校验、失败不构造对象；析构顺序正确。
8) CMake 与门禁：通过。评审者特意在**全新目录**全量重建验证 0 warning（避免增量构建下 grep 计数的假阳性）。
9) 命名与注释：通过。少数注释承诺多于代码（已修）。

**额外专项（作者自曝风险点）**：`AllocateAligned` 任意 (bytes,align) 无问题；`Skiplist::Iterator::Prev` 的
run 感知前驱**无边界漏洞**（fwd == reverse(bwd)，重复段逐节点回退实测通过）；64 KiB 探针正确；
`UserIterator::Prev` 三态与断言一致；**prerequisites §9 登记的三处测试修订经复核「只改下标/期望值、未放宽校验」**
（评审者手算验证了 `seq=562949953421354=42+0x02<<48` 与 evidence 一致），并指出 §9 漏登记了第 4 处
（迭代器泄漏修复 `a7b0052`）——已补登记为 §9.5。

## 4. 作者自查补充（评审未覆盖的部分）

- 64 KiB 上限 key 的全链路（`MemTable.Add/Get`、内部迭代器、`DB::Put/Get`、用户视图）用独立小程序在 VM 上实测，
  结果：`Add(64KiB)=OK / Get(64KiB)=kFound size=1024 match=1 / Get(64KiB-1)=kNotFound /
  Add(64KiB+1)=InvalidArgument / 内部迭代 1 条 user_key_size=65536 / DB::Get=OK`。
- 期间出现过一次"Get 返回 kFound 但 value 为空"的假警报，根因是**作者检查程序**里 `printf` 实参求值顺序未指定
  （`out.size()` 早于 `Get()` 求值），与库无关；改为先取结果再打印后复验通过。记录在此以免误读。

## 5. 修复后的全量重验（原始输出见 docs/m1-evidence.md）

| 门禁 | 结果 |
|---|---|
| `bash scripts/lsm_build.sh`（干净重建 + 0 warning 断言） | **47/47 PASSED**（43 A 组含 2 个新回归 + 4 B 组压力） |
| ASan（`build-asan`，RelWithDebInfo） | **47/47 PASSED**，退出码 0，Address/Leak 报告 0 条 |
| TSan（`build-tsan`，`setarch -R`） | **47/47 PASSED**，退出码 0，race 报告 0 条 |
