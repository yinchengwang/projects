---
project: mysql-server
feature: trx-lifecycle
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/trx/trx0trx.cc
    symbols: [trx_commit_for_mysql, trx_create_low]
  - path: storage/innobase/trx/trx0sys.cc
    symbols: [trx_sys_write_max_trx_id]
---

# 事务对象生命周期

## 一句话职责

InnoDB 事务对象（`trx_t`）的生命周期由 `storage/innobase/trx/trx0trx.cc` 与 `storage/innobase/trx/trx0sys.cc` 共同管理：前者负责把事务对象从对象池中取出（`trx_create_low()`）、置为活动态（`trx_start_low()`）、提交（`trx_commit_for_mysql()` → `trx_commit()` → `trx_commit_low()` → `trx_commit_in_memory()`）或回滚（`trx_rollback_for_mysql()` → `trx_rollback_low()`），并在提交时决定 undo 日志的生成与丢弃时机；后者负责把「下一个可分配的事务号」持久化到 TRX_SYS 系统头页（`trx_sys_write_max_trx_id()`），使事务号在反复崩溃重启后不发生重叠。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 InnoDB 事务对象的建立、提交与回滚（含 undo 的生成与丢弃时机）；**不负责行级锁的判定（J02）**、**不负责 MVCC 的可见性判定（K 组）**。

## 机制是什么

### 事务对象的六个状态

`trx_t::state` 是事务生命周期的主状态机，取值来自 `trx_start_low()` / `trx_commit_in_memory()` / `trx_rollback_low()` 的 switch 语句：

| 状态 | 进入时机 | 在本页源码中的处置 |
|---|---|---|
| `TRX_STATE_NOT_STARTED` | `trx_create_low()` 取到对象后立即设置；`trx_init()` 不重置它 | `trx_commit_for_mysql()` 与 `trx_rollback_low()` 遇到它都走「尚未真正开始」的分支 |
| `TRX_STATE_FORCED_ROLLBACK` | `trx_start_low()` 之前被外部标记 | `trx_commit_for_mysql()` 见到它会先调 `trx_start_low(trx, true)` 再提交；`trx_rollback_low()` 直接返回 `DB_SUCCESS` |
| `TRX_STATE_ACTIVE` | `trx_start_low()` 内设置 | 提交与回滚的主路径 |
| `TRX_STATE_PREPARED` | XA PREPARE 阶段 | 提交走 `trx_commit_low()` 的 `trx_write_serialisation_history()`；回滚走 `trx_rollback_low()` 的 `TRX_STATE_PREPARED` 分支 |
| `TRX_STATE_COMMITTED_IN_MEMORY` | `trx_release_impl_and_expl_locks()` 内 `state_transition()` lambda 设置 | 表示提交已在内存中生效但尚未落盘；此时 `trx_commit_for_mysql()` 落到 `ut_error` |
| `TRX_STATE_NOT_STARTED`（回收） | `trx_commit_in_memory()` 尾部 `trx_init()` + `trx_free()` | 事务对象归还对象池 |

### 三个号：id、no、max_trx_id

事务对象上有两个独立的编号，加上系统级的第三个：

- **`trx->id`**：事务标识。分配点在 `trx_start_low()`（`trx_sys_allocate_trx_id()`），只对读写事务分配；纯只读事务在 `trx_start_low()` 的 else 分支里被置为 `trx->id = 0`。分配后同时压入 `trx_sys->rw_trx_ids`，并通过 `trx_sys_rw_trx_add(trx)` 与 `trx_add_to_rw_trx_list(trx)` 挂到活动事务表上。
- **`trx->no`**：序列化号。分配点在 `trx_add_to_serialisation_list()` 里的 `trx_sys_allocate_trx_no()`，受 `trx_sys_serialisation_mutex` 保护。`trx_start_low()` 在事务开始时先把它置为 `TRX_ID_MAX`（源码注释说明该值供 `read_view_open_now` 使用）。
- **`max_trx_id`**：系统头页里持久化的「下一个待分配号」。`trx_sys_init_at_db_start()` 读出页内旧值后，把内存里的 `trx_sys->next_trx_id_or_no` 设为 `max_trx_id + 2 * trx_sys_get_trx_id_write_margin()`，随后调用 `trx_sys_write_max_trx_id()` 落盘。

### 提交的三个阶段

`trx_commit_for_mysql()` → `trx_commit()` → `trx_commit_low()` → `trx_commit_in_memory()` 是一条固定的四层链路，每层职责不同：

1. `trx_commit_for_mysql(trx)`：入口层。检查事务是否已被强制中止，若是则返回 `DB_FORCED_ABORT`；若事务处于 `TRX_STATE_NOT_STARTED` / `TRX_STATE_FORCED_ROLLBACK`，先补一次 `trx_start_low(trx, true)` 让事务真正跑起来；然后为 GTID 持久化补一条 update undo（`trx_undo_gtid_add_update_undo(trx, false, false)`，失败直接返回错误码）；再调 `trx_commit(trx)`。
2. `trx_commit(trx)`：决定要不要开 mini-transaction。`trx_is_rseg_updated(trx)` 为真才 `mtr_start_sync(mtr)`，否则传 `mtr = nullptr`——**没改过任何数据的事务不产生 redo，也不产生提交 LSN**。
3. `trx_commit_low(trx, mtr)`：写序列化历史。`mtr != nullptr` 时先 `mtr->set_sync()`，再 `trx_write_serialisation_history(trx, mtr)` 分配 `trx->no` 并把 undo 段状态从 `TRX_UNDO_ACTIVE` 改成终态，最后 `mtr_commit(mtr)` 拿到提交 LSN；`mtr == nullptr` 时 `serialised = false`。
4. `trx_commit_in_memory(trx, mtr, serialised)`：内存态收尾。释放锁、从活动事务表摘除、关闭读视图、丢弃 insert undo、按 `srv_flush_log_at_trx_commit` 决定是否刷日志。

### 回滚与 undo 的两种归宿

回滚路径在 `storage/innobase/trx/trx0roll.cc`：`trx_rollback_for_mysql()` → `trx_rollback_low()` → `trx_rollback_for_mysql_low()` → `trx_rollback_to_savepoint_low(trx, nullptr)`。

undo 日志在事务结束时按类型走向三个终态，由 `trx_undo_set_state_at_finish()` 决定：

- `TRX_UNDO_CACHED`：undo 只有一个页（`undo->size == 1`）且该页空闲空间低于 `TRX_UNDO_PAGE_REUSE_LIMIT` —— 段留在回滚段里复用。
- `TRX_UNDO_TO_FREE`：undo 类型是 `TRX_UNDO_INSERT` —— insert undo 在提交或回滚后即可丢弃，由 `trx_undo_insert_cleanup()` 负责：段状态是 `TRX_UNDO_CACHED` 时移入 `rseg->insert_undo_cached`，否则 `trx_undo_seg_free()` 删段 + `trx_undo_mem_free()` 释放内存。
- `TRX_UNDO_TO_PURGE`：其余情况（update undo）—— 交给 purge 线程处理。

## 怎么实现的

### 建立：对象池 + 显式初始化

事务对象不是每次 `new` 出来的，而是从池里取。`trx_pool_init()` 建立池管理器 `trx_pools`，块大小由常量 `MAX_TRX_BLOCK_SIZE` 决定，源码里写死为 `1024 * 1024 * 4`（4 MiB）。

`trx_create_low()` 是真正的取对象点，它做的都是「把上一次用剩的状态抹平」：

```
trx->persists_gtid = false;
trx->api_trx = false;
trx->api_auto_commit = false;
trx->read_write = true;
trx->purge_sys_trx = false;
trx->in_innodb |= TRX_FORCE_ROLLBACK_DISABLE;
trx->state.store(TRX_STATE_NOT_STARTED, std::memory_order_relaxed);
```

随后它为 `trx->lock.autoinc_locks` 建一个专门的堆（`mem_heap_create` + `ib_heap_allocator_create`），并用 `ib_vector_create(alloc, sizeof(void **), 4)` 建向量——**初始容量是 4**。源码里 `trx_free()` 上方留了一条 FIXME，注明「每次 commit 都要做一次 heap free/alloc」是待优化项。

对外的两个入口：`trx_allocate_for_background()` 在取到对象后挂上 `trx_dummy_sess`；`trx_allocate_for_mysql()` 在此基础上，在 `trx_sys_mutex` 保护下把对象插到 `trx_sys->mysql_trx_list` 头部。

### 开始：`trx_start_low()` 的三条分支

`trx_start_low(trx, read_write)` 是事务真正「跑起来」的地方，核心是按 `read_only` 与 `read_write` 组合分成三条路径：

- **读写分支**：`!trx->read_only && (trx->mysql_thd == nullptr || read_write || trx->ddl_operation)` 成立时，先 `trx_assign_rseg_durable(trx)` 分配持久回滚段，再在 `trx_sys_mutex` 下 `trx_sys_allocate_trx_id()` 拿 id、压 `rw_trx_ids`、`trx_add_to_rw_trx_list(trx)`，状态置 `TRX_STATE_ACTIVE`，最后 `trx_sys_rw_trx_add(trx)`。
- **只读但要写临时表**：`trx->id = 0`，若 `read_write` 为真则在 `trx_sys_mutex` 下照样分配 id 并加进读写列表。
- **纯只读**：`trx->id = 0`，只把状态置 `TRX_STATE_ACTIVE`。

`trx->auto_commit` 与 `trx->read_only` 的判定：

```
trx->auto_commit = (trx->api_trx && trx->api_auto_commit) ||
                   thd_trx_is_auto_commit(trx->mysql_thd);

trx->read_only = (trx->api_trx && !trx->read_write) ||
                 (!trx->internal && thd_trx_is_read_only(trx->mysql_thd)) ||
                 srv_read_only_mode;
```

注意 `srv_read_only_mode` 参与 `read_only` 判定——**只读模式下事务一律被标成只读**。而 `trx_assign_rseg_durable()` 里 `trx->rsegs.m_redo.rseg = srv_read_only_mode ? nullptr : get_next_redo_rseg()`，即只读模式不分配持久回滚段。

`trx->start_time` 在有 `mysql_thd` 时取 `thd_start_time(trx->mysql_thd)`，否则退化为 `system_clock::from_time_t(time(nullptr))`。

### 提交：`trx_commit_for_mysql()` 的错误分支

`trx_commit_for_mysql()` 里的错误分支是明确的：

```
TrxInInnoDB trx_in_innodb(trx, true);

if (trx_in_innodb.is_aborted() &&
    trx->killed_by != std::this_thread::get_id()) {
  return (DB_FORCED_ABORT);
}
```

条件里的 `trx->killed_by != std::this_thread::get_id()` 是一个自豁免：发起 kill 的那个线程自己不被这个检查拦住。

GTID 持久化的 undo 补齐也是一条错误出口：

```
db_err = trx_undo_gtid_add_update_undo(trx, false, false);
if (db_err != DB_SUCCESS) {
  return (db_err);
}
```

`trx_undo_gtid_add_update_undo()` 在 `storage/innobase/trx/trx0undo.cc` 里，若事务只有 insert undo 且需要持久化 GTID，会在 `trx->undo_mutex` 保护下调 `trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE)` 补一条 update undo；补不到时打印 `ER_IB_CLONE_GTID_PERSIST` 并把 `trx->persists_gtid` 置回 `false`。

`trx_commit_in_memory()` 的分叉则决定了「提交到底做了什么」：

- `trx_is_autocommit_non_locking(trx)`（自动提交、非加锁、只读）走独立分支：断言 `trx->id == 0`、`trx->read_only`、`!trx->in_rw_trx_list`，关掉读视图，状态直接回到 `TRX_STATE_NOT_STARTED`，并 `MONITOR_INC(MONITOR_TRX_NL_RO_COMMIT)`。
- 其余走 `trx_release_impl_and_expl_locks(trx, serialised)`：先在 `trx_sys_mutex` 下 `trx_erase_lists(trx)` 把事务从活动事务表摘掉；再在 `trx->mutex` 下把状态改成 `TRX_STATE_COMMITTED_IN_MEMORY`（源码注释明确指出「这一时刻是事务的隐式锁被释放的时刻」）；随后按 `trx->id > 0` 决定是否从 `Trx_shard` 的 `active_rw_trxs` 中 `erase(trx->id)`；最后在 `trx_sys_serialisation_mutex` 下 `trx_erase_from_serialisation_list_low(trx)`，并调 `lock_trx_release_locks(trx)` 释放显式锁。

**顺序有强约束**：源码注释写明事务必须先从 `rw_trx_ids` / `rw_trx_list` 移除，之后才能从序列化列表移除，否则「读视图可能仍然认为该事务的修改不可见，但相关 undo 记录却会被 purge 掉」。

### 落盘：`trx_sys_write_max_trx_id()` 与启动时的号跳跃

`trx_sys_write_max_trx_id()` 在 `storage/innobase/trx/trx0sys.cc`：

```
ut_ad(trx_sys_mutex_own() || trx_sys_serialisation_mutex_own());

if (!srv_read_only_mode) {
  mtr_start(&mtr);
  sys_header = trx_sysf_get(&mtr);
  const trx_id_t max_trx_id = trx_sys->next_trx_id_or_no.load();
  mlog_write_ull(sys_header + TRX_SYS_TRX_ID_STORE, max_trx_id, &mtr);
  mtr_commit(&mtr);
}
```

三个要点：

1. **只读模式不写**：整个 `mtr` 落在 `if (!srv_read_only_mode)` 里。
2. **并发安全靠页锁**：源码注释说明，持 `trx_sys->mutex` 的线程和持序列化 mutex 的线程可以并发进入这里并各自开 `mtr`，真正的串行化发生在 `trx_sysf_get()` 内部——只有一个线程能拿到头页的 x-lock，后到的那个会重新读一次最新的 `max_trx_id` 再写。
3. **调试开关**：源码里有 `DBUG_EXECUTE_IF("trx_sys_write_max_trx_id__all_blocked", ...)` 的死循环钩子，以及 `DEBUG_SYNC_C("trx_sys_write_max_trx_id__ser")`。

启动时的号跳跃在 `trx_sys_init_at_db_start()`：

```
const trx_id_t max_trx_id = mach_read_from_8(sys_header + TRX_SYS_TRX_ID_STORE);

trx_sys->next_trx_id_or_no.store(max_trx_id +
                                 2 * trx_sys_get_trx_id_write_margin());

trx_sys->serialisation_min_trx_no.store(trx_sys->next_trx_id_or_no.load());
```

**系数是 2**，源码注释解释了原因：`next_trx_id_or_no` 可能被两个线程并发推进（一个持 `trx_sys_mutex`，一个持 `trx_sys_serialisation_mutex`），若把系数 2 改小，测试 `innodb.max_trx_id` 会失败。

系统头页的初值由 `trx_sysf_create()` 设定：`mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1)`——**事务号从 1 开始计数**。同一函数里，回滚段槽位用 `0xff` 填充，长度按 `std::max(TRX_SYS_OLD_N_RSEGS, TRX_SYS_N_RSEGS) * TRX_SYS_RSEG_SLOT_SIZE` 计算，注释说明老版本 InnoDB 定义 `TRX_SYS_N_RSEGS` 为 256（即 `TRX_SYS_OLD_N_RSEGS`）并要求整个数组都被初始化。

启动时还会回写一次：`trx_sys_mutex_enter(); trx_sys_write_max_trx_id(); trx_sys_mutex_exit();`。

### 日志刷盘：`srv_flush_log_at_trx_commit` 的三个取值

刷盘策略在 `trx_flush_log_if_needed_low()` 的 switch 里，值的来源是 `srv_flush_log_at_trx_commit`：

| 值 | 行为 |
|---|---|
| 0 | 什么都不做（`return`），提交不落盘 |
| 1 | `log_write_up_to(*log_sys, lsn, flush)` —— 写日志并刷盘 |
| 2 | `flush = false` 后 fallthrough 到 1 —— 只写日志，不刷盘 |

`flush` 的初值来自平台：Windows 下恒为 `true`，其他平台是 `srv_unix_file_flush_method != SRV_UNIX_NOSYNC`。

`trx_flush_log_if_needed()` 还有一条优先分支：`trx->ddl_operation || trx->ddl_must_flush` 为真时直接 `log_write_up_to(*log_sys, lsn, true)`，无视 `srv_flush_log_at_trx_commit`。

### 组提交：刷盘被推迟到 `trx_commit_complete_for_mysql()`

`trx_commit_in_memory()` 里 `trx->flush_log_later` 为真时不刷盘，只设 `trx->must_flush_log_later = true` 并记住 `trx->ddl_must_flush = trx->ddl_operation`。真正的刷盘推迟到 `trx_commit_complete_for_mysql()`：

```
if (trx->id != 0 || !trx->must_flush_log_later ||
    (thd_requested_durability(trx->mysql_thd) == HA_IGNORE_DURABILITY &&
     !trx->ddl_must_flush)) {
  return;
}

trx_flush_log_if_needed(trx->commit_lsn, trx);
```

`trx_commit_in_memory()` 的注释解释了原因：若在 `prepare_commit_mutex` 内就刷盘，这个 mutex 会把所有提交串行化，事务就无法聚集，组提交算法失效。

### 回收：`trx_free()` 与 `trx_init()`

`trx_free()` 在把对象还池前做一系列断言与清理：清 `mysql_thd`、释放 `autoinc_locks` 向量、清空 `mod_tables`，断言 `trx->read_view == nullptr`、`trx->is_dd_trx == false`、`trx->in_depth == 0`、`trx->will_lock == 0`，最后 `trx->in_innodb &= TRX_FORCE_ROLLBACK_MASK` 后 `trx_pools->mem_free(trx)`。

`trx_init()` 负责重置字段，其中两项对本页机制特别关键：

- `trx->isolation_level = TRX_ISO_REPEATABLE_READ` —— 默认隔离级别。
- `trx->no = TRX_ID_MAX` —— 与 `trx_start_low()` 中的同值初始化一致。

`trx_init()` 里有一段专门的注释说明它**不重置 `trx_t::state`**，因为「`FORCED_ROLLBACK` 状态是异步处理所需要的」。

`trx_free_for_mysql()` = `trx_disconnect_plain()` + `trx_free_for_background()`；后者先调 `trx_validate_state_before_free()` 做检查，再 `trx_free()`。

## 关键取舍

**1. 事务对象池化，但每次 commit 仍付一次堆开销。** `MAX_TRX_BLOCK_SIZE` 定为 `1024 * 1024 * 4`（4 MiB）作为池块大小，事务对象从池里取而不是逐个分配。但 `trx_create_low()` 每次都重建 `autoinc_locks` 向量的堆，`trx_free()` 每次都释放它，`trx_free()` 上方留了 FIXME 承认「需要为每次 commit 避免这次 heap free/alloc」——**这个取舍是「对象池复用」与「autoinc 锁向量需要独立生命周期」之间的冲突**。

**2. 只读事务不分配事务号。** `trx_start_low()` 的三条分支让纯只读事务保持 `trx->id = 0`，`trx_commit_in_memory()` 里对应 `MONITOR_INC(MONITOR_TRX_NL_RO_COMMIT)`。代价是只读事务不进 `rw_trx_list`，对 MVCC 不可见——这是**读性能与写可见性语义的取舍**。

**3. 提交与刷盘分离，换取组提交能力。** `trx->flush_log_later` 把刷盘推迟到 `trx_commit_complete_for_mysql()`，代价是 `trx_commit_for_mysql()` 返回时事务可能还没落盘。

**4. 事务号在启动时跳跃前进，用空间换崩溃安全。** `max_trx_id + 2 * trx_sys_get_trx_id_write_margin()`，系数 2 是为并发推进留的余量。代价是**事务号空间被加速消耗**，换来的是反复崩溃重启后事务号不重叠。

**5. 落盘的持久化程度由 `srv_flush_log_at_trx_commit` 单点控制。** 取 0 时提交完全不落盘（崩溃丢数据），取 1 时写且刷，取 2 时只写不刷。DDL 操作是例外：`trx->ddl_operation || trx->ddl_must_flush` 强制 `log_write_up_to(*log_sys, lsn, true)`。

**6. undo 段的三种终态按「是否可复用、是否还需保留」分流。** `trx_undo_set_state_at_finish()` 用两个条件决定：单页且空闲空间低于 `TRX_UNDO_PAGE_REUSE_LIMIT` → `TRX_UNDO_CACHED`（段留着复用）；insert undo → `TRX_UNDO_TO_FREE`（可立即丢弃）；其余（update undo）→ `TRX_UNDO_TO_PURGE`（交给 purge）。

**7. 回滚对已被强制中止的事务直接放弃。** `trx_rollback_low()` 在 `TRX_STATE_FORCED_ROLLBACK` / `TRX_STATE_NOT_STARTED` 下把 `trx->will_lock = 0` 后直接返回 `DB_SUCCESS`，不做实际回滚。**架构推论：** 这是因为事务尚未产生 undo，或者已被别的路径（`trx_rollback_for_mysql()` 由 kill 路径触发）处理；依据是 `trx_rollback_low()` 的断言 `ut_ad(trx->in_mysql_trx_list)` 与 `assert_trx_nonlocking_or_in_list(trx)`。

**8. XA 回滚要把 undo 状态从 `TRX_UNDO_PREPARED` 改回 `TRX_UNDO_ACTIVE`。** `trx_rollback_low()` 的 `TRX_STATE_PREPARED` 分支调 `trx_undo_set_state_at_prepare(trx, undo_ptr, true, &mtr)`（第三个参数 `rollback = true`），源码注释说明这样「如果系统被 kill，恢复过程会执行回滚」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/trx/trx0trx.cc | `trx_create_low` | 从事务对象池取对象并重置状态：置 `TRX_STATE_NOT_STARTED`、`read_write = true`、置 `TRX_FORCE_ROLLBACK_DISABLE`；为 `lock.autoinc_locks` 建堆并 `ib_vector_create(..., 4)` 建向量 |
| storage/innobase/trx/trx0trx.cc | `MAX_TRX_BLOCK_SIZE` | 事务对象池的块大小常量，源码写死为 `1024 * 1024 * 4`（4 MiB） |
| storage/innobase/trx/trx0trx.cc | `trx_pool_init` | 建立 `trx_pools` 池管理器，块大小取 `MAX_TRX_BLOCK_SIZE` |
| storage/innobase/trx/trx0trx.cc | `trx_allocate_for_background` | 后台事务入口：调 `trx_create_low()` 后挂 `trx_dummy_sess` |
| storage/innobase/trx/trx0trx.cc | `trx_allocate_for_mysql` | MySQL 事务入口：在 `trx_sys_mutex` 保护下把对象插到 `trx_sys->mysql_trx_list` 头部 |
| storage/innobase/trx/trx0trx.cc | `trx_init` | 重置事务字段：`isolation_level = TRX_ISO_REPEATABLE_READ`、`no = TRX_ID_MAX`、`id = 0`、`undo_no = 0`；注释说明不重置 `state` |
| storage/innobase/trx/trx0trx.cc | `trx_free` | 归还对象到池：释放 `autoinc_locks` 堆、清 `mod_tables`、断言 `read_view == nullptr` 与 `will_lock == 0`，最后 `trx_pools->mem_free(trx)` |
| storage/innobase/trx/trx0trx.cc | `trx_free_for_mysql` | MySQL 侧回收：`trx_disconnect_plain()` + `trx_free_for_background()` |
| storage/innobase/trx/trx0trx.cc | `trx_validate_state_before_free` | 回收前校验：`declared_to_be_inside_innodb` 时报 `ER_IB_MSG_1202`；`n_mysql_tables_in_use` 或 `mysql_n_tables_locked` 非零时报 `ER_IB_MSG_1203` |
| storage/innobase/trx/trx0trx.cc | `trx_start_low` | 事务开始：算 `auto_commit` / `read_only`，按读写三分支决定是否 `trx_assign_rseg_durable` 与 `trx_sys_allocate_trx_id`，置 `TRX_STATE_ACTIVE` |
| storage/innobase/trx/trx0trx.cc | `trx_start_if_not_started_low` | 幂等启动入口：状态为 `NOT_STARTED` / `FORCED_ROLLBACK` 时调 `trx_start_low`；`ACTIVE` 且 `id == 0` 时按需 `trx_set_rw_mode` |
| storage/innobase/trx/trx0trx.cc | `trx_start_if_not_started_xa_low` | XA 版幂等启动：只读事务改用 `trx_assign_rseg_temp(trx)` 分配临时回滚段 |
| storage/innobase/trx/trx0trx.cc | `trx_assign_rseg_durable` | 分配持久回滚段：`srv_read_only_mode ? nullptr : get_next_redo_rseg()` |
| storage/innobase/trx/trx0trx.cc | `trx_assign_rseg_temp` | 分配临时表空间回滚段；`trx->id == 0` 时在 `trx_sys_mutex` 下补分配事务号 |
| storage/innobase/trx/trx0trx.cc | `trx_set_rw_mode` | 把只读事务提升为读写：调 `trx_assign_rseg_durable` 后分配事务号并加入读写列表；`srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO` 时直接返回 |
| storage/innobase/trx/trx0trx.cc | `trx_add_to_serialisation_list` | 分配 `trx->no`（`trx_sys_allocate_trx_no`），非只读事务挂入 `trx_sys->serialisation_list`；列表长度为 1 时刷新 `serialisation_min_trx_no` |
| storage/innobase/trx/trx0trx.cc | `trx_write_serialisation_history` | 提交关键一步：持回滚段 mutex，把 undo 段状态从 `TRX_UNDO_ACTIVE` 改成终态，写入序列化历史 |
| storage/innobase/trx/trx0trx.cc | `trx_commit_for_mysql` | 提交入口：被强制中止时返回 `DB_FORCED_ABORT`；未开始时补 `trx_start_low(trx, true)`；调 `trx_undo_gtid_add_update_undo` 补 GTID undo（失败即返回）；最后调 `trx_commit` |
| storage/innobase/trx/trx0trx.cc | `trx_commit` | 决定是否开 mini-transaction：`trx_is_rseg_updated(trx)` 为真才 `mtr_start_sync(mtr)`，否则传 `nullptr` |
| storage/innobase/trx/trx0trx.cc | `trx_commit_low` | 写序列化历史并提交 mtr：先 `fts_commit(trx)`（容忍 `DB_DUPLICATE_KEY`），再 `trx_write_serialisation_history` + `mtr_commit(mtr)` |
| storage/innobase/trx/trx0trx.cc | `trx_commit_in_memory` | 内存态收尾：释放锁与读视图、丢弃 insert undo、按 `srv_flush_log_at_trx_commit` 决定刷盘、递减 `rseg->trx_ref_count`、释放所有 savepoint |
| storage/innobase/trx/trx0trx.cc | `trx_release_impl_and_expl_locks` | 释放锁与摘表：`trx_erase_lists` → 状态改 `TRX_STATE_COMMITTED_IN_MEMORY` → 从 `Trx_shard::active_rw_trxs` 擦除 → `trx_erase_from_serialisation_list_low` → `lock_trx_release_locks` |
| storage/innobase/trx/trx0trx.cc | `trx_erase_from_serialisation_list_low` | 从序列化列表移除事务；列表空时把 `serialisation_min_trx_no` 设为 `trx_sys_get_next_trx_id_or_no()` |
| storage/innobase/trx/trx0trx.cc | `trx_flush_log_if_needed` | 刷盘分发：DDL 操作强制 `log_write_up_to(*log_sys, lsn, true)`；其余走 `trx_flush_log_if_needed_low` |
| storage/innobase/trx/trx0trx.cc | `trx_flush_log_if_needed_low` | 按 `srv_flush_log_at_trx_commit` 分派：0 不做、1 写并刷、2 只写；`flush` 初值依赖 `srv_unix_file_flush_method != SRV_UNIX_NOSYNC` |
| storage/innobase/trx/trx0trx.cc | `trx_commit_complete_for_mysql` | 补刷盘：`trx->id != 0` 或未设 `must_flush_log_later` 时直接返回，否则按 `trx->commit_lsn` 刷盘 |
| storage/innobase/trx/trx0trx.cc | `trx_mark_sql_stat_end` | 语句结束标记：`NOT_STARTED` / `FORCED_ROLLBACK` 下清 `undo_no` 与 `undo_rseg_space`；`ACTIVE` 下更新 `last_sql_stat_start.least_undo_no` |
| storage/innobase/trx/trx0trx.cc | `trx_cleanup_at_db_startup` | 启动清理：`trx_undo_insert_cleanup(&trx->rsegs.m_redo, false)`，清 `rsegs` 与 `undo_no`，从 `rw_trx_list` 移除，状态置 `TRX_STATE_NOT_STARTED` |
| storage/innobase/trx/trx0trx.cc | `trx_free_for_background` | 后台回收：`trx_validate_state_before_free` + `trx_free` |
| storage/innobase/trx/trx0trx.cc | `trx_free_prepared_or_active_recovered` | 回收 prepared / 恢复态事务：断言 `TRX_STATE_ACTIVE` 时 `is_recovered`、否则 `TRX_STATE_PREPARED`；调 `trx_release_impl_and_expl_locks` 与 `trx_undo_free_trx_with_prepared_or_active_logs` |
| storage/innobase/trx/trx0trx.cc | `trx_disconnect_prepared` | prepared 事务脱离 MySQL 会话：从 `mysql_trx_list` 摘除、关读视图、清 `trx->mysql_thd` |
| storage/innobase/trx/trx0sys.cc | `trx_sys_write_max_trx_id` | 把 `trx_sys->next_trx_id_or_no` 写入 TRX_SYS 头页的 `TRX_SYS_TRX_ID_STORE` 偏移；只在 `!srv_read_only_mode` 时执行 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_init_at_db_start` | 启动初始化：读页内 `max_trx_id`，置 `next_trx_id_or_no = max_trx_id + 2 * trx_sys_get_trx_id_write_margin()`，再调 `trx_sys_write_max_trx_id()` 落盘 |
| storage/innobase/trx/trx0sys.cc | `trx_sysf_create` | 建库时建 TRX_SYS 页：`mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1)` 把事务号初值设为 1；回滚段槽位按 `std::max(TRX_SYS_OLD_N_RSEGS, TRX_SYS_N_RSEGS) * TRX_SYS_RSEG_SLOT_SIZE` 用 `0xff` 填充 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_close` | 关闭事务系统：读视图未全关时报 `ER_IB_MSG_1201`；关 `trx_dummy_sess`；关 purge 系统 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_create` | 建 `trx_sys` 实例：建 `mutex` 与 `serialisation_mutex`、初始化三条事务链表、建 `MVCC`（容量 1024）、建 `Trx_shard` 数组与 `Rsegs` |
| storage/innobase/trx/trx0sys.cc | `ReadView::check_trx_id_sanity` | 事务号合法性检查：`id >= trx_sys_get_next_trx_id_or_no()` 时报 `ER_IB_MSG_1196` 并向 `current_thd` 推 `ER_SIGNAL_WARN` 警告 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_persist_gtid_num` | 把 GTID 事务号写入 TRX_SYS 页的 `TRX_SYS_TRX_NUM_GTID` 偏移 |
| storage/innobase/trx/trx0roll.cc | `trx_rollback_for_mysql` | 回滚入口：异步回滚（`TrxInInnoDB::is_async_rollback`）时不进 `TrxInInnoDB` 构造，否则构造后调 `trx_rollback_low` |
| storage/innobase/trx/trx0roll.cc | `trx_rollback_low` | 按状态分派：`FORCED_ROLLBACK` / `NOT_STARTED` 直接返回 `DB_SUCCESS`；`ACTIVE` 补 GTID undo 后回滚；`PREPARED` 先把 undo 状态改回 `TRX_UNDO_ACTIVE` 再回滚 |
| storage/innobase/trx/trx0roll.cc | `trx_rollback_for_mysql_low` | 回滚实现：设 `op_info = "rollback"`，调 `trx_rollback_to_savepoint_low(trx, nullptr)` |
| storage/innobase/trx/trx0roll.cc | `trx_rollback_to_savepoint` | 回滚到保存点：`trx_start_if_not_started_xa(trx, true, UT_LOCATION_HERE)` 后调 `trx_rollback_to_savepoint_low` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_gtid_add_update_undo` | 为 GTID 持久化补 update undo：在 `trx->undo_mutex` 保护下调 `trx_undo_assign_undo(trx, undo_ptr, TRX_UNDO_UPDATE)`；失败报 `ER_IB_CLONE_GTID_PERSIST` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_set_state_at_finish` | 决定 undo 段终态：单页且空闲低于 `TRX_UNDO_PAGE_REUSE_LIMIT` → `TRX_UNDO_CACHED`；insert undo → `TRX_UNDO_TO_FREE`；其余 → `TRX_UNDO_TO_PURGE` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_insert_cleanup` | 丢弃 insert undo：`TRX_UNDO_CACHED` 移入 `rseg->insert_undo_cached`，否则 `trx_undo_seg_free` 删段 + `trx_undo_mem_free` 释放内存 |
| storage/innobase/trx/trx0undo.cc | `trx_undo_set_state_at_prepare` | XA PREPARE / XA ROLLBACK 时改 undo 段状态；`rollback = true` 时把状态改回 `TRX_UNDO_ACTIVE` |
| storage/innobase/trx/trx0undo.cc | `trx_undo_free_trx_with_prepared_or_active_logs` | 关库时释放 prepared / 恢复态事务的 undo 内存；从 `update_undo_list` 与 `insert_undo_list` 摘除并 `trx_undo_mem_free` |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 WAL 与 pager，与 InnoDB 的 `trx0trx.cc` 没有代码级关联，但**机制形态高度同构**，可对照：

- **两者都把「提交」拆成内存生效与持久化两步。** 主核的 WAL 需要落盘才持久，InnoDB 这边 `trx_commit_in_memory()` 只做内存态收尾、刷盘推迟到 `trx_commit_complete_for_mysql()`；主核对应的做法是 WAL 记录写入与 fsync 的分离。
- **两者都维护一个「单调递增的事务/日志序号」。** 本页的 `trx_sys->next_trx_id_or_no` 与 `TRX_SYS_TRX_ID_STORE` 页内存储，对应主核 WAL 中的日志序号（LSN）；主核也需要「用多少页空间换崩溃安全」的号跳跃决策。
- **两者都把 undo/回滚信息与数据分开存放。** InnoDB 把 undo 放在独立回滚段（`trx_assign_rseg_durable` / `trx_assign_rseg_temp`），主核把回滚信息放在 WAL 里。

**架构推论：** 本页最值得主核借鉴的是 `trx_sys_write_max_trx_id()` 的并发设计——它不靠额外互斥量串行化，而是让两个可能并发的调用方各自开 mtr、在 `trx_sysf_get()` 内部用页级 x-lock 收敛，失败者重读最新值再写。主核若要落盘「下一个序号」，可对照这一「乐观重试」模式，而不是再加一把全局锁。

**架构推论：** 本页 `trx_commit_for_mysql()` 里 `trx->killed_by != std::this_thread::get_id()` 这个自豁免条件，是一种「让发起者不被自己的 kill 拦住」的模式。主核若实现强制中断（如 kill 连接后回滚），同类自豁免判断可以避免发起线程被自己触发的中断逻辑二次阻塞。

## 参考

- [storage/innobase/trx/trx0trx.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0trx.cc) —— 本页全部主符号（`trx_commit_for_mysql`、`trx_create_low`、`trx_start_low`、`trx_commit_low`、`trx_commit_in_memory`、`trx_release_impl_and_expl_locks`、`trx_flush_log_if_needed_low`）的权威来源。
- [storage/innobase/trx/trx0sys.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0sys.cc) —— `trx_sys_write_max_trx_id` 与 `trx_sys_init_at_db_start` 的实现位置；启动时事务号跳跃的系数 2 及其原因就在后者注释里。
- [storage/innobase/trx/trx0roll.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0roll.cc) —— 回滚路径 `trx_rollback_for_mysql` / `trx_rollback_low` / `trx_rollback_for_mysql_low` 的实现，含 XA 回滚把 undo 状态改回 `TRX_UNDO_ACTIVE` 的分支。
- [InnoDB Transaction Model（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-transaction-model.html) —— 官方对 InnoDB 事务模型（MVCC + 行级两阶段锁）的定位说明，是本页「不负责 MVCC 可见性判定」这一边界的文档依据。
- [autocommit, Commit, and Rollback（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-autocommit-commit-rollback.html) —— 官方对 `COMMIT` / `ROLLBACK` 语义与隐式提交的说明，对照本页 `trx_commit_for_mysql()` 的入口语义。
- [Undo Tablespaces（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-undo-tablespaces.html) —— 官方对回滚段与 undo 表空间、`innodb_rollback_segments` 默认值 128 的说明，对照本页 `trx_assign_rseg_durable()` 的回滚段分配。
