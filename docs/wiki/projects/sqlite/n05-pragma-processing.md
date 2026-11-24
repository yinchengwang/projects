---
project: sqlite
feature: pragma-processing
page_type: feature
schema_version: 1
refs:
  - path: src/pragma.c
    symbols: [sqlite3Pragma, sqlite3GetBoolean]
---

# PRAGMA 处理

## 一句话职责

把一条 `PRAGMA [schema.]name(=value)` 语句解析成「查表 → 定位 → 分派」，并为每个具体 PRAGMA 生成对应的 VDBE 代码或直接改写连接/分页器状态；不负责这些参数的实际消费方（各层），也不负责模式变更本身（N01）。

## 机制是什么

PRAGMA 是 SQLite 特有的 SQL 扩展，用与普通语句相同的接口发出，但语义上不是「查询表」而是「读写库内部状态」：页大小、页缓存大小、日志模式、锁模式、同步级别、自动 vacuum、堆上限、模式 cookie，以及 `table_info` / `index_list` / `integrity_check` 这类自省查询。

`src/pragma.c` 把它实现成**一张查表 + 一个 switch**：

1. **查表**：`aPragmaName[]` 是一张按名字字典序排列的静态表，字段为 `PragmaName` 结构体（由 `tool/mkpragmatab.tcl` 生成到 `src/pragma.h`）。每条记录给出：名字 `zName`、处理类别 `ePragTyp`（`PragTyp_XXX`）、标志位 `mPragFlg`（`PragFlg_XXX` 的按位或）、结果列名在 `pragCName[]` 中的起点与个数、额外参数 `iArg`。
2. **定位**：`pragmaLocate` 在这张有序表上做**二分查找**（`sqlite3_stricmp` 大小写不敏感比较）。表为什么有序——`src/pragma.c` 头部注释写明「pragma.h 是自动生成文件，确保 `aPragmaName[]` 按字典序排列以便二分查找」，且 `tool/mkpragmatab.tcl` 的注释明确要求「新增 PRAGMA 要改 `tool/mkpragmatab.tcl`，再重新生成 `pragma.h`」。
3. **分派**：`sqlite3Pragma` 里的 `switch( pPragma->ePragTyp )` 按类别跳到对应 `case PragTyp_XXX:` 处理代码。同一类别下的一批 PRAGMA 共用一个处理分支（例如 `PragTyp_HEADER_VALUE` 同时覆盖 `schema_version` / `user_version` / `application_id` / `data_version`，靠 `pPragma->iArg` 区分读哪个 cookie）。

**标志位**（含义取自 `tool/mkpragmatab.tcl` 里 `flagMeaning` 数组的原文）：

| 标志 | 含义（原文） | 作用点 |
| --- | --- | --- |
| `PragFlg_NeedSchema` | Force schema load before running | `sqlite3Pragma` 里置位时先调 `sqlite3ReadSchema(pParse)` |
| `PragFlg_ReadOnly` | Read-only HEADER_VALUE | `PragTyp_HEADER_VALUE` 分支里带参数时不写 cookie |
| `PragFlg_Result0` | Acts as query when no argument | 决定是否注册同名表值函数 |
| `PragFlg_Result1` | Acts as query when has one argument | 同上 |
| `PragFlg_SchemaReq` | Schema required - "main" is default | 表值函数是否带 `schema` 隐藏列 |
| `PragFlg_SchemaOpt` | Schema restricts name search if present | 同上 |
| `PragFlg_NoColumns` | OP_ResultRow called with zero columns | 不注册结果列名 |
| `PragFlg_NoColumns1` | zero columns if RHS argument is present | 带参数时不出结果行 |

**边界**（与页面清单给出的 boundary 一致）：本页负责 PRAGMA 语句的分派与各类参数的读写；不负责这些参数的实际消费方（各层），不负责模式变更本身（N01）。

## 怎么实现的

### 语法入口：五条产生式

`src/parse.y` 的 PRAGMA 段有五条产生式，全部调用 `sqlite3Pragma`：

```
cmd ::= PRAGMA nm(X) dbnm(Z).                {sqlite3Pragma(pParse,&X,&Z,0,0);}
cmd ::= PRAGMA nm(X) dbnm(Z) EQ nmnum(Y).    {sqlite3Pragma(pParse,&X,&Z,&Y,0);}
cmd ::= PRAGMA nm(X) dbnm(Z) LP nmnum(Y) RP. {sqlite3Pragma(pParse,&X,&Z,&Y,0);}
cmd ::= PRAGMA nm(X) dbnm(Z) EQ minus_num(Y).
                                             {sqlite3Pragma(pParse,&X,&Z,&Y,1);}
cmd ::= PRAGMA nm(X) dbnm(Z) LP minus_num(Y) RP.
                                             {sqlite3Pragma(pParse,&X,&Z,&Y,1);}
```

参数含义：`X` 是左边的名字 token，`Z` 是「库名」部分，`Y` 是值 token，最后一个参数 `minusFlag` 表示值前面带负号。括号形式 `PRAGMA x(y)` 与等号形式 `PRAGMA x=y` 落到同一分支。

### 分派主体：sqlite3Pragma

`sqlite3Pragma(Parse *pParse, Token *pId1, Token *pId2, Token *pValue, int minusFlag)` 的执行顺序如下，每一步都能在函数体里对上：

**第 1 步：取 VDBE，准备寄存器。**
`Vdbe *v = sqlite3GetVdbe(pParse);` 后 `if( v==0 ) return;`，再 `sqlite3VdbeRunOnlyOnce(v);`（保证只执行一次的语句不被重复编译）与 `pParse->nMem = 2;`。

**第 2 步：解析 `[schema.]` 前缀。**
`iDb = sqlite3TwoPartName(pParse, pId1, pId2, &pId);`，`iDb<0` 直接返回。`sqlite3TwoPartName`（在 `src/build.c`）的逻辑是：`pName2->n>0`（即写了库名）时用 `sqlite3FindDb(db, pName1)` 查库索引，查不到就 `sqlite3ErrorMsg(pParse, "unknown database %T", pName1)` 并返回 -1；否则 `iDb = db->init.iDb`、`*pUnqual = pName1`。紧接着 `pDb = &db->aDb[iDb];`。

**第 3 步：显式点名 temp 库时确保它已打开。**
`if( iDb==1 && sqlite3OpenTempDatabase(pParse) ){ return; }`。

**第 4 步：取名字与值。**
`zLeft = sqlite3NameFromToken(db, pId);`；值侧：带负号时 `zRight = sqlite3MPrintf(db, "-%T", pValue);`，否则 `zRight = sqlite3NameFromToken(db, pValue);`。

**第 5 步：授权检查。**
`if( sqlite3AuthCheck(pParse, SQLITE_PRAGMA, zLeft, zRight, zDb) ){ goto pragma_out; }`，其中 `zDb = pId2->n>0 ? pDb->zDbSname : 0;`（只在显式写了库名时才有）。

**第 6 步：先给 VFS 一个接管机会。**
`aFcntl[0]=0; aFcntl[1]=zLeft; aFcntl[2]=zRight; aFcntl[3]=0;` 然后 `rc = sqlite3_file_control(db, zDb, SQLITE_FCNTL_PRAGMA, (void*)aFcntl);`。

- `rc==SQLITE_OK`：认为 VFS 已经处理了这个 pragma，用 `sqlite3VdbeSetNumCols(v, 1)`、`sqlite3VdbeSetColName(v, 0, COLNAME_NAME, aFcntl[0], SQLITE_TRANSIENT)` 和 `returnSingleText(v, aFcntl[0])` 生成一条文本结果，然后 `goto pragma_out`。
- `rc!=SQLITE_NOTFOUND`（既不是 OK 也不是 NOTFOUND）：报错路径——`aFcntl[0]` 非空时 `sqlite3ErrorMsg(pParse, "%s", aFcntl[0])`，然后 `pParse->nErr++; pParse->rc = rc;`。

`SQLITE_FCNTL_PRAGMA` 的值是 14，定义在 `src/sqlite.h.in`。`src/sqlite.h.in` 里该 file control 的文档写明：VFS 可以新增 PRAGMA 或覆盖内置 PRAGMA 的含义；返回 `SQLITE_NOTFOUND` 则继续走正常处理。

**第 7 步：查表定位。**
`pPragma = pragmaLocate(zLeft);`。**查不到不报错**——源码注释引了需求编号 `R-43042-22504`「未知 pragma 不产生错误消息」，直接 `goto pragma_out`。这与官方文档《PRAGMA Statements》第 3 条一致：未知 pragma 被静默忽略，拼写错误不会被发现。

**第 8 步：按标志决定是否先加载模式、按标志决定是否注册结果列名。**
- `if( (pPragma->mPragFlg & PragFlg_NeedSchema)!=0 ){ if( sqlite3ReadSchema(pParse) ) goto pragma_out; }`
- `if( (pPragma->mPragFlg & PragFlg_NoColumns)==0 && ((pPragma->mPragFlg & PragFlg_NoColumns1)==0 || zRight==0) ){ setPragmaResultColumnNames(v, pPragma); }`

`setPragmaResultColumnNames` 的行为：`n = pPragma->nPragCName;` 若 `n==0` 则 `sqlite3VdbeSetNumCols(v, 1)` 且列名用 pragma 名本身（`SQLITE_STATIC`）；否则按 `iPragCName` 起点逐个取 `pragCName[j]` 设列名（同样 `SQLITE_STATIC`）。

**第 9 步：switch 分派。**
`switch( pPragma->ePragTyp )` 后是一长串 `case PragTyp_XXX:`。

### 典型分支：三类写法

**（一）改连接级状态，不产生结果行。** `PragTyp_FLAG`（覆盖 `foreign_keys` / `query_only` / `writable_schema` / `recursive_triggers` / `trusted_schema` 等一批布尔开关）：

```
if( zRight==0 ){
  setPragmaResultColumnNames(v, pPragma);
  returnSingleInt(v, (db->flags & pPragma->iArg)!=0 );
}else{
  u64 mask = pPragma->iArg;
  if( db->autoCommit==0 ){
    mask &= ~(SQLITE_ForeignKeys);
  }
  if( sqlite3GetBoolean(zRight, 0) ){
    if( (mask & SQLITE_WriteSchema)==0 || (db->flags & SQLITE_Defensive)==0 ){
      db->flags |= mask;
    }
  }else{
    db->flags &= ~mask;
    ...
  }
  sqlite3VdbeAddOp0(v, OP_Expire);
  setAllPagerFlags(db);
}
```

关键点：不在自动提交模式下，`SQLITE_ForeignKeys` 位被从掩码里剔除——源码注释写明「非自动提交模式下不能启停外键支持」。`SQLITE_WriteSchema`（`writable_schema`）在 `SQLITE_Defensive` 模式下被拒。末尾的 `OP_Expire` 用来让已编译语句失效，源码注释说明原因：「很多 flag-pragma 会改变 SQL 编译器生成的代码（例如 count_changes），所以改完 pragma 值要加一条 opcode 让所有已编译语句过期」。`setAllPagerFlags` 把 `pDb->safety_level` 与 `db->flags & PAGER_FLAGS_MASK` 合并后逐库调 `sqlite3BtreeSetPagerFlags`。

**（二）改 b-tree / pager 状态并回读。** `PragTyp_CACHE_SIZE`：

```
assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
if( !zRight ){
  returnSingleInt(v, pDb->pSchema->cache_size);
}else{
  int size = sqlite3Atoi(zRight);
  pDb->pSchema->cache_size = size;
  sqlite3BtreeSetCacheSize(pDb->pBt, pDb->pSchema->cache_size);
}
```

注意：`cache_size` 只改**当前连接**的 `pDb->pSchema->cache_size`，不写文件。而 `PragTyp_DEFAULT_CACHE_SIZE`（已废弃，走 `OP_SetCookie` 写 `BTREE_DEFAULT_CACHE_SIZE` cookie）才是持久化路径——`BTREE_DEFAULT_CACHE_SIZE` 的值是 3，定义在 `src/btree.h`。

`PragTyp_PAGE_SIZE` 的写法不同：设值时先记 `db->nextPagesize = sqlite3Atoi(zRight);`，再 `sqlite3BtreeSetPageSize(pBt, db->nextPagesize,0,0)`；若返回 `SQLITE_NOMEM` 则 `sqlite3OomFault(db)`。源码注释写明分配失败的原因：设置页大小时 pager 模块要调 `sqlite3_realloc()` 调整内部缓冲区。

**（三）生成 VDBE 代码，在执行期读回。** `PragTyp_JOURNAL_MODE`：

- 无值时 `eMode = PAGER_JOURNALMODE_QUERY`（值为 -1，定义在 `src/pager.h`）。
- 有值时用 `sqlite3JournalModename` 循环比对模式名（`sqlite3JournalModename` 返回小写名数组，顺序为 `delete`、`persist`、`off`、`truncate`、`memory`，未定义 `SQLITE_OMIT_WAL` 时再加 `wal`；对应的 `PAGER_JOURNALMODE_DELETE`=0 到 `PAGER_JOURNALMODE_WAL`=5，定义在 `src/pager.h`）。匹配不上就退化为查询。
- `eMode==PAGER_JOURNALMODE_OFF` 且 `db->flags & SQLITE_Defensive` 时，退化为查询——源码注释写明理由：「防御模式下不允许 journal-mode 为 OFF，因为关掉日志后普通 SQL 就能把库搞坏」。
- `PRAGMA journal_mode` 不带库名时，改写成 `PRAGMA main.journal_mode`（`iDb = 0; pId2->n = 1;`）。
- 最后对每个库生成 `OP_JournalMode`：`sqlite3VdbeAddOp3(v, OP_JournalMode, ii, 1, eMode);`，再 `sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);`。

`PragTyp_SYNCHRONOUS` 的写入路径带事务检查：`if( !db->autoCommit )` 时 `sqlite3ErrorMsg(pParse, "Safety level may not be changed inside a transaction")`；`iDb!=1`（不是 temp 库）时才真正改，`iLevel = (iLevel+1) & PAGER_SYNCHRONOUS_MASK;`（掩码值 0x07，定义在 `src/pager.h`），且 `if( iLevel==0 ) iLevel = 1;`。读取侧回 `pDb->safety_level-1`。

`PragTyp_INTEGRITY_CHECK` 的错误上限来自常量：`mxErr = SQLITE_INTEGRITY_CHECK_ERROR_MAX;`，该宏在 `src/pragma.c` 内 `#ifndef` 定义时取 **100**；若用户传了整数参数且 `mxErr<=0`，则退回同一个默认值；若参数不是整数，则被当作表名做局部检查（`sqlite3LocateTable`）。

`PragTyp_OPTIMIZE` 的分析上限来自常量 `SQLITE_DEFAULT_OPTIMIZE_LIMIT`，在 `src/pragma.c` 顶部 `#ifndef` 定义时取 **2000**；源码注释写明这个 2000 是实测选出来的，目标是让最坏情况下 `PRAGMA optimize` 在 Raspberry Pi 4 上不超过 100 毫秒。默认掩码是 `0xfffe`；带参数时 `opMask = (u32)sqlite3Atoi(zRight)`，且 `if( (opMask & 0x02)==0 ) break;`——没置 `0x02` 位就直接退出，不做任何事。表大小变化阈值 `const LogEst iRange = 33;`（源码注释标为「10x size change」）。末尾还有一次按 btree 数量缩放：`if( !db->mallocFailed && nLimit>0 && nBtree>100 ){ nLimit = 100*nLimit/nBtree; if( nLimit<100 ) nLimit = 100; }`。

### 布尔字面量解析：sqlite3GetBoolean

`sqlite3GetBoolean(const char *z, u8 dflt)` 只有一行函数体：`return getSafetyLevel(z,1,dflt)!=0;`。它复用内部静态函数 `getSafetyLevel(const char *z, int omitFull, u8 dflt)`：

```
static const char zText[] = "onoffalseyestruextrafull";
static const u8 iOffset[] = {0, 1, 2,  4,    9,  12,  15,   20};
static const u8 iLength[] = {2, 2, 3,  5,    3,   4,   5,    4};
static const u8 iValue[] =  {1, 0, 0,  0,    1,   1,   3,    2};
```

这张表共 8 项，依次是 `on`(值 1)、`no`(0)、`off`(0)、`false`(0)、`yes`(1)、`true`(1)、`extra`(3)、`full`(2)。

- 若首字符是数字（`sqlite3Isdigit(*z)`），直接 `return sqlite3Atoi(z)!=0;`。
- 否则按 `iLength`/`iOffset` 逐项用 `sqlite3StrNICmp` 匹配，命中且（`omitFull` 为 0 或该项值 `<=1`）时返回 `iValue[i]`。
- 全不匹配则返回调用方给的 `dflt`。

`omitFull=1`（`sqlite3GetBoolean` 传的就是 1）意味着 **`full` 与 `extra` 不被接受为布尔真值**——`getSafetyLevel` 的注释写明这是因为「FULL 和 EXTRA 选项在 omitFull 参数为 1 时不允许」，`full` 的返回值 2 与 `extra` 的 3 都 `>1`，被条件 `iValue[i]<=1` 挡掉。

`sqlite3GetBoolean` 不只服务于 PRAGMA。`src/pragma.c` 的注释写明：「`sqlite3GetBoolean()` 被其他模块使用，但本文件其余部分是 PRAGMA 处理专用的。」仓内实际调用方：`src/pragma.c` 内 4 处（`PragTyp_SECURE_DELETE`、`PragTyp_CACHE_SPILL`、`PragTyp_FLAG`、`PragTyp_CASE_SENSITIVE_LIKE`），以及 `src/main.c` 的 `sqlite3_uri_boolean`——该函数体是 `bDflt = bDflt!=0; return z ? sqlite3GetBoolean(z, bDflt) : bDflt;`。

`getSafetyLevel` 的另一半用途是 `PragTyp_SYNCHRONOUS`：那里传 `omitFull=0`，所以 `full`/`extra` 是合法的同步级别。

### 表值函数形式：pragma_* 虚拟表

同一份 `aPragmaName[]` 表还被复用为**表值函数**。`sqlite3PragmaVtabRegister(sqlite3 *db, const char *zName)` 的函数体：

```
assert( sqlite3_strnicmp(zName, "pragma_", 7)==0 );
pName = pragmaLocate(zName+7);
if( pName==0 ) return 0;
if( (pName->mPragFlg & (PragFlg_Result0|PragFlg_Result1))==0 ) return 0;
assert( sqlite3HashFind(&db->aModule, zName)==0 );
return sqlite3VtabCreateModule(db, zName, &pragmaVtabModule, (void*)pName, 0);
```

即：只有同时带 `PragFlg_Result0` 或 `PragFlg_Result1` 标志的 pragma 才注册成表值函数。调用方是 `src/build.c`——在虚表名解析路径里 `pMod = sqlite3PragmaVtabRegister(db, zName);`。

`pragmaVtabFilter` 的做法是**把表值函数调用翻译回一条 PRAGMA SQL 再 prepare**：它拼出 `"PRAGMA "` + 可选的 `"schema."` + pragma 名 + 可选的 `"=参数"`，然后 `rc = sqlite3_prepare_v2(pTab->db, zSql, -1, &pCsr->pPragma, 0);`，最后 `return pragmaVtabNext(pPragmaCursor);` 推进第一行。`pragmaVtabNext` 里 `if( SQLITE_ROW!=sqlite3_step(pCsr->pPragma) ){ rc = sqlite3_finalize(pCsr->pPragma); pCsr->pPragma = 0; pragmaVtabCursorClear(pCsr); }`。

`pragmaVtabConnect` 用 `sqlite3_declare_vtab` 声明列：按 `pPragma->iPragCName`/`nPragCName` 取 `pragCName[]` 里的列名；若 `nPragCName==0` 则用 pragma 名本身作为唯一列名；`PragFlg_Result1` 置位时追加 `,arg HIDDEN`；`PragFlg_SchemaOpt|PragFlg_SchemaReq` 置位时追加 `,schema HIDDEN`。`pragmaVtabBestIndex` 用 `pTab->nHidden==0` 提前返回；第一个隐藏参数没有 `==` 约束时把 `estimatedCost` 设成 `2147483647`（源码注释：隐藏参数无约束就返回高代价），有约束时设成 `20`。

### 编译期裁剪

`src/pragma.c` 整个文件被 `#if !defined(SQLITE_OMIT_PRAGMA)` 包住（从 `sqlite3GetBoolean` 之后的注释处开始），文件末尾 `#endif /* SQLITE_OMIT_PRAGMA */` 收尾。源码注释解释了这个切分的设计意图：「`sqlite3GetBoolean()` 被其他模块使用，但本文件其余部分是 PRAGMA 处理专用的。所以如果构建里裁掉 PRAGMA，就省掉本文件其余部分。」——**`sqlite3GetBoolean` 因此保留在裁剪之外**。

更细的裁剪按能力分组：`SQLITE_OMIT_PAGER_PRAGMAS`（分页器类 PRAGMA）、`SQLITE_OMIT_AUTOVACUUM`、`SQLITE_OMIT_FLAG_PRAGMAS`、`SQLITE_OMIT_SCHEMA_PRAGMAS`、`SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS`、`SQLITE_OMIT_INTROSPECTION_PRAGMAS`、`SQLITE_OMIT_DEPRECATED`、`SQLITE_OMIT_WAL`、`SQLITE_OMIT_FOREIGN_KEY`、`SQLITE_OMIT_VIRTUALTABLE`。

## 关键取舍

**未知 PRAGMA 静默忽略。** `pragmaLocate` 查不到时直接 `goto pragma_out`，不产生错误消息（源码注释引 `R-43042-22504`）。官方文档把这一条列为 PRAGMA 语义的固有特征，并写明没有改严的计划——理由是既有大量依赖当前宽松行为的存量应用。代价是拼写错误静默失效，官方文档也承认这是「bad design」但选择不改。

**表驱动 + 二分查找，而不是大 if-else。** 新增 PRAGMA 要改 `tool/mkpragmatab.tcl` 再生成 `src/pragma.h`（`src/pragma.c` 头部注释明确「不要直接编辑 `pragma.h`」）。`tool/mkpragmatab.tcl` 负责生成三类东西：`PragTyp_XXX` 编号（按首次出现顺序递增）、`PragFlg_XXX` 位标志（按 `lsort` 后的名字顺序，每次乘 2 递增，`fv` 从 1 开始）、以及 `aPragmaName[]` 查表（按名字字典序排列以便二分）。代价是新增一条 PRAGMA 要改脚本并重跑生成，不是纯 C 单文件改动。

**标志位驱动通用行为，而不是每个 PRAGMA 各写一遍。** `NeedSchema` 统一处理「要先加载模式」、`NoColumns`/`NoColumns1` 统一处理「要不要出结果列/结果行」、`ReadOnly` 统一处理「只读 cookie」、`Result0`/`Result1` 统一处理「能不能做成表值函数」。`sqlite3Pragma` 里有专门的断言配合：`if( (pPragma->mPragFlg & PragFlg_NoColumns1) && zRight ){ sqlite3VdbeVerifyNoResultRow(v); }`——源码注释说明这个块「除非定义 `SQLITE_DEBUG` 否则是 no-op，唯一目的是跑 assert 验证：置了 `PragFlg_NoColumns1` 且调用方给了参数时，实现没有往虚拟机里加任何 `OP_ResultRow`」。

**读写分离在连接级与持久级之间。** `cache_size` 只改 `pDb->pSchema->cache_size`（当前连接、有效到连接关闭），`default_cache_size` 才通过 `OP_SetCookie` 写 `BTREE_DEFAULT_CACHE_SIZE`（值 3）落盘；`page_size` 设值时先记进 `db->nextPagesize` 等到文件首次创建时生效；`synchronous` 只改 `pDb->safety_level`，源码注释写明「改本地值不会改磁盘文件，下次打开数据库时默认值会恢复」。

**给 VFS 留后门的分派顺序。** 内置表查找发生在 `SQLITE_FCNTL_PRAGMA` 文件控制**之后**：VFS 返回 `SQLITE_OK` 就由 VFS 完全接管（并可回填一列文本结果），返回 `SQLITE_NOTFOUND` 才继续走内置表。代价是 VFS 能覆盖内置 PRAGMA 的语义，这是官方文档明说的扩展点。

**参数越界不报错。** `max_page_count` 分支里 `if( x<0 ) x = 0; else if( x>0xfffffffe ) x = 0xfffffffe;`——超范围被夹到边界而不是报错；`journal_size_limit` 里 `if( iLimit<-1 ) iLimit = -1;`。这与官方文档第 4 条一致：PRAGMA 的数值参数越界或格式不对时，语句「尽力应付」，不强制报错。

**防御模式下的多处拒绝。** `SQLITE_Defensive` 至少三处生效：`PragTyp_JOURNAL_MODE` 拒绝 OFF；`PragTyp_FLAG` 拒绝置 `SQLITE_WriteSchema`；`PragTyp_HEADER_VALUE` 里若 `iCookie==BTREE_SCHEMA_VERSION` 且处于防御模式，把 `OP_SetCookie` 改写成 `OP_Noop`（源码注释写明理由：`PRAGMA schema_version=` 可被用来绕过模式版本检查，源码原话是 "potentially dangerous and may lead to program crashes or database corruption"）。

**布尔解析复用同步级别解析。** `sqlite3GetBoolean` 复用 `getSafetyLevel`，靠 `omitFull` 参数区分「布尔」与「安全级别」两种语义。副作用是布尔语境下 `full`/`extra` 不被接受——`sqlite3GetBoolean` 的行为完全由 `getSafetyLevel` 的 `iValue[i]<=1` 判定决定。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/pragma.c | sqlite3Pragma | PRAGMA 分派主体：解析 `[schema.]` 前缀、取名字与值、`sqlite3AuthCheck` 授权、先发 `SQLITE_FCNTL_PRAGMA` 文件控制给 VFS、再 `pragmaLocate` 查表、按 `PragFlg_NeedSchema` 决定是否先加载模式、按 `PragFlg_NoColumns`/`NoColumns1` 决定是否注册结果列名，最后 `switch( pPragma->ePragTyp )` 分派。 |
| src/pragma.c | sqlite3GetBoolean | 布尔字面量解析：`return getSafetyLevel(z,1,dflt)!=0;`，复用 `getSafetyLevel` 的 8 项查找表并禁止 `full`/`extra`；被 `src/main.c` 的 `sqlite3_uri_boolean` 复用。 |
| src/pragma.c | getSafetyLevel | 静态函数：按 `zText`/`iOffset`/`iLength`/`iValue` 四张表匹配 `on/no/off/false/yes/true/extra/full`，数字首字符走 `sqlite3Atoi(z)!=0`，未命中返回 `dflt`；`omitFull` 非 0 时拒绝值大于 1 的项。 |
| src/pragma.c | pragmaLocate | 在按字典序排列的 `aPragmaName[]` 上做二分查找，比较用 `sqlite3_stricmp`；找不到返回 0。 |
| src/pragma.c | setPragmaResultColumnNames | 按 `pPragma->nPragCName` 设结果列数与列名：为 0 时用 pragma 名做单列（`SQLITE_STATIC`），否则从 `pragCName[iPragCName]` 起逐个取名。 |
| src/pragma.c | returnSingleInt / returnSingleText | 生成「单行单列」结果：`sqlite3VdbeAddInt64(v, 1, value)` + `OP_ResultRow`；文本侧走 `sqlite3VdbeLoadString` + `OP_ResultRow`。 |
| src/pragma.c | getLockingMode | 把字符串映射到 `PAGER_LOCKINGMODE_EXCLUSIVE` / `PAGER_LOCKINGMODE_NORMAL`，其余返回 `PAGER_LOCKINGMODE_QUERY`。 |
| src/pragma.c | getAutoVacuum | 把 `none`/`full`/`incremental` 映射到 `BTREE_AUTOVACUUM_NONE`/`FULL`/`INCR`；数字等价物 0/1/2，超出 0..2 一律折为 0。 |
| src/pragma.c | getTempStore | `temp_store` 取值解析：首字符在 '0'..'2' 直接用数字；`file` 返回 1、`memory` 返回 2；其余返回 0 表示用编译期默认值。 |
| src/pragma.c | invalidateTempStorage | 改 temp 存储位置前把已打开的 temp btree 关掉并 `sqlite3ResetAllSchemasOfConnection(db)`；在事务内或备份进行中时 `sqlite3ErrorMsg` 报 `"temporary storage cannot be changed from within a transaction"` / `"...while a backup is running"` 并返回 `SQLITE_ERROR`。 |
| src/pragma.c | changeTempStorage | `temp_store` 的写入路径：值不变直接返回；否则先 `invalidateTempStorage`，成功才写 `db->temp_store`。 |
| src/pragma.c | setAllPagerFlags | 把 `pDb->safety_level` 与 `db->flags & PAGER_FLAGS_MASK` 合并后逐个库调 `sqlite3BtreeSetPagerFlags`；仅在 `db->autoCommit` 为真时执行。 |
| src/pragma.c | sqlite3JournalModename | 返回 `delete`/`persist`/`off`/`truncate`/`memory`（未定义 `SQLITE_OMIT_WAL` 时含 `wal`）的小写名；`eMode==ArraySize(azModeName)` 时返回 0 表示查不到。 |
| src/pragma.c | actionName | 把外键级联动作码映射为可读名：`OE_SetNull`→"SET NULL"、`OE_SetDflt`→"SET DEFAULT"、`OE_Cascade`→"CASCADE"、`OE_Restrict`→"RESTRICT"、其余→"NO ACTION"。 |
| src/pragma.c | pragmaFunclistLine | `PRAGMA function_list` 的输出行生成：按 `SQLITE_FUNC_INTERNAL` 过滤内部函数，按 `xValue`/`xFinalize` 判定类型 `w`/`a`/`s`，用 `azEnc[]` 映射编码名。 |
| src/pragma.c | integrityCheckResultRow | `integrity_check` 的输出行助手：`OP_ResultRow` 后接 `OP_IfPos`/`OP_Halt`，用于达到最大错误数时提前停止。 |
| src/pragma.c | tableSkipIntegrityCheck | 决定某张表是否跳过完整性检查：`pObjTab` 非空时按表身份比较，为空时只跳过伪装表（`TF_Imposter`）。 |
| src/pragma.c | pragmaVtabConnect / pragmaVtabBestIndex / pragmaVtabOpen / pragmaVtabClose / pragmaVtabFilter / pragmaVtabNext / pragmaVtabEof / pragmaVtabColumn / pragmaVtabRowid | `pragma_*` 表值函数的虚表模块：`xConnect` 用 `sqlite3_declare_vtab` 声明列并按标志追加 `arg HIDDEN`/`schema HIDDEN`；`xFilter` 拼出 PRAGMA SQL 后 `sqlite3_prepare_v2` 再 `sqlite3_step`；`xBestIndex` 对首个隐藏参数无约束时返回代价 2147483647。 |
| src/pragma.c | pragmaVtabModule | 虚表模块结构体，槽位按 `sqlite3_module` 顺序填入上述九个函数，写事务相关槽位（`xUpdate`/`xBegin`/`xSync`/`xCommit`/`xRollback` 等）全为 0。 |
| src/pragma.c | sqlite3PragmaVtabRegister | 把名字去掉 `pragma_` 前缀后 `pragmaLocate`，只对带 `PragFlg_Result0` 或 `PragFlg_Result1` 的 pragma 注册虚表模块。 |
| src/pragma.c | SQLITE_DEFAULT_OPTIMIZE_LIMIT | 编译期常量，默认 2000；`PRAGMA optimize` 触发的 ANALYZE 临时分析上限，源码注释写明该值按最坏 100 毫秒实测选出。 |
| src/pragma.c | SQLITE_INTEGRITY_CHECK_ERROR_MAX | 编译期常量，默认 100；`integrity_check` 最多报告的错误条数，用户传入非正整数时回退到该值。 |
| src/parse.y | PRAGMA 产生式段 | 五条产生式把 `PRAGMA nm dbnm`、`= nmnum`、`( nmnum )` 与带负号的两个变体统一映射到 `sqlite3Pragma(pParse,&X,&Z,&Y,minusFlag)`。 |
| src/build.c | sqlite3TwoPartName | 把 `[schema.]` 前缀解析成库索引 `iDb`：写了库名时用 `sqlite3FindDb` 查，未找到报 `"unknown database %T"` 并返回 -1；否则取 `db->init.iDb`。 |
| src/build.c | sqlite3PragmaVtabRegister 调用点 | 虚表名解析路径中尝试把 `pragma_xxx` 识别成 PRAGMA 表值函数。 |
| src/pragma.h | aPragmaName / PragmaName / PragTyp_XXX / PragFlg_XXX / pragCName | 由 `tool/mkpragmatab.tcl` 生成：`PragmaName` 结构体含 `zName`/`ePragTyp`/`mPragFlg`/`iPragCName`/`nPragCName`/`iArg`；`aPragmaName[]` 按字典序排列供二分查找。 |
| tool/mkpragmatab.tcl | pragma_def / flagMeaning / 生成流程 | PRAGMA 清单的唯一真源：`pragma_def` 列出名字、类型、参数、编译条件与标志；`flagMeaning` 给出各 `PragFlg_*` 的含义；生成流程按首次出现顺序发 `PragTyp_*` 编号、按 `lsort` 顺序发 `PragFlg_*` 位值、并输出按字典序排列的查表。 |
| src/sqlite.h.in | SQLITE_FCNTL_PRAGMA | 文件控制码，值 14；文档写明 VFS 可新增 PRAGMA 或覆盖内置 PRAGMA 语义，返回 `SQLITE_NOTFOUND` 则继续常规处理。 |
| src/pager.h | PAGER_JOURNALMODE_* / PAGER_LOCKINGMODE_* / PAGER_SYNCHRONOUS_MASK | `PAGER_JOURNALMODE_QUERY`=-1、DELETE=0、PERSIST=1、OFF=2、TRUNCATE=3、MEMORY=4、WAL=5；`PAGER_LOCKINGMODE_QUERY`=-1、NORMAL=0、EXCLUSIVE=1；`PAGER_SYNCHRONOUS_MASK`=0x07。 |
| src/btree.h | BTREE_AUTOVACUUM_* / BTREE_DEFAULT_CACHE_SIZE / BTREE_LARGEST_ROOT_PAGE / BTREE_INCR_VACUUM | `BTREE_AUTOVACUUM_NONE`=0、`FULL`=1、`INCR`=2；`BTREE_DEFAULT_CACHE_SIZE`=3、`BTREE_LARGEST_ROOT_PAGE`=4、`BTREE_INCR_VACUUM`=7，分别对应 `OP_ReadCookie`/`OP_SetCookie` 用的 cookie 槽位。 |
| src/sqliteLimit.h | SQLITE_DEFAULT_CACHE_SIZE | 编译期默认值，定义在 `#ifndef` 保护下取 **-2000**；`src/pragma.c` 的 `PragTyp_DEFAULT_CACHE_SIZE` 分支用它作为 `OP_Integer` 的填充值。 |
| src/main.c | sqlite3_uri_boolean | `sqlite3GetBoolean` 在 PRAGMA 之外的调用方：`bDflt = bDflt!=0; return z ? sqlite3GetBoolean(z, bDflt) : bDflt;`。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的分层里没有「PRAGMA 分派」这一层——它的配置面走的是 `db_*` 公开 API 与 `engine/db/tools/` 里的运维工具，不是 SQL 层的会话级语句。若主核要在 SQL 层开配置面，`src/pragma.c` 值得抄的是**表驱动 + 标志位 + 二分查找**这套骨架：一张生成出来的有序表负责「名字 → 行为类别 + 通用标志」，避免每加一个配置项就往一个巨型 switch 里塞一段特判。`tool/mkpragmatab.tcl` 那种「唯一真源在脚本、生成物不入手改」的做法也值得抄——它把「加配置项」的改动收敛成改一处声明，而不是改一处声明 + 一处 C 代码。

**架构推论：** `src/pragma.c` 里的三处防御性拒绝（`SQLITE_Defensive` 挡住 `journal_mode=OFF`、挡住 `writable_schema`、把 `schema_version=` 改写成 `OP_Noop`）代表一种「不可信 SQL 面」的收紧思路：不是简单地拒绝执行，而是**把危险的写操作降级为读操作**。主核若将来暴露类似 SQL 配置面，`PRAGMA schema_version` 这条路径（改写 cookie 可绕过模式失效检查）正是需要在主核里提前堵上的那一类。

**架构推论：** 本页是 AI 检索 SQLite PRAGMA 机制时的落点。全部 PRAGMA 语义集中在 `src/pragma.c`：`sqlite3Pragma`（分派主体）、`sqlite3GetBoolean` 与 `getSafetyLevel`（布尔/安全级别字面量）、`pragmaLocate`（查表）、`setPragmaResultColumnNames`（结果列名）、`getLockingMode` / `getAutoVacuum` / `getTempStore`（取值映射）、`sqlite3JournalModename`（日志模式名）、`sqlite3PragmaVtabRegister` 与 `pragmaVtabModule`（`pragma_*` 表值函数）。凡是「某个 PRAGMA 的默认值从哪来」「为什么未知 PRAGMA 不报错」「为什么 `PRAGMA cache_size` 重开库就没了」「`PRAGMA schema_version` 什么时候被禁掉」「`pragma_index_list` 这种写法是怎么实现的」这类问题，检索落点都应指向 `src/pragma.c`；而 `src/parse.y` 只负责把那五种语法形态送进来，`src/build.c` 只负责 `[schema.]` 前缀解析与虚表名注册，`tool/mkpragmatab.tcl` 才是 PRAGMA 清单的真源。

## 参考

- SQLite 官方文档《PRAGMA Statements》（含全部 pragma 说明、PRAGMA 函数与语法）：<https://sqlite.org/pragma.html>
- SQLite 官方文档《PRAGMA optimize》与《PRAGMA integrity_check》：<https://sqlite.org/pragma.html#pragma_optimize>
- SQLite 官方文档《sqlite3_file_control》（`SQLITE_FCNTL_PRAGMA` 的语义与返回码约定）：<https://sqlite.org/c3ref/file_control.html>
- SQLite 官方文档《Compile-time Options》（`SQLITE_DEFAULT_CACHE_SIZE`、`SQLITE_OMIT_PRAGMA` 等）：<https://sqlite.org/compile.html>
- SQLite 官方 Fossil 源码 `src/pragma.c`（trunk，PRAGMA 分派主体的官方版本）：<https://sqlite.org/src/doc/trunk/src/pragma.c>
- SQLite 官方 Fossil 源码 `tool/mkpragmatab.tcl`（trunk，PRAGMA 清单的唯一真源与代码生成脚本）：<https://sqlite.org/src/doc/trunk/tool/mkpragmatab.tcl>
- GitHub 源码镜像 `src/pragma.c`：<https://github.com/sqlite/sqlite/blob/master/src/pragma.c>
- GitHub 源码镜像 `tool/mkpragmatab.tcl`：<https://github.com/sqlite/sqlite/blob/master/tool/mkpragmatab.tcl>
- GitHub 源码镜像 `src/parse.y`（PRAGMA 语法产生式）：<https://github.com/sqlite/sqlite/blob/master/src/parse.y>
