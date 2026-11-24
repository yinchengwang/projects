---
project: sqlite
feature: where-loop-optimization
page_type: feature
schema_version: 1
refs:
  - path: src/where.c
    symbols: [sqlite3WhereBegin, sqlite3WhereEnd, sqlite3WhereCodeOneLoopStart, sqlite3ShowWhereLoop]
---

# WHERE 条件的索引选择

## 一句话职责

把一条查询的 WHERE 条件翻译成「每张表用哪个索引、按什么顺序扫」的嵌套循环计划：为每张表枚举候选索引并估代价，在所有满足依赖关系的候选组合中挑出总代价最低的那条路径，最后把选中的路径翻译成具体的 VDBE 扫描指令。

## 机制是什么

SQLite 的查询执行模型是「嵌套循环」：FROM 子句里的每张表对应一层循环，WHERE 条件里可被索引满足的项决定这一层是走索引定位还是走全表扫描。本机制只负责**挑哪条路径**，不负责条件求值（那是 `expr.c` 的职责），也不负责 B-tree 的实际定位（那是 `btree.c` 的职责）。

计划器内部用三种对象表达这件事：

| 对象 | 语义 | 关键字段 |
|---|---|---|
| `WhereTerm` | WHERE 子句按 AND 切分后的**单个条件项**（`a=1`、`b>2` 这类） | `eOperator`（比较运算符位掩码）、`prereqAll`/`prereqRight`（依赖哪些表的位掩码）、`leftCursor`/`u.x.leftColumn`（约束落在哪张表哪一列） |
| `WhereLoop` | 某张表的**一种候选走法**（用哪个索引、匹配哪些条件项） | `prereq`（必须先跑完哪些循环）、`rSetup`/`rRun`/`nOut`（建立代价、单行代价、输出行数）、`wsFlags`（`WHERE_*` 标志位）、`u.btree.pIndex`（选中的索引） |
| `WherePath` | 一条完整的**候选计划**：每张表各挑一个 `WhereLoop` 组成的有序序列 | `rCost`（总代价）、`nRow`（输出行数）、`isOrdered`（已满足几个 ORDER BY 项）、`maskLoop`（已覆盖的表位掩码） |

**候选从哪来。** 每张表由 `whereLoopAddBtree()` 枚举：先造一个假的主键索引对象 `sPk`（`idxType = SQLITE_IDXTYPE_IPK`）代表 rowid 主键，再串上该表的全部真实索引。带 `INDEXED BY` 的表只探那一个索引；`WITHOUT ROWID` 表的主键索引由 `sqlite3PrimaryKeyIndex()` 提供。**自动索引（`WHERE_AUTO_INDEX`）是独立的一路候选**，与真实索引并列参与竞争。

**代价怎么算。** 计划器不枚举所有组合，而是逐层做「N 选一」：对每个可能的下一层，从已有的候选 `WherePath` 出发，试着接上每个 `WhereLoop`，算出代价后只保留最好的若干条，再进入下一层。最后一条路径的 `rCost` 最小者胜出。

**最终产出是什么。** 计划器不产出「计划表」，而是直接产出**代码**：`sqlite3WhereBegin()` 一边挑路径一边把「打开游标 / 定位 / 取下一行」翻译成 VDBE 指令；`sqlite3WhereEnd()` 补上循环收尾，并把「读表列」改写成「读索引列」（覆盖索引优化）。

## 怎么实现的

### `sqlite3WhereBegin()`：入口与整体编排

`sqlite3WhereBegin()` 位于 `src/where.c`，签名为 `WhereInfo *sqlite3WhereBegin(Parse *pParse, SrcList *pTabList, Expr *pWhere, ExprList *pOrderBy, ExprList *pResultSet, Select *pSelect, u16 wctrlFlags, int iAuxArg)`。返回的 `WhereInfo` 是整个计划器的状态载体（`src/whereInt.h` 的 `struct WhereInfo`），末尾由 `sqlite3WhereEnd()` 释放。

**开头的三道门槛。**

1. **ORDER BY / GROUP BY 项数上限。** `if( pOrderBy && pOrderBy->nExpr>=BMS )` 时把 `pOrderBy` 置 0、去掉 `WHERE_WANT_DISTINCT`、加上 `WHERE_KEEP_ALL_JOINS`。`BMS` 是位掩码的位数——源码 `src/sqliteInt.h` 写的是 `#define BMS ((int)(sizeof(Bitmask)*8))`，即 `Bitmask` 类型占 8 字节时的 64 位。排序项数达到 `BMS` 就不再走排序消除优化。
2. **FROM 表数上限。** `if( pTabList->nSrc>BMS )` 直接 `sqlite3ErrorMsg(pParse, "at most %d tables in a join", BMS)` 并 `return 0`。也就是说**一张查询最多只能连接 `BMS` 张表**（在 `Bitmask` 为 64 位的平台上即 64 张）。
3. **OR 子句模式只编第一张表。** `nTabList = (wctrlFlags & WHERE_OR_SUBCLAUSE) ? 1 : pTabList->nSrc;`。

**内存布局。** `nByteWInfo = SZ_WHEREINFO(nTabList)`（`src/whereInt.h`：`ROUND8(offsetof(WhereInfo,a)+(N)*sizeof(WhereLevel))`），一次 `sqlite3DbMallocRawNN` 分配 `nByteWInfo + sizeof(WhereLoop)` 整块：`WhereInfo` 结构 + `WhereInfo.a[]` 的 `WhereLevel` 数组 + `WhereClause` + `WhereMaskSet` + 一个 `WhereLoop`。`WhereClause` 含 8 字节的 `Bitmask` 字段，需要 8 字节对齐，所以 `SZ_WHEREINFO` 里带 `ROUND8`。

**拆条件与分析。** `sqlite3WhereClauseInit()` 初始化条件表，`sqlite3WhereSplit(&pWInfo->sWC, pWhere, TK_AND)` 按 AND 把 WHERE 拆成多个 `WhereTerm`；`sqlite3WhereExprAnalyze(pTabList, &pWInfo->sWC)` 逐项分析；`pSelect->pLimit` 存在时 `sqlite3WhereAddLimit()` 把 LIMIT 也并进条件表。

**恒假条件旁路（False-WHERE-Term-Bypass）。** 源码注释列了四个条件：(1) 该项不引用任何 join 中的表（`pT->prereqAll==0`）；(2) 不是 LEFT/FULL JOIN 右侧的 ON 子句项；(3) 不是 ON 子句项，或 `pTabList` 里没有 RIGHT/FULL OUTER JOIN；(4) 表达式是确定性的（`exprIsDeterministic(pX)`）。四项都满足时，该项被提前求值并直接跳到 `pWInfo->iBreak`，即整条查询不产生任何输出行。

**DISTINCT 的三条分支。** `WHERE_WANT_DISTINCT` 置位时：若 `OptimizationDisabled(db, SQLITE_DistinctOpt)` 则清掉该标志（`sqlite3_test_ctrl(SQLITE_TESTCTRL_OPTIMIZATIONS,...)` 可关）；否则若 `isDistinctRedundant()` 判定 DISTINCT 冗余则置 `pWInfo->eDistinct = WHERE_DISTINCT_UNIQUE`；否则若没有 ORDER BY，则把 `pWInfo->pOrderBy` 改成 `pResultSet` 并加 `WHERE_DISTINCTBY`，打算靠排序结果消重。

**核心流程：候选枚举 → 路径求解。**

```
if( nTabList!=1 || whereShortCut(&sWLB)==0 ){
    rc = whereLoopAddAll(&sWLB);      /* 枚举全部候选 WhereLoop */
    ...
    wherePathSolver(pWInfo, 0);        /* 第一趟：先求行数估计 */
    if( pWInfo->pOrderBy ){
        whereInterstageHeuristic(pWInfo);
        wherePathSolver(pWInfo, pWInfo->nRowOut<0 ? 1 : pWInfo->nRowOut+1);
    }
    if( (pWInfo->wctrlFlags & WHERE_WANT_DISTINCT)!=0 ){
        pWInfo->nRowOut -= Tuning(30,24);
    }
}
```

`whereShortCut()` 是**单表简单查询的捷径**：只有一张表、没有 `INDEXED BY` / `NOT INDEXED`、不是虚表、且有一个等值项落在 rowid 上（`WHERE_COLUMN_EQ|WHERE_IPK|WHERE_ONEROW`）或一个唯一索引的全部键列上（`WHERE_COLUMN_EQ|WHERE_ONEROW|WHERE_INDEXED`）时才走捷径。rowid 查找的代价是 `Tuning(33,22)`（源码注释写明 `33==sqlite3LogEst(10)`，即 10 次查找），唯一索引查找是 `Tuning(39,23)`（注释写明 `39==sqlite3LogEst(15)`，即 15 次查找）。不满足条件时返回 0，转入通用规划器。

**DISTINCT 的行数折减**：`pWInfo->nRowOut -= Tuning(30,24);`，源码注释写明这是假设 DISTINCT 把输出规模缩小 8 倍（LogEst -30），并指向 tag-20250414a。

**去掉无效表（omit-noop-join）。** 条件是 `pWInfo->nLevel>=2` 且 `pResultSet!=0` 且 `wctrlFlags` 不含 `WHERE_AGG_DISTINCT` / `WHERE_KEEP_ALL_JOINS` 且 `OptimizationEnabled(db, SQLITE_OmitNoopJoin)`，满足时调 `whereOmitNoopJoin(pWInfo, notReady)`，并把 `nTabList` 改成 `pWInfo->nLevel`（去掉被裁掉的层）。源码注释说明该优化被刻意拆成独立函数，是为了不让 `sqlite3WhereBegin()` 变得过大——因为「函数太大会妨碍 C 编译器内联 `sqlite3WhereCodeOneLoopStart()`，而内联它对性能很重要」。

**Bloom filter 判定。** `pWInfo->nLevel>=2 && OptimizationEnabled(db, SQLITE_BloomFilter) && !sqlite3WhereMightRerunAfterWrite(pWInfo)` 时调 `whereCheckIfBloomFilterIsUseful(pWInfo)`。Bloom 过滤器的具体生成在 `sqlite3ConstructBloomFilter()`，源码注释在 `src/where.c` 里写明 `sqlite3WhereCodeOneLoopStart()` 会生成测试 Bloom 过滤器的代码。

**ONEPASS 优化。** `WHERE_ONEPASS_DESIRED` 置位时，检查 `pWInfo->a[0].pWLoop->wsFlags` 里的 `WHERE_ONEROW`；或同时满足「`WHERE_ONEPASS_MULTIROW` 已置位」「不是虚表」「（没走 OR 优化 或 `WHERE_DUPLICATES_OK` 已置位）」「`OptimizationEnabled(db, SQLITE_OnePass)`」。满足则 `pWInfo->eOnePass` 置 `ONEPASS_SINGLE` 或 `ONEPASS_MULTI`。源码注释说明 `WHERE_DUPLICATES_OK` 只对 DELETE 指定，这是为了让 UPDATE 用 `pWInfo->aiCurOnePass[1]` 判断能否真用 one-pass。

**打开游标。** 逐层 `sqlite3OpenTable(pParse, pTabItem->iCursor, iDb, pTab, op)`，`op` 在 one-pass 时是 `OP_OpenWrite`、否则 `OP_OpenRead`。若循环用索引（`pLoop->wsFlags & WHERE_INDEXED`），再开一个索引游标：`sqlite3VdbeAddOp3(v, op, iIndexCur, pIx->tnum, iDb)` 加 `sqlite3VdbeSetP4KeyInfo(pParse, pIx)`。当循环带等值约束、无范围约束、非 skip-scan、非 big-null 排序、非 IN seek-scan、且不是 `WHERE_ORDERBY_MIN`、`eDistinct != WHERE_DISTINCT_ORDERED` 时，把 P5 置 `OPFLAG_SEEKEQ`（走等值 seek）。

**RIGHT JOIN 的额外设施。** 检测到 `JT_RIGHT` 时分配 `WhereRightJoin`：`pRJ->iMatch = pParse->nTab++`（一个临时索引游标）、`pRJ->regReturn = ++pParse->nMem`，并 `sqlite3VdbeAddOp2(v, OP_OpenEphemeral, pRJ->iMatch, 1)` 打开临时索引记录已匹配的主键；若表有 rowid 则 `bBloomable = 1`，之后 `pRJ->regBloom = ++pParse->nMem;` 并 `sqlite3VdbeAddOp2(v, OP_Blob, 65536, pRJ->regBloom)` 分配 65536 字节的 Bloom 过滤器。RIGHT JOIN 还会强制 `pWInfo->nOBSat = 0;` 与 `pWInfo->eDistinct = WHERE_DISTINCT_UNORDERED;`——源码注释说明 RIGHT JOIN 的处理会打乱输出顺序，所以不做 ORDER BY / GROUP BY 消除优化，必须真排序。

**调用 `sqlite3WhereCodeOneLoopStart()`。** 逐层调 `notReady = sqlite3WhereCodeOneLoopStart(pParse,v,pWInfo,ii,pLevel,notReady);`，并把 `pWInfo->iContinue` 更新为该层的 `addrCont`。

**错误路径。** `whereBeginError:` 标签统一处理失败：恢复 `pParse->nQueryLoop = pWInfo->savedNQueryLoop`、调 `whereInfoFree(db, pWInfo)`、`return 0`。触发点包括 `db->mallocFailed`、`pParse->nErr` 非零、以及 `whereLoopAddAll()` 返回非 `SQLITE_OK`。

### `whereLoopAddAll()`：候选枚举的编排

`whereLoopAddAll()` 逐张 FROM 表处理，每张表：

- **算前置依赖 `mPrereq`。** 遇到 `JT_OUTER` / `JT_CROSS` / `JT_LTORJ` 时 `mPrereq |= mPrior`，禁止重排；否则（`!hasRightCrossJoin`）`mPrereq = 0`，允许自由重排。源码注释解释 `hasRightCrossJoin` 的作用：防止 FROM 项从 LEFT JOIN / CROSS JOIN 的右侧跑到左侧，「对 LEFT JOIN 这是必需的——不强制会导致答案错误」。`bFirstPastRJ` 布尔量保证 RIGHT JOIN 的右操作数不会与更右边的项交换。
- **虚表走 `whereLoopAddVirtual()`，普通表走 `whereLoopAddBtree()`。**
- **有 OR 时额外走 `whereLoopAddOr()`。** 条件是 `pBuilder->pWC->hasOr`。

**规划预算。** `pBuilder->iPlanLimit = SQLITE_QUERY_PLANNER_LIMIT;` 每处理一张表前 `pBuilder->iPlanLimit += SQLITE_QUERY_PLANNER_LIMIT_INCR;`。两个常量的定义在 `src/whereInt.h`，可用 `#ifndef` 覆盖：默认 `SQLITE_QUERY_PLANNER_LIMIT` 是 **20000**、`SQLITE_QUERY_PLANNER_LIMIT_INCR` 是 **1000**。源码注释说明：`SQLITE_QUERY_PLANNER_LIMIT` 是基线上限，处理每张 FROM 表前先加上 `SQLITE_QUERY_PLANNER_LIMIT_INCR`，保证**即使前面的表已耗尽基线预算，后面的表仍能提出一些索引+约束组合**。超限时 `rc==SQLITE_DONE`，转为 `sqlite3_log(SQLITE_WARNING, "abbreviated query algorithm search")` 并把 `rc` 置回 `SQLITE_OK`。

### `whereLoopAddBtree()`：单张表的候选索引

`whereLoopAddBtree()` 对一张普通表产出多组候选。

**假的主键索引。** 有 rowid 的表会造一个局部 `Index sPk`：`sPk.nKeyCol = 1; sPk.nColumn = 1; sPk.aiColumn = &aiColumnPk;`（`aiColumnPk = -1`，表示 rowid）、`sPk.onError = OE_Replace`、`sPk.idxType = SQLITE_IDXTYPE_IPK`、`sPk.szIdxRow = Tuning(3,11)`（源码注释：IPK 表内部行很小）、`sPk.aiRowLogEst[0] = pTab->nRowLogEst`。`sPk.pNext` 串到 `pSrc->pSTab->pIndex`（真实索引链），`pProbe = &sPk`。

**自动索引候选。** 触发条件一串：不属于 OR 优化（`!pBuilder->pOrSet`）、`wctrlFlags` 不含 `WHERE_RIGHT_JOIN` / `WHERE_OR_SUBCLAUSE`、`db->flags & SQLITE_AutoIndex` 非零、无 `INDEXED BY`、无 `NOT INDEXED`、非关联子查询、非递归 CTE、不是 RIGHT JOIN 的右表。每个可驱动索引的条件项（`termCanDriveIndex(pTerm, pSrc, 0)`）产出一条候选。代价：

- `pNew->rSetup = rLogSize + rSize;` 然后普通表加 `Tuning(28,12)`、视图/物化临时表减 `Tuning(25,13)`（源码注释说视图/子查询的自动索引建立代价「greatly reduced」，因为没法给子查询加 schema 索引）。
- `pNew->nOut = Tuning(43,14);`（源码注释写明 `43==sqlite3LogEst(20)`，即每次索引查找假设产出 20 行，「比通常的 10 行的猜测更大，因为我们无从知道索引最终有多选择性的」）。
- `pNew->rRun = sqlite3LogEstAdd(rLogSize,pNew->nOut);`

**真实索引候选。** 遍历 `pProbe` 链（`INDEXED BY` 或 `NOT INDEXED` 时只探一个）。跳过条件：部分索引不适用（`whereUsablePartialIndex()` 返回假，注释里挂着 ticket `[98d973b8f5]`）、`pProbe->bNoQuery`（索引显式声明不参与查询）。每个候选初始化 `nEq/nBtm/nTop/nDistinctCol` 为 0、`nSkip` 为 0、`nLTerm` 为 0、`iSortIdx` 为 0、`rSetup` 为 0、`prereq = mPrereq`、`nOut = rSize`（`rSize = pProbe->aiRowLogEst[0]`）。

**SQLite 的成本单位。** 所有代价都用 `LogEst`——即以 10 为底的对数表示（源码注释 `assert( 48==sqlite3LogEst(28) )`、`assert( 43==sqlite3LogEst(20) )`、`assert( 33==sqlite3LogEst(10) )`、`assert( 39==sqlite3LogEst(15) )`、`assert( 10==sqlite3LogEst(2) )` 可证：`sqlite3LogEst(x) = 10*log2(x)`）。

### `wherePathSolver()`：N 选一的路径求解

`wherePathSolver(pWInfo, nRowEst)` 是「选路径」的核心。它把 `WhereLoop` 看成图上的节点、`WherePath` 看成路径。

**每层保留多少条候选。** `mxChoice`：1 张表时是 1；2 张表时是 `Tuning(5,19)`；否则由 `computeMxChoice()` 决定——源码注释的对照表写着「nLoop=3+ 时是 12 或 18」，对应 `computeMxChoice()` 末尾的 `return Tuning(18,25);`（星型查询保留 18 条路径）与 `return Tuning(12,26);`（其余保留 12 条路径）。

**起点。** `aFrom[0].nRow = MIN(pParse->nQueryLoop, Tuning(48,27));` 源码注释写明 `48==sqlite3LogEst(28)`，且注明「TUNING: Do not let the number of iterations go above 28. If the cost of computing an automatic index is not paid back within the first 28 rows, then do not use the automatic index.」

**逐层扩展。** 对每个已有路径 `pFrom`，遍历全部候选 `WhereLoop`（`pWInfo->pLoops` 链表），跳过三类：

1. `pWLoop->prereq & ~pFrom->maskLoop` 非零——依赖的表还没进路径。
2. `pWLoop->maskSelf & pFrom->maskLoop` 非零——这张表已经在路径里。
3. `(pWLoop->wsFlags & WHERE_AUTO_INDEX)!=0 && pFrom->nRow<3`——自动索引在预期只跑不到 1.25 次时不用（源码注释：`assert( 10==sqlite3LogEst(2) )`，即 `nRow<3` 对应 LogEst 2 的量级）。

代价累加：`rUnsort = pWLoop->rRun + pFrom->nRow;`，若 `pWLoop->rSetup` 非零则 `rUnsort = sqlite3LogEstAdd(pWLoop->rSetup, rUnsort)`。

**调用时机。** `sqlite3WhereBegin()` 里 `wherePathSolver()` 最多调两次：第一次传 `nRowEst = 0`（源码注释说明：此时若 `pWInfo->pOrderBy==0 || nRowEst==0` 则 `nOrderBy = 0`，即忽略排序代价，只为估出输出行数）；第二次传 `pWInfo->nRowOut<0 ? 1 : pWInfo->nRowOut+1`，此时排序代价被计入。

**中间启发式 `whereInterstageHeuristic()`。** 位于 `src/where.c`，在两次求解之间执行。逐层检查已选的 `WhereLoop`：遇到 `WHERE_VIRTUALTABLE` 就 `break`（虚表扫描视同全表扫描）；若该层有 `WHERE_COLUMN_EQ` / `WHERE_COLUMN_NULL` / `WHERE_COLUMN_IN` 之一，则把同表的其他候选循环的 `prereq` 置成 `ALLBITS`——**源码注释直言这一步的目的就是「Prevent 2nd solver() from using this one」**，即阻止第二轮求解再用这些「本来可以更早定案」的循环。否则 `break`。

### `sqlite3WhereCodeOneLoopStart()`：把选中路径翻译成代码

`sqlite3WhereCodeOneLoopStart()` 实际定义在 **`src/wherecode.c`**（不在 `src/where.c`），签名 `Bitmask sqlite3WhereCodeOneLoopStart(Parse *pParse, Vdbe *v, WhereInfo *pWInfo, int iLevel, WhereLevel *pLevel, Bitmask notReady)`。它负责**一层循环**的代码生成，末尾返回 `pLevel->notReady`。

**六个分支。** 函数体是一条长 if-else 链，按循环类型分六种情况：

1. **协程式子查询**（`pTabItem->fg.viaCoroutine`）：`sqlite3VdbeAddOp3(v, OP_InitCoroutine, regYield, 0, pSubq->addrFillSub)` 初始化协程，`sqlite3VdbeAddOp2(v, OP_Yield, regYield, addrBrk)` 取下一行，`pLevel->op = OP_Goto`。
2. **虚表**（`WHERE_VIRTUALTABLE`）：用 `OP_VFilter` / `OP_VNext`。约束值放进 `sqlite3GetTempRange(pParse, nConstraint+2)` 分配的临时寄存器，末尾 `sqlite3VdbeAddOp4(v, OP_VFilter, iCur, addrNotFound, iReg, pLoop->u.vtab.idxStr, ...)`。源码注释强调「紧邻 `OP_VFilter` 的前一条指令必须是设置 xFilter 的 argc 值的 `OP_Integer`」，并指向 tag-20250207a。
3. **rowid 等值定位**（`WHERE_IPK` + `WHERE_COLUMN_EQ|WHERE_COLUMN_IN`）：`codeEqualityTerm()` 算出 rowid 寄存器，然后 `sqlite3VdbeAddOp3(v, OP_SeekRowid, iCur, addrMiss, iRowidReg)`，`pLevel->op = OP_Noop`（循环体内没有「取下一行」指令，因为只有一行）。
4. **rowid 范围扫描**（`WHERE_IPK` + `WHERE_COLUMN_RANGE`）：`aMoveOp[]` 静态表把 `TK_GT`/`TK_LE`/`TK_LT`/`TK_GE` 映射到 `OP_SeekGT`/`OP_SeekLE`/`OP_SeekLT`/`OP_SeekGE`（源码注释说这个映射依赖 `TK_xx` 的特定顺序，并有三条 `assert` 固化该顺序）。循环步进指令是 `OP_Next`（正序）或 `OP_Prev`（逆序）。范围端点值算完后若是非向量比较，用 `OP_Ge`/`OP_Gt`/`OP_Le`/`OP_Lt` 在循环体内逐行判界。
5. **索引扫描**（`WHERE_INDEXED`）：核心分支。起始 seek 操作码由 `aStartOp[]` 查表得到，索引是 4 位编码：`(start_constraints<<2) + (startEq<<1) + bRev`。`aStartOp[]` 的 8 项分别是 `0`、`0`、`OP_Rewind`、`OP_Last`、`OP_SeekGT`、`OP_SeekLT`、`OP_SeekGE`、`OP_SeekLE`。范围终点操作码由 `aEndOp[]` 查表（2 位编码 `bRev*2 + endEq`），四项是 `OP_IdxGE`、`OP_IdxGT`、`OP_IdxLE`、`OP_IdxLT`。等值项由 `codeAllEqualityTerms()` 求值并放进 `regBase` 起的连续寄存器；范围项紧随其后。**跳过扫描**时先发一条 `OP_SeekScan`，其 P1 是 `(pIdx->aiRowLogEst[0]+9)/10`——源码注释解释：「一次 seek 的代价正比于树中条目数的对数，所以按 B-tree 估计行数来决定试走多少步看起来是个好猜测」。
6. **全表扫描**（无可用索引）：`aStep[] = { OP_Next, OP_Prev }`、`aStart[] = { OP_Rewind, OP_Last }`，`pLevel->p5 = SQLITE_STMTSTATUS_FULLSCAN_STEP`。递归 CTE 的伪游标（`pTabItem->fg.isRecursive`）例外，`pLevel->op = OP_Noop`。

**覆盖索引的决定。** `omitTable = (pLoop->wsFlags & WHERE_IDX_ONLY)!=0 && (pWInfo->wctrlFlags & (WHERE_OR_SUBCLAUSE|WHERE_RIGHT_JOIN))==0;` 为真时完全不碰主表。需要碰主表时：有 rowid 走 `codeDeferredSeek()`；`WITHOUT ROWID` 表则把主键列从索引读出来再 `OP_NotFound` 回查。

**部分索引的 WHERE 消除。** `pIdx->pPartIdxWhere` 存在且该层不是外连接右表时，调 `whereApplyPartialIndexConstraints(pIdx->pPartIdxWhere, iCur, pWC)`，把部分索引自身的 WHERE 条件必然蕴含的项从查询的 WHERE 里去掉。源码注释指出这个优化对外连接无效，并引用了 2019-11-02 的 ticket 623eff57e76d45f6（LEFT JOIN）与 2025-05-29 的论坛帖 7dee41d32506c4ae（RIGHT JOIN）。

**剩余条件的编译（push-down）。** 源码注释描述了一个最多跑三趟的循环：`iLoop==1` 只编译完全被 `pIdx` 覆盖的表达式；`iLoop==2` 编译不含关联子查询的剩余表达式；`iLoop==3` 编译全部剩余。源码说明这一优化在 MySQL 里叫 "push-down"，在 SQLite 里叫 "MySQL push-down"（与另一处完全无关的 "WHERE-clause push-down" 区分）。判定用 `sqlite3ExprCoveredByIndex()` 与 `whereExprIsReady()`。每项用 `sqlite3ExprIfFalse(pParse, pE, addrCont, SQLITE_JUMPIFNULL)` 编译——不满足就跳到 `addrCont`（下一轮）。

**传递性约束。** 源码举例：WHERE 里同时有 `t1.a=t2.b` 和 `t2.b=123` 时，在编 `t1` 循环而 `t2` 循环尚未编译的情况下，不能用 `t1.a=t2.b`，但可以编出隐含的 `t1.a=123`。实现是 `sqlite3WhereFindTerm(pWC, iCur, pTerm->u.x.leftColumn, notReady, WO_EQ|WO_IN|WO_IS, 0)` 找到替代项，再把 `sEAlt.pLeft` 换成 `pE->pLeft` 编译。

**循环步进指令的选择。** 末尾：
```
if( (pLoop->wsFlags & WHERE_ONEROW)
 || (pLevel->u.in.nIn && regBignull==0 && whereLoopIsOneRow(pLoop)) ){
  pLevel->op = OP_Noop;
}else if( bRev ){ pLevel->op = OP_Prev; }else{ pLevel->op = OP_Next; }
```
另外 `pLevel->p3 = (pLoop->wsFlags&WHERE_UNQ_WANTED)!=0 ? 1:0;`，`pLevel->p5` 在无约束（`WHERE_CONSTRAINT` 未置位）时置 `SQLITE_STMTSTATUS_FULLSCAN_STEP`。

### `sqlite3WhereEnd()`：收尾与覆盖索引改写

`sqlite3WhereEnd(WhereInfo *pWInfo)` 在 `src/where.c`。它做三件事：

**1. 逐层收尾循环。** 从最内层 `i = pWInfo->nLevel-1` 向外遍历到 0。RIGHT JOIN 层先 `sqlite3VdbeResolveLabel(v, pLevel->addrCont)` 并把 `pLevel->addrCont` 换成一个新标签（源码注释说这样做是为了让后面那次 resolve 有东西可解），然后 `sqlite3VdbeAddOp3(v, OP_Return, pRJ->regReturn, pRJ->addrSubrtn, 1)` 结束子程序，`nRJ++`。接着按 `pLevel->op` 发出循环步进指令 `OP_Next` / `OP_Prev` / `OP_VNext` / `OP_Return`，再解 `addrCont`（继续）、`addrNxt`（下一个 IN 值）、`addrBrk`（跳出）。

**2. 覆盖索引改写。** 若循环用索引（`pLoop->wsFlags & (WHERE_INDEXED|WHERE_IDX_ONLY)`）或走了多索引 OR（`WHERE_MULTI_OR`，此时索引是 `pLevel->u.pCoveringIdx`），就把 `pLevel->addrBody` 之后、`pWInfo->iEndWhere`（one-pass 且表有 rowid 时）之前的指令扫描一遍，把引用主表游标 `pLevel->iTabCur` 的指令改写成引用索引游标 `pLevel->iIdxCur`：

- `OP_Column`（以及 `SQLITE_ENABLE_OFFSET_SQL_FUNC` 下的 `OP_Offset`）：列号要经 `sqlite3StorageColumnToTable()`（有 rowid）或 `sqlite3PrimaryKeyIndex()` 的 `aiColumn[]`（无 rowid）换算，再经 `sqlite3TableColumnToIndex(pIdx, x)` 换成索引内的列号。
- `OP_Rowid` → `OP_IdxRowid`。
- `OP_IfNullRow` → 改指索引游标。

**改写失败时的错误分支。** 若某列在索引里不存在（`x < 0`）而循环却标着 `WHERE_IDX_ONLY`，源码走 `sqlite3ErrorMsg(pParse, "internal query planner error"); pParse->rc = SQLITE_INTERNAL;`。若标的是 `WHERE_EXPRIDX`（规划器认为「大概」是覆盖索引但不能百分之百确定），则清掉 `pLoop->wsFlags &= ~WHERE_EXPRIDX;` 并调 `sqlite3WhereAddExplainText()` 修正 EXPLAIN 输出里「COVERING INDEX」的措辞——源码注释写明这条路上 `OP_Explain` 描述的是覆盖索引，但 `OP_Column` 证明它不是，于是必须改口。

**3. 收尾与释放。** `sqlite3VdbeResolveLabel(v, pWInfo->iBreak);` 定下跳出点；`pParse->nQueryLoop = pWInfo->savedNQueryLoop;` 恢复计数；`whereInfoFree(db, pWInfo);` 释放计划器状态；`pParse->withinRJSubrtn -= nRJ;` 扣减 RIGHT JOIN 子程序嵌套深度。

### `sqlite3ShowWhereLoop()`：调试输出

`sqlite3ShowWhereLoop(const WhereLoop *p)` 定义在 `src/where.c`，函数体只有一句 `if( p ) sqlite3WhereLoopPrint(p, 0);`。同文件里的 `sqlite3ShowWhereLoopList()` 沿 `p->pNextLoop` 链表逐个打印。`sqlite3WhereLoopPrint()` 打印 `p->rSetup`、`p->rRun`、`p->nOut`、`p->rStarDelta`（WHERETRACE 下），以及 `p->aLTerm[]` 里的每个条件项（需要 `sqlite3WhereTrace & 0x4000`）。这两个函数被 `sqlite3WhereBegin()` 的 `whereBeginError:` 路径调用了一次（`sqlite3ShowWhereLoopList(0);`，源码注释解释这是为了「Prevent harmless compiler warnings about debugging routines being declared but never used」），实际的可观察输出依赖 `WHERETRACE_ENABLED` 编译开关与 `sqlite3WhereTrace` 运行时变量。

## 关键取舍

- **计划器直接产出代码，不产出中间「计划表」。** `sqlite3WhereBegin()` 一边选路径一边开游标、定位、生成步进指令。收益是没有「计划 → 代码」这一层的额外数据结构与转换开销；代价是路径求解与代码生成纠缠在一个函数里，`sqlite3WhereBegin()` 因此非常长，源码注释为此专门说明要把 omit-noop-join 拆出去以保住 `sqlite3WhereCodeOneLoopStart()` 的内联机会。

- **候选枚举与路径求解分成两段。** `whereLoopAddAll()` 先把每张表的所有候选 `WhereLoop` 造出来（链表 `pWInfo->pLoops`），`wherePathSolver()` 再做全局组合。这样路径求解不必关心索引是怎么找到的，只看 `prereq` / `rSetup` / `rRun` / `nOut` 四个数。代价是候选总数可能很大，因此需要 `SQLITE_QUERY_PLANNER_LIMIT`（20000）与 `SQLITE_QUERY_PLANNER_LIMIT_INCR`（1000）这样的硬预算来封顶。

- **只保留有限条路径，而不是穷举。** 每层保留 `mxChoice` 条（1 张表 1 条、2 张表 5 条、3 张表以上 12 或 18 条）。这是经典的 beam search 近似：省计划期开销，代价是不保证全局最优。

- **有硬上限，就必然有降级。** 候选枚举超限时 `rc==SQLITE_DONE`，源码的处理是打一条 `SQLITE_WARNING` 级日志「abbreviated query algorithm search」并把 `rc` 置回 `SQLITE_OK` —— **查询照常执行，只是计划是次优的**。这是明确的降级路径，触发者是 `whereLoopAddAll()` 里的 `rc==SQLITE_DONE` 分支。

- **自动索引是候选，不是命令。** 自动索引有真实的建立代价（`rSetup` 里含 `rLogSize + rSize + Tuning(28,12)`），并且在 `wherePathSolver()` 里被一道门槛挡住：`pFrom->nRow<3` 时不用自动索引（源码注释解释：自动索引的建立成本若在前 28 行内赚不回来就别用）。因此「有没有自动索引」与「用不用自动索引」是两件事。

- **等价判定故意宽松。** `whereLoopIsNoBetter()` 在两个候选代价相同时，比较 `pCandidate->u.btree.pIndex->szIdxRow` 与 `pBaseline->u.btree.pIndex->szIdxRow`：候选索引行更小就判候选更好（返回 0），否则返回 1 表示「分不出高下」。函数注释写明返回 true 的含义是「We cannot tell the difference」。

- **覆盖索引的判定可以事后翻案。** 规划器给 `WHERE_EXPRIDX`（推测是覆盖索引）与 `WHERE_IDX_ONLY`（确定是覆盖索引）两种标记。事后改写时若证明并非覆盖索引，`WHERE_IDX_ONLY` 触发硬错误 `SQLITE_INTERNAL`，`WHERE_EXPRIDX` 则只清标志并改 EXPLAIN 文案。这是「宁可报错也不静默出错」与「推测性优化可退让」两种处理方式的并存。

- **表数与排序项数都有硬顶。** 表数超过 `BMS` 直接报错「at most %d tables in a join」；排序项达到 `BMS` 则放弃排序消除优化但不改结果。**架构推论：** 这个上限不是随手写的一个数，而是位掩码宽度的直接后果——源码 `src/sqliteInt.h` 的 `#define BMS ((int)(sizeof(Bitmask)*8))` 把 `BMS` 绑定到 `Bitmask` 的位数（在 `Bitmask` 为 64 位宽的平台上即 64）。依赖关系、可用表集合、循环前置条件全都用位掩码表达（`WhereLoop.prereq`、`WhereLoop.maskSelf`、`WherePath.maskLoop`、`WhereTerm.prereqAll`），表的个数因此被掩码宽度锁死。这条设计决定了 SQLite 无法支持超过 `BMS` 路连接，也决定了多表查询里「谁能先跑」必须编码成掩码关系。

- **ORDER BY 的消除被刻意做得保守。** RIGHT JOIN 一律 `pWInfo->nOBSat = 0` 并强制真排序；`pOrderBy` 项数达 `BMS` 时直接丢弃排序优化；`WHERE_KEEP_ALL_JOINS` 标志可以强制关掉 omit-noop-join。**架构推论：** 这些「宁可多排序也不排错」的开关集中在少数几处，说明 SQLite 把「结果正确」置于「省一次排序」之上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/where.c | `sqlite3WhereBegin` | 计划器入口：拆 WHERE 项、分析条件、枚举候选、求解路径、打开游标并逐层调 `sqlite3WhereCodeOneLoopStart()` 生成扫描代码 |
| src/where.c | `sqlite3WhereEnd` | 收尾：逐层发出循环步进与跳转指令，把读表列的 `OP_Column`/`OP_Rowid`/`OP_IfNullRow` 改写成读索引列，最后释放 `WhereInfo` |
| src/wherecode.c | `sqlite3WhereCodeOneLoopStart` | 单层循环的代码生成：六个分支（协程 / 虚表 / rowid 等值 / rowid 范围 / 索引扫描 / 全表扫描），并编译剩余 WHERE 项与传递性约束 |
| src/where.c | `sqlite3ShowWhereLoop` | 打印单个 `WhereLoop` 的代价与命中项，转调 `sqlite3WhereLoopPrint()` |
| src/where.c | `whereLoopAddAll` | 候选枚举编排：逐表算前置依赖 `mPrereq`，分派到虚表 / B-tree / OR 三路，监控 `iPlanLimit` 预算 |
| src/where.c | `whereLoopAddBtree` | 单张普通表的候选来源：假主键索引 `sPk`、自动索引候选、真实索引遍历与部分索引过滤 |
| src/where.c | `wherePathSolver` | N 选一路径求解：逐层扩展候选路径，按 `prereq` 过滤、累加 `rSetup`/`rRun` 代价，每层只保留 `mxChoice` 条最优路径 |
| src/where.c | `whereShortCut` | 单表简单查询的快捷路径：rowid 等值定位或唯一索引全键列等值，代价分别取 `Tuning(33,22)` 与 `Tuning(39,23)` |
| src/where.c | `whereInterstageHeuristic` | 两轮求解之间的中间启发式：把已被等值项定案的表的其他候选循环 `prereq` 置 `ALLBITS`，阻止第二轮求解再用 |
| src/where.c | `constructAutomaticIndex` | 自动索引的代码生成：收集可驱动索引的条件项、确定键列、构造覆盖索引并生成填充循环 |
| src/where.c | `whereOmitNoopJoin` | 去掉不影响结果的表，使 `pWInfo->nLevel` 下降；由 `SQLITE_OmitNoopJoin` 开关控制 |
| src/where.c | `whereCheckIfBloomFilterIsUseful` | 判定是否有 SEARCH 循环适合用 Bloom 过滤器；由 `SQLITE_BloomFilter` 开关控制 |
| src/where.c | `isDistinctRedundant` | 判定 DISTINCT 是否多余，为真时置 `pWInfo->eDistinct = WHERE_DISTINCT_UNIQUE` |
| src/where.c | `whereReverseScanOrder` | 无 ORDER BY 且连接带 `SQLITE_ReverseOrder` 标志时反转扫描方向 |
| src/where.c | `whereLoopIsNoBetter` | 两个候选代价持平时用索引行大小 `szIdxRow` 细判，返回 true 表示分不出高下 |
| src/wherecode.c | `whereApplyPartialIndexConstraints` | 部分索引的 WHERE 消除：把部分索引自身 WHERE 必然蕴含的项从查询条件里去掉 |
| src/wherecode.c | `codeAllEqualityTerms` | 把等值项（`==` / `IN` / `IS`）求值到 `regBase` 起的连续寄存器，含 skip-scan 的游标定位 |
| src/wherecode.c | `codeEqualityTerm` | 单个等值项的求值，产出 rowid 或等值比较的寄存器与跳转目标 |
| src/wherecode.c | `whereExprIsReady` | 判定某个 WHERE 项的依赖是否已就绪，可编译时才编 |
| src/wherecode.c | `findNoMatchContinuation` | 计算「无匹配行时跳到哪里」的续点，用于 rowid 等值定位与 Bloom 过滤器过滤 |
| src/wherecode.c | `whereLoopIsOneRow` | 判定循环是否只产出一行，用于把步进指令换成 `OP_Noop` |
| src/wherecode.c | `sqlite3WhereRightJoinLoop` | RIGHT JOIN 未匹配行的补齐循环：把左侧各表置 NULL 行后重扫右侧表并做 Bloom / 主键去重 |
| src/whereInt.h | `WhereTerm` | 单个 WHERE 条件项：`eOperator` 比较运算符位掩码、`prereqAll`/`prereqRight` 依赖位掩码、`leftCursor` 约束所在表 |
| src/whereInt.h | `WhereLoop` | 单表的一种候选走法：`prereq` 依赖、`rSetup`/`rRun`/`nOut` 代价、`wsFlags` 的 `WHERE_*` 标志、`u.btree.pIndex` 选中索引 |
| src/whereInt.h | `WherePath` | 一条完整候选计划：`rCost` 总代价、`nRow` 输出行数、`isOrdered` 已满足的 ORDER BY 项数、`maskLoop` 已覆盖表位掩码 |
| src/whereInt.h | `WhereInfo` | 计划器状态载体：`pLoops` 候选链表、`a[]` 各层选中的循环、`sWC` 条件表、`revMask`、`nLevel`、`eOnePass`、`eDistinct` |
| src/whereInt.h | `WHERE_COLUMN_EQ` / `WHERE_COLUMN_RANGE` / `WHERE_COLUMN_IN` / `WHERE_CONSTRAINT` | 条件类型标志位：`WHERE_CONSTRAINT` 是前四位的合集（`0x0000000f`） |
| src/whereInt.h | `WHERE_IDX_ONLY` / `WHERE_IPK` / `WHERE_INDEXED` / `WHERE_VIRTUALTABLE` / `WHERE_AUTO_INDEX` | 循环类型标志位：覆盖索引、rowid 主键、已选索引、虚表、自动索引 |
| src/whereInt.h | `SQLITE_QUERY_PLANNER_LIMIT` / `SQLITE_QUERY_PLANNER_LIMIT_INCR` | 候选枚举预算：默认 20000 与每张表递增 1000，可用 `#ifndef` 覆盖 |
| src/whereInt.h | `WhereOrSet` / `N_OR_COST` | OR 子句的候选代价集合，`N_OR_COST` 为 3（只保留最好的三条） |

## 与主核 / AI 的关系

主数据库内核（`engine/db/`）当前没有独立的查询规划层——`engine/db` 的执行器是单表扫描加过滤，索引由 `engine/db/storage/` 承担。

**架构推论：** SQLite 这套规划器的三段式结构——**候选枚举**（`whereLoopAddBtree`）、**路径求解**（`wherePathSolver`）、**代码生成**（`sqlite3WhereCodeOneLoopStart`）——对主核有直接参考价值。主核若要在存储层之上引入多表连接，应当照此把「有哪些走法」与「哪条走法更便宜」分成两件事：前者是局部信息（每张表自己知道有哪些索引），后者才是全局决策。当前 `engine/db/` 若直接在执行器里「有索引就用索引」，就丢掉了全局比价这一步。

**架构推论：** SQLite 的降级路径是显式且可追踪的——候选枚举超预算时打 `SQLITE_WARNING`「abbreviated query algorithm search」并照常执行；覆盖索引判定被推翻时，一类是 `SQLITE_INTERNAL` 硬错误，另一类是清标志改文案。主核的存储层如果要支持可插拔，应当照此设计明确的降级链，而不是让存储引擎静默降级。

对 AI 场景（模型生成 SQL 交给数据库执行）而言，这一层决定「模型写的条件能不能被优化器转成好计划」。**架构推论：** SQLite 的候选生成完全由**条件的书写形式**决定：等值项、范围项、IN 项、LIKE 项各自映射到不同的 `WhereTerm.eOperator` 位，能被索引满足的项才会进入 `WhereLoop.aLTerm[]`。这意味着模型能学到的是「条件该写成什么形式才可能命中索引」，而**学不到哪条路径更便宜**——后者是 `wherePathSolver()` 内部的代价比较，模型无法预判。若要让 AI 拿到反馈，最有价值的暴露点是 `EXPLAIN QUERY PLAN`（`sqlite3WhereExplainOneScan()`、`sqlite3WhereAddScanStatus()` 产出的扫描描述）与 `WHERETRACE` 调试输出，而不是最终计划本身。

**架构推论：** `whereInterstageHeuristic()` 那句「Prevent 2nd solver() from using this one」——把已被定案的表的其余候选循环的 `prereq` 直接置成 `ALLBITS`——是一种「让后续求解看不见某些候选」的手法。主核若要把规划分成多趟（先定连接顺序、再定索引），这种「靠改依赖关系来屏蔽候选」的做法比维护额外的「已定案」集合更省状态，但也更难推理。

## 参考

- [Query Planner Overview（SQLite 官方查询规划器文档）](https://sqlite.org/optoverview.html)
- [EXPLAIN QUERY PLAN（SQLite 官方文档）](https://sqlite.org/eqp.html)
- [SQLite 源码浏览：src/where.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/where.c)
- [sqlite/sqlite 仓库中的 src/where.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/where.c)
- [sqlite/sqlite 仓库中的 src/wherecode.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/wherecode.c)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [The VDBE Bytecode Engine（SQLite 官方文档，含 `OP_SeekGE` / `OP_Next` 等操作码说明）](https://sqlite.org/opcode.html)
