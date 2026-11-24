---
project: sqlite
feature: attach-database
page_type: feature
schema_version: 1
refs:
  - path: src/attach.c
    symbols: [sqlite3Attach, sqlite3Detach, sqlite3FixSrcList, sqlite3FixExpr]
---

# 附加数据库

## 一句话职责

把另一个数据库文件挂到当前连接上，让它成为连接内的一个具名数据库槽位：ATTACH 负责打开文件、登记进 `db->aDb[]` 并加载其模式，DETACH 负责卸载并回收槽位；跨库事务的原子性由主日志（g05）保证，模式加载的细节由 b05 覆盖。

## 机制是什么

SQLite 的一个连接（`sqlite3 *`）可以同时持有多个数据库文件。`db->aDb[]` 数组里的每个槽位就是一个数据库：槽位 0 固定是 `main`，槽位 1 固定是 `temp`，从槽位 2 起是 ATTACH 进来的库。`ATTACH DATABASE` 打开一个新文件并在数组末尾追加一个 `Db` 条目，`DETACH` 把它关掉并从数组里摘掉。

三条硬性规则来自 `src/attach.c` 的函数体：

1. **每个库有自己的名字**，名字在 `db->aDb[i].zDbSName` 里，查找用 `sqlite3DbIsNamed`（大小写不敏感比较 `zDbSName`），并且 `main` 永远可以作为 0 号槽位的别名。
2. **附加的库必须与主库使用相同的文本编码**。`attachFunc` 里判 `pNew->pSchema->file_format && pNew->pSchema->enc!=ENC(db)` 时报 `"attached databases must use the same text encoding as main database"` 并把 `rc` 置为 `SQLITE_ERROR`。
3. **附加库数量有上限**。判定是 `db->nDb>=db->aLimit[SQLITE_LIMIT_ATTACHED]+2`，即 `SQLITE_LIMIT_ATTACHED`（常量值 7，见 `src/sqlite.h.in`）之外还要额外容纳 `main` 与 `temp` 两个槽位；报错信息是 `"too many attached databases - max %d"`，其中 `%d` 打印的就是 `db->aLimit[SQLITE_LIMIT_ATTACHED]`。

**边界**（与页面清单给出的 boundary 一致）：本页负责 ATTACH/DETACH 的语义——把另一个数据库文件挂到当前连接上并参与跨库事务；不负责主日志的原子提交（g05），也不负责模式加载（b05）。

## 怎么实现的

### 编译期：ATTACH 不是一条普通 DDL，而是被翻译成一个 SQL 函数调用

`src/parse.y` 的语法规则把两条语句交给 `src/attach.c`：

```
cmd ::= ATTACH database_kw_opt expr(F) AS expr(D) key_opt(K). { sqlite3Attach(pParse, F, D, K); }
cmd ::= DETACH database_kw_opt expr(D).                  { sqlite3Detach(pParse, D); }
```

`sqlite3Attach` 与 `sqlite3Detach` 各自构造一个 `static const FuncDef`：`sqlite3Attach` 用 `nArg=3`、`zName="sqlite_attach"`、`xSFunc=attachFunc`；`sqlite3Detach` 用 `nArg=1`、`zName="sqlite_detach"`、`xSFunc=detachFunc`。两者都调 `codeAttach`。

`codeAttach` 的执行顺序（读函数体确认）：

1. `sqlite3ReadSchema(pParse)`——**在生成任何代码前先把当前模式读进来**。`sqlite3ReadSchema` 在 `src/prepare.c` 里实现：`if( !db->init.busy )` 时调 `sqlite3Init(db, &pParse->zErrMsg)`，失败时置 `pParse->rc` 并 `pParse->nErr++`。
2. `if( pParse->nErr ) goto attach_end;`——模式加载出错就直接放弃编译。
3. 对 `pFilename`、`pDbname`、`pKey` 三个表达式各调一次 `resolveAttachExpr`。
4. 授权检查：`sqlite3AuthCheck(pParse, type, zAuthArg, 0, 0)`，`type` 分别是 `SQLITE_ATTACH`（值 24）或 `SQLITE_DETACH`（值 25，见 `src/sqlite.h.in`）。`zAuthArg` 只在 `pAuthArg->op==TK_STRING` 时取 `pAuthArg->u.zToken`，否则为 0。
5. 生成代码：`sqlite3GetTempRange(pParse, 4)` 拿 4 个临时寄存器；`sqlite3ExprCode` 把文件名、库名、KEY 分别写进 `regArgs`、`regArgs+1`、`regArgs+2`；`sqlite3VdbeAddFunctionCall(pParse, 0, regArgs+3-pFunc->nArg, regArgs+3, pFunc->nArg, pFunc, 0)` 生成对该 SQL 函数的调用。
6. 追加 `sqlite3VdbeAddOp1(v, OP_Expire, (type==SQLITE_ATTACH))`——ATTACH 时 P1 为 1（只让本条语句过期），DETACH 时 P1 为 0（让所有已准备语句过期）。`OP_Expire` 在 `src/vdbe.c` 里：`if( !pOp->p1 ) sqlite3ExpirePreparedStatements(db, pOp->p2); else p->expired = pOp->p2+1;`。
7. `attach_end` 处 `sqlite3ExprDelete` 释放三个表达式。

`resolveAttachExpr` 是 ATTACH 独有的表达式解析规则：它只作用于表达式的根节点，若 `pExpr->op!=TK_ID` 就走正常的 `sqlite3ResolveExprNames`；若 `pExpr->op==TK_ID` 则直接把 `op` 改成 `TK_STRING`。**这意味着 `ATTACH DATABASE abc AS def` 里 `abc` 与 `def` 被当作字面量字符串，而不是列名或别名**。文件头注释写明这条规则只作用于根节点，所以 `ATTACH DATABASE abc||def AS 'db2'` 会失败——`abc` 与 `def` 都不是根节点，无法解析。

### 运行期：attachFunc —— 真正打开文件

`attachFunc` 是 ATTACH 的运行期实现。函数体开头先取参数：`zFile = sqlite3_value_text(argv[0])`、`zName = sqlite3_value_text(argv[1])`，任一为 NULL 时替换成空字符串。

`attachFunc` 有两条互斥的路，由 `REOPEN_AS_MEMDB(db)` 宏决定——该宏在 `SQLITE_OMIT_DESERIALIZE` 未定义时展开为 `db->init.reopenMemdb`，定义时恒为 0。

**Reopen-as-MemDB 路径**（`db->init.reopenMemdb` 为真时）：这不是真正的 ATTACH，而是 `sqlite3_deserialize()` 借道把 `db->init.iDb` 上的数据库关掉重开成空 MemDB。函数体检查 `sqlite3BtreeTxnState(pNew->pBt)!=SQLITE_TXN_NONE || sqlite3BtreeIsInBackup(pNew->pBt)`，成立则 `rc = SQLITE_BUSY` 并跳到 `attach_error`。否则用 `sqlite3_vfs_find("memdb")` 找 VFS（找不到就直接 `return`，不报错），调 `sqlite3BtreeOpen(pVfs, "x\0", db, &pNewBt, 0, SQLITE_OPEN_MAIN_DB)`，成功后调 `sqlite3SchemaGet` 拿新模式；`db->init.iDb==0` 时还要调 `sqlite3VtabEponymousTableClearAll(db)` 清掉 eponymous 虚表实例（它们持有刚被 `sqlite3BtreeClose` 释放的模式对象指针），并调 `sqlite3ExpirePreparedStatements(db, 1)`。

**真正的 ATTACH 路径**，函数体的判定顺序是：

1. **数量上限**：`if( db->nDb>=db->aLimit[SQLITE_LIMIT_ATTACHED]+2 )`，报 `"too many attached databases - max %d"`。
2. **名字冲突**：遍历 `i=0..db->nDb-1`，任一槽位满足 `sqlite3DbIsNamed(db, i, zName)` 就报 `"database %s is already in use"`。
3. **扩容 `db->aDb[]`**：若 `db->aDb==db->aDbStatic`（还在用静态数组），`sqlite3DbMallocRawNN(db, sizeof(db->aDb[0])*3)` 分配 3 个槽位并 `memcpy` 前 2 个；否则 `sqlite3DbRealloc(db, db->aDb, sizeof(db->aDb[0])*(1+(i64)db->nDb))`。随后 `pNew = &db->aDb[db->nDb]`，`memset(pNew, 0, sizeof(*pNew))`。
4. **解析 URI 并打开文件**：`flags = db->openFlags`（主库打开时的标志，来自 `sqlite3_open_v2` 的 flags，见 `src/main.c`），然后 `sqlite3ParseUri(db->pVfs->zName, zFile, &flags, &pVfs, &zPath, &zErr)`。解析失败时 `rc==SQLITE_NOMEM` 则 `sqlite3OomFault(db)`，用 `sqlite3_result_error(context, zErr, -1)` 报错后直接返回。
5. **按连接级开关裁剪打开权限**：`if( (db->flags & SQLITE_AttachWrite)==0 )` 时清掉 `SQLITE_OPEN_CREATE|SQLITE_OPEN_READWRITE` 并加上 `SQLITE_OPEN_READONLY`；`else if( (db->flags & SQLITE_AttachCreate)==0 )` 时只清掉 `SQLITE_OPEN_CREATE`。两个开关位 `SQLITE_AttachCreate`（`0x00010`）与 `SQLITE_AttachWrite`（`0x00020`）定义在 `src/sqliteInt.h`，默认在 `src/main.c` 的连接初始化里置位（`db->flags |= ... | SQLITE_AttachCreate | SQLITE_AttachWrite`），可用 `SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE` / `SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE` 关闭。
6. **打开 B-tree**：`flags |= SQLITE_OPEN_MAIN_DB`，然后 `sqlite3BtreeOpen(pVfs, zPath, db, &pNew->pBt, 0, flags)`，紧接着 `db->nDb++`，再 `pNew->zDbSName = sqlite3DbStrDup(db, zName)`。

之后的公共收尾段：

- `db->noSharedCache = 0;`。
- `rc==SQLITE_CONSTRAINT` 时改写为 `rc = SQLITE_ERROR` 并置 `zErrDyn = "database is already attached"`。
- `rc==SQLITE_OK` 时：`pNew->pSchema = sqlite3SchemaGet(db, pNew->pBt)`；拿不到模式则 `rc = SQLITE_NOMEM_BKPT`；模式有 `file_format` 且 `enc!=ENC(db)` 则报 `"attached databases must use the same text encoding as main database"`。随后 `sqlite3BtreeEnter(pNew->pBt)`，`sqlite3PagerLockingMode(pPager, db->dfltLockMode)` 把主库的锁模式同步过来，`sqlite3BtreeSecureDelete(pNew->pBt, sqlite3BtreeSecureDelete(db->aDb[0].pBt, -1))` 让新库的 secure-delete 设置跟随主库，`sqlite3BtreeSetPagerFlags(pNew->pBt, PAGER_SYNCHRONOUS_FULL | (db->flags & PAGER_FLAGS_MASK))` 设定 pager 的 synchronous 标志，最后 `sqlite3BtreeLeave(pNew->pBt)`。
- `pNew->safety_level = SQLITE_DEFAULT_SYNCHRONOUS+1;`——`SQLITE_DEFAULT_SYNCHRONOUS` 默认是 2（`src/sqliteInt.h`），加 1 是因为内部 pager 的 `PAGER_SYNCHRONOUS_*` 宏比对外的零基编号大 1（同处注释给出的对照表：OFF=0/1、NORMAL=1/2、FULL=2/3、EXTRA=3/4）。
- `rc==SQLITE_OK && pNew->zDbSName==0` 时 `rc = SQLITE_NOMEM_BKPT`（名字分配失败）。
- `sqlite3_free_filename(zPath)` 释放 URI 解析出来的文件名。

### 运行期：加载新库的模式

`rc==SQLITE_OK` 时进入模式加载段：

- `sqlite3BtreeEnterAll(db)` 锁住所有 btree。
- `db->init.iDb = 0;` —— **把模式加载的游标指向 0 号槽位**（主库），`db->init.iDb` 是 `sqlite3Init` 遍历各库时的进度游标。
- `db->mDbFlags &= ~(DBFLAG_SchemaKnownOk);` 清除"模式已知有效"的标记。
- `#ifdef SQLITE_ENABLE_SETLK_TIMEOUT` 分支里，若 `db->setlkFlags & SQLITE_SETLK_BLOCK_ON_CONNECT` 则用 `sqlite3OsFileControlHint(fd, SQLITE_FCNTL_BLOCK_ON_CONNECT, &val)` 打开阻塞式连接。
- 非 MemDB 路径调 `sqlite3Init(db, &zErrDyn)`。`sqlite3Init` 在 `src/prepare.c` 里：先处理 0 号槽位（`sqlite3InitOne(db, 0, pzErrMsg, 0)`），再从 `db->nDb-1` 向下遍历到 1（`for(i=db->nDb-1; i>0; i--)`），注释写明 **"The 'temp' schema must be last"**（i==1 是 TEMP 库）。`sqlite3InitOne` 只在 `!DbHasProperty(db, i, DB_SchemaLoaded)` 时才真正加载。
- `sqlite3BtreeLeaveAll(db)` 解锁。

### 失败回滚：把一切恢复原状

`rc` 非零时进入回滚段（`if( ALWAYS(!REOPEN_AS_MEMDB(db)) )`）：

- `int iDb = db->nDb - 1;` —— 回滚的是**最后一个**槽位。
- 若 `db->aDb[iDb].pBt` 非零：`sqlite3BtreeClose(db->aDb[iDb].pBt)` 关掉 btree，并把 `pBt` 与 `pSchema` 清零。
- `sqlite3ResetAllSchemasOfConnection(db)` —— **回退到 `sqlite3ResetAllSchemasOfConnection`**（定义在 `src/build.c`）：它遍历所有槽位，`db->nSchemaLock==0` 时调 `sqlite3SchemaClear(pDb->pSchema)`，否则置 `DB_ResetWanted`；随后 `sqlite3ExpirePreparedStatements(db, 0)`、`db->mDbFlags &= ~(DBFLAG_SchemaChange|DBFLAG_SchemaKnownOk)`、`sqlite3VtabUnlockList(db)`，并在 `db->nSchemaLock==0` 时调 `sqlite3CollapseDatabaseArray(db)` 收缩数组。
- `db->nDb = iDb;` 把槽位数退回去。
- 错误消息兜底：`rc==SQLITE_NOMEM || rc==SQLITE_IOERR_NOMEM` 时 `sqlite3OomFault(db)` 并把消息改成 `"out of memory"`；否则若 `zErrDyn==0` 则置为 `"unable to open database: %s"`（`%s` 是文件名）。
- 最后 `goto attach_error`：`sqlite3_result_error(context, zErrDyn, -1)` 报错，`rc` 非零时再 `sqlite3_result_error_code(context, rc)`。

### 运行期：detachFunc —— 卸载

`detachFunc` 是 DETACH 的运行期实现，函数体的判定顺序是：

1. `zName = sqlite3_value_text(argv[0])`，为 NULL 时替换成空字符串。
2. 遍历 `i=0..db->nDb-1` 找匹配项：`pDb->pBt==0` 的槽位跳过；`sqlite3DbIsNamed(db, i, zName)` 命中则 break。
3. `i>=db->nDb`（没找到）报 `"no such database: %s"`。
4. `i<2`（找到的是 main 或 temp）报 `"cannot detach database %s"`。
5. `sqlite3BtreeTxnState(pDb->pBt)!=SQLITE_TXN_NONE || sqlite3BtreeIsInBackup(pDb->pBt)` 成立报 `"database %s is locked"`。`sqlite3BtreeTxnState` 在 `src/btree.c` 里返回 `p->inTrans`（无事务时是 `SQLITE_TXN_NONE`，值 0，见 `src/sqlite.h.in`）；`sqlite3BtreeIsInBackup` 返回 `p->nBackup!=0`。
6. 通过检查后：`detachTempTriggers(db, pDb->pSchema)` → `sqlite3BtreeClose(pDb->pBt)` → `pDb->pBt = 0` → `pDb->pSchema = 0` → **`sqlite3CollapseDatabaseArray(db)`**（定义在 `src/build.c`）：从槽位 2 开始把 `pBt==0` 的空槽摘掉并把后面的条目往前挪，`db->nDb = j`；若 `db->nDb<=2 && db->aDb!=db->aDbStatic` 则把前 2 个槽位拷回 `db->aDbStatic` 并释放堆上的数组。

`detachTempTriggers`（`src/attach.c` 内的 `static` 函数）负责把 TEMP 库里引用了该模式的触发器改挂到 TEMP 模式自身：遍历 `db->aDb[1].pSchema->trigHash`，凡 `pTrig->pTabSchema==pSchema` 就把 `pTrig->pTabSchema = pTrig->pSchema`。

### 库名绑定：sqlite3FixSrcList 与 sqlite3FixExpr

`src/attach.c` 的后半段是"跨库对象归属"机制，与 ATTACH 本身共用一个文件但服务的是视图/索引/触发器的定义。

`sqlite3FixInit` 初始化 `DbFixer`（结构定义在 `src/sqliteInt.h`）：`pFix->zDb = db->aDb[iDb].zDbSName`（目标库名），`pFix->pSchema = db->aDb[iDb].pSchema`（目标模式），`pFix->zType` 是 `"view"` / `"trigger"` / `"index"` 三者之一（用于错误消息），`pFix->bTemp = (iDb==1)`（1 号槽位是 TEMP 库），并挂上两个 walker 回调 `fixExprCb` 与 `fixSelectCb`。

`sqlite3FixSrcList` 把 `SrcList` 包成一个只有 `pSrc` 成员的 `Select`，再交给 `sqlite3WalkSelect(&pFix->w, &s)` 遍历；`sqlite3FixExpr` 则直接 `sqlite3WalkExpr(&pFix->w, pExpr)`。

真正的判定在 `fixSelectCb` 里：

- `int iDb = sqlite3FindDbName(db, pFix->zDb);` —— `sqlite3FindDbName` 在 `src/build.c` 里，从 `db->nDb-1` 向前倒着找 `sqlite3_stricmp(pDb->zDbSName, zName)==0` 的槽位，且 `i==0` 时 `"main"` 永远是主库的合法别名；找不到返回 -1。
- 对每个源项：若不是 TEMP、不是子查询、且 `pItem->fg.fixedSchema==0 && pItem->u4.zDatabase!=0`（即源项带了显式库名限定），则检查 `iDb != sqlite3FindDbName(db, pItem->u4.zDatabase)`——**显式限定到别的库就报错** `"%s %T cannot reference objects in database %s"`（`%s` 是 `pFix->zType`，`%T` 是 `pFix->pName`）。检查通过则 `sqlite3DbFree(db, pItem->u4.zDatabase)` 释放这个库名字符串，并置 `pItem->fg.notCte = 1`、`pItem->fg.hadSchema = 1`。
- 无论上面是否命中，最后都把 `pItem->u4.pSchema = pFix->pSchema`、`pItem->fg.fromDDL = 1`、`pItem->fg.fixedSchema = 1` —— **把未限定的表引用钉到目标库的模式上**。
- 若源项带 `USING` 子句（`pList->a[i].fg.isUsing==0` 为假），而 `sqlite3WalkExpr(&pFix->w, pList->a[i].u3.pOn)` 成功，则返回 `WRC_Abort` 拒绝。
- 递归处理 CTE：`pSelect->pWith` 里的每个 CTE 的 `pSelect` 都再走一次 `sqlite3WalkSelect`。

`fixExprCb` 处理表达式侧：`if( !pFix->bTemp ) ExprSetProperty(pExpr, EP_FromDDL);` 给表达式打上"来自 DDL"的标记；若 `pExpr->op==TK_VARIABLE`（绑定参数），在 `pFix->pParse->db->init.busy` 为真时把 `op` 改成 `TK_NULL`，否则报 `"%s cannot use variables"`（`%s` 是 `pFix->zType`）并返回 `WRC_Abort`。**TEMP 库里的对象允许引用任何库**，这正是 `pFix->bTemp` 的作用——TEMP 库是 `bTemp==1`，它的对象不参与跨库限制。

**真实调用方**（`sqlite3FixSrcList` / `sqlite3FixExpr` 的实际调用点）：

| 符号 | 调用点 | 调用方式 |
|---|---|---|
| `sqlite3FixSrcList` | `src/trigger.c` 的 `sqlite3BeginTrigger` | `sqlite3FixInit(&sFix, pParse, iDb, "trigger", pName)` 后调 `sqlite3FixSrcList(&sFix, pTableName)`，把触发器体的表名钉到触发器所在库。 |
| `sqlite3FixSrcList` | `src/build.c` 的 `sqlite3CreateIndex` | `sqlite3FixInit(&sFix, pParse, iDb, "index", pName)` 后调 `sqlite3FixSrcList(&sFix, pTblName)`；该处注释写明"因为解析器只用单个标识符构造 `pTblName`，`sqlite3FixSrcList` 不会失败"，失败分支里是 `assert(0)`。 |
| `sqlite3FixSrcList` | `src/attach.c` 的 `sqlite3FixTriggerStep` | 遍历每个 `TriggerStep`，对 `pStep->pSrc` 调 `sqlite3FixSrcList(pFix, pStep->pSrc)`，同时走 `sqlite3WalkSelect` / `sqlite3WalkExpr` 处理 SELECT、WHERE、表达式清单，以及（非 `SQLITE_OMIT_UPSERT` 时）Upsert 的四个清单。 |
| `sqlite3FixExpr` | `src/trigger.c` 的 `sqlite3FinishTrigger` | `sqlite3FixInit(&sFix, pParse, iDb, "trigger", &nameToken)` 后 `sqlite3FixTriggerStep(&sFix, pTrig->step_list) || sqlite3FixExpr(&sFix, pTrig->pWhen)`，把触发器的 WHEN 表达式钉到触发器所在库。 |
| `sqlite3FixSelect` | `src/build.c` 的 `sqlite3CreateView` | `sqlite3FixInit(&sFix, pParse, iDb, "view", pName)` 后调 `sqlite3FixSelect(&sFix, pSelect)`，把视图的 SELECT 钉到视图所在库。 |

## 关键取舍

- **ATTACH 被编译成对一个内建 SQL 函数的调用，而不是一条专用 VDBE 指令**。**源码事实：**`sqlite3Attach` 构造 `FuncDef`（`zName="sqlite_attach"`、`nArg=3`、`xSFunc=attachFunc`），`codeAttach` 用 `sqlite3VdbeAddFunctionCall` 生成调用。**架构推论：**代价是文件名、库名、KEY 三个值必须先经 `sqlite3ExprCode` 落到寄存器再由函数取回；换来的是 ATTACH 得以复用表达式求值、授权回调与 VDBE 调用框架这几套现成设施，源码不需要新增指令编号。
- **附加库的名字与槽位绑定，且 `main` 是保留别名**。**源码事实：**`sqlite3DbIsNamed` 用 `sqlite3StrICmp` 比较 `zDbSName`，并额外放行 `iDb==0 && sqlite3StrICmp("main", zName)==0`。**架构推论：**名字是比较用的字符串标识而非文件路径，因此同一个文件可以被两个不同名字附加，SQLite 不做去重。
- **数量上限算的是槽位而不是文件**。**源码事实：**判定是 `db->nDb>=db->aLimit[SQLITE_LIMIT_ATTACHED]+2`，`+2` 是给 `main` 与 `temp` 留的位；`SQLITE_LIMIT_ATTACHED` 的常量值是 7（`src/sqlite.h.in`），硬上限 `SQLITE_MAX_ATTACHED` 默认 10（`src/sqliteLimit.h`），`src/main.c` 的 `sqlite3_limit` 里用 `assert( aHardLimit[SQLITE_LIMIT_ATTACHED]==SQLITE_MAX_ATTACHED )` 把两者绑死。**架构推论：**`+2` 把"用户可感知的附加数"与"内部槽位总数"这两套口径分开，使得 `sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, n)` 的语义是"最多再附加 n 个"而不是"连接里最多 n 个库"。
- **编码必须一致这条检查依赖模式已被读入**。**源码事实：**`attachFunc` 判 `pNew->pSchema->file_format && pNew->pSchema->enc!=ENC(db)`，而 `pNew->pSchema` 由 `sqlite3SchemaGet(db, pNew->pBt)` 得到，该调用发生在 `sqlite3BtreeOpen` 之后；`ENC(db)` 来自 `sqlite3Init` 里的 `ENC(db) = SCHEMA_ENC(db);`（`src/prepare.c`）。**架构推论：**因此 `codeAttach` 必须先 `sqlite3ReadSchema` 把主库模式读进来，`ENC(db)` 才有正确取值，否则比较无从进行。
- **失败时回滚到"最后一个槽位"**。**源码事实：**回滚段取 `int iDb = db->nDb - 1;`，`sqlite3BtreeClose` 之后调 `sqlite3ResetAllSchemasOfConnection(db)` 再 `db->nDb = iDb;`。**架构推论：**回滚只针对最后一个槽位，前提是 ATTACH 期间不会有别的线程往同一个连接上再附加一个库——SQLite 的连接对象本身不是多线程并发的入口。
- **附加库继承主库的若干设置，但同步等级走的是"至少 FULL"**。**源码事实：**`sqlite3PagerLockingMode(pPager, db->dfltLockMode)`、`sqlite3BtreeSecureDelete(...)` 跟随主库；而 `sqlite3BtreeSetPagerFlags(pNew->pBt, PAGER_SYNCHRONOUS_FULL | (db->flags & PAGER_FLAGS_MASK))` 把 `PAGER_SYNCHRONOUS_FULL` 作为基底。**架构推论：**附加库至少按 FULL 同步，跨库事务才有一致的持久性保证；这与主日志（g05）机制相扣。
- **DETACH 必须先搬走 TEMP 触发器的归属**。**源码事实：**`detachFunc` 在 `sqlite3BtreeClose(pDb->pBt)` 之前调 `detachTempTriggers(db, pDb->pSchema)`，后者把 `pTrig->pTabSchema==pSchema` 的 TEMP 触发器的 `pTabSchema` 改指 `pTrig->pSchema`。**架构推论：**若不搬，这些触发器会持有已释放模式的指针。
- **DETACH 靠"把槽位置空再压缩数组"实现删除**。**源码事实：**`detachFunc` 把 `pDb->pBt = 0; pDb->pSchema = 0;` 后调 `sqlite3CollapseDatabaseArray(db)`，后者从槽位 2 起跳过 `pBt==0` 的空槽并前移后面的条目。**架构推论：**删除语义与"标记 + 压缩"两步走，避免了数组中间删除导致的整体搬移；`main`/`temp` 两槽因此天然不可删——`sqlite3CollapseDatabaseArray` 的循环从 `i=j=2` 起。
- **跨库对象引用靠 `DbFixer` 这套 walker 机制统一拦截**。**源码事实：**`sqlite3FixInit` 填好 `DbFixer`（含 `pSchema`、`zDb`、`zType`、`bTemp`），`fixSelectCb` 用 `sqlite3FindDbName` 比较库名并在跨库时返回 `WRC_Abort`，`fixExprCb` 给表达式打 `EP_FromDDL` 标记并拒绝绑定参数。**架构推论：**视图/索引/触发器的定义体都靠这套机制被"钉"在所属库上，ATTACH 之后跨库读数据是允许的，但跨库**定义**对象不是。
- **TEMP 库的对象可以跨库引用，这是显式豁免**。**源码事实：**`sqlite3FixInit` 里 `pFix->bTemp = (iDb==1);`，`fixSelectCb` 的跨库检查条件是 `pFix->bTemp==0 && pItem->fg.isSubquery==0`，`fixExprCb` 里 `if( !pFix->bTemp ) ExprSetProperty(pExpr, EP_FromDDL);`。**架构推论：**TEMP 库是连接私有的临时空间，允许它引用任何库，避免了临时视图/触发器必须重复写全限定名的负担。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/attach.c | sqlite3Attach | `ATTACH` 的编译入口：由 `src/parse.y` 的 `cmd ::= ATTACH database_kw_opt expr(F) AS expr(D) key_opt(K)` 规则调用；构造 `nArg=3` / `zName="sqlite_attach"` / `xSFunc=attachFunc` 的 `FuncDef`，转 `codeAttach`。 |
| src/attach.c | sqlite3Detach | `DETACH` 的编译入口：由 `cmd ::= DETACH database_kw_opt expr(D)` 规则调用；构造 `nArg=1` / `zName="sqlite_detach"` / `xSFunc=detachFunc` 的 `FuncDef`，转 `codeAttach`。 |
| src/attach.c | codeAttach | ATTACH/DETACH 的公共代码生成：先 `sqlite3ReadSchema` 读模式，再对三个表达式各调 `resolveAttachExpr`，再做 `sqlite3AuthCheck`（`SQLITE_ATTACH` / `SQLITE_DETACH`），最后 `sqlite3ExprCode` + `sqlite3VdbeAddFunctionCall` 生成函数调用，并追加 `OP_Expire`。 |
| src/attach.c | resolveAttachExpr | ATTACH 专有的表达式解析：根节点是 `TK_ID` 时直接改成 `TK_STRING`（把 `abc` 当字面量而非列名），否则走 `sqlite3ResolveExprNames`。 |
| src/attach.c | attachFunc | ATTACH 的运行期实现：查 `SQLITE_LIMIT_ATTACHED` 上限、查名字冲突、扩容 `db->aDb[]`、`sqlite3ParseUri` 解析 URI、按 `SQLITE_AttachWrite` / `SQLITE_AttachCreate` 裁剪打开权限、`sqlite3BtreeOpen` 打开文件、`sqlite3SchemaGet` 取模式、校验文本编码与主库一致、加载模式；失败时关闭 btree 并回滚 `db->nDb`。 |
| src/attach.c | detachFunc | DETACH 的运行期实现：按名字找槽位，报 `"no such database"` / `"cannot detach database"` / `"database %s is locked"` 三类错误；成功则搬走 TEMP 触发器归属、关 btree、把槽位置空，再调 `sqlite3CollapseDatabaseArray` 压缩数组。 |
| src/attach.c | sqlite3DbIsNamed | 库名匹配：大小写不敏感比较 `db->aDb[iDb].zDbSName`，并额外放行 `iDb==0` 时的 `"main"` 别名。 |
| src/attach.c | detachTempTriggers | 把 TEMP 库里 `pTabSchema==pSchema` 的触发器改挂到 TEMP 模式自身（`pTrig->pTabSchema = pTrig->pSchema`），避免悬垂指针。 |
| src/attach.c | sqlite3FixInit | 初始化 `DbFixer`：`zDb` / `pSchema` 取自 `db->aDb[iDb]`，`bTemp = (iDb==1)`，挂上 `fixExprCb` 与 `fixSelectCb` 两个 walker 回调。 |
| src/attach.c | sqlite3FixSrcList | 把 `SrcList` 包成 `Select` 后交给 `sqlite3WalkSelect` 遍历，把未限定的表引用钉到目标库模式；跨库限定由 `fixSelectCb` 拒绝。 |
| src/attach.c | sqlite3FixExpr | 表达式侧的固定：交给 `sqlite3WalkExpr` 遍历，由 `fixExprCb` 打 `EP_FromDDL` 标记并拒绝绑定参数（`"%s cannot use variables"`）。 |
| src/attach.c | fixSelectCb | 跨库检查的实际判定：用 `sqlite3FindDbName(db, pFix->zDb)` 与 `sqlite3FindDbName(db, pItem->u4.zDatabase)` 比较，跨库时 `sqlite3ErrorMsg` 报 `"%s %T cannot reference objects in database %s"`；否则释放库名字符串并置 `pItem->u4.pSchema = pFix->pSchema`、`fixedSchema = 1`。 |
| src/attach.c | fixExprCb | 表达式 walker 回调：非 TEMP 时置 `EP_FromDDL`；遇到 `TK_VARIABLE` 时在 `db->init.busy` 下改成 `TK_NULL`，否则报错中止。 |
| src/parse.y | ATTACH / DETACH 语法规则 | `cmd ::= ATTACH database_kw_opt expr(F) AS expr(D) key_opt(K)` 调 `sqlite3Attach`；`cmd ::= DETACH database_kw_opt expr(D)` 调 `sqlite3Detach`；`key_opt` 规则解析可选的 `KEY expr`，缺省时为 0。 |
| src/build.c | sqlite3CollapseDatabaseArray | 压缩 `db->aDb[]`：从槽位 2 起跳过 `pBt==0` 的空槽并前移后面的条目，`db->nDb = j`；`db->nDb<=2` 时把数组还给 `db->aDbStatic`。DETACH 与 `sqlite3ResetAllSchemasOfConnection` 都会调它。 |
| src/build.c | sqlite3ResetAllSchemasOfConnection | ATTACH 失败时的模式回退：清空各库模式、`sqlite3ExpirePreparedStatements(db, 0)`、清 `DBFLAG_SchemaChange`/`DBFLAG_SchemaKnownOk`、`sqlite3VtabUnlockList`，并在无 schema 锁时调 `sqlite3CollapseDatabaseArray`。 |
| src/build.c | sqlite3FindDbName | 按名字找库槽位：从 `db->nDb-1` 向前倒查 `zDbSName`，`i==0` 时 `"main"` 恒为别名；找不到返回 -1。 |
| src/prepare.c | sqlite3Init | 模式加载：先 0 号槽位，再从 `db->nDb-1` 向下到 1（注释写明 TEMP 库必须最后），`ENC(db) = SCHEMA_ENC(db)` 也在此设置。`attachFunc` 与 `sqlite3ReadSchema` 都会调它。 |
| src/prepare.c | sqlite3ReadSchema | `db->init.busy` 为假时调 `sqlite3Init(db, &pParse->zErrMsg)`，失败则置 `pParse->rc` 并 `pParse->nErr++`。`codeAttach` 在生成代码前先调它。 |
| src/sqliteInt.h | SQLITE_AttachCreate / SQLITE_AttachWrite | 连接级开关，值分别为 `0x00010`（允许 ATTACH 创建新库）与 `0x00020`（允许 ATTACH 以写方式打开）；`attachFunc` 用它们裁剪 `SQLITE_OPEN_CREATE` / `SQLITE_OPEN_READWRITE`。 |
| src/sqliteInt.h | SQLITE_DEFAULT_SYNCHRONOUS | 默认同步级别，默认值 2（对应对外的 FULL）；`attachFunc` 用 `SQLITE_DEFAULT_SYNCHRONOUS+1` 初始化新库的 `safety_level`。 |
| src/sqliteInt.h | struct DbFixer | `DbFixer` 结构：`pParse` / `w`（Walker）/ `pSchema` / `bTemp` / `zDb` / `zType` / `pName`，是 `sqlite3Fix*` 系列的上下文。 |
| src/sqlite.h.in | SQLITE_LIMIT_ATTACHED | 可调限制的编号常量，值 7；`attachFunc` 用 `db->aLimit[SQLITE_LIMIT_ATTACHED]` 判上限。 |
| src/sqlite.h.in | SQLITE_ATTACH / SQLITE_DETACH | 授权动作码，值分别为 24 与 25；`codeAttach` 传给 `sqlite3AuthCheck`。 |
| src/sqliteLimit.h | SQLITE_MAX_ATTACHED | 编译期硬上限，默认 10；注释说明取值必须在 0 到 125 之间，因为附加库数量用带符号 8 位整数计数，且要给 `main` 与 `temp` 留 2 个名额。 |
| src/main.c | sqlite3_limit | 用 `aHardLimit` 表限制可调上限，`assert( aHardLimit[SQLITE_LIMIT_ATTACHED]==SQLITE_MAX_ATTACHED )` 把运行期上限与编译期硬上限绑定；运行时默认值即硬上限。 |
| src/main.c | sqlite3ParseUri | URI 解析：`vfs=` 选 VFS，`mode=`（`ro`/`rw`/`rwc`/`memory`）与 `cache=`（`shared`/`private`）改写打开标志；`mode` 请求的权限超出 `limit` 时报 `"%s mode not allowed: %s"` 并返回 `SQLITE_PERM`。`attachFunc` 用它解析 ATTACH 的文件参数。 |
| src/btree.c | sqlite3BtreeOpen | 打开数据库文件的 btree：`:memory:` 或 `SQLITE_OPEN_MEMORY` 置 `BTREE_MEMORY`；`SQLITE_OPEN_MAIN_DB` 配合内存/临时库时改写为 `SQLITE_OPEN_TEMP_DB`。`attachFunc` 与 Reopen-as-MemDB 路径都调它。 |
| src/btree.c | sqlite3BtreeTxnState / sqlite3BtreeIsInBackup | 事务状态与备份状态查询，返回 `p->inTrans` 与 `p->nBackup!=0`；`attachFunc` 与 `detachFunc` 用它们拒绝在事务/备份期间操作。 |
| src/vdbe.c | OP_Expire | 使已准备语句过期：`P1` 为 0 时立即调 `sqlite3ExpirePreparedStatements(db, pOp->p2)`，否则只置 `p->expired = pOp->p2+1`。ATTACH 发 P1=1（只过期自身），DETACH 发 P1=0（过期全部）。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里没有"多库连接"这一层，它的 `engine/db/` 全部围绕单个 storage 实例展开。若主核要支持附加库，`src/attach.c` 定下的三条边界值得抄：一是**元数据登记与文件打开分离**——`attachFunc` 先做上限、名字冲突、权限裁剪这些判定，再做 `sqlite3BtreeOpen`，文件打开失败时的回滚只针对"最后一个槽位"；二是**库名是连接内的比较标识而非路径**——`sqlite3DbIsNamed` 只比 `zDbSName`，`sqlite3FindDbName` 倒序查找，这意味着同一个文件可以被两个名字附加，主核若照抄要注意"同名不同库"和"同库不同名"两种情形；三是**跨库对象定义靠"固定到模式"实现**——`DbFixer` 把未限定的表引用钉到所属库的模式对象上，而不是在名字层面做检查。

**架构推论：** 本页是 AI 检索 SQLite 附加数据库机制时的落点。全部对外入口集中在 `src/attach.c`：`sqlite3Attach` / `sqlite3Detach`（编译期）、`attachFunc` / `detachFunc`（运行期）、`codeAttach`（公共代码生成）、`sqlite3FixInit` / `sqlite3FixSrcList` / `sqlite3FixExpr`（跨库对象归属）。凡是"ATTACH 报 too many attached databases"、"为什么附加库编码必须和主库一样"、"DETACH 为什么报 database is locked"、"视图/触发器能不能跨库引用表"、"ATTACH 之后 URI 的 mode= 参数起什么作用"这类问题，检索落点都应指向 `src/attach.c`，而不是模式加载（b05）、名称解析（b03）、主日志（g05）或触发器编译（k03）。

## 参考

- SQLite 官方文档《ATTACH DATABASE》：<https://sqlite.org/lang_attach.html>
- SQLite 官方文档《DETACH DATABASE》：<https://sqlite.org/lang_detach.html>
- SQLite 官方文档《sqlite3_limit》（`SQLITE_LIMIT_ATTACHED` 的定义与取值范围）：<https://sqlite.org/limit.html>
- SQLite 官方文档《PRAGMA database_list》（列出当前连接上所有已附加的库及其文件名）：<https://sqlite.org/pragma.html#pragma_database_list>
- SQLite 官方 Fossil 源码 `src/attach.c`（trunk，ATTACH/DETACH 与 `sqlite3Fix*` 的全部实现）：<https://sqlite.org/src/file?name=src/attach.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/parse.y`（trunk，ATTACH/DETACH 的 Lemon 语法规则）：<https://sqlite.org/src/file?name=src/parse.y&ci=trunk>
- GitHub 源码镜像 `src/attach.c`：<https://github.com/sqlite/sqlite/blob/master/src/attach.c>
- GitHub 源码镜像 `src/sqliteLimit.h`（`SQLITE_MAX_ATTACHED` 的编译期默认值与取值范围说明）：<https://github.com/sqlite/sqlite/blob/master/src/sqliteLimit.h>
