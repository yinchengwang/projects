---
project: sqlite
feature: common-table-expression
page_type: feature
schema_version: 1
refs:
  - path: src/build.c
    symbols: [sqlite3CteDelete]
  - path: src/select.c
    symbols: [sqlite3ExpandSubquery, sqlite3SelectExpand]
---

# 公用表表达式

## 一句话职责

公用表表达式（CTE）负责把 `WITH` 子句里声明的临时命名空间落到可执行形态：解析期建出 `Cte`/`With` 对象，名称解析期把 FROM 项匹配到 CTE 并造出临时 `Table`，代码生成期再决定这个 CTE 是走协程、就地物化成临时表、还是共享一份物化结果。

## 机制是什么

边界（依据条目 `boundary`）：负责 WITH 子句定义的临时命名空间、递归 CTE 的展开方式以及 CTE 表的生命周期；不负责子查询的一般表达式路径（C04）、不负责物化执行（D 组）。

### 三个数据结构

CTE 的全部状态装在三个结构里，定义都在 `src/sqliteInt.h`：

| 结构 | 关键成员 | 作用 |
|---|---|---|
| `Cte` | `zName` / `pCols` / `pSelect` / `zCteErr` / `pUse` / `eM10d` | 单个 CTE 的定义：`pSelect` 是它的查询体，`pCols` 是可选的显式列名表，`zCteErr` 暂存循环引用错误串，`pUse` 挂使用信息，`eM10d` 是物化标志 |
| `With` | `nCte` / `bView` / `pOuter` / `a[]` | 一个 `WITH` 子句：数组式存 `nCte` 个 `Cte`，`pOuter` 指向外层 `WITH` 构成命名空间栈，`bView` 标记该 WITH 属于视图的最外层 SELECT |
| `CteUse` | `nUse` / `addrM9e` / `regRtn` / `iCur` / `nRowEst` / `eM10d` | CTE 的使用态：被引用次数、物化子程序地址、返回地址寄存器、物化临时表游标、行数估计 |

`With` 的容量用 `SZ_WITH(N)` 宏算，定义在 `src/sqliteInt.h`：`SZ_WITH(N)` 为 `offsetof(With,a) + (N)*sizeof(Cte)`，即 `With` 头加上 N 个 `Cte` 槽位。`With.a` 是 `FLEXARRAY`，所以 `With` 对象本身按需扩容。

物化标志 `eM10d` 的三个取值也定义在 `src/sqliteInt.h`：`M10d_Yes` 为 `0`（`AS MATERIALIZED`）、`M10d_Any` 为 `1`（未指定，由查询规划器决定）、`M10d_No` 为 `2`（`AS NOT MATERIALIZED`）。

### 命名空间是栈，不是全局表

`With.pOuter` 构成一条链，链头保存在 `Parse.pWith` 上。名字查找沿这条链从内往外走，所以内层 `WITH` 遮蔽外层同名 CTE。`searchWith()`（`src/select.c` 静态函数）在链上逐层遍历每层的 `a[i]`，用 `sqlite3StrICmp` 做大小写不敏感比较，命中即返回该 `Cte` 并通过出参回填匹配的 `With` 层；一旦某层 `bView` 为真就停止外扩——视图最外层 SELECT 的 `WITH` 不该看到调用方的 CTE。

### 生命周期分三段

1. **解析期建对象**：`src/parse.y` 的 `wqitem` 产生式调 `sqlite3CteNew()` 造出 `Cte`，`wqlist` 产生式调 `sqlite3WithAdd()` 把 `Cte` 挂进 `With` 数组；`with ::= WITH wqlist` 与 `with ::= WITH RECURSIVE wqlist` 都调 `sqlite3WithPush(pParse, W, 1)` 把整个 `With` 压到 `Parse.pWith` 栈上。
2. **名称解析期建临时表**：`resolveFromTermToCte()`（`src/select.c` 静态函数）匹配到 CTE 后，为它造一个 `Table`，并用 `sqlite3SrcItemAttachSubquery()` 挂上一份 `Select` 副本。
3. **代码生成期定形态**：`sqlite3Select()` 里遍历 FROM 子句，决定走协程、共享物化、还是就地物化。

## 怎么实现的

### 解析期：sqlite3CteNew 与 sqlite3WithAdd

`sqlite3CteNew(Parse *pParse, Token *pName, ExprList *pArglist, Select *pQuery, u8 eM10d)` 定义在 `src/build.c`，位于 `#ifndef SQLITE_OMIT_CTE` 之内。它用 `sqlite3DbMallocZero()` 分配 `Cte`，然后填四个字段：`pSelect = pQuery`、`pCols = pArglist`、`zName = sqlite3NameFromToken()`、`eM10d = eM10d`。若 `db->mallocFailed` 成立，则改为释放传入的 `pArglist`（`sqlite3ExprListDelete`）与 `pQuery`（`sqlite3SelectDelete`）——即入参所有权在失败路径上转移给调用方清理。

`sqlite3WithAdd(Parse *pParse, With *pWith, Cte *pCte)` 同样在 `src/build.c`。它先做重名检查：若 `zName` 非空且已有 `pWith`，就遍历 `pWith->a[0..nCte)`，用 `sqlite3StrICmp` 比较，命中即 `sqlite3ErrorMsg(pParse, "duplicate WITH table name: %s", zName)`。随后按 `pWith` 是否为空决定 `sqlite3DbRealloc(db, pWith, SZ_WITH(pWith->nCte+1))` 还是 `sqlite3DbMallocZero(db, SZ_WITH(1))` 扩容。

**降级路径：** `sqlite3WithAdd()` 在 `db->mallocFailed` 成立时调 `sqlite3CteDelete(db, pCte)` 释放这个还没挂进数组的 `Cte`，并把 `pNew` 回退成原来的 `pWith`（`pNew = pWith`）——也就是这次 CTE 被整个丢弃，WITH 子句维持扩容前的样子，错误由 `db->mallocFailed` 承载向上传播。

`sqlite3CteDelete(sqlite3 *db, Cte *pCte)` 定义在 `src/build.c`，函数体只有两步：先调 `cteClear(db, pCte)` 清内容，再调 `sqlite3DbFree(db, pCte)` 释放对象本身。`cteClear()` 是同文件的静态函数，清三样：`sqlite3ExprListDelete(db, pCte->pCols)`、`sqlite3SelectDelete(db, pCte->pSelect)`、`sqlite3DbFree(db, pCte->zName)`。

### 释放路径：sqlite3WithDelete 与 clearSelect

`sqlite3WithDelete(sqlite3 *db, With *pWith)` 在 `src/build.c`，遍历 `pWith->a[0..nCte)` 对每个元素调 `cteClear(db, &pWith->a[i])`，然后 `sqlite3DbFree(db, pWith)`。注意它调的是 `cteClear` 而不是 `sqlite3CteDelete`——因为 `a[]` 是 `With` 内嵌的数组，不是单独分配的 `Cte`，不能再单独 free 一次。

`sqlite3WithDeleteGeneric(sqlite3 *db, void *pWith)` 是 `src/build.c` 里的薄包装，把 `void*` 强转成 `With*` 后调 `sqlite3WithDelete`。它的存在是为了配合 `sqlite3ParserAddCleanup()`——清理回调的签名是 `void (*)(void*,void*)`。

`src/select.c` 的 `clearSelect(sqlite3 *db, Select *p, int bFree)` 在遍历 `Select` 链时对每个 `p->pWith` 调 `sqlite3WithDelete(db, p->pWith)`，这是 SELECT 语法树销毁时 WITH 子句的主要释放点。

`src/parse.y` 里另有两处解析器析构规则：`%destructor wqlist {sqlite3WithDelete(pParse->db, $$);}` 负责解析中途出错时回收 `With`；而 `%destructor wqitem {sqlite3CteDelete(pParse->db, $$);}` 这条被注释掉了，注释写明 `// not reachable`——`wqitem` 产生式总是把 `Cte` 交给 `sqlite3WithAdd`，后者会接管所有权。

### 解析期的另一条栈操作：sqlite3WithPush

`sqlite3WithPush(Parse *pParse, With *pWith, u8 bFree)` 在 `src/select.c`。若 `bFree` 为真，它先调 `sqlite3ParserAddCleanup(pParse, sqlite3WithDeleteGeneric, pWith)` 把这个 `With` 挂到解析器的清理链上（`With` 对象此时被转移，后续不能再直接用原指针，只能用返回值）；然后在 `pParse->nErr==0` 时做 `pWith->pOuter = pParse->pWith; pParse->pWith = pWith;` 完成压栈。

### 名称解析期：resolveFromTermToCte

`resolveFromTermToCte(Parse *pParse, Walker *pWalker, SrcItem *pFrom)` 是 `src/select.c` 的静态函数，返回值约定写在函数头注释里：`0` 表示无匹配，`1` 表示匹配成功，`2` 表示检出错误条件。

它先做四道前置拒绝：

- `pParse->pWith==0` → 返回 0（栈上没有 WITH 子句，不可能匹配）。
- `pParse->nErr` 非零 → 返回 0（注释说明先前错误可能让 `pParse->pWith` 处于怪状态，不再往下走）。
- FROM 项带 schema 限定且 `fg.fixedSchema==0 && u4.zDatabase!=0` → 返回 0（注释写明：这种项不可能是 CTE 引用）。
- `pFrom->fg.notCte` 为真 → 返回 0。注释列出两种情形：其一是触发器里原本带 `zDatabase`、后被 `sqlite3FixTriggerStep()` 去掉；其二是 `UPDATE` 语句 FROM 子句的第一项。

匹配用 `searchWith(pParse->pWith, pFrom, &pWith)`。命中后进入主流程：

**错误分支一（循环引用）：** 函数体开头检查 `pCte->zCteErr`，非空则 `sqlite3ErrorMsg(pParse, pCte->zCteErr, pCte->zName)` 并返回 2。`zCteErr` 的取值在函数末尾被写入三处：递归分支下若 `pSel->selFlags & SF_Recursive` 则置 `"multiple recursive references: %s"`，否则置 `"recursive reference in a subquery: %s"`；函数正常收尾时置回 `0`。这意味着 `zCteErr` 是「解析期间临时存放错误消息模板」的一次性槽位。

**错误分支二（索引提示）：** `pFrom->fg.isIndexedBy` 为真时 `sqlite3ErrorMsg(pParse, "no such index: \"%s\"", pFrom->u1.zIndexedBy)` 并返回 2。CTE 不能带 `INDEXED BY`。

**错误分支三（多重递归引用）：** 递归项扫描时若 `pRecTerm->selFlags & SF_Recursive` 已置位，说明同一个递归项里第二次出现 CTE 自身名字，`sqlite3ErrorMsg(pParse, "multiple references to recursive table: %s", pCte->zName)` 并返回 2。

**错误分支四（CTE 数量超限）：** `if( pParse->nTab >= db->aLimit[SQLITE_LIMIT_CTES] && db->aLimit[SQLITE_LIMIT_CTES]>0 )` 则 `sqlite3ErrorMsg(pParse, "too many CTEs")` 并返回 2。`SQLITE_LIMIT_CTES` 的编号为 `15`，定义在 `src/sqlite.h.in`；`SQLITE_N_LIMIT` 在 `src/sqliteInt.h` 定义为 `(SQLITE_LIMIT_CTES+1)`。硬上限 `SQLITE_MAX_CTES` 在 `src/sqliteLimit.h` 定义为 `50000`；`src/main.c` 里有断言 `aHardLimit[SQLITE_LIMIT_CTES]==SQLITE_MAX_CTES`。

**错误分支五（列数不匹配）：** 若 `pCte->pCols` 非空，而左侧最外层 SELECT 的结果列数 `pEList->nExpr` 与 `pCte->pCols->nExpr` 不等，则 `sqlite3ErrorMsg(pParse, "table %s has %d values for %d columns", pCte->zName, pEList->nExpr, pCte->pCols->nExpr)` 并返回 2。

**构造临时表：** 匹配成功后 `sqlite3DbMallocZero(db, sizeof(Table))` 造 `pTab`，置 `pTab->nTabRef = 1`、`pTab->zName = sqlite3DbStrDup(db, pCte->zName)`、`pTab->iPKey = -1`、`pTab->nRowLogEst = 200`、`pTab->tabFlags |= TF_Ephemeral | TF_NoVisibleRowid`。行数估计 `200` 来自断言 `assert( 200==sqlite3LogEst(1048576) )`，即按 1048576 行取对数估计。

**使用态记账：** `pCte->pUse` 为空时用 `sqlite3DbMallocZero(db, sizeof(pCteUse[0]))` 造一个，并用 `sqlite3ParserAddCleanup(pParse, sqlite3DbFree, pCteUse)` 挂到解析器清理链上。`src/sqliteInt.h` 的注释解释了原因：`Cte` 对象不保证活到代码生成结束（flattening 或其它解析树编辑可能删掉它），而 `CteUse` 必须活到代码生成结束，所以用 `sqlite3ParserAddCleanup()` 而不是 `sqlite3SelectDelete()` 来释放。随后 `pCteUse->eM10d = pCte->eM10d`、`pFrom->u2.pCteUse = pCteUse`、`pCteUse->nUse++`。

**挂载查询体：** `sqlite3SrcItemAttachSubquery(pParse, pFrom, pCte->pSelect, 1)` 最后一个参数为 `1`，表示**复制**一份 `Select`。`sqlite3SrcItemAttachSubquery()` 在 `src/build.c`，`dupSelect` 为真时调 `sqlite3SelectDup(pParse->db, pSelect, 0)` 造副本，副本失败则返回 0（OOM）。挂上后 `pSel->selFlags |= SF_CopyCte` 被置位，`pFrom->fg.isCte = 1`。

`SF_CopyCte` 这个标志有一个明确的用途：`src/select.c` 里一个把 `EXISTS` 转成 join 的改写函数在检查 `pSub->selFlags & SF_CopyCte` 时直接 `return 0`，注释写 `/* Not a CTE */`——即被复制的 CTE 不能参与该改写。

### 递归 CTE 的识别与展开

递归判定在 `resolveFromTermToCte()` 的主流程里：`pRecTerm = pSel`；`bMayRecursive = ( pSel->op==TK_ALL || pSel->op==TK_UNION )`；随后 `while( bMayRecursive && pRecTerm->op==pSel->op )` 沿 `pPrior` 链向后扫每一个复合项，在每项的 FROM 子句里找「名字与 CTE 同名、且不是子查询、且没有 schema 限定」的项：

```c
if( pItem->zName!=0
 && !pItem->fg.hadSchema
 && ALWAYS( !pItem->fg.isSubquery )
 && (pItem->fg.fixedSchema || pItem->u4.zDatabase==0)
 && 0==sqlite3StrICmp(pItem->zName, pCte->zName)
)
```

命中后 `pItem->pSTab = pTab`、`pTab->nTabRef++`、`pItem->fg.isRecursive = 1`，并给该项单独分配一个游标 `iRecTab`（`if( iRecTab<0 ) iRecTab = pParse->nTab++;` 后 `pItem->iCursor = iRecTab;`）。

**递归与非递归的代码生成走两条不同的路：** 若 `pSel->selFlags & SF_Recursive` 成立（是递归 CTE），代码把递归项的 `pWith` 临时设为 `pSel->pWith`，调 `sqlite3WalkSelect(pWalker, pRecTerm)` 走一遍，结束后把 `pRecTerm->pWith` 复位为 0；否则调 `selectExpanderWalk(pWalker, pSel)`，让普通 CTE 的查询体照常展开。

### 递归 CTE 的执行：generateWithRecursiveQuery

`generateWithRecursiveQuery(Parse *pParse, Select *p, SelectDest *pDest)` 是 `src/select.c` 的静态函数，函数头注释完整描述了算法。它处理的形态是 `<递归表> AS (<setup查询> UNION [ALL] <递归查询>)`，其中 `p->pPrior` 是 setup 查询、`p` 是递归查询。

算法细节：

- **禁止窗口函数：** 函数开头 `if( p->pWin )` 则 `sqlite3ErrorMsg(pParse, "cannot use window functions in recursive queries")` 并返回。
- **授权：** `sqlite3AuthCheck(pParse, SQLITE_RECURSIVE, 0, 0, 0)` 失败则返回。
- **LIMIT/OFFSET：** 调 `computeLimitRegisters(pParse, p, addrBreak)` 算出限长寄存器，然后临时把 `p->pLimit` 与 `p->iLimit`/`p->iOffset` 清零，把 `p->pOrderBy` 也暂存。
- **行数估计：** `p->nSelectRow = 320;` 注释写 `/* 4 billion rows */`。
- **游标分配：** 递归表游标从 FROM 子句里带 `fg.isRecursive` 标记的项取 `iCursor`，即 `iCurrent`；队列表游标 `iQueue = pParse->nTab++`。若 `p->op==TK_UNION`（去重），再分配 `iDistinct = pParse->nTab++`，且注释写明 Distinct 表的游标号必须恰好比 Queue 大 1，`SRT_DistFifo` 与 `SRT_DistQueue` 目标才正确。
- **队列目标类型：** 有 ORDER BY 时用 `SRT_DistQueue`/`SRT_Queue`，无 ORDER BY 时用 `SRT_DistFifo`/`SRT_Fifo`。
- **临时表打开：** 递归表用 `OP_OpenPseudo`（`sqlite3VdbeAddOp3(v, OP_OpenPseudo, iCurrent, regCurrent, nCol)`）；队列表有 ORDER BY 时用 `OP_OpenEphemeral` 并带 `P4_KEYINFO`（列数 `pOrderBy->nExpr+2`），否则用 `OP_OpenEphemeral` 且列数为 `nCol`；去重表用 `OP_OpenEphemeral` 带 `P4_KEYINFO`。
- **禁止聚合：** 沿 `pPrior` 链扫到最左项，若任一项 `selFlags & SF_Aggregate` 则 `sqlite3ErrorMsg(pParse, "recursive aggregate queries not supported")` 并跳到 `end_of_recursive_query`。
- **主循环：** `OP_Rewind iQueue` 到 `addrBreak`；从队列取一行（有序时用 `OP_Column iQueue pOrderBy->nExpr+1`，无序时用 `OP_RowData iQueue`）放进 `regCurrent`；`OP_Delete iQueue` 把该行从队列取出；`OP_Stale iCurrent` 重置列缓存；然后 `selectInnerLoop(pParse, p, iCurrent, 0, 0, pDest, addrCont, addrBreak)` 输出这一行，`OP_DecrJumpZero regLimit addrBreak` 做 LIMIT 计数；再 `sqlite3Select(pParse, p, &destQueue)` 跑递归部分，结果写回队列；最后 `sqlite3VdbeGoto(v, addrTop)` 回到循环顶部，直到队列空。

### 展开阶段：sqlite3SelectExpand 与 selectExpander

`sqlite3SelectExpand(Parse *pParse, Select *pSelect)` 是 `src/select.c` 的静态函数，函数体很短：造一个 `Walker w`，置 `w.xExprCallback = sqlite3ExprWalkNoop`、`w.pParse = pParse`、`w.eCode = 0`，然后调 `selectExpanderWalk(&w, pSelect)`。

`selectExpanderWalk(Walker *pWalker, Select *pSelect)` 先看 `pWalker->pParse->hasCompound`：若为真，把 `pWalker->xSelectCallback` 临时换成 `convertCompoundSelectToSubquery`、`xSelectCallback2` 置 0，先走一遍 `sqlite3WalkSelect()`——函数头注释说明这一步是「把带 COLLATE 子句的 ORDER BY 表达式的复合 SELECT 转成子查询」。之后把 `xSelectCallback` 设回 `selectExpander`、`xSelectCallback2` 设为 `sqlite3SelectPopWith`，再 `sqlite3WalkSelect(pWalker, pSelect)`。

`sqlite3SelectPopWith(Walker *pWalker, Select *p)` 是 `src/select.c` 导出的函数，作用是把 `Parse.pWith` 栈弹回上一层：`pParse->pWith = pWith->pOuter`。它只在 `p->pPrior==0`（复合 SELECT 的最右项）时执行，取的是 `findRightmost(p)->pWith`。

`selectExpander(Walker *pWalker, Select *p)` 是 `src/select.c` 的静态函数，函数头注释列出「展开」的四件事：给 FROM 子句每一项分配 VDBE 游标号；填充 `SrcList` 各项的 `pTab`（视图则填 `pSelect` 的副本）；把 NATURAL / ON / USING 折进 WHERE 子句；把结果列里的 `*` 与 `TABLE.*` 展开成实际列。

CTE 相关的部分在它遍历 FROM 子句的循环里。循环对每个 `pFrom`：

```c
if( pFrom->pSTab ) continue;        /* 已有 pSTab，跳过 */
if( pFrom->zName==0 ){
    /* FROM 项是子查询 */
    ...
    if( sqlite3WalkSelect(pWalker, pSel) ) return WRC_Abort;
    if( sqlite3ExpandSubquery(pParse, pFrom) ) return WRC_Abort;
}else if( (rc = resolveFromTermToCte(pParse, pWalker, pFrom))!=0 ){
    if( rc>1 ) return WRC_Abort;
    pTab = pFrom->pSTab;
}else{
    /* 普通表或视图名 */
    pFrom->pSTab = pTab = sqlite3LocateTableItem(pParse, 0, pFrom);
    ...
}
```

即：先查 CTE（`resolveFromTermToCte`），匹配不到才按普通表处理（`sqlite3LocateTableItem`）。CTE 匹配返回 `1` 时继续（`pTab` 已由 `resolveFromTermToCte` 填好），返回 `2` 时 `WRC_Abort` 中止整个展开。

`selectExpander` 还有一处与 CTE 栈相关：若 `pParse->pWith` 非空且 `p->selFlags & SF_View`，则把 `p->pWith->bView = 1`——这个标志会截断 `searchWith()` 的外层搜索，防止视图引用到调用方的 CTE。随后 `sqlite3WithPush(pParse, p->pWith, 0)` 把当前 `With` 压栈。

### 展开阶段：sqlite3ExpandSubquery

`sqlite3ExpandSubquery(Parse *pParse, SrcItem *pFrom)` 定义在 `src/select.c`。它只处理「FROM 项是子查询」这一路（断言 `pFrom->fg.isSubquery` 且 `pFrom->u4.pSubq!=0`），给子查询造一个临时的 `Table`：

- `pTab = sqlite3DbMallocZero(pParse->db, sizeof(Table))`，为空则返回 `SQLITE_NOMEM`。
- `pTab->nTabRef = 1`。
- 名字：FROM 项有别名就用 `sqlite3DbStrDup(pParse->db, pFrom->zAlias)`，否则用 `sqlite3MPrintf(pParse->db, "%!S", pFrom)` 生成。
- 列定义：`while( pSel->pPrior ){ pSel = pSel->pPrior; }` 走到复合 SELECT 的最左项，再调 `sqlite3ColumnsFromExprList(pParse, pSel->pEList, &pTab->nCol, &pTab->aCol)`。
- `pTab->iPKey = -1`、`pTab->eTabType = TABTYP_VIEW`。
- `pTab->nRowLogEst = 200; assert( 200==sqlite3LogEst(1048576) );` —— 与 CTE 的临时表用同一个行数估计。
- 表标志：`#ifndef SQLITE_ALLOW_ROWID_IN_VIEW` 分支下置 `TF_Ephemeral | TF_NoVisibleRowid`（注释写「The usual case - do not allow ROWID on a subquery」）；否则走兼容模式置 `TF_Ephemeral | sqlite3Config.mNoVisibleRowid`。
- 返回 `pParse->nErr ? SQLITE_ERROR : SQLITE_OK`。

### 代码生成期：三种形态

形态的判定在 `src/select.c` 的 `sqlite3Select()` 里遍历 FROM 子句的部分，分支顺序固定：

**分支一：协程。** 条件是 `fromClauseTermCanBeCoroutine(pParse, pTabList, i, p->selFlags)` 为真。该函数是 `src/select.c` 的静态函数，函数头注释列出五项条件。它对 CTE 有两条专门判定：

```c
if( pItem->fg.isCte ){
    const CteUse *pCteUse = pItem->u2.pCteUse;
    if( pCteUse->eM10d==M10d_Yes ) return 0;                          /* (2a) */
    if( pCteUse->nUse>=2 && pCteUse->eM10d!=M10d_No ) return 0;       /* (2b) */
}
```

即：`AS MATERIALIZED` 的 CTE 不能走协程；被引用两次及以上（`pCteUse->nUse>=2`）且没有显式 `AS NOT MATERIALIZED` 的 CTE 也不能走协程。

协程分支生成 `OP_InitCoroutine`、`sqlite3SelectDestInit(&dest, SRT_Coroutine, pSubq->regReturn)`、`sqlite3Select(pParse, pSub, &dest)`，末尾 `sqlite3VdbeEndCoroutine(v, pSubq->regReturn)`，并在 EXPLAIN 里打 `"CO-ROUTINE %!S"`。

**分支二：共享物化。** 条件是 `pItem->fg.isCte && pItem->u2.pCteUse->addrM9e>0`——即该 CTE 的物化子程序代码此前已经生成过。此时生成：

```c
int addrOnce = sqlite3VdbeAddOp0(v, OP_Once); VdbeCoverage(v);
sqlite3VdbeAddOp2(v, OP_Gosub, pCteUse->regRtn, pCteUse->addrM9e);
if( pItem->iCursor!=pCteUse->iCur ){
    sqlite3VdbeAddOp2(v, OP_OpenDup, pItem->iCursor, pCteUse->iCur);
}
sqlite3VdbeJumpHere(v, addrOnce);
if( pItem->iCursor!=pCteUse->iCur ){
    sqlite3VdbeAddOp1(v, OP_NullRow, pItem->iCursor);
}
pSub->nSelectRow = pCteUse->nRowEst;
```

注释写明：物化结果在语句或触发器程序每次运行只算一次，之后不变，所以 `OP_Gosub` 子程序调用与 `OP_OpenDup` 只在首次经过时执行，而 `OP_NullRow` 每次都执行（作用是让重复游标不定位在任何行上，与 `OP_OpenDup` 后的状态一致）。

**分支三：就地物化。** 兜底分支，注释写「Materialize the view. If the view is not correlated, generate a subroutine to do the materialization so that subsequent uses of the same view can reuse the materialization.」。它生成 `OP_Goto` 跳过物化代码、记 `pSubq->addrFillSub = topAddr+1`、`pItem->fg.isMaterialized = 1`；若 `pItem->fg.isCorrelated==0` 再用 `OP_Once` 包一层；然后 `sqlite3SelectDestInit(&dest, SRT_EphemTab, pItem->iCursor)`、`sqlite3Select(pParse, pSub, &dest)`，末尾 `OP_Return`。

**共享物化的记账点：** 就地物化分支的末尾有一段专门的条件：

```c
if( pItem->fg.isCte && pItem->fg.isCorrelated==0 ){
    CteUse *pCteUse = pItem->u2.pCteUse;
    pCteUse->addrM9e = pSubq->addrFillSub;
    pCteUse->regRtn = pSubq->regReturn;
    pCteUse->iCur = pItem->iCursor;
    pCteUse->nRowEst = pSub->nSelectRow;
}
```

即只有非相关的 CTE 才把物化子程序地址记回 `CteUse`，后面的引用才能通过分支二复用。

**物化是优化栅栏：** `src/select.c` 里在尝试 flatten 子查询之前有一段：

```c
if( pItem->fg.isCte && pItem->u2.pCteUse->eM10d==M10d_Yes ){
    continue;
}
```

注释写明「Do not attempt the usual optimizations (flattening and ORDER BY elimination) on a MATERIALIZED common table expression because a MATERIALIZED common table expression is an optimization fence.」

**谓词下推也受物化标志约束：** 谓词下推的条件是 `OptimizationEnabled(db, SQLITE_PushDown) && (pItem->fg.isCte==0 || (pItem->u2.pCteUse->eM10d!=M10d_Yes && pItem->u2.pCteUse->nUse<2)) && pushDownWhereTerms(...)`。

**CTE 与 ORDER BY 的交互：** `src/select.c` 里判断能否删掉子查询 ORDER BY 的条件里含 `&& (pSub->selFlags & (SF_OrderByReqd|SF_Recursive))==0`，注释第 (6) 条写明「The subquery is not a recursive CTE. ORDER BY has a different meaning for recursive CTEs and this optimization does not apply.」

## 关键取舍

**取舍一：CTE 是命名空间里的临时对象，不是持久化的表。** CTE 在 `src/select.c` 里被建成 `Table` 但带 `TF_Ephemeral | TF_NoVisibleRowid` 标志，`pTab->iPKey = -1`，没有独立的存储。代价是 CTE 不能有索引，`resolveFromTermToCte()` 里 `pFrom->fg.isIndexedBy` 为真时直接报「no such index」。收益是 CTE 的定义随语句走，语句结束即消失，不需要任何落盘或失效处理。

**取舍二：CTE 的查询体被复制而不是共享。** `sqlite3SrcItemAttachSubquery(pParse, pFrom, pCte->pSelect, 1)` 的最后一个参数为 `1`，走 `sqlite3SelectDup()` 造副本。`src/build.c` 的注释说明 `dupSelect` 为真时 SrcItem 负责删除副本，为假时所有权转移给 SrcItem。**架构推论：** 复制让每个 CTE 引用点可以独立做 flatten / 游标重编号等解析树改写，不必担心改到共享节点；代价是同一个 CTE 被引用 N 次就要复制 N 份语法树，内存开销与引用次数成正比。

**取舍三：`Cte` 与 `CteUse` 分离，专门应对「解析树先于代码生成被销毁」。** `src/sqliteInt.h` 的注释明确写：「The Cte object is not guaranteed to persist for the entire duration of code generation. (The query flattener or other parser tree edits might delete it.)」，因此 `CteUse` 用 `sqlite3ParserAddCleanup()` 释放而不是 `sqlite3SelectDelete()`——这样它能活到代码生成结束。`resolveFromTermToCte()` 里 `pCte->pUse` 只在第一次匹配时分配，后续引用复用同一个 `CteUse`，`pCteUse->nUse++` 累加计数。这个计数直接决定了两件事：是否禁止走协程（`nUse>=2`）、是否禁止谓词下推（`nUse<2`）。

**取舍四：循环引用检测靠「先置错误串、再遍历」的时序技巧。** `resolveFromTermToCte()` 在展开 CTE 查询体之前先把 `pCte->zCteErr` 置成错误消息模板，展开完再置回 0。这样一旦 CTE 查询体内部再次引用自己，`resolveFromTermToCte()` 开头那句 `if( pCte->zCteErr )` 就会命中并报错。错误消息模板有三档，分别对应「递归 CTE 里的多重递归引用」「子查询里的递归引用」「非递归 CTE 的纯循环引用」，由 `pSel->selFlags & SF_Recursive` 和 `bMayRecursive` 两个条件组合决定。

**取舍五：递归 CTE 用「Queue 表 + Current 表」双临时表实现，而不是真正的迭代器。** `generateWithRecursiveQuery()` 的注释描述了这个模型：setup 查询产出的行进 Queue 表；每次从 Queue 取一行放到 Current 表；Current 表的内容作为递归表喂给递归查询；递归查询的输出写回 Queue；Queue 空则终止。这个模型的一个直接后果是递归项必须是 UNION 或 UNION ALL（`bMayRecursive = ( pSel->op==TK_ALL || pSel->op==TK_UNION )`），否则无法递归。UNION 的去重靠额外一张 Distinct 表实现，其游标号必须恰好是 Queue 游标号加一。

**取舍六：`AS MATERIALIZED` 是显式的优化栅栏，`AS NOT MATERIALIZED` 是显式的解禁。** 三态 `eM10d`（`M10d_Yes`/`M10d_Any`/`M10d_No`）的判定散在三处：`fromClauseTermCanBeCoroutine()` 里 `M10d_Yes` 直接禁止协程、`nUse>=2` 且非 `M10d_No` 也禁止协程；flatten 前 `M10d_Yes` 直接 `continue`；谓词下推条件里 `M10d_Yes` 或 `nUse>=2` 都禁止。**架构推论：** 物化决策不是集中在一处算出来的，而是「若干处各自读取同一个 `eM10d` 标志做局部否决」的分布式设计；好处是每处优化都能独立判断，代价是要理解整体行为必须把所有读点串起来看，新增优化时容易漏掉其中一处。

**取舍七：`sqlite3WithAdd()` 的重名检查只报同名错误，不阻止加入。** 函数体里 `sqlite3ErrorMsg(pParse, "duplicate WITH table name: %s", zName)` 之后代码继续往下走，仍把 `pCte` 追加进 `a[]`。错误通过 `pParse->nErr` 承载，后续阶段靠 `pParse->nErr` 非零提前返回。**架构推论：** 这与 `resolveFromTermToCte()` 里多处「检测到错误就 `return 2` 中止」的做法不同——解析期是「记录错误 + 继续」，解析后期是「检测到就中止」。

**取舍八：CTE 的生命周期绑定在语法树与解析器清理链两套机制上。** `With` 对象既是 `Select.pWith` 的一部分（随语法树走 `sqlite3WithDelete`），也可能被 `sqlite3WithPush()` 用 `bFree=1` 挂到解析器清理链上（走 `sqlite3WithDeleteGeneric`）。`src/parse.y` 的 `with ::= WITH wqlist` 与 `with ::= WITH RECURSIVE wqlist` 两条产生式都传 `bFree=1`；而 `src/select.c` 的 `selectExpander()` 里 `sqlite3WithPush(pParse, p->pWith, 0)` 传 `bFree=0`。`sqlite3WithPush()` 的注释说明了差别：`bFree` 为真时这个 `With` 永不从栈上弹出，要随 `Parse` 一起释放；为假时它随所关联的 SELECT 语句一起释放。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/build.c | `sqlite3CteDelete` | 释放一个 `Cte`：先 `cteClear()` 清 `pCols`/`pSelect`/`zName`，再 `sqlite3DbFree()` 释放对象本身 |
| src/build.c | `cteClear` | `sqlite3CteDelete` 与 `sqlite3WithDelete` 共用的内容清理：删 `pCols` 表达式列表、删 `pSelect` 语法树、释放 `zName` |
| src/build.c | `sqlite3CteNew` | 造 `Cte`：`pSelect`/`pCols`/`zName`/`eM10d` 四字段赋值；OOM 时改为释放入参 `pArglist` 与 `pQuery` |
| src/build.c | `sqlite3WithAdd` | 把 `Cte` 挂进 `With` 数组，按 `SZ_WITH(nCte+1)` 扩容；重名时报 "duplicate WITH table name"；OOM 时调 `sqlite3CteDelete` 并回退到原 `pWith` |
| src/build.c | `sqlite3WithDelete` | 释放 `With`：逐个 `cteClear` 后 `sqlite3DbFree` |
| src/build.c | `sqlite3WithDeleteGeneric` | `void*` 版本的 `sqlite3WithDelete`，供 `sqlite3ParserAddCleanup()` 使用 |
| src/build.c | `sqlite3SrcItemAttachSubquery` | 把 `Select` 挂到 `SrcItem`；`dupSelect` 为真时经 `sqlite3SelectDup()` 造副本，CTE 引用走的正是这条复制路径 |
| src/select.c | `sqlite3ExpandSubquery` | 为 FROM 子句里的子查询造临时 `Table`：列定义取复合 SELECT 最左项的 `pEList`，`nRowLogEst=200`，置 `TF_Ephemeral｜TF_NoVisibleRowid`，`eTabType=TABTYP_VIEW` |
| src/select.c | `sqlite3SelectExpand` | 展开 SELECT 的入口：造 `Walker`（`xExprCallback=sqlite3ExprWalkNoop`、`eCode=0`）后交 `selectExpanderWalk` |
| src/select.c | `selectExpanderWalk` | 展开遍历：复合 SELECT 场景先跑一遍 `convertCompoundSelectToSubquery`，再把 `xSelectCallback` 设为 `selectExpander`、`xSelectCallback2` 设为 `sqlite3SelectPopWith` |
| src/select.c | `selectExpander` | 展开的回调本体：分配游标号、填 `pTab`、折 ON/USING 进 WHERE、展开 `*`；FROM 项优先走 `resolveFromTermToCte`，返回 2 时 `WRC_Abort` |
| src/select.c | `resolveFromTermToCte` | CTE 名字解析与临时表构造：匹配、递归项识别、错误分支（循环引用、INDEXED BY、多重递归、CTE 数量超限、列数不匹配）、`CteUse` 记账 |
| src/select.c | `searchWith` | 沿 `With.pOuter` 链做大小写不敏感名字查找，遇 `bView` 为真的层即停止外扩 |
| src/select.c | `sqlite3WithPush` | WITH 命名空间栈的压栈：`bFree` 为真时先挂 `sqlite3ParserAddCleanup`，再置 `pWith->pOuter = pParse->pWith` |
| src/select.c | `sqlite3SelectPopWith` | 展开遍历的 `xSelectCallback2`，把 `Parse.pWith` 弹回 `pWith->pOuter` |
| src/select.c | `generateWithRecursiveQuery` | 递归 CTE 的执行代码生成：Queue 表 + Current 表双临时表模型，`OP_Once`/`OP_Gosub`/`OP_OpenDup`/`OP_NullRow` 组合 |
| src/select.c | `fromClauseTermCanBeCoroutine` | 判定 FROM 项能否走协程；对 CTE 有两条专门否决：`eM10d==M10d_Yes`，以及 `nUse>=2` 且 `eM10d!=M10d_No` |
| src/select.c | `clearSelect` | SELECT 语法树销毁时逐层调 `sqlite3WithDelete(db, p->pWith)`，是 WITH 子句的主要释放点 |
| src/sqliteInt.h | `Cte` / `With` / `CteUse` | 三个核心结构定义；`With.a` 是 `FLEXARRAY`，`SZ_WITH(N)` 为 `offsetof(With,a)+(N)*sizeof(Cte)` |
| src/sqliteInt.h | `M10d_Yes` / `M10d_Any` / `M10d_No` | 物化标志三态，取值 `0`/`1`/`2` |
| src/sqliteLimit.h | `SQLITE_MAX_CTES` | CTE 数量硬上限，值为 `50000` |
| src/sqlite.h.in | `SQLITE_LIMIT_CTES` | 可运行时调整的 CTE 数量上限，编号为 `15` |
| src/parse.y | `wqitem` / `wqlist` / `with` 产生式 | 解析期建 CTE：`sqlite3CteNew` 造 `Cte`，`sqlite3WithAdd` 挂进 `With`，`sqlite3WithPush(pParse, W, 1)` 压栈 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里（`common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`）没有与 SQLite 的 CTE 命名空间对位的结构。SQLite 的做法是把 CTE 完全放在**编译期**——`With` 对象挂在 `Parse` 上、临时 `Table` 带 `TF_Ephemeral`、执行期只剩一个临时表游标。主核若要在运行期支持命名视图或中间结果集，就必须引入运行期的命名空间对象与并发可见性问题，这是两种成本模型完全不同的设计。

**架构推论：** SQLite 的 CTE 复制策略（`sqlite3SrcItemAttachSubquery` 的 `dupSelect=1`）让每个引用点持有独立语法树，这使得「同一个 CTE 被引用 N 次」的内存开销与 N 成正比。对 AI 生成 SQL 的场景，这是个实际风险：AI 倾向于在一条 SQL 里重复引用同一段 CTE 定义以表达公共逻辑，引用次数一多，解析树的复制开销就会线性上升。

**架构推论：** `eM10d` 三态标志（`AS MATERIALIZED` / 未指定 / `AS NOT MATERIALIZED`）本质上是把「是否物化」这个决定权从规划器手里交还给了用户，但实现上并非集中决策，而是散落在 `fromClauseTermCanBeCoroutine()`、flatten 前置检查、谓词下推条件三处各自读标志做局部否决。AI 若要预测一条带 CTE 的查询的执行形态，必须能同时读懂这三个点；主核若要对齐这一接口，更合理的做法是提供一个集中的「物化策略」判定函数，而不是复制这种分散否决的结构。

**架构推论：** 递归 CTE 的 Queue 表 + Current 表模型要求递归项必须是 UNION 或 UNION ALL，且 UNION 的去重靠一张额外临时表实现——其游标号必须恰好比 Queue 游标号大一（源码注释明确写了这个约束）。这种「靠游标号相邻来编码语义」的技巧对外部实现者极不直观：主核若要实现递归查询，等价于要在自己的执行器里显式暴露「队列表游标号必须紧邻去重表游标号」这类约定。

## 参考

- [SQLite WITH Clause（CTE 的官方语义、递归 CTE 形式与 MATERIALIZED 提示）](https://sqlite.org/lang_with.html)
- [SQLite Limits（运行时可调上限列表，含 CTE 数量上限）](https://sqlite.org/limits.html)
- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层）](https://sqlite.org/arch.html)
- [SQLite Query Planner（物化与协程在查询规划中的位置）](https://sqlite.org/optoverview.html)
- [sqlite/sqlite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c)
- [sqlite/sqlite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
- [sqlite/sqlite 源码：src/parse.y](https://github.com/sqlite/sqlite/blob/master/src/parse.y)
