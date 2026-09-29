# M2 独立评审与处置（docs/m2-review.md）

> 评审对象：`~/lsm-kv`（本机克隆 `D:\JLProject\lsm-kv`），HEAD `d1afcbb`（评审期间 VM 仅多一个 docs-only 提交 `67de95e`）。
> 方式：**独立 subagent**（与作者不同上下文），任务书含 M2 指令 §4 的 9 条评审重点 + 作者自曝的 5 个风险点；
> 评审者自带对抗性探针（VM `/tmp/m2rev/`，**未改动仓库任何文件**，`git status` 干净）。
> 本文件由作者转写评审结论 + 记录处置；阻断项的原始复现输出照录。

## 1. 阻断项与处置（6 条，全部已修 + 补回归）

| # | 问题 | 复现证据（评审原文） | 处置 |
|---|---|---|---|
| **1** | `RunFlusher` 取批只查 `IsFrozen()`，而 `MemTable::Add` 判据是 `IsFrozen() \|\| 用量 >= 上限` ⇒ 刚好触顶的批走接受路径（WAL 落盘 + 推进 sequence），随后 Add 返回 `kFrozen`：① 被拒写重启后**复活**；② 同配置重开 `Open` 返回 **Corruption** | 4 MiB 档：`PUT_FAILED_AT=28340 status=Frozen`、`PUTS_OK=28339`；WAL 逐字节扫描 `RECORDS(full) 28307 TYPES {1:28307,2:33,4:33}` ⇒ **28340** 条 record（ack 只有 28339）；`./r_cap d2 0 4194304` ⇒ `OPEN rc=Corruption: 重放 Add 失败: Frozen`；`r_get` 里 `GET k00028340 -> FOUND`（这条 Put 返回的是 kFrozen） | `930f877`：容量判据收敛为**单一真相源** `MemTable::WouldReject(extra)`（Add 自身也改用它），取批时按**整批预估占用**（编码 + 128 B/条节点）预留校验；判不过**整批拒绝、不 Append、不推进 sequence**。回归 `DB.PutAfterFreezeIsNotPersisted`（默认 4 MiB 档；`22efb52`） |
| **2** | `durable_seq_` **无条件发布**（未 fsync 也前进）⇒ design §7.1 论证 I11 的结构证据链断裂 | `r_durable`：`AFTER_8_UNSYNCED_PUTS durable_seq=8`，而 `synced_size=0 sync_calls=0`；`SimulateCrash` 后 `present=0/8` | `930f877`：`if (need_sync) durable_seq_ = ...`。回归 `GroupCommit.DurableSeqOnlyAdvancesOnSync`（`22efb52`） |
| **3** | **崩溃对账门禁"空绿"**：`RC!=0` 未 `exit 1`；`MISSING_TOTAL=$((... + M))` 在 `M` 空时是算术错误被静默跳过 ⇒ 计数恒 0 ⇒ 工具每轮崩溃也打印 `[OK]` 并 `exit 0`（`lsm_gate.sh` 因此也绿） | 桩工具（writer=sleep、recover=`exit 139`）×3：`ROUND n ... RC 139` / `TOTAL_ROUNDS 3 MISSING_TOTAL 0` / `[OK] 全部 3 轮 missing 0` / `SCRIPT_EXIT=0` | `6f6ff2d`：RC≠0 或缺 ROUND 行即 `exit 1`；新增 `ROUNDS_OK`/`ACKED_TOTAL` 校验。**双向验证**：桩工具 ⇒ `ROUND_FAIL 1 RC 139` + `STUB_EXIT=1`；真工具 30 轮 ⇒ `ROUNDS_OK 30 ACKED_TOTAL 880 MISSING_TOTAL 0` + `[OK]` |
| **4** | **D10 LOCK 完全未接线**（用户已裁决纳入，却零调用点）⇒ 双进程可同时 Open 同一目录 | `r_lock`：`pid=45667 OPEN=OK` / `pid=45668 OPEN=OK`，目录里无 LOCK 文件 | `6f6ff2d`：`RecoverAndOpen` 取 LOCK（RAII 守卫保证提前返回也释放）→ 所有权交 DB → `Close()` 释放（含幂等路径）。双进程实测：第二个进程 `IOError: LockFile: already held by another process: .../LOCK: Resource temporarily unavailable` |
| **5** | 缺 A22/A25/A26 三条强制用例 + 四项冻结交付物（`GetRecoveryStats`/`Env::SyncDir`/`MaybeDeleteObsoleteLogs`/截断 WARN）；`Truncate` 后未 fsync（§5.2 要求） | 静态核对 + `--gtest_list_tests` grep `mixed\|zeroio\|lockorder\|spy\|muheld` 零命中 | 部分修复：`Truncate` 后补 `ReopenAndSync`（`a4c77c0`）；A22 的核心（durable 水位只在 fsync 后前进）已由 `GroupCommit.DurableSeqOnlyAdvancesOnSync` 覆盖；**A22 的"混合同批"形态、A25、A26 与四项交付物仍未做**（见 §3 残留清单） |
| **6** | `Close()` 只等 `!flusher_active_`，设计要求 `&& queue_.empty()` ⇒ 批边界之外被留在队列的写者在 `Close` 返回后仍可能进 `RunFlusher`（与 `Close(); delete db;` 组合是 UAF 窗口）【评审标注：静态推断，未复现】 | 需要 ≥65 写者或 >1 MiB 组才走到，现有用例（4/32/64 写者）覆盖不到 | `930f877`：谓词改为 `!flusher_active_ && queue_.empty()`。回归 `Close.WaitsForDrainedQueue`（100 写者 > kMaxGroupRecs；`22efb52`） |

## 2. 优化建议处置

| 建议 | 处置 |
|---|---|
| **D12 容量公式漏算 MemTable 每条目开销**（实测：64 MiB 写满 → 1 MiB 重开 ⇒ Corruption `cap=7648576`） | **已修**（`22efb52`）：扫描阶段顺带解析 batch 统计条目数（顺带提前判畸形 batch），`cap = max(wbs, payload×2 + entries×2×128 + 1 MiB)` |
| **超大 value 触发粘性写只读**（`PUT 65MiB -> InvalidArgument` 之后写与 Close 全部返回同一错误） | **已修**（`a4c77c0`）：`Write` 入口按同一上限拒绝且**不入队** |
| §4.3 文档把"in_frag 时文件结束"列为 PARSE_FAIL，实现返回 `kTailResidue`（两者在 §5.3 下结果等价，实现是对的） | **接受待改文档**（未做；登记于此） |
| resync 探针未检查"候选不得跨块边界"，另有 4096 候选/8 MiB 上限；放宽方向保守 | **接受不改**（登记）；截断量 WARN 未做（属 §3 残留） |
| `seq <= last 则跳过` 静默（§5.4 承诺"跳过 + 计数上报"） | **接受待补计数**（依赖 `GetRecoveryStats`，未做） |
| 持 `mutex_` 做 ≤1 MiB 的 `EncodeGroup` 拷贝（阻塞 Get/NewIterator） | **接受待优化**（未做） |
| `:271` 对空 deque 调 `front()` 的防御（当前不可达） | **接受待补**（未做） |
| 测试缺口：A21 只用 32 写者（< 64）；1 MiB value 走 `DB::Put` 组批上限无用例 | **部分覆盖**：新增 100 写者用例（`Close.WaitsForDrainedQueue`）；1 MiB 组批用例未做 |
| 内存模式新增"Close 后拒写"属未登记行为变化（`src/db.cpp`） | **接受待登记**（未做） |
| 微基准 `fs see below` 与 §9.3 单行格式不符 | **接受待改**（未做） |

## 3. 残留清单（M2 收口后仍存在，**不得当作已完成**）

1. **A22 的"混合同批"形态**（`GroupCommit.MixedSyncPropagates`：同批混入 sync=true/false ⇒ 该批必须 fsync，且 sync=true 者返回后 `durable_seq_ >= 其 end_seq`）。当前只有"未 fsync 不推进水位"的部分覆盖（`DurableSeqOnlyAdvancesOnSync`）。
2. **A25**（`Locks.ZeroIoWhileHoldingDbMutex`）：I17 目前只有**代码评审级**证据（4 处双锁点、IO 全在锁外），缺探针式验证。需要 `SpyLogWriter` + `MuHeldGuard` 线程局部标记（约 80 行 src 仪表 + 用例）。
3. **A26**（`Locks.LockOrderIsQueueThenDb`）：由评审逐处核对 + TSan 覆盖，无独立用例。
4. **四项交付物**：`GetRecoveryStats()`、`Env::SyncDir`、`MaybeDeleteObsoleteLogs`（只声明 + 断言不实现）、截断量超阈值的 WARN。
5. §2 表中标"待…"的优化项。

## 4. 评审者逐条结论（原文摘要）

1. **存量改动最小化**：通过（内部 key 编码/比较、MemTable 容量与冻结语义零改动；M1 47 例全绿；唯一改动的 M1 断言已在 §9 登记）——内存模式"Close 后拒写"属未登记变化。
2. **durable 语义**：有疑虑（→ 阻断项 2 已修）。
3. **锁纪律**：有疑虑（锁序 4 处一致、无反向嵌套、`mutex_` 下零 IO、谓词不丢唤醒、D4 窗口最后一步放开 → 阻断项 6 已修；仍缺 A25 探针）。
4. **崩溃一致性**：有疑虑（→ 阻断项 1 已修；截断 fsync 已补）。
5. **错误处理与资源安全**：通过（`Pending*` 生命周期安全；失败路径均推进/放锁/唤醒；粘性错误与 `closed_` 防段错误；RAII 齐全）。
6. **恢复算法正确性**：通过（编号升序 + 文件内字节序、`last_sequence_ = max(seq+count-1)`、tombstone 重放、批原子性由单 record 保证；A12 注释"两种顺序都会收敛"不准确——**作者注：该注释已按评审意见修正为"字符串序会让 a/b 丢失、用例会红"**）。
7. **Env 抽象与测试可信度**：有疑虑（MemEnv 忠实实现追加/fsync 水位/固定种子撕裂；但 `Truncate` 恒 durable、`FileExists` 不认 `dirs_`、`GetChildren` 对不存在目录返回 OK ⇒ 截断持久性与 ENOENT 路径不可用 MemEnv 验证；崩溃脚本属单侧证据）。
8. **CMake 与脚本**：有疑虑（→ 阻断项 3 已修）。
9. **边界 case**：通过（空 WAL/缺目录、跨块 32761/32762/2×32761、padding 32761..32767、1 MiB value、64 MiB 上限、ENOSPC 注入、组批上限——唯一遗患"填满 MemTable"即阻断项 1，已修并有回归）。

**评审者未跑**：`lsm_build.sh` 干净重建（避免动 VM 三套构建目录）、100 轮 kill -9（作者已在修复后复跑，见 §5）。

## 5. 修复后的完整门禁（原始输出见 `docs/m2-evidence.md` 与本仓库提交信息）

```
scripts/lsm_gate.sh --rounds 100   ->  5/5 PASS
  PASS  clean rebuild + 0 warning + full suite (79/79)
  PASS  ASan full suite
  PASS  crash reconciliation kill -9 x 100 (sync mode)
  PASS  byte-by-byte truncation scan (B03, 1401/1401)
  PASS  middle corruption refused (B04)
  [OK] all gates passed
TSan 全量（修复后）-> 见 docs/m2-evidence.md 的收口小节
```
