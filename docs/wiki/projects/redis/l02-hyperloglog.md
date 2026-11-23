---
project: redis
feature: hyperloglog
page_type: feature
schema_version: 1
refs:
  - path: src/hyperloglog.c
    symbols: [hllAdd, hllCount, pfaddCommand]
---

# 基数估算

## 一句话职责

`src/hyperloglog.c` 负责基数估算数据结构的更新与估算读取：把元素哈希成「寄存器下标 + 前导零长度」，只保留每个寄存器见过的最大值，再由寄存器直方图算出近似基数；不负责集合类型的精确计数（f04）。

## 机制是什么

一个 HLL 就是一个 Redis 字符串，并没有新增数据类型。字符串开头是 16 字节的 `struct hllhdr`：4 字节 magic `HYLL`、1 字节 `encoding`、3 字节保留位、8 字节小端缓存基数 `card`。缓存的有效性用一位表达：`HLL_INVALIDATE_CACHE` 把 `card[7]` 的最高位置 1 表示作废，`HLL_VALID_CACHE` 判这一位为 0 才允许复用。

寄存器数量由 `HLL_REGISTERS` 决定，它是 `1<<HLL_P`，而 `HLL_P` 为 14，因此寄存器数是 16384；每个寄存器是 `HLL_BITS` 为 6 的整数，上限 `HLL_REGISTER_MAX` 为 63，够记到 63 个前导零。编码有三种：`HLL_DENSE` 每寄存器 6 bit，总长 `HLL_DENSE_SIZE` = 16 字节头 + (16384×6+7)/8 = 12288 字节寄存器，合计 12304 字节；`HLL_SPARSE` 用 ZERO / XZERO / VAL 三种操作码做游程压缩，空 HLL 只是 16 字节头加一条 `XZERO:16384`，正是 `createHLLObject` 里算出并断言过的 18 字节；`HLL_RAW` 是内部专用编码，只出现在 PFCOUNT 多键与 PFMERGE 的栈上裸寄存器数组里，`isHLLObjectOrReply` 用 `hdr->encoding > HLL_MAX_ENCODING` 把它挡在真实键值之外。

## 怎么实现的

**元素落到哪个寄存器：`hllPatLen`。** 用 `MurmurHash64A`（seed `0xadc83b19ULL`）算出 64 位哈希；低 `HLL_P` 位经 `HLL_P_MASK` 掩码成寄存器下标，右移 `HLL_P` 位后剩下的 Q = 50 位用来数前导零。代码先把 `1<<HLL_Q` 那一位置 1 以保证循环必然结束，于是计数落在 1 到 Q+1，即 1 到 51，实现上用 `__builtin_ctzll(hash)+1`。

**写寄存器：`hllAdd` 按编码分派。** `HLL_DENSE` 走 `hllDenseAdd`，它是 `hllPatLen` 加 `hllDenseSet`：新计数大于旧值才写回并返回 1，否则返回 0。`HLL_SPARSE` 走 `hllSparseAdd`，它是 `hllPatLen` 加 `hllSparseSet`。其余编码一律返回 -1，源码注释写明这是 invalid representation。

**稀疏写的四个出口：`hllSparseSet`。** 先判 `count > HLL_SPARSE_VAL_MAX_VALUE`（32），成立就直接 `goto promote`；再顺着操作码扫到覆盖 `index` 的那一条，扫不到（`span == 0 || p >= end`）返回 -1 表示格式损坏。命中后按情况处理：VAL 操作码且旧值已不小于 count 时返回 0（无变化）；VAL 且 run-length 为 1、ZERO 且 run-length 为 1 时原地改写；其余情形把原操作码拆成最多 5 字节的新序列插进去。拆分后若 `sdslen(o->ptr) + deltalen > server.hll_sparse_max_bytes` 同样 `goto promote`，该配置项是 `hll-sparse-max-bytes`，`src/config.c` 里 `createSizeTConfig` 的默认值参数是 3000。扩容只在 `sdsalloc` 未到上限且剩余空间不足 3 字节时用 `sdsResize`，新长度取 `sdslen+3`，再 `+= min(newlen, 300)`，最后压到上限。

**promote 等于降到稠密编码。** `promote` 标签调用 `hllSparseToDense`，返回 C_ERR 说明格式损坏，此时 `hllSparseSet` 返回 -1；成功后用 `hllDenseSet` 补写这一次的寄存器，并 `serverAssert(dense_retval == 1)`。注释给了理由：既然需要转换，说明这次确实有寄存器要变大，因此 PFADD 一定被传播，副本与 AOF 上会做同样的转换。

**读估算：`hllCount`。** 先按 encoding 调 `hllDenseRegHisto` / `hllSparseRegHisto` / `hllRawRegHisto` 得到「寄存器值 → 个数」的直方图 `reghisto[64]`，未知 encoding 走 `serverPanic`。再按 Otmar Ertl 的算法：`z = m * hllTau((m - reghisto[HLL_Q+1]) / m)`，让 j 从 `HLL_Q` 递减到 1 做 `z += reghisto[j]; z *= 0.5;`，最后 `z += m * hllSigma(reghisto[0] / m)`，结果 `E = llroundl(HLL_ALPHA_INF * m * m / z)`，其中 `HLL_ALPHA_INF` 为 0.721347520444481703680、m 是 `HLL_REGISTERS`。

**命令层：`pfaddCommand`。** 先 `lookupKeyWriteWithLink`；键不存在就 `createHLLObject` 建稀疏空 HLL 并 `dbAddByLink`，同时 `updated++`，所以 PFADD 对新建键即使不带元素也回 1。键存在时先 `isHLLObjectOrReply` 校验，再 `dbUnshareStringValue`。随后对 `argv[2..]` 逐个 `hllAdd`：返回 1 计一次 updated，返回 -1 就 `addReplyError(invalid_hll_err)` 并立即返回。全部走完后若 updated 非零，`HLL_INVALIDATE_CACHE` 置缓存无效位、`keyModified`、发 `pfadd` 键空间通知、`server.dirty += updated`；回复是 `updated ? shared.cone : shared.czero`。

**PFCOUNT 的两条路径。** 单键用 `lookupKeyRead`：键不存在回 0；`HLL_VALID_CACHE` 为真就直接把小端 `card` 的 8 个字节拼成 uint64 返回；缓存无效则 `hllCount(hdr,&invalid)` 重算，`invalid` 非零回 `invalid_hll_err`，否则把新值逐字节写回 `card` 并 `keyModified`、`server.dirty++`——`src/commands.def` 给 pfcount 标 `CMD_READONLY|CMD_MAY_REPLICATE` 正是为了这次写回。多键走另一支：栈上 `uint8_t max[HLL_HDR_SIZE+HLL_REGISTERS]`，encoding 设为 `HLL_RAW`，逐键 `hllMerge` 做 MAX 合并（键不存在直接跳过，按空 HLL 处理），最后 `hllCount` 一次，这条路径不碰缓存也不弄脏键。

**`hllMerge` 与 PFMERGE。** `hllMerge` 对稠密走 `hllMergeDense`（AVX2 或 NEON 可用时走 SIMD 版本，否则逐寄存器取 MAX），对稀疏走操作码扫描，扫完 `i != HLL_REGISTERS` 或中途溢出就返回 C_ERR。`pfmergeCommand` 只要任一输入是稠密就把 `use_dense` 置 1，目标键不存在先 `createHLLObject`，需要时用 `hllSparseToDense` 把目标转稠密，再要么 `hllDenseCompress` 一次性压入，要么逐寄存器用 `hllDenseSet` / `hllSparseSet` 写非零项；收尾同样 `HLL_INVALIDATE_CACHE` 并发出 `pfadd` 事件，注释说是按 mass-add of elements 的语义处理。

**校验的六道关：`isHLLObjectOrReply`。** 类型必须是 `OBJ_STRING`；必须是 sds 编码；长度不小于头长度；magic 逐字节是 `HYLL`；`encoding <= HLL_MAX_ENCODING`；稠密编码时长度必须精确等于 `HLL_DENSE_SIZE`。任一不过都回 `-WRONGTYPE Key is not a valid HyperLogLog string value.`。

## 关键取舍

**源码事实：** 精度与内存由 `HLL_P` 定死，16384 个 6 bit 寄存器对应 12304 字节的稠密上界；标准误由 `pfselftestCommand` 里的 `relerr = 1.04/sqrt(HLL_REGISTERS)` 定义，约为 0.81%，该自检用的阈值是 `maxerr = ceil(relerr*6*checkpoint)`，并对 checkpoint 为 10 单独放宽成 1。**源码事实：** 稀疏编码只能表示 0 到 32 的寄存器值（`HLL_SPARSE_VAL_MAX_VALUE`），超过 32 或长度超过 `hll_sparse_max_bytes` 就转稠密。**源码事实：** 默认 `hll-sparse-max-bytes` 是 3000，而 `src/hyperloglog.c` 头注释里的实测表显示稀疏编码在基数约 10000 时仍只占约 10591 字节；源码给的理由不是省内存，而是基数更大时维护稀疏编码的常数时间不再值得。**源码事实：** 稀疏转稠密只升不降，`hllSparseToDense` 成功后 `sdsfree(o->ptr)` 换上新字符串，没有反向路径。**源码事实：** 缓存基数只用 `card[7]` 的最高位表示脏，写路径只做 `HLL_INVALIDATE_CACHE`，读路径只有 PFCOUNT 单键会重算并回填。

**架构推论：** 「一个字符串装下全部状态」让 HLL 直接继承字符串类型的所有设施——`GET` / `SET` 能迁移、RDB 与 AOF 复用同一套字符串编码、`OBJECT ENCODING` 与 `MEMORY USAGE` 不必新增分支；代价是没有独立类型，任何针对键的字符串写入都能造出「不是合法 HLL 的值」，只能靠 `isHLLObjectOrReply` 的 magic 校验在命令入口兜住。**架构推论：** 稀疏编码用「每次写入线性扫操作码」换低基数省内存，因此判定转稠密的真正驱动力是 CPU 而不是内存，3000 字节这个默认值等于把大量低基数键提前推进稠密态。**架构推论：** 缓存基数让 PFCOUNT 在无写入时是纯读，但缓存一旦脏了就要全量扫 16384 个寄存器并回写 `card`，于是「读命令也会弄脏键」；pfcount 在命令表上带 `CMD_MAY_REPLICATE` 就是这条设计的下游投影。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/hyperloglog.c` | `hllPatLen` | 哈希元素，产出寄存器下标与 1 到 51 的前导零计数 |
| `src/hyperloglog.c` | `hllDenseSet` | 稠密写寄存器：新值更大才写，返回是否改变了估算 |
| `src/hyperloglog.c` | `hllSparseSet` | 稀疏写寄存器：原地改写或拆分操作码，超过 32 或超过 `hll_sparse_max_bytes` 就 promote |
| `src/hyperloglog.c` | `hllSparseToDense` | 稀疏转稠密，校验解码后下标必须正好走完 16384 个寄存器 |
| `src/hyperloglog.c` | `hllAdd` | 按 encoding 分派到稠密或稀疏写，非法编码返回 -1 |
| `src/hyperloglog.c` | `hllCount` | 按编码算寄存器直方图，再用 `hllTau` / `hllSigma` 估基数 |
| `src/hyperloglog.c` | `hllMerge` | 用 MAX 把另一个 HLL 合并进裸寄存器数组 |
| `src/hyperloglog.c` | `createHLLObject` | 建空 HLL：18 字节稀疏串，全部寄存器由一条 XZERO 覆盖 |
| `src/hyperloglog.c` | `isHLLObjectOrReply` | 类型、magic、encoding、长度六道校验，失败回 WRONGTYPE |
| `src/hyperloglog.c` | `pfaddCommand` | PFADD 命令体：建键、逐元素写入、失效缓存、传播与键空间通知 |
| `src/hyperloglog.c` | `pfcountCommand` | PFCOUNT 命令体：单键读或回填缓存，多键在栈上 MAX 合并后估算 |
| `src/hyperloglog.c` | `pfmergeCommand` | PFMERGE 命令体：合并输入后写入目标键并失效缓存 |

## 与主核 / AI 的关系

HLL 在本仓之外的第一个真实调用方是集合模块：`src/t_set.c` 的 `sunionDiffGenericCommand` 在 `approx` 为真时先 `createHLLObject`，再把并集元素逐个喂给 `hllAdd`，用 `hllCount` 读估算，并以 `HLL_CHECK_INTERVAL_FLOOR`（1024）为下限安排两次估算之间的采样间隔，估算值达到 LIMIT 就提前退出；这条路径由 `sunioncardCommand` 的 APPROX 选项触发。**架构推论：** 这说明 HLL 的地位是「一个可被其他类型借用的估算内核」，而不是只服务 PF* 命令的封闭类型——主核若要给「近似基数」做统一能力，可以照这个形态把估算逻辑做成可调用的内核函数，命令层只负责参数解析与结果截断。

对 AI 侧，HLL 适合「统计不重复的 chunk id、文档 id、请求指纹」这类只关心量级的场景：12304 字节的稠密上界与 18 字节的空串起步，让它可以按 key 铺开而不是集中一张大表。**架构推论：** 但它只回答「有多少个不同元素」，不回答「哪些是不同的」；任何需要回查成员的去重（例如判定某 chunk 是否已入库）仍然要落到 f04 的精确集合或主核自己的索引，把 HLL 当前置的粗筛可以省内存，但不能当前置的判存依据。

## 参考

- [Redis 源码：hyperloglog.c](https://github.com/redis/redis/blob/unstable/src/hyperloglog.c)
- [Redis 文档：HyperLogLog 数据类型](https://redis.io/docs/latest/develop/data-types/probabilistic/hyperloglogs/)
- [Redis 命令：PFADD](https://redis.io/docs/latest/commands/pfadd/)
- [Redis 命令：PFCOUNT](https://redis.io/docs/latest/commands/pfcount/)
- [Redis 命令：PFDEBUG](https://redis.io/docs/latest/commands/pfdebug/)
