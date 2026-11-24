---
project: mysql-server
feature: buf-page-get
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/buf/buf0buf.cc
    symbols: [buf_page_get_gen, buf_pool_get]
---

# 缓冲池取页

## 一句话职责

`storage/innobase/buf/buf0buf.cc` 的 `buf_page_get_gen()` 负责把一个「页号（`page_id_t`）+ 取页模式（`Page_fetch`）」变成一个**已被 buffer-fix 住的 `buf_block_t`**：先在页哈希表里查这一页是否已在缓冲池中，命中就加引用计数并按要求上锁；未命中就发起读盘请求把页读进来，再走同一套「查表 + 引用计数 + 上锁」流程。`buf_pool_get()` 则按页所属的表空间 id 选出这一页应该落在哪个缓冲池实例（`buf_pool_t`），是「取页」这一步的最前置的路由。

## 机制是什么

**它解决的是「我此刻要用的这一页，在内存里吗？能给���一个稳定的指针吗？不能的话把它读进来」。** InnoDB 把缓冲池的「取页」与「放回页」（写回、淘汰）彻底分开：本机制只管把页**取到手并钉住**，不管它之后怎么被刷盘、怎么被淘汰出池。

理解这一层需要先分清三个对象：

- **页哈希表（`buf_pool->page_hash`）**：键是 `page_id_t`（表空间 id + 页号）的哈希值，值是 `buf_page_t *`。这是「页在不在池里」的唯一权威判据。
- **`buf_block_t`（块）**：页框与它的控制块。`buf_page_t` 是控制信息，`buf_block_t` 是「控制信息 + 页框内存」的组合。
- **`buf_fix_count`（引用计数）**：一个线程要长期持有某个页时，把这个计数加一；释放时减一。计数非零的页不会被换出。本机制的核心职责之一就是**把这个计数正确地加上去**。

**取页不是一个动作，而是一串模式。** `buf_page_get_gen()` 的第三个参数是 `Page_fetch` 枚举，不同模式决定了「查不到页时怎么办」：

| 模式 | 语义 | 查不到时的行为 |
|---|---|---|
| `NORMAL` | 常规取页 | 读盘，把页取回来 |
| `SCAN` | 扫描式取页 | 读盘，但不做读前预读 |
| `IF_IN_POOL` | 乐观取页 | **不读盘**，直接返回 `nullptr` |
| `PEEK_IF_IN_POOL` | 窥视取页 | **不读盘**，且不解压压缩页 |
| `IF_IN_POOL_OR_WATCH` | 取页或登记「观察」 | 不读盘，改为在 `buf_pool->watch[]` 上登记一个哨兵项 |
| `POSSIBLY_FREED` | 允许页已被释放 | 读盘（用于取一个可能已被 DROP 掉的页） |
| `NO_LATCH` | 不上加锁 | 与 `rw_latch` 组合使用，要求 `rw_latch == RW_NO_LATCH` |

这组模式是「取页」与「能否触发磁盘 I/O」的接口：**上层可以用 `IF_IN_POOL` 明确表达「这一页如果不在池里，我宁可失败也不要去读盘」**——这正是 change buffer（insert buffer）能生效的前提。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责按页号在缓冲池里找到或读入页面、加引用计数与钉住页面；**不负责页的写回（H02）、不负责 LRU 淘汰与脏页刷新（H03）**。

## 怎么实现的

### 入口 `buf_page_get_gen()`：校验、路由、委派

`buf_page_get_gen()` 的函数体做三件事，然后就把活交给模板类 `Buf_fetch`：

1. **调试期校验**：`UNIV_DEBUG` 下断言 `mtr->is_active()`；断言 `rw_latch` 只能是 `RW_S_LATCH` / `RW_X_LATCH` / `RW_SX_LATCH` / `RW_NO_LATCH` 四者之一；断言 `mode` 落在 `NO_LATCH` / `NORMAL` / `SCAN` / `IF_IN_POOL` / `PEEK_IF_IN_POOL` / `IF_IN_POOL_OR_WATCH` / `POSSIBLY_FREED` 之内，遇到未知值走 `ib::fatal(UT_LOCATION_HERE, ER_IB_ERR_UNKNOWN_PAGE_FETCH_MODE)` 并 `ut_error`。
2. **页大小一致性检查**：`UNIV_DEBUG` 下用 `fil_space_get_page_size(page_id.space(), &found)` 取该表空间的页大小，断言「若空间存在，则请求的 `page_size` 必须与之一致」。
3. **路由到两个不同的取页实现**：

```
if (mode == Page_fetch::NORMAL && !fsp_is_system_temporary(page_id.space())) {
    Buf_fetch_normal fetch(page_id, page_size);
    ...
    return (fetch.single_page());
} else {
    Buf_fetch_other fetch(page_id, page_size);
    ...
    return (fetch.single_page());
}
```

也就是说：**常规模式且不是系统临时表空间，走 `Buf_fetch_normal`；其余一切情况走 `Buf_fetch_other`。** 两个派生类只在 `get()` 这一层有区别，共享 `Buf_fetch` 模板基类的 `lookup()` / `is_on_watch()` / `read_page()` / `check_state()` / `mtr_add_page()` / `zip_page_handler()` 等实现。

注意分工是明确的：`Buf_fetch_normal::get()` 的函数体注释写着「Keep this path as simple as possible」，它只有「查表 → 命中就 fix 并放锁 → 未命中就 `read_page()`」三步；`Buf_fetch_other::get()` 多出的分支全是「不读盘」的语义处理。

### `Buf_fetch` 基类构造：`buf_pool_get()` 决定落哪个池

`Buf_fetch` 的构造函数体里：

```
: m_page_id(page_id),
  m_page_size(page_size),
  m_is_temp_space(fsp_is_system_temporary(page_id.space())),
  m_buf_pool(buf_pool_get(m_page_id)) {}
```

`buf_pool_get(m_page_id)` 就是**「这一页属于哪个缓冲池实例」的路由**。MySQL 8.0 支持把缓冲池切成多个实例（`innodb_buffer_pool_instances`），每个实例有自己的页哈希表、free list 与 LRU 链表。同一个表空间的所有页必须落在同一个实例里，否则哈希表就分不清了——`buf_pool_get()` 保证了这个约束。

`m_is_temp_space` 同时被记下来：系统临时表空间（临时表用的）**不做 redo 日志**，因此它的加锁规则与普通页不同。

### 第一步：`lookup()` —— 在页哈希表里查

`Buf_fetch<T>::lookup()` 的函数体：

```
m_hash_lock = buf_page_hash_lock_get(m_buf_pool, m_page_id);
auto block = m_guess;
rw_lock_s_lock(m_hash_lock, UT_LOCATION_HERE);
m_hash_lock = buf_page_hash_lock_s_confirm(m_hash_lock, m_buf_pool, m_page_id);
```

它先按页号算出这一页该用哪把哈希锁 `m_hash_lock`，以共享模式（`rw_lock_s_lock`）加锁。加锁后立刻用 `buf_page_hash_lock_s_confirm()` **重新确认锁仍然有效**——因为缓冲池可以在线扩缩容（`buf_resize_thread()`），哈希表会被重建、加锁位图会变，加锁后必须确认自己拿的还是「这一页对应的那把锁」。

然后处理 `m_guess`（调用方给的页块猜测）：源码注释说明了这个猜测为什么可能失效——压缩页的控制块可能已被 `buf_relocate()` 释放，缓冲池扩容也可能释放了 `m_guess` 所在的 chunk。所以要三重校验：

```
if (!buf_is_block_in_instance(m_buf_pool, block) ||
    m_page_id != block->page.id ||
    buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
  block = m_guess = nullptr;
}
```

猜测无效就丢弃，回落到真正的哈希查找：

```
block = reinterpret_cast<buf_block_t *>(buf_page_hash_get_low(m_buf_pool, m_page_id));
```

若查到的是「观察哨兵」而不是真页（`buf_pool_watch_is_sentinel()` 为真），`lookup()` 释放哈希锁并返回 `nullptr`——哨兵不是页。

### 第二步：命中后的处理——引用计数与放锁

在 `Buf_fetch_normal::get()` 里，命中后：

```
if (block->page.was_stale()) {
  if (!buf_page_free_stale(m_buf_pool, &block->page, m_hash_lock)) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  continue;   // 重新 lookup
}
buf_block_fix(block);
rw_lock_s_unlock(m_hash_lock);
```

三点：

- **stale 页（被 DROP 掉的表空间留下的页）必须先清掉**。`buf_page_free_stale()` 的函数体：断言页确实是 stale 的、断言持有 `buf_pool->LRU_list_mutex`；取页的 io 状态，若 `io_type == BUF_IO_NONE` 则把脏页从 flush list 摘掉（`buf_flush_remove(bpage)`）并调 `buf_LRU_free_page(bpage, true)` 把它换出；返回是否成功。**若清不掉（页正在 I/O 中），就 `sleep` 100 微秒后重试**。那个 100 微秒来自文件顶部的常量 `constexpr std::chrono::microseconds WAIT_FOR_READ{100};`——源码注释写明理由是「页正在 I/O 中无法释放，等一会儿以免陷入空转消耗 CPU，这不是常发事件」。
- **`buf_block_fix(block)` 就是加引用计数**，这一步是「钉住页面」的核心动作：此后该页不会被换出。
- **释放哈希锁**：注释写着「Now safe to release page_hash S lock」——引用计数已经加上了，不再需要哈希锁保护这块控制块的生命周期。

`Buf_fetch_other::get()` 的命中分支多一个临时表空间的分支：`if (m_is_temp_space) { temp_space_page_handler(block); } else { buf_block_fix(block); }`。`temp_space_page_handler()` 的函数体说明：临时表空间用 `block->mutex` 而不是 `block->lock` 来做用户线程与刷写线程之间的同步，因此它在 `block->mutex` 保护下调 `buf_block_fix(block)`。

### 第三步：未命中——`read_page()` 读盘进来

`Buf_fetch<T>::read_page()` 的函数体：

```
if (buf_read_page(m_page_id, m_page_size)) {
  if (m_mode != Page_fetch::SCAN) {
    buf_read_ahead_random(m_page_id, m_page_size, ibuf_inside(m_mtr));
  }
  m_retries = 0;
} else if (m_retries < BUF_PAGE_READ_MAX_RETRIES) {
  ++m_retries;
  DBUG_EXECUTE_IF("innodb_page_corruption_retries",
                  m_retries = BUF_PAGE_READ_MAX_RETRIES;);
} else {
  ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_74)
      << "Unable to read page " << m_page_id << " into the buffer pool after "
      << BUF_PAGE_READ_MAX_RETRIES << " attempts. ...";
}
```

三个数字都能追到源码常量：

- **重试上限 100 次**：来自文件顶部的 `static const ulint BUF_PAGE_READ_MAX_RETRIES = 100;`，注释是「Number of attempts made to read in a page in the buffer pool」。超过上限就 `ib::fatal` 报 `ER_IB_MSG_74`，错误信息里点名了两种典型原因（表损坏 / 用了本实例不支持的压缩算法），并建议用 `innodb_force_recovery`。
- **调试开关可直接把重试次数拉满**：`DBUG_EXECUTE_IF("innodb_page_corruption_retries", ...)` 会让 `m_retries` 直接等于 `BUF_PAGE_READ_MAX_RETRIES`，从而立刻走进 fatal 分支——这是给测试用的一条调试路径。
- **等待 100 微秒**：`WAIT_FOR_READ{100}` 与 `WAIT_FOR_WRITE{100}` 两个常量都定义在文件顶部。

`buf_read_page()` 定义在 `storage/innobase/buf/buf0rea.cc`，它调 `buf_read_page_low(&err, true, 0, BUF_READ_ANY_PAGE, page_id, page_size, false)`，返回 `count > 0`；若 `err == DB_TABLESPACE_DELETED`，则用 `ER_IB_MSG_141` 报错「trying to read page ... in nonexisting or being-dropped tablespace」。

**读进来之后不是直接返回**：还要做读前预读（read-ahead）。`read_page()` 在 `m_mode != Page_fetch::SCAN` 时调 `buf_read_ahead_random()`（`storage/innobase/buf/buf0rea.cc`）。它的函数体里：若 `!srv_random_read_ahead`（用户关掉了）直接返回 0；若 `srv_startup_is_before_trx_rollback_phase`（回滚阶段）也返回 0 以避免线程死锁；若页是 ibuf 位图页或事务系统头页则返回 0（避免破坏 ibuf 的访问顺序）。否则按 `buf_pool->read_ahead_area` 把页号对齐到随机读区域的两端，`low` 与 `high` 由 `page_no / read_ahead_area * read_ahead_area` 算出。

另一条预读路径在 `single_page()` 末尾：若这是该页的**首次访问**（`access_time` 为零值），且 `m_mode != PEEK_IF_IN_POOL`，则调 `buf_read_ahead_linear()` 做线性预读。

### 第四步：`check_state()` —— 状态分流

`Buf_fetch<T>::check_state()` 按 `buf_block_get_state(block)` 分派：

- **`BUF_BLOCK_FILE_PAGE`**：正常页。额外判一个条件——`m_is_temp_space && block->page.was_io_fixed()`：说明这一页正在被刷写，为避免返回一个正在被写的页，源码调 `buf_block_unfix(block)`、睡 `WAIT_FOR_WRITE`（100 微秒）并返回 `DB_FAIL`，让 `single_page()` 的外层 `for(;;)` 重来一次。
- **`BUF_BLOCK_ZIP_PAGE` / `BUF_BLOCK_ZIP_DIRTY`**：压缩页，转 `zip_page_handler(block)`。
- **`BUF_BLOCK_POOL_WATCH` / `BUF_BLOCK_NOT_USED` / `BUF_BLOCK_READY_FOR_USE` / `BUF_BLOCK_MEMORY` / `BUF_BLOCK_REMOVE_HASH`**：全部 `ut_error`。

`zip_page_handler()` 的函数体值得单列（这是压缩页的解压路径）：`m_mode == PEEK_IF_IN_POOL` 时直接 `buf_block_unfix(fix_block)` 并返回 `DB_NOT_FOUND`——源码注释说明该模式只用于删除自适应哈希索引，压缩页不可能有 AHI，所以不解压。若 `bpage->buf_fix_count > 1 || bpage->was_io_fixed()`，说明这块正被别的线程 buffer-fix 或 I/O-fix，退回 `buf_block_unfix` 并睡 `WAIT_FOR_READ`，返回 `DB_FAIL` 让上层重试。否则：调 `buf_LRU_get_free_block(m_buf_pool)` 取一个空块，依次加 `LRU_list_mutex`、哈希锁 X、`zip_mutex`；把压缩页 `buf_relocate()` 到新块并解压；新块状态置 `BUF_BLOCK_FILE_PAGE`，加入 `unzip_LRU` 链表；最后在**不持任何缓冲池互斥量**的情况下调 `buf_zip_decompress(block, false)` 做真正的解压（源码注释：解压时不持锁，因为校验和在读盘时已做过）。

### 第五步：`single_page()` —— 外层循环与收尾

`Buf_fetch<T>::single_page()` 的函数体结构：

```
Counter::inc(m_buf_pool->stat.m_n_page_gets, m_page_id.page_no());
for (;;) {
  if (static_cast<T *>(this)->get(block) == DB_NOT_FOUND) return (nullptr);
  ...
  switch (check_state(block)) {
    case DB_NOT_FOUND: return (nullptr);
    case DB_FAIL: continue;      // 重启外层循环
    case DB_SUCCESS: break;
  }
  ...
  break;
}
```

- **每次取页都计一次数**：`Counter::inc(m_buf_pool->stat.m_n_page_gets, m_page_id.page_no())`，这是缓冲池命中率统计的来源。
- **乐观模式下的额外检查**：若 `is_optimistic()`（即模式为 `IF_IN_POOL` 或 `PEEK_IF_IN_POOL`），先在 `block->mutex` 下读 `buf_page_get_io_fix(bpage)`；**若状态是 `BUF_IO_READ`（页正在读入），就 `buf_block_unfix(block)` 并返回 `nullptr`**——源码注释写明「页正在读入缓冲池，但我们不能等它读完」。
- `is_optimistic()` 的判定就是 `m_mode == Page_fetch::IF_IN_POOL || m_mode == Page_fetch::PEEK_IF_IN_POOL`。

循环外的收尾动作：

- **首次访问打时间戳**：`access_time == std::chrono::steady_clock::time_point{}` 表示从未被访问过，此时 `buf_page_mutex_enter(block)` 后调 `buf_page_set_accessed(&block->page)`。源码注释称这是「a heuristic and we don't care about ordering issues」。
- **是否把页变「年轻」**：`if (m_mode != Page_fetch::PEEK_IF_IN_POOL && m_mode != Page_fetch::SCAN) { buf_page_make_young_if_needed(&block->page); }`。注释解释：对 `PEEK_IF_IN_POOL` 与 `SCAN` 两种模式，不把页移到 LRU 链表头部，让它能被快速丢弃——**这是缓冲池抗扫描能力的入口点**。
- **等读盘完成**：`buf_wait_for_read(block)`。注释说明必须在这里等，是因为 `IO_READ` 状态是在哈希锁保护下设置的，不是在 `block->mutex` 和 `block->lock` 下。
- **脏标记**：`if (m_dirty_with_no_latch) { block->made_dirty_with_no_latch = m_dirty_with_no_latch; }`。注释解释了为什么不写成 `= false`：同一个块可能被两个 mtr 共享，若第二个 mtr 把脏标记写成 false，就会丢失对已修改块的刷写。
- **把页加进 mtr**：`mtr_add_page(block)`。这是**加锁与 mtr 记账合一**的步骤——`mtr_add_page()` 的函数体按 `m_rw_latch` 分派：`RW_NO_LATCH` → 只 `mtr_memo_push(m_mtr, block, MTR_MEMO_BUF_FIX)`；`RW_S_LATCH` → `rw_lock_s_lock_gen(&block->lock, 0, loc)` 后 `MTR_MEMO_PAGE_S_FIX`；`RW_SX_LATCH` → `rw_lock_sx_lock_gen` 后 `MTR_MEMO_PAGE_SX_FIX`；其余（即 X 锁）→ `rw_lock_x_lock_gen` 后 `MTR_MEMO_PAGE_X_FIX`。**锁随 mtr 提交/回滚一起释放，调用方不需要自己管锁的生命周期。**
- **线性预读**：若首次访问且模式不是 `PEEK_IF_IN_POOL`，调 `buf_read_ahead_linear(m_page_id, m_page_size, ibuf_inside(m_mtr))`。

### 「不读盘」模式的三条出路

`Buf_fetch_other::get()` 的函数体把「查不到页」分成三条出路：

```
if (m_mode == Page_fetch::IF_IN_POOL_OR_WATCH) {
  block = is_on_watch();
}
if (block != nullptr) break;
if (is_optimistic() || m_mode == Page_fetch::IF_IN_POOL_OR_WATCH) {
  return (DB_NOT_FOUND);
}
read_page();
```

1. **`IF_IN_POOL_OR_WATCH` → 登记观察**：`is_on_watch()` 的函数体：把哈希锁升到 X 模式（`rw_lock_x_lock`）并用 `buf_page_hash_lock_x_confirm()` 确认，调 `buf_pool_watch_set(m_page_id, &m_hash_lock)` 登记哨兵。`buf_pool_watch_set()`（同文件）会先查哈希表：若页已被别的线程读进来就直接返回该页；若已存在哨兵则调 `buf_block_fix(bpage)` 挂到已有哨兵上；否则要拿全部哈希锁（注释说明这是重路径，只被 purge 线程调用，不在关键路径上），并遍历 `buf_pool->watch[]` 数组找空哨兵。源码注释明确指出：**purge 线程数不会超过 `BUF_POOL_WATCH_SIZE`**，所以一个 purge 线程不可能在另一个 purge 线程持有哨兵时再登记一个。
2. **`IF_IN_POOL` / `PEEK_IF_IN_POOL`（乐观模式）→ 直接返回「没找到」**：`return (DB_NOT_FOUND)`，而 `single_page()` 收到 `DB_NOT_FOUND` 就 `return (nullptr)`。
3. **其余模式 → 读盘**：`read_page()`。

### 「回退到 X」的真实调用方

- **回退到 change buffer**：调用方是 `storage/innobase/btr/btr0cur.cc` 的 `btr_cur_search_to_nth_level()`。它的函数体里有这么一段——当 `latch_mode <= BTR_MODIFY_LEAF` 且 `btr_op != BTR_NO_OP` 且 `ibuf_should_try(index, ...)` 为真时，把取页模式改成乐观模式：

  ```
  fetch = btr_op == BTR_DELETE_OP ? Page_fetch::IF_IN_POOL_OR_WATCH
                                  : Page_fetch::IF_IN_POOL;
  ```

  随后调 `buf_page_get_gen(page_id, page_size, rw_latch, ..., fetch, {file, line}, mtr)`。若返回的 `block == nullptr`，函数体进入 change buffer 分支：按 `btr_op` 分别调 `ibuf_insert(IBUF_OP_INSERT, ...)`（并把 `cursor->flag` 置为 `BTR_CUR_INSERT_TO_IBUF`）、`ibuf_insert(IBUF_OP_DELETE_MARK, ...)`（置 `BTR_CUR_DEL_MARK_IBUF`）、或 `ibuf_insert(IBUF_OP_DELETE, ...)`（置 `BTR_CUR_DELETE_IBUF`）。**这就是「取页失败」的合法回退路径：不是报错，而是把写操作缓冲到 change buffer 里。**
- **回退到观察哨兵**：调用方同样是 `btr_cur_search_to_nth_level()`，但只在 `btr_op == BTR_DELETE_OP` 时用 `IF_IN_POOL_OR_WATCH`；此时若 `row_purge_poss_sec(cursor->purge_node, index, tuple)` 为假，置 `cursor->flag = BTR_CUR_DELETE_REF`（暂不清理），若 change buffer 也接不住则调 `buf_pool_watch_unset(page_id)` 撤销哨兵。
- **回退到自适应哈希索引路径**：调用方是 `storage/innobase/btr/btr0sea.cc`，它用 `Page_fetch::PEEK_IF_IN_POOL` 取页；源码注释说明它必须用 X 锁而不是 S 锁，因为调用方可能已持有该页的 X 锁。

### 各类调用方实际用的模式

| 调用方（文件） | 模式 | 锁 |
|---|---|---|
| `btr_cur_search_to_nth_level`（`btr/btr0cur.cc`） | `cursor->m_fetch_mode`，默认 `NORMAL` | `RW_NO_LATCH` / `upper_rw_latch` / `RW_SX_LATCH` |
| `btr_cur_search_to_nth_level`（`btr/btr0cur.cc`，可缓冲时） | `IF_IN_POOL` 或 `IF_IN_POOL_OR_WATCH` | 同上 |
| `btr_cur_search_to_nth_level`（`btr/btr0cur.cc`，根页猜块） | 传 `index->search_info->root_guess` 作 `guess` | 同上 |
| `btr_pcur_restore` 一类（`btr/btr0cur.cc`） | `POSSIBLY_FREED` | `RW_S_LATCH` |
| `btr_search`（`btr/btr0sea.cc`） | `PEEK_IF_IN_POOL` | `RW_X_LATCH` |
| `lock_page` 一类（`lock/lock0lock.cc`） | `POSSIBLY_FREED` | `RW_NO_LATCH` / `RW_X_LATCH` |
| `row_search_...`（`row/row0sel.cc`） | `NORMAL` | `RW_X_LATCH` / `RW_NO_LATCH` |

### 相关的旁路入口

同一文件里还有几个「已知页块」的取页入口，它们不走哈希查找，直接对已知块加引用计数：

- **`buf_page_optimistic_get()`**：先在 `block->mutex` 下检查 `buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE` 就不成立（返回 false）；成立则 `buf_block_buf_fix_inc(block, ...)` 加引用计数、`buf_page_set_accessed()` 打时间戳，然后按 `rw_latch` 用 `rw_lock_s_lock_nowait` / `rw_lock_x_lock_nowait` 尝试加锁（**用 nowait 版本**）。若 `modify_clock != block->modify_clock`（说明拿到的块已经被改过），则解锁并 `buf_block_buf_fix_dec(block)` 减引用计数，返回 false——**这是「乐观取页」的字面含义：拿到的必须是未被并发修改的那一版。**
- **`buf_page_get_known_nowait()`**：处理「已知块但状态可能是 `BUF_BLOCK_REMOVE_HASH`」的情况。源码注释说明：`BUF_BLOCK_REMOVE_HASH` 意味着另一个线程正在把这个块从 LRU 链表里摘掉，此时页已经被从页哈希表里移除了，**通过哈希索引访问到它的尝试只能是无效的**，因此直接返回 false。它同样用 `rw_lock_s_lock_nowait` / `rw_lock_x_lock_nowait`，且注释写明 `RW_SX_LATCH` 未实现（`default: ut_error; /* RW_SX_LATCH is not implemented yet */`）。

## 关键取舍

- **源码事实：取页被拆成「`Buf_fetch_normal`」与「`Buf_fetch_other`」两份 `get()` 实现，而不是一份带参数的实现。** 常规模式的 `get()` 函数体只有「查表 → 命中就 fix 并放锁 → 未命中就 `read_page()`」三步，函数体注释直接写着「Keep this path as simple as possible」。收益是**最热的路径没有多余的分支判断**；代价是任何语义调整都要同步改两处，两份实现容易漂移。

- **源码事实：`buf_block_fix()`（加引用计数）发生在**释放哈希锁之前**，且发生在确认页状态合法之后。** 顺序不能反——先释放哈希锁再 fix，页就可能在两者之间被换出。源码注释「Now safe to release page_hash S lock」就是在说这一点。收益是引用计数一旦加上就一定有效；代价是哈希锁的持有时间被拉长到「fix + 放锁」这一小段。

- **源码事实：读盘失败不是立刻报错，而是重试到 `BUF_PAGE_READ_MAX_RETRIES`（100）次才 fatal。** 理由写在 fatal 消息里：表可能已损坏。收益是能扛住瞬时的 I/O 错误；代价是真正的数据损坏会把错误发现时间推迟到第 100 次尝试之后。

- **源码事实：stale 页与「正在 I/O 的页」用两种不同的等待常量区分。** stale 页清不掉时睡 `WAIT_FOR_READ`（100 微秒），压缩页正在被解压 / 临时页正在被刷写时分别睡 `WAIT_FOR_READ` / `WAIT_FOR_WRITE`（都是 100 微秒）。源码注释强调这个等待是为了「不陷入空转消耗 CPU」，并注明「这不是常发事件」。取舍很清楚：**用固定的短睡眠换掉忙等**，代价是引入了最多 100 微秒的额外延迟。

- **源码事实：乐观模式（`IF_IN_POOL` / `PEEK_IF_IN_POOL`）在两个位置各做一次「页是否正在读入」的检查。** 一处在 `Buf_fetch_other::get()` 的「查不到就返回 `DB_NOT_FOUND`」，一处在 `single_page()` 里读 `buf_page_get_io_fix(bpage)` 判定 `state == BUF_IO_READ` 时 `buf_block_unfix` 并返回 `nullptr`。收益是**乐观模式的语义是严格的：宁可返回「没有」，也不返回「一个正在读入的半成品」**；代价是同一页可能既被查哈希表又被查 io 状态，两次检查之间存在窗口。

- **源码事实：`SCAN` 模式被特殊对待两处。** 一是 `read_page()` 里跳过 `buf_read_ahead_random()`（注释说扫描已经在更高抽象层并行化，再预读会造成不必要的 I/O）；二是 `single_page()` 里跳过 `buf_page_make_young_if_needed()`（注释说不要把页移到 LRU 链表头部，好让它能被快速丢弃）。**这是缓冲池抗扫描能力在取页层的落点。**

- **源码事实：`m_guess`（页块猜测）被三重校验后才用。** `lookup()` 里要同时满足「块还在某个缓冲池实例内」「块的页号与请求一致」「块状态是 `BUF_BLOCK_FILE_PAGE`」，否则丢弃猜测回落哈希查找。收益是并发场景下不会因为一个失效指针而读到别人的页；代价是每次都做这三次校验。

- **架构推论：** 「取页模式」这一层抽象，本质是把「**这一页不在内存里，我愿不愿意为它付 I/O 代价**」这个决策权交给了上层。InnoDB 把它做成了枚举而不是布尔量，因此才能同时表达「不读盘」（`IF_IN_POOL`）、「不读盘但留下观察点」（`IF_IN_POOL_OR_WATCH`）、「读盘但别把我变年轻」（`SCAN`）这些语义相近却行为不同的策略。主核的页缓存层如果要支持类似的自适应策略，同样需要把「缺失策略」做成显式的枚举而不是一个 `bool load_if_missing`。

- **架构推论：** InnoDB 的取页路径把「引用计数」与「加锁」这两件截然不同的事，通过 `mtr_add_page()` 绑在了同一个 mtr 上——引用计数在 `lookup()` 阶段加，锁在 `single_page()` 阶段加，两者都随 mtr 的提交或回滚一起释放。这个设计让调用方（`btr_cur_search_to_nth_level()` 那种要连续锁住 B-tree 多层的代码）不必手工管理每一次取页的锁生命周期，而是靠 `mtr_set_savepoint()` / `mtr_release_block_at_savepoint()` 做局部回滚。主核若要让上层能「一次事务内锁住多层结构」，同样的「锁与事务生命周期绑定」是值得借鉴的结构。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/buf/buf0buf.cc` | `buf_page_get_gen` | 取页总入口：调试期校验 `mtr` 活跃、`rw_latch` 与 `mode` 合法、页大小与表空间一致；按「`mode == NORMAL` 且非系统临时表空间」分成 `Buf_fetch_normal` 与 `Buf_fetch_other` 两条实现，各自 `single_page()` 返回 |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_get` | 按页所属表空间 id 路由到具体的缓冲池实例 `buf_pool_t *`；被 `Buf_fetch` 构造函数、`buf_pool_watch_set()`、`buf_page_init()`、`buf_page_init_for_read()` 等处调用 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch` | 取页模板基类：持页 id、页大小、是否临时表空间、目标缓冲池、哈希锁、模式、mtr 等状态；提供 `lookup()` / `is_on_watch()` / `read_page()` / `check_state()` / `zip_page_handler()` / `mtr_add_page()` / `is_optimistic()` / `temp_space_page_handler()` / `single_page()` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch_normal` | 常规取页实现：`get()` 只有「查表 → 命中则 fix 并放哈希锁 → 未命中则 `read_page()`」三步，函数体注释要求保持最简 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch_other` | 其余模式取页实现：`get()` 在查表未命中时依次尝试 `is_on_watch()`、乐观模式返回 `DB_NOT_FOUND`、最后 `read_page()`；临时表空间走 `temp_space_page_handler()` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::lookup` | 在页哈希表查页：算哈希锁 → S 锁加锁 → `buf_page_hash_lock_s_confirm()` 确认 → 三重校验 `m_guess` → `buf_page_hash_get_low()` 查找 → 排除观察哨兵 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::is_on_watch` | `IF_IN_POOL_OR_WATCH` 模式的观察登记：哈希锁升 X 并确认 → `buf_pool_watch_set()` → fix 或返回哨兵 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::read_page` | 读盘：`buf_read_page()` 成功则（非 `SCAN` 模式）`buf_read_ahead_random()`；失败则重试至 `BUF_PAGE_READ_MAX_RETRIES`（100）次后 `ib::fatal` 报 `ER_IB_MSG_74` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::check_state` | 按 `buf_block_get_state()` 分派：普通页（临时表空间且正被刷写则 unfix + 睡 `WAIT_FOR_WRITE` 后返回 `DB_FAIL`）、压缩页（转 `zip_page_handler()`）、其余状态 `ut_error` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::zip_page_handler` | 压缩页解压路径：`PEEK_IF_IN_POOL` 直接返回 `DB_NOT_FOUND`；正被 fix / I/O-fix 则睡 `WAIT_FOR_READ` 返回 `DB_FAIL`；否则取空块、`buf_relocate()`、`buf_zip_decompress()`、加入 `unzip_LRU` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::mtr_add_page` | 把页与锁一起挂到 mtr：`RW_NO_LATCH` → `MTR_MEMO_BUF_FIX`；S 锁 → `MTR_MEMO_PAGE_S_FIX`；SX 锁 → `MTR_MEMO_PAGE_SX_FIX`；X 锁 → `MTR_MEMO_PAGE_X_FIX` |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::single_page` | 取页外层循环：统计 `m_n_page_gets`；乐观模式下若 `BUF_IO_READ` 则 unfix 返回 `nullptr`；`check_state()` 返回 `DB_FAIL` 则重来；收尾做首次访问打戳、`buf_page_make_young_if_needed()`（`PEEK_IF_IN_POOL` / `SCAN` 除外）、`buf_wait_for_read()`、脏标记、`mtr_add_page()`、线性预读 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::temp_space_page_handler` | 临时表空间页的加锁路径：在 `block->mutex` 保护下调 `buf_block_fix()`，用互斥量而非 `block->lock` 与刷写线程同步 |
| `storage/innobase/buf/buf0buf.cc` | `Buf_fetch::is_optimistic` | 判定乐观模式：`m_mode == Page_fetch::IF_IN_POOL || m_mode == Page_fetch::PEEK_IF_IN_POOL` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_watch_set` | 观察哨兵登记：先查哈希表（已读入则直接返回该页；已有哨兵则 fix 到已有哨兵）；否则拿全部哈希锁并遍历 `buf_pool->watch[]` 找空位；注释说明 purge 线程数不超过 `BUF_POOL_WATCH_SIZE` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_watch_is_sentinel` | 判断某页是否为观察哨兵：地址落在 `buf_pool->watch[0..BUF_POOL_WATCH_SIZE)` 区间内即为哨兵，断言其状态为 `BUF_BLOCK_ZIP_PAGE` 且不在 `zip_hash` 中 |
| `storage/innobase/buf/buf0buf.cc` | `buf_page_free_stale` | 清理 stale 页（两个重载）：断言持有 `LRU_list_mutex`；`io_type == BUF_IO_NONE` 时把脏页从 flush list 摘掉并调 `buf_LRU_free_page()`；带哈希锁的重载在页被 I/O-fix 时先释放哈希锁并返回 false |
| `storage/innobase/buf/buf0buf.cc` | `buf_page_optimistic_get` | 已知块上的乐观取页：`block->mutex` 下校验状态为 `BUF_BLOCK_FILE_PAGE`；`buf_block_buf_fix_inc()` 加引用计数；用 `rw_lock_s_lock_nowait` / `rw_lock_x_lock_nowait` 尝试加锁；`modify_clock` 不符则解锁、减计数并返回 false |
| `storage/innobase/buf/buf0buf.cc` | `buf_page_get_known_nowait` | 已知块上的 nowait 取页：块状态为 `BUF_BLOCK_REMOVE_HASH` 时直接返回 false（注释说明该状态下页已从哈希表移除）；加引用计数后用 nowait 加锁并 `mtr_memo_push`；`RW_SX_LATCH` 未实现，走 `ut_error` |
| `storage/innobase/buf/buf0buf.cc` | `buf_page_init` | 页读入后初始化块：置 `BUF_BLOCK_FILE_PAGE`、处理哈希表中残留的观察哨兵（继承其 `buf_fix_count` 并 `buf_pool_watch_remove()`）、`HASH_INSERT` 插入页哈希表 |
| `storage/innobase/buf/buf0buf.cc` | `BUF_PAGE_READ_MAX_RETRIES` | 读入页的最大重试次数常量，取值 100；超过后 `read_page()` 报 `ER_IB_MSG_74` |
| `storage/innobase/buf/buf0buf.cc` | `WAIT_FOR_READ` / `WAIT_FOR_WRITE` | 两个等待常量，各为 100 微秒；用于 stale 页清不掉、压缩页正被解压、临时页正被刷写三种等待场景 |
| `storage/innobase/buf/buf0rea.cc` | `buf_read_page` | 实际读盘入口：调 `buf_read_page_low(&err, true, 0, BUF_READ_ANY_PAGE, ...)`；`DB_TABLESPACE_DELETED` 时报 `ER_IB_MSG_141`；返回 `count > 0` |
| `storage/innobase/buf/buf0rea.cc` | `buf_read_ahead_random` | 随机读预读：受 `srv_random_read_ahead` 与 `srv_startup_is_before_trx_rollback_phase` 开关控制；ibuf 位图页与事务系统头页不预读；按 `buf_pool->read_ahead_area` 对齐区域边界 |
| `storage/innobase/buf/buf0rea.cc` | `buf_read_ahead_linear` | 线性读预读：按 `buf_pool->read_ahead_area` 计算区域，在页为该区域边界且同区页已被顺序访问时批量预读 |
| `storage/innobase/buf/buf0lru.cc` | `buf_LRU_relocate` 一侧的 sticky 标记 | 在释放 `block_mutex` 前调 `buf_page_set_sticky(b)`，注释写明目的是「Prevent `buf_page_get_gen()` from decompressing the block while we release block_mutex」 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_search_to_nth_level` | 本机制最重要的调用方：决定取页模式（可缓冲时用 `IF_IN_POOL` / `IF_IN_POOL_OR_WATCH`）、传 `root_guess` 猜测块；返回 `nullptr` 时回退到 change buffer（`ibuf_insert` 的 `IBUF_OP_INSERT` / `IBUF_OP_DELETE_MARK` / `IBUF_OP_DELETE`） |
| `storage/innobase/btr/btr0sea.cc` | `btr_search` | 自适应哈希索引查找：用 `PEEK_IF_IN_POOL` + `RW_X_LATCH` 取页；注释说明因调用方可能已持 X 锁而不能改用 S 锁 |
| `storage/innobase/lock/lock0lock.cc` | `lock_page` 一类入口 | 加锁系统读页：用 `POSSIBLY_FREED` 模式（注释说明表空间可能已被 DROP），分别用 `RW_NO_LATCH` 与 `RW_X_LATCH` |
| `storage/innobase/row/row0sel.cc` | `row_select` 一类入口 | 行选择读页：用 `NORMAL` 模式，分别以 `RW_X_LATCH` 与 `RW_NO_LATCH` 取二级索引页 |

## 与主核 / AI 的关系

主核（`engine/db`）的存储层目前没有 InnoDB 这套「页哈希表 + 多缓冲池实例 + 引用计数 + 取页模式枚举」的完整结构；`engine/db/storage/` 下的 pager 更接近单文件 / 单缓冲的形态，也没有「一页被多个事务共享并各自加引用计数」的机制。

**架构推论：** InnoDB 把「页在不在池里」这个判据收敛到**一个**页哈希表上（每个缓冲池实例一张），使得「取页」在热路径上只是一次哈希查找 + 一次 S 锁 + 一次计数加一。主核若要让多个连接共享同一份缓存页，同样需要一张「页 → 缓存帧」的全局映射表；没有这张表，「页被并发替换」与「页被并发修改」这两类竞态就无法分别处理。

**架构推论：** `Page_fetch` 枚举把「I/O 代价由谁承担」变成了接口的一部分——这是 InnoDB 能在不改动取页代码的前提下支持 change buffer、purge 观察哨兵、扫描抗污染三种截然不同的上层策略的原因。对主核的启示是：当一个缓存接口的下游有「宁可失败也不能付 I/O」「可以失败但要留下通知点」这类需求时，把缺失策略做成显式枚举、并让每个枚举值对应一条**有名字的真实回退路径**（本机制里分别是 `ibuf_insert()` 与 `buf_pool_watch_set()`），比留一个布尔开关更容易维护。

**架构推论：** 本机制把「引用计数」与「读写锁」的释放都绑定到 mtr 的生命周期上（`mtr_add_page()` 一次完成加锁与记账），这让 `btr_cur_search_to_nth_level()` 那种「在一次搜索里连续持有多层 B-tree 页锁」的代码可以用 savepoint 精确回滚。主核要在事务内锁住多层结构时，同样的绑定方式能显著降低上层出错概率。

**架构推论：** 对 AI 参与的负载，本机制暴露了一个值得主核注意的接口形态：InnoDB 在「页不在池里」时不是报错，而是**根据操作类型分流到不同的补偿机制**（写操作进 change buffer、purge 挂观察点、纯读则返回「没找到」由上层决定重试或降级）。AI 生成的 SQL 负载里批量读与点查混合、且写入常带唯一键冲突，这种「缺失 → 分流补偿」的结构比统一的「读失败」更能保持服务可用。

## 参考

- [MySQL 源码：`storage/innobase/buf/buf0buf.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/buf/buf0buf.cc) —— `buf_page_get_gen()`、`Buf_fetch` / `Buf_fetch_normal` / `Buf_fetch_other` 模板体系、`lookup()` / `read_page()` / `check_state()` / `zip_page_handler()` / `mtr_add_page()` / `single_page()`、`buf_pool_watch_set()`、`buf_page_free_stale()`，以及 `BUF_PAGE_READ_MAX_RETRIES`（100）、`WAIT_FOR_READ` / `WAIT_FOR_WRITE`（各 100 微秒）、`BUF_READ_AHEAD_PAGES`（64）、`BUF_READ_AHEAD_PORTION`（32）四个常量。
- [MySQL 源码：`storage/innobase/buf/buf0rea.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/buf/buf0rea.cc) —— `buf_read_page()` / `buf_read_page_low()` 的读盘路径，以及 `buf_read_ahead_random()` 与 `buf_read_ahead_linear()` 两个预读入口的开关与区域边界计算。
- [MySQL 源码：`storage/innobase/btr/btr0cur.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/btr/btr0cur.cc) —— 调用方 `btr_cur_search_to_nth_level()`：取页模式的选择（`IF_IN_POOL` / `IF_IN_POOL_OR_WATCH`）、`root_guess` 的使用，以及取页失败后回退到 change buffer 的三条 `ibuf_insert()` 分支。
- [MySQL 源码：`storage/innobase/lock/lock0lock.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/lock/lock0lock.cc) —— 锁系统以 `POSSIBLY_FREED` 模式取页的两处调用，及其「表空间可能已被 DROP」这一前提。
- [MySQL 参考手册：17.5.1 Buffer Pool（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-buffer-pool.html) —— 缓冲池的官方定义、LRU 中点插入策略（old 子列表默认占 3/8）、`SHOW ENGINE INNODB STATUS` 中 `BUFFER POOL AND MEMORY` 段的各项指标含义，是理解取页之后「页在池里如何被老化与淘汰」的官方材料。
- [MySQL 参考手册：17.5 InnoDB In-Memory Structures（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-in-memory-structures.html) —— InnoDB 内存结构总览，说明缓冲池在内存结构中的位置。
