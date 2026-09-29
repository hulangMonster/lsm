# M4.0 前置复核 + M4.1 交付记录（docs/m4-prerequisites.md）

> 纪律：本文所有"通过"都附原始命令与输出；未跑的一律写"未验证"。
> 基线：`~/lsm-kv` 的 `main` HEAD = `43cdea309aa487f1bfcdd6cd88d73bdba2999e8f`（tag `m3-sstable`），
> 开工时 `git status --short` 为空、`git tag` 含 `m1-memtable/m2-wal/m3-sstable`。
> **本轮不改 `docs/m4-design.md` / `docs/m3-*.md` / `docs/m3-evidence.md`**（按用户纪律）；
> 设计 §11 M4.0 要求的"§15 追加一条 R 记录"因此只登记在本文 §2/§5，待用户写入设计。

## 0. M3 收口前置（M4.0 清单 C1/C15 的证据）

基线门禁在 **`git archive 43cdea3` 的纯净导出**（`/tmp/lsm_baseline`）上重跑，避免工作区改动污染证据：

```
$ cd /tmp/lsm_baseline && bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3
...（每条腿 --- [PASS]）...
==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  崩溃对账（kill -9 x 100，sync 模式）
PASS  逐字节截断扫描（B03）
PASS  中间损坏拒绝启动（B04）
PASS  M3-B01 flush 崩溃对账（kill -9 x 100）
PASS  M3-B03 落盘重启（records_replayed == 0）
PASS  M3-B04 SSTable 损坏扫描（零静默错值）
PASS  M3-B05 句柄计数不增长
[OK] 全部门禁通过
```

- tag `m3-sstable` 存在、工作区干净（`git status --porcelain` 空）；
- `M3-B01~B05` 四个脚本全部存在（`lsm_flush_crash_test.sh` / `lsm_flush_restart_test.sh` /
  `lsm_sst_damage_test.sh` / `lsm_fd_leak_test.sh`）；
- 基线全量用例数 = **141**（`./build/bin/lsm_tests`：`[==========] 141 tests from 30 test suites ran.` /
  `[  PASSED  ] 141 tests.`）。

## 1. 开工前置复核清单（15 项：一致 / 差异 → 处置）

| # | 复核对象 | 落地实现（实测） | 结论 |
|---|---|---|---|
| C1 | M3 是否已收口 | HEAD `43cdea3`、tag `m3-sstable`、干净工作区、141 例、8/8 门禁 | **一致** |
| C2 | `Version` 真实形态 | `class Version`（`src/version_set.h:32`）：`files()`（无参）+ `log_number()` + `min_log_number_to_keep()` + `next_file_number()` + `MaxSequenceInFiles()`；无 `level_files`/`AllFiles`/`Ref`；构造后只读 | **差异**：设计 §5.3 的层级/引用计数尚未落地（M4.2 承诺）→ M4.1 按**只增不改**补上 `level_files(int)`/`AllFiles()`/`total_bytes()`/`Ref()/Unref()`，保留 `files() == level_files(0)` |
| C3 | `files()` 排序 | `src/version_set.cpp:114` `std::sort(..., a.number > b.number)` | **一致**（按文件号降序） |
| C4 | `TableCache` 落点/键 | `src/version_set.h:96`（无独立文件）；键 = 文件号；容量 = `Options::max_open_files` | **一致** |
| C5 | `VersionEdit` 真实签名 | 全量快照形态：`SetLogNumber/SetMinLogNumberToKeep/SetNextFileNumber/SetComparatorName/AddFile(FileMetaData)/files()/ClearFiles/EncodeTo/DecodeFrom`；**无 level、无 DeleteFile、无差分、无 Clear** | **差异**（设计 §5.2 的取代登记成立）：M4.1 追加 `Clear()` / `AddFile(int,FileMetaData)` / `DeleteFile(int,number)` / `added_files()` / `deleted_files()` / `EncodePayloadTo` / `DecodePayloadFrom`；**保留** M3 的 `EncodeTo/DecodeFrom`（META 兼容读入） |
| C6 | `VersionSet::Recover` 签名 | `Recover(Env*, dbname, Options, const vector<string>& children, shared_ptr<const Version>*, RecoveryResult*)`；**不含 META 字样**；另有 `Persist(Env*, dbname, Options, const Version&, VersionEdit*)` | **差异**：设计 §5.3 的 `Version** out` + `RecoveryStats*` 与 `LogAndApply` 不在落地形态里 → M4.1 新增 `RecoverManifest`（取代路径）并**保留** `Recover/Persist`（默认路径）；`LogAndApply` 的**有状态**签名未实现（见 §5） |
| C7 | `filename.{h,cpp}` | 只有 `MakeFileName/LogFileName/ParseLogFileName/LockFileName`；`TableFileName/TempFileName/ParseTableFileName` 实际落在 `src/version_set.{h,cpp}` | **差异**：M4.1 在 `filename.{h,cpp}` 追加 MANIFEST/CURRENT 族（`ManifestFileName/ManifestTempFileName/CurrentFileName/CurrentTempFileName/ParseManifestFileName` + `ParseManifestTempFileName`/`ParseCurrentContents`）；table 族**不迁移**（迁移会改 M3 测试的 include） |
| C8 | `Options` 字段 | `comparator/write_buffer_size/env/commit_hook/block_size/verify_checksums/max_open_files/flush_hook/recycle_log_files` | **差异**：§5.6 的 6 个字段全部缺失 → M4.1 全部补上（+ `kNumLevels` + `CompactionHook`） |
| C9 | `TableOptions` 收敛 | `src/sstable/table.h:38` 已把 `TableOptions` 定义为 `Options` 的别名；`block_size` 只在 `Options` | **一致**（已收敛，M4.1 无需再改） |
| C10 | `MergingIterator`/`DBIter` | `MergingIterator(const InternalKeyComparator*, Iterator**, int)`；`DBIter(const InternalKeyComparator*, Iterator*, SequenceNumber, ...)`（多一个参数） | **差异**：`DBIter` 多一个参数（实现为准），M4.2 复用不重定义 |
| C11 | flush 线程与 `FlushHook` | `bg_thread_`/`bg_cv_`（`src/db_impl.h:222-223`）+ `class FlushHook`（`src/common.h:243`）三注入点 | **一致** |
| C12 | `META` 落点行 | `grep -c 'META' src/version_set.cpp` = **6**（注释 3、`MetaFileName` 1、`MetaTempFileName` 1、Persist 注释 1） | **一致**（登记基线值；M4.1 的取代路径新增写路径，但默认路径仍写 META） |
| C13 | M3-A39/A40/A41 是否**不绑定** META 文件名 | `tests/recovery_m3_test.cpp` 用 `VersionSet::MetaFileName()` 定位，但**第 877 行**直接按字符串匹配事件 `"META.tmp"`，且 `Recover.MetaCorruptRefused` 篡改 META 字节后要求 Open 必须 kCorruption | **差异（重大）**：设计假设"语义层、不绑文件名"与落地不符 → 见 §2 第 1 条（M4.1 的取代路径默认关闭，避免与既有 141 例冲突） |
| C14 | `FakeClock` 是否存在 | `grep -rn 'FakeClock' tests/ src/ | wc -l` = **0** | **一致**（M4.2 新建，M4.1 不需要时间 seam） |
| C15 | 门禁 M3 腿状态 | 非 SKIP、非 PARTIAL，8/8 PASS（见 §0） | **一致** |

## 2. 与设计草案的差异登记（M4.0 要求的"逐条列出"）

1. **`META` 取代路径与既有 M3 用例冲突（需用户裁决）**：设计 §3.6 要求 M4 稳态**永不写 `META`**；
   但 `tests/recovery_m3_test.cpp` 的 4 条用例把 META 当权威（读字节、篡改后要求拒绝启动、删除后要求
   `kCorruption`、事件序列里要求 `META.tmp → META` 的 rename）。⇒ **默认 `Options::use_manifest_metadata=false`
   保持 M3 路径；=true 时走 MANIFEST+CURRENT**。新增用例全部在 true 下跑。
   **代价**：M4 的取代路径不是默认；**收益**：既有 141 例零回归、新路径可被完整验证。
   请裁决：M4.2 是否允许改这 4 条 M3 用例的判据（改为语义层）并把默认值翻成 true。
2. **`VersionEdit` 双编码**：设计 §5.2 让 `EncodeTo/DecodeFrom` 承担 §11.3 差分编码；落地必须保留 META 的
   定宽编码（`Persist` 与 M3 用例都依赖它）。⇒ `EncodeTo/DecodeFrom` = META（逐字节不改），
   `EncodePayloadTo/DecodePayloadFrom` = §11.3 差分。协议文字与实现字节仍逐字一致（payload 编码）。
3. **`Version::files()` 语义**：按 §5.3 收窄为 `level_files(0)`；M3 的库全部是 L0 ⇒ 既有调用方语义不变。
4. **`filename` 的编号解析放宽**：`%06u` 只是最小宽度（`n >= 1000000` 是 7 位），设计 §3.1 的正则
   `^MANIFEST-[0-9]{6}$` 字面会拒绝合法编号 ⇒ `ParseManifestFileName` 按数值语义接受 1..20 位数字，
   **仍精确拒绝 `.tmp`**。
5. **`Options` 新增两个非 §5.6 字段**（已在上表登记）：`use_manifest_metadata`（过渡开关）、
   `manifest_roll_bytes`（模式 (a) 的阈值；A40 可用小值强制重建）。
6. **`LogAndApply`**：设计的有状态签名（`manifest_file()` 常驻句柄 + `live_versions()` + `install_mu_`）
   属 M4.2 的锁纪律范畴；M4.1 落地为**无状态** `AppendEdit + WriteSnapshotManifest + WriteCurrentAtomic + ApplyEdit`，
   语义等价、可测。**未实现** `install_mu_`/延迟删除队列（见 §5）。
7. **进度语义**：M3 的 flush 是"先内存注册、后持久化"（失败置 `bg_error_`）；M4.1 的取代路径沿用同一顺序
   （而非 §8.1 模式 (b) 的"先落盘后安装"）。差异登记；M4.2 收口时按设计调整。
8. **MANIFEST 回放不做 `.sst` 存在性/大小/max_sequence 复核**（M3 的 META 路径有）；M4.1 只做 §8.3 的
   语义校验（level/number 重复/层内重叠/comparator）。理由：回放必须先于孤儿判定，且 compaction 输出
   可能在注册前未被引用；复核留给 M4.2 的孤儿/引用计数收口。
9. **A08 的注入方式**：设计用 `CompactionHook/FlushHook` 的注入点；M4.1 无 compaction，改为**手工构造**
   "rename 前（CURRENT.tmp 存在、CURRENT 指旧）"与"rename 后（CURRENT 指新）"两个状态并各自回放成功。
   等价判据、非字面注入，登记。
10. **A04 的 CRC**：期望字节里的 `length/type/payload` 手工拼装，但 tail CRC 仍调用 `crc32c::Value`
    （同一实现）⇒ "CRC 不独立"。登记为已知薄弱点（手工复算 CRC32C 表超出本阶段范围）。
11. **设计 §11 M4.0 要求的"§15 追加 R 记录"未做**（用户禁止改 `docs/m4-design.md`）⇒ 以本文替代。

## 3. M4.1 交付物与逐条判据

**新增文件**：`tests/version_test.cpp`（13 例）、`scripts/lsm_manifest_test.sh`（门禁腿）、本文。

**必改**：`src/filename.{h,cpp}`（MANIFEST/CURRENT 命名+解析）、`src/version_edit.{h,cpp}`（差分编码+record 帧）、
`src/version_set.{h,cpp}`（层级 Version、`ValidateLevelLayout`、`ApplyEdit`、`RecoverManifest`、
`ReadCurrent/WriteCurrentAtomic`、`WriteSnapshotManifest/AppendEdit`、`DirectoryMaxNumber`）、
`src/common.h`（§5.6 的 6 字段 + `kNumLevels` + `CompactionHook` + 2 个过渡字段）、
`src/db_impl.{h,cpp}`（取代路径的接入：恢复分支 + flush 的 MANIFEST 追加/重建 + 孤儿分类 +
`RecoveryStats` 增量字段 + 诊断 getter）、`CMakeLists.txt`（`tests/version_test.cpp` +
`lsm_version` 独立目标）、`.gitignore`（`MANIFEST-*.tmp`/`CURRENT.tmp`）。

| 判据 | 命令 | 结果 |
|---|---|---|
| 新用例全绿（A01~A10/A39/A40/A42） | `./build/bin/lsm_tests --gtest_filter='VersionEdit.*:Manifest.*:Current.*:Migration.*:Install.FailureKeepsCurrentRecoverable'` | `[==========] 13 tests from 5 test suites ran.` / `[  PASSED  ] 13 tests.` |
| 全量用例只增不减 | `./build/bin/lsm_tests` | `154 tests from 35 test suites ran.` / `[  PASSED  ] 154 tests.`（141 → 154，+13） |
| ASan 全绿 | `./build-asan/bin/lsm_tests` | `[==========] 154 tests ...` / `[  PASSED  ] 154 tests.`（无 ASan/LSan 报告） |
| TSan 全绿、race 0 | `setarch $(uname -m) -R ./build-tsan/bin/lsm_tests` | `[  PASSED  ] 154 tests.`；`grep -c 'WARNING: ThreadSanitizer'` = **0** |
| 干净重建 0 warning | 门禁第一腿 / `cmake --build build` | 无 `error`/`warning` 行 |
| `lsm_version` 零越权依赖 | `nm -C build/liblsm_version.a | grep -cE 'db_impl|wal|memtable'` | **0** |
| 门禁（含新 M4 腿） | `bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3` | **9/9 PASS**，末行 `[OK] 全部门禁通过`（原始汇总见 §4） |
| protocol §11 纯追加 | `wc -l docs/protocol.md`；`head -n 377 docs/protocol.md | sha256sum` | `377 → 469`；前缀 sha256 = `d812f693378e2e7a423976c6d6a2144fdfbcbef6c07d82414136522e5019a33f`（= 追加前的整文件 sha256） |

**A 组 13 条逐条**：A01 往返/空 edit/64 KiB 与 1 B key 长度+手工长度；A02 只含 new/只含 deleted；
A03 截断/CRC/长度 0/长度越界/未知 type/level 越界/number 重复/file_size 0/smallest>largest/非 internal key；
A04 手工拼字节逐字节相等；A05 全量快照+N 增量回放（逐层逐文件号 + `edits_replayed==N+1`）；
A06 尾部半条截断到 `last_good_end` + 计数 + **截断后追加再回放仍正确**；A07 中间 CRC 坏 ⇒ kCorruption 不截断；
A08 rename 前/后两态都能打开；A09 CURRENT 缺失+目录非空 ⇒ kCorruption、空目录 ⇒ 空库、CURRENT.tmp 残留 ⇒ 安全阀；
A10 CURRENT 内容严格校验（5 种非法 + 1 种合法）；A39 META→MANIFEST 一次性迁移（第二次 Open `meta_migrated==0`）；
A40 重建写全量快照 + 所有族编号 < `next_file_number_`；A42 追加/重建失败后 CURRENT 仍可完整回放。

## 4. 门禁运行原文（M4.1 树）

```
$ cd ~/lsm-kv && bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3
...
==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  崩溃对账（kill -9 x 100，sync 模式）
PASS  逐字节截断扫描（B03）
PASS  中间损坏拒绝启动（B04）
PASS  M3-B01 flush 崩溃对账（kill -9 x 100）
PASS  M3-B03 落盘重启（records_replayed == 0）
PASS  M3-B04 SSTable 损坏扫描（零静默错值）
PASS  M3-B05 句柄计数不增长
PASS  M4-B11 MANIFEST/VersionEdit A 组 + 零依赖
[OK] 全部门禁通过
```

`scripts/lsm_manifest_test.sh` 的收尾标记行原文：
```
M4_TESTS_RAN 13  M4_TESTS_FAILED 0  LSM_VERSION_FORBIDDEN 0  [MANIFEST_OK]
```

## 5. 反"空绿"检查（逐条实测）

| 检查 | 方法 | 结果 |
|---|---|---|
| 零断言 TEST 块 | `awk` 逐 `TEST(...){...}` 块扫描 `EXPECT_/ASSERT_` | **0 个** |
| 跳过/禁用/永真 | `grep -rnE 'DISABLED_|GTEST_SKIP|\|\| true' tests/` | **0 处** |
| 既有断言被删除 | `git diff -U0 -- tests/ | grep -cE '^-.*(EXPECT_|ASSERT_)'` | **0** |
| 门禁只看退出码 | `run_gate_m3_marked` 多标记 AND + 独立日志 | M4 腿 4 个标记全 AND；缺脚本 ⇒ SKIP + `[PARTIAL]` |
| `lsm_version` 反向自检 | `nm` 计数 | 0（且脚本在计数≠0 时非零退出） |
| "真的跑了 M4 用例" | `M4_TESTS_RAN 13` | >0 |

## 6. 未做 / 未验证清单（诚实列出，不得计入验收）

1. **compaction 本体**（选层/选文件/归并/输出滚动/丢弃判据/快照）全部属 M4.2，**未实现**；
   `src/compaction.{h,cpp}` 不存在。
2. **锁纪律 L22~L29 未落地**：无 `install_mu_`、无 `deletion_mu_` + 延迟删除队列、
   无 `live_versions_` 引用归零判定、无 compaction 线程；§9.4 的全局锁序**未接线**。
   `Version::Ref/Unref` 已提供但**未被任何调用方使用**（仅 API 就位）。
3. **`LogAndApply` 的有状态签名未实现**（见 §2 第 6 条）；`manifest_file()` 常驻句柄、`live_versions()`、
   `manifest_edits_` 的对外统计入口（`GetManifestStats`）只在 `db_impl` 内部有 getter。
4. **`use_manifest_metadata` 默认 false**：取代路径不是默认；4 条 M3 用例仍绑 META 行为（§2 第 1 条），
   需用户裁决后才可翻转默认值与改 M3 用例。
5. **模式 (a) 的旧 MANIFEST 删除是即时删除**（CURRENT 切换成功后立即 `DeleteFile`），不是 §8.1 ⑪⑫ 的
   延迟队列 + `MaybeDeleteObsoleteFiles`；崩溃窗口内的残留由下一次 Open 的孤儿清理兜底（有计数）。
6. **A08 不是字面注入**（无 `CompactionHook/FlushHook` 注入点），只是两态手工构造 + 回放（§2 第 9 条）。
7. **A04 的 CRC 不独立**（§2 第 10 条）。
8. **M4-A41（已 ack 数据在 MANIFEST 回放后可见）与 A43（孤儿输出清理）未写**：它们在 §11 的 M4.1 判据之外，
   且依赖 M4.2 的 compaction/孤儿来源；A41 的 WAL 侧由既有 M3 用例覆盖，MANIFEST 侧只做了 A05/A06 的回放。
9. **M4-A11~A38、A43 全部未写**（属 M4.2/M4.3）。
10. **B 组 M4-B01~B10 未做**；`scripts/lsm_compaction_stress.sh`、`docs/amplification.md`、
    `docs/m4-evidence.md` 未创建；门禁只接了 `M4-B11` 的 A 组部分（`lsm_manifest_test.sh`）。
11. **三张放大统计表与策略对照负结果未入档**（M4.3）。
12. **`docs/protocol.md` 只追加了 §11**；§3.1 里"META 稳态不写"的措辞与默认路径仍写 META 的现状不一致
    （由 §2 第 1 条的开关解释）。
13. **设计 §15 的 R 记录未写入**（用户禁止改 `docs/m4-design.md`）。
14. **`docs/m4-design.md` §5.6 的 6 个 Options 字段虽已加，但 M4.2 的 `compaction_pick_strategy` 枚举
    只有前置声明**（opaque enum），完整枚举值落在 M4.2 的 `compaction.h`；当前默认值 = 0。

## 7. M4.2 本轮状态（用户裁决后的执行记录）

### 7.1 裁决执行（默认翻转 + M3 用例语义化）

用户裁决（见会话）：批准把"META 当权威"的 M3 用例改写成**语义层**判据，并把元数据实现**默认翻转**为
CURRENT + MANIFEST；删除 `Options::use_manifest_metadata` 开关；只保留"仅有旧 META 时读取并迁移"的兼容路径。

| 项 | 落地 |
|---|---|
| 默认翻转 | `Options::use_manifest_metadata` **已删除**；`RecoverAndOpen` 无条件走 `VersionSet::RecoverManifest`；flush 无条件走 MANIFEST 追加/重建；稳态只写 CURRENT + MANIFEST |
| META 绝不稳态写 | `VersionSet::Persist` 仅作为 M3 遗留编码器保留（生产不调用）；META 只在"没有 CURRENT/MANIFEST 且 META 存在"时被**读一次**并迁移删除；CURRENT 存在时残留的 META/META.tmp 按 ⑥f 当迁移残片删除并计数 |
| 旧库兼容用例 | `Migration.MetaToManifestOneShot`：手工构造"真实 .sst + META（M3 定宽编码）、无 CURRENT/MANIFEST"的旧库 ⇒ 首次 Open `meta_migrated==1`、CURRENT/MANIFEST 存在、META 消失、数据可见；第二次 Open `meta_migrated==0` |
| 语义化改写 | `Recover.MetaMaxSequenceVerifiedAgainstFullScan`、`Recover.MetaCorruptRefused`、`Recover.MetaMissingWithSstRefused`、`Recover.ComparatorNameMismatchRefused`、`WalReclaim.DeleteOnlyAfterMetaDurable`、`Close.AbandonsImmutablesWithCounter`、`Flush.RegistrationOrderChain`、`CrashFlush.*`（CrashEnv 的元数据 fsync 注入点）：对象从 META 改成**活动元数据实现**（CURRENT → MANIFEST），覆盖不丢 |
| 新增语义校验 | `VersionSet::VerifyRegisteredFiles`（注册文件存在 / file_size 一致 / max_sequence 与全量扫描一致 ⇒ 不符 kCorruption）；`RecoverManifest` 回放时 comparator 名称不符 ⇒ kInvalidArgument。这两条保住 M3-A38/A42 的覆盖 |

> **删除断言的逐条交代（反"放宽"核对）**：`git diff -U0 -- tests/` 显示被删的 `EXPECT_/ASSERT_` 共 15 行，
> 全部是"对象从 META 换成活动元数据"或"同一判据换了更强形态"：
> ① `flush_test.cpp` 的 7 行 META.tmp 顺序链 ⇒ 7 行 MANIFEST-<n>.tmp 顺序链（同谓词、同数量）；
> ② A38 的 `DecodeFrom/edit.files()/max_sequence 上下界` 4 行 ⇒ 回放 MANIFEST record + `added_files` 断言 +
> **补回** `max_sequence ∈ [1, kN]` 两条（见新代码）；
> ③ A39 的 1 行 `IsCorruption` ⇒ 拆成 `IsCorruption`（length=0 / 中间 CRC / CURRENT 非法）与
> `IsNotSupported`（未知 type）两类，强度不降；
> ④ A41/A51 的 2 行 `MetaFileName` 存在性/删除 ⇒ 同语义的 `CurrentFileName` + `ManifestFileName`（对象替换）；
> ⑤ CrashFlush 的 1 行 `MetaFileName` 存在性 ⇒ `CurrentFileName` 存在性。

### 7.2 M4.2 已交付（本轮）

1. `src/compaction.{h,cpp}`：`MaxBytesForLevel` / `Score` / `PickLevel`（L0 文件数、L1+ 字节数、平手取小层号）、
   `PickInputs`（round_robin 与 min_overlap 两种策略 + L0 传递闭包 X1 + 下层重叠集合）、
   `ShouldDrop`（析取判据）、`IsBaseLevelForKey`（X6：从 level+2 起步）。
2. `tests/compaction_test.cpp`：8 条确定性用例（A11/A12/A13/A14/A15/A16/A23-X6/A26）；
3. 门禁 M4 腿的过滤器扩展到 Compaction/Level0/LevelN/PickLevel/PickFile/L0Inputs/BaseLevelForKey/Drop
   （收尾标记行仍为 `[MANIFEST_OK]`，见 §4；本次原文：`M4_TESTS_RAN 21  M4_TESTS_FAILED 0  LSM_VERSION_FORBIDDEN 0  [MANIFEST_OK]`）。

### 7.3 M4.2 未做 / 未验证（本轮诚实清单）

1. **compaction 执行体**：`Compaction::Run`（归并 → 输出滚动 → 输出文件 write+fsync+rename → VersionEdit）
   **未实现**；`src/compaction.cpp` 目前只有选择与判据部分。
2. **compaction 线程与调度**：`compaction_thread_` / `compact_cv_` / `MaybeScheduleCompaction` /
   `MakeRoomForWrite` 的层级分支 / flush 优先于 compaction 的让路检查 —— 全部未接线。
3. **读路径层级化**：`GetInternal`/`NewIterator` 仍只走 `version_->files()`（L0）；L1+ 的文件不会被读，
   因此本轮的 compaction 选择模块**尚未进入生产路径**（只被测试直调）。
4. **锁纪律 L22~L29 未接线**：无 `install_mu_` / `deletion_mu_` / 延迟删除队列 / `live_versions_` /
   `Version::Ref/Unref` 调用点（API 仍在但无人调用）；§9.4 的全局锁序未落地；旧 MANIFEST 仍是即时删除。
5. **有状态 `LogAndApply` / `manifest_file()` / `GetManifestStats` 未实现**：仍是无状态
   `AppendEdit/WriteSnapshotManifest/WriteCurrentAtomic`；`manifest_rolls()` 等只是 db_impl 的诊断 getter。
6. **快照 API 未实现**：`GetSnapshot/ReleaseSnapshot/GetAtSnapshot/NewIteratorAtSnapshot` 与
   `smallest_snapshot_` 均缺；`ShouldDrop` 的 snapshot 参数目前没有生产调用方。
7. **A 组缺口**：M4-A17~A38、A41、A43 未写；A04 的 CRC 仍复用 `crc32c::Value`（未独立复算）；
   A08 仍是两态手工构造（未字面注入）；A34/A35/A36/A37/A38 依赖前述未接线的锁/线程/统计。
8. **B 组 M4-B01~B10 未做**（`COMPACTION_ROUNDS_TOTAL > 0` 的正向标记因此无从产生）。

### 7.4 本轮判据（原始输出）

```
./build/bin/lsm_tests
[==========] 162 tests from 42 test suites ran. (35813 ms total)
[  PASSED  ] 162 tests.

./build-asan/bin/lsm_tests        → [  PASSED  ] 162 tests.
setarch $(uname -m) -R ./build-tsan/bin/lsm_tests → [  PASSED  ] 162 tests.; WARNING: ThreadSanitizer = 0

bash scripts/lsm_build.sh         → [CHECK] warning 计数 = 0 ; [OK] 干净重建 + 0 warning + lsm_tests 全绿
bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3 → 9/9 PASS ; [OK] 全部门禁通过
bash scripts/lsm_manifest_test.sh → M4_TESTS_RAN 21  M4_TESTS_FAILED 0  LSM_VERSION_FORBIDDEN 0  [MANIFEST_OK]

零断言 TEST = 0 ; DISABLED_/GTEST_SKIP/|| true = 0 ; 全量用例 154 → 162（+8）
```
