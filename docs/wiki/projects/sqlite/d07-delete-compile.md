---
project: sqlite
feature: delete-compile
page_type: feature
schema_version: 1
refs:
  - path: src/delete.c
    symbols: [sqlite3DeleteFrom, sqlite3GenerateRowDelete, sqlite3CodeChangeCount]
---

# DELETE 编译

## 一句话职责

本页负责把一条 `DELETE FROM ... WHERE ...` 语句编译成 VDBE 程序：定位表、决定用「一次性清表」还是「逐行删」两种落地方式、生成待删行的 key 收集循环、生成每一行的删除动作（索引条目删除 + 表条目删除 + 触发器 + 外键动作），并在需要时把「已删除行数」作为语句结果返回。实际的 B-tree 物理删除由 `OP_Delete` / `OP_IdxDelete` 在 VDBE 执行期完成，不在本页范围。

## 机制是什么

边界（依据条目 `boundary`）：负责 DELETE 语句的编译：定位要删的行、生成删除动作、维护变更计数；不负责 B-tree 的实际删除（F 组）、不负责外键级联（K 组）。

### 三条编译路径

`sqlite3DeleteFrom`（`src/delete.c`）在函数体里先做一系列判定，最终收敛到两条互斥的代码生成路径，外加一条「视图」旁路：

| 路径 | 触发条件 | 落地方式 |
|---|---|---|
| 清表路径 | `rcauth==SQLITE_OK && pWhere==0 && !bComplex && !IsVirtual(pTab)`，且（若定义了 `SQLITE_ENABLE_PREUPDATE_HOOK`）`db->xPreUpdateCallback==0` | 对表和每个索引各发一条 `OP_Clear` |
| 逐行删除路径 | 上述条件任一不成立 | WHERE 扫描收集 key，再逐行删 |
| 视图路径 | `isView` 为真 | 视图先被物化成临时表，然后走「只触发 INSTEAD OF 触发器」的删除 |

其中 `bComplex` 的定义是 `pTrigger || sqlite3FkRequired(pParse, pTab, 0, 0)`——即「表上有 DELETE 触发器」或「表被外键引用」。

### 三种删除策略：ONEPASS_OFF / ONEPASS_SINGLE / ONEPASS_MULTI

`sqlite3WhereOkOnePass(pWInfo, aiCurOnePass)` 的返回值 `eOnePass` 决定待删 key 的收集方式。这三个常量定义在 `src/sqliteInt.h`：

- `ONEPASS_OFF`（值 `0`）——「Use of ONEPASS not allowed」，两趟方式：先把 rowid（或主键记录）收进 RowSet / 临时表，再回头逐个删。
- `ONEPASS_SINGLE`（值 `1`）——「ONEPASS valid for a single row update」，一趟方式，最多删一行。
- `ONEPASS_MULTI`（值 `2`）——「ONEPASS is valid for multiple rows」，一趟方式，可删任意多行。

`sqlite3DeleteFrom` 里对它们的处理有明确分工：

- `eOnePass!=ONEPASS_SINGLE` 时调 `sqlite3MultiWrite(pParse)`，即多趟写入模式。
- `eOnePass!=ONEPASS_OFF` 时，不需要收集 key，直接把 key 留在寄存器里；`aToOpen` 数组（长度 `nIdx+2`）记录哪些游标要开，`memset(aToOpen, 1, nIdx+1)` 之后把 `aToOpen[nIdx+1]` 置 0，再把 `aiCurOnePass[0]`/`aiCurOnePass[1]` 对应的两项清 0——即 ONEPASS 已定位到的那两个游标不再重复打开。
- `eOnePass!=ONEPASS_OFF` 时 `addrBypass = sqlite3VdbeMakeLabel(pParse)` 建立跳转标签，循环末尾 `sqlite3VdbeResolveLabel(v, addrBypass)` 落点。
- `eOnePass==ONEPASS_MULTI` 时额外发一条 `OP_Once`，并在 `sqlite3OpenTableAndIndices` 之后用 `sqlite3VdbeJumpHereOrPopInst(v, iAddrOnce)` 把它改成可跳过的一次性守卫。

### rowid 表与 WITHOUT ROWID 表的分岔

`HasRowid(pTab)` 决定 key 的形态：

- **rowid 表**：`pPk=0`，`nPk` 保持初值 `1`。`iRowSet = ++pParse->nMem`，先发一条 `OP_Null` 把 RowSet 置空。key 用 `sqlite3ExprCodeGetColumnOfTable(v, pTab, iTabCur, -1, iKey)` 取 rowid（列号为 `-1`）。两趟方式下 key 用 `OP_RowSetAdd` 收进 RowSet；一趟方式下 `nKey=1`。
- **WITHOUT ROWID 表**：`pPk = sqlite3PrimaryKeyIndex(pTab)`，`nPk = pPk->nKeyCol`，主键值寄存器从 `iPk = pParse->nMem+1` 开始连续占 `nPk` 个。发 `OP_OpenEphemeral(iEphCur, nPk)` 开临时表，并调 `sqlite3VdbeSetP4KeyInfo(pParse, pPk)` 设置 KeyInfo。两趟方式下用 `OP_MakeRecord` 把主键打成一条记录存进 `iKey`，再 `OP_IdxInsert` 写进临时表；`nKey=0`，注释写明「Zero tells OP_Found to use a composite key」。

### 删除动作的固定顺序

`sqlite3GenerateRowDelete`（`src/delete.c`）是单行删除动作的生成器。它的函数头注释写明了三条前置条件：`iDataCur` 是指向表规范数据存储的已打开游标（rowid 表是表本身，WITHOUT ROWID 表是主键索引）；所有索引的读写游标必须已按 `iIdxCur+i` 打开；主键必须存放在从 `iPk` 开始的 `nPk` 个连续内存单元里（`nPk==0` 时表示 `OP_MakeRecord` 生成的搜索记录已放在 `iPk` 这一个单元）。

它内部的动作顺序是固定的：

1. **定位待删行**：`eMode==ONEPASS_OFF` 时发 `OP_NotExists`（rowid 表）或 `OP_NotFound`（WITHOUT ROWID 表）跳到 `iLabel`。函数注释说明：若这一行已经不存在（触发器程序可能已经删过），就不删也不触发 DELETE 触发器。
2. **准备 OLD.\* 伪表**：仅当 `sqlite3FkRequired(...)` 或 `pTrigger` 非 0 时才做。`mask` 由 `sqlite3TriggerColmask(pParse, pTrigger, 0, 0, TRIGGER_BEFORE|TRIGGER_AFTER, pTab, onconf)` 与 `sqlite3FkOldmask(pParse, pTab)` 合并得到；寄存器区间从 `iOld = pParse->nMem+1` 起占 `1 + pTab->nCol` 个。先 `OP_Copy` 复制 `iPk`，再逐列用 `sqlite3ExprCodeGetColumnOfTable` 把 mask 命中的列读进 `iOld+kk+1`，其中 `kk = sqlite3TableColumnToStorage(pTab, iCol)`。mask 的判定是 `mask==0xffffffff || (iCol<=31 && (mask & MASKBIT32(iCol))!=0)`——即列数超过 32 时靠 `0xffffffff` 全量兜底。
3. **BEFORE DELETE 触发器**：`sqlite3CodeRowTrigger(pParse, pTrigger, TK_DELETE, 0, TRIGGER_BEFORE, pTab, iOld, onconf, iLabel)`。若 BEFORE 触发器确实生成了代码（`addrStart<sqlite3VdbeCurrentAddr(v)`），**重发一次定位指令**，并把 `iIdxNoSeek` 置为 `-1` 关闭「免 seek」优化——函数注释给出的理由是「BEFORE 触发器可能移动了游标」。
4. **外键检查**：`sqlite3FkCheck(pParse, pTab, iOld, 0, 0, 0)`，检查引用本表的外键约束是否被违反。
5. **实际删除**（仅当 `!IsView(pTab)`）：先 `sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, 0, iIdxNoSeek)` 删所有索引条目，再发 `OP_Delete` 删表条目。`OP_Delete` 的 P2 是 `count?OPFLAG_NCHANGE:0`，其中 `count = (pParse->nested==0)`。非嵌套时追加 `P4_TABLE`。`eMode!=ONEPASS_OFF` 时补 `OPFLAG_AUXDELETE`；`iIdxNoSeek>=0 && iIdxNoSeek!=iDataCur` 时再发一条 `OP_Delete` 删那个免 seek 的索引游标；`eMode==ONEPASS_MULTI` 时补 `OPFLAG_SAVEPOSITION`。
6. **外键级联动作**：`sqlite3FkActions(pParse, pTab, 0, iOld, 0, 0)`，负责 ON CASCADE / SET NULL / SET DEFAULT。
7. **AFTER DELETE 触发器**：`sqlite3CodeRowTrigger(pParse, pTrigger, TK_DELETE, 0, TRIGGER_AFTER, pTab, iOld, onconf, iLabel)`。
8. **落点**：`sqlite3VdbeResolveLabel(v, iLabel)`——第 1 步的「行已不存在」跳转目标和 `RAISE(IGNORE)` 异常都落到这里。

### 变更计数的产生与返回

变更计数分两处：

- **语句内计数寄存器**：`sqlite3DeleteFrom` 里，条件是 `(db->flags & SQLITE_CountRows)!=0 && !pParse->nested && !pParse->pTriggerTab && !pParse->bReturning` 全部成立时，`memCnt = ++pParse->nMem` 并发一条 `OP_Integer(0, memCnt)`。计数在 WHERE 循环内以 `OP_AddImm(memCnt, 1)` 递增——注意这条指令发在 WHERE 循环体里、`sqlite3ExprCodeGetColumnOfTable` 取 key 之前。
- **结果行**：`sqlite3CodeChangeCount(v, memCnt, "rows deleted")`（`src/delete.c`）生成三条指令：`OP_FkCheck`、`OP_ResultRow(regCounter, 1)`、`sqlite3VdbeSetNumCols(v, 1)`，并用 `sqlite3VdbeSetColName(v, 0, COLNAME_NAME, zColName, SQLITE_STATIC)` 把结果列名设为传入的 `zColName`。调用点传的字符串就是 `"rows deleted"`。

## 怎么实现的

### 第一步：表定位与前置检查

`sqlite3DeleteFrom` 的入口序列：

1. `if( pParse->nErr ) goto delete_from_cleanup;` —— 有前置错误就直接跳过全部生成。
2. `pTab = sqlite3SrcListLookup(pParse, pTabList);` —— `sqlite3SrcListLookup` 在 `src/delete.c` 里，是非静态函数；它内部调 `sqlite3LocateTableItem(pParse, 0, pItem)` 找表，失败返回 0，此时 `sqlite3DeleteFrom` 跳到清理出口。
3. `pTrigger = sqlite3TriggersExist(pParse, pTab, TK_DELETE, 0, 0);`，`isView = IsView(pTab);`。
4. `bComplex = pTrigger || sqlite3FkRequired(pParse, pTab, 0, 0);`。
5. `sqlite3ViewGetColumnNames(pParse, pTab)` 失败则退出。
6. `sqlite3IsReadOnly(pParse, pTab, pTrigger)` 为真则退出。`sqlite3IsReadOnly` 在 `src/delete.c` 里，内部走 `tabIsReadOnly`：虚表走 `vtabIsReadOnly`（无 `xUpdate` 实现即为只读；`eVtabRisk` 高于 `SQLITE_TrustedSchema` 开关值时报 "unsafe use of virtual table"），系统表看 `sqlite3WritableSchema(db)==0 && pParse->nested==0`，影子表看 `sqlite3ReadOnlyShadowTables(db)`。另外视图本身会被拒（报 "cannot modify %s because it is a view"），除非是 `pTrigger->bReturning && pTrigger->pNext==0` 的返回型触发器。
7. **授权检查**：`rcauth = sqlite3AuthCheck(pParse, SQLITE_DELETE, pTab->zName, 0, db->aDb[iDb].zDbSName);`。断言 `rcauth` 只能是 `SQLITE_OK` / `SQLITE_DENY` / `SQLITE_IGNORE` 三者之一。`SQLITE_DENY` 直接退出；`SQLITE_IGNORE` 不退出，但会关掉清表优化——函数注释里标注了对应的实现要求 `R-17228-37124`。

### 第二步：LIMIT/ORDER BY 被改写成子查询

当定义了 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT` 且 `!isView` 时，`sqlite3DeleteFrom` 调 `sqlite3LimitWhere(pParse, pTabList, pWhere, pOrderBy, pLimit, "DELETE")` 把 `WHERE` 改写成 `rowid IN (SELECT rowid FROM ... ORDER BY ... LIMIT ...)`，随后 `pOrderBy = 0; pLimit = 0;`。

`sqlite3LimitWhere` 在 `src/delete.c` 里（非静态，条件编译于 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT && !SQLITE_OMIT_SUBQUERY`）。它的行为：

- **有 ORDER BY 但没有 LIMIT** 时报 "ORDER BY without LIMIT on %s" 并返回 0。
- **没有 LIMIT** 时原样返回 `pWhere`。
- key 表达式形态由表类型决定：`HasRowid(pTab)` 为真时用 `TK_ROW`；否则看 `sqlite3PrimaryKeyIndex(pTab)` 的 `nKeyCol`——等于 1 时用该列名构造 `TK_ID` 表达式，大于 1 时逐列构造 `TK_ID` 列表并用 `TK_VECTOR` 作为左操作数。

### 第三步：视图的物化

`isView` 为真时调 `sqlite3MaterializeView(pParse, pTab, pWhere, pOrderBy, pLimit, iTabCur)`（`src/delete.c`，编译条件是 `!defined(SQLITE_OMIT_VIEW) && !defined(SQLITE_OMIT_TRIGGER)`）。它把视图当成一个 `SRT_EphemTab` 目的地的 SELECT 物化到临时表，然后 `iDataCur = iIdxCur = iTabCur`，并清空 `pOrderBy`/`pLimit`。

### 第四步：游标分配与写事务

- `iTabCur = pTabList->a[0].iCursor = pParse->nTab++;` —— 表本身占一个游标号。
- 紧接着 `for(nIdx=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, nIdx++){ pParse->nTab++; }` —— 每个索引各占一个游标号。注意 `nIdx` 在这个循环里被重新赋值为 0，而 `sqlite3DeleteFrom` 里声明的 `int nIdx;` 在此之前未赋初值。
- `sqlite3VdbeCountChanges(v)` 仅在 `pParse->nested==0` 时调用。
- `sqlite3BeginWriteOperation(pParse, bComplex, iDb)` —— 第二个参数是 `bComplex`，决定写事务的粒度。
- WHERE 名字解析：`sqlite3ResolveExprNames(&sNC, pWhere)`，失败则退出。

### 第五步：清表路径的指令序列

清表路径发出的指令有明确的形态差异：

- **rowid 表**：`sqlite3VdbeAddOp4(v, OP_Clear, pTab->tnum, iDb, memCnt ? memCnt : -1, pTab->zName, P4_STATIC)`——`P3` 是变更计数寄存器，没有计数时填 `-1`。
- **WITHOUT ROWID 表的主键索引**：`sqlite3VdbeAddOp3(v, OP_Clear, pIdx->tnum, iDb, memCnt ? memCnt : -1)`。
- **其他索引**：`sqlite3VdbeAddOp2(v, OP_Clear, pIdx->tnum, iDb)`——`P3` 缺省为 0。

`OP_Clear` 的执行语义（`src/vdbe.c`）：调 `sqlite3BtreeClearTable(db->aDb[pOp->p2].pBt, (u32)pOp->p1, &nChange)`；`if( pOp->p3 )` 时把 `nChange` 累加进 `p->nChange`，且 `pOp->p3>0` 时再把 `nChange` 累加到寄存器 `aMem[pOp->p3].u.i`。这解释了为什么只有主表（和 WITHOUT ROWID 表的主键索引）的 `OP_Clear` 才带计数寄存器——其他索引的清空不重复计数。

清表路径前还会 `sqlite3TableLock(pParse, iDb, pTab->tnum, 1, pTab->zName)`。

### 第六步：WHERE 规划与 key 收集

`sqlite3DeleteFrom` 在逐行路径里调 `sqlite3WhereBegin(pParse, pTabList, pWhere, 0, 0, 0, wcf, iTabCur+1)`。`wcf` 的初值是 `WHERE_ONEPASS_DESIRED|WHERE_DUPLICATES_OK`，再按 `bComplex` 决定是否加 `WHERE_ONEPASS_MULTIROW`：

```
u16 wcf = WHERE_ONEPASS_DESIRED|WHERE_DUPLICATES_OK;
if( sNC.ncFlags & NC_Subquery ) bComplex = 1;
wcf |= (bComplex ? 0 : WHERE_ONEPASS_MULTIROW);
```

注意 `sNC.ncFlags & NC_Subquery` 会在此刻把 `bComplex` 置 1——即 WHERE 子句里含子查询时，ONEPASS 多行模式被禁用。`WHERE_ONEPASS_DESIRED`（`0x0004`）与 `WHERE_ONEPASS_MULTIROW`（`0x0008`）定义在 `src/sqliteInt.h`。

规划结果由 `sqlite3WhereOkOnePass(pWInfo, aiCurOnePass)` 读出。若 `sqlite3WhereUsesDeferredSeek(pWInfo)` 为真，发一条 `OP_FinishSeek(iTabCur)`。

key 的提取在两趟方式下走两条不同分支：

- WITH**OUT** ROWID：`sqlite3ExprCodeGetColumnOfTable` 逐列把 `pPk->aiColumn[i]` 读进 `iPk+i`，`iKey = iPk`。
- rowid 表：`iKey = ++pParse->nMem`，`sqlite3ExprCodeGetColumnOfTable(v, pTab, iTabCur, -1, iKey)`。

随后分两趟/一趟两种收尾：

- **ONEPASS（`eOnePass!=ONEPASS_OFF`）**：`nKey = nPk`（注释：`OP_Found will use an unpacked key`），开 `aToOpen = sqlite3DbMallocRawNN(db, nIdx+2)`（分配失败则 `sqlite3WhereEnd(pWInfo)` 后退出），并把 `addrEphOpen` 处的 `OP_OpenEphemeral` 改成空操作（`sqlite3VdbeChangeToNoop`）。
- **两趟（`eOnePass==ONEPASS_OFF`）**：rowid 表发 `OP_RowSetAdd(iRowSet, iKey)`；WITHOUT ROWID 表发 `OP_MakeRecord` + `OP_IdxInsert(iEphCur, iKey, iPk, nPk)`。两条路径最后都调 `sqlite3WhereEnd(pWInfo)`。

### 第七步：游标打开与删除循环

`sqlite3OpenTableAndIndices(pParse, pTab, OP_OpenWrite, OPFLAG_FORDELETE, iTabCur, aToOpen, &iDataCur, &iIdxCur)` 打开写游标。`OPFLAG_FORDELETE` 是专门为删除场景加的游标标志。

循环的三种入口：

- **ONEPASS 且非视图**：`sqlite3VdbeAddOp4Int(v, OP_NotFound, iDataCur, addrBypass, iKey, nKey)`——找不到就跳到 `addrBypass`（跳过删除）。
- **两趟 + WITHOUT ROWID**：`sqlite3VdbeAddOp1(v, OP_Rewind, iEphCur)` 建循环头；`IsVirtual(pTab)` 为真时用 `OP_Column(iEphCur, 0, iKey)`，否则用 `OP_RowData(iEphCur, iKey)`。
- **两趟 + rowid 表**：`sqlite3VdbeAddOp3(v, OP_RowSetRead, iRowSet, 0, iKey)`。`OP_RowSetRead` 的语义（`src/vdbe.c`）：从 RowSet 里取出最小值放进 `P3`；若 RowSet 为空则 `P3` 不变并跳到 `P2`。

循环尾：

- 两趟 + WITHOUT ROWID：`OP_Next(iEphCur, addrLoop+1)` 加 `sqlite3VdbeJumpHere(v, addrLoop)`。
- 两趟 + rowid 表：`sqlite3VdbeGoto(v, addrLoop)` 加 `sqlite3VdbeJumpHere(v, addrLoop)`。
- ONEPASS：直接 `sqlite3VdbeResolveLabel(v, addrBypass)` 收尾。

### 第八步：虚表的删除走完全不同的指令

`IsVirtual(pTab)` 为真时，删除动作不是 `sqlite3GenerateRowDelete`，而是直接发 `OP_VUpdate(0, 1, iKey, pVTab, P4_VTAB)` 并用 `sqlite3VdbeChangeP5(v, OE_Abort)` 设置 P5。函数里的断言写明虚表只能是 `ONEPASS_OFF` 或 `ONEPASS_SINGLE`，不能是 `ONEPASS_MULTI`。`ONEPASS_SINGLE` 时还会先发 `OP_Close(iTabCur)` 并在 `sqlite3IsToplevel(pParse)` 为真时把 `pParse->isMultiWrite` 置 0。发指令前调 `sqlite3VtabMakeWritable(pParse, pTab)` 与 `sqlite3MayAbort(pParse)`。

### 第九步：索引条目删除的细节

`sqlite3GenerateRowIndexDelete`（`src/delete.c`）遍历 `pTab->pIndex` 链表，对每个索引：

- 跳过主键索引（`if( pIdx==pPk ) continue;`，`pPk` 在 rowid 表上为 0）。
- 跳过免 seek 的那个游标（`if( iIdxCur+i==iIdxNoSeek ) continue;`）。
- key 由 `sqlite3GenerateIndexKey(pParse, pIdx, iDataCur, 0, 1, &iPartIdxLabel, pPrior, r1)` 生成，`prefixOnly` 参数传 1。
- 发 `OP_IdxDelete(iIdxCur+i, r1, p3)`，`P4` 设为索引对象（`P4_INDEX`），`P5` 设为 `pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn`。
- 部分索引（partial index）的跳过标签由 `sqlite3ResolvePartIdxLabel(pParse, iPartIdxLabel)` 收尾。

`sqlite3GenerateIndexKey` 里的列数取值是 `nCol = (prefixOnly && pIdx->uniqNotNull) ? pIdx->nKeyCol : pIdx->nColumn`——即「要求唯一前缀且索引键列非空」时只取键列，否则取全部列。它还带一个跨索引的寄存器复用缓存：`pPrior` 与当前索引共享同一列时跳过重复装载。

### 第十步：自增序列与结果返回

- `if( pParse->nested==0 && pParse->pTriggerTab==0 ){ sqlite3AutoincrementEnd(pParse); }` —— 更新 `sqlite_sequence` 表。
- `if( memCnt ){ sqlite3CodeChangeCount(v, memCnt, "rows deleted"); }` —— 生成结果行。
- 清理出口 `delete_from_cleanup:` 依次做 `sqlite3AuthContextPop(&sContext)`、`sqlite3SrcListDelete(db, pTabList)`、`sqlite3ExprDelete(db, pWhere)`、`sqlite3ExprListDelete(db, pOrderBy)`、`sqlite3ExprDelete(db, pLimit)`、`sqlite3DbNNFreeNN(db, aToOpen)`。

### 错误分支一览

| 分支 | 触发条件 | 表现 |
|---|---|---|
| 前置错误 | `pParse->nErr` 非 0 | 直接跳清理出口，不生成任何代码 |
| 表找不到 | `sqlite3SrcListLookup` 返回 0 | 跳清理出口 |
| 视图列名取不到 | `sqlite3ViewGetColumnNames` 返回非 0 | 跳清理出口 |
| 只读表 | `sqlite3IsReadOnly` 返回非 0 | 报 "table %s may not be modified" 后退出 |
| 授权拒绝 | `rcauth==SQLITE_DENY` | 跳清理出口 |
| WHERE 名字解析失败 | `sqlite3ResolveExprNames` 返回非 0 | 跳清理出口 |
| WHERE 规划失败 | `sqlite3WhereBegin` 返回 0 | 跳清理出口 |
| 内存分配失败 | `sqlite3DbMallocRawNN` 返回 0 | `sqlite3WhereEnd(pWInfo)` 后跳清理出口 |
| 虚拟表不安全 | `eVtabRisk` 高于 `SQLITE_TrustedSchema` 开关 | 报 "unsafe use of virtual table \"%s\"" |
| ORDER BY 无 LIMIT | `sqlite3LimitWhere` 中 `pOrderBy && pLimit==0` | 报 "ORDER BY without LIMIT on DELETE" |

## 关键取舍

- **源码事实：**清表优化的门槛是「无 WHERE、无触发器、无外键引用、非虚表、授权回调返回 `SQLITE_OK`」，并且（若定义了 `SQLITE_ENABLE_PREUPDATE_HOOK`）要求 `db->xPreUpdateCallback==0`。**架构推论：**这五个条件各自对应一条「批量清空会丢失的语义」——WHERE 限定了删除范围；触发器与外键动作需要逐行上下文（`OLD.*` 伪表、逐行触发器程序、逐行级联）；虚表的 `xUpdate` 接口本身没有批量语义；`SQLITE_IGNORE` 与 pre-update hook 都要求逐行回调可见每一次删除。

- **源码事实：**清表路径下只有主表（rowid 表）或 WITHOUT ROWID 表的主键索引的 `OP_Clear` 带 `P3=memCnt`，其他索引的 `OP_Clear` 不带。**架构推论：**`OP_Clear` 的执行语义里，`p->nChange` 累加与寄存器累加都由 `pOp->p3` 触发，若每个索引都带上计数寄存器，一次清表会被重复计成多倍行数。

- **源码事实：**两趟方式下，rowid 表用 RowSet（`OP_RowSetAdd`/`OP_RowSetRead`），WITHOUT ROWID 表用临时表（`OP_MakeRecord`+`OP_IdxInsert`/`OP_Rewind`+`OP_RowData`）。**架构推论：**RowSet 是按整数排序的集合，`OP_RowSetRead` 取的是集合里的最小值——rowid 表的 key 是单个整数，天然适配；而 WITHOUT ROWID 表的 key 是复合主键记录，需要临时表来保存并按 `OP_Rewind` 的键序回放。

- **源码事实：**`sNC.ncFlags & NC_Subquery` 会把 `bComplex` 置 1，从而去掉 `WHERE_ONEPASS_MULTIROW`。**架构推论：**ONEPASS 多行模式要求「扫描到的行就是要删的行」，而 WHERE 子句里的子查询可能引入对同表的二次访问；关掉多行 ONEPASS 让代码退到两趟方式，用 RowSet/临时表把待删 key 与扫描过程解耦。

- **源码事实：**BEFORE 触发器一旦生成代码，就会重发一次 `OP_NotExists`/`OP_NotFound`，并把 `iIdxNoSeek` 置为 `-1`。**架构推论：**`iIdxNoSeek` 这个优化依赖「索引游标仍停在待删的那条索引项上」这一前提；BEFORE 触发器里的用户代码可以移动游标甚至删掉该行，所以必须重新定位并放弃免 seek 优化。

- **源码事实：**`sqlite3GenerateRowDelete` 的第一步定位指令跳向的 `iLabel`，在函数末尾被 `sqlite3VdbeResolveLabel(v, iLabel)` 收束；同时这个标签也传给 `sqlite3CodeRowTrigger` 作为 `RAISE(IGNORE)` 的跳转目标。**架构推论：**「行已被别的程序删掉」与「触发器抛出 IGNORE」这两种情况在语义上等价——都不该继续删除、不该再触发 DELETE 触发器——所以复用同一个跳转标签。

- **源码事实：**变更计数寄存器 `memCnt` 只在 `db->flags & SQLITE_CountRows` 且非嵌套、非触发器体内、非返回型触发器时才分配；`OP_AddImm(memCnt, 1)` 发在 WHERE 循环体内。**架构推论：**计数放在扫描侧而不是删除侧，意味着计的是「被扫描判定为待删的行」；而 `sqlite3GenerateRowDelete` 里的 `OP_Delete` 又带 `OPFLAG_NCHANGE`（`count=(pParse->nested==0)`），两处计数的口径分工由 `sqlite3_code_changes()` 一类的 API 汇总。

- **源码事实：**`sqlite3LimitWhere` 在有 ORDER BY 而无 LIMIT 时直接报错。**架构推论：**SQLite 不实现「按 ORDER BY 顺序逐行删除」，因为删除会改变表的存储顺序，顺序保证在删除过程中没有稳定含义；限制成「必须带 LIMIT」把语义收敛为「先确定要删哪些行，再删」。

- **源码事实：**虚表的删除路径完全绕开 `sqlite3GenerateRowDelete`，直接发 `OP_VUpdate`，且断言虚表不能是 `ONEPASS_MULTI`。**架构推论：**虚表的行不存放在主 B-tree 里，主键与「行」的对应关系由虚表模块自己定义，无法用统一的 `OP_NotExists`/`OP_NotFound` 定位机制处理；`OP_VUpdate` 把定位与删除都交给模块的 `xUpdate` 回调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/delete.c` | `sqlite3DeleteFrom` | DELETE 语句编译的主入口：定位表、检查只读与授权、决定清表路径还是逐行路径、规划 WHERE、生成删除循环与结果行 |
| `src/delete.c` | `sqlite3GenerateRowDelete` | 生成单行删除动作：定位待删行、准备 `OLD.*` 伪表、触发 BEFORE/AFTER 触发器、删索引条目与表条目、发起外键动作 |
| `src/delete.c` | `sqlite3CodeChangeCount` | 生成变更计数结果行：`OP_FkCheck` + `OP_ResultRow(regCounter, 1)` + `SetNumCols(1)` + `SetColName` |
| `src/delete.c` | `sqlite3SrcListLookup` | 在 `SrcList` 中查找目标表，`sqlite3DeleteFrom` 的第一个真实调用方 |
| `src/delete.c` | `sqlite3IsReadOnly` | 判断目标表能否修改：虚表缺 `xUpdate`、系统表 `sqlite_sequence` 类的只读标志、影子表只读开关、视图本身 |
| `src/delete.c` | `sqlite3MaterializeView` | 把视图物化成临时表，作为「对视图执行 DELETE」的落地手段 |
| `src/delete.c` | `sqlite3LimitWhere` | 把 `WHERE/ORDER BY/LIMIT` 改写成 `rowid IN (SELECT ...)`，受 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT` 编译开关控制 |
| `src/delete.c` | `sqlite3GenerateRowIndexDelete` | 遍历表的所有索引逐个发 `OP_IdxDelete`，处理主键索引跳过、部分索引跳过与免 seek 跳过 |
| `src/delete.c` | `sqlite3GenerateIndexKey` | 为单个索引生成 key 寄存器，带跨索引的列值复用缓存；`prefixOnly` 时按 `uniqNotNull` 在 `nKeyCol` 与 `nColumn` 之间取舍 |
| `src/delete.c` | `sqlite3ResolvePartIdxLabel` | 收束部分索引（partial index）的跳过标签 |
| `src/sqliteInt.h` | `ONEPASS_OFF` / `ONEPASS_SINGLE` / `ONEPASS_MULTI` | 三种删除策略的常量，取值分别为 `0` / `1` / `2`，决定待删 key 是留在寄存器还是收进 RowSet/临时表 |
| `src/sqliteInt.h` | `WHERE_ONEPASS_DESIRED` / `WHERE_ONEPASS_MULTIROW` | WHERE 规划的控制标志，取值分别为 `0x0004` / `0x0008`；后者仅在 `bComplex` 为假时加入 |
| `src/vdbe.c` | `OP_Clear` | 清空表或索引；`P3` 非零时把清掉的行数同时累加进 `p->nChange` 与寄存器 |
| `src/vdbe.c` | `OP_RowSetAdd` / `OP_RowSetRead` | RowSet 的写入与读取；`OP_RowSetRead` 取出集合中的最小值，集合为空时跳到 `P2` |
| `src/vdbe.c` | `OP_VUpdate` | 虚表行更新/删除入口，参数含虚表模块名与 `P4_VTAB` 标记 |

## 与主核 / AI 的关系

本页对应的是 SQLite 把「DELETE 编译」拆成「收集待删 key」与「执行单行删除」两段的做法：前半段由 WHERE 规划层决定用哪一种 ONEPASS 策略，后半段由 `sqlite3GenerateRowDelete` 按固定顺序发出指令。主核（`engine/db/` 自研多模态数据库内核）的删除路径在形态上对应：先由扫描算子产出待删行的标识，再由删除算子对每行发出写入请求。两边的差别在于 SQLite 把「待删 key 的存放形态」与「表的存储形态」绑定得很死——rowid 表用整数 RowSet、WITHOUT ROWID 表用主键记录临时表，而主核的多模态行标识不止一种形态。

AI 侧没有直接对应实现。本页可作为 AI 理解 SQLite「批量清表 vs 逐行删除」这套取舍的材料：清表路径要求五个前提同时成立，任何一个语义要求（触发器、外键、逐行回调）都会把它打回逐行路径；逐行路径又按表的存储形态分成 RowSet 与临时表两种 key 容器。

## 参考

- [SQLite 官方文档：DELETE 语句](https://sqlite.org/lang_delete.html) —— DELETE 的标准语义，以及 `sqlite3_changes()` 与 `sqlite3_total_changes()` 对变更计数的定义
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明代码生成层与 VDBE、B-tree 的位置关系
- [SQLite 官方文档：WITHOUT ROWID 表](https://sqlite.org/withoutrowid.html) —— 主键索引作为表规范存储的表形态，解释删除时 key 形态的差异
- [SQLite 官方文档：变更计数器](https://sqlite.org/c3ref/changes.html) —— `sqlite3_changes()` 的语义，以及 `sqlite3_total_changes()` 与 `sqlite3_preupdate_hook` 的关系
- [SQLite 源码：src/delete.c](https://github.com/sqlite/sqlite/blob/master/src/delete.c) —— 本页全部 SQLite 侧符号的实现文件
- [SQLite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h) —— `ONEPASS_*` 与 `WHERE_ONEPASS_*` 常量定义所在处
- [SQLite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— `OP_Clear`、`OP_RowSetAdd`、`OP_RowSetRead`、`OP_VUpdate` 的执行语义
