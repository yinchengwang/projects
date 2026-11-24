---
project: sqlite
feature: join-planning
page_type: feature
schema_version: 1
refs:
  - path: src/select.c
    symbols: [sqlite3ProcessJoin, sqlite3JoinType, sqlite3SelectExpand]
---

# 连接顺序与外连接

## 一句话职责

连接规划负责把 FROM 子句里的一串表变成一个可执行的连接语义：先在解析期确定每个连接算子的类型（内连接 / 交叉连接 / 左外连接 / 右外连接 / 全外连接 / 自然连接），再在展开期把 NATURAL、ON、USING 三种连接条件统一折进 WHERE 子句并打上 `EP_OuterON` / `EP_InnerON` 标记，最后由 WHERE 循环规划器在这些标记的约束下决定各表实际的嵌套循环顺序。

## 机制是什么

边界（依据条目 `boundary`）：负责多表连接时的连接顺序确定、外连接的语义展开与连接结果列的生成；不负责单表索引选择（D02）、不负责连接结果的物化（D04）。

### 三层职责的分工

SQLite 的连接规划不是一处集中的决策，而是沿编译流水线分成三段，各段的产物互为输入：

| 阶段 | 承担者 | 产物 | 后续影响 |
|---|---|---|---|
| 解析期（算子类型判定） | `sqlite3JoinType()` | 每个 `SrcItem.fg.jointype` 上的连接类型位掩码 | 决定该连接是不是外连接、是不是自然连接 |
| 展开期（语义展开） | `sqlite3ProcessJoin()` | 折进 WHERE 的连接条件项 + `EP_OuterON`/`EP_InnerON` 标记 | 决定 WHERE 规划器哪些项可以下推、哪些项必须延后到外连接的右侧循环 |
| 规划期（顺序确定） | `whereLoopAddAll()` / `wherePathSolver()`（`src/where.c`） | 嵌套循环的表顺序与每一圈的访问方式 | 决定连接的实际执行形态 |

前两段在 `src/select.c` 内，属于本页的源码落点；第三段在 `src/where.c` 与 `src/wherecode.c`，属于 D02 的地盘，本页只描述它**受连接类型约束的方式**。

### 连接类型位掩码

`src/sqliteInt.h` 定义了七个连接类型位：

| 常量 | 值 | 含义 |
|---|---|---|
| `JT_INNER` | `0x01` | 任意形式的内连接或交叉连接 |
| `JT_CROSS` | `0x02` | 显式写了 `CROSS` 关键字 |
| `JT_NATURAL` | `0x04` | 自然连接 |
| `JT_LEFT` | `0x08` | 左外连接 |
| `JT_RIGHT` | `0x10` | 右外连接 |
| `JT_OUTER` | `0x20` | 出现了 `OUTER` 关键字 |
| `JT_LTORJ` | `0x40` | 该项是某个右外连接的左操作数 |
| `JT_ERROR` | `0x80` | 未知或不支持的连接类型 |

全外连接是 `JT_LEFT` 与 `JT_RIGHT` 的组合。注意 `JT_LTORJ` 不是语法层给出的：它由 `src/build.c` 的 `sqlite3SrcListShiftJoinType()` 在解析器收尾时补打——该函数先做一次整体右移（把 `a[i-1].fg.jointype` 复制到 `a[i].fg.jointype`），把 `a[0].fg.jointype` 清零；若整体标志里含 `JT_RIGHT`，则从最右的 `JT_RIGHT` 项开始向左逐项 `|= JT_LTORJ`。

### 连接条件的三种来源

ON、USING、NATURAL 三种写法最终都变成 WHERE 子句里的项，但携带的标记不同：

- `EP_InnerON`（`0x000002`）——来自内连接的 ON/USING；
- `EP_OuterON`（`0x000001`）——来自外连接的 ON/USING。

除标记外，这些项还带 `Expr.w.iJoin`，记录它依赖哪张表（右侧表）。`sqlite3SetJoinExpr()` 的函数头注释说明了这个字段的用途：`SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.b AND t1.x=5` 中，`t1.x=5` 这一项必须延后到 `t2` 的循环之后再处理，否则 `t1.x!=5` 的行永远不会出现在输出里。

## 怎么实现的

### 第一步：sqlite3JoinType 判定连接类型

`sqlite3JoinType(Parse *pParse, Token *pA, Token *pB, Token *pC)` 定义在 `src/select.c`，声明在 `src/sqliteInt.h`。它接受 JOIN 关键字前的 1 到 3 个标识符（`pA` 必非空，`pB`/`pC` 可空），返回一个位掩码。

实现方式是把 `pA`/`pB`/`pC` 逐个与一张关键字表比对。关键字表 `aKeyword` 用一个静态字符串 `zKeyText = "naturaleftouterightfullinnercross"` 加偏移量与长度来定位每个关键字，7 条表项分别是：

| 表项 | 在 `zKeyText` 中的偏移 | 长度 | 对应位掩码 |
|---|---|---|---|
| natural | `0` | `7` | `JT_NATURAL` |
| left | `6` | `4` | `JT_LEFT|JT_OUTER` |
| outer | `10` | `5` | `JT_OUTER` |
| right | `14` | `5` | `JT_RIGHT|JT_OUTER` |
| full | `19` | `4` | `JT_LEFT|JT_RIGHT|JT_OUTER` |
| inner | `23` | `5` | `JT_INNER` |
| cross | `28` | `5` | `JT_INNER|JT_CROSS` |

比对方式是长度相等且 `sqlite3StrNICmp` 大小写不敏感比较相等。三个标识符全部匹配完后，位掩码是各表项按位或的结果——这正是 `LEFT OUTER` 得到 `JT_LEFT|JT_OUTER`（`outer` 表项只贡献 `JT_OUTER`）、`FULL OUTER` 得到 `JT_LEFT|JT_RIGHT|JT_OUTER` 的原因。

**错误分支一（未知关键字）：** 某个标识符的长度与所有表项都不匹配时，循环在 `j>=ArraySize(aKeyword)` 处命中，函数置 `JT_ERROR` 位并 `break` 跳出标识符循环。

**错误分支二（互斥组合）：** 函数末尾有三条互斥检查：

```c
if(
   (jointype & (JT_INNER|JT_OUTER))==(JT_INNER|JT_OUTER) ||
   (jointype & JT_ERROR)!=0 ||
   (jointype & (JT_OUTER|JT_LEFT|JT_RIGHT))==JT_OUTER
){
```

第一条命中 `INNER` 与 `OUTER` 同时出现；第二条命中上面置的 `JT_ERROR`；第三条命中「只有 `OUTER` 而没有 `LEFT`/`RIGHT`/`FULL`」。三者任一成立时，函数调 `sqlite3ErrorMsg(pParse, "unknown join type: %T%s%T%s%T", pA, zSp1, pB, zSp2, pC)` 报错——消息里 `zSp1`/`zSp2` 是空格指针，在 `pB` 或 `pC` 为空时各自后移一字节，从而把不存在的标识符连同其前导空格一起省略。

**降级路径：** 报错之后函数并不返回错误码，而是把 `jointype` 强行回退成 `JT_INNER` 并正常返回。也就是说解析器拿到的是一个「合法」的连接类型，错误信息通过 `pParse->nErr` 承载，由上层在展开或代码生成阶段中止语句。

### 第二步：sqlite3ProcessJoin 展开连接语义

`sqlite3ProcessJoin(Parse *pParse, Select *p)` 是 `src/select.c` 的静态函数，函数头注释列出了它做的两件事：把 NATURAL 连接转成 USING 连接；把 ON 和 USING 子句变成额外的 WHERE 项并打上 `EP_OuterON` 或 `EP_InnerON` 标记。

它的数据结构约定写在注释里：FROM 子句的项存在 `Select.pSrc` 里，最左的表是 `pSrc->a[0]`，最右的是最后一项；**连接算子挂在右项上**——`a[1]` 存的是 `a[0]` 与 `a[1]` 之间的连接算子，该连接上的 ON/USING 也挂在右项上。

**早退分支：** 函数开头 `if( pSrc->nSrc<=1 ) return 0;`——只有一项时没有任何连接要处理，直接返回 0（返回值为遇到的错误个数）。

**标记的选取：** 循环体里 `joinType = (pRight->fg.jointype & JT_OUTER)!=0 ? EP_OuterON : EP_InnerON;`。注意判据是 `JT_OUTER` 而不是 `JT_LEFT`/`JT_RIGHT`——`JT_OUTER` 只在出现 `OUTER` 关键字时置位，而 `left` 与 `right` 两张关键字表项本身都自带 `JT_OUTER`，所以 `LEFT JOIN`（不写 `OUTER`）也会带上 `JT_OUTER` 位。

**NATURAL 分支：** 若右项带 `JT_NATURAL`：

1. 先检查 `pRight->fg.isUsing || pRight->u3.pOn` 是否成立，成立则报 "a NATURAL join may not have an ON or USING clause" 并 `return 1`。
2. 遍历右表的每一列（`for(j=0; j<pRightTab->nCol; j++)`），跳过 `IsHiddenColumn()` 为真的列，取列名 `pRightTab->aCol[j].zCnName`，调 `tableAndColumnIndex(pSrc, 0, i, zName, 0, 0, 1)` 在左侧的 FROM 项（索引 0 到 i）里找同名列。找到就把列名追加进一个新建的 `IdList`（`sqlite3IdListAppend`），并置 `pRight->fg.isUsing = 1`、`pRight->fg.isSynthUsing = 1`、`pRight->u3.pUsing = pUsing`。
3. `if( pParse->nErr ) return 1;` 收尾。

`tableAndColumnIndex()` 是 `src/select.c` 的静态函数，签名最后一个参数 `bIgnoreHidden` 为真时跳过隐藏列；它逐项调 `sqlite3ColumnIndex()` 找列名，命中即返回 1 并（出参非空时）回填表索引与列索引。

**USING 分支：** 若 `pRight->fg.isUsing` 为真，对 USING 列表里的每个列名 `zName`：

1. `iRightCol = sqlite3ColumnIndex(pRightTab, zName)` 找右表列号；若 `iRightCol<0`，或 `tableAndColumnIndex(pSrc, 0, i, zName, &iLeft, &iLeftCol, pRight->fg.isSynthUsing)` 返回 0（左侧找不到同名列），则报 "cannot join using column %s - column not present in both tables" 并 `return 1`。
2. 构造左侧列引用 `pE1 = sqlite3CreateColumnExpr(db, pSrc, iLeft, iLeftCol)` 并调 `sqlite3SrcItemColumnUsed(&pSrc->a[iLeft], iLeftCol)` 标记该列被使用。
3. **右外连接的 coalesce 处理：** 若 `pSrc->a[0].fg.jointype & JT_LTORJ` 非零且 `pParse->nErr==0`，进入一个 while 循环反复调 `tableAndColumnIndex(pSrc, iLeft+1, i, zName, ...)` 找左侧更多含该列名的表。若某张表不是 USING 连接（`pSrc->a[iLeft].fg.isUsing==0`），或者它的 USING 列表里没有这一列（`sqlite3IdListIndex(pSrc->a[iLeft].u3.pUsing, zName)<0`），就报 "ambiguous reference to %s in USING()" 并 `break`。否则把当前的 `pE1` 追加进 `pFuncArgs` 并换成下一张表的列引用。循环结束后若 `pFuncArgs` 非空，再把最后一个 `pE1` 追加进去，然后 `pE1 = sqlite3ExprFunction(pParse, pFuncArgs, &tkCoalesce, 0)` 造出 `coalesce(...)` 调用，其中 `tkCoalesce` 是函数内的静态 `Token`，值为 `{"coalesce", 8}`。造出后置 `pE1->affExpr = SQLITE_AFF_DEFER`。
4. **左外连接的分支：** 否则若 `pSrc->a[i+1].fg.jointype & JT_LEFT` 非零且 `pParse->nErr==0`，只做一件事——`ExprSetProperty(pE1, EP_CanBeNull)`，即给左侧列引用打上「可能为 NULL」的标记。
5. 构造右侧引用 `pE2 = sqlite3CreateColumnExpr(db, pSrc, i+1, iRightCol)`（注意用的是 `i+1` 即右项本身的索引），调 `sqlite3SrcItemColumnUsed(pRight, iRightCol)`。
6. 造等值项 `pEq = sqlite3PExpr(pParse, TK_EQ, pE1, pE2)`；若 `pEq` 非空，`ExprSetProperty(pEq, joinType)` 打上 `EP_OuterON` 或 `EP_InnerON`，`ExprSetVVAProperty(pEq, EP_NoReduce)`，并置 `pEq->w.iJoin = pE2->iTable`。
7. `p->pWhere = sqlite3ExprAnd(pParse, p->pWhere, pEq)` 把等值项 AND 进 WHERE；末尾 `if( pParse->nErr ) return 1;`。

**ON 分支：** 若既不是 USING 又挂了 `pRight->u3.pOn`，则 `sqlite3SetJoinExpr(pRight->u3.pOn, pRight->iCursor, joinType)` 给整个 ON 表达式打标记，然后 `p->pWhere = sqlite3ExprAnd(pParse, p->pWhere, pRight->u3.pOn)` 折进 WHERE，随即 `pRight->u3.pOn = 0`（置空，防止重复折入）、`pRight->fg.isOn = 1`、`p->selFlags |= SF_OnToWhere`。`SF_OnToWhere` 在 `src/sqliteInt.h` 的值为 `0x40000000`，注释是 "One or more ON clauses moved to WHERE"。

**表值函数的额外约束：** 循环体末尾有一段专门检查表值函数的参数是否非法引用了它右侧的表。条件是 `(pRight->fg.isTabFunc && joinType==EP_OuterON && pRight->u1.pFuncArg)` 或 `(bSeenTFunc && joinType==EP_OuterON)`，命中即置 `p->selFlags |= SF_OnToWhere`。`bSeenTFunc` 是循环外声明的标志，在循环里通过 `bSeenTFunc = (pLeft->fg.isTabFunc && pLeft->u1.pFuncArg)` 累积，表示「左边已经出现过带参数的表值函数」。

### 第三步：sqlite3SelectExpand 触发连接处理

`sqlite3SelectExpand(Parse *pParse, Select *pSelect)` 是 `src/select.c` 的静态函数，函数体只有四步：造一个 `Walker w`，置 `w.xExprCallback = sqlite3ExprWalkNoop`、`w.pParse = pParse`、`w.eCode = 0`，然后调 `selectExpanderWalk(&w, pSelect)`。

`selectExpanderWalk()` 先看 `pWalker->pParse->hasCompound`：为真时把 `xSelectCallback` 临时换成 `convertCompoundSelectToSubquery`、`xSelectCallback2` 置 0，先走一遍 `sqlite3WalkSelect()`（把带 COLLATE 子句的 ORDER BY 表达式的复合 SELECT 转成子查询）；之后把 `xSelectCallback` 设回 `selectExpander`、`xSelectCallback2` 设为 `sqlite3SelectPopWith`，再 `sqlite3WalkSelect(pWalker, pSelect)`。

真正干活的是 `selectExpander(Walker *pWalker, Select *p)`。它的函数头注释列出「展开」的四件事，其中第三件就是「Add terms to the WHERE clause to accommodate the NATURAL keyword on joins and the ON and USING clause of joins」。连接处理在 FROM 子句遍历循环**结束之后**立刻进行：

```c
/* Process NATURAL keywords, and ON and USING clauses of joins.
*/
assert( db->mallocFailed==0 || pParse->nErr!=0 );
if( pParse->nErr || sqlite3ProcessJoin(pParse, p) ){
  return WRC_Abort;
}
```

即：`sqlite3ProcessJoin()` 返回非零（表示遇到错误）时，`selectExpander` 返回 `WRC_Abort` 中止整个展开。

`sqlite3SelectExpand()` 的调用点是 `sqlite3SelectPrep()`（`src/select.c`）：`sqlite3SelectPrep` 先调 `sqlite3SelectExpand(pParse, p)`，紧接着 `if( pParse->nErr ) return;`，然后才调 `sqlite3ResolveSelectNames(pParse, p, pOuterNC)`。函数头注释明确写出这个顺序：「Expanding a SELECT statement is the first step in processing a SELECT statement. The SELECT statement must be expanded before name resolution is performed.」

### 第四步：展开之后的连接顺序约束

`sqlite3ProcessJoin` 只负责把语义折平，**并不决定连接顺序**。顺序由 `src/where.c` 的循环规划器决定，但它的可选空间被连接类型严格约束。关键约束在 `whereLoopAddAll()` 里：

```c
if( bFirstPastRJ
 || (pItem->fg.jointype & (JT_OUTER|JT_CROSS|JT_LTORJ))!=0
){
  ...
  if( pItem->fg.jointype & (JT_LTORJ|JT_CROSS) ){
    hasRightCrossJoin = 1;
  }
  mPrereq |= mPrior;
  bFirstPastRJ = (pItem->fg.jointype & JT_RIGHT)!=0;
}
```

即：带 `JT_OUTER`、`JT_CROSS` 或 `JT_LTORJ` 的项不能被重排——`mPrereq |= mPrior` 把前面所有项都设为它的前置依赖。源码注释说明了原因：这层限制对 LEFT JOIN 是正确性要求（不强制会得到错误结果），对 CROSS JOIN 则只是为了控制连接顺序、作为调节手段。

`bFirstPastRJ` 用来阻止右外连接的右操作数与其右侧的项互换：`bFirstPastRJ = (pItem->fg.jointype & JT_RIGHT)!=0` 在越过右外连接后置真。

`hasRightCrossJoin` 的语义在注释里写明：它阻止 FROM 项从 LEFT JOIN 或 CROSS JOIN 的右侧移到左侧。对 LEFT JOIN 这是正确性要求；对 CROSS JOIN 只是性能调节。

### 第五步：外连接的强度削弱

`src/select.c` 里有一段把外连接降级成普通连接的改写，源码注释称之为 "OUTER JOIN strength reduction"，并标注了测试标签 `tag-select-0220`。触发条件是三项同时成立：

```c
if( (pItem->fg.jointype & (JT_LEFT|JT_LTORJ))!=0
 && sqlite3ExprImpliesNonNullRow(p->pWhere, pItem->iCursor,
                                 pItem->fg.jointype & JT_LTORJ)
 && OptimizationEnabled(db, SQLITE_SimplifyJoin)
){
```

`sqlite3ExprImpliesNonNullRow(Expr *p, int iTab, int isRJ)` 定义在 `src/expr.c`。它的函数头注释写明：若 WHERE 子句要求表 `iTab` 的某列非 NULL，则该表的空行（NULL 扩展行）不可能出现在结果里，LEFT JOIN 可以安全转成普通 JOIN；**假阳性不允许**，因为会导致错误答案；假阴性只是错过优化机会。注释还写明：带 `EP_OuterON` 标记的项（来自外连接的 ON/USING）不参与这项分析。

削弱动作按标志位分三种：

- **FULL JOIN → RIGHT JOIN：** 命中 `JT_LEFT` 且命中 `JT_RIGHT` 时执行 `pItem->fg.jointype &= ~JT_LEFT`，`TREETRACE` 打印 "FULL-JOIN simplifies to RIGHT-JOIN on term %d"。
- **LEFT JOIN → JOIN：** 只命中 `JT_LEFT` 时执行 `pItem->fg.jointype &= ~(JT_LEFT|JT_OUTER)`，并调 `unsetJoinExpr(p->pWhere, pItem->iCursor, 0)` 把该表对应的 `EP_OuterON` 标记改写成 `EP_InnerON`。
- **RIGHT JOIN → JOIN：** 命中 `JT_LTORJ` 时，向右遍历后续项；若某项带 `JT_RIGHT` 且不带 `JT_LEFT`，执行 `pI2->fg.jointype &= ~(JT_RIGHT|JT_OUTER)` 并调 `unsetJoinExpr(p->pWhere, pI2->iCursor, 1)`（第二个参数 `nullable` 为 1，表示保留 `EP_CanBeNull` 位——因为该表是右外连接的左操作数，它的列本来就可为 NULL）。若是 FULL JOIN 则执行 `pI2->fg.jointype &= ~JT_RIGHT` 把它降为左外连接。

最后一步是从右往左清 `JT_LTORJ`：`for(j=pTabList->nSrc-1; j>=0; j--)` 逐项 `pI2->fg.jointype &= ~JT_LTORJ`，若清完之后该项既无 `JT_LEFT` 也无 `JT_RIGHT`，则调 `clearOuterOnExpr(p->pWhere, pI2->iCursor)` 抹掉 `EP_OuterON` 标记，再调 `unwrapIfNullRow(pParse, p, pI2->iCursor)` 移除 `TK_IF_NULL_ROW` 包装。

`unsetJoinExpr()` 是 `src/select.c` 的静态函数，函数头注释说明它用于两种场景：LEFT JOIN 被简化成普通 JOIN 时，以及 ON 表达式被下推到子查询的 WHERE 时。它遍历表达式树，对带 `EP_OuterON` 且 `w.iJoin==iTable` 的节点清除 `EP_OuterON|EP_InnerON`，随后若 `iTable>=0` 再补上 `EP_InnerON`。

`clearOuterOnExpr()` 是 `src/select.c` 的静态函数，用一个 `Walker` 配 `clearOuterOnExprCb` 回调完成，作用是**只抹掉 `EP_OuterON` 标记而不补 `EP_InnerON`**，让这些项变成普通的 WHERE 项。函数头注释写明它的使用场景是「表 `iTable` 仅仅因为是右外连接的左操作数（`JT_LTORJ`）而被标为外连接表，而这个状态已被外连接强度削弱清除」。

## 关键取舍

**取舍一：连接类型判定放在解析期，且错误时回退成内连接。** `sqlite3JoinType()` 遇到非法组合时，报错之后把 `jointype` 置成 `JT_INNER` 并正常返回，而不是返回错误码。**架构推论：** 好处是解析器可以一路把语法树建完，错误统一由 `pParse->nErr` 承载、在后续阶段一次性中止；代价是解析器内部会在一棵「已知有错」的语法树上继续工作，`sqlite3ProcessJoin()` 里的 `if( pParse->nErr ) return 1;` 这类检查必须存在，否则会在错误状态上继续改写语义。

**取舍二：ON/USING 的条件被"下沉"到 WHERE，而不是保留独立的连接条件树。** `sqlite3ProcessJoin()` 把 ON 表达式和 USING 折出的等值项全部 `sqlite3ExprAnd()` 进 `p->pWhere`。这个做法让 WHERE 规划器可以把连接条件当作普通等值约束参与代价估算与索引选择。代价是必须另设标记来区分「这条约束来自 ON/USING」还是「来自用户写的 WHERE」——这就是 `EP_OuterON` / `EP_InnerON` 存在的全部理由，`src/sqliteInt.h` 里 `Expr.w.iJoin` 的注释就是这么写的：「If EP_OuterON or EP_InnerON, the right table」。

**取舍三：外连接语义用"标记 + 延后处理"实现，而不是独立的连接算子树。** `EP_OuterON` 标记的项不会被立即求值。`src/sqliteInt.h` 里 `ExprUseWOfst(E)` 宏的定义为 `(((E)->flags&(EP_InnerON|EP_OuterON))==0)`——带这两个标记的项不用 WHERE 项的偏移量布局。**架构推论：** 这套「用位标记代替数据结构」的方案，让外连接与内连接共用同一套 WHERE 规划与代码生成路径，代价是要理解外连接语义必须同时读懂 `src/select.c`（打标记）与 `src/wherecode.c`（按标记分派）两处。

**取舍四：NATURAL 连接在展开期被改写成 USING。** `sqlite3ProcessJoin()` 把 NATURAL 连接的公共列名收集成一个 `IdList`，置 `pRight->fg.isSynthUsing = 1` 标记它是合成的，然后按 USING 的路径处理。函数头注释写明：改写之后「我们不再需要关心 NATURAL 连接，只需要考虑 USING 连接」。代价是合成的 USING 列表与用户手写的 USING 列表在后续处理上要走不同的分支（例如 coalesce 构造时 `bIgnoreHidden` 参数传的是 `pRight->fg.isSynthUsing`）。

**取舍五：右外连接靠 `JT_LTORJ` 标记隐式实现，而不是直接做表序翻转。** `JT_LTORJ` 打在右外连接左操作数的每一项上，让代码生成器能识别出「这张表可能成为空行的一侧」。`src/sqliteInt.h` 对它的注释是 "One of the LEFT operands of a RIGHT JOIN"。**架构推论：** 这种标记传播的做法避免了把 FROM 子句物理倒转（那会让 ON/USING 的方向全部反过来），代价是 `JT_LTORJ` 出现在全仓三十多处判断里（`src/select.c`、`src/expr.c`、`src/resolve.c`、`src/where.c`、`src/wherecode.c`、`src/whereexpr.c`、`src/treeview.c`），任何新增的连接相关逻辑都必须同时考虑这个标记。

**取舍六：连接顺序由规划器决定，但外连接与交叉连接把它钉死。** `whereLoopAddAll()` 里 `mPrereq |= mPrior` 把外连接/交叉连接右边的项锁在左边所有项之后。源码注释明确区分：这对 LEFT JOIN 是正确性要求（不强制会得到错误答案），对 CROSS JOIN 纯粹是控制连接顺序的手段（注释理由是「现实中关心性能的查询基本不会真写 CROSS JOIN 语法」）。**架构推论：** 也就是说 SQLite 牺牲了 CROSS JOIN 参与重排的自由度来换取连接顺序的稳定与 LEFT JOIN 的正确性。

**取舍七：外连接强度削弱只做安全的单向降级，并受 `SQLITE_SimplifyJoin` 开关控制。** `sqlite3ExprImpliesNonNullRow()` 的注释明确禁止假阳性，因为假阳性会导致错误答案；宁可漏掉优化。削弱动作全部是单向的（LEFT→JOIN、FULL→RIGHT、RIGHT→JOIN），没有任何逆向操作。

**取舍八：错误信息在连接类型上一次性给出完整三段，而不是逐段校验。** `sqlite3JoinType()` 的错误消息用 `%T%s%T%s%T` 模板拼接三个标识符，缺失的标识符连同其前导空格一起省略（`zSp1`/`zSp2` 是可后移的空格指针）。**架构推论：** 这让用户的拼写错误（如 `LEFT RIGHT JOIN`）一次就能看到全部三个词，而不是改一次试一次。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/select.c | `sqlite3JoinType` | 判定 JOIN 算子的类型：把 1 到 3 个标识符与 `aKeyword` 关键字表比对，返回 `JT_*` 位掩码；非法组合报 "unknown join type" 后回退成 `JT_INNER` |
| src/select.c | `sqlite3ProcessJoin` | 展开连接语义：NATURAL 转 USING、USING 折成等值项、ON 折进 WHERE，并打上 `EP_OuterON`/`EP_InnerON`；返回遇到的错误个数 |
| src/select.c | `sqlite3SelectExpand` | 展开 SELECT 的入口，造 `Walker` 后交 `selectExpanderWalk`；`selectExpander` 在其中调用 `sqlite3ProcessJoin` |
| src/select.c | `selectExpander` | 展开回调本体：分配游标号、填 `pTab`、折 ON/USING 进 WHERE、展开 `*`；`sqlite3ProcessJoin` 返回非零时 `WRC_Abort` |
| src/select.c | `selectExpanderWalk` | 展开遍历：复合 SELECT 场景先跑 `convertCompoundSelectToSubquery`，再把 `xSelectCallback` 设为 `selectExpander`、`xSelectCallback2` 设为 `sqlite3SelectPopWith` |
| src/select.c | `tableAndColumnIndex` | 在 `pSrc->a[iStart..iEnd]` 里按列名找表；`bIgnoreHidden` 为真时跳过隐藏列；命中时回填表索引与列索引并调 `sqlite3SrcItemColumnUsed` |
| src/select.c | `sqlite3SetJoinExpr` | 给整个 ON 表达式树打上 `EP_OuterON`/`EP_InnerON` 并逐节点置 `w.iJoin = iTable` |
| src/select.c | `unsetJoinExpr` | 外连接强度削弱的配套操作：把指定表的 `EP_OuterON` 改写成 `EP_InnerON`；`nullable` 为真时保留 `EP_CanBeNull` |
| src/select.c | `clearOuterOnExpr` | 只抹 `EP_OuterON` 不补 `EP_InnerON`，让这些项变回普通 WHERE 项；经 `clearOuterOnExprCb` 回调完成 |
| src/select.c | `unwrapIfNullRow` | 外连接强度削弱后移除整个 SELECT 里的 `TK_IF_NULL_ROW` 包装；内部用 `substSelect` 遍历替换 |
| src/select.c | `sqlite3SelectPrep` | 调用序列的编排者：先 `sqlite3SelectExpand`，再 `sqlite3ResolveSelectNames`，最后 `sqlite3SelectAddTypeInfo` |
| src/select.c | `sqlite3ColumnIndex` | 按列名在表里找列号：先用 `aHx` 哈希数组做一次快速命中，失败再全表线性扫 |
| src/sqliteInt.h | `JT_INNER` / `JT_CROSS` / `JT_NATURAL` / `JT_LEFT` / `JT_RIGHT` / `JT_OUTER` / `JT_LTORJ` / `JT_ERROR` | 连接类型位掩码，取值依次为 `0x01` / `0x02` / `0x04` / `0x08` / `0x10` / `0x20` / `0x40` / `0x80` |
| src/sqliteInt.h | `EP_OuterON` / `EP_InnerON` | 表达式属性位，取值 `0x000001` / `0x000002`，标记该项来自外连接/内连接的 ON 或 USING |
| src/sqliteInt.h | `Expr.w.iJoin` | 表达式依赖的右侧表号，仅在带 `EP_OuterON` 或 `EP_InnerON` 时有意义 |
| src/sqliteInt.h | `SF_OnToWhere` | `Select.selFlags` 的一个位，值为 `0x40000000`，表示有 ON 子句被移进了 WHERE |
| src/build.c | `sqlite3SrcListShiftJoinType` | 解析收尾时把连接算子整体右移一位并清空 `a[0].fg.jointype`；若含 `JT_RIGHT`，则向左逐项补打 `JT_LTORJ` |
| src/build.c | `sqlite3SrcListAssignCursors` | 给 FROM 子句每一项分配 VDBE 游标号 `iCursor = pParse->nTab++`；子查询项递归处理 |
| src/expr.c | `sqlite3ExprImpliesNonNullRow` | 判断 WHERE 是否蕴含「表 `iTab` 的某列非 NULL」；带 `EP_OuterON` 的项不参与分析；注释明确禁止假阳性 |
| src/where.c | `whereLoopAddAll` | 规划期把连接顺序钉死：带 `JT_OUTER`/`JT_CROSS`/`JT_LTORJ` 的项 `mPrereq |= mPrior`，禁止重排 |
| src/where.c | `wherePathSolver` | 在被钉死的顺序约束下搜索代价最低的嵌套循环路径 |
| src/whereInt.h | `SQLITE_QUERY_PLANNER_LIMIT` / `SQLITE_QUERY_PLANNER_LIMIT_INCR` | 规划器的算力预算：基线 `20000`，每处理一个 FROM 项加 `1000` |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里（`common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`）没有与 SQLite 的连接类型位掩码对位的结构。SQLite 把连接语义压缩成一组位（`JT_LEFT`/`JT_RIGHT`/`JT_OUTER`/`JT_CROSS`/`JT_NATURAL`/`JT_LTORJ`），并用 `EP_OuterON` 标记把「这条约束来自 ON 还是来自 WHERE」的信息挂在表达式节点上。主核若要对齐这一接口，需要一个等价机制把连接约束的来源与依赖表一起带在谓词上，否则外连接的延后求值规则无处表达。

**架构推论：** `sqlite3ProcessJoin()` 的展开策略是「把 ON/USING 全部下沉到 WHERE，再用位标记区分来源」。这让 SQLite 的连接条件与普通等值条件共用同一套 WHERE 规划路径，也让 `whereLoopAddAll()` 可以对二者一视同仁地做代价估算。对 AI 生成 SQL 的场景，这意味着 AI 只要写出正确的 SQL 文本，连接条件与普通条件的优化待遇就是自动一致的；但反过来，AI 也无法通过 SQL 文本显式要求「这条条件必须走索引而那条必须延后」——这个维度在 SQLite 里被完全隐藏在编译期。

**架构推论：** SQLite 对外连接的处理是「标记 + 延后」而非「独立算子树」，并依靠 `sqlite3ExprImpliesNonNullRow()` 做保守的强度削弱（注释明确禁止假阳性，因为假阳性会导致错误答案）。主核若要提供外连接支持，更稳妥的路线是保留独立的连接算子树而非复用 WHERE 谓词列表——代价是规划器要额外处理连接边界，但收益是外连接语义不必依赖一组全局分散的标记位。

**架构推论：** 右外连接在 SQLite 里靠 `JT_LTORJ` 传播标记隐式实现，该标记出现在 `src/select.c`、`src/expr.c`、`src/resolve.c`、`src/where.c`、`src/wherecode.c`、`src/whereexpr.c`、`src/treeview.c` 等多个文件的判断中。这种「一个语义靠一个全局传播的位来维持」的形态，对增量演进不友好：任何新增的连接相关代码路径若忘记检查 `JT_LTORJ`，就会在右外连接上产生错误结果且不易被发现。

## 参考

- [SQLite Query Planner（查询规划总览，含连接顺序与外连接在规划中的位置）](https://sqlite.org/optoverview.html)
- [SQLite SELECT（FROM 子句与 JOIN 语法的官方语义，含 USING / NATURAL / 外连接）](https://sqlite.org/lang_select.html)
- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层）](https://sqlite.org/arch.html)
- [SQLite Documentation Index（官方文档总入口）](https://sqlite.org/docs.html)
- [sqlite/sqlite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c)
- [sqlite/sqlite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c)
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c)
- [sqlite/sqlite 源码：src/where.c](https://github.com/sqlite/sqlite/blob/master/src/where.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
