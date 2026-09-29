# M3 设计（docs/m3-design.md）—— SSTable 与刷盘

> 本文件是流水线 `#0`（`/brainstorming`，architectural 路径）的产物，**冻结后为 M3 的唯一实现依据**。
> 本阶段**只产出设计**，不写任何实现代码，**不修改任何既有文件**（唯一新文件就是本文件）。
>
> 范围：`M3-SSTable与刷盘.md` §0 的 6 条目标、§1 的 7 条硬性约束、§0 第二步的 8 个开放决策。
>
> **流程闸门**：本文件写完后**必须暂停**，等用户明确评审批准后才允许进入 `#1`（`docs/m3-prerequisites.md`）。
> 本阶段**不得**跳到 `/writing-plans`——本流水线在设计与计划之间刻意插入 `#1`/`#2`。

---

## 0. 环境探测与原始证据（`#0` 的硬要求）

> 执行时间：2026-09-29 23:40 ~ 2026-09-30 00:20 (+08:00)。
> 机器 `tengyujie-virtual-machine`（`ubuntu-vm`）：Linux 6.8.0-138-generic x86_64，8 vCPU，
> 根文件系统 `/dev/mapper/vgubuntu-root` ext4；`g++ 11.4.0`、`cmake 3.22.1`、GTest 静态库 `/usr/local/lib/`。
> 探测程序全部放在 `/tmp/m3probe/` 与 `/tmp/m2base/`，**未触碰 `~/lsm-kv` 源码**。
> 探测期间 `loadavg` 峰值 `3.41 2.85 1.93`（探测自身造成），下文凡涉及耗时的数字都注明这一点。

### 0.1 前置：`~/lsm-kv` 工作区**不干净**，M2 基线必须用干净克隆

这是本阶段发现的**第一个阻断级事实**，它决定了后续所有基线数字的取法。

```
$ cd ~/lsm-kv && git log --oneline -1 && git status --porcelain && git stash list
3603696 feat(m2): A22 mixed-sync batch + A25 probe for I17 (zero IO while holding the DB mutex)
 M src/db_impl.cpp
 M src/db_impl.h
（git stash list 无输出）
```

在 `~/lsm-kv` 直接跑门禁的原始输出（**5 腿全 FAIL**）：

```
$ bash scripts/lsm_gate.sh --rounds 20 --no-asan
--- [FAIL] 干净重建 + 0 warning + 全量用例（详见 /tmp/lsm_gate_yWTHsc/gate.log）
/home/tengyujie/lsm-kv/src/db_impl.cpp:479:8: error: redefinition of ‘lsm::Status lsm::PersistentDBImpl::MaybeDeleteObsoleteLogs()’
/home/tengyujie/lsm-kv/src/db_impl.cpp:469:8: note: ‘lsm::Status lsm::PersistentDBImpl::MaybeDeleteObsoleteLogs()’ previously defined here
/home/tengyujie/lsm-kv/src/db_impl.cpp:694:12: error: ‘class lsm::Env’ has no member named ‘SyncDir’
gmake[2]: *** [CMakeFiles/lsm.dir/build.make:202: CMakeFiles/lsm.dir/src/db_impl.cpp.o] Error 1
...
==== lsm_gate 汇总 ====
FAIL  干净重建 + 0 warning + 全量用例
FAIL  崩溃对账（kill -9 x 20，sync 模式）
FAIL  逐字节截断扫描（B03）
FAIL  中间损坏拒绝启动（B04）
```

未提交 WIP 的三处问题（`git diff` 逐处可核）：

| # | 问题 | 后果 |
|---|---|---|
| W1 | 删除 `namespace { struct DbMutexGuard … }` 与 `bool DbMutexHeldOnThisThread()`，8 处改回 `std::lock_guard` | **回退刚提交（3603696）的 A25 探针**——M2 残留清单 §3.2 的 I17 探针式验证再次丢失 |
| W2 | `MaybeDeleteObsoleteLogs()` 在 `src/db_impl.cpp` 定义两次（`469` 与 `479` 行，注释不同） | 编译失败 |
| W3 | 新增 `s = env->SyncDir(name);` 但 `Env` 无该成员 | 编译失败 |

**处置**：本设计**不改动任何既有文件**，因此不为它做修复。M3 的基线在 `/tmp/m2base`
（`git clone ~/lsm-kv` 得到的 HEAD 干净树）上采集——**采集时** `~/lsm-kv` 不可用；
提交前它已回退（见下方复核块），此后两个路径等价。

> **提交前复核（2026-09-29T23:57:57+08:00，原文保留，处置已更新）**：上述 WIP **已被回退**。
> 复核命令与原始输出：
> ```
> $ cd ~/lsm-kv && git status --porcelain && git diff --stat && git rev-parse HEAD
> ?? docs/m3-design.md          ← 只有本设计文档是未跟踪的新文件
> （git diff --stat 无输出 = 工作区相对 HEAD 干净）
> 36036960af66658475a8c500ab15ebb4561665c0
> $ grep -c "DbMutexHeldOnThisThread\|DbMutexGuard" src/db_impl.cpp src/db_impl.h
> src/db_impl.cpp:12
> src/db_impl.h:1              ← A25 探针 12 处在位（W1 已消除）
> $ grep -c "MaybeDeleteObsoleteLogs" src/db_impl.cpp
> 0                            ← 重复定义已消除（W2）
> $ grep -c "SyncDir" src/util/env.h
> 0                            ← `Env::SyncDir` 仍未落地（**这是 M3 的必须交付项**，见 §1.3.2）
> ```
> ⇒ **Q7 关闭**（按推荐 ① 处置）：`~/lsm-kv` 现在本身就是干净的 M2 基线，与 §0.2 用的
> `/tmp/m2base` **同一 rev（`3603696`）**。上文 §0.1 的 5 腿全 FAIL 输出**保留为当时的事实记录**
> （`roadmap.md` §3.8"负结果入档：区分结论作废与原文保留"），不再代表当前状态。
> **注意**：`Env::SyncDir` 的缺失**不是**残留 WIP 造成的——它是 M2 设计 §5.7 声明过、
> 实现从未落地的能力缺口（`m2-review.md` §3 残留清单第 4 项），M3 必须交付（§1.3.2）。

```
$ git clone -q ~/lsm-kv /tmp/m2base && cd /tmp/m2base && git log --oneline -1 && git status --porcelain
3603696 feat(m2): A22 mixed-sync batch + A25 probe for I17 (zero IO while holding the DB mutex)
（git status --porcelain 无输出 = 干净树）
```

**tag 与 HEAD 的关系**（M3 必须声明"以哪个 rev 为 M2 基线"）：

```
$ cd ~/lsm-kv && git log --oneline -1 m2-wal && git log --oneline m2-wal..HEAD
8189607 docs(m2): record final gate matrix on the post-fix code (gate 5/5 + TSan 79/79 race 0)
3603696 feat(m2): A22 mixed-sync batch + A25 probe for I17 (zero IO while holding the DB mutex)
75eb1c0 feat(m2): GetRecoveryStats (design 8.2) with counted truncation/skips and threshold WARN
$ git merge-base --is-ancestor m2-wal HEAD && echo "m2-wal IS ancestor of HEAD"
m2-wal IS ancestor of HEAD
```

**结论**：tag `m2-wal` = `8189607`，**落后 HEAD 两个提交**。本设计以 **HEAD `3603696`** 为 M2 基线
（理由：它是干净树里能全绿的最新 rev，且含 `GetRecoveryStats`——M3 的可观测性口径要用它）。
`#1` 必须把"基线 rev"写死，评审据此核对。

### 0.2 探测①：门禁基线 —— 干净克隆上全绿（M2 收口基线成立）

```
$ cd /tmp/m2base && bash scripts/lsm_gate.sh --rounds 20 --no-asan
== lsm_gate: rounds=20 asan=0 tsan=0 log=/tmp/lsm_gate_c5TJXJ/gate.log ==
=== [gate] 干净重建 + 0 warning + 全量用例 ===
--- [PASS] 干净重建 + 0 warning + 全量用例
=== [gate] 崩溃对账（kill -9 x 20，sync 模式） ===
--- [PASS] 崩溃对账（kill -9 x 20，sync 模式）
=== [gate] 逐字节截断扫描（B03） ===
--- [PASS] 逐字节截断扫描（B03）
=== [gate] 中间损坏拒绝启动（B04） ===
--- [PASS] 中间损坏拒绝启动（B04）

==== lsm_gate 汇总 ====
PASS  干净重建 + 0 warning + 全量用例
PASS  崩溃对账（kill -9 x 20，sync 模式）
PASS  逐字节截断扫描（B03）
PASS  中间损坏拒绝启动（B04）
[OK] 全部门禁通过
```

同一份日志 `grep` 出的关键计数（`/tmp/lsm_gate_c5TJXJ/gate.log`）：

```
[CHECK] warning 计数 = 0（要求 0）
[==========] 82 tests from 19 test suites ran. (29526 ms total)
[  PASSED  ] 82 tests.
ROUND 1 ACKED_RECOVERED_MISSING_MISMATCH ROUND 0 ACKED 6 RECOVERED 6 MISSING 0 MISMATCH 0 TRUNCATED_BYTES 0 OPEN_MS 0 RC 0
ROUND 2 … ACKED 53 RECOVERED 53 MISSING 0 MISMATCH 0 …
（… ROUND 3..20 同形，全部 MISSING 0 MISMATCH 0 …）
TAIL_CASES 1401 TAIL_OK 1401 TAIL_FAIL 0 RECORD_BYTES 40
MIDDLE_OPEN_CORRUPTION 1 RECOVERED_PREFIX -1 DETAIL Corruption: RecoverAndOpen: log 中间损坏（其后仍有完好 record）: …/000001.log @1960 CRC 不符
```

**结论**：M2 收口基线在 HEAD `3603696` 的干净树上**成立**（82 例 / 0 warning / 4 腿 PASS）。
`#1` 与 `#3` 的每一条"未回归"结论都以这条数字为分母。
**该基线在提交时可从 `~/lsm-kv` 直接复现**（其工作区已回退到同一 rev，见 §0.1 的"提交前复核"），
`/tmp/m2base` 只是**采集时的**隔离手段。

### 0.3 探测②：`docs/protocol.md` 与 `src/wal.cpp` 的编码口径（M3 要复用）

静态核对（逐行）：

- `docs/protocol.md` §9.1：`record := header(7B) || payload`，`header := crc32c(4B, LE) || length(2B, LE) || type(1B)`。
- `docs/protocol.md` §9.3：`crc_input := length(2B LE) || type(1B) || payload`，即 **CRC 字段之后的全部字节**。
- `src/wal.cpp` 的函数 `FragmentCRC(prefix3, payload, n)`：`return crc32c::Extend(crc32c::Value(prefix3, 3), payload, n);`，
  调用点 `FragmentCRC(rest.data() + pos + 4, rest.data() + pos + kWALHeaderSize, len)`
  ⇒ `prefix3` 就是 `bytes[4..7)` = `{length_lo, length_hi, type}`，**与 §9.3 逐字一致**。
- `src/wal.cpp::DecodeHeader` 先校验 `type ∈ {1..4}` 且 `length ∈ [1, 32761]`，**再**算 CRC（"解码前必须校验 length 合法性"）。

**用真实 WAL 文件做的实证核对**（探针 `/tmp/m3probe/wal_crc_probe.cpp`，输入是 0.4 节产生的
`/tmp/m3scale/db1m/000001.log`，1 000 000 条写 / 138 028 642 字节）：

```
$ ./wal_crc_probe /tmp/m3scale/db1m/000001.log
FILE /tmp/m3scale/db1m/000001.log SIZE 138028642
OFF 0 HEADER_BYTES 4c7f42ba 8300 01 STORED_CRC ba427f4c LEN 131 TYPE 1 CRC_over_len_type_payload ba427f4c MATCH 1 | CRC_over_type_payload 8611f4bd MATCH 0
OFF 138 HEADER_BYTES 4f22fa85 8300 01 STORED_CRC 85fa224f LEN 131 TYPE 1 CRC_over_len_type_payload 85fa224f MATCH 1 | CRC_over_type_payload b9a9a9be MATCH 0
OFF 276 HEADER_BYTES 38daa954 8300 01 STORED_CRC 54a9da38 LEN 131 TYPE 1 CRC_over_len_type_payload 54a9da38 MATCH 1 | CRC_over_type_payload 68fa51c9 MATCH 0
BATCH_FIRST seq=1 count=1
SIZE_MOD_32768 9826
```

读法：`4c7f42ba 8300 01` = `crc=0xBA427F4C`(LE) `length=0x0083=131` `type=0x01=kFullType`；
`BATCH_FIRST seq=1 count=1` 与 §9.4 一致；`SIZE_MOD_32768 9826` 说明最后一个物理块未填满。

**结论（M3 必须复用的三条口径）**：
1. **CRC 覆盖面含长度字段**——`crc_input = length || type || payload`，且"含长度"的理由（让长度完整性成为
   CRC 契约的一部分，而不是靠"用错长度取到错 payload ⇒ CRC 碰巧失败"间接推断）**直接适用于 SSTable 块**。
   §0.3 的 `MATCH 1 / MATCH 0` 两列证明实现确实按此走，而 LevelDB 口径（不含 length）在本仓库**不成立**。
2. **解码前先校验结构字段合法性，再算 CRC**（`DecodeHeader` 的顺序），否则畸形 length 会先越界读。
3. **所有多字节整数逐字节拼装/解析、小端**，禁止 `reinterpret_cast`（§1）。`crc32c::Value/Extend` 是唯一 CRC 入口。

### 0.4 探测③：1 MiB value / 100 万 key 规模下 M2 现有的内存与吞吐（SSTable 判据的参照系）

探针 `/tmp/m3probe/scale_probe.cpp`（链接 `/tmp/m2base/build/liblsm.a`，**未进仓库**）。

**(a) MemTable 每条目真实占用**（决定"4 MiB 写缓冲能装多少条 ⇒ 一次 flush 的 SSTable 规模"）：

```
$ ./scale_probe /tmp/m3scale
MEMTABLE 16B/100B       klen=16 vlen=100 n=1000000 total_bytes=246000264 bytes_per_entry=246.0 entries_in_4MiB=17050 entries_in_64MiB=272800
MEMTABLE 16B/1024B      klen=16 vlen=1024 n=200000 total_bytes=234200264 bytes_per_entry=1171.0 entries_in_4MiB=3582 entries_in_64MiB=57309
MEMTABLE 24B/100B       klen=24 vlen=100 n=500000 total_bytes=127000264 bytes_per_entry=254.0 entries_in_4MiB=16513 entries_in_64MiB=264208
MEMTABLE 16B/1MiB       klen=16 vlen=1048576 n=64 total_bytes=67118600 bytes_per_entry=1048728.1 entries_in_4MiB=4 entries_in_64MiB=64
```

**独立交叉验证**：同一次探测的 DB 层用**默认 4 MiB 写缓冲**写，在第 17048 条被 `kFrozen` 拒绝：

```
DB def4MiB PUT_FAIL at i=17048 status=Frozen: Put/Delete: memtable is full (M2 无 flush)
```

`4 MiB / 246 B = 17050`，与 DB 层独立观测到的 `17048` 同量级 ⇒ **"246 B/条目"这个数字不是探针自证**。

**(b) 100 万 key（16 B key / 100 B value）的写吞吐 / WAL 体积 / 恢复耗时**（`sync=false`，写缓冲放大到 1 GiB 使 M2 不冻结）：

```
DB 1M-16/100      klen=16 vlen=100 n=1000000 wbs=1073741824 write_ms=15891.2 keys_per_s=62928 mean_put_us=15.89 close_rc=0 open_ms=5794.5 open_rc=0
DB 1M-16/100 DISK_CMD: du -sb /tmp/m3scale/db1m | cut -f1; ls -l …/000001.log
138032738
138028642
```

⇒ **写 100 万 key：15 891 ms（62 928 keys/s，均值 15.89 µs/Put）；WAL 138 028 642 B（138.0 B/条目）；
重新 `Open`（M2 = 全量重放 WAL）5 794.5 ms。**
**`Open` 5.79 s 就是 M3 的收益分母**：SSTable 之后，这 5.79 s 应收缩为"读 META + 重放未刷盘的 WAL 尾巴"。

**(c) 1 MiB value（M2 残留清单里"1 MiB 组批无用例"的补充）**：

```
DB 64x1MiB        klen=16 vlen=1048576 n=64 wbs=1073741824 write_ms=758.4 keys_per_s=84 mean_put_us=11850.59 close_rc=0 open_ms=765.5 open_rc=0
DB 64x1MiB DISK_CMD: du -sb /tmp/m3scale/db1mib | cut -f1
67129856
```

⇒ **1 MiB value 可用**（64 条 / 758.4 ms / 均值 11.85 ms/Put）；WAL 67 129 856 B，
比 payload 总和 64 × 1 048 576 = 67 108 864 B 多 **20 992 B**，即每条多 **328 B**
（= 33 个片段 × 7 B 头 = 231 B + batch 头 12 B + key/varint 等）⇒ 与 §9.2 的跨块切分一致：
1 MiB record 被切成 `ceil(1048576 / 32761) = 33` 个片段。**M3 必须沿用"单条目可远大于块大小"的口径**。

**(d) SSTable 块格式原型的真实规模**（探针 `/tmp/m3probe/block_probe.cpp`，**不依赖 liblsm**，
实现一份 LevelDB 风格"前缀压缩 + restart 数组"块编码，只为量尺寸；N = 1 000 000，16 B user key / 100 B value，
key 序列用 `InternalKeyComparator` 同构规则生成并逐条断言严格递增）：

```
$ ./block_probe
== M3 block probe (throwaway) N=1000000 user_key=16 value=100 ==
BLOCKSIZE   1024 restart=16 blocks=112000 entries_per_block=   8.93 data_MB= 109.36 index_KB= 4265.63 file_MB= 113.53 max_block=  1032 idx_entry_bytes= 39.00
BLOCKSIZE   2048 restart=16 blocks= 55800 entries_per_block=  17.92 data_MB= 109.13 index_KB= 2125.20 file_MB= 111.21 max_block=  2060 idx_entry_bytes= 39.00
BLOCKSIZE   4096 restart=16 blocks= 27778 entries_per_block=  36.00 data_MB= 108.51 index_KB= 1057.96 file_MB= 109.55 max_block=  4101 idx_entry_bytes= 39.00
BLOCKSIZE   8192 restart=16 blocks= 13889 entries_per_block=  72.00 data_MB= 108.21 index_KB=  528.98 file_MB= 108.73 max_block=  8174 idx_entry_bytes= 39.00
BLOCKSIZE  16384 restart=16 blocks=  6945 entries_per_block= 143.99 data_MB= 108.06 index_KB=  264.51 file_MB= 108.32 max_block= 16319 idx_entry_bytes= 39.00
BLOCKSIZE  65536 restart=16 blocks=  1728 entries_per_block= 578.70 data_MB= 108.08 index_KB=   65.82 file_MB= 108.14 max_block= 65622 idx_entry_bytes= 39.00
-- restart 间隔对照（block=4096）--
RESTART   1 blocks= 31250 data_MB= 125.05 file_MB= 126.21
RESTART   8 blocks= 28572 data_MB= 109.60 file_MB= 110.66
RESTART  16 blocks= 27778 data_MB= 108.51 file_MB= 109.55
RESTART  32 blocks= 27778 data_MB= 108.02 file_MB= 109.05
-- 尺寸敏感性（block=4096, restart=16）--
VALUE    16 blocks=  1429 entries_per_block= 139.96 data_MB=   5.60 file_MB=   5.65
VALUE   100 blocks=  5556 entries_per_block=  36.00 data_MB=  21.70 file_MB=  21.91
VALUE  1024 blocks= 66667 entries_per_block=   3.00 data_MB= 199.27 file_MB= 201.75
DONE
```

**从这四张表读出的、直接支撑 D1/D3/D4 的数字**：

| 观测 | 数字 | 用在哪 |
|---|---|---|
| 前缀压缩（restart 16）相对不压缩（restart 1）的体积收益 | `125.05 → 108.51 MB` = **−13.2%** | D1 选前缀压缩的依据 |
| restart 16 → 32 的额外收益 | `108.51 → 108.02 MB` = **−0.45%**（几乎为零） | restart=16 是膝点（32 无意义地放大块内线性扫描） |
| block 4096 的 entries/block | **36.00**（16 B key / 100 B value） | Seek 的块内扫描成本上界 |
| block 4096 的索引开销 | **1057.96 KB / 108.51 MB = 0.95%** | 单层索引足够，不需要两级索引 |
| block 1024 的索引开销 | 4265.63 KB / 109.36 MB = **3.9%** | 否决小块 |
| block 65536 的索引开销 | 65.82 KB / 108.08 MB = **0.06%**，但随机 Seek 一次读 64 KiB | 否决大块（读放大换索引变小，不划算） |
| block 4096 的总块数 | **27 778**（100 万 key） | 二级索引阈值、META 规模 |
| SSTable 相对 WAL 的体积比 | `109.55 MB`（block 4096）vs `138.03 MB`（WAL）= **0.79×** | 磁盘占用规划、D6 回收收益 |
| 一次 flush 的 SSTable 规模（4 MiB 写缓冲） | 17 050 条 ⇒ `17 050 × 114 B ≈ 1.94 MB`（≈480 个 4 KiB 块，索引 ≈19 KB） | 每次 flush 的成本量级 |
| 64 MiB 写缓冲的 SSTable 规模 | 272 800 条 ⇒ ≈31 MB，7 580 块，索引 ≈296 KB | 单层索引的**最坏**规模（写入 D4 的上界论证） |

> **口径声明（对齐 `roadmap.md` §3.7「不吹」）**：以上全部是**单机单块 ext4、8 vCPU 的 VM** 数字，
> 只用于本实现内部的**相对**判断（块大小/restart 间隔/索引粒度的取舍），**不构成任何跨机器承诺**。
> (d) 是**原型的字节计数**（不含任何 IO），不是端到端测得的吞吐；本文件不会把它的 MB 数写成 QPS。

### 0.5 顺带发现：M2 的 `Sync()` 水位发布越界（登记为 M3 的前置缺陷）

读 `src/db_impl.cpp`（HEAD `3603696`）逐行核对时发现，不是本阶段探测出来的运行期现象，
但**是 M3 必须处理的设计输入**，故在此登记：

```cpp
// RunFlusher()：取批时就推进了 last_sequence_（持 commit_mu_ + mutex_），Append 在**锁外**
…
    last_sequence_ = begin + static_cast<SequenceNumber>(members.size()) - 1;
    payload = EncodeGroup(begin, members);
  }
  …
  Status s = log_->Append(Slice(payload));      // ← 锁外；此处之前 last_sequence_ 已推进
  if (s.ok() && need_sync) s = log_->Sync();

// Sync()：只持 commit_mu_，fsync 之后把水位发布到"last_sequence_"（含上面那一批！）
Status PersistentDBImpl::Sync() {
  std::lock_guard<std::mutex> ql(commit_mu_);
  const Status s = log_->Sync();
  if (s.ok()) { DbMutexGuard ml(mutex_); durable_seq_ = last_sequence_; }
  return s;
}
```

**缺口**：`Sync()` 依赖的 `durable_seq_` 发布点用的是 `last_sequence_`，而后者在 `Append` **之前**就已推进。
⇒ 存在窗口：某批已分配 sequence、尚未 `Append`，此时并发 `Sync()` 抢先拿到 `commit_mu_`、fsync 当前
（尚不含该批的）文件内容、却把水位发布到**包含该批**的 `last_sequence_`。
`Sync()` 返回 `kOk`，但该批字节此刻可能只在用户态缓冲区里（甚至还没 `write`）。
这违反 `src/db.h` 对 `Sync()` 写死的契约："把此前**所有**已返回 `kOk` 的写入刷到磁盘（**返回即全部 durable**）"。

- **影响面界定（不夸大）**：`WriteOptions::sync = true` 的写**不受影响**——那条路径由 flusher 自己在
  同一临界区里完成 `Append + Sync + 发布`，`durable_before_ack` 仍然成立（M2 的 I11 论证链不断）。
  受影响的是**显式 `Sync()` API**（以及将来任何拿 `durable_seq_` 当 durable 水位的地方，M3 的 WAL 回收判据
  正准备这么做 ⇒ 必须在 M3 落地前解决）。
- **修法（约 3 行）**：水位必须按"**当前 log 实际已追加的边界**"发布，而不是按"已分配 sequence 的边界"。
  即 `Sync()` 记录 `durable_seq_ = log_last_appended_seq_`（由 flusher 在 `Append` 成功后、持 `commit_mu_` 时更新）。
- **处置（已在 M2 闭合，见 §15 R1）**：用户裁决为「单开 M2 补丁」，已于 `a3c85a8` 落地 —— 新增
  `appended_seq_`（`Append` 成功后持 `mutex_` 推进），`Sync()` 先快照该边界、再 fsync、按 `max()`
  单调发布，`RecoverAndOpen` 以同一边界起步；回归用例 `GroupCommit.SyncDoesNotClaimInFlightBatch`
  （RED 原始证据 `docs/m2-tdd-red-i32.log`）。⇒ **M3 不再承担此修复**，`M3-A50` 收窄为「轮转后的
  per-log 边界」回归；`log_last_appended_seq_` 仍保留，但理由是**轮转**（I33/I34 要区分"当前 log"
  与"历史 log"的边界），不再是 M2 缺口的补丁。

### 0.6 探测环境小结（判定这些数字的可信区间）

| 事实 | 值 | 对判据的影响 |
|---|---|---|
| `g++` / `cmake` / GTest | 11.4.0 / 3.22.1 / `/usr/local/lib/libgtest{,_main}.a` | 与原设计一致，判据命令可直接沿用 |
| 文件系统 | `ext4`，`/dev/mapper/vgubuntu-root`，37 G 容量 / 13 G 可用 | `/tmp` 与仓库同盘，B 组真实目录用例的空间够 |
| `loadavg` | 探测期间峰值 `3.41 2.85 1.93`，静载空转约 `0.0x` | §0.4 的耗时数字含探测自身干扰，**只做量级参照** |
| `fsync` 成本 | **不重测**，直接沿用 M2 §11.2 的三方一致结果 `p50 ≈ 2.6 ms`（指令写的 8 ms 已证伪） | M3 的每次 flush 有 2 次文件 fsync + 2 次目录 fsync ⇒ 单次 flush 的落盘下限 ≈ 4×2.6 ms；判据不用绝对吞吐 |
| `kill -9` 语义 | 沿用 M2 §11.3：**产生不了 torn record**，掉电语义只能靠 `MemEnv` 回滚注入 | M3 的"rename 前/注册前崩溃"用例**必须**用 `MemEnv` 或 `FlushHook` 注入点，不能只靠 `kill -9` |

---

## 1. 目标 / 非目标 / 与 M1、M2 的关系

### 1.1 目标（对应 `M3-SSTable与刷盘.md` §0 的 6 条）

| # | 目标 | 落地位置 |
|---|---|---|
| G1 | SSTable 二进制格式：数据块（4 KiB + 前缀压缩 + restart points）、索引块、footer（magic `"LSM1"` + 版本 + index/metaindex handle）、metaindex（**为 M5 的 Bloom 预留**） | §3、§4 |
| G2 | `TableBuilder` / `TableReader`：顺序追加写、`Finish`、块级读取、每块 CRC32C 校验（读时校验默认开、可配） | §5 |
| G3 | flush 路径：MemTable 满 → 冻结进 immutable 列表 → 写 SSTable（**先 durable 再对外可见**）→ 注册进版本 | §6 |
| G4 | 读路径串联：`Get` 依次查 MemTable → immutable → L0 SSTable（**新→旧**），正确实现"新版覆盖旧版"与"tombstone 屏蔽更旧版本" | §7 |
| G5 | 迭代器：`MergingIterator`（多路归并，内部 key 序）+ `DBIter`（sequence 可见性、跳过 tombstone）的最小可用版本 | §7.3 |
| G6 | 启动恢复：读版本元数据重建 SSTable 集合；**SSTable + WAL 组合恢复**（WAL 中比 SSTable 更新的部分继续重放） | §8 |
| G7 |（由 §1.2 边界 5 补齐）**WAL 回收判据落地**：M3 起才有"比 WAL 更持久的结构"，M2 D9 只定义不实现的删除判据在此兑现 | §6.6 |

### 1.2 非目标（M3 硬边界，评审逐条对照）

**M3 实现与测试中不得出现下列任何符号、文件、字段或占位实现（stub 也不行）**：

- **分层 compaction 与层级结构**（`Level`、`L0..Ln` 的显式分层、`Compaction`、`DoCompactionWork`、
  `PickCompaction`、`MaxBytesForLevel`、压实触发/限速）——M4
- **`MANIFEST` / `VersionEdit` 追加日志 / `VersionSet` 版本图 / 版本快照链表 / `CURRENT` 间接指针**——见 §4.1 的边界裁决
- **Bloom Filter** / filter block 的**内容**（M3 只写**空的** metaindex 块并预留 key 空间）——M5
- **WriteBatch 的对外接口**（不得新增 `WriteBatch` 类/头文件/公共 API）——M5
- **块缓存 / LRU block cache** / 压缩算法（Snappy 等）——M4/M5
- **并发 compaction / 多后台线程**（M3 只有**一个**后台 flush 线程）
- **`ReadOptions::snapshot` / `Snapshot*` 公共 API**（M3 的快照恒为 `last_sequence_`；见 D7）
- **`DB::Flush()` 等新增公共 API**（`src/db.h` **不在** M3 的「必须改」清单里；见 §8.7 E4）
- 与 raft-kv 的对接（M6）
- **真删除 SSTable**（M3 只删 `.sst.tmp` 与未注册的 `.sst` 孤儿；已注册的 SSTable 只能整体重写为新文件 ⇒ I21，
  重写是 M4 的事）
- **原地修改已注册的 SSTable 任何字节**（I21）

**必须显式登记的五条边界取舍**（§12.3 自检会再核一遍）：

1. **M2 的 `kFrozen` 语义在 DB 层被"消化"掉**：`MemTable::Add` 的 `kFrozen` **一个字节都不改**（M1 冻结契约），
   但 M3 的 `DBImpl` 写路径**不再把容量不足当拒绝**——改为"冻结旧表 + 换新表 + 本批照常写入"（§6.2）。
   依据：M2 设计 §1.2 边界 1 已明文写"M3 引入 flush 后由 `MakeRoomForWrite` 消化"。
   ⇒ M2 的 `Put/Delete` 返回 `kFrozen` 的**外部行为变化**，必须在 §12.4 登记，并由 §10 的 `M3-A20/A21` 覆盖。
2. **写路径唯一能让写失败的原因是 `bg_error_`（粘性 fail-stop）与 `closed_`**；容量不再是失败原因。
   ⇒ M2 评审阻断项 1（"被拒写复活"）的**整类 bug 在 M3 结构上消失**（§6.2 给出论证）。
3. **M3 只有一个后台线程**（flush）。它**不写 WAL**、**不做 WAL 轮转**（轮转由当前组提交 flusher 做，见 L20）。
4. **M3 不引入 `Options::repair`**：META 损坏/中间损坏一律 `kCorruption` 拒绝启动，不做自动修复。
   与 M2 的尾部截断对称：**尾部残骸（从未被 ack）可截断；结构损坏（已注册状态不可判定）必须拒绝启动**。
5. **WAL 删除（回收）纳入 M3**（G7）。这是 M3 唯一带"数据丢失能力"的新路径，因此：
   (a) 判据只有一个来源（I34）；(b) 默认开启但可用 `Options::recycle_log_files = false` 关闭做对照；
   (c) 必须有**专属**的 B 组用例与 A 组判据（`M3-A45/A46/A47/A48`、`M3-B09`）；
   (d) 删除**永远**发生在"META 已 fsync + rename + SyncDir"之后（I34）。
   这条**需要用户拍板**（§13 Q5）。

### 1.3 与 M1 / M2 的关系

#### 1.3.1 逐字复用（不改一行）的既有契约

| 契约 | 位置 | M3 如何用 |
|---|---|---|
| `Slice` / `Status`（含 `kCorruption`/`kNotSupported`/`kFrozen`） | `common.h` | SSTable 读写接口一律走 `Slice`；`kCorruption` 成为"读磁盘块失败"的**唯一**对外码 |
| **内部 key 编码与比较**（`PackTrailer` / `BuildInternalKey` / `ExtractUserKey` / `ParseInternalKey` / `InternalKeyComparator` 的"user key 升序 + trailer 降序"） | `common.h`、`protocol.md` §6 | **SSTable 里存的就是 internal key**（§3.2），块内排序、索引二分、归并全靠它；**M3 一个字都不改** |
| `kMaxSequenceNumber` / `ValueType` / `kTypeDeletion` / `kTypeValue` / `kValueTypeForSeek` / `kMaxUserKeySize` / `kInternalKeyTrailerSize` / `kInternalKeyMinSize` | `common.h`、`protocol.md` §1/§6 | 块解析的边界校验（`>= kInternalKeyMinSize` 的 MUST 在 SSTable 侧**同样成立**，输入同样来自不可信来源——磁盘） |
| `BuildLookupKey(user_key, snapshot)` | `common.h`、`protocol.md` §6.2 | `Table::Get` / `Table::Seek` 的探针构造口径与 MemTable 完全一致 |
| `Comparator` / `BytewiseComparator()` / `InternalKeyComparator::user_comparator()` | `common.h` | **META 持久化 `comparator->Name()` 并校验**（兑现 M2 D13 残留，§8.1） |
| `Status` 的失败必带上下文 | `m1-design.md` §4.2 | 每个 SSTable 失败都带 `文件 + 偏移 + 块类型`（可定位，评审按此核对） |
| `Arena` | `util/arena.h` | MemTable 内部；M3 **不**改 Arena。TableBuilder 的块缓冲用 `std::string`（与 M2 组提交缓冲同口径：需要可增长、不随容器释放） |
| `Env`（`WritableFile`/`SequentialFile`/`GetChildren`/`RenameFile`/`GetFileSize`/`RemoveFile`/`Truncate`/`LockFile`） | `util/env.h` | SSTable 层**不直接依赖 POSIX**，全部经 `Env` 注入（崩溃注入用例可确定性复现的**前提**，指令 §3 明文要求） |
| `coding`（varint32/64、fixed、length-prefixed） | `util/coding.h`、`protocol.md` §2~§4 | 块内 entry 的 `shared/non_shared/vlen` 全部用 `PutVarint32`/`EncodeVarint32`；**不新增第二套 varint** |
| `crc32c::Value/Extend` | `util/crc32c.h`、`protocol.md` §5 | 块尾 CRC 与 footer/META 的 CRC 唯一入口；口径与 WAL **完全一致**（§0.3 已实证） |
| `MemTable::{Add, Get, NewIterator, Freeze, WouldReject, ApproximateMemoryUsage, write_buffer_size, internal_comparator}` | `memtable.h` | flush 的唯一数据源；`WouldReject` 是**唯一**容量判据（M2 评审教训，§6.2 复用） |
| `Skiplist::Iterator` 的双向能力（`SeekToFirst/SeekToLast/Seek/Next/Prev`） | `skiplist.h` | `DBIter` 的双向语义需要它；M3 不改跳表 |
| `Iterator` 三态状态机 + `key()` 生命周期契约 | `common.h`、`m1-design.md` §4.4 | `DBIter` **保持逐字相同的语义**（含 `Prev`/`SeekToLast`），且把 `key()` 的失效边界从"M1 的一次定位调用"扩展为"**下一次定位调用**"不变 |
| `WALWriter` / `WALReader` / `WALScanVerdict` / `WALScanResult` / `WALHasValidFragmentAfter` | `wal.h`、`protocol.md` §9 | 恢复期重放 WAL 尾巴的**唯一**入口；M2 的"尾部截断 vs 中间损坏"判定**原样沿用** |
| `RecoveryStats`（含 `records_skipped` / `tail_truncated_bytes`） | `db_impl.h` | M3 **只增不改**字段（§8.4），并可观测性口径与 M2 一致 |
| `CommitHook`（`OnBeforeGroupAssemble` / `OnGroupTaken` / `OnAfterSyncBeforePublish`） | `common.h` | M3 沿用；组提交语义不变 ⇒ A20/A21/A22/A24 全部继续成立 |
| `FileLock`（`LOCK` 进程级独占） | `util/env.h` | M3 恢复流程沿用，`ScopedFileLock` 模式不变 |
| I1~I10 / L1~L6 / I11~I20 / L7~L12 | `m1-prerequisites.md` §1/§2、`m2-prerequisites.md` §1/§2 | **全部继续成立**；M3 只**追加**（§9） |

#### 1.3.2 被扩展的接口（"只增不改"逐条列出）

| 接口 | 变化 | 为什么必须 |
|---|---|---|
| `Env` | **新增** `RandomAccessFile` + `NewRandomAccessFile`（随机偏移读，无 seek 状态） | SSTable 的块级读取必须能"按 handle 读一段"，`SequentialFile` 无此能力（`Skip` 是顺序前进，不能回退） |
| `Env` | **新增** `SyncDir(const std::string& dir)` | **rename 的持久性依赖它**。M2 设计 §5.7 声明过、实现从未落地（§0.1 W3 恰好暴露了这点）。没有它，"rename 之后注册"**不构成**任何持久性保证（§6.3） |
| `Options` | **新增** `block_size=4096`、`verify_checksums=true`、`max_open_files=64`、`recycle_log_files=true`、`flush_hook=nullptr` | 前三个是 D1/D3/D8 的落点；第四个是 §1.2 边界 5 的对照开关；第五个是 A/B 组崩溃注入与"顺序断言"的 seam（与 `CommitHook` 同纪律：生产为 `nullptr`） |
| `Options::write_buffer_size` | **语义澄清（不改字段）**：它是"目标容量"，不是"硬上限"；`MemTable::WriteBufferSize()` 仍是唯一判据 | 见 §6.2：新表的容量取 `max(write_buffer_size, 触发批的预估占用 + slack)`，否则"单条大于写缓冲"会把库打成粘性只读（M2 评审优化项 2 的同类风险） |
| `filename.{h,cpp}` | **新增** `TableFileName`(`%06u.sst`)、`TempFileName`(`%06u.sst.tmp`)、`ParseTableFileName`、`ParseTempFileName`、`MetaFileName`(`META`) | 文件号空间的唯一命名入口；`.tmp` 与 `.sst` 的区分是"半边文件永不注册"的**结构性**保证（§6.3） |
| `db_impl.{h,cpp}` | **必要扩展**：读路径串联、flush 状态机、单后台线程、恢复接入 SSTable + 残余 WAL、`immutables_`/`version_`/`log_number_` 状态、`FlushStats` 诊断 | 指令 §1 允许的扩展面 |
| `memtable_` 的持有方式 | `std::unique_ptr<MemTable>` → `std::shared_ptr<MemTable>`（含 immutables） | **L19/L21 的前提**：`Get`/迭代器必须能在**锁外**做 SSTable IO，因此需要"取引用后再放锁"（§7.1） |
| `MemEnv` / `FaultyEnv` | 补 `RandomAccessFile`、`SyncDir`、rename 的目录语义、**文件句柄计数** | `Env` 加纯虚方法后它们必须实现；句柄计数探针是指令 §2 B 组"文件句柄计数与泄漏检查"的 seam |
| `CMakeLists.txt` | 新增源文件与测试目标（**保留 M1/M2 全部目标不变，不用 GLOB**） | 工程约定 |
| `.gitignore` | 新增 flush 崩溃脚本的临时目录 | 工程约定 |

**明确声明：本设计不修改** `src/skiplist.h`、`src/memtable.{h,cpp}`（含 `WouldReject` 的实现）、
`src/common.h` 的内部 key 编解码与比较规则、`docs/protocol.md` §1~§9 的任何一行、
M1/M2 既有测试的任何断言。

### 1.4 依赖方向（单向，禁止反向）

```
src/common.h + util（coding/crc32c/arena/env）
      ^
src/sstable/*        format / block_builder / block / footer / table_builder / table
      ^              ← 只依赖 common.h + util + protocol.md 的编码；**禁止** include version_*/db_*
src/version_*        version_edit / version_set（M3 简化版：只维护 SSTable 列表 + 元数据持久化）
      ^              ← 依赖 sstable（文件名/handle 类型）+ common.h；**禁止** include db_*
src/merging_iterator.* / db_iter.*
      ^              ← 依赖 common.h + Iterator；**禁止** include db_*
src/db_impl.*        ← 依赖以上全部
```

- **硬规则**：`src/sstable/` 下任何文件**不得** include `version_*`、`merging_iterator`、`db_iter`、`db_impl`、`wal.h`。
  理由：SSTable 层要能被 M4 的 compaction 独立复用（compaction 只组合 `TableBuilder` + `TableReader`），
  且它不得知道"版本""层次""DB"的存在。这条由 `#4` 评审按 include 图逐条核对。
- **硬规则**：`src/sstable/` 与 `merging_iterator`/`db_iter` **不得**出现任何 POSIX 头（`<fcntl.h>`/`<unistd.h>`/`<dirent.h>`）——
  全部 IO 经 `Env`。这条由 CMake 的**独立目标**在机制上保证：`#1` 必须把 `lsm_sstable` 拆成
  只链接 `util` 的静态库（详见 §11 的 M3.1 证据命令），这样"越权 include"会**链接失败**而不是靠人盯。

### 1.5 M3 不变量与锁纪律的映射（I21~I34 / L13~L21 预告）

`M3-SSTable与刷盘.md` §1 的 `#1` 段要求新增 I21~I30 与 L13~L18。它们**正式定义**在
`docs/m3-prerequisites.md`（`#1` 产物），但设计必须现在就说清"每条由谁保证、在哪一节落地、由哪个用例验证"。
本文档给出的映射在 **§9**（含 4 条建议追加的 I31~I34 与 3 条追加的 L19~L21）。

---

## 2. 开放决策记录（方案 → 取舍 → 推荐）

> 格式对齐 `docs/m2-design.md` §2：每条给 2~3 个方案、量化的取舍、明确推荐，并标注是否需要用户拍板。
> 涉及尺寸的取舍**一律引用 §0 的实测数字**，不凭空给理由。

### D1 文件格式与块大小 【需拍板】

| 方案 | 布局 | 取舍（用 §0.4(d) 的数字） |
|---|---|---|
| **A（推荐）** | **4 KiB 数据块 + 块内前缀压缩（restart 间隔 16）+ 每数据块一个索引项（单层索引）** | 索引开销 **0.95%**；随机 Seek 一次读 **1 个 4 KiB 块**（36 条/块）；前缀压缩省 **13.2%**；块内二分 + 至多 16 条线性扫描。实现量中等，且与 M4 compaction 的顺序块迭代**天然复用** |
| B | 不压缩定长条目 + 每块一个索引项 | 体积 **+15.2%**（`125.05 vs 108.51 MB`）；实现略有简化（去掉 restart 语义）。但 M4 一定要前缀压缩（否则 compaction 的写放大更差）⇒ **M3 省下的复杂度 M4 要还回去** |
| C | 整文件单块 | Seek 必须把**整个 SSTable**读进内存（4 MiB 写缓冲 ⇒ ≈2 MB/次 Seek）；前缀压缩的 restart 语义退化为"全文件 restart=1"⇒ 体积最差。**否决** |
| A' | A + 两级索引（data index + index-of-index） | 单层索引的最坏开销实测只有 **296 KB**（64 MiB 写缓冲 ⇒ 31 MB SSTable / 7 580 块，§0.4(d)）⇒ **两级索引在 M3 的规模下没有收益**，纯增复杂度。**留给 M4/M5**（触发阈值写在 §3.3） |

**推荐 A**（4 KiB / restart 16 / 单层索引）。`block_size` 可配但默认 4096；`Options` 里**不出现**两级索引开关
（避免"看起来能用但没实现"的字段——M1 §4.3 的纪律）。

### D2 footer 布局 【需拍板】

| 方案 | 布局 | 取舍 |
|---|---|---|
| **A（推荐）** | **定长 44 B**：`magic(4) ‖ version(4) ‖ index_handle(16) ‖ metaindex_handle(16) ‖ footer_crc(4)`，handle 用**定宽** `offset(8 LE) ‖ size(8 LE)` | 读取只需 `seek(size-44)`；每个字段可**独立**校验；无 varint 歧义 |
| B | LevelDB 风格：varint handle + padding 到 40 + magic(8) = 48 B | 省不到 10 B，却让 `handle` 的编码在"varint + padding"里变得**难以独立校验**（截断与越界只能间接推断）。**否决** |
| C | 全部元数据放进元数据块，footer 只留 magic + 指向元数据块的 handle | footer 仍需定长（要能 `seek` 到）；等于把 index handle 挪进元数据块，**多一次 IO 且多一层间接**。**否决** |
| — | **是否为 M5 的 filter block 预留 metaindex** | **推荐预留**（指令原文推荐）。M3 写**空的** metaindex 块并注册其 handle；M5 只新增一个 filter 块 + 让 metaindex 多一条 `"filter.leveldb.BuiltinBloomFilter2" → handle`，**footer 布局零改动、`kTableFormatVersion` 不升**（§3.4 给出理由与验证方式） |

**推荐 A + 预留 metaindex。** 追加 `footer_crc` 的理由见 D3（footer 里的 handle 损坏 = 随机跳读，必须有校验）。

### D3 校验 【需拍板】

| 方案 | 覆盖面 | 取舍 |
|---|---|---|
| **A（推荐）** | **每块 CRC32C，存在块尾（4 B LE）；覆盖 `length(4B LE) ‖ type(1B) ‖ payload`**；读时校验**默认开**，`Options::verify_checksums=false` 可关；**footer 与 META 各自也带 CRC** | 与 §0.3 实证的 WAL 口径**同形**（CRC 含长度）；单块读即可校验，能**定位**到块；`length` 损坏被 CRC 抓住 ⇒ **不会越界读**（正是指令"校验覆盖面是否含块头"的要求） |
| B | 整文件 CRC | 每次随机读都要读全文件才能校验 ⇒ 与"块级随机读"的目标冲突；且**无法定位**损坏块。**否决** |
| C | 每块 CRC 只覆盖 `payload` | `length` 或 `type` 损坏时，**先用错长度取到错 payload，再"碰巧"CRC 失败**——M2 §9.3 已明确否决这种**间接推断**。**否决** |

**推荐 A。** 三条配套纪律：
1. `verify_checksums=false` **不是**"跳过解析校验"：结构字段（`length`/`type`/`shared`/`non_shared`/`vlen`/restart 数组）
   的合法性校验**永远执行**（否则是 M1 评审阻断项 2 的同源越界读）。关掉的**只有** CRC 计算。
2. 校验失败一律 `kCorruption` + `文件 + 块偏移 + 块类型 + 期望/实际 CRC`，**绝不返回错值**。
3. 一次读里 CRC 计算的次数被 `Options::verify_checksums` 控制，A 组必须有"开/关都正确、且关掉时**确实**不报错"的对照用例
   （`M3-A15`），否则开关是死代码。

### D4 版本元数据 【需拍板 —— 本设计最大的一条，因为指令内部自相矛盾】

**指令的矛盾**：`M3-SSTable与刷盘.md` §0 的「非目标」与 roadmap §2 写"**M3：不得出现分层与 MANIFEST**"，
但同一份指令的 §1「必须新增」清单里列了 `src/version_edit.{h,cpp}`、`src/version_set.{h,cpp}`
（注："M3 可先做简化版：只维护 L0 文件列表 + 元数据持久化"），§0 第二步的决策 4 又要求
"MANIFEST vs 启动扫目录重建 vs 简化 meta 文件"三选一。**三者不可同时成立**，必须显式裁决。

| 方案 | 形态 | 取舍 |
|---|---|---|
| **D（推荐）** | **单快照元数据文件 `META` + 原子 rename**：无 `MANIFEST`、无 `VersionEdit` 追加日志、无 `VersionSet` 版本图、无 `CURRENT` 间接指针、无层级。每次 flush 注册时**整体重写** `META`（写 `META.tmp` → fsync → rename → **SyncDir**）。内容见 §8.1 | ① 字面满足"M3 不得出现 MANIFEST"（没有 MANIFEST 文件、没有编辑日志、没有层级）；② 兑现"元数据持久化"与"崩溃一致性"；③ **删除掉整整一类失效模式**（追加日志的尾部撕裂、CURRENT 切换窗口、版本图回收）；④ 成本可算：每次 flush 重写一个 `≈ 12 + F×(28+2×key)` 字节的文件，F = L0 文件数。F = 59（§0.4 推算）时 ≈ **2.0 KB + 1 次 rename + 1 次 SyncDir**，相对该次 flush 的 2 次文件 fsync 可忽略 |
| A | LevelDB 风格 `MANIFEST-<n>` + `VersionEdit` 追加 + `CURRENT` 原子切换 + `VersionSet` 版本图 | 面向 M4（几十万次 compaction 编辑）的必要形态；**在 M3 的规模下纯属过度设计**，且直接违反 roadmap §2 的红线。M3 若上它，`#4` 评审按"M3 不得出现 MANIFEST"一票否决 |
| B | **启动扫目录重建**（无元数据文件，`Open` 时把目录里所有 `.sst` 当作数据） | 简单，但**结构上无法区分"已注册文件"与"孤儿/残片"** ⇒ 直接违反 M3 硬性约束 6（读路径必须容忍"文件存在但未被版本引用"）与 I27（半写/未注册文件不得进入版本）。**否决 —— 这是决定性理由，不是偏好** |
| C | 简化 meta 文件但**保留 `CURRENT` 间接指针**（`META-<n>` + `CURRENT`） | `CURRENT` 在 LevelDB 里的作用是"避免原地重写 MANIFEST、允许旧 MANIFEST 留存"。**单快照设计不需要它**（rename 已是原子的），多一层指针只多一个损坏点。**否决** |

**推荐 D**，并**把与 M4 的迁移路径写死**，避免 M4 被 M3 的简化版卡住：

1. `META` 的**语义**定义为一个 `VersionEdit` 的**完整快照**（"当前版本 = 这些文件 + 这些水位"），
   而不是"差分"。⇒ M4 想改成 `MANIFEST` 追加日志时，只需把"写全量快照"换成"写一条全量 `VersionEdit`"，
   **`Version` 的内存表示、`Open` 的恢复入口、文件命名、SSTable 格式全部不动**。
2. `Open` 的恢复入口签名为 `static Status RecoverVersion(Env*, const std::string& dbname, Version** out, RecoveryStats*)`，
   **不暴露"META 是文件还是日志"**。M4 换实现只改这一个函数体。
3. `Version` 的接口**不出现** `level`/`LevelFiles`/`compaction` 字样：M3 只有 `files()`（一个按文件号降序的列表）
   与 `min_log_number_to_keep()`/`log_number()`。M4 把它扩成 `level_files(int)` 时**不需要改** `files()` 的调用方。
4. **`#1` 必须把这条"迁移路径"登记成一条显式约束**（M4 不得因为要加 `MANIFEST` 而回头改 M3 的 SSTable 格式或文件名规则）。

### D5 flush 并发模型 【需拍板，指令建议单后台线程】

| 方案 | 形态 | 取舍 |
|---|---|---|
| **B（推荐，指令建议）** | **冻结（锁内、纯内存）+ 单后台 flush 线程（锁外 IO）** | ① 冻结是纯内存操作 ⇒ 满足 L18（锁内零 IO）；② 落盘（write/fsync/rename/SyncDir）全在 DB 锁外；③ **`sync=true` 的写尾延迟与 SSTable 落盘时间解耦**（若同步 flush，触发冻结的那一批要额外背一次 ≈2 MB 写 + 2 次 fsync + 2 次 SyncDir ≈ 10 ms）；④ M4 正是这个线程 + `immutables_` 列表，**M3 落地骨架 = M4 不用重写整条写路径**（指令原文点名的理由） |
| A | MemTable 满即**同步** flush（阻塞写） | 实现最短（几十行），但：① 触发冻结的那一批尾延迟 = 整个 SSTable 的落盘成本（`p99` 恶化，且与 `write_buffer_size` 成正比）；② **M4 必须重写整条写路径**（从"写者自己 flush"改成"后台线程 + immutable"）⇒ 与本流水线"每步一个可实测判据、最小改动"的纪律冲突。**否决** |
| — | 额外复杂性（必须一并设计，不能留给 M4） | 有界 `immutables_`（`kMaxImmutableMemTables = 2`）+ 满时写者**停等**（不是返回 `kFrozen`）；`shared_ptr` 引用计数（读路径锁外使用）；关闭顺序。这些在 §6/§7/§9 逐条给出 |

**推荐 B。** 停等的上界可算：一次 flush ≈ `write .sst`（≈2 MB）+ `fsync` + `rename` + `SyncDir` + `META` 重写
+ `SyncDir` ⇒ 用 M2 §11.2 的 `fsync p50 ≈ 2.6 ms` 估 **4 次 fsync 类操作 ≈ 10.4 ms + 2 MB 顺序写**。
`kMaxImmutableMemTables = 2` 意味着**最多连续两次 flush 才停等**（≈21 ms），这是可接受的尾延迟上界，
且必须由 `M3-A25` 用注入的慢 flush 确定性验证（不靠调度赌）。

### D6 WAL 回收判据 【需拍板，含数据丢失能力】

| 方案 | 形态 | 取舍 |
|---|---|---|
| **B（推荐）** | **计算并持久化水位 + 真删除**：`min_log_number_to_keep`（I34）写进 `META`，删除**严格小于**它的 `.log`；默认 `Options::recycle_log_files = true`，可关 | ① 不实现删除 ⇒ **"flush 降低了恢复成本"这句话在 M3 无法被证伪也无法被证实**（恢复仍要重放全部 WAL）；② 100 轮 kill -9 门禁会累积日志，**"纯 SSTable 启动"这条验收口径失去可测性**（要靠测试手工删文件，是弱化版证据）；③ 删除代码本身 ≈15 行，但判据已在手上，**先 durable 再删**的顺序论证清晰（§6.6）；④ **风险集中在"判据算错"**，而判据是单一来源 + 一条可证的不变量（I34）⇒ 用专属 A/B 用例封住，比混进 M4 与 compaction 一起上更安全 |
| A | 保守标记不删（只记录可回收水位，`MaybeDeleteObsoleteLogs()` 恒空实现） | 最安全，但 ②③ 的代价都在：M3 的"更持久结构"就只是"多写了个文件"，磁盘占用随 flush 数**单调增长**（100 万写 / 4 MiB 缓冲 ⇒ 59 次 flush，WAL 仍是 138 MB 全量留存）⇒ 与"M3 起 D9 的删除判据在此落地"的指令原文不符 |
| C | 删除但**不持久化**水位（每次 `Open` 重算） | 重算需要"每个 log 里所有 record 都被某个已注册 SSTable 覆盖"的判定 ⇒ 要么扫 log（贵），要么**只能靠文件号序**——而文件号序的单调性依赖"META 记录了当时的 log_number"，不持久化就没有依据。**否决** |

**推荐 B**（默认开 + 可关 + 专属用例）。`M3-A45/A46/A47/A48` 覆盖判据本身，`M3-B09` 覆盖磁盘效果，
`M3-B01` 在"小写缓冲强制 flush + 真回收"的条件下重跑 `missing 0`。

### D7 迭代器职责划分 【需拍板，指令建议引入最小快照】

| 决策点 | 方案 | 推荐 |
|---|---|---|
| `MergingIterator` vs `DBIter` 边界 | **`MergingIterator` 只做归并**（N 路 child，按 `InternalKeyComparator` 全序输出**内部 key**，含同 key 的多个 sequence 与 tombstone，**不做任何可见性判断**）；**`DBIter` 只做用户视图**（sequence 过滤 `seq <= snapshot`、每 user key 只出最新可见版本、跳过 tombstone、internal→user key 转换、三态状态机） | 二者职责互斥、可独立测试（A 组 `M3-A31` 只测归并序，`M3-A32` 只测可见性）。**推荐** |
| 归并实现 | 线性扫描 N 个 child 取最小 vs 最小堆 | **线性扫描**：M3 的 child 数 = 1（memtable）+ ≤2（immutable）+ F（L0 文件，实测 F ≈ 59，§0.4 推算）⇒ 每次定位 O(N) 次比较（59 次 int 比较 ≈ 纳秒级），**远小于它省下的那次 4 KiB 块读**。最小堆是 M4 层级变多之后的事，且接口不变即可替换（`#1` 登记为 M4 的替换点）|
| 读快照 sequence | 引入**最小版本**：`snapshot = last_sequence_`（在 `mutex_` 内取一次），`DBIter` 持住它 | **推荐引入**（指令建议）。理由：① 不做的话 `DBIter` 只能假设"全部可见"，M4 的一致性根本无从谈起（M4 要在此基础上加 `Snapshot*`）；② 成本 = 一个 `SequenceNumber` 成员 + 一个 `seq <= snapshot` 判断；③ 它同时是 `Get` 的探针构造口径（`BuildLookupKey(key, snapshot)`），二者**同源**，不会漂移 |
| 迭代器方向 | 全双向（`SeekToFirst/SeekToLast/Seek/Next/Prev`） vs 只向前 | **全双向**。理由：`Iterator` 接口在 M1 冻结且 `Prev`/`SeekToLast` 有 M1 用例覆盖；持久模式若退化为"只向前"，`NewIterator()` 的语义就**随模式而变** ⇒ 契约分裂。成本：`Block::Iterator::Prev` 需要"回到当前 restart 组起点再前扫"（块内 O(restart_interval)），`Table::Iterator::Prev` 需要"退到前一数据块再 `SeekToLast`"，都是标准做法（§5.4）|

### D8 读路径缓存与读放大统计口径 【需拍板】

| 决策点 | 方案 | 推荐 |
|---|---|---|
| table cache（已打开文件句柄缓存） | 有界 LRU（`Options::max_open_files`，默认 64） vs 不缓存（每次 `Get` 开/关文件） vs 无界 map | **有界 LRU**。理由：① 不缓存 ⇒ 每次 `Get` 打开 F ≈ 59 个文件（§0.4）+ `open/close` 系统调用，尾延迟和 fd churn 都不可接受；② 无界 ⇒ 违反 I30（句柄有上限）且必然泄漏；③ **64 的上界不靠猜**：M3 一个 DB 的 SSTable 数在 100 万 key / 4 MiB 缓冲下 ≈ 59，64 覆盖整库；超出时 LRU 淘汰**最久未用**的（淘汰只在锁外 `close`，见 L19）。**M3 不做块缓存**（指令允许留 M4/M5，且块缓存会把 LRU 的键从"文件号"变成"(文件号, 块偏移)"，是一块独立的设计面） |
| 读放大的统计口径（"每层检查文件数、每文件读取块数、命中位置"） | 定义为 `ReadStats{ files_checked, key_range_skipped, index_blocks_read, data_blocks_read, bytes_read, crc_checked, crc_failed, hit_layer }`，`hit_layer ∈ {memtable, immutable, sstable}` | **定义如上**，由 `PersistentDBImpl::GetReadStats()` 只读暴露（与 `GetRecoveryStats()` 同纪律：诊断、不改行为）。**口径必须写死**，否则 M4 的三个放大数字与 M3 无法对比：`files_checked` 只数**真的进了 `Table::Get`** 的文件（被 key range 过滤掉的不算，另计 `key_range_skipped`）；`bytes_read` 含块头/CRC/restart 数组的**全部**字节（不是只算 payload）；MemTable/immutable 不计入 `files_checked` |
| 读放大的**预期量级**（M3 基线，供 M4 对比） | 100 万 key、4 MiB 写缓冲 ⇒ F ≈ 59 个 SSTable，每个 ≈17 050 条 / ≈474 个 4 KiB 块 / 索引 ≈19 KB。对一个**最老的** key：`files_checked = 59`，`index_blocks_read = 59`，`data_blocks_read = 59`，`bytes_read ≈ 59 × (18.5 KB + 4 KiB) ≈ 1.33 MB` | 记为**推算值**（由 §0.4(d) 的每块字节数推得，非端到端实测），由 `M3-B08` 用 `ReadStats` 实测并入库。**这正是 M4 compaction 的动机数字**（面试四问 #2 的分母） |

---

## 3. SSTable 位级格式

> **本节与 `docs/protocol.md` §10 必须逐字一致**（§4 给出追加 patch 的完整文本）。评审逐字对照检查。
> 所有多字节整数**小端（LE）**，逐字节拼装/解析，**禁止** `reinterpret_cast` 到整型指针（`protocol.md` §1）。

### 3.1 文件命名与文件号空间

```
<dbname>/
  LOCK                进程级独占（M2 的 D10，不变）
  META                版本快照（M3 新增；§8.1）——**不是** MANIFEST
  META.tmp            META 的写临时文件（rename 的源）
  %06u.log            WAL（M2）
  %06u.sst            SSTable（已注册，不可变）
  %06u.sst.tmp        SSTable 的写临时文件（**永不注册**）
```

| 规则 | 内容 | 理由 |
|---|---|---|
| 编号格式 | `%06u` 十进制零填充 | 与 M2 的 `.log` 同口径；字典序 == 数值序（但解析**一律按数值**，不靠字符串序） |
| 编号空间 | **`.log`/`.sst` 共享一个单调递增的 `next_file_number`** | 单一编号源 ⇒ "目录里任何编号不在 `META` 引用集里、也不是当前 log 的文件 = 孤儿"，孤儿判定不需要第二套规则 |
| 编号来源 | `META.next_file_number` 只是**提示**；`Open` 时权威值 = `max(META.next_file_number, max(目录中已存在的编号) + 1)` | 让"分配文件号"**不需要**立刻落盘（否则每次分配一次 `META` 重写）；崩溃后编号绝不重用 |
| 首次编号 | 空库 ⇒ `next_file_number = 1`，第一个 log 拿到 `1`（`000001.log`）⇒ **与 M2 逐字一致** | 兼容 M2 老库 |
| 正则 | `.log` 用 `^[0-9]{6}\.log$`；`.sst` 用 `^[0-9]{6}\.sst$`；临时文件 `^[0-9]{6}\.sst\.tmp$` | `META` / `LOCK` 不参与编号解析 |
| **`ParseTableFileName` 必须拒绝 `.sst.tmp`** | 后缀匹配必须**精确**比较 `.sst`，不得用 `starts_with` | 否则 `.tmp` 会被当成已注册表读入 ⇒ 半个文件进版本（最危险的失效模式） |

### 3.2 数据块（data block）

```
data_block_payload := entry* || restart_offset[uint32 LE] * restart_count || restart_count(uint32 LE)
entry              := varint32(shared) || varint32(non_shared) || key_delta[non_shared] || varint32(value_len) || value
key                := internal_key（protocol §6：user_key || trailer(8B LE)）
```

| 字段 | 字节数 | 字节序 | 取值 / 约束 |
|---|---|---|---|
| `shared` | varint32（1..5） | LE（可变长） | `0 .. 上一条 key 的长度`；**restart 点必须为 0**；否则 `kCorruption` |
| `non_shared` | varint32（1..5） | LE | `>= 1`；`shared + non_shared <= kMaxUserKeySize + 8`；且 `shared+non_shared <= 剩余字节`，否则 `kCorruption`（**先校验再读**） |
| `key_delta` | `non_shared` | — | `key = 上一条 key[0..shared) ‖ key_delta`；**restart 点**时 `上一条 key` 视为空 ⇒ `key = key_delta` |
| `value_len` | varint32（1..5） | LE | `0 .. 剩余字节`；tombstone（`type == kTypeDeletion`）时**必须为 0** |
| `value` | `value_len` | — | 原始字节；空 value 合法 |
| `restart_offset` | 4 | LE | 块内每个 restart 点的 entry **起始偏移**；`restart_offset[0] == 0`；**严格单调递增**（I24）；`< payload - 4*(restart_count+1)` |
| `restart_count` | 4 | LE | `>= 1`（空块的 restart 数组恒为 `[0]`，见下） |

**常量**：`kRestartInterval = 16`（每 16 条 entry 打一个 restart 点，**第一条 entry 必然是 restart 点**）。

**restart 点的语义（一份，不许有第二种解释）**：
- 每个 restart 组内**第一条** entry 的 `shared = 0`，`key_delta` = 完整 internal key。
- 组内后续 entry 的 `shared` = 与前一条 key 的**最长公共前缀**（可以大于上一条的 `shared`——因为 `上一条 key` 是**已解码的完整 key**，不是 `key_delta`）。
- **【组间不共享前缀】**：新 restart 组的第一条 `shared` 恒为 0，即使它与上一组最后一条有公共前缀。
- `last_key` 在遇到 restart 点时**重置为空**。⇒ 解码器**必须**从某个 restart 点开始解，不能从块中间任意字节开始（这正是"块内 Seek 必须从 restart 点起步"的根因）。

**空数据块**：合法。`payload = restart_offset[0]=0 (4B) || restart_count=1 (4B)` = **恰好 8 字节**。
（M3 的 TableBuilder 对**空表**不写数据块——空表的索引为空；但 `Block::Iterate` 与块校验器必须能处理这个 8 字节形态，
因为 M5 的 filter 块与一些边界用例会出现它。`M3-A01` 覆盖。）

**单条 entry 大于 `block_size`**：合法且必须支持。判据是"**加上这一条之后**是否超过 `block_size`"，
超了就**先封块再写**；因此一个块最多比 `block_size` 超出"一条 entry 的大小"（实测：4 KiB 块的最大块字节数
`max_block = 4101`，§0.4(d)）。⇒ 数据块大小**不是**硬上限，`block_size` 是**目标值**
（与 `Options::write_buffer_size` 同性质；口径单一来源：`BlockBuilder::EstimatedSizeAfter`）。

### 3.3 索引块（index block）

```
index_payload := index_entry* || restart_offset[uint32 LE] * n || n(uint32 LE)
index_entry   := varint32(internal_key_len) || internal_key || handle(16B)
handle        := offset(8B LE) || size(8B LE)
```

| 字段 | 字节数 | 字节序 | 语义 |
|---|---|---|---|
| `internal_key_len` | varint32 | LE | `>= kInternalKeyMinSize`(=8)；否则 `kCorruption` |
| `internal_key` | `internal_key_len` | — | **= 该数据块内最后一条 entry 的完整 internal key** |
| `handle.offset` | 8 | LE | 目标数据块的**起始**文件偏移（指向块头，不是 payload） |
| `handle.size` | 8 | LE | 目标块的**总字节数** = `kBlockHeaderSize + payload_len + kBlockTrailerSize` |

**索引 key 语义（写死，不许含糊）**：索引项 `i` 的 key = **数据块 `i` 的最后一条 internal key**。
⇒ `Table::Seek(target)` = 在索引块上找**第一个 key `>= target`** 的索引项（`>=`，不是 `>`），读该块，在块内 `Seek`。
**为什么不用"缩短的分隔 key"**（LevelDB 的 `FindShortestSeparator`）：索引只有 **0.95%** 的体积（§0.4(d)），
缩短收益约 39 B → 30 B/块（≈0.2% 总体积），却引入一个经典的"分隔 key 算错 ⇒ Seek 落到前一个块 ⇒
漏掉目标"的 bug 类。**留作 M4/M5 的优化项**，`#1` 登记。

**索引块的 `restart_interval` = 1**（`kIndexRestartInterval = 1`）。理由：
(a) 索引需要**精确**的二分定位（restart=1 时块内 `Seek` 是纯二分，零线性扫描）；
(b) 代价可算：前缀压缩在数据块上省了 **13.2%**（§0.4(d)），索引本身只占 0.95% ⇒ 放弃它损失的总体积 ≈ **0.1%**。

**单层索引，无两级索引。** 上界实测/推算（§0.4(d)）：

| 写缓冲 | 条目数 | SSTable ≈ | 块数 | 索引 ≈ |
|---|---|---|---|---|
| 4 MiB（默认） | 17 050 | 1.94 MB | ≈480 | ≈19 KB |
| 64 MiB | 272 800 | 31 MB | 7 580 | ≈296 KB |
| 256 MiB | 1 091 200 | 124 MB | 30 320 | ≈1.2 MB |

⇒ 单层索引在这三个量级下都只需**一次连续读**即可全部载入。**登记阈值**：当 `block_count > 65536`
（索引 > 2.5 MB）时，`Finish` 打一条 **WARN 并计数**（`index_size_warn`），**不阻断**——
提醒 `#1`/M4 该上两级索引，而不是让实现悄悄变得不可用。

### 3.4 元数据块（metaindex block）与 M5 的 filter 预留

```
metaindex_payload := meta_entry* || restart_offset[uint32 LE] * n || n(uint32 LE)
meta_entry        := varint32(name_len) || name || handle(16B)     // name = 普通 UTF-8/ASCII 字符串，**不是** internal key
```

- **M3 写一个空的 metaindex**（`payload` = 8 字节：`restart_offset[0]=0` + `n=1`），并在 footer 里注册其 handle。
- **M5 加 Bloom 时**：metaindex 多一条 `"filter.leveldb.BuiltinBloomFilter2"` → filter 块的 handle，
  **footer 布局零改动、`kTableFormatVersion` 不升**。
- **为什么 M3 就要写这个空块**（而不是 M5 再加）：让"footer 的 metaindex handle 合法 + metaindex 可解析"
  这条代码路径**从 M3 起就被真实执行与测试**（`M3-A18`），M5 只需往一个已验证的容器里塞一条记录。
  这直接兑现指令 D2 的"推荐预留"。
- **M3 的 reader 必须容忍未知的 metaindex 条目**（只记录、不使用）——这样 M5 写的 filter 不会让 M3 的 reader 报错。
  但**要计数上报**（`unknown_metaindex_entries`），不得静默（§12.2 的"截断/跳过必须计数"纪律）。

### 3.5 文件整体布局

```
偏移 0
  ┌──────────────────────────────────────────────┐
  │ data block 0                                 │  header(5) ‖ entries ‖ restarts ‖ crc(4)
  ├──────────────────────────────────────────────┤
  │ data block 1                                 │
  ├──────────────────────────────────────────────┤
  │ …                                            │
  ├──────────────────────────────────────────────┤
  │ data block N-1                               │
  ├──────────────────────────────────────────────┤
  │ metaindex block                              │  M3 内容为空（8 B payload）
  ├──────────────────────────────────────────────┤
  │ index block                                  │  每数据块一条：last_internal_key + handle
  ├──────────────────────────────────────────────┤
  │ footer (44 B, 定长, 文件末尾)                 │
  └──────────────────────────────────────────────┘
文件大小
```

**顺序是格式的一部分**（校验器据此检查）：
`metaindex.offset + metaindex.size <= index.offset` 且 `index.offset + index.size == file_size - kFooterSize`。
两个 handle 都必须满足 `offset + size <= file_size - kFooterSize`（**footer 区域被排除在块区域之外**）。

### 3.6 块头与块尾（CRC 覆盖面）—— 与 `protocol.md` §9.3 同形

```
block_on_disk := header(5B) ‖ payload ‖ crc32c(4B LE)
header        := length(4B LE) ‖ type(1B)
crc           := crc32c( length(4B LE) ‖ type(1B) ‖ payload )      // 即 header ‖ payload 的全部字节
```

| 字段 | 字节数 | 字节序 | 取值 |
|---|---|---|---|
| `length` | 4 | LE | payload 的字节数（**不含** header 与 crc）；`>= 8`（最小的合法 payload 是空块的 8 字节） |
| `type` | 1 | — | `0x01` data / `0x02` index / `0x03` metaindex / `0x04` filter（M5 预留，M3 不产出） |
| `payload` | `length` | — | §3.2/§3.3/§3.4 的三种布局之一 |
| `crc32c` | 4 | LE | 覆盖 `length ‖ type ‖ payload`（**含长度**） |

**常量**：`kBlockHeaderSize = 5`、`kBlockTrailerSize = 4`、`kBlockOverhead = 9`、`kBlockMinPayload = 8`。

**为什么 CRC 覆盖长度字段**：与 `protocol.md` §9.3 的"有意差异"**同一条理由**，且 §0.3 已用真实 WAL 文件
实证了本仓库就是这个口径（`CRC_over_len_type_payload MATCH 1 / CRC_over_type_payload MATCH 0`）。
不覆盖 `length` 的话，"`length` 被改大"只能靠"取到错 payload ⇒ CRC 碰巧失败"间接察觉——
而"碰巧"在对齐/全零区域是可构造的。**加进 CRC 后 `length` 的完整性是契约的一部分，不是推断。**

**`handle.size` 与块内 `length` 的冗余关系（写死，避免"M2 容量判据漂移"同类问题）**：

| 量 | 真相源 | 校验规则 |
|---|---|---|
| 读多少字节 | `handle.size`（**权威**） | 读取 `handle.size` 字节，`handle.size >= kBlockOverhead + kBlockMinPayload` |
| 块内自述长度 | `length`（**冗余，用于自检**） | 必须满足 `kBlockHeaderSize + length + kBlockTrailerSize == handle.size`，否则 `kCorruption`（在算 CRC **之前**校验） |
| 块类型 | `type` | 必须等于调用方期望的类型（`Table::Get` 期望 `0x01`；读索引期望 `0x02`），否则 `kCorruption`（I25） |

⇒ 这不是"两份判据"（M2 评审阻断项 1 的教训），而是**一份判据 + 一个冗余自检**：读的长度**只**取 `handle.size`，
`length` 只用来**检出不等于**。规则与失败码都写死在 §3.6，`#4` 评审按此逐条核对。

### 3.7 footer（定长 44 B）

```
footer := magic(4B) ‖ version(4B LE) ‖ index_handle(16B) ‖ metaindex_handle(16B) ‖ footer_crc(4B LE)
magic  := 'L','S','M','1'                       // 0x4C 0x53 0x4D 0x31
version:= 1
footer_crc := crc32c( footer[0..40) )            // 覆盖 magic ‖ version ‖ 两个 handle
```

| 字段 | 偏移 | 字节数 | 字节序 | 取值 |
|---|---|---|---|---|
| `magic` | 0 | 4 | — | 恒为 `"LSM1"`；不匹配 ⇒ `kCorruption` |
| `version` | 4 | 4 | LE | M3 写 `1`；读到 **≠ 1** ⇒ **`kNotSupported`**（是"更新的引擎写的合法文件"，不是损坏） |
| `index_handle` | 8 | 16 | LE | §3.3 |
| `metaindex_handle` | 24 | 16 | LE | §3.4 |
| `footer_crc` | 40 | 4 | LE | 覆盖前 40 字节 |

**为什么 footer 要自带 CRC**：footer 里的 `index_handle` 决定"去哪读索引"。它损坏 = **随机跳读**，
而随机跳读的结果是"读到一堆合法但不相关的字节"，其中"块头凑巧合法 + CRC 凑巧通过"的概率极低但**不是零**
（尤其当损坏落在 padding/全零区域时）。加 4 字节 CRC 把这件事变成**确定性检出**。
⇒ footer 从 40 B 变 44 B，代价可忽略。

**footer 解析的失败矩阵（`M3-A07`/`M3-A08` 逐条）**：

| 输入 | 结果 |
|---|---|
| 文件长度 `< kFooterSize`(44) | `kCorruption`（"too small to be an SSTable"） |
| `magic` 不符 | `kCorruption` |
| `version != 1` | `kNotSupported`（**不是** `kCorruption`：这是前向兼容信号） |
| `footer_crc` 不符 | `kCorruption` |
| `handle.offset + handle.size > file_size - 44` | `kCorruption`（越界，**不得**据此读取） |
| `metaindex.offset + metaindex.size > index.offset` | `kCorruption`（§3.5 的顺序约束） |
| `index.offset + index.size != file_size - 44` | `kCorruption`（索引必须紧贴 footer） |

### 3.8 与 `docs/protocol.md` 的衔接

`docs/protocol.md` 把自身定义为"位级编码契约：M1 定稿后冻结，M2（WAL record）、M3（SSTable block/index/footer）
**必须逐字复用**，不得各写一套"，并要求"任何变更须回到 `#0` 设计阶段修订本文件并说明影响面"。
SSTable 格式属于该文件**必须承载**的内容。⇒ 追加 **§10**。

**patch 纪律**（与 M2 追加 §9 时同）：**只追加，不改 §1~§9 任何一行**。
§5 那句"M2 起用于 WAL record 校验，M3 起用于 SSTable block/footer 校验"原文已经写好，无需改动。
**patch 的落地时机是 M3.1**（`#0` 门后），**本阶段只给文本**（§4）。

---

## 4. `docs/protocol.md` 追加 §10 的 patch 文本（**本阶段不落地**）

> 把下面整块**追加**到 `docs/protocol.md` 末尾（现末尾是 §9.4 的最后一行）。
> 与 §3 的正文完全一致；若本节与 §3 有不一致，以 §3 为准（§12.1 会核对一遍）。

````markdown
## 10. SSTable 编码（M3 定稿）

> 追加章节。§1~§9 为 M1/M2 冻结内容，本节不得反向修改它们。
> 本节的所有多字节整数一律**小端（LE）**，逐字节拼装/解析，禁止 `reinterpret_cast`（§1）。
> 本节与 §9 共用同一条 CRC 纪律：**CRC 覆盖面必须包含长度字段**（§9.3 的"有意差异"在此复用）。

### 10.1 文件命名与文件号空间

```
<dbname>/LOCK                 进程级独占锁（§9 无关，M2 的 D10）
<dbname>/META                 版本快照（§10.7）——**不是** MANIFEST
<dbname>/META.tmp             META 的写临时文件
<dbname>/%06u.log             WAL（§9）
<dbname>/%06u.sst             SSTable（已注册即不可变）
<dbname>/%06u.sst.tmp         SSTable 的写临时文件（**永不注册**）
```

- `.log` 与 `.sst` **共享**一个单调递增的 `next_file_number`（持久化在 `META`）。
- `META.next_file_number` 只是提示；`Open` 时的权威值 = `max(META.next_file_number, 目录中最大编号 + 1)`。
- 空库：`next_file_number = 1`，第一个 log 编号为 `1`。
- 后缀匹配**精确**：`ParseTableFileName` 必须拒绝 `%06u.sst.tmp`（禁止用前缀匹配）。

### 10.2 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `kTableMagic` | `"LSM1"`（4 B） | footer magic |
| `kTableFormatVersion` | `1` | footer 版本 |
| `kFooterSize` | `44` | 定长 footer |
| `kBlockHeaderSize` | `5` | `length(4B LE) ‖ type(1B)` |
| `kBlockTrailerSize` | `4` | `crc32c(4B LE)` |
| `kBlockOverhead` | `9` | `kBlockHeaderSize + kBlockTrailerSize` |
| `kBlockMinPayload` | `8` | 最小合法 payload（空块的 restart 数组） |
| `kDefaultBlockSize` | `4096` | 数据块目标大小 |
| `kRestartInterval` | `16` | 数据块 restart 间隔 |
| `kIndexRestartInterval` | `1` | 索引块 / metaindex 块的 restart 间隔 |
| `kBlockTypeData` | `0x01` | 数据块 |
| `kBlockTypeIndex` | `0x02` | 索引块 |
| `kBlockTypeMetaIndex` | `0x03` | 元数据块 |
| `kBlockTypeFilter` | `0x04` | 预留（M5 的 Bloom filter 块；M3 不产出） |
| `kMetaMagic` | `"LSMM"`（4 B） | `META` 的 magic（与 SSTable 的 `"LSM1"` **不同**，防止两类文件被互相当成对方） |

### 10.3 块（block）的通用外壳

```
block_on_disk := header(5B) ‖ payload ‖ crc32c(4B LE)
header        := length(4B LE) ‖ type(1B)
crc           := crc32c( length(4B LE) ‖ type(1B) ‖ payload )
```

| 字段 | 字节数 | 字节序 | 取值 |
|---|---|---|---|
| `length` | 4 | LE | payload 字节数（不含 header 与 crc），`>= 8` |
| `type` | 1 | — | §10.2 的块类型 |
| `payload` | `length` | — | §10.4 / §10.5 / §10.6 |
| `crc32c` | 4 | LE | 覆盖 `length ‖ type ‖ payload`（**含长度**，与 §9.3 同口径） |

**handle**（16 B，定宽）：

```
handle := offset(8B LE) ‖ size(8B LE)
```

`size` = 目标块的**总字节数** = `kBlockHeaderSize + length + kBlockTrailerSize`（即包含 header 与 crc）。

**读取与校验顺序（不可交换）**：

1. 校验 `handle.size >= kBlockOverhead + kBlockMinPayload`；
2. 按 `handle.size` 读取字节（**`handle.size` 是"读多少"的唯一真相源**）；
3. 解析 `header`，校验 `kBlockHeaderSize + length + kBlockTrailerSize == handle.size`（**先于 CRC**）；
4. 校验 `type` 等于调用方期望的块类型；
5. 计算 `crc32c(length ‖ type ‖ payload)` 与块尾 4 字节比较。

任一步失败 ⇒ `kCorruption`，**不得**用任何已解析出的长度去推进偏移或读取。

### 10.4 数据块 payload

```
data_block_payload := entry* ‖ restart_offset[uint32 LE] * restart_count ‖ restart_count(uint32 LE)
entry              := varint32(shared) ‖ varint32(non_shared) ‖ key_delta[non_shared] ‖ varint32(value_len) ‖ value
```

| 字段 | 编码 | 约束 |
|---|---|---|
| `shared` | varint32 | `0 .. 上一条完整 key 的长度`；**restart 点必须为 `0`** |
| `non_shared` | varint32 | `>= 1`；`shared + non_shared <= kMaxUserKeySize + 8`；`shared + non_shared <= 剩余字节` |
| `key_delta` | 字节 | `key = 上一条 key[0..shared) ‖ key_delta`；restart 点时"上一条 key"为空 |
| `value_len` | varint32 | `0 .. 剩余字节`；`type == kTypeDeletion` 时**必须为 0** |
| `value` | 字节 | 原始字节 |
| `restart_offset` | 4 B LE × n | `restart_offset[0] == 0`；**严格单调递增**；`< length - 4*(n+1)` |
| `restart_count` | 4 B LE | `>= 1` |

- `key` 是 **internal key**（§6）：`user_key ‖ trailer(8B LE)`。块内 entry 按 `InternalKeyComparator`（§6.1）
  **严格升序**；写入方必须保证，读取方不重排。
- **restart 组语义**：每 `kRestartInterval`(=16) 条 entry 一个 restart 点；**每组第一条的 `shared == 0`**，
  即**组间不共享前缀**（即使有公共前缀）。`last_key` 在 restart 点重置。
- **空数据块**：`payload` = `restart_offset[0]=0 (4B) ‖ restart_count=1 (4B)` = **8 字节**。
- **单条 entry 可以大于 `kDefaultBlockSize`**：切块判据是"**加上这一条之后**是否超过目标大小"，
  超了先封块；因此 `block_size` 是**目标值**，不是硬上限。

### 10.5 索引块 payload

```
index_payload := index_entry* ‖ restart_offset[uint32 LE] * n ‖ n(uint32 LE)
index_entry   := varint32(internal_key_len) ‖ internal_key ‖ handle(16B)
```

- `internal_key` = **对应数据块内最后一条 entry 的完整 internal key**（不做分隔 key 缩短）。
- `restart_interval = kIndexRestartInterval`(=1)。
- `Seek(target)`：在索引块上取**第一个 `internal_key >= target`** 的索引项（`>=`，不是 `>`）。

### 10.6 元数据块 payload

```
metaindex_payload := meta_entry* ‖ restart_offset[uint32 LE] * n ‖ n(uint32 LE)
meta_entry        := varint32(name_len) ‖ name ‖ handle(16B)
```

- `name` 是普通字符串（**不是** internal key）。M3 写**空表**（`n = 1`，`restart_offset[0] = 0`，无 entry）。
- M5 的 Bloom filter 通过 `name = "filter.leveldb.BuiltinBloomFilter2"` 指向 `kBlockTypeFilter` 块；
  **footer 布局不变、`kTableFormatVersion` 不升**。
- 读取方**必须容忍未知 `name`**（记录并计数，不报错、不影响其他块）。

### 10.7 footer

```
footer := magic(4B) ‖ version(4B LE) ‖ index_handle(16B) ‖ metaindex_handle(16B) ‖ footer_crc(4B LE)
```

| 字段 | 偏移 | 字节数 | 说明 |
|---|---|---|---|
| `magic` | 0 | 4 | `"LSM1"`；不符 ⇒ `kCorruption` |
| `version` | 4 | 4 | M3 写 `1`；读到 ≠1 ⇒ `kNotSupported` |
| `index_handle` | 8 | 16 | §10.3 |
| `metaindex_handle` | 24 | 16 | §10.3 |
| `footer_crc` | 40 | 4 | `crc32c(footer[0..40))` |

文件整体顺序（校验器据此检查）：
`metaindex.offset + metaindex.size <= index.offset`，且 `index.offset + index.size == file_size - kFooterSize`。

### 10.8 META（版本快照）

```
META := header ‖ file* ‖ tail
header := magic(4B "LSMM") ‖ format_version(4B LE = 1) ‖ comparator_name(len-prefixed)
          ‖ log_number(8B LE) ‖ min_log_number_to_keep(8B LE) ‖ next_file_number(8B LE) ‖ file_count(4B LE)
file   := number(8B LE) ‖ file_size(8B LE) ‖ max_sequence(8B LE)
          ‖ smallest(len-prefixed internal key) ‖ largest(len-prefixed internal key)
tail   := crc32c(4B LE)      // 覆盖 header ‖ file* 的全部字节
```

- `smallest`/`largest` 是该文件内按 `InternalKeyComparator` 的**最小/最大 internal key**（用于 key range 过滤）。
- `max_sequence` = 该文件内**最大**的 `sequence`（注意：**不是** `largest` 的 sequence——因为内部 key 序是
  "user key 升序 + trailer 降序"，`largest` 的 trailer 反而最小）。写入方在 `TableBuilder` 内顺带统计。
- `META` 的写入方式：写 `META.tmp` → `fsync` → `rename(META.tmp, META)` → `SyncDir(dir)`。
  **禁止原地覆写 `META`。**
- `META` 缺失时的语义见 §10.9。

### 10.9 恢复口径（与 §9 的 WAL 恢复组合）

1. `META` 存在：解析并校验 `tail` 的 CRC；任一处不符 ⇒ `kCorruption`（**不自动修复**）。
2. `META` **不存在**：
   - 目录中若存在 `*.sst` ⇒ `kCorruption`（"META 丢失但目录非空"——把最坏情况从**静默丢数据**变成**显式拒绝**）；
   - 否则版本为空、`min_log_number_to_keep = 1`、重放目录中**全部** `*.log`（等价于 M2 的行为）。
3. 重放集合由**目录实际内容**决定（不按水位裁剪）；`min_log_number_to_keep` 只用于**删除**已注册覆盖的 log。
4. 重放顺序：log 编号**数值升序**，文件内按字节顺序（§9.2 的 reader）。判定"尾部残骸 vs 中间损坏"沿用 §9 的
   口径：**只有最高编号的 log 允许尾部截断**，其余残骸 ⇒ `kCorruption`。
5. 恢复后的下一个可分配 sequence = `max(META 各文件的 max_sequence, WAL 重放到的最大 sequence) + 1`。
6. 恢复**不做任何基于水位的水位跳过**（只保留 §9 的"文件内 sequence 非递增则跳过并计数"）。
````

---

## 5. `TableBuilder` / `TableReader` 的接口、块读取路径与 CRC 口径

### 5.1 文件与类型清单（`src/sstable/`）

| 文件 | 内容 | 依赖 |
|---|---|---|
| `format.h` | §3.6/§3.7/§3.3 的**全部常量与 `Handle` 类型**；`BlockType` 枚举；`EncodeHandle` / `DecodeHandle` / `EncodeBlockHeader` / `DecodeBlockHeader` / `ComputeBlockCRC` / `EncodeFooter` / `DecodeFooter` | 只 `common.h` + `util/coding.h` + `util/crc32c.h` |
| `block_builder.{h,cpp}` | `BlockBuilder`（前缀压缩 + restart 数组 + `EstimatedSizeAfter` + `Finish`） | `format.h` |
| `block.{h,cpp}` | `Block`（解析但不拥有字节）、`Block::Iterator`（块内二分 Seek + 双向）、`ValidateBlock`（块结构校验器，I24） | `format.h` |
| `footer.{h,cpp}` | `Footer`（定长结构与解析失败矩阵，§3.7） | `format.h` |
| `table_builder.{h,cpp}` | `TableBuilder`（顺序写数据块 → 索引 → metaindex → footer） | `block_builder.h`、`footer.h`、`util/env.h` |
| `table.{h,cpp}` | `Table`（`Open`：读 footer + 索引；`Get`；`NewIterator`；`ApproximateOffsetOf`） | `block.h`、`footer.h`、`util/env.h` |

**依赖纪律（§1.4）**：`src/sstable/` 下**不得**出现 `version_*`/`merging_iterator`/`db_iter`/`db_impl`/`wal.h`，
**不得**出现任何 POSIX 头。`#1` 把它做成**独立静态库 `lsm_sstable`**（只链 `util`），
让越权依赖**链接失败**（机制保证，不靠人盯）。

### 5.2 `BlockBuilder` 完整签名

```cpp
// src/sstable/block_builder.h
namespace lsm {

// 把一个块按 protocol §10.4 的布局编出来。
// 容量口径：block_size 是**目标值**，判据只有 EstimatedSizeAfter 一处（禁止在别处复算）。
class BlockBuilder {
 public:
  // restart_interval：§10.4 的 kRestartInterval(16) 或 kIndexRestartInterval(1)。
  explicit BlockBuilder(int restart_interval);

  // 追加一条 (key, value)。前置条件：key 按 InternalKeyComparator 严格大于上一条（调用方保证，
  // 库内由 TableBuilder 的前驱检查保证；测试里由 CompareInternal 断言保证）。
  // 返回 void：本类不做校验（校验在 TableBuilder 层，失败带上下文返回 Status）——
  // 这与 M1 的 Arena/Skiplist 同纪律：底层容器不做业务校验。
  void Add(const Slice& key, const Slice& value);

  // 「再加上这一条之后」的字节数（含本条的 varint 前缀与 value）。
  // **唯一的切块判据**（§10.4）；TableBuilder::Add 只在它 > block_size 时封块。
  size_t EstimatedSizeAfter(const Slice& key, const Slice& value) const;

  // 收尾：写 restart 数组 + restart_count，返回 payload 总字节数。之后不可再 Add。
  size_t Finish();
  bool empty() const;                 // payload 为空（尚未 Add 过任何 entry）
  size_t NumEntries() const;
  size_t CurrentSizeEstimate() const; // == Finish() 会返回的值（用于判据自检，测试断言两者相等）
  const std::string& buffer() const;  // payload（Finish 之后含 restart 数组）
  void Reset();
 private:
  int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  std::string last_key_;              // 已解码的完整上一条 key（restart 点时清空）
  int counter_ = 0;
};

}  // namespace lsm
```

**`Add` 的编码算法（逐字，供 `#2` 的骨架与 `#4` 的逐行核对）**：

```
Add(key, value):
  shared := 0
  if counter_ < restart_interval_:
      shared := 最长公共前缀(last_key_, key)          // last_key_ 在 restart 点已被清空 ⇒ 自然为 0
  else:
      restarts_.push_back(buffer_.size())             // 本 entry 成为新 restart 点
      counter_ := 0
      last_key_.clear()                               // ⇒ shared 保持 0（组间不共享前缀）
  non_shared := key.size() - shared
  PutVarint32(&buffer_, shared)
  PutVarint32(&buffer_, non_shared)
  buffer_.append(key.data() + shared, non_shared)
  PutVarint32(&buffer_, value.size())
  buffer_.append(value.data(), value.size())
  last_key_.assign(key.data(), key.size())
  ++counter_

Finish():
  if restarts_.empty(): restarts_.push_back(0)        // 防御：空块也必须有 restart_count >= 1
  for r in restarts_: PutFixed32(&buffer_, r)
  PutFixed32(&buffer_, restarts_.size())
  return buffer_.size()
```

> **注意 `restarts_` 的初始化**：构造函数里 push 一个 `0`（与 LevelDB 同），因此**第一条 entry 的
> `shared` 通过"`counter_=0 < interval` 且 `last_key_` 为空"自然得到 0**，而不是靠一个特判分支。
> `#2` 的骨架与 `M3-A02` 必须覆盖"块内第一条的 restart 语义"，因为这是唯一一条"看起来像特判但不应写成特判"的地方。

### 5.3 `Block` 与块内 Seek 的二分规则（I24）

```cpp
// src/sstable/block.h
namespace lsm {

// Block 是**视图**：不拥有 contents_，调用方保证底层字节在 Block 存活期内有效且不可变。
class Block {
 public:
  Block(const BlockContents& contents, const InternalKeyComparator& icmp);
  ~Block();

  size_t size() const;
  uint32_t NumRestarts() const;                 // restart_count

  class Iterator : public Iterator {            // lsm::Iterator（common.h）
    // 三态状态机与 M1 的 Skiplist::Iterator 同语义：
    //   SeekToFirst / SeekToLast / Seek / Next / Prev
    //   key()   = 当前 entry 的**完整 internal key**（指向 contents_，有效期 = Block 存活期）
    //   value() = 当前 entry 的 value（同上）
    //   status()= 解析期发生的 kCorruption（块畸形时）
    // 块内 Prev：二分定位到当前 entry 所在的 restart 组起点，再从组起点向前扫描组内前一条
    //   （O(kRestartInterval)，与 LevelDB 同法）
  };
  Iterator* NewIterator() const;

 private:
  const char* data_; const size_t size_;
  uint32_t restart_offset_;                     // restart 数组起始偏移
  uint32_t num_restarts_;
  const InternalKeyComparator* icmp_;           // 不拥有（L5 析构顺序）
};

// 块结构校验器（I24 的判据载体，测试与可选的严格读模式使用）：
//   ① restart_count >= 1；② restart_offset[0] == 0；
//   ③ restart_offset 严格单调递增；④ 每个 restart_offset 落在一条 entry 的起始边界上
//      （即从该偏移起能完整解出一条 entry 且不越界）；
//   ⑤ 每条 entry 的 shared/non_shared/vlen 都在合法范围内
// 返回 Status：失败时 kCorruption + 精确偏移。
Status ValidateBlock(const BlockContents& contents, BlockType expected_type, std::string* why);

}  // namespace lsm
```

**块内 `Seek(target)` 的二分规则（写死，评审逐字核对）**：

```
Seek(target):
  ① 若 num_restarts_ == 0 或 size_ < 12 → Invalid（空块/畸形块）
  ② 在 restart 数组上二分：找**最后一个**满足 key_at(restart[i]) < target 的 i
     （比较一律走 InternalKeyComparator；畸形 key 由其"退化为整条字节序"的防御分支兜底）
     - 若 key_at(restart[0]) >= target → i = 0（**必须**从 restart[0] 起步，不能从块中间起步：
       因为组间不共享前缀，从任意字节开始解会得到错误的 key）
  ③ 从 restart[i] 起**顺序**解码（最多 kRestartInterval 条），直到 key >= target 或到达
     下一个 restart 点 / 块尾
  ④ 若到下一个 restart 点仍未命中且 i+1 < num_restarts_ → i := i+1，回到 ③
     （由 ② 的"最后一个 < target"语义保证**至多再扫一组**）
  ⑤ 命中 → Valid；越过块尾 → Invalid（三态进入 kPastEnd）
```

**术语纪律（避免歧义）**：本文档统一用「**restart 组**」指"由一个 restart 点起始的至多
`kRestartInterval` 条 entry 的连续段"，**不使用** LevelDB 文档里的 "region"/"block" 来指它
（"block" 已被"数据块/索引块"占用）。`#1` 与 `#4` 按同一术语核对。

### 5.4 `Table`、`TableBuilder` 与块读取路径

```cpp
// src/sstable/table.h
namespace lsm {

class Table {
 public:
  // 打开：读末尾 44 B footer → 校验 → 读 metaindex 块 → 读 index 块。
  // 失败一律 kCorruption（带文件 + 偏移 + 原因），或 kNotSupported（version != 1）。
  // 不拥有 file_（shared_ptr 由 TableCache 持有）。verify_checksums 透传读时校验开关。
  static Status Open(const Options& options, std::shared_ptr<RandomAccessFile> file,
                     uint64_t file_size, std::shared_ptr<Table>* table);

  Status Get(const ReadOptions& ropt, const Slice& lookup_key, std::string* value) const;
  // 返回值所有权归调用方；Iterator 内部持住 table_ 与文件的 shared_ptr（L19）。
  Iterator* NewIterator(const ReadOptions& ropt) const;

  const InternalKeyComparator& internal_comparator() const;
  const InternalKey& smallest() const;   // 来自 META 的缓存副本（供 key range 过滤，零 IO）
  const InternalKey& largest() const;
  uint64_t file_size() const;
  uint64_t NumDataBlocks() const;
  uint64_t MaxSequence() const;          // == META.max_sequence（供恢复期水位，§8.2）

 private:
  // 读一个块：按 handle 读 handle.size 字节 → §10.3 的 5 步校验 → 返回 BlockContents
  Status ReadBlock(const ReadOptions& ropt, const Handle& handle, BlockType expected,
                   BlockContents* out, ReadStats* stats) const;
  // index_handle_ 与 metaindex_handle_（已解析）；index_block_ 已载入内存（§3.3 的规模论证）
  ...
};

}  // namespace lsm
```

**`Get` 的块读取路径**（口径写死，`M3-A19` 断言它真的零 IO；`M3-A23` 断言块读在锁外）：

```
Table::Get(ropt, lookup_key, value):
  ① key range 过滤（**零 IO**）：若 lookup_key 的 user key < smallest().user_key
     或 > largest().user_key → 直接 kNotFound（stats.key_range_skipped++）
  ② index_block_.Seek(lookup_key)     —— 索引已在内存（Open 时载入），**零 IO**
     取第一个 key >= lookup_key 的索引项；无 → kNotFound（目标在末尾之后）
  ③ ReadBlock(index_entry.handle, kBlockTypeData)      —— **1 次块读**
  ④ data_block.Seek(lookup_key) → 拿到第一个 >= lookup_key 的 entry
  ⑤ 校验该 entry 的 user key == lookup_key 的 user key（走 user_comparator，**不得**退化成逐字节比较
     ——M1 #4 评审阻断项 1 的同源纪律，SSTable 侧的输入同样来自不可信来源）
  ⑥ type == kTypeDeletion → kDeleted；否则 value := entry.value → kFound
  ⑦ 未命中 → kNotFound
```

**CRC 口径**：每一次 `ReadBlock` 都按 §10.3 的 5 步走。第 5 步（CRC）**仅当 `ropt.verify_checksums` 为真**时执行；
前 4 步（结构校验）**永不跳过**。`stats.crc_checked` / `stats.crc_failed` 逐次累加。

```cpp
// src/sstable/table_builder.h
class TableBuilder {
 public:
  // file 由调用方打开（`%06u.sst.tmp`）；owns_file 表示析构时是否 Close。
  TableBuilder(const Options& options, WritableFile* file);

  // 追加一条。前置条件：key 严格大于上一条（违反 → kInvalidArgument，**不**写成文件）
  Status Add(const Slice& key, const Slice& value);

  // 收尾：封最后一个数据块 → 写 metaindex 块 → 写索引块 → 写 footer。
  // **不**做 fsync、**不**做 rename、**不**注册（那三件事是 flush 线程的职责，I22）。
  Status Finish();

  Status status() const;              // 粘性：任一 Add/Flush 失败后一直返回该错误
  uint64_t FileSize() const;
  uint64_t NumEntries() const;
  uint64_t NumDataBlocks() const;
  SequenceNumber MaxSequence() const; // §10.8 的 max_sequence（随 Add 统计）
  const std::string& smallest() const;  // internal key
  const std::string& largest() const;   // internal key
 private:
  Status FlushBlock();                // 把当前 BlockBuilder 的 payload 加外壳写出，注册索引项
  ...
};
```

**`TableBuilder::Add` 的块切分（唯一判据）**：

```
Add(key, value):
  if (block_builder_.EstimatedSizeAfter(key, value) > options_.block_size && !block_builder_.empty()):
      FlushBlock()                    // 先封块（因此单条 entry 可以超过 block_size，§10.4）
  if (!block_builder_.empty() && icmp_.Compare(key, pending_last_key_) <= 0):
      return Status::InvalidArgument("TableBuilder::Add: keys must be strictly increasing")
  block_builder_.Add(key, value)
  更新 smallest_/largest_/max_sequence_；entries_++
```

**`Finish` 的写出顺序（不可交换）**：`flush last data block` → `write metaindex block` → `write index block` → `write footer`
⇒ 满足 §3.5 的顺序约束。`Finish` **不** `Sync()`：持久化顺序由 flush 线程掌握（§6.3），
把 IO 顺序放在**一个**地方（flush 状态机）才可能被 `#4` 逐行核对。

---

## 6. flush 路径

### 6.1 状态与锁（在 M2 的两把锁上做最小扩展）

| 状态 | 保护者 | 说明 |
|---|---|---|
| `memtable_`（`shared_ptr<MemTable>`） | `mutex_` | **M3 从 `unique_ptr` 改为 `shared_ptr`**（L19/L21：读路径要在锁外持引用） |
| `immutables_`（`deque<shared_ptr<Immutable>>`，`Immutable{shared_ptr<MemTable>, uint64_t log_number}`） | `mutex_` | 已冻结待落盘的表；**新→旧 = back→front**（back 是最新冻结的） |
| `log_number_` | `mutex_` | 当前 WAL 编号；**只由当前 flusher 修改**（L20） |
| `log_sealed_` | `mutex_` | 当前 log 是否已"封口"（冻结时置位，轮转完成后清） |
| `next_file_number_` | `mutex_` | §3.1 的分配器 |
| `version_`（`shared_ptr<const Version>`） | `mutex_` | 不可变版本；注册 = 构造新版本 + 原子替换（L15） |
| `last_sequence_` / `bg_error_` / `closed_` / `file_lock_` | `mutex_`（`closed_`/`bg_error_` 读时也在 `commit_mu_` 下，同 M2） | 沿用 M2 |
| `log_`（`WALWriter`）/ `durable_seq_` / `log_last_appended_seq_` | `commit_mu_` | 沿用 M2；**新增 `log_last_appended_seq_`**（M3 的 per-log 边界，用于轮转，§15 R1）；M2 的 I32 已在 `a3c85a8` 用 `mutex_` 保护的 `appended_seq_` 修完 |
| `queue_` / `flusher_active_` / `commit_cv_` | `commit_mu_` | 沿用 M2 组提交 |
| 后台线程相关：`bg_thread_`、`bg_cv_`、`bg_started_`、`bg_stop_`、`flush_stats_` | `mutex_` + `bg_cv_` | M3 新增 |

- **锁序不变（L8）**：`commit_mu_ → mutex_`，禁止反向。
- **新增锁序规则（L21）**：后台 flush 线程**只**取 `mutex_`（它不碰组提交队列）；它**不允许**取 `commit_mu_`。
  ⇒ 后台线程与 flusher 之间**没有**锁序问题，二者只通过 `mutex_` 交互（`immutables_` / `version_` / `log_number_`）。
  `#4` 评审按"后台线程的函数体里不得出现 `commit_mu_`"逐行核对。

### 6.2 冻结（锁内、纯内存）与"容量不再是失败原因"

**触发点**：仍由**写路径的 flusher**在**取批时**判定（沿用 M2 的位置，`WouldReject` 是唯一判据），
但 M3 的行为从"拒绝"改成"冻结"：

```
RunFlusher() —— 阶段 A（持 commit_mu_ → mutex_）
  取批 members（字节上限 kMaxGroupBytes / 条数上限 kMaxGroupRecs，同 M2）
  footprint := Σ (p->entry_bytes + kMemTableNodeOverhead)         // 与 M2 逐字相同
  if (closed_) → 整批拒绝（IOError）
  else if (!bg_error_.ok()) → 整批拒绝（bg_error_）
  else if (memtable_->WouldReject(footprint)):                    // ← 唯一容量判据
      immutables_.push_back({memtable_, log_number_})             // ① 冻结：旧表进列表
      memtable_ = make_shared<MemTable>(icmp_, NewTableCapacity(footprint))   // ② 换新表（容量见下）
      log_sealed_ = true                                          // ③ 当前 log 封口（不再接受新记录）
      need_rotate_ = true                                         // ④ 待轮转（阶段 A' 做，IO 在锁外）
  // 注意：**不**在这里分配 sequence、**不**组 payload（留给阶段 B，见 §6.5 的 I33 理由）
```

**`NewTableCapacity(footprint)`（单一口径）**：

```
NewTableCapacity(footprint) := max(options_.write_buffer_size, footprint + kMemTableNodeOverhead)
```

**为什么必须这样**：若新表的容量仍是 `write_buffer_size`，那么"**单条/单批的预估占用本身就 >= write_buffer_size**"
时，`memtable_->Add` 会返回 `kFrozen` ⇒ 被 M2 的 flusher 当成 `bg_error_`（fail-stop）⇒
**整个库变成粘性写只读**。这正是 M2 评审优化项 2 的**同类风险**（当时是"超大 value 触发粘性写只读"，
通过在 `Write` 入口拒绝解决）。M3 的容量路径**不能再靠"入口拒绝"**（因为 M3 允许任意大小直到 `kMaxLogicalRecordSize`），
所以必须让"新表装得下这一批"**由构造保证**。

**证明（"不可拒绝"的可证性，而不是"应该不会"）**：
`MemTable::WouldReject(extra)` 的定义是 `IsFrozen() || ApproximateMemoryUsage() + extra >= write_buffer_size_`。
新表刚构造时 `ApproximateMemoryUsage() = arena_.BytesAllocated()(=0) + sizeof(MemTable)`，
而 `WouldReject(footprint)` 的判据里 `extra = footprint`。
取 `cap := footprint + kMemTableNodeOverhead + sizeof(MemTable) + 1`，则
`0 + sizeof(MemTable) + footprint < cap`，即 `WouldReject(footprint) == false`。
⇒ **本批的 `Add` 在结构上不可能返回 `kFrozen`**。
`M3-A21` 用"单条 64 KiB value + `write_buffer_size = 4 KiB`"把这条钉住，并断言"写成功、可读、无粘性错误"。

**"单一真相源"的口径声明（直接回应 M2 评审阻断项 1 的教训）**：

> **容量只有一处判据 —— `MemTable::WouldReject`（以及它内部读的 `write_buffer_size_`）。**
> DB 层**不**复算"能装多少条"，它只决定"给这个 `MemTable` 多大的 `write_buffer_size`"。
> 两处职责不重叠、不存在"两份公式漂移"的可能。
> M2 的教训（`RunFlusher` 只查 `IsFrozen()` 而 `Add` 查 `usage >= 上限` ⇒ 判据漂移 ⇒ 被拒写复活）
> 在 M3 由"**取批时用同一个 `WouldReject(footprint)` 判、且判过的批一定装得下**"闭合。

**冻结的幂等性与"重复冻结"**：`immutables_` 里同一个 `MemTable` 只会出现一次（冻结即从 `memtable_` 移走）。
`MemTable::Freeze()` 仍幂等（M1 语义不变）；M3 在冻结时**不调用** `Freeze()`——因为表已不再被写入，
`frozen_` 标志对 M3 无功能作用。**这是一个必须登记的语义收窄**：

> **登记（`#1` 必须抄进风险清单）**：M3 起，`PersistentDBImpl` 的写路径**不再依赖** `MemTable::frozen_`；
> 表的"不可再写"由"已从 `memtable_` 移入 `immutables_`"这一**所有权事实**保证。
> 内存模式的 `DBImpl`（M1）行为**完全不变**（它没有 flush，仍靠 `frozen_`）。

### 6.3 落盘（锁外 IO）：durable → rename → 注册，一步都不能少

**背景（必须写进设计的评审重点）**：指令硬性约束 5 只写"写文件 + fsync + 原子 rename 之后才允许注册"。
**这不够**：`rename` 只保证"目标名字在崩溃后指向新 inode **或** 旧 inode"，它**不保证目录项本身持久**。
在 `rename` 之后、目录项落盘之前掉电 ⇒ 重启后 `.sst` **不存在**，而 `META`（如果已经先写了）却引用它
⇒ 恢复时报"引用了一个不存在的文件"，或者更糟：把"文件不存在"当成"文件为空"。
⇒ **必须在 `rename` 之后、写 `META` 之前插入 `SyncDir`。** 这条是本设计对指令原文的**加强**，
理由是评审重点 4 明写的"rename 后目录未 fsync 导致元数据丢失"，且 M2 设计 §5.7 已声明过 `SyncDir`
（实现从未落地，见 §0.1 W3）。

**后台 flush 线程的一次迭代（`FlushImmutable`，逐字顺序不可交换）**：

```
①  [mutex_] imm = immutables_.front()（若空则回到等待）
      imm->log_number 已知；memtable 的 shared_ptr 已在手（后台线程持引用，L21）

②  num := next_file_number_（分配；只递增内存计数器，不落盘——§3.1 说权威值在 Open 时重算）
     tmp := TempFileName(dbname_, num)      // %06u.sst.tmp
     final := TableFileName(dbname_, num)   // %06u.sst

③  Env::NewWritableFile(tmp) → TableBuilder → 遍历 imm->memtable->NewIterator() 逐条 Add
     —— **锁外**；遍历期间该 MemTable 只读（I3：已发布节点不可变），无需持锁
     失败 ⇒ 跳到 ⑧（fail-stop 路径），**不动** META、**不删** WAL

④  tb->Finish()（写 footer）→ file->Sync()      ★ **步骤 ④ = "写文件 + fsync"**
     失败 ⇒ 删 tmp（尽力而为）→ ⑧

⑤  file->Close() → Env::RenameFile(tmp, final)  ★ **步骤 ⑤ = "原子 rename"**
     失败 ⇒ 删 tmp → ⑧
     ★ 此刻起 final 存在但**尚未注册** ⇒ 它在读路径上**不可见**（读路径只走 version_->files()），
       在崩溃恢复时是**孤儿**（会被清理，§8.3）⇒ 这就是"半边文件被注册"在**结构上不可能**的原因

⑥  Env::SyncDir(dbname_)                        ★ **步骤 ⑥ = 目录项 durable**（本设计的加强项）
     失败 ⇒ ⑧

⑦  [mutex_] 构造新 Version（= 旧 version_->files() ∪ {新文件元数据}，按文件号降序）
             → 原子替换 version_（L15）
             从 immutables_ 弹出 imm
             重算并持久化 min_log_number_to_keep（I34）与 next_file_number
             → 写 META：META.tmp → fsync → rename → SyncDir   ★ **步骤 ⑦ = "注册"**
             失败 ⇒ 回滚内存中的 version_？**不回滚**：META 没写成 ⇒ 重启后新文件是孤儿 ⇒ 数据仍在 WAL
                    ⇒ 置 bg_error_（fail-stop）、把 imm 放回 immutables_ 头部（内存态仍可读）、flush_stats_.failed++
            ★ **只有此刻（META 已 durable）才允许删除老 log**（⑨）

⑧  fail-stop 路径（④⑤⑥⑦ 任一失败）：
     bg_error_ = 具体 Status（粘性）；flush_stats_.failed++
     imm **不丢弃**（数据仍在 MemTable 内存里可读，且 WAL 未删）
     bg_cv_.notify_all()（唤醒可能停等的写者，让它们看到 bg_error_）
     线程继续循环（但要能安全退出，见 §6.5）

⑨  WAL 回收（**锁外**，见 §6.6）：删除编号 < min_log_number_to_keep 的 *.log，计数上报
```

**顺序的不可交换性（逐条给出反例）**：

| 若交换 | 后果 |
|---|---|
| ⑤ 在 ④ 之前（先 rename 后 fsync） | `rename` 后的文件内容可能只写了前一半 ⇒ 崩溃后 `META` 引用一个**内容不全**的 `.sst` ⇒ 读到错值或 CRC 失败。**这正是"半边文件"的另一种形态** |
| ⑦ 在 ⑤/⑥ 之前（先注册后 rename） | `META` 引用 `%06u.sst`，而该文件还不存在/目录项不持久 ⇒ 恢复时"引用了不存在的文件"。**指令硬性约束 5 明文禁止** |
| ⑦ 在 ⑥ 之前（注册但目录项不持久） | 掉电后 `META` 在、`.sst` 的目录项不在 ⇒ 同上。**这是指令原文没写、但评审重点 4 点名的那一条** |
| ⑨ 在 ⑦ 之前（先删 log 后注册） | 删掉了唯一副本；若注册失败 ⇒ **丢数据**。**I34 硬禁止** |

### 6.4 flush 失败如何 fail-stop（并保留可读性与 WAL）

| 失败点 | `bg_error_` | 写路径 | 读路径 | WAL | MemTable |
|---|---|---|---|---|---|
| 写 `.tmp` / `Finish` | 该 `kIOError`/`kCorruption`（粘性） | 后续 `Put`/`Delete` 一律返回 `bg_error_`（**不**入队、**不**分配 sequence、**不**写 WAL） | `Get`/迭代器**照常工作**（含内存里的 immutables） | **不删** | 保留在 `immutables_`，数据可读 |
| `Sync()`（fsync `.tmp`） | 同上 | 同上 | 同上 | **不删** | 同上 |
| `RenameFile` | 同上 | 同上 | 同上 | **不删** | 同上 |
| `SyncDir` | 同上 | 同上 | 同上 | **不删** | 同上 |
| 写/rename `META` | 同上 | 同上 | 同上（内存版本已含新文件 ⇒ 本轮可读；重启后新文件是孤儿 ⇒ 数据从 WAL 重放） | **不删** | 同上 |
| `Close()` 时的 `Sync()` | 沿用 M2（`Close` 返回该错误，但资源仍释放） | — | — | — | 未落盘的 immutables **被放弃**，`flush_stats_.abandoned++`（§6.5） |

**纪律（对齐 M2 的 D11，并吸收 M2 评审教训 4"输入校验不得触发 fail-stop"）**：

1. **只有 IO 失败与结构性损坏才进 `bg_error_`。** 任何**输入校验**类失败（key 为空/过长、value 超过
   `kMaxLogicalRecordSize`、`block_size` 非法、`max_open_files == 0`）都必须在**更早**的位置以
   `kInvalidArgument` 拒绝，**不得**污染 `bg_error_`。
   - 具体落点：`Put`/`Delete` 入口（M2 已有）；`Options` 的合法性在 `Open` 时一次性校验（§8.5）。
   - `M3-A24` 断言：注入 `fsync` 失败后 `bg_error_` 被置位且**写入此后返回同一错误**；而
     `M3-A21`/`M3-A53` 断言"越界输入"**不会**让后续合法写失败（防"粘性写只读"复活）。
2. **粘性**：`bg_error_` 一旦置位不再清除（重启是唯一的恢复途径）。`Get`/`NewIterator` 不受影响
   （只读降级，M2 同）。
3. **不静默**：每次失败都 `flush_stats_.failed++` + `flush_stats_.last_error`（一个可读的 Status 字符串），
   并打一条 `[WARN]`/`[ERROR]` 到 `stderr`（与 M2 的截断 WARN 同纪律）。

### 6.5 后台线程的生命周期、关闭顺序与停等

```
启动：PersistentDBImpl 构造后、RecoverAndOpen 成功返回前**不**启动线程（L12：恢复期间不得有后台线程运行）。
      RecoverAndOpen 末尾 db->StartBackgroundThread() —— 在 *dbptr 发布**之前**。

循环（单线程）：
  [mutex_] while (!bg_stop_ && immutables_.empty() && !rotate_needed_) bg_cv_.wait(lk)
           if (bg_stop_ && immutables_.empty()) break
           if (rotate_needed_) → 轮转（§6.6.1），continue
           取 immutables_.front()（拷贝 shared_ptr，**不弹出**）
  [锁外] FlushImmutable(imm)   （§6.3）
  回到循环

关闭（Close()，顺序不可交换，在 M2 的 I20/L11 之上扩展）：
  ① [commit_mu_ → mutex_] closed_ = true            // 拒绝新写（M2 的 L11）
  ② 等在途组提交批结算：commit_cv_.wait(commit_mu_, !flusher_active_ && queue_.empty())   // M2 阻断项 6 的修复
  ③ [mutex_] bg_stop_ = true; bg_cv_.notify_all()   // 让后台线程在当前迭代结束后退出
  ④ join 后台线程                                    // ★ 必须在 ⑤ 之前：否则它可能访问已关闭的文件/已析构的对象
  ⑤ [commit_mu_] log_->Sync()（M2 的"I20：Close 隐含 Sync"）→ log_->Close()
  ⑥ 释放 LOCK（幂等路径也要释放，M2 已有）
  ★ Close() **不**强制把 immutables_ 落盘：它们的数据在 WAL 里，重启后由恢复重放。
     但**必须计数**：flush_stats_.abandoned = 被放弃的 immutable 数（§6.4 表末行）。
     理由（"丢弃必须计数"纪律）：放弃是一类**事实上的丢弃**，即使数据在 WAL 里也必须可见，
     否则"Close 之后磁盘状态"不可观测，且掩盖"flush 永远跟不上"这种退化。
```

**写者的停等（`kMaxImmutableMemTables = 2`）**：

```
阶段 A 的取批入口（持 commit_mu_ → mutex_）：
  while (immutables_.size() >= kMaxImmutableMemTables && bg_error_.ok() && !closed_):
      bg_cv_.wait(lk)                 // 等后台线程消化掉一个
  ⇒ 谓词**必须**覆盖三个退出条件：容量让出、bg_error_、closed_。
     漏掉后两个 ⇒ 后台线程 fail-stop 或 Close 时写者永久睡下去（M2 阻断项 6 的同源教训：
     "谓词必须覆盖所有能让等待者离开的条件"）。
  ⇒ 这是"停等"而不是 kFrozen：M3 起容量不足**不再**是写的失败原因（§1.2 边界 2）。
  ⇒ 上界：最多连续 2 次 flush 才停等（§D5 估算 ≈21 ms）。`M3-A25` 用注入的慢 flush 确定性验证。
```

### 6.6 WAL 轮转（6.6.1）与回收判据（6.6.2）

#### 6.6.1 轮转：放在"flusher 的 IO 窗口内、分配 sequence 之前"

**为什么需要轮转**（不轮转则永远删不掉任何 log）：

- 若不轮转，`log_number_` 是唯一且最大的编号，而"当前 `memtable_` 的 log_number"恒等于它
  ⇒ `min_log_to_keep = log_number_` ⇒ 可删集合恒为空 ⇒ **WAL 永不回收**（泄漏）。
- 推导见 I34 的证明：需要一个**严格更大**的新 log 编号出现，才能让旧编号"落到水位之下"。

**轮转由谁做**：**当前组提交的 flusher**（持 `flusher_active_` 的那个写者），在它自己的 IO 窗口里做。
**后台 flush 线程不做轮转**（L20）。这条选择的三个好处：

1. **不需要新的同步原语**：M2 的 `flusher_active_` 已经保证"任一时刻至多一个写者在做 WAL IO"，
   轮转（创建文件 + `SyncDir`）落在这个独占窗口内 ⇒ **天然串行，无竞态**。
2. **不需要写者停等轮转**：其他写者本来就在 `commit_cv_` 上等着当前 flusher 完成
   （它们等的是 `w.done` 或"我能接手当 flusher"）。轮转只是让当前 flusher 的临界区长一点点。
3. **失败可以整批干净地拒绝**（见 I33）：把轮转放在"分配 sequence / `Append`"**之前**，
   轮转失败 ⇒ 本批**不 Append、不推进 sequence、不 Add**（M2 阻断项 1 修复的同一纪律：
   "判不过就整批拒绝，不留半成品"）。

**阶段划分（把 M2 的 `RunFlusher` 拆成 A / A' / B / C，语义增量最小）**：

```
阶段 A   持 commit_mu_ → mutex_：取批 members；冻结判定（§6.2）；**不**分配 sequence、**不**组 payload
阶段 A'  锁外，仍持"flusher 角色"：
           if (need_rotate_) → new_no := log_number_ + 1
                               Env::NewWritableFile(new_no.log) → Close
                               Env::SyncDir(dbname_)                 // 目录项 durable
                               失败 ⇒ 整批拒绝（kIOError）、**不**置 bg_error_（可用性优先：
                                      一次创建文件失败不代表库损坏；但要计数 rotate_failed++）
                               成功 ⇒ 回锁（commit_mu_ → mutex_）：log_ 换新、log_number_ = new_no、
                                      log_sealed_ = false、need_rotate_ = false
阶段 B   持 commit_mu_ → mutex_：begin := last_sequence_ + 1；members[i].begin := begin + i；
                                 last_sequence_ := begin + |members| - 1；payload := EncodeGroup(...)
阶段 C   锁外：log_->Append(payload)；if (need_sync) log_->Sync()；
                 if (Append 成功) [commit_mu_] log_last_appended_seq_ := 上面那个上界（per-log 边界；M2 的 I32 已在 a3c85a8 修完）
                 然后 [mutex_] 逐条 memtable_->Add(...)（失败 ⇒ bg_error_，见 §6.4）
         结算：整批同一个 Status（I16 不变）
```

**与 M2 的行为差异（必须登记，`#1` 抄进清单）**：

| 差异 | M2 | M3 | 影响 |
|---|---|---|---|
| sequence 分配时机 | 与取批同一临界区 | 移到阶段 B（轮转之后） | **语义不变**（同临界区属性保持：阶段 B 同时持 `commit_mu_` 与 `mutex_`）；外部观测不到 |
| 容量不足 | 整批拒绝 `kFrozen` | 冻结 + 换表 + 本批照常写入 | **外部行为变化**（§1.2 边界 1），由 `M3-A20` 覆盖 |
| log 编号 | `Open` 时取 max，之后不变 | 冻结后轮转到 `+1` | `%06u.log` 的编号不再是"恒为 1"；恢复按**数值**升序（M2 的 A12 口径不变） |
| 文件号空间 | 只有 `.log` | `.log`/`.sst` 共享 | 兼容：M2 老库（无 `META`）仍能打开（§8.3 规则 2） |

#### 6.6.2 `min_log_number_to_keep` 的判据（I34，单一真相源）

```
min_log_to_keep := min( { t.log_number : t ∈ pending } )        // pending = {memtable_} ∪ immutables_
```

- `pending` **恒非空**（至少含当前 `memtable_`）⇒ 定义永远有值。
- 每个表的 `log_number` 在**创建它的时候**确定，之后**不再改变**：
  - `Open` 后新建的 `memtable_`：`log_number = log_number_`（当前 log）。
  - 冻结时：**旧的 `memtable_` 保留它自己的 `log_number`**（不是当时的 `log_number_`——虽然此刻两者相等）；
    新 `memtable_` 的 `log_number = log_number_`（**同一个值**，因为轮转还没发生）。
  - 轮转之后，新 `memtable_` 的 `log_number` **不更新**（它仍然记录了"它的最早写入所在 log"）。
    ⇒ 这是**保守**的（把新 log 也留久一点），且保证不漏删。
- **可删集合** `:= { log n : n < min_log_to_keep }`（**严格小于**）。
- **每次 flush 注册成功后重算并写入 `META`**；删除**只在 `META` 的 `rename + SyncDir` 之后**执行（I34）。

**正确性证明（逐步，供评审逐条核对）**：

1. **单调性引理**：log 编号越大 ⇒ 其中所有 record 的 sequence 越大。
   证明：轮转只在当前 log 被"封口"后发生（`log_sealed_`），封口后**没有**任何新 record 进入旧 log
   （阶段 A' 之后所有 `Append` 都打到新 `log_`）；而 sequence 在阶段 B 分配、单调递增。∎
2. **不变式**：对任一 `pending` 中的表 `t`，`t` 的所有写入都落在编号 `>= t.log_number` 的某个 log 里，
   且**至少一条**落在 `t.log_number` 对应的 log 里。
   证明：`t` 创建时的 `log_number_` 就是 `t.log_number`，它的第一条写入必然被打到那个 log（阶段 C 在阶段 A' 之后）。∎
3. **完备性**：设 `W = min_log_to_keep`。任一编号 `n < W` 的 log，其**全部** record 都已被
   某个**已注册且 durable** 的 SSTable 覆盖。
   证明：`n < W <= t.log_number` 对**所有** `t ∈ pending` 成立。由引理 1 与不变式 2，
   编号 `n` 的 log 里的 record 的 sequence 都小于"任何 pending 表中的第一条写入"的 sequence
   ⇒ 它们**全部**属于**已经注册**的表（否则会有一个 pending 表的 `log_number <= n`，与 `n < W` 矛盾）。∎
4. **必要性**：编号 `>= W` 的 log **不得**删。
   反证：设 `t* ∈ pending` 达到最小 `t.log_number = W`。由不变式 2，`t*` 至少有一条写入落在编号 `W` 的 log 里，
   而 `t*` **尚未注册** ⇒ 它只在 WAL 里。删掉编号 `W` 的 log ⇒ 该写入丢。∎
5. **删除的安全性还要求"被覆盖的 SSTable 已 durable"**：由 ⑦（META 的 `rename + SyncDir`）保证；
   删除被放在 ⑦ 之后。∎

> **这是 M3 唯一一条"算错就丢数据"的推理链**，所以它必须（a）只有一处实现（`RecomputeMinLogToKeep()`，
> 在 `mutex_` 下调用），（b）由 `M3-A45`（判据本身）与 `M3-A47`（顺序）钉住，
> （c）由 `M3-B01`/`M3-B09` 在真实磁盘 + kill -9 下重跑 `missing 0`。

---

## 7. 读路径

### 7.1 层次、查找顺序与锁纪律

```
Get(user_key):
  [mutex_]  snapshot := last_sequence_
            mt := memtable_                                  // shared_ptr 拷贝
            imms := immutables_（整条 deque 的 shared_ptr 拷贝，新→旧）
            ver := version_                                  // shared_ptr<const Version> 拷贝
  [锁外]    lookup_key := BuildLookupKey(user_key, snapshot)
            ① mt->Get(lookup_key)                → kFound(kOk) / kDeleted(kNotFound) / kNotFound
            ② for imm in imms（back→front，即新→旧）: 同 ①
            ③ for f in ver->files()（**文件号降序 = 时间降序**，即新→旧）:
                   if (user_key < f.smallest.user_key || user_key > f.largest.user_key) continue;  // 零 IO
                   TableCache::Get(f, lookup_key)  → 命中即返回（含 kDeleted ⇒ NotFound）
            ④ 全部未命中 → Status::NotFound
```

**查找顺序的三条规则（写死，评审逐条对照）**：

1. **MemTable 先于 immutable 先于 SSTable**（最新数据在前）。同一 user key 的多个版本里，
   读路径**取第一个命中可见版本**的层，因为"新层里的版本 sequence 一定 >= 旧层里的任何版本"
   （写入顺序决定的；由 I31 的 `last_sequence_` 定义与 L15 的版本不可变性保证）。
2. **immutable 与 SSTable 内部都是"新→旧"**。immutable 的"新"由 `deque` 的 back 定义（最新冻结的在后）；
   SSTable 的"新"由**文件号**定义（号大 = 后写的 ⇒ 后冻结的表 ⇒ 更新的数据）。
   ⇒ **不需要**比较 key range 或 max_sequence 来决定谁新（那会引入第二套"谁更新"的判据）。
   L0 允许 key range 重叠（I23），所以**不能**遇到第一个"key 在范围内"的文件就停——必须**逐个**查到命中为止。
3. **tombstone 一旦命中就是终局**：`kDeleted` ⇒ 返回 `NotFound`，**不再向下查**。
   （"不得复活"的实现要点：不是"跳过 tombstone 继续找"，而是"tombstone 就是该 key 在该快照下的答案"。）

**锁纪律（L19）**：`Get` 在 `mutex_` 内**只**做"取引用 + 取快照"，**所有 IO 与比较都在锁外**。
这直接兑现 L18（禁止持 DB 锁做 IO）与 M1/M2 遗留的 I17 精神。
代价是 `Get` 不再"看见一个原子快照的 MemTable"，但它看见的是**一个一致的版本三元组**
（`last_sequence_` 与三层引用在同一个临界区内读取）⇒ 语义仍然正确：任何在此之后完成的 flush
只会**增加** SSTable（把已冻结表的内容搬进新文件），不会让"已经可见的版本"消失。

### 7.2 `Get` 的覆盖语义与 tombstone 屏蔽（逐场景）

| 场景 | `memtable_` | immutable | L0 新 | L0 旧 | 期望 | 依据 |
|---|---|---|---|---|---|---|
| 普通覆盖 | v2 | — | v1 | — | **v2** | 顺序规则 1 |
| 同 key 在 SSTable 与 MemTable 各一版 | v_new | — | v_old | — | **v_new** | `M3-A27` |
| MemTable tombstone 屏蔽 SSTable 旧值 | tombstone | — | v_old | — | **NotFound** | `M3-A28`、I26 |
| 新 SSTable 的 tombstone 屏蔽旧 SSTable | — | — | tombstone | v_old | **NotFound** | `M3-A29` |
| 三个文件同 key | — | — | v3 | v1,v2 | **v3**（最大可见 seq） | `M3-A30` |
| immutable 里的值覆盖已落盘文件 | — | v_new | v_old | — | **v_new** | `M3-A27`（子断言：`hit_layer == immutable`） |
| 全部未命中 | — | — | — | — | **NotFound** | `M3-A30`（子断言）+ `M3-B08` 的 `hit_layer == none` 计数 |
| tombstone 在**旧的**文件、值在**新的**文件 | — | — | v_new | tombstone | **v_new**（新文件先被查，命中即返回） | 顺序规则 2/3（`M3-A29` 的两向子断言） |

**"L0 内高层 tombstone 屏蔽低层旧值"这条（指令 A 组明写）**：实现上**不**需要任何特殊逻辑——
它就是"按文件号降序找、tombstone 命中即终局"，与场景 4 同一条路径。这正是"版本不可变 + 新→旧"
带来的**结构性**简单性（M4 引入层级后，这条会变成"从 L0 向下逐层找"，但**同一个函数**）。

### 7.3 `MergingIterator` / `DBIter` 的职责边界

```cpp
// src/merging_iterator.h
namespace lsm {
// 只做归并：N 路 child（每个都在 internal key 全序上），按 InternalKeyComparator 输出**内部 key**。
// **不做**任何可见性判断、**不**跳 tombstone、**不**转 user key。
// child 的风向：本类要求所有 child 都支持双向（M3 的 child 是 Skiplist::Iterator 与 Block::Iterator，
//   二者都实现 Iterator 全接口）。总方向 = 所有 child 的方向一致（LevelDB 同口径）。
class MergingIterator : public Iterator {
 public:
  // children：所有权归本类（析构时 delete[]）。icmp 不拥有。
  MergingIterator(const InternalKeyComparator* icmp, Iterator** children, int n);
  ~MergingIterator() override;
  // Valid / SeekToFirst / SeekToLast / Seek / Next / Prev / key / value / status
  //   key()/value() 指向**当前选中的 child**，有效期 = 该 child 的下一次定位调用；
  //   status() = 各 child status 的**第一个非 OK**（顺序扫描 children，确定性）
 private:
  void FindSmallest();      // 前向：在 n 个 child 里线性扫最小值（D7 的取舍）
  void FindLargest();       // 后向：同上
  const InternalKeyComparator* icmp_;
  Iterator** children_; int n_; Iterator* current_;
  Direction direction_;     // kForward / kReverse
};

}  // namespace lsm
```

```cpp
// src/db_iter.h
namespace lsm {
// 用户视图：**只**做 (a) sequence 可见性 seq <= snapshot、(b) 每个 user key 只出最新可见版本、
// (c) 跳过 tombstone、(d) internal→user key、(e) 三态状态机（kBeforeFirst/kValid/kPastEnd）。
// child：一个 internal key 序的迭代器（M3 传 MergingIterator；M1 的内存模式传单 child 的 MergingIterator）。
class DBIter : public Iterator {
 public:
  DBIter(const InternalKeyComparator* icmp, Iterator* internal, SequenceNumber snapshot);
  ~DBIter() override;
  // 语义与 M1 的 UserIterator **逐字相同**（含 Prev/SeekToLast 的"回到 restart 组/run 起点"算法）
 private:
  void ScanForwardToVisible();
  void ScanBackwardToVisible();
  void RewindToRunStart();          // = internal_->Seek(BuildLookupKey(当前 user_key, snapshot))
  ...
  Iterator* internal_;              // 拥有
  const InternalKeyComparator* icmp_;
  const SequenceNumber snapshot_;
  ...
};
}  // namespace lsm
```

**M1 的 `UserIterator` 怎么办（避免两份可见性实现漂移 —— M2 评审教训的同类）**：
`src/db_impl.cpp` 里的匿名类 `UserIterator` **提升**为 `DBIter`（同一个算法、同一个三态状态机），
`NewMemTableUserIterator(mem, icmp)` 保留为**薄包装**：

```cpp
Iterator* NewMemTableUserIterator(const MemTable* mem, const InternalKeyComparator& icmp) {
  auto** kids = new Iterator*[1];
  kids[0] = mem->NewIterator();
  return new DBIter(&icmp, new MergingIterator(&icmp, kids, 1), kMaxSequenceNumber);
}
```

⇒ **M1 的既有迭代器用例（含 `Prev`/`SeekToLast`/tombstone）一字不改地继续跑**，且它们现在**直接验证**
了 M3 的 `DBIter`。这是"最小改动 + 不引入第二份实现"的同时达成。

**`DBIter` 的生命周期（L19 / I35）**：`DBIter` 必须持住它访问的**每一个** `Table`/`Version`/`MemTable` 的引用，
否则"迭代期间发生 flush + 注册 + 释放内存"⇒ `Arena` 被释放而迭代器还在读 ⇒ UAF。
实现方式：内部迭代器（`Table::Iterator`）自己持 `shared_ptr<const Table>`，`Table` 持
`shared_ptr<RandomAccessFile>`；`MemTable` 的 child 由 `DBIter` 持有的 `shared_ptr<MemTable>` 保活。
`M3-A33` 在 ASan 下用"迭代到一半触发 flush + 注册"钉死这一条。

### 7.4 读路径缓存与读放大的测量口径

- **table cache**：`TableCache`（`src/version_set.{h,cpp}` 或独立文件，`#1` 定），键 = 文件号，
  值 = `shared_ptr<const Table>`，容量 `Options::max_open_files`（默认 64），LRU 淘汰。
  **淘汰时的 `Table` 析构会 `close` fd，必须在 `mutex_` 之外发生**（L19）。
- **无块缓存**（D8）。
- **`ReadStats` 的口径**见 D8；`PersistentDBImpl::GetReadStats()` 返回**累计**计数（M3 不做窗口/直方图，
  那属 M5 的微基准设施）。**B08 用它实测 §D8 的推算值**。

---

## 8. 版本元数据、启动恢复与 `0 丢写` 判据的保持

### 8.1 `Version` 与 `META`（M3 简化版：只有 L0 文件列表）

```cpp
// src/version_edit.h —— **不是** MANIFEST 的 VersionEdit：它是一个"全量快照"的内存表示 + 编解码
namespace lsm {
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  SequenceNumber max_sequence = 0;      // §10.8：文件内最大 sequence
  std::string smallest;                 // internal key
  std::string largest;                  // internal key
  uint64_t smallest_user_key_size() const;
};

// 全量快照的编解码（§10.8 的 META 格式），**没有** 追加/差分语义。
class VersionEdit {
 public:
  void SetLogNumber(uint64_t n);  uint64_t log_number() const;
  void SetMinLogNumberToKeep(uint64_t n);  uint64_t min_log_number_to_keep() const;
  void SetNextFileNumber(uint64_t n);
  void SetComparatorName(const std::string& n);
  void AddFile(const FileMetaData& f);
  const std::vector<FileMetaData>& files() const;
  // 编解码：EncodeTo(std::string*) / DecodeFrom(Slice, VersionEdit*, std::string* why)
  // 失败返回 false + why（含精确字段名与偏移）；不修改 *this 的任何已有字段。
  bool EncodeTo(std::string* dst) const;
  bool DecodeFrom(const Slice& src, std::string* why);
};
}  // namespace lsm
```

```cpp
// src/version_set.h（M3 简化版）
namespace lsm {
// Version 是**不可变**的（I21/L15）：构造后只读，注册 = make_shared 新对象 + 原子替换。
class Version {
 public:
  const std::vector<FileMetaData>& files() const;     // 按文件号**降序**（新→旧），构造时排好
  uint64_t log_number() const;
  uint64_t min_log_number_to_keep() const;
  SequenceNumber MaxSequenceInFiles() const;          // = max over files of f.max_sequence（§8.2）
};

// TableCache：文件号 → shared_ptr<const Table>，LRU，容量 Options::max_open_files
class TableCache { ... };

// M3 的"META 持久化"入口（§D4 的迁移点：M4 换成 MANIFEST 只改这个函数体）
class VersionSet {
 public:
  // 读 META（或：META 缺失时的兼容规则，§10.9）
  Status Recover(Env* env, const std::string& dbname, const Options& options,
                 Version** out, RecoveryStats* stats);
  // 把 new_version 作为**全量快照**持久化：META.tmp → fsync → rename → SyncDir
  Status Persist(Env* env, const std::string& dbname, const Version& v, VersionEdit* edit);
  static std::string MetaFileName(const std::string& dbname);   // dbname + "/META"
  static std::string MetaTempFileName(const std::string& dbname);  // dbname + "/META.tmp"
};
}  // namespace lsm
```

**为什么 `Recover` 的签名里没有"META"字样**：§D4 的迁移路径要求"`Open` 的恢复入口不暴露
'META 是文件还是日志'"。M4 改成 `MANIFEST` + `VersionEdit` 追加日志时，只改 `Recover`/`Persist` 的函数体。

### 8.2 `last_sequence_` 的唯一口径（I31 —— "0 丢写"的算术基础）

```
恢复后  last_sequence_ := max( max_replayed_seq_from_WAL , max_seq_present_in_registered_SSTables )
  其中  max_replayed_seq_from_WAL            = max over 重放到的每条 batch 的 (seq + count - 1)   （M2 逐字）
        max_seq_present_in_registered_SSTables = max over version_->files() of f.max_sequence        （§10.8）
```

**证明"任一已 ack 的写都被 `last_sequence_` 覆盖"**：

- 一个已 ack 的写，要么 (i) 已在某个**已注册**的 SSTable 里，要么 (ii) 只在 WAL 里（还没被 flush 覆盖）。
- (i) ⇒ 它的 sequence <= 该文件的 `max_sequence` <= `max_seq_present_in_registered_SSTables` <= `last_sequence_`。∎
- (ii) ⇒ 它所在的 log 编号 >= `min_log_number_to_keep`（I34 的必要性方向：编号更小的 log 的 record 都已注册）
  ⇒ 该 log 未被删除 ⇒ 恢复时会重放到它 ⇒ 它的 sequence <= `max_replayed_seq_from_WAL` <= `last_sequence_`。∎

**三条由此得出的硬纪律**（这是 M2 那条"绝对不能犯"的线的 M3 形态）：

1. **`META` 里不存"注册时刻的 `last_sequence_`"**。理由（必须写进 `#1` 的风险清单）：
   注册时刻的 `last_sequence_` 包含**当时还在 `memtable_`/`immutables_` 里、尚未进任何 SSTable** 的写。
   若拿它当"已持久化的水位"去做任何跳过/删 log 的判断，就会**丢**这些写。
   ⇒ 唯一允许持久化的水位是**逐文件的 `max_sequence`**（它的语义是"这个文件里真有的最大 sequence"，
   由 `TableBuilder` 在写块时顺带统计，**不是**推断出来的）。
2. **恢复期不做任何基于水位的跳过**。只保留 M2 的"文件内 sequence 非递增 ⇒ 跳过 + 计数上报"
   （`records_skipped`，M2 已实现）。跨文件的"这个 log 已被覆盖"判断**只**用于**删除**（I34），
   **不**用于跳过重放。⇒ "跳过"这一动作在 M3 **没有增加**任何一个新触发点（M2 评审观察项不扩大）。
3. **`META` 的 `max_sequence` 必须可被独立验证**：`M3-A38` 打开每个 `.sst` 全量扫一遍算真实
   `max_sequence`，与 `META` 的值比对，不等 ⇒ `kCorruption`。这条把"统计写错"从静默错误变成显式失败。

### 8.3 启动恢复算法（伪代码）

```
Status DB::Open(options, name, dbptr):
  ① 校验 Options（§8.5）；非法 ⇒ kInvalidArgument（**不得**进入后续路径）
  ② env := options.env ?: Env::Default()
     if (!FileExists(name)) CreateDir(name)          // 与 M2 同
     SyncDir(parent_of(name))                        // 本设计的加强项：目录项 durable（M2 缺失）
  ③ LockFile(LOCK) → ScopedFileLock（M2 的 D10，提前返回也要释放）
  ④ children := GetChildren(name)
  ⑤ 读版本元数据（VersionSet::Recover）：
     if (FileExists("META")):
        读全文件 → DecodeFrom → 校验：magic("LSMM") / format_version==1 / **comparator_name ==
             options.comparator->Name()**（不符 ⇒ kInvalidArgument，兑现 M2 的 D13 残留）/ tail CRC
        任一失败 ⇒ kCorruption（**不自动修复**）
        files := META.files；log_number := META.log_number；min_log_to_keep := META.min_log_number_to_keep
        next_file_number := max(META.next_file_number, max(目录编号) + 1)
     else:
        if (目录中存在 *.sst) ⇒ kCorruption("META 丢失但目录非空")            // ★ 安全阀（§10.9）
        files := {}；log_number := 0（未分配）；min_log_to_keep := 1
        next_file_number := max(1, max(目录编号) + 1)
  ⑥ 孤儿清理（**只清理可证明未被引用的**，逐类计数上报；失败只计数不阻断）：
      a) 删除所有 *.sst.tmp                —— 构造上永不注册（§6.3 步骤 ⑤ 之前崩溃）
      b) 删除编号 ∉ files 的 *.sst          —— 未注册残片（步骤 ⑤~⑦ 之间崩溃）
      c) 删除编号 < min_log_to_keep 的 *.log —— 已被注册的 SSTable 覆盖（I34）；**受 recycle_log_files 控制**
      ★ 读路径**必须容忍**孤儿存在（硬性约束 6）：即使清理失败，读路径也**只**走 version_->files()
        ⇒ 孤儿既不会被读入，也不会让 Open 失败。清理只是"顺手把垃圾收掉"。
  ⑦ 重放 WAL（集合 = 步骤 ⑥ 之后目录里**实际存在**的 *.log，按编号**数值升序**）：
     for each log（升序）:
        WALReader::ReadAll(...) → WALScanResult
        verdict kClean        → 全部重放
        verdict kTailResidue  → **只有最高编号的 log** 允许截断到 last_good_end（M2 §5.3 逐字）；
                                否则 kCorruption
        verdict kParseFail    → valid_record_after_failure ⇒ kCorruption（中间损坏）；
                                否则（且是最高编号）截断；否则 kCorruption
        重放每条 batch：seq <= last 则 ++records_skipped 并跳过（M2 逐字）；否则 memtable->Add(...)
        ★ 永不因容量失败：恢复后的 memtable 容量按 §8.4 放大（M2 的 D12 机制沿用）
  ⑧ last_sequence_ := max(max_replayed_seq, version->MaxSequenceInFiles())            // §8.2
  ⑨ 当前 log：若 log_number == 0（META 缺失）⇒ 分配 next_file_number_ 作为 log 编号；
              否则沿用 log_number；文件不存在则创建（计数上报，正常情况下不该发生）
     打开为追加（NewAppendableFile），block_offset 由现有大小推出（M2 逐字）
  ⑩ 重放出来的数据留在 memtable_；**不**在 Open 里主动 flush（M3 无 DB::Flush()，见 §8.7 E4）
     ⇒ 这份 memtable 的 log_number = min(被重放的 log 编号)（若没有重放任何 log，则 = log_number）
       ★ 这一条是 I34 的必要项：否则恢复出来的数据会在"被删掉的 log"里只剩内存一份（§6.6.2 证明 4）
  ⑪ StartBackgroundThread()（L12：恢复**之后**才启动）
  ⑫ db->file_lock_ := lock_guard.release()；db->closed_ := false；*dbptr := db.release()
```

### 8.4 `RecoveryStats` 与 `FlushStats`（"截断/跳过/丢弃必须计数 + 上报"的落点）

`RecoveryStats`（M2 的字段**只增不改**）：

| 字段 | 来源 | 新增? |
|---|---|---|
| `log_files` / `records_replayed` / `entries_replayed` / `records_skipped` / `tail_truncated_bytes` / `last_sequence` / `truncation_note` | M2 | 否（语义不变） |
| `meta_present` | 步骤 ⑤ | **是** |
| `sst_files_registered` / `sst_bytes_registered` | 步骤 ⑤ | **是** |
| `orphan_tmp_removed` / `orphan_sst_removed` / `orphan_bytes_removed` | 步骤 ⑥ a/b | **是** |
| `obsolete_logs_removed` / `obsolete_log_bytes_removed` | 步骤 ⑥ c | **是** |
| `orphan_remove_failed` | 步骤 ⑥ 任一失败 | **是** |
| `max_sequence_in_files` | 步骤 ⑧ | **是**（`M3-A38` 的比对对象） |
| `records_skipped`（已有） | M2 | 否 |
| `unknown_metaindex_entries` | 读 `.sst` 时（§3.4） | **是** |

`FlushStats`（M3 新增，`PersistentDBImpl::GetFlushStats()` 只读暴露）：

| 字段 | 含义 |
|---|---|
| `flushes_started` / `flushes_completed` / `flushes_failed` | 计数 |
| `immutables_abandoned` | `Close()` 时被放弃的 immutable 数（§6.5） |
| `stall_events` / `stall_micros` | 写者因 `kMaxImmutableMemTables` 停等的次数与总时长 |
| `rotations` / `rotate_failed` | WAL 轮转次数与失败次数 |
| `log_files_deleted` / `log_bytes_deleted` | WAL 回收的实际效果（§6.6.2） |
| `index_size_warn` | 索引超过阈值（§3.3）的次数 |
| `last_error` | 最近一次失败的 `Status::ToString()`（可读） |

**纪律**：这两张表是"不得静默"的**单一落点**。任何一处丢弃/跳过/截断/删除，都必须先在这里加一个计数
（`#1` 把它做成硬约束：**新增任何丢弃路径而不加计数 = 评审阻断项**）。

### 8.5 `Options` 的合法性校验（输入校验**不得**触发 fail-stop —— M2 评审教训 4）

在 `Open` 的**第一步**（§8.3 ①）一次性校验，全部返回 `kInvalidArgument`（**不是** `kCorruption`、**不**置 `bg_error_`）：

| 输入 | 判据 | 行为 |
|---|---|---|
| `comparator == nullptr` | — | `kInvalidArgument` |
| `write_buffer_size == 0` | — | `kInvalidArgument` |
| `block_size` | `< 512` 或 `> 1 MiB` | `kInvalidArgument`（下界防"每块 1 条 ⇒ 索引爆炸"，上界防"一次 Seek 读太多"） |
| `max_open_files` | `== 0` 或 `> 1000000` | `kInvalidArgument` |
| `kRestartInterval` | 编译期常量，不入 `Options`（避免可配出非法值） | — |
| `verify_checksums` | `bool`，任意值合法 | — |
| `Put`/`Delete` 的 key | 空 / `> kMaxUserKeySize` | `kInvalidArgument`（M2 已有，**不入队**） |
| `Put`/`Delete` 的 `entry_bytes + 16 > kMaxLogicalRecordSize` | — | `kInvalidArgument`（M2 评审优化项已修，**不入队**） |

> **反面教材（必须避免）**：M2 曾有"超大 value 触发粘性写只读"（`PUT 65MiB -> InvalidArgument` 之后
> 写与 `Close` 全部返回同一错误）。M3 的所有输入校验都必须**在入队之前**、**不写任何状态**、
> **不置 `bg_error_`**。`M3-A53` 专门钉这一条。

### 8.6 `0 丢写` 判据如何在 M3 保持

M2 的判据是：`kill -9` 循环 ⇒ 重启 ⇒ "已 ack 集合（sidecar）⊆ 恢复后 `Get` 到的集合"，逐字节相等，
`missing != 0` ⇒ 退出码 1。M3 之后**多了 SSTable**，判据本身**不变**（因为它是对 `Get` 的黑盒断言），
但**证据强度必须加强**，否则会出现 M2 已经踩过的"空绿"（评审阻断项 3）：

| 加强项 | 内容 | 防的是什么 |
|---|---|---|
| G1 | `ROUND` 行格式不变：`ROUND n ACKED x RECOVERED y MISSING m MISMATCH k …` | 保持与 M2 脚本的口径可比 |
| G2 | `RC != 0` 或缺 `ROUND` 行 ⇒ 立即 `exit 1`（M2 已修，**不许回退**） | "工具没跑起来"被当成 `missing 0` |
| G3 | 新增 `SST_FILES_TOTAL`（恢复后目录里 `.sst` 的数量）与 `SST_FILES_MAX` | **防"flush 一次都没发生"的空绿**：SSTable 从未落盘的门禁与"真落盘"的门禁必须可区分 |
| G4 | 新增 `RECORDS_REPLAYED_TOTAL` 与 `LOGS_DELETED_TOTAL` | 证明"读路径真走了 SSTable + WAL 回收真生效"（否则 M3 的门禁可能只是 M2 门禁的复制品） |
| G5 | **小写缓冲强制 flush**：M3 的崩溃脚本显式传 `--write-buffer-size`（默认 256 KiB），使每轮都发生 ≥1 次 flush 与轮转 | 否则 4 MiB 默认值下 100 轮里可能一次 collapse 都不发生 |
| G6 | ASan/TSan 下也要跑这套（至少 10 轮） | 迭代器/flush 并发的 UAF 只在 sanitizer 下暴露 |
| G7 | 门禁脚本必须有**正向标记**（`[..._OK]` 行），且 `lsm_gate.sh` 断言标记出现 | 比 M2 更强：M2 只查退出码，"退出 0 但什么都没做"仍可能过 |

⇒ `M3-B01` 的通过判据 = `ROUNDS_OK == ROUNDS` ∧ `MISSING_TOTAL == 0` ∧ `MISMATCH_TOTAL == 0`
∧ `SST_FILES_TOTAL > 0` ∧ `LOGS_DELETED_TOTAL > 0` ∧ 脚本末尾打印 `[FLUSH_CRASH_OK]`。

### 8.7 补充决策（指令 8 条之外，本设计新增；编号 E1~E8）

> 这 8 条是上述设计**必然牵出**的取舍，指令没有列出但必须拍板，否则 `#1` 会凭空补。

| # | 决策 | 方案与取舍 | 推荐 | 拍板 |
|---|---|---|---|---|
| **E1** | **冻结是否还需要 `MemTable::Freeze()`** | (a) 冻结时仍调 `Freeze()` 置 `frozen_`；(b) 不调，靠"已移入 `immutables_`"的所有权事实 | **(b)**。理由：M3 起 `frozen_` 对持久化路径无功能作用（表已不可写）；调它只会制造"两处说同一件事"（M2 教训）。**但**必须在 `#1` 登记这条语义收窄（§6.2 的登记块），并且**内存模式（M1 的 `DBImpl`）保持调 `Freeze()`**（它没有 flush，仍靠 `frozen_`） | 否 |
| **E2** | **`.sst` 的 `smallest`/`largest` 用 internal key 还是 user key** | (a) internal key（LevelDB）；(b) user key | **(a)**。理由：META 里存 internal key 才能在 M4 做"按内部 key 切分"的 compaction，且能用同一份数据校验"文件内序正确"；user key 可从它提取（`ExtractUserKey`），反之不成立 | 否 |
| **E3** | **`META.max_sequence` 是否冗余（与 `largest` 的 sequence 不同）** | 见 §8.2 纪律 1：必须用"文件内最大 sequence"，**不是** `largest` 的 sequence | **必存 `max_sequence`**，并在 `#1` 里把这两个量**用不同名字**写死（`max_sequence` vs `largest`），避免"同名不同义" | 否 |
| **E4** | **是否新增公共 `DB::Flush()`** | (a) 新增 ⇒ 立刻可测"纯 SSTable 启动"；(b) 不新增，用**小 `write_buffer_size`** 驱动 flush | **(b)**。理由：`src/db.h` **不在** M3 的「必须改」清单里（指令 §1 的清单只列了 `db_impl.{h,cpp}`/`env.*`/`CMakeLists.txt`/`.gitignore`）⇒ 新增公共 API 是**超出授权的契约变更**；且 (b) 完全够用（§10 的 `M3-B03` 用小 buffer 驱动）。**若用户愿意批准 (a)，M3 的实现与所有判据不变，只是多一个入口** | **是**（§13 Q6） |
| **E5** | **`Options::write_buffer_size` 的语义是否要改文档** | 目标值 vs 硬上限 | **改成明文的"目标值"**（§1.3.2 / §6.2），并在 `#1` 把 M1/M2 文档里任何"上限"措辞收窄。**不改字段、不改 `MemTable` 的判据** | 否 |
| **E6** | **`TableCache` 放哪个文件** | (a) 独立 `src/table_cache.{h,cpp}`；(b) 塞进 `version_set.{h,cpp}` | **(b)**。理由：M3 的 cache 只有 ~40 行，且它的键（文件号）与 `Version::files()` 同源；`#1` 若认为它会变胖可以再拆（拆的时候依赖方向不变） | 否（`#1` 可改） |
| **E7** | **`MergingIterator` 是否要 `Prev` 的 O(N) 反向线性扫描** | (a) 反向也用线性扫最小/最大（LevelDB 同）；(b) 堆 | **(a)**。D7 已论证 | 否 |
| **E8** | **`flush_hook` 的接口形态** | 与 `CommitHook` 同纪律：`class FlushHook { virtual void OnSSTableWritten(); virtual void OnBeforeRename(); virtual void OnBeforeRegister(); }`，`Options::flush_hook = nullptr` | **如上**。三个注入点**逐字对应**指令 §2 B 组的"写文件后 / rename 前 / 注册前"三个 kill -9 点；生产恒为 `nullptr` | 否 |

---

## 9. 不变量与锁纪律的增量（I21~I34 / L13~L21）

> 格式对齐 `docs/m2-design.md` §1.4：每条给"谁保证（代码位置）+ 怎么验（用例名）"。
> **I21~I30 与 L13~L18 的定义逐字取自 `M3-SSTable与刷盘.md` §1 的 `#1` 段**（`#1` 直接抄定义即可）；
> **I31~I34 与 L19~L21 是本设计新增的**（`#1` 必须收录，编号可重排但**不得丢条目**）。
> 表里的"保证点"指向本文档的章节号，`#4` 评审按此逐条找到代码位置。

### 9.1 不变量 I21~I34

| # | 不变量（一句话） | 谁保证（落地位置） | 怎么验（用例） |
|---|---|---|---|
| **I21** | SSTable 一旦被版本注册即**不可变**：注册后禁止任何原地修改，只能整体重写为新文件 | §6.3（写入的是 `%06u.sst.tmp`，`rename` 之后只读打开）；`Table` 与 `TableBuilder` **没有**任何写接口（§5.4/§5.2）；`Table` 只持 `std::shared_ptr<RandomAccessFile>`（只读） | `M3-A22`（事件序列里**没有**任何对已注册文件的写/rename）、`M3-B05`（多轮 flush 后已注册文件的 mtime/size 不变） |
| **I22** | flush 顺序必须是 `durable → rename → (SyncDir) → 注册`：`write+fsync` 成功、`rename` 成功之后才允许写版本元数据；任何一步失败不得注册 | §6.3 的步骤 ④⑤⑥⑦（顺序不可交换）+ §6.4 的失败矩阵 | `M3-A22`（**一条用例三条子断言**：`FlushHook::OnBeforeRename` 时注册尚未发生；Env 事件日志里 `fsync` 早于 `rename`；`SyncDir` 早于 `META` 的写）、`M3-B02`（三个注入点的进程级 kill -9） |
| **I23** | L0 内文件 key 范围允许重叠，故读路径必须按文件**新→旧**逐个检查，命中第一个可见版本即返回 | §7.1 的顺序规则 2（按**文件号**降序，不用 key range 判新旧） | `M3-A30`（三文件同 key）、`M3-A29`（新文件 tombstone 屏蔽旧文件）；`M3-B08`（`files_checked` 实测 > 1） |
| **I24** | 块内 restart 点有序（offset 单调递增且指向块内合法边界），保证块内二分 Seek 正确 | §3.2 的 restart 语义（唯一解释）+ §5.3 的二分规则；写入侧 `BlockBuilder`（§5.2），校验侧 `ValidateBlock`（§5.3） | `M3-A03`（restart 单调 + 边界合法）、`M3-A04`（Seek 四种语义）、`M3-A02`（组间不共享前缀） |
| **I25** | 索引项与数据块偏移自洽：每个索引项的块偏移/长度必须在文件边界内且与写入时一致 | §3.6 的"`handle.size` 是读多少的唯一真相源"+ 三条校验（长度自洽 / 类型匹配 / 索引紧贴 footer，§3.7） | `M3-A16`（索引 handle 指向 data 块 ⇒ 类型不符 ⇒ `kCorruption`）、`M3-A08`（handle 越界）、`M3-A17`（`handle.size` 与块内 `length` 不符） |
| **I26** | tombstone 在底层不得被误丢：M3 不删任何旧版本，tombstone 必须能屏蔽更旧版本 | §7.2 的 8 个场景表；`TableBuilder::Add` **不**做任何"删除/合并/丢弃"（§5.4 的 `Add` 只有切块与写块）；flush **不**做 compaction | `M3-A28`、`M3-A29`；`M3-A26` 的"写完的文件里 tombstone 条数 == 冻结时统计的条数"断言 |
| **I27** | 恢复后的版本 = **最后一次成功注册**的状态：半写/未注册文件不得进入版本 | §8.3 的步骤 ⑤⑥（版本只来自 `META`；`.tmp` 与未注册 `.sst` 一律当孤儿）+ §10.9 的安全阀 | `M3-A40`（只有 `.tmp` ⇒ 不注册 + 未注册 `.sst` ⇒ 不读入 + 被清理 + 计数）、`M3-A41`（META 缺失且有 `.sst` ⇒ 拒绝启动）、`M3-B02` |
| **I28** | 迭代器不得返回不可见版本（sequence 过滤）：只输出 `<= 读快照 sequence` 的版本 | §7.3 的 `DBIter`（`seq <= snapshot` 的判断在 `ScanForwardToVisible`/`ScanBackwardToVisible`）+ D7 的快照来源 | `M3-A32`（快照外的版本不可见、tombstone 之后 Seek 落点、正反向一致——同一条用例的三个子断言） |
| **I29** | 读路径不得把 tombstone 返回给用户：`DBIter` 跳过 tombstone，`Get` 遇 tombstone 返回 `NotFound` | §7.1 的 `Get` 步骤（`kDeleted ⇒ NotFound`，不再向下）+ §7.3 的 `DBIter` | `M3-A28`、`M3-A29`、`M3-A32` |
| **I30** | 文件句柄数量有上限且必须 RAII 释放：table cache 满时按 LRU 释放，任何路径不得泄漏句柄 | D8 的 `TableCache`（容量 `Options::max_open_files`）+ §7.4（淘汰的 `close` 在 `mutex_` 之外）+ `shared_ptr` RAII | `M3-B05`（多轮 flush + Get 后 `/proc/self/fd` 计数不增长）；`M3-A24`（失败路径也不泄漏） |
| **I31**（新增） | **恢复水位的唯一口径**：`last_sequence_ = max(WAL 重放最大值, 各已注册文件的 `max_sequence`)`；**禁止**用"注册时刻的 `last_sequence_`"当已持久化水位 | §8.2（含三段证明与三条纪律）；`Version::MaxSequenceInFiles()`（§8.1） | `M3-A37`（水位单调 + 下一次分配严格更大）、`M3-A38`（`META.max_sequence` 与全量扫描一致，不符 ⇒ `kCorruption`）、`M3-A36`（SSTable + WAL 尾巴组合） |
| **I32**（新增） | **`Sync()` 的水位边界**：`Sync()` 返回 `kOk` ⟹ 此前所有已 ack 写的字节已 `fsync` **到当前 log**；水位只按"当前 log **已追加**的边界"（`log_last_appended_seq_`）发布，不得按"已分配 sequence 的边界" | §6.1 的 `log_last_appended_seq_` + §6.6.1 阶段 C（`Append` 成功后持 `commit_mu_` 更新）+ §0.5 的缺陷分析 | M2 侧已由 `GroupCommit.SyncDoesNotClaimInFlightBatch`（`a3c85a8`）覆盖；M3 侧 `M3-A50` 收窄为 **per-log** 边界回归（轮转后不得把旧 log 的未落盘边界当成 durable） |
| **I33**（新增） | **轮转的原子性位置**：轮转在**分配 sequence 之前**完成；轮转失败 ⇒ 整批以非 `kOk` 拒绝、**不** `Append`、**不**推进 `sequence`、**不** `Add` | §6.6.1 的阶段 A / A' / B 划分（+ M2 阻断项 1 的"判不过整批拒绝"纪律） | `M3-A49`（注入 `SyncDir`/写失败于轮转 ⇒ 断言 sequence 未推进、`log_` 未换、`memtable_` 未变、整批同一 `Status`） |
| **I34**（新增） | **WAL 回收判据**：`min_log_to_keep = min({pending 表的 log_number})`；只有编号**严格小于**它的 `.log` 可删；删除**必须**在 `META` 的 `rename + SyncDir` 之后 | §6.6.2（判据 + 五步证明 `RecomputeMinLogToKeep()`）+ §6.3 步骤 ⑦→⑨ | `M3-A45`（判据本身：构造多表/多 log 场景断言可删集合）、`M3-A47`（注入 Env 断言删除晚于 `SyncDir`）、`M3-B09`（磁盘效果：log 数有界） |

**关于 I34 的"必要性"与 `Open` 的交互（易错点，单独强调）**：§8.3 步骤 ⑩ 要求"恢复出来的 `memtable_` 的
`log_number` = 被重放 log 的**最小**编号"。漏掉这一条会**直接丢数据**：
恢复后如果给这份 memtable 记 `log_number = log_number_`（当前 log），那么下一次 flush 注册后
`min_log_to_keep` 会跳过那些"只被这份 memtable 覆盖"的老 log，把它们删掉；此时若进程崩溃，
而这份 memtable 还没被 flush ⇒ 数据只剩被删掉的 log 一份 ⇒ **永久丢失**。
`M3-A46` 必须包含这个形态。

### 9.2 锁纪律 L13~L21

> **约定**：M3-Axx 指 §10.1 的 A 组用例（逐条对照，不引用未定义编号）。

| # | 纪律 | 落地方式 | 验证 |
|---|---|---|---|
| **L13** | 后台 flush 线程与前台写/读的交互：MemTable 满由**前台写者**触发冻结（内存操作、持 DB 锁）并唤醒后台线程；落盘（write/fsync/rename/SyncDir）在后台线程且**不持 DB 锁** | §6.2（冻结在阶段 A，纯内存）+ §6.3（整段落盘无锁）+ §6.5（`bg_cv_` 唤醒） | `M3-A23`（`DbMutexHeldOnThisThread` + `SpyEnv` 断言持锁期 IO 调用数 == 0）；`M3-A20`（自动落盘） |
| **L14** | DB 互斥锁保护的内存状态清单必须写死并评审：`memtable_`、`immutables_`、`version_` 指针、`last_sequence_`、`log_number_`、`log_sealed_`、`next_file_number_`、状态机；**禁止**把 `Table`/`TableReader`/文件句柄纳入 DB 锁保护范围 | §6.1 的表格（逐行列出"状态 → 保护者"）；`TableCache` 自持互斥量（§7.4），**不**在 `mutex_` 保护范围内 | 代码评审逐行核对 §6.1 表 + `M3-A23` |
| **L15** | 版本对象的引用计数与生命周期：版本用 `shared_ptr` 持有；读/迭代器在操作期间持住版本引用；flush 注册时**构造新版本并原子替换**，禁止原地修改已发布版本 | §8.1（`Version` 不可变）+ §7.1（`Get` 在锁内拷 `shared_ptr`）+ §6.3 步骤 ⑦ | `M3-A33`（迭代期间注册新版本，结果不变 + ASan 干净）；`M3-A22` |
| **L16** | 文件删除必须延迟：文件只有在**不被任何版本引用**且**无在读句柄**时才允许删除；禁止在注册新版本的同一临界区里 `unlink` 正在被读的文件 | §6.3 步骤 ⑨（删除在锁外、且**只**删 log）；M3 **不删**已注册的 `.sst`（§1.2 非目标）；log 的删除与"是否被引用"解耦（log 不被 `Version` 引用） | `M3-A47`；`M3-B05` |
| **L17** | `CURRENT` 指针读写的原子性……**本设计不引入 `CURRENT`**（D4 选 D：单快照 `META` + rename）。等价纪律落为：**`META` 的更新一律 `写 .tmp → fsync → rename → SyncDir`，禁止原地覆写** | §8.1（`VersionSet::Persist`）+ §3.1（`META.tmp` 命名）+ L17 的**加强版**在 §6.3 步骤 ⑥（rename 之后必须 `SyncDir`） | `M3-A47`（断言删除晚于 `META` 的 `rename`+`SyncDir`）；`M3-A40`/`M3-A41`（`META.tmp` 残留 + META 缺失的语义） |
| **L18** | 禁止持 DB 锁做 IO 的延续：flush 的 write/fsync/rename/SyncDir 与 `TableReader` 的块读取一律在 DB 锁外；后台线程持 DB 锁的时间只允许覆盖内存状态变更 | §6.3（③④⑤⑥⑨ 全在锁外）+ §7.1（`Get` 只锁内取引用）+ §6.5（⑤ 在 `commit_mu_` 下做 `log_->Sync()`——**这是 M2 的既有例外**，M3 不变：`commit_mu_` 不是 DB 互斥锁，M2 的 I17/L7 原文即如此界定） | `M3-A23`；**M2 的 `Locks.ZeroIoWhileHoldingDbMutex` 用例（A25）不得回退**（它正是 `~/lsm-kv` 脏 WIP 删掉的那个探针，见 §0.1 W1） |
| **L19**（新增） | 读句柄（`Get`/`NewIterator`）在 `mutex_` 内**只**做"取 `shared_ptr` 引用 + 取快照"；一切 IO 与遍历在锁外。**禁止**把 `MemTable*`/`Version*` 裸指针带出 `mutex_` | §7.1（`Get` 的三行取引用）+ §6.1（`memtable_` 改 `shared_ptr`）+ §7.3（`DBIter` 持引用） | `M3-A23`（SpyEnv 断言 `Get` 的块读在锁外）；`M3-A33`（ASan 下并发 flush 不 UAF） |
| **L20**（新增） | 只有**当前** flusher（持 `flusher_active_`）允许创建/安装/轮转 log；后台 flush 线程**只读** `log_number_`，**永不**写 WAL、**永不**取 `commit_mu_` | §6.6.1（轮转在 flusher 的 IO 窗口内）+ §6.1（后台线程只取 `mutex_`） | 代码评审核对"后台线程函数体内无 `commit_mu_`"；`M3-A49`；TSan 全量 |
| **L21**（新增） | `immutables_` 中 `MemTable` 的析构（`Arena` 释放）只允许在 (a) 已注册进版本 **且** (b) 无读句柄持引用之后发生；由 `shared_ptr` 引用计数保证。禁止在 `mutex_` 临界区内做 `reset()` 之外的释放动作（那会持锁释放 `Arena` 与 `Table`，把 IO/大块释放塞回锁内） | §6.1（`shared_ptr`）+ §6.3 步骤 ⑦（只做 `pop_front`，让最后一个引用自然释放）+ §7.3（迭代器持引用） | `M3-A33`（ASan + 迭代期间 flush）；`M3-A25`（停等路径不放锁死锁） |

**这张表的用途**：`#1` 写 `docs/m3-prerequisites.md` 时逐行抄"保证点"与"验证用例"两列即可；
`#4` 评审时逐行核对"保证点"的代码位置是否真的成立。

---

## 10. 测试矩阵

> A 组 = 确定性（`MemEnv` + `FlushHook` + 注入 Env，无真实磁盘、零 flaky）。
> B 组 = 真实磁盘 / 进程级（脚本驱动）。
> 每条给：**依赖假设 / 通过判据 / 需要的 seam**。
> 指令 §2 明写的四类必含用例分别标为 **[落盘重启]**、**[覆盖+tombstone]**、**[CRC 检出]**、**[与 M2 门禁共存]**。

### 10.1 A 组（确定性）

| 编号 | 用例名（GTest） | 依赖假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| M3-A01 | `Block.RoundTripEmptySingleMany` | 无 | 空块 payload **恰为 8 B**（`restart[0]=0`+`count=1`）；单条/多条往返 key/value 逐字节相等；`CurrentSizeEstimate() == Finish()` | 无 |
| M3-A02 | `Block.PrefixCompressionAndRestartGroups` | 无 | 共享前缀 0 / 1 / 整条相同 / 整条不同 四种边界都正确解码；第 17 条成为新 restart 点（`shared == 0`）；**组间不共享前缀**（构造"相邻两组有公共前缀"的 key 断言第二组首条 `shared == 0`） | 无 |
| M3-A03 | `Block.RestartOffsetsMonotonicAndAligned`（I24） | 无 | `ValidateBlock` 通过；手工破坏成"非单调"或"指向非边界"⇒ `kCorruption` + 精确偏移 | `ValidateBlock` |
| M3-A04 | `Block.SeekSemantics` | 无 | 命中已有 key；Seek 到两块之间的不存在 key **落在后一块**（`Table` 级见 A13）；Seek 到最后一个块之后 ⇒ 越过末尾；空块 ⇒ Invalid；块内组边界处 Seek 正确 | 无 |
| M3-A05 | `Block.MalformedEntryRejectedWithoutOOB` | 无（ASan 下跑） | `shared > 上一条 key 长度`、`shared+non_shared > 剩余`、`vlen > 剩余`、`restart_count == 0` 全部 ⇒ `kCorruption`，**且 ASan 无越界读报告** | 无 |
| M3-A06 | `Block.IteratorBidirectional` | 无 | `SeekToFirst`→`Next`×n 与 `SeekToLast`→`Prev`×n 得到**完全相同**的逆序序列；`Prev` 在组边界正确（跨组回退） | 无 |
| M3-A07 | `Footer.RoundTrip` | 无 | 编码 → 解码逐字段相等；`kFooterSize == 44` | 无 |
| M3-A08 | `Footer.RejectMatrix` | 无 | 6 行矩阵逐条（§3.7）：文件 < 44 / magic 坏 / version=2 ⇒ **`kNotSupported`** / `footer_crc` 坏 / handle 越界 / metaindex 与 index 顺序错 / index 未紧贴 footer ⇒ `kCorruption` | 无 |
| M3-A09 | `Table.EmptyAndBoundarySizes` | `MemEnv` | 0 条（空表）/ 1 条 / 恰好落在 `block_size` 边界 / 1 字节之差 四种表都能打开并读回全部 key | `MemEnv`（或临时目录） |
| M3-A10 | `Table.CrossBlockLargeValue` | `MemEnv` | 单条 value = 3×`block_size`（且 > `kRestartInterval` 条数的块）能完整读回；该块的 `max_block > block_size`（证明"目标值不是硬上限"） | `MemEnv` |
| M3-A11 | `Table.LargeNumberOfBlocksIndexWarn` | 无（纯 `TableBuilder` + 计数） | `block_count > 65536` 时 `index_size_warn` 计数递增，**不**阻断，`Finish()` 仍成功 | 计数器 |
| M3-A12 | `Table.CrcDetectsSingleByteFlipInDataBlock` **[CRC 检出]** | `MemEnv` | 对数据块 payload 的**每一个字节**逐字节翻转（≥1000 次），**全部**必须 `kCorruption`（`verify_checksums=true`）；**零**"静默返回错值" | `MemEnv::SetContents`（字节级手术）+ `verify_checksums` |
| M3-A13 | `Table.CrcDetectsFlipInHeaderLengthAndTrailer` **[CRC 检出]** | `MemEnv` | 翻转块的 `length` 字段 4 个字节中的任意一个、`type` 字节、块尾 CRC 字节 ⇒ 全部检出（`length` 变**小**时靠"`handle.size` 不等"检出，变大时靠 CRC 检出——两条都要断言） | `MemEnv` |
| M3-A14 | `Table.CrcDetectsFlipInIndexAndFooter` **[CRC 检出]** | `MemEnv` | 索引块 payload / `index_handle` / `metaindex_handle` / `footer_crc` 各翻转 1 字节 ⇒ 全部 `kCorruption`；**零**静默错值 | `MemEnv` |
| M3-A15 | `Table.VerifyChecksumsOffIsActuallyOff` | `MemEnv` | 关掉校验后：① 结构校验**仍然生效**（畸形 `length` 仍 `kCorruption`）；② 只损坏 payload 的 CRC **不再**报错（且读到的值就是被改坏的值）⇒ 证明开关**真的有作用**，不是死代码 | `MemEnv` |
| M3-A16 | `Table.IndexHandleTypeMismatchRejected`（I25） | `MemEnv` | 把某个索引项的 handle 指向另一个**类型**的块（data ↔ metaindex）⇒ `kCorruption`（类型不符） | `MemEnv` |
| M3-A17 | `Table.HandleSizeVsBlockLengthMismatch`（I25） | `MemEnv` | 手工把 `handle.size` ±1 ⇒ `kCorruption`（§3.6 的冗余自检），且**不**发生越界读 | `MemEnv` |
| M3-A18 | `Table.MetaIndexEmptyBlockParses`（M5 预留） | 无 | M3 写的 metaindex 为 **0 条**；`type == kBlockTypeMetaIndex`；人为塞一条未知 `name` ⇒ 只计数 `unknown_metaindex_entries`，不报错 | 无 |
| M3-A19 | `Table.KeyRangeFilterDoesZeroIo`（D8 口径） | 计数 Env | 范围外 key ⇒ `kNotFound` 且 `ReadStats.blocks_read == 0` 且 `key_range_skipped == 1` | `ReadStats` + 计数 Env |
| M3-A20 | `Flush.AutoFlushOnFullBufferNeverFrozen` | `MemEnv` + 小 `write_buffer_size` | 写 N 条（N ≫ 容量）**全部返回 `kOk`**，**没有任何一次 `kFrozen`**；`flushes_completed >= 1`；全部 key 可读回 | `MemEnv`、`GetFlushStats()` |
| M3-A21 | `Flush.SingleEntryLargerThanWriteBufferAccepted` | `MemEnv`，`write_buffer_size = 4 KiB`，单条 value = 64 KiB | 写返回 `kOk`；可读回且逐字节相等；**无** `bg_error_`（随后再写 10 条普通 key 仍 `kOk`）——**防"粘性写只读"复活** | `MemEnv` |
| M3-A22 | `Flush.OrderDurableRenameSyncDirRegister`（I22） | `FlushHook` + 计数 Env | 事件序列严格为 `OnSSTableWritten → (fsync) → OnBeforeRename → (rename) → (SyncDir) → OnBeforeRegister → (META)`；用 Env 的事件日志断言"`fsync` 早于 `rename`、`SyncDir` 早于 `META` 的写" | `FlushHook` + 事件日志 Env |
| M3-A23 | `Flush.NoIoWhileHoldingDbMutex`（L13/L18/L19） | `DbMutexHeldOnThisThread` + SpyEnv | flush 的 `write/fsync/rename/SyncDir` 与 `Get` 的块读，**在持锁期间调用次数 == 0** | SpyEnv + A25 探针（**M2 已交付，M3 不得删**） |
| M3-A24 | `Flush.FailureIsFailStopButReadable`（I22/§6.4） | 注入 Env，逐个失败点 | 每个失败点：后续 `Put` 返回**同一** `Status`；`Get` 仍能读到**内存中**的值；`*.log` **未被删**；`flushes_failed == 1`；失败后 `Close()` 返回该错误且资源已释放 | 注入 Env（write/fsync/rename/SyncDir 各自可失败） |
| M3-A25 | `Flush.ImmutableStallReleasesOnAllConditions` | `FlushHook`（慢 flush，用假时钟/屏障） | ① `immutables_.size() == 2` 时写者停等；② 后台完成后被唤醒；③ `bg_error_` 置位时停等立即解除；④ `closed_` 时停等立即解除（**不得**死锁/丢唤醒）；⑤ `stall_events >= 1` | `FlushHook` + `FakeClock`（MemEnv） |
| M3-A26 | `Flush.TombstoneCountPreserved`（I26） | `MemEnv` | 冻结时统计 tombstone 条数；对落盘的 `.sst` 全量扫一遍统计 `type == kTypeDeletion` 的条数 ⇒ **两者相等**（flush 不丢 tombstone） | 无 |
| M3-A27 | `Read.CoverageMemTableOverSSTable` **[覆盖+tombstone]** | `MemEnv` + 强制 flush | 同 key 在 SSTable 与 MemTable 各一版 ⇒ 读到 MemTable 版 | `MemEnv` |
| M3-A28 | `Read.TombstoneShadowsSSTableOldValue` **[覆盖+tombstone]** | 同上 | `Put`→flush→`Delete` ⇒ `Get` 返回 `kNotFound`（**不得复活**） | `MemEnv` |
| M3-A29 | `Read.NewerSSTableTombstoneShadowsOlderFile` **[覆盖+tombstone]** | 同上，两个文件 | 旧文件有值、新文件有 tombstone ⇒ `kNotFound` | `MemEnv` |
| M3-A30 | `Read.NewestWinsAcrossThreeFiles` **[覆盖+tombstone]** | 同上，三个文件同 key | 读到序号最大的可见版本；`ReadStats.files_checked >= 3`（**不得**因 key range 提前返回）；**全部未命中时**返回 `kNotFound` 且 `hit_layer == none` | `MemEnv` + `ReadStats` |
| M3-A31 | `Iter.MergingIteratorOrder` | 无（用内存 child） | 多路归并按内部 key 全序（同 key 按 sequence **降序**）；空 child 集合 / 单 child / 一个 child 整体在另一个之前 三种形态；`status()` 取第一个非 OK | 无 |
| M3-A32 | `Iter.DBIterVisibilityAndTombstoneSkip`（I28/I29） | 无 | 快照 `s` 下只输出 `seq <= s` 的版本；tombstone 被跳过；tombstone 之后 `Seek` 落在**下一条可见 key**；`Prev`/`SeekToLast` 与正向一致 | 无 |
| M3-A33 | `Iter.DBIterStableAcrossFlush`（L15/L21） | `MemEnv` + `FlushHook`（在迭代中途放行 flush + 注册） | 迭代结果与"不 flush"完全一致；ASan/LSan **零**报告（无 UAF、无泄漏） | `FlushHook` + ASan |
| M3-A34 | `Iter.UserIteratorUnchangedForMemoryMode` | 无 | M1 的 `NewIterator()` 语义（含 `Prev`/`SeekToLast`/tombstone）在 `DBIter` + 单 child 归并下**逐条不变** | 无（M1 既有用例继续跑） |
| M3-A35 | `Recover.SSTableOnly` **[落盘重启]** | `MemEnv`，小 buffer 强制 flush + 轮转 | 关库时**当前 log 为空**、老 log 已被回收；重开：`records_replayed == 0` ∧ `sst_files_registered >= 1` ∧ 全部 key 逐字节可读 | `MemEnv`、`recycle_log_files=true` |
| M3-A36 | `Recover.SSTablePlusWalTail` **[落盘重启]** | `MemEnv`，flush 后再写一批（不 flush）| WAL 中比 SSTable 更新的部分被重放；新值覆盖 SSTable 里的旧值；`records_replayed > 0` | `MemEnv` |
| M3-A37 | `Recover.LastSequenceUniqueSource`（I31） | `MemEnv` | 恢复后 `last_sequence_ == max(WAL 重放最大 seq, Σ 文件 max_sequence)`；下一次写入分配的 seq **严格大于**它（I13 的 M2 措辞不变） | `MemEnv`、`GetRecoveryStats().max_sequence_in_files` |
| M3-A38 | `Recover.MetaMaxSequenceVerifiedAgainstFullScan`（I31） | `MemEnv`，先构造再篡改 `META` | 正常：`META.max_sequence` == 全量扫描得到的值；篡改后：`Open` 返回 `kCorruption` | `MemEnv::SetContents` |
| M3-A39 | `Recover.MetaCorruptRefused`（I27/§1.2 边界 4） | `MemEnv` | `META` 的 magic / version / tail CRC / 字段长度 任一损坏 ⇒ `kCorruption`，**不自动修复、不重建** | `MemEnv` |
| M3-A40 | `Recover.OrphanHandledPerInvariant27`（I27） | `MemEnv` | ① 只有 `%06u.sst.tmp` ⇒ 不注册 + 被删 + `orphan_tmp_removed == 1`；② 未注册 `.sst` ⇒ 不读入 + 被删 + 计数；③ 清理**失败**（注入）⇒ `Open` 仍成功、`orphan_remove_failed == 1`、且该文件**仍不被读入** | `MemEnv` + 注入 |
| M3-A41 | `Recover.MetaMissingWithSstRefused`（§10.9 安全阀） | `MemEnv` | 删除 `META` 但保留 `.sst` ⇒ `kCorruption`（信息含"目录非空"）；删除 `META` 且只有 `.log` ⇒ **正常打开并读到全部数据**（兼容 M2 老库） | `MemEnv` |
| M3-A42 | `Recover.ComparatorNameMismatchRefused`（兑现 M2 D13 残留） | `MemEnv` | `META` 里的 `comparator_name` 与 `options.comparator->Name()` 不符 ⇒ `kInvalidArgument`（带两个名字） | `MemEnv` + 自定义 `Comparator` |
| M3-A43 | `Recover.LogOrderIsNumericAscending` | `MemEnv`，文件名 `000001/000002/000010` | 按**数值**升序重放（字符串序会是 1→10→2）；同 key 的最终值来自编号**最大**的 log | `MemEnv` |
| M3-A44 | `Recover.TruncationRulesUnchangedFromM2` | `MemEnv` | 只有最高编号 log 允许尾部残骸截断；非最高编号的残骸 ⇒ `kCorruption`；中间损坏 ⇒ `kCorruption`（**逐条沿用 M2 §5.3**） | `MemEnv` |
| M3-A45 | `WalReclaim.WatermarkCriterion`（I34） | `MemEnv`，多表多 log | 构造 `pending = {immutable@1, memtable@2}, log_number_=3` ⇒ `min_log_to_keep == 1`（**不是** 3）；注册 immutable 后 ⇒ `min_log_to_keep == 2`；再注册 ⇒ `3`；每一步断言可删集合 | `MemEnv` + `GetFlushStats()` |
| M3-A46 | `WalReclaim.RecoveredMemtableKeepsOldestLog`（I34 的必要性形态） | `MemEnv` | 恢复（重放了 log 1 与 log 2）后**立刻** flush+注册 ⇒ 断言 log 1 与 log 2 **未被删**（因为恢复出的 memtable 的 `log_number == 1`）；若实现漏掉 §8.3 步骤 ⑩，本用例必须**红** | `MemEnv` |
| M3-A47 | `WalReclaim.DeleteOnlyAfterMetaDurable`（I34） | 事件日志 Env | 删除 `*.log` 的事件**严格晚于** `META` 的 `rename` 与 `SyncDir` 事件 | 事件日志 Env |
| M3-A48 | `WalReclaim.DisabledByOption`（§1.2 边界 5 的对照） | `MemEnv`，`recycle_log_files = false` | 一个 log 都不删；数据仍全部可读（证明开关双向有效） | `Options` |
| M3-A49 | `Rotation.BeforeSequenceAssignAndAtomicOnFailure`（I33） | 注入 Env（轮转的 `SyncDir`/建文件失败） | 轮转成功：新 log 编号 == 旧 + 1、`log_sealed_` 清、后续写入落在新 log；轮转失败：整批同一非 `kOk`、`last_sequence_` **未推进**、`log_` **未换**、`memtable_` 内容不变 | 注入 Env + `FlushHook` |
| M3-A50 | `Sync.PerLogBoundaryAfterRotation`（I32 已在 M2 修完，本用例收窄为**轮转后的 per-log 边界**） | `CommitHook::OnGroupTaken` 屏障 + 注入慢 `Append` + 轮转 | 轮转后再调用 `Sync()`：断言 `durable_seq_ <= log_last_appended_seq_`（不得发布到当前 log 未追加的部分），且轮转点之前已发布的水位不变；`Sync()` 返回后再次 `Put(sync=true)` 水位单调 | `CommitHook` + 可暂停的 Env |
| M3-A51 | `Close.AbandonsImmutablesWithCounter`（§6.5） | `MemEnv` + 慢 flush | `Close()` 时有 1 个未落盘 immutable ⇒ 数据仍在 WAL（重开可读）；`immutables_abandoned == 1`；无 UAF/泄漏 | `MemEnv` + ASan |
| M3-A52 | `Close.DrainsBackgroundThread`（L21） | `MemEnv` | `Close()` 返回后后台线程已 join；`Close()` 幂等；`Close()` 期间并发写返回明确 `Status`（沿用 M2 的 A31） | `MemEnv` + ASan/TSan |
| M3-A53 | `Options.InvalidRejectedWithoutFailStop`（M2 教训 4） | 无 | `block_size=0/256/2MiB`、`max_open_files=0`、`write_buffer_size=0`、`comparator=nullptr` ⇒ `kInvalidArgument`；**且**在同一个 DB 上先触发一次 `kInvalidArgument` 的 `Put`（超大 entry），随后普通 `Put` **必须**返回 `kOk`（防粘性只读复活） | 无 |
| M3-A54 | `Flush.StatsCountedNotSilent`（"不得静默"纪律） | `MemEnv` + 注入 | 构造"截断尾部 + 跳过 record + 删 orphan + 删 obsolete log + 索引超阈值"五种情形，断言 §8.4 的**每一个**计数都非零、且 `Close` 后重开仍可读 | `MemEnv` + 注入 |

**A 组的零 flaky 纪律**（沿用 `m2-prerequisites.md` §7）：凡涉及随机的用例必须固定种子并打进 `INFO`/
`RecordProperty`；时间一律用 `MemEnv` 的假时钟（`NowMicros` 递增、`SleepForMicros` 不真睡）；
**不得**用 `sleep` 赌调度（M2 的 A20 教训），跨线程时序一律靠 `CommitHook`/`FlushHook` 的确定性屏障。

### 10.2 B 组（真实磁盘 / 进程级，脚本驱动）

| 编号 | 名称（脚本/命令） | 依赖假设 | 通过判据 | 需要的 seam |
|---|---|---|---|---|
| **M3-B01** | `scripts/lsm_flush_crash_test.sh`（kill -9 循环，`--write-buffer-size 256KiB`）**[与 M2 门禁共存]** | 真实目录、真实进程；`kill -9` **不丢 page cache**（M2 §11.3）⇒ 它只证明**进程级**一致性 | `ROUNDS_OK == ROUNDS` ∧ `MISSING_TOTAL == 0` ∧ `MISMATCH_TOTAL == 0` ∧ **`SST_FILES_TOTAL > 0`** ∧ **`LOGS_DELETED_TOTAL > 0`** ∧ 末尾 `[FLUSH_CRASH_OK]`；退出码 0 | `FlushHook`/`CommitHook` 环境变量注入点 + 固定行格式 + 正向标记 |
| **M3-B02** | 三注入点进程级 kill -9（写文件后 / rename 前 / 注册前） | 三个注入点必须能被**真实进程**打到（`FlushHook` 在注入点里 `raise(SIGKILL)`） | ① 恢复后**没有**"半边文件被注册"；② `.sst.tmp` 被清理且计数；③ 未注册 `.sst` 被清理且计数；④ `missing 0`；⑤ `META` 与目录内容自洽（引用集 ⊆ 存在集） | `FlushHook` + 环境变量选择注入点 |
| **M3-B03** | 真实目录 flush + `Close` + 重启 **[落盘重启]** | 小 `write_buffer_size` 使 ≥1 次 flush + 轮转发生 | 重开后：`records_replayed == 0`、`log_files` 只剩空的当前 log、**全部 key 逐字节可读**；再进一步：**物理删除**当前 log 后仍全部可读（"仅靠 SSTable"的强证据） | 小 `write_buffer_size` + `lsm_flush_crash_test` 的 inspect 模式 |
| **M3-B04** | 损坏 SSTable 启动行为（单字节翻转扫描）**[CRC 检出]** | 真实文件；沿用 M2 B03 的逐字节方法论 | 对 data/index/footer 每个字节翻转：`Open` 或**首次读到该块**时必须返回 `kCorruption`（带文件+偏移）；**零**"静默返回错值"；`verify_checksums=false` 的对照必须**不再**报错（证明开关有效） | `lsm_damage_test` 扩展（新增 SSTable 模式） |
| **M3-B05** | 文件句柄计数与泄漏检查（I30） | 真实进程；`/proc/self/fd` 可读 | 多轮（≥50）flush + 1000 次 `Get` 后 `fd` 计数**不增长**（相对基线的差值 <= table cache 容量 + 常数）；`TableCache` 的命中/淘汰计数自洽 | fd 计数探针（`MemEnv` 侧同步提供注入计数器） |
| **M3-B06** | M2 门禁共存（`scripts/lsm_gate.sh`）**[与 M2 门禁共存]** | 沿用 M2 的全部门禁腿 | **M2 的 4 条腿全部仍 PASS**（干净重建 + 0 warning + 82 例不得减少；ASan；100 轮 kill -9 `missing 0`；B03 截断扫描 1401/1401；B04 中间损坏）；**并新增** M3 的腿 | `lsm_gate.sh` 追加 M3 腿 + 正向标记断言 |
| **M3-B07** | ASan / TSan 干净 | 三构建目录互不共享；TSan 需 `setarch $(uname -m) -R` | ASan（含 LSan）**零**报告、退出码 0；TSan **零** race 报告、退出码 0；**含** M3 的 iter/flush 并发用例（`M3-A33`/`A52`） | `build-asan` / `build-tsan` |
| **M3-B08** | 读放大基线（D8 的口径实测） | 100 万 key、小 `write_buffer_size` ⇒ F ≈ 59 个 SSTable | 打印固定格式行（`FILES_CHECKED / KEY_RANGE_SKIPPED / INDEX_BLOCKS / DATA_BLOCKS / BYTES_READ / HIT_LAYER`）的 p50/p90；与 D8 的**推算值**（≈59 文件 / ≈1.33 MB）同量级（倍数关系作为 M4 的分母） | `GetReadStats()` + 固定输出格式 |
| **M3-B09** | WAL 回收的磁盘效果（I34） | 真实目录，100 万写 + 小 buffer | `du -sb` 与 `ls *.log` 的数量：log 文件数**有界**（<= `kMaxImmutableMemTables + 2`）；`LOGS_DELETED_TOTAL > 0`；总磁盘占用**不随 flush 次数线性增长**；全部数据可读 | `GetFlushStats()` + `du` |
| **M3-B10** | ENOSPC / 只读目录 | 需要 root 挂 loop ⇒ **可能无法制造** | 明确 `Status`、不破坏既有数据、不 panic；**若无法制造则如实登记"未验证"**（沿用 M2 B06 的处置） | 或由 A 组的注入式覆盖承担 |

**B 组的诚实性纪律**（沿用 `m2-prerequisites.md` §7.2 与 §11.3 的结论）：
**不得**由"`sync=false` 的 100 轮 `missing 0`"宣称 `sync=false` 有持久性（`kill -9` 不丢 page cache）；
**不得**由"`kill -9` 没产生 torn 文件"宣称"掉电安全"——掉电语义只能靠 `MemEnv` 回滚注入（A 组）承担。
`kill -9` 系列证明的是**进程级一致性**，`MemEnv` 系列证明的是**掉电语义**，二者的结论必须分开写。

---

## 11. 子里程碑拆分（M3.1 → M3.2 → M3.3）

> 每步**一个提交**，提交信息引用本文档的章节号；只实现让**当前阶段测试集**通过的最小代码，
> **不提前实现**下一步（尤其不提前实现 M4 的任何东西）。
> 每步结束必须给出**实测证据**（构建命令 + 原始输出摘录，`roadmap.md` §3.4"未跑不算过"）。

### M3.1 —— `format` / `block` / `table_builder` / `table`（纯格式层，零 DB 改动）

| 项 | 内容 |
|---|---|
| 新增 | `src/sstable/{format.h,block_builder.{h,cpp},block.{h,cpp},footer.{h,cpp},table_builder.{h,cpp},table.{h,cpp}}`；`tests/block_test.cpp`、`tests/sstable_test.cpp`；`docs/protocol.md` **追加 §10**（§4 的 patch 文本照抄） |
| 必改 | `src/util/env.h`+`env_posix.cpp`（`RandomAccessFile` + `NewRandomAccessFile` + `SyncDir`）；`tests/memenv.{h,cpp}`、`tests/faulty_env.{h,cpp}`（补新纯虚方法）；`CMakeLists.txt`（**新增独立目标 `lsm_sstable`**，只链 `util`） |
| **不改** | `db_impl.*`、`memtable.*`、`wal.*`、`skiplist.h`、`common.h`、M1/M2 既有测试 |
| 判据 | `block_test` + `sstable_test` 的 A 组全绿（`M3-A01~A19`）；干净重建 **0 warning**；**`lsm_sstable` 目标能独立构建** |
| 证据命令（原始输出入档） | `bash scripts/lsm_build.sh`（0 warning + 全部用例含 M1/M2 82 例不回归）<br>`./build/bin/lsm_tests --gtest_filter='Block.*:Footer.*:Table.*'`<br>`cmake --build build --target lsm_sstable`（越权依赖会在此链接失败）<br>`nm -C build/liblsm_sstable.a \| grep -c 'version\|db_impl\|wal'` ⇒ **期望 0**（机制化证明依赖纪律） |
| 风险 | restart 语义 / `handle.size` 与 `length` 的双向校验 / `kNotSupported` vs `kCorruption` 的区分（`M3-A08`） |

### M3.2 —— flush 路径 + 读路径串联 + `MergingIterator`/`DBIter` + 单后台线程（**过渡形态**，注册只在内存）

| 项 | 内容 |
|---|---|
| 新增 | `src/version_edit.{h,cpp}`、`src/version_set.{h,cpp}`（M3 简化版：`Version` + `TableCache`；**本步的注册只在内存**）、`src/merging_iterator.{h,cpp}`、`src/db_iter.{h,cpp}`；`tests/flush_test.cpp`、`tests/iterator_test.cpp` |
| 必改 | `src/db_impl.{h,cpp}`：`memtable_` 改 `shared_ptr`、`immutables_`、flush 状态机（§6.2/§6.3 但**去掉 ⑥⑦ 的 `META` 部分**）、单后台线程（§6.5）、读路径串联（§7.1）、`GetFlushStats`/`GetReadStats`；`CMakeLists.txt` |
| **本步的显式过渡妥协（照抄 M2.2 的先例）** | ① `META` **不写不读**；② `Open` 对"目录里存在 `*.sst`"一律 `kCorruption` 并给出"本子里程碑尚未实现 SSTable 恢复"的信息（**5 行 guard，M3.3 删除**）——**不许**静默忽略；③ **不**做 WAL 轮转与回收（`log_number_` 不变），`recycle_log_files` 在本步无效（文档必须写明） |
| 判据 | `flush_test` + `iterator_test` 全绿（`M3-A20~A34`）；ASan 干净；**M2 的 100 轮 kill -9 门禁仍 `missing 0`**（大 buffer ⇒ 不触发 flush，等价于 M2 行为） |
| 证据命令 | `bash scripts/lsm_build.sh`；`./build/bin/lsm_tests --gtest_filter='Flush.*:Read.*:Iter.*'`；<br>`cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests`；<br>`bash scripts/lsm_crash_test.sh --rounds 100 --mode sync`（M2 腿不回归）；<br>**L18 探针**：`./build/bin/lsm_tests --gtest_filter='Flush.NoIoWhileHoldingDbMutex'` |
| 风险 | 写者停等的丢唤醒（`M3-A25`）/ 迭代器与 flush 的并发（`M3-A33`）/ `Get` 的三行取引用是否真的把 IO 留在锁外（`M3-A23`） |

### M3.3 —— 版本元数据持久化 + 启动恢复 + WAL 回收 + 崩溃脚本 + 全部门禁

| 项 | 内容 |
|---|---|
| 新增 | `tests/recovery_m3_test.cpp`、`tests/crash_flush_test.cpp`、`scripts/lsm_flush_crash_test.sh`、`docs/m3-prerequisites.md`(由 `#1` 产出)、`docs/m3-evidence.md` |
| 必改 | `src/version_set.{h,cpp}`（`META` 的 `Recover`/`Persist`，§8.1）；`src/db_impl.{h,cpp}`（§8.3 的完整恢复流程、**删除 M3.2 的 5 行 guard**、WAL 轮转 §6.6.1、`min_log_to_keep` §6.6.2、孤儿清理、`RecoveryStats` 新字段）；`scripts/lsm_gate.sh`（追加 M3 腿 + **正向标记断言**）；`.gitignore` |
| 判据 | `recovery_m3_test` 全绿（`M3-A35~A54`）；`lsm_flush_crash_test.sh` 的 `missing 0` + `SST_FILES_TOTAL > 0` + `LOGS_DELETED_TOTAL > 0`；ASan/TSan 干净；M2 的 4 条腿仍 PASS；tag `m3-sstable` |
| 证据命令 | `bash scripts/lsm_gate.sh --rounds 100 --with-tsan`（一条命令跑完全部）；<br>`bash scripts/lsm_flush_crash_test.sh --rounds 100 --write-buffer-size 262144`；<br>`bash scripts/lsm_crash_test.sh --rounds 100 --mode sync`（M2 腿共存）；<br>B04 的逐字节翻转扫描输出；B08 的读放大固定格式行；B09 的 `du`/`ls` 输出 |
| 风险 | `META` 的原子性与"缺失"语义（`M3-A39/A41`）/ I34 判据（`M3-A45/A46`）/ 轮转后的 per-log 边界（`M3-A50`；M2 的 I32 缺口已在 `a3c85a8` 闭合）/ 与 M2 门禁共存（`M3-B06`） |

**"每步一个判据"的关键**（沿用 M2.2→M2.3 的先例）：M3.2 的"注册只在内存"与 M3.3 的"注册进 `META`"
**不改变** `Version`/`FlushImmutable` 的签名与不变量，只把"持久化"这一步从"无"变成"§8.1 的 5 行"。
⇒ 每一步的回归集是**上一个集合的超集**，不会出现"M3.3 改坏了 M3.2 的行为却没人发现"。

---

## 12. 自检（占位符 / 内部矛盾 / 歧义 / 范围越界）

### 12.1 占位符

- 全文**无** `TODO`/`TBD`/`XXX`/`FIXME`。出现的 `...` 全部在**引用的原始输出**或**伪代码省略号**里（属原文照录/结构性省略），
  逐处可核：§0.2 的门禁输出、§0.4 的探针输出、§5.2/§5.4 的算法片段。
- 每个新增常量都有明确取值（§10.2 的 16 个常量表）；每个新增类型都有完整签名或明确字段表
  （`Handle`/`FileMetaData`/`VersionEdit`/`Version`/`BlockBuilder`/`Block`/`Table`/`TableBuilder`/
  `MergingIterator`/`DBIter`/`ReadStats`/`FlushStats`/`RecoveryStats` 的增量字段）。
- **未验证项一律显式标注**：§0.6（`fsync` 未重测、`loadavg` 干扰）、D8 的"推算值非实测"、
  §10.2 的 `M3-B10`（ENOSPC 可能无法制造）、§13 的拍板清单。

### 12.2 内部矛盾（逐条核对）

| 潜在矛盾 | 处置 |
|---|---|
| §1.2 非目标"M3 不得出现 MANIFEST"，但 §1.3.2 清单里有 `version_set.{h,cpp}` 与"元数据持久化" | **已在 D4 显式裁决**：选 D（单快照 `META`，**无** MANIFEST/VersionEdit 日志/VersionSet 版本图/CURRENT/层级）。"`version_set` 这个**文件名**"与"M3 不得出现 MANIFEST"不矛盾——M3 的 `VersionSet` 里没有 `Version`*图*、没有编辑日志、没有层级；它的 `Recover`/`Persist` 是单文件快照。`#4` 评审按"文件里有没有 `MANIFEST`/`VersionEdit` 追加/`CURRENT`/`level` 字样"核对 |
| §1.2 说"M3 不删已注册的 SSTable"，但 §6.3 步骤 ⑥ 要删"未注册 `.sst` 孤儿" | **不矛盾**：删的是"**未被版本引用**的文件"（I27/约束 6 明确要求把孤儿当垃圾清理），不是已注册的表。措辞已在 §1.2 与 §6.3 双向写清（"只删 `.sst.tmp` 与未注册的 `.sst` 孤儿"） |
| D3 说"CRC 覆盖含长度"，而 §3.6 又说"`handle.size` 与块内 `length` 冗余" | **不矛盾且必要**：`handle.size` 是"读多少"的**唯一**真相源（一份判据）；`length` 只用于"等不等于"的**自检**（冗余校验）。§3.6 的表格把两者写死，并说明"这不是两份判据"（直接回应 M2 阻断项 1 的教训） |
| §6.2 说"`WouldReject` 是唯一容量判据"，但 §6.2 又给了 `NewTableCapacity(footprint)` 公式 | **不矛盾**：DB 层**不**判"装不装得下"，它只**决定新表的 `write_buffer_size`**；判据仍在 `MemTable::WouldReject` 一处。§6.2 的"单一真相源口径声明"块把这个界限写死，并给出"构造上不可拒绝"的证明 |
| §1.2 边界 1 说"M2 的 `kFrozen` 外部行为变化"，§1.3.1 又说"`MemTable` 的 `kFrozen` 一个字节都不改" | **不矛盾**：前者是 **DB 层**（`Put` 不再返回 `kFrozen`），后者是 **`MemTable` 类**（`Add` 仍返回 `kFrozen`）。两句话的主语不同，已在两处都写明主语 |
| §8.2 说"不做任何基于水位的跳过"，§10.2 的 `M3-B01` 又说 `MISSING 0` | **不矛盾**：不跳过 ⇒ 只会**多**重放（冗余但安全）⇒ 已 ack 集合一定被覆盖 ⇒ `missing 0` 更容易成立。跳过是风险，不是收益 |
| §8.3 步骤 ⑦ 说"重放集合 = 目录实际内容"，§10.9 规则 3 同 | **一致**（同一句话的两次表述）；`min_log_number_to_keep` **只**驱动删除，**不**驱动重放集合 |
| D7 说"线性扫描 N 个 child"，§10.1 的 `M3-A31` 又要测"空 child 集合" | **不矛盾**：`n == 0` 时线性扫描的循环体不执行，当前 child 置空 ⇒ 迭代器恒 Invalid；用例专测这个退化输入（不是"常见形态"） |
| §6.5 说"`Close()` 不强制 flush immutables"，§10.1 的 `M3-A35` 又说"关库时当前 log 为空、老 log 已回收" | **不矛盾**：`M3-A35` 的场景里 flush **已经完成**（写完就触发并等到了注册），所以 log 已回收；`Close()` 不 flush 指的是"**放弃**尚未完成的 flush"，两者是不同状态。`M3-A51` 专测后者 |
| L17 原文要求"CURRENT 的原子性"，本设计说"不引入 CURRENT" | **已显式映射**（§9.2 的 L17 行）：把该纪律的**实质**（原子切换 + 禁止原地覆写）落到 `META` 的 `写 .tmp → fsync → rename → SyncDir`，并**加强**一条"rename 之后必须 `SyncDir`"。`#1` 按此登记，不能只写"不适用" |

### 12.3 歧义（逐条消解，给出唯一解释）

| 歧义点 | 本设计的唯一解释 |
|---|---|
| "块大小 4 KiB" | 精确化为"**数据块 payload 的目标大小** `kDefaultBlockSize = 4096`；判据是 `BlockBuilder::EstimatedSizeAfter`；`block_size` **不是**硬上限（单条 entry 可超出）"（§3.2/§5.4） |
| "restart 间隔 16" | 精确化为"每 16 条 entry 一个 restart 点；**组间不共享前缀**（新组首条 `shared == 0`）；`last_key` 在 restart 点重置"（§3.2） |
| "索引项 = 分隔 key + 块偏移/长度" | 精确化为"索引 key = 该数据块**最后一条** entry 的**完整 internal key**（**不做**分隔 key 缩短）；`Seek` 取**第一个 `>= target`** 的索引项"（§3.3） |
| "CRC 覆盖面含块头" | 精确化为"`crc = crc32c(length(4B LE) ‖ type(1B) ‖ payload)`，即 **header ‖ payload 的全部字节**"（§3.6） |
| "footer 定长" | 精确化为"**44 字节**，字段与偏移见 §3.7 表；`footer_crc` 覆盖前 40 字节" |
| "未注册文件" | 精确化为"编号 ∉ `META.files` 的 `%06u.sst`，或任何 `%06u.sst.tmp`"（§3.1/§8.3 ⑥） |
| "新→旧" | 精确化为"immutable：`deque` 的 back→front；SSTable：**文件号降序**"（§7.1 规则 2）。**不用** key range 或 `max_sequence` 判新旧 |
| "命中第一个可见版本即返回" | 精确化为"按层递进；**同层内**逐个检查到命中为止；tombstone 命中即终局（返回 `NotFound`，不再向下）"（§7.1/§7.2） |
| "可回收水位" | 精确化为 `min_log_to_keep = min({pending 表的 log_number})`，可删 = **严格小于**它（§6.6.2） |
| "冻结" | 精确化为"在阶段 A（持 `commit_mu_ → mutex_`）把 `memtable_` 的 `shared_ptr` 移入 `immutables_`、替换为新 `MemTable`、置 `log_sealed_`/`need_rotate_`；**纯内存**"（§6.2） |
| "注册" | 精确化为"构造新 `Version`（含新文件元数据）→ 原子替换 `version_` → 写 `META`（`.tmp → fsync → rename → SyncDir`）成功"（§6.3 步骤 ⑦） |
| "落盘后重启仅靠 SSTable 读到全部数据" | 精确化为"`GetRecoveryStats().records_replayed == 0` ∧ `sst_files_registered >= 1` ∧ 全部 key 逐字节可读（`M3-A35`/`M3-B03`）" |
| "内层/外层"（若有人用层次词） | 本设计**不使用**"层/L0/Ln"作为**实现**概念（M4 才引入）。读路径的"层"仅指"数据来源类别"（MemTable / immutable / SSTable），全部 SSTable 同权、只按文件号排序（§7.1） |

### 12.4 范围越界（是否偷偷带了 M4+ 内容）

逐条对照 §1.2 的禁列，本设计**出现**下列"接近但不同"的项，全部有明确边界：

| 出现的东西 | 为什么不是越界 |
|---|---|
| `src/version_set.{h,cpp}` / `Version` / `VersionEdit` 这些**文件名与类型名** | 指令 §1 的「必须新增」清单**点名**要求它们（"M3 可先做简化版：只维护 L0 文件列表 + 元数据持久化"）。D4 已把"M3 的 `version_set` 里**没有** MANIFEST / 追加日志 / 版本图 / CURRENT / 层级"写成硬约束，并给出与 M4 的迁移路径 |
| `META` / `META.tmp` | 是"简化 meta 文件"（指令 D4 的第三个方案），**不是** MANIFEST/CURRENT。它的语义是"全量快照"，没有任何"编辑日志"性质 |
| "L0" 这个词 | 本设计**只在指代"M3 的唯一一层、文件号降序"时借用该词**，且 §12.3 已声明"不使用层/L0/Ln 作为实现概念"。代码里**不得**出现 `level`/`LevelFiles`/`L0_` 等符号（`#1` 登记为评审检查项） |
| `TableCache`（文件句柄 LRU） | 指令 D8 明确问"M3 是否引入 table cache"⇒ 属于决策范围。**块缓存**（`BlockCache`/块级 LRU）**不在**设计里（D8 明写"无块缓存"） |
| `ReadStats`（读放大统计） | 指令 D8 明确要求"读放大的初步统计口径"；不含三个放大率的**聚合与报告**（那属 M4）。M3 只给**原始计数** |
| WAL 回收（删除 `.log`） | 指令 §0 的 G7/非目标里**没有**禁止；M2 设计 §3.3 明确写"M3+ 行为：…可删"⇒ 属于 M3 的职责。已在 §1.2 边界 5 登记为"唯一带数据丢失能力的新路径"并附专属用例 |
| `flush_hook` / `FlushHook` | 与 M2 的 `CommitHook` 同纪律的生产 `nullptr` 观察点，**不含**任何生产逻辑；它是 B 组三个注入点的**前提**（指令 §2 要求） |
| `Options::block_size` / `verify_checksums` / `max_open_files` / `recycle_log_files` | 分别对应 D1/D3/D8/D6 的决策落点，每个都有默认值与判据。**没有** `Options::compression`、`Options::bloom_bits`、`Options::max_bytes_for_level_*` 等 M4/M5 字段 |
| `FlushStats` / `RecoveryStats` 的增量字段 | "不得静默"纪律的落点（M2 已为此立过 `GetRecoveryStats` 的先例）。全部是**计数/可读字符串**，不含 M4 的放大率统计 |
| `RandomAccessFile` / `SyncDir` 两个 `Env` 增补 | M1 设计明写"`RandomAccessFile` 留到 M3"；M2 设计 §5.7 已声明 `SyncDir`（实现没落地）。两者都是 M3 的**必要**能力 |

**明确声明：本设计不含** 分层 compaction / 层级结构 / MANIFEST / `VersionEdit` 追加日志 / `VersionSet` 版本图 /
CURRENT / Bloom filter 内容 / `WriteBatch` 公共 API / 块缓存 / 压缩算法 / 并发 compaction / `Snapshot*` 公共 API /
`DB::Flush()` / 真删除已注册 SSTable / raft-kv 对接。

### 12.5 本设计的已知薄弱点（主动暴露，供 `#4` 评审攻击）

1. **`kill -9` 不能验证 flush 的"掉电安全"**（沿用 M2 §11.3）：`kill -9` 不丢 page cache ⇒
   "B01/B02 的 `missing 0`"只证明**进程级**一致性。真正的掉电语义（尤其"`rename` 后、`SyncDir` 前掉电"）
   **只能**靠 `MemEnv` 的回滚注入近似（`MemEnv` 的 `synced_size` 水位模型对"目录项"是**不建模**的 ⇒
   本设计的 `SyncDir` 纪律在 A 组里只能验证"调用顺序"，**不能**验证"掉了会怎样"）。这条**必须**写进
   `#1` 的风险清单（诚实登记，不假装 A 组能覆盖它）。
2. **`META` 单快照在文件数很大时会变胖**：F 个文件 ⇒ `≈ 12 + F×(28 + 2×key_len)` 字节。
   F = 10 000（若用户把 `write_buffer_size` 设得很小）⇒ ≈ 700 KB，**每次 flush 重写一遍**。
   M3 有意接受（M4 换 MANIFEST 正好解决）；但 `#1` 应登记**阈值 WARN**：F > 1000 时计数上报
   （"该上 M4 的 MANIFEST 了"），避免用户在 M3 上把它推到不可用。
3. **L0 无 compaction ⇒ 读放大线性增长**：F ≈ 59（100 万 key / 4 MiB）时一次最老 key 的 `Get` ≈ 59 次
   索引读 + 59 次数据读 ≈ 1.33 MB（D8 推算）。这是**设计选择**（M4 解决），但要写清"M3 的读性能
   随写入量退化"，不能拿 M3 的数字去回答 M5 的"读为什么慢"。
4. **写者停等的上界是估算**（D5：≈21 ms），未实测；`M3-A25` 用假时钟/屏障验证**逻辑**，
   真实上界由 `M3-B09` 的 `stall_micros` 观测。
5. **`MemEnv` 不建模目录项**（见第 1 条）⇒ `SyncDir` 与"`META` 与 `.sst` 的目录项顺序"在 A 组是
   "顺序断言"而非"崩溃语义断言"。这是本设计**最明显的一处证据强度缺口**。
6. **`M3-B10`（ENOSPC）可能无法制造**（需 root 挂 loop）；若不能，只能靠 A 组注入（沿用 M2 B06 的处置）。
7. **`Table` 的 `max_sequence` 统计与全量扫描的一致性是"自证"风险**：`M3-A38` 用"打开文件全量扫一遍"
   作为独立参照，但**两者共用同一个块解码器** ⇒ 若解码器在 `SequenceNumber` 的提取上系统性写错，
   二者会同错。⇒ 必须补一条"**手工拼字节参照**"（沿用 M1 的 `ManualInternalKey` 纪律）：
   `M3-A38` 的子用例用手工构造的字节算期望 `max_sequence`。

### 12.6 需用户拍板清单（汇总）

见 §13。

---

## 13. 需用户拍板清单

> "推荐"列是本文档的立场；"若否决"列说明改了之后**哪些章节要重写**（便于评估代价）。

| # | 问题 | 本文推荐 | 若否决的代价 | 必须拍板 |
|---|---|---|---|---|
| **Q1** | **D4 版本元数据**：选 D（单快照 `META` + 原子 rename，**无** MANIFEST/CURRENT/层级）还是 A（LevelDB 式 MANIFEST + VersionEdit 追加 + CURRENT）？**这是本设计最大的一条**，因为指令的「非目标」与「必须新增清单」自相矛盾（§D4 已把矛盾原文列出） | **D** | 改 A ⇒ §3.1（命名）、§8.1（全部重写）、§8.3（⑤⑥ 重写）、§9.2 的 L17、§10 的 `M3-A37~A42` 全部重写；且**与 roadmap §2"M3 不得出现 MANIFEST"直接冲突**，需要用户同时裁决那条红线 | **★ 是** |
| **Q2** | **D1 块格式**：A（4 KiB + 前缀压缩 restart 16 + 单层索引）/ B（不压缩）/ C（整文件单块）/ A'（两级索引）？ | **A** | 改 B ⇒ §3.2/§5.2/§5.3 重写（去 restart），且 M4 要加回来；改 A' ⇒ §3.3 加重级索引（M3 无收益，见 §0.4(d)） | 是 |
| **Q3** | **D2 是否为 M5 预留 metaindex**（M3 写空块） | **预留** | 不预留 ⇒ M5 必须改 footer 布局 + 升 `kTableFormatVersion` ⇒ M5 要写格式迁移；违背指令 D2 的推荐 | 否（指令已推荐） |
| **Q4** | **D3 校验**：每块 CRC（含长度字段）+ footer CRC + 读时默认开可关？ | **是** | 不加 footer CRC ⇒ `index_handle` 损坏变成低概率静默错值（§3.7 的理由）；CRC 不含长度 ⇒ 回到 M2 §9.3 明确否决的"间接推断" | 否（指令已推荐"含块头"） |
| **Q5** | **D6 WAL 回收**：M3 就**真删**已完全落盘的 `.log`（默认开、可关）还是只记水位不删？ | **真删**（删在有专属用例 + 可关开关） | 只记水位 ⇒ G7 落空、"纯 SSTable 启动"只能靠测试手工删文件（弱化证据）、100 万写场景磁盘单调增长（§D6） | **★ 是**（唯一带数据丢失能力的新路径） |
| **Q6** | **§8.7 E4 是否新增公共 `DB::Flush()`** | **不新增**（用小子缓冲驱动 flush，`M3-B03` 覆盖） | 新增 ⇒ 要改 `src/db.h`（**不在** M3 的「必须改」清单里）⇒ 属于超出授权的契约变更；需要用户明确批准 | **★ 是** |
| **Q7** | ~~**§0.1 的脏工作区**如何处置？~~ | **已关闭：按 ① 执行** —— 该 WIP 已在本文档提交前被回退，`~/lsm-kv` 现为 HEAD `3603696` 的干净树（复核原始输出见 §0.1 的"提交前复核"块）。⇒ M3 的基线就是 `~/lsm-kv` 自身，不需要额外的克隆 | 无（已闭环） | 否（**已闭环**） |
| **Q8** | **§0.5 的 M2 `Sync()` 水位越界**：① 单开一个 M2 补丁（推荐，因为它是 M2 的缺陷）；② 在 M3 里顺手修（代码本来要改）；③ 登记为已知限制不修 | **① 单开 M2 补丁（用户裁决，已完成：`a3c85a8`）**；原②方案作废，M3 只保留 per-log 边界用于轮转 | 若选 ③ ⇒ `db.h` 对 `Sync()` 的契约长期不成立，且 **M3 的 WAL 回收判据若误用它会更危险**（§0.5 已注明） | **★ 是** |
| **Q9** | **M2 基线 rev**：以 `HEAD 3603696`（含 A22/A25 修复 + `GetRecoveryStats`）为准，还是以 tag `m2-wal`（`8189607`，落后两个提交）为准？ | **tag `m2-wal` → `a3c85a8`**（= 原 HEAD `3603696` + I32/I35 补丁；用户已批准前移该 tag，§15 R5） | 以 tag 为准 ⇒ 基线少 2 个提交的修复，且 `GetRecoveryStats` 不可用（§8.4 的可观测性口径要重写） | 是 |
| **Q10** | **§1.2 边界 1 的登记口径**：M3 起 `PersistentDBImpl::Put` **不再**返回 `kFrozen`（容量不足改为冻结 + 停等），这属于 M2 契约的**外部行为变化** | **按本设计接受**（依据 M2 设计 §1.2 边界 1 的原文"M3 引入 flush 后由 `MakeRoomForWrite` 消化"） | 若要保持"容量不足仍返回 `kFrozen`" ⇒ 与"M3 验收口径：自动落盘"直接冲突，且 M2 阻断项 1 的整类 bug 复活 | 是 |

---

## 14. 参考与引用

| 出处 | 用途 |
|---|---|
| `M3-SSTable与刷盘.md` §0/§1/§2 | 本阶段任务书（6 目标、7 条硬性约束、8 个开放决策、A/B 组必含用例、M3.1~M3.3 拆分表） |
| `README.md` §一~§五 | 五段流水线、`#0` 的暂停闸门、红线（不吹/不省门禁/不提前实现/不动别人的房子）、工程约定 |
| `docs/protocol.md` §1~§9 | 复用 §1（LE）/§2/§4（varint/length-prefix）/§5（CRC32C 与 `Extend`）/§6（内部 key 与降序比较）/§7（条目编码）/§9（WAL record 与 CRC 覆盖面）；**§4 给出追加 §10 的完整 patch** |
| `docs/m1-design.md` §4/§8/§10/§12 | M1 冻结接口、`MemTable`/`Arena` 口径、`Env` 能力边界、子里程碑拆分范式 |
| `docs/m1-prerequisites.md` §1/§2/§6/§7 | I1~I10、L1~L6、未定义行为清单（§6 的 11 条 M3 全部继续适用）、测试前置假设 |
| `docs/m1-review.md` §1/§2/§4 | "解码前必须校验长度"（阻断项 2）→ §3/§5.4 的块解析纪律；"比较器等价判定不得退化成逐字节"（阻断项 1）→ §5.4 步骤 ⑤ |
| `docs/m2-design.md` §1.2/§1.4/§3.3/§4/§5.3/§5.7/§6/§9/§10/§11/§12 | 边界取舍的写法、不变量映射表范式、WAL 生命周期与"可删判据"、`Env` 增补清单、`SYNC`/组提交/关闭路径、测试矩阵与子里程碑拆分范式、**§11.2 的 fsync 实测（≈2.6 ms，证伪 8 ms）**、**§11.3 的 `kill -9` 实测（产生不了 torn record）** |
| `docs/m2-prerequisites.md` §1~§11 | I11~I20、L7~L12（M3 全部继续成立）、风险清单、`#1` 阶段发现的 5 处设计缺陷（含"writer 能写出自己读不回来的文件"这类边界教训）、§11 的接续点 |
| `docs/m2-review.md` §1/§2/§3 | **必须吸取的 4 条教训**：① 判据只允许单一真相源（阻断项 1 的容量漂移 ⇒ §6.2）；② 门禁必须能区分"通过"与"工具没跑起来"（阻断项 3 的空绿 ⇒ §8.6 G3/G7）；③ 截断/跳过/丢弃必须计数 + 上报（⇒ §8.4 的两张表）；④ 输入校验不得触发 fail-stop（优化项 2 的粘性写只读 ⇒ §6.2/§8.5/`M3-A21`/`M3-A53`） |
| `docs/roadmap.md` §0/§2/§3 | 单向分层、**阶段硬边界（"M3 不得出现分层与 MANIFEST"）**、工程约定（未跑不算过、三构建目录、0 warning、负结果入档） |
| `src/db_impl.cpp`（HEAD `3603696`） | M2 的组提交/恢复实现，M3 的直接起点；§0.5 的 `Sync()` 缺口出处 |
| `src/wal.cpp` / `src/wal.h` | §0.3 口径实证的对象；恢复期重放 WAL 的唯一入口 |
| `src/util/env.h` / `env_posix.cpp` | `Env` 的能力边界；`SyncDir` 缺失的实证（§0.1 W3） |
| `tests/memenv.{h,cpp}` | 崩溃语义的唯一 seam（fsync 水位 + 固定种子撕裂）；M3 需补 `RandomAccessFile`/`SyncDir`/句柄计数 |
| 本设计的探针 `/tmp/m3probe/{scale_probe,block_probe,wal_crc_probe}.cpp` | §0.3/§0.4 的全部原始数字；**throwaway，不进仓库** |

---

## 15. 修订记录（`#1` 完成 → `#2` 开始之前）

本节记录 `#0` 设计冻结（`961343e`）之后发生的、**会改变本设计条文**的事实与裁决。任一条都不得被
后续实现当作"设计原文"绕过。

### R1 —— I32（M2 的 `Sync()` 水位越界）已在 M2 修完，M3 的责任被取代

- **原设计**：§0.5 登记 I32，Q8 裁决为"在 M3 里顺手修"，回归用例 `M3-A50`。
- **实际发生**：用户裁决改为「单开 M2 补丁」，已于 `a3c85a8` 落地（新增 `appended_seq_` + 先快照后 fsync +
  单调发布 + 恢复期同界起步），回归用例 `GroupCommit.SyncDoesNotClaimInFlightBatch`
  （RED 原始输出 `actual: 1 vs 1` 存档 `docs/m2-tdd-red-i32.log`；M2 的 TSan 同时抓到并修掉了
  `bg_error_` 的锁纪律缺陷 M2-I35）。
- **对本设计的修改**：删除"M3 承担 I32 修复"的一切表述（§0.5、§6.1 锁表、§6.6.1 阶段 C、§9.1 I32 行、
  §10.1 `M3-A50`、M3.3 风险行、§13 Q8）；`M3-A50` 改名为 `Sync.PerLogBoundaryAfterRotation`，
  只验证**轮转后**的 per-log 边界；`log_last_appended_seq_` 继续保留，理由改为轮转。
- **同步记录**：`docs/m2-prerequisites.md` §9.1、`docs/m2-evidence.md`（M2 侧证据与验收数字）。

### R2 —— N1 裁决：批准按契约收窄 `tests/crash_test.cpp:456` 的 `kFrozen` 断言

- **冲突**：该用例 `ASSERT_TRUE(last.IsFrozen())`（:474），而本设计 §1.2 边界 1 / Q10 明写 M3 起
  `PersistentDBImpl::Put` **不再**返回 `kFrozen`（容量不足改为冻结 + 写者停等，由 flush 消化）。
- **裁决（批准收窄）**：在 M3.2 的同一次提交里把该用例改名为描述新契约者（`DB.PutBlocksUntilFlush`），
  断言改为「写入最终返回 `kOk` 且数据可读、期间发生了 flush」；旧 `kFrozen` 断言删除。
  依据 = `m2-prerequisites.md` §9 对 `DB.OpenPersistentMode` 的先例（契约变化 ⇒ 断言随契约更新）。
- **禁止的替代做法**：为了让旧断言继续绿而保留 `Put` 返回 `kFrozen` —— 与"M3 验收口径：自动落盘"
  直接冲突，且会复活 M2 阻断项 1 那一整类 bug。

### R3 —— N2 裁决：`SpyEnv` 必须先加宽，否则 `M3-A23` 属"空绿"

- **问题**：现有 `SpyEnv` 只拦 `Append`/`Sync`，看不到 `rename`/`CreateDir`/`SyncDir`/`GetFileSize`/
  `GetChildren`/`RemoveFile`/`Truncate`/块读 ⇒ `M3-A23`（"注册时才落盘、顺序正确"）会通过但什么都没测。
- **裁决**：`M3-A23` 与 `M3-A47` 的**前置条件**是探针覆盖上述全部调用，并且**必须**配一条反向自检
  （把注入点计数断言写反应当失败，或断言每个注入点的计数 `> 0`）。未加宽探针之前，这两个用例的
  通过**不得**计入 M3 验收。

### R4 —— N3 裁决：`MemEnv` 不建模 dirent ⇒ `SyncDir` 结论只能写成"顺序断言"

- **事实**：`MemEnv::RenameFile` 一次赋值即永久生效，`SimulateCrash()` 不触碰 `dirs_` ⇒ 目录项掉电
  语义在本机与 VM 上都**没有**验证手段（`Env::SyncDir` 在 `src`/`tests` 里此前零命中，M3 才首次实现）。
- **裁决**：M3 交付物中所有涉及 `SyncDir` 的结论**只能**写"调用了 `SyncDir` 且顺序位于 `rename` 之后、
  写 `META` 之前"；**禁止**写"掉电安全已证明"。该缺口登记为 M3 的头号证据强度薄弱点（§12.5）。

### R5 —— 基线重钉

`m2-wal` 已从 `8189607` 前移到 **`a3c85a8`**（force push；远端 tag 对象 `9bde3a6`）。⇒ M3 的实现基线
= `a3c85a8`（含 A22/A25 + `GetRecoveryStats` + I32/I35）；设计文档本身冻结在 `961343e`，`#1` 前置条件
文档为 `a83053e`。M3 后续一切"与 M2 基线对比"的证据都必须以 `a3c85a8` 为基准。

### R6 —— M3.1 文件拆分偏离（登记，非放宽）

- **设计原文（§11.1）**：新增 `src/sstable/{format.h, block_builder.{h,cpp}, block.{h,cpp},
  footer.{h,cpp}, table_builder.{h,cpp}, table.{h,cpp}}`。
- **实际交付**：`src/sstable/format.{h,cpp}`（常量 + `BlockHandle` + `Footer`）、
  `src/sstable/block.{h,cpp}`（`BlockBuilder` + `BlockReader` + `ValidatePayload`），
  用例文件 `tests/sstable_format_test.cpp`（对应 §10.1 的 A01~A08）。
- **理由**：把 `BlockHandle`/`Footer`（合计不足 90 行）单独拆成 `footer.*` 之类的文件对，会让
  M3.1 的 6 个文件里 3 个近乎空壳；合并后每个文件的职责边界反而更清楚（"块内布局" vs "文件级布局"）。
- **影响面（必须明说）**：**§3.2/§3.3/§3.6/§3.7 的位级契约不受任何影响**（同一份字节布局）；
  受影响的只是"文件如何切分"这一工程组织项。
- **不得因此省掉的判据**：§11.1 的**零依赖**机制化断言必须原样保留 ——
  `nm -C build/liblsm_sstable.a | grep -cE 'version|db_impl|wal'` ⇒ **0**（R6-a 已落地并实测为 0）。
- **后续遵守**：M3.1 剩余部分（`table_builder.*` / `table.*`）**按设计原文件名**落地，不再合并。
- **另一条已登记的偏离及其处置（R6-e，暂不闭合）**：`Table::ReadExactFile` 当前用
  `Env::NewSequentialFile + Skip + Read` 模拟按偏移读块（每读一个块打开一次顺序文件）。
  代理已在 `src/sstable/table.h` 顶部显式登记该偏离与替换点。本设计**刻意不在 M3.1 内闭合它**：
  §11.1 只要求 `Env` **具备** `RandomAccessFile`（R6-b 已交付），并未要求 M3.1 的 `Table` 改用它；
  而隔离实验证明现在就换会让 `M3-A19` 的"计数 seam 真被用上"断言（`sequential_files_opened >= 1`）
  失效 —— 该断言的意图是防空绿，值本身要随 API 一起改。⇒ 与 `TableCache` 的引入**同批**在 M3.2
  完成（一次把"随机读 API + 缓存 + 计数 seam"三件事改到位），M3.2 必须更新该计数器的语义。
