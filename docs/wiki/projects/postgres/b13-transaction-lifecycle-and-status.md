---
project: postgres
feature: transaction-lifecycle-and-status
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xact.c
    symbols: [StartTransaction, CommitTransaction, AbortTransaction]
  - path: src/backend/access/transam/transam.c
    symbols: [TransactionIdDidCommit, TransactionIdDidAbort, TransactionIdCommitTree]
---

# 事务生命周期与状态接口

## 一句话职责

事务控制器把客户端的 `BEGIN`、语句完成、`COMMIT`、`ROLLBACK` 解释成一组分层状态迁移，调用存储子系统完成提交/回滚，并通过独立接口读取或更新事务结局状态。

## 机制是什么

`xact.c` 同时维护两套互补但不等价的状态。低层 `TransState` 描述后端当前执行事务的阶段（`TRANS_DEFAULT`、`TRANS_START`、`TRANS_INPROGRESS`、`TRANS_COMMIT`、`TRANS_ABORT`、`TRANS_PREPARE`）；高层 `TBlockState` 描述客户端事务块控制状态（如 `TBLOCK_BEGIN`、`TBLOCK_INPROGRESS`、`TBLOCK_ABORT`、`TBLOCK_END`），还包括 savepoint 对应的子事务状态。两套状态都放在每个 backend 的 `TransactionStateData` 链上：当前指针 `CurrentTransactionState` 指向最深一层，子事务 `parent` 回链到父层；结构还持有 `fullTransactionId`、`childXids[]`、事务内存上下文、`ResourceOwner`、前一用户安全上下文等生命周期数据。

`StartTransaction` 建立**后端本地事务上下文**：初始化状态栈与计数器、创建内存/资源拥有者、登记 virtual xid、初始化 GUC/cache/trigger 管理器；它不立刻分配永久 XID。永久 XID 惰性延后到 `AssignTransactionId`，那是 b12 的发号机制。顶层提交/回滚由 `CommitTransaction` / `AbortTransaction` 编排；真正的持久状态字节在 CLOG/`pg_xact` 页里，本文不讲 CLOG 页的布局、缓存、截断或 SLRU I/O（b08）；也不讲 `TransactionIdDidCommit` 如何从持久状态作事务可见性判定的细节（b09）。

`transam.c` 是另一侧的状态接口：`TransactionIdDidCommit` / `TransactionIdDidAbort` 把 `TransactionLogFetch` 取到的 CLOG 状态转成布尔结果；遇到 `SUB_COMMITTED` 会沿 `SubTransGetParent` 递归查父事务，但 XID 早于 `TransactionXmin` 时不再查子事务映射。`TransactionIdCommitTree` 则将父 XID 和子 XID 数组交给 CLOG 的 `TransactionIdSetTreeStatus`。这里只说明 xact 状态机如何调用它；状态字节编码/写页分组/原子性边界属于 CLOG 机制，不重复展开。

## 怎么实现的

1. **语句边界驱动状态机**。`StartTransactionCommand` 看 `blockState`：`TBLOCK_DEFAULT` 时调 `StartTransaction` 并置 `TBLOCK_STARTED`；`BEGIN` 命令经 `TBLOCK_BEGIN` 转成 `TBLOCK_INPROGRESS`。每条语句完成时 `CommitTransactionCommandInternal` 在普通进行态执行 `CommandCounterIncrement`；遇到 `TBLOCK_END` 才进入顶层 `CommitTransaction`。外层 `CommitTransactionCommand` 用循环迭代处理嵌套 savepoint，避免深度递归。
2. **开始只初始化本地生命周期**。`StartTransaction` 先将 `CurrentTransactionState` 置为顶层结构，并断言状态为 `TRANS_DEFAULT`；接着置 `TRANS_START`、清空 XID/子 XID/子事务计数、保存用户与安全上下文，依据 `RecoveryInProgress()` 设成只读或使用默认事务属性，初始化 `AtStart_Memory` / `AtStart_ResourceOwner`，创建 virtual xid 并先锁入虚拟事务锁表再向 ProcArray 公告。接下来设 `transaction_timestamp()`、初始化 GUC/cache/after-trigger 状态，最后才置 `TRANS_INPROGRESS`。任何启动中途错误都会进入 abort wrapper，避免遗留半初始化资源。
3. **提交先跑可能执行用户代码的前置动作**。`CommitTransaction` 先检查 deferred triggers、关闭 portals（可能再次产生触发器，所以循环到稳定），再执行 pre-commit callbacks、并行清理、ON COMMIT 动作、待同步文件、通知队列与 SSI 检查。只有进入不可再调用用户代码的收尾阶段后，才持 `HOLD_INTERRUPTS`，置 `TRANS_COMMIT` 并禁用事务超时。
4. **持久提交由 `RecordTransactionCommit` 完成**。有 XID 的非并行 worker 会把提交信息写入 XACT WAL；如果要求同步提交，先 `XLogFlush` 再调用 `TransactionIdCommitTree` 更新 CLOG；异步提交则以提交 LSN 调用 `TransactionIdAsyncCommitTree`，该 LSN 被 CLOG 存在 `group_lsn[]` 里，`TransactionIdGetStatus` 会把它一起返回。这份 LSN 的真实消费者是 `heapam_visibility.c` 的 `SetHintBitsExt`：它用 `TransactionIdGetCommitLSN` 取 LSN 后判 `XLogNeedsFlush(commitLSN) && BufferGetLSNAtomic(buffer) < commitLSN`，两个条件都成立才**不**设置 hint bit——也就是说，异步提交的检查是在读侧按需做，而不是让人去阻塞等待。无 XID 事务不会强行分配 XID，也不会写普通 COMMIT 记录。并行 worker 不标记提交状态，由 leader 负责。
5. **对外宣布结束与资源释放有严格顺序**。`CommitTransaction` 在记录提交后调用 `ProcArrayEndTransaction`，但必须在释放事务锁之前；之后先放可被其他 backend 观察的资源，再释放锁，最后清理 backend-local GUC、SPI、snapshot、memory 等状态，清空 `fullTransactionId` 并回到 `TRANS_DEFAULT`。源码注释特别强调，等锁等待者被唤醒时事务要已经完整收尾。
6. **异常与回滚路径**。`AbortCurrentTransaction` 的循环 wrapper 按 `TBlockState` 决定简单回滚并清理，还是留在 `TBLOCK_ABORT` 等客户端显式 `ROLLBACK`。`AbortTransaction` 先 `HOLD_INTERRUPTS`、恢复 abort memory/resource owner、尽快释放所有 LWLock 与 buffer content lock、清理 LSN/锁等待；之后置 `TRANS_ABORT`，撤销通知等子系统动作，再由 `RecordTransactionAbort` 写 ABORT WAL 并调用 `TransactionIdAbortTree`。源码注释明确：abort WAL 不 flush，因为崩溃时默认推定事务已 abort 即可。最后 `ProcArrayEndTransaction`，状态留在 `TRANS_ABORT` 直至 `CleanupTransaction` 真正清除本地资源、回到 `TRANS_DEFAULT`。
7. **状态查询层不等同于运行中名单**。`TransactionIdDidCommit` 先查单项缓存，缓存只记不会再变化的 committed/aborted，不缓存 in-progress/sub-committed。普通 XID 才查 `TransactionIdGetStatus`；sub-committed 子事务则借 `pg_subtrans` 递归问父 XID。`TransactionIdDidAbort` 对显式 aborted 返回 true，但源码注释指出，崩溃前未清理的事务可能仍显示 in-progress，不能将它理解为「所有实际上回滚的事务都已显式标记 aborted」。

## 关键取舍

**把「事务是否存在」与「永久 XID 是否分配」拆开。** `StartTransaction` 已进入 `TRANS_INPROGRESS`，但 `fullTransactionId` 仍无效；只有事务第一次做需要 XID 的事才在 `AssignTransactionId` 分配。这避免只读事务和不改变数据库状态的事务污染 XID 空间，代价是一些提交路径必须处理「有有效事务但无 XID」的分支：`RecordTransactionCommit` 对无 XID 事务不写 COMMIT WAL；若有需要传播的 invalidation 消息，它改写专门的消息记录。替代方案是启动时一律发号，实施简单但会让大量纯读事务加快回卷压力。

**先做可能失败的提交前工作，再进入提交临界区并落状态。** deferred trigger、portal 关闭、ON COMMIT 与 SSI 检查都可能报错，因此在这些结束前不能不可逆地标记已提交。进入 `TRANS_COMMIT` 后 `RecordTransactionCommit` 才先写 WAL，满足同步提交时先 flush，再写 CLOG committed 状态；如果反过来先落 CLOG，崩溃可能留下「状态显示 committed、提交记录却未持久」的矛盾。异步提交是明确的性能取舍：`TransactionIdAsyncCommitTree` 先把 commit LSN 与状态一起交给 CLOG，后续读取者需等 WAL flush，换较少的前台等待，代价是 postmaster 崩溃可能丢失事务。

**中断与错误路径显式分两段清理，而不试图把 abort 压成一个函数返回。** `AbortTransaction` 先进入 `TRANS_ABORT` 并清理必须立即释放的锁/等待状态，`CleanupTransaction` 再清空内存、snapshot、resource owner 并将状态复位；显式事务块在两者之间还可能留在 `TBLOCK_ABORT`，只允许后续 `ROLLBACK` 脱离。放弃的是简单线性流程，换来错误发生时仍保留客户端协议状态，避免把一个失败语句误当成整个会话已回到 idle。真实 caller 是 `AbortCurrentTransactionInternal`：普通语句块错误立即 Abort+Cleanup，而显式 `BEGIN` 内错误只 Abort 并保持 `TBLOCK_ABORT` 等待 rollback。

**`TransactionIdDidAbort` 只认显式 abort 标记，不推断 crash abort。** `transam.c` 的注释明言：崩溃导致的隐式 abort 常在 CLOG 仍显示 in-progress；`TransactionIdDidAbort` 不会因此返回 true。放弃的是单次函数调用给出所有终局的强语义，代价是 caller 往往需要先做 `TransactionIdIsInProgress` / `TransactionIdDidCommit` 组合判定；替代方案是把每个 crash 遗留 XID 都同步补写为 aborted，恢复阶段将承担额外全量清理与持久化成本。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xact.c` | `StartTransaction` | 建立顶层 `TransactionStateData` 生命周期、事务内存/资源拥有者、virtual xid 与本地事务属性；不立即发永久 XID |
| `src/backend/access/transam/xact.c` | `CommitTransaction` | 提交的编排器：触发 pre-commit 工作，最终调用 `RecordTransactionCommit`，然后结束 ProcArray 状态并按依赖顺序释放资源 |
| `src/backend/access/transam/xact.c` | `AbortTransaction` | 回滚的低层收尾器：解除等待与轻量锁、撤销子系统动作、写 abort 记录并结束 ProcArray 状态，但将最终本地清理留给 `CleanupTransaction` |
| `src/backend/access/transam/transam.c` | `TransactionIdDidCommit` | 读 CLOG 状态并把 committed/sub-committed 状态转成事务结局查询；子事务可能经 `pg_subtrans` 递归追父 XID |
| `src/backend/access/transam/transam.c` | `TransactionIdDidAbort` | 只判断明确记录的 abort 状态，sub-committed 路径同样沿父 XID 查；不把 crash 后仍为 in-progress 的字节推断成已标记 abort |
| `src/backend/access/transam/transam.c` | `TransactionIdCommitTree` | CLOG 状态写入的高层适配器，将父 XID 与子 XID 数组转交 `TransactionIdSetTreeStatus`；不负责事务块状态迁移 |

## 参考

- [PostgreSQL 文档：事务管理](https://www.postgresql.org/docs/current/tutorial-transactions.html) —— 客户端事务块 begin/commit/rollback 的外部语义
- [PostgreSQL 文档：事务隔离与 MVCC](https://www.postgresql.org/docs/current/transaction-iso.html) —— 提交状态与并发可见性的关系
- [PostgreSQL 官方源码：xact.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xact.c) —— 事务生命周期编排实现

## 与主核 / AI 的关系

暂无直接对应。
