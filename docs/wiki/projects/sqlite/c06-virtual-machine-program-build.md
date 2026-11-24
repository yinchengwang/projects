---
project: sqlite
feature: virtual-machine-program-build
page_type: feature
schema_version: 1
refs:
  - path: src/vdbeaux.c
    symbols: [sqlite3VdbeAddOp0, sqlite3VdbeAddOp1, sqlite3VdbeAddOp2, sqlite3VdbeAddOp3, sqlite3VdbeAddOp4]
  - path: src/build.c
    symbols: [sqlite3VdbeMakeReady]
---

# 虚拟机构建

## 一句话职责

把代码生成阶段决定好的「做什么」翻译成一条可以直接交给虚拟机执行的 opcode 序列——追加操作码、分配寄存器、登记跳转标签，最后把这段程序封箱（MakeReady），使它成为与 Parse 对象无关、可独立运行的自足实体。

## 机制是什么

SQLite 的执行模型是「编译成字节码，再由虚拟机跑字节码」。代码生成器（`expr.c`、`select.c`、`where.c` 等）负责决定语义；本机制负责把语义落到一条**线性操作码数组**上。

一条 VDBE 程序由若干条指令组成，每条指令含一个 opcode 与 P1–P5 五个操作数。程序构建期要解决四件事：

1. **追加指令**：`sqlite3VdbeAddOp0` / `sqlite3VdbeAddOp1` / `sqlite3VdbeAddOp2` / `sqlite3VdbeAddOp3` / `sqlite3VdbeAddOp4` 是唯一的入口（`sqlite3VdbeAddOp4` 在 `sqlite3VdbeAddOp3` 之上补一个 P4 操作数）。它们返回新指令的地址，地址就是后续打补丁的锚点。
2. **分配寄存器**：寄存器编号从 0 开始，代码生成期通过 `Parse.nMem` 记录「已用寄存器数」，`sqlite3VdbeMakeReady` 据此分配 `aMem[]` 数组。参数寄存器 `aVar[]`、游标指针 `apCsr[]` 同理。
3. **解析跳转标签**：跳转指令的 P2 在生成期先写成**负的标签编号**（`Parse.aLabel` 维护标签表），`sqlite3VdbeMakeReady` 结尾的 `resolveP2Values` 反向扫描整条程序，把负标签替换成绝对指令地址。
4. **封箱**：`sqlite3VdbeMakeReady` 把 VDBE 从 Parse 上摘下来（`p->pVList = pParse->pVList; pParse->pVList = 0;`），此后禁止再调用任何 `sqlite3VdbeAddOp*`。

本页不涉及 opcode 的**执行语义**（那是 `vdbe.c` 的 `sqlite3VdbeExec` 负责的），也不涉及查询规划。

## 怎么实现的

### 追加操作码：`sqlite3VdbeAddOp3` 是唯一的真身

`sqlite3VdbeAddOp0/1/2` 都是 `sqlite3VdbeAddOp3` 的薄包装，把没用的操作数补 0：

- `sqlite3VdbeAddOp0(p, op)` → `sqlite3VdbeAddOp3(p, op, 0, 0, 0)`
- `sqlite3VdbeAddOp1(p, op, p1)` → `sqlite3VdbeAddOp3(p, op, p1, 0, 0)`
- `sqlite3VdbeAddOp2(p, op, p1, p2)` → `sqlite3VdbeAddOp3(p, op, p1, p2, 0)`

`sqlite3VdbeAddOp3` 的流程：

1. 取 `i = p->nOp` 作为新指令的下标。
2. 断言 `p->eVdbeState==VDBE_INIT_STATE`（程序必须还处于「生成中」状态）且 `op>=0 && op<0xff`（opcode 必须落在单字节范围内）。
3. 若 `p->nOpAlloc<=i`（数组已满），转 `growOp3` 慢路径。
4. 否则 `p->nOp++`，填 `pOp->opcode`、`p1`、`p2`、`p3`，并把 `p4.p` 置 0、`p4type` 置 `P4_NOTUSED`、`p5` 置 0。
5. 按编译期开关补零：`SQLITE_ENABLE_EXPLAIN_COMMENTS` 下清 `zComment`；`SQLITE_ENABLE_STMT_SCANSTATUS` 或 `VDBE_PROFILE` 下清 `nExec`/`nCycle`；`SQLITE_VDBE_COVERAGE` 下清 `iSrcLine`；`SQLITE_DEBUG` 且 db 带 `SQLITE_VdbeAddopTrace` 标志时调 `sqlite3VdbePrintOp` 打印刚插入的指令。
6. 返回 `i`。

`sqlite3VdbeAddOp4` 则是「先 `sqlite3VdbeAddOp3` 拿地址，再 `sqlite3VdbeChangeP4` 补 P4」。`sqlite3VdbeChangeP4` 的分支逻辑是：若 `n>=0` 或该指令已有 `p4type`，走 `vdbeChangeP4Full`（先释放旧 P4，再用 `sqlite3DbStrNDup` 复制字符串、置 `P4_DYNAMIC`）；否则若 `n==P4_INT32` 直接写 `p4.i`；否则把 `zP4` 指针原样存进 `p4.p` 并把 `n` 写成 `p4type`，遇到 `P4_VTAB` 时额外调 `sqlite3VtabLock`。`addr<0` 时表示「改最近一条指令」，取 `p->nOp - 1`。

### 数组扩容：`growOpArray` 与 `SQLITE_LIMIT_VDBE_OP`

`growOpArray` 决定新数组容量：

- 默认（无 `SQLITE_TEST_REALLOC_STRESS`）：`nNew = 2*nOpAlloc`（翻倍）；若 `nOpAlloc` 为 0，初值为 `1024/sizeof(Op)` 条指令。
- 定义了 `SQLITE_TEST_REALLOC_STRESS`（仅测试用）：容量未到 512 条时每次只加 `nOp` 条，到 512 条后翻倍。
- 硬上限：`if( nNew > p->db->aLimit[SQLITE_LIMIT_VDBE_OP] )` 就调 `sqlite3OomFault(p->db)` 并返回 `SQLITE_NOMEM`。`SQLITE_LIMIT_VDBE_OP` 的编译期硬上限来自 `sqliteLimit.h` 里的 `SQLITE_MAX_VDBE_OP`（默认 250000000），运行时默认值由 `main.c` 的 `aHardLimit[SQLITE_LIMIT_VDBE_OP]` 给出，可用 `sqlite3_limit()` 调整。
- 扩容用 `sqlite3DbRealloc`；成功后 `p->szOpAlloc = sqlite3DbMallocSize(...)`，`v->nOpAlloc = szOpAlloc/sizeof(Op)`。

扩容失败时 `aOp` 与 `nOpAlloc` 保持不变，已分配的操作码仍能随 Vdbe 一起正确释放（`growOpArray` 注释明确说明这一点）。

### 标签与跳转地址：`resolveP2Values`

代码生成期跳转指令的 P2 写的是**负的标签号**，真实地址要到封箱时才知道。`resolveP2Values` 在 `sqlite3VdbeMakeReady` 里被调用，它从最后一条指令**反向**遍历，止于首条的 `OP_Init`：

- 只处理 `pOp->opcode <= SQLITE_MX_JUMP_OPCODE` 的指令；`SQLITE_MX_JUMP_OPCODE` 由 `tool/mkopcodeh.tcl` 生成（该脚本刻意把所有 JUMP 类 opcode 排在 opcode 表靠前位置以缩短这个扫描）。
- 遇到 `OP_Transaction`（P2 非 0 时把 `p->readOnly` 置 0）、`OP_AutoCommit`、`OP_Savepoint` → `p->bIsReader = 1`；遇到 `OP_Checkpoint`、`OP_Vacuum`、`OP_JournalMode` → `p->readOnly = 0` 且 `p->bIsReader = 1`。
- 遇到 `OP_VUpdate` 时用其 P2 更新 `nMaxVtabArgs`；遇到 `OP_VFilter` 时读**紧邻前一条** `OP_Integer` 的 P1 作为 argc 更新 `nMaxVtabArgs`。
- 默认分支：若 `pOp->p2 < 0`，则执行 `pOp->p2 = aLabel[ADDR(pOp->p2)]`，把负标签号换成绝对地址。
- 收尾：释放 `pParse->aLabel`，置 `pParse->nLabel = 0`，并把 `nMaxVtabArgs` 通过 `*pMaxVtabArgs` 返回。

### 封箱：`sqlite3VdbeMakeReady`

`sqlite3VdbeMakeReady(p, pParse)` 由 `src/build.c` 的 `sqlite3FinishCoding` 在 `pParse->nErr==0` 时调用（调用后 `pParse->rc = SQLITE_DONE`；若 `pParse->nErr` 非 0 则走 `pParse->rc = SQLITE_ERROR` 分支，不封箱）。它的步骤：

1. `p->pVList = pParse->pVList; pParse->pVList = 0;`——把变量清单从 Parse 移到 VDBE，使两者解耦。
2. 取四个规模：`nVar = pParse->nVar`（参数个数）、`nMem = pParse->nMem`（已用寄存器数）、`nCursor = pParse->nTab`（游标数）、`nArg = pParse->nMaxArg`（虚表 xFilter/xUpdate 最大参数数）。
3. **寄存器数还要加游标数**：`nMem += nCursor;`——每个游标占用一个内存单元；游标 0 可以复用 `aMem[0]`，其余游标在 `aMem[]` 尾部另占位置。若 `nCursor==0 && nMem>0`，再 `nMem++` 给 `aMem[0]` 留位。
4. **复用操作码数组尾部**：`n = ROUND8P(sizeof(Op)*p->nOp)` 算出已用操作码字节数，`x.pSpace` 指向 `p->aOp` 之后的空闲区，`x.nFree = ROUNDDOWN8(pParse->szOpAlloc - n)`。
5. 调 `resolveP2Values(p, &nArg)` 解析跳转标签并回填虚表参数上限。
6. `p->usesStmtJournal = (u8)(pParse->isMultiWrite && pParse->mayAbort)`。
7. 若 `pParse->explain` 打开：`if( nMem<10 ) nMem = 10;`（至少 10 个寄存器），`p->nResColumn = 12 - 4*p->explain`。
8. **两趟内存分配**：`allocSpace` 先试着从 `x.pSpace` 的空闲区切出 `aMem`（`nMem*sizeof(Mem)`）、`aVar`（`nVar*sizeof(Mem)`）、`apArg`（`nArg*sizeof(Mem*)`）、`apCsr`（`nCursor*sizeof(VdbeCursor*)`）；切不下的部分记入 `x.nNeeded`，第二趟统一 `sqlite3DbMallocRawNN` 一块新内存再切分。注释明确这是为了「显著减少 prepared statement 持有的内存」。
9. 若 `db->mallocFailed`：把 `p->nVar`、`p->nCursor`、`p->nMem` 全置 0；否则 `p->nCursor = nCursor`、`p->nVar = nVar`，用 `initMemArray` 初始化 `aVar`（标志 `MEM_Null`）与 `aMem`（标志 `MEM_Undefined`），并 `memset` 清空 `apCsr`。
10. 最后 `sqlite3VdbeRewind(p)`：置 `p->eVdbeState = VDBE_READY_STATE`、`p->pc = -1`、`p->rc = SQLITE_OK`、`p->errorAction = OE_Abort`、`p->nChange = 0`、`p->cacheCtr = 1`、`p->minWriteFileFormat = 255`、`p->iStatement = 0`、`p->nFkConstraint = 0`。

### 寄存器分配发生在哪

寄存器编号不是在本机制里分配的，而是代码生成期通过 `Parse.nMem` 记账：`sqlite3GetTempRange` 在临时寄存器池不够时执行 `i = pParse->nMem+1; pParse->nMem += nReg;`；`sqlite3TouchRegister` 在 `pParse->nMem < iReg` 时把 `nMem` 抬到 `iReg`。本机制只在 `sqlite3VdbeMakeReady` 里读 `pParse->nMem` 来确定 `aMem[]` 的实际大小。

## 关键取舍

- **单字节 opcode + 五个操作数**：opcode 必须在 `0..0xff` 范围内（`sqlite3VdbeAddOp3` 的断言），限制了单个程序的操作码种类上限，但换来了紧凑的指令编码与 O(1) 的 `sqlite3OpcodeProperty[]` 查表。
- **生成期用「标签」而非地址**：跳转目标在生成期还不可知，用负标签号占位，把地址解析推迟到 `sqlite3VdbeMakeReady` 一次做完。这让代码生成器可以任意顺序地前后跳转而不需要回填补丁。代价是必须维护 `Parse.aLabel` 标签表，并在封箱时反向扫描整条程序。
- **操作码数组翻倍扩容**：默认每次容量翻倍（`nNew = 2*nOpAlloc`），用内存冗余换摊还 O(1) 的追加；代价是尾部最多浪费近一半空间——`sqlite3VdbeMakeReady` 正是把这段浪费的空间回收给 `aMem`/`aVar`/`apArg`/`apCsr`。
- **P4 走「指针 vs 副本」两条路**：`sqlite3VdbeChangeP4` 用 `n` 的正负区分两种语义——`n>=0` 表示复制字符串（`P4_DYNAMIC`，需 `freeP4` 释放），`n<0` 且非 `P4_INT32` 表示存指针（`P4_STATIC`、`P4_COLLSEQ`、`P4_VTAB` 等，生命周期由 VDBE 保证）。`P4_INT32` 单独走 `p4.i` 整数槽。
- **错误优先走 OOM 通道**：`growOpArray` 触发上限时用 `sqlite3OomFault` 置 `db->mallocFailed`，让后续调用统一在 `db->mallocFailed` 分支上短路，而不是在每个调用点分散处理。
- **封箱后彻底封死**：`sqlite3VdbeMakeReady` 把 VDBE 与 Parse 解绑并置 `VDBE_READY_STATE`，此后调用 `sqlite3VdbeAddOp*` 会触发断言失败。**架构推论：** 这是为了让「程序构建期」与「程序执行期」在状态机上有明确边界，`resolveP2Values` 里那些 `assert(p->eVdbeState==VDBE_INIT_STATE)` 就是这条边界的守卫。

## 与主核 / AI 的关系

本机制是 SQLite「SQL 文本 → 字节码」编译链的最后一环：上游是 `expr.c` / `select.c` / `where.c` 等代码生成器决定发什么 opcode，`sqlite3FinishCoding`（`src/build.c`）负责补上收尾指令（`OP_Halt`、各库的 `OP_Transaction`、虚表的 `OP_VBegin`）后调用 `sqlite3VdbeMakeReady` 封箱；下游是 `vdbe.c` 的虚拟机执行这段程序。

**架构推论：** 主数据库内核（`engine/db/`）如果要走 SQLite 这条路，`sqlite3VdbeMakeReady` 揭示的「寄存器数组大小在编译期一次性确定」这一约束，对自研内核的寄存器分配器设计有直接参考价值——主核当前是在 `engine/db/` 内自持实现，若要换成「运行时增长」的寄存器模型，对应的就是把 `aMem[]` 从定长数组换成可增长结构。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbeaux.c | `sqlite3VdbeAddOp0` | 追加无操作数指令，内部转调 `sqlite3VdbeAddOp3(p, op, 0, 0, 0)` |
| src/vdbeaux.c | `sqlite3VdbeAddOp1` | 追加单操作数指令，内部转调 `sqlite3VdbeAddOp3(p, op, p1, 0, 0)` |
| src/vdbeaux.c | `sqlite3VdbeAddOp2` | 追加双操作数指令，内部转调 `sqlite3VdbeAddOp3(p, op, p1, p2, 0)` |
| src/vdbeaux.c | `sqlite3VdbeAddOp3` | 追加指令的唯一实现：分配下标、填 opcode/P1–P3、置 `P4_NOTUSED`、按编译期开关清零可选字段，返回新指令地址 |
| src/vdbeaux.c | `sqlite3VdbeAddOp4` | 在 `sqlite3VdbeAddOp3` 之上追加 P4 操作数，调用 `sqlite3VdbeChangeP4` 写入指针或字符串副本 |
| src/vdbeaux.c | `sqlite3VdbeChangeP4` | 改写指定指令（`addr<0` 表示最近一条）的 P4；`n>=0` 走 `vdbeChangeP4Full` 复制字符串置 `P4_DYNAMIC`，`n==P4_INT32` 写整数槽，其余负值按指针存并置 `p4type` |
| src/vdbeaux.c | `growOp3` | `sqlite3VdbeAddOp3` 的扩容慢路径：先 `growOpArray` 扩数组，再递归回 `sqlite3VdbeAddOp3` |
| src/vdbeaux.c | `growOpArray` | 操作码数组扩容：默认容量翻倍（初值 `1024/sizeof(Op)` 条），超出 `db->aLimit[SQLITE_LIMIT_VDBE_OP]` 则 `sqlite3OomFault` 并返回 `SQLITE_NOMEM` |
| src/vdbeaux.c | `sqlite3VdbeAddOp4Int` | 追加 P4 为 32 位整数的指令；`p4type` 置 `P4_INT32` |
| src/vdbeaux.c | `resolveP2Values` | 反向扫描操作码数组，把跳转指令的负标签号替换为绝对地址，并回填虚表参数上限与只读/读标志 |
| src/vdbeaux.c | `allocSpace` | 优先从操作码数组尾部空闲区切分内存，切不下时累加到 `nNeeded` 留待第二趟统一分配 |
| src/vdbeaux.c | `initMemArray` | 初始化 `Mem` 数组：置 `flags`、`db`、`szMalloc`（DEBUG 下另清 `pScopyFrom`/`bScopy`） |
| src/vdbeaux.c | `sqlite3VdbeJumpHere` | 把跳转指令的 P2 改为「下一条待生成指令的地址」（即 `p->nOp`） |
| src/vdbeaux.c | `sqlite3VdbeChangeToNoop` | 把某条指令改成 `OP_Noop`，并按 `p4type` 调 `freeP4` 释放旧 P4 |
| src/vdbeaux.c | `sqlite3VdbeTakeOpArray` | 取走整条操作码数组（触发 `resolveP2Values`），用于把子程序字节码移交给父程序 |
| src/build.c | `sqlite3VdbeMakeReady` | 封箱入口：摘走 `pParse->pVList`、按 `pParse->nMem/nVar/nTab/nMaxArg` 定规模、复用操作码尾部内存分配寄存器数组、调 `resolveP2Values` 解析跳转、最后 `sqlite3VdbeRewind` 进入 `VDBE_READY_STATE` |
| src/build.c | `sqlite3FinishCoding` | `sqlite3VdbeMakeReady` 的实际调用方：补 `OP_Halt`、各库 `OP_Transaction`、虚表 `OP_VBegin` 后，在 `pParse->nErr==0` 时封箱并置 `pParse->rc = SQLITE_DONE` |
| src/expr.c | `sqlite3GetTempRange` | 寄存器分配的实际发生点：临时寄存器池不足时 `pParse->nMem += nReg` 扩张已用寄存器数 |
| src/expr.c | `sqlite3TouchRegister` | 把 `pParse->nMem` 抬到指定寄存器号以上，保证该寄存器号在 `aMem[]` 范围内 |
| src/sqliteInt.h | `Parse` | 承载 `nMem`（已用寄存器数）、`nTab`（游标数）、`nVar`（参数数）、`nMaxArg`（虚表参数上限）、`nLabel`/`aLabel`（跳转标签表）、`szOpAlloc`（操作码数组已分配字节数） |
| src/vdbe.h | `P4_NOTUSED` / `P4_INT32` / `P4_DYNAMIC` / `P4_STATIC` / `P4_COLLSEQ` / `P4_VTAB` | P4 操作数的类型标记，决定 `sqlite3VdbeChangeP4` 与 `freeP4` 如何处置 P4 值 |
| src/sqliteLimit.h | `SQLITE_MAX_VDBE_OP` | 单个 VDBE 操作码条数的编译期硬上限，默认 250000000 |
| src/main.c | `SQLITE_LIMIT_VDBE_OP` | 该上限对应的运行时 limit 槽位，由 `sqlite3_limit()` 调整，扩容时在 `growOpArray` 内被检查 |
| tool/mkopcodeh.tcl | `SQLITE_MX_JUMP_OPCODE` | 生成的最大 JUMP 类 opcode 编号，`resolveP2Values` 据此剪枝扫描范围 |

## 参考

- [The SQLite Bytecode Engine（VDBE 官方文档）](https://sqlite.org/opcode.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/vdbeaux.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/vdbeaux.c)
- [sqlite/sqlite 仓库中的 src/vdbeaux.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c)
- [EXPLAIN 查询计划（官方文档）](https://sqlite.org/lang_explain.html)
