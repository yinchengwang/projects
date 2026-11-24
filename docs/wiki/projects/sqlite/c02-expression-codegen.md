---
project: sqlite
feature: expression-codegen
page_type: feature
schema_version: 1
refs:
  - path: src/expr.c
    symbols: [sqlite3ExprCode, sqlite3ExprCodeGetColumn, sqlite3ExprCodeTarget]
---

# 表达式代码生成

## 一句话职责

把已经解析好的表达式树翻译成一串 VDBE 操作码（常量入寄存器、列值取值、函数调用、比较与逻辑运算），并保证结果落在调用方指定的寄存器里。

## 机制是什么

SQLite 的执行模型是「编译成字节码，再由虚拟机跑字节码」。表达式这一层就是「编译」与「运行」的交界处：解析器产出 `Expr` 树之后，代码生成器把它变成寄存器上的操作序列。

这一层的核心约定是**寄存器模型**。VDBE 程序有一组固定编号的寄存器，`Parse.nMem`（`src/sqliteInt.h`）记录已用寄存器数量，断言 `target>0 && target<=pParse->nMem` 出现在几乎每个代码生成入口。代码生成函数不直接返回「值」，而是把值写进某个寄存器，调用方再从那个寄存器读。

这一层要处理三类取值动作，正好对应题目给定的三条边界：

- **常量**（整数、浮点、字符串、BLOB、NULL）：常量不依赖行数据，直接在目标寄存器上物化。
- **列值**（`TK_COLUMN`）：需要游标指向当前行，用 `OP_Column` 之类取出。
- **函数调用的取值动作**（`TK_FUNCTION`）：先把实参逐个求值到一段连续寄存器，再发出 `OP_Function` / `OP_PureFunc`。

除这三类，还有两大类非叶子节点需要生成**分支与跳转**：比较运算（`TK_LT` / `TK_EQ` 等）和逻辑运算（`TK_AND` / `TK_OR`）。前者生成带条件跳转的 `OP_Eq` / `OP_Lt` 一类，后者生成 `OP_And` / `OP_Or` 加上短路跳转。

整层的入口是一对函数，区别在于「结果寄存器是否被保证」：

- `sqlite3ExprCodeTarget()` 是主体。它**不保证**结果落在 `target`：函数头注释写明「there is no guarantee that results will be stored in target. The result might be stored in some other register if it is convenient to do so」，返回值才是结果实际所在的寄存器号。
- `sqlite3ExprCode()` 是薄包装。它调用 `sqlite3ExprCodeTarget()` 拿到 `inReg`，若 `inReg!=target` 就补一条搬运指令，把结果搬回 `target`。函数头注释写明「The results are guaranteed to appear in register target」。

这个「两段式」是整层的核心设计：内层可以在任何方便的寄存器上算，外层负责归位。

## 怎么实现的

### 主体：`sqlite3ExprCodeTarget()`

`src/expr.c` 的 `sqlite3ExprCodeTarget()` 是一个以 `switch(op)` 为骨架的大函数，`op` 取自 `pExpr->op`（表达式节点的类型码，`TK_` 前缀）。函数开头的断言是 `assert( target>0 && target<=pParse->nMem )` 与 `assert( v!=0 )`，其中 `v` 是 `pParse->pVdbe`，即正在构建中的虚拟机。

函数体开头有三层前置处理：

1. `pExpr==0` 时 `op = TK_NULL`，直接走 NULL 分支。
2. 若 `pParse->pIdxEpr!=0` 且表达式不是叶子（`!ExprHasProperty(pExpr, EP_Leaf)`），则尝试 `sqlite3IndexedExprLookup(pParse, pExpr, target)`：命中表达式索引时直接返回该函数写入的寄存器号，跳过整棵子树。`sqlite3IndexedExprLookup()` 在 `src/expr.c` 中声明为 `static SQLITE_NOINLINE int`，函数头注释说明它「check to see if pExpr is one of the indexed expressions on pParse->pIdxEpr」，命中则从索引游标读值，未命中返回负数。函数体里还有一条排除：若 `sqlite3ExprIsConstant(pParse, pExpr)` 为真则 `return -1`，注释写明这是为了「prevent an attempt to use an index value outside of the loop that uses the index」，并标注了两个 bug 号 `2026-10-11T19:26:40Z` 与 `2026-11-03T02:12:43Z`。
3. `assert( op!=TK_ORDER )`，然后进 `switch`。

### 常量分支

- **整数** `TK_INTEGER` → `codeInteger(pParse, pExpr, 0, target)`。`codeInteger()` 是 `static void`，函数体分两条路：若 `pExpr->flags & EP_IntValue` 则直接取 `pExpr->u.iValue` 发 `OP_Integer`；否则用 `sqlite3DecOrHexToI64(z, &value)` 解析文本。解析结果 `c` 有特殊语义：`c==3` 表示值过大已按 `SMALLEST_INT64` 饱和（`if( negFlag ){ value = c==3 ? SMALLEST_INT64 : -value; }`）；`(c==3 && !negFlag) || (c==2) || (negFlag && value==SMALLEST_INT64)` 三个条件命中任一即走浮点路径，由 `codeReal()` 发 `OP_Real`。
- **浮点** `TK_FLOAT` → `codeReal(v, pExpr->u.zToken, 0, target)`。`codeReal()` 内部用 `sqlite3AtoF(z, &value)`，函数体里有断言 `assert( !sqlite3IsNaN(value) )`，注释写明「the new AtoF never returns NaN」。
- **字符串** `TK_STRING` → `sqlite3VdbeLoadString(v, target, pExpr->u.zToken)`。
- **BLOB** `TK_BLOB` → 由 `sqlite3HexToBlob()` 把十六进制字面量转成 blob，再发 `OP_Blob`；长度是 `n/2`，其中 `n = sqlite3Strlen30(z) - 1`。
- **NULL** `TK_NULL`（默认分支）→ `sqlite3VdbeAddOp2(v, OP_Null, 0, target)`。默认分支的注释写明这是为了让非法节点「be handled sanely and not crash」，但仍保留断言 `assert( op==TK_NULL || op==TK_ERROR || pParse->db->mallocFailed )`。
- **NULL 区间** `TK_NULLS` → `sqlite3VdbeAddOp3(v, OP_Null, 0, target, target + pExpr->y.nReg - 1)`，一次把 `pExpr->y.nReg` 个寄存器置 NULL。

### 列值分支：`sqlite3ExprCodeGetColumn()`

`TK_COLUMN` 分支最终调用 `sqlite3ExprCodeGetColumn(pParse, pExpr->y.pTab, pExpr->iColumn, iTab, target, pExpr->op2)`，返回寄存器号。

`sqlite3ExprCodeGetColumn()` 的函数头注释写明两点契约：「There must be an open cursor to pTab in iTable when this routine is called. If iColumn<0 then code is generated that extracts the rowid.」

它的函数体很短：

```
sqlite3ExprCodeGetColumnOfTable(pParse->pVdbe, pTab, iTable, iColumn, iReg);
if( p5 ){
  VdbeOp *pOp = sqlite3VdbeGetLastOp(pParse->pVdbe);
  if( pOp->opcode==OP_Column ) pOp->p5 = p5;
  if( pOp->opcode==OP_VColumn ) pOp->p5 = (p5 & OPFLAG_NOCHNG);
}
return iReg;
```

即：**先生成取列指令，再回头改这条指令的 P5 标志位**。`p5` 的来源是 `pExpr->op2`，断言要求它只含 `OPFLAG_NOCHNG` / `OPFLAG_TYPEOFARG` / `OPFLAG_LENGTHARG` 三种位（`assert( (p5 & (OPFLAG_NOCHNG|OPFLAG_TYPEOFARG|OPFLAG_LENGTHARG))==p5 )`）。这些常量的值在 `src/sqliteInt.h`：`OPFLAG_NOCHNG` 为 `0x01`，`OPFLAG_LENGTHARG` 为 `0x40`，`OPFLAG_TYPEOFARG` 为 `0x80`，`OPFLAG_BYTELENARG` 为 `0xc0`。

`sqlite3ExprCodeGetColumnOfTable()`（`src/expr.c`）负责真正决定发哪条指令：

- `iCol<0` 或 `iCol==pTab->iPKey` → 发 `OP_Rowid`，并用 `VdbeComment()` 写注释 `"%s.rowid"`。
- 虚表（`IsVirtual(pTab)`）→ 发 `OP_VColumn`，此时 `OP_VColumn` 的 P5 只保留 `OPFLAG_NOCHNG` 位。
- 虚生成列（`COLFLAG_VIRTUAL`）→ 转 `sqlite3ExprCodeGeneratedColumn()`。
- 无 rowid 表（`!HasRowid(pTab)`）→ 列号经 `sqlite3TableColumnToIndex(sqlite3PrimaryKeyIndex(pTab), iCol)` 转换。
- 普通表 → 列号经 `sqlite3TableColumnToStorage(pTab, iCol)` 转换，然后发 `OP_Column`，紧跟一条 `sqlite3ColumnDefault(v, pTab, iCol, regOut)`。

### 函数调用分支：`TK_FUNCTION`

`TK_FUNCTION` 分支（`src/expr.c`）是取值动作里最复杂的一段，顺序如下：

1. **窗口函数短路**：`if( ExprHasProperty(pExpr, EP_WinFunc) ) return pExpr->y.pWin->regResult;`——直接返回窗口函数的结果寄存器，不生成任何调用。
2. **常量折叠**：`if( ConstFactorOk(pParse) && sqlite3ExprIsConstantNotJoin(pParse,pExpr) )` 则 `return sqlite3ExprCodeRunJustOnce(pParse, pExpr, -1);`。注释写明理由是「SQL functions can be expensive. So try to avoid running them multiple times if we know they always give the same result」。
3. **查函数定义**：`pDef = sqlite3FindFunction(db, zId, nFarg, enc, 0)`。查不到时调 `sqlite3ErrorMsg(pParse, "unknown function: %#T()", pExpr)`。
4. **内联函数**：`if( (pDef->funcFlags & SQLITE_FUNC_INLINE)!=0 && ALWAYS(pFarg!=0) )` 则 `return exprCodeInlineFunction(...)`——内联函数不走 VDBE 调用指令，直接在代码生成期展开。
5. **常量实参掩码**：循环实参，`if( i<32 && sqlite3ExprIsConstant(pParse, pFarg->a[i].pExpr) ) constMask |= MASKBIT32(i);`。`i<32` 是硬编码上限，超出 32 个实参后常量掩码不再覆盖后面的实参。
6. **实参寄存器分配**：`if( constMask ){ r1 = pParse->nMem+1; pParse->nMem += nFarg; } else { r1 = sqlite3GetTempRange(pParse, nFarg); }`。有常量实参时用永久寄存器段（这样 `OP_Function` 的 P1 掩码才能在运行期跳过这些参数），否则用临时寄存器段。
7. **实参求值**：`sqlite3ExprCodeExprList(pParse, pFarg, r1, 0, SQLITE_ECEL_FACTOR)`。
8. **虚表函数重载**：`sqlite3VtabOverloadFunction(db, pDef, nFarg, ...)`。中缀函数（`LIKE` / `GLOB` / `REGEXP` / `MATCH`，带 `EP_InfixFunc`）用**第二个**实参去检测重载，注释解释原因：「The expression "A glob B" is equivalent to "glob(B,A). We want to use the A in "A glob B" to test for function overloading. But we use the B term in "glob(B,A)."」
9. **发出调用指令**：`sqlite3VdbeAddFunctionCall(pParse, constMask, r1, target, nFarg, pDef, pExpr->op2)`。
10. **释放实参寄存器**：`constMask==0` 时 `sqlite3ReleaseTempRange(pParse, r1, nFarg)`；否则 `sqlite3VdbeReleaseRegisters(pParse, r1, nFarg, constMask, 1)`——带上掩码，让常量实参所在的寄存器不被回收。

`sqlite3VdbeAddFunctionCall()` 在 `src/vdbeaux.c`：它分配 `SZ_CONTEXT(nArg)` 大小的 `sqlite3_context`，然后发 `sqlite3VdbeAddOp4(v, eCallCtx ? OP_PureFunc : OP_Function, p1, p2, p3, (char*)pCtx, P4_FUNCCTX)`。函数头注释列出了 `eCallCtx` 的取值含义：`NC_IsCheck` 表示由检查约束调用、`NC_IdxExpr` 表示作为索引表达式的一部分、`NC_PartIdx` 表示用于部分索引的 WHERE 子句、`NC_GenCol` 表示在计算生成列值时调用，`0` 是常规调用。

### 比较与逻辑分支

比较运算（`TK_LT` / `TK_LE` / `TK_GT` / `TK_GE` / `TK_NE` / `TK_EQ`）的分支：

- `TK_IS` / `TK_ISNOT` 先归一化为 `TK_EQ` / `TK_NE`，并置 `p5 = SQLITE_NULLEQ`，然后共用同一段代码。
- 左操作数是向量（`sqlite3ExprIsVector(pLeft)`）时走 `codeVectorCompare(pParse, pExpr, target, op, p5)`。
- 否则先 `sqlite3VdbeAddOp2(v, OP_Integer, 1, inReg)` 把结果寄存器预置为真（1），再用 `codeCompare(pParse, pLeft, pExpr->pRight, op, r1, r2, sqlite3VdbeCurrentAddr(v)+2, p5, ExprHasProperty(pExpr,EP_Commuted))` 生成比较跳转。
- 比较结束后：若 `p5==SQLITE_NULLEQ` 则发 `OP_Integer, 0, inReg`；否则发 `OP_ZeroOrNull` 置 `inReg`，若操作数中含子查询（`addrIsNull` 非零）还要补一段跳转，把 `inReg` 置 NULL。

`codeCompare()`（`src/expr.c` 的 `static int`）负责挑比较序列：若 `isCommuted` 则 `sqlite3BinaryCompareCollSeq(pParse, pRight, pLeft)`，否则 `(pParse, pLeft, pRight)`。然后 `sqlite3VdbeAddOp4(pParse->pVdbe, opcode, in2, dest, in1, (void*)p4, P4_COLLSEQ)` 发指令，`sqlite3VdbeChangeP5()` 设置 P5。

逻辑运算 `TK_AND` / `TK_OR` 交给 `exprCodeTargetAndOr(pParse, pExpr, target, &regFree1)`，该函数声明为 `static SQLITE_NOINLINE int`，函数头注释说明它「leaving a boolean result in a register」并「Use short-circuit evaluation to avoid computing both operands, if possible」。它内部：

- 先尝试 `sqlite3ExprSimplifiedAndOr(pExpr)`，若得到化简后的等价表达式 `pAlt != pExpr`，则直接对 `pAlt` 求值再发 `OP_And`（`sqlite3VdbeAddOp3(v, OP_And, r1, r1, target)`）。
- 否则 `skipOp = op==TK_AND ? OP_IfNot : OP_If`，先算一个操作数，再用 `skipOp` 跳过另一个操作数；跳过的分支里发 `OP_Or, regSS, regSS, target` 填短路值。

### 临时寄存器与常量外提

**临时寄存器**由 `sqlite3GetTempReg()` / `sqlite3ReleaseTempReg()` 管理（`src/expr.c`）：`sqlite3GetTempReg()` 在 `pParse->nTempReg==0` 时 `return ++pParse->nMem;`，否则从 `pParse->aTempReg[]` 栈里弹出。`sqlite3ReleaseTempReg()` 把它压回栈，并调 `sqlite3VdbeReleaseRegisters(pParse, iReg, 1, 0, 0)`。成块的分配用 `sqlite3GetTempRange()` / `sqlite3ReleaseTempRange()`。

`sqlite3ExprCodeTemp()` 是「求值到一个临时寄存器」的封装。它先把 `pExpr` 过一遍 `sqlite3ExprSkipCollateAndLikely()`（跳过 `TK_COLLATE` 与 `unlikely()` 之类的包装），然后：

```
if( ConstFactorOk(pParse) && ALWAYS(pExpr!=0)
 && pExpr->op!=TK_REGISTER
 && sqlite3ExprIsConstantNotJoin(pParse, pExpr) ){
  *pReg = 0;
  r2 = sqlite3ExprCodeRunJustOnce(pParse, pExpr, -1);
}else{
  int r1 = sqlite3GetTempReg(pParse);
  r2 = sqlite3ExprCodeTarget(pParse, pExpr, r1);
  if( r2==r1 ){ *pReg = r1; }
  else { sqlite3ReleaseTempReg(pParse, r1); *pReg = 0; }
}
```

注意结果寄存器不等于临时寄存器时，临时寄存器会被立刻释放，且 `*pReg` 置 0（表示「没有可释放的临时寄存器」）。

**常量外提**由 `sqlite3ExprCodeRunJustOnce()` 实现（`src/expr.c` 的 `int` 函数）。函数头注释写明：若表达式含函数（可能抛异常）就用 `OP_Once` 指令保证只执行一次；否则把代码外提到 prepared statement 的初始化段。若 `regDest>0` 则结果固定放该寄存器且不可复用；若 `regDest<0` 则可自由选址，两个相同表达式可能编码到同一寄存器（因为都被外提到初始化段）。函数体里的复用判定是 `pItem->fg.reusable && sqlite3ExprCompare(0, pItem->pExpr, pExpr, -1)==0`，命中就返回 `pItem->u.iConstExprReg`。

外提出来的代码由 `src/build.c` 在编译收尾时统一发出：`if( pParse->pConstExpr )` 块里遍历 `pEL`，先 `pParse->okConstFactor = 0;` 禁止再次外提，再对每个表达式调 `sqlite3ExprCode(pParse, pEL->a[i].pExpr, pEL->a[i].u.iConstExprReg)`。`pParse->okConstFactor` 这个 1 位标志位在 `src/sqliteInt.h` 中声明，宏 `ConstFactorOk(P)` 定义为 `((P)->okConstFactor)`。

### 表达式列表：`sqlite3ExprCodeExprList()`

`sqlite3ExprCodeExprList()`（`src/expr.c`）把一个表达式列表逐项压进从 `target` 开始的连续寄存器。标志位定义在 `src/sqliteInt.h`：`SQLITE_ECEL_DUP` 为 `0x01`（深拷贝而非浅拷贝）、`SQLITE_ECEL_FACTOR` 为 `0x02`（允许把常量实参外提）、`SQLITE_ECEL_REF` 为 `0x04`（用 `ExprList.u.x.iOrderByCol`）、`SQLITE_ECEL_OMITREF` 为 `0x08`（有 `iOrderByCol` 的项直接省略）。

函数体里的关键分支：

- `u8 copyOp = (flags & SQLITE_ECEL_DUP) ? OP_Copy : OP_SCopy;`——拷贝指令由标志位决定。
- `if( !ConstFactorOk(pParse) ) flags &= ~SQLITE_ECEL_FACTOR;`——外提能力被禁时，连标志位一起清掉。
- `SQLITE_ECEL_REF` 且 `pItem->u.x.iOrderByCol>0` 时：若同时有 `SQLITE_ECEL_OMITREF` 则直接 `i--; n--;`（该项不占寄存器），否则从 `j+srcReg-1` 拷到 `target+i`。
- `SQLITE_ECEL_FACTOR` 且表达式是常量时：`sqlite3ExprCodeRunJustOnce(pParse, pExpr, target+i)`。
- 其余情况走 `sqlite3ExprCodeTarget(pParse, pExpr, target+i)`，若返回寄存器不等于 `target+i` 则补拷贝指令。补拷贝时还有一次合并优化：若上条指令已是 `OP_Copy` 且 `pOp->p1+pOp->p3+1==inReg && pOp->p2+pOp->p3+1==target+i && pOp->p5==0`（`p5` 为 0 表示 do-not-merge 标志未置），就把 `pOp->p3++` 合并成一条多寄存器拷贝。

返回值 `n` 通常等于 `pList->nExpr`，但在 `SQLITE_ECEL_OMITREF` 下会减少——函数头注释写明「The number returned will usually be pList->nExpr but might be reduced if SQLITE_ECEL_OMITREF is defined.」

`src/select.c` 是 `SQLITE_ECEL_OMITREF` 的实际使用方：当 `pSort && hasDistinct==0 && eDest!=SRT_EphemTab && eDest!=SRT_Table` 时，置 `ecelFlags |= (SQLITE_ECEL_OMITREF|SQLITE_ECEL_REF)`，注释说明目的是「This allows the p->pEList field to be omitted from the sorted record, saving space and CPU cycles.」

### 归位：`sqlite3ExprCode()`

`sqlite3ExprCode()` 的函数体决定了用 `OP_Copy` 还是 `OP_SCopy`：

```
inReg = sqlite3ExprCodeTarget(pParse, pExpr, target);
if( inReg!=target ){
  Expr *pX = sqlite3ExprSkipCollateAndLikely(pExpr);
  if( ALWAYS(pX) && (ExprHasProperty(pX,EP_Subquery) || pX->op==TK_REGISTER) ){
    op = OP_Copy;
  }else{
    op = OP_SCopy;
  }
  sqlite3VdbeAddOp2(pParse->pVdbe, op, inReg, target);
}
```

即：结果来自子查询或来自 `TK_REGISTER` 时用 `OP_Copy`（完整复制值），其余用 `OP_SCopy`（浅拷贝，保留 subtype 等附属信息）。

`sqlite3ExprCodeCopy()` 是 `sqlite3ExprCode()` 的「不改动输入表达式」版本：先 `sqlite3ExprDup(db, pExpr, 0)` 复制一份，编码后 `sqlite3ExprDelete(db, pExpr)` 释放。

## 关键取舍

- **两段式接口，而不是一个函数搞定**：`sqlite3ExprCodeTarget()` 允许结果留在任意寄存器，`sqlite3ExprCode()` 负责归位。代价是多一次寄存器搬运（`OP_SCopy` / `OP_Copy`），换来内层实现不必关心调用方的寄存器布局，可以就地复用已有寄存器。

- **列值取值用「先生成再改标志」的两步法**：`sqlite3ExprCodeGetColumn()` 不直接构造带 P5 的 `OP_Column`，而是先调 `sqlite3ExprCodeGetColumnOfTable()` 生成指令，再通过 `sqlite3VdbeGetLastOp()` 找到刚生成的那条指令改 P5。这条路径覆盖了四类列（rowid、虚表列、虚生成列、普通列），各自的指令形态不同，只有这样才能统一地在事后打标志。代价是必须保证 `sqlite3ExprCodeGetColumnOfTable()` 生成的确实是最后一条指令。

- **`length()` / `typeof()` / `octet_length()` 避免加载整列数据**：`TK_FUNCTION` 分支里检查 `(pDef->funcFlags & (SQLITE_FUNC_LENGTH|SQLITE_FUNC_TYPEOF))!=0`，若唯一实参是 `TK_COLUMN` 或 `TK_AGG_COLUMN`，就把 `pFarg->a[0].pExpr->op2` 设为 `pDef->funcFlags & OPFLAG_BYTELENARG`。这三个标志的值经断言绑定：`OPFLAG_LENGTHARG`（`0x40`）、`OPFLAG_TYPEOFARG`（`0x80`）、`OPFLAG_BYTELENARG`（`0xc0`），断言还写明 `(OPFLAG_LENGTHARG|OPFLAG_TYPEOFARG)==OPFLAG_BYTELENARG`。运行时 `OP_Column` 见到这些标志就知道只算长度/类型而不取整列数据。

- **常量外提的边界由 `sqlite3ExprIsConstantNotJoin()` 划定**：这个判定（`src/expr.c` 的 `static int`，实为 `exprIsConst(pParse, p, 2)`）要求表达式是常量、**不**来自 LEFT JOIN 的 ON / USING 子句、**不**含常量传播优化造出的 `EP_FixedCol` 列。函数头注释解释为什么：不满足条件就不能放进 `pParse->pConstExpr` 列表、在 prepared statement 启动时求值一次。

- **常量外提与「只执行一次」是两套机制**：`sqlite3ExprCodeRunJustOnce()` 里，含函数的表达式走 `OP_Once` 指令（在字节码层保证每次 prepared statement 执行只跑一次），不含函数的表达式外提到初始化段（连字节码都不在主循环里）。前者解决「副作用只发生一次」，后者解决「重复计算浪费」。

- **常量实参用永久寄存器段而非临时寄存器段**：`TK_FUNCTION` 分支里 `constMask` 非零时走 `r1 = pParse->nMem+1; pParse->nMem += nFarg;`，不走 `sqlite3GetTempRange()`。因为 `OP_Function` 的 P1 是「常量实参掩码」，运行期据此跳过这些参数——参数寄存器必须在调用点仍然有效。释放时因此改用 `sqlite3VdbeReleaseRegisters(pParse, r1, nFarg, constMask, 1)`（带掩码释放）而不是 `sqlite3ReleaseTempRange()`。

- **常量实参掩码只覆盖前 32 个实参**：判定条件是 `i<32 && sqlite3ExprIsConstant(...)`。第 33 个及之后的常量实参不会被标进掩码，仍按普通实参求值。这是为了让 P1 这个 32 位字段能装下掩码。

- **短路求值优先于「把两个操作数都算出来」**：`exprCodeTargetAndOr()` 里对 `TK_AND` 用 `OP_IfNot`、对 `TK_OR` 用 `OP_If` 实现跳过。哪个操作数先算由 `exprEvalRhsFirst(pExpr)` 决定：先算右边时，左边能被右边的结果完全决定就跳过左边；先算左边时，只有右边是子查询（`EP_Subquery`）才允许跳过。

- **比较运算的结果寄存器预置为真**：`sqlite3ExprCodeTarget()` 在 `codeCompare()` 之前发 `sqlite3VdbeAddOp2(v, OP_Integer, 1, inReg)`。这样比较跳转不成立时结果天然为真（1），只需再处理 NULL 情形。这是「三值逻辑」在字节码层的落地方式。

- **表达式列表的拷贝指令可以合并**：`sqlite3ExprCodeExprList()` 里若上一条已是 `OP_Copy` 且源/目标寄存器连续，就把 `pOp->p3++` 扩成一条多寄存器拷贝。前提是 `pOp->p5==0`（do-not-merge 标志必须未置）。

- **`sqlite3ExprCodeTarget()` 用 `goto expr_code_doover` 代替递归**：`TK_COLLATE`、`TK_SPAN`、`TK_UPLUS` 三个分支都改写 `pExpr` 后跳回函数开头的标签。源码注释在 `TK_COLLATE` 处写「Prevent deep recursion.」，在 `TK_SPAN` / `TK_UPLUS` 处写「Prevent deep recursion. OSSFuzz.」——递归深度是被 fuzzing 发现过的问题。

- **默认分支不崩溃但仍留断言**：非法节点落到 `default:` 时发 `OP_Null`，注释写明「Make NULL the default case so that if a bug causes an illegal Expr node to be passed into this function, it will be handled sanely and not crash. But keep the assert() to bring the problem to the attention of the developers.」

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/expr.c | sqlite3ExprCode | 薄包装：调 sqlite3ExprCodeTarget 后把结果归位到 target；子查询/TK_REGISTER 用 OP_Copy，其余用 OP_SCopy |
| src/expr.c | sqlite3ExprCodeTarget | 代码生成主体：按 pExpr->op 分派到常量、列值、函数、比较、逻辑各分支；返回结果实际所在寄存器 |
| src/expr.c | sqlite3ExprCodeGetColumn | 取第 iColumn 列到 iReg：先调 sqlite3ExprCodeGetColumnOfTable 生成指令，再回头改写该指令的 P5 标志 |
| src/expr.c | sqlite3ExprCodeGetColumnOfTable | 决定发 OP_Rowid / OP_VColumn / OP_Column 中的哪一条，并做 rowid 表与普通表的列号转换 |
| src/expr.c | sqlite3ExprCodeExprList | 把表达式列表逐项压进连续寄存器；按 SQLITE_ECEL_DUP / FACTOR / REF / OMITREF 四个标志位分派 |
| src/expr.c | sqlite3ExprCodeTemp | 求值到临时寄存器；若表达式是常量则走 sqlite3ExprCodeRunJustOnce 外提 |
| src/expr.c | sqlite3ExprCodeCopy | sqlite3ExprCode 的不改动输入版本：先 sqlite3ExprDup 再编码后删除 |
| src/expr.c | sqlite3ExprCodeFactorable | 常量则走 sqlite3ExprCodeRunJustOnce，否则退到 sqlite3ExprCodeCopy |
| src/expr.c | sqlite3ExprCodeRunJustOnce | 常量外提：含函数用 OP_Once 保护，不含函数则登记进 pParse->pConstExpr 待初始化段求值 |
| src/expr.c | sqlite3ExprCodeGeneratedColumn | 计算生成列值：OP_IfNullRow 保护 + sqlite3ExprCodeCopy + 按需 OP_TypeCheck / OP_Affinity |
| src/expr.c | sqlite3ExprCodeLoadIndexColumn | 取索引第 iIdxCol 列：表达式索引用 sqlite3ExprCodeCopy，普通列转 sqlite3ExprCodeGetColumnOfTable |
| src/expr.c | sqlite3ExprCodeMove | 用一条 OP_Move 把 nReg 个寄存器整体搬到目标位置 |
| src/expr.c | codeInteger | 生成整数常量入寄存器；解析溢出时按 SMALLEST_INT64 饱和或退回浮点路径 |
| src/expr.c | codeReal | 用 sqlite3AtoF 解析浮点字面量后发 OP_Real；断言 AtoF 不返回 NaN |
| src/expr.c | codeCompare | 为比较运算挑比较序列并发出带 P4_COLLSEQ 的比较指令 |
| src/expr.c | exprCodeTargetAndOr | 生成 AND / OR 的短路求值：OP_IfNot / OP_If 跳过不需计算的一侧 |
| src/expr.c | exprPartidxExprLookup | 部分索引的列引用替换为索引表达式求值，附带 OP_Affinity 转换 |
| src/expr.c | sqlite3IndexedExprLookup | 表达式索引命中时直接从索引游标读值，跳过整棵子树 |
| src/expr.c | sqlite3ExprIsConstantNotJoin | 判定表达式是否可外提：常量、非 LEFT JOIN 的 ON/USING、不含 EP_FixedCol 列 |
| src/expr.c | sqlite3ExprSkipCollateAndLikely | 剥掉表达式根部的 TK_COLLATE 与 unlikely() 之类包装 |
| src/expr.c | sqlite3GetTempReg | 分配单个临时寄存器：栈空时递增 pParse->nMem，否则从 pParse->aTempReg 弹出 |
| src/expr.c | sqlite3ReleaseTempReg | 回收临时寄存器并压回 pParse->aTempReg 栈 |
| src/expr.c | sqlite3GetTempRange | 分配一段连续寄存器；nReg==1 时退化为 sqlite3GetTempReg |
| src/vdbeaux.c | sqlite3VdbeAddFunctionCall | 发 OP_Function 或 OP_PureFunc；eCallCtx 非零时选 OP_PureFunc，P4 挂 P4_FUNCCTX 的 sqlite3_context |
| src/build.c | sqlite3ExprCode（调用点） | 编译收尾时把 pParse->pConstExpr 里的外提常量逐条发出，先置 okConstFactor=0 禁止再外提 |
| src/select.c | sqlite3ExprCodeExprList（调用点） | 排序场景置 SQLITE_ECEL_OMITREF|SQLITE_ECEL_REF，使 pEList 不进入排序记录 |
| src/sqliteInt.h | ConstFactorOk | 宏，展开为 ((P)->okConstFactor)，控制是否允许常量外提 |
| src/sqliteInt.h | SQLITE_ECEL_DUP / SQLITE_ECEL_FACTOR / SQLITE_ECEL_REF / SQLITE_ECEL_OMITREF | 四个表达式列表编码标志，值分别为 0x01 / 0x02 / 0x04 / 0x08 |
| src/sqliteInt.h | OPFLAG_NOCHNG / OPFLAG_LENGTHARG / OPFLAG_TYPEOFARG / OPFLAG_BYTELENARG | OP_Column 的 P5 标志，值分别为 0x01 / 0x40 / 0x80 / 0xc0 |

## 与主核 / AI 的关系

**架构推论：** 本仓主核（`engine/db/`）的分层纪律要求 L2 catalog 层不依赖 query / spi / modalities，L3 query / spi 层不依赖 modalities。SQLite 的这一层正处在「语义分析」与「执行计划」之间：`src/expr.c` 里既有 `sqlite3ExprAffinity`、`sqlite3ExprAnalyzeAggregates` 这类语义分析入口，也有 `sqlite3ExprCodeTarget()` 这类纯代码生成入口，两类函数混在同一文件里，靠注释里的分工说明来区分。这提示主核若要拆出独立的「代码生成层」，不能只按文件切分，得按「是否只依赖语义层产物」来切。

**架构推论：** SQLite 这层最值得借鉴的不是具体 opcode，而是「返回值寄存器可能不是入参寄存器」这个约定。`sqlite3ExprCodeTarget()` 允许结果落在任意寄存器、由调用方读返回值归位，这个约定让内层实现可以自由复用寄存器。嵌入 SDK（`engine/src/sdk/`）若自己实现 SQL 引擎适配层，应把「请求一个结果槽位」和「结果实际在哪个槽位」在接口上分开，否则上层每处都要写一遍搬运逻辑——这正是 `sqlite3ExprCode()` 存在的理由。

**架构推论：** 常量外提机制（`sqlite3ExprCodeRunJustOnce()` → `pParse->pConstExpr` → `src/build.c` 收尾统一发出）与主核的「编译期常量折叠 / 执行期缓存」是同一类优化：把与参数无关的计算移出热路径。差别在于 SQLite 用的是「外提到字节码初始化段 + `OP_Once` 自改代码保证只跑一次」两套手段，而 `OP_Once` 是靠与 `OP_Init` 的 P1 比较实现自改的（见官方 bytecode 文档的 Self-Altering Code 一节），这在主核里对应的是「把一次性计算的时机交给执行引擎首帧」。

## 参考

- [SQLite Bytecode Engine](https://sqlite.org/opcode.html) —— VDBE 指令格式、寄存器模型、Self-Altering Code（`OP_Once` 与 `OP_Init` 的 P1 比较机制），以及 `OP_Column` / `OP_Function` / `OP_Copy` / `OP_SCopy` / `OP_Affinity` 等指令的官方说明。
- [sqlite/sqlite 源码：src/expr.c](https://github.com/sqlite/sqlite/blob/master/src/expr.c) —— `sqlite3ExprCode()` / `sqlite3ExprCodeTarget()` / `sqlite3ExprCodeGetColumn()` / `sqlite3ExprCodeExprList()` / `sqlite3ExprCodeRunJustOnce()` 的上游实现。
- [sqlite/sqlite 源码：src/vdbeaux.c](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c) —— `sqlite3VdbeAddFunctionCall()` 生成 `OP_Function` / `OP_PureFunc` 的上游实现。
- [SQLite Documentation: SQLite C Interface](https://sqlite.org/c3ref/intro.html) —— `sqlite3_prepare_v2()` 作为「SQL 到字节码的编译器」的官方定位说明。
