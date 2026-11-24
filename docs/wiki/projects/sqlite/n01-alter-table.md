---
project: sqlite
feature: alter-table
page_type: feature
schema_version: 1
refs:
  - path: src/alter.c
    symbols: [sqlite3AlterRenameTable, sqlite3AlterRenameColumn, sqlite3AlterBeginAddColumn, sqlite3AlterFinishAddColumn, sqlite3AlterDropColumn]
---

# ALTER TABLE

## 一句话职责

`ALTER TABLE` 是 SQLite 里唯一能改动既有表模式的语句家族，它不新建表、不改表，而是**改写 `sqlite_schema`（`sqlite_master`）里那条 `CREATE TABLE` 文本**，再让数据库重新解析这段文本；只有 `ALTER TABLE ... DROP COLUMN` 还会额外重写底层 B-tree 的行记录。

## 机制是什么

SQLite 的表没有独立的「模式对象」——一个表的全部定义就是 `sqlite_schema` 里的一行：`type='table'`、`name` 是表名、`tbl_name` 是表名、`rootpage` 是根页号、`sql` 字段保存建表语句原文。`ALTER TABLE` 因此只有一条实现策略：**改 `sql` 字段**。

四个受支持的形态，各自的物理代价不同：

- **`RENAME TO`（改表名）**：不动 B-tree，只改 `sql` 文本里的表名，以及 `sqlite_schema` 的 `name` / `tbl_name` 两列，附带 `sqlite_sequence` 表里记录的行号计数器表名。
- **`RENAME COLUMN`（改列名）**：不动 B-tree，只改 `sql` 文本里的列名出现位置。
- **`ADD COLUMN`（加列）**：不动 B-tree 的既有行，但必须改 `sql` 文本把列定义插进 `CREATE TABLE` 的括号里；旧行读出来时新列取默认值。
- **`DROP COLUMN`（删列）**：既改 `sql` 文本，**又必须逐行重写 B-tree 里的记录**，把被删列的字段从每条 record 中抠掉。

三个重命名/加列形态能只改文本，是因为 SQLite 的行记录里存的是**列值的序列**而不是「列名 → 值」的映射：给记录追加一个字段，旧记录自然短一截；删一个字段则必须重排每个字段，所以 `DROP COLUMN` 独占了一条重写路径。

`ALTER TABLE` 还有一层容易被忽略的连带影响：模式变了，所有引用旧名字的对象（视图、触发器、索引、外键）里的 SQL 文本也必须同步改，否则它们会指向不存在的对象。SQLite 的做法是**把整个 schema 里每一段 `sql` 都重新 parse 一遍**，找出所有提到旧名字的 token 并替换。

## 怎么实现的

### 入口：五个 parser 动作

`src/parse.y` 里 `ALTER TABLE` 的语法动作直接调用 `src/alter.c` 的五个函数。`ADD COLUMN` 被拆成两段动作，因为列定义里的约束列表（`carglist`）在解析时还需要往表结构里加列：

| 语法规则 | 调用 |
|---|---|
| `cmd ::= ALTER TABLE fullname(X) RENAME TO nm(Z)` | `sqlite3AlterRenameTable(pParse, X, &Z)` |
| `alter_add(A) ::= ALTER TABLE fullname(X) ADD kwcolumn_opt nm(Y) typetoken(Z)` | `sqlite3AlterBeginAddColumn(pParse, X)`，随后 `sqlite3AddColumn(pParse, Y, Z)` |
| `cmd ::= alter_add(Y) carglist.` | `sqlite3AlterFinishAddColumn(pParse, &Y)` |
| `cmd ::= ALTER TABLE fullname(X) DROP kwcolumn_opt nm(Y)` | `sqlite3AlterDropColumn(pParse, X, &Y)` |
| `cmd ::= ALTER TABLE fullname(X) RENAME kwcolumn_opt nm(Y) TO nm(Z)` | `sqlite3AlterRenameColumn(pParse, X, &Y, &Z)` |

`alter_add` 动作里还调了 `disableLookaside(pParse)`——`ADD COLUMN` 期间解析器要往临时表结构里塞东西，lookaside 缓存的内存布局不能被复用。

### 内部 SQL 函数：文本改写的主力

`src/alter.c` 的 `sqlite3AlterFunctions()` 注册了一组 `INTERNAL_FUNCTION` 内部函数。它们全部以 `sqlite_` 前缀命名、只在内部使用（`src/sqliteInt.h` 的 `INTERNAL_FUNCTION` 宏），由 `src/func.c` 的 `sqlite3RegisterBuiltinFunctions()` 调用注册：

- `sqlite_rename_table`（7 参数，`renameTableFunc`）——把 `sql` 文本里的表名换成新表名。
- `sqlite_rename_column`（9 参数，`renameColumnFunc`）——把 `sql` 文本里的列名换成新列名。
- `sqlite_drop_column`（3 参数，`dropColumnFunc`）——从 `sql` 文本里删掉一列。
- `sqlite_rename_quotefix`（2 参数，`renameQuotefixFunc`）——把双引号字符串字面量改写成单引号形式。
- `sqlite_rename_test`（7 参数，`renameTableTest`）——重命名后逐条检查 schema 里的 SQL 是否还能正常 parse/解析符号。
- `sqlite_check_type`（4 参数，`checkTypeFunc`）——`STRICT` 表加列时检查默认值类型。
- `sqlite_fail`（2 参数，`failConstraintFunc`）——把参数 1 的文本当作错误消息抛出，错误码取参数 2。

这组函数是 `ALTER TABLE` 的真正引擎：外层的 `sqlite3AlterXxx` 函数负责校验和发指令，**文本改写本身发生在这些内部函数里**，而它们是通过 `sqlite3NestedParse` 生成的嵌套 UPDATE 语句被调用的。

### `sqlite3NestedParse`：用 SQL 驱动文本改写

`src/build.c` 的 `sqlite3NestedParse(pParse, zFormat, ...)` 把格式串格式化成一个 SQL 串，再递归调 parser 生成并执行嵌套语句。`src/alter.c` 里所有改 `sqlite_schema` 的动作都走这条路，例如 `sqlite3AlterRenameTable` 里那条：

```
UPDATE "main".sqlite_master SET
  sql = CASE WHEN type='index' AND name LIKE 'sqliteX_autoindex%%' ESCAPE 'X' THEN sql
       ELSE sqlite_rename_table(%Q, type, name, sql, %Q, %Q, %d) END,
  ...
```

注意 `'sqliteX_autoindex%%'`：`X` 是 `ESCAPE` 的转义字符，所以 `sqliteX_autoindex%%` 匹配的是字面量 `sqlite_autoindex%`。自动索引（`sqlite_autoindex_*`）没有自己的 `sql` 文本（它的 `sql` 列是 NULL），所以这里显式跳过。

### 文本改写的原理：重新 parse + token 映射

内部函数要改 `sql` 文本里的某个名字，做法不是字符串替换，而是**把 `sql` 重新交给 parser**：

1. `renameParseSql`（`src/alter.c`）准备一个临时的 `Parse` 对象，把 `p->eParseMode` 设为 `PARSE_MODE_RENAME`，调 `sqlite3RunParser(p, zSql)` 解析整段 SQL。解析前会检查 `sqlite3StrNICmp(zSql,"CREATE ",7)!=0` 就返回 `SQLITE_CORRUPT_BKPT`——因为 schema 里 `sql` 列只应存 `CREATE` 语句。解析后会清掉 `db->init.iDb`。
2. 解析器在 `PARSE_MODE_RENAME` 模式下会把每个 token 的原始位置记进 `Parse.pRename` 链表（`sqlite3RenameTokenMap`）。链表节点是 `struct RenameToken { const void *p; Token t; RenameToken *pNext; }`：`p` 指向产生这个 token 的语法树元素，`t` 是 token 本身（含指向原文的 `z` 指针）。
3. `renameTokenFind(pParse, pCtx, pPtr)` 在链表里按语法树元素地址查找对应的 `RenameToken`；找到时把它从 `pParse->pRename` 摘下来挂到 `RenameCtx.pList` 上，并 `pCtx->nList++`。
4. `renameEditSql` 拿到 `RenameCtx.pList` 后，按 token 在原文中的位置逐个替换，输出新文本。

`renameParseSql` 内部把 `db->flags` 临时或上 `SQLITE_Comments`（解析完恢复），保证注释被完整保留在 token 映射里。

`renameTokenFind` 的查找键是**语法树元素地址**，不是字符串——所以「表名 `t1`」和「列名 `t1`」不会互相误伤；同一个名字在不同语法位置是不同元素，映射表天然区分它们。

### `sqlite3AlterRenameTable`：改表名

`src/alter.c` 的 `sqlite3AlterRenameTable` 执行顺序：

1. `sqlite3LocateTableItem(pParse, 0, &pSrc->a[0])` 找到待改名的表；找不到直接跳 `exit_rename_table`。
2. `sqlite3SchemaToIndex` 拿到库序号 `iDb`，`zDb = db->aDb[iDb].zDbSName` 是库名（`main` / `temp` / 附加库名）。
3. `sqlite3NameFromToken(db, pName)` 把新表名的 token 转成 NUL 结尾字符串。
4. **重名检查**：`sqlite3FindTable(db, zName, zDb) || sqlite3FindIndex(db, zName, zDb) || sqlite3IsShadowTableOf(db, pTab, zName)` 任一命中就报 `"there is already another table or index with this name: %s"`。
5. `isAlterableTable(pParse, pTab)`：名字以 `"sqlite_"` 开头（`sqlite3StrNICmp(pTab->zName, "sqlite_", 7)==0`）、是 eponymous 虚表（`TF_Eponymous`）、或只读 shadow 表（`TF_Shadow` 且 `sqlite3ReadOnlyShadowTables`），任一命中报 `"table %s may not be altered"`。
6. `sqlite3CheckObjectName(pParse, zName, "table", zName, zDb)` 检查目标名是否合法。
7. 视图不能改名：`IsView(pTab)` 报 `"view %s may not be altered"`。
8. 授权检查 `sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0)`。
9. 虚表走 `sqlite3GetVTable` 拿到模块，若 `pModule->xRename` 非空则生成 `OP_VRename` 指令，把新名字作为 P4_VTAB 参数传下去，让虚表实现自己去改它占用的资源名。
10. `sqlite3MayAbort(pParse)`：声明本语句会抛异常（嵌套 SQL 里的内部函数会报错），让 VDBE 在合适的地方检查中断。
11. 统计旧表名的 UTF-8 字符数 `nTabName = sqlite3Utf8CharLen(zTabName, -1)`——后面拼 `sqlite_autoindex_` 名字时要用字符数而不是字节数。
12. 发出那条嵌套 UPDATE，同时改 `sql`、`tbl_name`、`name` 三列。`name` 列对 `sqlite_autoindex_*` 的处理是 `'sqlite_autoindex_' || %Q || substr(name,%d+18)`——`18` 是 `"sqlite_autoindex_"` 的字符长度。
13. 若 `sqlite3FindTable(db, "sqlite_sequence", zDb)` 命中（表用了 `AUTOINCREMENT`），发一条 `UPDATE ... sqlite_sequence set name = %Q WHERE name = %Q` 把行号计数器的表名也改掉。
14. 若 `iDb!=1`（改的不是 temp 库），还要改 temp 库里的视图和触发器定义：`UPDATE sqlite_temp_schema SET sql = sqlite_rename_table(...), tbl_name = ... WHERE type IN ('view', 'trigger')`。
15. `renameReloadSchema(pParse, iDb, INITFLAG_AlterRename)`：调 `sqlite3ChangeCookie` 递增 schema cookie，再 `sqlite3VdbeAddParseSchemaOp(pParse->pVdbe, iDb, 0, p5)` 让 VDBE 在语句末尾重新解析 schema。若 `iDb!=1` 还会顺带给 temp 库（`1`）加一个同样的 parse-schema 操作。
16. `renameTestSchema(pParse, zDb, iDb==1, "after rename", 0)`：改完之后把 schema 里每条 SQL 重新 parse 一遍，确认没有把对象改坏。

`INITFLAG_AlterRename` 是 `src/sqliteInt.h` 里 `INITFLAG_AlterMask`（`0x0007`）下的一个值 `0x0001`，注释是 "Reparse after a RENAME"。

`renameTestSchema` 本身不发 UPDATE，而是发一条 SELECT：`SELECT 1 FROM "%w".sqlite_master WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X' AND sql NOT LIKE 'create virtual%%' AND sqlite_rename_test(...) IS NULL`。也就是说 `sqlite_rename_test` 在 `SELECT` 里被逐行调用——每行 schema 记录都检查一遍。

### `sqlite3AlterRenameColumn`：改列名

`src/alter.c` 的 `sqlite3AlterRenameColumn`：

1. `sqlite3LocateTableItem` 找表。
2. `isAlterableTable` 排除系统表；`isRealTable(pParse, pTab, 0)` 排除视图和虚表——`isRealTable` 的 `iOp` 参数是索引，`azMsg[] = {"rename columns of", "drop column from", "edit constraints of"}`，`iOp=0` 对应 `"rename columns of"`，报错文本形如 `cannot rename columns of view "v1"`。
3. 授权检查 `sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0)`。
4. `sqlite3NameFromToken` 取旧列名，`sqlite3ColumnIndex(pTab, zOld)` 找列下标；`iCol<0` 报 `"no such column: \"%T\""`。
5. `renameTestSchema(pParse, zDb, iSchema==1, "", 0)` + `renameFixQuotes(pParse, zDb, iSchema==1)`：先把 schema 里的双引号字符串字面量统一改成单引号，避免后续的 token 替换把字符串内容误当成标识符。`renameFixQuotes` 发的是 `UPDATE ... SET sql = sqlite_rename_quotefix(%Q, sql) WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X' AND sql NOT LIKE 'create virtual%%'`。
6. `isRowidAlias(pParse, pTab, zOld, "rename from")` 和 `isRowidAlias(pParse, pTab, zNew, "rename to")`：`HasRowid(pTab) && sqlite3IsRowid(zCol)` 时报 `"cannot %s rowid alias: %s"`——`rowid`、`oid`、`_rowid_` 这三个别名不能改名。
7. `bQuote = sqlite3Isquote(pNew->z[0])`：新列名在原始 SQL 里是否带引号。`renameEditSql` 里如果 `bQuote==0` 且原 token 首字符是标识符字符，就用裸名替换；否则用加引号的形式（`sqlite3MPrintf(db, "\"%w\" ", zNew)`）。
8. 发出嵌套 UPDATE：`UPDATE "%w".sqlite_master SET sql = sqlite_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, %d) WHERE name NOT LIKE 'sqliteX_%%' ESCAPE 'X' AND (type != 'index' OR tbl_name = %Q)`。九个实参对应 `renameColumnFunc` 读的 `argv[0..8]`：`argv[0]` 是 `sql`，`argv[3]` 是库名，`argv[4]` 是表名，`argv[5]` 是列下标 `iCol`，`argv[6]` 是新列名，`argv[7]` 是 `bQuote`，`argv[8]` 是是否 temp。
9. 若 `iSchema!=1`，还要改 temp 库里的触发器和视图：`UPDATE temp.sqlite_master SET sql = sqlite_rename_column(sql, type, name, %Q, %Q, %d, %Q, %d, 1) WHERE type IN ('trigger', 'view')`。
10. `renameReloadSchema(pParse, iSchema, INITFLAG_AlterRename)`。
11. `renameTestSchema(pParse, zDb, iSchema==1, "after rename", 1)`——最后一个参数 `bNoDQS=1`，表示这一轮检查不允许 DQS（双引号字符串）怪癖。

`renameColumnFunc` 内部：先 `sqlite3FindTable(db, zTable, zDb)` 拿到表，`iCol>=pTab->nCol` 就直接返回；`zOld = pTab->aCol[iCol].zCnName` 拿到旧列名。然后按被解析出来的对象类型分派：

- **视图**（`IsView(sParse.pNewTable)`）：清掉 `SF_View` 标志后跑 `sqlite3SelectPrep`，再 `sqlite3WalkSelect` 遍历 SELECT 树。
- **普通表**（`IsOrdinaryTable`）：用 `sqlite3_stricmp(zTable, sParse.pNewTable->zName)` 判断 `bFKOnly`——名字不等说明这段 `sql` 只是引用了这个表（比如别的表的外键），这时只处理外键，不动列名。名字相等时：`renameTokenFind` 定位被改名列的 token、`&sParse.pNewTable->iPKey` 的 token、`sqlite3WalkExprList` 遍历 CHECK 约束和索引表达式、以及所有生成列的表达式。
- **索引**（`sParse.pNewIndex`）：遍历 `aColExpr` 和 `pPartIdxWhere`。
- **触发器**（`pParse.pNewTrigger`）：先 `renameResolveTrigger` 解析，然后遍历 `step_list`，对目标表是本表的步骤处理 UPSERT、IdList 和 ExprList。

`renameTableFunc` 的分派类似，但判断依据是「SQL 里出现的表名」：对视图、`sParse.pNewTable`、索引、触发器分别处理；`isLegacy = (db->flags & SQLITE_LegacyAlter)` 控制非 legacy 模式下还要遍历 CHECK 表达式和触发器体。改表名时还会改外键的 `zTo`（`renameTokenFind(&sParse, &sCtx, (void*)pFKey->zTo)`）。

两个内部函数都会临时把 `db->xAuth` 置 0（保存原值在局部变量 `xAuth` 里，函数末尾恢复），避免重新 parse schema 时反复触发授权回调。

### `sqlite3AlterBeginAddColumn` + `sqlite3AlterFinishAddColumn`：加列

`ADD COLUMN` 被拆成两段，因为列定义里的约束要逐条加到表结构上。

`sqlite3AlterBeginAddColumn`（在 `ALTER TABLE ... ADD <colname> <type>` 之后、约束列表之前调用）：

1. `sqlite3LocateTableItem` 找表。
2. 虚表报错 `"virtual tables may not be altered"`；视图报错 `"Cannot add a column to a view"`；`isAlterableTable` 排除系统表。
3. `sqlite3MayAbort(pParse)`。
4. 断言 `pTab->u.tab.addColOffset>0`——`addColOffset` 是 `CREATE TABLE ` 之后第一个左括号的位置，由 `src/build.c` 的 `sqlite3StartTable` 算出来：`p->u.tab.addColOffset = 13 + (int)(pCons->z - pParse->sNameToken.z)`。`13` 是 `"CREATE TABLE "` 的长度。
5. **复制一份表结构**放到 `pParse->pNewTable`：`sqlite3DbMallocZero(db, sizeof(Table))`，`pNew->nCol = pTab->nCol`，列数组按 `nAlloc = (((pNew->nCol-1)/8)*8)+8` 分配（向上取 8 的倍数），`memcpy` 复制 `Column` 数组，每列的 `zCnName` 用 `sqlite3DbStrDup` 重拷一份、`hName` 用 `sqlite3StrIHash` 重算哈希。
6. **表名加前缀**：`pNew->zName = sqlite3MPrintf(db, "sqlite_altertab_%s", pTab->zName)`。注释说明理由：用户表不允许以 `sqlite_` 开头，所以这个前缀保证临时名字不会与现有表冲突。
7. `pNew->u.tab.pDfltList = sqlite3ExprListDup(db, pTab->u.tab.pDfltList, 0)` 复制默认值表达式列表；`pNew->u.tab.addColOffset = pTab->u.tab.addColOffset` 复制偏移量。

`sqlite3AddColumn` 随后的解析会把新列追加到 `pParse->pNewTable->aCol` 末尾。

`sqlite3AlterFinishAddColumn`（在 `carglist` 解析完之后调用）：

1. `zTab = &pNew->zName[16]`——`16` 是 `"sqlite_altertab_"` 的长度，跳过前缀还原成原表名。
2. `pCol = &pNew->aCol[pNew->nCol-1]` 指向刚加的那一列；`pDflt = sqlite3ColumnExpr(pNew, pCol)` 取默认值表达式。
3. 授权检查 `sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, 0)`。
4. **约束检查**：
   - `pCol->colFlags & COLFLAG_PRIMKEY` → `"Cannot add a PRIMARY KEY column"`。
   - `pNew->pIndex` 非空 → `"Cannot add a UNIQUE column"`。
   - `isRowidAlias(pParse, pTab, pCol->zCnName, "add")` → 不能加 `rowid` 别名列。
   - 默认值必须能被 `sqlite3ValueFromExpr` 处理（不能是 `CURRENT_TIME` 这类运行时值）。不能处理时报 `"Cannot add a column with non-constant default"`——注意这个错误用 `sqlite3ErrorIfNotEmpty` 发出，即**表非空才报错**，表为空则允许加。
   - 默认值求值为 NULL 时，把 `pDflt` 置 0，后续逻辑按「无默认值」处理。
   - 开了 `SQLITE_ForeignKeys` 且新列带 REFERENCES 且默认值非 NULL → `"Cannot add a REFERENCES column with non-NULL default value"`（同样要求表为空）。
   - `pCol->notNull && !pDflt` → `"Cannot add a NOT NULL column with default value NULL"`（同样要求表为空）。
   - 生成列且是 `COLFLAG_STORED` → `"cannot add a STORED column"`（同样要求表为空）。
5. **改 `sql` 文本**：`alterRtrimFragment(db, (char*)pColDef->z, pColDef->n)` 把列定义尾部多余的空白、分号、`--` 注释、未闭合的 C 风格注释去掉（`alterRtrimFragment` 逐 token 扫描，保留正确闭合的 `/* */` 注释）。然后发出嵌套 UPDATE：
   ```
   UPDATE "main".sqlite_master SET
     sql = printf('%.<addColOffset>s, ',sql) || <新列定义>
       || substr(sql,1+length(printf('%.<addColOffset>s',sql)))
   WHERE type = 'table' AND name = <表名>
   ```
   用 `printf('%.Ns')` 而不是 `substr` 直接切，是因为 `substr` 按字符计而 `addColOffset` 是字节数，必须用 `printf` 做单位转换（源码注释明确说明这一点）。
6. **schema 版本处理**：`OP_ReadCookie` 读 `BTREE_FILE_FORMAT`，`OP_AddImm -2` 减 2，`OP_IfPos` 判断是否还大于 0（即当前版本 >= 3），是则 `OP_SetCookie` 写成 `3`。注释说明：只保证版本至少为 3，但**不从低于 3 的版本升到 4**，因为那会破坏已有的 DESC 索引。`BTREE_FILE_FORMAT` 是文件格式版本 cookie 的槽位。
7. `renameReloadSchema(pParse, iDb, INITFLAG_AlterAdd)` 重新加载 schema（`INITFLAG_AlterAdd` = `0x0003`，注释 "Reparse after an ADD COLUMN"）。
8. **约束回验**：满足下列任一条件时必须扫全表——
   - 新列带 CHECK 约束（`pNew->pCheck!=0`）；
   - 新列是 NOT NULL 的生成列（`pCol->notNull && isGen`）；
   - 表是 STRICT 的且新列是类型不为 ANY 的生成列（`isStrict && isGen && pCol->eCType!=COLTYPE_ANY`）。

   这时发一条 `SELECT ... FROM pragma_quick_check(%Q,%Q)`，根据 `quick_check` 的结果文本分派错误：`GLOB 'CHECK*'` → `"CHECK constraint failed"`，`GLOB 'non-* value in*'` → `"type mismatch on DEFAULT"`，其余 → `"NOT NULL constraint failed"`，都用 `sqlite_fail` 抛出。
9. 若不是上面三种情况，但有默认值且表是 STRICT 的且新列类型不是 ANY（`pDflt && isStrict && pCol->eCType>COLTYPE_ANY`），则用 `sqlite_check_type` 检查首行的 `typeof()` 值与期望类型是否一致。类型名表是 `static const char *azTypeof[] = {"any", "blob", "integer", "integer", "real", "text"}`，按 `pCol->eCType-1` 索引。`checkTypeFunc` 内部跑 `SELECT typeof("%w") FROM "%w"."%w" LIMIT 1`，值与类型名 `sqlite3_stricmp` 不等就返回 1，触发 `"type mismatch on DEFAULT"`。

### `sqlite3AlterDropColumn`：删列

`src/alter.c` 的 `sqlite3AlterDropColumn` 是唯一要动 B-tree 的分支。前半段：

1. `sqlite3LocateTableItem` 找表。
2. `isAlterableTable` 排除系统表；`isRealTable(pParse, pTab, 1)` 排除视图和虚表（`iOp=1` 对应 `"drop column from"`）。
3. `sqlite3ColumnIndex(pTab, zCol)` 找列下标，`iCol<0` 报 `"no such column: \"%T\""`。
4. `isRowidAlias(pParse, pTab, zCol, "drop")` 排除 rowid 别名。
5. **主键/唯一列不能删**：`pTab->aCol[iCol].colFlags & (COLFLAG_PRIMKEY|COLFLAG_UNIQUE)` → `"cannot drop %s column: \"%s\""`，`%s` 按 `COLFLAG_PRIMKEY` 命中与否取 `"PRIMARY KEY"` 或 `"UNIQUE"`。
6. **不能删到零列**：`pTab->nCol<=1` → `"cannot drop column \"%s\": no other columns exist"`。
7. 授权检查 `sqlite3AuthCheck(pParse, SQLITE_ALTER_TABLE, zDb, pTab->zName, zCol)`——注意这里第 4 个参数传的是被删的列名，而重命名/加列传的是 `0`。
8. `renameTestSchema` + `renameFixQuotes` 同上。
9. 嵌套 UPDATE：`UPDATE "%w".sqlite_master SET sql = sqlite_drop_column(%d, sql, %d) WHERE (type=='table' AND tbl_name=%Q COLLATE nocase)`，三个参数是库序号、原始 `sql` 文本、列下标。
10. `renameReloadSchema(pParse, iDb, INITFLAG_AlterDrop)`（`INITFLAG_AlterDrop` = `0x0002`）。
11. `renameTestSchema(pParse, zDb, iDb==1, "after drop column", 1)`。

`dropColumnFunc`（即 `sqlite_drop_column`）的实现：它重新 parse 那段 `CREATE TABLE`，拿到 `pTab->aCol[]`。若 `pTab==0 || pTab->nCol==1 || iCol>=pTab->nCol`，返回 `SQLITE_CORRUPT_BKPT`（源码注释说 "This can happen if the sqlite_schema table is corrupt"）。要删的列若不是最后一列（`iCol<pTab->nCol-1`），就取被删列和下一列的名字 token（`renameTokenFind(&sParse, 0, (void*)pTab->aCol[iCol].zCnName)` 与 `aCol[iCol+1].zCnName`），用下一列 token 的起始位置 `zEnd` 作为删除区间的末尾。若删的是最后一列，则从前一列的名字 token 开始用 `getConstraintToken` 逐 token 扫描直到遇到 `TK_COMMA`，把 `pCol->t.z` 回退一字节指到那个逗号；`zEnd = &zSql[pTab->u.tab.addColOffset]` 即 `CREATE TABLE` 的右括号位置。最后 `sqlite3MPrintf(db, "%.*s%s", pCol->t.z-zSql, zSql, zEnd)` 拼出删掉该列后的新 `CREATE TABLE` 文本。

后半段是**数据文件重写**，在 `pParse->nErr==0` 时执行：

1. `OP_DbLock` 加写锁。
2. 若被删列不是虚拟列（`(pTab->aCol[iCol].colFlags & COLFLAG_VIRTUAL)==0`），才开始重写：
   - `sqlite3OpenTable(pParse, iCur, iDb, pTab, OP_OpenWrite)` 打开写游标；`OP_Rewind` 回到表头。
   - 分配寄存器组：若有 rowid（`HasRowid(pTab)`），`OP_Rowid` 取出 rowid，`pParse->nMem += pTab->nCol` 预留每列一个寄存器；否则（WITHOUT ROWID 表）用 `sqlite3PrimaryKeyIndex(pTab)` 拿到主键索引，逐列 `OP_Column` 读出主键列，`nField = pPk->nKeyCol`。
   - 遍历所有列 `i`，跳过 `i==iCol` 和虚拟列。非 rowid 表时用 `sqlite3TableColumnToIndex(pPk, i)` 计算目标寄存器位置；若该列是主键列（`iPos<pPk->nKeyCol`）直接跳过。
   - 若 `i==pTab->iPKey`（整数主键列），写入 `OP_Null`——因为 rowid 别名列本身不存在于记录里；否则用 `sqlite3ExprCodeGetColumnOfTable` 生成取值代码，并 `sqlite3VdbeDeletePriorOpcode(v, OP_RealAffinity)` 去掉附带的 REAL affinity 转换（源码断言转换只在 `pTab->aCol[i].affinity==SQLITE_AFF_REAL` 时才出现）。
   - `nField==0` 时补一个 `OP_Null` 并置 `nField=1`（注释指向 dbsqlfuzz 的一个具体用例 `5f09e7bcc78b4954d06bf9f2400d7715f48d1fef`）。
   - `OP_MakeRecord` 用 `nField` 个寄存器拼出新记录。
   - 写回：rowid 表用 `OP_Insert`（`OP_Insert iCur, regRec, reg`，第三个参数是 rowid）；WITHOUT ROWID 表用 `OP_IdxInsert iCur, regRec, reg+1, pPk->nKeyCol`。
   - `sqlite3VdbeChangeP5(v, OPFLAG_SAVEPOSITION)` 给 `OP_Next` 加上「保存位置」标志，保证插入操作不破坏游标位置。
   - `OP_Next` 迭代，`sqlite3VdbeJumpHere(v, addr)` 回到 `OP_Rewind` 之后。

被删列是虚拟列（生成列）时不做数据重写——生成列的值不占记录空间。

## 关键取舍

- **改文本而不是改二进制模式**：SQLite 不维护一份独立的「编译后表结构」，`sqlite_schema.sql` 的原文就是唯一真相。这让 `ALTER TABLE` 的实现简化为「字符串改写」，也让 `PRAGMA writable_schema` 这类直接改 schema 文本的手段得以使用。代价是每次打开数据库都要重新 parse 一遍建表语句，且文本改写本身容易出错——这就是为什么 `sqlite_rename_test` 必须在每次重命名后把 schema 里每条 SQL 重新 parse 一遍做回归检查。
- **重新 parse 而不是字符串替换**：`sqlite_rename_column` / `sqlite_rename_table` 都走「重新 parse → 用语法树元素地址定位 token → 替换」。这保证了「字符串字面量里的同名字符串」不会被误改。代价是每次 `ALTER TABLE` 都要付一次完整 parse 的开销。
- **`ADD COLUMN` 不动 B-tree，`DROP COLUMN` 必须动**：`ADD COLUMN` 只改 `sql` 文本，旧记录短一截、新列取默认值，读的时候按列下标对齐即可；`DROP COLUMN` 要把每个 record 中间的字段抠掉，必须逐行重写整张表。代价是 `DROP COLUMN` 在大表上是 O(表大小) 的写放大。
- **`DROP COLUMN` 拒绝主键和唯一列**：`COLFLAG_PRIMKEY|COLFLAG_UNIQUE` 直接报错。这条限制来自记录布局：主键列和唯一约束列参与索引，删掉它们要连带重写索引，而 SQLite 选择不支持。
- **`DROP COLUMN` 不允许删到零列**：`pTab->nCol<=1` 就报 `"no other columns exist"`。
- **`ADD COLUMN` 的部分约束要求表为空**：非空表加列时，默认值必须常量、不能是 `NOT NULL` 且无默认、不能是带非 NULL 默认值的 REFERENCES 列、不能是 STORED 生成列。这三条错误都通过 `sqlite3ErrorIfNotEmpty` 发出——它生成 `SELECT sqlite_fail(%Q, 1) FROM "%w"."%w"`，表为空则这个 SELECT 返回零行，`sqlite_fail` 根本不执行；表非空才抛错。这样既保住了 ALTER 的原子性（不写数据文件），又不用把表清空。
- **`ADD COLUMN` 的回验要扫全表**：CHECK 约束、NOT NULL 生成列、STRICT 表的非 ANY 生成列，加完之后必须整表扫一遍（`pragma_quick_check`）。这是纯粹的代价换取正确性。
- **schema 版本只升到 3**：`sqlite3AlterFinishAddColumn` 里 `OP_SetCookie` 写死 `3`，注释明确说不能从低于 3 升到 4，因为那会破坏已有的 DESC 索引。
- **`sqlite_altertab_` 前缀做临时表名**：`sqlite3AlterBeginAddColumn` 用这个前缀构造临时的表结构副本。前缀带 `sqlite_` 是因为用户表不允许以此开头，天然不会撞名；`sqlite3AlterFinishAddColumn` 用 `&pNew->zName[16]` 把前缀切掉（`16` 是前缀长度）。
- **嵌套 SQL 驱动一切**：所有对 `sqlite_schema` 的修改都是 `sqlite3NestedParse` 发出的嵌套 UPDATE，内部改写交给 `sqlite_rename_table` / `sqlite_rename_column` / `sqlite_drop_column` / `sqlite_rename_quotefix` 这几个内部函数。这套「用 SQL 驱动模式变更」的架构让 ALTER 的逻辑可以复用 SQL 引擎自身的解析与执行能力，代价是语句准备开销。
- **授权回调在内部函数里被临时屏蔽**：`renameColumnFunc` / `renameTableFunc` / `renameQuotefixFunc` / `dropColumnFunc` / `checkTypeFunc` 都把 `db->xAuth` 临时置 0（保存到局部变量 `xAuth`，末尾恢复）。理由是这些函数会重新 parse schema 里的 SQL，如果授权回调仍然生效，每次内部 parse 都会重复触发授权。
- **虚表的改名交给模块自己处理**：`sqlite3AlterRenameTable` 只在 `pModule->xRename` 非空时才发 `OP_VRename`；模块没提供 `xRename` 就当作普通表处理。虚表的加列则一律拒绝（`"virtual tables may not be altered"`）。
- **`ALTER TABLE` 的语法入口全在 `parse.y`**：`src/alter.c` 本身不解析 SQL，只提供被 parser 动作调用的 API。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/alter.c | sqlite3AlterRenameTable | ALTER TABLE ... RENAME TO 的实现：重名检查、系统表/视图排除、授权检查、虚表 OP_VRename、嵌套 UPDATE 改写 sqlite_master 的 sql/name/tbl_name、sqlite_sequence 表名同步、renameReloadSchema + renameTestSchema 收尾 |
| src/alter.c | sqlite3AlterRenameColumn | ALTER TABLE ... RENAME COLUMN 的实现：isAlterableTable + isRealTable 排除视图虚表、sqlite3ColumnIndex 定位列、rowid 别名检查、renameFixQuotes 归一化引号、嵌套 UPDATE 调用 sqlite_rename_column、renameReloadSchema + renameTestSchema |
| src/alter.c | sqlite3AlterBeginAddColumn | ADD COLUMN 的前半段：排除虚表/视图/系统表、复制 Table 结构到 Parse.pNewTable（列数组按 8 的倍数分配）、表名加 sqlite_altertab_ 前缀、复制 addColOffset 与默认值表达式列表 |
| src/alter.c | sqlite3AlterFinishAddColumn | ADD COLUMN 的后半段：拒绝主键列/唯一列/rowid 别名列、校验默认值必须常量、alterRtrimFragment 清理列定义文本、用 printf+substr 把列定义插进 CREATE TABLE、schema 版本 cookie 升到 3、renameReloadSchema、必要时用 pragma_quick_check 与 sqlite_check_type 回验约束 |
| src/alter.c | sqlite3AlterDropColumn | ALTER TABLE ... DROP COLUMN 的实现：拒绝主键/唯一列、拒绝删到零列、嵌套 UPDATE 调用 sqlite_drop_column 改写 sql 文本、renameReloadSchema，随后生成 VDBE 代码逐行重写 B-tree 记录（OP_Rewind/OP_Column/OP_MakeRecord/OP_Insert 或 OP_IdxInsert） |
| src/alter.c | renameTableFunc | 内部函数 sqlite_rename_table 的实现：重新 parse 建表文本，按视图/表/索引/触发器分派，用 renameTokenFind 定位表名 token，isLegacy 模式控制是否遍历 CHECK 表达式与触发器体 |
| src/alter.c | renameColumnFunc | 内部函数 sqlite_rename_column 的实现：重新 parse 后按视图/普通表/索引/触发器分派，定位被改列名的 token、CHECK 约束、索引表达式、生成列表达式与外键列 |
| src/alter.c | dropColumnFunc | 内部函数 sqlite_drop_column 的实现：按列 token 位置算出要删除的文本区间，末列情形用 getConstraintToken 找逗号，输出删列后的 CREATE TABLE 文本 |
| src/alter.c | renameParseSql | 用 PARSE_MODE_RENAME 重新 parse 一段 CREATE 语句，产出 Parse.pRename 的 token 映射链；非 CREATE 开头返回 SQLITE_CORRUPT_BKPT |
| src/alter.c | renameTokenFind | 在 Parse.pRename 链表中按语法树元素地址查找 RenameToken，命中时摘到 RenameCtx.pList 并计数 nList |
| src/alter.c | renameEditSql | 按 RenameCtx.pList 逐 token 替换生成新的 SQL 文本；bQuote 为真或原 token 首字符非标识符字符时用加引号形式 |
| src/alter.c | renameTestSchema | 生成对 sqlite_master 逐行调用 sqlite_rename_test 的 SELECT，检查重命名后 schema 是否仍可解析 |
| src/alter.c | renameFixQuotes | 生成调用 sqlite_rename_quotefix 的 UPDATE，把 schema 里的双引号字符串字面量改写成单引号形式 |
| src/alter.c | renameReloadSchema | 递增 schema cookie 并用 sqlite3VdbeAddParseSchemaOp 安排 schema 重解析，p5 传 INITFLAG_Alter* 常量 |
| src/alter.c | isAlterableTable | 判断表是否可改：sqlite_ 前缀的系统表、eponymous 虚表、只读 shadow 表都报 "table %s may not be altered" |
| src/alter.c | isRowidAlias | 判断列名是否为 rowid 别名，是则报 "cannot %s rowid alias: %s" |
| src/alter.c | isRealTable | 判断目标是否为真实表；iOp 取 0/1/2 分别对应 rename columns of / drop column from / edit constraints of |
| src/alter.c | alterRtrimFragment | 逐 token 扫描列定义片段，剔除尾部空白、分号、-- 注释和未闭合的 C 风格注释，保留正确闭合的 /* */ |
| src/alter.c | sqlite3ErrorIfNotEmpty | 生成 SELECT sqlite_fail(%Q, 1) FROM db.tab：表非空才触发错误，用于「加列时表必须为空」的一组约束 |
| src/alter.c | sqlite3AlterFunctions | 注册 ALTER TABLE 用的内部函数：sqlite_check_type(4)、sqlite_rename_column(9)、sqlite_rename_table(7)、sqlite_rename_test(7)、sqlite_drop_column(3)、sqlite_rename_quotefix(2)、sqlite_drop_constraint(2)、sqlite_fail(2)、sqlite_add_constraint(3)、sqlite_find_constraint(2) |
| src/alter.c | checkTypeFunc | 内部函数 sqlite_check_type 的实现：跑 SELECT typeof(col) FROM db.tab LIMIT 1，比对类型名不一致则返回 1 |
| src/alter.c | failConstraintFunc | 内部函数 sqlite_fail 的实现：把参数 1 当错误消息抛出，错误码取参数 2 |
| src/parse.y | sqlite3AlterRenameTable | 语法动作：ALTER TABLE fullname RENAME TO nm 调用 sqlite3AlterRenameTable |
| src/parse.y | sqlite3AlterBeginAddColumn | 语法动作：alter_add 规则里先 disableLookaside 再调 sqlite3AlterBeginAddColumn 与 sqlite3AddColumn |
| src/parse.y | sqlite3AlterFinishAddColumn | 语法动作：cmd ::= alter_add(Y) carglist 调用 sqlite3AlterFinishAddColumn |
| src/parse.y | sqlite3AlterDropColumn | 语法动作：ALTER TABLE fullname DROP kwcolumn_opt nm 调用 sqlite3AlterDropColumn |
| src/parse.y | sqlite3AlterRenameColumn | 语法动作：ALTER TABLE fullname RENAME kwcolumn_opt nm TO nm 调用 sqlite3AlterRenameColumn |
| src/build.c | sqlite3NestedParse | 把格式串格式化成 SQL 后递归解析并执行，ALTER TABLE 用它发出所有改写 sqlite_schema 的嵌套 UPDATE/SELECT |
| src/build.c | sqlite3StartTable | 建表时计算 addColOffset（13 + 偏移），ADD COLUMN 用它定位 CREATE TABLE 的右括号 |
| src/func.c | sqlite3RegisterBuiltinFunctions | 注册内置函数时调用 sqlite3AlterFunctions，把 ALTER TABLE 的内部函数装进函数表 |
| src/sqliteInt.h | INITFLAG_AlterRename | 0x0001，重命名后重新解析 schema 的标志位（掩码 INITFLAG_AlterMask 为 0x0007） |
| src/sqliteInt.h | INITFLAG_AlterDrop | 0x0002，删除列后重新解析 schema 的标志位 |
| src/sqliteInt.h | INITFLAG_AlterAdd | 0x0003，添加列后重新解析 schema 的标志位 |
| src/sqliteInt.h | LEGACY_SCHEMA_TABLE | "sqlite_master"，ALTER TABLE 所有改写都作用在这张表上 |

## 与主核 / AI 的关系

**架构推论：** 主数据库内核（`engine/db/`）的 catalog 层与 query 层严格分离（L2 catalog 禁依赖 query，L3 query 禁依赖 modalities）。SQLite 的 `ALTER TABLE` 把「模式文本的存储」「模式文本的解析」「数据文件的重写」三件事挤在同一个文件 `src/alter.c` 里：模式定义是 `sqlite_master.sql` 的文本，模式解析走的是同一个 parser，数据重写直接生成 VDBE 指令操作 B-tree。主核若照抄「schema 存 SQL 文本」的做法，就等于让 catalog 层依赖 parser 与 VDBE，需要重新划边界——否则 DDL 路径会绕过 catalog 层的抽象直接摸存储层。

**架构推论：** SQLite 的 `DROP COLUMN` 是「改文本 + 逐行重写记录」两段式，主核的多模态表若也有需要删列的场景（例如删除向量列），这两段的代价模型完全不同：SQLite 的记录是定长字段序列，删一列要重排每条 record 的所有后续字段；多模态记录里向量列往往是变长的大对象，抠掉它等于整条记录重写。**架构推论：** 主核若提供删列能力，应当在存储层提供「按列裁剪记录」的原生操作，而不是在 SQL 编译层拼装重写逻辑。

**架构推论：** SQLite 把「模式变更的正确性验证」做成一条可复用的内部 SQL 函数（`sqlite_rename_test`），改完之后把整个 schema 重新 parse 一遍做回归检查。这个思路对 AI 驱动的系统尤其有价值：如果让模型生成或自动应用 DDL，同样需要在变更后跑一遍全量 schema 的可解析性校验，而不是只验证被改的那一条语句。

## 参考

- SQLite 官方文档：ALTER TABLE — https://sqlite.org/lang_altertable.html
- SQLite 官方文档：The SQLite File Format（schema 表与 `sqlite_schema` 的存储）— https://sqlite.org/fileformat2.html
- SQLite 官方文档：PRAGMA legacy_alter_table（`SQLITE_LegacyAlter` 标志的来源）— https://sqlite.org/pragma.html#pragma_legacy_alter_table
- GitHub 源码页：src/alter.c — https://github.com/sqlite/sqlite/blob/master/src/alter.c
- GitHub 源码页：src/parse.y（ALTER TABLE 的语法动作）— https://github.com/sqlite/sqlite/blob/master/src/parse.y
