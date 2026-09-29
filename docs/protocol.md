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
