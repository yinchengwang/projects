---
project: mysql-server
feature: read-view
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/trx/trx0sys.cc
    symbols: [ReadView::check_trx_id_sanity, trx_sys_write_max_trx_id]
---

# 读视图

## 一句话职责

读视图（ReadView）是 InnoDB 在 REPEATABLE READ 下为一致性读建立的**时间点快照**：它记录下「打开视图的那一刻哪些读写事务还活着」，从而让后续每一行版本都能被判成「可见」或「不可见」；`storage/innobase/trx/trx0sys.cc` 里的 `ReadView::check_trx_id_sanity` 是这张视图附带的**自检闸门**——当被判定的那一行携带的事务号已经超过系统当前最大事务号时，它发出告警并把问题推给会话，`trx_sys_write_max_trx_id` 则负责把「系统当前最大事务号」这个水位持久化到事务系统头页上，供重启后恢复。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责维护一致性读视图（活跃事务列表）并据此判定可见性；**不负责行锁（J02）**、**不负责 undo 的结构（K 组）**。

## 机制是什么

### 一致性读要回答的问题

一致性读（consistent read）不是「读到最新的一行」，而是「读到**某一个时间点**上的一行」。要实现它，InnoDB 必须先固定一个时间点，再对每一行算：这个行版本是不是在这个时间点之前就已经提交了？

InnoDB 的做法是**事务号（trx_id）单调递增**：`trx_sys->next_trx_id_or_no` 是全局的原子计数器，`trx_sys_allocate_trx_id()` 分配 `trx->id`（写事务用），`trx_sys_allocate_trx_no()` 分配 `trx->no`（序列化号）。凡是拿到的号都严格递增，因此「号的大小」就等价于「提交先后的时间顺序」。

### 视图里到底存了什么

一张读视图记的是三样东西（字段定义在 `storage/innobase/include/read0types.h` 的 `ReadView` 类中）：

| 字段 | 含义 | 谁写它 |
|---|---|---|
| `m_low_limit_id` | 高水位：事务号 **≥** 此值的一律不可见 | `ReadView::prepare` 取 `trx_sys_get_next_trx_id_or_no()` |
| `m_up_limit_id` | 低水位：事务号 **<** 此值的一律可见 | `ReadView::prepare` 取活跃事务表里的最小 id |
| `m_ids` | 打开视图那一刻**还活着**的读写事务号列表（已排序） | `ReadView::copy_trx_ids` 从 `trx_sys->rw_trx_ids` 拷贝 |

外加一个 `m_creator_trx_id`（创建视图的事务自己的号）——**视图必须看见自己写的行**，这是「读自己的写」的实现方式。

注意 `m_ids` 只装**活跃**事务：已经提交的事务号小于 `m_up_limit_id`，不必逐个记；尚未开始的事务号大于等于 `m_low_limit_id`，也不必记。**只有「号落在两个水位之间」的活跃事务才需要进列表**——这是这张表能保持小规模的关键。

### 可见性判定：`changes_visible`

判定逻辑全部在 `ReadView::changes_visible` 里（`read0types.h`）：

```
[[nodiscard]] bool changes_visible(trx_id_t id, const table_name_t &name) const {
  ut_ad(id > 0);

  if (id < m_up_limit_id || id == m_creator_trx_id) {
    return (true);
  }

  check_trx_id_sanity(id, name);

  if (id >= m_low_limit_id) {
    return (false);

  } else if (m_ids.empty()) {
    return (true);
  }

  const ids_t::value_type *p = m_ids.data();

  return (!std::binary_search(p, p + m_ids.size(), id));
}
```

逐段读这段函数体，四个分支依次是：

1. **「早于低水位」或「就是我自己」→ 可见。** `id < m_up_limit_id` 说明这个事务在视图打开前就已经提交了（它不在 `m_ids` 里），所以它的修改可以看见。`id == m_creator_trx_id` 是「读自己的写」。
2. **调用 `check_trx_id_sanity(id, name)`** 做自检——注意这个调用夹在「已经确定不可见」之前，所以**自检对任何越过低水位的事务号都会执行**，包括那些最终被判为不可见的。
3. **`id >= m_low_limit_id` → 不可见。** 这个事务在视图打开之后才开始，它的修改在快照时间点还不存在。
4. **落到两个水位之间 → 查活跃事务表。** `m_ids` 为空则直接可见；否则用 `std::binary_search` 在有序的 `m_ids` 里找这个 id，**找到了就不可见**（这个事务当时还活着），找不到就可见（它在视图打开前已经提交，只是号恰好落在两个水位之间）。

**这是整页最关键的一点：可见性判定的全部依据是「事务号 vs 两个水位 + 活跃事务表」，而不是「事务的提交状态位」。** 提交状态是易失的，事务号是单调的——后者才能承载「某个时间点」的语义。

### `check_trx_id_sanity`：视图的自检闸门

`ReadView::check_trx_id_sanity` 的完整函数体（`storage/innobase/trx/trx0sys.cc`）：

```
void ReadView::check_trx_id_sanity(trx_id_t id, const table_name_t &name) {
  if (&name == &dict_sys->dynamic_metadata->name) {
    /* The table mysql.innodb_dynamic_metadata uses a
    constant DB_TRX_ID=~0. */
    ut_ad(id == (1ULL << 48) - 1);
    return;
  }

  if (id >= trx_sys_get_next_trx_id_or_no()) {
    ib::warn(ER_IB_MSG_1196)
        << "A transaction id"
        << " in a record of table " << name << " is newer than the"
        << " system-wide maximum.";
    ut_d(ut_error);
#ifndef UNIV_DEBUG
    THD *thd = current_thd;
    if (thd != nullptr) {
      char table_name[MAX_FULL_NAME_LEN + 1];

      innobase_format_name(table_name, sizeof(table_name), name.m_name);

      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SIGNAL_WARN,
                          "InnoDB: Transaction id"
                          " in a record of table"
                          " %s is newer than system-wide"
                          " maximum.",
                          table_name);
    }
#endif
  }
}
```

三件事，一件不多：

1. **白名单：`mysql.innodb_dynamic_metadata`。** 判据是**指针相等**（`&name == &dict_sys->dynamic_metadata->name`），不是字符串比较。这张表用的 `DB_TRX_ID` 是常量 `(1ULL << 48) - 1`（源码注释写作 `DB_TRX_ID=~0`），调试版本用 `ut_ad` 断言这个常量成立，然后**直接 return**——不去跟系统最大事务号比较。
2. **异常判定：`id >= trx_sys_get_next_trx_id_or_no()`。** 这个门槛就是「系统当前已分配出去的最大事务号」。注意判定用的是 `>=`：等于水位也算异常，因为 `next_trx_id_or_no` 本身是**下一个将要分配**的号，一个已经拿到号的活跃事务其号必然小于它。
3. **两级上报。** 调试版本（`ut_d(ut_error)`）直接触发 `ut_error` 中止；发布版本（`#ifndef UNIV_DEBUG`）用 `push_warning_printf` 以 `Sql_condition::SL_WARNING` 的等级把告警推给当前 THD，条件码是 `ER_SIGNAL_WARN`，消息文本是 `"InnoDB: Transaction id in a record of table %s is newer than system-wide maximum."`。

**注意 `push_warning_printf` 这一段被 `#ifndef UNIV_DEBUG` 包住，而它上面的 `ut_d(ut_error)` 只在调试版本生效——两者是互斥的，不是叠加的。** 发布版本的行为是「记日志 + 推一条会话级 warning」，不中止。

**这个函数是纯检查，不改变判定结果。** `changes_visible` 调用它之后，无论它告不告警，判定都会继续走完（上面第 3、4 段）。

### `trx_sys_write_max_trx_id`：把水位持久化

`trx_sys_write_max_trx_id` 的完整函数体（同一个文件）：

```
void trx_sys_write_max_trx_id(void) {
  mtr_t mtr;
  trx_sysf_t *sys_header;

  /* The final synchronization happens here between maximum 2 threads,
  one holding the trx_sys_t::mutex and one holding the serialisation
  mutex. They can concurrently enter here, and start their mtrs.
  They will synchronize inside trx_sysf_get because only one of them
  could succeed in acquiring the x-lock for the header page to modify.
  That thread will then read the max_trx_id and write to the page.
  After it finishes mtr_commit, the another thread will succeed in
  acquiring the x-lock and it will again read the newest max_trx_id,
  and possibly re-write it. */

  ut_ad(trx_sys_mutex_own() || trx_sys_serialisation_mutex_own());

  if (!srv_read_only_mode) {
    DBUG_EXECUTE_IF(
        "trx_sys_write_max_trx_id__all_blocked",
        while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); });

#ifdef UNIV_DEBUG
    if (trx_sys_serialisation_mutex_own()) {
      DEBUG_SYNC_C("trx_sys_write_max_trx_id__ser");
    }
#endif /* UNIV_DEBUG */

    mtr_start(&mtr);

    sys_header = trx_sysf_get(&mtr);

    const trx_id_t max_trx_id = trx_sys->next_trx_id_or_no.load();

    mlog_write_ull(sys_header + TRX_SYS_TRX_ID_STORE, max_trx_id, &mtr);

    mtr_commit(&mtr);
  }
}
```

要点：

- **入口断言 `ut_ad(trx_sys_mutex_own() || trx_sys_serialisation_mutex_own())`**——调用方必须持有两把互斥量之一。这两把闩分别是「保护 `trx->id` 分配」和「保护 `trx->no` 分配」的，源码注释说明了它们可以并发进入本函数，最终在 `trx_sysf_get` 里靠对头页的 x-lock 串行化。
- **只读模式直接跳过**（`if (!srv_read_only_mode)`）。只读实例不分配新事务号，也就不需要落盘。
- **写到哪里**：`sys_header + TRX_SYS_TRX_ID_STORE`。`TRX_SYS_TRX_ID_STORE` 是事务系统头页里存放「最大事务号」的字段偏移（定义在 `storage/innobase/include/trx0sys.h`，值为 0）。用的是 `mlog_write_ull`——写 redo 日志，所以这个值是崩溃可恢复的。
- **写的是 `trx_sys->next_trx_id_or_no.load()`，不是「已经用掉的最大号」**，而是「下一个将要分配的号」。这是有意的：头页里存的值必须**高于任何已经用过的号**。

### 什么时候写：`TRX_SYS_TRX_ID_WRITE_MARGIN`

`trx_sys_write_max_trx_id` 不是每分配一个号就调一次。分配侧（`storage/innobase/include/trx0sys.ic` 的 `trx_sys_allocate_trx_id_or_no`）的规则是：

```
inline trx_id_t trx_sys_allocate_trx_id_or_no() {
  ut_ad(trx_sys_mutex_own() || trx_sys_serialisation_mutex_own());

  trx_id_t trx_id = trx_sys->next_trx_id_or_no.fetch_add(1);

  if (trx_id % trx_sys_get_trx_id_write_margin() == 0) {
    ...
    trx_sys_write_max_trx_id();
  }
  return trx_id;
}
```

`trx_sys_get_trx_id_write_margin()` 返回 `TRX_SYS_TRX_ID_WRITE_MARGIN`，**该常量定义为 `256`**（`storage/innobase/include/trx0sys.h`）；只有在 DBUG 开关 `trx_sys_low_trx_id_write_margin` 打开时才返回 8。`TRX_SYS_TRX_ID_WRITE_MARGIN` 必须是 2 的幂——源码注释明写「which must be a power of two」，`%` 取模才等价于位掩码测试。

**所以：每分配 256 个事务号，才往头页落一次盘。** 注释同时解释了为什么一次跳 256 不会漏号：命中这个条件的那条线程**持有 `trx_sys_mutex` 或 `trx_sys_serialisation_mutex` 之一且一直持有到 `trx_sys_write_max_trx_id()` 返回**，所以从这次写入到下一次写入之间，被保留的号区间一定连续。

### 重启后的水位恢复

`trx_sys_init_at_db_start` 读回头页里的值并抬高水位：

```
const trx_id_t max_trx_id =
    mach_read_from_8(sys_header + TRX_SYS_TRX_ID_STORE);

trx_sys->next_trx_id_or_no.store(max_trx_id +
                                 2 * trx_sys_get_trx_id_write_margin());

trx_sys->serialisation_min_trx_no.store(trx_sys->next_trx_id_or_no.load());
```

**系数是 2，源码注释专门解释了为什么不能改小**：注释点名了「两个线程可能并发抬高 `next_trx_id_or_no`——一个持 `trx_sys_mutex`，一个持 `trx_sys_serialisation_mutex`」，并直接给出后果——「如果你把这个系数 2 改小，`innodb.max_trx_id` 这个测试就会失败」。所以 `2 × 256 = 512` 这个抬高量不是随手写的。

抬高的目的是避免**崩溃重启后事务号回退、进而与旧事务号重叠**。源码注释说得很直接：「这样事务号在数据库反复崩溃重启时不会重叠」。

创建库时的初值是 1（`trx_sysf_create` 里的 `mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1)`，注释写着「Start counting transaction ids from number 1 up」）。

### 视图的生命周期

**打开**：`trx_assign_read_view`（`storage/innobase/trx/trx0trx.cc`）：

```
ReadView *trx_assign_read_view(trx_t *trx) {
  ut_ad(trx_can_be_handled_by_current_thread_or_is_hp_victim(trx));
  ut_ad(trx->state.load(std::memory_order_relaxed) == TRX_STATE_ACTIVE);

  if (srv_read_only_mode) {
    ut_ad(trx->read_view == nullptr);
    return (nullptr);

  } else if (!MVCC::is_view_active(trx->read_view)) {
    trx_sys->mvcc->view_open(trx->read_view, trx);
  }

  return (trx->read_view);
}
```

三个分支：**只读模式返回 `nullptr`（不建视图）**；**已有活跃视图就复用**（这正是 REPEATABLE READ 下「同一事务内所有一致性读看到同一个快照」的落点）；否则调 `trx_sys->mvcc->view_open()` 新建。

`MVCC::view_open` 内部（`storage/innobase/read/read0read.cc`）还有一条**复用捷径**：如果事务是「autocommit 非加锁」事务（`trx_is_autocommit_non_locking(trx)`）且旧视图的 `m_ids` 为空，同时旧视图的 `m_low_limit_id` 仍等于当前的 `trx_sys_get_next_trx_id_or_no()`，就直接把旧视图重新标记为未关闭并返回——**没有新写事务开始时，不必重建快照**。

`MVCC::view_open` 真正的建视图动作是 `ReadView::prepare`：

```
void ReadView::prepare(trx_id_t id) {
  ut_ad(trx_sys_mutex_own());

  m_creator_trx_id = id;

  m_low_limit_no = trx_get_serialisation_min_trx_no();

  m_low_limit_id = trx_sys_get_next_trx_id_or_no();

  ut_a(m_low_limit_no <= m_low_limit_id);

  if (!trx_sys->rw_trx_ids.empty()) {
    copy_trx_ids(trx_sys->rw_trx_ids);
  } else {
    m_ids.clear();
  }

  /* The first active transaction has the smallest id. */
  m_up_limit_id = !m_ids.empty() ? m_ids.front() : m_low_limit_id;

  ut_a(m_up_limit_id <= m_low_limit_id);

  ut_d(m_view_low_limit_no = m_low_limit_no);
  m_closed = false;
}
```

**全部读视图的建立都在 `trx_sys_mutex` 保护下**（`ut_ad(trx_sys_mutex_own())`），这是「快照是原子的」这一保证的来源：取 `m_low_limit_id`、拷 `rw_trx_ids`、算 `m_up_limit_id` 三步之间不可能有别的线程插入。

**关闭**：`MVCC::view_close` 用指针最低位当「已关闭」标记位。`own_mutex == true`（调用方持有 `trx_sys->mutex`）走完整路径：`view->close()` → `UT_LIST_REMOVE(m_views, view)` → `UT_LIST_ADD_LAST(m_free, view)`，视图回到空闲池可被复用。`own_mutex == false` 只打标记位，视图留在 `m_views` 里但 `m_closed == true`，由后续的 `MVCC::view_release` 回收。

`trx0trx.cc` 里三处调用 `view_close`：

| 调用点 | `own_mutex` | 场景 |
|---|---|---|
| `trx_erase_lists` | `true` | 读写事务从 `rw_trx_list` 摘除时（注释：「为了 consistent snapshot，我们需要在提交和释放锁之前把当前事务从 mvcc 的运行事务 id 列表里移除」） |
| `trx_commit_in_memory` 的 AC-NL-RO 分支 | `false` | 只加关闭标记，不摘链 |
| `trx_commit_in_memory` 的只读 / 无 redo rseg 分支 | `false` | 同上 |

**提交时「先摘链、后释放锁」的顺序是刻意的**，源码注释写明：「For consistent snapshot, we need to remove current transaction from running transaction id list for mvcc before doing commit and releasing locks.」

**「读自己的写」的实现**：`trx_start_low`（`trx0trx.cc`）里分配 `trx->id` 之后：

```
/* So that we can see our own changes. */
if (MVCC::is_view_active(trx->read_view)) {
  MVCC::set_view_creator_trx_id(trx->read_view, trx->id);
}
```

`MVCC::set_view_creator_trx_id` 最终调 `ReadView::creator_trx_id(id)`，而 `creator_trx_id` 的实现里有 `ut_ad(m_creator_trx_id == 0)`——**只允许设置一次**。

### 视图与 purge 的关系

purge 侧也用一张视图，但它取的是**最老的那张活跃视图**：`MVCC::clone_oldest_view`（`read0read.cc`）从 `m_views` 链表尾部往回找第一个 `!is_closed()` 的视图（`get_oldest_view`），克隆它的内容，再 `copy_complete()` 把克隆者的 `m_creator_trx_id` 插进 `m_ids` 并调整 `m_up_limit_id`。

`storage/innobase/trx/trx0purge.cc` 的 `trx_purge_sys_initialize` 里建了 purge 自己的视图：

```
new (&purge_sys->view) ReadView();

trx_sys->mvcc->clone_oldest_view(&purge_sys->view);

purge_sys->view_active = true;
```

`trx_purge` 每轮批次开始时会重新克隆一次（更新 `purge_sys->view`）。purge 用这张视图的两处判据：

- `trx_purge_fetch_next_rec` 里：`if (purge_sys->iter.trx_no >= purge_sys->view.low_limit_no()) return nullptr;`——**事务号已经达到视图的低水位就不能再 purge 了**。
- `row_vers_must_preserve_del_marked`（`storage/innobase/row/row0vers.cc`）：`return (!purge_sys->view.changes_visible(trx_id, name));`——**如果视图看不见这个事务的修改，delete-mark 的旧版本就必须保留**。

**这就是「读视图」在 InnoDB 里承担的第二重职责：它同时是 purge 的护栏。** 长事务不提交 → 视图不关闭 → purge 视图的 `low_limit_no` 抬不上去 → undo 无法回收。

### 视图的关闭检查

`trx_sys_close`（`trx0sys.cc`）在关停时检查是否有视图漏关：

```
ulint size = trx_sys->mvcc->size();

if (size > 0) {
  ib::error(ER_IB_MSG_1201) << "All read views were not closed before"
                                 " shutdown: "
                              << size << " read views open";
}
```

`MVCC::size()` 遍历 `m_views`，只数 `!is_closed()` 的。**这是一个错误级日志（`ib::error`），不是中止**——关停流程继续往下走。

## 怎么实现的

### 判定路径总览

一行数据从磁盘到「可见 / 不可见」的完整调用链：

| 环节 | 文件 | 符号 | 做什么 |
|---|---|---|---|
| 取记录上的事务号 | storage/innobase/lock/lock0lock.cc | `row_get_rec_trx_id` 取出的 `trx_id` | 从记录里读出 `DB_TRX_ID` |
| 聚簇索引一致性读判定 | storage/innobase/lock/lock0lock.cc | `lock_clust_rec_cons_read_sees` | 调 `view->changes_visible(trx_id, index->table->name)` |
| 二级索引一致性读判定 | storage/innobase/lock/lock0lock.cc | `lock_clust_rec_cons_read_sees` 之外的二级索引分支 | 同样落到 `changes_visible` |
| 构造旧版本 | storage/innobase/row/row0vers.cc | `row_vers_build_for_consistent_read` | 沿 undo 链回溯，逐版本调 `view->changes_visible` |
| purge 护栏 | storage/innobase/row/row0vers.cc | `row_vers_must_preserve_del_marked` | `!purge_sys->view.changes_visible(trx_id, name)` |
| 索引可见性 | storage/innobase/dict/dict0mem.cc | `dict_index_t::is_usable` | `trx->read_view->changes_visible(trx_id, table->name)` |

`lock_clust_rec_cons_read_sees` 的函数体（`storage/innobase/lock/lock0lock.cc`）：

```
bool lock_clust_rec_cons_read_sees(
    const rec_t *rec, const dict_index_t *index, const ulint *offsets,
    ReadView *view) {
  ut_ad(index->is_clustered());
  ut_ad(page_rec_is_user_rec(rec));
  ut_ad(rec_offs_validate(rec, index, offsets));

  /* Temp-tables are not shared across connections and multiple
  transactions from different connections cannot simultaneously
  operate on same temp-table and so read of temp-table is
  always consistent read. */
  if (srv_read_only_mode || index->table->is_temporary()) {
    ut_ad(view == nullptr || index->table->is_temporary());
    return (true);
  }

  /* NOTE that we call this function while holding the search
  system latch. */

  trx_id_t trx_id = row_get_rec_trx_id(rec, index, offsets);

  return (view->changes_visible(trx_id, index->table->name));
}
```

**两个短路分支：只读模式与临时表一律直接返回可见。** 源码注释给了理由——临时表不跨连接共享，不同连接的事务不可能同时操作同一张临时表，所以临时表的读**总是**一致性读。

`row_vers_build_for_consistent_read`（`storage/innobase/row/row0vers.cc`）是「不可见时怎么办」的实现——沿 undo 链往回找可见版本：

```
trx_id = row_get_rec_trx_id(rec, index, *offsets);
...
ut_ad(!view->changes_visible(trx_id, index->table->name));
...
for (;;) {
  ...
  bool purge_sees =
      trx_undo_prev_version_build(rec, mtr, version, index, *offsets, heap,
                                  &prev_version, nullptr, vrow, 0, lob_undo);

  err = (purge_sees) ? DB_SUCCESS : DB_MISSING_HISTORY;

  if (prev_version == nullptr) {
    /* It was a freshly inserted version */
    *old_vers = nullptr;
    break;
  }

  *offsets = rec_get_offsets(prev_version, index, *offsets, ULINT_UNDEFINED,
                             UT_LOCATION_HERE, offset_heap);
  ...
  trx_id = row_get_rec_trx_id(prev_version, index, *offsets);

  if (view->changes_visible(trx_id, index->table->name)) {
    /* The view already sees this version: we can copy
    it to in_heap and return */
    buf = static_cast<byte *>(mem_heap_alloc(in_heap, rec_offs_size(*offsets)));
    *old_vers = rec_copy(buf, prev_version, *offsets);
    ...
    break;
  }

  version = prev_version;
}
```

循环的终止条件有两个：**找到可见版本**（复制出来返回）或**`prev_version == nullptr`**（说明是「快照之后新插入的行」，此时 `*old_vers = nullptr` 表示「这个记录在视图里根本不存在」）。中间还有一个 `purge_sees` 判定——**如果 purge 已经看不到了（undo 链断了），返回 `DB_MISSING_HISTORY`**。

### 「视图没分配」的运行时告警

`row0sel.cc` 的一致性读路径上有一处硬检查：

```
if (!MVCC::is_view_active(trx->read_view) && !srv_read_only_mode &&
    prebuilt->select_lock_type == LOCK_NONE) {
  ib::error(ER_IB_MSG_1031) << "MySQL is trying to perform a"
                               " consistent read but the read view is not"
                               " assigned!";
  trx_print(stderr, trx, 600);
  fputc('\n', stderr);
  ut_error;
}
```

**判据是三个条件的合取**：视图不活跃 **且** 不在只读模式 **且** `select_lock_type == LOCK_NONE`（即无锁读，也就是一致性读）。这说明「一致性读必须有视图」是 InnoDB 内部的不变量，违反时直接 `ut_error` 中止。

同一个文件里另一处 `MVCC::is_view_active(trx->read_view)` 参与的是**走自适应哈希索引的准入条件**：

```
if (trx->mysql_n_tables_locked == 0 && !prebuilt->ins_sel_stmt &&
    prebuilt->select_lock_type == LOCK_NONE &&
    trx->isolation_level > TRX_ISO_READ_UNCOMMITTED &&
    MVCC::is_view_active(trx->read_view)) {
  /* This is a SELECT query done as a consistent read,
  and the read view has already been allocated:
  let us try a search shortcut through the hash
  index. ... */
```

**四个前置条件缺一不可**，其中 `MVCC::is_view_active(trx->read_view)` 保证「已有快照」——否则走 AHI 快捷路径就无法保证一致性。

### 视图的分配时机：不是每条 SELECT 都建

`row0sel.cc` 里 `prebuilt->sql_stat_start` 区分了两条路径：

```
if (!prebuilt->sql_stat_start) {
  /* No need to set an intention lock or assign a read view */

  if (!MVCC::is_view_active(trx->read_view) && !srv_read_only_mode &&
      prebuilt->select_lock_type == LOCK_NONE) {
    ... ut_error;
  }
}
```

**`sql_stat_start == false` 表示这不是语句的第一次取数**，此时不分配新视图，直接复用事务已有的视图。这就是 REPEATABLE READ 下快照在整个事务内保持不变的实现点。

### 异常事务号的两条不同的检查路径

值得注意的是，「事务号异常」在 InnoDB 里由**两个不同的函数**检查，行为不同：

| 检查者 | 文件 | 触发条件 | 行为 |
|---|---|---|---|
| `ReadView::check_trx_id_sanity` | storage/innobase/trx/trx0sys.cc | `changes_visible` 里对越过 `m_low_limit_id` 的号 | `ib::warn` + 调试版 `ut_error` + 发布版 `push_warning_printf` 推会话 warning |
| `lock_check_trx_id_sanity` | storage/innobase/lock/lock0lock.cc | 记录的事务号 vs `trx_sys_get_next_trx_id_or_no()` | 返回 bool；失败时调 `lock_report_trx_id_insanity` 打 `ib::error` |

`lock_check_trx_id_sanity` 的函数体（`storage/innobase/lock/lock0lock.cc`）：

```
bool lock_check_trx_id_sanity(trx_id_t trx_id, const rec_t *rec,
                              const dict_index_t *index, const ulint *offsets) {
  ut_ad(rec_offs_validate(rec, index, offsets));

  trx_id_t next_trx_id = trx_sys_get_next_trx_id_or_no();
  bool is_ok = trx_id < next_trx_id;

  if (!is_ok) {
    lock_report_trx_id_insanity(trx_id, rec, index, offsets, next_trx_id);
  }

  return (is_ok);
}
```

**注意两者的比较运算符不同：`check_trx_id_sanity` 用 `>=`（等于水位也算异常），`lock_check_trx_id_sanity` 用 `<`（等于水位算正常）。** 这不是笔误——`next_trx_id_or_no` 是「下一个将要分配的号」，等于它的号尚未被分配出去；而 `check_trx_id_sanity` 判 `>=` 是因为它拿到的号来自一个**已经存在的事务**，等于水位意味着「有个事务拿到了本不该拿到的号」。

`lock_report_trx_id_insanity` 打的是 `ib::error(ER_IB_MSG_634)`，消息含事务号、记录内容、索引名、表名与全局计数器。

`lock_check_trx_id_sanity` 的两个调用方：

| 调用方 | 文件 | 失败后怎么办 |
|---|---|---|
| `lock_sec_rec_some_has_impl` | storage/innobase/lock/lock0lock.cc | 源码注释：「The page is corrupt: try to avoid a crash by returning 0」——`trx = nullptr`，回退到「没有隐式锁」 |
| `row_vers_impl_x_locked` 的邻近路径 | storage/innobase/row/row0vers.cc | 注释：「该修改或插入 `clust_rec` 的事务已不再活跃，或者它已损坏：记录上没有隐式锁」——`return nullptr` |

**这两处回退的共同点是「返回空指针 / 视作无隐式锁」，而不是中止。** 源码注释明确说是「try to avoid a crash」。

### `check_trx_id_sanity` 的白名单为什么是指针相等

`mysql.innodb_dynamic_metadata` 表用的 `DB_TRX_ID` 是常量 `(1ULL << 48) - 1`。这个常量**远大于任何真实事务号**，而 `changes_visible` 的第一步就是 `if (id < m_up_limit_id || id == m_creator_trx_id) return true;`——常量 `(1ULL << 48) - 1` 不满足 `< m_up_limit_id`，所以**如果不做白名单，它会被判为不可见**。

白名单的比较用**指针相等**（`&name == &dict_sys->dynamic_metadata->name`）而不是字符串比较，这样不依赖表名的字符串形式，也不必在热路径上做字符串比较。

**架构推论：** 指针相等意味着白名单命中的是「字典里那个 `mysql.innodb_dynamic_metadata` 表对象本身」。表被 drop 并重建后对象地址会变，但重建后的 `dict_sys->dynamic_metadata` 仍指向新对象，因此白名单依然生效；这一点本页未检出 `dict0mem.cc` 中 `dynamic_metadata` 的赋值路径，属推论，不作断言。

### 数字与常量的来源汇总

| 数字 / 表达式 | 来源 | 含义 |
|---|---|---|
| `(1ULL << 48) - 1` | storage/innobase/trx/trx0sys.cc（`check_trx_id_sanity` 内联字面量，注释写 `DB_TRX_ID=~0`） | `mysql.innodb_dynamic_metadata` 的常量事务号 |
| `1ULL << 48` | 同上 | 48 位左移后减 1 |
| `TRX_SYS_TRX_ID_WRITE_MARGIN = 256` | storage/innobase/include/trx0sys.h | 每分配多少个事务号往头页落一次盘；必须是 2 的幂 |
| `2 * trx_sys_get_trx_id_write_margin()` | storage/innobase/trx/trx0sys.cc（`trx_sys_init_at_db_start`） | 重启后水位抬高的倍数；系数 2 由「两个线程并发抬高」决定，源码点名 `innodb.max_trx_id` 测试 |
| `TRX_SYS_TRX_ID_STORE = 0` | storage/innobase/include/trx0sys.h | 头页里存最大事务号的字段偏移 |
| `TRX_SYS_OLD_N_RSEGS = 256` | storage/innobase/include/trx0sys.h | 旧版本回滚段槽位数，用于文件兼容（`trx_sysf_create` 里 `std::max(TRX_SYS_OLD_N_RSEGS, TRX_SYS_N_RSEGS)`） |
| `1024` | storage/innobase/trx/trx0sys.cc（`trx_sys_create`） | `MVCC` 构造参数：预分配的读视图个数（`ut::new_withkey<MVCC>(..., 1024)`） |
| `TRX_SHARDS_N = 256` | storage/innobase/include/trx0sys.h | 事务分片数；`trx_get_shard_no` 用 `trx_id % TRX_SHARDS_N` |
| `1000000000` | storage/innobase/trx/trx0sys.cc（`trx_sys_init_at_db_start`） | 回滚行数统计的显示单位切换阈值：超过则改用 `"M"` 单位并除以 1000000 |
| `MAX_FULL_NAME_LEN + 1` | storage/innobase/trx/trx0sys.cc（`check_trx_id_sanity`） | `push_warning_printf` 前格式化表名的缓冲区大小 |

## 关键取舍

### 用「事务号 + 水位」而不是「提交状态位」来表达可见性

可见性判定的输入是三个数字（`m_low_limit_id`、`m_up_limit_id`、`m_ids`），不是任何事务的状态字段。**取舍：放弃了「直接看事务状态」这一更直觉的做法，换来判定的 O(log n) 成本（`std::binary_search`）与「快照是一个不可变值」的语义。** 状态位是易失的——提交后就没了；而快照必须能在整个事务生命周期内反复复用同一个判定依据。

### 只记「水位之间的活跃事务」

`m_ids` 不装所有活跃事务，只装事务号落在 `[m_up_limit_id, m_low_limit_id)` 区间里的那些。**取舍：视图大小与并发事务数不成正比，换来的是「早于低水位的已提交事务不必逐个记」。** 代价是 `m_ids` 的复制开销随该区间内的活跃事务数增长——这是 `copy_trx_ids` 里那段 `memmove` 加 `std::lower_bound` 定位 creator trx id 的代码存在的原因。

### 判定与自检耦合在同一个函数里

`changes_visible` 里直接调 `check_trx_id_sanity`，把「判可见性」和「判数据是否损坏」缝在一起。**取舍：一致性读的判定热路径上多了一次「事务号 vs 系统水位」的比较和一次指针比较，换来「读到不该存在的版本时立刻告警」而不是「静默返回错误结果」。** 代价是这条路径上的告警在异常数据下会反复触发（每行都判一次）。

### 发布版只推 warning，不中止

`check_trx_id_sanity` 在发布版本（`#ifndef UNIV_DEBUG`）只 `push_warning_printf` 推一条会话级 warning，调试版本则 `ut_d(ut_error)` 中止。**取舍：放弃「一发现问题就停下来」的强硬态度，换取生产环境不因为单行数据的异常而中断整个服务。** `mysql.innodb_dynamic_metadata` 的白名单是这条路上的例外——那个常量是设计上的，不是异常。

### `check_trx_id_sanity` 与 `lock_check_trx_id_sanity` 的判据不同

前者用 `>=`，后者用 `<`。**取舍：两个语义相近的检查函数没有统一成同一个比较运算符，换来各自场景下的判定正确性。** 差别源于入参含义不同：前者拿到的是「某个已存在事务的号」（等于水位说明号分配出了问题），后者拿到的是「记录里的号」（等于水位说明这个号尚未被分配，记录来自未来）。

### 异常时宁可「视作无锁」也不崩

`lock_sec_rec_some_has_impl` 在 `lock_check_trx_id_sanity` 失败时把 `trx` 置为 `nullptr` 并继续，源码注释：「The page is corrupt: try to avoid a crash by returning 0」。**取舍：放弃「检测到损坏就报错中止」，换来「损坏数据不会导致整个实例崩溃」。** 代价是损坏被降级成了静默的行为差异。

### 视图既是读的一致性依据，也是 purge 的护栏

同一套 `ReadView` 结构被两条路径复用：用户事务用它判可见性，purge 用它的克隆体判「这条 undo 还能不能删」。**取舍：把两件事绑在同一个生命周期上，换来「长事务不提交 → undo 不回收」这一约束自动成立，不需要第二套记账机制。** 代价是长事务会同时拖住读一致性和 undo 空间——这正是 InnoDB 文档反复劝用户「即使只做一致性读也要及时提交」的原因。

### 提交时先摘链、后释放锁

`trx_erase_lists` 在提交路径上先把自己从 `rw_trx_ids` 里摘掉，然后才释放锁。源码注释：「For consistent snapshot, we need to remove current transaction from running transaction id list for mvcc before doing commit and releasing locks.」**取舍：牺牲「提交与摘链原子完成」的表述便利，换来「视图看到的活跃事务集合」与「持锁事务集合」不会错位。**

### 每 256 个号才落一次盘

`TRX_SYS_TRX_ID_WRITE_MARGIN = 256` 让写头页的频率降到「每 256 个事务号一次」。**取舍：崩溃后最多浪费 255 个事务号，换来头页写入的 mtr 与 redo 开销被摊薄 256 倍。** 兜底是 `trx_sys_init_at_db_start` 里那个 `2 × 256 = 512` 的抬高量，以及「持有互斥量直到写盘完成」这条保证。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/trx/trx0sys.cc | `ReadView::check_trx_id_sanity` | 读视图的自检闸门：白名单 `mysql.innodb_dynamic_metadata`（指针相等，调试版断言常量 `(1ULL << 48) - 1`）；`id >= trx_sys_get_next_trx_id_or_no()` 判异常 → `ib::warn(ER_IB_MSG_1196)` + 调试版 `ut_d(ut_error)` + 发布版 `push_warning_printf` 推会话 warning（`ER_SIGNAL_WARN`） |
| storage/innobase/trx/trx0sys.cc | `trx_sys_write_max_trx_id` | 把 `trx_sys->next_trx_id_or_no.load()` 通过 `mlog_write_ull` 写到头页的 `TRX_SYS_TRX_ID_STORE` 偏移；入口断言持有 `trx_sys_mutex` 或 `trx_sys_serialisation_mutex` 之一；`srv_read_only_mode` 时跳过 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_init_at_db_start` | 启动时用 `mach_read_from_8(sys_header + TRX_SYS_TRX_ID_STORE)` 读回水位，抬高为 `max_trx_id + 2 * trx_sys_get_trx_id_write_margin()`；初始化 `serialisation_min_trx_no`；调 `trx_sys_write_max_trx_id()` 落盘；统计待回滚事务与行数（超 `1000000000` 改用 `"M"` 单位） |
| storage/innobase/trx/trx0sys.cc | `trx_sys_create` | 建 `trx_sys_t`；`trx_sys->mvcc = ut::new_withkey<MVCC>(UT_NEW_THIS_FILE_PSI_KEY, 1024)` 预分配 1024 张视图；建 `mutex` 与 `serialisation_mutex`；初始化 `serialisation_list` / `rw_trx_list` / `mysql_trx_list` |
| storage/innobase/trx/trx0sys.cc | `trx_sys_close` | 关停时用 `trx_sys->mvcc->size()` 检查漏关的视图，`> 0` 则打 `ib::error(ER_IB_MSG_1201)` |
| storage/innobase/trx/trx0sys.cc | `trx_sysf_create` | 建库时把 `TRX_SYS_TRX_ID_STORE` 处的计数初值写成 1；回滚段槽位按 `std::max(TRX_SYS_OLD_N_RSEGS, TRX_SYS_N_RSEGS)` 初始化 |
| storage/innobase/trx/trx0sys.cc | `trx_sys_oldest_trx_no` | 序列化队列非空时返回队首事务的 `no`，否则返回 `trx_sys_get_next_trx_id_or_no()` |
| storage/innobase/trx/trx0trx.cc | `trx_assign_read_view` | 一致性读的视图入口：`srv_read_only_mode` 返回 `nullptr`；已有活跃视图则复用；否则调 `trx_sys->mvcc->view_open(trx->read_view, trx)` |
| storage/innobase/trx/trx0trx.cc | `trx_erase_lists` | 提交 / 回滚时把事务从 `rw_trx_list` 与 `rw_trx_ids` 摘除，并调 `trx_sys->mvcc->view_close(trx->read_view, true)` 完整关闭视图 |
| storage/innobase/trx/trx0trx.cc | `trx_commit_in_memory` | AC-NL-RO 分支与只读 / 无 redo rseg 分支调 `trx_sys->mvcc->view_close(trx->read_view, false)`（只打关闭标记） |
| storage/innobase/trx/trx0trx.cc | `trx_start_low` | 分配 `trx->id` 后，若视图已活跃则 `MVCC::set_view_creator_trx_id(trx->read_view, trx->id)`，实现「读自己的写」 |
| storage/innobase/trx/trx0trx.cc | `trx_add_to_serialisation_list` | 分配 `trx->no`；只读事务不进 `serialisation_list`；队列长度变 1 时更新 `serialisation_min_trx_no` |
| storage/innobase/trx/trx0trx.cc | `trx_erase_from_serialisation_list_low` | 从 `serialisation_list` 摘除；队列非空则取队首 `no`，为空则置为 `trx_sys_get_next_trx_id_or_no()` |
| storage/innobase/lock/lock0lock.cc | `lock_clust_rec_cons_read_sees` | 聚簇索引一致性读判定：`srv_read_only_mode` 或临时表直接可见，否则 `view->changes_visible(trx_id, index->table->name)` |
| storage/innobase/lock/lock0lock.cc | `lock_check_trx_id_sanity` | 独立的事务号合理性检查：`trx_id < trx_sys_get_next_trx_id_or_no()` 为真才正常，失败调 `lock_report_trx_id_insanity`（`ib::error(ER_IB_MSG_634)`） |
| storage/innobase/lock/lock0lock.cc | `lock_sec_rec_some_has_impl` | 二级索引隐式锁查找；`lock_check_trx_id_sanity` 失败时按注释「try to avoid a crash by returning 0」把 `trx` 置空 |
| storage/innobase/row/row0vers.cc | `row_vers_build_for_consistent_read` | 沿 undo 链回溯构造可见旧版本：逐版本 `view->changes_visible`；`prev_version == nullptr` 表示快照后新插入；`purge_sees` 为假时返回 `DB_MISSING_HISTORY` |
| storage/innobase/row/row0vers.cc | `row_vers_must_preserve_del_marked` | purge 护栏：`!purge_sys->view.changes_visible(trx_id, name)` 为真则 delete-mark 的旧版本必须保留 |
| storage/innobase/row/row0sel.cc | `row_sel`（一致性读路径） | `sql_stat_start == false` 时复用事务已有视图；视图不活跃且非只读且 `select_lock_type == LOCK_NONE` 时打 `ib::error(ER_IB_MSG_1031)` 并 `ut_error` |
| storage/innobase/row/row0sel.cc | `row_sel`（AHI 快捷路径准入） | 走自适应哈希索引需同时满足 `mysql_n_tables_locked == 0`、非 `ins_sel_stmt`、`select_lock_type == LOCK_NONE`、隔离级别高于 `TRX_ISO_READ_UNCOMMITTED`、`MVCC::is_view_active(trx->read_view)` |
| storage/innobase/dict/dict0mem.cc | `dict_index_t::is_usable` | 索引可见性判定：临时表、`trx_id == 0`、视图不活跃，或 `trx->read_view->changes_visible(trx_id, table->name)` |
| storage/innobase/trx/trx0purge.cc | `trx_purge_sys_initialize` | 用 `new (&purge_sys->view) ReadView()` 建 purge 视图，再 `trx_sys->mvcc->clone_oldest_view(&purge_sys->view)` 克隆最老的活跃视图 |
| storage/innobase/trx/trx0purge.cc | `trx_purge` | 每轮 purge 批次开始时在 `purge_sys->latch` 排他闩保护下重新 `clone_oldest_view` |
| storage/innobase/trx/trx0purge.cc | `trx_purge_fetch_next_rec` | purge 护栏：`purge_sys->iter.trx_no >= purge_sys->view.low_limit_no()` 时返回 `nullptr`，停止本轮回收 |
| storage/innobase/include/trx0sys.h | `TRX_SYS_TRX_ID_WRITE_MARGIN` | 常量 256：每分配多少个事务号往头页落一次盘；注释要求必须是 2 的幂 |
| storage/innobase/include/trx0sys.h | `TRX_SYS_TRX_ID_STORE` | 常量 0：头页中存放最大事务号的字段偏移 |
| storage/innobase/include/trx0sys.h | `TRX_SHARDS_N` | 常量 256：事务分片数，`trx_get_shard_no` 用 `trx_id % TRX_SHARDS_N` 计算片号 |
| storage/innobase/include/trx0sys.ic | `trx_sys_get_trx_id_write_margin` | 返回 `TRX_SYS_TRX_ID_WRITE_MARGIN`；DBUG 开关 `trx_sys_low_trx_id_write_margin` 下返回 8 |
| storage/innobase/include/trx0sys.ic | `trx_sys_allocate_trx_id_or_no` | `fetch_add(1)` 分配号；`trx_id % 写余量 == 0` 时调 `trx_sys_write_max_trx_id()`；注释说明持锁保证落盘期间号区间连续 |
| storage/innobase/include/trx0sys.ic | `trx_sys_allocate_trx_id` / `trx_sys_allocate_trx_no` | 分别要求持 `trx_sys_mutex` / `trx_sys_serialisation_mutex`，都走 `trx_sys_allocate_trx_id_or_no()` |
| storage/innobase/include/trx0sys.ic | `trx_sys_get_next_trx_id_or_no` | 读取 `trx_sys->next_trx_id_or_no`——`check_trx_id_sanity` 的异常门槛 |
| storage/innobase/include/read0types.h | `ReadView::changes_visible` | 可见性判定的全部逻辑：`id < m_up_limit_id \|\| id == m_creator_trx_id` → 可见；`check_trx_id_sanity`；`id >= m_low_limit_id` → 不可见；否则 `std::binary_search` 查活跃事务表 |
| storage/innobase/include/read0types.h | `ReadView::creator_trx_id` | 设置创建者事务号，`ut_ad(m_creator_trx_id == 0)` 保证只设一次 |
| storage/innobase/include/read0read.h | `MVCC::is_view_active` | 判断视图是否活跃有效：`view != nullptr && !(intptr_t(view) & 0x1)` |
| storage/innobase/include/read0read.h | `MVCC::set_view_creator_trx_id` | 转发到 `ReadView::creator_trx_id`，`ut_ad(id > 0)` |
| storage/innobase/read/read0read.cc | `ReadView::prepare` | 建视图：`m_low_limit_no = trx_get_serialisation_min_trx_no()`；`m_low_limit_id = trx_sys_get_next_trx_id_or_no()`；拷 `rw_trx_ids`；`m_up_limit_id` 取活跃表最小 id；全程在 `trx_sys_mutex` 下 |
| storage/innobase/read/read0read.cc | `MVCC::view_open` | 打开视图；含「autocommit 非加锁事务 + 旧视图为空 + 水位未变」则复用旧视图的捷径 |
| storage/innobase/read/read0read.cc | `MVCC::view_close` | 关闭视图：`own_mutex == true` 走完整路径（close → 移出 `m_views` → 进 `m_free`）；`false` 只在指针最低位打「已关闭」标记 |
| storage/innobase/read/read0read.cc | `MVCC::clone_oldest_view` | 克隆最老的活跃视图供 purge 用；无活跃视图时 `view->prepare(0)`；克隆后 `copy_complete()` 把 creator trx id 插入 `m_ids` 并调整 `m_up_limit_id` |
| storage/innobase/read/read0read.cc | `MVCC::size` | 统计 `m_views` 中 `!is_closed()` 的视图数，用于关停时的漏关检查 |
| storage/innobase/read/read0read.cc | `ReadView::copy_trx_ids` | 从 `trx_sys->rw_trx_ids` 拷贝活跃事务号并排除 creator trx id；`m_up_limit_id = m_ids.front()` |

## 与主核 / AI 的关系

主核（`engine/db/`）的事务层与 InnoDB 的读视图没有代码级关联，但**两者的可见性判定模型在结构上高度同构**，可作对照：

- **都把「可见性判定」与「版本存储」分离。** InnoDB 里读视图只回答「这个版本在这个时间点可见吗」（`changes_visible`），版本本身存在 undo 里（`row_vers_build_for_consistent_read` 负责回溯）。主核的 WAL 与事务层同样需要把「读到一个版本」和「这个版本能不能给本次读看」分成两件事。
- **都用「单调序号 + 水位」而不是「状态位」表达快照。** InnoDB 用事务号 `next_trx_id_or_no` 加 `m_low_limit_id` / `m_up_limit_id`；主核侧的 LSN 同样单调递增，可以直接充当水位。
- **都把「长事务拖住回收」作为必须暴露的行为。** InnoDB 里长事务不提交 → 视图不关 → purge 的 `low_limit_no` 抬不上去 → undo 回收不了。主核若要做 undo / vacuum，同样需要一个「最老的活跃读者」概念。

**架构推论：** 主核侧若要实现快照隔离，最值得直接借鉴的是 InnoDB 的两点——(1) **把快照建成不可变值**（三个数字 + 一个有序数组），判定退化成纯函数 `changes_visible`，不需要在读路径上持有任何事务状态锁；(2) **让回收方复用同一个快照结构**，把「最老的活跃读者」作为回收水位，而不是另建一套记账。本页未检出主核侧的事务层实现，故主核是否已有等价机制、采用何种粒度，均不作断言。

**架构推论：** InnoDB 这套「读视图同时承担读一致性与回收护栏」的双重角色，隐含了一个代价——**回收能力被最慢的读者绑定**。主核若在这一点上做取舍（例如把回收水位与读快照解耦、允许按策略放宽回收），需要显式承认这会削弱「读者看到的东西绝不会被抽走」这一保证，而不是把它当作纯粹的优化。

## 参考

- [storage/innobase/trx/trx0sys.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0sys.cc) —— 本页两个主符号 `ReadView::check_trx_id_sanity` 与 `trx_sys_write_max_trx_id` 的权威来源；同一文件中的 `trx_sys_init_at_db_start` / `trx_sys_create` / `trx_sys_close` / `trx_sysf_create` 亦为水位与视图生命周期的出处。
- [storage/innobase/include/read0types.h（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/include/read0types.h) —— `ReadView` 类的字段定义（`m_low_limit_id` / `m_up_limit_id` / `m_ids` / `m_creator_trx_id`）与 `changes_visible` 的完整判定逻辑。
- [storage/innobase/read/read0read.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/read/read0read.cc) —— `MVCC` 视图管理器：`view_open` / `view_close` / `clone_oldest_view` / `size` 与 `ReadView::prepare` / `copy_trx_ids`；文件头注释里的 FACT A / B / C 给出了游标读视图与 purge 的正确性论证。
- [storage/innobase/include/trx0sys.h（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/include/trx0sys.h) —— `TRX_SYS_TRX_ID_WRITE_MARGIN = 256`、`TRX_SYS_TRX_ID_STORE = 0`、`TRX_SHARDS_N = 256` 等常量的定义处。
- [storage/innobase/trx/trx0trx.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0trx.cc) —— `trx_assign_read_view`、`trx_erase_lists`、`trx_commit_in_memory`、`trx_start_low`、`trx_add_to_serialisation_list` 等视图生命周期与序列化号的实际调用点。
- [Consistent Nonlocking Reads（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-consistent-read.html) —— 一致性读与快照的官方语义：REPEATABLE READ 下同一事务的所有一致性读共享同一快照，READ COMMITTED 下每次一致性读各取新快照。
- [InnoDB Multi-Versioning（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-multi-versioning.html) —— 多版本机制与 `DB_TRX_ID` / `DB_ROLL_PTR` / `DB_ROW_ID` 三个隐藏字段；以及「update undo 只能在没有任何持有快照的事务后才可丢弃」这一读视图与回收之间的因果关系。
