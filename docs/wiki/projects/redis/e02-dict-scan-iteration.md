---
project: redis
feature: dict-scan-iteration
page_type: feature
schema_version: 1
refs:
  - path: src/dict.c
    symbols: [dictScan, dictGetIterator, dictNext, dictUnlink]
---

# 哈希表的反向遍历与安全游标

## 一句话职责

dict 提供两套遍历：一套是**无状态游标扫描** `dictScan`——把进度编码进一个整数交给调用者保存，因此 rehash 期间表变大变小都不用重头再来；另一套是**有状态迭代器** `dictGetIterator` / `dictNext`——逐桶推进，可以在遍历途中安全地把元素摘出去（`dictUnlink`）。

## 机制是什么

**扫描游标是一个反向二进制计数器。** `src/dict.c` 的注释把算法讲得很清楚：起始时用游标 0 调函数，函数做一步迭代并返回新游标；新游标为 0 表示遍历结束。推进方式不是普通的 `v++`，而是「把游标的位反转、加一、再反转回来」。注释点明这套算法由 Pieter Noordhuis 设计。

为什么必须反转？因为哈希表大小恒为 2 的幂，键的位置由 hash 的低位决定。若表从 16 涨到 64（掩码从 4 位变 6 位），原来落在桶 `1100` 的键可能落到 4 个新桶里的任意一个。反转计数器让**高位先变**：已经扫过的低位组合不会被重复扫，表变大时游标可以直接继续往前，不用从头再来；表变小时同理，已经穷尽的高位变体不会被重扫。

**两张表怎么合并扫。** 注释解释：rehash 期间确实有两张表，但总是先扫小表，再把小表当前游标在**大表里的所有展开位置**都扫一遍——大表在这些位置上的桶与小表的桶是同一个键集合的扩展，这样就把两张表的问题化约成了一张表。

**迭代器是另一套东西。** `dictIterator` 记录当前表号 `table`、桶下标 `index`、当前条目 `entry`、下一个条目 `nextEntry`，以及两个模式位：`safe`（安全迭代，允许遍历期间修改 dict）与 `fingerprint`（普通迭代在收尾时校验 dict 是否被改过）。

**删键分两步。** `dictUnlink` 只把元素从桶链上摘下来，**不释放键、值与条目本身**；释放要另调 `dictFreeUnlinkedEntry`。源码注释给出动机：如果想先取出值再删，普通做法要查两次（先 `dictFind` 再 `dictDelete`），而 `dictUnlink` 让「摘链 → 用值 → 释放」变成一次查找。

本页只讲遍历与删键路径。rehash 的推进策略在 e01；SCAN 命令本身的游标语义与结果裁剪在 c06。

## 怎么实现的

**`dictScan` 的骨架。** `dictScan` 本身只有一行，转调 `dictScanDefrag(d, v, fn, NULL, privdata)`——后者多带一组碎片整理回调，`dictScan` 传的是 NULL。

`dictScanDefrag` 的流程：

1. `dictSize(d) == 0` 直接返回 0（空表遍历立即结束）。
2. `dictPauseRehashing(d)`——源码注释写明原因：「This is needed in case the scan callback tries to do dictFind or alike.」回调里若做查找，查找会顺带推进 rehash，把正在扫的桶搬走。
3. **不在 rehash**：只扫表 0 的 `v & m0` 桶；然后 `v |= ~m0` 把掩码之外的位全置 1（注释：「Set unmasked bits so incrementing the reversed cursor operates on the masked bits」），再做 `rev(v)` → `v++` → `rev(v)`。
4. **正在 rehash**：先保证 `htidx0` 是较小的表、`htidx1` 是较大的表（源码里会交换）。扫小表的 `v & m0` 桶后，进入 do-while：扫大表的 `v & m1` 桶，同样做 `v |= ~m1` 加反转加一，循环条件是 `v & (m0 ^ m1)`——即「大小两表掩码不同的那些位还没被穷举完」。注释解释这一步：「Continue while bits covered by mask difference is non-zero」。
5. `dictResumeRehashing(d)`，返回新游标 `v`。

**`dictScanDefragBucket` 如何允许回调删元素。** 它遍历桶链时维护一个 `plink`（指向「上一个条目的 next 槽」或桶槽本身）。每次调 `fn(privdata, de, plink)` 之后检查：如果 `*plink` 仍然指向刚访问过的 `de`，说明回调没有删它，就把 `plink` 前进到 `&de->next`；否则说明回调已经把这条链摘了，`plink` 保持不动，指向回调留下的位置。源码注释写得很直白：「if `*plink` still pointing to 'de', then it means that the visited item wasn't deleted by fn()」。

**`dictGetIterator` 与 `dictNext`。** `dictGetIterator` 分配一个 `dictIterator` 并调 `dictInitIterator`，初值是 `table = 0`、`index = -1`、`safe = 0`、`entry = NULL`、`nextEntry = NULL`、`prefetch = 0`。`dictGetSafeIterator` 在此基础上把 `safe` 置 1。

`dictNext` 的循环体分两支：

- **`entry == NULL`（要取下一个桶）**：若是第一次取（`index == -1 && table == 0`），安全模式下调 `dictPauseRehashing`，普通模式下记录 `iter->fingerprint = dictFingerprint(iter->d)`。若正在 rehash，把 `index` 设为 `d->rehashidx - 1`——源码注释写明「skip the rehashed slots in table[0]」，即表 0 中已经搬走的那段桶直接跳过。然后 `index++`；越过当前表的末尾时，若正在 rehash 且还在表 0，就切到表 1 并把 `index` 归零，否则结束遍历。最后取 `ht_table[table][index]` 作为当前条目。
- **`entry != NULL`（还在同一条链上）**：当前条目换成先前保存的 `nextEntry`。

取到条目后立刻 `iter->nextEntry = dictGetNext(iter->entry)`，源码注释解释原因：「We need to save the 'next' here, the iterator user may delete the entry we are returning.」——**先记住下一个，再把当前交出去**，这样调用方删掉当前条目也不会让迭代器踩空。

**预取。** `dictIteratorEnablePrefetch` 打开后，`dictNext` 每次换桶时调 `dictIteratorPrefetchAhead`：`DICT_ITER_PREFETCH_AHEAD` 是 16，于是预取当前下标 +32 处的条目、+16 处的键、+8 处的值（值地址由 dictType 的 `prefetchEntryValue` 给出）。注释写明这是给「full scans that read every entry, like the RDB save」用的。

**收尾校验：`dictResetIterator`。** 若迭代器不是「一步都没走」的状态，安全模式下调 `dictResumeRehashing` 恢复搬迁；普通模式下断言 `iter->fingerprint == dictFingerprint(iter->d)`。`dictFingerprint` 把两张表的指针、指数、元素数共 6 个整数按顺序做滚动哈希（注释说用 Tomas Wang 的 64 位整数哈希），并解释这样「同一组整数换个顺序会（很可能）哈希到不同的数」。

**`dictUnlink` 与 `dictGenericDelete`。** `dictUnlink(d, key)` 就是 `dictGenericDelete(d, key, 1)`，第三个参数 `nofree = 1`。`dictGenericDelete` 的流程：空表直接返回 NULL；算 hash 得桶号；`_dictRehashStepIfNeeded` 推进一格 rehash；然后从表 0 开始扫链，若在表 0 且 `idx < rehashidx` 就跳过（桶已搬走）；找到匹配的键（先比指针，再比 `keyCompare`）就把条目从链上摘下——有前驱就改前驱的 next，否则改桶槽；`nofree` 为假时才调 `dictFreeUnlinkedEntry`；`ht_used[table]--`；再调 `_dictShrinkIfNeeded` 看是否该收缩。

`dictFreeUnlinkedEntry(d, he)` 做三件事：释放键、释放值、释放条目本身（`entryIsKey(he)` 为假时才释放，因为直接入槽的键没有独立条目）。它接受 `he == NULL`。

**真实 caller。**

- `dictScan` 的真实调用方：`src/db.c` 的 `scanGenericCommand`（集合内部扫描）、`src/kvstore.c` 的 `kvstoreScan`（键空间扫描）、`src/module.c` 的模块扫描回调、`src/t_hash.c` 的 `fieldsLpReclaimCollect`（哈希字段回收）。
- `dictGetIterator` / `dictInitIterator` 的真实调用方遍布各处，代表性的有：`src/t_set.c` 的集合迭代器、`src/t_zset.c` 的有序集合元素索引迭代、`src/rdb.c` 的哈希模板按字段索引、`src/pubsub.c` 的通道与模式遍历、`src/sentinel.c` 的实例表遍历、`src/redis-cli.c` 的多种客户端侧遍历。
- `dictGetSafeIterator` / `dictInitSafeIterator` 的真实调用方是**遍历期间可能改 dict** 的那些：`src/cluster_legacy.c` 的集群节点表、`src/config.c` 的配置表、`src/db.c` 的阻塞键表（`db->blocking_keys`）、`src/pubsub.c` 的客户端订阅表、`src/multi.c` 的 `watched_keys`、`src/latency.c` 的延迟事件表。
- `dictUnlink` 的真实调用方：`src/eval.c`（删 Lua 脚本缓存项）、`src/functions.c`（删函数库）、`src/module.c`（删模块配置队列项）、`src/pubsub.c`（退订时删通道 / 删模式）、`src/t_hash.c`（哈希字段集的反向查找表）、`src/t_set.c`、`src/t_zset.c`（有序集合的元素索引）。

**退化与回退路径。**

- **扫描可能重复返回元素。** 源码注释把这一条列为该设计的缺点：「It is possible we return elements more than once. However this is usually easy to deal with in the application level.」所以 `dictScan` 的调用方必须能容忍重复。
- **扫描回调删元素是被支持的。** `dictScanDefragBucket` 的 `*plink == de` 判断就是为此存在的；但删的必须是**当前正在访问的**那一条，删其他条目会破坏链指针语义。
- **普通迭代器 + 改 dict = 断言失败。** `dictResetIterator` 里 `assert(iter->fingerprint == dictFingerprint(iter->d))`——迭代期间 dict 被改过（扩容、增删）指纹就变了，程序会在收尾时断言失败。安全迭代器用 `dictPauseRehashing` / `dictResumeRehashing` 换掉了这条断言，代价是遍历期间 rehash 完全停摆。
- **`dictUnlink` 之后必须配一次 `dictFreeUnlinkedEntry`。** 源码注释把配对写进了示例：`entry = dictUnlink(dictionary, entry); ... dictFreeUnlinkedEntry(entry);`——只摘不释放就是内存泄漏。
- **删空表的兜底。** `dictGenericDelete` 在 `dictSize(d) == 0` 时直接返回 NULL，`dictUnlink` 因此返回 NULL，调用方拿到 NULL 要能正常处理。

## 关键取舍

**源码事实：** 扫描游标是反转后的二进制计数器，保证所有元素至少被返回一次，但可能重复返回。**源码事实：** 扫描期间 rehash 被暂停，回调若做查找也不会把正在扫的桶搬走。**源码事实：** rehash 期间扫描先扫小表，再穷举大表里对应的展开位置。**源码事实：** 迭代器在交出当前条目前先记住下一个，调用方删当前条目不会让迭代器踩空。**源码事实：** 普通迭代器收尾时用指纹断言校验 dict 未被改动，安全迭代器改为暂停 rehash。**源码事实：** `dictUnlink` 只摘链不释放，释放是独立的一步。

**架构推论：** 把扫描进度编码成整数而不是返回迭代器对象，是为了让 SCAN 协议能跨多次网络往返、甚至跨进程持久化——代价是**重复返回无法避免**，调用方必须自己去重。**架构推论：** 用「反转计数器」而不是「记录上次访问的键」来实现可续扫，是因为后者在 rehash 期间无法保证不漏——键可能已经搬到新表的另一个位置；反转计数器把「表大小变化」降级成「掩码变化」，是这套设计的核心洞察。**架构推论：** `dictScan` 暂停 rehash 而迭代器不暂停（除非是 safe 模式），说明两者对「遍历期间允许做什么」的假设不同：扫描是短促的、回调受限的；迭代器是长程的、可能要改数据的。**架构推论：** 指纹断言是一种「把运行期契约变成可执行检查」的做法——代价是调试构建与发布构建行为不同（断言在 release 下被编译掉，契约就没人守了）。主核是多写者引擎，这套「遍历期间暂停结构变更」的做法不可照搬：主核的扫描要面对并发写者，必须自己定义可见性规则；但「先记住下一个再交出当前」这条防御式约定值得直接抄——它是并发场景下防止迭代器踩空的标准做法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/dict.c` | `dictScan` | 无状态游标扫描的入口，用反转计数器推进，支持 rehash 期间不漏不重地覆盖所有元素 |
| `src/dict.c` | `dictGetIterator` | 分配并初始化迭代器，返回可供 `dictNext` 推进的遍历句柄 |
| `src/dict.c` | `dictNext` | 迭代器推进：跨桶前进、rehash 时跳过已搬走的桶、在交出当前条目前预存下一个 |
| `src/dict.c` | `dictUnlink` | 把元素从桶链上摘下但暂不释放，交给调用方取用值后再释放 |

## 参考

- [Redis 源码：dict.c](https://github.com/redis/redis/blob/unstable/src/dict.c)
- [Redis 文档：SCAN](https://redis.io/docs/latest/commands/scan/)
- [Redis 文档：键空间遍历](https://redis.io/docs/latest/develop/use/keyspace/)

## 与主核 / AI 的关系

这一页最值得主核带走的是**「先记住下一个，再把当前交出去」**这条约定：主核的向量索引遍历必然与删除、压缩、更新并发，如果迭代器在交出当前记录之后才去取「下一条」，那么并发删除就会让迭代器拿到已释放的内存。这是并发数据结构里最常见的一类缺陷，而 Redis 用一行「先存后交」就把它消掉了。

第二值得带走的是**「遍历期间结构变更被显式冻结」**这个思路在主核里的对应物：主核不能冻结写入，但可以给扫描一个**快照或版本号**，让扫描看到的是一致的视图，并把「扫描期间的并发写入不保证被看到」写进契约。

第三是**重复返回的容忍度**。Redis 的 SCAN 明确允许重复，主核的批量遍历接口若追求「不重不漏」，就必须引入快照或代价更高的版本链；AI 侧若用 Redis 遍历向量集合做批量重算或清理，要按「至少一次」而不是「恰好一次」来设计幂等，否则并发写入期间会漏算或重复算。
