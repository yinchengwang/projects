---
project: redis
feature: bitops
page_type: feature
schema_version: 1
refs:
  - path: src/bitops.c
    symbols: [setbitCommand, getbitCommand, bitcountCommand, bitposCommand, bitfieldGeneric]
---

# 位与位域操作

## 一句话职责

`src/bitops.c` 把「把一个字符串键当作一段连续位」收敛成五条命令入口：单比特读写走 `setbitCommand` / `getbitCommand`，区间计数与定位走 `bitcountCommand` / `bitposCommand`，任意宽度位域的读改写走 `bitfieldGeneric`；位序约定与偏移合法区间由 `getBitOffsetFromArgument` 和 `normalizeBitRange` 这两个共用辅助函数统一裁决。

## 机制是什么

位序是这一族命令的公共约定。`src/bitops.c` 里 setUnsignedBitfield 段落的注释写明：bit 编号 0 是首个字节的最高位，与 SETBIT 等命令同一约定；这条约定在 `setbitCommand` 里落实为 `byte = bitoffset >> 3` 与 `bit = 7 - (bitoffset & 0x7)`。`setUnsignedBitfield` / `getUnsignedBitfield` 用同一组算式逐位搬运，所以 SETBIT 与 BITFIELD 在同一个字节上看的是同一位。

值的底层表示仍然是一个字符串对象（或者可选的原生 Roaring 位图对象），位操作不引入新的值类型语义。写入越出当前长度时，由 `lookupStringForBitCommand` 负责增长并零填充——「SETBIT 一个远偏移会把值撑到多长」这件事只由这一步决定。

命令表给出的参数个数口径见 `src/commands.def`：SETBIT arity `4`、GETBIT arity `3`、BITCOUNT arity `-2`、BITPOS arity `-3`、BITFIELD 与 BITFIELD_RO 均为 `-2`；BITCOUNT 带 `CMD_READONLY`，BITFIELD 带 `CMD_WRITE|CMD_DENYOOM`，BITFIELD_RO 带 `CMD_READONLY|CMD_FAST`。

同一个源文件里还有 BITOP、BITCONVERT、BITRANGEINTARRAY 等入口，它们不在本页 refs 声明的符号集合内，本页不展开。字符串类型的普通读写归 f01 页，对象编码的选择与转换归 d02 页。

## 怎么实现的

**偏移闸门：`getBitOffsetFromArgument`。** 所有按位寻址的命令都从这里进。它先看 `#<offset>` 形态：只有当调用方传 `hash=1` 且 `bits > 0` 时才把首字符 `#` 当标记（BITFIELD 的解析传 `1`，SETBIT / GETBIT 传 `0`）。接着用 `string2ll` 解析，失败或 `loffset < 0` 一律回 "bit offset is not an integer or out of range"。`#<offset>` 相对偏移还要乘 `bits`，乘之前用 `loffset > LLONG_MAX / bits` 挡溢出。最后一道是上限：`(loffset >> 3) >= server.proto_max_bulk_len` 时回同一个错误；`mustObeyClient(c)` 为真（AOF 客户端与主节点客户端）时跳过这道检查，因为它们写的是已经存在的逻辑状态。`proto-max-bulk-len` 由 `src/config.c` 的 `createLongLongConfig` 注册，默认值 `512ll*1024*1024`，下界 `1024*1024`、上界 `LONG_MAX`，所以默认上限下最大字节下标是 `512*1024*1024-1`、最大位下标是 `512*1024*1024*8-1`。

**写入：`setbitCommand`。** 解析完偏移后用 `getLongFromObjectOrReply` 读 bit 值，`on & ~1` 非零就回 "bit is not an integer or out of range"——只认 0 与 1。随后 `lookupKeyWriteWithLink` 一次拿到值与链接，`bitroarResolveTarget` 决定是否切入原生位图。字符串路径交给 `lookupStringForBitCommand`：键不存在就 `createObject(OBJ_STRING, sdsnewlen(NULL, byte+1))` 再 `dbAddByLink`；键已存在就 `dbUnshareStringValue` 后 `sdsgrowzero(o->ptr, byte+1)`，`strGrowSize` 记下长度增量。改位本身是三句：读旧字节、`byteval &= ~(1 << bit)`、`byteval |= ((on & 0x1) << bit)`。是否算修改由 `changed = strGrowSize || (!!bitval != on)` 判定——源码注释强调 `bitval` 是十进制数，所以要用 `!!` 归一；为真才 `keyModified`、发 `NOTIFY_STRING` 的 `setbit` 键空间通知、`server.dirty++`。回复永远是**旧值**：`addReply(c, bitval ? shared.cone : shared.czero)`。

**读取：`getbitCommand`。** `lookupKeyReadOrReply(c, c->argv[1], shared.czero)`：键不存在直接回 0 并返回，不存在不是错误。类型由 `checkStringOrBitmapType` 把关（`src/object.c` 里它就是「不是 OBJ_STRING 也不是 OBJ_BITMAP 就回 WRONGTYPE」）。原生位图分支调 `bitroarGetBit`，它在 `(bitoffset >> 3) >= bitmap->byte_len` 时返回 0；字符串分支在 `byte < sdslen` 时读字节；整型编码的值底层没有 sds，要先 `ll2string` 到 `LONG_STR_SIZE`（`src/util.h` 定义为 `21`）大小的栈缓冲，再从缓冲按字节读。越界读一律是 0，源码注释把这条写成「like any other unset bit」。

**区间归一：`normalizeBitRange`。** BITCOUNT 与 BITPOS 共用它。`totlen` 先取 `strlen`，`isbit` 为真时左移 3 位换算成位总数；负的 start / end 各自加 `totlen`，仍为负的夹到 `0`，`end >= totlen` 的夹到 `totlen-1`。区间为空（`start > end`）时它直接返回，不产出位区间。`isbit` 分支把半开位区间 `[start, end+1)` 写进 `bit_start` / `bit_end`，并算出两个掩码：`first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF`、`last_byte_neg_mask = (1<<(7-(end&7)))-1`；随后 start / end 落回字节坐标。这两个掩码就是「按位区间只覆盖首尾字节一部分」时的减法依据。

**BITCOUNT：`bitcountCommand`。** 参数只认三种形态：`argc == 2` 全串（内部取 `start = 0`、`end = strlen-1`）、`argc == 4` 带 start / end、`argc == 5` 再带 `BIT|BYTE`，其它形态回 `shared.syntaxerr`；第 5 个参数用 `strcasecmp` 与完整字符串 `bit` / `byte` 比较，不匹配就回 `shared.syntaxerr`。键不存在时回 `shared.czero`。计数主调用是 `redisPopcountAuto(p+start, bytes)`；只要两个掩码有一个非零，就再拼一个两字节 `firstlast` 缓冲，让首尾字节里落在区间外的位保留下来，用同一个 popcount 实现数出后从总数里减掉：`count -= redisPopcountAuto(firstlast, 2)`，源码注释把这一步解释成「把区间内的位清零」的等价技巧。

**BITPOS：`bitposCommand`。** bit 参数不在 `{0,1}` 里直接回 "The bit argument must be 1 or 0."。给 start 不给 end 时（`argc < 5`）结束位置按 `isbit` 取：位模式是 `(strlen<<3)+7`，字节模式是 `strlen-1`，源码注释说明这是「字符串右侧按零填充」的口径。键不存在时按「无限长的 0 数组」处理：找 0 回 `0`，找 1 回 `-1`。区间为空回 `-1`，空区间既没有 0 也没有 1。带掩码时先把首字节按反相掩码处理——找 1 用 `p[start] & ~first_byte_neg_mask`（区间外清成 0），找 0 用 `p[start] | first_byte_neg_mask`（区间外置成 1），让区间外的位一律不等于目标，再对单字节缓冲调 `redisBitpos(&tmpchar,1,bit)` 并提前收尾；末尾字节同理。`redisBitpos` 自身的返回约定写在它的注释里：找 1 而全串无 1 时回 `-1`，找 0 且找不到时按零填充回 `count*8`；若走到 `while(one)` 之外就是 `serverPanic("End of redisBitpos() reached.")`，那里不该到达。收尾还有一条纠偏：`end_given && bit == 0 && pos == bytes<<3` 说明第一个 0 落在显式区间之外，回 `-1`；命中值要加 `pos += start<<3`，补回被跳过的字节偏移。

**位域：`bitfieldGeneric`。** BITFIELD 传 `BITFIELD_FLAG_NONE`（`0`），BITFIELD_RO 传 `BITFIELD_FLAG_READONLY`（`1<<0`），之后流程完全共用。解析在 `bitfieldParseOps`：子命令按「剩余参数够不够」判定——`get` 要 `remargs >= 2`、`set` / `incrby` 要 `>= 3`、`overflow` 要 `>= 1`；`OVERFLOW` 的类型是**粘性**的，初值 `BFOVERFLOW_WRAP`（`0`），`sat` 置 `BFOVERFLOW_SAT`（`1`），`fail` 置 `BFOVERFLOW_FAIL`（`2`），非法名回 "Invalid OVERFLOW type specified"。类型串由 `getBitfieldTypeFromArgument` 解：首字符必须是 `i` 或 `u`，其后 `string2ll`；宽度下限 `1`，`i` 上限 `64`，`u` 上限 `63`（源码注释给了理由：RESP 无法回大于 INT64_MAX 的无符号值）。写操作顺带维护 `highest_write_offset = max(bitoffset + bits - 1)`，这是「值至少要长到哪一位」的唯一来源。解析完若 `readonly` 仍为真（全部是 GET），走 `bitfieldReadOnly`；否则若外层带 `BITFIELD_FLAG_READONLY`，回 "BITFIELD_RO only supports the GET subcommand"。

写路径 `lookupKeyWriteWithLink` 只做一次查找（源码注释说明：字符串路径靠它增长，原生位图路径靠它安装新值），然后分两路：字符串走 `bitfieldWriteString`，位图走 `bitfieldWriteRoaring`。字符串侧先按 `highest_write_offset` 调 `lookupStringForBitCommand` 保证长度，再 `addReplyArrayLen(c, numops)`（**每个子命令一个数组元素**），然后 `bitfieldExecuteStringOps` 逐条执行。GET 的技巧是用 `unsigned char buf[9] = {0}` 拷最多 9 个字节，让 64 位宽且未对齐的读也能在零填充缓冲上完成；SET / INCRBY 交给 `bitfieldSignedResult` / `bitfieldUnsignedResult` 算新值与回复值，再由 `setSignedBitfield` / `setUnsignedBitfield` 落位。

溢出语义落在 `checkSignedBitfieldOverflow` 与 `checkUnsignedBitfieldOverflow`：`WRAP` 走 `handle_wrap` 算掩码后的回绕值，`SAT` 把 `*limit` 设成该宽度的最大值或最小值，`FAIL` 直接返回非零。`bitfieldSignedResult` / `bitfieldUnsignedResult` 只在 `overflow && owtype == BFOVERFLOW_FAIL` 时返回 `C_ERR`，调用方据此 `addReplyNull` 并 `continue`——**不写入**。INCRBY 回复新值、SET 回复旧值，这两条分别写在两个 result 函数的 `INCRBY` 与 `else` 分支里。

**原生位图分支与传播顺序。** `bitroarResolveTarget` 决定要不要从字符串切到原生位图：只有「键不存在或值是 OBJ_STRING」且 `bitroarDefaultEnabled(c)` 为真时才可切换；后者在 `bitmap-default-roaring` 关闭（`src/config.c` 的 `createBoolConfig`，默认值 `0`）、`server.masterhost` 非空（本节点为从库）、或 `mustObeyClient` 为真时返回 0，并且要求 `server.allowed_propagate_targets` 同时含 `PROPAGATE_AOF` 与 `PROPAGATE_REPL`。可切换后还要过两道边界：`bitroarCanRepresentBit(maxbit)` 不通过回 "bit offset is out of range"；值是字符串且 `bitroarStringTooLarge` 为真回 "bitmap length exceeds Roaring bitmap limit"。内部上限分两处定义：`src/bitroar.h` 的 `BITROAR_MAX_BYTES = (uint64_t)(INT64_MAX >> 3)`，`src/bitroar.c` 再据此定义 `BITROAR_MAX_BITOFFSET = BITROAR_MAX_BYTES*8-1`，换算出的最大位下标是 `9223372036854775799`。切换动作是「先排队 BITCONVERT、再就地转换」：`bitroarPropagateConvert` 用 `alsoPropagate` 把 `BITCONVERT <key>` 发往 AOF 与副本，`bitroarConvertKey` 就地新建位图或替换值（替换时发 `NOTIFY_TYPE_CHANGED` 的 `type_changed`）。源码注释解释了为什么必须重新查找一次：转换期间的 `NOTIFY_NEW` / `NOTIFY_TYPE_CHANGED` 回调可以同步替换或删除这个键，所以随后用 `lookupKeyWriteWithFlags(c->db, key, LOOKUP_NOTOUCH)` 重新拿值，并且刻意不再套一次默认位图判定。

## 关键取舍

**源码事实：** 位序固定为「首字节最高位是 bit 0」，SETBIT 与 BITFIELD 共用同一组算式，两者可以在同一个键上交替使用而不需要换算。**源码事实：** 把某位写进不存在的键时并不只写一个字节，`lookupStringForBitCommand` 按 `byte+1` 建全长（`byte = maxbit >> 3`），中间全部零填充，`strGrowSize` 因此能作为「是否修改」的判据之一。**源码事实：** 偏移上限有两道闸，命令层用 `server.proto_max_bulk_len`（默认 512 MB）挡普通客户端，位图层用 `BITROAR_MAX_BITOFFSET` 挡内部长度算术；`mustObeyClient` 只让前者对主从与 AOF 客户端失效，后者对所有客户端都生效。**源码事实：** BITCOUNT 在按位区间上不是先裁位再数，而是「整字节计数 + 首尾掩码修正」。**源码事实：** popcount 的选择发生在 `redisPopcountAuto`：编译期有 AVX512 且 `BITOPS_USE_AVX512_POPCOUNT` 为真走 `redisPopCountAvx512`；否则编译期有 AVX2 且 `BITOP_USE_AVX2` 为真走 `redisPopCountAvx2`；否则 AArch64 构建走 `redisPopCountAarch64`；其余走 `redisPopcount`。`redisPopcount` 自己是标量版：`__builtin_cpu_supports("popcnt")` 为真时按 `32` 字节展开 `__builtin_popcountll`，否则按 `28` 字节跑 SWAR 位技巧，短尾统一查 `bitsinbyte[256]` 表。NEON 版的累加上限是 `POPCOUNT_NEON_MAX_ITERS = 1024` 次 128 字节迭代，源码注释说明 16 位 lane 在 2048 次会溢出，取 1024 留两倍余量。**源码事实：** BITFIELD 的 `OVERFLOW` 声明是粘性的，一次声明对后面所有子命令生效，直到下一次 `OVERFLOW`。**源码事实：** `OVERFLOW FAIL` 的溢出子命令回空数组元素且不写值，`WRAP` 与 `SAT` 都会回一个值（回绕值或饱和值）。

**架构推论：** 位域宽度上限卡在 `u63` 而不是 `u64`，是为了让回复统一走有符号 64 位整数协议；代价是无符号全宽字段在 BITFIELD 里不可表达。**架构推论：** 用固定 9 字节的栈缓冲做 GET，是把「最大 64 位宽加最多 7 位不齐」的跨字节读摊平成一次零填充小数组，免掉读路径的内存分配；代价是每次 GET 都要拷最多 9 个字节。**架构推论：** 表示转换必须作为独立的 `BITCONVERT` 传播，而不是让副本按自己的本地配置决定，这样同一条逻辑写在不同配置的节点上落到同一个表示；代价是 AOF 与副本流里多出一条内部命令。**架构推论：** 把「位运算命令」全部收在一个文件、共用偏移解析与区间归一，代价是该文件同时承担命令语义、数值溢出语义与 SIMD 选型三件事，读某一个命令的人会被另外两层细节干扰。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/bitops.c` | `setbitCommand` | SETBIT：偏移与 bit 值校验、切位图或走字符串、按 `byte+1` 增长并零填充、按「长度变化或位值变化」记账、回复旧值 |
| `src/bitops.c` | `getbitCommand` | GETBIT：键不存在回 0，越界读一律 0，整型编码先 `ll2string` 再按字节读 |
| `src/bitops.c` | `bitcountCommand` | BITCOUNT：参数三形态、`normalizeBitRange` 归一、`redisPopcountAuto` 计数、首尾掩码修正 |
| `src/bitops.c` | `bitposCommand` | BITPOS：bit 参数只认 0/1、缺省 end 的两种口径、掩码清位、`redisBitpos` 定位与 `end_given` 纠偏 |
| `src/bitops.c` | `bitfieldGeneric` | BITFIELD / BITFIELD_RO 共用入口：解析、只读分流、切位图、两套执行器与记账通知 |
| `src/bitops.c` | `getBitOffsetFromArgument` | 偏移解析与两道闸门：`#<offset>` 乘 `bits`、`proto_max_bulk_len` 上限、`mustObeyClient` 例外 |
| `src/bitops.c` | `normalizeBitRange` | start / end 归一成字节坐标，派生 `bit_start` / `bit_end` 与首尾字节掩码 |
| `src/bitops.c` | `lookupStringForBitCommand` | 写前按 `maxbit >> 3` 建全长或 `sdsgrowzero` 增长，返回长度新旧值 |
| `src/bitops.c` | `redisPopcountAuto` | popcount 选型：AVX512 → AVX2 → NEON → 标量 `redisPopcount` |
| `src/bitops.c` | `bitfieldParseOps` | 子命令解析、粘性 OVERFLOW、`highest_write_offset` 计算 |
| `src/bitops.c` | `checkSignedBitfieldOverflow` / `checkUnsignedBitfieldOverflow` | WRAP / SAT / FAIL 三种溢出语义的实际计算 |
| `src/bitops.c` | `bitroarResolveTarget` / `bitroarConvertKey` | 默认位图的切换判定与就地转换 |
| `src/bitroar.c` | `bitroarCanRepresentBit` / `bitroarGetBit` | 内部可表示上界判定与位图取位（越界返回 0） |
| `src/bitroar.c` | `BITROAR_MAX_BITOFFSET` | 由 `BITROAR_MAX_BYTES*8-1` 定义的位下标上界，`bitroarCanRepresentBit` 的判据 |
| `src/bitroar.h` | `BITROAR_MAX_BYTES` | 位图逻辑长度的内部上界 `INT64_MAX>>3`，只保护长度与区间算术 |

## 与主核 / AI 的关系

主核的查询路径里常见「用位图做行过滤」的形态：布隆过滤器、向量索引的候选集屏蔽位、删除标记位。这套命令给出的可借鉴结构是三点——**单一的偏移闸门**（`getBitOffsetFromArgument` 统一负责负数、溢出、上限，命令实现里不再重复判定）、**单一的区间归一**（`normalizeBitRange` 把负下标、越界、位/字节双口径一次算清，调用方只拿结果），以及**写前一次查找带链接**（`lookupKeyWriteWithLink` 避免二次查找被 rehash 打断）。

AI 侧把位图当特征位集合用时有两个真实坑。第一是位序：Redis 是 MSB-first（首字节最高位是 bit 0），与多数语言里 `1 << (i % 8)` 的 LSB-first 直觉相反，跨系统对位图做归并时是静默错位的来源。第二是宽度与偏移：BITFIELD 的 `u` 类型上限 `63` 位、偏移默认受 `proto-max-bulk-len` 约束，默认 512 MB 意味着位下标最多到 `512*1024*1024*8-1`（约 4 Gbit 的位空间），超过这个位空间的稠密位图会先在 `getBitOffsetFromArgument` 的上限判断处被拒。想突破字符串长度带来的内存浪费，本分支提供了 `bitmap-default-roaring`（默认关闭）的原生 Roaring 位图表示，但它的切换只发生在 AOF 与副本都可达的写入上，从库上的写不会自主转换。

## 参考

- [Redis 源码：bitops.c](https://github.com/redis/redis/blob/unstable/src/bitops.c)
- [Redis 源码：bitroar.c](https://github.com/redis/redis/blob/unstable/src/bitroar.c)
- [Redis 文档：SETBIT](https://redis.io/docs/latest/commands/setbit/)
- [Redis 文档：GETBIT](https://redis.io/docs/latest/commands/getbit/)
- [Redis 文档：BITCOUNT](https://redis.io/docs/latest/commands/bitcount/)
- [Redis 文档：BITPOS](https://redis.io/docs/latest/commands/bitpos/)
- [Redis 文档：BITFIELD](https://redis.io/docs/latest/commands/bitfield/)
- [Redis 文档：BITFIELD_RO](https://redis.io/docs/latest/commands/bitfield_ro/)
