---
project: redis
feature: expiration-metadata
page_type: feature
schema_version: 1
refs:
  - path: src/db.c
    symbols: [setExpire]
---

# 过期时间元数据

## 一句话职责

Redis 把「这个键什么时候失效」记成一个绝对毫秒时间戳，写进键值对象自带的元数据区，同时在过期字典里登记一份索引——一份让读取零成本，一份让扫描只遍历带 TTL 的键。

## 机制是什么

过期时间在 Redis 里有**两份表示**，这是本页的核心。

第一份在键值对象内部。`src/object.c` 的 `kvobjGetExpire` 读的是对象元数据区里的一个槽位——前提是对象的 `metabits` 带 `KEY_META_MASK_EXPIRE` 位；没有这一位就直接返回 `-1`，表示「无过期时间」。写这份的是 `kvobjSetExpire`。

第二份是过期字典 `db->expires`。它是主键空间 `db->keys` 的一个影子索引，只装着带 TTL 的键，字典类型 `dbExpiresDictType` 没有键析构回调——它只是借用主字典里那个键值对象，不拥有它。

时间是**绝对时间戳**而不是相对 TTL。`setExpire` 的注释写明：`when` 参数是「键不再被认为有效之后的绝对 Unix 毫秒时间」。命令层把 EX/PX 这类相对时间在进入这里之前就换算成绝对时间了。

本页只讲过期时间怎么被记录、读取与清除。主动扫描过期键的算法在 c04，内存淘汰在 c05，查找时的过期判定在 c01。

## 怎么实现的

**入口与委派。** `setExpire(client *c, redisDb *db, robj *key, long long when)` 是一行委派：`return setExpireByLink(c, db, key->ptr, when, NULL)`。带 link 的版本 `setExpireByLink` 才是真正的实现——link 是主字典里那个键的条目指针，传入它可以省掉一次查找。`setExpire` 的注释有一条重要提醒：「It may reallocate kvobj. The returned ref may point to a new object」，也就是**返回的 kvobj 可能是新对象**，调用方必须改用返回值。

**定位键。** `setExpireByLink` 先算槽位 `getKeySlot(key)`。若调用方没给 link，就自己 `kvstoreDictFindLink(db->keys, slot, key, NULL)` 去找，并紧跟一句 `serverAssert(keyLink != NULL)`——**主字典里必须有这个键**，否则直接断言失败。这条断言是这份元数据接口的前置条件：过期时间只能挂在一个已经存在的键上。

**读旧值。** `kvobj *kv = dictGetKV(*keyLink)` 拿到键值对象，`long long old_when = kvobjGetExpire(kv)` 读出当前过期时间，`-1` 表示原来没有 TTL。

**分支 A：键原来已有过期时间。** 只做一件事：`kvobj *kvnew = kvobjSetExpire(kv, when)`，然后 `serverAssert(kv == kvnew)`。注释解释了为什么可以断言相等——「Val already had an expire field, so it was not reallocated」。因为过期元数据位已经在对象里，写新时间戳只是改槽位里的值，不需要重新分配。这一分支**不会**碰过期字典：键已经在 `db->expires` 里了。

**分支 B：键原来没有过期时间。** 动作多得多：

1. 开了内存追踪就先记下旧占用 `oldsize = kvobjAllocSize(kv)`。
2. 若这个键的类型支持成员级过期（`typeMaySubexpire`，即哈希字段与集合成员各自带 TTL），先 `estoreRemove(db->subexpires, slot, kv)` 把它从全局的成员过期结构里摘掉。注释给的理由是「先从全局 subexpires DS 摘掉，再去改 kvobj，免得 kvobj 在过程中被释放」。
3. `kvobjSetExpire(kv, when)` 写入过期时间。这里**可能重新分配**：对象原本没有 `KEY_META_MASK_EXPIRE` 位，元数据区放不下这个字段，`kvobjSetExpire` 会用 `kvobjSet(kvobjGetKey(kv), kv, kv->metabits | KEY_META_MASK_EXPIRE)` 重新分配一个带该位的对象。若指针变了，就 `kvstoreDictSetAtLink(db->keys, slot, kvnew, &keyLink, 0)` 把主字典里的条目换成新对象，并在开了内存追踪时用 `updateSlotAllocSize` 修正占用。
4. `kvstoreDictAddRaw(db->expires, slot, kv, NULL)` 把这个键登记进过期字典，并断言成功。这一步是「只遍历带 TTL 的键」这种扫描方式能成立的前提。
5. 若第 2 步摘出过成员过期信息，把新的成员过期时间 `estoreAdd` 回 `db->subexpires`。

**可写副本的额外记账。** 函数末尾算 `int writable_slave = server.masterhost && server.repl_slave_ro == 0`；若当前有客户端 `c`、本实例是可写副本、且该客户端不是主节点连接（`!(c->flags & CLIENT_MASTER)`），就调 `rememberSlaveKeyWithExpire(db, key)`——让副本自己记住哪些键带 TTL，好在副本上也能正确处理过期。

**元数据位机制。** `src/object.c` 的 `kvobjSetExpire` 逻辑很短：若对象没有过期元数据位，且 `expire` 不是 `-1`，先重新分配加上这一位；然后 `*kvobjMetaRef(kv, KEY_META_ID_EXPIRE) = expire` 写值。它的注释同样提醒「may reallocate the value」，并且要求「Use the returned object instead of 'val' after calling this function」。`kvobjGetExpire` 则是它的读侧镜像：没有过期元数据位就返回 `-1`。

**清除过期时间。** `removeExpire` 在 `src/db.c`：两阶段摘链从 `db->expires` 里找到并摘掉条目，然后 `kvobjSetExpire(kv, -1)` 把对象上的过期值清掉，返回 1 表示清掉了、0 表示这个键本来就没有 TTL。`getExpire(db, key, kv)` 是读侧接口：可以传入已有的 kvobj 免一次查找，否则用 `dbFindExpires` 查；查不到或没有 TTL 都返回 `-1`。

**「时间戳已经过去」的退化路径。** 命令层不会给键写一个已经过期的 TTL。`src/expire.c` 的 `expireGenericCommand` 与 `src/t_string.c` 的 `setGenericCommand` 都先调 `checkAlreadyExpired(when)`：若绝对时间戳已经过去，直接 `dbGenericDelete` 把键删掉，并把命令重写成 `DEL` 或 `UNLINK` 再传播。源码注释给的理由是——如果传的是绝对时间戳（EXAT/PXAT），这个时间戳可能早就过去了，那就得删键；传播成显式的 DEL 之后，副本与 AOF 拿到的是确定的删除指令，而不是一个已经越界的 TTL。

**真实调用方。** `setExpire` 的调用者是所有设置 TTL 的命令路径：`src/expire.c` 的 `expireGenericCommand`（EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT 家族）、`src/t_string.c` 的 `setGenericCommand`（SET 的 EX/PX/EXAT/PXAT 选项）、`src/t_string.c` 里 MSET 携带过期参数的路径、`src/t_string.c` 的 GETEX，以及 `src/module.c` 的模块过期接口（那里注释同样写着「setExpire() might realloc kvobj」，并把返回值赋回 `key->kv`）。`src/db.c` 的 `dbAddInternal` 在插入带过期元数据的新键时也走同一套逻辑。`removeExpire` 的调用者是 `src/expire.c` 的 PERSIST 命令、`src/t_string.c` 的 SET PERSIST 选项、`src/module.c` 的模块接口，以及 `src/db.c` 的 `dbSetValue`——就地交换分支里，当不保留 TTL 且旧值有过期时间时，它调 `removeExpire` 把 `db->expires` 里的条目摘掉。

## 关键取舍

**源码事实：** 过期时间是绝对毫秒时间戳，不是相对 TTL。**源码事实：** 过期信息存两份：对象内的元数据位与 `db->expires` 影子索引。**源码事实：** 给原本没有 TTL 的键加 TTL 可能触发键值对象重新分配；改已有 TTL 不会。**源码事实：** 过期字典的条目类型没有析构回调，从它那里删条目不等于释放对象。**源码事实：** 带成员级过期的键还牵着一个 `db->subexpires` 结构。**源码事实：** 过期时间只能挂在已存在的键上，否则断言失败。**源码事实：** 时间戳已过去的命令走删除分支并传播成 DEL/UNLINK。

**架构推论：** 双份表示是典型的空间换扫描效率：过期字典只装带 TTL 的键，主动过期扫描遍历它比遍历整个键空间小一个量级；代价是每次设 TTL 都要维护两份状态，而两份状态的一致性完全依赖 Redis 的单线程执行——并发场景下这两份结构的原子更新需要额外协议。**架构推论：** 把 TTL 放进对象而不是挂在旁边，让「读一个键的过期时间」不需要二次查找，代价是「加 TTL」会因元数据位不足而重新分配对象，从而可能让持有该对象指针的调用方手里的指针失效——这就是 `setExpire` 必须在注释里强调返回新指针的原因。主核的多模态对象若要支持行级 TTL，元数据随对象走会引发同样的指针失效问题。**架构推论：** 绝对时间戳把「何时过期」在写入时就钉死，跨命令、跨副本不会漂移；代价是依赖墙上时钟，机器时钟回拨会让本该过期的键看起来尚未到期。主核若引入租约/TTL，需要单独决定时钟来源（墙上时钟还是逻辑时钟）并显式记录这一假设。**架构推论：** 命令层把「已过期的时间戳」转成删除而不是写入无效 TTL，是为了让复制流与 AOF 里出现的永远是可重放的确定指令；主核的 WAL 里若直接记录一个已越界的绝对过期时间，恢复时可能遇到语义歧义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/db.c` | `setExpire` | 记录键的绝对过期时间：委派给带 link 的版本，负责在对象元数据与过期字典之间保持两份表示一致 |

## 参考

- [Redis 源码：db.c](https://github.com/redis/redis/blob/unstable/src/db.c)
- [Redis 源码：object.c](https://github.com/redis/redis/blob/unstable/src/object.c)
- [Redis 文档：键空间与过期](https://redis.io/docs/latest/develop/use/keyspace/)
- [Redis 命令：EXPIRE](https://redis.io/docs/latest/commands/expire/)

## 与主核 / AI 的关系

「一份内嵌、一份索引」的双份过期表示，对主核的多模态存储有直接借鉴价值：行对象自带过期字段让读取不必二次查表，而过期索引让后台回收只扫该扫的集合。但主核是多写者引擎，这两份结构的更新需要跟值写入放在同一个锁或事务保护下，否则读者可能看到「值已更新、过期索引未更新」的中间态——Redis 靠单线程避免了这一整类问题。

AI 侧的向量缓存尤其依赖过期语义：嵌入向量按 TTL 淘汰是控制显存与召回新鲜度的常规手段。把 TTL 做成绝对时间戳并允许随时重分配对象，对主核的含义是——若向量记录与其过期信息分离存放，重分配向量缓冲区就会让过期引用悬空；因此过期字段更适合与记录同生共死。另外，命令层「时间戳已过去就转成删除」的做法提示：AI 侧写入带 TTL 的缓存时，若 TTL 在到达存储层之前就已越界，应当直接判为无效而不是写入一条永远过期的记录。
