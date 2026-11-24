---
project: mysql-server
feature: row-delete
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/row/row0mysql.cc
    symbols: [row_delete_for_mysql_using_cursor, btr_cur_del_mark_set_clust_rec]
---

# 删除行

## 一句话职责

把一次行删除落实为索引记录的删除标记：普通表在聚簇索引记录上做带 undo 与事务号处理的 delete-mark，再逐个标记二级索引记录；intrinsic 表走一条只标记、不写 undo 的游标路径。物理清理留给 purge，本页不含它。

## 机制是什么

删除在这条调用链上**不**立即从 B-tree 页里摘掉记录，而是先给记录打上 delete-mark。这个标记在各处都由 `rec_get_deleted_flag` / `btr_rec_set_deleted_flag` 读写：`storage/innobase/page/page0zip.cc` 在压缩页时把记录 info bits 里的 `REC_INFO_DELETED_FLAG` 折成页目录槽位 `PAGE_ZIP_DIR_SLOT_DEL`，`storage/innobase/rem/rem0rec.cc` 打印记录时读同一位并输出 `Deleted`。两条路径的差别在于标记是怎么写的：

- **普通表**：聚簇索引记录经 `btr_cur_del_mark_set_clust_rec` 标记，这一步同时做锁检查、`trx_undo_report_row_operation` 生成修改 undo、回填记录的事务号与 roll pointer，并写 `MLOG_REC_CLUST_DELETE_MARK` redo；二级索引记录经 `btr_cur_del_mark_set_sec_rec` 只做锁检查 + 原地置位 + `MLOG_REC_SEC_DELETE_MARK` redo。
- **intrinsic 表**：`row_update_for_mysql` 直接把删除交给 `row_del_upd_for_mysql_using_cursor`，它调用 `row_delete_for_mysql_using_cursor`，后者在遍历各索引时用 `btr_rec_set_deleted_flag` 直接置位，不经过 `btr_cur_del_mark_set_clust_rec`，也不产生 undo 与 roll pointer。

上层的入口顺序是：server 层的 `DeleteCurrentRowAndProcessTriggers` 调 `handler::ha_delete_row`，handler 再调存储引擎的 `delete_row`、成功后调 `binlog_log_row` 写 `Delete_rows_log_event`；InnoDB 侧由 `row_update_for_mysql` 分派到上面两条路径之一。

边界之外：purge 真正摘除记录并释放外部字段属 J05；undo 日志的存储格式属 J05；B-tree 分裂与页维护属 G02。

## 怎么实现的

### 普通表的执行顺序

1. `row_update_for_mysql` 判断 `prebuilt->table->is_intrinsic()`，为假则调 `row_update_for_mysql_using_upd_graph`。该函数先查 `prebuilt->table->ibd_file_missing`（缺 .ibd 直接返回 `DB_ERROR`），再查 `srv_force_recovery` 与 `dict_sys_t::is_dd_table_id` 的豁免条件（不满足返回 `DB_READ_ONLY`），随后是 `row_mysql_delay_if_needed`、`init_fts_doc_id_for_ref(table, &fk_depth)`（`dict_table_is_referenced_by_foreign_key(table)` 为真时再调一次）、`trx_start_if_not_started_xa`、`trx_savept_take` 取 savepoint，把 `node->state` 置为 `UPD_NODE_UPDATE_CLUSTERED`，最后调 `row_upd_step`。
2. `row_upd_step` 在 `UPD_NODE_SET_IX_LOCK` 状态下、且 `node->has_clust_rec_x_lock` 为假时先 `lock_table(0, node->table, LOCK_IX, thr)`，然后把状态置为 `UPD_NODE_UPDATE_CLUSTERED` 并走 `row_upd`。`row_upd` 里因为 `node->is_delete` 为真，`cmpl_info` 被强制置 0（不走 `UPD_NODE_NO_ORD_CHANGE` 的快速就地更新分支），随即调用 `row_upd_clust_step`。
3. `row_upd_clust_step` 对临时表加 `BTR_NO_LOCKING_FLAG` 与 `MTR_LOG_NO_REDO`，对 intrinsic 表再加 `BTR_NO_UNDO_LOG_FLAG`，然后 `pcur->restore_position` 恢复持久游标。恢复失败即返回 `DB_RECORD_NOT_FOUND`。`node->has_clust_rec_x_lock` 为假时调用 `lock_clust_rec_modify_check_and_lock`，失败则提交 mtr 并返回该错误。
4. `node->is_delete` 为真时调 `row_upd_del_mark_clust_rec`：先 `row_upd_store_row` 把整行存进节点堆（二级索引条目要靠它重建），再 `btr_cur_del_mark_set_clust_rec`，成功且 `referenced`（`row_upd_index_is_referenced(index)` 判定）时调 `row_upd_check_references_constraints` 校验外键引用，最后 `mtr_commit`。
5. 成功后把 `node->state` 置为 `UPD_NODE_UPDATE_ALL_SEC`、`node->index = index->next()`，`row_upd` 用 do-while 逐索引调用 `row_upd_sec_step`。循环体每轮先 `dict_table_skip_corrupt_index(node->index)`（索引损坏会被置空并 `break`），`node->index->type == DICT_FTS` 的索引跳过，其余进 `row_upd_sec_step`。该步的进入条件是 `node->state == UPD_NODE_UPDATE_ALL_SEC` 或 `row_upd_changes_ord_field_binary` 为真——删除刚把状态置成前者，所以每个未损坏的非 FTS 索引都会进入：`node->index->is_multi_value()` 时删除调 `row_upd_del_multi_sec_index_entry`——它用 `Multi_value_entry_builder_normal` 的 `begin(node->del_multi_val_pos)` / `next()` 逐条取多值条目，逐条复用 `row_upd_sec_index_entry_low`；中途出错时把 `node->del_multi_val_pos` 记成 `last_multi_value_position()` 以便续做，全部成功后复位为 0。非多值索引进 `row_upd_sec_index_entry`。每处理完一个索引推进到 `index->next()`。
6. 任一环节返回非 `DB_SUCCESS` 就立刻返回，不再处理后续索引。
7. 全部索引处理成功后，`row_update_for_mysql_using_upd_graph` 在 `dict_table_has_fts_index(table)` 且 `trx->fts_next_doc_id != UINT64_UNDEFINED` 时调用 `row_fts_update_or_delete`；该函数对删除分支以 `nullptr` 索引参数调用 `fts_trx_add_op(trx, table, old_doc_id, FTS_DELETE, nullptr)`（源码注释说明一次删除影响该表全部 FTS 索引，故传 NULL）。

### 聚簇索引的删除标记

`btr_cur_del_mark_set_clust_rec` 的函数体按顺序做四件事：

- **已标记就直接放行**：`rec_get_deleted_flag` 为真时断言记录事务号等于当前事务并返回 `DB_SUCCESS`——源码注释说明这是级联删除会碰到的情形。
- **锁检查**：`lock_clust_rec_modify_check_and_lock(BTR_NO_LOCKING_FLAG, ...)`，非 `DB_SUCCESS` 立即返回。
- **undo 报告**：`trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr, index, entry, nullptr, 0, rec, offsets, &roll_ptr)`，失败立即返回。
- **置位与落日志**：`btr_rec_set_deleted_flag(rec, page_zip, true)`；intrinsic 表到此直接返回（不维护 roll pointer）；否则在 `dict_index_is_online_ddl(index)` 时调 `row_log_table_delete`，再 `row_upd_rec_sys_fields` 回填事务号与 roll pointer，最后 `btr_cur_del_mark_set_clust_rec_log` 以 `MLOG_REC_CLUST_DELETE_MARK` 写 redo。

### 二级索引的删除标记

`row_upd_sec_index_entry_low` 先按 `referenced || index->table->is_temporary() || dict_index_is_spatial(index)` 决定检索模式：三者都为假时才带 `BTR_DELETE_MARK`（允许走 change buffer 缓冲删除标记），为真时只带 `BTR_MODIFY_LEAF`；空间索引再追加 `BTR_RTREE_DELETE_MARK`。非提交态的在线索引先按 `dict_index_get_online_status(index)` 分派：`ONLINE_INDEX_CREATION` 调 `row_log_online_op` 记 DELETE、非删除操作再记 INSERT 后提交并退出，`ONLINE_INDEX_ABORTED` / `ONLINE_INDEX_ABORTED_DROPPED` 直接提交退出。

`row_search_index_entry` 返回后按结果分派：`ROW_BUFFERED` 直接放过（源码注释为「Entry was delete marked already」，即删除标记已进 change buffer）；`ROW_NOT_FOUND` 在索引未提交时放过、空间索引 `btr_cur->rtr_info->fd_del` 时放过，其余 `ib::error(ER_IB_MSG_1044)` 报「was not found on update」；`ROW_NOT_DELETED_REF` 落进 `ut_error`；`ROW_FOUND` 且记录尚未 delete-mark 时调 `btr_cur_del_mark_set_sec_rec`，`referenced` 为真再调 `row_upd_check_references_constraints`。收尾是 `pcur.close()` + `mtr_commit(&mtr)`，然后 `if (node->is_delete || err != DB_SUCCESS) goto func_exit` ——删除操作到此结束，**不**插入新条目；只有 UPDATE 才继续 `row_ins_sec_index_entry`。

`btr_cur_del_mark_set_sec_rec` 本身很短：`lock_sec_rec_modify_check_and_lock` 失败即返回；成功则 `btr_rec_set_deleted_flag` 原地置位并 `btr_cur_del_mark_set_sec_rec_log` 写 redo。被调用的 `lock_sec_rec_modify_check_and_lock` 在注释里写明：走到这里时聚簇索引记录已经被改过，因此别的事务不会在该二级记录上持有隐式锁。

### intrinsic 表的游标删除

`row_delete_for_mysql_using_cursor(node, delete_entries, restore_delete)` 的契约是「删到一半被紧随的 UPDATE 打断时要能撤回，所以要留住游标位置」：

1. `mtr_start` + `dict_disable_redo_if_temporary(table, &mtr)`。
2. 遍历 `table->indexes`：若已出错或 `restore_delete` 为真就停止遍历（`restore_delete` 是回滚模式，只做撤销）；`row_build_index_entry` 构造条目，`pcur.open(index, 0, entry, PAGE_CUR_LE, BTR_MODIFY_LEAF, ...)` 定位。
3. 命中 infimum / supremum 时置 `err = DB_ERROR`；否则 `btr_rec_set_deleted_flag(..., true)`、用一次 `mlog_open` / `mlog_close` 空调把 mtr 的修改位打开（注释写明不这样做块不会被调度刷盘）、`pcur.store_position`，再把游标塞进 `delete_entries`。
4. 循环后若 `err != DB_SUCCESS || restore_delete`，对 `delete_entries` 里每个游标 `restore_position(BTR_MODIFY_LEAF, ...)` 后把删除标记改回 false，同样补一次 `mlog_open` / `mlog_close`。`restore_position` 返回假时源码直接 `ut_a(success)` 断言失败。
5. `mtr_commit` 并返回 `err`。

它的调用方是 `row_del_upd_for_mysql_using_cursor`：先释放 `clust_index->last_ins_cur`（缓存插入位置会让 B-tree 结构改变），`copy_stored_position` 拿到原行游标，`row_upd_store_row`；非删除操作先试 `row_update_inplace_for_intrinsic`，只有它成功才提前返回；然后 `row_delete_for_mysql_using_cursor(node, delete_entries, false)` 删旧行。若接着要插入新行（UPDATE）且插入失败，`row_update_for_mysql_using_cursor` 会以 `restore_delete = true` 再调一次同一个函数撤销删除标记；`DB_TOO_BIG_RECORD` 分支同样撤销。仅删除时，成功后 `dict_table_n_rows_dec` 并累加 `srv_stats.n_rows_deleted` 或 `n_system_rows_deleted`（`is_system_table` 区分）。

### 锁等待与错误分派

`row_update_for_mysql_using_upd_graph` 读 `trx->error_state`；非成功时 `que_thr_stop_for_mysql`。`DB_RECORD_NOT_FOUND` 特殊处理：把 `trx->error_state` 复位为 `DB_SUCCESS`、`op_info` 清空、`fk_cascade_depth > 0` 时 `que_graph_free_recursive(node)`，然后跳错误出口。其余错误经 `row_mysql_handle_errors`：`DB_LOCK_WAIT` 先 `trx_kill_blocking`、`lock_wait_suspend_thread`，等醒后状态已恢复就直接 `goto run_again` 重跑 `row_upd_step`；语句级错误（`DB_DUPLICATE_KEY`、`DB_TOO_BIG_RECORD`、`DB_ROW_IS_REFERENCED` 等）在有 savepoint 时 `trx_rollback_to_savepoint(trx, savept)`；`DB_DEADLOCK` / `DB_LOCK_TABLE_FULL` 用 `nullptr` savepoint 回滚整个事务；`DB_FOREIGN_EXCEED_MAX_CASCADE` 只打 `ER_IB_MSG_974`。

级联删除由 `row_update_cascade_for_mysql` 处理：`thr->fk_cascade_depth++` 后超过 `FK_MAX_CASCADE_DEL` 即返回 `DB_FOREIGN_EXCEED_MAX_CASCADE`；锁等待在此处走 `lock_wait_suspend_thread` 后 `goto run_again`，递归返回后把计数复位为 0。

删除成功后 `row_update_for_mysql_using_upd_graph` 走 `dict_table_n_rows_dec` 与 `srv_stats.n_rows_deleted` / `n_system_rows_deleted`，并按 `node->is_delete || !(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)` 调 `row_update_statistics_if_needed`——该函数按 `dict_stats_is_persistent_enabled` 分两条阈值：持久统计下 `counter > n_rows / 10` 且自动重算开启时进 `dict_stats_recalc_pool_add`，非持久统计下 `counter > 16 + n_rows / 16` 时 `dict_stats_update`。

## 关键取舍

- **源码事实：**普通表先聚簇索引、再二级索引，聚簇那一步承担 undo 报告与事务号回填，二级那一步只置位写 redo；`row_upd` 的循环体用 `node->index` 单向推进。**架构推论：**把行版本的事务信息集中放在聚簇记录上，二级索引就不必各自记录事务号，删除的可见性判定因此只需查聚簇记录 + undo 链。
- **源码事实：**`btr_cur_del_mark_set_clust_rec` 已见 `rec_get_deleted_flag` 为真时断言「记录事务号 == 当前事务」并返回成功；`row_upd_sec_index_entry_low` 对 `ROW_FOUND` 也先查一次删除标记。**架构推论：**两条路径都做了幂等保护，是为了覆盖锁等待重跑与级联删除重入这两种「同一个删除被执行两次」的场景，避免重复写 redo 或重复报告 undo。
- **源码事实：**`row_upd_sec_index_entry_low` 只在 `referenced`、临时表、空间索引三者都为假时才带 `BTR_DELETE_MARK`，源码注释给的理由是「能被缓冲的删除标记必须没有外键引用指向该索引」。**架构推论：**这会牺牲「无引用的二级索引删除」的即时性换取 change buffer 的批量 I/O，代价是被缓冲的删除标记要等 merge 才落到叶页。
- **源码事实：**`row_delete_for_mysql_using_cursor` 为了能撤销，把 `btr_pcur_t` 逐个存进 `delete_entries` 并在失败/回滚模式里逐条 `restore_position` 反向撤销，而不是依赖 undo。**架构推论：**intrinsic 表不写 undo（`BTR_NO_UNDO_LOG_FLAG`），所以撤销只能靠保存的游标位置——这解释了为什么这条路径必须把「删半截」当成正常分支而不是异常分支。
- **源码事实：**`row_upd_sec_index_entry_low` 发现 `ROW_NOT_FOUND` 时，索引未提交（在线建索引场景）只放过不报错，其余情况打 `ER_IB_MSG_1044` 错误日志。**架构推论：**在线 DDL 与新写入之间存在「索引里根本没有这条旧记录」的合法窗口，删除路径必须容忍它，否则并发 DDL 会把正常的删除变成错误。
- **源码事实：**`row_update_statistics_if_needed` 的两条阈值分别是 `n_rows / 10`（持久统计）与 `16 + n_rows / 16`（非持久统计），删除无条件触发它（`node->is_delete` 直接进判据）。**架构推论：**删除对索引结构的影响比「只改非索引列的更新」更大，所以这里不设 `cmpl_info` 例外；阈值按统计持久化与否分成两档，是因为非持久统计重算更便宜、可以更频繁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/row/row0mysql.cc` | `row_delete_for_mysql_using_cursor` | intrinsic 表的删除路径：逐索引定位、`btr_rec_set_deleted_flag` 置位、保存游标以便 `restore_delete` 时反向撤销。 |
| `storage/innobase/row/row0mysql.cc` | `btr_cur_del_mark_set_clust_rec` | 该文件内的调用点：`row_explicit_rollback` 以 `BTR_NO_LOCKING_FLAG \| BTR_NO_UNDO_LOG_FLAG` 调它，把 `row_insert_for_mysql_using_cursor` 第 5 步里「插入某个索引失败时已插入的前序条目」显式撤销（该处注释写明「Automatic rollback is not in action as UNDO logs are turned-off」；二级条目的对应调用是 `btr_cur_del_mark_set_sec_rec`）。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_del_mark_set_clust_rec` | 聚簇索引删除标记的实现：锁检查、`trx_undo_report_row_operation`、置位、系统字段回填与 `MLOG_REC_CLUST_DELETE_MARK` redo。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_del_mark_set_sec_rec` | 二级索引删除标记的实现：`lock_sec_rec_modify_check_and_lock` + 原地置位 + `MLOG_REC_SEC_DELETE_MARK` redo。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_step` | 删除更新图的调度入口：`UPD_NODE_SET_IX_LOCK` 且未持聚簇 X 锁时先 `lock_table(..., LOCK_IX, ...)`，再逐节点执行删除。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_del_mark_clust_rec` | 聚簇删除步骤：`row_upd_store_row` 保存整行、调聚簇删除标记、`referenced` 时查外键引用、`mtr_commit`。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_sec_index_entry_low` | 二级索引删除的低层实现：检索模式选择、在线状态分派、`ROW_FOUND` 时置位，删除操作不插新条目。 |
| `storage/innobase/row/row0upd.cc` | `row_upd_check_references_constraints` | 外键引用校验：`table->referenced_set` 为空或 DD 表直接返回成功，否则 `row_ins_check_foreign_constraint`。 |
| `storage/innobase/lock/lock0lock.cc` | `lock_clust_rec_modify_check_and_lock` | 聚簇记录修改锁：`lock_rec_convert_impl_to_expl` 后请求 `LOCK_X \| LOCK_REC_NOT_GAP`。 |
| `storage/innobase/lock/lock0lock.cc` | `lock_sec_rec_modify_check_and_lock` | 二级记录修改锁：同样请求 `LOCK_X \| LOCK_REC_NOT_GAP`；加锁成功或新建锁时才以 `page_update_max_trx_id` 刷新页上最大事务号。 |
| `storage/innobase/row/row0mysql.cc` | `row_mysql_handle_errors` | 错误分派：`DB_LOCK_WAIT` 挂起后重跑、语句级错误回滚到 savepoint、死锁回滚整个事务。 |
| `sql/handler.cc` | `handler::ha_delete_row` | server 层到引擎的删除入口：`mark_trx_read_write`、`delete_row`，成功后 `binlog_log_row` 写 `Delete_rows_log_event`。 |
| `sql/sql_delete.cc` | `DeleteCurrentRowAndProcessTriggers` | BEFORE 触发器 → `ha_delete_row` → AFTER 触发器，并累加 `deleted_rows`。 |

## 与主核 / AI 的关系

主核 `engine/db/` 的 L2 索引层与 L3 查询层同样要处理「删掉的记录何时才算真没了」，可对照的设计点有三个：

- 主核若采用「删除标记 + 后台清理」的延迟删除，需要明确标记位存在哪、由哪个角色负责真正摘除，以及未清理期间索引扫描如何跳过它。
- 主核的删除是否也把「先聚簇 / 先主索引、再二级索引」的顺序固定下来，以及二级索引的标记是否复用同一个日志协议。
- 主核是否存在无 undo 的短生命周期表（临时表 / 内存表），其撤销路径是否像 `row_delete_for_mysql_using_cursor` 那样依赖保存游标位置而非回滚日志。

**架构推论：**InnoDB 把「标记」与「物理清除」拆成两个机制、并让撤销能力成为两条删除路径的分界线（有 undo 才能回滚、没 undo 就存游标），这一分层值得主核参考；主核是否采用同等分层，取决于它的日志协议能否在崩溃恢复后重建出同样的「已标记未清除」状态。

## 参考

- [MySQL 8.0 Reference Manual：InnoDB 多版本并发控制（delete-mark 与 undo 的关系）](https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html) —— 官方对删除标记、undo 记录与 purge 的分工说明。
- [MySQL 8.0 Reference Manual：InnoDB 锁与事务模型](https://dev.mysql.com/doc/refman/8.0/en/innodb-locking.html) —— 记录锁与行级锁语义的官方说明。
- [MySQL 官方源码：storage/innobase/row/row0mysql.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0mysql.cc) —— `row_delete_for_mysql_using_cursor` 与 `row_update_for_mysql` 的定义处。
- [MySQL 官方源码：storage/innobase/btr/btr0cur.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/btr/btr0cur.cc) —— 聚簇与二级索引删除标记的实现处。
- [MySQL 官方源码：storage/innobase/row/row0upd.cc](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0upd.cc) —— 删除更新图与二级索引删除步骤的实现处。
