---
project: sqlite
feature: expression-tree
page_type: feature
schema_version: 1
refs:
  - path: src/expr.c
    symbols: [sqlite3ExprCodeTarget, sqlite3ExprAffinity, sqlite3ExprAnalyzeAggregates]
---

# 表达式树

## 一句话职责

表达式树是 SQLite 内部表示 SQL 表达式（WHERE 条件、列值、函数调用、聚合调用）的树形数据结构 `Expr`，配套 `Walker` 遍历器提供统一的树遍历入口，供名称解析、亲和性判定、聚合分析与字节码生成共用。

## 机制是什么

**边界（来自条目定义）：** 负责表达式的树形表示、节点构造与遍历 walker 的注册；不负责把表达式变成字节码（C02），不负责列存表与类型的判定（C03）。

本篇聚焦三件事：

1. **树形表示**：`struct Expr` 定义在 `src/sqliteInt.h`，用 `op` 字段区分节点种类，用 `pLeft` / `pRight` 表达二叉结构，用 `x.pList` 表达变长子节点列表。
2. **遍历 walker 的注册**：`struct Walker` 定义在 `src/sqliteInt.h`，通过 `xExprCallback` / `xSelectCallback` / `xSelectCallback2` 三个回调把「遍历行为」与「遍历实现」分离。
3. **节点上的语义判定**：亲和性由 `sqlite3ExprAffinity` 给出，聚合函数位置由 `sqlite3ExprAnalyzeAggregates` 分析并登记到 `AggInfo`。

按条目边界，本篇**不覆盖**：`sqlite3ExprCodeTarget` 把树翻译成 VDBE 操作码序列的全过程属于「表达式代码生成」（C02）——本篇只说明它在表达式树上的入口契约（目标寄存器与回退约定），不展开操作码细节；「列存表与类型的判定」属于 C03。

### 节点结构

`struct Expr`（`src/sqliteInt.h`）的关键字段：

| 字段 | 含义（源码注释原文口径） |
|---|---|
| `op` | 本节点执行的操作 |
| `affExpr` | 亲和性，或 RAISE 类型 |
| `op2` | `TK_REGISTER`/`TK_TRUTH` 时是原始的 `Expr.op`；`TK_COLUMN` 时是 `OP_Column` 的 p5；`TK_AGG_FUNCTION` 时是嵌套深度；`TK_FUNCTION` 时是 `NC_SelfRef` 标志 |
| `flags` | 各种 `EP_*` 标志位 |
| `u` | 联合体：`zToken` 是零结尾且已去引号的记号值；`u.iValue` 在 `EP_IntValue` 置位时是非负整数值 |
| `pLeft` / `pRight` | 左/右子节点 |
| `x` | 联合体：`x.pList` 用于 `op` 为 `IN, EXISTS, SELECT, CASE, FUNCTION, BETWEEN`；`x.pSelect` 在 `EP_xIsSelect` 置位且 `op` 为 `IN, EXISTS, SELECT` 时有效 |
| `iTable` | 多义字段：`TK_COLUMN` 时是持有该列的表游标号；`TK_REGISTER` 时是寄存器号；`TK_TRIGGER` 时 1 表示 new.\*、0 表示 old.\*；`TK_SELECT_COLUMN` 时是左侧结果向量的列数 |
| `iColumn` | `TK_COLUMN` 时是列索引，-1 表示 rowid |
| `iAgg` | 指向 `pAggInfo->aCol[]` 或 `->aFunc[]` 的哪一项 |
| `pAggInfo` | 仅 `TK_AGG_COLUMN` 与 `TK_AGG_FUNCTION` 使用 |
| `y` | 联合体：`y.pTab` 是含该列的 `Table`（表达式索引的列可以为 NULL）；`y.pWin` 是 `EP_WinFunc` 的窗口/过滤定义；`y.sub.iAddr` 与 `y.sub.regReturn` 是 `TK_IN`/`TK_SELECT`/`TK_EXISTS` 的子程序入口地址与返回地址寄存器 |

源码用两个「截断大小」宏把 `Expr` 做成可变长度结构：`EXPR_REDUCEDSIZE` 定义为 `offsetof(Expr,iTable)`，`EXPR_TOKENONLYSIZE` 定义为 `offsetof(Expr,pLeft)`。当 `flags` 里 `EP_Reduced` 置位时只分配 `EXPR_REDUCEDSIZE` 字节，`EP_TokenOnly` 置位时只分配 `EXPR_TOKENONLYSIZE` 字节——访问被截断掉的字段会段错误。判定用宏 `ExprHasProperty(E,P)`（`((E)->flags&(u32)(P))!=0`）。

字段的多义性靠 `ExprUse*` 系列宏区分：`ExprUseUToken` 判 `EP_IntValue` 是否未置位，`ExprUseXList` 判 `EP_xIsSelect` 是否未置位，`ExprUseXSelect` 判 `EP_xIsSelect` 是否置位，`ExprUseYTab` 判 `EP_WinFunc|EP_Subrtn` 是否都未置位。

### 遍历器

`struct Walker`（`src/sqliteInt.h`）带三个回调指针与一个上下文联合体：

- `xExprCallback(Walker*, Expr*)`：每遇到一个表达式节点调用一次。
- `xSelectCallback(Walker*, Select*)`：进入一个 `Select` 时调用，返回 `WRC_Prune`/`WRC_Abort` 可剪枝或中止。
- `xSelectCallback2(Walker*, Select*)`：离开 `Select` 时调用，类型是 `void`。
- `walkerDepth`：当前子查询嵌套深度，由 `sqlite3WalkerDepthIncrease` / `sqlite3WalkerDepthDecrease` 维护。
- `eCode`：一个 `u16` 的小处理码，多个常量判定函数靠它区分「要找哪种常量」。
- `u`：回调用的额外数据，`src/sqliteInt.h` 里列了 21 个成员，其中 `u.pNC` 存 `NameContext`、`u.pGroupBy` 存 GROUP BY 子句、`u.pSelect` 存 HAVING→WHERE 的上下文。

遍历实现在 `src/walker.c`：`sqlite3WalkExpr` 是入口（`pExpr` 为 NULL 时直接返回 `WRC_Continue`），实际递归在 `sqlite3WalkExprNN`；`sqlite3WalkExprList` 遍历 `ExprList`；`sqlite3WalkSelectExpr` 遍历 `Select` 的各表达式子句（`pEList`、`pWhere`、`pGroupBy`、`pHaving`、`pOrderBy`、`pLimit`）；`sqlite3WalkSelectFrom` 遍历 FROM 子句里的子查询与表函数参数；`sqlite3WalkSelect` 串起这些并沿 `p->pPrior` 处理复合 SELECT。

## 怎么实现的

### 1. `sqlite3ExprAffinity`：沿树向下找出表达式的亲和性

`src/expr.c` 的 `sqlite3ExprAffinity` 是一个 `while(1)` 循环，循环变量 `op` 初始为 `pExpr->op`，按下列顺序判定：

1. `op==TK_COLUMN`，或 `op==TK_AGG_COLUMN && pExpr->y.pTab!=0`：调 `sqlite3TableColumnAffinity(pExpr->y.pTab, pExpr->iColumn)` 返回该列的亲和性字符。`sqlite3TableColumnAffinity` 在列索引越界时（`iCol<0` 或 `iCol>=pTab->nCol`）返回 `SQLITE_AFF_INTEGER`。
2. `op==TK_SELECT`：递归取 `pExpr->x.pSelect->pEList->a[0].pExpr` 的亲和性，即子查询第一列的亲和性。
3. `op==TK_CAST`（`#ifndef SQLITE_OMIT_CAST`）：调 `sqlite3AffinityType(pExpr->u.zToken, 0)`，第二个参数传 NULL 表示不涉及列的默认亲和性。
4. `op==TK_SELECT_COLUMN`：递归取 `pExpr->pLeft->x.pSelect->pEList->a[pExpr->iColumn].pExpr` 的亲和性。
5. `op==TK_VECTOR`：递归取 `pExpr->x.pList->a[0].pExpr` 的亲和性。
6. `op==TK_FUNCTION && pExpr->affExpr==SQLITE_AFF_DEFER`：取第一个实参 `pExpr->x.pList->a[0].pExpr` 的亲和性，但若结果是 `SQLITE_AFF_REAL` 则改返回 `SQLITE_AFF_NUMERIC`。源码注释说明这是 FULL JOIN 的 USING 列场景：`coalesce(L.x, R.x)` 用 L.x 的亲和性，但值可能来自 R.x，所以 REAL 降级为 NUMERIC。
7. `ExprHasProperty(pExpr, EP_Skip|EP_IfNullRow)`：断言此时 `op` 必为 `TK_COLLATE`、`TK_IF_NULL_ROW` 或 `TK_REGISTER` 且 `op2==TK_IF_NULL_ROW`；把 `pExpr` 换成 `pExpr->pLeft`，`op` 换成新节点的 `op`，`continue` 重新判定。
8. `op==TK_REGISTER`：把 `op` 换成 `pExpr->op2` 继续循环；若 `NEVER(op==TK_REGISTER)` 则跳出。

都不匹配时返回 `pExpr->affExpr`，也就是表达式节点自带的亲和性；无亲和性时该值为 0x00。

### 2. `sqlite3ExprAnalyzeAggregates`：注册聚合信息并做 walker 回调

`sqlite3ExprAnalyzeAggregates(NameContext *pNC, Expr *pExpr)` 的实现只有七行，全部工作交给一个 `Walker`：

```
w.xExprCallback = analyzeAggregate;
w.xSelectCallback = sqlite3WalkerDepthIncrease;
w.xSelectCallback2 = sqlite3WalkerDepthDecrease;
w.walkerDepth = 0;
w.u.pNC = pNC;
w.pParse = 0;
sqlite3WalkExpr(&w, pExpr);
```

用 `sqlite3WalkerDepthIncrease`/`sqlite3WalkerDepthDecrease` 作 SELECT 回调，意味着遍历进入子查询时 `walkerDepth` 加一、离开时减一。函数前置断言 `pNC->pSrcList!=0`，且源码注释写明它必须在 `sqlite3ResolveExprNames()` 之后调用。

真正的活儿在静态回调 `analyzeAggregate`（`src/expr.c`）里，按 `pExpr->op` 分派：

- **`TK_AGG_COLUMN` / `TK_COLUMN` / `TK_IF_NULL_ROW`**：遍历 `pNC->pSrcList` 的每个 `SrcItem`，若 `pExpr->iTable==pItem->iCursor` 则调 `findOrCreateAggInfoColumn(pParse, pAggInfo, pExpr)`，然后返回 `WRC_Continue`。
- **`TK_AGG_FUNCTION`**：当 `(pNC->ncFlags & NC_InAggFunc)==0 && pWalker->walkerDepth==pExpr->op2 && pExpr->pAggInfo==0` 时，先在 `pAggInfo->aFunc[]` 里查重——用 `sqlite3ExprCompare(0, pItem->pFExpr, pExpr, -1)==0` 判等。查重的循环上界取 `mxTerm = pParse->db->aLimit[SQLITE_LIMIT_COLUMN]`。若 `i>mxTerm` 则报 `"more than %d aggregate terms"` 并把 `i` 夹到 `mxTerm`；否则若 `i>=pAggInfo->nFunc` 则调 `addAggInfoFunc` 新建一项，并填 `pFExpr`、`pFunc`（由 `sqlite3FindFunction(db, pExpr->u.zToken, nArg, enc, 0)` 查出，`nArg` 取 `pExpr->x.pList->nExpr`，无参时为 0）、`iOBTab`、`bOBPayload`、`bOBUnique`、`bUseSubtype`、`iDistinct`。最后设 `pExpr->iAgg = (i16)i`、`pExpr->pAggInfo = pAggInfo`，并置 `EP_NoReduce` 属性，返回 `WRC_Prune`（不再下钻这个子树）。若条件不满足则返回 `WRC_Continue`。
- **默认分支**：处理「表达式可被替换成索引列读取」的重写。只有在 `(pNC->ncFlags & NC_InAggFunc)!=0`、`pParse->pIdxEpr!=0`、`pWalker->walkerDepth==0` 时才继续；遍历 `pParse->pIdxEpr` 链，对每个 `IndexedExpr` 跳过 `iDataCur<0` 与 `bMaybeNullRow` 的项，用 `indexedExprCompare(pIEpr, pExpr, iDataCur)==0` 匹配。匹配成功后构造一个临时 `Expr`（`op=TK_AGG_COLUMN`、`iTable=pIEpr->iIdxCur`、`iColumn=pIEpr->iIdxCol`），调 `findOrCreateAggInfoColumn`，再把 `pAggInfo->aCol[tmp.iAgg].pCExpr` 指向原表达式，设 `pExpr->pAggInfo` 与 `pExpr->iAgg`，返回 `WRC_Prune`。源码注释记录了一个具体缺陷编号：`break; /* See bug report 2026-11-07T18:33:11Z */`——即嵌套深度大于 0 时不做该重写。

**聚合项数量上限的来源**：`findOrCreateAggInfoColumn` 与 `analyzeAggregate` 都用 `mxTerm = pParse->db->aLimit[SQLITE_LIMIT_COLUMN]`。这个运行时上限的硬上界来自 `src/sqliteLimit.h` 的 `SQLITE_MAX_COLUMN`（默认 2000，`#elif SQLITE_MAX_COLUMN>32767` 时 `#error`），`src/main.c` 的 `aHardLimit[]` 数组把 `SQLITE_MAX_COLUMN` 放在 `SQLITE_LIMIT_COLUMN` 对应的位置，并有 `assert( aHardLimit[SQLITE_LIMIT_COLUMN]==SQLITE_MAX_COLUMN )`。

### 3. `sqlite3ExprCodeTarget`：表达式树进入代码生成阶段的入口契约

按本篇边界，字节码生成本身属 C02；这里说明它在表达式树上的**契约**，因为它决定了表达式树的形状约束。

`sqlite3ExprCodeTarget(Parse *pParse, Expr *pExpr, int target)` 的返回约定写在函数头注释里：不保证结果一定落在 `target` 寄存器，可能落在别的寄存器，**调用方必须检查返回值并把结果搬到目标寄存器**。落地这件事由 `sqlite3ExprCode` 完成——它调 `sqlite3ExprCodeTarget` 拿到 `inReg`，若 `inReg!=target` 则补一条 `OP_Copy` 或 `OP_SCopy`（当表达式带 `EP_Subquery` 或 `op==TK_REGISTER` 时用 `OP_Copy`，否则用 `OP_SCopy`）把它搬到 `target`。

函数体内的几处约束值得记：

- 入口断言 `target>0 && target<=pParse->nMem`。
- `pExpr==0` 时按 `TK_NULL` 处理。
- 当 `pParse->pIdxEpr!=0` 且表达式非 `EP_Leaf` 时，先试 `sqlite3IndexedExprLookup(pParse, pExpr, target)`，命中则直接返回该寄存器号。
- 函数体开头有 `expr_code_doover:` 标签，`TK_COLLATE`（带 `EP_Collate`）、`TK_SPAN`、`TK_UPLUS` 三种情况把 `pExpr` 换到左子节点后 `goto expr_code_doover`，源码注释写明动机是 `Prevent deep recursion`，其中 `TK_SPAN`/`TK_UPLUS` 的注释还标了 `OSSFuzz`——这是为了避免畸形输入把表达式树撑成深链导致栈溢出。
- 尾部的 `TK_RAISE` 分支：若 `pParse->pTriggerTab==0` 则报 `"RAISE() may only be used within a trigger-program"` 并返回 0。
- 函数结尾统一 `sqlite3ReleaseTempReg(pParse, regFree1)` 与 `sqlite3ReleaseTempReg(pParse, regFree2)` 归还临时寄存器，返回 `inReg`。

### 4. 遍历器在表达式树上的其他注册点

`src/expr.c` 里还有多处用同一套 `Walker` 机制做树遍历，可作为「walker 注册」这一职责的旁证：

- `exprNodeIsConstant`（配 `exprNodeIsConstantFunction`）：判定表达式是否常量。`src/expr.c` 的注释明确列出 `Walker.eCode` 的取值约定：`eCode==1` 对应 `sqlite3ExprIsConstant`，`eCode==2` 对应 `sqlite3ExprIsConstantNotJoin`，`eCode==3` 对应 `sqlite3ExprIsConstantOrFunction` 系列的表常量判定，`eCode==4 or 5` 对应 `sqlite3ExprIsConstantOrFunction`，`eCode==1 or 6` 对应 `sqlite3ExprListIsConstant`。回调把 `eCode` 置 0 即表示「不是常量」。
- `exprNodeIsConstantOrGroupBy`：判定表达式是否常量或可由 GROUP BY 项满足。
- `exprNodeCanReturnSubtype`：判定表达式能否返回 subtype，命中时置 `pWalker->eCode=1`。
- `impliesNotNullRow` / `bothImplyNotNullRow`：判定表达式是否隐含「该行的列非 NULL」。
- `exprIdxCover`：判定表达式是否被某个索引覆盖。

## 关键取舍

- **源码事实：**`sqlite3ExprAffinity` 对 `TK_FUNCTION && affExpr==SQLITE_AFF_DEFER` 的处理是把 `SQLITE_AFF_REAL` 降级为 `SQLITE_AFF_NUMERIC`，其余亲和性原样保留。**架构推论：**FULL JOIN 的 USING 列用 `coalesce(L.x, R.x)` 承载，值可能来自右侧，REAL 亲和性会强制把整数变成浮点，若沿用 L.x 的 REAL 会引入不必要的有损转换；降级为 NUMERIC 保留了「不主动变浮点」的语义，代价是该列在部分场景下不再满足「严格 REAL 亲和性」的直觉。

- **源码事实：**`sqlite3ExprCodeTarget` 允许结果落在非 `target` 的寄存器，由 `sqlite3ExprCode` 检查返回值后补 `OP_Copy`/`OP_SCopy` 搬运。**架构推论：**这让代码生成器可以在已有寄存器恰好持有该值时直接复用，省掉一次搬运指令；代价是所有调用方都必须处理「返回值不等于请求寄存器」的情况，接口本身不保证寄存器位置，调用方写错就会读到过期寄存器。

- **源码事实：**`TK_COLLATE`、`TK_SPAN`、`TK_UPLUS` 用 `goto expr_code_doover` 而非递归，注释标注 `Prevent deep recursion` 与 `OSSFuzz`。**架构推论：**SQLite 把表达式树深度限制交给 `SQLITE_MAX_EXPR_DEPTH`（`src/sqliteLimit.h` 默认 1000）来兜底，但代码生成阶段改用迭代跳转来消除深链场景下的栈风险；代价是这几个 case 的控制流不是常规的「递归返回」结构，读代码时需要顺着标签跳转才能跟完。

- **源码事实：**`Expr` 用 `EP_TokenOnly`/`EP_Reduced` 配合 `EXPR_TOKENONLYSIZE`/`EXPR_REDUCEDSIZE` 做变长结构，截断后的字段访问会段错误。**架构推论：**把「只用到记号」的表达式（常量、列名）压缩到只够放记号的大小，可以显著降低模式表与常量子树的内存占用；代价是失去类型安全，误访问被截断字段不是返回错误而是直接崩溃。

- **源码事实：**`sqlite3ExprAnalyzeAggregates` 用 `sqlite3WalkerDepthIncrease`/`sqlite3WalkerDepthDecrease` 维护 `walkerDepth`，且 `analyzeAggregate` 的默认分支在 `pWalker->walkerDepth>0` 时不把表达式重写成索引列读取，源码标注 `See bug report 2026-11-07T18:33:11Z`。**架构推论：**用深度计数而不是布尔标志来区分「顶层聚合」与「嵌套聚合」，让「聚合套聚合」这种非法嵌套能被识别并走到 `TK_AGG_FUNCTION` 分支的错误路径；代价是所有需要感知嵌套层数的分析都得挂一个深度计数的 walker 回调。

- **源码事实：**`sqlite3ExprAnalyzeAggregates` 的实现把全部遍历工作交给 `analyzeAggregate` 回调，`Walker` 的 `u` 联合体只填了 `pNC`，`pParse` 显式置 0。**架构推论：**把「遍历算法」与「节点判定逻辑」彻底分开，使得同一个 `sqlite3WalkExpr` 能被常量判定、subtype 判定、索引覆盖判定、聚合分析等多种分析复用；代价是每次分析都要重新组装一个 `Walker` 并填一遍回调，调用方（`src/select.c` 中的 `sqlite3ExprAnalyzeAggList` 与 `sqlite3ExprAnalyzeAggregates` 调用点）需要自己管理上下文。

- **源码事实：**聚合项数量上限取自运行时 `pParse->db->aLimit[SQLITE_LIMIT_COLUMN]`，其硬上界是 `SQLITE_MAX_COLUMN`（默认 2000）。**架构推论：**把上限做成可由 `sqlite3_limit()` 调节的运行时限制而非写死的编译期常量，让应用能按自身资源调整；代价是同一个 SQL 在不同连接配置下可能得到不同的报错行为。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/expr.c` | `sqlite3ExprCodeTarget` | 把表达式树翻译成 VDBE 代码的入口；返回实际持有结果的寄存器号，调用方须自行搬到目标寄存器。`TK_COLLATE`/`TK_SPAN`/`TK_UPLUS` 走 `goto expr_code_doover` 避免深递归。 |
| `src/expr.c` | `sqlite3ExprAffinity` | 沿树向下判定表达式亲和性：处理 `TK_COLUMN`、`TK_AGG_COLUMN`、`TK_SELECT`、`TK_CAST`、`TK_SELECT_COLUMN`、`TK_VECTOR`、`SQLITE_AFF_DEFER` 函数、`EP_Skip`/`EP_IfNullRow` 与 `TK_REGISTER`，其余返回 `pExpr->affExpr`。 |
| `src/expr.c` | `sqlite3ExprAnalyzeAggregates` | 组装 `Walker`（回调 `analyzeAggregate`，SELECT 回调 `sqlite3WalkerDepthIncrease`/`sqlite3WalkerDepthDecrease`）并调 `sqlite3WalkExpr`，把聚合信息登记进 `AggInfo`。 |
| `src/expr.c` | `sqlite3TableColumnAffinity` | 返回表列的亲和性字符；列索引越界时返回 `SQLITE_AFF_INTEGER`。 |
| `src/expr.c` | `analyzeAggregate` | `sqlite3ExprAnalyzeAggregates` 的 walker 回调；按 `TK_COLUMN`/`TK_AGG_COLUMN`/`TK_IF_NULL_ROW`/`TK_AGG_FUNCTION` 分派，登记 `AggInfo` 的 `aCol[]` 与 `aFunc[]`，并在顶层把表达式重写为索引列读取。 |
| `src/expr.c` | `findOrCreateAggInfoColumn` | 在 `AggInfo->aCol[]` 中查找或新建列条目；超出 `aLimit[SQLITE_LIMIT_COLUMN]` 时报 `"more than %d aggregate terms"`。 |
| `src/expr.c` | `addAggInfoFunc` | 通过 `sqlite3ArrayAllocate` 为 `AggInfo->aFunc[]` 扩容并返回新项下标，分配失败返回负值。 |
| `src/expr.c` | `sqlite3ExprCode` | `sqlite3ExprCodeTarget` 的包装；返回值与 `target` 不一致时补 `OP_Copy` 或 `OP_SCopy` 搬运。 |
| `src/expr.c` | `sqlite3ExprCompare` | 表达式结构相等判定，被 `analyzeAggregate` 用于聚合函数查重。 |
| `src/expr.c` | `sqlite3ExprDup` | 深拷贝表达式树；`flags` 取 `EXPRDUP_REDUCE` 时产出 `EP_Reduced` 截断版本用于模式表内存表示。 |
| `src/expr.c` | `sqlite3ExprDelete` | 递归释放表达式树。 |
| `src/expr.c` | `sqlite3ExprAnalyzeAggList` | 对 `ExprList` 中每个表达式调 `sqlite3ExprAnalyzeAggregates`。 |
| `src/expr.c` | `exprNodeIsConstant` | 常量判定 walker 回调；按 `Walker.eCode` 区分常量种类，命中非常量时把 `eCode` 置 0 并中止。 |
| `src/expr.c` | `exprNodeCanReturnSubtype` | 判定表达式能否返回 subtype，命中时置 `pWalker->eCode=1`。 |
| `src/expr.c` | `exprIdxCover` | 判定表达式是否被指定索引覆盖，命中时置 `pWalker->eCode=1`。 |
| `src/expr.c` | `impliesNotNullRow` | 判定表达式是否隐含「该行的列非 NULL」。 |
| `src/walker.c` | `sqlite3WalkExpr` | 表达式树遍历入口；`pExpr` 为 NULL 时返回 `WRC_Continue`。 |
| `src/walker.c` | `sqlite3WalkExprNN` | 实际递归遍历：先调 `xExprCallback`，再按 `EP_TokenOnly`/`EP_Leaf` 决定是否下钻 `pLeft`、`pRight`、`x.pSelect`、`x.pList` 与窗口定义。 |
| `src/walker.c` | `sqlite3WalkExprList` | 遍历 `ExprList` 中每个表达式。 |
| `src/walker.c` | `sqlite3WalkSelect` | 遍历 `Select`：先调 `xSelectCallback`，再 `sqlite3WalkSelectExpr` 与 `sqlite3WalkSelectFrom`，最后调 `xSelectCallback2`，并沿 `p->pPrior` 处理复合 SELECT。 |
| `src/walker.c` | `sqlite3WalkSelectExpr` | 遍历 `Select` 的 `pEList`、`pWhere`、`pGroupBy`、`pHaving`、`pOrderBy`、`pLimit` 与 `pWinDefn`。 |
| `src/walker.c` | `sqlite3WalkSelectFrom` | 遍历 FROM 子句中的子查询与表函数参数。 |
| `src/walker.c` | `sqlite3WalkerDepthIncrease` | 进入子查询时 `pWalker->walkerDepth++`，返回 `WRC_Continue`。 |
| `src/walker.c` | `sqlite3WalkerDepthDecrease` | 离开子查询时 `pWalker->walkerDepth--`。 |
| `src/sqliteInt.h` | `struct Expr` | 表达式树节点定义：`op`、`affExpr`、`op2`、`flags`、`u`、`pLeft`/`pRight`、`x`、`iTable`、`iColumn`、`iAgg`、`pAggInfo`、`y`。 |
| `src/sqliteInt.h` | `struct Walker` | 遍历器定义：`xExprCallback`、`xSelectCallback`、`xSelectCallback2`、`walkerDepth`、`eCode`、`mWFlags` 与 21 个成员的上下文联合体 `u`。 |
| `src/sqliteInt.h` | `struct AggInfo` | 聚合信息结构：`aCol[]`（源表用到的列）、`aFunc[]`（聚合函数项）、`nColumn`、`nFunc`、`directMode`、`useSortingIdx`、`pGroupBy`。 |
| `src/sqliteInt.h` | `ExprHasProperty` | `((E)->flags&(u32)(P))!=0`，用于判定 `EP_*` 标志位。 |
| `src/sqliteInt.h` | `ExprUseYTab` | `(flags&(EP_WinFunc|EP_Subrtn))==0`，判定 `y` 联合体当前存的是 `pTab`。 |
| `src/sqliteInt.h` | `EXPR_REDUCEDSIZE` / `EXPR_TOKENONLYSIZE` | 变长 `Expr` 的两种截断大小，分别定义为 `offsetof(Expr,iTable)` 与 `offsetof(Expr,pLeft)`。 |
| `src/sqliteLimit.h` | `SQLITE_MAX_EXPR_DEPTH` | 表达式树深度上限，默认 1000；大于 0 时 `Expr` 才含 `nHeight` 字段。 |
| `src/sqliteLimit.h` | `SQLITE_MAX_COLUMN` | `SQLITE_LIMIT_COLUMN` 的硬上界，默认 2000。 |
| `src/main.c` | `aHardLimit` | 运行时限额表的硬上界数组，`SQLITE_MAX_COLUMN` 对应 `SQLITE_LIMIT_COLUMN`。 |
| `src/build.c` | `sqlite3AffinityType` | 由声明类型串推出亲和性字符，`sqlite3ExprAffinity` 处理 `TK_CAST` 时调用。 |

## 与主核 / AI 的关系

主核（`engine/db/`）的分层纪律把类型系统放在 `common/` 之下、与索引层同级，不允许 `query/` 反向依赖 `modalities/`。本篇对应的「表达式树 + 遍历器」职责在主核中有两个可对照的落点：

- **遍历器与节点判定的分离**：`Walker` 把遍历算法与判定逻辑解耦的做法可直接借鉴到主核的 `common/` 层——主核的 `L0 common/` 本就是所有上层共享的底座，把「树遍历」放这里能避免 `query/` 与 `spi/` 各自实现一套。
- **寄存器/缓冲区的目标契约**：`sqlite3ExprCodeTarget` 返回「结果实际在哪」而非「强行写进指定位置」，再由上层搬运，是一种明确的「生产者不保证位置、消费者负责搬运」的接口风格。主核若在 `L3 query/` 层做表达式求值，可用同一风格把「结果落在哪个 slot」的判定责任分层，避免 `query/` 直接依赖 `modalities/` 的缓冲布局。

**架构推论：**SQLite 的 `Expr` 节点用「变长结构 + 截断字段」压缩内存，靠 `EP_TokenOnly`/`EP_Reduced` 两种形态区分用途；主核是多模态内核，节点除了列引用还要携带模态相关的元数据，变长截断的收益会被模态字段的多样性削弱——**架构推论：**主核更适合保持定长节点、把模态差异下沉到 `modalities/` 层的独立结构，而把「节点形状统一、判定逻辑集中」这条原则保留下来。

## 参考

- [SQLite 官方文档：Datatypes In SQLite](https://sqlite.org/datatype3.html) —— 第 3 节「Type Affinity」给出列亲和性的五条判定规则与「Affinity Of Expressions」的表达式亲和性规则，是 `sqlite3ExprAffinity` 行为的规范口径。
- [SQLite 官方文档：SQL Language Expressions](https://sqlite.org/lang_expr.html) —— SQL 表达式语法（`expr` 产生式），对应 `Expr.op` 的取值范围。
- [SQLite 官方文档：Architecture of SQLite](https://sqlite.org/arch.html) —— 说明 `expr.c` 在代码生成器中的定位，以及表达式树到字节码的转换路径。
- [SQLite 官方源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c) —— 对照 `sqlite3ExprAffinity`、`sqlite3ExprCodeTarget`、`sqlite3ExprAnalyzeAggregates` 与 `analyzeAggregate` 的当前实现。
