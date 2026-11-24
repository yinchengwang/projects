---
project: sqlite
feature: constraint-checks
page_type: feature
schema_version: 1
refs:
  - path: src/insert.c
    symbols: [sqlite3GenerateConstraintChecks]
  - path: src/build.c
    symbols: [sqlite3AddNotNull, sqlite3AddCheckConstraint, sqlite3AddPrimaryKey]
---

# 约束检查

## 一句话职责

本页负责 SQLite 在写入路径上「生成并执行约束检查」的代码：主键（rowid / INTEGER PRIMARY KEY / WITHOUT ROWID 表的主键索引）、唯一约束、NOT NULL、CHECK 四类约束的检查代码在哪里生成、约束冲突时按哪种冲突解决策略处置、以及这些检查在 DDL 阶段是如何被登记进模式对象的。索引键的实际存储（B-Tree 与索引页）、外键约束检查、触发器分别由别的小组负责，不在本页范围内。

## 机制是什么

边界（依据条目 `boundary`）：负责在写入时生成并执行约束检查（主键、唯一、非空、CHECK）的代码路径；不负责索引的实际存储（F 组）、不负责外键（k02）。

SQLite 的约束检查被拆成两半：

| 阶段 | 做的事 | 落点 |
|---|---|---|
| DDL 阶段（`CREATE TABLE` 解析时） | 把约束登记进 `Table` / `Column` / `Index` 对象：NOT NULL 记到列上、CHECK 记到 `Table.pCheck` 表达式列表、主键要么变成 `Table.iPKey` 要么变成一个 `SQLITE_IDXTYPE_PRIMARYKEY` 索引 | `src/build.c` 的 `sqlite3AddNotNull`、`sqlite3AddCheckConstraint`、`sqlite3AddPrimaryKey` |
| 写入阶段（`INSERT` / `UPDATE` 编译时） | 为这些约束生成 VDBE 指令：读寄存器、比较、跳转、报错 | `src/insert.c` 的 `sqlite3GenerateConstraintChecks` |

登记入口在 `src/parse.y` 的 `ccons` / `tcons` 规则上：

- `ccons ::= NOT NULL onconf(R).` → `sqlite3AddNotNull(pParse, R);`
- `ccons ::= PRIMARY KEY sortorder(Z) onconf(R) autoinc(I).` → `sqlite3AddPrimaryKey(pParse, 0, R, I, Z);`
- `tcons ::= PRIMARY KEY LP sortlist(X) autoinc(I) RP onconf(R).` → `sqlite3AddPrimaryKey(pParse, X, R, I, 0);`
- `ccons ::= CHECK LP(A) expr(X) RP(B).` 与 `tcons ::= CHECK LP(A) expr(E) RP(B) onconf.` → `sqlite3AddCheckConstraint(pParse, X/E, A.z, B.z);`

冲突解决策略是整页的核心常量，定义在 `src/sqliteInt.h`：

| 常量 | 值 | 语义（源码注释） |
|---|---|---|
| `OE_None` | `0` | 没有约束要检查 |
| `OE_Rollback` | `1` | 失败并回滚事务 |
| `OE_Abort` | `2` | 撤销本条命令的改动，但不回滚整个事务 |
| `OE_Fail` | `3` | 停止本条操作，保留此前所有改动 |
| `OE_Ignore` | `4` | 忽略错误，不做这次 INSERT / UPDATE |
| `OE_Replace` | `5` | 删掉已有记录，再做 INSERT / UPDATE |
| `OE_Update` | `6` | 按 UPSERT 的 DO UPDATE 处理 |
| `OE_Default` | `11` | 用该约束自己的默认动作 |

`onconf`（列级/表级 `ON CONFLICT` 子句）在 `src/parse.y` 里由 `resolvetype` 映射到这些常量：`resolvetype(A) ::= raisetype(A).` 覆盖 `ROLLBACK`/`ABORT`/`FAIL`（`raisetype` 三条规则分别给出 `OE_Rollback`/`OE_Abort`/`OE_Fail`），`resolvetype(A) ::= IGNORE.` 给出 `OE_Ignore`，`resolvetype(A) ::= REPLACE.` 给出 `OE_Replace`；`onconf(A) ::= .` 给出 `OE_Default`。

写入阶段的入口 `sqlite3GenerateConstraintChecks` 同时服务 INSERT 与 UPDATE：INSERT 由 `src/insert.c` 的 `sqlite3Insert` 调用（`regOldData` 传 `0`），UPDATE 由 `src/update.c` 调用（`regOldData` 传旧行寄存器 `regOldRowid`）。函数内部用 `isUpdate = regOldData!=0;` 区分两者。

## 怎么实现的

### 一、DDL 登记：三个 `sqlite3Add*` 函数

#### `sqlite3AddNotNull` —— 把 NOT NULL 记到列上

`src/build.c` 的 `sqlite3AddNotNull(Parse *pParse, int onError)` 做的事只有三件：

```
p = pParse->pNewTable;
if( p==0 || NEVER(p->nCol<1) ) return;
pCol = &p->aCol[p->nCol-1];
pCol->notNull = (u8)onError;
p->tabFlags |= TF_HasNotNull;
```

即：约束落在「当前正在构造的那一列」上（`pTab->aCol[pTab->nCol-1]`），冲突策略直接存进 `Column.notNull`（`src/sqliteInt.h` 中该字段为 `u8 notNull`），同时给表打上 `TF_HasNotNull` 标志。`TF_HasNotNull` 的值是 `0x00000800`，定义在 `src/sqliteInt.h`。

第三件事容易被漏掉：如果这一列同时是 UNIQUE 或主键，已经建好的索引要补一个 `uniqNotNull` 标记：

```
if( pCol->colFlags & COLFLAG_UNIQUE ){
  Index *pIdx;
  for(pIdx=p->pIndex; pIdx; pIdx=pIdx->pNext){
    assert( pIdx->nKeyCol==1 && pIdx->onError!=OE_None );
    if( pIdx->aiColumn[0]==p->nCol-1 ){
      pIdx->uniqNotNull = 1;
    }
  }
}
```

`COLFLAG_UNIQUE` 的值是 `0x0008`，`Index.uniqNotNull` 是 `unsigned uniqNotNull:1` 位域（`src/sqliteInt.h`）。这个标记后来会在 `sqlite3CompleteInsertion` 里决定 `OP_IdxInsert` 比较几列：`pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn`。

注意 `sqlite3AddNotNull` **不做**任何「UNIQUE 索引上的 NOT NULL 也要拒绝 NULL 值」的运行时检查——UNIQUE 索引允许存在多个 NULL，NOT NULL 的判定完全走列上的 `notNull` 字段。

#### `sqlite3AddCheckConstraint` —— 把 CHECK 表达式挂到表上

`src/build.c` 的 `sqlite3AddCheckConstraint(Parse *pParse, Expr *pCheckExpr, const char *zStart, const char *zEnd)` 把表达式追加进 `pTab->pCheck`（一个 `ExprList`）：

```
pTab->pCheck = sqlite3ExprListAppend(pParse, pTab->pCheck, pCheckExpr);
```

追加后还要给这条约束起名字（用于报错信息）：如果 `pParse->u1.cr.constraintName.n` 非 0（说明写了 `CONSTRAINT <name>`），用 `sqlite3ExprListSetName(pParse, pTab->pCheck, &pParse->u1.cr.constraintName, 1)`；否则从源码文本里截取：跳过 `(` 之后的空白得到 `zStart`，去掉 `zEnd` 之前的空白得到 `zEnd`，然后 `t.n = (int)(zEnd - t.z);` 作为表达式文本长度，同样走 `sqlite3ExprListSetName`。

有两个前置门槛会让约束被**丢弃**（不是报错，是静默不登记）：

```
if( pTab && !IN_DECLARE_VTAB
 && !sqlite3BtreeIsReadonly(db->aDb[db->init.iDb].pBt)
){ ...登记... } else { sqlite3ExprDelete(pParse->db, pCheckExpr); }
```

即：虚拟表声明上下文（`IN_DECLARE_VTAB`）里不登记；主库 B-Tree 是只读时也不登记——源码注释说明这么做的原因是只读库上的 CHECK 约束没有意义，因为根本写不进去。

整个函数体被 `#ifndef SQLITE_OMIT_CHECK` 包住，编译期裁掉 CHECK 支持时函数体只剩 `sqlite3ExprDelete(pParse->db, pCheckExpr);`。

#### `sqlite3AddPrimaryKey` —— 主键的两条完全不同的路

`src/build.c` 的 `sqlite3AddPrimaryKey(Parse *pParse, ExprList *pList, int onError, int autoInc, int sortOrder)` 决定主键以哪种形态存在。

先做唯一性检查：`if( pTab->tabFlags & TF_HasPrimaryKey )` 则报 `table "%s" has more than one primary key` 并跳到 `primary_key_exit`。`TF_HasPrimaryKey` 的值是 `0x00000004`。

列的收集有两种入参形态：`pList==0`（列级 `PRIMARY KEY`）时取最后一列 `iCol = pTab->nCol - 1`，`nTerm = 1`；`pList!=0`（表级 `PRIMARY KEY(...)`）时遍历 `pList->nExpr`，对每个 `TK_ID` 节点用 `sqlite3ColumnIndex(pTab, pCExpr->u.zToken)` 找到列下标，逐列调 `makeColumnPartOfPrimaryKey(pParse, pCol)`。

**分岔点是这一条判断：**

```
if( nTerm==1
 && pCol
 && pCol->eCType==COLTYPE_INTEGER
 && sortOrder!=SQLITE_SO_DESC
){
  pTab->iPKey = iCol;
  pTab->keyConf = (u8)onError;
  assert( autoInc==0 || autoInc==1 );
  pTab->tabFlags |= autoInc*TF_Autoincrement;
  if( pList ) pParse->iPkSortOrder = pList->a[0].fg.sortFlags;
  (void)sqlite3HasExplicitNulls(pParse, pList);
}else if( autoInc ){
  sqlite3ErrorMsg(pParse, "AUTOINCREMENT is only allowed on an "
     "INTEGER PRIMARY KEY");
}else{
  sqlite3CreateIndex(pParse, 0, 0, 0, pList, onError, 0,
                         0, sortOrder, 0, SQLITE_IDXTYPE_PRIMARYKEY);
  pList = 0;
}
```

- 命中「单列 + `COLTYPE_INTEGER` + 非 `SQLITE_SO_DESC`」：这一列**就是 rowid**。`Table.iPKey` 记下它的下标（`-1` 表示没有 INTEGER PRIMARY KEY），`Table.keyConf` 记下冲突策略，AUTOINCREMENT 则通过 `pTab->tabFlags |= autoInc*TF_Autoincrement` 打标志（`autoInc` 被断言只能是 `0` 或 `1`）。这条路上**不创建任何索引**。
- 带 AUTOINCREMENT 但没命中上面条件：报 `AUTOINCREMENT is only allowed on an INTEGER PRIMARY KEY`。
- 其余情况：调 `sqlite3CreateIndex(..., SQLITE_IDXTYPE_PRIMARYKEY)` 建一个主键索引，`onError` 作为该索引的冲突策略传进去。

`sortOrder!=SQLITE_SO_DESC` 这一条是有意的：降序排列的 INTEGER PRIMARY KEY 不能当 rowid 用。

### 二、写入阶段：`sqlite3GenerateConstraintChecks` 的骨架

`src/insert.c` 的 `sqlite3GenerateConstraintChecks` 签名较长，关键参数：`aRegIdx`（每个索引一个寄存器槽，0 表示该索引不写）、`iDataCur`（主表游标，WITHOUT ROWID 表则是主键索引游标）、`iIdxCur`（第一个索引游标）、`regNewData`（新行寄存器段的起点）、`regOldData`（旧行寄存器段起点，INSERT 传 0）、`pkChng`、`overrideError`、`ignoreDest`（`OE_Ignore` 时跳转的目标）、`pbMayReplace`（出参）、`aiChng`（UPDATE 时每列是否变化的数组）、`pUpsert`。

函数开头就确定主键形态：

```
if( HasRowid(pTab) ){
  pPk = 0;
  nPkField = 1;
}else{
  pPk = sqlite3PrimaryKeyIndex(pTab);
  nPkField = pPk->nKeyCol;
}
```

即 rowid 表的「真主键」是 1 个字段（rowid 本身），WITHOUT ROWID 表的「真主键」是主键索引的 `nKeyCol` 个字段。

然后按固定顺序生成四段检查代码：**NOT NULL → CHECK → rowid 主键 → UNIQUE 索引**。

### 三、NOT NULL 检查：两遍扫描

NOT NULL 段只在 `pTab->tabFlags & TF_HasNotNull` 时才生成。结构是一个 `while(1)` 的两遍循环：

```
while(1){  /* Make 2 passes over columns. Exit loop via "break" */
  for(i=0; i<nCol; i++){
    Column *pCol = &pTab->aCol[i];
    onError = pCol->notNull;
    if( onError==OE_None ) continue;      /* 没有 NOT NULL */
    if( i==pTab->iPKey ) continue;        /* ROWID 永远不是 NULL */
    isGenerated = pCol->colFlags & COLFLAG_GENERATED;
    if( isGenerated && !b2ndPass ){ nGenerated++; continue; }
    if( aiChng && aiChng[i]<0 && !isGenerated ) continue;  /* 未变化的列不检查 */
    ...
  }
  if( nGenerated==0 && nSeenReplace==0 ) break;  /* 一遍就够 */
  if( b2ndPass ) break;                          /* 最多两遍 */
  b2ndPass = 1;
  ...
}
```

两遍扫描的必要性来自生成列与 REPLACE 的组合：第一遍里如果某个 NOT NULL 约束是 `ON CONFLICT REPLACE`，它会用列的默认值把 NULL 替换掉；而默认值里可能有生成列，生成列的值又依赖别的列，所以要在替换之后重算生成列再查第二遍。源码注释写明：「If any NOT NULL ON CONFLICT REPLACE constraints fired on the first pass, recomputed values for all generated columns, as those values might depend on columns affected by the REPLACE.」，重算调用的是 `sqlite3ComputeGeneratedColumns(pParse, regNewData+1, pTab)`。

REPLACE 还有一条降级规则：

```
if( onError==OE_Replace ){
  if( b2ndPass        /* REPLACE becomes ABORT on the 2nd pass */
   || pCol->iDflt==0  /* REPLACE is ABORT if no DEFAULT value */
  ){
    onError = OE_Abort;
  }else{
    /* 生成 OP_NotNull + sqlite3ExprCodeCopy(...) 填默认值 */
  }
}
```

即：**没有默认值的列，`ON CONFLICT REPLACE` 在 NOT NULL 上退化成 `OE_Abort`**；第二遍扫描时一律退化成 `OE_Abort`。降级分支发的是 `OP_HaltIfNull` 加 `OP_NotNull`+`sqlite3ExprCodeCopy`（用 `sqlite3ColumnExprAuth(pParse)` 取列的默认/生成表达式，`sqlite3ColumnExprAuth` 定义在 `src/build.c`）。

非 REPLACE 分支按策略分三种：

| 策略 | 生成的指令 |
|---|---|
| `OE_Abort` / `OE_Rollback` / `OE_Fail` | `OP_HaltIfNull`，P1 是 `SQLITE_CONSTRAINT_NOTNULL`，P2 是 `onError`，P4 是 `"表名.列名"` 的动态字符串，P5 置 `P5_ConstraintNotNull`；`OE_Abort` 时额外调 `sqlite3MayAbort(pParse)` |
| `OE_Ignore` | `OP_IsNull iReg, ignoreDest` |
| `OE_Replace` | `OP_NotNull` + 填默认值 |

`SQLITE_CONSTRAINT_NOTNULL` 定义在 `src/sqlite.h.in`，值为 `SQLITE_CONSTRAINT | (5<<8)`，而 `SQLITE_CONSTRAINT` 的值是 `19`。`P5_ConstraintNotNull`、`P5_ConstraintUnique`、`P5_ConstraintCheck` 定义在 `src/vdbe.h`，值分别是 `1`、`2`、`3`。

### 四、CHECK 检查

CHECK 段的前提是 `pTab->pCheck` 非空，且 `(db->flags & SQLITE_IgnoreChecks)==0`。

```
pParse->iSelfTab = -(regNewData+1);
onError = overrideError!=OE_Default ? overrideError : OE_Abort;
for(i=0; i<pCheck->nExpr; i++){
  ...
  if( aiChng
   && !sqlite3ExprReferencesUpdatedColumn(pExpr, aiChng, pkChng)
  ){
    continue;   /* 约束不引用任何被改动的列，跳过 */
  }
  if( bAffinityDone==0 ){
    sqlite3TableAffinity(v, pTab, regNewData+1);
    bAffinityDone = 1;
  }
  allOk = sqlite3VdbeMakeLabel(pParse);
  sqlite3VdbeVerifyAbortable(v, onError);
  pCopy = sqlite3ExprDup(db, pExpr, 0);
  if( !db->mallocFailed ){
    sqlite3ExprIfTrue(pParse, pCopy, allOk, SQLITE_JUMPIFNULL);
  }
  sqlite3ExprDelete(db, pCopy);
  if( onError==OE_Ignore ){
    sqlite3VdbeGoto(v, ignoreDest);
  }else{
    char *zName = pCheck->a[i].zEName;
    if( onError==OE_Replace ) onError = OE_Abort; /* IMP: R-26383-51744 */
    sqlite3HaltConstraint(pParse, SQLITE_CONSTRAINT_CHECK,
                          onError, zName, P4_TRANSIENT,
                          P5_ConstraintCheck);
  }
  sqlite3VdbeResolveLabel(v, allOk);
}
pParse->iSelfTab = 0;
```

要点：

- CHECK 的默认策略是 `OE_Abort`（`overrideError` 为 `OE_Default` 时）。
- **CHECK 配 `REPLACE` 是非法的**，源码直接把它改写成 `OE_Abort`，并挂着 IMP 编号 `R-26383-51744`。
- UPDATE 场景下用 `sqlite3ExprReferencesUpdatedColumn(pExpr, aiChng, pkChng)` 判断这条约束是否引用了被改动的列；不引用就 `continue` 跳过整个约束。该函数定义在 `src/insert.c`，内部用 `Walker` 遍历表达式树，收集 `CKCNSTRNT_COLUMN` / `CKCNSTRNT_ROWID` 两种标志。
- 表达式是**复制**后再编译的：`pCopy = sqlite3ExprDup(db, pExpr, 0);` → `sqlite3ExprIfTrue(pParse, pCopy, allOk, SQLITE_JUMPIFNULL);` → `sqlite3ExprDelete(db, pCopy);`。复制是为了让 `pParse->iSelfTab` 的临时重绑定不影响模式里存的原始表达式。
- `pParse->iSelfTab = -(regNewData+1);` 把「新行寄存器段」映射成约束表达式里 `NEW.col` 的引用基准。
- `bAffinityDone` 标志保证 `sqlite3TableAffinity(v, pTab, regNewData+1)` 只发一次（`sqlite3TableAffinity` 定义在 `src/insert.c`：STRICT 表发 `OP_TypeCheck`，普通表发 `OP_Affinity`）。

### 五、rowid 主键检查

`pkChng && pPk==0` 时（即 rowid 表且 rowid 可能变了）生成 rowid 冲突检查。

策略来源是 `pTab->keyConf`（`sqlite3AddPrimaryKey` 里写下的那个 `onError`），`overrideError` 非 `OE_Default` 时以它为准，否则 `OE_Default` 退化为 `OE_Abort`。

UPDATE 场景先比一次旧值，rowid 没变就跳过整段冲突逻辑：

```
if( isUpdate ){
  sqlite3VdbeAddOp3(v, OP_Eq, regNewData, addrRowidOk, regOldData);
  sqlite3VdbeChangeP5(v, SQLITE_NOTNULL);
}
sqlite3VdbeAddOp3(v, OP_NotExists, iDataCur, addrRowidOk, regNewData);
```

`OP_NotExists` 命中说明 rowid 没冲突，跳到 `addrRowidOk`；否则按策略分派：

| 策略 | 动作 |
|---|---|
| `OE_Rollback` / `OE_Abort` / `OE_Fail` | `sqlite3RowidConstraint(pParse, onError, pTab)` |
| `OE_Replace` | 有 replace 触发器时 `sqlite3MultiWrite(pParse)` + `sqlite3GenerateRowDelete(...)`；否则只发 `OP_Delete`（`SQLITE_ENABLE_PREUPDATE_HOOK` 下）或 `sqlite3GenerateRowIndexDelete(...)` 删索引项 |
| `OE_Update` | `sqlite3UpsertDoUpdate(pParse, pUpsert, pTab, 0, iDataCur)`（`src/upsert.c`），随后落到 `OE_Ignore` |
| `OE_Ignore` | `sqlite3VdbeGoto(v, ignoreDest)` |

`sqlite3RowidConstraint` 定义在 `src/build.c`：如果 `pTab->iPKey>=0`（表有 INTEGER PRIMARY KEY），错误码是 `SQLITE_CONSTRAINT_PRIMARYKEY`，消息是 `"表名.列名"`；否则错误码是 `SQLITE_CONSTRAINT_ROWID`，消息是 `"表名.rowid"`。两者最终都走 `sqlite3HaltConstraint(pParse, rc, onError, zMsg, P4_DYNAMIC, P5_ConstraintUnique)`。

`sqlite3HaltConstraint`（`src/build.c`）是所有约束报错的统一出口：

```
if( onError==OE_Abort ){
  sqlite3MayAbort(pParse);
}
sqlite3VdbeAddOp4(v, OP_Halt, errCode, onError, 0, p4, p4type);
sqlite3VdbeChangeP5(v, p5Errmsg);
```

即发一条 `OP_Halt`，P1 是扩展错误码、P2 是冲突解决策略（由 VDBE 决定要不要回滚）、P4 是错误消息、P5 是消息类型。

**rowid 的 REPLACE 被推迟到最后执行**：源码注释写得很清楚——「If the response to a rowid conflict is REPLACE but the response to some other UNIQUE constraint is FAIL or IGNORE, then we need to defer the running of the rowid conflict checking until after the UNIQUE constraints have run.」。实现方式是插一对 `OP_Goto`：`ipkTop` 记下跳转目标，`ipkBottom` 记下回跳位置，函数末尾「If the IPK constraint is a REPLACE, run it last」处用 `sqlite3VdbeGoto(v, ipkTop)` 与 `sqlite3VdbeJumpHere(v, ipkBottom)` 把这段代码搬到 UNIQUE 检查之后。

### 六、UNIQUE / 主键索引检查

最后一个大段遍历 `pTab->pIndex` 链，对每个唯一索引生成冲突检测。

遍历顺序不是简单的 `pNext` 顺序，而是通过 `IndexIterator` 抽象出两种模式：`eType==0` 走 `Index.pNext` 链表，`eType==1` 走 `IndexListTerm` 数组。数组模式只在有 UPSERT 且带约束目标时启用，目的是让所有 `ON CONFLICT` 子句指定的索引先按顺序检查。构造数组时先遍历 `pUpsert` 链表把带 `pUpsertTarget` 的索引按 `ON CONFLICT` 出现顺序填进数组，再用 `bUsed[]` 标记去重（重复的 `ON CONFLICT` 子句被忽略），最后把剩余索引按原顺序补进数组。数组用 `sqlite3DbMallocZero(db, nByte)` 分配，`nByte = (sizeof(IndexListTerm)+1)*nIdx + nIdx;`，分配失败直接 `return;`（OOM）。

每个索引的处理顺序：

1. `if( aRegIdx[ix]==0 ) continue;` —— 这个索引不需要写，跳过。
2. 生成索引键记录存进 `aRegIdx[ix]`：逐列 `OP_IntCopy`（rowid / INTEGER PRIMARY KEY）或 `OP_SCopy`（普通列），最后 `OP_MakeRecord` 组装。部分索引用 `OP_Null 0, aRegIdx[ix]` 先把目标寄存器置 NULL，再用 `sqlite3ExprIfFalseDup` 判断 WHERE 谓词，不成立就跳到 `addrUniqueOk`。
3. UPDATE 场景下，若这是 WITHOUT ROWID 表的主键索引且 `pkChng==0`，冲突不可能发生：`if( isUpdate && pPk==pIdx && pkChng==0 )` 直接跳到 `addrUniqueOk`。
4. 冲突策略来自 `pIdx->onError`；`OE_None` 表示这不是唯一索引，跳过。`overrideError` 优先，否则 `OE_Default` 退化为 `OE_Abort`。
5. 有匹配的 UPSERT 子句时策略被改写：`DO NOTHING` → `OE_Ignore`，`DO UPDATE` → `OE_Update`。
6. 冲突检测本体是一条 `OP_NoConflict`：

```
addrConflictCk =
  sqlite3VdbeAddOp4Int(v, OP_NoConflict, iThisCur, addrUniqueOk,
                       regIdx, pIdx->nKeyCol); VdbeCoverage(v);
```

`OP_NoConflict` 的 P4 是参与比较的键列数 `pIdx->nKeyCol`。

7. 冲突命中后的分支：

| 策略 | 动作 |
|---|---|
| `OE_Rollback` / `OE_Abort` / `OE_Fail` | `sqlite3UniqueConstraint(pParse, onError, pIdx)` |
| `OE_Update` | `sqlite3UpsertDoUpdate(pParse, pUpsert, pTab, pIdx, iIdxCur+ix)`（`src/upsert.c`），随后落到 `OE_Ignore` |
| `OE_Ignore` | `sqlite3VdbeGoto(v, ignoreDest)` |
| `OE_Replace`（default 分支） | 有 replace 触发器时 `sqlite3MultiWrite(pParse)`；`sqlite3GenerateRowDelete(...)` 删掉冲突行；置 `seenReplace = 1` |

`sqlite3UniqueConstraint`（`src/build.c`）构造错误消息：表达式索引（`pIdx->aColExpr` 非空）用 `index '%q'`；普通索引逐个键列拼成 `"表名.列名"` 列表。错误码按 `IsPrimaryKeyIndex(pIdx)` 分 `SQLITE_CONSTRAINT_PRIMARYKEY` 与 `SQLITE_CONSTRAINT_UNIQUE`。

### 七、REPLACE 的索引顺序与「重查」机制

**REPLACE 索引排在最后。** `sqlite3GenerateConstraintChecks` 的源码注释明确写了这个排序契约：

> The order of constraints used to have OE_Update as (2) and OE_Abort and so forth as (1). But apparently PostgreSQL checks the OE_Update constraint before any others, so it had to be moved.

排序由 `src/build.c` 的 `sqlite3CreateIndex` 在退出时维护：

```
for(ppFrom=&pTab->pIndex; (pThis = *ppFrom)!=0; ppFrom=&pThis->pNext){
  Index *pNext;
  if( pThis->onError!=OE_Replace ) continue;
  while( (pNext = pThis->pNext)!=0 && pNext->onError!=OE_Replace ){
    *ppFrom = pNext;
    pThis->pNext = pNext->pNext;
    pNext->pNext = pThis;
    ppFrom = &pNext->pNext;
  }
  break;
}
```

`sqlite3CompleteInsertion`（`src/insert.c`）里有对应的断言把这个排序当不变量守住：

```
assert( pIdx->onError!=OE_Replace
     || pIdx->pNext==0
     || pIdx->pNext->onError==OE_Replace );
```

**替换触发器跑完后要重查唯一约束。** 机制是这样的：先判断「会不会因为 REPLACE 的删除动作而触发 DELETE 触发器或外键级联动作」——

```
if( (db->flags & (SQLITE_RecTriggers|SQLITE_ForeignKeys))==0 ){
  pTrigger = 0;
  regTrigCnt = 0;
}else{
  if( db->flags&SQLITE_RecTriggers ){
    pTrigger = sqlite3TriggersExist(pParse, pTab, TK_DELETE, 0, 0);
    regTrigCnt = pTrigger!=0 || sqlite3FkRequired(pParse, pTab, 0, 0);
  }else{
    pTrigger = 0;
    regTrigCnt = sqlite3FkRequired(pParse, pTab, 0, 0);
  }
  if( regTrigCnt ){
    if( overrideError!=OE_Replace ){
      if( overrideError!=OE_Default ){
        regTrigCnt = 0;
      }else if( pkChng==0 || pPk || pTab->keyConf!=OE_Replace ){
        for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
          if( pIdx->onError==OE_Replace ) break;
        }
        if( pIdx==0 ) regTrigCnt = 0;
      }
    }
  }
  if( regTrigCnt ){
    regTrigCnt = ++pParse->nMem;
    sqlite3VdbeAddOp2(v, OP_Integer, 0, regTrigCnt);
    lblRecheckOk = sqlite3VdbeMakeLabel(pParse);
    addrRecheck = lblRecheckOk;
  }
}
```

`sqlite3FkRequired` 定义在 `src/fkey.c`：非 UPDATE 路径下只要表是任何外键的父表或子表就返回非 0（`bHaveFK = (sqlite3FkReferences(pTab) || pTab->u.tab.pFKey);`）。

若判定「可能触发 replace 触发器」，就分配一个计数器寄存器 `regTrigCnt`（`++pParse->nMem`）并初始化为 0。每次 REPLACE 真正删掉一行时发 `OP_AddImm regTrigCnt, 1` 递增。所有约束检查跑完后：

```
if( nReplaceTrig ){
  sqlite3VdbeAddOp2(v, OP_IfNot, regTrigCnt, lblRecheckOk);VdbeCoverage(v);
  if( !pPk ){
    if( isUpdate ){
      sqlite3VdbeAddOp3(v, OP_Eq, regNewData, addrRecheck, regOldData);
      sqlite3VdbeChangeP5(v, SQLITE_NOTNULL);
      VdbeCoverage(v);
    }
    sqlite3VdbeAddOp3(v, OP_NotExists, iDataCur, addrRecheck, regNewData);
    VdbeCoverage(v);
    sqlite3RowidConstraint(pParse, OE_Abort, pTab);
  }else{
    sqlite3VdbeGoto(v, addrRecheck);
  }
}
```

`OP_IfNot` 的语义是「计数为 0 就跳到 `lblRecheckOk` 跳过重查」——即只有真的发生过替换删除才重查。重查代码是把之前的冲突检测指令**复制一遍**，把跳转目标改成 `lblRecheckOk`：

```
while( nConflictCk>0 ){
  VdbeOp x;
  x = *sqlite3VdbeGetOp(v, addrConflictCk);
  if( x.opcode!=OP_IdxRowid || isUpdate ){
    int p2;
    const char *zP4;
    if( sqlite3OpcodeProperty[x.opcode]&OPFLG_JUMP ){
      p2 = lblRecheckOk;
    }else{
      p2 = x.p2;
    }
    zP4 = x.p4type==P4_INT32 ? SQLITE_INT_TO_PTR(x.p4.i) : x.p4.z;
    sqlite3VdbeAddOp4(v, x.opcode, x.p1, p2, x.p3, zP4, x.p4type);
    sqlite3VdbeChangeP5(v, x.p5);
    VdbeCoverageIf(v, p2!=x.p2);
  }
  nConflictCk--;
  addrConflictCk++;
}
sqlite3UniqueConstraint(pParse, OE_Abort, pIdx);
```

**重查时的策略固定为 `OE_Abort`**（最后一行的 `sqlite3UniqueConstraint(pParse, OE_Abort, pIdx)`），源码注释说明原因：「But on the recheck, the resolution is OE_Abort instead of OE_Replace.」——否则会陷入「替换→触发器→再替换」的循环。

### 八、收尾：生成表记录

函数最后为 rowid 表生成表记录：

```
if( HasRowid(pTab) ){
  int regRec = aRegIdx[ix];
  sqlite3VdbeAddOp3(v, OP_MakeRecord, regNewData+1, pTab->nNVCol, regRec);
  sqlite3SetMakeRecordP5(v, pTab);
  if( !bAffinityDone ){
    sqlite3TableAffinity(v, pTab, 0);
  }
}
*pbMayReplace = seenReplace;
```

`pTab->nNVCol` 是非虚拟列的个数（`sqlite3AddColumn` 里 `p->nCol++; p->nNVCol++;` 一起递增）。源码注释解释为什么记录要在约束检查期间生成：「It is important that the record be generated during constraint checks to avoid affinity changes to the register content that occur after constraint checks but before the new record is inserted.」

`*pbMayReplace = seenReplace` 是给调用方的信号：`src/insert.c` 的 `sqlite3Insert` 用它算 `bUseSeek = (isReplace==0 || !sqlite3VdbeHasSubProgram(v));`；`src/update.c` 用它决定要不要重新 seek 主表游标（`if( bReplace || chngKey )`）。

## 关键取舍

- **源码事实：**CHECK 约束配 `ON CONFLICT REPLACE` 在代码生成期被改写成 `OE_Abort`（挂 IMP 编号 `R-26383-51744`）。**架构推论：**CHECK 表达式的语义是「这一行本身是否合法」，REPLACE 的语义是「把旧行删掉换成新行」——删旧行并不能让原本不合法的表达式变合法，所以唯一合理的处置就是中止。对比之下，UNIQUE 配 REPLACE 是有意义的（冲突的是「键值重复」，删掉旧行正好解决），NOT NULL 配 REPLACE 也有意义（用默认值填上）。

- **源码事实：**NOT NULL 检查走两遍扫描，且 REPLACE 在没有默认值时降级为 `OE_Abort`、在第二遍一律降级为 `OE_Abort`。**架构推论：**「用默认值替换 NULL」是一次会改变列值的写操作，列值一变，依赖它的生成列的值就过期了；源码选择「替换后重算生成列再查一遍」而不是「不替换」，是为了让 `NOT NULL ON CONFLICT REPLACE` 在有生成列的表上仍然给出正确结果。代价是最坏情况下多一轮列扫描。

- **源码事实：**REPLACE 的 rowid 冲突检查被推迟到所有 UNIQUE 检查之后执行（`ipkTop` / `ipkBottom` 的一对 `OP_Goto`）。**架构推论：**如果先做 rowid 的 REPLACE，删掉旧行可能连带删掉别的行，进而让后续的 UNIQUE 检查面对一个已经变过的表。推迟到末尾能让「先失败的事先失败」——即 `FAIL`/`IGNORE` 语义要求的「不得改动任何已有行」得以保证。

- **源码事实：**REPLACE 索引被 `sqlite3CreateIndex` 统一挪到 `pTab->pIndex` 链表末尾，`sqlite3CompleteInsertion` 用断言守住这个不变量。**架构推论：**约束检查的生成顺序直接由链表顺序决定，而链表顺序又是模式对象的一部分。把「REPLACE 排最后」做成链表的构造期不变量，比在约束检查代码里每次排序便宜得多——排序成本一次性付在 DDL 阶段。

- **源码事实：**替换触发器跑完后要重查所有唯一约束，且重查固定用 `OE_Abort`。**架构推论：**REPLACE 删掉旧行会触发 DELETE 触发器和外键级联，这些动作可能又插入或删除了别的行，原先通过的唯一约束未必还成立；但重查时若仍用 `OE_Replace`，就会变成「替换 → 触发 → 再替换」的无界循环，所以重查的策略必须收紧到「只报错，不替换」。

- **源码事实：**CHECK 表达式在编译前先 `sqlite3ExprDup` 复制一份，编译完立刻 `sqlite3ExprDelete` 释放。**架构推论：**`pParse->iSelfTab` 会被临时改写成 `-(regNewData+1)`，把约束里的列引用重定向到「新行寄存器段」。如果直接编译模式对象里存的那棵表达式树，重定向会污染模式本身，后续语句编译同一张表时就会读到被改坏的表达式树。复制是隔离这次重定向的最低成本手段。

- **源码事实：**`sqlite3AddCheckConstraint` 在只读库（`sqlite3BtreeIsReadonly`）与虚拟表声明上下文（`IN_DECLARE_VTAB`）下静默丢弃 CHECK 约束。**架构推论：**只读库上根本执行不了写入，登记约束没有可观测的行为；虚拟表的约束由虚拟表实现自己负责，核心引擎无权代管。两者都不报错，是把「这个约束在当前上下文不适用」与「这个约束写错了」区分开。

- **源码事实：**`sqlite3GenerateConstraintChecks` 的错误出口统一是 `sqlite3HaltConstraint` 发的一条 `OP_Halt`，错误码细分到 `SQLITE_CONSTRAINT_NOTNULL`（`SQLITE_CONSTRAINT | (5<<8)`）、`SQLITE_CONSTRAINT_PRIMARYKEY`（`6<<8`）、`SQLITE_CONSTRAINT_UNIQUE`（`8<<8`）、`SQLITE_CONSTRAINT_ROWID`（`10<<8`）、`SQLITE_CONSTRAINT_CHECK`（`1<<8`）。**架构推论：**用一条 `OP_Halt` 承载全部约束失败，把「是否回滚」这个决策交给 P2 里带的冲突策略，而不生成多套不同指令。这让 VDBE 的指令集不必为每种约束类型各长一套。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/insert.c` | `sqlite3GenerateConstraintChecks` | 写入阶段的约束检查总入口：依次生成 NOT NULL、CHECK、rowid 主键、UNIQUE 索引四段检查代码，处理 `overrideError`/UPSERT 的策略改写、REPLACE 的延迟执行与替换触发器后的唯一约束重查，最终回填 `*pbMayReplace` |
| `src/insert.c` | `indexIteratorFirst` / `indexIteratorNext` | 索引遍历器的两种模式：`eType==0` 走 `Index.pNext` 链表，`eType==1` 走 `IndexListTerm` 数组（UPSERT 指定顺序） |
| `src/insert.c` | `sqlite3TableAffinity` | 给新行寄存器段套用列亲和性：STRICT 表发 `OP_TypeCheck`，普通表发 `OP_Affinity`；约束检查期间用它保证比较发生在亲和性转换之后 |
| `src/insert.c` | `sqlite3ExprReferencesUpdatedColumn` | 判断 CHECK 约束表达式是否引用了 UPDATE 改动的列（含 rowid），不引用则跳过该约束 |
| `src/insert.c` | `sqlite3CompleteInsertion` | 约束检查之后的写入阶段：逐索引发 `OP_IdxInsert`，键列数取 `pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn`；含「REPLACE 索引必须在链表末尾」的断言 |
| `src/insert.c` | `sqlite3Insert` | INSERT 编译入口，调用 `sqlite3GenerateConstraintChecks`（`regOldData` 传 0），并用返回的 `isReplace` 计算 `bUseSeek` |
| `src/update.c` | `sqlite3Update` | UPDATE 编译入口，调用 `sqlite3GenerateConstraintChecks`（`regOldData` 传 `regOldRowid`，`aiChng` 传 `aXRef`），并按 `bReplace || chngKey` 重新 seek 主表游标 |
| `src/build.c` | `sqlite3AddNotNull` | DDL 登记：把冲突策略写进 `Column.notNull`，给表打 `TF_HasNotNull` 标志，并给同列的 UNIQUE 索引补 `uniqNotNull` 标记 |
| `src/build.c` | `sqlite3AddCheckConstraint` | DDL 登记：把 CHECK 表达式追加进 `Table.pCheck`，并用 `CONSTRAINT <name>` 或源码文本给约束命名；只读库与虚拟表上下文中静默丢弃 |
| `src/build.c` | `sqlite3AddPrimaryKey` | DDL 登记：单列 INTEGER 且非降序时把该列设为 rowid（写 `Table.iPKey`、`Table.keyConf`、`TF_Autoincrement`），否则创建 `SQLITE_IDXTYPE_PRIMARYKEY` 索引；重复主键报 `table "%s" has more than one primary key` |
| `src/build.c` | `sqlite3CreateIndex` | 索引创建；在退出阶段把所有 `OE_Replace` 索引重排到 `pTab->pIndex` 链表末尾，维持约束检查的生成顺序契约 |
| `src/build.c` | `sqlite3HaltConstraint` | 约束报错的统一出口：发一条 `OP_Halt`，P1 扩展错误码、P2 冲突策略、P4 消息、P5 消息类型；`OE_Abort` 时额外调 `sqlite3MayAbort` |
| `src/build.c` | `sqlite3UniqueConstraint` | UNIQUE / 主键索引冲突的报错生成：错误码按 `IsPrimaryKeyIndex(pIdx)` 分 `SQLITE_CONSTRAINT_PRIMARYKEY` 与 `SQLITE_CONSTRAINT_UNIQUE`，消息为 `表名.列名` 列表或 `index '名字'` |
| `src/build.c` | `sqlite3RowidConstraint` | rowid 冲突的报错生成：有 INTEGER PRIMARY KEY 时错误码 `SQLITE_CONSTRAINT_PRIMARYKEY`、消息 `表名.列名`，否则错误码 `SQLITE_CONSTRAINT_ROWID`、消息 `表名.rowid` |
| `src/build.c` | `sqlite3ColumnExprAuth` | 取列的默认/生成表达式（`sqlite3ColumnExpr`），并在有授权回调时用 `sqlite3FuncAuth` 检查其中用到的 SQL 函数；NOT NULL 的 REPLACE 分支用它填默认值 |
| `src/upsert.c` | `sqlite3UpsertDoUpdate` / `sqlite3UpsertOfIndex` / `sqlite3UpsertNextIsIPK` | UPSERT 支撑：`sqlite3UpsertOfIndex` 找出针对某索引的 `ON CONFLICT` 子句，`sqlite3UpsertDoUpdate` 生成 DO UPDATE 的代码，`sqlite3UpsertNextIsIPK` 判断下一条是否针对 IPK |
| `src/fkey.c` | `sqlite3FkRequired` | 判断本次写入是否需要外键处理（触发 FK 级联动作时需要重查唯一约束） |
| `src/parse.y` | `ccons ::= NOT NULL onconf(R).` / `ccons ::= PRIMARY KEY sortorder(Z) onconf(R) autoinc(I).` / `tcons ::= PRIMARY KEY LP sortlist(X) autoinc(I) RP onconf(R).` / `ccons ::= CHECK LP(A) expr(X) RP(B).` / `tcons ::= CHECK LP(A) expr(E) RP(B) onconf.` | DDL 语法的动作，分别调用 `sqlite3AddNotNull`、`sqlite3AddPrimaryKey`、`sqlite3AddCheckConstraint` |
| `src/parse.y` | `onconf` / `resolvetype` / `raisetype` | 把 SQL 里的冲突解决子句映射成 `OE_*` 常量：`ROLLBACK`/`ABORT`/`FAIL`/`IGNORE`/`REPLACE`，缺省为 `OE_Default` |
| `src/sqliteInt.h` | `OE_None` / `OE_Rollback` / `OE_Abort` / `OE_Fail` / `OE_Ignore` / `OE_Replace` / `OE_Update` / `OE_Default` | 冲突解决策略常量，值分别是 `0` / `1` / `2` / `3` / `4` / `5` / `6` / `11` |
| `src/sqliteInt.h` | `TF_HasNotNull` / `TF_HasPrimaryKey` / `TF_Autoincrement` | 表级标志位，值分别是 `0x00000800` / `0x00000004` / `0x00000008` |
| `src/sqliteInt.h` | `COLFLAG_UNIQUE` / `COLFLAG_GENERATED` | 列标志位，值分别是 `0x0008` / `0x0060` |
| `src/sqlite.h.in` | `SQLITE_CONSTRAINT` / `SQLITE_CONSTRAINT_CHECK` / `SQLITE_CONSTRAINT_NOTNULL` / `SQLITE_CONSTRAINT_PRIMARYKEY` / `SQLITE_CONSTRAINT_UNIQUE` / `SQLITE_CONSTRAINT_ROWID` | 约束错误的扩展错误码，基码 `19`，各子码分别是 `1<<8` / `5<<8` / `6<<8` / `8<<8` / `10<<8` |
| `src/vdbe.h` | `P5_ConstraintNotNull` / `P5_ConstraintUnique` / `P5_ConstraintCheck` | `OP_Halt` 的 P5 消息类型标记，值分别是 `1` / `2` / `3` |

## 与主核 / AI 的关系

主核（`engine/db/` 自研多模态数据库内核）在写路径上遇到的是同一类问题：写入前必须判定这一行是否合法，判定失败时要么回滚要么跳过。SQLite 的做法给了一个可直接对照的三层分工样本——**登记**（`sqlite3Add*` 把约束挂到模式对象上）、**生成**（`sqlite3GenerateConstraintChecks` 把它翻译成 VDBE 指令）、**执行**（`OP_Halt` 抛错 + P2 携带的策略决定回滚范围）。主核若采用「运行时逐行解释执行」而非「预编译成字节码」，第 2 层可以省掉，但第 1 层的登记形态与第 3 层的错误分类仍要对齐：特别是把「唯一约束」「主键冲突」「非空违规」「CHECK 失败」拆成不同错误码，而不是一律报同一个约束错误——调用方要靠这个区分才能决定是重试、跳过还是回滚。

SQLite 这里有两处取舍值得主核特别留意：一是 `OE_Fail` 与 `OE_Abort` 的区别（前者保留此前改动、后者只撤销当前命令），主核若只有「整条语句回滚」一种粒度，UPSERT 批量写入时的部分成功语义就表达不出来；二是 `OE_Replace` 的「删除冲突行」动作会连带触发 DELETE 触发器与外键级联，SQLite 为此专门做了「替换后重查唯一约束」的一整套机制。

AI 侧没有直接对应实现。本页可作为理解「一条 INSERT 编译出的 VDBE 程序里，约束检查占多大篇幅」的材料：约束检查把 NOT NULL、CHECK、rowid 冲突、UNIQUE 冲突统一压成「读寄存器 → 比较 → 跳转 → 一条 `OP_Halt`」的形状，是 VDBE 这种「指令极简、错误统一出口」风格最典型的体现；也提示 AI 生成的存储引擎代码里，约束检查往往是最容易被写成「边遍历边判断」的片段，而 SQLite 的做法是先把约束固化成可复用的模式对象，再在编译期一次性生成检查代码。

## 参考

- [SQLite 官方文档：CREATE TABLE](https://sqlite.org/lang_createtable.html) —— 表级与列级约束语法、`ON CONFLICT` 子句、CHECK 约束的语义
- [SQLite 官方文档：冲突解决算法](https://sqlite.org/lang_conflict.html) —— ROLLBACK / ABORT / FAIL / IGNORE / REPLACE 五种算法的官方定义与差异
- [SQLite 官方文档：UPSERT](https://sqlite.org/lang_upsert.html) —— `ON CONFLICT DO NOTHING` / `DO UPDATE` 的语法与约束目标匹配规则
- [SQLite 官方文档：STRICT Tables](https://sqlite.org/stricttables.html) —— STRICT 表的类型约束如何与 `sqlite3TableAffinity` 发出的 `OP_TypeCheck` 对应
- [SQLite 官方文档：架构总览](https://sqlite.org/arch.html) —— 从 SQL 文本到 B-Tree 的分层，说明代码生成器与约束检查在流水线上的位置
- [SQLite 官方文档：VDBE 指令一览](https://sqlite.org/opcode.html) —— `OP_Halt`、`OP_HaltIfNull`、`OP_NoConflict`、`OP_NotExists`、`OP_IdxInsert`、`OP_MakeRecord` 等操作码的语义
- [SQLite 源码：src/insert.c](https://github.com/sqlite/sqlite/blob/master/src/insert.c) —— 本页 `refs` 指向的文件，`sqlite3GenerateConstraintChecks` 的实现
- [SQLite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c) —— 本页 `refs` 指向的文件，`sqlite3AddNotNull`、`sqlite3AddCheckConstraint`、`sqlite3AddPrimaryKey` 的实现
- [SQLite 源码：src/update.c](https://github.com/sqlite/sqlite/blob/master/src/update.c) —— UPDATE 路径对 `sqlite3GenerateConstraintChecks` 的调用
- [SQLite 源码：src/upsert.c](https://github.com/sqlite/sqlite/blob/master/src/upsert.c) —— UPSERT 的 `ON CONFLICT` 目标匹配与 DO UPDATE 代码生成
