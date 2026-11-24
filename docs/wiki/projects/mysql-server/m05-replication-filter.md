---
project: mysql-server
feature: replication-filter
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_filter.cc
    symbols: [Rpl_filter::tables_ok, Rpl_filter::reset]
---

# 复制过滤规则

## 一句话职责

复制过滤机制负责**决定某条复制事件、某张表、某个库到底要不要复制/应用**：它把 `REPLICATE_DO_DB` / `REPLICATE_IGNORE_DB` / `REPLICATE_DO_TABLE` / `REPLICATE_IGNORE_TABLE` / `REPLICATE_WILD_DO_TABLE` / `REPLICATE_WILD_IGNORE_TABLE` / `REPLICATE_REWRITE_DB` 这七类规则读成内存里的结构，然后由 `Rpl_filter::tables_ok`（表级判定）、`Rpl_filter::db_ok`（库级判定）、`Rpl_filter::db_ok_with_wild_table`（库名对通配表规则判定）给出放行/丢弃的布尔结论。规则的增删改由 `sql/rpl_filter.cc` 的 `Rpl_filter::set_do_db` / `set_ignore_db` / `set_do_table` / `set_ignore_table` / `set_wild_do_table` / `set_wild_ignore_table` / `set_db_rewrite` 落地，全量清理由 `Rpl_filter::reset` 落地。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责按库表白名单/黑名单过滤复制事件；**不负责事件内容（L 组）**、**不负责从库的位点管理（M02）**。

## 机制是什么

### 七类规则、三种存储形态

`Rpl_filter` 把七类规则拆进两种容器形态，共六个集合，外加一个库名改写对列表：

| 规则类型 | 容器 | 匹配方式 | 判定入口 |
|---|---|---|---|
| `REPLICATE_DO_TABLE` | `do_table_hash`（哈希表） | `db.table` 全名精确命中 | `Rpl_filter::tables_ok` |
| `REPLICATE_IGNORE_TABLE` | `ignore_table_hash`（哈希表） | `db.table` 全名精确命中 | `Rpl_filter::tables_ok` |
| `REPLICATE_WILD_DO_TABLE` | `wild_do_table`（动态数组） | `my_wildcmp` 通配匹配 | `Rpl_filter::tables_ok` / `db_ok_with_wild_table` |
| `REPLICATE_WILD_IGNORE_TABLE` | `wild_ignore_table`（动态数组） | `my_wildcmp` 通配匹配 | `Rpl_filter::tables_ok` / `db_ok_with_wild_table` |
| `REPLICATE_DO_DB` | `do_db`（链表 `I_List<i_string>`） | `my_strcasecmp` 库名精确比对 | `Rpl_filter::db_ok` |
| `REPLICATE_IGNORE_DB` | `ignore_db`（链表 `I_List<i_string>`） | `my_strcasecmp` 库名精确比对 | `Rpl_filter::db_ok` |
| `REPLICATE_REWRITE_DB` | `rewrite_db`（链表 `I_List<i_string_pair>`） | 源库名精确比对，替换为目标库名 | `Rpl_filter::get_rewrite_db` |

**为什么精确表规则用哈希表、通配表规则用动态数组：** `sql/rpl_filter.cc` 的 `Rpl_filter::build_do_table_hash` 与 `build_ignore_table_hash` 会把动态数组里的规则整体搬进哈希表，然后**把原动态数组释放掉**（`free_string_array(&do_table_array); do_table_array_inited = false;`）。而通配规则无法哈希化——`my_wildcmp` 需要逐条试匹配，所以 `wild_do_table` / `wild_ignore_table` 始终保留为动态数组，`Rpl_filter::find_wild` 线性遍历数组逐条比对。

**哈希表的键构造方式：** `Rpl_filter::add_table_rule_to_hash` 把 `db.table` 整串作为哈希键存进 `TABLE_RULE_ENT`（`e->db` 指向块内偏移 `sizeof(TABLE_RULE_ENT)`，`e->tbl_name` 指向 `.` 之后的表名，`e->key_len` 记整串长度），块大小是 `sizeof(TABLE_RULE_ENT) + len`，一次 `my_malloc` 分配、一次 `memcpy` 拷贝。

**表规则必须带库名前缀：** `Rpl_filter::add_table_rule_to_array` 与 `Rpl_filter::add_table_rule_to_hash` 都以 `const char *dot = strchr(table_spec, '.'); if (!dot) return 1;` 开头——**找不到 `.` 直接返回错误码 1**，规则不会被接受。

### 两种大小写语义：一个全局字符集决定一切

`sql/rpl_filter.cc` 里所有名字比对都走同一个字符集 `table_alias_charset`：`Rpl_filter::db_ok` 用 `my_strcasecmp(table_alias_charset, tmp->ptr, db)`、`Rpl_filter::find_wild` 用 `my_wildcmp(table_alias_charset, ...)`、`Rpl_filter::get_rewrite_db` 用 `my_strcasecmp(table_alias_charset, tmp->key, db)`。

这个字符集由 `sql/mysqld.cc` 的初始化代码设定：

```cpp
/* Reset table_alias_charset, now that lower_case_table_names is set. */
table_alias_charset = (lower_case_table_names ? &my_charset_utf8mb3_tolower_ci
                                              : &my_charset_bin);
```

即**`lower_case_table_names` 非 0 时规则名大小写不敏感，为 0 时大小写敏感**。源码注释把这个语义写在 `db_ok` 与 `find_wild` 的注释里：「Filters will follow the setting of lower_case_table_name to be case sensitive when lower_case_table_name=0. Otherwise they will be case insensitive but accent sensitive.」

### 大小写规则还有一个前置依赖

`sql/mysqld.cc` 里 `table_alias_charset` 的赋值在 `lower_case_table_names` 定型之后，紧接着才是哈希表构建：

```cpp
if (rpl_global_filter.build_do_table_hash() ||
    rpl_global_filter.build_ignore_table_hash()) {
  LogErr(ERROR_LEVEL, ER_CANT_HASH_DO_AND_IGNORE_RULES);
  return 1;
}
```

**顺序是硬约束**——哈希表必须在字符集定型之后构建，否则建出来的键的大小写折叠语义与运行时不一致。构建失败直接 `LogErr(ERROR_LEVEL, ER_CANT_HASH_DO_AND_IGNORE_RULES)` 并让启动流程返回 1。

### 规则有「全局」和「按通道」两套

`sql/mysqld.cc` 里有两处全局对象：`rpl_global_filter`（`Rpl_global_filter` 实例，`sql/mysqld.cc` 定义）与 `rpl_channel_filters`（按通道取过滤器）。启动参数 `--replicate-*` 的分派逻辑靠 `is_rpl_global_filter_setting` 判断归属：

```cpp
static bool is_rpl_global_filter_setting(char *argument) {
  bool res = false;
  char *p = strchr(argument, ':');
  if (p == nullptr) res = true;
  return res;
}
```

**参数里不含 `:` 就是全局规则，含 `:` 就是通道规则**（通道名与规则值由 `parse_filter_arg` 按第一个 `:` 切开）。两条路径的差别不只在归属，还在统计口径：全局路径记 `CONFIGURED_BY_STARTUP_OPTIONS`，通道路径记 `CONFIGURED_BY_STARTUP_OPTIONS_FOR_CHANNEL`。

### 全局规则如何下沉到通道

`Rpl_filter::copy_global_replication_filters` 负责把全局规则复制到某个通道的过滤器。复制有三条不同的路径，因为三对容器的形态不同：

- **精确表规则**：全局侧存的是哈希（数组在建哈希时被释放了），所以要**反向**做一次哈希转数组：`table_rule_ent_hash_to_array` 从全局的 `do_table_hash` 还原出 `do_table_array`，再调 `build_do_table_hash` 在本通道重新建哈希。
- **通配表规则**：全局侧本来就是数组，直接 `table_rule_ent_array_to_array` 逐条深拷贝。
- **库规则与改写规则**：库规则走 `parse_filter_list(I_List<i_string>*, Add_filter)` 逐条 `add_do_db` / `add_ignore_db`；改写规则则遍历全局 `rewrite_db` 链表逐对调 `add_db_rewrite`。

**复制是「只填空缺」的**：`do_table_hash_inited` / `ignore_table_hash_inited` / `wild_do_table_inited` / `wild_ignore_table_inited` 为真时对应分支整段跳过；`do_db` / `ignore_db` / `rewrite_db` 则用 `is_empty()` 判定。

**真实调用方是 `sql/rpl_info_factory.cc`：** 该文件在通道配置时装配过滤规则，`rli->rpl_filter->copy_global_replication_filters()` 返回非 0 时记 `LogErr(ERROR_LEVEL, ER_RPL_REPLICA_GLOBAL_FILTERS_COPY_FAILED, channel_name)` 并返回失败。

### 组复制通道被显式排除

同一段代码里有两条硬排除：

- `sql/rpl_info_factory.cc`：`if (channel_map.is_group_replication_channel_name(channel_name)) return false;`，注释写明「GROUP REPLICATION channels should not be configurable using --replicate* nor CHANGE REPLICATION FILTER, and should not inherit from global filters.」
- `sql/rpl_filter.cc` 的 `Sql_cmd_change_repl_filter::change_rpl_filter`：显式带 `FOR CHANNEL` 且通道名是组复制通道时，报 `ER_REPLICA_CHANNEL_OPERATION_NOT_ALLOWED`。

## 怎么实现的

### 表级判定：`Rpl_filter::tables_ok`

`Rpl_filter::tables_ok(const char *db, Table_ref *tables)` 是本页 refs 点名的两个符号之一。它的函数体结构是「**逐表遍历 + 短路返回**」：

```cpp
bool Rpl_filter::tables_ok(const char *db, Table_ref *tables) {
  bool some_tables_updating = false;

  for (; tables; tables = tables->next_global) {
    char hash_key[2 * NAME_LEN + 2];
    char *end;
    uint len;

    if (!tables->updating) continue;
    some_tables_updating = true;
    end = my_stpcpy(hash_key, tables->db ? tables->db : db);
    *end++ = '.';
    len = (uint)(my_stpcpy(end, tables->table_name) - hash_key);
    if (do_table_hash_inited)  // if there are any do's
    {
      if (do_table_hash->count(std::string(hash_key, len)) != 0) {
        do_table_statistics.increase_counter();
        return true;
      }
    }
    if (ignore_table_hash_inited)  // if there are any ignores
    {
      if (ignore_table_hash->count(std::string(hash_key, len)) != 0) {
        ignore_table_statistics.increase_counter();
        return false;
      }
    }
    if (wild_do_table_inited && find_wild(&wild_do_table, hash_key, len)) {
      wild_do_table_statistics.increase_counter();
      return true;
    }
    if (wild_ignore_table_inited &&
        find_wild(&wild_ignore_table, hash_key, len)) {
      wild_ignore_table_statistics.increase_counter();
      return false;
    }
  }

  return some_tables_updating && !do_table_hash_inited && !wild_do_table_inited;
}
```

**四个判定分支的顺序就是优先级：** do_table（哈希）→ ignore_table（哈希）→ wild_do_table → wild_ignore_table。**先命中 do 就返回 true（放行），先命中 ignore 就返回 false（丢弃）**。`do_table_hash` 与 `ignore_table_hash` 同时包含同一张表时，**do 胜出**——因为 do 分支在前。

**只看「要改的表」：** 循环开头 `if (!tables->updating) continue;` 跳过只读表，并把 `some_tables_updating` 置真。`tables->updating` 由 `sql/sql_parse.cc` 的 `Query_block::set_lock_for_table` 设置（写锁时 `table->updating = for_update;`），注释明确说这是「to get tables_ok to know that the table is being updated by the query」。

**兜底的返回值语义**：`return some_tables_updating && !do_table_hash_inited && !wild_do_table_inited;` ——三项同时成立才放行。逐项拆开：

- `some_tables_updating` 为假（整个语句不更新任何表）→ 返回 false，**不复制**。源码注释解释：「If no table was to be updated, ignore statement (no reason we play it on slave, slave is supposed to replicate _changes_ only)」。
- 存在 do 规则（`do_table_hash_inited` 或 `wild_do_table_inited` 为真）→ 返回 false，**不在白名单里就不复制**。注释：「If no explicit rule found and there was a do list, do not replicate.」
- 既没有 do 规则、也有表要更新 → 返回 true，**放行**。注释：「If there was no do list, go ahead」。

**哈希键的构造**：`end = my_stpcpy(hash_key, tables->db ? tables->db : db); *end++ = '.';` ——表自带库名就用表自己的，没有就用传进来的 `db` 参数。缓冲区是 `char hash_key[2 * NAME_LEN + 2]`（`NAME_LEN` 是 MySQL 标识符长度上限，定义在 `mysql_com.h`；该文件不在本地稀疏检出内，本页不引用其具体数值）。

**多表语句要调用两次：** 函数上方的注释写明，语句是 multi-delete（`SQLCOM_DELETE_MULTI`）时会**第二次调用** `tables_ok`，这次传入的 `tables` 里带 `updating==TRUE`（DELETE 之后的那部分表），由第二次调用做决定；源码把整体判据写成 `all_tables_not_ok() = !tables_ok(1st_list) && !tables_ok(2nd_list)`。

### `all_tables_not_ok` 与它的调用方

`sql/sql_parse.cc` 里有一个包装层：

```cpp
bool all_tables_not_ok(THD *thd, Table_ref *tables) {
  Rpl_filter *rpl_filter = thd->rli_slave->rpl_filter;

  return rpl_filter->is_on() && tables && !thd->sp_runtime_ctx &&
         !rpl_filter->tables_ok(thd->db().str, tables);
}
```

**三个前置条件缺一不可**：`is_on()` 为真、`tables` 非空、**当前不在存储过程上下文里**（`!thd->sp_runtime_ctx`）。`is_on()` 返回的是成员 `table_rules_on`，它在 `Rpl_filter::add_do_table_array` / `add_ignore_table_array` / `add_wild_do_table` / `add_wild_ignore_table` 四个函数里被置真——**即只有表类规则才会把 `table_rules_on` 打开**，库类规则不影响它。

`sql/sql_parse.cc` 的 `SQLCOM_UPDATE_MULTI` 且 `thd->table_map_for_update` 非空的路径上调用了 `all_tables_not_ok(thd, all_tables)`（主库把要更新的表位图存进 binlog，从库据此免开表即可套用过滤规则）。

### 库级判定：`Rpl_filter::db_ok`

`Rpl_filter::db_ok(const char *db, bool need_increase_counter)` 的判据只有四步：

1. `if (do_db.is_empty() && ignore_db.is_empty()) return true;` ——**没有任何库规则就一律放行**（注释：`// Ok to replicate if the user puts no constraints`）。
2. `if (!db) return true;` ——**库名为空一律放行**。注释解释了为什么改的：「Previous behaviour "if the user has specified restrictions on which databases to replicate and db was not selected, do not replicate" has been replaced with "do replicate". Since the filtering criteria is not equal to "NULL" the statement should be logged into binlog.」
3. `if (!do_db.is_empty())` ——**有 do 列表时，只看 do 列表**：逐条 `my_strcasecmp(table_alias_charset, tmp->ptr, db)`，命中则 `do_db_statistics.increase_counter()`（仅当 `need_increase_counter` 为真）并 `return true`；**do 列表遍历完没命中就 `return false`**。
4. 否则（只有 ignore 列表）——逐条比对 ignore 列表，命中则 `return false`；**遍历完没命中则 `return true`**。

**关键结论：do 列表与 ignore 列表不同时生效。** 代码里 do 分支是 `if (!do_db.is_empty()) { ... return; } else { ... }`——只要 do 列表非空就**根本不会去看 ignore 列表**。`sql/sql_parse.cc` 的 `check_database_filters` 注释也印证了这一点：「This means that when using this function replicate-do-db and replicate-ignore-db take precedence over wild do rules.」

### 库名对通配表规则判定：`Rpl_filter::db_ok_with_wild_table`

这个函数的用途在源码注释里说得很清楚：`binlog-ignore-db` / `replicate-ignore-db` 只检查「当前选中的库」，所以 `USE db2; UPDATE db1.t SET ...` 这种跨库语句会漏过；而 `replicate_wild_ignore_table='db1.%'` 在所有情况下都有效。为支持 `DROP DATABASE db1` 这类不涉及表的操作，注释说「we want to interpret 'db1.%' as "everything touching db1"」，因此要把库名 `db1` 也拿去和 `db1.%` 这类通配规则比。

函数体：

```cpp
bool Rpl_filter::db_ok_with_wild_table(const char *db) {
  char hash_key[NAME_LEN + 2];
  char *end;
  size_t len;
  end = my_stpcpy(hash_key, db);
  *end++ = '.';
  len = end - hash_key;
  if (wild_do_table_inited && find_wild(&wild_do_table, hash_key, len)) {
    wild_do_table_statistics.increase_counter();
    return true;
  }
  if (wild_ignore_table_inited &&
      find_wild(&wild_ignore_table, hash_key, len)) {
    wild_ignore_table_statistics.increase_counter();
    return false;
  }
  return !wild_do_table_inited;
}
```

**它构造的键是 `db` 加一个 `.`（注意末尾那个点被算进了 `len`），即拿 `db.` 去和通配规则比**——这样 `db1.%` 规则里的 `%` 正好能匹配 `db1.` 后面的空串。

**兜底返回值是 `!wild_do_table_inited`**：存在 wild_do 规则而没命中 → false（不放行）；没有 wild_do 规则 → true（放行）。

### 谁在什么时机调用这两个库级判定

`sql/sql_parse.cc` 的 `check_database_filters` 是从库执行语句时的库级过滤入口：

```cpp
inline bool check_database_filters(THD *thd, const char *db,
                                   enum_sql_command sql_cmd) {
  assert(thd->slave_thread);
  if (!db || is_normal_transaction_boundary_stmt(sql_cmd) ||
      is_xa_transaction_boundary_stmt(sql_cmd))
    return true;

  Rpl_filter *rpl_filter = thd->rli_slave->rpl_filter;
  auto db_ok{rpl_filter->db_ok(db)};
  if (db_ok && (rpl_filter->get_do_db()->is_empty() &&
                rpl_filter->get_ignore_db()->is_empty())) {
    switch (sql_cmd) {
      case SQLCOM_CREATE_DB:
      case SQLCOM_ALTER_DB:
      case SQLCOM_DROP_DB:
        db_ok = rpl_filter->db_ok_with_wild_table(db);
      default:
        break;
    }
  }
  return db_ok;
}
```

三点值得注意：

- **事务边界语句直接放行**：`is_normal_transaction_boundary_stmt` 覆盖 `SQLCOM_BEGIN` / `SQLCOM_COMMIT` / `SQLCOM_SAVEPOINT` / `SQLCOM_ROLLBACK` / `SQLCOM_ROLLBACK_TO_SAVEPOINT`；`is_xa_transaction_boundary_stmt` 覆盖 `SQLCOM_XA_START` / `SQLCOM_XA_END` / `SQLCOM_XA_COMMIT` / `SQLCOM_XA_ROLLBACK`。
- **`db_ok_with_wild_table` 只在「没有库规则」时才可能被调用**——条件是 `rpl_filter->get_do_db()->is_empty() && rpl_filter->get_ignore_db()->is_empty()`。
- **且只对三类库操作生效**：`SQLCOM_CREATE_DB` / `SQLCOM_ALTER_DB` / `SQLCOM_DROP_DB`。

**从库侧还有一处独立的库级判定**——`sql/log_event.cc` 的 `check_table_map`：

```cpp
if (rli->info_thd->slave_thread /* filtering is for slave only */ &&
    (!rli->rpl_filter->db_ok(table_list->db) ||
     (rli->rpl_filter->is_on() &&
      !rli->rpl_filter->tables_ok("", table_list))))
```

**三个条件：** 必须是 `slave_thread`（注释：`/* filtering is for slave only */`）；库级判定 `db_ok` 与表级判定 `tables_ok` **任一为假就过滤掉**。表级判定在这里传的是空串 `""` 作为 db 参数，因为 `RPL_Table_ref` 自带 `db` 字段。

**XA 事务里的特殊结果：** 命中过滤条件时，如果事务处于 XA 活跃态（`get_transaction()->xid_state()->has_state(XID_STATE::XA_ACTIVE)`），结果是 `FILTERED_WITH_XA_ACTIVE` 而不是 `FILTERED_OUT`；`sql/log_event.cc` 的调用点据此报 `ER_XA_REPLICATION_FILTERS`。

### 规则的写入路径

`CHANGE REPLICATION FILTER` 的执行体是 `Sql_cmd_change_repl_filter::change_rpl_filter`，`Sql_cmd_change_repl_filter::execute` 只是转调它。整个流程有明确的先后顺序：

**第一步：权限检查。** 需要 `SUPER_ACL` 或全局权限 `REPLICATION_SLAVE_ADMIN`，否则报 `ER_SPECIFIC_ACCESS_DENIED_ERROR`（参数串是 `"SUPER or REPLICATION_SLAVE_ADMIN"`）。

**第二步：锁 `channel_map`。** `channel_map.rdlock()`，全程持有到函数末尾的 `err:` 标签处 `channel_map.unlock()`。

**第三步：分「带 FOR CHANNEL」与「不带」两条路。**

不带 `FOR CHANNEL` 时：

1. `channel_map.get_num_instances(true) == 0` → 报 `ER_REPLICA_CONFIGURATION` 失败。
2. 遍历所有通道，对每个已配置通道（`Master_info::is_configured(mi)`）先 `mysql_mutex_lock(&mi->rli->run_lock)` 锁住 SQL 线程，再用 `init_thread_mask(&thread_mask, mi, false)` 检查线程状态。
3. **`thread_mask & SLAVE_SQL` 非零就拒绝**：报 `ER_REPLICA_CHANNEL_SQL_THREAD_MUST_STOP`（带通道名），并且 `it_end = ++it;` 记下停点——注释说「Stop acquiring the run_locks once finding a SQL thread running and record the stop position.」。
4. 全部通过后，对每个通道的 `mi->rli->rpl_filter` 加写锁，依次调 `set_do_db` / `set_ignore_db` / `set_do_table` / `set_ignore_table` / `set_wild_do_table` / `set_wild_ignore_table` / `set_db_rewrite`。
5. 再对 `rpl_global_filter` 加写锁，做同一套七次调用，然后 `rpl_global_filter.reset_pfs_view()`。
6. 最后把从第 3 步记的 `it_end` 起逐个 `mysql_mutex_unlock(&mi->rli->run_lock)`。

**`init_thread_mask` 的第三个参数是 `false /*not inverse*/`**——即不取反，按 `SLAVE_SQL` 位的原始含义判定。

带 `FOR CHANNEL` 时：

1. 通道名是组复制通道 → 报 `ER_REPLICA_CHANNEL_OPERATION_NOT_ALLOWED`。
2. `channel_map.get_mi(lex->mi.channel)` 拿到通道，`!Master_info::is_configured(mi)` → 报 `ER_REPLICA_CONFIGURATION`。
3. 同样先 `mysql_mutex_lock(&mi->rli->run_lock)`，`init_thread_mask` 检查到 `thread_mask & SLAVE_SQL` → 报 `ER_REPLICA_CHANNEL_SQL_THREAD_MUST_STOP`。
4. 通过后只改这一个通道的 `mi->rli->rpl_filter`，七次 `set_*` 调用传的 `configured_by` 是 `CONFIGURED_BY_CHANGE_REPLICATION_FILTER_FOR_CHANNEL`（不带 FOR CHANNEL 的全局路径传的是 `CONFIGURED_BY_CHANGE_REPLICATION_FILTER`）。

**SQL 线程必须在停止状态**，是这套机制的一条硬约束——因为 `tables_ok` 读的是过滤器里的容器，运行中改规则会与正在应用的语句竞争。

**错误分支统一收敛到 `ER_OUTOFMEMORY`：** 两条路径里七次 `set_*` 用 `||` 串成一条链，第一个返回非 0 就短路，统一 `my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 0)`。

### `set_*` 系列的内部动作

每个 `set_*` 都遵循同一套模式：**先断言已持写锁**（`m_rpl_filter_lock->assert_some_wrlock();`），**参数为 `nullptr` 时直接返回 0**（`if (!do_db_list) return 0;`），**然后清空旧值、解析新值、更新统计口径**。

- `set_do_db` / `set_ignore_db`：`free_string_list` 清空旧链表 → `parse_filter_list(do_db_list, &Rpl_filter::add_do_db)` 解析 → `do_db_statistics.set_all(configured_by)`。
- `set_do_table` / `set_ignore_table`：**先 `delete do_table_hash` 并置 `do_table_hash_inited = false`**（精确表规则每次重建哈希），再 `free_string_array` 清数组，再 `parse_filter_list(..., &Rpl_filter::add_do_table_array)` 解析成数组，最后 `build_do_table_hash()` 建哈希。**建完还要再判一次空**：`if (do_table_hash_inited && do_table_hash->empty()) { delete do_table_hash; do_table_hash = nullptr; do_table_hash_inited = false; }`——**规则被全部清空时哈希容器要一并销毁**，避免留一个「已初始化但为空」的哈希。
- `set_wild_do_table` / `set_wild_ignore_table`：`free_string_array` 清数组 → `parse_filter_list` 解析 → **若 `wild_do_table.empty()` 则 `shrink_to_fit()` 并把 `wild_do_table_inited` 置回 false**。通配规则不建哈希，所以清理粒度是数组本身。
- `set_db_rewrite`：`free_string_pair_list` 清空 → **按「一次取两个」的方式解析**——注释说「grammar itself allows only even number of db values. So it is ok to do it++ twice without checking anything.」，因此 `Item *db_key = *it++; Item *db_val = *it++;` 不做任何越界检查。

**`parse_filter_list` 有两个重载**：`mem_root_deque<Item *>*` 版本对每个 `Item` 调 `item->val_str(&buf)->c_ptr()` 取字符串；`I_List<i_string>*` 版本直接取 `istr->ptr`。两者都通过成员函数指针 `Add_filter` 分派。

**`Add_filter` 的类型就是这四个 add 函数的指针**：`Rpl_filter::add_do_db` / `add_ignore_db` / `add_do_table_array` / `add_ignore_table_array` / `add_wild_do_table` / `add_wild_ignore_table` / `add_db_rewrite`。

### 统计口径与性能模式视图

每个规则集合都配一个 `Rpl_filter_statistics`（共七个），记录三样东西：**配置来源**（`enum_configured_by`）、**命中计数器**、**生效起始时间**。

`Rpl_filter_statistics::set_all` 设置配置来源并把计数器归零，同时算出生效时间：

```cpp
void Rpl_filter_statistics::set_all(enum_configured_by configured_by) {
  m_configured_by = configured_by;
  m_atomic_counter = 0;

  THD *thd = current_thd;
  if (thd == nullptr)
    m_active_since = my_micro_time();
  else {
    my_timeval stmt_start_time = thd->query_start_timeval_trunc(6);
    m_active_since =
        stmt_start_time.m_tv_sec * 1000000 + stmt_start_time.m_tv_usec;
  }
}
```

**`query_start_timeval_trunc(6)` 的 6 是微秒精度截断位**——结果是「自 1970-01-01 起的微秒数」。

`Rpl_filter_statistics::reset()` 把 `m_configured_by` 置回 `CONFIGURED_BY_STARTUP_OPTIONS`、计数器归零、`m_active_since` 归零。

**P_S 视图**：`sql/rpl_filter.cc` 的 `Rpl_filter::put_filters_into_vector` 依次检查七个统计对象的 `get_active_since() > 0`，**只有大于 0 的（即真正被设置过的）才进 P_S 视图**，并打上 `REPLICATE_DO_DB` / `REPLICATE_IGNORE_DB` / `REPLICATE_DO_TABLE` / `REPLICATE_IGNORE_TABLE` / `REPLICATE_WILD_DO_TABLE` / `REPLICATE_WILD_IGNORE_TABLE` / `REPLICATE_REWRITE_DB` 七个名字。全局视图由 `Rpl_global_filter::reset_pfs_view` 重建（传 `nullptr` 作为通道名，因为全局过滤器不挂通道）。

### 全量清空：`Rpl_filter::reset`

`Rpl_filter::reset` 是本页 refs 点名的第二个符号，析构函数 `Rpl_filter::~Rpl_filter` 也调它。函数体按顺序做三件事：

**第一步：销毁哈希容器并清所有 inited 标志。**

```cpp
if (do_table_hash_inited) delete do_table_hash;
if (ignore_table_hash_inited) delete ignore_table_hash;

do_table_hash_inited = false;
ignore_table_hash_inited = false;
do_table_array_inited = false;
ignore_table_array_inited = false;
wild_do_table_inited = false;
wild_ignore_table_inited = false;
table_rules_on = false;
```

**第二步：释放四类容器。** `free_string_array(&do_table_array)` / `free_string_array(&ignore_table_array)` / `free_string_array(&wild_do_table)` / `free_string_array(&wild_ignore_table)`，以及 `free_string_list(&do_db)` / `free_string_list(&ignore_db)` / `free_string_pair_list(&rewrite_db)`。`free_string_array` 内部是 `my_free_container_pointers(*a); a->shrink_to_fit();`。

**第三步：重置七个统计对象。** 逐个调 `reset()`。

**`reset` 的真实调用方**在 `sql/rpl_info_factory.cc`：启动时用户可能把 `--replicate-*` 参数打在一个尚未初始化的通道上，代码检测到 `!rli->rpl_filter->is_empty()` 就记 `LogErr(WARNING_LEVEL, ER_RPL_REPLICA_RESET_FILTER_OPTIONS, channel_name)` 并调 `rli->rpl_filter->reset()` ——**注释写明「The filter options will be reset with a warning.」**。

### `is_empty` 与复制判据

`Rpl_filter::is_empty` 的判据是**九个条件全为真**：六个 `*_inited` 标志全为假、`do_db.is_empty()`、`ignore_db.is_empty()`、`rewrite_db.is_empty()`。函数体首尾各加一次读锁（`rdlock()` / `unlock()`）。

这个判据被 `Rpl_filter::copy_global_replication_filters` 用作早退条件——`if (rpl_global_filter.is_empty()) return 0;`，全局规则为空时不做任何复制。

## 关键取舍

### 取舍一：精确表规则用哈希、通配表规则用线性数组

**取舍：** 精确规则走 `do_table_hash->count(std::string(hash_key, len))` 一次哈希查找；通配规则只能靠 `Rpl_filter::find_wild` 逐条 `my_wildcmp` 线性比对。

**代价：** 规则条数多时通配匹配是线性开销，而精确匹配是常数开销。**这是把「规则形态」直接映射到「数据结构」的必然结果**——通配语义无法哈希化。

### 取舍二：do 规则优先于 ignore 规则，且同形态内 do 优先

**取舍：** `tables_ok` 的分支顺序把 do 放在最前，`db_ok` 里 do 列表非空就完全不看 ignore 列表。

**代价：** **用户配了冲突规则时结果依赖规则形态，行为不直观**。源码在 `tables_ok` 上方注释里直接说了：「Note also order of precedence of do/ignore rules (see code). For that reason, users should not set conflicting rules because they may get unpredicted results (precedence order is explained in the manual).」

### 取舍三：库规则与表规则分属两套判定，互不替代

**取舍：** 库名过滤走 `db_ok` / `db_ok_with_wild_table`，表名过滤走 `tables_ok`，两者在 `sql/log_event.cc` 的 `check_table_map` 里是**「与」关系**（任一为假就丢弃）。

**代价：** 用户想「排除某个库的一切」必须选对规则类型——`binlog-ignore-db` 只挡库级语句，挡不住 `USE db2; UPDATE db1.t` 这种跨库操作。`sql/rpl_filter.cc` 的 `db_ok_with_wild_table` 注释给出了官方建议：用 `replicate_wild_ignore_table='db1.%'` 而不是 `binlog_ignore_db` / `replicate_ignore_db` 来安全排除一个库。

### 取舍四：`db_ok` 把「库名未选中」从「丢弃」改成「放行」

**取舍：** `if (!db) return true;`。源码注释说这个行为是被**替换**掉的旧行为，理由是「Since the filtering criteria is not equal to "NULL" the statement should be logged into binlog.」

**代价：** 库名未知的语句不再被库级规则拦下，需要靠表级规则补位。

### 取舍五：改规则要求 SQL 线程停止

**取舍：** `change_rpl_filter` 两条路径都先 `mysql_mutex_lock(&mi->rli->run_lock)`，再用 `init_thread_mask` 检查到 `thread_mask & SLAVE_SQL` 就报 `ER_REPLICA_CHANNEL_SQL_THREAD_MUST_STOP` 拒绝执行。

**代价：** 运维上必须先 `STOP REPLICA SQL_THREAD` 才能改过滤规则，**不能在复制进行中热改**。换来的是 `tables_ok` 读容器时不需要与写侧抢锁——它本身对容器没有任何加锁保护。

### 取舍六：全局规则向通道复制是「只填空缺」

**取舍：** `copy_global_replication_filters` 的每个分支都先判 `*_inited` 或 `is_empty()`，已有内容就不覆盖。

**代价：** **通道上只要有一条同类型规则，该类型的全局规则就完全不生效**（不是合并，是整类跳过）。同时复制是**单向的一次性动作**——全局规则之后发生变化不会自动同步到已配置的通道。

### 取舍七：规则集合的「空」要连容器一起销毁

**取舍：** `set_do_table` 里 `build_do_table_hash()` 之后还要判 `do_table_hash->empty()`，为空就 `delete do_table_hash` 并把 `do_table_hash_inited` 置回 false；`set_wild_do_table` 里为空则 `shrink_to_fit()` 并把 `wild_do_table_inited` 置回 false。

**代价：** 多了一层判断。换来的是 **`is_empty()` 与 `tables_ok` 的 `do_table_hash_inited` 判据一致**——`tables_ok` 里 `if (do_table_hash_inited)` 这个分支只在容器真的有内容时才进入，否则 `!do_table_hash_inited` 参与兜底表达式的运算。

### 取舍八：`db_ok` 的 do/ignore 列表互斥，导致 ignore 列表在有 do 列表时形同虚设

**取舍：** 代码结构是 `if (!do_db.is_empty()) { ...只查 do...; return; } else { ...只查 ignore...; }`。

**代价：** 同时配了 `REPLICATE_DO_DB` 和 `REPLICATE_IGNORE_DB` 时，**ignore 列表完全不参与判定**。这是源码结构（而非文档）的既定事实。

### 取舍九：错误统一收敛为 `ER_OUTOFMEMORY`

**取舍：** `change_rpl_filter` 里七次 `set_*` 用 `||` 串联，任一失败都报 `ER_OUTOFMEMORY`。`copy_global_replication_filters` 的 `err:` 标签同样只报 `ER_OUTOFMEMORY`。

**代价：** 失败原因被抹平——语法错误与内存不足在调用方看来是同一个错误码。源码用 `DBUG_EXECUTE_IF("simulate_out_of_memory_on_...")` 这类调试开关逐点模拟内存不足，是这条设计的可测性补丁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_filter.cc | `Rpl_filter::tables_ok` | 表级判定主入口：逐表遍历（跳过 `!tables->updating` 的表），按 do_table 哈希 → ignore_table 哈希 → wild_do_table → wild_ignore_table 的顺序短路返回；兜底返回 `some_tables_updating && !do_table_hash_inited && !wild_do_table_inited` |
| sql/rpl_filter.cc | `Rpl_filter::reset` | 全量清空：删两个哈希、六个 `*_inited` 标志全置假、`table_rules_on` 置假、释放四个数组与三个链表、重置七个统计对象；`Rpl_filter::~Rpl_filter` 也调它 |
| sql/rpl_filter.cc | `Rpl_filter::db_ok` | 库级判定：无库规则或库名为空直接放行；有 do 列表时只查 do 列表（未命中即丢弃），否则只查 ignore 列表（未命中即放行） |
| sql/rpl_filter.cc | `Rpl_filter::db_ok_with_wild_table` | 拿 `db` 加 `.` 去匹配通配表规则；兜底返回 `!wild_do_table_inited` |
| sql/rpl_filter.cc | `Rpl_filter::find_wild` | 线性遍历通配规则数组，逐条 `my_wildcmp(table_alias_charset, key, key_end, e->db, e->db + e->key_len, '\\', wild_one, wild_many)` |
| sql/rpl_filter.cc | `Rpl_filter::build_do_table_hash` / `build_ignore_table_hash` | 把动态数组搬进哈希表，搬完释放数组并把 `*_array_inited` 置假 |
| sql/rpl_filter.cc | `Rpl_filter::build_table_hash_from_array` | 逐条 `add_table_rule_to_hash`；`array_inited` 为假时直接返回 0 |
| sql/rpl_filter.cc | `Rpl_filter::add_table_rule_to_hash` | 单条规则入哈希：`strchr(table_spec, '.')` 找不到点返回 1；块大小 `sizeof(TABLE_RULE_ENT) + len` |
| sql/rpl_filter.cc | `Rpl_filter::add_table_rule_to_array` | 单条规则入数组：`strchr` 找不到点返回 1；`push_back` 失败则 `my_free` 并返回 1 |
| sql/rpl_filter.cc | `Rpl_filter::set_do_db` / `set_ignore_db` | 断言已持写锁 → 参数为空返回 0 → 清链表 → `parse_filter_list` → `statistics.set_all(configured_by)` |
| sql/rpl_filter.cc | `Rpl_filter::set_do_table` / `set_ignore_table` | 删旧哈希 → 清数组 → 解析成数组 → `build_*_table_hash` → 空则再删一次哈希 → 更新统计 |
| sql/rpl_filter.cc | `Rpl_filter::set_wild_do_table` / `set_wild_ignore_table` | 清数组 → 解析 → 空则 `shrink_to_fit()` 且 `*_inited` 置假 → 更新统计 |
| sql/rpl_filter.cc | `Rpl_filter::set_db_rewrite` | 清改写对列表 → 按「一次取两个」解析（语法保证偶数个值，注释说明无需越界检查）→ 更新统计 |
| sql/rpl_filter.cc | `Rpl_filter::parse_filter_list` | 两个重载：`mem_root_deque<Item *>*` 版走 `item->val_str(&buf)->c_ptr()`；`I_List<i_string>*` 版取 `istr->ptr`；都经成员函数指针 `Add_filter` 分派 |
| sql/rpl_filter.cc | `Rpl_filter::copy_global_replication_filters` | 全局规则下沉到通道：哈希转数组（`table_rule_ent_hash_to_array`）→ 重建哈希；数组转数组（`table_rule_ent_array_to_array`）；库规则与改写规则逐条复制；只填空缺 |
| sql/rpl_filter.cc | `Rpl_filter::is_empty` | 九个条件全真才算空（六个 `*_inited` 标志 + 三个链表），首尾各加一次读锁 |
| sql/rpl_filter.cc | `Rpl_filter::is_on` | 返回成员 `table_rules_on`，该标志只在四个表类 `add_*` 里被置真 |
| sql/rpl_filter.cc | `Rpl_filter::get_rewrite_db` | 按源库名比对返回目标库名；`rewrite_db` 为空或 `db` 为空时原样返回 |
| sql/rpl_filter.cc | `Rpl_filter::table_rule_ent_hash_to_str` | 把哈希里的规则按 `std::string(a->db, a->key_len)` 排序后拼成逗号分隔串（排序仅为测试确定性，非协议要求） |
| sql/rpl_filter.cc | `Rpl_filter::put_filters_into_vector` | 填 P_S 视图：七个统计对象中 `get_active_since() > 0` 的才进视图，并打上对应的 `REPLICATE_*` 名字 |
| sql/rpl_filter.cc | `Rpl_filter_statistics::set_all` | 记录配置来源、计数器归零、生效时间（无 THD 时用 `my_micro_time()`，有 THD 时用 `query_start_timeval_trunc(6)` 折算成微秒） |
| sql/rpl_filter.cc | `Sql_cmd_change_repl_filter::change_rpl_filter` | `CHANGE REPLICATION FILTER` 执行体：权限检查（`SUPER_ACL` 或 `REPLICATION_SLAVE_ADMIN`）→ `channel_map.rdlock()` → 分带/不带 `FOR CHANNEL` 两条路 → 每路都先锁 `mi->rli->run_lock` 并用 `init_thread_mask` 拒绝运行中的 SQL 线程 → 七次 `set_*` → 失败统一 `ER_OUTOFMEMORY` |
| sql/rpl_filter.cc | `Sql_cmd_change_repl_filter::set_filter_value` | 把解析出的 `Item` 列表按 `options_mysqld` 类型分发到七个成员列表；未知类型 `assert(0)` |
| sql/rpl_filter.cc | `Rpl_global_filter::reset_pfs_view` | 清空并重建全局过滤器的 P_S 视图（传 `nullptr` 通道名，因为全局过滤器不挂通道） |
| sql/sql_parse.cc | `all_tables_not_ok` | `tables_ok` 的包装层：要求 `is_on()`、`tables` 非空、且不在存储过程上下文（`!thd->sp_runtime_ctx`） |
| sql/sql_parse.cc | `check_database_filters` | 从库库级过滤入口：事务边界语句直接放行；`db_ok` 通过且无库规则时，`CREATE/ALTER/DROP DATABASE` 才走 `db_ok_with_wild_table` |
| sql/sql_parse.cc | `Query_block::set_lock_for_table` | 写锁时置 `table->updating = for_update`，注释说明这是为了让 `tables_ok` 知道哪些表被更新 |
| sql/log_event.cc | `check_table_map` | 从库表映射事件入口：`slave_thread` 且（`db_ok` 为假或 `is_on()` 且 `tables_ok` 为假）时过滤掉；XA 活跃态下返回 `FILTERED_WITH_XA_ACTIVE` |
| sql/mysqld.cc | `table_alias_charset` 赋值 | `lower_case_table_names` 非 0 取 `my_charset_utf8mb3_tolower_ci`，为 0 取 `my_charset_bin`；决定了所有规则名比对的大小写语义 |
| sql/mysqld.cc | `is_rpl_global_filter_setting` | 判断 `--replicate-*` 参数归属：参数串里不含 `:` 是全局规则，含 `:` 是通道规则 |
| sql/mysqld.cc | `parse_filter_arg` | 按第一个 `:` 把参数切成通道名与规则值（`':db1'` 切成空通道名 + `db1`） |
| sql/mysqld.cc | `rpl_global_filter` 初始化段 | `build_do_table_hash()` / `build_ignore_table_hash()` 失败记 `ER_CANT_HASH_DO_AND_IGNORE_RULES` 并返回 1；随后 `rpl_global_filter.reset_pfs_view()` |
| sql/rpl_info_factory.cc | 过滤器装配 | 组复制通道直接 `return false`；已配置通道调 `copy_global_replication_filters()`，失败记 `ER_RPL_REPLICA_GLOBAL_FILTERS_COPY_FAILED`；未配置通道但过滤器非空时记 WARNING `ER_RPL_REPLICA_RESET_FILTER_OPTIONS` 并调 `rli->rpl_filter->reset()` |

## 与主核 / AI 的关系

主核（`engine/db/`）与本页的复制过滤机制没有代码级关联——本页全部结论来自 `sql/rpl_filter.cc` 及其调用方。可对照的形态差异有三点。

- **过滤点放在「执行侧」而非「产生侧」。** MySQL 的复制过滤是在**从库应用事件之前**做判定（`sql/log_event.cc` 的 `check_table_map` 在 TABLE_MAP 事件上判、`sql/sql_parse.cc` 的 `check_database_filters` 在语句执行前判），过滤掉的表不会进入 `rli->tables_to_lock`。**架构推论：** 这意味着过滤发生在「已经收到了事件」之后，binlog 里仍保有完整事件；主核若要做 CDC 链路上的同类裁剪，裁剪点的选择（产生侧裁剪 vs 消费侧裁剪）会直接决定下游能否自证完整性——本页没有核对主核 CDC 实现，不作断言。
- **规则是「声明式白/黑名单 + 进程内内存结构」，不是「谓词下推」。** 七类规则编译成哈希表、动态数组和链表三种结构，匹配是精确串比对或 `my_wildcmp` 通配比对。**架构推论：** 这类规则无法表达「行级条件」「基于列值的谓词」；主核的写集跟踪（M06）与并行复制依赖判定是另一个量级的表达能力，两者不构成同一层。
- **配置变更要求 SQL 线程停止，是一条运维层面的硬约束。** `change_rpl_filter` 在两条路径上都先 `mysql_mutex_lock(&mi->rli->run_lock)`，再以 `init_thread_mask` 得到的 `thread_mask & SLAVE_SQL` 判定拒绝执行。**架构推论：** 若主核或 AI 侧的消费链路要求「不停机改规则」，那它必须自带一套比 `mi->rli->run_lock` 更细粒度的并发保护，这是本页记录的实现里没有的形态。

## 参考

- [How Servers Evaluate Replication Filtering Rules（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-rules.html) —— 复制过滤规则的官方总览与判定顺序，对应本页「机制是什么」的规则表与 `Rpl_filter::tables_ok` / `db_ok` 的分支顺序。
- [Evaluation of Table-Level Replication Options（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-rules-table-options.html) —— `REPLICATE_DO_TABLE` / `REPLICATE_IGNORE_TABLE` / `REPLICATE_WILD_DO_TABLE` / `REPLICATE_WILD_IGNORE_TABLE` 四条表级规则的官方语义，对应本页 `tables_ok` 的四个判定分支。
- [Evaluation of Database-Level Replication and Binary Logging Options（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-rules-db-options.html) —— `REPLICATE_DO_DB` / `REPLICATE_IGNORE_DB` 两条库级规则的官方语义，对应本页 `Rpl_filter::db_ok` 的「有 do 列表时只查 do 列表」判定。
- [Replication Channel Based Filters（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-rules-channel-based-filters.html) —— 通道级过滤器与 `REPLICATE_REWRITE_DB` 的官方语义，对应本页 `copy_global_replication_filters` 的全局规则下沉与 `get_rewrite_db` 的库名改写。
- [CHANGE REPLICATION FILTER Statement（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/change-replication-filter.html) —— 该语句的官方语法、权限要求与 `FOR CHANNEL` 语义，对应本页 `Sql_cmd_change_repl_filter::change_rpl_filter` 的两条执行路径。
- [GTID Format and Storage（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/replication-gtids-concepts.html) —— `mysql.gtid_executed` 表的官方结构（`source_id` / `interval_start` / `interval_end` 三列），是 M04 与本页共用的复制元数据出口，用于核对复制侧的整体版图。
- [Server System Variables（MySQL 8.4 Reference Manual）](https://dev.mysql.com/doc/refman/8.4/en/server-system-variables.html) —— `lower_case_table_names` 的官方取值与语义，对应本页 `table_alias_charset` 的大小写折叠规则。
- [sql/rpl_filter.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_filter.cc) —— 本页 refs 的唯一条目：`Rpl_filter::tables_ok` / `Rpl_filter::reset`，以及 `db_ok`、`db_ok_with_wild_table`、`find_wild`、`copy_global_replication_filters`、`set_*` 系列与 `Sql_cmd_change_repl_filter::change_rpl_filter` 的真实实现。
- [sql/sql_parse.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/sql_parse.cc) —— `all_tables_not_ok` 与 `check_database_filters` 两个过滤调用方，以及 `Query_block::set_lock_for_table` 对 `updating` 标志的设置。
- [sql/log_event.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/log_event.cc) —— `check_table_map` 的库级 + 表级联合判定与 `FILTERED_WITH_XA_ACTIVE` 分支。
- [sql/mysqld.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/mysqld.cc) —— `table_alias_charset` 的赋值时机、`is_rpl_global_filter_setting` 的全局/通道分派，以及全局过滤器哈希表的启动期构建。
- [sql/rpl_info_factory.cc（MySQL Server 8.4 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_info_factory.cc) —— `copy_global_replication_filters` 与 `Rpl_filter::reset` 的真实调用方，组复制通道的排除逻辑。
