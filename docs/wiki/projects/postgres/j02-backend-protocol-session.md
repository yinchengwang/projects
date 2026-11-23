---
project: postgres
feature: backend-protocol-session
page_type: feature
schema_version: 1
refs:
  - path: src/backend/tcop/backend_startup.c
    symbols: [BackendMain, ProcessStartupPacket]
  - path: src/backend/tcop/postgres.c
    symbols: [PostgresMain, exec_simple_query]
---

# 后端协议与会话循环

## 一句话职责

一个后端进程在握手阶段读出客户端的启动包并填充会话身份，之后进入消息循环，按 frontend 消息类型把 SQL 文本或 prepared-statement 指令送进相应的执行路径，并在每个消息边界维护错误恢复与 ReadyForQuery 语义。

## 机制是什么

会话建立分为两段：`BackendMain` 是 postmaster fork 出的子进程入口，它先做**不依赖共享内存**的初始化并收取启动包；成功后才 `InitProcess()` 建立 `PGPROC`，再交给 `PostgresMain` 完成数据库/角色认证后的正式会话。因此 `ProcessStartupPacket` 运行时的目标是填出一个完整的 `Port`：协议版本（`port->proto`）、用户名、数据库名、`options` 中的命令行 GUC、其它 GUC 键值（含 `application_name`）以及是否请求复制连接。

握手还负责协议层的选择：客户端可以在正式启动包之前插入 SSL 请求或 GSSENC 请求，服务端以单字节应答支持/不支持，接受后重新读包；取消请求是另一类特殊包，它不建立会话，而是把目标 PID 与 cancel key 交给 `BackendInitialize` 后续派发。所有这些“特殊码 vs 真协议版本”的分流都发生在**同一个长度前缀包里**，非法长度或非法布局直接终止连接。

`PostgresMain` 是会话期主循环。它在一份 `MessageContext` 里逐条读消息首字节并分派：`Query` 走 simple 协议（整串 SQL 一次交给 `exec_simple_query`），`Parse`/`Bind`/`Execute` 走 extended 协议（分别建立 prepared statement、portal 与执行），`Describe`/`Close`/`Flush`/`Sync` 处理元数据与批次边界，`Terminate`/EOF 结束进程。本页只讲“握手 + 消息循环 + 每条 SQL 串的事务/错误边界”；解析、重写、计划与执行器的内部结构属于各自机制页，utility 命令内部的分派属于命令执行页。

## 怎么实现的

1. `BackendMain` 断言收到 `BackendStartupData` 且 `MyClientSocket` 有效；在 `EXEC_BACKEND` 平台上重新初始化 SSL（失败只记日志并继续，以免所有连接都不可用）。随后 `BackendInitialize` 完成网络侧准备：`ReserveExternalFD`、`pq_init` 建立 `Port`、把 `whereToSendOutput = DestRemote`（此后才能 ereport 给客户端）、做反向名字解析并记录 `remote_host`/`remote_port`、按 `log_connections` 记录 receipt。
2. `BackendInitialize` 在**触碰共享内存之前**安装 `process_startup_packet_die`（SIGTERM）与超时机制，并切换 `StartupBlockSig`；其设计前提是此时进程尚无外部状态需清理，所以超时/关停可以直接 `_exit`。收包后它检查 `canAcceptConnections` 给定的降级状态（dead-end 子进程只回错误就退出），再做认证与 `InitPostgres`。
3. `ProcessStartupPacket` 先单独读长度字的第一个字节，用以区分“完全没有数据”（静默关闭，常见于监控/端口扫描/SSL 拒绝后放弃）与“不完整长度字”（`COMMERROR`）；再做长度合理性检查（不小于协议版本字段、不超过 `MAX_STARTUP_PACKET_LENGTH`）。随后一次性读入整包并多分配一个字节置零，保证包内任意字符串都有 NUL 结尾。
4. 包首 4 字节被解释为协议版本或特殊请求码：`CANCEL_REQUEST_CODE` 交给 `ProcessCancelRequestPacket` 记录 PID 与 cancel key 后不在本连接继续；`NEGOTIATE_SSL_CODE`/`NEGOTIATE_GSS_CODE` 在未协商过该层时回单字节 `S`/`N`、`G`/`N`，接受则调用相应 secure 层并 `goto retry` 重新读包；每种协商只允许一次，若接受加密后发现缓冲区里仍有未加密数据，按协议违规 FATAL 处理（明确提示中间人风险）。
5. 真协议版本先 `FrontendProtocol = Min(proto, PG_PROTOCOL_LATEST)`（使后续 ereport 知道该用什么格式），再校验主版本号范围；然后按 v3 布局扫描 `name`/`value` 字符串对，把 `database`、`user`、`options`、`replication` 填入 `Port` 专用字段，`_pq_.` 前缀的协议选项只记录为“未识别”，其余一律当作 GUC 键值对成对压入 `port->guc_options`，并在遇到 `application_name` 时预先清洗保存。扫描必须正好在包尾的终止字节结束，否则 FATAL。
6. 包尾收尾：缺用户名直接 FATAL；缺数据库名则默认取用户名；两者都按 `NAMEDATALEN` 截断；若请求了 replication 但值不是 `database`，则置 `am_walsender` 并把 `MyBackendType` 改成 `B_WAL_SENDER`；非“连库的 walsender”会清空数据库名（流复制不需要库，逻辑复制需要）。
7. `PostgresMain` 按是否 walsender 选信号集（walsender 走 `WalSndSignals()`，普通后端装 SIGHUP/SIGINT/SIGTERM/SIGQUIT/SIGUSR1/SIGUSR2 等），`BaseInit()` 后解除阻塞；若输出目标是远端则生成随机 cancel key（协议版本 ≥ 3.2 用长 key），再由 `InitPostgres` 完成数据库与角色初始化。
8. 主循环用 `PG_exception_stack` 捕获 `ERROR`：进入错误块先 `disable_all_timeouts(false)` 并清 `QueryCancelPending`、复位读写状态与 `pq_comm_reset()`，然后 `EmitErrorReport()`、`AbortCurrentTransaction()`、`PortalErrorCleanup()`、释放 replication slot 与临时 slot、`FlushErrorState()`，再回到 `MessageContext`。若错误发生在 extended 消息处理中则 `ignore_till_sync = true`（跳过后续消息直到 `Sync`，且不提前发 ReadyForQuery）；若错误发生在读消息途中则 `pq_is_reading_msg()` 为真，直接 FATAL“protocol synchronization was lost”，因为已无法可靠定位下一条消息边界。
9. 每轮循环开头复位 `doing_extended_query_message`、清空 `MessageContext`、`InvalidateCatalogSnapshotConditionally()`；当 `send_ready_for_query` 为真时先把上一批输出 flush 掉，按事务状态置 `idle`/`idle in transaction`/`idle in transaction (aborted)` 并启动对应 idle 超时，空闲且不在事务块时处理 `notifyInterruptPending`（保证通知先于 ReadyForQuery 送达）、按 `pgstat_report_stat` 的返回值安排统计刷新超时，`ReportChangedGUCOptions()` 上报已变更 GUC，最后发 ReadyForQuery。
10. 随后禁用 `DoingCommandRead` 并 `CHECK_FOR_INTERRUPTS()`，处理 `ConfigReloadPending`，再按首字节分派。`Query` 消息提取字符串后：walsender 先试 `exec_replication_command`，未识别则退回 `exec_simple_query`；普通后端直接 `exec_simple_query`。`Parse`/`Bind`/`Execute`/`Describe`/`Close`/`FunctionCall` 在 walsender 上被 `forbidden_in_wal_sender` 拒绝；`Flush` 只在远端输出时 `pq_flush`；`Sync` 先 `EndImplicitTransactionBlock()` 再 `finish_xact_command()` 并置 `send_ready_for_query`；`CopyData`/`CopyDone`/`CopyFail` 按协议规范接受但忽略。
11. `exec_simple_query` 是 simple 协议的语句边界实现：`start_xact_command()` 开事务命令，`drop_unnamed_stmt()` 复用未命名 statement 语义，`pg_parse_query` 得到 raw parsetree 列表；多于一条时 `use_implicit_block` 使它们共享一个隐式事务块（历史行为：一条 Query 消息里的多条语句是一起提交的，除非里面出现事务控制语句）。
12. 逐条 parsetree 处理：先取 `commandTag`、`BeginCommand`；若事务已 abort 且该语句不是事务退出语句则直接报“commands ignored until end of transaction block”（必须在 parse analysis 前判断，否则访问数据库会失败）；再 `start_xact_command()`、必要时 `BeginImplicitTransactionBlock()`，若分析需要快照则 `PushActiveSnapshot(GetTransactionSnapshot())` 并分析/重写/计划，然后 `PopActiveSnapshot()`——源码明确不复用该快照执行，因为它是在给表加锁之前取的。
13. 每条语句建未命名 portal（`CreatePortal("", true, true)` 且 `visible = false`）、`PortalDefineQuery`、`PortalStart`、选定输出格式（text 默认，`FETCH` 到 binary 游标时用 binary）、建 `DestReceiver`，再 `PortalRun(... FETCH_ALL, true ...)` 并销毁 receiver 与 portal。最后一条 parsetree 之前先 `EndImplicitTransactionBlock()` 并 `finish_xact_command()`，使事务结束错误先于 command-complete 上报；中途遇事务控制语句也 `finish_xact_command()`；其余语句改用 `CommandCounterIncrement()` 并 `disable_statement_timeout()` 让超时按每条语句独立计时。
14. 每条 parsetree 发一次 `EndCommand`；空 parsetree 列表时发 `NullCommand`；按 `check_log_duration` 做时长日志。`PostgresMain` 在 `EOF`/`Terminate` 时把 `whereToSendOutput` 改成 `DestNone`（防止 ereport 再写客户端）后 `proc_exit(0)`，其余清理一律交给 `on_proc_exit`/`on_shmem_exit` 回调，源码注释明确禁止在该分支里堆叠逻辑，否则其它关停路径会漏调用。

## 关键取舍

- 源码事实：`BackendInitialize` 刻意在 `InitProcess()`（共享内存）之前完成收包，并只为该阶段安装 `_exit` 式 SIGTERM/超时处理。架构推论：让卡在启动包上的连接能被立即杀掉而无需回滚任何共享状态，代价是这段代码不能依赖共享内存、也不能复用常规后端清理路径；若先建 `PGPROC` 再收包，关停一个恶意/卡死握手就要走完整退出流程。
- 源码事实：`ProcessStartupPacket` 接受 SSL/GSS 协商后 `goto retry` 重读包，并显式检查协商后缓冲区残留数据；两种协商各只允许一次。架构推论：把“加密层协商”与“正式启动包”串在同一连接状态机上，可支持两种加密的任意尝试顺序，代价是重读逻辑与“接受后仍收到明文”必须作为协议违规处理，否则会掩盖中间人注入。
- 源码事实：extended 协议出错后置 `ignore_till_sync` 并抑制 ReadyForQuery，直到客户端发 `Sync`；若错误发生在读消息过程中则直接 FATAL。架构推论：批次语义要求客户端明确知道“这一批已被放弃”，跳过到 Sync 才能与客户端认知对齐；而读消息中途出错意味着消息边界已不可信，继续读可能把残片当新消息，因此只能断连。
- 源码事实：`exec_simple_query` 用 `use_implicit_block` 让一条 Query 消息里的多条语句共享隐式事务块，并在分析/计划后主动 `PopActiveSnapshot()`，不复用该快照执行。架构推论：前者维持历史语义（一条消息一个事务，除非显式事务控制），后者避免“先取快照后加锁”导致的可见性异常；两者都以放弃更细粒度的并发优化换取行为可预期。
- 源码事实：`Query` 消息在 walsender 上先走 `exec_replication_command`，失败才回落到 `exec_simple_query`；而 extended 消息在 walsender 上一律被拒。架构推论：复制连接复用同一消息循环可以少一套协议，但必须显式限制可用消息集合，否则客户端可能把普通 SQL 语义带进复制连接。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/tcop/backend_startup.c` | `BackendMain` | 后端子进程入口：先做不依赖共享内存的初始化与启动包收取，再建 `PGPROC` 并转交 `PostgresMain`。 |
| `src/backend/tcop/backend_startup.c` | `ProcessStartupPacket` | 解析长度前缀启动包，分流取消请求与 SSL/GSS 协商，并把协议版本、用户/库名、GUC 与复制标志填入 `Port`。 |
| `src/backend/tcop/postgres.c` | `PostgresMain` | 会话主循环：信号设置、`ERROR` 恢复块、ReadyForQuery/idle 状态维护，以及按 frontend 消息首字节分派。 |
| `src/backend/tcop/postgres.c` | `exec_simple_query` | simple 协议的语句边界实现：事务命令增删、隐式事务块、逐条 parsetree 走分析/计划/portal 执行并上报 command 完成。 |

## 与主核 / AI 的关系

主核的对外接入协议与会话循环不采用 PostgreSQL 的 startup packet + SSL/GSS 协商 + simple/extended 消息状态机。可借鉴的是把“握手阶段（无共享状态）”与“会话阶段”硬性分开、以及把未完成批次的错误恢复表现为显式跳过到同步点；不要把 `ReadyForQuery` 或 `ignore_till_sync` 当作主核已有的会话语义。

## 参考

- [PostgreSQL 文档：前端/后端协议](https://www.postgresql.org/docs/current/protocol.html) —— 说明启动包、消息格式与简单/扩展查询流程。
- [PostgreSQL 文档：协议流与消息类型](https://www.postgresql.org/docs/current/protocol-flow.html) —— 说明 simple query 与 extended query 的消息顺序及 Sync 边界。
- [PostgreSQL 官方源码：会话主循环](https://github.com/postgres/postgres/blob/master/src/backend/tcop/postgres.c) —— 支撑消息分派、错误恢复与事务命令边界的实现事实。
