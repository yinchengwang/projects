---
project: mysql-server
feature: privilege-check
page_type: feature
schema_version: 1
refs:
  - path: sql/auth/sql_authorization.cc
    symbols: [check_access]
  - path: sql/auth/sql_security_ctx.cc
    symbols: [Security_context::check_access]
---

# 权限判定

## 一句话职责

在一条语句真正落到数据上之前，按「用户表全局权限 → 库级权限 → 表级权限 → 列级权限 → 例程权限」这条既定顺序逐级判定操作者是否被授权，并把每一级实际生效的权限位回填到 `GRANT_INFO` 里供后续检查复用。

## 机制是什么

权限判定是**分层降级**的：全局权限不满足时才去看库级权限，库级权限不满足时才去看表级权限，表级权限不满足时才去看列级权限。每一层都由一个独立的检查函数负责，且判定函数一律「**返回 true 表示拒绝**」。

`sql/auth/sql_authorization.cc` 里的 `check_access()` 负责前两级（全局 + 库级）。它的返回值语义在函数文档注释里写明：返回 true 表示「访问被全局/库级权限独占地拒绝，除非继续检查列级与表级权限，否则无法通过」。

判定分两条互斥的路径，取决于当前会话**是否激活了角色**：

- **无激活角色**时，走经典的「哈希表 + 缓存」路径。库级权限由 `acl_get()` 从 `db_cache` 哈希里查；表级/列级权限由 `check_grant()` 从 `column_priv_hash` 里查。
- **有激活角色**时，走 `Acl_map` 路径。库级权限由 `Security_context::db_acl()` 从 `m_acl_map->db_acls()` / `m_acl_map->db_wild_acls()` 里查；表级/列级权限由 `Security_context::table_and_column_acls()` 从 `m_acl_map->table_acls()` 里查。

两者的分界点是同一个判断：`sctx->get_active_roles()->size() > 0`。

**边界（与相邻机制的分界）：**

- 不负责认证。「你是谁」由 `sql/auth/sql_authentication.cc` 的 `acl_authenticate()` 判定（见 `n01-authentication` 页）。权限判定只接受已经填好 `priv_user()` / `priv_host()` / `master_access()` 的 `Security_context`。
- 不负责角色的授予与激活。角色的创建、授予、展开在 `sql/auth/roles.cc`（见 `n03-role-model` 页）；本页只消费 `get_active_roles()` 的结果。
- 不负责行级与列级权限在执行期的**施加**（哪些行可见、哪些列可写），那属于执行器与表打开阶段；本页只回答「这个操作者有没有做这件事的权限」。
- 不负责动态权限的判定语义本身。动态权限（`ROLE_ADMIN`、`TABLE_ENCRYPTION_ADMIN` 等）走 `Security_context::has_global_grant()`，其内部实现不在本页两个 refs 文件内。

## 怎么实现的

### `check_access()` 的判定顺序

`check_access(THD *thd, Access_bitmask want_access, const char *db, Access_bitmask *save_priv, GRANT_INTERNAL_INFO *grant_internal_info, bool dont_check_global_grants, bool no_errors)` 在 `sql/auth/sql_authorization.cc` 中定义。它的执行顺序是固定的：

**第一步：初始化输出。**

```c
if (save_priv)
  *save_priv = 0;
else {
  save_priv = &dummy;
  dummy = 0;
}
```

`save_priv` 是「本次判定实际拿到的权限位」的出口。调用方不关心它时，函数内部用一个局部 `dummy` 顶替，保证后面 `*save_priv |= ...` 的写法统一。

**第二步：判定「是否连库都没选」。**

```c
if ((!db || !db[0]) && !thd->db().str && !dont_check_global_grants) {
  if (!no_errors) my_error(ER_NO_DB_ERROR, MYF(0));
  return true;
}
```

注意 `!dont_check_global_grants` 这个条件：`dont_check_global_grants` 为真表示「这次是库级 GRANT 语句，不该要求已选库」，所以该分支被跳过。

**第三步：内部 schema 的特例。**

`db != any_db` 时（`any_db` 是文件里定义的特殊符号 `"*any*"`，注释为 `Special symbol for check_access`），先查内部 schema 的访问规则：

```c
access = get_cached_schema_access(grant_internal_info, db);
if (access) {
  switch (access->check(want_access, save_priv, false)) {
    case ACL_INTERNAL_ACCESS_GRANTED:  return false;
    case ACL_INTERNAL_ACCESS_DENIED:   /* ER_DBACCESS_DENIED_ERROR */ return true;
    case ACL_INTERNAL_ACCESS_CHECK_GRANT:
      want_access &= ~(*save_priv);
      break;
  }
}
```

`get_cached_schema_access()` 用 `grant_internal_info->m_schema_lookup_done` 做一次性缓存，查找走 `ACL_internal_schema_registry::lookup(schema_name)`。`IS_internal_schema_access::check()` 的实现是：先 `want_access &= ~SELECT_ACL;`，再 `if (unlikely(want_access & DB_ACLS)) return ACL_INTERNAL_ACCESS_DENIED;`，然后无条件 `*save_priv |= SELECT_ACL;`，最后 `return want_access ? ACL_INTERNAL_ACCESS_CHECK_GRANT : ACL_INTERNAL_ACCESS_GRANTED;`。也就是说 `information_schema` 只给 `SELECT_ACL`，任何库级权限位一律拒绝。

**第四步：全局权限命中即通过。**

```c
if (sctx->check_access(want_access, db_name)) {
  if (!(sctx->check_access(SELECT_ACL, db_name))) {
    /* 需要把库级权限并进 save_priv */
    ...
    *save_priv |= sctx->master_access(db_name) | db_access;
  } else
    *save_priv |= sctx->master_access(db_name);
  return false;
}
```

这一步的语义在源码注释里写明：即使全局权限已经足够，仍要把「库级权限」也算进来，是因为像 `UPDATE t1 SET a=1 WHERE b>0` 这种语句需要知道当前库的访问权才能处理；而如果全局已有 `SELECT_ACL`，就不必再查库。

**第五步：判定「这个权限根本不可能被授予」。**

```c
if (((want_access & ~sctx->master_access(db_name)) & ~DB_ACLS) ||
    (!db && dont_check_global_grants)) {
  /* ER_ACCESS_DENIED_NO_PASSWORD_ERROR 或 ER_ACCESS_DENIED_ERROR */
  return true;
}
```

`~DB_ACLS` 的含义是：超出库级权限集合的那些位（表级、列级、过程/函数级）不可能在「库级 GRANT」这一层被满足，所以直接拒绝。错误分支里还区分了 `thd->password == 2`（无口令）时用 `ER_ACCESS_DENIED_NO_PASSWORD_ERROR`，否则用 `ER_ACCESS_DENIED_ERROR`，后者的第三个参数是「本次是否使用了口令」（`ER_THD(thd, ER_YES)` / `ER_THD(thd, ER_NO)`）。

**第六步：查库级权限。**

```c
if (db && (!thd->db().str || db_is_pattern || strcmp(db, thd->db().str))) {
  if (sctx->get_active_roles()->size() > 0) {
    db_access = sctx->db_acl({db, strlen(db)}, db_is_pattern);
  } else {
    db_access = acl_get(thd, sctx->host().str, sctx->ip().str,
                        sctx->priv_user().str, db, db_is_pattern);
  }
} else
  db_access = sctx->current_db_access();
```

判定「是不是当前库」的条件有三个，命中任一就走「查某个具体库」的分支：当前没有默认库（`!thd->db().str`）、库名是通配模式（`db_is_pattern`）、或者库名与当前库不同（`strcmp(db, thd->db().str)`）。否则直接用 `sctx->current_db_access()` 拿缓存值。

**第七步：合并并判定。**

```c
db_access = (db_access | sctx->master_access(db_name));
*save_priv |= db_access;

bool need_table_or_column_check =
    (want_access & (TABLE_ACLS | PROC_ACLS | db_access)) == want_access;

if (((db_access & want_access) == want_access) ||
    (!dont_check_global_grants && need_table_or_column_check)) {
  return false;
}
/* ER_DBACCESS_DENIED_ERROR */
return true;
```

`(db_access & want_access) == want_access` 是「请求的权限位被 db_access 全包含」。`need_table_or_column_check` 是「请求的权限位全部落在 `TABLE_ACLS | PROC_ACLS | db_access` 之内」，意思是这些权限**可能**由表级或例程级授权满足，所以放行到下一层。源码注释写明这对应 `check_grant()` 的第 3、4 步。

**`db_is_pattern` 的来源：**

```c
const bool db_is_pattern =
    ((want_access & GRANT_ACL) && dont_check_global_grants);
```

源码注释解释：GRANT 语句在库级授权时库名可能是通配模式，在表级/列级授权时库名不能是模式。`dont_check_global_grants` 就被复用为「这是库级 GRANT」的标志位。

### `Security_context::check_access()` 的全局权限判定

`Security_context::check_access(Access_bitmask want_access, const std::string &db_name, bool match_any)` 在 `sql/auth/sql_security_ctx.cc` 中定义，实现只有三行：

```c
if ((want_access & DB_ACLS) &&
    (is_access_restricted_on_db(want_access, db_name))) {
  return false;
}
return (match_any ? (m_master_access & want_access)
                  : ((m_master_access & want_access) == want_access));
```

三个要点：

1. **库级权限位单独走限制检查。** `want_access & DB_ACLS` 非零时，先问 `is_access_restricted_on_db()`——这是 partial revoke（部分撤销）机制：某个库上的某些权限被单独收回了。这时直接返回 false（拒绝），因为库级权限位不能由全局权限位满足。
2. **`match_any` 决定是「任一命中」还是「全部命中」。** `match_any` 为真时返回 `(m_master_access & want_access)`（位与非零即为真）；为假时要求 `(m_master_access & want_access) == want_access`，即请求的权限位被全局权限位全包含。
3. **比较用的是 `m_master_access` 原值，不是 `master_access()`。** `Security_context::master_access(db_name)` 是另一个函数，它返回 `filter_access(m_master_access, db_name)`——先按库的 restrictions 过滤掉被部分撤销的位。也就是说 `check_access()` 走的是「原值 → 先查限制 → 判否」，而 `master_access()` 走的是「先过滤 → 得结果」，两条路殊途同归。

### 部分撤销：`filter_access()` 与 `is_access_restricted_on_db()`

`Security_context::filter_access(const Access_bitmask access, const std::string &db_name) const` 的实现：

```c
Access_bitmask access_mask = access;
auto &db_restrictions = m_restrictions.db();
if (!db_restrictions.is_empty()) {
  Access_bitmask restrictions_mask;
  if (db_restrictions.find(db_name, restrictions_mask))
    access_mask = (access_mask & restrictions_mask) ^ access;
}
return access_mask;
```

`(access_mask & restrictions_mask) ^ access` 这个写法的效果是：把 `restrictions_mask` 覆盖到的那些位从 `access` 里抠掉。`is_access_restricted_on_db()` 则是它的反向判定：

```c
const Access_bitmask filtered_access = filter_access(want_access, db_name);
return (filtered_access != want_access);
```

过滤结果与原值不同，就说明这个库上确实存在限制。

### 角色路径：`db_acl()` 与 `table_and_column_acls()`

`Security_context::db_acl(LEX_CSTRING db, bool use_pattern_scan) const` 在 `sql/auth/sql_security_ctx.cc` 中定义：

- `m_acl_map == nullptr || db.length == 0` 时返回 0。
- 先在 `m_acl_map->db_acls()` 里按库名精确查找，命中则 `return filter_access(found_acl_it->second, key);`。
- 未命中则遍历 `m_acl_map->db_wild_acls()` 做通配匹配。匹配方式由 `mysqld_partial_revokes()` 决定：为真时用 `my_strcasecmp(system_charset_info, db.str, it->first.c_str()) == 0`；为假时用 `wild_compare(db.str, db.length, it->first.c_str(), it->first.size(), use_pattern_scan) == 0`。命中即 `access |= it->second;` 并 `return filter_access(access, key);`。
- 全部不匹配则返回 0。

`Security_context::table_and_column_acls(LEX_CSTRING db, LEX_CSTRING table)` 用 `db` 与 `table` 拼出 `"db.table"` 形式的 key（经 `append_identifier` 处理标识符引用），在 `m_acl_map->table_acls()` 里查找，命中返回 `Grant_table_aggregate`（含 `table_access` 与 `cols` 两部分），未命中返回默认构造的空 `Grant_table_aggregate()`。

### 无角色路径：`acl_get()` 与库级缓存

`acl_get(THD *thd, const char *host, const char *ip, const char *user, const char *db, bool db_is_pattern)` 在 `sql/auth/sql_auth_cache.cc` 中定义：

- 键长校验：`copy_length = (strlen(ip) + strlen(user) + strlen(db)) + 2;`（末尾加 2 是源码注释说明的：`Added 2 at the end to avoid buffer overflow at strmov()`）。`if (copy_length >= ACL_KEY_LENGTH) return 0;`——超长直接返回 0，即「无权限」。
- 加读锁：`Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE); if (!acl_cache_lock.lock(false)) return db_access;`。
- 键的构造：`my_stpcpy` 把 `ip`、`user`、`db` 用 `'\0'` 分隔拼进 `key` 缓冲。若 `lower_case_table_names` 为真，则对库名做 `my_casedn_str(files_charset_info, tmp_db)` 并把 `db` 指向小写化后的副本。
- 查缓存：`if (!db_is_pattern)` 才查 `db_cache`（`db_cache` 的键是 `std::string(key, key_length)`）。命中直接返回 `it->second->access`。
- 未命中则遍历 `acl_dbs` 链表：`acl_db->user` 为空或与 `user` 相等，且 `acl_db->host.compare_hostname(host, ip)` 通过，再判库名匹配。库名匹配同样是按 `mysqld_partial_revokes()` 二选一：为真时 `!strcmp(db, acl_db->db)`；为假时 `!wild_compare(db, strlen(db), acl_db->db, strlen(acl_db->db), db_is_pattern)`。命中后若 `acl_db->host.get_host()` 非空则 `goto exit`（完全指定的主机，直接采用）；否则 `break`。
- 写回缓存：`exit:` 标签处若 `!db_is_pattern` 则分配 `acl_entry` 写入 `db_cache`。

源码注释明确写了 `db_cache is not used if db_is_pattern is set.`，即通配模式的库名不缓存。

### 表级与列级：`check_grant()` 与 `check_grant_column()`

`check_grant(THD *thd, Access_bitmask want_access, Table_ref *tables, bool any_combination_will_do, uint number, bool no_errors)` 在 `sql/auth/sql_authorization.cc` 中定义。它的函数文档把权限检查队列明确列为五步：

1. Global privileges
2. OR (db privileges AND host privileges)
3. OR table privileges
4. OR column privileges（**此函数不检查**）
5. OR routine privileges（**此函数不检查**）

`check_access()` 负责第 1、2 步，`check_grant()` 负责第 3 步，4、5 步在别处。

`check_grant()` 的循环体里，每张表依次处理：

- **内部表访问规则。** `get_cached_table_access(&t_ref->grant.m_internal, db_name, t_ref->get_table_name())` 取内部表规则，返回 `ACL_INTERNAL_ACCESS_GRANTED` 时置 `t_ref->grant.privilege |= TMP_TABLE_ACLS;` 并 `continue`（跳过列检查）；返回 `ACL_INTERNAL_ACCESS_DENIED` 时 `goto err`。
- **减掉全局权限。** `want_access = orig_want_access; want_access &= ~sctx->master_access(db_name); if (!want_access) continue;`——全局已满足的位不再往下查。
- **跳过内部表与 schema 表。** `if (!(~t_ref->grant.privilege & want_access) || t_ref->is_internal() || t_ref->schema_table) continue;`
- **跳过临时表。** `is_temporary_table(t_ref)` 为真时 `t_ref->grant.privilege |= TMP_TABLE_ACLS;` 并 `continue`。源码注释说明：临时表的所有相关表级权限检查都跳过，但**创建**临时表时仍需检查 `CREATE_TMP_ACL`。
- **角色路径。** `sctx->get_active_roles()->size() != 0` 时，`t_ref->grant.grant_table = nullptr; t_ref->grant.version = grant_version;`（**主动丢弃旧的哈希表授权指针**），然后取 `sctx->table_and_column_acls(...)` 得到 `aggr`。若 `any_combination_will_do && (aggr.cols != 0 || aggr.table_access != 0)` 则 `continue`（SHOW COLUMNS / SHOW INDEX 只要任意列组合有权限即可）。否则 `t_ref->grant.privilege |= aggr.table_access;`，若已覆盖 `want_access` 则 `continue`；若 `want_access & ~(aggr.cols | t_ref->grant.privilege)` 非零，说明有权限位连列级都覆盖不了，`want_access &= ~(aggr.cols | t_ref->grant.privilege); goto err;`。
- **无角色路径（延迟加锁）。** 无角色时不立刻进 ACL 缓存，而是先 `tables_to_be_processed_further.push_back(tl);`——源码注释解释：先遍历所有 `Table_ref` 收集待处理表，再统一访问 ACL 缓存，这样可以把加锁推迟。
- **统一加锁查表级授权。** 循环外 `Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE); if (!acl_cache_lock.lock(!no_errors)) return true;`，然后 `table_hash_search(sctx->host().str, sctx->ip().str, db_name, sctx->priv_user().str, t_ref->get_table_name(), false)` 查 `GRANT_TABLE`。查不到则 `want_access &= ~t_ref->grant.privilege; goto err;`。查到则 `t_ref->grant.grant_table = grant_table;`（留给列级检查用）、`t_ref->grant.version = grant_version;`、`t_ref->grant.privilege |= grant_table->privs;`。
- **失败出口。** `err:` 标签处用 `get_privilege_desc(command, sizeof(command), want_access)` 把权限位翻译成可读文本，再报 `ER_TABLEACCESS_DENIED_ERROR`，参数依次是权限描述、用户名、主机或 IP、表名（表名为空时写 `"unknown"`）。

列级检查在 `check_grant_column(THD *thd, GRANT_INFO *grant, const char *db_name, const char *table_name, const char *name, size_t length, Security_context *sctx, Access_bitmask want_privilege)` 中实现。它先做 `want_privilege &= ~grant->privilege;`（表级已覆盖的部分不再查），`if (!want_privilege) return false;`，然后加 `Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::READ_MODE);` 并 `if (!acl_cache_lock.lock()) return true;`。

### 权限缓存与失效

权限缓存有两层。

**第一层：表/列授权缓存。** 用全局 `grant_version` 做版本号。它在 `sql/auth/sql_auth_cache.cc` 中定义为 `uint grant_version = 0; /* Version of priv tables */`。检查侧把当前版本记进 `grant.version`（如 `t_ref->grant.version = grant_version;`），再用 `if (grant->version != grant_version)` 判断缓存是否过期，过期则重查并更新 `grant->version`。失效的触发点是 `grant_version++`，出现在 `sql/auth/sql_auth_cache.cc` 的 ACL 重载路径里。

**第二层：角色 ACL 映射缓存。** `Acl_cache` 用 `m_role_graph_version` 做版本号：

```c
void Acl_cache::increase_version() {
  ++m_role_graph_version;
  flush_cache();
}
uint64 Acl_cache::version() { return m_role_graph_version.load(); }
```

`Acl_cache::checkout_acl_map(Security_context *sctx, Auth_id_ref &uid, List_of_auth_id_refs &active_roles)` 用「版本号 + 用户 + 激活角色列表」构造哈希键（`create_acl_cache_hash_key`），在 `m_cache` 这个 LF_HASH 里查找。命中则 `map->increase_reference_count()` 返回旧映射；未命中则 `create_acl_map(version, sctx)` 新建并 `lf_hash_insert`。插入返回非零（键重复）时释放已分配内存并**递归重试** `checkout_acl_map()`。

`Acl_cache::flush_cache()` 用 `cache_flusher` 回调逐个删除条目，删除条件是 `entry->map->reference_count() == 0 && entry->map->version() < l_cache_flusher_global_version`——**引用计数非零的映射不被删除**，这是正在使用中的会话不受 GRANT/REVOKE 影响的原因。

**语句级的重新取图。** `Security_context::checkout_access_maps(void)` 在 `sql/auth/sql_security_ctx.cc` 中定义：

- 先把已有的 `m_acl_map` 还回缓存（`get_global_acl_cache()->return_acl_map(m_acl_map);`）并置空。
- `if (m_active_roles.size() == 0) return;`——**没有激活角色就不需要 Acl_map**，直接返回。
- 有角色则 `get_global_acl_cache()->checkout_acl_map(this, uid, m_active_roles)` 取映射；取到则 `set_master_access(m_acl_map->global_acl(), m_acl_map->restrictions());`，取不到则 `set_master_access(0);`。

它的函数文档说明了调用时机：「We do this subscription before execution of every statement (prepared or conventional) as the global acl version might have increased due to a grant/revoke or flush. Hence, the granularity of after effects of grant/revoke or flush due to roles is per statement.」调用点之一在 `sql/sql_parse.cc` 中，每条语句执行前调 `thd->security_context()->checkout_access_maps();`；另一个调用点在 `sql/auth/sql_authentication.cc` 的认证成功路径上。

### 上游调用方

`sql/sql_parse.cc` 的 `mysql_execute_command()` 里有多处 `check_access()` 调用。以 GRANT 分支为例：

- 静态权限场景：`check_access(thd, lex->grant | lex->grant_tot_col | GRANT_ACL, first_table ? first_table->db : query_block->db, first_table ? &first_table->grant.privilege : nullptr, first_table ? &first_table->grant.m_internal : nullptr, first_table ? false : true, false)`，返回真则 `goto error`。
- 动态权限场景：调用同一个 `check_access()`，但最后一个参数 `no_errors` 传 `true`。源码注释说明理由：「check_access will load grant.privilege and grant.m_internal with values which are used later during column privilege checking. The return value isn't interesting as we'll check for dynamic global privileges later.」

表级与列级的调用方在同一个文件的同一分支里：GRANT 存储过程/函数走 `check_grant_routine(thd, grants | GRANT_ACL, all_tables, ...)`，否则走 `check_grant(thd, (lex->grant | lex->grant_tot_col | GRANT_ACL), all_tables, false, UINT_MAX, false)`。

`check_table_access()` 是把 `check_access()` 与 `check_grant()` 串起来的胶水：它先对每张表调 `check_access()`（把 `table_ref->grant.privilege` 和 `table_ref->grant.m_internal` 作为输出参数传进去），成功后再调 `check_grant()`。它的注释里写明了这个分工：「This functions calls the help functions check_access and check_grant to verify the first three steps in the privileges check queue」。

## 关键取舍

**「返回 true 表示拒绝」的约定贯穿整个模块。** `check_access`、`check_grant`、`check_grant_column`、`check_table_access` 全部遵循这个约定。好处是这些函数可以直接串在 `if (...) goto err;` 的形式里；代价是这个约定与 C++ 习惯相反，调用方必须逐个确认。

**用位掩码的「全包含」判定而不是逐位检查。** 核心判定是 `(db_access & want_access) == want_access` 这一类表达式，一次比较就判定「请求的权限位是否被全部覆盖」。`Security_context::check_access()` 里 `match_any` 参数提供的则是「任一命中即通过」的变体。代价是位掩码语义完全依赖调用方传对的常量——`DB_ACLS`、`TABLE_ACLS`、`PROC_ACLS` 这些掩码的定义在本次本地检出未包含的头文件里，读不到具体位值，只能从用法反推它们的归属层级。

**角色的存在与否会切换整套数据源。** `sctx->get_active_roles()->size() > 0` 这一个判断决定走 `Acl_map` 还是走 `acl_get()` + `column_priv_hash`。这不是性能优化，而是语义切换：角色的库级权限需要通配匹配（`db_wild_acls()`），而经典路径的库名匹配在 `mysqld_partial_revokes()` 为假时才用 `wild_compare()`。代价是同一套权限判定逻辑被写了两遍，两条路径的行为必须人工保持一致。

**内部 schema 的权限被硬编码成「只有 SELECT」。** `IS_internal_schema_access::check()` 里 `if (unlikely(want_access & DB_ACLS)) return ACL_INTERNAL_ACCESS_DENIED;` 加上无条件的 `*save_priv |= SELECT_ACL;`。`information_schema` 因此不需要任何授权就能查，但任何库级权限位都会直接被拒。

**表级检查刻意延迟加锁。** `check_grant()` 在无角色路径下先把待查的表收进 `tables_to_be_processed_further`，再统一加一次 `Acl_cache_lock_guard` 读锁。源码注释写明：「We will first go over all Table_ref objects and create a list of objects to be processed further. Later, we will access ACL caches for these tables. It allows us to delay locking of ACL caches.」代价是这段代码里用了 `goto err` 跳进循环外的错误处理块，控制流不线性。

**角色路径下主动丢弃旧的哈希表授权指针。** 角色分支里 `t_ref->grant.grant_table = nullptr;` 这行把之前 `check_access()` 填进去的 `GRANT_TABLE *` 指针清掉。原因是角色的表级权限来自 `Acl_map` 而不是 `column_priv_hash`，留着旧指针会让列级检查读到错误的授权数据。

**`Acl_cache::flush_cache()` 不删除正在被使用的映射。** 删除条件里的 `entry->map->reference_count() == 0` 是硬门槛。这意味着一条 GRANT 语句不会让正在执行的另一条语句中途失去权限——代价是被引用的旧映射要等引用归零才被回收。

**架构推论：** `check_access()` 与 `check_grant()` 的接口设计（一个返回「实际生效的权限位」写到 `save_priv`，一个返回「是否拒绝」）让两级检查可以各自独立演进：新增一类权限只要在这两个函数之间加一层，不必改动所有调用点。而 `GRANT_INFO` 这个结构体（`privilege` + `grant_table` + `version` + `m_internal`）在两级之间传递「已判定什么、还没判定什么」，是整个判定流程的隐式状态载体。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/auth/sql_authorization.cc` | `check_access` | 全局 + 库级权限判定的主函数：先判内部 schema 规则，再判全局权限命中，未命中时查库级权限；返回 true 表示拒绝，并把实际生效的权限位写入 `save_priv` |
| `sql/auth/sql_authorization.cc` | `check_table_access` | 胶水层：对每张表依次调 `check_access()` 与 `check_grant()`，把两者串成完整的表级判定 |
| `sql/auth/sql_authorization.cc` | `check_grant` | 表级授权判定：减掉全局已覆盖的位，查 `column_priv_hash`（无角色）或 `m_acl_map->table_acls()`（有角色）；失败报 `ER_TABLEACCESS_DENIED_ERROR` |
| `sql/auth/sql_authorization.cc` | `check_grant_column` | 列级授权判定：先减去表级已覆盖的位，再查 `GRANT_COLUMN`；拒绝时报 `ER_COLUMNACCESS_DENIED_ERROR` |
| `sql/auth/sql_authorization.cc` | `check_grant_db` | 库级可访问性判定：有角色时用 `db_acl()` 与 `DB_OP_ACLS`，无角色时查 `column_priv_hash` 的表级授权，再退到 `check_grant_db_routine()` |
| `sql/auth/sql_authorization.cc` | `check_grant_db_routine` | 在 `proc_priv_hash` / `func_priv_hash` 里按用户、库名、主机匹配例程级授权 |
| `sql/auth/sql_authorization.cc` | `check_grant_routine` | 例程级权限判定，供 GRANT PROCEDURE / GRANT FUNCTION 路径使用 |
| `sql/auth/sql_authorization.cc` | `get_cached_schema_access` | 内部 schema 访问规则的缓存查找，用 `m_schema_lookup_done` 避免重复查注册表 |
| `sql/auth/sql_authorization.cc` | `get_cached_table_access` | 内部表访问规则的缓存查找，先解析 schema 规则再查表名 |
| `sql/auth/sql_authorization.cc` | `IS_internal_schema_access::check` | `information_schema` 的权限规则：只授予 `SELECT_ACL`，任何库级权限位直接 `ACL_INTERNAL_ACCESS_DENIED` |
| `sql/auth/sql_authorization.cc` | `lock_tables_precheck` | `LOCK TABLES` 的前置检查，对每张表调 `check_table_access(thd, LOCK_TABLES_ACL | SELECT_ACL, ...)` |
| `sql/auth/sql_security_ctx.cc` | `Security_context::check_access` | 全局权限位判定：先查库级权限位是否被部分撤销（`is_access_restricted_on_db`），再按 `match_any` 做「任一命中」或「全包含」比较 |
| `sql/auth/sql_security_ctx.cc` | `Security_context::master_access` | 返回按库 restrictions 过滤后的全局权限位：`filter_access(m_master_access, db_name)` |
| `sql/auth/sql_security_ctx.cc` | `Security_context::filter_access` | 按库的 restrictions 从权限掩码里抠掉被部分撤销的位 |
| `sql/auth/sql_security_ctx.cc` | `Security_context::is_access_restricted_on_db` | 判断某库上是否存在权限限制：过滤结果与原掩码不同即为有 |
| `sql/auth/sql_security_ctx.cc` | `Security_context::db_acl` | 角色路径的库级权限查询：先查 `db_acls()` 精确匹配，再遍历 `db_wild_acls()` 做通配匹配 |
| `sql/auth/sql_security_ctx.cc` | `Security_context::table_and_column_acls` | 角色路径的表级与列级权限查询，按 `db.table` 拼键查 `m_acl_map->table_acls()` |
| `sql/auth/sql_security_ctx.cc` | `Security_context::checkout_access_maps` | 语句级重新取角色权限映射：归还旧 `Acl_map`、无激活角色时直接返回、有角色时向 `Acl_cache` 重新 checkout |
| `sql/auth/sql_security_ctx.cc` | `Security_context::has_global_grant` | 动态权限判定：返回「是否拥有」与「是否带 GRANT OPTION」两元组；`--skip-grant-tables` 时直接返回真 |
| `sql/auth/sql_security_ctx.cc` | `Security_context::skip_grants` | `--skip-grant-tables` 模式：把 `m_master_access` 置为 `ALL_ACCESS` |
| `sql/auth/sql_auth_cache.cc` | `acl_get` | 无角色路径的库级权限查询：查 `db_cache` 哈希，未命中则遍历 `acl_dbs` 链表并写回缓存；键长超 `ACL_KEY_LENGTH` 直接返回 0 |
| `sql/auth/sql_auth_cache.cc` | `Acl_cache::checkout_acl_map` | 按「版本号 + 用户 + 激活角色」构造键，在 LF_HASH 中取或建 `Acl_map`；插入冲突时递归重试 |
| `sql/auth/sql_auth_cache.cc` | `Acl_cache::increase_version` | 权限版本推进：`++m_role_graph_version` 后调 `flush_cache()` |
| `sql/auth/sql_auth_cache.cc` | `Acl_cache::flush_cache` | 清理哈希中引用计数为 0 且版本落后的 `Acl_map` 条目 |
| `sql/auth/sql_auth_cache.cc` | `grant_version` | 表/列授权缓存的全局版本号，ACL 重载时递增，使已缓存的 `grant.version` 失效 |

## 与主核 / AI 的关系

主核（`engine/db`）是嵌入式内核，没有连接、没有角色、也没有多级权限，因此没有与本页一一对应的机制。主核里最接近的是接口契约层的能力校验——SPI 调用方在进入内核前做鉴权，校验点是「这次调用被允许吗」这一个布尔判定。

**架构推论：** MySQL 这套判定里有两点对 AI 场景直接有用。其一是**权限位掩码的全包含判定**。`(db_access & want_access) == want_access` 用一次位运算就判定「请求的能力集合是否被授权集合完全覆盖」，而不是逐个能力做列表比对。AI 代理接入时若要表达「我可以调用 A 和 B，但不能调用 C」，掩码式的判定比能力列表更省事——但代价是掩码里的位只能由服务端定义，代理侧无法自行扩展。其二是**部分撤销（partial revoke）的表达方式**。`filter_access()` 用「在某个库上从全局掩码里抠掉某些位」来表达「全局有这个权限，但这个库上没有」。这种「基线 + 按范围扣减」的模型比「逐条授予」更贴近现实权限体系——AI 代理的权限往往就是「全局能做某事，但在某些资源上不能做」。

**架构推论：** MySQL 的权限判定有明确的「分层降级」结构：全局 → 库 → 表 → 列 → 例程，每层只在前一层不满足时才参与。AI 代理的授权模型通常没有这么多层，因此迁移时最容易犯的错误是把「分层」理解成「串联检查」——实际上 `check_access()` 内部对全局权限的判定是**短路返回**的：全局足够就直接 `return false`，根本不会去查库级权限。这意味着「全局权限」在语义上是一个上界而不是一个额外层级。

对 AI 场景而言，本页机制与 AI 代理的关系主要体现在三点：

- **判定发生在命令解析之后、执行之前。** `check_access()` 的调用点在 `sql/sql_parse.cc` 的 `mysql_execute_command()` 内，即语句已解析成命令之后。AI 代理作为自研客户端，如果发送的是协议层的 COM_QUERY，权限判定由服务端完成；如果走的是嵌入式接口，则判定责任落在调用方一侧——这与 MySQL 的「服务端负责」立场相反，是设计 AI 接入层时必须先定下来的分工。

- **GRANT 语句的动态权限路径刻意忽略 `check_access()` 的返回值。** 源码注释写明：「check_access will load grant.privilege and grant.m_internal with values which are used later during column privilege checking. The return value isn't interesting as we'll check for dynamic global privileges later.」也就是说 `check_access()` 在这条路径上只被当作「填表结构的工具」而非「判定器」。AI 代理若要实现自己的授权管理接口，必须清楚区分「判定」与「填充上下文」这两种用法——同一个函数在两条路径上的契约不同。

- **权限失效的粒度是「每条语句」。** `checkout_access_maps()` 的文档写明粒度是 per statement：每条语句执行前重新 checkout 一次角色权限映射。因此 GRANT/REVOKE 对已连接会话的影响，最迟在下一条语句生效。AI 代理在长会话中持有凭据时，若需要「权限被撤销后立即失效」的语义，不能依赖服务端这条路径——需要代理自己在每轮调用前主动重新校验。

## 参考

- [MySQL 源码：`sql/auth/sql_authorization.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authorization.cc)
- [MySQL 源码：`sql/auth/sql_security_ctx.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_security_ctx.cc)
- [MySQL 源码：`sql/auth/sql_auth_cache.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_auth_cache.cc)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 参考手册：权限系统概览（The MySQL Access Control System）](https://dev.mysql.com/doc/refman/8.0/en/privileges.html)
- [MySQL 参考手册：授权级别（Privilege Levels）](https://dev.mysql.com/doc/refman/8.0/en/privilege-levels.html)
- [MySQL 参考手册：动态权限（Dynamic Privileges）](https://dev.mysql.com/doc/refman/8.0/en/dynamic-privileges.html)
- [MySQL 参考手册：基于角色的访问控制（Role-Based Access Control）](https://dev.mysql.com/doc/refman/8.0/en/roles.html)
- [MySQL 参考手册：`partial_revokes` 系统变量](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_partial_revokes)
