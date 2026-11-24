---
project: mysql-server
feature: table-open-cache
page_type: feature
schema_version: 1
refs:
  - path: sql/table_cache.cc
    symbols: [Table_cache::init, Table_cache_manager::cached_tables]
  - path: sql/sql_base.cc
    symbols: [open_table_from_share]
---

# 表打开与表缓存

## 一句话职责

把一份表定义（`TABLE_SHARE`）实例化成一份可用的 `TABLE`（含 handler、字段、记录缓冲），并把用完的 `TABLE` 留在服务器层的表缓存里供后续语句复用，同时在表定义变化或缓存失效时把旧实例作废。

## 机制是什么

MySQL 服务器层有两级表缓存，**不是一套**：

- **表定义缓存（table definition cache）**：缓存 `TABLE_SHARE`。全局只有一个，由 `sql/sql_base.cc` 里的全局变量 `table_def_cache`（类型 `Table_definition_cache`）持有，键是 `db\0table` 形式的 `table_cache_key`。
- **表打开缓存（table open cache）**：缓存 `TABLE` 实例。**分片**存放，`sql/table_cache.cc` 里的全局对象 `table_cache_manager`（`Table_cache_manager`）持有 `m_table_cache[]` 数组，每个元素是一个 `Table_cache`，每个 `Table_cache` 自带一把互斥锁 `m_lock`（PSI 名为 `LOCK_table_cache`）。

分片的份数由全局变量 `table_cache_instances` 决定（声明在 `sql/mysqld.cc`，类型 `ulong`）。每个 `TABLE_SHARE` 都带一个 `cache_element[]` 数组（长度 `table_cache_instances`），在 `sql/table.cc` 的 `alloc_table_share()` 里与 `TABLE_SHARE` 一起分配并清零——即同一张表的 `TABLE` 实例可以分布在不同分片里。

缓存中的 `TABLE` 按使用状态分成两组链表：`used_tables`（正在被某个线程使用）与 `free_tables`（空闲、可复用）。空闲表还有一条跨分片的全局链表 `m_unused_tables` 串起来，用于在超限时按 LRU 顺序淘汰——`Table_cache::check_unused()` 里 `m_unused_tables` 的遍历计数上限就是 `m_table_count`。

命中与未命中由两个状态变量记录：`thd->status_var.table_open_cache_hits`（命中）与 `thd->status_var.table_open_cache_misses`（未命中）。这两个计数器在 `sql/mysqld.cc` 里注册为 Performance Schema 的 `Table_open_cache_hits` / `Table_open_cache_misses` 状态项；同处还注册了 `Table_open_cache_overflows`。

**边界**：本机制负责 `TABLE_SHARE` → `TABLE` 的实例化与 `TABLE` 的复用/失效；不负责存储引擎如何打开文件（那是 `handler::ha_open` 的事，见 F01），也不负责 MDL 锁的申请与等待（见 F03 元数据锁页）。`sql/table_cache.cc` 的注释明确写着调用方「应当持有 `LOCK_open` 与所有表缓存实例的锁」。

## 怎么实现的

### 1. 缓存的建立与销毁

`Table_cache::init()` 是单个分片的初始化：初始化 `m_lock`（`MY_MUTEX_INIT_FAST`）、把 `m_unused_tables` 置空、`m_table_count` 置 0，恒定返回 `false`（失败语义与 MySQL 其它 `init()` 约定相反——注释写明 `@retval false - success`）。

`Table_cache_manager::init()` 遍历 `m_table_cache[i]`（`i < table_cache_instances`）逐个调用 `Table_cache::init()`；一旦某个分片失败，就把已建好的分片 `m_table_cache[i].destroy()` 并返回 `true`。销毁路径是 `Table_cache_manager::destroy()` → 每个分片的 `Table_cache::destroy()`（`mysql_mutex_destroy(&m_lock)`）。

`table_def_init()`（在 `sql/sql_base.cc`）是服务器启动时的总入口：先 `mysql_mutex_init(key_LOCK_open, &LOCK_open, ...)` 与 `mysql_cond_init(key_COND_open, &COND_open)`，再调 `table_cache_manager.init()`；失败时回滚 `COND_open`/`LOCK_open` 并返回 `true`；成功后 `table_def_cache = new Table_definition_cache(key_memory_table_share)`。`table_def_free()` 反向销毁：先 `delete table_def_cache` 并置空，再 `table_cache_manager.destroy()`，最后销毁 `COND_open` 与 `LOCK_open`。

### 2. 分片锁与表定义缓存锁的成对获取

`Table_cache_manager::lock_all_and_tdc()` 先对每个分片 `m_table_cache[i].lock()`，再 `mysql_mutex_lock(&LOCK_open)`；`unlock_all_and_tdc()` 反序释放（先放 `LOCK_open`，再放各分片锁）。**这个顺序是全局统一的**，所以任何需要同时访问 `TABLE_SHARE` 与 `TABLE` 的路径都必须走这一对函数。

`Table_cache_manager::assert_owner_all_and_tdc()` 断言持有全部分片锁**且**持有 `LOCK_open`（`mysql_mutex_assert_owner(&LOCK_open)`）；`assert_owner_all()` 只断言分片锁；`assert_owner(THD *thd)` 通过 `get_cache(thd)` 取到本线程所属分片再断言其单锁。

### 3. 打开路径：命中与未命中

入口是 `sql/sql_base.cc` 的 `open_table()`。它先用 `get_table_def_key(table_list, &key)` 生成缓存键——该函数直接复用 MDL 子系统的键对象（源码注释说明表定义缓存键是 MDL 键的后缀），因此键的生成不额外分配内存。若请求了 secondary storage engine，键换成 `create_table_def_key_secondary()` 的结果（在 `db\0table` 后追加一个值为 1 的字节，用于区分主表与影子表）。

接着进入 `retry_share:` 标签：取 `Table_cache *tc = table_cache_manager.get_cache(thd)` 并 `tc->lock()`，然后：

- **命中**（`tc->get_table(thd, key, key_length, &share)` 返回非空的 `table`）：拿到一个空闲 `TABLE`。若未带 `MYSQL_OPEN_IGNORE_FLUSH` 标志，先 `assert(!share->has_old_version())`，再比较 `thd->open_tables->s->version() != share->version()`——若本语句已打开的表的版本与本次取到的不一致，说明期间发生过刷新，于是 `tc->release_table(thd, table)` 把它退回、`tc->unlock()`，并 `ot_ctx->request_backoff_action(Open_table_context::OT_REOPEN_TABLES, nullptr)` 请求回退重开，直接返回失败。版本一致则 `tc->unlock()`，随后在临界区外调 `table->file->rebind_psi()` 与 `table->file->ha_extra(HA_EXTRA_RESET_STATE)`，`thd->status_var.table_open_cache_hits++`，跳转到 `table_found:` 复用这个 `TABLE`。**注意：命中路径不会调用 `open_table_from_share()`。**
- **有 share 无 table**（`get_table()` 只填了 `share`）：说明表定义在缓存里但没有空闲实例。此时 `mysql_mutex_lock(&LOCK_open)`、`tc->unlock()`、`share->increment_ref_count()`，跳转到 `share_found:` 去新建实例。
- **两者皆无**：`tc->unlock()` 后 `mysql_mutex_lock(&LOCK_open)`，调 `get_table_share_with_discover()` 从表定义缓存或磁盘取 `TABLE_SHARE`。

`share_found:` 处若 `share->has_old_version()` 为真（除非带 `MYSQL_OPEN_IGNORE_FLUSH`），则 `release_table_share(share)`、`mysql_mutex_unlock(&LOCK_open)`，装上 `MDL_deadlock_handler`，调 `tdc_wait_for_old_version()` 等待旧版本消失；等待失败返回错误，成功则 `goto retry_share` 重试。死锁权重取 `ot_ctx->can_back_off() ? MDL_wait_for_subgraph::DEADLOCK_WEIGHT_DML : mdl_ticket->get_deadlock_weight()`。

### 4. 未命中：`open_table_from_share()` 的实际工作

`open_table_from_share()` 定义在 `sql/table.cc`（不在 `sql/sql_base.cc`——`sql/sql_base.cc` 里只有四处调用）。签名：

```
int open_table_from_share(THD *thd, TABLE_SHARE *share, const char *alias,
                          uint db_stat, uint prgflag, uint ha_open_flags,
                          TABLE *outparam, bool is_create_table,
                          const dd::Table *table_def_param)
```

返回码有语义差别：`0` 成功；`1`/`2`/`4` 是错误（分别对应 `open_table_error()` 里的「表不存在/表空间缺失」「打不开文件」「不是有效表定义」三档）；**`7` 表示引擎里表定义已变，`8` 表示引擎里行格式已变**。

函数体按顺序做这些事：

1. `new (outparam) TABLE()` 就地构造，`outparam->in_use = thd`、`outparam->s = share`、`outparam->db_stat = db_stat`、`outparam->write_row_record = nullptr`。错误码初值设为 `1`。
2. 分配内存根：非内部临时表用 `&outparam->mem_root` 并 `init_sql_alloc(key_memory_TABLE, root, TABLE_ALLOC_BLOCK_SIZE)`；内部临时表用 `&share->mem_root`。别名 `alias` 在内部临时表下用 `strdup_root(root, alias)`，否则用 `my_strdup(key_memory_TABLE, alias, MYF(MY_WME))`。
3. 初始化 5 个 `Bitmap`（`quick_keys`、`possible_quick_keys`、`covering_keys`、`merge_keys`、`keys_in_use_for_query`）。
4. **分配 handler**：若 `prgflag` 不带 `SKIP_NEW_HANDLER`，则 `get_new_handler(share, share->m_part_info != nullptr, root, share->db_type())` 拿到 `outparam->file`，再 `outparam->file->set_ha_share_ref(&share->ha_share)`。带 `SKIP_NEW_HANDLER` 时 `assert(!db_stat)`（不分配 handler 就没有可统计的表）。
5. 错误码推进到 `4`；`outparam->reginfo.lock_type = TL_UNLOCK`、`outparam->current_lock = F_UNLCK`。
6. 记录缓冲分配：记录条数 `records` 的算法是——`(db_stat & HA_OPEN_KEYFILE)` 或 `(prgflag & DELAYED_OPEN)` 为真则 `records = 1`；`prgflag & (READ_ALL + EXTRA_RECORD)` 为真则再 `records++`。缓冲大小是 `share->rec_buff_length * records + share->null_bytes`。若 `records == 0`（源码注释：此时可能处于硬修复，缓冲不应被使用），`outparam->record[0] = outparam->record[1] = share->default_values` 且置 `has_default_values = true`；否则 `record[0]` 指向缓冲、`records > 1` 时 `record[1]` 指向 `record + share->rec_buff_length`，`records == 1` 时 `record[1] = record[0]`（源码注释标为 Safety）。NULL 位图位置为 `record + records * share->rec_buff_length`，并 `memset` 清零 `share->null_bytes` 字节。
7. 字段克隆：对 `share->fields` 个字段逐个 `share->field[i]->clone(root)`，然后 `new_field->init(outparam)`、`new_field->move_field_offset(move_offset)`；`move_offset` 为 `outparam->record[0] - share->default_values`（内部临时表再额外加 `2 * share->rec_buff_length`）。克隆时顺带检测全文索引文档列：若 `outparam->file->ha_table_flags()` 带 `HA_CAN_FULLTEXT_EXT` 且字段名等于 `FTS_DOC_ID_COL_NAME`，则记为 `fts_doc_id_field`。
8. 键部分字段：`share->key_parts` 非空时调 `create_key_part_field_with_prefix_length()`；若键带 `HA_FULLTEXT` 标志则把 `outparam->fts_doc_id_field` 指向 `fts_doc_id_field`。
9. 分区：`share->partition_info_str_len` 且有 `outparam->file` 时调 `unpack_partition_info(thd, outparam, share, share->m_part_info->default_engine_type, is_create_table)`；若此时 `is_create_table` 为真则置 `error_reported = true`（源码注释：CREATE/ALTER TABLE 期间报错是允许的，普通查询打开表时不是）。
10. 生成列能力检查：`share->vfields` 非空但引擎不带 `HA_GENERATED_COLUMNS` 时，报 `ER_UNSUPPORTED_ACTION_ON_GENERATED_COLUMN`。
11. **位图**：`bitmap_size = share->column_bitmap_size`，一次性分配 `bitmap_size * 8` 字节，依次 `bitmap_init` 出 `def_read_set`、`def_write_set`、`tmp_set`、`cond_set`、`def_fields_set_during_insert`、`fields_for_functional_indexes`、`pack_row_tmp_set`、`read_set_internal` 共 8 个位图，再调 `outparam->default_column_bitmaps()`。源码注释说明位图必须先于生成列分配，因为生成列会调 `fix_fields`，而函数可能要看这些位图。
12. 生成列与默认表达式、检查约束：`unpack_value_generator()` 分别以 `VGS_GENERATED_COLUMN`、`VGS_DEFAULT_EXPRESSION`、`VGS_CHECK_CONSTRAINT` 三种模式展开。引擎能力检查有两处：引擎不带 `HA_GENERATED_COLUMNS` 时报生成列错误；引擎不带 `HA_SUPPORTS_DEFAULT_EXPRESSION` 时报 `ER_UNSUPPORTED_ACTION_ON_DEFAULT_VAL_GENERATED`。
13. **真正打开引擎文件**：错误码推进到 `2`。若 `db_stat` 非零，先取 `dd::Table`（`table_def_param` 为空时调 `thd->dd_client()->acquire(share->db.str, share->table_name.str, &table_def)`；取不到则 `error = 1` 且 `set_my_errno(ENOENT)`），然后调 `outparam->file->ha_open(outparam, share->normalized_path.str, (db_stat & HA_READ_ONLY ? O_RDONLY : O_RDWR), <打开模式>, table_def)`。打开模式按 `HA_OPEN_TEMPORARY` → `HA_OPEN_TMP_TABLE`、`HA_WAIT_IF_LOCKED` → `HA_OPEN_WAIT_IF_LOCKED`、`HA_ABORT_IF_LOCKED|HA_GET_INFO` → `HA_OPEN_ABORT_IF_LOCKED`、否则 `HA_OPEN_IGNORE_IF_LOCKED` 逐级择一，再或上 `ha_open_flags`。若 `db_stat` 为零但有 `outparam->file`，则只调 `outparam->file->change_table_ptr(outparam, share)`。
14. **打开失败的分支**：`ha_open()` 返回非零时，先置 `share->crashed`（条件是 `ha_err == HA_ERR_CRASHED_ON_USAGE` 且 `outparam->file->auto_repair()` 且 `!(ha_open_flags & HA_OPEN_FOR_REPAIR)`），再按 `ha_err` 分支：`HA_ERR_TABLESPACE_MISSING` → `error = 1`；`HA_ERR_NO_SUCH_TABLE` → `error = 1` 且 `set_my_errno(ENOENT)`（源码注释：引擎里没有这张表，报错与 .frm 不存在时相同）；`EMFILE` → `error = 1` 且 `set_my_errno(EMFILE)`（源码注释：打开的文件过多，报错与 .frm 打不开时相同）；其余走 `default`，调 `outparam->file->print_error(ha_err, MYF(0))` 并置 `error_reported = true`，其中 `ha_err == HA_ERR_TABLE_DEF_CHANGED` 时 `error = 7`，`ha_err == HA_ERR_ROW_FORMAT_CHANGED` 时 `error = 8`。
15. 复制标志：`share->table_category` 为 `TABLE_CATEGORY_LOG`/`TABLE_CATEGORY_RPL_INFO`/`TABLE_CATEGORY_GTID` 时置 `outparam->no_replicate = true`；否则按 `ha_table_flags()` 判——引擎既不带 `HA_BINLOG_STMT_CAPABLE` 也不带 `HA_BINLOG_ROW_CAPABLE`，或带 `HA_HAS_OWN_BINLOGGING`，则 `no_replicate = true`。
16. `if (db_stat) thd->status_var.opened_tables++;`——**只有真正打开引擎文件才计数**。返回 `0`。

出错路径（`err:` 标签）：若 `!error_reported` 则调 `open_table_error(thd, share, error, my_errno())`；随后 `destroy(outparam->file)`，并依次释放分区项、生成列项、默认表达式项、检查约束项的 `item_list`；把 `outparam->file` 置空、`outparam->db_stat = 0`（源码注释：便于错误检查）；非内部临时表则 `root->Clear()`；最后 `my_free(outparam->alias)` 并返回 `error`。

### 5. 回到 `open_table()`：把新实例挂进缓存

`open_table_from_share()` 返回后，`sql/sql_base.cc` 的 `open_table()` 继续：

- 返回值非零时 `destroy(table)` + `my_free(table)`，并按错误码分派回退动作：`error == 7` → `OT_DISCOVER`；`error == 8` → `OT_FIX_ROW_TYPE`；`share->crashed` → `OT_REPAIR`。然后跳到 `err_lock:`。
- 返回值为零但 `share->crashed` 为真时，按 `thd->lex->sql_command` 判定：`SQLCOM_ALTER_TABLE`、`SQLCOM_REPAIR`、`SQLCOM_CHECK`、`SQLCOM_SHOW_CREATE` 放行；其余一律 `closefrm(table, false)` + `destroy` + `my_free` 并报 `ER_CRASHED_ON_USAGE`。
- 再调 `open_table_entry_fini(thd, share, table_def, table)`（负责装载触发器等），失败则 `closefrm` + `destroy` + `my_free`。
- 挂入缓存：`Table_cache *tc = table_cache_manager.get_cache(thd)`、`tc->lock()`、`tc->add_used_table(thd, table)`；失败则 `tc->unlock()` 并跳 `err_lock:`。
- `thd->status_var.table_open_cache_misses++;` —— **未命中计数在这里**。

`table_found:` 段是命中与未命中两条路径的汇合点：`table->mdl_ticket = mdl_ticket`；`table->next = thd->open_tables` 并 `thd->set_open_tables(table)`（挂到本线程的打开表链表）；`table->reginfo.lock_type = TL_READ`（源码注释：假定为读锁）；`reset:` 标签处 `table->reset()`、`table->set_created()`、`assert(table->file->pushed_cond == nullptr)`；`table_list->set_updatable()`、`table_list->set_insertable()`、`table_list->table = table`；分区位图在非 `MYSQL_OPEN_NO_NEW_TABLE_IN_SE` 模式下由 `table->part_info->set_partition_bitmaps(table_list)` 设置，若表未分区却带了 `PARTITION ()` 子句则报 `ER_PARTITION_CLAUSE_ON_NONPARTITIONED`；最后 `table->init(thd, table_list)`，并在 `in_LTM(thd)` 且锁类型为 `F_UNLCK` 且属于 P_S 表时补一次 `ha_external_lock(thd, F_RDLCK)`。

### 6. 释放：回缓存还是直接关掉

`close_thread_table(THD *thd, TABLE **table_ptr)` 负责把单个 `TABLE` 从本线程链表摘下。它先断言 `table->key_read == 0` 与 `table->file->inited == handler::NONE`，断言持有该表的 `MDL_SHARED` 以上元数据锁（源码注释：元数据锁必须在把表还给表缓存**之后**才释放），并把 `table->mdl_ticket` 与 `table->pos_in_table_list` 置空。摘链在 `thd->LOCK_thd_data` 保护下做。随后若 `!table->has_invalid_dict()`，则 `table->file->ha_extra(HA_EXTRA_DETACH_CHILDREN)`、`free_blob_buffers_and_reset(table, MAX_TDC_BLOB_SIZE)`、`table->file->ha_reset()`；再 `table->file->unbind_psi()`（源码注释：必须在进入 `LOCK_open` 临界区之前做），最后调 `release_or_close_table()`。

**核心决策在 `release_or_close_table(THD *thd, TABLE *table)`**：取本线程的分片 `tc`，`tc->lock()`，然后判断

```
if (table->s->has_old_version() || table->has_invalid_dict() ||
    table->has_invalid_stats() || table_def_shutdown_in_progress) {
  tc->remove_table(table);
  mysql_mutex_lock(&LOCK_open);
  intern_close_table(table);
  mysql_mutex_unlock(&LOCK_open);
} else
  tc->release_table(thd, table);
```

即：**表定义已过期、字典无效、统计信息无效、或服务器正在关停**（`table_def_shutdown_in_progress`）→ 直接销毁；否则**退回缓存等待复用**。`intern_close_table()` 的实际动作是 `free_io_cache(table)`、`destroy(table->triggers)`、有 `table->file` 时 `closefrm(table, true)`、`destroy(table)`、`my_free(table)`。

### 7. 缓存失效与淘汰

`Table_cache_manager::free_table(THD *thd, enum_tdc_remove_table_type remove_type, TABLE_SHARE *share)` 按类型清理。它先 `assert_owner_all_and_tdc()`，然后**把 `share->cache_element[]` 整个数组 memcpy 到栈上**（源码注释：释放最后一个 `TABLE` 实例会销毁 share 及其 `cache_element[]` 数组，把数组复制到栈上可以让遍历在 share 中途被销毁时仍然安全）。随后对每个分片：若 `remove_type == TDC_RT_REMOVE_ALL` 则 `assert(cache_el[i]->used_tables.is_empty())`；若 `remove_type` 是 `TDC_RT_REMOVE_NOT_OWN` 或 `TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE`，则遍历 `used_tables` 断言每个 `table->in_use == thd`；若 `remove_type == TDC_RT_MARK_FOR_REOPEN`，则遍历 `used_tables` 对每个表调 `table->invalidate_stats()`。最后把该分片 `free_tables` 里的每个表 `m_table_cache[i].remove_table(table)` + `intern_close_table(table)`。

`Table_cache::free_all_unused_tables()` 沿 `m_unused_tables` 链表把每个空闲表 `remove_table()` + `intern_close_table()`。`Table_cache_manager::free_all_unused_tables()` 对每个分片调它。

`tdc_remove_table(THD *thd, enum_tdc_remove_table_type remove_type, const char *db, const char *table_name, bool has_lock)`（`sql/sql_base.cc`）是按表名移除的入口：未预先持锁则 `table_cache_manager.lock_all_and_tdc()`，否则 `table_cache_manager.assert_owner_all_and_tdc()`；断言除 `TDC_RT_REMOVE_UNUSED` 与 `TDC_RT_MARK_FOR_REOPEN` 外，调用方必须持有该表的 `MDL_EXCLUSIVE` 锁。键由 `create_table_def_key(db, table_name, key)` 生成，在 `table_def_cache` 里查找；若表有 secondary storage engine 的影子副本或不知道有没有，则**也要一并尝试移除影子表**（用 `create_table_def_key_secondary()` 的键再查一次）。移除时若 `share->ref_count() > 0`，则除 `TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE` 与 `TDC_RT_MARK_FOR_REOPEN` 外调 `share->clear_version()`（把版本置零使其在无人引用后自动删除），再 `table_cache_manager.free_table(thd, remove_type, share)`；若 `ref_count() == 0` 则直接从 `table_def_cache` 里 `erase`。

**淘汰策略在 `release_table_share(THD *shash *share)` 里**：`LOCK_open` 保护下，`share->decrement_ref_count() == 0` 时，若 `share->has_old_version()` 或 `table_def_shutdown_in_progress` 则直接从 `table_def_cache` 里 `erase`；否则挂到 `end_of_unused_share` 链尾（即 `unused` 链表尾部，`oldest_unused_share` 是链头），并在 `table_def_cache->size() > table_def_size` 时 `erase` 链头那个（源码注释：删除最久未用的 share 以保持 LRU 顺序）。`table_def_size` 由 `sql/mysqld.cc` 的 `adjust_table_def_size()` 设定：默认值为 `min<ulong>(400 + table_cache_size / 2, 2000)`，并写回 `table_definition_cache` 系统变量的默认值；若用户未显式指定该变量（`table_definition_cache_specified` 为假），则 `table_def_size = default_value`。

`close_cached_tables(THD *thd, Table_ref *tables, bool wait_for_refresh, ulong timeout)` 是「关掉所有空闲表」的入口：`tables` 为空时 `refresh_version++`（源码注释：递增全局刷新版本号），再 `table_cache_manager.free_all_unused_tables()`，然后 `while (oldest_unused_share->next)` 逐个 `table_def_cache->erase` 掉没被隐式释放的 share；`tables` 非空时对每个表 `tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED, ...)`。另有 `tdc_flush_unused_tables()`，其注释写明是给 `handle_manager` 周期性清空所有未使用表用的。

### 8. 缓存大小的计算

表打开缓存的总容量由全局变量 `table_cache_size` 决定，`sql/mysqld.cc` 的 `adjust_table_cache_size(ulong requested_open_files)` 负责调整：

```
limit = max<ulong>((requested_open_files - 10 - max_connections) / 2, TABLE_OPEN_CACHE_MIN);
```

其中 `TABLE_OPEN_CACHE_MIN` 是同文件里的 `static constexpr const ulong`，值为 `400`。若算出的 `limit < table_cache_size`，则记 `ER_CHANGED_TABLE_OPEN_CACHE` 警告日志并把 `table_cache_size` 改成 `limit`。最后 `table_cache_size_per_instance = table_cache_size / table_cache_instances`。

同文件里的 `adjust_open_files_limit()` 反向依赖表缓存：`limit_1 = 10 + max_connections + table_cache_size * 2`（源码注释：MyISAM 每张表需要两个文件句柄），`limit_2 = max_connections * 5`，`limit_3 = open_files_limit ? open_files_limit : 5000`，三者取最大作为请求值。`adjust_max_connections()` 则用 `TABLE_OPEN_CACHE_MIN * 2` 反推最大连接数上限。

### 9. 统计与自检

`Table_cache_manager::cached_tables()` 把各分片的 `m_table_cache[i].cached_tables()` 累加返回；源码注释说明它**不要求获取表缓存锁**，因为它给出的是个近似值（`Doesn't require acquisition of table cache locks if inexact number of tables is acceptable`）。注意它统计的是「已用 + 未用」的 `TABLE` 对象总数。

`Table_cache::check_unused()`（仅 `EXTRA_DEBUG` 编译）校验三件事：`m_unused_tables` 链表的双向指针是否正确（`cur_link != cur_link->next->prev`）；空闲链表里的表必须有 `db_stat` 与 `file`（源码注释：空闲链表里不该有已关文件的表），且 `entry->in_use` 必须为空；使用链表里的表 `entry->in_use` 必须非空。最后比对计数与 `m_table_count` 是否一致。

`Table_cache::print_tables()`（仅 `NDEBUG` 未定义时）逐表打印库名、表名、`share->version()`、占用线程 id、是否有 `db_stat` 以及 `lock_descriptions[entry->reginfo.lock_type]`。

## 关键取舍

- **`TABLE_SHARE` 与 `TABLE` 分两级缓存**：表定义缓存全局单锁 `LOCK_open`，表打开缓存分片各带一把锁。代价是打开一张表要同时涉及两套锁的序（源码注释在 `sql/table_cache.cc` 里明确要求「持有 `LOCK_open` 和所有表缓存实例的锁」）；收益是不同表可以由不同线程并行打开，不必争抢同一把全局锁。
- **命中路径完全不走 `open_table_from_share()`**：命中时只做 `rebind_psi()` + `ha_extra(HA_EXTRA_RESET_STATE)` 两个轻量动作，不重新克隆字段、不重新分配记录缓冲。代价是缓存里的 `TABLE` 必须能被安全复用（源码用 `assert(table->file != nullptr)` 与 `assert(table->file->pushed_cond == nullptr)` 兜底），收益是重复查询省掉整个实例化开销。
- **版本比对失败即回退重开**：即使命中了空闲表，只要 `thd->open_tables->s->version() != share->version()`，就退回该表并请求 `OT_REOPEN_TABLES`。源码注释解释这是为了处理「并发 flush 导致已打开的表变旧」的情况。
- **空闲表复用前必须过版本与有效性检查**：`release_or_close_table()` 里四项条件（`has_old_version()`、`has_invalid_dict()`、`has_invalid_stats()`、`table_def_shutdown_in_progress`）任一成立就直接销毁而不是回缓存。这是「宁可少一次复用，也不能复用一张已经失效的表」的取舍。
- **销毁 `TABLE_SHARE` 时先 memcpy 缓存元素数组到栈**：`Table_cache_manager::free_table()` 的注释说明，释放最后一个 `TABLE` 实例会连带销毁 share 及其 `cache_element[]` 数组，把数组复制到栈上才能保证遍历中途 share 消失也不出错。代价是一次数组拷贝，收益是消除了迭代器失效。
- **`TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE` 不清版本号**：`tdc_remove_table()` 里 `TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE` 与 `TDC_RT_MARK_FOR_REOPEN` 都不调 `share->clear_version()`，即保留 share 的版本信息。源码注释说明 `clear_version()` 的目的是让 share 在无人引用后自动删除，而这两个类型要求 share 继续存在。
- **表定义缓存按 LRU 淘汰，表打开缓存按空闲链表淘汰**：前者用 `oldest_unused_share` 链头（`release_table_share()`），后者用 `m_unused_tables` 链表（`Table_cache::free_all_unused_tables()`）。两条淘汰路径各自独立。
- **打开引擎失败时统一伪装成「表不存在」**：`HA_ERR_NO_SUCH_TABLE` 与 `EMFILE` 分别被映射成 `ENOENT` 与 `EMFILE`，最终由 `open_table_error()` 转成 `ER_NO_SUCH_TABLE` / `ER_CANT_OPEN_FILE`。源码注释明确说这是为了给出与 .frm 文件不存在时相同的错误消息。
- **内部临时表共用 `TABLE_SHARE` 的 mem_root**：`open_table_from_share()` 里 `internal_tmp` 为真时 `root = &share->mem_root`、别名为 `strdup_root`，字段偏移还要额外加 `2 * share->rec_buff_length`。源码注释解释这样「释放更简单」。
- **命中/未命中用会话级状态变量计数**：`table_open_cache_hits` / `table_open_cache_misses` 挂在 `thd->status_var` 上；`Table_open_cache_overflows` 也在 `sql/mysqld.cc` 注册为状态项，但本地检出的 `sql/` 源码里没有它的自增点（`sql/sql_base.cc` 与 `sql/table_cache.cc` 中均未见），故本页不描述其触发条件。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/table_cache.cc | `Table_cache::init` | 单个分片初始化：初始化 `m_lock`、`m_unused_tables` 置空、`m_table_count` 置 0，恒返回 `false`（成功） |
| sql/table_cache.cc | `Table_cache_manager::init` | 遍历 `m_table_cache[i]`（`i < table_cache_instances`）逐个 `init()`，任一失败则销毁已建分片并返回 `true` |
| sql/table_cache.cc | `Table_cache_manager::cached_tables` | 累加各分片的 `cached_tables()`，返回已用 + 未用 `TABLE` 对象总数；注释说明不要求加表缓存锁（值为近似） |
| sql/table_cache.cc | `Table_cache_manager::lock_all_and_tdc` | 先锁全部分片再 `mysql_mutex_lock(&LOCK_open)`，全局统一的加锁顺序 |
| sql/table_cache.cc | `Table_cache_manager::unlock_all_and_tdc` | 与 `lock_all_and_tdc` 反序解锁：先放 `LOCK_open`，再放各分片锁 |
| sql/table_cache.cc | `Table_cache_manager::assert_owner_all_and_tdc` | 断言持有全部分片锁且持有 `LOCK_open` |
| sql/table_cache.cc | `Table_cache_manager::free_table` | 按 `enum_tdc_remove_table_type` 清理某 share 的 `TABLE` 实例；先把 `share->cache_element[]` memcpy 到栈再遍历；`TDC_RT_MARK_FOR_REOPEN` 时对使用中的表调 `invalidate_stats()` |
| sql/table_cache.cc | `Table_cache::free_all_unused_tables` | 沿 `m_unused_tables` 链表把每个空闲表 `remove_table()` + `intern_close_table()` |
| sql/table_cache.cc | `Table_cache_manager::free_all_unused_tables` | 对每个分片调 `Table_cache::free_all_unused_tables()` |
| sql/table_cache.cc | `Table_cache::check_unused` | 仅 `EXTRA_DEBUG` 编译：校验空闲链表指针、免费表的 `db_stat`/`file` 非空、使用链表的 `in_use` 非空，并与 `m_table_count` 比对 |
| sql/table_cache.cc | `Table_cache::print_tables` | 调试打印：库名、表名、`share->version()`、占用线程 id、锁类型（`lock_descriptions`） |
| sql/sql_base.cc | `open_table` | 打开一张表的总入口：算缓存键、取 MDL 锁、查表缓存（命中即复用），未命中则建实例并挂入缓存 |
| sql/sql_base.cc | `open_table_from_share` | `TABLE_SHARE` → `TABLE` 的实例化：分配 handler、克隆字段、分配记录缓冲与 8 个位图、展开生成列/默认表达式/检查约束，最后 `ha_open()` 打开引擎文件；返回码 7/8 分别表示引擎内表定义/行格式已变 |
| sql/sql_base.cc | `close_thread_table` | 把 `TABLE` 从线程链表摘下，做 `ha_extra(HA_EXTRA_DETACH_CHILDREN)`、`free_blob_buffers_and_reset(table, MAX_TDC_BLOB_SIZE)`、`ha_reset()`、`unbind_psi()`，再交给 `release_or_close_table()` |
| sql/sql_base.cc | `release_or_close_table` | 决定回缓存还是直接销毁：旧版本 / 字典无效 / 统计无效 / 关停中 → `remove_table()` + `intern_close_table()`；否则 `release_table(thd, table)` 回缓存 |
| sql/sql_base.cc | `intern_close_table` | 真正销毁：`free_io_cache()`、`destroy(table->triggers)`、`closefrm(table, true)`、`destroy()`、`my_free()` |
| sql/sql_base.cc | `tdc_remove_table` | 按库表名移除表：`create_table_def_key()` 定位 share，主表与 secondary 影子表各移除一次；`ref_count > 0` 时按类型决定是否 `share->clear_version()` |
| sql/sql_base.cc | `close_cached_tables` | `tables` 为空时 `refresh_version++` 并清空全部空闲表与 share；否则对每个表调 `tdc_remove_table(..., TDC_RT_REMOVE_UNUSED, ...)` |
| sql/sql_base.cc | `tdc_flush_unused_tables` | 给 `handle_manager` 周期性清空所有未使用表用：`lock_all_and_tdc()` → `free_all_unused_tables()` → `unlock_all_and_tdc()` |
| sql/sql_base.cc | `release_table_share` | share 引用计数归零时的去向决策：旧版本或关停中直接 `erase`；否则挂到 `end_of_unused_share` 链尾，并在 `table_def_cache->size() > table_def_size` 时淘汰链头（LRU） |
| sql/sql_base.cc | `table_def_init` | 启动时初始化 `LOCK_open`、`COND_open`、`table_cache_manager.init()`，再建 `table_def_cache` |
| sql/sql_base.cc | `table_def_free` | 关停时 `delete table_def_cache`、`table_cache_manager.destroy()`，销毁 `COND_open` 与 `LOCK_open` |
| sql/sql_base.cc | `create_table_def_key` | 生成 `db\0table` 形式的缓存键（`strmake` + 显式 NUL） |
| sql/sql_base.cc | `get_table_def_key` | 从 `Table_ref` 的 MDL 键对象直接取缓存键，不额外分配内存 |
| sql/table.cc | `open_table_from_share` | `TABLE_SHARE` → `TABLE` 的实例化实现（定义处）：错误码 1/2/4 分别为「表不存在/表空间缺失」「文件打不开」「非有效表定义」 |
| sql/table.cc | `open_table_error` | 按错误码翻译成用户可见错误：`ENOENT` → `ER_NO_SUCH_TABLE`、`HA_ERR_TABLESPACE_MISSING` → `ER_TABLESPACE_MISSING`、`EMFILE` → `ER_CANT_OPEN_FILE`、错误码 2 时按 `ENOENT`/`EAGAIN`/其它分派 `ER_FILE_NOT_FOUND`/`ER_FILE_USED`/`ER_CANT_OPEN_FILE` |
| sql/table.cc | `closefrm` | 关闭 `TABLE`：按 `table->db_stat` 决定是否 `ha_close()`，释放别名与字段，`destroy(table->file)`，按 `free_share` 决定 `release_table_share()` 还是 `free_table_share()` |
| sql/table.cc | `alloc_table_share` | 分配 `TABLE_SHARE` 与其 `cache_element[]` 数组（长度 `table_cache_instances`）并清零，把数组指针挂到 `share->cache_element` |
| sql/mysqld.cc | `adjust_table_cache_size` | 表打开缓存容量调整：`limit = max((requested_open_files - 10 - max_connections) / 2, TABLE_OPEN_CACHE_MIN)`，`TABLE_OPEN_CACHE_MIN` 为 400；再算 `table_cache_size_per_instance = table_cache_size / table_cache_instances` |
| sql/mysqld.cc | `adjust_open_files_limit` | 反向依赖表缓存：`limit_1 = 10 + max_connections + table_cache_size * 2`（MyISAM 每表两个句柄）、`limit_2 = max_connections * 5`、`limit_3` 取 5000 兜底 |
| sql/mysqld.cc | `adjust_table_def_size` | 表定义缓存上限：`table_definition_cache` 变量默认值设为 `min(400 + table_cache_size / 2, 2000)`；用户未显式指定时 `table_def_size` 取该默认值 |
| sql/mysqld.cc | `table_cache_instances` | 表打开缓存的分片数（`ulong`），`Table_cache_manager` 按它遍历 `m_table_cache[]`；`TABLE_SHARE::cache_element[]` 数组长度也取自它 |
| sql/mysqld.cc | `Table_open_cache_hits` / `Table_open_cache_misses` / `Table_open_cache_overflows` | 三个状态项注册到 Performance Schema，对应 `System_status_var` 里的同名字段 |

## 与主核 / AI 的关系

**架构推论：** 这一层是「服务器层」与「存储引擎」之间唯一的翻译带：`TABLE_SHARE` 是不可变的表定义，`TABLE` 是带私有状态的实例。MySQL 把两者分开并让 `TABLE` 可跨语句复用，是为了让「打开表」这件频繁发生的事不必每次都重新向引擎申请资源。主核若要复刻同一思路，需要区分两件事：**可共享的元数据**（字段布局、键信息、记录长度）与**每次打开都要重建的私有状态**（handler 句柄、记录缓冲里的默认值位、列使用位图）。

**架构推论：** `open_table_from_share()` 的错误码分层（1/2/4 是普通错误，7/8 是「引擎侧状态变了、值得重试」）是一个值得借鉴的设计：把「失败」细分成「重试能解决」与「重试解决不了」，让上层的回退策略（`OT_DISCOVER` / `OT_FIX_ROW_TYPE` / `OT_REPAIR`）有明确的触发依据，而不是笼统地报一个错误码。

**架构推论：** 缓存的失效判定被拆成四类正交条件（定义过期、字典无效、统计无效、关停中），而不是一个统一的「脏」标记。这让不同性质的失效可以有不同处理：定义过期要重开、统计无效只 `invalidate_stats()`、关停中要立刻释放引擎引用以配合插件卸载。

**架构推论：** 命中路径刻意做到「几乎什么都不做」（只 `rebind_psi()` 与 `ha_extra(HA_EXTRA_RESET_STATE)`），说明缓存的正确性责任被前移到了「什么时候可以放回缓存」的那一侧。对 AI 辅助的内核实现而言，这意味着复用路径必须自己承担状态清理的义务，而不是指望下一次使用者来收拾。

## 参考

- [MySQL 参考手册：The Table Open Cache](https://dev.mysql.com/doc/refman/8.0/en/table-open-cache.html) —— 表打开缓存的官方语义：缓存中存放什么、何时被淘汰、与 `table_open_cache` 的关系。
- [MySQL 参考手册：服务器系统变量 `table_open_cache`](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_table_open_cache) —— 表打开缓存容量的配置入口。
- [MySQL 参考手册：INFORMATION_SCHEMA `TABLE_OPEN_CACHE` 表](https://dev.mysql.com/doc/refman/8.0/en/information-schema-table_open_cache.html) —— 缓存内部状态的官方可观测口径。
- [mysql/mysql-server 源码：sql/table_cache.cc（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/table_cache.cc) —— `Table_cache` / `Table_cache_manager` 的分片结构、加锁顺序与失效清理。
- [mysql/mysql-server 源码：sql/sql_base.cc（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_base.cc) —— `open_table()` 的命中/未命中分支、`release_or_close_table()` 的回缓存判定、`tdc_remove_table()` 的移除语义。
- [mysql/mysql-server 源码：sql/table.cc（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/table.cc) —— `open_table_from_share()` 的实例化实现与 `open_table_error()` 的错误码翻译。
