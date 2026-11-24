---
project: sqlite
feature: name-resolution
page_type: feature
schema_version: 1
refs:
  - path: src/resolve.c
    symbols: [sqlite3ResolveExprNames, sqlite3ResolveSelectNames, sqlite3ResolveOrderGroupBy]
---

# 名称解析

## 一句话职责

名称解析把语法树里的表名/列名绑定到模式中的具体对象：把 `Z`、`Y.Z`、`X.Y.Z` 三种形态的名字解析成「哪个库、哪张表、哪一列、哪条游标」，并对别名、歧义、越界等情况报出确定的错误。

## 机制是什么

边界（依据条目 `boundary`）：负责把语法树里的表名/列名绑定到模式里的具体对象，处理别名与歧义报错；不负责语法树的构造（B02）、不负责生成执行代码（C 组）。

整个机制建立在一个数据结构上：`NameContext`（定义在 `src/sqliteInt.h`）。它描述「当前正在解析的名字可以指向哪里」，字段包括：

- `pSrcList`：当前层 FROM 子句的来源表列表（`SrcList`），表名、别名、游标号都在这里。
- `uNC`：联合体，按场景装 `pEList`（结果集列清单，用于别名匹配）、`pAggInfo`、`pUpsert`、`iBaseReg`。
- `pNext`：指向外层（父查询）的 NameContext，构成一条从内到外的链；`NULL` 表示最外层。
- `nRef`：本层已解析的名字计数，用于判断子查询是否相关（correlated）。
- `nNcErr`：本层累计的解析错误数。
- `ncFlags`：一组 `NC_*` 位标志，控制「此处允许什么」。`src/sqliteInt.h` 里定义了 `NC_AllowAgg`（`0x000001`，此处允许聚合函数）、`NC_PartIdx`（`0x000002`）、`NC_IsCheck`（`0x000004`）、`NC_GenCol`（`0x000008`）、`NC_HasAgg`（`0x000010`）、`NC_IdxExpr`（`0x000020`）、`NC_SelfRef`（`0x00002e`）、`NC_Subquery`（`0x000040`）、`NC_UEList`（`0x000080`，`uNC.pEList` 有效）、`NC_UAggInfo`、`NC_UUpsert`、`NC_UBaseReg`、`NC_MinMaxAgg`（`0x001000`）、`NC_AllowWin`（`0x004000`）、`NC_HasWin`（`0x008000`）、`NC_IsDDL`（`0x010000`）、`NC_InAggFunc`（`0x020000`）、`NC_FromDDL`（`0x040000`）、`NC_NoSelect`（`0x080000`，不下钻进子查询）、`NC_Where`（`0x100000`）、`NC_OrderAgg`（`0x8000000`）。

`src/sqliteInt.h` 在这些常量上方写了取值约束（以 `assert()` 检查）：`NC_HasAgg == SF_HasAgg == EP_Agg`，`NC_MinMaxAgg == SF_MinMaxAgg == SQLITE_FUNC_MINMAX`，`NC_OrderAgg == SF_OrderByReqd == SQLITE_FUNC_ANYORDER`，`NC_HasWin == EP_Win`。

解析的输出是表达式节点本身被改写：命中一列后，`Expr.op` 变成 `TK_COLUMN`，`Expr.iTable` 存该表在 FROM 中的游标号，`Expr.iColumn` 存列号（rowid 为 `-1`），`Expr.y.pTab` 指向 `Table` 结构，`Expr.affExpr` 记录亲和性。命中结果集别名时，节点被整体替换成结果集对应表达式的副本，节点类型保持 `TK_COLUMN` 之外的原类型。

## 怎么实现的

### 入口与遍历骨架

三个公开入口都在 `src/resolve.c`：

- `sqlite3ResolveExprNames(NameContext *pNC, Expr *pExpr)`：解析单个表达式。它构造一个 `Walker`，把 `w.xExprCallback` 设为 `resolveExprStep`，把 `w.xSelectCallback` 设为 `resolveSelectStep`（当 `ncFlags` 含 `NC_NoSelect` 时置 `0`，即不下钻进子查询），`w.u.pNC = pNC`，然后调 `sqlite3WalkExprNN()` 走完整棵树。函数先保存 `NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg` 四个聚合/窗口标志并清零，遍历结束后把结果合并回表达式属性：`ExprSetProperty(pExpr, pNC->ncFlags & (NC_HasAgg|NC_HasWin))`，最后把聚合标志还回 `pNC`。返回值是 `pNC->nNcErr>0 || w.pParse->nErr>0`（非零表示出错）。

  该函数同时做表达式树深度检查：在 `SQLITE_MAX_EXPR_DEPTH>0` 编译条件下累加 `w.pParse->nHeight += pExpr->nHeight`，并调 `sqlite3ExprCheckHeight()`，超限直接返回 `SQLITE_ERROR`。

- `sqlite3ResolveSelectNames(Parse *pParse, Select *p, NameContext *pOuterNC)`：解析整个 SELECT 及其所有后代。它同样构造 `Walker`（回调为 `resolveExprStep` / `resolveSelectStep`），`w.u.pNC = pOuterNC`，然后调 `sqlite3WalkSelect(&w, p)`。它的注释明确要求：所有 SELECT 应当已由 `sqlite3SelectExpand()` 展开过再调用本函数。

- `sqlite3ResolveOrderGroupBy(Parse *pParse, Select *pSelect, ExprList *pOrderBy, const char *zType)`：把 ORDER BY / GROUP BY 中「引用结果集列」的项替换成结果集表达式的副本。它先检查 `pOrderBy==0 || pParse->db->mallocFailed || IN_RENAME_OBJECT`，命中则直接返回 0；再检查 `pOrderBy->nExpr>db->aLimit[SQLITE_LIMIT_COLUMN]`，超限则报 `"too many terms in %s BY clause"` 并返回 1。最后遍历每一项，若 `pItem->u.x.iOrderByCol` 非零且超出 `pEList->nExpr`，调 `resolveOutOfRangeError()` 报错返回 1；否则调 `resolveAlias()` 做替换。

### 名字查找：lookupName

真正的查找发生在 `src/resolve.c` 的静态函数 `lookupName()`。它接收 `zDb`（库名，可为 `NULL`）、`zTab`（表名，可为 `NULL`）、`pRight`（列名所在节点）、`pNC`（当前 NameContext）、`pExpr`（待改写的表达式节点）。

查找顺序与关键分支：

1. **初始化为不匹配。** `pExpr->iTable = -1`，并置 `EP_NoReduce`。
2. **库名解析。** 若给了 `zDb`，先在 `db->aDb[0..db->nDb)` 里用 `sqlite3StrICmp(db->aDb[i].zDbSName, zDb)` 逐个比对，找到则取其 `pSchema`。如果全都不匹配但 `zDb` 大小写无关地等于 `"main"`，则取 `db->aDb[0].pSchema` 并把 `zDb` 换成 `db->aDb[0].zDbSName`——源码注释说明这个分支覆盖「main 库被 `SQLITE_DBCONFIG_MAINDBNAME` 改名」的情况。若 `ncFlags` 含 `NC_PartIdx` 或 `NC_IsCheck`，则直接把 `zDb` 置 `0` 忽略库限定符，注释给出的理由是不为遗留语法报错。
3. **从内向外遍历 NameContext 链。** `do { ... pNC = pNC->pNext; nSubquery++; } while( pNC )`，每轮从 `pNC->pSrcList` 的每个 `SrcItem` 出发。
4. **表名限定。** 若给了 `zTab`：有 `zDb` 时要求 `pTab->pSchema == pSchema`（且 `pSchema==0` 且 `zDb!="*"` 时跳过）；表名匹配先比 `pItem->zAlias`（别名），没有别名则比 `pTab->zName`；都不中时，若 `pTab->tnum==1`（即模式表），交给 `isValidSchemaTableName()` 判断——该函数只接受前缀为 `"sqlite_"`（长度 7）的名字，且要在 `LEGACY_SCHEMA_TABLE`、`PREFERRED_SCHEMA_TABLE`、`LEGACY_TEMP_SCHEMA_TABLE`、`PREFERRED_TEMP_SCHEMA_TABLE` 这几个常量里对上后缀。
5. **列名匹配。** 调 `sqlite3ColumnIndex(pTab, zCol)`（定义在 `src/select.c`），它先用 `sqlite3StrIHash()` 的哈希槽 `pTab->aHx[h % sizeof(pTab->aHx)]` 做「幸运命中」快速判定，失败则全量线性扫描 `pTab->aCol`，比对条件是 `aCol[i].hName==h && sqlite3StrICmp(aCol[i].zCnName, zCol)==0`。返回 `-1` 表示无此列。
6. **歧义判定。** 每次新增匹配 `cnt++`。若 `cnt>0` 时又来了一个匹配：先看 `pItem->fg.isUsing` 与 `sqlite3IdListIndex(pItem->u3.pUsing, zCol)`——不满足 USING 条件则清空 `pFJMatch` 让 `cnt` 继续增长（最终报 "ambiguous column name"）；若满足 USING 且是 INNER/LEFT JOIN（`jointype & JT_RIGHT)==0`），跳过当前表用最左表；若满足 USING 且是 RIGHT JOIN（`jointype & JT_LEFT)==0`），则 `cnt=0` 重置后用最右表；若是 FULL JOIN，则调 `extendFJMatch()` 累积候选，最后构造 `coalesce()` 函数节点（`pExpr->op = TK_FUNCTION`，`pExpr->u.zToken = "coalesce"`，`pExpr->affExpr = SQLITE_AFF_DEFER`）。
7. **rowid 兜底。** 若 `cnt==0 && cntTab>=1 && pMatch` 且 `ncFlags` 不含 `NC_IdxExpr|NC_GenCol`，且 `sqlite3IsRowid(zCol)` 为真（`src/expr.c` 中该函数对 `"_ROWID_"`、`"ROWID"`、`"OID"` 三个名字做 `sqlite3StrICmp` 比对），且 `VisibleRowid(pMatch->pSTab)`（即 `TF_NoVisibleRowid` 位未置），则 `cnt = cntTab`，`pExpr->iColumn = -1`，`pExpr->affExpr = SQLITE_AFF_INTEGER`。
8. **INTEGER PRIMARY KEY 特例。** 匹配到的列号 `j` 若等于 `pTab->iPKey` 且当前不在 `IN_RENAME_OBJECT` 状态，则 `pExpr->iColumn = -1`（即当作 rowid 处理）；否则 `pExpr->iColumn = (i16)j`。
9. **结果集别名匹配。** 若 `cnt==0 && (ncFlags & NC_UEList)!=0 && zTab==0`，则遍历 `pNC->uNC.pEList`，找 `fg.eEName==ENAME_NAME` 且 `sqlite3_stricmp(zEName, zCol)==0` 的项。命中后检查三件事：若 `NC_AllowAgg` 未置而原表达式带 `EP_Agg` 属性，报 `"misuse of aliased aggregate %s"`；若原表达式带 `EP_Win` 而（`NC_AllowWin` 未置或 `pNC != pTopNC`），报 `"misuse of aliased window function %s"`；若 `sqlite3ExprVectorSize(pOrig)!=1`，报 `"row value misused"`。三种情况都 `return WRC_Abort`。检查通过则调 `resolveAlias()` 把 `pExpr` 换成结果集表达式的副本。
10. **错误分支。** `cnt!=1` 时：若 `pFJMatch` 存在且 `pFJMatch->nExpr==cnt-1`，走 coalesce 路径；否则 `zErr = cnt==0 ? "no such column" : "ambiguous column name"`，按是否带 `zDb` / `zTab` 拼出错误消息（带双引号的标识符在 `cnt==0` 时追加提示 `"- should this be a string literal in single-quotes?"`），调 `sqlite3RecordErrorOffsetOfExpr()` 记录出错位置，`pParse->checkSchema = 1`，`pTopNC->nNcErr++`，`eNewExprOp = TK_NULL`。
11. **双引号字符串兼容。** 若 `cnt==0 && zTab==0` 且表达式带 `EP_DblQuoted` 属性，并且 `areDoubleQuotedStringsEnabled()` 为真，则调 `sqlite3_log(SQLITE_WARNING, "double-quoted string literal: \"%w\"", zCol)`，把 `pExpr->op` 改为 `TK_STRING` 并 `return WRC_Prune`。源码注释里作者明确说这是为兼容早期 MySQL 3.x 的历史包袱，且只用于遗留兼容。
12. **收尾。** 若 `pMatch` 非空且 `pExpr->iColumn>=0`，则 `pMatch->colUsed |= sqlite3ExprColUsed(pExpr)`；否则置 `pMatch->fg.rowidUsed = 1`。注释说明 `colUsed` 位掩码用于判断索引是否覆盖，第 63 位及之后统一置位。最后 `pExpr->op = eNewExprOp`。返回时若 `cnt==1` 则把 `pTopNC` 到 `pNC` 之间所有上下文的 `nRef` 加一，并 `return WRC_Prune`；否则 `return WRC_Abort`。

### 表达式遍历：resolveExprStep

`resolveExprStep()` 是 `resolveExprNames` 挂上的表达式回调，按 `pExpr->op` 分派：

- `TK_ROW`：改写为第一张表的 `TK_COLUMN`，`iColumn--`，`affExpr = SQLITE_AFF_INTEGER`。
- `TK_NOTNULL` / `TK_ISNULL`：做「NOT NULL 强度削减」优化——先递归解析左子树，若 `sqlite3ExprCanBeNull(pExpr->pLeft)` 为真则放弃优化；否则要求当前上下文链上每一层的 `ncFlags` 都带 `NC_Where`（不在 WHERE 子句里就不优化），满足则把节点改写成 `TK_INTEGER`（`pExpr->u.iValue = (pExpr->op==TK_NOTNULL)`），并把之前保存的 `nRef` 还原，避免把列误算成被引用而误判为相关子查询。源码注释给出的例子是 `CREATE TABLE t1(a INT NOT NULL); SELECT a, a IS NULL, a IS NOT NULL, count(*) FROM t1;`——在还不知道是否聚合时不能优化，所以只在 WHERE 子句里做。
- `TK_ID` / `TK_DOT`：三种名字形态的唯一入口，最终都调 `lookupName()`。`TK_DOT` 分支先用 `sqlite3ResolveNotValid()` 把 `"."` 运算符在 `NC_IdxExpr|NC_GenCol` 上下文里判为非法。
- `TK_FUNCTION`：解析函数名。先 `sqlite3FindFunction(db, zId, n, enc, 0)` 精确按参数个数查找；找不到再按 `-2` 查找（表示「任意参数个数」），此时置 `wrong_num_args=1`；再找不到置 `no_such_func=1`。若 `pDef->xFinalize!=0` 则 `is_agg=1`。聚合函数在 `NC_AllowAgg` 未置时被判为 "misuse of aggregate function"；窗口函数被误用时报 "misuse of window function"。函数不存在时报 "no such function"，但当 `pParse->db->init.busy!=0` 时（正在读 `sqlite_schema`）会被抑制，除非是内部专用函数（`no_such_func==2` 且 `init.busy==2`）。参数个数不对时报 "wrong number of arguments to function"。聚合函数的 `TK_FUNCTION` 节点被改写为 `TK_AGG_FUNCTION`；`pExpr->op2` 累加外层嵌套层数（`pExpr->op2 += (1 + pNC2->nNestedSelect)`），用来标记它被提升了几层。
- `TK_EXISTS` / `TK_SELECT` / `TK_IN`：处理子查询。递归解析后若 `pNC->nRef` 发生变化，则给表达式置 `EP_VarSelect` 并给子查询的 `Select` 置 `SF_Correlated`。
- `TK_VARIABLE`：参数（`?` / `?NNN` / `:name`）在 `NC_IsCheck|NC_PartIdx|NC_IdxExpr|NC_GenCol` 上下文中非法。
- `TK_IS` / `TK_ISNOT` 及所有比较运算符：检查左右两侧的向量元素个数是否一致，不一致报 "row value misused"。

### SELECT 层遍历：resolveSelectStep

`resolveSelectStep()` 是挂在 `Walker.xSelectCallback` 上的 SELECT 层回调，处理顺序：

1. 若 `p->selFlags` 已含 `SF_Resolved`，返回 `WRC_Prune` 跳过。
2. 若 `p->selFlags` 未含 `SF_Expanded`，改调 `sqlite3SelectPrep(pParse, p, pOuterNC)` 并返回（源码注释说明这是表达式内子查询的场景）。
3. LIMIT / OFFSET 用一个清零的 NameContext 解析——注释写明这两个子句不允许引用任何名字。
4. 递归解析 FROM 子句里的子查询：调 `sqlite3ResolveSelectNames(pParse, pItem->u4.pSubq->pSelect, pOuterNC)`。解析后比较 `pOuterNC->nRef` 前后是否变化，变化则置 `pItem->fg.isCorrelated`。
5. 构造本层 NameContext：`sNC.ncFlags = NC_AllowAgg|NC_AllowWin`，`sNC.pSrcList = p->pSrc`，`sNC.pNext = pOuterNC`。
6. 用 `sqlite3ResolveExprListNames()` 解析结果集列表。若存在 GROUP BY 或结果集里含聚合，则 `p->selFlags |= SF_Aggregate | (sNC.ncFlags&(NC_MinMaxAgg|NC_OrderAgg))`；否则清掉 `NC_AllowAgg`，禁止其他子句里出现聚合。
7. 挂上结果集列清单：`sNC.uNC.pEList = p->pEList; sNC.ncFlags |= NC_UEList;`——注释说明这是为了让 WHERE 等子句能引用结果集别名（SQLite 非标准扩展）。
8. 依次解析 HAVING（若 `(p->selFlags & SF_Aggregate)==0` 则先报 "HAVING clause on a non-aggregate query"）、WHERE（解析前后临时置/清 `NC_Where`）、表值函数的参数。
9. ORDER BY：调静态函数 `resolveOrderGroupBy(&sNC, p, p->pOrderBy, "ORDER")`。
10. GROUP BY：调 `resolveOrderGroupBy(&sNC, p, pGroupBy, "GROUP")`，随后逐项检查 `EP_Agg` 属性，命中报 "aggregate functions are not allowed in the GROUP BY clause"。
11. 复合 SELECT 检查：若 `p->pNext` 存在且 `p->pEList->nExpr != p->pNext->pEList->nExpr`，调 `sqlite3SelectWrongNumTermsError()`。
12. 若 `p->selFlags` 含 `SF_OnToWhere`，调 `sqlite3SelectCheckOnClauses()`。
13. 复合 SELECT 的 ORDER BY 在所有分支都解析完之后由 `resolveCompoundOrderBy(pParse, pLeftmost)` 处理。

### ORDER BY / GROUP BY 的三段式处理

`resolveOrderGroupBy()`（静态）负责「判定一个 ORDER BY/GROUP BY 项指的是哪一列」，`sqlite3ResolveOrderGroupBy()`（公开）负责「把该项替换成结果集表达式的副本」，`resolveCompoundOrderBy()`（静态）处理复合 SELECT。

`resolveOrderGroupBy()` 对每一项：

1. 用 `sqlite3ExprSkipCollateAndLikely()` 剥掉 COLLATE / LIKELY 包装。
2. 若不是 GROUP BY（`zType[0]!='G'`），先调 `resolveAsName()` 匹配结果集的 `AS` 别名。`resolveAsName()` 只在 `pE->op==TK_ID` 且结果集项的 `fg.eEName==ENAME_NAME` 且 `sqlite3_stricmp(zEName, zCol)==0` 时返回 `i+1`（1 起）。命中则置 `pItem->u.x.iOrderByCol = (u16)iCol` 并 `continue`。
3. 否则若是整数常量，`sqlite3ExprIsInteger(pE2, &iCol, 0, 1)` 取出，若 `iCol<1 || iCol>0xffff` 则调 `resolveOutOfRangeError()` 报错返回 1，否则置 `pItem->u.x.iOrderByCol = (u16)iCol`。`0xffff` 就是这里的上界常量（`u16` 字段的取值上限）。
4. 否则按普通表达式处理：置 `pItem->u.x.iOrderByCol = 0`，调 `sqlite3ResolveExprNames(pNC, pE)`；若成功，再用 `sqlite3ExprCompare(0, pE, pSelect->pEList->a[j].pExpr, -1)==0` 与结果集各项比对，命中则把 `iOrderByCol` 置为 `j+1` 并调 `windowRemoveExprFromSelect()` 移除该表达式所属的 Window 对象。
5. 最后调 `sqlite3ResolveOrderGroupBy()` 完成替换。

`sqlite3ResolveOrderGroupBy()` 的替换动作是 `resolveAlias(pParse, pEList, pItem->u.x.iOrderByCol-1, pItem->pExpr, 0)`。`resolveAlias()` 用 `sqlite3ExprDup()` 复制结果集第 `iCol` 项的原表达式，交换到 `pExpr` 里；若原 `pExpr` 是 `TK_COLLATE`，则用 `sqlite3ExprAddCollateString()` 把 COLLATE 保留下来（源码注释举例 `SELECT a+b, c+d FROM t1 ORDER BY 1 COLLATE nocase;` 应被改写成 `SELECT a+b, c+d FROM t1 ORDER BY (a+b) COLLATE nocase;`）。复制出的表达式还要调 `incrAggFunctionDepth(pDup, nSubquery)` 把其中 `TK_AGG_FUNCTION` 节点的 `op2` 增加 `nSubquery`，以便把聚合的层级提升信息带上。

`resolveCompoundOrderBy()` 的处理与上面类似但跨复合分支：先把 `pOrderBy` 各项的 `fg.done` 清零，然后沿 `pSelect->pPrior` 链从最左分支开始逐个尝试匹配。对表达式项，它复制一份 `pDup = sqlite3ExprDup(db, pE, 0)`，调 `resolveOrderByTermToExprList(pParse, pSelect, pDup)` 去匹配——该函数构造一个 `nc.ncFlags = NC_AllowAgg|NC_UEList|NC_NoSelect` 的临时上下文，临时置 `db->suppressErr = 1` 后调 `sqlite3ResolveExprNames()`，然后用 `sqlite3ExprCompare(0, pEList->a[i].pExpr, pE, -1)<2` 逐项比对，命中即返回 `i+1`。匹配成功后（`!IN_RENAME_OBJECT`）把该项替换成整数常量 `sqlite3ExprInt32(db, iCol)`，并保留原有的 COLLATE 包装。所有分支都匹配不上的项，最后报 `"%r ORDER BY term does not match any column in the result set"`。

### 别名替换：resolveAlias 与 incrAggFunctionDepth

`resolveAlias()` 的核心是三步 `memcpy` 交换：`pDup`（结果集表达式副本）、`temp` 与 `pExpr` 依次交换，使得 `pExpr` 最终持有结果集表达式的全部内容，而原来的 `pExpr` 被 `sqlite3ExprDeferredDelete()` 延后释放。若 `pExpr` 带 `EP_WinFunc` 属性，还要把 `pExpr->y.pWin->pOwner` 重新指向新的 `pExpr`。

`incrAggFunctionDepth(pExpr, N)` 构造一个 `Walker`，回调 `incrAggDepth()` 对每个 `TK_AGG_FUNCTION` 节点执行 `pExpr->op2 += pWalker->u.n`。只在 `N>0` 时才真正遍历。

### 单表/无表场景：sqlite3ResolveSelfReference

`sqlite3ResolveSelfReference()` 处理「只能引用单张表或不能引用任何表」的表达式：CHECK 约束（`NC_IsCheck`）、部分索引的 WHERE 子句（`NC_PartIdx`）、索引表达式（`NC_IdxExpr`）、生成列（`NC_GenCol`）、VACUUM INTO 的参数（类型 0）。

它构造一个假的 `SrcList`（用 `union { SrcList sSrc; u8 srcSpace[SZ_SRCLIST_1]; }` 复用内存），若 `pTab` 非空则填入 `pSrc->nSrc = 1`、`zName = pTab->zName`、`pSTab = pTab`、`iCursor = -1`。若 `pTab->pSchema != pParse->db->aDb[1].pSchema`，则给 `type` 加上 `NC_FromDDL`（注释说明这会让非 TEMP 模式元素上的 `TK_FUNCTION` 节点带上 `EP_FromDDL`）。最后设 `sNC.ncFlags = type | NC_IsDDL`，调 `sqlite3ResolveExprNames()` 与 `sqlite3ResolveExprListNames()`。

其注释写明输出语义：除 VACUUM INTO（type 0）外，`TK_COLUMN` 节点的 `Expr.iTable` 置 `-1`、`Expr.iColumn` 置列号；VACUUM INTO 情形下 `TK_COLUMN` 节点直接导致错误。

## 关键取舍

**取舍一：把「查找」和「报错」放在同一个函数里。** `lookupName()` 在找到匹配时改写表达式，在找不到或匹配过多时写错误消息并置 `pTopNC->nNcErr++`。好处是所有名字形态（`Z`、`Y.Z`、`X.Y.Z`）共享一条错误路径，错误文本格式统一；代价是这个函数体很长，把表名匹配、列名匹配、USING/FULL JOIN 消歧、rowid 兜底、触发器 new/old 引用、UPSERT 的 excluded 引用、双引号兼容全部塞在一起。

**取舍二：歧义不立即报错，而是先把计数推上去。** 同一列名在多张表出现时，函数先通过 `cnt` 计数并依据 JOIN 类型决定「用最左表」「用最右表」还是「构造 coalesce()」，只有当 USING 条件也不满足、且 `cnt` 最终不为 1 时才报 "ambiguous column name"。FULL JOIN 的处理是特例：把各候选列收集进 `pFJMatch`，最后生成一个 `coalesce()` 函数节点，`affExpr` 设为 `SQLITE_AFF_DEFER`。

**取舍三：结果集别名是兼容性扩展。** `NC_UEList` 机制让 WHERE / GROUP BY / HAVING 能引用结果集的 `AS` 别名。源码注释明确写这「不是标准 SQL」，是「为了向后兼容才支持的（有点离谱的）SQLite 扩展」，并且每次使用都会通过 `sqlite3_log()` 发一条警告。代价是别名引用的表达式会被重复求值（注释里写明「表达式将被重新求值」）。

**取舍四：双引号字符串兼容是明确的历史包袱。** 当双引号标识符匹配不到任何列时，`lookupName()` 会把它降级成 `TK_STRING` 字符串字面量，而不是报 "no such column"。源码注释里作者写道：这是早期为兼容使用双引号表示字符串的 MySQL 3.x 而加的 hack，他「非常后悔」，其后果是拼错的标识符会被静默转成字符串而非报错；因为大量遗留 SQL 依赖它，只能保留并通过 `sqlite3_log(SQLITE_WARNING, ...)` 记录警告。

**取舍五：深度检查与聚合标志的精细管理。** `sqlite3ResolveExprNames()` 每次进入都先清掉 `NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg`，因为这些标志描述的是「本表达式内」的聚合情况；遍历完再把聚合标志通过 `ExprSetProperty` 写到表达式节点上，并把保存的旧值还回 `pNC`。这保证嵌套表达式的聚合标记不会污染外层。

**取舍六：NOT NULL 强度削减只在 WHERE 子句里做，且会还原 `nRef`。** 优化条件是：左子树不可能为 NULL，且当前上下文链上每一层都带 `NC_Where`。第二条件来自源码注释给出的具体反例（`a INT NOT NULL` 的列在聚合查询里仍可能为 NULL），第一条件则保证列不会被误算作「被引用」，从而避免把普通 SELECT 误判为相关子查询。

**取舍七：单值上限直接来自字段宽度。** `ORDER BY` 项的列号存在 `ExprList.a[].u.x.iOrderByCol` 这个 `u16` 字段里，因此 `resolveOrderGroupBy()` 用 `iCol>0xffff` 作为越界判定，`sqlite3ResolveOrderGroupBy()` 用 `pItem->u.x.iOrderByCol>pEList->nExpr` 作为越界判定。另一处独立上限是 `db->aLimit[SQLITE_LIMIT_COLUMN]`，来自 `src/sqliteLimit.h` 中 `SQLITE_MAX_COLUMN` 的默认值 `2000`（该头文件同时规定 `SQLITE_MAX_COLUMN` 不得超过 `32767`），超限时报 "too many terms in ORDER BY clause" 或 "too many terms in GROUP BY clause"。

**取舍八：错误计数放在 NameContext 上而非全局。** `NameContext.nNcErr` 累加本层错误数，`sqlite3ResolveExprNames()` 用 `pNC->nNcErr>0 || w.pParse->nErr>0` 判定失败，`sqlite3ResolveExprListNames()` 则在循环中一旦 `w.pParse->nErr>0` 就立刻返回 `SQLITE_ERROR`。这样错误可以按层归因，同时保留解析上下文的局部性。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/resolve.c | `sqlite3ResolveExprNames` | 单表达式解析入口；构造 Walker、清/还聚合标志、做表达式深度检查 |
| src/resolve.c | `sqlite3ResolveSelectNames` | 整个 SELECT 及其后代的解析入口，走 `sqlite3WalkSelect()` |
| src/resolve.c | `sqlite3ResolveOrderGroupBy` | 把 ORDER BY/GROUP BY 中指向结果集列的项替换成结果集表达式副本；检查 `SQLITE_LIMIT_COLUMN` 上限 |
| src/resolve.c | `sqlite3ResolveExprListNames` | 表达式列表版本的解析，逐项处理聚合标志 |
| src/resolve.c | `sqlite3ResolveSelfReference` | 单表/无表场景（CHECK、部分索引、索引表达式、生成列、VACUUM INTO） |
| src/resolve.c | `resolveExprStep` | 表达式节点回调：分派 TK_ID/TK_DOT/TK_FUNCTION/TK_ROW/TK_IS 等各类节点 |
| src/resolve.c | `resolveSelectStep` | SELECT 节点回调：LIMIT、FROM 子查询、结果集、HAVING、WHERE、ORDER BY、GROUP BY 的解析顺序 |
| src/resolve.c | `lookupName` | 名字查找核心：库/表/列三级匹配、USING 与 FULL JOIN 消歧、rowid 兜底、别名匹配、错误分支 |
| src/resolve.c | `resolveAlias` | 用结果集表达式副本替换别名引用，保留 COLLATE，处理 EP_WinFunc 的 owner 重指 |
| src/resolve.c | `incrAggFunctionDepth` / `incrAggDepth` | 提升聚合表达式层级时递增 `TK_AGG_FUNCTION` 节点的 `op2` |
| src/resolve.c | `resolveAsName` | 在结果集中匹配 `AS` 别名，返回 1 起的列号 |
| src/resolve.c | `resolveOrderGroupBy` | 判定 ORDER BY/GROUP BY 项指向的结果集列（别名、整数、表达式三条路径） |
| src/resolve.c | `resolveCompoundOrderBy` | 复合 SELECT 的 ORDER BY 解析，逐分支尝试匹配 |
| src/resolve.c | `resolveOrderByTermToExprList` | 把 ORDER BY 表达式与结果集表达式逐项比对，临时抑制错误输出 |
| src/resolve.c | `resolveOutOfRangeError` | 生成 "ORDER BY / GROUP BY term out of range" 错误消息 |
| src/resolve.c | `extendFJMatch` | 为 FULL JOIN 收集候选列，最终构造 coalesce() |
| src/resolve.c | `isValidSchemaTableName` | 校验 `sqlite_` 前缀的模式表名（sqlite_schema / sqlite_temp_schema） |
| src/resolve.c | `areDoubleQuotedStringsEnabled` | 判断是否把双引号标识符降级为字符串字面量 |
| src/resolve.c | `windowRemoveExprFromSelect` | 表达式被改写为结果集列后，从 `Select.pWin` 摘掉其 Window 对象 |
| src/sqliteInt.h | `NameContext` | 名称解析上下文：pSrcList、uNC、pNext、nRef、nNcErr、ncFlags |
| src/sqliteInt.h | `NC_AllowAgg` / `NC_UEList` / `NC_Where` / `NC_NoSelect` | 上下文标志位：允许聚合、结果集列表有效、处于 WHERE、不下钻进子查询 |
| src/sqliteInt.h | `sqlite3ResolveExprNames` / `sqlite3ResolveSelectNames` / `sqlite3ResolveOrderGroupBy` | 三个公开入口的函数声明 |
| src/select.c | `sqlite3ColumnIndex` | 按列名查列号：先走 `aHx` 哈希槽快速命中，失败再全量扫描 `aCol` |
| src/select.c | `sqlite3SelectPrep` | SELECT 准备总入口：先 `sqlite3SelectExpand()` 再 `sqlite3ResolveSelectNames()` |
| src/expr.c | `sqlite3IsRowid` | 判断名字是否为 `_ROWID_` / `ROWID` / `OID` 之一 |
| src/sqliteInt.h | `VisibleRowid` | 宏：`(TF_NoVisibleRowid)==0` 时表可见 rowid |
| src/sqliteLimit.h | `SQLITE_MAX_COLUMN` | 列数上限默认值 `2000`，上限 `32767`；经 `db->aLimit[SQLITE_LIMIT_COLUMN]` 生效 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 是从零手写的多模态数据库内核，其分层（`common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`）中没有与 SQLite 的 `NameContext` 链对位的结构。SQLite 的名称解析是「一次解析期绑定 + 表达式节点改写」模型：名字在编译期就被钉死成游标号 + 列号，执行期不再查名字。主核的 `catalog/` 层如果采用运行期按名字查表的模型，两者在解析成本与执行期开销上的分布会完全不同。

**架构推论：** SQLite 这套机制对多模态场景有直接参考价值的一点是「一个名字解析器服务多种模式」。`NameContext` 的 `ncFlags` 把「此处允许什么」全部编码成位标志——`NC_IsCheck`、`NC_PartIdx`、`NC_IdxExpr`、`NC_GenCol` 分别代表四种「只能引用本表」的上下文。主核若要支持生成列、部分索引表达式等特性，需要同样一套「上下文受限」机制，而不是为每种特性各写一套解析逻辑。

**架构推论：** AI 侧的风险点集中在「错误消息的可诊断性」上。`lookupName()` 把错误分成 "no such column"、"ambiguous column name"、以及带双引号时的追加提示 "- should this be a string literal in single-quotes?"，并且用 `sqlite3RecordErrorOffsetOfExpr()` 记录出错的表达式位置。嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库，AI 生成的 SQL 若在 SDK 层被包装成统一的「查询失败」，这些细分错误类型就会丢失，用户无法区分「模型写错了列名」与「表里有两列同名」。SDK 层如果要保留可诊断性，需要把这些错误消息按类别转发，而不是压成单一字符串。

**架构推论：** 另一个可借鉴的点是「别名与列名同名的歧义处理策略」。SQLite 对 FULL JOIN 的同名列构造 `coalesce()` 函数节点，而不是直接报错——这在多模态场景里对应「多个模态分支返回同名列」的情况，是一种在语义上说得通的处理方式。

## 参考

- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator 的分层）](https://sqlite.org/arch.html)
- [SQLite Documentation Index（语言与编译选项总览）](https://sqlite.org/docs.html)
- [SQLite Result and Error Codes](https://sqlite.org/rescode.html)
- [SQLite Compile-Time Options（`SQLITE_MAX_COLUMN` 等上限的官方说明）](https://sqlite.org/compile.html)
- [sqlite/sqlite 源码：src/resolve.c](https://github.com/sqlite/sqlite/blob/master/src/resolve.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
- [sqlite/sqlite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c)
