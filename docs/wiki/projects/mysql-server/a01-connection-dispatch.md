---
project: mysql-server
feature: connection-dispatch
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_parse.cc
    symbols: [do_command, dispatch_command]
---

# 连接与命令分派

## 一句话职责

把客户端发来的一帧协议包读成一条 `COM_XXX` 命令，按命令类型分派到对应的处理分支，并在命令结束后统一做状态收尾（慢日志、审计、PSI 语句结束、内存根回收、运行线程计数递减）。

## 机制是什么

MySQL 的一条连接上，服务端不区分「查询」和「其它命令」——它们都是同一层协议包。分派机制就是这条链：**读包 → 取出命令字节 → 分派 → 收尾**。

命令字节是包体的第一个字节。`Protocol_classic::get_command()` 读完之后做 `*cmd = (enum enum_server_command)(uchar)input_raw_packet[0];`，随即 `if (*cmd >= COM_END) *cmd = COM_END;`——越界的命令字节被夹到 `COM_END`，不会变成随机值。若包体长度为 0，则先填 `input_raw_packet[0] = (uchar)COM_SLEEP` 当作安全兜底。

分派本体是一个 `switch (command)`，覆盖 `COM_QUERY`、`COM_INIT_DB`、`COM_CHANGE_USER`、`COM_STMT_*`、`COM_QUIT`、`COM_PING`、`COM_STATISTICS` 等命令。其中只有 `COM_QUERY` 会进入 SQL 层：`dispatch_command()` 调 `dispatch_sql_command()`，后者解析并调 `mysql_execute_command()`。其余命令各有一条自己的处理路径——`COM_INIT_DB` 走 `mysql_change_db()`，`COM_CHANGE_USER` 走 `acl_authenticate()`，`COM_STMT_EXECUTE` 走 `mysqld_stmt_execute()`，`COM_QUIT` 走 `thd->get_stmt_da()->disable_status()` 并置 `error = true` 表示结束连接。

**收尾是分派的一部分，不是它的附属。** `dispatch_command()` 的 `done:` 标签之后固定做一批动作：把命令名与错误码送进审计事件、记慢日志、把 `thd->lex->sql_command` 复位为 `SQLCOM_END`、`thd->set_command(COM_SLEEP)`、`MYSQL_END_STATEMENT()` 关闭 Performance Schema 语句、`thd_manager->dec_thread_running()` 递减运行中线程计数，最后按内存根占用决定 `ClearForReuse()` 还是 `Clear()`。

**边界（与相邻机制的分界）：**

- 不负责 SQL 解析——`dispatch_sql_command()` 之后的 `parse_sql()` 与 `mysql_execute_command()` 属于 B 组与 C 组。
- 不负责优化器与执行计划。
- 不负责线程模型——连接处理器如何分配线程在 `sql/conn_handler/` 下。
- 不负责 THD 的建立与销毁（那是 `thd-lifecycle` 页）。

## 怎么实现的

### 读包：`do_command()`

`do_command()` 的注释写明它「由线程函数在循环中调用」，且「为了 profiling 能工作，绝不能被递归调用」。函数体顺序如下：

1. `assert(thd->is_classic_protocol());`——这条断言把整个函数钉死在经典文本协议上。
2. `thd->lex->set_current_query_block(nullptr);` 清掉「LEX 未初始化」的指示器，让后续错误处理走正常流程。
3. `thd->clear_error();` 与 `thd->get_stmt_da()->reset_diagnostics_area();` 清掉上一条命令遗留的错误。
4. `net = thd->get_protocol_classic()->get_net();` 取网络对象，`my_net_set_read_timeout(net, thd->variables.net_wait_timeout);` 把读超时设成 `net_wait_timeout`——空闲时用长超时，一旦开始读命令就换成短超时。
5. `net_new_transaction(net);` 开始一次网络事务。
6. `DEBUG_SYNC(thd, "before_do_command_net_read");`——KILL_CONNECTION 的测试同步点，源码注释说明它用于复现 Bug#37780。
7. 若 `opt_log_slow_extra` 打开，则 `thd->copy_status_var(&query_start_status);` 拷一份计数快照，供 `log_slow_statement` 算差值。
8. `rc = thd->m_mem_cnt.reset();` 重置内存计数；成功则进入读包：`thd->m_server_idle = true;` → `rc = thd->get_protocol()->get_command(&com_data, &command);` → `thd->m_server_idle = false;`。

**读包失败的分支是两级**：`rc` 非 0 时先把该语句按 `com_statement_info[COM_END].m_key` 精化标记为 `statement/com/error`，再 `thd->send_statement_status()` 并 `MYSQL_END_STATEMENT()` 关掉语句。随后按 `rc < 0` 区分：`rc < 0` 表示必须关闭连接，`return_value = true` 并跳到 `out`；否则只是包本身有问题，`net->error = NET_ERROR_UNSET;` 后 `return_value = false` 继续用这条连接。

读包成功后做两件收尾小事：输出包按 `thd->variables.net_buffer_length` 收缩（`thd->get_protocol_classic()->get_output_packet()->shrink(...)`），读超时从 `net_wait_timeout` 换回 `net_read_timeout`。

### 分派：`dispatch_command()`

进入后按顺序做「命令前置检查」，这些检查在任何具体命令分支之前：

1. `thd->m_statement_psi = MYSQL_REFINE_STATEMENT(thd->m_statement_psi, com_statement_info[command].m_key);`——把 PSI 语句标记从通用命令细化为本命令类型。
2. `thd->set_command(command);` 记录当前命令。
3. `thd->enable_slow_log = true;`；`thd->lex->sql_command = SQLCOM_END;`——注释说明这是「避免 VIEW 检测器被误导」。
4. `if (thd->killed == THD::KILL_QUERY) thd->killed = THD::NOT_KILLED;`——只清 `KILL_QUERY`，不清 `KILL_CONNECTION`（后者必须终止连接）。源码注释说明：语句最大执行时间用的定时器在 `mysql_execute_command()` 的清理阶段已解除，所以这里不可能是 `KILL_TIMEOUT`。
5. `thd->set_time();` 记录命令开始时间。紧接着有一处时间有效性检查：`is_time_t_valid_for_timestamp(thd->query_start_in_secs())` 为假时，用 `const int max_tries = 5;` 的循环重试取时间；5 次都失败则记 `ER_UNSUPPORTED_DATE`，给当前安全上下文补上 `SHUTDOWN_ACL`，并调 `kill_mysql()` 关服。
6. `thd->set_query_id(next_query_id());` 分配查询 ID；`thd->reset_rewritten_query();`；`thd_manager->inc_thread_running();`。
7. 计数：`if (!(server_command_flags[command] & CF_SKIP_QUESTIONS)) thd->status_var.questions++;`。哪些命令不计入 `Questions` 由 `init_sql_command_flags()` 里的 `server_command_flags` 决定——`COM_STATISTICS` 与 `COM_PING` 是 `CF_SKIP_QUESTIONS`；`COM_STMT_PREPARE` 与 `COM_STMT_CLOSE` 是 `CF_SKIP_QUESTIONS | CF_ALLOW_PROTOCOL_PLUGIN`。其余被显式列出的命令带 `CF_ALLOW_PROTOCOL_PLUGIN`。
8. `thd->server_status &= ~SERVER_STATUS_CLEAR_SET;` 清掉每条命令开头必须清的状态位。
9. 可插拔协议校验：`if (thd->get_protocol()->type() == Protocol::PROTOCOL_PLUGIN && !(server_command_flags[command] & CF_ALLOW_PROTOCOL_PLUGIN))` 则报 `ER_PLUGGABLE_PROTOCOL_COMMAND_NOT_SUPPORTED`，置 `thd->killed = THD::KILL_CONNECTION`，`error = true` 并跳到 `done`。
10. 口令过期校验：`thd->security_context()->password_expired()` 为真且命令不在白名单里时报 `ER_MUST_CHANGE_PASSWORD`。白名单由源码里的比较式给出：`COM_QUERY`、`COM_STMT_CLOSE`、`COM_STMT_SEND_LONG_DATA`、`COM_PING`、`COM_QUIT`、`COM_STMT_PREPARE`、`COM_STMT_EXECUTE`。
11. `mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_COMMAND_START), command, Command_names::str_global(command).c_str())`——审计插件若返回非零，直接 `goto done` 放弃执行。

### 各命令分支

**`COM_QUERY` 是最长的一条。** 分支内先重置摘要状态（`thd->m_digest = &thd->m_digest_state;` 加 `reset(thd->m_token_array, max_digest_length)`），再 `alloc_query(thd, com_data->com_query.query, com_data->com_query.length)` 把查询文本拷进 THD 内存。`alloc_query()` 会掐掉首部空白与尾部的 `;` 和空白，然后 `thd->set_query(query, packet_length)`。

接着建 `Parser_state parser_state;` 并 `parser_state.init(thd, thd->query().str, thd->query().length)`。摘要开关由 `get_max_digest_length() != 0` 决定（为 0 表示显式关闭摘要计算）。副引擎策略先设为 `Secondary_engine_optimization::PRIMARY_TENTATIVELY`，然后 `copy_bind_parameter_values()` 深拷贝命名参数（包字节会在处理过程中失效），最后 `dispatch_sql_command(thd, &parser_state)`。

**多语句在同一个 `COM_QUERY` 包里被逐条执行。** 循环条件是 `while (!thd->killed && (parser_state.m_lip.found_semicolon != nullptr) && !thd->is_error())`。每轮先把已执行的语句收尾（`thd->update_slow_query_status()`、`thd->send_statement_status()`、`log_slow_statement(thd)`、`MYSQL_END_STATEMENT()`、重建 `thd->m_statement_psi`、重设 `thd->set_query_id(next_query_id())`、`thd->set_time()`），再 `parser_state.reset(beginning_of_next_stmt, length)` 并再次 `dispatch_sql_command()`。每条子语句都独立 `thd->status_var.questions++;`。循环前还会把 `thd->server_status |= SERVER_MORE_RESULTS_EXISTS`（在 `dispatch_sql_command()` 内）。

**副引擎回退由 `check_secondary_engine_statement()` 承担。** 它只在「非致命错误」时考虑重启：`if (!thd->is_error() || thd->is_killed() || thd->is_fatal_error()) return;`。策略状态机是三态：`PRIMARY_TENTATIVELY` 时若错误码是 `ER_PREPARE_FOR_SECONDARY_ENGINE` 就切成 `SECONDARY` 并 `mysql_thread_set_secondary_engine(use_secondary_engine)`；`SECONDARY` 时若语句正在用副引擎就切成 `PRIMARY_ONLY`；`default` 直接返回。重启时它会结束旧 PSI 语句、重置摘要、重置解析器状态（`parser_state->reset(query_string, query_length)`）、临时把 `thd->variables.option_bits |= OPTION_LOG_OFF`（避免通用日志记两遍），然后再次调 `dispatch_sql_command(thd, parser_state)`，并递归调用自己以支持再次回退。触发点是 `COM_QUERY` 分支里的 `check_secondary_engine_statement(thd, &parser_state, orig_query.str, orig_query.length);`。

**`COM_CHANGE_USER` 的失败路径是硬失败。** 分支先 `thd->cleanup_connection()`，保存 `USER_CONN`、当前库与安全上下文副本，再调 `acl_authenticate(thd, COM_CHANGE_USER)`。失败时恢复安全上下文、用户连接与库，报 `ER_ACCESS_DENIED_CHANGE_USER_ERROR`，置 `thd->killed = THD::KILL_CONNECTION` 并 `error = true`。成功时才 `decrease_user_connections(save_user_connect)` 并 `my_free()` 掉旧库名。

**`COM_STMT_*` 家族统一先做 `mysql_stmt_precheck()`。** `COM_STMT_EXECUTE`、`COM_STMT_FETCH`、`COM_STMT_SEND_LONG_DATA`、`COM_STMT_PREPARE`、`COM_STMT_CLOSE`、`COM_STMT_RESET` 都是 `if (!mysql_stmt_precheck(thd, com_data, command, &stmt))` 才继续调 `mysqld_stmt_execute()` / `mysqld_stmt_fetch()` / `mysql_stmt_get_longdata()` / `mysqld_stmt_prepare()` / `mysqld_stmt_close()` / `mysqld_stmt_reset()`。`COM_STMT_SEND_LONG_DATA` 与 `COM_STMT_CLOSE` 先调 `thd->get_stmt_da()->disable_status()`。

**`COM_QUIT` 是结束连接的信号。** 分支把 `thd->set_sent_row_count(0)`（避免客户端看到「n>0 rows sent」），清掉 `net->error`（不发送 abort 消息），`thd->get_stmt_da()->disable_status()`，然后 `error = true`——这个返回值最终被连接循环读作「关掉这条连接」。

**`COM_STATISTICS` 自带固定缓冲。** 分支声明 `char buff[250];`，用 `snprintf(buff, buff_len - 1, ...)` 填 `Uptime`、`Threads`、`Questions`、`Slow queries`、`Opens`、`Flush tables`、`Open tables` 与 `Queries per second avg`。其中 `Open tables` 取自 `table_cache_manager.cached_tables()`，`Queries per second avg` 由 `thd->query_id * 1000LL / uptime` 算出（`uptime` 为 0 时该值置 0）。

**未知命令统一报 `ER_UNKNOWN_COM_ERROR`。** `switch` 的末尾把 `COM_SLEEP`、`COM_CONNECT`、`COM_TIME`、`COM_DELAYED_INSERT`、`COM_END` 与 `default` 合并为一个分支。

### 收尾：`done:` 之后

`done:` 标签之后是所有命令共享的收尾序列：

- `assert(thd->open_tables == nullptr || (thd->locked_tables_mode == LTM_LOCK_TABLES));`——表必须已关，除非处于 LOCK TABLES 模式。
- `thd->update_slow_query_status();`；`if (thd->killed) thd->send_kill_message();`；`thd->send_statement_status();`。
- `COM_CLONE` 的特例：`clone_cmd->execute_server(thd)` 在响应发出后执行。
- `COM_SUBSCRIBE_GROUP_REPLICATION_STREAM` 且无错时，调 `call_gr_incoming_connection_cb(thd, thd->active_vio->mysql_socket.fd, ...)`。
- `thd->rpl_thd_ctx.session_gtids_ctx().notify_after_response_packet(thd);`
- 审计三连：无错且未被杀时发 `MYSQL_AUDIT_GENERAL_RESULT`，再发 `MYSQL_AUDIT_GENERAL_STATUS`（带错误码），最后发 `MYSQL_AUDIT_COMMAND_END`（源码注释：这是纯信息性的，插件此时已无法中止执行）。
- `log_slow_statement(thd);`；`THD_STAGE_INFO(thd, stage_cleaning_up);`
- 状态复位：`thd->reset_query();`、`thd->set_command(COM_SLEEP);`、`thd->set_proc_info(nullptr);`、`thd->lex->sql_command = SQLCOM_END;`
- `MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());`，随后 `thd->m_statement_psi = nullptr;` 与 `thd->m_digest = nullptr;`
- `thd->reset_query_for_display();`、`thd->reset_rewritten_query();`——注释说明后者是为了防止重写后的查询「卡」在 SHOW PROCESSLIST 里。
- `thd_manager->dec_thread_running();`
- 内存根回收：`constexpr size_t kPreallocSz = 40960;`，`if (thd->mem_root->allocated_size() < kPreallocSz) thd->mem_root->ClearForReuse(); else thd->mem_root->Clear();`。源码注释解释了阈值来源：默认预分配大小 8192（注释里说明「实际上已不再预分配」），因子 5 相当随意，但能让当前分配策略下做三次分配（1 + 1.5 + 1.5²）。

## 关键取舍

**用「一个 `switch` 覆盖所有命令」而不是表驱动。** `dispatch_command()` 的 `switch` 有二十余个分支，命令与处理代码是硬绑定的。好处是每个分支能就地访问 `com_data` 里该命令特有的字段（如 `com_data->com_init_db.db_name`、`com_data->com_kill.id`、`com_data->com_set_option.opt_command`），不必做间接跳转；代价是新增命令必须改这个函数，`COM_STMT_*` 家族里六个几乎同构的分支（都是 `mysql_stmt_precheck()` 加一个 `mysqld_stmt_*` 调用）就是这份重复成本的体现。

**命令前置检查集中在一处，而不是散在分支里。** 可插拔协议校验、口令过期校验、审计 START 事件、`Questions` 计数、`SERVER_STATUS_CLEAR_SET` 清位都排在 `switch` 之前。这保证了「一条命令被处理之前必须经过的门槛」是同一条路径，插件也无法通过某个分支绕开审计。代价是这三项检查对 `COM_SLEEP` 这类不存在的命令同样要付出开销。

**口令过期采用白名单而不是黑名单。** 白名单里的 `COM_QUERY`、`COM_STMT_PREPARE`、`COM_STMT_EXECUTE` 之所以放行，源码注释说明它们「稍后做更细粒度的检查」——真正的粒度化检查在 `dispatch_sql_command()` 里：那里比对 `lex->sql_command`，且只放行 `SQLCOM_SET_PASSWORD` 与 `SQLCOM_ALTER_USER`。代价是同一语义在两处以不同粒度各写一遍。

**用 `error` 布尔量而非错误码表达「命令没成功」。** `dispatch_command()` 的返回值是一个布尔：`true` 表示请求关闭连接（`COM_QUIT`、`COM_CHANGE_USER` 认证失败、可插拔协议不支持），`false` 表示连接可以继续。错误码与诊断信息走 `thd->get_stmt_da()`，与分派返回值的语义是分开的。

**COM_QUERY 的多语句靠「找到分号再切一段」实现。** `parser_state.m_lip.found_semicolon` 给出下一条语句的起点，循环每次执行一段并重置解析器状态。这让客户端可以在一个包里塞多条语句，代价是每条子语句都要完整重走一遍 `dispatch_sql_command()`，且 `thd->set_query_id(next_query_id())` 与 `thd->set_time()` 要逐条重做。

**副引擎回退用「状态机 + 递归自检」。** `check_secondary_engine_statement()` 靠 `Secondary_engine_optimization` 的三态决定是否重启，重启后再次递归调用自己以处理「二次失败」。代价是重启路径要手工复原相当多状态（摘要、解析器状态、PSI 语句、`option_bits`），漏一项就会让观测数据与实际执行不一致。

**内存根用双阈值回收。** `kPreallocSz = 40960` 以下用 `ClearForReuse()` 保留最后一块内存供下次复用，以上用 `Clear()` 全清。这让「一条大查询吃掉的内存不会长期滞留」与「小查询不必每次向 OS 要内存」两件事同时成立。

**架构推论：** `do_command()` 的注释写明它「由线程函数在循环中调用」，而本机制页面不覆盖线程模型（连接处理器如何分配线程属于 `sql/conn_handler/`）。因此可以推出「读包—分派—收尾」这一轮循环发生在连接处理器线程里，而不是由调度器派发的独立任务；线程池模式下同一逻辑仍成立，但线程与连接的绑定关系由连接处理器决定，不在本页边界内。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_parse.cc` | `do_command` | 连接线程的读包与分派循环入口：取网络对象、设读超时、读命令包、区分「必须关连接」与「包有误但可继续」，成功后调 `dispatch_command` |
| `sql/sql_parse.cc` | `dispatch_command` | 命令前置检查（PSI 精化、协议类型、口令过期、审计 START）、`COM_XXX` 的 `switch` 分派、`done:` 收尾（审计三连、慢日志、状态复位、PSI 结束、内存根回收） |
| `sql/sql_parse.cc` | `init_sql_command_flags` | 初始化 `server_command_flags`：设定哪些命令不计入 `Questions`（`CF_SKIP_QUESTIONS`）、哪些命令允许在可插拔协议下执行（`CF_ALLOW_PROTOCOL_PLUGIN`） |
| `sql/sql_parse.cc` | `dispatch_sql_command` | `COM_QUERY` 的 SQL 层入口：重置 THD、`lex_start`、`parse_sql`、查询重写、`mysql_execute_command` |
| `sql/sql_parse.cc` | `check_secondary_engine_statement` | 副引擎回退：按 `Secondary_engine_optimization` 三态决定是否重启，重启时复原摘要、解析器状态与 PSI 语句 |
| `sql/sql_parse.cc` | `alloc_query` | 把查询文本拷入 THD 内存，掐掉首部空白与尾部 `;` 及空白 |
| `sql/sql_parse.cc` | `copy_bind_parameter_values` | 深拷贝命名参数的名称与字节值，避免包字节失效后悬垂 |
| `sql/sql_parse.cc` | `shutdown` | `COM_QUERY` 走 SQL 层时处理 SHUTDOWN：校验 `SHUTDOWN_ACL` 后调 `kill_mysql()` |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 分派没有「命令协议层」这一层：嵌入式 SPI 的调用方直接给出已解析的语句，不存在 COM 包、命令字节、连接级 `switch`。

**架构推论：** 主核若要支持远程协议接入，值得借鉴 MySQL 的两点。其一是**把「命令前置门槛」与「命令语义」分成两段**：`dispatch_command()` 的前置检查（协议类型、口令过期、审计 START）集中在一处，`switch` 分支只管语义。这让新增命令不必重复实现门槛，也让门槛无法被某个分支绕开。其二是**「命令结束」有统一的收尾序列**：`done:` 标签之后固定做审计事件、慢日志、PSI 语句结束、运行线程计数递减与内存根回收，并且用 `kPreallocSz = 40960` 这一个常量统一决定内存保留策略。主核的 SPI 层若要跑批处理，同样需要一个统一的「一次调用结束」收尾点，否则观测计数与内存回收会散落在各调用方。

对 AI 场景而言，分派层给出了「一次工具调用的边界」这一明确切面：一条 COM_QUERY 包在 `dispatch_command()` 里被切开成「解析 → 执行 → 收尾」，多语句场景下每条子语句各自走一遍完整边界并各自 `thd->set_query_id(next_query_id())`（因此每条子语句在 Performance Schema 里是独立记录）。AI 代理若要「一次调用发多条语句」，就必须意识到观测与审计是按子语句切分的，而不是按包切分。同时 `dispatch_command()` 里副引擎回退的 `Secondary_engine_optimization` 三态机说明：一条语句可能被服务端重跑，客户端侧的「语句只执行一次」假设在有副引擎时不成立。

## 参考

- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 源码：`sql/protocol_classic.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/protocol_classic.cc)
- [MySQL 源码：`sql/sql_prepare.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_prepare.cc)
- [MySQL Server 开发文档：`do_command`](https://dev.mysql.com/doc/dev/mysql-server/latest/do__command.html)
- [MySQL 参考手册：客户端与服务端通信协议（`COM_` 命令）](https://dev.mysql.com/doc/refman/8.0/en/client-server-protocol.html)
- [MySQL 参考手册：Performance Schema 语句事件表](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-events-statements.html)
