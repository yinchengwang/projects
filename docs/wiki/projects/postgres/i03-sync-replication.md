---
project: postgres
feature: sync-replication
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/syncrep.c
    symbols: [SyncRepWaitForLSN, SyncRepReleaseWaiters, SyncRepGetSyncRecPtr]
---

# 同步复制

## 一句话职责

同步复制在主库侧决定哪几个备库算作同步副本，并把等待提交的 backend 按 LSN 排进队列，直到备库回报的确认位置越过它们的提交点才放行。

## 机制是什么

`syncrep.c` 头部注释写明了整体设计意图：同步复制的全部逻辑都在主库执行，备库完全不知道主库事务的持久性要求，因此 standby 侧不需要传输任何 per-transaction 状态信息，代码与带宽都更省。主库靠备库定期回报的 write / flush / apply 三个位置判断「已经复制到哪儿了」。

策略由两个 GUC 决定。`synchronous_standby_names` 被解析成扁平的 `SyncRepConfigData`（`config_size`、`num_sync` 需要等几个同步副本、`syncrep_method` 是 `SYNC_REP_PRIORITY` 还是 `SYNC_REP_QUORUM`、`nmembers` 与 `member_names[]`）；`synchronous_commit` 则被映射成内部等待档位 `SyncRepWaitMode`：`SYNCHRONOUS_COMMIT_OFF`/`LOCAL_FLUSH` 对应 `SYNC_REP_NO_WAIT`，`REMOTE_WRITE`/`REMOTE_FLUSH`/`REMOTE_APPLY` 对应 `SYNC_REP_WAIT_WRITE`/`SYNC_REP_WAIT_FLUSH`/`SYNC_REP_WAIT_APPLY`。

等待队列按档位分成三条，挂在 `WalSndCtl->SyncRepQueue[mode]` 上，每条队列是一个按 `waitLSN` 升序排列的双向链表，节点直接是等待者自己的 `PGPROC`（`MyProc->syncRepLinks`）。同一档位下，队列头位置由 `WalSndCtl->lsn[mode]` 记录，表示该档位已经确认到的位置。

本页只讲**复制侧的确认与 quorum 策略**：谁算同步副本、需要等几个、确认位置怎么算、等待者怎么排队与被唤醒。它不负责通用的 LSN 等待器（`xlogwait.c` 那套按 LSN 排序的 waiter heap 与 latch 唤醒，属于 WAL 进度等待机制），也不负责把 WAL 发给备库（`walsender.c` 的发送与反馈回路），更不负责复制槽对 WAL 的保留（`slot.c`）。

## 怎么实现的

1. **提交路径的快速判定。** `SyncRepWaitForLSN` 由提交路径调用（`xact.c` 的 `CommitTransaction` 在 `wrote_xlog && markXidCommitted` 时以 `XactLastRecEnd` 调用它）。函数先无锁读 `WalSndCtl->sync_standbys_status`：若 `SYNC_STANDBY_INIT` 已置而 `SYNC_STANDBY_DEFINED` 未置，说明根本没有配置同步备库，直接返回不等待。这一步是为了避免每次提交都去取 `SyncRepLock`。
2. **确定等待档位。** commit 记录用 `SyncRepWaitMode` 原值；非 commit 记录被 `Min(SyncRepWaitMode, SYNC_REP_WAIT_FLUSH)` 夹到最高只等 flush，源码注释解释：只有 commit 记录才会带来 apply 反馈。
3. **入队。** 取得 `SyncRepLock` 独占后，再看 `WalSndCtl->lsn[mode]`：若目标 LSN 已经不超过这个已确认位置，说明无需等待，立即返回。若共享内存里的同步备库数据还没被 checkpointer 初始化（`SYNC_STANDBY_INIT` 未置），就退化为直接查 GUC `SyncStandbysDefined()`——GUC 为空则确定不用等；GUC 非空则必须等 checkpointer 更新状态，否则无法判断是否该等。通过检查后置 `MyProc->waitLSN = lsn`、`MyProc->syncRepState = SYNC_REP_WAITING`，调 `SyncRepQueueInsert(mode)` 按 LSN 升序插入（从队尾向前找，找到第一个 `proc->waitLSN < MyProc->waitLSN` 的节点插在它之后）。
4. **睡眠。** 之后是标准的 latch 循环：`ResetLatch(MyLatch)` → 查 `MyProc->syncRepState == SYNC_REP_WAIT_COMPLETE` → 处理 `ProcDiePending` 与 `QueryCancelPending` → `WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1, WAIT_EVENT_SYNC_REP)`。postmaster 死亡时置 `ProcDiePending` 并终止等待。
5. **备库侧算确认位置。** `SyncRepReleaseWaiters` 由 walsender 调用（收到备库反馈的 `ProcessStandbyReply` 里，以及配置重载后 `WalSndHandleConfigReload` 里）。它先做快速排除：自己不是候选同步备库（`sync_standby_priority == 0`）、当前状态不是 `WALSNDSTATE_STREAMING`/`WALSNDSTATE_STOPPING`、或自己的 flush 位置无效，都直接返回。随后取 `SyncRepLock` 独占，调用 `SyncRepGetSyncRecPtr`。
6. **quorum 与 priority 两种算法。** `SyncRepGetSyncRecPtr` 先用 `SyncRepGetCandidateStandbys` 收集候选：必须 pid 非 0、状态是 STREAMING 或 STOPPING、`sync_standby_priority` 非 0、flush 位置有效；priority 模式下若候选多于 `num_sync`，按 `standby_priority_comparator` 排序后只保留前 `num_sync` 个。接着判断本 walsender 是否在候选里（`is_me`）、候选数是否够 `num_sync`，任一不满足就返回 false。满足时：priority 模式用 `SyncRepGetOldestSyncRecPtr` 取所有同步备库中最老的 write/flush/apply；quorum 模式用 `SyncRepGetNthLatestSyncRecPtr`，把三个位置各自降序排序后取第 `num_sync` 新的那个。
7. **推进水位并唤醒。** `SyncRepReleaseWaiters` 先把 `WalSndCtl->lsn[SYNC_REP_WAIT_*]` 单调推进到新的确认位置，再调 `SyncRepWakeQueue(false, mode)`。该函数在 `SyncRepLock` 独占下从队头遍历，因为队列按 LSN 升序，遇到第一个 `WalSndCtl->lsn[mode] < proc->waitLSN` 就停。对已满足的等待者：先 `dlist_delete_thoroughly` 摘链，再 `pg_write_barrier()`（等待者读状态时不持锁，必须保证它先看到摘链、后看到状态变更），然后置 `proc->syncRepState = SYNC_REP_WAIT_COMPLETE`，最后 `SetLatch(&proc->procLatch)`。
8. **等待者收尾。** 醒来后执行 `pg_read_barrier()`，断言自己已不在队列上，把 `MyProc->syncRepState` 复位为 `SYNC_REP_NOT_WAITING`、`MyProc->waitLSN` 复位为 `InvalidXLogRecPtr`，并清掉 `ps` 显示里的等待后缀。

并发边界集中在 `SyncRepLock` 一把 LWLock 上：队列的插入、摘除、确认水位与 `sync_standbys_status` 都在它保护下；而等待者读自己的 `syncRepState` 不持锁，靠 `pg_write_barrier()`/`pg_read_barrier()` 与状态单向性保证可见性——源码注释说明 `SYNC_REP_WAIT_COMPLETE` 一旦设置就不会再改，所以不会读到陈旧值。

## 关键取舍

**同步策略完全由主库决定，备库对此无感知。** 源码头部注释给出的理由是：standby 不知道主库事务的持久性要求，可以简化 standby 操作并省掉 per-transaction 状态的网络传输。放弃的是备库侧自行决定「我要不要当同步副本」的能力，代价是主库必须维护每个备库的优先级与三个位置，配置变更（改 `synchronous_standby_names`）也需要主库侧重新解析并让各 walsender 重新计算自己的优先级（`SyncRepInitConfig`）。

**用一条按 LSN 升序的等待队列，而不是每次收到回复就扫所有等待者。** 源码注释直言这是「管理等待 backend 的最佳性能方式」，因为队列有序，唤醒时只需从队头扫到第一个未满足者。代价是入队必须维持有序，`SyncRepQueueInsert` 只能从队尾往前找插入点，源码注释也承认「通常会插到队尾，但也可能乱序到达」。

**事务已本地提交后，遇到管理员终止或查询取消只能发 WARNING 并放弃等待，不能报错。** `SyncRepWaitForLSN` 里 `ProcDiePending` 分支的注释解释了原因：此时若抛 ERROR，客户端会以为事务回滚了，而事务其实已经提交在本地；若继续确认提交，则违反了客户端「已确认提交即已复制」的预期。代价是客户端拿不到结构化的错误码，只能看到一条 WARNING 加连接被终止。真实退化路径是 `SyncRepWaitForLSN` 本身的 `ProcDiePending`/`QueryCancelPending` 分支，调用方在 `src/backend/access/transam/xact.c` 的 `CommitTransaction`。

**非 commit 记录最多只能等到 remote flush。** `SyncRepWaitForLSN` 把非 commit 的等待档位夹到 `SYNC_REP_WAIT_FLUSH`，因为备库的 apply 反馈只对 commit 记录有意义。放弃的是「prepare 记录也等到 remote apply」的可能，代价是 `synchronous_commit = remote_apply` 时两阶段提交的准备阶段并不享受最强的持久性保证。真实 caller 是 `src/backend/access/transam/twophase.c` 里以 `commit = false` 调用的 `SyncRepWaitForLSN(gxact->prepare_end_lsn, false)`。

**priority 模式下取同步备库中最老的位置作为确认位置。** `SyncRepGetOldestSyncRecPtr` 逐个比较取最小值。放弃的是「只要有一个同步备库很快就能确认」的可能性，代价是一个慢备库会拖住所有提交——这正是 priority 模式想要的语义：列表靠前的备库一旦掉线，会被下一优先级的备库立即顶替。

**quorum 模式要排序，priority 模式不排序。** `SyncRepGetNthLatestSyncRecPtr` 对 write/flush/apply 三个数组分别 `qsort` 降序后取第 `num_sync` 个；`SyncRepGetOldestSyncRecPtr` 只做一次线性扫描。源码注释解释了原因：priority 模式用最老算法「更高效一点」，并留了一条 TODO 说明当候选数与请求数相同时 quorum 也可以走最老算法。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/syncrep.c` | `SyncRepWaitForLSN` | 提交侧的等待入口：无锁快速判定是否需要等待、确定等待档位、按 LSN 升序入队、在 latch 上等待并处理管理员终止与查询取消、醒来后清理共享状态 |
| `src/backend/replication/syncrep.c` | `SyncRepReleaseWaiters` | 备库反馈侧的释放入口：排除非候选 walsender，算出新的确认位置后推进 `WalSndCtl->lsn[]` 水位并调用 `SyncRepWakeQueue` 唤醒已满足的等待者 |
| `src/backend/replication/syncrep.c` | `SyncRepGetSyncRecPtr` | 确认位置的计算入口：判定本 walsender 是否为同步副本、候选数是否够，并按 priority 取最老位置、按 quorum 取第 N 新位置 |

## 参考

- [PostgreSQL 文档：同步复制](https://www.postgresql.org/docs/current/warm-standby.html#SYNCHRONOUS-REPLICATION) —— `synchronous_standby_names` 的 FIRST/ANY 语义与 `synchronous_commit` 档位
- [PostgreSQL 文档：流复制协议](https://www.postgresql.org/docs/current/protocol-replication.html) —— 备库回报 write/flush/apply 位置的反馈报文格式
- [PostgreSQL 官方源码：syncrep.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/syncrep.c) —— 等待队列、quorum 计算与唤醒顺序的实现

## 与主核 / AI 的关系

主核 `engine/db` 没有复制层，因此也不存在「同步副本」「quorum」「等待提交确认」这些概念；它的 `wal_truncate` 与 `checkpoint_lsn` 只服务于单机崩溃恢复。若主核将来要支持多副本读或跨节点同步，`syncrep.c` 里两处设计可直接借鉴：一是把「哪些节点算同步副本」完全收在主节点、让副本不感知持久性要求；二是把等待者按 LSN 排成一条有序队列，使每次进度推进只需扫描队列头部一段，而不是遍历全部等待者。
