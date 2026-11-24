---
project: sqlite
feature: schema-load
page_type: feature
schema_version: 1
refs:
  - path: src/prepare.c
    symbols: [sqlite3InitOne, sqlite3ReadSchema]
---

# 模式表加载

## 一句话职责

把数据库文件里的模式表（`sqlite_master`）读出来，在内存中重建表、索引、触发器、视图对象，让后续语句编译能拿到正确的模式信息。

## 机制是什么

SQLite 的模式（schema）不是单独存放的一张描述表，而是**由模式表里的 SQL 文本反解出来的**。每个表、索引、触发器、视图在 `sqlite_master`（TEMP 库用 `sqlite_temp_master`，见 `SCHEMA_TABLE` 宏）里占一行，列依次为 `type / name / tbl_name / rootpage / sql`。其中 `sql` 列保存着原始的 `CREATE` 语句文本——真正的模式信息就藏在这些文本里。

因此「加载模式」在 SQLite 里不是读一张结构化的描述表再填充字段，而是**把 `sqlite_master` 当成一份 SQL 脚本来重放**：对每一行取出 `sql` 列，重新交给解析器跑一遍，让解析器构造出 `Table`、`Index`、`Trigger`、`View` 这些内存对象。解析器在 `db->init.busy` 非零时只建数据结构、不生成也不执行 VDBE 代码，所以重放不会产生任何副作用。

加载的产物挂在 `db->aDb[iDb].pSchema` 上，其中 `tblHash` / `idxHash` / `trigHash` 三个哈希表分别存放表、索引、触发器对象；视图是以 `type='view'` 的形式存在 `tblHash` 里的表对象。

页面边界按机制清单的划分：只负责「启动时从 `sqlite_master` 读出模式并建好内存中的表/索引对象」，不负责模式的修改（`CREATE`/`ALTER`/`DROP` 属于 K/N 组），也不负责语句编译本身（属于 B04）。

## 怎么实现的

### 入口：`sqlite3ReadSchema`

`sqlite3ReadSchema` 是一层薄包装：

- 若 `db->init.busy` 已非零，直接返回 `SQLITE_OK`——说明当前正处于重放模式，不允许递归加载。
- 否则调用 `sqlite3Init(db, &pParse->zErrMsg)`；失败时把 `rc` 写入 `pParse->rc` 并令 `pParse->nErr++`。
- 成功后，若 `db->noSharedCache` 为真，置位 `db->mDbFlags |= DBFLAG_SchemaKnownOk`。

`sqlite3ReadSchema` 的调用方遍布各模块：`build.c` 的建表/建索引/删表/重建索引路径、`attach.c` 的 `ATTACH`、`alter.c`、`trigger.c`、`pragma.c`、`insert.c`、`analyze.c` 都会先调它。**这正是「模式按需加载」的机制**——不是连接一建立就全量加载，而是任何需要模式的操作先触发一次加载，之后靠 `DB_SchemaLoaded` 标记位跳过。

### 调度：`sqlite3Init`

`sqlite3Init` 负责遍历所有数据库文件并保证「主库先、其余逆序、temp 最后」：

1. 先 `ENC(db) = SCHEMA_ENC(db)`，把连接编码对齐到主库模式。
2. 断言 `db->init.busy==0` 且 `sqlite3BtreeHoldsMutex(db->aDb[0].pBt)`。
3. 若 `DbHasProperty(db, 0, DB_SchemaLoaded)` 为假，调 `sqlite3InitOne(db, 0, pzErrMsg, 0)`；返回非零则直接返回错误。
4. 从 `db->nDb-1` 逆序遍历到 1，对每个未加载的库调 `sqlite3InitOne(db, i, pzErrMsg, 0)`。逆序保证 TEMP 库（索引 1）最后处理。
5. 若 `commit_internal`（即 `db->mDbFlags` 没有 `DBFLAG_SchemaChange`），调 `sqlite3CommitInternalChanges(db)`。

`sqlite3Init` 的直接调用方：`main.c` 里 `sqlite3_last_insert_rowid` 之类的 API 在 `sqlite3BtreeEnterAll(db)` 之后调它；`attach.c` 在附加新库后调它。

### 主体：`sqlite3InitOne`

`sqlite3InitOne(db, iDb, pzErrMsg, mFlags)` 是真正干活的函数，按顺序做这些事：

**第一步，用解析器直接建出模式表自身。** 它把 `azArg[0..5]` 填成：

```
azArg[0] = "table"
azArg[1] = zSchemaTabName = SCHEMA_TABLE(iDb)
azArg[2] = azArg[1]
azArg[3] = "1"
azArg[4] = "CREATE TABLE x(type text,name text,tbl_name text,rootpage int,sql text)"
```

然后直接调 `sqlite3InitCallback(&initData, 5, (char **)azArg, 0)`。注释说得很清楚：表名交给解析器自动插入，所以这里用缩写 `"x"`；解析器也会自动把模式表标记为只读。

**第二步，处理无 btree 的库。** 若 `pDb->pBt==0`（`iDb==1`，即 TEMP），直接 `DbSetProperty(db, 1, DB_SchemaLoaded)` 并返回 `SQLITE_OK`。

**第三步，开事务。** `sqlite3BtreeEnter(pDb->pBt)` 后，若 `sqlite3BtreeTxnState(pDb->pBt)==SQLITE_TXN_NONE`，则 `sqlite3BtreeBeginTrans(pDb->pBt, 0, 0)` 打开一个只读事务，并置 `openedTransaction=1`。函数返回前若 `openedTransaction` 则 `sqlite3BtreeCommit(pDb->pBt)`，再 `sqlite3BtreeLeave(pDb->pBt)`。

**第四步，读元数据。** 用 `sqlite3BtreeGetMeta(pDb->pBt, i+1, ...)` 循环读 `ArraySize(meta)` 个值。`meta` 是 `int meta[5]`，即只读前 5 个头字段，其含义由 `btree.h` 的常量索引确定：

| meta 下标 | `btree.h` 常量 | 含义 |
| --- | --- | --- |
| 0 | `BTREE_SCHEMA_VERSION`（值 1） | 模式 cookie |
| 1 | `BTREE_FILE_FORMAT`（值 2） | 模式层文件格式 |
| 2 | `BTREE_DEFAULT_CACHE_SIZE`（值 3） | 建议页缓存大小 |
| 3 | （未用常量对应） | 最大 rootpage（auto/incremental_vacuum） |
| 4 | `BTREE_TEXT_ENCODING`（值 5） | 文本编码 |

若 `db->flags` 含 `SQLITE_ResetDatabase`，则 `memset(meta, 0, sizeof(meta))` 把这些值全清零。

接着：
- `pDb->pSchema->schema_cookie = meta[BTREE_SCHEMA_VERSION-1]`——这就是 A04 组「模式版本与缓存失效」用的那个 cookie。
- 若 `meta[BTREE_TEXT_ENCODING-1]` 非零：主库且 `DBFLAG_EncodingFixed` 未置位时，`encoding = (u8)meta[BTREE_TEXT_ENCODING-1] & 3`，`encoding==0` 则改用 `SQLITE_UTF8`，最后 `sqlite3SetTextEncoding(db, encoding)`；若是附加库，则 `(meta[BTREE_TEXT_ENCODING-1] & 3)!=ENC(db)` 时报错 `"attached databases must use the same text encoding as main database"`，`rc=SQLITE_ERROR`。
- 若 `pDb->pSchema->cache_size==0`：`SQLITE_OMIT_DEPRECATED` 未定义时取 `size = sqlite3AbsInt32(meta[BTREE_DEFAULT_CACHE_SIZE-1])`，`size==0` 则取 `SQLITE_DEFAULT_CACHE_SIZE`；否则直接用 `SQLITE_DEFAULT_CACHE_SIZE`。随后 `sqlite3BtreeSetCacheSize(pDb->pBt, pDb->pSchema->cache_size)`。`SQLITE_DEFAULT_CACHE_SIZE` 在 `sqliteLimit.h` 里默认定义为 `-2000`，负值表示「页数 × 负数 = 字节上限」。
- `pDb->pSchema->file_format = (u8)meta[BTREE_FILE_FORMAT-1]`，为 0 时置 1；若超过 `SQLITE_MAX_FILE_FORMAT`（`sqliteInt.h` 里定义为 `4`）则报 `"unsupported file format"`，`rc=SQLITE_ERROR`。
- 若 `iDb==0` 且 `meta[BTREE_FILE_FORMAT-1]>=4`，清掉 `db->flags` 里的 `SQLITE_LegacyFileFmt`（注释引的是 ticket #2804，避免 VACUUM 把库降级从而废掉用户建的 DESC 索引）。

**第五步，读模式表内容。** `initData.mxPage = sqlite3BtreeLastPage(pDb->pBt)` 拿到最大页号作为 rootpage 上界，然后拼出查询串：

```sql
SELECT*FROM"<dbname>".<schemaTable> [WHERE type<>'trigger'] ORDER BY rowid
```

`WHERE type<>'trigger'` 这段条件只在 `sqlite3BtreeIsReadonly(db->aDb[iDb].pBt)` 为真时拼接——只读库上不解析触发器（`sqlite3InitCallback` 里有对应断言）。执行前临时把 `db->xAuth` 置 0、执行后恢复，这样读模式表不触发授权回调；随后 `sqlite3_exec(db, zSql, sqlite3InitCallback, &initData, 0)`。若 `rc==SQLITE_OK` 则取 `rc = initData.rc`。

**第六步，加载统计信息。** `SQLITE_OMIT_ANALYZE` 未定义且 `rc==SQLITE_OK` 时，调 `sqlite3AnalysisLoad(db, iDb)`（实现在 `src/analyze.c`）。该函数先清掉所有表上的 `TF_HasStat1` 标志与索引上的 `hasStat1`，再从 `sqlite_stat1` 表读回统计，最后对没有 `hasStat1` 的索引调 `sqlite3DefaultRowEst(pIdx)`（实现在 `src/build.c`）填默认值。

**第七步，处理错误与置位。** 若 `db->mallocFailed`，置 `rc = SQLITE_NOMEM_BKPT` 并调 `sqlite3ResetAllSchemasOfConnection(db)`。否则，若 `rc==SQLITE_OK`，**或** `db->flags` 含 `SQLITE_NoSchemaError` 且 `rc!=SQLITE_NOMEM`，则调 `DbSetProperty(db, iDb, DB_SchemaLoaded)` 并把 `rc` 强制改回 `SQLITE_OK`。源码注释把这条称为「Hack」：即使出错（除 OOM），也认为模式已加载——本次 `sqlite3_prepare()` 仍会失败，但下一次 prepare 会拿出错之前已加载的那部分模式继续编译。目的是在 `sqlite_schema` 内容损坏时仍允许访问它。

**第八步，错误清理。** `error_out` 处：若 `rc` 非零，`SQLITE_NOMEM`/`SQLITE_IOERR_NOMEM` 时调 `sqlite3OomFault(db)`，然后调 `sqlite3ResetOneSchema(db, iDb, 0)`（`build.c`）把这次半途而废的模式状态清掉。最后 `db->init.busy = 0`，返回 `rc`。

### 逐行处理：`sqlite3InitCallback`

`sqlite3InitCallback` 是 `sqlite3_exec` 的回调，每行模式表记录调一次。参数约定写在函数注释里：

```
argv[0] = 对象类型: "table", "index", "trigger", 或 "view"
argv[1] = 对象名
argv[2] = 索引/触发器关联的表名
argv[3] = 表或索引的 rootpage；触发器/视图为 0
argv[4] = CREATE 语句的 SQL 文本
```

处理顺序：

1. `db->mDbFlags |= DBFLAG_EncodingFixed`；若 `argv==0` 直接返回 0（`EMPTY_RESULT_CALLBACKS` 打开时可能发生）。
2. `pData->nInitRow++`；若 `db->mallocFailed`，调 `corruptSchema` 并返回 1。
3. 若 `argv[3]==0`，调 `corruptSchema(pData, argv, 0)`。
4. 否则若 `argv[4]` 的首两个字符（经 `sqlite3UpperToLower` 转小写后比较）是 `'c'` 和 `'r'`，进入重放分支：
   - 断言 `db->init.busy` 非零，保存 `db->init.iDb` 后置为 `iDb`。
   - 用 `sqlite3GetUInt32(argv[3], &db->init.newTnum)` 解析 rootpage。该函数（`src/util.c`）只接受十进制表示，解析成功返回 1、失败返回 0 并把 `*pI` 置 0。所以源码里的判断 `sqlite3GetUInt32(...)==0` 即「解析失败」；再加上 `db->init.newTnum>pData->mxPage` 且 `pData->mxPage>0`（rootpage 超过库的最大页号）这一条，且 `sqlite3Config.bExtraSchemaChecks` 打开，则调 `corruptSchema(pData, argv, "invalid rootpage")`。
   - 置 `db->init.orphanTrigger = 0`，`db->init.azInit = (const char**)argv`。
   - 调 `sqlite3Prepare(db, argv[4], -1, 0, 0, &pStmt, &zEnd)` 解析这条 CREATE 语句——这才是真正建内存对象的地方。返回值 `rc = db->errCode`。
   - 恢复 `db->init.iDb = saved_iDb`。若 `rc != SQLITE_OK`：若 `db->init.orphanTrigger` 为真则仅断言 `iDb==1`；否则 `rc > pData->rc` 时更新 `pData->rc`，`rc==SQLITE_NOMEM` 时调 `sqlite3OomFault(db)`，`rc!=SQLITE_INTERRUPT` 且 `(rc&0xFF)!=SQLITE_LOCKED` 时调 `corruptSchema(pData, argv, sqlite3_errmsg(db))`。
   - 若 `rc==SQLITE_OK` 但 `zEnd[0]` 非空（CREATE 语句后面还有内容），同样调 `corruptSchema`。
   - `db->init.azInit = sqlite3StdType`，然后 `sqlite3_finalize(pStmt)`。
5. 否则若 `argv[1]==0`，或 `argv[4]!=0 && argv[4][0]!=0`，调 `corruptSchema`。走到这一支意味着「既没有 CREATE 文本、又不是内部索引」——正常情况下 `sql` 列为 NULL 的只有 `UNIQUE`/`PRIMARY KEY` 自动生成的内部索引。
6. 最后一支是内部索引：`pIndex = sqlite3FindIndex(db, argv[1], db->aDb[iDb].zDbSName)`（`build.c`，在 `idxHash` 里按名查找，先搜 TEMP 再搜 MAIN）。找不到则 `corruptSchema(pData, argv, "orphan index")`；找到则用 `sqlite3GetUInt32` 校验 rootpage（同样以返回 0 为失败），条件为：解析失败、`pIndex->tnum<2`（rootpage 1 恒为模式表，索引不能占）、`pIndex->tnum>pData->mxPage`、或 `sqlite3IndexHasDuplicateRootPage(pIndex)` 为真（同一张表上两个索引共用 rootpage）。满足且 `sqlite3Config.bExtraSchemaChecks` 打开时，报 `"invalid rootpage"`。
7. 最后统一检查对象总数：若 `pX->tblHash.count + pX->idxHash.count + pX->trigHash.count` 大于 `db->aLimit[SQLITE_LIMIT_SCHEMA]`，报 `"too many schema objects"`，`pData->rc = SQLITE_ERROR`，返回 1。`SQLITE_LIMIT_SCHEMA` 在 `sqlite.h.in` 里值为 13；其硬上限 `SQLITE_MAX_SCHEMA` 在 `sqliteLimit.h` 里定义为 `10000000`，`main.c` 里用 `assert( aHardLimit[SQLITE_LIMIT_SCHEMA]==SQLITE_MAX_SCHEMA )` 固定住这层关系。

### 错误文案：`corruptSchema`

`corruptSchema` 按优先级分四条支路：

1. `db->mallocFailed` → `pData->rc = SQLITE_NOMEM_BKPT`。
2. `pData->pzErrMsg[0]!=0` → 已有错误信息，不覆盖。
3. `pData->mInitFlags & INITFLAG_AlterMask` → 属 ALTER 重解析场景，按 `INITFLAG_AlterMask` 的低 3 位从 `azAlterType[]` 里取 `"rename"` / `"drop column"` / `"add column"` / `"drop constraint"`，拼成 `"error in %s %s after %s: %s"`，`pData->rc = SQLITE_ERROR`。
4. 否则：`db->flags & SQLITE_WriteSchema` 为真时 `pData->rc = SQLITE_CORRUPT_BKPT`；否则拼出 `"malformed database schema (%s)"`（对象名缺失时用 `"?"`），`zExtra` 非空时再追加 `" - %s"`，`pData->rc = SQLITE_CORRUPT_BKPT`。

`INITFLAG_AlterMask` 定义为 `0x0007`，具体子标志为 `INITFLAG_AlterRename`(0x0001)、`INITFLAG_AlterDrop`(0x0002)、`INITFLAG_AlterAdd`(0x0003)、`INITFLAG_AlterDropCons`(0x0004)。

### 另一条进入路径：`OP_ParseSchema`

`sqlite3InitCallback` 不只被 `sqlite3InitOne` 调用——函数注释明确写着「This routine is also called from the OP_ParseSchema opcode of the VDBE」。`vdbe.c` 的 `OP_ParseSchema` 分支有两种形态：

- `pOp->p4.z==0`（ALTER 场景）：先 `sqlite3ExpireStmtsUsingSchema(db, iDb, p)`、`sqlite3SchemaClear(db->aDb[iDb].pSchema)`、清 `DBFLAG_SchemaKnownOk`，然后调 `sqlite3InitOne(db, iDb, &p->zErrMsg, pOp->p5)`，并置 `db->mDbFlags |= DBFLAG_SchemaChange`。
- `pOp->p4.z!=0`：拼出 `SELECT*FROM"<db>".sqlite_master WHERE <p4> ORDER BY rowid`，令 `db->init.busy = 1` 后同样用 `sqlite3InitCallback` 执行。**若执行成功但 `initData.nInitRow==0`，直接置 `rc = SQLITE_CORRUPT_BKPT`**——注释说这种情况说明 `sqlite_schema` 表已损坏。

### `init.busy` 语义

`db->init.busy` 是加载模式的互斥标志。`sqlite3InitOne` 置 `db->init.busy = 1 + ((mFlags & INITFLAG_AlterAdd)!=0)`，即普通加载为 1，ALTER TABLE ADD COLUMN 重解析为 2。源码注释解释：`init.busy==2` 额外表示「这次扫描属于 ADD COLUMN，对函数名解析的检查更严格」。`sqlite3ReadSchema` 正是靠它来避免递归加载。

## 关键取舍

**用 SQL 文本重放，而不是读一张结构化描述表。** 代价是启动时要把每条 CREATE 语句重新解析一遍；收益是模式与「用户实际写的 DDL」保持单一事实来源，不需要维护两套表示，ALTER TABLE 之类对模式的修改只要重写 `sql` 列即可。代价是 `sqlite_master` 的 `sql` 列被篡改或损坏时，整个模式就加载不出来。

**加载是惰性的，按需触发。** `sqlite3ReadSchema` 只在需要时被调用，加载完用 `DB_SchemaLoaded` 标记跳过重复加载。代价是第一次触发的那个语句要多付一次全表扫描的代价。

**`SQLITE_NoSchemaError` 的容错旁路。** 模式损坏时仍置 `DB_SchemaLoaded` 并把 `rc` 改回 `SQLITE_OK`，让下一次 prepare 拿「已加载的部分模式」继续。这是为了在 `sqlite_schema` 损坏时仍能访问该表本身。代价是错误被推迟到下一次 prepare 才暴露。

**`SQLITE_OMIT_DEPRECATED` 影响默认缓存大小来源。** 定义了它就完全不读文件头里的 `BTREE_DEFAULT_CACHE_SIZE`，直接用编译期常量 `SQLITE_DEFAULT_CACHE_SIZE`；未定义时优先用文件头记录，为 0 才回退到该常量。

**额外的模式一致性检查默认关闭。** `sqlite3IndexHasDuplicateRootPage`（同一表上两索引共用 rootpage）、rootpage 越界等检查，都只在 `sqlite3Config.bExtraSchemaChecks` 打开时才升级为错误，否则静默放过。这是在「检查开销」与「容忍损坏文件」之间的取舍。

**只读库跳过触发器解析。** 读模式表的 SQL 串在只读库上会附加 `WHERE type<>'trigger'`，因为触发器里可能引用无法在只读库上解析的内容。

**回退到 `sqlite3ResetOneSchema` 清理半途失败的状态。** `sqlite3InitOne` 的 `error_out` 分支在 `rc` 非零时调 `sqlite3ResetOneSchema(db, iDb, 0)`（实现在 `build.c`）。该函数把 `DB_ResetWanted` 标记到 `db->aDb[iDb].pSchema` 和 `db->aDb[1].pSchema`，清 `DBFLAG_SchemaKnownOk`，置 `db->bPendingSchemaReset = 1`；若 `db->nSchemaLock==0` 则立即调 `sqlite3ExpireStmtsUsingSchema(db, i, pSafe)` 并 `sqlite3SchemaClear(db->aDb[i].pSchema)` 真正清空对应 schema。调用方 `sqlite3ResetOneSchema` 的调用者是 `sqlite3InitOne` 自身与 `build.c` 里的其他路径。

**回退到 `sqlite3AnalysisLoad` 的默认值填充。** 模式加载完成后立即补统计信息；`sqlite_stat1` 里没有的索引由 `sqlite3DefaultRowEst(pIdx)` 填默认估算值，而不是留空。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/prepare.c | `sqlite3InitOne` | 单个数据库文件的模式加载主体：建模式表对象、开只读事务、读头元数据、跑 `SELECT*FROM sqlite_master`、置 `DB_SchemaLoaded` |
| src/prepare.c | `sqlite3ReadSchema` | 对外入口：模式未加载时调 `sqlite3Init`，失败时写 `pParse->rc` 与 `pParse->nErr` |
| src/prepare.c | `sqlite3Init` | 遍历所有数据库文件，主库优先、TEMP 最后，逐个调 `sqlite3InitOne` |
| src/prepare.c | `sqlite3InitCallback` | `sqlite3_exec` 的逐行回调：识别对象类型、重放 CREATE 语句建内存对象、校验 rootpage、统计对象总数 |
| src/prepare.c | `corruptSchema` | 模式损坏时的错误文案与错误码选择（OOM / ALTER 上下文 / `SQLITE_WriteSchema` / 一般损坏） |
| src/prepare.c | `sqlite3IndexHasDuplicateRootPage` | 检查同一张表上是否有两个索引共用同一 rootpage |
| src/prepare.c | `schemaIsValid` | 逐库比对 schema cookie，与内存中的 `schema_cookie` 不符则置 `pParse->rc = SQLITE_SCHEMA` 并 `sqlite3ResetOneSchema` |
| src/vdbe.c | `OP_ParseSchema` | 另一条进入 `sqlite3InitCallback` 的路径：ALTER 场景转调 `sqlite3InitOne`，带条件场景直接跑一条 SELECT |
| src/sqliteInt.h | `SCHEMA_TABLE` / `LEGACY_SCHEMA_TABLE` / `LEGACY_TEMP_SCHEMA_TABLE` | 模式表名常量：`sqlite_master` 与 `sqlite_temp_master`（TEMP 库 iDb==1 时用后者） |
| src/sqliteInt.h | `INITFLAG_AlterMask` / `INITFLAG_AlterRename` / `INITFLAG_AlterDrop` / `INITFLAG_AlterAdd` / `INITFLAG_AlterDropCons` | `mInitFlags` 的 ALTER 重解析类型标记，用于 `corruptSchema` 选择错误文案 |
| src/sqliteInt.h | `SQLITE_MAX_FILE_FORMAT` | 可接受的最大模式层文件格式号（4），超过则报 `"unsupported file format"` |
| src/sqliteInt.h | `DB_SchemaLoaded` | schema 的 `schemaFlags` 位，置位表示该库模式已加载，用于跳过重复加载 |
| src/sqliteInt.h | `DbSetProperty` / `DbHasProperty` | `schemaFlags` 的置位与查询宏 |
| src/sqliteInt.h | `InitData` | `sqlite3Init` 与 `OP_ParseSchema` 传给 `sqlite3InitCallback` 的上下文结构（db / pzErrMsg / iDb / rc / mInitFlags / nInitRow / mxPage） |
| src/btree.h | `BTREE_SCHEMA_VERSION` / `BTREE_FILE_FORMAT` / `BTREE_DEFAULT_CACHE_SIZE` / `BTREE_TEXT_ENCODING` | 文件头元数据索引常量（1 / 2 / 3 / 5），决定 `meta[]` 各下标的含义 |
| src/btree.c | `sqlite3BtreeGetMeta` | 按索引读出文件头 4 字节元数据（`idx>=0 && idx<=15`） |
| src/sqliteLimit.h | `SQLITE_MAX_SCHEMA` | 模式对象数量硬上限（10000000） |
| src/sqliteLimit.h | `SQLITE_DEFAULT_CACHE_SIZE` | 默认建议缓存大小（-2000，负值表示字节上限） |
| src/build.c | `sqlite3FindIndex` | 在 `idxHash` 中按名查找索引对象，回调用它定位内部索引 |
| src/build.c | `sqlite3ResetOneSchema` | 模式加载失败时的清理入口：置 `DB_ResetWanted`、清 `DBFLAG_SchemaKnownOk`、必要时 `sqlite3SchemaClear` |
| src/analyze.c | `sqlite3AnalysisLoad` | 模式加载后加载 `sqlite_stat1` 统计，并对无统计的索引调 `sqlite3DefaultRowEst` |
| src/build.c | `sqlite3DefaultRowEst` | 为 `sqlite_stat1` 里缺失的索引填默认行数估算 |
| src/attach.c | `sqlite3Init` 调用点 | `ATTACH` 新库后触发该库模式加载 |
| src/main.c | `sqlite3Init` 调用点 | 部分公共 API 在 `sqlite3BtreeEnterAll` 后触发模式加载 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的 catalog 层负责的是「运行期目录对象」——表、列、索引的运行时结构与事务内的可见性；SQLite 这一层负责的是「把模式从磁盘文本还原成内存对象」。两者在职责划分上可对照：SQLite 的 `sqlite_master` + `sqlite3InitCallback` 相当于主核的持久化目录与目录重建入口，差别在于 SQLite 的目录就是 DDL 文本本身。

**架构推论：** AI 侧若要生成 DDL 或做模式变更，直接改 `sqlite_master` 的 `sql` 列是危险的——下一次 `sqlite3InitOne` 会把这段文本重放给解析器，解析失败即触发 `corruptSchema`。正确入口是走 `ALTER TABLE` / `CREATE` 这类正常 DDL 路径，让 SQLite 自己写回 `sql` 列。

## 参考

- [SQLite Database File Format（模式层与 sqlite_schema 的官方定义）](https://sqlite.org/fileformat2.html)
- [SQLite Architecture（官方架构总览，说明解析器与模式的关系）](https://sqlite.org/arch.html)
- [sqlite/sqlite 源码页：src/prepare.c](https://github.com/sqlite/sqlite/blob/master/src/prepare.c)
- [sqlite/sqlite 源码页：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
