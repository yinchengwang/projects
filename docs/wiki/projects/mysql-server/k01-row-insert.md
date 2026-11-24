---
project: mysql-server
feature: row-insert
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/row/row0mysql.cc
    symbols: [row_insert_for_mysql, row_ins_step]
---

# 插入行

## 一句话职责

把 server 层送来的一行 MySQL 格式记录翻译成 InnoDB 的行与索引条目，逐个索引落到 B-tree 上：普通表走 INSERT 执行图（加表 IX 锁、分配 row id、外键检查、唯一键检查、聚簇索引与各二级索引依次插入），intrinsic 表走一条绕过锁与事务语义的直接游标路径。物理页维护、B-tree 分裂与 undo 生成不在本页范围内。

## 机制是什么

一次插入在 InnoDB 侧有两条互斥的路径，由 `row_insert_for_mysql` 按表类型分派：

- **普通表**（`prebuilt->table->is_intrinsic()` 为假）走 `row_insert_for_mysql_using_ins_graph`：它构造或复用 INSERT 执行图，把 MySQL 行格式转成 InnoDB 的 `dtuple_t`，然后反复调 `row_ins_step` 驱动执行图前进。
- **intrinsic 表**（`is_intrinsic()` 为真）走 `row_insert_for_mysql_using_cursor`：源码注释写明这是为了放宽 intrinsic 表的诸多限制（表锁、事务处理等），它直接遍历各索引并逐条插入，不走执行图。

两条路径共享的下层是同一套索引插入原语：聚簇索引走 `row_ins_clust_index_entry`，二级索引走 `row_ins_sec_index_entry`，多值二级索引走 `row_ins_sec_index_multi_value_entry`（由 `row_ins_index_entry` 按 `index->is_multi_value()` 分派）。

索引插入的共同形状是「先乐观下降，失败再悲观下降」：`row_ins_clust_index_entry` 先以 `BTR_MODIFY_LEAF` 调 `row_ins_clust_index_entry_low`，只有返回 `DB_FAIL` 时才以 `BTR_MODIFY_TREE` 再调一次；`row_ins_sec_index_entry` 的形状相同，先 `row_ins_sec_index_entry_low(BTR_MODIFY_LEAF)`，返回 `DB_FAIL` 时再 `row_ins_sec_index_entry_low(BTR_MODIFY_TREE)`。乐观下降不锁整棵树，只在叶页上尝试插入；失败说明插入位置已经变了（页已满或分裂），此时改走悲观路径——加锁并从根重新下降，允许沿路做页分裂。

边界之外：B-tree 页分裂与分裂键的挑选属 G02；undo 日志的生成与格式属 J05；行锁的加锁语义属 J02。

## 怎么实现的

### 入口分派与前置检查

`row_insert_for_mysql` 只有一句判断：`prebuilt->table->is_intrinsic()` 为真就调 `row_insert_for_mysql_using_cursor`，否则调 `row_insert_for_mysql_using_ins_graph`。

`row_insert_for_mysql_using_ins_graph` 在真正动手之前做了一串早退检查，顺序即源码顺序：

1. `dict_table_is_discarded(prebuilt->table)` 为真 → 打 `ER_IB_MSG_976` 后返回 `DB_TABLESPACE_DELETED`（表空间被丢弃）。
2. `prebuilt->table->ibd_file_missing` 为真 → 打 `ER_IB_MSG_977` 后返回 `DB_TABLESPACE_NOT_FOUND`（.ibd 文件缺失）。
3. `srv_force_recovery` 非零，且不满足豁免条件 `srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN && dict_sys_t::is_dd_table_id(prebuilt->table->id)` → 打 `ER_IB_MSG_978` 后返回 `DB_READ_ONLY`。豁免的作用是允许在部分场景下修改硬编码的 DD 表以让 DDL 能进行。
4. `DBUG_EXECUTE_IF("mark_table_corrupted", ...)` 调试钩子会把聚簇索引标为损坏；随后 `table->is_corrupted()` 为真 → 打 `ER_IB_MSG_979` 后返回 `DB_TABLE_CORRUPT`。

通过检查后设 `trx->op_info = "inserting"`，调 `row_mysql_delay_if_needed`（测试用的注入延迟点），调 `trx_start_if_not_started_xa(trx, true, UT_LOCATION_HERE)` 启动事务。

### 行格式转换

`row_get_prebuilt_insert_row(prebuilt)` 取出待插入行：若 `prebuilt->ins_node` 已存在，则把 `prebuilt->ins_node->ins_multi_val_pos` 复位为 0，并在 `prebuilt->trx_id == table->def_trx_id` 且 `UT_LIST_GET_LEN(prebuilt->ins_node->entry_list) == UT_LIST_GET_LEN(table->indexes)`（索引没有增删）时直接复用旧的 `ins_node->row`；否则 `que_graph_free_recursive(prebuilt->ins_graph)` 释放旧执行图重建。重建时用 `ins_node_create(INS_DIRECT, table, prebuilt->heap)` 建节点、`dtuple_create_with_vcol` 按 `table->get_n_cols()` 与 `dict_table_get_n_v_cols(table)` 建行元组，并按 `table->n_m_v_cols` 分配多值数据区。

`row_mysql_convert_row_to_innobase(node->row, prebuilt, mysql_rec, &temp_heap)` 做逐列翻译：按 `prebuilt->mysql_template` 的每一项取列，`templ->mysql_null_bit_mask != 0` 时用 `mysql_rec[templ->mysql_null_byte_offset] & templ->mysql_null_bit_mask` 判 SQL NULL 并 `dfield_set_null(dfield)` 后跳过该列。虚拟列走 `dtuple_get_nth_v_field`；多值虚拟列调 `innobase_get_multi_value` 取值后用 `dfield_multi_value_dup` 在 `temp_heap` 上深拷贝（源码注释说明深拷贝成本高，但因为 server 后续可能覆写这块内存，深拷贝是最安全的做法）。普通列调 `row_mysql_store_col_in_innobase_format` 写入 `prebuilt->ins_upd_rec_buff + templ->mysql_col_offset`。表带 FTS 时（`prebuilt->table->fts` 非空）最后调 `fts_create_doc_id(prebuilt->table, row, prebuilt->heap)` 分配文档 ID。

### 执行图驱动

转换完成后 `savept = trx_savept_take(trx)` 取 savepoint，`thr = que_fork_get_first_thr(prebuilt->ins_graph)` 取执行线程。首行状态由 `prebuilt->sql_stat_start` 决定：为真时 `node->state = INS_NODE_SET_IX_LOCK`，否则 `node->state = INS_NODE_ALLOC_ROW_ID`（前者表示统计信息已经采过样，可以跳过重新采样）。随后 `que_thr_move_to_run_state_for_mysql(thr, trx)` 把线程切到运行态，进入 `run_again` 标签：

```
run_again:
  thr->run_node = node;
  thr->prev_node = node;
  row_ins_step(thr);
```

`row_ins_step` 之后读 `err = trx->error_state`。成功则继续做 FTS 处理；失败则跳到 `error_exit`：`que_thr_stop_for_mysql(thr)`，把 `thr->lock_state` 置为 `QUE_THR_LOCK_ROW`，调 `row_mysql_handle_errors(&err, trx, thr, &savept)`，再把 `thr->lock_state` 置回 `QUE_THR_LOCK_NOLOCK`。`row_mysql_handle_errors` 返回 `was_lock_wait` 为真时——即错误是 `DB_LOCK_WAIT` 且线程已被唤醒、`trx->error_state` 已恢复——断言节点状态是 `INS_NODE_INSERT_ENTRIES` 或 `INS_NODE_ALLOC_ROW_ID`，然后 `goto run_again` 重跑 `row_ins_step`。这是插入路径唯一的重试点。

### row_ins_step 与 row_ins

`row_ins_step`（在 `storage/innobase/row/row0ins.cc`）先 `trx_start_if_not_started_xa`，取 `node = thr->run_node`，断言 `que_node_get_type(node) == QUE_NODE_INSERT` 且 `node->table->is_intrinsic()` 为假。它每轮把 `trx->id` 写进 `node->trx_id_buf`（`memset(node->trx_id_buf, 0, DATA_TRX_ID_LEN)` 后 `trx_write_trx_id`）。

节点状态为 `INS_NODE_SET_IX_LOCK` 时：若 `trx->id == node->trx_id`（同一事务里已经加过 IX 锁，例如分区表在同一条语句里先查后插），直接跳到 `same_trx`；否则调 `lock_table(0, node->table, LOCK_IX, thr)` 加表级 IX 锁，失败则跳 `error_handling`。加锁成功记 `node->trx_id = trx->id`。`same_trx` 处，`node->ins_type == INS_SEARCHED` 时把 `sel_node->state` 复位为 `SEL_NODE_OPEN`，把 `thr->run_node` 设为 `sel_node` 返回——即 INSERT ... SELECT 的插入行由 select 节点逐行喂进来。

状态推进到 `row_ins(node, thr)` 时，`row_ins` 先判 `node->state == INS_NODE_ALLOC_ROW_ID`：调 `row_ins_alloc_row_id_step(node)` 分配 row id，然后把 `node->index` 置为 `node->table->first_index()`（聚簇索引）、`node->entry` 置为 `UT_LIST_GET_FIRST(node->entry_list)`。接着按 `node->ins_type` 取行：`INS_SEARCHED` 调 `row_ins_get_row_from_query_block(node)`，`INS_VALUES` 调 `row_ins_get_row_from_values(node)`（后者对 `node->values_list` 逐项 `eval_exp` 后 `dfield_copy_data`）。状态置为 `INS_NODE_INSERT_ENTRIES`。

之后的索引循环体是 `row_ins` 的核心：

```
while (node->index != nullptr) {
  if (node->index->type != DICT_FTS) {
    err = row_ins_index_entry_step(node, thr);
    switch (err) {
      case DB_SUCCESS: break;
      case DB_DUPLICATE_KEY:
        thr_get_trx(thr)->error_state = DB_DUPLICATE_KEY;
        thr_get_trx(thr)->error_index = node->index;
        [[fallthrough]];
      default: return err;
    }
  }
  node->index = node->index->next();
  node->entry = UT_LIST_GET_NEXT(tuple_list, node->entry);
  ...
}
```

三点值得注意：其一，`node->index->type == DICT_FTS` 的索引被跳过——FTS 索引的维护不走这条 B-tree 插入路径；其二，唯一键冲突（`DB_DUPLICATE_KEY`）会把 `trx->error_state` 设为 `DB_DUPLICATE_KEY` 并把 `thr_get_trx(thr)->error_index` 记成冲突的那个索引，这样上层的 `row_mysql_handle_errors` 才能报出「哪个索引上的键重复」；其三，循环尾部有一个 `while (node->index && node->index->is_corrupted())` 跳过已损坏的二级索引及其条目。循环结束后 `node->state` 复位为 `INS_NODE_ALLOC_ROW_ID`，`thr_get_trx(thr)->error_index` 清空。

`row_ins_index_entry_step` 两步：先 `row_ins_index_entry_set_vals(node->index, node->entry, node->row)` 把行里对应的列值拷进索引条目，再 `row_ins_index_entry(node->index, node->entry, node->ins_multi_val_pos, thr)` 真正插入。

### 索引条目的构造与插入

`row_ins_index_entry_set_vals` 按索引字段逐个取列：普通列取 `dtuple_get_nth_field(row, ind_field->col->ind)`，虚拟列取 `dtuple_get_nth_v_field(row, v_col->v_pos)`。列前缀索引（`ind_field->prefix_len > 0`）用 `dtype_get_at_most_n_mbchars` 截断长度。空间索引的首个字段特殊处理：先检查 `row_field->data` 非空且 `row_field->len >= GEO_DATA_HEADER_SIZE`，否则返回 `DB_CANT_CREATE_GEOMETRY_OBJECT`；然后调 `row_ins_spatial_index_entry_set_mbr_field` 生成 MBR，若 `index->srid_is_valid && index->srid != srid` 同样返回 `DB_CANT_CREATE_GEOMETRY_OBJECT`。

`row_ins_index_entry` 按索引类型三分：聚簇索引调 `row_ins_clust_index_entry(index, entry, thr, false)`；多值索引调 `row_ins_sec_index_multi_value_entry(index, entry, multi_val_pos, thr)`；其余调 `row_ins_sec_index_entry(index, entry, thr, false)`。这个函数里还有两个测试钩子：`row_ins_index_entry_timeout` 直接返回 `DB_LOCK_WAIT`。

### 聚簇索引的插入

`row_ins_clust_index_entry` 先做外键检查——`index->table->foreign_set` 非空时调 `row_ins_check_foreign_constraints(index->table, index, entry, thr)`，失败即返回。该函数内部先跳过 DD 表（`dict_sys_t::is_dd_table_id(table->id)` 返回 `DB_SUCCESS`）和 `trx->check_foreigns == false` 的情形，再遍历 `table->foreign_set`，只对 `foreign->foreign_index == index` 的那条外键调 `row_ins_check_foreign_constraint`。

唯一键判定用 `n_uniq = dict_index_is_unique(index) ? index->n_uniq : 0`。随后组装 B-tree 标志位：普通表 `flags = index->table->is_temporary() ? BTR_NO_LOCKING_FLAG : 0`；`index->table->skip_alter_undo` 为真（ALTER 中间表）时追加 `BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG`；intrinsic 表则是 `BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG`。

插入本身在 `row_ins_clust_index_entry_low`：

- 临时表用 `mtr.set_log_mode(MTR_LOG_NO_REDO)` 关闭 redo（源码注释：临时表生命周期限于 server 或连接期间，重启恢复不需要其 redo 信息）。
- `mode == BTR_MODIFY_LEAF && dict_index_is_online_ddl(index)` 时，模式追加 `BTR_ALREADY_S_LATCHED` 并先 `mtr_s_lock(dict_index_get_lock(index), ...)`。
- 用 `pcur.open(index, 0, entry, PAGE_CUR_LE, mode, &mtr, ...)` 定位（源码注释说明用 `PAGE_CUR_LE` 是为了在 low_match 与 up_match 上都拿到合理值）。
- AUTO_INCREMENT：`!index->table->is_temporary() && dict_table_has_autoinc_col(index->table)` 时取 `row_get_autoinc_counter(entry, index->table->autoinc_field_no)`，非零则先 `dict_table_autoinc_log(index->table, counter, &mtr)` 落盘。源码注释写明要在拿到索引锁之后、任何进一步资源获取之前写日志以避免死锁，且「总是先记计数器变更，这样它不会被后续失败影响」。`persist_autoinc` 为真时在函数出口 `dict_table_persist_to_dd_table_buffer(index->table)` 持久化（源码注释：在关闭 mini-transaction 并释放 latch 之后做）。
- 唯一键冲突检测：`!index->allow_duplicates && n_uniq && (cursor->up_match >= n_uniq || cursor->low_match >= n_uniq)` 时进入检查分支。特殊标志组合 `BTR_CREATE_FLAG | BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG` 走 `row_ins_duplicate_error_in_clust_online`（在线表重建应用日志时只查重复不真插），其余走 `row_ins_duplicate_error_in_clust`（源码注释说明它也可能返回 `DB_LOCK_WAIT`）。冲突时 `thr_get_trx(thr)->error_index = cursor->index`。
- `dup_chk_only` 为真时提交 mtr 直接返回，只做重复检查不插入。
- `row_ins_must_modify_rec(cursor)` 为真时（`cursor->low_match >= dict_index_get_n_unique_in_tree(cursor->index)` 且游标不在 infimum）走 `row_ins_clust_index_entry_by_modify`——把插入转成对已有记录的修改。源码注释解释：已有记录与新记录公共前缀足够长，必须转成 modify；若新记录放不进原槽位，旧记录被加进 free list 并插入新记录，这会让为 SELECT 缓存的游标失效，所以函数先把 `index->last_sel_cur->invalid` 置真。
- 否则（乐观路径）`mode != BTR_MODIFY_TREE` 时调 `btr_cur_optimistic_insert`；悲观路径先判 `buf_LRU_buf_pool_running_out()` 为真则返回 `DB_LOCK_TABLE_FULL`，否则先试 `btr_cur_optimistic_insert`，返回 `DB_FAIL` 才调 `btr_cur_pessimistic_insert`。
- `big_rec` 非空（有列被放到页外存储）时提交 mtr 后调 `row_ins_index_entry_big_rec` 写页外字段，再 `dtuple_convert_back_big_rec` 还原条目。
- 出口统一 `pcur.close()`，释放 `offsets_heap`。

### 二级索引的插入

`row_ins_sec_index_entry` 与聚簇的形状一致，但标志位与堆不同：它先 `mem_heap_create(1024, ...)` 建 `offsets_heap` 与 `heap` 两个堆（常量 1024 是初始堆容量），标志位同样对临时表加 `BTR_NO_LOCKING_FLAG`、对 `skip_alter_undo` 表加 `BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG`（同时把 `trx_id` 记为 `thr_get_trx(thr)->id`，供在线 DDL 记录用），intrinsic 表用 `BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG`。先 `row_ins_sec_index_entry_low(flags, BTR_MODIFY_LEAF, ...)`，返回 `DB_FAIL` 时 `mem_heap_empty(heap)` 后再以 `BTR_MODIFY_TREE` 调一次。

`row_ins_sec_index_entry_low` 内部：临时表设 `mtr.set_log_mode(MTR_LOG_NO_REDO)`；非空间索引追加 `BTR_INSERT`（源码注释：既不是临时表也不是空间索引时才启用 insert buffering，即 change buffer）；`!thr_get_trx(thr)->check_unique_secondary` 时追加 `BTR_IGNORE_SEC_UNIQUE`；空间索引走 `btr_cur_search_to_nth_level` 带 `PAGE_CUR_RTREE_INSERT` 的 R-tree 路径，普通索引用 `PAGE_CUR_LE`。游标返回 `BTR_CUR_INSERT_TO_IBUF` 时说明插入已被缓冲，直接结束。

二级唯一索引的冲突检测在 `row_ins_sec_index_entry_low` 里：`dict_index_is_unique(index) && (cursor.low_match >= n_unique || cursor.up_match >= n_unique)` 时，调 `row_ins_scan_sec_index_for_duplicate` 扫描重复，命中 `DB_DUPLICATE_KEY` 且索引尚未提交（`!index->is_committed()`，即在线建索引场景）时，源码注释说明不向上层返回错误，而是 `dict_set_corrupted(index)` 并把 `err` 改为 `DB_SUCCESS`——重复由 ALTER TABLE 或 CREATE UNIQUE INDEX 自己报。查到重复且事务已持锁后，会重新定位游标（去掉 `BTR_INSERT | BTR_IGNORE_SEC_UNIQUE`）再继续插入。

### FTS 与统计

插入成功后，`row_insert_for_mysql_using_ins_graph` 处理 FTS：`dict_table_has_fts_index(table)` 为真时先用 `fts_get_doc_id_from_row(table, node->row)` 从隐藏的 FTS 列取出 doc id。`doc_id <= 0` 打 `ER_IB_MSG_980` 并返回 `DB_FTS_INVALID_DOCID`；若表还没有 `DICT_TF2_FTS_HAS_DOC_ID` 标志，`doc_id < next_doc_id` 打 `ER_IB_MSG_981` 返回同一错误；`doc_id - next_doc_id >= FTS_DOC_ID_MAX_STEP` 打 `ER_IB_MSG_982` 返回同一错误（源码注释说明 Doc ID 的差值被限制在 4 字节整数内，参见 `fts_get_encoded_len()`，相邻 doc id 的差不得超过 `FTS_DOC_ID_MAX_STEP`）。最后按 `table->skip_alter_undo` 分流：为真时建 `fts_trx` 并调 `fts_add_doc_from_tuple`；否则调 `fts_trx_add_op(trx, table, doc_id, FTS_INSERT, nullptr)`（源码注释：一次 INSERT 影响该表全部 FTS 索引，所以受影响列传 NULL）。

然后 `que_thr_stop_for_mysql_no_error(thr, trx)`，按 `table->is_system_table` 分别累加 `srv_stats.n_system_rows_inserted` 或 `srv_stats.n_rows_inserted`，调 `dict_table_n_rows_inc(table)`（源码注释：为性能考虑不受 `dict_table_stats_lock()` 保护，宁可让 `stat_n_rows` 这个估计值不准，也不愿为此加 latch），再调 `row_update_statistics_if_needed(table)`，最后清空 `trx->op_info` 并释放 `temp_heap`。

### intrinsic 表的游标插入

`row_insert_for_mysql_using_cursor` 按源码注释分了 5 步：

1. `row_get_prebuilt_insert_row(prebuilt)` 取行与执行线程。
2. `row_mysql_to_innobase(node->row, prebuilt, mysql_rec)` 做格式转换（与普通表用的是不同函数，普通表用 `row_mysql_convert_row_to_innobase`）。
3. 聚簇索引非唯一时用 `dict_table_get_next_table_sess_row_id(node->table)` 填 `node->row_id_buf`；再用 `dict_table_get_next_table_sess_trx_id(node->table)` 填 `node->trx_id_buf`。
4. 遍历 `node->table->indexes`：先 `row_ins_index_entry_set_vals`，再按 `index->is_clustered()` 调 `row_ins_clust_index_entry` 或 `row_ins_sec_index_entry`；成功则记 `inserted_upto = index`，失败则中断。
5. 出错时把已插入的前序索引显式撤销：`mtr_start` + `dict_disable_redo_if_temporary` 后遍历到 `inserted_upto`，逐个调 `row_explicit_rollback(index, node->entry, thr, &mtr)`，然后 `mtr_commit`。源码注释写明这是因为 UNDO 日志已关闭，自动回滚不生效，所以必须显式撤销。

成功后同样累加 `dict_table_n_rows_inc` 与 `srv_stats.n_rows_inserted` / `n_system_rows_inserted`，最后把 `thr_get_trx(thr)->error_state` 置为 `DB_SUCCESS`。

## 关键取舍

- **源码事实：**`row_insert_for_mysql_using_ins_graph` 在拿索引锁之后、任何进一步资源获取之前就先写 AUTO_INCREMENT 计数器日志，源码注释写明理由是「防止死锁」，并且「总是先记计数器变更，这样它不会被后续失败影响」。**架构推论：**自增计数器的持久化点与事务成败解耦，是为了让并发插入在计数器分配上不互锁——若等事务提交后再持久化，多个事务会读到同一个已缓存的计数器值。
- **源码事实：**聚簇与二级索引都采用「先乐观下降、返回 `DB_FAIL` 再悲观下降」的两段式，`row_ins_clust_index_entry_low` 里 `BTR_MODIFY_LEAF` 只调 `btr_cur_optimistic_insert`，`BTR_MODIFY_TREE` 才在乐观失败后调 `btr_cur_pessimistic_insert`。**架构推论：**大多数插入只改一个叶页，乐观路径避免了沿根到叶整条路径的 latch 持有，把并发插入的锁持有时间压到单页级别；只有页满时才付悲观路径的代价。
- **源码事实：**二级索引的插入在「既非临时表也非空间索引」时才追加 `BTR_INSERT` 标志（源码注释：既不是 temp-table nor spatial index 才启用 insert buffering）。**架构推论：**change buffer 要求插入点唯一可判定且不需要即时可见性判定，临时表没有 redo 恢复需求、空间索引走的是 R-tree 而非 B-tree，两者都不适用。
- **源码事实：**`row_ins` 的索引循环跳过 `DICT_FTS` 类型的索引；FTS 的维护在 `row_insert_for_mysql_using_ins_graph` 里通过 `fts_trx_add_op` 单独完成。**架构推论：**FTS 索引的正文数据以文档为单位组织，与行级 B-tree 插入是两套不同的索引结构，混在同一个循环里会让每行的插入都付出文档级维护的代价。
- **源码事实：**二级唯一索引在在线建索引（`!index->is_committed()`）时遇到重复，源码注释明确说明「不向上层返回任何错误，重复由 ALTER TABLE 或 CREATE UNIQUE INDEX 报告」，做法是 `dict_set_corrupted(index)` 并把 `err` 改成 `DB_SUCCESS`。**架构推论：**在线 DDL 期间的重复键是合法的中间状态（数据正在迁移），把它当普通错误抛给 DML 会打断本可成功的 ALTER。
- **源码事实：**intrinsic 表路径用 `row_explicit_rollback` 显式撤销已插入的索引条目，源码注释写明「Automatic rollback is not in action as UNDO logs are turned-off」。**架构推论：**无 undo 的表必须自带撤销机制，这条路径与普通表共用同一套索引插入原语、只在撤销策略上分叉。
- **源码事实：**`row_ins` 遇到 `DB_DUPLICATE_KEY` 时同时记录 `trx->error_state` 与 `thr_get_trx(thr)->error_index`。**架构推论：**把「冲突发生在哪个索引」作为一等错误信息向上传递，server 层才能区分「主键冲突」与「唯一二级索引冲突」并给出对应的报错文案。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/row/row0mysql.cc` | `row_insert_for_mysql` | 插入总入口：按 `prebuilt->table->is_intrinsic()` 在 `row_insert_for_mysql_using_cursor` 与 `row_insert_for_mysql_using_ins_graph` 之间分派。 |
| `storage/innobase/row/row0mysql.cc` | `row_insert_for_mysql_using_ins_graph` | 普通表的执行图路径：表空间/强制恢复/表损坏的前置检查、行格式转换、取 savepoint、驱动 `row_ins_step`、FTS 处理、统计累加与 `row_mysql_handle_errors` 错误分派。 |
| `storage/innobase/row/row0mysql.cc` | `row_insert_for_mysql_using_cursor` | intrinsic 表的直接游标路径：转换行、逐索引插入、失败时用 `row_explicit_rollback` 显式撤销已插入的条目。 |
| `storage/innobase/row/row0mysql.cc` | `row_mysql_convert_row_to_innobase` | MySQL 行格式到 InnoDB `dtuple_t` 的逐列翻译：判 NULL、虚拟列、多值列深拷贝、BLOB 虚拟列复制、FTS doc id 分配。 |
| `storage/innobase/row/row0mysql.cc` | `row_get_prebuilt_insert_row` | 取出或重建插入节点与行元组：索引未增删时复用，否则 `que_graph_free_recursive` 后重建 `ins_node` 与 `dtuple`。 |
| `storage/innobase/row/row0mysql.cc` | `row_mysql_handle_errors` | 错误分派：`DB_LOCK_WAIT` 挂起后返回 true 让调用方重跑；`DB_DUPLICATE_KEY` 等语句级错误回滚到 savepoint；`DB_DEADLOCK` / `DB_LOCK_TABLE_FULL` 回滚整个事务。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_step` | INSERT 执行图的调度入口：`INS_NODE_SET_IX_LOCK` 时 `lock_table(..., LOCK_IX, ...)`，`INS_SEARCHED` 时把运行节点交回 select 节点，最终调 `row_ins`。 |
| `storage/innobase/row/row0ins.cc` | `row_ins` | 插入编排：分配 row id、取行、逐索引调用 `row_ins_index_entry_step`，跳过 `DICT_FTS` 索引与已损坏的二级索引，记录 `DB_DUPLICATE_KEY` 的 `error_index`。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_alloc_row_id_step` | 聚簇索引非唯一时用 `dict_sys_get_new_row_id` 分配 row id 并写入 `node->row_id_buf`。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_index_entry_set_vals` | 构造索引条目：逐列取值、列前缀长度截断、空间索引首字段生成 MBR 与 SRID 校验。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_index_entry` | 按索引类型分派：聚簇走 `row_ins_clust_index_entry`，多值走 `row_ins_sec_index_multi_value_entry`，其余走 `row_ins_sec_index_entry`。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_clust_index_entry` | 聚簇索引插入：外键检查、唯一性判定、组装 `BTR_NO_LOCKING_FLAG` / `BTR_NO_UNDO_LOG_FLAG`、先 `BTR_MODIFY_LEAF` 后 `BTR_MODIFY_TREE`。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_clust_index_entry_low` | 聚簇插入的低层实现：AUTO_INCREMENT 计数器先落日志、唯一键冲突检测、`row_ins_must_modify_rec` 转 modify、乐观/悲观插入、页外字段 `big_rec` 写出。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_sec_index_entry` | 二级索引插入：建 `offsets_heap` 与 `heap`、标志位组装、先叶页后整树的两段式。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_sec_index_entry_low` | 二级插入的低层实现：临时表关 redo、`BTR_INSERT` 启用 change buffer、唯一二级索引的重复扫描与在线 DDL 下的特殊处理。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_check_foreign_constraints` | 插入时的外键检查：跳过 DD 表与 `trx->check_foreigns == false`，只对 `foreign->foreign_index == index` 的那条外键执行检查。 |
| `sql/handler.cc` | `handler::ha_write_row` | server 层写入入口：`mark_trx_read_write` 后调 `write_row`，成功后 `binlog_log_row` 写 binlog 行事件。 |
| `sql/sql_insert.cc` | `write_record` | server 层插入语句把一行交给引擎的调用点：`table->file->ha_write_row(table->record[0])`；`INSERT IGNORE` 场景走另一处 `ha_write_row` 调用点，出错时按 `table->file->is_fatal_error(error)` 决定错误是否致命。 |

## 与主核 / AI 的关系

主核 `engine/db/` 的 L2 索引层与 L3 查询层同样要处理「一行数据如何落到多个索引结构上」，可对照的设计点有三个：

- 主核的插入是否也固定「先主索引、再二级索引」的顺序，以及二级索引的插入失败时主索引已写入的行如何回滚（InnoDB 普通表靠 undo、intrinsic 表靠 `row_explicit_rollback` 显式撤销）。
- 主核是否区分「临时表 / 内存表」这类无持久化需求的表，它们的撤销路径是否也像 `row_insert_for_mysql_using_cursor` 那样不依赖回滚日志。
- 主核的索引插入是否也区分乐观与悲观下降，以及页满时的页分裂由谁负责（本仓的分层里，B-tree 分裂属 G02，主核的分裂实现在自己的索引层）。

**架构推论：**InnoDB 把「插入路径的编排」（加表锁、取 row id、外键检查、逐索引分发）与「单个索引条目的物理插入」（乐观/悲观下降、页分裂）分成两层，接口边界是 `row_ins_index_entry` 这一族函数；主核若照此分层，索引层只需提供「插入一个条目」的原语，编排逻辑（锁、事务、约束检查）就能独立演进。

## 参考

- [MySQL 8.0 Reference Manual：InnoDB 聚簇索引与二级索引](https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html) —— 官方对聚簇索引与二级索引组织方式的说明。
- [MySQL 8.0 Reference Manual：InnoDB 事务模型](https://dev.mysql.com/doc/refman/8.0/en/innodb-transactions.html) —— 插入路径上的事务与锁语义。
- [MySQL 8.0 Reference Manual：InnoDB 全文索引（FULLTEXT）](https://dev.mysql.com/doc/refman/8.0/en/innodb-fulltext-index.html) —— FTS doc id 与插入期维护的官方说明。
- [MySQL 官方源码：storage/innobase/row/row0mysql.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0mysql.cc) —— `row_insert_for_mysql` 及其两条子路径的定义处。
- [MySQL 官方源码：storage/innobase/row/row0ins.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0ins.cc) —— `row_ins_step`、索引条目构造与聚簇/二级索引插入原语的实现处。
- [MySQL 官方源码：sql/handler.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/handler.cc) —— server 层 `handler::ha_write_row` 的定义处。
