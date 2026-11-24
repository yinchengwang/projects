---
project: mysql-server
feature: mvcc-visibility
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/row/row0sel.cc
    symbols: [row_search_mvcc, row_sel_get_clust_rec]
---

# MVCC 可见性判定

## 一句话职责

在一致性读里按读视图判定「某一行的某个版本对当前事务是否可见」：看得见就直接用，看不见就沿 undo 链回溯到上一个可见版本，回溯不到就判定这行在快照里根本不存在；锁读走另一条分支——不判版本可见性，而是加锁冲突时决定等还是不等。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责在一致性读里按读视图判定某版本对当前事务是否可见、以及在写锁下判定是否需要等待；**不负责锁的排队（J02）**、**不负责 undo 的存储（J05）**。

## 机制是什么

### 可见性判定发生在哪一层

判定发生在**存储引擎层**，不是 server 层。server 层把「读一行」这个动作交给 handler，handler 调 InnoDB 的索引扫描接口，InnoDB 在扫描每一行时自己判断这一行属于哪个版本、这个版本对本事务可不可见。

`storage/innobase/row/row0sel.cc` 里有两条并行的扫描入口，走的判定逻辑不同：

| 入口 | 形态 | 可见性判定的位置 |
|---|---|---|
| `row_search_mvcc` | handler 接口的索引扫描（`row_prebuilt_t` 驱动） | PHASE 4 的分支：`prebuilt->select_lock_type != LOCK_NONE` 走加锁分支，否则走一致性读分支 |
| `row_sel` | 查询优化器内部的老式 select 步骤（`sel_node_t` 驱动） | PHASE 3：`consistent_read` 为真时按 `node->read_view` 判定 |

判定的**共同底座**是 `ReadView::changes_visible`（读视图的具体结构与该函数的实现见 J04 读视图页）。本页关注的是「谁来调它、调完之后怎么办」。

### 一致性读的判定：两级对象

一致性读要判定的**不是行，而是「行的某个版本」**。InnoDB 的记录里带了修改它的事务号，因此判定一个聚簇索引记录是否可见，等价于判定「这个记录的事务号在视图里可不可见」。

- **聚簇索引记录**：事务号就存在记录里。`lock_clust_rec_cons_read_sees`（`storage/innobase/lock/lock0lock.cc`）取 `row_get_rec_trx_id(rec, index, offsets)` 得到 `trx_id`，再调 `view->changes_visible(trx_id, index->table->name)`。
- **二级索引记录**：二级索引记录里**没有**完整的事务号历史，只能拿到页上的最大事务号 `page_get_max_trx_id(page_align(rec))`。`lock_sec_rec_cons_read_sees` 用它调 `view->sees(max_trx_id)`。源码注释写明这条路径的局限：「二级索引页上关于修改的信息太少，即使判定为 false，当前版本也可能是对的，但必须到聚簇索引记录里确认」。

所以：**二级索引上扫到的记录不能直接判可见性，必须回聚簇索引取真正的版本**。

### 三种「不可见」的结果

判定不可见之后，代码分三条路走，**结果完全不同**：

1. **回溯到更早的可见版本**。`row_vers_build_for_consistent_read`（`storage/innobase/row/row0vers.cc`）沿 undo 链逐版本回溯，每个版本都重新问一次 `view->changes_visible`，直到找到可见的版本，把它复制出来返回。
2. **这行在快照里不存在**。回溯链走到头（`prev_version == nullptr`），说明这行是快照之后才插入的。调用方拿到 `old_vers == nullptr`，直接 `goto next_rec` 跳过这行。
3. **历史已经被 purge 掉**。`row_vers_build_for_consistent_read` 里 `trx_undo_prev_version_build` 返回的 `purge_sees` 为假时，函数返回 `DB_MISSING_HISTORY`——undo 链断了，找不到能覆盖当前视图的历史版本。

### 锁读：判的不是版本，是「要不要等」

`prebuilt->select_lock_type != LOCK_NONE` 时走的是完全不同的分支——**不查视图、不回溯版本**。它做的是「加锁冲突时怎么办」的判定：

- 冲突且不能半一致读 → 返回 `DB_LOCK_WAIT`，进入等待（**锁的排队属于 J02，本页不展开**）。
- 冲突但可以半一致读（semi-consistent read）→ 返回 `DB_SKIP_LOCKED`，改为读取**最后一个已提交版本**，不等锁。

半一致读的准入条件写在 `row_search_mvcc` 里：

```
const bool use_semi_consistent =
    prebuilt->row_read_type == ROW_READ_TRY_SEMI_CONSISTENT &&
    !unique_search && index == clust_index && !trx_is_high_priority(trx);
```

四个条件缺一不可：**读类型是「尝试半一致读」、不是唯一搜索、扫的是聚簇索引、事务不是高优先级**。

半一致读构造旧版本用的是 `row_vers_build_for_semi_consistent_read`（`row0vers.cc`），它**不看读视图**，判据是 `trx_rw_is_active(version_trx_id, false)`——事务不活跃就说明已提交，这个版本可用。

## 怎么实现的

### `row_search_mvcc`：handler 扫描路径

`row_search_mvcc`（`storage/innobase/row/row0sel.cc`）是 handler 接口的索引扫描主函数，签名是 `row_search_mvcc(byte *buf, page_cur_mode_t mode, row_prebuilt_t *prebuilt, ulint match_mode, const ulint direction)`。

#### 前置检查与分流

进入主循环前，函数先做一串早退检查：

| 检查 | 返回值 | 位置 |
|---|---|---|
| `prebuilt->index->type & DICT_FTS` | `DB_END_OF_INDEX` | 注释写明 FTS 用反向倒排索引实现，不支持这种传统索引查询 |
| `dict_table_is_discarded(prebuilt->table)` | `DB_TABLESPACE_DELETED` | 表被丢弃 |
| `prebuilt->table->ibd_file_missing` | `DB_TABLESPACE_NOT_FOUND` | .ibd 文件缺失 |
| `!prebuilt->index_usable` | `DB_MISSING_HISTORY` | 索引不可用 |
| `prebuilt->index->is_corrupted()` | `DB_CORRUPTION` | 索引已损坏 |

`direction == 0` 表示开启一次新扫描：重置 `n_rows_fetched`、`n_fetch_cached`、`fetch_cache_first`、`m_end_range`，清空记录缓冲，若 `prebuilt->sel_graph == nullptr` 则调 `row_prebuild_sel_graph` 造一个哑查询图。

`direction != 0` 表示继续取下一行：若方向与 `prebuilt->fetch_direction` 不同，**清空取数缓存**并断言 `record_buffer == nullptr`（源码注释：滚动游标不使用记录缓冲）；若缓存里还有行，调用 `row_sel_dequeue_cached_row_for_mysql` 直接吐出一行。

`prebuilt->n_rows_fetched` 超过 `1000000000` 时被重置为 `500000000`——源码注释是「Prevent wrap-over」，即防止计数器回绕。

#### 视图的分配

视图的分配点由 `prebuilt->sql_stat_start` 与 `prebuilt->select_lock_type` 决定：

```
} else if (prebuilt->select_lock_type == LOCK_NONE) {
  /* This is a consistent read */
  /* Assign a read view for the query */

  if (!srv_read_only_mode) {
    trx_assign_read_view(trx);
  }

  prebuilt->sql_stat_start = false;
} else {
wait_table_again:
  err = lock_table(0, index->table,
                   prebuilt->select_lock_type == LOCK_S ? LOCK_IS : LOCK_IX,
                   thr);
  ...
}
```

- `select_lock_type == LOCK_NONE` → 一致性读 → 调 `trx_assign_read_view(trx)` 分配读视图（只读模式 `srv_read_only_mode` 下跳过）。
- 否则 → 锁读 → 调 `lock_table`，`LOCK_S` 映射为 `LOCK_IS`、`LOCK_X` 映射为 `LOCK_IX`。`lock_table` 返回非 `DB_SUCCESS` 时置 `table_lock_waited = true` 并跳 `lock_table_wait`。

而 `!prebuilt->sql_stat_start` 时（不是语句的第一次取数）不重新分配视图——这一段还带一条硬检查：视图不活跃、且非只读模式、且 `select_lock_type == LOCK_NONE` 时，打 `ib::error(ER_IB_MSG_1031)` 并 `ut_error` 中止。

#### 一致性读分支：PHASE 4 的可见性判定

PHASE 4 拿到一条用户记录、算完 offsets 之后，按 `prebuilt->select_lock_type` 分成两支。

**分支一：一致性读（`select_lock_type == LOCK_NONE`）**

```
if (trx->isolation_level == TRX_ISO_READ_UNCOMMITTED) {
  /* Do nothing: we let a non-locking SELECT read the
  latest version of the record */

} else if (index == clust_index) {
  if (srv_force_recovery < 5 &&
      !lock_clust_rec_cons_read_sees(rec, index, offsets,
                                     trx_get_read_view(trx))) {
    ...
    err = row_sel_build_prev_vers_for_mysql(
        trx->read_view, clust_index, prebuilt, rec, &offsets, &heap,
        &old_vers, need_vrow ? &vrow : nullptr, &mtr,
        prebuilt->get_lob_undo());

    if (err != DB_SUCCESS) {
      goto lock_wait_or_error;
    }

    if (old_vers == nullptr) {
      /* The row did not exist yet in
      the read view */
      goto next_rec;
    }

    rec = old_vers;
    prev_rec = rec;
    ...
  }
} else {
  /* We are looking into a non-clustered index, ... */

  if (!srv_read_only_mode &&
      !lock_sec_rec_cons_read_sees(rec, index, trx->read_view)) {
    switch (row_search_idx_cond_check(buf, prebuilt, rec, offsets)) {
      case ICP_NO_MATCH:
        goto next_rec;
      case ICP_OUT_OF_RANGE:
        err = DB_RECORD_NOT_FOUND;
        goto idx_cond_failed;
      case ICP_MATCH:
        goto requires_clust_rec;
    }
    ut_error;
  }
}
```

三个子分支的判据：

- **READ UNCOMMITTED**（`trx->isolation_level == TRX_ISO_READ_UNCOMMITTED`）：什么都不做，直接读最新版本。源码注释就是「we let a non-locking SELECT read the latest version of the record」。
- **扫的是聚簇索引**：`srv_force_recovery < 5` **且** `lock_clust_rec_cons_read_sees` 判不可见时，才回溯旧版本。`srv_force_recovery < 5` 这个条件是源码注释写明的理由——「如果强制恢复级别很高，我们尽量避免崩溃，跳过这次查找」。回溯失败（`err != DB_SUCCESS`）跳 `lock_wait_or_error`；回溯到头（`old_vers == nullptr`）跳 `next_rec` 跳过该行。
- **扫的是二级索引**：`!srv_read_only_mode` **且** `lock_sec_rec_cons_read_sees` 判不可见时，先做 ICP 检查（`row_search_idx_cond_check`）：不匹配就跳过（`ICP_NO_MATCH`），超出范围就返回 `DB_RECORD_NOT_FOUND`（`ICP_OUT_OF_RANGE`），匹配才跳 `requires_clust_rec` 标签去取聚簇记录。**注意这里没有 `trx->isolation_level` 判断**——READ UNCOMMITTED 下二级索引仍会走这条判定，只是 `lock_sec_rec_cons_read_sees` 内部对临时表等有短路。

#### 锁读分支：等待还是不等

锁读分支先调 `row_compare_row_to_range` 判定「这一行与扫描范围的关系」，得到三个布尔量：`row_can_be_in_range`、`gap_can_intersect_range`、`row_must_be_at_end`。这三个量决定锁的类型：

| `row_can_be_in_range` | `gap_can_intersect_range` | 锁类型 | 动作 |
|---|---|---|---|
| 真 | 真 | `LOCK_ORDINARY` | 记录 + 间隙 |
| 真 | 假 | `LOCK_REC_NOT_GAP` | 只锁记录 |
| 假 | 真 | `LOCK_GAP` | 只锁间隙 |
| 假 | 假 | —— | `err = DB_RECORD_NOT_FOUND`，`goto normal_return` |

然后调 `sel_set_rec_lock`，**半一致读时传 `SELECT_SKIP_LOCKED` 而不是 `prebuilt->select_mode`**——源码注释解释：「这样我们不会浪费时间去创建一个 WAITING 锁，反正也不会等它」。

`sel_set_rec_lock` 的返回值分派（`DB_SUCCESS_LOCKED_REC` 归一化成 `DB_SUCCESS` 后继续）：

| 返回值 | 处理 |
|---|---|
| `DB_SUCCESS_LOCKED_REC` | 记 `prebuilt->new_rec_lock[row_prebuilt_t::LOCK_PCUR] = true`（当 `trx->releases_non_matching_rows()` 为真），转 `DB_SUCCESS` |
| `DB_SUCCESS` | 若 `row_must_be_at_end` 则置 `prebuilt->m_stop_tuple_found = true` |
| `DB_SKIP_LOCKED` | 若 `prebuilt->select_mode == SELECT_SKIP_LOCKED` 则跳过该行；否则走半一致读：调 `row_sel_build_committed_vers_for_mysql` 取最后一个已提交版本 |
| `DB_LOCK_WAIT` | 清 `prebuilt->new_rec_lock`，跳 `lock_wait_or_error`（**等待与排队属 J02**） |
| `DB_RECORD_NOT_FOUND` | 空间索引跳 `next_rec`，否则跳 `lock_wait_or_error` |

**半一致读拿到 `old_vers == nullptr` 时，源码注释是「The row was not yet committed」，直接 `goto next_rec` 跳过该行**；拿到非空则 `rec = old_vers`、`prev_rec = rec`，继续用这个旧版本往下走。

#### delete-mark 与最终结果

可见性判定通过后，仍有一道 delete-mark 判定：

```
if (rec_get_deleted_flag(rec, comp)) {
  /* The record is delete-marked: we can skip it */

  /* No need to keep a lock on a delete-marked record in lower isolation
  levels - it's similar to when Server sees the WHERE condition doesn't match
  and calls unlock_row(). */
  prebuilt->try_unlock(true);
  ...
  if (index == clust_index && unique_search && !prebuilt->used_in_HANDLER) {
    err = DB_RECORD_NOT_FOUND;
    goto normal_return;
  }

  goto next_rec;
}
```

判定为 delete-mark 后**先 `prebuilt->try_unlock(true)` 释放这条记录上的锁**（源码注释说明理由：在低隔离级别下没必要为 delete-mark 的记录保留锁，效果等同于 server 层发现 WHERE 不匹配后调 `unlock_row()`）。若同时是聚簇索引、唯一搜索、且不是 HANDLER 查询，直接返回 `DB_RECORD_NOT_FOUND`——源码注释解释这是「跳过 delete-mark 记录后的 next key lock 优化」，只对聚簇索引的唯一搜索成立。

### `row_sel_get_clust_rec`：老式 select 步骤路径

`row_sel_get_clust_rec`（`storage/innobase/row/row0sel.cc`）是 `row_sel` 路径上从二级索引记录取聚簇记录的实现，签名是 `row_sel_get_clust_rec(sel_node_t *node, plan_t *plan, rec_t *rec, que_thr_t *thr, rec_t **out_rec, mtr_t *mtr)`。

它的分支由 `node->read_view` 决定——**`node->read_view` 为空就是锁读，非空就是一致性读**：

```
if (!node->read_view) {
  /* Try to place a lock on the index record */

  ulint lock_type;
  trx_t *trx;

  trx = thr_get_trx(thr);

  lock_type = trx->skip_gap_locks() ? LOCK_REC_NOT_GAP : LOCK_ORDINARY;

  err = lock_clust_rec_read_check_and_lock(
      lock_duration_t::REGULAR, plan->clust_pcur.get_block(), clust_rec,
      index, offsets, SELECT_ORDINARY,
      static_cast<lock_mode>(node->row_lock_mode), lock_type, thr);
  ...
} else {
  /* This is a non-locking consistent read: if necessary, fetch
  a previous version of the record */

  old_vers = nullptr;

  if (!lock_clust_rec_cons_read_sees(clust_rec, index, offsets,
                                     node->read_view)) {
    err = row_sel_build_prev_vers(node->read_view, index, clust_rec, &offsets,
                                   &heap, &plan->old_vers_heap, &old_vers, mtr);

    if (err != DB_SUCCESS) {
      goto err_exit;
    }

    clust_rec = old_vers;

    if (clust_rec == nullptr) {
      goto func_exit;
    }
  }
  ...
}
```

**锁读分支**：锁类型由 `trx->skip_gap_locks()` 决定——跳过间隙锁用 `LOCK_REC_NOT_GAP`，否则用 `LOCK_ORDINARY`。锁模式取 `node->row_lock_mode`。返回 `DB_SUCCESS` / `DB_SUCCESS_LOCKED_REC` 视为成功，其余跳 `err_exit`。

**一致性读分支**：判不可见时调 `row_sel_build_prev_vers`（它转调 `row_vers_build_for_consistent_read`），`old_vers == nullptr` 表示「这行在视图里不存在」，`goto func_exit` 返回空记录。

一致性读分支末尾还有一道**二级索引与聚簇记录的配对检查**：

```
if (old_vers ||
    rec_get_deleted_flag(rec, dict_table_is_comp(plan->table))) {
  bool rec_equal;

  err = row_sel_sec_rec_is_for_clust_rec(rec, plan->index, clust_rec, index,
                                         thr, rec_equal);
  if (err != DB_SUCCESS) {
    goto err_exit;
  } else if (!rec_equal) {
    goto func_exit;
  }
}
```

源码注释说明了必要性：「如果回溯到了更早的版本，或者二级索引记录被 delete-mark 了，那么与 `clust_rec` 对应的二级索引记录可能不是 `rec`；这时必须忽略这行，因为在我们的快照里 `rec` 根本不存在」。**从二级索引记录无法直接看出它对应哪个事务，必须回到聚簇索引记录才知道。** `row_sel_sec_rec_is_for_clust_rec` 逐个用户定义的列比较二级索引记录与聚簇索引记录的对应列值，**按 collation 比较而不是二进制比较**（函数注释明确写了这一点）；聚簇记录被 delete-mark 时直接判 `is_equal = false` 并返回。

### `row_sel_get_clust_rec_for_mysql`：handler 路径上的对应物

handler 路径上取聚簇记录的是 `Row_sel_get_clust_rec_for_mysql::operator()`（同在 `row0sel.cc`），它是 `row_sel_get_clust_rec` 的对应实现。关键差异：

- **锁读分支**（`prebuilt->select_lock_type != LOCK_NONE`）：调 `lock_clust_rec_read_check_and_lock`，锁类型**固定为 `LOCK_REC_NOT_GAP`**——源码注释给的理由是「我们是用唯一条件搜聚簇记录的，所以设 `LOCK_REC_NOT_GAP` 类型的锁」。
- **一致性读分支**：判定条件多了隔离级别这一层：

```
/* If the isolation level allows reading of uncommitted data,
   then we never look for an earlier version */

if (trx->isolation_level > TRX_ISO_READ_UNCOMMITTED &&
    !lock_clust_rec_cons_read_sees(clust_rec, clust_index, *offsets,
                                   trx_get_read_view(trx))) {
```

**`trx->isolation_level > TRX_ISO_READ_UNCOMMITTED` 这个前置条件是这一处与 `row_search_mvcc` 的关键差异**：`row_search_mvcc` 的一致性读分支把 READ UNCOMMITTED 的短路放在最外层，而这里放在 `lock_clust_rec_cons_read_sees` 之前——两者效果一致（RU 下都直接用最新版本），但这处是显式地在**调用判定函数之前**就短路。

这个函数还有一层**旧版本缓存**：`clust_rec != cached_clust_rec` 时才真去回溯并缓存 `cached_clust_rec` / `cached_old_vers`；相同则直接复用缓存的 `old_vers`，但**必须重算 offsets**——源码注释写明「最新版本与旧版本的 offsets 不一定相同，要为 `old_vers` 重新计算 offsets」。

### 半一致读与 `row_compare_row_to_range`

`row_compare_row_to_range`（`row0sel.cc`）是锁读路径上的范围判定辅助函数，它**不改任何参数**，只返回一个三布尔量的结构体。它的第一个早退分支：

```
if (!set_also_gap_locks || trx->skip_gap_locks() ||
    (unique_search && !rec_get_deleted_flag(rec, comp)) ||
    dict_index_is_spatial(index) ||
    (index == clust_index && mode == PAGE_CUR_GE && direction == 0 &&
     dtuple_get_n_fields_cmp(search_tuple) ==
         dict_index_get_n_unique(index) &&
     0 == cmp_dtuple_rec(search_tuple, rec, index, offsets))) {
  row_to_range_relation.gap_can_intersect_range = false;
  return (row_to_range_relation);
}
```

五种情形任一成立就**不需要锁间隙**（只可能锁记录）：不设间隙锁、事务跳过间隙锁、唯一搜索且记录未 delete-mark、空间索引、或者是「聚簇索引上 `PAGE_CUR_GE` 方向为 0 且搜索元组字段数等于唯一键字段数且完全相等」。

第二种早退是 `prebuilt->used_in_HANDLER`——源码注释写明「我不知道怎么处理 HANDLER 接口」，直接返回默认值。

第三种早退针对「行已在扫描范围末尾」的情形，源码注释解释了限制条件：索引本身可以按列声明升序或降序，游标可能是 `PAGE_CUR_G(E)` 或 `PAGE_CUR_L(E)`，扫描方向可能是 `0`、`ROW_SEL_NEXT` 或 `ROW_SEL_PREV`，还可能是带重复值的二级索引——所以只处理「沿索引自然顺序迭代」的常见情形。

### `set_also_gap_locks` 的由来

`set_also_gap_locks` 初值为 `true`，只在两处被置为 `false`：

```
if (prebuilt->table->skip_gap_locks() ||
    (trx->skip_gap_locks() && prebuilt->select_lock_type != LOCK_NONE &&
     trx->mysql_thd != nullptr && thd_is_query_block(trx->mysql_thd))) {
  /* It is a plain locking SELECT and the isolation
  level is low: do not lock gaps */

  /* Reads on DD tables dont require gap-locks as serializability
  between different DDL statements is achieved using
  metadata locks */
  set_also_gap_locks = false;
}
```

源码注释给的两条理由：**低隔离级别的普通锁读不锁间隙**；**DD 表的读不需要间隙锁，因为不同 DDL 语句之间的可串行化由元数据锁保证**。

### 错误出口

`row_search_mvcc` 的错误出口是 `lock_wait_or_error` → `lock_table_wait`：

```
lock_wait_or_error:
  if (UNIV_UNLIKELY(prebuilt->row_read_type == ROW_READ_DID_SEMI_CONSISTENT)) {
    prebuilt->row_read_type = ROW_READ_TRY_SEMI_CONSISTENT;
  }
  did_semi_consistent_read = false;

  if (!dict_index_is_spatial(index)) {
    pcur->store_position(&mtr);
  }

lock_table_wait:
  mtr_commit(&mtr);
  mtr_has_extra_clust_latch = false;

  trx->error_state = err;
```

进入错误出口时**把 `ROW_READ_DID_SEMI_CONSISTENT` 复位成 `ROW_READ_TRY_SEMI_CONSISTENT`**——即「这次半一致读做过了，下次可以再试」。`trx->error_state = err` 是把错误码挂到事务上，由上层（`row_mysql_handle_errors` 一类）读取并处理。

`next_rec` 标签处同样复位这两个标志，并重置 `prebuilt->new_rec_lock`。

### `row_sel` 路径的判定

`row_sel`（`row0sel.cc`）是老式 select 步骤，可见性判定在 PHASE 3：

```
if (consistent_read) {
  /* This is a non-locking consistent read: if necessary, fetch
  a previous version of the record */

  if (index->is_clustered()) {
    if (!lock_clust_rec_cons_read_sees(rec, index, offsets,
                                       node->read_view)) {
      err = row_sel_build_prev_vers(node->read_view, index, rec, &offsets,
                                    &heap, &plan->old_vers_heap, &old_vers,
                                    &mtr);
      ...
      if (old_vers == nullptr) {
        /* The record does not exist
        in our read view. Skip it, but
        first attempt to determine
        whether the index segment we
        are searching through has been
        exhausted. */
        ...
        if (!row_sel_test_end_conds(plan)) {
          goto table_exhausted;
        }
        goto next_rec;
      }

      rec = old_vers;
    }
  } else if (!srv_read_only_mode &&
             !lock_sec_rec_cons_read_sees(rec, index, node->read_view)) {
    cons_read_requires_clust_rec = true;
  }
}
```

与 handler 路径的两处差异：

- **`old_vers == nullptr` 时不直接跳过**，而是先 `row_sel_fetch_columns` 取出列值，再 `row_sel_test_end_conds(plan)` 判定「索引段是否已扫完」——条件不满足才 `goto table_exhausted`，满足才 `goto next_rec`。源码注释写明「跳过它，但先试着确定我们正在搜索的索引段是否已经耗尽」。
- **二级索引判不可见时不立即回聚簇索引**，而是置 `cons_read_requires_clust_rec = true`，留到 PHASE 5 再取聚簇记录。PHASE 4 的 delete-mark 判定因此带了一个额外条件：`rec_get_deleted_flag(rec, ...) && !cons_read_requires_clust_rec`——**若是二级索引且判定不可见，即便二级记录已 delete-mark 也不能跳过**，因为它可能对应一个可见的旧版本。

## 关键取舍

- **源码事实：**`row_search_mvcc` 的一致性读分支在 `srv_force_recovery < 5` 时才回溯旧版本，源码注释写明「如果强制恢复级别很高，我们尽量避免崩溃，跳过这次查找」。**架构推论：**这是把「数据可恢复性」置于「读结果精确性」之上——在已经从磁盘损坏中抢救数据的场景里，少读一行旧版本比在回溯 undo 时崩溃更可接受。代价是 `srv_force_recovery` 达到 5 及以上时，一致性读可能返回比快照更晚的版本。

- **源码事实：**`lock_sec_rec_cons_read_sees` 只用二级索引页上的 `page_get_max_trx_id` 判定，函数注释写明「二级索引页上关于修改的信息太少，即使判定为 false，当前版本也可能是对的，但必须从聚簇索引记录确认」。**架构推论：**二级索引的可见性判定是一个**保守的否证**（先假设可能不可见，再回聚簇索引确认），而不是肯定判断。这让二级索引扫描的每次判定都可能在后面追加一次聚簇索引查找——`row_search_mvcc` 里 `requires_clust_rec` 标签承担的就是这次追加查找。代价是二级索引扫描的成本不可预测。

- **源码事实：**`row_sel_get_clust_rec_for_mysql::operator()` 在 `trx->isolation_level > TRX_ISO_READ_UNCOMMITTED` 时才判可见性。**架构推论：**READ UNCOMMITTED 被显式排除在「版本回溯」之外，意味着 RU 下读到的是**正在被写的版本本身**，而不是任何历史版本。这是 RU 隔离级别语义的直接实现。

- **源码事实：**半一致读的构造走 `row_vers_build_for_semi_consistent_read`，其判据是 `trx_rw_is_active(version_trx_id, false)`，**全程不看读视图**。**架构推论：**半一致读服务于「需要加锁但不想等」的场景，它要的是「最新的已提交数据」而不是「快照时刻的数据」——这两者在 REPEATABLE READ 下是不同的语义。源码注释也点明了这一点：半一致读只在「不使用间隙锁」时使用（`ut_ad(!trx->skip_gap_locks())` 与 `ut_ad(prebuilt->row_read_type == ROW_READ_WITH_LOCKS)` 两条断言）。

- **源码事实：**delete-mark 记录在可见性判定通过后被 `prebuilt->try_unlock(true)` 释放锁，源码注释写明理由是「低隔离级别下没必要为 delete-mark 的记录保留锁，效果等同于 server 层发现 WHERE 不匹配后调 `unlock_row()`」。**架构推论：**这把「引擎层的可见性判定」与「server 层的行过滤」做了等价化处理，减少了持锁记录的数量。代价是引擎在这个点上不再持有锁，若之后 server 层改变判定结果需要重新加锁。

- **源码事实：**`row_sel_get_clust_rec_for_mysql` 缓存了 `cached_clust_rec` / `cached_old_vers`，复用时必须为 `old_vers` 重算 offsets，源码注释写明「最新版本与旧版本的 offsets 不一定相同」。**架构推论：**缓存避免了同一条聚簇记录被反复回溯 undo 链——二级索引扫描时多条二级记录可能指向同一条聚簇记录。代价是缓存命中时要额外做一次 `rec_get_offsets`。

- **源码事实：**`row_sel_sec_rec_is_for_clust_rec` 的函数注释写明比较**不是二进制比较，字符字段按 collation 比较**。**架构推论：**二级索引里的字符列是按列的 collation 排序存储的，与聚簇索引记录里的同一列比较时必须用同一套 collation 规则，否则在重排序规则（collation change）之后会误判「这条二级记录不属于这条聚簇记录」，从而漏掉本该可见的行。

- **源码事实：**`row_search_mvcc` 在 `trx->isolation_level == TRX_ISO_READ_UNCOMMITTED` 时**完全跳过**版本判定，直接读最新版本。**架构推论：**这是把 RU 的「脏读」语义做成了「不做回溯」而非「回溯到任意版本」——省掉了整个 undo 链遍历。代价是 RU 下的读会看到未提交的写。

- **源码事实：**`row_search_mvcc` 的锁读分支里，`row_to_range_relation.row_can_be_in_range` 为假且 `gap_can_intersect_range` 也为假时，直接 `err = DB_RECORD_NOT_FOUND` 并 `goto normal_return`。**架构推论：**这两条判定把「这一行已确定在扫描范围之外」提前成���，使得扫描不必再走到取记录、格式转换那一步就能终止。代价是这条判定依赖 `row_compare_row_to_range` 里的启发式（源码注释自己承认「我认为我们正确处理了半一致读，但证明相当复杂」）。

- **源码事实：**`row_search_mvcc` 的一致性读分支里，二级索引判不可见后要先做 ICP 检查（`row_search_idx_cond_check`）再决定是否回聚簇索引。**架构推论：**这一步把「索引条件不匹配」这个更便宜的判据前置，避免为一个注定不满足 WHERE 条件的行去做聚簇索引查找。代价是 ICP 的判定本身要读二级索引记录上的列值。

- **源码事实：**`row_search_mvcc` 的 `unique_search` 判定要求 `match_mode == ROW_SEL_EXACT && dict_index_is_unique(index) && dtuple_get_n_fields(search_tuple) == dict_index_get_n_unique(index) && (index->is_clustered() || !dtuple_contains_null(search_tuple))`。**架构推论：**唯一搜索能用 `LOCK_REC_NOT_GAP` 代替 next-key 锁，依赖「搜索键完整匹配唯一键」这一前提——源码注释专门解释了为什么二级索引要排除含 NULL 的情况：「唯一二级索引里，若某列是 SQL NULL，同一个键值可能对应多行」。这是 MySQL 的 NULL 不参与唯一性约束这一语义在 InnoDB 锁层面的落地。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/row/row0sel.cc` | `row_search_mvcc` | handler 接口的索引扫描主函数：PHASE 4 按 `prebuilt->select_lock_type` 分流到加锁分支或一致性读分支；一致性读分支按 `TRX_ISO_READ_UNCOMMITTED`、`index == clust_index`、二级索引三种情形分别处理；`old_vers == nullptr` 跳 `next_rec` |
| `storage/innobase/row/row0sel.cc` | `row_sel_get_clust_rec` | 老式 select 步骤路径上从二级索引记录取聚簇记录：按 `node->read_view` 是否为空分流到锁读（`lock_clust_rec_read_check_and_lock`，锁类型由 `trx->skip_gap_locks()` 决定）或一致性读（`lock_clust_rec_cons_read_sees` + `row_sel_build_prev_vers`） |
| `storage/innobase/row/row0sel.cc` | `Row_sel_get_clust_rec_for_mysql::operator()` | handler 路径上取聚簇记录：锁读分支固定用 `LOCK_REC_NOT_GAP`；一致性读分支以 `trx->isolation_level > TRX_ISO_READ_UNCOMMITTED` 为前置条件才判可见性；缓存 `cached_clust_rec` / `cached_old_vers` 并为旧版本重算 offsets |
| `storage/innobase/row/row0sel.cc` | `row_compare_row_to_range` | 锁读路径的范围判定：返回 `row_can_be_in_range` / `gap_can_intersect_range` / `row_must_be_at_end` 三布尔量，决定 `LOCK_ORDINARY` / `LOCK_REC_NOT_GAP` / `LOCK_GAP` |
| `storage/innobase/row/row0sel.cc` | `sel_set_rec_lock` | 加锁的统一入口：聚簇索引走 `lock_clust_rec_read_check_and_lock`，空间索引走 `sel_set_rtr_rec_lock`，其余走 `lock_sec_rec_read_check_and_lock`；半一致读时传 `SELECT_SKIP_LOCKED` |
| `storage/innobase/row/row0sel.cc` | `row_sel_sec_rec_is_for_clust_rec` | 二级索引记录与聚簇记录的配对校验：逐用户定义列按 collation 比较；聚簇记录已 delete-mark 时判 `is_equal = false` |
| `storage/innobase/row/row0sel.cc` | `row_sel_build_prev_vers` | 一致性读回溯旧版本的封装：建/复用 `old_vers_heap`（首次 `mem_heap_create(512, ...)`），转调 `row_vers_build_for_consistent_read` |
| `storage/innobase/row/row0sel.cc` | `row_sel_build_prev_vers_for_mysql` | handler 路径的回溯封装：堆大小 `mem_heap_create(200, ...)`，转调 `row_vers_build_for_consistent_read`，额外传 `lob_undo` |
| `storage/innobase/row/row0sel.cc` | `row_sel_build_committed_vers_for_mysql` | 半一致读的旧版本构造：转调 `row_vers_build_for_semi_consistent_read`，堆大小取 `rec_offs_size(*offsets)` |
| `storage/innobase/row/row0sel.cc` | `row_sel` | 老式 select 步骤：PHASE 3 按 `node->read_view` 判定一致性读；`old_vers == nullptr` 时先 `row_sel_test_end_conds` 再决定跳过还是结束；PHASE 4 的 delete-mark 判定带 `!cons_read_requires_clust_rec` 条件 |
| `storage/innobase/row/row0sel.cc` | `row_sel_try_search_shortcut` | `row_sel` 路径上的 AHI 快捷搜索：聚簇索引用 `lock_clust_rec_cons_read_sees` 判定，不可见则返回 `SEL_RETRY` 回退到常规路径 |
| `storage/innobase/lock/lock0lock.cc` | `lock_clust_rec_cons_read_sees` | 聚簇索引记录的一致性读判定：只读模式或临时表直接可见，否则 `view->changes_visible(row_get_rec_trx_id(...), index->table->name)` |
| `storage/innobase/lock/lock0lock.cc` | `lock_sec_rec_cons_read_sees` | 二级索引记录的一致性读判定：恢复中返回 `false`，临时表返回 `true`，否则 `view->sees(page_get_max_trx_id(page_align(rec)))` |
| `storage/innobase/lock/lock0lock.cc` | `lock_clust_rec_read_check_and_lock` | 锁读路径的聚簇记录加锁：`srv_read_only_mode` 或临时表直接成功；否则 `lock_rec_convert_impl_to_expl` 后调 `lock_rec_lock` |
| `storage/innobase/row/row0vers.cc` | `row_vers_build_for_consistent_read` | 一致性读回溯旧版本：沿 undo 链逐版本 `view->changes_visible`；`prev_version == nullptr` 表示快照后新插入；`purge_sees` 为假返回 `DB_MISSING_HISTORY` |
| `storage/innobase/row/row0vers.cc` | `row_vers_build_for_semi_consistent_read` | 半一致读的旧版本构造：判据是 `trx_rw_is_active(version_trx_id, false)`，**不查读视图**；事务不活跃即视为已提交版本 |
| `storage/innobase/trx/trx0trx.cc` | `trx_assign_read_view` | 一致性读的视图入口：`srv_read_only_mode` 返回 `nullptr`；已有活跃视图则复用；否则调 `trx_sys->mvcc->view_open` |

## 与主核 / AI 的关系

主核（`engine/db/`）的事务层与 InnoDB 的可见性判定没有代码级关联，但判定模型在结构上可以对照（主核侧的实现本页未检出，以下均为对照问题而非断言）：

- **可见性判定与版本存储是否分离。** InnoDB 里 `changes_visible` 只回答「这个版本在这个时间点可见吗」，版本本身存在 undo 链里，由 `row_vers_build_for_consistent_read` 负责回溯。主核的 WAL 与事务层是否也把「读到一个版本」与「这个版本能不能给本次读看」分成两件事，是可直接对照的设计点。
- **二级索引的判定是否只是保守否证。** InnoDB 的 `lock_sec_rec_cons_read_sees` 明确「即使判 false，当前版本也可能是对的，但必须回聚簇索引确认」。主核若有多索引结构，需要明确每个索引能独立支持到什么程度的可见性判定。
- **锁读与一致性读是否共用同一条扫描路径。** InnoDB 在 `row_search_mvcc` 里用 `prebuilt->select_lock_type` 一个判据把两条语义完全不同的路（版本回溯 vs 等待/半一致读）分开，代价是同一个函数里塞了两套逻辑。主核若也有「快照读」与「加锁读」两种模式，这个分叉点放在哪里值得对照。

**架构推论：** InnoDB 把可见性判定放在存储引擎层而非 server 层，意味着判定与「行版本怎么存」必须由同一个组件负责，否则跨层传递版本信息会带来额外开销。主核若把可见性判定放在查询层，则需要额外的接口把「版本」概念从存储层暴露出来——这是两套分层方案的核心代价差异。

**架构推论：** 半一致读读的是「最新已提交版本」而不是「快照版本」，这是一条与快照语义并存的第二套可见性规则。主核若实现了快照隔离，需要显式决定是否也提供这条「读最新已提交」的路径；若提供，就要说清楚它在同一个事务内与快照读并存时的语义关系（同一事务的两次读可能看到不同数据）。

## 参考

- [storage/innobase/row/row0sel.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0sel.cc) —— 本页两个主符号 `row_search_mvcc` 与 `row_sel_get_clust_rec` 的权威来源；同一文件中的 `Row_sel_get_clust_rec_for_mysql::operator()`、`row_compare_row_to_range`、`sel_set_rec_lock`、`row_sel_sec_rec_is_for_clust_rec` 亦为判定与加锁分支的出处。
- [storage/innobase/lock/lock0lock.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/lock/lock0lock.cc) —— `lock_clust_rec_cons_read_sees`、`lock_sec_rec_cons_read_sees`、`lock_clust_rec_read_check_and_lock` 的实现处。
- [storage/innobase/row/row0vers.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/row/row0vers.cc) —— `row_vers_build_for_consistent_read` 与 `row_vers_build_for_semi_consistent_read` 的实现处。
- [InnoDB Multi-Versioning（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html) —— 官方对多版本机制、隐藏字段 `DB_TRX_ID` / `DB_ROLL_PTR` 与版本回溯的说明。
- [Consistent Nonlocking Reads（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/innodb-consistent-read.html) —— 一致性读与快照的官方语义，以及 READ UNCOMMITTED 下读取最新版本的行为。
- [InnoDB Locking（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/innodb-locking.html) —— 记录锁、间隙锁与 next-key 锁的官方定义，对应本页 `LOCK_ORDINARY` / `LOCK_GAP` / `LOCK_REC_NOT_GAP` 三种锁型。
