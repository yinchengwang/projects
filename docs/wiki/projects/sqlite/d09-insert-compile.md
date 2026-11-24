---
project: sqlite
feature: insert-compile
page_type: feature
schema_version: 1
refs:
  - path: src/insert.c
    symbols: [sqlite3Insert, sqlite3CompleteInsertion, sqlite3AutoincrementBegin]
---

# INSERT 编译

## 一句话职责

本页负责把一条 `INSERT` 语句编译成 VDBE 程序：定位目标表、解析列清单与 VALUES 列表、决定 rowid 怎么来（显式给值、用 `OP_NewRowid` 自动分配、还是走 AUTOINCREMENT 序列）、把新行装配进寄存器数组，最后发出 `OP_IdxInsert` / `OP_Insert` 完成写入。本页不负责 B-Tree 的实际写入，也不负责唯一/外键约束的检查语义。

## 机制是什么

边界（依据条目 `boundary`）：负责 INSERT 语句的编译：拼装新行、分配 rowid、维护自增序列；不负责 B-tree 的写入（F 组）、不负责唯一/外键约束的检查（K 组）。

INSERT 编译的入口是 `src/insert.c` 的 `sqlite3Insert`，由 `src/parse.y` 的两条语法规则调用：`cmd ::= with insert_cmd(R) INTO xfullname(X) idlist_opt(F) select(S) upsert(U)` 调 `sqlite3Insert(pParse, X, S, F, R, U)`；`cmd ::= with insert_cmd(R) INTO xfullname(X) idlist_opt(F) DEFAULT VALUES returning` 调 `sqlite3Insert(pParse, X, 0, F, R, 0)`——后者数据源为 0，走 `INSERT INTO ... DEFAULT VALUES` 路径。

整个编译过程由一组寄存器编号贯穿，这组编号是「行」在 VDBE 里的物理表示：

| 寄存器 | 含义 | 分配处 |
|---|---|---|
| `regRowid` | 存放新行的 rowid | `sqlite3Insert` 起始段 |
| `regData` | 新行第一个数据列的寄存器 | `regRowid+1` |
| `regIns` | `OP_Insert` 用的行记录块基址 | `regRowid`（虚拟表额外前移） |
| `aRegIdx[i]` | 第 i 个索引的键寄存器；0 表示该索引不需要写 | 打开表与索引后分配 |

`sqlite3Insert` 内部要处理四种数据来源，对应源码注释里说的「四种模板」：

| 模板 | 形态 | 路径 |
|---|---|---|
| 模板 1 | `INSERT ... VALUES(...)` 单行 | `pList` 直接求值 |
| 模板 2 | `INSERT INTO t1 SELECT * FROM t2` | `xferOptimization` 转移优化 |
| 模板 3 | `INSERT ... SELECT` 逐行直写目标表 | 协程序 + `OP_Yield` 循环 |
| 模板 4 | `INSERT ... SELECT` 先落临时表再逐行搬 | `OP_OpenEphemeral` 临时表 + `OP_Next` 循环 |

## 怎么实现的

### 第一步：入口检查与表定位

`sqlite3Insert` 的开头是一串「拒绝与定位」判断。

- `if( pParse->nErr ) goto insert_cleanup;` —— 上游已报错直接返回。
- 单行 VALUES 会被就地简化：`if( pSelect && (pSelect->selFlags & SF_Values)!=0 && pSelect->pPrior==0 && pSelect->pSrc->nSrc==0 )` 时，把 `pSelect->pEList` 摘到 `pList`，然后 `sqlite3SelectDelete(db, pSelect); pSelect = 0;` —— 销毁整个 Select 树，只留表达式列表。
- `pTab = sqlite3SrcListLookup(pParse, pTabList);`，`iDb = sqlite3SchemaToIndex(db, pTab->pSchema)` 拿到库序号。
- 授权：`sqlite3AuthCheck(pParse, SQLITE_INSERT, pTab->zName, 0, db->aDb[iDb].zDbSName)` 返回非 0 即返回。
- `withoutRowid = !HasRowid(pTab);`。
- 触发器与视图：`pTrigger = sqlite3TriggersExist(pParse, pTab, TK_INSERT, 0, &tmask);`，`isView = IsView(pTab);`。`tmask` 是 `TRIGGER_BEFORE`（`1`）与 `TRIGGER_AFTER`（`2`）的掩码，两者都定义在 `src/sqliteInt.h`。
- `sqlite3ViewGetColumnNames(pParse, pTab)` 失败即返回；`sqlite3IsReadOnly(pParse, pTab, pTrigger)` 返回非 0 也返回。
- 取 VDBE：`v = sqlite3GetVdbe(pParse);`，非顶层时 `sqlite3VdbeCountChanges(v)`，然后 `sqlite3BeginWriteOperation(pParse, pSelect || pTrigger, iDb)` 开启写操作。

### 第二步：转移优化（模板 2）——一条直接跳出去的旁路

在分配自增信息之前，`sqlite3Insert` 会尝试一条快速路径：

```
if( pColumn==0
 && pSelect!=0
 && pTrigger==0
 && xferOptimization(pParse, pTab, pSelect, onError, iDb)
){
  goto insert_end;
}
```

`xferOptimization`（`src/insert.c` 静态函数）有一长串前置条件，任一不满足就返回 0：不能带 WITH（`if( pParse->pWith || pSelect->pWith ) return 0;`，注释说明理由是 CTE 可能与目标表同名导致误报 "no such table"）、目标表不能是虚拟表、FROM 子句必须恰好一项且不能是子查询、不能有 `INDEXED BY`、不能有 WHERE / ORDER BY / GROUP BY / LIMIT、不能是复合查询、不能是 DISTINCT、结果集必须恰好一列且该列必须是 `TK_ASTERISK`（即 `SELECT *`）、源表与目标表不能是同一张表。

条件全满足时它生成的是「整表搬字节」式的代码：源表用 `OP_OpenRead` 打开、目标表用 `OP_OpenWrite` 打开，主循环里 `sqlite3VdbeAddOp3(v, OP_RowCell, iDest, iSrc, regRowid)` 把源行整条复制到目标表，再 `sqlite3VdbeAddOp3(v, OP_Insert, iDest, regData, regRowid)` 落库。

插入标志位按场景分两种（源码字面量）：

```
if( db->mDbFlags & DBFLAG_Vacuum ){
  insFlags = OPFLAG_APPEND|OPFLAG_USESEEKRESULT|OPFLAG_PREFORMAT;
}else{
  insFlags = OPFLAG_NCHANGE|OPFLAG_LASTROWID|OPFLAG_APPEND|OPFLAG_PREFORMAT;
}
```

即 VACUUM 场景不带 `OPFLAG_NCHANGE`（不更新 `db->nChange`）与 `OPFLAG_LASTROWID`（不更新 `db->lastRowid`）。`OPFLAG_NCHANGE` 的值是 `0x01`，`OPFLAG_LASTROWID` 是 `0x20`，`OPFLAG_APPEND` 是 `0x08`，`OPFLAG_USESEEKRESULT` 是 `0x10`，`OPFLAG_PREFORMAT` 是 `0x80`，全部定义在 `src/sqliteInt.h`。

VACUUM 场景还会在索引层多做一个判断：只有当源索引所有列的排序规则都是 BINARY 时才启用 `OP_SeekEnd` 预定位——代码遍历 `pSrcIdx->azColl[i]`，用 `sqlite3_stricmp(sqlite3StrBINARY, zColl)` 比较，一旦发现非 BINARY 就 break，否则发 `OP_SeekEnd` 并设 `idxInsFlags = OPFLAG_USESEEKRESULT|OPFLAG_PREFORMAT`。源码注释解释：用户可能改了排序规则定义再跑 VACUUM，键就不再严格有序。

### 第三步：自增序列的登记——autoIncBegin

`regAutoinc = autoIncBegin(pParse, iDb, pTab);` 决定本条语句是否需要 AUTOINCREMENT 语义。

`autoIncBegin`（`src/insert.c` 静态函数）的进入条件是两个与运算同时成立：`(pTab->tabFlags & TF_Autoincrement)!=0`（`TF_Autoincrement` 的值是 `0x00000008`，定义在 `src/sqliteInt.h`）且 `(pParse->db->mDbFlags & DBFLAG_Vacuum)==0`——VACUUM 期间不更新自增计数器。

它还要校验 `sqlite_sequence` 表的形状，条件是 `pSeqTab==0 || !HasRowid(pSeqTab) || NEVER(IsVirtual(pSeqTab)) || pSeqTab->nCol!=2`——必须是普通 rowid 表且恰好两列。任一不满足则 `pParse->nErr++; pParse->rc = SQLITE_CORRUPT_SEQUENCE; return 0;`（源码注释挂着 ticket `d8dc2b3a58cd5dc2918a1d4acb 2018-05-23`）。

登记结构体 `AutoincInfo` 挂在顶层 Parse 的 `pToplevel->pAinc` 链表上，查找时 `while( pInfo && pInfo->pTab!=pTab ){ pInfo = pInfo->pNext; }`——同一张表在触发器里被多次插入也只占一个结构体。首次登记时分配四个连续寄存器（源码注释逐条列出）：表名、`pTab` 的最大 ROWID、`sqlite_sequence` 里的 rowid、原始最大 ROWID 或 NULL。

### 第四步：sqlite3AutoincrementBegin —— 把序列初值读进寄存器

`sqlite3AutoincrementBegin` 是条目 refs 里的第三个符号，定义在 `src/insert.c`。它的调用点在 `src/build.c` 的代码生成收尾阶段：`if( pParse->usesAinc ) sqlite3AutoincrementBegin(pParse);` —— 也就是说它不在 INSERT 编译内部被调用，而是在 `sqlite3FinishCoding` 收尾时统一触发。

函数开头的断言说明它的调用条件：`assert( pParse->pTriggerTab==0 );`（触发器生成期不会调它）、`assert( sqlite3IsToplevel(pParse) );`（只在顶层）、`assert( v );`、`assert( pParse->usesAinc );`。

它对 `pParse->pAinc` 链表上每个 `AutoincInfo` 生成一段固定的 12 条指令（源码里的 `autoInc[]` 数组，`ArraySize(autoInc)` 为 12）：

```
{OP_Null, 0, 0, 0},   {OP_Rewind, 0, 10, 0}, {OP_Column, 0, 0, 0},
{OP_Ne, 0, 9, 0},     {OP_Rowid, 0, 0, 0},    {OP_Column, 0, 1, 0},
{OP_AddImm, 0, 0, 0}, {OP_Copy, 0, 0, 0},     {OP_Goto, 0, 11, 0},
{OP_Next, 0, 2, 0},   {OP_Integer, 0, 0, 0},  {OP_Close, 0, 0, 0}
```

逐条填参数后，这段代码的语义是：打开 `sqlite_sequence` 表（`sqlite3OpenTable(pParse, 0, p->iDb, pDb->pSchema->pSeqTab, OP_OpenRead)`），把表名装进 `memId-1` 号寄存器（`sqlite3VdbeLoadString(v, memId-1, p->pTab->zName)`），然后遍历 `sqlite_sequence` 的每一行，比较表名是否匹配当前表；匹配则取该行的 `seq` 列值（`OP_Column` 的 p3 被改成 `memId`，读的是 `seq` 列），用 `OP_AddImm` 加 1，再 `OP_Copy` 回 `memId+2` 号寄存器（源码注释：orig max val，即原始最大 ROWID）。表名不匹配则 `OP_Ne` 跳到 `OP_Next` 继续下一行。遍历结束后 `OP_Integer` 把 `memId` 置 0（没有匹配行则序列从 0 起）。

`OP_Ne` 的 P5 被设为 `SQLITE_JUMPIFNULL`，与 `OP_Ne` 配合实现「列为 NULL 也算不匹配」的语义。

### 第五步：列清单解析与默认值归属

如果 INSERT 带 IDLIST（列名清单），`sqlite3Insert` 会建一张 `aTabColMap`（`sqlite3DbMallocZero(db, pTab->nCol*sizeof(int))`），把「表的列下标 → IDLIST 中的第几项」映射起来：

```
for(i=0; i<pColumn->nId; i++){
  j = sqlite3ColumnIndex(pTab, pColumn->a[i].zName);
  if( j>=0 ){
    if( aTabColMap[j]==0 ) aTabColMap[j] = i+1;
    if( i!=j ) bIdListInOrder = 0;
    if( j==pTab->iPKey ){ ipkColumn = i; assert( !withoutRowid ); }
    ...
  }else{
    if( sqlite3IsRowid(pColumn->a[i].zName) && !withoutRowid ){
      ipkColumn = i;
      bIdListInOrder = 0;
    }else{
      sqlite3ErrorMsg(pParse, "table %S has no column named %s", ...);
      pParse->checkSchema = 1;
      goto insert_cleanup;
    }
  }
}
```

其中 `bIdListInOrder` 初值是 `(pTab->tabFlags & (TF_OOOHidden|TF_HasStored))==0`；只要有一列的 IDLIST 位置与存储顺序不一致就置 0。源码注释明确写了这个标志的语义：「假阴性无害，假阳性会导致数据库损坏」。

对生成列，插入被直接拒绝：`if( pTab->aCol[j].colFlags & (COLFLAG_STORED|COLFLAG_VIRTUAL) )` 时报 `cannot INSERT into generated column "%s"`。`COLFLAG_STORED` 的值是 `0x0040`，`COLFLAG_VIRTUAL` 是 `0x0020`，定义在 `src/sqliteInt.h`。

列数不匹配有两处检查：

- 无 IDLIST 时：`if( nColumn!=(pTab->nCol-nHidden) )` 报 `table %S has %d columns but %d values were supplied`。`nHidden` 的算法是遍历 `pTab->aCol[i].colFlags & COLFLAG_NOINSERT` 计数，`COLFLAG_NOINSERT` 的值是 `0x0062`（`_HIDDEN`、`_STORED`、`_VIRTUAL` 的组合掩码）。
- 有 IDLIST 时：`if( pColumn!=0 && nColumn!=pColumn->nId )` 报 `%d values for %d columns`。

### 第六步：列值的装配循环

`sqlite3Insert` 有一个遍历 `pTab->nCol` 的主循环，把每个普通列的值算进 `regData` 起的连续寄存器。每列的分支顺序是：

1. `if( i==pTab->iPKey )` —— INTEGER PRIMARY KEY 列发一条 `OP_SoftNull` 占位。源码注释（tag-20191021-002）解释：文件格式要求记录里必须留一个 NULL，不能整列省略。
2. `if( (colFlags & COLFLAG_NOINSERT)!=0 )` —— 虚拟生成列不参与 `OP_MakeRecord`，`iRegStore--` 回退一个槽位；存储生成列留待稍后计算，但若有 BEFORE 触发器则先发 `OP_SoftNull` 防止读未初始化寄存器；隐藏列且未被 IDLIST 显式点名时取默认值。
3. `if( pColumn )` —— IDLIST 里没点名的列取默认值。
4. `else if( nColumn==0 )` —— `INSERT ... DEFAULT VALUES`，所有列取默认值。
5. 其余情况按数据来源取值：临时表路径发 `OP_Column`；SELECT 路径发 `OP_SCopy`；VALUES 路径用 `sqlite3ExprCodeTarget` 求值，若目标寄存器与返回值不同则补一条 `OP_Copy` 或 `OP_SCopy`（子查询用 `OP_Copy`，由 `ExprHasProperty(pX, EP_Subquery)` 判定）。

取默认值的统一写法是 `sqlite3ExprCodeFactorable(pParse, sqlite3ColumnExprAuth(pTab, &pTab->aCol[i], pParse), iRegStore)`。`sqlite3ColumnExprAuth` 定义在 `src/build.c`。

### 第七步：rowid 的三种来源

rowid 的计算在列值装配之后（源码注释：生成列可能在 rowid 算完之后才计算，因为它们可能依赖 rowid）。

**情形一：IDLIST 里显式给了 INTEGER PRIMARY KEY 的值。** 走 `sqlite3ExprCode(pParse, pList->a[ipkColumn].pExpr, regRowid)`。若该表达式是字面 NULL（`pIpk->op==TK_NULL`）且不是虚拟表，则改走 `sqlite3VdbeAddOp3(v, OP_NewRowid, iDataCur, regRowid, regAutoinc)` 并置 `appendFlag = 1`。

**情形二：显式给了值但不是 NULL。** 补一段空值检查与整数强制：

```
addr1 = sqlite3VdbeAddOp1(v, OP_NotNull, regRowid);
sqlite3VdbeAddOp3(v, OP_NewRowid, iDataCur, regRowid, regAutoinc);
sqlite3VdbeJumpHere(v, addr1);
sqlite3VdbeAddOp1(v, OP_MustBeInt, regRowid);
```

即：值非 NULL 就沿用；值是 NULL 才用 `OP_NewRowid` 生成。`OP_MustBeInt` 把 rowid 强制成整数，随后 `sqlite3MayAbort(pParse)` 标记该指令可能引发回滚。

**情形三：完全没给 rowid。** `sqlite3VdbeAddOp3(v, OP_NewRowid, iDataCur, regRowid, regAutoinc); appendFlag = 1;`——第三个参数 `regAutoinc` 非 0 就表示该表是 AUTOINCREMENT 表，`OP_NewRowid` 必须从 `sqlite_sequence` 的最大值往后取。

WITHOUT ROWID 表和虚拟表走另一分支：`sqlite3VdbeAddOp2(v, OP_Null, 0, regRowid);`。

`OP_NewRowid` 的行为在 `src/vdbe.c`：先 `sqlite3BtreeLast` 找最大 rowid 加一；最大值常量 `MAX_ROWID` 在 `SQLITE_32BIT_ROWID` 下是 `0x7fffffff`，否则是 `(i64)( (((u64)0x7fffffff)<<32) | (u64)0xffffffff )`。当现值已达 `MAX_ROWID` 时置 `pC->useRandomRowid = 1`，转而随机取 rowid 并 `v &= (MAX_ROWID>>1); v++;` 保证为正，最多重试 100 次（`while( ... && (++cnt<100) )`）；仍失败则报 `SQLITE_FULL`。AUTOINCREMENT 表则不允许随机模式：`if( pMem->u.i==MAX_ROWID || pC->useRandomRowid ){ rc = SQLITE_FULL; ... }`。

### 第八步：自增计数器的推进——autoIncStep

rowid 定下来后，`autoIncStep(pParse, regAutoinc, regRowid);` 推进序列最大值。函数体只有一条有效语句：

```
static void autoIncStep(Parse *pParse, int memId, int regRowid){
  if( memId>0 ){
    sqlite3VdbeAddOp2(pParse->pVdbe, OP_MemMax, memId, regRowid);
  }
}
```

`memId>0` 是「本表确实登记了 AUTOINCREMENT」的判据——非 AUTOINCREMENT 表 `autoIncBegin` 返回 0，这条指令就不会发出。`OP_MemMax` 把两个内存单元取大后写回，即维护「最大已用 rowid」。

收尾时 `sqlite3AutoincrementEnd(pParse)` 把序列写回 `sqlite_sequence`。它的调用点在 `sqlite3Insert` 的 `insert_end` 标签之后：`if( pParse->nested==0 && pParse->pTriggerTab==0 ){ sqlite3AutoincrementEnd(pParse); }` —— 只有顶层语句且不在触发器生成期才写回。`sqlite3AutoincrementEnd` 内部 `if( pParse->usesAinc ) autoIncrementEnd(pParse);`，而 `autoIncrementEnd`（`src/insert.c` 静态函数）同样对链表上每个 `AutoincInfo` 生成一段 5 条指令的 `autoIncEnd[]` 数组。

### 第九步：约束检查与写入——sqlite3CompleteInsertion

`sqlite3Insert` 先调约束检查：

```
sqlite3GenerateConstraintChecks(pParse, pTab, aRegIdx, iDataCur, iIdxCur,
    regIns, 0, ipkColumn>=0, onError, endOfLoop, &isReplace, 0, pUpsert);
if( db->flags & SQLITE_ForeignKeys ){
  sqlite3FkCheck(pParse, pTab, 0, regIns, 0, 0);
}
```

`sqlite3GenerateConstraintChecks` 定义在 `src/insert.c`（它也被 UPDATE 复用，参数 `regOldData` 非 0 即视为 UPDATE）。`sqlite3FkCheck` 定义在 `src/fkey.c`。`isReplace` 是输出参数，表示本次插入可能触发 REPLACE 冲突解决。

然后是条目 refs 里的第二个符号 `sqlite3CompleteInsertion`。它遍历 `pTab->pIndex` 链逐索引写入：

```
for(i=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, i++){
  if( aRegIdx[i]==0 ) continue;
  if( pIdx->pPartIdxWhere || (update_flags && pIdx->bHasExpr) ){
    sqlite3VdbeAddOp2(v, OP_IsNull, aRegIdx[i], sqlite3VdbeCurrentAddr(v)+2);
  }
  pik_flags = (useSeekResult ? OPFLAG_USESEEKRESULT : 0);
  if( IsPrimaryKeyIndex(pIdx) && !HasRowid(pTab) ){
    pik_flags |= OPFLAG_NCHANGE;
    pik_flags |= (update_flags & OPFLAG_SAVEPOSITION);
    if( update_flags==0 ){
      codeWithoutRowidPreupdate(pParse, pTab, iIdxCur+i, aRegIdx[i]);
    }
  }
  sqlite3VdbeAddOp4Int(v, OP_IdxInsert, iIdxCur+i, aRegIdx[i],
                       aRegIdx[i]+1,
                       pIdx->uniqNotNull ? pIdx->nKeyCol: pIdx->nColumn);
  sqlite3VdbeChangeP5(v, pik_flags);
}
```

三点值得注意：

- 部分索引（`pIdx->pPartIdxWhere` 非 0）或 UPDATE 场景下的表达式索引，会先用 `OP_IsNull` 判断记录是否为 NULL——为 NULL 表示这一行不进该索引，跳过插入。
- `OP_IdxInsert` 的第 4 个参数是索引的键列数：`pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn`——唯一索引且非 NULL 语义时只比较键列，否则比较全部列。
- `aRegIdx[i]==0` 的索引被跳过（该索引不需要写）。

表数据本身的写入在索引之后：

```
if( !HasRowid(pTab) ) return;
if( pParse->nested ){
  pik_flags = 0;
}else{
  pik_flags = OPFLAG_NCHANGE;
  pik_flags |= (update_flags?update_flags:OPFLAG_LASTROWID);
}
if( appendBias ){ pik_flags |= OPFLAG_APPEND; }
if( useSeekResult ){ pik_flags |= OPFLAG_USESEEKRESULT; }
sqlite3VdbeAddOp3(v, OP_Insert, iDataCur, aRegIdx[i], regNewData);
if( !pParse->nested ){ sqlite3VdbeAppendP4(v, pTab, P4_TABLE); }
sqlite3VdbeChangeP5(v, pik_flags);
```

关键点：`if( !HasRowid(pTab) ) return;` —— WITHOUT ROWID 表的数据就存在主键索引里，前面遍历索引时已经写完，这里直接返回，不发 `OP_Insert`。`pParse->nested` 为真（触发器内嵌语句）时不带 `OPFLAG_NCHANGE` 与 `OPFLAG_LASTROWID`，也不挂 `P4_TABLE`。

`useSeekResult` 的取值来自调用方：`bUseSeek = (isReplace==0 || !sqlite3VdbeHasSubProgram(v));`。源码注释解释：只有「没有 REPLACE 约束」或「没有子程序」时才能安全地省掉一次 seek——REPLACE 命中时会执行 `OP_Delete` 或 `OP_IdxDelete`，这些指令会清掉 `VdbeCursor.seekResult`，使 `OPFLAG_USESEEKRESULT` 失效。

### 第十步：临时表路径与触发器

模板 4 的临时表路径在 `sqlite3Insert` 里是这么生成的：

```
srcTab = pParse->nTab++;
regRec = sqlite3GetTempReg(pParse);
regTempRowid = sqlite3GetTempReg(pParse);
sqlite3VdbeAddOp2(v, OP_OpenEphemeral, srcTab, nColumn);
addrL = sqlite3VdbeAddOp1(v, OP_Yield, dest.iSDParm);
sqlite3VdbeAddOp3(v, OP_MakeRecord, regFromSelect, nColumn, regRec);
sqlite3VdbeAddOp2(v, OP_NewRowid, srcTab, regTempRowid);
sqlite3VdbeAddOp3(v, OP_Insert, srcTab, regRec, regTempRowid);
sqlite3VdbeGoto(v, addrL);
```

何时必须用临时表：`if( pTrigger || readsTable(pParse, iDb, pTab) ){ useTempTable = 1; }`。`readsTable`（`src/insert.c` 静态函数）遍历已生成的 VDBE 程序，检查是否存在 `OP_OpenRead` 且 `p3==iDb`、其目标 `p2` 等于目标表或它的任一索引的 `tnum`——即 SELECT 是否会读到目标表本身。

BEFORE / INSTEAD OF 触发器的代码在 `sqlite3Insert` 中由 `sqlite3CodeRowTrigger`（定义在 `src/trigger.c`）生成。源码注释说明一个细节：BEFORE 触发器看到 NEW 行时，若 INTEGER PRIMARY KEY 插入的是 NULL，还不知道最终的 rowid 是多少，因此**用一个 rowid 为 -1 的替身**：`sqlite3VdbeAddOp2(v, OP_Integer, -1, regCols);`。反过来，若插入的是非 NULL 值，则发 `OP_NotNull` 检查，通过就保留原值，为 NULL 才替换成 -1，最后统一发 `OP_MustBeInt` 强制成整数。

生成列在两个位置计算，各有不同基准寄存器：BEFORE 触发器路径用 `sqlite3ComputeGeneratedColumns(pParse, regCols+1, pTab)`（基准是 `NEW.*` 行副本），主路径用 `sqlite3ComputeGeneratedColumns(pParse, regRowid+1, pTab)`（基准是真正的新行）。两次调用的源码注释都写明「必须在计算完 ROWID 之后，因为生成列可能引用 ROWID」。

## 关键取舍

- **源码事实：**`xferOptimization` 的准入条件有十余条，且每条不满足都是 `return 0`。**架构推论：**这层的收益（`OP_RowCell` 整行复制 + `OPFLAG_PREFORMAT` 免去逐列重建记录）建立在「源与目标的行布局完全一致」这一强前提上；一旦源表是视图、带 WHERE、带表达式列或列序不同，整行复制的正确性就不成立。因此准入检查写成一串早退条件而非逐项累加的评分，是为了让每条不满足都能独立否决而不影响其他判定。

- **源码事实：**`bIdListInOrder` 的注释写明「假阴性无害，假阳性会导致数据库损坏」。**架构推论：**这个标志决定「是否要把列值重排到存储顺序」；重排错了不会立刻报错，而是把 A 列的值写进 B 列——所以判定必须偏保守，宁可多一次拷贝也不能错位。列顺序不一致只降性能，顺序判定错则是静默的数据损坏。

- **源码事实：**AUTOINCREMENT 的序列值持久化在 `sqlite_sequence` 表里，读取由 `sqlite3AutoincrementBegin` 在代码生成收尾时统一完成，写回由 `sqlite3AutoincrementEnd` 在语句末尾完成。**架构推论：**「读」与「写」被拆到编译的两个头尾，是因为序列值必须在所有行插入完之后才能确定；而读取放在收尾阶段，可以让同一条语句里多次插入同一张表时共享同一个 `AutoincInfo`（链表查找 `pInfo->pTab!=pTab` 保证了这一点），只读一次序列。

- **源码事实：**AUTOINCREMENT 表在 `OP_NewRowid` 处不允许随机 rowid——`if( pMem->u.i==MAX_ROWID || pC->useRandomRowid ){ rc = SQLITE_FULL; ... }`，而 `src/vdbe.c` 里另有断言 `assert( pOp->p3==0 ); /* We cannot be in random rowid mode if this is an AUTOINCREMENT table. */`。**架构推论：**随机 rowid 只能保证「不与现有行重复」，不能保证「单调递增且不复用」；AUTOINCREMENT 的语义承诺是 rowid 永不复用，随机分配会破坏这个承诺，所以宁可报 `SQLITE_FULL` 也不降级到随机。

- **源码事实：**`sqlite3Insert` 里 BEFORE 触发器的 NEW.rowid 用 -1 替身；源码注释写明「我们还不知道唯一的 ID 会是什么（因为插入还没发生）」。**架构推论：**rowid 在触发器阶段尚未由 B-Tree 分配，且 BEFORE 触发器还可能否决这次插入——真正的 rowid 要到 `OP_Insert` 执行时才确定。用 -1 作为「未知」哨兵，配合 `OP_MustBeInt` 强制整数，是把「尚未确定」这个状态编码进值域的最省事做法。

- **源码事实：**`useSeekResult` 只有在 `isReplace==0 || !sqlite3VdbeHasSubProgram(v)` 时才置位，源码注释指出 REPLACE 命中会执行 `OP_Delete`/`OP_IdxDelete` 从而清掉 `seekResult`。**架构推论：**省一次 seek 的收益只在小写入量场景明显，而 REPLACE 与触发器子程序都会让游标位置失效；条件不成立时多花一次 seek 是安全代价。

- **源码事实：**`sqlite3CompleteInsertion` 对部分索引用 `OP_IsNull` 判断后再决定是否 `OP_IdxInsert`。**架构推论：**部分索引的谓词可能因新行而不成立，此时往索引里写一条不该存在的键，会让查询规划器按索引扫描时读到不满足谓词的行。所以这里不是「写不写」的优化选择，而是正确性要求。

- **源码事实：**`pParse->nested` 为真时 `OP_Insert` 不带 `OPFLAG_NCHANGE` 与 `OPFLAG_LASTROWID`。**架构推论：**嵌套语句指触发器体内执行的语句；它的行数变化与 rowid 不应计入顶层语句的 `sqlite3_changes()` / `sqlite3_last_insert_rowid()` 语义，否则一次 INSERT 触发的多行写入会被算成多行。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/insert.c` | `sqlite3Insert` | INSERT 编译总入口：简化单行 VALUES、定位目标表、鉴权、判定触发器与视图、选择四种模板、分配寄存器、解析 IDLIST、装配列值、计算 rowid、串起约束检查与写入、收尾写回自增序列 |
| `src/insert.c` | `sqlite3CompleteInsertion` | 写入阶段：遍历索引发 `OP_IdxInsert`（部分索引用 `OP_IsNull` 跳过），再对 rowid 表发 `OP_Insert`；置 `OPFLAG_NCHANGE`/`OPFLAG_LASTROWID`/`OPFLAG_APPEND`/`OPFLAG_USESEEKRESULT` |
| `src/insert.c` | `sqlite3AutoincrementBegin` | 遍历 `pParse->pAinc` 链表，为每个 AUTOINCREMENT 表生成 12 条固定指令，扫 `sqlite_sequence` 求出最大 rowid 并加 1 存入寄存器 |
| `src/insert.c` | `autoIncBegin` | 判定表是否为 AUTOINCREMENT（`TF_Autoincrement` 且非 VACUUM），校验 `sqlite_sequence` 形状（普通 rowid 表且恰好两列），登记 `AutoincInfo` 并分配四个寄存器 |
| `src/insert.c` | `autoIncStep` | 发 `OP_MemMax` 推进最大 rowid 寄存器；`memId>0` 才发，非 AUTOINCREMENT 表为 0 |
| `src/insert.c` | `autoIncrementEnd` | 生成 5 条固定指令把序列值写回 `sqlite_sequence`：`OP_Le` 比较、`OP_NewRowid` 取新 rowid、`OP_MakeRecord` 组记录、`OP_Insert`（带 `OPFLAG_APPEND`）、`OP_Close` |
| `src/insert.c` | `sqlite3AutoincrementEnd` | `sqlite3AutoincrementEnd` 的外层包装，`if( pParse->usesAinc ) autoIncrementEnd(pParse);` |
| `src/insert.c` | `xferOptimization` | 转移优化：校验十余条准入条件后生成「`OP_RowCell` 整行复制 + `OP_Insert`」的快速路径，索引层可按 BINARY 排序规则加 `OP_SeekEnd` |
| `src/insert.c` | `readsTable` | 遍历已生成的 VDBE 程序，检查是否有 `OP_OpenRead` 打开了目标表或其索引，决定是否必须用临时表 |
| `src/insert.c` | `sqlite3GenerateConstraintChecks` | 生成 NOT NULL / UNIQUE / CHECK 约束检查代码，并回填 `isReplace`（是否可能触发 REPLACE 冲突解决） |
| `src/insert.c` | `sqlite3ComputeGeneratedColumns` | 计算生成列的值，用两遍算法解生成列间的相互引用 |
| `src/insert.c` | `codeWithoutRowidPreupdate` | 为 WITHOUT ROWID 表的主键索引插入生成 preupdate 钩子调用 |
| `src/build.c` | `sqlite3AutoincrementBegin` 调用点 | 代码生成收尾阶段统一触发自增序列初始化：`if( pParse->usesAinc ) sqlite3AutoincrementBegin(pParse);` |
| `src/build.c` | `sqlite3ColumnExprAuth` | 取列的默认值/生成表达式，并调 `sqlite3FuncAuth` 让授权回调检查其中用到的 SQL 函数 |
| `src/parse.y` | `cmd ::= with insert_cmd(R) INTO xfullname(X) idlist_opt(F) select(S) upsert(U)` | INSERT 语法动作，调用 `sqlite3Insert(pParse, X, S, F, R, U)` |
| `src/parse.y` | `cmd ::= with insert_cmd(R) INTO xfullname(X) idlist_opt(F) DEFAULT VALUES returning` | `DEFAULT VALUES` 语法动作，调用 `sqlite3Insert(pParse, X, 0, F, R, 0)` |
| `src/trigger.c` | `sqlite3CodeRowTrigger` | 生成 BEFORE / INSTEAD OF / AFTER 行级触发器的执行代码 |
| `src/fkey.c` | `sqlite3FkCheck` | 外键检查：`db->flags & SQLITE_ForeignKeys` 打开时由 `sqlite3Insert` 调用 |
| `src/vdbe.c` | `MAX_ROWID` | rowid 上限常量：`SQLITE_32BIT_ROWID` 下为 `0x7fffffff`，否则为 `(i64)( (((u64)0x7fffffff)<<32) | (u64)0xffffffff )`；AUTOINCREMENT 表达到该值时报 `SQLITE_FULL` |
| `src/sqliteInt.h` | `TF_Autoincrement` | 表级标志位，值 `0x00000008`，表示整型主键是自增的 |
| `src/sqliteInt.h` | `COLFLAG_VIRTUAL` / `COLFLAG_STORED` / `COLFLAG_GENERATED` / `COLFLAG_NOINSERT` | 列标志位，值分别是 `0x0020` / `0x0040` / `0x0060` / `0x0062` |
| `src/sqliteInt.h` | `OPFLAG_NCHANGE` / `OPFLAG_LASTROWID` / `OPFLAG_APPEND` / `OPFLAG_USESEEKRESULT` / `OPFLAG_PREFORMAT` / `OPFLAG_SAVEPOSITION` | VDBE 插入标志位，值分别是 `0x01` / `0x20` / `0x08` / `0x10` / `0x80` / `0x02` |
| `src/sqliteInt.h` | `TRIGGER_BEFORE` / `TRIGGER_AFTER` | 触发器时机掩码，值分别是 `1` / `2` |
| `src/sqliteInt.h` | `OE_None` / `OE_Rollback` / `OE_Abort` / `OE_Fail` / `OE_Ignore` / `OE_Replace` / `OE_Default` | 冲突解决策略常量，值分别是 `0` / `1` / `2` / `3` / `4` / `5` / `11` |
| `src/sqliteLimit.h` | `SQLITE_MAX_COLUMN` / `SQLITE_MAX_VARIABLE_NUMBER` | 编译期上限：`SQLITE_MAX_COLUMN` 默认 `2000`（不得超过 `32767`），`SQLITE_MAX_VARIABLE_NUMBER` 默认 `32766` |

## 与主核 / AI 的关系

主核（`engine/db/` 自研多模态数据库内核）的写路径与 SQLite 的 INSERT 编译有可对照的结构：都要先确定「这一行的主键是什么」，再把各列装配成一条记录，最后落盘。差别在于 SQLite 把「主键从哪来」显式分成三条互斥路径（显式值 / `OP_NewRowid` 自动分配 / AUTOINCREMENT 序列），并把序列的持久化完全外包给 `sqlite_sequence` 表；主核若要支持自增，需要自行决定序列存在哪、如何在回滚时保持一致。

AI 侧没有直接对应实现。本页可作为理解「一条 INSERT 编译出的 VDBE 程序长什么样」的材料：一次 INSERT 固定产生「定位表 → 装配列值 → 定 rowid → 算生成列 → 约束检查 → 写索引 → 写行 → 写回序列」这条序列；其中「用哪条路径算 rowid」「要不要用临时表中转」「能不能走转移优化」是 SQLite 用少量布尔与标志位表达策略选择的地方。

## 参考

- [SQLite 官方文档：INSERT 语句](https://sqlite.org/lang_insert.html) —— INSERT 的标准语义、`DEFAULT VALUES`、`ON CONFLICT`（UPSERT）形式
- [SQLite 官方文档：AUTOINCREMENT](https://sqlite.org/autoinc.html) —— AUTOINCREMENT 与 `sqlite_sequence` 表的关系，以及它与普通 rowid 分配在「不复用 rowid」上的差别
- [SQLite 官方文档：WITHOUT ROWID 表](https://sqlite.org/withoutrowid.html) —— WITHOUT ROWID 表把数据直接存在主键索引里，因此没有 `OP_Insert` 这一步
- [SQLite 官方文档：生成列](https://sqlite.org/gencol.html) —— STORED / VIRTUAL 生成列的定义与「不能直接 INSERT 生成列」的限制
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明代码生成器在流水线上的位置
- [SQLite 官方文档：VDBE 指令一览](https://sqlite.org/opcode.html) —— `OP_NewRowid`、`OP_Insert`、`OP_IdxInsert`、`OP_MemMax`、`OP_MakeRecord` 等操作码的语义
- [SQLite 源码：src/insert.c](https://github.com/sqlite/sqlite/blob/master/src/insert.c) —— 本页 `refs` 指向的文件，`sqlite3Insert`、`sqlite3CompleteInsertion`、`sqlite3AutoincrementBegin` 的实现
- [SQLite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c) —— `sqlite3AutoincrementBegin` 的调用点与 `sqlite3ColumnExprAuth`
- [SQLite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— `OP_NewRowid` 的实现与 `MAX_ROWID` 常量
