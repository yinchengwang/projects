---
project: mysql-server
feature: buf-pool-instance
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/buf/buf0buf.cc
    symbols: [buf_pool_init, buf_pool_free_instance, buf_pool_invalidate_instance]
---

# 缓冲池多实例

## 一句话职责

`storage/innobase/buf/buf0buf.cc` 的 `buf_pool_init()` 负责把一个逻辑上的大缓冲池**切分成 `srv_buf_pool_instances` 个互相独立的 `buf_pool_t` 实例**并把实例数组 `buf_pool_ptr` 建起来；每个实例有自己的页哈希表、free list、LRU 链表与一整套互斥量，从而把「页的读写」在实例维度上分散开，减少互斥竞争。`buf_pool_invalidate_instance()` 负责在归档恢复完成后把**单个实例**里的文件页全部作废并复位该实例的统计与 LRU 状态；`buf_pool_free_instance()` 负责把**单个实例**的互斥量、chunk、页哈希表与 zip 哈希表逐项释放。

## 机制是什么

**它解决的是「缓冲池太大，一个大锁把所有并发都串起来了」这个问题。** InnoDB 把缓冲池切成若干个**实例（instance）**，每个实例是一个独立的 `buf_pool_t` 结构体，有自己的：

- 页哈希表 `buf_pool->page_hash`（键是 `page_id_t`）与压缩页哈希表 `buf_pool->zip_hash`；
- LRU 链表 `buf_pool->LRU`、空闲块链表 `buf_pool->free`、待回收链表 `buf_pool->withdraw`、刷脏链表 `buf_pool->flush_list`、解压链 `buf_pool->unzip_LRU`；
- 一整套互斥量：`chunks_mutex`、`LRU_list_mutex`、`free_list_mutex`、`zip_free_mutex`、`zip_hash_mutex`、`zip_mutex`、`flush_state_mutex`、`flush_list_mutex`。

**实例归属是「一表空间一实例」而不是「一实例一表空间」。** 源码里唯一负责「页 → 实例」路由的函数是 `buf_pool_get()`；它被 `Buf_fetch` 模板类的构造函数调用（`m_buf_pool(buf_pool_get(m_page_id))`），也被 `buf_page_init()` 用断言校验（`ut_ad(buf_pool == buf_pool_get(page_id))`）。**架构推论：** 这条断言的存在说明「某个表空间的所有页必须落在同一个实例里」是硬约束——若同一表空间的页被分到不同实例，每个实例各有一张页哈希表，哈希查找就会漏页。因此 `buf_pool_get()` 的映射粒度是表空间（space id）而不是页号。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责把缓冲池拆成多个实例以减少互斥竞争、以及页到实例的归属计算；**不负责实例内部的 LRU 链表（H03）、不负责页的读入（H01）**。

## 怎么实现的

### `buf_pool_init()`：断言、分配、并行创建、失败回滚

`buf_pool_init(ulint total_size, ulint n_instances)` 的函数体按顺序做了六件事：

**第一步，三条断言把入参锁死。**

```
ut_ad(n_instances > 0);
ut_ad(n_instances <= MAX_BUFFER_POOLS);
ut_ad(n_instances == srv_buf_pool_instances);
```

- `n_instances > 0`：实例数必须为正，否则后面 `total_size / n_instances` 会除零。
- `n_instances <= MAX_BUFFER_POOLS`：实例数上限来自常量 `MAX_BUFFER_POOLS`（定义在 InnoDB 头文件里，本地稀疏检出未包含该头文件，故此处只引用常量名而不复述其数值）。
- `n_instances == srv_buf_pool_instances`：这是**关键的一致性断言**——`srv_buf_pool_instances` 是系统变量 `innodb_buffer_pool_instances` 的运行时值，全代码库遍历实例时用的都是它（`buf_get_total_list_len()`、`buf_pool_invalidate()`、`buf_pool_free_all()`、`buf_pool_set_sizes()` 等一律以 `srv_buf_pool_instances` 为循环上界）。如果调用方传入的 `n_instances` 与它不一致，实例数组的长度与后续所有遍历都会错位。

**第二步，每实例平分总大小。**

```
const ulint size = total_size / n_instances;
```

这是**整数除法，向下取整**。余数被直接丢弃：`srv_buf_pool_size` 不是 `n_instances` 的整数倍时，实际分配的总量会略小于配置值。

**第三步，分配实例数组与全局结构。**

```
buf_pool_ptr = (buf_pool_t *)ut::zalloc_withkey(
    UT_NEW_THIS_FILE_PSI_KEY, n_instances * sizeof *buf_pool_ptr);
buf_chunk_map_reg = ut::new_withkey<buf_pool_chunk_map_t>(UT_NEW_THIS_FILE_PSI_KEY);
```

`buf_pool_ptr` 是文件顶部的全局变量 `buf_pool_t *buf_pool_ptr;`——**它是一个长度为 `n_instances` 的连续数组**，后面所有「取第 i 个实例」的操作都走 `buf_pool_from_array(i)`。`zalloc_withkey` 保证整块清零，因此每个实例的 `buf_pool_t` 初始为零值。`buf_chunk_map_reg` 是「首帧地址 → chunk」的映射表 `buf_pool_chunk_map_t`，供按地址反查 chunk 用。

**第四步，按 CPU 核数分批并行创建实例。**

```
#ifdef UNIV_LINUX
ulint n_cores = sysconf(_SC_NPROCESSORS_ONLN);
if (n_cores > 8) {
  n_cores = 8;
}
#else
ulint n_cores = 4;
#endif /* UNIV_LINUX */
```

并发批大小来自这两个分支：Linux 上取 `sysconf(_SC_NPROCESSORS_ONLN)` 并**上限截断到 8**（源码注释写明：经验值来自「4 socket × 10 核 × 2 超线程」的机器，「128G / 16 实例」用这个优化约 4 秒，不优化约 10 秒）；非 Linux 固定为 4。

随后是一个外层循环，每批最多 `n_cores` 个实例：

```
for (i = 0; i < n_instances; /* no op */) {
  ulint n = i + n_cores;
  if (n > n_instances) {
    n = n_instances;
  }
  std::vector<std::thread> threads;
  std::mutex m;
  for (ulint id = i; id < n; ++id) {
    threads.emplace_back(std::thread(buf_pool_create, &buf_pool_ptr[id], size,
                                     id, &m, std::ref(errs[id])));
  }
  for (ulint id = i; id < n; ++id) {
    threads[id - i].join();
    if (errs[id] != DB_SUCCESS) {
      err = errs[id];
    }
  }
  ...
  i = n;
}
```

每个实例由一个 `std::thread` 调用 `buf_pool_create()` 创建，错误码写入 `errs[id]`（一个预先 `assign(n_instances, DB_SUCCESS)` 的 `std::vector<dberr_t>`）。批内线程全部 `join()` 后才检查错误。

**第五步，失败路径：逐实例回滚后返回错误。**

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

这是本机制里**最重要的一条降级/回退路径**：任何一个实例创建失败，就地调用 `buf_pool_free_instance()` 把**本批已建成的实例**释放掉（判据是 `buf_pool_ptr[id].chunks != nullptr`，即该实例已经分配过 chunk 数组——`buf_pool_create()` 在 chunk 分配失败时会把 `buf_pool->chunks` 置回 `nullptr` 后返回 `DB_ERROR`），然后调 `buf_pool_free()` 释放全局结构（`buf_stat_per_index`、`buf_chunk_map_reg`、`buf_pool_ptr` 数组本身），最后把错误码原样返回。**注意这个回滚只清理本批（`id` 到 `n`），此前批次已成功创建的实例不会被这个循环触及**——它们已随 `buf_pool_ptr` 一起被 `buf_pool_free()` 释放了实例数组，但实例内部的 chunk 与互斥量不会被 `buf_pool_free()` 释放（`buf_pool_free()` 只做 `ut::delete_` / `ut::free` 三件事）。

**第六步，创建成功后的全局收尾。**

```
buf_pool_set_sizes();
buf_LRU_old_ratio_update(100 * 3 / 8, false);
btr_search_sys_create(buf_pool_get_curr_size() / sizeof(void *) / 64);
buf_stat_per_index = ut::new_withkey<buf_stat_per_index_t>(...);
return (DB_SUCCESS);
```

- `buf_pool_set_sizes()` 遍历 `srv_buf_pool_instances` 个实例，把每个实例的 `curr_pool_size` 累加成 `curr_size`；若 `srv_buf_pool_curr_size == 0`（首次启动）就取这个累加值，否则取 `srv_buf_pool_size`；并把 `srv_buf_pool_old_size` 与 `srv_buf_pool_base_size` 都置为 `srv_buf_pool_size`，最后 `os_wmb`（写内存屏障）。
- `buf_LRU_old_ratio_update(100 * 3 / 8, false)`：**LRU 中点插入策略的 old 子列表占比设为 3/8**（`100 * 3 / 8` 是源码里的整除写法，结果为 37）。这属于实例内部的 LRU 链表（H03），此处只是初始化时设置一次全局比例。
- `btr_search_sys_create(buf_pool_get_curr_size() / sizeof(void *) / 64)`：**自适应哈希索引（AHI）哈希表大小按「缓冲池总页数 / 指针大小 / 64」计算**，那个 64 是源码里的除数常量。

### `buf_pool_create()`：单个实例长什么样

`buf_pool_create(buf_pool_t *buf_pool, ulint buf_pool_size, ulint instance_no, std::mutex *mutex, dberr_t &err)` 是每个实例的构造函数体，按源码注释分成两大段：

**第 1 段「Initialize general fields」：**

- **Linux 下绑核**：用 `CPU_ZERO` / `CPU_SET(instance_no % n_cores, &cpuset)` 把创建线程绑到第 `instance_no % 核数` 个 CPU 上，然后 `pthread_setaffinity_np()`；失败则报 `ER_IB_ERR_SCHED_SETAFFNINITY_FAILED`。随后 `setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), -20)` 把该线程优先级提到 -20（源码注释：Linux 允许给不同线程设不同设置，值得一试提高这个线程的优先级）。
- **建互斥量**：`mutex_create` 依次建 `chunks_mutex`、`LRU_list_mutex`、`free_list_mutex`、`zip_free_mutex`、`zip_hash_mutex`、`zip_mutex`、`flush_state_mutex`。
- **分配 chunk**：`ut_ad(buf_pool_size % srv_buf_pool_chunk_unit == 0)` 断言实例大小是 chunk 单位的整数倍，然后 `buf_pool->n_chunks = buf_pool_size / srv_buf_pool_chunk_unit`，`chunk_size = srv_buf_pool_chunk_unit`，`buf_pool->chunks` 用 `ut::zalloc_withkey` 分配。
- **初始化链表**：`UT_LIST_INIT` 依次初始化 `LRU`、`free`、`withdraw`、`flush_list`、`unzip_LRU`（以及调试构建下的 `zip_clean`），`buf_pool->withdraw_target = 0`；压缩页空闲链表 `zip_free[i]` 按 `UT_ARR_SIZE(buf_pool->zip_free)` 逐个初始化。
- **chunk 初始化循环**：`buf_chunk_init(buf_pool, chunk, chunk_size, mutex)` 逐 chunk 建立页框。若某个 chunk 初始化失败，函数体**逆序回滚**已建好的 chunk（逐块 `mutex_free(&block->mutex)` / `rw_lock_free(&block->lock)` / `buf_pool->deallocate_chunk(chunk)`），`ut::free(buf_pool->chunks)` 并把 `buf_pool->chunks` 置为 `nullptr`，`err = DB_ERROR` 后返回。**这个 `chunks == nullptr` 的状态正是 `buf_pool_init()` 回滚时用来判断「该实例是否需要释放」的判据。**
- **实例身份与预读区大小**：
  ```
  buf_pool->instance_no = instance_no;
  buf_pool->read_ahead_area = static_cast<page_no_t>(
      std::min(BUF_READ_AHEAD_PAGES,
               ut_2_power_up(buf_pool->curr_size / BUF_READ_AHEAD_PORTION)));
  buf_pool->curr_pool_size = buf_pool->curr_size * UNIV_PAGE_SIZE;
  ```
  `BUF_READ_AHEAD_PAGES` 与 `BUF_READ_AHEAD_PORTION` 都是 `buf0buf.cc` 文件顶部的 `static const ulint`：**前者为 64，后者为 32**（注释：read-ahead 缓冲最多可占用缓冲池的 1/32 部分）。`ut_2_power_up()` 把值向上取整到 2 的幂，所以预读区大小始终是 2 的幂。**每个实例有自己的 `read_ahead_area`**，因为它由该实例自己的 `curr_size` 算出。
- **哈希表**：
  ```
  srv_n_page_hash_locks = static_cast<ulong>(ut_2_power_up(srv_n_page_hash_locks));
  ut_a(srv_n_page_hash_locks != 0);
  ut_a(srv_n_page_hash_locks <= MAX_PAGE_HASH_LOCKS);
  buf_pool->page_hash = ib_create(2 * buf_pool->curr_size, LATCH_ID_HASH_TABLE_RW_LOCK,
                                  srv_n_page_hash_locks, MEM_HEAP_FOR_PAGE_HASH);
  buf_pool->zip_hash = ut::new_<hash_table_t>(2 * buf_pool->curr_size);
  ```
  源码注释写明「保护 page_hash 的锁数必须是 2 的幂」，因此用 `ut_2_power_up()` 向上取整；上限是常量 `MAX_PAGE_HASH_LOCKS`。**两张哈希表的桶数都是 `2 * curr_size`**——每页平均两个桶。**注意 `srv_n_page_hash_locks` 是全局变量，被每个实例的 `buf_pool_create()` 反复向上取整，因此所有实例最终使用同一个（取整后的）锁数。**
- **观察哨兵数组**：`buf_pool->watch` 用 `ut::zalloc_withkey` 分配 `sizeof(*buf_pool->watch) * BUF_POOL_WATCH_SIZE` 个元素，并逐个把 `buf_pool->watch[i].buf_pool_index = buf_pool->instance_no`——**哨兵记住了自己属于哪个实例**。
- **脏页跟踪默认关闭**：`buf_pool->track_page_lsn = LSN_MAX;`（源码注释「Dirty Page Tracking is disabled by default.」）。

**第 2 段「Initialize flushing fields」：**

- `mutex_create(LATCH_ID_FLUSH_LIST, &buf_pool->flush_list_mutex)`；
- `for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) buf_pool->no_flush[i] = os_event_create();`——**每个实例有自己的一组 `no_flush` 事件**，事件个数由 `BUF_FLUSH_N_TYPES` 决定；
- 构造三个 hazard pointer 与两个迭代器：`flush_hp`、`oldest_hp`（用 `FlushHp`，绑定 `flush_list_mutex`）、`lru_hp`（用 `LRUhp`，绑定 `LRU_list_mutex`）、`lru_scan_itr` 与 `single_scan_itr`（用 `LRUItr`，绑定 `LRU_list_mutex`）。

**这是「多实例减少互斥竞争」的物理基础：`LRU_list_mutex`、`free_list_mutex`、`zip_mutex` 这些最热的互斥量是每实例一份的。**

### `buf_pool_free_instance()`：单个实例的释放顺序

`buf_pool_free_instance(buf_pool_t *buf_pool)` 的函数体按固定顺序释放，顺序本身是有讲究的：

1. **先释放 7 个互斥量**：`LRU_list_mutex`、`free_list_mutex`、`zip_free_mutex`、`zip_hash_mutex`、`flush_state_mutex`、`zip_mutex`、`flush_list_mutex`。
2. **清理 LRU 链表上的页描述符**：从链表尾部往前遍历（`UT_LIST_GET_LAST` → `UT_LIST_GET_PREV`），对每个页断言它在文件里（`buf_page_in_file(bpage)`）且在 LRU 链表上（`bpage->in_LRU_list`）；若其状态不是 `BUF_BLOCK_FILE_PAGE`，则断言状态只能是 `BUF_BLOCK_ZIP_PAGE` 或 `srv_fast_shutdown == 2`（即快速关闭模式），然后调 `buf_page_free_descriptor(bpage)` 释放描述符。**注意：处于 `BUF_BLOCK_FILE_PAGE` 状态的页在此不做释放**——它们的页框由 chunk 释放路径回收。
3. **释放观察哨兵数组**：`ut::free(buf_pool->watch); buf_pool->watch = nullptr;`。
4. **释放 chunk**：进入 `chunks_mutex`，从最后一个 chunk 往前遍历，对每个块 `mutex_free(&block->mutex)` 与 `rw_lock_free(&block->lock)`（调试构建下还有 `rw_lock_free(&block->debug_latch)`），然后调 `buf_pool->deallocate_chunk(chunk)`；循环结束后 `ut::free(buf_pool->chunks)`，退出 `chunks_mutex`，最后 `mutex_free(&buf_pool->chunks_mutex)`。
5. **销毁刷脏事件**：`for (ulint i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; ++i) os_event_destroy(buf_pool->no_flush[i]);`。
6. **释放两张哈希表**：`ha_clear(buf_pool->page_hash)` 后 `ut::delete_(buf_pool->page_hash)`；`ut::delete_(buf_pool->zip_hash)`。

**它的两个真实调用方：**

- **`buf_pool_init()` 的错误回滚**（上文第五步）：只有 `buf_pool_ptr[id].chunks != nullptr` 的实例才会被释放——因为实例创建失败时 `buf_pool_create()` 已把 `chunks` 置 `nullptr`，那时的实例只有互斥量与哈希表需要释放，而 `buf_pool_free_instance()` 里的 chunk 遍历依赖 `buf_pool->n_chunks` 与 `buf_pool->chunks` 一致。
- **`buf_pool_free_all()`**（`buf_pool_free_all()` 遍历 `srv_buf_pool_instances`，逐个调 `buf_pool_free_instance(ptr)`，最后调 `buf_pool_free()`）——这是**正常关闭路径**上的调用方。

### `buf_pool_invalidate_instance()`：单个实例的作废与复位

`buf_pool_invalidate_instance(buf_pool_t *buf_pool)` 的函数体分四段：

1. **断言与加锁**：断言当前线程不持有 `LRU_list_mutex`（`ut_ad(!mutex_own(&buf_pool->LRU_list_mutex))`），然后 `mutex_enter(&buf_pool->flush_state_mutex)`。
2. **等待刷脏批次结束**：遍历 `i` 从 `BUF_FLUSH_LRU` 到 `BUF_FLUSH_N_TYPES`（不含），对每种刷脏类型断言 `buf_pool->init_flush[i] == false`（源码注释说明：此函数在启动期与恢复期被调用，此时 InnoDB 除 IO 辅助线程外是单线程的，不会再有新的写批次处于初始化阶段）；若 `buf_pool->n_flush[i] > 0`，就退出 `flush_state_mutex`、调 `buf_flush_wait_batch_end(buf_pool, type)` 等待该批结束、再重新进入 `flush_state_mutex`。**源码注释解释了为什么必须等：之前投递的写批可能尚未完成，缓冲池作废要继续就必须确保没有写活动在进行。**
3. **扫描并释放所有块**：`while (buf_LRU_scan_and_free_block(buf_pool, true)) { }`——反复扫描直到再也释放不出块为止。
4. **复位链表与统计**：进入 `LRU_list_mutex`，断言 `UT_LIST_GET_LEN(buf_pool->LRU) == 0` 且 `UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0`，然后把 `buf_pool->freed_page_clock = 0`、`buf_pool->LRU_old = nullptr`、`buf_pool->LRU_old_len = 0`；退出锁后 `buf_pool->stat.reset()` 并调 `buf_refresh_io_stats(buf_pool)`（后者把 `last_printout_time` 设为当前时刻并把 `stat` 拷到 `old_stat`）。

**它的唯一真实调用方是 `buf_pool_invalidate()`**——后者遍历 `srv_buf_pool_instances`，逐个调 `buf_pool_invalidate_instance(buf_pool_from_array(i))`。而 `buf_pool_invalidate()` 的调用方是 `storage/innobase/log/log0recv.cc` 的 `recv_apply_hashed_log_recs(log_t &log, bool allow_ibuf)`：在 `!allow_ibuf` 分支里，函数体先取 `recv_sys->writer_mutex` 停住 recv_writer 线程的 LRU 刷脏批次、`buf_flush_wait_LRU_batch_end()` 等当前批次结束、设置 `recv_sys->flush_type = BUF_FLUSH_LIST` 并触发一次 flush，等到 `recv_sys->flush_end` 之后才调 `buf_pool_invalidate()`，最后释放 `recv_sys->writer_mutex`。源码注释写明这一步的意图是「Flush all the file pages to disk and invalidate them in the buffer pool」。

### 实例数 `srv_buf_pool_instances` 在别处被怎么用

`srv_buf_pool_instances` 是系统变量 `innodb_buffer_pool_instances` 的运行时值，在本地稀疏检出可读到的文件里有多处真实使用：

- **`buf0buf.cc` 的 `buf_block_alloc()`**：当调用方传入 `buf_pool == nullptr` 时，函数体用 `index = buf_pool_index++ % srv_buf_pool_instances` 做**轮转分配**，注释写明「We are allocating memory from any buffer pool, ensure we spread the grace on all buffer pool instances」。**这与 `buf_pool_get()` 的「一表空间一实例」是两条互补的归属路径**：页的读入走表空间归属，需要新页框时走轮转。**
- **`buf0buf.cc` 的 `buf_pool_resize()`**：`new_instance_size = srv_buf_pool_size / srv_buf_pool_instances;` 然后 `new_instance_size /= UNIV_PAGE_SIZE;`——**扩缩容时每个实例的目标页数也是按实例数平分**。函数体随后逐实例设置 `curr_size` 与 `n_chunks_new`、按实例设置 `withdraw_target`、必要时逐实例调 `buf_pool_resize_hash(buf_pool)` 重建哈希表（触发条件是 `new_size_too_diff`，即 `srv_buf_pool_base_size > srv_buf_pool_size * 2 || srv_buf_pool_base_size * 2 < srv_buf_pool_size`），最后逐实例释放 `chunks_mutex`、`flush_state_mutex`、`zip_hash_mutex`、`free_list_mutex`、`zip_free_mutex`、`LRU_list_mutex` 与页哈希表的全部 X 锁，并置 `buf_pool_resizing = false`。
- **`buf0dblwr.cc` 的 `dblwr::open()`**：`Double_write::s_n_instances = std::max(4UL, srv_buf_pool_instances * 2);`——**double write buffer 的实例数是缓冲池实例数的两倍，下限 4**。源码注释写明理由是「Separate instances for LRU and FLUSH list write requests.」，即 LRU 与 FLUSH 两条写路径各自独立。
- **`buf0flu.cc` 的 `buf_flush_page_cleaner_init()`**：`page_cleaner->n_slots = static_cast<ulint>(srv_buf_pool_instances);`——**page cleaner 的槽位数等于缓冲池实例数**，每个实例一个槽。

## 关键取舍

- **源码事实：实例数在 `buf_pool_init()` 里被三条断言锁死，其中 `n_instances == srv_buf_pool_instances` 是最要紧的一条。** 全代码库遍历实例时用的循环上界一律是 `srv_buf_pool_instances`（`buf_get_total_list_len()`、`buf_get_total_stat()`、`buf_pool_set_sizes()`、`buf_pool_invalidate()`、`buf_pool_free_all()`、`buf_pool_clear_hash_index()`、`buf_pool_get_oldest_modification_approx()` 无一例外）。收益是「实例数组长度」与「遍历上界」不会错位；代价是 `buf_pool_init()` 必须由持有系统变量真实值的调用方驱动，不能传一个「临时算出来的」实例数。

- **源码事实：实例是按 CPU 核数分批并行创建的，批大小在 Linux 上被截断到 8。** 源码注释给出了实测依据：「128G / 16 实例」在 4 socket × 10 核 × 2 超线程的机器上，用这个优化约 4 秒、不用约 10 秒。收益是启动时间对实例数不敏感；代价是实例创建不是完全并行，超过 8 个实例时分多批串行。

- **源码事实：实例创建失败时的回滚只覆盖当前批次。** `buf_pool_init()` 的错误分支只对 `id` 到 `n` 这一批调 `buf_pool_free_instance()`，然后调 `buf_pool_free()` 释放全局结构。**回滚到 `buf_pool_free()` 的调用方是 `buf_pool_init()` 自身**；`buf_pool_free()` 只做 `ut::delete_(buf_stat_per_index)`、`ut::delete_(buf_chunk_map_reg)`、`ut::free(buf_pool_ptr)` 三件事——**它不释放各实例内部的 chunk 与哈希表**。这意味着失败路径上，先前批次已创建的实例存在资源未完全回收的窗口。**架构推论：** 这是「实例批量创建 + 部分失败回滚」这种模式的固有复杂度：批次边界与回滚边界不一致。

- **源码事实：每个实例的 `read_ahead_area` 是独立算出来的，用的是同一对常量。** `read_ahead_area = min(BUF_READ_AHEAD_PAGES, ut_2_power_up(curr_size / BUF_READ_AHEAD_PORTION))`，`BUF_READ_AHEAD_PAGES = 64`、`BUF_READ_AHEAD_PORTION = 32` 都定义在 `buf0buf.cc` 文件顶部。因为所有实例大小相同（都是 `total_size / n_instances`），各实例的 `read_ahead_area` 实际相等；用 `ut_2_power_up()` 向上取整到 2 的幂保证对齐运算可用。

- **源码事实：页哈希表的锁数 `srv_n_page_hash_locks` 是全局的，不是每实例一份。** `buf_pool_create()` 里对它做 `ut_2_power_up()` 向上取整（源码注释：保护 page_hash 的锁数必须是 2 的幂），并断言不超过 `MAX_PAGE_HASH_LOCKS`。因为每个实例都被创建一遍，这个取整是幂等的，最终所有实例共用同一个锁数。收益是锁位图可以全局共享一套编号；代价是实例数多时，**每个实例的哈希表都会用掉同样多的锁**——这正是「多实例减少竞争」的效果来源：不同实例的哈希锁互不重叠。

- **源码事实：`buf_pool_invalidate_instance()` 必须先等刷脏批次结束，再作废页。** 它在 `flush_state_mutex` 保护下检查 `buf_pool->n_flush[i] > 0`，若有则退出锁调 `buf_flush_wait_batch_end(buf_pool, type)`，再重新进锁。源码注释明确写出前置条件：「For buffer pool invalidation to proceed we must ensure there is NO write activity happening.」收益是作废动作不会与正在进行的写批撞车；代价是作废的延迟取决于当前写批何时结束。

- **源码事实：`buf_pool_invalidate_instance()` 结束时对链表长度的断言是对「前面那个等待 + 扫描循环做对了」的验收。** `ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0)` 与 `ut_ad(UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0)` 断言在 `while (buf_LRU_scan_and_free_block(buf_pool, true))` 循环退出后，LRU 与 unzip_LRU 都已清空。收益是调试构建能立刻发现「扫描一轮不够」；代价是调试构建每实例多两次链表长度读。

- **源码事实：多实例的收益不只来自哈希表，还来自「谁能用同一个 LRU 链表」这一层。** `LRU_list_mutex`、`free_list_mutex`、`zip_mutex`、`flush_list_mutex` 都是每实例一份，而 `buf_pool->watch` 数组在 `buf_pool_create()` 里逐项写入 `buf_pool->watch[i].buf_pool_index = buf_pool->instance_no`，说明连观察哨兵这种「跨线程协作对象」都被绑到了具体实例上。**架构推论：** 8.0 把缓冲池从「单实例」改为「多实例」是一次「把全局锁拆成实例级锁」的重构；`buf_pool_get()` 的表空间级映射保证了页不会跨实例，因此每个实例的 LRU 链表可以独立维护而不会漏页。

- **架构推论：** 多实例的代价是「全局视图」必须靠遍历所有实例拼出来。`buf_get_total_list_len()` 遍历 `srv_buf_pool_instances` 累加各实例的 `LRU` / `free` / `flush_list` 长度；`buf_get_total_stat()` 遍历各实例累加各项计数；`buf_pool_get_oldest_modification_approx()` 遍历各实例在 `flush_list_mutex` 保护下找最老的修改 LSN；`buf_pool_invalidate()` 遍历各实例逐个作废。**这意味着任何「全局」统计或全局操作都是 O(实例数) 的，且必须逐实例加各自的锁**——这既是多实例省下来的锁竞争，又立刻在跨实例操作上还回去。主核若引入分片缓存，同样要面对「分片省锁、全局视图还债」这一对矛盾。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_init` | 缓冲池实例数组的创建入口：断言 `n_instances > 0`、`n_instances <= MAX_BUFFER_POOLS`、`n_instances == srv_buf_pool_instances`；按 `total_size / n_instances` 平分大小；`ut::zalloc_withkey` 分配全局 `buf_pool_ptr` 数组；按 CPU 核数（Linux 上 `sysconf(_SC_NPROCESSORS_ONLN)` 截断到 8，非 Linux 为 4）分批起线程调 `buf_pool_create()`；失败时逐实例调 `buf_pool_free_instance()` 并调 `buf_pool_free()` 后返回错误；成功后调 `buf_pool_set_sizes()`、`buf_LRU_old_ratio_update(100 * 3 / 8, false)`、`btr_search_sys_create(buf_pool_get_curr_size() / sizeof(void *) / 64)` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_create` | 单实例构造函数：Linux 下绑核（`CPU_SET(instance_no % n_cores)` + `pthread_setaffinity_np` + `setpriority(..., -20)`）；建 7 个实例级互斥量；按 `srv_buf_pool_chunk_unit` 切 chunk 并逐块 `buf_chunk_init()`，失败则逆序回滚并把 `chunks` 置 `nullptr`、返回 `DB_ERROR`；算 `read_ahead_area = min(BUF_READ_AHEAD_PAGES, ut_2_power_up(curr_size / BUF_READ_AHEAD_PORTION))`；`srv_n_page_hash_locks` 向上取 2 的幂并断言不超过 `MAX_PAGE_HASH_LOCKS`；建 `page_hash` 与 `zip_hash`（桶数均为 `2 * curr_size`）；分配 `BUF_POOL_WATCH_SIZE` 个观察哨兵并写入 `instance_no`；`track_page_lsn = LSN_MAX` 关闭脏页跟踪；建 `no_flush` 事件与三个 hazard pointer、两个迭代器 |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_free_instance` | 单实例释放：先释放 7 个互斥量；从 LRU 链表尾部往前清理非 `BUF_BLOCK_FILE_PAGE` 的页描述符（断言其状态只能是 `BUF_BLOCK_ZIP_PAGE` 或 `srv_fast_shutdown == 2`）；释放 `watch` 数组；逐 chunk 释放块互斥量与读写锁并 `deallocate_chunk()`；销毁 `BUF_FLUSH_LRU..BUF_FLUSH_N_TYPES` 的 `no_flush` 事件；`ha_clear` + `ut::delete_` 释放 `page_hash` 与 `zip_hash` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_invalidate_instance` | 单实例作废与复位：断言不持 `LRU_list_mutex`；持 `flush_state_mutex` 逐刷脏类型检查 `n_flush[i] > 0`，非零则退出锁调 `buf_flush_wait_batch_end()` 等批次结束后重新进锁；`while (buf_LRU_scan_and_free_block(buf_pool, true))` 扫空全部块；断言 `LRU` 与 `unzip_LRU` 长度均为 0；复位 `freed_page_clock`、`LRU_old`、`LRU_old_len`；`stat.reset()` 与 `buf_refresh_io_stats()` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_invalidate` | `buf_pool_invalidate_instance()` 的唯一调用方：遍历 `srv_buf_pool_instances`，逐个调 `buf_pool_invalidate_instance(buf_pool_from_array(i))` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_free_all` | 正常关闭路径：遍历 `srv_buf_pool_instances`，逐个调 `buf_pool_free_instance(ptr)`，最后调 `buf_pool_free()` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_free` | 释放全局结构：`ut::delete_(buf_stat_per_index)`、`ut::delete_(buf_chunk_map_reg)`、`ut::free(buf_pool_ptr)` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_set_sizes` | 实例创建后的容量汇总：遍历 `srv_buf_pool_instances` 累加各实例 `curr_pool_size`；`srv_buf_pool_curr_size` 为 0 时取累加值，否则取 `srv_buf_pool_size`；把 `srv_buf_pool_old_size` 与 `srv_buf_pool_base_size` 置为 `srv_buf_pool_size`；最后 `os_wmb` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_ptr` | 全局实例数组 `buf_pool_t *buf_pool_ptr`，长度由 `buf_pool_init()` 的 `n_instances` 决定；各实例通过 `buf_pool_from_array(i)` 访问 |
| `storage/innobase/buf/buf0buf.cc` | `buf_block_alloc` | 需要新页框但未指定实例时的轮转分配路径：`index = buf_pool_index++ % srv_buf_pool_instances` 后 `buf_pool_from_array(index)`，注释说明目的是把负载分散到所有实例 |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_resize` | 在线扩缩容：按 `srv_buf_pool_size / srv_buf_pool_instances / UNIV_PAGE_SIZE` 算出每实例新页数，逐实例设置 `curr_size` 与 `n_chunks_new`、按实例设 `withdraw_target`；必要时逐实例调 `buf_pool_resize_hash()`；逐实例释放各互斥量与页哈希表 X 锁；最后按 `new_size_too_diff` 决定是否连带调整 `lock_sys`、`btr_search_sys`、`dict_sys` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_get` | 页 → 实例的路由函数：被 `Buf_fetch` 构造函数用于确定 `m_buf_pool`，被 `buf_page_init()` 用 `ut_ad(buf_pool == buf_pool_get(page_id))` 校验「同一表空间的页必须落在同一实例」这一硬约束 |
| `storage/innobase/buf/buf0buf.cc` | `buf_get_total_list_len` | 跨实例全局视图：遍历 `srv_buf_pool_instances`，累加各实例 `LRU`、`free`、`flush_list` 的链表长度 |
| `storage/innobase/buf/buf0buf.cc` | `buf_get_total_stat` | 跨实例全局统计：遍历 `srv_buf_pool_instances`，把各实例 `stat` 的取页数、读页数、写页数、LRU 老化计数等累加进 `tot_stat` |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_get_oldest_modification_approx` | 跨实例求最老修改 LSN：遍历各实例，在该实例 `flush_list_mutex` 保护下从 `flush_list` 尾部往前找，跳过系统临时表空间的页 |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_clear_hash_index` | 清空全部实例的自适应哈希索引：遍历 `srv_buf_pool_instances`，对每个实例的每个 chunk 的每个块调 `block->ahi.validate()` |
| `storage/innobase/buf/buf0buf.cc` | `BUF_READ_AHEAD_PAGES` / `BUF_READ_AHEAD_PORTION` | 文件顶部的两个 `static const ulint` 常量，取值分别为 64 与 32；前者是预读页数上限，后者是「预读区最多可占缓冲池的 1/32 部分」的分母 |
| `storage/innobase/buf/buf0buf.cc` | `buf_pool_should_madvise` | 静态布尔量，初值 `false`；注释说明在 `buf_pool_init()` 开头初始化，之后随 `@@global.innodb_buffer_pool_in_core_file` 变化由 `buf_pool_update_madvise()` 更新，且更新时需持有全部实例的 `chunks_mutex` |
| `storage/innobase/buf/buf0dblwr.cc` | `dblwr::open` | double write buffer 的实例数由缓冲池实例数派生：`Double_write::s_n_instances = std::max(4UL, srv_buf_pool_instances * 2)`，注释写明是为了给 LRU 与 FLUSH 两条写路径各自独立实例 |
| `storage/innobase/buf/buf0flu.cc` | `buf_flush_page_cleaner_init` | page cleaner 的槽位数直接等于缓冲池实例数：`page_cleaner->n_slots = static_cast<ulint>(srv_buf_pool_instances)`，每个实例一个槽 |
| `storage/innobase/log/log0recv.cc` | `recv_apply_hashed_log_recs` | `buf_pool_invalidate()` 的真实调用方：`!allow_ibuf` 分支里取 `recv_sys->writer_mutex` 停住 recv_writer 的 LRU 刷批、`buf_flush_wait_LRU_batch_end()` 等当前批次结束、触发一次 `BUF_FLUSH_LIST` 刷脏并等 `recv_sys->flush_end`，之后才调 `buf_pool_invalidate()` |
| `storage/innobase/buf/buf0lru.cc` | `buf_LRU_free_page` | 实例内部的 LRU 淘汰实现（属 H03）：被 `buf_pool_free_instance()` 之外的 LRU 路径调用，是「页从某实例的 LRU 链表移出」的入口 |

## 与主核 / AI 的关系

主核（`engine/db`）的存储层目前是单缓冲形态：`engine/db/storage/` 下的 pager 直接管理自己的页帧，没有「一个逻辑缓冲池切成多个独立实例」这层结构，也没有「表空间 → 实例」的归属映射。

**架构推论：** InnoDB 的多实例设计里有一条硬约束值得主核借鉴——**归属映射的粒度必须与「哪些对象会共享同一份索引结构」对齐**。InnoDB 选的是表空间级，因为同一表空间的页要共享同一棵 B-tree 树结构与同一套哈希锁；而 `buf_page_init()` 里那句 `ut_ad(buf_pool == buf_pool_get(page_id))` 就是把这条约束写成了断言——**用断言把架构约束钉在代码里，而不是只写在文档里**。主核若要把页缓存分片，同样的做法是：先确定「哪些页必须共享同一个索引结构」，再用一条断言保证分片函数遵守它。

**架构推论：** `buf_pool_init()` 的失败回滚路径暴露了「批量并行创建 + 部分失败」这类模式的固有复杂度——**批次边界与回滚边界不一致**（回滚只覆盖当前批次，先前批次已建成的实例只被 `buf_pool_free()` 释放了实例数组本身）。主核若要并行初始化多个分片缓存，同样会遇到「部分分片建好后失败」的清理问题；InnoDB 的做法给出了两条可直接照搬的原则：**（1）让失败点自身留下可判定的状态标记**（这里是 `buf_pool->chunks == nullptr`），**（2）把「释放单个分片」与「释放全局结构」拆成两个函数**（`buf_pool_free_instance()` 与 `buf_pool_free()`）。

**架构推论：** 多实例把「热路径锁」从全局降到了实例级，但立刻在「全局视图」上还债——`buf_get_total_list_len()`、`buf_get_total_stat()`、`buf_pool_get_oldest_modification_approx()` 三处都要遍历所有实例并逐实例加锁。对 AI 参与的负载，这一点有直接含义：AI 生成的 SQL 常带「先跑一个聚合统计、再跑若干点查」的混合形态，聚合统计恰好落在需要遍历全局视图的那条路径上，而点查落在实例级热路径上。**主核在设计分片缓存时，应当把「全局聚合视图」的代价显式建模（例如维护每分片的增量计数，避免每次全遍历），否则 AI 负载里占比不高的聚合语句会吃掉分片带来的收益。**

## 参考

- [MySQL 源码：`storage/innobase/buf/buf0buf.cc`（mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/buf/buf0buf.cc) —— `buf_pool_init()`、`buf_pool_create()`、`buf_pool_free_instance()`、`buf_pool_invalidate_instance()`、`buf_pool_invalidate()`、`buf_pool_free_all()`、`buf_pool_set_sizes()`、`buf_pool_resize()` 的完整实现，以及文件顶部的 `BUF_READ_AHEAD_PAGES`（64）与 `BUF_READ_AHEAD_PORTION`（32）常量。
- [MySQL 源码：`storage/innobase/buf/buf0dblwr.cc`（mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/buf/buf0dblwr.cc) —— `dblwr::open()` 里 `Double_write::s_n_instances = std::max(4UL, srv_buf_pool_instances * 2)`：缓冲池实例数如何派生 double write buffer 的实例数。
- [MySQL 源码：`storage/innobase/buf/buf0flu.cc`（mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/buf/buf0flu.cc) —— `buf_flush_page_cleaner_init()` 里 `page_cleaner->n_slots = srv_buf_pool_instances`：刷脏线程槽位与缓冲池实例数的一一对应关系。
- [MySQL 源码：`storage/innobase/log/log0recv.cc`（mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/log/log0recv.cc) —— `recv_apply_hashed_log_recs()` 中的 `buf_pool_invalidate()` 调用点及其前置的刷脏批次等待逻辑，是 `buf_pool_invalidate_instance()` 的真实调用链。
- [MySQL 参考手册：17.8.3.2 Configuring Multiple Buffer Pool Instances（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-multiple-buffer-pools.html) —— 多缓冲池实例的官方说明：`innodb_buffer_pool_instances` 的默认值与最大值 64、该选项需配合 `innodb_buffer_pool_size` 不低于 1GB 才生效、「总大小在所有缓冲池之间分摊」、每个缓冲池实例各自管理 free list / flush list / LRU 等数据结构，以及「每个页被随机分配到某一个缓冲池，使用哈希函数」这一归属规则——与源码里 `buf_pool_get()` 做路由、`buf_pool_create()` 每实例建各自哈希表与链表的事实对应。
- [MySQL 参考手册：17.5.1 Buffer Pool（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-buffer-pool.html) —— 缓冲池的官方定义、LRU 中点插入策略（old 子列表默认占 3/8，与源码 `buf_LRU_old_ratio_update(100 * 3 / 8, false)` 对应），以及 `SHOW ENGINE INNODB STATUS` 中 `BUFFER POOL AND MEMORY` 段各项指标的含义。
