---
project: mysql-server
feature: role-model
page_type: feature
schema_version: 1
refs:
  - path: sql/auth/roles.cc
    symbols: [Role_activation::activate, Role_activation::activate_role_all]
  - path: sql/auth/sql_authorization.cc
    symbols: [mysql_grant_role, mysql_revoke_role]
---

# 角色模型

## 一句话职责

角色模型负责把「角色」这个授权 ID（authid）从纯粹的命名实体变成可展开的权限载体：它维护角色授予关系的有向图（谁被授予了哪些角色）、把角色到权限的展开落到会话安全上下文的活动角色列表上，并在授予/撤销时同步维护 `mysql.roles_edges` 与 `mysql.default_roles` 两张授权表。

## 机制是什么

MySQL 的角色模型由三层构成，三层各自独立落盘与独立生效：

1. **授予关系层（持久）。** 角色授予是一条「被授予者 → 授予者」的有向边。`mysql_grant_role()` 负责写入，`mysql_revoke_role()` 负责删除，两者在写库之前先改内存图（`grant_role()` / `revoke_role()`），让会话立刻看到变化。
2. **默认角色层（持久）。** 每个用户可以指定一组「默认角色」，连接建立时自动激活。默认角色存在 `mysql.default_roles` 表里，由 `mysql_alter_or_clear_default_roles()` 维护。
3. **活动角色层（会话态）。** 会话当前生效的角色集合存在 `Security_context::m_active_roles` 里，由 `Roles::Role_activation::activate()` 及其分支写入。角色只有在「被激活」之后，它携带的权限才会通过 `checkout_access_maps()` 合进会话的 master access。

角色的「身份判定」不是靠一张角色表，而是靠**入度**：一个 authid 只要在授予图里还有别人向它授予角色，它就是角色；入度归零后它退回普通账号。`update_role_flag_of_acl_user()` 用 `boost::in_degree(role_vert, *g_granted_roles)` 计算入度，并把结果写进 `ACL_USER::is_role`；`grant_role()` 则直接置 `role->is_role = true`。`is_role_id()` 读的就是这个标志位。

**边界（与相邻机制的分界）：**

- 不负责权限判定本身——「这次操作是否被授权」是 `sql/auth/sql_authorization.cc` 的 `check_access`（见 `n02-privilege-check` 页）。角色模型只负责把角色**展开**成权限位并挂到会话上，判定发生在展开之后。
- 不负责认证——「这个口令对不对」是 `sql/auth/sql_authentication.cc` 的 `acl_authenticate`（见 `n01-authentication` 页）。角色不参与口令校验。
- 不负责连接建立——见 `a01-connection-dispatch` 页。

## 怎么实现的

### 授予：`mysql_grant_role()`

`sql/auth/sql_authorization.cc` 的 `mysql_grant_role(THD *thd, const List<LEX_USER> *users, const List<LEX_USER> *roles, bool with_admin_opt)` 是 `GRANT role TO user` 的落点。执行顺序：

1. **binlog 格式强制降级为 statement。** 函数开头构造 `Save_and_Restore_binlog_format_state binlog_format_state(thd)`，源码注释写明：即使使用 row-based replication，这条语句也按 statement 复制，函数退出时恢复原值。
2. **打开授权表。** `open_grant_tables(thd, tables, &transactional_tables)`，失败时 `return ret != 1`。
3. **取写锁。** `Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::WRITE_MODE)`；`acl_cache_lock.lock()` 失败则 `commit_and_close_mysql_tables(thd)` 后返回 true。
4. **SYSTEM_USER 闸门。** `check_system_user_privilege(thd, *users)` 返回 true（目标用户带 SYSTEM_USER 权限而当前用户没有）时直接中止。该函数在同文件末尾定义，逐个 `get_current_user()` 解析列表项，再 `sctx->can_operate_with({user}, consts::system_user)` 比对。
5. **取边表。** `table = tables[6].table;`——这一行用的是**字面量下标 6**，而不是 `ACL_TABLES::TABLE_ROLE_EDGES` 枚举名；同文件的 `mysql_revoke_role()` 用的是 `tables[ACL_TABLES::TABLE_ROLE_EDGES].table`。两者指向同一张角色边表。
6. **逐用户逐角色循环。** 每个用户先处理 `CURRENT_USER()`（`lex_user->user.str == nullptr` 时调 `get_current_user(thd, lex_user)`），再判匿名用户：`lex_user->user.length == 0 || *(lex_user->user.str) == '\0'` 时报 `ER_CANNOT_GRANT_ROLES_TO_ANONYMOUS_USER`——**角色不能授予匿名用户**。接着 `find_acl_user(...)` 找不到用户报 `ER_UNKNOWN_AUTHID`。
7. **角色自身的校验。** 角色名长度为 0 时报 `ER_FAILED_ROLE_GRANT`；角色不存在报 `ER_UNKNOWN_AUTHID`。源码注释写明这一处的匿名检查「keep the check in sync with `check_authorization_id_string`」。
8. **SYSTEM_USER 交叉校验。** `sctx->can_operate_with({role}, consts::system_user)` 为 true 时中止（不允许把带 SYSTEM_USER 权限的角色授予他人）。
9. **自环与环路检测。** `acl_user == acl_role` 或 `check_if_granted_role_recursive(role->user, role->host, lex_user->user, lex_user->host)` 为真时报 `ER_ROLE_GRANTED_TO_ITSELF`。`check_if_granted_role_recursive()` 用 Boost 的 `breadth_first_search` 在 `g_granted_roles` 图上从 `start` 顶点做 BFS，命中 `m_needle` 顶点即返回 true；它还会额外遍历 `get_mandatory_roles()` 返回的强制角色。
10. **改内存图 + 落盘。** 先 `grant_role(acl_role, acl_user, with_admin_opt)` 改内存图，再 `modify_role_edges_in_table(thd, table, from_user, to_user, with_admin_opt, false)` 写表。`grant_role()` 在图里 `add_edge(user_vert, role_vert, *g_granted_roles)`，并把 `with_admin_opt` 编码成边容量：`boost::put(edge_colors, edge, (with_admin_opt ? 1 : 0))`，最后置 `role->is_role = true`。注意加边的方向是 **user→role**。
11. **提交。** `log_and_commit_acl_ddl(thd, transactional_tables)` 写 binlog 并提交事务，随后 `get_global_acl_cache()->increase_version()` 让全局 ACL 缓存版本递增，使缓存失效。
12. **收尾。** 无错则 `my_ok(thd)`，再 `acl_notify_htons(thd, SQLCOM_GRANT, users)` 通知存储引擎。

### 撤销：`mysql_revoke_role()`

`mysql_revoke_role(THD *thd, const List<LEX_USER> *users, const List<LEX_USER> *roles)` 的骨架与授予几乎一致（同样的 binlog 格式降级、`open_grant_tables`、WRITE_MODE 锁、`check_system_user_privilege`、`log_and_commit_acl_ddl`、`increase_version`），差异在错误分支：

- **强制角色不可撤销。** 撤销前先 `get_mandatory_roles(&mandatory_roles)`，若待撤销角色在强制角色列表里：带 `IF EXISTS` 时 `push_warning_printf(thd, Sql_condition::SL_WARNING, ER_MANDATORY_ROLE, ...)`，否则 `my_error(ER_MANDATORY_ROLE, MYF(0), out.c_str())` 并中止。
- **用户不存在。** `find_acl_user(...)` 返回 nullptr 时走 `report_missing_user_grant_message(thd, false, ..., ER_UNKNOWN_AUTHID)`，**不中断整个循环**（`continue`），这与 `mysql_grant_role()` 直接 `my_error(ER_UNKNOWN_AUTHID)` 并 break 的处理不同。
- **角色不存在。** 同样按 `thd->lex->grant_if_exists` 分流：带则 `push_warning_printf(..., ER_UNKNOWN_AUTHID, ...)` 后 `continue`，否则 `my_error(ER_UNKNOWN_AUTHID, MYF(0), ...)` 并 `break`。
- **成功路径。** `modify_role_edges_in_table(thd, table, from_user, to_user, false, true)` 删边（第五个参数 `delete_option` 传 true），失败报 `ER_FAILED_REVOKE_ROLE`；成功则 `revoke_role(thd, acl_role, acl_user)` 改内存图（内部 `boost::remove_edge(*user_vert, *role_vert, *g_granted_roles)`，再由 `update_role_flag_of_acl_user()` 按入度重算 `is_role`）；最后 `drop_default_role_policy(thd, tables[ACL_TABLES::TABLE_DEFAULT_ROLES].table, create_authid_from(acl_role), create_authid_from(acl_user))` 把该角色从被撤销用户的默认角色集合里摘掉——**撤销角色会连带清掉指向它的默认角色条目**。

### 边的落盘：`modify_role_edges_in_table()`

实现在 `sql/auth/role_tables.cc`。字段下标由文件顶部的宏固定：`MYSQL_ROLE_EDGES_FIELD_FROM_HOST` 为 0、`FROM_USER` 为 1、`TO_HOST` 为 2、`TO_USER` 为 3、`TO_WITH_ADMIN_OPT` 为 4。写入逻辑：

- 先 `Acl_table_intact table_intact(thd); table_intact.check(table, ACL_TABLES::TABLE_ROLE_EDGES)` 校验授权表未被篡改，失败返回 true。
- 用 `table->file->ha_index_read_idx_map(..., HA_WHOLE_KEY, HA_READ_KEY_EXACT)` 按主键精确读。
- **删除分支**（`delete_option` 为 true）：读到行则 `ha_delete_row`；返回 `HA_ERR_KEY_NOT_FOUND` 时**直接 `return false`**——注释写明「the record is already gone and all is well」，撤销一条不存在的边不算错误。
- **插入分支**：`HA_ERR_KEY_NOT_FOUND` 时 `ha_write_row` 插入新边。
- **升级分支**：`ret == 0 && with_admin_option_char == 'Y'` 时，把旧行 `store_record(table, record[1])` 备份后再次写入 `'Y'`，再用 `compare_records(table)` 判断是否已是 WITH ADMIN——已是则不更新（NOP），不是则 `ha_update_row(table->record[1], table->record[0])`。
- `WITH ADMIN OPTION` 在表里的存储形态是单字符 `'Y'` / `'N'`。

### 会话侧激活：`Role_activation::activate()`

`sql/auth/roles.cc` 的 `Roles::Role_activation::activate()` 是一个纯粹的分派器，按 `m_type`（`role_enum`）分四路：

| `m_type` | 分支函数 | 语义 |
|---|---|---|
| `ROLE_DEFAULT` | `activate_role_default()` | 激活该用户的默认角色 |
| `ROLE_ALL` | `activate_role_all()` | 激活全部已授予角色（可排除若干） |
| `ROLE_NAME` | `activate_role_name()` | 激活 `m_role_list` 里点名的角色 |
| `ROLE_NONE` / 其它 | `activate_role_none()` | 停用全部角色 |

构造函数 `Role_activation::Role_activation(THD *thd, Security_context *sctx, role_enum type, const List<LEX_USER> *role_list, bool raise_error)` 有一处前置校验：`!m_thd || (m_type == role_enum::ROLE_NAME && m_role_list == nullptr)` 时把 `m_valid` 置 false——**ROLE_NAME 必须带角色列表**。

`activate_role_all()` 的展开过程：

1. 取 `Acl_cache_lock_guard acl_cache_lock(m_thd, Acl_cache_lock_mode::READ_MODE)` 读锁，`acl_cache_lock.lock(m_raise_error)` 失败返回 true。
2. 把当前活动角色列表浅拷贝到 `backup_active_list`，再 `m_sctx->get_active_roles()->clear()` 与 `m_sctx->clear_active_roles()` 清空。
3. `LEX_USER *current_user = create_default_definer(m_thd);` 取当前授权 ID，`create_authid_str_from(current_user)` 得到 authid 字符串。
4. 在 `g_authid_to_vertex` 里查该 authid 对应的顶点；**查不到就整个跳过**（图里没这个点，说明它没有角色边）。
5. `get_granted_roles(user_vertex, &granted_roles)` 取出全部出边角色。`get_granted_roles()` 遍历 `boost::out_edges(v, *g_granted_roles)`，逐个取目标顶点的 `ACL_USER`，并从 `edge_capacity_t` 读出 `with_admin_opt != 0` 一起返回。
6. **强制角色合并进来**：`get_mandatory_roles(&mandatory_roles)` 的结果被 `granted_roles.push_back(std::make_pair(rid, false))` 追加到同一列表——即强制角色与普通已授予角色在展开阶段被同等对待，`with_admin_opt` 一律填 false。
7. **排除列表。** 若 `m_role_list && m_role_list->elements > 0`，逐个比较：用户部分用 `strncmp` 精确比较长度与内容，host 部分用 `native_strncasecmp`（**host 大小写不敏感、user 大小写敏感**）；命中则该角色跳过。
8. 逐个 `m_sctx->activate_role(...)`，失败且 `m_raise_error` 时报 `ER_ROLE_NOT_GRANTED` 并 `break`。
9. 成功则 `m_sctx->checkout_access_maps()`、`m_sctx->db_acl(m_thd->db())`、`m_sctx->cache_current_db_access(new_db_access)`，然后释放备份内存；失败则 `m_sctx->clear_active_roles()` 并把备份浅拷贝回活动列表——**激活失败会整体回滚到激活前的角色集合**。

`activate_role_default()` 与 `activate_role_name()` 都有同样的「备份 → 清空 → 逐个激活 → 成功则提交、失败则回滚」骨架。`activate_role_default()` 通过 `get_default_definer(m_thd, &current_user)` 取当前用户（`get_default_definer()` 定义在 `sql/sql_parse.cc`），再 `get_default_roles(current_user_authid, authids)` 从 `g_default_roles` 里 `equal_range` 出默认角色集合。

`activate_role_none()` 走的是另一条路：它不做备份/回滚那套，而是 `m_sctx->clear_active_roles()` + `m_sctx->clear_db_restrictions()` + `m_sctx->checkout_access_maps()`，然后取 `ACL_USER *user = find_acl_user(m_sctx->priv_host().str, m_sctx->priv_user().str, true)`，用 `m_sctx->set_master_access(user->access, acl_restrictions->find_restrictions(user))` 把权限**还原成用户自己原有的权限**。

### 展开的落点：`Security_context::activate_role()`

`sql/auth/sql_security_ctx.cc` 的 `Security_context::activate_role(LEX_CSTRING role, LEX_CSTRING role_host, bool validate_access)` 是「角色 → 会话权限」的关键一步：

- 先 `std::find` 在 `m_active_roles` 里查重，**已激活的角色被静默忽略**（直接 `return 0`，注释写明「silently ignore requests of activating an already active role」）。
- 角色名与 host 各 `my_strdup` 一份（`PSI_NOT_INSTRUMENTED`），因为活动角色列表持有的内存需要在停用时释放。
- `validate_access` 为真时调 `check_if_granted_role(priv_user(), priv_host(), dup_role, dup_role_host)`；校验不过则释放刚分配的内存并返回 `ER_ACCESS_DENIED_ERROR`。`check_if_granted_role()` 定义在 `sql/auth/sql_authorization.cc`，先在 `g_authid_to_vertex` 里找用户顶点、再用 `find_if_granted_role()` 遍历出边匹配角色；都没匹配上则遍历 `get_mandatory_roles()`，命中强制角色也算已授予。
- 校验通过则 `m_active_roles.push_back(...)`。

真正的权限合并发生在 `Security_context::checkout_access_maps()`：它先归还旧的 `Acl_map`，然后 `if (m_active_roles.size() == 0) return;`——**没有活动角色就直接返回，不重新取 map**；否则 `get_global_acl_cache()->checkout_acl_map(this, uid, m_active_roles)` 取出合并后的 map，并用 `set_master_access(m_acl_map->global_acl(), m_acl_map->restrictions())` 覆盖会话权限。取不到 map 时 `set_master_access(0)`，即权限归零。

### 强制角色：`mandatory_roles`

`get_mandatory_roles(std::vector<Role_id> *mandatory_roles)` 从系统变量 `opt_mandatory_roles` 读取逗号分隔的授权 ID 列表：

- 用 `mysql_mutex_lock(&LOCK_mandatory_roles)` 保护，`opt_mandatory_roles_cache` 为真时直接拷贝缓存 `g_mandatory_roles`。
- 缓存未建时解析 `opt_mandatory_roles.str`，用 `iterate_comma_separated_quoted_string` 逐项 `get_authid_from_quoted_string` 拆分；**host 段为空时补成 `"%"`**（`if (el.second == "") el.second = "%";`）。
- 用户名为空的项被拒（记 `ER_ANONYMOUS_AUTH_ID_NOT_ALLOWED_IN_MANDATORY_ROLES` 警告）；`find_acl_user()` 找不到的项被拒（记 `ER_UNKNOWN_AUTH_ID_IN_MANDATORY_ROLE` 警告）；能查到的项才进列表，并用 `std::find` 去重。
- 最后 `std::sort(g_mandatory_roles->begin(), g_mandatory_roles->end())` 排序。

### 图与缓存的重建：`populate_roles_caches()`

`sql/auth/role_tables.cc` 的 `populate_roles_caches(THD *thd, Table_ref *tablelst)` 在授权表重载时重建两张内存结构：

- 先 `get_global_acl_cache()->increase_version()` 无条件递增缓存版本，注释说明是为了避免不一致。
- 扫 `mysql.roles_edges`：逐行取 from/to 的 host 与 user 与 `WITH ADMIN OPTION` 标志；**用户名或 host 长度为 0 的行被跳过**（记 `ER_AUTHCACHE_ROLE_EDGES_IGNORED_EMPTY_NAME` 警告）；from/to 任一端 `find_acl_user()` 找不到的行被跳过（记 `ER_AUTHCACHE_ROLE_EDGES_UNKNOWN_AUTHORIZATION_ID` 警告）；合法的行调 `grant_role(acl_role, acl_user, *with_admin_opt == 'Y' ? true : false)` 重建边。
- 扫 `mysql.default_roles`：同样跳过空名与查不到的 authid，合法行 `g_default_roles->emplace(user_id, role_id)`。
- 收尾 `rebuild_vertex_index(thd)` 重建 `g_authid_to_vertex` 索引（因为 `boost::remove_edge` 造成的顶点间隙会让旧的顶点描述符失效），并置 `opt_mandatory_roles_cache = false` 让强制角色下次重新解析。

## 关键取舍

**取舍一：授予先改内存图再落盘。** `mysql_grant_role()` 先调 `grant_role()` 改内存图，再调 `modify_role_edges_in_table()` 写表。好处是同一语句内的后续操作立即看到新角色；代价是写表失败时内存图已经被改过，需要靠 `log_and_commit_acl_ddl()` 失败后的事务回滚与 `increase_version()` 触发的缓存重建来收拾。

**取舍二：撤销先删边再改内存图。** `mysql_revoke_role()` 的顺序相反：先 `modify_role_edges_in_table(..., delete_option=true)` 删表记录，成功后才 `revoke_role()` 改内存图。这样写表失败时内存图保持原状，不会出现「图里没边、表里有边」的不一致。

**取舍三：删不存在的边不算错误。** `modify_role_edges_in_table()` 的删除分支在 `HA_ERR_KEY_NOT_FOUND` 时 `return false`，源码注释明确写「the record is already gone and all is well」。这让 `REVOKE` 变成幂等操作。

**取舍四：激活失败整体回滚，而不是部分生效。** `activate_role_all()` / `activate_role_default()` / `activate_role_name()` 都做「备份 → 清空 → 逐个激活」，任一角色激活失败就 `clear_active_roles()` 并把备份浅拷贝回去。好处是会话不会停留在「半套角色」的状态；代价是需要为每次激活做一份浅拷贝和对应的 `my_free`。

**取舍五：重复激活静默忽略。** `Security_context::activate_role()` 在 `m_active_roles` 里 `std::find` 到已存在就 `return 0`，不报错也不重复分配内存。

**取舍六：host 比较大小写不敏感，user 比较大小写敏感。** `activate_role_all()` 的排除列表比较里，user 段用 `strncmp`，host 段用 `native_strncasecmp`。这与 `find_if_granted_role()` 的写法一致（host 段 `role_host.length == 0` 时视作匹配任意 host）。

**取舍七：角色是「图上的入度」而非独立实体。** MySQL 不维护一张「角色表」——角色就是有入边的授权 ID。`update_role_flag_of_acl_user()` 用 `boost::in_degree` 重算 `ACL_USER::is_role`，入度大于 0 才是角色。好处是不需要为角色单独建表、也不需要在角色被删除时做引用检查；代价是角色身份在内存里是派生状态，重建索引（`rebuild_vertex_index()`）是必要步骤。

**取舍八：角色授予始终按 statement 复制。** `mysql_grant_role()` 与 `mysql_revoke_role()` 开头都构造 `Save_and_Restore_binlog_format_state`，源码注释写明角色语句即使在 row-based replication 下也按 statement 复制。这是角色边在复制拓扑中保持确定性的前提。

**取舍九：mandatory_roles 与普通角色在展开期被合并。** `activate_role_all()` 把 `get_mandatory_roles()` 的结果直接 `push_back` 进 `granted_roles`，且 `with_admin_opt` 一律为 false。强制角色因此和普通角色走完全相同的激活路径，区别只在授予阶段：强制角色不能被 `mysql_revoke_role()` 撤销（报 `ER_MANDATORY_ROLE`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/auth/roles.cc | `Role_activation::activate` | 会话角色激活的分派器，按 `role_enum` 选 `activate_role_none/default/all/name` 四条路之一 |
| sql/auth/roles.cc | `Role_activation::activate_role_all` | 激活全部已授予角色：遍历角色图出边、合并强制角色、应用排除列表、失败整体回滚 |
| sql/auth/roles.cc | `Role_activation::activate_role_default` | 激活当前用户的默认角色，角色集来自 `mysql.default_roles` |
| sql/auth/roles.cc | `Role_activation::activate_role_name` | 激活点名的角色列表，逐个 `Security_context::activate_role` |
| sql/auth/roles.cc | `Role_activation::activate_role_none` | 停用全部角色，把会话权限还原为用户自身权限 |
| sql/auth/roles.cc | `Role_activation::Role_activation` | 构造函数；`ROLE_NAME` 缺角色列表时置 `m_valid` 为 false |
| sql/auth/sql_authorization.cc | `mysql_grant_role` | `GRANT role TO user` 的落点：SYSTEM_USER 闸门、环路检测、改内存图、写 `mysql.roles_edges` |
| sql/auth/sql_authorization.cc | `mysql_revoke_role` | `REVOKE role FROM user` 的落点：强制角色保护、删边、改内存图、清默认角色条目 |
| sql/auth/sql_authorization.cc | `grant_role` | 在 `g_granted_roles` 上 `add_edge(user_vert, role_vert)`，边容量编码 `with_admin_opt`，置 `role->is_role = true` |
| sql/auth/sql_authorization.cc | `revoke_role` | `boost::remove_edge` 删边，并按入度重算 `is_role` |
| sql/auth/sql_authorization.cc | `revoke_role_helper` | 删边前的顶点查找，找不到 user 或 role 返回 true |
| sql/auth/sql_authorization.cc | `update_role_flag_of_acl_user` | 用 `boost::in_degree` 判定一个 authid 是否仍是角色 |
| sql/auth/sql_authorization.cc | `check_if_granted_role_recursive` | Boost BFS 在角色图上查间接授予关系，兼查强制角色 |
| sql/auth/sql_authorization.cc | `check_if_granted_role` | 查直接授予关系，未命中再查强制角色列表 |
| sql/auth/sql_authorization.cc | `find_if_granted_role` | 遍历顶点出边匹配角色名与 host |
| sql/auth/sql_authorization.cc | `get_granted_roles` | 收集顶点的全部出边角色，并带出 `with_admin_opt` |
| sql/auth/sql_authorization.cc | `get_default_roles` | 从 `g_default_roles` 用 `equal_range` 取默认角色集合 |
| sql/auth/sql_authorization.cc | `get_mandatory_roles` | 解析 `mandatory_roles` 系统变量，空 host 补 `%`，去重排序 |
| sql/auth/sql_authorization.cc | `drop_default_role_policy` | 撤销角色时清掉指向它的默认角色条目 |
| sql/auth/sql_authorization.cc | `mysql_alter_or_clear_default_roles` | 设置/清除默认角色，落到 `mysql.default_roles` 表 |
| sql/auth/sql_authorization.cc | `mysql_set_active_role_none` | `SET ROLE NONE` 的入口，构造 `ROLE_NONE` 的 `Role_activation` 并激活 |
| sql/auth/sql_authorization.cc | `mysql_set_role_default` | `SET ROLE DEFAULT` 的入口 |
| sql/auth/sql_authorization.cc | `mysql_set_active_role_all` | `SET ROLE ALL` 的入口，接受排除列表 |
| sql/auth/sql_authorization.cc | `mysql_set_active_role` | `SET ROLE <list>` 的入口 |
| sql/auth/sql_authorization.cc | `check_system_user_privilege` | 目标账号带 SYSTEM_USER 而当前用户没有时否决操作 |
| sql/auth/sql_authorization.cc | `check_authorization_id_string` | 校验 `mandatory_roles` 设置：拒匿名 authid，拒带 SYSTEM_USER 的 authid |
| sql/auth/sql_authorization.cc | `rebuild_vertex_index` | 重建 `g_authid_to_vertex` 索引，修正删边导致的顶点描述符失效 |
| sql/auth/role_tables.cc | `modify_role_edges_in_table` | 角色边表读写：按主键精确读，插入/删除/升级 `WITH ADMIN OPTION` 三条路径 |
| sql/auth/role_tables.cc | `populate_roles_caches` | 从两张表重建角色图与默认角色表，并刷新强制角色缓存标志 |
| sql/auth/role_tables.cc | `modify_default_roles_in_table` | 默认角色表的插入与删除 |
| sql/auth/sql_security_ctx.cc | `Security_context::activate_role` | 校验角色是否已授予后把角色加入活动角色列表 |
| sql/auth/sql_security_ctx.cc | `Security_context::checkout_access_maps` | 用活动角色列表取出合并后的 Acl_map 并覆盖会话权限 |
| sql/auth/sql_security_ctx.cc | `Security_context::clear_active_roles` | 清空活动角色列表并释放每个角色名/host 的 `my_strdup` 内存 |
| sql/auth/sql_security_ctx.cc | `Security_context::can_operate_with` | SYSTEM_USER 权限交叉校验，被 `mysql_grant_role()` 与 `mysql_revoke_role()` 调用 |

## 与主核 / AI 的关系

**架构推论：** 角色模型把「授权 ID → 权限集合」的映射从代码里抽出来放进了数据（`mysql.roles_edges` 表）与运行时图结构（`g_granted_roles`）里，因此新增一种「可授予的权限容器」不需要改判定逻辑——只要能写进角色边表、能被 `grant_role()` 挂到图上，`activate_role_all()` 就会把它展开进会话。这正是本仓主核的多租户与最小权限需求可以借鉴的形状：把「角色」做成**数据 + 图**而不是**代码分支**。

**架构推论：** 主核的多角色模型可以照搬这里的两段式结构——「持久授予关系」与「会话活动集合」分离：授权变更走持久层并递增版本号，会话激活走内存层并做失效重建。本页读到的 `increase_version()` 与 `rebuild_vertex_index()` 正好对应主核里「权限快照缓存 + 失效重建」的常见做法。

**架构推论：** 角色模型里「撤销一条不存在的边不算错误」「重复激活静默忽略」这两条幂等设计，适合被 AI 工具调用时采纳——自动化系统重试 `GRANT`/`REVOKE` 或重放 `SET ROLE` 时不需要先查询状态，避免了 TOCTOU 竞态。

## 参考

- [MySQL 8.0 Reference Manual：Using Roles（角色的定义、`SET DEFAULT ROLE`、强制角色与自动激活）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/roles.html) —— 官方对「角色是一组权限的命名集合」「角色可被授予与撤销」的定位，以及 `mandatory_roles` / `activate_all_roles_on_login` 两个系统变量的存在。
- [MySQL 8.0 Reference Manual：GRANT 语句（`GRANT role TO user WITH ADMIN OPTION` 语法）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/grant.html) —— 角色授予的官方语法，以及 `ROLE_ADMIN` 权限「Enable roles to be granted or revoked, use of WITH ADMIN OPTION」的说明。
- [MySQL 8.0 Reference Manual：SET ROLE 语句](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/set-role.html) —— 会话内切换活动角色的四种形式（`NONE` / `ALL` / `ALL EXCEPT` / 点名列表）。
- [MySQL 8.0 Reference Manual：SET DEFAULT ROLE 语句](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/set-default-role.html) —— 默认角色的设置语法，对应 `mysql.default_roles` 表的持久化语义。
- [MySQL 8.0 Reference Manual：Privileges Provided by the Server（`SYSTEM_USER` / `ROLE_ADMIN` 等静态权限）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/privileges-provided.html) —— `SYSTEM_USER` 被列为「Server administration」级全局静态权限的位置。
- [mysql/mysql-server 源码：sql/auth/roles.cc（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/roles.cc) —— 会话侧角色激活的四个分支实现。
- [mysql/mysql-server 源码：sql/auth/sql_authorization.cc（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authorization.cc) —— 角色授予/撤销、角色图维护与强制角色解析。
- [mysql/mysql-server 源码：sql/auth/role_tables.cc（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/role_tables.cc) —— 角色边表与默认角色表的读写及缓存重建。
