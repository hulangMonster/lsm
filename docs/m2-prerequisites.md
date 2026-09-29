# M2 实现前置校验（docs/m2-prerequisites.md）

> `#1` 阶段产物。**本文件不写实现代码**；它把 `docs/m2-design.md`（e24df25 + ed8ce47 修订）与 `docs/protocol.md`
> 冻结成一组可校验的不变量（I11~I20）、锁纪律（L7~L12）、风险、文件清单、边界全集、未定义行为清单与验收命令。
>
> 流程锁：`#1` 阶段发现缺陷必须回退 `#0` 修改设计，**禁止在校验阶段私自改实现方案**。

## 0. 校验范围与结论

- 校验对象：`docs/m2-design.md`（§1~§12，1373 行）、`docs/protocol.md`（M1 冻结，M2 只允许**追加** §9 WAL 章节）。
- 结论：设计已覆盖 M2 指令的 8 条开放决策与全部硬判据（§9 测试矩阵 A01~A31 / B01~B07 逐条可执行）。
- 本阶段发现 **2 处判据与「A 组零 flaky」自相矛盾**，已回退 `#0` 修订（提交 `ed8ce47`），见 §9。
- **三条环境事实**（实测，决定验收方式，见 `docs/m2-design.md` §11 与本文件 §3）：
  1. commit 级 `fsync` 实测 **p50 2.556 ms / mean 2.611 ms**（三方独立一致）；指令写的「约 8 ms」**不成立**（约 1/3）。
     ⇒ 组提交收益用 2.6 ms 作分母；M5 必须重测，不得继承 8 ms。
  2. **`kill -9` 产生不了 torn record**：裸 `write()` 逐条 100 轮 `TAIL_WHOLE_RECORDS 100`；带 4 KiB stdio 缓冲 100 轮 `TORN 95`。
     ⇒ ① WAL 路径**禁止任何用户态缓冲**；② **不得由「nosync 轮 missing 0」宣称 `sync=false` 有持久性**（page cache 不随进程死亡消失）；
     ③ 掉电语义只能靠 MemEnv 回滚注入，torn 形态全集只能靠真实文件逐字节 truncate 扫描。
  3. **resync 扫描是必做项**：不做 resync 会把「第 3 条坏一字节」误判为尾部、截掉后面 71.8 万条完好 record（静默丢数据，比拒绝启动危险）。

## 1. 不变量 I11~I20 —— 「谁保证 + 怎么验」

| # | 不变量 | 谁保证（代码位置） | 怎么验（用例） |
|---|---|---|---|
| I11 | durable-before-ack：`sync=true` 返回 `kOk` 前该记录已 `write` 且 `fsync` 成功（**双检查**） | §6.4 双检查 + §7.1 三段式论证（`Append` 字节数 / `Sync` 返回值 / 同一临界区发布 `durable_seq_`） | A10、A22、A29、A30；B01 |
| I12 | WAL 是未刷盘数据的唯一真相源：M2 恢复 = 全量重放 WAL | §5.1/§5.2 扫描**所有** `*.log`；§3.3 M2 任何 log 都不可删除 | A11、A12；B01 |
| I13 | 重放顺序 = 写入顺序 = sequence 升序；**恢复后 `last_sequence_ = max(replayed batch.sequence + count - 1)`，下一次写入分配的 sequence 严格大于任何已重放记录** | §5.2 按编号升序 + 字节顺序（**不重排序**）；§5.4 取 max；§6.2/§6.3 序列号分配在 `log_mu_`/`commit_mu_` 覆盖内 | A13、A19、A27 |
| I14 | 半条 record 永不生效：不完整则截断到最后一条完整 record，被截断部分不得对查询可见 | §4.3 `TAIL_RESIDUE`；§5.3 判定表只截断到 `last_good_end` | A06、A08、A15；B03 |
| I15 | 组提交的原子单位是「批」：批内全 durable 或全不 durable，等待者不得看到部分成功 | §4.7 **一条 record = 一个原子批**（一个 CRC）；§6.3 `need_sync` 取组内 OR、`w.done` 只在整批结算后置位 | A20、A22、A23 |
| I16 | `fsync` 失败必须传播给该批**所有**等待者，不得静默返回 `kOk`；失败后 WAL 状态明确 | §6.4 粘性 `commit_error_` + `bg_error_`；D11 fail-stop（转写只读） | A10、A23、`GroupCommit.WriteAfterErrorIsRejected` |
| I17 | 持锁零 IO：WAL 的 `write`/`fsync` 一律在 DB 互斥锁之外 | §6.2/§6.3 的解锁点 + L7 | A25（`MuHeldGuard` + `SpyLogWriter` 断言持锁期 IO 调用数 == 0） |
| I18 | 恢复只读：不写 WAL、不触发 flush、不修改既有文件（**唯一例外**是最高编号 log 尾部的 `ftruncate`，有日志有判据） | §5.5；§1.2 M2 无 flush | A14（幂等：WAL sha1 不变）、A18 |
| I19 | 无撕裂值：任一 key 只能是某个**完整版本**，不得出现半写 value | §7.3 第 1 条（value 只存在于完整 record 内）；§4.3 截断到 record 边界 | A15、A27 |
| I20 | 关闭语义：`Close()`/析构保证此前全部已 ack 数据 durable；关闭后无后台线程访问已释放对象 | §6.5 拒绝新写 → 等在途批 → `Sync` → `Close`（顺序不可换）；M2 无后台线程 ⇒ 结构性成立 | A30、A31 |

**I13 的措辞澄清（设计门已确认）**：M2 指令原文「恢复后 `last_sequence` **严格大于**任何已重放记录的 sequence」与 M1 `DBImpl::Write` 的
`last_sequence_ + 1` 口径互斥。落地为：**恢复后 `last_sequence_ = max_replayed`；下一次写入分配的 sequence = `max_replayed + 1`**，
「严格大于」施加在**下一次分配值**上。测试按此断言（A13）。

## 2. 锁纪律 L7~L12

| # | 纪律 | 落地方式 | 验证 |
|---|---|---|---|
| L7 | DB 互斥锁只保护内存状态，不保护 IO | §6.1 状态划分 + §6.2/§6.3 解锁点 | A25；代码评审 |
| L8 | 组提交队列由独立互斥量保护；**锁序固定 `commit_mu_ → mutex_`，禁止反向** | §6.1 两把锁；§6.6 列出 M2 仅 3 处同时持两锁 | A26（TSan）+ 评审逐处核对 |
| L9 | 条件变量谓词必须覆盖「我已完成」与「**我能否接手当 flusher**」两个条件 | §6.3 谓词 `while (!w.done && !(!flusher_active_ && queue_.front() == &w))` + 四场景分析 | A21（`wait_for(200ms)` 超时即 FAIL——不是「慢」，是丢唤醒） |
| L10 | 交接必须在仍有未结算成员时通知；清 `flusher_active_` 与唤醒新队首的**顺序不可交换**，不得依赖下一次写入来唤醒 | §6.3 ②（两步顺序）；§6.5 防御性 `notify_all` | A21 |
| L11 | 关闭路径与并发写者交互：拒绝新写 → 等在途批 → 再销毁 | §6.5 | A31 |
| L12 | 恢复是单线程的；恢复期间不得有后台线程运行 | §5.2 `RecoverAndOpen` 不创建线程；§6.5 末 | `Recovery.*` 全为单线程 + 评审 |

## 3. 风险清单（风险 → 触发场景 → 检测 → 缓解）

| 风险 | 触发场景 | 检测手段 | 缓解 |
|---|---|---|---|
| **`kill -9` 证明不了持久性** | 拿 `--mode nosync` 的 `missing 0` 当成 `sync=false` 的 durable 证据 | `docs/m2-design.md` §11.3 的 100 轮探针（`TORN 0`） | 门禁只用 `--mode sync`；`nosync` 只断言「无半写/无乱序/无旧值覆盖」；掉电语义由 `CrashSim.*` 承担 |
| **中间损坏被误判为尾部 → 静默丢数据** | reader 停在第一个失败点就返回「尾部」 | `WAL.MiddleCorruptionRejected`（A07）+ `lsm_corrupt_middle_test.sh`（B04） | §5.3 的 resync 判定：失败点之后**存在完好 record ⇒ 中间损坏 ⇒ 拒绝启动** |
| **用户态缓冲制造真实撕裂** | WAL 走 `fwrite`/`iostream`/`std::string` 缓冲后再落盘 | §11.3 的 stdio 探针（`TORN 95`） | WAL 路径禁止任何用户态缓冲；A/B 组都覆盖单字节截断 |
| **fsync 成本被高估** | 沿用指令/roadmap 写的「约 8 ms」论证组提交收益 | §11.2 三方独立实测（2.2~3.1 ms） | 用实测 2.6 ms 作分母；M5 重测，不继承历史值 |
| **A20 赌调度** | 「64 写者 ⇒ fsync ≤ 8 次」在串行调度下必然失败 | 已回退 #0 修订（ed8ce47） | 改 `CommitHook` 确定性屏障；统计性判据降级为 A20b（不作门禁） |
| 双进程同时 `Open` 同一目录 | 崩溃脚本/人工误操作并发 | `DB.OpenHoldsExclusiveLock` | D10 的 `LOCK` 文件独占（`fcntl(F_SETLK)`，崩溃由内核释放） |
| 短写/`fsync` 失败后偏移不可信 | 失败后继续追加 | A09、A10、A23 | D11 fail-stop：粘性错误 + DB 转写只读；`Get`/迭代照常 |
| 组提交丢唤醒 / 通知丢失 | 谓词不完整、或交接时忘记 `notify_one` | A21（四场景精确时序） | L9/L10；谓词显式覆盖「我能接手当 flusher」 |
| 恢复期 `kFrozen` 导致恢复失败 | 100 MiB WAL + 4 MiB `write_buffer_size`（M2 无 flush） | `Recovery.*` + B01 | D12 两遍扫描，按 WAL 实测字节数放大 MemTable 容量 |
| 崩溃脚本自身不可靠 → 假 `missing` | sidecar 在 `Put` 之前记行、或 stdout 缓冲丢失 | 固定行格式 + 每轮 `ACKED/RECOVERED` 计数 | D8：sidecar 独立文件、**`Put` 返回 `kOk` 之后**才记行、逐行 fsync（可 `--ack-sync-every N` 放宽并显式标注） |
| 损坏注入把「连续损坏到文件尾」判成尾部 | 信息论限制 | B03/B04 | 恢复报告必须打印被截断字节数，超阈值 WARN（§5.3 已声明局限） |
| WAL 中 record 交错 | 并发写未串行化 | A26 + 评审 | 写入串行点 = 单 flusher 在 `commit_mu_` 临界区内 `Append` |
| `frag_buf` 被畸形 length 撑爆 | 手工构造的 WAL | A17 | §4.3 的 `kMaxLogicalRecordSize = 64 MiB` 上限 ⇒ `PARSE_FAIL` |
| ENOSPC / 只读文件系统未定义 | 真实磁盘满 | B06（需 root 挂 loop，**可能无法制造，则如实登记未验证**） | 由 A09/A23 的注入式覆盖承担判据；不破坏既有数据、返回明确 `Status` |

## 4. 文件清单

| 分类 | 文件 |
|---|---|
| **必须新增（实现）** | `src/wal.{h,cpp}`、`src/filename.{h,cpp}`、`src/db_impl.{h,cpp}` |
| **必须新增（测试）** | `tests/memenv.{h,cpp}`（内存 Env + 崩溃/撕裂/失败注入）、`tests/wal_test.cpp`、`tests/recovery_test.cpp`、`tests/crash_test.cpp` |
| **必须新增（脚本/基准）** | `scripts/lsm_crash_test.sh`、`scripts/fsbench_commit_latency.cpp`、`scripts/lsm_tail_truncate_test.sh`、`scripts/lsm_corrupt_middle_test.sh` |
| **必须新增（文档）** | `docs/m2-design.md`、`docs/m2-prerequisites.md`、`docs/m2-tdd-red.log`、`docs/m2-evidence.md`、`docs/m2-review.md` |
| **必须改** | `src/db.{h,cpp}`（`WriteOptions`/`Sync`/`Close`/`Open` 恢复分支）、`src/util/env.h` + `env_posix.cpp`（`NewAppendableFile`、`LockFile/UnlockFile`、`GetChildren`、`RemoveFile`、尾部 `Truncate` 能力）、`CMakeLists.txt`、`.gitignore`、`docs/protocol.md`（**只追加** §9 WAL 章节，patch 文本见 `m2-design.md` §4.6） |
| **禁止改动** | M1 的内部 key 编码与比较规则（`common.h` 的编解码/比较）、`src/skiplist.h`、`src/memtable.{h,cpp}` 的语义、**M1 既有测试的任何断言**、任何 SSTable/compaction/filter/WriteBatch 公共 API 符号、`~/raft-kv` 任何文件 |

**脚本裁决**：`scripts/lsm_tail_truncate_test.sh` 与 `scripts/lsm_corrupt_middle_test.sh` **各自独立成脚本**（而不是并入 `lsm_crash_test.sh`）。
理由：它们与 kill -9 驱动的对账协议**职责不同**（前者对真实 WAL 做逐字节截断扫描、不开子进程；后者对真实文件做定点字节翻转），
合并会让同一个脚本承担两种失败模式、失败定位变差；M2 指令的文件清单是「必须新增」的下限而非上限（M1 已有 `docs/m1-evidence.md`/`m1-review.md` 的先例）。

## 5. 边界 case 全集（输入/场景 → 期望 → 覆盖用例）

| 场景 | 期望 | 用例 |
|---|---|---|
| 空 WAL / 目录不存在（自动创建）/ 目录内只有非 `.log` 文件 | `kOk` 启动；`Get` 返回 `kNotFound` | A16 |
| payload = 1 / 32761 / 32762 / 2×32761 | 正确的 FULL / 单段 / 跨两块 / 跨三块 | A02 |
| `record.size() == 0` | 构造期拒绝（不写出 `length = 0` 的非法片段） | A01、A02 |
| 写入使 `block_offset` 落在 32762..32767 | 补零 padding；reader 跳过；`kZeroType` 不被当成 record | A03 |
| `crc`/`length`/`type`/payload 逐位翻转 | 均不得被当成有效 record（payload 翻转必须 CRC 失败） | A04 |
| `type ∉ {1..4}`、`length == 0`、`length > 32761` | 判损坏，**不得越界读**（length 合法性检查必须先于 CRC 计算） | A05、A17 |
| 对每个长度 `0..file_size` 逐字节截断 | 返回 `CLEAN` 或 `TAIL_RESIDUE`，`last_good_end` 必为 record 边界；任何长度都不 panic/越界/死循环 | A06；B03 |
| 尾部损坏（CRC 不符但其后无完好 record） | 截断 + `TRUNCATED_BYTES > 0` + 报告可见 | A08、A15 |
| 中间损坏（其后存在完好 record） | `kCorruption` 拒绝启动，信息含文件 + 偏移 + resync 位置 | A07；B04 |
| 非最高编号 log 的尾部残骸 | **拒绝启动**（不得当残骸截掉老数据） | A18 |
| `count == 0` / `count > kMaxBatchCount` / entry 解码后有剩余 / key 为空 / key > 64 KiB | `kCorruption`（不是 `kOk`、不崩） | A17 |
| 同 key 1000 个版本 | `Get` 返回最大 seq 的值；内部迭代 1000 条且 seq 严格降序 | A19 |
| `Close()` 期间并发 `Put` | 明确 `Status`；不 UAF、不死锁 | A31 |
| 注入短写（每次 1 字节 / 永久 0 / ENOSPC） | 前者仍成功且内容正确；后者 `kIOError` 且**绝不返回 `kOk`** | A09 |
| 注入 `fsync` 失败 | `Write` 返回同一错误，`durable_seq_` **不前进**，本批全部等待者都拿到非 `kOk` | A10、A23 |
| 混入 `sync=false` 与 `sync=true` 的批 | 该批必须 fsync；`sync=true` 者返回后 `durable_seq_ >= 其 end_seq` | A22 |
| 连续 50 轮「写→崩溃→恢复」 | 每轮满足 A27 三条 + `last_sequence_` 单调不减 + 无泄漏/无 fd 泄漏 | A28 |
| 恢复中途再次崩溃 | 幂等：再次 `Open` 结果一致，WAL sha1 不变 | A14、A28 |
| 多文件乱序编号（1/2/10） | 必须按**数值**升序重放（字符串序会是 10→1→2） | A12 |
| 1 MiB value 的写者 | 独占一批；WAL 侧内存 = 1 MiB + 15 字节；不饿死其它写者 | A02、A20b |
| 真实 ENOSPC / 只读目录 | 明确 `Status`、不破坏既有数据、不 panic（**若无法制造真实场景则登记未验证**） | B06 |

## 6. 未定义行为清单（评审逐条禁止）

1. `reinterpret_cast` 到 `uint32_t*`/`uint16_t*` 读写 WAL 头（必须逐字节拼装/解析）。
2. 解码前不做 `length` 合法性检查就算 CRC（`length` 为畸形值时越界读——与 `m1-review.md` 阻断项 2 同源）。
3. `size_t` 下溢（`size() - kHeaderSize` 类运算必须在前置条件成立后才做）。
4. **WAL 路径出现任何用户态缓冲**（`fwrite`/`iostream`/先攒 `std::string` 再一次性 `write` 以外的缓冲策略）。
5. 持 DB 互斥锁做 `write`/`fsync`（I17/L7）。
6. 忽略 `write` 返回值或短写后不处理；忽略 `fsync` 返回值。
7. 恢复期写 WAL（唯一例外：最高编号 log 尾部 `ftruncate`）。
8. 两个进程同时 `Open` 同一目录（D10 由 `LOCK` 拒绝；绕过即 UB）。
9. 跨重启更换 `Options::comparator`（D13 已登记为限制；M3 由 MANIFEST 的 `Comparator::Name()` 校验）。
10. `frag_buf` 无上限增长（必须受 `kMaxLogicalRecordSize = 64 MiB` 约束）。
11. 迭代器/`Slice` 跨定位调用保留 `key()` 的返回值（M1 §4.4 已冻结：必须 `ToString()`）。

## 7. 测试前置假设（测试必须显式依赖的假设）

1. **`MemEnv` 忠实模拟三件事**：追加语义、`fsync` 水位、崩溃 = 回滚到 `last_synced_offset`（并可按固定种子概率撕裂最后一块）。这是 M2 **唯一**能确定性验证掉电语义的机制。
2. **`kill -9` 不丢 page cache**（§11.3 实测）⇒ `--mode nosync` 轮**不构成** durable 证据（只验证无半写/无乱序/无旧值覆盖）。
3. **A 组凡涉及随机的用例必须固定种子**并把种子打进 INFO/`RecordProperty`（失败可复现）。
4. **A20 用 `CommitHook` 确定性屏障**；真实无屏障并发只作 A20b 的统计（不作硬门禁）。
5. B 组用真实临时目录 + `kill -9`；**对账必须在恢复进程内完成**（因为 `Open` 会截断尾部，无法事后单独扫 WAL 对账）。
6. sidecar 记录**先 `Put` 拿到 `kOk`**、后追加行；`--ack-sync-every N` 放宽时输出必须显式标注「本次对账强度被放宽」。
7. M2 的 `Get` 只查 MemTable 即代表全量数据（无 SSTable）；`Recovery.*` 的断言依赖此。
8. 时间：A 组用 FakeClock（不依赖真实时钟）；B 组真实时钟并记录 `OPEN_MS`。
9. 三门禁：ASan（含 LSan）与 TSan 使用独立目录；TSan 运行需 `setarch $(uname -m) -R`。

## 8. 验收命令清单（每一步必须附原始输出）

```bash
# 1) 干净重建 + 0 warning + 全量用例（含 M1 的 47 例不得回归）
bash scripts/lsm_build.sh
# 2) ASan（独立目录）
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests
# 3) TSan（独立目录；必须 setarch）
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j8 && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests
# 4) 崩溃对账门禁（唯一可宣称 durable 的模式）
bash scripts/lsm_crash_test.sh --rounds 100 --mode sync   # 期望 TOTAL_ROUNDS 100 MISSING_TOTAL 0，退出码 0
# 5) 逐字节截断扫描 / 中间损坏 / 恢复代价
bash scripts/lsm_tail_truncate_test.sh
bash scripts/lsm_corrupt_middle_test.sh
bash scripts/lsm_crash_test.sh --rounds 30 --mode nosync  # 只断言无半写/无乱序/无旧值覆盖
# 6) 微基准（固定输出格式，供 M5 引用）
./build/bin/fsbench_commit_latency /tmp/fsb 500
```

## 9. `#1` 阶段发现的设计缺陷（已回退 `#0`，提交 `ed8ce47`）

| # | 设计原文 | 为什么与「零 flaky」冲突 | 修订 |
|---|---|---|---|
| A20 | 「64 个并发写者，`Sync` 调用次数 `<= 8`」 | 串行调度下每人自成一批 ⇒ 64 次 fsync ⇒ 假失败（赌调度） | 改为 `CommitHook` 确定性屏障（断言本批含 64 个写者且 fsync 次数 == 1）；真实并发统计降级为 A20b（不作门禁） |
| A27/A28 | 「随机写 → 随机崩溃 → 随机撕裂长度」 | 未固定种子 ⇒ 失败不可复现 | 必须固定种子并打进 INFO；撕裂长度只能来自该种子驱动的 PRNG |
| §4.2 writer | padding 条件写 `(kWALBlockSize - block_offset_) < kWALHeaderSize` | `block_offset_ = 32761`（剩余正好 7）时不补 padding，`avail = 0` ⇒ 写出 `length = 0` 的片段；而 §4.3 的 reader 规定 `len == 0 ⇒ PARSE_FAIL`，**writer 能写出自己读不回来的文件** | 改为 `<= kWALHeaderSize`；A03 的 `block_offset` 范围由 `32762..32767` 扩到 **`32761..32767`**（原范围恰好吃掉这个边界） |
| A07/A08 | 判据要求 WAL 层就给出「尾部截断 vs 中间损坏」 | §5.3 的重同步判定属恢复层；WAL 层只能提供事实 | WAL 层返回 `verdict == kParseFail` + `valid_record_after_failure`（其后是否存在完好 record）+ `last_good_end`；§5.3 的分类由 M2.2 的 `Recovery.*` 施加 |
| 契约 | §5.1 让 `Open(options, name非空)` = 持久模式，而 M1 的 `DB.OpenRejectsNonEmptyName` 断言的是 `kNotSupported` | 两者不可同时成立（M2 指令自己写了「Open 的恢复分支」） | **该断言随契约更新**（改名为 `DB.OpenPersistentMode`，并补上「重启后从 WAL 恢复」）；§4 的「禁止改动 M1 既有断言」按此**收窄为**「禁止改动与 M2 契约变化无关的 M1 断言」 |
| 实测口径 | §9.1 的 A09「write 每次只写 1 字节 ⇒ Append 仍成功」需要 WAL 层知道写入字节数 | M1 冻结的 `WritableFile::Append` 只返回 `Status`，调用方拿不到字节数 | M2.1 用「信封式 FaultyEnv 把写切成 N 字节块」实现该注入（数据仍完整），并在测试注释里写明这一点 |
| **观察项（待 #4 评审裁决）** | D7「重放时 `batch.sequence <= last_sequence_` 则跳过」把「sequence 回退」**静默**吞掉 | 写 A12 时实测到：只要文件编号与 sequence 反序，数据会被无声丢弃且 `Open` 返回 `kOk` —— 这正是"写入串行化被破坏"的 bug 该被暴露的场景（§5.2 自己也写了"不得重排序，乱序应报 kCorruption 暴露"） | 本阶段**按设计实现**（静默跳过，不算偏离），但登记为观察项：建议 M2.3 或 #4 评审时改为「跳过并打 WARN」或「判 kCorruption」，二选一须与 D7 一起拍板 |

### 9.1 `#4` 评审后补丁阶段发现的实现缺陷（M2-I32 / M2-I35，均已修复，附原始证据）

| # | 现象 | 为什么是真的（原始证据） | 修法 | 回归用例 |
|---|---|---|---|---|
| **M2-I32** | `Sync()` 把 durable 水位发布到 `last_sequence_`，而该值在 `RunFlusher()` **锁内取批时**（`db_impl.cpp:393`）就推进，`log_->Append` 却是在**锁外**做的（`:411`） | 用 `CommitHook::OnGroupTaken`（批次已取、sequence 已推进、Append 尚未执行）把窗口确定化，主线程在该点调用 `DB::Sync()`：RED 原始输出 `Expected: (early_durable) < (late_durable), actual: 1 vs 1` ⇒ `Sync()` 返回 `kOk` 却声称**尚未落盘**的批次已 durable，违反 `db.h` 的「返回即 durable」契约 | 新增 `appended_seq_`（`Append` 成功后推进）；`Sync()` **先**快照该边界、**再** fsync，按 `max()` 单调发布；`RecoverAndOpen()` 以同一边界起始 | `GroupCommit.SyncDoesNotClaimInFlightBatch`（RED 输出存档 `docs/m2-tdd-red-i32.log`） |
| **M2-I35** | `bg_error_` 的读（`Write` / `Sync` / `Close`）只在 `commit_mu_` 下进行，而写（`RunFlusher`）持 `mutex_` | TSan 原始报告（栈见 `docs/m2-evidence.md` §I35）：写栈 `Status::operator=` @ `db_impl.cpp:430`（持 `mutex_`），读栈 `Status::ok()` @ `db_impl.cpp:283`（持另一把锁）⇒ `Status` 的 code 与 message 可能来自不同错误，最坏情况漏掉粘性 fail-stop（把已损坏的库当成可写） | 三处读统一改为持 `mutex_`（`db_impl.h` 原本就声明 `mutex_` 保护 `bg_error_`，属**锁纪律违反**而非设计问题）；同时保持 I17：`Close()` 在锁内拷出水位、锁外才 fsync | TSan 全量：修复前 1 条 warning，修复后 **0**；`GroupCommit.FailurePropagatesToAllWaiters` 等并发失败传播用例全绿 |

配套的**测试替身加固**（并发用例的前提，非产品缺陷）：① `MemEnv` 内部**完全没有加锁**，而 I32 用例第一次让「T1 在飞 `Append`」与「主线程 `Sync()`」并发 ⇒ `MemEnv::AppendRaw` 数据竞争；真实 POSIX 文件的并发 `write`/`fsync` 由内核保证，内存替身必须自己串行化，故加 `std::recursive_mutex`（`DeleteFile→RemoveFile`、`FileExists→Find` 存在公开方法间调用）。② 新用例的 hook 原用 `mutex`+`condition_variable`，TSan 实测 `double lock` + 2 条 data race，改为原子布尔 + 有限自旋，判定不变（`taken_` 置位严格早于 `Append`）。

**对 M3 的影响**：`docs/m3-design.md` §0.5 登记的 I32 与本表为**同一缺陷**，M3 侧不必再开 `M3-A50` 的重复用例，改为引用本条回归用例；M3 设计里「I32 在 M3 顺手修」的计划已被本补丁**取代**（设计文档相应句子在进 `#2` 之前需要一次小修订）。

## 11. M2 进度与接续点（滚动更新：任何时刻中断，从这里接着做）

| 子里程碑 | 状态 | 证据锚点 |
|---|---|---|
| M2.1 WAL record 格式与跨块切分 | ✅ | A01~A10 全绿；`docs/m2-evidence.md` 的 M2.1 小节 |
| M2.2 DB::Open 恢复 + 崩溃对账 | ✅ | `TOTAL_ROUNDS 100 MISSING_TOTAL 0 MISMATCH_TOTAL 0`；A11~A19 全绿 |
| M2.2c 损坏注入端到端 | ✅ | B03 `TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0`；B04 中间损坏拒绝启动且可定位 |
| M2.2d 基准与门禁入口 | DONE | B05 恢复代价基线、B07 提交延迟（ext4 中位 2.5~3.2ms，证伪指令里 8ms）；scripts/lsm_gate.sh 一条命令跑完全部门禁 |
| M2.2e MemEnv + Options::env + A27~A30（掉电语义） | DONE | 3 种子 x 3 撕裂概率；50 轮连续崩溃；Sync/Close 持久性 |
| M2.2f A31（Close 期间并发写） | DONE | 4 写者 + 并发 Close：不 UAF/不死锁、拒绝新写、Close 幂等 |
| M2.3 组提交 + A20/A21/A23/A24 | DONE | 队首即 flusher；批合并成一条 record；谓词覆盖我能否接手；窗口最后一步放开；失败传播整批 |
| M2.3(b) TSan 全量（组提交后） | DONE | TSan 71/71、退出码 0、race 0 条（410s，含 1M 压力） |
| M2.3(c) A20 确定性版 | DONE | CommitHook::OnBeforeGroupAssemble 屏障：64 写者 -> 本批含 64、sync_calls==1、水位一步跳到 64 |
| docs/protocol.md 追加第 9 章 WAL 编码 | DONE | 204 行；patch 文本来自设计 4.6 |
| 门禁总览 | DONE | 干净重建 0 warning + 75/75；ASan 干净；TSan 全量 race 0；100 轮 kill -9 MISSING 0；截断扫描 1401/1401；中间损坏拒绝启动且可定位 |
| A25（持锁零 IO 的探针式验证） | TODO | 需 SpyLogWriter + MuHeldGuard（约 80 行 src 仪表 + 用例）；目前 I17 只有代码评审级证据 |
| #4 独立评审（M2） | 进行中 | 后台 subagent；9 条评审重点 + 5 个自曝风险点 |
| tag m2-wal | TODO | 待评审阻断项修完 |
| #4 独立评审 + 阻断项修复 + tag `m2-wal` | ⏳ 未做 | 评审重点见 M2 指令 §4 的 9 条 |

**接续时先做的事**：① `bash scripts/lsm_build.sh` 确认 66/66；② 读本文件 §9 的 5 处修订 + 1 条观察项；
③ 从 M2.2d 开始（`tests/memenv.{h,cpp}` 的内存 FS + fsync 水位 + 固定种子撕裂 + 崩溃回滚），
它同时也是 M2.3 组提交测试（A20~A26）的 seam。

另有两处**指令未覆盖、由设计补齐**的必要项（已在设计门获批，登记备查）：
- **D12**：M2 无 flush ⇒ 恢复期必须按 WAL 实测字节数放大 MemTable 容量，否则 100 MiB WAL 会在 `write_buffer_size` 处 `kFrozen` 导致恢复失败。
- **D10**：`LOCK` 文件进程级独占（超出指令明确要求，已由用户裁决纳入）。
