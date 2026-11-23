---
project: postgres
feature: error-reporting
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/error/elog.c
    symbols: [errstart, errfinish, EmitErrorReport, in_error_recursion_trouble]
---

# 错误上报

## 一句话职责

错误上报把一次 `ereport`/`elog` 调用收集到的分级消息组织成一条错误记录，决定它是否落服务器日志、是否发给客户端，并按级别执行相应控制流（返回、长跳到事务错误处理、退出进程或 abort）。

## 机制是什么

`ereport(elevel, ...)` 展开成 `errstart` → 若干字段设置函数（`errmsg`/`errdetail`/`errhint`/`errcode` 等）→ `errfinish` 三步，字段在**错误栈帧**里累积而不是逐次写日志。栈是 `errordata[ERRORDATA_STACK_SIZE]`（源码定义为 5 项），`errordata_stack_depth` 指向当前帧，`recursion_depth` 独立记录“错误处理过程中又出错误”的真实递归层数。

级别是一组数值常量：`DEBUG5`=10 到 `DEBUG1`=14、`LOG`=15、`LOG_SERVER_ONLY`=16、`INFO`=17、`NOTICE`=18、`WARNING`=19、`WARNING_CLIENT_ONLY`=20、`ERROR`=21、`FATAL`=22、`FATAL_CLIENT_ONLY`=23、`PANIC`=24。数值顺序不等于“严重程度单调”：源码专门提供 `is_log_level_output` 处理 `LOG` 夹在 `ERROR` 与 `FATAL` 之间这种乱序关系，并让 `*_CLIENT_ONLY` 级别永不进日志。

本页只讲错误对象的分级、收集、递归保护与两个上报出口；“日志写到哪个目的地、`log_line_prefix` 如何拼装、syslog/csvlog/jsonlog 各格式细节”属于日志目的地实现，“事务中止后如何恢复”（`AbortCurrentTransaction`、`PG_exception_stack` 的设立）属于会话错误恢复，两者本页只在必要处点名，不展开。

## 怎么实现的

1. `errstart(elevel, domain)` 先做**升级判定**（仅 `elevel >= ERROR` 时）：`CritSectionCount > 0` 时一律升为 `PANIC`；`ERROR` 在“`PG_exception_stack == NULL`（没有处理器，如 postmaster 或后端启动早期）”、“`ExitOnAnyError`（initdb 用）”、“`proc_exit_inprogress`”三种情形下升为 `FATAL`。随后遍历栈上已有帧取级别最大值，防止正在上报 `PANIC` 时被一个低级别错误覆盖成 `ERROR`。
2. 然后按两个独立策略判是否“值得处理”：`should_output_to_server` 用 `is_log_level_output(elevel, log_min_messages[MyBackendType])`；`should_output_to_client` 要求 `whereToSendOutput == DestRemote` 且级别不是 `LOG_SERVER_ONLY`，认证尚未完成时（`ClientAuthInProgress`）只有 `>= ERROR` 才发给客户端。两者都为假且级别低于 `ERROR` 时直接返回 false，调用方据此短路整个上报（`message_level_is_interesting` 是给调用方预算前置开销用的同一判据）。
3. 真正开始前若 `ErrorContext` 还是 NULL，只能用 `write_stderr` 写一句“error occurred before error message processing is available”并 `exit(2)`；这说明该路径假定内存上下文早已初始化。
4. 递归保护在这一步做：`recursion_depth++ > 0 && elevel >= ERROR` 表示在错误处理中又报错，此时 `MemoryContextReset(ErrorContext)`；若 `in_error_recursion_trouble()`（即 `recursion_depth > 2`）为真，还会清掉 `error_context_stack` 与 `debug_query_string`——源码解释：无限递归可能正是上下文回溯例程本身坏了，长错误语句也可能是递归源。
5. 取一个栈帧 `get_error_stack_entry()`，写入 `elevel`、`output_to_server`、`output_to_client`、domain，并按级别给默认 `sqlerrcode`：`>= ERROR` 用 `ERRCODE_INTERNAL_ERROR`，`>= WARNING` 用 `ERRCODE_WARNING`，其余用 `ERRCODE_SUCCESSFUL_COMPLETION`；`assoc_context` 设为 `ErrorContext`，随后 `recursion_depth--` 并返回 true。
6. `errfinish(filename, lineno, funcname)` 继续 `recursion_depth++` 并 `CHECK_STACK_DEPTH()`（若栈已空则报“errstart was not called”），记录位置信息，切到 `ErrorContext`。若 `backtrace_functions` 匹配本函数名且尚未收集，则 `set_backtrace(edata, 2)`；随后倒序调用 `error_context_stack` 上的所有 `callback`（每条错误记录里的 `CONTEXT:` 行由此产生，源码注明这些回调中报错会被当作递归错误处理）。收齐后置 `errordata_complete[depth] = true`。
7. `elevel == ERROR` 时**不打印**：先做最小清理再长跳。清理包括把 `InterruptHoldoffCount`、`QueryCancelHoldoffCount`、`CritSectionCount` 归零（注释强调“假设处理器自己不在 holdoff 段里”），并故意让 `CurrentMemoryContext` 停在 `ErrorContext`，然后 `PG_RE_THROW()`。打印与弹栈由外层处理器负责（`PostgresMain` 的 `PG_CATCH` 调 `EmitErrorReport`）。
8. 非 `ERROR` 级别才真正上报：调 `EmitErrorReport()`。之后按嵌套层级决定清理方式——若 `errordata_stack_depth == 0 && recursion_depth == 1`（最外层）则 `MemoryContextReset(ErrorContext)` 一次性回收，否则只 `FreeErrorDataContents(edata)` 释放当前帧的子数据；再 `errordata_stack_depth--`、恢复原 `MemoryContext`、`recursion_depth--`。
9. `FATAL`/`FATAL_CLIENT_ONLY` 走 `proc_exit(1)`：若 `PG_exception_stack == NULL` 且仍在向远端输出，先把 `whereToSendOutput` 设为 `DestNone`（启动失败时客户端会断开，别再发）；`fflush(NULL)` 只为“万一 `proc_exit` 也崩了还能看到消息”；若 `pgStatSessionEndCause == DISCONNECT_NORMAL` 则标记为 `DISCONNECT_FATAL`。
10. `elevel >= PANIC` 走 `fflush(NULL)` + `abort()`：源码注释说明 postmaster 会观察到 SIGABRT 退出状态并连带杀掉其它后端，同时承认“如果就在 postmaster 里，`abort()` 杀不掉子进程”是已知缺陷。非致命级别最后统一 `CHECK_FOR_INTERRUPTS()`，让用户可以中断一个疯狂刷 NOTICE/WARNING 的循环。
11. `EmitErrorReport()` 是真正的两个出口：切到 `edata->assoc_context`，先重置格式化时间戳缓存（`saved_timeval_set = false`、`formatted_log_time[0] = '\0'`），再在 `output_to_server` 为真且注册了 `emit_log_hook` 时调用 hook —— hook **只允许关掉** `output_to_server`，不允许打开，因为 `errstart` 阶段判为“不值得处理”的消息根本走不到这里；之后按标记分别 `send_message_to_server_log(edata)` 与 `send_message_to_frontend(edata)`。它自己对 `recursion_depth` 增减，使 hook 或目的地内部的报错也进入递归保护。
12. `FlushErrorState()` 是错误处理器退出错误子系统的收口：把 `errordata_stack_depth` 归 -1、`recursion_depth` 归 0、`MemoryContextReset(ErrorContext)`。源码注释强调“在调用它之前你还没离开错误子系统”，因此 `CopyErrorData()` 必须把数据拷到调用者上下文（`assoc_context = CurrentMemoryContext`），否则 reset 后就失效；`CopyErrorData` 还会把 `filename`/`funcname` 这类看似常量的字符串也 `pstrdup`，因为可能指向 JIT 生成的代码段。

## 关键取舍

- 源码事实：`ERROR` 级别在 `errfinish` 里**不调用 `EmitErrorReport`**，只做计数器复位后 `PG_RE_THROW()`；打印由外层 `PG_CATCH` 负责（`PostgresMain` 的恢复块即如此）。架构推论：把“上报”与“控制转移”分开，让处理器在事务仍处于半完成状态时先做必要清理、再在相对安全的位置输出；若在 `errfinish` 里直接打印，处理器必须先输出再清理，输出过程中的再次报错会叠加在未清理状态上。代价是所有 `PG_TRY` 使用方都必须自己调 `EmitErrorReport`，漏掉就静默吞掉错误。
- 源码事实：`in_error_recursion_trouble()` 的判据是 `recursion_depth > 2`，触发时不仅重置 `ErrorContext`，还丢弃 `error_context_stack` 与 `debug_query_string`；同样地 `err_gettext` 在递归麻烦时跳过 `gettext`。架构推论：递归往往由例程本身引起，因此“少做点事”比“再努力一次”更安全，宁可丢掉 context 行和翻译文本也要先打破递归；代价是在真正需要 context 的深层错误场景下，输出会退化为不完整信息。
- 源码事实：级别数值中 `LOG`=15 落在 `ERROR`=21 之前，所以日志侧判定用 `is_log_level_output` 而非简单 `>=`，并让 `LOG_SERVER_ONLY`/`WARNING_CLIENT_ONLY`/`FATAL_CLIENT_ONLY` 各自绕过正常路径。架构推论：同一套 `elevel` 要同时表达“服务器日志的严重度”和“客户端的可见性”，两者排序关系不同，只能靠额外层级做映射；代价是判定逻辑分散在 `is_log_level_output` 与 `should_output_to_client` 两处，且必须与 `errstart` 的短路判断保持一致（`message_level_is_interesting` 的注释明确要求如此）。
- 源码事实：`errstart` 里 `ERROR` 会在 `PG_exception_stack == NULL`、`ExitOnAnyError`、`proc_exit_inprogress` 三种情形下被升为 `FATAL`，并遍历栈取最大级别。架构推论：没有处理器时抛 `ERROR` 只会 unwind 到未定义位置，因此必须升级为可控的进程退出；同时防止“正在上报 FATAL/PANIC 时被内层低级别错误降级”。代价是同一份 `ereport(ERROR, ...)` 代码在不同上下文里可能变成进程终止，调用方不能假设 `ERROR` 一定能被捕获。
- 源码事实：`EmitErrorReport` 允许 `emit_log_hook` 只关不开 `output_to_server`，并在发送前重置格式化时间缓存。架构推论：hook 定位为“过滤/转发”，不是“制造新日志”；若允许打开，则等于绕开 `errstart` 的级别短路，导致配置为不记录的消息也被记录。代价是扩展无法通过该 hook 实现“额外记录某些消息”。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/error/elog.c` | `errstart` | 一次上报的起点：级别升级（临界段→PANIC、无处理器 ERROR→FATAL）、按两个输出策略判定是否值得处理、递归时重置 ErrorContext、初始化栈帧的级别/输出标记/默认 errcode。 |
| `src/backend/utils/error/elog.c` | `errfinish` | 一次上报的终点：收集位置与 backtrace、跑 `error_context_stack` 回调、`ERROR` 直接长跳、其它级别调 `EmitErrorReport` 并按 FATAL/PANIC 执行退出或 abort，最后弹栈并检查中断。 |
| `src/backend/utils/error/elog.c` | `EmitErrorReport` | 两个实际出口：先给 `emit_log_hook` 机会（只能关日志输出），再按标记分别送服务器日志与客户端；自身也计入 `recursion_depth`。 |
| `src/backend/utils/error/elog.c` | `in_error_recursion_trouble` | 递归保护的统一判据（`recursion_depth > 2`），供 `errstart` 放弃错误上下文与调试语句、供 `err_gettext` 跳过本地化。 |

## 与主核 / AI 的关系

主核的错误上报没有 PostgreSQL `errstart`/`errfinish` 宏协议、`ERROR`/`FATAL`/`PANIC` 语义分级与 `PG_exception_stack` 长跳机制的直接对应物。可借鉴的是把“错误收集（栈帧累积字段）”与“错误上报（服务器日志/客户端/控制流）”分层，并把递归保护做成单一判据函数；不要把 `ereport` 的 `elevel` 数值或 `PG_RE_THROW` 直接套用到主核现有错误码体系。

## 参考

- [PostgreSQL 文档：运行时错误报告](https://www.postgresql.org/docs/current/runtime-config-logging.html) —— 说明 `log_min_messages`、`client_min_messages` 等控制消息去向的配置。
- [PostgreSQL 文档：错误消息与严重级别](https://www.postgresql.org/docs/current/error-message-reporting.html) —— 说明 `ereport` 的级别分类与字段构成。
- [PostgreSQL 官方源码：elog 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/error/elog.c) —— 支撑级别升级、递归保护与上报出口的实现事实。
