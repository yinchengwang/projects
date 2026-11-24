---
project: sqlite
feature: triggers
page_type: feature
schema_version: 1
refs:
  - path: src/trigger.c
    symbols: [sqlite3CodeRowTrigger, sqlite3BeginTrigger, sqlite3FinishTrigger, sqlite3DeleteTrigger]
---

# 触发器

## 一句话职责

给 SQLite 的写入路径挂上一套**行级事件回调**：在 INSERT / UPDATE / DELETE 真正落到表之前或之后，按行把预先登记好的触发器程序编译成 VDBE 子程序并调用它；触发器本身的登记（`CREATE TRIGGER`）、删除（`DROP TRIGGER`）与生命周期管理也归这里。

## 机制是什么

触发器在 SQLite 里不是运行期解释的钩子，而是**编译期生成的 VDBE 子程序**：

1. `CREATE TRIGGER` 语法被 `src/parse.y` 的 Lemon 语法拆成两段：`trigger_decl` 规则在解析到 `BEGIN` 之前调 `sqlite3BeginTrigger`，`cmd ::= createkw trigger_decl(A) BEGIN trigger_cmd_list(S) END(Z)` 规则在 `END` 之后调 `sqlite3FinishTrigger`。中间 `BEGIN ... END` 里的每条语句被解析成一个 `TriggerStep`，串成链表。
2. `sqlite3FinishTrigger` 把整条 `CREATE TRIGGER` 语句的原文（`Token all`）写进 `sqlite_schema` 表，`type='trigger'`，同时发一条 `sqlite3VdbeAddParseSchemaOp` 让模式 cookie 递增。
3. DML 编译期，INSERT / UPDATE / DELETE 的代码生成器调用 `sqlite3TriggersExist` 拿到该表上"确实会在本次操作中点燃"的触发器链表和一个 BEFORE/AFTER 掩码；随后用 `sqlite3CodeRowTrigger` 按行逐个把触发器编译成 `OP_Program` 指令。
4. 触发器体的每条语句被 `codeTriggerProgram` 编译进一个独立的 **sub-vdbe**，主 VDBE 用一条 `OP_Program` 调用它，`P4` 是 `SubProgram` 指针。

触发器分 BEFORE / AFTER 两类，常量是 `src/sqliteInt.h` 里的 `TRIGGER_BEFORE`（值 1）与 `TRIGGER_AFTER`（值 2）。视图只能挂 INSTEAD OF 触发器，表只能挂 BEFORE / AFTER 触发器。

**边界**（与页面清单给出的 boundary 一致）：本页负责在表操作前后编译并执行触发器程序，以及触发器的登记与删除；**不负责 DML 的编译**（那属于 d07 / d08 / d09），也**不负责外键**（k02）。外键的级联动作虽然复用 `Trigger` 结构承载，但它的触发器是 `fkActionTrigger` 在 `src/fkey.c` 里手工构造并缓存的，走的是 `sqlite3CodeRowTriggerDirect` 而非本页的通用路径。

## 怎么实现的

### 登记：sqlite3BeginTrigger —— CREATE TRIGGER 的前半段

`sqlite3BeginTrigger` 在 `CREATE TRIGGER` 的 `BEGIN` 之前被调用，负责所有"校验 + 分配骨架"的动作，读函数体确认的判定顺序：

- **TEMP 触发器不能带库名限定**：`if( isTemp )` 分支里 `pName2->n>0` 时报 `"temporary trigger may not have qualified name"`，并把 `iDb` 固定为 1（`db->aDb[1]`，即 TEMP 库）。
- **非 TEMP 时用 `sqlite3TwoPartName` 解析库名**，返回负值即出错。
- **表名与库名不匹配时把表名里的库名抹掉**：`if( db->init.busy && iDb!=1 )` 分支释放 `pTableName->a[0].u4.zDatabase` 并置 0。源码注释写明这是为了兼容 `CREATE TRIGGER attached.demo AFTER INSERT ON attached.tab` 这种历史语法。
- **表是 TEMP 表且触发器名未限定时，`iDb` 被改成 1**（触发器落在 TEMP 库）。判定条件是 `db->init.busy==0 && pName2->n==0 && pTab && pTab->pSchema==db->aDb[1].pSchema`，且被 `IN_RENAME_OBJECT` 排除。
- **表必须存在**：`sqlite3SrcListLookup` 返回 0 时走 `trigger_orphan_error`。
- **虚表不能挂触发器**：`IsVirtual(pTab)` 时报 `"cannot create triggers on virtual tables"`。
- **只读影子表不能挂触发器**：`(pTab->tabFlags & TF_Shadow)!=0 && sqlite3ReadOnlyShadowTables(db)` 时报 `"cannot create triggers on shadow tables"`。
- **触发器名不能与已有对象冲突**：`sqlite3CheckObjectName(pParse, zName, "trigger", pTab->zName, db->aDb[iDb].zDbSName)`。同名触发器已存在时（`sqlite3HashFind(&(db->aDb[iDb].pSchema->trigHash),zName)` 命中），`noErr` 为假报 `"trigger %T already exists"`；`noErr` 为真且 `db->init.busy` 非零时报空串错误消息（源码注释说明由 `corruptSchema()` 生成）；否则调 `sqlite3CodeVerifySchema` 并在调试构建下置 `pParse->ifNotExists = 1`。
- **系统表上不能建触发器**：`sqlite3StrNICmp(pTab->zName, "sqlite_", 7)==0` 时报 `"cannot create trigger on system table"`。这段被 `#if !defined(SQLITE_ALLOW_TRIGGERS_ON_SYSTEM_TABLES)` 包住，源码注释明确标注该编译选项是"experimental and unsupported"。
- **视图与表的触发器时机互斥**：视图上建 BEFORE / AFTER 触发器报 `"cannot create BEFORE trigger on view"` / `"cannot create AFTER trigger on view"`；表上建 INSTEAD OF 触发器报 `"cannot create INSTEAD OF trigger on table"`。
- **授权检查**：`sqlite3AuthCheck` 依次检查 `SQLITE_CREATE_TRIGGER`（表在 TEMP 库或触发器是 TEMP 时改用 `SQLITE_CREATE_TEMP_TRIGGER`）与对 `SCHEMA_TABLE(iTabDb)` 的 `SQLITE_INSERT` 权限。
- **INSTEAD OF 被就地翻译成 BEFORE**：`if (tr_tm == TK_INSTEAD){ tr_tm = TK_BEFORE; }`。源码注释写明理由：视图只支持 INSTEAD OF，翻译成 BEFORE 可以简化其他地方的代码。

通过全部校验后构造 `Trigger` 对象：`pTrigger->op = (u8)op`（`TK_INSERT` / `TK_UPDATE` / `TK_DELETE`），`pTrigger->tr_tm = tr_tm==TK_BEFORE ? TRIGGER_BEFORE : TRIGGER_AFTER`，`pTrigger->pWhen` 由 `sqlite3ExprDup(db, pWhen, EXPRDUP_REDUCE)` 复制，`pTrigger->pColumns` 保存 `UPDATE OF <col-list>` 的列清单，最后挂到 `pParse->pNewTrigger` 上等 `sqlite3FinishTrigger` 接手。

`trigger_orphan_error` 标签处有一段特殊处理：当 `db->init.iDb==1` 时置 `db->init.orphanTrigger = 1`。源码注释引 Ticket #3810 说明场景：TEMP 触发器建在非 TEMP 表上，而该表被**另一个连接**删掉，导致触发器成为孤儿。

### 完成：sqlite3FinishTrigger —— CREATE TRIGGER 的后半段

`sqlite3FinishTrigger` 在 `END` 之后被调用，把 `TriggerStep` 链表挂到触发器上并完成登记，读函数体确认：

- `pTrig->step_list = pStepList`，`pTrig->nSelect = pParse->nSelect`（`nSelect` 是触发器体里 `Select.selId` 的上界，用于避免重编译时 selId 冲突，见 `src/sqliteInt.h` 中 `struct Trigger` 的注释）。
- 遍历链表给每个 `TriggerStep` 回填 `pStep->pTrig = pTrig`，同时统计步数 `n`。
- **步数上限检查**：`if( n>pParse->db->aLimit[SQLITE_LIMIT_TRIGGER_STEPS] )` 报 `"trigger \"%w\" contains too many steps"`。`SQLITE_LIMIT_TRIGGER_STEPS` 的常量值是 14（`src/sqlite.h.in`），对应的硬上限 `SQLITE_MAX_TRIGGER_STEPS` 默认 65000（`src/sqliteLimit.h`），运行时默认值等于硬上限（`src/main.c` 的 `memcpy(db->aLimit, aHardLimit, sizeof(db->aLimit))`）。
- 名字修正用 `sqlite3FixInit` + `sqlite3FixTriggerStep` + `sqlite3FixExpr`，把触发器体里的表名与 WHEN 表达式里的表名绑定到触发器所在的库。
- **只读影子表的额外检查**：`sqlite3ReadOnlyShadowTables(db)` 为真时遍历 `pStep->pSrc`，若某步要写的目标是影子表，报 `"trigger \"%s\" may not write to shadow table \"%s\"`。
- **写入 sqlite_schema**：`sqlite3BeginWriteOperation` 后用 `sqlite3NestedParse` 执行 `INSERT INTO %Q.` LEGACY_SCHEMA_TABLE `VALUES('trigger',%Q,%Q,0,'CREATE TRIGGER %q')`，然后 `sqlite3ChangeCookie` 并发 `sqlite3VdbeAddParseSchemaOp`。
- **初始化模式加载路径**：`if( db->init.busy )` 分支把触发器插入 `db->aDb[iDb].pSchema->trigHash`（`sqlite3HashInsert`），并在其 `pSchema==pTabSchema` 时把它挂到表的 `pTrigger` 链表头部（`pLink->pNext = pTab->pTrigger; pTab->pTrigger = pLink;`）。

### 删除：sqlite3DeleteTrigger 与 DROP TRIGGER

`sqlite3DeleteTrigger` 是递归释放：`sqlite3DeleteTriggerStep` 逐项释放 `pWhere` / `pExprList` / `pSelect` / `pIdList` / `pUpsert` / `pSrc` / `zSpan`，然后释放 `zName` / `table` / `pWhen` / `pColumns` 与 `Trigger` 本体。函数开头有一句短路：`if( pTrigger==0 || pTrigger->bReturning ) return;`——`bReturning` 为真的瞬态触发器（RETURNING 用）不归它释放。

两条 DROP 路径：

- **`sqlite3DropTrigger`（按名字）**：`src/parse.y` 的 `cmd ::= DROP TRIGGER nm(X)` 规则调用它。它先 `sqlite3ReadSchema`，然后从 `OMIT_TEMPDB` 起遍历 `db->nDb`，且 `int j = (i<2) ? i^1 : i;`——**TEMP 库先于 MAIN 库被搜**（注释写明 "Search TEMP before MAIN"）。找不到时 `noErr` 为假报 `"no such trigger: %S"`，否则调 `sqlite3CodeVerifyNamedSchema` 并置 `pParse->checkSchema = 1`。找到后转 `sqlite3DropTriggerPtr`。
- **`sqlite3DropTriggerPtr`（按指针）**：生成 `DELETE FROM %Q.` LEGACY_SCHEMA_TABLE ` WHERE name=%Q AND type='trigger'`，`sqlite3ChangeCookie`，并发一条 `OP_DropTrigger`（P4 是触发器名）。`OP_DropTrigger` 在 `src/vdbe.c` 里执行 `sqlite3UnlinkAndDeleteTrigger(db, pOp->p1, pOp->p4.z)`，把触发器从 `trigHash` 摘掉、从表的 `pTrigger` 链表摘掉，并置 `db->mDbFlags |= DBFLAG_SchemaChange`。

**DROP TABLE 时连带删触发器**：`src/build.c` 的 DROP TABLE 路径调 `sqlite3TriggerList` 拿到该表上全部触发器，逐个调 `sqlite3DropTriggerPtr`。

### 触发器的查找：sqlite3TriggerList 与 sqlite3TriggersExist

`sqlite3TriggerList` 返回"挂在这张表上、会因这张表的操作而点燃"的全部触发器链表：

- 基础是 `pTab->pTrigger`（表自己的触发器链表）。
- **额外把 TEMP 库里的触发器插到链表头部**：遍历 `pParse->db->aDb[1].pSchema->trigHash`，凡满足 `pTrig->pTabSchema==pTab->pSchema && pTrig->table && 0==sqlite3StrICmp(pTrig->table, pTab->zName) && (pTrig->pTabSchema!=pTmpSchema || pTrig->bReturning)` 就插到链表头。
- 另有 `pTrig->op==TK_RETURNING` 的分支，把瞬态 RETURNING 触发器的 `table` 与 `pTabSchema` 改写成当前表并挂上链表。

`sqlite3TriggersExist` 是 DML 代码生成器真正调用的入口：

- **短路条件**：`if( (pTab->pTrigger==0 && !tempTriggersExist(pParse->db)) || pParse->disableTriggers )` 直接返回 0 并把 `*pMask` 置 0。
- 真正的判定在 `triggersReallyExist` 里：只有 `p->op==op` 且 `checkColumnOverlap(p->pColumns, pChanges)` 成立的触发器才计入，其 `tr_tm` 位被或进 `mask`。`checkColumnOverlap` 的语义是：`pIdList`（`UPDATE OF` 列清单）为 NULL 时视为匹配一切；`pEList`（UPDATE 的 SET 子句）为 NULL 时也视为匹配一切。
- **`SQLITE_DBCONFIG_ENABLE_TRIGGER` 关闭时只保留 TEMP 触发器**：`if( (pParse->db->flags & SQLITE_EnableTrigger)==0 && pTab->pTrigger!=0 && sqlite3SchemaToIndex(pParse->db, pTab->pTrigger->pSchema)!=1 )` 分支把链表截断到只剩 TEMP 触发器。`SQLITE_EnableTrigger` 常量值是 `0x00040000`（`src/sqliteInt.h`）。
- `mask` 的取值是 `TRIGGER_BEFORE`（1）与 `TRIGGER_AFTER`（2）的按位或，供调用方判断本次是否需要编译 BEFORE / AFTER 触发器。

### 编译：sqlite3CodeRowTrigger 与 codeRowTrigger

`sqlite3CodeRowTrigger` 是对外的编译入口，遍历触发器链表，对每个满足下面全部条件的触发器生成调用代码：

```
p->op==op || (p->bReturning && p->op==TK_INSERT && op==TK_UPDATE)
&& p->tr_tm==tr_tm
&& checkColumnOverlap(p->pColumns, pChanges)
```

即"操作类型对得上 + 时机对得上 + UPDATE OF 列清单对得上"。第二个条件覆盖 UPSERT 场景：RETURNING 触发器是 INSERT 触发的，但 UPSERT 的 UPDATE 分支也要点燃它。命中后分两条路：

- **普通触发器**调 `sqlite3CodeRowTriggerDirect`。
- **RETURNING 触发器**且 `sqlite3IsToplevel(pParse)` 为真时调 `codeReturningTrigger`——源码注释写明 RETURNING 的代码是**内联生成**的，不走子程序。

`sqlite3CodeRowTriggerDirect` 内部先调 `getRowTrigger` 拿到 `TriggerPrg`，然后在主 VDBE 里发一条 `sqlite3VdbeAddOp4(v, OP_Program, reg, ignoreJump, ++pParse->nMem, (const char *)pPrg->pProgram, P4_SUBPROGRAM)`，并用 `sqlite3VdbeChangeP5(v, (u16)bRecursive)` 设置 P5 位。`bRecursive` 的判定是 `(p->zName && 0==(pParse->db->flags&SQLITE_RecTriggers))`——**它是个真触发器（`zName` 非空）且 `PRAGMA recursive_triggers` 关闭**时置位。`SQLITE_RecTriggers` 常量值是 `0x00002000`（`src/sqliteInt.h`）。

`getRowTrigger` 负责复用：先在顶层 `Parse` 的 `pTriggerPrg` 链表里找 `pTrigger` 与 `orconf` 都匹配的条目，找到就直接复用；找不到才调 `codeRowTrigger` 新建。复用分支里还会检查递归：沿 `pParse->pOuterParse` 往上找，若某层 `p->pTrigCoded==pTrigger` 就调 `sqlite3MayAbort(pParse)`。

`codeRowTrigger` 是真正把触发器体编译成子程序的函数，读函数体确认的关键点：

- **嵌套深度检查**：`for(nDepth=0; pTop->pOuterParse; pTop = pTop->pOuterParse, nDepth++){}` 统计解析层数，`if( nDepth>=db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH] )` 报 `"triggers nested too deep"` 并返回 0。`SQLITE_LIMIT_TRIGGER_DEPTH` 的常量值是 10（`src/sqlite.h.in`），硬上限 `SQLITE_MAX_TRIGGER_DEPTH` 默认 1000（`src/sqliteLimit.h`），运行时默认值等于硬上限。
- **分配顺序**：先 `sqlite3DbMallocZero(db, sizeof(TriggerPrg))` 并挂到顶层 `Parse` 的 `pTriggerPrg` 链表（源码注释说这样做是为了出错时能被统一释放），再 `sqlite3DbMallocZero(db, sizeof(SubProgram))`，最后 `sqlite3VdbeLinkSubProgram`。
- **子解析上下文**：`sqlite3ParseObjectInit(&sSubParse, db)` 初始化一个独立的 `Parse`，设置 `sSubParse.pTriggerTab = pTab`、`sSubParse.pToplevel = pTop`、`sSubParse.pTrigCoded = pTrigger`、`sSubParse.nSelect = pTrigger->nSelect`、`sSubParse.zAuthContext = pTrigger->zName`、`sSubParse.eTriggerOp = pTrigger->op`、`sSubParse.oldmask = 0`、`sSubParse.newmask = 0`。
- **WHEN 子句**：存在 `pTrigger->pWhen` 时复制并 `sqlite3ResolveExprNames` 解析，然后 `sqlite3ExprIfFalse(&sSubParse, pWhen, iEndTrigger, SQLITE_JUMPIFNULL)`——**WHEN 求值为假或 NULL 都跳到程序末尾**。
- **体编译**：`codeTriggerProgram(&sSubParse, pTrigger->step_list, orconf)`。
- **结尾**：解析 `iEndTrigger` 标签后发 `sqlite3VdbeAddOp0(v, OP_Halt)`，然后 `transferParseError` 把子解析的错误搬到父解析。
- **列使用掩码**：`pPrg->aColmask[0] = sSubParse.oldmask`、`pPrg->aColmask[1] = sSubParse.newmask`。

`codeTriggerProgram` 遍历 `TriggerStep` 链表，按 `pStep->op` 分派：

- `TK_UPDATE` → `sqlite3Update`；`TK_INSERT` → `sqlite3Insert`；`TK_DELETE` → `sqlite3DeleteFrom`；默认（`TK_SELECT`）→ `sqlite3Select`，且 `sqlite3SelectDestInit(&sDest, SRT_Discard, 0)` 丢弃结果。
- 前三种分派之后都发一条 `sqlite3VdbeAddOp0(v, OP_ResetCount)` 重置影响行数计数。
- **冲突解决算法的继承规则**：`pParse->eOrconf = (orconf==OE_Default)?pStep->orconf:(u8)orconf;`——外层语句显式给了 ON CONFLICT 就用外层的，否则用触发器步骤自己写的。源码注释给了例子：`CREATE TRIGGER AFTER INSERT ON t1 BEGIN INSERT OR REPLACE INTO t2 VALUES(new.a, new.b); END;` 里，外层 `INSERT INTO t1` 用 REPLACE 策略，`INSERT OR IGNORE INTO t1` 用 IGNORE 策略。

### 触发器步骤的构造

触发器体里的每条语句在解析期被转成 `TriggerStep`（定义见 `src/sqliteInt.h`）：

- `sqlite3TriggerSelectStep`：`op = TK_SELECT`，`orconf = OE_Default`，`zSpan = triggerSpanDup(db, zStart, zEnd)`。
- `sqlite3TriggerInsertStep` / `sqlite3TriggerUpdateStep` / `sqlite3TriggerDeleteStep`：都先调 `triggerStepAllocate` 分配骨架并复制目标表（`sqlite3SrcListDup(db, pTabList, EXPRDUP_REDUCE)`），再各自挂 `pSelect` / `pIdList` / `pUpsert` / `pExprList` / `pWhere`。
- **`triggerStepAllocate` 的关键限制**：当触发器所在的库不是 TEMP 库（`pNew->pSchema!=db->aDb[1].pSchema`）且目标表带了库名限定（`pTabList->a[0].u4.zDatabase`）时，报 `"qualified table names are not allowed on INSERT, UPDATE, and DELETE statements within triggers"`。
- `triggerSpanDup` 把语句原文里的所有空白字符替换成普通空格——这是为了让 `OP_Trace` 打出来的注释与原文一致。

### 触发器体的 ON CONFLICT 与 RAISE

`codeTriggerProgram` 里的 `orconf` 参数来自 `sqlite3CodeRowTrigger` 的 `orconf` 形参，也就是外层 DML 语句的冲突解决算法；`sqlite3CodeRowTriggerDirect` 把它写进 `pPrg->orconf`，并在 `OP_Program` 的 P2 位放 `ignoreJump`——即触发器体里 `RAISE(IGNORE)` 时要跳到的目标指令。

### 真实调用方

| 调用点 | 位置 | 调用方式 |
|---|---|---|
| INSERT 编译 | `src/insert.c` | 先 `sqlite3TriggersExist(pParse, pTab, TK_INSERT, 0, &tmask)`；行写入前 `sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0, TRIGGER_BEFORE, pTab, regCols-pTab->nCol-1, onError, endOfLoop)`，行写入后 `sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0, TRIGGER_AFTER, pTab, regData-2-pTab->nCol, onError, endOfLoop)`。 |
| UPDATE 编译 | `src/update.c` | 先 `sqlite3TriggersExist(pParse, pTab, TK_UPDATE, pChanges, &tmask)`；BEFORE 触发器在 `if( tmask&TRIGGER_BEFORE )` 分支里编译，之后用 `OP_NotFound` / `OP_NotExists` 复查该行是否还在；AFTER 触发器在行更新完成后编译。 |
| DELETE 编译 | `src/delete.c` | 先 `sqlite3TriggersExist(pParse, pTab, TK_DELETE, 0, 0)`；BEFORE 触发器编译后要把游标重新定位到该行；AFTER 触发器在 `sqlite3FkActions` 之后编译。 |
| DROP TABLE 编译 | `src/build.c` | `sqlite3TriggerList(pParse, pTab)` 取出全部触发器，逐个 `sqlite3DropTriggerPtr(pParse, pTrigger)`。 |
| DROP TRIGGER 编译 | `src/parse.y` | `sqlite3DropTrigger(pParse, X, NOERR)`。 |
| 外键级联动作 | `src/fkey.c` | `sqlite3CodeRowTriggerDirect(pParse, pAct, pTab, regOld, OE_Abort, 0)`——动作触发器不走通用 `sqlite3CodeRowTrigger`。 |
| 触发器删除的运行期落地 | `src/vdbe.c` 的 `OP_DropTrigger` | `sqlite3UnlinkAndDeleteTrigger(db, pOp->p1, pOp->p4.z)`。 |
| ALTER TABLE RENAME 路径 | `src/alter.c` | `sqlite3DeleteTrigger(db, pParse->pNewTrigger)`。 |

### 触发器体的执行期语义

- **触发器体是子程序**：`OP_Program` 在 `src/vdbe.c` 里新建一个 `VdbeFrame`，运行时空间为 `ROUND8(sizeof(VdbeFrame)) + nMem*sizeof(Mem) + nCsr*sizeof(VdbeCursor*) + (7+nOp)/8`，其中 `nMem = pProgram->nMem + pProgram->nCsr`。
- **递归触发器的禁用方式**：P5 位置位时，`OP_Program` 沿 `p->pFrame` 往上找 `pFrame->token==pProgram->token` 的帧，找到就 `break` 跳过这次调用。`pProgram->token` 在 `codeRowTrigger` 里被设为 `(void *)pTrigger`。
- **运行期的嵌套深度检查**：`if( p->nFrame>=db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH] )` 时 `rc = SQLITE_ERROR`，报错 `"triggers nested too deep"` 并中止。这是与编译期 `codeRowTrigger` 里的那次检查**并存的第二道闸**。
- **OLD.\* / NEW.\* 寄存器布局**：`sqlite3CodeRowTrigger` 的注释写明——若表有 N 列，`reg+0` 是 `OLD.rowid`，`reg+1..reg+N` 是 OLD 各列，`reg+N+1` 是 `NEW.rowid`，`reg+N+2..reg+N+N+1` 是 NEW 各列。ON DELETE 时 NEW 段不会被访问，ON INSERT 时 OLD 段不会被访问，因此调用方不分配也不填这些寄存器。
- **只加载用到的列**：`sqlite3TriggerColmask` 返回 32 位掩码，bit 0 对应最左列。表列数超过 32 且可能访问到下标大于 32 的列时返回 `0xffffffff`；`bReturning` 的触发器直接返回 `0xffffffff`；视图返回 `0xffffffff`。`src/delete.c` 与 `src/update.c` 都用这个掩码决定哪些列需要真的从 B-tree 读进寄存器，没用到的列填 `OP_Null`。

## 关键取舍

- **触发器编译成子程序而不是运行期解释**。**源码事实：**`codeRowTrigger` 为每个触发器建一个独立 `Parse` 与独立 `SubProgram`，主 VDBE 用 `OP_Program` 调用；`OP_Program` 在运行期要分配 `VdbeFrame` 并做 `ROUND8(sizeof(VdbeFrame))` 对齐的内存布局计算。**架构推论：**换来的是触发器体与主语句共享同一套表达式代码生成与 VDBE 指令语义，代价是每个触发器多一份子程序内存与一次帧切换开销。
- **子程序按 `(触发器, ON CONFLICT 算法)` 复用**。**源码事实：**`getRowTrigger` 在顶层 `Parse` 的 `pTriggerPrg` 链表里找 `pPrg->pTrigger==pTrigger && pPrg->orconf==orconf` 的条目，找到就复用，否则新建。**架构推论：**同一个触发器在外层用不同冲突算法触发时需要两份程序，SQLite 用 `orconf` 作为缓存键把这两份区分开。
- **INSTEAD OF 被翻译成 BEFORE**。**源码事实：**`sqlite3BeginTrigger` 里 `if (tr_tm == TK_INSTEAD){ tr_tm = TK_BEFORE; }`，注释写明"视图只支持 INSTEAD OF，翻译成 BEFORE 可以简化其他地方的代码"。**架构推论：**视图上的 INSTEAD OF 触发器在执行时与普通 BEFORE 触发器走同一条路径，少了一条独立的执行分支。
- **深度限制有两道闸：编译期 + 运行期**。**源码事实：**`codeRowTrigger` 用解析层数 `nDepth` 与 `db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH]` 比；`src/vdbe.c` 的 `OP_Program` 用运行期帧数 `p->nFrame` 与同一个 limit 比。**架构推论：**编译期那道挡的是"触发器套触发器套触发器"的语法嵌套，运行期那道挡的是数据驱动的递归点燃，两者的触发条件不同，所以两道闸都得在。
- **递归触发默认关闭，关闭方式是在帧链上查 token**。**源码事实：**`sqlite3CodeRowTriggerDirect` 里 `bRecursive = (p->zName && 0==(pParse->db->flags&SQLITE_RecTriggers))`，写入 `OP_Program` 的 P5；`OP_Program` 执行时沿 `p->pFrame` 找同 `token` 的帧，找到就跳过调用。**架构推论：**用"同一个触发器对象是否已在帧链上"判定递归，语义精确到单个触发器而不是整个语句。
- **触发器步数有上限，超限在 CREATE 时就报错**。**源码事实：**`sqlite3FinishTrigger` 统计步数 `n`，超过 `db->aLimit[SQLITE_LIMIT_TRIGGER_STEPS]` 报 `"trigger \"%w\" contains too many steps"`；上限常量是 `SQLITE_LIMIT_TRIGGER_STEPS`（值 14），硬上限 `SQLITE_MAX_TRIGGER_STEPS` 默认 65000。**架构推论：**把限制放在登记期而不是执行期，避免了一个巨长触发器在每次 DML 时被反复解析。
- **触发器登记进 `sqlite_schema`，内容是原始 SQL 文本**。**源码事实：**`sqlite3FinishTrigger` 用 `sqlite3NestedParse` 执行 `INSERT INTO ... VALUES('trigger',%Q,%Q,0,'CREATE TRIGGER %q')`，`%q` 填的是 `Token all` 指向的整条语句原文。**架构推论：**存原文而不是存编译产物，意味着模式重载（`db->init.busy` 路径）需要重新解析，代价是每次加载模式都要重跑一遍触发器语法分析。
- **DROP TRIGGER 时 TEMP 库优先于 MAIN 库被搜索**。**源码事实：**`sqlite3DropTrigger` 里 `int j = (i<2) ? i^1 : i;`，注释写明 "Search TEMP before MAIN"。**架构推论：**同名触发器可以同时存在于 TEMP 库和 MAIN 库，解析顺序决定了不带库限定的 DROP TRIGGER 删掉哪一个。
- **只读影子表有两处独立的触发器限制**。**源码事实：**`sqlite3BeginTrigger` 拒绝在影子表上建触发器；`sqlite3FinishTrigger` 拒绝触发器体写入影子表。**架构推论：**前者挡住"触发器挂在影子表上"，后者挡住"触发器去改影子表"，两个方向都得堵，否则影子表的内容可以被旁路修改。
- **外键级联动作复用 Trigger 结构但走独立通道**。**源码事实：**`src/fkey.c` 的 `fkActionTrigger` 手工分配 `sizeof(Trigger) + sizeof(TriggerStep)`，执行时调 `sqlite3CodeRowTriggerDirect` 而不是 `sqlite3CodeRowTrigger`。**架构推论：**级联动作不需要触发器的登记/删除/校验那一整套流程，只借它的"表达式 + 子程序"骨架，少走一遍模式登记。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/trigger.c | sqlite3CodeRowTrigger | DML 编译期触发触发器的对外入口：遍历触发器链表，按"操作类型 + BEFORE/AFTER 时机 + UPDATE OF 列清单"三重条件筛选，命中后转 `sqlite3CodeRowTriggerDirect` 或 `codeReturningTrigger`。 |
| src/trigger.c | sqlite3BeginTrigger | `CREATE TRIGGER` 的前半段：解析库名、校验表存在且非虚表、拒绝系统表与影子表、校验触发器名不冲突、限制视图/表的触发器时机、做授权检查，把 INSTEAD OF 翻译成 BEFORE，构造 `Trigger` 骨架挂到 `pParse->pNewTrigger`。 |
| src/trigger.c | sqlite3FinishTrigger | `CREATE TRIGGER` 的后半段：挂上 `TriggerStep` 链表、统计步数并与 `SQLITE_LIMIT_TRIGGER_STEPS` 比对、把原始 SQL 写进 `sqlite_schema`、发 `sqlite3VdbeAddParseSchemaOp`，初始化模式下把触发器插入 `trigHash` 与表的 `pTrigger` 链表。 |
| src/trigger.c | sqlite3DeleteTrigger | 递归释放触发器：`sqlite3DeleteTriggerStep` 释放每一步的表达式/SELECT/SrcList/Upsert/原文，`bReturning` 为真的瞬态触发器直接跳过。 |
| src/trigger.c | sqlite3TriggerList | 返回挂在这张表上的全部触发器：基础是 `pTab->pTrigger`，再把 TEMP 库中的同名触发器插到链表头部。 |
| src/trigger.c | sqlite3TriggersExist | DML 代码生成器判断"有没有触发器要点燃"的入口：短路条件是表无触发器且无 TEMP 触发器，或 `pParse->disableTriggers`；返回链表并通过 `pMask` 给出 BEFORE/AFTER 掩码。 |
| src/trigger.c | triggersReallyExist | 真正的匹配逻辑：按 `p->op==op` 与 `checkColumnOverlap` 过滤并累积 `tr_tm` 掩码；`SQLITE_EnableTrigger` 关闭时把链表截断到只剩 TEMP 触发器；处理 RETURNING 触发器与 UPSERT 的交叉情形。 |
| src/trigger.c | checkColumnOverlap | 判定 `UPDATE OF <列清单>` 是否与本次 UPDATE 的 SET 子句有交集；列清单为 NULL 或 SET 子句为 NULL 时视为匹配一切。 |
| src/trigger.c | codeRowTrigger | 把一个触发器体编译成子程序：检查解析嵌套深度与 `SQLITE_LIMIT_TRIGGER_DEPTH`、建独立 `Parse` 与 `SubProgram`、编译 WHEN 子句（假或 NULL 都跳到结尾）、调 `codeTriggerProgram`、结尾发 `OP_Halt`、记录 `aColmask` 的 old/new 列使用掩码。 |
| src/trigger.c | codeTriggerProgram | 编译触发器体的每一步：`TK_UPDATE` 走 `sqlite3Update`、`TK_INSERT` 走 `sqlite3Insert`、`TK_DELETE` 走 `sqlite3DeleteFrom`、`TK_SELECT` 走 `sqlite3Select`（结果丢弃）；前三种之后发 `OP_ResetCount`；冲突算法按"外层显式指定优先，否则用步骤自身的"规则决定。 |
| src/trigger.c | sqlite3CodeRowTriggerDirect | 在主 VDBE 里发 `OP_Program` 调用触发器子程序，并把 `SQLITE_RecTriggers` 关闭时的递归禁用标志写入 P5。 |
| src/trigger.c | getRowTrigger | 按 `(触发器, ON CONFLICT 算法)` 在顶层 `Parse` 的 `pTriggerPrg` 链表中复用或新建子程序；复用时沿 `pOuterParse` 检查递归并调 `sqlite3MayAbort`。 |
| src/trigger.c | triggerStepAllocate | 分配 `TriggerStep` 骨架并复制目标表；触发器体里的 INSERT/UPDATE/DELETE 不允许带库名限定的表名。 |
| src/trigger.c | sqlite3DropTrigger | 按名字删除触发器：先 `sqlite3ReadSchema`，再遍历各库搜索（TEMP 先于 MAIN），找不到时报错或走 `sqlite3CodeVerifyNamedSchema`，找到则转 `sqlite3DropTriggerPtr`。 |
| src/trigger.c | sqlite3DropTriggerPtr | 按指针删除：生成 `DELETE FROM ... sqlite_schema WHERE name=... AND type='trigger'`，发 `OP_DropTrigger`，并做 `SQLITE_DROP_TRIGGER` / `SQLITE_DROP_TEMP_TRIGGER` 授权检查。 |
| src/trigger.c | sqlite3UnlinkAndDeleteTrigger | 把触发器从 `trigHash` 与表的 `pTrigger` 链表摘掉、释放，并置 `db->mDbFlags |= DBFLAG_SchemaChange`；由 `OP_DropTrigger` 调用。 |
| src/trigger.c | sqlite3TriggerColmask | 返回 32 位掩码，指示触发器实际访问了 OLD.\* / NEW.\* 的哪些列，供调用方裁剪寄存器填充；视图与 RETURNING 触发器返回 `0xffffffff`。 |
| src/trigger.c | codeReturningTrigger | 为 RETURNING 触发器**内联**生成代码（不走子程序）：展开 `*` 通配符、解析表达式名、把结果行写进一个瞬态游标。 |
| src/trigger.c | sqlite3DeleteTriggerStep | 逐项释放一个 `TriggerStep`：`pWhere` / `pExprList` / `pSelect` / `pIdList` / `pUpsert` / `pSrc` / `zSpan`。 |
| src/sqliteInt.h | struct Trigger | 触发器对象定义：`zName` / `table` / `op`（`TK_DELETE`/`TK_UPDATE`/`TK_INSERT`）/ `tr_tm` / `bReturning` / `nSelect` / `pWhen` / `pColumns` / `pSchema` / `pTabSchema` / `step_list` / `pNext`。 |
| src/sqliteInt.h | struct TriggerStep | 触发器体的一步：`op` / `orconf` / `pTrig` / `pSelect` / `pSrc` / `pWhere` / `pExprList` / `pIdList` / `pUpsert` / `zSpan` / `pNext` / `pLast`。 |
| src/sqliteInt.h | struct TriggerPrg | 触发器子程序的缓存项：`pTrigger` / `pNext` / `pProgram` / `orconf` / `aColmask[2]`（old 与 new 的列使用掩码）。 |
| src/sqliteInt.h | TRIGGER_BEFORE / TRIGGER_AFTER | 触发器时机常量，值分别为 1 与 2；两者可按位或组成掩码。 |
| src/sqliteInt.h | SQLITE_RecTriggers / SQLITE_EnableTrigger | 连接级开关，值分别为 `0x00002000`（是否允许递归触发）与 `0x00040000`（是否启用触发器；关闭后只剩 TEMP 触发器生效）。 |
| src/sqlite.h.in | SQLITE_LIMIT_TRIGGER_DEPTH / SQLITE_LIMIT_TRIGGER_STEPS | 可调限制的编号常量，值分别为 10 与 14；前者是触发器递归深度上限，后者是单个触发器内 SQL 语句条数上限。 |
| src/sqliteLimit.h | SQLITE_MAX_TRIGGER_DEPTH / SQLITE_MAX_TRIGGER_STEPS | 编译期硬上限，默认分别为 1000 与 65000；`src/main.c` 里 `aHardLimit` 用它们填表，运行期 `db->aLimit` 初值即等于硬上限。 |
| src/vdbe.c | OP_Program | 调用触发器子程序：P4 是 `SubProgram`，P5 置位时按 `pProgram->token` 在帧链上查同触发器以禁止递归；`p->nFrame>=db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH]` 时报 `"triggers nested too deep"`。 |
| src/vdbe.c | OP_DropTrigger | 触发器删除的运行期落地：调 `sqlite3UnlinkAndDeleteTrigger(db, pOp->p1, pOp->p4.z)`。 |
| src/parse.y | CREATE TRIGGER 语法规则 | `trigger_decl` 规则调 `sqlite3BeginTrigger`，`cmd ::= createkw trigger_decl(A) BEGIN trigger_cmd_list(S) END(Z)` 规则调 `sqlite3FinishTrigger` 并把整条语句的原文范围传进去。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层（L0 common / L1 storage / L2 index / L3 query / L4 modalities）里没有触发器这一层。若主核要补触发器，SQLite 在 `src/trigger.c` 里定下的三条边界值得抄：一是**判定与执行分离**——`sqlite3TriggersExist` 只在编译期回答"要不要点燃、是 BEFORE 还是 AFTER"，真正的触发器体编译交给 `codeRowTrigger`；二是**触发器体复用表达式与 VDBE 基础设施**——触发器体的每条语句就是一条普通 DML，靠 `sqlite3Insert` / `sqlite3Update` / `sqlite3DeleteFrom` 复用既有代码生成路径，主核不需要另写一套表达式编译；三是**限制要显式且可配置**——`SQLITE_LIMIT_TRIGGER_DEPTH` 与 `SQLITE_LIMIT_TRIGGER_STEPS` 把递归深度与体长度都变成连接级可调参数，且编译期与运行期各设一道闸。

**架构推论：** 本页是 AI 检索 SQLite 触发器机制时的落点。触发器的全部对外入口集中在 `src/trigger.c` 一个文件里：`sqlite3BeginTrigger` / `sqlite3FinishTrigger`（登记与删除）、`sqlite3TriggersExist`（有没有触发器要跑）、`sqlite3CodeRowTrigger`（编译触发调用）、`sqlite3TriggerColmask`（要读哪些列）。凡是"触发器为什么没生效"、"触发器能不能改同一张表"、"触发器嵌套上限是多少"、"TEMP 触发器和 MAIN 触发器同名会怎样"这类问题，检索落点都应指向 `src/trigger.c` 与 `src/vdbe.c` 的 `OP_Program`，而不是 DML 的编译（d07/d08/d09）、外键（k02）或表达式代码生成（c02）。

## 参考

- SQLite 官方文档《CREATE TRIGGER》：<https://sqlite.org/lang_createtrigger.html>
- SQLite 官方文档《Triggers》（含 BEFORE/AFTER/INSTEAD OF 语义、OLD.\* / NEW.\* 伪表、RAISE() 与 `PRAGMA recursive_triggers`）：<https://sqlite.org/triggers.html>
- SQLite 官方文档《PRAGMA recursive_triggers》：<https://sqlite.org/pragma.html#pragma_recursive_triggers>
- SQLite 官方 Fossil 源码 `src/trigger.c`（trunk，触发器子系统的全部实现）：<https://sqlite.org/src/file?name=src/trigger.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/sqliteInt.h`（trunk，`struct Trigger` / `struct TriggerStep` / `struct TriggerPrg` 与 `TRIGGER_BEFORE` / `TRIGGER_AFTER` 定义）：<https://sqlite.org/src/file?name=src/sqliteInt.h&ci=trunk>
- GitHub 源码镜像 `src/trigger.c`：<https://github.com/sqlite/sqlite/blob/master/src/trigger.c>
- GitHub 源码镜像 `src/vdbe.c`（`OP_Program` 与 `OP_DropTrigger` 的 VDBE 指令语义）：<https://github.com/sqlite/sqlite/blob/master/src/vdbe.c>
