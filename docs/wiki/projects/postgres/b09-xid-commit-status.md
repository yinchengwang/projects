---
project: postgres
feature: xid-commit-status
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/clog.c
    symbols: [TransactionIdSetTreeStatus, TransactionIdGetStatus]
---

# XID 提交状态（CLOG/pg_xact）

## 一句话职责

CLOG（`pg_xact`）为事务及其子事务记录最终提交/中止状态，并提供低层状态读取及异步提交持久化所需的 WAL 水位。

## 机制是什么

CLOG 的逻辑状态不是 XID 分配器：它只为已分配 XID 保存 `IN_PROGRESS`、`COMMITTED`、`ABORTED` 或中间 `SUB_COMMITTED` 状态；XID 分配与是否仍在 ProcArray 活跃由其他事务管理路径负责。`TransactionIdSetTreeStatus` 处理根事务和子事务状态，`TransactionIdGetStatus` 返回单个 XID 的提交状态及一个足以保证相关 commit WAL 已刷新的 LSN 提示。

本页关注事务树的提交/中止状态语义，不展开 b08 的通用 SLRU bank/page I/O 算法，也不负责 b10 的 subXID→parent XID 映射或 MultiXact 成员状态。CLOG 在两个事务落入不同页时分批更新，利用 `SUB_COMMITTED` 中间态使并发状态查询不会观察到根事务已提交而其子事务仍被错误当作独立未提交。

## 怎么实现的

1. XID 通过 `TransactionIdToPage/Byte/BIndex` 定位 `pg_xact` 页、字节和字节内位域；`CLOG_BITS_PER_XACT` 为 2，因此一个字节编码多个 XID。`XactCtl` 配置为带 `group_lsn` 的 SLRU，但具体缓存读写交由 b08 负责。
2. `TransactionIdSetTreeStatus` 找出子事务中与根 XID 位于同一页的前缀。同页时一次持 bank lock 后写根和子事务；跨页提交时先把其他页子事务置为 `SUB_COMMITTED`，再原子更新根页及同页子事务为终态，最后将其余子事务从中间态推进到终态。
3. 每页更新由 `TransactionIdSetPageStatus` 取得对应 bank lock；若小型事务树满足条件且锁竞争，可经 `TransactionGroupUpdateXidStatus` 让同页多个提交者由 leader 一次更新，否则普通等待锁。`TransactionIdSetPageStatusInternal` 更新状态位及 dirty 标记。
4. 异步提交将 commit record LSN 传入状态写入，并在 SLRU `group_lsn` 中保留所属 XID 组里最高 LSN；写页前确保此 WAL 水位持久化。同步提交 caller 已在状态更新前刷 WAL，因此可传 invalid LSN；abort 状态不需按该路径先刷 commit WAL。
5. `TransactionIdGetStatus` 只读加载相应页并读取状态位及所在 LSN 组值，释放 bank lock 后返回。该 LSN 可能是同组较晚事务的 LSN，或历史已落盘页返回 invalid，不承诺等于精确的 commit record LSN。

## 关键取舍

- **跨页树提交采用中间状态与分阶段更新（源码事实）**：每页锁只能提供页内原子修改，故把其他页子事务先标成 `SUB_COMMITTED`，再提交根事务页，最后完成剩余页；避免持有多个 bank lock 的长锁序，但操作不是单一物理页写入。
- **按一组 XID 记录最大 WAL LSN（源码事实）**：`CLOG_XACTS_PER_LSN_GROUP` 是 32，`GetLSNIndex` 将一组映射到一个 group LSN；节省共享元数据空间，但查询返回的是保守上界而不是单事务精确提交 LSN。保留每个 XID 精确 LSN 会显著增加元数据需求。
- **同页小型提交可 group update（源码事实）**：竞争时同一 leader 汇总同页状态更新，减少 bank lock 反复交接；group 仅接受适合条件和相同页的成员，失败 caller 回退到普通独占锁路径。实际 caller `RecordTransactionCommit` 在 `xact.c` 完成提交状态写入；若让 group 接受跨页更新，就必须增加跨 bank 锁协调且可能制造死锁。
- **CLOG 状态更新记录与 WAL commit record 分工（源码事实）**：commit/abort 事务事件本身由 `xact.c` 生成 WAL，CLOG 不重复为每次状态位变化写一条 WAL；新页清零仍需要自己的 WAL 记录。若每个状态位都另写 WAL，会重复记录本已可重放的事务终态事件。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/clog.c` | `TransactionIdSetTreeStatus` | 将根事务及子事务按页批次推进到提交或中止状态 |
| `src/backend/access/transam/clog.c` | `TransactionIdGetStatus` | 读取状态位并返回该 XID 所属组的 WAL 刷新水位 |

## 与主核 / AI 的关系

暂无直接对应。提交状态表是历史事务结果查询，不等同于活跃事务集合、XID 分配器或主核自己的事务状态表示。

## 参考

- [PostgreSQL 文档：事务 ID 与可见性](https://www.postgresql.org/docs/current/transaction-id.html) —— 支撑事务 ID 状态与可见性背景。
- [PostgreSQL 官方源码：clog.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/clog.c) —— 支撑事务树写状态、group LSN 与状态读取细节。
- [PostgreSQL 官方源码：xact.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xact.c) —— 支撑 commit WAL 与状态更新的实际调用关系。
