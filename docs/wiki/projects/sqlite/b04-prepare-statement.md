---
project: sqlite
feature: prepare-statement
page_type: feature
schema_version: 1
refs:
  - path: src/prepare.c
    symbols: [sqlite3_prepare_v2, sqlite3Prepare]
---

# 语句编译入口

## 一句话职责

`sqlite3_prepare_v2` 把一段 SQL 文本编译成一条可执行的预编译语句（`sqlite3_stmt`）并返回语句句柄：`sqlite3Prepare` 负责真正的编译（建 Parse 上下文、查模式锁、跑解析器、产出 VDBE 程序），外层的 `sqlite3LockAndPrepare` 负责加锁、重试与错误码收敛。

## 机制是什么

边界（依据条目 `boundary`）：负责 prepare 接口的对外语义：把 SQL 文本编译成一条可执行的预编译语句，并返回语句句柄；不负责语法分析本身（B02）、不负责执行时的虚拟机推进（E 组）。

这条机制要回答的问题只有一个：**「一段 SQL 文本，怎么变成一条可以反复 `sqlite3_step()` 的语句句柄？」** 答案落在四个环节上。

1. **谁在编译。** 真正的编译动作在静态函数 `sqlite3Prepare()` 里，它是 `src/prepare.c` 中唯一的编译实现；`sqlite3_prepare`、`sqlite3_prepare_v2`、`sqlite3_prepare_v3` 三个公开接口都只是它的包装层。包装层之间唯一的差别是传下去的 `prepFlags`：源码注释在 `sqlite3_prepare_v2` 上写明「works exactly the same as sqlite3_prepare_v3() with a zero prepFlags parameter」，而 `sqlite3_prepare_v3` 的注释写明「differs from sqlite3_prepare_v2() only in having the extra prepFlags parameter」。

2. **编译产出物。** 编译成功后 `sqlite3Prepare` 把 `sParse.pVdbe` 转型成 `sqlite3_stmt*` 交给调用方（`*ppStmt = (sqlite3_stmt*)sParse.pVdbe`），并调 `sqlite3ErrorClear(db)` 清掉连接上的旧错误。也就是说，SQLite 的「预编译语句」在实现层面就是一个 VDBE 程序（`Vdbe`），句柄只是它的一层公开视图。

3. **SQL 文本是否保留。** 是否保留原始 SQL 文本决定语句在模式变更后能否自动重编译。`sqlite3_prepare_v2` 传入的 `prepFlags` 是 `SQLITE_PREPARE_SAVESQL`（定义在 `src/vdbe.h`，取值 `0x80`，注释为 "Preserve SQL text"）。`sqlite3Prepare` 在编译成功后调 `sqlite3VdbeSetSql(sParse.pVdbe, zSql, (int)(sParse.zTail-zSql), prepFlags)`；该函数（`src/vdbeaux.c`）在 `prepFlags` 没有 `SQLITE_PREPARE_SAVESQL` 位时把 `p->expmask` 和 `p->smimask` 清零，有该位时才 `p->zSql = sqlite3DbStrNDup(p->db, z, n)` 把文本复制进 VDBE。`sqlite3_prepare`（旧版）传的是 `0`，不保留文本。

4. **对外的错误语义。** 编译失败时 `sqlite3Prepare` 把 `sParse.rc` 作为返回码，并按 `sParse.zErrMsg` 是否存在分别调 `sqlite3ErrorWithMsg(db, rc, "%s", sParse.zErrMsg)` 或 `sqlite3Error(db, rc)`，错误消息因此进入连接句柄，可由 `sqlite3_errmsg()` 取出。

## 怎么实现的

### 调用链：四个公开接口汇到一处

`src/prepare.c` 中三个 UTF-8 公开接口的调用方式完全一致：

- `sqlite3_prepare(db, zSql, nBytes, ppStmt, pzTail)`：调 `sqlite3LockAndPrepare(db, zSql, nBytes, 0, 0, ppStmt, pzTail)`，`prepFlags` 传 `0`，`pOld` 传 `0`。
- `sqlite3_prepare_v2(db, zSql, nBytes, ppStmt, pzTail)`：调 `sqlite3LockAndPrepare(db, zSql, nBytes, SQLITE_PREPARE_SAVESQL, 0, ppStmt, pzTail)`。
- `sqlite3_prepare_v3(db, zSql, nBytes, prepFlags, ppStmt, pzTail)`：调 `sqlite3LockAndPrepare(db, zSql, nBytes, SQLITE_PREPARE_SAVESQL|(prepFlags&SQLITE_PREPARE_MASK), 0, ppStmt, pzTail)`，其中 `SQLITE_PREPARE_MASK` 定义在 `src/vdbe.h`，取值 `0x3f`，注释为 "Mask of public flags"——即把用户传的公开标志位与内部标志位隔离开。

UTF-16 版本走 `sqlite3Prepare16()`，它的做法是先把 UTF-16 转成 UTF-8 再调 `sqlite3_prepare()`。

**架构推论：** 三层包装（公开接口 → `sqlite3LockAndPrepare` → `sqlite3Prepare`）把「对外语义」与「编译实现」隔开，使得 v2/v3 的差异只体现为一个 `u32 prepFlags` 的差别，新增标志位不需要新增公开函数。

### sqlite3LockAndPrepare：加锁、重试与错误码收敛

`sqlite3LockAndPrepare()` 是三个公开接口共同经过的一层，它按顺序做五件事：

1. **参数校验。** `*ppStmt = 0`；若 `!sqlite3SafetyCheckOk(db)||zSql==0` 则返回 `SQLITE_MISUSE_BKPT`。`sqlite3SafetyCheckOk()`（`src/util.c`）的判定是 `db->eOpenState != SQLITE_STATE_OPEN` 时返回 0，即连接未打开或已关闭时拒绝。
2. **加锁。** `sqlite3_mutex_enter(db->mutex)` 之后 `sqlite3BtreeEnterAll(db)`，把该连接打开的所有 B-tree 都纳入锁范围。
3. **重试循环。** 循环体是 `rc = sqlite3Prepare(db, zSql, nBytes, prepFlags, pOld, ppStmt, pzTail)`，循环条件为 `(rc==SQLITE_ERROR_RETRY && ALWAYS(cnt<=SQLITE_MAX_PREPARE_RETRY)) || (rc==SQLITE_SCHEMA && (sqlite3ResetOneSchema(db,-1,0), cnt)==1)`。`SQLITE_MAX_PREPARE_RETRY` 定义在 `src/prepare.c` 自身，默认值 `25`，注释写明「Maximum number of times that we will try again to prepare a statement that returns SQLITE_ERROR_RETRY」。`SQLITE_SCHEMA` 分支只允许一次：先调 `sqlite3ResetOneSchema(db,-1,0)` 重置模式，再靠 `cnt==1` 这个条件保证重置后只再试一次——源码注释解释「A schema problem after one schema reset is considered a permanent error」。
4. **解锁与错误码收敛。** `sqlite3BtreeLeaveAll(db)` 之后 `rc = sqlite3ApiExit(db, rc)`（`src/malloc.c`：若 `db->mallocFailed || rc` 则走 `apiHandleError(db, rc)`），再清 `db->busyHandler.nBusy = 0`，最后 `sqlite3_mutex_leave(db->mutex)`。
5. **结果一致性保证。** 循环内的 `assert( rc==SQLITE_OK || *ppStmt==0 )` 和函数末尾的 `assert( rc==SQLITE_OK || (*ppStmt)==0 )` 把「失败时绝不留半成品句柄」写成了断言契约。

### sqlite3Prepare：真正的编译

`sqlite3Prepare()` 的函数体按顺序做以下事情：

1. **建立 Parse 上下文。** 因为性能考虑，函数没有调 `sqlite3ParseObjectInit()`，而是内联了它的等价动作：`memset(PARSE_HDR(&sParse), 0, PARSE_HDR_SZ)`、`memset(PARSE_TAIL(&sParse), 0, PARSE_TAIL_SZ)`，并把 `sParse.pOuterParse = db->pParse; db->pParse = &sParse; sParse.db = db;`。若 `db->mallocFailed` 为真则记 `"out of memory"` 并置 `db->errCode = rc = SQLITE_NOMEM`，跳到 `end_prepare`。
2. **处理重编译。** 若 `pReprepare` 非空，则 `sParse.pReprepare = pReprepare`，并用 `sqlite3_stmt_isexplain((sqlite3_stmt*)pReprepare)` 继承 EXPLAIN 状态；否则 `assert( sParse.pReprepare==0 )`。
3. **持久语句不用 lookaside。** 若 `prepFlags & SQLITE_PREPARE_PERSISTENT`，则 `sParse.disableLookaside++` 并执行 `DisableLookaside`。`SQLITE_PREPARE_PERSISTENT` 定义在 `src/sqlite.h.in`，取值 `0x01`，其官方说明是「a hint to the query planner that the prepared statement will be retained for a long time」，并写明当前实现「acts on this hint by avoiding the use of lookaside memory」。随后 `sParse.prepFlags = prepFlags & 0xff`——只保留低 8 位。
4. **模式锁检查。** 若 `!db->noSharedCache`，则遍历 `db->aDb[0..db->nDb)`，对每个非空 `pBt` 调 `sqlite3BtreeSchemaLocked(pBt)`；非零则用 `sqlite3ErrorWithMsg(db, rc, "database schema is locked: %s", zDb)` 报错并跳到 `end_prepare`。`sqlite3BtreeSchemaLocked()`（`src/btree.c`）的函数体是 `querySharedCacheTableLock(p, SCHEMA_ROOT, READ_LOCK)`，其注释写明：若同一 shared-cache btree 上别的用户对 `sqlite_schema` 表持有排他锁则返回 `SQLITE_LOCKED_SHAREDCACHE`，否则返回 `SQLITE_OK`。函数上方的长注释说明了原因：若在别的连接的未提交模式变更上编译语句，而那些变更之后被回滚，则模式 cookie 检测不到模式变更，「Disaster would follow」。
5. **虚拟表解锁。** 在 `#ifndef SQLITE_OMIT_VIRTUALTABLE` 下，若 `db->pDisconnect` 则调 `sqlite3VtabUnlockList(db)`。
6. **长度上限检查与解析。** 进入条件是 `nBytes>=0 && (nBytes==0 || zSql[nBytes-1]!=0)`——即长度显式给出且最后字节不是 `'\0'`。此时 `mxLen = db->aLimit[SQLITE_LIMIT_SQL_LENGTH]`；若 `nBytes>mxLen` 则 `sqlite3ErrorWithMsg(db, SQLITE_TOOBIG, "statement too long")` 并 `rc = sqlite3ApiExit(db, SQLITE_TOOBIG)` 后跳转。长度合法则用 `sqlite3DbStrNDup(db, zSql, nBytes)` 复制一份（因为解析器会原地改写文本），调 `sqlite3RunParser(&sParse, zSqlCopy)`，再把 `sParse.zTail` 换算回原串偏移 `sParse.zTail = &zSql[sParse.zTail-zSqlCopy]`，最后 `sqlite3DbFree(db, zSqlCopy)`；复制失败时直接把 `sParse.zTail` 设为 `&zSql[nBytes]`。长度未显式给出（`nBytes<0`）时直接 `sqlite3RunParser(&sParse, zSql)`，让解析器自己走到 `'\0'`。
7. **回填 tail 与 SQL 文本。** 若 `pzTail` 非空则 `*pzTail = sParse.zTail`——这就是「一条 SQL 里编译第一条语句、剩下的交给调用方」的能力来源。若 `db->init.busy==0` 则调 `sqlite3VdbeSetSql(sParse.pVdbe, zSql, (int)(sParse.zTail-zSql), prepFlags)` 保存 SQL 文本。
8. **成功/失败分流。** 若 `db->mallocFailed` 则 `sParse.rc = SQLITE_NOMEM_BKPT` 且 `sParse.checkSchema = 0`。判定条件是 `sParse.rc!=SQLITE_OK && sParse.rc!=SQLITE_DONE`：失败分支里，若 `sParse.checkSchema && db->init.busy==0` 则调 `schemaIsValid(&sParse)`；若 `sParse.pVdbe` 非空则 `sqlite3VdbeFinalize(sParse.pVdbe)`；随后 `rc = sParse.rc`，并按 `sParse.zErrMsg` 是否存在分别走 `sqlite3ErrorWithMsg` 或 `sqlite3Error`。成功分支里 `*ppStmt = (sqlite3_stmt*)sParse.pVdbe; rc = SQLITE_OK; sqlite3ErrorClear(db);`。
9. **清理触发器程序。** 循环释放解析期间分配的 `TriggerPrg` 链表：`while( sParse.pTriggerPrg ){ ... sqlite3DbFree(db, pT); }`。
10. **收尾。** 标签 `end_prepare` 处调 `sqlite3ParseObjectReset(&sParse)` 归还 Parse 资源并返回 `rc`。

### schemaIsValid：编译失败时的模式一致性校验

`schemaIsValid(Parse *pParse)` 是 `sqlite3Prepare` 失败分支里唯一被调用的模式校验函数。它遍历 `db->aDb[0..db->nDb)`，对每个非空 `pBt`：

- 若 `sqlite3BtreeTxnState(pBt)==SQLITE_TXN_NONE`，先 `sqlite3BtreeBeginTrans(pBt, 0, 0)` 开一个事务；若返回 `SQLITE_NOMEM` 或 `SQLITE_IOERR_NOMEM` 则 `sqlite3OomFault(db)` 且 `pParse->rc = SQLITE_NOMEM`，随后因 `rc!=SQLITE_OK` 直接返回。
- 用 `sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, (u32 *)&cookie)` 读出磁盘上的模式 cookie，与内存中的 `db->aDb[iDb].pSchema->schema_cookie` 比较；不等则在该库已加载模式（`DbHasProperty(db, iDb, DB_SchemaLoaded)`）时把 `pParse->rc` 置为 `SQLITE_SCHEMA`，并调 `sqlite3ResetOneSchema(db, iDb, 0)` 重置该库的模式。
- 若事务是本函数开的，最后 `sqlite3BtreeCommit(pBt)` 提交掉。

### 错误分支清单

`sqlite3Prepare` 里可返回的错误码及触发条件，全部来自函数体中的判断：

| 错误码 | 触发条件 | 源码位置 |
|---|---|---|
| `SQLITE_NOMEM` | `db->mallocFailed` 为真（进入函数时已失败） | `sqlite3Prepare` 开头的 `db->mallocFailed` 分支 |
| `SQLITE_NOMEM_BKPT` | 解析结束后 `db->mallocFailed` 为真 | `sqlite3Prepare` 的 `if( db->mallocFailed )` 分支 |
| `SQLITE_LOCKED_SHAREDCACHE` | `sqlite3BtreeSchemaLocked(pBt)` 非零，即别的连接对 `sqlite_schema` 持排他锁 | `sqlite3Prepare` 的模式锁检查循环 |
| `SQLITE_TOOBIG` | `nBytes > db->aLimit[SQLITE_LIMIT_SQL_LENGTH]` | `sqlite3Prepare` 的长度上限检查 |
| `SQLITE_SCHEMA` | 失败时 `schemaIsValid` 发现模式 cookie 不匹配 | `schemaIsValid` 内的 cookie 比较分支 |
| `SQLITE_MISUSE_BKPT` | `sqlite3SafetyCheckOk(db)` 为假或 `zSql==0` | `sqlite3LockAndPrepare` 的参数校验 |

长度上限 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]` 的默认值来自 `src/sqliteLimit.h` 中的 `SQLITE_MAX_SQL_LENGTH`，默认值 `1000000000`；`src/main.c` 的 `aHardLimit[]` 数组把它列为 `SQLITE_LIMIT_SQL_LENGTH` 的硬上限，并在编译期检查 `SQLITE_MAX_SQL_LENGTH` 不得小于 `100`、不得大于 `SQLITE_MAX_LENGTH`、不得大于 `2147482624`。

## 关键取舍

**取舍一：三个公开接口共用一个实现，只靠 `prepFlags` 区分。** `sqlite3_prepare`、`sqlite3_prepare_v2`、`sqlite3_prepare_v3` 的差别被压缩成一个 `u32` 参数：`0`、`SQLITE_PREPARE_SAVESQL`、`SQLITE_PREPARE_SAVESQL|(prepFlags&SQLITE_PREPARE_MASK)`。源码注释把这种关系写成了显式的等价性论证（「works exactly the same as...」「differs from... only in having the extra prepFlags parameter」）。代价是 `SQLITE_PREPARE_SAVESQL` 必须与公开标志位在同一个位空间里共存，于是 `src/vdbe.h` 用 `SQLITE_PREPARE_MASK`（`0x3f`）把公开位与内部位隔开。

**取舍二：旧接口不保存 SQL 文本是有意的行为差异。** `sqlite3_prepare` 传 `prepFlags=0`，`sqlite3VdbeSetSql` 因此清掉 `expmask`/`smimask` 且不复制 SQL 文本；源码在 `sqlite3_prepare` 上方的注释明确写出后果：「In the legacy version, the original SQL text is not saved in the prepared statement and so if a schema change occurs, SQLITE_SCHEMA is returned by sqlite3_step(). In the new version, the original SQL text is retained and the statement is automatically recompiled if an schema change occurs.」

**取舍三：重试次数有明确上限，但 `SQLITE_SCHEMA` 只重试一次。** `SQLITE_ERROR_RETRY` 的重试上限是 `SQLITE_MAX_PREPARE_RETRY`（默认 `25`，定义在 `src/prepare.c`），由 `ALWAYS(cnt<=SQLITE_MAX_PREPARE_RETRY)` 守卫；`SQLITE_SCHEMA` 的重试条件 `cnt==1` 配合前置的 `sqlite3ResetOneSchema(db,-1,0)`，含义是「重置模式后再试一次，仍失败即为永久错误」。

**取舍四：模式锁检查与「不持模式锁」的组合。** `sqlite3Prepare` 查模式锁时并不自己加模式锁——源码注释解释，当前线程已经通过 `sqlite3BtreeEnterAll()` 持有了所有 B-tree 的 mutex，别的线程无法在此期间发起新的模式变更，所以只需要确认「没有别人正持锁」。注释还专门指出：`SQLITE_ReadUncommit` 会覆盖多数锁检测，但**不覆盖**模式锁检测。

**取舍五：显式长度与 NUL 结尾两种输入走不同路径。** 判定条件 `nBytes>=0 && (nBytes==0 || zSql[nBytes-1]!=0)` 把「显式长度」和「NUL 结尾」区分开：前者必须复制一份 `zSqlCopy` 再解析（因为解析器会原地改写文本，复制后还要把 `zTail` 换算回原串偏移 `&zSql[sParse.zTail-zSqlCopy]`），后者直接原地解析。

**取舍六：失败时保证不产出半成品句柄。** 失败分支先 `sqlite3VdbeFinalize(sParse.pVdbe)` 释放已生成的 VDBE，再把错误码与消息转移到连接句柄上；成功分支才 `*ppStmt = (sqlite3_stmt*)sParse.pVdbe`。`sqlite3LockAndPrepare` 用两条断言把这个不变量固化下来。

**取舍七：持久语句主动放弃 lookaside。** `SQLITE_PREPARE_PERSISTENT` 会让 `sParse.disableLookaside++` 并执行 `DisableLookaside`。`src/sqlite.h.in` 的官方说明写明当前实现「acts on this hint by avoiding the use of lookaside memory so as not to deplete the limited store of lookaside memory」；`sqlite3ParseObjectReset()` 在收尾时用 `db->lookaside.bDisable -= pParse->disableLookaside` 把计数还回去。代价是持久语句的单次分配成本更高。

**取舍八：错误信息的落点在连接句柄而不是语句句柄。** `sqlite3Prepare` 的成功分支调 `sqlite3ErrorClear(db)`，失败分支调 `sqlite3ErrorWithMsg`/`sqlite3Error`，错误消息挂在 `db` 上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/prepare.c | `sqlite3_prepare_v2` | 公开入口；以 `SQLITE_PREPARE_SAVESQL` 为 prepFlags 调 `sqlite3LockAndPrepare` |
| src/prepare.c | `sqlite3Prepare` | 真正的编译实现：建 Parse 上下文、模式锁检查、长度上限检查、跑解析器、成功/失败分流、清理 TriggerPrg |
| src/prepare.c | `sqlite3LockAndPrepare` | 加锁与重试层：参数校验、`sqlite3BtreeEnterAll`、`SQLITE_ERROR_RETRY` 与 `SQLITE_SCHEMA` 重试、`sqlite3ApiExit` 收敛错误码 |
| src/prepare.c | `sqlite3_prepare` | 旧版公开入口；prepFlags 传 0，不保留 SQL 文本 |
| src/prepare.c | `sqlite3_prepare_v3` | 公开入口；prepFlags 为 `SQLITE_PREPARE_SAVESQL|(prepFlags&SQLITE_PREPARE_MASK)` |
| src/prepare.c | `sqlite3Prepare16` | UTF-16 入口：先转 UTF-8 再编译，并换算 `*pzTail` 指针 |
| src/prepare.c | `schemaIsValid` | 编译失败时的模式 cookie 校验：读 `BTREE_SCHEMA_VERSION`、比对 `schema_cookie`、必要时置 `SQLITE_SCHEMA` 并重置模式 |
| src/prepare.c | `sqlite3Reprepare` | 模式变更后的重编译入口：取回 SQL 文本与 prepFlags 后重走 `sqlite3LockAndPrepare`，再 `sqlite3VdbeSwap` |
| src/prepare.c | `SQLITE_MAX_PREPARE_RETRY` | 宏，默认 `25`：`SQLITE_ERROR_RETRY` 的最大重试次数 |
| src/prepare.c | `sqlite3ParseObjectInit` / `sqlite3ParseObjectReset` | Parse 上下文的初始化与回收；`sqlite3Prepare` 为性能内联了初始化动作 |
| src/tokenize.c | `sqlite3RunParser` | 驱动 LEMON 生成的 LALR(1) 解析器；自身也按 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]` 递减计长，超限置 `SQLITE_TOOBIG` |
| src/btree.c | `sqlite3BtreeSchemaLocked` | 检查同一 shared-cache btree 上是否有别人对 `sqlite_schema` 持排他锁；返回 `SQLITE_LOCKED_SHAREDCACHE` 或 `SQLITE_OK` |
| src/build.c | `sqlite3ResetOneSchema` | 重置指定库（`iDb<0` 时为全部）的模式，置 `DB_ResetWanted` 并清 `DBFLAG_SchemaKnownOk` |
| src/build.c | `pParse->rc = SQLITE_ERROR_RETRY` | 产生 `SQLITE_ERROR_RETRY` 的位置：索引因缺少排序规则被停用 |
| src/vdbe.h | `SQLITE_PREPARE_SAVESQL` | 宏，`0x80`，"Preserve SQL text"；决定 VDBE 是否保存原始 SQL |
| src/vdbe.h | `SQLITE_PREPARE_MASK` | 宏，`0x3f`，"Mask of public flags"；隔离用户可见标志位 |
| src/vdbeaux.c | `sqlite3VdbeSetSql` | 按 prepFlags 决定是否复制 SQL 文本进 VDBE；无 `SQLITE_PREPARE_SAVESQL` 时清 `expmask`/`smimask` |
| src/vdbeaux.c | `sqlite3VdbePrepareFlags` | 取回 VDBE 上的 prepFlags，供 `sqlite3Reprepare` 复用 |
| src/sqlite.h.in | `SQLITE_PREPARE_PERSISTENT` 等 | 公开 prepFlags 定义：`PERSISTENT 0x01`、`NORMALIZE 0x02`、`NO_VTAB 0x04`、`DONT_LOG 0x10`、`FROM_DDL 0x20` |
| src/sqliteLimit.h | `SQLITE_MAX_SQL_LENGTH` | 宏，默认 `1000000000`；经 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]` 生效 |
| src/main.c | `aHardLimit[]` | 硬上限表；`SQLITE_LIMIT_SQL_LENGTH` 项取 `SQLITE_MAX_SQL_LENGTH` |
| src/util.c | `sqlite3SafetyCheckOk` | 连接可用性检查：`db->eOpenState != SQLITE_STATE_OPEN` 即拒绝 |
| src/malloc.c | `sqlite3ApiExit` | 错误码收敛出口：`db->mallocFailed || rc` 时走 `apiHandleError` |
| src/vdbeapi.c | `sqlite3_step` | 执行期入口；`SQLITE_SCHEMA` 时循环调 `sqlite3Reprepare` 重编译，上限 `SQLITE_MAX_SCHEMA_RETRY`（`src/vdbeInt.h` 中默认 `50`） |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层是 `common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`，其中没有与 SQLite 的「编译期一次性产出可执行程序」对位的结构。SQLite 的 prepare 把「解析 + 名称绑定 + 代码生成 + 查询规划」全部压在编译期，执行期只剩 VDBE 指令流推进。主核若采用「运行时解释执行」模型，则索引选择、模式快照失效、语句复用的成本分布会整体不同——同一个多模态查询在两种模型下的端到端时延特征不一样。

**架构推论：** SQLite 在编译期就把名字钉死成「表游标号 + 列号」（详见 B03 名称解析），执行期不再查名字。这一设计让模式 cookie 成为唯一的失效判据：`schemaIsValid` 比对 `BTREE_SCHEMA_VERSION` 与 `pSchema->schema_cookie`，`sqlite3_step` 在遇到 `SQLITE_SCHEMA` 时调 `sqlite3Reprepare` 重编译（重试上限 `SQLITE_MAX_SCHEMA_RETRY`，默认 `50`）。主核若支持运行时 schema 变更与长驻查询，需要同等强度的失效检测，否则会读到已被改写的对象。

**架构推论：** `sqlite3Prepare` 中「模式锁检查」这一段（`sqlite3BtreeSchemaLocked` + "database schema is locked: %s"）暴露了一个嵌入式数据库特有的约束：编译发生在别的连接持有未提交模式变更时，是不安全的。源码注释把后果写得很直白——「Disaster would follow」。AI 侧的风险在于，模型生成的 SQL 会被反复编译（同一模板、不同参数），如果编译点恰好落在别的连接改表的窗口内，就会命中这条路径。嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库，SDK 层若要提供「预编译语句缓存」，就需要把这条错误路径单独暴露出来，而不是笼统归入「prepare 失败」。

**架构推论：** `SQLITE_PREPARE_PERSISTENT` 体现的是「预编译语句的生命周期预期」这一信息对资源分配策略的影响——长期持有的语句禁用 lookaside 内存，避免把有限的 lookaside 池耗尽。AI 侧若做「语句模板缓存」，长期复用的模板与一次性执行的模板在资源策略上应当区别对待，SQLite 用一个标志位就把这个区分传到了分配器层。

## 参考

- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator 的分层）](https://sqlite.org/arch.html)
- [SQLite Documentation Index（语言与编译选项总览）](https://sqlite.org/docs.html)
- [SQLite Result and Error Codes（`SQLITE_SCHEMA` / `SQLITE_TOOBIG` / `SQLITE_LOCKED_SHAREDCACHE` 等）](https://sqlite.org/rescode.html)
- [SQLite Compile-Time Options（`SQLITE_MAX_SQL_LENGTH` 等上限的官方说明）](https://sqlite.org/compile.html)
- [sqlite/sqlite 源码：src/prepare.c](https://github.com/sqlite/sqlite/blob/master/src/prepare.c)
- [sqlite/sqlite 源码：src/vdbe.h（`SQLITE_PREPARE_SAVESQL` / `SQLITE_PREPARE_MASK`）](https://github.com/sqlite/sqlite/blob/master/src/vdbe.h)
- [sqlite/sqlite 源码：src/tokenize.c（`sqlite3RunParser`）](https://github.com/sqlite/sqlite/blob/master/src/tokenize.c)
- [sqlite/sqlite 源码：src/btree.c（`sqlite3BtreeSchemaLocked`）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
