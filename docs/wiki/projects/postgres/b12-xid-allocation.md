---
project: postgres
feature: xid-allocation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/varsup.c
    symbols: [GetNewTransactionId, ReadNextFullTransactionId, AdvanceNextFullTransactionIdPastXid]
---

# XID 分配与防回卷

## 一句话职责

`varsup.c` 维护集群唯一的 `nextXid` 计数器，负责在事务真正改数据时发号，并在号段逼近 32 位边界时用警告、强制 autovacuum、拒绝执行三道闸门拖住回卷。

## 机制是什么

XID 是 32 位、会回卷的整数。`.cache` 里的 `transam.h` 把几个边界值定死：`InvalidTransactionId` 是 0、`BootstrapTransactionId` 是 1、`FrozenTransactionId` 是 2、`FirstNormalTransactionId` 是 3，而 `MaxTransactionId` 是 `0xFFFFFFFF`。为绕开回卷语义上的歧义，集群内部用 `FullTransactionId`（一个包着 `uint64 value` 的 struct，避免隐式转换）表示号源：高 32 位是 epoch，低 32 位是 XID，由 `FullTransactionIdFromEpochAndXid` 拼装、`EpochFromFullTransactionId` / `XidFromFullTransactionId` 拆分。**号源只有一个**，存在共享内存的 `TransamVariables->nextXid`，受 `XidGenLock` 保护。

本页只管**发号**：何时取号、取号时顺手扩张哪些 SLRU、号源推进到哪里、逼近边界时怎么反应。它**不管**「这个 XID 后来提交了还是回滚了」——那是 CLOG 与 `transam.c` 的查状态路径；也**不管** snapshot 里怎么判断可见性，那读的是 `ProcArray` 与 `TransamVariables->latestCompletedXid`。取号后把新 XID 写进 `MyProc->xid` / `ProcGlobal->xids[]` 这一步确实发生在这里，但它的用意是让 `OldestXmin` 的计算能看到活跃事务，不是给查询者提供状态。

号是**惰性**发的：`xact.c` 的 `AssignTransactionId` 才调 `GetNewTransactionId`，注释写明「XIDs are now not allocated until the transaction does something」。因此一个只读事务根本不消耗 XID，这也是 `GetNewTransactionId` 里那句「we are actually already inside a valid transaction」能成立的前提。

## 怎么实现的

`GetNewTransactionId(bool isSubXact)` 全程持 `XidGenLock` 排他锁（除中间刻意放锁的一段），顺序是：

1. **前置拒绝**。并行模式下调它直接 `elog(ERROR)`（并行 worker 在每个并行操作开始时同步事务状态，之后不能再产生新 XID）；bootstrap 模式返回固定的 `BootstrapTransactionId` 并写进 `MyProc->xid`；`RecoveryInProgress()` 时报错，因为恢复期不能发号。
2. **抄号**。`full_xid = TransamVariables->nextXid`，`xid = XidFromFullTransactionId(full_xid)`。注意抄号时**尚未推进**计数器。
3. **回卷闸门**。若 `TransactionIdFollowsOrEquals(xid, xidVacLimit)` 成立，先把 `xidWarnLimit` / `xidStopLimit` / `xidWrapLimit` / `oldestXidDB` 拷到局部变量，**主动释放 `XidGenLock`**，再做可能阻塞的动作：每 65536 个 XID 发一次 `PMSIGNAL_START_AUTOVAC_LAUNCHER`；越过 `xidStopLimit` 时 `ereport(ERROR)` 拒绝发号（错误信息点名是哪个库的 `datfrozenxid` 最老）；越过 `xidWarnLimit` 时发 `WARNING` 并算出剩余百分比。做完重新拿锁、重新抄号——期间别的后端可能已经推进过。
4. **扩张 SLRU**，仍在持锁状态：依次 `ExtendCLOG(xid)`、`ExtendCommitTs(xid)`、`ExtendSUBTRANS(xid)`。源码注释解释了为什么必须在锁内：否则另一个事务可能拿到更靠后的 XID 并提交，而 CLOG 页还未零化。注释也点明了这些扩张的稀疏性——一页 CLOG 能装 32K 以上事务。
5. **推进号源**。`FullTransactionIdAdvance(&TransamVariables->nextXid)`。顺序被注释特意强调：必须等 `ExtendCLOG` 成功之后才推进，失败就让下一个事务重试，"We cannot assign more XIDs until there is CLOG space for them"。
6. **登记到 ProcArray**，仍持锁：顶层事务把 XID 写进 `MyProc->xid` 与 `ProcGlobal->xids[MyProc->pgxactoff]`；子事务则进 `MyProc->subxids.xids[]` 缓存，满了 `PGPROC_MAX_CACHED_SUBXIDS`（`proc.h` 里定义为 64）就置 `subxidStatus.overflowed`，逼读者改走 `pg_subtrans` 查父链。这里用 `pg_write_barrier()` 与释放锁的内存屏障配对，保证别的进程不会读到「计数已加但数组项还没写」的中间态。
7. 释放 `XidGenLock` 并返回 `full_xid`（含 epoch）。

`ReadNextFullTransactionId` 只是持 `XidGenLock` **共享锁**读一下 `nextXid` 就返回，不发号——名字里的 "Read" 就是这个意思。`transam.h` 的 `ReadNextTransactionId` 是它的 32 位薄包装。

`AdvanceNextFullTransactionIdPastXid(TransactionId xid)` 是**恢复期与启动期专用**的号源校正：把 `nextXid` 推到「给定 xid 的下一个」。它先做一个无锁快退（若 `xid` 不大于当前 32 位 nextxid 就直接返回），然后 `TransactionIdAdvance(xid)` 前移一位，再解决 epoch：取现有 epoch，若发现 `xid < next_xid`（说明跨过了 2^32 边界）就 `++epoch`，最后 `FullTransactionIdFromEpochAndXid` 拼回去并持 `XidGenLock` 排他写回。注释解释了为什么可以推断 epoch：WAL 记录与 2PC 状态里只有 32 位 xid，靠的是「任意时刻活跃 XID 跨度不超过一个 epoch」这一不变量。调用者遍布恢复路径：`xlogrecovery.c` 在重放每条记录前用它保证 `nextXid` 超过 `record->xl_xid`，`xact.c` 的 commit/abort redo 用 `TransactionIdLatest` 取最大 xid 后再推进，`procarray.c` 在 standby 的 known-assigned-xids 路径上用它。

边界值本身由 `SetTransactionIdLimit(oldest_datfrozenxid, oldest_datoid)` 算出，公式都在源码里：`xidWrapLimit = oldest_datfrozenxid + (MaxTransactionId >> 1)`，`xidStopLimit = xidWrapLimit - 3000000`，`xidWarnLimit = xidWrapLimit - 100000000`，`xidVacLimit = oldest_datfrozenxid + autovacuum_freeze_max_age`。注释解释了 3M 这个数字的两重理由：给 DBA 留出在 standalone backend 里修问题的余量，并在默认 `BLCKSZ` 下留两个完全空闲的段，让页/段算术的边界 bug 在单用户模式外不可达。

## 关键取舍

**取号在锁内做 SLRU 扩张，而不是延迟到第一次写状态时。** 放弃的是「取号极快且无 I/O」这个理想性质——`GetNewTransactionId` 的注释明确承认「even when it's not so fast, no actual I/O need happen unless we're forced to write out a dirty CommitTs or xlog page」。换来的是一个简单的不变量：拿到 XID 的事务一定已经有 CLOG/commit_ts/subtrans 的槽位可用，提交路径不必再处理「页不够」的失败分支。如果把扩张挪到提交侧，提交路径就要能容忍扩张失败并回滚，复杂度会明显上升。真实退化点是 `ExtendCLOG` 失败时的行为：`nextXid` 不推进，下一个事务重试同一个号。

**逼近回卷时先放锁再报警/拒绝，而不是持锁做完。** 源码注释说得很直白：不是为了并发，而是避免在 `get_database_name()` 期间死锁。放弃的是「闸门判定与后续动作的原子性」——放锁后别的后端可能已经推进了 `nextXid`，所以必须重新抄号重来（代码里确实重抄了一遍）。代价是一次重入，收益是不在持锁时做可能触发 catalog 访问的操作。

**`AdvanceNextFullTransactionIdPastXid` 用 epoch 推断而不是显式传入。** 32 位 XID 无法自证属于哪个 epoch，函数靠「现有 epoch + 是否跨界」猜。注释承认这在 WAL/2PC 场景安全，因为活跃跨度不超过一个 epoch。放弃的是「对任意历史 XID 都精确」的能力：一个远早于当前 epoch 的 XID 传进来，函数只会把它当作相邻 epoch 处理（不过由于它开头有「不大于当前 nextxid 就返回」的快退，实际不会造成回退）。真实 caller 是 `src/backend/access/transam/xlogrecovery.c` 的 redo 主循环与 `src/backend/access/transam/xact.c` 的 `xact_redo_commit` / `xact_redo_abort`。

**`AdvanceNextFullTransactionIdPastXid` 在快退判定时无锁读 `nextXid`。** 它只在 `AmStartupProcess() || !IsUnderPostmaster` 时被调用（源码用 `Assert` 把这个前提写死），所以「没有别的进程能改它」是成立的，注释也直接这么写。放弃的是「函数可以被任意进程调用」的通用性；作为交换，恢复期最热的每条记录重放都省掉一次锁获取。如果真有多进程并发调用，这里的无锁快退与后面持锁写回之间会出现「读完发现不需要推进」的错误判断。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/varsup.c` | `GetNewTransactionId` | 唯一的发号点：持 `XidGenLock` 抄 `nextXid`、做回卷三道闸门、按顺序扩张 CLOG/commit_ts/subtrans、`FullTransactionIdAdvance` 推进号源、把新 XID 登记进 `MyProc->xid` 或子事务缓存，返回带 epoch 的 `FullTransactionId` |
| `src/backend/access/transam/varsup.c` | `ReadNextFullTransactionId` | 持 `XidGenLock` 共享锁只读 `nextXid`，不发号不扩张；`transam.h` 的 `ReadNextTransactionId` 薄包装、`xid8funcs.c` 与 `walsender.c` 两个同名 `TransactionIdInRecentPast`（判断给定 64 位 xid 是否「在近期过去」、可查 CLOG）、`nbtpage.c` 取 safe xid 都靠它观察号源位置 |
| `src/backend/access/transam/varsup.c` | `AdvanceNextFullTransactionIdPastXid` | 恢复期/启动期专用的号源校正：把 `nextXid` 推到给定 32 位 xid 之后一位，靠 `xid < next_xid` 判定跨 epoch 并 `++epoch`，先无锁快退再持排他锁写回 |

## 参考

- [PostgreSQL 文档：事务 ID 回卷与 `autovacuum_freeze_max_age`](https://www.postgresql.org/docs/current/routine-vacuuming.html) —— 回卷闸门三个阈值要防的具体故障
- [PostgreSQL 文档：`pg_control` 与 `nextXid` 的控制文件字段](https://www.postgresql.org/docs/current/app-pgcontroldata.html) —— epoch 与 XID 分开存储的观测点
- [PostgreSQL 官方源码：varsup.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/varsup.c) —— 本文所读的实现

## 与主核 / AI 的关系

暂无直接对应。
