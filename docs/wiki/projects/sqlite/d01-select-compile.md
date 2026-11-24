---
project: sqlite
feature: select-compile
page_type: feature
schema_version: 1
refs:
  - path: src/select.c
    symbols: [sqlite3SelectPrep, sqlite3Select, sqlite3GenerateColumnNames]
---

# SELECT 编译

## 一句话职责

`sqlite3Select` 把一棵已解析、已解析名字的 `Select` 树编译成 VDBE 程序：分配游标、展开 FROM 子句、生成结果列名与类型、组织 WHERE 过滤、GROUP BY 聚合、ORDER BY 排序和 DISTINCT 去重，并决定结果输出去向。

## 机制是什么

SQLite 的 SELECT 编译分成两层：

- **准备层** `sqlite3SelectPrep` 负责语义准备，不产生字节码。它按固定顺序做三件事——`sqlite3SelectExpand` 展开 FROM 子句与通配符、`sqlite3ResolveSelectNames` 解析标识符与绑定表达式、`sqlite3SelectAddTypeInfo` 补齐子查询结果集的列类型与排序规则。函数开头有 `SF_HasTypeInfo` 幂等检查，已准备过的 `Select` 直接返回。
- **代码生成层** `sqlite3Select` 负责把准备好的树翻译成 VDBE 指令。它按顺序安排：丢弃无意义的 ORDER BY（`IgnorableDistinct` 场景）、调用 `sqlite3SelectPrep`、检查 UPDATE...FROM 的目标表名冲突（`SF_UFSrcCheck`）、在 `SRT_Output` 目的地下调用 `sqlite3GenerateColumnNames` 定下结果列名、窗口函数改写（`sqlite3WindowRewrite`）、FROM 子句级优化（外连接强度削弱、子查询扁平化）、复合 SELECT 转交 `multiSelect`、EXISTS 转连接、WHERE 常量传播，然后进入 FROM 子句代码生成与聚合/非聚合两条主路径。

`sqlite3Select` 的返回值语义是 `rc = (pParse->nErr>0)`：返回 0 表示编译成功，非 0 表示解析上下文中已记录错误。函数体内所有出错路径统一 `goto select_end` 汇合，错误信息由各处 `sqlite3ErrorMsg` 写入 `pParse`。

## 怎么实现的

1. **入口与前置检查**：`sqlite3Select` 取 `pParse->db`，通过 `sqlite3GetVdbe` 拿到待构造的 Vdbe。若 `p==0` 或 `pParse->nErr` 非零直接返回 1；否则先做 `sqlite3AuthCheck(pParse, SQLITE_SELECT, 0, 0, 0)` 授权检查，失败即返回 1。

2. **ORDER BY 与 DISTINCT 的先行裁剪**：当 `IgnorableDistinct(pDest)` 为真时（目的地为 `SRT_Exists`、`SRT_Discard`、`SRT_DistQueue`、`SRT_DistFifo`），若存在 `p->pOrderBy` 就用 `sqlite3ParserAddCleanup(pParse, sqlite3ExprListDeleteGeneric, p->pOrderBy)` 把它从树上摘掉并置 `p->pOrderBy = 0`。DISTINCT 的清除带一个例外：目的地为 `SRT_Exists` 且 `p->pLimit` 非空而 `p->pLimit->pRight==0` 时不清除，因为此时 OFFSET 必须作用在去重之后。

3. **准备阶段**：`sqlite3SelectPrep(pParse, p, 0)`。`sqlite3SelectPrep` 依次调用 `sqlite3SelectExpand`、`sqlite3ResolveSelectNames`、`sqlite3SelectAddTypeInfo`，每步之后检查 `pParse->nErr`，出错即返回。`sqlite3SelectExpand` 内部的 `selectExpander` 会调用 `sqlite3SrcListAssignCursors` 给 FROM 子句各项分配 VDBE 游标号，为 FROM 中的子查询构造临时 `Table` 结构，并展开 `*` 与 `TABLE.*`。

4. **结果列名**：`pDest->eDest==SRT_Output` 时调用 `sqlite3GenerateColumnNames(pParse, p)`。该函数先 `sqlite3VdbeSetNumCols(v, pEList->nExpr)` 设定结果列数，再逐列决定名字：`AS` 子句给出的名字优先级最高（`pEList->a[i].zEName` 且 `fg.eEName==ENAME_NAME`）；否则当 `srcName` 为真且表达式是 `TK_COLUMN` 时取源表列名，`iColumn<0` 时改用 `pTab->iPKey` 指向的行键，若连行键列号也小于 0 则名字为 `"rowid"`；`fullName` 为真时名字拼成 `"表名.列名"`；其余情况回退到 `sqlite3MPrintf(db, "column%d", i+1)`。函数以 `generateColumnTypes(pParse, pTabList, pEList)` 收尾补列类型。复合 SELECT 的列名由最左分支决定，函数开头 `while( pSelect->pPrior ) pSelect = pSelect->pPrior;` 完成这一跳转。函数还受 `pParse->colNamesSet` 保护，重复调用直接返回。

5. **FROM 子句优化与子查询代码生成**：`sqlite3Select` 遍历 `pTabList->nSrc` 每一项。外连接强度削弱的条件是 `pItem->fg.jointype` 含 `JT_LEFT` 或 `JT_LTORJ`，且 `sqlite3ExprImpliesNonNullRow` 判定该表不可能是连接的 NULL 行，且 `OptimizationEnabled(db, SQLITE_SimplifyJoin)` 成立——此时按 `FULL JOIN → RIGHT JOIN`、`LEFT JOIN → JOIN`、`RIGHT JOIN → JOIN` 逐级改写 `pItem->fg.jointype`。对 FROM 中的子查询，先检查视图声明列数与子查询列数是否一致（`pTab->nCol != pSub->pEList->nExpr` 时报 `expected %d columns for '%s' but got %d`），再依次跳过 `MATERIALIZED` CTE（`pItem->u2.pCteUse->eM10d==M10d_Yes`）和聚合子查询（`pSub->selFlags & SF_Aggregate`），随后判断是否删除无意义的子查询 ORDER BY（条件见下），最后调用 `flattenSubquery(pParse, p, i, isAgg)` 尝试把子查询并入父查询。子查询的代码生成有四条互斥路径：`fromClauseTermCanBeCoroutine` 为真时走协程（`OP_InitCoroutine` + `sqlite3SelectDestInit(&dest, SRT_Coroutine, ...)` + `sqlite3VdbeEndCoroutine`）；CTE 已生成物化代码时走 `OP_Once` + `OP_Gosub` + `OP_OpenDup`；`isSelfJoinView` 命中时复用前一项已物化的视图（`OP_Gosub` + `OP_OpenDup`）；否则走物化路径（`OP_Goto` 跳到 `OP_Return` 之后，`sqlite3SelectDestInit(&dest, SRT_EphemTab, pItem->iCursor)`）。

6. **非聚合路径**：当 `!isAgg && pGroupBy==0` 时，先按需 `OP_OpenEphemeral` 开一个 DISTINCT 临时表（`sqlite3VdbeChangeP5(v, BTREE_UNORDERED)`，`sDistinct.eTnctType = WHERE_DISTINCT_UNORDERED`），再调用 `sqlite3WhereBegin(pParse, pTabList, pWhere, sSort.pOrderBy, p->pEList, p, wctrlFlags, p->nSelectRow)` 交给 WHERE 层选索引，`pWInfo==0` 即出错退出。计划器回报的输出行数 `sqlite3WhereOutputRowCount(pWInfo)` 若小于 `p->nSelectRow` 就用前者覆盖；目的地位于 `SRT_DistQueue` 到 `SRT_DistFifo` 之间时再减去 `Tuning(30,1)`（UNION 隐含 DISTINCT 的代价折扣）。若计划器已能按 ORDER BY 顺序出数（`sqlite3WhereIsOrdered(pWInfo) == sSort.pOrderBy->nExpr`），就把 `sSort.pOrderBy` 置 0 并用 `sqlite3VdbeChangeToNoop` 把之前开的排序临时表改成空操作。最后 `selectInnerLoop` 生成内层循环，`sqlite3WhereEnd(pWInfo)` 收尾。

7. **聚合路径**：当存在聚合函数或 GROUP BY 时进入 `else` 分支。若有 `pGroupBy`，先把结果集与 GROUP BY 之间的别名清零（`pItem->u.x.iAlias = 0`），并把 `p->nSelectRow` 压到 66——`assert( 66==sqlite3LogEst(100) )` 说明这是 LogEst 刻度下 100 行对应的估值上限。随后构造 `AggInfo`，设置 `pAggInfo->nSortingColumn` 与 `pAggInfo->pGroupBy`，用 `sqlite3ExprAnalyzeAggList` 分析结果列表与排序列表中的聚合函数，`sqlite3ExprAnalyzeAggregates` 分析 HAVING。若 GROUP BY 与 ORDER BY 列表相同（`sqlite3CopySortOrder` 成立且 `sqlite3ExprListCompare` 返回 0），置 `orderByGrp = 1`。有 GROUP BY 时先 `OP_SorterOpen` 开排序器，随后 `sqlite3WhereBegin` 传入 `WHERE_GROUPBY`（`sDistinct.isTnct==2` 时改传 `WHERE_DISTINCTBY`）与可选的 `WHERE_SORTBYGROUP`；若计划器不能直接按组序出数（`sqlite3WhereIsOrdered(pWInfo) != pGroupBy->nExpr`），走 `OP_SorterInsert` + `OP_SorterSort` 的排序路径，用 `OP_Compare` 比较前后两组键值，组变化时 `OP_Gosub` 到输出子程序并 `OP_Gosub` 到 `resetAccumulator` 重置累加器。无 GROUP BY 时分两种：`isSimpleCount` 命中 `SELECT count(*) FROM <tbl>` 形态时直接发 `OP_OpenRead` + `OP_Count` + `OP_Close`，并在其中挑选 `bUnordered==0 && szIdxRow<pTab->szTabRow && pPartIdxWhere==0` 的最小索引；否则走通用累加器路径，`sqlite3WhereBegin` 传入 `minMaxFlag|distFlag`，`updateAccumulator` 生成累加代码，`finalizeAggFunctions` 收尾。

8. **排序与收尾**：若 `sSort.pOrderBy` 仍非空，`generateSortTail` 生成排序输出循环。最后 `sqlite3VdbeResolveLabel(v, iEnd)` 闭合 LIMIT 跳转目标，`rc = (pParse->nErr>0)` 决定返回值。

9. **错误与清理**：所有失败路径跳到 `select_end`，其中 `sqlite3ExprListDelete(db, pMinMaxOrderBy)` 释放 min/max 优化期间追加的 ORDER BY 列表。`sqlite3SelectPrep` 内的 `sqlite3SelectExpand` 在 `pParse->nErr` 时立即中止后续步骤。

## 关键取舍

- **源码事实：**`sqlite3SelectPrep` 用 `SF_HasTypeInfo` 位做一次性检查，展开、名字解析、类型补齐三步严格按此顺序，且每步后立即检查 `pParse->nErr`。**架构推论：**展开必须先于名字解析，因为解析需要知道通配符展开后的实际列集合；而类型补齐必须最后做，因为子查询结果集的列类型依赖已经解析完成的标识符。顺序反了会导致列数或类型信息与最终结果集不匹配。

- **源码事实：**`sqlite3Select` 返回 `rc = (pParse->nErr>0)` 而不是直接返回 `SQLITE_OK`/`SQLITE_ERROR`，错误详情由 `sqlite3ErrorMsg` 写入 `pParse`。**架构推论：**错误集中在 `pParse` 一处，调用方只需在编译结束后统一检查 `pParse->nErr` 与 `db->mallocFailed`，不必逐层传递错误码；代价是错误发生点与错误消息分离。

- **源码事实：**编译期同时生成排序索引和 DISTINCT 索引，随后按计划器反馈用 `sqlite3VdbeChangeToNoop` / `sqlite3VdbeChangeOpcode` 把不再需要的指令改掉（`OP_OpenEphemeral` 改成 `OP_Noop`，或在 `p->iLimit==0` 时改成 `OP_SorterOpen` 并置 `SORTFLAG_UseSorter`）。**架构推论：**先无条件生成再回头裁剪，避免了在生成排序代码之前就确定计划形态所必需的额外往返；代价是生成阶段要短暂持有最终不会使用的游标号。

- **源码事实：**删除 FROM 子句子查询的 ORDER BY 需同时满足六个条件：外层有 ORDER BY 或 FROM 项数大于 1、子查询无 LIMIT、子查询无 `SF_OrderByReqd`/`SF_Recursive`、外层无 `SF_OrderByReqd`、且 `OptimizationEnabled(db, SQLITE_OmitOrderBy)` 成立。**架构推论：**删掉子查询的 ORDER BY 会改变行的产生顺序，进而改变外层 LIMIT/聚合函数的输入顺序，因此只在能证明外层顺序由别处保证时才删除。

- **源码事实：**聚合路径把 `p->nSelectRow` 压到 66，且该值由 `assert( 66==sqlite3LogEst(100) )` 锚定。**架构推论：**聚合查询的输出行数上限等于分组数，用 100 行对应的 LogEst 估值作为上界，是为了让代价模型在"行数极多"时不再区分 100 行和更多行，避免分组数无界增长拖慢估算。

- **源码事实：**`sqlite3GenerateColumnNames` 对 `AS` 子句名字的优先级最高，`SQLITE_FullColNames` 决定是否拼成 `"表名.列名"`，而 `SQLITE_ShortColNames` 与 `fullName` 任一成立都会让 `srcName` 为真。**架构推论：**这三条开关互相独立地覆盖了"别名优先""短名""全名"三种历史行为；函数注释明确写道 SQLite 对列名唯一的正式保证只有 `AS` 子句这一条，其余是为了不破坏历史应用而保留的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/select.c` | `sqlite3SelectPrep` | SELECT 准备层：依次做展开、名字解析、类型补齐，受 `SF_HasTypeInfo` 幂等保护 |
| `src/select.c` | `sqlite3Select` | SELECT 代码生成层：分配游标、FROM 子句优化与子查询代码生成、WHERE 委托、聚合与排序分支、错误汇合到 `select_end` |
| `src/select.c` | `sqlite3GenerateColumnNames` | 决定结果列数与每列的名字（`AS` 优先、源表列名、`表名.列名`、回退 `column%d`），并触发列类型生成 |

## 与主核 / AI 的关系

本页描述的是 SQLite 自身的查询编译器，与主核（`engine/db/` 自研多模态数据库内核）的查询层只在"从解析后的语法树生成可执行计划"这一抽象形态上对应：两边都把规划分成"语义准备"与"代码生成"两个阶段，但主核不生成 VDBE 字节码，而是构造自己的算子节点树。AI 侧没有直接对应实现；本页可作为 AI 理解 SQLite "语义准备与代码生成分离"这一设计模式的材料。

## 参考

- [SQLite 官方文档：SELECT 语句](https://sqlite.org/lang_select.html) —— SELECT 语法与 DISTINCT / GROUP BY / ORDER BY / LIMIT 的标准语义
- [SQLite 官方文档：查询优化器概览](https://sqlite.org/optoverview.html) —— 规划器如何挑选索引、JOIN 顺序与代价估算
- [SQLite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c) —— 本页全部符号的实现文件
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的整体分层
