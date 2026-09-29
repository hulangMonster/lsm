# M3 实现前置校验（docs/m3-prerequisites.md）

> `#1` 阶段产物（`/verification-before-completion`）。**本文件不写实现代码**；它把已冻结的
> `docs/m3-design.md`（2188 行，rev `961343e`）与 `docs/protocol.md`（204 行，§1~§9）冻结成一组可校验的
> 不变量（I21~I34）、锁纪律（L13~L21）、**前置条件清单**、风险与未知项、文件清单、边界全集、
> 未定义行为清单、**测试套件缺陷登记**与验收命令。
>
> 流程锁：`#1` 阶段发现缺陷必须回退 `#0` 修改设计，**禁止在校验阶段私自改实现方案**。
>
> 本文件的一切「已具备 / 通过」结论都附**本轮实测**的命令与原始输出；跑不出来的写「未验证」，
> 不写推断值。所有探测均为**只读**：未修改 `src/`、`tests/`、`scripts/`、`CMakeLists.txt`，
> 未 `git commit`、未 `git push`、未在仓库内做干净重建。

---

## 0. 校验范围与结论

- 校验对象：`docs/m3-design.md`（§0~§14，2188 行）、`docs/protocol.md`（§1~§9，204 行；
  **§10 SSTable 章节尚未落地**，设计 §4 以完整 patch 文本给出）、`docs/m1-prerequisites.md`（I1~I10/L1~L6）、
  `docs/m2-prerequisites.md`（I11~I20/L7~L12）。
- 结论：设计已覆盖 `M3-SSTable与刷盘.md` 的 6 条目标、7 条硬性约束、8 个开放决策；
  §10 的测试矩阵 A01~A54 / B01~B10 **逐条可执行**，每条都标了依赖假设/通过判据/所需 seam。
- 本阶段**未发现需要回退 `#0` 的阻断级设计缺陷**。但登记了 **3 条前置条件缺口**（§3 的 P1/P2/P6 等，
  均为设计已预见并已列入「必须新增/必改」清单）与 **6 条测试套件缺陷/疑点**（§9），
  其中 **§9.1 与 §9.3 必须在写用例之前与用户对齐**（一条是既有断言与 M3 契约直接冲突，一条是 M2 门禁的
  「持锁零 IO」探针覆盖不到 M3 新增的 IO 形状）。
- **一条基线事实（实测）**：探测期间 `~/lsm-kv` 工作区被**另一个并发进程**改脏（不是本阶段所为），
  见 §1.3。本文件的一切源码/测试结论都**锚定 rev `961343e` 的干净本机只读克隆**
  （`D:\JLProject\lsm-kv`），不受该并发改动影响。

---

## 1. 范围与基线

### 1.1 M3 要做什么（6 条目标，逐字对应设计 §1.1 的 G1~G6，外加设计补齐的 G7）

| # | 目标 | 落地章节 |
|---|---|---|
| G1 | SSTable 二进制格式：4 KiB 数据块（前缀压缩 + restart 16）+ 索引块 + 44 B 定长 footer（magic `"LSM1"`）+ 空 metaindex（为 M5 的 Bloom 预留） | 设计 §3 / §4 |
| G2 | `TableBuilder` / `TableReader`：顺序追加、`Finish`、块级读取、每块 CRC32C（读时校验默认开、可关） | 设计 §5 |
| G3 | flush 路径：MemTable 满 → 冻结进 `immutables_` → 写 SSTable（**durable → rename → SyncDir → 注册**） | 设计 §6 |
| G4 | 读路径串联：`Get` 依次查 MemTable → immutable → SSTable（**文件号降序 = 新→旧**），覆盖语义 + tombstone 屏蔽 | 设计 §7 |
| G5 | `MergingIterator`（只归并）+ `DBIter`（只做可见性/tombstone/三态状态机）的最小可用版本 | 设计 §7.3 |
| G6 | 启动恢复：`META` 重建 SSTable 集合 + **SSTable + 残余 WAL** 组合恢复 | 设计 §8 |
| G7 |（设计补齐）WAL 回收判据落地：`min_log_to_keep`（I34）+ 真删 `.log` | 设计 §6.6.2 |

### 1.2 M3 **不**做什么（硬边界，评审逐条对照设计 §1.2 / §12.4）

分层 compaction 与层级结构（`Level`/`Compaction`/`MaxBytesForLevel`）、**`MANIFEST`/`VersionEdit` 追加日志/
`VersionSet` 版本图/`CURRENT` 间接指针**、Bloom filter 内容（只写**空的** metaindex）、`WriteBatch` 对外接口、
块缓存/LRU block cache、压缩算法、并发 compaction/多后台线程（M3 只有**一个**后台 flush 线程）、
`ReadOptions::snapshot`/`Snapshot*` 公共 API、**`DB::Flush()` 等新增公共 API**、与 raft-kv 的对接、
**真删除已注册的 SSTable**、原地修改已注册 SSTable 的任何字节（I21）。

### 1.3 基线 rev 与 tag（实测）

**权威基线 = `961343e`**（= M2 最终代码 rev `3603696` + M3 设计冻结提交）。

```
$ cd ~/lsm-kv && git rev-parse HEAD
961343e7ae0b8ea6c77c8b886550c5cc5867e59e
$ git log --oneline -4
961343e (HEAD -> main, origin/main) docs(m3): 冻结 M3 设计（SSTable 格式 + flush/读路径 + 测试矩阵）
3603696 feat(m2): A22 mixed-sync batch + A25 probe for I17 (zero IO while holding the DB mutex)
75eb1c0 feat(m2): GetRecoveryStats (design 8.2) with counted truncation/skips and threshold WARN
8189607 (tag: m2-wal) docs(m2): record final gate matrix on the post-fix code (gate 5/5 + TSan 79/79 race 0)
$ git rev-list --count 8189607..HEAD
3
$ git rev-list --count 8189607..3603696
2
```

**tag `m2-wal` 的精确形态（实测，比「指向 `8189607`」更准确）**：它是一个**附注 tag（annotated tag）**，
tag 对象自身是 `5e3a7b8`，剥离后指向 commit `8189607`。

```
$ git cat-file -t m2-wal
tag
$ git rev-parse m2-wal
5e3a7b8b88d4bcc789b2f928a05736a2fa031112      ← tag 对象本身（不是 commit）
$ git rev-parse "m2-wal^{commit}"
8189607d649cc5bf69b4c0a753f2e5560d9df999      ← 它指向的 commit
```

- `8189607` 落后 **M2 最终代码 rev `3603696` 两个提交**（`75eb1c0`、`3603696`），
  落后 HEAD `961343e` **三个提交**（多出的第三个是设计冻结提交）。
- ⇒ **M3 的实现基线是 `961343e`，不是 `m2-wal`**。tag 少两个提交的修复，且不含 `GetRecoveryStats`
  （设计 §8.4 的可观测性口径要用它）。
- **`m2-wal` 前移约定**：**I32 补丁落地后会把 `m2-wal` 移到 M2 最终 rev**（`M3-B06` 与 `docs/m3-design.md`
  §13 Q9 的口径据此核对）。本阶段实测到该补丁**正在被并发应用**，见下条。

### 1.3.1 探测期间观测到的并发改动（**必须登记，因为它动基线**）

本阶段开工时工作区干净，探测途中被**另一个并发进程**改脏。**本阶段没有修改也没有回退它。**

```
# 开工时（00:05）
$ git status --porcelain | wc -l
0
# 跑完一轮只读探测后（00:06）——本阶段只执行了 ./build/bin/lsm_tests 与 git 读命令
$ git status --porcelain | wc -l
3
$ git status --porcelain
 M src/db_impl.cpp
 M src/db_impl.h
 M tests/crash_test.cpp
$ git diff --stat
 src/db_impl.cpp      | 22 +++++++++++++++--
 src/db_impl.h        |  3 +++
 tests/crash_test.cpp | 70 ++++++++++++++++++++++++++++++++++++++++++++++++++++
 3 files changed, 93 insertions(+), 2 deletions(-)
$ ps -eo pid,etime,args | grep -E 'cmake|make|c\+\+' | head
69074       00:16 cmake --build build-asan -j8
69551       00:03 /usr/bin/c++ ... -fsanitize=address ... -c .../tests/crash_test.cpp
```

改动内容与设计 §0.5 登记的 **M2 `Sync()` 水位越界完全对应**（新增 `appended_seq_`、`Sync()` 改为
「先快照 `appended_seq_` 再 fsync、单调发布 `durable_seq_ = max(...)`」、恢复期
`db->appended_seq_ = last;`，以及新增 `TEST(GroupCommit, SyncDoesNotClaimInFlightBatch)`）。
即：这就是设计 §1.2 所指的 **I32 补丁**。

- **对 M3 的影响**：若该补丁落成 commit，**M3 的实现基线必须重钉到那个新 rev**（它会更接近
  `docs/m3-design.md` §0.5/I32 所假设的代码状态）。本节登记的 `961343e` 是**本阶段实测的**基线；
  `#2`/`#3` 开工前必须重新 `git rev-parse HEAD` 并确认是否已前移。
- **本阶段的处置**：全部源码/测试结论改用**干净的本机只读克隆**核对，避免把并发改动误当基线：

```
$ cd /d/JLProject/lsm-kv && git rev-parse HEAD && git status --porcelain | wc -l
961343e7ae0b8ea6c77c8b886550c5cc5867e59e
0
```

### 1.4 沿用而不重定义的上游契约

I1~I10 / L1~L6（`m1-prerequisites.md` §1/§2）与 I11~I20 / L7~L12（`m2-prerequisites.md` §1/§2）
**全部继续成立**，M3 只**追加**（本文件 §4/§5）。设计 §1.3.1 逐条列出了 M3「逐字复用、不改一行」的既有契约
（内部 key 编码与比较、`Slice`/`Status`、`BuildLookupKey`、`coding`、`crc32c::Value/Extend`、
`MemTable::{Add,Get,NewIterator,WouldReject}`、`Iterator` 三态状态机、`WALReader` 的
「尾部残骸 vs 中间损坏」判定、`RecoveryStats` 只增不改、`CommitHook` 语义不变、`FileLock`）。

---

## 2. 环境实测（全部为本轮真实跑出，附命令与原始输出）

> 执行时间：2026-09-30 00:05 ~ 00:20 (+08:00)，机器 `ubuntu-vm`（`tengyujie-virtual-machine`）。
> **注意**：探测期间有一个并发 `cmake --build build-asan -j8` 在跑（§1.3.1），
> `loadavg` 为 `1.22 1.17 1.32`（不是静载）。凡涉及耗时的数字都标注这一点。

### 2.1 内核 / 发行版 / CPU / 内存

```
$ uname -a
Linux tengyujie-virtual-machine 6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC Fri Aug  7 13:43:15 UTC  x86_64 GNU/Linux
$ grep -E '^(NAME|VERSION)=' /etc/os-release
NAME="Ubuntu"
VERSION="22.04.5 LTS (Jammy Jellyfish)"
$ nproc
8
$ grep -m1 'model name' /proc/cpuinfo
model name	: 13th Gen Intel(R) Core(TM) i7-13700H
$ free -h | head -2
               total        used        free      shared  buff/cache   available
Mem:           7.7Gi       1.4Gi       3.5Gi        13Mi       2.8Gi       6.0Gi
```

⇒ 8 vCPU / 7.7 GiB RAM。**内存 7.7 GiB 是 M3 的压力上界**：设计 §0.4(b) 的 100 万 key 探针用了
1 GiB 写缓冲，M3 的 B 组（`M3-B08`/`M3-B09`）在同一台机器上跑 100 万写时**必须换算写缓冲**，
不能照搬探针的 1 GiB 配置。

### 2.2 工具链

```
$ g++ --version | head -1
g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0
$ cmake --version | head -1
cmake version 3.22.1
$ make --version | head -1
GNU Make 4.3
$ which g++ cc cmake make ninja
/usr/bin/g++
/usr/bin/cc
/usr/bin/cmake
/usr/bin/make
（ninja 未安装 ⇒ 门禁脚本只能用 `cmake --build ... -j8`，沿用 M2）
```

⇒ **C++17 判别式**：`g++ 11.4.0` 全量支持 M3 要用的 C++17 设施（`std::shared_ptr`/`std::deque`/
结构化绑定/`if constexpr`）。CMake **3.22.1** ⇒ 可用 `target_link_libraries` 的 PUBLIC/PRIVATE 语义
（设计 §11 的 `lsm_sstable` 独立目标依赖它）。

### 2.3 可观测性（决定性能剖析手段）

```
$ cat /proc/sys/kernel/perf_event_paranoid
4
$ cat /proc/sys/kernel/kptr_restrict
1
$ cat /proc/sys/kernel/yama/ptrace_scope
1
```

⇒ `perf_event_paranoid = 4`（`>2`）⇒ **perf 火焰图在本机不可用**（与 `roadmap.md` §五 一致）。
M3 的性能结论**只能**来自自带计数的微基准（设计 §7.4 的 `ReadStats`、§8.4 的 `FlushStats`），
**不得**声称做过任何 profiler 级归因。

### 2.4 磁盘（决定 B 组真实目录用例与「单块盘」口径）

```
$ df -hT $HOME /tmp
Filesystem                Type  Size  Used Avail Use% Mounted on
/dev/mapper/vgubuntu-root ext4   37G   24G   12G  67% /
/dev/mapper/vgubuntu-root ext4   37G   24G   12G  67% /
$ findmnt -no SOURCE,FSTYPE /
/dev/mapper/vgubuntu-root ext4
$ lsblk -d -o NAME,SIZE,ROTA,MODEL
NAME     SIZE ROTA MODEL
sda       40G    1 VMware Virtual S
$ cat /sys/block/sda/queue/rotational
1
```

⇒ 文件系统 **ext4**，设备 `/dev/mapper/vgubuntu-root`（LVM，底层 `sda` = **VMware Virtual S**，40 GB）。
**`/` 与 `/tmp` 同盘同分区**（`df` 两行相同）⇒ 设计 §11 里「B 组的真实目录用例放在临时目录」与
「`/tmp` 与仓库同盘」是同一件事，不存在跨盘干扰，也**不能**用「换盘」隔离 IO 干扰。
**剩余可用 12 GB**：`M3-B08`/`M3-B09` 的 100 万写场景（设计 §0.4 实测 WAL 138 MB + SSTable 110 MB
量级）空间足够，但**必须每轮清理**，否则 100 轮累积会吃掉余量。
**`ROTA=1`（rotational）**：虚拟盘对内核自报为旋转设备 ⇒ M3 **不得**依据 `ROTA` 做任何 IO 调度器假设。

### 2.5 GTest

```
$ ls -la /usr/local/lib/libgtest*
-rw-r--r-- 1 root root 2927934  9月  4 11:01 /usr/local/lib/libgtest.a
-rw-r--r-- 1 root root   3876  9月  4 11:01 /usr/local/lib/libgtest_main.a
$ ls -d /usr/local/include/gtest
/usr/local/include/gtest
$ ls /usr/lib/x86_64-linux-gnu/libgtest*
ls: cannot access '/usr/lib/x86_64-linux-gnu/libgtest*': No such file or directory
$ dpkg -l | grep -i gtest
（无输出）
```

⇒ GTest 是**源码自建于 `/usr/local`** 的静态库，**不是** apt 包（`dpkg` 无记录、系统库目录无产物）。
⇒ M3 新增测试（`block_test.cpp` 等）沿用 `CMakeLists.txt` 现有的既有查找方式即可，
**不要**引入 `find_package(GTest)` 或在文档里写「apt 安装 gtest」。

### 2.6 三个构建目录（**均可用，本轮实测**）

```
--- build ---            dir_mtime=2026-09-29 23:46:44
CMAKE_BUILD_TYPE:STRING=Release
ENABLE_ASAN:BOOL=OFF
ENABLE_TSAN:BOOL=OFF
bin/: fsbench_commit_latency  lsm_crash_recover  lsm_crash_writer  lsm_damage_test  lsm_tests
--- build-asan ---       dir_mtime=2026-09-29 23:48:00
CMAKE_BUILD_TYPE:STRING=RelWithDebInfo
ENABLE_ASAN:BOOL=ON
ENABLE_TSAN:BOOL=OFF
bin/: 同上 5 个目标
--- build-tsan ---       dir_mtime=2026-09-29 23:50:19
CMAKE_BUILD_TYPE:STRING=RelWithDebInfo
ENABLE_ASAN:BOOL=OFF
ENABLE_TSAN:BOOL=ON
bin/: 同上 5 个目标
```

**三者的「可用」不只是目录存在，而是真的跑起来了（原始输出）**：

```
$ ./build/bin/lsm_tests 2>&1 | tail -3
[==========] 82 tests from 19 test suites ran. (27612 ms total)
[  PASSED  ] 82 tests.
EXIT=0

$ ./build-asan/bin/lsm_tests 2>&1 | tail -3
[==========] 82 tests from 19 test suites ran. (73575 ms total)
[  PASSED  ] 82 tests.
EXIT=0

$ setarch $(uname -m) -R ./build-tsan/bin/lsm_tests --gtest_filter="Slice.*:Status.*:Coding.*:CRC32C.*" 2>&1 | tail -3
[==========] 11 tests from 4 test suites ran. (396 ms total)
[  PASSED  ] 11 tests.
EXIT=0
```

- **诚实登记**：TSan 只跑了 **11 例冒烟**（证明二进制可加载、TSan 运行时在位、`setarch -R` 有效），
  **没有**跑全量 82 例。全量 TSan 在本机约 410 s（M2 记录）+ 当前有并发构建占 CPU，
  本阶段判定「不可用/全量未验证」。**全量 TSan 必须由 `#2`/`#3` 在无并发负载时补跑。**
- ⇒ **`setarch $(uname -m) -R` 仍是 TSan 的必需前缀**（实测有效）。

### 2.7 当前全量用例数与分布

```
$ ./build/bin/lsm_tests 2>&1 | tail -3
[==========] 82 tests from 19 test suites ran. (27612 ms total)
[  PASSED  ] 82 tests.
$ ./build/bin/lsm_tests --gtest_list_tests 2>/dev/null | grep -c '^  '
82
```

19 个套件：`Slice` `Status` `Coding` `CRC32C` `Arena` `Env` `InternalKey` `MemTableKeyComparator`
`Skiplist` `MemTable` `DB` `Stress` `WAL` `Recovery` `CrashSim` `Sync` `Close` `GroupCommit` `Locks`。

按文件的静态分布（本轮实测，与本机只读克隆一致）：

| 文件 | TEST 数 | 行数 |
|---|---|---|
| `tests/memtable_test.cpp` | 27 | 1537 |
| `tests/util_test.cpp` | 20 | 1210 |
| `tests/crash_test.cpp` | 15 | 668 |
| `tests/recovery_test.cpp` | 10 | 398 |
| `tests/wal_test.cpp` | 10 | 430 |
| **合计** | **82** | — |

⇒ 与设计 §0.2 的基线 **82 例 / 0 warning** 完全一致 ⇒ **M2 的收口基线在本阶段可复现**。
`M3-B06` 的「82 例不得减少」这条分母成立。

### 2.8 `write+fsync` 成本（**本阶段亲自重测，不复用任何历史数字**）

开发指令写的「commit 级 fsync 约 8 ms」已被 M2 实测证伪；本阶段独立重测一次，
探针为一次性程序（写在 `/tmp/m3probe/`，**未进仓库**），每轮 = `write(4096)` + `fsync`：

```
$ cat /proc/loadavg          # 探测前
1.22 1.17 1.32 3/661 70042
$ g++ -O2 -std=c++17 -o /tmp/m3probe/fsync_probe /tmp/m3probe/fsync_probe.cpp && /tmp/m3probe/fsync_probe 200
FSYNC_PROBE n=200 bytes=4096 p50=3.759 ms p90=5.360 ms mean=3.963 ms min=2.011 max=9.446
$ /tmp/m3probe/fsync_probe 200        # 第二次
FSYNC_PROBE n=200 bytes=4096 p50=3.600 ms p90=4.914 ms mean=3.575 ms min=1.483 max=8.885
$ cat /proc/loadavg          # 探测后
1.37 1.21 1.33 2/659 70051
```

| 量 | 本轮实测（负载下） | M2 实测（`docs/m2-design.md` §11.2） | 开发指令 |
|---|---|---|---|
| `write+fsync` p50 | **3.60 ~ 3.76 ms** | 2.556 ms | 「约 8 ms」 |
| `write+fsync` mean | **3.58 ~ 3.96 ms** | 2.611 ms | — |

**结论（三条，都必须写进结论）**：

1. **指令的「约 8 ms」再次被证伪**：本轮 p50 3.60~3.76 ms，是它的 **~0.46 倍**。
2. 本轮比 M2 的 2.556 ms **高约 40%**，归因已定位：探测期间有并发 `cmake --build build-asan -j8`
   （§1.3.1），`loadavg` 1.22~1.37，且这是**同一块虚拟盘**。⇒ 这是**负载下的上界**，不是静载值。
   两者同量级、方向一致（都是「2.6~3.8 ms，远小于 8」），**不构成矛盾**。
3. 设计 §0.6 取 `p50 ≈ 2.6 ms` 做 D5/D6 的成本分母是**保守且合理**的；M3 的判据
   （设计 §10）**不使用绝对吞吐**，只用「4 次 fsync 类操作 ≈ 10.4 ms + 2 MB 顺序写」这个**量级**论证，
   该论证对本轮任何一次测量都成立。

> **口径声明**：以上是**单机单块 ext4 虚拟盘、8 vCPU、带并发负载**的数字，
> 只用于本实现内部的**相对**判断，**不构成任何跨机器承诺**（`roadmap.md` §3.7「不吹」）。

### 2.9 与设计 §0 的差异（诚实登记）

| 项 | 设计 §0 记录 | 本阶段实测 | 说明 |
|---|---|---|---|
| 工作区状态 | §0.1 采集时脏、提交前已回退干净 | 开工干净，**探测中途再次被动变脏**（§1.3.1） | 并发 I32 补丁；不影响锚定 `961343e` 的结论 |
| `fsync` p50 | 未重测，沿用 M2 的 2.6 ms | **重测 3.60~3.76 ms**（负载下） | 见 §2.8，同量级 |
| TSan | 未在 §0 记录 | 冒烟 11 例通过；**全量未验证** | 见 §2.6 |
| 磁盘余量 | 13 G 可用 | **12 G 可用** | 100 轮门禁必须每轮清理 |

---

## 3. M3 前置条件清单（前置项 → 为何需要 → 是否具备 → 证据/缺口）

> 「是否具备」只有三种取值：**已具备**（附原始证据）、**部分具备**（说清哪一半）、**不具备**。
> 「不具备」不等于阻断：设计已把它们列入「必须新增/必改」清单，本表的作用是**把它们变成可核对的闸门**。

### 3.1 `Env` 能力

| # | 前置项 | 为什么 M3 需要它 | 是否具备 | 证据（命令 + 输出）/ 缺口 |
|---|---|---|---|---|
| **P1** | **`Env::SyncDir(const std::string&)`** | **M3 的 `META` 原子 rename 持久化的唯一依赖**。写 `META.tmp` → `fsync` → `rename` → **`SyncDir`**；没有它，「rename 之后注册」**不构成任何持久性保证**（设计 §1.3.2、§6.3 步骤 ⑥、I22/L17 加强版）。M2 设计 §5.7 声明过、实现从未落地 | **不具备** | `grep -rn "SyncDir" src tests` ⇒ **零命中**（命中全在 `docs/`）。`src/util/env.h` 的 `class Env`（41~70 行）无该成员。原始佐证：设计 §0.1 的编译错误 `error: 'class lsm::Env' has no member named 'SyncDir'`。⇒ **M3 必须新增，并且是三处同步**：`env.h`（纯虚）、`env_posix.cpp`（实现）、`MemEnv`/`FaultyEnv`（实现） |
| **P2** | **`Env::RandomAccessFile` + `NewRandomAccessFile`** | SSTable 的块级随机读必须能「按 handle 读一段」。`SequentialFile` 只有 `Read`/`Skip`（单向前进），**无法回退**，因此读索引→读数据块的两跳无法实现（设计 §1.3.2） | **不具备** | `grep -rn "RandomAccessFile" src tests` ⇒ 唯一命中是 `src/util/env.h:4` 的注释「`RandomAccessFile` 留到 M3（避免未使用接口）」。⇒ 属 M1 有意留给 M3 的能力，**M3 必须新增** |
| **P3** | **`Env` 的 rename / 文件大小 / 目录枚举 / 删除** | flush 的 `rename`、恢复的 `GetChildren` 扫目录、`GetFileSize` 校验 `index.offset + index.size == file_size - kFooterSize`、孤儿清理的 `RemoveFile`/`DeleteFile` | **已具备** | `src/util/env.h`：`GetFileSize`(55)、`DeleteFile`(56)、`RenameFile`(57)、`GetChildren`(62)、`RemoveFile`(63)、`Truncate`(64)、`CreateDir`(58)。8 个纯虚方法齐备且被 M2 的恢复路径实际使用 |
| **P4** | **只读打开** | SSTable **绝不**以可写方式打开（I21「注册后不可变」的结构性保证） | **部分具备** | `NewSequentialFile`(53) 是只读但**只能顺序读**；`NewWritableFile`(47) 是截断式可写。**没有**「只读 + 随机访问」的打开方式。⇒ P2 落地时必须保证 `NewRandomAccessFile` **只读**（`O_RDONLY`），这是 I21 的机制保证，不能靠人工纪律 |
| **P5** | `Env::NewAppendableFile` | WAL 恢复后继续追加 | **已具备** | `src/util/env.h:51`（M2 增补，M3 沿用） |

### 3.2 测试 seam

| # | 前置项 | 为什么 M3 需要它 | 是否具备 | 证据（命令 + 输出）/ 缺口 |
|---|---|---|---|---|
| **P6** | **`CommitHook` 的三个注入点是否够 M3 的 flush 测试用** | M3-A20/A21/A22/A24（组提交语义不变）要用它；但 **M3 的 flush 顺序判据（`M3-A22`）需要打在 flush 路径上** | **不够（需新增 `FlushHook`）** | `src/common.h:27-35`：`class CommitHook` 只有 3 个虚函数 —— `OnBeforeGroupAssemble()`(32)、`OnGroupTaken()`(33)、`OnAfterSyncBeforePublish()`(34)，三个**全在 WAL/组提交路径上**，**没有一个在 flush 的 write/rename/register 上**。设计 §8.7 **E8** 已要求新增 `class FlushHook { OnSSTableWritten(); OnBeforeRename(); OnBeforeRegister(); }`，与 `M3-B02` 的三个进程级 kill -9 注入点**逐字对应**。`Options`（`src/common.h:241-250`）现只有 `comparator`/`write_buffer_size`/`env`(246)/`commit_hook`(249)，**无** `flush_hook` |
| **P7** | **`MemEnv` 是否建模目录项与 rename**（关系到掉电语义可测性） | 设计 §12.5 第 1/5 条**主动暴露的最明显证据强度缺口**：`META` 的持久性靠「rename + SyncDir」，若 `MemEnv` 不建模 dirent，则 A 组**只能断言调用顺序，不能断言掉了会怎样** | **不具备（只建模文件数据，不建模目录项）** | `tests/memenv.h:86-87`：状态只有 `std::map<std::string, File> files_` + `std::set<std::string> dirs_`，`File` 只有 `{data, synced_size, exists}`（78-82）——**没有 dirent、没有「已 rename 但未 SyncDir」这个状态**。`tests/memenv.cpp` 的 `RenameFile` 直接搬 map 条目（`files_[src].exists=false; files_[target]=moved;`），**原子且永久**；`SimulateCrash()` 只回滚每个文件的 `data` 到 `synced_size`（+ 固定种子撕裂），**从不触碰 `dirs_`、从不撤销 rename**。⇒ **"rename 已发生但目录项未落盘"这一失效模式在 `MemEnv` 里无法表达**。设计 §6.5/§10 的 `M3-A22`/`M3-A47` 因此只能做**顺序断言**；**这是必须写进风险清单的诚实缺口（§6 R1）** |
| **P8** | **`MemEnv` 的 `RandomAccessFile` / `SyncDir` / 句柄计数** | `Env` 加纯虚方法后子类必须实现；`M3-B05`/`I30` 的「文件句柄数有上限、不泄漏」需要句柄计数探针（设计 §1.3.2 末行） | **不具备** | `tests/memenv.h` 无 `RandomAccessFile`；无 `SyncDir`；无任何句柄计数成员（只有 `write_calls_`/`sync_calls_`，98-99）。⇒ 三项都要补 |
| **P9** | **`MemEnv` 的假时钟** | A 组「零 flaky」要求时间不真睡（`M3-A25` 的停等上界、`M3-A51`） | **已具备** | `tests/memenv.h:49-50`：`NowMicros(){return now_micros_;}`、`SleepForMicros(us){now_micros_+=us;}` —— 时间推进但**不真的睡**。起点 `now_micros_=1000000`(91) |
| **P10** | **`MemEnv` 的确定性崩溃注入** | 掉电语义（回滚到 fsync 水位 + 固定种子撕裂尾部）是 A 组唯一能验证「未 fsync 的后缀丢失」的机制 | **已具备** | `tests/memenv.h:53-57`：`SetTearProbability`、`SetSeed`、`SimulateCrash`；`rng_state_` 固定种子 `0x5EED2026`(92) |
| **P11** | **`FaultyEnv` 的故障注入覆盖面**（截断 / 短写 / EIO / ENOSPC） | `M3-A24` 要求「注入 Env，逐个失败点（write / fsync / **rename** / **SyncDir**）各自可失败」；`M3-A49` 要求轮转的 `SyncDir`/建文件失败可注入 | **部分具备（覆盖面明显不足）** | `tests/faulty_env.h:26-30`：只有 `SetShortWrite`(26)、`SetEnospc`(28)、`SetSyncFailureAfter`(30) 三种；计数只有 `write_calls_`/`sync_calls_`(33-34)。**缺口逐条**：① `RenameFile`(46-48)、`CreateDir`(49)、`GetFileSize`(42)、`DeleteFile`/`RemoveFile`(45/53)、`Truncate`(54)、`GetChildren`(50) **全是纯转发，无法注入失败**；② **无 `SyncDir`**（因 `Env` 没有）；③ **无通用 EIO 注入**；④ **无 `Truncate` 注入**（M2 的「截断注入」是 `MemEnv` 侧的，`FaultyEnv` 没有）；⑤ **无事件日志**（`M3-A22`/`M3-A47` 的「顺序断言」需要）。⇒ `M3-A24`/`M3-A49`/`M3-A22`/`M3-A47` **都缺 seam** |
| **P12** | **事件日志 Env（记录「谁先谁后」）** | `M3-A22`（`fsync` 早于 `rename`、`SyncDir` 早于 `META` 的写）与 `M3-A47`（删除 `*.log` 严格晚于 `META` 的 `rename`+`SyncDir`）**都是纯顺序判据** | **不具备** | `grep -rniE "event.?log\|SpyEnv\|CountingEnv\|handle_count\|/proc/self/fd" tests/` ⇒ 唯一命中是 `tests/crash_test.cpp:615` 的 `SpyEnv`（只做「是否持锁」判定，**不记事件顺序**）。⇒ 需新增一个记录操作序列的 Env |
| **P13** | **`SpyEnv`（持锁零 IO 探针）的覆盖面** | `M3-A23`（L13/L18/L19）要求断言「flush 的 `write/fsync/rename/SyncDir` 与 `Get` 的块读，在持锁期间调用次数 == 0」 | **已具备但覆盖不到 M3 新增的 IO 形状** | `tests/crash_test.cpp:633` `TEST(Locks, ZeroIoWhileHoldingDbMutex)` 用 `SpyEnv`(615) 包 `MemEnv`，但 `SpyEnv` **只重写了 `NewWritableFile`(617) 与 `NewAppendableFile`(623)**，`SpyFile` **只拦 `Append`(599) 与 `Sync`(604)**。⇒ `rename`/`GetFileSize`/`GetChildren`/`RemoveFile`/`Truncate`/（新增的）`SyncDir` **全部不可见**；失败信息也只说「WAL 的 write/fsync」(663)。**`M3-A23` 按现设计无法满足，必须先把 `SpyEnv` 加宽**（见 §9.2） |
| **P14** | M2 的 `DbMutexHeldOnThisThread()` 探针**必须仍在位且不得被删** | `M3-A23` 的「持有 DB 锁时零 IO」判据直接用它；设计 §0.1 的 W1 记录过它曾被脏 WIP 删掉 | **已具备** | `src/db_impl.h:24` 声明 `bool DbMutexHeldOnThisThread();`；`src/db_impl.cpp:19-21` 的 `struct DbMutexGuard`（构造置 `g_db_mutex_held=true`）、`src/db_impl.cpp:481` 定义。`grep -c` 在 `db_impl.cpp` 命中 12 处 ⇒ 探针在位 |
| **P15** | **慢 flush 的确定性屏障**（不是靠 `sleep` 赌调度） | `M3-A25`（停等解除的四个条件）、`M3-A33`（迭代中途注册）、`M3-A51`（Close 放弃 immutable）都需要「把 flush 卡在中间」 | **不具备** | 现有 `CommitHook` 的屏障（`OnGroupTaken`，见 `tests/crash_test.cpp` 的并发热点）是**组提交**的；flush 侧无对应物。⇒ 由 `P6` 的 `FlushHook` 一并提供（设计 §8.7 E8） |

### 3.3 代码基线（「不提前实现」的核对）

| # | 前置项 | 为什么 M3 需要它 | 是否具备 | 证据（命令 + 输出）/ 缺口 |
|---|---|---|---|---|
| **P16** | **SSTable 相关代码是否存在** | M3 要从零实现；若已存在占位符号，说明 M1/M2 违反了「不提前实现」红线 | **完全没有（符合预期）** | `find src tests -type f` 的完整清单里**没有** `src/sstable/`、`version_edit.*`、`version_set.*`、`merging_iterator.*`、`db_iter.*`。`grep -rniE "sstable\|table_builder\|block_builder\|footer\|merging_iterator\|db_iter\|version_set\|version_edit\|kTableMagic\|LSM1"` 在 `src/` 只命中 **3 处注释**（`db_impl.h:42`、`crc32c.h:3`、`env.h:4`，都是「M3 才做」的说明）。`grep -rniE "MANIFEST\|CURRENT\|bloom\|compaction\|VersionEdit\|VersionSet"` 在 `src/` ⇒ **零命中** |
| **P17** | **`META` / `%06u.sst` 等新文件名的命名入口** | 文件号空间与 `.tmp`/`.sst` 的区分是「半边文件永不注册」的**结构性**保证（设计 §3.1） | **部分具备** | `src/filename.{h,cpp}` 存在（M2 为 `%06u.log` 而建），M3 需**只增** `TableFileName`/`TempFileName`/`ParseTableFileName`/`ParseTempFileName`/`MetaFileName` |
| **P18** | **`docs/protocol.md` §10 是否落地** | 硬性约束 4：「SSTable 磁盘编码**必须与 `docs/protocol.md` 一致**，编码实现与文档不得各说一套，评审会逐字对照检查」 | **未落地（设计已给出完整 patch 文本）** | `grep -n '^## ' docs/protocol.md` ⇒ 止于 `## 9. WAL record 编码（M2 定稿）`；全文件 **204 行**，**无 §10**。设计 §4（`m3-design.md` 773~948 行）以 ````markdown 代码块给出**可直接追加**的 §10 全文（§10.1 命名 ~ §10.9 恢复口径）。⇒ **M3.1 必须把它落地**（设计 §11 M3.1 的「新增」列已点名），落地前后要 `diff` 核对与设计 §3 逐字一致 |
| **P19** | **`CMakeLists.txt` 的独立目标 `lsm_sstable`** | 设计 §1.4 的**机制化**依赖纪律：「`src/sstable/` 不得出现任何 POSIX 头、不得 include `version_*`/`db_impl`/`wal.h`」；靠独立静态库让越权依赖**链接失败**，而不是靠人盯 | **不具备** | 现有目标（`grep -nE "add_executable\|add_library" CMakeLists.txt`）：`lsm`(46, STATIC) / `lsm_tests`(79) / `lsm_crash_writer`(104) / `lsm_crash_recover`(107) / `lsm_damage_test`(111) / `fsbench_commit_latency`(115)。**无 `lsm_sstable`**。设计 §11 M3.1 的证据命令 `cmake --build build --target lsm_sstable` + `nm -C build/liblsm_sstable.a \| grep -c 'version\|db_impl\|wal'` ⇒ 期望 0 |
| **P20** | **崩溃脚本的正向标记与 M3 计数** | `M3-B01` 的判据含 `SST_FILES_TOTAL > 0` ∧ `LOGS_DELETED_TOTAL > 0` ∧ 末尾 `[FLUSH_CRASH_OK]`；`M3-B06` 要求 `lsm_gate.sh` **断言**这些标记 | **不具备** | `scripts/lsm_crash_test.sh` 末尾只打印 `TOTAL_ROUNDS / ROUNDS_OK / ACKED_TOTAL / MISSING_TOTAL / MISMATCH_TOTAL`（第 83 行）与 `[OK] 全部 N 轮 missing 0 / mismatch 0`（96），**无 `SST_FILES_TOTAL`/`LOGS_DELETED_TOTAL`/`[FLUSH_CRASH_OK]`**。`scripts/lsm_gate.sh` 的 `run_gate()` **只看退出码**（`if "$@" >>"$LOG" 2>&1; then`），**没有任何正向标记断言**。⇒ 设计 §8.6 的 G3/G4/G7 全部要新增（新脚本 `scripts/lsm_flush_crash_test.sh` + `lsm_gate.sh` 增补） |

### 3.4 前置条件缺口汇总（按「是否阻断 #2 开工」分类）

| 分类 | 项 | 影响 |
|---|---|---|
| **必须先做，否则 A 组用例写不出来（seam 缺失）** | P6 `FlushHook`、P7 `MemEnv` 目录语义、P8 `MemEnv` 新增能力、P11 `FaultyEnv` 覆盖面、P12 事件日志 Env、P13 `SpyEnv` 加宽、P15 慢 flush 屏障 | 这些**都是测试基础设施**，属于 `#2` 的前置。设计 §11 已把它们分配到 M3.1/M3.2 的「必改」列 |
| **必须先做，否则编译不过** | P1 `Env::SyncDir`、P2 `Env::RandomAccessFile` | 加纯虚方法 ⇒ `MemEnv`/`FaultyEnv` 必须同步实现（设计 §1.3.2 已写） |
| **必须先做，否则与文档不一致（评审逐字对照会挂）** | P18 `protocol.md` §10 落地 | M3.1 的首要交付物之一 |
| **机制保证（不是靠纪律）** | P19 `lsm_sstable` 独立目标、P4 只读打开 | 设计 §1.4 明确要求 |
| **门禁强度** | P20 正向标记 + M3 计数 | 防「空绿」（M2 评审阻断项 3 的教训） |

---

## 4. 不变量 I21~I34（「谁保证 + 怎么验」）

> **I21~I30 的定义逐字取自 `M3-SSTable与刷盘.md` §1 的 `#1` 段**；
> **I31~I34 取自设计 §9.1（本设计新增）**。设计明写「`#1` 必须收录，编号可重排但**不得丢条目**」。
> 「谁保证」列指向设计章节号（`#4` 评审据此找到代码位置），「怎么验」列指向设计 §10 的用例编号。

| # | 不变量 | 谁保证（设计章节） | 怎么验（用例） |
|---|---|---|---|
| **I21** | SSTable 一旦被版本注册即**不可变**：注册后禁止任何原地修改，只能整体重写为新文件 | §6.3（写的是 `%06u.sst.tmp`，rename 后只读打开）；`Table`/`TableBuilder` **没有**写接口；`Table` 只持 `shared_ptr<RandomAccessFile>` | `M3-A22`（事件序列里无对已注册文件的写/rename）、`M3-B05`（多轮 flush 后已注册文件 mtime/size 不变） |
| **I22** | flush 顺序必须是 `durable → rename → (SyncDir) → 注册`：`write+fsync` 成功、`rename` 成功之后才允许写版本元数据；任何一步失败不得注册 | §6.3 步骤 ④⑤⑥⑦（顺序不可交换）+ §6.4 失败矩阵 | `M3-A22`（一条用例三条子断言）、`M3-B02`（三注入点进程级 kill -9） |
| **I23** | L0 内文件 key 范围允许重叠，故读路径必须按文件**新→旧**逐个检查，命中第一个可见版本即返回 | §7.1 顺序规则 2（按**文件号降序**，不用 key range 判新旧） | `M3-A30`、`M3-A29`；`M3-B08`（`files_checked` > 1） |
| **I24** | 块内 restart 点有序（offset 单调递增且指向块内合法边界），保证块内二分 Seek 正确 | §3.2 restart 语义 + §5.3 二分规则；写入侧 `BlockBuilder`，校验侧 `ValidateBlock` | `M3-A03`、`M3-A04`、`M3-A02` |
| **I25** | 索引项与数据块偏移自洽：每个索引项的块偏移/长度必须在文件边界内且与写入时一致 | §3.6（`handle.size` 是「读多少」的唯一真相源）+ §3.7 三条校验 | `M3-A16`、`M3-A08`、`M3-A17` |
| **I26** | tombstone 在底层不得被误丢：M3 不删任何旧版本，tombstone 必须能屏蔽更旧版本 | §7.2 八场景表；`TableBuilder::Add` 不做删除/合并/丢弃；flush 不做 compaction | `M3-A28`、`M3-A29`、`M3-A26`（落盘文件里 tombstone 条数 == 冻结时统计值） |
| **I27** | 恢复后的版本 = **最后一次成功注册**的状态：半写/未注册文件不得进入版本 | §8.3 步骤 ⑤⑥（版本只来自 `META`）+ §10.9 安全阀 | `M3-A40`、`M3-A41`、`M3-B02` |
| **I28** | 迭代器不得返回不可见版本（sequence 过滤）：只输出 `<= 读快照 sequence` 的版本 | §7.3 `DBIter` 的 `ScanForwardToVisible`/`ScanBackwardToVisible` + D7 快照来源 | `M3-A32` |
| **I29** | 读路径不得把 tombstone 返回给用户：`DBIter` 跳过 tombstone，`Get` 遇 tombstone 返回 `NotFound` | §7.1 `Get` 步骤（`kDeleted ⇒ NotFound`，不再向下）+ §7.3 | `M3-A28`、`M3-A29`、`M3-A32` |
| **I30** | 文件句柄数量有上限且必须 RAII 释放：table cache 满时按 LRU 释放，任何路径不得泄漏句柄 | D8 的 `TableCache`（容量 `Options::max_open_files`）+ §7.4（淘汰的 close 在 `mutex_` 外）+ `shared_ptr` RAII | `M3-B05`（`/proc/self/fd` 计数不增长）、`M3-A24`（失败路径也不泄漏） |
| **I31**（设计新增） | **恢复水位的唯一口径**：`last_sequence_ = max(WAL 重放最大值, 各已注册文件的 max_sequence)`；**禁止**用「注册时刻的 `last_sequence_`」当已持久化水位 | §8.2（含三段证明与三条纪律）；`Version::MaxSequenceInFiles()` | `M3-A37`、`M3-A38`、`M3-A36` |
| **I32**（设计新增） | **`Sync()` 的水位边界**：`Sync()` 返回 `kOk` ⟹ 此前所有已 ack 写的字节已 `fsync` **到当前 log**；水位只按「当前 log **已追加**的边界」（`log_last_appended_seq_`）发布，**不得**按「已分配 sequence 的边界」 | §6.1 `log_last_appended_seq_` + §6.6.1 阶段 C + §0.5 缺陷分析 | `M3-A50`（**M2 缺口的回归**，见 §9.1） |
| **I33**（设计新增） | **轮转的原子性位置**：轮转在**分配 sequence 之前**完成；轮转失败 ⇒ 整批以非 `kOk` 拒绝、**不** Append、**不**推进 sequence、**不** Add | §6.6.1 阶段 A/A'/B（+ M2 阻断项 1 的「判不过整批拒绝」纪律） | `M3-A49` |
| **I34**（设计新增） | **WAL 回收判据**：`min_log_to_keep = min({pending 表的 log_number})`；只有编号**严格小于**它的 `.log` 可删；删除**必须**在 `META` 的 `rename + SyncDir` 之后 | §6.6.2（判据 + 五步证明 `RecomputeMinLogToKeep()`）+ §6.3 步骤 ⑦→⑨ | `M3-A45`、`M3-A47`、`M3-B09` |

**I34 的易错点（设计 §9.1 单独强调，本文件照录以确保不丢）**：恢复出的 `memtable_` 的 `log_number`
必须是**被重放 log 的最小编号**（§8.3 步骤 ⑩）。漏掉它会**直接丢数据**：恢复后若给它记
`log_number = log_number_`（当前 log），下一次 flush 注册后 `min_log_to_keep` 会跳过那些「只被这份
memtable 覆盖」的老 log 并删掉；此时若进程崩溃而这份 memtable 尚未 flush ⇒ 数据只剩被删掉的 log 一份
⇒ **永久丢失**。`M3-A46` 必须包含这个形态。

---

## 5. 锁纪律 L13~L21

> **L13~L18 逐字取自 `M3-SSTable与刷盘.md` §1 的 `#1` 段；L19~L21 取自设计 §9.2（本设计新增）。**

| # | 纪律 | 落地方式（设计章节） | 验证 |
|---|---|---|---|
| **L13** | 后台 flush 线程与前台写/读的交互：MemTable 满由**前台写者**触发冻结（内存操作、持 DB 锁）并唤醒后台线程；落盘（write/fsync/rename/SyncDir）在后台线程且**不持 DB 锁** | §6.2 阶段 A（纯内存）+ §6.3（整段落盘无锁）+ §6.5（`bg_cv_` 唤醒） | `M3-A23`（`DbMutexHeldOnThisThread` + SpyEnv 断言持锁期 IO 调用数 == 0）；`M3-A20` |
| **L14** | DB 互斥锁保护的内存状态清单写死并评审：`memtable_`、`immutables_`、`version_` 指针、`last_sequence_`、`log_number_`、`log_sealed_`、`next_file_number_`、状态机；**禁止**把 `Table`/`TableReader`/文件句柄纳入 DB 锁保护范围 | §6.1 状态表；`TableCache` 自持互斥量（§7.4） | 代码评审逐行核对 §6.1 表 + `M3-A23` |
| **L15** | 版本对象的引用计数与生命周期：版本用 `shared_ptr`；读/迭代器在操作期间持住版本引用；flush 注册时**构造新版本并原子替换**，禁止原地修改已发布版本 | §8.1（`Version` 不可变）+ §7.1（`Get` 锁内拷 `shared_ptr`）+ §6.3 步骤 ⑦ | `M3-A33`（迭代期间注册新版本，结果不变 + ASan 干净）；`M3-A22` |
| **L16** | 文件删除必须延迟：文件只有在**不被任何版本引用**且**无在读句柄**时才允许删除；禁止在注册新版本的同一临界区里 `unlink` 正在被读的文件 | §6.3 步骤 ⑨（删除在锁外、且**只**删 log）；M3 **不删**已注册 `.sst`（§1.2） | `M3-A47`；`M3-B05` |
| **L17** | `CURRENT` 指针读写的原子性 | **本设计不引入 `CURRENT`**（D4 选 D）。等价纪律落为：**`META` 更新一律 `写 .tmp → fsync → rename → SyncDir`，禁止原地覆写**（**加强版**：多了 rename 之后必须 `SyncDir`） | `M3-A47`；`M3-A40`/`M3-A41` |
| **L18** | 禁止持 DB 锁做 IO 的延续：flush 的 write/fsync/rename/SyncDir 与 `TableReader` 的块读取一律在 DB 锁外；后台线程持 DB 锁只允许覆盖内存状态变更 | §6.3（③④⑤⑥⑨ 全在锁外）+ §7.1 + §6.5（⑤ 在 `commit_mu_` 下做 `log_->Sync()` —— **这是 M2 的既有例外**，`commit_mu_` 不是 DB 互斥锁） | `M3-A23`；**M2 的 `Locks.ZeroIoWhileHoldingDbMutex`（A25）不得回退** |
| **L19**（设计新增） | 读句柄（`Get`/`NewIterator`）在 `mutex_` 内**只**做「取 `shared_ptr` 引用 + 取快照」；一切 IO 与遍历在锁外。**禁止**把 `MemTable*`/`Version*` 裸指针带出 `mutex_` | §7.1、§6.1（`memtable_` 改 `shared_ptr`）、§7.3 | `M3-A23`；`M3-A33`（ASan 下并发 flush 不 UAF） |
| **L20**（设计新增） | 只有**当前** flusher（持 `flusher_active_`）允许创建/安装/轮转 log；后台 flush 线程**只读** `log_number_`，**永不**写 WAL、**永不**取 `commit_mu_` | §6.6.1 + §6.1（后台线程只取 `mutex_`） | 代码评审核对「后台线程函数体内无 `commit_mu_`」；`M3-A49`；TSan 全量 |
| **L21**（设计新增） | `immutables_` 中 `MemTable` 的析构（Arena 释放）只允许在 (a) 已注册进版本 **且** (b) 无读句柄持引用之后发生，由 `shared_ptr` 保证；禁止在 `mutex_` 临界区内做 `reset()` 之外的释放动作 | §6.1 + §6.3 步骤 ⑦（只 `pop_front`）+ §7.3 | `M3-A33`；`M3-A25`（停等路径不放锁死锁） |

---

## 6. 风险与未知项

### 6.1 风险清单（风险 → 触发场景 → 检测 → 缓解），16 条 ≥ 指令要求的 10 条

| # | 风险 | 触发场景 | 检测手段 | 缓解 |
|---|---|---|---|---|
| R1 | **`MemEnv` 不建模目录项 ⇒ `SyncDir` 的掉电语义无法验证** | 「`rename` 已发生、`SyncDir` 之前掉电」在 `MemEnv` 里不存在该状态（`RenameFile` 原子且永久，`SimulateCrash` 不触碰 dirent） | 无法在 A 组检测（**这就是缺口本身**） | 诚实登记为**已知证据强度缺口**（设计 §12.5 第 1/5 条）：A 组只做**调用顺序断言**（`M3-A22`/`M3-A47`）；**禁止**任何文档把「A 组通过」写成「掉电安全已证明」 |
| R2 | **torn 文件被注册** | 写文件或 rename 中途崩溃后残片被版本引用 | `M3-A40`（孤儿不注册）、`M3-B02`（三注入点 kill -9） | `.tmp` 永不注册 + 注册只来自 `META`（I27）；`.sst.tmp`/未注册 `.sst` 一律当垃圾 |
| R3 | **索引与数据块不一致** | 索引偏移/长度与块实际写入不符 ⇒ Seek 读错块或越界 | `M3-A17`（`handle.size` ±1）、`M3-A16`（类型不符）、`M3-A08`（handle 越界） | `handle.size` 是「读多少」的唯一真相源 + §3.6 三条冗余自检（**不是两份判据**，M2 阻断项 1 教训） |
| R4 | **restart 语义写错** | 共享前缀长度或 restart 偏移错误 ⇒ 解码错位、Seek 偏差 | `M3-A02`（组间不共享前缀）、`M3-A03`（单调+边界）、`M3-A04` | §3.2 唯一解释 + `ValidateBlock`；restart=16 是实测膝点（§0.4(d)） |
| R5 | **偏移 32 位溢出** | 文件 > 4 GiB 时块偏移/长度被截断 | 评审 + 常量表（`handle` 用 8 B offset/size ⇒ **64 位**）；`M3-A11`（块数超阈值只 WARN） | `handle` 定宽 16 B（8+8），不用 32 位；`length` 是 4 B 但受 `handle.size` 自洽校验约束 |
| R6 | **文件描述符耗尽** | table cache 无上限或句柄未释放，长跑后 open 失败 | `M3-B05`（`/proc/self/fd` 计数不增长）、`M3-A24`（失败路径不泄漏） | 有界 LRU（`max_open_files`，默认 64）+ `shared_ptr` RAII（I30） |
| R7 | **后台 flush 与 M4 compaction 的竞态预留** | M3 的单后台线程若未写死「同一文件不可被并发改写」，M4 会踩坑 | 评审 + `M3-A22`（事件序列无对已注册文件的写） | I21（注册后不可变）+ L20（只有当前 flusher 能安装/轮转）；设计 §11 M3.2 明写「骨架 = M4 不用重写写路径」 |
| R8 | **WAL 删早了导致数据丢失** | SSTable 未 durable 或未注册就回收对应 WAL | `M3-A45`（判据本身）、`M3-A47`（删除晚于 `META` durable）、`M3-B09` | I34 单一真相源 + 删除永远在 `META` 的 `rename+SyncDir` 之后 + `recycle_log_files` 可关 |
| R9 | **tombstone 提前消失** | flush 或读路径误丢 tombstone ⇒ 旧值复活 | `M3-A26`（tombstone 条数守恒）、`M3-A28`、`M3-A29` | `TableBuilder::Add` 不做任何删除/合并；flush 不做 compaction（I26） |
| R10 | **大 value 跨块边界** | 单条 entry 超过块大小 | `M3-A10`（value = 3×`block_size`） | §3.2 明写「`block_size` 是**目标值**不是硬上限」，切块判据是「加上这一条之后是否超过目标」 |
| R11 | **恢复时把孤儿文件当有效数据** | 目录里存在未注册 SSTable 被读入 | `M3-A40`、`M3-A41` | 版本只来自 `META`；`META` 缺失且目录有 `.sst` ⇒ `kCorruption`（把静默丢数据变成显式拒绝） |
| R12 | **`META` 单快照在文件数很大时变胖** | F 个文件 ⇒ `≈12 + F×(28+2×key_len)` 字节，**每次 flush 重写一遍**；F=10 000 ⇒ ≈700 KB | 设计 §12.5 第 2 条要求 `F > 1000` 时**计数 + WARN 上报** | M3 有意接受（M4 换 MANIFEST 正好解决）；`#1` 要求登记该阈值 WARN（本文件 §8.4 的 `FlushStats` 计数） |
| R13 | **L0 无 compaction ⇒ 读放大线性增长** | F ≈ 59 时一次最老 key 的 `Get` ≈ 59 索引读 + 59 数据读 ≈ 1.33 MB（D8 **推算值**，非实测） | `M3-B08`（`ReadStats` 固定格式实测） | 设计选择（M4 解决）；**必须写清「M3 的读性能随写入量退化」，不得拿 M3 的数字回答 M5 的「读为什么慢」** |
| R14 | **写者停等的上界是估算** | D5 估 ≈21 ms（`kMaxImmutableMemTables=2`），**未实测** | `M3-A25` 验证**逻辑**（假时钟/屏障）；真实上界由 `M3-B09` 的 `stall_micros` 观测 | 登记为估算；不得写成实测值 |
| R15 | **`M3-B10`（ENOSPC / 只读目录）可能无法制造** | 需 root 挂 loop 设备 | 若不能 ⇒ **如实登记「未验证」**（沿用 M2 B06 的处置） | A 组的注入式覆盖承担判据（`M3-A24`） |
| R16 | **`max_sequence` 的「自证」风险** | `M3-A38` 用「全量扫一遍」做独立参照，但**两者共用同一个块解码器** ⇒ 若解码器在 `SequenceNumber` 提取上系统性写错，二者会同错 | 设计 §12.5 第 7 条**主动暴露** | 必须补「**手工拼字节参照**」（沿用 M1 的 `ManualInternalKey`：`tests/test_harness.h:128` 已存在该助手）：`M3-A38` 的子用例用手工构造的字节算期望 `max_sequence` |

### 6.2 无法在本机 / 该 VM 上验证的东西（**明确写出，不许含糊**）

| # | 无法验证的命题 | 为什么 | M3 的替代证据 / 诚实结论 |
|---|---|---|---|
| **U1** | **目录项（dirent）的掉电语义**，尤其「`rename` 完成后、`SyncDir` 之前掉电」与「`META` 与 `.sst` 的目录项落地顺序」 | `MemEnv` **只建模文件数据 + fsync 水位，不建模目录项**（§3.2 P7 的实测证据：`RenameFile` 直接搬 map、`SimulateCrash` 不碰 `dirs_`）；`kill -9` **不丢 page cache**（M2 §11.3 实测） | A 组只能断言**调用顺序**（`M3-A22`：`fsync` 早于 `rename`、`SyncDir` 早于 `META` 的写；`M3-A47`：删除晚于 `META` 的 `rename+SyncDir`）。**「顺序对」不等价于「掉电安全」**——必须在验收文字里分开写 |
| **U2** | **真掉电（断电/掉盘）下的 flush 一致性** | 同上：`kill -9` 系列只证明**进程级**一致性（`M3-B01`/`M3-B02`） | 设计 §10.2 的诚实性纪律已写明：`kill -9` 证明进程级，`MemEnv` 证明掉电语义，**两者结论必须分开写**；本阶段的实测（§2.6）也只证明用例能跑 |
| **U3** | **`perf` 级性能归因** | `perf_event_paranoid=4`（§2.3 实测）⇒ 火焰图不可用 | 只用自带计数的微基准（`ReadStats`/`FlushStats`/`fsbench`）；**不得**声称做过 profiler 归因 |
| **U4** | **全量 TSan 下的 M3 并发正确性** | 本阶段只跑 11 例冒烟（§2.6）；全量约 410 s 且探测时有并发构建占 CPU | **本阶段标记为「全量未验证」**；`#3` 必须在**无并发负载**时补跑全量 TSan（含 `M3-A33`/`A52`） |
| **U5** | **真实 ENOSPC / 只读文件系统** | 需 root 挂 loop（`M3-B10` 自认「可能无法制造」） | 若无法制造 ⇒ 登记「未验证」；由 A 组注入覆盖（`M3-A24`） |
| **U6** | **写者停等的真实尾延迟上界**（D5 的 ≈21 ms） | 假时钟只验证逻辑；真实值未被本阶段测量 | `M3-B09` 的 `stall_micros` 观测；**在此之前该数字只能标为估算** |
| **U7** | **跨机器 / 跨盘的任何性能承诺** | 单机单块虚拟盘（§2.4 实测 `sda` = `VMware Virtual S`） | `roadmap.md` §3.7「不吹」：M3 的数字只用于**本实现内部的相对判断** |

---

## 7. 文件清单

| 分类 | 文件 |
|---|---|
| **必须新增（实现）** | `src/sstable/{format.h, block_builder.{h,cpp}, block.{h,cpp}, footer.{h,cpp}, table_builder.{h,cpp}, table.{h,cpp}}`、`src/version_edit.{h,cpp}`、`src/version_set.{h,cpp}`（M3 简化版：`Version` + `TableCache`，**无** MANIFEST/编辑日志/版本图/CURRENT/层级）、`src/merging_iterator.{h,cpp}`、`src/db_iter.{h,cpp}` |
| **必须新增（测试）** | `tests/block_test.cpp`、`tests/sstable_test.cpp`、`tests/flush_test.cpp`、`tests/iterator_test.cpp`、`tests/recovery_m3_test.cpp`、`tests/crash_flush_test.cpp` |
| **必须新增（脚本）** | `scripts/lsm_flush_crash_test.sh`（固定行格式 `ROUND/ACKED/RECOVERED/MISSING` + `SST_FILES_TOTAL`/`LOGS_DELETED_TOTAL` + `[FLUSH_CRASH_OK]` 正向标记） |
| **必须新增（文档）** | `docs/m3-prerequisites.md`（本文件，`#1` 产物）、`docs/m3-tdd-red.log`（`#2` 的原始 RED 输出）、`docs/m3-evidence.md`、`docs/m3-review.md` |
| **必须改** | `src/db_impl.{h,cpp}`（读路径串联、flush 状态机、单后台线程、恢复接入 SSTable + 残余 WAL、`immutables_`/`version_`/`log_number_` 状态、WAL 轮转与回收、`FlushStats`/`ReadStats` 诊断）、`src/util/env.h` + `src/util/env_posix.cpp`（**`SyncDir`** + **`RandomAccessFile`/`NewRandomAccessFile`**）、`src/filename.{h,cpp}`（`TableFileName`/`TempFileName`/`ParseTableFileName`/`ParseTempFileName`/`MetaFileName`）、`src/common.h`（`Options` 增 `block_size`/`verify_checksums`/`max_open_files`/`recycle_log_files`/`flush_hook`；新增 `FlushHook`）、`CMakeLists.txt`（**新增独立目标 `lsm_sstable`**，只链 `util`；新增源文件与测试目标；**不用 GLOB**；保留 M1/M2 全部目标不变）、`.gitignore`（flush 崩溃脚本临时目录）、`docs/protocol.md`（**只追加** §10 SSTable 章节，patch 文本见设计 §4）、`tests/memenv.{h,cpp}`、`tests/faulty_env.{h,cpp}`、`tests/test_harness.h`（SSTable 生成器/块内容对比/句柄计数探针）、`scripts/lsm_gate.sh`（追加 M3 腿 + **正向标记断言**） |
| **可选扩展** | `src/wal.{h,cpp}`（回收判据/删除接口）；`src/util/coding.{h,cpp}`、`src/util/crc32c.{h,cpp}`（若需新原语） |
| **禁止改动** | M1 定稿的内部 key 编码与比较规则（`src/common.h`）、`src/skiplist.h`、`src/memtable.{h,cpp}` 的语义、**M1/M2 既有测试的断言**（**唯一例外见 §9.3**）、`docs/protocol.md` §1~§9 的任何一行、`~/raft-kv` 任何文件；**禁止**在 M3 引入 compaction/Bloom filter/块缓存/WriteBatch 公共 API 的文件或符号 |

**与开发指令文件清单的一处必要差异（已由设计 §13 Q5/Q6 批准）**：指令 §1「必须新增」列了
`src/version_edit.{h,cpp}` 与 `src/version_set.{h,cpp}`，而指令 §0「非目标」与 `roadmap.md` §2 又写
「M3 不得出现分层与 MANIFEST」。设计 **D4** 已显式裁决为「单快照 `META` + 原子 rename」，
`version_set` 里**没有** MANIFEST/`VersionEdit` 追加日志/版本图/`CURRENT`/层级。
⇒ 本文件沿用该裁决；`#4` 评审按「文件里有没有 `MANIFEST`/`VersionEdit` 追加/`CURRENT`/`level` 字样」核对。

---

## 8. 边界 case 全集、未定义行为清单与测试前置假设

### 8.1 边界 case 全集（输入/场景 → 期望 → 覆盖用例）

| 场景 | 期望 | 用例 |
|---|---|---|
| 空数据块 | payload **恰为 8 B**（`restart[0]=0` + `count=1`） | `M3-A01` |
| 共享前缀 0 / 1 / 整条相同 / 整条不同 | 四种边界都正确解码；第 17 条成为新 restart 点（`shared == 0`） | `M3-A02` |
| 相邻两组有公共前缀 | 第二组首条 `shared == 0`（**组间不共享前缀**） | `M3-A02` |
| restart 偏移非单调 / 指向非边界 | `kCorruption` + 精确偏移 | `M3-A03` |
| Seek 命中已有 key / 落在两块之间 / 越过最后一个块 / 空块 | 分别：命中、**落在后一块**、越过末尾（Invalid）、Invalid | `M3-A04`、`M3-A13` |
| `shared > 上一条 key 长度` / `shared+non_shared > 剩余` / `vlen > 剩余` / `restart_count == 0` | 全部 `kCorruption`，**ASan 无越界读报告** | `M3-A05` |
| footer：文件 < 44 B / magic 坏 / version=2 / `footer_crc` 坏 / handle 越界 / metaindex 与 index 顺序错 / index 未紧贴 footer | version=2 ⇒ **`kNotSupported`**；其余 ⇒ `kCorruption` | `M3-A08` |
| 表：0 条 / 1 条 / 恰好落在 `block_size` 边界 / 1 字节之差 | 四种都能打开并读回全部 key | `M3-A09` |
| 单条 value = 3×`block_size` | 完整读回；该块 `max_block > block_size`（证明目标值不是硬上限） | `M3-A10` |
| 数据块 payload / 块 `length` 字段 / `type` 字节 / 块尾 CRC 逐字节翻转 | **全部** `kCorruption`；**零**静默错值 | `M3-A12`、`M3-A13` |
| 索引块 payload / `index_handle` / `metaindex_handle` / `footer_crc` 各翻转 1 字节 | 全部 `kCorruption` | `M3-A14` |
| `verify_checksums=false` | ① 结构校验**仍生效**；② 只损坏 payload 的 CRC **不再**报错（读到被改坏的值）⇒ 开关**真的有作用** | `M3-A15` |
| 索引项 handle 指向另一**类型**的块 | `kCorruption`（类型不符） | `M3-A16` |
| `handle.size` ±1 | `kCorruption`，**不**越界读 | `M3-A17` |
| metaindex 为空表 / 人为塞一条未知 `name` | 空表（`n=1`，无 entry）；未知 name **只计数不报错** | `M3-A18` |
| key 落在文件 key range 之外 | `kNotFound` 且 `ReadStats.blocks_read == 0`、`key_range_skipped == 1` | `M3-A19` |
| 写 N 条（N ≫ 容量） | **全部** `kOk`、**无任何一次** `kFrozen`；`flushes_completed >= 1` | `M3-A20` |
| `write_buffer_size = 4 KiB`，单条 value = 64 KiB | `kOk`；可读回且逐字节相等；**无** `bg_error_`（防「粘性写只读」复活） | `M3-A21` |
| flush 各失败点（write/fsync/rename/SyncDir） | 后续 `Put` 返回**同一** `Status`；`Get` 仍读到内存值；`*.log` **未被删**；`flushes_failed == 1` | `M3-A24` |
| `immutables_.size() == 2` / 后台完成 / `bg_error_` 置位 / `closed_` | 分别：写者停等、被唤醒、停等立即解除、停等立即解除（**不得**死锁/丢唤醒） | `M3-A25` |
| 同 key 在 SSTable 与 MemTable 各一版 | 读到 **MemTable 版** | `M3-A27` |
| `Put` → flush → `Delete` | `kNotFound`（**不得复活**） | `M3-A28` |
| 三个文件同 key / 新文件是 tombstone | 读到序号最大的可见版本；新 tombstone 屏蔽旧值 | `M3-A29`、`M3-A30` |
| 快照外的版本 / tombstone / tombstone 之后 Seek | 不可见；被跳过；落在**下一条可见 key**；`Prev`/`SeekToLast` 与正向一致 | `M3-A32` |
| 空 child 集合 / 单 child | 分别：恒 Invalid（退化输入专测）、正常 | `M3-A31` |
| `META` 的 magic / version / tail CRC / 字段长度 任一损坏 | `kCorruption`，**不自动修复、不重建** | `M3-A39` |
| 只有 `.sst.tmp` / 未注册 `.sst` / 清理失败（注入） | 分别：不注册+被删+计数；不读入+被删+计数；`Open` 仍成功+`orphan_remove_failed==1`+该文件**仍不被读入** | `M3-A40` |
| 删 `META` 但保留 `.sst` / 删 `META` 且只有 `.log` | 前者 ⇒ `kCorruption`（信息含「目录非空」）；后者 ⇒ **正常打开并读到全部数据**（兼容 M2 老库） | `M3-A41` |
| `META` 的 `comparator_name` 与 `options.comparator->Name()` 不符 | `kInvalidArgument`（带两个名字） | `M3-A42` |
| 文件名 `000001/000002/000010` | 按**数值**升序重放（字符串序会是 1→10→2） | `M3-A43` |
| 非最高编号 log 的尾部残骸 / 中间损坏 | 均 `kCorruption`（**逐条沿用 M2 §5.3**）；只有最高编号允许尾部截断 | `M3-A44` |
| `pending = {immutable@1, memtable@2}`，`log_number_=3` | `min_log_to_keep == 1`（**不是** 3）；注册后逐步变 2、3 | `M3-A45` |
| 恢复后**立刻** flush + 注册 | log 1 与 log 2 **未被删**（恢复出的 memtable `log_number == 1`）；漏掉 §8.3 步骤 ⑩ 则本用例必须**红** | `M3-A46` |
| `recycle_log_files = false` | 一个 log 都不删；数据仍全部可读（证明开关双向有效） | `M3-A48` |
| 轮转失败（注入 `SyncDir`/建文件失败） | 整批同一非 `kOk`；`last_sequence_` **未推进**；`log_` **未换**；`memtable_` 内容不变 | `M3-A49` |
| 「已分配 sequence、`Append` 未完成」窗口里调 `Sync()` | `durable_seq_ <= log_last_appended_seq_`（**不得**发布到含未追加批的水位） | `M3-A50` |
| `block_size=0/256/2MiB`、`max_open_files=0`、`write_buffer_size=0`、`comparator=nullptr` | `kInvalidArgument`；**且**先触发一次 `kInvalidArgument` 的 `Put` 之后，普通 `Put` **必须** `kOk`（防粘性只读复活） | `M3-A53` |
| 截断尾部 + 跳过 record + 删 orphan + 删 obsolete log + 索引超阈值 | §8.4 的**每一个**计数都非零；`Close` 后重开仍可读 | `M3-A54` |
| 真实 ENOSPC / 只读目录 | 明确 `Status`、不破坏既有数据、不 panic（**若无法制造则登记未验证**） | `M3-B10` |

### 8.2 未定义行为清单（评审逐条禁止）

1. `reinterpret_cast` 到 `uint32_t*`/`uint64_t*` 读写块头/`length`/restart 数组/`handle`（必须逐字节拼装，小端）。
2. **解码前不做结构字段合法性检查就算 CRC**：`length`/`type`/`shared`/`non_shared`/`vlen`/restart 数组
   必须先校验，否则畸形值越界读（M1 评审阻断项 2 / M2 §9.3 的同源）。
3. `size_t` 下溢（`size() - kBlockHeaderSize`、`length - 4*(n+1)` 类运算必须在前置条件成立后才做）。
4. 用**任何**已解析出的（可能损坏的）长度去推进偏移或读取——`handle.size` 是「读多少」的唯一真相源。
5. 持 DB 互斥锁做 `write`/`fsync`/`rename`/`SyncDir`/块读（I17/L7/L18）。
6. 在 `rename` 之前把文件注册进版本，或在 `SyncDir` 之前认为 `META` 已持久（I22/L17）。
7. 原地覆写 `META`（必须 `.tmp → fsync → rename → SyncDir`）。
8. 把裸 `MemTable*`/`Version*` 带出 `mutex_`（L19）；读句柄在锁外使用时对象已被释放。
9. `immutables_` 中的 `MemTable` 在仍有读句柄时析构（L21 / UAF）。
10. 后台 flush 线程写 WAL 或取 `commit_mu_`（L20）。
11. 在注册新版本的同一临界区里 `unlink` 正在被读的文件（L16）。
12. `TableBuilder` 写入**非严格升序**的内部 key（§10.4 要求写入方保证；读取方不重排）。
13. `restart_offset` 不是严格单调递增、或 `restart_offset[0] != 0`、或指向非 entry 边界。
14. 块内 `entry` 的 `key` 不是 internal key（缺 trailer / `size() < kInternalKeyMinSize`）——
    **SSTable 侧与 MemTable 侧同源，同样来自不可信来源（磁盘）**。
15. 迭代器所有权的重复释放或泄漏（`NewIterator()` 返回值归调用方）。
16. 测试代码访问私有成员（验收只能通过公开行为与 `Stats` 诊断接口）。
17. `sstable` 层 include `version_*`/`db_impl`/`db_iter`/`wal.h`，或出现任何 POSIX 头（设计 §1.4 硬规则；
   `lsm_sstable` 独立目标会让它**链接失败**）。

### 8.3 测试前置假设（测试必须显式依赖的假设）

1. **WAL 为空时 `Get` 只查 MemTable 与 SSTable 即代表全量数据**——M3 成立（M2 的 §7.7 假设在 M3 升级为
   「MemTable → immutable → SSTable 全部查完」）。
2. **`MemEnv` 忠实模拟三件事**：追加语义、`fsync` 水位、崩溃 = 回滚到 `last_synced_offset`（+ 固定种子撕裂）。
   **它不模拟目录项**（§6.2 U1）⇒ A 组的 `SyncDir` 结论只能是**顺序断言**。
3. **`kill -9` 不丢 page cache** ⇒ B01/B02 只证明**进程级**一致性，**不构成**掉电安全证据。
4. A 组凡涉及随机（撕裂位置）必须**固定种子**并打进 `INFO`/`RecordProperty`。
5. **不得**用 `sleep` 赌调度（M2 的 A20 教训）；跨线程时序一律靠 `CommitHook`/`FlushHook` 的确定性屏障。
6. **时间一律用 `MemEnv` 的假时钟**（`NowMicros` 递增、`SleepForMicros` 不真睡）。
7. 三构建目录互不共享；TSan 运行需 `setarch $(uname -m) -R`（§2.6 实测有效）。
8. B 组用真实临时目录 + 独立进程；对账必须在恢复进程内完成（`Open` 会截断尾部）。
9. 崩溃脚本每轮必须用**独立临时目录**，且 100 轮后清理（§2.4 实测仅 12 G 可用）。

### 8.4 「不得静默」纪律的计数落点（M2 教训 3 的延续）

`FlushStats` / `RecoveryStats` / `ReadStats` 的三个计数集必须覆盖：截断尾部、跳过 record、
删 orphan、删 obsolete log、索引超阈值（`M3-A54` 逐项断言非零）、
以及 `F > 1000` 时的 `META` 变胖 WARN（R12）。

---

## 9. 测试套件缺陷登记表

> 范本：`m1-prerequisites.md` §9（M1 曾登记 5 条用例自身缺陷）。
> **本阶段未修改任何测试**；`grep`/`read` 全部锚定 rev `961343e` 的干净本机只读克隆。
> 编号规则：`D9.n`；每条给「现象 / 证据 / 处置」。
> 静态审计基线（本轮实测）：`tests/*.cpp` 共解析 **82** 个 `TEST` 块，
> **0 个零断言用例**、**0 个永真/空断言**（`EXPECT_TRUE(true)`/`EXPECT_EQ(x,x)`/`ASSERT_TRUE(1)` 全无命中）、
> **0 处裸 `NewIterator()` 未接管所有权**（31 处全部包在 `std::unique_ptr` 里，M1 §9.5 的泄漏未回归）。

### D9.1 **`DB.PutAfterFreezeIsNotPersisted` 与 M3 的契约变更**直接冲突（**必须在 `#2` 之前裁决**）

- **现象**：`tests/crash_test.cpp:456` 的 `TEST(DB, PutAfterFreezeIsNotPersisted)` 用默认 4 MiB 写缓冲
  连写 200 000 次，断言**必须**撞到容量上限并返回 `kFrozen`：
  - `crash_test.cpp:468`：`EXPECT_TRUE(last.IsFrozen())`
  - `crash_test.cpp:474`：`ASSERT_TRUE(last.IsFrozen()) << "必须真的撞到容量上限（否则本用例没意义）"`
  - `crash_test.cpp:485`：被拒的写**绝不能**进 WAL（`IsNotFound`）。
- **冲突点**：设计 **§1.2 边界 1** 明写「M3 起 `DBImpl` 写路径**不再把容量不足当拒绝**——改为
  『冻结旧表 + 换新表 + 本批照常写入』」，并且 §1.2 边界 2 断言「M2 评审阻断项 1（被拒写复活）的**整类
  bug 在 M3 结构上消失**」；`docs/m3-design.md` §13 **Q10** 把这条列为**需拍板项**（「M3 起
  `PersistentDBImpl::Put` **不再**返回 `kFrozen`」）。
- **后果（可推导，非猜测）**：M3 落地后该循环在 465~472 行**永远不会**因非 `kOk` 而 `break`，
  `last` 保持 `kOk` ⇒ **474 行的 `ASSERT_TRUE(last.IsFrozen())` 必然 FAIL**；
  且 485 行要保护的语义（「被拒的写不得进 WAL」）**变成空命题**（不存在被拒的写）。
- **处置（建议，待用户确认）**：**不是**「为了通过而删测试」，而是**按契约收窄重写**——
  保留它的真正价值（「不给用户留下『写被拒了、但重启后数据又出现』的错觉」与「重开必须成功」），
  改名为「容量不足不再拒绝写」的**新**用例（对应 `M3-A20`），并**单独成提交 + 附设计章节引用**。
  这与 M2 `m2-prerequisites.md` §9 处理 `DB.OpenRejectsNonEmptyName` 的先例同构：
  「禁止改动 M1/M2 既有断言」在该处已被**收窄为**「禁止改动与契约变化无关的断言」。
  **本阶段不改动它**（`#1` 只登记，`#0` 已将该变更列为 Q10，需用户明确批准收窄口径）。
- **注意**：`tests/memtable_test.cpp:748 MemTable.FreezeRejectsWriteWithFrozenStatus` 与
  `memtable_test.cpp:292/329/797` 的 `IsFrozen()` 断言**不受影响**——设计 §1.3.1 明写
  「`MemTable::Add` 的 `kFrozen` **一个字节都不改**」，变的只是 **DB 层**的消化方式。
  `tests/crash_test.cpp:237/250` 的并发写用例把 `IsFrozen()` 列为**可接受**错误之一，
  M3 后 `Put` 返回 `kOk`（不被计入 `unexpected`）⇒ **不受影响**。

### D9.2 **`SpyEnv`（持锁零 IO 探针）覆盖面窄于 `M3-A23` 的判据**

- **现象**：`tests/crash_test.cpp:615` 的 `class SpyEnv : public MemEnv` **只重写** `NewWritableFile`(617)
  与 `NewAppendableFile`(623)；`SpyFile`(594) **只拦** `Append`(599) 与 `Sync`(604)。
- **证据**：`M3-A23` 的判据（设计 §10.1）要求断言「flush 的 `write/fsync/**rename/SyncDir**` 与 `Get` 的
  **块读**，在持锁期间调用次数 == 0」。而 `Env::RenameFile`(`env.h:57`)、`GetFileSize`(55)、
  `GetChildren`(62)、`RemoveFile`(63)、`Truncate`(64)、`NewSequentialFile`(53) 与**尚不存在的**
  `SyncDir`/`NewRandomAccessFile` **全部绕过了这层 Spy**。失败信息也只覆盖 WAL（`crash_test.cpp:663`
  写的是「发生了 WAL 的 write/fsync」）。
- **后果**：若 `#2` 直接照抄 `M3-A23`，该用例会**通过但什么都没测**（M3 新增的 rename/SyncDir/块读
  即便被错误地放进锁内也**不会**让 `violations` 递增）⇒ 属于 M2 评审阻断项 3「空绿」的同类风险。
- **处置**：`#2` 必须先**加宽** `SpyEnv`（覆盖 `RenameFile`/`GetFileSize`/`GetChildren`/`RemoveFile`/
  `Truncate`/`SyncDir`/`RandomAccessFile::Read`），再写 `M3-A23`；并保留一个「故意在锁内做一次
  rename ⇒ `violations` 必须递增」的**反向自检**，证明探针本身有效。
  **本阶段不改动任何测试**。

### D9.3 **既有 3 条 `durable_seq` 用例在结构上无法检出 I32 缺陷（M2 门禁的强度被高估）**

- **现象**：M2 的 `Sync()` 把 `durable_seq_` 发布到 `last_sequence_`，而 `last_sequence_` 在
  **`Append` 之前**（锁内取批时）就已推进 ⇒ 存在「某批已分配 sequence、`Append` 未完成」的窗口里，
  并发 `Sync()` 会 fsync 一份**不含该批**的日志却把水位发布到**包含该批**（设计 §0.5 逐行分析）。
- **证据（三条用例全部是单线程，窗口不可能出现）**：
  - `tests/crash_test.cpp:159 TEST(Sync, SyncCoversAllPriorWrites)`：**同一线程**先 `Put`×100 再 `Sync()`；
    写者只有在**自己的批被结算后**才返回，因此 `Sync()` 被调用时全部字节**都已 `Append`** ⇒
    水位边界与已追加边界重合，缺陷窗口为空。
  - `tests/crash_test.cpp:493 TEST(GroupCommit, DurableSeqOnlyAdvancesOnSync)`：8 次 `sync=false` +
    1 次 `sync=true`，**全为串行调用**，断言 `durable_seq()==0` 与 `>=9`。
  - `tests/crash_test.cpp:284 TEST(GroupCommit, WindowNotOpenedEarly)`：20 次 `sync=true` 串行 `Put`，
    断言的是「水位在 `OnAfterSyncBeforePublish` **之后**才发布」（**方向相反**的另一件事）。
- **后果**：M2 的「A29/A24 覆盖了 `Sync()` 的持久性」这句话**读起来比实际强**。
  82/82 全绿**不能**被当成「I32 不存在」的证据。
- **处置**：`M3-A50` 必须用 `CommitHook::OnGroupTaken`（把「批已取、`Append` 未执行」这一窗口
  **确定化**）+ 可暂停的 `Env`，断开「已分配」与「已追加」两个边界后再断言
  `durable_seq_ <= log_last_appended_seq_`。**RED 的原始输出属 `#2` 阶段义务**
  （本阶段不做实现、不改测试，故不在此宣称该用例会红）。
  **旁证（非本阶段产出）**：本阶段观测到并发 I32 补丁新增了
  `TEST(GroupCommit, SyncDoesNotClaimInFlightBatch)` + `BlockingTakeHook`（用 `OnGroupTaken` 卡住），
  其形态与上述建议一致（§1.3.1）。

### D9.4 **既有测试直接依赖具体实现类 `PersistentDBImpl`（11 处），构成 M3 状态改造的隐式回归面**

- **现象**：`tests/crash_test.cpp` 与 `tests/recovery_test.cpp` 通过 `#include "db_impl.h"` +
  `static_cast<PersistentDBImpl*>(db)` 调用诊断接口（`durable_seq()`/`pending_writers()`/
  `GetRecoveryStats()`）。
- **证据**：`grep -c PersistentDBImpl tests/crash_test.cpp` ⇒ **11**；
  `tests/crash_test.cpp:19` 的注释自述「durable_seq() 诊断接口在具体实现上（不进 DB 公共接口）」；
  `tests/recovery_test.cpp:12`、`355`、`377`、`389` 用 `GetRecoveryStats()`。
- **性质**：**不是** UB 清单第 16 条「访问私有成员」的违反（这些都是 impl 的**公开**诊断方法），
  但它使 M3 的 `memtable_` → `shared_ptr`、`immutables_`、`version_` 等内部改造**有可能**在编译期或
  语义上牵动既有用例。设计 §8.4 已声明 `RecoveryStats`「只增不改」。
- **处置**：登记为**观察项**。`#3` 必须保证 `RecoveryStats` 的既有字段**不改名、不重排、不删除**，
  且 `recovery_test.cpp:356~361` 断言的 6 个字段语义不变（`log_files`/`records_replayed`/
  `entries_replayed`/`records_skipped`/`tail_truncated_bytes`/`last_sequence`）。
  M3 新增 `FlushStats`/`ReadStats` 沿用同一「只读诊断」纪律（与 `GetRecoveryStats` 同款）。
  **本阶段不改动任何测试**。

### D9.5 **`MemEnv` 的「崩溃」语义被测试前置假设依赖，但它对目录项是不建模的（诚实登记）**

- **现象**：`tests/memenv.h` 文件头注释（1~12 行）自述崩溃模型是「回滚到 `synced_size` +
  固定种子撕裂未 fsync 后缀」，这条模型**只针对文件数据**。
- **证据**：`tests/memenv.h:78-82` 的 `struct File { std::string data; uint64_t synced_size; bool exists; }`
  没有 dirent 概念；`tests/memenv.cpp` 的 `RenameFile` 一次赋值即永久生效，`SimulateCrash()` 的循环
  只操作 `files_`、**从不触碰** `dirs_`(`memenv.h:87`)。
- **后果**：M3 的核心持久化纪律（`rename` + `SyncDir`）在 A 组**只能**被验证为「调用顺序正确」，
  **不能**被验证为「掉电后会怎样」。若 `#2`/`#3` 的文档把 `M3-A22`/`M3-A47` 的绿色说成
  「`SyncDir` 的持久性已证明」，那是**超出证据的结论**。
- **处置**：写进验收口径（§6.2 U1）与本表；`#2` 新增用例时必须在用例名/注释里写明
  「本用例只断言调用顺序」。**本阶段不改动任何测试**。

### D9.6 **正向标记缺口：现有门禁无法区分「通过」与「什么都没做」**

- **现象**：`scripts/lsm_gate.sh` 的 `run_gate()` **只看退出码**（`if "$@" >>"$LOG" 2>&1; then`），
  脚本内**没有任何正向标记断言**；`scripts/lsm_crash_test.sh` 末尾只打印
  `TOTAL_ROUNDS/ROUNDS_OK/ACKED_TOTAL/MISSING_TOTAL/MISMATCH_TOTAL`（第 83 行）与
  `[OK] 全部 N 轮 missing 0 / mismatch 0`（第 96 行）。
- **证据**：设计 §8.6 的 G3/G4/G7 明确要求新增 `SST_FILES_TOTAL`、`RECORDS_REPLAYED_TOTAL`、
  `LOGS_DELETED_TOTAL` 与 `[FLUSH_CRASH_OK]` 标记，理由是「M2 只查退出码，『退出 0 但什么都没做』仍可能过」。
- **后果**：若把 M3 的 flush 腿直接挂到现有 `lsm_gate.sh` 上，**会出现「一次 flush 都没发生」的空绿**
  ——M3 的门禁就退化成 M2 门禁的复制品。
- **处置**：**必须新增** `scripts/lsm_flush_crash_test.sh`（不在 M3 的「禁止改动」之列，属新文件），
  并在 `lsm_gate.sh` 中**断言** `SST_FILES_TOTAL > 0` ∧ `LOGS_DELETED_TOTAL > 0` ∧ `[FLUSH_CRASH_OK]` 出现
  （设计 §10.2 `M3-B01`/`M3-B06`）。`#2` 写脚本时必须把这三条作为**硬断言**而不是日志。

### D9.7 静态审计的**阴性**结论（同样入档，避免「只报坏消息」）

- `tests/*.cpp` 共 **82** 个 `TEST` 块：**零**零断言用例、**零**永真断言、**零** `EXPECT_EQ(x,x)`。
- **31** 处 `NewIterator()` **全部**由 `std::unique_ptr` 接管（M1 §9.5 的泄漏缺陷未回归）。
- `ExpectStrictTotalOrder`（`tests/util_test.cpp`，被 `InternalKey`/`MemTableKeyComparator` 两处复用）
  **完整**校验自反性、反对称性与传递性，**不是**「只查非降序」的弱化版；
  M3 的块内排序与索引二分可放心复用同一比较器（设计 §1.3.1 的口径成立）。
- Test 代码中**没有**发现比较器误用（内部 key 一律经 `InternalKeyComparator`；`BytewiseComparator()`
  只用于 `user_comparator` 或跳表的 user-key 序）。

---

## 10. 验收命令清单（每一步必须附原始输出）

```bash
# 1) 干净重建 + 0 warning + 全量用例（M1/M2 的 82 例不得减少）
bash scripts/lsm_build.sh

# 2) M3.1 的机制化依赖纪律（越权 include 会在此链接失败）
cmake --build build --target lsm_sstable
nm -C build/liblsm_sstable.a | grep -c 'version\|db_impl\|wal'      # 期望 0

# 3) ASan（独立目录）
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-asan -j8 && ./build-asan/bin/lsm_tests

# 4) TSan（独立目录；必须 setarch —— 本阶段实测有效）
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-tsan -j8 && setarch $(uname -m) -R ./build-tsan/bin/lsm_tests

# 5) M3 崩溃对账门禁（唯一能宣称「SSTable 真的落盘 + WAL 真的回收」的模式）
bash scripts/lsm_flush_crash_test.sh --rounds 100 --write-buffer-size 262144
#    期望：ROUNDS_OK == ROUNDS ∧ MISSING_TOTAL 0 ∧ MISMATCH_TOTAL 0
#          ∧ SST_FILES_TOTAL > 0 ∧ LOGS_DELETED_TOTAL > 0 ∧ 末尾 [FLUSH_CRASH_OK]，退出码 0

# 6) M2 腿共存（M2 的 4 条腿必须仍 PASS）
bash scripts/lsm_crash_test.sh --rounds 100 --mode sync
bash scripts/lsm_tail_truncate_test.sh
bash scripts/lsm_corrupt_middle_test.sh

# 7) 全部门禁一条命令 + 正向标记断言
bash scripts/lsm_gate.sh --rounds 100 --with-tsan

# 8) 单点判据（按需）
./build/bin/lsm_tests --gtest_filter='Block.*:Footer.*:Table.*'
./build/bin/lsm_tests --gtest_filter='Flush.NoIoWhileHoldingDbMutex'    # L18 探针
./build/bin/lsm_tests --gtest_filter='Sync.BoundaryDoesNotOvershoot'    # I32 回归
```

**未跑不算过**：任何「通过」结论必须粘贴上述命令的**原始输出摘录**
（构建日志尾部 + `[  PASSED  ] N tests.` + `ROUND …MISSING 0` + 正向标记行 + sanitizer 无报告行）。
本文件 §2 的数字即按此口径给出，并已标注哪些**没跑**（§2.6 全量 TSan、§2.8 的负载条件）。

---

## 11. 待用户 / 评审确认项

### 11.1 设计文档中标 ★ 的决策（**用户已批准**：`docs/m3-design.md` 已冻结于 `961343e`）

| # | 决策 | 设计推荐 | 状态 | 若否决的代价 |
|---|---|---|---|---|
| **Q1 ★** | **D4 版本元数据**：单快照 `META` + 原子 rename（**无** MANIFEST / `VersionEdit` 追加日志 / `VersionSet` 版本图 / `CURRENT` / 层级）vs LevelDB 式 MANIFEST | **D（单快照 META）** | **已批准** | 改 A ⇒ §3.1 命名、§8.1 全部、§8.3 的 ⑤⑥、§9.2 的 L17、`M3-A37~A42` 全部重写；且与 `roadmap.md` §2「M3 不得出现 MANIFEST」**直接冲突** |
| **Q5 ★** | **D6 WAL 回收**：M3 就**真删**已完全落盘的 `.log`（默认开、可关）vs 只记水位不删 | **真删** | **已批准** | 只记水位 ⇒ G7 落空、「纯 SSTable 启动」只能靠测试手工删文件（弱化证据）、100 万写场景磁盘单调增长 |
| **Q6 ★** | **§8.7 E4 是否新增公共 `DB::Flush()`** | **不新增**（用小子缓冲驱动 flush，`M3-B03` 覆盖） | **已批准** | 新增 ⇒ 要改 `src/db.h`（**不在** M3 的「必须改」清单里）⇒ 超出授权的契约变更 |
| **Q8 ★** | **§0.5 的 M2 `Sync()` 水位越界（I32）**：① 单开 M2 补丁；② 在 M3 里顺手修；③ 登记为已知限制不修 | **② 在 M3 里修**（§6.6.1 阶段 C 正好要加 `log_last_appended_seq_`），`M3-A50` 回归 | **已批准**；**实测该补丁正被并发应用**（§1.3.1） | 选 ③ ⇒ `db.h` 对 `Sync()` 的契约长期不成立，且 M3 的 WAL 回收判据若误用它**更危险** |

### 11.2 设计文档中标「是 / 需拍板」但非 ★ 的项（**已批准**）

| # | 决策 | 设计推荐 | 状态 |
|---|---|---|---|
| Q2 | **D1 块格式**：4 KiB + 前缀压缩 restart 16 + 单层索引（否决「不压缩」「整文件单块」「两级索引」） | **A** | 已批准（实测依据：索引开销 0.95%、前缀压缩省 13.2%、restart 16 是膝点） |
| Q3 | **D2 为 M5 预留 metaindex**（M3 写空块） | **预留** | 已批准（指令已推荐） |
| Q4 | **D3 校验**：每块 CRC32C（**含长度字段**）+ footer CRC + 读时默认开可关 | **是** | 已批准（指令已推荐「含块头」） |
| Q7 | ~~§0.1 的脏工作区如何处置~~ | **已闭环**（WIP 已回退；本阶段实测开工时干净） | 已闭环（**但探测中途再次被动变脏，见 §1.3.1**） |
| Q9 | **M2 基线 rev**：HEAD `3603696`（含 A22/A25 + `GetRecoveryStats`）vs tag `m2-wal`（`8189607`，落后两个提交） | **HEAD `3603696`** | 已批准；**M3 的实现基线因此是 `961343e`**（§1.3） |
| Q10 | **§1.2 边界 1**：M3 起 `PersistentDBImpl::Put` **不再**返回 `kFrozen`（容量不足改为冻结 + 停等） | **按设计接受** | 已批准 —— **但它会使 `tests/crash_test.cpp:456` 的既有断言失败，见 §9.1（本阶段新增的必须裁决项）** |

### 11.3 设计 §8.7 的补充决策 E1~E8（**已批准**）

E1 冻结不再调 `MemTable::Freeze()`（持久模式；内存模式保持调用）／E2 `smallest`/`largest` 用 internal key／
E3 `META` 必存 `max_sequence`（与 `largest` 的 sequence 不同，**用不同名字写死**）／
E4 不新增 `DB::Flush()`／E5 `write_buffer_size` 语义收窄为「**目标值**」非硬上限／
E6 `TableCache` 放进 `version_set.{h,cpp}`（`#1` 可改）／E7 `MergingIterator` 的 `Prev` 也用 O(N) 线性扫／
E8 `FlushHook` 三注入点（`OnSSTableWritten`/`OnBeforeRename`/`OnBeforeRegister`），生产恒为 `nullptr`。

### 11.4 与开发指令的偏离（**逐条列出，均已批准**）

| # | 偏离 | 性质 | 依据 |
|---|---|---|---|
| **V1** | **D4 单快照 `META`（无 MANIFEST）** | 指令**内部自相矛盾**（§0 非目标 + `roadmap.md` §2 说「不得出现 MANIFEST」，§1 的「必须新增」却列 `version_edit`/`version_set`）；设计 D4 显式裁决 | Q1 ★ 已批准 |
| **V2** | **D6 WAL 真删**（默认开、可关、专属用例） | 指令未禁止；M2 设计 §3.3 明写「M3+ 可删」。**这是 M3 唯一带数据丢失能力的新路径** | Q5 ★ 已批准 |
| **V3** | **不加 `DB::Flush()`**（用小子缓冲驱动 flush） | `src/db.h` **不在**指令的「必须改」清单里 | Q6 ★ 已批准 |
| **V4** | **`Env::SyncDir` 必交付** | M2 设计 §5.7 **声明过、实现从未落地**（`m2-review.md` §3 残留清单第 4 项）。没有它，M3 的「rename 后注册」**不构成**持久性保证 | 设计 §1.3.2；本文件 §3.1 P1 实测「零命中」 |
| **V5** | **I32 在 M3 内修** | 设计 §0.5 逐行证明 M2 的 `Sync()` 水位越界；M3 本来就要重写轮转逻辑 | Q8 ★ 已批准 |
| **V6** | **新增 `Env::RandomAccessFile`/`NewRandomAccessFile`** | M1 设计明写「留到 M3」（`env.h:4` 注释实测） | 设计 §1.3.2 |
| **V7** | **D5 引入单后台 flush 线程**（而非同步 flush） | 指令**建议**；否决同步 flush 的理由是 M4 要重写整条写路径 + 尾延迟与 `write_buffer_size` 成正比 | 设计 D5 已批准 |
| **V8** | **新增 I31~I34 与 L19~L21**（超出指令要求的 I21~I30 / L13~L18） | 指令说「编号可重排但不得丢条目」；设计给的是**追加** | 设计 §9.1/§9.2；本文件 §4/§5 已全收 |
| **V9** | **新增 `FlushHook`（指令未列）** | `CommitHook` 的 3 个注入点**全在 WAL 路径**，flush 顺序判据够不着（本文件 §3.2 P6 实测）；`M3-B02` 的三个 kill -9 点需要它 | 设计 §8.7 E8 |
| **V10** | **新增独立 CMake 目标 `lsm_sstable`** | 把「sstable 层不得依赖 db/version」从人工纪律升级为**链接期机制保证** | 设计 §1.4/§11 |
| **V11** | **`protocol.md` 追加 §10**（`#0` 明确「本阶段不落地」） | 硬性约束 4 要求编码实现与文档一致；`#0` 只给 patch 文本 | 设计 §4；本文件 §3.3 P18 实测「无 §10」 |
| **V12** | **`write_buffer_size` 语义收窄为「目标值」**（并收窄 M1/M2 文档里任何「上限」措辞） | 否则「单条大于写缓冲」会把库打成**粘性只读**（M2 评审优化项 2 的同类风险） | 设计 E5 + §1.2 边界 2 |

### 11.5 **本 `#1` 阶段新提出的、仍需用户裁决的 3 项**

| # | 事项 | 本阶段的建议 | 为什么需要裁决 |
|---|---|---|---|
| **N1** | `tests/crash_test.cpp:456 DB.PutAfterFreezeIsNotPersisted` 在 M3 后**必然 FAIL**（§9.1） | **按契约收窄重写**（保留「重开必须成功」与「不留被拒写的复活错觉」，改名为对应 `M3-A20` 的新用例），单独成提交 + 引用设计 §1.2 边界 1 与 Q10；**不删测试** | 它触发「禁止改动 M1/M2 既有断言」的例外，必须由用户批准口径（同 M2 §9 对 `DB.OpenRejectsNonEmptyName` 的先例） |
| **N2** | `SpyEnv` 必须**加宽**后 `M3-A23` 才有意义（§9.2） | `#2` 先加宽探针（覆盖 rename/`GetFileSize`/`GetChildren`/`RemoveFile`/`Truncate`/`SyncDir`/随机读），并加「故意在锁内 rename ⇒ 必须报警」的反向自检 | 否则 `M3-A23` 会**通过但什么都没测**（M2 阻断项 3「空绿」的同类风险） |
| **N3** | `MemEnv` **不建模目录项** ⇒ `SyncDir` 的掉电语义在 A 组**不可验证**（§6.2 U1 / §9.5） | 接受该限制，但**强制**：所有涉及 `SyncDir` 的用例与验收文字必须写「只断言调用顺序」，**不得**写成「掉电安全已证明」；若用户要求更强证据，则需回退 `#0` 扩 `MemEnv` 的目录项模型（会改变 A 组设计面） | 这是设计 §12.5 自认的**最明显证据强度缺口**，必须在验收口径上锁死，否则 M3 的结论会被高估 |

---

## 12. 接续点（`#2` 开工前先做的事）

1. **重新确认基线**：`cd ~/lsm-kv && git status --porcelain && git rev-parse HEAD`。
   若 §1.3.1 的 I32 补丁已落成 commit，**M3 的实现基线重钉到该新 rev**（并确认 `m2-wal` 是否已前移）。
   工作区若不干净，先查清归属（可能是并发进程），**不要自行回退**。
2. **读本文件 §9 的 6 条测试套件缺陷**，特别是 **D9.1（必须裁决）**、**D9.2（必须先加宽探针）**、
   **D9.3（`M3-A50` 的 RED 是 `#2` 义务）**。
3. **先建测试 seam，再写用例**（§3.4 的「必须先做」清单）：
   `Env::SyncDir` + `NewRandomAccessFile` → `MemEnv`/`FaultyEnv` 补齐（含句柄计数与事件日志）→
   `FlushHook` → `SpyEnv` 加宽 → SSTable 生成器/块内容对比助手。
4. **M3.1 的第一个交付物是 `docs/protocol.md` §10**（照抄设计 §4 的 patch 文本，逐字；
   落地后 `diff` 核对与设计 §3 一致）。
5. `#2` 必须**实测 RED 并粘贴原始失败输出**（`docs/m3-tdd-red.log`）——**未跑不算过**。

---

## 13. 参考与引用

| 出处 | 本文件用它的地方 |
|---|---|
| `docs/m3-design.md`（2188 行，rev `961343e`） | §1~§5 的目标/边界/不变量/锁纪律/前置条件；§4 的 `protocol.md` §10 patch 文本；§9 的测试矩阵 A01~A54/B01~B10；§12.5 的已知薄弱点；§13 的 ★ 决策清单 |
| `docs/protocol.md`（204 行，§1~§9） | §3.3 P18 实测「无 §10」；§8.2 UB 清单的编码纪律来源 |
| `docs/m1-prerequisites.md`（191 行） | §9 缺陷登记表的**格式范本**；I1~I10/L1~L6；§6 UB 清单（M3 全部继续适用） |
| `docs/m2-prerequisites.md`（194 行） | §1~§11 的结构范本；I11~I20/L7~L12；§7 的测试前置假设；§9 的「既有断言随契约收窄」先例 |
| `docs/roadmap.md` | §0 单向分层、§2 阶段硬边界（「M3 不得出现分层与 MANIFEST」）、§3 工程约定（未跑不算过/不吹/负结果入档） |
| `M3-SSTable与刷盘.md` | `#1` 段的 I21~I30 / L13~L18 定义、风险清单下限（10 条）、存量代码修改点清单、A/B 组必含用例 |
| 本阶段实测（§2/§3/§9） | 全部环境数字、能力缺口的「零命中」证据、82 例基线与静态审计结论 |
| 一次性探针 `/tmp/m3probe/fsync_probe.cpp`（**throwaway，未进仓库**） | §2.8 的 `write+fsync` 实测 |
