---
project: sqlite
feature: type-affinity
page_type: feature
schema_version: 1
refs:
  - path: src/expr.c
    symbols: [sqlite3AffinityType, sqlite3CompareAffinity]
  - path: src/build.c
    symbols: [sqlite3AffinityType]
---

# 类型亲和性

## 一句话职责

类型亲和性决定「一个值该以什么存储类参与比较和落盘」：SQLite 不做静态类型约束，而是给每列和每个表达式打一个亲和性标签（TEXT / NUMERIC / INTEGER / REAL / BLOB），在比较和插入这两个节点上按标签把值转换成合适的表示。

## 机制是什么

边界（依据条目 `boundary`）：负责列与表达式的类型亲和性判定及其在比较、插入时的转换规则；不负责具体值的存储表示（E 组）、不负责函数的类型约定（L 组）。

### 亲和性取值

亲和性的全部取值定义在 `src/sqliteInt.h`：

| 宏 | 值 | 注释字符 |
|---|---|---|
| `SQLITE_AFF_NONE` | `0x40` | `@` |
| `SQLITE_AFF_BLOB` | `0x41` | `A` |
| `SQLITE_AFF_TEXT` | `0x42` | `B` |
| `SQLITE_AFF_NUMERIC` | `0x43` | `C` |
| `SQLITE_AFF_INTEGER` | `0x44` | `D` |
| `SQLITE_AFF_REAL` | `0x45` | `E` |
| `SQLITE_AFF_FLEXNUM` | `0x46` | `F` |
| `SQLITE_AFF_DEFER` | `0x58` | `X` |

`src/sqliteInt.h` 的注释写明了两条设计约束：一是数值型（NUMERIC / INTEGER / REAL / FLEXNUM）被排成连续区间，这样「是不是数值亲和性」只是一次比较，由宏 `sqlite3IsNumericAffinity(X)` 判定为 `(X)>=SQLITE_AFF_NUMERIC`；二是这些值同时被用作 ASCII 字符（`'@'` 到 `'X'`），以便在编码成 P4 字符串操作数时仍然可读。`SQLITE_AFF_MASK` 定义为 `0x47`，用于从操作码 P5 里掩出亲和性位。

### 三条判定链

亲和性判定分三条互不相同的链，各自有独立的入口函数：

1. **声明类型 → 列亲和性**：`sqlite3AffinityType()`。输入是 CREATE TABLE 里写的类型名，输出是 `SQLITE_AFF_*` 之一。这是唯一一处把「字符串」变成「亲和性」的地方。
2. **表达式 → 表达式亲和性**：`sqlite3ExprAffinity()`。输入是表达式节点，输出是该表达式承载的亲和性，或 `0x00`（无亲和性）。
3. **两操作数 → 比较亲和性**：`sqlite3CompareAffinity()`。输入是一侧表达式的亲和性与另一侧的亲和性，输出是比较时真正施加的亲和性。

这三条链在 `src/expr.c` 和 `src/build.c` 里各自独立实现，下面的「怎么实现的」逐条展开。

## 怎么实现的

### 第一步：声明类型判定——sqlite3AffinityType

`sqlite3AffinityType(const char *zIn, Column *pCol)` 定义在 `src/build.c`。它不是查表，而是**逐字符扫描类型名**，用滑动窗口哈希判断关键子串是否出现。

初始值是 `char aff = SQLITE_AFF_NUMERIC`。扫描时维护 `u32 h`，每读一个字节就 `h = (h<<8) + sqlite3UpperToLower[x]`，即把最近 4 个字符（已转小写）左移进 32 位哈希。随后按序做以下判定（全部来自函数体本身）：

- `h` 等于 `'c','h','a','r'` 四字节的左移组合（即含 `CHAR`）→ `aff = SQLITE_AFF_TEXT`，并记下 `zChar = zIn`（当前位置，用于后面提取长度参数）。
- `h` 等于 `'c','l','o','b'` 组合（含 `CLOB`）→ `aff = SQLITE_AFF_TEXT`。
- `h` 等于 `'t','e','x','t'` 组合（含 `TEXT`）→ `aff = SQLITE_AFF_TEXT`。
- `h` 等于 `'b','l','o','b'` 组合（含 `BLOB`）**且**当前 `aff` 仍是 `SQLITE_AFF_NUMERIC` 或 `SQLITE_AFF_REAL` → `aff = SQLITE_AFF_BLOB`；若 `zIn[0]=='('` 则同样记下 `zChar`。
- `h` 等于 `'r','e','a','l'` 组合（含 `REAL`）**且** `aff==SQLITE_AFF_NUMERIC` → `aff = SQLITE_AFF_REAL`。
- `h` 等于 `'f','l','o','a'` 组合（含 `FLOA`）**且** `aff==SQLITE_AFF_NUMERIC` → `aff = SQLITE_AFF_REAL`。
- `h` 等于 `'d','o','u','b'` 组合（含 `DOUB`）**且** `aff==SQLITE_AFF_NUMERIC` → `aff = SQLITE_AFF_REAL`。
- `(h & 0x00FFFFFF)` 等于 `'i','n','t'` 三字节组合（即含 `INT`）→ `aff = SQLITE_AFF_INTEGER`，并 `break` 结束扫描。

后三条 REAL/FLOA/DOUB 判定被包在 `#ifndef SQLITE_OMIT_FLOATING_POINT` 里——浮点被编译掉时这些子串不再改变亲和性。

这个「顺序敏感」的设计是关键：`INT` 命中后直接 `break`，所以同时含 `INT` 与 `CHAR` 的类型名（如 `CHARINT`）结果是 INTEGER；`BLOB` 分支要求当前亲和性是 NUMERIC 或 REAL，所以先命中 `TEXT` 的类型名（如 `TEXTBLOB`）最终落在 TEXT；REAL/FLOA/DOUB 分支要求当前亲和性是 NUMERIC，所以它们只会在没有任何更强信号时才生效。**架构推论：** 这套「最后一次命中覆盖前一次、但部分分支带前置守卫」的写法把 SQL 标准里五条规则的优先级硬编码进了 if-else 链，而不是建成表驱动；扩展新类型名必须改这个函数。

`pCol` 参数非空时，函数还顺带估算字段大小：若 `aff<SQLITE_AFF_NUMERIC`（即 TEXT 或 BLOB 一类），若记下了 `zChar` 则向后扫描直到找到数字，用 `sqlite3GetInt32()` 读出长度 `k`，注释给出的换算是 `BLOB(k), VARCHAR(k), CHAR(k) -> r=(k/4+1)`；若没有 `zChar` 则取 `v = 16`，注释写 `BLOB, TEXT, CLOB -> r=5 (approx 20 bytes)`。

### 第二步：列亲和性的写入

`src/build.c` 里建列的代码在 `sType.n==0`（未指定类型）时把 `pCol->affinity` 设为传入的默认 `affinity`，注释写明此时默认亲和性是 BLOB、默认大小 4 字节；否则把类型名拷贝出来、调 `sqlite3Dequote()` 去引号，再调 `sqlite3AffinityType(zType, pCol)` 写入 `pCol->affinity`，并置 `COLFLAG_HASTYPE`。

反过来，`src/build.c` 中还有一处断言用亲和性做往返校验：生成列定义 SQL 时按 `azType[pCol->affinity - SQLITE_AFF_BLOB]` 取类型名（`azType` 数组按 BLOB / TEXT / NUMERIC / INTEGER / REAL 顺序对应 `" BLOB"` / `" TEXT"` / `" NUM"` / `" INT"` / `" REAL"`），随后断言 `pCol->affinity==SQLITE_AFF_BLOB || pCol->affinity==SQLITE_AFF_FLEXNUM || pCol->affinity==sqlite3AffinityType(zType, 0)`——即「由亲和性反推出的类型名再正算回来必须得到同一个亲和性」。

### 第三步：表达式亲和性——sqlite3ExprAffinity

`sqlite3ExprAffinity(const Expr *pExpr)` 定义在 `src/expr.c`，返回值是表达式承载的亲和性，或 `0x00` 表示无亲和性。函数体是一个 `while(1)` 循环，按 `pExpr->op` 依次判定：

- `TK_COLUMN` 或 `TK_AGG_COLUMN`（且 `pExpr->y.pTab!=0`）→ 返回 `sqlite3TableColumnAffinity(pExpr->y.pTab, pExpr->iColumn)`，即该列的亲和性。`sqlite3TableColumnAffinity()` 本身在 `src/expr.c`，列号越界时返回 `SQLITE_AFF_INTEGER`，否则返回 `pTab->aCol[iCol].affinity`。
- `TK_SELECT` → 递归取该子查询结果集第 0 个表达式的亲和性。
- `TK_CAST`（在 `#ifndef SQLITE_OMIT_CAST` 下）→ 返回 `sqlite3AffinityType(pExpr->u.zToken, 0)`，即 CAST 的目标类型走与列声明类型完全相同的判定路径。
- `TK_SELECT_COLUMN` → 递归取对应子查询结果集项的亲和性。
- `TK_VECTOR` → 取向量第 0 个子表达式的亲和性。
- `TK_FUNCTION` 且 `pExpr->affExpr==SQLITE_AFF_DEFER` → 取第一个参数的亲和性，但若为 `SQLITE_AFF_REAL` 则改返回 `SQLITE_AFF_NUMERIC`。源码注释说明这是 FULL JOIN 的 USING 列 `coalesce(L.x, R.x)`：因为值可能来自 R.x，所以不能假定是 REAL。
- 带 `EP_Skip` 或 `EP_IfNullRow` 属性的表达式（`TK_COLLATE` / `TK_IF_NULL_ROW`）→ 取 `pExpr->pLeft` 后 `continue` 重新判定，即 COLLATE 不改变亲和性。
- `TK_REGISTER` → 取 `pExpr->op2` 作为新的 `op` 继续判定。

其余情况跳出循环，末尾 `return pExpr->affExpr;`——即表达式自身携带的亲和性，`0x00` 表示无亲和性。

注意 `TK_COLUMN` 判定要求 `op==TK_COLUMN || (op==TK_AGG_COLUMN && pExpr->y.pTab!=0)`，而 `TK_AGG_COLUMN` 在 `pExpr->y.pTab==0` 时会落到循环末尾返回 `pExpr->affExpr`。

### 第四步：比较亲和性——sqlite3CompareAffinity

`sqlite3CompareAffinity(const Expr *pExpr, char aff2)` 定义在 `src/expr.c`，注释说明它的定位是「`pExpr` 是比较运算符的一个操作数，`aff2` 是另一个操作数的亲和性，返回比较时应当使用的亲和性」。

函数体只有两个分支：

1. **两侧都有亲和性**（`aff1>SQLITE_AFF_NONE && aff2>SQLITE_AFF_NONE`）：若 `sqlite3IsNumericAffinity(aff1)` 或 `sqlite3IsNumericAffinity(aff2)` 任一成立，返回 `SQLITE_AFF_NUMERIC`；否则返回 `SQLITE_AFF_BLOB`。源码注释写明：「Both sides of the comparison are columns. If one has numeric affinity, use that. Otherwise use no affinity.」
2. **只有一侧有亲和性**：断言 `aff1<=SQLITE_AFF_NONE || aff2<=SQLITE_AFF_NONE`，然后返回 `(aff1<=SQLITE_AFF_NONE ? aff2 : aff1) | SQLITE_AFF_NONE`。源码注释写明：「One side is a column, the other is not. Use the columns affinity.」——`| SQLITE_AFF_NONE` 即 `0x40`，这是 `src/sqliteInt.h` 注释里说的「可以被 OR 进亲和性而不改变亲和性本身」的附加位。

`sqlite3CompareAffinity()` 的返回值有两个直接下游：

- `comparisonAffinity()`（`src/expr.c` 静态函数）用它给比较表达式定亲和性：先取 `sqlite3ExprAffinity(pExpr->pLeft)`，若有 `pExpr->pRight` 则调 `sqlite3CompareAffinity(pExpr->pRight, aff)`；若没有右操作数且 `aff==0`，返回 `SQLITE_AFF_BLOB`。`comparisonAffinity()` 又被 `sqlite3IndexAffinityOk()` 用来判断「某索引的亲和性能不能支撑这个比较」。
- `binaryCompareP5()`（`src/expr.c` 静态函数）把它编码进比较操作码的 P5：`u8 aff = (char)sqlite3ExprAffinity(pExpr2); aff = (u8)sqlite3CompareAffinity(pExpr1, aff) | (u8)jumpIfNull;`。P5 的低 7 位由 `SQLITE_AFF_MASK`（`0x47`）掩出亲和性字符，其余位是 `SQLITE_NULLEQ` / `SQLITE_JUMPIFNULL` 这类标志。

### 第五步：运行时施加亲和性

亲和性最终落在两处运行时动作上。

**比较时**：`src/vdbe.c` 的比较操作码（`OP_Eq` 等）从 `pOp->p5 & SQLITE_AFF_MASK` 取回亲和性。若 `affinity>=SQLITE_AFF_NUMERIC` 且至少一侧是字符串（`MEM_Str`），则对纯字符串的那一侧调 `applyNumericAffinity(pIn1,0)` / `applyNumericAffinity(pIn3,0)`；若 `affinity==SQLITE_AFF_TEXT` 且至少一侧是字符串，则对已是字符串的一侧清掉 `MEM_Int|MEM_Real|MEM_IntReal` 标记，对是数值的一侧调 `sqlite3VdbeMemStringify(pIn1, encoding, 1)`。操作码注释明确写着：「the affinity conversions are stored back into the input registers P1 and P3. So this opcode can cause persistent changes to registers P1 and P3.」

`applyNumericAffinity(Mem *pRec, int bTryForInt)` 在 `src/vdbe.c`：先用 `sqlite3MemRealValueRC()` 解析字符串，`rc<=0` 直接返回（不是数字就保持原样）；`rc&0x10` 也直接返回（表示不是良构数字）；若 `(rc&2)==0 && alsoAnInt(pRec, rValue, &pRec->u.i)` 则置 `MEM_Int`，否则置 `MEM_Real` 并在 `bTryForInt` 为真时调 `sqlite3VdbeIntegerAffinity(pRec, 1)`。函数末尾清掉 `MEM_Str`，注释引 Ticket `343634942dd54ab57b7024` 说明原因：TEXT→NUMERIC 是「多对一」，原字符串表示未必是数值的最规范写法。

**插入时**：`src/insert.c` 的 `sqlite3TableAffinity(Vdbe *v, Table *pTab, int iReg)` 负责生成施加亲和性的字节码。对普通（非 STRICT）表，它取 `pTab->zColAff`（未算过则调 `sqlite3TableAffinityStr(0, pTab)` 现算），若该亲和性字符串非空且 `iReg>0` 就发一条 `OP_Affinity`（`sqlite3VdbeAddOp4(v, OP_Affinity, iReg, i, 0, zColAff, i)`）；若 `iReg==0` 则改为把上一条操作码（断言为 `OP_MakeRecord`）的 P4 改成亲和性字符串（`sqlite3VdbeChangeP4`）。STRICT 表走另一条路：插入 `OP_TypeCheck` 操作码做类型检查，源码注释说明 STRICT 下不做亲和性转换而是直接校验。

`sqlite3TableAffinityStr()`（`src/insert.c`）把每列的 `affinity` 字节拼成一串；注释写明只有非虚拟列（`COLFLAG_VIRTUAL` 未置）才进串。索引侧的对应函数是 `computeIndexAffStr()`，它为每列取亲和性并做两次夹逼：`if( aff<SQLITE_AFF_BLOB ) aff = SQLITE_AFF_BLOB; if( aff>SQLITE_AFF_NUMERIC) aff = SQLITE_AFF_NUMERIC;`——即索引亲和性串里只保留 BLOB 到 NUMERIC 之间的五种，行号列 `XN_ROWID` 固定取 `SQLITE_AFF_INTEGER`。

`src/vdbe.c` 的 `OP_Affinity` 操作码逐个寄存器地调 `applyAffinity(pIn1, zAffinity[0], encoding)`；若亲和性是 `SQLITE_AFF_REAL` 且寄存器带 `MEM_Int`，额外把 `pIn1->u.r = (double)pIn1->u.i` 并置 `MEM_Real`、清掉 `MEM_Int|MEM_Str`——这就是 REAL 亲和性强制整数转浮点的落点。

`applyAffinity(Mem *pRec, char affinity, u8 enc)`（`src/vdbe.c` 静态函数）的规则由其头部注释逐条写明：

- `SQLITE_AFF_INTEGER` / `SQLITE_AFF_REAL` / `SQLITE_AFF_NUMERIC`：若值还不是整数表示（`MEM_Int` 未置），且既不是 REAL 也不是 `MEM_IntReal`，则对字符串调 `applyNumericAffinity(pRec,1)`；若已经是 REAL 类表示且 `affinity<=SQLITE_AFF_REAL` 则调 `sqlite3VdbeIntegerAffinity(pRec, 0)`。注释特别说明「整数表示总是更优先，即使亲和性是 REAL，因为整数表示在磁盘上更省空间」。
- `SQLITE_AFF_FLEXNUM`：若值是文本则尝试转成某种数值，不做其他改动。
- `SQLITE_AFF_TEXT`：仅当值有整数或浮点表示而无字符串表示时才调 `sqlite3VdbeMemStringify(pRec, enc, 1)` 转成文本（BLOB 与 NULL 不转换），随后清掉 `MEM_Real|MEM_Int|MEM_IntReal` 标记。
- `SQLITE_AFF_BLOB` / `SQLITE_AFF_NONE`：空操作，值不变。

`src/vdbe.c` 另导出 `sqlite3ValueApplyAffinity(sqlite3_value *pVal, u8 affinity, u8 enc)`，它把 `sqlite3_value*` 强转成 `Mem*` 后直接调 `applyAffinity()`，供 `src/vdbemem.c` 等外部调用方使用。

### 第六步：视图与子查询的列亲和性

`sqlite3SubqueryColumnTypes()`（`src/select.c`）为视图 / 子查询的临时 `Table` 对象算亲和性：先 `pCol->affinity = sqlite3ExprAffinity(p)`；若结果为 `SQLITE_AFF_NONE` 或更小且复合 SELECT 还有后续分支，就沿 `pSelect->pNext` 链继续取后续分支的表达式亲和性；若最终仍无亲和性则取传入的默认 `aff`（调用方传入的默认值按断言只可能是 `SQLITE_AFF_NONE` 或 `SQLITE_AFF_BLOB`）。

复合 SELECT 的额外规则由 `sqlite3ExprDataType()`（`src/expr.c`）提供：它返回结果类型位掩码，`0x01` 表示可能是数值、`0x02` 表示可能是文本、`0x04` 表示可能是 BLOB。若列亲和性是 `SQLITE_AFF_TEXT` 而 `m&0x01` 非零（即存在数值型分支），则把亲和性降级为 `SQLITE_AFF_BLOB`；若列亲和性 `>=SQLITE_AFF_NUMERIC` 而 `m&0x02` 非零（即存在文本型分支），同样降级为 `SQLITE_AFF_BLOB`。此外若亲和性 `>=SQLITE_AFF_NUMERIC` 且原表达式是 `TK_CAST`，则改设为 `SQLITE_AFF_FLEXNUM`。

### 第七步：IN 运算符的亲和性约束

`src/expr.c` 里 `IN` 右侧是表时，代码用 `sqlite3TableColumnAffinity(pTab,iCol)` 取右侧表列的亲和性作为 `idxaff`，再用 `sqlite3CompareAffinity(pLhs, idxaff)` 算出实际比较亲和性 `cmpaff`，然后按 `cmpaff` 分派：`SQLITE_AFF_BLOB` 直接放行；`SQLITE_AFF_TEXT` 断言 `idxaff==SQLITE_AFF_TEXT` 后放行（源码注释解释：只有当右侧是 TEXT 且左侧无亲和性时才可能得到 TEXT）；其余情况置 `affinity_ok = sqlite3IsNumericAffinity(idxaff)`。任一列不满足就把 `affinity_ok` 清零，从而不选该表的任何索引。

## 关键取舍

**取舍一：用「类型名字符串 → 亲和性」的单向映射，而不是完整类型系统。** SQLite 不保存声明类型的原文语义，只保存一个亲和性字节。这让 CREATE TABLE 解析、CAST 表达式、视图列类型推断三条路径共用同一个 `sqlite3AffinityType()`，代价是 `VARCHAR(255)` 里的长度信息被丢弃（官方文档也说明 SQLite 不对字符串长度施加限制，除全局 `SQLITE_MAX_LENGTH` 外）。`sqlite3AffinityType()` 里 `pCol` 非空时会顺便估算 `szEst` 字段大小，但那只服务于优化器估算，不影响类型语义。

**取舍二：亲和性是「建议」而非「约束」，转换只在无损时进行。** 官方文档明确：任何列都可以存任何类型的值，亲和性只是「preferred storage class」。实现上体现为 `applyNumericAffinity()` 在字符串不像数字时直接 `return`（`rc<=0` 或 `rc&0x10`），保持原样而不强行转换。对比之下 `src/vdbemem.c` 的 `sqlite3VdbeMemCast()` 处理 CAST 时是强制的——函数头注释写明「Casting is different from applying affinity in that a cast is forced... even if that results in loss of data」，其实现按 `SQLITE_AFF_BLOB` / `SQLITE_AFF_NUMERIC` / `SQLITE_AFF_INTEGER` / `SQLITE_AFF_REAL` / 默认 TEXT 五个分支分别调 `sqlite3VdbeMemSetStr` 路径、`sqlite3VdbeMemNumerify()`、`sqlite3VdbeMemIntegerify()`、`sqlite3VdbeMemRealify()` 与 `sqlite3ValueApplyAffinity(pMem, SQLITE_AFF_TEXT, encoding)`。

**取舍三：比较亲和性的两条分支不追求「精确」，只追求「确定」。** `sqlite3CompareAffinity()` 只有两个分支：两侧都有亲和性时，任意一侧是数值亲和性就统一用 NUMERIC，否则统一用 BLOB；只有一侧有亲和性时用那一侧的亲和性。`src/expr.c` 里 `sqlite3IndexAffinityOk()` 进一步把亲和性分成三档来匹配索引：`aff<SQLITE_AFF_TEXT` 直接可用；`aff==SQLITE_AFF_TEXT` 要求索引亲和性也是 TEXT；其余情况要求索引亲和性是数值亲和性。**架构推论：** 这套「粗粒度但稳定」的判定让查询规划器不必穷举类型组合，代价是存在亲和性不同但实际可比较的情形被保守地排除在索引之外。

**取舍四：亲和性转换会就地改写寄存器，是有意的副作用。** `src/vdbe.c` 的比较操作码注释明确写着转换结果会写回 P1 与 P3 寄存器，「So this opcode can cause persistent changes to registers P1 and P3」。这避免了为转换分配临时寄存器，但要求使用方知道操作码有副作用。

**取舍五：REAL 亲和性的整数压缩优化在两个层次上分别实现。** 一处在 `applyAffinity()`：REAL/INTEGER/NUMERIC 下若值已是 REAL 类表示且 `affinity<=SQLITE_AFF_REAL`，调 `sqlite3VdbeIntegerAffinity(pRec, 0)`（不尝试转整数）；另一处在 `OP_Affinity` 操作码里：亲和性是 `SQLITE_AFF_REAL` 且寄存器带 `MEM_Int` 时，显式把整数转成 `double` 并清掉 `MEM_Int` 标记。官方文档说明这是一种内部优化：小浮点数会以整数形式落盘以省空间，读出时再转回浮点，SQL 层面不可见。

**取舍六：REAL 亲和性下超大整数会被提前转成 REAL。** `src/vdbemem.c` 的 `sqlite3ValueFromExpr()` 里有一段特判：若 `affinity==SQLITE_AFF_REAL` 且值是整数，且 `pVal->u.i<-140737488355328LL || pVal->u.i>140737488355327LL`，则调 `sqlite3VdbeMemRealify(pVal)`。这两个字面量就是 2 的 47 次方（140737488355328），注释说明原因是绝大多数情况下会由 `OP_RealAffinity` 操作码完成转换，但当值直接从表读出用于构造索引键时该操作码会被省略，因此必须在这里拿到真正的浮点值，否则比较结果会错——因为 SQLite 默认在比较前会把浮点转成整数，与「先转浮点」是相反方向。

**取舍七：复合 SELECT 的列亲和性被主动降级为 BLOB。** `sqlite3SubqueryColumnTypes()` 里的降级规则（TEXT 亲和性遇到数值分支、数值亲和性遇到文本分支，都降为 `SQLITE_AFF_BLOB`）是一种保守取舍：官方文档明确说明复合 SELECT 各分支亲和性不同时，「无法确定」会用哪个分支，且可能在不同版本、不同查询甚至同一查询的不同时间点变化，因此建议不要在复合 SELECT 中混用亲和性。**架构推论：** 降级到 BLOB 而不是报错，是 SQLite 用「语义降级」换取「查询永不因类型问题失败」的一贯风格。

**取舍八：亲和性串在索引里被夹逼到 BLOB..NUMERIC 区间。** `src/insert.c` 的 `computeIndexAffStr()` 里 `if( aff<SQLITE_AFF_BLOB ) aff = SQLITE_AFF_BLOB; if( aff>SQLITE_AFF_NUMERIC) aff = SQLITE_AFF_NUMERIC;`。原因是 `SQLITE_AFF_NONE`（`0x40`）与 `SQLITE_AFF_DEFER`（`0x58`）不适合作为索引列亲和性；而 `SQLITE_AFF_INTEGER` / `SQLITE_AFF_REAL` / `SQLITE_AFF_FLEXNUM` 在索引场景下按数值处理，统一夹到 `SQLITE_AFF_NUMERIC`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/build.c | `sqlite3AffinityType` | 声明类型名 → 列亲和性。逐字符扫描 + 4 字符滑动窗口哈希，按顺序匹配 CHAR/CLOB/TEXT/BLOB/REAL/FLOA/DOUB/INT；INT 命中即 break；非 NUMERIC 时顺带估算字段大小 |
| src/expr.c | `sqlite3AffinityType` | 同一函数的另一处调用点：`sqlite3ExprAffinity()` 处理 `TK_CAST` 时用它取 CAST 目标类型的亲和性 |
| src/expr.c | `sqlite3CompareAffinity` | 两操作数亲和性 → 比较亲和性。两侧都有亲和性时数值优先返回 `SQLITE_AFF_NUMERIC`，否则返回 `SQLITE_AFF_BLOB`；只有一侧有亲和性时取该侧并 `\| SQLITE_AFF_NONE` |
| src/expr.c | `sqlite3ExprAffinity` | 表达式 → 表达式亲和性。按 `TK_COLUMN`/`TK_SELECT`/`TK_CAST`/`TK_SELECT_COLUMN`/`TK_VECTOR`/`TK_FUNCTION`(DEFER)/`TK_COLLATE`/`TK_REGISTER` 分派，兜底返回 `pExpr->affExpr` |
| src/expr.c | `sqlite3TableColumnAffinity` | 表列号 → 列亲和性，列号越界返回 `SQLITE_AFF_INTEGER` |
| src/expr.c | `comparisonAffinity` | 比较表达式整体亲和性，供 `sqlite3IndexAffinityOk` 判断索引可用性 |
| src/expr.c | `binaryCompareP5` | 把比较亲和性编码进比较操作码 P5，低位为亲和性字符，叠加 `SQLITE_JUMPIFNULL` 等标志 |
| src/expr.c | `sqlite3IndexAffinityOk` | 按三档规则判定索引亲和性能否支撑某比较 |
| src/expr.c | `sqlite3ExprDataType` | 返回表达式结果类型位掩码（`0x01` 数值 / `0x02` 文本 / `0x04` BLOB），供复合 SELECT 亲和性降级判定 |
| src/expr.c | IN 右侧表分支 | 用 `sqlite3TableColumnAffinity` + `sqlite3CompareAffinity` 校验 IN 与右侧表列亲和性是否匹配，不匹配则不选该表索引 |
| src/select.c | `sqlite3SubqueryColumnTypes` | 为视图 / 子查询的临时 `Table` 计算列亲和性；复合 SELECT 中亲和性冲突时降级为 `SQLITE_AFF_BLOB`，CAST 且数值亲和性时改为 `SQLITE_AFF_FLEXNUM` |
| src/insert.c | `sqlite3TableAffinity` | 生成施加亲和性的字节码：普通表发 `OP_Affinity` 或改写 `OP_MakeRecord` 的 P4；STRICT 表改插 `OP_TypeCheck` |
| src/insert.c | `sqlite3TableAffinityStr` | 把各列亲和性拼成字符串，仅含非虚拟列 |
| src/insert.c | `computeIndexAffStr` | 索引亲和性串，把每列亲和性夹逼到 `SQLITE_AFF_BLOB`..`SQLITE_AFF_NUMERIC`，行号列取 `SQLITE_AFF_INTEGER` |
| src/vdbe.c | `applyAffinity` | 亲和性 → 值转换的规则本体：数值类优先整数表示，TEXT 仅在有数值表示时转换并清数值标记，BLOB/NONE 空操作 |
| src/vdbe.c | `applyNumericAffinity` | 字符串 → 数值，仅在无损时转换；`rc<=0` 或 `rc&0x10` 时保持原样；转换后清 `MEM_Str` |
| src/vdbe.c | `sqlite3ValueApplyAffinity` | `applyAffinity()` 的 `sqlite3_value*` 版本导出接口 |
| src/vdbe.c | `OP_Affinity` | 按亲和性字符串逐寄存器施加亲和性；REAL 亲和性且值为整数时显式转 double |
| src/vdbe.c | 比较操作码分支 | 从 `pOp->p5 & SQLITE_AFF_MASK` 取亲和性，数值类对字符串侧调 `applyNumericAffinity`，TEXT 类对数值侧调 `sqlite3VdbeMemStringify` |
| src/vdbemem.c | `sqlite3VdbeMemCast` | CAST 的强制转换，与亲和性转换的「不丢信息才转」不同 |
| src/vdbemem.c | `sqlite3ValueFromExpr` | REAL 亲和性下，整数绝对值超出 2 的 47 次方时调 `sqlite3VdbeMemRealify` 提前转浮点 |
| src/sqliteInt.h | `SQLITE_AFF_NONE` / `SQLITE_AFF_BLOB` / `SQLITE_AFF_TEXT` / `SQLITE_AFF_NUMERIC` / `SQLITE_AFF_INTEGER` / `SQLITE_AFF_REAL` / `SQLITE_AFF_FLEXNUM` / `SQLITE_AFF_DEFER` | 亲和性取值常量，数值型连续排列以便一次比较判定 |
| src/sqliteInt.h | `sqlite3IsNumericAffinity` / `SQLITE_AFF_MASK` | 数值亲和性判定宏（`>= SQLITE_AFF_NUMERIC`）与 P5 掩码（`0x47`） |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里（`common/`、`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`、`modalities/`）没有与 SQLite 亲和性对位的结构。SQLite 的做法是「把类型判定压缩成一个字节的标签，然后把标签沿编译期传递到运行时」，标签只出现在三个地方：列定义（`Column.affinity`）、表达式（`Expr.affExpr`）、操作码 P5 的一位。主核如果采用运行期按 schema 查类型的模型，每次比较都要回表拿元数据，成本分布与 SQLite 完全不同。

**架构推论：** SQLite 的「转换只在无损时发生」这条规则（`applyNumericAffinity()` 在字符串不像数字时原样返回）对多模态场景有直接参考价值。向量、embedding、JSON 这类值既不是数字也不是文本，用「亲和性」这种单向标签刻画会失真；主核 `modalities/` 层若要为每种模态单独定义「亲和性」，就必须额外引入「不可转换」的显式状态，而 SQLite 的五种亲和性里没有这个位置。

**架构推论：** AI 生成 SQL 的场景下，亲和性是「同一段 SQL 在不同列类型下结果不同」的主要来源。`sqlite3CompareAffinity()` 的分支 1（两侧都有亲和性时数值优先）意味着 `WHERE text_col = 1` 与 `WHERE num_col = 1` 会走完全不同的比较路径，AI 若要推理查询结果，必须知道目标表的列声明类型。嵌入式 SDK（`engine/src/sdk/`）若把 SQLite 作为执行引擎对外暴露，AI 层要拿到列亲和性就得读 `sqlite3_table_column_metadata` 或 `PRAGMA table_info`，而 PRAGMA `table_info` 返回的类型名是声明类型原文，需要 AI 侧再实现一遍 `sqlite3AffinityType()` 的五条规则才能得到亲和性。

**架构推论：** `sqlite3AffinityType()` 的规则完全依赖字符串子串匹配，没有查表，这带来一个可预见的风险：类型名里的子串顺序会改变结果（`CHARINT` 是 INTEGER、`TEXTBLOB` 是 TEXT、`FLOATING POINT` 因结尾的 `INT` 而落到 INTEGER）。若 AI 自动生成列声明，「类型名看起来更精确」与「实际亲和性」会不一致，主核若要提供类型推荐能力，需要把这套子串规则显式建模而不是让 AI 猜。

## 参考

- [SQLite Datatypes In SQLite（Type Affinity 一节的官方规则与示例）](https://sqlite.org/datatype3.html)
- [SQLite STRICT Tables（刚性类型表与亲和性的对照）](https://sqlite.org/stricttables.html)
- [SQLite Architecture（SQL 文本经 tokenizer → parser → code generator → VDBE 的分层）](https://sqlite.org/arch.html)
- [SQLite VDBE Opcode Documentation（比较类操作码 P5 中亲和性位的说明）](https://sqlite.org/opcode.html)
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c)
- [sqlite/sqlite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c)
- [sqlite/sqlite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
- [sqlite/sqlite 源码：src/insert.c](https://github.com/sqlite/sqlite/blob/master/src/insert.c)
- [sqlite/sqlite 源码：src/sqliteInt.h](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)
