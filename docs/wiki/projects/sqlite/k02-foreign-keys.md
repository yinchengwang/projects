---
project: sqlite
feature: foreign-keys
page_type: feature
schema_version: 1
refs:
  - path: src/fkey.c
    symbols: [sqlite3FkCheck, sqlite3FkActions, sqlite3FkRequired, sqlite3FkLocateIndex]
---

# 外键约束

## 一句话职责

给 SQLite 的写入路径补上一套**外键引用完整性**机制：在 DML 编译期判断"父表有没有对应行、要不要级联、子键被改没被改"，并把判定结果编译成 VDBE 指令（计数器增减、查找父行、扫描子行）或等价的触发器程序；约束不成立时以 `SQLITE_CONSTRAINT_FOREIGNKEY` 中止当前语句。

## 机制是什么

外键在 SQLite 里不是运行期拦截器，而是**编译期生成 VDBE 代码**的机制。`src/fkey.c` 文件头的注释把整套方案写清楚了：

- **immediate 与 deferred 两种约束**。immediate 约束被违反时返回 `SQLITE_CONSTRAINT_FOREIGNKEY` 并回滚当前语句事务；deferred 约束被违反时当下不处理，只有在事务提交前仍未修复才会失败。
- **deferred 用计数器实现**。计数器与数据库句柄关联：事务打开时置零；每有一条语句造成一次违例就加一；每有一条语句消解掉一次已有违例就减一；提交时计数大于零则提交失败。文件头注释自己承认这套方案有两个缺点：提交失败时无法告知是哪条外键、哪一行不满足；以及**数据库在事务打开时就已经存在外键违例**的情况下机制会失灵。
- **immediate 的计数器存在语句对象里**（`struct Vdbe`）。语句跑完后若该语句的 immediate 计数器大于零，则返回 `SQLITE_CONSTRAINT_FOREIGNKEY` 并回滚语句事务。
- **一个例外**：单行 INSERT 语句（不带触发器）不开启语句事务，因此改为**立刻抛异常**而不是走计数器。`src/fkey.c` 的 `fkLookupParent` 里对应的分支条件是 `!pFKey->isDeferred && !(pParse->db->flags & SQLITE_DeferFKs) && !pParse->pToplevel && !pParse->isMultiWrite`，命中时调 `sqlite3HaltConstraint(pParse, SQLITE_CONSTRAINT_FOREIGNKEY, OE_Abort, 0, P4_STATIC, P5_ConstraintFK)`。

四种 DML 场景对计数器的处理在文件头被编号为 I.1 / I.2 / D.1 / D.2：

| 场景 | 判定对象 | 计数器动作 |
|---|---|---|
| I.1 | 子表新行在父表找不到匹配 | 加一 |
| I.2 | 父表新行对应的子表行 | 减一 |
| D.1 | 子表被删行在父表找不到匹配 | 减一 |
| D.2 | 父表被删行对应的子表行 | 加一 |

UPDATE 需要走完上面四步，但**只对受影响列真正被改动的外键**做，列值必须在运行期比较。

**边界**（与页面清单给出的 boundary 一致）：本页负责外键的登记、级联动作的判定与触发时机；**不负责索引查找的实现**（那属于 F 组 B-tree），也**不负责触发器子系统的通用机制**（属于 k03）。级联动作虽然借用了 `Trigger` 结构，但触发器的注册、删除、通用执行路径不在本页范围内。

## 怎么实现的

### 外键对象的登记

外键用 `struct FKey` 表示（定义在 `src/sqliteInt.h`）。关键字段：

- `pFrom`：含 `REFERENCES` 子句的表（子表 / child）。
- `pNextFrom`：同一个子表上的下一个外键。
- `zTo`：被指向的父表名。
- `pNextTo` / `pPrevTo`：同一个父表上的外键链。
- `nCol`：外键列数。
- `isDeferred`：是否延迟到提交时检查（`u8`，取值只能是 0 或 1）。
- `aAction[2]`：`u8` 数组，**下标 0 是 ON DELETE 动作，下标 1 是 ON UPDATE 动作**。动作码取自 `src/sqliteInt.h` 的常量组：`OE_None`（0，无约束）、`OE_Restrict`（7）、`OE_SetNull`（8）、`OE_SetDflt`（9）、`OE_Cascade`（10）。
- `apTrigger[2]`：缓存的两个动作触发器，分别对应 `aAction[0]`（DELETE）与 `aAction[1]`（UPDATE）。
- `aCol[]`：`struct sColMap` 数组，`iFrom` 是子表列下标，`zCol` 是父表列名（为 NULL 时表示映射到父表主键）。

两张表之间的双向索引：

- **子表 → 父表**：`Table.u.tab.pFKey` 链表，`src/sqliteInt.h` 的注释写明"The list of all parents for child Table X is held at X.pFKey"。
- **父表 → 子表**：`Schema.fkeyHash` 哈希表，键是父表名。`src/fkey.c` 的 `sqlite3FkReferences` 就是 `sqlite3HashFind(&pTab->pSchema->fkeyHash, pTab->zName)`。

`sqlite3FkDelete` 负责摘链与释放：把 `FKey` 从 `fkeyHash` 里摘掉（链表首元素走 `sqlite3HashInsert`，其余走 `pPrevTo->pNextTo` 指针改写），调 `fkTriggerDelete` 释放 `apTrigger[0]` 与 `apTrigger[1]`，最后 `sqlite3DbFree(db, pFKey)`。

### sqlite3FkLocateIndex：找到父键的唯一索引

`sqlite3FkLocateIndex` 的职责是给外键找到"父表上被引用列的唯一索引"。返回值 0 表示成功，非 0 表示失败并把错误写进 `pParse`。

判定规则（读函数体确认）：

- **单列外键且映射到父表 INTEGER PRIMARY KEY 时直接返回 0**，此时 `*ppIdx` 与 `*paiCol` 保持为 0（调用方据此知道"父键就是 rowid"）。判断条件是 `pParent->iPKey>=0` 且外键首列名 `zKey` 与父表 `iPKey` 列名 `sqlite3StrICmp` 相等（显式映射），或 `zKey` 为 NULL（隐式映射到主键）。
- **复合外键才分配 `aiCol` 数组**，长度 `nCol`（即 `pFKey->nCol`），用 `sqlite3DbMallocRawNN` 分配。
- **候选索引必须同时满足**：列数相等（`pIdx->nKeyCol==nCol`）、是唯一索引（`IsUniqueIndex`，即 `pIdx->onError!=OE_None`）、不是部分索引（`pIdx->pPartIdxWhere==0`）。
- **显式列映射时**还要逐列校验：索引列下标 `iCol` 不得为负（负值代表表达式索引，直接放弃），索引使用的排序规则必须与该列的默认排序规则一致（默认取 `sqlite3StrBINARY`），且索引列名必须能在 `pFKey->aCol[j].zCol` 中匹配上。
- **找不到时的错误信息**是 `"foreign key mismatch - \"%w\" referencing \"%w\""`，且只在 `!pParse->disableTriggers` 时才写错误（`disableTriggers` 期间静默跳过）。

### sqlite3FkRequired：判断"这次 DML 要不要跑 FK 逻辑"

`sqlite3FkRequired` 被 DELETE / UPDATE / INSERT 的代码生成器调用，返回值有三态：

- **0**：不需要任何外键处理。
- **1**：需要外键处理。
- **2**：需要外键处理，且额外要求"先删旧行再插新行"（UPDATE 场景）。

判定分两种输入：

- **DELETE（`aChange==0`）**：只要 `sqlite3FkReferences(pTab)` 非空（该表是某些外键的父表）或 `pTab->u.tab.pFKey` 非空（该表含外键定义）就需要处理。
- **UPDATE（`aChange!=0`）**：
  - 遍历 `pTab->u.tab.pFKey`（子表侧），用 `fkChildIsModified` 判断子键列是否被改。若被改且**子表名与父表名同名**（`sqlite3_stricmp(pTab->zName, p->zTo)==0`，即自引用），返回值提升为 2。
  - 遍历 `sqlite3FkReferences(pTab)`（父表侧），用 `fkParentIsModified` 判断父键列是否被改。若被改，且 `SQLITE_FkNoAction` 标志未置且 `p->aAction[1]!=OE_None`（即 ON UPDATE 动作不是 NO ACTION），**直接 return 2**；若父表同时也是子表（`p->pFrom==pTab`），也提升为 2。

`fkChildIsModified` / `fkParentIsModified` 都靠 `aChange[]` 数组：`aChange[i]>=0` 表示第 i 列被本次 UPDATE 修改，`-1` 表示未修改；rowid 是否被改由独立的 `bChngRowid` 参数携带。

### sqlite3FkCheck：生成约束检查代码

`sqlite3FkCheck` 是外键检查的主入口。参数约定：`regOld` 与 `regNew` **恰好一个非零**（函数开头 `assert((regOld==0)!=(regNew==0))`），`regOld` 指向旧行数据寄存器数组（第一个是 rowid，后面依次是各列），`regNew` 指向新行数据。

前置短路：

- `if( (db->flags&SQLITE_ForeignKeys)==0 ) return;`——外键未开启时整个函数是空操作。
- `if( !IsOrdinaryTable(pTab) ) return;`——视图、虚表不参与。

主体分三段：

**第一段：子表侧（本表是外键的子表）**

外键被分成两轮处理：`for(iPass=0; iPass<2; iPass++)`，**第 0 轮处理 immediate，第 1 轮处理 deferred**，跳过条件是 `pFKey->isDeferred!=iPass`。文件里的理由写明：immediate 处理可能让 VM 中止，在没有语句日志的情况下必须先于 deferred 处理。

UPDATE 场景下有剪枝：`if( aChange && sqlite3_stricmp(pTab->zName, pFKey->zTo)!=0 && fkChildIsModified(...)==0 ) continue;`——子键没被改就跳过。

找到父表：`pParse->disableTriggers` 为真时用 `sqlite3FindTable`，否则用 `sqlite3LocateTable`。随后调 `sqlite3FkLocateIndex` 拿父键唯一索引。

**父表找不到时的降级路径**：`if( !pTo || sqlite3FkLocateIndex(...) )` 分支里，若 `isIgnoreErrors`（即 `pParse->disableTriggers`）为真且不是 OOM，则当 `pTo==0` 时生成一段代码：逐列检查旧行的子键是否非 NULL（`OP_IsNull`），只要有一列为 NULL 就跳过，否则发出 `OP_FkCounter` 把计数器减一。源码注释解释：这是 DROP TABLE 路径——SQLite 会先对被删表跑一次 `DELETE FROM xxx` 来检查外键，父表已缺失时按"父表为空"处理。

行级检查由 `fkLookupParent` 生成，分两个方向：

- `regOld!=0`（删子行）：调 `fkLookupParent(..., regOld, -1, bIgnore)`，nIncr 为负，语义是"删除子行消解了一次违例"。
- `regNew!=0 && !isSetNullAction(pParse, pFKey)`（插子行）：调 `fkLookupParent(..., regNew, +1, bIgnore)`。`isSetNullAction` 用来识别"当前正在编译的触发器其实就是本外键自己的 SET NULL 动作"，此时子键必然全为 NULL，不可能造成违例，整段扫描被省略。

**第二段：父表侧（本表被别的表引用）**

遍历 `sqlite3FkReferences(pTab)`。

- UPDATE 剪枝：`if( aChange && fkParentIsModified(pTab, pFKey, aChange, bChngRowid)==0 ) continue;`
- **单行 INSERT 到父表的特殊短路**：条件为 `!pFKey->isDeferred && !(db->flags & SQLITE_DeferFKs) && !pParse->pToplevel && !pParse->isMultiWrite`，注释写明"插入父表单行不可能造成（或修复）一个 immediate 外键违例"，直接 `continue`。
- 用 `sqlite3FkLocateIndex` 拿到本表上被引用的唯一索引；失败时同样走 `isIgnoreErrors` 分支。
- 把子表包成 `SrcList`（`sqlite3SrcListAppend`），供 `sqlite3WhereBegin` 使用。
- `regNew!=0` 时调 `fkScanChildren(..., regNew, -1)`：父表插行 → 扫子表 → 计数器减一。
- `regOld!=0` 时调 `fkScanChildren(..., regOld, 1)`：父表删行 → 扫子表 → 计数器加一。动作码取 `pFKey->aAction[aChange!=0]`（DELETE 取 `aAction[0]`，UPDATE 取 `aAction[1]`），但若 `db->flags & SQLITE_FkNoAction` 置位则强制改成 `OE_None`。

**第三段：may-abort 标志的设置**

在父表删行的扫描之后有条件地调 `sqlite3MayAbort(pParse)`：`!pFKey->isDeferred && ((eAction!=OE_Cascade && eAction!=OE_SetNull) || fkBeforeTriggersExist(pParse, pFKey, aChange!=0))`。两个例外写在源码注释里：

- 若适用 CASCADE 或 SET NULL，违例会被动作触发器纠正，不必置 may-abort。
- 若子表上存在 BEFORE 触发器，它可能用 `RAISE(IGNORE)` 阻止 SET NULL / CASCADE 动作生效，此时必须置 may-abort。

`fkBeforeTriggersExist` 用 `sqlite3TriggersExist` 查子表触发器，返回值里取 `TRIGGER_BEFORE` 位。

### fkLookupParent：编译"到父表里找一行"

`fkLookupParent` 生成查找父行的 VDBE 代码。关键分支：

- **nIncr<0 时先查计数器是否为零**：`sqlite3VdbeAddOp2(v, OP_FkIfZero, pFKey->isDeferred, iOk)`。注释解释：若当前没有任何待解决的违例，删除一行就不必去查父表。这条指令的语义在 `src/vdbe.c` 的 `OP_FkIfZero` 分支里：P1 非零时检查 `db->nDeferredCons==0 && db->nDeferredImmCons==0`，P1 为零时检查 `p->nFkConstraint==0 && db->nDeferredImmCons==0`。
- **子键含 NULL 则约束视为满足**：对每个外键列发一条 `OP_IsNull` 跳到 `iOk` 标签，直接跳过整个查找。NULL 语义在文件头注释里对应"constraint is considered satisfied"。
- **父键是父表 INTEGER PRIMARY KEY（`pIdx==0`）**：先把子键值 `OP_SCopy` 到临时寄存器，再 `OP_MustBeInt` 施加父键亲和性，然后 `OP_NotExists` 在父表游标上查找。若 `pTab==pFKey->pFrom && nIncr==1`（自引用且是插入方向），额外发一条 `OP_Eq` 比较 `regData` 与临时寄存器，防止"自己引用自己"时误报违例。
- **父键有唯一索引（`pIdx!=0`）**：发 `OP_OpenRead` 打开该索引，设 `P4KeyInfo`，把各子键值 `OP_Copy` 到临时寄存器数组，发 `OP_Affinity` 施加 `sqlite3IndexAffinityStr(pParse->db,pIdx)` 给出的亲和性串，再发 `OP_Found` 查找。自引用 + 插入方向时额外用 `OP_Ne`（带 `SQLITE_JUMPIFNULL`）排除当前行本身。
- **计数器的最终动作**：
  - 若命中"单行 INSERT 立刻抛异常"的条件，则发 `sqlite3HaltConstraint(pParse, SQLITE_CONSTRAINT_FOREIGNKEY, OE_Abort, 0, P4_STATIC, P5_ConstraintFK)`。
  - 否则发 `sqlite3VdbeAddOp2(v, OP_FkCounter, pFKey->isDeferred, nIncr)`。若 `nIncr>0 && pFKey->isDeferred==0`（即 immediate 约束计数加一），先调 `sqlite3MayAbort(pParse)`。

`OP_FkCounter` 的语义在 `src/vdbe.c`：P1 非零（deferred）时把 `db->nDeferredCons` 加 P2；P1 为零（immediate）时，若 `db->flags & SQLITE_DeferFKs` 置位则加到 `db->nDeferredImmCons`，否则加到语句私有的 `p->nFkConstraint`。

### fkScanChildren：编译"扫子表找引用行"

`fkScanChildren` 为每一对父子键列构造一个等值条件：`pLeft = exprTableRegister(pParse, pTab, regData, iCol)` 取父表行的寄存器值，`pRight` 是子表列引用，两者用 `fkParentChildEq` 组合成 `pLeft = pRight`，再用 `sqlite3ExprAnd` 串成 WHERE 子句。比较采用**父键列的排序规则与亲和性**。

`fkParentChildEq` 的三种构造：

- 若 `fkUsesLhsAffinity` 为真，直接返回 `sqlite3PExpr(pParse, TK_EQ, pLeft, pRight)`。
- 否则（`SQLITE_OMIT_CAST` 未定义时）额外构造 `pIn`：`pRight COLLATE <coll> IN (+pLeft, +CAST(pLeft AS NUMERIC))`。这个 IN 项**只在 `bComposite` 或 `fkChildKeyIsIndexed(pChild, iChildCol, zColl)` 成立时才生成**，注释解释：IN 项的作用是能驱动子表上的索引查找；不生成它就得对子表全表扫描并逐行求值。
- 最终表达式是 `pLeft = +pRight` 与 `pIn` 的 AND（`sqlite3ExprAnd`）。

`fkUsesLhsAffinity` 的规则（读函数体确认）：若 `aff1`（父列亲和性）是数值亲和性则返回 1；若 `aff2`（子列亲和性）是数值亲和性则返回 0；否则两者必为 TEXT 或 BLOB，返回 `(aff1==aff2 || aff1==SQLITE_AFF_BLOB)`。

**自引用排除**：若 `pTab==pFKey->pFrom && nIncr>0`，会在 WHERE 上追加排除当前行的条件。rowid 表用 `$current_rowid != rowid`（`TK_NE`）；WITHOUT ROWID 表用 `NOT( $current_a IS a AND $current_b IS b AND ... )`，其中比较的是索引的主键列（`pIdx->aiColumn[i]`）。

WHERE 子句通过 `sqlite3ResolveExprNames` 解析名字，再交给 `sqlite3WhereBegin` 生成循环体；循环体里发 `OP_FkCounter`（参数 `pFKey->isDeferred` 与 `nIncr`）。`nIncr<0` 时循环体外还会有一条 `OP_FkIfZero` 作为守卫。

### sqlite3FkActions：级联动作的执行

`sqlite3FkActions` 在 DELETE 或 UPDATE 完成后被调用，负责跑 ON DELETE / ON UPDATE 的级联动作：

```
for(pFKey = sqlite3FkReferences(pTab); pFKey; pFKey=pFKey->pNextTo){
  if( aChange==0 || fkParentIsModified(pTab, pFKey, aChange, bChngRowid) ){
    Trigger *pAct = fkActionTrigger(pParse, pTab, pFKey, pChanges);
    if( pAct ){
      sqlite3CodeRowTriggerDirect(pParse, pAct, pTab, regOld, OE_Abort, 0);
    }
  }
}
```

动作触发器由 `fkActionTrigger` 生成并**缓存在 `pFKey->apTrigger[iAction]`**（`iAction = (pChanges!=0)`，即 UPDATE 用下标 1、DELETE 用下标 0）。

`fkActionTrigger` 的构造过程：

- 动作码取 `pFKey->aAction[iAction]`；若 `db->flags & SQLITE_FkNoAction` 置位则强制改成 `OE_None`。
- 若动作是 `OE_Restrict` 且 `db->flags & SQLITE_DeferFKs` 置位，直接返回 0（不生成动作触发器）。
- `OE_None` 返回 NULL——注释写明 NO ACTION 不需要特殊处理，其代码由 `fkScanChildren` 生成。
- WHERE 条件形如 `OLD.<父键列> = <子键列>`，由 `fkParentChildEq` 构造。注释强调 `OLD.tToCol` 必须放在 `=` 左侧，这样比较才用父表的排序规则；并且显式给 `OLD.tToCol` 设了亲和性（`pLeft->affExpr = pTab->aCol[iToCol].affinity`），因为 OLD.* 列默认没有亲和性。
- UPDATE 动作还构造 WHEN 子句 `NOT(old.col1 IS new.col1 AND ... AND old.colN IS new.colN)`，即只有父键值真的变了才触发级联。
- **SET NULL / SET DEFAULT 的赋值列表**：
  - `OE_Cascade` 时新值取 `NEW.<父键列>`。
  - `OE_SetDflt` 时新值取子列的默认表达式（`sqlite3ColumnExprAuth`）；若该列是生成列（`COLFLAG_GENERATED`），则新值置 NULL（`pDflt = 0` 走 `sqlite3ExprAlloc(db, TK_NULL, 0, 0)` 分支）。
  - 其余（`OE_SetNull`）新值为 NULL。
- **`OE_Restrict` 的实现**不是触发器动作而是一段 `SELECT RAISE(...)`：构造 `TK_RAISE` 表达式，字符串固定为 `"FOREIGN KEY constraint failed"`，`pRaise->affExpr = OE_Abort`。
- 触发器对象的分配：`sqlite3DbMallocZero(db, sizeof(Trigger) + sizeof(TriggerStep))`，`TriggerStep` 直接放在 `Trigger` 结构之后（`pStep = pTrigger->step_list = (TriggerStep *)&pTrigger[1]`），且分配期间用 `DisableLookaside` / `EnableLookaside` 关掉 lookaside 缓冲。
- 触发器步骤的动作类型由 switch 决定：`OE_Restrict` → `TK_SELECT`；`OE_Cascade` 且是 DELETE 方向 → `TK_DELETE`；其余（含 `OE_Cascade` 的 UPDATE 方向）→ `TK_UPDATE`。
- 分配失败（`db->mallocFailed==1`）时调 `fkTriggerDelete` 释放并返回 0。

`sqlite3FkClearTriggerCache` 在模式变化时清空所有外键的 `apTrigger` 缓存（逐个 `fkTriggerDelete` 并置 0）。

### sqlite3FkDropTable：DROP TABLE 时的检查

`sqlite3FkDropTable` 在 DROP TABLE 时生成等价的 `DELETE FROM <tbl>` 来检查外键。触发条件：

- 该表是某外键的父表（`sqlite3FkReferences(pTab)!=0`），或
- 该表含 deferred 外键（遍历 `pTab->u.tab.pFKey` 找 `p->isDeferred` 或 `SQLITE_DeferFKs` 置位）。

删除过程中 `pParse->disableTriggers` 被置 1 再清零——注释写明"触发器被禁用，但外键动作不被禁用"。删除之后，若 `SQLITE_DeferFKs` 未置位，则发 `OP_FkIfZero` 检查 immediate 计数器，为零才继续；不为零则 `sqlite3HaltConstraint(..., SQLITE_CONSTRAINT_FOREIGNKEY, OE_Abort, 0, P4_STATIC, P5_ConstraintFK)`。注释解释了原因：语句事务无法回滚模式变更，所以必须在模式改动之前就中止。

### 真实调用方

| 调用点 | 位置 | 调用方式 |
|---|---|---|
| INSERT 编译 | `src/insert.c` 的 `sqlite3CompleteInsertion` 路径 | `if( db->flags & SQLITE_ForeignKeys ) sqlite3FkCheck(pParse, pTab, 0, regIns, 0, 0);`——只传 `regNew`。 |
| DELETE 编译 | `src/delete.c` 的 `sqlite3GenerateRowDelete` 路径 | `sqlite3FkCheck(pParse, pTab, iOld, 0, 0, 0);`，随后 `sqlite3FkActions(pParse, pTab, 0, iOld, 0, 0);`。 |
| UPDATE 编译 | `src/update.c` 的 `sqlite3Update` 路径 | 先 `sqlite3FkRequired(pParse, pTab, aXRef, chngKey)` 取 `hasFK`；再 `sqlite3FkCheck(pParse, pTab, regOldRowid, 0, aXRef, chngKey)`（旧行）、`sqlite3FkCheck(pParse, pTab, 0, regNewRowid, aXRef, chngKey)`（新行）、`sqlite3FkActions(pParse, pTab, pChanges, regOldRowid, aXRef, chngKey)`。 |
| DELETE 编译（是否需要复杂处理） | `src/delete.c` | `bComplex = pTrigger \|\| sqlite3FkRequired(pParse, pTab, 0, 0);` |
| INSERT 编译（是否需要触发器/外键计数） | `src/insert.c` | `regTrigCnt = pTrigger!=0 \|\| sqlite3FkRequired(pParse, pTab, 0, 0);` |
| DROP TABLE 编译 | `src/build.c` | `sqlite3FkDropTable(pParse, pName, pTab);` |
| `PRAGMA foreign_key_check` | `src/pragma.c` | 两次调 `sqlite3FkLocateIndex(pParse, pParent, pFK, &pIdx, ...)`，用 `OP_Found` 在父键索引上定位。 |

### 计数器在事务层面的落点

- **deferred 计数**存放在数据库句柄上：`src/sqliteInt.h` 里 `sqlite3` 结构有 `i64 nDeferredCons;`（注释"Net deferred constraints this transaction."）与 `i64 nDeferredImmCons;`（"Net deferred immediate constraints"）。
- **immediate 计数**存放在语句对象上：`src/sqliteInt.h` 里 `Vdbe` 结构有 `i64 nDeferredCons;`（"Number of deferred fk violations"）与 `i64 nDeferredImmCons;`（"Number of deferred imm fk."）；`OP_FkCounter` 里用的是 `p->nFkConstraint`。
- 打开外键的开关是 `SQLITE_ForeignKeys`（`src/sqliteInt.h` 中值为 `0x00004000`，注释"Enforce foreign key constraints"）；`SQLITE_DeferFKs`（值 `0x00080000`，"Defer all FK constraints"）把所有外键延迟；`SQLITE_FkNoAction`（值 `HI(0x00008)`，"Treat all FK as NO ACTION"）把所有外键动作降级为 NO ACTION。

## 关键取舍

- **用计数器而不是"记录哪一行违规"**。**源码事实：**文件头注释明确写出 deferred 方案的两个缺点——提交失败时无法告知是哪条约束、哪一行不满足；以及数据库在事务打开时已存在违例时机制会失灵。注释给出的理由是"这个方案似乎比替代方案更简单"。**架构推论：**换来的是实现面极小（只有一个整型计数加减），代价是错误信息里不含具体的表名与行号，用户拿到的是一句泛化的 `FOREIGN KEY constraint failed`。
- **immediate 用计数器 + 单行 INSERT 例外**。**源码事实：**`fkLookupParent` 里 immediate 计数器加在语句对象 `p->nFkConstraint` 上；但单行无触发器的 INSERT 不开语句事务，因此走 `sqlite3HaltConstraint` 直接中止。**架构推论：**这条特判把"语句级回滚"和"立刻失败"两种语义缝在一起，是为了让单行 INSERT 也能正确报出 `SQLITE_CONSTRAINT_FOREIGNKEY`。
- **immediate 约束先于 deferred 约束处理**。**源码事实：**`sqlite3FkCheck` 的外层循环是 `for(iPass=0; iPass<2; iPass++)`，跳过条件 `pFKey->isDeferred!=iPass`，即第 0 轮只处理 immediate、第 1 轮只处理 deferred；注释给的理由是 immediate 处理可能让 VM 中止，无语句日志时必须先做。**架构推论：**这个顺序约束让"立即失败"的语义保持优先，避免 deferred 计数器先被污染再中止。
- **外键检查编译成 VDBE，而不是运行期解释**。**源码事实：**`fkLookupParent` 与 `fkScanChildren` 都只生成 `OP_IsNull` / `OP_Found` / `OP_FkCounter` / `OP_FkIfZero` 等指令，检查逻辑分散在编译期生成的指令流里。**架构推论：**好处是同一份检查在批量语句与单行语句下复用同一套生成代码，运行时无解释开销；代价是父表结构变化后已编译语句可能失效，需要触发重编译（对应 b06-reprepare）。
- **级联动作用 Trigger 结构承载，但语义上不属于触发器子系统**。**源码事实：**`fkActionTrigger` 手工分配 `sizeof(Trigger) + sizeof(TriggerStep)` 并把 `TriggerStep` 放在 `Trigger` 之后，执行时调 `sqlite3CodeRowTriggerDirect` 而非通用的触发器路径；`sqlite3FkClearTriggerCache` 在模式变化时单独清这个缓存。**架构推论：**复用 Trigger 结构省掉了为级联另建一套执行引擎的工作，代价是 Trigger 结构的生命周期被两个子系统共同管理。
- **SET DEFAULT 遇到生成列时退化为置 NULL**。**源码事实：**`fkActionTrigger` 里 `if( pCol->colFlags & COLFLAG_GENERATED )` 时 `pDflt = 0`，新值落到 `TK_NULL` 分支。**架构推论：**生成列没有可写回的默认值语义，退化路径避免了往生成列里写值这个非法操作。
- **父表缺失时按"父表为空"降级**。**源码事实：**`sqlite3FkCheck` 里 `!pTo` 且 `isIgnoreErrors` 时生成 `OP_IsNull` 检查 + `OP_FkCounter` 减一的代码；注释说明这是 DROP TABLE 前跑 `DELETE FROM xxx` 的场景。**架构推论：**这条降级路径让 DROP TABLE 与普通 DELETE 复用同一套 FK 检查代码，不必为 DROP 单独写一份约束检查。
- **复合外键才付 `aiCol` 数组的代价**。**源码事实：**`sqlite3FkLocateIndex` 里 `nCol==1` 时直接早退（不分配数组），只有 `nCol>1` 才 `sqlite3DbMallocRawNN` 分配 `nCol*sizeof(int)`。**架构推论：**绝大多数外键是单列的，这条分支把常见路径的内存开销降为零。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/fkey.c | sqlite3FkLocateIndex | 为外键在父表上定位唯一索引：单列且映射到父表 INTEGER PRIMARY KEY 时早退返回 0；复合外键才分配 `aiCol` 映射数组；找不到时报 `"foreign key mismatch"`。 |
| src/fkey.c | sqlite3FkRequired | 供 DELETE / UPDATE / INSERT 代码生成器判断是否需要 FK 处理：返回 0（不需要）、1（需要）、2（需要且 UPDATE 必须先删旧行再插新行）。 |
| src/fkey.c | sqlite3FkCheck | 外键检查主入口：外键关闭时直接返回；按 immediate（第 0 轮）→ deferred（第 1 轮）两遍遍历子表侧外键，再遍历父表侧外键；子表侧用 `fkLookupParent` 找父行，父表侧用 `fkScanChildren` 扫子行。 |
| src/fkey.c | sqlite3FkActions | 执行级联动作：遍历引用本表的外键，用 `fkActionTrigger` 取出（并缓存）动作触发器，再调 `sqlite3CodeRowTriggerDirect` 编译执行。 |
| src/fkey.c | fkLookupParent | 生成"到父表找一行"的 VDBE 代码：子键含 NULL 则跳过；父键是 IPK 时用 `OP_MustBeInt` + `OP_NotExists`，有唯一索引时用 `OP_Affinity` + `OP_Found`；结尾决定发 `OP_FkCounter` 还是 `sqlite3HaltConstraint`。 |
| src/fkey.c | fkScanChildren | 生成"扫子表找引用行"的 VDBE 代码：按父键列逐列构造等值条件（用 `fkParentChildEq`），自引用时追加排除当前行的条件，最终发 `OP_FkCounter`。 |
| src/fkey.c | fkActionTrigger | 构造并缓存 ON DELETE / ON UPDATE 的动作触发器：SET NULL / SET DEFAULT / CASCADE 生成赋值列表，RESTRICT 生成 `SELECT RAISE('FOREIGN KEY constraint failed')`，NO ACTION 返回 NULL。 |
| src/fkey.c | fkParentChildEq | 构造父键与子键的等值比较：亲和性兼容时直接 `TK_EQ`；不兼容时生成 `pLeft = +pRight AND (pRight COLLATE coll IN (+pLeft, +CAST(pLeft AS NUMERIC)))`，IN 项仅在复合外键或子列有可用索引时生成。 |
| src/fkey.c | fkUsesLhsAffinity | 判定比较是否使用左操作数（父列）的亲和性：父列为数值亲和性返回 1；子列为数值亲和性返回 0；否则按 TEXT/BLOB 组合判定。 |
| src/fkey.c | fkChildIsModified / fkParentIsModified | 依据 `aChange[]` 数组与 `bChngRowid` 判断本次 UPDATE 是否改动了子键列 / 父键列。 |
| src/fkey.c | fkBeforeTriggersExist | 用 `sqlite3TriggersExist` 判断子表上是否存在 BEFORE 触发器（取 `TRIGGER_BEFORE` 位），决定是否置 may-abort 标志。 |
| src/fkey.c | isSetNullAction | 判断当前编译的触发器是否是本外键自己的 SET NULL 动作（比对 `pParse->pTrigCoded` 与 `pFKey->apTrigger[0/1]` 及 `pFKey->aAction[0/1]==OE_SetNull`）。 |
| src/fkey.c | sqlite3FkDropTable | DROP TABLE 时生成等价的 `DELETE FROM <tbl>` 做外键检查；`disableTriggers` 期间禁用触发器但保留外键动作；随后用 `OP_FkIfZero` 检查 immediate 计数器，非零则以 `SQLITE_CONSTRAINT_FOREIGNKEY` 中止。 |
| src/fkey.c | sqlite3FkReferences | 返回引用本表的所有外键：`sqlite3HashFind(&pTab->pSchema->fkeyHash, pTab->zName)`。 |
| src/fkey.c | sqlite3FkDelete | 释放外键：把 `FKey` 从 `fkeyHash` 摘链、删除 `apTrigger[0/1]` 两个动作触发器、释放 `FKey` 本体。 |
| src/fkey.c | sqlite3FkClearTriggerCache | 模式变化时清空全部外键的动作触发器缓存（`apTrigger[0]` 与 `apTrigger[1]` 置 0）。 |
| src/fkey.c | fkTriggerDelete | 释放动作触发器：`TriggerStep` 的 `pSrc` / `pWhere` / `pExprList` / `pSelect` 与 `Trigger.pWhen` 逐项释放，最后释放 `Trigger` 本体。 |
| src/sqliteInt.h | struct FKey | 外键对象定义：`pFrom` / `pNextFrom` / `zTo` / `pNextTo` / `pPrevTo` / `nCol` / `isDeferred` / `aAction[2]`（ON DELETE、ON UPDATE）/ `apTrigger[2]` / `aCol[]`（`iFrom` + `zCol`）。 |
| src/sqliteInt.h | OE_None / OE_Restrict / OE_SetNull / OE_SetDflt / OE_Cascade | 外键动作码常量，取值分别为 0 / 7 / 8 / 9 / 10，与 `OE_Rollback`(1)、`OE_Abort`(2)、`OE_Fail`(3)、`OE_Ignore`(4)、`OE_Replace`(5)、`OE_Update`(6)、`OE_Default`(11) 同属一组冲突解决动作码。 |
| src/sqliteInt.h | SQLITE_ForeignKeys / SQLITE_DeferFKs / SQLITE_FkNoAction | 三个连接级开关，值分别为 `0x00004000`、`0x00080000`、`HI(0x00008)`，分别控制"是否强制外键"、"是否全部延迟"、"是否把动作全部当 NO ACTION"。 |
| src/vdbe.c | OP_FkCounter | 按 P1 决定计数器落点：P1 非零加到 `db->nDeferredCons`；P1 为零且 `SQLITE_DeferFKs` 置位加到 `db->nDeferredImmCons`，否则加到语句私有的 `p->nFkConstraint`。 |
| src/vdbe.c | OP_FkIfZero | 计数器为零时跳转：P1 非零检查 `db->nDeferredCons==0 && db->nDeferredImmCons==0`，P1 为零检查 `p->nFkConstraint==0 && db->nDeferredImmCons==0`。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层（L0 common / L1 storage / L2 index / L3 query / L4 modalities）里没有外键这一层。若主核要补外键，值得抄的是 SQLite 在 `src/fkey.c` 里定下的三条边界：一是**判定与执行分离**——判定（`sqlite3FkRequired`）在编译期只回答"要不要处理"，真正的扫描与计数被编译成 VDBE 指令；二是**计数器语义与事务语义对齐**——deferred 计数挂在事务上、immediate 计数挂在语句上，提交与语句回滚分别读不同的计数；三是**降级路径要显式**——父表缺失时按"父表为空"处理，而不是直接报错。

**架构推论：** 本页是 AI 检索 SQLite 写入约束时的落点：SQLite 的主键 / 唯一 / 非空 / CHECK 走 `src/insert.c` 与 `src/build.c`（对应 k01），外键整体落在 `src/fkey.c` 这一个文件里，`sqlite3FkCheck` / `sqlite3FkActions` / `sqlite3FkRequired` / `sqlite3FkLocateIndex` 四个对外函数即其全部入口。凡是"外键为什么不生效"、"级联动作什么时候触发"、"deferred 外键为什么提交失败"这类问题，检索落点都应指向 `src/fkey.c` 与 `src/vdbe.c` 的 `OP_FkCounter` / `OP_FkIfZero`，而不是 B-tree 索引查找（F 组）或触发器通用机制（k03）。

## 参考

- SQLite 官方文档《Foreign Key Support》：<https://sqlite.org/foreignkeys.html>
- SQLite 官方文档《PRAGMA foreign_keys》（含 `foreign_key_check` 与 `foreign_key_list`）：<https://sqlite.org/pragma.html#pragma_foreign_keys>
- SQLite 官方 Fossil 源码 `src/fkey.c`（trunk，文件头注释含 deferred/immediate 方案说明与 I.1/I.2/D.1/D.2 定义）：<https://sqlite.org/src/file?name=src/fkey.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/sqliteInt.h`（trunk，`struct FKey` 与 `OE_*` 动作码定义）：<https://sqlite.org/src/file?name=src/sqliteInt.h&ci=trunk>
- GitHub 源码镜像 `src/fkey.c`：<https://github.com/sqlite/sqlite/blob/master/src/fkey.c>
- GitHub 源码镜像 `src/vdbe.c`（`OP_FkCounter` 与 `OP_FkIfZero` 的 VDBE 指令语义）：<https://github.com/sqlite/sqlite/blob/master/src/vdbe.c>
