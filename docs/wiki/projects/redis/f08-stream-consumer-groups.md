---
project: redis
feature: stream-consumer-groups
page_type: feature
schema_version: 1
refs:
  - path: src/t_stream.c
    symbols: [streamCreateCG]
---

# 流的消费者组

## 一句话职责

`streamCreateCG` 在流上建起一个消费者组：一个组级游标、一份待确认条目列表、一组消费者——由此把「投递」与「确认」拆成两件事。

## 机制是什么

消费者组把流从「一份可重复读的日志」变成「一份可分摊消费的工作队列」。每个组维护三样东西：

1. **组级游标 `last_id`**：`src/stream.h` 的注释写得很明确——「Last delivered (not acknowledged) ID for this group. Consumers that will just ask for more messages will served with IDs > than this」。它是**已投递但未确认**的最后一条 ID，因此「再要更多消息」的消费者拿到的是 `> last_id` 的条目。
2. **待确认列表 PEL**（`cg->pel`）：一棵 radix tree，key 是 128 位大端 ID，value 是 `streamNACK`。它装着所有「已投递给消费者（NOACK 除外）但尚未被确认处理完」的消息。
3. **消费者表**（`cg->consumers`）：按名字索引的 radix tree，每个消费者有自己的 `pel`，**但与组 PEL 共享同一个 `streamNACK` 结构**——`src/stream.h` 的注释写明「the value is shared」。

`streamNACK` 是待确认条目的实体：`delivery_time`（最后一次投递时间）、`delivery_count`（投递次数）、`consumer`（当前持有它的消费者）、`id`、`cgroup_ref_node`（它挂在流的 `cgroups_ref` 反向索引上的位置），以及 `pel_prev`/`pel_next` 两个指针——组成一条**按投递时间排序的双向链表**，用来 O(1) 取到最老的那条待确认条目。

流的条目本身（追加、迭代、裁剪）在 f07 页，本页只讲组这一层的语义。

## 怎么实现的

**建组：`streamCreateCG(s, name, namelen, id, entries_read)`。**

1. 流还没有组表时先建：`s->cgroups = raxNewEx(0, &s->alloc_size, 0)`。
2. **重名检查**：`raxFindLink(s->cgroups, name, namelen, NULL, &link)`——命中就**返回 NULL**，一个指针都不分配。调用方 `xgroupCommand` 拿到 NULL 时回 `-BUSYGROUP Consumer Group name already exists`。
3. 分配 `streamCG` 结构，尺寸记进 `s->alloc_size`。
4. **建组 PEL**：`cg->pel = raxNewEx(sizeof(streamDistribStamp), &s->alloc_size, sizeof(streamID))`。这个 rax 带一段自定义元数据（`streamDistribStamp`），源码注释说明它承载 INFO Streams 用的组级统计戳；头部的分配类别留了空间放它。key 长度是 `sizeof(streamID)`，即 16 字节。
5. **时间链表初始化**：`pel_time_head`、`pel_time_tail`、`pel_nack_tail` 全置 NULL。最后一个是「NACK 区」的尾部指针——NACK 掉的条目占据链表头部到 `pel_nack_tail` 之间的位置。
6. **建消费者表**：`cg->consumers = raxNewEx(0, &s->alloc_size, 0)`。
7. **设组级游标**：先把 `cg->last_id.ms` 与 `cg->last_id.seq` 置 0，再调 `streamUpdateCGroupLastId(s, cg, id)` 写入目标值。这个函数顺带维护流级的缓存：若该组原来的 `last_id` 正好等于全流的最小 `last_id`，或者新值比当前最小值还小，就把 `s->min_cgroup_last_id_valid` 清掉——缓存失效后需要重新扫所有组才能算回最小值。
8. `cg->entries_read = entries_read`；统计戳的 `epoch` 置 `STREAM_DISTRIB_NEVER_COUNTED`、`counted` 置 0。
9. `raxInsertAt(s->cgroups, name, namelen, cg, NULL, &link)` 落位——用的是第 2 步 `raxFindLink` 顺手记下的插入位，省掉一次树遍历。

**投递即入 PEL。** `XREADGROUP` 真正把一条消息交给消费者时，会调 `streamCreateNACK(s, consumer, &id)` 建 NACK（`delivery_time` 取 `commandTimeSnapshot()`、`delivery_count` 置 1、`consumer` 指向接收方），然后 `raxTryInsert(group->pel, buf, sizeof(buf), nack, NULL)`。**插入失败说明这条 ID 已经在组 PEL 里**——源码注释解释了为什么会这样：管理员可以用 `XGROUP SETID` 改组的最后投递 ID，所以同一条消息可能被再次投递。此时的处理是**转移所有权**而不是新建：把旧消费者 PEL 里的同 ID 项删掉、把 `nack->consumer` 改成新消费者、插进新消费者的 PEL、把 `delivery_count` 重置为 1，再调 `pelListUpdate(group, nack, cmd_time_snapshot)` 更新投递时间并在时间链表里重新定位。新建成功的那条分支则是：插进消费者 PEL、`streamLinkCGroupToEntry` 挂到流的 `cgroups_ref` 反向索引、`pelListInsertAtTail` 挂到时间链表尾部。

**时间链表的三种插入。** `pelListInsertAtTail` 是 O(1) 的尾部追加，也是最常见的路径（投递时间就是当前时间）。`pelListInsertSorted` 处理「投递时间被设成过去时刻」的边缘情况与 RDB 载入——它从尾部往回扫（注释说「most times are recent, so the common case is still fast」），并且**绝不插进 NACK 区**：扫描的边界是 `cg->pel_nack_tail`，这样 NACK 区的 FIFO 顺序不会被破坏。`pelListInsertNacked` 则把 NACK 掉的条目放到 NACK 区末尾，并把它的时间置 0。

**转移：`XCLAIM`。** `xclaimCommand` 找到目标 ID 在组 PEL 里的 NACK 后，先检查 `minidle`（`now - nack->delivery_time` 小于要求的空闲时间就跳过这条），随后若持有者不是目标消费者，就从旧消费者的 PEL 里摘掉；`pelListUpdate(group, nack, deliverytime)` 改投递时间；投递次数按 `retrycount` 参数设为指定值，否则自增 1（已经到 `LLONG_MAX` 就不再增）；最后插进新消费者的 PEL 并把 `nack->consumer` 指向它。`FORCE` 选项还能为「流里根本不存在」的 ID **凭空造一个 PEL 条目**——源码注释写明这是为了「be used to create entries in the PEL. Useful for AOF and replication of consumer groups」。

**确认：`XACK`。** `xackCommand` 的执行顺序值得注意：**先把所有 ID 参数解析完再动手**。源码注释给的理由是「the return value of this command cannot be an error in case the client successfully acknowledged some messages, so it should be executed in an 'all or nothing' fashion」——参数解析出错就直接 `goto cleanup`，不会造成「确认了一半」的中间状态。

逐个 ID 处理时：在组 PEL 里 `raxFind` 找到对应的 NACK → `pelListUnlink` 从时间链表摘掉 → `raxRemove(group->pel, ...)` → 若 `nack->consumer` 非空则 `raxRemove(nack->consumer->pel, ...)` → `streamDestroyNACK` 释放 NACK 并把它从流的 `cgroups_ref` 反向索引里摘掉。每确认一条 `acknowledged++`、`server.dirty++`、调 `keyModified`。ID 不在 PEL 里就**不计数**——回复的数字是「实际确认掉的条数」，不是「请求确认的条数」。

**释放：`streamDestroyCG`。** 销毁组时先遍历该组的 PEL，把每个 NACK 从流的 `cgroups_ref` 反向索引里摘掉（`streamUnlinkEntryFromCGroupRef`）；然后若被销毁的组正好持有全流最小的 `last_id`，把 `s->min_cgroup_last_id_valid` 清掉，让缓存失效；最后 `streamFreeCG` 释放 PEL 与消费者表。释放 PEL 时用 `raxFreeWithCbAndContext(cg->pel, streamFreeNACKGeneric, &ctx)`，回调里先 `pelListUnlink` 再释放 NACK，函数末尾断言三个链表指针都已是 NULL。

**传播。** 消费者组的变更要传播到 AOF 与副本，但传播的不是原始命令：一次 `XREADGROUP` 投递被传播成 `XCLAIM`（`streamPropagateXCLAIM` 带上组 last_id、消费者名、投递时间与投递次数）；`NOACK` 模式下没有 PEL 变更可传播，就改传播 `XGROUP SETID <key> <group> <id> ENTRIESREAD <n>`（`streamPropagateGroupID`）；某条 PEL 条目在 `XCLAIM`/`XAUTOCLAIM` 时已不存在且没有持有者时，传播成 `XACK`（`streamPropagateXACK`）。注释说明了原因：`XCLAIM` 的传播需要 NACK 上有消费者指针，无主的条目不适用。

## 关键取舍

**源码事实：** 组的游标是「已投递未确认」的位置，不是「已确认」的位置。**源码事实：** 组 PEL 与消费者 PEL **共享同一个 `streamNACK` 对象**，所以转移所有权只需改一个指针。**源码事实：** PEL 之外还挂了一条按投递时间排序的双向链表，用于 O(1) 取最老条目与 O(1) 追加。**源码事实：** 重复投递时**转移**已有 NACK 而不新建，`delivery_count` 被重置为 1。**源码事实：** `XACK` 先全量解析参数再执行，保证「要么全确认要么不确认」。**源码事实：** `XACK` 的回复是实际确认掉的条数。**源码事实：** 组 PEL 的 rax 携带一段自定义元数据用于 INFO Streams 的统计戳。**源码事实：** `FORCE` 允许为不存在的消息 ID 凭空创建 PEL 条目，服务于 AOF 与复制。

**架构推论：** 用「投递即入 PEL、确认才移除」实现至少一次投递，主键是把「处理失败」与「还没处理」区分开；代价是每个消费者都必须记得 XACK，否则待确认列表会无限增长——这也是 XAUTOCLAIM 存在的理由。**架构推论：** 组 PEL 与消费者 PEL 共享 NACK 对象，把「一个消费者转交消息给另一个消费者」的成本降到 O(1)；代价是两份 rax 的一致性必须靠「同时从两边删」来维持，源码里 `xackCommand` 就是严格按这个顺序删的。**架构推论：** 额外维护一条按投递时间排序的双向链表，是为了让「找空闲时间最长的待确认条目」变成 O(1)——否则 XAUTOCLAIM 要在 rax 上做范围扫描。代价是每次投递/确认/转移都要多维护两个指针，三处数据结构必须同步。**架构推论：** 把投递传播成 XCLAIM 而不是传播原始的 XREADGROUP，是为了让副本能重放出完全一致的组状态；代价是持久化流里的命令与用户实际发出的命令不一致，排查时需要额外的心智映射。**架构推论：** 组级最小 `last_id` 用「缓存 + 失效标志位」而不是每次现算，是典型的空间/时间换；代价是缓存失效的那一次要全量扫描所有组。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/t_stream.c` | `streamCreateCG` | 建消费者组：重名检查返回 NULL、建组 PEL（带统计戳元数据）、建消费者表、设组级游标与已读计数、登记到流的组表 |

## 参考

- [Redis 源码：t_stream.c](https://github.com/redis/redis/blob/unstable/src/t_stream.c)
- [Redis 文档：流数据类型](https://redis.io/docs/latest/develop/data-types/streams/)
- [Redis 命令：XGROUP](https://redis.io/docs/latest/commands/xgroup/)
- [Redis 命令：XACK](https://redis.io/docs/latest/commands/xack/)
- [Redis 命令：XCLAIM](https://redis.io/docs/latest/commands/xclaim/)

## 与主核 / AI 的关系

消费者组是 Redis 里最接近「分布式任务队列」的结构：投递即入待确认列表、显式确认才移除、超时未确认可被他人认领（XAUTOCLAIM），配套的 `delivery_count` 就是重试次数。主核要做推理任务的分发与回收，这套语义可以整体照搬，但**「至少一次」必须配套幂等**——Redis 把幂等责任完全推给了使用者（消息可能被投递多次）。

AI 侧的映射非常直接：一批推理请求进流，多个 worker 组成一个消费者组各自读，谁读到谁处理，处理完 XACK；某个 worker 挂掉后，它手里未确认的条目由 XAUTOCLAIM 按空闲时间回收给别的 worker。这里有两个 Redis 的设计细节特别值得注意：一是**重复投递时 `delivery_count` 被重置为 1**（而不是累加），主核若做重试统计必须自己修正这个语义；二是 **`XACK` 是全有或全无**的，参数解析失败不会确认任何一条，这给了客户端一个「要么整批确认、要么一条都不确认」的安全边界。

主核是多写者引擎，Redis 的消费者组建立在单线程顺序推进之上——「组游标只前进」这条不变量在并发下需要额外机制。Redis 用「所有状态变更都在主线程」把它免费拿到，主核必须用锁或事务边界把它显式建立起来；否则两个消费者同时读会拿到重叠的区间，破坏「分摊」语义。

另外，Redis 的组 PEL 是一棵 radix tree、按 ID 排序，它天然只能按 ID 区间检索，无法回答「这个消费者的所有待确认条目」这类查询——主核的 AI 任务场景里，「列出某 worker 的全部在途任务」是高频需求，若直接照抄 radix tree 结构会需要额外的索引；这一点 Redis 是靠消费者各自的 PEL rax 补上的，代价是每次转移都要同步维护两份树。
