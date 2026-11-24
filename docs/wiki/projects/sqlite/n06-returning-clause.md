---
project: sqlite
feature: returning-clause
page_type: feature
schema_version: 1
refs:
  - path: src/build.c
    symbols: [sqlite3AddReturning]
  - path: src/trigger.c
    symbols: [sqlite3ProcessReturningSubqueries, sqlite3ReturningSubqueryCorrelated]
---

# RETURNING 子句

## 一句话职责

负责 DML 的 RETURNING 子句在编译期被识别并生成返回结果列；不负责 DML 本身的编译（D07/D08/D09）、不负责结果列的读取（E03）。

## 机制是什么

`RETURNING` 允许 `DELETE` / `INSERT` / `UPDATE` 在修改行的同时把结果集吐出来，例如 `DELETE FROM t RETURNING *` 会把被删掉的每一行返回给调用方。

SQLite 的实现方式**不是**给 DML 加一条新的结果集管线，而是把 RETURNING **伪装成一个 TEMP 触发器**。`src/build.c` 的 `sqlite3AddReturning` 函数注释写得很直白：这个例程为当前正在解析的 DML 语句创建一个特殊的 TEMP 触发器，该触发器对 DML 的每一行触发一次，触发器体内是一个结果集恰好等于 RETURNING 参数的 `SELECT` 语句；该触发器带 `Trigger.bReturning` 标志，opcode 是 `TK_RETURNING` 而不是 `TK_SELECT`，因此触发器代码生成器知道要特殊处理它；这个 TEMP 触发器在解析结束时自动删除。

这句话里有四个可验证的构件，逐一对应源码：

1. **一个 TEMP 触发器**：`sqlite3AddReturning` 把 `pRet->retTrig.pSchema` 与 `pRet->retTrig.pTabSchema` 都设成 `db->aDb[1].pSchema`（`db->aDb[1]` 就是 TEMP 库），并把 `pRet->retTrig.zName` 指向 `pRet->zName`；同时 `sqlite3HashInsert` 把它塞进 `db->aDb[1].pSchema->trigHash` 哈希表。
2. **一个 `TK_RETURNING` opcode**：`pRet->retTrig.op = TK_RETURNING;`，且 `pRet->retTStep.op = TK_RETURNING;`。
3. **一个 `TRIGGER_AFTER` 时机**：`pRet->retTrig.tr_tm = TRIGGER_AFTER;`，`TRIGGER_AFTER` 的值是 2（定义在 `src/sqliteInt.h`，同处 `TRIGGER_BEFORE` 为 1）。
4. **一个解析期清理钩子**：`sqlite3ParserAddCleanup(pParse, sqlite3DeleteReturning, pRet)`。

触发器的名字由 `sqlite3_snprintf(sizeof(pRet->zName), pRet->zName, "sqlite_returning_%p", pParse)` 生成，即把 `Parse*` 指针格式化进名字，保证同一次解析里唯一。`pRet->zName` 是 `Returning` 结构里的 `char zName[40]`，`sizeof(pRet->zName)` 就是 40——这是这个格式串的缓冲区上限来源。

`TK_RETURNING` 这个 opcode 在源码里只出现在两个地方：`src/build.c` 里设置触发器与触发步的 op，以及 `src/trigger.c` 里消费它。`src/sqliteInt.h` 的 `TriggerStep` 结构注释明确列出：当 `op == TK_RETURNING` 时，`pExprList` 存放 RETURNING 关键字之后的表达式列表。

**RETURNING 的结果列不在 DML 编译期直接产出，而是先落进一张临时表。** `src/trigger.c` 的 `codeReturningTrigger` 函数把每个受影响行算出的返回列打包成一条记录，`OP_Insert` 进一张 ephemeral 游标表（游标号 `pReturning->iRetCur`）；DML 全部执行完之后，再由 `src/build.c` 的代码生成阶段把这张 ephemeral 表 `OP_Rewind` 出来，逐列 `OP_Column`、逐行 `OP_ResultRow` 输出给客户端。

**边界**（与页面清单给出的 boundary 一致）：本页负责 DML 的 RETURNING 子句在编译期被识别并生成返回结果列；不负责 DML 本身的编译（D07/D08/D09）、不负责结果列的读取（E03）。

## 怎么实现的

### 语法识别：四条产生式，一个入口

RETURNING 的语法由 `src/parse.y` 的 Lemon 语法定义，四个产生式全部调用同一个函数 `sqlite3AddReturning`：

| 产生式 | 语句形态 | 动作 |
| --- | --- | --- |
| `where_opt_ret(A) ::= RETURNING selcollist(X).` | `DELETE ... RETURNING ...`（无 WHERE） | `{sqlite3AddReturning(pParse,X); A = 0;}` |
| `where_opt_ret(A) ::= WHERE expr(X) RETURNING selcollist(Y).` | `DELETE ... WHERE ... RETURNING ...` | `{sqlite3AddReturning(pParse,Y); A = X;}` |
| `upsert(A) ::= RETURNING selcollist(X).` | `INSERT ... RETURNING ...` | `{ A = 0; sqlite3AddReturning(pParse,X); }` |
| `returning ::= RETURNING selcollist(X).` | `INSERT ... ON CONFLICT ... RETURNING ...` | `{sqlite3AddReturning(pParse,X);}` |

`where_opt_ret` 与 `returning` 这两个非终结符各有一个空产生式（`where_opt_ret(A) ::= .` 与 `returning ::= .`），所以「没有 RETURNING」时 `sqlite3AddReturning` 根本不会被调用。

注意 `where_opt_ret` 的第一条产生式把 `A` 置 0——DELETE 语句的 WHERE 表达式被丢掉是正常的，RETURNING 分支不携带 WHERE。

### 注册：sqlite3AddReturning

`src/build.c` 的 `sqlite3AddReturning` 函数体按顺序做这些事：

**第 1 步：拒绝在触发器里用 RETURNING。**

```
if( pParse->pNewTrigger ){
  sqlite3ErrorMsg(pParse, "cannot use RETURNING in a trigger");
}else{
  assert( pParse->bReturning==0 || pParse->ifNotExists );
}
pParse->bReturning = 1;
```

`pParse->pNewTrigger` 非空表示当前正在解析一条 `CREATE TRIGGER` 语句。错误消息文本就是源码里的字面量 `"cannot use RETURNING in a trigger"`。注意 `pParse->bReturning = 1` 在 `if/else` **之外**——即使报了错，标志仍然置上，后续 `codeReturningTrigger` 会因 `pParse->nErr` 非零而跳过生成代码。

**第 2 步：分配 `Returning` 结构。**

```
pRet = sqlite3DbMallocZero(db, sizeof(*pRet));
if( pRet==0 ){
  sqlite3ExprListDelete(db, pList);
  return;
}
assert( !pParse->isCreate );
pParse->u1.d.pReturning = pRet;
pRet->pParse = pParse;
pRet->pReturnEL = pList;
```

`sqlite3DbMallocZero` 失败时把 `pList`（RETURNING 的表达式列表）交给 `sqlite3ExprListDelete` 释放后直接返回——这是 SQLite 处理 OOM 的惯用模式：**谁最后拿到所有权，谁负责在失败路径上释放**。`assert( !pParse->isCreate )` 表明 RETURNING 不允许出现在 `CREATE TABLE` 之类的语句里。

**第 3 步：登记清理钩子。**

```
sqlite3ParserAddCleanup(pParse, sqlite3DeleteReturning, pRet);
testcase( pParse->earlyCleanup );
if( db->mallocFailed ) return;
```

`sqlite3DeleteReturning`（`src/build.c`）是配套的清理函数，函数体是：

```
static void sqlite3DeleteReturning(sqlite3 *db, void *pArg){
  Returning *pRet = (Returning*)pArg;
  Hash *pHash;
  pHash = &(db->aDb[1].pSchema->trigHash);
  sqlite3HashInsert(pHash, pRet->zName, 0);
  sqlite3ExprListDelete(db, pRet->pReturnEL);
  sqlite3DbFree(db, pRet);
}
```

它做三件事：从 TEMP schema 的 `trigHash` 里把同名项删掉（`sqlite3HashInsert(pHash, pRet->zName, 0)` 的第三个参数为 0 表示删除该项）、释放 `pReturnEL` 表达式列表、释放 `Returning` 结构本身。这就是「TEMP 触发器在解析结束时自动删除」这句话的落点。

**第 4 步：填写触发器字段。**

```
sqlite3_snprintf(sizeof(pRet->zName), pRet->zName,
                 "sqlite_returning_%p", pParse);
pRet->retTrig.zName = pRet->zName;
pRet->retTrig.op = TK_RETURNING;
pRet->retTrig.tr_tm = TRIGGER_AFTER;
pRet->retTrig.bReturning = 1;
pRet->retTrig.pSchema = db->aDb[1].pSchema;
pRet->retTrig.pTabSchema = db->aDb[1].pSchema;
pRet->retTrig.step_list = &pRet->retTStep;
pRet->retTStep.op = TK_RETURNING;
pRet->retTStep.pTrig = &pRet->retTrig;
pRet->retTStep.pExprList = pList;
```

`retTrig.step_list` 指向 `retTStep`——触发器只有一个触发步，触发步的 `pExprList` 就是 RETURNING 的表达式列表。`pSchema` 与 `pTabSchema` 都指向 TEMP schema。

**第 5 步：插入 TEMP 触发器哈希表。**

```
pHash = &(db->aDb[1].pSchema->trigHash);
assert( sqlite3HashFind(pHash, pRet->zName)==0
        || pParse->nErr  || pParse->ifNotExists );
if( sqlite3HashInsert(pHash, pRet->zName, &pRet->retTrig)
        ==&pRet->retTrig ){
  sqlite3OomFault(db);
}
```

`sqlite3HashInsert` 的返回值等于传入的指针表示插入成功（否则返回已有项或 0）；失败则 `sqlite3OomFault(db)` 设置 OOM 标志。

### 从 TEMP 触发器到「真实触发器」：op 的第一次改写

`sqlite3AddReturning` 只知道「有 RETURNING」，还不知道这条 DML 是 DELETE、INSERT 还是 UPDATE。源码注释写明：此时先构造成 RETURNING 触发器，它会在**第一次调用 `sqlite3TriggersExist()` 时**被转换成合适的类型。

这个转换发生在 `src/trigger.c` 的 `triggersReallyExist` 函数体里（`sqlite3TriggersExist` 是它的包装）：

```
}else if( p->op==TK_RETURNING ){
  /* The first time a RETURNING trigger is seen, the "op" value tells
  ** us what time of trigger it should be. */
  assert( sqlite3IsToplevel(pParse) );
  p->op = op;
  if( IsVirtual(pTab) ){
    if( op!=TK_INSERT ){
      sqlite3ErrorMsg(pParse,
        "%s RETURNING is not available on virtual tables",
        op==TK_DELETE ? "DELETE" : "UPDATE");
    }
    p->tr_tm = TRIGGER_BEFORE;
  }else{
    p->tr_tm = TRIGGER_AFTER;
  }
  mask |= p->tr_tm;
}
```

要点：

- `p->op = op` 把 `TK_RETURNING` 改写成实际的 `TK_DELETE` / `TK_INSERT` / `TK_UPDATE`。
- 对普通表，`tr_tm` 设为 `TRIGGER_AFTER`——RETURNING 在行被修改**之后**求值。
- 对虚表（`IsVirtual(pTab)`），`tr_tm` 被改成 `TRIGGER_BEFORE`；并且当 `op` 不是 `TK_INSERT` 时报错 `"%s RETURNING is not available on virtual tables"`，`%s` 由 `op==TK_DELETE ? "DELETE" : "UPDATE"` 决定。也就是说 **虚表只支持 INSERT ... RETURNING**。
- `assert( sqlite3IsToplevel(pParse) )` —— `sqlite3IsToplevel` 在 `src/sqliteInt.h` 里定义为 `((p)->pToplevel==0)`，即 RETURNING 只能出现在顶层语句，不能嵌在触发器或子查询里。

`triggersReallyExist` 里还有一条专门为 UPSERT 准备的分支：

```
}else if( p->bReturning && p->op==TK_INSERT && op==TK_UPDATE
          && sqlite3IsToplevel(pParse) ){
  /* Also fire a RETURNING trigger for an UPSERT */
  mask |= p->tr_tm;
}
```

即当 DML 是 `INSERT ... ON CONFLICT DO UPDATE`（op 为 `TK_UPDATE`）而 RETURNING 触发器此前已被定型为 `TK_INSERT` 时，触发器照样要触发——因为 UPSERT 的冲突分支走的是 UPDATE 路径。

### 触发器列表的装配：sqlite3TriggerList

RETURNING 触发器要能被 DML 编译器找到，必须出现在目标表的触发器链上。`src/trigger.c` 的 `sqlite3TriggerList` 函数体遍历 TEMP schema 的 `trigHash`，对每个临时触发器分两种情况处理：

```
if( pTrig->pTabSchema==pTab->pSchema
 && pTrig->table
 && 0==sqlite3StrICmp(pTrig->table, pTab->zName)
 && (pTrig->pTabSchema!=pTmpSchema || pTrig->bReturning)
){
  pTrig->pNext = pList;
  pList = pTrig;
}else if( pTrig->op==TK_RETURNING ){
  ...
  assert( &(pParse->u1.d.pReturning->retTrig) == pTrig );
  pTrig->table = pTab->zName;
  pTrig->pTabSchema = pTab->pSchema;
  pTrig->pNext = pList;
  pList = pTrig;
}
```

第二个分支就是 RETURNING 专用路径：它先断言这个 `TK_RETURNING` 触发器正是 `pParse->u1.d.pReturning->retTrig`（唯一的 RETURNING 触发器），然后把 `table` 设为目标表名、`pTabSchema` 设为目标表的 schema，再挂进链表。注意此时 `pTrig->op` 已经被 `triggersReallyExist` 改写过了——`sqlite3TriggerList` 是在 `triggersReallyExist` 内部被调用的（`pList = sqlite3TriggerList(pParse, pTab);`），所以改写发生在前、装配发生在后。

第一个分支里的 `(pTrig->pTabSchema!=pTmpSchema || pTrig->bReturning)` 是让 TEMP 里的 RETURNING 触发器绕过「TEMP 触发器只能作用于 TEMP 表」这一常规限制的开关。

### 代码生成：codeReturningTrigger

`src/trigger.c` 的 `codeReturningTrigger` 是真正生成 VDBE 指令的地方。它的注释指出：与其他触发器把子程序放进字节码不同，RETURNING 的代码是**内联生成**的。

函数体的关键步骤：

**第 1 步：两道早退。**

```
if( !pParse->bReturning ){
  /* This RETURNING trigger must be for a different statement as
  ** this statement lacks a RETURNING clause. */
  return;
}
...
pReturning = pParse->u1.d.pReturning;
if( pTrigger != &(pReturning->retTrig) ){
  /* This RETURNING trigger is for a different statement */
  return;
}
```

**第 2 步：为列名生成做一次 SELECT 预演。**

```
memset(&sSelect, 0, sizeof(sSelect));
memset(&uSrc, 0, sizeof(uSrc));
pFrom = &uSrc.sSrc;
sSelect.pEList = sqlite3ExprListDup(db, pReturning->pReturnEL, 0);
sSelect.pSrc = pFrom;
pFrom->nSrc = 1;
pFrom->a[0].pSTab = pTab;
pFrom->a[0].zName = pTab->zName; /* tag-20240424-1 */
pFrom->a[0].iCursor = -1;
sqlite3SelectPrep(pParse, &sSelect, 0);
if( pParse->nErr==0 ){
  assert( db->mallocFailed==0 );
  sqlite3GenerateColumnNames(pParse, &sSelect);
}
sqlite3ExprListDelete(db, sSelect.pEList);
```

这段把 RETURNING 表达式列表当成一个单表 SELECT 来准备一次，目的是让 `sqlite3GenerateColumnNames` 生成结果列的列名（`RETURNING expr AS alias` 里的 alias 在这一步生效）。`pFrom->a[0].iCursor = -1` 表示游标号未定。`pFrom` 用了一个 union 加 `u8 fromSpace[SZ_SRCLIST_1]` 的栈上空间，避免额外的堆分配。

**第 3 步：展开通配符。**

```
pNew = sqlite3ExpandReturning(pParse, pReturning->pReturnEL, pTab);
```

`sqlite3ExpandReturning` 复制一份表达式列表，并把 `*` 展开成目标表的全部可见列。展开时逐列检查 `IsHiddenColumn(pTab->aCol+jj)`，隐藏列被跳过；列名写入 `pItem->zEName` 并置 `pItem->fg.eEName = ENAME_NAME`。对于 `TABLE.*` 形式，`isAsteriskTerm` 报错 `"RETURNING may not use \"TABLE.*\" wildcards"`——注意 `isAsteriskTerm` 在遇到 `TABLE.*` 时先 `sqlite3ErrorMsg` 再 `return 1`，也就是仍然按「是通配符」处理，但错误已经记下。

**第 4 步：分配游标与寄存器。**

```
if( pReturning->nRetCol==0 ){
  pReturning->nRetCol = pNew->nExpr;
  pReturning->iRetCur = pParse->nTab++;
}
```

返回列数就是展开后的表达式个数 `pNew->nExpr`；`iRetCur` 从 `pParse->nTab` 里分配一个新的游标号。

**第 5 步：解析名字上下文。**

```
NameContext sNC;
memset(&sNC, 0, sizeof(sNC));
if( pReturning->nRetCol==0 ){ ... }
sNC.pParse = pParse;
sNC.uNC.iBaseReg = regIn;
sNC.ncFlags = NC_UBaseReg;
pParse->eTriggerOp = pTrigger->op;
pParse->pTriggerTab = pTab;
if( sqlite3ResolveExprListNames(&sNC, pNew)==SQLITE_OK
 && ALWAYS(!db->mallocFailed)
){
```

`NC_UBaseReg` 这个标志让 `src/resolve.c` 的名字解析知道「基址寄存器」由 `sNC.uNC.iBaseReg` 给出。`pParse->eTriggerOp` 与 `pParse->pTriggerTab` 是给解析器用的上下文：前者记下 DML 类型（`TK_DELETE`/`TK_INSERT`/`TK_UPDATE`），后者记下目标表。

**第 6 步：处理相关子查询（RETURNING 的自引用语义）。**

```
sqlite3ProcessReturningSubqueries(pNew, pTab);
```

这是本页 `refs` 列出的第二个符号，其细节见下一节。

**第 7 步：逐列求值并生成记录。**

```
int nCol = pNew->nExpr;
int reg = pParse->nMem+1;
sqlite3ProcessReturningSubqueries(pNew, pTab);
pParse->nMem += nCol+2;
pReturning->iRetReg = reg;
for(i=0; i<nCol; i++){
  Expr *pCol = pNew->a[i].pExpr;
  assert( pCol!=0 );
  sqlite3ExprCodeFactorable(pParse, pCol, reg+i);
  if( sqlite3ExprAffinity(pCol)==SQLITE_AFF_REAL ){
    sqlite3VdbeAddOp1(v, OP_RealAffinity, reg+i);
  }
}
sqlite3VdbeAddOp3(v, OP_MakeRecord, reg, i, reg+i);
sqlite3VdbeAddOp2(v, OP_NewRowid, pReturning->iRetCur, reg+i+1);
sqlite3VdbeAddOp3(v, OP_Insert, pReturning->iRetCur, reg+i, reg+i+1);
```

寄存器布局：起始寄存器 `reg = pParse->nMem+1`，前 `nCol` 个寄存器放各列的值，第 `nCol` 个（`reg+i`）放 `OP_MakeRecord` 产出的记录，第 `nCol+1` 个（`reg+i+1`）放 `OP_NewRowid` 生成的行号。`pParse->nMem += nCol+2` 一次性把这 `nCol+2` 个寄存器占住——`+2` 就是记录寄存器与 rowid 寄存器。

亲和性处理：`sqlite3ExprAffinity(pCol)==SQLITE_AFF_REAL` 时补一条 `OP_RealAffinity`，把整数值转成浮点表示。

**第 8 步：清理。**

```
sqlite3ExprListDelete(db, pNew);
pParse->eTriggerOp = 0;
pParse->pTriggerTab = 0;
```

展开出来的表达式列表是临时副本，用完即删；`eTriggerOp` 与 `pTriggerTab` 复位成 0，避免影响后续解析。

### 触发链：从 DML 编译器到 codeReturningTrigger

`codeReturningTrigger` 由 `src/trigger.c` 的 `sqlite3CodeRowTrigger` 调用：

```
if( (p->op==op || (p->bReturning && p->op==TK_INSERT && op==TK_UPDATE))
 && p->tr_tm==tr_tm
 && checkColumnOverlap(p->pColumns, pChanges)
){
  if( !p->bReturning ){
    sqlite3CodeRowTriggerDirect(pParse, p, pTab, reg, orconf, ignoreJump);
  }else if( sqlite3IsToplevel(pParse) ){
    codeReturningTrigger(pParse, p, pTab, reg);
  }
}
```

RETURNING 触发器与普通触发器在这里分道扬镳：普通触发器走 `sqlite3CodeRowTriggerDirect`（生成子程序），RETURNING 走 `codeReturningTrigger`（内联生成）。`sqlite3IsToplevel(pParse)` 再判一次顶层，保证嵌在触发体里的语句不生成 RETURNING 结果。

`sqlite3CodeRowTrigger` 的调用方有三个，全在各自的 DML 编译文件里：

| 调用方 | 位置 | 传入的 op 与时机 |
| --- | --- | --- |
| `sqlite3Insert`（`src/insert.c`） | `sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0, TRIGGER_BEFORE, ...)` 与 `sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0, TRIGGER_AFTER, ...)` | `TK_INSERT`，BEFORE 与 AFTER 各一次 |
| `sqlite3Update`（`src/update.c`） | `sqlite3CodeRowTrigger(pParse, pTrigger, TK_UPDATE, pChanges, ...)` 两处 | `TK_UPDATE`，带 `pChanges` 变更列列表 |
| `sqlite3DeleteFrom`（`src/delete.c`） | `sqlite3CodeRowTrigger(pParse, pTrigger, ...)` 两处 | `TK_DELETE` |

由于 RETURNING 触发器在 `triggersReallyExist` 里被定型为 `TRIGGER_AFTER`，实际命中的是 AFTER 那一次调用。

### 相关子查询：sqlite3ProcessReturningSubqueries 与 sqlite3ReturningSubqueryCorrelated

RETURNING 里可以写子查询，例如 `UPDATE t SET x=1 RETURNING (SELECT count(*) FROM u)`。但如果这个子查询**引用了正在被修改的那张表**，就会出现「子查询读到的是改前还是改后的值」的歧义。SQLite 的处理方式是：把这类子查询标记为相关（`SF_Correlated`），让它按逐行重算的语义执行。

`src/trigger.c` 的 `sqlite3ProcessReturningSubqueries` 函数体：

```
Walker w;
memset(&w, 0, sizeof(w));
w.xExprCallback = sqlite3ExprWalkNoop;
w.xSelectCallback = sqlite3ReturningSubqueryCorrelated;
w.u.pTab = pTab;
sqlite3WalkExprList(&w, pEList);
if( w.eCode ){
  w.xExprCallback = sqlite3ReturningSubqueryVarSelect;
  w.xSelectCallback = sqlite3SelectWalkNoop;
  sqlite3WalkExprList(&w, pEList);
}
```

它**走两遍**：

- **第 1 遍**：`xExprCallback` 是空操作 `sqlite3ExprWalkNoop`，`xSelectCallback` 换成 `sqlite3ReturningSubqueryCorrelated`。目的是找出所有引用了目标表的 SELECT，并把它们标记为 `SF_Correlated`。
- **第 2 遍**：只有当第 1 遍真的发现了相关子查询（`w.eCode` 非零）时才走。此时 `xExprCallback` 换成 `sqlite3ReturningSubqueryVarSelect`，`xSelectCallback` 换成 `sqlite3SelectWalkNoop`——不再标记 SELECT，而是把**含相关子查询的那个表达式**标记为 `EP_VarSelect`。

`w.eCode` 由 `sqlite3ReturningSubqueryCorrelated` 设置，该函数体是：

```
static int sqlite3ReturningSubqueryCorrelated(Walker *pWalker, Select *pSelect){
  int i;
  SrcList *pSrc;
  assert( pSelect!=0 );
  pSrc = pSelect->pSrc;
  assert( pSrc!=0 );
  for(i=0; i<pSrc->nSrc; i++){
    if( pSrc->a[i].pSTab==pWalker->u.pTab ){
      testcase( pSelect->selFlags & SF_Correlated );
      pSelect->selFlags |= SF_Correlated;
      pWalker->eCode = 1;
      break;
    }
  }
  return WRC_Continue;
}
```

判据是**指针相等**：子查询的 `FROM` 列表里某一项的 `pSTab` 与正在被修改的表 `pWalker->u.pTab` 是同一个 `Table*` 指针。命中后给该 SELECT 打上 `SF_Correlated` 标志，并把 `pWalker->eCode` 置 1，然后 `break`（同一子查询只需标记一次）。`w.u.pTab` 这个字段正是 `Walker` 结构 union 里的 `struct Table *pTab`。

第 2 遍用的 `sqlite3ReturningSubqueryVarSelect`：

```
if( ExprUseXSelect(pExpr)
 && (pExpr->x.pSelect->selFlags & SF_Correlated)!=0
){
  testcase( ExprHasProperty(pExpr, EP_VarSelect) );
  ExprSetProperty(pExpr, EP_VarSelect);
}
```

只对「自身就是一个相关 SELECT」的表达式打 `EP_VarSelect` 标志——这正是 `EXISTS (SELECT ...)`、`IN (SELECT ...)` 这类把子查询包在算子里的表达式形态。

源码注释里给出的出处是 `https://sqlite.org/forum/forumpost/2c83569ce8945d39`。

### 结果的输出：epemeral 表 + 收尾扫描

RETURNING 的结果行先写进一张 ephemeral 表，再在语句尾部读出来。

**写入端**在 `codeReturningTrigger`（上一节第 7 步）：`OP_MakeRecord` 造记录、`OP_NewRowid` 造行号、`OP_Insert` 插进 `pReturning->iRetCur`。

**建表**在 `src/build.c` 的代码生成收尾阶段：

```
if( pParse->bReturning ){
  Returning *pRet;
  assert( !pParse->isCreate );
  pRet = pParse->u1.d.pReturning;
  if( pRet->nRetCol ){
    sqlite3VdbeAddOp2(v, OP_OpenEphemeral, pRet->iRetCur, pRet->nRetCol);
  }
}
```

`OP_OpenEphemeral` 的 P2 是列数，即 `pRet->nRetCol`。

**读出端**在同一函数的收尾阶段（更靠前的一段）：

```
if( pParse->bReturning ){
  Returning *pReturning;
  int addrRewind;
  int reg;
  assert( !pParse->isCreate );
  pReturning = pParse->u1.d.pReturning;
  if( pReturning->nRetCol ){
    sqlite3VdbeAddOp0(v, OP_FkCheck);
    addrRewind =
       sqlite3VdbeAddOp1(v, OP_Rewind, pReturning->iRetCur);
    VdbeCoverage(v);
    reg = pReturning->iRetReg;
    for(i=0; i<pReturning->nRetCol; i++){
      sqlite3VdbeAddOp3(v, OP_Column, pReturning->iRetCur, i, reg+i);
    }
    sqlite3VdbeAddOp2(v, OP_ResultRow, reg, i);
    sqlite3VdbeAddOp2(v, OP_Next, pReturning->iRetCur, addrRewind+1);
    VdbeCoverage(v);
    sqlite3VdbeJumpHere(v, addrRewind);
  }
}
```

顺序是：`OP_FkCheck`（外键检查）、`OP_Rewind` 重置游标、取 `addrRewind` 作为循环标签、逐列 `OP_Column` 读出到 `reg+i`、一条 `OP_ResultRow` 输出、`OP_Next` 跳回 `addrRewind+1` 继续，直到 `OP_Next` 走完自然落到 `addrRewind` 处的 `OP_JumpHere`。

**架构推论：** 这个「先全量落盘再统一输出」的形状意味着 RETURNING 的结果集**不能边改边吐**，必须等 DML 全部执行完才输出。这与官方文档《RETURNING》里 “Processing Order” 一节描述的语义一致，也解释了为什么 `src/delete.c` 与 `src/update.c` 在有 RETURNING 时都要关闭行数计数器：`src/delete.c` 里 `if( (db->flags & SQLITE_CountRows)!=0 && !pParse->nested && !pParse->pTriggerTab && !pParse->bReturning )`，`src/update.c` 里同样带 `!pParse->bReturning` 条件——两者在 `pParse->bReturning` 为真时都不分配 `memCnt` / `regRowCount` 寄存器，即不统计改动行数。

### 名字解析：RETURNING 里怎么引用被修改的列

RETURNING 表达式里写列名，解析到哪张表？`src/resolve.c` 的 `lookupName` 一节（`pParse->pTriggerTab!=0` 分支）：

```
if( pParse->bReturning ){
  if( (pNC->ncFlags & NC_UBaseReg)!=0
   && ALWAYS(zTab==0
             || sqlite3StrICmp(zTab,pParse->pTriggerTab->zName)==0
             || isValidSchemaTableName(zTab, pParse->pTriggerTab, 0))
  ){
    pExpr->iTable = op!=TK_DELETE;
    pTab = pParse->pTriggerTab;
  }
}
```

`pExpr->iTable = op!=TK_DELETE` —— DELETE 语句里返回的是**旧值**（`iTable` 为 0），INSERT / UPDATE 里返回的是**新值**（`iTable` 为 1）。之后落到通用列解析分支：

```
if( pParse->bReturning ){
  eNewExprOp = TK_REGISTER;
  pExpr->op2 = TK_COLUMN;
  pExpr->iColumn = iCol;
  pExpr->iTable = pNC->uNC.iBaseReg + (pTab->nCol+1)*pExpr->iTable +
     sqlite3TableColumnToStorage(pTab, iCol) + 1;
}
```

即 RETURNING 里的列引用被编译成 `TK_REGISTER`（读寄存器）而不是普通触发器的 `TK_TRIGGER`（读 new/old 伪表）。寄存器地址由 `iBaseReg` 加上偏移算出，偏移量 `(pTab->nCol+1)*iTable + sqlite3TableColumnToStorage(pTab, iCol) + 1` 与 `src/trigger.c` 的 `sqlite3CodeRowTrigger` 注释里描述的寄存器布局对应：OLD 值占 `reg+1` 到 `reg+N`，`NEW.rowid` 占 `reg+N+1`，NEW 值占 `reg+N+2` 起。

## 关键取舍

**用 TEMP 触发器复用整条触发器管线，而不是新写一套 DML 结果集管线。** 这是 RETURNING 实现的核心决策。好处是 RETURNING 自动获得触发器已有的全部语义：时机（`TRIGGER_AFTER`）、列覆盖判定（`checkColumnOverlap`）、UPSERT 兼容（`p->bReturning && p->op==TK_INSERT && op==TK_UPDATE` 分支）、外键检查（`OP_FkCheck`）。代价是引入了一层间接：`sqlite3AddReturning` 必须造一个假触发器，再由 `triggersReallyExist` 在第一次 `sqlite3TriggersExist()` 时把它「变形」成真正的 DML 类型。

**先落 ephemeral 表，再统一输出。** 换来的是 DML 执行与结果输出的顺序完全确定（官方文档《RETURNING》的 “Processing Order” 一节）。代价是内存占用随被修改行数增长——每行都要在 ephemeral 表里留一条记录。`src/insert.c` 里 `SQLITE_ALLOW_ROWID_IN_VIEW` 打开时还有一条特殊分支：`else if( pParse->bReturning )` 时往 rowid 寄存器写常量 `-1`，注释说明这是为了应对返回表达式引用视图的 `"rowid"` 的情况。

**通配符展开成显式列，并禁止 `TABLE.*`。** `sqlite3ExpandReturning` 把 `*` 展开成目标表的全部**可见**列（`IsHiddenColumn` 过滤掉的跳过），并保留每个展开列的 `zEName`。`TABLE.*` 被 `isAsteriskTerm` 拒绝，错误文本是 `"RETURNING may not use \"TABLE.*\" wildcards"`。代价是 `*` 的展开在编译期就要遍历表结构，且列数写死在 `nRetCol` 里。

**自引用子查询交给相关子查询语义，不给「改前/改后」的明确答案。** `sqlite3ProcessReturningSubqueries` 不判断「子查询应该看到改前还是改后的值」，只是把引用了目标表的 SELECT 标记为 `SF_Correlated`、把外层表达式标记为 `EP_VarSelect`，让 VDBE 按逐行重算处理。官方文档把这类行为明确写成 “Self-Referential Subqueries Are Indeterminate”。源码注释引用的论坛帖 `forumpost/2c83569ce8945d39` 就是这条语义的来源依据。

**两遍遍历，且第二遍是条件触发的。** 第一遍只标记 SELECT，第二遍只标记表达式，`sqlite3ProcessReturningSubqueries` 用 `w.eCode` 把两遍串起来——没有相关子查询时第二遍整个跳过。代价是遍历次数翻倍（最坏情况），收益是没有相关子查询时不付 `EP_VarSelect` 标记的成本。

**顶层限定是硬约束。** 三处独立的检查共同保证 RETURNING 不嵌在触发器或子查询里：`sqlite3AddReturning` 里 `pParse->pNewTrigger` 非空时报 `"cannot use RETURNING in a trigger"`；`triggersReallyExist` 与 `sqlite3CodeRowTrigger` 里用 `sqlite3IsToplevel(pParse)` 判定（`sqlite3IsToplevel` 即 `p->pToplevel==0`）。这与「RETURNING 触发器只能是顶层语句的触发器」这一事实一致——嵌套 DML 没有独立的返回通道。

**虚表降级为只支持 INSERT。** `triggersReallyExist` 里 `IsVirtual(pTab)` 分支把 `tr_tm` 改成 `TRIGGER_BEFORE`（虚表的 BEFORE 触发器能拿到将要写入的值），且当 `op != TK_INSERT` 时报 `"%s RETURNING is not available on virtual tables"`。这是**回退到 `sqlite3TriggersExist` 的虚表分支**——具体由 `triggersReallyExist` 函数体里的 `IsVirtual(pTab)` 判断实现，报错后 DML 语句整体失败。

**不用 `sqlite3_count_changes` 语义与 RETURNING 共存。** `src/delete.c` 与 `src/update.c` 都在 `pParse->bReturning` 为真时跳过行数计数器。这不是「回退」，而是**主动关闭**：`src/delete.c` 的条件里 `!pParse->bReturning` 让 `memCnt` 保持 0，`src/update.c` 的条件里 `!pParse->bReturning` 让 `regRowCount` 保持 0。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/build.c | sqlite3AddReturning | RETURNING 的注册入口：拒绝在触发器中使用（报 `"cannot use RETURNING in a trigger"`）、置 `pParse->bReturning`、分配 `Returning`、登记 `sqlite3DeleteReturning` 清理钩子、造出名为 `sqlite_returning_%p` 的 TEMP 触发器（`op=TK_RETURNING`、`tr_tm=TRIGGER_AFTER`、`bReturning=1`），并插入 `db->aDb[1].pSchema->trigHash`。 |
| src/build.c | sqlite3DeleteReturning | 解析期清理：把触发器从 TEMP schema 的 `trigHash` 删除、释放 `pReturnEL` 表达式列表、释放 `Returning` 结构。 |
| src/build.c | sqlite3FinishCoding 中的 RETURNING 收尾 | 语句尾部生成 `OP_OpenEphemeral`（建临时结果表，列数为 `pRet->nRetCol`）以及 `OP_FkCheck` / `OP_Rewind` / `OP_Column` / `OP_ResultRow` / `OP_Next` 循环（读出并输出全部返回行）。 |
| src/trigger.c | sqlite3ProcessReturningSubqueries | 两遍扫描 RETURNING 表达式列表：第 1 遍用 `sqlite3ReturningSubqueryCorrelated` 找引用了目标表的 SELECT 并标 `SF_Correlated`；`w.eCode` 非零时第 2 遍用 `sqlite3ReturningSubqueryVarSelect` 给外层表达式标 `EP_VarSelect`。 |
| src/trigger.c | sqlite3ReturningSubqueryCorrelated | `xSelectCallback`：遍历子查询 `pSrc->a[i].pSTab`，与 `pWalker->u.pTab` 指针相等即给该 SELECT 打 `SF_Correlated` 标志并把 `pWalker->eCode` 置 1。 |
| src/trigger.c | sqlite3ReturningSubqueryVarSelect | 第 2 遍的 `xExprCallback`：对 `ExprUseXSelect(pExpr)` 且其 SELECT 带 `SF_Correlated` 的表达式打 `EP_VarSelect` 标志。 |
| src/trigger.c | codeReturningTrigger | RETURNING 的代码生成主体：预演 SELECT 生成列名、`sqlite3ExpandReturning` 展开通配符、分配 ephemeral 游标 `iRetCur`、按 `NC_UBaseReg` 解析列引用、逐列求值并补 `OP_RealAffinity`、`OP_MakeRecord` + `OP_NewRowid` + `OP_Insert` 写入临时表。 |
| src/trigger.c | sqlite3ExpandReturning | 复制 RETURNING 表达式列表并展开 `*`：逐列跳过 `IsHiddenColumn` 的隐藏列，展开列写入 `zEName` 并置 `ENAME_NAME`。 |
| src/trigger.c | isAsteriskTerm | 判定通配符项：裸 `*` 返回 1；`TABLE.*` 报 `"RETURNING may not use \"TABLE.*\" wildcards"`。 |
| src/trigger.c | triggersReallyExist | 第一次遇到 RETURNING 触发器时把 `p->op` 从 `TK_RETURNING` 改写为实际的 DML 类型；虚表改 `TRIGGER_BEFORE` 且非 INSERT 时报 `"%s RETURNING is not available on virtual tables"`；另有 UPSERT 分支让 INSERT 型 RETURNING 触发器在 UPDATE 路径也触发。 |
| src/trigger.c | sqlite3TriggerList | 把 TEMP schema 的 RETURNING 触发器挂进目标表的触发器链：断言它是 `pParse->u1.d.pReturning->retTrig`，设置 `table` 与 `pTabSchema`。 |
| src/trigger.c | sqlite3CodeRowTrigger | 区分两类触发器：普通触发器走 `sqlite3CodeRowTriggerDirect`，RETURNING 触发器在 `sqlite3IsToplevel(pParse)` 为真时走 `codeReturningTrigger`。 |
| src/trigger.c | sqlite3TriggerColmask | 遇到 `p->bReturning` 的触发器直接返回 `0xffffffff`，表示全部列都可能被 RETURNING 表达式访问。 |
| src/sqliteInt.h | struct Returning | RETURNING 的状态结构：`pParse`、`pReturnEL`、`retTrig`（瞬态触发器）、`retTStep`（触发步）、`iRetCur`（结果表游标）、`nRetCol`（展开后列数）、`iRetReg`（结果行寄存器首址）、`char zName[40]`（触发器名缓冲区）。 |
| src/sqliteInt.h | struct TriggerStep | 触发步结构；`op` 字段取值含 `TK_RETURNING`，此时 `pExprList` 存放 RETURNING 关键字之后的表达式列表。 |
| src/sqliteInt.h | TRIGGER_BEFORE / TRIGGER_AFTER | 触发时机掩码，值分别为 1 与 2；RETURNING 触发器初始用 `TRIGGER_AFTER`。 |
| src/sqliteInt.h | sqlite3IsToplevel | 宏 `((p)->pToplevel==0)`（`SQLITE_OMIT_TRIGGER` 下退化为常量 1）；RETURNING 的三层嵌套检查都依赖它。 |
| src/sqliteInt.h | sqlite3AddReturning | 内部声明 `void sqlite3AddReturning(Parse*,ExprList*)`；由 `src/parse.y` 的四条产生式调用。 |
| src/parse.y | where_opt_ret / upsert / returning | RETURNING 的四条语法产生式，全部调用 `sqlite3AddReturning(pParse, X)`；`where_opt_ret` 与 `returning` 各带一个空产生式。 |
| src/resolve.c | lookupName 的 bReturning 分支 | 把 RETURNING 里的列引用解析为 `TK_REGISTER`：`pExpr->iTable = op!=TK_DELETE` 区分旧值（DELETE）与新值（INSERT/UPDATE），寄存器地址由 `iBaseReg` 加偏移算出。 |
| src/insert.c | sqlite3Insert 的 TRIGGER_AFTER 调用 | `sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0, TRIGGER_AFTER, ...)`——INSERT 路径上 RETURNING 触发器的接入点。 |
| src/insert.c | sqlite3Insert 的 bReturning 分支 | `SQLITE_ALLOW_ROWID_IN_VIEW` 打开时，若有 RETURNING 则往 rowid 寄存器写常量 `-1`，应对返回表达式引用视图 `"rowid"` 的情形。 |
| src/update.c | sqlite3Update 的 TRIGGER_AFTER 调用 | `sqlite3CodeRowTrigger(pParse, pTrigger, TK_UPDATE, pChanges, ...)`——UPDATE 路径上 RETURNING 触发器的接入点。 |
| src/update.c | sqlite3Update 的计数条件 | 行数计数器的启用条件含 `!pParse->bReturning`，RETURNING 存在时不统计改动行数。 |
| src/delete.c | sqlite3DeleteFrom 的 TRIGGER_AFTER 调用 | `sqlite3CodeRowTrigger(pParse, pTrigger, ...)`——DELETE 路径上 RETURNING 触发器的接入点。 |
| src/delete.c | sqlite3DeleteFrom 的计数条件 | 行数计数器的启用条件含 `!pParse->bReturning`，RETURNING 存在时不统计改动行数。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里，DML 编译与结果产出是两条独立通路，没有「DML 直接带结果集」这一形态。若主核要支持同类能力，SQLite 这套实现里有三处值得抄：一是**把新语义挂到已有扩展点上**——`sqlite3AddReturning` 造的是一个普通 TEMP 触发器，插进 `trigHash`，后续被 `sqlite3TriggerList` 摘出来、走 `sqlite3CodeRowTrigger` 的既有分支，RETURNING 本身几乎没有新增管线；主核若照抄，应把「DML 带返回行」实现成既有执行节点的一种扩展，而不是新开一条结果通道。二是**结果集的物化与输出的分离**——写入端（`codeReturningTrigger` 里的 `OP_Insert` 到 ephemeral 表）与读出端（`src/build.c` 收尾里的 `OP_Rewind`/`OP_ResultRow` 循环）分处两段代码，中间靠 `Returning.iRetCur` 与 `Returning.iRetReg` 两个字段衔接；这让「每行算一次」与「输出一次」的顺序问题变成两个可独立验证的点。三是**相关子查询用标记而非特判**——`sqlite3ProcessReturningSubqueries` 不去判断「改前还是改后」，只打 `SF_Correlated` / `EP_VarSelect` 两个标志，把语义推给已有的相关子查询执行路径；主核若要在 DML 返回行里支持子查询，这条「不特判、只标记」的路线比在执行器里加一堆分支更省事。

**架构推论：** 本页是 AI 检索 SQLite RETURNING 机制时的落点。RETURNING 的语义主体集中在两处：`src/build.c` 的 `sqlite3AddReturning`（注册：造 TEMP 触发器、清理钩子、哈希插入）与 `src/trigger.c` 的 `codeReturningTrigger`（代码生成：展开通配符、解析列引用、算列值、插 ephemeral 表）。凡是「RETURNING 的结果列是怎么来的」「为什么 RETURNING 不能放在触发器里」「为什么虚表只支持 INSERT ... RETURNING」「`RETURNING *` 会不会带出隐藏列」「为什么有 RETURNING 时 `sqlite3_count_changes` 拿不到行数」这类问题，检索落点都应指向这两个文件；`src/trigger.c` 的 `sqlite3ProcessReturningSubqueries` / `sqlite3ReturningSubqueryCorrelated` 负责的是「RETURNING 里引用被修改表」的子查询语义，`src/resolve.c` 的 bReturning 分支负责的是「RETURNING 里写列名解析成什么」——这两处是边界上的邻居而不是主体。

## 参考

- SQLite 官方文档《RETURNING》：<https://sqlite.org/lang_returning.html>
- SQLite 官方语法图《returning-clause》：<https://sqlite.org/syntax/returning-clause.html>
- SQLite 官方 Fossil 源码 `src/build.c`（trunk，`sqlite3AddReturning` 与 RETURNING 的收尾代码生成）：<https://sqlite.org/src/file?name=src/build.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/trigger.c`（trunk，`codeReturningTrigger` 与相关子查询标记）：<https://sqlite.org/src/file?name=src/trigger.c&ci=trunk>
- GitHub 源码镜像 `src/build.c`：<https://github.com/sqlite/sqlite/blob/master/src/build.c>
- GitHub 源码镜像 `src/trigger.c`：<https://github.com/sqlite/sqlite/blob/master/src/trigger.c>
