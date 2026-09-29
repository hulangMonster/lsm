# M2 设计（docs/m2-design.md）—— WAL 与崩溃恢复

> 状态：**#0 设计定稿，待用户评审**（`M2-WAL与崩溃恢复.md` §0 的硬性闸门：评审批准前不得进入 #1）。
> 冻结范围（批准后）：§4 的 WAL record 位级布局 + §5 的恢复算法与损坏判定 + §6 的组提交协议与锁纪律 +
> §7 的 `sync` 语义表 + §8 的对账协议 + §9 的测试矩阵 + §10 的子里程碑判据。M2.1~M2.3 不得私自偏离。
>
> 本文件是 #0 唯一交付物。**不写业务实现代码**；唯一的新文件就是本文件。
> 决策记录：13 项开放决策的「方案 → 取舍 → 推荐」见 §2，其中 8 项标注**需用户拍板**。
>
> 依赖的冻结契约：`docs/m1-design.md`（接口/不变量/Arena/MemTable/Env）、`docs/protocol.md`（位级编码，§4.6 给出追加章节的 patch 文本）、
> `docs/roadmap.md`（阶段边界）、`docs/m1-prerequisites.md`（I1~I10 / L1~L6）、`docs/m1-review.md`（#4 评审暴露的坑）。
> 只读参照：raft-kv `docs/m5-design.md` §6（两段式持久化）、`docs/m5-review.md` §3（组提交丢唤醒与 P2a）、
> `scripts/fsbench_commit_latency.cpp`（微基准口径）。

---

## 1. 目标 / 非目标 / 与 M1 的关系

### 1.1 目标（对应 `M2-WAL与崩溃恢复.md` §0 的 6 条）

| # | 目标 | 本设计的落地位置 |
|---|---|---|
| G1 | 顺序写 WAL：record 二进制格式（长度 + 类型 + CRC32C）、跨块切分与重组、损坏检测 | §4 |
| G2 | `DB::Open` 崩溃恢复：扫描 WAL → 逐条重放 → 重建 MemTable 与 sequence → 安全启动 | §5 |
| G3 | fsync 策略：`sync = true` = durable-before-ack；`sync = false` 由 OS 决定 + 显式 `Sync()`；策略可配 | §7 |
| G4 | 组提交：多个并发写者合并为一次 fsync（单 flusher + 条件变量交接），无丢唤醒、失败正确传播 | §6 |
| G5 | 崩溃对账脚本：kill -9 随机时刻循环 → 重启 → 已 ack 写集合与恢复后集合对账，`missing != 0` 退出码 1 | §8 |
| G6 | fsync 成本基线：独立微基准记录 fsync / fdatasync / 预分配+fsync / O_DIRECT 的单次成本 | §9.3、§11.2 |

### 1.2 非目标（M2 硬边界，评审逐条对照）

**M2 实现与测试中不得出现下列任何符号、文件、字段或占位实现（stub 也不行）**：

- SSTable（`table.{h,cpp}`、`block_builder`、`footer`、`index block`、`.sst`/`.ldb` 扩展名、`RandomAccessFile`）
- flush / `CompactMemTable` / `MakeRoomForWrite` 的落盘分支 / 后台 flush 线程
- compaction / `Version` / `VersionSet` / `MANIFEST` / `CURRENT` / 层级（L0..Ln）/ 文件号回收
- Bloom Filter / filter block / `KeyMayMatch`
- **WriteBatch 的对外接口**（仅允许 **WAL 内部按 batch 组织一次写**，见 §4.7；不得新增 `WriteBatch` 类/头文件/公共 API）
- 块缓存 / LRU cache / 压缩算法（Snappy 等）
- 快照与多版本读视图（`ReadOptions::snapshot`、`Snapshot*`）——M2 的读快照恒为 `last_sequence_`
- 与 raft-kv 的对接（M6）
- **WAL 文件的删除**：只定义接口与判据（§3.3），**不实现**（M3 之后才能安全回收）

**必须显式登记的三条边界取舍**（§12 自检会再核一遍）：

1. MemTable 触顶后 M1 的 `kFrozen` 语义在 M2 **原样保留**：`DBImpl::Write` 把 `Status::Frozen` 原样返回且**不写 WAL、不消耗 sequence**；M3 引入 flush 后由 `MakeRoomForWrite` 消化。M2 的测试统一显式传入足够大的 `write_buffer_size`（沿用 M1 B 组 §7.6 的做法）。
2. M2 的 `DBImpl` **只有一个 MemTable**，恢复即全量重放；不允许出现"多 MemTable / immutable memtable"的影子实现。
3. M2 **不引入 `Options::repair`**（§2 D6）：尾部损坏自动截断 + 打日志，中间损坏拒绝启动。理由：M2 阶段 WAL 是唯一真相源，"静默修好并继续"比"拒绝启动"的风险高一个量级。

### 1.3 与 M1 的关系

**逐字复用（不改一行）的 M1 冻结契约：**

| 契约 | 位置 | M2 如何用 |
|---|---|---|
| `Slice` / `Status`（含 `kCorruption` 的首次真实产生点） | `common.h` | WAL 读写接口一律走 `Slice`；`kCorruption` 在 M1 是"预留码"，**M2 是它第一次被真正产生**（§5.3） |
| 内部 key 编码与比较（`PackTrailer` / `BuildInternalKey` / `InternalKeyComparator` §6.1 降序） | `common.h`、`protocol.md` §6 | 重放时按 `(seq, type)` 重建条目；多版本排序依赖 M1 的降序规则，**M2 一个字都不改** |
| `MemTable::{Add, Get, NewIterator}` + `kFrozen` | `memtable.{h,cpp}` | 重放路径的唯一写入入口；容量口径沿用 `BytesAllocated() + sizeof(MemTable)` |
| `Arena` | `util/arena.h` | MemTable 内部；M2 另用一块 **普通 `std::string`** 作 WAL 组提交缓冲（不占 Arena——WAL 缓冲需要可增长且不随 MemTable 释放） |
| `Env`（文件 + `CLOCK_MONOTONIC` 时间） | `util/env.h`、`env_posix.cpp` | WAL 文件 IO 与计时的唯一入口；M2 **只增补**接口（§5.7），不改已有语义 |
| `SequenceNumber` / `ValueType` / `kMaxUserKeySize` / `kMaxSequenceNumber` | `common.h` | batch 起始 sequence 与 count 的合法性校验 |
| `coding`（varint32/64、fixed、length-prefixed） | `util/coding.{h,cpp}` | §4.7 batch payload 逐字复用 `PutVarint32` / `PutLengthPrefixedSlice` |
| `crc32c`（`Value` / `Extend`） | `util/crc32c.h` | §4.4：M1 只做纯函数与向量测试，**M2 是它的第一个真实使用者** |
| `DB` 接口（只允许增补） | `db.h` | 新增 `WriteOptions` 重载 + `Sync()` + `Close()`；M1 的无 options 重载保留为 `sync=false` 的等价形式 |
| `DB::Open` 的 `name` 语义 | `m1-design.md` §4.5 | `name` 为空 → 内存模式（M1 行为逐字保留）；**`name` 非空 → 从 `kNotSupported` 变为真实持久化实现** |
| 迭代器三态状态机、用户视图去重/跳 tombstone | `m1-design.md` §4.4 | M2 不碰；恢复后 `Get`/迭代器的可见语义不变 |
| `L1~L6` 锁与生命周期纪律 | `m1-prerequisites.md` §2 | M2 沿用并新增 `L7~L12`（§6.6） |

**M1 定义了、但直到 M2 才真正被用起来的接口**（M1 里它们是"有测试覆盖的前瞻接口"，不是死代码）：

| M1 接口 | M1 状态 | M2 的用法 |
|---|---|---|
| `WritableFile::Sync()` | 只有 `Env.FileRoundTrip` 覆盖，注释写"（M2 的 commit 语义依赖它）" | durable-before-ack 的落点（I11） |
| `WritableFile::Flush()` | POSIX 实现是 `Status::OK()` 空操作 | WAL 追加**不经用户态缓冲**（§11.3 实测：stdio 缓冲才是 torn tail 的真凶），故 `Flush()` 在 WAL 路径上保持空操作语义 |
| `Env::NewSequentialFile` / `GetFileSize` / `RenameFile` | 仅 `util_test` 用 | 恢复扫描；`RenameFile` M2 **不用**（留给 M3 的 `CURRENT` 原子切换） |
| `crc32c::Extend` | 仅向量测试 | 跨块 record 的流式 CRC 累加（§4.5） |
| `Status::kCorruption` | M1 声明"M1 不产生，预留给 M2/M3" | §5.3 中间损坏 + §4.2 header/payload 格式违规 |
| `Status::kFrozen` | M1 产生 | M2 原样透传（§1.2 边界 1） |
| `Status::kIOError` | 仅 Env 层 | §6.4 失败传播：`fsync`/短写/ENOSPC 一路带着上下文回到调用方 |
| `Options::write_buffer_size` | 仅 MemTable 容量 | M2 恢复期用它决定 MemTable 初始容量（§5.5），写路径用它做冻结判据 |
| `InternalKeyComparator::user_comparator()` | 仅编码测试用 | 恢复时重放记录的排序与 `MemTable::Get` 的等价判定都经它 |

**M1 明确"留给 M2"，M2 必须兑现的：**

- `Options` 不加 `WriteOptions` 以外的新字段（`m1-design.md` §1.4：M2 加 sync 时以**新增重载**形式补）。
- `m1-review.md` §2 建议 7 的"两份小端实现（`common.h` vs `coding.h`）合并"**列入 M2 前的可选重构，M2 不做**（保持最小改动，M3 之前不引入与 WAL 无关的重构）。

---

### 1.4 M2 不变量与锁纪律的映射（I11~I20 / L7~L12）

`M2-WAL与崩溃恢复.md` §1 的 `#1` 段要求新增 I11~I20 与 L7~L12。它们**正式定义**在 `docs/m2-prerequisites.md`（`#1` 产物），
但设计必须现在就说清"每条由谁保证、在哪一节落地、由哪个用例验证"，否则 `#1` 只能凭空写不变量。

| 不变量 | 一句话 | 本设计的保证点 | 验证用例 |
|---|---|---|---|
| **I11** | durable-before-ack：`sync=true` 返回 `kOk` 前该记录已 `write` 且 `fsync` 成功（双检查） | §6.4 的"双检查"+ §7.1 的三段式论证（`Append` 字节数 / `Sync` 返回值 / 同一临界区发布水位） | A10、A22、A29、A30；B01 |
| **I12** | WAL 是未刷盘数据的唯一真相源：M2 恢复 = 全量重放 WAL | §5.1/§5.2（扫描**所有** `*.log`）+ §3.3（M2 任何 log 都不可删除） | A11、A12；B01 |
| **I13** | 重放顺序 = 写入顺序 = sequence 升序；恢复后下一次写入的 sequence 严格大于任何已重放记录 | §5.2（按编号升序 + 字节顺序，**不重排序**）+ §5.4（`last_sequence_ = max_replayed`）+ §6.2/§6.3（`log_mu_`/`commit_mu_` 覆盖 sequence 分配 ⇒ WAL 顺序 = seq 顺序） | A13、A19、A27（重放 seq 严格递增）；**§12.6 Q7 的措辞澄清** |
| **I14** | 半条 record 永不生效：不完整则截断到最后一条完整 record，被截断部分不得对查询可见 | §4.3（`TAIL_RESIDUE`）+ §5.3（判定表 + 只截断到 `last_good_end`） | A06、A08、A15；B03 |
| **I15** | 组提交的原子单位是"批"：批内全 durable 或全不 durable，等待者不得看到部分成功 | §4.7（**一条 record = 一个原子批**，一个 CRC）+ §6.3（`need_sync` 取组内 OR；`w.done` 只在整批结算后置位） | A20、A22、A23 |
| **I16** | `fsync` 失败必须传播给该批**所有**等待者，不得静默返回 `kOk`；失败后 WAL 状态明确 | §6.4（粘性 `commit_error_` + `bg_error_`）+ D11（fail-stop 写只读） | A10、A23；（新增）`GroupCommit.WriteAfterErrorIsRejected` |
| **I17** | 持锁零 IO：WAL 的 `write`/`fsync` 一律在 DB 互斥锁之外 | §6.2/§6.3 的 `cl.unlock()` / 释放 `mutex_` 位置 + L7 | A25（`MuHeldGuard` + `SpyLogWriter` 断言持锁期 IO 调用数 == 0） |
| **I18** | 恢复只读：不写 WAL、不触发 flush、不修改既有文件（除显式 repair 截断） | §5.5（唯一写 = 尾部 `ftruncate`，有日志有判据）+ §1.2 边界 2（无 flush） | A14（幂等：WAL sha1 不变）、A18 |
| **I19** | 无撕裂值：任一 key 只能是某个**完整版本**，不得出现半写 value | §7.3 第 1 条（value 只存在于完整 record 内）+ §4.3 截断到 record 边界 | A15、A27 |
| **I20** | 关闭语义：`Close()`/析构在 `sync=true` 下保证此前全部已 ack 数据 durable；关闭后无后台线程访问已释放对象 | §6.5（拒绝新写 → 等在途批 → `Sync` → `Close`，顺序不可换；M2 无后台线程 ⇒ 结构性成立） | A30、A31 |
| **L7** | DB 互斥锁只保护内存状态，不保护 IO | §6.1 的状态划分 + §6.2/§6.3 的解锁点 | A25 |
| **L8** | 组提交队列由独立互斥量保护；锁序固定 `commit_mu_ → mutex_`，禁止反向 | §6.1（两把锁）+ §6.6（M2 只有 3 处同时持两锁，逐处可核对） | A26（TSan）+ 代码评审 |
| **L9** | 条件变量谓词必须覆盖「我已完成」与「我能接手当 flusher」两个条件 | §6.3 的谓词 `while (!w.done && !(!flusher_active_ && queue_.front() == &w))` + 四场景分析 | A21（`wait_for(200ms)` 超时即 FAIL） |
| **L10** | 交接必须在仍有未结算成员时通知；清 `flusher_active_` 与唤醒新队首的顺序不可交换，不得依赖下一次写入唤醒 | §6.3 的 ②（两步顺序）+ §6.5 的防御性 `notify_all` | A21（场景 1/3 的精确时序） |
| **L11** | 关闭路径与并发写者的交互：拒绝新写 → 等在途批 → 再销毁 | §6.5 | A31 |
| **L12** | 恢复是单线程的；恢复期间不得有后台线程运行 | §6.5 末（M2 无后台线程 ⇒ 结构性成立）+ §5.2（`RecoverAndOpen` 不创建线程） | `Recovery.*` 全部用例单线程 + 代码评审 |

**这张表的用途**：`#1` 写 `m2-prerequisites.md` 时逐行抄"保证点"与"验证用例"两列即可，不需要重新推导；
`#4` 评审时逐行核对"保证点"的代码位置是否真的成立。

---

## 2. 开放决策记录（方案 → 取舍 → 推荐）

> **拍板标记**：`【需拍板】` = 影响面超出实现细节，请用户明确批准或改选；`【默认采纳】` = 若无反对，按推荐执行。

### D1 WAL record 格式 【需拍板】

| 方案 | 取舍 |
|---|---|
| **A. LevelDB 风格：32 KiB 固定块 + 7 字节头（CRC32C(4) LE + length(2) LE + type(1)）+ type ∈ {FULL,FIRST,MIDDLE,LAST}，大 record 跨块切分** | 优点：① 损坏定位粒度 = 单条 record，且 CRC 自包含；② 1 MiB value 无需整体缓冲（流式切块，§4.5）；③ 块大小固定 ⇒ 预分配与 O_DIRECT 对齐友好；④ M3 的 block 读写复用同一"7 字节头 + CRC"心智模型。缺点：跨块状态机 + 块尾 padding + reader 重组，实现与测试面最大 |
| **B. 单条变长 record：`crc32c(4) \| length(4) \| payload`** | 优点：实现最简，reader 无需块状态。缺点：① 大 value 必须整体缓冲（M2 的 4 MiB MemTable 下可行，但 M3/M5 会立刻成为瓶颈）；② **无块边界 ⇒ header 的 length 损坏时无法 resync**，只能整文件截断，"中间损坏"与"尾部损坏"无法区分（直接违反 M2 验收口径）；③ 损坏定位粒度粗 |
| **C. 长度前缀 + 尾部 CRC** | 最弱：torn write 时 CRC 根本没写，只能靠"长度不足"检出；length 损坏同样无法 resync。**否决** |

**推荐 A。** 决定性理由不是"LevelDB 这么做"，而是 **B/C 在"中间损坏必须可定位、尾部损坏必须可截断"这条 M2 硬判据上是结构性不可达的**：没有独立可校验的定长块边界，reader 在 header 损坏处就失去了继续前进的能力。A 的额外实现成本（跨块状态机）在 §4.3 用 30 行伪代码封闭，且是 M3 直接复用的资产。
**拍板点**：若用户认为 M2 应优先压实现复杂度、接受"中间损坏 = 拒绝启动但无法定位到 record"，可选 B；本设计按 A 展开。

### D2 CRC 覆盖面 【需拍板】

| 方案 | 取舍 |
|---|---|
| **A. `crc = crc32c(length(2) \|\| type(1) \|\| payload)`**（覆盖 header 中除 CRC 自身外的全部字段） | 优点：length 的完整性成为 CRC 契约的一部分；reader 可以在验 CRC 之后再信任 length（虽然为防越界读仍必须先做长度合法性检查）；与指令决策 2 的"header + payload"字面一致。代价：比 LevelDB 多 3 字节参与 CRC（零额外存储、零额外 syscall） |
| **B. LevelDB 口径：`crc = crc32c(type(1) \|\| payload)`**（不覆盖 length） | 优点：与 LevelDB 完全一致，便于对照。缺点：length 被篡改的情形只能靠"用错的长度去取 payload ⇒ CRC 必失败"间接检出；且"length 合法但语义错"（如 5→6）的检出依赖 payload 内容差异，非确定性契约 |
| **C. 用 xxHash/CRC64 等更强校验和** | 无必要：本项目只做**损坏检出**（非对抗性），CRC32C 已在 M1 冻结且 M3 复用；换算法会分裂 M1 的 `crc32c` 契约 |

**推荐 A。** 成本为零、语义更强，且让 §4.1 的字段表能写出一句干净的话：「CRC 覆盖 CRС 字段之后的全部字节」。M1 的 `crc32c::Extend` 直接支持流式累加（先 `Value(3 字节前缀)` 再逐块 `Extend`）。
**拍板点**：这是对 LevelDB 惯例的一处**有意偏离**，需用户确认（若要求与 LevelDB 逐字节兼容以便对照，选 B）。

### D3 组提交的边界 【需拍板】

| 子问题 | 方案 | 取舍 | 推荐 |
|---|---|---|---|
| 谁当 flusher | **A. 首到达者（队首）当选，无后台线程** / B. 固定后台 IO 线程 / C. 每批重新竞选 | A：零额外线程、空闲时零延迟（p=1 最优）、与 LevelDB 同构；B：写路径只入队+等待，逻辑更集中，但多一个线程的生命周期/关闭顺序（L11/L12）负担，且空闲时至少多一次上下文切换的延迟；C：thundering herd | **A** |
| 一批的范围 | **A. 从队首起连续取，上限 `kMaxGroupBytes = 1 MiB` 且 `kMaxGroupRecs = 64`，且至少含队首自己** / B. 无上限（取空队列） / C. 固定条数 | A：兼顾吞吐与尾延迟；单个超大 value 不会饿死别人（它自己独占一批）；B：一个 1 MiB value 之后的写者要等它写完；C：小记录下批太小，大并发下 fsync 次数仍多 | **A** |
| 批的编码粒度 | **A. 把整批合并成「一条」WAL record**（一个 batch payload，含多个 entry，§4.7）/ B. 每个写者一条 record，多条 record 一次 fsync | A：**一条 record = 一个 CRC = 一个原子单位**，I14/I15 有字节级判据；一次 `write()` 而非 N 次；与 M5 `WriteBatch` 布局同构（M5 只加公共 API）。B：torn write 会落在批中间，虽然 fsync 成功即全部持久，但"批原子性"只能靠 fsync 语义而非字节结构保证 | **A** |
| 失败传播 | **A. 粘性 `commit_error_`：首个错误写入后永不清除，当前与后续所有等待者都拿到同一 `Status`，无人拿到 `kOk`** / B. 每批独立错误 / C. 重试 N 次 | A 直接满足 I16；B 会让后续批在"文件偏移已不可信"的状态下继续追加 → 产生中间损坏；C 见 D11 | **A** |
| 混合 sync | **A. 批内任一成员要求 sync ⇒ 本批必须 fsync** / B. 只看队首的 `sync`（LevelDB 的实际行为） | A 正确；B 会让"队首 `sync=false`、组员 `sync=true`"时，组员在 fsync 之前被 ack → 违反 I11 | **A（明确优于 LevelDB）** |

**拍板点**：合并成单条 record（A）意味着「**组提交的批 = WAL 的一条 record**」，这会写进 `protocol.md` 的 payload 章节，需用户确认。

### D4 flush 窗口放开时机 【需拍板】

- **方案 A（推荐）：`flusher_active_` 只在「`Append` + `Sync` 均返回 → `written_seq_`/`durable_seq_` 已更新 → 组内成员已全部出队、`done=true` 并 `notify_one` → **最后**清 `flusher_active_` → 立刻 `notify_one` 新队首」之后才放开，全部在同一把 `commit_mu_` 临界区内完成。**
- 方案 B：`Sync()` 返回即放开（在回锁发布水位之前或之后立刻清标志）。
- 方案 C：`Append()` 返回即放开 `Sync` 之前的部分窗口。

**推荐 A。** 依据是 raft-kv 的 P2a 复盘（`docs/m5-design.md` v2.10 / `docs/m5-review.md` §3）：`syncInFlight_` 在 fsync 返回即放开 → 上一批仍在做阻塞发送、下一批已开始 fsync，**多个 flusher 在同一把 `mu_` 上排队**（peer=2 的"拿到锁之前"中位 **11.5 ms**，而一次真正往返 **<1 ms**）；修复（等本批发送全部发出后再放开窗口）后 p=8 从 **0.47× 恢复到 1.34×**、p=64 到 **2.20×**。

lsm-kv 的对应机制：若在 `Sync()` 返回后立刻清 `flusher_active_`，新队首会立即成为第二个 flusher 并再次 `Append`+`Sync` 同一个 fd——`write()` 在 `O_APPEND` 下原子，但：
1. **两个 fsync 会在内核块层排队**（ext4 journal commit 串行），批间延迟被拉长；
2. 更严重的是**顺序风险**：新 flusher 的 `Append` 与旧 flusher 的 `Sync` 交错时，文件里 record 的**落盘顺序**不再等于 sequence 顺序，I13 的"重放顺序 = 写入顺序 = sequence 升序"在恢复侧虽因 MemTable 按 sequence 排序而不致命，但会让"尾部截断到最后一条完整 record"失去"被截断的一定是最大 sequence"这一性质（§5.4 的论证依赖它）。

**回归用例**：`GroupCommit.WindowNotOpenedEarly`（§9.1 M2.A20）在"窗口内"计数新形成的 flusher 数，断言为 **0**。

### D5 `sync` 配置语义 【需拍板】

| 方案 | durable 保证 | 取舍 |
|---|---|---|
| **A（推荐）：`WriteOptions::sync` 默认 `false`；`sync=true` 逐条 durable；`sync=false` 仍写 WAL 但不 fsync；另提供 `DB::Sync()`（刷此前全部写入）与 `DB::Close()`（隐含 Sync）** | 见 §7 的表 | 与 LevelDB 的 `WriteOptions` 形态一致；默认不牺牲吞吐；对账门禁显式用 `sync=true`；`Options` 不加全局 sync 字段（避免"看起来能用"的第二个开关） |
| B：`sync=true` 默认 | 每条 durable | 最安全，但默认路径吞吐 ≈ 1/2.8ms ≈ 357 次提交/秒（§11.2 实测），M2 的 B 组压测与 M5 的基准都会被这个默认值污染 |
| C：混合——后台定时 fsync（`Options::sync_interval_us > 0`） | 有界丢失窗口 | 需要后台线程 + 关闭顺序（L11/L12）+ 与组提交的交互（定时器与 flusher 抢窗口）；**M2 不引入**，列入 M5 的可选项 |
| `sync=false` 时是否仍写 WAL | — | **必须写**（WAL 的本意）；否则 `sync=false` 就变成了"无持久化"，`DB::Sync()` 也无从谈起 |
| `DB::Sync()` 语义 | — | "把此前**所有**已返回 `kOk` 的写入刷到磁盘，返回即全部 durable"；实现为「入队一个不带数据的 barrier，等 `durable_seq_ >= 入队时的 last_sequence_`」 |
| `DB::Close()` 语义 | — | 拒绝新写 → 等在途批完成 → `Sync()` → 关闭文件句柄；**幂等**；析构函数等价于 `Close()`（I20） |

**推荐 A。** `【需拍板】` 点是默认值取 `false`（性能优先）还是 `true`（安全优先）。本文按 `false` 展开，但**所有验收门禁（100 轮 `missing 0`）显式使用 `sync = true`**。

### D6 恢复流程与尾部损坏策略 【需拍板】

| 子问题 | 方案 | 推荐 |
|---|---|---|
| 扫描范围 | A. 全量重放**所有** `*.log`（I12）/ B. 只重放最高编号 | **A**。M2 无 flush ⇒ WAL 是唯一真相源，老 log 永远不能被跳过。**这是 M2 与 M3 的分水岭**（M3 起变为"SSTable + 增量 WAL"） |
| 尾部损坏 | A. 截断到最后一条完整 record 并**记录非静默的恢复报告**（文件名 + 截断字节数 + 原因）/ B. 拒绝启动并要求人工修复 | **A**。判据：被截断的字节**不可能对应任何已 ack 的 `sync=true` 写**（§7 的论证）；`sync=false` 下被截断的写属于已声明的可丢失集合 |
| 中间损坏（非尾部） | A. `kCorruption` 拒绝启动 + 可定位信息（文件 + 偏移 + 期望/实际 CRC + resync 位置）/ B. 跳过损坏段继续 / C. 截断到损坏点之前 | **A**。B/C 会把"静默丢数据"引入到本该 fail-stop 的路径上 |
| `Options::repair` 开关 | A. **不引入**（M2 非目标不加字段；`m1-design.md` §1.4 明确禁止"看起来能用但没实现"的字段）/ B. 引入 `bool repair = false`，true 时对中间损坏也做截断 | **A**。若要人工修复，M2 提供的是"把损坏位置打印出来"而不是"自动猜" |
| 恢复是否重写/截断 WAL | A. 只在最高编号 log 的尾部做 `ftruncate` + `fsync`（这是 I18 允许的唯一写）/ B. 完全不写 | **A**，但必须满足 §5.3 的"尾部损坏只允许发生在最高编号 log"前提，且截断前后都给出日志 |
| 尾部损坏判定规则 | A. 只把"字节不足"（header 不完整 / payload 被截断）判为尾部，CRC 不符一律拒绝启动 / B. **"其后不存在任何完好 record" ⇒ 尾部**（需要一次 resync 扫描） | **B**。A 无法通过 M2 的验收（"CRC 不符"被明确列为三种尾部注入之一）。B 的诚实局限见 §5.3 末 |

**拍板点**：B 的 resync 扫描引入「连续损坏到文件尾会被判为尾部」（字节层面与"尾部损坏"不可区分，属信息论限制）。本设计以「恢复报告必须打印被截断的字节数，超阈值打 WARN」缓解，并把这条局限写进 §12。

### D7 sequence 与幂等 【需拍板】

| 子问题 | 方案 | 推荐 |
|---|---|---|
| record 是否携带 sequence | A. **batch 头携带 `sequence(8B LE)` + `count(4B LE)`**（§4.7）/ B. 不携带，恢复时按 1,2,3… 重新分配 | **A**。B 会让"恢复后新写的 sequence"与"崩溃前已 ack 的 sequence"重叠，且无法表达"一批的原子可见"。A 还让"重启后 sequence 继续单调"有字节级依据 |
| 恢复后 `last_sequence_` | A. `= max(replayed batch.sequence + count - 1)`（0 若无记录）/ B. `= 重放条目数` | **A**。B 在"同 key 高频覆盖 + 部分损坏"下会倒退 |
| **I13 措辞澄清** | I13 原文「恢复后 `last_sequence` **严格大于**任何已重放记录的 sequence」与 M1 口径（`DBImpl::Write` 用 `last_sequence_ + 1`，故 `last_sequence_` **等于**已用最高 seq）**互斥**。落地口径：恢复后 `last_sequence_ = max_replayed`，**下一次写入分配的 sequence = `max_replayed + 1` 严格大于任何已重放记录**。| **按后者落地，并请用户确认 I13 的措辞**（它决定 `#1` 的 I13 怎么写、测试怎么断言） |
| 重放顺序 | A. 按 `*.log` 编号升序 + 文件内字节顺序 / B. 按 sequence 排序后再重放 | **A**。恢复**不得**重排序：若 WAL 里出现 sequence 乱序，那是"写入串行化被破坏"的 bug，必须**报 `kCorruption` 暴露**而不是被排序掩盖（这正是不引入"重排后再重放"的理由） |
| 重复 record 的幂等 | A. 同 `(user_key, sequence, type)` 的 record 重放两次**不产生第二条 MemTable 条目**（M1 的跳表在 internal key 相等时按 value 段比较 ⇒ 同 key 同 value 会插入重复条目！）/ B. 接受重复条目 | **A 需要额外机制**：重放时检查 `batch.sequence <= last_sequence_` 则跳过（`last_sequence_` 在重放期单调推进）。这既实现幂等，也顺带拒绝"sequence 回退"的畸形 WAL。`【需拍板】`：这条需要在 `MemTable` 语义之外加一层判断，是否接受？备选是让重复重放产生重复条目（`Get` 结果仍正确，只浪费内存） |
| 同 key 多版本 | 依赖 M1 的 `InternalKeyComparator`（trailer 降序）+ `BuildLookupKey`，**不引入 M2 自己的版本逻辑** | **不做额外设计**；用 `Recovery.SameKeyManyVersionsReplaysLatest` 覆盖 |

### D8 崩溃对账的落地方式 【需拍板】

| 子问题 | 方案 | 推荐 |
|---|---|---|
| 已 ack 集合记录在哪 | A. **独立 sidecar 文件**（每行自校验 + `fsync`）/ B. 由外部脚本记录（写者进程打印到 stdout，被 kill 时缓冲区丢失）/ C. 写进 DB 自身 | **A**。B 在 kill -9 下必丢（stdout 有用户态缓冲）；C 会污染被测系统 |
| sidecar 何时写 | **A. `Put` 返回 `kOk` **之后**再写 sidecar 行**；顺序不可颠倒 | **A**。反过来会把"未完成的写"记为已 ack ⇒ **假 `missing`**（对账工具必须证明自己可靠，否则门禁不可信） |
| sidecar 是否逐行 fsync | A. 逐行 `fsync` / B. 不 fsync（依赖 kill -9 不丢 page cache） | **A 为默认**（协议在掉电场景也成立），但**如实记录**：§11.3 实测 kill -9 不丢 page cache，故 B 在 kill -9 门禁里同样能过。`【需拍板】`：逐行 fsync 会把 100 轮 × ~200 写变成 ~4 万次额外 fsync（≈2 分钟墙钟）；是否接受，或提供 `--ack-sync-every N` 开关 |
| 对账工具 | A. **复用 `DB::Get` 逐 key 校验**（走被测代码的读路径）/ B. 独立扫描 WAL 文件 | **A**。B 会把"恢复逻辑的 bug"与"对账逻辑的 bug"耦合在一起；A 用 M1 已经对账过的 `Get` 作为独立参照。**A 需要一个前提**：对账进程 `Open` DB 会截断尾部（§5.3），所以对账必须在**恢复进程退出后**单独起一个进程，或由恢复进程内部完成（推荐后者：`lsm_crash_test` 的恢复端就是 `lsm_crash_recover`，它 `Open` 后 `Get` 全部 sidecar key 并输出计数） |
| 输出与退出码 | 固定行格式（§8.3）：`ROUND <n> ACKED <a> RECOVERED <r> MISSING <m> TRUNCATED_BYTES <t> OPEN_MS <ms>`；`MISSING != 0` 或 `Open` 失败 ⇒ **退出码 1**；100 轮后打印 `TOTAL_ROUNDS`/`MISSING_TOTAL` | **照搬 raft-kv 的门禁风格** |
| `sync=false` 如何区分"允许丢的最近写"与"不允许丢的写" | A. **用 `WriteOptions::sync` 划水位线**：sidecar 行记录 `sync` 标志；门禁只对 `sync=true` 的行要求 `missing 0`，对 `sync=false` 的行要求"值必须是某个完整版本且 = sidecar 中该 key 某一前缀末端的值" / B. 全部要求存活 | **A**。`【需拍板】`：A 引入了"前缀末端"这条更强也更难写的判据，需确认是否按此实现 |

### D9 WAL 文件编号与生命周期 【需拍板】

| 方案 | 取舍 |
|---|---|
| **A（推荐）：编号 `%06u.log`；`Open` 时扫描目录取最大编号；紧接着用 `NewAppendableFile` 打开它（先截断尾部再追加）；**不新建文件、不删除任何文件** | 优点：① M2 无 flush ⇒ 老 log 永不可回收，复用同一文件使**文件数恒为 1**；② "截断尾部 → 继续追加"是天然顺序；③ 100 轮 kill -9 不会产生 100 个文件 |
| B：每次 `Open` 新建 `(max+1).log`，老文件保留 | 缺点：100 轮门禁产生 100 个文件；每轮恢复要重放全部历史（A 也要，因为数据都在 WAL 里），差别只在文件数与 dirent 开销；优点：M3 的"flush 后新建 log + 删除旧 log"路径天然就位 |
| C：`Open` 时把所有老 log 合并重写成一个新 log | **否决**：恢复路径写新数据，违反 I18（恢复只读）的意图，且把"恢复"变成"迁移"，崩溃在中途会同时损坏新旧两份 |

**推荐 A。** 并**现在就定义 WAL 删除的接口与判据（不实现）**：

```
// 判据：一个 log 文件可被删除 ⟺ 它包含的全部 record 所对应的数据都已存在于某个
//       更持久的、恢复路径必然会读到的结构中（M3 起 = SSTable + MANIFEST）。
// M2：不存在这样的结构 ⇒ 任何 log 文件都不可删除。
// 接口（M2 只声明 + 断言不实现）：Status DBImpl::MaybeDeleteObsoleteLogs();
//   M2 的实现体：LSM_ASSERT(obsolete.empty()) 并 return Status::OK();
```
`【需拍板】`：是否接受 M2 用"恒为 1 个 log 文件"的形态（与 M3 的行为差异较大，M3 要改成"每 Open 新建 + 删除被 flush 覆盖的 log"）。

### D10 进程级独占（`LOCK` 文件） 【需拍板，建议纳入】

- **方案 A：M2 就加**（`Env::LockFile/UnlockFile`，POSIX `fcntl(F_SETLK)` 写锁；进程崩溃时内核自动释放）。
- 方案 B：M2 不加，留到 M3。

**推荐 A**，理由：`lsm_crash_test.sh` 与对账工具在同一目录上跑；一旦出现"两个进程同时 `Open`"（脚本 bug / 人工误操作），两边会同时 `Append` 同一 WAL ⇒ record 交错 ⇒ 恢复时报中间损坏，**而这条损坏路径正是 M2 最难排查的**。成本 ≈ 40 行 Env 代码 + 1 个 `DB.OpenHoldsExclusiveLock` 用例。**这是本设计唯一的"超出指令明确要求"的建议**，故单独标出请用户裁决（不加也不影响任何验收判据）。

### D11 `fsync`/短写失败后 DB 的状态 【需拍板】

- **方案 A（推荐，fail-stop）：粘性 `commit_error_` + `bg_error_`，DB 进入"写只读"（后续 `Put`/`Delete` 立刻返回同一错误），`Get`/迭代器照常。**
- 方案 B：重试 N 次（退避）后仍失败才只读。
- 方案 C：只把错误返回给当前批，后续写继续。

**推荐 A。** 决定性理由：短写/`fsync` 失败后**文件偏移已不可信**（`Append` 内部重试后仍短写意味着部分字节已落盘，文件尾部处于"半条 record"状态）；此时继续追加会让**中间损坏**出现在文件中间，使下次恢复无法区分"中间损坏（必须拒绝启动）"与"尾部损坏（可安全截断）"。C 直接制造这个场景。B 的代价是延长"错误状态"的持续时间且不改变结论。**必须实测**：`WAL.FsyncFailurePropagates` + `GroupCommit.FailurePropagatesToAllWaiters` + `GroupCommit.WriteAfterErrorIsRejected`。

### D12 恢复期 MemTable 容量 【需拍板，但无替代】

- **方案 A（推荐）：恢复采用「两遍」——第一遍只扫描（得到 `last_sequence`、`total_record_bytes`、截断点），第二遍用 `write_buffer_size = max(options.write_buffer_size, total_record_bytes + kSlack)` 构造 MemTable 后重放。**
- 方案 B：直接用 `options.write_buffer_size`，触顶就 `kFrozen` ⇒ 恢复失败。
- 方案 C：触顶后重建一个更大的 MemTable 并整体重放（O(n²)）。

**推荐 A。** 这不是优化而是**正确性要求**：M2 无 flush ⇒ 全部数据必须装进一个 MemTable；若按 `options.write_buffer_size = 4 MiB` 直接重放一个 100 MiB 的 WAL，M1 的 `kFrozen` 会让恢复在 4 MiB 处失败——而 M2 的验收要求"恢复必须成功启动"。M3 引入 flush 后这条限制自然消失（恢复完立即 flush 成 SSTable）。代价：一次额外的 WAL 扫描（顺序读 + 无 CRC 校验的**快速估算**遍历；CRC 仍在校验，因为我们要在第二遍才真正解析）——见 §5.5 的口径说明。

### D13 自定义 comparator 与恢复 【登记为限制，不需拍板】

`Options::comparator` 在 M2 **不落盘**（`MANIFEST` 是 M3）。若用户用 `BytewiseComparator` 写入后用其它比较器 `Open`，重放结果的排序与去重语义会与写入时不同。M2 的处理：**登记为已知限制**，写进 `m2-prerequisites.md` 的"未定义行为清单"，并在 `DB::Open` 的注释里写明契约（"比较器由调用方保证跨重启一致；M3 起由 MANIFEST 校验 `Comparator::Name()`"）。不引入 name 落盘（那是 M3 的 `MANIFEST`）。

---

## 3. WAL 文件命名、编号与生命周期

### 3.1 命名规则

```
<dbname>/                   目录（Open 时若不存在则创建，并 fsync 父目录；见 §5.7）
<dbname>/000001.log         预写日志，编号 6 位十进制零填充，编号即文件在恢复顺序中的位置
<dbname>/LOCK               进程级独占锁（仅当 D10 采纳；内容为空，语义靠 fcntl 锁）
```

- 编号从 **1** 开始（0 保留给"无文件"），`%06u` 格式化，**严格递增**。
- 名字匹配用正则 `^[0-9]{6}\.log$`（`Env::GetChildren` 返回的名字直接匹配，不做路径拼接）。
- M2 **只有** `.log`。`.sst`/`.ldb`/`MANIFEST`/`CURRENT`/`.dbtmp` 全部留给 M3（§1.2）。

### 3.2 编号分配（`Open` 时）

```
children = Env::GetChildren(dir)
nums = { parse(n) : n in children, n 匹配 ^[0-9]{6}\.log$ }
log_number = nums.empty() ? 1 : *max_element(nums)
```

`log_number` 之后**不再变化**（M2 不轮转，D9）。

### 3.3 生命周期与"可删除"判据（M2 只定义，不实现）

| 事件 | M2 行为 | M3+ 行为（登记，不实现） |
|---|---|---|
| `Open` | 最高编号 log 存在 → `NewAppendableFile`；不存在 → `NewWritableFile` 创建 + `SyncDir` | 新建 `(max+1).log`；老 log 在 flush 覆盖后删除 |
| 写 | 追加到该 log | 追加到当前 log |
| `Close` | `Sync()` + `Close()`，**不删除** | 同 |
| 删除 | **永不删除**（无 SSTable ⇒ 删了数据就没了） | log 中全部 record 的 (seq,key) 都被某个 SSTable 覆盖且该 SSTable 已被 MANIFEST 引用 ⇒ 可删 |

---

## 4. WAL record 位级格式

> 本节是 `docs/protocol.md` 的**追加章节**（§4.6 给出可直接落地的 patch 文本）。所有多字节整数**小端（LE）**，
> 逐字节拼装/解析，**禁止 `reinterpret_cast` 到整型指针**（`protocol.md` §1 的硬约束，M1 未定义行为清单第 1 条）。

### 4.1 物理块与 record 头

WAL 文件被划分为**固定 32 KiB（32768 字节）的物理块**；块内可含 0 条或多条 record 片段（fragment）。
一条逻辑 record 跨越一个或多个物理块，每个块内的片段带一个 **7 字节头**：

```
record := header(7B) || payload(length B)
header := crc32c(4B, LE) || length(2B, LE) || type(1B)
```

| 字段 | 字节数 | 字节序 | 取值 | 说明 |
|---|---|---|---|---|
| `crc32c` | 4 | LE | `uint32` | CRC32C（`protocol.md` §5，`crc32c(0, data)` 口径） |
| `length` | 2 | LE | `1 .. 32761`（= 32768 − 7） | **本片段** payload 的字节数；`0` 非法 |
| `type` | 1 | — | `1..4` | `1=kFullType`、`2=kFirstType`、`3=kMiddleType`、`4=kLastType`；`0` 只作 padding 哨兵 |
| `payload` | `length` | — | 原始字节 | 逻辑 record 的一段；payload 本身是 §4.7 的 batch 编码 |

**常量（冻结）**

```cpp
constexpr size_t kWALBlockSize   = 32768;   // 32 KiB
constexpr size_t kWALHeaderSize  = 7;
constexpr size_t kWALMaxPayload  = kWALBlockSize - kWALHeaderSize;   // 32761
enum RecordType : uint8_t {
  kZeroType   = 0x00,   // 仅 padding（crc==0 && length==0 && type==0）
  kFullType   = 0x01,   // 完整 record 在单一片段内
  kFirstType  = 0x02,   // 跨块 record 的第一段
  kMiddleType = 0x03,   // 跨块 record 的中间段
  kLastType   = 0x04,   // 跨块 record 的最后一段
};
```

**为什么 32 KiB**：① 每条 fragment 都要带 7 字节头，块越小碎片越多（4 KiB 块下一条 64 KiB value 要 16 个头 = 112 字节开销 + 16 次 CRC 分段）；② 32 KiB 与 ext4 的 `delayed allocation` 与块设备的合并窗口同量级，预分配/O_DIRECT 对齐友好；③ 与 M3 的 SSTable block 在"CRC + length 前缀"的心智模型上同构（但 SSTable 的块尾是 restart array，**不是**同一格式，M3 不得直接复用本节的类型字节）。

### 4.2 类型状态机（writer 侧唯一真相）

```
// 写一条逻辑 record（payload 为 §4.7 的 batch 字节）
WriteRecord(payload):
  begin = true
  while true:
    if (kWALBlockSize - block_offset_) <= kWALHeaderSize:      # 修订（#1 回退 #0）：原来是 "<"，
        # 剩余空间 <= 7 时也必须补 padding：若只剩 7 字节，头之后 avail == 0，
        # 会写出 length == 0 的非法片段 —— 而本设计的 reader 明文规定 len == 0 ⇒ PARSE_FAIL，
        # 即 writer 能写出自己读不回来的文件（LevelDB 允许空片段，本设计**不允许**）。
        # 块尾放不下一个头（或放得下头但放不下 payload）⇒ 补零 padding 到块尾
        if (kWALBlockSize - block_offset_) > 0: emit zeros(kWALBlockSize - block_offset_)
        block_offset_ = 0
    avail   = kWALBlockSize - block_offset_ - kWALHeaderSize
    frag    = min(payload.size(), avail)
    end     = (payload.size() == frag)          # 注意：余量正好放完也是 end
    type    = begin ? (end ? kFullType : kFirstType)
                    : (end ? kLastType : kMiddleType)
    crc     = crc32c(LE16(frag) || type_byte || payload[0..frag))
    emit   (crc LE32) (frag LE16) (type) payload[0..frag)
    block_offset_ += kWALHeaderSize + frag
    payload.remove_prefix(frag)
    begin = false
    if end: break
```

- **一条逻辑 record 的每个片段都必须有 `length >= 1`**（`payload.size() > 0`；`avail >= 1` 由上面的 padding 前置条件保证）。因此 §4.7 的 batch payload 必须非空（`count >= 1` 且至少一条 entry）。
- `end` 的判定是 `payload.size() == frag`（**不是** `payload.empty()`）：这保证"余量正好放完"时给出 `kFullType`/`kLastType`，而不是产生一个 `length = 0` 的尾片段。这是 LevelDB `AddRecord` 的关键细节（其注释：`if (leftover < kHeaderSize) ...` 与 `if (end)` 的组合），漏掉会写出 `length = 0` 的非法片段。

### 4.3 reader 侧状态机与重组

```
ReadAllRecords(file) -> (records[], last_good_end, verdict):
  offset = 0
  last_good_end = 0
  in_frag = false; frag_buf.clear()
  loop:
    block_off = offset % kWALBlockSize
    block_end = (offset / kWALBlockSize + 1) * kWALBlockSize
    # (a) 块尾不足一个头 ⇒ 只能是 padding
    if block_off + kWALHeaderSize > kWALBlockSize:
        if file_size < block_end: return TAIL_RESIDUE(offset)      # padding 被截断
        offset = block_end; continue
    # (b) 头不完整
    if file_size - offset < kWALHeaderSize: return TAIL_RESIDUE(offset)
    (crc, len, type) = decode(file[offset .. offset+7))
    # (c) padding 哨兵：真实 record 的 length>=1 且 type in 1..4，与哨兵不相交
    if crc == 0 && len == 0 && type == 0:
        if file_size < block_end: return TAIL_RESIDUE(offset)
        offset = block_end; continue
    # (d) 头字段合法性（先于 CRC，防越界读）
    if type not in {1,2,3,4}: return PARSE_FAIL(offset)
    if len == 0 || len > kWALMaxPayload: return PARSE_FAIL(offset)
    # (e) payload 不全
    if file_size - offset - kWALHeaderSize < len: return TAIL_RESIDUE(offset)
    # (f) CRC（§4.4 的覆盖面）
    if crc32c(file[offset+4 .. offset+7+len)) != crc: return PARSE_FAIL(offset)
    # (g) 类型状态机
    switch type:
      kFullType:   require(!in_frag); emit_record(file[offset+7 .. +len))
      kFirstType:  require(!in_frag); in_frag = true; frag_buf = payload
      kMiddleType: require(in_frag);  frag_buf += payload
      kLastType:   require(in_frag);  frag_buf += payload; emit_record(frag_buf); in_frag = false
    offset += kWALHeaderSize + len
    last_good_end = offset
```

- **`require(...)` 违反 ⇒ `PARSE_FAIL`**（`kCorruption`）：`kFirstType` 出现在 `kFullType` 之后、`kLastType` 缺 `kFirstType`、文件在 `in_frag == true` 时结束等。这些是"类型状态机不自洽"，与"字节不足"不同——后者是 `TAIL_RESIDUE`。
- 重组缓冲 `frag_buf` 的内存开销 = 最大逻辑 record 大小 ≤ `kMaxGroupBytes + 最大单条 value 开销`。M2 的 `kMaxGroupBytes = 1 MiB`，但**单条 record 可以更大**（一个写者的 1 MiB value + 15 字节开销 ≈ 1 MiB + 15）。为防"恶意/畸形 length 组合撑爆内存"，reader 在重组前对 `frag_buf.size() + len` 设上限 `kMaxLogicalRecordSize = 64 MiB`，超限 ⇒ `PARSE_FAIL`（可定位）。这个上限比 `kWALMaxPayload × 2` 大得多，不会误伤正常路径（M1 允许 1 MiB value，M2 的 batch 上限 1 MiB）。
- `TAIL_RESIDUE(p)` 与 `PARSE_FAIL(p)` 的**后续处理不同**：前者直接截断（§5.3 规则 R1），后者要进入 §5.3 的 resync 判定。

### 4.4 CRC 覆盖面（对应 D2）

```
crc_input := length(2B LE) || type(1B) || payload(length B)      // 即 header 去掉 crc 字段后的全部字节 + payload
crc       := crc32c(crc_input)
```

- **覆盖范围 = 从 `length` 字段的第一个字节到 record 的最后一个字节**。等价说法：**CRC 覆盖 CRC 字段之后的全部字节**。
- 与 LevelDB 的差异：LevelDB 是 `crc32c(type || payload)`（不含 `length`）。本设计**有意**多覆盖 2 字节（D2 方案 A）：
  - 零额外存储、零额外 syscall；
  - 让"`length` 字段的完整性"成为 CRC 契约的一部分，而不是依赖"用错 length 取到错 payload ⇒ CRC 碰巧失败"这一非契约性推断；
  - reader 仍**必须**先做 (d) 的长度合法性检查再算 CRC——顺序不能反（否则畸形 length 导致越界读，正是 `m1-review.md` 阻断项 2 的同源风险）。
- 流式（跨块）CRC 累加：`c = crc32c::Value(prefix3, 3)`，随后每段 `c = crc32c::Extend(c, frag, n)`（`protocol.md` §5 的 `Extend` 语义：`Extend(Value(a), b, m) == Value(a ++ b)`）。
- `prefix3` 是 3 字节缓冲 `{length_lo, length_hi, type}`（按磁盘顺序），不是 `{type, length_lo, length_hi}`。**这个顺序是契约的一部分**，测试用 `ManualRecordCRC`（测试内手工拼字节）交叉核对，避免"写读两侧同时错"的假通过——沿用 M1 `InternalKey.BuildParseRoundTrip` 靠 `ManualInternalKey` 交叉核对的做法。

### 4.5 大 record 的流式写入（内存上界）

- **writer 侧**：`AddRecord` 接受一个 `Slice`，内部按 §4.2 分片，**不复制整条 record**。
- **组提交的批**（§6）在内存里是一块连续缓冲（`group_buf_`，上限 1 MiB + 单个超大写者）。因此 M2 的 **单次 WAL 写入 ≤ `kMaxGroupBytes + 最大单条写者 payload`**；1 MiB value 的写者独占一批，WAL 侧内存 = 1 MiB + 15 字节。
- **不要把"大 value 需要整体缓冲"当成 M2 的问题**：M1 的 `MemTable::Add` 本来就要把 value 拷进 Arena，WAL 侧多一份 ≤1 MiB 的临时缓冲，相对成本可忽略（§11.2 实测 `append-nosync` 中位 1 µs）。

### 4.6 与 `docs/protocol.md` 的衔接（**patch 文本，本阶段不落地**）

> 为什么需要 patch：`protocol.md` 的定位是"位级编码契约：M1 定稿后冻结，M2（WAL record）、M3（SSTable block/index/footer）**必须逐字复用**"，
> 并明确"任何变更须回到 `#0` 设计阶段修订本文件并说明影响面"。WAL record 格式属于该文件必须承载的内容。
> **本阶段只给出 patch 文本，`docs/protocol.md` 在 #0 门后（M2.1）才落地。**
> **patch 只追加，不改 §1~§8 任何一行**（§5 CRC32C 那句"M2 起用于 WAL record 校验"原文已经写好，无需改动）。

把下面整块**追加**到 `docs/protocol.md` 末尾（现末尾是 §8 的最后一行）：

````markdown
## 9. WAL record 编码（M2 定稿）

> 追加章节。§1~§8 为 M1 冻结内容，本节不得反向修改它们。
> 本节的所有多字节整数一律**小端**，逐字节拼装/解析，禁止 `reinterpret_cast`（§1）。

### 9.1 物理块与 record 头

WAL 文件划分为固定 **32 KiB（32768 B）** 的物理块；一条逻辑 record 可跨多个块，
每个块内片段带 7 字节头：

```
record := header(7B) || payload(length B)
header := crc32c(4B, LE) || length(2B, LE) || type(1B)
```

| 字段 | 字节数 | 字节序 | 取值 |
|---|---|---|---|
| `crc32c` | 4 | LE | CRC32C（§5），覆盖面见 §9.3 |
| `length` | 2 | LE | `1 .. 32761`（= 32768 − 7）；`0` 非法 |
| `type` | 1 | — | `0x00` padding 哨兵；`0x01` kFullType；`0x02` kFirstType；`0x03` kMiddleType；`0x04` kLastType |
| `payload` | `length` | — | 本片段，内容为 §9.4 的 batch 编码 |

常量：`kWALBlockSize = 32768`、`kWALHeaderSize = 7`、`kWALMaxPayload = 32761`。

### 9.2 跨块切分与 padding

- 若块内剩余不足 7 字节，写者用 `0x00` 补齐到块边界，再在新块起始写下一个片段。
- `type` 状态机：单块 record = `kFullType`；跨块 = `kFirstType` / `kMiddleType`* / `kLastType`。
- 每个片段的 `length >= 1`（写者在分片时保证 `end` 判据为 `payload.size() == frag`）。
- reader：块内剩余 < 7 字节 ⇒ 跳过 padding 到块边界；7 字节头全零（`crc==0 && length==0 && type==0`）
  ⇒ 视为 padding 跳到块边界。真实 record 的 `length >= 1` 且 `type ∈ {1..4}`，与哨兵**不相交**，故二者在字节形态上可判定区分。
- reader 对 `type ∉ {1..4}` 或 `length ∉ [1, 32761]` 一律判为损坏，**不得**用该 `length` 推进偏移（防越界读）。
- 跨块重组缓冲设上限 `kMaxLogicalRecordSize = 64 MiB`，超限判损坏（可定位）。

### 9.3 CRC 覆盖面

```
crc_input := length(2B LE) || type(1B) || payload(length B)     # 即 crc 字段之后的全部字节
crc       := crc32c(crc_input)                                  # §5 的 crc32c(0, data) 口径
```

- **与 LevelDB 的有意差异**：LevelDB 只覆盖 `type || payload`，本节额外覆盖 `length` 的 2 字节。
  理由：让 `length` 的完整性成为 CRC 契约的一部分，而不是依赖"用错长度取到错 payload ⇒ CRC 碰巧失败"的间接推断。
- 跨块流式累加：`c = Value(prefix3)`，随后逐段 `c = Extend(c, frag, n)`（§5 的 `Extend` 语义）。
  `prefix3` 的字节序为 `{length_lo, length_hi, type}`（磁盘顺序），是契约的一部分。

### 9.4 WAL batch payload 编码

一条逻辑 record 的 payload 是**一个 batch**（M2 的一次写 = 一个 batch；组提交把整批合并为一条 record）：

```
payload := sequence(8B, LE) || count(4B, LE) || entry[0..count)
entry   := type(1B) || key_len(varint32) || key || [ value_len(varint32) || value ]
```

| 字段 | 编码 | 约束 |
|---|---|---|
| `sequence` | 8B LE | batch 中第一条 entry 的 sequence；`sequence + count - 1 <= kMaxSequenceNumber` |
| `count` | 4B LE | `1 .. kMaxBatchCount`（`kMaxBatchCount = 1 << 20`，防畸形撑爆） |
| `type` | 1B | `0x0 = kTypeDeletion`（无 value 字段）、`0x1 = kTypeValue` |
| `key_len` / `key` | varint32 + 字节（§4） | `1 .. kMaxUserKeySize` |
| `value_len` / `value` | varint32 + 字节（§4） | 仅 `kTypeValue`；空 value 合法 |

- 第 `i` 条 entry 的 sequence = `sequence + i`。
- 解析必须**恰好消费完** payload：`count` 条 entry 解完后仍有剩余字节 ⇒ 损坏。
- 本编码与 M5 的 `WriteBatch` 落盘布局同构（M2 只作为 WAL 内部组织，**不提供公共 API**）。
````

### 4.7 WAL batch payload 编码（正文摘要）

完整定义见上面的 §4.6 patch §9.4。设计要点：

1. **`sequence + count` 是"I13 恢复 sequence"的字节级依据**：恢复只需取 `max(sequence + count - 1)`，不需要数条目。
2. **一条 record = 一个原子批**（D3）：组提交把 N 个写者的单条目 batch **合并**成一条 `count = N` 的 record，`sequence` = 队首写者的序号，且因为序号在入队时按 `commit_mu_` 内的顺序连续分配，`sequence + i` 恰好等于第 i 个写者的序号（§6.3 的"连续分配"不变式）。
3. **`count` 上限**与 `kMaxGroupRecs = 64` 一致地为 1 MiB 级，防御畸形 WAL 撑爆内存：解析器在 `count > kMaxBatchCount` 时直接判损坏。
4. 复用 `protocol.md` §2 的 `GetVarint32`（失败时不修改输出）与 §4 的 length-prefix 口径，**不引入 M2 自己的新原语**。

---

## 5. `DB::Open` 的恢复流程

### 5.1 前置与目录处理

```
DB::Open(options, name, dbptr):
  *dbptr = nullptr                                   // I8：失败不产生半构造对象
  if options.comparator == nullptr || options.write_buffer_size == 0: return InvalidArgument
  if name.empty():                                    // M1 内存模式，逐字保留
      return DB::OpenMem(options, dbptr)
  // ---- 持久化模式 ----
  if !Env::FileExists(name):
      s = Env::CreateDir(name);  if !s.ok(): return s
      Env::SyncDir(parent_of(name))                   // 新建目录的 dirent 必须 durable
  // D10 采纳时：fd_lock = Env::LockFile(name + "/LOCK")，失败 → kIOError("already held")
  log_number = ScanMaxLogNumber(name)                 // §3.2
  return DBImpl::RecoverAndOpen(options, name, log_number, dbptr)
```

### 5.2 恢复算法（伪代码）

```
DBImpl::RecoverAndOpen(options, name, log_number, dbptr):
  # ---- 第一遍：只扫描，不建 MemTable ----
  plan = {}
  for n in sorted_ascending(all *.log in name):        # I12：全量重放
      r = WALReader::Scan(name/n)                     # §4.3 的 reader
      case r.verdict:
        CLEAN:        plan.append(n, records=r.records)
        TAIL_RESIDUE:
            if n != log_number: return Corruption("非最高编号 log 的尾部残骸", name/n, r.offset)
            plan.append(n, records=r.records, truncate_at=r.last_good_end)
        PARSE_FAIL:
            v = ResyncScan(name/n, r.offset)           # §5.3
            if v.found_valid_record_at >= 0:
                return Corruption("中间损坏", name/n, offset=r.offset,
                                  expected_crc=..., actual_crc=..., resync_at=v.found_valid_record_at)
            if n != log_number:
                return Corruption("非最高编号 log 的不可判定损坏", name/n, r.offset)
            plan.append(n, records=r.records, truncate_at=r.last_good_end,
                        note="对不可判定损坏按尾部处理，丢弃 N 字节")
  # ---- 截断（I18 允许的唯一写）----
  if plan.truncate_at is set:
      Log(WARN, "WAL %s: truncating %llu bytes at offset %llu (reason=%s)")
      s = Env::TruncateFile(hi_log_path, plan.truncate_at); if !s.ok(): return s
      s = Env::ReopenAndSync(hi_log_path);                if !s.ok(): return s
  # ---- 第二遍：按 D12 决定容量后重放 ----
  cap = max(options.write_buffer_size, plan.total_payload_bytes + kRecoverySlack)
  mem_ = new MemTable(InternalKeyComparator(options.comparator), cap)
  last_sequence_ = 0
  for (n, records) in plan:                             # 编号升序，文件内字节顺序
      for rec in records:                               # 顺序重放，**不重排序**
          b = ParseBatch(rec.payload)                   # §4.7；失败 → Corruption
          if b.sequence <= last_sequence_: continue     # D7 幂等 + 拒绝 sequence 回退
          for i in 0..b.count:
              s = mem_->Add(b.sequence + i, b.entries[i].type, b.entries[i].key, b.entries[i].value)
              if !s.ok(): return Corruption("重放 Add 失败", n, rec_offset, s.ToString())
          last_sequence_ = b.sequence + b.count - 1
  # ---- 打开 log 继续追加 ----
  if Env::FileExists(hi_log_path):
      s = Env::NewAppendableFile(hi_log_path, &logfile_)
  else:
      s = Env::NewWritableFile(hi_log_path, &logfile_); Env::SyncDir(name)
  if !s.ok(): return s
  log_number_ = log_number
  *dbptr = this
  return OK
```

### 5.3 尾部 vs 中间损坏的判定（**M2 的核心判定规则**）

在 reader 报告 `PARSE_FAIL(p)` 时，**先做一次 resync 扫描**，再决定是截断还是拒绝启动：

```
ResyncScan(path, p) -> first_offset_with_valid_record, 或 -1:
  # 只在首次解析失败时执行一次（正常恢复不付这个成本）
  for q in (p+1) .. (file_size - kWALHeaderSize):
      (crc, len, type) = header_at(q)
      if type not in {1..4}: continue
      if len == 0 || len > kWALMaxPayload: continue
      if q + 7 + len > file_size: continue
      if q % kWALBlockSize + 7 + len > kWALBlockSize: continue   # 不得跨块边界
      if crc32c(file[q+4 .. q+7+len)) == crc: return q          # 找到完好 record
  return -1
```

| 情形 | 判定 | 动作 |
|---|---|---|
| reader 返回 `TAIL_RESIDUE`（字节不足：header 不完整 / payload 被截断 / padding 被截断） | **尾部残骸** | 截断到 `last_good_end`（仅限最高编号 log） |
| reader 返回 `PARSE_FAIL` 且 `ResyncScan == -1`（其后不存在任何完好 record） | **尾部残骸（不可判定型）** | 截断到 `last_good_end`，**日志必须打印丢弃字节数与 `kMaxTailCorruptWarnBytes` 阈值告警** |
| reader 返回 `PARSE_FAIL` 且 `ResyncScan >= 0` | **中间损坏** | `Status::Corruption`（文件名 + 偏移 `p` + `resync` 位置 + 期望/实际 CRC），**拒绝启动**，`*dbptr = nullptr`，不截断任何字节 |
| 上述任一情形发生在**非最高编号** log | 一律 **`kCorruption`，拒绝启动** | 理由：更高编号的 log 里还有数据，说明这不是"最后一段没写完"，而"更早的文件被损坏"无法安全自治 |

**为什么"其后不存在完好 record ⇒ 尾部"是对的定义**：写者严格顺序追加，因此"崩溃时未写完的部分"必然是文件的**后缀**。反过来，如果损坏点之后仍存在结构完好、CRC 正确的 record，那损坏点只可能是被"写穿"的（顺序写不可能跳过中间一段），即中间损坏。

**诚实登记的局限（必须写进 `m2-prerequisites.md`）**：
若损坏恰好连续延伸到文件末尾（例如磁盘上最后 N 个扇区同时坏掉），字节层面与"尾部残骸"**不可区分**。这是信息论层面的限制，任何实现都无法消除。缓解：
1. 恢复报告**必须**输出 `TRUNCATED_BYTES`，且当 `TRUNCATED_BYTES > kMaxTailCorruptWarnBytes`（取 `2 * kWALBlockSize`）时打 `WARN` 并提示人工检查；
2. `sync = true` 的已 ack 数据受 §7.1 的论证保护（fsync 成功的字节不可能被截断，除非介质本身损坏），因此该局限只会影响 `sync = false` 或介质损坏场景。
3. 门禁脚本在输出里带上 `TRUNCATED_BYTES`，一旦某轮出现非零值即可被肉眼/CI 发现（M2 的 100 轮门禁预期它是 0，因为 §11.3 实测 kill -9 不产生 torn record）。

### 5.4 sequence 跨重启的单调性（I1 在崩溃后仍成立）

**不变式**：设进程 P1 崩溃，P2 从同一目录 `Open`。
1. P2 恢复后的 `last_sequence_ = max_replayed`（D7）。
2. P1 在崩溃前 **ack 过**（`Put` 返回 `kOk` 且 `WriteOptions::sync == true`）的每一条写，其 record 的字节在崩溃时已 durable（§7.1）⇒ 必然被 P2 重放 ⇒ 其 sequence ≤ `max_replayed`。
3. P1 在崩溃前 **未 ack** 的写，其 sequence 可能 > `max_replayed`，也可能 ≤；但该写：
   - 若曾进入 MemTable（P1 的内存态），随进程消失，且没有任何对外承诺（未 ack）；
   - 若曾进入 WAL 但未 durable，P2 不会重放它（其字节不存在或被截断）；
   - **不会与 P2 的新写冲突**：P2 从 `max_replayed + 1` 起分配，若某个未被重放的旧写用了 seq ∈ (`max_replayed`, `max_replayed + 1`] 的区间，那个旧写在 P2 的世界里**不存在**（不在 MemTable、不在 WAL），没有任何结构引用它。
4. 因此 P2 的 sequence 序列 `{max_replayed+1, max_replayed+2, …}` 与 P1 已 ack 的全部 sequence 集合**不相交且严格更大** ⇒ I1（sequence 单调不减、internal key 唯一）在崩溃后仍成立。

**为什么"截断"不会破坏这个论证**：截断点 = 最后一条**完整** record 的末尾（§5.3），而 §7.1 证明任何已 ack（`sync=true`）的 record 都是完整的、且在其后的字节被截断之前就已 durable。所以被截断的字节要么属于未 ack 的写，要么属于 `sync=false` 的写。

**恢复不得重排序**：见 D7。若 WAL 里出现 sequence 乱序（`b.sequence <= last_sequence_` 而 record 又不同），M2 的选择是**跳过**（幂等）而不是报错——因为"同一条 record 被扫描两次"在 M2 的结构下不会发生（每个文件的每个偏移只扫一次），所以 `b.sequence <= last_sequence_` 只可能来自**真正的乱序**。`【需拍板】`：§2 D7 已登记该口径（跳过 vs 报错），本设计按"跳过 + 计数上报"落地，并把计数放进恢复报告。

### 5.5 恢复只读性、幂等性与耗时口径

- **只读**（I18）：恢复路径**不写 WAL**（不追加任何新 record）、**不触发 flush**（M2 无 flush）、**不修改任何既有文件**，唯一例外是 §5.2 的尾部 `ftruncate`（显式、有日志、有判据）。
- **幂等**：第二次 `Open` 时，第一次已把尾部截断到完整 record 边界 ⇒ reader 返回 `CLEAN` ⇒ 不写入任何字节 ⇒ 文件内容逐字节不变。用例 `Recovery.IdempotentAcrossRepeatedOpens` 断言：连续 `Open`/`Close` 3 次后，WAL 文件的 `sha256` 与第一次恢复后一致，且 `Get` 结果一致。
- **耗时口径**：M2 的恢复是 O(全部 WAL 字节)（I12 的必然结果），必须**实测并记录基线**（`ROUND ... OPEN_MS <ms>` 与 `scripts/lsm_recovery_stats.sh`），供 M3 对照（M3 起只需重放 SSTable 之后的增量 WAL）。**M2 接受这个线性成本**，但不接受"未记录"。
- **容量口径的诚实说明（D12）**：两遍扫描使 `Open` 的最坏耗时 ≈ 2× WAL 字节的读取成本。第一遍**不解析 batch、不算 CRC**（只按 header 前进；遇 `PARSE_FAIL` 才做 resync 扫描）——**等等，这与"第一遍必须发现损坏"矛盾**。修正：第一遍**必须**做完整校验（否则第二遍才知道坏在哪，会让 `last_good_end` 与容量估算不一致）。因此口径是：**两遍都完整校验**，`Open` 成本 ≈ 2× 单遍扫描。若后续实测认为不可接受，M2.3 可优化为"第一遍按文件大小上界估容量，第二遍完整校验 + 按需增长"，但那会破坏"恢复一定能成功"的保证（D12 的 A/B 对比），故**先按两遍落地并记录耗时**。

### 5.6 恢复后的写路径

```
DBImpl::Put(wo, key, value) -> Write(wo, kTypeValue, key, value)
DBImpl::Delete(wo, key)     -> Write(wo, kTypeDeletion, key, Slice())
```
`Write` 在 M2.2 与 M2.3 有**两种实现形态**，但对外语义、返回值与不变量完全相同（§6.2 / §6.3）：

| 阶段 | 形态 | 不变量 |
|---|---|---|
| M2.2 | 单把 `log_mu_` 串行化整个「分配 seq → MemTable::Add → WAL Append(+Sync)」；`log_mu_` 锁序为 `log_mu_ → mutex_` | I11、I17 已成立（IO 在 `mutex_` 之外）；吞吐 = 每写一次 fsync |
| M2.3 | §6.3 的组提交队列（`commit_mu_` + `pending_` + flusher 选举） | 同上 + I15/I16 + L9/L10 |

**M2.2 的 `log_mu_` 必须覆盖 sequence 分配**，否则两个写者可能"先拿 seq 的后写"，使 WAL 中的 record 顺序 ≠ sequence 顺序（违反 I13 的前提）。这是 M2.2 唯一容易写错的地方，写进 `m2-prerequisites.md`。

### 5.7 `Env` 的增补（只增不改）

| 增补 | 签名 | 用途 |
|---|---|---|
| 追加打开 | `Status NewAppendableFile(const std::string& fname, WritableFile** result)` | 复用最高编号 log（D9） |
| 目录枚举 | `Status GetChildren(const std::string& dir, std::vector<std::string>* result)` | 扫描 `*.log`（§3.2） |
| 截断 | `Status TruncateFile(const std::string& fname, uint64_t size)` | §5.3 尾部截断（I18 的唯一写） |
| 目录 fsync | `Status SyncDir(const std::string& dir)` | 新建 db 目录 / 新建首个 log 后的 dirent durable |
| 文件锁（D10） | `Status LockFile(const std::string& fname, FileLock** lock)` / `class FileLock` | 进程级独占 |

**不改** M1 已有的 8 个 `WritableFile`/`SequentialFile`/`Env` 方法语义。`RandomAccessFile` 仍然留到 M3（§1.2）。

---

## 6. 两段式持久化 / 锁内零 IO / 组提交

### 6.1 锁与状态

```
// ── DB 锁（保护内存状态；**绝不**在其临界区内做任何文件 IO）──  L7
std::mutex mutex_;
MemTable* mem_;                     // 唯一 MemTable（§1.2 边界 2）
SequenceNumber last_sequence_;
bool shutting_down_;
Status bg_error_;                   // 粘性：日志层失败后 DB 写只读

// ── 提交队列锁（叶子锁；锁序 commit_mu_ → mutex_，禁止反向）──  L8
std::mutex commit_mu_;
std::condition_variable cv_commit_;
struct Writer {                     // 存活于调用方栈帧（等待期间一直有效）
  uint64_t         end_seq;         // 本写者的最后一条 sequence
  bool             sync;            // 本写者是否要求 durable-before-ack
  bool             done;            // 已由 flusher 结算
  Status           status;          // flusher 写入的结算结果
  const std::string* encoded;       // 指向调用方栈上的 batch payload（入队时不做拷贝）
  std::condition_variable cv;
};
std::deque<Writer*> queue_;         // FIFO；queue_.front() 天然是"下一个 flusher 候选人"
bool     flusher_active_;           // 单 flusher 标志
uint64_t written_seq_;              // 已进入 WAL 文件（write() 返回）的最高 sequence
uint64_t durable_seq_;              // 已 fsync 的最高 sequence
Status   commit_error_;             // 粘性；首个错误
std::string group_buf_;             // flusher 私有组缓冲（不做拷贝则为只读借用）
```

**为什么必须是两把锁**（指令 §1.4 的硬要求，也确有收益）：① `Get`/`NewIterator` 只碰 `mutex_`，永不与提交队列争用；② `pending_` 的搬运与组缓冲构建发生在 `commit_mu_` 下，与内存状态无耦合；③ 锁序单向（`commit_mu_ → mutex_`）使"入队前先分配 sequence"这一步天然原子，无需回滚路径。

### 6.2 M2.2 的过渡形态（最小正确实现）

```
Status DBImpl::Write(const WriteOptions& wo, ValueType type, const Slice& key, const Slice& value):
  s = ValidateKey(key); if !s.ok(): return s                      // 锁外
  std::string encoded;
  std::unique_lock<std::mutex> ll(log_mu_);                       // 串行化"seq 分配 + WAL 追加"
  {
    std::lock_guard<std::mutex> dl(mutex_);                       // 只写内存（L7）
    if (shutting_down_) return Status::IOError("Write: DB is closing");
    if (!bg_error_.ok()) return bg_error_;
    seq = last_sequence_ + 1;
    s = mem_->Add(seq, type, key, value);                         // 冻结 → 不消耗 seq
    if (!s.ok()) return s;
    last_sequence_ = seq;
    EncodeBatchPayload(&encoded, seq, 1, type, key, value);        // §4.7
  }                                                               // ← 释放 mutex_，下面才是 IO
  s = logfile_->Append(Slice(encoded));                           // 锁外 IO
  if (s.ok() && wo.sync) s = logfile_->Sync();                    // durable-before-ack
  if (!s.ok()) { std::lock_guard<std::mutex> dl(mutex_); bg_error_ = s; }
  return s;
```

- **先 `mem_->Add` 再 `Append`** 是**有意**的顺序：反过来的话，`Append` 成功而 `Add` 失败（如 `kFrozen`）会在 WAL 里留下一条"被拒绝的写"，恢复时被重放 ⇒ **被拒绝的写复活**。代价是失败路径上"该值已对同进程读者可见但重启后不恢复"——写进 §7 的语义表（D11 的 fail-stop 使这个不一致窗口在错误返回后立即冻结）。
- `log_mu_` 覆盖 IO，但它**不是** `mutex_`，故满足 I17。

### 6.3 M2.3 的组提交（flusher 选举 + 谓词 + 窗口放开）

```
Status DBImpl::Write(const WriteOptions& wo, ValueType type, const Slice& key, const Slice& value):
  s = ValidateKey(key); if !s.ok(): return s
  std::string encoded;
  Writer w; w.sync = wo.sync; w.done = false;

  std::unique_lock<std::mutex> cl(commit_mu_);                 // L8：commit_mu_ → mutex_
  {
    std::lock_guard<std::mutex> dl(mutex_);                    // 只写内存
    if (shutting_down_) return Status::IOError("Write: DB is closing");
    if (!bg_error_.ok()) return bg_error_;
    const SequenceNumber seq = last_sequence_ + 1;
    s = mem_->Add(seq, type, key, value);
    if (!s.ok()) return s;                                     // 失败不消耗 seq，不入队
    last_sequence_ = seq;
    w.end_seq = seq;
    EncodeBatchPayload(&encoded, seq, /*count=*/1, type, key, value);
    w.encoded = &encoded;
  }
  queue_.push_back(&w);                                        // 入队与 seq 分配在同一临界区（原子）

  // ── 等待：谓词必须覆盖「我已完成」与「我能接手当 flusher」两件事  L9 ──
  while (!w.done && !(!flusher_active_ && queue_.front() == &w)) w.cv.wait(cl);
  if (w.done) return w.status;                                 // 由别人（含失败路径）结算

  // ── 我当选 flusher：取批（快照边界）──
  flusher_active_ = true;
  Writer* last = nullptr;  uint64_t group_end = 0;  bool need_sync = false;
  size_t bytes = 0;  size_t recs = 0;
  group_buf_.clear();
  while (!queue_.empty()) {
    Writer* f = queue_.front();
    const size_t n = f->encoded->size();
    if (last != nullptr && (bytes + n > kMaxGroupBytes || recs >= kMaxGroupRecs)) break;   // 批上限
    group_buf_.append(*f->encoded);
    bytes += n; recs += 1;
    group_end = f->end_seq;
    need_sync |= f->sync;                                      // D3：任一成员要 sync ⇒ 全批 fsync
    last = f;
    queue_.pop_front();
  }
  cl.unlock();                                                 // ← 出锁，下面才是 IO（I17）

  Status st = commit_error_;                                   // 粘性错误 ⇒ 不做 IO，直接结算
  if (st.ok()) {
    st = logfile_->Append(Slice(group_buf_));                  // 锁外 write
    if (st.ok() && need_sync) st = logfile_->Sync();           // 锁外 fsync（I11 的落点）
  }

  cl.lock();                                                   // ── 回锁发布 ──
  if (st.ok()) { written_seq_ = group_end; if (need_sync) durable_seq_ = group_end; }
  else if (commit_error_.ok()) commit_error_ = st;             // 首个错误成为粘性错误（I16）
  // ① 出队 + 结算 + 唤醒组内成员（严格按批成员顺序）
  for (;;) {
    Writer* r = queue_.empty() ? last : queue_.front();        // 注意：组内成员已在上面的循环里 pop 掉
    ...
  }
```

上面最后一段的写法容易出错（组内成员在取批时已经 `pop_front`，回锁时无法再遍历）。**最终采用的形态**：取批时把成员指针同时记入 `group_members_`（`std::vector<Writer*>`，复用成员变量避免每次分配），回锁后按该向量结算：

```
  cl.lock();
  if (st.ok()) { written_seq_ = group_end; if (need_sync) durable_seq_ = group_end; }
  else if (commit_error_.ok()) commit_error_ = st;
  // ① 结算组内成员（含队首自己）
  for (Writer* m : group_members_) {
    if (m != &w) { m->status = commit_error_; m->done = true; m->cv.notify_one(); }
  }
  group_members_.clear();
  // ② 【顺序不可交换】先清标志，再唤醒新队首（L10）
  flusher_active_ = false;
  if (!queue_.empty()) queue_.front()->cv.notify_one();
  cl.unlock();
  return commit_error_;
```

**四个丢唤醒场景的逐条分析（`m1-review.md` 的"前车之鉴"要求）**

| # | 场景 | 为什么会丢 | 本设计如何避免 |
|---|---|---|---|
| 1 | 写者 W 入队时已有 flusher，于是 `wait()`；当前批结束后 W 不在批内（超出上限） | 若 fiisher 只 `notify_one` 组内成员，W 永远不被唤醒 | ② 的"**唤醒新队首**"就是为 W 准备的；且清标志在唤醒之前，W 醒来后谓词的 `!flusher_active_` 为真 |
| 2 | W 入队后还没进入 `wait()` 就被 flusher 取进批并结算 | W 醒来（或从没睡）时 `w.done == false` 而 `queue_.front() != &w`（已被 pop）⇒ 若谓词只查"我是不是队首"会**永久睡死** | 谓词**先查 `w.done`**；`done` 在 `commit_mu_` 内设置，与 `pop_front` 同一临界区 ⇒ 不会漏 |
| 3 | W 是队首，`flusher_active_ == false`（上一批刚好结束），但 W 已在 `wait()` 中 | 没有人会 `notify` 它（上一批的 ② 已经发过 notify，但 W 是在那之后才进 `wait()` 的——按"先清标志再 notify + 谓词在同一把锁下复查"的规则，这种时序不可能：`wait()` 释放锁、`notify` 需先拿锁） | `std::condition_variable` + 同一把 `mutex_` 的谓词循环在语义上封死了这个窗口（notify 与谓词判定互斥） |
| 4 | 队列非空但 `flusher_active_ == false`（W 在 ② 的 `notify_one` 之后才入队，而此刻还没有 flusher） | W 自己会立即在谓词处发现 `!flusher_active_ && front == &w` ⇒ **自己当 flusher**，不需要别人通知 | 由 D3 的"队首即候选人"设计天然覆盖；不需要额外通知 |

**窗口放开时机（D4 / raft-kv P2a）**：② 的两步是**整个写路径的最后两个动作**。`flusher_active_` 在这一刻之前始终为 `true`，因此：
- 不会有第二个 flusher 在 `Sync()` 进行中/刚返回时抢入 `Append`；
- 组批的"成形窗口"= 取批循环那一段临界区（快照边界），一旦 `cl.unlock()`，`group_buf_` 的内容与顺序就不可变，新写者只可能进入**下一批**（写入顺序 = sequence 顺序，I13 的前提）。

**回归用例**（§9.1 A20）：在 `OnAfterSyncBeforePublish` hook 里统计"窗口内启动的新 flusher 数"，断言 == 0；并以 `OnBeforeUnlockAfterGroupTaken` hook 构造场景 1/2 的精确时序，断言等待者被唤醒而非睡满超时。

### 6.4 失败传播（I16）

| 失败点 | 检测方式 | 传播路径 |
|---|---|---|
| `Append` 短写 | `WritableFile::Append` 内部循环重试 `EINTR`；`write()` 返回 0 或重试后仍短写 ⇒ `kIOError` | 成为粘性 `commit_error_`；本批全部成员（含 flusher 自己）返回同一 `Status`；`bg_error_` 置位 ⇒ 后续 `Write` 立即返回同一错误（D11） |
| `fsync` 失败 | `Sync()` 的返回值**必须被检查**（指令 §1.5 的"双检查"） | 同上；**绝不返回 `kOk`** |
| `ENOSPC` / `EROFS` | `write`/`fsync` 的 `errno` 经 `Status::IOError` 带 `strerror` 上下文 | 同上；既有数据不受影响（不截断、不重写） |
| 恢复期 `kCorruption` | §5.3 | `Open` 失败，`*dbptr = nullptr`，**不修改任何文件**（截断只发生在判定为尾部之后） |

**"双检查"的具体含义**（I11 的落地）：
```
s = logfile_->Append(buf);                       // ① 字节数检查在 Append 内部（written == data.size()）
if (s.ok() && need_sync) s = logfile_->Sync();   // ② fsync 返回值检查
```
`Append` 的契约是"返回 `kOk` ⟺ `data.size()` 字节已全部交给内核（`write()` 返回总数）"，`Sync` 的契约是"返回 `kOk` ⟺ 该 fd 此前全部写入已 durable"。二者都成功才允许 `durable_seq_` 前进。

### 6.5 关闭路径（I20 / L11）

```
Status DBImpl::Close():                       // 幂等
  if (closed_.exchange(true)) return Status::OK();
  std::unique_lock<std::mutex> cl(commit_mu_);
  shutting_down_ = true;                      // 拒绝新写（与入队同一把锁 ⇒ 无竞态窗口）
  cv_commit_.notify_all();                    // 防御性通知（见 §6.3 场景 3/4）
  // 等在途批完成：谓词 = 没有 flusher 且队列已空
  while (flusher_active_ || !queue_.empty()) cv_commit_.wait(cl);
  Status st = logfile_->Sync();               // 最终 Sync（若 sync=false 也做：Close 承诺 durable）
  Status st2 = logfile_->Close();
  cl.unlock();
  // 此后：无后台线程、无写者；迭代器不得晚于 DB 存活（M1 L4/L5 的既有契约）
  return st.ok() ? st2 : st;
```
- **顺序不可换**：拒绝新写 → 等在途批 → `Sync` → `Close` 文件。反过来会 UAF（析构后仍有批在写文件）。
- **M2 无后台线程**（组提交用写者自身当 flusher），故 L12"恢复是单线程"与 I20"关闭后无后台线程访问已释放对象"是**结构性成立**的，而不是靠 join 保证。这是选 D3 方案 A（首到达者当 flusher）的一个额外收益，写进设计理由。
- `DB::~DB()` 调用 `Close()`（虚析构，M1 已 `virtual`）。

### 6.6 M2 新增锁纪律（L7~L12，供 `#1` 的 `m2-prerequisites.md` 引用）

| # | 纪律 | 落地与验证 |
|---|---|---|
| **L7** | DB 互斥锁只保护内存状态（`mem_`/`last_sequence_`/`shutting_down_`/`bg_error_`），不保护 IO | `Locks.ZeroIoWhileHoldingDbMutex`（SpyLogWriter + `MuHeldGuard` 线程局部标志，生产为零开销）+ 代码评审 |
| **L8** | 组提交队列由独立互斥量保护；锁序固定 `commit_mu_ → mutex_`，禁止反向 | TSan + 评审逐处核对（M2 只有 3 处同时持两锁：`Write` 开头、`Close`、`RecoverAndOpen` 的尾部） |
| **L9** | 条件变量谓词必须覆盖「我已完成（`w.done`）」与「我能接手当 flusher（`!flusher_active_ && front == me`）」两个条件 | §6.3 的四场景分析 + `GroupCommit.NoLostWakeup`（`wait_for(200ms)` 超时即 FAIL） |
| **L10** | 交接必须在仍有未结算成员时通知；**清 `flusher_active_` 与唤醒新队首的顺序不可交换**，且不得依赖下一次写入来唤醒 | `GroupCommit.NoLostWakeup` 场景 1 的精确时序用例 + 代码注释标注 |
| **L11** | 关闭路径与并发写者的交互：拒绝新写 → 等在途批完成 → 再销毁 | `Close.RejectsNewWriters`（ASan + TSan） |
| **L12** | 恢复是单线程的；恢复期间不得有后台线程运行（M2 无后台线程 ⇒ 结构性成立） | 代码评审（`RecoverAndOpen` 不创建线程）+ `Recovery.*` 全部用例单线程 |

---

## 7. `sync` 语义

### 7.1 `sync = true` 的 durable-before-ack 论证（I11 / I19）

**承诺**：`Put(WriteOptions{.sync=true}, k, v)` 返回 `kOk` ⟹ `(k, v)` 在返回前已 `write()` 且 `fsync()` 成功，且**崩溃（进程被杀 / 机器重启）后必须可读**。

**判据（可实测的三段式）**：
1. `Append` 返回 `kOk` ⟺ `encoded.size()` 字节已全部被 `write()` 接受（`WritableFile::Append` 内部循环到写完，短写 ⇒ `kIOError`，绝不以 `kOk` 返回）；
2. `Sync` 返回 `kOk` ⟺ `fsync(fd)` 成功（同一 `fd`，无用户态缓冲，`Flush()` 是空操作）；
3. `durable_seq_ >= w.end_seq` 只在 (1)(2) 都成功且**同一临界区内**推进，且 `w.status = commit_error_` 也只在该临界区内写 ⇒ `w.status.ok()` ⟹ `durable_seq_ >= w.end_seq`。

因此"已 ack ⟹ 已 durable"是**结构性成立**的，不依赖任何时序假设。证据记录：`Sync.SyncCoversAllPriorWrites` 断言 MemEnv 的 `last_synced_offset >= 全部写入字节`。

**§5.4 用到的一条推论**：`fsync` 成功的字节不会在崩溃后消失 ⇒ 已 ack 的 record 在恢复时要么被完整重放，要么介质本身损坏（超出 M2 的保证范围）。这条推论是"截断不会丢已 ack 数据"的全部依据。

### 7.2 语义表

| 场景 | `write()` | `fsync` | 返回值含义 | 崩溃后（`kill -9`） | 崩溃后（掉电 / 介质疑失） |
|---|---|---|---|---|---|
| `WriteOptions{.sync=true}` | ✔ 必做 | ✔ 必做（组内任一成员要就做） | 返回 `kOk` = **已 durable** | 必存活 | 必存活（除介质疑失） |
| `WriteOptions{.sync=false}`（默认） | ✔ 必做 | ✘ 不做 | 返回 `kOk` = **已交给内核**（不承诺 durable） | **本 VM 实测必存活**（§11.3：kill -9 不丢 page cache；`sync` 与否都不丢） | **可能丢失最近的连续后缀**（允许）；**不得**出现半条生效、乱序生效、旧值覆盖新值（I19 + §9.1 A27 的判据） |
| `DB::Sync()` | — | ✔ | 返回 `kOk` = **此前全部已 `kOk` 的写入**都已 durable | 全部存活 | 全部存活 |
| `DB::Close()` | — | ✔（即使全部是 `sync=false` 的写） | 返回 `kOk` = 此前全部写入 durable，且文件句柄已安全关闭 | 全部存活 | 全部存活 |
| 写失败（`kIOError`）| 可能部分写入 | 可能未执行 | 返回错误；`bg_error_` 置位，DB 写只读（D11） | 该写**不保证**存活（未 ack）；**已 ack 的写不受影响** | 同左 |
| `MemTable` 触顶（`kFrozen`） | ✘ 不写 WAL | ✘ | `kFrozen`；**不消耗 sequence** | 无影响 | 无影响 |

### 7.3 `sync = false` 的三条硬约束（M2 验收口径）

即使 `sync = false`，下列三条**必须成立**（`CrashSim.*` 用例的判据）：

1. **无半条生效（I19）**：恢复后任一 key 的值只能是某个**完整写入的版本**（逐字节等于某次 `Put` 的 value），不得出现两次写入拼接、截断或部分字节。
   - 机制：value 的字节只在**一条完整的 WAL record** 里；batch 的每条 entry 独立（`type + key_len + key + value_len + value`），fragment 截断由 §5.3 截断到 record 边界保证。
2. **无乱序生效**：同 key 的多版本在恢复后必须满足"编号更小的 log 里的记录、或同文件内更早 byte offset 的记录，其 sequence 更小"。恢复按文件编号升序 + 字节顺序**不重排序**（D7），因此乱序只可能来自写入侧的串行化被破坏。用例断言"重放过程中 `b.sequence` 严格递增"。
3. **无旧值覆盖新值**：恢复后某 key 的最终值 = `sidecar` 中该 key 的某个**前缀末端的值**（在 sidecar 的写入顺序下），不得等于某个被后续写覆盖过的更早值。
   - 机制：丢失只可能发生在"未 durable 的连续后缀"上（掉电语义），即被丢的是最近的一段连续写。

### 7.4 `DB::Sync()` 的实现

```
Status DBImpl::Sync():
  uint64_t target;
  { std::lock_guard<std::mutex> dl(mutex_); target = last_sequence_; }
  if (target == 0) return Status::OK();
  // 走与 Write 相同的组提交路径：入队一个"零 entry"的 barrier Writer（不写 WAL record）
  // 等待谓词：durable_seq_ >= target || !commit_error_.ok()
  // flusher 侧：Append 空缓冲（跳过）+ Sync()
  return st;
```
- `target` 在 `mutex_` 下取，保证覆盖"调用时刻已返回 `kOk` 的全部写"；此后新进来的写不在此次 `Sync` 的承诺内（与 LevelDB 的 `Sync()` 语义一致）。
- barrier Writer **不产生 WAL record**（`count == 0` 的 batch 非法，故不入 `group_buf_`），只贡献 `need_sync = true`。

---

## 8. 崩溃对账协议

### 8.1 写入端（`lsm_crash_writer`）

```
usage: lsm_crash_writer <dbdir> <sidecar> <mode> [--keys N] [--ack-sync-every K]
  mode = sync | nosync

Open(dbdir, Options{.write_buffer_size = 64 MiB})          // 避免 kFrozen 干扰（§1.2 边界 1）
for i in 1..N:
   k = sprintf("kW-%08d", i);  v = sprintf("v-%08d-%s", i, filler)
   s = db->Put(WriteOptions{.sync = (mode == sync)}, k, v)
   if !s.ok(): fprintf(stderr, "PUT_FAIL %s\n", s.ToString()); exit(2)   // 绝不当成功
   // ★ 顺序不可颠倒：先确认 Put 成功，才记 sidecar（否则会产生假 missing）
   fprintf(ack_fd, "seq=%llu key=%s val_sha1=%s\n", i, k, sha1(v))
   if (i % K == 0) fsync(ack_fd)          // 默认 K=1（逐行落盘）；--ack-sync-every 调大以压测
  // 压测模式：另起 M 个线程并发 Put（组提交的并发正确性）
  while (true) { /* 持续写到被 kill -9 */ }
```

- sidecar 行**自带校验字段**（`key` + `val_sha1`），使对账工具能独立验证"恢复后的值逐字节相等"，不依赖 `lsm_crash_writer` 的额外状态。
- 退出码：`2` = 写失败（测试框架视为**必失败**，与 `missing` 无关）；被 `kill -9` 时无退出码。

### 8.2 对账端（`lsm_crash_recover`）

```
Open(dbdir, ...)                          // 若返回错误 → 输出 OPEN_FAIL 并 exit 1
for each line in sidecar:
    parse (seq, key, want_sha1)
    s = db->Get(key, &got)
    if !s.ok():                                  missing++
    else if sha1(got) != want_sha1:              mismatch++
    else                                         recovered++
// 全局一致性：任一 key 的最终值必须是"某个前缀末端"（sync=false 的软判据，见 §7.3）
Close()
print "ROUND <n> MODE <mode> ACKED <a> RECOVERED <r> MISSING <m> MISMATCH <x> TRUNCATED_BYTES <t> OPEN_MS <ms>"
exit (missing == 0 && mismatch == 0) ? 0 : 1
```
- `TRUNCATED_BYTES` 由 `lsm_crash_recover` 从 `DBImpl` 的恢复报告读出（恢复时打印，也提供一个 `GetRecoveryStats()` 诊断接口——**这个接口 M2 就要有**，否则门禁看不到截断量）。
- `OPEN_MS` 用 `CLOCK_MONOTONIC`（`Env::NowMicros`）。

### 8.3 驱动脚本（`scripts/lsm_crash_test.sh`）

```
usage: lsm_crash_test.sh [--rounds 100] [--mode sync|nosync|both] [--kill-min-ms 50] [--kill-max-ms 500]

for r in 1..rounds:
    dir=$(mktemp -d /tmp/lsm_crash.XXXXXX)
    lsm_crash_writer "$dir" "$dir/../ack.$r" $mode &
    pid=$!
    sleep $(( random(kill_min..kill_max) ))ms          # 写压测进行中
    kill -9 $pid; wait $pid 2>/dev/null
    out=$(lsm_crash_recover "$dir" "$dir/../ack.$r" $r $mode)
    echo "$out"                                        # 固定行格式，便于 CI 解析
    echo "$out" | grep -qE 'MISSING 0 ' || { echo "ROUND_FAIL $r"; exit 1; }
    rm -rf "$dir"
echo "TOTAL_ROUNDS $rounds MISSING_TOTAL 0"
exit 0
```
- 每轮**独立临时目录**（避免跨轮污染）；`trap` 清理；`missing != 0` 或 `OPEN_FAIL` **立刻退出码 1**（照搬 raft-kv 的门禁风格）。
- **`--mode sync` 是 `missing 0` 门禁的唯一合法模式**（§7.2：只有 `sync=true` 的 ack 才有 durable 承诺）。
- `--mode nosync` 作为**对照轮次**，判据改为 §7.3 的三条（`MISSING` 不参与门禁，但 `MISMATCH`、`TRUNCATED_BYTES` 的异常增长参与），并在输出里标注 `MODE nosync`。**必须在脚本注释里写明**："本 VM 实测 `kill -9` 不丢 page cache（§11.3），故 `nosync` 轮的 `MISSING` 预期也是 0；它只验证进程级一致性，不验证掉电语义——掉电语义由 A 组的 `CrashSim.*`（MemEnv 回滚到 fsync 水位）覆盖。"
- `sync=false` 的掉电语义**不靠 kill -9 验证**（这一点必须写进 `m2-prerequisites.md` 的"测试前置假设"，否则会出现"跑了 nosync 轮且 missing 0 ⇒ 误以为 sync=false 也是 durable"的错误结论）。

---

## 9. 测试矩阵

> 每条用例给出：**名字 / 依赖的假设 / 通过判据 / 需要的 seam**。
> A 组必须零 flaky（无真实磁盘语义依赖、无真实时间依赖、无网络）；B 组进程级/磁盘级，脚本驱动。
> **A 组凡涉及随机的用例必须固定种子，并把种子打进 INFO / RecordProperty**（失败必须可复现，见 §9.4）。

### 9.4 修订（#1 阶段回退 #0）

- **A20 判据与零 flaky 冲突**：初稿写「64 个并发写者，`Sync` 调用次数 `<= 8`」。若调度器恰好让写者串行推进，
  每人自成一批 ⇒ 64 次 fsync ⇒ 断言失败。这不是实现 bug，是测试在赌调度。改为用 `CommitHook` 造**确定性屏障**（A20），
  并把真实无屏障并发下的统计降级为 **A20b**（只断言 `fsync_calls < writer_count`，不作硬门禁）。
- **A27/A28 的随机性未固定种子**：崩溃模拟必须用固定种子（失败可复现），种子打进 INFO；
  `MemEnv::SimulateCrash()` 的撕裂长度也只能来自该种子驱动的 PRNG。

### 9.1 A 组（确定性：MemEnv + FakeClock + 可注入故障）

**WAL 层（M2.1，`tests/wal_test.cpp`）**

| # | 名字 | 依赖的假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| A01 | `WAL.WriteReadRoundTrip` | MemEnv 忠实实现 `append`/`read` | 1..N 条随 record 往返逐字节一致；含空 payload 的**拒绝**路径 | `MemEnv` |
| A02 | `WAL.LargeRecordCrossBlockSplit` | 无 | 边界四组：payload = `32761`（正好一块）、`32762`（跨两块）、`2*32761`（跨三块）、`1`（最小合法）；`record.size() == 0` ⇒ 构造期拒绝 | 无 |
| A03 | `WAL.BlockTailPadding` | 无 | 写入使 `block_offset` 落在 **`32761..32767`**（**含 32761**：剩余正好 7 字节是 writer 会写出 length=0 片段的边界，见 §4.2 修订）的 record，断言 padding 为 0、**任何片段的 length 都 >= 1**、reader 正确跳过；`kZeroType` 不被当成 record | 无 |
| A04 | `WAL.CrcDetectsSingleByteFlip` | 无 | 对 `crc`/`length`/`type`/payload 四处各逐位翻转（≥4×8 次），**每次都不得被当成有效 record**；payload 翻转必须 CRC 失败 | 无 |
| A05 | `WAL.RejectsIllegalTypeAndLength` | 无 | `type ∉ {1..4}`、`length == 0`、`length > 32761` ⇒ 判损坏，**且不得越界读**（ASan 下跑） | ASan |
| A06 | `WAL.TruncatedTailIsCut` | 无 | 对一条含 3 条 record 的 WAL，**对每个长度 0..file_size 都做一遍**：`Open`/`Scan` 必须返回 `CLEAN`（长度落在 record 边界）或 `TAIL_RESIDUE`，且 `last_good_end` 是某个 record 边界；任何长度都不得 panic / 越界 / 死循环 | ASan + 逐字节截断循环 |
| A07 | `WAL.MiddleCorruptionRejected` | 无 | 翻转**非最后一条** record 的 header/payload ⇒ `kCorruption`，`Status::ToString()` 含文件名与字节偏移与 `resync` 位置 | 无 |
| A08 | `WAL.TailCorruptionWithNoValidRecordAfterIsCut` | 无 | 翻转最后一条 record 的 payload ⇒ 截断到最后一条完好 record，`TRUNCATED_BYTES > 0` 且恢复报告可见 | 恢复报告接口 |
| A09 | `WAL.ShortWriteIsRetriedThenFails` | `Env` 可注入短写 | ① `write` 每次只写 1 字节 ⇒ `Append` 仍成功且内容正确；② `write` 永久返回 0/`ENOSPC` ⇒ `Append` 返回 `kIOError` 且**不**返回 `kOk` | `MemEnv::SetShortWrite(n)` / `SetEnospc(true)` |
| A10 | `WAL.FsyncFailurePropagates` | `Env` 可注入 `fsync` 失败 | `WAL.FsyncFailurePropagates`：`Sync()` 返回 `kIOError` ⇒ `Write` 返回同一错误，且 `durable_seq_` **未**前进 | `MemEnv::SetSyncFailureAfter(n)` |

**恢复层（M2.2，`tests/recovery_test.cpp`）**

| # | 名字 | 依赖的假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| A11 | `Recovery.ReplayPutDeleteAndBatches` | 恢复后 `Get` 只查 MemTable 即代表全量数据（M2 成立） | Put/Delete 重放后 `Get` 结果与崩溃前内存态一致；tombstone 生效 | `MemEnv` |
| A12 | `Recovery.MultiFileAscendingOrder` | 编号规则 | 手工放 `000001.log`/`000002.log`/`000010.log`，恢复顺序必须是 1→2→10（**字符串序会是 10→1→2，故必须有数值排序用例**） | `MemEnv` |
| A13 | `Recovery.SequenceRestoredToMaxReplayed` | 无 | 恢复后 `last_sequence_ == max(seq+count-1)`；`GetRecoveryStats()` 可读；随后新写的 seq 严格大于任何已重放 record 的 seq | 诊断接口 |
| A14 | `Recovery.IdempotentAcrossRepeatedOpens` | 无 | 连续 `Open`/`Close` 3 次：WAL 文件内容 sha1 不变、`Get` 结果一致、`last_sequence_` 一致、无泄漏（LSan） | `MemEnv` + LSan |
| A15 | `Recovery.TornTailDoesNotResurrectOlderValue` | 无 | 同 key 写 `v1`（完整）+ `v2`（撕裂）⇒ 截断后 `Get` 必须返回 `v1`，**不得**返回半条 `v2`、也不得返回 `kNotFound` | 逐字节截断 |
| A16 | `Recovery.EmptyWalAndMissingDir` | 无 | 空 `*.log`、`Open` 不存在的目录（自动创建）、目录里只有非 `.log` 文件 ⇒ 全部 `kOk` 且 `Get` 返回 `kNotFound` | `MemEnv` |
| A17 | `Recovery.MalformedBatchPayloadIsCorruption` | 无 | `count == 0`、`count > kMaxBatchCount`、entry 解码后有剩余字节、key 为空/超 64 KiB ⇒ `kCorruption`（不是 `kOk`、不是崩溃） | 手工构造 WAL 字节 |
| A18 | `Recovery.NonHighestLogTailCorruptionRejected` | 无 | 在 `000001.log`（非最高）注入尾部残骸 ⇒ `kCorruption` 拒绝启动 | `MemEnv` |
| A19 | `Recovery.SameKeyManyVersionsReplaysLatest` | `InternalKeyComparator` 降序 | 同 key 1000 个版本重放后 `Get` 返回最大 seq 的值；内部迭代 1000 条且 seq 严格降序 | 无 |

**组提交与锁（M2.3，`tests/crash_test.cpp` 的同步部分）**

| # | 名字 | 依赖的假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| A20 | `GroupCommit.NWritersOneFsync` | **无（不依赖调度）** | **确定性**：64 个写者全部在 `CommitHook` 的屏障点等待，第 64 个入队后统一放行 ⇒ 断言「本批含全部 64 个写者」且「**本批 fsync 次数 == 1**」 | `CommitHook` + `SpyLogWriter` |
| A20b | `GroupCommit.BatchingReducesFsyncCount` | 线程调度可产生并发（**允许调度相关，不作硬门禁**） | 无屏障真实并发 64 写者：`fsync_calls < writer_count` 且全部 `kOk`；实测 `fsync_per_write` 打进 INFO | `SpyLogWriter` |
| A21 | `GroupCommit.NoLostWakeup` | 无 | 用 `OnBeforeUnlockAfterGroupTaken` 构造"最后写者在放锁窗口内"的时序：所有写者在 `wait_for(200ms)` 内完成，**超时即 FAIL**（不是"慢"而是"丢唤醒"）；覆盖 §6.3 的场景 1/2/3/4 | `CommitHook`（测试注入；生产为 `nullptr`） |
| A22 | `GroupCommit.MixedSyncPropagates` | 无 | 一批内混入 `sync=false` 与 `sync=true` 的写者 ⇒ 该批必须 `fsync`；`sync=true` 的写者返回 `kOk` 后 `durable_seq_ >= 其 end_seq` | `SpyLogWriter` + `MemEnv` 的 fsync 水位 |
| A23 | `GroupCommit.FailurePropagatesToAllWaiters` | 注入失败 | 注入 `fsync` 失败 ⇒ **本批全部**等待者都拿到非 `kOk`，**无一人**拿到 `kOk`；`commit_error_` 粘性；后续写立即返回同一错误 | `MemEnv::SetSyncFailureAfter` |
| A24 | `GroupCommit.WindowNotOpenedEarly` | 无 | 在 `OnAfterSyncBeforePublish` 内计数"新启动的 flusher 数"，断言 == 0；且 `durable_seq_` 在该 hook 内尚未推进 | `CommitHook` |
| A25 | `Locks.ZeroIoWhileHoldingDbMutex` | 无 | `SpyLogWriter` 记录每次 `Append`/`Sync` 调用时的"是否持 DB 锁"（`MuHeldGuard` 线程局部标志）；断言**持锁期间的 IO 调用数 == 0** | `MuHeldGuard`（仅测试构建启用）+ `SpyLogWriter` |
| A26 | `Locks.LockOrderIsQueueThenDb` | 无 | TSan 无报告；代码评审逐处核对 3 个双锁点 | TSan |

**崩溃语义（`CrashSim`，M2.2/M2.3）**

| # | 名字 | 依赖的假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| A27 | `CrashSim.UnsyncedSuffixLostNoTornValue` | **MemEnv 忠实模拟"崩溃 = 回滚到 fsync 水位"**（这是 M2 唯一能确定性验证掉电语义的机制） | 随机写 N 条（`sync=false`）→ 随机 `SimulateCrash()`（回滚到 `last_synced_offset`，并以随机概率把最后一个块撕裂成随机长度）→ 恢复：① 每个 key 的值逐字节等于某次完整写入的值（无半写）；② 重放 sequence 严格递增（无乱序）；③ 最终值 ∈ sidecar 的某前缀末端（无旧值覆盖新值） | `MemEnv::SimulateCrash()` + `MemEnv::SetTearProbability(p)` |
| A28 | `CrashSim.RepeatedCrashRecoverLoop50` | 同上 | 同一目录连续 50 轮"写→崩溃→恢复"，每轮断言 A27 的三条 + `last_sequence_` 单调不减 + LSan 无泄漏 + 无 fd 泄漏 | `MemEnv` 崩溃 API |
| A29 | `Sync.SyncCoversAllPriorWrites` | 无 | 写 100 条（`sync=false`）→ `DB::Sync()` → 断言 `MemEnv.last_synced_offset >= 全部写入字节`；随后 `SimulateCrash()` → 100 条全部可读 | `MemEnv` |
| A30 | `Sync.CloseIsDurable` | 无 | `Close()` 后 `SimulateCrash()` → 全部可读；重复 `Close()` 幂等返回 `kOk` | `MemEnv` |
| A31 | `Close.RejectsNewWriters` | 无 | `Close()` 期间并发 `Put` ⇒ 明确 `Status`（不是 `kOk`、不 UAF、不死锁）；ASan + TSan 干净 | 多线程 + sanitizer |

### 9.2 B 组（真实磁盘 / 进程级，脚本驱动）

| # | 名字 | 依赖的假设 | 通过判据 | 需要的 seam / 命令 |
|---|---|---|---|---|
| B01 | `lsm_crash_test.sh --rounds 100 --mode sync` | ① `fsync` 语义被内核与宿主遵守；② 本 VM 的 fsync 不返回假成功（**不可验证宿主缓存**，见 §11.2 的诚实说明） | **100/100 轮 `MISSING 0` 且脚本退出码 0**；每轮 `OPEN_MS`/`TRUNCATED_BYTES` 入档 | 真实临时目录 + `kill -9` |
| B02 | `lsm_crash_test.sh --rounds 30 --mode nosync` | 同上 + "kill -9 不丢 page cache"（§11.3 实测） | `MISMATCH 0`、无半写、无乱序、无旧值覆盖；`MISSING` 如实记录（**不作为门禁**） | 同上 |
| B03 | `lsm_tail_truncate_test.sh` | 无 | 生成一条含 200 条 record 的真实 WAL；对 `truncate` 长度 ∈ {每个 record 边界的 ±3 字节} 全部做一遍：`Open` 必须成功且恢复出的 key 集合 == 前 k 条（k 由截断长度决定）；无 panic | 真实文件 + `truncate(1)` |
| B04 | `lsm_corrupt_middle_test.sh` | 无 | 用 `dd` 翻转中间 record 的一个字节 ⇒ `Open` 返回 `kCorruption` 且 `stderr` 含文件名 + 偏移 + `resync` 位置；退出码非 0 | 真实文件 + `dd` |
| B05 | `lsm_recovery_stats.sh` | 无 | 记录"WAL 大小 → `OPEN_MS`"若干点的基线（供 M3 对照）；输出固定格式 | 真实文件 |
| B06 | 真实 `ENOSPC` / 只读目录 | 需要挂载权限 | 返回明确 `Status`、不破坏既有数据、不 panic。**若本 VM 无法制造真实 ENOSPC（需要 root 挂小 loop 设备），如实记录"未验证"，由 A09/A23 的注入式覆盖承担判据** | `fallocate` 填满 / `chmod` 只读目录 |
| B07 | `fsbench_commit_latency`（微基准） | 无 | 四档 + 目录 fsync 的 `MIN/MEDIAN/P90/MAX`（§11.2 已给出 M2 前的环境探测值）；固定输出格式供 M5 引用 | `scripts/fsbench_commit_latency.cpp`（**新增，属 M2 交付物**，风格对齐 raft-kv 同名脚本） |

### 9.3 微基准的固定输出格式（G6）

```
== fsbench: 单次提交延迟（4096B 追加 + flush，N=500，目录 <dir>）==
machine 8 cores  load <l1> <l5> <l15>  fs ext4  mount <src>
STRATEGY append+fsync          N  500  MIN_MS  x.xxx  MEDIAN_MS  x.xxx  P90_MS  x.xxx  MAX_MS  x.xxx
STRATEGY append+fdatasync      ...
STRATEGY prealloc+fsync        ...
STRATEGY O_DIRECT+fsync        ...
STRATEGY create+fsync+dirfsync ...
NOTE 每次提交 = 1 条 4 KiB 记录（预分配档先 posix_fallocate(64 MiB)）
```
必须打印 `machine`/`load`/`fs`/`mount` 行——§11.2 的经验教训是：**同一个脚本、同一台 VM、不同时间点，结果可以差 3 倍**，不记环境就无法解释差异。

---

## 10. 子里程碑拆分（M2.1 → M2.2 → M2.3）

| 阶段 | 内容（只实现让本级判据通过的最小代码） | 通过判据 | 提交信息 | 证据命令（原始输出必须入档） |
|---|---|---|---|---|
| **M2.1** | `src/wal.{h,cpp}`（writer/reader、§4 的位级格式、跨块切分、padding、CRC、损坏检测与 resync）+ `Env` 增补（§5.7）+ `tests/memenv.{h,cpp}` + `tests/wal_test.cpp` | A01~A10 全绿；ASan 干净；干净重建 0 warning | `m2.1: WAL record 格式与跨块切分（docs/m2-design.md §4）` | `bash scripts/lsm_build.sh`；`cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests --gtest_filter='WAL.*'` |
| **M2.2** | `src/filename.{h,cpp}`、`src/db_impl.{h,cpp}`（§5 的恢复 + §6.2 的 `log_mu_` 过渡写路径 + `Sync()`/`Close()`）、`src/db.{h,cpp}` 增补 `WriteOptions`/`Sync`/`Close`、`tests/recovery_test.cpp`、`scripts/lsm_crash_test.sh` + `lsm_crash_writer`/`lsm_crash_recover` | A11~A19 + A27~A31 全绿；`lsm_crash_test.sh --rounds 100 --mode sync` **`MISSING 0` 退出码 0** | `m2.2: DB::Open 崩溃恢复与对账门禁（docs/m2-design.md §5/§7/§8）` | `bash scripts/lsm_build.sh`；`bash scripts/lsm_crash_test.sh --rounds 100 --mode sync`（贴 100 行 ROUND + `TOTAL_ROUNDS 100 MISSING_TOTAL 0`）；`bash scripts/lsm_tail_truncate_test.sh` |
| **M2.3** | §6.3 的组提交（`commit_mu_`/`pending` 队列/flusher 选举/谓词/窗口放开/失败传播）、`CommitHook` 与 `SpyLogWriter`/`MuHeldGuard`、A20~A26、`scripts/fsbench_commit_latency.cpp` | A20~A26 全绿；**TSan 干净**；微基准输出入档；tag `m2-wal` | `m2.3: 组提交与 sync 语义（docs/m2-design.md §6/§7）` | `cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j8 && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests`；`./build/bin/fsbench_commit_latency /tmp/fsb 500`；**M2.3 后必须复跑 M2.2 的全部 B 组门禁** |
| 收口 | tag `m2-wal`（`#4` 评审阻断项修完后） | 全部 A/B 组 + ASan + TSan + 0 warning + 100 轮 `missing 0` + 微基准 | — | 按 `docs/roadmap.md` §3.4"未跑不算过" |

**规则**（沿用 `M2-WAL与崩溃恢复.md` §3 的 #3 段）：

- M2.1 结束时 `DBImpl` 还不存在（`DB::Open` 仍返回 `kNotSupported`），A 组只跑 `WAL.*` 过滤集；**`scripts/lsm_build.sh` 必须仍然全绿**（M1 的 47 例不得回归）。
- M2.2 的写路径是**串行低吞吐但正确**的（每写一次 fsync）；M2.3 只把它换成组提交，**`Write()` 的签名、返回值与全部不变量不变**——这是"每步一个提交、判据可实测"的关键：M2.2 的 100 轮门禁在 M2.3 之后必须同样通过。
- 每个提交只引用 `docs/m2-design.md` 的章节号，不引用未冻结的临场决定；若实现中发现设计与现实不符，**回退 #0 改设计**（流程锁，禁止在校验/实现阶段私自改方案）。
- `CMakeLists.txt` 最小改动：新增源文件加入 `lsm` 静态库，新增测试文件加入 `lsm_tests`，**保留 M1 全部目标与测试用例不变**；`.gitignore` 追加崩溃脚本的临时目录模式。

---

## 11. 环境探测与原始证据（#0 的硬要求，全部为实测原始输出）

> 执行时间：2026-09-29 19:29–19:41 (+08:00)；机器 `tengyujie-virtual-machine`，Linux 6.8.0-138-generic x86_64，8 vCPU，
> 根文件系统 `/dev/mapper/vgubuntu-root` ext4；探测程序全部放在 `/tmp/m2probe/`，**未触碰仓库源码**。

### 11.1 `~/lsm-kv` 当前可干净构建（HEAD `56d0cc2`，工作区干净）

```
$ cd ~/lsm-kv && git log --oneline -3 && git status --porcelain
56d0cc2 docs(m1): #4 评审处置归档 —— m1-review.md + design §4.4 状态表纠错 + prerequisites §9.5/§10
41be47c fix(m1): #4 评审两处阻断项 + 6 条优化建议落地（含 2 个回归用例）
cb0a3b7 m1.3: 压力边界与门禁收口（docs/m1-design.md §11/§12）
（git status --porcelain 无输出 = 工作区干净）

$ rm -rf build && bash scripts/lsm_build.sh   # 日志 /tmp/m2probe/build.log
== lsm_build: root=/home/tengyujie/lsm-kv build_dir=build jobs=8 ==
[CHECK] warning 计数 = 0（要求 0）
===== [2026-09-29T19:29:56+08:00] 运行 lsm_tests =====
[==========] 47 tests from 12 test suites ran. (22554 ms total)
[  PASSED  ] 47 tests.
[CHECK] 用例计数：
[==========] 47 tests from 12 test suites ran. (22554 ms total)
[  PASSED  ] 47 tests.
[OK] 干净重建 + 0 warning + lsm_tests 全绿（日志：build/build.log）
EXIT=0
```

工具链：`g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0`、`cmake version 3.22.1`、GTest 静态库位于 `/usr/local/lib/{libgtest.a,libgtest_main.a}`。
探测期间 `loadavg = 0.85 0.97 1.17`（静载）。

### 11.2 commit 级 fsync 成本（M2 指令给的"约 8 ms"**未复现**）

自写探针 `/tmp/m2probe/fsprobe.c`（gcc -O2，五种策略 + 目录 fsync，`N=500`）：

```
$ cd /tmp/m2probe && ./fsprobe /tmp/m2probe/fs 500
STRATEGY append+fsync         N  500 MIN_MS    0.748 MEDIAN_MS    2.805 P90_MS    3.579 MAX_MS    6.779
STRATEGY append+fdatasync     N  500 MIN_MS    0.578 MEDIAN_MS    2.566 P90_MS    3.276 MAX_MS    5.468
STRATEGY prealloc+fsync       N  500 MIN_MS    1.010 MEDIAN_MS    2.946 P90_MS    3.734 MAX_MS    6.480
STRATEGY O_DIRECT+fsync       N  500 MIN_MS    0.843 MEDIAN_MS    1.918 P90_MS    2.508 MAX_MS    4.346
STRATEGY append-nosync        N  500 MIN_MS    0.001 MEDIAN_MS    0.001 P90_MS    0.003 MAX_MS    0.070
STRATEGY create+fsync+dirfsync N   50 MIN_MS    1.311 MEDIAN_MS    2.233 P90_MS    3.005 MAX_MS    3.961
DONE
```

重复测量（`append+fsync` 中位，不同时间点）：`2.290 / 2.729 / 2.861 / 2.818 / 2.805 ms` —— **稳定在 2.3~2.9 ms，不是 8 ms**。

**独立交叉核对 1**：用 raft-kv 自己的 `scripts/fsbench_commit_latency.cpp`（**原文件复制到 `/tmp` 编译，未改 raft-kv 仓库**）在本机复跑：

```
$ g++ -O2 -std=c++17 -o fsbench_raft fsbench_commit_latency.cpp && ./fsbench_raft
== 单次提交延迟（4096B 追加 + flush，200 次，本机 /tmp）==
fsync（当前实现）      n=200  avg=2.155ms  p50=2.185ms  p99=3.682ms  max=3.711ms
fdatasync                    n=200  avg=2.258ms  p50=2.256ms  p99=4.876ms  max=5.769ms
prealloc+fdatasync           n=200  avg=2.155ms  p50=2.188ms  p99=3.327ms  max=3.430ms
prealloc+fsync               n=200  avg=2.680ms  p50=2.698ms  p99=4.261ms  max=4.764ms
O_DIRECT+fdatasync           n=200  avg=2.568ms  p50=2.784ms  p99=3.819ms  max=3.945ms
（第二次）
fsync（当前实现）      n=200  avg=2.875ms  p50=3.056ms  p99=4.589ms  max=4.773ms
```
而 raft-kv `docs/m5-bench.md`/`docs/m5-review.md` 记录的是 `fsync 9.67ms / fdatasync 9.74ms / 预分配+fsync 7.93ms / O_DIRECT 7.60ms`。

**独立交叉核对 2（协调者 `session-715f77e8` 的独立实测，不同探针、不同时间点）**：

```
write+fsync(4KiB)     n=200  mean=2.611ms  p50=2.556ms  p99=4.245ms  min=1.194ms  max=4.325ms
write+fdatasync(4KiB) n=200  mean=2.879ms  p50=2.891ms  p99=4.091ms
文件系统 ext4 /dev/mapper/vgubuntu-root，非预分配稀疏文件
```

**三方测量并置**（同一台 VM，三种独立探针，不同时间点）：

| 探针 | `fsync` 中位 / 均值 | `fdatasync` |
|---|---|---|
| 本设计 §11.2 `fsprobe`（128 B 追加 + fsync，N=500） | **p50 = 2.805 ms** | p50 = 2.566 ms |
| 本设计 §11.2 raft-kv `fsbench`（4 KiB 追加，N=200） | avg 2.155 / 2.875 ms | avg 2.258 / 2.653 ms |
| 协调者独立实测（4 KiB，N=200） | **p50 = 2.556 / mean = 2.611 ms** | mean = 2.879 ms |

**三方一致落在 2.2~3.1 ms**，与历史记录的 7.6~9.7 ms 相差约 **3 倍**。

**结论（如实登记，不编造）**：
1. **M2 指令与 roadmap 里写的"commit 级 fsync 约 8 ms 量级"在本机当前状态下不成立**；三方独立实测一致落在 **2.3~2.9 ms**（约 1/3）。
2. 同一份 raft-kv 脚本在本机复跑也给出 2.2~3.1 ms ⇒ **不是探针口径差异**，而是**时间点/宿主缓存状态差异**。最可能的原因是虚拟磁盘由宿主页缓存承接（guest 侧 `/sys/block/*/queue/write_cache` 全部显示 `write through`，但**宿主是否真的落盘无法从 guest 内验证**）。
3. **必须警惕的推论**：如果 fsync 实际未到达物理介质，那么"durable-before-ack"在该 VM 上**无法用真实掉电验证**；`kill -9` 只能证明进程级一致性（见 §11.3）。这条限制必须写进 `m2-prerequisites.md` 的风险清单，并且 **M5 的性能分母必须重测，不能继承 8 ms**。
4. 其他可用的相对结论（同轮同机，可用作实现内部的相对判断）：`fdatasync` 与 `fsync` 基本持平（差异在噪声内，二者互有胜负）；`O_DIRECT+fsync` 在预分配下略优（≈1.9 ms vs 2.8 ms）；无 fsync 的纯追加是 **1 µs 量级**（≈ 3 个数量级差距）。

**组提交收益的定量论证（用实测值，不外推）**：

设单次提交落盘成本 `T_commit ≈ 2.6 ms`（三方中位数区间中值），则：

| 并发写者数 | 无组提交（每写一次 fsync） | 有组提交（每批 1 次 fsync） | 相对收益 |
|---|---|---|---|
| N = 1 | 2.6 ms/写 → 385 写/s | 2.6 ms/写 → 385 写/s（批=1，**不劣化**：D3 的"至少含队首自己"保证了这一点） | 1.0× |
| N = 8 | 2.6 ms/写 → **385 写/s**（写者之间串行等待 FSYNC，吞吐与并发度无关） | 2.6 ms/批 → **≈ 3 077 写/s** | ≈ 8× |
| N = 64（批上限 64） | 385 写/s | 2.6 ms/批 → **≈ 24 615 写/s** | ≈ 64× |

关键洞察（也是本设计把 `kMaxGroupRecs` 设为 64 的量化依据）：**无组提交时吞吐与并发度完全解耦**——所有写者排队等同一个串行 fsync，加线程只能加尾延迟；有组提交时 `T_commit` 被整批摊薄，吞吐 ∝ 批大小。上表的绝对数字**只用于说明实现内部的相对结论**（`roadmap.md` §3.7"不吹"），不构成任何跨机器承诺；实测比值由 §9.1 A20（`fsync_per_write`）与 §9.2 B07 给出。

目录 fsync（`create+fsync+dirfsync`）中位 **2.233 ms**，与单次文件 fsync 同量级 ⇒ M2 只在"新建目录/新建首个 log"时付这个成本（一次），不在写路径上。

### 11.3 `kill -9` 后 WAL 尾部的真实形态（**M2 最重要的一条经验证据**）

探针：`/tmp/m2probe/waltail/{waltail_writer.c,waltail_verify.c,waltail_run.sh}`。
record = `crc32c(4)||length(2)||type(1)||payload(16)` = **23 字节**，每条一次 `write()`，不跨块。
三种模式：`nosync`（逐条 `write()`，不 fsync）、`sync`（逐条 `write()` + `fsync`）、`stdio`（`fwrite` 逐条，4 KiB stdio 缓冲，不 `fflush`）。

**(a) 单轮：kill -9 后逐字节 dump 与结构判定**

```
---- mode=nosync ----（12 轮全部）
round  1 killdelay_ms  194  FILE .../wal_nosync_1.log SIZE 1565403 COMPLETE 68061 LAST_GOOD_END 1565403 TAIL_LEN 0 TAIL_CLASS CLEAN TAIL_HEX (none)
round  4 killdelay_ms  198  ... SIZE 1698504 COMPLETE 73848 LAST_GOOD_END 1698504 TAIL_LEN 0 TAIL_CLASS CLEAN TAIL_HEX (none)
round 12 killdelay_ms   49  ... SIZE  416921 COMPLETE 18127 LAST_GOOD_END  416921 TAIL_LEN 0 TAIL_CLASS CLEAN TAIL_HEX (none)
---- mode=sync ----（12 轮全部 CLEAN，写入量小得多：每轮 14~70 条）
round 10 killdelay_ms   42  ... SIZE 322 COMPLETE 14 LAST_GOOD_END 322 TAIL_LEN 0 TAIL_CLASS CLEAN TAIL_HEX (none)
---- mode=stdio ----
round  1 killdelay_ms  103  ... SIZE 9236480 COMPLETE 401586 LAST_GOOD_END 9236478 TAIL_LEN 2 TAIL_CLASS PARTIAL_HEADER TAIL_HEX d8e4
round  2 killdelay_ms   58  ... SIZE 9089024 COMPLETE 395174 LAST_GOOD_END 9089002 TAIL_LEN 22 TAIL_CLASS PARTIAL_BODY   TAIL_HEX 872733c8100001a60706000000000003a2a3a5a5a5a5
round  7 killdelay_ms   63  ... SIZE 5890048 COMPLETE 256089 LAST_GOOD_END 5890047 TAIL_LEN 1 TAIL_CLASS PARTIAL_HEADER TAIL_HEX 84
round 10 killdelay_ms  198  ... SIZE 19046400 COMPLETE 828104 LAST_GOOD_END 19046392 TAIL_LEN 8 TAIL_CLASS PARTIAL_BODY TAIL_HEX da681ab1100001c8

尾部 96 字节 dump（nosync round 1，SIZE=1565403 = 23 × 68061，余数 0）：
00000000: a5a5 a5a5 6eaa 1169 1000 01d9 0901 0000  ....n..i........
00000010: 0000 007c aca4 a5a5 a5a5 a5a6 c293 9010  ...|............
00000020: 0001 da09 0100 0000 0000 7fac a4a5 a5a5  ................
00000030: a5a5 b1c8 493b 1000 01db 0901 0000 0000  ....I;..........
00000040: 007e aca4 a5a5 a5a5 a5c7 657b 6610 0001  .~...........e{f.
00000050: dc09 0100 0000 0000 79ac a4a5 a5a5 a5a5  ........y.......
（可读出：`1000 01` = length=16 LE, type=1(kFullType)；record 一个接一个，末尾对齐到 23 字节边界）
```

**(b) 100 轮普查：`size % 23` 的分布**

```
MODE nosync ROUNDS 100 TAIL_WHOLE_RECORDS 100 TAIL_TORN 0
  size%23 分布: 0 0 0 0 0 0 0 0 0 0 ... （100 个 0）
MODE stdio ROUNDS 100 TAIL_WHOLE_RECORDS 5 TAIL_TORN 95
  size%23 分布: 6 4 1 2 0 15 11 12 14 20 13 1 0 13 14 22 8 21 14 17 ...
```

**(c) 结论（直接决定 §4 与 §8 的设计）**

1. **Linux 上对普通文件的一次 `write()` 不会被 `SIGKILL` 打断**：`kill -9` 之后文件的**尾部永远是整条 record 的边界**（100/100 轮 `size % 23 == 0`）。因此 **`kill -9` 本身产生不了 torn record**。
2. **实践中真正的 torn tail 来自用户态缓冲**：`stdio` 模式（4 KiB 缓冲、不 `fflush`）**95/100 轮**留下撕裂尾部，形态正好是"最后一个 4096 字节 stdio 块被写了一半" ⇒ 撕裂点落在某条 record 的中间，且残留字节数 = `文件大小 - 最后一条完整 record 末尾`，**没有任何规律**（分布 1..22 均匀）。
   - 由此得到一条**硬设计约束**：**WAL 的写入路径不得经过任何用户态缓冲**（`WritableFile::Append` 必须直接 `write(fd)` 到内核；`Flush()` 保持空操作）。M1 的 `PosixWritableFile` 已经满足这一点（`Append` 直接 `::write`），M2 必须**保持**它，且**禁止**为了性能在 WAL 前面加一层 `stdio`/自研 buffer。
3. `PARTIAL_HEADER`（剩余 < 7 字节）与 `PARTIAL_BODY`（头完整、payload 被截断）**两种形态都真实出现了**（分别是 `TAIL_LEN 1/2` 与 `TAIL_LEN 8/22`）⇒ §4.3 reader 的 (b)/(e) 两条分支都有实测依据，不是纸面设计。
4. **由此确定 M2 的两套崩溃模型，各自管一件事**：

   | 机制 | 能验证什么 | 不能验证什么 |
   |---|---|---|
   | `kill -9`（B 组） | **进程级一致性**：进程死掉后文件内容仍是完整 record 前缀；恢复能重放；`missing 0` 对账 | torn record（实测产生不了）；**掉电语义**（page cache 随进程死亡不消失，故 `sync=false` 也一条不丢） |
   | `MemEnv` 回滚到 fsync 水位 + 可配置撕裂（A 组 `CrashSim.*`） | **掉电语义**：`sync=false` 的可丢失后缀；torn record 的安全截断；`sync=true` 的必存活 | 真实内核/磁盘的行为 |
   | 真实文件逐字节 `truncate` 扫描（B03） | torn record 的**全部**可能形态都能安全截断（字节级真值） | — |

   **必须写进 `m2-prerequisites.md`**：不能因为"跑了 30 轮 `nosync` 且 `missing 0`"就宣称 `sync=false` 有 durable 保证——那是 §11.3 的 page cache 特性造成的假象。

**(d) 真实的截断扫描与损坏定位（同一探针，`truncate` + `dd` 注入）**

```
full file: SIZE 16514345 COMPLETE 718015 LAST_GOOD_END 16514345 TAIL_LEN 0 TAIL_CLASS CLEAN
truncate=22    -> COMPLETE 0 LAST_GOOD_END 0 TAIL_LEN 22 TAIL_CLASS PARTIAL_BODY   TAIL_HEX 9d2ee7d81000010000000000000000a5a5a5a5a5a5a5
truncate=23    -> COMPLETE 1 LAST_GOOD_END 23 TAIL_LEN 0 TAIL_CLASS CLEAN
truncate=24    -> COMPLETE 1 LAST_GOOD_END 23 TAIL_LEN 1 TAIL_CLASS PARTIAL_HEADER TAIL_HEX 8a
truncate=30    -> COMPLETE 1 LAST_GOOD_END 23 TAIL_LEN 7 TAIL_CLASS PARTIAL_BODY   TAIL_HEX 8a243d73100001
truncate=46    -> COMPLETE 2 LAST_GOOD_END 46 TAIL_LEN 0 TAIL_CLASS CLEAN
truncate=100   -> COMPLETE 4 LAST_GOOD_END 92 TAIL_LEN 8 TAIL_CLASS PARTIAL_BODY   TAIL_HEX 23eb577c10000104
truncate=7     -> COMPLETE 0 LAST_GOOD_END 0 TAIL_LEN 7 TAIL_CLASS PARTIAL_BODY   TAIL_HEX 9d2ee7d8100001
truncate=6     -> COMPLETE 0 LAST_GOOD_END 0 TAIL_LEN 6 TAIL_CLASS PARTIAL_HEADER TAIL_HEX 9d2ee7d81000
truncate=1     -> COMPLETE 0 LAST_GOOD_END 0 TAIL_LEN 1 TAIL_CLASS PARTIAL_HEADER TAIL_HEX 9d
truncate=0     -> SIZE 0 TAIL_CLASS CLEAN
# 非尾部（中间）损坏：
flip@55（第 3 条 record 的 payload 内 1 字节）-> COMPLETE 2 LAST_GOOD_END 46 TAIL_CLASS CRC_MISMATCH
   TAIL_HEX 424cbf8a1000010200ff0000000000a7a5a5a5a5a5a5a5554665211000010300
```

注意 `flip@55` 那一行：探针的 verifier 只做"停在第一个人为失败点"的简单扫描，它把中间损坏也报成 `CRC_MISMATCH`（因为它不 resync）。这**恰好证明了 §5.3 的 resync 扫描是必需的**：若 reader 不做 resync，第 3 条 record 的一字节损坏会被误判为"尾部残骸"而把后面 71.8 万条完好 record 全部截断——**这是最危险的静默丢数据路径**，也是本设计把"resync 扫描"列为**必做项**而不是优化项的原因。

---

## 12. 自检（占位符 / 内部矛盾 / 歧义 / 范围越界）

### 12.1 占位符

- 无 `TODO`/`TBD`/`XXX`/`...`（三处 `...` 均出现在**引用的原始输出**里，属原文照录）。
- 每个新增类型/常量/接口都有明确取值与语义：`kWALBlockSize=32768`、`kWALHeaderSize=7`、`kWALMaxPayload=32761`、
  `kMaxLogicalRecordSize=64 MiB`、`kMaxGroupBytes=1 MiB`、`kMaxGroupRecs=64`、`kMaxBatchCount=1<<20`、
  `kMaxTailCorruptWarnBytes=2*kWALBlockSize`、`kRecoverySlack`、`kMaxTailCorruptWindow`(未使用，已删除)。
- `Options` **不加**任何新字段；`Env` 只列 §5.7 的 5 个增补，每个都有落地位置与用例。

### 12.2 内部矛盾（逐条核对）

| 潜在矛盾 | 处置 |
|---|---|
| §1.2 说"M2 不引入 `Options::repair`"，§5.3 又说"自动截断" | **不矛盾**：`repair` 是针对**中间损坏**的自动修复开关；尾部截断不是"修复"而是"丢弃从未被 ack 的后缀"，且必须打日志（§5.2/§5.3） |
| §5.5 第一遍"不解析 batch、不算 CRC" vs "第一遍必须发现损坏" | **已在 §5.5 就地修正**：两遍都做完整校验，`Open` 成本 ≈ 2× 单遍扫描；并给出 M2.3 的优化方向与"先按两遍落地"的决定 |
| §4.3 reader 允许空 record vs §4.2 writer 禁止 `length==0` | **不矛盾**：`length==0` 的 record 在 writer 侧被禁止（§4.2 的 `end` 判据），reader 侧的 `length==0` 一律判损坏（§4.3 (d)）；`crc==0&&len==0&&type==0` 只作 padding 哨兵 |
| D3"合并成单条 record" vs §4.7"batch 头 `count` 上限 1<<20" | **不矛盾**：`count` 上限是**解析侧防御**，`kMaxGroupRecs=64` 是**合并侧上限**；两者量级不同且都必要 |
| §6.3 的 `for (Writer* r : group_members_)` 与取批时 `pop_front` | **已就地修正**：取批时把成员记入 `group_members_`，回锁后按该向量结算并清空 |
| §6.2 说"`mem_->Add` 在 `Append` 之前" vs §6.3 同 | **一致**（同一顺序、同一理由：防止被拒绝的写复活） |
| D12 说"恢复期 MemTable 容量 = max(write_buffer_size, WAL 字节数)" vs §1.2 边界 1"M2 保留 kFrozen" | **不矛盾且必要**：写路径仍可能 `kFrozen`（用户配了很小的 `write_buffer_size`），恢复路径则**必须成功**，故用实测字节数放大容量；两条各自有独立理由 |
| D7"跳过 sequence 回退" vs I13"重放顺序必须严格递增" | **已在 D7/§5.4 明确**：跳过是幂等机制；若真的发生乱序，`Recovery.*` 会通过"跳过计数"上报（**需拍板**：是否改为报 `kCorruption`） |

### 12.3 歧义（逐条消解）

| 歧义点 | 本设计的唯一解释 |
|---|---|
| "CRC 覆盖 header + payload" | 精确化为"覆盖 `length(2) || type(1) || payload`"，即 **CRC 字段之后的全部字节**（§4.4） |
| "尾部损坏" | 精确化为"`TAIL_RESIDUE`"或"`PARSE_FAIL` 且 `ResyncScan == -1`"（§5.3 的判定表） |
| "中间损坏" | 精确化为"`PARSE_FAIL` 且 `ResyncScan >= 0`"（§5.3） |
| "一批" | 精确化为"从队首起连续、受 `kMaxGroupBytes`/`kMaxGroupRecs` 约束、且至少含队首自己的一组写者"，且**合并为一条 WAL record**（D3） |
| "已 ack" | 精确化为"`Put`/`Delete` 的返回值 `.ok() == true`"（§7.1/§8.1） |
| "窗口放开" | 精确化为"`flusher_active_` 从 `true` 变为 `false` 的那一个临界区内的最后两步"（D4/§6.3） |
| "sync=false 允许丢最近的写" | 精确化为"允许丢失 WAL 中**未 fsync 的连续后缀**"，且必须满足 §7.3 三条硬约束 |
| "尾延迟/批上限" | `kMaxGroupBytes=1 MiB`、`kMaxGroupRecs=64`，且"单个超大写者独占一批" |
| I13 的"严格大于" | 消解为"下一次写入分配的 sequence 严格大于任何已重放 record 的 sequence"（D7，**需拍板**） |

### 12.4 范围越界（是否偷偷带了 M3+ 内容）

逐条对照 §1.2 的禁列，本设计**出现**下列**接近但不同**的项，全部有明确边界，**不构成 M3 提前实现**：

| 出现的东西 | 为什么不是越界 |
|---|---|
| WAL batch payload 布局与 M5 的 `WriteBatch` 同构 | 指令 §0 明确"仅允许 WAL 内部按 batch 组织一次写"；**不提供任何公共 API/类/头文件**；M5 只加公共封装而不改格式，这是**减少** M5 工作量的格式选择，不是提前实现 |
| `Env::NewAppendableFile` / `GetChildren` / `TruncateFile` / `SyncDir` | 恢复路径的**必要**能力（枚举 log、追加、截断尾部）；不含 `RandomAccessFile`（M3） |
| "WAL 文件删除的接口与判据" | 指令 §0 明确要求 M2"只定义接口与判据，不实现"；本设计给出判据 + **一个断言不实现的空实现体**（§3.3），且判据说清"M2 不存在可删除条件" |
| `GetRecoveryStats()` 诊断接口（截断字节数/重放条数/`OPEN_MS`） | 门禁可观测性的最低要求（§9.3 的输出格式需要它）；不涉及 SSTable/compaction 的任何统计 |
| 组提交的"批" | 指令 §0 目标 4 明确要求；不是 WriteBatch API |
| `Close()`/`Sync()` | 指令 §0 目标 3 + I20 要求 |
| 恢复期按 WAL 字节数放大 MemTable 容量 | M2 无 flush 的必要补救，**恰好是 M3 会删掉的一行**（M3 起恢复完直接 flush） |
| `kMaxLogicalRecordSize = 64 MiB` | reader 的防越界上限，与 SSTable/块缓存无关 |

**明确声明：本设计不含** SSTable / flush / compaction / Bloom / 块缓存 / 压缩 / 快照读视图 / `MANIFEST`/`CURRENT` / raft-kv 对接 / WAL 文件删除的**实现**。评审可逐条对照 §1.2 的禁列。

### 12.5 本设计的已知薄弱点（主动暴露，供 #4 评审攻击）

1. **resync 扫描的"连续损坏到文件尾"误判**（§5.3 末）：不可消除，只能靠恢复报告与告警缓解。
2. **`fsync` 在该 VM 上是否真的落盘不可验证**（§11.2）：`kill -9` 无法区分"durable"与"在 page cache 里"。三方独立实测一致给出 ≈2.3~2.9 ms（而非指令写的 8 ms），进一步支持"宿主缓存承接了 fsync"的怀疑。这是环境的限制，不是设计的缺陷，但**必须让读者知道 M2 的 `missing 0` 证据强度到哪里为止**。
3. **两遍扫描让 `Open` 成本翻倍**（§5.5）：M2 有意接受，M3 缓解。
4. **M2.2 的串行写路径**（每写一次 fsync）在 100 轮门禁下会很慢（每轮 ~200 写 × 2.8 ms ≈ 0.6 s，加 sidecar fsync ≈ 1.2 s/轮 ⇒ 100 轮 ≈ 2 分钟）：可接受，但要事先说明，否则会被误判为"卡住"。
5. **`group_members_` 复用成员变量**避免每次分配，但它使"取批"与"结算"必须在同一个 `DBImpl` 实例上串行——由 `flusher_active_` 保证，但这是个隐性耦合，需在代码注释里写明。
6. **D10（LOCK 文件）是本设计唯一的"超出指令明确要求"的建议**，需用户裁决。
7. **自定义 comparator 跨重启不一致**（D13）：登记为限制，不解决。

### 12.6 需用户拍板清单（汇总）

| # | 问题 | 本文的推荐 | 影响面 |
|---|---|---|---|
| Q1 | D1 record 格式选 A（LevelDB 风格 32 KiB 块）还是 B（单条变长）？ | **A** | WAL 实现量、M3 复用度 |
| Q2 | D2 CRC 是否比 LevelDB 多覆盖 `length` 两个字节？ | **是**（覆盖） | `protocol.md` 的 CRC 契约 |
| Q3 | D3 组提交是否把整批**合并成一条 record**？ | **是** | `protocol.md` 的 payload 契约 |
| Q4 | D4 窗口放开时机按"最后一步"（P2a 口径）？ | **是** | 吞吐回归 A24 |
| Q5 | D5 `WriteOptions::sync` 默认 `false` 还是 `true`？ | **`false`** | 默认吞吐 vs 默认安全 |
| Q6 | D6 尾部自动截断（不引入 `Options::repair`）？ | **是** | 数据安全口径 |
| Q7 | D7 恢复遇 `sequence` 回退是"跳过+计数"还是"报 `kCorruption`"？且 **I13 措辞**（"严格大于"）按"下一次分配严格大于"落地？ | **跳过+计数；按后者落地** | `#1` 的 I13 与用例断言 |
| Q8 | D8 sidecar 是否逐行 `fsync`（影响 100 轮墙钟 ≈ 2 分钟）？`sync=false` 的判据是否用"前缀末端"？ | **逐行 fsync；用前缀末端** | 门禁可信度 vs 测试时长 |
| Q9 | D9 `Open` 复用最高编号 log（文件数恒为 1）还是每次新建？ | **复用** | 与 M3 的行为差异 |
| Q10 | D10 是否把 `LOCK` 文件独占纳入 M2？ | **纳入** | 唯一的超范围建议 |
| Q11 | D11 `fsync`/短写失败后 DB 转写只读（fail-stop）？ | **是** | 可用性 vs 一致性 |
| Q12 | D12 恢复期按 WAL 实测字节数放大 MemTable 容量（两遍扫描）？ | **是** | `Open` 耗时 ×2 |
| Q13 | §11.2 的"8 ms 不可复现"如何处置——M2 微基准是否作为新的分母入档，M5 是否必须重测？ | **M2 微基准入档；M5 必须重测，不继承 8 ms**（三方独立实测一致 ≈2.3~2.9 ms） | M5 的性能口径 |

---

## 13. 参考与引用

| 出处 | 用途 |
|---|---|
| `docs/m1-design.md` §4/§6/§8/§10/§12 | M1 冻结接口、Arena/MemTable 口径、Env 能力边界、子里程碑拆分范式 |
| `docs/m1-prerequisites.md` §1/§2/§6/§7 | I1~I10、L1~L6、未定义行为清单、测试前置假设（M2 必须继续成立） |
| `docs/m1-review.md` §1/§2/§4 | 两个阻断项的教训（解码前长度校验、比较器等价判定）→ §4.3 的 (d) 与 §5.4 的排序论证 |
| `docs/protocol.md` §1~§8 | 复用 §1（LE）、§2/§4（varint/length-prefix）、§5（CRC32C 与 `Extend`）、§6（内部 key 降序）；§4.6 给出追加 §9 的 patch |
| `docs/roadmap.md` §0/§2/§3 | 单向分层、阶段硬边界、工程约定（未跑不算过、三构建目录、0 warning、负结果入档） |
| `M2-WAL与崩溃恢复.md` §0/§1/§3 | 本阶段任务书（6 目标 + 硬约束 + 8 个开放决策 + M2.1~M2.3 拆分） |
| raft-kv `docs/m5-design.md` §6 | 两段式持久化协议（锁内只写内存、锁外 fsync、回锁校验）的具体形态 |
| raft-kv `docs/m5-review.md` §3 / `docs/m5-design.md` v2.10 | 组提交"丢唤醒"与"窗口放开过早"（P2a：0.47× → 1.34×）的复盘 → §6.3 的四场景分析与 D4 |
| raft-kv `scripts/fsbench_commit_latency.cpp` | 微基准口径（同轮同机、固定输出、median/p90）；本设计 §11.2 用它做了独立交叉核对 |
| raft-kv `docs/m5-bench.md` §3.6/§3.7 | "绝对判据物理不可达时先测下限、再改同机比值口径"的方法论 → §9.3 的输出格式 |
