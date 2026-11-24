---
project: mysql-server
feature: buf-flush
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/buf/buf0flu.cc
    symbols: [buf_flush_page_coordinator_thread, buf_flush_page_cleaner_thread]
---

# 脏页刷新

## 一句话职责

`storage/innobase/buf/buf0flu.cc` 里的脏页刷新机制负责把 InnoDB 缓冲池中**已被修改但尚未落盘**的页写回数据文件：由 `buf_flush_page_coordinator_thread()` 作为唯一的协调线程，按「脏页比例」与「redo 生成速率」两个维度算出本轮该刷多少页、按缓冲池实例拆分成 slot 下发请求，再由 `buf_flush_page_cleaner_thread()` 这组工作线程各自领走一个 slot 完成实际写盘；刷脏进度由 `pc_request()` / `pc_flush_slot()` / `pc_wait_finished()` 这一对「请求—执行—回收」的三段协议控制。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责挑选脏页写回磁盘、批量刷新与刷脏进度控制；**不负责页的取用（H01）**、**不负责 redo 日志的写入（I 组）**。

## 机制是什么

### 两条独立的刷新通道

`buf0flu.cc` 里存在两条互相独立的刷新通道，由 `buf_flush_t` 枚举区分：

| 通道 | 枚举值 | 源链表 | 目的 |
|---|---|---|---|
| LRU 刷新 | `BUF_FLUSH_LRU` | `buf_pool->LRU` 尾部 | 淘汰脏页，顺带为 free list 腾位置 |
| flush list 刷新 | `BUF_FLUSH_LIST` | `buf_pool->flush_list` 尾部 | 按 `oldest_modification` 顺序推进 redo 检查点 |

两者在代码里共享同一套「找页 → 判断是否可刷 → 发 IO」的骨架，但**扫描起点和终止条件完全不同**：

- `buf_do_flush_list_batch()` 从 `UT_LIST_GET_LAST(buf_pool->flush_list)` 起步，循环条件是 `count < min_n && bpage != nullptr && len > 0 && bpage->get_oldest_lsn() < lsn_limit`——**LSN 上限是最硬的停止条件**。
- `buf_flush_LRU_list_batch()` 从 `UT_LIST_GET_LAST(buf_pool->LRU)` 起步，循环条件是 `count + evict_count < max && free_len < srv_LRU_scan_depth + withdraw_depth && lru_len > BUF_LRU_MIN_LEN`——**free list 的水位与 LRU 链长度下限是停止条件**。

`buf_flush_batch()` 是二者的统一入口，它用 `switch (flush_type)` 分派：

```
switch (flush_type) {
  case BUF_FLUSH_LRU:
    mutex_enter(&buf_pool->LRU_list_mutex);
    count = buf_do_LRU_batch(buf_pool, min_n);
    mutex_exit(&buf_pool->LRU_list_mutex);
    break;
  case BUF_FLUSH_LIST:
    count = buf_do_flush_list_batch(buf_pool, min_n, lsn_limit);
    break;
  default:
    ut_error;
}
```

### flush list 是「按 LSN 有序」的脏页队列

`buf_flush_insert_into_flush_list()` 把被修改的页插到 `buf_pool->flush_list` 的**头部**（`UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page)`），并把该页的最老修改 LSN 记在 `block->page.set_oldest_lsn(lsn)`。由于插入点在头部、扫描点在尾部（`UT_LIST_GET_LAST`），**最早被修改的页就在列表尾部**，正好是刷脏时优先处理的对象。

顺序并非严格有序。`buf_flush_list_order_validate()` 给出的保证是：

```
return (earlier_added_lsn <=
        new_added_lsn + log_buffer_flush_order_lag(*log_sys));
```

即「较早插入的页的 `oldest_modification`，最多只能比后插入页的 `oldest_modification` 大出 `log_buffer_flush_order_lag()` 这个量」。这是**松弛有序**，用断言 `buf_flush_validate_low()` 在调试构建下校验：

```
ut_a(bpage == nullptr ||
     buf_flush_list_order_validate(bpage->get_oldest_lsn(), om));
```

### 无 redo 的页如何入队

`buf_flush_insert_into_flush_list()` 有一条专门处理「无 redo 变脏」的分支：当传入的 `lsn == 0` 时，页从 `buf_flush_borrow_lsn()` **借用**一个 LSN：

```
const auto page = UT_LIST_GET_FIRST(buf_pool->flush_list);

if (page == nullptr) {
  /* Flush list is empty - use lsn up to which we know that all
  dirty pages with smaller oldest_modification were added to
  the flush list (they were flushed as the flush list is empty). */
  const lsn_t lsn = log_buffer_dirty_pages_added_up_to_lsn(*log_sys);

  if (lsn < LOG_START_LSN) {
    ut_ad(srv_read_only_mode);
    return LOG_START_LSN + LOG_BLOCK_HDR_SIZE;
  }
  return lsn;
}
...
return page->get_oldest_lsn();
```

逻辑是：flush list 非空就借队首（最老）的 `oldest_lsn`；flush list 为空就借 `log_buffer_dirty_pages_added_up_to_lsn()`（即「已知所有更小 LSN 的脏页都已入队」的界）。借到之后再取一次 `std::max(lsn, log_sys->flushed_to_disk_lsn.load())` 写入 `set_newest_lsn()`，防止用更小的 LSN 二次刷盘。

### page_cleaner：协调者 + 工作线程

刷新线程的组织形式是「**一个协调者 + 若干工作线程**」，由 `page_cleaner_t` 结构描述：

- `page_cleaner->n_slots` 等于 `srv_buf_pool_instances`（见 `buf_flush_page_cleaner_init()`：`page_cleaner->n_slots = static_cast<ulint>(srv_buf_pool_instances);`）——**每个缓冲池实例对应一个 slot**。
- `page_cleaner_slot_t::state` 是一个四态机：`PAGE_CLEANER_STATE_NONE` → `PAGE_CLEANER_STATE_REQUESTED` → `PAGE_CLEANER_STATE_FLUSHING` → `PAGE_CLEANER_STATE_FINISHED`。
- `buf_flush_page_cleaner_init()` 启动协调者线程，并把它本身登记为 `srv_threads.m_page_cleaner_workers[0]`；`buf_flush_page_coordinator_thread()` 内部再创建编号 1 到 `srv_threads.m_page_cleaner_workers_n - 1` 的工作线程：

```
for (size_t i = 1; i < srv_threads.m_page_cleaner_workers_n; ++i) {
  srv_threads.m_page_cleaner_workers[i] = os_thread_create(
      page_flush_thread_key, i, buf_flush_page_cleaner_thread);

  srv_threads.m_page_cleaner_workers[i].start();
}
```

- `buf_flush_page_cleaner_thread()` 是工作线程主体，循环体极简：等 `page_cleaner->is_requested` 事件 → 检查 `page_cleaner->is_running` → 调 `pc_flush_slot()`。**「领活」这件事完全由 `pc_flush_slot()` 内部的互斥量决定，工作线程之间没有任务队列。**

### 协调者同时扮演两个角色

`buf_flush_page_coordinator_thread()` 除了派活，**自己也干活**：`pc_flush_slot()` 里抢到 slot 的一定是某个线程，协调者调用它同样会抢到 slot 并执行刷新。源码注释写得很直白：

```
/* Coordinator also treats requests */
while (pc_flush_slot() > 0) {
  /* No op */
}
```

## 怎么实现的

前面描述的概念在代码里落地成一条固定的流水线：**先算刷多少页 → 再按实例拆成 slot 下发 → 各 slot 领活执行 → 汇总结果**。对应的实现从「算目标量」开始。

### 刷脏目标量的计算：`Adaptive_flush` 命名空间

`buf0flu.cc` 里有一个 `namespace Adaptive_flush`，负责算出「这一轮该刷多少页」。入口是 `Adaptive_flush::page_recommendation(last_pages_in, is_sync_flush, sync_flush_limit_lsn)`，它依次调用四个函数：

#### `initialize()`：建立本轮迭代的基线

```
lsn_t curr_lsn = log_buffer_dirty_pages_added_up_to_lsn(*log_sys);
const auto curr_time = std::chrono::steady_clock::now();

if (prev_lsn == 0) {
  /* First time initialization for next average computation. */
  prev_lsn = curr_lsn;
  prev_time = curr_time;
  prev_iter_time = curr_time;

  return (true);
}
```

首次调用直接返回 `true`（表示「本轮应跳过」），`page_recommendation()` 随即返回 `0`——**第一次迭代不刷任何页**。

#### `set_average()`：跨多轮平滑速率

它维护两个速率：`page_avg_rate`（页/秒）与 `lsn_avg_rate`（字节/秒）。更新条件是：

```
auto avg_loops = srv_flushing_avg_loops;
...
if (n_iterations < avg_loops &&
    time_elapsed < std::chrono::seconds{avg_loops}) {
  return;
}
```

即**每 `srv_flushing_avg_loops` 轮或每 `srv_flushing_avg_loops` 秒（取先到者）才更新一次平均速率**，注释写明动机是「smooth out transition in workload」。更新公式是「旧值与新值取算术平均再除以 2」：

```
page_avg_rate =
    static_cast<ulint>(((sum_pages / time_elapsed_sec) + page_avg_rate) / 2);
...
lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;
```

#### `set_flush_target_by_lsn()`：按 redo 推进压力定目标

它取两个百分比并取较大者：

```
ulint pct_for_dirty = get_pct_for_dirty();
ulint pct_for_lsn = get_pct_for_lsn(age);
ulint pct_total = std::max(pct_for_dirty, pct_for_lsn);
```

- **`get_pct_for_dirty()`** 依据脏页比例。前提断言是 `ut_a(srv_max_dirty_pages_pct_lwm <= srv_max_buf_pool_modified_pct);`。若 `srv_max_dirty_pages_pct_lwm == 0`（用户没开预刷脏），则脏页比例越过 `srv_max_buf_pool_modified_pct` 后返回 `100`（即「按 `innodb_io_capacity` 的 100% 刷」）；否则在脏页比例达到低水位后返回 `(dirty_pct * 100) / (srv_max_buf_pool_modified_pct + 1)`。
- **`get_pct_for_lsn()`** 依据 redo 的「年龄」`age`。它从 `log_files_capacity_get_limits()` 拿到两个界：`limit_for_free_check`（redo 可用空间界）与 `limit_for_dirty_page_age`。低水位是 `af_lwm = (srv_adaptive_flushing_lwm * limit_for_free_check) / 100`；`age < af_lwm` 直接返回 `0`（不刷）。若 `age < limit_for_dirty_page_age && !srv_adaptive_flushing`（用户关掉了自适应刷新且还没到上限）也返回 `0`。否则：

```
lsn_age_factor = (age * 100.0) / limit_for_dirty_page_age;

ut_ad(srv_max_io_capacity >= srv_io_capacity);

return (static_cast<ulint>(((srv_max_io_capacity / srv_io_capacity) *
                            (lsn_age_factor * sqrt(lsn_age_factor))) /
                           7.5));
```

**`7.5` 这个除数是源码里的硬编码常量**，出现在 `get_pct_for_lsn()` 的返回表达式中，用来把 `lsn_age_factor` 的三次方根形状（`factor * sqrt(factor)`）归一化到 0~100 的量纲。

`set_flush_target_by_lsn()` 接着算出目标 LSN 并据此统计每个实例该刷多少页：

```
if (sync_flush) {
  target_lsn = sync_flush_limit_lsn;
  ut_a(target_lsn < LSN_MAX);
  scan_factor = 1;
  buf_flush_sync_lsn = target_lsn;
} else {
  target_lsn = oldest_lsn + lsn_avg_rate * buf_flush_lsn_scan_factor;
  scan_factor = buf_flush_lsn_scan_factor;
  buf_flush_sync_lsn = 0;
}
```

**`buf_flush_lsn_scan_factor` 是源码里的常量：**

```
/** Factor for scan length to determine n_pages for intended oldest LSN
progress */
static uint buf_flush_lsn_scan_factor = 3;
```

即**非同步刷新时，目标 LSN 领先当前最老脏页 LSN 的是「平均 redo 生成速率 × 3」**。扫描每个实例的 flush list 尾部、统计 `oldest_lsn <= target_lsn` 的页数时，单个实例的扫描上限是：

```
const ulint sum_pages_max = srv_max_io_capacity * 2;

const ulint pages_for_lsn_max =
    (sum_pages_max / srv_buf_pool_instances) * scan_factor * 2;
```

**`2` 这个系数出现在两处**：`sum_pages_max = srv_max_io_capacity * 2`（总量上限是最大 IO 能力的两倍）和 `pages_for_lsn_max` 里的 `* 2`（单实例扫描上限再乘两倍 scan_factor）。

普通（非同步）模式下最终页数：

```
n_pages = (PCT_IO(pct_total) + page_avg_rate + pages_for_lsn) / 3;
if (n_pages > srv_max_io_capacity) {
  n_pages = srv_max_io_capacity;
}
```

**`/ 3` 是把三个来源（脏页百分比折算、历史平均刷页速率、LSN 推进折算）取平均**，且结果被 `srv_max_io_capacity` 截顶。

同步刷新模式下有**下限保护**：

```
if (n_pages < srv_io_capacity) {
  n_pages = srv_io_capacity;
}
```

注释解释了原因：「if the target is evaluated to zero the flush could be stuck in sync flush mode indefinitely, flushing nothing.」

#### `set_flush_target_by_page()`：按脏页增速兜底

它**只在 redo 日志被关闭时才生效**：

```
/* Enable page based target only when redo logging is disabled. */
if (mtr_t::s_logging.is_enabled()) {
  MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_DIRTY_PAGE, 0);
  return (n_pages_lsn);
}
```

其核心是把「上一轮的刷页速率」加上「脏页数的变化速率」作为下一轮估计，并用脏页比例开方做放大：

```
auto estimate = prev_page_rate_sec + dirty_page_change_sec;
...
double boost_factor = sqrt(static_cast<double>(cur_iter_dirty_pct) / 90.0);

n_pages = static_cast<ulint>(boost_factor * n_pages);

n_pages = (page_avg_rate + n_pages) / 2;
```

**`90.0` 是源码里的除数**，注释写明：「The boost factor monotonically increases from 0.10(1%) - 1.05 (100%) with a value 1 at 90%.」——即 `boost_factor` 在脏页比例 90% 时恰为 1，低于 90% 时小于 1（抑制），高于 90% 时大于 1（放大）。

### 三段式 slot 协议

这是本机制最核心的控制结构，三个函数构成「请求 → 执行 → 回收」：

#### `pc_request(min_n, lsn_limit)`：下发请求

```
if (min_n != ULINT_MAX) {
  /* Ensure that flushing is spread evenly amongst the
  buffer pool instances. ... */
  min_n = ut::div_ceil(min_n, ulint{srv_buf_pool_instances});
}

mutex_enter(&page_cleaner->mutex);

page_cleaner->requested = (min_n > 0);
page_cleaner->lsn_limit = lsn_limit;

for (ulint i = 0; i < page_cleaner->n_slots; i++) {
  page_cleaner_slot_t *slot = &page_cleaner->slots[i];

  if (min_n == ULINT_MAX) {
    slot->n_pages_requested = ULINT_MAX;
  } else if (min_n == 0) {
    slot->n_pages_requested = 0;
  }

  /* slot->n_pages_requested was already set by
  Adaptive_flush::page_recommendation() */

  slot->state = PAGE_CLEANER_STATE_REQUESTED;
}

page_cleaner->n_slots_requested = page_cleaner->n_slots;
...
os_event_set(page_cleaner->is_requested);
```

要点：**页数被均摊到各实例**（`ut::div_ceil(min_n, srv_buf_pool_instances)`）；**`min_n` 不是 `ULINT_MAX` 时不改写 `slot->n_pages_requested`**（因为它已由 `Adaptive_flush::page_recommendation()` 按实例算好了）；`min_n == ULINT_MAX` 表示「不限页数，刷到 LSN 上限为止」。

#### `pc_flush_slot()`：领活并执行

它在 `page_cleaner->mutex` 下找一个 `PAGE_CLEANER_STATE_REQUESTED` 的 slot，把状态改成 `PAGE_CLEANER_STATE_FLUSHING`，**然后释放互斥量**再干活：

```
mutex_exit(&page_cleaner->mutex);

const auto lru_start = std::chrono::steady_clock::now();

/* Flush pages from end of LRU if required */
slot->n_flushed_lru = buf_flush_LRU_list(buf_pool);

lru_time = std::chrono::steady_clock::now() - lru_start;
lru_pass = 1;

if (!page_cleaner->is_running) {
  slot->n_flushed_list = 0;
} else {
  if (page_cleaner->requested) {
    slot->succeeded_list = buf_flush_do_batch(
        buf_pool, BUF_FLUSH_LIST, slot->n_pages_requested,
        page_cleaner->lsn_limit, &slot->n_flushed_list);
    ...
  } else {
    slot->n_flushed_list = 0;
    slot->succeeded_list = true;
  }
}
```

**每个 slot 的刷新动作固定是两步：先 `buf_flush_LRU_list()`，再 `buf_flush_do_batch(BUF_FLUSH_LIST, ...)`。** 注意 `page_cleaner->requested` 为假时（`min_n == 0`）**只刷 LRU、不刷 flush list**，`succeeded_list` 记为 `true`。

返回值是「尚未处理的 slot 数」：

```
ulint ret = page_cleaner->n_slots_requested;

mutex_exit(&page_cleaner->mutex);

return (ret);
```

调用方一律写成 `while (pc_flush_slot() > 0) { }`——**反复调用直到没有待处理 slot 为止**。

#### `pc_wait_finished()`：回收结果

```
os_event_wait(page_cleaner->is_finished);

mutex_enter(&page_cleaner->mutex);

for (ulint i = 0; i < page_cleaner->n_slots; i++) {
  page_cleaner_slot_t *slot = &page_cleaner->slots[i];

  *n_flushed_lru += slot->n_flushed_lru;
  *n_flushed_list += slot->n_flushed_list;
  all_succeeded &= slot->succeeded_list;

  slot->state = PAGE_CLEANER_STATE_NONE;

  slot->n_pages_requested = 0;
}
...
os_event_set(buf_flush_tick_event);
```

它把各 slot 的刷新数累加出来，并把 slot 状态复位成 `NONE`。**返回值的语义是「本轮所有 flush_list 刷新批次是否都成功」**（`all_succeeded &= slot->succeeded_list`），失败信息通过 `slot->succeeded_list` 逐 slot 汇总。

### 「批次」互斥：`buf_flush_start()` / `buf_flush_end()`

同一个缓冲池实例**同时只能有一个同类型的刷新批次**在跑。`buf_flush_start()` 做了这件事：

```
mutex_enter(&buf_pool->flush_state_mutex);

if (buf_pool->n_flush[flush_type] > 0 ||
    buf_pool->init_flush[flush_type] == true) {
  /* There is already a flush batch of the same type running */

  mutex_exit(&buf_pool->flush_state_mutex);

  return false;
}

buf_pool->init_flush[flush_type] = true;

os_event_reset(buf_pool->no_flush[flush_type]);
```

**返回 `false` 就是「已有同类批次在跑，本实例跳过」**。`buf_flush_do_batch()` 据此决定成败：

```
if (!buf_flush_start(buf_pool, type)) {
  return (false);
}

ulint page_count = buf_flush_batch(buf_pool, type, min_n, lsn_limit);

buf_flush_end(buf_pool, type);
```

`buf_flush_end()` 除了清 `init_flush` 标记，还负责把数据真正 fsync 出去——**这是 dblwr 路径与非 dblwr 路径的分叉点**：

```
if (!srv_read_only_mode) {
  if (dblwr::is_enabled()) {
    dblwr::force_flush(flush_type, buf_pool_index(buf_pool));
  } else {
    buf_flush_sync_datafiles();
  }
} else {
  os_aio_simulated_wake_handler_threads();
}
```

而 `buf_flush_sync_datafiles()` 走的是「先等 IO 提交完，再 fsync 文件」：

```
os_aio_simulated_wake_handler_threads();
os_aio_wait_until_no_pending_writes();
fil_flush_file_spaces();
```

### `buf_flush_lists()`：跨实例的顺序刷新

`buf_flush_lists(min_n, lsn_limit, n_processed)` 是**从调用者线程**（不是 page_cleaner）发起的刷新，它按实例顺序逐个刷新，并处理「某实例已有批次在跑」的情况：

```
for (ulint i = 0; i < srv_buf_pool_instances; i++) {
  buf_pool_t *buf_pool;
  ulint page_count = 0;

  buf_pool = buf_pool_from_array(i);

  if (!buf_flush_do_batch(buf_pool, BUF_FLUSH_LIST, min_n, lsn_limit,
                          &page_count)) {
    /* We have two choices here. If lsn_limit was
    specified then skipping an instance of buffer
    pool means we cannot guarantee that all pages
    up to lsn_limit has been flushed. ... */
    success = false;

    continue;
  }

  n_flushed += page_count;
}
```

**关键降级路径：某个实例刷新失败（`buf_flush_do_batch()` 返回 `false`）时，`buf_flush_lists()` 并不中止，而是 `success = false` 后 `continue` 继续刷下一个实例**，最后返回 `false` 给调用方。注释明确说明了这一取舍：跳过实例意味着无法保证所有页都刷到 `lsn_limit`，但仍尝试刷其他实例「以帮助随后的重试」。

`buf_flush_sync_all_buf_pools()` 是最激进的调用者，它循环到成功为止：

```
do {
  n_pages = 0;
  success = buf_flush_lists(ULINT_MAX, LSN_MAX, &n_pages);
  buf_flush_wait_batch_end(nullptr, BUF_FLUSH_LIST);

  if (!success) {
    MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
  }
  ...
} while (!success);

ut_a(success);

/* All pages have been written to disk, but we need to make fsync for files
to which the writes have been made. */
buf_flush_fsync();
```

**`buf_flush_wait_batch_end(nullptr, BUF_FLUSH_LIST)` 等的是 `buf_pool->no_flush[flush_type]` 事件**——这个事件在 `buf_flush_write_complete()` 里当最后一个 IO 完成时置位：

```
buf_pool->n_flush[flush_type]--;

if (buf_pool->n_flush[flush_type] == 0 &&
    buf_pool->init_flush[flush_type] == false) {
  /* The running flush batch has ended */

  os_event_set(buf_pool->no_flush[flush_type]);
}
```

### 单页写盘的真正执行者：`buf_flush_page()`

前面所有机制最终都落到 `buf_flush_page(buf_pool, bpage, flush_type, sync)`。它做四件事：

**（1）决定要不要刷。** 对未压缩页，需要 `buf_fix_count == 0` 或 `flush_type == BUF_FLUSH_LIST`；临时表空间还有额外限制：

```
} else if (!(no_fix_count || flush_type == BUF_FLUSH_LIST) ||
           (!no_fix_count &&
            srv_shutdown_state.load() < SRV_SHUTDOWN_FLUSH_PHASE &&
            fsp_is_system_temporary(bpage->id.space()))) {
  /* This is a heuristic, to avoid expensive SX attempts. */
  flush = false;
}
```

**（2）登记 IO 状态。**

```
buf_page_set_io_fix(bpage, BUF_IO_WRITE);
buf_page_set_flush_type(bpage, flush_type);

if (buf_pool->n_flush[flush_type] == 0) {
  os_event_reset(buf_pool->no_flush[flush_type]);
}

++buf_pool->n_flush[flush_type];

if (bpage->get_oldest_lsn() > buf_pool->max_lsn_io) {
  buf_pool->max_lsn_io = bpage->get_oldest_lsn();
}
```

**（3）取排他锁（flush list 路径是延迟取的）。**

```
if (flush_type == BUF_FLUSH_LIST && is_uncompressed &&
    !rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE, UT_LOCATION_HERE)) {
  if (!fsp_is_system_temporary(bpage->id.space()) && dblwr::is_enabled()) {
    dblwr::force_flush(flush_type, buf_pool_index(buf_pool));
  } else {
    buf_flush_sync_datafiles();
  }

  rw_lock_sx_lock_gen(rw_lock, BUF_IO_WRITE, UT_LOCATION_HERE);
}
```

**这条分支是 flush list 路径特有的「拿不到锁就先把已有数据刷出去」的降级路径**：先 `dblwr::force_flush()`（dblwr 启用时）或 `buf_flush_sync_datafiles()`（未启用时），再用阻塞版 `rw_lock_sx_lock_gen()` 拿到锁。

**（4）真正写盘。** 交给 `buf_flush_write_block_low()`，它内部调用 `dblwr::write()`：

```
dberr_t err = dblwr::write(flush_type, bpage, sync);

ut_a(err == DB_SUCCESS || err == DB_TABLESPACE_DELETED);
```

写之前还有一道 redo 屏障：

```
const lsn_t flush_to_lsn = bpage->get_newest_lsn();

if (log_sys->flushed_to_disk_lsn.load() < flush_to_lsn) {
  Wait_stats wait_stats;

  wait_stats = log_write_up_to(*log_sys, flush_to_lsn, true);

  MONITOR_INC_WAIT_STATS_EX(MONITOR_ON_LOG_, _PAGE_WRITTEN, wait_stats);
}
```

**「先把 redo 刷到该页的最新 LSN，再写数据页」这个 WAL 顺序保证，就在 `buf_flush_write_block_low()` 里。**

### dblwr 双写路径

`dblwr::write()` 决定了写盘是走双写缓冲还是直接写数据文件：

```
if (srv_read_only_mode || fsp_is_system_temporary(space_id) ||
    !dblwr::is_enabled() || Double_write::s_instances == nullptr ||
    mtr_t::s_logging.dblwr_disabled()) {
  /* Skip the double-write buffer since it is not needed. Temporary
  tablespaces are never recovered, therefore we don't care about
  torn writes. */
  bpage->set_dblwr_batch_id(std::numeric_limits<uint16_t>::max());
  err = Double_write::write_to_datafile(bpage, sync, nullptr);
  ...
} else {
  file::Block *e_block = dblwr::get_encrypted_frame(bpage);

  if (!sync && flush_type != BUF_FLUSH_SINGLE_PAGE) {
    MONITOR_INC(MONITOR_DBLWR_ASYNC_REQUESTS);
    ...
    Double_write::submit(flush_type, bpage, e_block);
    err = DB_SUCCESS;
    ...
  } else {
    MONITOR_INC(MONITOR_DBLWR_SYNC_REQUESTS);
    bpage->set_dblwr_batch_id(std::numeric_limits<uint16_t>::max());
    err = Double_write::sync_page_flush(bpage, e_block);
  }
}
```

**降级条件明确列出五项**：只读模式、系统临时表空间、dblwr 未启用、`Double_write::s_instances` 为空、redo 被关闭（`mtr_t::s_logging.dblwr_disabled()`）。命中任一项即**跳过双写缓冲直接写数据文件**，注释给出的理由是「临时表空间不会被恢复，因此不需要关心撕裂写」。异步路径走 `Double_write::submit()` 批量提交，同步路径走 `Double_write::sync_page_flush()`。

### 相邻页合并写：`buf_flush_try_neighbors()`

`buf_flush_page_and_try_neighbors()` 在刷一个页时，会顺带刷它周围的邻页。邻域大小是：

```
buf_flush_area = std::min(buf_pool->read_ahead_area,
                          static_cast<page_no_t>(buf_pool->curr_size / 16));

low = (page_id.page_no() / buf_flush_area) * buf_flush_area;
high = (page_id.page_no() / buf_flush_area + 1) * buf_flush_area;
```

**`/ 16` 是源码里的除数**——邻域最大不超过当前缓冲池实例容量的十六分之一。另有前置门槛：

```
if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN ||
    srv_flush_neighbors == 0) {
  /* If there is little space or neighbor flushing is
  not enabled then just flush the victim. */
  low = page_id.page_no();
  high = page_id.page_no() + 1;
}
```

以及 `srv_flush_neighbors == 1` 时的「只刷连续脏页」收紧逻辑。

**`BUF_LRU_MIN_LEN = 256` 是 `buf0flu.cc` 里的编译期常量：**

```
/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
constexpr uint32_t BUF_LRU_MIN_LEN = 256;
```

**`BUF_LRU_MIN_LEN` 是源码里唯一的具体数值常量**（256），它出现在 `buf_flush_LRU_list_batch()` 的循环条件 `lru_len > BUF_LRU_MIN_LEN` 中，含义是「LRU 链短于 256 页时停止 LRU 刷新，避免抖动」。

### 协调者主循环的节流

`buf_flush_page_coordinator_thread()` 的主循环靠 `pc_sleep_if_needed()` 节流：

```
if (next_loop_time > cur_time) {
  auto sleep_time = next_loop_time - cur_time;
  if (sleep_time > std::chrono::seconds{1}) {
    sleep_time = std::chrono::seconds{1};
  }
  ...
  return (os_event_wait_time_low(
      buf_flush_event,
      std::chrono::duration_cast<std::chrono::microseconds>(sleep_time),
      sig_count));
}

return (OS_SYNC_TIME_EXCEEDED);
```

**单次睡眠上限是 `std::chrono::seconds{1}`（1 秒）**，超时返回 `OS_SYNC_TIME_EXCEEDED`。主循环把「超了一秒」当作「服务器空闲」的信号：

```
if ((is_server_active || buf_get_n_pending_read_ios() || n_flushed == 0) &&
    !is_sync_flush) {
  ret_sleep = pc_sleep_if_needed(loop_start_time + std::chrono::seconds{1}, sig_count);
  ...
} else if (std::chrono::steady_clock::now() >
           loop_start_time + std::chrono::seconds{1}) {
  ret_sleep = OS_SYNC_TIME_EXCEEDED;
}
```

「服务器是否活跃」由 `srv_check_activity(last_activity)` 与 `srv_get_activity_count()` 判定，另外**缓冲池缩容（withdraw）过程也算活跃**：

```
const bool is_server_active = is_withdrawing || was_server_active ||
                              srv_check_activity(last_activity);
```

空闲且刷过一轮之后，走 `srv_idle_flush_pct` 这条后台刷新路径：

```
} else if (ret_sleep == OS_SYNC_TIME_EXCEEDED && srv_idle_flush_pct) {
  /* no activity, slept enough */
  buf_flush_lists(PCT_IO(srv_idle_flush_pct), LSN_MAX, &n_flushed);
```

**注意这条路径调用的是 `buf_flush_lists()` 而不是 `pc_request()`**——它在协调者线程里**同步地**把每个实例的 flush list 刷一遍。

### 同步刷新（sync flush）模式

当 `srv_flush_sync` 打开且非只读时，协调者会走同步刷新：

```
if (srv_flush_sync && !srv_read_only_mode) {
  /* lsn_limit!=0 means there are requests. needs to check the lsn. */
  lsn_limit = log_sync_flush_lsn(*log_sys);
  if (lsn_limit != 0) {
    /* Avoid aggressive sync flush beyond limit when redo is disabled. */
    if (mtr_t::s_logging.is_enabled()) {
      lsn_limit += Adaptive_flush::lsn_avg_rate * buf_flush_lsn_scan_factor;
    }
    is_sync_flush = true;
  } else {
    /* Stop the sync flush. */
    is_sync_flush = false;
  }
}
```

**同步刷新的目标 LSN 在 `log_sync_flush_lsn()` 给出的基础上，再加上「平均 redo 速率 × `buf_flush_lsn_scan_factor`（3）」作为缓冲**——但**仅在 redo 仍启用时才加**（`mtr_t::s_logging.is_enabled()`），注释说明是「Avoid aggressive sync flush beyond limit when redo is disabled.」。

同步模式下 `set_flush_target_by_lsn()` 用 `scan_factor = 1`（而非 3），且注释说明其意图：

```
/* Flush n_to_flush pages or stop if you reach lsn_limit earlier.
This is because in sync-flush mode we want finer granularity of
flushes through all BP instances. */
```

### 关闭过程：三轮递减收敛

`buf_flush_page_coordinator_thread()` 在退出主循环后有一段专门的三阶段关闭逻辑：

**第一阶段**（`SRV_SHUTDOWN_CLEANUP` 之后到 `SRV_SHUTDOWN_FLUSH_PHASE` 之前）持续全量刷新：

```
do {
  pc_request(ULINT_MAX, LSN_MAX);

  while (pc_flush_slot() > 0) {
  }
  ...
  /* We sleep only if there are no pages to flush */
  if (n_flushed == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
} while (srv_shutdown_state.load() < SRV_SHUTDOWN_FLUSH_PHASE);
```

**空刷时的休眠是 `std::chrono::milliseconds(100)`（100 毫秒）**。

**第二阶段**做最终清场，循环条件把三个条件合在一起：

```
do {
  are_any_read_ios_still_underway = buf_get_n_pending_read_ios() > 0;
  pc_request(ULINT_MAX, LSN_MAX);
  ...
  success = pc_wait_finished(&n_flushed_lru, &n_flushed_list);
  ...
  buf_flush_wait_batch_end(nullptr, BUF_FLUSH_LIST);
  buf_flush_wait_LRU_batch_end();

} while (!success || n_flushed > 0 || are_any_read_ios_still_underway);
```

**退出条件要求三者同时满足**：所有批次成功、这一轮确实刷到了页（`n_flushed > 0` 为假即本轮没刷到东西）、且没有待完成的读 IO。最后用断言确认缓冲池已清空：

```
for (ulint i = 0; i < srv_buf_pool_instances; i++) {
  buf_pool_t *buf_pool = buf_pool_from_array(i);
  ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == 0);
}
```

**极快关闭（`srv_fast_shutdown == 2`）时直接跳过整个刷脏过程**：

```
if (srv_fast_shutdown == 2 ||
    srv_shutdown_state.load() == SRV_SHUTDOWN_EXIT_THREADS) {
  /* In very fast shutdown or when innodb failed to start, we
  simulate a crash of the buffer pool. We are not required to do
  any flushing. */
  goto thread_exit;
}
```

### 空闲时的单页 LRU 刷新：降级路径

当用户线程找不到 free block 时，`buf0lru.cc` 里会退到单页刷新。**实际调用 `buf_flush_single_page_from_LRU()` 的是 `buf0lru.cc` 里的 `buf_LRU_search_and_free_block()`**：

```
/* No free block was found: try to flush the LRU list.
This call will flush one page from the LRU and put it on
the free list. That means that the free block is up for grabs for
all user threads. */

if (!buf_flush_single_page_from_LRU(buf_pool)) {
  MONITOR_INC(MONITOR_LRU_SINGLE_FLUSH_FAILURE_COUNT);
  ++flush_failures;
}
```

`buf_flush_single_page_from_LRU()` 扫描 LRU 尾部，对每个页：若是 stale 页则 `buf_page_free_stale()`；若 `buf_flush_ready_for_replace()` 为真则 `buf_LRU_free_page()`；否则若 `buf_flush_ready_for_flush(bpage, BUF_FLUSH_SINGLE_PAGE)` 为真则调 `buf_flush_page(buf_pool, bpage, BUF_FLUSH_SINGLE_PAGE, true)`。**它是 `BUF_FLUSH_SINGLE_PAGE` 类型的唯一发起者，且是同步刷（`sync = true`）。**

### 刷新观察者：`Flush_observer`

`buf0flu.cc` 里的 `Flush_observer` 提供「等某个表空间的脏页刷干净」的回调机制。它统计每个实例的已刷页数与已移除页数：

```
void Flush_observer::notify_flush(buf_pool_t *buf_pool, buf_page_t *) {
  m_flushed.at(buf_pool->instance_no).fetch_add(1, std::memory_order_relaxed);

  if (m_stage != nullptr) {
    m_stage->inc(1);
  }
}
```

`Flush_observer::flush()` 的行为分两种：

```
if (m_interrupted) {
  buf_remove = BUF_REMOVE_FLUSH_NO_WRITE;
} else {
  buf_remove = BUF_REMOVE_FLUSH_WRITE;
  ...
}

/* Flush or remove dirty pages. */
buf_LRU_flush_or_remove_pages(m_space_id, buf_remove, m_trx);

/* Wait for all dirty pages were flushed. */
for (ulint i = 0; i < srv_buf_pool_instances; i++) {
  while (!is_complete(i)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
```

**中断时走 `BUF_REMOVE_FLUSH_NO_WRITE`（只移除不写盘），否则走 `BUF_REMOVE_FLUSH_WRITE`（写了再移除）**——这是明确的降级分支。等待每实例完成时的轮询间隔是 `std::chrono::milliseconds(2)`（2 毫秒）。

## 关键取舍

### 取舍一：协调者 + 工作线程，而不是「一个线程刷所有实例」

`pc_request()` 把页数按 `ut::div_ceil(min_n, srv_buf_pool_instances)` 均摊到各 slot，各 slot 各自领活并行执行。代价是**协调者自己也要参与干活**（源码注释「Coordinator also treats requests」），执行路径变得不够纯粹。收益是**刷新吞吐随缓冲池实例数近似线性增长**——这也是多实例缓冲池（H04）的主要动机之一。

### 取舍二：slot 状态机四态，而不是简单的任务队列

`NONE → REQUESTED → FLUSHING → FINISHED` 四态机把「已下发」「正在刷」「已完成」三种事实显式记在 slot 上，好处是 `pc_wait_finished()` 可以在回收时直接断言一致性（`ut_ad(page_cleaner->n_slots_finished == page_cleaner->n_slots);`），坏处是**状态转移全部在 `page_cleaner->mutex` 下完成，成为刷脏路径上的一处全局串行点**。

### 取舍三：flush list 目标 LSN 领先 3 倍速率，而不是「刚好追平」

`buf_flush_lsn_scan_factor = 3` 让目标 LSN 领先当前最老脏页 LSN 「平均 redo 速率 × 3」。这意味着**刷脏总是故意跑在 redo 前面一段距离**，留出缓冲以吸收速率抖动。代价是**平均多刷了约 3 倍速率对应的页**，IO 带宽被提前占用；收益是**不会因为速率估计的滞后而让检查点被 redo 空间卡住**。

### 取舍四：自适应刷脏把三个来源取平均，而不是取最大值

`n_pages = (PCT_IO(pct_total) + page_avg_rate + pages_for_lsn) / 3;` 里，`pct_total` 本身已经是「脏页比例」与「LSN 压力」两个百分比的最大值，但最终页数是**三项的平均**而非最大值。取最大值会让刷脏量跳变，取平均更平滑——这与 `set_average()` 里「每 `srv_flushing_avg_loops` 轮才更新一次速率」的平滑意图一致。

### 取舍五：松弛有序的 flush list，换取插入 O(1)

`buf_flush_insert_into_flush_list()` 用 `UT_LIST_ADD_FIRST` 在**头部**插入，O(1)；代价是链表**不严格按 LSN 排序**，只能靠 `buf_flush_list_order_validate()` 的松弛界（`log_buffer_flush_order_lag()`）来保证「不会差太多」。严格排序需要在头部按 LSN 插入，代价是 O(n)。恢复期因为 LSN 顺序不确定，另走 `buf_flush_insert_sorted_into_flush_list()`（用红黑树 `buf_pool->flush_rbt` 维护顺序）。

### 取舍六：拿不到排他锁时先刷已有数据，而不是干等

`buf_flush_page()` 里 flush list 路径的降级是：先 `dblwr::force_flush()` / `buf_flush_sync_datafiles()` 把已经写出去的 IO 落盘，再用阻塞版 `rw_lock_sx_lock_gen()` 拿锁。这**把「等锁」的时间用来推进 I/O**，而不是纯粹空等。代价是多一次强制刷盘调用。

### 取舍七：某实例刷新失败时继续刷其他实例，而不是立即返回

`buf_flush_lists()` 遇到 `buf_flush_do_batch()` 返回 `false`（该实例已有批次在跑）时置 `success = false` 并 `continue`。源码注释解释了理由：跳过实例意味着**无法保证所有页都刷到 `lsn_limit`**，但仍尝试刷其他实例「以帮助随后的重试」。真正的严格保证由 `buf_flush_sync_all_buf_pools()` 的 `do { ... } while (!success);` 循环补上。

### 取舍八：极快关闭直接放弃刷脏

`srv_fast_shutdown == 2` 时 `goto thread_exit` 跳过全部刷脏，注释写明「we simulate a crash of the buffer pool. We are not required to do any flushing.」。这是明确的取舍：**用「不保证数据落盘」换关闭速度**，一致性由 redo 恢复承担。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/buf/buf0flu.cc | `buf_flush_page_coordinator_thread` | 刷新协调线程：创建工作线程、主循环节流、计算刷脏目标、下发 slot 请求、驱动关闭过程的三阶段收敛 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_page_cleaner_thread` | 刷新工作线程：等 `page_cleaner->is_requested` 事件后反复调 `pc_flush_slot()` 领活 |
| storage/innobase/buf/buf0flu.cc | `pc_request` | 下发刷新请求：把目标页数按 `srv_buf_pool_instances` 均摊到各 slot，置状态为 `PAGE_CLEANER_STATE_REQUESTED` |
| storage/innobase/buf/buf0flu.cc | `pc_flush_slot` | 领活并执行：抢一个 `REQUESTED` slot，先刷 LRU 再刷 flush list，累加刷新计数 |
| storage/innobase/buf/buf0flu.cc | `pc_wait_finished` | 回收结果：等所有 slot 完成，累加刷新数与失败标志，把 slot 状态复位为 `NONE` |
| storage/innobase/buf/buf0flu.cc | `pc_sleep_if_needed` | 主循环节流：单次睡眠上限 1 秒，超时返回 `OS_SYNC_TIME_EXCEEDED` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_page_cleaner_init` | 初始化 `page_cleaner_t`：`n_slots = srv_buf_pool_instances`，创建协调者线程并把它登记为 worker[0] |
| storage/innobase/buf/buf0flu.cc | `buf_flush_page_cleaner_close` | 关闭 page_cleaner：等待所有 worker 退出，销毁互斥量与事件 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_batch` | 刷新批次的统一入口，按 `flush_type` 分派到 `buf_do_LRU_batch()` 或 `buf_do_flush_list_batch()` |
| storage/innobase/buf/buf0flu.cc | `buf_do_flush_list_batch` | 从 flush list 尾部按 `oldest_lsn < lsn_limit` 扫描并刷页 |
| storage/innobase/buf/buf0flu.cc | `buf_do_LRU_batch` | LRU 批量刷新：先处理 unzip_LRU，再调 `buf_flush_LRU_list_batch()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_LRU_list_batch` | 从 LRU 尾部扫描：可淘汰则 `buf_LRU_free_page()`，可刷则派发 IO；停止条件受 `BUF_LRU_MIN_LEN` 与 `srv_LRU_scan_depth` 约束 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_LRU_list` | 计算 LRU 扫描深度（`srv_LRU_scan_depth` 与 withdraw 深度的较小值）后发起 `BUF_FLUSH_LRU` 批次 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_start` / `buf_flush_end` | 批次互斥：同类批次已有在跑时 `buf_flush_start()` 返回 `false`；`buf_flush_end()` 负责 dblwr 或 `buf_flush_sync_datafiles()` 的落盘 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_do_batch` | 批次包装：`buf_flush_start()` 失败即返回 `false`，否则跑 `buf_flush_batch()` 再 `buf_flush_end()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_lists` | 跨实例顺序刷新：逐实例调 `buf_flush_do_batch()`，某实例失败时置 `success=false` 并继续刷其余实例 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_sync_all_buf_pools` | 全量同步刷新：`do { buf_flush_lists(ULINT_MAX, LSN_MAX, ...) } while (!success)`，最后 `buf_flush_fsync()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_sync_datafiles` | 等所有异步写提交给 OS 后 `fil_flush_file_spaces()` 落盘 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_fsync` | 按 `srv_unix_file_flush_method` / `srv_win_file_flush_method` 决定是否 `fil_flush_file_spaces()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_page` | 单页写盘的真正执行者：判定可否刷、登记 IO 状态、取排他锁、通知 flush observer、交给 `buf_flush_write_block_low()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_write_block_low` | 写盘前的 redo 屏障（`log_write_up_to()`），随后调 `dblwr::write()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_page_and_try_neighbors` | 刷一页后顺带刷邻页，邻域大小取 `read_ahead_area` 与实例容量 `/16` 的较小值 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_try_neighbors` | 邻域扫描：逐个邻页判可刷并派发 IO，超过 `n_to_flush` 时提前收手 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_ready_for_flush` / `buf_flush_was_ready_for_flush` | 判定页是否可刷：非 dirty 或已被 IO 固定则否；`BUF_FLUSH_LIST` 还要求状态不是 `BUF_BLOCK_REMOVE_HASH` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_insert_into_flush_list` | 把变脏页插入 flush list 头部；对无 redo 的页用 `buf_flush_borrow_lsn()` 借 LSN |
| storage/innobase/buf/buf0flu.cc | `buf_flush_insert_sorted_into_flush_list` | 恢复期按 LSN 严格排序插入（借助 `flush_rbt` 红黑树） |
| storage/innobase/buf/buf0flu.cc | `buf_flush_borrow_lsn` | 无 redo 页的 LSN 借用：flush list 非空借队首 `oldest_lsn`，为空借 `log_buffer_dirty_pages_added_up_to_lsn()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_remove` | 把页从 flush list 摘除、置为 clean、通知 flush observer 的 `notify_remove()` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_write_complete` | 写完成回调：清 IO 固定、`n_flush[flush_type]--`，归零时置 `no_flush` 事件 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_wait_batch_end` / `buf_flush_wait_LRU_batch_end` | 等待指定实例（或全部实例）的某类刷新批次结束 |
| storage/innobase/buf/buf0flu.cc | `buf_flush_single_page_from_LRU` | 单页 LRU 刷新：供 `buf0lru.cc` 的 `buf_LRU_search_and_free_block()` 在找不到 free block 时降级调用 |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::page_recommendation` | 刷脏目标量总入口：依次调 `initialize()`、`set_average()`、`set_flush_target_by_lsn()`、`set_flush_target_by_page()` |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::set_flush_target_by_lsn` | 按 redo 推进压力定目标：算 `pages_for_lsn`，目标 LSN 为 `oldest_lsn + lsn_avg_rate * buf_flush_lsn_scan_factor` |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::get_pct_for_lsn` | redo 压力百分比：含 `srv_adaptive_flushing_lwm` 低水位与 `7.5` 的归一化除数 |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::get_pct_for_dirty` | 脏页比例百分比：低水位为 0 时越过高水位返回 `100`，否则按比例折算 |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::set_flush_target_by_page` | 无 redo 时的兜底目标：用脏页增速与 `sqrt(pct/90.0)` 放大因子估算 |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::set_average` | 跨轮平均速率：每 `srv_flushing_avg_loops` 轮更新 `page_avg_rate` 与 `lsn_avg_rate` |
| storage/innobase/buf/buf0flu.cc | `Adaptive_flush::initialize` | 建立本轮迭代基线；首次调用返回 `true` 表示本轮跳过 |
| storage/innobase/buf/buf0flu.cc | `Flush_observer::flush` | 观察者刷新：中断时用 `BUF_REMOVE_FLUSH_NO_WRITE`，否则 `BUF_REMOVE_FLUSH_WRITE`，逐实例轮询至完成 |
| storage/innobase/buf/buf0flu.cc | `buf_pool_get_dirty_pages_count` | 统计某表空间（或某观察者）在单个实例 flush list 中的脏页数 |
| storage/innobase/buf/buf0dblwr.cc | `dblwr::write` | 写盘落地：命中降级条件则跳过双写直写数据文件，否则异步走 `Double_write::submit()`、同步走 `Double_write::sync_page_flush()` |
| storage/innobase/buf/buf0dblwr.cc | `dblwr::force_flush` | 强制把某类型的待写批次落盘（由 `buf_flush_end()` 与 `buf_flush_page()` 的降级路径调用） |
| storage/innobase/buf/buf0dblwr.cc | `Double_write::write_data_pages` | 批量把缓冲页写到数据文件，逐页处理 `DB_PAGE_IS_STALE` / `DB_TABLESPACE_DELETED` |
| storage/innobase/log/log0write.cc | `log_write_up_to` | 写盘前的 redo 屏障：把 redo 刷到 `bpage->get_newest_lsn()`（由 `buf_flush_write_block_low()` 调用） |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_search_and_free_block` | 找不到 free block 时的降级调用方：调 `buf_flush_single_page_from_LRU()` 单页刷 LRU |

## 与主核 / AI 的关系

**架构推论：** 本页描述的脏页刷新是 InnoDB **存储引擎私有**的持久化机制，完全位于 `storage/innobase/` 之下；Server 层（`sql/` 目录）不感知 flush list、slot 或 dirty ratio 的存在。这与 H 组其余页面一致：H01 取页、H02 刷脏、H03 淘汰、H04 多实例，四者共同构成缓冲池这一「引擎私有」子系统。

**架构推论：** 若主核（`engine/db/`）要实现可崩溃恢复的存储层，本机制里最值得复刻的**不是函数结构，而是三条控制原则**：其一，「redo 先于数据页」的 WAL 顺序保证（`buf_flush_write_block_low()` 中的 `log_write_up_to()` 屏障）；其二，「刷脏目标随 redo 生成速率自适应」（`Adaptive_flush::set_flush_target_by_lsn()` 中 `target_lsn = oldest_lsn + lsn_avg_rate * buf_flush_lsn_scan_factor`）；其三，「同一实例同类型刷新批次互斥」（`buf_flush_start()` 返回 `false` 的语义）。

**架构推论：** 对 AI 检索场景，本机制的特点是「**刷脏进度与决策依据高度依赖运行时状态**」——刷多少页取决于 `lsn_avg_rate`、`page_avg_rate`、`srv_io_capacity`、`srv_max_io_capacity`、`srv_max_dirty_pages_pct_lwm`、`srv_adaptive_flushing_lwm`、`srv_flush_neighbors`、`srv_LRU_scan_depth`、`srv_flushing_avg_loops` 等一组可调参数与两个滑动平均速率。这意味着**任何「为什么 MySQL 此刻刷得慢 / 刷得猛」的问答，都必须先还原这些参数与速率的历史序列**，而不能只从某一时刻的页数快照反推。

**架构推论：** 与之对照，本机制中的**硬性常量极少且都在源码里可枚举**：`BUF_LRU_MIN_LEN = 256`、`buf_flush_lsn_scan_factor = 3`、`get_pct_for_lsn()` 的除数 `7.5`、`set_flush_target_by_page()` 的 `90.0`、`pages_for_lsn_max` 的系数 `2`、`sum_pages_max = srv_max_io_capacity * 2`、邻域上限 `curr_size / 16`。**架构推论：** 这类「少量魔数 + 大量可调参数」的组合，使得刷脏行为既有稳定的骨架（可推断），又有高度可变的强度（不可推断）——AI 在解释具体刷脏行为时应优先引用前者。

## 参考

- [Buffer Pool（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-buffer-pool.html) —— 缓冲池、脏页比例与刷脏行为的官方说明。
- [InnoDB Configuration（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-parameters.html) —— `innodb_io_capacity`、`innodb_io_capacity_max`、`innodb_max_dirty_pages_pct`、`innodb_adaptive_flushing_lwm`、`innodb_flush_neighbors` 等刷脏相关参数的官方定义。
- [InnoDB Doublewrite Buffer（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-doublewrite-buffer.html) —— 双写缓冲的官方说明，对应本页 `dblwr::write()` 的异步批量路径与 `Double_write::sync_page_flush()` 同步路径。
- [InnoDB Checkpoints（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-checkpoints.html) —— 检查点推进与 flush list 刷新的关系背景。
- [storage/innobase/buf/buf0flu.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0flu.cc) —— 本页全部主符号的权威来源。
- [storage/innobase/buf/buf0dblwr.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0dblwr.cc) —— 双写缓冲路径的权威来源。
- [storage/innobase/buf/buf0lru.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0lru.cc) —— 单页 LRU 刷新降级路径的调用方 `buf_LRU_search_and_free_block()` 所在文件。
