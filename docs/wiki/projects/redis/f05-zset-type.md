---
project: redis
feature: zset-type
page_type: feature
schema_version: 1
refs:
  - path: src/t_zset.c
    symbols: [zsetAdd, zsetScore, zsetDel, zsetRank, zsetTypeMaybeConvert]
---

# 有序集合类型

## 一句话职责

`src/t_zset.c` 负责有序集合的成员级语义：给成员打分、读分值、删成员、算名次，并决定紧凑编码装不下时该升成哪种通用编码。

## 机制是什么

有序集合是「成员 → 分值」的映射，同时按分值排序，成员唯一。与哈希不同，它多了两个维度：**分数**和**名次**。命令层对外是 ZADD/ZSCORE/ZREM/ZRANK 这一族，内部则按编码分派。

**这个分支的内部编码有三套**，这一点必须先说清楚，否则后面的判据会读不懂：

- **listpack**：成员与分值交替排在紧凑数组里，适合小集合。
- **skip list**：成员挂在跳表上、同时在一个哈希表里按成员名索引——这是 Redis 长期以来的「通用编码」。
- **B+ tree**（`OBJ_ENCODING_BTREE`）：本分支新增的打包 B 树，**它是集合变大时的默认去向**。

也就是说：`zsetTypeMaybeConvert` 把 listpack 升成的是 **B+ 树**，不是跳表。跳表在这份代码里是**兼容旧数据与旧 RDB 的遗留编码**，仍然被完整支持（读、写、删、名次都有分支），只是不再是自动升级的目标。跳表自身的插入与层级维护属于 f06 页；本页讲有序集合的命令语义与编码切换判据。

## 怎么实现的

**编码切换判据：`zsetTypeMaybeConvert(zobj, size_hint)`。** 规则只有一条：对象是 listpack 且 `size_hint > server.zset_max_listpack_entries`，就 `zsetConvertAndExpand(zobj, OBJ_ENCODING_BTREE, size_hint)` 并返回 1（表示发生了转换）；其余情况返回 0。**非 listpack 编码走这条函数等于什么都不做**——这是它的退化路径。

它的调用者是 `zaddGenericCommand`：键已存在时用 `elements`（本次命令里的 score-member 对数）作为 size_hint。键不存在时走另一条路 `zsetTypeCreate(size_hint, val_len_hint)`——两个提示都在阈值内（`zset_max_listpack_entries` 与 `zset_max_listpack_value`）就建 listpack 对象，否则直接建 B+ 树对象。

两个阈值来自配置项，源码默认值在 `src/config.c`：`zset-max-listpack-entries` 默认 **128**，`zset-max-listpack-value` 默认 **64**。

**写入：`zsetAdd(zobj, score, ele, in_flags, out_flags, newscore)`。** 输入标志是 `ZADD_IN_INCR`/`ZADD_IN_NX`/`ZADD_IN_XX`/`ZADD_IN_GT`/`ZADD_IN_LT`，输出标志是 `ZADD_OUT_NAN`/`ZADD_OUT_ADDED`/`ZADD_OUT_UPDATED`/`ZADD_OUT_NOP`。返回值 1 表示成功，0 表示出错（目前只可能是分值算出 NaN）。

开头第一件事是判 NaN：入参就是 NaN 时置 `ZADD_OUT_NAN` 直接返回 0——**与其它选项无关，NaN 优先报错**。之后按编码分派：

- **listpack**：用 `zzlFind` 按成员名找。找到时：NX → `ZADD_OUT_NOP` 返回；INCR 时把 `curscore` 加到 score 上并再判一次 NaN；GT/LT 条件不满足（`score >= curscore` 或 `score <= curscore`）→ NOP 返回；最后若 `score != curscore` 就 `zzlDelete` 再 `zzlInsert`（**listpack 改分值是删除重插**，置 UPDATED），否则什么都不做。没找到且不是 XX 时，先做溢出检查：`zzlLength(o->ptr)+1 > server.zset_max_listpack_entries`、`sdslen(ele) > server.zset_max_listpack_value`、或 `!lpSafeToAdd(o->ptr, sdslen(ele))`——任一成立就 `zsetConvertAndExpand(zobj, OBJ_ENCODING_BTREE, zsetLength(zobj) + 1)`，**然后落到 B+ 树分支继续**；否则 `zzlInsert` 并置 ADDED。XX 且成员不存在 → NOP。
- **skip list**：用 `dictFindLink` 一次拿到桶与链接。找到时语义与 listpack 一致，唯一区别是改分值走 `zslUpdateScore` 而不是删了重插——源码注释写明「we did not remove the original element from the hash table, so we don't need to update the dict - the node pointer stays the same」。没找到且不是 XX 时 `zslInsert(zs->zsl, score, ele)` 建跳表节点，再 `dictSetKeyAtLink(zs->dict, znode, &bucket, 1)` 挂进哈希表。
- **B+ tree**：用 `zbtreeFindForAdd` 找位置，找到则 `zbtreeUpdateScore`，没找到则 `zbtreeInsertNew`。语义与前两者一致。

**读分值：`zsetScore(zobj, member, score)`。** 三条路径：listpack 用 `zzlFind`；skip list 用 `dictFind` 拿到哈希表项、取出跳表节点的 `score` 字段；B+ 树用 `zbtreeScore`。对象或成员为 NULL、或成员不存在，一律返回 `C_ERR`。

**删成员：`zsetDel(zobj, ele)`。** listpack 走 `zzlFind` + `zzlDelete`；skip list 走 `zsetRemoveFromSkiplist`；B+ 树走 `zbtreeDelete`。`zsetRemoveFromSkiplist` 里的顺序有明确要求：先 `dictUnlink` 摘出哈希表项、`dictFreeUnlinkedEntry` 释放它，再 `zslDelete` 删跳表节点——源码注释写明「deleting from the skiplist actually releases the SDS string representing the element, which is shared between the skiplist and the hash table, so we need to delete from the skiplist as the final step」。

**算名次：`zsetRank(zobj, ele, reverse, output_score)`。** 名次是 **0 基**的（源码注释：第一元素名次 0，第二 1，直到 length-1）。三条路径的算法完全不同：

- **listpack**：`lpSeek` 从头开始线性遍历，用 `lpCompare` 逐个比对，`rank` 从 1 开始计数，找到后按 `reverse` 决定返回 `llen - rank` 还是 `rank - 1`。也就是紧凑编码下的名次是 **O(n) 线性扫描**。
- **skip list**：先 `dictFind` 拿节点，再 `zslGetRankByNode` 算名次。这个函数从节点**向后**走，累加每跳的 span，最后用 `zsl->length - distance_to_end` 反推名次，源码注释说这是 O(log N) 均摊且「避免昂贵的字符串比较」。
- **B+ 树**：`zbtreeRank`。

成员不存在时统一返回 **-1**。

**批量插入后的回调编码。** `zsetConvertAfterBulkInsert` 在 RDB 载入与 ZUNIONSTORE 批量插入后被调用：若结果集合能装进 listpack 就 `zsetConvert(zobj, OBJ_ENCODING_LISTPACK)`；否则若当前还是 skip list 就 `zsetConvert(zobj, OBJ_ENCODING_BTREE)`。

## 关键取舍

**源码事实：** 本分支的大集合默认编码是 **B+ 树**，跳表是兼容旧数据/旧 RDB 的遗留编码，但读写删与名次在三条路径上都被完整支持。**源码事实：** 同一语义在三种编码下有三种实现：改分值在 listpack 上是「删了重插」，在 skip list 上是 `zslUpdateScore`（节点指针不变，不用动哈希表），在 B+ 树上是 `zbtreeUpdateScore`。**源码事实：** listpack 下的名次是线性扫描 O(n)，skip list 与 B+ 树是 O(log N)。**源码事实：** skip list 删除时哈希表与跳表的摘除顺序不可颠倒，因为两者共享同一份成员 SDS。**源码事实：** `zsetTypeMaybeConvert` 的 hint 是「本次命令的参数对数」，可能重复也可能被 NX/XX/GT/LT 跳过，因此转换后需要 `zbtreeShrinkToFit` 收回多预分配的槽位。

**架构推论：** 用「同语义、三实现」的方式兼容旧编码，让升级路径与降级路径都能被显式触发（RDB 载入路径上的 `zsetConvertAfterBulkInsert` 就是显式降级的例子）；代价是同一语义要在三处分别维护，改动语义时容易漏改其中一处——本分支跳表与 B+ 树并存正是这种代价的体现。**架构推论：** listpack 编码保留线性名次扫描，是因为小集合上 O(n) 与 O(log n) 差距不大，而省掉一整套索引结构更划算；这解释了为什么阈值定在 128 这个量级。**架构推论：** ZUNION/ZINTER 的无 STORE 变体仍需要一条真实链接的跳表来产出回复，源码注释写明 STORE-less 变体「must keep getting a real, linked skiplist」，因此这一分支无法简单绕过跳表——这也是跳表在本分支不能被立刻删除的原因。**架构推论：** 用「参数个数」而不是「实际新增数」作为转换提示，是拿「可能过度升级」换「插入路径上不必反复检查长度」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_zset.c` | `zsetAdd` | 加成员/改分值：按编码分派实现 NX/XX/INCR/GT/LT 语义，listpack 装不下时升级编码 |
| `src/t_zset.c` | `zsetScore` | 读分值：三条编码路径各走各的查找，找不到返回 C_ERR |
| `src/t_zset.c` | `zsetDel` | 删成员：listpack 删除重插、skip list 摘哈希表与跳表、B+ 树直接删 |
| `src/t_zset.c` | `zsetRank` | 算名次：listpack 线性扫描、skip list 走 span 反推、B+ 树走树索引，不存在返回 -1 |
| `src/t_zset.c` | `zsetTypeMaybeConvert` | 编码升级判据：listpack 超过条目阈值就升成 B+ 树，其余编码直接返回 |

## 参考

- [Redis 源码：t_zset.c](https://github.com/redis/redis/blob/unstable/src/t_zset.c)
- [Redis 文档：有序集合数据类型](https://redis.io/docs/latest/develop/data-types/sorted-sets/)
- [Redis 命令：ZADD](https://redis.io/docs/latest/commands/zadd/)
- [Redis 命令：ZRANGE](https://redis.io/docs/latest/commands/zrange/)

## 与主核 / AI 的关系

有序集合是 Redis 里唯一一个「既要按分值排序、又要按成员名唯一查找」的容器——跳表管排序、哈希管按名查找，两份结构指向同一个节点。这是主核做向量 TopK 时最值得借鉴的形态：按相似度排序走有序索引，按 chunk id 去重走哈希索引，两者共享同一个节点指针，避免了「排序结构里再存一份 id 列表」的双写。

但本分支的走向值得警惕：Redis 把通用编码换成了打包 B+ 树，跳表退成兼容编码。这暗示**跳表在高并发写 + 大数据量 + 范围扫描**的组合下并不是最优解（跳表的指针追逐对缓存不友好、节点分配分散）。主核若要自研有序索引，不应默认「Redis 用跳表所以我们也用跳表」，而应先想清楚自己的访问模式是「范围扫描为主」还是「随机点查为主」。

AI 侧用有序集合存「文档 → 相似度分数」时有一个具体的坑：紧凑编码（listpack）下 ZRANK 是 O(n) 线性扫描，集合一大，排名查询会直接变成延迟尖刺。要么把 `zset-max-listpack-entries` 调大让集合停在紧凑编码之外，要么干脆不用有序集合做排名——Redis 的 ZRANGE 在紧凑编码下同样是线性扫描。
