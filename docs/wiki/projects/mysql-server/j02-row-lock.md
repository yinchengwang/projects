---
project: mysql-server
feature: row-lock
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/lock/lock0lock.cc
    symbols: [lock_table, lock_clust_rec_read_check_and_lock, lock_rec_lock]
---

# 行锁与锁等待

## 一句话职责

`storage/innobase/lock/lock0lock.cc` 里的行锁机制负责在 InnoDB 层完成「记录锁 / 间隙锁 / Next-Key 锁」的加锁判定、冲突检测、锁等待队列的入队与唤醒，以及死锁检测与回滚：`lock_table()` 处理表级锁的获取与等待，`lock_clust_rec_read_check_and_lock()` 把读路径的加锁请求转交 `lock_rec_lock()`，而 `lock_rec_lock()` 走「快路径 → 慢路径 → 等待队列」三级流程完成真正的行级加锁。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责记录锁与间隙锁的加锁判定、锁等待队列与死锁检测；**不负责元数据锁（F03）**、**不负责意向锁的实现细节（J 组）**。

## 机制是什么

### 两种粒度：表级锁与记录锁共用一套冲突判定

InnoDB 的锁在 `lock0lock.cc` 里分成两类，用 `lock_get_type_low()` 区分：

- **表级锁**：`LOCK_TABLE` 类型，模式是 `LOCK_S` / `LOCK_X` / `LOCK_IS` / `LOCK_IX` / `LOCK_AUTO_INC`，由 `lock_table()` 负责。
- **记录锁**：`LOCK_REC` 类型，模式是 `LOCK_S` / `LOCK_X`，可叠加 `LOCK_GAP` / `LOCK_REC_NOT_GAP` / `LOCK_INSERT_INTENTION` 等标志位，由 `lock_rec_lock()` 负责。

两者的加锁判定是同一套模式：

- 表级：`lock_table_other_has_incompatible()` 遍历 `table->locks` 链表，用 `lock_mode_compatible()` 判断是否兼容。
- 记录级：`rec_lock_check_conflict()` 遍历记录锁哈希链，同样先走 `lock_mode_compatible()`，再叠加间隙锁的特例规则。

### 记录锁的三种「强度」标志

记录锁的 `type_mode` 里叠加的标志位决定了锁的实际语义，`lock_rec_lock.cc` 里提供了三个读取函数：

| 标志 | 读取函数 | 语义 |
|---|---|---|
| `LOCK_GAP` | `lock_rec_get_gap()` | 只锁间隙，不锁记录本身 |
| `LOCK_REC_NOT_GAP` | `lock_rec_get_rec_not_gap()` | 只锁记录，不锁间隙 |
| `LOCK_INSERT_INTENTION` | `lock_rec_get_insert_intention()` | 插入意向锁 |

`LOCK_ORDINARY`（既不设 `LOCK_GAP` 也不设 `LOCK_REC_NOT_GAP`）就是 Next-Key 锁：记录锁 + 前置间隙锁。源码注释明确写了这一点：

```
/* A very common type of lock in InnoDB is "Next Key Lock", which is almost
equivalent to two locks: Record Lock and GAP Lock separately. */
```

### 锁队列：一条记录一条链，已授予在前、等待在后

记录锁存在 `lock_sys->rec_hash` 哈希表里。哈希键是页 id（`lock_rec_lock_hash_value()` → `lock_rec_hash_value(lock->rec_lock.page_id)`），同一条记录（同一 page_id + 同一 heap_no）的所有锁落在同一条队列上。

队列顺序有明确保证，`lock_rec_grant_by_heap_no()` 里的断言写得很直白：

```
if (!lock->is_waiting()) {
  /* Granted locks should be before waiting locks. */
  ut_ad(!seen_waiting_lock);
  granted.push_back(lock);
  return false;
}
```

`lock_rec_has_expl()` 也依赖这个顺序做提前退出：

```
/* There are no GRANTED locks after the first WAITING lock in the queue. */
return first == nullptr || first->is_waiting() ? nullptr : first;
```

### 等待队列与死锁检测分居两个文件

- **加锁与等待入队**在 `lock0lock.cc`：`RecLock::add_to_waitq()` 负责把事务挂进等待状态。
- **等待线程的挂起 / 唤醒、超时检查、死锁图分析**在 `lock0wait.cc`：`lock_wait_suspend_thread()` 挂起，`lock_wait_timeout_thread()` 是后台巡检线程。

两者之间的接缝是 `lock_create_wait_for_edge()`：它在入队时记录「谁在等谁」这条边。

## 怎么实现的

### `lock_table()`：表级锁的五步判定

`lock_table()` 的函数体（`lock0lock.cc`）按顺序做五件事：

**第一步，三条短路返回。**

```
if ((flags & BTR_NO_LOCKING_FLAG) || srv_read_only_mode ||
    table->is_temporary()) {
  return (DB_SUCCESS);
}
```

`BTR_NO_LOCKING_FLAG` 表示建索引时不需要加锁；只读模式与临时表直接跳过加锁。源码注释说明理由：临时表的可见性范围有限，可以避免加锁开销。

**第二步，同事务已有同等或更强锁则直接成功。**

```
if (lock_table_has(trx, table, mode)) {
  ut_ad(lock_table_has(trx, table, mode));
  return (DB_SUCCESS);
}
```

源码在这段之前有很长一段注释，列举了表锁会被并发修改的调用链（`lock_release_autoinc_last_lock` → `lock_release_autoinc_locks` → `lock_cancel_waiting_and_release`、`lock_remove_all_on_table_for_trx` → `lock_remove_all_on_table` → `row_drop_table_for_mysql` 等），并说明即使加了 `trx->mutex` 保护也无法杜绝「更高层」的竞态——只能做到「不比以前更差」。

**第三步，必要时把事务提升为读写模式。**

```
if ((mode == LOCK_IX || mode == LOCK_X) && !trx->read_only &&
    trx->rsegs.m_redo.rseg == nullptr) {
  trx_set_rw_mode(trx);
}
```

源码注释解释了动机：只读事务也能写临时表，但不想把它们提升为读写事务，因为它们的更新对其他事务不可见，可以排除在读视图之外。

**第四步，扫描表锁队列找冲突者。**

```
wait_for = lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode);
```

`lock_table_other_has_incompatible()` 里有一个重要优化：

```
if ((mode == LOCK_IS || mode == LOCK_IX) &&
    table->count_by_mode[LOCK_S] == 0 && table->count_by_mode[LOCK_X] == 0) {
  return nullptr;
}
```

源码注释说：按 `lock_compatibility_matrix`，意向锁只能等待 `LOCK_S` 或 `LOCK_X`；队列里没有这两者时可直接返回 `nullptr`，不必遍历链表。这避免了 OLTP 场景下大量 DML 因持有意向锁而反复争抢表锁分片闩锁。

**第五步，入队等待或直接授予。**

```
if (wait_for != nullptr) {
  err = lock_table_enqueue_waiting(mode | flags, table, thr, wait_for);
} else {
  lock_table_create(table, mode | flags, trx);
  ut_a(!flags || mode == LOCK_S || mode == LOCK_X);
  err = DB_SUCCESS;
}
```

`lock_table_enqueue_waiting()` 内部有三道闸：`que_thr_stop(thr)` 返回真时 `ut_error`（查询线程本来就要停，不必再入队）；事务正处于字典操作（`TRX_DICT_OP_TABLE` / `TRX_DICT_OP_INDEX`）时打印 `ER_IB_MSG_642` 并在 debug 下 `ut_error`；`trx->in_innodb & TRX_FORCE_ROLLBACK` 时返回 `DB_DEADLOCK`。

`lock_table()` 的返回值被断言限定为三种：

```
ut_ad(err == DB_SUCCESS || err == DB_LOCK_WAIT || err == DB_DEADLOCK);
```

### `lock_clust_rec_read_check_and_lock()`：读路径加锁的入口

这个函数负责「读路径」上的行锁请求，函数体顺序如下：

**第一步，断言与只读 / 临时表短路。**

```
if (srv_read_only_mode || index->table->is_temporary()) {
  return (DB_SUCCESS);
}
```

**第二步，非 supremum 记录先做隐式锁转显式锁。**

```
heap_no = page_rec_get_heap_no(rec);

if (heap_no != PAGE_HEAP_NO_SUPREMUM) {
  lock_rec_convert_impl_to_expl(block, rec, index, offsets);
}
```

`lock_rec_convert_impl_to_expl()` 会去找是否别的活跃事务持有该记录的隐式锁（聚簇索引用 `lock_clust_rec_some_has_impl()`，二级索引用 `lock_sec_rec_some_has_impl()`），找到就把那个事务的隐式锁转成显式锁，避免其事务提交时锁凭空消失。

**第三步，取页分片闩锁后进入核心。**

```
locksys::Shard_latch_guard guard{UT_LOCATION_HERE, block->get_page_id()};

if (duration == lock_duration_t::AT_LEAST_STATEMENT) {
  lock_protect_locks_till_statement_end(thr);
}

err = lock_rec_lock(false, sel_mode, mode | gap_mode, block, heap_no, index, thr);

MONITOR_INC(MONITOR_NUM_RECLOCK_REQ);
```

注意 `lock_rec_lock()` 的第一个实参是 `false`（`impl = false`），意味着**即使无需等待也会创建显式锁**。

函数末尾的断言把全部可能的返回码列全了：

```
ut_ad(err == DB_SUCCESS || err == DB_SUCCESS_LOCKED_REC ||
      err == DB_LOCK_WAIT || err == DB_DEADLOCK || err == DB_SKIP_LOCKED ||
      err == DB_LOCK_NOWAIT);
```

其中 `DB_SKIP_LOCKED` 与 `DB_LOCK_NOWAIT` 对应 `SELECT ... SKIP LOCKED` 与 `NOWAIT`。

同文件的 `lock_clust_rec_read_check_and_lock_alt()` 是这个函数的变体：它不要求调用方预先算好 `offsets`，内部用 `Rec_offsets().compute(rec, index)` 现算，并把 `DB_SUCCESS_LOCKED_REC` 归一化为 `DB_SUCCESS`：

```
dberr_t err = lock_clust_rec_read_check_and_lock(
    lock_duration_t::REGULAR, block, rec, index,
    Rec_offsets().compute(rec, index), SELECT_ORDINARY, mode, gap_mode, thr);

if (err == DB_SUCCESS_LOCKED_REC) {
  err = DB_SUCCESS;
}
```

### `lock_rec_lock()`：三级降级

`lock_rec_lock()` 是本页最核心的函数。它先用一条断言限定 `impl` 与模式的关系：

```
/* Implicit locks are equivalent to LOCK_X|LOCK_REC_NOT_GAP, so we can omit
creation of explicit lock only if the requested mode was LOCK_REC_NOT_GAP */
ut_ad(!impl || ((mode & LOCK_REC_NOT_GAP) == LOCK_REC_NOT_GAP));
```

然后进入快路径：

```
switch (lock_rec_lock_fast(impl, mode, block, heap_no, index, thr)) {
  case LOCK_REC_SUCCESS:
    return (DB_SUCCESS);
  case LOCK_REC_SUCCESS_CREATED:
    return (DB_SUCCESS_LOCKED_REC);
  case LOCK_REC_FAIL:
    return (
        lock_rec_lock_slow(impl, sel_mode, mode, block, heap_no, index, thr));
  default:
    ut_error;
}
```

#### 快路径 `lock_rec_lock_fast()`

它只做一件事：在同一页的锁集合里找第一个锁对象（`lock_sys->rec_hash.find_on_block()`），然后看能不能复用它。

- 页上没有任何锁：若 `impl == false`，直接 `RecLock rec_lock(index, block, heap_no, mode); rec_lock.create(trx);`，返回 `LOCK_REC_SUCCESS_CREATED`。
- 页上已有锁：只有在「该锁属于同一事务、类型模式完全一致（`lock->type_mode != (mode | LOCK_REC)` 不成立）、且其位图覆盖目标 heap_no（`lock_rec_get_n_bits(lock) > heap_no`）」这三个条件同时成立时才能复用；否则返回 `LOCK_REC_FAIL` 转慢路径。复用时若目标 bit 未置位，就 `lock_rec_set_nth_bit(lock, heap_no)`。

**这就是「一条锁对象带一张位图、可同时锁住页上多条记录」的来源。**

#### 慢路径 `lock_rec_lock_slow()`

慢路径的核心是「Next-Key 锁的拆分启发式」。源码注释解释得很完整：

```
/* A very common type of lock in InnoDB is "Next Key Lock", which is almost
equivalent to two locks: Record Lock and GAP Lock separately.
Thus, in case we need to wait, we check if we already own a Record Lock,
and if we do, we only need the GAP Lock.
We don't do the opposite thing (of checking for GAP Lock, and only requesting
Record Lock), because if Next Key Lock has to wait, then it is because of a
conflict with someone who locked the record, as locks on gaps are compatible
with each other ... */
```

具体实现分三段。

**第一段，检查自己是否已持有足够强的锁。**

```
auto checked_mode =
    (heap_no != PAGE_HEAP_NO_SUPREMUM && lock_mode_is_next_key_lock(mode))
        ? mode | LOCK_REC_NOT_GAP
        : mode;

const auto *held_lock = lock_rec_has_expl(checked_mode, block, heap_no, trx);
```

对非 supremum 的 Next-Key 请求，先降级成「只要记录锁」去查自己已持有的锁。若查到：

- `checked_mode == mode`（说明请求本来就不是 Next-Key）：直接 `return (DB_SUCCESS)`，什么都不做。
- 否则说明请求是 Next-Key 锁、但自己只有记录锁：调 `lock_reuse_for_next_key_lock()` 补一个间隙锁再返回 `DB_SUCCESS`。

`lock_reuse_for_next_key_lock()` 的实现：

```
if (!held_lock->is_record_not_gap()) {
  ut_ad(held_lock->is_next_key_lock());
  return;
}

/* We have a Record Lock granted, so we only need a GAP Lock. */
mode |= LOCK_GAP;
ut_ad(nullptr ==
      lock_rec_other_has_conflicting(mode, block, heap_no, trx).wait_for);

if (lock_rec_has_expl(mode, block, heap_no, trx) == nullptr) {
  lock_rec_add_to_queue(LOCK_REC | mode, block, heap_no, index, trx);
}
```

**第二段，冲突检测。**

```
const auto conflicting =
    lock_rec_other_has_conflicting(mode, block, heap_no, trx);
```

`lock_rec_other_has_conflicting()` 遍历 `lock_sys->rec_hash` 中该记录的锁队列，对每个锁调 `rec_lock_check_conflict()`，返回 `{wait_for, bypassed}` 二元组。

`rec_lock_check_conflict()` 是间隙锁特例规则集中地，逐条如下：

1. **同一事务，或模式兼容** → `NO_CONFLICT`：

```
if (trx == lock2->trx ||
    lock_mode_compatible(static_cast<lock_mode>(LOCK_MODE_MASK & type_mode),
                         lock_get_mode(lock2))) {
  return Conflict::NO_CONFLICT;
}
```

2. **高优先级事务可以忽略低优先级事务的等待锁**：

```
const bool is_hp = trx_is_high_priority(trx);
if (is_hp && lock2->is_waiting() && !trx_is_high_priority(lock2->trx)) {
  return Conflict::NO_CONFLICT;
}
```

3. **纯间隙锁不阻塞任何人**（除插入意向锁外）：

```
if ((lock_is_on_supremum || (type_mode & LOCK_GAP)) &&
    !(type_mode & LOCK_INSERT_INTENTION)) {
  return Conflict::NO_CONFLICT;
}
```

源码注释：不同用户可以在同一间隙上持有互相冲突的锁类型，所以间隙锁不需要等任何东西。**supremum 记录上的锁按间隙锁处理**，因此永不需要等待。

4. **记录锁不等间隙锁**：

```
if (!(type_mode & LOCK_INSERT_INTENTION) && lock_rec_get_gap(lock2)) {
  return Conflict::NO_CONFLICT;
}
```

5. **间隙锁不等 `LOCK_REC_NOT_GAP` 锁**：

```
if ((type_mode & LOCK_GAP) && lock_rec_get_rec_not_gap(lock2)) {
  return Conflict::NO_CONFLICT;
}
```

6. **插入意向锁不阻塞任何人**：

```
if (lock_rec_get_insert_intention(lock2)) {
  return Conflict::NO_CONFLICT;
}
```

源码注释给出的理由：插入意向锁之间的插入位置不冲突，可以共存；不阻塞还能消除一类假死锁。

7. **插入意向锁不得越过同 heap_no 上的等待锁**。这是唯一返回 `CAN_BYPASS` 的分支：

```
if (!(type_mode & LOCK_INSERT_INTENTION) && lock2->is_waiting() &&
    lock2->mode() == LOCK_X && (type_mode & LOCK_MODE_MASK) == LOCK_X) {
  ...
  if (trx_locks_cache.has_granted_blocker(trx, lock2)) {
    return Conflict::CAN_BYPASS;
  }
}
```

源码注释解释了为什么插入意向锁不能越过：后续的插入会分裂间隙、复制等待锁，违反「一个事务最多只能等一个锁」的约束。`Trx_locks_cache::has_granted_blocker()` 的判据是「自己是否已持有强于或等于 `LOCK_S|LOCK_REC_NOT_GAP` 的锁」——若已持有，则挡住 `lock2` 的锁是自己放的，可以绕过。

**第三段，按 `sel_mode` 分派。**

```
if (conflicting.wait_for != nullptr) {
  switch (sel_mode) {
    case SELECT_SKIP_LOCKED:
      return (DB_SKIP_LOCKED);
    case SELECT_NOWAIT:
      return (DB_LOCK_NOWAIT);
    case SELECT_ORDINARY:
      RecLock rec_lock(thr, index, block, heap_no, mode);
      trx_mutex_enter(trx);
      dberr_t err = rec_lock.add_to_waitq(conflicting.wait_for);
      trx_mutex_exit(trx);
      return (err);
  }
}
```

无冲突时：

```
if (!impl || conflicting.bypassed) {
  lock_rec_add_to_queue(LOCK_REC | mode, block, heap_no, index, trx);
  return (DB_SUCCESS_LOCKED_REC);
}
return (DB_SUCCESS);
```

源码注释说明了 `conflicting.bypassed` 的待遇：即使绕过了冲突的等待者，也优先创建显式锁，这样「等待关系」更容易被追踪。

### `RecLock::add_to_waitq()`：进入等待状态

`RecLock::add_to_waitq()` 是行锁等待的入队点：

```
if (m_trx->in_innodb & TRX_FORCE_ROLLBACK) {
  return (DB_DEADLOCK);
}

m_mode |= LOCK_WAIT;

prepare();

lock_t *lock = create(m_trx, prdt);

lock_create_wait_for_edge(lock, wait_for);

set_wait_state(lock);

MONITOR_INC(MONITOR_LOCKREC_WAIT);

return (DB_LOCK_WAIT);
```

`prepare()` 做两道前置检查：查询线程本来就要停（`que_thr_stop(m_thr)` 返回真）时 `ut_error`；事务处于字典操作时打印 `ER_IB_MSG_635` 并在 debug 下 `ut_error`。

`set_wait_state()` 记录等待起点并把事务状态改为 `TRX_QUE_LOCK_WAIT`：

```
m_trx->lock.wait_started =
    std::chrono::system_clock::from_time_t(time(nullptr));

m_trx->lock.que_state = TRX_QUE_LOCK_WAIT;

m_trx->lock.was_chosen_as_deadlock_victim = false;

bool stopped = que_thr_stop(m_thr);
ut_a(stopped);
```

### 死锁检测：`lock0wait.cc` 的等待图

死锁检测完全在 `lock0wait.cc` 里。

**边的建立。** `lock_create_wait_for_edge()` 在入队时写入「谁挡着我」：

```
waiter->lock.blocking_trx.store(blocker);
lock_report_wait_for_edge_to_server(waiting_lock, blocking_lock);
```

**后台线程。** `lock_wait_timeout_thread()` 是唯一的巡检线程，循环做两件事：

```
if (std::chrono::seconds(1) <= current_time - last_checked_for_timeouts_at) {
  last_checked_for_timeouts_at = current_time;
  lock_wait_check_slots_for_timeouts();
}

lock_wait_update_schedule_and_check_for_deadlocks();

os_event_wait_time_low(event, std::chrono::seconds{1}, sig_count);
```

源码注释解释了 1 秒这个粒度：锁超时以秒为单位指定，没必要比一秒更频繁地醒。

**超时判定。** `lock_wait_check_and_cancel()`：

```
const auto wait_time = std::chrono::steady_clock::now() - slot->suspend_time;
/* Timeout exceeded or a wrap-around in system time counter */
const auto timeout = slot->wait_timeout < std::chrono::seconds{100000000} &&
                     wait_time > slot->wait_timeout;
trx_t *trx = thr_get_trx(slot->thr);

if (!trx_is_interrupted(trx) && !timeout) {
  return;
}
```

超时后 `lock_wait_try_cancel()` 把 `trx->error_state` 置为 `DB_LOCK_WAIT_TIMEOUT`，再调 `lock_cancel_waiting_and_release()` 取消等待请求：

```
ut_ad(trx->error_state != DB_DEADLOCK);
trx->error_state = DB_LOCK_WAIT_TIMEOUT;
...
lock_cancel_waiting_and_release(trx);
```

源码注释说明这里刻意不覆盖 `DB_DEADLOCK`：死锁回滚整个事务，比单条语句超时更重要。

**等待槽位。** `lock_wait_table_reserve_slot()` 从 `lock_sys->waiting_threads` 起扫描 `srv_max_n_threads` 个槽位。槽位用尽时打印 `ER_IB_MSG_646` 并 `ut_error`。这个上限来自 `lock_sys_create()` 的分配：

```
lock_sys_sz = sizeof(*lock_sys) + srv_max_n_threads * sizeof(srv_slot_t);
```

**死锁图分析。** `lock_wait_find_and_handle_deadlocks()` 用染色法在等待图上找环：

```
uint current_color = 0;
for (uint start = 0; start < n; ++start) {
  if (colors[start] != 0) {
    continue;
  }
  ++current_color;
  for (int id = start; 0 <= id; id = outgoing[id]) {
    ut_ad(id != outgoing[id]);
    if (colors[id] == 0) {
      colors[id] = current_color;
      continue;
    }
    if (colors[id] == current_color) {
      /* found a candidate cycle! */
      lock_wait_extract_cycle_ids(cycle_ids, id, outgoing);
      if (lock_wait_check_candidate_cycle(cycle_ids, infos, new_weights)) {
        MONITOR_INC(MONITOR_DEADLOCK);
      } else {
        MONITOR_INC(MONITOR_DEADLOCK_FALSE_POSITIVES);
      }
    }
    break;
  }
}
```

候选环还要过一道 `lock_wait_check_candidate_cycle()` 的复核：先查 `lock_wait_trxs_are_still_in_slots()` 确认这些事务还在槽位里（源码注释说这是为了在不访问已释放 `trx_t` 的前提下验证指针有效性），再取全局独占闩锁查 `lock_wait_trxs_are_still_waiting()`，确认它们确实还在等。

**选牺牲者。** `lock_wait_choose_victim()` 按事务的「调度权重」（`schedule_weight`）选，权重由 `lock_wait_compute_and_publish_weights_except_cycles()` 计算——它把「自己 + 所有被自己挡住的事务」的权重累加。选好后 `lock_wait_handle_deadlock()`：

```
lock_wait_update_weights_on_cycle(chosen_victim, cycle_ids, infos, new_weights);

lock_notify_about_deadlock(
    lock_wait_trxs_rotated_for_notification(cycle_ids, infos), chosen_victim);

lock_wait_rollback_deadlock_victim(chosen_victim);
```

### 唤醒：`lock_rec_grant_by_heap_no()`

释放锁时，`lock_rec_dequeue_from_page()` 先摘除再唤醒：

```
static void lock_rec_dequeue_from_page(lock_t *in_lock) {
  lock_rec_discard(in_lock);
  lock_rec_grant(in_lock);
}
```

`lock_rec_grant_by_heap_no()` 把队列拆成四组：已授予、高优先级等待、低优先级轻量等待（`schedule_weight <= 1`）、低优先级重量等待。排序规则是「权重大的在前，同权重按原队列位置」：

```
std::stable_sort(low_priority_heavier.begin(), low_priority_heavier.end(),
                 [](const LockDescriptorEx &a, const LockDescriptorEx &b) {
                   return (a.first > b.first);
                 });
```

然后逐个检查等待者是否还要等：

```
const lock_t *blocking_lock =
    lock_rec_has_to_wait_for_granted(wait_lock, granted, new_granted_index);
if (blocking_lock == nullptr) {
  lock_grant(wait_lock);
  lock_rec_move_granted_to_front(wait_lock, rec_id);
  granted.push_back(wait_lock);
} else {
  lock_update_wait_for_edge(wait_lock, blocking_lock);
}
```

`lock_grant()` 最终调 `lock_reset_wait_and_release_thread_if_suspended(lock)` 把挂起的线程唤醒。

### 释放：`lock_rec_unlock()` 与 `lock_trx_release_locks()`

`lock_rec_unlock()` 按事务 + 模式 + `LOCK_REC_NOT_GAP` 三个条件在队列里找锁：

```
lock_t *lock = lock_sys->rec_hash.find_on_record(rec_id, [&](lock_t *lock) {
  return lock->trx == trx && lock_get_mode(lock) == lock_mode &&
         lock_rec_get_rec_not_gap(lock);
});
```

找不到会打印 `ER_IB_MSG_1228`。

事务提交时的整体释放在 `lock_trx_release_locks()`，它循环调 `locksys::try_release_all_locks()` 直到成功：

```
while (!locksys::try_release_all_locks(trx)) {
  std::this_thread::yield();
}
```

`try_release_all_locks()` 里有一段关于加锁顺序的长注释，说明为什么不能直接持 `trx->mutex` 遍历 `trx->lock.trx_locks`：加锁顺序要求先拿 lock_sys 分片闩锁再拿 `trx->mutex`，而遍历事务锁列表又需要 `trx->mutex`；直接用全局独占闩锁虽然能解决，但源码注释说它「对性能有严重影响（sysbench {pareto,uniform}-2S-{128,1024}-usrs 测试下 TPS 掉 3% 到 11%）」。解决办法是 `try_relatch_trx_and_shard_and_do()` 的「放 mutex → 取分片闩锁 → 重取 mutex → 校验指针仍在列表里 → 再操作」五步。

### 真实调用方

`lock_clust_rec_read_check_and_lock()` 的调用方（均已读过调用点）：

| 调用方 | 场景 | 关键参数 |
|---|---|---|
| `row_sel_read_clust_rec`（`row0sel.cc`） | 聚簇索引加锁读 | `lock_type = trx->skip_gap_locks() ? LOCK_REC_NOT_GAP : LOCK_ORDINARY`，`mode = node->row_lock_mode` |
| `row_sel_set_rec_lock`（`row0sel.cc`） | 统一的记录加锁入口，按索引类型分派 | 聚簇走 `lock_clust_rec_read_check_and_lock()`，空间索引走 `sel_set_rtr_rec_lock()`，其余走 `lock_sec_rec_read_check_and_lock()` |
| `row_ins_set_exclusive_rec_lock`（`row0ins.cc`） | 插入前的排他锁 | 用 `lock_clust_rec_read_check_and_lock_alt()` 变体 |
| `row_ins_step`（`row0ins.cc`） | 插入时同时加二级索引锁 | 聚簇用 `lock_clust_rec_read_check_and_lock()`，二级用 `lock_sec_rec_read_check_and_lock(lock_duration_t::AT_LEAST_STATEMENT, ...)` |

`lock_table()` 的调用方：

| 调用方 | 场景 | 关键参数 |
|---|---|---|
| `row_ins_step`（`row0ins.cc`） | 插入前取表级意向锁 | `lock_table(0, table, LOCK_IX, thr)` |
| `row_mysql_lock_table`（`row0mysql.cc`） | 统一的表级加锁入口 | `lock_table(0, table, mode, thr)` |
| `row_lock_table`（`row0mysql.cc`） | 显式表锁 | `lock_table(0, prebuilt->table, ...)` |
| `row_mysql_lock_autoinc_table`（`row0mysql.cc`） | AUTO-INC 表锁 | `lock_table(0, prebuilt->table, LOCK_AUTO_INC, thr)` |
| `row_search_mvcc`（`row0sel.cc`） | 读路径取表级意向锁 | `lock_table(0, table_node->table, i_lock_mode, thr)` |
| `row_upd_step`（`row0upd.cc`） | 更新前取表级意向锁 | `lock_table(0, node->table, LOCK_IX, thr)` |
| `lock_table_for_trx`（`lock0lock.cc`） | Memcached 插件路径的表锁 | `lock_table(0, table, mode, thr)` |

`lock_table_for_trx()` 的源码注释记录了这条调用链：

```
innodb_store -> innodb_conn_init -> innodb_api_begin -> innodb_cb_cursor_lock
-> ib_cursor_set_lock_mode -> ib_cursor_lock -> ib_trx_lock_table_with_retry
-> lock_table_for_trx -> lock_table -> lock_table_has
```

`lock_rec_lock()` 的调用方（全在 `lock0lock.cc` 内，且都是 `impl = true` 的写路径）：

| 调用方 | 场景 | 关键参数 |
|---|---|---|
| `lock_clust_rec_modify_check_and_lock` | 修改聚簇索引记录 | `lock_rec_lock(true, SELECT_ORDINARY, LOCK_X \| LOCK_REC_NOT_GAP, ...)` |
| `lock_sec_rec_modify_check_and_lock` | 修改二级索引记录 | 同上模式 |

## 关键取舍

### 快路径先扫「本页第一个锁」，而不是精确匹配目标记录

`lock_rec_lock_fast()` 只取 `find_on_block()` 返回的第一个锁，然后靠「同事务 + 类型模式完全一致 + 位图已覆盖目标 heap_no」三个条件判断能否复用。**取舍：牺牲精度换速度——不做全队列搜索，只看第一个锁就能决定绝大多数情况。** 代价是不满足三个条件时一律退回慢路径做完整冲突检测。

### 显式锁对象带位图，一条锁对象可覆盖页上多条记录

`RecLock::lock_alloc()` 中 `rec_lock.n_bits = static_cast<uint32_t>(8 * size)`，一个锁对象后面跟着一段零初始化的位图，`lock_rec_set_nth_bit()` / `lock_rec_get_nth_bit()` 在上面操作。**取舍：位图让「同一页同一事务的多个记录锁」合并成一个对象，减少内存与链表节点；代价是释放时必须逐位清理**，这也是 `lock_release_gap_lock()` 要单独处理 supremum 记录的原因（源码注释解释：不能在步骤 1 清 supremum 的 bit，否则步骤 2 的 `lock_rec_grant` 会因为输入锁对象缺了 supremum bit 而无法授予）。

### 间隙锁之间不互斥，用「抑制性」换并发度

`rec_lock_check_conflict()` 的第 3 条规则让纯间隙锁永不等任何人。**取舍：放弃间隙上的严格互斥，换取间隙锁可以共存——官方文档也解释了这个取舍：如果索引记录被 purge，不同事务持有的间隙锁必须能合并。**

### 插入意向锁不得越过等待中的 X 锁

`rec_lock_check_conflict()` 的 `CAN_BYPASS` 分支有明确的反向约束：插入意向锁不能绕过同 heap_no 上的等待锁。**取舍：牺牲「插入尽快拿到间隙」的收益，换取「一个事务最多等一个锁」这条不变量不被破坏——否则间隙分裂会复制等待锁，破坏该约束。**

### Next-Key 锁只在「自己已有记录锁」时拆成间隙锁，不做反向拆分

`lock_rec_lock_slow()` 的启发式只朝一个方向拆。**取舍：只做安全方向的拆分，避免了「已有间隙锁就把请求缩成记录锁」这种会导致冲突依旧存在、却白白丢失间隙保护的错误优化。** 源码注释明确写了原因：Next-Key 锁之所以要等，是因为与别人锁住记录冲突，而不是因为间隙冲突。

### `impl = true` 时不创建显式锁，改为隐式锁

写路径（`lock_clust_rec_modify_check_and_lock` / `lock_sec_rec_modify_check_and_lock`）传 `impl = true`，无冲突时直接返回 `DB_SUCCESS` 而不建锁对象。断言限定了这一行为：

```
ut_ad(!impl || ((mode & LOCK_REC_NOT_GAP) == LOCK_REC_NOT_GAP));
```

**取舍：写路径省掉一次锁对象分配与哈希插入；代价是这些锁在 `SHOW ENGINE INNODB STATUS` 里不可见**，需要靠 `lock_rec_convert_impl_to_expl()` 在后续路径上按需补出显式锁。

### 死锁检测用「等待图 + 权重」而不是逐对检测

后台线程每秒扫一次等待图，染色法找环，用 `schedule_weight`（自己 + 所有被自己挡住的事务的权重和）选牺牲者。**取舍：牺牲「总是回滚最小事务」的绝对最优，换取检测成本与等待事务数成线性关系而不是平方关系；代价是权重计算有延迟，被选中的牺牲者未必当场最小。** 源码注释坦承这点：

```
/* We now update the `schedule_weight`s on the cycle taking into account that
chosen_victim will be rolled back.
This is mostly for "correctness" as the impact on performance is negligible
(actually it looks like it is slowing us down). */
```

### 超时与死锁用不同的错误码，且死锁优先

`lock_wait_try_cancel()` 在超时时置 `DB_LOCK_WAIT_TIMEOUT`，但源码注释强调不覆盖已有的 `DB_DEADLOCK`——因为死锁回滚整个事务，而超时只回滚当前语句。**取舍：让更严重的错误优先上报，代价是等待线程会先拿到超时错误码再被回滚。**

### 加锁顺序：先分片闩锁，后 `trx->mutex`

`lock_rec_lock()` 与 `lock_table()` 都是先取 `locksys::Shard_latch_guard` 再进 `trx_mutex_enter(trx)`。**取舍：固定的加锁顺序避免了死锁，代价是不能在持有 `trx->mutex` 时反过来操作锁对象——这正是 `try_relatch_trx_and_shard_and_do()` 要「放 mutex → 取分片闩锁 → 重取 mutex → 校验指针」绕一圈的原因。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/lock/lock0lock.cc | `lock_table` | 表级锁入口：三条短路、已有锁判定、`trx_set_rw_mode` 提升、`lock_table_other_has_incompatible` 冲突扫描、入队等待或直接授予 |
| storage/innobase/lock/lock0lock.cc | `lock_clust_rec_read_check_and_lock` | 聚簇索引读路径加锁入口：只读/临时表短路、`lock_rec_convert_impl_to_expl` 隐式转显式、`lock_rec_lock` 委托、`MONITOR_NUM_RECLOCK_REQ` 计数 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock` | 行锁核心：断言 `impl` 与 `LOCK_REC_NOT_GAP` 的关系，派发到快路径 `lock_rec_lock_fast` / 慢路径 `lock_rec_lock_slow` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_fast` | 快路径：在页内取第一个锁对象，判断能否复用；命中返回 `LOCK_REC_SUCCESS_CREATED`，否则 `LOCK_REC_FAIL` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_slow` | 慢路径：Next-Key 锁拆分启发式、`lock_rec_has_expl` 查自身持锁、`lock_rec_other_has_conflicting` 冲突检测、按 `sel_mode` 分派 `DB_SKIP_LOCKED` / `DB_LOCK_NOWAIT` / 入队 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_other_has_conflicting` | 冲突扫描：遍历记录锁队列调 `rec_lock_check_conflict`，返回 `{wait_for, bypassed}` |
| storage/innobase/lock/lock0lock.cc | `rec_lock_check_conflict` | 冲突规则集：同事务/兼容、高优先级忽略低优先级等待锁、纯间隙不阻塞、记录锁不等间隙锁、间隙锁不等 `LOCK_REC_NOT_GAP`、插入意向锁不阻塞、插入意向不得越过等待锁（唯一 `CAN_BYPASS` 来源） |
| storage/innobase/lock/lock0lock.cc | `lock_reuse_for_next_key_lock` | Next-Key 拆分的落点：已有记录锁时补一个 `LOCK_GAP` 锁 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_has_expl` | 查本事务是否已持有足够强的显式锁；遇首个等待锁即停（队列中已授予锁必在等待锁之前） |
| storage/innobase/lock/lock0lock.cc | `lock_rec_has_to_wait_for_granted` / `lock_rec_grant_by_heap_no` | 唤醒路径：把队列拆成已授予/高优先级/低优先级轻/低优先级重四组，按 `schedule_weight` 排序后逐个授予 |
| storage/innobase/lock/lock0lock.cc | `lock_grant` | 授予后调 `lock_reset_wait_and_release_thread_if_suspended` 唤醒挂起线程；`LOCK_AUTO_INC` 特殊处理 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_add_to_queue` | 把锁对象插入记录锁队列（`LOCK_REC` 类型强制置位），supremum 记录上清除 `LOCK_GAP` / `LOCK_REC_NOT_GAP` 位 |
| storage/innobase/lock/lock0lock.cc | `RecLock::add_to_waitq` | 行锁等待入队：检查 `TRX_FORCE_ROLLBACK`、`prepare()`、`create()`、`lock_create_wait_for_edge`、`set_wait_state` |
| storage/innobase/lock/lock0lock.cc | `RecLock::set_wait_state` | 记录 `wait_started`、`que_state = TRX_QUE_LOCK_WAIT`、清 `was_chosen_as_deadlock_victim` |
| storage/innobase/lock/lock0lock.cc | `RecLock::prepare` | 入队前置检查：`que_thr_stop` 与字典操作（打印 `ER_IB_MSG_635`） |
| storage/innobase/lock/lock0lock.cc | `RecLock::lock_alloc` | 分配锁对象与位图（`n_bits = 8 * size`），从 `rec_pool` 复用或从堆分配（`REC_LOCK_SIZE` 上限） |
| storage/innobase/lock/lock0lock.cc | `lock_rec_convert_impl_to_expl` / `lock_rec_convert_impl_to_expl_for_trx` | 隐式锁转显式锁：聚簇走 `lock_clust_rec_some_has_impl`，二级走 `lock_sec_rec_some_has_impl` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_insert_check_and_lock` | 插入路径加锁：`LOCK_X \| LOCK_GAP \| LOCK_INSERT_INTENTION` 模式，`ut_a(!conflicting.bypassed)` 约束插入意向不得绕过等待锁 |
| storage/innobase/lock/lock0lock.cc | `lock_clust_rec_modify_check_and_lock` | 写路径：`lock_rec_lock(true, SELECT_ORDINARY, LOCK_X \| LOCK_REC_NOT_GAP, ...)` |
| storage/innobase/lock/lock0lock.cc | `lock_sec_rec_read_check_and_lock` | 二级索引读路径加锁，结构与聚簇版对称但调 `lock_sec_rec_some_has_impl` |
| storage/innobase/lock/lock0lock.cc | `lock_sec_rec_modify_check_and_lock` | 二级索引写路径加锁，成功后 `page_update_max_trx_id` |
| storage/innobase/lock/lock0lock.cc | `lock_clust_rec_read_check_and_lock_alt` | 免 `offsets` 变体：内部 `Rec_offsets().compute()`，并把 `DB_SUCCESS_LOCKED_REC` 归一化为 `DB_SUCCESS` |
| storage/innobase/lock/lock0lock.cc | `lock_table_other_has_incompatible` | 表锁冲突扫描：意向锁且队列无 `LOCK_S`/`LOCK_X` 时直接返回 `nullptr` |
| storage/innobase/lock/lock0lock.cc | `lock_table_enqueue_waiting` | 表锁等待入队：`que_thr_stop`、字典操作检查（`ER_IB_MSG_642`）、`TRX_FORCE_ROLLBACK` 返回 `DB_DEADLOCK` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_unlock` | 按事务 + 模式 + `LOCK_REC_NOT_GAP` 找锁释放；找不到打印 `ER_IB_MSG_1228` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_release_gap_lock` | 释放 Next-Key 锁的间隙部分并授予等待者；supremum 需单独释放（否则 `lock_rec_grant` 会漏掉该 bit） |
| storage/innobase/lock/lock0lock.cc | `lock_trx_release_locks` | 事务提交时的整体释放：循环调 `locksys::try_release_all_locks`，最后清空 `lock_heap` |
| storage/innobase/lock/lock0lock.cc | `lock_trx_release_read_locks` | 释放读锁：先试 S 模式最多 `MAX_FAILURES = 5` 次，再无限循环试 X 模式 |
| storage/innobase/lock/lock0lock.cc | `lock_sys_create` | 分配 `lock_sys`，等待槽位数组大小 `srv_max_n_threads * sizeof(srv_slot_t)` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_hash_value` / `lock_sys_resize` | 记录锁哈希键为页 id；`lock_sys_resize` 迁移哈希表并调整分片闩锁映射 |
| storage/innobase/lock/lock0wait.cc | `lock_wait_suspend_thread` | 挂起等待线程：`trx_lock_wait_timeout_get` 取超时值、预留槽位、释放数据字典闩锁、`os_event_wait` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_table_reserve_slot` | 从 `lock_sys->waiting_threads` 起扫 `srv_max_n_threads` 个槽位；用尽时打印 `ER_IB_MSG_646` 并 `ut_error` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_check_and_cancel` / `lock_wait_try_cancel` | 超时判定（`wait_time > slot->wait_timeout`）与取消：置 `DB_LOCK_WAIT_TIMEOUT` 后调 `lock_cancel_waiting_and_release` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_timeout_thread` | 后台巡检线程：每秒检查超时（`std::chrono::seconds(1)`），每轮跑等待图死锁检测 |
| storage/innobase/lock/lock0wait.cc | `lock_wait_find_and_handle_deadlocks` | 等待图染色法找环；确认环 `MONITOR_DEADLOCK`，误报 `MONITOR_DEADLOCK_FALSE_POSITIVES` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_check_candidate_cycle` | 候选环复核：`lock_wait_trxs_are_still_in_slots` + 全局独占闩锁下 `lock_wait_trxs_are_still_waiting` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_handle_deadlock` | 选定牺牲者后：更新环上权重、`lock_notify_about_deadlock`、`lock_wait_rollback_deadlock_victim` |
| storage/innobase/lock/lock0wait.cc | `lock_wait_compute_and_publish_weights_except_cycles` | 计算调度权重：初始权重 + 入边计数累加，环上事务留作部分和 |
| storage/innobase/lock/lock0latches.cc | `Latches::Page_shards::get_shard` | 页分片闩锁映射：以 `cell_id` 而非原始哈希值为输入，保证同桶队列同分片 |
| storage/innobase/row/row0sel.cc | `row_sel_read_clust_rec` / `row_sel_set_rec_lock` | `lock_clust_rec_read_check_and_lock()` 的调用方；`row_sel_set_rec_lock` 按索引类型分派聚簇/空间/二级三条路径 |
| storage/innobase/row/row0ins.cc | `row_ins_step` / `row_ins_set_exclusive_rec_lock` | `lock_table()` 与 `lock_clust_rec_read_check_and_lock()` 的调用方；插入时同时加聚簇与二级索引锁 |
| storage/innobase/row/row0upd.cc | `row_upd_step` | `lock_table(0, node->table, LOCK_IX, thr)` 与 `lock_clust_rec_modify_check_and_lock()` 的调用方 |
| storage/innobase/row/row0mysql.cc | `row_mysql_lock_table` / `row_lock_table` / `row_mysql_lock_autoinc_table` | `lock_table()` 的调用方；后者用 `LOCK_AUTO_INC` 模式 |
| storage/innobase/btr/btr0cur.cc | `btr_cur_optimistic_update` 等 | `lock_rec_insert_check_and_lock()` 与 `lock_sec_rec_modify_check_and_lock()` 的调用方 |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 `txn/` 事务层，与 InnoDB 的 `lock0lock.cc` 没有代码级关联，但**机制形态可以对照**：

- **两者都有「等待队列 + 冲突检测 + 牺牲者选择」三段式结构**：主核的锁等待也需要在检测到环路时选一个事务回滚，本页 `lock_wait_find_and_handle_deadlocks()` → `lock_wait_choose_victim()` → `lock_wait_rollback_deadlock_victim()` 是同一形态。
- **两者都有「间隙 / 范围锁」与「记录锁」的强度分层**：主核若要在多维索引上支持范围写，也需要类似 `LOCK_GAP` / `LOCK_REC_NOT_GAP` 的标志组合，否则无法表达「锁记录但不锁区间」。
- **两者都有「快路径 + 慢路径」的分级加锁**：本页 `lock_rec_lock_fast()` / `lock_rec_lock_slow()` 的二分，与主核常见的「先试一次快速路径，失败再走完整流程」是同一形态。

**架构推论：** 本页的「冲突规则集中在一个函数里」的做法值得主核借鉴——`rec_lock_check_conflict()` 把所有间隙锁特例规则（纯间隙不阻塞、记录锁不等间隙锁、间隙锁不等 `LOCK_REC_NOT_GAP`、插入意向不阻塞、插入意向不得越过等待锁）收敛成一处返回 `NO_CONFLICT` / `CAN_BYPASS` / `HAS_TO_WAIT` 的三值判定。主核若实现范围锁，把这些特例放进一个类似的单一判定点，比分散在各加锁入口更不容易漏规则。

**架构推论：** 本页的「先扫队列第一个锁对象、不做精确匹配」是典型的空间换时间取舍。主核若已有锁对象池与位图机制，可以沿用同一思路；但主核是否已有等价实现，本页不作断言，需另行核对主核源码。

**架构推论：** 本页的死锁检测把「找环」与「选牺牲者」分开成两个函数（`lock_wait_find_and_handle_deadlocks()` 与 `lock_wait_choose_victim()`），中间用 `schedule_weight` 这个可增量维护的量传递信息。主核若要在高并发下做死锁检测，这个「权重可增量更新」的接口形态比「一次性全图重算」更适配后台巡检线程。

## 参考

- [storage/innobase/lock/lock0lock.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/lock/lock0lock.cc) —— 本页全部主符号（`lock_table`、`lock_clust_rec_read_check_and_lock`、`lock_rec_lock`）的权威来源。
- [storage/innobase/lock/lock0wait.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/lock/lock0wait.cc) —— 等待槽位、超时检查与等待图死锁检测（`lock_wait_suspend_thread`、`lock_wait_find_and_handle_deadlocks` 等）的来源。
- [InnoDB Locking（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-locking.html) —— 记录锁、间隙锁、Next-Key 锁、插入意向锁的官方语义定义，以及「间隙锁可以共存、纯抑制性」的说明，是本页 `rec_lock_check_conflict()` 中间隙锁特例规则的上位背景。
- [Deadlocks in InnoDB（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-deadlocks.html) —— 死锁的官方定义与 `innodb_deadlock_detect` / `innodb_lock_wait_timeout` 两个开关的分工，是本页「死锁检测」与「超时回滚」两条路径的上位背景。
- [Deadlock Detection（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-deadlock-detection.html) —— 官方对死锁检测开销与关闭检测的说明，以及等待列表长度上限对应的行为，是本页后台巡检线程每秒跑一轮检测这一设计取舍的对照。
