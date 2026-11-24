---
project: sqlite
feature: views
page_type: feature
schema_version: 1
refs:
  - path: src/build.c
    symbols: [sqlite3CreateView]
---

# 视图

## 一句话职责

视图（VIEW）是「存下来的 SELECT 文本」：SQLite 把 `CREATE VIEW` 语句原文写进 `sqlite_schema` 表，查询时再把视图名替换成它记录的 SELECT 定义，当作子查询展开执行。

## 机制是什么

视图不是一个有自己页面的对象。它的全部状态是三样东西：

1. **名称**：存在 schema 的 `tblHash` 哈希表里，和普通表共用一张表（`sqlite3FindTable` 能同时找到表和视图）。
2. **类型标记**：`Table.eTabType` 取 `TABTYP_VIEW`，`IsView(X)` 宏即 `(X)->eTabType==TABTYP_VIEW`。视图同时被标记 `TF_NoVisibleRowid`，因此视图上没有 `rowid`。
3. **定义**：`Table.u.view.pSelect` 指向一棵 `Select` 语法树副本，以及 `Table.pCheck` 指向可选的列名列表（在普通表上 `pCheck` 存 CHECK 约束，在视图上被复用来存 `CREATE VIEW v(a,b) AS ...` 里的列名）。

落盘形态：视图在 `sqlite_schema` 里 `type='view'`、`rootpage=0`、`sql` 字段是完整的 `CREATE VIEW ...` 文本。重新打开数据库时，SQLite 读 `sqlite_schema`，把 `sql` 文本重新交给 parser 解析一遍，从而重建视图的 `Select` 树——视图不保存任何已解析的二进制形态。

查询展开：当 `FROM` 子句里出现一个视图名，SQLite 并不为视图单独走一套执行计划，而是把 `pTab->u.view.pSelect` 复制一份，替换掉 FROM 项，改成「FROM (视图的 SELECT)」的形态，之后完全按子查询处理。

## 怎么实现的

### 创建：`sqlite3CreateView`

`src/build.c` 的 `sqlite3CreateView` 由 parser 调用（`src/parse.y` 的 `cmd ::= createkw(X) temp(T) VIEW ifnotexists(E) nm(Y) dbnm(Z) eidlist_opt(C) AS select(S)` 动作）。执行顺序：

1. **参数检查**：`if( pParse->nVar>0 )` 则报 `"parameters are not allowed in views"` 并跳到 `create_view_fail`。视图定义里不允许绑定参数。
2. **登记对象**：`sqlite3StartTable(pParse, pName1, pName2, isTemp, 1, 0, noErr)`，第五个实参 `isView=1`。`sqlite3StartTable` 内部会：解析库名（`sqlite3TwoPartName`）、校验命名空间冲突（同库不能与已有表/视图同名，也不能与已有索引同名，报 `"there is already an index named %s"`）、做授权检查（`SQLITE_CREATE_VIEW` / `SQLITE_CREATE_TEMP_VIEW`）、分配 `Table` 结构放到 `pParse->pNewTable`。
3. **关闭 rowid**：`p->tabFlags |= TF_NoVisibleRowid`。若编译期定义了 `SQLITE_ALLOW_ROWID_IN_VIEW`，改置 `sqlite3Config.mNoVisibleRowid`，即允许通过 `sqlite3_config(SQLITE_CONFIG_ROWID_IN_VIEW, ...)` 恢复旧行为。
4. **库归属绑定**：`sqlite3TwoPartName` 拿到非限定名，`sqlite3SchemaToIndex` 拿到库序号 `iDb`，然后 `sqlite3FixInit(&sFix, pParse, iDb, "view", pName)` + `sqlite3FixSelect(&sFix, pSelect)`。这一步保证视图定义里未限定库名的表引用被绑定到视图自己所在的库；引用了别的库则报错，走 `create_view_fail`。
5. **深拷贝 SELECT**：`pSelect->selFlags |= SF_View`（`SF_View` = `0x0200000`，注释为 "SELECT statement is a view"）；`p->u.view.pSelect = sqlite3SelectDup(db, pSelect, EXPRDUP_REDUCE)`。拷贝是必需的：注释明确说这是为了让所有 `Expr.token.z` 指向动态分配的内存，从而在当前 `sqlite3_exec()` 返回后依然有效。
6. **保存列名列表**：`p->pCheck = sqlite3ExprListDup(db, pCNames, EXPRDUP_REDUCE)`。
7. **打类型标记**：`p->eTabType = TABTYP_VIEW`。
8. **定位语句末尾**：`sEnd = pParse->sLastToken`，向前回退掉尾部空白，把 `sEnd` 定在语句最后一个字符上。
9. **写入 schema**：`sqlite3EndTable(pParse, 0, &sEnd, 0, 0)`。

失败路径统一收敛到 `create_view_fail`：释放 `pSelect`、释放 `pCNames`。

### 落盘：`sqlite3EndTable`

`sqlite3EndTable` 是视图和普通表共用的收尾函数。对视图：

- `IsOrdinaryTable(p)` 为假，所以 `zType = "view"`、`zType2 = "VIEW"`。
- 视图没有自己的 B-tree。`sqlite3StartTable` 对 `isView || isVirtual` 的情况发出的是 `OP_Integer 0, reg2` 而不是 `OP_CreateBtree`，所以视图的 `rootpage` 恒为 0。
- 语句文本按 `"CREATE %s %.*s"` 拼成 `CREATE VIEW <原文>`，原文从 `pParse->sNameToken.z` 起算到 `pEnd->z + pEnd->n`。
- 写盘用的是一条嵌套解析出来的 UPDATE：`UPDATE %Q.sqlite_schema SET type='view', name=%Q, tbl_name=%Q, rootpage=#%d, sql=%Q WHERE rowid=#%d`。
- 随后 `sqlite3ChangeCookie` 递增 schema cookie，`sqlite3VdbeAddParseSchemaOp` 让 VDBE 在语句末尾重新解析 schema。

### 列名的确定：`viewGetColumnNames` / `sqlite3ViewGetColumnNames`

视图的列名不在创建时算出来，而是懒计算的：

- `sqlite3ViewGetColumnNames(pParse, pTable)`：若 `pTable->nCol>0`（列名已知）直接返回 0，否则调 `viewGetColumnNames`。
- `viewGetColumnNames` 把 `pTable->u.view.pSelect` 复制一份（`sqlite3SelectDup(db, pTable->u.view.pSelect, 0)`），在副本上跑 `sqlite3ResultSetOfSelect` 拿到假表 `pSelTab`。用副本是因为 `sqlite3ResultSetOfSelect` 会展开 `*` 并给 FROM 项分配游标，这些副作用不能污染定义。
- 循环检测靠 `pTable->nCol` 的符号：进入时若 `nCol<0` 说明正在计算中，报 `"view %s is circularly defined"`。
- 若视图带显式列名列表（`pTable->pCheck` 非空），列名取自 `sqlite3ColumnsFromExprList(pParse, pTable->pCheck, &pTable->nCol, &pTable->aCol)`，并且只在 `pTable->nCol == pSel->pEList->nExpr` 时才调 `sqlite3SubqueryColumnTypes` 推导类型。
- 若没有显式列名，列名直接来自 SELECT 的结果集：`pTable->nCol = pSelTab->nCol`、`pTable->aCol = pSelTab->aCol`，并把 `pSelTab->tabFlags & COLFLAG_NOINSERT` 搬到视图上。
- 每次算完都置 `pTable->pSchema->schemaFlags |= DB_UnresetViews`。`sqliteViewResetAll` 会遍历该 schema 的 `tblHash`，对每个 `IsView` 的表调 `sqlite3DeleteColumnNames`，然后清掉 `DB_UnresetViews`。

### 查询展开：`selectExpander` 回调

`src/select.c` 的 `selectExpander` 遍历 FROM 子句时，对每个普通表/视图名：

1. `sqlite3LocateTableItem` 找到 `Table`。
2. 若不是普通表（即视图或虚表），先 `sqlite3ViewGetColumnNames(pParse, pTab)` 补齐列名。
3. 若 `IsView(pTab)`：
   - 若 `(db->flags & SQLITE_EnableView)==0` 且该视图不在 temp schema（`pTab->pSchema != db->aDb[1].pSchema`），报 `"access to view \"%s\" prohibited"`。
   - `sqlite3SrcItemAttachSubquery(pParse, pFrom, pTab->u.view.pSelect, 1)`：把视图的 SELECT 复制一份挂成 FROM 项的子查询，`dupSelect=1` 表示复制而非接管所有权。
4. 展开期间把 `pTab->nCol` 临时置为 -1、`pWalker->eCode = 1` 打开 `Select.selId` 重编号，展开完再恢复。

### 重新加载：schema 回放

视图定义靠 `sql` 文本重放。`sqlite3InitCallback`（`src/prepare.c`）从 `sqlite_schema` 读出 `sql` 字段，要求它的头两个字符小写化后是 `"cr"`，然后 `sqlite3Prepare(db, argv[4], -1, 0, 0, &pStmt, &zEnd)` 重新解析。因为此时 `db->init.busy` 为 1，`sqlite3StartTable` 走的是「读 schema」分支：不生成也不执行 VDBE 代码，只重建内部数据结构。

### 删除

`DROP VIEW` 在 `src/parse.y` 里映射到 `sqlite3DropTable(pParse, X, 1, E)`（`isView=1`）。`sqlite3DropTable` 中若 `!isView && IsView(pTab)` 报 `"use DROP VIEW to delete view %s"`；反之 `isView && !IsView(pTab)` 报 `"use DROP TABLE to delete table %s"`。`sqlite3CodeDropTable` 里 `if( !isView && !IsVirtual(pTab) ) destroyTable(...)`——视图不调 `destroyTable`，因为视图没有 btree 页要释放。

## 关键取舍

- **存 SQL 文本，不存语法树**：视图在 `sqlite_schema` 里只留 `CREATE VIEW ...` 原文。每次连接打开都要重新 parse 一遍。好处是文件格式稳定、可以用 `PRAGMA writable_schema` 之类的手段改写；代价是每次打开都要付 parse 成本，且视图定义里的对象名解析推迟到使用时的名称解析阶段。
- **视图展开为子查询，不做独立执行计划**：视图不是物化表，也没有独立的索引与统计信息。查询优化器看到的是展开后的 SELECT，因此视图的 WHERE 条件可以与外层查询一起被优化器重排。这是「视图没有性能开销（也没有性能收益）」的根本原因。
- **`EXPRDUP_REDUCE` 精简拷贝**：`sqlite3CreateView` 里对 SELECT 树和列名列表都用 `EXPRDUP_REDUCE`（值 `0x0001`，注释 "Used reduced-size Expr nodes"）做深拷贝，比完整拷贝省内存；代价是 `EP_NoReduce` 标记的表达式不能被精简。
- **循环检测依赖 `nCol` 的符号编码**：把「正在计算中」编码成 `nCol<0`，复用同一个字段表示三种状态（正数=已知、零=未算、负数=计算中），省掉一个字段但让状态机更隐晦。
- **列名懒计算 + `DB_UnresetViews` 失效标记**：视图列名在第一次被引用时才算，算完置 `DB_UnresetViews`，后续 schema 变更时由 `sqliteViewResetAll` 统一清空所有视图的列名缓存，代价是每次 DDL 之后所有视图列名都要重算。
- **`rowid` 默认禁止**：默认给视图打 `TF_NoVisibleRowid`，因为视图本就没有 rowid。但保留了 `SQLITE_ALLOW_ROWID_IN_VIEW` 编译开关和 `sqlite3_config(SQLITE_CONFIG_ROWID_IN_VIEW, ...)` 运行期开关给遗留应用。
- **视图定义不得含参数**：`sqlite3CreateView` 第一件事就检查 `pParse->nVar>0` 并报错。参数化视图会让同一份 schema 文本在不同绑定下语义不同，与「定义存文本」的设计直接冲突。
- **跨库引用被 `sqlite3FixSelect` 挡住**：`sqlite3FixInit` + `sqlite3FixSelect`（`src/attach.c`）把未限定库名的引用绑定到视图所在库，显式引用别的库则报错——这与触发器、索引使用同一套 `DbFixer` 机制。TEMP 库里的视图不受此限（`src/attach.c` 的注释明确说明 TEMP 库中的 index/trigger/view 允许引用任意对象）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/build.c | sqlite3CreateView | CREATE VIEW 的入口：参数检查、调用 sqlite3StartTable 登记对象、置 TF_NoVisibleRowid、用 sqlite3FixSelect 绑定库归属、深拷贝 SELECT 与列名列表、置 TABTYP_VIEW、调用 sqlite3EndTable 落盘 |
| src/build.c | sqlite3StartTable | 视图与表共用的对象登记：解析库名、命名冲突检查、授权检查、分配 Table 结构、对视图发出 OP_Integer 0 使 rootpage 为 0 |
| src/build.c | sqlite3EndTable | 把视图写入 sqlite_schema：type='view'、rootpage=0、sql=CREATE VIEW 原文，并递增 schema cookie、安排 schema 重解析 |
| src/build.c | viewGetColumnNames | 懒计算视图列名：复制 SELECT、在副本上跑 sqlite3ResultSetOfSelect、用 pCheck 或结果集填 aCol，检测循环定义 |
| src/build.c | sqlite3ViewGetColumnNames | viewGetColumnNames 的对外入口，列名已知时短路返回 |
| src/build.c | sqliteViewResetAll | schema 变更后清空该 schema 内所有视图的列名缓存 |
| src/build.c | sqlite3SrcItemAttachSubquery | 把视图的 SELECT 复制挂成 FROM 项的子查询，实现查询时的展开 |
| src/build.c | sqlite3DropTable | DROP VIEW 的落点：拒绝用 DROP TABLE 删视图、用 DROP VIEW 删表 |
| src/build.c | sqlite3CodeDropTable | 生成删除 schema 行的嵌套 SQL；视图不调用 destroyTable |
| src/select.c | selectExpander | FROM 子句展开：定位视图、校验 SQLITE_EnableView、把视图 SELECT 作为子查询挂上 |
| src/select.c | sqlite3ExpandSubquery | 子查询形式 FROM 项的通用展开，同为视图提供参照实现 |
| src/expr.c | sqlite3SelectDup | 深拷贝 Select 语法树，视图定义与展开都依赖它 |
| src/attach.c | sqlite3FixSelect | 走 DbFixer 检查并绑定视图定义中表引用的库归属 |
| src/parse.y | sqlite3CreateView | 语法动作：CREATE VIEW 规则调用 sqlite3CreateView，DROP VIEW 调用 sqlite3DropTable |
| src/prepare.c | sqlite3InitCallback | 从 sqlite_schema 读回视图的 sql 文本并重新 prepare，重建视图的 Select 树 |
| src/sqliteInt.h | SF_View | 标志位（0x0200000），标记 SELECT 树是一个视图的定义 |
| src/sqliteInt.h | TABTYP_VIEW | 表类型常量（2），IsView 宏据此判定视图 |

## 与主核 / AI 的关系

**架构推论：** 主数据库内核（`engine/db/`）把 catalog 层与 query 层严格分层（L2 catalog 禁依赖 query，L3 query 禁依赖 modalities）。SQLite 的视图机制相当于把「catalog 里存一张虚拟表 + 名字到查询定义的映射」和「query 展开时的替换」耦在了同一处：`sqlite3CreateView` 同时写 schema 行又构造 `Select` 树，`selectExpander` 同时做名称解析与子查询替换。主核若要提供视图能力，view 定义应存在 catalog 层，展开动作放在 query 层，跨层只通过一条「视图名 → 查询定义」的接口。

**架构推论：** SQLite 把视图定义存成 SQL 文本，意味着视图定义可以被 `PRAGMA writable_schema` 直接改写，也意味着主核若照抄这一做法，就必须同时解决「文本回放时重新解析并做名称解析」的成本与安全边界（`sqlite3InitCallback` 只接受以 "cr" 开头的文本，正是为了防止损坏的 schema 触发任意语句）。

**架构推论：** SQLite 的视图不做物化、不做独立索引，所以优化器把视图当子查询处理，外层条件可以下推进视图定义内部。主核的多模态场景里如果要做「视图」语义（例如把向量检索 + 结构化过滤固化成可复用单元），需要显式决定是走 SQLite 式的文本回放+查询改写，还是走物化路径——这两条路的失效模式与一致性保证完全不同。

## 参考

- SQLite 官方文档：CREATE VIEW — https://sqlite.org/lang_createview.html
- SQLite 官方文档：DROP TABLE（含 DROP VIEW 的行为说明）— https://sqlite.org/lang_droptable.html
- GitHub 源码页：src/build.c — https://github.com/sqlite/sqlite/blob/master/src/build.c
- GitHub 源码页：src/select.c — https://github.com/sqlite/sqlite/blob/master/src/select.c
