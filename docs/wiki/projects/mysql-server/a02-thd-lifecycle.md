---
project: mysql-server
feature: thd-lifecycle
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_class.cc
    symbols: [THD::sql_parser]
  - path: sql/mysqld_thd_manager.cc
    symbols: [THD]
---

# THD 线程对象生命周期

## 一句话职责

THD 是 MySQL 里代表「一个正在服务的连接」的会话对象：它把会话状态（系统变量、诊断区、元数据锁上下文、协议栈）收在一处，把 SQL 层的事务上下文 `Transaction_ctx` 挂成自己的成员，并在「建立 → 上下文切换 → 销毁」这条链上被创建、切换和释放。

## 机制是什么

**THD 不是操作系统线程，而是线程之上的会话对象。** `sql/conn_handler/connection_handler_per_thread.cc` 的 `handle_connection()` 是一个 `for (;;)` 循环：每轮用 `init_new_thd()` 为新到的连接造一个 THD，`thd_manager->add_thd(thd)` 把它登记进全局表，然后在 `while (thd_connection_alive(thd))` 里反复 `do_command(thd)`，退出后依次 `close_connection(thd, 0, false, false)` → `thd->release_resources()` → `thd_manager->remove_thd(thd)` → `delete thd`，最后 `channel_info = Per_thread_connection_handler::block_until_new_connection()` 回到线程池等下一条连接（`pthread_reused = true`）。线程与 THD 是两个独立的生命周期：同一 pthread 会先后服务多个互不相同的 THD 对象。

**THD 上挂的东西分四类：**

1. 会话配置与状态：`variables`（会话系统变量）、`server_status`、`status_var`。
2. 解析与执行上下文：`main_lex`/`lex`、`stmt_map`（预处理语句表）、`Query_arena` 及其内存根 `main_mem_root`。
3. 锁与并发上下文：`mdl_context`、`LOCK_thd_data` 等互斥量、`ha_data`（引擎私有句柄容器）。
4. 事务挂载点：`m_transaction`。

**事务挂载点是本页的核心。** `Transaction_ctx` 是 SQL 层的事务协调对象，存储引擎自己的事务对象不在这条链上。THD 构造时就把它建好：`m_transaction(new Transaction_ctx())`，随后在构造函数体里置 `get_transaction()->m_flags.enabled = true`。挂载点只有一个，换事务上下文必须走 `THD::set_transaction()`，它先 `delete m_transaction.release()` 再 `m_transaction.reset(transaction_ctx)`——旧的 `Transaction_ctx` 被真删掉，不是回收复用。`THD::init()` 还会对挂载点调用 `reset_unsafe_rollback_flags(Transaction_ctx::SESSION)` 与 `reset_unsafe_rollback_flags(Transaction_ctx::STMT)`。

**销毁被切成两段。** `THD::cleanup()` 回收会话级资源（事务、元数据锁、表、锁），`THD::release_resources()` 回收线程级资源（vio、插件变量、外部存储、PSI 聚合）。这一刀让 `COM_CHANGE_USER` 可以只做前者再 `init()` 复位，从而复用同一个 THD 对象。

**全局可见性由 `Global_THD_manager` 提供。** 它把 THD 按 `thread_id % NUM_PARTITIONS` 分片存放，给 SHOW PROCESSLIST、KILL、关闭服务器这三类「从别的线程观察所有会话」的需求提供统一入口。

**生命周期是显式状态机。** `m_thd_life_cycle_stage` 的取值覆盖 `ACTIVE`、`ACTIVE_AND_CLEAN`、`CLEANED_UP`、`SCHEDULED_FOR_DISPOSAL`、`RESOURCES_RELEASED`、`DISPOSED`。这个字段参与实际判断：`Global_THD_manager::remove_thd()` 断言目标 THD 已 `release_resources_done()`；`Global_THD_manager::find_thd()` 命中之后若 `is_being_disposed()` 为真就返回空，避免把正在析构的会话对象交给别人。

## 怎么实现的

### 建立

`THD::THD(bool enable_plugins)` 是唯一的构造入口：初始化 LEX（`main_lex`/`lex`）、安全上下文（`m_main_security_ctx`/`m_security_ctx`）、两个协议对象（`protocol_text`/`protocol_binary`）、`m_transaction(new Transaction_ctx())` 等成员，初始化 `LOCK_thd_data`、`LOCK_thd_query`、`LOCK_thd_sysvar`、`LOCK_thd_protocol`、`LOCK_thd_security_ctx`、`LOCK_query_plan`、`LOCK_current_cond` 与 `COND_thr_lock`，随后 `reset_open_tables_state()`、`init()`，并以 `protocol_text` 作为默认协议。此刻 `m_thread_id` 仍是 `Global_THD_manager::reserved_thread_id`。

真实调用方有两条：`sql/conn_handler/connection_handler_per_thread.cc` 的 `init_new_thd()`（前台连接）与 `sql/sql_thd_internal_api.cc` 的 `create_thd()` / `create_internal_thd()`（内部线程）。前台路径 `init_new_thd()` 的顺序是 `channel_info->create_thd()` → `thd->set_new_thread_id()` → `thd_set_thread_stack(thd, (char *)&thd)` → `thd->store_globals()`；若 `create_thd()` 返回空指针，则 `channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false)` 后返回空，调用方 `handle_connection()` 据此计入 `connection_errors_internal` 并 `break` 退出服务循环。

内部线程有两条略有差别的入口。`create_thd(enable_plugins, background_thread, bound, psi_key, psi_seqnum)` 在 `background_thread` 为真时**先**置 `thd->system_thread = SYSTEM_THREAD_BACKGROUND` 并调 `thd->security_context()->skip_grants()`，**再**调 `thd_init()`；`create_internal_thd()` 则直接 `new THD(false)`（内部线程不启用插件），置 `SYSTEM_THREAD_BACKGROUND`、`skip_grants()`、`thread_stack` 后调 `store_globals()`。`thd_init(THD *thd, char *stack_start)` 只在 `thd->system_thread != SYSTEM_THREAD_BACKGROUND` 时才 `thd->set_new_thread_id()` 并 `thd_manager->add_thd(thd)`——这正是后台线程不进全局 THD 表的原因，源码注释同时点明「THD 需要 `thread_id != 0` 才能加入全局 THD 表」。

`THD::set_new_thread_id()` 从 `Global_THD_manager::get_new_thread_id()` 取号，把 `variables.pseudo_thread_id` 同步为同一个值，再调 `thr_lock_info_init(&lock_info, m_thread_id, &COND_thr_lock)`。`Global_THD_manager::get_new_thread_id()` 在 `LOCK_thread_ids` 保护下自增 `thread_id_counter`（初值 `reserved_thread_id + 1`），并用 `thread_ids.insert_unique(new_id)` 保证不与在用 ID 重复。

登记进全局表由 `Global_THD_manager::add_thd(THD *thd)` 完成：断言 `thd->thread_id() != reserved_thread_id`（保留 ID 不进表），按 `thd_partition(thd->thread_id())` 选分片，在 `LOCK_thd_list[partition]` 下 `thd_list[partition].insert_unique(thd)`，插入成功则 `++atomic_global_thd_count`。前台连接侧的调用点是 `handle_connection()` 里的 `thd_manager->add_thd(thd)`；内部线程侧在 `thd_init()` 里，且后台线程被排除在外。

### 上下文切换

`THD::store_globals()` 把线程局部状态切到本 THD：`current_thd = this`、`THR_MALLOC = &mem_root`、`is_killable = true`、`real_id = my_thread_self()`。`THD::restore_globals()` 反向清空：`real_id = 0`、`current_thd = nullptr`、`THR_MALLOC = nullptr`。`THD::release_resources()` 在 `current_thd == this` 时调用 `restore_globals()`。

协议栈是另一层切换：`THD::push_protocol(Protocol *)` 与 `THD::pop_protocol()` 在 `m_protocol` 上做栈式替换，用于在一条连接里临时套用别的协议对象。

单条命令在 THD 上的落点：`THD::set_command(enum_server_command)` 记录当前命令并同步给 PSI（`set_thread_command`），`THD::set_query(LEX_CSTRING)` 在 `LOCK_thd_query` 下写 `m_query_string`。

### 解析与语句边界

`THD::sql_parser()` 是解析入口，函数头注释已把返回约定写明：成功返回 0 且 `THD::is_error()` 为假，失败返回 1 且 `is_error()` 为真。函数体是三段：`MYSQLparse(this, &root)` 返回非零或 `is_error()` 为真时，调 `THD::cleanup_after_parse_error()` 后返回 true——`cleanup_after_parse_error()` 取 `lex->sphead`，非空则先 `sp->m_parser_data.finish_parsing_sp_body(this)`，并且**只有**在 `sp_runtime_ctx == nullptr`（不在存储过程执行上下文中）时才 `sp_head::destroy(sp)` 并把 `lex->sphead` 置空；解析成功但 `root != nullptr` 时再调 `lex->make_sql_cmd(root)`，其返回非零则返回 true；`root == nullptr` 时按函数注释「解析器已内部调用过 `lex->make_sql_cmd()`」，直接返回 false。

语句结束后 `THD::end_statement()` 只做两件事：调 `lex_end(lex)` 并把 `lex->result` 置空，源码注释明确 Item 的释放不在此处而在 `cleanup_after_query()`，且 `mem_root` 不在此释放而是留到 `dispatch_command` 结束时统一释放。`THD::cleanup_after_query()` 负责清理 Item 列表（`cleanup_items(item_list())` 与 `free_items()`）、把 `where` 复位为常量 `THD::DEFAULT_WHERE`（其定义就是字符串字面量 `"field list"`）、把 `table_map_for_update` 归零、逐个 `logout()` 并清空 `m_view_ctx_list` 里的视图安全上下文、把 `check_for_truncated_fields` 复位为 `CHECK_FIELD_IGNORE`，以及在 `rli_slave` 非空时调 `rli_slave->cleanup_after_query()`。

### 事务状态的挂载与换挂

`THD::Transaction_state::backup(THD *thd)` 备份一份会话事务状态：`m_sql_command`、`m_trx = thd->get_transaction()`、`m_ha_data`（引擎私有数据）、`m_tx_isolation`、`m_tx_read_only`、`m_thd_option_bits`、`m_sql_mode`、`m_transaction_psi`、`m_server_status`、`m_in_lock_tables`、`m_time_zone_used`、`m_transaction_rollback_request`。`THD::Transaction_state::restore(THD *thd)` 用 `thd->set_transaction(this->m_trx)` 把挂载点换回去，再逐项回填其余字段。

`THD::Attachable_trx` 是这套机制的使用者。构造函数先 `m_trx_state.backup(m_thd)`，然后把挂载点整个换掉——`m_thd->m_transaction.release()`（旧上下文已被备份）接着 `m_thd->m_transaction.reset(new Transaction_ctx())`——并强制附加事务的语义：`tx_isolation = ISO_READ_COMMITTED`、`tx_read_only = true`、置 `OPTION_AUTOCOMMIT`、清 `OPTION_NOT_AUTOCOMMIT` 与 `OPTION_BEGIN`、清 `OPTION_BIN_LOG`（附加事务不写 binlog）、清 `SERVER_STATUS_IN_TRANS` 与 `SERVER_STATUS_IN_TRANS_READONLY`、`variables.sql_mode = 0`、`m_transaction_psi = nullptr`、`in_lock_tables = false`、`time_zone_used = false`、`transaction_rollback_request = false`。

析构时顺序相反且必须先提交：`trans_commit_attachable(m_thd)` → `close_thread_tables(m_thd)` → `ha_close_connection(m_thd)` → `m_trx_state.restore(m_thd)` → `restore_backup_open_tables_state(...)` → 还原 query tables 列表。`THD::Attachable_trx_rw` 在父类之外把 `tx_read_only = false`、`lex->sql_command = SQLCOM_END`、`get_transaction()->xid_state()->set_state(XID_STATE::XA_NOTR)`。

### 销毁

`THD::cleanup()` 先把 `killed = KILL_CONNECTION`。事务处理分两支：若 `trn_ctx->xid_state()->has_state(XID_STATE::XA_PREPARED)`，因为连接即将消失无法回滚，改为 `MDL_context_backup_manager::instance().create_backup(...)` 备份元数据锁，失败则记 `ER_XA_CANT_CREATE_MDL_BACKUP`，然后 `xa::Transaction_cache::detach(trn_ctx)`；否则 `xs->set_state(XID_STATE::XA_NOTR)`、`trans_rollback(this)`、`xa::Transaction_cache::remove(trn_ctx)`。之后依次解锁 LOCK TABLES 持有的表（`locked_tables_list.unlock_locked_tables(this)`）、`mysql_ha_cleanup(this)`、`mdl_context.release_transactional_locks()`、释放 global read lock、`mysql_ull_cleanup(this)`、`release_all_locking_service_locks(this)`、`release_backup_lock(this)`，再清 `user_vars`、`user_var_events`、临时表（`close_temporary_tables(this)`）与存储过程/函数缓存；若 `tc_log` 存在则 `tc_log->commit(this, true)`；最后 `session_tracker.deinit()`。结束时若阶段是 `ACTIVE` 置为 `ACTIVE_AND_CLEAN`，否则置为 `CLEANED_UP`。

`THD::release_resources()` 做线程级回收：先 `Global_THD_manager::get_instance()->release_thread_id(m_thread_id)` 归还线程 ID，再 `start_disposal()`（阶段推进到 `SCHEDULED_FOR_DISPOSAL`），随后关闭 classic 协议的 vio（`vio_delete` 与 `end_net()`）、`mysql_audit_free_thd(this)`、`stmt_map.reset()` 关闭所有预处理语句，若 `!is_cleanup_done()` 则补调 `cleanup()`，再 `mdl_context.destroy()`、`ha_close_connection(this)`、`plugin_thdvar_cleanup(this, m_enable_plugins)`、销毁 timer 与 `rli_fake`、释放 `external_store_`；若 `current_thd == this` 则 `restore_globals()`；最后把 `status_var` 汇总进 `global_status_var` 并做 PSI 聚合，阶段置为 `RESOURCES_RELEASED`。

`THD::~THD()` 若 `!release_resources_done()` 则先补 `release_resources()`，然后 `clear_next_event_pos()`、销毁各 mutex 与 cond、`get_transaction()->free_memory()`、`unregister_replica(this, true, true)`、`main_mem_root.Clear()`，阶段置为 `DISPOSED`。

从全局表摘除由 `Global_THD_manager::remove_thd(THD *thd)` 完成：断言 `thd->release_resources_done()`，在 `LOCK_thd_remove[partition]` 与 `LOCK_thd_list[partition]` 下 `erase_unique(thd)`，成功则 `--atomic_global_thd_count`，最后 `mysql_cond_broadcast(&COND_thd_list[partition])` 唤醒等待「所有 THD 退出」的线程——`Global_THD_manager::wait_till_no_thd()` 就是在等这个条件变量。

**释放资源与摘表的先后顺序在两条销毁路径上完全一致，都是「先释放、后摘表、再 delete」，这是本页最容易被写错的地方。** 前台连接路径 `handle_connection()`（`sql/conn_handler/connection_handler_per_thread.cc`）的顺序是：`close_connection(thd, 0, false, false)` → `thd->get_stmt_da()->reset_diagnostics_area()` → `thd->release_resources()` → `thd_manager->remove_thd(thd)` → `delete thd`。内部线程路径 `destroy_thd()`（`sql/sql_thd_internal_api.cc`）的顺序是：`thd->release_resources()` →（`thd->system_thread != SYSTEM_THREAD_BACKGROUND` 时）`thd_manager->remove_thd(thd)` → `delete thd`。两条路径都先调 `release_resources()` 再摘表，所以 `remove_thd()` 里那句 `assert(unit_test || thd->release_resources_done())` 才成立——若把顺序倒过来，这条断言会立刻失败。

### 跨线程观察与中断

`Global_THD_manager::find_thd()` 返回 `THD_ptr`：其构造函数在 `m_underlying != nullptr` 时加 `LOCK_thd_data`（并断言当前线程不持有该锁），`release()` 解锁并交还裸指针，因此调用方持有它期间目标 THD 不会被并发销毁。两个 `find_thd()` 重载在命中目标后都会检查 `thd_ptr->is_being_disposed()`，为真则返回空 `THD_ptr{nullptr}`——按 ID 查找的重载用 `thd_partition(func->m_thread_id)` 直接定位到唯一分片，不做全分片扫描。`Find_thd_with_id::operator()` 在目标 `thd->get_command() == COM_DAEMON` 且未显式允许时返回 false，即按 ID 查找会跳过守护线程。

真实调用方是 `sql/sql_parse.cc` 的 `kill_one_thread(THD *thd, my_thread_id id, bool only_kill_query)`：它构造 `Find_thd_with_id find_thd_with_id(id)`，调 `Global_THD_manager::get_instance()->find_thd(&find_thd_with_id)`，返回值 `tmp` 为空时直接返回函数初值 `ER_NO_SUCH_THREAD`。命中后先做权限判定——`sctx->check_access(SUPER_ACL)` 或 `sctx->has_global_grant(STRING_WITH_LEN("CONNECTION_ADMIN")).first` 或 `sctx->user_matches(tmp->security_context())` 三者之一成立才继续；否则返回 `ER_KILL_DENIED_ERROR`。权限通过后还有两层判断：`tmp->killed != THD::KILL_CONNECTION` 才实际动手，若 `tmp->is_system_user() && !thd->is_system_user()` 则返回 `ER_KILL_DENIED_ERROR`，否则调 `tmp->awake(only_kill_query ? THD::KILL_QUERY : THD::KILL_CONNECTION)` 并置 `error = 0`；目标已在被杀（`killed == THD::KILL_CONNECTION`）时也直接置 `error = 0`。`sql_kill()` 是它的上一层包装：`kill_one_thread()` 返回 0 时 `my_ok(thd)`，否则 `my_error(error, MYF(0), id)`。另一条路径是 `Kill_non_super_conn::operator()`，它作为 `Do_THD_Impl` 经 `do_for_all_thd()` 遍历所有 THD，在目标 THD 的 `LOCK_thd_data` 下判定 `!thd_to_kill->is_connection_admin() && thd_to_kill->killed != THD::KILL_CONNECTION && !thd_to_kill->slave_thread && !has_higher_privilege` 时调 `awake(THD::KILL_CONNECTION)`。

`THD::awake(THD::killed_state)` 要求持有 `LOCK_thd_data`（函数头注释与 `mysql_mutex_assert_owner` 都写明了）。若 `kill_immunizer` 处于激活状态，只 `save_killed_state(state_to_set)` 后返回，等退出免疫模式再由 `awake()` 用保存的状态重放；若 `m_server_idle` 且要杀的是 `KILL_QUERY`，则不置 `killed` 标志，避免影响下一条命令；否则当目标不是当前线程时调 `shutdown_active_vio()` 关闭目标 socket（源码注释说明这是为了避免信号丢失导致目标卡在 `read()`，并显式接受由此产生的竞态），并通过 `Connection_handler_manager::event_functions` 的 `post_kill_notification` 通知调度器；`ha_kill_connection(this)` 打断引擎内的等待；`is_killable` 时在 `current_cond` 与 `current_mutex` 均为非零的前提下广播 `current_cond` 把目标从条件变量上踢醒。`KILL_TIMEOUT` 这一档额外累加 `status_var.max_execution_time_exceeded++`，且函数对它断言 `!status_var_aggregated`。

## 关键取舍

**用会话对象而不是线程对象建模。** 线程缓存与线程池都要求「一条 pthread 服务多条连接」，THD 一连接一对象正好对上；代价是每条连接都要构造并析构一个体量不小的对象（LEX、内存根、安全上下文、两个协议对象），连接建立与断开这条高频路径上这笔开销看得见。

**销毁切成会话级与线程级两段。** `cleanup()` 只管事务、元数据锁、表和锁，`release_resources()` 才管 vio、插件变量、外部存储。这样 `COM_CHANGE_USER`（`THD::cleanup_connection()` → `cleanup()` → `init()`）可以原地复位会话继续用同一个 THD，而连接真正结束时才把线程级资源放掉。代价是两段清理的边界要靠 `is_cleanup_done()` 与 `release_resources_done()` 守住，`release_resources()` 里那句 `if (!is_cleanup_done()) cleanup();` 就是在补齐「换了用户但连接尚未结束」留下的空档。

**生命周期状态显式化。** `m_thd_life_cycle_stage` 让 `remove_thd()` 能断言「必须先释放资源」，让 `find_thd()` 能跳过正在析构的对象；代价是每一次阶段推进都要维护这个字段，且源码对这些取值做了序比较（`>= enum_thd_life_cycle_stages::CLEANED_UP`、`>= enum_thd_life_cycle_stages::SCHEDULED_FOR_DISPOSAL`），说明它们是有序枚举，阶段只能单向前进。

**事务挂载点只有一个。** `set_transaction()` 删除旧 `Transaction_ctx` 再挂新的，杜绝「两个事务上下文同时存在」的歧义；代价是任何需要临时事务的场景都必须先把原状态备份下来——`Transaction_state::backup()`/`restore()` 要备份的字段清单（sql_command、ha_data、tx_isolation、tx_read_only、option_bits、sql_mode、m_transaction_psi、server_status、in_lock_tables、time_zone_used、transaction_rollback_request）就是这笔账。`Attachable_trx` 为了跑一个只读的附加事务，代价是整个会话事务状态被换掉又换回来。

**全局 THD 表按 ID 取模分片。** `thread_id % NUM_PARTITIONS` 让锁竞争分散；代价是遍历必须逐分片进行：`do_for_all_thd()` 在每个分片上持 `LOCK_thd_list` 逐个执行回调，回调期间整个分片锁被占住；`do_for_all_thd_copy()` 则先在 `LOCK_thd_remove[partition]` 与 `LOCK_thd_list[partition]` 双锁下复制一份分片快照，随即释放 `LOCK_thd_list` 再执行回调，保证回调期间新加入的 THD 不被计入、同时不阻塞 `remove_thd()` 的摘表操作。

**保留 ID 专供不入表的临时 THD。** `Global_THD_manager::reserved_thread_id` 固定为 0，构造时就把 0 标记为「已占用」；`add_thd()` 断言新 THD 的 ID 不等于它，`release_thread_id()` 对 0 直接返回。

**架构推论：** `NUM_PARTITIONS` 的定义在本次稀疏检出未包含的头文件 `sql/mysqld_thd_manager.h` 里，此处不断言其字面写法。但可从 `sql/mysqld_thd_manager.cc` 的两处事实推出它等于 8：`Global_THD_manager` 构造函数为成员 `thd_list` 的花括号初始化列表中恰好给出了 8 个 `THD_array` 初始化器，而 `thd_list` 是按 `NUM_PARTITIONS` 定长声明的数组；同一文件的 `Global_THD_manager::Global_THD_manager()`、`~Global_THD_manager()`、`wait_till_no_thd()`、`do_for_all_thd_copy()`、`do_for_all_thd()`、`find_thd()` 两个重载以及 `thd_lock_thread_count()` / `thd_unlock_thread_count()` 共 9 处循环一律以 `i < NUM_PARTITIONS` 为上界，且分片下标一律由 `thd_partition(thread_id)` 的 `thread_id % NUM_PARTITIONS` 得出。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_class.cc` | `THD::THD` | 构造会话对象，建立 `m_transaction` 挂载点与各互斥量 |
| `sql/sql_class.cc` | `THD::init` / `THD::cleanup_connection` | 会话状态初始化与「换用户」时的原地复位 |
| `sql/sql_class.cc` | `THD::set_new_thread_id` | 分配全局线程 ID 并同步 `pseudo_thread_id` |
| `sql/sql_class.cc` | `THD::store_globals` / `THD::restore_globals` | 线程局部上下文（`current_thd`、`THR_MALLOC`）的切换与清空 |
| `sql/sql_class.cc` | `THD::push_protocol` / `THD::pop_protocol` | 协议对象在 `m_protocol` 上的栈式替换 |
| `sql/sql_class.cc` | `THD::sql_parser` | 解析入口，失败时调 `cleanup_after_parse_error` 还原 LEX |
| `sql/sql_class.cc` | `THD::cleanup_after_parse_error` | 解析出错时收尾存储过程解析状态并按需销毁 `sp_head` |
| `sql/sql_class.cc` | `THD::end_statement` / `THD::cleanup_after_query` | 语句边界清理：LEX 收尾与 Item 释放、`where` 复位 |
| `sql/sql_class.cc` | `THD::Transaction_state::backup` / `restore` | 会话事务状态的备份与回填 |
| `sql/sql_class.cc` | `THD::Attachable_trx` / `THD::Attachable_trx_rw` | 换挂一个新的 `Transaction_ctx` 跑只读附加事务 |
| `sql/sql_class.cc` | `THD::set_transaction` | 事务挂载点的唯一换挂入口，删除旧 `Transaction_ctx` |
| `sql/sql_class.cc` | `THD::is_cleanup_done` / `release_resources_done` / `is_being_disposed` / `start_disposal` | 生命周期阶段的判定与推进 |
| `sql/sql_class.cc` | `THD::cleanup` | 会话级清理：事务回滚、元数据锁、表与锁释放 |
| `sql/sql_class.cc` | `THD::release_resources` | 线程级清理，阶段推进到 `RESOURCES_RELEASED` |
| `sql/sql_class.cc` | `THD::awake` | 跨线程中断：置 `killed`、关 vio、踢条件变量 |
| `sql/sql_class.cc` | `THD::~THD` | 析构，阶段推进到 `DISPOSED` |
| `sql/mysqld_thd_manager.cc` | `Global_THD_manager::add_thd` / `remove_thd` | 全局 THD 分片列表的登记与摘除 |
| `sql/mysqld_thd_manager.cc` | `Global_THD_manager::get_new_thread_id` / `release_thread_id` | 线程 ID 的分配、去重与归还 |
| `sql/mysqld_thd_manager.cc` | `Global_THD_manager::find_thd` | 定位 THD，并跳过正在销毁的会话对象 |
| `sql/mysqld_thd_manager.cc` | `Global_THD_manager::do_for_all_thd` / `do_for_all_thd_copy` | 遍历全部 THD；后者先取快照再放锁执行 |
| `sql/mysqld_thd_manager.cc` | `Global_THD_manager::wait_till_no_thd` | 等待所有分片的 THD 列表清空 |
| `sql/mysqld_thd_manager.cc` | `THD_ptr` | 持 `LOCK_thd_data` 的 THD 引用 |
| `sql/mysqld_thd_manager.cc` | `Find_thd_with_id` | 按 thread_id 定位 THD，跳过 `COM_DAEMON` |
| `sql/conn_handler/connection_handler_per_thread.cc` | `init_new_thd` / `handle_connection` | 前台连接的 THD 建立点与销毁点 |
| `sql/sql_thd_internal_api.cc` | `create_thd` / `thd_init` / `destroy_thd` | 内部线程的 THD 建立、入表与销毁 |
| `sql/sql_parse.cc` | `kill_one_thread` / `sql_kill` | 跨线程定位目标 THD 并调用 `awake` 中断 |
| `sql/sql_parse.cc` | `Kill_non_super_conn` / `killall_non_super_threads` | 批量中断非特权连接 |

## 与主核 / AI 的关系

主核（`engine/db`）把事务放在 L2 的 txn 层，会话侧没有 MySQL 这样「可被跨线程查找、可被 KILL」的会话注册表。

**架构推论：** THD 有两点值得主核借鉴。其一是「一次工作单元一个独立上下文对象 + 显式生命周期阶段」：`m_thd_life_cycle_stage` 让「正在被销毁」这件事对其他线程可观察，主核若要支持连接级取消，同样需要一个能跨线程读到的会话状态字段，而不是各处散落判断。其二是「事务上下文挂在会话对象上、需要临时独立事务时换挂载点」：`Attachable_trx` 用 backup/restore 换挂 `Transaction_ctx`，让数据字典这类系统动作不污染用户事务；主核的 SPI 层若要跑只读系统查询，可以照这个模式做，而不必复用用户事务。

对 AI 场景而言，THD 给出了代理接入数据库的一个清晰切面：代理的一次工具调用对应一个会话上下文对象，其可取消性（`awake` 与 `killed` 标志）、可观测性（`Global_THD_manager` 的全局表、PSI 的线程与语句聚合）、事务隔离（`Transaction_ctx` 挂载点与 `Attachable_trx` 只读附加事务）三件事都有明确落点，而不是隐在调用栈里。

## 参考

- [MySQL 源码：`sql/sql_class.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_class.cc)
- [MySQL 源码：`sql/mysqld_thd_manager.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/mysqld_thd_manager.cc)
- [MySQL 源码：`sql/conn_handler/connection_handler_per_thread.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/conn_handler/connection_handler_per_thread.cc)
- [MySQL 源码：`sql/sql_thd_internal_api.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_thd_internal_api.cc)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL Server 开发文档：`class THD`](https://dev.mysql.com/doc/dev/mysql-server/latest/classTHD.html)
- [MySQL 参考手册：InnoDB 事务模型](https://dev.mysql.com/doc/refman/8.0/en/innodb-transactions.html)
- [MySQL 参考手册：Performance Schema 线程表](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-threads.html)
- [MySQL 参考手册：KILL 语句](https://dev.mysql.com/doc/refman/8.0/en/kill.html)
- [MySQL 参考手册：SHOW PROCESSLIST 语句](https://dev.mysql.com/doc/refman/8.0/en/show-processlist.html)
