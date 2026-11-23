---
project: postgres
feature: commit-timestamp
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/commit_ts.c
    symbols: [TransactionTreeSetCommitTsData, TransactionIdGetCommitTsData]
---

# 提交时间戳存储

## 一句话职责

`commit_ts` 模块把每个事务（含其子事务）的提交时间与复制原点持久化到一条独立的 SLRU 里，并提供按 XID 反查该时间的能力。

## 机制是什么

`pg_xact` 只能回答「这个 XID 提交还是回滚」，回答不了「什么时候提交的」。`commit_ts` 是为后者新建的一条 SLRU：`CommitTimestampEntry` 只有两个字段，`TimestampTz time` 与 `ReplOriginId nodeid`（源码注释明确它按 8+2 字节计），页内条目数 `COMMIT_TS_XACTS_PER_PAGE` 由 `BLCKSZ` 除以该结构体大小推出，页号与页内槽位分别由 `TransactionIdToCTsPage`、`TransactionIdToCTsEntry` 从 XID 直接算出。因此它和 CLOG 一样是「XID 号段直接寻址」的定长数组，没有索引、没有 B-tree。

它负责的是**时间戳本身的存取与有效区间**，不负责判定事务结局。谁提交、何时提交由 `xact.c` 的事务提交路径决定，本模块只是被调用者；XID 的分配与回卷由 `varsup.c` 驱动（`ExtendCommitTs` 由 `GetNewTransactionId` 调用），本模块只顺势扩张自己的页。它也不产出「每次提交一条 WAL」——文件头注释写清了原因：提交路径本身会写 XACT 记录，redo 重放那条记录时会再执行一次 `TransactionTreeSetCommitTsData`，所以这里只对**页初始化与截断**这两种不体现在别的记录里的动作写 WAL（`COMMIT_TS_ZEROPAGE`、`COMMIT_TS_TRUNCATE`）。

模块可开关，且开关状态是跨节点传播的。`ActivateCommitTs` / `DeactivateCommitTs` 存在的理由在源码注释里：它可能被运行期启用，主库的启用/停用要通过 redo 传播到 standby，别的 SLRU 没有这个性质。激活状态放在共享结构 `CommitTimestampShared`（`xidLastCommit`、`dataLastCommit`、`commitTsActive`）里，而不是只读 GUC，正是为了让 standby 能跟随主库。

## 怎么实现的

写入走 `TransactionTreeSetCommitTsData(xid, nsubxids, subxids, timestamp, nodeid)`，四个要点：

1. 先做无锁读 `commitTsShared->commitTsActive` 的短路。注释说明这里可以不加锁：standby 上唯一会改这个标志的是恢复进程，而唯一调用本函数的也是恢复进程。
2. `newestXact` 取子事务数组末元素或父 XID，用于后面推进 `newestCommitTsXid`。
3. **按页分组**：以 `headxid = xid` 起，扫描 `subxids` 直到出现落入另一页的元素，把这一组交给 `SetXidCommitTsInPage` 一次性写；然后以跨界那个子事务为新 head 继续。这样一棵子事务树里每一页只取一次锁、只 `SimpleLruReadPage` 一次，而不是每个 XID 各来一遍。`SetXidCommitTsInPage` 持的是 `SimpleLruGetBankLock(CommitTsCtl, pageno)`——**每页一把**的 bank 锁，不是全 SLRU 一把大锁。
4. 更新共享缓存：持 `CommitTsLock` 排他写 `xidLastCommit` / `dataLastCommit`，并在 `TransactionIdPrecedes` 判定需要时把 `TransamVariables->newestCommitTsXid` 前推。这两个 XID 边界字段在 `TransamVariablesData` 里声明为受 `CommitTsLock` 保护。

读取走 `TransactionIdGetCommitTsData`，返回 `bool` 表示「有没有查到」：

1. `InvalidTransactionId` 直接报错；frozen / bootstrap 这类非普通 XID 直接返回 `*ts = 0` 与 `false`（注释：它们等于「很久以前就提交了」）。
2. 持 `CommitTsLock` 共享锁检查 `commitTsActive`，未启用则 `error_commit_ts_disabled` 抛错，hint 会区分主库与 standby 两种场景提示去设 `track_commit_timestamp`。
3. 命中 `xidLastCommit` 就直接返回内存里的缓存值——最近一次提交是最热的查询点，不必碰 SLRU。
4. 未命中则先拷出 `oldestCommitTsXid` / `newestCommitTsXid` 再放锁，用 `TransactionIdPrecedes` 做区间判断；落在区间外直接返回 `false`，**不去读 SLRU**，这样也避开了已被截断的段。
5. 区间内才 `SimpleLruReadPage_ReadOnly` 读页并 `memcpy` 出条目。注意这个函数是「返回时仍持 bank 锁」的约定，所以调用方补一次 `LWLockRelease(SimpleLruGetBankLock(...))`。

生命周期由几条路径配合：`ExtendCommitTs` 在 `GetNewTransactionId` 持 `XidGenLock` 期间被调用，只在「本页第一个 XID」时动作（并特判回卷后页 0 的首个 XID 是 `FirstNormalTransactionId`），零化该页并写 `COMMIT_TS_ZEROPAGE`；`TruncateCommitTs` 先 `SlruScanDirCbReportPresence` 判断有无可删文件，有才写 `COMMIT_TS_TRUNCATE` 再 `SimpleLruTruncate`；`CheckPointCommitTs` 只是 `SimpleLruWriteAll`。redo 侧 `commit_ts_redo` 只认这两个 opcode，其余 `elog(PANIC)`。

## 关键取舍

**为每个子事务重复存一份时间戳，而不是只存顶层 XID。** 源码注释给了明确理由：`pg_subtrans`（子事务到父事务的映射）不是永久数据，崩溃后不可依赖，所以无法从顶层 XID 反推子事务的提交时间，只能在写入时就把同一时间戳刷到每个子 XID 的槽位上。放弃的是存储空间（一棵 N 个子事务的树要写 N+1 条记录），换来的是崩溃后仍能回答任意子 XID 的时间戳。如果将来 subtrans 变成永久结构，注释里也写明这条决策可以重议。

**区间外直接返回「无数据」而不是尝试读页。** `TransactionIdGetCommitTsData` 用 `oldestCommitTsXid` / `newestCommitTsXid` 先做闸门。这放弃的是「覆盖历史所有 XID」的可用性——`TruncateCommitTs` 之后老数据确实没了，`DeactivateCommitTs` 更是在持 `CommitTsLock` 排他锁期间用 `SlruScanDirCbDeleteAll` 把**所有**段文件删光（注释承认这种做法 very heavy-handed，理由是它只可能在 standby 上极罕见地发生）。换来的是查询侧永远不必处理「页不存在」这种 I/O 异常，把失效判定压缩成一次内存比较。相应地 `SetCommitTsLimit` / `AdvanceOldestCommitTsXid` 只能单向收紧边界：前者只在「已有值更靠前/更新值更靠后」时更新，后者只前推，都不会把区间放宽。

**默认关闭（`track_commit_timestamp` 是 PGC_POSTMASTER）。** 关闭时写入侧只有一次无锁布尔读（`TransactionTreeSetCommitTsData` 与 `ExtendCommitTs` 的入口短路），查询侧直接报错。放弃的是「随时可以回溯历史提交时间」，代价是启用必须重启，且启用前的历史无法补录。这个取舍的退化路径是真实存在的：`ExtendCommitTs` 的早退分支由 `src/backend/access/transam/varsup.c` 的 `GetNewTransactionId` 直接踩到，而 `pg_xact_commit_timestamp` 这条 SQL 入口在模块未激活时走 `error_commit_ts_disabled`。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/commit_ts.c` | `TransactionTreeSetCommitTsData` | 写入入口：按 SLRU 页把父 XID 与子事务数组分组，逐页一次性写入同一时间戳，最后更新 `CommitTimestampShared` 缓存并把 `newestCommitTsXid` 前推 |
| `src/backend/access/transam/commit_ts.c` | `TransactionIdGetCommitTsData` | 查询入口：先查 `xidLastCommit` 内存缓存，再用 `oldestCommitTsXid`/`newestCommitTsXid` 做区间闸门，区间内才用 `SimpleLruReadPage_ReadOnly` 读页取条目 |

## 参考

- [PostgreSQL 文档：`pg_xact_commit_timestamp` 等提交时间戳函数](https://www.postgresql.org/docs/current/functions-info.html) —— 本模块的 SQL 出口与返回语义
- [PostgreSQL 文档：`track_commit_timestamp` 配置项](https://www.postgresql.org/docs/current/runtime-config-replication.html) —— 说明该特性默认关闭、需重启生效
- [PostgreSQL 官方源码：commit_ts.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/commit_ts.c) —— 本文所读的实现

## 与主核 / AI 的关系

暂无直接对应。
