---
project: postgres
feature: async-notify-listen
page_type: feature
schema_version: 1
refs:
  - path: src/backend/commands/async.c
    symbols: [Async_Notify, Async_Listen, ProcessIncomingNotify, PreCommit_Notify]
---

# LISTEN/NOTIFY

## 一句话职责

LISTEN/NOTIFY 提供数据库内的异步消息：事务内发起的 `NOTIFY` 在提交时按提交顺序写入共享队列，监听该频道的后端在空闲点被唤醒并从队列中按序读取、转换为发给客户端的通知消息。

## 机制是什么

它的核心是一个**共享的 SLRU 通知队列**加一张**全局频道→监听者表**。写者（调用 `NOTIFY` 的事务）在提交前把通知写进队列；读者（执行过 `LISTEN` 的后端）各自维护一个队列读取位置，被写者用信号唤醒后从自己的位置读到当时的队头。因为写入只发生在提交路径且写者被一把跨事务的重型锁串行化，队列内容天然是“已提交、按提交序”的，读者可以据此把未提交事务写入的条目视为“尚未可见”。

事务内 `NOTIFY` 与 `LISTEN` 都不立即生效：`Async_Notify` 只是把事件挂在事务本地的 `pendingNotifies` 链上（并按频道+payload 去重），`Async_Listen`/`Async_Unlisten`/`Async_UnlistenAll` 只是把动作记进 `pendingActions` 链。真正的状态推进分成两个阶段：`PreCommit_Notify` 在提交前做预检与写入（可失败回滚），`AtCommit_Notify` 在写 CLOG 之后才应用本地监听表变更、注册/注销监听者并给目标后端发信号。

本页只讲队列写入、监听表维护、唤醒与读取投递这条链路；队列条目如何在 SLRU 页上组织、页何时被截断（`QUEUE_CLEANUP_DELAY` 等）属于 async 队列存储细节，`NOTIFY` 的 SQL 语义与客户端收到 `NotificationResponse` 后的处理也不在此展开。

## 怎么实现的

1. `Async_Notify(channel, payload)` 先拒绝 `IsParallelWorker()`；校验频道非空且长度小于 `NAMEDATALEN`、payload 长度小于 `NOTIFY_PAYLOAD_MAX_LENGTH`（源码定义为 `BLCKSZ - NAMEDATALEN - 128`）。然后把事件构造成变长的 `Notification`（头部记录 `channel_len`/`payload_len`，数据区依次放频道和 payload）分配在 `CurTransactionContext` 中——它要活到事务结束。
2. 去重与分层：若当前没有 `pendingNotifies` 或本次事务嵌套层更深，则新建一个 `NotificationList`（在 `TopTransactionContext` 中分配，`nestingLevel` 记当前层）压栈；否则先在已有表里查重——事件数达到 `MIN_HASHABLE_NOTIFIES`（16）后会用 `hash_create` 建 `hashtab` 与 `uniqueChannelHash` 加速，未达阈值则线性扫描。重复事件直接 `pfree` 丢弃。
3. `Async_Listen(channel)` 只调 `queue_listen(LISTEN_LISTEN, channel)`：把 `ListenAction` 记进 `pendingActions`，并按嵌套层压 `ActionList`。源码明确**不在这一步合并重复动作**，因为要保留顺序语义（例如 `UNLISTEN *` 与后续 `LISTEN` 的交互），最终意图在 `PreCommit_Notify` 才计算。`Async_Unlisten` 还会先判断“根本不可能在监听则不必排队”。
4. `PreCommit_Notify` 是提交前的可失败阶段：若无 pending 动作和通知则直接返回；先 `initGlobalChannelTable()` 预检全局表可用，再对 `pendingActions` 逐个调用 `BecomeRegisteredListener()` / `PrepareTableEntriesForListen` / `PrepareTableEntriesForUnlisten` / `PrepareTableEntriesForUnlistenAll` 来暂存变更（此时不动全局状态）。
5. 随后处理 `pendingNotifies`：先算出本次要通知的**唯一频道名**（有 `uniqueChannelHash` 就走 hash 扫描，否则退回 O(N²) 列表比较），并预分配 `signalPids`/`signalProcnos`（`MaxBackends` 大小，放在 `TopMemoryContext`）。再 `GetCurrentTransactionId()` 提前拿 XID，避免之后持锁时才取。
6. 关键串行化：`LockSharedObject(DatabaseRelationId, InvalidOid, 0, AccessExclusiveLock)` —— 对“数据库 0”加一把 `AccessExclusiveLock`，一直持到提交或回滚后由锁管理器释放。源码说明两个目的：①保证队列条目按提交顺序出现，不会出现“未提交条目堵在已提交条目前面”；②让 `SignalBackends` 的直接推进优化成立，因为写者之间被完全串行化。注释也承认“锁在 database 0 上很丑”，但值得，并给出替代方案：若将来为扩展性去掉这把锁，就必须在全部插入期间持 `NotifyQueueLock` 排他而非逐页释放。
7. 写队列时逐条推进，`SET_QUEUE_POS(queueHeadBeforeWrite, 0, 0)` 先给安全初值；循环里每轮 `LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE)`、首次迭代记录 `queueHeadBeforeWrite = QUEUE_HEAD`、`asyncQueueFillWarning()`、`asyncQueueIsFull()` 为真则 `ERROR`（“too many notifications in the NOTIFY queue”）、`asyncQueueAddEntries` 写一批、记录 `queueHeadAfterWrite`、再释放锁。逐页加锁的原因写在注释里：让读者能在写者推进过程中插入。此处仍处于“尚未提交到 CLOG”的阶段，因此报错还能回滚。
8. `AtCommit_Notify` 在 CLOG 提交之后运行：`ApplyPendingListenActions(true)` 落地监听表变更；若原本是注册监听者且本地表已空则 `asyncQueueUnregister()` 退出监听者数组；若有 pending 通知则 `SignalBackends()`。
9. `SignalBackends` 在 `NotifyQueueLock` 排他锁下只做“算目标列表”，不发信号：先按本次唯一频道查 `globalChannelTable`（`dshash_find`），遍历 `ListenerEntry`，跳过 `QUEUE_BACKEND_WAKEUP_PENDING` 已置位者、跳过 `QUEUE_POS_EQUAL(pos, QUEUE_HEAD)` 已追平者，其余置 wakeup 标志并记入数组。注意注释指出：即使监听者在 `PreCommit` 后失败、`removeOnAbort` 未生效，也照样会被选入信号列表——它收到信号什么也不会做，但若不这样做，新监听者可能因直接推进逻辑而漏消息。
10. 同一次扫描还做“直接推进”优化：对未被标记、且不在推进中的监听者，若其位置不早于 `queueHeadBeforeWrite` 且早于 `queueHeadAfterWrite`，直接把 `QUEUE_BACKEND_POS(i)` 改到 `queueHeadAfterWrite`，省掉一次唤醒；否则若它落后到 `QUEUE_CLEANUP_DELAY` 个页以上就唤醒让它追赶。释放锁之后才真正 `kill` 目标 PID。
11. `ProcessIncomingNotify(bool flush)` 是读者侧：**无论是否在监听都必须先清 `notifyInterruptPending`**；本地频道表为空则直接返回。随后 `set_ps_display("notify interrupt")`，并且必须在事务中调用 `asyncQueueReadAllNotifications()`（注释说明否则出错会有麻烦），完成后 `CommitTransactionCommand()`，`flush` 为真时 `pq_flush()` 保证客户端尽快收到，再恢复 `set_ps_display("idle")`。
12. `asyncQueueReadAllNotifications` 先在 `NotifyQueueLock` 共享锁下取自己的 `QUEUE_BACKEND_POS` 与当前 `QUEUE_HEAD`，并清 wakeup 标志；若已追平就直接返回。否则置 `QUEUE_BACKEND_IS_ADVANCING = true` 后放锁，用 `RegisterSnapshot(GetSecondarySnapshot())` 取快照用于判定“写入事务是否仍in progress”（源码用一段双后端时序图说明为何必须这样取，并提示应用必须在 `LISTEN` 提交后才检查数据库状态）。
13. 读取循环临时把 `ExitOnAnyError` 打开，使发送失败（例如编码转换失败）升级为 FATAL 并断连接——源注释给出理由：过去只是跳过坏消息继续，会让客户端在收到 ERROR 却漏掉通知的状态下无法判断是否丢消息，断连是明确信号。`asyncQueueProcessPageEntries` 反复处理直到到达进入时的 stop 位置、页尾或未提交条目；处理期间不持 `NotifyQueueLock`（因为队列只会往 head 之外延伸，且自己的位置不动，没人会截断其页），最后再在共享锁下写回位置并清 advancing 标志。
14. 单条投递由 `NotifyMyFrontEnd(channel, payload, srcPid)` 完成：远端输出时组 `PqMsg_NotificationResponse`（PID、频道、payload）并 `pq_endmessage`，**不在这一层 flush**，把合并发包的机会留给上层；非远端输出（如独立后端）则退化为 `elog(INFO, ...)`。

## 关键取舍

- 源码事实：`PreCommit_Notify` 用 `LockSharedObject(DatabaseRelationId, InvalidOid, 0, AccessExclusiveLock)` 在“数据库 0”上加排他锁，一直持到提交/回滚后由锁管理器释放；源码注释明确说这是为了“队列条目按提交顺序出现”，并承认锁标签的选择“很丑”。架构推论：用一把跨事务的重型锁换取全局写者串行化，从而不需要在队列里记录、排序或清理未提交条目；代价是同一时刻全库只有一个事务能提交带通知的改动，热点库上 NOTIFY 写入被串行化，且该锁的获取顺序会参与死锁检测。
- 源码事实：写队列时逐页 `LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE)` 并随即释放，而不是整批持锁；`SignalBackends` 又依赖 `queueHeadBeforeWrite`/`queueHeadAfterWrite` 做“直接推进”。架构推论：逐页持锁让读者能在写者推进期间读取，减小持锁窗口；而直接推进需要“写者之间没有别人插入”这一保证，故必须由上面那把重型锁兜住。若去掉重型锁，注释给出的替代是全程持 `NotifyQueueLock` 排他，换来更长的锁持有时间。
- 源码事实：`Async_Notify` 在事务内做频道+payload 完全匹配的去重（表项达到 `MIN_HASHABLE_NOTIFIES`=16 才建哈希），`Async_Listen` 却刻意**不去重**而保留动作顺序。架构推论：通知是“最终要投递的事件集合”，同一事务内重复的同一事件对语义无影响；而监听动作有顺序依赖（`UNLISTEN *` 之后的 `LISTEN` 必须生效），必须保留序列到 `PreCommit` 才归纳。代价是监听列表可能比实际变更长很多。
- 源码事实：`ProcessIncomingNotify` 在 `asyncQueueReadAllNotifications` 前临时把 `ExitOnAnyError = true`，发送失败即 FATAL 断连；且清理 `notifyInterruptPending` 发生在“是否在监听”判断之前。架构推论：宁可断连也不让客户端处于“收到过 ERROR 但不知道自己漏了通知”的不确定状态，同时确保中断标志不会被“我没在监听”这条提前返回路径漏清；代价是一次可恢复的编码错误会直接终结会话。
- 源码事实：`Async_Notify`/`Async_Listen` 都只写本事务上下文里的链表，`PreCommit_Notify`/`AtCommit_Notify` 才分两阶段落地，且 `PreCommit` 阶段仍可 `ERROR` 回滚（队列满即报 “too many notifications in the NOTIFY queue”）。架构推论：把“能失败的准备工作”放在提交前、把“不可失败的状态变更与唤醒”放在 CLOG 之后，使队列满这类问题还能干净回滚；代价是逻辑被拆成两个函数，两次都要处理 `pendingActions`/`pendingNotifies` 为空时的快速返回。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/commands/async.c` | `Async_Notify` | 事务内收集 NOTIFY：校验频道/payload 长度上限，构造 `Notification` 并在 `pendingNotifies` 链上做去重与分层压栈。 |
| `src/backend/commands/async.c` | `Async_Listen` | 只把 `LISTEN_LISTEN` 动作排入 `pendingActions`，不做去重，保留顺序到 `PreCommit_Notify` 才计算最终监听意图。 |
| `src/backend/commands/async.c` | `PreCommit_Notify` | 提交前可失败阶段：预检全局表、暂存监听变更、算唯一频道、加跨事务重型锁、在 `NotifyQueueLock` 下逐页把通知写入共享 SLRU 队列。 |
| `src/backend/commands/async.c` | `ProcessIncomingNotify` | 读者侧被唤醒后的处理：清中断标志、在事务中读队列并投递到客户端、按需 flush，再恢复空闲显示状态。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL 这种基于共享 SLRU 队列与频道→监听者全局表的进程间异步消息机制，也没有 `LISTEN`/`NOTIFY` 的提交序保证模型。可借鉴的是把“事务内收集 → 提交前可失败写入 → 提交后唤醒”分成三段以保持可回滚性；不要把它与普通发布订阅或进程内事件总线等同。

## 参考

- [PostgreSQL 文档：NOTIFY](https://www.postgresql.org/docs/current/sql-notify.html) —— 说明 NOTIFY 的频道名、payload 限制与提交后投递语义。
- [PostgreSQL 文档：LISTEN](https://www.postgresql.org/docs/current/sql-listen.html) —— 说明 LISTEN 的会话作用域与与 NOTIFY 的配对关系。
- [PostgreSQL 官方源码：async 通知实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/async.c) —— 支撑队列写入、跨事务锁与读者推进的实现事实。
