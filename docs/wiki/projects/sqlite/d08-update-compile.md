---
project: sqlite
feature: update-compile
page_type: feature
schema_version: 1
refs:
  - path: src/update.c
    symbols: [sqlite3Update, sqlite3ColumnDefault]
---

# UPDATE 编译

## 一句话职责

本页负责把一条 `UPDATE` 语句编译成 VDBE 程序：识别哪些列被改、rowid 或主键是否被改、哪些索引需要跟着改、如何定位待更新的行（走单遍 one-pass、临时表两遍，还是带 `FROM` 的物化路径），并把新行内容装进寄存器数组交给后续的约束检查与写入。本页不负责 B-Tree 的实际写入，也不负责触发器的语义。

## 机制是什么

边界（依据条目 `boundary`）：负责 UPDATE 语句的编译：定位待更新行、生成新行记录、处理列默认值与生成列；不负责 B-tree 的写入（F 组）、不负责触发器（K 组）。

UPDATE 编译的入口是 `src/update.c` 的 `sqlite3Update`，由 `src/parse.y` 的 `cmd ::= with UPDATE ...` 规则调用——调用点有两处，带 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT` 的分支传 `pOrderBy`/`pLimit`，另一分支传 0、0。

整条编译链由一张 **aXRef 数组** 贯穿。`aXRef` 的长度是 `pTab->nCol+nIdx+1`，按 `sqlite3DbMallocRawNN` 分配，其中前 `pTab->nCol` 个槽位是「表列 → `pChanges` 下标」的映射，`aXRef[i]==-1` 表示第 i 列未被修改；`aRegIdx` 紧接其后，每个索引一个槽位，值为该索引键所占用的寄存器号（非 0 表示该索引需要更新）；`aToOpen` 紧接其后，是每索引一个字节的「是否需要打开游标」标志。

编译过程可拆成六段：

| 阶段 | 关键动作 | 主要符号 |
|---|---|---|
| 解析与授权 | 逐个 SET 项解析名字、判定 rowid/PK 是否变、生成列直接报错、逐列走授权回调 | `sqlite3ResolveExprNames`、`sqlite3ColumnIndex`、`sqlite3AuthCheck` |
| 派生列传播 | 生成列的生成表达式若引用了被改列，则该生成列也标为「会变」 | `sqlite3ExprReferencesUpdatedColumn` |
| 索引需求判定 | 逐索引判断是否需要重写键，必要时分配寄存器 | `indexColumnIsBeingUpdated`、`indexWhereClauseMightChange` |
| 定位待更新行 | one-pass / 两遍临时表 / 带 `FROM` 物化三条路径 | `sqlite3WhereBegin`、`sqlite3WhereOkOnePass`、`updateFromSelect` |
| 装配新行 | 把新值装进 `regNew` 起的寄存器数组，触发器前后重算生成列 | `sqlite3ComputeGeneratedColumns`、`sqlite3TableAffinity` |
| 落地序列 | 约束检查 → 删旧索引项 → 删旧行 → 插新行 → 外键动作 → 行计数 | `sqlite3GenerateConstraintChecks`、`sqlite3GenerateRowIndexDelete`、`sqlite3CompleteInsertion` |

## 怎么实现的

### 第一步：sqlite3Update 的入口检查与表定位

`sqlite3Update` 的前几百行是「拒绝与定位」阶段。

- `if( pParse->nErr ) goto update_cleanup;` —— 上游已经报错则直接返回。
- `pTab = sqlite3SrcListLookup(pParse, pTabList); if( pTab==0 ) goto update_cleanup;` —— 定位目标表；`iDb = sqlite3SchemaToIndex(pParse->db, pTab->pSchema)` 拿到它所在的数据库序号。
- 触发器与视图判定：`pTrigger = sqlite3TriggersExist(pParse, pTab, TK_UPDATE, pChanges, &tmask);`，`isView = IsView(pTab);`。`tmask` 是 `TRIGGER_BEFORE|TRIGGER_AFTER` 掩码。
- `sqlite3ViewGetColumnNames(pParse, pTab)` 失败即返回；`sqlite3IsReadOnly(pParse, pTab, pTrigger)` 返回非 0 也返回。`sqlite3IsReadOnly`（定义在 `src/delete.c`）在两种情况报错：`tabIsReadOnly` 为真时报 `table %s may not be modified`；`IsView(pTab)` 且没有 INSTEAD OF 触发器时报 `cannot modify %s because it is a view`。

`nChangeFrom` 的取值只有两种：`nChangeFrom = (pTabList->nSrc>1) ? pChanges->nExpr : 0;`。源码断言 `nChangeFrom==0 || pUpsert==0`——带 `FROM` 的 UPDATE 不能是 UPSERT 的一部分。

### 第二步：SET 项的逐项解析——rowid、主键与生成列

遍历 `pChanges->nExpr` 的循环是整个编译期最关键的判定点。

**名字解析**：`if( nChangeFrom==0 && sqlite3ResolveExprNames(&sNC, pChanges->a[i].pExpr) )` —— 带 `FROM` 时不在此处解析，源码注释写明「对带 FROM 子句的 UPDATE，不在此处解析表达式，由下面的 `sqlite3Select()` 调用完成」。

**列定位**：`j = sqlite3ColumnIndex(pTab, pChanges->a[i].zEName);`，然后分三条路：

1. `j==pTab->iPKey`（INTEGER PRIMARY KEY 列）→ `chngRowid = 1; pRowidExpr = pChanges->a[i].pExpr; iRowidExpr = i;`。
2. `pPk` 非空（WITHOUT ROWID 表）且 `pTab->aCol[j].colFlags & COLFLAG_PRIMKEY` → `chngPk = 1;`。`COLFLAG_PRIMKEY` 的值是 `0x0001`，定义在 `src/sqliteInt.h`。
3. `pTab->aCol[j].colFlags & COLFLAG_GENERATED` → 直接报错 `cannot UPDATE generated column "%s"` 并返回。`COLFLAG_GENERATED` 是 `0x0060`，即 `COLFLAG_VIRTUAL`（`0x0020`）与 `COLFLAG_STORED`（`0x0040`）的组合掩码。
4. `j<0` 且不是 rowid 别名 → 报错 `no such column: %s`，并置 `pParse->checkSchema = 1`。若 `pPk==0 && sqlite3IsRowid(pChanges->a[i].zEName)` 则视为 rowid 别名，走 `chngRowid=1` 分支。

命中真实列后 `aXRef[j] = i;` 记录映射。

**授权**：`sqlite3AuthCheck(pParse, SQLITE_UPDATE, pTab->zName, j<0 ? "ROWID" : pTab->aCol[j].zCnName, db->aDb[iDb].zDbSname)`。返回 `SQLITE_DENY` 时放弃整条语句；返回其他非 `SQLITE_OK` 的值时只放弃这一列——`j<0 || j==pTab->iPKey` 时清 `chngRowid`，否则清 `aXRef[j]`。

### 第三步：生成列的依赖传播

`if( pTab->tabFlags & TF_HasGenerated )` 开启一个 do-while 循环。`TF_HasGenerated` 的值是 `0x00000060`，等于 `TF_HasVirtual`（`0x00000020`）与 `TF_HasStored`（`0x00000040`）的组合。

循环体逐列检查：对 `aXRef[i]<0` 且带 `COLFLAG_GENERATED` 的列，调 `sqlite3ExprReferencesUpdatedColumn(sqlite3ColumnExpr(pTab, &pTab->aCol[i]), aXRef, chngRowid)`；若返回真，把 `aXRef[i] = 99999`。源码注释解释了这个魔数的用意：「生成表达式的实际 aXRef 值不被使用，除了检查它是否非负……我们用 99999，这样在符号调试器里看 aXRef[] 时这个值显而易见」。

`sqlite3ExprReferencesUpdatedColumn`（定义在 `src/insert.c`）是一个 Walker：回调 `checkConstraintExprNode` 遇到 `TK_COLUMN` 节点时，若 `pWalker->u.aiCol[pExpr->iColumn]>=0` 就置位 `CKCNSTRNT_COLUMN`（`0x01`），若 `pExpr->iColumn<0` 则置位 `CKCNSTRNT_ROWID`（`0x02`）；函数返回前若 `chngRowid` 为假就清掉 `CKCNSTRNT_ROWID` 位。

循环用 `bProgress` 标志做不动点迭代：因为一个生成列可能引用另一个生成列，需要反复扫描直到没有新的列被标脏。

### 第四步：索引是否需要重写——两个判定函数

`hasFK = sqlite3FkRequired(pParse, pTab, aXRef, chngKey);` 之后是索引需求判定。遍历 `pTab->pIndex` 链，对每个索引算一个寄存器号：

```
if( chngKey || hasFK>1 || pIdx==pPk
 || indexWhereClauseMightChange(pIdx,aXRef,chngRowid)
){
  reg = ++pParse->nMem;
  pParse->nMem += pIdx->nColumn;
}else{
  reg = 0;
  for(i=0; i<pIdx->nKeyCol; i++){
    if( indexColumnIsBeingUpdated(pIdx, i, aXRef, chngRowid) ){
      reg = ++pParse->nMem;
      pParse->nMem += pIdx->nColumn;
      if( onError==OE_Default && pIdx->onError==OE_Replace ){
        bReplace = 1;
      }
      break;
    }
  }
}
if( reg==0 ) aToOpen[nAllIdx+1] = 0;
aRegIdx[nAllIdx] = reg;
```

`indexColumnIsBeingUpdated`（`src/update.c` 静态函数）取 `pIdx->aiColumn[iCol]`：非负时直接查 `aXRef[iIdxCol]>=0`；为 `XN_EXPR` 时走 `sqlite3ExprReferencesUpdatedColumn(pIdx->aColExpr->a[iCol].pExpr, aXRef, chngRowid)`。函数头注释写明它的不对称性：假阳性只是性能退化，假阴性会导致索引损坏与错误答案——所以判定必须偏保守。

`indexWhereClauseMightChange` 判定部分索引（partial index）的条件表达式是否会变：`if( pIdx->pPartIdxWhere==0 ) return 0;`，否则调 `sqlite3ExprReferencesUpdatedColumn(pIdx->pPartIdxWhere, aXRef, chngRowid)`。

`aRegIdx[nAllIdx] = ++pParse->nMem;` 单独给表记录本身留一个寄存器。`bReplace` 为真时 `memset(aToOpen, 1, nIdx+1);` —— 源码注释解释：若 REPLACE 冲突解决可能被触发，必须打开所有索引的游标，以便需要时删除冲突记录。

### 第五步：列默认值——sqlite3ColumnDefault

`sqlite3ColumnDefault(Vdbe *v, Table *pTab, int i, int iReg)` 是条目 refs 里的第二个符号。它做两件事：

1. `pCol = &pTab->aCol[i]; if( pCol->iDflt ){ columnDefaultUncommonCase(v,pTab,pCol); }` —— 列有默认值时走 `columnDefaultUncommonCase`。
2. `if( pCol->affinity==SQLITE_AFF_REAL ) sqlite3VdbeAddOp1(v, OP_RealAffinity, iReg);` —— REAL 亲和的列额外发一条 `OP_RealAffinity`。`SQLITE_AFF_REAL` 的值是 `0x45`，定义在 `src/sqliteInt.h`。

`columnDefaultUncommonCase`（`SQLITE_NOINLINE` 标注的静态函数）调 `sqlite3ValueFromExpr(sqlite3VdbeDb(v), sqlite3ColumnExprAuth(pTab,pCol,pParse), enc, pCol->affinity, &pValue)`；若得到 `pValue`，则 `sqlite3VdbeAppendP4(v, pValue, P4_MEM)` 把默认值挂到最近一条 `OP_Column` 指令的 P4 参数上。

函数头注释解释了为什么默认值只可能以「字面量」形式出现：DEFAULT 值要么在建表时给出（此时所有行记录都含该列的值，不需要 P4），要么是 ALTER TABLE ADD COLUMN 后加的（此时磁盘上的行记录可能不含该列），而 ALTER TABLE 只能加字面量默认值（数字、NULL 或字符串）——更复杂的默认表达式在 ALTER TABLE 执行时就被求值成字面量写进 `sqlite_schema` 表了。

`sqlite3ColumnExprAuth`（定义在 `src/build.c`）在返回表达式前会调 `sqlite3FuncAuth(pParse, pExpr)`，即授权回调会检查默认值表达式里用到的 SQL 函数。

### 第六步：定位待更新的行——三种策略

`eOnePass` 的取值来自 `src/sqliteInt.h` 的三个常量：`ONEPASS_OFF`（`0`，不允许用 one-pass）、`ONEPASS_SINGLE`（`1`，one-pass 对单行更新有效）、`ONEPASS_MULTI`（`2`，one-pass 对多行更新有效）。

**多行 one-pass 的禁用条件**（源码注释列了五条）：

```
if( !pParse->nested
 && !pTrigger
 && !hasFK
 && !chngKey
 && !bReplace
 && (pWhere==0 || !ExprHasProperty(pWhere, EP_Subquery))
){
  flags |= WHERE_ONEPASS_MULTI;
}
pWInfo = sqlite3WhereBegin(pParse, pTabList, pWhere,0,0,0,flags,iIdxCur);
```

**one-pass 结果的复核**：`eOnePass = sqlite3WhereOkOnePass(pWInfo, aiCurOnePass);`，`bFinishSeek = sqlite3WhereUsesDeferredSeek(pWInfo);`。若 `eOnePass==ONEPASS_MULTI` 且 `aiCurOnePass[1]>=0 && aiCurOnePass[1]!=iDataCur && aToOpen[iCur-iBaseCur]`，则把 `eOnePass` 降为 `ONEPASS_OFF`。源码注释给出这条降级的理由：「若扫描所用索引的任一列正在被更新，则多行 one-pass 策略不可用。否则像 `UPDATE t1 SET b=b+1 WHERE b>?` 这样的语句会形成死循环。」

**两遍策略（ONEPASS_OFF）**：对有 rowid 的表，先 `OP_Rowid` 取出当前行 rowid 存进 `regOldRowid`，再 `OP_Insert` 把它写进临时表 `iEph`（一个 `OP_OpenEphemeral` 打开的 RowSet）。之后主循环用 `OP_Rewind` + `OP_Next` 遍历这个临时表。对 WITHOUT ROWID 表则是把主键各分量读进 `iPk` 起的寄存器，`OP_MakeRecord` 打包成一条记录，再 `OP_IdxInsert` 写进临时表。

**带 FROM 的 UPDATE**：`nChangeFrom` 非 0 时走 `updateFromSelect`，它把 `SELECT <other-columns>, pChanges FROM pTabList WHERE pWhere` 编译成一个查询，结果写进临时表 `iEph`。临时表的列数是 `nEphCol = nPk + nChangeFrom + (isView ? pTab->nCol : 0)`。带 FROM 时 `eOnePass` 被强制置为 `ONEPASS_OFF`。

**UPSERT 分支**：`pUpsert` 非 0 时 `pWInfo = 0; eOnePass = ONEPASS_SINGLE;`，且 `bFinishSeek = 0;`——因为 INSERT 已经打开了所有游标并定位到待更新的行，UPDATE 部分要跳过找行的过程。

### 第七步：装配新行寄存器数组

寄存器分配：`regOldRowid = regNewRowid = ++pParse->nMem;`；若 `chngPk || pTrigger || hasFK` 则再给旧行内容留 `pTab->nCol` 个寄存器（`regOld`）；若 `chngKey || pTrigger || hasFK` 则 `regNewRowid` 单独占一个；`regNew = pParse->nMem + 1; pParse->nMem += pTab->nCol;`。

旧行内容的读取由 `oldmask` 控制。`oldmask` 由两部分按位或得到：`sqlite3FkOldmask(pParse, pTab)`（外键涉及的列）与 `sqlite3TriggerColmask(pParse, pTrigger, pChanges, 0, TRIGGER_BEFORE|TRIGGER_AFTER, pTab, onError)`（触发器读到的 `old.*` 列）。逐列判断：`oldmask==0xffffffff`（全要）、`(i<32 && (oldmask & MASKBIT32(i))!=0)`、或该列是 `COLFLAG_PRIMKEY` 时读真值，否则 `OP_Null` 写 0。

新行数组的装配循环逐列分四种情形：

- `i==pTab->iPKey` → `OP_Null`（rowid 不放在列数组里）。
- 带 `COLFLAG_GENERATED` → 跳过；若同时带 `COLFLAG_VIRTUAL` 则 `k--`（虚拟生成列不占存储槽位）。
- `aXRef[i]>=0`（该列被 SET 改）→ `sqlite3ExprCode(pParse, pChanges->a[j].pExpr, k)` 计算新值。
- 未被改 → 若没有 BEFORE 触发器，或列号大于 31，或 `newmask & MASKBIT32(i)` 命中，则 `sqlite3ExprCodeGetColumnOfTable` 读原值；否则 `OP_Null` 写 0。

`newmask` 由 `sqlite3TriggerColmask(pParse, pTrigger, pChanges, 1, TRIGGER_BEFORE, pTab, onError)` 得到——注意 `isNew` 参数为 1，且只统计 BEFORE 触发器。若表带生成列且 BEFORE 触发器通过 `new.*` 读了生成列，源码把 `newmask` 提升为 `0xffffffff`（全要），因为此时无法精确知道哪些列会被用到。

**生成列的计算**：装配完数组后，若 `pTab->tabFlags & TF_HasGenerated` 则调 `sqlite3ComputeGeneratedColumns(pParse, regNew, pTab)`。这个函数在 `src/insert.c`，它先调 `sqlite3TableAffinity(pParse->pVdbe, pTab, iRegStore)` 做亲和转换，然后用「两遍算法」处理生成列间的相互引用：第一遍把所有生成列标上 `COLFLAG_NOTAVAIL` 标记（标记定义在 `src/sqliteInt.h`），第二遍才逐个求值。

### 第八步：触发器前后与约束检查的顺序

生成的代码序列严格是：

1. **BEFORE UPDATE 触发器**：`if( tmask&TRIGGER_BEFORE )` 分支里先 `sqlite3TableAffinity(v, pTab, regNew)`，再 `sqlite3CodeRowTrigger(pParse, pTrigger, TK_UPDATE, pChanges, TRIGGER_BEFORE, pTab, regOldRowid, onError, labelContinue)`。源码注释指出触发器在约束检查之前触发，并自陈「有人认为这是错的」。
2. **触发器可能删掉该行**：BEFORE 触发器之后发 `OP_NotFound`（有 PK 时）或 `OP_NotExists`（无 PK 时），跳到 `labelContinue` 跳到下一行。源码注释明确：这个行为在文档中未定义。
3. **触发器修改后的重载**：只对 `aXRef[i]<0 && i!=pTab->iPKey` 的列（即本 UPDATE 没有改的列）重新 `sqlite3ExprCodeGetColumnOfTable` 读一次，因为 BEFORE 触发器可能改过它们。若表带生成列，再算一次 `sqlite3ComputeGeneratedColumns`。
4. **约束检查**：`sqlite3GenerateConstraintChecks(pParse, pTab, aRegIdx, iDataCur, iIdxCur, regNewRowid, regOldRowid, chngKey, onError, labelContinue, &bReplace, aXRef, 0)`。
5. **可能的重新定位**：若 `bReplace || chngKey`，再发一次 `OP_NotFound`/`OP_NotExists` 定位到行——源码注释解释：约束检查过程可能移动了数据游标。
6. **外键检查**：`if( hasFK ) sqlite3FkCheck(pParse, pTab, regOldRowid, 0, aXRef, chngKey);`。
7. **删旧索引项**：`sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, aRegIdx, -1);`。
8. **`OP_FinishSeek`**：若 `bFinishSeek` 则发 `sqlite3VdbeAddOp1(v, OP_FinishSeek, iDataCur);`。源码注释说明只在「自 `OP_DeferredSeek` 以来可能没有过 `OP_Column`」时才必须做，同时要避免它因为耗 CPU。
9. **删旧行**：有 `SQLITE_ENABLE_PREUPDATE_HOOK` 时发 `OP_Delete`，P3 是 `OPFLAG_ISUPDATE | ((hasFK>1 || chngKey) ? 0 : OPFLAG_ISNOOP)`；否则只在 `hasFK>1 || chngKey` 时发 `OP_Delete`。
10. **外键再检查**：`if( hasFK ) sqlite3FkCheck(pParse, pTab, 0, regNewRowid, aXRef, chngKey);`。
11. **写新行**：`sqlite3CompleteInsertion(pParse, pTab, iDataCur, iIdxCur, regNewRowid, aRegIdx, OPFLAG_ISUPDATE | (eOnePass==ONEPASS_MULTI ? OPFLAG_SAVEPOSITION : 0), 0, 0);`。
12. **外键级联动作**：`if( hasFK ) sqlite3FkActions(pParse, pTab, pChanges, regOldRowid, aXRef, chngKey);`。
13. **行计数**：`if( regRowCount ) sqlite3VdbeAddOp2(v, OP_AddImm, regRowCount, 1);`。
14. **AFTER UPDATE 触发器**：`sqlite3CodeRowTrigger(..., TRIGGER_AFTER, ...)`。

行计数寄存器只在满足五个条件时才分配：`db->flags&SQLITE_CountRows` 非 0、不在触发器内（`!pParse->pTriggerTab`）、非嵌套（`!pParse->nested`）、非 RETURNING（`!pParse->bReturning`）、非 UPSERT。计数值最后由 `sqlite3CodeChangeCount(v, regRowCount, "rows updated")` 输出。

### 第九步：updateVirtualTable——虚拟表的另一条路径

虚拟表在 `sqlite3Update` 里被提前分流：`if( IsVirtual(pTab) ){ updateVirtualTable(...); goto update_cleanup; }`。`updateVirtualTable` 用 `OP_VUpdate` 调用虚拟表的 `xUpdate` 方法，参数个数是 `nArg = 2 + pTab->nCol`——前两个是旧 rowid 与新 rowid，后面是各列新值。

它有两条策略。默认策略把每行变更打包成一条记录存进临时表（`OP_OpenEphemeral` + `OP_MakeRecord` + `OP_NewRowid` + `OP_Insert`），然后遍历临时表逐条 `OP_VUpdate`；「onepass」策略不使用临时表，把同样的值放在寄存器数组里，只调一次 `OP_VUpdate`。onepass 策略只有在 `sqlite3WhereOkOnePass(pWInfo, aDummy)` 返回 `ONEPASS_SINGLE` 时使用——源码断言 `eOnePass==ONEPASS_OFF || eOnePass==ONEPASS_SINGLE`，即虚拟表上没有 `ONEPASS_MULTI`。

`OP_VUpdate` 的 P5 由 `sqlite3VdbeChangeP5(v, onError==OE_Default ? OE_Abort : onError)` 设定，冲突策略常量定义在 `src/sqliteInt.h`：`OE_Rollback`（`1`）、`OE_Abort`（`2`）、`OE_Fail`（`3`）、`OE_Ignore`（`4`）、`OE_Replace`（`5`）。

### 第十步：updateFromSelect——带 FROM 与视图的物化

`updateFromSelect` 是 `src/update.c` 的静态函数，负责把「哪些行要改、改成什么」的结果物化进临时表。

它的 `<other-columns>` 取决于源表类型：

- WITHOUT ROWID 表（`pPk` 非空）：取主键各列，顺序为 `pPk->aiColumn[i]`；结果以索引键形式写入临时表，用 `OP_IdxInsert`。
- 视图（`IsView(pTab)`）：取视图全部 `pTab->nCol` 列，结果以记录形式写入，用自动分配的整数键。
- 虚拟表或普通 intkey 表：`<other-columns>` 是 rowid。

目的地类型 `eDest` 同样分三种：有主键时 `IsVirtual(pTab) ? SRT_Table : SRT_Upfrom`；视图时 `SRT_Table`；其余情况 `IsVirtual(pTab) ? SRT_Table : SRT_Upfrom`。

排序与限额的约束：函数在 `SQLITE_ENABLE_UPDATE_DELETE_LIMIT` 下先检查 `if( pOrderBy && pLimit==0 ){ sqlite3ErrorMsg(pParse, "ORDER BY without LIMIT on UPDATE"); return; }`。这条错误的另一处实现在 `src/delete.c` 的 `sqlite3LimitWhere`，那里的消息模板是 `"ORDER BY without LIMIT on %s"`，`zStmtType` 参数传 `"UPDATE"`。

`sqlite3LimitWhere` 的调用点在 `sqlite3Update` 里，条件是 `!isView && nChangeFrom==0`，调用后 `pOrderBy = 0; pLimit = 0;` —— 也就是说 LIMIT/ORDER BY 语义被整体改写进 `pWhere`（形如 `rowid IN (SELECT rowid FROM ... ORDER BY ... LIMIT ...)`），后续不再单独处理。

视图路径由 `sqlite3MaterializeView`（定义在 `src/delete.c`）负责：它在 `sqlite3Update` 里的调用条件是 `nChangeFrom==0 && isView`，把视图定义物化进一个临时表，目标是 `SRT_EphemTab` 目的地。

## 关键取舍

- **源码事实：**`aXRef` 用 `99999` 作为生成列的标记值，而不是某个具名常量。**架构推论：**这个值不需要精确语义，只需要「非负即视为会变」，所以用一个大到不可能与 `pChanges->nExpr` 下标混淆的魔数最省事；源码注释也直接说这个选择是为了在符号调试器里一眼可辨。它同时被 `indexColumnIsBeingUpdated` 与 `indexWhereClauseMightChange` 依赖——这两个函数只检查 `>=0`，因此标记值不影响它们的结论。

- **源码事实：**`indexColumnIsBeingUpdated` 与 `indexWhereClauseMightChange` 的注释都写明「假阳性只是性能退化，假阴性会导致索引损坏和错误答案」。**架构推论：**SQLite 在索引重写判定上刻意偏保守：宁可多算几个要改的索引（多分配寄存器、多发删除/插入指令），也不能漏掉一个。判定不精确的代价落在性能侧，判定过松的代价落在正确性侧。

- **源码事实：**多行 one-pass 在 `pParse->nested`、`pTrigger`、`hasFK`、`chngKey`、`bReplace` 任一成立，或 WHERE 子句含子查询时都被禁用。**架构推论：**这五类情形都会让「扫描游标」在 UPDATE 过程中失效或需要额外处理——嵌套 UPDATE 会再开一个扫描、触发器与外键可能读写同一张表、rowid/主键变更会让扫描位置失效、REPLACE 需要删除冲突行。因此 one-pass 只保留在「扫一行改一行、且这一行不会被动到别处」的窄路径上，用空间换复杂度。

- **源码事实：**`UPDATE t1 SET b=b+1 WHERE b>?` 这类语句会触发 `ONEPASS_MULTI` → `ONEPASS_OFF` 的降级。**架构推论：**若用同一个索引既扫描又改写，改动会让扫描位置跳到别处，下一轮可能重复命中同一行而无限循环；降级到两遍策略（先收集行 id 再逐行改）把「扫描」与「改写」在时间上隔开，代价是多一个临时表。

- **源码事实：**BEFORE UPDATE 触发器在约束检查之前触发，源码注释自陈「有人认为这是错的」。**架构推论：**触发器先于约束执行，意味着触发器看到的是尚未校验的新行；反过来若约束先跑，触发器里的写入就建立在已确认合法的行上。SQLite 选择前者，是为了让 BEFORE 触发器具备「否决这次更新」的能力（例如抛 RAISE(ABORT)），这也是源文件里 BEFORE 触发器后紧跟 `OP_NotFound`/`OP_NotExists` 检查的由来。

- **源码事实：**BEFORE 触发器之后只重载 `aXRef[i]<0` 的列，被改的列不重载。**架构推论：**被 SET 显式赋值的列，其值以 UPDATE 语句里的表达式为准，触发器改不动它们（除非触发器再次显式赋值）；而未被 SET 覆盖的列可能被 BEFORE 触发器修改，所以必须从库里重新读一遍。源码注释提到测试用例 `trigger1-18.0` 记录了这一行为。

- **源码事实：**`bReplace` 为真时把 `aToOpen` 全部置 1，强制打开所有索引游标。**架构推论：**REPLACE 冲突解决要在插入新行之前删除已有的冲突记录，删除哪条取决于哪个唯一索引先冲突，因此所有索引都必须可用；代价是即使某索引的列根本没被改，也要开游标。

- **源码事实：**虚拟表没有 `ONEPASS_MULTI`，只有 `ONEPASS_OFF` 与 `ONEPASS_SINGLE` 两种。**架构推论：**虚拟表的 `xUpdate` 一次只能处理一行的更新，SQLite 无法像普通表那样让一个索引扫描同时充当写游标，因此虚拟表的多行更新必须走「临时表收集 → 逐条回调」的两遍路径。

- **源码事实：**`sqlite3ColumnDefault` 只在 `pCol->iDflt` 非 0 时挂默认值，且 REAL 亲和列无条件追加 `OP_RealAffinity`。**架构推论：**默认值只在「ALTER TABLE ADD COLUMN 后磁盘行记录不含该列」这一种场景下才是必需的，所以 `iDflt` 这个存在性标志就足以区分两种情况；而 REAL 亲和的额外转换是因为注释所述的情形——该值可能以整数形式被存过，取出后需要显式转成 REAL 才能与列的声明亲和性一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/update.c` | `sqlite3Update` | UPDATE 编译总入口：解析 SET 项、判定 rowid/主键变更、传播生成列依赖、决定索引是否重写、选择 one-pass / 两遍 / 带 FROM 三种定位策略、生成新行寄存器数组、串起约束检查与写入序列 |
| `src/update.c` | `sqlite3ColumnDefault` | 给列装默认值：列有 `iDflt` 时走 `columnDefaultUncommonCase` 挂 P4；列亲和性为 `SQLITE_AFF_REAL` 时追加 `OP_RealAffinity` |
| `src/update.c` | `columnDefaultUncommonCase` | 用 `sqlite3ValueFromExpr` 把默认值表达式转成 `sqlite3_value`，再 `sqlite3VdbeAppendP4` 挂到 `OP_Column` 的 P4 参数上 |
| `src/update.c` | `indexColumnIsBeingUpdated` | 判断索引第 i 个键列是否引用了被更新的列（含表达式索引走 `sqlite3ExprReferencesUpdatedColumn`） |
| `src/update.c` | `indexWhereClauseMightChange` | 判断部分索引的条件表达式是否会因本次更新而变 |
| `src/update.c` | `exprRowColumn` | 造一个 `TK_ROW` 表达式，由名字解析阶段改写为读源表第 i 列的 `TK_COLUMN` |
| `src/update.c` | `updateFromSelect` | 把「待更新的行 + 新值」物化进临时表：无 PK 取主键列、视图取全部列、其余取 rowid；`eDest` 按 `SRT_Table` / `SRT_Upfrom` 分派 |
| `src/update.c` | `updateVirtualTable` | 虚拟表 UPDATE 的编译：准备 `nArg = 2 + pTab->nCol` 个 `xUpdate` 参数、选 onepass 或临时表两条策略、发 `OP_VUpdate` |
| `src/parse.y` | `cmd ::= with UPDATE ...` | UPDATE 语句的语法动作，调用 `sqlite3Update(pParse,X,Y,W,R,O,L,0)` |
| `src/insert.c` | `sqlite3ComputeGeneratedColumns` | 计算生成列的值：先做亲和转换，再用两遍算法解生成列间的相互引用 |
| `src/insert.c` | `sqlite3ExprReferencesUpdatedColumn` | 判定一个表达式是否引用了被更新的列或 rowid；Walker 回调 `checkConstraintExprNode` 置 `CKCNSTRNT_COLUMN`（`0x01`）/ `CKCNSTRNT_ROWID`（`0x02`）位 |
| `src/insert.c` | `sqlite3CompleteInsertion` | 写入新行：遍历索引生成插入指令；部分索引或表达式索引先用 `OP_IsNull` 判断是否跳过 |
| `src/insert.c` | `sqlite3TableAffinity` | 给寄存器数组施加列亲和性；表是 STRICT 时生成 `OP_TypeCheck` |
| `src/delete.c` | `sqlite3GenerateRowIndexDelete` | 生成删除旧索引项的代码：`sqlite3GenerateIndexKey` 组键后发 `OP_IdxDelete`；`aRegIdx[i]==0` 的索引跳过 |
| `src/delete.c` | `sqlite3MaterializeView` | 把视图定义物化进临时表，供 INSTEAD OF 触发器路径使用 |
| `src/delete.c` | `sqlite3LimitWhere` | 把 UPDATE/DELETE 的 ORDER BY + LIMIT 改写成 `rowid IN (SELECT ...)` 形式的 WHERE 表达式 |
| `src/delete.c` | `sqlite3IsReadOnly` | 判定表是否可写：只读表报 `table %s may not be modified`；无 INSTEAD OF 触发器的视图报 `cannot modify %s because it is a view` |
| `src/build.c` | `sqlite3ColumnExprAuth` | 取列的默认值/生成表达式，并调 `sqlite3FuncAuth` 让授权回调检查其中用到的 SQL 函数 |
| `src/trigger.c` | `sqlite3TriggerColmask` | 统计触发器通过 `old.*` / `new.*` 访问了哪些列，用掩码返回；视图直接返回 `0xffffffff` |
| `src/fkey.c` | `sqlite3FkRequired` | 判断本次 UPDATE 是否需要外键处理；返回 2 表示需要先删旧行再做 FK 检查（父键或自引用情形） |
| `src/fkey.c` | `sqlite3FkOldmask` | 返回外键涉及的列掩码，供 UPDATE 决定读哪些旧列；`COLUMN_MASK(x)` 对大于 31 的列号返回 `0xffffffff` |
| `src/where.c` | `sqlite3WhereOkOnePass` | 回报 WHERE 规划的结果：`ONEPASS_OFF`（`0`）/ `ONEPASS_SINGLE`（`1`）/ `ONEPASS_MULTI`（`2`），并给出用到的两个游标号 |
| `src/where.c` | `sqlite3WhereUsesDeferredSeek` | 回报 WHERE 循环是否用了 `OP_DeferredSeek`，决定是否需要发 `OP_FinishSeek` |
| `src/sqliteInt.h` | `COLFLAG_PRIMKEY` / `COLFLAG_GENERATED` / `COLFLAG_VIRTUAL` / `COLFLAG_STORED` | 列标志位，值分别是 `0x0001` / `0x0060` / `0x0020` / `0x0040`；`COLFLAG_GENERATED` 是 `_VIRTUAL` 与 `_STORED` 的组合掩码 |
| `src/sqliteInt.h` | `TF_HasGenerated` / `TF_HasVirtual` / `TF_HasStored` | 表级标志位，值分别是 `0x00000060` / `0x00000020` / `0x00000040` |
| `src/sqliteInt.h` | `ONEPASS_OFF` / `ONEPASS_SINGLE` / `ONEPASS_MULTI` | one-pass 策略的三种取值：`0` / `1` / `2` |
| `src/sqliteInt.h` | `OE_Rollback` / `OE_Abort` / `OE_Fail` / `OE_Ignore` / `OE_Replace` | 冲突解决策略常量，值分别是 `1` / `2` / `3` / `4` / `5` |
| `src/sqliteInt.h` | `SQLITE_AFF_REAL` | REAL 亲和性常量，值 `0x45` |
| `src/sqliteInt.h` | `MASKBIT32` | 掩码位宏，定义为 `(((unsigned int)1)<<(n))` |

## 与主核 / AI 的关系

主核（`engine/db/` 自研多模态数据库内核）的写路径与 SQLite 的 UPDATE 编译有可对照的结构：两者都要先确定「改哪些列」，再把新行装配成一条记录，最后做约束检查与写入。差别在于 SQLite 把「改哪些列」编码成一张 `aXRef` 表并在多处复用（索引重写判定、外键判定、生成列传播、旧行读取掩码），而主核的写路径通常按「列槽位 + 更新位图」表达同一件事。

AI 侧没有直接对应实现。本页可作为理解 SQLite「一个 UPDATE 语句编译出的 VDBE 程序长什么样」的材料：一次 UPDATE 会产生「扫描/收集 → 定位行 → 读旧值 → 算新值 → 触发器 → 约束检查 → 删旧索引项 → 删旧行 → 插新索引项 → 插新行 → 外键动作 → 计数」这样的固定序列；其中「是否用 one-pass」「哪些索引要重写」「生成列何时重算」三个判定点是 SQLite 用少量布尔/位掩码表达策略选择的地方。

## 参考

- [SQLite 官方文档：UPDATE 语句](https://sqlite.org/lang_update.html) —— UPDATE 的标准语义、UPSERT 的 `ON CONFLICT` 形式、以及 `sqlite3_changes()` 计数含义
- [SQLite 官方文档：生成列](https://sqlite.org/gencol.html) —— STORED / VIRTUAL 生成列的定义与「不能直接 UPDATE 生成列」的限制
- [SQLite 官方文档：DEFAULT 子句](https://sqlite.org/lang_createtable.html#defaultvalues) —— DEFAULT 值的求值规则，以及 `ALTER TABLE ADD COLUMN` 追加默认值的行为
- [SQLite 官方文档：外键](https://sqlite.org/foreignkeys.html) —— 外键的 `ON DELETE` / `ON UPDATE` 动作与 `PRAGMA foreign_keys` 开关
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明代码生成器在流水线上的位置
- [SQLite 源码：src/update.c](https://github.com/sqlite/sqlite/blob/master/src/update.c) —— 本页 `refs` 指向的文件，`sqlite3Update` 与 `sqlite3ColumnDefault` 的实现
- [SQLite 源码：src/insert.c](https://github.com/sqlite/sqlite/blob/master/src/insert.c) —— `sqlite3ComputeGeneratedColumns`、`sqlite3ExprReferencesUpdatedColumn`、`sqlite3CompleteInsertion`、`sqlite3TableAffinity`
- [SQLite 源码：src/delete.c](https://github.com/sqlite/sqlite/blob/master/src/delete.c) —— `sqlite3GenerateRowIndexDelete`、`sqlite3MaterializeView`、`sqlite3LimitWhere`、`sqlite3IsReadOnly`
