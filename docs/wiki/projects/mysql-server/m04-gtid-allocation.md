---
project: mysql-server
feature: gtid-allocation
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_gtid_persist.cc
    symbols: [Gtid_table_persistor::save, Gtid_table_persistor::write_row]
  - path: sql/rpl_gtid_mode.cc
    symbols: [Gtid_mode::set, Gtid_mode::get]
---

# GTID 分配与持久化

## 一句话职责

GTID 分配与持久化机制负责两件事：**给事务分配一个全局唯一的 GTID**（`sidno:gno`，sidno 指向本机 UUID，gno 是该 sidno 下的单调序号），以及**把已分配的 GTID 落进 `mysql.gtid_executed` 表**并最终压缩成连续区间。落盘的实现在 `sql/rpl_gtid_persist.cc` 的 `Gtid_table_persistor::save` / `Gtid_table_persistor::write_row`；决定「要不要分配 GTID」的总开关在 `sql/rpl_gtid_mode.cc` 的 `Gtid_mode::set` / `Gtid_mode::get`。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责事务 GTID 的分配、以及全局已执行 GTID 集合的落盘；**不负责集合的运算（M03）**、**不负责 binlog 里的 GTID 事件（L 组）**。

## 机制是什么

### 三个可分离的层次

从源码看，GTID 的「分配」与「持久化」其实是两套独立机制，靠 `Gtid_state` 这一个结构串起来：

| 层次 | 承载对象 | 关键符号 | 落点 |
|---|---|---|---|
| 模式开关 | `global_gtid_mode`（`Gtid_mode` 实例） | `Gtid_mode::set` / `Gtid_mode::get` | `sql/rpl_gtid_mode.cc` |
| 号段分配 | `Gtid_state` | `Gtid_state::generate_automatic_gtid` / `Gtid_state::get_automatic_gno` / `Gtid_state::acquire_ownership` | `sql/rpl_gtid_state.cc` |
| 落盘 | `Gtid_table_persistor` | `Gtid_table_persistor::save` / `Gtid_table_persistor::write_row` | `sql/rpl_gtid_persist.cc` |

`Gtid_mode::set` 与 `Gtid_mode::get` 的函数体只有两行：

```cpp
void Gtid_mode::set(value_type value) {
  m_atomic_mode.store(value, std::memory_order_release);
}

Gtid_mode::value_type Gtid_mode::get() const {
  return (value_type)m_atomic_mode.load(std::memory_order_acquire);
}
```

即**读写走的是原子量加 release/acquire 配对，而不是文件里那个 `Checkable_rwlock Gtid_mode::lock`**——`Gtid_mode::lock` 在 `sql/rpl_gtid_mode.cc` 里被定义（绑定 PSI key `key_rwlock_gtid_mode_lock`），但本文件内的 `set` / `get` 都不碰它。

### 模式的四个取值与它们的排序

`Gtid_mode::names[]` 定义了模式的字符串表，顺序即数值顺序：

```cpp
const char *Gtid_mode::names[] = {"OFF", "OFF_PERMISSIVE", "ON_PERMISSIVE",
                                  "ON", NullS};
```

`Gtid_mode::from_string` 用 `std::find` 在 `names` 里定位（末尾的 `NullS` 被显式排除：`auto end = names + sizeof(names) / sizeof(names[0]) - 1;`），成功时返回 `static_cast<value_type>(ret - names)`——**数值就是数组下标**；`Gtid_mode::to_string` 反过来返回 `names[value]`。所以四个模式的数值是 OFF=0、OFF_PERMISSIVE=1、ON_PERMISSIVE=2、ON=3，**这个顺序有语义**：`Gtid_state::generate_automatic_gtid` 的判据正是 `if (global_gtid_mode.get() >= Gtid_mode::ON_PERMISSIVE)`。

模式值的来源是 `Gtid_mode::sysvar_mode`（`ulong`）。`sql/mysqld.cc` 的 `gtid_server_init()` 把它灌进全局量：

```cpp
global_gtid_mode.set(
    static_cast<Gtid_mode::value_type>(Gtid_mode::sysvar_mode));
```

`gtid_server_init()` 同一个函数里还创建了 `global_sid_lock`（`Checkable_rwlock`）、`global_sid_map`（`Sid_map`）、`gtid_state`（`Gtid_state`）与 `gtid_table_persistor`（`Gtid_table_persistor`）——**这是全服务器唯一的 GTID 状态与持久化器实例**。

### 分配：两个来源，一个出口

GTID 有两种来源：

- **自动分配**（`GTID_NEXT=automatic`）：`Gtid_state::generate_automatic_gtid` 里 `automatic_gtid.sidno == 0` 时把 sidno 填成本机 sidno（`get_server_sidno()`），`automatic_gtid.gno == 0` 时调 `get_automatic_gno` 取号。
- **显式指定**（`GTID_NEXT=uuid:gno`）：由 `sql/rpl_gtid_execution.cc` 的 `set_gtid_next` 路径处理，走 `Gtid_state::acquire_ownership` 拿所有权。该路径同样受模式约束：`Gtid_mode::ON` 下设成 anonymous 会报 `ER_CANT_SET_GTID_NEXT_TO_ANONYMOUS_WHEN_GTID_MODE_IS_ON`，`Gtid_mode::OFF` 下设成指定 GTID 会报 `ER_CANT_SET_GTID_NEXT_TO_GTID_WHEN_GTID_MODE_IS_OFF`。

模式为 OFF 或 OFF_PERMISSIVE 时，`generate_automatic_gtid` 走 else 分支：**不分配 GTID**，而是把 `thd->owned_gtid.sidno` 置为 `THD::OWNED_SIDNO_ANONYMOUS`、`gno` 置 0，并调 `acquire_anonymous_ownership()`。

### 落盘：`mysql.gtid_executed` 表

`Gtid_table_access_context::TABLE_NAME` 与 `DB_NAME` 把目标表钉死：

```cpp
const LEX_CSTRING Gtid_table_access_context::TABLE_NAME = {
    STRING_WITH_LEN("gtid_executed")};
const LEX_CSTRING Gtid_table_access_context::DB_NAME = {
    STRING_WITH_LEN("mysql")};
```

表结构是三列：`source_id`（sid，UUID 文本）、`interval_start`（gno_start）、`interval_end`（gno_end）。`Gtid_table_persistor::write_row` 就是往这张表里插一行区间的实现。

### 压缩：后台线程把碎片合并成区间

`mysql.gtid_executed` 里每一行是一个区间，但持续写入会产生大量碎片行。`sql/rpl_gtid_persist.cc` 用一个独立后台线程做压缩，线程入口是文件内的 `compress_gtid_table`，由 `create_compress_gtid_table_thread()` 创建、由 `terminate_compress_gtid_table_thread()` 终止。

## 怎么实现的

### 第一步：取号——`Gtid_state::get_automatic_gno`

`get_automatic_gno(rpl_sidno sidno) const` 是自动分配时真正选号的函数。函数体开头是 `assert_sidno_lock_owner(sidno)`，然后用 `Gtid_set::Const_interval_iterator ivit(&executed_gtids, sidno)` 遍历已执行集合在该 sidno 下的区间。

起点不是 1，而是一个「推测的下一个空闲号」：

```cpp
Gtid next_candidate = {sidno,
                       sidno == get_server_sidno() ? next_free_gno : 1};
```

`next_free_gno` 是 `Gtid_state` 的成员，在 `generate_automatic_gtid` 里被更新：`if (automatic_gtid.sidno == get_server_sidno() && automatic_gtid.gno != -1) next_free_gno = automatic_gtid.gno + 1;`。**只有本机 sidno 才走 `next_free_gno` 优化，其它 sidno 从 1 开始。**

函数体的注释把这条优化的理由写得很清楚：一个 GNO 被分配给某个事务后，**它不会从空闲区间里移除**，而是被移进 `owned_gtids` 集合；因此从 1 开始逐个找，常常撞上「已被别人占用但还没提交」的号，导致反复重试。注释给的量级是「一个线程要尝试 N 次，N 等于 FLUSH 阶段的事务数加上 COMMIT 阶段尚未释放所有权的事务数」。

选号循环：

```cpp
while (true) {
  const Gtid_set::Interval *iv = ivit.get();
  rpl_gno next_interval_start = iv != nullptr ? iv->start : GNO_END;
  while (next_candidate.gno < next_interval_start &&
         DBUG_EVALUATE_IF("simulate_gno_exhausted", false, true)) {
    if (owned_gtids.is_owned_by(next_candidate, 0)) return next_candidate.gno;
    next_candidate.gno++;
  }
  if (iv == nullptr ||
      DBUG_EVALUATE_IF("simulate_gno_exhausted", true, false)) {
    my_error(ER_GNO_EXHAUSTED, MYF(0));
    return -1;
  }
  if (next_candidate.gno < iv->end) next_candidate.gno = iv->end;
  ivit.next();
}
```

三个要点：

- **候选号必须没被任何人占用**：`owned_gtids.is_owned_by(next_candidate, 0)` 的第二个参数是 `0`，表示**不排除任何线程**，任何线程持有该号都不算空闲。
- **号段耗尽报 `ER_GNO_EXHAUSTED`**，返回 `-1`。
- **跳过已执行区间**：候选号落在某个已执行区间内时，直接跳到 `iv->end`（`if (next_candidate.gno < iv->end) next_candidate.gno = iv->end;`），不逐号试探。

`next_free_gno` 的回填逻辑在 `Gtid_state::update_gtids_impl_own_gtid`：注释说明事务回滚释放所有权时，若释放的 GNO 小于 `next_free_gno`，就把 `next_free_gno` 拉回该值，以填补 `GTID_EXECUTED` 里的空洞。

### 第二步：拿所有权——`Gtid_state::acquire_ownership`

`acquire_ownership(THD *thd, const Gtid &gtid)` 的前置条件写在函数体开头：

```cpp
global_sid_lock->assert_some_lock();
gtid_state->assert_sidno_lock_owner(gtid.sidno);
assert(!executed_gtids.contains_gtid(gtid));
assert(thd->owned_gtid.sidno == 0);
```

即**调用方必须同时持有 `global_sid_lock` 和该 sidno 的 sidno 锁，且这个 GTID 不能已在已执行集合里、也不能已被本线程持有**。

函数体主路径：`owned_gtids.add_gtid_owner(gtid, thd->thread_id())` 成功后，若线程有 `gtid_next` 列表（`thd->get_gtid_next_list() != nullptr`）则走 `HAVE_GTID_NEXT_LIST` 分支把 GTID 并入 `thd->owned_gtid_set` 并置 `thd->owned_gtid.sidno = THD::OWNED_SIDNO_GTID_SET`；否则直接 `thd->owned_gtid = gtid`，并把 `thd->owned_sid` 设为 `sid_map->sidno_to_sid(gtid.sidno)`，同时更新 `thd->rpl_thd_ctx.last_used_gtid_tracker_ctx().set_last_used_gtid(gtid)`。

失败路径（`err:` 标签）会把这笔所有权**回滚**：列表分支里逐个 `owned_gtids.remove_gtid(g)`，然后 `thd->clear_owned_gtids()`，返回 `RETURN_REPORTED_ERROR`。

### 第三步：模式闸门——`generate_automatic_gtid` 的分叉

`Gtid_state::generate_automatic_gtid` 是分配的总入口，模式判断写在最前面：

```cpp
if (global_gtid_mode.get() >= Gtid_mode::ON_PERMISSIVE) {
```

分支内：

1. `automatic_gtid.sidno == 0` → `automatic_gtid.sidno = get_server_sidno()`。
2. 按需 `lock_sidno(automatic_gtid.sidno)`。注释说明：若调用方已传了 `locked_sidno` 且其值大于 0 而又与本次不同，则先 `unlock_sidno(*locked_sidno)`；注释同时指出「以当前代码来看这不会发生，因为服务端只用本机 UUID 作为 sid 生成自动 GTID」。
3. `automatic_gtid.gno == 0` → 调 `get_automatic_gno` 取号，并更新 `next_free_gno`。
4. `automatic_gtid.gno == -1 || acquire_ownership(thd, automatic_gtid)` 任一成立 → `ret = RETURN_STATUS_REPORTED_ERROR`。
5. 收尾调 `gtid_set_performance_schema_values(thd)` 把结果写进 Performance Schema。

### 第四步：落盘——`Gtid_table_persistor::save` 的三个重载

`Gtid_table_persistor` 有三个 `save`，分工明确：

| 重载 | 签名 | 用途 |
|---|---|---|
| 单 GTID | `save(THD *thd, const Gtid *gtid)` | 提交单个事务时把它自己的 GTID 写进表 |
| 整集合 | `save(const Gtid_set *gtid_set, bool compress)` | 把一整个 GTID 集合写进表，并按需触发压缩 |
| 表级 | `save(TABLE *table, const Gtid_set *gtid_set)` | 已打开表的情况下直接按区间逐行写 |

`save(THD *thd, const Gtid *gtid)` 的函数体顺序：

1. **取 sid**：在 `global_sid_lock` 读锁下 `global_sid_map->sidno_to_sid(gtid->sidno)` 得到 `rpl_sid`，再 `sid.to_string(buf)` 转成文本。缓冲区是 `char buf[binary_log::Uuid::TEXT_LENGTH + 1]`。
2. **开表**：`table_access_ctx.init(&thd, &table, true)` 失败则 `error = 1` 并跳到 `end`。
3. **调试分支**：`DBUG_EXECUTE_IF("disable_se_persists_gtid", { error = write_row(table, buf, gtid->gno, gtid->gno); });`——注释写明「直接写 `gtid_executed` 表仅为满足调试测试」。
4. **标记由 SE 持久化**：`thd->request_persist_gtid_by_se();`。
5. **错误注入**：`DBUG_EXECUTE_IF("simulate_err_on_write_gtid_into_table", ...)` 里 `error = -1` 并调 `thd->reset_gtid_persisted_by_se();`。
6. **收尾**：`table_access_ctx.deinit(thd, table, 0 != error, false)`；若 deinit 失败，再调 `thd->reset_gtid_persisted_by_se()` 并把 `error` 置 `-1`。函数注释说明压缩「由 GTID 后台线程按需触发」。

`save(const Gtid_set *gtid_set, bool compress)` 有一处值得注意的**容错设计**：若 `table_access_ctx.init` 失败，源码注释写明「GTID 表还没准备好用，打开失败。忽略这个错误」，于是 `thd->clear_error()` 并 `thd->get_stmt_da()->set_ok_status(0, 0, nullptr)`，然后跳到 `end`——**打不开表不视为致命错误**。

集合落盘走 `save(TABLE *table, const Gtid_set *gtid_set)`：先 `gtid_set->get_gtid_intervals(&gtid_intervals)` 取出全部区间，再逐区间 `write_row(table, buf, iter->gno_start, iter->gno_end)`，**任一区间写失败就 `break`**。

### 第五步：写一行——`Gtid_table_persistor::write_row`

`write_row(TABLE *table, const char *sid, rpl_gno gno_start, rpl_gno gno_end)` 的函数体：

```cpp
fields = table->field;
empty_record(table);

if (fill_fields(fields, sid, gno_start, gno_end)) return -1;

error = table->file->ha_write_row(table->record[0]);
if (DBUG_EVALUATE_IF("simulate_err_on_write_gtid_into_table", (error = -1),
                     error)) {
  if (error == HA_ERR_FOUND_DUPP_KEY) {
    LogErr(WARNING_LEVEL, ER_GTID_ALREADY_ADDED_BY_USER,
           Gtid_table_access_context::TABLE_NAME.str);
  } else {
    table->file->print_error(error, MYF(0));
    return -1;
  }
}
return 0;
```

三个必须记住的行为：

- **重复主键不算失败**：`HA_ERR_FOUND_DUPP_KEY` 只记一条 WARNING 级 `ER_GTID_ALREADY_ADDED_BY_USER`（错误信息里带上表名 `gtid_executed`），函数仍返回 0。
- **其它写错误一律致命**：`print_error` 后返回 `-1`，且注释明确「这保证返回的是 -1 而不是 handler 返回的状态码」。
- **`fill_fields` 的三列写入**：字段 0 存 sid（`binary_log::Uuid::TEXT_LENGTH` 长度，字符集 `my_charset_bin`），字段 1 存 `gno_start`，字段 2 存 `gno_end`，**后两者都以 `unsigned = true` 存储**。任一 `store` 失败报 `ER_RPL_INFO_DATA_TOO_LONG` 并返回 `-1`。

### 第六步：压缩——后台线程与区间合并

压缩线程的信号机制是 `LOCK_compress_gtid_table` + `COND_compress_gtid_table` 两个全局量：

- **触发压缩**：`save(const Gtid_set *gtid_set, bool compress)` 末尾，若 `error == 0 && compress && DBUG_EVALUATE_IF("dont_compress_gtid_table", 0, 1)`，则 `mysql_mutex_lock(&LOCK_compress_gtid_table)` → `should_compress = true` → `mysql_cond_signal(&COND_compress_gtid_table)` → 解锁。
- **线程启动即压一次**：线程函数 `compress_gtid_table` 在进入主循环**之前**就置 `should_compress = true`，源码注释是「在服务器启动时压缩表」。
- **等待条件**：主循环用 `while (!(should_compress || terminate_compress_thread)) mysql_cond_wait(&COND_compress_gtid_table, &LOCK_compress_gtid_table);`，注释写明这个 while 是「为了处理系统的伪唤醒（spurious wakeups）」。
- **错误被吞掉**：压缩失败只记一条 WARNING 级 `ER_FAILED_TO_COMPRESS_GTID_EXECUTED_TABLE` 并 `thd->clear_error()`，注释说明「压缩线程会吞掉任何错误，去等下一次压缩信号，直到它被终止」。

真正的合并动作在 `Gtid_table_persistor::compress_first_consecutive_range`：按主键 `(sid, gno_start)` 递增顺序读行，判据是 `sid == cur_sid && gno_end + 1 == cur_gno_start`——**同一 sid 且当前区间末尾加一正好等于下一区间开头**。命中就 `find_first_consecutive_gtids = true` 并把 `gno_end` 推到 `cur_gno_end`，同时**删除当前行**（注释说明「不删除第一个连续 GTID，以便稍后更新它」）；遇到非连续区间时，若已经找到过连续段就 `break`。

合并结束后，若 `find_first_consecutive_gtids` 为真，调 `update_row(table, sid.c_str(), gno_start, gno_end)` 把第一行的 `gno_end` 改成整段的末尾。

「是否压完」的判据是 `is_complete = (err == HA_ERR_END_OF_FILE);`。

### 压缩的调度粒度

`Gtid_table_persistor::compress(THD *thd)` 的循环是：

```cpp
while (!is_complete && !error)
  error = compress_in_single_transaction(thd, is_complete);
```

即**每次压缩只处理一个事务内的一「段」连续区间**（`compress_in_single_transaction`），没压完就再来一轮——每一轮都开一次表、提交一次事务。压缩全程持 `LOCK_reset_gtid_table`。

压缩期间线程状态被设为 `stage_compressing_gtid_table`（`THD_STAGE_INFO`），源码注释说明这是为了「覆盖 `open_table(...)` 里设的 `stage_system_lock`」。

### 权限与只读绕过

压缩线程需要更新 `mysql.gtid_executed`，所以必须绕过只读检查。源码里有三处：

- `Gtid_table_access_context::before_open` 设置打开标志 `m_flags = (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH | MYSQL_LOCK_IGNORE_TIMEOUT | MYSQL_OPEN_IGNORE_KILLED)`。注释写明这样做的目的是「允许在会话断开期间操作 `gtid_executed` 表」。
- 压缩线程自己 `thd->set_skip_readonly_check()`，注释说明「GTID 表压缩线程应当忽略 `read-only` 和 `super_read_only` 选项，以便它能更新 `mysql.gtid_executed` 复制仓库表」。
- `Gtid_table_access_context::create_thd` 里也调 `thd->set_skip_readonly_check()`。

写操作还要**临时关掉 binlog**：`Gtid_table_access_context::init` 的 `m_is_write` 分支把 `(*thd)->variables.option_bits` 里的 `OPTION_BIN_LOG` 位清掉并存回 `m_tmp_disable_binlog__save_options`，在 `deinit` 里恢复。**GTID 表自身的写入不进 binlog**。

### 事务边界：`Attachable_trx_rw`

`Gtid_table_access_context::init` 里有一段关于事务类型的判定：

```cpp
if (!(*thd)->get_transaction()->xid_state()->has_state(XID_STATE::XA_NOTR)) {
  assert(... has_state(XID_STATE::XA_IDLE) || ... has_state(XID_STATE::XA_PREPARED));
  (*thd)->begin_attachable_rw_transaction();
}
```

源码注释解释了为什么可以这么做：「这类调用方与主事务之间是无死锁的，因为禁止 XA 主事务更新 `mysql.gtid_executed`。」`deinit` 里对应调 `thd->end_attachable_transaction()`。

### 谁来触发落盘

落盘的触发点分布在三处：

| 触发点 | 位置 | 调用的函数 |
|---|---|---|
| 提交时写自己的 GTID | `sql/handler.cc` 的 `ha_commit_low` 相关路径 | `gtid_state->save(thd)`，条件是 `thd->owned_gtid.sidno > 0 && !thd->se_persists_gtid()` |
| 不写 binlog 的语句 | `sql/binlog.cc` 的 `MYSQL_BIN_LOG::gtid_end_transaction` | `gtid_state->save(thd)`，条件是 `!opt_bin_log || (thd->slave_thread && !opt_log_replica_updates)` |
| binlog 轮转时补齐上一个文件的 GTID | `sql/binlog.cc` | `gtid_state->save_gtids_of_last_binlog_into_table()` |
| 关机时补齐 | `sql/mysqld.cc` | `gtid_state->save_gtids_of_last_binlog_into_table()`，失败记 WARNING `ER_CANT_SAVE_GTIDS` |
| 崩溃恢复时补齐 | `sql/mysqld.cc` | `gtid_state->save(&gtids_in_binlog_not_in_table)`，返回 `-1` 则 `unireg_abort(MYSQLD_ABORT_EXIT)` |

`sql/handler.cc` 的注释解释了「SE 已持久化 GTID」的情况：「GTID 将由 `Commit_order_manager::flush_engine_and_signal_threads`（从 `Commit_order_manager::wait_and_finish` 调入）保存」，因此 `thd->se_persists_gtid()` 为真时跳过。

`sql/binlog.cc` 的 `gtid_end_transaction` 注释还记录了 `gtid_state->save` 的**隐式提交链**：`Gtid_state::save` → `Gtid_table_persistor::save` → `Gtid_table_access_context::deinit` → `System_table_access::close_table` → `ha_commit_trans` → … → `ha_commit_low`。注释同时指出：若 `replica-preserve-commit-order` 未启用，这条栈不会调 `update_on_commit`，因为它是从 `MYSQL_BIN_LOG::commit` 的特例分支进来、跳过了 `ordered_commit` 的。

### 落盘的错误语义

`Gtid_state::save(THD *thd)` 把 `gtid_table_persistor->save(thd, &thd->owned_gtid)` 的返回值翻译成两种语义：

- 返回 `1`——**表打不开**，注释写明「GTID 表还没准备好用，打开失败。忽略这个错误」，于是 `thd->clear_error()` 并 `set_ok_status(0, 0, nullptr)`，函数返回 0（成功）。
- 返回 `-1`——**真错误**，函数返回 `-1`。

这个区分很关键：**「表暂时打不开」被降级为成功**，而「写了但写失败」才是致命错误。

### 写表时表打不开会怎样

`System_table_access::open_table`（`sql/rpl_table_access.cc`）在 `open_n_lock_single_table` 失败时，**根据 `thd->is_operating_gtid_table_implicitly` 分两种报错**：

```cpp
if (thd->is_operating_gtid_table_implicitly)
  LogErr(WARNING_LEVEL, ER_RPL_GTID_TABLE_CANNOT_OPEN, dbstr.c_str(), tbstr.c_str());
else
  my_error(ER_NO_SUCH_TABLE, MYF(0), dbstr.c_str(), tbstr.c_str());
```

前者只记 WARNING（隐式操作 gtid 表时），后者报 `ER_NO_SUCH_TABLE`（用户显式操作时）。`is_operating_gtid_table_implicitly` 由 `Gtid_table_access_context::init` 置真、`deinit` 置假。

同一函数还有一道**列数校验**：`if (tables.table->s->fields < max_num_field)` 时 `ha_rollback_trans` 并报 `ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2`，注释说「这只可能发生在有人启动服务端后又改了表结构的情况下」。传入的 `max_num_field` 就是 `Gtid_table_persistor::number_fields`。

## 关键取舍

### 取舍一：模式开关用原子量而不是读写锁

`Gtid_mode::set` / `Gtid_mode::get` 用 `m_atomic_mode.store(value, std::memory_order_release)` 与 `load(std::memory_order_acquire)`，而同一文件里明明定义了 `Checkable_rwlock Gtid_mode::lock`（绑定 PSI key `key_rwlock_gtid_mode_lock`）却没有用它。

**取舍：读写路径零锁开销、无阻塞。** 代价是这个 `Gtid_mode::lock` 在 `sql/rpl_gtid_mode.cc` 内没有任何使用者——**模式值一旦设定就靠原子量读，模式本身的并发修改没有锁保护**。release/acquire 配对保证的是「写入者之前的数据写入对读到新模式值的线程可见」，而不是模式切换与其它状态的原子协同。

### 取舍二：GTID 号从「推测的下一个空闲号」起步

`get_automatic_gno` 对本机 sidno 用 `next_free_gno` 起步，其它 sidno 从 1 起步。

**取舍：命中率高的时候省掉大量试探。** 代价是**必须靠回填维持正确性**——事务回滚时若释放的 GNO 小于 `next_free_gno`，`update_gtids_impl_own_gtid` 要把 `next_free_gno` 拉回去，否则这个空洞就永远被跳过。

### 取舍三：候选号必须「没人持有」，不只看已执行集合

`owned_gtids.is_owned_by(next_candidate, 0)` 的第二个参数是 `0`，表示不排除任何线程。已执行集合 `executed_gtids` 和在途持有集合 `owned_gtids` 是**两套独立的判据**：前者防重复，后者防并发抢号。

**取舍：多查一个集合，换取「同一 sidno 下不出现两个并发事务拿同一个号」。** 代价是注释里点明的重复试探——号被分配后不会从空闲区间移除，所以只能靠 `owned_gtids` 判据兜底。

### 取舍四：表打不开降级为成功，真写失败才算致命

`Gtid_state::save(THD *thd)` 把 persistor 的返回值 `1` 翻译成「忽略这个错误」并 `set_ok_status(0, 0, nullptr)`，只有 `-1` 才向上传播。

**取舍：GTID 表暂时不可用不阻塞业务事务。** 代价是**「已执行集合」与「表内容」之间可能出现缺口**——这正是 `Gtid_state::save_gtids_of_last_binlog_into_table()` 在 binlog 轮转和关机时补齐、以及崩溃恢复时用 `gtids_in_binlog_not_in_table` 补齐的原因。源码注释列出了需要补齐的四种情形：升级场景、从主库备份初始化后又 `RESET MASTER` / `RESET REPLICA` 的场景、上次 `RESET MASTER` 后没轮转过 binlog 的场景、以及崩溃导致最后一个 binlog 的 GTID 未落表的场景。

### 取舍五：重复主键只告警不失败

`write_row` 遇到 `HA_ERR_FOUND_DUPP_KEY` 只记 WARNING `ER_GTID_ALREADY_ADDED_BY_USER`，返回 0。

**取舍：用户手工往 `mysql.gtid_executed` 塞过 GTID 时，服务器不会因此报错。** 代价是这类重复写入在错误日志里只是一条告警，不会中断事务。

### 取舍六：压缩在后台线程里、分事务进行

`compress()` 的循环是 `while (!is_complete && !error) error = compress_in_single_transaction(thd, is_complete);`，每次只合并一段连续区间就提交一次事务。

**取舍：单次事务小、锁持有时间短、不与业务事务争长事务槽。** 代价是压缩不是原子的——中途失败会留下部分合并、部分未合并的状态，且线程会吞掉错误等下一轮信号。

### 取舍七：GTID 表的写入不进 binlog

`Gtid_table_access_context::init` 在 `m_is_write` 为真时清掉 `OPTION_BIN_LOG` 位，`deinit` 恢复。

**取舍：`mysql.gtid_executed` 自身的变化不产生 binlog 事件。** 代价是从库看不到源库的 GTID 表变动——但从库有自己的一份表，这是刻意的不对称。

### 取舍八：写 GTID 表要绕开只读与全局读锁

`before_open` 设置的五个 `MYSQL_OPEN_IGNORE_*` / `MYSQL_LOCK_IGNORE_*` 标志，加上 `set_skip_readonly_check()`，让压缩线程和 GTID 落盘路径在 `read_only` / `super_read_only` 下仍能工作。

**取舍：复制仓库表的可写性不受全局只读约束。** 代价是这套绕过标志只在 `Gtid_table_access_context` 这一个上下文里生效——用户显式操作该表时走的是 `ER_NO_SUCH_TABLE` / 常规权限路径。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::save` | 三个重载：单 GTID 落盘（取 sid → 开表 → `request_persist_gtid_by_se` → deinit）、整集合落盘（失败降级为成功 + 触发压缩信号）、表级按区间逐行写 |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::write_row` | 往 `mysql.gtid_executed` 插一行区间：清记录 → `fill_fields` → `ha_write_row`；重复主键记 WARNING `ER_GTID_ALREADY_ADDED_BY_USER`，其它错误返回 `-1` |
| sql/rpl_gtid_mode.cc | `Gtid_mode::set` | 把模式值写入原子量，`memory_order_release` |
| sql/rpl_gtid_mode.cc | `Gtid_mode::get` | 从原子量读模式值，`memory_order_acquire` |
| sql/rpl_gtid_mode.cc | `Gtid_mode::names` | 模式字符串表，顺序即数值顺序（OFF / OFF_PERMISSIVE / ON_PERMISSIVE / ON），末尾 `NullS` 终止 |
| sql/rpl_gtid_mode.cc | `Gtid_mode::from_string` | 用 `std::find` 在 `names` 里定位，成功返回数组下标作为模式值，失败返回 `(true, OFF)` |
| sql/rpl_gtid_mode.cc | `Gtid_mode::to_string` | 反查：`names[value]` |
| sql/rpl_gtid_mode.cc | `Gtid_mode::lock` | `Checkable_rwlock`，绑定 PSI key `key_rwlock_gtid_mode_lock`；本文件内的 `set` / `get` 都不使用它 |
| sql/rpl_gtid_state.cc | `Gtid_state::get_automatic_gno` | 自动选号：起点为本机 sidno 用 `next_free_gno`、其它从 1；跳过已执行区间；`owned_gtids.is_owned_by(next_candidate, 0)` 判空闲；号段耗尽报 `ER_GNO_EXHAUSTED` 并返回 `-1` |
| sql/rpl_gtid_state.cc | `Gtid_state::generate_automatic_gtid` | 分配总入口：模式 `>= Gtid_mode::ON_PERMISSIVE` 才分配，否则置 `THD::OWNED_SIDNO_ANONYMOUS`；取号后更新 `next_free_gno`，再 `acquire_ownership` |
| sql/rpl_gtid_state.cc | `Gtid_state::acquire_ownership` | 拿 GTID 所有权：`add_gtid_owner` → 设 `thd->owned_gtid` / `thd->owned_sid` / last-used tracker；失败时回滚所有权并 `clear_owned_gtids()` |
| sql/rpl_gtid_state.cc | `Gtid_state::save` | 把 persistor 返回值 `1`（表打不开）降级为成功，返回 `-1` 才传播错误 |
| sql/rpl_gtid_state.cc | `Gtid_state::save_gtids_of_last_binlog_into_table` | 用 `executed_gtids - previous_gtids_logged - gtids_only_in_table` 算出上一个 binlog 的 GTID 集合并落表；栈上预分配 `PREALLOCATED_INTERVAL_COUNT = 64` 个区间槽位 |
| sql/rpl_gtid_state.cc | `Gtid_state::update_gtids_impl_own_gtid` | 提交/回滚时把 GTID 并入 `executed_gtids` 或在回滚时回填 `next_free_gno` |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::compress` | 压缩主循环：`while (!is_complete && !error) error = compress_in_single_transaction(thd, is_complete);`，压完把 `m_atomic_count` 归零 |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::compress_in_single_transaction` | 单次压缩事务：持 `LOCK_reset_gtid_table`，开表后调 `compress_first_consecutive_range` |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::compress_first_consecutive_range` | 按主键递增扫描合并连续区间：判据 `sid == cur_sid && gno_end + 1 == cur_gno_start`；删除多余行并用 `update_row` 改写首行 `gno_end`；`is_complete = (err == HA_ERR_END_OF_FILE)` |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::update_row` | 按 `(sid, gno_start)` 精确读后改写 `gno_end`；找不到行则 `err` 非 0 返回 `-1` |
| sql/rpl_gtid_persist.cc | `Gtid_table_persistor::fill_fields` | 三列写入：sid 用 `binary_log::Uuid::TEXT_LENGTH` 与 `my_charset_bin`，gno_start / gno_end 以 unsigned 存；失败报 `ER_RPL_INFO_DATA_TOO_LONG` |
| sql/rpl_gtid_persist.cc | `Gtid_table_access_context::init` | 开表准备：临时清 `OPTION_BIN_LOG` 关 binlog、按 XA 状态决定是否 `begin_attachable_rw_transaction`、置 `is_operating_gtid_table_implicitly`、以 `Gtid_table_persistor::number_fields` 为列数下限开表 |
| sql/rpl_gtid_persist.cc | `Gtid_table_access_context::deinit` | 收尾：提交或回滚、结束 attachable 事务、恢复 binlog 开关与只读检查标志、销毁临时 THD |
| sql/rpl_gtid_persist.cc | `Gtid_table_access_context::before_open` | 设置 `MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK`、`MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY`、`MYSQL_OPEN_IGNORE_FLUSH`、`MYSQL_LOCK_IGNORE_TIMEOUT`、`MYSQL_OPEN_IGNORE_KILLED` 五个标志 |
| sql/rpl_gtid_persist.cc | `create_compress_gtid_table_thread` | 创建压缩线程（`mysql_thread_create` + `key_thread_compress_gtid_table`），线程作用域 `PTHREAD_SCOPE_SYSTEM` |
| sql/rpl_gtid_persist.cc | `terminate_compress_gtid_table_thread` | 置 `terminate_compress_thread`、发信号、`my_thread_join` 回收线程 |
| sql/rpl_gtid_mode.cc | `Gtid_mode::sysvar_mode` | 模式系统变量的存储位；`sql/mysqld.cc` 的 `gtid_server_init()` 把它灌进 `global_gtid_mode` |
| sql/mysqld.cc | `gtid_server_init` | 初始化 GTID 全局量：`global_gtid_mode.set(...)`、`global_sid_lock`、`global_sid_map`、`gtid_state`、`gtid_table_persistor` |
| sql/mysqld.cc | 启动自检 | `global_gtid_mode.get() == Gtid_mode::ON` 且 GTID 一致性模式不是 ON 时报 `ER_RPL_GTID_MODE_REQUIRES_ENFORCE_GTID_CONSISTENCY_ON` 并 `unireg_abort(MYSQLD_ABORT_EXIT)` |
| sql/handler.cc | `ha_commit_low` 提交路径 | 条件 `thd->owned_gtid.sidno > 0 && !thd->se_persists_gtid()` 时调 `gtid_state->save(thd)` 落盘 |
| sql/binlog.cc | `MYSQL_BIN_LOG::gtid_end_transaction` | 不写 binlog 的语句（`!opt_bin_log` 或从库且 `!opt_log_replica_updates`）直接 `gtid_state->save(thd)`；注释记录 `save` 的隐式提交链 |
| sql/binlog.cc | `MYSQL_BIN_LOG::new_file_impl` | 非 relay log 时调 `gtid_state->save_gtids_of_last_binlog_into_table()`；失败且错误码为 `ER_RPL_GTID_TABLE_CANNOT_OPEN` 时按文件是否达 `max_size` 分成「放弃轮转」与「报 WARNING 后继续」两条路径 |
| sql/rpl_table_access.cc | `System_table_access::open_table` | 开表并校验列数（`tables.table->s->fields < max_num_field` 时报 `ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2`）；打开失败时按 `is_operating_gtid_table_implicitly` 分「WARNING `ER_RPL_GTID_TABLE_CANNOT_OPEN`」与「`ER_NO_SUCH_TABLE`」 |
| sql/rpl_table_access.cc | `System_table_access::close_table` | 关闭表：有错则 `ha_rollback_trans`，无错则 `ha_commit_trans`（`ignore_global_read_lock = true`）；`need_commit` 为真时再做一次提交/回滚 |
| sql/rpl_gtid_execution.cc | `set_gtid_next` 处理路径 | 模式校验：`Gtid_mode::ON` 下设 anonymous 报 `ER_CANT_SET_GTID_NEXT_TO_ANONYMOUS_WHEN_GTID_MODE_IS_ON`；`Gtid_mode::OFF` 下设指定 GTID 报 `ER_CANT_SET_GTID_NEXT_TO_GTID_WHEN_GTID_MODE_IS_OFF`；指定 GTID 走 `Gtid_state::acquire_ownership` |

## 与主核 / AI 的关系

主核（`engine/db/`）与本页的 GTID 分配机制没有代码级关联，但在「持久化归属」这一层有可对照的形态差异。

- **持久化主体不同。** MySQL 把 GTID 落盘放在**应用层**（`sql/rpl_gtid_persist.cc` 的 `Gtid_table_persistor` 直接写 `mysql.gtid_executed` 表），而 `sql/handler.cc` 的 `ha_commit_low` 路径里有一个 `thd->se_persists_gtid()` 分支——**存储引擎可以自己持久化 GTID**（源码注释明确说「GTID 将由 `Commit_order_manager::flush_engine_and_signal_threads` 保存」）。同一份源码里两条路径并存，说明 MySQL 把「谁负责落盘」做成了可切换的分工。**架构推论：** 主核是否采用类似的「引擎可选持久化」分工需另行核对 `engine/db/` 源码，本页不作断言。
- **落盘失败被降级。** `Gtid_state::save` 把「表打不开」降级为成功，靠 binlog 轮转（`save_gtids_of_last_binlog_into_table`）与崩溃恢复（`gtids_in_binlog_not_in_table`）来补齐缺口。**架构推论：** 这是「允许落后、但保证最终一致」的典型设计——主核 WAL 若有类似的「落盘滞后靠后续补偿」的机制，其形态值得对照，但本页没有核对主核 WAL 的实现。
- **对 AI / CDC 链路而言，GTID 表是「已执行集合」的权威出口。** `sql/rpl_gtid_persist.cc` 的 `Gtid_table_persistor::fetch_gtids` 从表里读回全部 GTID 集合，`encode_gtid_text` 把一行编码成 `sid:start-end` 文本再交给 `gtid_set->add_gtid_text`。**架构推论：** AI 侧若要判定「某个事务是否已被本库执行」，读的是 binlog 而不是这张表，两者的一致性由 `Gtid_state::save_gtids_of_last_binlog_into_table` 的补齐逻辑维持；AI 侧若自行维护「已消费 GTID」水位，就不能假设它与 `mysql.gtid_executed` 的内容天然对齐——因为本页记录的「表打不开 → 降级成功 → 事后补齐」路径会让两者短暂出现缺口。
- **GTID 集合的运算不在本页。** 集合的表示、区间合并与包含关系判定属于 M03（GTID 集合）。本页只负责「产生一个号」和「把号写进表」；**架构推论：** AI 侧若要自己维护 GTID 集合的区间合并，等于重新实现 M03 描述的那一层，而不是本页的分配与落盘。

## 参考

- [Replication with Global Transaction Identifiers（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-gtids.html) —— GTID 的官方定位：GTID 代替文件偏移量作为复制定位方式，以及 `gtid_mode` 四档取值（`OFF` / `OFF_PERMISSIVE` / `ON_PERMISSIVE` / `ON`）的官方含义。
- [System Variable References（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/server-system-variables.html) —— `gtid_mode` 与 `gtid_next` 系统变量的官方定义与取值范围，对应本页 `Gtid_mode::sysvar_mode` 与 `Gtid_mode::names`。
- [SET GTID_NEXT（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/set-gtid-next.html) —— `GTID_NEXT` 的 `automatic` / `anonymous` / `uuid:gno` 三种取值的官方语义，以及各取值在 `gtid_mode` 不同档位下的可用性约束，对应本页「分配：两个来源，一个出口」。
- [The mysql.gtid_executed Table（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/gtid-executed-table.html) —— `mysql.gtid_executed` 表的官方结构（`source_id` / `interval_start` / `interval_end` 三列）与语义，对应本页 `Gtid_table_persistor::write_row` 与 `fill_fields`。
- [Global Transaction Identifiers（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/gtid.html) —— GTID 的组成（`source_id:transaction_id`）与官方术语表，是本页 `sidno` / `gno` 命名的上位依据。
- [sql/rpl_gtid_persist.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_gtid_persist.cc) —— 本页 refs 第一条的权威来源：`Gtid_table_persistor::save` / `write_row`、`compress_first_consecutive_range`、`Gtid_table_access_context::init` / `deinit`、压缩线程 `compress_gtid_table`。
- [sql/rpl_gtid_mode.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_gtid_mode.cc) —— 本页 refs 第二条的权威来源：`Gtid_mode::set` / `get`、`names` 数组、`from_string` / `to_string`、`Gtid_mode::lock`。
- [sql/rpl_gtid_state.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_gtid_state.cc) —— 分配侧：`Gtid_state::get_automatic_gno`、`generate_automatic_gtid`、`acquire_ownership`、`save`、`save_gtids_of_last_binlog_into_table` 的真实实现。
- [sql/rpl_table_access.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_table_access.cc) —— `System_table_access::open_table` / `close_table` 的真实实现，含列数校验与「隐式操作 gtid 表 vs 显式操作」两条报错分支。
- [sql/handler.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/handler.cc) —— 提交路径上 `gtid_state->save(thd)` 的调用条件 `thd->owned_gtid.sidno > 0 && !thd->se_persists_gtid()`。
