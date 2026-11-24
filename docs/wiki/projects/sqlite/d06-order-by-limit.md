---
project: sqlite
feature: order-by-limit
page_type: feature
schema_version: 1
refs:
  - path: src/select.c
    symbols: [sqlite3CopySortOrder, sqlite3ConvertedOrderByColl]
---

# 排序与 LIMIT

## 一句话职责

本页负责两件在 SELECT 编译期确定的事：一是 ORDER BY 的排序键表达如何从「ORDER BY 项」归约成「结果集列引用 + 排序方向」，以及在复合 SELECT 被改写成子查询后如何补回 COLLATE 排序规则；二是 LIMIT/OFFSET 的截断语义如何变成 VDBE 里的计数器寄存器、跳转标签与截断指令。其中「顺序能否由索引直接满足」由 WHERE 层（`src/where.c`）回报给 SELECT 层，SQLite 只是在收到回报后决定用不用排序器。

## 机制是什么

边界（依据条目 `boundary`）：负责 ORDER BY 的排序键表达与 LIMIT/OFFSET 的截断语义（含是否可用索引直接满足顺序）；不负责排序器的具体实现（F 组 sorter）、不负责索引扫描（F 组）。

### 两条彼此独立的判定链

SQLite 把「排序」和「截断」拆成两条独立的判定链，在代码生成阶段才汇合：

| 链条 | 判定内容 | 承载结构 | 判定方 |
|---|---|---|---|
| 排序链 | ORDER BY 有几项能被索引直接满足 | `SortCtx.nOBSat` | `src/where.c` 的 `sqlite3WhereIsOrdered` 回报，`src/select.c` 消费 |
| 截断链 | LIMIT/OFFSET 展开成哪几个寄存器、跳到哪个标签 | `Select.iLimit` / `Select.iOffset` / `Select.pLimit` | `src/select.c` 的 `computeLimitRegisters` 生成 |

两者的交点是「排序器要不要为 LIMIT 提前裁剪」：`pushOntoSorter` 里的 `iLimit` 取值规则是 `pSelect->iOffset ? pSelect->iOffset+1 : pSelect->iLimit`，即带 OFFSET 时按 `LIMIT+OFFSET` 条数裁剪，不带 OFFSET 时按 `LIMIT` 条数裁剪。

### 排序键的表达形式

ORDER BY 的每一项最终落到 `ExprList_item` 的两个字段上（定义在 `src/sqliteInt.h` 的 `struct ExprList_item`）：

- `u.x.iOrderByCol`——该 ORDER BY 项是结果集里的第几列（1 基）；为 0 表示不是结果集列的引用。
- `fg.sortFlags`——`KEYINFO_ORDER_*` 掩码，其中 `KEYINFO_ORDER_DESC`（`0x01`）表示降序，`KEYINFO_ORDER_BIGNULL`（`0x02`）表示 NULL 比其他值都大。

这两者由 `src/resolve.c` 的 `resolveOrderGroupBy` 在名字解析阶段填好：ORDER BY 项若与结果集的 `AS` 别名匹配，或本身是整数常量，就置 `iOrderByCol`；否则按普通表达式解析，再与结果集逐列比对，命中则把 `iOrderByCol` 置为该列序号（1 基）。

### LIMIT / OFFSET 的语法形态

`src/parse.y` 的 `limit_opt` 非终结符把 LIMIT 子句规约成两种形态：

- `LIMIT expr(X)` → `sqlite3PExpr(pParse,TK_LIMIT,X,0)`，`pLeft` 是 LIMIT 值，`pRight` 为 0（无 OFFSET）。
- `LIMIT expr(X) OFFSET expr(Y)` 与 `LIMIT expr(X) COMMA expr(Y)` → `sqlite3PExpr(pParse,TK_LIMIT,X,Y)`，后者等价于 `LIMIT Y OFFSET X`（逗号形式里第一个数是 OFFSET，第二个数是 LIMIT）。

## 怎么实现的

### 第一步：sqlite3CopySortOrder —— 把排序方向从一个列表搬到另一个列表

`sqlite3CopySortOrder(ExprList *p1, ExprList *p2)` 是 `src/select.c` 的静态函数。它的完整判定条件写在函数头注释里：只有 `p2` 存在**且** `p1->nExpr==p2->nExpr` 时才动手。

搬运的内容只有一个位：`sortFlags = p2->a[ii].fg.sortFlags & KEYINFO_ORDER_DESC`。也就是**只搬降序标志，不搬 `KEYINFO_ORDER_BIGNULL`**。搬运是整体覆盖式的（`p1->a[ii].fg.sortFlags = sortFlags`），不是按位或，所以 `p1` 原来的 `KEYINFO_ORDER_BIGNULL` 位会被清掉。

函数返回 1 表示搬成功，返回 0 表示「`p2` 为空或两个列表长度不等，什么都没做」。

它在 `src/select.c` 里有两个调用点：

1. **DISTINCT 改写成 GROUP BY**：条件是 `(p->selFlags & (SF_Distinct|SF_Aggregate))==SF_Distinct`（即只有 DISTINCT、没有聚合）且 `sqlite3CopySortOrder(pEList, sSort.pOrderBy)` 返回真，且 `sqlite3ExprListCompare(pEList, sSort.pOrderBy, -1)==0`（结果集与 ORDER BY 列表逐项相同），且 `OptimizationEnabled(db, SQLITE_GroupByOrder)` 成立，且 `p->pWin==0`。改写内容是：清掉 `SF_Distinct`，`pGroupBy = p->pGroupBy = sqlite3ExprListDup(db, pEList, 0)`，把每个 `pGroupBy->a[i].u.x.iOrderByCol` 置为 `i+1`，并置上 `SF_Aggregate`。
2. **GROUP BY 与 ORDER BY 相同的判定**：`sqlite3CopySortOrder(pGroupBy, sSort.pOrderBy)` 返回真且 `sqlite3ExprListCompare(pGroupBy, sSort.pOrderBy, -1)==0` 时置 `orderByGrp = 1`。

**为什么必须先调 `sqlite3CopySortOrder`**：`sqlite3ExprListCompare` 内部（`src/expr.c`）逐项比较时会先判 `if( pA->a[i].fg.sortFlags!=pB->a[i].fg.sortFlags ) return 1;`。两个列表的 `sortFlags` 若不同，比较直接返回 1（非 0），两条改写路径都不会被触发。所以 `sqlite3CopySortOrder` 的作用是把两个列表的 `sortFlags` 对齐，让「表达式相同」这个判定不被排序方向的差异干扰。

### 第二步：sqlite3ConvertedOrderByColl —— 复合 SELECT 改写后补回 COLLATE

`sqlite3ConvertedOrderByColl(Parse *pParse, Select *pSub, ExprList *pOrderBy)` 是 `src/select.c` 的非静态函数（`src/sqliteInt.h` 里有对应声明；当编译期打开 `SQLITE_OMIT_COMPOUND_SELECT` 时该声明被条件编译掉）。

它的调用方是 `src/resolve.c` 的名字解析回调，触发条件是 `p->selFlags & SF_Converted`——这个标志由 `src/select.c` 的 `convertCompoundSelectToSubquery` 打上。调用顺序固定是：先把复合子查询的 ORDER BY 从子查询搬回父查询（`p->pOrderBy = pSub->pOrderBy; pSub->pOrderBy = 0;`），再调 `sqlite3ConvertedOrderByColl(pParse, pSub, p->pOrderBy)`。

函数体逐项处理 ORDER BY 列表：

1. `if( IN_RENAME_OBJECT==0 )` —— 处于 RENAME 操作中时整个函数体被跳过，不加任何 COLLATE。
2. 对每一项取 `iCol = p->u.x.iOrderByCol - 1`，并断言 `iCol>=0 && iCol<pSub->pEList->nExpr`。
3. `if( ExprHasProperty(p->pExpr, EP_Collate) ) continue;` —— 这一项已经自带 COLLATE，不动它。`EP_Collate`（`0x000200`）的语义是「树里含 TK_COLLATE 运算符」。
4. `pColl = multiSelectCollSeq(pParse, pSub, iCol);` —— 去复合子查询里取第 `iCol` 列生效的排序规则。
5. 若 `pColl` 非空，用 `sqlite3ExprAddCollateString(pParse, p->pExpr, pColl->zName)` 给该表达式套上 COLLATE。

**`multiSelectCollSeq` 的取法**（`src/select.c` 的静态函数）：递归沿 `p->pPrior` 往左走到最左分支取 `pRet`；`pRet` 为 0 时，若 `iCol < p->pEList->nExpr` 且该语句不是多行 VALUES，就取 `sqlite3ExprCollSeq(pParse, p->pEList->a[iCol].pExpr)`。函数头注释写明规则是「复合 SELECT 的排序规则取自最左分支中最先带排序规则的那一项」。

**对比：`multiSelectByMergeKeyInfo` 的取法不同。** 同样是复合 SELECT 的 ORDER BY，但走归并（merge）路径时，排序规则的兜底不一样：`multiSelectByMergeKeyInfo` 在 ORDER BY 项没有 `EP_Collate` 时取 `multiSelectCollSeq(...)`，**若结果为 0 则回退到 `db->pDfltColl`**（默认排序规则），并把该排序规则写回 `pOrderBy->a[i].pExpr`；`sqlite3ConvertedOrderByColl` 没有这个 `db->pDfltColl` 兜底，`pColl` 为 0 就什么都不加。

### 第三步：复合 SELECT 改写成子查询的触发条件

`convertCompoundSelectToSubquery`（`src/select.c` 的静态函数，作为 Walker 回调使用）是 `SF_Converted` 的来源。它的门槛有两条：

- **必须含 ORDER BY**：`if( p->pOrderBy==0 ) return WRC_Continue;`。
- **必须含 COLLATE**：从最后一个 ORDER BY 项往前找带 `EP_Collate` 标志的项，`for(i=p->pOrderBy->nExpr-1; i>=0; i--){ if( a[i].pExpr->flags & EP_Collate ) break; }`，若 `i<0`（没有任何一项带 COLLATE）就 `return WRC_Continue`。

改写动作：新建一个 `Select`（`pNew`）作为子查询，`p->pSrc` 换成只含这一项子查询的 `SrcList`，`p->pEList` 换成单个 `*`（`sqlite3Expr(db, TK_ASTERISK, 0)`），清掉 `p->pWhere`/`p->pGroupBy`/`p->pHaving`，`p->pPrior`/`p->pNext`/`p->pWith` 置 0，并置上 `SF_Converted`。`pNew->pLimit = 0`。

### 第四步：LIMIT/OFFSET 展开成寄存器 —— computeLimitRegisters

`computeLimitRegisters(Parse *pParse, Select *p, int iBreak)` 是 `src/select.c` 的静态函数。函数头注释说明了寄存器约定：`iLimit` 寄存器装 LIMIT 值，`iOffset` 寄存器装 OFFSET 值，`iOffset+1` 寄存器装 `LIMIT+OFFSET`。函数开头的 `if( p->iLimit ) return;` 是幂等保护——只有 `pLimit->pLeft` 存在时才重新定义这两个寄存器，注释明确写「UNION ALL 操作符依赖这个性质来让多个 SELECT 语句复用同一对 limit/offset 寄存器」。

LIMIT 值的两种处理分支：

- **常量 LIMIT**（`sqlite3ExprIsInteger(pLimit->pLeft, &n, pParse, 0)` 为真）：发 `OP_Integer` 把 `n` 装进 `iLimit` 寄存器。若 `n==0`，直接 `sqlite3VdbeGoto(v, iBreak)` 跳到语句结束——`LIMIT 0` 意味着零行输出（函数注释里承认这里有争议：「`LIMIT -1` 总是显示全部行。到底什么才是正确行为存在争议。当前实现把 `LIMIT 0` 解释成没有行」）。若 `n>=0 && p->nSelectRow>sqlite3LogEst((u64)n)`，则把 `p->nSelectRow` 压到 `sqlite3LogEst((u64)n)` 并置上 `SF_FixedLimit`（`0x0004000`）。
- **非常量 LIMIT**：用 `sqlite3ExprCode` 求值装进 `iLimit`，再用 `OP_MustBeInt` 强制转整数，最后 `OP_IfNot` 跳到 `iBreak`。`OP_MustBeInt` 的语义在 `src/vdbe.c`：值不是整数且无法无损转成整数时，若 `P2==0` 就抛 `SQLITE_MISMATCH`，否则跳到 `P2`（此处 `P2` 为 0，即抛 `SQLITE_MISMATCH`）。

OFFSET 分支（`pLimit->pRight` 存在时）：`p->iOffset = iOffset = ++pParse->nMem;` 然后 `pParse->nMem++;` 多占一个寄存器给 `LIMIT+OFFSET`，用 `sqlite3ExprCode` 求值装进 `iOffset`，再用 `OP_MustBeInt` 强制转整数，最后发 `OP_OffsetLimit`（`P1=iLimit`、`P2=iOffset+1`、`P3=iOffset`）。

**`OP_OffsetLimit` 的语义**（`src/vdbe.c`）：读 `r[P1]`（LIMIT）与 `r[P3]`（OFFSET），若 `r[P1]<=0` 或 `LIMIT+OFFSET` 溢出（`sqlite3AddInt64` 返回非 0），则把 `r[P2]` 置为 `-1`；否则置为 `LIMIT+OFFSET`。源码注释说明 `-1` 的含义是「无限循环」：LIMIT 小于等于零时循环永远不结束（这是文档化的行为）；LIMIT+OFFSET 超过 2^63 时同样无限循环（这是未文档化的行为），注释给出的理由是按每秒十亿次迭代估算，要接近 300 年才会真正到达该上限。

### 第五步：截断的三种落地方式

LIMIT 的语义最终由三处代码落地，各自的调用方不同：

| 落地方式 | 位置 | 触发条件 | 语义 |
|---|---|---|---|
| 内层循环递减计数 | `src/select.c` 的 `selectInnerLoop` 尾部 | `pSort==0 && p->iLimit`（**没有**排序器时） | `OP_DecrJumpZero` 递减 `iLimit` 寄存器，减到 0 跳 `iBreak` |
| 排序器侧裁剪 | `src/select.c` 的 `pushOntoSorter` | 有排序器时 | 排序器内最多保留 `LIMIT+OFFSET` 条；满后新记录比现有最大记录小则 `OP_Delete` 掉最大者再插入 |
| 输出侧跳 OFFSET | `src/select.c` 的 `codeOffset` | `iOffset>0` | `OP_IfPos` 跳过前 `iOffset` 行 |

`OP_DecrJumpZero` 的语义（`src/vdbe.c`）：`if( pIn1->u.i>SMALLEST_INT64 ) pIn1->u.i--;`，减到 0 跳转。

**排序器侧裁剪的具体指令序列**（`pushOntoSorter`）：先 `OP_IfNotZero` 检查 `iLimit` 寄存器，为 0 则跳过整段；然后 `OP_Last` 取排序器最后一条记录，`OP_IdxLE` 判断新记录是否 ≤ 排序器中的最大记录，是则 `OP_Delete` 删掉最大记录，随后才做插入（`OP_SorterInsert` 或 `OP_IdxInsert`，取决于 `SORTFLAG_UseSorter`）。函数注释写明：这样排序器里的条目数永远不超过 `LIMIT+OFFSET`。

**`OP_IfNotZero` 的语义**（`src/vdbe.c`）：寄存器内容非 0 就减 1 并跳转到 `P2`；为 0 则原样保留并顺序执行。

### 第六步：ORDER BY 能否免排序 —— 索引直接满足顺序

`src/select.c` 的 `sqlite3Select` 在非聚合路径里调用 `sqlite3WhereBegin(pParse, pTabList, pWhere, sSort.pOrderBy, p->pEList, p, wctrlFlags, p->nSelectRow)`，把 ORDER BY 列表交给 WHERE 层规划。随后读取回报：

```
if( sSort.pOrderBy ){
  sSort.nOBSat = sqlite3WhereIsOrdered(pWInfo);
  sSort.labelOBLopt = sqlite3WhereOrderByLimitOptLabel(pWInfo);
  if( sSort.nOBSat==sSort.pOrderBy->nExpr ){
    sSort.pOrderBy = 0;
  }
}
```

`sqlite3WhereIsOrdered`（`src/where.c`）的返回值语义写在函数头注释里：**返回 0 表示输出必须完全排序；返回等于 ORDER BY 项数表示完全不需要排序；返回正数但小于项数表示需要分块排序（block sorting）**。实现只有一行：`return pWInfo->nOBSat<0 ? 0 : pWInfo->nOBSat;`。

判定结果有两个后果：

- **完全满足**（`nOBSat == ORDER BY 项数`）：`sSort.pOrderBy` 被置 0；随后 `if( sSort.addrSortIndex>=0 && sSort.pOrderBy==0 ) sqlite3VdbeChangeToNoop(v, sSort.addrSortIndex);` 把先前开好的排序临时表改成空操作。
- **部分满足**（`0 < nOBSat < ORDER BY 项数`）：保留排序器，但 `pushOntoSorter` 只对未被索引满足的那几项做比较——`nOBSat` 之前的项由 `OP_Compare` 判前缀是否变化，前缀变了就 `OP_ResetSorter` 并 `OP_Gosub` 到 `pSort->labelBkOut` 输出已有结果。

**ORDER BY LIMIT 优化的跳转目标**由 `sqlite3WhereOrderByLimitOptLabel`（`src/where.c`）给出：若 `!pWInfo->bOrderedInnerLoop`（内层循环已知递增出序这一前提不成立），返回 `pWInfo->iContinue`；否则取最内层 `WhereLevel`，返回 `pInner->pRJ ? pWInfo->iContinue : pInner->addrNxt`。该函数的注释说明：跳到次内层循环的续点是优化，跳到最内层循环的续点总是安全的。

**代价估算里的 LIMIT 折扣**：`src/where.c` 的排序代价计算里，`if( (pWInfo->wctrlFlags & WHERE_USE_LIMIT)!=0 )` 成立时给排序代价加 `Tuning(10,16)`（注释标为「用 LIMIT 时额外 2.0 倍」），部分排序再加 `Tuning(6,17)`（「部分排序额外 1.5 倍」），并把估算行数 `nRow` 压到 `pWInfo->iLimit`。`WHERE_USE_LIMIT` 的值是 `0x4000`，与 `SF_FixedLimit` 同值（`src/sqliteInt.h` 中两处注释互相指认）。

### 第七步：ORDER BY 列表被裁剪与被改写的三种情形

1. **无意义的 ORDER BY 被丢弃**：`sqlite3Select` 开头，当 `IgnorableDistinct(pDest)` 为真（目的地是 `SRT_Exists`、`SRT_Discard`、`SRT_DistQueue`、`SRT_DistFifo`）时，若有 `p->pOrderBy` 就用 `sqlite3ParserAddCleanup` 把它从树上摘掉并置 0。注释写明这些目的地「也能忽略 ORDER BY 子句」。
2. **DISTINCT 改写成 GROUP BY**：见第一步的调用点 1。
3. **GROUP BY 与 ORDER BY 相同**：置 `orderByGrp = 1`，注释解释这是「因为 GROUP BY 会让元素按正确顺序出来」，但也承认「也可能不是——GROUP BY 可能用了一个把行分组到一起但并没有排序的索引」。

### 第八步：复合 SELECT 的 LIMIT 处理

复合 SELECT 走 `multiSelect` 路径（`src/select.c`）。UNION ALL 分支（`op==TK_ALL` 或 `op==TK_UNION` 且不满足归并条件）的 LIMIT 处理是：先把 `p->iLimit`/`p->iOffset` 复制给 `pPrior`，跑完左分支后取回；若 `p->iLimit` 非零，发 `OP_IfNot` 跳到「LIMIT 已达」的位置，若 `p->iOffset` 存在则再发一条 `OP_OffsetLimit`。

而归并路径（`multiSelectByMerge`）的 LIMIT 处理是：先 `computeLimitRegisters(pParse, p, addrBreak)`，然后 `if( p->iLimit && (op==TK_ALL || unionWithLimitOne(db,p)) )` 时把 `p->iOffset ? p->iOffset+1 : p->iLimit` 里的寄存器值 `OP_Copy` 给 `regLimit`，让左右两个分支共用同一个 LIMIT 计数器。

复合 SELECT 走归并路径时，ORDER BY 的排序规则由 `multiSelectByMergeKeyInfo` 构造 `KeyInfo`：`pRet->aColl[i] = pColl`，`pRet->aSortFlags[i] = pOrderBy->a[i].fg.sortFlags`。

### 第九步：排序器的两种形态

`SortCtx.sortFlags` 的 `SORTFLAG_UseSorter`（`0x01`）决定用哪种排序容器：

- **未置位**（用临时 B 树）：`OP_OpenEphemeral` 开临时索引，插入用 `OP_IdxInsert`，输出用 `OP_Sort` + `OP_Next`；排序记录里额外带一个序号列（`bSeq`）。
- **置位**（用 Sorter）：`OP_SorterOpen` 开，插入用 `OP_SorterInsert`，输出用 `OP_SorterSort` + `OP_SorterData` + `OP_SorterNext`。

置位时机在 `src/select.c` 的 `sqlite3Select` 里：`if( p->iLimit==0 && sSort.addrSortIndex>=0 )` 时把 `OP_OpenEphemeral` 改成 `OP_SorterOpen` 并置 `SORTFLAG_UseSorter`。**注意判断条件是 `p->iLimit==0`**——有 LIMIT 时不用 Sorter，因为 LIMIT 截断逻辑依赖 `pushOntoSorter` 里的 `OP_Last`/`OP_IdxLE`/`OP_Delete` 序列，这些指令作用于 B 树型排序器。

输出侧 `generateSortTail`（`src/select.c` 的静态函数）在未置 `SORTFLAG_UseSorter` 时会调 `codeOffset(v, p->iOffset, addrContinue)` 跳过 OFFSET 行，并在 `p->iOffset>0` 时发 `OP_AddImm` 把 `p->iLimit` 减 1（把「LIMIT 条数 + OFFSET 条数」还原成「LIMIT 条数」）。而在 `SORTFLAG_UseSorter` 分支里有 `assert( p->iLimit==0 && p->iOffset==0 );`——这条断言印证了上面「有 LIMIT 就不用 Sorter」的结论。

### 第十步：ORDER BY 项数上限

`src/resolve.c` 的 `sqlite3ResolveOrderGroupBy` 开头有一条硬上限：`if( pOrderBy->nExpr>db->aLimit[SQLITE_LIMIT_COLUMN] )` 时报 `"too many terms in %s BY clause"` 并返回 1。`zType` 参数传的是 `"ORDER"` 或 `"GROUP"`，所以 ORDER BY 报的是 `too many terms in ORDER BY clause`。

### 第十一步：min/max 优化给 ORDER BY 打的排序标志

`minMaxQuery`（`src/select.c` 的静态函数）在识别出聚合函数是 `min` 或 `max` 时，会给新造的 ORDER BY 列表打排序标志：min 且参数可能为 NULL 时置 `KEYINFO_ORDER_BIGNULL`；max 时置 `KEYINFO_ORDER_DESC`。标志写在 `pOrderBy->a[0].fg.sortFlags` 上。

**这条路径与 `sqlite3CopySortOrder` 的交互值得注意**：`sqlite3CopySortOrder` 只搬 `KEYINFO_ORDER_DESC`、不搬 `KEYINFO_ORDER_BIGNULL`。所以 min/max 优化打上的 `KEYINFO_ORDER_BIGNULL` 标志不会被 `sqlite3CopySortOrder` 传播。

## 关键取舍

- **源码事实：**`sqlite3CopySortOrder` 只搬 `KEYINFO_ORDER_DESC` 一个位，且是整体覆盖而非按位或。**架构推论：**这是为了让「结果集列表」与「ORDER BY 列表」在 `sqlite3ExprListCompare` 眼里完全可比——该函数逐项比较时先比 `sortFlags`，只要有一个位不同就直接返回 1（判定为「肯定不同」）。整体覆盖保证比较用的两份 `sortFlags` 逐位相同，代价是 `p1` 上原有的 `KEYINFO_ORDER_BIGNULL` 被抹掉；因为 min/max 优化打的 `KEYINFO_ORDER_BIGNULL` 与这两处调用点（结果集 vs ORDER BY、GROUP BY vs ORDER BY）的场景不重合，抹掉它不改变这两条改写路径的结论。

- **源码事实：**`sqlite3ConvertedOrderByColl` 在 `pColl` 为 0 时不加 COLLATE，而 `multiSelectByMergeKeyInfo` 在同样情况下回退到 `db->pDfltColl`。**架构推论：**两条路径对「复合 SELECT 的排序规则」给出了不同的兜底策略——`sqlite3ConvertedOrderByColl` 的场景是「复合子查询已被改写成子查询、ORDER BY 已搬回父查询」，此时表达式上还没有任何 COLLATE 包装，不加就沿用表达式自身的默认规则；`multiSelectByMergeKeyInfo` 的场景是「归并需要构造一个可比较的 KeyInfo」，KeyInfo 的 `aColl` 槽位必须非空才能做键比较，所以必须有兜底。

- **源码事实：**`convertCompoundSelectToSubquery` 的两个前置门槛是「有 ORDER BY」和「至少一项带 `EP_Collate`」。**架构推论：**改写把复合查询变成「父查询 + 一个子查询」，此时 ORDER BY 会落到父查询上，而父查询的结果列来自子查询的 `*` 展开，COLLATE 信息若不显式搬运就会在改写中丢失。只有确实存在 COLLATE 时才值得付出改写成本；没有 COLLATE 时保持复合查询原样，走 `multiSelect` 的归并或顺序执行路径。

- **源码事实：**`computeLimitRegisters` 对常量 LIMIT 在 `n==0` 时直接 `sqlite3VdbeGoto(v, iBreak)`，非常量 LIMIT 走 `OP_MustBeInt` + `OP_IfNot`。**架构推论：**常量 `LIMIT 0` 在编译期就能确定结果为空，用一条无条件跳转替代运行时判断，省掉计数器寄存器与每行的递减开销；而非常量 LIMIT 必须先求值再判断，`OP_MustBeInt` 保证后续的 `OP_IfNot` 与 `OP_DecrJumpZero` 面对的是整数寄存器。

- **源码事实：**`OP_OffsetLimit` 在 `LIMIT+OFFSET` 超过 2^63 时把 `LIMIT+OFFSET` 寄存器置为 `-1`。**架构推论：**`-1` 在后续的 `OP_IfNotZero`/`OP_DecrJumpZero` 语义下意味着计数器永不为零，语句因此不终止。源码注释自陈这是未文档化的行为，并给出「按每秒十亿次迭代需近 300 年才触及上限」的工程理由来说明这一近似可接受。

- **源码事实：**有 LIMIT 时 `p->iLimit != 0`，`sqlite3Select` 因此不会把 `OP_OpenEphemeral` 改成 `OP_SorterOpen`，排序器保持 B 树形态；`generateSortTail` 的 Sorter 分支里有 `assert( p->iLimit==0 && p->iOffset==0 )`。**架构推论：**LIMIT 截断需要在排序过程中即时淘汰最大记录，`pushOntoSorter` 用的是 `OP_Last`/`OP_IdxLE`/`OP_Delete`，这组指令要求排序器是 B 树；Sorter 接口不提供「取最后一条并删除」的等价操作，所以两条路径互斥。

- **源码事实：**`sqlite3WhereIsOrdered` 返回 `pWInfo->nOBSat<0 ? 0 : pWInfo->nOBSat`，调用方拿它与 `sSort.pOrderBy->nExpr` 相等比较来决定是否整体丢弃排序。**架构推论：**「部分满足」被保留为分块排序而非直接丢弃，是因为 `nOBSat` 只说明前若干项有序，后面几项仍需比较；把 `nOBSat` 归零（返回 0 的那条分支）表示连前缀顺序都无法保证。

- **源码事实：**`sqlite3WhereOrderByLimitOptLabel` 在 `!pWInfo->bOrderedInnerLoop` 时返回 `pWInfo->iContinue`，否则返回 `pInner->pRJ ? pWInfo->iContinue : pInner->addrNxt`。**架构推论：**跳到次内层循环的续点能省掉内层循环剩余的行，前提是内层循环递增出序（`bOrderedInnerLoop`）且不是右外连接的右侧（`pRJ`）；任一前提不成立就退回最内层循环的续点，函数注释明确说「总是返回最内层循环的续点在正确性上是安全的」。

- **源码事实：**`sqlite3ResolveOrderGroupBy` 用 `db->aLimit[SQLITE_LIMIT_COLUMN]` 限制 ORDER BY 项数。**架构推论：**排序项数直接决定 KeyInfo 的字段数与排序比较的开销，SQLite 把它绑定到与列数相同的运行期上限上，避免用户构造超长 ORDER BY 列表放大编译期与运行期开销。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/select.c` | `sqlite3CopySortOrder` | 把 `p2` 的 `KEYINFO_ORDER_DESC` 标志搬到 `p1`（要求两者项数相等），供 `sqlite3ExprListCompare` 对齐排序方向后再判定表达式列表是否相同 |
| `src/select.c` | `sqlite3ConvertedOrderByColl` | 复合 SELECT 被 `convertCompoundSelectToSubquery` 改写成子查询后，为搬回父查询的 ORDER BY 项补上 `multiSelectCollSeq` 取到的 COLLATE 包装 |
| `src/select.c` | `multiSelectCollSeq` | 递归取复合 SELECT 第 `iCol` 列生效的排序规则，规则来自最左分支中最先带排序规则的项 |
| `src/select.c` | `multiSelectByMergeKeyInfo` | 为复合 SELECT 的 ORDER BY 构造 KeyInfo，逐项写入 `aColl` 与 `aSortFlags`，排序规则为空时回退到 `db->pDfltColl` |
| `src/select.c` | `computeLimitRegisters` | 把 `Select.pLimit` 展开成 `iLimit`/`iOffset`/`LIMIT+OFFSET` 三个寄存器，常量 LIMIT 走 `OP_Integer`，非常量走 `OP_MustBeInt` |
| `src/select.c` | `pushOntoSorter` | 把一行推入排序器；带 LIMIT 时按 `LIMIT+OFFSET` 条裁剪，用 `OP_Last`/`OP_IdxLE`/`OP_Delete` 淘汰最大记录 |
| `src/select.c` | `generateSortTail` | 生成排序输出循环：Sorter 分支走 `OP_SorterSort`/`OP_SorterData`/`OP_SorterNext`，临时 B 树分支走 `OP_Sort`/`OP_Next` 并调 `codeOffset` 跳过 OFFSET 行 |
| `src/select.c` | `codeOffset` | 在输出侧跳过前 `iOffset` 行，发 `OP_IfPos` |
| `src/select.c` | `convertCompoundSelectToSubquery` | 把带 COLLATE 的复合 SELECT 改写成「父查询 + 子查询」，置 `SF_Converted`，是 `sqlite3ConvertedOrderByColl` 的触发前提 |
| `src/select.c` | `minMaxQuery` | 识别 min/max 聚合并给新造的 ORDER BY 列表打 `KEYINFO_ORDER_BIGNULL` 或 `KEYINFO_ORDER_DESC` 标志 |
| `src/select.c` | `sqlite3Select` | 汇总点：消费 `sqlite3WhereIsOrdered` 的回报决定是否丢弃排序，按 `p->iLimit==0` 决定 `OP_OpenEphemeral` 是否改成 `OP_SorterOpen` |
| `src/resolve.c` | `resolveOrderGroupBy` | 名字解析阶段把 ORDER BY 项与结果集列对应，填 `u.x.iOrderByCol` |
| `src/resolve.c` | `sqlite3ResolveOrderGroupBy` | ORDER BY 项数超 `db->aLimit[SQLITE_LIMIT_COLUMN]` 时报 `too many terms in ORDER BY clause` |
| `src/where.c` | `sqlite3WhereIsOrdered` | 回报被索引满足的 ORDER BY 项数：0 表示需全排序，等于项数表示无需排序，中间值表示分块排序 |
| `src/where.c` | `sqlite3WhereOrderByLimitOptLabel` | 给出 ORDER BY LIMIT 优化的跳转目标，前提不成立时退回最内层循环续点 |
| `src/vdbe.c` | `OP_OffsetLimit` | 计算 `LIMIT+OFFSET`；LIMIT 非正或和溢出时置 `-1`，意味着计数永不到零、语句不终止 |

## 与主核 / AI 的关系

本页对应的是 SQLite 把「排序键的表达」与「截断计数」分开处理的做法：排序方向靠 `sortFlags` 里的 `KEYINFO_ORDER_DESC` 表达、靠 `iOrderByCol` 表达「这一项其实是结果集的第几列」，截断靠 `iLimit`/`iOffset` 两个寄存器加跳转标签表达。主核（`engine/db/` 自研多模态数据库内核）的查询层在形态上对应：排序算子也有自己的排序键列表，LIMIT 也在执行计划里作为算子参数存在。两边的差别在于 SQLite 把「顺序能否免排序」的判定完全下放给 WHERE 层并通过返回值回传，主核的排序算子则需要自己决定是走索引有序扫描还是显式排序节点。

AI 侧没有直接对应实现。本页可作为 AI 理解 SQLite「排序方向用位标志表达、截断用寄存器计数表达」这一设计模式的材料：前者让 `sqlite3ExprListCompare` 能在比较表达式列表时把排序方向一并纳入判定，后者让 LIMIT 的语义可以用三条 VDBE 指令（`OP_IfNot` / `OP_DecrJumpZero` / `OP_OffsetLimit`）在多个执行路径上复用。

## 参考

- [SQLite 官方文档：SELECT 语句](https://sqlite.org/lang_select.html) —— ORDER BY / LIMIT / OFFSET 的标准语义与复合 SELECT 规则
- [SQLite 官方文档：查询优化器概览](https://sqlite.org/optoverview.html) —— 「ORDER BY Optimizations」与「Partial ORDER BY via Index」两节，说明索引满足顺序与分块排序的代价模型
- [SQLite 官方文档：表达式](https://sqlite.org/lang_expr.html) —— ORDER BY 项可写成结果集列序号或 `AS` 别名的语法依据
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明排序器与 WHERE 规划器在流水线上的位置
- [SQLite 源码：src/select.c](https://github.com/sqlite/sqlite/blob/master/src/select.c) —— 本页全部 SQLite 侧符号的实现文件
- [SQLite 源码：src/where.c](https://github.com/sqlite/sqlite/blob/master/src/where.c) —— `sqlite3WhereIsOrdered` 与 `sqlite3WhereOrderByLimitOptLabel` 的实现
