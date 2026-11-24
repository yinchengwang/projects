---
project: mysql-server
feature: buf-lru-eviction
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/buf/buf0lru.cc
    symbols: [buf_LRU_free_page, buf_LRU_stat_update]
  - path: storage/innobase/buf/buf0buf.cc
    symbols: [buf_pool_free]
---

# LRU 淘汰

## 一句话职责

`storage/innobase/buf/buf0lru.cc` 里的 LRU 淘汰机制负责**在缓冲池实例内维护空闲页（free list）与 LRU 链表这两套结构，并在需要帧（frame）时决定淘汰哪一个页**：由 `buf_LRU_get_free_block()` 驱动取帧流程，先看 free list 是否还有空闲块，没有则从 LRU 链尾部扫描挑选牺牲者（victim），由 `buf_LRU_free_page()` 完成「从 page hash 与 LRU 链摘除 → 把帧交还 free list」这一动作；压缩页另有一条 `unzip_LRU` 旁路，其取舍由 `buf_LRU_evict_from_unzip_LRU()` 依据 `buf_LRU_stat_update()` 采集的历史 I/O 与解压计数决定。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责缓冲池空闲页的 LRU 链表维护与淘汰页的选择；**不负责写回（H02）**、**不负责页的取用（H01）**。

## 机制是什么

### 三条链表，各司其职

在单个缓冲池实例 `buf_pool_t` 内，与淘汰相关的链表有三套：

| 链表 | 成员变量 | 含义 | 相关操作 |
|---|---|---|---|
| free list | `buf_pool->free` | 已分配但当前不承载任何文件页的帧，等待被取用 | `buf_LRU_get_free_only()` 从中取块 |
| LRU 链 | `buf_pool->LRU` | 已读入的文件页，按访问新旧排序，**头部最新、尾部最旧** | `buf_LRU_add_block_low()` / `buf_LRU_remove_block()` |
| unzip_LRU 链 | `buf_pool->unzip_LRU` | 压缩页的**解压帧**单独成链，压缩数据本体仍留在 LRU 链上 | `buf_unzip_LRU_add_block()` / `buf_unzip_LRU_remove_block_if_needed()` |

free list 与 LRU 链由两把互斥量分别保护：`buf_pool->free_list_mutex` 与 `buf_pool->LRU_list_mutex`。`buf_LRU_get_free_only()` 全程持 `free_list_mutex`，`buf_LRU_get_free_block()` 与 `buf_LRU_scan_and_free_block()` 全程持 `LRU_list_mutex`。

### LRU 链的「旧 / 新」分段

`buf_LRU_add_block_low()` 把页插入 LRU 链时，插入点取决于 `old` 参数：

```
if (!old || (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN)) {
    UT_LIST_ADD_FIRST(buf_pool->LRU, bpage);
    bpage->freed_page_clock = buf_pool->freed_page_clock;
} else {
    UT_LIST_INSERT_AFTER(buf_pool->LRU, buf_pool->LRU_old, bpage);
    buf_pool->LRU_old_len++;
}
```

- `old == false`（新页）：插到**链头**，并把 `bpage->freed_page_clock` 记为当前的 `buf_pool->freed_page_clock`——这是一个**时钟戳**，记录该页进入链头的时刻。
- `old == true`（旧页）：插到 `buf_pool->LRU_old` 之后，即**新旧分界点**处。

分段长度由 `calculate_desired_LRU_old_size()` 决定：

```
return std::min(UT_LIST_GET_LEN(buf_pool->LRU) *
                    static_cast<size_t>(buf_pool->LRU_old_ratio) /
                    BUF_LRU_OLD_RATIO_DIV,
                UT_LIST_GET_LEN(buf_pool->LRU) -
                    (BUF_LRU_OLD_TOLERANCE + BUF_LRU_NON_OLD_MIN_LEN));
```

**取值来源可追到源码常量：** 上限是「LRU 链长 × `LRU_old_ratio` ÷ `BUF_LRU_OLD_RATIO_DIV`」，下限是「LRU 链长 −（`BUF_LRU_OLD_TOLERANCE` + `BUF_LRU_NON_OLD_MIN_LEN`）」。其中 `BUF_LRU_OLD_TOLERANCE` 与 `BUF_LRU_NON_OLD_MIN_LEN` 是 `buf0lru.cc` 中的编译期常量：

```
constexpr uint32_t BUF_LRU_OLD_TOLERANCE = 20;
constexpr uint32_t BUF_LRU_NON_OLD_MIN_LEN = 5;
```

即**新旧分界点允许偏离理想位置 20 个页，且新段至少保留 5 个页**。`buf_LRU_old_adjust_len()` 按这个容差移动 `LRU_old` 指针：偏差小于容差时把 `LRU_old` 前移一页并把该页标记为 old，超出容差时后移一页并取消其 old 标记，直到落回容差区间内。

`buf_LRU_old_ratio` 本身由 `buf_LRU_old_ratio_update_instance()` 换算并夹紧：

```
ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100;
if (ratio < BUF_LRU_OLD_RATIO_MIN) {
    ratio = BUF_LRU_OLD_RATIO_MIN;
} else if (ratio > BUF_LRU_OLD_RATIO_MAX) {
    ratio = BUF_LRU_OLD_RATIO_MAX;
}
```

**`old_pct` 的初始值来自 `buf_pool_init()`（`buf0buf.cc`）中的 `buf_LRU_old_ratio_update(100 * 3 / 8, false);`——即 `old_pct = 37`（`100 * 3 / 8` 是整数除法，`300 / 8 = 37`），旧段占 37%。**

### 摘链时的分界点维护

`buf_LRU_remove_block()` 摘除一个块时，若被摘的正是 `buf_pool->LRU_old`，会把分界点前移一页：

```
if (bpage == buf_pool->LRU_old) {
    buf_page_t *prev_bpage = UT_LIST_GET_PREV(LRU, bpage);
    ut_a(prev_bpage);
    buf_pool->LRU_old = prev_bpage;
    buf_page_set_old(prev_bpage, true);
    buf_pool->LRU_old_len++;
}
```

若摘除后 LRU 链长度**短于** `BUF_LRU_OLD_MIN_LEN`，则**整个分段机制被关闭**：所有剩余页的 `old` 标志被清掉，`LRU_old` 置为 `nullptr`，`LRU_old_len` 归零并直接返回。

### unzip_LRU 旁路：只为压缩页而设

压缩页在缓冲池中有两份数据：磁盘上的**压缩帧**（`bpage->zip.data`）与内存中的**解压帧**（`block->frame`）。`buf_page_belongs_to_unzip_LRU()` 判定的压缩页会把解压帧登记到 `buf_pool->unzip_LRU` 链上。

从 unzip_LRU 淘汰时，**只丢弃解压帧，压缩数据本体保留**——因此「从 unzip_LRU 淘汰脏页是允许的」，而从普通 LRU 链淘汰则必须页是干净的。这条差异是 `buf_LRU_free_page()` 里 `zip` 参数的语义来源。

`buf_LRU_evict_from_unzip_LRU()` 决定本次淘汰走哪条链：

```
if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0) {
    return false;
}

if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) <=
    UT_LIST_GET_LEN(buf_pool->LRU) / 10) {
    return false;
}

if (buf_pool->freed_page_clock == 0) {
    return true;
}

ulint io_avg =
    buf_LRU_stat_sum.io / BUF_LRU_STAT_N_INTERVAL + buf_LRU_stat_cur.io;

ulint unzip_avg =
    buf_LRU_stat_sum.unzip / BUF_LRU_STAT_N_INTERVAL + buf_LRU_stat_cur.unzip;

return (unzip_avg <= io_avg * BUF_LRU_IO_TO_UNZIP_FACTOR);
```

**四个判据的数字全部来自源码常量：**

- `unzip_LRU` 为空 → 走普通 LRU。
- `unzip_LRU` 长度 **≤ LRU 长度 ÷ 10** → 走普通 LRU。源码注释说明这个余量是「为了把热的解压页留在缓冲池里」。
- `buf_pool->freed_page_clock == 0` → **淘汰尚未开始**，按「负载受磁盘限制」的默认假设走 unzip_LRU。
- 否则按历史均值比较：I/O 次数均值 × **`BUF_LRU_IO_TO_UNZIP_FACTOR`（= 50）** 与解压次数均值比较，解压均值更小（或相等）则走 unzip_LRU。

**`BUF_LRU_IO_TO_UNZIP_FACTOR = 50` 与 `BUF_LRU_STAT_N_INTERVAL = 50` 都是 `buf0lru.cc` 里的编译期常量：**

```
static const ulint BUF_LRU_STAT_N_INTERVAL = 50;
static const ulint BUF_LRU_IO_TO_UNZIP_FACTOR = 50;
```

`BUF_LRU_STAT_N_INTERVAL` 的源码注释写明「每个区间 1 秒，由 `srv_error_monitor_thread()` 调用 `buf_LRU_stat_update()` 的频率定义」。

## 怎么实现的

### 入口：`buf_LRU_get_free_block()` 的迭代协议

用户线程需要一帧时调用 `buf_LRU_get_free_block()`。它的循环协议在源码注释里写得很清楚，代码与注释一致：

```
loop:
  buf_LRU_check_size_of_non_data_objects(buf_pool);

  block = buf_LRU_get_free_only(buf_pool);
  if (block != nullptr) {
    ...
    return block;
  }

  freed = false;
  if (buf_pool->try_LRU_scan || n_iterations > 0) {
    freed = buf_LRU_scan_and_free_block(buf_pool, n_iterations > 0);
    if (!freed && n_iterations == 0) {
      buf_pool->try_LRU_scan = false;
      os_wmb;
    }
  }

  if (freed) {
    goto loop;
  }
  ...
  if (!buf_flush_single_page_from_LRU(buf_pool)) {
    MONITOR_INC(MONITOR_LRU_SINGLE_FLUSH_FAILURE_COUNT);
    ++flush_failures;
  }

  srv_stats.buf_pool_wait_free.add(n_iterations, 1);
  n_iterations++;
  goto loop;
```

对应的迭代协议是：

- **iteration 0**：先取 free list；若 `buf_pool->try_LRU_scan` 为真，扫描 LRU 链**尾部**（扫描深度受 `srv_LRU_scan_depth` 约束）找一个干净块放回 free list；成功后重试 free list。若仍失败，则从 LRU 链尾部刷一个脏页到磁盘，成功后重试 free list。
- **iteration 1**：同 iteration 0，但**扫描整条 LRU 链**，且**无论 `buf_pool->try_LRU_scan` 是否为真都扫描**。
- **iteration > 1**：同 iteration 1，但每轮**睡眠 10 毫秒**。

**「10 毫秒」来自源码常量：**

```
if (n_iterations > 1) {
    MONITOR_INC(MONITOR_LRU_GET_FREE_WAITS);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

### 扫描：`buf_LRU_scan_and_free_block()` 的两级结构

```
bool buf_LRU_scan_and_free_block(buf_pool_t *buf_pool, bool scan_all) {
    bool freed = false;
    bool use_unzip_list = UT_LIST_GET_LEN(buf_pool->unzip_LRU) > 0;

    mutex_enter(&buf_pool->LRU_list_mutex);

    if (use_unzip_list) {
        freed = buf_LRU_free_from_unzip_LRU_list(buf_pool, scan_all);
    }

    if (!freed) {
        freed = buf_LRU_free_from_common_LRU_list(buf_pool, scan_all);
    }

    if (!freed) {
        mutex_exit(&buf_pool->LRU_list_mutex);
    }

    return (freed);
}
```

**顺序是固定的：先 unzip_LRU，再普通 LRU。** 且只有 unzip_LRU 非空时才尝试第一条路径。

**`scan_all` 参数决定扫描深度，两个调用方传的值不同：**

- `buf_LRU_get_free_block()` 传 `n_iterations > 0`——**只有第 0 轮是浅扫描，之后才是深扫描**。
- `buf0buf.cc` 的 `buf_pool_invalidate_instance()` 传 `true`——**永远深扫描**。

### 从 unzip_LRU 挑牺牲者：`buf_LRU_free_from_unzip_LRU_list()`

```
for (buf_block_t *block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
     block != nullptr && !freed && (scan_all || scanned < srv_LRU_scan_depth);
     ++scanned) {
    ...
    freed = buf_LRU_free_page(&block->page, false);
    ...
}
```

**两个要点：**

1. 从链尾（**最旧**的一端）开始扫描。
2. **浅扫描的深度是 `srv_LRU_scan_depth`（不是 `BUF_LRU_SEARCH_SCAN_THRESHOLD`）**，与普通 LRU 路径不同。
3. 调用 `buf_LRU_free_page()` 时 **`zip` 参数传 `false`**——只释放解压帧，压缩数据本体保留。

进入该函数的第一件事是 `if (!buf_LRU_evict_from_unzip_LRU(buf_pool)) return (false);`——即**是否走 unzip_LRU 的判定在这里再确认一次**，与 `buf_LRU_scan_and_free_block()` 里的 `use_unzip_list` 判据是两个不同的条件（前者是长度判据，后者是长度 + 历史统计的复合判据）。

### 从普通 LRU 挑牺牲者：`buf_LRU_free_from_common_LRU_list()`

```
for (buf_page_t *bpage = buf_pool->lru_scan_itr.start();
     bpage != nullptr && !freed &&
     (scan_all || scanned < BUF_LRU_SEARCH_SCAN_THRESHOLD);
     ++scanned, bpage = buf_pool->lru_scan_itr.get()) {
    ...
    const auto accessed = buf_page_is_accessed(bpage);

    if (bpage->was_stale()) {
        freed = buf_page_free_stale(buf_pool, bpage);
    } else {
        mutex_enter(block_mutex);

        if (buf_flush_ready_for_replace(bpage)) {
            freed = buf_LRU_free_page(bpage, true);
        }

        if (!freed) {
            mutex_exit(block_mutex);
        }
    }

    if (freed && accessed == std::chrono::steady_clock::time_point{}) {
        ++buf_pool->stat.n_ra_pages_evicted;
    }
    ...
}
```

**四个可追到源码的细节：**

1. **浅扫描深度是 `BUF_LRU_SEARCH_SCAN_THRESHOLD`，其值来自 `buf0lru.cc` 的编译期常量：**

   ```
   /** We scan these many blocks when looking for a clean page to evict
   during LRU eviction. */
   static const ulint BUF_LRU_SEARCH_SCAN_THRESHOLD = 100;
   ```

2. **stale 页走独立分支**：`bpage->was_stale()` 为真时不检查 `buf_flush_ready_for_replace()`，直接调 `buf_page_free_stale()`（定义在 `buf0buf.cc`）。`buf_page_free_stale()` 内部只在 `io_type == BUF_IO_NONE` 时才尝试淘汰，且对脏页会先 `buf_flush_remove(bpage)` 摘出 flush list 再调 `buf_LRU_free_page(bpage, true)`。

3. **非 stale 页必须先过 `buf_flush_ready_for_replace()` 这一关**，才调 `buf_LRU_free_page(bpage, true)`——此时 **`zip` 参数传 `true`**，表示**整个块都要释放**（解压帧与压缩数据一起）。

4. **「从未被访问过的页被淘汰」会被单独计数**：若页被淘汰且其访问时间戳为「零值」（`std::chrono::steady_clock::time_point{}`，即从未被访问过），则 `++buf_pool->stat.n_ra_pages_evicted`。源码注释写明这「给出了预读（readahead）有效性的度量」。

### 真正的淘汰动作：`buf_LRU_free_page()`

`buf_LRU_free_page(bpage, zip)` 是本机制的**核心淘汰函数**，返回值是布尔——**`true` 表示淘汰成功**。

**（1）前置断言与第一道拒绝条件。** 函数进入时断言调用方已持有 `LRU_list_mutex`、块互斥量，且页在文件中：

```
ut_ad(bpage->in_LRU_list);
ut_ad(mutex_own(&buf_pool->LRU_list_mutex));
ut_ad(mutex_own(block_mutex));
ut_ad(buf_page_in_file(bpage));

if (!buf_page_can_relocate(bpage)) {
    /* Do not free buffer fixed and I/O-fixed blocks. */
    return (false);
}
```

**被 buffer-fix 或 IO-fix 的块不能被淘汰**——这是第一道拒绝。

**（2）区分「整块释放」与「只释放解压帧」。**

```
buf_page_t *b{};
auto is_dirty = bpage->is_dirty();

if (zip || bpage->zip.data == nullptr) {
    /* This would completely free the block. */
    /* Do not completely free dirty blocks. */

    if (is_dirty) {
        return (false);
    }
} else if (is_dirty && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
    ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_DIRTY);
    return (false);
} else if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {
    b = buf_page_alloc_descriptor();
    ut_a(b);
}
```

三条分支：

- **`zip == true`（或该页无压缩数据）**：会**完全释放整个块**，因此**脏页直接拒绝**。
- **`zip == false` 且页是脏的且状态不是 `BUF_BLOCK_FILE_PAGE`**（即 `BUF_BLOCK_ZIP_DIRTY`）：拒绝。
- **`zip == false` 且页是脏的但状态是 `BUF_BLOCK_FILE_PAGE`**：此时**先为压缩数据本体另分配一个描述符 `b`**（`buf_page_alloc_descriptor()`），后续只释放解压帧，把压缩数据本体连同 `b` 一起保留。

**（3）释放块互斥量、拿 page hash 排他锁、再检查一次。** 源码在这里显式地「放锁—加锁」并**重新检查条件**：

```
mutex_exit(block_mutex);
DBUG_EXECUTE_IF("buf_lru_free_page_delay_block_mutex_reacquisition",
                std::this_thread::sleep_for(std::chrono::microseconds(100)););

rw_lock_x_lock(hash_lock, UT_LOCATION_HERE);
mutex_enter(block_mutex);
is_dirty = bpage->is_dirty();

if (!buf_page_can_relocate(bpage) ||
    ((zip || bpage->zip.data == nullptr) && is_dirty)) {
    rw_lock_x_unlock(hash_lock);
    if (b != nullptr) {
        buf_page_free_descriptor(b);
    }
    return (false);
}

if (is_dirty && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
    ...
    return (false);
}
```

**这是全函数最关键的一段：解锁窗口内别的线程可能已经把页改成脏或加了 fix，所以必须在重新持锁后把两个拒绝条件再判一次。** 源码注释解释了放锁的理由——对应的调试点是 `buf_lru_free_page_delay_block_mutex_reacquisition`，睡眠 **100 微秒**（`std::chrono::microseconds(100)`）。

**（4）从 page hash 与 LRU 链摘除。**

```
if (!buf_LRU_block_remove_hashed(bpage, zip, false)) {
    mutex_exit(&buf_pool->LRU_list_mutex);

    if (b != nullptr) {
        buf_page_free_descriptor(b);
    }
    return true;
}
```

`buf_LRU_block_remove_hashed()` 的返回值语义在源码声明处写明：「`true` 表示 `BUF_BLOCK_FILE_PAGE` 已从 page_hash 摘除，调用方需要把页放回 free list；`false` 表示 `BUF_BLOCK_ZIP_PAGE` 已从 page_hash 摘除，此时该块已经交还给 buddy 分配器」。**两条路径都算淘汰成功，返回 `true`。**

**（5）压缩页只释放解压帧时，把压缩数据本体「换回」原位。** 这是本函数最复杂的一段：摘除前已把 `bpage` 的内容拷贝进 `b`（`new (b) buf_page_t(*bpage);`），摘除后需要把 `b` 重新插回 page hash 与 LRU 链的原位置：

```
auto prev_b = UT_LIST_GET_PREV(LRU, b);

rw_lock_x_lock(hash_lock, UT_LOCATION_HERE);
mutex_enter(block_mutex);

ut_a(!buf_page_hash_get_low(buf_pool, b->id));

b->state = b->is_dirty() ? BUF_BLOCK_ZIP_DIRTY : BUF_BLOCK_ZIP_PAGE;

HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, b->id.hash(), b);

if (prev_b != nullptr) {
    UT_LIST_INSERT_AFTER(buf_pool->LRU, prev_b, b);
    incr_LRU_size_in_bytes(b, buf_pool);

    if (buf_page_is_old(b)) {
        buf_pool->LRU_old_len++;
        if (buf_pool->LRU_old == UT_LIST_GET_NEXT(LRU, b)) {
            buf_pool->LRU_old = b;
        }
    }

    auto lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

    if (lru_len > BUF_LRU_OLD_MIN_LEN) {
        buf_LRU_old_adjust_len(buf_pool);
    } else if (lru_len == BUF_LRU_OLD_MIN_LEN) {
        buf_LRU_old_init(buf_pool);
    }
} else {
    ut_d(b->in_LRU_list = false);
    buf_LRU_add_block_low(b, buf_page_is_old(b));
}
```

注意**重新入链后仍要重新维护新旧分界**：`lru_len > BUF_LRU_OLD_MIN_LEN` 时调 `buf_LRU_old_adjust_len()`，**恰好等于** `BUF_LRU_OLD_MIN_LEN` 时调 `buf_LRU_old_init()`（该函数断言 `ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);`）。

**（6）清理压缩数据的校验和，最后把帧交还 free list。**

```
mutex_enter(&buf_pool->zip_mutex);
rw_lock_x_unlock(hash_lock);
...
buf_flush_relocate_on_flush_list(bpage, b);
...
mutex_exit(&buf_pool->zip_mutex);
mutex_exit(block_mutex);
...
buf_LRU_block_free_hashed_page((buf_block_t *)bpage);
```

`buf_LRU_block_free_hashed_page()` 把块状态置为 `BUF_BLOCK_MEMORY` 再调 `buf_LRU_block_free_non_file_page()`，后者在 `UNIV_DEBUG` 下把整页内容清零、非调试下只把 `page_no` 与 `space_id` 位置写成 `0xfe`，然后 `UNIV_MEM_ASSERT_AND_FREE(block->frame, UNIV_PAGE_SIZE)` 释放帧内存并把块挂回 `buf_pool->free`。

**（7）压缩数据本体的校验和在互斥量之外计算。** 源码注释说明理由：此时块已是 `BUF_BLOCK_REMOVE_HASH` 状态且已从 `page_hash` 摘除，别的线程访问不到：

```
const uint32_t checksum = reporter.calc_zip_checksum(
    static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm));

mach_write_to_4(b->zip.data + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
```

### 统计采样：`buf_LRU_stat_update()`

`buf_LRU_stat_update()` 每秒被 `srv_error_monitor_thread()` 调用一次（频率由源码注释给出），作用是把「本区间」的计数并入历史环形缓冲。

```
for (ulint i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool = buf_pool_from_array(i);

    if (buf_pool->freed_page_clock != 0) {
        evict_started = true;
        break;
    }
}

if (!evict_started) {
    goto func_exit;
}

item = &buf_LRU_stat_arr[buf_LRU_stat_arr_ind];
buf_LRU_stat_arr_ind++;
buf_LRU_stat_arr_ind %= BUF_LRU_STAT_N_INTERVAL;

cur_stat = buf_LRU_stat_cur;

buf_LRU_stat_sum.io += cur_stat.io - item->io;
buf_LRU_stat_sum.unzip += cur_stat.unzip - item->unzip;

memcpy(item, &cur_stat, sizeof *item);

func_exit:
    memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
    os_wmb;
```

**三个可追到源码的事实：**

1. **「淘汰尚未开始」就不采样**：遍历全部 `srv_buf_pool_instances` 个实例，若所有实例的 `freed_page_clock` 都是 `0`，则直接跳到 `func_exit`（只清零当前计数，不写历史）。**`freed_page_clock` 非零是「本实例已经发生过淘汰」的标志**——`buf_LRU_add_block_low()` 每次把新页插到链头时都会把 `bpage->freed_page_clock` 设为当前值。
2. **环形缓冲长度是 `BUF_LRU_STAT_N_INTERVAL`（= 50）**，游标 `buf_LRU_stat_arr_ind` 按 `% BUF_LRU_STAT_N_INTERVAL` 循环。
3. **`buf_LRU_stat_sum` 是滑动和的维护方式**：`sum += cur - item`（加上本区间、减去被覆盖的旧区间）。源码注释解释为什么要先拷到局部变量 `cur_stat`：「`buf_LRU_stat_cur` 不受任何互斥量保护，在加到 `buf_LRU_stat_sum` 和拷到 `item` 之间它可能变化，赋给局部变量以保证同一个值同时赋给两者」。

**这两个统计量直接喂给 `buf_LRU_evict_from_unzip_LRU()` 的最后一段判据**——`buf_LRU_stat_sum.io` 与 `buf_LRU_stat_sum.unzip` 各除以 `BUF_LRU_STAT_N_INTERVAL` 得到历史均值，再与本区间未清零的 `buf_LRU_stat_cur` 相加。

### 降级路径与调用方

**降级路径一：单页 LRU 刷新。** `buf_LRU_get_free_block()` 在 free list 取不到块、LRU 扫描也没成功时，调用 **`buf_flush_single_page_from_LRU(buf_pool)`**（定义在 `buf0flu.cc`）。该函数从 LRU 链尾扫描：stale 页走 `buf_page_free_stale()`；`buf_flush_ready_for_replace()` 为真则调 `buf_LRU_free_page(bpage, true)`；否则若 `buf_flush_ready_for_flush(bpage, BUF_FLUSH_SINGLE_PAGE)` 为真则派发 IO 刷盘。**它是「先写回再淘汰」的兜底**——把脏页落盘后腾出帧。

**降级路径二：关停期全量淘汰。** `buf0buf.cc` 的 `buf_pool_invalidate_instance()` 在归档恢复结束后清空缓冲池：

```
ut_d(buf_must_be_all_freed_instance(buf_pool));

while (buf_LRU_scan_and_free_block(buf_pool, true)) {
}

mutex_enter(&buf_pool->LRU_list_mutex);

ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0);
ut_ad(UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0);

buf_pool->freed_page_clock = 0;
buf_pool->LRU_old = nullptr;
buf_pool->LRU_old_len = 0;
```

**它循环调用 `buf_LRU_scan_and_free_block(buf_pool, true)` 直到返回 `false`（扫不出可淘汰的页），随后断言 LRU 链与 unzip_LRU 链都已清空，并把 `freed_page_clock` 归零。** 注意 `freed_page_clock` 归零后，`buf_LRU_stat_update()` 的 `evict_started` 判据会重新回到「未开始淘汰」状态。

### `buf_pool_free()`：不是页淘汰，而是缓冲池全局结构的销毁

`buf0buf.cc` 里的 `buf_pool_free()` **与页淘汰无关**——它释放的是缓冲池的**全局数据结构**：

```
static void buf_pool_free() {
    ut::delete_(buf_stat_per_index);

    ut::delete_(buf_chunk_map_reg);
    buf_chunk_map_reg = nullptr;

    ut::free(buf_pool_ptr);
    buf_pool_ptr = nullptr;
}
```

它销毁三样东西：按索引的缓冲池统计 `buf_stat_per_index`、chunk 映射注册表 `buf_chunk_map_reg`、以及**实例数组 `buf_pool_ptr`**。调用点是 `buf_pool_init()` 的失败回滚：

```
if (err != DB_SUCCESS) {
    for (size_t id = 0; id < n; ++id) {
        if (buf_pool_ptr[id].chunks != nullptr) {
            buf_pool_free_instance(&buf_pool_ptr[id]);
        }
    }

    buf_pool_free();

    return (err);
}
```

**「回退到 `buf_pool_free()`」的实际调用方是 `buf_pool_init()`**——当某个实例的 `buf_pool_create()` 失败（`errs[id] != DB_SUCCESS`）时，先逐个实例调 `buf_pool_free_instance()` 释放实例资源，再调 `buf_pool_free()` 释放全局结构，最后返回错误码。

### `buf_pool_free_instance()`：单实例的页级回收

`buf_pool_free_instance()` 才真正遍历页并释放描述符：

```
for (bpage = UT_LIST_GET_LAST(buf_pool->LRU); bpage != nullptr;
     bpage = prev_bpage) {
    prev_bpage = UT_LIST_GET_PREV(LRU, bpage);
    buf_page_state state = buf_page_get_state(bpage);

    ut_ad(buf_page_in_file(bpage));
    ut_ad(bpage->in_LRU_list);

    if (state != BUF_BLOCK_FILE_PAGE) {
        /* We must not have any dirty block except
        when doing a fast shutdown. */
        ut_ad(state == BUF_BLOCK_ZIP_PAGE || srv_fast_shutdown == 2);
        buf_page_free_descriptor(bpage);
    }
}
```

**它遍历整条 LRU 链，对状态不是 `BUF_BLOCK_FILE_PAGE` 的块调 `buf_page_free_descriptor()` 释放描述符。** 断言 `ut_ad(state == BUF_BLOCK_ZIP_PAGE || srv_fast_shutdown == 2)` 说明：**除极快关闭（`srv_fast_shutdown == 2`）外，不允许有脏块留在链上**。

之后它释放实例的全部互斥量（`LRU_list_mutex`、`free_list_mutex`、`zip_free_mutex`、`zip_hash_mutex`、`flush_state_mutex`、`zip_mutex`、`flush_list_mutex`）、`buf_pool->watch`、所有 chunk 的块互斥量与 `rw_lock`、所有 `no_flush[i]` 事件，以及 `page_hash` / `zip_hash` 两个哈希表。

**它是 `buf_pool_free()` 的对称操作**——`buf_pool_free_instance()` 释放单实例资源，`buf_pool_free()` 释放全局资源。

### `buf_LRU_old_init()`：分界点的诞生

当 LRU 链长度**恰好等于** `BUF_LRU_OLD_MIN_LEN` 时，`buf_LRU_old_init()` 被调用：

```
ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);

for (buf_page_t *bpage = UT_LIST_GET_LAST(buf_pool->LRU); bpage != nullptr;
     bpage = UT_LIST_GET_PREV(LRU, bpage)) {
    bpage->old = true;
}

buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
buf_pool->LRU_old_len = UT_LIST_GET_LEN(buf_pool->LRU);

buf_LRU_old_adjust_len(buf_pool);
```

**它先把链上所有块标记为 old，再把 `LRU_old` 指向链头，最后调 `buf_LRU_old_adjust_len()` 把分界点移到正确位置。** 源码注释说明这个循环「临时违反了 `buf_page_set_old()` 的断言」。

### `buf_LRU_make_block_young()` / `buf_LRU_make_block_old()`

一对显式移动页的新旧位置的接口：

```
void buf_LRU_make_block_young(buf_page_t *bpage) {
    ...
    if (bpage->old) {
        buf_pool->stat.n_pages_made_young++;
    }

    buf_LRU_remove_block(bpage);
    buf_LRU_add_block_low(bpage, false);
}

void buf_LRU_make_block_old(buf_page_t *bpage) {
    ...
    buf_LRU_remove_block(bpage);
    buf_LRU_add_block_low(bpage, true);
}
```

**`buf_LRU_make_block_young()` 在页原本是 old 时会把 `buf_pool->stat.n_pages_made_young` 加一**——这个计数只在「从 old 变 young」时增加，是观察新旧分界活跃度的直接指标。

### 校验：`buf_LRU_validate_instance()`

调试构建下的 `buf_LRU_validate_instance()`（`UNIV_DEBUG || UNIV_BUF_DEBUG`）校验分界点的精确性：

```
if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {
    ut_a(buf_pool->LRU_old);

    const size_t new_len = calculate_desired_LRU_old_size(buf_pool);

    ut_a(buf_pool->LRU_old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
    ut_a(buf_pool->LRU_old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
}
```

**分界长度必须落在「理想长度 ± `BUF_LRU_OLD_TOLERANCE`（= 20）」之内**，否则断言失败。它还遍历整条链校验：所有 old 页必须是**连续的一段**（`old_len` 计数必须与 `buf_pool->LRU_old_len` 相等），以及 free list 中所有块的状态都必须是 `BUF_BLOCK_NOT_USED`。

## 关键取舍

### 取舍一：淘汰与写回解耦，脏页不能被直接淘汰

`buf_LRU_free_page()` 在 `zip == true`（整块释放）时对脏页**直接返回 `false`**，源码注释写明「Do not completely free dirty blocks」。这意味着**LRU 淘汰路径本身不具备写回能力**——脏页要腾位置，只能靠 `buf_flush_single_page_from_LRU()` 这条降级路径先刷盘。**代价是「要淘汰的页是脏的」时多一次刷盘往返；收益是淘汰路径不必处理 redo 与 WAL 顺序保证。**

### 取舍二：unzip_LRU 让「解压帧」与「压缩数据」解耦

压缩页把解压帧单独挂到 unzip_LRU 链，淘汰时只丢解压帧，压缩数据保留。**收益是解压帧可以随手淘汰而不必考虑脏页**；**代价是需要一条额外链表、一套额外判据（`buf_LRU_evict_from_unzip_LRU()`）和额外的锁内分支**。

### 取舍三：用 10% 的余量保住热的解压页

`buf_LRU_evict_from_unzip_LRU()` 里「unzip_LRU 长度 ≤ LRU 长度 ÷ 10 就走普通 LRU」这条判据，源码注释写明「这个余量让我们把热的解压页留在缓冲池里」。**代价是解压帧的内存占用被压在一个与 LRU 规模成比例的量级内；收益是避免把刚解压出来的高频页立刻扔掉。**

### 取舍四：I/O 权重 50 而不是 1

`buf_LRU_evict_from_unzip_LRU()` 的最终判据是 `unzip_avg <= io_avg * BUF_LRU_IO_TO_UNZIP_FACTOR`，而 `BUF_LRU_IO_TO_UNZIP_FACTOR = 50`。源码注释解释这个系数的含义：「用 I/O 操作次数乘以该系数，使其与 `page_zip_decompress()` 操作次数等量齐观」。**这是一个**人为设定的**代价函数——一次磁盘 I/O 被认为比一次内存解压「贵 50 倍」，于是负载偏 I/O 时倾向多从 unzip_LRU 淘汰（省掉重新解压的 CPU 代价）。** 取舍在于：`50` 是经验值而非可推导值，改动它会直接改变负载判定的分界线。

### 取舍五：放锁窗口内重新校验，宁可白跑一趟也不误淘汰

`buf_LRU_free_page()` 中「`mutex_exit(block_mutex)` → 加 page hash 排他锁 → `mutex_enter(block_mutex)` → 重新检查」这一段，主动放弃了持锁的连续性，只为在拿到 hash 排他锁后能重新判断 `buf_page_can_relocate()` 与脏状态。**代价是多一次解锁—加锁往返，且源码专门留了 `buf_lru_free_page_delay_block_mutex_reacquisition` 调试点（睡眠 100 微秒）来放大这个窗口做竞态测试；收益是不会把已经被别的线程加了 fix 或改脏的页误淘汰掉。**

### 取舍六：浅扫描用两个不同的深度

unzip_LRU 路径的浅扫描深度是 `srv_LRU_scan_depth`（可调参数），普通 LRU 路径是 `BUF_LRU_SEARCH_SCAN_THRESHOLD`（编译期常量 100）。**两个深度不统一，说明这两条路径被当成性质不同的负载来调**：unzip_LRU 是压缩页专区、深度可调；普通 LRU 的扫描窗口则是固定的。**代价是行为不完全对称；收益是压缩场景下可按需加深扫描以提高命中率。**

### 取舍七：淘汰路径不写盘，靠单页刷盘兜底

`buf_LRU_get_free_block()` 的最后一档是调 `buf_flush_single_page_from_LRU(buf_pool)`（在 `buf0flu.cc` 中），失败时累加 `flush_failures` 并增加 `MONITOR_LRU_SINGLE_FLUSH_FAILURE_COUNT`。而当迭代次数超过 **20** 且缓冲池未在缩容（`n_iterations > 20 && srv_buf_pool_size == srv_buf_pool_old_size`）时，发出 `ER_IB_MSG_134` 警告并启动 InnoDB Monitor。**「20」这个阈值来自源码的字面比较 `n_iterations > 20`。** 取舍在于：把「找不到空闲帧」当成异常上报，而不是静默降级。

### 取舍八：`buf_LRU_stat_update()` 用无锁计数器 + 环形缓冲

`buf_LRU_stat_cur` 与 `buf_LRU_stat_sum` **都不受任何互斥量保护**（源码注释明确写出），采样时先拷到局部变量再分别累加与写入。**取舍是「统计精度换零同步开销」**——统计值在极端并发下可能少计或多计，但绝不为统计付出锁代价。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/buf/buf0lru.cc | `buf_LRU_free_page` | 核心淘汰函数：前置检查、区分整块释放与只释放解压帧、放锁—加锁后二次校验、摘除 page hash 与 LRU 链、压缩数据本体的换回与校验和计算 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_stat_update` | 每秒一次的统计采样：判断淘汰是否已开始、写入长度为 `BUF_LRU_STAT_N_INTERVAL` 的环形缓冲、维护滑动和 `buf_LRU_stat_sum` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_get_free_block` | 取帧入口的迭代协议：free list → 浅扫 LRU → 单页刷盘兜底；第 0 轮浅扫，之后深扫并每轮睡眠 10 毫秒 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_get_free_only` | 只从 free list 取块：持 `free_list_mutex`，取到的块置为 `BUF_BLOCK_READY_FOR_USE` 并分配 `UNIV_PAGE_SIZE` 的帧 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_scan_and_free_block` | 扫描总入口：先 unzip_LRU 后普通 LRU；`scan_all` 为假时解锁 `LRU_list_mutex` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_free_from_unzip_LRU_list` | 从 unzip_LRU 链尾挑牺牲者，浅扫深度 `srv_LRU_scan_depth`，调 `buf_LRU_free_page(..., false)` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_free_from_common_LRU_list` | 从普通 LRU 链挑牺牲者，浅扫深度 `BUF_LRU_SEARCH_SCAN_THRESHOLD`（100）；stale 页走 `buf_page_free_stale()`，否则先过 `buf_flush_ready_for_replace()` 再调 `buf_LRU_free_page(..., true)` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_evict_from_unzip_LRU` | 决定走 unzip_LRU 还是普通 LRU：空链、10% 余量、`freed_page_clock == 0`、以及 I/O 与解压计数按 `BUF_LRU_IO_TO_UNZIP_FACTOR`（50）加权的均值比较 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_add_block_low` | 按 `old` 参数把块插入 LRU 链头或 `LRU_old` 之后；新页记录 `freed_page_clock` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_remove_block` | 摘链：维护 `LRU_old` 指针与 `LRU_old_len`，链长短于 `BUF_LRU_OLD_MIN_LEN` 时清空全部 old 标志 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_make_block_young` / `buf_LRU_make_block_old` | 显式移动页的新旧位置；前者会把 `stat.n_pages_made_young` 加一 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_old_init` | LRU 链长恰好等于 `BUF_LRU_OLD_MIN_LEN` 时初始化分界点：全链标 old、指向链头、再调 `buf_LRU_old_adjust_len()` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_old_adjust_len` | 按 `BUF_LRU_OLD_TOLERANCE`（20）容差移动 `LRU_old` 指针，维持新旧分段长度 |
| storage/innobase/buf/buf0lru.cc | `calculate_desired_LRU_old_size` | 计算旧段理想长度：LRU 链长 × `LRU_old_ratio` ÷ `BUF_LRU_OLD_RATIO_DIV`，并以「链长 −（`BUF_LRU_OLD_TOLERANCE` + `BUF_LRU_NON_OLD_MIN_LEN`）」为下限 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_old_ratio_update_instance` / `buf_LRU_old_ratio_update` | 把百分比换算成 `LRU_old_ratio` 并夹紧到 `BUF_LRU_OLD_RATIO_MIN` 与 `BUF_LRU_OLD_RATIO_MAX` 之间；`adjust` 为真时同步调整链 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_free_one_page` | 单页强制淘汰入口：调 `buf_LRU_block_remove_hashed(bpage, true, ignore_content)` 后把块交还 free list |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_block_free_hashed_page` / `buf_LRU_block_free_non_file_page` | 把帧交还 free list：置 `BUF_BLOCK_MEMORY`，按 `UNIV_DEBUG` 决定整页清零或只清 `page_no`/`space_id`，释放帧内存并入 free 链 |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_buf_pool_running_out` | 判定缓冲池是否只剩不到四分之一：比较 free list 与 LRU 链长度之和与 `min(curr_size, old_size) / 4` |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_check_size_of_non_data_objects` | 监控非数据对象占用：低于 `curr_size / 20` 时 `ib::fatal`，低于 `curr_size / 3` 时 `ib::warn` 并开启 InnoDB Monitor |
| storage/innobase/buf/buf0lru.cc | `buf_LRU_validate_instance` | 调试校验：分界长度必须落在理想值 ± `BUF_LRU_OLD_TOLERANCE` 内，old 页必须连续，free list 中块状态必须为 `BUF_BLOCK_NOT_USED` |
| storage/innobase/buf/buf0buf.cc | `buf_pool_free` | 释放缓冲池**全局**结构：`buf_stat_per_index`、`buf_chunk_map_reg`、实例数组 `buf_pool_ptr`；由 `buf_pool_init()` 的失败回滚调用 |
| storage/innobase/buf/buf0buf.cc | `buf_pool_free_instance` | 释放单个实例：遍历 LRU 链释放非 `BUF_BLOCK_FILE_PAGE` 块的描述符，销毁互斥量、chunk、`no_flush` 事件与两个哈希表 |
| storage/innobase/buf/buf0buf.cc | `buf_pool_init` | 创建缓冲池：并发调用 `buf_pool_create` 建实例，失败时逐个 `buf_pool_free_instance()` 后调 `buf_pool_free()` 回滚；末尾 `buf_LRU_old_ratio_update(100 * 3 / 8, false)` 设旧段比例 37% |
| storage/innobase/buf/buf0buf.cc | `buf_pool_invalidate_instance` | 关停期清空缓冲池：循环调 `buf_LRU_scan_and_free_block(buf_pool, true)` 直到扫不出，断言两条链已空，`freed_page_clock` 归零 |
| storage/innobase/buf/buf0buf.cc | `buf_page_free_stale` | stale 页淘汰：仅在 `io_type == BUF_IO_NONE` 时尝试，脏页先 `buf_flush_remove()` 再调 `buf_LRU_free_page(bpage, true)` |
| storage/innobase/buf/buf0flu.cc | `buf_flush_single_page_from_LRU` | 淘汰路径的降级兜底：用户线程找不到空闲帧时，从 LRU 链尾刷一页脏盘以腾出帧 |

## 与主核 / AI 的关系

**架构推论：** 本页描述的 LRU 淘汰是 InnoDB **存储引擎私有**的内存管理机制，完全位于 `storage/innobase/buf/` 之下；Server 层（`sql/` 目录）不感知 LRU 链、free list 或 `LRU_old` 分界点的存在。它与 H01（取页）、H02（刷脏）、H04（多实例）共同构成缓冲池这一「引擎私有」子系统，其中 H03 承担的是「**在已有帧不足时决定牺牲谁**」这一决策职责。

**架构推论：** 若主核（`engine/db/`）要实现内存页缓存并支持「缓存页可被驱逐」，本机制里最值得复刻的**不是函数结构，而是三条控制原则**：其一，**新旧分段的容量容差化**（`buf_LRU_old_adjust_len()` 用 `BUF_LRU_OLD_TOLERANCE` 允许分界点偏离理想位置，避免每次插入都精确调整）；其二，**「只释放一部分资源」的淘汰语义**（压缩页只丢解压帧、保留压缩数据，即 `buf_LRU_free_page()` 的 `zip` 参数）；其三，**释放锁窗口后的二次校验**（放锁—加锁后重新判断 `buf_page_can_relocate()` 与脏状态，宁可白跑一趟也不误淘汰）。

**架构推论：** 对 AI 检索场景，本机制的特点是「**决策依据高度依赖运行时状态，且统计口径分裂**」：`buf_LRU_evict_from_unzip_LRU()` 的最终判据同时依赖 `buf_LRU_stat_sum`（历史滑动和）、`buf_LRU_stat_cur`（本区间未清零计数）、`buf_pool->freed_page_clock`（是否已发生过淘汰）以及两个实例级长度（`unzip_LRU` 与 `LRU` 的长度比）。这意味着**任何「为什么 MySQL 此刻从解压链淘汰而不是从普通 LRU 淘汰」的问答，都必须先还原 `buf_LRU_stat_update()` 的采样序列**，而不能只从某一时刻的链表长度反推。

**架构推论：** 与之对照，本机制中的**硬性常量极少且都在源码里可枚举**：`BUF_LRU_OLD_TOLERANCE = 20`、`BUF_LRU_NON_OLD_MIN_LEN = 5`、`BUF_LRU_SEARCH_SCAN_THRESHOLD = 100`、`BUF_LRU_STAT_N_INTERVAL = 50`、`BUF_LRU_IO_TO_UNZIP_FACTOR = 50`、`BUF_LRU_DROP_SEARCH_SIZE = 1024`，以及 `buf_pool_init()` 中旧段比例 `100 * 3 / 8`（即 37%）。**架构推论：** 这类「少量魔数 + 少量可调参数（`srv_LRU_scan_depth`）」的组合，使得淘汰策略既有稳定的骨架（可推断），又有高度可变的强度（不可推断）——AI 在解释具体淘汰行为时应优先引用前者。

**架构推论：** 值得特别指出的是**淘汰路径本身不具备写回能力**（`buf_LRU_free_page()` 对脏页返回 `false`），这与许多「缓存驱逐时顺手落盘」的设计不同。它把「淘汰」与「持久化」彻底分成两条路径，由 `buf_flush_single_page_from_LRU()` 作为唯一的兜底连接点。这条分界让缓冲池的内存管理与 WAL 顺序保证互不干扰——**主核若要复刻，需要显式设计这条兜底路径的调用链**（在 InnoDB 中是 `buf_LRU_get_free_block()` → `buf_flush_single_page_from_LRU()`）。

## 参考

- [Buffer Pool（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-buffer-pool.html) —— 缓冲池、LRU 链表与页淘汰的官方说明。
- [InnoDB Configuration（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-parameters.html) —— `innodb_lru_scan_depth` 等与 LRU 扫描深度相关的参数官方定义。
- [InnoDB Checkpoints（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-checkpoints.html) —— 淘汰与刷脏相互配合的背景说明。
- [storage/innobase/buf/buf0lru.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0lru.cc) —— 本页全部 LRU 淘汰主符号的权威来源。
- [storage/innobase/buf/buf0buf.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0buf.cc) —— `buf_pool_free`、`buf_pool_free_instance`、`buf_pool_invalidate_instance` 与 `buf_page_free_stale` 的权威来源。
- [storage/innobase/buf/buf0flu.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/buf/buf0flu.cc) —— 淘汰路径的降级兜底 `buf_flush_single_page_from_LRU()` 所在文件。
