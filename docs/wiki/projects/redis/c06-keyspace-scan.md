---
project: redis
feature: keyspace-scan
page_type: feature
schema_version: 1
refs:
  - path: src/db.c
    symbols: [scanGenericCommand]
---

# 键空间游标扫描

## 一句话职责

Redis 的 SCAN 家族靠一个不透明的游标把「遍历整个键空间」拆成多次可中断的调用：每次尽量返回 `COUNT` 个元素，超预算就带着新游标返回，调用者重复调用直到游标归零。

## 机制是什么

`src/db.c` 的 `scanGenericCommand` 是 SCAN、HSCAN、SSCAN、ZSCAN 的共同实现。函数的第二个参数 `o` 决定遍历对象：`o` 为 NULL 时遍历当前数据库的键空间；否则遍历这个集合对象内部的元素。它的开头有一句断言：对象必须是 NULL，或者是集合、有序集合、哈希三者之一。

**游标是调用方无法解释的黑盒。** 命令的游标参数由 `parseScanCursorOrReply` 解析——不是合法数字就回 `invalid cursor`。`scanGenericCommand` 拿到的游标只用于「从哪里继续」，返回给客户端时原样回传。

**`COUNT` 是提示而不是保证。** 源码注释写得很清楚：遍历时把最大迭代次数设为 `COUNT` 的十倍，「so if the hash table is in a pathological state (very sparsely populated) we avoid to block too much time at the cost of returning no or very few elements」——宁可少返回几个，也不让一次调用卡住。

**不同编码走不同遍历策略。** 源码第二步的注释给出判据：「if the object is encoded with a listpack, intset, or any other representation that is not a hash table, we are sure that it is also composed of a small number of elements. So to avoid taking state we just return everything inside the object in a single call, setting the cursor to zero to signal the end of the iteration.」也就是说紧凑编码一次性返回全部、游标归零，只有哈希表形态才做增量游标扫描。

本页只讲游标推进与结果裁剪。哈希表渐进 rehash 的实现本身在 e01，游标在 rehash 期间如何保持安全在 e02，键匹配之后的命令语义不在本页。

## 怎么实现的

**选项解析。** 第一步逐个解析参数。`COUNT` 默认值是 10，小于 1 直接回语法错误。`MATCH` 记下模式串与长度，并且有特判：`use_pattern = !(patlen == 1 && pat[0] == '*')`——模式正好是 `*` 时等价于关掉匹配。`TYPE` 只在 `o == NULL`（键空间扫描）时允许，类型名经 `getObjectTypeByName` 解析；**未知名当前不报错**——源码里那句回错误的语句被注释掉了，旁边留着 `TODO: uncomment in redis 8.0`。`NOVALUES` 只允许用于 HSCAN，否则回 `NOVALUES option can only be used in HSCAN`。

**选取遍历载体。** 哈希表形态有三条：`OBJ_SET` 且编码是 `OBJ_ENCODING_HT`、`OBJ_HASH` 且编码是 `OBJ_ENCODING_HT`、`OBJ_ZSET` 且编码是 `OBJ_ENCODING_SKIPLIST`——有序集合取的是跳表里的那个字典 `zs->dict`。键空间扫描（`o == NULL`）则由 kvstore 提供多字典的遍历。

**哈希表形态的游标推进。** `maxiterations` 的算式是 `(count > LONG_MAX / 10) ? LONG_MAX : count * 10`。扫描上下文 `scanData` 装着：结果容器 `keys`、被扫对象 `o`、目标类型 `type`、模式串 `pattern`、已采样计数 `sampled`、`no_values`、类型名 `typename`、以及数据库 `db`。

集群模式下有一处削减：若 `o == NULL`、用了模式匹配、且启用了集群，就用 `patternHashSlot(pat, patlen)` 推断出这个模式只可能落在哪个槽——注释写的是「A pattern may restrict all matching keys to one cluster slot.」；`patternHashSlot` 在 `src/cluster.c`，只有当模式不含通配符、不含转义、且（要么整串匹配一个键、要么带非空哈希标签）时才返回具体槽号，否则返回 -1 表示不能削减。

推进循环是 `do { cursor = kvstoreScan(...) } while (cursor && maxiterations-- && data.sampled < count)`——键空间扫描调 `kvstoreScan(c->db->keys, cursor, onlydidx, scanCallback, scanShouldSkipDict, &data)`，集合内扫描调 `dictScan(ht, cursor, scanCallback, &data)`。循环继续的条件有三个：游标还没归零、迭代预算没用完、采样数还没达到 `count`。

`kvstoreScan`（`src/kvstore.c`）的游标是复合的：注释写明「During dictionary traversal, 48 upper bits in the cursor are used for positioning in the HT. Following lower bits are used for the dict index number」——高位是哈希表内的位置，低位是字典序号。当前字典扫完时，`kvstoreScan` 找下一个非空字典继续；找不到就返回 0，表示遍历结束。

**结果裁剪：扫描回调。** `scanCallback` 对每个被访问到的条目做四件事。

第一，**模式匹配**：`stringmatchlen` 不匹配就跳过——注意模式是在**取到键之后**才匹配的，过滤发生在遍历过程中而不是遍历之后。

第二，**过期检查**，只对键空间扫描生效：`expireIfNeeded(data->db, NULL, kv, 0) != KEY_VALID` 就跳过。源码注释点明这一条用的是 kvobj 而不是 robj，「to avoid robj creation」，并且强调检查顺序——先判过期，再判类型。

第三，**类型过滤**，也只对键空间扫描生效：若指定了类型名，未知类型（`LLONG_MAX`）会把所有键都跳过；已知类型则用 `objectTypeCompare` 比对，不匹配就跳过。

第四，**集合内部元素的过期**：集合成员用 `sEntryIsExpired` 判、哈希字段用 `hEntryIsExpired` 判，过期的成员/字段被跳过。

最后 `data->sampled++`——这个计数是**访问过的条目数**，不是返回给客户端的条数。循环条件里的 `data.sampled < count` 用的是它，所以带模式匹配时返回的条目数可能远少于 `COUNT`。

**紧凑编码的遍历。** 非哈希表形态各有分支：

- **集合（紧凑编码）**：回复长度固定发 2（游标 + 数组），游标恒为 0。长度是否延迟取决于 `deferred_len = use_pattern || setTypeHasExpireSupport(o)`——注释解释：带模式的集合长度未知；带成员过期的集合里「过期成员尚未被移除，所以回复长度事先也不知道」。迭代用 `setTypeInitIterator(&si, o, SET_ITER_SKIP_EXPIRED)` 直接跳过过期成员。
- **有序集合的 BTREE 编码**：走 `zbtreeScan(o->ptr, cursor, count, zsetBtreeScanReply, &data)`，这一支**是**带游标的，返回的下一个游标由 `zbtreeScan` 给出；回复可能走流式（`data.streaming`）或缓冲后统一发出两种路径。
- **哈希或有序集合的 LISTPACK 编码**：直接用 `lpFirst`/`lpGet`/`lpNext` 走一遍 listpack，游标恒为 0；哈希返回字段与值两列（`NOVALUES` 时只返回字段）。
- **哈希的 LISTPACK_EX 编码**：同样遍历 listpack，但每个字段后面跟着一个过期时间戳，`lpGetIntegerValue` 取出后用 `hashTypeIsExpired` 判断；因为可能有字段被过期，**长度总是延迟给出**。
- **哈希的模板编码**（`OBJ_ENCODING_TMPL_LP` 与 `OBJ_ENCODING_TMPL_ARRAY`）：用 `hashTypeInitIterator`/`hashTypeNext` 迭代，游标恒为 0。
- 落不到任何一支就 `serverPanic("Not handled encoding in SCAN.")`。

**回复格式。** 哈希表形态的回复固定是长度为 2 的数组：先回新游标，再回元素数组。有序集合的元素数组长度是元素数的两倍——每个成员后面跟着它的分值（分值直接从跳表节点上取）。键空间扫描与集合、哈希的元素数组则是每元素一项。

**键空间扫描里被跳过的字典。** `scanShouldSkipDict` 转调 `accessKeysShouldSkipDictIndex`，后者判 `clusterCanAccessKeysInSlot`——原子槽位迁移期间，正在被导入的槽里的键处于中间态，不能访问，因此整个字典被跳过。这个回调同时被 `dbRandomKey`、`keysCommand` 与 `scanCommand` 复用。

**真实调用方。** `src/db.c` 的 `scanCommand`（SCAN，传入 `o = NULL`）、`src/t_hash.c` 的 `hscanCommand`、`src/t_set.c` 的 `sscanCommand`、`src/t_zset.c` 的 `zscanCommand` 都调用它。后三者都先用 `lookupKeyReadOrReply` 取到对象并用 `checkType` 校验类型，然后在调用前后用 `updateSlotAllocSize` 修正内存追踪的槽位占用（因为遍历过程可能触发过期删除，键的分配大小会变）。注意 `scanGenericCommand` 的注释只写了「This command implements SCAN, HSCAN and SSCAN commands.」，但 ZSCAN 实际也走这里——这条注释与实际调用者不一致。

**退化路径。** 几种情况会让一次 SCAN 返回零个元素却带着非零游标：模式过滤掉了全部采样到的键；类型过滤掉了全部采样到的键；采样到的键都已过期。调用者必须靠「游标是否归零」判断是否结束，不能靠「本次返回了几个」。反过来，一次调用也可能返回**多于** `COUNT` 的元素——紧凑编码分支是一次性全量返回，哈希表分支在 `data.sampled` 达标前可能已经收集了超过 `count` 个匹配项。

## 关键取舍

**源码事实：** `COUNT` 是提示，实际最多做十倍迭代，返回条数既可能少于 `COUNT` 也可能多于。**源码事实：** `MATCH` 为 `*` 时被当作关闭匹配。**源码事实：** `TYPE` 只在键空间扫描有效，未知名当前不报错（错误分支被注释，留有 8.0 的 TODO）。**源码事实：** `NOVALUES` 只允许 HSCAN。**源码事实：** 紧凑编码一次性返回并把游标置零，只有哈希表与 BTREE 编码走增量游标。**源码事实：** 过期与类型过滤发生在遍历过程中，不是遍历之后。**源码事实：** 集群下若模式可归约到单个槽，只扫那个字典。**源码事实：** 迁移中的槽整字典跳过。

**架构推论：** 游标不透明是有意的设计——调用者无法解释游标，Redis 就能在不破坏客户端协议的前提下替换底层遍历策略（哈希表、BTREE、listpack 各走各的），这是把「遍历键空间」做成稳定 API 的前提。代价是调用者无法把游标持久化后跨重启继续，也无法并发地用多个游标保证不重不漏。**架构推论：** 过滤在遍历过程中进行而不是遍历完再筛，是为了不让一次 SCAN 拉回整个键空间再丢掉大部分；代价是「返回条数」与「采样条数」不再等价，调用方无法用返回条数推断进度。**架构推论：** 紧凑编码一次性返回并把游标置零，是把「小对象不值得维护状态」这一判断编码进协议——它让 HSCAN/SSCAN 在小集合上变成 O(n) 的一次性操作，代价是这类命令在大集合退化到紧凑编码时会有一次明显的耗时尖峰。**架构推论：** 集群下按模式归约槽位，是把「模式扫描」从全节点扫描收敛到单槽扫描；但这个归约只在模式不含通配符时成立，含通配符时每个节点仍要扫自己的槽——主核做分布式扫描时，同样的「能否把谓词下推成键空间裁剪」问题会以类似形式出现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/db.c` | `scanGenericCommand` | SCAN 家族的共同实现：选项解析、按编码分派遍历方式、游标推进、模式/类型/过期三重过滤与回复组装 |

## 参考

- [Redis 源码：db.c](https://github.com/redis/redis/blob/unstable/src/db.c)
- [Redis 文档：SCAN](https://redis.io/docs/latest/commands/scan/)
- [Redis 文档：键空间遍历](https://redis.io/docs/latest/develop/use/keyspace/)
- [Redis 命令：HSCAN](https://redis.io/docs/latest/commands/hscan/)

## 与主核 / AI 的关系

「不透明游标 + 采样推进 + 过滤前置」是主核做全表或全向量集合遍历时最值得直接借用的形状：不透明游标让底层存储结构可以自由更换遍历实现；过滤前置避免把无关数据拉回；`COUNT` 作为提示而非承诺，让调用方无法依赖精确的进度。

主核是多写者引擎，这一点与 Redis 的差别最大：Redis 的 SCAN 在单线程里遍历一个近乎静止的哈希表，返回的集合是一个近似快照；主核的扫描要与并发写入共存，扫描期间新写入的记录是否可见、已删除的记录是否会被返回，都需要主核自己定义可见性规则——是按扫描开始时的快照、还是按每条记录各自的事务可见性判定，两种语义给出的结果集合完全不同。

AI 侧用 SCAN 遍历向量集合做批量重算或清理时，要特别注意 Redis 的两条语义：`COUNT` 不保证返回条数，所以按返回条数估算剩余工作量会失真；而过滤在遍历中发生，被模式或类型过滤掉的键不会出现在任何一次返回里，因此「扫完了」只能由游标归零来判定，不能靠「这次返回为空」判定——后者在过滤掉全部键时会在同一游标上反复空转。
