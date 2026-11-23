---
project: redis
feature: keyspace-read-lookup
page_type: feature
schema_version: 1
refs:
  - path: src/db.c
    symbols: [lookupKeyRead, lookupKeyReadWithFlags, lookupKeyWrite]
---

# 键的读侧查找

## 一句话职责

Redis 把「按键找到值」收敛到一组查找入口上：命中时顺手做过期判定、访问时间更新与命中计数，未命中时补一条 `keymiss` 通知——读路径上全部副作用都在这一次查找里完成。

## 机制是什么

`src/db.c` 对外暴露三个查找入口：`lookupKeyRead`（纯读）、`lookupKeyReadWithFlags`（纯读且可带标志）、`lookupKeyWrite`（写意图）。三者的实质差别只有一行标志位，函数体都落到同一个内部实现 `lookupKey` 上。

`lookupKey` 的注释把它的副作用列成四条：

1. 键到达 TTL 时被过期掉；
2. 键的最近访问时间被更新；
3. 全局 keys hits/misses 统计被更新（`INFO` 里可见）；
4. 开启键空间通知时，未命中会触发 `keymiss` 事件。

返回值是 `kvobj *`——本版本把键和值统一成一个键值对象。找不到、或键在逻辑上已过期且被判为无效时，返回 NULL。

读与写的差别由 `LOOKUP_WRITE` 标志承载。全部标志定义在 `src/server.h`：`LOOKUP_NOTOUCH`（不更新访问时间）、`LOOKUP_NONOTIFY`（未命中不发通知）、`LOOKUP_NOSTATS`（不记命中统计）、`LOOKUP_WRITE`（准备写这个键）、`LOOKUP_NOEXPIRE`（做过期检查但不删键）、`LOOKUP_ACCESS_EXPIRED`（允许取到已过期的键）、`LOOKUP_ACCESS_TRIMMED`（允许取到正在被裁剪的槽位里的键），以及把前四个关掉的组合 `LOOKUP_NOEFFECTS`。

本页只讲读路径上的查找、过期判定与命中统计。插入与覆盖走 c02，主动扫描过期键走 c04，键空间通知的投递走 i02。

## 怎么实现的

**三个入口的差别。** `lookupKeyReadWithFlags` 先 `serverAssert(!(flags & LOOKUP_WRITE))`——断言调用方没有塞进写意图——再 `lookupKey(db, key, flags, NULL)`；`lookupKeyRead` 就是 `lookupKeyReadWithFlags(db, key, LOOKUP_NONE)`。`lookupKeyWrite` 转手给 `lookupKeyWriteWithFlags`，后者做的是 `lookupKey(db, key, flags | LOOKUP_WRITE, NULL)`。源码注释把两条路径的语义写明：读入口「不应在拿到对象之后再写这个键」，写入口则是「如果需要，顺手把到期的键过期掉」。

**查找本身。** `lookupKey` 第一步调 `dbFindByLink` 在主键空间 `db->keys` 里找。该函数的注释写明它一次返回两样东西：命中时 link 指向该键的条目，未命中时 link 指向「这个键应该被插进去的那只桶」，哈希表尚未分配时 link 为 NULL。也就是说一次哈希查找同时给出了「找到了什么」和「要插到哪里」。

**过期判定。** 命中后按标志拼出 `expire_flags`：`LOOKUP_WRITE` 且当前不是只读副本时加 `EXPIRE_FORCE_DELETE_EXPIRED`；`LOOKUP_NOEXPIRE` 加 `EXPIRE_AVOID_DELETE_EXPIRED`；`LOOKUP_ACCESS_EXPIRED` 加 `EXPIRE_ALLOW_ACCESS_EXPIRED`；`LOOKUP_ACCESS_TRIMMED` 加 `EXPIRE_ALLOW_ACCESS_TRIMMED`。然后交给 `expireIfNeeded`，返回非 `KEY_VALID` 就把结果置 NULL 并把 link 清空。

`expireIfNeeded` 的返回值语义写在 `src/db.c` 的注释里：`KEY_VALID` 表示键还有效；`KEY_EXPIRED` 表示已过期但没删；`KEY_DELETED` 表示已过期且已删；`KEY_TRIMMED` 表示键在槽位迁移的裁剪任务里。

**只读副本不删过期键。** `lookupKey` 先算 `is_ro_replica = server.masterhost && server.repl_slave_ro`，只有「带写意图且不是只读副本」才给出 `EXPIRE_FORCE_DELETE_EXPIRED`。源码注释给的理由是：在只读副本上强制删除会让副本与主节点不一致，因此禁止；但可写副本上必须允许，好让写命令的行为保持一致。注释还点出一个例外——写意图标志可能在只读命令执行期间被置上，因为命令触发的事件可能让模块追加写操作。

**访问时间更新。** 条件是「没带 `LOOKUP_NOTOUCH`」且「当前没有活跃子进程」（`hasActiveChildProcess()`，注释的理由是这时更新会引发 copy-on-write 狂潮）。更新策略由 maxmemory-policy 决定：带 LFU 标志走 `updateLFU`；否则，**只有不是 LRM 策略**时才写 `val->lru = LRU_CLOCK()`。注释明确写了「LRM policy should NOT update timestamp on reads」。

还有一条隐式注入：若当前客户端带 `CLIENT_NO_TOUCH`，且当前执行的命令不是 `touchCommand`（判断条件是 `server.executing_client->cmd->proc != touchCommand`），就临时补上 `LOOKUP_NOTOUCH`。语义是「这个客户端的访问不许影响 LRU」，而 `TOUCH` 命令恰恰就是要改 LRU 的，所以要给它开后门。

**统计与通知。** 命中时 `server.stat_keyspace_hits++`；未命中时先 `notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id)`，再 `server.stat_keyspace_misses++`。两处都被 `!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE))` 守住——写路径不计入读侧的命中统计。源码在这两处都留了 TODO：写侧要用独立的统计口径和独立的事件。

**退化路径。** 「逻辑上已过期」不等于「查不到」，但对外表现一致：`expireIfNeeded` 返回 `KEY_EXPIRED`（副本上，或带 `EXPIRE_AVOID_DELETE_EXPIRED`）时，键其实还留在字典里，查找却返回 NULL，同时 link 被清空。`lookupKeyWriteWithLink` 的注释把四种返回组合列清楚了：找到且有效 → 返回 kvobj、link 是键的条目；不存在 → 返回 NULL、link 是应插入的桶；已过期或被裁剪 → 返回 NULL、link 为 NULL（键可能还留在字典里）；字典尚未分配 → 返回 NULL、link 为 NULL。第三种情况下 link 为 NULL 是关键——调用方不能把它当作「可插入的桶」直接用，否则会把一个还没被物理删除的过期键重复插入。

**真实调用方。** 读侧最常见的是 `lookupKeyReadOrReply`（找不到就直接回错误回复），`src/t_string.c` 的 `getGenericCommand`、`src/t_hash.c`、`src/t_set.c`、`src/t_zset.c` 的读命令都走它。需要「查但别扰动 LRU」的场景用 `lookupKeyReadWithFlags`：`src/db.c` 的 `typeCommand` 与 `src/expire.c` 的 `expireCommand` 都带 `LOOKUP_NOTOUCH`。完全不想产生任何副作用的场景用 `LOOKUP_NOEFFECTS`：`src/blocked.c` 判断被阻塞的键是否已就绪、`src/module.c` 的模块读接口都这么写。写侧入口 `lookupKeyWriteOrReply` 是列表、集合、有序集合、哈希、流在「先取到对象再改它」时的标准取法。

## 关键取舍

**源码事实：** 读与写的差别被压缩成标志位而不是两套函数，三个入口函数本身极薄，只做断言与标志拼装。**源码事实：** 查找的四个副作用各自可关，`LOOKUP_NOEFFECTS` 就是四个「不做什么」标志的组合。**源码事实：** 只读副本不删过期键，只返回 `KEY_EXPIRED`；可写副本允许删。**源码事实：** 写路径的命中/未命中统计与通知被显式排除，源码留有 TODO。**源码事实：** 有活跃子进程时不更新访问时间。

**架构推论：** 把过期检查塞进查找而不是各命令自己判断，意味着「读到一个逻辑上已过期的键」与「读到一个不存在的键」对调用方完全等价，命令层不必再关心 TTL；代价是查找不再是纯函数——副作用的先后顺序（先过期判定、再更新访问时间、最后记统计）成了一条隐式契约，任何绕过这三个入口直接摸字典的代码都会漏掉这些行为。**架构推论：** 「副作用可关闭」而不是「拆出无副作用版本」，是为了给 SCAN、KEYS 这类遍历命令留一条不污染 LRU、不虚增 hits/misses 的路；`src/db.c` 的 `scanCallback` 正是直接调 `expireIfNeeded` 而不是 `lookupKeyRead`，就是为了既跳过过期键又不计入读侧统计。**架构推论：** 把删除过期键的权利收在主节点（副本只判不删），是把副本一致性交给主节点 DEL 传播来保证；主核若做多副本，需要一套等价的「谁有权删除」仲裁，否则副本之间会在同一条键上分叉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/db.c` | `lookupKeyRead` | 纯读查找入口，等价于带 `LOOKUP_NONE` 的带标志版本 |
| `src/db.c` | `lookupKeyReadWithFlags` | 纯读查找的可配置版本，断言标志里不含写意图 |
| `src/db.c` | `lookupKeyWrite` | 写意图查找入口，内部追加 `LOOKUP_WRITE` 标志 |

## 参考

- [Redis 源码：db.c](https://github.com/redis/redis/blob/unstable/src/db.c)
- [Redis 文档：键空间通知](https://redis.io/docs/latest/develop/pubsub/keyspace-notifications/)
- [Redis 命令：TYPE](https://redis.io/docs/latest/commands/type/)
- [Redis 命令：INFO](https://redis.io/docs/latest/commands/info/)

## 与主核 / AI 的关系

「一次查找带齐过期判定、访问时间与统计」是主核存储层读路径可以直接借用的形状：把跨层的一致性检查收进唯一的入口，命令层就只面对「有值 / 没值」两种结果，不必在每条读命令里重写 TTL 判断。主核是多写者引擎，Redis 这套「查找即过期判定」依赖单线程顺序执行才成立——并发事务下两个写者同时命中同一个过期键时，谁来真正删除、删除要不要走事务日志，都得由主核自己定义，不能照搬 Redis 的就地删除加 DEL 传播。

AI 侧若用 Redis 承担向量缓存或会话缓存，命中统计与 `keymiss` 通知是观察缓存命中率的天然数据源；但要注意 `lookupKeyWrite` 不计入命中统计，用写路径做压测会让 `INFO` 里的 `keyspace_hits/misses` 失真。另外 `CLIENT_NO_TOUCH` 与 `TOUCH` 的后门规则说明：一旦把缓存访问权交给不同的客户端身份，LRU/LFU 的老化行为会随客户端角色变化，缓存的淘汰分布也会跟着变。
