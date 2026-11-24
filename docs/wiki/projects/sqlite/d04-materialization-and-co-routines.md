---
project: sqlite
feature: materialization-and-co-routines
page_type: feature
schema_version: 1
refs:
  - path: src/delete.c
    symbols: [sqlite3MaterializeView]
  - path: src/select.c
    symbols: [sqlite3SelectDestInit]
  - path: src/delete.c
    symbols: [sqlite3MaterializeView]
---

# 物化与协程式子查询

## 一句话职责

物化与协程式子查询负责给 SQLite 的 SELECT 结果定「去处」：同一段 `sqlite3Select()` 代码既可以把行写进一张临时表（ephemeral table，即物化），也可以把行放进一组寄存器、由一条独立的协程逐行吐出（co-routine）；判据是 `SelectDest.eDest` 这个取自 `SRT_*` 常量表的枚举值。

## 机制是什么

边界（依据条目 `boundary`）：负责子查询结果的物化成临时表，以及用协程把子查询展开成逐行推进的执行单元；不负责临时表的存储实现（F 组）、不负责聚合函数的实现（L 组）。

### 一条 SELECT 的结果有多种去处

`src/select.c` 里的 `sqlite3Select()` 只有一个出口参数 `SelectDest *pDest`，`pDest->eDest` 决定结果往哪走。`src/sqliteInt.h` 定义了 13 个 `SRT_*` 常量：

| 常量 | 值 | 结果去处 |
|---|---|---|
| `SRT_Exists` | `1` | 往 `iSDParm` 寄存器写 1 表示结果非空 |
| `SRT_Discard` | `2` | 丢弃结果 |
| `SRT_DistFifo` | `3` | 存进临时表，只保留互不相同的行 |
| `SRT_DistQueue` | `4` | 存进优先级队列，只保留互不相同的行 |
| `SRT_Queue` | `5` | 存进优先级队列 |
| `SRT_Fifo` | `6` | 存进临时表，自动分配 rowid |
| `SRT_Output` | `7` | 生成 `OP_ResultRow`，交回调用方 |
| `SRT_Mem` | `8` | 存进一个内存单元（单列结果） |
| `SRT_Set` | `9` | 存进索引表的键 |
| `SRT_EphemTab` | `10` | 建一张临时表并存进去 |
| `SRT_Coroutine` | `11` | 生成一条协程，每次调用产出一行 |
| `SRT_Table` | `12` | 存进临时表，自动分配 rowid |
| `SRT_Upfrom` | `13` | 存进已打开的临时表，按指定键插入 |

`SelectDest` 结构体本身只有 8 个字段（`src/sqliteInt.h`）：`eDest`（去处）、`iSDParm` / `iSDParm2`（两个去处相关参数）、`iSdst`（结果寄存器基址）、`nSdst`（寄存器个数）、`zAffSdst`（`SRT_Set` 用的亲和性串）、`pOrderBy`（`SRT_Queue` / `SRT_DistQueue` 用的键列）。

`src/sqliteInt.h` 里的注释直接写明了 `SRT_Coroutine` 与 `SRT_EphemTab` 的区别：前者「Generate a co-routine that returns a new row of result on each invocation」，结果寄存器起点是 `pDest->iSDParm`；后者「Create an temporary table pDest->iSDParm and store the results into it」，临时表游标号也是 `pDest->iSDParm`。

### 两种「多行结果」执行形态

同一个子查询，在 SQLite 里有两条根本不同的落地路径：

- **物化（materialization）**：先把子查询的全部结果算完、写进一张临时表（ephemeral table），外层再扫这张临时表。子查询只执行一遍，但结果集必须完整落盘（内存或临时文件）。
- **协程（co-routine）**：把子查询编译成一段独立的 VDBE 代码块，用 `OP_InitCoroutine` / `OP_Yield` / `OP_EndCoroutine` 三条操作码与调用方来回传控。外层每需要一行就 `OP_Yield` 一次，子查询从上次挂起处继续。子查询不落盘，但每行都要在寄存器组与调用方之间搬一次。

`src/sqliteInt.h` 里有两条标志位把这两条路径固化在 `SrcItem` 上：`isMaterialized:1`（注释 "This is a materialized view"）与 `viaCoroutine:1`（注释 "Implemented as a co-routine"）。

### 物化的两个具体触发点

**触发点一：视图被当成写入目标。** `sqlite3MaterializeView()`（`src/delete.c`）把一个视图的查询结果灌进临时表，让 `DELETE` / `UPDATE` 可以对着临时表操作。

**触发点二：FROM 子句里的项既不是协程也不是 CTE 复用。** `src/select.c` 的 `sqlite3Select()` 在遍历 FROM 子句时，若该项走不到协程路径、也不是已经物化好的 CTE、也不是自连接视图复用，就落到 `else` 分支做物化。

## 怎么实现的

### 第一步：sqlite3SelectDestInit 初始化目的地

`sqlite3SelectDestInit(SelectDest *pDest, int eDest, int iParm)` 定义在 `src/select.c`，声明在 `src/sqliteInt.h`。函数体只有 6 条赋值：

```c
pDest->eDest = (u8)eDest;
pDest->iSDParm = iParm;
pDest->iSDParm2 = 0;
pDest->zAffSdst = 0;
pDest->iSdst = 0;
pDest->nSdst = 0;
```

注意 `eDest` 被截断成 `u8` 存储——`SelectDest.eDest` 的声明类型就是 `u8`。`iSdst` 与 `nSdst` 初始为 0，表示「结果寄存器还没分配」；`sqlite3Select()` 内部发现 `iSdst==0` 时才去分配。

有一处调用方故意不通过 `sqlite3SelectDestInit` 传 `eDest`：`src/expr.c` 的 `sqlite3CodeSubselect()` 先调 `sqlite3SelectDestInit(&dest, 0, pParse->nMem+1)`（此时 `eDest` 被置为 0），随后用 `dest.eDest = SRT_Mem` 或 `dest.eDest = SRT_Exists` 直接覆盖。

### 第二步：sqlite3MaterializeView 把视图灌进临时表

`sqlite3MaterializeView(Parse *pParse, Table *pView, Expr *pWhere, ExprList *pOrderBy, Expr *pLimit, int iCur)` 定义在 `src/delete.c`，其定义整体被 `#if !defined(SQLITE_OMIT_VIEW) && !defined(SQLITE_OMIT_TRIGGER)` 包住。函数体按顺序做了 7 件事：

1. `sqlite3SchemaToIndex(db, pView->pSchema)` 拿到视图所在数据库的编号 `iDb`。
2. `pWhere = sqlite3ExprDup(db, pWhere, 0)` 复制 WHERE 表达式，避免改到调用方持有的树。
3. `pFrom = sqlite3SrcListAppend(pParse, 0, 0, 0)` 新建一个只有一项的 FROM 列表；该表的库名取自 `db->aDb[iDb].zDbSname`（源码里是 `zDbSName`），表名取 `pView->zName`。
4. `pSel = sqlite3SelectNew(pParse, 0, pFrom, pWhere, 0, 0, pOrderBy, SF_IncludeHidden, pLimit)` 造出 SELECT 树。`selFlags` 传 `SF_IncludeHidden`（`src/sqliteInt.h` 中值为 `0x0020000`，注释 "Include hidden columns in output"）——视图定义里的隐藏列必须一并取出，否则 `UPDATE` 视图时找不到目标列。
5. `sqlite3SelectDestInit(&dest, SRT_EphemTab, iCur)` 指定去处是临时表，游标号就是传进来的 `iCur`。
6. `sqlite3Select(pParse, pSel, &dest)` 真正编译这段 SELECT。
7. `sqlite3SelectDelete(db, pSel)` 释放刚造出来的 SELECT 树。

函数本身不处理错误——它不检查 `sqlite3Select()` 的返回值，错误由 `pParse->nErr` 承载。

**调用方一：`sqlite3DeleteFrom()`（`src/delete.c`）。** 在视图分支里：

```c
if( isView ){
  sqlite3MaterializeView(pParse, pTab, pWhere, pOrderBy, pLimit, iTabCur);
  iDataCur = iIdxCur = iTabCur;
  pOrderBy = 0;
  pLimit = 0;
}
```

物化完成后，数据游标与索引游标都指向临时表 `iTabCur`，并且把 `pOrderBy` / `pLimit` 清零——排序与限量已经在物化时由视图自己的 SELECT 处理掉了，外层不再需要。

**调用方二：`sqlite3Update()`（`src/update.c`）。** 条件是 `nChangeFrom==0 && isView`，即不是 `UPDATE ... FROM` 形式：

```c
if( nChangeFrom==0 && isView ){
  sqlite3MaterializeView(pParse, pTab, pWhere, pOrderBy, pLimit, iDataCur);
  pOrderBy = 0;
  pLimit = 0;
}
```

区别在于传给 `iCur` 的是 `iDataCur` 而非 `iTabCur`——`UPDATE` 走的是数据游标。

### 第三步：sqlite3Select 打开临时表并写入

物化的落地发生在 `src/select.c` 的 `sqlite3Select()` 里。临时表的创建由目标类型触发：

```c
if( pDest->eDest==SRT_EphemTab ){
  sqlite3VdbeAddOp2(v, OP_OpenEphemeral, pDest->iSDParm, pEList->nExpr);
  ...
}
```

`OP_OpenEphemeral` 的第二个参数是列数，取自 `pEList->nExpr`（结果列表达式个数）。

写入行则由内层循环的 `switch( eDest )` 分派。`SRT_Fifo` / `SRT_DistFifo` / `SRT_Table` / `SRT_EphemTab` 四个分支共用一段代码：先 `sqlite3VdbeAddOp3(v, OP_MakeRecord, regResult, nResultCol, r1+nPrefixReg)` 把结果列打成一条记录；无排序时再 `OP_NewRowid` 生成 rowid、`OP_Insert` 插入，并调 `sqlite3VdbeChangeP5(v, OPFLAG_APPEND)`。

带 `ORDER BY` 时走 `pushOntoSorter()`，即结果先进排序器再落表——这也是 `src/select.c` 里 `SORTFLAG_UseSorter`（`0x01`，注释 "Use SorterOpen instead of OpenEphemeral"）控制的分岔：当 `p->iLimit==0` 且存在排序索引时，`sqlite3Select()` 会把 `OP_OpenEphemeral` 改写成 `OP_SorterOpen` 并置上 `SORTFLAG_UseSorter`。

### 第四步：协程路径的判定

`fromClauseTermCanBeCoroutine(Parse *pParse, SrcList *pTabList, int i, int selFlags)` 是 `src/select.c` 的静态函数，函数头注释列出了 5 条允许条件。函数体逐条实现：

| 注释编号 | 实现 | 常量/取值 |
|---|---|---|
| (1) | 子查询要能待在 FROM 子句的最外层循环 | `i==0` 时：`pTabList->nSrc==1` 直接返回 1；`pTabList->a[1].fg.jointype & JT_CROSS` 也返回 1；否则在 `SF_UpdateFrom` 未置位时返回 1 |
| (2) | 不该被物化的 CTE | `pItem->fg.isCte` 时：`pCteUse->eM10d==M10d_Yes` 返回 0；`pCteUse->nUse>=2 && pCteUse->eM10d!=M10d_No` 返回 0 |
| (3) | 不是右外连接的左操作数 | `pTabList->a[0].fg.jointype & JT_LTORJ` 非零则返回 0 |
| (4) | 协程优化没被关掉 | `OptimizationDisabled(pParse->db, SQLITE_Coroutines)` 为真则返回 0 |
| (5) | 不是自连接视图 | `isSelfJoinView(pTabList, pItem, i+1, pTabList->nSrc)` 非零则返回 0 |

`M10d_Yes` / `M10d_Any` / `M10d_No` 在 `src/sqliteInt.h` 中的取值依次为 `0` / `1` / `2`，注释分别是 "AS MATERIALIZED"、"Not specified. Query planner's choice"、"AS NOT MATERIALIZED"。`SQLITE_Coroutines` 的值是 `26`，注释 "Co-routines for subqueries"。

非首项（`i!=0`）时，函数用一个 `while( 1 /*exit-by-break*/ )` 循环向左回溯：遇到带 `JT_OUTER` 或 `JT_CROSS` 的项返回 0；遇到 `pItem->fg.isSubquery` 的项返回 0（即左边还有别的子查询时不能当协程）。

### 第五步：协程的编译与挂载

`sqlite3Select()` 里协程分支的代码是：

```c
int addrTop = sqlite3VdbeCurrentAddr(v)+1;
pSubq->regReturn = ++pParse->nMem;
sqlite3VdbeAddOp3(v, OP_InitCoroutine, pSubq->regReturn, 0, addrTop);
pSubq->addrFillSub = addrTop;
sqlite3SelectDestInit(&dest, SRT_Coroutine, pSubq->regReturn);
sqlite3Select(pParse, pSub, &dest);
pItem->pSTab->nRowLogEst = pSub->nSelectRow;
pItem->fg.viaCoroutine = 1;
pSubq->regResult = dest.iSdst;
sqlite3VdbeEndCoroutine(v, pSubq->regReturn);
sqlite3VdbeJumpHere(v, addrTop-1);
```

要点：

- `pSubq->regReturn` 是一枚新分配的寄存器（`++pParse->nMem`），作协程的程序计数器。
- `OP_InitCoroutine` 的 P3 是协程入口地址 `addrTop`，P2 传 0。按 `src/vdbe.c` 里 `OP_InitCoroutine` 的实现，寄存器 `P1` 被置为 `pOp->p3 - 1`，`flags` 置 `MEM_Int`；`P2==0` 时不跳转，协程体紧接着 `OP_InitCoroutine` 之后。
- `sqlite3SelectDestInit(&dest, SRT_Coroutine, pSubq->regReturn)`：去处是协程，参数就是那枚返回地址寄存器。
- `pSubq->regResult = dest.iSdst` 记录结果寄存器的基址。
- `sqlite3VdbeEndCoroutine(v, pSubq->regReturn)` 在 `src/vdbeaux.c`，它发出 `OP_EndCoroutine`，然后把 `v->pParse->nTempReg` 与 `v->pParse->nRangeReg` 归零。函数注释写明原因：协程之间可能共用临时寄存器，而寄存器需要在 `OP_Yield` 前后保持不变，清缓存保证每条协程有自己独立的一套。

`src/select.c` 里 `SRT_Coroutine` 的产出点在 `switch( eDest )` 中与 `SRT_Output` 共用一个分支：

```c
case SRT_Coroutine:       /* Send data to a co-routine */
case SRT_Output: {        /* Return the results */
  if( pSort ){
    pushOntoSorter(pParse, pSort, p, regResult, regOrig, nResultCol, nPrefixReg);
  }else if( eDest==SRT_Coroutine ){
    sqlite3VdbeAddOp1(v, OP_Yield, pDest->iSDParm);
  }else{
    sqlite3VdbeAddOp2(v, OP_ResultRow, regResult, nResultCol);
  }
  break;
}
```

即协程每产出一行就发一条 `OP_Yield`；有 `ORDER BY` 时则先 `pushOntoSorter()`，由排序器统一吐行。

### 第六步：协程的三条操作码语义

三条操作码的实现在 `src/vdbe.c`：

- **`OP_InitCoroutine P1 P2 P3`**：把寄存器 `P1` 置为 `P3 - 1`，`flags` 置 `MEM_Int`。若 `P2!=0`，则跳到地址 `P2`（跳过的正是协程体）。
- **`OP_Yield P1 P2`**：把 `P1` 里存的地址与当前程序计数器对调——`pIn1->u.i = (int)(pOp - aOp)` 存回原处，`pOp = &aOp[pcDest]` 跳进协程。
- **`OP_EndCoroutine P1`**：取 `P1` 里存的地址（注释断言那里必须是一条 `OP_Yield`），跳到那条 `OP_Yield` 的 `P2` 参数处；同时把 `P1` 的值改写为当前 `OP_EndCoroutine` 指令的地址，使后续的 `OP_Yield` 回到同一条 `OP_EndCoroutine`。

三者的配合使得协程体可以停在 `OP_Yield` 上被反复唤醒。

### 第七步：复合 SELECT 的双协程归并

`src/select.c` 里复合 SELECT（`UNION` / `UNION ALL` / `EXCEPT` / `INTERSECT`）的归并算法用两条并行协程。代码先分配 4 枚寄存器：`regAddrA`、`regAddrB`（协程返回地址）与 `regOutA`、`regOutB`（输出子程序），然后：

```c
sqlite3SelectDestInit(&destA, SRT_Coroutine, regAddrA);
sqlite3SelectDestInit(&destB, SRT_Coroutine, regAddrB);
...
addrSelectA = sqlite3VdbeCurrentAddr(v) + 1;
addr1 = sqlite3VdbeAddOp3(v, OP_InitCoroutine, regAddrA, 0, addrSelectA);
sqlite3Select(pParse, pPrior, &destA);
sqlite3VdbeEndCoroutine(v, regAddrA);
```

B 侧同理，但 `pPrior->iLimit = regLimitA`、`p->iLimit = regLimitB`、`p->iOffset = 0`，把 LIMIT/OFFSET 拆给两侧单独计数。

输出子程序由静态函数 `generateOutputSubroutine(Parse *pParse, Select *p, SelectDest *pIn, SelectDest *pDest, int regReturn, int regPrev, KeyInfo *pKeyInfo, int iBreak)` 生成。它的开头就断言 `pIn->eDest==SRT_Coroutine`，即它只接受协程作为输入。

函数头注释描述了 UNION / EXCEPT / INTERSECT 三类运算各自需要哪些子程序：A 侧的 `outA` / `outB` 负责输出，`AltB` / `AeqB` / `AgtB` 负责比较分支，`EofA` / `EofB` 负责一侧耗尽的处理。`UNION ALL` 与 `UNION` 需要 `outA` 和 `outB` 两个输出子程序；`EXCEPT` 与 `INTERSECT` 只需 `outA`（源码注释写明 "EXCEPT and INTERSECT never output a row that appears only in B"）。

去重逻辑：若 `regPrev>0`，则先 `OP_IfNot` 检查「是否有前一次输出」的标志位，再用 `OP_Compare` 把当前寄存器组 `pIn->iSdst` 起的 `pIn->nSdst` 个寄存器与 `regPrev+1` 起的同长寄存器比较，相等则跳到 `iContinue` 跳过本次输出；不等则 `OP_Copy` 保存当前值并 `OP_Integer 1` 置标志位。

OFFSET 处理由 `codeOffset(v, p->iOffset, iContinue)` 完成；LIMIT 处理在函数末尾：`if( p->iLimit ) sqlite3VdbeAddOp2(v, OP_DecrJumpZero, p->iLimit, iBreak);`。

### 第八步：协程的消费者

协程产出的行由调用方在 `generateOutputSubroutine()` 里按 `pDest->eDest` 分派。其中 `SRT_Coroutine` 分支（协程喂协程）：

```c
case SRT_Coroutine: {
  if( pDest->iSdst==0 ){
    pDest->iSdst = sqlite3GetTempRange(pParse, pIn->nSdst);
    pDest->nSdst = pIn->nSdst;
  }
  sqlite3ExprCodeMove(pParse, pIn->iSdst, pDest->iSdst, pIn->nSdst);
  sqlite3VdbeAddOp1(v, OP_Yield, pDest->iSDParm);
  break;
}
```

注意 `iSdst==0` 时才分配寄存器——这正是 `sqlite3SelectDestInit()` 把 `iSdst` 与 `nSdst` 初始化为 0 的用意。

`SRT_EphemTab` 分支把协程的行写进临时表：`OP_MakeRecord` → `OP_NewRowid` → `OP_Insert`，并 `sqlite3VdbeChangeP5(v, OPFLAG_APPEND)`。

`SRT_Mem` 分支用 `sqlite3ExprCodeMove(pParse, pIn->iSdst, pDest->iSDParm, pIn->nSdst)` 把结果搬进目标内存单元。

### 第九步：IN 运算符的物化路径

`src/expr.c` 的 `sqlite3CodeRhsOfIN()` 是 IN 运算右值的物化入口，与协程是两条不同的路。

它先用 `sqlite3VdbeAddOp2(v, OP_OpenEphemeral, pExpr->iTable, nVal)` 打开临时表，`nVal` 是左值向量的大小 `sqlite3ExprVectorSize(pExpr->pLeft)`；再用 `sqlite3KeyInfoAlloc(pParse->db, nVal, 1)` 造键信息。

然后 `sqlite3SelectDestInit(&dest, SRT_Set, iTab)`——去处是 `SRT_Set`（存进索引表的键），并设 `dest.zAffSdst = exprINAffinity(pParse, pExpr)` 给出亲和性串。

若 `addrOnce` 非 0、`allowBloom` 为真且 `OptimizationEnabled(pParse->db, SQLITE_BloomFilter)` 成立，还额外发一条 `sqlite3VdbeAddOp2(v, OP_Blob, 10000, regBloom)` 造出一块 10000 字节的 Bloom filter 缓冲区，`dest.iSDParm2 = regBloom` 后在 `OP_IdxInsert` 之后追加 `OP_FilterAdd`。

**复用路径：** 函数开头有一整段「已经物化过就别再算一遍」的逻辑。当 `!ExprHasProperty(pExpr, EP_VarSelect) && pParse->iSelfTab==0` 时可以复用：先检查 `ExprHasProperty(pExpr, EP_Subrtn)` 或 `findCompatibleInRhsSubrtn(pParse, pExpr, pSig)`，命中则发一条 `OP_Gosub` 跳到既有子程序，再用 `OP_OpenDup` 把既有游标复制一份。

### 第十步：标量子查询与 EXISTS 的落点

`src/expr.c` 的 `sqlite3CodeSubselect()` 处理 `EXISTS` 与标量子查询，用的都不是协程：

- 标量 SELECT：`dest.eDest = SRT_Mem`，寄存器数 `nReg = pSel->pEList->nExpr`。
- EXISTS：`dest.eDest = SRT_Exists`，`nReg = 1`。

两者都会给子查询加 `LIMIT 1`——没有 LIMIT 时造 `sqlite3PExpr(pParse, TK_LIMIT, sqlite3ExprInt32(pParse->db, 1), 0)`；已有 LIMIT 且其左值不是整数 `1` 或 `0` 时，改写成 `X<>0`，即新 LIMIT 要么是 1 要么是 0。

整段子查询代码被包在 `OP_BeginSubrtn` / `OP_Return` 子程序里（`pExpr->y.sub.regReturn` 与 `pExpr->y.sub.iAddr`），因此 `EP_Subrtn` 标记的表达式第二次出现时可以直接 `OP_Gosub` 复用，源码里对应 `EXPLAIN` 输出的 "REUSE SUBQUERY"。

## 关键取舍

**取舍一：结果去处用一个 `u8` 枚举表示，而不是一个函数指针或一个虚表。** `SelectDest.eDest` 是 `u8`，取值范围是 `SRT_Exists`(1) 到 `SRT_Upfrom`(13)。`src/select.c` 里 `sqlite3Select()` 与 `generateOutputSubroutine()` 各有一份 `switch( eDest )` 分派表。**架构推论：** 好处是所有去处共用同一套编译流程（同一个 `sqlite3Select()`），改动一处枚举就要在两处 switch 里同步；代价是新增一种去处必须同时改 `sqlite3SelectDestInit()`、`sqlite3Select()` 的 switch、`generateOutputSubroutine()` 的 switch 三处，且 `u8` 的取值空间只有 256 个。

**取舍二：物化与协程的判定依据是「结果要不要被反复扫描」，而不是「结果集有多大」。** `fromClauseTermCanBeCoroutine()` 的 5 条判据全部是结构性的：是否在最外层循环、CTE 是否被强制物化、是否右外连接左操作数、优化开关、是否自连接。**架构推论：** 这意味着一个哪怕只返回一行的子查询，只要在结构上允许，也可能走协程路径；反过来一个返回千万行的子查询只要结构允许也走协程——协程路径不按行数做判断，因此不存在「小结果集自动物化」这类基于代价的切换。

**取舍三：协程用寄存器交换程序计数器实现，而不是用函数调用栈。** `OP_Yield` 的实现是把 `pIn1->u.i` 与当前 `pOp` 对调，`OP_InitCoroutine` 把寄存器初始化成 `p3 - 1`，`OP_EndCoroutine` 把寄存器改写成自己的地址。**架构推论：** 好处是不需要额外的栈结构，一条 `OP_Yield` 就是一次完整的上下文切换；代价是协程的挂起状态完全由那枚寄存器里的整数表达，无法挂起超过一个寄存器宽度的状态。

**取舍四：每条协程有独立的临时寄存器集，靠清 `nTempReg` / `nRangeReg` 实现。** `sqlite3VdbeEndCoroutine()` 在发出 `OP_EndCoroutine` 后把 `v->pParse->nTempReg` 与 `v->pParse->nRangeReg` 归零。函数注释写明原因：协程可能在 `OP_Yield` 前后保持寄存器不变，两条协程共用同一枚临时寄存器会互相破坏。**架构推论：** 这是在「临时寄存器可复用」与「协程状态可保持」之间的取舍——SQLite 选了后者，代价是同时并存的协程各自占用一份寄存器。

**取舍五：视图作为写入目标时走物化，且物化时必须带上隐藏列。** `sqlite3MaterializeView()` 传 `SF_IncludeHidden` 给 `sqlite3SelectNew()`，`src/sqliteInt.h` 里该标志的注释是 "Include hidden columns in output"，值为 `0x0020000`。**架构推论：** 视图的隐藏列（如 `rowid`）是 `UPDATE` 定位目标行的依据，少取一列就定位不到行；代价是物化出的临时表列数比用户看到的视图列数多。

**取舍六：`sqlite3MaterializeView()` 不检查 `sqlite3Select()` 的返回值，错误只走 `pParse->nErr`。** 函数体第 6 步直接调 `sqlite3Select(pParse, pSel, &dest)` 并丢弃返回值，第 7 步照常 `sqlite3SelectDelete(db, pSel)`。**架构推论：** 好处是错误处理路径只有一条；代价是函数返回后调用方必须自己检查 `pParse->nErr`——两个调用方 `sqlite3DeleteFrom()` 与 `sqlite3Update()` 都在后续流程里检查。

**取舍七：IN 的右值走 `SRT_Set` 物化，且右值集合的复用靠子程序签名匹配。** `sqlite3CodeRhsOfIN()` 造 `SubrtnSig`（含 `selId` 与亲和性串 `zAff`）来识别「同一个 IN 运算的重复出现」，命中时用 `OP_Gosub` + `OP_OpenDup` 复用。**架构推论：** 这比协程路径多了一套签名匹配机制，但换来了「右值只需算一次」的保证——IN 的语义本身就要求右值集合完整可见，协程的逐行吐出不满足这一点。

**取舍八：`SRT_Set` 与 `SRT_EphemTab` 的写入方式不同。** `SRT_Set` 用 `OP_IdxInsert`（写进索引表的键，键信息带亲和性 `pDest->zAffSdst`），`SRT_EphemTab` 用 `OP_NewRowid` + `OP_Insert`（写进带自动 rowid 的表）。前者不需要 rowid，后者需要。**架构推论：** 集合语义（IN / EXISTS）关心的是键的唯一性与比较规则，临时表语义关心的是行序与可重复扫描。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/delete.c | `sqlite3MaterializeView` | 把视图的查询结果灌进临时表：`sqlite3SelectNew` 带 `SF_IncludeHidden` 造 SELECT，`sqlite3SelectDestInit(&dest, SRT_EphemTab, iCur)` 指定去处，最后 `sqlite3SelectDelete` 释放 SELECT 树 |
| src/delete.c | `sqlite3DeleteFrom` | `sqlite3MaterializeView` 的调用方之一：视图分支里传 `iTabCur` 作临时表游标，物化后把 `iDataCur` 与 `iIdxCur` 都设为 `iTabCur`，并清零 `pOrderBy` / `pLimit` |
| src/update.c | `sqlite3Update` | `sqlite3MaterializeView` 的另一个调用方：条件为 `nChangeFrom==0 && isView`，传的是 `iDataCur` |
| src/select.c | `sqlite3SelectDestInit` | 初始化 `SelectDest`：`eDest` 截断为 `u8` 存入，`iSDParm` 存参数，`iSDParm2`/`zAffSdst`/`iSdst`/`nSdst` 清零 |
| src/select.c | `sqlite3Select` | 主编译流程：按 `pDest->eDest` 打开临时表（`OP_OpenEphemeral`）、发 `OP_Yield`、发 `OP_ResultRow`；`SRT_EphemTab` 分支把 `OP_OpenEphemeral` 改成 `OP_SorterOpen` 并置 `SORTFLAG_UseSorter` |
| src/select.c | `fromClauseTermCanBeCoroutine` | 判定 FROM 子句里的一项能否做成协程：5 条判据（最外层循环、CTE 物化标志、右外连接左操作数、`SQLITE_Coroutines` 开关、自连接视图） |
| src/select.c | `generateOutputSubroutine` | 协程输出的消费者：生成 out-A / out-B / eof-A / eof-B 等子程序，做去重（`OP_Compare` + `OP_Copy`）、OFFSET（`codeOffset`）、LIMIT（`OP_DecrJumpZero`） |
| src/select.c | `selectInnerLoop` | 内层循环，按 `switch( eDest )` 分派结果去处；`SRT_Coroutine` 发 `OP_Yield`，`SRT_EphemTab` 等走 `OP_MakeRecord` + `OP_NewRowid` + `OP_Insert` |
| src/expr.c | `sqlite3CodeRhsOfIN` | IN 右值的物化：`OP_OpenEphemeral` 开表、`sqlite3SelectDestInit(&dest, SRT_Set, iTab)`、可选用 `OP_Blob`（10000 字节）造 Bloom filter；`EP_Subrtn` 或 `findCompatibleInRhsSubrtn` 命中时用 `OP_Gosub` + `OP_OpenDup` 复用 |
| src/expr.c | `sqlite3CodeSubselect` | 标量子查询与 EXISTS 的落点：`SRT_Mem` / `SRT_Exists`，子查询被加上 `LIMIT 1`；用 `OP_BeginSubrtn` / `OP_Return` 包成子程序以便 `EP_Subrtn` 复用 |
| src/vdbe.c | `OP_InitCoroutine` | 把寄存器 `P1` 置为 `P3 - 1` 并打上 `MEM_Int`；`P2!=0` 时跳到 `P2` 越过协程体 |
| src/vdbe.c | `OP_Yield` | 与调用方交换程序计数器：`P1` 存回原指令地址，`pOp` 跳到 `P1` 原先存的地址 |
| src/vdbe.c | `OP_EndCoroutine` | 跳到 `P1` 存的 `OP_Yield` 的 `P2` 处，并把 `P1` 改写成自己的指令地址 |
| src/vdbeaux.c | `sqlite3VdbeEndCoroutine` | 发出 `OP_EndCoroutine`，并把 `pParse->nTempReg` 与 `pParse->nRangeReg` 归零，保证每条协程有独立寄存器集 |
| src/sqliteInt.h | `SRT_Exists` / `SRT_Discard` / `SRT_DistFifo` / `SRT_DistQueue` / `SRT_Queue` / `SRT_Fifo` / `SRT_Output` / `SRT_Mem` / `SRT_Set` / `SRT_EphemTab` / `SRT_Coroutine` / `SRT_Table` / `SRT_Upfrom` | 结果去处枚举，取值依次为 `1` / `2` / `3` / `4` / `5` / `6` / `7` / `8` / `9` / `10` / `11` / `12` / `13` |
| src/sqliteInt.h | `struct SelectDest` | 结果去处的载体：`eDest`（`u8`）、`iSDParm` / `iSDParm2`、`iSdst` / `nSdst`、`zAffSdst`、`pOrderBy` |
| src/sqliteInt.h | `struct Subquery` | 子查询的实现记录：`addrFillSub`（初始化子程序地址）、`regReturn`（返回地址寄存器）、`regResult`（协程结果寄存器基址） |
| src/sqliteInt.h | `SrcItem.fg.isMaterialized` / `SrcItem.fg.viaCoroutine` | 两条互斥的落地形态标志：前者注释 "This is a materialized view"，后者注释 "Implemented as a co-routine" |
| src/sqliteInt.h | `SF_IncludeHidden` | `Select.selFlags` 的一个位，值 `0x0020000`，注释 "Include hidden columns in output"；`sqlite3MaterializeView` 用它保证视图的隐藏列也被取出 |
| src/sqliteInt.h | `SF_UpdateFrom` | `Select.selFlags` 的一个位，值 `0x10000000`，注释 "Query originates with UPDATE FROM"；`fromClauseTermCanBeCoroutine` 用它排除 `UPDATE ... FROM` |
| src/sqliteInt.h | `SQLITE_Coroutines` | 优化开关，值 `26`，注释 "Co-routines for subqueries"；关掉后所有子查询都退回物化 |
| src/sqliteInt.h | `M10d_Yes` / `M10d_Any` / `M10d_No` | CTE 的 MATERIALIZED 标志取值，依次 `0` / `1` / `2`，分别是 "AS MATERIALIZED"、"Not specified. Query planner's choice"、"AS NOT MATERIALIZED" |
| src/sqliteInt.h | `JT_LTORJ` / `JT_CROSS` / `JT_OUTER` | 连接类型位，用于 `fromClauseTermCanBeCoroutine` 排除右外连接左操作数与外连接/交叉连接项 |
| src/select.c | `SORTFLAG_UseSorter` | 值为 `0x01`，注释 "Use SorterOpen instead of OpenEphemeral"；置位后排序走 `OP_SorterOpen` + `OP_SorterSort` + `OP_SorterData` 而非 `OP_Sort` |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里没有与 SQLite 的 `SRT_*` 结果去处枚举对位的结构。SQLite 把「结果往哪去」压缩成一个 `u8` 值加两个整数参数，然后用两处 `switch` 覆盖全部 13 种去处。主核若要支持流式与物化两条路径，同样需要一个把「结果去处」显式表达出来的抽象，而不是让每个算子各自决定输出方式。

**架构推论：** SQLite 的协程机制建立在 VDBE 的寄存器交换之上（`OP_Yield` 交换程序计数器），这意味着协程的挂起状态只有一个整数宽。主核若要做等价的事，需要先决定挂起点能保存多少状态——这个决定会直接影响算子能否在协程边界上跨行推进。

**架构推论：** 对 AI 生成 SQL 的场景，这套机制的价值在于「写对了就一定对」：物化路径与协程路径的语义完全等价，SQL 文本里看不出区别，AI 不需要知道 SQLite 选了哪条路。反过来，AI 也无法通过 SQL 文本要求某条路径——`PRAGMA` 层面能看到的只有 `SQLITE_Coroutines` 这个全局优化开关。

**架构推论：** 物化与协程的选择完全发生在编译期，运行时无代价差异。AI 若要解释「为什么这条查询慢」，需要区分「结果集被完整落盘」与「逐行 yield」两种形态——前者受临时文件与页缓存压力影响，后者受每行寄存器搬运影响，两者瓶颈不同。

## 参考

- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层，含 co-routine 的定位）](https://sqlite.org/arch.html)
- [SQLite VDBE Programmer's Guide（协程与 OP_Yield / OP_InitCoroutine 的官方说明）](https://sqlite.org/vdbepg.html)
- [SQLite Opcode List（OP_InitCoroutine / OP_Yield / OP_EndCoroutine / OP_OpenEphemeral 的操作码文档）](https://sqlite.org/opcode.html)
- [SQLite SELECT（FROM 子句中子查询与视图的官方语义）](https://sqlite.org/lang_select.html)
- [SQLite Query Planner（查询规划总览）](https://sqlite.org/optoverview.html)
- [SQLite Documentation Index（官方文档总入口）](https://sqlite.org/docs.html)
- [sqlite/sqlite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c)
- [sqlite/sqlite 源码：src/delete.c](https://github.com/sqlite/sqlite/blob/master/src/delete.c)
- [sqlite/sqlite 源码：src/update.c](https://github.com/sqlite/sqlite/blob/master/src/update.c)
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c)
- [sqlite/sqlite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
- [sqlite/sqlite 源码：src/vdbeaux.c](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
