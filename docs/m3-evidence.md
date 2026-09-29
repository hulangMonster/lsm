# M3 证据（滚动更新）

> 纪律：本节所有数字都来自本仓库**实测**命令，附命令与原始结果行；未跑的一律写"未验证"。
> 基线：M2 最终 rev `a3c85a8`（tag `m2-wal`）；M3 设计 `961343e` + 修订 `fa086ca`。

## M3.1 —— SSTable 格式层（`format` / `block` / `table_builder` / `table`）

### 1. 交付
| 文件 | 说明 |
|---|---|
| `src/sstable/format.{h,cpp}` | 常量、块头/块尾、`BlockType`、`BlockHandle`(16B)、`Footer`(44B) |
| `src/sstable/block.{h,cpp}` | `BlockBuilder` / `BlockReader` / `ValidatePayload`（restart 语义、先校验再读、`EstimatedSizeAfter`） |
| `src/sstable/table_builder.{h,cpp}` | 数据块* → metaindex（空块）→ 索引块 → footer；CRC 覆盖 `length‖type‖payload`；`index_size_warn` |
| `src/sstable/table.{h,cpp}` | `Table::Open`/`ReadBlock`/`Get`/迭代；footer 失败矩阵；`verify_checksums` 可关（结构校验永不可关） |
| `tests/sstable_format_test.cpp` | `M3-A01`~`A08` |
| `tests/sstable_table_test.cpp` + `tests/sstable_counting_env.h` | `M3-A09`~`A19`（含 A19 的计数 Env seam） |
| `src/util/env.{h,cpp}` | **R6-b**：`RandomAccessFile` / `NewRandomAccessFile` / `SyncDir`（+ 所有 `Env` 子类兜底自检） |
| `scripts/lsm_gate.sh` | **v2**：多标记 AND 判定 + M3 四腿接线（缺脚本 ⇒ SKIP，绝不空绿） |
| `docs/protocol.md` | 追加 §10（204 → 377 行，**只追加**，sha256 前缀证明） |
| `docs/m3-design.md` | §15 追加 R6（文件拆分偏离 + R6-e 推迟决定） |

### 2. 判据实测（同一棵树）
| 判据 | 命令 | 结果 |
|---|---|---|
| 干净重建 0 warning + 全量 | `bash scripts/lsm_build.sh`（门禁腿内） | `PASS 干净重建 + 0 warning + 全量用例` |
| 默认全量 | `./build/bin/lsm_tests` | `102 tests from 22 test suites ran.` / `[  PASSED  ] 102 tests.` |
| ASan 全量 | `./build-asan/bin/lsm_tests` | `[  PASSED  ] 102 tests.`（无 ASan/LSan 报告） |
| TSan 全量 | `setarch $(uname -m) -R ./build-tsan/bin/lsm_tests` | `[  PASSED  ] 102 tests.`；`grep -c 'WARNING: ThreadSanitizer'` = **0** |
| 格式层零依赖（§11.1 机制化判据） | `cmake --build build --target lsm_sstable` + `nm -C build/liblsm_sstable.a \| grep -cE 'version\|db_impl\|wal'` | `[100%] Built target lsm_sstable`；计数 **0** |
| M2 四腿不退化（门禁 v2 严格标记） | `bash scripts/lsm_gate.sh --rounds 100 --no-asan` | 4 腿全 `PASS`；M3 四腿 `SKIP`，末行 `[PARTIAL] …（未验证，不是通过）` |

### 3. 反"空绿"检查（本仓库纪律，逐条实测）
| 检查 | 方法 | 结果 |
|---|---|---|
| 零断言 TEST 块 | `awk` 逐 `TEST(...){...}` 块扫描 `EXPECT_/ASSERT_` | **0 个** |
| 跳过/禁用/永真 | `grep -nE 'DISABLED_\|GTEST_SKIP\|\|\| true'` | **0 处** |
| 既有断言是否被放宽 | `git diff -U0` 中被删除行里的 `EXPECT_/ASSERT_` 计数 | **0**（`tests/sstable_format_test.cpp` 的 +46/-5 中，删掉的 5 行全是过期注释） |
| 逐字节翻转扫描是否真跑 | 用例打印的真实计数 | `FLIPS_DONE_A12=4912`、`FLIPS_DONE_A13=11`、`FLIPS_DONE_A14_INDEX=42`、`FLIPS_DONE_A14_TOTAL=45`（A12 另有 `EXPECT_GE(flips, 1000)`） |
| 门禁是否只看退出码 | `scripts/lsm_gate.sh` v2 的 `run_gate_marked` | 每条腿要求**多条标记 AND**，且每条腿独立日志（防"坏轮被好轮掩盖"）；缺脚本 ⇒ `SKIP` + `[PARTIAL]` |

### 4. 已登记的偏离与未闭合项（**诚实列出，不得当成已完成**）
1. **`TableOptions` vs `Options`**：设计 §405 要求把 `block_size`/`verify_checksums` 加进 `common.h` 的 `Options`；
   本阶段文件许可不含 `common.h`，实现改为在 `src/sstable/table.h` 定义 `TableOptions{block_size=4096, verify_checksums=true}`
   （语义逐字一致，名字与落点不同）。⇒ **M3.2 决定**：把 `Options` 别名/嵌套到 `TableOptions`，或反向统一。
2. **`RandomAccessFile` 未接入读块路径**：`Env` 已具备该能力（R6-b 交付），但 `Table::ReadExactFile` 仍用
   `NewSequentialFile + Skip + Read`（每读一块打开一次文件）。**刻意推迟到 M3.2**，与 `TableCache` + 随机读 +
   计数 seam 同批改（理由见 `docs/m3-design.md` §15 R6；隔离实验证明现在改会打破 `M3-A19` 的防空绿断言）。
3. **`Table::Get` 把 tombstone 与"不存在"都归 `kNotFound`**（`Status` 无 `kDeleted`）⇒ M3.2 的 `DBIter` 需要三态时在上层区分。
4. **`ReadStats.files_checked` 的递增归属未定**：应由 DB 层按"真的进了 `Table::Get` 的文件"计数；M3.2 的 `M3-A30` 需确认由谁递增，避免双重计数。
5. **`Table::Open` 会预读第一个数据块取 `smallest_`**（纯格式层没有 `META` 的 smallest/largest）⇒ 接 `Version`/`META` 后改为从元数据取。
6. **`M3-A11` 用 `block_size = 1`**：设计规定 `block_size < 512` 由 Options 校验拒绝，但该校验属 DB Open；在 `TableBuilder` 加校验时需改用例或改为直接注入计数器。
7. **`M3-A13` 的检查顺序是有意的**：`length < handle.size-9` 在 CRC **之前**检出、`length > handle.size-9` 由 CRC **之后**的长度上界兜底——为满足设计"两条都要断言"而排定，待 `#4` 评审确认。
8. **索引 key 校验比 §3.3 略严**：除 `len >= 8` 外还要求 `ParseInternalKey` 成功；库内不产生反例，属未覆盖边界。
9. **`M3-B01`~`B05`（flush 崩溃/落盘重启/SST 损坏扫描/句柄）尚未交付** ⇒ 门禁 v2 显式 `SKIP` 并汇总 `[PARTIAL]`，**未验证**，不得计入 M3 验收。

### 5. 与 M2 的关系
M2 的 5 条腿在 M3.1 后全部仍 `PASS`（见 §2 最后一行）；M2 的 83 例无回归，M3.1 新增 19 例（`Block`/`Footer`/`Table` 三个 suite）。

---

## M3.2 —— flush 路径 + 读路径 + `MergingIterator`/`DBIter` + 单后台线程

### 1. 交付
新增：`src/version_edit.{h,cpp}`、`src/version_set.{h,cpp}`（内存版 `Version` + `TableCache`）、
`src/merging_iterator.{h,cpp}`、`src/db_iter.{h,cpp}`、`tests/flush_test.cpp`、`tests/iterator_test.cpp`。
必改：`src/db_impl.{h,cpp}`（`memtable_` 改 `shared_ptr`、`immutables_`、flush 状态机、单后台线程、读路径串联、
`GetFlushStats()`/`DbReadStats`）、`CMakeLists.txt`；R6-e：`src/sstable/table.cpp`、`tests/sstable_counting_env.h`、
`tests/sstable_table_test.cpp`；设计 §8.7 E8：`src/common.h`（`Options` 增 `block_size`/`verify_checksums`/
`max_open_files`/`flush_hook` + `FlushHook`）；§8.5：`src/db.cpp`（`Open` 一次性校验）；§15 R2：`tests/crash_test.cpp`。

### 2. 判据实测（**本节数字全部由我在提交前独立复现**，非转述）
| 判据 | 命令 | 结果 |
|---|---|---|
| 干净重建 + 0 warning + 全量 | `bash scripts/lsm_build.sh` | `117 tests from 25 test suites ran.` / `[  PASSED  ] 117 tests.` / `[OK] 干净重建 + 0 warning + lsm_tests 全绿` |
| ASan 全量 | `./build-asan/bin/lsm_tests` | `[  PASSED  ] 117 tests.`（无 ASan/LSan 报告） |
| TSan 全量 | `setarch $(uname -m) -R ./build-tsan/bin/lsm_tests` | `[  PASSED  ] 117 tests.`；`grep -c 'WARNING: ThreadSanitizer'` = **0** |
| 格式层零依赖 | `nm -C build/liblsm_sstable.a \| grep -cE 'version\|db_impl\|wal'` | **0** |
| L18 探针（持锁零 IO） | `--gtest_filter='Flush.NoIoWhileHoldingDbMutex'` | `[  PASSED  ] 1 test.` |
| M2 门禁不退化（v2 严格标记） | `bash scripts/lsm_gate.sh --rounds 100 --no-asan` | 4 腿全 `PASS`（含 100 轮 kill -9 对账 `MISSING_TOTAL 0`）；M3 四腿 `SKIP` + `[PARTIAL]` |
| 用例数 | — | **102 → 117**（+15：`M3-A20`~`A34`） |

### 3. 反"空绿"核对
零断言 TEST = **0**；`DISABLED_`/`GTEST_SKIP`/`|| true` = **0**；被删除的断言共 **10 行**，逐条核对为
**两处已授权变更**：① §15 R2 的契约收窄（`DB.PutAfterFreezeIsNotPersisted` → `DB.PutBlocksUntilFlush`，删除
`IsFrozen` 断言）；② R6-e 的 seam 对象替换（顺序文件计数 → 随机读计数，**强度不变**：范围外必须 0、范围内必须 ≥1）。

### 4. 本阶段发现并修复的 **M3.1 实现缺陷**（我的 M3.1 审计漏网，登记在案）
- **缺陷**：`BlockReader::Seek` 的 restart 二分与线性扫描用 `std::string` **逐字节**比较，而不是
  `InternalKeyComparator`（违反设计 §5.3）。
- **为什么 M3.1 审计没抓到**：`M3-A04` 直接拿**裸 user key** 当 target；且当时没有"快照 sequence **小于**
  文件内 sequence"的查询。DB 层改用 `BuildLookupKey(user, snapshot)` 后，trailer 是**小端**字节序，
  逐字节比较会误判 `key < target` ⇒ 命中失败（隔离实验：同一文件 `snapshot=2000` 查不到、
  `snapshot=1048576` 查得到）。
- **修复**：`BlockReader::Open` 增加可选 `const InternalKeyComparator* icmp = nullptr`；非空时 `Seek` 一律走
  `icmp->Compare`；为空时保持逐字节比较 ⇒ M3.1 既有直接 `BlockReader` 用例零改动。
- **我的独立核对**：`src/sstable/block.h:70`（参数）、`block.h:104`（成员）、`block.cpp:192`（实现）已落地；
  117 例全绿。
- **教训**：格式层的字节级用例（A01~A08）**不足以**覆盖"键序语义"——键序要在**接入 DB 层 lookup key** 之后才暴露。
  ⇒ 已在 §6 的 M3.3 前置项里要求补一条"带 snapshot 的块内 Seek"回归。

### 5. 已跟踪的缺口与过渡妥协（不得当成已完成）
1. **R2 改写暂时丢失"重开后仍在 / 被拒 key 不存在"覆盖**：因过渡妥协②（`Open` 见到 `*.sst` ⇒ `kCorruption`）
   在 M3.2 无法测"重开"。⇒ **M3.3 必须由 `M3-A35`/`A36`（落盘重启、SSTable+WAL 尾巴）恢复该覆盖**，并作为 M3.3 验收前置。
2. **三条过渡妥协**（照设计 §11.2 写进代码注释）：① `META` 不写不读；② `RecoverAndOpen` 的 **5 行 guard**
   （目录存在 `*.sst` ⇒ `kCorruption` + 明确信息，**M3.3 删除**）；③ 不做 WAL 轮转与回收，`recycle_log_files` 本步无效。
3. **fd 缓存弱于设计 §7.4**：`TableCache` 缓存的是**已解析的 `Table`（内存索引）**而非常驻 fd；每次 `ReadBlock`
   经 `NewRandomAccessFile` 打开一次并随即释放。I30（句柄有上限、不泄漏）仍成立，但"缓存打开句柄"这层**未实现**。
4. **文件名 helper 落点**：`TableFileName`/`TempFileName`/`ParseTableFileName` 暂落在 `src/version_set.{h,cpp}`
   （设计 P17 要求进 `src/filename.*`；本阶段文件许可不含它）⇒ M3.3/M4 收拢。
5. `Table::Open` 仍保留"不传 `known_smallest/largest` 时预读首块"的兼容路径（供 M3.1 既有用例）。

### 6. 对 M3.3 的前置要求（由此阶段实测暴露，逐条必须落实）
- 删除过渡妥协②的 5 行 guard，并以 `M3-A35`/`A36` 恢复"重开后仍在"的覆盖。
- 补一条**带 snapshot 的块内 Seek** 回归（针对 §4 的缺陷类：键序必须走 `InternalKeyComparator`）。
- `recycle_log_files` 必须真正接线（本步为无效开关，M3.3 生效并有专属用例 `M3-A48`）。
