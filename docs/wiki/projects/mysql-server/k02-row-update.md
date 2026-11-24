---
project: mysql-server
feature: row-update
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/row/row0mysql.cc
    symbols: [row_update_for_mysql, row_upd_step]
---

# 更新行

## 一句话职责

把一次 UPDATE 落实为「聚簇索引记录的版本变更 + 各二级索引条目的同步变更」：先在聚簇索引上判定这次更新是否改动了排序字段，据此选择原地更新或「标记删除旧记录 + 插入新记录」，再逐个二级索引按需删除旧条目、插入新条目。边界之外：B-tree 页的物理维护与分裂属 G02，undo 日志的存储与 purge 属 J05。

## 机制是什么

一次 UPDATE 在 InnoDB 里不是「把记录原地改掉」这一件事，而是**两阶段、三分支**的动作：

1. **判定阶段**：先算清这次更新到底动没动聚簇索引的排序字段（clustered index 的 ordering field）。
2. **聚簇索引阶段**：根据判定结果二选一——
   - 没动排序字段：**原地更新**（in-place update）。旧记录被就地改写，同时把旧值写进 undo，形成「旧版本 + 新版本」的链；事务号与 roll pointer 被回填到记录里。
   - 动了排序字段：**删除标记 + 插入**（delete-mark & insert）。旧记录被打上 delete-mark（不物理摘除），新记录按新排序字段插进 B-tree。这条路径同时被「UPDATE 改了主键」和「UPDATE 改了聚簇索引排序字段」共用。

   注意：这里的「删旧插新」和真正的 DELETE 行不是一回事——它产生的是同一行的新版本，旧版本靠 undo 反查，不是靠 purge 清理。
3. **二级索引阶段**：只有受影响的二级索引才动，规则是「删旧条目 + 插新条目」。

版本链的支点在聚簇索引记录上。`btr_cur_upd_lock_and_undo` 在聚簇记录上调用 `trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr, index, nullptr, update, cmpl_info, rec, offsets, roll_ptr)` 生成修改类 undo，并把 `roll_ptr` 回填到记录；二级索引记录走的是 `lock_sec_rec_modify_check_and_lock`，注释明确写着「we do undo logging only when we update a clustered index record」。

**架构推论：**把行版本的事务信息集中在聚簇记录上、二级索引不记事务号，是为了让一致性读只需查「聚簇记录 + undo 链」就能判定可见性，不必在每个二级索引上重复做版本判定。

判定「排序字段变没变」的函数是 `row_upd_changes_ord_field_binary`（声明在 `row0upd.h`，该头文件不在本次本地检出内）。其实现体 `row_upd_changes_ord_field_binary_func` 在 `storage/innobase/row/row0upd.cc` 内：它遍历索引的前 `dict_index_get_n_unique(index)` 个字段，虚拟列按 `vcol->v_pos` 取更新项、实体列按 `dict_col_get_clust_pos(col, clust_index)` 取更新项；命中更新项后用 `dfield_datas_are_binary_equal(dfield, &upd_field->new_val, ind_field->prefix_len)` 做二进制比较，不等即判定为「变了」。函数注释写明这是「compare the fields as binary strings」——不做类型转换的等值比较。

## 怎么实现的

### 顶层分派

`row_update_for_mysql` 只做一件事：按 `prebuilt->table->is_intrinsic()` 分派。

- intrinsic 表 → `row_del_upd_for_mysql_using_cursor(prebuilt)`（走游标路径，见 K03）。
- 普通表 → 断言 `prebuilt->template_type == ROW_MYSQL_WHOLE_ROW`（或表有虚拟列且 `prebuilt->read_just_key`），然后进 `row_update_for_mysql_using_upd_graph(mysql_rec, prebuilt)`。

`row_update_for_mysql_using_upd_graph` 的前置检查按顺序是：

1. `prebuilt->table->ibd_file_missing` 为真 → 打 `ER_IB_MSG_984` 并返回 `DB_ERROR`。
2. `srv_force_recovery > 0` 且不满足豁免条件（`srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN && dict_sys_t::is_dd_table_id(prebuilt->table->id)`）→ 打 `ER_IB_MSG_985` 并返回 `DB_READ_ONLY`。
3. `trx->op_info = "updating or deleting"`；`row_mysql_delay_if_needed()`。
4. `init_fts_doc_id_for_ref(table, &fk_depth)`；若 `dict_table_is_referenced_by_foreign_key(table)` 为真则再调一次。
5. `trx_start_if_not_started_xa(trx, true, UT_LOCATION_HERE)`。
6. `node->del_multi_val_pos = 0`、`node->upd_multi_val_pos = 0`。
7. 定位游标：`clust_index = table->first_index()`；若 `prebuilt->pcur->m_btr_cur.index == clust_index` 则 `btr_pcur_t::copy_stored_position(node->pcur, prebuilt->pcur)`，否则 `copy_stored_position(node->pcur, prebuilt->clust_pcur)`。随后断言 `node->pcur->m_rel_pos == BTR_PCUR_ON`。
8. `savept = trx_savept_take(trx)` 取保存点。
9. `thr = que_fork_get_first_thr(prebuilt->upd_graph)`；`node->state = UPD_NODE_UPDATE_CLUSTERED`。
10. `que_thr_move_to_run_state_for_mysql(thr, trx)`，然后进入 `run_again:` 标签。

### row_upd_step：更新图的调度

`row_upd_step(thr)` 是 `storage/innobase/row/row0upd.cc` 里的调度入口，函数体顺序：

1. `trx_start_if_not_started_xa(trx, true, UT_LOCATION_HERE)`。
2. 取 `node = static_cast<upd_node_t *>(thr->run_node)`、`sel_node = node->select`、`parent = que_node_get_parent(node)`；断言 `que_node_get_type(node) == QUE_NODE_UPDATE`。
3. 若 `thr->prev_node == parent`，把 `node->state` 置为 `UPD_NODE_SET_IX_LOCK`。
4. `node->state == UPD_NODE_SET_IX_LOCK` 时：若 `!node->has_clust_rec_x_lock`，调 `lock_table(0, node->table, LOCK_IX, thr)`，失败即 `goto error_handling`。随后 `node->state = UPD_NODE_UPDATE_CLUSTERED`；若 `node->searched_update` 为真，把 `sel_node->state` 置为 `SEL_NODE_OPEN`，令 `thr->run_node = sel_node` 并返回（先取一行再改）。
5. `sel_node` 非空且 `sel_node->state != SEL_NODE_FETCH` 时：非 `searched_update` 命中 `ut_error`；否则断言 `sel_node->state == SEL_NODE_NO_MORE_ROWS`，令 `thr->run_node = parent` 并返回（没有更多行可改）。
6. 调 `row_upd(node, thr)` 做实际更新。
7. `error_handling:` 把 `err` 写入 `trx->error_state`；非 `DB_SUCCESS` 直接返回 `nullptr`。
8. 成功时按 `node->searched_update` 决定 `thr->run_node` 指向 `sel_node`（取下一行）还是 `parent`（显式游标更新结束），最后把 `node->state` 重置为 `UPD_NODE_UPDATE_CLUSTERED` 并返回 `thr`。

### row_upd：真正的分发者

`row_upd(node, thr)` 是 `row_upd_step` 调用的实际执行体：

1. 在 `node->in_mysql_interface` 为真（MySQL 接口路径必然如此）时，**重新计算** `cmpl_info`：若 `node->is_delete` 或 `row_upd_changes_some_index_ord_field_binary(node->table, node->update)` 为真，则 `node->cmpl_info = 0`；否则 `node->cmpl_info = UPD_NODE_NO_ORD_CHANGE`。源码注释写明：MySQL 解释器不给这个值，必须现算。
2. `switch (node->state)` 命中 `UPD_NODE_UPDATE_CLUSTERED` / `UPD_NODE_INSERT_CLUSTERED` 时：非 intrinsic 表先 `log_free_check()`，然后 `err = row_upd_clust_step(node, thr)`。
3. `node->index == nullptr`，或（`!node->is_delete` 且 `cmpl_info & UPD_NODE_NO_ORD_CHANGE`）→ 直接返回 `DB_SUCCESS`，**跳过全部二级索引**。这是「更新没碰任何索引排序字段」时的快路径。
4. 否则 `do { ... } while` 逐索引推进：每轮先 `dict_table_skip_corrupt_index(node->index)`（索引被判损坏则被置空，`break` 退出）；`node->index` 非空且 `node->index->type != DICT_FTS` 时调 `row_upd_sec_step(node, thr)`，失败立即返回；最后 `node->index = node->index->next()`。
5. 收尾：`node->row != nullptr` 时清空 `node->row / node->ext / node->upd_row / node->upd_ext` 并 `mem_heap_empty(node->heap)`；`node->state` 重置为 `UPD_NODE_UPDATE_CLUSTERED`。

`row_upd_changes_some_index_ord_field_binary(table, update)` 的判据比逐索引判定更粗：它只看**聚簇索引**的字段。虚拟列看 `dict_table_get_nth_v_col(index->table, upd_field->field_no)->m_col.ord_part`，实体列看 `index->get_field(upd_field->field_no)->col->ord_part`，任一为真即返回 true。

### row_upd_clust_step：聚簇索引这一步

`row_upd_clust_step(node, thr)` 的函数体顺序：

1. `index = node->table->first_index()`；`referenced = row_upd_index_is_referenced(index)`（该函数查 `table->referenced_set` 里是否有以该索引为被引用索引的外键）。
2. `mtr_start(&mtr)`。临时表加 `BTR_NO_LOCKING_FLAG`、设 `MTR_LOG_NO_REDO`；intrinsic 表再加 `BTR_NO_UNDO_LOG_FLAG`。
3. 断言 `pcur->m_rel_pos == BTR_PCUR_ON`。
4. `mode`：在线 DDL 索引用 `BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED` 并 `mtr_s_lock(dict_index_get_lock(index), &mtr, ...)`；普通索引用 `BTR_MODIFY_LEAF`。
5. `pcur->restore_position(mode, &mtr, ...)` 返回假 → `err = DB_RECORD_NOT_FOUND`，`mtr_commit` 后返回。
6. `rec = pcur->get_rec()`；`offsets = rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, ...)`。
7. `!node->has_clust_rec_x_lock` 时调 `lock_clust_rec_modify_check_and_lock(flags, pcur->get_block(), rec, index, offsets, thr)`，失败则 `mtr_commit` 并返回。
8. `node->is_delete` 为真 → 走 `row_upd_del_mark_clust_rec`（删除路径，见 K03），成功后 `node->state = UPD_NODE_UPDATE_ALL_SEC`、`node->index = index->next()`。
9. **更新路径**：`!node->in_mysql_interface` 时先 `row_upd_copy_columns` + `row_upd_eval_new_vals`（自算新值）。
10. `node->cmpl_info & UPD_NODE_NO_ORD_CHANGE` 为真 → 直接 `row_upd_clust_rec(...)` 返回（这就是「没动排序字段」的原地更新分支）。
11. 否则先 `row_upd_store_row(node, trx->mysql_thd, ...)` 保存整行（后续要靠它重建二级索引条目），再 `row_upd_changes_ord_field_binary(index, node->update, thr, node->row, node->ext, nullptr)`：
    - 为真 → `row_upd_clust_rec_by_insert(flags, node, index, thr, referenced, &mtr)`（删旧插新），成功后 `node->state = UPD_NODE_UPDATE_ALL_SEC`。
    - 为假 → `row_upd_clust_rec(flags, node, index, offsets, &heap, thr, &mtr)`（原地更新），成功后 `node->state = UPD_NODE_UPDATE_SOME_SEC`。
12. `node->index = index->next()`。

### 原地更新：row_upd_clust_rec

`row_upd_clust_rec` 的函数体顺序：

1. 在线 DDL 索引先 `row_log_table_get_pk` 取回旧主键；若 `row_log_table_get_error(index) == DB_INDEX_CORRUPT` 则 `err = DB_CORRUPTION`、`mtr->commit()` 后退出。
2. `persist_autoinc = row_upd_check_autoinc_counter(node, mtr)`——该函数在表有自增列、非临时表、且 `node->row != nullptr` 时，比较 `row_upd_get_new_autoinc_counter(node->update, table->autoinc_field_no)` 与旧值，新值更大才 `dict_table_autoinc_log`。
3. 两条更新方式二选一：
   - `node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE` → `btr_cur_update_in_place(flags | BTR_NO_LOCKING_FLAG, btr_cur, offsets, node->update, node->cmpl_info, thr, thr_get_trx(thr)->id, mtr)`。
   - 否则 → `btr_cur_optimistic_update(flags | BTR_NO_LOCKING_FLAG, btr_cur, &offsets, offsets_heap, node->update, node->cmpl_info, thr, thr_get_trx(thr)->id, mtr)`。
4. 失败则 `mtr->commit()`；`buf_LRU_buf_pool_running_out()` 为真 → `err = DB_LOCK_TABLE_FULL` 退出。
5. 否则重新 `mtr->start()`，临时表再补 `BTR_NO_LOCKING_FLAG` + `MTR_LOG_NO_REDO`（intrinsic 表再补 `BTR_NO_UNDO_LOG_FLAG`）。
6. `ut_a(pcur->restore_position(BTR_MODIFY_TREE, mtr, UT_LOCATION_HERE))` —— 断言恢复必然成功，源码注释说明理由：事务已对该记录持有 s/x 锁，且同事务的其他线程不会同时改这条记录。
7. `btr_cur_pessimistic_update(flags | BTR_NO_LOCKING_FLAG | BTR_KEEP_POS_FLAG, btr_cur, &offsets, offsets_heap, heap, &big_rec, node->update, node->cmpl_info, thr, trx_id, trx->undo_no, mtr)`。若 `big_rec` 非空（记录超长需外部存储），调 `lob::btr_store_big_rec_extern_fields(trx, pcur, node->update, offsets, big_rec, mtr, lob::OPCODE_UPDATE)`。
8. 在线 DDL 索引调 `row_log_table_update` 记日志。
9. `mtr->commit()`；`persist_autoinc` 为真时在关 mtr 与放 latch 之后调 `dict_table_persist_to_dd_table_buffer(node->table)`。

**btr 层的两条更新路径。** `btr_cur_update_in_place` 在 `storage/innobase/btr/btr0cur.cc`：先对压缩页检查空间（`btr_cur_update_alloc_zip` 失败返回 `DB_ZIP_OVERFLOW`），再 `btr_cur_upd_lock_and_undo` 做加锁与 undo 记录；未带 `BTR_KEEP_SYS_FLAG` 且非 intrinsic 表时 `row_upd_rec_sys_fields` 回填事务号与 roll pointer；`block->ahi.index` 非空且（索引非聚簇，或 `row_upd_changes_ord_field_binary(index, update, thr, nullptr, nullptr, nullptr)` 为真）时 `btr_search_update_hash_on_delete(cursor)` 移除哈希索引指针；最后 `row_upd_rec_in_place(rec, index, offsets, update, page_zip)` 真正改写记录，`btr_cur_update_in_place_log` 以 `MLOG_REC_UPDATE_IN_PLACE` 写 redo。

`btr_cur_upd_lock_and_undo` 是版本生成的真正落点：非聚簇索引只调 `lock_sec_rec_modify_check_and_lock`（不做 undo，注释明写 "We do undo logging only when we update a clustered index record"）；聚簇索引在未带 `BTR_NO_LOCKING_FLAG` 时调 `lock_clust_rec_modify_check_and_lock`，然后 `trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr, index, nullptr, update, cmpl_info, rec, offsets, roll_ptr)`。

`btr_cur_optimistic_update` 的分派：`row_upd_changes_field_size_or_external(index, *offsets, update)` 为假（最常见情形——不改变任何字段长度、无外部存储字段）→ 直接退化为 `btr_cur_update_in_place`；记录或更新向量里存在外部存储字段 → 返回 `DB_OVERFLOW`，交给悲观路径；否则构造 `new_entry = row_rec_to_index_entry(rec, index, *offsets, *heap)`、`row_upd_index_replace_new_col_vals_index_pos` 替换新值、再算 `new_rec_size = rec_get_converted_size(index, new_entry)` 与 `old_rec_size = rec_offs_size(*offsets)` 决定页内是否放得下。

### 删旧插新：row_upd_clust_rec_by_insert

`row_upd_clust_rec_by_insert(flags, node, index, thr, referenced, mtr)` 的函数体顺序：

1. `entry = row_build_index_entry_low(node->upd_row, node->upd_ext, index, heap, ROW_BUILD_FOR_INSERT)`；`row_upd_index_entry_sys_field(entry, index, DATA_TRX_ID, trx->id)` 写入事务号。
2. `switch (node->state)`：
   - `UPD_NODE_INSERT_CLUSTERED` → 说明上一次调用在 `row_ins_clust_index_entry` 里发生了锁等待，这次只做 `row_upd_clust_rec_by_insert_inherit(index, nullptr, nullptr, entry, node->update)` 继承外部字段。
   - `UPD_NODE_UPDATE_CLUSTERED` → 首次调用。取 `rec = btr_cur_get_rec(btr_cur)`、`offsets = rec_get_offsets(...)`。若记录已被 delete-mark，断言 `rec_get_trx_id(rec, index) == trx->id` 且 roll pointer 不是 insert undo，直接跳到 `check_fk`（这是锁等待重入的场景）。否则 `btr_cur_del_mark_set_clust_rec(flags, btr_cur_get_block(btr_cur), rec, index, offsets, thr, node->row, mtr)` 给旧记录打删除标记。
3. `rec_offs_any_extern(offsets)` 为真且 `row_upd_clust_rec_by_insert_inherit` 返回真 → 用 `lob::BtrContext` 的 `disown_inherited_fields(node->update)` 把外部字段的所有权从旧记录移交给新条目（源码注释说明：若插入失败，这次移交会在回滚时撤销）。
4. `check_fk:` `referenced` 为真 → `row_upd_check_references_constraints(node, pcur, table, index, offsets, thr, mtr)`，失败走 `err_exit`。
5. `mtr_commit(mtr)`；`err = row_ins_clust_index_entry(index, entry, thr, false)`；`node->state = UPD_NODE_INSERT_CLUSTERED`。

### 二级索引：row_upd_sec_step

`row_upd_sec_step(node, thr)` 的进入条件由断言给出：状态必须是 `UPD_NODE_UPDATE_ALL_SEC` 或 `UPD_NODE_UPDATE_SOME_SEC`，且 `!node->index->is_clustered()`。

分派逻辑：`node->state == UPD_NODE_UPDATE_ALL_SEC` 或 `row_upd_changes_ord_field_binary(node->index, node->update, thr, node->row, node->ext, ...)` 为真时才动手，否则直接返回 `DB_SUCCESS`（该二级索引未受影响，跳过）。动手时分三种：

- 多值索引 + 删除 → `row_upd_del_multi_sec_index_entry(node, thr)`。
- 多值索引 + 更新 → `row_upd_multi_sec_index_entry(node, thr, (node->state == UPD_NODE_UPDATE_ALL_SEC || non_mv_upd))`。
- 普通索引 → `row_upd_sec_index_entry(node, thr)`，即 `row_upd_sec_index_entry_low(node, nullptr, thr)`。

**row_upd_sec_index_entry_low 的函数体顺序：**

1. `index = node->index`；`referenced = row_upd_index_is_referenced(index)`。
2. 空间索引的 SRID 校验：非删除、`dict_index_is_spatial(index)` 且 `index->srid_is_valid` 时，取 `node->upd_row` 里该列的 `uint32_t srid`，与 `index->srid` 不等直接返回 `DB_CANT_CREATE_GEOMETRY_OBJECT`。
3. 构造旧条目：`entry = row_build_index_entry(node->row, node->ext, index, heap)`（`node->row` 是 `row_upd_store_row` 存的旧行）。
4. `mtr_start(&mtr)`；临时表加 `BTR_NO_LOCKING_FLAG` + `MTR_LOG_NO_REDO`（intrinsic 表再补 `BTR_NO_UNDO_LOG_FLAG`）。
5. 索引未提交（在线建索引）时按 `dict_index_get_online_status(index)` 分派：`ONLINE_INDEX_COMPLETE` 什么都不做；`ONLINE_INDEX_CREATION` → `row_log_online_op(index, entry, 0)` 记 DELETE，非删除操作再 `row_build_index_entry(node->upd_row, node->upd_ext, index, heap)` 重建新条目并 `row_log_online_op(index, entry, trx->id)` 记 INSERT；`ONLINE_INDEX_ABORTED` / `ONLINE_INDEX_ABORTED_DROPPED` → `mtr_commit` 后退出。
6. 检索模式 `mode` 的选择条件写在源码注释里：「We can only buffer delete-mark operations if there are no foreign key constraints referring to the index. Change buffering is disabled for temporary tables and spatial index.」即 `referenced || index->table->is_temporary() || dict_index_is_spatial(index)` 三者任一为真 → 只带 `BTR_MODIFY_LEAF`；三者皆假 → `BTR_MODIFY_LEAF | BTR_DELETE_MARK`（允许进 change buffer）。空间索引再加 `BTR_RTREE_DELETE_MARK`。
7. `search_result = row_search_index_entry(index, entry, mode, &pcur, &mtr)`，按结果分派：
   - `ROW_BUFFERED` → 直接放过（源码注释：Entry was delete marked already）。
   - `ROW_NOT_FOUND` → 索引未提交时放过（在线 CREATE INDEX 的正常窗口）；空间索引 `btr_cur->rtr_info->fd_del` 为真时放过；否则 `ib::error(ER_IB_MSG_1044)` 报「was not found on update」。
   - `ROW_NOT_DELETED_REF` → `ut_error`。
   - `ROW_FOUND` → 记录未 delete-mark 时 `btr_cur_del_mark_set_sec_rec(flags, btr_cur, true, thr, &mtr)` 标记旧二级条目；`referenced` 为真再 `row_upd_check_references_constraints`。
8. `pcur.close()`；`mtr_commit(&mtr)`。
9. `node->is_delete || err != DB_SUCCESS` → 直接返回，**不插新条目**（删除路径到此结束）。
10. 更新路径：`mem_heap_empty(heap)` 后 `entry = row_build_index_entry(node->upd_row, node->upd_ext, index, heap)` 构造新条目，`row_ins_sec_index_entry(index, entry, thr, false)` 插入。

`row_ins_sec_index_entry`（`storage/innobase/row/row0ins.cc`）自身先查外键（`index->table->foreign_set` 非空时 `row_ins_check_foreign_constraints`），然后先乐观下降 `row_ins_sec_index_entry_low(flags, BTR_MODIFY_LEAF, ...)`，返回 `DB_FAIL` 再悲观下降 `row_ins_sec_index_entry_low(flags, BTR_MODIFY_TREE, ...)`。intrinsic 表的 flags 固定为 `BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG`。

### 多值索引的特殊处理

`row_upd_multi_sec_index_entry(node, thr, non_mv_upd)` 用 `Multi_value_entry_builder_normal` 分两轮：第一轮以 `node->row`（旧行）为源，`begin(node->del_multi_val_pos)` 起逐条取多值条目，每条调 `row_upd_del_one_multi_sec_index_entry(index, entry, thr)` 删旧条目；第二轮以 `node->upd_row`（新行）为源，`begin(node->upd_multi_val_pos)` 起逐条 `row_ins_sec_index_entry(index, entry, thr, false)` 插新条目。任一轮出错就把进度记入 `node->del_multi_val_pos` / `node->upd_multi_val_pos`（取 `mv_entry_builder.last_multi_value_position()`）以便续做，成功后复位为 0。函数前置断言：`!dict_index_is_spatial(index)`、`!node->index->table->is_intrinsic()`、`!row_upd_index_is_referenced(node->index)`、`node->index->is_committed()`、`!dict_index_is_online_ddl(node->index)`。

### 错误分派与锁等待

`row_update_for_mysql_using_upd_graph` 在 `row_upd_step` 返回后读 `err = trx->error_state`：

- `err != DB_SUCCESS` → `que_thr_stop_for_mysql(thr)`。若 `err == DB_RECORD_NOT_FOUND`：复位 `trx->error_state = DB_SUCCESS`、清空 `trx->op_info`，`thr->fk_cascade_depth > 0` 时 `que_graph_free_recursive(node)`，然后 `goto error`。
- 其余错误：`thr->lock_state = QUE_THR_LOCK_ROW` → `row_mysql_handle_errors(&err, trx, thr, &savept)` → `thr->lock_state = QUE_THR_LOCK_NOLOCK`。`row_mysql_handle_errors` 返回 `true`（是锁等待）时 `goto run_again` 重跑 `row_upd_step`。

`row_mysql_handle_errors` 的分派（`storage/innobase/row/row0mysql.cc`）：

- `DB_LOCK_WAIT_TIMEOUT` 且 `row_rollback_on_timeout` 为真 → `trx_rollback_to_savepoint(trx, nullptr)`；否则 fallthrough 到下面一组语句级错误。
- 语句级错误一组（`DB_DUPLICATE_KEY`、`DB_FOREIGN_DUPLICATE_KEY`、`DB_TOO_BIG_RECORD`、`DB_UNDO_RECORD_TOO_BIG`、`DB_ROW_IS_REFERENCED`、`DB_NO_REFERENCED_ROW`、`DB_CANNOT_ADD_CONSTRAINT`、`DB_TOO_MANY_CONCURRENT_TRXS`、`DB_OUT_OF_FILE_SPACE`、`DB_READ_ONLY`、`DB_FTS_INVALID_DOCID`、`DB_INTERRUPTED`、`DB_CANT_CREATE_GEOMETRY_OBJECT`、`DB_COMPUTE_VALUE_FAILED`、`DB_LOCK_NOWAIT`、`DB_BTREE_LEVEL_LIMIT_EXCEEDED`）：有 savepoint 时 `trx_rollback_to_savepoint(trx, savept)`，注释写明「MySQL will roll back the latest SQL statement」。
- `DB_LOCK_WAIT` → `trx_kill_blocking(trx)`、`lock_wait_suspend_thread(thr)`；醒来后 `trx->error_state` 仍非成功则重新分派，否则返回 `true` 让调用方重跑。
- `DB_DEADLOCK` / `DB_LOCK_TABLE_FULL` → `trx_rollback_to_savepoint(trx, nullptr)` 回滚整个事务（注释写明这是 3.23.43 版本加的处理）。
- `DB_MUST_GET_MORE_FILE_SPACE` → `ib::fatal(ER_IB_MSG_972)`。
- `DB_CORRUPTION` → `ib::error(ER_IB_MSG_973)`。

### 成功后的统计与计数

`row_update_for_mysql_using_upd_graph` 成功路径：

1. `dict_table_has_fts_index(table)` 且 `trx->fts_next_doc_id != UINT64_UNDEFINED` → `row_fts_update_or_delete(prebuilt)`；失败 `goto error`。
2. 非删除时累加 `srv_stats.n_system_rows_updated`（`table->is_system_table`）或 `srv_stats.n_rows_updated`。
3. `node->is_delete || !(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)` 时调 `row_update_statistics_if_needed(prebuilt->table)`。该函数的阈值是常量表达式：持久统计下 `counter > n_rows / 10`（源码注释标为 10%）且 `dict_stats_auto_recalc_is_enabled(table)` 时 `dict_stats_recalc_pool_add(table)` 并把 `stat_modified_counter` 归零；非持久统计下 `counter > 16 + n_rows / 16`（源码注释标为 6.25%）时 `dict_stats_update(table, DICT_STATS_RECALC_TRANSIENT)`。表未 `stat_initialized` 直接返回。

## 关键取舍

- **源码事实：**`row_upd` 在 `node->in_mysql_interface` 下自己重算 `cmpl_info`，用 `row_upd_changes_some_index_ord_field_binary`（只看聚簇索引的 `ord_part`）判据，把 `UPD_NODE_NO_ORD_CHANGE` 置上或置 0。**架构推论：**这是把「有没有必要动二级索引」的判定前移到聚簇阶段一次性做完，避免逐个二级索引重复比较全量列值；代价是判据比逐索引判据粗——`row_upd_changes_some_index_ord_field_binary` 只看聚簇索引的排序字段是否变化，二级索引自身的排序字段是否变化要等到 `row_upd_sec_step` 里再判一次。
- **源码事实：**`row_upd` 里 `cmpl_info & UPD_NODE_NO_ORD_CHANGE` 为真时**完全跳过**二级索引循环并直接返回 `DB_SUCCESS`。**架构推论：**只改非索引列的 UPDATE 不产生任何二级索引写放大，也不产生二级索引的 undo——这是 InnoDB 性能上最常见的一条快路径。
- **源码事实：**`btr_cur_optimistic_update` 在「不改变字段长度、无外部存储字段」时直接退化为 `btr_cur_update_in_place`；注释自称「the simplest and the most common case」。**架构推论：**原地更新（同一页内改字节）与删旧插新（B-tree 结构调整）之间存在明确的成本台阶，InnoDB 用「页内能否放得下」作为分界线，而不是无条件走删旧插新。
- **源码事实：**`row_upd_clust_rec` 里乐观更新失败后 `mtr->commit()` 再 `mtr->start()` 重新走悲观路径，并 `ut_a(pcur->restore_position(BTR_MODIFY_TREE, mtr, ...))` 断言恢复必成功。**架构推论：**断言而非错误处理，依赖的是「已持记录锁 + 同事务不并发改同记录」这个不变式——这把「恢复失败」从运行期错误变成了开发期不可能事件。
- **源码事实：**`row_upd_sec_index_entry_low` 只在 `referenced`、临时表、空间索引三者都为假时才带 `BTR_DELETE_MARK`，源码注释给出理由是「能被缓冲的删除标记必须没有外键引用指向该索引」。**架构推论：**用 change buffer 批量写二级索引的删除标记，代价是被缓冲的删除标记要等 merge 才落到叶页；外键引用的索引不能缓冲，因为外键校验需要立刻看到该记录的真实状态。
- **源码事实：**`row_upd_changes_ord_field_binary_func` 用 `dfield_datas_are_binary_equal` 做二进制比较，不做类型转换。**架构推论：**这条路径要同时服务 CHAR/VARCHAR、NUMERIC、日期时间、空间等多种类型，逐类型写等值比较不现实；代价是 `0001` 与 `1` 这类「语义相等但二进制不等」的值会被判为「变了」，从而走删旧插新这条更贵的路径。
- **源码事实：**`row_upd_sec_index_entry_low` 对 `ROW_NOT_FOUND` 在索引未提交时放过、其余情况报 `ER_IB_MSG_1044`。**架构推论：**在线 CREATE INDEX 与并发 UPDATE 之间存在「二级索引里根本没有这条旧记录」的合法窗口，更新路径必须容忍它，否则并发 DDL 会把正常更新变成错误日志。
- **源码事实：**`row_update_for_mysql_using_upd_graph` 的成功路径只在 `node->is_delete || !(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)` 时才调 `row_update_statistics_if_needed`。**架构推论：**只改非索引列的 UPDATE 不改变索引结构，所以不触发统计重算；这与删除路径无条件触发统计重算形成对照。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/row/row0mysql.cc` | `row_update_for_mysql` | 顶层入口：按 `prebuilt->table->is_intrinsic()` 分派到 `row_del_upd_for_mysql_using_cursor` 或 `row_update_for_mysql_using_upd_graph`。 |
| `storage/innobase/row/row0mysql.cc` | `row_update_for_mysql_using_upd_graph` | 普通表更新主流程：`.ibd` 缺失与 force-recovery 前置检查、游标定位、`trx_savept_take` 取保存点、调 `row_upd_step`、`row_mysql_handle_errors` 分派错误与锁等待、成功后更新统计与计数。 |
| `storage/innobase/row/row0mysql.cc` | `row_mysql_handle_errors` | 错误分派：`DB_LOCK_WAIT` 挂起后返回 `true` 让调用方重跑；语句级错误 `trx_rollback_to_savepoint(trx, savept)`；`DB_DEADLOCK` / `DB_LOCK_TABLE_FULL` 回滚整个事务。 |
| `storage/innobase/row/row0mysql.cc` | `row_update_statistics_if_needed` | 统计重算触发器：持久统计阈值 `counter > n_rows / 10`，非持久统计阈值 `counter > 16 + n_rows / 16`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_step` | 更新图调度：`UPD_NODE_SET_IX_LOCK` 时 `lock_table(..., LOCK_IX, ...)`，`searched_update` 时切到 `sel_node` 取下一行，最后调 `row_upd`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd` | 执行分发：重算 `cmpl_info`、调 `row_upd_clust_step`、按需逐索引调 `row_upd_sec_step`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_clust_step` | 聚簇索引一步：恢复游标、`lock_clust_rec_modify_check_and_lock`、按 `cmpl_info` 与 `row_upd_changes_ord_field_binary` 选原地更新或删旧插新，并决定 `UPD_NODE_UPDATE_ALL_SEC` / `UPD_NODE_UPDATE_SOME_SEC`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_clust_rec` | 原地更新：`btr_cur_update_in_place` / `btr_cur_optimistic_update` → 失败转 `btr_cur_pessimistic_update`，超长记录走 `lob::btr_store_big_rec_extern_fields`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_clust_rec_by_insert` | 删旧插新：`btr_cur_del_mark_set_clust_rec` 标记旧聚簇记录、`row_upd_clust_rec_by_insert_inherit` 移交外部字段、`row_ins_clust_index_entry` 插入新记录，状态置 `UPD_NODE_INSERT_CLUSTERED`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_sec_step` | 二级索引一步：按 `UPD_NODE_UPDATE_ALL_SEC` 或该索引自身的 `row_upd_changes_ord_field_binary` 判定是否动手，并按是否多值索引分派三个分支。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_sec_index_entry_low` | 二级索引更新的底层实现：检索模式选择、在线索引状态分派、`ROW_FOUND` 时删除标记旧条目、更新时 `row_ins_sec_index_entry` 插新条目。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_multi_sec_index_entry` | 多值索引更新：两轮 `Multi_value_entry_builder_normal`，先删旧多值条目再插新条目，进度记在 `node->del_multi_val_pos` / `node->upd_multi_val_pos`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_changes_ord_field_binary_func` | 排序字段变更判据的实现：遍历前 `dict_index_get_n_unique(index)` 个字段，用 `dfield_datas_are_binary_equal` 做二进制比较。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_changes_some_index_ord_field_binary` | 粗判据：只看聚簇索引字段的 `col->ord_part`，决定能否置 `UPD_NODE_NO_ORD_CHANGE`（即跳过二级索引）。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_store_row` | 保存整行到节点堆：构建 `node->row`，更新时再 `dtuple_copy` 得 `node->upd_row` 并用 `row_upd_replace` 填入新值。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_check_references_constraints` | 外键引用校验：`table->is_dd_table` 或 `table->referenced_set` 为空直接返回成功，否则逐条检查被引用的外键。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_index_is_referenced` | 判断索引是否被外键引用：查 `table->referenced_set` 里是否存在 `dict_foreign_with_index(index)`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_rec_in_place` | 原地改写记录的字段值，并按压缩/Redundant 格式分别维护 INSTANT / VERSION 位与 info bits。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_update_in_place` | 原地更新的 btr 实现：压缩页空间检查、`btr_cur_upd_lock_and_undo`、`row_upd_rec_sys_fields` 回填事务号与 roll pointer、`row_upd_rec_in_place` 改写、`MLOG_REC_UPDATE_IN_PLACE` 写 redo。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_optimistic_update` | 页内乐观更新：字段长度不变且无外部字段时退化为 `btr_cur_update_in_place`，有外部字段时返回 `DB_OVERFLOW` 转悲观路径。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_upd_lock_and_undo` | 版本生成落点：聚簇索引调 `lock_clust_rec_modify_check_and_lock` 后 `trx_undo_report_row_operation(..., TRX_UNDO_MODIFY_OP, ...)`；二级索引只加锁不做 undo。 |
| `storage/innobase/lock/lock0lock.cc` | `lock_clust_rec_modify_check_and_lock` | 聚簇记录修改锁：`lock_rec_convert_impl_to_expl` 后请求 `LOCK_X \| LOCK_REC_NOT_GAP`。 |
| `storage/innobase/lock/lock0lock.cc` | `lock_sec_rec_modify_check_and_lock` | 二级记录修改锁：请求 `LOCK_X \| LOCK_REC_NOT_GAP`；注释说明此时别的事务不可能对该记录持有隐式锁。 |
| `storage/innobase/row/row0ins.cc` | `row_ins_sec_index_entry` | 二级索引新条目插入：先 `row_ins_check_foreign_constraints`，再先乐观 `BTR_MODIFY_LEAF` 后悲观 `BTR_MODIFY_TREE` 两级下降。 |

## 与主核 / AI 的关系

主核 `engine/db/` 的 L2 索引层与 L3 查询层同样要处理「一次 UPDATE 怎么落到多条索引上」，可对照的设计点有三个：

- 主核的 UPDATE 是否也把「排序字段有没有变」作为一级分水岭，变与不变分别走哪两条路径。若不区分，页内放不下时的回退就会缺少一条明确的「删旧插新」路径。
- 主核是否为「删旧插新」保存旧记录与新记录的对应关系。InnoDB 靠 undo + roll pointer 把旧版本挂回原记录，主核若没有等价机制，就需要另一套手段让二级索引条目能追溯回它对应的行版本。
- 主核的二级索引更新是「逐索引判定是否受影响」还是「先粗判后细化」。InnoDB 用 `UPD_NODE_NO_ORD_CHANGE` 先粗判一次跳过全部二级索引，再在 `row_upd_sec_step` 里对具体索引细判一次——这个两级结构省掉了大多数 UPDATE 的二级索引开销。

**架构推论：**InnoDB 把「版本信息只挂在聚簇记录上」当作整条更新链的地基，更新路径上所有与事务相关的动作（undo 报告、事务号回填、可见性判定）都围绕聚簇记录展开，二级索引只做结构上的跟随。主核若采用行存 + 多索引结构，可以参考这个分工：让事务元信息有唯一的落点，避免每个索引都维护一份版本信息，从而把「一次更新要动几个索引」的复杂度控制住。

## 参考

- [MySQL 8.0 Reference Manual：InnoDB 多版本并发控制](https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html) —— 官方对行版本、undo 记录与可见性的说明。
- [MySQL 8.0 Reference Manual：InnoDB 锁](https://dev.mysql.com/doc/refman/8.0/en/innodb-locking.html) —— 记录锁与行级锁语义的官方说明。
- [MySQL 8.0 Reference Manual：InnoDB 聚簇索引与二级索引](https://dev.mysql.com/doc/refman/8.0/en/innodb-index-types.html) —— 聚簇索引与二级索引组织方式的官方说明。
- [MySQL 官方源码：storage/innobase/row/row0mysql.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0mysql.cc) —— `row_update_for_mysql` 与 `row_update_for_mysql_using_upd_graph` 的定义处。
- [MySQL 官方源码：storage/innobase/row/row0upd.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0upd.cc) —— `row_upd_step`、`row_upd`、`row_upd_clust_step`、`row_upd_sec_step` 的定义处。
- [MySQL 官方源码：storage/innobase/btr/btr0cur.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/btr/btr0cur.cc) —— `btr_cur_update_in_place`、`btr_cur_optimistic_update`、`btr_cur_upd_lock_and_undo` 的定义处。
