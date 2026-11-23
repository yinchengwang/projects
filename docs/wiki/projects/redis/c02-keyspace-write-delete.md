---
project: redis
feature: keyspace-write-delete
page_type: feature
schema_version: 1
refs:
  - path: src/db.c
    symbols: [dbAdd, setKey, dbDelete, dbSyncDelete]
---

# 键的写侧插入与删除

## 一句话职责

Redis 把「往键空间放值」拆成新增原语与高层写入口两个层次，把「从键空间拿走键」统一到一个两阶段摘链的删除实现上，释放方式按命令语义与配置在同步与后台线程之间选择。

## 机制是什么

`src/db.c` 把「往键空间里放一个值」和「从键空间里拿走一个键」各收敛成两个入口：新增用 `dbAdd`，覆盖或新增用 `setKey`；删除用 `dbDelete`（跟随惰性释放配置）或 `dbSyncDelete`（强制同步）。四个入口背后是三个内部实现：`dbAddInternal`（插入）、`dbSetValue`（替换已有键的值）、`dbGenericDelete`（删除）。

`setKey` 是注释里自称的「High level Set operation」——注释明确写着「All the new keys in the database should be created via this interface」，也就是说它是新建键的推荐入口，`dbAdd` 是它下面的低层原语。

**插入与覆盖的分工。** `dbAdd` 只负责「键原先不存在」这一种情况：它构造一个不带任何元数据的键值对象塞进主字典。`setKey` 负责「无论原先存不存在」的情况：它先判断键在不在，在就走替换（值的类型、编码、TTL 都可能被换掉），不在就走新增。两条路径都会写键空间通知事件、都会更新键大小直方图、都会触发 WATCH 与客户端追踪失效。

**删除的分工。** `dbDelete`、`dbSyncDelete`、`dbAsyncDelete` 都是 `dbGenericDelete` 的包装，区别只在第二个参数 `async`：`dbSyncDelete` 传 0（同步释放），`dbAsyncDelete` 传 1（交给后台线程释放），`dbDelete` 传 `server.lazyfree_lazy_server_del`——也就是跟随 `lazyfree-lazy-server-del` 配置，该配置默认关闭，所以 `dbDelete` 默认行为等同同步删除。

本页只讲写侧的插入、覆盖与删除动作本身。读侧查找与过期判定在 c01，过期时间的记录方式在 c03，对象编码的转换在 d02，键空间通知的投递机制在 i02。

## 怎么实现的

**新增：`dbAdd` → `dbAddInternal`。** `dbAdd` 只做一件事：构造一个空的 `kvSpec`（`kvSpecInit`，不带元数据），然后转给 `dbAddInternal`。`dbAddInternal` 的动作按源码顺序是：

1. `getKeySlot(key->ptr)` 算出槽位——集群模式下键必须落在自己的槽里。
2. `kvobjSet(key->ptr, val, spec->metabits)` 造出键值对象：键内嵌在对象里，值是调用方给的对象。
3. `initObjectLRUOrLFU(kv)` 给这个新键一个初始老化分——这是新键第一次进入 LRU/LFU 体系。
4. `kvstoreDictSetAtLink(db->keys, slot, kv, link, 1)` 写进主键空间。
5. 处理元数据：若 `spec->metabits` 带过期位，按注释「Expiry is always the first meta (from last)」取出过期时间，调 `setExpireByLink` 写进去；若带模块元数据位，则 `memcpy` 到 kvobj 的元数据区开头。
6. `spec->no_evict` 为真时 `blessSetNoEvict(db, kv, 1)`——把键标记为 NO-EVICT，淘汰时跳过。
7. `signalKeyAsReady(db, key, kv->type)` 唤醒等在这个键上的阻塞客户端。
8. `notifyKeyspaceEvent(NOTIFY_NEW, "new", key, db->id)` 发新增事件。
9. `updateKeysizesHist(db, kv->type, -1, getObjectLength(kv))` 把这个键的体积记进直方图。
10. 开了内存追踪时 `updateSlotAllocSize` 修正槽位占用。
11. 最后 `*valref = kv`——**把调用方手里的值指针改写成存进库里的那个**。

`dbAdd` 的注释给了两条硬契约：存进库的是键的一份拷贝；值对象可能被重新分配因而调用方手里的指针失效，所以要用返回的 `*valref`；值的引用计数**不会**被加一，调用方事后**不能**再 `decrRefCount` 它。

**覆盖：`setKey` → `setKeyByLink`。** `setKey` 的注释列出四条语义：值可能被重分配（`*valref` 会被改写）；WATCH 了这个键的客户端会被通知；**过期时间会被重置（键变成持久键），除非 flags 里带 `SETKEY_KEEPTTL`**；查找可以在这个接口之外做，结果用 `SETKEY_ALREADY_EXIST` 或 `SETKEY_DOESNT_EXIST` 告知。

`setKeyByLink` 按 flags 分三种模式：`SETKEY_ALREADY_EXIST` 要求必须提供 link（键确定存在，直接取用）；`SETKEY_DOESNT_EXIST` 时 link 可选（键确定不存在，link 若给出就当插入桶用）；不带这两个标志时是「加或改」模式，此时用 `lookupKeyWriteWithLink` 查一次，由返回值决定走哪条分支。

键已存在时的动作：

1. `dbSetValue(db, key, valref, *link, 1, 1, flags & SETKEY_KEEPTTL)` 完成真正的替换。
2. `notifyKeyspaceEvent(NOTIFY_OVERWRITTEN, "overwritten", key, db->id)`。
3. 类型变了就再发一条 `NOTIFY_TYPE_CHANGED`（`"type_changed"`）。
4. 新值是列表时唤醒阻塞客户端：类型没变走 `signalKeyAsReadyNonEmptyList`（只唤醒「等非空」的等待者，注释举例是 `BLMOVEM EXACTLY`）；类型变了走 `signalKeyAsReady`（把 `BLPOP`/`BLMOVE`/模块等待者都唤醒）。

键不存在时直接 `dbAddByLink` 走新增。最后两条路径汇合到 `keyModified(c, db, key, *valref, !(flags & SETKEY_NO_SIGNAL))`：有值就更新 LRM 时间戳；`signal` 为真时 `touchWatchedKey` 通知 WATCH 客户端、`trackingInvalidateKey` 发客户端追踪失效。

**替换的实现：`dbSetValue`。** 它的 `overwrite` 参数区分两种语义——「整体替换一个键」（视作先删后加，需要发删除信号）与「只是更新一个键的值」（不发信号）。`setKey` 传的是 overwrite=1。

overwrite 路径上依次做：丢掉旧的模块元数据位；`incrRefCount(old)` 保住旧对象（注释说明模块钩子可能通过 `RM_StringDMA` 触发 `dbUnshareStringValue` 把旧值释放掉）；`keyMetaOnUnlink` 释放模块元数据；`moduleNotifyKeyUnlink(key, old, db->id, DB_FLAG_KEY_OVERWRITE)`——注释写明「虽然键并没有真的从库里删掉，但覆盖视作 unlink + add 两步，所以仍然要调模块的 unlink 回调」；`signalDeletedKeyAsReady` 尝试唤醒等待者；`decrRefCount(old)`；最后 `dbRefreshLink` 重新确认 link——注释理由是「上面的模块钩子可能替换了值或者查了键，这会移动条目」。

值本身的替换有两条分支。**就地交换**：当旧值与新值的引用计数都是 1、编码都不是 EMBSTR、且没有模块元数据要释放时，只交换两者的 `type`/`encoding`/`ptr`，旧对象留在库里、新对象被换出去等着释放——避免了重新分配。**整体替换**：否则造一个新的 `kvobjSet(key->ptr, val, newKeyMetaBits)` 写回原位，并单独处理过期：`keepTTL` 为真时把旧过期时间搬进新对象并同步更新 `db->expires` 里的条目；否则把 `db->expires` 里的条目删掉（键变成持久键）。之后更新键大小直方图与槽位占用。

旧值的释放有三条路：多线程 IO 模式下且旧值是 RAW 编码字符串、引用计数为 1 时，交给 IO 线程延迟释放；否则 `lazyfree-lazy-server-del` 打开时走 `freeObjAsync`；否则直接 `decrRefCount`。

**删除：`dbDelete` / `dbSyncDelete` → `dbGenericDelete`。** `dbGenericDelete` 用两阶段摘链：先 `kvstoreDictTwoPhaseUnlinkFind` 找到条目，摘除动作放到后面做，中间的模块钩子即使改动字典也不会出问题。找到后按顺序：

1. 记录旧长度 `oldlen` 与类型 `type`。
2. 类型支持成员级过期（`typeMaySubexpire`）时，从 `db->subexpires` 里摘掉这个键。
3. 是流类型时 `streamKeyRemoved` 清理 IDMP 跟踪。
4. `incrRefCount(kv)`——注释写明这是为了防 `RM_StringDMA` 路径把 kv 释放掉，引用计数从 1 变 2。
5. 若是 NO-EVICT 键，`blessSetNoEvict(db, kv, 0)` 取消标记。
6. 有模块元数据时 `keyMetaOnUnlink`。
7. `moduleNotifyKeyUnlink` 通知模块键已摘除。
8. `signalDeletedKeyAsReady` 唤醒阻塞在这个键上的客户端。
9. `decrRefCount(kv)`——注释写明「必须在 `freeObjAsync` 之前 decr，否则引用计数可能大于 1，`freeObjAsync` 就不起作用」。
10. 重新 `dictGetKV(*link)` 取一次 kv——注释说明「因为 `dbUnshareStringValue`，库里的值可能已经变了」。
11. 若 `kvobjGetExpire(kv) != -1`，从 `db->expires` 里删掉对应条目。注释特别澄清：删这个条目**不是**对 kvobj 的 `decrRefCount`——过期字典的条目类型没有析构回调，kvobj 的所有权在主字典那边。
12. 异步路径（`async` 为真）下：`updateSlotAllocSize` 修正占用，`freeObjAsync(key, kv, db->id)` 把值交给后台线程，再 `kvstoreDictSetAtLink(db->keys, slot, NULL, &link, 0)` 把主字典里的槽位置成 NULL——注释说这是「在主字典里把键设为 NULL」，避免异步线程释放后又被字典二次释放。
13. `kvstoreDictTwoPhaseUnlinkFree` 真正完成摘除。主键空间的字典类型 `dbDictType` 的键析构回调是 `dictDestructorKV`（定义在 `src/server.c`），它的最后一句就是 `decrRefCount(kv)`——**同步路径的值释放正是在这一步随条目析构发生的**，所以同步路径里 `dbGenericDelete` 自己并不去释放值。
14. 除非带了 `DB_FLAG_NO_UPDATE_KEYSIZES`，否则 `updateKeysizesHist` 把这个键的体积从直方图里减掉。

找不到键时 `dbGenericDelete` 返回 0，调用方据此判断「没删掉任何东西」。

**同步删除与延迟删除的选择。** 三条包装路径对应三种意图：命令自己明确要慢释放时用 `dbAsyncDelete`——`src/db.c` 的 `delGenericCommand` 就是这么分的：`lazy ? dbAsyncDelete : dbSyncDelete`，`UNLINK` 走异步、`DEL` 走同步。`dbDelete` 跟随全局配置 `lazyfree-lazy-server-del`（默认关闭）。`dbSyncDelete` 用于必须立刻释放的场合。

即便是异步路径也不保证一定慢释放：`src/lazyfree.c` 的 `freeObjAsync` 先算「释放代价」，只有代价超过 `LAZYFREE_THRESHOLD`（源码里定义为 64）且对象引用计数为 1 时才真的 `bioCreateLazyFreeJob` 交给 bio 线程，否则就地 `decrRefCount`。引用计数不为 1 时也不能异步释放——注释说明这很少发生，但核心代码里确实有地方会 `incrRefCount` 保护对象后再 `dbDelete`。

**真实调用方。** `dbAdd` 的调用者是「新建一个集合型键」的场景：`src/t_hash.c` 的 HSET 新建路径、`src/t_list.c` 的 LMOVE/RPOPLPUSH 目标键、`src/t_zset.c` 的 ZADD 新建、`src/t_set.c` 的 SADD 新建、`src/t_array.c`、以及 `src/module.c` 的模块写接口。`setKey` 的调用者是「写一个字符串或整体替换」的路径：`src/t_string.c` 的 `setGenericCommand` 与 `getsetCommand`、MSET 家族、`src/bitops.c` 的 BITFIELD、`src/geo.c` 的落库、`src/sort.c` 的 STORE 落库、`src/t_set.c` 与 `src/t_zset.c` 的落库命令、以及 `src/module.c` 的模块写接口（那里带 `SETKEY_NO_SIGNAL`，因为模块自己管追踪失效）。`dbSyncDelete` 的调用者是 `src/db.c` 的 `delGenericCommand`（DEL 的同步分支）、条件删除路径（按 `lazyfree-lazy-user-del` 在 `dbAsyncDelete` 与 `dbSyncDelete` 之间二选一）、`src/t_string.c` 的 `getdelCommand`，以及 `src/rdb.c` 载入时的重复键处理（`RDBFLAGS_ALLOW_DUP` 模式下先删旧键再装新键）。

## 关键取舍

**源码事实：** 新增与覆盖分成两个入口，`setKey` 内部再按「键在不在」分派；`dbAdd` 只能用于键原先不存在的场景。**源码事实：** `setKey` 默认重置 TTL，只有 `SETKEY_KEEPTTL` 才保留。**源码事实：** 覆盖被当作「unlink + add」两步，模块的 unlink 回调照常触发。**源码事实：** 删除走两阶段摘链，模块钩子期间不真正摘除。**源码事实：** 异步释放有代价门槛（`LAZYFREE_THRESHOLD` 为 64）和引用计数门槛，不是无条件慢释放。**源码事实：** 过期字典的条目没有析构回调，kvobj 的所有权始终在主字典。

**架构推论：** 「删除时先 `incrRefCount` 再 `decrRefCount`，且 decr 必须早于 `freeObjAsync`」这一串操作，是为了让模块 API 在删除途中仍能安全访问该键的值；代价是删除路径上多了几次引用计数操作，且任何在 `freeObjAsync` 之后还持有该 kvobj 指针的代码都会踩空。**架构推论：** 惰性释放只对「大对象」有意义——小对象异步释放的调度开销大于直接释放的收益，所以用代价门槛把大部分删除留在主线程；主核若实现异步回收，同样的阈值与「引用计数为 1」前提都得重新论证，因为主核的页/段回收粒度与 Redis 的对象粒度完全不同。**架构推论：** `setKey` 默认清 TTL 是一个容易被忽略的语义：它把「写值」和「设 TTL」拆成了两件事，命令层若想保留 TTL 必须显式传标志；这与主核的事务语义直接冲突——在主核里改值与改过期时间若不在同一个事务内，就会出现「读到旧值但 TTL 已变」的中间态，Redis 靠单线程命令原子性规避了这个问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/db.c` | `dbAdd` | 新增键的低层入口：构造空元数据规格后转给内部插入实现 |
| `src/db.c` | `setKey` | 高层写入口：按「键在不在」分派到替换或新增，并统一处理 TTL、通知与追踪失效 |
| `src/db.c` | `dbDelete` | 删除键，释放方式跟随 `lazyfree-lazy-server-del` 配置 |
| `src/db.c` | `dbSyncDelete` | 删除键并同步释放值对象，不交给后台线程 |

## 参考

- [Redis 源码：db.c](https://github.com/redis/redis/blob/unstable/src/db.c)
- [Redis 文档：内存与延迟优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/)
- [Redis 命令：DEL](https://redis.io/docs/latest/commands/del/)
- [Redis 命令：UNLINK](https://redis.io/docs/latest/commands/unlink/)

## 与主核 / AI 的关系

写侧的三段式——「新增原语 / 高层写入口 / 删除包装」——对主核有直接参考价值：把「这个键存在吗」的判断收敛到唯一入口，可以避免每个写命令各自实现一遍存在性检查与 TTL 处理。但主核的多写者并发让「存在性判断 + 写入」不再能是一个原子步骤，Redis 靠单线程命令执行消掉的竞态，在主核里必须靠锁或事务来补。

删除路径上的两阶段摘链与引用计数保护顺序，是主核实现「删除期间仍有并发读者」时可直接套用的模式：先让读者看见的是一个仍然有效的对象引用，摘除推迟到所有钩子跑完。AI 侧若用 Redis 存向量索引或对话缓存，删除一条 embedding 记录与维护 TTL 是两件事，`setKey` 默认清 TTL 这一点很容易在「更新向量」时被忽略，从而让本应过期的缓存变成永久键——在主核的存储层做类似操作时，过期时间应当与值同事务更新。
