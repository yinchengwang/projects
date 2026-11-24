---
project: sqlite
feature: user-defined-functions
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_create_function, sqlite3CreateFunc]
  - path: src/vdbeapi.c
    symbols: [sqlite3_result_text, sqlite3_result_int64, sqlite3_value_blob, sqlite3_user_data]
---

# 用户自定义函数

## 一句话职责

把应用注册的 C 回调登记到指定连接的函数表里，并在 SQL 执行时把该回调原样调起、把回调写回的返回值变成 SQL 值。

## 机制是什么

用户自定义函数（UDF）在 SQLite 里就是一张「连接级函数表 + 一组 C 回调指针」。应用调用 `sqlite3_create_function`（或 `sqlite3_create_function_v2`、`sqlite3_create_window_function`）把回调指针连同用户数据指针注册进**该 `sqlite3` 连接**的函数哈希表 `db->aFunc`；语句执行到调用点时，VDBE 从同一张表里取出该函数的 `FuncDef`，构造 `sqlite3_context`，把参数以 `sqlite3_value *` 数组交给应用回调；回调用 `sqlite3_result_*` 系列把结果写回 `sqlite3_context` 指向的输出 `Mem`，VDBE 再把这个 `Mem` 当作该表达式的结果继续运算。

一个 `FuncDef` 里并存两组回调指针：标量函数用 `xSFunc`（`xSFunc` 为 0 时 `xSFunc` 槽位实际存的是 `xStep`），聚合/窗口函数用 `xStep` / `xFinalize` / `xValue` / `xInverse`。`sqlite3CreateFunc` 明确断言 `xSFunc` 与 `xFinal` 不能同时非空，且 `xFinal` 与 `xStep` 必须同时有或同时没有——即「标量」与「聚合」是互斥的两种形态，不存在「一个函数既是标量又是聚合」的注册方式。

聚合函数的累加器约定由 `sqlite3_aggregate_context` 承担：首次调用时按 `nByte` 分配一块内存并把 `MEM_Agg` 标志打在累加器 `Mem` 上，后续调用返回同一块内存的指针；`xFinalize` 在聚合结束时被调用一次，把最终值写回输出 `Mem`。窗口函数在此之上额外要求 `xValue` 与 `xInverse` 成对提供，用于在窗口帧内取值与反向撤销。

本机制的边界是「登记 + 回调 + 结果写回 + 聚合累加器约定」。内置函数的注册与分派属于 l01，参数绑定（`sqlite3_bind_*`）属于 E04，本机制不涉及。

## 怎么实现的

**注册路径**

1. 应用调用 `sqlite3_create_function`，它把参数原样转交给 `createFunctionApi`（`src/main.c`）。`createFunctionApi` 在 `SQLITE_ENABLE_API_ARMOR` 打开时先过 `sqlite3SafetyCheckOk`，然后 `sqlite3_mutex_enter(db->mutex)` 持连接互斥锁。
2. 只有 `_v2` 与 `sqlite3_create_window_function` 才带析构回调。`createFunctionApi` 在此时用 `sqlite3Malloc(sizeof(FuncDestructor))` 分配一个 `FuncDestructor`（含 `nRef`、`xDestroy`、`pUserData` 三个成员），并把 `pArg->nRef` 初始化为 0；分配失败则调 `sqlite3OomFault(db)`、立即调用 `xDestroy(p)` 并跳到 `out`。
3. `createFunctionApi` 调 `sqlite3CreateFunc` 完成真正的登记。返回后若 `pArg` 存在且 `pArg->nRef==0`（说明该 `FuncDef` 没有接管这个析构器），`createFunctionApi` 会补调 `xDestroy(p)` 并 `sqlite3_free(pArg)`——这是「注册失败也要归还用户数据所有权」的兜底。
4. `sqlite3CreateFunc` 先做参数合法性检查：函数名非空、`xSFunc` 与 `xFinal` 不同为非空、`xFinal` 与 `xStep` 同生共死、`xValue` 与 `xInverse` 同生共死、`nArg` 落在 `-1` 到 `SQLITE_MAX_FUNCTION_ARG` 之间、函数名长度不超过 `sqlite3Strlen30(zFunctionName)` 返回的上限 255（条件写作 `255<sqlite3Strlen30(...)`）。任一条不满足即返回 `SQLITE_MISUSE_BKPT`。其中 `SQLITE_MAX_FUNCTION_ARG` 默认是 1000，定义在 `src/sqliteLimit.h`；`src/main.c` 另有编译期检查要求该宏在 0 到 32767 之间（因为它要装进 `i16`）。
5. `sqlite3CreateFunc` 处理标志位：先把 `SQLITE_DETERMINISTIC|SQLITE_DIRECTONLY|SQLITE_SUBTYPE|SQLITE_INNOCUOUS|SQLITE_RESULT_SUBTYPE|SQLITE_SELFORDER1` 摘出来存进 `extraFlags`，再用 `SQLITE_FUNC_ENCMASK|SQLITE_ANY` 掩掉剩余编码位。`extraFlags ^= SQLITE_FUNC_UNSAFE` 这一步是把 API 侧的 `SQLITE_INNOCUOUS` 位翻成内部的 `SQLITE_FUNC_UNSAFE` 位——两者共用同一位但语义相反，源码注释标为 `tag-20230109-1`。
6. 编码归一化：`SQLITE_UTF16` 被换成 `SQLITE_UTF16NATIVE`；`SQLITE_ANY` 会递归调用 `sqlite3CreateFunc` 两次，分别注册 `SQLITE_UTF8` 与 `SQLITE_UTF16LE` 版本，最后把当前编码置为 `SQLITE_UTF16BE`；其余未知编码落到默认分支 `enc = SQLITE_UTF8`。`SQLITE_OMIT_UTF16` 编译下则无条件 `enc = SQLITE_UTF8`。
7. 覆盖/删除既有函数的处理：先用 `sqlite3FindFunction(db, zFunctionName, nArg, (u8)enc, 0)` 查同名同参数的旧定义。若命中且旧定义的编码掩码与新编码一致、`p->nArg==nArg`，则：连接上有活动语句（`db->nVdbeActive` 非零）时返回 `SQLITE_BUSY`，错误消息为 `"unable to delete/modify user-function due to active statements"`；没有活动语句时调 `sqlite3ExpirePreparedStatements(db, 0)` 让已预编译语句全部失效。若根本没命中且 `xSFunc` 与 `xFinal` 都为 0（即删除一个不存在的函数），按源码注释指向的论坛帖 `https://sqlite.org/forum/forumpost/726219164b` 直接返回 `SQLITE_OK`，把删除不存在的函数视为 no-op。
8. 登记本体：`sqlite3FindFunction(db, zFunctionName, nArg, (u8)enc, 1)` 带 `createFlag=1` 再查一次，命中已有定义就复用它，找不到就新建一个 `FuncDef`（`sqlite3FindFunction` 内部用 `sqlite3DbMallocZero` 分配 `sizeof(*pBest)+nName+1`，把函数名转小写存进哈希表 `db->aFunc`）。取不到定义时 `sqlite3CreateFunc` 返回 `SQLITE_NOMEM_BKPT`。
9. 覆盖既有定义前先调 `functionDestroy(db, p)` 释放旧版本挂着的析构器：`functionDestroy` 把 `pDestructor->nRef` 减一，减到 0 时才调 `pDestructor->xDestroy(pDestructor->pUserData)` 并 `sqlite3DbFree(db, pDestructor)`。引用计数是必要的，因为 `SQLITE_ANY` 一次注册会产生多份定义共享同一个 `FuncDestructor`。
10. 最后写入新状态：若本次带析构器则 `pDestructor->nRef++`；`p->u.pDestructor = pDestructor`；`p->funcFlags` 只保留旧的编码掩码再并上新的 `extraFlags`；`p->xSFunc = xSFunc ? xSFunc : xStep`；`p->xFinalize = xFinal`；`p->xValue = xValue`；`p->xInverse = xInverse`；`p->pUserData = pUserData`；`p->nArg = (u16)nArg`。

**调用路径**

11. 语句编译期通过 `sqlite3FindFunction` 定位函数定义（`src/func.c` 中表达式求值时以 `sqlite3FindFunction(db, pExpr->u.zToken, nExpr, SQLITE_UTF8, 0)` 查找），得到 `FuncDef` 写进 VDBE 操作码的 `P4_FUNCDEF`。`sqlite3FindFunction` 先在应用自定义函数哈希表 `db->aFunc` 里找，没有命中再查内置函数表（`createFlag` 为真时直接跳过内置函数，因为内置 `FuncDef` 是只读的）；多个候选按 `matchQuality` 打分择最优。
12. 标量调用：`src/vdbe.c` 的 `OP_Function` 分支构造 `sqlite3_context`（`pOut` 指向结果寄存器，`argc` 为参数个数，`argv` 指向各参数 `Mem`），然后执行 `(*pCtx->pFunc->xSFunc)(pCtx, pCtx->argc, pCtx->argv)`。回调返回后检查 `pCtx->isError`：大于 0 时用 `sqlite3_value_text(pCtx->pOut)` 取错误文本交给 `sqlite3VdbeError`，并把 `rc` 置为该错误码，非零则 `goto abort_due_to_error`。
13. 聚合调用：`OP_AggStep` / `OP_AggInverse` 分支首次执行时分配 `sqlite3_context` 加一块累加器 `Mem`（`nAlloc = ROUND8P(SZ_CONTEXT(n))`，`pCtx->pOut` 指向紧跟其后、8 字节对齐的 `Mem`），并把操作码从 `OP_AggStep0` 改写成 `OP_AggStep1` 以便 `sqlite3_context` 只初始化一次；`p1!=0`（即 `OP_AggInverse`）时调 `(pCtx->pFunc->xInverse)(pCtx,pCtx->argc,pCtx->argv)`，否则调 `(pCtx->pFunc->xSFunc)(pCtx,pCtx->argc,pCtx->argv)`。每步调用前 `pMem->n++`。
14. 聚合收尾：`OP_AggFinal` 分支调 `sqlite3VdbeMemFinalize(pMem, pOp->p4.pFunc)`；窗口取值分支 `OP_AggValue` 调 `sqlite3VdbeMemAggValue(pMem, &aMem[pOp->p3], pOp->p4.pFunc)`。`sqlite3VdbeMemFinalize` 在栈上构造一个 `sqlite3_context ctx` 和一个临时 `Mem t`，令 `ctx.pOut = &t`、`ctx.pMem = pMem`、`ctx.pFunc = pFunc`，调 `pFunc->xFinalize(&ctx)`，然后把 `t` 整体 `memcpy` 回 `pMem`，并返回 `ctx.isError`。`sqlite3VdbeMemAggValue` 同构，区别是它调 `pFunc->xValue(&ctx)` 且输出写到调用方给的 `pOut`。这两处都位于 `SQLITE_OMIT_WINDOWFUNC` 之外时可用。
15. 累加器内存：`sqlite3_aggregate_context(p, nByte)` 在 `p->pMem->flags & MEM_Agg` 为 0 时调 `createAggContext` 分配——`nByte<=0` 时把 `Mem` 置空并让 `pMem->z = 0`，否则 `sqlite3VdbeMemClearAndResize(pMem, nByte)` 分配、打上 `MEM_Agg` 标志、把 `pMem->u.pDef = p->pFunc` 记下归属函数、`memset` 清零；后续调用直接返回 `(void*)p->pMem->z`。累加器 `Mem` 被释放时（`vdbeMemClearExternAndSetNull`）若带 `MEM_Agg` 标志，会先调 `sqlite3VdbeMemFinalize(p, p->u.pDef)`，保证 `xFinalize` 不会漏调。

**取值与写回**

16. 参数侧：`sqlite3_value_blob(pVal)` 在 `p->flags` 含 `MEM_Blob` 或 `MEM_Str` 时先调 `ExpandBlob(p)` 展开可能存在的零填充 blob（展开失败时置 `MEM_Null` 并返回 0），再打上 `MEM_Blob` 标志并返回 `p->n ? p->z : 0`；若参数既不是 blob 也不是字符串，则回退到 `sqlite3_value_text(pVal)`。也就是说取 blob 会触发一次可能的类型转换。
17. 结果侧：`sqlite3_result_int64(pCtx, iVal)` 走 `sqlite3VdbeMemSetInt64(pCtx->pOut, iVal)`。`sqlite3_result_text(pCtx, z, n, xDel)` 走 `setResultStrOrError(pCtx, z, n, SQLITE_UTF8, xDel)`——`enc==SQLITE_UTF8` 时调 `sqlite3VdbeMemSetText`，否则调 `sqlite3VdbeMemSetStr`；`SQLITE_UTF8_ZT` 分支额外打 `MEM_Term` 终止符标志。写入失败时按返回码分流：`SQLITE_TOOBIG` 调 `sqlite3_result_error_toobig(pCtx)`（置 `pCtx->isError = SQLITE_TOOBIG`，错误文本 `"string or blob too big"`），其余只能是 `SQLITE_NOMEM`，调 `sqlite3_result_error_nomem(pCtx)`。写入成功后再调 `sqlite3VdbeChangeEncoding(pOut, pCtx->enc)` 做编码转换，并用 `sqlite3VdbeMemTooBig(pOut)` 复查是否超限，超限同样报 toobig。长度上限受 `SQLITE_LIMIT_LENGTH` 约束，其编译期默认值 `SQLITE_MAX_LENGTH` 为 1000000000，定义在 `src/sqliteLimit.h`。
18. 错误上报：`sqlite3_result_error(pCtx, z, n)` 置 `pCtx->isError = SQLITE_ERROR` 并把错误文本以 `SQLITE_TRANSIENT` 方式拷入输出 `Mem`；`sqlite3_result_error_code(pCtx, errCode)` 置 `pCtx->isError = errCode ? errCode : -1`，并在输出 `Mem` 仍为 `MEM_Null` 时用 `sqlite3ErrStr(errCode)` 填入错误文本。VDBE 侧在回调返回后统一检查 `pCtx->isError` 并转成语句级错误。
19. 用户数据：`sqlite3_user_data(p)` 直接返回 `p->pFunc->pUserData`，即注册时传入的第五个参数；`sqlite3_context_db_handle(p)` 返回 `p->pOut->db`，即注册所在的连接。

## 关键取舍

- **源码事实：**`sqlite3CreateFunc` 在有活动语句时拒绝覆盖/删除函数并返回 `SQLITE_BUSY`，无活动语句时改为 `sqlite3ExpirePreparedStatements(db, 0)` 让所有预编译语句失效。**架构推论：**SQLite 不做「语句级函数表快照」，函数表是连接级共享状态，因此修改函数只能靠「拒绝并发修改」或「作废已编译语句」二选一；前者避免让正在跑的语句行为突变，后者让后续执行重新绑定新定义。
- **源码事实：**`SQLITE_ANY` 会为同一个函数注册 UTF8 / UTF16LE / UTF16BE 三份 `FuncDef`，`functionDestroy` 用 `FuncDestructor.nRef` 引用计数保证析构器只在最后一份额外释放时触发。**架构推论：**把「偏好编码」做进注册期而不是调用期再转换，可让多数调用零拷贝命中偏好编码；代价是同一逻辑函数在哈希表里占多份记录，注册路径必须成组失败回滚（`SQLITE_ANY` 分支中任一次 `sqlite3CreateFunc` 非 `SQLITE_OK` 即整体返回该错误码）。
- **源码事实：**`sqlite3_value_blob` 对非 blob/字符串参数回退到 `sqlite3_value_text`，并在 blob 情形先 `ExpandBlob`。**架构推论：**参数取值接口选择「惰性转换 + 统一返回字节」而非要求调用方先判类型，减少了应用侧样板代码，但代价是一次隐式类型转换和一次可能的零填充展开。
- **源码事实：**`sqlite3_result_text` 系列在写入失败时不静默截断，而是通过 `sqlite3_result_error_toobig` / `sqlite3_result_error_nomem` 把错误码写进 `pCtx->isError`，由 VDBE 在回调返回后统一中断语句。**架构推论：**结果写入与错误传播分成「回调内设标志、VDBE 外统一处理」两段，使回调实现不必自己了解 VDBE 的错误恢复机制。
- **源码事实：**`sqlite3_result_error_code` 在 `errCode` 为 0 时写入 `isError = -1`。**架构推论：**`-1` 表示「有错误但未指定具体错误码」，与 `sqlite3_result_error` 写入的具体 `SQLITE_ERROR` 区分开，使调用方能区分「通用错误」与「带码错误」。
- **源码事实：**`sqlite3_aggregate_count` 在源码注释中被明确标注为 deprecated，新聚合实现应自己把计数存进聚合上下文。**架构推论：**计数器的归属从框架下沉到应用，避免框架为所有聚合函数都维护一个计数，也避免应用在聚合上下文之外再找地方存计数。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/main.c` | `sqlite3_create_function` | 标量/聚合用户函数的公开注册入口，转交 `createFunctionApi`。 |
| `src/main.c` | `sqlite3CreateFunc` | 真正的登记实现：参数合法性校验、编码归一化与 `SQLITE_ANY` 展开、覆盖/删除的 `SQLITE_BUSY` 与语句失效处理、`FuncDef` 字段写入。 |
| `src/main.c` | `createFunctionApi` | 持连接互斥锁、分配并托管 `FuncDestructor`、失败时归还用户数据析构权。 |
| `src/main.c` | `functionDestroy` | 引用计数式释放旧函数定义挂着的析构器。 |
| `src/vdbeapi.c` | `sqlite3_result_text` | 把回调返回的文本写入输出 `Mem`，失败时按 toobig / nomem 分流报错。 |
| `src/vdbeapi.c` | `sqlite3_result_int64` | 把 64 位整数结果写入输出 `Mem`。 |
| `src/vdbeapi.c` | `sqlite3_value_blob` | 读取参数字节，必要时展开零填充 blob，否则回退到文本取值。 |
| `src/vdbeapi.c` | `sqlite3_user_data` | 从 `sqlite3_context` 取回注册时传入的用户数据指针。 |
| `src/vdbeapi.c` | `sqlite3_aggregate_context` | 分配并复用聚合累加器上下文，打 `MEM_Agg` 标志。 |
| `src/vdbeapi.c` | `createAggContext` | 首次分配累加器内存、清零、记下归属 `FuncDef`。 |
| `src/vdbeapi.c` | `setResultStrOrError` | 结果写入的公共实现：按编码分派、超限与 OOM 转成错误。 |
| `src/vdbeapi.c` | `sqlite3_result_error_code` | 按错误码置 `pCtx->isError` 并填入错误文本。 |
| `src/vdbe.c` | `OP_Function` | 标量函数调用点：构造 `sqlite3_context` 并调 `xSFunc`。 |
| `src/vdbe.c` | `OP_AggStep` / `OP_AggInverse` | 聚合逐步累加与窗口反向撤销的调用点。 |
| `src/vdbe.c` | `OP_AggFinal` / `OP_AggValue` | 聚合收尾与窗口取值的调用点。 |
| `src/vdbemem.c` | `sqlite3VdbeMemFinalize` | 构造临时上下文调用 `xFinalize` 并把结果写回累加器 `Mem`。 |
| `src/vdbemem.c` | `sqlite3VdbeMemAggValue` | 构造临时上下文调用 `xValue` 并把结果写入指定 `Mem`。 |
| `src/callback.c` | `sqlite3FindFunction` | 按名字、参数个数与编码查找或新建 `FuncDef`，并按匹配质量择优。 |
| `src/callback.c` | `matchQuality` | 给候选定义打分：参数个数精确匹配得 4 分、可接受任意个数得 1 分，编码精确匹配再加 2 分、同为 UTF16 但字节序不同再加 1 分，满分 6 分为 `FUNC_PERFECT_MATCH`。 |
| `src/sqliteLimit.h` | `SQLITE_MAX_FUNCTION_ARG` | 用户函数最大参数个数上限，编译期默认 1000。 |
| `src/sqliteInt.h` | `SQLITE_FUNC_ENCMASK` / `SQLITE_FUNC_UNSAFE` | 内部函数标志位定义，`SQLITE_FUNC_UNSAFE` 与 API 侧 `SQLITE_INNOCUOUS` 同位反义。 |

## 与主核 / AI 的关系

暂无直接对应。主核若要提供扩展点，可借鉴两点：一是「连接级函数表 + 语句编译期绑定」的分工——注册期只落 `FuncDef`，执行期才绑回调，避免函数表变动影响在跑的语句；二是「回调内设错误标志、执行器外统一转异常」的两段式错误传播。累加器约定（首次分配、后续复用、释放时必调 finalize）可直接照搬到主核的聚合算子设计上。AI 侧若把自然语言意图编译成自定义函数，函数名长度上限 255 字节与参数个数上限 1000 这两个常量是生成 SQL 时必须遵守的硬边界。

## 参考

- [SQLite 官方文档：Create Or Redefine SQL Functions](https://sqlite.org/c3ref/create_function.html) —— 说明 `sqlite3_create_function` 系列 API 的参数含义、编码偏好、析构回调与覆盖规则。
- [SQLite 官方文档：Function Flags](https://sqlite.org/c3ref/c_deterministic.html) —— 说明 `SQLITE_DETERMINISTIC`、`SQLITE_DIRECTONLY` 等标志位语义。
- [GitHub 官方镜像源码：src/main.c](https://github.com/sqlite/sqlite/blob/master/src/main.c) —— 对照 `sqlite3_create_function` 与 `sqlite3CreateFunc` 的注册实现。
- [GitHub 官方镜像源码：src/vdbeapi.c](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c) —— 对照取值与写回接口的实现。
