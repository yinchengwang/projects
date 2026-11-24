---
project: mysql-server
feature: change-db
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_db.cc
    symbols: [mysql_change_db]
---

# USE 与当前库切换

## 一句话职责

`sql/sql_db.cc` 里的 `mysql_change_db` 是 MySQL 切换「当前默认数据库」的唯一入口：它校验库名合法性与访问权限、确认目标库在数据字典中存在、取该库的默认排序规则，然后把库名、有效库级权限（`db_access`）与 `@@collation_database` 三件事一起写进 THD，并在成功后通知会话状态跟踪器。

## 机制是什么

**切换当前库不是一个赋值，而是四件事的原子组合。** `mysql_change_db_impl` 的函数体把这四件事写成了编号的步骤：第 1 步改 THD 里的库名（`thd->set_db()` 或 `thd->reset_db()`），第 2 步更新安全上下文（`thd->security_context()->cache_current_db_access(new_db_access)`），第 3 步更新库字符集环境变量（`thd->db_charset` 与 `thd->variables.collation_database`），第 4 步（不在 impl 里，在 `mysql_change_db` 末尾的 `done:` 标签处）通知 `CURRENT_SCHEMA_TRACKER` 与 `SESSION_STATE_CHANGE_TRACKER`。

**库名在 THD 里是「THD 自持的一份拷贝」。** `mysql_change_db` 先用 `my_strndup(key_memory_THD_db, new_db_name.str, new_db_name.length, MYF(MY_WME))` 把库名复制到 `key_memory_THD_db` 这块内存上，再把它交给 `mysql_change_db_impl` 写进 THD。函数注释说明了原因：5.0 之前不同的模块用不同的内存存库名，导致内存破坏——存储过程设置的栈指针被复制线程在栈早已消失后使用。注释同时点明了这块内存的归属：「in mysql_change_db_impl() new_db_file_name is assigned to THD attributes and will be freed in THD::~THD()」。

**`force_switch` 是这套机制里唯一的语义分叉点。** 它把「校验失败」从致命错误降级为「把当前库清空并继续」。函数注释逐条列出了两种模式下的差异：库名为空、库名非法、用户无权限、目标库不存在这四种情形，在 `force_switch` 为假时都是失败；在 `force_switch` 为真时，前两种会清空当前库并仍然失败（库名非法那一条是「清空但仍返回失败」），无权限那一条直接跳过权限检查（注释里对这一条留了 `TODO: is this really the intention? (see sp-security.test)` 的疑问），目标库不存在则清空当前库、抛一条 warning 并让操作成功。

**权限检查分两条路，取决于会话是否激活了角色。** `sctx->get_active_roles()->size() == 0` 时走 `sctx->check_access(DB_OP_ACLS, ...)`，通过则直接取 `DB_OP_ACLS`，否则退回 `acl_get(...)` 与 `sctx->master_access(...)` 按位或；角色非空时走 `sctx->db_acl(new_db_file_name_cstr) | sctx->master_access(new_db_file_name.str)`。

**目标库存在与否由数据字典回答，不是扫目录。** `mysql_change_db` 用 `dd::Schema_MDL_locker mdl_handler(thd)` 拿元数据锁，用 `thd->dd_client()->acquire(new_db_file_name.str, &schema)` 取 `dd::Schema` 对象。`schema == nullptr` 就是「库不存在」。

**排序规则来自数据字典里该 schema 的默认排序规则。** `get_default_db_collation(*schema, &db_default_cl)` 调 `get_charset(schema.default_collation_id(), MYF(0))`，取不到就报 `ER_UNKNOWN_COLLATION`。取到的排序规则若为空指针，则回退到 `thd->collation()`。

**当前库的元数据缓存会随切换一起失效。** 这是 boundary 里「随之而来的表缓存与元数据失效」那半句的落点：`mysql_change_db_impl` 换库名时在 `thd->LOCK_thd_data` 保护下先 `my_free` 掉旧库名再 `thd->reset_db(new_db_name)`，并在 `DEBUG_SYNC(thd, "after_freeing_thd_db")` 这个同步点上留了调试钩子。

## 怎么实现的

### 入口与调用方

`mysql_change_db` 在本地检出的源码里被五处调用，覆盖了三类完全不同的语义：

- **客户端协议入口。** `sql/sql_parse.cc` 的 `COM_INIT_DB` 分支：`thd->status_var.com_stat[SQLCOM_CHANGE_DB]++`，先 `thd->convert_string(&tmp, system_charset_info, com_data->com_init_db.db_name, com_data->com_init_db.length, thd->charset())` 把客户端传来的库名从系统字符集转到会话字符集，再 `mysql_change_db(thd, tmp_cstr, false)`；成功后 `query_logger.general_log_write(thd, command, thd->db().str, thd->db().length)` 写通用日志并 `my_ok(thd)`。
- **SQL 语句入口。** `sql/sql_parse.cc` 的 `SQLCOM_CHANGE_DB` 分支（即 `USE db` 语句）：`const LEX_CSTRING db_str = {query_block->db, strlen(query_block->db)}`，然后 `mysql_change_db(thd, db_str, false)`，成功则 `my_ok(thd)`。这里 `force_switch` 是假，与 `COM_INIT_DB` 一致。
- **登录时选择初始库。** `sql/auth/sql_authentication.cc` 在握手完成后：`sctx->cache_current_db_access(0)` 先把默认库访问权清零（注释说明此时还没有激活的数据库），随后 `if (mpvio.db.length) { if (mysql_change_db(thd, to_lex_cstring(mpvio.db), false)) { ... release_user_connection(thd); ... inc_host_errors(mpvio.ip, &errors); login_failed_error(...); goto end; } }`——**登录时切库失败会导致整个登录失败**，错误信息由 `mysql_change_db()` 自己压入。
- **存储过程与预处理语句的临时切库。** `sql/sp.cc`、`sql/sp_head.cc`、`sql/sql_prepare.cc` 都通过 `mysql_opt_change_db`（见下）在执行存储例程或预处理语句前切到例程/语句自己的库，执行完再切回。这些调用点传的 `force_switch` 常为真。

### `mysql_change_db` 的分支顺序

函数体从头到尾的判定顺序是固定的：

1. **空库名。** `if (new_db_name.str == nullptr || new_db_name.length == 0)`。`force_switch` 为真时调 `mysql_change_db_impl(thd, NULL_CSTR, 0, thd->variables.collation_server)` 并 `goto done`；为假时 `my_error(ER_NO_DB_ERROR, MYF(0))` 并返回失败。源码注释把这条路径限定为「切换回加载存储程序之前的库」，并留了一条 TODO 说 `new_db_name` 似乎总是非 NULL，存储程序的情形是 `new_db_name->str == ""` 且长度为 0。
2. **INFORMATION_SCHEMA 特判。** `if (is_infoschema_db(new_db_name.str, new_db_name.length))` 则 `mysql_change_db_impl(thd, INFORMATION_SCHEMA_NAME, SELECT_ACL, system_charset_info)` 并 `goto done`。注意这里**完全绕过了权限检查与数据字典查询**，权限被硬编码为 `SELECT_ACL`，排序规则被硬编码为 `system_charset_info`。
3. **库名合法性。** 先 `my_strndup` 复制出 `new_db_file_name`，再 `check_and_convert_db_name(&new_db_file_name, false)`，返回值不是 `Ident_name_check::OK` 就 `my_free` 掉副本；`force_switch` 为真时额外调 `mysql_change_db_impl(thd, NULL_CSTR, 0, thd->variables.collation_server)` 清空当前库，随后**无论 `force_switch` 与否都返回失败**。函数上方的注释明确写了「if check_db_name() fails, we should throw an error in any case, even if we are called from sp_head::execute()」。
4. **计算 `db_access`。** 按「有无激活角色」二选一（见上）。
5. **权限拒绝。** `if (!force_switch && !(db_access & DB_OP_ACLS) && check_grant_db(thd, new_db_file_name.str, true))` 则 `my_error(ER_DBACCESS_DENIED_ERROR, MYF(0), sctx->priv_user().str, sctx->priv_host().str, new_db_file_name.str)`，并额外调 `query_logger.general_log_print(thd, COM_INIT_DB, ER_DEFAULT(ER_DBACCESS_DENIED_ERROR), ...)` 写一条通用日志。注意 `force_switch` 为真时**整条权限判断被短路**。
6. **元数据锁与字典查询。** `if (mdl_handler.ensure_locked(new_db_file_name.str) || thd->dd_client()->acquire(new_db_file_name.str, &schema))` 失败就释放副本并返回失败。
7. **库不存在。** `schema == nullptr` 时：`force_switch` 为真则 `push_warning_printf(thd, Sql_condition::SL_NOTE, ER_BAD_DB_ERROR, ER_THD(thd, ER_BAD_DB_ERROR), new_db_file_name.str)`、清空当前库、`goto done`（操作成功）；为假则 `my_error(ER_BAD_DB_ERROR, MYF(0), new_db_file_name.str)` 并返回失败。
8. **取默认排序规则。** `get_default_db_collation(*schema, &db_default_cl)` 失败则断言 `thd->is_error() || thd->killed` 后返回失败。
9. **落地。** `db_default_cl = db_default_cl ? db_default_cl : thd->collation();` 之后 `mysql_change_db_impl(thd, new_db_file_name_cstr, db_access, db_default_cl)`。

### `mysql_change_db_impl`：真正的落地动作

`mysql_change_db_impl` 接受四参数：`new_db_name`（`NULL_CSTR` 表示清空）、`new_db_access`、`new_db_charset`。它按库名分三种写法：

- **`new_db_name.str == nullptr`**（清空）：`thd->set_db(NULL_CSTR)`。注释说 `THD::set_db()` 把所有活干完——释放旧库名并设置新库名。
- **目标是 INFORMATION_SCHEMA**：`thd->set_db(INFORMATION_SCHEMA_NAME)`。注释说明这里必须用 `set_db()`，因为要把 `INFORMATION_SCHEMA_NAME` 这个常量**复制**一份。
- **普通库名**：先 `mysql_mutex_lock(&thd->LOCK_thd_data)`，若 `thd->db().str` 非空则 `my_free` 掉旧库名（注释说明 `THD::reset_db()` 不释放旧库名，所以要显式释放），再 `thd->reset_db(new_db_name)`，最后解锁。锁的必要性在 `sql/sql_class.cc` 的 `THD::set_db` 里写明了：要么释放旧内存并重新分配、要么 memcpy 新库名，两种都会改 `m_db`。

### `mysql_opt_change_db`：带「是否真的变了」判定的包装

`mysql_opt_change_db(thd, new_db_name, saved_db_name, force_switch, cur_db_changed)` 是存储程序与预处理语句用的包装层。它先 `*cur_db_changed = !cmp_db_names(thd->db().str, new_db_name.str)`，若没变就直接 `return false`（不做任何切换）；变了才 `backup_current_db_name(thd, saved_db_name)` 把旧库名拷进调用方给的缓冲，然后调 `mysql_change_db(thd, new_db_name, force_switch)`。

**库名比较是「忽略大小写」的。** `cmp_db_names` 的判据是：两个都为空则相等；两个都非空且 `my_strcasecmp(system_charset_info, db1_name, db2_name) == 0` 则相等。函数注释写明比较用的是系统字符集 UTF8 下的规则。

**旧库名的备份有长度前提。** `backup_current_db_name` 在当前库为空时把 `saved_db_name->str` 置空、长度置 0；否则 `strmake(saved_db_name->str, thd->db().str, saved_db_name->length - 1)` 并把长度更新为 `thd->db().length`。这里 `strmake` 的第三个参数是 `saved_db_name->length - 1`，即调用方提供的缓冲必须放得下旧库名加结尾 NUL。

### 存储程序与预处理语句的「切过去—切回来」

这套配对在三个文件里以相同形状出现：

- `sql/sp.cc` 在编译存储程序前 `mysql_opt_change_db(thd, {sp_db, sp_db_len}, &saved_cur_db_name, true, &cur_db_changed)`（`force_switch` 为真，源码旁留着 `TODO: why do we force switch here?`），编译完成后 `if (cur_db_changed && mysql_change_db(thd, to_lex_cstring(saved_cur_db_name), true))` 切回；切回失败则 `sp_head::destroy(*sphp)` 销毁刚编译出的例程并返回 `SP_INTERNAL_ERROR`。切回用的 `force_switch` 为真的理由写在注释里：旧库**可能为 NULL**，此时 `mysql_change_db()` 会报错。
- `sql/sp_head.cc` 在执行例程体前用 `m_db.length` 作前提调 `mysql_opt_change_db(thd, to_lex_cstring(m_db), &saved_cur_db_name, false, &cur_db_changed)`（此处 `force_switch` 为假）；例程执行收尾时 `if (cur_db_changed && thd->killed != THD::KILL_CONNECTION) err_status |= mysql_change_db(thd, to_lex_cstring(saved_cur_db_name), true);`——切回强制进行。
- `sql/sql_prepare.cc` 的 `Prepared_statement::execute()` 用 `mysql_opt_change_db(thd, m_db, &saved_cur_db_name, true, &cur_db_changed)`，注释解释强制切换的理由是「预处理语句的库可能是 NULL（可以在没有选中当前库的情况下创建预处理语句）」；重整路径 `Prepared_statement::reprepare()` 用 `mysql_opt_change_db(thd, copy.m_db, &saved_cur_db_name, true, &cur_db_changed)` 临时切库，重整完成后 `if (cur_db_changed) mysql_change_db(thd, to_lex_cstring(saved_cur_db_name), true);` 切回。

### 删库时对当前库的联动处理

`mysql_rm_db` 里有这样一段联动：当 `thd->db().str && !strcmp(thd->db().str, db.str)` 时（即被删的正是当前库），调 `mysql_change_db_impl(thd, NULL_CSTR, 0, thd->variables.collation_server)` 把当前库静默清空。源码注释写明这样做的目的是「让将来的 `SELECT DATABASE()` 返回空」。清空之后只通知 `CURRENT_SCHEMA_TRACKER` 的 `mark_as_changed`，没有通知 `SESSION_STATE_CHANGE_TRACKER`。紧接着 `thd->server_status |= SERVER_STATUS_DB_DROPPED`。

### 会话状态跟踪的收尾

`mysql_change_db` 末尾的 `done:` 标签处做两件事，都是「若对应的 tracker 已启用就标记为已变更」：

```cpp
if (thd->session_tracker.get_tracker(CURRENT_SCHEMA_TRACKER)->is_enabled()) {
  thd->session_tracker.get_tracker(CURRENT_SCHEMA_TRACKER)->mark_as_changed(thd, {});
}
if (thd->session_tracker.get_tracker(SESSION_STATE_CHANGE_TRACKER)->is_enabled())
  thd->session_tracker.get_tracker(SESSION_STATE_CHANGE_TRACKER)->mark_as_changed(thd, {});
```

这两处是 `sql/sql_db.cc` 中唯一切换路径一定会到达的收尾——无论走的是哪条分支（空库名、INFORMATION_SCHEMA、普通库、库不存在时的强制降级），最终都会 `goto done` 或自然流到这里。

## 关键取舍

**库名复制到 THD 自有内存，而不是就地改写调用方的缓冲。** 代价是每次切换都付一次 `my_strndup` 加一次 `my_free`；收益是库名的生命周期与 THD 一致，任何模块（连接处理器、存储程序、复制线程）都能安全地持有它。注释把这条历史动机写得很直接：不同模块各持一份不同内存的库名曾经造成过内存破坏。

**`force_switch` 把「校验失败」降级为「清空当前库」。** 这是同一段代码同时服务两类语义需求的结果：面向客户端的 `USE`/`COM_INIT_DB` 需要严格失败，而存储程序与预处理语句在恢复旧库时可能面对一个已被删掉的库或一个 NULL 的库名，降级让「恢复」不至于把整个语句打断。代价是 `force_switch` 为真时权限检查被整条短路（`!force_switch &&` 出现在条件里），函数注释里 `TODO: is this really the intention?` 的疑问正指向这一处。

**INFORMATION_SCHEMA 走硬编码旁路。** `mysql_change_db_impl(thd, INFORMATION_SCHEMA_NAME, SELECT_ACL, system_charset_info)` 不查数据字典、不做权限检查、不取默认排序规则。好处是切换到信息模式永远成功且开销固定；代价是这条路径的行为与普通库路径不一致。

**权限检查在 `force_switch` 为真时被完全跳过。** `db_access` 仍然会被计算并写进安全上下文（`mysql_change_db_impl` 第 2 步的 `cache_current_db_access(new_db_access)`），但没有权限时的拒绝分支不会触发。

**库不存在时的错误是「可降级」的 warning。** `force_switch` 为真时用 `push_warning_printf(thd, Sql_condition::SL_NOTE, ER_BAD_DB_ERROR, ...)` 抛 warning（注意级别是 `SL_NOTE`），同时把当前库清空并让整个操作返回成功。

**当前库的比较忽略大小写，但删库时的比较是区分大小写的。** `cmp_db_names` 用 `my_strcasecmp(system_charset_info, ...)`，而 `mysql_rm_db` 里判断「被删库是否就是当前库」用的是 `strcmp`。两处的严格程度不一致。

**切换的元数据失效靠「先释放旧库名再设置新库名」实现，没有独立的缓存失效调用。** 锁（`thd->LOCK_thd_data`）保护的是库名指针本身的换手。

**架构推论：** `mysql_opt_change_db` 的「没变就不切」短路（`*cur_db_changed` 为假直接返回）与「切完必须切回」的配对，在存储程序与预处理语句两条路径上被独立地重复实现了三次（`sql/sp.cc`、`sql/sp_head.cc`、`sql/sql_prepare.cc`），没有抽出公共的「作用域式切库」辅助类型。这是从三处调用点形状相同、代码彼此独立推出的，不是在源码里直接陈述的结论。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_db.cc` | `mysql_change_db` | 切库主入口：校验库名、算权限、查数据字典、取默认排序规则、落地并通知 tracker |
| `sql/sql_db.cc` | `mysql_change_db_impl` | 实际落地四件事：改 THD 库名、更新安全上下文的库级权限、更新 `db_charset` 与 `collation_database` |
| `sql/sql_db.cc` | `mysql_opt_change_db` | 带「是否真的变了」判定的包装：先比库名，变了才备份旧名并切 |
| `sql/sql_db.cc` | `cmp_db_names` | 库名比较：两个都空或忽略大小写相等（按 `system_charset_info` 比较） |
| `sql/sql_db.cc` | `backup_current_db_name` | 把当前库名拷进调用方缓冲；当前库为空时把指针置空、长度置 0 |
| `sql/sql_db.cc` | `get_default_db_collation` | 从 `dd::Schema` 的 `default_collation_id()` 取排序规则，取不到报 `ER_UNKNOWN_COLLATION` |
| `sql/sql_db.cc` | `mysql_rm_db` | 删库时若被删库正是当前库，静默把当前库清空并置 `SERVER_STATUS_DB_DROPPED` |
| `sql/sql_parse.cc` | `mysql_change_db`（`COM_INIT_DB` 分支） | 客户端 `COM_INIT_DB` 入口：转换字符集后切库，成功写通用日志并 `my_ok` |
| `sql/sql_parse.cc` | `mysql_change_db`（`SQLCOM_CHANGE_DB` 分支） | `USE db` 语句入口：以 `force_switch` 为假切库 |
| `sql/auth/sql_authentication.cc` | `mysql_change_db` | 登录握手后选择初始库；切库失败则释放连接、累加 host 错误并按登录失败处理 |
| `sql/sp.cc` | `mysql_opt_change_db` | 编译存储程序前强制切到例程自己的库，编译后强制切回 |
| `sql/sp_head.cc` | `mysql_opt_change_db` / `mysql_change_db` | 执行例程体前切库、收尾时强制切回（旧库可能为 NULL） |
| `sql/sql_prepare.cc` | `mysql_opt_change_db` / `mysql_change_db` | 预处理语句执行前与重整时的临时切库，以及执行后的切回 |
| `sql/sql_class.cc` | `THD::set_db` | 在 `LOCK_thd_data` 保护下释放/复用库名缓冲并写入新库名 |

## 与主核 / AI 的关系

主核（`engine/db`）的会话上下文里，「当前库/当前 schema」是一个需要在多执行入口之间来回搬运的状态：查询层、存储过程层、迁移工具层都可能在执行中临时切换它。主核目前没有 MySQL 这套「切过去—必须切回」的配对设施。

**架构推论：** `mysql_opt_change_db` 的形状（一个 `cur_db_changed` 出参 + 一个 `saved_db_name` 备份缓冲 + `force_switch` 语义位）是可直接搬到主核的。主核若要支持「语句级 schema 覆盖」，照这个形状做一层作用域封装即可：进入作用域时比一次、变了才备份并切、退出作用域时按同一个 `cur_db_changed` 决定要不要切回。MySQL 这边把这个模式在三处独立重复实现，说明它确实是反复出现的需求形状，而不是某一处的偶然写法。

**架构推论：** MySQL 这里的 `force_switch`（容错切库，允许清空当前库并继续）与主核需要的「切库失败即整个工作单元失败」是不同语义。主核若照搬，必须把 `force_switch` 收敛掉——主核的工作单元没有「库可能已被删掉」这种外部竞态窗口可容忍，否则工作单元的执行结果会建立在不完整的上下文上。

对 AI 场景而言，「切库」与「表缓存失效」耦合在一起这件事值得留意。AI 代理生成的多步工具调用经常跨库操作（先查 A 库的元数据，再写 B 库），而 MySQL 把这两件事绑定在同一次 `mysql_change_db` 里，说明在有存储引擎与表缓存的实现中，「换库」不是一次廉价的指针赋值而是一次带元数据副作用的状态变更。代理侧的启示是：切换上下文这件事必须由一个统一的入口收口，而不是各处自行改写会话变量——否则元数据失效就会被绕过。

## 参考

- [MySQL 源码：`sql/sql_db.cc`（tag mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_db.cc)
- [MySQL 源码：`sql/sql_db.cc` 原始文件（tag mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/sql/sql_db.cc)
- [MySQL 源码：`sql/sql_parse.cc`（tag mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_parse.cc)
- [MySQL 源码：`sql/sql_class.cc`（tag mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_class.cc)
- [MySQL 源码：`sql/sp_head.cc`（tag mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sp_head.cc)
- [MySQL 源码：`sql/sql_prepare.cc`（tag mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_prepare.cc)
- [MySQL 参考手册：The USE Statement](https://dev.mysql.com/doc/refman/8.0/en/use.html)
- [MySQL Server 开发文档：Security_context 类](https://dev.mysql.com/doc/dev/mysql-server/latest/classSecurity__context.html)
