---
project: sqlite
feature: step-and-finalize
page_type: feature
schema_version: 1
refs:
  - path: src/vdbeapi.c
    symbols: [sqlite3_step, sqlite3_finalize, sqlite3_reset]
---

# step 与 finalize

## 一句话职责

把「一次 `sqlite3_step()` 推进一行」和「语句生命周期终结时释放资源」这两件事的对外语义固定下来：前者负责调度虚拟机、吞掉 schema 失效并自动重编译、屏蔽扩展返回码；后者负责把语句从可用状态推到不可用状态并归还全部内存。

## 机制是什么

SQLite 把「执行一条 SQL」拆成三个对外动作：

1. **prepare**：把 SQL 文本编译成一条 VDBE 程序，得到 `sqlite3_stmt`（详见 B04）。
2. **step**：每次调用推进程序，直到有一行结果可取（`SQLITE_ROW`）、执行完毕（`SQLITE_DONE`）或出错。**取一行、调一次 step** 是 SQLite 的核心迭代协议。
3. **finalize**：销毁语句，释放它占用的全部内存。

本机制负责第 2、3 步的**对外语义**，不负责虚拟机内部 opcode 的执行语义（那是 `vdbe.c` 的 `sqlite3VdbeExec`，见 E01），也不负责参数写入变量槽位（那是 `sqlite3_bind_*`，见 E04）。

`sqlite3_step` 的返回值契约（源码 `src/vdbeapi.c` 的 `sqlite3_step` / `sqlite3Step`）：

- `SQLITE_ROW`（常量值 100，定义在 `src/sqlite.h.in`）：结果行已就绪，调用方用 `sqlite3_column_*` 取值后再 step 一次。
- `SQLITE_DONE`：语句成功执行完毕。
- `SQLITE_SCHEMA`：语句因 schema 变更而失效。对 v2/v3 接口，`sqlite3_step` 内部会自动重编译并重试，不把 `SQLITE_SCHEMA` 抛给调用方。
- `SQLITE_BUSY` / `SQLITE_LOCKED`：无法取得所需锁。
- `SQLITE_MISUSE`：API 被不当调用（对 NULL 语句、已 finalize 的语句调用）。

`sqlite3_reset` 与 `sqlite3_finalize` 的分工是本页最容易被误解的地方：**reset 让语句可复用，finalize 让语句消失**。两者都调用同一个底层复位函数 `sqlite3VdbeReset`，区别只在 reset 之后语句仍在，finalize 之后语句已被删除。

## 怎么实现的

### `sqlite3_step`：外层薄壳 + 内层执行体

`sqlite3_step` 本身很短，真正的执行体在同文件的静态函数 `sqlite3Step` 里。分工是刻意的：`sqlite3_step` 只做「schema 失效自动重编译」这一件事，其余全交给 `sqlite3Step`。

`sqlite3_step` 的流程：

1. `vdbeSafetyNotNull(v)` 检查：`v==0` 时 `sqlite3_log(SQLITE_MISUSE, ...)` 并返回 `SQLITE_MISUSE_BKPT`；`v->db==0`（语句已 finalize）同样返回 `SQLITE_MISUSE_BKPT`。
2. `sqlite3_mutex_enter(db->mutex)` 持连接互斥锁。
3. 进入重编译循环：条件是 `sqlite3Step(v)` 返回 `SQLITE_SCHEMA` **且** `cnt++ < SQLITE_MAX_SCHEMA_RETRY`。`SQLITE_MAX_SCHEMA_RETRY` 定义在 `src/vdbeInt.h`，默认值 **50**；它同时也是公开文档里承诺的重编译次数上限。
4. 重编译体 `sqlite3Reprepare(v)`（实现在 `src/prepare.c`）：
   - 它要求语句是 v2/v3 接口准备的——`sqlite3Reprepare` 内断言 `zSql!=0`，注释写明「Reprepare only called for prepare_v2() statements」。legacy `sqlite3_prepare` 接口不保存 SQL 文本，因此拿不到 `SQLITE_SCHEMA` 之外的路径。
   - 成功后调 `sqlite3VdbeSwap((Vdbe*)pNew, p)` 把新编译出的字节码整体换进旧语句对象（`sqlite3VdbeSwap` 交换 `*pA` 与 `*pB` 的整个结构体、链表指针 `pVNext`/`ppVPrev`、`zSql`，并把 `pB->aCounter[SQLITE_STMTSTATUS_REPREPARE]++`），再 `sqlite3TransferBindings(pNew, (sqlite3_stmt*)p)` 把参数绑定搬到新对象上，最后 `sqlite3VdbeResetStepResult` 与 `sqlite3VdbeFinalize` 收掉临时对象。
   - 失败分支：重编译失败时把连接上的错误信息复制到语句的 `v->zErrMsg`（`sqlite3DbStrDup`），并把 `v->pc` 置 0，确保随后 `sqlite3_finalize` 或 `sqlite3_reset` 时错误信息仍可通过 `sqlite3_errmsg()` / `sqlite3_errcode()` 取到；若 `db->mallocFailed` 已置位，则 `v->zErrMsg = 0` 且 `v->rc = rc = SQLITE_NOMEM_BKPT`，然后 `break` 跳出循环。
5. 重编译成功后调 `sqlite3_reset(pStmt)` 把程序计数器归位。
6. 若 `savedPc>=0`（说明这是重编译后的第二次尝试），置 `v->minWriteFileFormat = 254`——源码注释说明这是给 `OP_Init` 与 `OP_Trace` 的信号，让它们**不要**再做一次 `SQLITE_TRACE_STMT`，因为上一次 `sqlite3_step` 触发 `SQLITE_SCHEMA` 时已经做过了（源码标注 `tag-20220401a`）。
7. `sqlite3_mutex_leave(db->mutex)`，返回 `rc`。

返回前还有一层错误码收敛：`sqlite3Step` 末尾执行 `return (rc&db->errMask);`。`db->errMask` 在 `src/main.c` 的 `sqlite3_open_v2` 里按连接标志初始化——`(flags & SQLITE_OPEN_EXRESCODE)!=0` 时为 `0xffffffff`（返回扩展码），否则为 `0xff`（只保留主码）；`sqlite3_extended_result_codes(db, onoff)` 也改写同一个字段，取值同样是 `0xffffffff` 或 `0xff`。这就是「legacy 接口只返回通用错误码、v2 接口直接返回具体错误码」在源码里的实现方式。

### `sqlite3Step`：状态机与三态返回

`sqlite3Step` 的核心是 VDBE 的四态机。状态常量定义在 `src/vdbeInt.h`：

| 常量 | 值 | 含义 |
|---|---|---|
| `VDBE_INIT_STATE` | 0 | 预编译语句构造中 |
| `VDBE_READY_STATE` | 1 | 已就绪，尚未开始执行 |
| `VDBE_RUN_STATE` | 2 | 执行中 |
| `VDBE_HALT_STATE` | 3 | 已结束，需要 reset 或 finalize |

`sqlite3Step` 的分支：

- **`eVdbeState != VDBE_RUN_STATE`** 时进入 `restart_step` 标签：
  - 若状态是 `VDBE_READY_STATE` 且 `p->expired`（语句因 schema 变更而过期）：置 `p->rc = SQLITE_SCHEMA`，返回 `SQLITE_ERROR`（v2 接口下 `rc = sqlite3VdbeTransferError(p)`，把语句上的错误码与错误信息搬到连接句柄上）。这是外层 `sqlite3_step` 重编译循环的触发源。
  - 若 `db->nVdbeActive==0`（没有别的语句在跑），清掉中断标志 `AtomicStore(&db->u1.isInterrupted, 0)`，避免 `sqlite3_interrupt` 打断一个还没开始的语句。
  - 计数活跃度：`db->nVdbeActive++`；若 `p->readOnly==0` 则 `db->nVdbeWrite++`；若 `p->bIsReader` 则 `db->nVdbeRead++`。
  - 置 `p->pc = 0`、`p->eVdbeState = VDBE_RUN_STATE`。
  - 若状态是 `VDBE_HALT_STATE`（上一次跑完或出错）：**默认路径下 `sqlite3Step` 自己调 `sqlite3_reset((sqlite3_stmt*)p)`** 再 `goto restart_step` 重新开始。这就是 SQLite 3.7.0 起的「自动 reset」——源码注释明确写着：在 3.6.23 及更早版本上必须由应用先调 `sqlite3_reset()`，否则 `sqlite3_step()` 返回 `SQLITE_MISUSE`。
  - 编译期开关 `SQLITE_OMIT_AUTORESET` 可恢复旧行为：此时若 `p->rc&0xff` 是 `SQLITE_BUSY` 或 `SQLITE_LOCKED` 才调 `sqlite3_reset()`，否则直接 `return SQLITE_MISUSE_BKPT`。源码注释给出的理由是：有些针对 3.6.23 及更早版本写的应用确实依赖 `SQLITE_MISUSE` 返回值。
- 正式执行：`db->nVdbeExec++`，调 `sqlite3VdbeExec(p)`（`src/vdbe.c`），再 `db->nVdbeExec--`。若开了 `SQLITE_OMIT_EXPLAIN` 未定义且 `p->explain` 为真，则改走 `sqlite3VdbeList(p)` 输出 VDBE 程序清单。
- **返回 `SQLITE_ROW` 的分支**：`sqlite3VdbeExec` 返回 `SQLITE_ROW` 时，断言 `p->rc==SQLITE_OK` 且 `db->mallocFailed==0`，置 `db->errCode = SQLITE_ROW`，直接 `return SQLITE_ROW`。`SQLITE_ROW` 的产生点在 `src/vdbe.c` 的 `OP_ResultRow`：`p->pResultRow = &aMem[pOp->p1]` 之后 `rc = SQLITE_ROW; goto vdbe_return;`。
- **非 `SQLITE_ROW` 的分支**：
  - 先 `checkProfileCallback(db, p)` 触发 profile 回调。
  - 置 `p->pResultRow = 0`。
  - 若 `rc==SQLITE_DONE && db->autoCommit`：置 `p->rc = doWalCallbacks(db)`，失败则 `rc = SQLITE_ERROR`。`doWalCallbacks` 遍历 `db->nDb` 个库，对每个有 `pBt` 的库调 `sqlite3PagerWalCallback(sqlite3BtreePager(pBt))` 拿到待回调条目数 `nEntry`，若 `nEntry>0` 且 `db->xWalCallback` 已注册且 `rc==SQLITE_OK`，则调 `db->xWalCallback(db->pWalArg, db, db->aDb[i].zDbSName, nEntry)`。整段被 `#ifndef SQLITE_OMIT_WAL` 包住。
  - 若 `rc!=SQLITE_DONE` 且语句带 `SQLITE_PREPARE_SAVESQL` 标志（即 v2/v3 接口准备的语句）：`rc = sqlite3VdbeTransferError(p)`，把语句上的错误码与错误信息搬到连接句柄上。
- 收尾：`db->errCode = rc`；若 `sqlite3ApiExit(p->db, p->rc)` 返回 `SQLITE_NOMEM`（即 OOM），置 `p->rc = SQLITE_NOMEM_BKPT`，并在 v2 接口下把 `rc = p->rc`。
- `end_of_step` 处的断言限定了 legacy 接口允许返回的码：必须是 `SQLITE_ROW`、`SQLITE_DONE`、`SQLITE_ERROR`，或 `(rc&0xff)==SQLITE_BUSY`，或 `SQLITE_MISUSE`。
- 最终 `return (rc&db->errMask);`。

`sqlite3ApiExit`（`src/malloc.c`）是所有 API 出口的统一错误闸门：`if( db->mallocFailed || rc )` 才走 `apiHandleError(db, rc)`，后者在 `db->mallocFailed || rc==SQLITE_IOERR_NOMEM` 时 `sqlite3OomClear(db)`、`sqlite3Error(db, SQLITE_NOMEM)` 并返回 `SQLITE_NOMEM_BKPT`；否则返回 `rc & db->errMask`。

### `sqlite3_reset`：复位但保留语句

`sqlite3_reset` 的实现（`src/vdbeapi.c`）：

1. `pStmt==0` 时返回 `SQLITE_OK`（空指针无害）。
2. 取 `db = v->db`，`sqlite3_mutex_enter(db->mutex)`。
3. `checkProfileCallback(db, v)`。
4. `rc = sqlite3VdbeReset(v)`——把虚拟机从 `VDBE_RUN_STATE` 或 `VDBE_HALT_STATE` 拉回 `VDBE_READY_STATE`。
5. `sqlite3VdbeRewind(v)`——把程序计数器归位到程序开头，并清一批运行期状态：`p->pc = -1`、`p->rc = SQLITE_OK`、`p->errorAction = OE_Abort`、`p->nChange = 0`、`p->cacheCtr = 1`、`p->minWriteFileFormat = 255`、`p->iStatement = 0`、`p->nFkConstraint = 0`。
6. `rc = sqlite3ApiExit(db, rc)`。
7. `sqlite3_mutex_leave(db->mutex)`，返回 `rc`。

`sqlite3VdbeReset`（`src/vdbeaux.c`）内部做的事：

- 若 `p->eVdbeState==VDBE_RUN_STATE` 先调 `sqlite3VdbeHalt(p)` 强行停下。`sqlite3VdbeHalt` 是**唯一**能把状态机从 RUN 推到 HALT 的地方：它 `closeAllCursors(p)`、`checkActiveVdbeCnt(db)`，在 `p->bIsReader` 时锁住语句用到的所有 btree（`sqlite3VdbeEnter(p)`），并判定 `SQLITE_NOMEM` / `SQLITE_IOERR` / `SQLITE_INTERRUPT` / `SQLITE_FULL` 这四类「特殊错误」需要回滚；若是自动提交模式且没有遗留锁，会提交改动。
- 若 `p->pc>=0`（程序至少跑了一部分），把语句上的错误码与错误信息搬回连接句柄：先 `vdbeInvokeSqllog(p)`，然后 `if( db->pErr || p->zErrMsg )` 走 `sqlite3VdbeTransferError(p)`，否则只 `db->errCode = p->rc`。
- 释放 `p->zErrMsg`（`sqlite3DbFree`）并置 0，置 `p->pResultRow = 0`。
- 最后 `return p->rc & db->errMask;`。

`sqlite3VdbeTransferError`（`src/vdbeaux.c`）把 `p->rc` 与 `p->zErrMsg` 复制到连接的 `db->pErr` 上（`sqlite3ValueSetStr(..., SQLITE_TRANSIENT)`），并置 `db->errCode = rc`、`db->errByteOffset = -1`。它**不**清语句上的错误码——这是「reset 返回上一次执行的错误码」的来源。

关键点：`sqlite3_reset` **不改变参数绑定值**。`sqlite3_clear_bindings` 才是清绑定的入口。

### `sqlite3_finalize`：复位 + 删除

`sqlite3_finalize` 的实现（`src/vdbeapi.c`）：

1. `pStmt==0` 时 `rc = SQLITE_OK`——官方文档明确写「Invoking sqlite3_finalize() on a NULL pointer is a harmless no-op」。
2. `vdbeSafety(v)` 检查：`v->db==0` 说明语句已 finalize 过，`sqlite3_log(SQLITE_MISUSE, "API called with finalized prepared statement")` 后返回 `SQLITE_MISUSE_BKPT`。
3. 若定义了 `SQLITE_DISPOSAL_NOTIFIER`，先调 `SQLITE_DISPOSAL_NOTIFIER(2, pStmt)` 通知外部资源回收钩子。
4. `sqlite3_mutex_enter(db->mutex)`、`checkProfileCallback(db, v)`，并断言 `v->eVdbeState >= VDBE_READY_STATE`。
5. `rc = sqlite3VdbeReset(v)`——与 reset 相同的复位动作。
6. `sqlite3VdbeDelete(v)`——真正销毁。`sqlite3VdbeDelete`（`src/vdbeaux.c`）先 `sqlite3VdbeClearObject(db, p)` 清对象内部资源，再把语句从 `db->pVList` 链表上摘除（`*p->ppVPrev = p->pVNext`），最后 `sqlite3DbNNFreeNN(db, p)` 释放主体内存。
7. `rc = sqlite3ApiExit(db, rc)`。
8. `sqlite3LeaveMutexAndCloseZombie(db)`——`src/main.c` 中：若连接状态不是 `SQLITE_STATE_ZOMBIE`，或 `connectionIsBusy(db)`（还有未 finalize 的语句/备份），则仅 `sqlite3_mutex_leave(db->mutex)` 返回；否则说明 `sqlite3_close_v2` 已经调过且所有语句都已 finalize，于是 `sqlite3RollbackAll(db, SQLITE_OK)` 回滚未提交事务、`sqlite3CloseSavepoints(db)` 释放保存点、逐个 `sqlite3BtreeClose(pDb)` 关闭各库、`sqlite3SchemaClear(db->aDb[1].pSchema)` 清 TEMP schema、`sqlite3VtabUnlockList(db)` 解锁虚表、`sqlite3CollapseDatabaseArray(db)` 释放附加库数组、`sqlite3ConnectionClosed(db)` 通知 notify.c，最后释放连接本身。

**finalize 与 reset 的返回值差异**：两者都返回「上一次执行是否成功」。`sqlite3_finalize` 的返回值来自 `sqlite3VdbeReset` 的 `p->rc & db->errMask`；若最近一次评估出错，返回对应的错误码或扩展错误码。

### 一个容易忽略的细节：finalize 时的「僵尸连接」回收

`sqlite3_finalize` 是**唯一**会触发连接整体回收的语句级 API（通过 `sqlite3LeaveMutexAndCloseZombie`）。这解释了官方文档为什么强调「应用必须 finalize 每一条 prepared statement，否则会泄漏资源」——没 finalize 的语句会让连接停留在 busy 状态，`sqlite3_close_v2` 就无法真正释放连接。

## 关键取舍

- **「取一行」与「执行完」用同一个返回通道**：`sqlite3_step` 既返回 `SQLITE_ROW` 也返回 `SQLITE_DONE`，调用方靠返回值区分「继续循环」和「退出循环」。代价是循环体必须写 `while( rc==SQLITE_ROW )` 这种双段式判定，无法用 `while( sqlite3_step(...)==SQLITE_OK )` 走完。
- **自动 reset 而不是返回 `SQLITE_MISUSE`**：SQLite 3.7.0 起，`sqlite3Step` 在 `VDBE_HALT_STATE` 下自己调 `sqlite3_reset()` 并重新开始，而不是报错。源码注释给出了双向理由：向前看，认为「任何会收到 `SQLITE_MISUSE` 的应用本身就是坏的」；向后看，承认确有针对 3.6.23 及更早版本写的应用依赖 `SQLITE_MISUSE`，因此保留 `SQLITE_OMIT_AUTORESET` 编译期开关恢复旧行为。
- **schema 失效对调用方透明**：`sqlite3_step` 内部把 `SQLITE_SCHEMA` 吞掉并自动重编译，重试上限为 `SQLITE_MAX_SCHEMA_RETRY`（`src/vdbeInt.h` 默认 **50** 次）。代价是错误路径变长——重编译失败时要手工把连接上的错误信息复制进语句的 `v->zErrMsg`，并把 `v->pc` 置 0，好让随后 `sqlite3_finalize` / `sqlite3_reset` 时错误信息仍可读。
- **错误码靠掩码收敛而不是分支**：不区分 legacy 与 v2 接口，而是统一在出口做 `rc & db->errMask`。`db->errMask` 由 `SQLITE_OPEN_EXRESCODE` 连接标志或 `sqlite3_extended_result_codes()` 决定取 `0xffffffff`（保留扩展码）还是 `0xff`（只留主码）。好处是错误处理代码只有一条路径，代价是必须显式开 `SQLITE_OPEN_EXRESCODE` 或调 `sqlite3_extended_result_codes` 才能看到 `SQLITE_CONSTRAINT_UNIQUE` 这类细分码。
- **finalize 顺带承担连接回收职责**：`sqlite3LeaveMutexAndCloseZombie` 把「连接是否所有语句都已释放」这一判定放在 `sqlite3_finalize` 里而不是 `sqlite3_close_v2` 里，等于用「最后一个语句的 finalize」作为触发点。
- **profile 回调在两处触发**：`sqlite3_reset` / `sqlite3_finalize` 都调 `checkProfileCallback(db, v)`，而 `sqlite3Step` 在非 `SQLITE_ROW` 分支也调一次——保证语句因错误中止时 profiling 数据不丢。
- **WAL 回调只在「自动提交 + 执行完成」时触发**：`sqlite3Step` 里 `doWalCallbacks` 的条件是 `rc==SQLITE_DONE && db->autoCommit`。在显式事务里语句执行完不会触发 WAL 回调。

## 与主核 / AI 的关系

本机制是 SQLite「执行期对外契约」的所在：它是应用与虚拟机之间的唯一窄门。往上游走，`sqlite3_step` 消费的是 `vdbe.c` 的 `sqlite3VdbeExec` 产出的 `SQLITE_ROW` / `SQLITE_DONE`；往下游走，`sqlite3_finalize` 把语句从 `db->pVList` 链表上摘除，从而决定 `sqlite3_close_v2` 能否真正回收连接。

**架构推论：** 主数据库内核（`engine/db/`）若借鉴这套接口形状，关键约束是「行驱动 + 显式终结」：语句执行被建模成一个可以被反复推进的状态机（`VDBE_READY_STATE` / `VDBE_RUN_STATE` / `VDBE_HALT_STATE` 三态加上 `VDBE_INIT_STATE`），而不是「一次性跑完返回结果集」。这个约束的价值在于把「结果集的生命周期」与「语句的生命周期」解耦——`sqlite3_reset` 保留绑定值复用语句，`sqlite3_finalize` 才彻底销毁资源，两者在 `sqlite3VdbeReset` 处共用同一套复位逻辑。

**架构推论：** `sqlite3_step` 里的 schema 重试循环（上限 `SQLITE_MAX_SCHEMA_RETRY` = 50）是「元数据变更不应打断业务代码」这一诉求的具体实现：把编译期与执行期的耦合点收敛到一次自动重编译里。**架构推论：** 主核 `engine/db/` 的 catalog 层若允许运行时改表结构，同样需要在执行入口做一层等价的「计划失效 → 重新规划 → 重试」封装，且需要给重试次数设上限以避免活锁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbeapi.c | `sqlite3_step` | 对外执行入口：检查空指针/已 finalize 的语句，持连接互斥锁，在 `SQLITE_SCHEMA` 且 `cnt++ < SQLITE_MAX_SCHEMA_RETRY` 时循环调用 `sqlite3Reprepare` 重编译并 `sqlite3_reset` 归位，最后返回结果码 |
| src/vdbeapi.c | `sqlite3Step` | `sqlite3_step` 的执行体：驱动 VDBE 四态机（`VDBE_READY_STATE`/`VDBE_RUN_STATE`/`VDBE_HALT_STATE`），在 HALT 态下默认自动 `sqlite3_reset`（`SQLITE_OMIT_AUTORESET` 下改为返回 `SQLITE_MISUSE_BKPT`），调 `sqlite3VdbeExec` 执行，`SQLITE_ROW` 直返，其余走 `doWalCallbacks` 与 `sqlite3VdbeTransferError`，出口 `return (rc&db->errMask)` |
| src/vdbeapi.c | `sqlite3_finalize` | 销毁语句：`vdbeSafety` 检查已 finalize 的语句、`sqlite3VdbeReset` 复位、`sqlite3VdbeDelete` 释放、`sqlite3ApiExit` 收敛错误码、`sqlite3LeaveMutexAndCloseZombie` 在最后一条语句释放时回收僵尸连接 |
| src/vdbeapi.c | `sqlite3_reset` | 复位语句使其可复用：`sqlite3VdbeReset` + `sqlite3VdbeRewind`（`p->pc=-1`、`p->rc=SQLITE_OK`、`p->nChange=0`、`p->cacheCtr=1`），保留参数绑定值，返回上一次执行的错误码 |
| src/vdbeapi.c | `vdbeSafety` | 判断语句是否已 finalize（`p->db==0`），是则 `sqlite3_log(SQLITE_MISUSE, ...)` 并返回 1 |
| src/vdbeapi.c | `vdbeSafetyNotNull` | `vdbeSafety` 的空指针版本，额外检查 `p==0` |
| src/vdbeapi.c | `doWalCallbacks` | 事务提交后触发 WAL 钩子：遍历 `db->nDb` 个库取 `sqlite3PagerWalCallback` 的条目数，调 `db->xWalCallback` |
| src/vdbeapi.c | `sqlite3TransferBindings` | 重编译时把参数绑定搬到新语句对象：逐个 `sqlite3VdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i])` |
| src/vdbeapi.c | `checkProfileCallback` | 宏：`(P)->startTime>0` 时调 `invokeProfileCallback` |
| src/vdbeapi.c | `invokeProfileCallback` | 计算耗时并调用 `db->xProfile(db->pProfileArg, p->zSql, iElapse)` |
| src/vdbeapi.c | `sqlite3_clear_bindings` | 与 reset 对照的另一个入口：把 `p->nVar` 个变量槽的 `flags` 全置 `MEM_Null`，并按 `expmask`/`smimask` 与 `maskSmallInt` 决定是否置 `p->expired = 1` |
| src/vdbeaux.c | `sqlite3VdbeReset` | 复位核心：`VDBE_RUN_STATE` 时先 `sqlite3VdbeHalt`；`p->pc>=0` 时把语句上的错误码/信息搬回连接句柄（`sqlite3VdbeTransferError`）；释放 `p->zErrMsg`；返回 `p->rc & db->errMask` |
| src/vdbeaux.c | `sqlite3VdbeRewind` | 归位程序：`p->eVdbeState = VDBE_READY_STATE`、`p->pc = -1`、`p->rc = SQLITE_OK`、`p->errorAction = OE_Abort`、`p->nChange = 0`、`p->cacheCtr = 1`、`p->minWriteFileFormat = 255`、`p->iStatement = 0`、`p->nFkConstraint = 0` |
| src/vdbeaux.c | `sqlite3VdbeHalt` | 唯一把状态机从 RUN 推到 HALT 的地方：`closeAllCursors`、锁住语句用到的 btree、对 `SQLITE_NOMEM`/`SQLITE_IOERR`/`SQLITE_INTERRUPT`/`SQLITE_FULL` 四类特殊错误执行回滚、返回 `SQLITE_BUSY` 表示提交未完成需重试 |
| src/vdbeaux.c | `sqlite3VdbeDelete` | 销毁语句对象：`sqlite3VdbeClearObject` 清内部资源，把语句从 `db->pVList` 链表摘除，`sqlite3DbNNFreeNN` 释放主体内存 |
| src/vdbeaux.c | `sqlite3VdbeTransferError` | 把语句上的错误码与错误消息复制到连接的 `db->pErr`，置 `db->errCode = rc`、`db->errByteOffset = -1`；不清理语句上的错误码 |
| src/vdbeaux.c | `sqlite3VdbeSwap` | 重编译时交换新旧字节码：交换整个 `Vdbe` 结构体、`pVNext`/`ppVPrev` 链表指针、`zSql`，并 `pB->aCounter[SQLITE_STMTSTATUS_REPREPARE]++` |
| src/vdbeaux.c | `sqlite3VdbeResetStepResult` | 置 `p->rc = SQLITE_OK`，清掉上一次的 step 结果 |
| src/vdbeaux.c | `sqlite3VdbeFinalize` | 内部版 finalize：`eVdbeState>=VDBE_READY_STATE` 时先 `sqlite3VdbeReset`，再 `sqlite3VdbeDelete` |
| src/prepare.c | `sqlite3Reprepare` | schema 变更后重编译：要求语句带 SQL 文本（v2/v3 接口），`sqlite3LockAndPrepare` 编译新语句，`sqlite3VdbeSwap` 换入新字节码，`sqlite3TransferBindings` 搬绑定，再 `sqlite3VdbeResetStepResult` + `sqlite3VdbeFinalize` 收掉临时对象 |
| src/main.c | `sqlite3LeaveMutexAndCloseZombie` | 连接已是 `SQLITE_STATE_ZOMBIE` 且 `connectionIsBusy(db)` 为假时回收整个连接：`sqlite3RollbackAll`、`sqlite3CloseSavepoints`、逐个 `sqlite3BtreeClose`、`sqlite3SchemaClear` 清 TEMP schema、`sqlite3VtabUnlockList`、`sqlite3CollapseDatabaseArray`、`sqlite3ConnectionClosed` |
| src/main.c | `sqlite3_open_v2` | 连接创建时设定 `db->errMask = (flags & SQLITE_OPEN_EXRESCODE)!=0 ? 0xffffffff : 0xff` |
| src/main.c | `sqlite3_extended_result_codes` | 运行期切换 `db->errMask`：`onoff ? 0xffffffff : 0xff`，决定返回扩展错误码还是主错误码 |
| src/malloc.c | `sqlite3ApiExit` | 所有 API 出口的错误闸门：`db->mallocFailed || rc` 非零时转 `apiHandleError` |
| src/malloc.c | `apiHandleError` | `db->mallocFailed || rc==SQLITE_IOERR_NOMEM` 时 `sqlite3OomClear` + `sqlite3Error(db, SQLITE_NOMEM)` 并返回 `SQLITE_NOMEM_BKPT`；否则返回 `rc & db->errMask` |
| src/vdbe.c | `sqlite3VdbeExec` | 虚拟机的实际执行循环，由 `sqlite3Step` 调用；`OP_ResultRow` 处 `rc = SQLITE_ROW`，`OP_AutoCommit`/`OP_Savepoint` 等处 `rc = SQLITE_DONE` |
| src/vdbe.c | `OP_ResultRow` | 结果行产生点：`p->pResultRow = &aMem[pOp->p1]`，`p->pc = (int)(pOp - aOp) + 1`，`rc = SQLITE_ROW` |
| src/vdbeInt.h | `SQLITE_MAX_SCHEMA_RETRY` | schema 重编译重试上限，默认 50（`#ifndef` 保护，可被编译期覆盖） |
| src/vdbeInt.h | `VDBE_INIT_STATE` / `VDBE_READY_STATE` / `VDBE_RUN_STATE` / `VDBE_HALT_STATE` | VDBE 四态机的状态常量，取值 0 / 1 / 2 / 3 |
| src/sqliteInt.h | `SQLITE_MISUSE_BKPT` | 展开为 `sqlite3MisuseError(__LINE__)`，用于 API 不当调用 |
| src/sqlite.h.in | `SQLITE_ROW` | 结果行就绪的返回码，常量值 100 |
| src/sqlite.h.in | `sqlite3_step` / `sqlite3_finalize` / `sqlite3_reset` | 三个公开 API 的原型与官方语义说明文档 |

## 参考

- [Evaluate An SQL Statement（sqlite3_step 官方文档）](https://sqlite.org/c3ref/step.html)
- [Reset A Prepared Statement Object（sqlite3_reset 官方文档）](https://sqlite.org/c3ref/reset.html)
- [Destroy A Prepared Statement Object（sqlite3_finalize 官方文档）](https://sqlite.org/c3ref/finalize.html)
- [The SQLite Bytecode Engine（VDBE 官方文档）](https://sqlite.org/opcode.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/vdbeapi.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/vdbeapi.c)
- [sqlite/sqlite 仓库中的 src/vdbeapi.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c)
