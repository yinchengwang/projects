---
project: redis
feature: ziplist-legacy-encoding
page_type: feature
schema_version: 1
refs:
  - path: src/ziplist.c
    symbols: [ziplistIndex]
---

# 旧版紧凑列表编码

## 一句话职责

ziplist 是 Redis 早期的紧凑列表编码，它靠「每个条目自带前一条目的长度」实现双向遍历；今天它只出现在读旧 RDB 快照的路径上，`ziplistIndex` 是这套定位语义的入口。

## 机制是什么

`src/ziplist.c` 顶部的注释描述了布局：整个 ziplist 是一段连续内存，开头是固定的头（总字节数 uint32、尾条目偏移 uint32、条目数 uint16），末尾是一个 `0xFF` 的结束条目，中间是若干条目。

每个条目的格式是 `<prevlen> <encoding> <entry-data>`。`prevlen` 是**前一条目的总长度**：小于 254 时只占 1 字节，等于或大于 254 时占 5 字节（首字节 254，后跟 4 字节小端长度）。`encoding` 说明本条目是字符串还是整数，以及字符串的长度编码方式。

关键设计是：`prevlen` 让**反向遍历每步都是 O(1)**——给定当前条目指针，直接减去 `prevlen` 就得到前一条目的起点；而**正向遍历必须解 `encoding` 才能算出本条目有多长**。两个方向的成本不对称，这正是 `ziplistIndex` 要分别写两套逻辑的原因。

今天 ziplist 已经不是活跃编码：新写入的列表用 listpack（e05），列表 / 哈希 / 有序集合的紧凑编码都已换成 listpack 系。ziplist 剩下的价值是**兼容读取与升级路径**——`src/rdb.c` 载入旧版本 RDB 快照时仍会遇到 ziplist 编码的列表、哈希与有序集合。

本页只讲这套旧编码的定位与遍历语义。listpack 的实现是 e05，列表 / 哈希 / 有序集合的命令语义是 F 组。

## 怎么实现的

**头部与边界。** `src/ziplist.c` 的宏把布局写死：`ZIPLIST_BYTES` 取总字节数、`ZIPLIST_TAIL_OFFSET` 取尾条目偏移、`ZIPLIST_LENGTH` 取条目数（uint16）、`ZIPLIST_HEADER_SIZE` 是 `sizeof(uint32_t)*2+sizeof(uint16_t)`、`ZIPLIST_END_SIZE` 是 `sizeof(uint8_t)`、`ZIPLIST_ENTRY_HEAD` 指向第一个条目、`ZIPLIST_ENTRY_TAIL` 用头里的偏移直接指向最后一个条目、`ZIPLIST_ENTRY_END` 指向末尾那个 `0xFF`。

**条目编码。** 字符串长度有三种编码：`|00pppppp|`（1 字节头，长度 ≤ 63）、`|01pppppp|qqqqqqqq|`（2 字节头，长度 ≤ 16383，14 位大端）、`|10000000|qqqqqqqq|rrrrrrrr|ssssssss|`（5 字节头，32 位大端长度）。整数有六种编码：int16（3 字节）、int32（5 字节）、int64（9 字节）、24 位有符号（4 字节）、8 位有符号（2 字节）、`|1111xxxx|` 立即数（1 字节，取值 1 到 13——源码注释说明 0000 与 1111 不可用，所以要减 1）。所有整数一律小端存放，即使编译到大端平台（只有上面那两种字符串长度字段按注释是大端）。

**长度上限。** `ZIPLIST_LENGTH` 是 uint16，宏上方的注释写明「this macro should never overflow the unsigned 16 bit integer... When UINT16_MAX is reached we want the count to stay there to signal that a full scan is needed to get the number of items inside the ziplist」。另一个安全上限是 `ZIPLIST_MAX_SAFETY_SIZE`，定义为 `(1<<30)`，注释写明「Don't let ziplists grow over 1GB in any case, don't wanna risk overflow in zlbytes」。

**`ziplistIndex`。** 注释是「Returns an offset to use for iterating with ziplistNext. When the given index is negative, the list is traversed back to front. When the list doesn't contain an element at the provided index, NULL is returned.」函数体分两支：

- **负下标（从尾往前）。** 先 `index = (-index)-1`，把「倒数第 n 个」翻译成「从尾往前走 n 步」。起点 `p = ZIPLIST_ENTRY_TAIL(zl)`，也就是头里记的尾条目偏移。若 `p[0] != ZIP_END`，用 `ZIP_DECODE_PREVLENSIZE` 解出 `prevlen` 占几字节、用 `ZIP_DECODE_PREVLEN` 解出前一条目的总长度，然后 `while (prevlen > 0 && index--)` 循环：`p -= prevlen` 跳到前一条目，再解出新位置的 `prevlen`。源码注释解释了这里为什么不需要「安全」检查：「when going backwards, we know the header we're parsing is in the range, we just need to assert (below) that the size we take doesn't cause p to go outside the allocation.」
- **非负下标（从头往后）。** 起点 `p = ZIPLIST_ENTRY_HEAD(zl)`，然后 `while (index--)` 循环：`p += zipRawEntryLengthSafe(zl, zlbytes, p)` 前进一条目，若 `p[0] == ZIP_END` 就跳出。源码注释解释了这里为什么必须用「安全」版本：「When we go forward, we need to be careful not to decode an entry header if it's past the ziplist allocation.」`zipRawEntryLengthSafe` 内部先调 `zipEntrySafe` 校验条目头与条目数据不会越出 ziplist 的分配范围，再返回「头部长度 + 数据长度」。

两支汇合后统一判越界：`if (p[0] == ZIP_END || index > 0) return NULL;`——落在末尾、或步数没走够（下标超出元素个数），都返回 NULL。返回前还会 `zipAssertValidEntry(zl, zlbytes, p)` 校验条目本身在分配范围内（这一次的校验额外包含 `prevlen` 指回的前一条目位置）。

**真实 caller。**

- **兼容读取与升级路径的主入口是 `src/rdb.c`。** 旧 RDB 快照里的 `RDB_TYPE_LIST_ZIPLIST` / `RDB_TYPE_HASH_ZIPLIST` / `RDB_TYPE_ZSET_ZIPLIST` 在载入时先过 `ziplistValidateIntegrity`，其回调把每个 ziplist 条目取出来转成新格式：`_ziplistEntryConvertAndValidate` 的注释写明「The ziplist element pointed by 'p' will be converted and stored into listpack.」，用的是 `ziplistGet` + `lpAppend` / `lpAppendInteger`；列表走的是 `_listZiplistEntryConvertAndValidate`，注释写明「...converted and stored into quicklist」，用的是 `ziplistGet` + `quicklistPushTail`；哈希走 `_ziplistPairsEntryConvertAndValidate`，除转 listpack 外还检查字段名是否重复。转换完成后 `src/rdb.c` 会把对象编码改成 `OBJ_ENCODING_QUICKLIST` 或 `OBJ_ENCODING_LISTPACK`——**ziplist 在读路径上被就地升级掉，不会留在内存里**。
- **`ziplistIndex` 自身的 caller 主要在 `src/ziplist.c` 内。** `ziplistDeleteRange` 用它把下标转成指针；`ziplistRandomPair` / `ziplistRandomPairs` / `ziplistRandomPairsUnique` 用它定位随机抽样的起点（这些函数的注释里写明 `total_count` 是预先算好的「length/2 of the ziplist (to avoid calls to ziplistLen)」）。
- **`src/redis-check-rdb.c`** 按 `list-ziplist` / `zset-ziplist` / `hash-ziplist` 这些类型名识别旧快照里的 ziplist 编码。
- **`src/server.c`** 把 `ziplistTest` 注册进测试命令表，所以 ziplist 至今仍有自测入口。

**退化与回退路径。**

- **下标越界返回 NULL 而不是崩溃。** 真实 caller 是 `src/ziplist.c` 的 `ziplistDeleteRange`：`p = ziplistIndex(zl,index); return (p == NULL) ? zl : __ziplistDelete(zl,p,num);`——定位失败就原样返回 ziplist，不做任何删除。
- **正向定位用「安全」长度，越界不解析。** 真实 caller 是 `ziplistIndex` 的正向分支本身：`zipRawEntryLengthSafe(zl, zlbytes, p)` 在解 encoding 之前先确认条目没越出 `zlbytes`。
- **反向定位只做断言。** 真实 caller 是 `ziplistIndex` 的反向分支：`assert(p + prevlensize < zl + zlbytes - ZIPLIST_END_SIZE)` 与 `assert(p >= zl + ZIPLIST_HEADER_SIZE && p < zl + zlbytes - ZIPLIST_END_SIZE)`——注释说这里不需要「安全」检查，因为反向走时已知解析的表头在范围内，只需断言步长不会跑出分配。
- **载入时的最终退路是报损坏。** 真实 caller 是 `src/rdb.c`：`ziplistValidateIntegrity` 失败时 `rdbReportCorruptRDB("List ziplist integrity check failed.")` 并放弃载入该键。

## 关键取舍

**源码事实：** ziplist 每条目自带前一条目的总长度，因此反向遍历每步 O(1)、正向遍历必须解 encoding 才能算长度。**源码事实：** 字符串长度有 3 种编码、整数有 6 种编码，条目头最短 1 字节。**源码事实：** 条目数用 uint16 计数，满了停在 `UINT16_MAX` 表示需要全扫描。**源码事实：** 整个结构有 1GB 的安全上限。**源码事实：** ziplist 现在只出现在 RDB 兼容读取路径与自测里，读入后即被升级为 listpack / quicklist。

**架构推论：** 用「每条目冗余前一条目长度」换取双向遍历能力，代价是每条目多占 1 到 5 字节，并且插入或删除之后被改动位置之后的所有 `prevlen` 都会失效、需要级联修正——`src/ziplist.c` 里的 `__ziplistCascadeUpdate` 就是干这个的。**架构推论：** 「反向 O(1)、正向需解头」的不对称让 ziplist 更适合尾部操作，这正是它当年的设计取向。**架构推论：** 把 ziplist 留成只读兼容格式、读入即升级为 listpack，是把「新写入走新格式」与「旧数据仍可读」分开，避免为兼容长期付成本；代价是每次读旧快照都要付一次格式转换的 CPU 与临时内存。**架构推论：** 主核若要做多版本数据文件兼容，值得抄的是「读入即升级」这条策略——不要让旧格式长期留在活跃数据里；同时要抄的是「越界返回空指针而不是崩溃」这条边界约定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ziplist.c` | `ziplistIndex` | 把下标转成条目指针：负下标从尾反向走，非负下标从头正向走，越界返回 NULL |

## 参考

- [Redis 源码：ziplist.c](https://github.com/redis/redis/blob/unstable/src/ziplist.c)
- [Redis 源码：rdb.c](https://github.com/redis/redis/blob/unstable/src/rdb.c)
- [Redis 文档：数据类型（List）](https://redis.io/docs/latest/develop/data-types/lists/)
- [Redis 文档：持久化 RDB](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/)

## 与主核 / AI 的关系

ziplist 对主核的直接价值不大——它是 Redis 自己的历史包袱，主核不需要照抄这套编码；真正值得带走的是**「旧格式只读兼容 + 读入即升级」这条策略**：主核若要支持多版本数据文件或多种向量索引格式并存，正确的做法是让旧格式只出现在读取路径上，读进来就转成当前格式，不要让两套格式在活跃数据里长期共存，否则每个读写路径都要做格式判断，复杂度会渗透到每一处。同时值得带走的是**「下标越界返回空指针而不是崩溃」**这条约定——主核的多模态列要支持按序号访问，越界必须是一个可判定的返回值，而不是让读路径踩到未定义行为。不可照搬的是 ziplist 的**「每条目冗余前一条目长度」**：它让插入与删除都需要级联修正 `prevlen`，在主核的并发写模型下这种「一个元素的改动牵连后续一串元素」的结构会直接与行锁粒度冲突，主核更适合用变长记录加目录索引的形态。AI 侧若把模型版本、特征维度的历史产物落盘，保留旧格式只用于回放与迁移、当前形态统一由新格式承担，与 Redis 这条「读入即升级」的路线是一致的。
