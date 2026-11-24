---
project: mysql-server
feature: gtid-set
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_gtid_set.cc
    symbols: [Gtid_set::add_gtid_set, Gtid_set::is_subset]
---

# GTID 集合

## 一句话职责

`sql/rpl_gtid_set.cc` 里的 `Gtid_set` 是 GTID 集合的表示与运算层：它把「某个 SID 下的一串 GNO」压成若干个**闭开区间**（`Interval{start, end}`，`end` 是开区间上界），提供区间合并（`add_gno_interval`）、区间切分（`remove_gno_interval`）、集合并（`add_gtid_set`）、集合差（`remove_gtid_set`）、子集判定（`is_subset`）与交集判定（`is_intersection_nonempty`）等运算；它不分配事务 GTID、不落盘、不维护从库的已执行集合。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 GTID 集合的表示、区间合并与包含关系判定；**不负责 GTID 的分配与持久化（M04）**、**不负责从库已执行集合的维护（M05）**。

## 机制是什么

### 集合的元素与表示

一个 GTID 由两部分组成：SID（源实例的 UUID，`typedef binary_log::Uuid rpl_sid`）与 GNO（该 SID 内的单调事务序号，`using rpl_gno = binary_log::gtids::gno_t`）。`Gtid_set` 的组织方式是**按 SID 分桶、桶内按 GNO 区间排列**：

- `m_intervals` 是 `Prealloced_array<Interval *, 8>`，第 N 个元素保存 SIDNO N+1 的区间链表头（`sql/rpl_gtid.h` 中 `Gtid_set` 的成员声明）。
- `Interval` 的定义是 `struct Interval { rpl_gno start; rpl_gno end; Interval *next; }`，注释写明 `start` 是「本区间的第一个 GNO」、`end` 是「本区间之后的第一个 GNO」——**`end` 是开区间上界**，区间内实际覆盖的 GNO 是 `[start, end)`。
- `sid_map`（`Sid_map`）是 SID 与 SIDNO（`typedef int rpl_sidno`，SIDNO 恒 `>= 1`）之间的双向映射。`Gtid_set` 自身不存 UUID，只存 SIDNO；`to_string` 输出时再经 `sid_map->sidno_to_sid(sidno)` 还原成 UUID。

因为区间是开区间上界，写进 `Interval` 的一定是 `end > start`：`add_gno_interval` 入口就断言 `assert(start > 0)` 与 `assert(start < end)`，`remove_gno_interval` 断言 `assert(start < end)`。

### 三种字符串形态

`Gtid_set` 有三套预置的 `String_format`，定义在 `sql/rpl_gtid_set.cc` 里：

| 常量 | 分隔符 | 空集合表示 | 用途 |
|---|---|---|---|
| `Gtid_set::default_string_format` | SID/GNO 用 `:`，区间用 `-`，SID 间用 `,\n` | 空串 | 默认输出 |
| `Gtid_set::sql_string_format` | SID/GNO 用 `:`，区间用 `-`，SID 间用 `',\n'` | `''` | SQL 字面量形态 |
| `Gtid_set::commented_string_format` | 同上，但前缀 `# ` | `# [empty]` | 注释形态 |

三者的 `String_format` 初始化列表都写死了各段长度，例如 `default_string_format` 的最后五项是 `1, 1, 1, 2, 0`，对应 `sid_gno_separator_length = 1`（`:`）、`gno_start_end_separator_length = 2`（`-`）、`gno_gno_separator_length = 0`、`gno_sid_separator_length = 1`（`,`）、`empty_set_string_length = 0`。

### 区间分配：chunk 与空闲链表

`Gtid_set` 不逐个 `malloc` 区间，而是**批量分配 chunk**：

- `struct Interval_chunk { Interval_chunk *next; Interval intervals[1]; }`——`intervals` 数组长度为 1 是 C 的柔性数组惯用法，`create_new_chunk` 实际分配的字节数是 `sizeof(Interval_chunk) + sizeof(Interval) * (size - 1)`。
- `CHUNK_GROW_SIZE` 静态常量值为 **8**（`sql/rpl_gtid.h` 中 `Gtid_set` 的私有静态成员，注释写作「The default number of intervals in an Interval_chunk」），是 `get_free_interval` 在空闲链表为空时新分配的区间个数。
- `MAX_NEW_CHUNK_ALLOCATE_TRIES` 宏值为 **10**（定义在 `sql/rpl_gtid_set.cc` 顶部），是 `create_new_chunk` 分配失败时的重试次数上限。

### GTID 号的上界

`sql/rpl_gtid.h` 定义了三个常量：

- `GNO_END = INT64_MAX`，注释为「One-past-the-max value of GNO」——GNO 的开区间上界。
- `GNO_WARNING_THRESHOLD = (GNO_END / 100) * 99`——GNO 超过它就产生警告。
- `MAX_GNO_TEXT_LENGTH = 19`——GNO 十进制文本的最大长度。

`parse_gno` 的返回值检查直接引用 `GNO_END`：`if (ret < 0 || ret >= GNO_END) return -1;`——**超过 `GNO_END` 的数字一律解析失败**。

## 怎么实现的

### 最低层：区间合并 `add_gno_interval`

`Gtid_set::add_gno_interval(Interval_iterator *ivitp, rpl_gno start, rpl_gno end, Free_intervals_lock *lock)` 是所有「加 GTID」的最终落点，注释称其为「the lowest-level function that adds gtids; this is where Interval objects are added, grown, or merged」。

函数体是一个有序单链表上的合并过程：

1. 断言 `start > 0` 且 `start < end`。
2. 清掉字符串长度缓存：`has_cached_string_length = false; cached_string_length = 0;`。
3. 沿区间链表向前走，对当前区间 `iv`：
   - 若 `iv->end >= start` 且 `iv->start > end`，说明新区间严格位于当前区间之前，`break` 出去走「新建区间」路径。
   - 否则两者相接或相交：先把 `start` 往前推到 `iv->start`（`if (iv->start < start) start = iv->start;`），然后**持续吞掉后继区间**——`while (iv->next && end >= iv->next->start) { lock->lock_if_not_locked(); ivit.remove(this); iv = ivit.get(); }`。最后把合并结果写回 `iv->start = start;` 与 `if (iv->end < end) iv->end = end;`。
4. 若走完链表都没遇到可合并的区间（新区间落在前后两个区间之间的空隙），则 `get_free_interval(&new_iv)` 取一个空闲区间，填入 `new_iv->start = start; new_iv->end = end;`，用 `ivit.insert(new_iv)` 插到当前位置。

**相邻即合并**：判定用的是 `iv->end >= start` 与 `end >= iv->next->start`，而不是严格的 `>`。所以 `[1,5)` 与 `[5,9)` 这种首尾相接的区间会被合并成 `[1,9)`。

**锁是懒获取的**：`lock->lock_if_not_locked()` 只在真正要动空闲链表（`ivit.remove` / `get_free_interval`）时才加锁，且加锁后不立即释放——由顶层函数构造的 `Free_intervals_lock` 析构时统一释放。`Free_intervals_lock::lock_if_not_locked` 的实现是 `if (gtid_set->sid_lock && !locked)`——**`sid_lock` 为空的 `Gtid_set` 是线程局部的，完全不加锁**。

### 最低层：区间切分 `remove_gno_interval`

`Gtid_set::remove_gno_interval(Interval_iterator *ivitp, rpl_gno start, rpl_gno end, Free_intervals_lock *lock)` 是对称的删除路径，注释称其为「the lowest-level function that removes gtids; this is where Interval objects are removed, truncated, or split」。

函数体处理三种几何关系：

- **新区间完全覆盖某区间**（`iv->end <= end`）：`ivit.remove(this)` 整段摘掉。
- **新区间切在某区间中间**（`iv->start < start` 且 `iv->end > end`）：**把一个区间劈成两个**——新取一个空闲区间 `new_iv`，令 `new_iv->start = end; new_iv->end = iv->end;`，再把原区间截断为 `iv->end = start;`，然后 `ivit.next(); ivit.insert(new_iv);`。
- **只切头尾**：切头则 `iv->end = start;` 并前进一格；切尾则 `iv->start = end;`。

注意 `remove_gno_interval` 的注释明确写着：**不要求被删除的 GNO 真的存在于这个 `Gtid_set` 里**（「It is not required that the gtids in the interval exist in this Gtid_set.」）——所以「差集」运算不会因不存在的元素而报错。

### 集合并：`add_gtid_set`

`Gtid_set::add_gtid_set(const Gtid_set *other)` 是两个集合求并的入口。函数体开头就断言写锁：`if (sid_lock != nullptr) sid_lock->assert_some_wrlock();`，随后取 `rpl_sidno max_other_sidno = other->get_max_sidno();` 并构造 `Free_intervals_lock lock(this);`。

接下来**按两个集合是否共用同一个 `Sid_map` 分成两条路径**：

**路径一：`other->sid_map == sid_map || other->sid_map == nullptr || sid_map == nullptr`**（同 map，sidno 可直接对应）

```
PROPAGATE_REPORTED_ERROR(ensure_sidno(max_other_sidno));
for (rpl_sidno sidno = 1; sidno <= max_other_sidno; sidno++)
  add_gno_intervals(sidno, Const_interval_iterator(other, sidno), &lock);
```

先把本集合的 sidno 容量扩到 `max_other_sidno`（`ensure_sidno`），再逐 sidno 把对方的区间整体灌进来。`add_gno_intervals` 内部对对方的每个区间调 `add_gno_interval`——**所以合并语义完全由 `add_gno_interval` 提供，路径一不重复实现合并逻辑**。

**路径二：两个 `Sid_map` 不同**（sidno 不能直接对应，必须按 UUID 重新映射）

```
for (rpl_sidno other_sidno = 1; other_sidno <= max_other_sidno; other_sidno++) {
  Const_interval_iterator other_ivit(other, other_sidno);
  if (other_ivit.get() != nullptr) {
    const rpl_sid &sid = other_sid_map->sidno_to_sid(other_sidno);
    rpl_sidno this_sidno = sid_map->add_sid(sid);
    if (this_sidno <= 0) RETURN_REPORTED_ERROR;
    PROPAGATE_REPORTED_ERROR(ensure_sidno(this_sidno));
    add_gno_intervals(this_sidno, other_ivit, &lock);
  }
}
```

**空区间桶被跳过**（`if (other_ivit.get() != nullptr)`），只有真正装了区间的 sidno 才会被 `sid_map->add_sid` 注册进本集合的 map，从而**避免把对方的空桶也拉进来**。

**错误传播**：`add_gtid_set` 返回 `enum_return_status`（`RETURN_STATUS_OK` / `RETURN_STATUS_UNREPORTED_ERROR` / `RETURN_STATUS_REPORTED_ERROR`）。只有两处会失败：`sid_map->add_sid(sid)` 返回 `<= 0`（`RETURN_REPORTED_ERROR`），以及 `ensure_sidno` 失败（内存不足时 `BINLOG_ERROR(("Out of memory."), (ER_OUT_OF_RESOURCES, MYF(0)))` 后 `RETURN_REPORTED_ERROR`）。

### 集合差：`remove_gtid_set`

`Gtid_set::remove_gtid_set(const Gtid_set *other)` 与 `add_gtid_set` 同样按 `sid_map` 是否相同分成两条路径，区别在于**路径二用的是查表而不是插入**：

```
rpl_sidno this_sidno = sid_map->sid_to_sidno(sid);
if (this_sidno != 0)
  remove_gno_intervals(this_sidno, other_ivit, &lock);
```

`Sid_map::sid_to_sidno` 对不存在的 SID 返回 **0**，而 0 是非法的 sidno，所以 `if (this_sidno != 0)` 就跳过了「本集合里根本没有这个 SID」的情况。

路径一还有一个收窄：`rpl_sidno max_sidno = min(max_other_sidno, get_max_sidno());`——只处理双方都有的 sidno 区间。

### 子集判定：`is_subset`

`Gtid_set::is_subset(const Gtid_set *super) const` 判定「本集合是否被 `super` 包含」。函数体：

```
for (int sidno = 1; sidno <= max_sidno; sidno++) {
  Const_interval_iterator ivit(this, sidno);
  const Interval *iv = ivit.get();
  if (iv != nullptr) {
    int super_sidno;
    if (super_sid_map == sid_map || super_sid_map == nullptr || sid_map == nullptr)
      super_sidno = sidno;
    else {
      super_sidno = super_sid_map->sid_to_sidno(sid_map->sidno_to_sid(sidno));
      if (super_sidno == 0) return false;
    }
    if (super_sidno > super_max_sidno) return false;
    Const_interval_iterator super_ivit(super, super_sidno);
    if (!is_interval_subset(&ivit, &super_ivit)) return false;
  }
}
return true;
```

要点：

- **只遍历本集合里有区间的 sidno**（`if (iv != nullptr)`）——本集合没装的 SID 天然是 `super` 的子集，不构成反例。
- **sidno 映射不同 map 时**，先经 `sid_map->sidno_to_sid(sidno)` 把本方 sidno 换成 UUID，再用 `super_sid_map->sid_to_sidno` 查对方的 sidno。**查不到（返回 0）直接 `return false`**——本集合有的 SID 在 `super` 里完全不存在，显然不是子集。
- `super_sidno > super_max_sidno` 也 `return false`。
- 真正的区间比较交给静态方法 `is_interval_subset`。

`Gtid_set::is_interval_subset(Const_interval_iterator *sub, Const_interval_iterator *super)` 是双指针扫描，注释给出的算法是：让 `sub_iv` 遍历子集合区间，对每个 `sub_iv` 跳过所有「结束在 `sub_iv` 之前」的 `super` 区间，找到第一个不早于 `sub_iv` 的 `super` 区间后检查它是否覆盖 `sub_iv`。

```
while (sub_iv->start > super_iv->end) {
  super->next();
  super_iv = super->get();
  if (super_iv == nullptr) return false;
}
if (sub_iv->start < super_iv->start || sub_iv->end > super_iv->end)
  return false;
```

**注意 `sub_iv->start > super_iv->end` 用的是严格大于**，而 `end` 是开区间上界，所以 `[1,5)` 与 `[5,9)` 会被判定为覆盖关系（`5 > 5` 为假，不跳过）——这与 `add_gno_interval` 的首尾相接合并语义一致。

`is_subset` 有个姐妹方法 `is_subset_for_sid(super, superset_sidno, subset_sidno)`：它做的是**单个 SID 维度**的子集判定，且对 sidno 为 0 的情况有明确约定——`if (subset_sidno == 0) return true;`（子集合根本没有这个 SID，故它被判定为任何集合的子集）与 `if (superset_sidno == 0) return false;`（超集合没有这个 SID，故它判定为不是超集）。

### 交集判定：`is_intersection_nonempty`

`Gtid_set::is_intersection_nonempty(const Gtid_set *other) const` 的实现**刻意不走差集路线**。函数体内的注释解释了原因：

```
This could in principle be implemented as follows:
  Gtid_set this_minus_other(sid_map);
  this_minus_other.add_gtid_set(this);
  this_minus_other.remove_gtid_set(other);
  bool ret= equals(&this_minus_other);
  return ret;
However, that does not check the return values from add_gtid_set
or remove_gtid_set, and there is no way for this function to
return an error.
```

所以它直接做双指针扫描 `is_interval_intersection_nonempty(ivit1, ivit2)`：跳过 `iv2->end <= iv1->start` 的区间，找到第一个与 `iv1` 起点相交的 `iv2` 区间后判 `if (iv2->start < iv1->end) return true;`。

### 求交：`intersection`

`Gtid_set::intersection(const Gtid_set *other, Gtid_set *result)` 用纯集合代数实现，函数体注释写明：「In set theory, intersection(A, B) == A - (A - B)」：

```
Gtid_set this_minus_other(sid_map);
Gtid_set intersection(sid_map);
PROPAGATE_REPORTED_ERROR(this_minus_other.add_gtid_set(this));
this_minus_other.remove_gtid_set(other);
PROPAGATE_REPORTED_ERROR(intersection.add_gtid_set(this));
intersection.remove_gtid_set(&this_minus_other);
PROPAGATE_REPORTED_ERROR(result->add_gtid_set(&intersection));
```

**每次求交要构造两个临时 `Gtid_set`**。源码里的 `@todo` 注释承认这个算法「simple, a little bit slower than necessary」，并说更高效的做法是像 `add_gno_interval` 那样同时迭代两个集合的区间。

### 文本解析：`add_gtid_text`

`Gtid_set::add_gtid_text(const char *text, bool *anonymous, bool *starts_with_plus)` 把字符串解析成集合。函数体要点：

- 开头断言写锁：`if (sid_lock != nullptr) sid_lock->assert_some_wrlock();`。
- **`+` 前缀**：`starts_with_plus` 非空时，先跳空白，若首字符是 `+` 则置 `*starts_with_plus = true` 并跳过该字符。这个标志的语义是「本次是往已有集合上**追加**」，而非替换。
- **`ANONYMOUS` 关键字**：`if (anonymous != nullptr && strncmp(s, "ANONYMOUS", 9) == 0)` 时置 `*anonymous = true` 并跳过 9 个字符，不做任何 sidno 解析。
- **一次性预分配**：`if (chunks == nullptr)` 时先数一遍文本里**冒号 `:` 的个数**（源码注释：「compute number of intervals in text: it is equal to the number of colons」），然后 `create_new_chunk(n_intervals)` 一次把区间全部分配好。
- **UUID 解析**：`sid.parse(s, binary_log::Uuid::TEXT_LENGTH) != 0` 失败则 `goto parse_error`。成功则 `sidno = sid_map->add_sid(sid)`，`sidno <= 0` 直接 `RETURN_REPORTED_ERROR`，否则 `PROPAGATE_REPORTED_ERROR(ensure_sidno(sidno))`。
- **区间解析**：对每个 `:`，先读 `start = parse_gno(&s)`；`start <= 0` 报错（`start == 0` 与 `start < 0` 分别打不同的 DBUG_PRINT）。若紧跟 `-`，则读 `end = parse_gno(&s)` 并 `end++`；否则 `end = start + 1`（**单点区间**）。**只有 `end > start` 时才调 `add_gno_interval`**。
- **迭代器复用**：`Interval *current = ivit.get(); if (current == nullptr || start < current->start) ivit.init(this, sidno);`——如果新区间比当前迭代位置更靠前，就重置迭代器从头扫，否则沿用当前位置。

**错误出口**统一是 `parse_error:` 标签：

```
BINLOG_ERROR(("Malformed Gtid_set specification '%.200s'.", text),
             (ER_MALFORMED_GTID_SET_SPECIFICATION, MYF(0), text));
RETURN_REPORTED_ERROR;
```

`BINLOG_ERROR` 是个宏，服务端与 mysqlbinlog 两边用不同实现（`sql/rpl_gtid.h` 中 `#define BINLOG_ERROR(MYSQLBINLOG_ERROR, SERVER_ERROR) my_error SERVER_ERROR`）。

### 二进制编码：`encode` / `add_gtid_encoding`

`Gtid_set::encode(uchar *buf)` 的二进制布局是：

```
uint64 n_sids            // 8 字节，SID 数量
per SID:
  16 字节 UUID          // binary_log::Uuid::BYTE_LENGTH
  uint64 n_intervals     // 8 字节
  per interval:
    int64 start          // 8 字节，int8store
    int64 end            // 8 字节，int8store（开区间上界）
```

编码里同时存了 `n_sids` 与 `n_intervals` 两个计数，`int8store` / `uint8korr` / `sint8korr` 分别是写与读。`get_encoded_length` 给出与之对应的长度公式：

```
size_t ret = 8;
for (rpl_sidno sidno = 1; sidno <= max_sidno; sidno++)
  if (contains_sidno(sidno)) ret += 16 + 8 + 2 * 8 * get_n_intervals(sidno);
```

**`encode` 只写有区间的 sidno**（`if (iv != nullptr)` 才 `n_sids++`），但遍历范围是 `sidmap_max_sidno`——源码注释说明「it is possible that the sid_map has more SIDNOs than the set」，所以要显式 `if (sidno > max_sidno) continue;` 跳过。

`Gtid_set::add_gtid_encoding(const uchar *encoded, size_t length, size_t *actual_length)` 是逆操作，三处长度校验：

- `if (length < 8)` ——连 SID 数量都读不出来。
- `if (length - pos < 16 + 8)` ——读 UUID 与区间数不够。
- `if (length - pos < 2 * 8 * n_intervals)` ——区间区不够。

**区间合法性检查**是 `if (start <= last || end <= start)`，其中 `last` 初始为 0，随循环推进为上一个区间的 `end`——**这强制区间严格递增且互不重叠**。任何一条不满足就 `goto report_error`，报 `ER_MALFORMED_GTID_SET_ENCODING`。

末尾还有一处严格性检查：只有当 `actual_length == nullptr` 时才要求 `pos == length`；否则把 `*actual_length = pos` 交还调用方——**允许缓冲区尾部有多余数据**。

### 容量扩展：`ensure_sidno`

`Gtid_set::ensure_sidno(rpl_sidno sidno)` 在 sidno 超出当前容量时扩容。函数体里有一段精细的锁处理：源码注释写明「Not all Gtid_sets are protected by an rwlock. But if this Gtid_set is, we assume that the read lock has been taken. Then we temporarily upgrade it to a write lock while resizing the array, and then we restore it to a read lock at the end.」

具体地：先 `is_wrlock = sid_lock->is_wrlock()`；若当前不是写锁，则 `unlock()` → `wrlock()`，**并且在拿到写锁后重新检查一次条件**（注释：「maybe a concurrent thread already resized the Gtid_set while we released the lock; check the condition again」），若发现已被别人扩好，就 `unlock()` → `rdlock()` 并 `RETURN_OK` 直接返回。扩容用 `m_intervals.push_back(null_p)` 逐个补空桶，失败时 `goto error` 报 `ER_OUT_OF_RESOURCES`。

### 内存耗尽时的行为

`Gtid_set::create_new_chunk(int size)` 是**唯一**批量分配区间块的地方，它的失败处理是「重试若干次，仍失败就杀进程」：

```
while (i < MAX_NEW_CHUNK_ALLOCATE_TRIES) {
  new_chunk = (Interval_chunk *)my_malloc(...);
  if (new_chunk != nullptr) {
    if (i > 0) LogErr(WARNING_LEVEL, ER_RPL_GTID_MEMORY_FINALLY_AVAILABLE, i + 1);
    break;
  }
  my_sleep(1);   // Sleep 1 microsecond per try to avoid temporary 'out of memory'
  i++;
}
if (MAX_NEW_CHUNK_ALLOCATE_TRIES == i ||
    DBUG_EVALUATE_IF("rpl_simulate_new_chunk_allocate_failure", 1, 0)) {
  my_safe_print_system_time();
  my_safe_printf_stderr("%s", "[Fatal] Out of memory while allocating a new chunk of intervals for storing GTIDs.\n");
  _exit(MYSQLD_FAILURE_EXIT);
}
```

三点值得注意：

1. **每次重试前 `my_sleep(1)` 睡 1 微秒**，注释说明是为了避开「临时性」OOM。
2. **重试耗尽后调 `_exit(MYSQLD_FAILURE_EXIT)` 直接终止进程**，不是抛异常也不是返回错误码。
3. `i > 0` 时会打一条 `ER_RPL_GTID_MEMORY_FINALLY_AVAILABLE` 警告日志，参数是「第几次尝试才成功」。
4. 存在 debug 开关 `rpl_simulate_new_chunk_allocate_failure` 用来强制触发这条致命路径；`get_free_interval` 里另有开关 `rpl_gtid_get_free_interval_simulate_out_of_memory` 会连带打开前者。

### 锁模型

`Gtid_set` 有两层锁：

- **`sid_lock`（`Checkable_rwlock *`）**：保护 sidno 数量与整体结构。构造函数接收它，可为空——**`sid_lock` 为空表示这个 `Gtid_set` 是线程局部的，不受并发保护**。所有公开的修改性方法在入口都用 `assert_some_wrlock()` 断言调用方持有写锁。
- **`free_intervals_mutex`（`mysql_mutex_t`）**：保护空闲区间链表。由内部类 `Free_intervals_lock` 管理，它在析构时自动解锁；`lock_if_not_locked()` 的条件是 `if (gtid_set->sid_lock && !locked)`——**`sid_lock` 为空时连这把互斥锁都不加**。

`ensure_sidno` 是唯一会临时升降 `sid_lock` 读锁到写锁的地方，且降级回读锁时中间存在「锁完全没持有」的短暂窗口（`sql/rpl_gtid.h` 中 `Sid_map` 的注释也描述了同样的升级-降级模式）。

## 关键取舍

### 用区间链而不是逐个 GTID 存储

**取舍：把连续的 GNO 压成一个区间，换来集合大小与事务数脱钩。** 一个跑了几百万事务的实例，`Gtid_set` 里存的是几十个 `Interval`，而不是几百万个 GTID 号。代价是**所有运算都必须按区间做，不能按单点做**——这直接决定了 `is_interval_subset`、`is_interval_intersection_nonempty` 都必须是双指针扫描，而不是哈希查找。

### 区间是开区间上界，首尾相接即合并

`add_gno_interval` 用 `iv->end >= start` 与 `end >= iv->next->start` 判定相接，把 `[1,5)` 与 `[5,9)` 合并成 `[1,9)`。

**取舍：区间数量保持最小，代价是「区间端点」在内部表示与外部文本之间有一层转换。** 对外输出时 `to_string` 要把开区间上界 `end` 还原成闭区间上界 `end - 1`：`if (iv->end > iv->start + 1) { ...; s += format_gno(s, iv->end - 1); }`——**单点区间不输出 `-`，只有长度大于 1 的区间才输出 `start-end` 形式**。

### 空闲链表 + chunk 批量分配

**取舍：内存按块分配、按链表复用，代价是分配路径上有一处「失败即杀进程」的硬退出。** `create_new_chunk` 重试 `MAX_NEW_CHUNK_ALLOCATE_TRIES`（10）次后调 `_exit(MYSQLD_FAILURE_EXIT)`。这与本文件其它函数「返回 `enum_return_status`」的错误风格截然不同——**集合运算的失败是可恢复的，但「连一个区间都分配不出来」被判定为不可恢复**。

`Gtid_set::add_interval_memory(int n_intervals, Interval *intervals_param)` 提供了「用调用方自备的区间数组」的优化路径，`sql/rpl_gtid_state.cc` 的 `Gtid_state::save_gtids_of_last_binlog_into_table` 就用了它：静态常量 `PREALLOCATED_INTERVAL_COUNT = 64`，在栈上开 `Gtid_set::Interval iv[PREALLOCATED_INTERVAL_COUNT]` 后调 `logged_gtids_last_binlog.add_interval_memory(PREALLOCATED_INTERVAL_COUNT, iv)`——**源码注释说明动机是「Allocate some intervals on stack to reduce allocation」**。

### 求交用「两次减法」而不是双指针

**取舍：`intersection` 走 `A - (A - B)` 的集合代数，实现极简但每次要构造两个临时 `Gtid_set`。** 源码 `@todo` 明确说这不是最优解，理由写在注释里：「At the moment the performance of this is not super-important. /Sven」。

**但 `is_intersection_nonempty` 走的是双指针**——同一个文件里两种实现并存，`is_intersection_nonempty` 的注释专门解释了为什么不用差集路线：差集路线会**丢掉 `add_gtid_set` / `remove_gtid_set` 的返回值**，而该函数「there is no way for this function to return an error」。

**取舍：`is_intersection_nonempty` 为了「不返回错误」而承担了重复实现扫描逻辑的成本，换来调用方（如 `Gtid_state::add_lost_gtids`）可以直接把它用在 `if` 条件里。**

### 子集判定遇到「SID 不存在」直接判否

`is_subset` 在 sidno 映射失败时 `return false`。

**取舍：语义上正确且保守——「本集合有的东西在超集合里找不到」必然不是子集；代价是判否比判真更容易。** 在 `Gtid_state::add_lost_gtids` 里这个保守方向是必需的：`if (!gtid_state->get_lost_gtids()->is_subset(gtid_set))` 为真就报 `ER_CANT_SET_GTID_PURGED_DUE_SETS_CONSTRAINTS`，错误文本是 "the new value must be a superset of the old value"——**新值必须是旧值的超集**，判否方向错了就会让 `@@GLOBAL.GTID_PURGED` 被改小。

### 空桶不参与跨 map 合并

`add_gtid_set` 的路径二用 `if (other_ivit.get() != nullptr)` 过滤掉对方没装区间的 sidno。

**取舍：避免把对方的空桶注册进本方的 `Sid_map`，代价是路径二的实现比路径一多一层过滤。** 这一点对 `Sid_map` 的规模有实际影响——`Sid_map` 里的每个 SIDNO 都要占一份 `Prealloced_array` 槽位。

### 字符串长度缓存与失效

`Gtid_set` 缓存了字符串长度：`has_cached_string_length`、`cached_string_length`、`cached_string_format` 三个成员。`get_string_length` 的缓存键是**格式指针**——`if (has_cached_string_length == false || cached_string_format != sf)` 才重算。

**任何修改集合的操作都必须让缓存失效**，而且两种失效方式不同：`add_gno_interval` 置 `has_cached_string_length = false; cached_string_length = 0;`，`remove_gno_interval` 置 `has_cached_string_length = false; cached_string_length = -1;`。**取值不同（0 与 -1）但都靠 `has_cached_string_length` 这个布尔位决定是否重算**——`cached_string_length` 的具体数值在缓存有效时不参与判断。

### `Gtid_set` 与 `Gtid_state` 的分工

`Gtid_set` 本身**不含任何持久化逻辑**。持久化在 `Gtid_state` 与 `Gtid_table_persistor` 那一侧：

- `Gtid_state::add_lost_gtids(Gtid_set *gtid_set, bool starts_with_plus)` 用 `add_lost_gtids` 处理 `@@GLOBAL.GTID_PURGED` 的设置。它是**「追加」语义的真实调用方**——`starts_with_plus` 正是 `add_gtid_text` 解析出的那个标志：为假时先校验新集合必须是旧 `lost_gtids` 的超集（`is_subset`），再 `gtid_set->remove_gtid_set(gtid_state->get_lost_gtids())` 把旧值摘掉。
- `Gtid_state::update_prev_gtids(Gtid_set *write_gtid_set)` 用 `remove_gtid_set` + `add_gtid_set` 维护 `previous_gtids_logged`。
- `Gtid_state::save_gtids_of_last_binlog_into_table()` 用 `add_gtid_set` / `remove_gtid_set` 算出 `executed_gtids - previous_gtids_logged - gtids_only_in_table`。

### 实际的调用方

`Gtid_set::add_gtid_set` 在源码中的实际调用方（均在可核对的源码文件里）：

| 调用方 | 位置 | 用途 |
|---|---|---|
| `Relay_log_info::add_gtid_set` | sql/rpl_rli.cc | 从库侧把已执行集合并入 `rli->gtid_set`，加锁由它自己做（`get_sid_lock()->wrlock()`） |
| `MYSQL_BIN_LOG::get_gtid_executed` | sql/binlog.cc | 在 `LOCK_commit` 与 `global_sid_lock` 双锁下把 `gtid_state->get_executed_gtids()` 并入目标集合 |
| `Gtid_state::add_lost_gtids` | sql/rpl_gtid_state.cc | `@@GLOBAL.GTID_PURGED` 设置路径 |
| `Gtid_state::update_prev_gtids` | sql/rpl_gtid_state.cc | 维护 `previous_gtids_logged` |
| `Gtid_state::save_gtids_of_last_binlog_into_table` | sql/rpl_gtid_state.cc | 算出最后一个 binlog 的 GTID 集合 |
| `MYSQL_BIN_LOG` 的 PREVIOUS_GTIDS 生成路径 | sql/binlog.cc | 算 `logged_gtids_binlog = executed_gtids - gtids_only_in_table` |
| `init_gtid_sets` | sql/mysqld.cc | 服务器启动时比对 binlog 与 `gtid_executed` 表 |
| `sql/rpl_replica.cc` 的 `gtid_executed` 组装 | sql/rpl_replica.cc | 发起 dump 时合并从库已执行集合 |

`Gtid_set::is_subset` 的实际调用方：

| 调用方 | 位置 | 用途 |
|---|---|---|
| `Relay_log_info::wait_for_gtid_set` | sql/rpl_rli.cc | 「等待目标 GTID 集合被应用」的判定：`if (wait_gtid_set->is_subset(executed_gtids) && !owned_gtids->is_intersection_nonempty(wait_gtid_set))` |
| `Until_after_gtids::check_at_start_slave` | sql/rpl_replica_until_options.cc | `UNTIL AFTER GTIDS` 的启动检查：`if (m_gtids.is_subset(gtid_state->get_executed_gtids()))` |
| `Gtid_state::add_lost_gtids` | sql/rpl_gtid_state.cc | 校验新的 `GTID_PURGED` 必须是旧值的超集 |
| `MYSQL_BIN_LOG::find_first_log_not_in_gtid_set` | sql/binlog.cc | 逐个 binlog 倒查 `binlog_previous_gtid_set.is_subset(gtid_set)`，定位第一个仍含未执行 GTID 的 binlog |
| `init_gtid_sets` | sql/mysqld.cc | 启动时判断 `gtids_in_binlog.is_subset(executed_gtids)` |

### 跨 `Sid_map` 的比较要付出 UUID 转换成本

`is_subset`、`equals`、`is_intersection_nonempty`、`add_gtid_set` 在两个 `Sid_map` 不同时都要做 `sid_map->sidno_to_sid(sidno)` + `sid_to_sidno(sid)` 的往返。

**取舍：支持了「两个独立 `Sid_map` 的集合互比」，代价是这条路径比同 map 路径多一次哈希查找。** `equals` 在不同 map 时甚至要按 SID 的排序顺序做归并遍历（`sid_map->get_sorted_sidno(sid_i)`），比 `is_subset` 的按 sidno 遍历更贵。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_gtid_set.cc | `Gtid_set::add_gtid_set` | 集合并：按两个集合是否共用 `Sid_map` 分成两条路径，同 map 直接按 sidno 灌入、异 map 先经 `sid_map->add_sid` 重新映射并跳过空桶；返回 `enum_return_status` |
| sql/rpl_gtid_set.cc | `Gtid_set::is_subset` | 子集判定：遍历本集合有区间的 sidno，异 map 时经 `sid_to_sidno` 映射（查不到即 `return false`），逐 sidno 委托 `is_interval_subset` |
| sql/rpl_gtid_set.cc | `Gtid_set::add_gno_interval` | 最低层区间合并：相接或相交时吞掉后继区间并写回合并结果，否则新建区间插入；首尾相接（`iv->end >= start`）也会合并 |
| sql/rpl_gtid_set.cc | `Gtid_set::remove_gno_interval` | 最低层区间切分：完全覆盖则整段移除，中段覆盖则把一个区间劈成两个，只切头尾则截断 |
| sql/rpl_gtid_set.cc | `Gtid_set::remove_gtid_set` | 集合差：同 map 时取双方 sidno 交集，异 map 时用 `sid_to_sidno` 查表（返回 0 则跳过） |
| sql/rpl_gtid_set.cc | `Gtid_set::is_interval_subset` | 区间级子集判定：双指针扫描，`sub_iv->start > super_iv->end` 时跳过 super 区间，否则检查覆盖关系 |
| sql/rpl_gtid_set.cc | `Gtid_set::is_interval_intersection_nonempty` | 区间级相交判定：跳过 `iv2->end <= iv1->start` 的区间后检查 `iv2->start < iv1->end` |
| sql/rpl_gtid_set.cc | `Gtid_set::is_intersection_nonempty` | 集合级相交判定；注释明确说明不采用 `A - (A - B)` 路线，因为那条路线会丢掉 `add_gtid_set` / `remove_gtid_set` 的返回值 |
| sql/rpl_gtid_set.cc | `Gtid_set::intersection` | 求交：按 `intersection(A, B) == A - (A - B)` 实现，每次构造两个临时 `Gtid_set`；源码 `@todo` 承认这不是最优解 |
| sql/rpl_gtid_set.cc | `Gtid_set::equals` | 集合相等判定：同 map 时逐 sidno 比对区间，异 map 时按 SID 排序归并遍历 |
| sql/rpl_gtid_set.cc | `Gtid_set::add_gtid_text` | 文本解析：处理 `+` 前缀、`ANONYMOUS` 关键字、UUID 与区间；按冒号个数一次性 `create_new_chunk` 预分配；出错报 `ER_MALFORMED_GTID_SET_SPECIFICATION` |
| sql/rpl_gtid_set.cc | `Gtid_set::is_valid` | 纯校验，不改动集合：UUID 合法性 + 区间数值合法性 |
| sql/rpl_gtid_set.cc | `parse_gno` | 解析 GNO 文本，返回 -1 表示失败；判定条件是 `ret < 0 || ret >= GNO_END` |
| sql/rpl_gtid_set.cc | `Gtid_set::create_new_chunk` | 批量分配区间块：重试 `MAX_NEW_CHUNK_ALLOCATE_TRIES`（10）次，每次失败 `my_sleep(1)`；耗尽后打印 `[Fatal] Out of memory while allocating a new chunk of intervals for storing GTIDs.` 并 `_exit(MYSQLD_FAILURE_EXIT)` |
| sql/rpl_gtid_set.cc | `Gtid_set::get_free_interval` | 取空闲区间：链表为空时 `create_new_chunk(CHUNK_GROW_SIZE)`（8 个区间） |
| sql/rpl_gtid_set.cc | `Gtid_set::ensure_sidno` | 扩展 sidno 容量：需要时把读锁临时升级为写锁、扩容后降级回读锁；释放锁后重新检查条件以应对并发扩容；失败报 `ER_OUT_OF_RESOURCES` |
| sql/rpl_gtid_set.cc | `Gtid_set::to_string` | 输出字符串：按 `sid_map->get_sorted_sidno` 顺序遍历，把开区间上界 `end` 还原成闭区间上界 `end - 1`，单点区间不输出 `-` |
| sql/rpl_gtid_set.cc | `Gtid_set::get_string_length` | 计算并缓存字符串长度，缓存键是 `String_format` 指针 |
| sql/rpl_gtid_set.cc | `Gtid_set::encode` | 二进制编码：先写 `uint64` 的 SID 数量，再逐 SID 写 16 字节 UUID、`uint64` 区间数与各区间的 `int64` 起止值 |
| sql/rpl_gtid_set.cc | `Gtid_set::add_gtid_encoding` | 二进制解码：校验三处长度（`< 8`、`< 16 + 8`、`< 2 * 8 * n_intervals`），并强制区间递增不重叠（`start <= last || end <= start` 即报错） |
| sql/rpl_gtid_set.cc | `Gtid_set::get_encoded_length` | 编码长度公式：`8` 加上每个非空 sidno 的 `16 + 8 + 2 * 8 * get_n_intervals(sidno)` |
| sql/rpl_gtid_set.cc | `Gtid_set::contains_gtid` | 单个 GTID 是否在集合中：`gno < iv->start` 返回 false，`gno < iv->end` 返回 true |
| sql/rpl_gtid_set.cc | `Gtid_set::get_last_gno` | 取某 sidno 的最大 GNO：遍历区间取 `iv->end - 1` |
| sql/rpl_gtid_set.cc | `Gtid_set::clear` | 清空集合：把各 sidno 的区间链表接到空闲链表尾部 |
| sql/rpl_gtid_set.cc | `Gtid_set::clear_set_and_sid_map` | 同时清集合与 `Sid_map`；注释说明只清其一会导致 `Gtid_set::get_max_sidno()` 大于 `Sid_map::get_max_sidno()` |
| sql/rpl_gtid_set.cc | `Gtid_set::is_subset_for_sid` | 单 SID 维度的子集判定；`subset_sidno == 0` 返回 true、`superset_sidno == 0` 返回 false |
| sql/rpl_gtid_set.cc | `Gtid_set::get_gtid_intervals` | 把集合导出为 `list<Gtid_interval>` |
| sql/rpl_gtid_set.cc | `Gtid_set::is_size_greater_than_or_equal` | 按 sidno 累加 `get_gtid_count`，达到阈值即提前返回 true |
| sql/rpl_gtid_set.cc | `Gtid_set::default_string_format` / `sql_string_format` / `commented_string_format` | 三套预置输出格式：分隔符与空集合表示各不相同 |
| sql/rpl_gtid.h | `Gtid_set` | 类定义：`m_intervals` 为 `Prealloced_array<Interval *, 8>`，`Interval` 为 `struct { rpl_gno start; rpl_gno end; Interval *next; }`（`end` 为开区间上界） |
| sql/rpl_gtid.h | `CHUNK_GROW_SIZE` | 静态常量，值为 8：单个 `Interval_chunk` 默认容纳的区间个数 |
| sql/rpl_gtid.h | `GNO_END` / `GNO_WARNING_THRESHOLD` / `MAX_GNO_TEXT_LENGTH` | 分别为 `INT64_MAX`、`(GNO_END / 100) * 99`、19 |
| sql/rpl_gtid.h | `Gtid_set::Free_intervals_lock` | 空闲链表互斥锁的 RAII 封装；`lock_if_not_locked` 的条件是 `gtid_set->sid_lock && !locked` |
| sql/rpl_gtid_state.cc | `Gtid_state::add_lost_gtids` | `@@GLOBAL.GTID_PURGED` 设置路径：非追加模式下先用 `is_subset` 校验新值是旧值的超集，再 `remove_gtid_set` 摘掉旧值 |
| sql/rpl_gtid_state.cc | `Gtid_state::save_gtids_of_last_binlog_into_table` | 用栈上 `Interval iv[PREALLOCATED_INTERVAL_COUNT]`（64）配合 `add_interval_memory` 减少分配，再算 `executed_gtids - previous_gtids_logged - gtids_only_in_table` |
| sql/rpl_gtid_state.cc | `Gtid_state::update_prev_gtids` | 用 `remove_gtid_set` + `add_gtid_set` 维护 `previous_gtids_logged` |
| sql/rpl_rli.cc | `Relay_log_info::add_gtid_set` | 从库侧把集合并入 `rli->gtid_set`，自行加 `get_sid_lock()->wrlock()` |
| sql/rpl_rli.cc | `Relay_log_info::wait_for_gtid_set` | 「等待目标 GTID 集合被应用」的判定：要求 `wait_gtid_set->is_subset(executed_gtids)` 且与 `owned_gtids` 无交集 |
| sql/rpl_replica_until_options.cc | `Until_after_gtids::check_at_start_slave` | `UNTIL AFTER GTIDS` 的启动检查，判定条件是 `m_gtids.is_subset(gtid_state->get_executed_gtids())` |
| sql/rpl_replica_until_options.cc | `Until_before_gtids::check_at_start_slave` | `UNTIL BEFORE GTIDS` 的启动检查，判定条件是 `m_gtids.is_intersection_nonempty(gtid_state->get_executed_gtids())` |
| sql/binlog.cc | `MYSQL_BIN_LOG::get_gtid_executed` | 在 `LOCK_commit` 与 `global_sid_lock` 双锁下把 `gtid_state->get_executed_gtids()` 并入目标集合 |
| sql/binlog.cc | `MYSQL_BIN_LOG::find_first_log_not_in_gtid_set` | 倒序遍历 binlog，用 `binlog_previous_gtid_set.is_subset(gtid_set)` 定位第一个仍含未执行 GTID 的 binlog |
| sql/mysqld.cc | `init_gtid_sets` | 服务器启动时用 `is_subset` / `add_gtid_set` / `remove_gtid_set` 比对 binlog 与 `gtid_executed` 表，重建 `executed_gtids`、`gtids_only_in_table`、`lost_gtids`、`previous_gtids_logged` |

## 与主核 / AI 的关系

主核（`engine/db/`）与本页的 GTID 集合没有代码级关联，但机制形态上有可对照之处：

- **两者都把「已完成的序列」压成区间而不是逐条存储。** 本页是 `Interval{start, end}` 链表 + `add_gno_interval` 的合并逻辑；主核侧若也有类似「已提交 LSN / 已应用序号」的集合，其表示方式需要另行核对主核源码。**架构推论：** 主核是否存在等价结构本页不作断言。
- **两者都把「集合运算」与「持久化」分层。** 本页的 `Gtid_set` 完全不含落盘逻辑，落盘在 `Gtid_state` / `Gtid_table_persistor` 一侧（见 M04）。**架构推论：** 主核 WAL / 事务状态若有对应分层，其边界是否如此清晰需另行核对主核源码，本页不作断言。
- **本页有一处对 AI / 辅助工具友好的性质：`Gtid_set` 的所有运算都是纯函数式的集合运算，没有副作用。** `is_subset`、`is_intersection_nonempty`、`equals` 只读不写，锁也只是断言调用方已持有。这意味着**「本集合是否被某集合包含」这类问题可以被独立地提取成一个纯判定工具**，不需要启动服务器、不需要连库。

## 参考

- [sql/rpl_gtid_set.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_gtid_set.cc) —— 本页全部符号的权威来源：`Gtid_set::add_gtid_set`、`Gtid_set::is_subset`、`add_gno_interval`、`remove_gno_interval`、`add_gtid_text`、`encode` / `add_gtid_encoding` 等。
- [sql/rpl_gtid.h（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_gtid.h) —— `Gtid_set` 类定义、`Interval` 结构、`CHUNK_GROW_SIZE`、`GNO_END` / `GNO_WARNING_THRESHOLD` / `MAX_GNO_TEXT_LENGTH`、`Free_intervals_lock` 的定义处。
- [sql/rpl_gtid_state.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_gtid_state.cc) —— `Gtid_set` 的主要使用方之一：`Gtid_state::add_lost_gtids`、`update_prev_gtids`、`save_gtids_of_last_binlog_into_table`。
- [sql/rpl_replica_until_options.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_replica_until_options.cc) —— `is_subset` 与 `is_intersection_nonempty` 的真实调用点：`Until_after_gtids::check_at_start_slave` 与 `Until_before_gtids::check_at_start_slave`。
- [sql/rpl_rli.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_rli.cc) —— `Relay_log_info::add_gtid_set` 与 `Relay_log_info::wait_for_gtid_set` 的实现，后者是「等 GTID 集合被应用」的核心判定。
- [Replication with Global Transaction Identifiers（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/replication-gtids.html) —— GTID 的官方定义与基于 GTID 的复制模型，是本页「机制是什么」中 SID/GNO 概念的上位依据。
- [Global Transaction Identifiers (GTIDs)（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/gtid.html) —— GTID 集合的官方文本语法（`uuid:gno[:gno-end]`、逗号分隔多 SID），对应本页 `add_gtid_text` 解析的格式。
- [The Binary Log（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/binary-log.html) —— binlog 与 GTID 事件的上位背景。
