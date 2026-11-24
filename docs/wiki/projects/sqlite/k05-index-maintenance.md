---
project: sqlite
feature: index-maintenance
page_type: feature
schema_version: 1
refs:
  - path: src/delete.c
    symbols: [sqlite3GenerateIndexKey, sqlite3GenerateRowIndexDelete]
  - path: src/build.c
    symbols: [sqlite3CreateIndex]
---

# 索引维护

## 一句话职责

本页负责 SQLite 中「表数据变化后，索引键值如何同步更新」的代码路径：从索引键的组装（`sqlite3GenerateIndexKey`）、到按索引逐个删除旧键（`sqlite3GenerateRowIndexDelete`）、再到建索引时把整张表回填进索引（`sqlite3CreateIndex` 调用的 `sqlite3RefillIndex`）。索引该被谁使用（查询计划的选择）、索引键在 B-Tree 里怎么落盘，都不在本页范围内。

## 机制是什么

边界（依据条目 `boundary`）：负责在表数据变化后同步维护相关索引的键值；不负责索引的选择（D02）、不负责 B-tree 写入（F03）。

SQLite 的索引维护被拆成三类动作，对应三条不同的代码路径：

| 动作 | 触发场景 | 生成/执行者 | 落点 |
|---|---|---|---|
| 删旧键 | DELETE 行、UPDATE 行、REPLACE 冲突时先删掉冲突行 | `sqlite3GenerateRowIndexDelete` | `src/delete.c` |
| 组装键 | 任何需要「把一行变成某个索引的键」的动作 | `sqlite3GenerateIndexKey` | `src/delete.c` |
| 建索引回填 | `CREATE INDEX` 或 `REINDEX`，把已有表数据灌进新建的索引 | `sqlite3RefillIndex`（由 `sqlite3CreateIndex` 调用） | `src/build.c` |

三者的枢纽是同一个函数 `sqlite3GenerateIndexKey`：`sqlite3GenerateRowIndexDelete` 为每个索引调它生成待删除的键，`sqlite3RefillIndex` 也调它生成待插入的键。

关键的「键」概念：一个索引的键不只有用户写下的索引列，还包括尾部的行定位信息。`src/build.c` 的 `sqlite3CreateIndex` 在建索引时把表主键（或 rowid）追加到索引列末尾——WITHOUT ROWID 表追加的是声明的 PRIMARY KEY（`pPk` 非空分支），普通表追加的是 rowid（`pIndex->aiColumn[i] = XN_ROWID;`，`XN_ROWID` 定义在 `src/sqliteInt.h`，值为 `-1`）。因此索引键的列数 `Index.nColumn` 通常比用户指定的索引列数 `Index.nKeyCol` 多一个。

## 怎么实现的

### 一、`sqlite3GenerateIndexKey`：把一行变成索引键

`src/delete.c` 的 `sqlite3GenerateIndexKey(Parse *pParse, Index *pIdx, int iDataCur, int regOut, int prefixOnly, int *piPartIdxLabel, Index *pPrior, int regPrior)` 的返回值是「存放索引键各列元素的那段寄存器的起始编号」。它的输出形态由三个入参决定：

1. **要不要跳过部分索引（partial index）**：`piPartIdxLabel` 非空且 `pIdx->pPartIdxWhere` 非空时，先 `sqlite3VdbeMakeLabel(pParse)` 造一个标签，再用 `sqlite3ExprIfFalseDup(pParse, pIdx->pPartIdxWhere, *piPartIdxLabel, SQLITE_JUMPIFNULL)` 生成「WHERE 条件不成立就跳过整个索引」的跳转。调用方随后用 `sqlite3ResolvePartIdxLabel` 把这个标签补齐。

   这里有一处必须点名的降级：`pPrior = 0; /* Ticket a9efb42811fa41ee 2019-11-02; ** pPartIdxWhere may have corrupted regPrior registers */`——即**遇到部分索引时，前面索引的寄存器复用缓存被主动作废**，因为 `pPartIdxWhere` 的求值可能破坏先前算好的寄存器内容。

2. **键取几列**：

   ```
   nCol = (prefixOnly && pIdx->uniqNotNull) ? pIdx->nKeyCol : pIdx->nColumn;
   ```

   即：只有「只取前缀」且该索引是「唯一且 NOT NULL」时，才只取 `nKeyCol` 列（用户指定的索引列），不取尾部的 rowid；其余情况一律取全部 `nColumn` 列（含 rowid）。`uniqNotNull` 在 `src/build.c` 的 `sqlite3CreateIndex` 里由 `pIndex->uniqNotNull = onError!=OE_None;` 初始化，遇到可空列或表达式索引会被清零（`pIndex->uniqNotNull = 0;`）。

3. **寄存器复用缓存**：`pPrior` 指向「刚算过键的上一个索引」，`regPrior` 是它的寄存器段起点。若 `regBase!=regPrior` 或 `pPrior->pPartIdxWhere` 非空，则 `pPrior = 0` 放弃复用。复用条件是同列下标相同且不是表达式列：

   ```
   if( pPrior
    && pPrior->aiColumn[j]==pIdx->aiColumn[j]
    && pPrior->aiColumn[j]!=XN_EXPR
   ){
     continue;   /* 该列已由上一个索引算过，跳过装载 */
   }
   ```

   `XN_EXPR` 定义在 `src/sqliteInt.h`，值为 `-2`。表达式列不能复用，因为每列的表达式可能不同。

逐列装载用 `sqlite3ExprCodeLoadIndexColumn(pParse, pIdx, iDataCur, j, regBase+j)`（定义在 `src/expr.c`）：`aiColumn[j]==XN_EXPR` 时走 `sqlite3ExprCodeCopy` 编译表达式；否则走 `sqlite3ExprCodeGetColumnOfTable` 直接取表列。

装载完还有一处修正：若该列不是虚拟列（`(pIdx->pTable->aCol[iTabCol].colFlags & COLFLAG_VIRTUAL)==0`），就调 `sqlite3VdbeDeletePriorOpcode(v, OP_RealAffinity)` 把刚生成的 `OP_RealAffinity` 删掉。源码注释解释：REAL 亲和性列在表里以整数紧凑存储、取出时转成 REAL，但要写回索引时应保持整数形式。

最后 `regOut` 非零时用 `sqlite3VdbeAddOp3(v, OP_MakeRecord, regBase, nCol, regOut)` 把整段寄存器组装成一条记录，函数返回 `regBase`。返回前 `sqlite3ReleaseTempRange(pParse, regBase, nCol)` 释放临时寄存器段——所以**返回的 `regBase` 在函数返回后已不再持有有效数据**，调用方只能拿它当下标用。

### 二、`sqlite3GenerateRowIndexDelete`：逐索引删旧键

`src/delete.c` 的 `sqlite3GenerateRowIndexDelete(Parse *pParse, Table *pTab, int iDataCur, int iIdxCur, int *aRegIdx, int iIdxNoSeek)` 遍历 `pTab->pIndex` 链表，为每个索引生成一条 `OP_IdxDelete`。

前置条件（源码注释写明）：`iDataCur` 是打开在表规范存储 B-Tree 上的读写游标；`iIdxCur+i` 是第 i 个索引的读写游标；`iDataCur` 必须已经定位在待删的行上。

遍历循环里有三道跳过：

```
if( aRegIdx!=0 && aRegIdx[i]==0 ) continue;   /* 本次写入不涉及该索引 */
if( pIdx==pPk ) continue;                       /* WITHOUT ROWID 的主键索引不删 */
if( iIdxCur+i==iIdxNoSeek ) continue;           /* 该游标已由别处处理 */
```

`pPk` 的取法：`pPk = HasRowid(pTab) ? 0 : sqlite3PrimaryKeyIndex(pTab);`——rowid 表没有主键索引，WITHOUT ROWID 表的主键索引是表本身的存储，跳过。

对每个未跳过的索引：

```
r1 = sqlite3GenerateIndexKey(pParse, pIdx, iDataCur, 0, 1,
    &iPartIdxLabel, pPrior, r1);
```

注意 `regOut` 传 `0`（不生成 `OP_MakeRecord`）、`prefixOnly` 传 `1`。`prefixOnly=1` 的效果在上面第 2 点已经说明：唯一且 NOT NULL 的索引只取 `nKeyCol` 列，其余取全部 `nColumn` 列。

随后：

```
if( pIdx->bHasExpr && aRegIdx ){
  p3 = aRegIdx[i];
}
sqlite3VdbeAddOp3(v, OP_IdxDelete, iIdxCur+i, r1, p3);
sqlite3VdbeChangeP4(v, -1, (const char*)pIdx, P4_INDEX);
sqlite3VdbeChangeP5(v, pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn);
sqlite3ResolvePartIdxLabel(pParse, iPartIdxLabel);
pPrior = pIdx;
```

`OP_IdxDelete` 的参数含义见 `src/vdbe.c` 的注释：P2 起始的 P5 个寄存器构成未打包的索引键；P4 是 `Index` 结构指针；**P3 非零时是「待写入的新记录」所在寄存器，若该记录与待删除的记录相同，则跳过删除并把 P3 寄存器置 NULL**。P5 是参与比较的键列数，取值规则与 `sqlite3GenerateIndexKey` 一致。

`pPrior`/`r1` 在循环中一路传递，形成跨索引的寄存器复用链。

`OP_IdxDelete` 在 `src/vdbe.c` 的执行体里：先 `sqlite3BtreeIndexMoveto` 定位；若 `res!=0`（没找到匹配项），走 `sqlite3VdbeFindIndexKey` 再找一次；仍找不到就报 `SQLITE_CORRUPT_INDEX`（消息 `"index corruption"`），但**在可写模式（`sqlite3WritableSchema(db)`）下不报错**，只把游标记为 `CACHE_STALE` 并跳过。找到后 `sqlite3BtreeDelete(pCrsr, BTREE_AUXDELETE)` 删除。

### 三、三个调用方

`sqlite3GenerateRowIndexDelete` 有三个真实调用方：

1. **`src/delete.c` 的 `sqlite3GenerateRowDelete`**（DELETE 语句编译）：`sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, 0, iIdxNoSeek);`——`aRegIdx` 传 0（不跳过任何索引），`iIdxNoSeek` 传本轮的 `iIdxNoSeek`（若 BEFORE 触发器改动了游标位置，这里被置为 `-1`，源码注释写明「Also disable the iIdxNoSeek optimization since the BEFORE trigger may have moved that cursor」）。调用点在 `OP_Delete` 之前。

2. **`src/update.c` 的 UPDATE 编译路径**：`sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, aRegIdx, -1);`——`aRegIdx` 传 UPDATE 的每索引寄存器数组（值为 0 的索引跳过），`iIdxNoSeek` 传 `-1`（不做 NoSeek 优化）。

3. **`src/insert.c` 的 REPLACE 冲突处理**：`sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, 0, -1);`——位于 `OE_Replace` 分支中，源码注释说明这一步「removes the index b-tree entries only. The table b-tree entry will be replaced by the new entry when it is inserted.」。调用前有 `if( pTab->pIndex ){ sqlite3MultiWrite(pParse); ... }` 的守卫——表上没有索引时连 `sqlite3MultiWrite` 都不发，这样 `REPLACE INTO t(rowid) VALUES($newrowid)` 这类语句不需要 statement journal。

反过来，**写入新键**由 `src/insert.c` 的 `sqlite3CompleteInsertion` 负责，它遍历 `pTab->pIndex` 为每个索引发一条 `OP_IdxInsert`：

```
sqlite3VdbeAddOp4Int(v, OP_IdxInsert, iIdxCur+i, aRegIdx[i],
                     aRegIdx[i]+1,
                     pIdx->uniqNotNull ? pIdx->nKeyCol: pIdx->nColumn);
```

部分索引与表达式索引更新时，先发 `OP_IsNull aRegIdx[i], <下一条>` 跳过——源码注释：记录寄存器被置 NULL 表示「这一行不进这个索引」。

`OP_IdxInsert` 在 `src/vdbe.c` 的执行体里调 `sqlite3BtreeInsert`，P5 的 `OPFLAG_APPEND`/`OPFLAG_SAVEPOSITION`/`OPFLAG_PREFORMAT` 决定插入策略，`OPFLAG_USESEEKRESULT` 时复用游标上一次的定位结果。

### 四、`sqlite3CreateIndex`：建索引与回填

`src/build.c` 的 `sqlite3CreateIndex` 是建索引的入口，参数含义：`pName1`/`pName2` 是索引名（主键或 UNIQUE 约束隐含的索引传 NULL），`pTblName` 是被索引的表（`NULL` 表示用 `pParse->pNewTable`），`pList` 是索引列表达式列表，`onError` 是冲突策略（`OE_Abort`/`OE_Ignore`/`OE_Replace`/`OE_None`），`pPIWhere` 是部分索引的 WHERE 子句，`idxType` 是索引类型（`SQLITE_IDXTYPE_APPDEF`=0、`SQLITE_IDXTYPE_UNIQUE`=1、`SQLITE_IDXTYPE_PRIMARYKEY`=2、`SQLITE_IDXTYPE_IPK`=3，定义在 `src/sqliteInt.h`）。

流程分几步：

**第一步：各种早期拒绝。** `pParse->nErr` 非零、`IN_DECLARE_VTAB` 且 `idxType!=SQLITE_IDXTYPE_PRIMARYKEY`、`sqlite3ReadSchema` 失败、`sqlite3HasExplicitNulls` 检出显式 NULLS FIRST/LAST，都直接 `goto exit_create_index`。表名以 `sqlite_` 开头报 `"table %s may not be indexed"`；视图报 `"views may not be indexed"`；虚拟表报 `"virtual tables may not be indexed"`。

**第二步：索引命名。** 显式名走 `sqlite3CheckObjectName` 校验；同名表报 `"there is already a table named %s"`，同名索引报 `"index %s already exists"`（`ifNotExist` 为真时不报错，改调 `sqlite3CodeVerifySchema` 与 `sqlite3ForceNotReadOnly`）。隐式索引（主键/UNIQUE 约束）自动命名：

```
for(pLoop=pTab->pIndex, n=1; pLoop; pLoop=pLoop->pNext, n++){}
zName = sqlite3MPrintf(db, "sqlite_autoindex_%s_%d", pTab->zName, n);
```

即 `sqlite_autoindex_<表名>_<序号>`，序号从 1 开始按已有索引个数递增。

**第三步：分配 `Index` 对象。** `sqlite3AllocateIndexObject(db, pList->nExpr + nExtraCol, nName + nExtra + 1, &zExtra)`，其中 `nExtraCol = pPk ? pPk->nKeyCol : 1`——WITHOUT ROWID 表为索引预留主键列数，普通表预留 1 列（rowid）。分配后 `pIndex->nKeyCol = pList->nExpr;`（用户指定的索引列数），`pIndex->nColumn` 在 `sqlite3AllocateIndexObject` 里先被设为 `nCol`（含预留列），之后建索引过程会调整。

**第四步：逐列解析索引项。** 对每个索引表达式：

- 普通列：`j = pCExpr->iColumn;`，写进 `pIndex->aiColumn[i]`；`j<0`（即 rowid 引用）时用 `pTab->iPKey`。
- 表达式（非 `TK_COLUMN`）：`pIndex->aiColumn[i] = XN_EXPR;`、`pIndex->uniqNotNull = 0;`、`pIndex->bHasExpr = 1;`。在 `CREATE TABLE` 上下文（`pTab==pParse->pNewTable`）里表达式被禁止，报 `"expressions prohibited in PRIMARY KEY and UNIQUE constraints"`。
- 可空列会把 `pIndex->uniqNotNull` 清零：`if( pTab->aCol[j].notNull==0 ){ pIndex->uniqNotNull = 0; }`。
- 虚拟列（`COLFLAG_VIRTUAL`）置 `pIndex->bHasVCol = 1; pIndex->bHasExpr = 1;`。
- 排序序列：显式 `COLLATE` 用其名字，否则用 `sqlite3ColumnColl(&pTab->aCol[j])`，都没有则 `zColl = sqlite3StrBINARY`；`sqlite3LocateCollSeq` 找不到该排序序列则报错退出。
- 排序方向：`sortOrderMask` 由文件格式决定——`pDb->pSchema->file_format>=4` 时 `sortOrderMask = -1`（尊重 DESC），否则 `sortOrderMask = 0`（忽略 DESC）。

**第五步：追加表主键/rowid。** WITHOUT ROWID 表把主键索引的各列追加进去，若某列与已有索引列重复（`isDupColumn`）则 `pIndex->nColumn--`；普通表追加 `XN_ROWID`，排序序列用 `sqlite3StrBINARY`。

**第六步：统计信息与覆盖索引标记。** `sqlite3DefaultRowEst(pIndex)` 填默认行数估计，`estimateIndexWidth(pIndex)` 估宽度（仅在 `pParse->pNewTable==0` 时，即不是 CREATE TABLE 内建的隐式索引）。覆盖索引判定：`if( pTblName!=0 && pIndex->nColumn>=pTab->nCol )` 再逐列检查是否都被索引到，是则 `pIndex->isCovering = 1`。

**第七步：隐式索引去重。** `pTab==pParse->pNewTable` 时（即 `CREATE TABLE t(x PRIMARY KEY, y)` 或 `CREATE TABLE t(x, y, UNIQUE(x, y))` 这类隐式索引），遍历已有 `pTab->pIndex`，比较 `nKeyCol`、各列 `aiColumn`、排序序列 `azColl`（`sqlite3StrICmp` 比较），完全等价则**不建新索引**，直接复用已有的那个；两者 `onError` 不同时若都是显式指定则报 `"conflicting ON CONFLICT clauses specified"`。

**第八步：真正落地。** 显式 `CREATE INDEX` 的路径：

```
pIndex->tnum = (Pgno)sqlite3VdbeAddOp0(v, OP_Noop);
sqlite3VdbeAddOp3(v, OP_CreateBtree, iDb, iMem, BTREE_BLOBKEY);
```

先用一条 `OP_Noop` 占位记下 `tnum`，源码注释解释这是为了「如果这个索引其实是主键且表是 WITHOUT ROWID 表，`convertToWithoutRowidTable()` 会把这条 Noop 换成一条 Goto，跳过下面生成的 VDBE 代码」。接着用 `OP_CreateBtree` 建索引的 B-Tree 根页（`BTREE_BLOBKEY` 标志），然后 `sqlite3NestedParse` 往 `sqlite_schema` 插一条 `'index'` 记录。

**第九步：回填索引内容。**

```
if( pTblName ){
  sqlite3RefillIndex(pParse, pIndex, iMem);
  sqlite3ChangeCookie(pParse, iDb);
  sqlite3VdbeAddParseSchemaOp(v, iDb, ..., 0);
  sqlite3VdbeAddOp2(v, OP_Expire, 0, 1);
}
```

`sqlite3RefillIndex`（`src/build.c` 的静态函数）的动作：

1. 授权检查 `sqlite3AuthCheck(pParse, SQLITE_REINDEX, ...)`，不通过直接返回。
2. `sqlite3TableLock(pParse, iDb, pTab->tnum, 1, pTab->zName)` 取表写锁。
3. 开排序器：`sqlite3VdbeAddOp4(v, OP_SorterOpen, iSorter, 0, pIndex->nKeyCol, ...)`——排序器按 `nKeyCol` 列排序。
4. 扫全表进排序器：`sqlite3OpenTable(pParse, iTab, iDb, pTab, OP_OpenRead)` → `OP_Rewind` → 每行调 `sqlite3GenerateIndexKey(pParse, pIndex, iTab, regRecord, 0, &iPartIdxLabel, 0, 0)` 生成键 → `OP_SorterInsert` → `sqlite3ResolvePartIdxLabel` → `OP_Next`。
5. `if( memRootPage<0 ) sqlite3VdbeAddOp2(v, OP_Clear, tnum, iDb);`——`memRootPage` 负值表示索引已存在（REINDEX 场景），必须先 `OP_Clear` 清空再重灌；新建索引时根页刚由 `OP_CreateBtree` 建成，是空的。
6. `OP_OpenWrite` 打开索引，`OPFLAG_BULKCSR`（新建时另有 `OPFLAG_P2ISREG`）。
7. `OP_SorterSort` 把排好序的键吐出来；若是唯一索引（`IsUniqueIndex(pIndex)`），先 `OP_SorterCompare` 比相邻键，相同则 `sqlite3UniqueConstraint(pParse, OE_Abort, pIndex)` 报错。
8. `OP_SorterData` 取数据 → `OP_SeekEnd`（`pIndex->bAscKeyBug` 为真时跳过，源码注释说明该优化对「WITHOUT ROWID 表上带 DESC 主键的唯一索引」不成立）→ `OP_IdxInsert`（带 `OPFLAG_USESEEKRESULT`）→ `OP_SorterNext` 循环。
9. 收尾 `OP_Close` 三个游标。

非唯一索引走另一支：只调 `sqlite3MayAbort(pParse)`，源码注释说明理由——「Most CREATE INDEX and REINDEX statements that are not UNIQUE can not abort」，唯一的例外是索引表达式里含会抛异常的用户函数，但源码认为 statement journal 的开销很小，所以对所有 CREATE INDEX 都调 `sqlite3MayAbort`。

**第十步：链表排序不变量。** `exit_create_index` 标签处有一段链表重排，把所有 `onError==OE_Replace` 的索引挪到 `pTab->pIndex` 链表末尾（源码注释：进入本函数时链表已有序，此刻至多只有一个新索引位置不对，所以最多重排一个）。`SQLITE_DEBUG` 下还有断言守住这个不变量。

### 五、`sqlite3CreateIndex` 的三个调用方

- `src/build.c` 的 `sqlite3AddPrimaryKey`：`sqlite3CreateIndex(pParse, 0, 0, 0, pList, onError, 0, 0, sortOrder, 0, SQLITE_IDXTYPE_PRIMARYKEY);`——为非 INTEGER、非单列的主键建 `SQLITE_IDXTYPE_PRIMARYKEY` 索引。
- `src/build.c` 的 `sqlite3AddUniqueConstraint`（约在 `sqlite3CreateIndex` 定义的第二处调用点）：`sqlite3CreateIndex(pParse, 0, 0, 0, pList, pTab->keyConf, 0, 0, 0, 0, SQLITE_IDXTYPE_UNIQUE);`——为 UNIQUE 约束建 `SQLITE_IDXTYPE_UNIQUE` 索引。
- 显式 `CREATE INDEX` 语句的解析动作（`src/parse.y`），传入显式名与列列表。

## 关键取舍

- **源码事实：**`sqlite3GenerateIndexKey` 在遇到部分索引时把 `pPrior` 强制置 0（源码注释挂着 Ticket `a9efb42811fa41ee`，2019-11-02）。**架构推论：**部分索引的 WHERE 谓词求值会占用并可能破坏寄存器，跨索引复用寄存器的正确性依赖「前一个索引的键仍完好」。放弃复用是保守选择，代价是部分索引表上的多索引 DELETE 会重复装载列值，换来的是不需要为「哪些指令会破坏寄存器」做精细分析。

- **源码事实：**`sqlite3GenerateIndexKey` 删掉 `OP_RealAffinity` 的前提是「该列不是虚拟列」。**架构推论：**REAL 亲和性列在表里用整数紧凑表示，取出时转 REAL，写进索引时又要保持整数形式——B-Tree 的键比较依赖存储形式一致，转换一次就够。虚拟列没有「表里的紧凑表示」这回事，所以不做这个修正。

- **源码事实：**删除路径用 `OP_IdxDelete` 而不是「定位 + `OP_Delete`」，且 `OP_IdxDelete` 找不到匹配项时报 `SQLITE_CORRUPT_INDEX`，但 `sqlite3WritableSchema(db)` 为真时只跳过不报错。**架构推论：**索引是冗余结构，正常情况下表与索引必须一致；不一致意味着文件损坏。可写模式是「用户明确表示知道自己在干什么」的逃生口，此时报错会挡住用户用工具修库，所以降级为跳过并把游标记为 `CACHE_STALE`（强制下次重读）。

- **源码事实：**`OP_IdxDelete` 支持 P3 寄存器，若待写入的新记录与待删除的记录完全相同则跳过删除并把 P3 置 NULL。**架构推论：**UPDATE 时若索引键没变（改的是未被索引的列），删旧键再插新键是纯粹的浪费。P3 机制让「键未变则原地跳过」在 VDBE 层完成，不需要上层做额外比较。

- **源码事实：**`sqlite3CreateIndex` 用「已存在的等价隐式索引」去重——比较 `nKeyCol`、每列 `aiColumn`、每列排序序列（`sqlite3StrICmp`），三项全同则不建新索引。**架构推论：**排序序列不同或列序不同会让 B-Tree 键的排序结果不同，是两个不同的索引，不能合并。排序序列相同但大小写不同（`sqlite3StrICmp`）视为同一个，因为 SQLite 的排序序列名本身大小写不敏感。

- **源码事实：**`sqlite3CreateIndex` 把 `pIndex->tnum` 先占位成一条 `OP_Noop` 的地址，源码注释说明这是为了让 `convertToWithoutRowidTable()` 能把它换成 `Goto` 跳过整段建索引代码。**架构推论：**WITHOUT ROWID 表的主键索引就是表本身，不该再单独建一棵 B-Tree。把「回填代码」做成可被跳过的独立块，而不是靠一堆 `if` 散落在流程中，是为了让这个改写只需替换一条指令。

- **源码事实：**`sqlite3RefillIndex` 用排序器（sorter）而不是直接往索引 B-Tree 里插。**架构推论：**索引键的物理顺序必须与索引的排序规则一致，而表的物理顺序与索引顺序无关。排序器承担了「把无序的表行转成有序的索引键流」这一步；代价是 CREATE INDEX 需要额外的临时空间与两遍扫描（先扫表进排序器，再从排序器灌进索引）。好处是 UNIQUE 冲突能在排序后的相邻键之间一次性检出（`OP_SorterCompare`），而不是在 B-Tree 里逐条比对。

- **源码事实：**`sqlite3RefillIndex` 在 `memRootPage<0`（REINDEX，索引已存在）时才发 `OP_Clear` 清空索引；新建索引时根页刚由 `OP_CreateBtree` 建成本来就是空的。**架构推论：**清空动作必须与「索引是否已有内容」严格对应，多发一次 `OP_Clear` 会把刚建好的空索引再清一遍（虽然结果相同，但多一次写事务），少发一次则会让 REINDEX 在旧索引内容上叠加新内容。

- **源码事实：**`sqlite3CreateIndex` 在 `pParse->pNewTable==0`（显式 CREATE INDEX）时才调 `estimateIndexWidth`，隐式索引不调。**架构推论：**隐式索引是 CREATE TABLE 的一部分，表本身还没有数据（`pParse->pNewTable` 阶段表还是空的），宽度估计没有意义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/delete.c` | `sqlite3GenerateIndexKey` | 把表的一行组装成某个索引的键：处理部分索引的跳过标签（`SQLITE_JUMPIFNULL`）、决定键列数（`prefixOnly && uniqNotNull ? nKeyCol : nColumn`）、跨索引复用寄存器（`pPrior`/`regPrior`）、删除多余的 `OP_RealAffinity`、用 `OP_MakeRecord` 组装并返回寄存器段起点 |
| `src/delete.c` | `sqlite3GenerateRowIndexDelete` | 遍历 `pTab->pIndex` 为每个索引生成一条 `OP_IdxDelete`；三道跳过条件：`aRegIdx[i]==0`、`pIdx==pPk`、`iIdxCur+i==iIdxNoSeek`；设置 P5 为 `uniqNotNull ? nKeyCol : nColumn` |
| `src/delete.c` | `sqlite3ResolvePartIdxLabel` | 补齐部分索引的跳过标签（`iLabel` 为 0 时是空操作） |
| `src/delete.c` | `sqlite3GenerateRowDelete` | DELETE 编译路径中调用 `sqlite3GenerateRowIndexDelete` 的地方；在 BEFORE 触发器改动过游标后把 `iIdxNoSeek` 置 `-1` 关闭 NoSeek 优化 |
| `src/build.c` | `sqlite3CreateIndex` | 建索引入口：早期拒绝与命名（`sqlite_autoindex_<表名>_<序号>`）、分配 `Index` 对象、逐列解析（`XN_EXPR`/`XN_ROWID`、排序序列、`uniqNotNull`）、追加表主键、覆盖索引判定、隐式索引去重、发 `OP_CreateBtree` 与 `sqlite_schema` 记录、调 `sqlite3RefillIndex` 回填、退出时把 `OE_Replace` 索引重排到链表末尾 |
| `src/build.c` | `sqlite3RefillIndex` | 索引回填：取表写锁、开排序器、扫全表生成键进排序器、必要时 `OP_Clear` 清空旧索引、`OP_SorterSort` 后逐条 `OP_IdxInsert`；唯一索引用 `OP_SorterCompare` 检冲突 |
| `src/build.c` | `sqlite3AllocateIndexObject` | 分配 `Index` 对象与 `azColl`/`aiRowLogEst`/`aiColumn`/`aSortOrder` 数组；`nColumn` 设为含预留列的总列数，`nKeyCol` 设为 `nCol-1` |
| `src/build.c` | `sqlite3DefaultRowEst` | 填默认行数估计：首项取表的 `nRowLogEst`（不足 1000 时抬到 LogEst 99），部分索引再减 10；后续项取 `aVal[] = { 33, 32, 30, 28, 26 }`，再往后统一为 23（LogEst 5） |
| `src/expr.c` | `sqlite3ExprCodeLoadIndexColumn` | 装载索引第 i 列的值：`XN_EXPR` 走 `sqlite3ExprCodeCopy` 编译表达式，否则走 `sqlite3ExprCodeGetColumnOfTable` 取表列 |
| `src/insert.c` | `sqlite3CompleteInsertion` | 写入新索引键：逐索引发 `OP_IdxInsert`，P4 为 `uniqNotNull ? nKeyCol : nColumn`；部分索引/表达式索引更新时先发 `OP_IsNull` 跳过 |
| `src/insert.c` | `sqlite3GenerateConstraintChecks` | REPLACE 冲突分支里调用 `sqlite3GenerateRowIndexDelete` 删旧索引项；调用前用 `if( pTab->pIndex )` 守卫决定是否发 `sqlite3MultiWrite` |
| `src/update.c` | UPDATE 编译路径 | `sqlite3GenerateRowIndexDelete(pParse, pTab, iDataCur, iIdxCur, aRegIdx, -1)`——先删旧索引项再写新行 |
| `src/vdbe.c` | `OP_IdxInsert` | 执行体：`sqlite3BtreeInsert` 写入索引 B-Tree；P5 的 `OPFLAG_APPEND`/`OPFLAG_SAVEPOSITION`/`OPFLAG_PREFORMAT` 决定插入策略，`OPFLAG_USESEEKRESULT` 复用上次定位结果 |
| `src/vdbe.c` | `OP_IdxDelete` | 执行体：`sqlite3BtreeIndexMoveto` 定位，未命中则 `sqlite3VdbeFindIndexKey` 再找，仍未命中报 `SQLITE_CORRUPT_INDEX`（可写模式下降级为跳过）；P3 记录与待删记录相同时跳过删除并把 P3 置 NULL |
| `src/prepare.c` | `sqlite3IndexHasDuplicateRootPage` | 检查同表兄弟索引是否共用同一根页；共用说明模式损坏，`sqlite3CreateIndex` 据此报 `"invalid rootpage"` 并设 `SQLITE_CORRUPT_BKPT` |
| `src/sqliteInt.h` | `XN_ROWID` / `XN_EXPR` | 索引列的特殊下标，值分别是 `-1`（rowid）与 `-2`（表达式） |
| `src/sqliteInt.h` | `SQLITE_IDXTYPE_APPDEF` / `SQLITE_IDXTYPE_UNIQUE` / `SQLITE_IDXTYPE_PRIMARYKEY` / `SQLITE_IDXTYPE_IPK` | 索引类型标记，值分别是 `0` / `1` / `2` / `3` |
| `src/sqliteInt.h` | `COLFLAG_VIRTUAL` | 列标志；带此标志的列被索引时置 `Index.bHasVCol` 与 `Index.bHasExpr`，并使 `sqlite3GenerateIndexKey` 跳过 `OP_RealAffinity` 修正 |

## 与主核 / AI 的关系

主核（`engine/db/` 自研多模态数据库内核）在写路径上面对的是同一件事：表数据变了，所有派生结构（索引、向量索引、倒排）都要跟着变。SQLite 在这里给出的可对照样本有三条：

其一，**索引键的组装被单独抽成一个函数**（`sqlite3GenerateIndexKey`），所有需要「一行 → 索引键」的场景都调它：删旧键用它，建索引回填用它。**架构推论：**主核若有多种派生索引（结构化列索引 + 向量索引 + 倒排索引），把「从一行提取各类派生键」也抽成一层，删除路径与重建路径（REINDEX）才能复用同一套提取逻辑，避免两条路径的键定义悄悄漂移。

其二，**「索引 = 用户列 + 行定位列」这个模型是显式的**：建索引时把 rowid（或 WITHOUT ROWID 表的主键）追加到索引列末尾，删除时按键的列数 `nColumn` 而非 `nKeyCol` 比较。**架构推论：**主核若把向量索引的定位信息（向量 ID）与结构化索引分开存放，删除时需要同时按两者定位，代价是两条删除路径必须保持严格一致；SQLite 选择把定位列并入同一个键，让删除只需要一次 B-Tree 定位。

其三，**重建索引走「排序器 + 批量插入」而不是「逐行插入」**（`sqlite3RefillIndex`）。**架构推论：**主核若需要支持「重建向量索引」这类运维操作，逐行插入会退化成 O(n log n) 次随机写且页分裂严重；排序器方案用额外临时空间换取顺序写。同样的问题在主核里以「重建 HNSW 索引」的形式出现——HNSW 本身的建图过程就要求按某种顺序插入以控制图的质量。

AI 侧没有直接对应实现，但本页可作为理解「一条 DELETE 编译出的 VDBE 程序里，索引维护占多大篇幅」的材料：索引维护的全部内容就是「为每个索引生成一条 `OP_IdxDelete`」，指令形状极简；复杂之处全在**编译期**——哪些索引要删、键取几列、寄存器能否复用，都是生成代码时静态决定的。这也提示 AI 生成的存储引擎代码里，索引维护常被写成「遍历索引列表，逐个重建键」的热路径代码，而 SQLite 的做法是把跨索引的键提取优化（`pPrior`/`regPrior` 复用）做在代码生成阶段，运行期零开销。

## 参考

- [SQLite 官方文档：CREATE INDEX](https://sqlite.org/lang_createindex.html) —— 显式索引的语法、部分索引（partial index）的 WHERE 子句语义
- [SQLite 官方文档：UNIQUE 约束](https://sqlite.org/uniqueidx.html) —— 隐式索引（`sqlite_autoindex_*`）的由来与 UNIQUE 约束的实现方式
- [SQLite 官方文档：WITHOUT ROWID 表](https://sqlite.org/withoutrowid.html) —— WITHOUT ROWID 表把 PRIMARY KEY 直接作为表存储，因此主键索引不参与索引维护
- [SQLite 官方文档：REINDEX](https://sqlite.org/lang_reindex.html) —— REINDEX 触发的索引整体重建，对应 `sqlite3RefillIndex` 中 `memRootPage<0` 的分支
- [SQLite 官方文档：部分索引](https://sqlite.org/partialindex.html) —— 部分索引的 WHERE 谓词与查询计划器对它的利用
- [SQLite 官方文档：VDBE 指令一览](https://sqlite.org/opcode.html) —— `OP_IdxInsert`、`OP_IdxDelete`、`OP_MakeRecord`、`OP_SorterInsert`、`OP_SorterSort`、`OP_SorterCompare`、`OP_Clear` 等操作码的语义
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明代码生成器与索引维护在流水线上的位置
- [SQLite 源码：src/delete.c](https://github.com/sqlite/sqlite/blob/master/src/delete.c) —— 本页 `refs` 指向的文件，`sqlite3GenerateIndexKey` 与 `sqlite3GenerateRowIndexDelete` 的实现
- [SQLite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c) —— 本页 `refs` 指向的文件，`sqlite3CreateIndex` 与 `sqlite3RefillIndex` 的实现
- [SQLite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— `OP_IdxInsert` 与 `OP_IdxDelete` 的执行体
