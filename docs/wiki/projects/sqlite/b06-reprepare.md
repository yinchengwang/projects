---
project: sqlite
feature: reprepare
page_type: feature
schema_version: 1
refs:
  - path: src/prepare.c
    symbols: [sqlite3Reprepare]
---

# 模式变更后的语句重编译

## 一句话职责

当数据库模式（schema）发生变化导致已编译语句失效时，用保存下来的原始 SQL 文本把这条语句重新编译一遍，让调用方感觉不到模式变更的存在。

## 机制是什么

SQLite 把「语句编译」与「语句执行」拆成两步：`sqlite3_prepare_v2()` 把 SQL 文本编译成一段 VDBE 字节码，之后 `sqlite3_step()` 反复驱动这段字节码。字节码里嵌了编译那一刻的模式信息（哪些表存在、有哪些索引、列的亲和性是什么），一旦模式变了，这段字节码就不再正确。

SQLite 对此有两套方案，取决于 prepare 接口的代际：

- 旧接口 `sqlite3_prepare()`：不保留原始 SQL 文本。模式一变，`sqlite3_step()` 直接返回 `SQLITE_SCHEMA`，由应用自己决定怎么办。
- 新接口 `sqlite3_prepare_v2()` / `sqlite3_prepare_v3()`：语句对象里保存了原始 SQL 文本。模式一变，`sqlite3_step()` 会自动调用 `sqlite3Reprepare()` 重新编译，然后从头再跑一遍。

「模式变了」在源码里有两条触发路径：

- **运行期检测**：`OP_Transaction` 在 `P5!=0` 时会比对 schema cookie 与 schema generation counter（`iMeta` 对 `pOp->p3`，`pSchema->iGeneration` 对 `pOp->p4.i`），不一致就置 `p->expired = 1` 并令 `rc = SQLITE_SCHEMA`。
- **主动失效**：`sqlite3ExpirePreparedStatements()` 遍历 `db->pVdbe` 链表把 `p->expired` 置为 `iCode+1`；`sqlite3ExpireStmtsUsingSchema()` 只把用了指定 schema 的语句置为 `1`。

`Vdbe.expired` 的取值有语义区分（见 `sqlite3ExpirePreparedStatements` 的注释）：`1` 是立即失效，`2` 是建议性失效（advisory，语句可以跑完再重编译）。`sqlite3VdbeSwap` 在交换两台 VM 时会执行 `pB->expmask |= pA->expmask`，把失效掩码一并带过去。

除模式变化外，绑定参数的值也会让语句失效：当某个 host parameter 的取值影响查询计划选择时（它位于 `LIKE` / `GLOB` 的左侧，或与索引列比较且启用了 `SQLITE_ENABLE_STAT4`），`sqlite3_bind_*()` 按 `Vdbe.expmask` / `Vdbe.smimask` 判定并置 `p->expired = 1`，效果与模式变更等同。`sqlite3_bind_int64()` 的判定是：命中 `expmask` 且 `smimask` 未命中时，只要新值与旧值不同即置 `expired`；命中 `smimask` 时仅在「原值与新值是否同属 0/1 区间」发生翻转时置 `expired`。

## 怎么实现的

核心函数是 `src/prepare.c` 的 `sqlite3Reprepare()`，流程如下：

1. 从语句对象取出原始 SQL 文本：`zSql = sqlite3_sql((sqlite3_stmt *)p)`。函数体里有断言 `zSql!=0`，注释写明「Reprepare only called for prepare_v2() statements」——因为旧接口不保存 SQL 文本，根本无从重编译。
2. 取回原语句的 prepare 标志：`prepFlags = sqlite3VdbePrepareFlags(p)`（`src/vdbeaux.c`，直接返回 `v->prepFlags`）。这一步保证重编译时 `SQLITE_PREPARE_PERSISTENT`、`SQLITE_PREPARE_NORMALIZE` 等标志不丢失。
3. 调用 `sqlite3LockAndPrepare(db, zSql, -1, prepFlags, p, &pNew, 0)` 做真正的编译。第 5 个参数 `pOld` 非空，即 `sqlite3Prepare()` 里的 `pReprepare` 参数，被写入 `sParse.pReprepare`。`sParse.explain` 也从旧语句继承：`sParse.explain = sqlite3_stmt_isexplain((sqlite3_stmt*)pReprepare)`。
4. 编译成功后做「换壳」三连：
   - `sqlite3VdbeSwap((Vdbe*)pNew, p)`（`src/vdbeaux.c`）——把新字节码搬到旧语句对象里。实现是整体交换两个 `Vdbe` 结构体内容，再交换链表指针 `pVNext` / `ppVPrev`，交换 `zSql`，并把 `expmask`、`smimask` 取或合并、把 `aCounter` 拷过去，最后 `pB->aCounter[SQLITE_STMTSTATUS_REPREPARE]++` 记一次重编译计数。旧字节码被换到 `pNew` 一侧，随后被销毁。
   - `sqlite3TransferBindings(pNew, (sqlite3_stmt*)p)`（`src/vdbeapi.c`）——逐个 `sqlite3VdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i])` 把已绑定的参数值搬到新字节码上，循环上界是 `pFrom->nVar`。函数体里有断言 `pTo->nVar==pFrom->nVar`。
   - `sqlite3VdbeResetStepResult((Vdbe*)pNew)`（`src/vdbeaux.c`）——把 `p->rc` 置回 `SQLITE_OK`。
   - 最后 `sqlite3VdbeFinalize((Vdbe*)pNew)` 销毁被换出来的旧 VM。`sqlite3VdbeFinalize` 内部先按状态调 `sqlite3VdbeReset()` 再 `sqlite3VdbeDelete()`。

`sqlite3Reprepare()` 的返回值语义（见其上方注释）：成功返回 `SQLITE_OK`；如果因为别的连接锁住了 `sqlite3_schema` 表而无法重编译，返回 `SQLITE_LOCKED`；其他错误返回 `SQLITE_SCHEMA`。函数体里对 `SQLITE_NOMEM` 单独调 `sqlite3OomFault(db)` 后原样返回 `rc`。

### 触发与重试

`src/vdbeapi.c` 的 `sqlite3_step()` 是外层包装器，也是自动重编译的调用方。它的循环条件是：

```
while( (rc = sqlite3Step(v))==SQLITE_SCHEMA
       && cnt++ < SQLITE_MAX_SCHEMA_RETRY ){
  ...
  rc = sqlite3Reprepare(v);
  ...
  sqlite3_reset(pStmt);
  ...
}
```

- `SQLITE_MAX_SCHEMA_RETRY` 定义在 `src/vdbeInt.h`，默认值 `50`（`#ifndef` 保护，可被编译期覆盖）。这是重编译重试次数的上限。
- 重编译失败时，错误信息已经从编译器进入 `db->pErr`；这段代码把它复制进 `v->zErrMsg` 并令 `v->rc = rc = sqlite3ApiExit(db, rc)`，目的是让 `sqlite3_errmsg()` / `sqlite3_errcode()` 在语句被 finalize 或 reset 后仍能给出原因。若 `db->mallocFailed` 为真则改走 `v->zErrMsg = 0; v->rc = rc = SQLITE_NOMEM_BKPT;`。
- 重编译成功后调用 `sqlite3_reset(pStmt)` 把语句复位到 `VDBE_READY_STATE`。若重编译前语句已经跑过一段（`savedPc>=0`），会把 `v->minWriteFileFormat = 254`，注释说明这是给 `OP_Init` 和 `OP_Trace` 的信号，让它们不要再重复发 `SQLITE_TRACE_STMT` 事件。

内层的 `sqlite3Step()` 负责「过期即报错」这一半：`src/vdbeapi.c` 里当 `p->eVdbeState==VDBE_READY_STATE` 且 `p->expired` 非零时，置 `p->rc = SQLITE_SCHEMA`；若语句带 `SQLITE_PREPARE_SAVESQL` 标志（`src/vdbe.h` 定义为 `0x80`），错误码经 `sqlite3VdbeTransferError(p)` 直接返回给调用方，否则只回一个笼统的 `SQLITE_ERROR`。

### 编译期自身的重试

`sqlite3LockAndPrepare()`（`src/prepare.c`）内部还有一个编译重试循环：

```
do{
  rc = sqlite3Prepare(db, zSql, nBytes, prepFlags, pOld, ppStmt, pzTail);
  if( rc==SQLITE_OK || db->mallocFailed ) break;
  cnt++;
}while( (rc==SQLITE_ERROR_RETRY && ALWAYS(cnt<=SQLITE_MAX_PREPARE_RETRY))
       || (rc==SQLITE_SCHEMA && (sqlite3ResetOneSchema(db,-1,0), cnt)==1) );
```

- `SQLITE_MAX_PREPARE_RETRY` 定义在 `src/prepare.c`，默认值 `25`，注释写明「Maximum number of times that we will try again to prepare a statement that returns SQLITE_ERROR_RETRY」。
- `SQLITE_SCHEMA` 分支只重试一次（条件里的 `cnt)==1`），且重试前先调 `sqlite3ResetOneSchema(db,-1,0)` 重置模式；函数上方注释说明「A schema problem after one schema reset is considered a permanent error」。
- 编译前还会检查模式锁：`sqlite3Prepare()` 遍历 `db->aDb` 调 `sqlite3BtreeSchemaLocked(pBt)`，被锁就以 `SQLITE_ERROR` + 「database schema is locked: %s」返回。这正是 `sqlite3Reprepare()` 注释里说的「another connection has locked the sqlite3_schema table」那条路径。

### 主动触发重编译的接口

`sqlite3_stmt_explain()`（`src/vdbeapi.c`）是一个显式调用方：把语句切到 EXPLAIN / EXPLAIN QUERY PLAN 模式时，若不需要重新生成字节码（`v->nMem>=10 && (eMode!=2 || v->haveEqpOps)`）就直接改 `v->explain`；否则调 `sqlite3Reprepare(v)`，失败时把 `v->explain` 回滚为 `savedMode`。调用前会检查 `(v->prepFlags & SQLITE_PREPARE_SAVESQL)==0` 就返回 `SQLITE_ERROR`——没有保存 SQL 文本就无法重编译。

## 关键取舍

- **保存 SQL 文本换自动重编译**：新接口多存一份 SQL 字符串，换来模式变更时应用无感。代价是每条语句的常驻内存变大。旧接口 `sqlite3_prepare()` 不保存文本，因此模式一变只能把 `SQLITE_SCHEMA` 抛给应用自己处理。
- **重编译后从头执行，而不是断点续跑**：`sqlite3_step()` 在重编译成功后调 `sqlite3_reset(pStmt)`，语句从 `pc = 0` 重新开始。`OP_Transaction` 里的注释写明了这一点：「The sqlite3_step() wrapper function might then reprepare the statement and rerun it from the beginning.」
- **字节码整体交换而非逐字段搬运**：`sqlite3VdbeSwap()` 直接交换两个 `Vdbe` 结构体内容，再交换 `pVNext` / `ppVPrev` 链表指针和 `zSql`。这样旧语句对象在调用者手里的指针保持有效，句柄不变。
- **绑定参数必须跟着搬**：如果只换字节码不搬 `aVar[]`，参数就丢了。`sqlite3TransferBindings()` 逐个 `sqlite3VdbeMemMove` 搬运，并用断言 `pTo->nVar==pFrom->nVar` 保证新旧语句的参数个数一致。
- **重试次数有上限，防止活锁**：`sqlite3_step()` 层的 `SQLITE_MAX_SCHEMA_RETRY`（默认 `50`）和编译层的 `SQLITE_MAX_PREPARE_RETRY`（默认 `25`）都是编译期常量。模式持续被别的连接改动时，靠这两个上限收敛，而不是无限重编译。
- **模式锁检查优先于编译**：`sqlite3Prepare()` 在解析 SQL 之前先查 `sqlite3BtreeSchemaLocked()`。函数里的长注释解释了原因：若在别的连接未提交的模式变更上编译，而那些变更随后回滚，schema cookie 将检测不到模式变化，「Disaster would follow.」
- **编译期重试与执行期重试分开计数**：前者针对 `SQLITE_ERROR_RETRY`（解析器请求重试），后者针对 `SQLITE_SCHEMA`（模式变化）。两者用不同的常量和不同的循环条件，互不干扰。
- **重编译会打乱 trace 事件**：`sqlite3_step()` 用 `v->minWriteFileFormat = 254` 抑制重复的 `SQLITE_TRACE_STMT`。这是重编译语义的副作用，源码注释以 tag `20220401a` 标注。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/prepare.c | sqlite3Reprepare | 重编译总入口：取原始 SQL、取 prepFlags、调用编译、交换 VM、搬运绑定值 |
| src/prepare.c | sqlite3LockAndPrepare | 加锁并编译；内部按 SQLITE_MAX_PREPARE_RETRY（默认 25）重试 SQLITE_ERROR_RETRY，按 SQLITE_SCHEMA 分支重置一次模式 |
| src/prepare.c | sqlite3Prepare | 真正的编译实现；接收 pReprepare 并写入 sParse.pReprepare，继承 explain 模式；编译前检查 sqlite3BtreeSchemaLocked |
| src/prepare.c | schemaIsValid | 编译期比对各库 schema cookie，不一致时置 pParse->rc = SQLITE_SCHEMA 并调 sqlite3ResetOneSchema |
| src/prepare.c | sqlite3_prepare_v2 / sqlite3_prepare_v3 | 以 SQLITE_PREPARE_SAVESQL 标志调用 sqlite3LockAndPrepare，使语句保存 SQL 文本从而具备自动重编译能力 |
| src/vdbeapi.c | sqlite3_step | 自动重编译的调用方；在 SQLITE_SCHEMA 时循环调用 sqlite3Reprepare，次数上限为 SQLITE_MAX_SCHEMA_RETRY（定义在 src/vdbeInt.h，默认 50） |
| src/vdbeapi.c | sqlite3Step | 内层执行器；语句处于 READY 状态且 expired 非零时置 p->rc = SQLITE_SCHEMA |
| src/vdbeapi.c | sqlite3TransferBindings | 把旧语句的 aVar[] 绑定值逐个搬到新语句，断言 nVar 相同 |
| src/vdbeapi.c | sqlite3_stmt_explain | 主动触发重编译的接口；切换 EXPLAIN 模式时调 sqlite3Reprepare，失败则回滚 explain 模式 |
| src/vdbeapi.c | sqlite3_expired | 查询语句是否已失效（返回 Vdbe.expired），受 SQLITE_OMIT_DEPRECATED 保护 |
| src/vdbeaux.c | sqlite3VdbeSwap | 交换新旧 VM 的字节码、链表指针与 zSql，合并 expmask/smimask，并累加 SQLITE_STMTSTATUS_REPREPARE 计数 |
| src/vdbeaux.c | sqlite3VdbeSetSql | 保存语句的 SQL 文本；仅当带 SQLITE_PREPARE_SAVESQL 时保留 expmask/smimask |
| src/vdbeaux.c | sqlite3VdbePrepareFlags | 返回 Vdbe.prepFlags，供重编译时复用 |
| src/vdbeaux.c | sqlite3VdbeResetStepResult | 把 Vdbe.rc 复位为 SQLITE_OK |
| src/vdbeaux.c | sqlite3ExpirePreparedStatements | 把连接上所有语句的 expired 置为 iCode+1（1 表示立即失效，2 表示建议性失效） |
| src/vdbeaux.c | sqlite3ExpireStmtsUsingSchema | 把使用指定 schema 的语句 expired 置为 1（排除 pExcept 指定的那条） |
| src/vdbe.c | OP_Transaction | 运行期检测模式变化：比对 schema cookie 与 generation counter，不一致则置 expired 并返回 SQLITE_SCHEMA |
| src/vdbe.c | OP_Expire | 字节码层面主动使语句失效；P1==0 走 sqlite3ExpirePreparedStatements，P1!=0 只失效当前语句 |
| src/build.c | sqlite3ResetOneSchema | 重置指定 schema 的内存表示并连带 TEMP schema；必要时调 sqlite3ExpireStmtsUsingSchema |
| src/vdbe.h | SQLITE_PREPARE_SAVESQL | 值为 0x80 的标志，表示语句保存了 SQL 文本，是自动重编译的前提 |
| src/vdbeInt.h | SQLITE_MAX_SCHEMA_RETRY | 执行期重编译重试次数上限，默认 50 |
| src/sqliteInt.h | Parse.pReprepare | 指向正在被重编译的 VM；编译期据此读取旧绑定值并决定是否需要重新规划 |

## 与主核 / AI 的关系

**架构推论：** 本仓主核（`engine/db/`）的分层纪律是 L2 catalog 层不依赖 query / spi / modalities，L3 query / spi 层不依赖 modalities；主核自带 `Makefile`，不在 CMake 构建图内。SQLite 的重编译机制属于「语句编译 + 模式对象」之间的耦合点，对应主核里 catalog 层与 query 层之间的边界。

**架构推论：** 主核内置 SDK（`engine/src/sdk/`）是另一条独立线，只依赖 SQLite 这一个外部库。若 SDK 层使用 `sqlite3_prepare_v2()` / `sqlite3_prepare_v3()` 而不是旧接口 `sqlite3_prepare()`，则在其他连接改动模式时不会收到 `SQLITE_SCHEMA`，而是由 SQLite 内部自动重编译；这与主核自身的 catalog 失效重载逻辑是两套独立的失效检测机制。

**架构推论：** `Vdbe.expired` 的两档语义（`1` 立即失效 / `2` 建议性失效）区分了「必须重来」和「可以跑完再重来」两种代价完全不同的场景。主核若要在自己的 catalog 层引入类似缓存失效，需要区分「立刻作废」与「标记待重载」两类策略，否则会把代价高昂的重编译强行推给正在执行的长查询。

## 参考

- [SQLite C Interface: Compiling An SQL Statement](https://sqlite.org/c3ref/prepare.html) —— `sqlite3_prepare_v2()` 与旧接口 `sqlite3_prepare()` 的语义差异，以及 `SQLITE_MAX_SCHEMA_RETRY` 重试机制在文档中的表述。
- [SQLite C Interface: Evaluate An SQL Statement](https://sqlite.org/c3ref/step.html) —— `sqlite3_step()` 在 legacy 与 "vX" 接口下的返回值差异。
- [SQLite Compile-time Options](https://sqlite.org/compile.html) —— 编译期选项总表，含 `SQLITE_ENABLE_STAT4` 等影响重编译触发条件的开关。
- [sqlite/sqlite 源码：src/prepare.c](https://github.com/sqlite/sqlite/blob/master/src/prepare.c) —— `sqlite3Reprepare()` / `sqlite3LockAndPrepare()` / `sqlite3Prepare()` 的上游实现。
- [sqlite/sqlite 源码：src/vdbeapi.c](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c) —— `sqlite3_step()` 重编译循环与 `sqlite3TransferBindings()` 的上游实现。
