---
project: mysql-server
feature: lock-latch
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/lock/lock0latches.cc
    symbols: [Latches::Page_shards::get_shard, Latches::Table_shards::get_shard]
---

# 闩锁

## 一句话职责

`storage/innobase/lock/lock0latches.cc` 里的闩锁（latch）机制负责给 InnoDB 的锁系统提供**分片互斥量**：它把 lock_sys 的锁队列按两类键切成若干片——记录锁按页（`page_id`）归片、表锁按表（`table_id`）归片——每片一把 `Lock_mutex`，并提供 `Latches::Page_shards::get_shard()` 与 `Latches::Table_shards::get_shard()` 两个纯函数把「页 / 表」映射到片号，从而让不同页 / 不同表上的锁队列操作互不串行。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责保护内存结构的互斥闩（latch）与它们在页、缓冲池、字典结构上的使用；**不负责事务级的行锁（J02）**。

## 机制是什么

### 闩锁（latch）与行锁（lock）是两件不同的事

InnoDB 里「锁」这个词覆盖了两类互斥机制：

| | 闩锁 latch | 行锁 lock |
|---|---|---|
| 保护对象 | 内存数据结构（锁队列、哈希表、页缓冲） | 数据的逻辑可见性（记录、间隙） |
| 生命周期 | 持有几微秒到几毫秒，纯内存态 | 持有到事务提交或回滚 |
| 是否进 undo | 否 | 是（记录锁要写 undo） |
| 是否可被用户观测 | 否 | 是（`SHOW ENGINE INNODB STATUS`、`performance_schema.data_locks`） |

本页讲的是**前者**。行锁的加锁判定、等待队列、死锁检测属于 J02 页（`lock0lock.cc` 中的 `lock_rec_lock`、`lock_table` 等）。

### `Latches` 的三组闩

`lock0latches.cc` 里定义了三个分片容器，都放在 `namespace locksys` 下：

| 容器 | 键 | 每个分片持有的闩 | 用途 |
|---|---|---|---|
| `Latches::Page_shards` | `page_id` | `Lock_mutex` | 记录锁队列（`lock_sys->rec_hash`） |
| `Latches::Table_shards` | `table_id` | `Lock_mutex` | 表锁队列（`table->locks`） |
| `Latches::Unique_sharded_rw_lock` | — | 单个 `rw_lock` | 保护「每个分片恰好一个持有者」这类唯一性约束 |

前两个是**分片互斥量数组**，第三个是一个全局读写锁。

### `SHARDS_COUNT`：分片数量是编译期常量

两个分片容器都用同一个常量 `Latches::SHARDS_COUNT` 决定片数：

```
Latches::Page_shards::Page_shards() {
  for (size_t i = 0; i < SHARDS_COUNT; ++i) {
    mutex_create(LATCH_ID_LOCK_SYS_PAGE, mutexes + i);
  }
}
```

`Latches::Table_shards::Table_shards()` 的循环体完全对称，只是把 `LATCH_ID_LOCK_SYS_PAGE` 换成 `LATCH_ID_LOCK_SYS_TABLE`。

**注意：`SHARDS_COUNT` 的字面数值定义在 `storage/innobase/include/lock0latches.h` 中，该头文件不在本地稀疏检出范围内，因此本页不给出它的具体数值。** 页面上所有涉及片数的表述一律以 `SHARDS_COUNT` 这个符号本身指代。

### 页分片：为什么不能直接用哈希值取模

`Latches::Page_shards::get_shard()` 的完整函数体是：

```
size_t Latches::Page_shards::get_shard(const page_id_t &page_id) {
  /* We always use lock_sys->rec_hash regardless of the exact type of the
  lock. It may happen that the lock is a predicate lock, in which case, it would
  make more sense to use hash_calc_cell_id with proper hash table size. The
  current implementation works, because the size of all three hashmaps is always
  the same. This allows an interface with less arguments. */
  ut_ad(lock_sys->rec_hash.get_n_cells() == lock_sys->prdt_hash.get_n_cells());
  ut_ad(lock_sys->rec_hash.get_n_cells() ==
        lock_sys->prdt_page_hash.get_n_cells());
  /* We need a property that if two pages are mapped to the same bucket of the
  hash table, and thus their lock queues are merged, then these two lock queues
  are protected by the same shard. This is why to compute the shard we use the
  cell_id as the input and not the original lock_rec_hash_value's result. */
  return lock_sys->rec_hash.get_cell_id(lock_rec_hash_value(page_id)) %
         SHARDS_COUNT;
}
```

这里有四个要点，全部来自函数体本身：

**第一，取模的输入是 `cell_id` 而不是哈希值本身。** 源码注释写明了这个约束：「如果两个页被映射到哈希表的同一个桶（因而它们的锁队列被合并），那么这两个锁队列必须由同一片保护——这就是为什么计算分片时用 `cell_id` 作为输入，而不是 `lock_rec_hash_value` 的原始结果。」

哈希桶数与分片数不是同一个量；直接对 `lock_rec_hash_value(page_id)` 取模会得到一个与桶无关的值，无法保证「同桶必同片」。`Locks_hashtable::get_cell_id()` 把这个转换做实：

```
size_t Locks_hashtable::get_cell_id(uint64_t hash_value) {
  return hash_calc_cell_id(hash_value, ht.get());
}
```

**第二，三个哈希表共用 `rec_hash` 做桶计算。** 两条 `ut_ad` 断言（仅调试版本生效）保证 `rec_hash`、`prdt_hash`、`prdt_page_hash` 的 `get_n_cells()` 三者相等。函数体注释解释了当前实现的取舍：如果锁是谓词锁（predicate lock），按理应该用带正确哈希表尺寸的 `hash_calc_cell_id`；但因为三个哈希表尺寸始终相同，现在这种「一律用 `rec_hash`」的写法同样成立，好处是接口可以少一个参数。

**第三，分片是「页」粒度，不是「记录」粒度。** 输入是 `page_id`（空间号 + 页号），所以同一页上所有记录锁共用一片闩锁。`lock0lock.cc` 里所有按页取闩的调用点都传 `block->get_page_id()`，可佐证这一点。

**第四，结果对 `SHARDS_COUNT` 取模。** 桶数可以远大于片数（桶数随 `lock_sys_resize()` 变化），片数固定。

### 表分片：直接用 `table_id` 取模

`Latches::Table_shards::get_shard()` 简单得多：

```
size_t Latches::Table_shards::get_shard(const table_id_t table_id) {
  return table_id % SHARDS_COUNT;
}
```

**表分片不需要「cell_id 中转」这一层**，因为表锁队列不放在哈希表里——它直接挂在 `dict_table_t` 的 `locks` 成员上（见 `lock0iter.cc` 中 `for (lock_t *lock : table->locks)`）。没有桶合并问题，也就没有「同桶必同片」的要求。

### `get_mutex()`：从片号取闩，并提供 const / 非 const 两个版本

`Page_shards` 与 `Table_shards` 各有一组 `get_mutex()` 重载。`Page_shards` 的三个重载：

```
const Lock_mutex &Latches::Page_shards::get_mutex(const page_id_t &page_id) const {
  return mutexes[get_shard(page_id)];
}

Lock_mutex &Latches::Page_shards::get_mutex(const page_id_t &page_id) {
  /* See "Effective C++ item 3: Use const whenever possible" for explanation of
  this pattern, which avoids code duplication by reusing const version. */
  return const_cast<Lock_mutex &>(
      const_cast<const Latches::Page_shards *>(this)->get_mutex(page_id));
}

Lock_mutex &Latches::Page_shards::get_mutex(const uint64_t cell_id) {
  return mutexes[cell_id % SHARDS_COUNT];
}
```

前两个是标准的 const 正确性模式：非 const 版本复用 const 版本，再 `const_cast` 掉。源码注释明确指向 "Effective C++ item 3: Use const whenever possible"。

**第三个重载接受 `cell_id` 而不是 `page_id`**，直接 `cell_id % SHARDS_COUNT`。这是给「已经知道桶号」的场景用的——`lock0iter.cc` 里的 `Shard_naked_latch_guard` 就走这条路，见下文。

`Table_shards` 侧的对应重载多了一个直接吃 `dict_table_t` 的版本：

```
const Lock_mutex &Latches::Table_shards::get_mutex(
    const dict_table_t &table) const {
  return get_mutex(table.id);
}
```

`Table_shards::get_mutex(const table_id_t)` 与 `get_mutex(const dict_table_t &)` 之间的 const / 非 const 转换模式与 `Page_shards` 完全一致。

### `Unique_sharded_rw_lock`：保护「分片内唯一持有者」

```
Latches::Unique_sharded_rw_lock::Unique_sharded_rw_lock() {
  rw_lock.create(
#ifdef UNIV_PFS_RWLOCK
      lock_sys_global_rw_lock_key,
#endif
      LATCH_ID_LOCK_SYS_GLOBAL, 64);
}
```

构造函数创建一个 rw_lock， PFS 插桩 key 为 `lock_sys_global_rw_lock_key`（仅在 `UNIV_PFS_RWLOCK` 下传入），latch id 为 `LATCH_ID_LOCK_SYS_GLOBAL`，**名称长度为 64**。

配套的线程局部 shard id：

```
thread_local size_t Latches::Unique_sharded_rw_lock::m_shard_id{NOT_IN_USE};
```

`m_shard_id` 是 `thread_local`，初值 `NOT_IN_USE`——即「当前线程没有持有任何分片」。`Unique_sharded_rw_lock` 的析构函数调 `rw_lock.free()`。

**架构推论：** `m_shard_id` 用 `thread_local` 而不是普通成员，说明它记录的是「当前线程正持有哪个分片」，用于实现「同一时刻只有一个线程能成为某分片的唯一持有者」这一语义——这是一个**带所有权检查的锁**，而非单纯的互斥量。本页未检出 `lock0latches.h`，`m_shard_id` 的写入点与「唯一持有者」的具体判定逻辑不在可见范围内，故此处只标注为推论。

### 全局闩：`Global_shared_latch_guard` / `Global_exclusive_latch_guard`

`lock0lock.cc` 中大量出现 `locksys::Global_exclusive_latch_guard` 与 `locksys::Global_shared_latch_guard`，它们对应 `Latches` 上的全局闩（`owns_exclusive_global_latch()` / `owns_shared_global_latch()`）。

`locksys::owns_exclusive_global_latch()` / `owns_shared_global_latch()` 的定义（`#ifdef UNIV_DEBUG` 包裹，仅调试版本可用）：

```
bool owns_exclusive_global_latch() {
  return lock_sys->latches.owns_exclusive_global_latch();
}

bool owns_shared_global_latch() {
  return lock_sys->latches.owns_shared_global_latch();
}
```

以及按类型分派的两个：

```
bool owns_page_shard(const page_id_t &page_id) {
  return lock_sys->latches.owns_page_shard(page_id);
}

bool owns_table_shard(const dict_table_t &table) {
  return lock_sys->latches.owns_table_shard(table);
}
```

以及统一入口：

```
bool owns_lock_shard(const lock_t *lock) {
  if (lock->is_record_lock()) {
    return lock_sys->latches.owns_page_shard(lock->rec_lock.page_id);
  } else {
    return lock_sys->latches.owns_table_shard(*lock->tab_lock.table);
  }
}
```

**这就是本页机制的核心判据：记录锁归页片、表锁归表片，由锁的类型自动决定。**

## 怎么实现的

### 初始化：构造时逐片建闩，析构时逐片销毁

`Page_shards` 与 `Table_shards` 的构造 / 析构完全对称：

```
Latches::Page_shards::Page_shards() {
  for (size_t i = 0; i < SHARDS_COUNT; ++i) {
    mutex_create(LATCH_ID_LOCK_SYS_PAGE, mutexes + i);
  }
}

Latches::Page_shards::~Page_shards() {
  for (size_t i = 0; i < SHARDS_COUNT; ++i) {
    mutex_destroy(mutexes + i);
  }
}
```

`Table_shards` 用 `LATCH_ID_LOCK_SYS_TABLE` 做同样的事。**片数在构造期就被固定**——`mutexes` 是定长数组，不是运行时增长的容器。

创建路径是 `lock_sys_create()`（`lock0lock.cc`）：

```
void lock_sys_create(ulint n_cells) {
  ...
  lock_sys = static_cast<lock_sys_t *>(
      ut::zalloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, lock_sys_sz));

  new (lock_sys) lock_sys_t{n_cells};
  ...
}
```

`lock_sys_t{n_cells}` 的构造里创建 `Latches` 成员，`Latches` 的成员构造即触发上面两个循环。`n_cells` 是哈希表的初始桶数，与 `SHARDS_COUNT` 是两个独立的量。

### 记录锁路径：页片闩在何处被取

`lock0lock.cc` 中所有按页取闩的调用点都传 `block->get_page_id()`：

```
locksys::Shard_latch_guard guard{UT_LOCATION_HERE, block->get_page_id()};
```

这类 `Shard_latch_guard` 在 `lock0lock.cc` 中出现于页分裂 / 页合并 / 页废弃 / 记录加锁 / 记录解锁等路径，例如：

- `lock_rec_add_to_queue` 相关路径（记录入队）
- 页分裂与合并：`locksys::Shard_latches_guard guard{UT_LOCATION_HERE, *block, *new_block};`——一次同时取两片（分裂的左右两页、合并的供体页与受赠页）
- 记录加锁：`lock_rec_lock()` / `lock_rec_lock_slow()` 的入口断言 `ut_ad(locksys::owns_page_shard(block->get_page_id()));`

**「取两片」的 `Shard_latches_guard` 是本页机制里最需要注意的一点**：页分裂 / 合并会同时触及两个页片，因此必须按固定顺序取两片闩锁，否则两个并发分裂会交叉死锁。`lock0lock.cc` 中的调用点一律写成 `{*block, *new_block}`、`{*left_block, *root}`、`{*heir_block, *block}` 这种固定参数顺序，正是为了固定取锁顺序。

### 表锁路径：表片闩在何处被取

表锁队列的操作在 `lock0lock.cc` 里成对出现：

```
locksys::Shard_latch_guard table_latch_guard{UT_LOCATION_HERE, *table};
```

两处都在 `table->locks` 队列被读写的函数里：

1. `lock_table()`（在 `lock_table_has()` 判重之后、`lock_table_other_has_incompatible()` 判冲突之前）
2. `lock_table_ix_resurrect()`（给恢复的事务补 IX 锁）

`lock_table()` 的关键片段：

```
locksys::Shard_latch_guard table_latch_guard{UT_LOCATION_HERE, *table};

/* We have to check if the new lock is compatible with any locks
other transactions have in the table lock queue. */

wait_for = lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode);

trx_mutex_enter(trx);

if (wait_for != nullptr) {
  err = lock_table_enqueue_waiting(mode | flags, table, thr, wait_for);
} else {
  lock_table_create(table, mode | flags, trx);
  ...
  err = DB_SUCCESS;
}

trx_mutex_exit(trx);
```

**取表片闩的顺序是「先闩、后 `trx_mutex_enter`」**。这一顺序不是随意的：`lock0lock.cc` 中 `try_relatch_trx_and_shard_and_do()` 的注释写明了「latching order requires lock_sys latches to be taken before trx->mutex」——**先 lock_sys 闩，再 trx->mutex**。

### `lock_sys_resize()`：为什么必须拿全局排他闩

哈希表扩容会改变桶数，从而改变 `get_shard()` 的结果。`lock_sys_resize()` 因此必须先拿全局排他闩：

```
void lock_sys_resize(ulint n_cells) {
  /* We will rearrange locks between cells and change the parameters of hash
  function used in sharding of latches, so we have to prevent everyone from
  accessing lock sys queues, or even computing shard id. */
  locksys::Global_exclusive_latch_guard guard{UT_LOCATION_HERE};

  /* This invalidates All_lock_iterators */
  lock_sys->n_resizes++;

  lock_sys->rec_hash.resize(n_cells);
  ...
  lock_sys->prdt_hash.resize(n_cells);

  lock_sys->prdt_page_hash.resize(n_cells);
}
```

源码注释点明了两个后果：**（1）** 会在锁之间重新排列（rearrange locks between cells）；**（2）** 会改变分片所用的哈希函数参数（change the parameters of hash function used in sharding of latches）。因此注释要求「阻止所有人访问 lock sys 队列，甚至阻止计算 shard id」。

**这是本页机制存在全局排他闩的根本原因：分片结果依赖哈希表尺寸，而哈希表尺寸可变。**

`lock_sys->n_resizes++` 的作用是让已存在的迭代器失效——`lock0iter.cc` 的迭代器在开始时记下 `lock_sys->n_resizes`，中途比对，发现不一致就停止。

### `Locks_hashtable::resize()`：桶迁移的实现

```
void Locks_hashtable::resize(size_t n_cells) {
  auto new_ht = ut::new_<hash_table_t>(n_cells);
  HASH_MIGRATE(ht.get(), new_ht, lock_t, hash, lock_rec_lock_hash_value);
  ht.reset(new_ht);
  cells_in_use = Cells_in_use{ht->get_n_cells(),
                              ut::make_psi_memory_key(mem_key_lock_sys)};
  for (size_t i = 0; i < ht->get_n_cells(); ++i) {
    if (hash_get_first(ht.get(), i) != nullptr) {
      cells_in_use.set(i);
    }
  }
}
```

迁移用的哈希函数是：

```
static uint64_t lock_rec_lock_hash_value(const lock_t *lock) {
  return lock_rec_hash_value(lock->rec_lock.page_id);
}
```

**注意迁移键就是 `page_id` 的哈希值**，与 `get_shard()` 里 `lock_rec_hash_value(page_id)` 是同一个来源——这正是「同桶必同片」约束能成立的基础。

迁移后重建 `cells_in_use` 位图，逐桶检查 `hash_get_first()` 是否为空，非空的桶置位。

### `lock0iter.cc`：分片迭代器的实际用法

`All_locks_iterator::iterate_over_current_cell()` 是本页机制最完整的消费方之一。它按桶遍历哈希表，每取一片闩就处理该桶：

```
const size_t shard_id = m_bucket_id % locksys::Latches::SHARDS_COUNT;
/* We need to latch the shard of lock-sys which contains the locks from
hash_get_nth_cell(hash_table, m_bucket_id). We know that they must be in a
single shard, as otherwise lock-sys wouldn't be able to iterate over bucket.*/
locksys::Shard_naked_latch_guard shard_guard{UT_LOCATION_HERE, nullptr,
                                             m_bucket_id};
m_bucket_id = hash_table.find_set_in_this_shard(m_bucket_id);
if (m_bucket_id < hash_table.get_n_cells()) {
  hash_table.find_in_cell(m_bucket_id, [&](lock_t *lock) {
    std::forward<F>(f)(*lock);
    return false;
  });

  m_bucket_id += locksys::Latches::SHARDS_COUNT;
}
if (m_bucket_id < hash_table.get_n_cells()) {
  return true;
}
m_bucket_id = shard_id + 1;
return m_bucket_id != locksys::Latches::SHARDS_COUNT;
```

这段代码的推进逻辑值得逐句读：

- `shard_id = m_bucket_id % SHARDS_COUNT`：当前桶落在哪一片。
- `Shard_naked_latch_guard shard_guard{..., nullptr, m_bucket_id}`：**传 `nullptr` 而不是页对象**——走的是 `Page_shards::get_mutex(const uint64_t cell_id)` 那个重载，直接 `cell_id % SHARDS_COUNT`。这印证了上文「已知桶号时不必再算哈希」的重载存在的原因。
- `find_set_in_this_shard(m_bucket_id)`：找到本片内下一个有内容的桶。
- 处理完该桶后 `m_bucket_id += SHARDS_COUNT`：**跳过本片内的其余桶**（同一片的桶在同一个闩保护下，可以一起处理）。
- 片子走完后 `m_bucket_id = shard_id + 1`，并以 `m_bucket_id != SHARDS_COUNT` 判断是否全部片都走完。

**这一整套「桶 → 片」的映射，跟 `get_shard()` 里 `cell_id % SHARDS_COUNT` 是同一个公式。** 迭代器的正确性直接依赖 `get_shard()` 的「同桶必同片」性质。

同一文件里，表锁的遍历走表片：

```
dict_table_t *table = dd_table_open_on_id_in_mem(table_id, false);
if (table != nullptr) {
  {
    locksys::Shard_latch_guard table_latch_guard{UT_LOCATION_HERE, *table};
    for (lock_t *lock : table->locks) {
      std::forward<F>(f)(*lock);
    }
  }
  dd_table_close(table, nullptr, nullptr, false);
}
```

**记录锁按页片、表锁按表片，两条完全不同的取闩路径，在同一个迭代器里并存。**

迭代器的四个阶段（`stage_t`）是：`TABLE_LOCKS` → `PRDT_PAGE_LOCKS` → `PRDT_LOCKS` → `REC_LOCKS`。后三个阶段都走 `iterate_over_current_cell()`，第一个走 `iterate_over_current_table()`。

### 迭代器与扩容的竞态：源码注释给出的取舍

`iterate_over_current_cell()` 里有一段长注释，说明并发扩容时的行为：

```
if (m_lock_sys_n_resizes_at_the_beginning != lock_sys->n_resizes ||
    hash_table.get_n_cells() <= m_bucket_id) {
  return false;
}
```

注释说明：当前实现在 `lock_sys_resize()` 与迭代并发时**不会崩溃，而是返回不完整的数据**。理由是这样做「好过把某些锁报告两次（会违反主键约束）」。注释进一步限定：这个迭代器当前只用于 `performance_schema.data_locks` 与 `performance_schema.data_wait_locks`，两者都不保证视图一致性；源码作者认为这是「实现简单性与正确性之间的合理取舍」。注释还提到一个更复杂的替代方案——为 `lock_sys_resize()` 配一把专用 rwlock（写侧独占、迭代器构造时读侧共享、析构时释放），但作者认为「持有长期闩锁、且该类生命周期非平凡，会引入过多复杂度（光是推理加锁顺序就非常复杂）」。

### 「重入加闩」这个鸡生蛋问题的解法

`lock0lock.cc` 的 `try_relatch_trx_and_shard_and_do()` 注释描述了一个真实存在的死锁环：

```
/** A helper function which solves a chicken-and-egg problem occurring when one
needs to iterate over trx's locks and perform some actions on them. Iterating
over this list requires trx->mutex (or exclusive global lock_sys latch), and
operating on a lock requires lock_sys latches, yet the latching order requires
lock_sys latches to be taken before trx->mutex.
One way around it is to use exclusive global lock_sys latch, which heavily
deteriorates concurrency. Another is to try to reacquire the latches in needed
order, veryfing that the list wasn't modified meanwhile.
This function performs following steps:
1. releases trx->mutex,
2. acquires proper lock_sys shard latch for given lock,
3. reaquires trx->mutex
4. executes f unless trx's locks list has changed
Before and after this function following should hold:
- the shared global lock_sys latch is held
- the trx->mutex is held
*/
```

解法是：**放掉 `trx->mutex` → 取分片闩 → 重新拿 `trx->mutex` → 用版本号校验锁链表在期间没被改动**。

版本号校验的代码：

```
const auto expected_version = trx->lock.trx_locks_version;
return latch_peeked_shard_and_do(lock, [&]() {
  ut_ad(trx_mutex_own(trx));
  /* Check that list was not modified while we were reacquiring latches */
  if (expected_version != trx->lock.trx_locks_version) {
    /* Someone has modified the list while we were re-acquiring the latches
    so, it is unsafe to operate on the lock. It might have been released, or
    maybe even assigned to another transaction (in case of AUTOINC lock). More
    importantly, we need to let know the caller that the list it is iterating
    over has been modified, which affects next/prev pointers. */
    return false;
  }
  std::forward<F>(f)();
  ...
});
```

**这就是「回退到全局排他闩」这条路径的代价说明**——`try_release_read_locks_in_x_mode()` 是 `try_release_read_locks_in_s_mode()` 失败后的回退分支，它改用 `locksys::Global_exclusive_latch_guard` 一次性锁住整个 lock_sys。源码注释给出了量化代价：「exclusive mode（这也会阻止对 `trx->lock.trx_locks` 的任何改动），然而这会影响 appliers 的性能（TPS 最多下降 10%）」。

### 共享全局闩也有临界区上限

`try_release_read_locks_in_s_mode()` 拿的是 `locksys::Global_shared_latch_guard`（共享模式）。源码为共享模式定义了临界区时长上限：

```
/** We don't want to hold the Global latch for too long, even in S mode, not to
starve threads waiting for X-latch on it such as lock_wait_timeout_thread().
This defines the longest allowed critical section duration. */
constexpr auto MAX_CS_DURATION = std::chrono::seconds{1};
```

**`MAX_CS_DURATION` 是 1 秒**，来自 `std::chrono::seconds{1}`。理由写得很直白：即使在共享模式下也不能长期持有全局闩，否则会饿死等待排他闩的线程——源码点名了 `lock_wait_timeout_thread()`。

### 全局闩的调试断言：谁必须持有什么闩

`lock0lock.cc` 里的 `ut_ad` 断言把「闩的所有权规则」固化成代码。典型例子：

```
ulint lock_number_of_rows_locked(const trx_lock_t *trx_lock) {
  /* We need exclusive lock_sys access, because trx_lock->n_rec_locks is
  modified while holding sharded lock only, so we need to disable all writers
  for this number to be meaningful */
  ut_ad(locksys::owns_exclusive_global_latch());

  return (trx_lock->n_rec_locks);
}
```

**理由是 `trx_lock->n_rec_locks` 只在持有分片闩时修改**，所以读这个计数必须先禁掉所有写者，即拿排他全局闩。

反过来，`lock_rec_lock_fast()` 与 `lock_rec_lock_slow()` 的入口都断言**持有页片闩**：

```
ut_ad(locksys::owns_page_shard(block->get_page_id()));
```

`lock_rec_has_to_wait_in_queue()` 则断言持有等待锁所在页的片闩：

```
const auto page_id = wait_lock->rec_lock.page_id;
ut_ad(locksys::owns_page_shard(page_id));
```

### 边界内的其他使用者：缓冲池与字典结构

条目的 boundary 还提到「它们在页、缓冲池、字典结构上的使用」。这两处闩不在 `lock0latches.cc` 里，但同属本页机制描述的「保护内存结构的互斥闩」范畴：

**缓冲池侧**（`storage/innobase/buf/buf0buf.cc`）：每个缓冲块有自己的 `rw_lock`（`block->lock`），由 `rw_lock_create(buf_block_lock_key, &block->lock, LATCH_ID_BUF_BLOCK_LOCK)` 创建；调试版本另有 `block->debug_latch`（`LATCH_ID_BUF_BLOCK_DEBUG`）。缓冲池级别还有一批互斥量：`LATCH_ID_BUF_POOL_CHUNKS`、`LATCH_ID_BUF_POOL_LRU_LIST`、`LATCH_ID_BUF_POOL_FREE_LIST`、`LATCH_ID_BUF_POOL_ZIP_FREE`、`LATCH_ID_BUF_POOL_ZIP_HASH`、`LATCH_ID_BUF_POOL_ZIP`、`LATCH_ID_BUF_POOL_FLUSH_STATE`、`LATCH_ID_FLUSH_LIST`。页的取用模式由 `RW_S_LATCH` / `RW_X_LATCH` / `RW_SX_LATCH` / `RW_NO_LATCH` 四种取值区分，`buf_page_get_gen()` 的断言 `ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_X_LATCH || rw_latch == RW_SX_LATCH || rw_latch == RW_NO_LATCH)` 固化了这一集合。

**字典侧**（`storage/innobase/dict/dict0dict.cc`）：`dict_sys->mutex`（`LATCH_ID_DICT_SYS`）、`dict_operation_lock`（`LATCH_ID_DICT_OPERATION`）、`dict_foreign_err_mutex`（`LATCH_ID_DICT_FOREIGN_ERR`）、每张表的 `stats_latch`（`LATCH_ID_DICT_TABLE_STATS`，按需惰性创建，`dict_table_stats_latch_create()` 用 `os_once::do_or_wait_for_done()` 保证只创建一次）、每张表的 `autoinc_mutex`（`LATCH_ID_AUTOINC`）与 `autoinc_persisted_mutex`（`LATCH_ID_PERSIST_AUTOINC`）、每个索引的 `zip_pad.mutex`（`LATCH_ID_ZIP_PAD_MUTEX`）、每个索引的 `lock`（`LATCH_ID_INDEX_TREE`）。

**这两侧的闩与 lock_sys 的分片闩是彼此独立的闩域**，不共享 `SHARDS_COUNT` 那套分片机制。

### 真实调用方

| 调用方 | 文件 | 取哪种闩 |
|---|---|---|
| `lock_rec_lock()` | storage/innobase/lock/lock0lock.cc | 页片（入口断言 `owns_page_shard(block->get_page_id())`） |
| `lock_rec_lock_slow()` | storage/innobase/lock/lock0lock.cc | 页片（同上） |
| `lock_rec_lock_fast()` | storage/innobase/lock/lock0lock.cc | 页片（同上） |
| `lock_table()` | storage/innobase/lock/lock0lock.cc | 表片（`Shard_latch_guard table_latch_guard{..., *table}`） |
| `lock_table_ix_resurrect()` | storage/innobase/lock/lock0lock.cc | 表片（同上） |
| `lock_rec_expl_exist_on_page()` | storage/innobase/lock/lock0lock.cc | 页片（`Shard_latch_guard guard{..., page_id}`） |
| `lock_sys_resize()` | storage/innobase/lock/lock0lock.cc | 全局排他闩（`Global_exclusive_latch_guard`） |
| `lock_rec_other_trx_holds_expl()` | storage/innobase/lock/lock0lock.cc | 全局排他闩（需跨 `trx_sys->rw_trx_list` 逐事务查） |
| `try_release_read_locks_in_s_mode()` | storage/innobase/lock/lock0lock.cc | 全局共享闩 + 逐锁页片闩 |
| `try_release_read_locks_in_x_mode()` | storage/innobase/lock/lock0lock.cc | 全局排他闩（`s_mode` 失败后的回退分支） |
| `All_locks_iterator::iterate_over_current_cell()` | storage/innobase/lock/lock0iter.cc | 页片（`Shard_naked_latch_guard`，走 `cell_id` 重载） |
| `All_locks_iterator::iterate_over_current_table()` | storage/innobase/lock/lock0iter.cc | 表片（`Shard_latch_guard` + `*table`） |
| `locksys::find_blockers()` | storage/innobase/lock/lock0iter.cc | 断言持有对应锁所在片的闩（`ut_ad(locksys::owns_lock_shard(&wait_lock))`） |

## 关键取舍

### 用「片」代替「桶」：把并发度与锁数量解耦

哈希桶数会随数据量增长（`lock_sys_resize()` 改的就是它），闩锁数量则固定为 `SHARDS_COUNT`。**取舍：桶数远大于片数时，同片的多个桶共用一把闩，锁竞争上升；换来的是闩锁数组大小恒定、内存占用可预测。**

### 「同桶必同片」是用公式而不是用约定保证的

`get_shard()` 里取模的输入是 `cell_id` 而不是原始哈希值，这不是风格问题而是正确性约束。**取舍：牺牲「谓词锁用自己哈希表的桶号」这一更精确的做法（源码注释承认「按理应该这么做」），换来接口少一个参数、且三个哈希表共用同一套分片逻辑。** 前提是三个哈希表的 `get_n_cells()` 始终相等，由两条 `ut_ad` 断言守住。

### 扩容必须拿全局排他闩：一致性与并发度的直接交换

`lock_sys_resize()` 拿 `Global_exclusive_latch_guard`，期间所有锁队列访问与分片号计算都被阻塞。**取舍：扩容期间的并发度归零，换来「分片号在一次操作内保持稳定」这一前提。** 源码注释直接写了这个代价——「阻止所有人访问 lock sys 队列，甚至阻止计算 shard id」。

### 迭代器接受「返回不完整数据」而不是加锁

`iterate_over_current_cell()` 在检测到 `lock_sys->n_resizes` 变化后直接 `return false` 停止遍历。**取舍：放弃视图完整性，换来迭代器不需要长期持有闩锁——源码注释明确说「持有长期闩锁、且该类生命周期非平凡，会引入过多复杂度（光是推理加锁顺序就非常复杂）」。** 这个取舍被限定在 `performance_schema.data_locks` / `data_wait_locks` 这类本就不保证一致性的视图上。

### 「先闩后 mutex」的加锁顺序是硬约束，不是风格

加锁顺序被源码注释反复强调：`latching order requires lock_sys latches to be taken before trx->mutex`。**取舍：牺牲「先拿 `trx->mutex` 再拿闩」这种更自然的写法，换来无死锁的层级。** 代价是出现了 `try_relatch_trx_and_shard_and_do()` 这种「放掉再重拿 + 版本号校验」的复杂模式。

### 分片闩拿不到时，退回全局排他闩

`try_release_read_locks_in_s_mode()` 失败时，回退到 `try_release_read_locks_in_x_mode()`（拿 `Global_exclusive_latch_guard`）。**取舍：回退路径换来简单与正确，代价是源码注释给出的量化数字——「在 appliers 中 TPS 最多下降 10%」。** 失败原因是「有很多并发线程在修改该事务的锁链表（例如 B-tree 页合并或分裂，或隐式锁转显式锁）」。

### 共享模式也要限临界区时长

`MAX_CS_DURATION = std::chrono::seconds{1}` 对共享全局闩设了 1 秒上限。**取舍：牺牲临界区长度上限的自由，换取等待排他闩的线程（如 `lock_wait_timeout_thread()`）不被饿死。**

### 页分裂 / 合并一次取两片闩，参数顺序固定

`Shard_latches_guard` 的调用点一律写成固定顺序（`{*block, *new_block}`、`{*left_block, *root}`、`{*heir_block, *block}`）。**取舍：牺牲「任意顺序取两片」的灵活性，换来固定取锁顺序带来的无死锁保证。** 代价是这类路径的并发度低于单片路径。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/lock/lock0latches.cc | `Latches::Page_shards::get_shard` | 页 → 记录锁分片号：取 `rec_hash` 的 `cell_id` 后对 `SHARDS_COUNT` 取模；带「同桶必同片」的注释与两条哈希表尺寸相等的 `ut_ad` 断言 |
| storage/innobase/lock/lock0latches.cc | `Latches::Table_shards::get_shard` | 表 → 表锁分片号：直接 `table_id % SHARDS_COUNT` |
| storage/innobase/lock/lock0latches.cc | `Latches::Page_shards::get_mutex` | 由 `page_id` 取页片闩；const / 非 const 两个重载，非 const 版复用 const 版再 `const_cast`；另有接受 `cell_id` 的重载直接 `cell_id % SHARDS_COUNT` |
| storage/innobase/lock/lock0latches.cc | `Latches::Table_shards::get_mutex` | 由 `table_id` 或 `dict_table_t` 取表片闩；const / 非 const 转换模式与 `Page_shards` 一致 |
| storage/innobase/lock/lock0latches.cc | `Latches::Page_shards::Page_shards` | 构造时按 `SHARDS_COUNT` 循环 `mutex_create(LATCH_ID_LOCK_SYS_PAGE, ...)` 建闩 |
| storage/innobase/lock/lock0latches.cc | `Latches::Table_shards::Table_shards` | 构造时按 `SHARDS_COUNT` 循环 `mutex_create(LATCH_ID_LOCK_SYS_TABLE, ...)` 建闩 |
| storage/innobase/lock/lock0latches.cc | `Latches::Page_shards::~Page_shards` | 析构时按 `SHARDS_COUNT` 循环 `mutex_destroy` 释放页片闩 |
| storage/innobase/lock/lock0latches.cc | `Latches::Table_shards::~Table_shards` | 析构时按 `SHARDS_COUNT` 循环 `mutex_destroy` 释放表片闩 |
| storage/innobase/lock/lock0latches.cc | `Latches::Unique_sharded_rw_lock::Unique_sharded_rw_lock` | 创建全局读写闩：PFS key `lock_sys_global_rw_lock_key`（`UNIV_PFS_RWLOCK` 下）、latch id `LATCH_ID_LOCK_SYS_GLOBAL`、名称长度 64 |
| storage/innobase/lock/lock0latches.cc | `Latches::Unique_sharded_rw_lock::~Unique_sharded_rw_lock` | 析构时 `rw_lock.free()` |
| storage/innobase/lock/lock0latches.cc | `Latches::Unique_sharded_rw_lock::m_shard_id` | `thread_local` 的当前分片号，初值 `NOT_IN_USE`，记录当前线程持有的分片 |
| storage/innobase/lock/lock0lock.cc | `lock_sys_create` | 创建 lock_sys：`lock_sys_t{n_cells}` 构造触发 `Latches` 成员构造（逐片建闩） |
| storage/innobase/lock/lock0lock.cc | `lock_sys_resize` | 哈希表扩容：先取 `Global_exclusive_latch_guard`（防止分片号计算与访问），再 `n_resizes++`，再依次 resize 三个哈希表 |
| storage/innobase/lock/lock0lock.cc | `Locks_hashtable::resize` | 桶迁移实现：`HASH_MIGRATE` + 重建 `cells_in_use` 位图；迁移键为 `lock_rec_lock_hash_value` |
| storage/innobase/lock/lock0lock.cc | `Locks_hashtable::get_cell_id` | 把哈希值转成桶号：`hash_calc_cell_id(hash_value, ht.get())`；`get_shard()` 的取模输入 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_hash_value` | 记录锁的迁移哈希键：`lock_rec_hash_value(lock->rec_lock.page_id)` |
| storage/innobase/lock/lock0lock.cc | `locksys::owns_exclusive_global_latch` / `owns_shared_global_latch` | 调试断言用：是否持有全局排他 / 共享闩 |
| storage/innobase/lock/lock0lock.cc | `locksys::owns_page_shard` / `owns_table_shard` | 调试断言用：是否持有指定页 / 表的片闩 |
| storage/innobase/lock/lock0lock.cc | `locksys::owns_lock_shard` | 按锁类型自动分派：记录锁查页片，表锁查表片 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock` | 行锁入口：断言持有页片闩，先试 `lock_rec_lock_fast`，失败回退 `lock_rec_lock_slow` |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_fast` | 常见情况的快速加锁：断言持有页片闩，查 `rec_hash` 找同页已有锁 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_lock_slow` | 慢路径加锁：断言持有页片闩；处理 next-key lock 拆分、冲突判定、`SELECT_SKIP_LOCKED` / `SELECT_NOWAIT` 分支、等待入队 |
| storage/innobase/lock/lock0lock.cc | `lock_table` | 表锁入口：取表片闩后判冲突，再 `trx_mutex_enter` → 入队或建锁 → `trx_mutex_exit` |
| storage/innobase/lock/lock0lock.cc | `lock_table_ix_resurrect` | 给已恢复事务补 IX 表锁：取表片闩后建锁 |
| storage/innobase/lock/lock0lock.cc | `lock_rec_expl_exist_on_page` | 查页上是否存在显式记录锁：取页片闩后查 `rec_hash` |
| storage/innobase/lock/lock0lock.cc | `lock_number_of_rows_locked` | 读 `n_rec_locks` 必须持排他全局闩（该计数只在分片闩保护下修改） |
| storage/innobase/lock/lock0lock.cc | `try_relatch_trx_and_shard_and_do` | 解决「迭代 trx 锁链表」与「加锁顺序要求先闩后 mutex」的鸡生蛋问题：放 mutex → 取分片闩 → 重拿 mutex → 版本号校验 |
| storage/innobase/lock/lock0lock.cc | `try_release_read_locks_in_s_mode` | 共享全局闩下释放读锁；失败返回 false，由调用方走排他模式回退 |
| storage/innobase/lock/lock0lock.cc | `try_release_read_locks_in_x_mode` | 排他全局闩下释放读锁，是 `s_mode` 失败后的回退分支 |
| storage/innobase/lock/lock0lock.cc | `MAX_CS_DURATION` | 共享全局闩的临界区时长上限，值 1 秒（`std::chrono::seconds{1}`），防饿死等排他闩的线程 |
| storage/innobase/lock/lock0iter.cc | `All_locks_iterator::iterate_over_current_cell` | 按桶迭代哈希表：用 `cell_id % SHARDS_COUNT` 定位页片并取 `Shard_naked_latch_guard`，同片桶一次处理完（`m_bucket_id += SHARDS_COUNT`） |
| storage/innobase/lock/lock0iter.cc | `All_locks_iterator::iterate_over_current_table` | 按表迭代表锁队列：取表片闩后遍历 `table->locks` |
| storage/innobase/lock/lock0iter.cc | `locksys::find_blockers` | 查找阻塞锁：断言持有该锁所在片的闩（`owns_lock_shard`） |
| storage/innobase/buf/buf0buf.cc | `buf_page_get_gen` | 缓冲池取页：页闩模式限定为 `RW_S_LATCH` / `RW_X_LATCH` / `RW_SX_LATCH` / `RW_NO_LATCH` |
| storage/innobase/dict/dict0dict.cc | `dict_table_stats_latch_create` | 表统计闩的惰性创建（`os_once::do_or_wait_for_done` 保证只创建一次），latch id `LATCH_ID_DICT_TABLE_STATS` |
| storage/innobase/dict/dict0dict.cc | `dict_table_stats_lock` / `dict_table_stats_unlock` | 按 `RW_S_LATCH` / `RW_X_LATCH` 取放表统计闩 |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的存储层闩锁体系，与 InnoDB 的 `lock0latches.cc` 没有代码级关联，但**机制形态高度同构**，可作对照：

- **两者都把「保护数据结构的互斥量」与「保护数据可见性的锁」分开**：主核的 WAL 与 pager 用自己的 latch / mutex 保护内存结构，其事务层锁语义是另一套。本页 `Latches::Page_shards` / `Table_shards` 保护的是锁队列这一数据结构，与 J02 的记录锁是两层东西。
- **两者都用「分片 + 固定片数」降低热点**：本页用 `SHARDS_COUNT` 固定片数、页 / 表为键取模。
- **两者都需要显式的加锁顺序**：本页是「先 lock_sys 闩、后 `trx->mutex`」，由源码注释与 `ut_ad` 断言共同固化。

**架构推论：** 主核侧可以借鉴本页的三点设计——(1) 用**固定片数的哈希分片**而不是「一把大锁 + 一张哈希表」来把临界区切细，片数恒定因此内存可预测；(2) 把「分片键 → 片号」的映射写成**纯函数**（`get_shard()`），使得「同桶必同片」这类正确性约束能被一条公式而非口头约定保证；(3) 在必须拿全局锁的路径（扩容、全局统计）上，用 `ut_ad` 断言把「此刻应该持有什么闩」固化成可编译检查的规则。

**架构推论：** 本页 `MAX_CS_DURATION` 这类「即使是共享锁也要限制临界区时长」的设计，对主核同样成立——一个长持有的共享闩同样会饿死等排他闩的线程（如主核自己的锁等待超时线程）。但主核是否已有等价机制、以及是否采用相同的分片粒度，本页不作断言，需另行核对主核源码。

## 参考

- [storage/innobase/lock/lock0latches.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/lock/lock0latches.cc) —— 本页全部主符号（`Latches::Page_shards::get_shard`、`Latches::Table_shards::get_shard`、`get_mutex`、`Unique_sharded_rw_lock`）的权威来源。
- [storage/innobase/lock/lock0lock.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/lock/lock0lock.cc) —— `lock_sys_create`、`lock_sys_resize`、`Locks_hashtable::resize`、`owns_lock_shard`、`lock_rec_lock`、`lock_table`、`try_relatch_trx_and_shard_and_do` 等分片闩消费方与加锁顺序约束的来源。
- [InnoDB Locking（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-locking.html) —— 记录锁、间隙锁、next-key 锁、意向锁的官方定义；对照本页「闩锁保护内存结构、行锁保护数据可见性」的分工。
- [InnoDB Locking and Transaction Model（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-locking-transaction-model.html) —— InnoDB 锁体系的上位章节，是本页所属 J 组「InnoDB 事务与锁」的官方背景。
