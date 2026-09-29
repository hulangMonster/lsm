# lsm-kv Roadmap（M1~M6）

> 本文件是阶段划分、验收判据与工程约定的唯一入口。每阶段的具体设计见 `docs/mX-design.md`；
> 跨阶段冻结的编码契约见 `docs/protocol.md`。
>
> 项目定位：LevelDB 简化版 LSM-Tree 存储引擎，**C++17**，仅标准库 + pthread + GoogleTest（测试用），无第三方依赖。

## 0. 依赖方向（单向，禁止反向依赖）

```
src/common.h        Slice / Status / SequenceNumber / ValueType / Comparator / Options / 内部 key 编解码
      ^
src/util/           status.cpp / coding / crc32c / arena / env
      ^
src/skiplist.h      Slice 为 key 的跳表（Comparator 注入）
      ^
src/memtable.*      MemTable（内部 key 编码 + 冻结 + 内部序迭代器）
      ^
src/db.*            DB 对外接口 + M1 内存实现（含用户视图迭代器）
```

- 箭头方向表示「被依赖」：`util` 可以 include `common.h`，`common.h` 不得 include 任何项目头（只依赖标准库）。
- 说明：开发指令原文写「util → common → memtable → db」。但 `common.h` 里的 `Slice` 被 `util/coding` 使用，
  两条依赖同时成立会形成环。本仓库按**单向分层**落地并以此处为准：`common.h`（类型契约，最底层）
  → `util` → `memtable` → `db`。这是对原文那一行的澄清，不是对契约的修改。
- 禁止：`util` 不得 include `memtable.h`/`db.h`；`memtable` 不得 include `db.h`。

## 1. 阶段总览

| 阶段 | 设计文档 | 目标 | 主要通过判据 | tag |
|---|---|---|---|---|
| M1 | `docs/m1-design.md` | 工程骨架 + KV 接口（Slice/Status/Options/DB）+ 跳表 + MemTable + 迭代器 | 10 万条与 `std::map` 全量对账；ASan/TSan 干净；干净重建 0 warning | `m1-memtable` |
| M2 | `docs/m2-design.md` | 顺序写 WAL、record 格式与 CRC、`DB::Open` 恢复、`sync` 语义、组提交 | `kill -9` × 100 轮 `missing 0`；torn record 安全截断；fsync 成本微基准 | `m2-wal` |
| M3 | `docs/m3-design.md` | SSTable 格式（数据块/索引块/footer）、flush 路径、读路径串联、MergingIterator/DBIter | 落盘后重启仅靠 SSTable 读到全部数据；覆盖语义与 tombstone 正确；CRC 检出损坏 | `m3-sstable` |
| M4 | `docs/m4-design.md` | Version/MANIFEST、L0 触发、分层合并、快照一致性、三个放大统计 | 层级约束达成；并发压测 + `kill -9` 后 `missing 0`；两种策略对照（负结果也入档） | `m4-compaction` |
| M5 | `docs/m5-design.md` | Bloom Filter、WriteBatch、四类负载微基准与数据表、门禁脚本 | Bloom 假阴性专项测试；批原子性 `kill -9` 用例；`bench_lsm.sh` 退出码门禁实测有效 | `m5-optimize` |
| M6 | `docs/m6-design.md` | 用 lsm-kv 实现 raft-kv 的 LogStore/状态机后端 | raft-kv 既有门禁不降级；接口等价性（尤其 `truncateSuffix`）；A/B 同轮交替 + `missing 0`；一键回退可用 | `m6-raft-integration` |

## 2. 阶段间的硬边界（禁止提前实现）

- M1：**不得**出现 WAL / SSTable / flush / compaction / Bloom / WriteBatch / 快照读视图 / 组提交的任何符号或占位实现（stub 也不行）。
- M2：不得出现 SSTable / compaction；M3：不得出现分层与 MANIFEST；M4：不得出现 Bloom/Batch；M5：不得改 Raft 语义。
- M6 对 raft-kv **只新增适配层**，不改 Raft 语义与既有测试断言。
- 每阶段的「非目标」段是硬边界，评审按此对照检查。

## 3. 工程约定（贯穿全部阶段）

1. **开发与提交只在虚拟机**：`~/lsm-kv`（M6 另加 `~/raft-kv`）。GitHub `origin` 为唯一远端，
   远端为 `git@github.com:hulangMonster/lsm.git`（开发指令原文写的 `lsm-kv.git` 在 GitHub 上不存在，
   以用户指定的 `lsm.git` 为准）。Windows 本地 `D:\JLProject\lsm-kv` 是**只读克隆**（只 pull，不 commit、不 push），
   不得用 tar/scp 回灌覆盖虚拟机工作区。
2. **提交粒度**：每个子里程碑一个提交（M1.1 / M1.2 / M1.3 …），提交信息说明动机并引用 `docs/mX-design.md` 的章节号。
   设计/校验阶段的文档单独成一个 `docs(mX):` 提交。
3. **tag**：每阶段完成（#4 评审的阻断项修完）后打 `mX-*` tag 并 push。
4. **未跑不算过**：任何「通过/完成」的结论必须附本轮实测命令与原始输出（构建日志、测试计数、sanitizer 结论），
   不得转述、不得只声称「应该通过」。
5. **三构建目录**：`build`（Release）/ `build-asan` / `build-tsan`，互不共享缓存；
   TSan 运行需 `setarch $(uname -m) -R`（本机 `perf_event_paranoid=4`，火焰图不可用）。
6. **0 warning**：`-Wall -Wextra`，由 `scripts/lsm_build.sh` 抓 `warning:` 计数并断言为 0。
7. **不吹**：不写未实测的规模与倍速；单机数字只用于实现内部的相对结论。
8. **负结果入档**：做失败的方案也要把数据和结论写进文档（区分「结论作废」与「原文保留」）。

## 4. M6：与 raft-kv 的对接计划（预留）

- raft-kv 仓库（`~/raft-kv`，只读参照）现有 `LogStore` 抽象、`file_log_store`、`verify missing 0` 口径与
  `scripts/` 门禁风格可直接复用；M6 的目标是提供 `lsm_log_store`（或等价适配层）+ 状态机后端。
- 对接的接口等价性检查点（尤其 `truncateSuffix`）、A/B 同轮交替口径与一键回退开关，在 `docs/m6-design.md` 里展开。
- **红线**：不修改 raft-kv 的 Raft 语义与既有测试断言；不复制 raft-kv 业务代码进 lsm-kv。

## 5. 面试四问（M5 完成后自测）

1. 为什么 LSM 写快读慢，什么场景不适合？
2. compaction 为什么分层，写/读/空间放大三者的换算关系（要用本项目实测的三个数字回答，不能背定义）。
3. WAL 为什么要 fsync，fsync 后崩溃如何恢复（用 M2 的 `kill -9` 数据回答）。
4. 跳表 vs 红黑树，MemTable 为什么选跳表（用 M1 的层高分布与对账数据回答）。
