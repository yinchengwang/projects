---
project: sqlite
feature: group-by-and-aggregate
page_type: feature
schema_version: 1
refs:
  - path: src/expr.c
    symbols: [sqlite3ExprAnalyzeAggList, sqlite3ExprAnalyzeAggregates]
---

# 分组与聚合

## 一句话职责

分组与聚合在编译期把「哪些表达式是分组键」「哪些调用是聚合函数」「每个聚合函数要读哪些列」这三件事算清楚，结果写进 `AggInfo` 结构里的 `aCol[]` 与 `aFunc[]` 两张表，供后续代码生成阶段把分组键取到排序索引里、把聚合调用翻译成 `OP_AggStep` / `OP_AggFinal`。

## 机制是什么

边界（依据条目 `boundary`）：负责 GROUP BY/HAVING 的分组键提取与聚合累加器的组织；不负责聚合函数本身的实现（L 组）、不负责排序的执行（D06）。

### 编译期 vs 执行期

SQLite 把「聚合查询」拆成两个阶段，两者由 `AggInfo` 这一个结构缝合：

| 阶段 | 承担者 | 产物 | 落在哪 |
|---|---|---|---|
| 编译期（分析） | `sqlite3ExprAnalyzeAggregates` / `sqlite3ExprAnalyzeAggList` + 回调 `analyzeAggregate` | `AggInfo.aCol[]`（用到的源表列）、`AggInfo.aFunc[]`（聚合函数）、`AggInfo.pGroupBy`（分组键）、`AggInfo.nSortingColumn` | `src/expr.c` |
| 编译期（寄存器分配） | `assignAggregateRegisters` | `AggInfo.iFirstReg`，即累加器寄存器区起点 | `src/select.c` |
| 编译期（代码生成） | `updateAccumulator` | `OP_AggStep` / `OP_AggFinal` / `OP_OpenEphemeral` 等 VDBE 指令 | `src/select.c` |

本条目覆盖的是第一行——**分析阶段**。累加器寄存器怎么排、聚合函数体怎么执行，属 L 组与 `src/select.c` 的代码生成部分。

### AggInfo 的两张表

`AggInfo` 结构定义在 `src/sqliteInt.h`：

- `aCol[]`（元素类型 `AggInfo_col`）：每个被引用到的源表列一项。成员含 `pTab`（源表）、`pCExpr`（原始表达式）、`iTable`（源表游标号）、`iColumn`（列号）、`iSorterColumn`（该列在排序索引里的列号）。
- `aFunc[]`（元素类型 `AggInfo_func`）：每个聚合函数一项。成员含 `pFExpr`（表达式）、`pFunc`（`FuncDef`，即函数实现）、`iDistinct`（强制 DISTINCT 用的临时表游标）、`iDistAddr`（`OP_OpenEphemeral` 的地址）、`iOBTab`（聚合内部 ORDER BY 用的临时表游标）、`bOBPayload`、`bOBUnique`、`bUseSubtype`。

`AggInfo` 的注释明确写出 `pGroupBy` 与 `aFunc.pExpr` 指向原 `Select` 结构里的字段，释放 `AggInfo` 时不必单独释放它们。

### 编译器如何知道「这是聚合函数」

`src/resolve.c` 的名字解析阶段把函数调用的 `op` 从 `TK_FUNCTION` 改写成 `TK_AGG_FUNCTION`，并把 `op2` 置 0，随后按外层嵌套层数累加：`pExpr->op2 += (1 + pNC2->nNestedSelect)`。

`op2` 的语义是**该聚合函数所属的查询嵌套深度**。`src/expr.c` 的 `analyzeAggregate` 用 `pWalker->walkerDepth==pExpr->op2` 判断「当前遍历到的这个聚合函数，恰好属于我这一层 SELECT」——这是把聚合函数归属到正确 SELECT 层的唯一依据。`src/window.c` 里的窗口函数解析也用同一套判据（`pExpr->op2!=pWalker->walkerDepth` 则跳过）。

## 怎么实现的

### 第一步：sqlite3ExprAnalyzeAggregates 装配遍历器

`sqlite3ExprAnalyzeAggregates(NameContext *pNC, Expr *pExpr)` 定义在 `src/expr.c`。它的函数头注释写明：分析表达式找聚合函数，以及找出需要加入 `AggInfo` 对象的变量；**这个例程只能在表达式已被 `sqlite3ResolveExprNames()` 分析之后调用**。

函数体全部内容是装配一个 `Walker` 再走一遍表达式树：

```c
void sqlite3ExprAnalyzeAggregates(NameContext *pNC, Expr *pExpr){
  Walker w;
  w.xExprCallback = analyzeAggregate;
  w.xSelectCallback = sqlite3WalkerDepthIncrease;
  w.xSelectCallback2 = sqlite3WalkerDepthDecrease;
  w.walkerDepth = 0;
  w.u.pNC = pNC;
  w.pParse = 0;
  assert( pNC->pSrcList!=0 );
  sqlite3WalkExpr(&w, pExpr);
}
```

关键点：

- `w.pParse = 0` —— 遍历器不带 `Parse`，因为这个阶段不需要生成代码。
- `xSelectCallback` 用 `sqlite3WalkerDepthIncrease`、`xSelectCallback2` 用 `sqlite3WalkerDepthDecrease`（都在 `src/walker.c`）：进入子查询时 `walkerDepth++`，离开时 `walkerDepth--`。这正是 `op2` 能与 `walkerDepth` 对上的原因。
- `w.u.pNC = pNC` 把 `NameContext` 挂在遍历器的联合体里，回调 `analyzeAggregate` 通过 `pWalker->u.pNC` 取回它。

`src/walker.c` 的 `sqlite3WalkExprNN` 是前序遍历：先调 `xExprCallback`，再按 `pLeft` → `pRight` → `x.pList` → `x.pSelect` 的顺序下潜。回调返回 `WRC_Prune` 表示不再下潜子节点但继续处理兄弟节点。

### 第二步：sqlite3ExprAnalyzeAggList 逐项遍历列表

`sqlite3ExprAnalyzeAggList(NameContext *pNC, ExprList *pList)` 是列表版包装，函数头注释写明「对列表里每个表达式调 `sqlite3ExprAnalyzeAggregates()`」：

```c
void sqlite3ExprAnalyzeAggList(NameContext *pNC, ExprList *pList){
  struct ExprList_item *pItem;
  int i;
  if( pList ){
    for(pItem=pList->a, i=0; i<pList->nExpr; i++, pItem++){
      sqlite3ExprAnalyzeAggregates(pNC, pItem->pExpr);
    }
  }
}
```

注意 `if( pList )` 的空列表保护——传入 NULL 列表时直接跳过，不解引用。

**真实调用方**（`src/select.c`）：

- `sqlite3ExprAnalyzeAggList(&sNC, pEList)` —— 分析 SELECT 结果列。
- `sqlite3ExprAnalyzeAggList(&sNC, sSort.pOrderBy)` —— 分析 ORDER BY 列表。
- `sqlite3ExprAnalyzeAggList(pNC, pExpr->x.pList)` —— 在 `analyzeAggFuncArgs` 里分析聚合函数的实参列表。
- `sqlite3ExprAnalyzeAggList(pNC, pExpr->pLeft->x.pList)` —— 同处，分析聚合函数内部的 ORDER BY 列表。
- `sqlite3ExprAnalyzeAggregates(pNC, pExpr->y.pWin->pFilter)` —— 同处，分析窗口函数的 FILTER 表达式。

`sqlite3ExprAnalyzeAggregates` 的另一处真实调用方是 `src/select.c` 里对 HAVING 子句的分析：`sqlite3ExprAnalyzeAggregates(&sNC, pHaving)`。

### 第三步：analyzeAggregate 回调的四条分支

`analyzeAggregate(Walker *pWalker, Expr *pExpr)` 是 `src/expr.c` 的静态函数，函数头注释写明它是 `sqlite3ExprAnalyzeAggregates()` 的 `xExprCallback`。它按 `pExpr->op` 分派四条路径。

#### 分支一：TK_AGG_FUNCTION —— 登记聚合函数

这是本页最核心的分支。进入条件是三个条件同时成立：

```c
if( (pNC->ncFlags & NC_InAggFunc)==0
 && pWalker->walkerDepth==pExpr->op2
 && pExpr->pAggInfo==0
){
```

逐条解释：

- `NC_InAggFunc` 未置位 —— 当前不在「正在分析聚合函数的实参」这一层。`NC_InAggFunc` 定义在 `src/sqliteInt.h`，值为 `0x020000`，注释是 "True if analyzing arguments to an agg func"。它在 `analyzeAggFuncArgs` 里被置位、函数末尾被清掉。
- `pWalker->walkerDepth==pExpr->op2` —— 该聚合函数属于当前这一层 SELECT。
- `pExpr->pAggInfo==0` —— 该表达式尚未被登记过。

**去重：** 函数先遍历 `pAggInfo->aFunc[]`，用 `sqlite3ExprCompare(0, pItem->pFExpr, pExpr, -1)` 比较。`sqlite3ExprCompare` 的注释写明：返回 0 或 1 可以确定两个表达式相同，返回 2 表示「无法证明相同，出于安全返回 2」。因此去重只在能确证相同时发生，不确定的会各自成为独立条目。

**上限与错误分支：** 上限取自 `int mxTerm = pParse->db->aLimit[SQLITE_LIMIT_COLUMN];`。`SQLITE_LIMIT_COLUMN` 的硬上限在 `src/sqliteLimit.h` 里是 `#define SQLITE_MAX_COLUMN 2000`（未另行定义时），并且 `src/main.c` 有 `#if SQLITE_MAX_COLUMN>32767 / # error SQLITE_MAX_COLUMN must not exceed 32767` 的编译期检查，`aHardLimit[]` 数组把 `SQLITE_MAX_COLUMN` 挂在 `SQLITE_LIMIT_COLUMN` 槽位上。若找到重复项时 `i>mxTerm`，函数调 `sqlite3ErrorMsg(pParse, "more than %d aggregate terms", mxTerm)` 报错，并把 `i` 夹到 `mxTerm`。

**新建条目的流程**（`i>=pAggInfo->nFunc` 分支）：

1. `i = addAggInfoFunc(pParse->db, pAggInfo)` 追加数组元素。`addAggInfoFunc` 内部调 `sqlite3ArrayAllocate`（`src/build.c`），该函数按 2 的幂次扩容（`n==0` 时取 1，否则取 `2*n`），并把新槽位清零；OOM 时把 `*pIdx` 置 -1 并返回原数组。
2. `pItem->pFunc = sqlite3FindFunction(pParse->db, pExpr->u.zToken, nArg, enc, 0)` 查函数实现，其中 `nArg = pExpr->x.pList ? pExpr->x.pList->nExpr : 0`，`enc = ENC(pParse->db)`（连接的文字编码）。`sqlite3FindFunction` 定义在 `src/callback.c`，先在应用自定义函数哈希表里找，再在内置函数表里找，按 `matchQuality` 打分取最优匹配。
3. **聚合内 ORDER BY 的处理：** 若 `pExpr->pLeft` 非空（即聚合函数带 ORDER BY 子句）且 `pItem->pFunc->funcFlags & SQLITE_FUNC_NEEDCOLL` 为 0（`SQLITE_FUNC_NEEDCOLL` 定义在 `src/sqliteInt.h`，值 `0x0020`，注释 "sqlite3GetFuncCollSeq() might be called"），则分配 `pItem->iOBTab = pParse->nTab++` 作为排序用临时表游标。此时再判两个条件：若 ORDER BY 列表恰好 1 项、聚合实参也恰好 1 项、且两者表达式可确证相同（`sqlite3ExprCompare` 返回 0），则 `pItem->bOBPayload = 0`、`pItem->bOBUnique = ExprHasProperty(pExpr, EP_Distinct)`；否则 `pItem->bOBPayload = 1`。源码注释点明了 NEEDCOLL 这个判据的作用：「The NEEDCOLL test above causes any ORDER BY clause on aggregate min() or max() to be ignored.」
4. `pItem->bUseSubtype = (pItem->pFunc->funcFlags & SQLITE_SUBTYPE)!=0`。`SQLITE_SUBTYPE` 定义在 `src/sqlite.h.in`，值为 `0x000100000`。
5. **DISTINCT 的处理：** 若表达式带 `EP_Distinct` 标记（`src/sqliteInt.h` 中值 `0x000004`，注释 "Aggregate function with DISTINCT keyword"）且 `!pItem->bOBUnique`，则 `pItem->iDistinct = pParse->nTab++` 分配强制去重用的临时表游标；否则 `pItem->iDistinct = -1` 表示不用。

**收尾：** 函数给表达式打上 `EP_NoReduce`（`ExprSetVVAProperty`，`src/sqliteInt.h` 中值 `0x01`，注释 "Cannot EXPRDUP_REDUCE this Expr"），置 `pExpr->iAgg = (i16)i`，置 `pExpr->pAggInfo = pAggInfo`，然后 `return WRC_Prune` —— 不再下潜聚合函数的实参（实参由 `analyzeAggFuncArgs` 单独分析）。

条件不成立时走 `else` 分支，直接 `return WRC_Continue`。

#### 分支二：TK_COLUMN / TK_AGG_COLUMN / TK_IF_NULL_ROW —— 登记用到的源表列

这三个 op 走同一段代码：

```c
case TK_IF_NULL_ROW:
case TK_AGG_COLUMN:
case TK_COLUMN: {
  if( ALWAYS(pSrcList!=0) ){
    SrcItem *pItem = pSrcList->a;
    for(i=0; i<pSrcList->nSrc; i++, pItem++){
      if( pExpr->iTable==pItem->iCursor ){
        findOrCreateAggInfoColumn(pParse, pAggInfo, pExpr);
        break;
      }
    }
  }
  return WRC_Continue;
}
```

即：在 FROM 子句的源表列表里找游标号匹配的那一项，找到就调 `findOrCreateAggInfoColumn` 登记这一列。注意这里返回的是 `WRC_Continue` —— 列引用要继续下潜。

#### 分支三：default —— 表达式索引替换

`default` 分支处理的是「一个既不是列、也不是聚合函数、也不是聚合函数的表达式」。它只在以下四个条件全部成立时继续：

- `pNC->ncFlags & NC_InAggFunc` 非零 —— 当前正在分析聚合函数的实参；
- `pParse->pIdxEpr` 非空 —— 存在正在使用的表达式索引（`Parse.pIdxEpr` 是 `IndexedExpr` 链表，定义在 `src/sqliteInt.h`）；
- `pWalker->walkerDepth==0` —— 不在子查询里。源码注释引用了具体缺陷报告 `bug report 2026-11-07T18:33:11Z` 说明这一限制的由来。

然后遍历 `pParse->pIdxEpr` 链表，对每个索引调 `indexedExprCompare(pIEpr, pExpr, iDataCur)` 比对。两个跳过条件：`iDataCur<0` 直接 `continue`；`pIEpr->bMaybeNullRow` 为真则 `continue`——源码注释说明理由：索引可能建在外连接的 NULL 行上，此时索引列的值未必是 NULL，不能用读索引列来替代原表达式。

命中后构造一个临时的 `Expr tmp`（`op = TK_AGG_COLUMN`，`iTable = pIEpr->iIdxCur`，`iColumn = pIEpr->iIdxCol`），调 `findOrCreateAggInfoColumn(pParse, pAggInfo, &tmp)` 登记，再把 `pAggInfo->aCol[tmp.iAgg].pCExpr = pExpr` 指回原表达式，置 `pExpr->pAggInfo` 与 `pExpr->iAgg`，`return WRC_Prune`。

这一分支里还有若干提前退出条件：`ExprUseYTab(pExpr)` 为假（`src/sqliteInt.h` 中 `ExprUseYTab(E)` 定义为 `(((E)->flags&(EP_WinFunc|EP_Subrtn))==0)`）；表达式带 `EP_SubtArg` 标记且 `sqlite3ExprCanReturnSubtype()` 为真；`pParse->nErr` 非零时 `return WRC_Abort`。

#### 分支四：TK_AGG_COLUMN 的自引用修正

`findOrCreateAggInfoColumn` 的末尾有一段 `fix_up_expr` 逻辑：

```c
fix_up_expr:
  ExprSetVVAProperty(pExpr, EP_NoReduce);
  assert( pExpr->pAggInfo==0 || pExpr->pAggInfo==pAggInfo );
  pExpr->pAggInfo = pAggInfo;
  if( pExpr->op==TK_COLUMN ){
    pExpr->op = TK_AGG_COLUMN;
  }
  assert( k <= SMXV(pExpr->iAgg) );
  pExpr->iAgg = (i16)k;
```

即：把列引用节点的 op 从 `TK_COLUMN` 改写成 `TK_AGG_COLUMN`，让它在后续代码生成阶段能通过 `pExpr->pAggInfo` 与 `pExpr->iAgg` 找到自己对应的累加器寄存器。

### 第四步：findOrCreateAggInfoColumn 登记分组键

`findOrCreateAggInfoColumn(Parse *pParse, AggInfo *pAggInfo, Expr *pExpr)` 是 `src/expr.c` 的静态函数。函数头注释写明：在 `AggInfo` 里找 `iTable` 和 `iColumn` 都匹配的 `aCol[]` 条目并返回其索引；找不到就新建。

查找循环里有两个判据：

- `if( pCol->pCExpr==pExpr ) return;` —— 同一个表达式节点直接复用已有条目。
- `pCol->iTable==pExpr->iTable && pCol->iColumn==pExpr->iColumn && pExpr->op!=TK_IF_NULL_ROW` —— 同表同列视为同一项，跳到 `fix_up_expr`。

新建条目时的错误分支：`k = addAggInfoColumn(pParse->db, pAggInfo)` 返回负数表示 OOM（扩容失败），此时直接返回；`k>mxTerm` 时调 `sqlite3ErrorMsg(pParse, "more than %d aggregate terms", mxTerm)` 报错并把 `k` 夹到 `mxTerm`（`mxTerm` 同样取自 `db->aLimit[SQLITE_LIMIT_COLUMN]`）。

**分组键与排序列的绑定：** 填好条目后，函数在 `pAggInfo->pGroupBy` 里找有没有与本列同表同列的 `TK_COLUMN` 节点，找到则 `pCol->iSorterColumn = j`（`j` 是分组列表里的下标）；找不到则 `pCol->iSorterColumn = pAggInfo->nSortingColumn++`。

**架构推论：** 这一段把「聚合用到的列」和「GROUP BY 里的分组键」在编译期就对齐到同一个排序索引列号上。后续 `src/expr.c` 的代码生成在 `pAggInfo->useSortingIdx` 模式下，直接用 `OP_Column` 从 `pAggInfo->sortingIdxPTab` 伪表的 `pCol->iSorterColumn` 号位置取值，替代从源表读列。

### 第五步：编译期顺序 —— 谁在什么时候调用分析

`src/select.c` 的聚合查询处理路径里的顺序是：

1. 分配 `pAggInfo = sqlite3DbMallocZero(db, sizeof(*pAggInfo))`，并用 `sqlite3ParserAddCleanup(pParse, agginfoFree, pAggInfo)` 登记清理回调。
2. `pAggInfo->selId = p->selId`；`pAggInfo->nSortingColumn = pGroupBy ? pGroupBy->nExpr : 0`；`pAggInfo->pGroupBy = pGroupBy`。
3. `sqlite3ExprAnalyzeAggList(&sNC, pEList)` 分析结果列。
4. `sqlite3ExprAnalyzeAggList(&sNC, sSort.pOrderBy)` 分析 ORDER BY。
5. HAVING 的处理：若 `pGroupBy` 存在且 `OptimizationEnabled(db, SQLITE_HavingToWhere)` 为真，则先调 `havingToWhere(pParse, p)` 把可下放的 HAVING 项搬进 WHERE，再 `sqlite3ExprAnalyzeAggregates(&sNC, pHaving)`。
6. `pAggInfo->nAccumulator = pAggInfo->nColumn` —— 此刻 `aCol[]` 里全部是「聚合函数外部引用的列」，它们要直接输出到结果集。
7. `analyzeAggFuncArgs(pAggInfo, &sNC)` —— 置上 `NC_InAggFunc` 标志后，分析聚合函数实参里的列引用，这些列会被追加到 `aCol[]` 末尾，不属于 `nAccumulator` 范围。

第 6 步的时机很关键：`nAccumulator` 必须在分析聚合实参**之前**取值，因为它划出了「直接透出到输出的列」和「只作为聚合函数参数使用的列」的分界线。`src/select.c` 的注释明确写明了这一分工：前段的 `aCol[]` 条目是被聚合函数外部引用的列（可能包含 GROUP BY 里的列），后段的是仅作为聚合函数参数使用的列。

### 第六步：analyzeAggFuncArgs —— 递归地分析实参

`analyzeAggFuncArgs(AggInfo *pAggInfo, NameContext *pNC)` 是 `src/select.c` 的静态函数，负责「计算（或重新计算）`aCol[]` 中仅作为聚合函数参数使用的那些条目」。

```c
pNC->ncFlags |= NC_InAggFunc;
for(i=0; i<pAggInfo->nFunc; i++){
  Expr *pExpr = pAggInfo->aFunc[i].pFExpr;
  sqlite3ExprAnalyzeAggList(pNC, pExpr->x.pList);
  if( pExpr->pLeft ){
    sqlite3ExprAnalyzeAggList(pNC, pExpr->pLeft->x.pList);
  }
  if( ExprHasProperty(pExpr, EP_WinFunc) ){
    sqlite3ExprAnalyzeAggregates(pNC, pExpr->y.pWin->pFilter);
  }
}
pNC->ncFlags &= ~NC_InAggFunc;
```

**`NC_InAggFunc` 的作用是防止聚合函数自我嵌套。** 有了这个标志，`analyzeAggregate` 的 `TK_AGG_FUNCTION` 分支不会给「正在分析的聚合函数的实参里的聚合函数」建条目——即聚合函数不能嵌套聚合函数。

### 第七步：编译器如何消费分析结果

分析结果通过 `AggInfo` 的两个字段传给代码生成阶段：

- 表达式侧：`pExpr->pAggInfo` 指向所属的 `AggInfo`，`pExpr->iAgg` 是它在 `aCol[]` 或 `aFunc[]` 里的下标。`src/expr.c` 的 `sqlite3ExprCodeTarget` 遇到 `TK_AGG_FUNCTION` 时返回 `AggInfoFuncReg(pInfo, pExpr->iAgg)`；遇到 `TK_AGG_COLUMN` 时按 `pAggInfo->directMode` 与 `pAggInfo->useSortingIdx` 两条路分别返回累加器寄存器或直接从排序索引伪表读列。
- 寄存器分配：`src/select.c` 的 `assignAggregateRegisters(Parse *pParse, AggInfo *pAggInfo)` 置 `pAggInfo->iFirstReg = pParse->nMem + 1`，并令 `pParse->nMem += pAggInfo->nColumn + pAggInfo->nFunc`。`AggInfoColumnReg(A,I)` 与 `AggInfoFuncReg(A,I)` 两个宏（`src/sqliteInt.h`）据此计算寄存器号：列寄存器是 `iFirstReg + I`，函数累加器寄存器是 `iFirstReg + nColumn + I`。
- 累加：`src/select.c` 的 `updateAccumulator` 遍历 `aFunc[]`，为每个聚合函数生成 `OP_AggStep`（参数是 `AggInfoFuncReg(pAggInfo,i)`），结束时生成 `OP_AggFinal`。

### 第八步：AggInfo 的持久化

`src/expr.c` 的 `agginfoPersistExprCb` 是另一个 Walker 回调，处理「`AggInfo` 里存的 `Expr*` 指向的表达式本身可能被改写」的问题。函数头注释写明：如果 `AggInfo` 直接引用了某个 `Expr`，而这个 `Expr` 本身是会被改写的，就要复制一份。

它的做法是：若 `pExpr->pAggInfo!=0` 且（对非 `TK_AGG_FUNCTION` 节点）`pAggInfo->aCol[iAgg].pCExpr==pExpr`，或（对 `TK_AGG_FUNCTION` 节点）`pAggInfo->aFunc[iAgg].pFExpr==pExpr`，则调 `sqlite3ExprDup` 复制表达式，并用 `sqlite3ExprDeferredDelete` 把副本登记为延迟删除；副本登记成功后，把 `AggInfo` 里的指针改指向副本。

## 关键取舍

**取舍一：分析期与执行期用 `AggInfo` 一个结构缝合，而不是在执行期重新识别聚合。** 编译期一次性把「用到哪些列」「有哪些聚合函数」算清楚，执行期只按 `iAgg` 下标取寄存器。**架构推论：** 好处是执行期不需要再判断表达式是不是聚合函数，也不需要在每行上重复查函数表；代价是编译期必须把所有潜在问题（包括上限溢出、函数找不到）都在这一阶段暴露出来，一旦进入执行期就没有补救机会。

**取舍二：聚合函数去重只在能「确证相同」时发生。** `analyzeAggregate` 用 `sqlite3ExprCompare` 去重，而 `sqlite3ExprCompare` 的注释明确写着：返回 2 表示「无法证明两个表达式相同，出于安全就返回 2」。去重判据是 `==0`（确证相同）。**架构推论：** 代价是 `count(a)` 与 `count(b)` 这类无法证明相同的写法会各占一个 `aFunc[]` 条目、各自生成一套累加器；收益是绝不会把两个语义不同的聚合误合并成一个。

**取舍三：分组键通过 `iSorterColumn` 与聚合列对齐，而不是为分组键单独开数据结构。** `findOrCreateAggInfoColumn` 在 `pGroupBy` 里找同表同列的 `TK_COLUMN` 节点，找到就把该列的 `iSorterColumn` 设为它在分组列表里的下标。**架构推论：** 代价是分组键必须是能被 `TK_COLUMN` 精确匹配的列引用，表达式形式的分组键走不通这条路；收益是排序索引的列布局与分组键布局完全一致，后续可以直接从排序索引伪表读出分组键。

**取舍四：列上限复用了 `SQLITE_LIMIT_COLUMN`，与表的列数上限共用一个配额。** `analyzeAggregate` 与 `findOrCreateAggInfoColumn` 都取 `pParse->db->aLimit[SQLITE_LIMIT_COLUMN]` 作为聚合项数上限，超限时调 `sqlite3ErrorMsg(pParse, "more than %d aggregate terms", mxTerm)`。**架构推论：** 这意味着一个 `SQLITE_LIMIT_COLUMN` 配额同时约束「表能有多少列」和「一次查询能有多少个聚合项」，两者共享同一个可调参数 `sqlite3_limit()`。

**取舍五：上限错误在登记时立即报，而不是等到代码生成。** 超限时函数把下标 `i` 夹到 `mxTerm` 后继续（`i = mxTerm; assert( i<pAggInfo->nFunc );`），同时已经通过 `sqlite3ErrorMsg` 置了 `pParse->nErr`。**架构推论：** 报错后仍继续走完当前流程，是为了让后续代码看到的是一个结构自洽的 `AggInfo`，错误由 `pParse->nErr` 统一在阶段边界处截断——这与 `src/select.c` 里大量 `if( pParse->nErr ) goto select_end;` 的写法一致。

**取舍六：聚合函数内部 ORDER BY 借用了临时表游标，且在 NEEDCOLL 函数上直接放弃。** `analyzeAggregate` 里 `pItem->iOBTab = pParse->nTab++` 分配排序临时表；而当 `pFunc->funcFlags & SQLITE_FUNC_NEEDCOLL` 非零时走 `else` 分支，`pItem->iOBTab = -1`。源码注释说明：NEEDCOLL 这个判据会让聚合 `min()` / `max()` 上的 ORDER BY 子句被忽略。**架构推论：** 代价是 `min(x ORDER BY y)` 这类写法在需要 collation 的函数上不按 y 排序；收益是不需要在累加器里额外携带 collation 序列的传递。

**取舍七：DISTINCT 去重表与 ORDER BY 排序表复用同一个 `iDistinct` 游标分配路径。** 带 `EP_Distinct` 且 `!bOBUnique` 时 `pItem->iDistinct = pParse->nTab++`；否则 `pItem->iDistinct = -1`。`src/select.c` 的 `resetAccumulator` 里若 `pFunc->iDistinct>=0` 则检查 `pE->x.pList->nExpr!=1`，不满足就报 "DISTINCT aggregates must have exactly one argument"。

**取舍八：`AggInfo` 的持久化靠复制表达式而非复制整个 `AggInfo`。** `agginfoPersistExprCb` 只在 `AggInfo` 里的指针确实指向那个会被改写的 `Expr` 节点时才复制该节点。**架构推论：** 好处是大多数情况下零拷贝；代价是要维护「`AggInfo` 里的指针不得指向可变节点」这条不变式，任何新增的表达式改写路径都必须考虑 `agginfoPersistExprCb`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/expr.c | `sqlite3ExprAnalyzeAggregates` | 装配 `Walker`（`xExprCallback` 为 `analyzeAggregate`，`xSelectCallback`/`xSelectCallback2` 为 `sqlite3WalkerDepthIncrease`/`sqlite3WalkerDepthDecrease`，`walkerDepth` 初值 0，`w.u.pNC` 挂 `NameContext`，`w.pParse` 置 0），然后调 `sqlite3WalkExpr` |
| src/expr.c | `sqlite3ExprAnalyzeAggList` | 遍历 `ExprList`，对每个元素调 `sqlite3ExprAnalyzeAggregates`；`pList` 为 NULL 时直接跳过 |
| src/expr.c | `analyzeAggregate` | 分析回调本体：按 `pExpr->op` 分派 `TK_AGG_FUNCTION` / `TK_COLUMN`+`TK_AGG_COLUMN`+`TK_IF_NULL_ROW` / `default`（表达式索引替换）三条路径 |
| src/expr.c | `findOrCreateAggInfoColumn` | 在 `aCol[]` 里按 `iTable`+`iColumn` 找或建条目，填 `pTab`/`iTable`/`iColumn`/`pCExpr`，并在 `pGroupBy` 里匹配出 `iSorterColumn` |
| src/expr.c | `addAggInfoColumn` | 通过 `sqlite3ArrayAllocate` 追加 `aCol[]` 元素，返回新下标；OOM 返回负数 |
| src/expr.c | `addAggInfoFunc` | 通过 `sqlite3ArrayAllocate` 追加 `aFunc[]` 元素，返回新下标；OOM 返回负数 |
| src/expr.c | `agginfoPersistExprCb` | 复制会被改写的表达式节点，把 `AggInfo` 里指向它的指针改指向副本，并用 `sqlite3ExprDeferredDelete` 登记延迟删除 |
| src/select.c | `analyzeAggFuncArgs` | 置 `NC_InAggFunc` 后分析每个聚合函数的实参列表、内部 ORDER BY 列表与窗口函数 FILTER 表达式，末尾清标志 |
| src/select.c | `assignAggregateRegisters` | 置 `pAggInfo->iFirstReg = pParse->nMem + 1`，`pParse->nMem += nColumn + nFunc`，定下累加器寄存器区起点 |
| src/select.c | `updateAccumulator` | 消费 `aFunc[]`/`aCol[]` 生成 `OP_AggStep`、`OP_AggFinal` 与排序表插入指令 |
| src/select.c | `resetAccumulator` | 生成 `OP_Null` 清空累加器寄存器区，并为 `iDistinct>=0` 的聚合生成 `OP_OpenEphemeral` |
| src/select.c | `havingToWhere` | 把只由常量与 BINARY collation 的 GROUP BY 项构成的 HAVING 项下放到 WHERE |
| src/resolve.c | `resolveExprStep` | 名字解析阶段把聚合函数的 `op` 改成 `TK_AGG_FUNCTION`、`op2` 置 0 并按嵌套层数累加 `op2` |
| src/walker.c | `sqlite3WalkExprNN` | 前序遍历表达式树：回调返回 `WRC_Prune` 时不再下潜子节点，`WRC_Abort` 时整体中止 |
| src/walker.c | `sqlite3WalkerDepthIncrease` / `sqlite3WalkerDepthDecrease` | 进入子查询时 `walkerDepth++`、离开时 `walkerDepth--`，使 `walkerDepth` 能与 `TK_AGG_FUNCTION` 的 `op2` 对齐 |
| src/build.c | `sqlite3ArrayAllocate` | 按 2 的幂次扩容数组并清零新槽位，OOM 时把新下标置 -1 并返回原数组 |
| src/callback.c | `sqlite3FindFunction` | 按名字与参数个数查 `FuncDef`：先查应用自定义函数哈希表，再查内置函数表，按 `matchQuality` 打分取最优 |
| src/sqliteInt.h | `AggInfo` | 聚合分析结果结构：`aCol[]`、`aFunc[]`、`pGroupBy`、`nSortingColumn`、`nAccumulator`、`iFirstReg` 等成员 |
| src/sqliteInt.h | `AggInfoColumnReg` / `AggInfoFuncReg` | 累加器寄存器号宏：列寄存器为 `iFirstReg + I`，函数累加器为 `iFirstReg + nColumn + I` |
| src/sqliteInt.h | `NC_InAggFunc` / `NC_UAggInfo` / `NC_HasAgg` | 名字上下文的聚合相关标志，取值依次为 `0x020000` / `0x000100` / `0x000010` |
| src/sqliteInt.h | `EP_Distinct` / `EP_NoReduce` / `EP_SubtArg` | 表达式属性位，取值依次为 `0x000004` / `0x01` / `0x80000000` |
| src/sqliteInt.h | `SQLITE_FUNC_NEEDCOLL` / `SQLITE_FUNC_COUNT` / `SQLITE_FUNC_MINMAX` | 函数标志位，取值依次为 `0x0020` / `0x0100` / `0x1000` |
| src/sqlite.h.in | `SQLITE_SUBTYPE` | 函数标志位，值 `0x000100000`，用于决定是否在排序表中传递子类型 |
| src/sqliteLimit.h | `SQLITE_MAX_COLUMN` | 聚合项数与表列数共用的硬上限，未另行定义时为 `2000`；`src/main.c` 另有编译期检查禁止超过 `32767` |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里（`common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`）没有与 SQLite 的 `AggInfo` 对位的结构。SQLite 把聚合分析的全部产物压进一张编译期表（`aCol[]` + `aFunc[]`），执行期只按下标取寄存器。主核若要对齐这一接口，需要一个等价的编译期结构，把「分组键」「聚合函数」「聚合实参列」「累加器寄存器映射」四类信息一起交给执行器，而不是让执行器在运行时重新识别聚合。

**架构推论：** SQLite 用 `Expr.op2` 承载「聚合函数所属的查询嵌套深度」，用 `Walker.walkerDepth` 配合它来判断聚合归属。这套机制让同一份表达式树能被多层 SELECT 共享而不错配聚合。但它把语义塞进了 `op2` 这个复用字段——`op2` 在别处还承担 `TK_IS`/`TK_ISNOT` 区分、`OPFLAG_BYTELENARG` 标记等完全不同的含义。主核若采用类似做法，需要为「聚合归属深度」单开一个字段，否则任何复用 `op2` 的新逻辑都会与聚合归属判断相互干扰。

**架构推论：** 对 AI 生成 SQL 的场景，SQLite 的行为边界值得注意：SQLite 允许引用既不在 GROUP BY 里、也不在聚合函数参数里的列（`src/select.c` 的注释明确写道「Other database engines would throw an error if there is a column reference that is not in the GROUP BY clause and that is not part of an aggregate function argument. But SQLite allows this.」）。AI 生成的 SQL 若按标准 SQL 严格性假设去写「GROUP BY 覆盖所有非聚合列」，在 SQLite 上并不会得到报错，只会得到一个取任意一行值的语义。同一段注释还说明 `aCol[]` 里 `nAccumulator` 之前与之后的条目分工不同——这直接对应 SQLite 在聚合查询中「哪些列直接透出、哪些列只作为参数」的判定。

**架构推论：** 聚合项数上限复用 `SQLITE_LIMIT_COLUMN`（默认 `SQLITE_MAX_COLUMN` 即 `2000`），而不是单设一个「聚合项数」配额。这意味着调低列上限会连带收紧聚合查询的可用项数。对主核的启示是：把不同维度的资源配额合并到同一个旋钮上简化了配置面，但也让「列多」和「聚合项多」这两类问题的诊断互相干扰。

## 参考

- [SQLite SELECT 官方文档（GROUP BY / HAVING 语义）](https://sqlite.org/lang_select.html)
- [SQLite 内置聚合函数官方文档](https://sqlite.org/lang_aggfunc.html)
- [SQLite Query Planner 总览（聚合与分组在查询规划中的位置）](https://sqlite.org/optoverview.html)
- [SQLite 架构说明（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层）](https://sqlite.org/arch.html)
- [SQLite 官方文档总入口](https://sqlite.org/docs.html)
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c)
- [sqlite/sqlite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c)
- [sqlite/sqlite 源码：src/resolve.c](https://github.com/sqlite/sqlite/blob/master/src/resolve.c)
- [sqlite/sqlite 源码：src/walker.c](https://github.com/sqlite/sqlite/blob/master/src/walker.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
