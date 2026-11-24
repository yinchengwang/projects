---
project: sqlite
feature: schema-cookie
page_type: feature
schema_version: 1
refs:
  - path: src/build.c
    symbols: [sqlite3ChangeCookie]
---

# 模式版本与缓存失效

## 一句话职责

模式 cookie 是 SQLite 用来判断「我缓存的模式是不是已经过期」的一个 32 位计数器：模式变更时递增它，其他连接在每次执行语句时比对它，不一致就丢弃自己的模式缓存并重编译语句。

## 机制是什么

边界（依据条目 `boundary`）：负责模式变更时递增 cookie 值，并让其他连接据此判定自己缓存的模式是否过期；不负责模式本身的增删（K/N 组），不负责语句的重新编译（E 组）。

cookie 存在两个地方：

- **磁盘上**：数据库文件头第 40 字节起的 4 字节大端整数。文件头里 `BTREE_SCHEMA_VERSION` 的下标是 `1`，而 `sqlite3BtreeGetMeta()` 用 `get4byte(&pBt->pPage1->aData[36 + idx*4])` 取值，代入 `idx=1` 正好是偏移 40，与文件格式文档一致。
- **内存里**：每个连接每个库各有一份 `Schema.schema_cookie`（`sqliteInt.h` 的 `struct Schema`），以及配套的 `Schema.iGeneration` 世代计数。

`sqlite3ChangeCookie()` 本身只做一件事：把「当前 cookie + 1」编译进 VDBE 指令流，指令是 `OP_SetCookie`，cookie 槽位是 `BTREE_SCHEMA_VERSION`。真正落盘发生在该指令执行时。

需要注意的是这个 cookie 的失效语义是「单值递增」：源码注释明确写出，模式可能被反复改动而 cookie 绕回旧值，撞回同一值的概率是 2^32 分之一，SQLite 认为这个概率足够低，可以接受。

## 怎么实现的

### 写侧：递增 cookie

`sqlite3ChangeCookie(Parse *pParse, int iDb)` 在 `src/build.c`，函数体只有一条有效语句：

```c
sqlite3VdbeAddOp3(v, OP_SetCookie, iDb, BTREE_SCHEMA_VERSION,
                 (int)(1+(unsigned)db->aDb[iDb].pSchema->schema_cookie));
```

即新值 = 当前内存 cookie 加一。`(unsigned)` 转换让 `INT_MAX` 加一后回绕成 `INT_MIN`，不会溢出成未定义行为。

调用方（全部经 grep 在 `src/` 下确认）：

| 调用点 | 语义 |
|---|---|
| `src/build.c` 的 `sqlite3CreateTable` 相关路径 | 表创建后写入 `sqlite_schema` 行，紧接着递增 cookie |
| `src/build.c` 的 `sqlite3DropTable` 路径 | 删表前递增 cookie |
| `src/build.c` 的 `sqlite3CreateIndex` 路径 | 建索引并 `sqlite3RefillIndex()` 后递增 cookie |
| `src/build.c` 的 `sqlite3DropIndex` 路径 | 删索引后递增 cookie |
| `src/alter.c` 的 `renameReloadSchema` | 重命名后重新加载模式时递增 cookie |
| `src/trigger.c` 的建触发器路径 | 触发器写入 `sqlite_schema` 后递增 cookie |
| `src/trigger.c` 的 `sqlite3DropTrigger` 路径 | 删触发器后递增 cookie |
| `src/vtab.c` 的虚拟表销毁路径 | 销毁虚表后递增 cookie |

### 执行侧：OP_SetCookie

`src/vdbe.c` 的 `case OP_SetCookie:` 做三件事：

1. `sqlite3BtreeUpdateMeta(pDb->pBt, pOp->p2, pOp->p3)` 把新值写回文件头。`sqlite3BtreeUpdateMeta()` 内部用 `put4byte(&pP1[36 + idx*4], iMeta)` 落盘，要求 `p->inTrans==TRANS_WRITE`。
2. 当 `pOp->p2==BTREE_SCHEMA_VERSION` 时更新内存副本：`*(u32*)&pDb->pSchema->schema_cookie = *(u32*)&pOp->p3 - pOp->p5;`，并置 `db->mDbFlags |= DBFLAG_SchemaChange`，同时调 `sqlite3FkClearTriggerCache(db, pOp->p1)`。
3. `pOp->p1==1`（TEMP 库）时调 `sqlite3ExpirePreparedStatements(db, 0)`，把本连接所有已编译语句作废（源码注释标为 Ticket #1644）。

这里 `p5` 是「内部 cookie 与数据库 cookie 的偏移量」。`OP_SetCookie` 的注释写明：`PRAGMA schema_version=N` 会把 `p5` 置 1，于是内部 schema version 与数据库里的 schema version 不相等，从而强制触发一次模式重置。

### 读侧：比对与作废

**编译期埋点。** `sqlite3CodeVerifySchema()`（`src/build.c`）把库号记进 `pToplevel->cookieMask`。真正生成校验代码的是 `sqlite3FinishCoding()` 里的一段循环：对 `cookieMask` 中每个置位的库，发一条 `OP_Transaction`，把 `pSchema->schema_cookie` 放进 P3、`pSchema->iGeneration` 放进 P4。

**运行期比对。** `src/vdbe.c` 的 `case OP_Transaction:` 先调 `sqlite3BtreeBeginTrans(pBt, pOp->p2, &iMeta)`。`sqlite3BtreeBeginTrans()` 在 `src/btree.c`，它把当前文件头偏移 40 处的 4 字节读进 `*pSchemaVersion`：

```c
*pSchemaVersion = get4byte(&pBt->pPage1->aData[40]);
```

（已处于事务中时走 `sqlite3BtreeBeginTrans` 的快速路径，同样读这一处。）

随后 `OP_Transaction` 的校验条件是：

```c
if( rc==SQLITE_OK && pOp->p5 && (iMeta!=pOp->p3 || pDb->pSchema->iGeneration!=pOp->p4.i) )
```

命中即：设错误消息 `"database schema has changed"`，若 `db->aDb[pOp->p1].pSchema->schema_cookie!=iMeta` 则调 `sqlite3ResetOneSchema(db, pOp->p1, p)`，并把 `p->expired = 1`、`rc = SQLITE_SCHEMA`。源码注释里给出了不重载模式的理由：虚表模块常把数据存在普通表里并在 `xNext()` 中查询，如果因为这种过期就丢弃模式，虚表实现方就得随时准备应对 `sqlite3_vtab` 结构被作废。

`pOp->p5` 由 `sqlite3FinishCoding()` 通过 `sqlite3VdbeChangeP5(v, 1)` 置位（条件是 `db->init.busy==0`），所以「初始化读模式」阶段不做这个比对。

### 失效后的重载链路

- `sqlite3ResetOneSchema()`（`src/build.c`）：置 `DB_ResetWanted`、清 `DBFLAG_SchemaKnownOk`；在 `db->nSchemaLock==0` 且没有别的活跃 VDBE 占用该库时，遍历各库调 `sqlite3ExpireStmtsUsingSchema(db, i, pSafe)` 与 `sqlite3SchemaClear(db->aDb[i].pSchema)`。若 `db->nSchemaLock!=0`，重置被推迟，函数注释写明可用 `iDb<0` 再次调用重试。
- `sqlite3ResetAllSchemasOfConnection()`（`src/build.c`）：清所有附加库的 `sqlite3SchemaClear()`，调 `sqlite3ExpirePreparedStatements(db, 0)`，并清 `DBFLAG_SchemaChange|DBFLAG_SchemaKnownOk`。
- `sqlite3SchemaClear()`（`src/callback.c`）在 `DB_SchemaLoaded` 已置位时执行 `pSchema->iGeneration++`，这就是 `iGeneration` 这个「世代计数」的来源。
- `OP_ParseSchema`（`src/vdbe.c`）：`pOp->p4.z==0` 的分支里调 `sqlite3ExpireStmtsUsingSchema()`、`sqlite3SchemaClear()`、`sqlite3InitOne()`，最后置 `DBFLAG_SchemaChange`。
- `sqlite3InitOne()`（`src/prepare.c`）：从文件头读回全部 meta 值，其中 `pDb->pSchema->schema_cookie = meta[BTREE_SCHEMA_VERSION-1]`，把当前磁盘上的 cookie 同步进内存。

### 上层如何处理 SQLITE_SCHEMA

`sqlite3_step()`（`src/vdbeapi.c`）是一个循环：

```c
while( (rc = sqlite3Step(v))==SQLITE_SCHEMA && cnt++ < SQLITE_MAX_SCHEMA_RETRY ){
    ...
    rc = sqlite3Reprepare(v);
    ...
    sqlite3_reset(pStmt);
}
```

`SQLITE_MAX_SCHEMA_RETRY` 在 `src/vdbeInt.h` 中默认定义为 `50`。重编译由 `sqlite3Reprepare()`（`src/prepare.c`）完成，它调 `sqlite3LockAndPrepare()` 重新编译 SQL 文本并用 `sqlite3VdbeSwap()` 换上新程序。

因此实际效果是：`sqlite3_prepare_v2()` 系列的语句遇到模式变更会自动重编译并重跑；只有反复失败超过 `SQLITE_MAX_SCHEMA_RETRY` 次后，`SQLITE_SCHEMA` 才会从 `sqlite3_step()` 返回给应用。

### 顺带一提：PRAGMA schema_version

`src/pragma.c` 的 `case PragTyp_HEADER_VALUE:` 用 `OP_Transaction` + `OP_ReadCookie` / `OP_SetCookie` 实现 `PRAGMA schema_version` 的读写。写入分支把 `aOp[1].p5 = 1`，注释写明这是为了让内部 schema version 与数据库 schema version 不同从而触发一次模式重置。防御模式（`SQLITE_Defensive`）下，写 `BTREE_SCHEMA_VERSION` 会被改成 `OP_Noop`，即被忽略。官方文档在这一节明确标注该 pragma 属于「仅供测试使用，不建议在应用程序中使用」。

## 关键取舍

**取舍一：单值递增而不是内容哈希。** 用一个计数器而不是模式内容的哈希，代价是可能撞回旧值。`sqlite3ChangeCookie()` 上方的注释直接承认这一点，并给出 2^32 的碰撞概率作为可接受依据。换成哈希会让每次模式变更都要重算全部模式内容，成本远高于加一。

**取舍二：cookie 与 iGeneration 双条件。** 校验条件是 `iMeta!=pOp->p3 || pDb->pSchema->iGeneration!=pOp->p4.i`，两者取或。仅靠 cookie 无法发现「模式内容变了但 cookie 没变」的情况（例如 `PRAGMA schema_version` 被绕过，或模式被整体清空重载）；`iGeneration` 由 `sqlite3SchemaClear()` 递增，补偿了这一类。

**取舍三：比对失败时是否重载模式是有条件的。** 只有 `db->aDb[pOp->p1].pSchema->schema_cookie!=iMeta` 才调 `sqlite3ResetOneSchema()`。源码注释给出了理由：虚表模块的数据常存于普通表并在 `xNext()` 内被查询，这种情况下不能让 `sqlite3_vtab` 结构在虚表方法调用中被作废。

**取舍四：重置可被推迟。** `sqlite3ResetOneSchema()` 在 `db->nSchemaLock!=0` 或有别的活跃 VDBE 使用该库时只打 `DB_ResetWanted` 标记，不立即清缓存。这避免了正在运行的语句踩到半清空的状态，代价是需要后续再触发一次重置。

**取舍五：自动重编译有次数上限。** `SQLITE_MAX_SCHEMA_RETRY` 默认为 `50`（`src/vdbeInt.h`）。超过后 `SQLITE_SCHEMA` 直接抛给应用，而不是无限重试。这避免了模式被反复改动时的活锁。

**取舍六：写入必须落在写事务内。** `sqlite3BtreeUpdateMeta()` 断言 `p->inTrans==TRANS_WRITE`，`OP_SetCookie` 也断言 `p->readOnly==0`。cookie 的持久化因此天然受事务原子性保护，不存在「cookie 已递增但模式改动未提交」的中间态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/build.c | `sqlite3ChangeCookie` | 生成「cookie 加一」的 `OP_SetCookie` 指令，cookie 槽位为 `BTREE_SCHEMA_VERSION` |
| src/build.c | `sqlite3CodeVerifySchema` | 把需要校验 cookie 的库号记入 `cookieMask` |
| src/build.c | `sqlite3CodeVerifySchemaAtToplevel` | `cookieMask` 的实际写入点，并按需打开 TEMP 库 |
| src/build.c | `sqlite3ResetOneSchema` | 重置单个库的模式缓存，必要时推迟 |
| src/build.c | `sqlite3ResetAllSchemasOfConnection` | 重置本连接全部附加库的模式缓存 |
| src/build.c | `sqlite3CommitInternalChanges` | 提交后清除 `DBFLAG_SchemaChange` |
| src/vdbe.c | `OP_SetCookie` | 落盘新 cookie、更新内存副本、置 `DBFLAG_SchemaChange`、清外键触发器缓存 |
| src/vdbe.c | `OP_ReadCookie` | 读指定 cookie 值到寄存器 |
| src/vdbe.c | `OP_Transaction` | 读回文件头 cookie 并与编译期常量比对，不符则报 `SQLITE_SCHEMA` |
| src/vdbe.c | `OP_ParseSchema` | 模式重载：`sqlite3SchemaClear()` + `sqlite3InitOne()` |
| src/vdbe.c | `OP_Savepoint` | 回滚到保存点时若发生模式变更，作废语句并重置模式 |
| src/vdbeapi.c | `sqlite3_step` | 捕获 `SQLITE_SCHEMA` 后调 `sqlite3Reprepare()` 重编译重跑 |
| src/vdbeaux.c | `sqlite3ExpirePreparedStatements` | 给所有已编译语句置 `expired` 标记（1 立即、2 建议） |
| src/vdbeaux.c | `sqlite3ExpireStmtsUsingSchema` | 只作废使用指定库模式的语句 |
| src/btree.c | `sqlite3BtreeGetMeta` | 按 `36 + idx*4` 偏移从文件头读 meta 值 |
| src/btree.c | `sqlite3BtreeUpdateMeta` | 按 `36 + idx*4` 偏移把 meta 值写回文件头 |
| src/btree.c | `sqlite3BtreeBeginTrans` | 事务开始时把文件头偏移 40 的 cookie 读给调用方 |
| src/btree.h | `BTREE_SCHEMA_VERSION` | 模式 cookie 在 meta 数组中的下标，值为 `1` |
| src/btree.h | `SQLITE_N_BTREE_META` | meta 项总数，值为 `16`，决定 cookie 下标合法上界 |
| src/sqliteInt.h | `Schema` | 持有 `schema_cookie` 与 `iGeneration` 两个内存态字段 |
| src/callback.c | `sqlite3SchemaClear` | 清模式哈希表，并在已加载时递增 `iGeneration` |
| src/prepare.c | `sqlite3InitOne` | 从文件头读回 cookie 同步进 `pSchema->schema_cookie` |
| src/prepare.c | `sqlite3Reprepare` | 重新编译 SQL 文本并替换原 VDBE 程序 |
| src/vdbeInt.h | `SQLITE_MAX_SCHEMA_RETRY` | 自动重编译次数上限，默认 `50` |
| src/pragma.c | `PragTyp_HEADER_VALUE` | `PRAGMA schema_version` 读写；防御模式下禁止写 cookie |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 是从零手写的多模态数据库内核，其目录分层（`storage/`、`wal/`、`index/`、`txn/`、`catalog/`、`query/`、`spi/`）里没有一个与 SQLite 的模式 cookie 对位。SQLite 的 cookie 解决的是「多连接共享一个文件、每个连接各自缓存模式」的问题；主核是单进程内的自有内核，模式由 `catalog/` 层独占持有。

**架构推论：** 若主核未来要支持多连接共享同一存储，需要一个等价的失效信号。最接近的现成参照是 SQLite 的双保险设计——`schema_cookie` 负责跨进程可见的版本号，`iGeneration` 负责进程内清空模式时不被漏掉；只做前者会漏掉「模式被整体重载但版本号没变」的情况。

**架构推论：** AI 侧（AI 应用层）很少直接接触 cookie，但它的影响会以「语句执行时抛出 SQLITE_SCHEMA」的形式出现。嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库，AI 生成的 SQL 若长期持有 prepared statement，在他人执行 DDL 后会命中这条失效路径，SDK 层若把 `SQLITE_SCHEMA` 直接当错误抛给上层，就会把一次「自动重编译本可解决」的抖动变成硬失败。

## 参考

- [SQLite Database File Format（模式 cookie 位于文件头偏移 40）](https://sqlite.org/fileformat2.html)
- [SQLite Result and Error Codes（`SQLITE_SCHEMA` 与 `SQLITE_MAX_SCHEMA_RETRY`）](https://sqlite.org/rescode.html#schema)
- [SQLite PRAGMA 文档（`PRAGMA schema_version`，标注为仅供测试使用）](https://sqlite.org/pragma.html#pragma_schema_version)
- [sqlite/sqlite 源码：src/build.c](https://github.com/sqlite/sqlite/blob/master/src/build.c)
- [sqlite/sqlite 源码：src/vdbe.c](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
