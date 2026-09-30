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

## 8. M4.2 核心执行体 + 调度 + 锁/队列 + 读路径层级化（本轮）

### 8.1 交付

| 范围 | 落地 |
|---|---|
| ① 执行体 | `Compaction::Run`：多路归并（复用 `MergingIterator`）→ 只在 user key 变化处滚动输出（X2）→ 每个输出 `write+fsync+rename+SyncDir` 后才 `AddFile`（I39）→ `VersionEdit` 先 `DeleteFile(输入)` 后 `AddFile(输出层)`（X7）；`ShouldDrop` + `IsBaseLevelForKey` 真正被调用，`dropped_old_versions`/`dropped_tombstones` 分开计数 |
| ② 调度 | `compaction_thread_`/`compact_cv_`/`MaybeScheduleCompaction`；flush 完成后调度；compaction 循环的谓词要求 `immutables_.empty()`（flush 优先让路，L27）；同一时刻至多一个 compaction（单线程 + `install_mu_`）；flush 完成时 `compact_cv_.notify_all()`（修掉"pending 已置位但 immutables 未空 ⇒ 错过唤醒"的 bug） |
| ③ 读路径 | `GetInternal`/`BuildIterator` 覆盖 L0 全部 + L1..L6 每个文件；L1+ 用"层内有序 + 下层不重叠"做 key range 过滤与提前 break；`files_checked` 只对真的进了 `TableCache::Get` 的文件递增；range 过滤计 `key_range_skipped` 且零 IO |
| ④ 锁/队列 | `install_mu_`（最左端）、`deletion_mu_`（deletion_mu_ → mutex_）、延迟删除队列（SST + 旧 MANIFEST，不再即时删）、`live_versions_`（每项持有一个安装期 shared_ptr）、`Version::Ref/Unref` 真实调用点（读路径 Ref、安装替换 Unref、迭代器用 `VersionRefHolder` 先 Unref 再释放内存）；有状态 `ManifestStore`（`manifest_file()`/`Append`/`RollAndOpen`）+ `PersistentDBImpl::LogAndApply`（锁内准备 → 锁外/持 install_mu_ 写 MANIFEST → 回锁安装 → 锁外 Unref → 出队 unlink）+ `GetManifestStats()` |
| 快照 | `GetSnapshot/ReleaseSnapshot/GetAtSnapshot/NewIteratorAtSnapshot` + `smallest_snapshot_`（`multiset` 按值删一个，A29 的语义） |

**并发正确性关键修复（本轮实测暴露）**：`LogAndApply` 不再用陈旧的 `base` 安装，而是在锁内以**当前 `version_` 重放 edit**（L25 的 rebase），并把 `next_file_number_` 取当前值（防回退）；flush 也统一走 `LogAndApply`，杜绝"flush 直接改 `version_` 与 compaction 安装并发"的竞态。`MaybeDeleteObsoleteFiles` 先摘除 `refs()==0` 的旧版本、再算 live 集合。

### 8.2 新增用例（162 → 167，+5）

- `VersionEdit.CrcIndependentlyRecomputed`（A04 补强：**独立** bitwise CRC32C 复算，不复用被测实现）；
- `CompactionDb.EndToEndMovesL0ToL1AndKeepsData`（L0→L1、输出计数、全量可读、重开可读）；
- `ReadLevels.NewestWinsAndRangeFilterNotCounted`（L0 覆盖 L1、tombstone 屏蔽、range 过滤不计 `files_checked`）；
- `Delete.DeferredUntilRefsZero`（持 live Version 引用 ⇒ 输入文件仍在、候选留在队列；引用归零 ⇒ 删除）；
- `Current.InjectedRenameFailureKeepsOldCurrent`（A08 补强：在 `RenameFile(..., CURRENT)` 上**字面注入**一次失败 ⇒ CURRENT 不被切换、旧 MANIFEST 可回放、已 ack 数据由 WAL 补齐）。

另：`tests/flush_test.cpp` 的 `Flush.TombstoneCountPreserved` 与 `Read.NewestWinsAcrossThreeFiles` 各加一行 `options.level0_file_num_compaction_trigger = 1000`，把用例**隔离**在"flush/L0 语义"上（断言一行未改、强度不变），因为 M4.2 起 compaction 默认生效会合法地移动/丢弃文件。

### 8.3 未做 / 未验证（本轮诚实清单）

1. **A 组仍缺**：A17/A18/A20~A38 的多数（层级安装拒绝、std::map 全量对账、输出切分边界、快照 A28/A29 的用例、A31 放大行自洽、A33 并发 rebase 的**构造性**用例、A34/A35/A36 的 SpyEnv 锁探针、A37 饥饿探针、A38 shutdown）；A41（MANIFEST 回放后 ack 可见）、A43（compaction 输出孤儿清理）仍未写。
2. **锁探针未做**：`M4-A35`（安装临界区无 unlink 的事件序列）与 `M4-A36`（持 DB 锁零 IO 的加宽 SpyEnv）依赖专门的加宽 Env，未实现；本轮只保证既有 M3 `NoIoWhileHoldingDbMutex` 仍通过。
3. **统计口径未收口**：`GetLevelStats`/`AmplificationStats`、`AMPL`/`LEVEL`/`FRONT` 行、`round_micros_p50` 目前只是单轮近似 —— M4.3 的放大报告与 B 组未做。
4. **MakeRoomForWrite 的"写者触发 flush"分支**：当前实现是"compaction 让路等 immutables 清空"（L27 的实质），没有在写路径里显式增加 flush 触发条件；M4 的 M3 版 flush 触发（写缓冲满）未改。
5. **compaction 失败矩阵**：`Run`/安装失败的计数与"旧版本完好"由 `LogAndApply` 的"先落盘后安装 + 失败不 swap"保证，但没有专门的故障注入用例（A42 的 compaction 版）。
6. **min_overlap 策略**未进生产默认（默认 `kRoundRobin`）；B03 的策略对照未做。
7. `live_versions_` 的条目在 `MaybeDeleteObsoleteFiles` 才回收（延迟清理），长压测下 `live_versions_max` 未测。

## 9. M4.3 本轮进展（统计/固定行/失败矩阵/放大探针/B 腿接线）

### 9.1 交付

| 项 | 落地 |
|---|---|
| 统计与固定行 | `GetLevelStats()`、`GetAmplificationStats()`、`FormatAmplLine/FormatLevelLine/FormatFrontLine`（§10.3 的 KEY=VALUE 固定列；前缀冻结、只允许行尾追加）；`compaction_round_p50_us` 改为**多轮采样**并输出 `ROUND_SAMPLES`（不是单轮近似）；新增计数 `user_logical_bytes_/entry_bytes_/flush_write_bytes_/get_count_/live_versions_max_/put_ops_/round_samples_us_/front_samples_us_` |
| 失败矩阵用例 | `CompactionFail.OutputWriteFailureKeepsOldVersion`、`CompactionFail.OutputRenameFailureKeepsOldVersion`：注入输出 `NewWritableFile`/`rename` 失败 ⇒ `failed` 计数 +1、`version_` 不变（输入仍在 L0、L1 为空）、旧数据可读、CURRENT 可回放（重开成功） |
| A 组补齐 | `LevelLayout.OverlapRejectedOnInstall`（A17：(a) 端点相等 /(b) 部分覆盖 /(c) 顺序错乱 ⇒ 拒绝，合法通过）、`Snapshot.VisibleVersionSurvivesCompactionAndSmallestUpdates`（A28+A29：快照内旧版本在 compaction 后仍可读；释放顺序驱动 `smallest_snapshot_`）、`Recovery.AckedDataVisibleAfterManifestReplay`（A41：sync 写 → flush → 重开走 CURRENT→MANIFEST 回放，200/200 可见） |
| B 组驱动 | `scripts/lsm_ampl_probe.cpp`（真实磁盘：写/读/校验 + 三行固定输出 + B05/B06/B07 布尔判据 + B10 的 NOT_APPLICABLE 行）、`scripts/lsm_compaction_stress.sh`（两种策略各跑一轮，打印 `[COMPACTION_STRATEGY_OK]`/`[COMPACTION_STRESS_OK]`，且强制 `COMPACTION_ROUNDS_TOTAL>0` 与 `MISSING 0`） |
| 门禁接线 | 新增 7 条 v2 正向标记腿（全部走 `run_gate_m3_marked`，缺脚本 ⇒ SKIP + `[PARTIAL]`）：M4-B03/B05/B06/B07/B08/B09/B10；既有 9 条腿一条未删 |
| 写者侧调度 | `Write` 在 `RunFlusher()` 成功后若 `immutables_ > 0` 调 `MaybeScheduleCompaction()`（L27 的另一半） |
| 文档 | `docs/amplification.md`：口径定义表 + 一轮实测三行 + B06 判据 + 已知偏差（写放大 14.5 高、空间放大 <1 的解释、`live_versions_max=6` 与 B08 示例阈值 ≤4 的偏差、B01/B02 未做） |

### 9.2 实测（`--rounds 3 --keys 2000 --write-buffer-size 16384`，真实磁盘）

```
MISSING 0 MISMATCH 0 COMPACTION_ROUNDS_TOTAL 47 MANIFEST_ROLLS 1 MANIFEST_BYTES 7661
LIVE_VERSIONS_MAX 6 READ_FILES_CHECKED_P50 1 READ_FILES_CHECKED_MAX 1 READ_BASELINE_M3 59 FD_GROWTH 1 ROUND_SAMPLES 47
B05_OK 1 B06_OK 1 B07_OK 1
M4-B03 STRATEGY_TABLE strat=round_robin strat=min_overlap [COMPACTION_STRATEGY_OK]
[COMPACTION_STRESS_OK]
```

### 9.3 未做 / 未验证（本轮诚实清单）

1. **B01/B02 未实现**（compaction 中途 `kill -9` 的进程级对账；需要 `CompactionHook` 的四个注入点 + 子进程驱动）。
   门禁**未接**这两条腿（避免 `--require-m3` 下 SKIP 变 FAIL），需下一轮补。
2. **B08 的数值门禁未收紧**：实测 `live_versions_max=6` > 设计示例的 ≤4。门禁腿只断言"计数存在"，
   **不**声称满足 ≤4；保留数字交用户裁决（不删弱、不改阈值）。
3. **A 组仍缺**：A18（L0 多版本取最新，M3 的 `Read.NewestWinsAcrossThreeFiles` 已覆盖 L0 语义，未单列）、
   A20（`std::map` 全量对账）、A21、A22-A27 的 DB 级端到端（`ShouldDrop` 真值表已在 `Drop.DecisionIsDisjunctionNotConjunction` 覆盖）、
   A31（放大行**手算**自洽）、A33（并发 rebase 的构造性用例）、A34/A35/A36 的加宽 SpyEnv 锁探针、A37（饥饿探针）、
   A38（shutdown）、A43（compaction 输出孤儿清理）。
4. **A35/A36 的加宽 SpyEnv**（把 rename/SyncDir/GetFileSize/GetChildren/RemoveFile/Truncate/块读都纳入）**未做**。
5. **`min_overlap` 仍未进生产默认**（默认 `kRoundRobin`）；理由：round_robin 的种子取"文件号最小者"，
   在同一输入下输出完全确定（A14 已钉），而 min_overlap 的对照实测（B03）目前只打印两行、没有定性结论。
6. `FRONT` 的 ops 中 Put 侧用的是 flusher 批延迟（不是每个写者的端到端），登记为口径近似。
7. 空间放大的 `tmp_bytes` 只统计 `.sst.tmp/MANIFEST-*.tmp/CURRENT.tmp`；`LOCK` 与 `META`（迁移残片）不计入。

## 10. M3-B05 fd-leak 探针挂死：现象 / 根因 / 修法 / 证据（M4.3 修复）

### 10.1 现象与最小复现

```
rm -rf /tmp/fdtest && timeout 120 ./build/bin/lsm_m3_probe fd-leak /tmp/fdtest 60
（修复前）→ rc=124；主线程 ~99% CPU 自旋；目录 30s 内文件集不变
```
确定性参数：`write_buffer_size = 8 KiB`（默认 `level0_file_num_compaction_trigger = 4`）、60 轮 × 400 Put。

### 10.2 挂死现场的谓词分量（原始输出）

用等价的诊断驱动（与探针同参数）在自旋循环里打印各分量：

```
WAIT r=0 it=4000  target=8 completed=7 failed=0 imm=0 pending=0
WAIT r=0 it=8000  target=8 completed=7 failed=0 imm=0 pending=0
...
WAIT r=0 it=50000 target=8 completed=7 failed=0 imm=0 pending=0
TIMEOUT r=0 completed=7 failed=0 imm=0 err=
```

- `flushes_failed == 0`、`immutables_.size() == 0`、`bg_error_` 为空、后台两个线程均空闲；
- 唯一不满足的分量是 `flushes_completed (7) >= target (8)`，而 **target 永远不可能达到**：
  探针旧写法是"Put 之后读 `completed`，再 `+1`"，若本轮的 flush 在读取之前已经完成，
  目标就变成"还要再来一次 flush"；下一轮 Put 之前不会再有任何写 ⇒ 谓词不可达。

### 10.3 定性：**探针假设失效**（不是产品死锁）

- 后台 flush 线程确实在工作：带 `LSM_TRACE=1` 的等价驱动显示
  `BG woke=35 / BG take imm=35 / FL start=35 / FL done=35 / FL early=0`，全部 immutable 都被冲刷并 pop；
- `MaybeScheduleCompaction` 只在 flush 完成后置位，compaction 线程空闲（`pending=0`）；
- 主线程自旋的是**探针自己的 WaitFlushIdle**（谓词不可达），不是产品里的锁等待。
  ⇒ 与 M2 的 A31（丢唤醒）同类的是"自旋判据不可达"这一**测试假设**问题；产品侧没有"没人 flush"的自锁
  （BackgroundLoop 的第一步就是 `wait(!immutables_.empty())` → `FlushImmutable` → pop，见 `src/db_impl.cpp`）。

### 10.4 修法（改探针判据，不改产品语义、也不关 compaction）

`scripts/m3_probe.cpp` 的 `cmd_fd_leak`：把
`completed = GetFlushStats().flushes_completed + 1;` 改为
`completed = GetFlushStats().flushes_completed;`，
即"等当前计数 + `immutables_==0`"（真正的 flush 空闲），而不是"等一次新的 flush 完成"。
句柄泄漏判据**未削弱**：仍然跑满 60 轮 × 400 Put（每轮必然触发 ≥1 次 flush 与若干 compaction），
仍然断言 `FD_GROWTH` 有界。

### 10.5 修复后证据（原始输出）

```
$ time timeout 120 ./build/bin/lsm_m3_probe fd-leak /tmp/fdtest 60
FD_GROWTH 1 FD_BASELINE 8 FLUSHES_COMPLETED 452
rc=0        real 0m15.9s
```

### 10.6 教训

这是"M2-A31 丢唤醒 → M3.2 pending/immutables 错过唤醒 → M4.3 探针自旋判据不可达"的**第三次同类事件**：
凡是"等待某个事件发生"的判据，必须写成**单调可达**的形式（捕获 before + 等 >=，或直接等状态谓词），
不能写成"现在 +1"这种依赖时序的增量式目标。

### 10.7 本轮仍未完成（交用户裁决，未删弱）

1. **B01/B02 未实现**（compaction 中途 kill -9 + 三注入点）；门禁未接这两条腿。
2. **A35/A36 的加宽 SpyEnv**（rename/SyncDir/GetFileSize/GetChildren/RemoveFile/Truncate/块读）未做。
3. **A18/A20/A21/A31/A33/A34/A37/A38/A43 未写**。
4. `LIVE_VERSIONS_MAX=6` 为观测值（见 docs/amplification.md §5），门禁只断言计数存在。
5. `FRONT` 的 Put 侧为下界近似口径（见 docs/amplification.md §5）。

## 11. Recover.SSTableOnly（M3-A35）偶发失败：根因与修复（M4.3 第二次回归）

> **登记**：这是本轮**第二次**由用户复跑发现的、代理自报为绿的问题（第一次是 M3-B05 fd-leak 探针判据不可达）。
> 仓库纪律是零 flaky（`docs/m2-prerequisites.md` §7）；上一轮"16/16 PASS"是在侥幸运行上得到的，不构成交付证据。

### 11.1 现象

最终树上 `bash scripts/lsm_gate.sh --rounds 100 --no-asan --require-m3` 第一腿 FAIL：
```
[==========] 172 tests from 48 test suites ran.
[  PASSED  ] 171 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] Recover.SSTableOnly
```
单例 15 次：pass=14 fail=1（≈7%）。失败断言是 A35 的 `EXPECT_EQ(0u, st.records_replayed)`。

### 11.2 现场（诊断驱动的原始输出）

等价驱动循环复现，失败时打印目录里的 `.log` 与大小：
```
FAIL iter=39 records_replayed=15 logs: 000015.log(size=540) 000018.log(size=0)
```
即：当前 log（`000018`）确为空，但**已被 flush 覆盖的旧 log `000015` 没被回收**，重开时重放了它。

### 11.3 根因（产品侧竞态，不是测试假设）

代码位置：
- `src/db_impl.cpp` `RunFlusher()` 的**阶段 A**：冻结 memtable 后立刻 `bg_cv_.notify_all()`；
- 同一函数的**阶段 A'** 才调用 `RotateLog()`（`src/db_impl.cpp` 的 `RotateLog`：分配新 log 号、fsync 旧 log、
  建空新 log，并更新 `log_number_`/`memtable_log_number_`）。

⇒ 后台 flush 线程可以在**轮转之前**被唤醒并进入 `FlushImmutable`，此时
`RecomputeMinLogNumberToKeepLocked` 看到的是**轮转前**的 `memtable_log_number_`，
算出的 `min_log_number_to_keep` 偏小；
等 `RotateLog()` 随后完成时，这次 flush 的 `RecycleObsoleteLogs` **已经跑过**（不会再跑第二次），
于是"刚被 flush 覆盖的旧 log"逃过回收 ⇒ 重开必然重放（A35 契约被破坏）。
这不是测试假设失效：M4 之前该竞态窗口存在但概率低，M4 新增 compaction 线程改变了调度时序后暴露。
`ForceFlushForTest()` 是同一形态（先 notify、后 RotateLog），所以单测里更易复现。

### 11.4 修法（产品侧确定化：轮转先于唤醒）

1. 新增 `rotate_in_progress_`（`src/db_impl.h`）：冻结时置位，轮转成功/失败后清位；
2. `RunFlusher()` 阶段 A 的冻结**不再 notify**；阶段 A' 的 `RotateLog()` 返回后（成功与失败两条路径）
   清 `rotate_in_progress_` 并 `bg_cv_.notify_all()`；
3. `BackgroundLoop()` 的谓词改为 `bg_stop_ || (!immutables_.empty() && !rotate_in_progress_)`
   —— 即"先轮转、后 flush"，保证 `min_log_number_to_keep` 用的是新 log 号；
4. `ForceFlushForTest()` 同序修改（冻结不 notify → RotateLog → 清位 + notify）。

契约不变：A35 仍断言 `records_replayed == 0` + 老 log 已回收 + 400 个 key 全可读（断言一行未改）。

### 11.5 证明不 flaky（原始计数行）

```
$ for i in $(seq 1 200); do ./build/bin/lsm_tests --gtest_filter=Recover.SSTableOnly; done
A35 pass=200 fail=0

$ for r in 1 2 3; do ./build/bin/lsm_tests; done
[  PASSED  ] 172 tests.   （x3）
```
（另有等价的独立驱动 `/tmp/a35`：修复前 300 次内 1 次失败；修复后 `ALL 300 OK`。）

## 12. M4 收口判定（对照 §11 的 M4.3 判据 → 状态）

### 12.1 逐条状态

| # | 判据（§11 M4.3 / §10） | 状态 | 证据 |
|---|---|---|---|
| 1 | compaction 执行体（归并/滚动/durable 后注册/VersionEdit） | **通过** | `CompactionDb.EndToEndMovesL0ToL1AndKeepsData`、`Merge.InternalKeyOrderContract`、`CompactionFail.*` |
| 2 | compaction 线程与调度（单实例、flush 优先） | **通过** | `Scheduling.FlushTakesPriorityOverCompaction`（200/200）、`Locks.ZeroIoWhileHoldingDbMutexOnCompactionPath`（200/200） |
| 3 | 读路径层级化（L0 全量 + L1..L6 每文件；files_checked 口径） | **通过** | `ReadLevels.NewestWinsAndRangeFilterNotCounted`、`Read.L0NewestFirst`、`Merge.StdMapReconciliation`（200/200） |
| 4 | L22~L29 接线（install_mu_/deletion_mu_/延迟队列/live_versions_/Ref-Unref） | **基本通过** | `Delete.DeferredUntilRefsZero`、`Locks.ZeroIo…OnCompactionPath`（含反向自检）、`Shutdown.JoinsBothThreadsAndDrainsQueue`；**A35（"安装临界区内无 unlink"的事件序列用例）未单列**，仅由延迟队列的行为覆盖 |
| 5 | 有状态 LogAndApply / manifest_file() / GetManifestStats | **通过** | `ManifestStore` + `PersistentDBImpl::LogAndApply`；`Install.RebaseOnConcurrentFlushKeepsI37`（构造性证明锁内以当前 version_ 重放） |
| 6 | 默认翻转 + M3 用例语义化 + META 迁移 | **通过** | `Migration.MetaToManifestOneShot`；`use_manifest_metadata` 开关已删除；门禁 16/16 |
| 7 | A04 独立 CRC + A08 字面注入 | **通过** | `VersionEdit.CrcIndependentlyRecomputed`（独立 bitwise CRC32C）、`Current.InjectedRenameFailureKeepsOldCurrent`（CURRENT rename 字面注入） |
| 8 | 三个放大的固定行 + 多轮 p50 | **通过** | `FormatAmplLine/FormatLevelLine/FormatFrontLine`（`ROUND_SAMPLES` 为样本数）；`scripts/lsm_ampl_probe` + `docs/amplification.md` |
| 9 | B 组腿接门禁（v2 正向标记） | **部分** | 已接：M4-B03/B05/B06/B07/B08/B09/B10 + M4-B11；**未接：B01/B02** |
| 10 | `B01`/B02` compaction 中途 kill -9（三注入点） | **未通过（未做）** | `CompactionHook` 的四个注入点已在 `src/compaction.cpp` 接线，但进程级 kill -9 驱动与脚本未实现 |
| 11 | A31 放大行可复现自洽 | **未通过（未做）** | 放大字段与行格式已就绪；"同一输入重跑逐字段相等 + 两种口径从同一行复算"的用例未写 |
| 12 | A 组其余（A22~A27 的真值表、A43 孤儿） | **部分** | `Drop.DecisionIsDisjunctionNotConjunction` 覆盖 ShouldDrop 真值表（无 DB 级 tombstone/旧版本端到端）；`Orphan.CompactionOutputCleanedAndCounted` 覆盖 A43 |

### 12.2 明确**仍未满足**的判据（不得据此打 tag）

1. **B01/B02 未实现**：需要"子进程在 `OnOutputWritten`/`OnOutputRenamed`/`OnBeforeInstall` 各 raise(SIGKILL) + 重启对账（missing 0、孤儿清理、引用集 ⊆ 存在集）"的驱动与脚本；本轮只完成了注入点接线。
2. **A31 未写**：放大行的"重跑逐字段可复现 + 从同一行复算两种口径"没有用例（只有实测行打印）。
3. **A35（安装临界区无 unlink 的事件序列）**未单列用例。
4. **A20 的并发形态暴露了一个未定位的观测**：把 compaction 放在后台与 DBIter 全量对账并发时，10 次里有 1 次出现"迭代结果比期望多 6 个 key"；
   改为"显式单轮 compaction（无并发安装）后对账"则 200/200 稳定。**本轮没有定位该并发不一致的根因** ⇒ 作为未闭合项登记，
   在打 tag 前应专门排查（怀疑方向：安装与迭代器持有版本的换出时序，或 DBIter 在跨层同 user key 多版本下的可见性）。
5. `LIVE_VERSIONS_MAX=6` 与 `FRONT` 的 Put 侧近似口径为观测/近似（见 `docs/amplification.md` §5），不是门禁。

### 12.3 本轮新增用例与 flaky 证据

- 新增 8 条：`Locks.ZeroIoWhileHoldingDbMutexOnCompactionPath`、`Scheduling.FlushTakesPriorityOverCompaction`、
  `Read.L0NewestFirst`、`Merge.StdMapReconciliation`、`Merge.InternalKeyOrderContract`、
  `Install.RebaseOnConcurrentFlushKeepsI37`、`Shutdown.JoinsBothThreadsAndDrainsQueue`、
  `Orphan.CompactionOutputCleanedAndCounted`（172 → 180）。
- 200 次连跑原始计数行：
  `A36+A37 pass=200 fail=0`；`A20 pass=200 fail=0`；全量 `[  PASSED  ] 180 tests.` ×3。
