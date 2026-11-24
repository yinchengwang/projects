---
project: sqlite
feature: subquery-and-in
page_type: feature
schema_version: 1
refs:
  - path: src/expr.c
    symbols: [sqlite3CodeSubselect, sqlite3ExprCodeIN, sqlite3CodeRhsOfIN]
---

# 子查询与 IN 条件

## 一句话职责

把 SQL 里的子查询与 `IN` / `EXISTS` 条件翻译成 VDBE 可执行的字节码：标量子查询与 `EXISTS` 走「子程序 + `LIMIT 1`」路径，`IN` 走「右值物化成临时 B-tree（或复用已有索引）后做二分查找」路径，并在相关子查询下退化为逐行重算。

## 机制是什么

边界（依据条目 `boundary`）：负责把子查询与 IN/EXISTS 条件展开成可执行的代码路径（相关子查询的逐行求值、IN 列表的物化与短路）；不负责外层查询的规划（D 组）、不负责表达式的通用代码生成（C02）。

本机制在 `src/expr.c` 里有三个承重符号，各自对应一条独立的编译路径：

| 符号 | 覆盖的语法 | 产出 |
|---|---|---|
| `sqlite3CodeSubselect` | 标量子查询 `(SELECT ...)`、`EXISTS (SELECT ...)` | 一个寄存器（多列时是连续寄存器数组），以及 `OP_BeginSubrtn` / `OP_Return` 子程序对 |
| `sqlite3CodeRhsOfIN` | `IN` 的右值（值列表或子查询） | 一张 ephemeral B-tree（`OP_OpenEphemeral`），或复用已有索引 |
| `sqlite3ExprCodeIN` | `IN` 运算符整体（`TK_IN`） | 三路跳转：`destIfFalse`（不在集合内）、`destIfNull`（因 NULL 无法判定）、以及「落在原地」即命中 |

三个符号之间的关系是：`sqlite3ExprCodeIN` 是 IN 运算符的总入口，它先调 `sqlite3FindInIndex()` 决定右值用什么 B-tree 来表达；只有在没有现成索引可用时，`sqlite3FindInIndex()` 才回调 `sqlite3CodeRhsOfIN()` 去物化 ephemeral 表。而 `sqlite3CodeSubselect` 是独立于 IN 的另一条路，处理 `TK_SELECT` 与 `TK_EXISTS`。

三者的源码位置都在 `src/expr.c`；`sqlite3CodeRhsOfIN` 与 `sqlite3CodeSubselect` 在 `src/sqliteInt.h` 里导出，`sqlite3ExprCodeIN` 是 `src/expr.c` 的静态函数，只有 `src/expr.c` 内部可用。

## 怎么实现的

### 一、`sqlite3CodeSubselect`：标量子查询与 EXISTS

函数签名是 `int sqlite3CodeSubselect(Parse *pParse, Expr *pExpr)`，返回结果所在寄存器号，出错返回 0。函数体开头有两道断言固定了它的适用范围：`pExpr->op` 必须是 `TK_EXISTS` 或 `TK_SELECT`，且 `ExprUseXSelect(pExpr)` 为真。

**第一步：已编译过的子查询走「调用子程序」。** 函数体检查 `ExprHasProperty(pExpr, EP_Subrtn)`；若已置位，说明这个子查询的代码此前已生成过，于是发出 `OP_Gosub`（P1 = `pExpr->y.sub.regReturn`，P2 = `pExpr->y.sub.iAddr`），并直接返回 `pExpr->iTable`。这条路径用于同一个子查询表达式在语句里被引用多次的场景。

**第二步：开子程序。** 未编译过时，函数体置 `EP_Subrtn` 属性，把 `pExpr->y.sub.regReturn` 设为 `++pParse->nMem`（新分配一个寄存器作返回地址），并发出 `OP_BeginSubrtn`（P2 = 该返回寄存器）。`pExpr->y.sub.iAddr` 取 `OP_BeginSubrtn` 的地址加一。

**第三步：决定要不要每行重算。** 函数体的注释明确列出三种「必须每次遇到都重算」的情况：右值是相关子查询（correlated subquery）、右值是含变量的表达式列表、当前处于触发器内部。对应到代码上，判断依据是 `ExprHasProperty(pExpr, EP_VarSelect)` —— `src/sqliteInt.h` 里 `EP_VarSelect` 的注释是「pSelect is correlated, not constant」。只有**不**带 `EP_VarSelect` 时才发出 `OP_Once`，把子查询结果缓存起来供后续复用。

**第四步：设定结果落点。** `TK_SELECT` 与 `TK_EXISTS` 走两条不同的落点：

- `TK_SELECT`：`dest.eDest = SRT_Mem`，结果列数取 `pSel->pEList->nExpr`（`nReg`）。函数体先用 `OP_Null` 把 `dest.iSDParm` 起的 `pParse->nMem` 个寄存器清空（源码注释 "Init subquery result"），再把 `dest.nSdst` 设为 `nReg`。这里有一个特殊分支：当子查询同时带 `SF_Distinct` 和 `OFFSET`（`pSel->pLimit->pRight` 非空）时，`dest.iSdst` 单独分配在 `pParse->nMem+1`，多占 `nReg` 个寄存器；源码注释解释这是为了让 `OFFSET` 把所有行都排除时 `dest.iSDParm` 数组保持全 NULL，而不是残留最后一次访问到的行内容。
- `TK_EXISTS`：`dest.eDest = SRT_Exists`，只写一个整数。函数体发出 `OP_Integer`（P1 = 0）初始化该寄存器，源码注释 "Init EXISTS result"。

**第五步：强制 `LIMIT 1`。** 无论原查询有没有 `LIMIT`，子查询最终都只取第一行。函数体的处理分两种：

- 已有 `pSel->pLimit` 时，函数体检查其左子表达式 `pLeft` 是否带 `EP_IntValue` 属性且值既不是 1 也不是 0；不满足时把 `pLimit` 换成 `sqlite3PExpr(pParse, TK_NE, sqlite3ExprDup(db, pLeft, 0), sqlite3ExprInt32(db, 0))`——即把原 LIMIT 表达式改写成「原表达式 <> 0」，使新 LIMIT 的求值结果被限制为 1 或 0 这两种取值。源码注释写明：「If the pre-existing limit X is not already integer value 1 or 0, then make the new limit X<>0 so that the new limit is either 1 or 0」。
- 没有 `pSel->pLimit` 时，函数体发出 `sqlite3ExprInt32(pParse->db, 1)` 并组装成 `TK_LIMIT` 节点挂到 `pSel->pLimit`（源码注释 "If there is no limit add a limit of 1"）。

**第六步：交给 `sqlite3Select()` 生成查询计划。** 函数体调 `sqlite3Select(pParse, pSel, &dest)`；返回非 0 表示失败，此时函数体把 `pExpr->op2` 存为原来的 `op`、把 `pExpr->op` 改成 `TK_ERROR` 并返回 0。成功后 `pExpr->iTable = rReg = dest.iSDParm`，并置 `EP_NoReduce` 属性（`src/sqliteInt.h` 注释：「Cannot EXPRDUP_REDUCE this Expr」）。

**第七步：收尾。** 如果 `dest.eDest==SRT_Mem` 且 `sqlite3ExprCanReturnSubtype()` 为真，发出 `OP_ClrSubtype` 清子类型标记；最后发出 `OP_Return`（P1 = `regReturn`，P2 = `iAddr`，P3 = 1）结束子程序，并调 `sqlite3ClearTempRegCache(pParse)` 释放临时寄存器缓存。

`sqlite3CodeSubselect()` 的调用方有三个，都在 `src/expr.c`：

- `exprCodeSubselect()`（静态）：只在 `pExpr->op==TK_SELECT` 时才转调 `sqlite3CodeSubselect()`，否则返回 0。
- `exprCodeVector()`（静态）：向量表达式（列数大于 1 的 `TK_SELECT`）走这条路。
- `sqlite3ExprCode()` 的 `TK_SELECT` / `TK_EXISTS` / `TK_SELECT_COLUMN` 分支：`TK_SELECT_COLUMN` 分支还带一层重算保护，条件是 `pLeft->iTable==0 || pParse->withinRJSubrtn > pLeft->op2`。

`src/wherecode.c` 里另有一处调用：`sqlite3WhereCodeOneLoopStart()` 处理 `WO_IN` 时会调 `sqlite3CodeSubselect()` 生成驱动索引的子查询。

### 二、`sqlite3CodeRhsOfIN`：把 IN 的右值物化成 B-tree

函数签名是 `void sqlite3CodeRhsOfIN(Parse *pParse, Expr *pExpr, int iTab, int allowBloom)`，返回的是 ephemeral 表的游标号——函数体注释明确说明「the cursor number returned might not be the same, as it might have been duplicated using OP_OpenDup」。

**第一步：决定是否允许复用右值。** 函数体注释列出三种必须每次重算的情况（相关子查询、含变量的表达式列表、触发器内），代码上的判据是 `!ExprHasProperty(pExpr, EP_VarSelect) && pParse->iSelfTab==0`。`pParse->iSelfTab` 非零即表示处于触发器上下文。

**第二步：找已生成的等价右值。** 允许复用时，函数体先算一个签名 `SubrtnSig`：仅当右值是子查询且带 `SF_All` 标志之外的情况（`selFlags & SF_All)==0`）才分配签名，签名里记 `selId` 与由 `exprINAffinity()` 算出的亲和性串 `zAff`。然后检查 `ExprHasProperty(pExpr, EP_Subrtn)` 或调 `findCompatibleInRhsSubrtn()`——后者扫描已生成的字节码，找 `p4type==P4_SUBRTNSIG` 且 `bComplete` 为真、`selId` 相同、`zAff` 相同的 `OP_BeginSubrtn`，命中就把它的 `iAddr` / `regReturn` / `iTable` 抄到当前表达式上。命中路径发出 `OP_Gosub` 调用该子程序，再用 `OP_OpenDup`（P1 = 新游标 `iTab`，P2 = 原游标 `pExpr->iTable`）复制一份游标，然后跳到 `OP_Once` 的落点。

**第三步：分配 ephemeral 表。** 函数体发出 `OP_OpenEphemeral`（P1 = `pExpr->iTable = iTab`，P2 = `nVal`），其中 `nVal = sqlite3ExprVectorSize(pLeft)`——左值是几列向量，右值就有几列。随后用 `sqlite3KeyInfoAlloc(pParse->db, nVal, 1)` 分配键信息，分配失败则直接返回（`pParse->nErr` 已置位）。

**第四步（情形一）：右值是子查询。** 函数体用 `sqlite3SelectDestInit(&dest, SRT_Set, iTab)` 设定落点，用 `exprINAffinity()` 填 `dest.zAffSdst`。键信息的每列排序规则由 `sqlite3BinaryCompareCollSeq()` 逐列算出；函数体里有一段专门的守卫：若任一列的排序规则不是二元的（`sqlite3IsBinary(pColl)` 为假），就把 `allowBloom` 清零（源码标注 tag-202607231411）——因为 Bloom 过滤器只对等价字节序列有效。

**Bloom 过滤器的启用条件**（三条同时成立才启用）：`addrOnce` 非空（允许复用，即右值可缓存）、`allowBloom` 为真、`OptimizationEnabled(pParse->db, SQLITE_BloomFilter)` 为真。启用时函数体分配 `regBloom = ++pParse->nMem`，发出 `OP_Blob`（P1 = **10000**）生成一个 10000 字节的 blob 作为位图，把 `dest.iSDParm2` 指向它，并把 `pKeyInfo` 通过 `sqlite3VdbeChangeP4(v, addr, (void *)pKeyInfo, P4_KEYINFO)` 挂到 `OP_OpenEphemeral` 上。函数体还有一段注释说明 `OP_Filter` 的 P3 操作数来源：它把 `OP_Once` 的 `p3` 改成 `dest.iSDParm2`（tag-202407032019），若 `dest.iSDParm2==0`（过滤器实际不会被用）则把 `OP_Blob` 的 P1 从 10000 改成 **10**（注释写 "If the Bloom filter won't actually be used, keep it small"）。

`SQLITE_BloomFilter` 定义在 `src/sqliteInt.h`，值为 **20**，注释是「Use a Bloom filters」。判定宏是 `OptimizationEnabled(D,M)`，展开为 `(((D)->dbOptFlags&(1ull<<((M)-1)))==0)`——即 `dbOptFlags` 的第 19 位为 0 时该优化开启。`dbOptFlags` 由 `sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, N)` 设置，正常运行时为 0，所以 Bloom 过滤器默认开启。

**第五步（情形二）：右值是表达式列表。** 函数体取左值亲和性 `sqlite3ExprAffinity(pLeft)`，并做两处归一：`affinity<=SQLITE_AFF_NONE` 改成 `SQLITE_AFF_BLOB`，`affinity==SQLITE_AFF_REAL` 改成 `SQLITE_AFF_NUMERIC`。键信息的排序规则固定取左值的 `sqlite3ExprCollSeq()`。然后循环每个列表项：若该项不是常量（`sqlite3ExprIsConstant()` 为假），就撤销前面发出的 `OP_Once`（`sqlite3VdbeChangeToNoop`）并清掉 `EP_Subrtn` 属性、把 `addrOnce` 清零——源码注释解释：非常量表达式必须每次重跑。随后对每个值发出 `OP_MakeRecord`（P4 为亲和性字符）与 `OP_IdxInsert`。

**第六步：收尾。** 函数体把 `pKeyInfo` 挂到 `OP_OpenEphemeral` 的 P4 上；若 `addrOnce` 非空，发出 `OP_NullRow`（P1 = `iTab`）、跳到 `OP_Once` 落点，最后发 `OP_Return` 结束子程序，并调 `sqlite3ClearTempRegCache(pParse)`。

### 三、`sqlite3ExprCodeIN`：IN 运算符的整体编译

这是 `src/expr.c` 的静态函数 `static void sqlite3ExprCodeIN(Parse *pParse, Expr *pExpr, int destIfFalse, int destIfNull)`。它生成的是「三路跳转」而非「写一个 0/1 寄存器」：`destIfFalse` 表示左值确定不在右值集合内，`destIfNull` 表示因 NULL 无法判定，落在原地（即不跳）表示命中。

**前置检查。** 函数体第一件事是调 `sqlite3ExprCheckIN(pParse, pExpr)`。该函数定义在 `src/expr.c`，检查两侧列数是否匹配：右值是子查询时要求 `sqlite3ExprVectorSize(pIn->pLeft)` 等于 `pIn->x.pSelect->pEList->nExpr`，不等则调 `sqlite3SubselectError()` 报「sub-select returns %d columns - expected %d」；右值不是子查询时要求左值向量长度为 1，否则调 `sqlite3VectorErrorMsg()` 报「row value misused」。检查失败即返回。

**亲和性串。** 函数体调 `exprINAffinity(pParse, pExpr)` 拿到亲和性串。该函数为每个向量元素算亲和性：右值是子查询时用 `sqlite3CompareAffinity(pSelect->pEList->a[i].pExpr, a)`（右侧表达式的亲和性与左侧的），否则直接取左侧亲和性；并把 `SQLITE_AFF_REAL` 统一替换为 `SQLITE_AFF_NUMERIC`（函数头注释解释：比较中二者等价，但 REAL 亲和性下 `OP_Affinity` 与 `OP_MakeRecord` 会把整数转成浮点，注释原文写的是 "might change the value of a large integer"——即大整数在这条路径上会丢失精度）。

**选择右值 B-tree。** 函数体调 `sqlite3FindInIndex(pParse, pExpr, IN_INDEX_MEMBERSHIP | IN_INDEX_NOOP_OK, destIfFalse==destIfNull ? 0 : &rRhsHasNull, aiMap, &iTab)`。返回类型常量定义在 `src/sqliteInt.h`：

| 常量 | 值 | 含义 |
|---|---|---|
| `IN_INDEX_ROWID` | 1 | 右值是表的 rowid |
| `IN_INDEX_EPH` | 2 | 右值是临时生成的 ephemeral 表 |
| `IN_INDEX_INDEX_ASC` | 3 | 右值用升序索引 |
| `IN_INDEX_INDEX_DESC` | 4 | 右值用降序索引 |
| `IN_INDEX_NOOP` | 5 | 没有可用 B-tree，改用逐项比较 |

标志位：`IN_INDEX_NOOP_OK` = `0x0001`、`IN_INDEX_MEMBERSHIP` = `0x0002`、`IN_INDEX_LOOP` = `0x0004`。

`sqlite3FindInIndex()` 的判定逻辑（`src/expr.c`）：

- 先用 `isCandidateForInOpt()` 判断右值子查询能否简化为直接表访问。该函数排除的情况有：不是子查询、带 `EP_VarSelect`（相关子查询）、复合 SELECT（`pPrior` 非空）、带聚合（`SF_Aggregate`）、带 `LIMIT`、带 `WHERE`、FROM 子句不止一项、FROM 是子查询或视图、是虚拟表、以及结果列不是纯 `TK_COLUMN`。
- 候选通过后，若右值是 `rowid`（`nExpr==1` 且列号为负），走 `IN_INDEX_ROWID`。
- 否则遍历该表的索引，逐列用 `sqlite3BinaryCompareCollSeq()` 校验排序规则一致、每列只能用一次（`colUsed` 位掩码）。索引列数 `pIdx->nColumn` 必须 `>= nExpr`，且必须 `< BMS-1`（源码注释："Maximum nColumn is BMS-2, not BMS-1, so that we can compute BITMASK(nExpr) without overflowing"）。命中后 `eType = IN_INDEX_INDEX_ASC + pIdx->aSortOrder[0]`（源码断言 `IN_INDEX_INDEX_DESC == IN_INDEX_INDEX_ASC+1`）。
- 索引不可用且满足「`IN_INDEX_NOOP_OK` 已置、右值是列表（非子查询）、右值非常量或列表项不超过 2 条」时，退回 `IN_INDEX_NOOP`，并回退已分配的游标（`pParse->nTab--`，`iTab = -1`）。**回退的实际调用方是 `sqlite3ExprCodeIN()`**，它在自己的代码里处理 `IN_INDEX_NOOP` 分支，改为逐项比较。
- 最后若仍无 B-tree，走 `IN_INDEX_EPH`：此时调 `sqlite3CodeRhsOfIN(pParse, pX, iTab, bloomOk)` 物化 ephemeral 表。`bloomOk` 初值是 `(inFlags & IN_INDEX_MEMBERSHIP)!=0`；若不是成员测试、但右值子查询带 `SF_ClonedRhsIn` 标志，则仍置 `bloomOk=1`。

**第一步（`IN_INDEX_NOOP` 分支）：逐项比较。** 这是 `sqlite3ExprCodeIN()` 函数体里的第一个算法步骤（注释标为 "This is step (1) in the in-operator.md optimized algorithm"）。此时断言 `nVector==1` 且右值是表达式列表。函数体做法：

- 若 `destIfNull != destIfFalse`（即需要区分 FALSE 与 NULL），先分配 `regCkNull` 临时寄存器，发出 `OP_BitAnd`（P1 = `rLhs`，P2 = `rLhs`，P3 = `regCkNull`）记录左值是否为 NULL；循环里每遇到一个 `sqlite3ExprCanBeNull()` 为真的列表项，就再发一条 `OP_BitAnd` 把该值的 NULL 状态并进 `regCkNull`。
- 逐项比较：非最后一项（或需要区分 NULL 时）发 `OP_Eq`（P1 = `rLhs`，P2 = `labelOk`，P3 = `r2`，P4 为 `P4_COLLSEQ`），并用 `sqlite3VdbeChangeP5(v, zAff[0])` 写入亲和性；最后一项且不区分 NULL 时发 `OP_Ne`（P1 = `rLhs`，P2 = `destIfFalse`，P3 = `r2`），P5 写入 `zAff[0] | SQLITE_JUMPIFNULL`。`SQLITE_JUMPIFNULL` 定义在 `src/sqliteInt.h`，值为 `0x10`，注释「jumps if either operand is NULL」。当 `rLhs == r2`（左右同寄存器）时，比较操作码换成 `OP_NotNull` / `OP_IsNull`——因为此时比较无意义，只需判 NULL。
- 循环结束后若有 `regCkNull`，发 `OP_IsNull`（P1 = `regCkNull`，P2 = `destIfNull`）并 `sqlite3VdbeGoto(v, destIfFalse)`；随后解析 `labelOk` 标签，跳转出去即表示「命中」。

**第二步：施加亲和性并按索引顺序重排左值。** 若 `eType != IN_INDEX_ROWID`，函数体先发 `OP_Affinity`：单列时若 `zAff[0] >= SQLITE_AFF_TEXT` 且与 `sqlite3ExprAffinity(pLeft)` 不同，先 `OP_Copy` 复制一份再施加（源码注释说明原因：原寄存器还会被后续代码再次使用，例如 `TK_AGG_COLUMN`）；多列时发一条 `OP_Affinity`（P2 = `nVector`，P4 = `zAff`）。随后检查 `aiMap` 是否需要重排，若需要就分配 `nVector` 个临时寄存器并逐个 `OP_Copy`——`aiMap` 是「左值原始字段序 → 右值索引列序」的映射，由 `sqlite3FindInIndex()` 在选中索引时填出。

**第三步（源码注释的 Step 2）：左值含 NULL 则直接跳出。** 函数体对每个向量字段调 `sqlite3ExprCanBeNull()`，为真则发 `OP_IsNull`（P1 = `rLhs+aiMap[i]`，P2 = `destStep2`）。`destStep2` 在 `destIfNull==destIfFalse` 时等于 `destIfFalse`，否则等于新建的 `destStep6` 标签。

**第四步（Step 3）：二分查找。** 分两种：

- `eType == IN_INDEX_ROWID`：发 `OP_SeekRowid`（P1 = `iTab`，P2 = `destIfFalse`，P3 = `rLhs`），因为 rowid 本身非 NULL，Step 3 与 Step 4 合并成一条指令。
- 否则，若 `destIfFalse == destIfNull`（不区分 FALSE 与 NULL），Step 3 与 Step 5 合并：函数体先检查 `pExpr->y.sub.iAddr` 处那条指令是否是 `OP_Once`，若是且其 `p3 > 0`（说明有 Bloom 过滤器），发 `OP_Filter`（P1 = `pOp->p3`，P2 = `destIfFalse`，P3 = `rLhs`，P4 = `nVector`）；然后发 `OP_NotFound`（P1 = `iTab`，P2 = `destIfFalse`，P3 = `rLhs`，P4 = `nVector`），直接跳到 `sqlite3ExprCodeIN_finished`。
- 否则（区分 FALSE 与 NULL）发 `OP_Found`（P1 = `iTab`，P2 = 0，P3 = `rLhs`，P4 = `nVector`），把「命中」地址记在 `addrTruthOp`。

**第五步（Step 4）：右值确定无 NULL 时直接判 FALSE。** 仅当 `rRhsHasNull && nVector==1` 时发 `OP_NotNull`（P1 = `rRhsHasNull`，P2 = `destIfFalse`）。`rRhsHasNull` 这个寄存器由 `sqlite3SetHasNullFlag()` 填充：该函数发出 `OP_Integer`（P1 = 0）初始化，然后发 `OP_Rewind`（升序）或 `OP_Last`（降序）、`OP_Column`（P1 = 游标，P2 = 0 列，P3 = 目标寄存器），并用 `sqlite3VdbeChangeP5(v, OPFLAG_TYPEOFARG)` 标记。

**第六步（Step 5 / Step 6 / Step 7）：区分 NULL 时的逐行扫描。** 若 `destIfFalse==destIfNull`，发 `OP_Goto` 跳到 `destIfFalse` 即返回 FALSE。否则进入 Step 6：先按 `eType` 决定扫描方向——`IN_INDEX_INDEX_DESC` 且单列时发 `OP_Last`（P2 = `destIfFalse`），其余情况发 `OP_Rewind`（P2 = `destIfFalse`）。随后对每个向量字段发 `OP_Column`（P1 = `iTab`，P2 = `aiMap[i]`，P3 = 临时寄存器）与 `OP_Ne`（P1 = `rLhs+aiMap[i]`，P2 = `destNotNull`，P3 = 临时寄存器）。`destNotNull` 在 `nVector==1` 时直接等于 `destIfFalse`（源码注释：单列时检查 RHS 第一行就够），多列时是新建标签。多列时解析 `destNotNull` 后发 `OP_Next`（P1 = `iTab`，P2 = `addrTop+1`）继续下一行，最后发 `OP_Goto` 跳到 `destIfFalse`。扫描完后解析 `addrTruthOp` 标签作为「命中」出口。

**收尾。** 函数体在 `sqlite3ExprCodeIN_finished` 标签处释放 `aiMap` 与 `zAff`（两者都用 `sqlite3DbFree()` 释放）；OOM 路径 `sqlite3ExprCodeIN_oom_error` 同样释放这两块内存。

### 四、`sqlite3ExprCodeIN()` 的三个调用点

`sqlite3ExprCodeIN()` 是静态函数，只在 `src/expr.c` 内部被调用，共三处：

1. `sqlite3ExprCode()` 的 `TK_IN` 分支（约在函数体内 `case TK_IN:`）：先发 `OP_Null`（P1 = 0，P2 = `target`）把目标寄存器置 NULL 作为默认值，调用 `sqlite3ExprCodeIN(pParse, pExpr, destIfFalse, destIfNull)` 后发 `OP_Integer`（P1 = 1，P2 = `target`）写入「真」，然后解析 `destIfFalse` 标签并发 `OP_AddImm`（P1 = `target`，P2 = 0）把 1 变 0，最后解析 `destIfNull` 标签返回（此时 `target` 保持 NULL）。
2. `sqlite3ExprIfTrue()` 的 `TK_IN` 分支：`destIfNull = jumpIfNull ? dest : destIfFalse`，随后发 `OP_Goto` 跳到 `dest`。
3. `sqlite3ExprIfFalse()` 的 `TK_IN` 分支：`jumpIfNull` 为真时调 `sqlite3ExprCodeIN(pParse, pExpr, dest, dest)`（两个跳转目标相同，即不区分 FALSE 与 NULL）；否则新建 `destIfNull` 标签，调 `sqlite3ExprCodeIN(pParse, pExpr, dest, destIfNull)`，随后解析 `destIfNull` 标签。

### 五、相关子查询的逐行重算

「相关子查询每行重算」在源码里体现为 `EP_VarSelect` 属性的两处使用：

- `sqlite3CodeSubselect()`：不带 `EP_VarSelect` 才发 `OP_Once`；带了就不发，即子程序体每次被 `OP_Gosub` 调用都重新求值。
- `sqlite3CodeRhsOfIN()`：同样的 `!ExprHasProperty(pExpr, EP_VarSelect) && pParse->iSelfTab==0` 判定；不满足时连 `OP_BeginSubrtn` 子程序都不建，右值直接在建在主流程里。

相关子查询的 `EP_VarSelect` 由解析阶段设置（`src/resolve.c` 一带），本机制只消费这个属性。

### 六、右值子程序的跨表达式复用

`sqlite3CodeRhsOfIN()` 的签名机制（`SubrtnSig`）允许两个语义相同的 `IN` 右值共用一份物化结果：

- `SubrtnSig` 记录 `selId`（子查询编号）、`zAff`（亲和性串）、`iAddr`（子程序地址）、`regReturn`（返回寄存器）、`iTable`（ephemeral 表游标）、`bComplete`（是否已填完）。
- 签名通过 `sqlite3VdbeChangeP4(v, -1, (const char*)pSig, P4_SUBRTNSIG)` 挂到 `OP_BeginSubrtn` 指令的 P4 上。
- `pParse->mSubrtnSig` 是个位掩码，函数体写 `pParse->mSubrtnSig = 1 << (pSig->selId&7)`，用于快速跳过签名编号对不上的场景；`findCompatibleInRhsSubrtn()` 开头即检查 `(pParse->mSubrtnSig & (1<<(pNewSig->selId&7)))==0` 就直接返回 0。
- 命中后复用方式是 `OP_Gosub` 调用原子程序 + `OP_OpenDup` 复制游标。

## 关键取舍

**取舍一：IN 的右值优先复用现成索引，而不是无条件物化。** `sqlite3FindInIndex()` 把「找现成索引」放在「物化 ephemeral 表」之前，返回 `IN_INDEX_ROWID` / `IN_INDEX_INDEX_ASC` / `IN_INDEX_INDEX_DESC` 时完全不建临时表。代价是必须逐项校验亲和性（`sqlite3CompareAffinity` 与 `sqlite3TableColumnAffinity`）与排序规则（`sqlite3BinaryCompareCollSeq`），任何一列不匹配就整个放弃该索引——源码里 `affinity_ok` 一旦被清零就跳过全部索引。

**取舍二：右值小到不值得建表时，退化为逐项比较。** `sqlite3FindInIndex()` 的 `IN_INDEX_NOOP` 分支条件是：允许 NOOP、右值是列表（非子查询）、并且「右值非常量 **或** 列表项不超过 2 条」。也就是说常量列表超过 2 项就仍然建 ephemeral 表。**回退到逐项比较的实际调用方是 `sqlite3ExprCodeIN()`**，它在 `eType==IN_INDEX_NOOP` 分支里实现完整的比较序列。这是「小集合不建表」与「常量折叠友好」之间的折中。

**取舍三：子查询强制 `LIMIT 1`，且把已有的任意 LIMIT 改写成布尔形式。** `sqlite3CodeSubselect()` 把原 LIMIT 表达式改写成 `X <> 0`，而不是简单地替换成常量 1。源码注释明确说这样「the new limit is either 1 or 0」——语义上等价于「只取一行」，但保留了原表达式的求值副作用（例如 `LIMIT (SELECT ...)`）。代价是这条路径上多了一层表达式节点。

**取舍四：用子程序 + `OP_Once` 换取「只算一次」与「可复用」。** `sqlite3CodeSubselect()` 和 `sqlite3CodeRhsOfIN()` 都用 `OP_BeginSubrtn` / `OP_Return` 包住主体，靠 `OP_Once` 保证只执行一次。代价是引入了子程序这一层间接：返回值要经 `regReturn` 寄存器中转，`OP_Once` 的跳转目标要靠 `sqlite3VdbeJumpHere` 回填。`sqlite3CodeRhsOfIN()` 里「右值含非常量表达式就撤销 `OP_Once`」的分支（用 `sqlite3VdbeChangeToNoop` 把已发出的 `OP_Once` 改成空操作）是这套机制的典型代价——先乐观假设可以缓存，发现不行再回退。

**取舍五：右值子查询的复用靠「签名匹配」而非结构相等。** `findCompatibleInRhsSubrtn()` 只比较 `selId` 与亲和性串 `zAff` 两项。两项都相同即认为可复用。**架构推论：** 这依赖 `selId` 的分配规则保证「语义相同且亲和性相同的子查询有相同 selId」；若这个前提被打破，就会出现错误的复用。

**取舍六：Bloom 过滤器只在「右值可缓存 + 排序规则二元 + 优化未被禁用」时才用。** 三道门槛分别来自 `sqlite3CodeRhsOfIN()` 的 `addrOnce` 非空判断、`sqlite3IsBinary(pColl)` 检查、以及 `OptimizationEnabled(pParse->db, SQLITE_BloomFilter)`（`src/sqliteInt.h` 中值为 20）。位图大小默认 10000 字节（`OP_Blob` 的 P1），确认不会用到时缩到 10 字节。这套设计的收益是排除性查找能在读 B-tree 之前就判定「一定不在集合内」，代价是每次 IN 比较多一次哈希加一次位测试。

**取舍七：NULL 语义被显式建模为「第三条出路」。** `sqlite3ExprCodeIN()` 的签名里 `destIfFalse` 与 `destIfNull` 是两个独立标签，函数体用 `destIfFalse==destIfNull` 这个恒等判断来识别「调用方不关心 FALSE 与 NULL 的区别」，并在这种情形下把 Step 3 与 Step 5 合并成一条 `OP_NotFound`。这让最常见的 `WHERE x IN (...)` 走最短路径，代价是同一份代码要维护两套控制流。

**取舍八：`IN` 的 LHS 一律不参与常量提升。** `sqlite3ExprCodeIN()` 在求值左值前先把 `pParse->okConstFactor` 保存到局部变量、置 0、求值完再恢复。函数体注释写明原因：「Avoid factoring the LHS of the IN(...) expression out of the loop, even if it is constant, as OP_Affinity may be used on the register by code generated below.」

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/expr.c | `sqlite3CodeSubselect` | 标量子查询与 EXISTS 的编译：开 `OP_BeginSubrtn` 子程序、强制 `LIMIT 1`（已有 LIMIT 改写为 `X<>0`）、调 `sqlite3Select()` 生成计划、结尾发 `OP_Return`；带 `EP_Subrtn` 时改为 `OP_Gosub` 复用 |
| src/expr.c | `sqlite3ExprCodeIN` | IN 运算符整体编译：调 `sqlite3ExprCheckIN()` 校验列数、`sqlite3FindInIndex()` 选 B-tree，按 Step 1..7 生成 `OP_Eq`/`OP_NotFound`/`OP_Found`/`OP_Filter`/`OP_Column`/`OP_Ne`/`OP_Next` 等序列，产出 `destIfFalse` / `destIfNull` 两路跳转 |
| src/expr.c | `sqlite3CodeRhsOfIN` | 物化 IN 的右值：发 `OP_OpenEphemeral` 建 ephemeral 表；右值是子查询走 `SRT_Set` 落点，右值是列表则逐项 `OP_MakeRecord` + `OP_IdxInsert`；启用 Bloom 时发 `OP_Blob`（P1 = 10000）并把键信息挂到 `OP_OpenEphemeral` 的 P4 |
| src/expr.c | `sqlite3FindInIndex` | 选择 IN 右值的 B-tree：优先复用 rowid 或现成索引（`IN_INDEX_ROWID` / `IN_INDEX_INDEX_ASC` / `IN_INDEX_INDEX_DESC`），否则 `IN_INDEX_EPH`（回调 `sqlite3CodeRhsOfIN()`），小常量列表则退回 `IN_INDEX_NOOP` |
| src/expr.c | `sqlite3ExprCheckIN` | 校验 IN 两侧列数；不匹配时报「sub-select returns %d columns - expected %d」或「row value misused」 |
| src/expr.c | `exprINAffinity` | 生成 IN 每一列的亲和性串；子查询右值用 `sqlite3CompareAffinity()` 计算，`SQLITE_AFF_REAL` 统一替换为 `SQLITE_AFF_NUMERIC` |
| src/expr.c | `findCompatibleInRhsSubrtn` | 扫描已生成的字节码找可复用的 `OP_BeginSubrtn`（`P4_SUBRTNSIG`），按 `selId` 与亲和性串匹配，命中则复用子程序并 `OP_OpenDup` 复制游标 |
| src/expr.c | `isCandidateForInOpt` | 判定 IN 右值子查询能否简化为直接表访问；排除相关子查询、复合 SELECT、聚合、LIMIT、WHERE、多表 FROM、虚拟表、非常量结果列 |
| src/expr.c | `sqlite3SetHasNullFlag` | 生成探测索引首列是否含 NULL 的代码（`OP_Rewind`/`OP_Last` + `OP_Column` + `OPFLAG_TYPEOFARG`），供 `rRhsHasNull` 使用 |
| src/expr.c | `exprCodeSubselect` | `TK_SELECT` 表达式求值的薄封装，只有 `op==TK_SELECT` 时才转调 `sqlite3CodeSubselect()` |
| src/expr.c | `exprCodeVector` | 向量求值：列数为 1 走 `sqlite3ExprCodeTemp()`，否则 `TK_SELECT` 走 `sqlite3CodeSubselect()`、`TK_VECTOR` 逐项 `sqlite3ExprCodeFactorable()` |
| src/expr.c | `sqlite3InRhsIsConstant` | 判断 IN 的列表右值是否常量，用于 `IN_INDEX_NOOP` 判定 |
| src/expr.c | `sqlite3SubselectError` | 统一产生「sub-select returns %d columns - expected %d」错误消息 |
| src/expr.c | `sqlite3VectorErrorMsg` | 产生「row value misused」或子查询列数错误消息 |
| src/expr.c | `sqlite3ExprCode` | `TK_SELECT` / `TK_EXISTS` / `TK_SELECT_COLUMN` 分支调 `sqlite3CodeSubselect()`；`TK_IN` 分支调 `sqlite3ExprCodeIN()` |
| src/expr.c | `sqlite3ExprIfTrue` | `TK_IN` 分支调 `sqlite3ExprCodeIN(pParse, pExpr, destIfFalse, destIfNull)`，按 `jumpIfNull` 决定两个跳转目标是否合并 |
| src/expr.c | `sqlite3ExprIfFalse` | `TK_IN` 分支调 `sqlite3ExprCodeIN()`，`jumpIfNull` 为真时传 `(dest, dest)` |
| src/wherecode.c | `sqlite3WhereCodeOneLoopStart` | `WO_IN` 分支调 `sqlite3FindInIndex(pParse, pX, IN_INDEX_LOOP, ...)` 让 IN 驱动索引扫描；多列 IN 命中多个索引列时清掉 `WHERE_IN_SEEKSCAN` |
| src/wherecode.c | `sqlite3CodeRhsOfIN` 调用点 | 虚拟表 `OP_VFilter` 路径下显式调 `sqlite3CodeRhsOfIN(pParse, pTerm->pExpr, iTab, 0)` 物化 IN 右值 |
| src/sqliteInt.h | `IN_INDEX_ROWID` / `IN_INDEX_EPH` / `IN_INDEX_INDEX_ASC` / `IN_INDEX_INDEX_DESC` / `IN_INDEX_NOOP` | `sqlite3FindInIndex()` 的返回类型常量，值依次为 1 / 2 / 3 / 4 / 5 |
| src/sqliteInt.h | `IN_INDEX_NOOP_OK` / `IN_INDEX_MEMBERSHIP` / `IN_INDEX_LOOP` | `sqlite3FindInIndex()` 的标志位，值依次为 `0x0001` / `0x0002` / `0x0004` |
| src/sqliteInt.h | `SQLITE_BloomFilter` | 优化开关编号 20；`OptimizationEnabled()` 判定 `dbOptFlags` 第 19 位为 0 时开启，正常运行时开启 |
| src/sqliteInt.h | `SQLITE_JUMPIFNULL` | 值为 `0x10`，比较操作码 P5 的「操作数含 NULL 也跳转」标志 |
| src/sqliteInt.h | `EP_VarSelect` | 值为 `0x000040`，注释「pSelect is correlated, not constant」；决定 IN 右值与子查询是否每行重算 |
| src/sqliteInt.h | `EP_Subrtn` | 值为 `0x2000000`，注释「Uses Expr.y.sub. TK_IN, _SELECT, or _EXISTS」；标记表达式已编译为子程序 |
| src/vdbe.c | `OP_Filter` | Bloom 过滤器操作码：按 `pIn1->n*8` 取模后做位测试，命中过滤则跳到 P2；计数进 `SQLITE_STMTSTATUS_FILTER_HIT` / `SQLITE_STMTSTATUS_FILTER_MISS` |

## 与主核 / AI 的关系

**架构推论：** SQLite 把「右值集合」抽象成游标 + B-tree，而不是抽象成「集合类型」。`sqlite3FindInIndex()` 返回的 5 个 `IN_INDEX_*` 值说明：对上层代码而言，右值究竟是表、索引、ephemeral 表还是「无 B-tree」都不重要，重要的是它能不能提供「按序遍历 + 二分查找」这两件事。主核 `engine/db/` 的 `query/` 层若要支持向量检索类的 IN 语义，最直接的对应物是 `index/` 层暴露的「可顺序扫描的游标 + 可二分定位的键序」，而不是新增一种「集合」抽象。

**架构推论：** `sqlite3ExprCodeIN()` 的三路跳转（命中 / 明确不在 / 因 NULL 未知）是三值逻辑在编译期的一次显式展开。对 AI 生成 SQL 的场景，这条性质有直接影响：AI 若只根据「IN 列表里没有这个值」就断言结果为 FALSE，就忽略了 NULL 传播。SQLite 把「左值含 NULL」单独做成 Step 2（`OP_IsNull`）并跳到独立的 `destStep6` 标签，正是为了让这条语义在字节码层面可见，而不是被优化掉。

**架构推论：** Bloom 过滤器只被用在「右值可缓存」的场景，且要求排序规则是二元的（`sqlite3IsBinary()`）。这意味着 SQLite 的过滤加速建立在「键的等价性可以直接用字节比较判定」这个假设上——对于文本的 `NOCASE` 排序规则、或者带数值归一化的键，这个假设不成立，函数体因此直接关掉 `allowBloom`。主核若要对 JSON 键或向量做类似的前置过滤，必须显式区分「可哈希的键」与「需规范化的键」，SQLite 的做法是只在前者上加速。

**架构推论：** `findCompatibleInRhsSubrtn()` 靠 `selId` + 亲和性串两个字段判定两个 IN 右值可复用，这个「复用签名」的做法把语义等价判定压缩成了一个编号加一个短字符串。对 AI 场景有参考价值：AI 生成或改写 SQL 时极易产生「同一个子查询被抄写多遍」的形态，SQLite 的编译期签名匹配能自动把它们合并；但这也意味着编译器必须给每个子查询分配稳定的编号，改写器（如 `src/rewrite.c` 一带的规则改写）若复制了子查询节点却不复制编号，就会失去这份优化。

**架构推论：** `sqlite3CodeRhsOfIN()` 的「先乐观发 `OP_Once`，发现右值含非常量再 `sqlite3VdbeChangeToNoop` 撤销」是典型的「先发码后修正」编译策略。SQLite 之所以能这么做，是因为 VDBE 提供了按地址改写指令的能力（`sqlite3VdbeChangeToNoop` / `sqlite3VdbeChangeP4` / `sqlite3VdbeChangeP5`）。主核 `engine/db/` 若采用「一次遍历直接产出执行计划」的编译模型，就没有这层事后修正能力，需要在生成前就把右值性质判定清楚。

## 参考

- [SQLite SQL Language Expressions（IN / NOT IN、EXISTS、子查询表达式与相关子查询的官方语义）](https://sqlite.org/lang_expr.html)
- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层）](https://sqlite.org/arch.html)
- [SQLite VDBE Opcode Documentation（OP_Once、OP_OpenEphemeral、OP_Filter、OP_NotFound、OP_Found 等操作码说明）](https://sqlite.org/opcode.html)
- [SQLite Query Planner（查询规划与索引选择的官方说明）](https://sqlite.org/queryplanner.html)
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c)
- [sqlite/sqlite 源码：src/wherecode.c](https://github.com/sqlite/sqlite/blob/master/src/wherecode.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
- [sqlite/sqlite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
