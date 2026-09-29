# lsm-kv 编码协议（docs/protocol.md）

> 本文件是**位级编码契约**：M1 定稿后冻结，M2（WAL record）、M3（SSTable block/index/footer）必须逐字复用，
> 不得各写一套。任何变更须回到 `#0` 设计阶段修订本文件并说明影响面。
>
> 所有多字节整数落盘/落内存缓冲区一律**小端（little-endian）**，由函数逐字节拼装/解析，
> **禁止** `reinterpret_cast` 到 `uint32_t*`/`uint64_t*` 直接读写（会引入主机字节序依赖与严格别名 UB）。

## 1. 基本类型

| 类型 | 定义 | 说明 |
|---|---|---|
| `SequenceNumber` | `uint64_t` | 只使用低 56 位 |
| `kMaxSequenceNumber` | `(1ULL << 56) - 1` = `0x00FFFFFFFFFFFFFF` | 顺序号上界 |
| `ValueType` | `uint8_t` | `0x0 = kTypeDeletion`，`0x1 = kTypeValue` |
| `kValueTypeForSeek` | `kTypeValue`（`0x1`） | 构造 lookup key 时使用 |

## 2. Varint（LEB128）

| 函数 | 编码 | 最大字节数 | 合法范围 |
|---|---|---|---|
| `PutVarint32` / `GetVarint32` | 7 bit/byte，最高位=续位 | 5 | `0 .. 2^32-1` |
| `PutVarint64` / `GetVarint64` | 同上 | 10 | `0 .. 2^64-1` |

- `GetVarint32`（不做零扩展检查的变体另计）必须拒绝溢出：第 5 字节的高 4 位不全为 0 → 解析失败返回 `false`。
- 截断输入（剩余字节不足）→ 返回 `false`，**不修改**调用方已有输出。
- 边界向量：`0`、`127`、`128`、`16383`、`16384`、`2^31-1`、`2^32-1`、`2^63`、`2^64-1` 必须往返一致。

## 3. Fixed 编码

| 函数 | 布局 |
|---|---|
| `PutFixed32` / `DecodeFixed32` | 4 字节小端 |
| `PutFixed64` / `DecodeFixed64` | 8 字节小端 |

## 4. Length-Prefixed

| 函数 | 布局 |
|---|---|
| `PutLengthPrefixedSlice(dst, s)` | `varint32(s.size())` + `s` 字节 |
| `GetLengthPrefixedSlice(input, result)` | 读 varint32 长度 + 该长度字节；长度越界/截断 → `false` |

## 5. CRC32C

- 多项式 Castagnoli：正向 `0x1EDC6F41`，反射实现用 `0x82F63B78`。
- 初始值 `0xFFFFFFFF`，结果与 `0xFFFFFFFF` 异或（等价 `crc32c(0, data)`）。
- 提供 `Extend(init_crc, data)` 以支持增量计算。
- 已知向量（M1 测试断言）：
  - `crc32c("") = 0x00000000`
  - `crc32c("123456789") = 0xE3069283`
  - `crc32c("The quick brown fox jumps over the lazy dog") = 0x22620404`
- M2 起用于 WAL record 校验，M3 起用于 SSTable block/footer 校验；M1 只做纯函数实现与向量测试。

## 6. 内部 key（internal key）

```
internal_key := user_key (0..kMaxUserKeySize-1 字节) || trailer (8 字节)
trailer      := (sequence << 8) | type        // 64 位小端写入，物理上小端序的 8 字节
```

| 字段 | 字节数 | 字节序 | 取值范围 |
|---|---|---|---|
| `user_key` | `1 .. kMaxUserKeySize`（=64 KiB） | — | 非空；超过上限由上层拒绝（`kInvalidArgument`） |
| `trailer` | 8 | 小端 | 见下 |
| `sequence` | 逻辑 7 字节（trailer 的 bit 8..63） | 随 trailer 小端 | `0 .. kMaxSequenceNumber` |
| `type` | 逻辑 1 字节（trailer 的 bit 0..7） | 随 trailer 小端 | `0x0` 或 `0x1` |

- `kInternalKeyMinSize = 8`。**任何解码前必须校验 `internal_key.size() >= 8`**；畸形输入返回 `false`，不得越界读。
- `type` 不在 `{0x0, 0x1}` 内视为畸形（`ParseInternalKey` 返回 `false`）——为 M3 的损坏检测留出统一判定口。

### 6.1 比较规则（`InternalKeyComparator`）

1. 先按 **user key 升序**（字节序：逐字节无符号比较，短者为小）。
2. user key 相等时，按 **trailer 降序**（即 sequence 大的在前；sequence 相同则 `kTypeValue` 在 `kTypeDeletion` 前）。

```
Compare(a, b):
  c = user_comparator->Compare(ExtractUserKey(a), ExtractUserKey(b))
  if c != 0: return c
  ta = DecodeFixed64(a.data() + a.size() - 8); tb = DecodeFixed64(b.data() + b.size() - 8)
  return (ta > tb) ? -1 : (ta < tb ? +1 : 0)     // trailer 降序
```

**为什么**：同一 user key 的最新版本（最大 sequence）排在最先，`Seek + Next` 天然拿到「≤ 快照的最新版本」。

### 6.2 lookup key（读取用）

```
lookup_key := user_key || ((snapshot << 8) | kValueTypeForSeek) 的小端 8 字节
```

`lookup_key` 是「该 user key 下、序列号 ≤ snapshot 的最大可能内部 key」，因此
`Seek(lookup_key)` 命中第一条即该快照下可见的最新版本。`snapshot` 取 `kMaxSequenceNumber` 表示读最新。

## 7. MemTable 条目编码（M1 内存结构，同时是 M3 复用点）

MemTable 的跳表 key 是**整条编码后的条目**（`Slice` 视图，指向 Arena）：

```
entry        := varint32(internal_key_size) || internal_key || varint32(value_size) || value
internal_key := user_key || trailer(8B)
```

| 字段 | 编码 | 说明 |
|---|---|---|
| `internal_key_size` | varint32 | = `user_key.size() + 8` |
| `internal_key` | §6 | |
| `value_size` | varint32 | tombstone 时为 `0` |
| `value` | 原始字节 | 空 value 合法（`value_size = 0`） |

- **该编码不是字节序保持的**（变长长度前缀破坏排序），因此跳表必须注入 `MemTableKeyComparator`：
  1) 先解码两侧的 `internal_key`，**解码前必须校验 `internal_key_size >= kInternalKeyMinSize`**（§6 的 MUST）；
  2) 按 §6.1 比较 internal key，非 0 即返回；
  3) internal key 按比较器**等价**时，**只比较 value 段**（`varint32(vlen) | value` 中的 `value` 字节序），
     **不得**退化成整条 `entry` 的字节序 —— 否则在「等价 key 的字节形态可以不同」的比较器
     （如大小写不敏感）下，由 lookup key 构造的探针条目会被排到逻辑相等的真实条目**之后**，
     `Seek` 直接跳过它、`Get` 落空（#4 评审阻断项 1 的更深根因）。只比 value 还顺带保证
     「探针（空 value）≤ 同 internal key 的真实条目」，Seek 必能命中；
  4) 任一侧畸形（解码失败，含 3) 的校验失败）时退化为整条字节序，仅用于保证**内存安全与全序**，
     正常路径不经过。
  该比较器是**严格弱序**（跳表正确性的前置条件）；长度前缀自描述，配合 1) 的校验不会越界读。
- **修订（#4 评审回退 #0）**：本条原文只写"两者完全相等时，再按整条 `entry` 的字节序比较"，
  已按上述 3) 修正，并把 1) 的长度校验写成硬约束。
- 复用理由：`varint` 与 length-prefix 在 M1 就真实使用（不是为了占位），M3 的 block entry 沿用同一套。

## 8. 边界与拒绝口径（M1）

| 输入 | 行为 |
|---|---|
| 空 user key | `kInvalidArgument` |
| user key 长度 > `kMaxUserKeySize`（64 KiB） | `kInvalidArgument` |
| 空 value | 合法 |
| value 大小 | M1 不单独设上限，仅受 `write_buffer_size` 与内存约束；1 MiB value 必须可用 |
| `internal_key.size() < 8` | 解析失败（`false`），不越界读 |
| `sequence > kMaxSequenceNumber` | 打包前由调用方保证；测试须覆盖上界往返 |

---

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

## 11. MANIFEST / VersionEdit 编码（M4 定稿）

> 追加章节。§1~§10 为 M1/M2/M3 冻结内容，本节不得反向修改它们。
> 本节与 §9/§10 共用同一条 CRC 纪律：**CRC 覆盖面必须包含长度字段**（§9.3 的"有意差异"在此复用）。
> **`META`（§10.8）自本节起为兼容读入格式**：M4 起稳态元数据是 `CURRENT` + `MANIFEST-<n>`；
> `META` 仅在"首次打开 M3 旧库"时被读一次，随后立即迁移并删除（§11.6）。§10.8/§10.9 的字节布局与
> 拒绝口径**不变**，只追加"M4 不再写它"这一条事实。

### 11.1 文件命名与文件号空间（追加）

```
<dbname>/CURRENT              指向当前 MANIFEST 的原子指针（ASCII 十进制编号 + '\n'）
<dbname>/CURRENT.tmp          CURRENT 的写临时文件（**永不**被当作 CURRENT 读）
<dbname>/MANIFEST-<n>         VersionEdit 追加日志（恢复的权威源）
<dbname>/MANIFEST-<n>.tmp     MANIFEST 的写临时文件（**永不注册**）
```

- `.log`、`.sst`、`MANIFEST-<n>` **共享**一个单调递增的 `next_file_number`（沿用 §10.1 的规则）。
  ⇒ `Open` 时权威值 = `max(MANIFEST 里的 next_file_number, max(目录中所有族的编号) + 1)`；
  **目录扫描必须包含 `MANIFEST-<n>` 的 `n`**，否则会重用 MANIFEST 编号、覆盖 CURRENT 指向的文件。
- `CURRENT` 的内容 = `^[0-9]{1,20}\n$`（纯十进制编号 + 恰好一个换行）。读侧**严格校验**，
  不符 ⇒ `kCorruption`。`CURRENT` **不**参与编号分配。
- 正则：`MANIFEST` 用 `^MANIFEST-[0-9]{6}$`；临时文件 `^MANIFEST-[0-9]{6}\.tmp$`。
  `ParseManifestFileName` **必须拒绝** `MANIFEST-<n>.tmp`（后缀匹配精确，禁止前缀匹配，同 §10.1）。

### 11.2 MANIFEST record 帧格式

```
manifest_on_disk := record*
record           := length(4B LE) ‖ type(1B) ‖ payload ‖ crc32c(4B LE)
  length  = payload 字节数（不含 length/type/crc 自身）
  type    = 记录类型；0x01 = kManifestRecordTypeVersionEdit（M4 只定义此一个值）
  crc     = crc32c( length(4B LE) ‖ type(1B) ‖ payload )        // **含长度**
```

| 字段 | 字节数 | 字节序 | 取值 / 约束 |
|---|---|---|---|
| `length` | 4 | LE | `1 .. 67108864`（64 MiB 软上界）；越界 ⇒ `kCorruption` |
| `type` | 1 | — | `0x01`；其他值 ⇒ `kNotSupported`（不是 `kCorruption`） |
| `payload` | `length` | — | §11.3 的 VersionEdit 编码 |
| `crc32c` | 4 | LE | 覆盖 `length ‖ type ‖ payload` |

- record 之间**没有**填充与对齐，reader 是无状态的"顺序读 + 每条自定界"循环。
- **每条 record 的解码必须"全或无"**：先校验 `length`/`type`/`crc`，再整体解码到临时 `VersionEdit`，
  成功后才应用。禁止边解边应用（半个 edit 生效 = §9 的"半条 record 永不生效"在元数据侧的对应物）。

### 11.3 VersionEdit 字段表

```
version_edit_payload := field*
field                := tag(varint32) ‖ value(tag 依赖)

1 kComparator          : len_prefixed_string
2 kLogNumber           : varint64
3 kNextFileNumber      : varint64
4 kMinLogNumberToKeep  : varint64
5 kDeletedFile         : level(varint32) ‖ number(varint64)
6 kNewFile             : level(varint32) ‖ number(varint64) ‖ file_size(varint64)
                         ‖ max_sequence(varint64) ‖ smallest(len_prefixed) ‖ largest(len_prefixed)
```

- `level ∈ [0, kNumLevels)`，`kNumLevels = 7`（编译期常量，不入 `Options`）。
- `smallest` / `largest` 是 **internal key**（§6），`smallest <= largest`（按 §6.1 的比较器）。
- **不定义** `last_sequence` 字段：唯一合法的恢复水位口径是 §9 的
  `max(WAL 重放最大值, 各已注册文件的 max_sequence)`（§10.8 的 `max_sequence` 字段语义不变）。
- **不定义** `compact_pointer` 字段：M4 的选文件轮转指针只在内存（重启后从各层最左重新开始）。
- 同一 edit 内**允许** `kDeletedFile` 与 `kNewFile` 混合（flush 的 edit 只有 `kNewFile`；
  compaction 的 edit 两者都有）。

### 11.4 全量快照 edit

```
全量快照 edit := kComparator ‖ kLogNumber ‖ kNextFileNumber ‖ kMinLogNumberToKeep ‖ kNewFile*
```
恢复 = 从**空版本**开始按顺序应用所有 record。⇒ 不需要"这是快照"的标志位：
新建/重建 MANIFEST 的首条 record 就是"相对空版本的全量增量"。

### 11.5 层内布局不变式（安装期校验）

- **L0**：允许 key range 重叠；文件按 `number` **降序**（新→旧）。读路径逐个检查，命中即返回。
- **L1..L6**：层内按 `smallest` 的 **user key 升序**；相邻两文件必须 `largest.user_key <
  smallest.user_key`（**严格**，不得共享任何 user key）。违反 ⇒ **拒绝安装该 Version**。
- 安装期校验必须覆盖**全部**安装路径：恢复回放的收尾、每一次 `LogAndApply`、`META` 迁移。

### 11.6 `META` 的迁移（一次性）

`Open` 的优先级：`CURRENT` 存在 ⇒ 走 MANIFEST；否则 `META` 存在 ⇒ 兼容读入（§10.8/§10.9 逐字解码），
随后**立即**写 `MANIFEST-<n>`（首条 = 全量快照 edit）→ `fsync` → 写 `CURRENT.tmp` → `fsync` →
`rename(CURRENT.tmp, CURRENT)` → `SyncDir`，然后删除 `META` / `META.tmp`。
两者都不存在时：目录中若有 `*.sst` 或 `MANIFEST-*` ⇒ `kCorruption`（"元数据丢失但目录非空"），
否则按空库处理。**稳态只写 MANIFEST + CURRENT，永不写 `META`。**
