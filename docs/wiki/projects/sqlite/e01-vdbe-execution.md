---
project: sqlite
feature: vdbe-execution
page_type: feature
schema_version: 1
refs:
  - path: src/vdbe.c
    symbols: [sqlite3VdbeExec]
---

# 虚拟机主循环

## 一句话职责

`sqlite3VdbeExec` 是 SQLite 虚拟机的执行引擎：它把一条已经封箱好的 VDBE 程序按操作码逐条取指执行，用一个 `switch( pOp->opcode )` 分派到每条指令的处理分支，负责取指、跳转、寄存器读写与异常中断，并在错误时统一收束到一条 abort 路径。

## 机制是什么

SQLite 的执行模型是「SQL 文本编译成字节码，再由虚拟机跑字节码」。代码生成器（`expr.c`、`select.c` 等）产出的是一条**线性操作码数组** `p->aOp`，每条指令是一个 `Op`，含 `opcode` 与 P1–P5 五个操作数；本机制负责的是这条数组的**运行**。

主循环的结构由 `sqlite3VdbeExec` 的三段组成：

1. **进入前的一次性准备**：断言语句处于 `VDBE_RUN_STATE`；若 `p->lockMask` 非零则调 `sqlite3VdbeEnter(p)` 锁住本语句会用到的 btree；按 `db->nProgressOps` 算出本次的进度回调触发阈值 `nProgressLimit`；把 `p->rc` 复位为 `SQLITE_OK`；检查中断标志。
2. **取指—执行循环**：`for(pOp=&aOp[p->pc]; 1; pOp++)` 是一个**没有退出条件**的无限循环，每轮 `nVmStep++` 递增虚拟机的步数计数，然后用 `switch( pOp->opcode )` 分派。正常结束靠指令流自己走到 `OP_Halt`，异常结束靠 `goto abort_due_to_error`。
3. **收束与归还**：`vdbe_return:` 标签处累加 `p->aCounter[SQLITE_STMTSTATUS_VM_STEP]`，若 `p->lockMask` 非零则调 `sqlite3VdbeLeave(p)` 释放 btree 锁，最后 `return rc`。

指令跳转不是靠 `pc` 变量自增，而是靠**改写 `pOp` 指针本身**：`OP_Goto` 把 `pOp = &aOp[pOp->p2 - 1]`，循环末尾的 `pOp++` 再把它推进到目标指令。由于「跳转后要落到下一条」的约定，`p2` 存的是**目标地址 + 1**，取指令时统一 `- 1`。

页面边界：本机制只负责**执行**已生成的操作码，不负责操作码的生成（那是 `c06-virtual-machine-program-build` 讲的 `sqlite3VdbeMakeReady`），也不负责语句对外的 `step`/`finalize` 生命周期（那是 `e02-step-and-finalize`）。不过本机制内部确实调用了 `sqlite3_step` 路径上的公共设施（如 `sqlite3VdbeHalt`），因为退出虚拟机必须做提交或回滚。

## 怎么实现的

### 进入前的准备

`sqlite3VdbeExec` 开头做了一串一次性动作，每一项都有明确来源：

- `assert( p->eVdbeState==VDBE_RUN_STATE )`：调用方（`src/vdbeapi.c` 的 `sqlite3_step`）在 `p->eVdbeState==VDBE_READY_STATE` 时置 `p->pc = 0; p->eVdbeState = VDBE_RUN_STATE;` 后才调用本函数。
- `if( DbMaskNonZero(p->lockMask) ) sqlite3VdbeEnter(p);`：`sqlite3VdbeEnter`（`src/vdbeaux.c`）遍历 `db->aDb`，对 `DbMaskTest(p->lockMask,i)` 命中的 btree 调 `sqlite3BtreeEnter`。共享缓存下这一层锁保证语句执行期间相关 btree 不被换出。
- 进度回调阈值：`if( db->xProgress )` 时 `nProgressLimit = db->nProgressOps - (iPrior % db->nProgressOps);`，其中 `iPrior = p->aCounter[SQLITE_STMTSTATUS_VM_STEP]`、`db->nProgressOps` 由 `sqlite3_progress_handler`（`src/main.c`）在 `nOps>0` 时写入；没有进度回调时 `nProgressLimit = LARGEST_UINT64`。
- `if( p->rc==SQLITE_NOMEM ) goto no_mem;`：上一次执行遗留的 OOM 状态在这里直接转成 OOM 错误。
- `db->busyHandler.nBusy = 0;` 复位忙处理器计数；`p->iCurrentTime = 0;` 复位当前时间缓存。
- `if( AtomicLoad(&db->u1.isInterrupted) ) goto abort_due_to_interrupt;`：进入前先查一次中断。

### 取指循环

循环体开头是每次迭代都要做的事：

- `nVmStep++;`：虚拟机步数计数器加一。
- `pOp->nExec++;` 与 `pnCycle` 统计：仅在 `VDBE_PROFILE` 或 `SQLITE_ENABLE_STMT_SCANSTATUS` 编译开关下生效，用于逐指令的性能剖析。
- **调试期不变量检查**：在 `SQLITE_DEBUG` 下，按 `sqlite3OpcodeProperty[pOp->opcode]` 的标志位检查操作数合法性——`OPFLG_IN1`/`OPFLG_IN2`/`OPFLG_IN3` 要求对应寄存器号落在 `1 .. (p->nMem+1 - p->nCursor)` 之间且 `memIsValid(&aMem[...])` 成立；`OPFLG_OUT2`/`OPFLG_OUT3` 要求先调 `memAboutToChange(p, &aMem[...])`。
- `sqlite3_interrupt_count` 递减（仅 `SQLITE_TEST` 下）：模拟中断以测试中断路径。

之后进入 `switch( pOp->opcode )`。整个 switch 是一个巨大的 case 集合，每条指令一个 case。

### 跳转的三种写法

跳转指令共用几个标签，构成控制流的骨架：

- **`jump_to_p2_and_check_for_interrupt`**（`OP_Goto` / `OP_Gosub` 走这里）：`pOp = &aOp[pOp->p2 - 1];` 然后落到 `check_for_interrupt`。
- **`check_for_interrupt`**：这是**循环底部**的统一检查点。`OP_Next`、`OP_Prev`、`OP_VNext`、`OP_SorterNext` 等「循环最后一次迭代」的指令都在完成后跳到这里。检查内容有两项：中断标志（`AtomicLoad(&db->u1.isInterrupted)`），以及进度回调——`while( nVmStep>=nProgressLimit && db->xProgress!=0 )` 每次把 `nProgressLimit += db->nProgressOps`，回调返回非零则置 `rc = SQLITE_INTERRUPT` 并 `goto abort_due_to_error`。
- **`jump_to_p2`**（`OP_InitCoroutine` 走这里）：`pOp = &aOp[pOp->p2 - 1];` 但**不**做中断/进度检查。

源码注释明确写了这个「用非结构化 goto 写检查点」的取舍动机：这样写是为了性能，避免在每个 opcode 上都跑一遍中断与进度检查，注释给出的量化结论是「helps sqlite3_step() to run about 1.5% faster according to \"valgrind --tool=cachegrind\"」。

### 寄存器与内存单元

寄存器就是 `Mem` 数组 `p->aMem[]`，循环开始时被复制到局部变量 `aMem`，因此每条指令读写寄存器走的是 `aMem[pOp->p1]` 这样的直接下标。三个输入/输出指针 `pIn1`/`pIn2`/`pIn3`/`pOut` 是复用的临时变量，避免反复声明。

指令的输入输出角色由 `sqlite3OpcodeProperty[]` 表描述，该表由 `tool/mkopcodeh.tcl` 扫描 `src/vdbe.c` 里 `case OP_` 行后的注释生成（注释里的 `in1`、`in2`、`in3`、`out2`、`out3` 关键字构成 `OPFLG_INITIALIZER`）。同一条注释里的「`/# same as ... #/`」用来决定 opcode 的具体编号。

### 结束路径：`OP_Halt`

`OP_Halt` 是正常终止的出口，语义分两种：

1. **子程序返回**：当 `p->pFrame` 非空且 `pOp->p1==SQLITE_OK` 时，这是一条子程序（典型是触发器）自己的 halt。处理是：取 `pFrame = p->pFrame`，把 `p->pFrame` 退到 `pFrame->pParent`，`p->nFrame--`，调 `sqlite3VdbeSetChanges(db, p->nChange)` 上报变更计数，调 `sqlite3VdbeFrameRestore(pFrame)` 恢复父程序的 `aOp`/`nOp`/`aMem`/`nMem`/`apCsr`/`nCursor`/`lastRowid`/`nChange`，然后把 `pOp` 指回父程序继续跑。若 `pOp->p2==OE_Ignore`（子程序抛出 IGNORE 异常），额外跳到调用方 `OP_Program` 的 `p2` 指定地址。
2. **整条语句终止**：`p->rc = pOp->p1; p->errorAction = (u8)pOp->p2;`。若 `p->rc` 非零则按 `p3`/`p4`/`p5` 组织错误消息——`p5` 取值 1–4 分别对应 `"NOT NULL"`、`"UNIQUE"`、`"CHECK"`、`"FOREIGN KEY"` 四类约束，取自 `azType[]` 数组。随后调 `rc = sqlite3VdbeHalt(p)`。

`sqlite3VdbeHalt`（`src/vdbeaux.c`）是「把 VM 从 RUN 状态迁到 HALT 状态」的唯一途径：它先 `closeAllCursors(p)`，再根据 `p->bIsReader` 与错误类型决定提交还是回滚——`SQLITE_NOMEM`、`SQLITE_IOERR`、`SQLITE_INTERRUPT`、`SQLITE_FULL` 四类「特殊错误」会触发回滚（`sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)` 或 `SAVEPOINT_ROLLBACK`），最后 `sqlite3VdbeEnter`/`sqlite3VdbeLeave` 成对释放 btree 锁。

回到 `OP_Halt`：若 `rc==SQLITE_BUSY` 则 `p->rc = SQLITE_BUSY`；否则 `rc = p->rc ? SQLITE_ERROR : SQLITE_DONE;`——**语句结束时返回 `SQLITE_DONE`，出错时返回 `SQLITE_ERROR`**，然后 `goto vdbe_return`。

### 异常路径

四条异常路径最终都汇到 `abort_due_to_error`：

- **`abort_due_to_interrupt`**：`rc = SQLITE_INTERRUPT; goto abort_due_to_error;`。由 `sqlite3_interrupt()` 设置的 `db->u1.isInterrupted` 标志触发。
- **`no_mem`**：`sqlite3OomFault(db); sqlite3VdbeError(p, "out of memory"); rc = SQLITE_NOMEM_BKPT;`。
- **`too_big`**：`sqlite3VdbeError(p, "string or blob too big"); rc = SQLITE_TOOBIG;`。触发点是长度检查，例如 `if( len>db->aLimit[SQLITE_LIMIT_LENGTH] ) goto too_big;`，而 `SQLITE_LIMIT_LENGTH` 的运行时默认值来自 `src/main.c` 的 `aHardLimit[]`，其首项是 `SQLITE_MAX_LENGTH`，编译期默认值定义在 `src/sqliteLimit.h`。
- **`abort_due_to_error`**：错误汇聚点。处理顺序是——若 `db->mallocFailed` 则 `rc = SQLITE_NOMEM_BKPT`；若 `rc==SQLITE_IOERR_CORRUPTFS` 则 `rc = SQLITE_CORRUPT_BKPT`；若尚无错误消息则 `sqlite3VdbeError(p, "%s", sqlite3ErrStr(rc))` 补上；`p->rc = rc`；`sqlite3SystemError(db, rc)`；`sqlite3VdbeLogAbort(p, rc, pOp, aOp)` 记日志；`if( p->eVdbeState==VDBE_RUN_STATE ) sqlite3VdbeHalt(p);` 做提交/回滚收尾；若 `rc==SQLITE_IOERR_NOMEM` 则 `sqlite3OomFault(db)`；若 `rc==SQLITE_CORRUPT && db->autoCommit==0` 则置 `db->flags |= SQLITE_CorruptRdOnly`。最后 `rc = SQLITE_ERROR;`。

注意 `abort_due_to_error` 里 `rc = SQLITE_ERROR;` 这一句：它把具体错误码收进 `p->rc` 与 `p->zErrMsg`，而**返回给调用方的统一是 `SQLITE_ERROR`**；真正的细分错误码由 `sqlite3_step` 侧的 `sqlite3VdbeTransferError(p)` 取回。

### 触发器子程序：`OP_Program`

`OP_Program` 用于执行触发器子程序：它把父程序的 `aOp`/`aMem`/`apCsr` 存进一个 `VdbeFrame`，再把 `pOp` 切到子程序的操作码数组。嵌套深度受 `db->aLimit[SQLITE_LIMIT_TRIGGER_DEPTH]` 限制，超过则 `rc = SQLITE_ERROR; sqlite3VdbeError(p, "triggers nested too deep"); goto abort_due_to_error;`。该 limit 的编译期硬上限是 `src/sqliteLimit.h` 里的 `SQLITE_MAX_TRIGGER_DEPTH`（默认 1000）。

## 关键取舍

- **指令指针而非程序计数器**：跳转通过改写 `pOp` 指针实现，循环靠 `pOp++` 推进。这让「跳到任意指令」与「顺序执行」共用同一套代码路径，代价是每条指令末尾都要能被 `pOp++` 正确接续——所以 `p2` 存的是目标地址加一，取指时统一 `- 1`。
- **错误靠 goto 汇聚而非返回值**：每条指令内部发现错误时直接 `goto abort_due_to_error`，而不是层层返回错误码。源码注释在循环开头写明了这个约定：「Errors are detected by individual opcodes, with an immediate jumps to abort_due_to_error.」
- **中断与进度检查放在循环底部**：不逐指令检查，而是让「循环最后一次迭代」的指令统一跳到 `check_for_interrupt`。源码注释给出了动机——为了性能，避免每条 opcode 都跑检查，量化结果是按 `valgrind --tool=cachegrind` 测得 `sqlite3_step()` 快约 1.5%。代价是**单条长耗时指令内部的中断不会被及时响应**，只能在指令返回后才被感知。
- **返回码坍缩为 `SQLITE_ERROR`**：虚拟机对外只返回 `SQLITE_OK`/`SQLITE_ROW`/`SQLITE_DONE`/`SQLITE_ERROR`/`SQLITE_BUSY`/`SQLITE_NOMEM` 这一小撮，细分码保存在 `p->rc` 里。`abort_due_to_error` 末尾那行 `rc = SQLITE_ERROR;` 就是这条规则的落点。
- **寄存器数组在编译期定长**：`aMem` 的大小由 `sqlite3VdbeMakeReady` 按 `pParse->nMem` 一次性确定，运行时不再增长。这让执行循环可以用裸下标 `aMem[pOp->p1]` 而不必查边界——代价是寄存器数在生成期就必须算准，执行期无法临时申请新寄存器。
- **子程序用帧（frame）而非递归**：`OP_Program` 把父程序状态压进 `VdbeFrame`，`OP_Halt` 通过 `sqlite3VdbeFrameRestore` 弹回，避免了 C 栈递归带来的深度与栈空间问题；深度改由 `SQLITE_LIMIT_TRIGGER_DEPTH` 这个**计数上限**来管，而不是靠栈溢出自然终止。
- **调试期不变量检查走查表**：`sqlite3OpcodeProperty[]` 由工具从源码注释自动生成，而不是手工维护的表，避免了两份事实来源不一致。

## 与主核 / AI 的关系

本机制是 SQLite 执行链的**最底一层运行时**：上游是 `sqlite3VdbeMakeReady`（`src/vdbeaux.c`）交付的封箱程序，由 `src/vdbeapi.c` 的 `sqlite3_step` 调入；下游是 `sqlite3VdbeHalt`（`src/vdbeaux.c`）负责的提交/回滚收尾，再往下是 B-Tree（`btree.c`）与 Pager（`pager.c`）。这条链上「操作码语义」与「操作码生成」被严格切在 `src/vdbe.c` 与代码生成器之间——`src/vdbe.c` 的 `case OP_` 行同时被 `tool/mkopcodeh.tcl` 扫描成 opcode 编号表，说明指令集本身是**源码即定义**。

**架构推论：** 主数据库内核（`engine/db/`）在 `engine/db/` 内自持实现，没有采用字节码虚拟机的路子，而是直接在查询层走 AST 求值；本页给出的三条可迁移经验是——(1) 「执行期错误统一汇聚到一处」比逐层返回错误码更容易保证一致性，主核若保留逐层返回，需要额外的一致性审计；(2) 「中断检查放在循环底部而非每条指令」是一个显式的延迟取舍，主核的向量化算子若有长耗时阶段，需要同样明确「中断在算子边界才被响应」；(3) 「寄存器/内存单元数量在编译期定长」与主核的算子内存模型直接相关，迁移时需要把「执行期临时申请」的代价算清楚。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbe.c | `sqlite3VdbeExec` | 虚拟机主循环：进入前准备（`sqlite3VdbeEnter`、进度阈值、中断初查），`for(pOp=&aOp[p->pc]; 1; pOp++)` 取指执行循环，`switch( pOp->opcode )` 分派，收束于 `vdbe_return` |
| src/vdbe.c | `OP_Goto` | 无条件跳转：`pOp = &aOp[pOp->p2 - 1];`，落到 `check_for_interrupt` |
| src/vdbe.c | `OP_Gosub` | 把当前地址写入寄存器 P1 后跳转到 P2，末尾 `goto jump_to_p2_and_check_for_interrupt` |
| src/vdbe.c | `OP_Return` | 从寄存器 P1 取出返回地址跳转；P3 非零时若 P1 不是整数则退化为空操作 |
| src/vdbe.c | `OP_Halt` | 正常终止出口：子程序返回走 `sqlite3VdbeFrameRestore`，语句终止走 `sqlite3VdbeHalt`，末尾 `rc = p->rc ? SQLITE_ERROR : SQLITE_DONE` |
| src/vdbe.c | `OP_Next` | 游标前进：`sqlite3BtreeNext` 成功后跳 `jump_to_p2_and_check_for_interrupt`，返回 `SQLITE_DONE` 时跳 `check_for_interrupt` |
| src/vdbe.c | `OP_Program` | 执行触发器子程序：压入 `VdbeFrame`、切换操作码数组，嵌套过深时报 "triggers nested too deep" |
| src/vdbe.c | `OP_InitCoroutine` | 协程初始化，末尾走 `jump_to_p2` 标签（跳转但不做中断/进度检查） |
| src/vdbe.c | `OP_Yield` | 与寄存器 P1 交换程序计数器，实现协程让出 |
| src/vdbeaux.c | `sqlite3VdbeEnter` | 按 `p->lockMask` 对涉及的 btree 调 `sqlite3BtreeEnter` 加锁 |
| src/vdbeaux.c | `sqlite3VdbeLeave` | 与 `sqlite3VdbeEnter` 成对，按 `p->lockMask` 调 `sqlite3BtreeLeave` 解锁 |
| src/vdbeaux.c | `sqlite3VdbeHalt` | VM 从 RUN 到 HALT 的唯一途径：`closeAllCursors`、按错误类型决定提交或回滚、释放 btree 锁 |
| src/vdbeaux.c | `sqlite3VdbeFrameRestore` | 从 `VdbeFrame` 恢复父程序的 `aOp`/`nOp`/`aMem`/`nMem`/`apCsr`/`nCursor`/`lastRowid`/`nChange`，返回父程序地址 |
| src/vdbeaux.c | `sqlite3VdbeSetChanges` | 把变更计数写入 `db->nChange` 并累加 `db->nTotalChange` |
| src/vdbeaux.c | `sqlite3VdbeList` | `EXPLAIN` 路径：按 `p->explain` 取值输出操作码清单，不进入 `sqlite3VdbeExec` 的主循环 |
| src/vdbeapi.c | `sqlite3_step` | 主循环的唯一调用方：置 `p->pc = 0` 与 `p->eVdbeState = VDBE_RUN_STATE` 后调 `sqlite3VdbeExec(p)`，把 `SQLITE_ROW` 直返，其余路径经 `sqlite3VdbeTransferError` 取回细分错误码 |
| src/main.c | `sqlite3_progress_handler` | 进度回调注册入口：`nOps>0` 时写 `db->xProgress`、`db->nProgressOps`、`db->pProgressArg`，决定主循环的检查频率 |
| src/main.c | `aHardLimit` | 各运行时 limit 的默认值数组，首项 `SQLITE_MAX_LENGTH`、含 `SQLITE_MAX_TRIGGER_DEPTH`，是主循环里长度与嵌套深度检查的阈值来源 |
| src/sqliteLimit.h | `SQLITE_MAX_LENGTH` | 字符串/Blob 长度上限的编译期默认值，运行时经 `SQLITE_LIMIT_LENGTH` 暴露 |
| src/sqliteLimit.h | `SQLITE_MAX_TRIGGER_DEPTH` | 触发器嵌套深度的编译期硬上限，默认 1000 |
| src/sqlite.h.in | `SQLITE_STMTSTATUS_VM_STEP` | 语句状态计数器槽位，主循环每步 `nVmStep++`、退出时累加到 `p->aCounter[SQLITE_STMTSTATUS_VM_STEP]` |
| src/sqliteInt.h | `sqlite3` 的 `nProgressOps` 字段 | 进度回调的触发步数间隔，主循环用它算出 `nProgressLimit` |

## 参考

- [The SQLite Bytecode Engine（VDBE 官方文档，含全部 opcode 语义）](https://sqlite.org/opcode.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/vdbe.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/vdbe.c)
- [sqlite/sqlite 仓库中的 src/vdbe.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c)
- [sqlite/sqlite 仓库中的 src/vdbeaux.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c)
- [SQLite VDBE 编程接口（sqlite3_step 官方文档）](https://sqlite.org/c3ref/step.html)
