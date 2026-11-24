---
project: sqlite
feature: transaction-begin-in-vdbe
page_type: feature
schema_version: 1
refs:
  - path: src/build.c
    symbols: [sqlite3BeginTransaction, sqlite3BeginWriteOperation]
---

# 虚拟机内的事务与保存点

## 一句话职责

把「语句要开事务」和「语句要写库」这两件事翻译成虚拟机能执行的形式：前者由 `sqlite3BeginTransaction` 发出 `OP_Transaction` / `OP_AutoCommit` 指令，后者由 `sqlite3BeginWriteOperation` 在编译期打上写库标记位，再由 `sqlite3FinishCoding` 落成 `OP_Transaction` 的 P2 操作数。

## 机制是什么

SQLite 的事务边界有两个层次，容易混淆：

**显式事务边界**（`BEGIN` / `COMMIT` / `ROLLBACK`）由 `sqlite3BeginTransaction` 与 `sqlite3EndTransaction` 在**语法分析阶段**翻译成指令。`sqlite3BeginTransaction` 读 `src/parse.y` 的 `cmd ::= BEGIN transtype(Y) trans_opt` 规则，`transtype` 产生 `TK_DEFERRED` / `TK_IMMEDIATE` / `TK_EXCLUSIVE` 三种 token 值。

**隐式语句级事务**（写操作自动包一层可回滚的语句事务）由 `sqlite3BeginWriteOperation` 在**代码生成阶段**登记，最终体现为 `OP_Transaction` 的 P2 参数非零。

两者的共同产物是 `OP_Transaction` 指令，但触发路径完全不同：

- `sqlite3BeginTransaction` 走**语法动作**路径，直接往当前 VDBE 程序里追加指令。
- `sqlite3BeginWriteOperation` 走**编译期状态**路径，它不追加任何指令，只在 `Parse` 结构上置位；指令由 `sqlite3FinishCoding`（`src/build.c`）在语句收尾时统一发出。

保存点（`SAVEPOINT` / `RELEASE` / `ROLLBACK TO`）由 `sqlite3Savepoint` 处理，同样在语法阶段翻译成一条 `OP_Savepoint` 指令。其 P1 操作数取自 `src/sqliteInt.h` 定义的三个常量：

| 常量 | 值 | 含义 |
|---|---|---|
| `SAVEPOINT_BEGIN` | 0 | 建立保存点 |
| `SAVEPOINT_RELEASE` | 1 | 释放（提交）保存点 |
| `SAVEPOINT_ROLLBACK` | 2 | 回滚到保存点 |

这三个常量同时是 `sqlite3Savepoint` 的第二个入参和 `OP_Savepoint` 指令的 P1 操作数——`src/sqliteInt.h` 的注释明确写了这一点。

本机制的边界：**不负责文件锁的获取**（那是 pager / btree 层），**不负责回滚日志的写入**（那是 G 组）。这里只负责「在虚拟机里表达出事务边界与保存点」。

## 怎么实现的

### `sqlite3BeginTransaction`：显式 BEGIN 的指令生成

函数体（`src/build.c` 的 `sqlite3BeginTransaction`）的执行顺序：

1. `assert( pParse!=0 )`，取 `db = pParse->db`。
2. `sqlite3AuthCheck(pParse, SQLITE_TRANSACTION, "BEGIN", 0, 0)` 失败则**直接 return，不生成任何指令**。`SQLITE_TRANSACTION` 是 `src/sqlite.h.in` 里定义的授权动作码 **22**；`sqlite3AuthCheck`（`src/auth.c`）在 `pParse->db->xAuth==0` 或 `pParse->db->init.busy` 非零时返回 `SQLITE_OK`，否则转 `realAuthCheck`。
3. `v = sqlite3GetVdbe(pParse)`；`v` 为 0 则 return。`sqlite3GetVdbe` 定义在 `src/select.c`。
4. **`type!=TK_DEFERRED` 时**遍历 `i=0..db->nDb-1`，对每个库决定事务类型 `eTxnType`：
   - `pBt && sqlite3BtreeIsReadonly(pBt)` 为真 → `eTxnType = 0`（只读事务）。`sqlite3BtreeIsReadonly`（`src/btree.c`）判断 `p->pBt->btsFlags & BTS_READ_ONLY`。
   - 否则 `type==TK_EXCLUSIVE` → `eTxnType = 2`（排他事务）。
   - 否则 → `eTxnType = 1`（写事务）。
   - 然后 `sqlite3VdbeAddOp2(v, OP_Transaction, i, eTxnType)` 追加指令，并 `sqlite3VdbeUsesBtree(v, i)` 把该库登记进语句的 btree 掩码。`sqlite3VdbeUsesBtree`（`src/vdbeaux.c`）置 `p->btreeMask` 的第 i 位；若 `i!=1` 且 `sqlite3BtreeSharable(db->aDb[i].pBt)` 为真，额外置 `p->lockMask` 的第 i 位。
   - **注意 `TK_DEFERRED` 完全跳过这个循环**：`DEFERRED` 不发出任何 `OP_Transaction`，只靠末尾那条 `OP_AutoCommit` 关闭自动提交模式。这是源码里 `if( type!=TK_DEFERRED )` 这一行判断的直接后果。
5. 无论事务类型如何，最后都 `sqlite3VdbeAddOp0(v, OP_AutoCommit)`。`OP_AutoCommit` 的 P1 是目标自动提交状态（此处为 0，表示**进入**显式事务），P2 为 0（不回滚）。

`OP_AutoCommit` 的执行语义见 `src/vdbe.c`：当 `desiredAutoCommit != db->autoCommit` 时才动手；若 `iRollback` 非零则 `sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)` 并把 `db->autoCommit` 置 1；若 `desiredAutoCommit` 为真而 `db->nVdbeWrite>0`，报 "cannot commit transaction - SQL statements in progress" 并返回 `SQLITE_BUSY`；否则调 `sqlite3VdbeCheckFkDeferred(p)` 检查未决外键约束，最后 `db->autoCommit = (u8)desiredAutoCommit`。当 `desiredAutoCommit == db->autoCommit`（即重复 BEGIN）时进入 else 分支报错，错误信息由三元表达式择一："cannot start a transaction within a transaction"、"cannot rollback - no transaction is active" 或 "cannot commit - no transaction is active"，返回 `SQLITE_ERROR`。

`sqlite3EndTransaction`（`src/build.c`）对称地只发一条 `sqlite3VdbeAddOp2(v, OP_AutoCommit, 1, isRollback)`，其中 `isRollback = eType==TK_ROLLBACK`。

### `sqlite3BeginWriteOperation`：编译期的写库登记

函数体（`src/build.c` 的 `sqlite3BeginWriteOperation`）只有三行实质逻辑：

```c
Parse *pToplevel = sqlite3ParseToplevel(pParse);
sqlite3CodeVerifySchemaAtToplevel(pToplevel, iDb);
DbMaskSet(pToplevel->writeMask, iDb);
pToplevel->isMultiWrite |= setStatement;
```

逐项说明：

- `sqlite3ParseToplevel(pParse)` 是 `src/sqliteInt.h` 里的宏 `(p)->pToplevel ? (p)->pToplevel : (p)`。在启用触发器的编译（`SQLITE_OMIT_TRIGGER` 未定义）下取顶层 `Parse`；在 `SQLITE_OMIT_TRIGGER` 下退化为恒等宏 `p`。也就是说，**触发器子程序里的写操作会登记到顶层语句上**，而不是登记到子程序自己的 `Parse` 上。
- `sqlite3CodeVerifySchemaAtToplevel(pToplevel, iDb)`：若 `DbMaskTest(pToplevel->cookieMask, iDb)` 为 0，则 `DbMaskSet(pToplevel->cookieMask, iDb)`，并在 `iDb==1`（TEMP 库）且未定义 `SQLITE_OMIT_TEMPDB` 时调 `sqlite3OpenTempDatabase(pToplevel)`。作用是确保「要校验 schema cookie 的库集合」与「要写事务的库集合」最终一致。
- `DbMaskSet(pToplevel->writeMask, iDb)`：置写库掩码的第 iDb 位。`writeMask` 在 `src/sqliteInt.h` 的 `struct Parse` 里声明为 `yDbMask writeMask;`，注释是 "Start a write transaction on these databases"。
- `pToplevel->isMultiWrite |= setStatement`：第二个参数 `setStatement` 是「本次调用是否代表一条会写多行的语句」的标志。源码注释解释了为什么需要这个标志：「对于可能在执行中途失败（因约束）且需要撤销部分写入的操作，应设置 checkpoint；对于所有约束都能在改动之前检查完的操作，永远不需要撤销写入，也不应设置 checkpoint。」

**`setStatement` 参数的真实调用方**（全部在 `src/build.c` 内，实际调用 `sqlite3BeginWriteOperation`）：

| 调用方 | `setStatement` | `iDb` |
|---|---|---|
| `sqlite3StartTable`（建表路径） | 1 | 建表所在库 |
| `sqlite3CodeDropTable` | 1 | 目标库 |
| `sqlite3DropTable` | 1 | 目标库 |
| `sqlite3CreateIndex`（建索引路径） | 1 | 目标库 |
| `sqlite3DropIndex` | 1 | 目标库 |
| `sqlite3CodeRefillIndex`（REINDEX 路径） | 0 | 目标库 |
| `sqlite3VtabUsesAllSchemas`（`src/where.c`） | 0 | 遍历全部库 |

`setStatement=0` 的两处都传 0：REINDEX 只需保证该库已开写事务，但重建索引不产生需要回滚的约束冲突；`sqlite3VtabUsesAllSchemas` 只在 `DbMaskNonZero(pParse->writeMask)` 为真（已有别的库被标记为写）时才逐库补登记。

**指令是怎么落地成 `OP_Transaction` 的**：`sqlite3FinishCoding`（`src/build.c`）在语句收尾时遍历 `iDb=0..db->nDb-1`，对每个 `DbMaskTest(pParse->cookieMask, iDb)` 为真的库发一条 `sqlite3VdbeAddOp4Int(v, OP_Transaction, iDb, DbMaskTest(pParse->writeMask,iDb), pSchema->schema_cookie, pSchema->iGeneration)`。**`writeMask` 的第 iDb 位就是 `OP_Transaction` 的 P2 操作数**。同时该处用 `VdbeComment` 记下 `"usesStmtJournal=%d"`，取值是 `pParse->mayAbort && pParse->isMultiWrite`。

`OP_Transaction` 的 P2 语义见 `src/vdbe.c` 的注释：P2 为 0 开只读事务，P2 非 0 开写事务（或把已有的只读事务升级为写事务），P2 大于等于 2 开排他事务。同一段注释说明：当 P2 非 0 且 `Vdbe.usesStmtJournal` 为真时，会额外开一个**语句事务**——「语句事务允许本 VDBE 做出的改动在出错后被回滚，而不必回滚整个事务；若无错误，语句事务将在 VDBE 停机时自动提交」。

`usesStmtJournal` 的赋值在 `src/vdbeaux.c`：`p->usesStmtJournal = (u8)(pParse->isMultiWrite && pParse->mayAbort);`。`mayAbort` 由 `sqlite3MayAbort`（`src/build.c`）置位，`isMultiWrite` 由 `sqlite3MultiWrite`（`src/build.c`）或 `sqlite3BeginWriteOperation` 置位。

### `sqlite3Savepoint`：保存点的指令生成

函数体（`src/build.c`）：

1. `zName = sqlite3NameFromToken(pParse->db, pName)`，为空则整体跳过。
2. 取 `v = sqlite3GetVdbe(pParse)`。
3. `if( !v || sqlite3AuthCheck(pParse, SQLITE_SAVEPOINT, az[op], zName, 0) )` 则 `sqlite3DbFree(pParse->db, zName)` 并 return。`SQLITE_SAVEPOINT` 是 `src/sqlite.h.in` 里的授权动作码 **32**；`az` 是静态数组 `{"BEGIN", "RELEASE", "ROLLBACK"}`，源码用 `assert( !SAVEPOINT_BEGIN && SAVEPOINT_RELEASE==1 && SAVEPOINT_ROLLBACK==2 )` 把数组下标与常量值对齐。
4. `sqlite3VdbeAddOp4(v, OP_Savepoint, op, 0, 0, zName, P4_DYNAMIC)`——名字用 `P4_DYNAMIC` 传入，由 VDBE 负责释放。

`OP_Savepoint` 的执行语义见 `src/vdbe.c`：P1 为 `SAVEPOINT_BEGIN` 时，若 `db->nVdbeWrite>0` 则报 "cannot open savepoint - SQL statements in progress" 并返回 `SQLITE_BUSY`；否则分配 `Savepoint` 结构（`src/sqliteInt.h` 里定义为含 `zName`、两个延迟外键计数 `nDeferredCons` / `nDeferredImmCons`、父指针 `pNext` 的结构体），并**在 `db->autoCommit` 为真时把 `db->autoCommit` 置 0 且置 `db->isTransactionSavepoint = 1`**——即在自动提交模式下建立保存点会隐式开启一个事务；若已有事务，则 `db->nSavepoint++`。

### 与保存点相关的边界判定

- `OP_Savepoint` 里有断言 `db->pSavepoint==0 || db->autoCommit==0`：**没有打开的事务就不允许有保存点**。
- `OP_AutoCommit` 成功切换后调 `sqlite3CloseSavepoints(db)` 关闭所有保存点。
- `sqlite3VdbeAssertMayAbort`（`src/vdbeaux.c`）是编译期的一致性检查器，`src/build.c` 的 `sqlite3FinishCoding` 用 `assert( !pParse->isMultiWrite || sqlite3VdbeAssertMayAbort(v, pParse->mayAbort) )` 校验「标记了多行写就必须标记可能中止」。

### `writeMask` 的下游影响

`writeMask` 不只影响事务指令，还影响查询计划。`src/where.c` 的 `sqlite3WhereMightRerunAfterWrite` 用 `DbMaskNonZero(sqlite3ParseToplevel(pWInfo->pParse)->writeMask)` 判断「本次查询是否会因写入而重跑子查询」，进而决定 Bloom filter / 自动索引是否可能过期；`src/wherecode.c` 中 `DbMaskAllZero(sqlite3ParseToplevel(pParse)->writeMask)` 为真时才启用某个 OP_DeferredSeek 的优化路径。

## 关键取舍

- **显式 BEGIN 与隐式写事务走两条完全不同的路径**：`sqlite3BeginTransaction` 直接追加指令，`sqlite3BeginWriteOperation` 只置编译期位标志。代价是同一个语义（开写事务）在源码里有两处表达，读代码时必须区分「这条 `OP_Transaction` 是谁发出的」。
- **`DEFERRED` 不发 `OP_Transaction`**：`sqlite3BeginTransaction` 里 `if( type!=TK_DEFERRED )` 的判断意味着 `BEGIN DEFERRED` 只发 `OP_AutoCommit`，不预先在任一库上开事务。好处是延迟到第一次读/写时才真正开锁，代价是「BEGIN DEFERRED 之后锁什么时候被拿」不由 BEGIN 语句本身决定。
- **只读库被强制降级为只读事务**：`sqlite3BeginTransaction` 里 `sqlite3BtreeIsReadonly(pBt)` 命中时 `eTxnType=0`，即使用户写的是 `BEGIN EXCLUSIVE`，该库也只开只读事务——**权限降级发生在代码生成期，不是执行期**。
- **触发器内的写操作登记到顶层语句**：`sqlite3BeginWriteOperation` 用 `sqlite3ParseToplevel(pParse)` 而非 `pParse` 本身。好处是触发器子程序触发的写不会漏掉写事务标记，代价是子程序无法独立控制事务粒度。
- **`setStatement` 用「按位或」而非「置位」**：`pToplevel->isMultiWrite |= setStatement` 允许先以 0 登记「该库要开写事务」再由别处以 1 升级，反过来则不行——`isMultiWrite` 一旦被置 1 不会被清 0。
- **`mayAbort` 只置不清**：源码注释明确记录了当年的取舍——「技术上只需在 isMultiWrite 已被设置时才设置 mayAbort 标志……利用这个时间依赖会让某些涉及 REPLACE 冲突解决的语句快一点，但取这个时间依赖会让证明代码正确变得更困难（尤其妨碍写出一个有效的 sqlite3AssertMayAbort() 实现），所以我们选择走安全路线、跳过这个优化。」
- **保存点隐式开启事务**：`OP_Savepoint` 在 `db->autoCommit` 为真时把 `db->autoCommit` 置 0 并标记 `db->isTransactionSavepoint = 1`。好处是 `SAVEPOINT` 在自动提交模式下也有事务语义，代价是「保存点数」与「事务数」在源码里不是同一套计数。
- **保存点与写入语句互斥**：`OP_Savepoint` 在 `db->nVdbeWrite>0` 时返回 `SQLITE_BUSY`。这是显式的互斥检查，不是靠锁等待。

## 与主核 / AI 的关系

本机制是 SQLite「事务」这一概念在编译期与执行期之间的**翻译层**：SQL 文本的 `BEGIN`/`SAVEPOINT` 关键字在这里被降解为具体的 opcode 与具体的掩码位。

**架构推论：** 主数据库内核（`engine/db/`）的事务层若对标这套设计，值得借鉴的是「把事务意图的登记（编译期位掩码）与事务的实际开启（执行期指令）彻底分开」这一分层——`sqlite3BeginWriteOperation` 只置 `writeMask` 位，`sqlite3FinishCoding` 才据此发出 `OP_Transaction`。这个分层让「哪些库需要写事务」这一信息可以被多处复用：事务指令、查询计划的 `sqlite3WhereMightRerunAfterWrite` 判定、`wherecode.c` 的优化开关都读同一份掩码。

**架构推论：** `sqlite3MayAbort` 与 `sqlite3MultiWrite` 的「只置不清 + 走安全路线」取舍，对应到主核的 `engine/db/txn/` 层，是一个可直接借用的设计准则——当「是否需要语句级回滚」这一判定存在时序依赖时，宁可多开回滚点也不要引入难以证明正确的优化。**架构推论：** AI 辅助阅读这一层代码时，需要注意 `OP_Transaction` 有两个来源（`sqlite3BeginTransaction` 直接追加、`sqlite3FinishCoding` 按 `writeMask` 追加），只看 opcode 列表无法判断事务是显式开的还是隐式开的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/build.c | `sqlite3BeginTransaction` | 生成 BEGIN 语句的 VDBE 代码：先做 `SQLITE_TRANSACTION` 授权检查；`type!=TK_DEFERRED` 时遍历 `db->nDb` 个库，按 `sqlite3BtreeIsReadonly`/`TK_EXCLUSIVE` 决定 `eTxnType`（0 只读 / 1 写 / 2 排他），发出 `OP_Transaction` 并调 `sqlite3VdbeUsesBtree`；最后发出 `OP_AutoCommit` |
| src/build.c | `sqlite3BeginWriteOperation` | 编译期的写事务登记：取 `sqlite3ParseToplevel`，调 `sqlite3CodeVerifySchemaAtToplevel`，置 `writeMask` 的 iDb 位，`isMultiWrite |= setStatement`；不直接追加任何 opcode |
| src/build.c | `sqlite3EndTransaction` | 生成 COMMIT/ROLLBACK 的 VDBE 代码：`sqlite3VdbeAddOp2(v, OP_AutoCommit, 1, isRollback)`，其中 `isRollback = eType==TK_ROLLBACK` |
| src/build.c | `sqlite3Savepoint` | 生成保存点的 VDBE 代码：`SQLITE_SAVEPOINT` 授权检查后发出 `sqlite3VdbeAddOp4(v, OP_Savepoint, op, 0, 0, zName, P4_DYNAMIC)` |
| src/build.c | `sqlite3FinishCoding` | 语句收尾：按 `writeMask` 发出 `OP_Transaction`（P2 取 `DbMaskTest(pParse->writeMask,iDb)`），并用 `assert( !pParse->isMultiWrite \|\| sqlite3VdbeAssertMayAbort(v, pParse->mayAbort) )` 校验一致性 |
| src/build.c | `sqlite3MultiWrite` | 置 `pToplevel->isMultiWrite = 1`，表示本语句可能改写/插入多行，出错时需要撤销已完成的写入 |
| src/build.c | `sqlite3MayAbort` | 置 `pToplevel->mayAbort = 1`，表示本语句可能抛出 ABORT 异常 |
| src/build.c | `sqlite3CodeVerifySchemaAtToplevel` | 置 `cookieMask` 的 iDb 位；`iDb==1` 且未定义 `SQLITE_OMIT_TEMPDB` 时调 `sqlite3OpenTempDatabase` |
| src/build.c | `sqlite3HaltConstraint` | `onError==OE_Abort` 时调 `sqlite3MayAbort(pParse)`，再发出 `OP_Halt` |
| src/build.c | `sqlite3StartTable` | 建表路径，调用方之一：`sqlite3BeginWriteOperation(pParse, 1, iDb)` |
| src/build.c | `sqlite3CodeDropTable` | 删表路径，调用方之一：`sqlite3BeginWriteOperation(pParse, 1, iDb)` |
| src/build.c | `sqlite3DropTable` | 删表入口，调用方之一：`sqlite3BeginWriteOperation(pParse, 1, iDb)` |
| src/build.c | `sqlite3CreateIndex` | 建索引路径，调用方之一：`sqlite3BeginWriteOperation(pParse, 1, iDb)` |
| src/build.c | `sqlite3DropIndex` | 删索引路径，调用方之一：`sqlite3BeginWriteOperation(pParse, 1, iDb)` |
| src/build.c | `sqlite3CodeRefillIndex` | REINDEX 路径，调用方之一：`sqlite3BeginWriteOperation(pParse, 0, iDb)` |
| src/parse.y | `cmd ::= BEGIN transtype(Y) trans_opt` | 语法动作 `{sqlite3BeginTransaction(pParse, Y);}`，把 BEGIN 交给代码生成器 |
| src/parse.y | `transtype` | 产生 `TK_DEFERRED`（缺省）/ `TK_IMMEDIATE` / `TK_EXCLUSIVE` 三种事务类型 token |
| src/parse.y | `cmd ::= SAVEPOINT nm(X)` / `cmd ::= RELEASE savepoint_opt nm(X)` / `cmd ::= ROLLBACK trans_opt TO savepoint_opt nm(X)` | 三条语法动作，分别调 `sqlite3Savepoint(pParse, SAVEPOINT_BEGIN/RELEASE/ROLLBACK, &X)` |
| src/sqliteInt.h | `SAVEPOINT_BEGIN` / `SAVEPOINT_RELEASE` / `SAVEPOINT_ROLLBACK` | 三个常量，取值 0 / 1 / 2，既是 `sqlite3Savepoint` 的第二参，也是 `OP_Savepoint` 的 P1 |
| src/sqliteInt.h | `struct Savepoint` | 保存点结构：`zName`、`nDeferredCons`、`nDeferredImmCons`、`pNext` |
| src/sqliteInt.h | `struct Parse` | 承载 `isMultiWrite`、`mayAbort`、`writeMask`、`cookieMask` 四个编译期事务状态字段 |
| src/sqliteInt.h | `DbMaskSet` / `DbMaskTest` / `DbMaskNonZero` / `DbMaskAllZero` | 库位掩码操作宏；`SQLITE_MAX_ATTACHED` 大于 30 时掩码为字节数组，否则为 `unsigned int` |
| src/sqliteInt.h | `sqlite3ParseToplevel` | 宏 `(p)->pToplevel ? (p)->pToplevel : (p)`；`SQLITE_OMIT_TRIGGER` 下退化为恒等宏 `p` |
| src/sqliteInt.h | `SQLITE_MAX_DB` | 宏 `(SQLITE_MAX_ATTACHED+2)`，即库总数上限 |
| src/sqliteLimit.h | `SQLITE_MAX_ATTACHED` | 附加库数量上限，`#ifndef` 保护，缺省值 **10** |
| src/vdbe.c | `OP_Transaction` | 开事务指令：P1 库号，P2 为 0 开只读、非 0 开写、大于等于 2 开排他；P5 非 0 时校验 P3 的 schema cookie 与 P4 的 generation 计数 |
| src/vdbe.c | `OP_AutoCommit` | 切换自动提交状态：P2 非 0 时 `sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)`；`desiredAutoCommit` 为真而 `db->nVdbeWrite>0` 时返回 `SQLITE_BUSY`；成功后 `sqlite3CloseSavepoints(db)` |
| src/vdbe.c | `OP_Savepoint` | 按 P1 执行建立/释放/回滚保存点；`SAVEPOINT_BEGIN` 时若 `db->nVdbeWrite>0` 返回 `SQLITE_BUSY`，并把 `db->autoCommit` 置 0、`db->isTransactionSavepoint` 置 1 |
| src/vdbeaux.c | `sqlite3VdbeUsesBtree` | 置 `p->btreeMask` 的第 i 位；`i!=1` 且 `sqlite3BtreeSharable` 为真时额外置 `p->lockMask` 的第 i 位 |
| src/vdbeaux.c | `sqlite3VdbeAssertMayAbort` | 编译期一致性检查：遍历 VDBE 指令，核对「可能抛 ABORT 的指令」与 `mayAbort` 标志是否一致 |
| src/btree.c | `sqlite3BtreeIsReadonly` | 返回 `(p->pBt->btsFlags & BTS_READ_ONLY)!=0`，供 `sqlite3BeginTransaction` 判定事务类型 |
| src/auth.c | `sqlite3AuthCheck` | 授权闸门：`xAuth==0` 或 `init.busy` 非零时直接返回 `SQLITE_OK`，否则转 `realAuthCheck` |
| src/sqlite.h.in | `SQLITE_TRANSACTION` | 授权动作码 **22**，用于 BEGIN / COMMIT / ROLLBACK 的授权检查 |
| src/sqlite.h.in | `SQLITE_SAVEPOINT` | 授权动作码 **32**，用于保存点相关语句的授权检查 |

## 参考

- [BEGIN TRANSACTION（sqlite.org 官方文档）](https://sqlite.org/lang_transaction.html)
- [SAVEPOINT（sqlite.org 官方文档）](https://sqlite.org/lang_savepoint.html)
- [SQLite VDBE 指令参考（OP_Transaction / OP_AutoCommit / OP_Savepoint）](https://sqlite.org/opcode.html)
- [Isolation In SQLite（sqlite.org 官方文档）](https://sqlite.org/isolation.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/build.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/build.c)
- [sqlite/sqlite 仓库中的 src/build.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/build.c)
