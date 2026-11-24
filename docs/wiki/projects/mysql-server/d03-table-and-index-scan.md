---
project: mysql-server
feature: table-and-index-scan
page_type: feature
schema_version: 1
refs:
  - path: sql/iterators/basic_row_iterators.cc
    symbols: [TableScanIterator::Init, TableScanIterator::Read]
  - path: sql/range_optimizer/index_range_scan.cc
    symbols: [IndexRangeScanIterator::Init, IndexRangeScanIterator::Read]
---

# 全表扫描与索引扫描

## 一句话职责

全表扫描与索引扫描是执行器里两类「叶子取行」实现：`TableScanIterator` 用 `ha_rnd_init`/`ha_rnd_next` 把存储引擎返回的整表原始行按扫描方向依次送出，`IndexRangeScanIterator` 用 `ha_index_init`/`ha_multi_range_read_next` 在一组已算好的 `QUICK_RANGE` 上做边界判定并逐段取行；两者只负责「把行从引擎搬出来、按范围裁剪、按方向输出」，不负责优化器为什么选这条路，也不负责存储引擎内部如何定位记录。

## 机制是什么

**这一层是「执行器 ↔ 存储引擎」之间最薄的一层翻译：把引擎的游标语义翻译成 `RowIterator` 的 `Init()`/`Read()` 协议。**

同一个 `RowIterator` 协议下，取行方式按「引擎提供的游标类型」分成几族：`TableScanIterator` 走引擎的随机读游标（rnd），`IndexScanIterator<Reverse>` 走引擎的单索引游标（index），`IndexRangeScanIterator` 走引擎的多范围读游标（MRR）。本页覆盖前两族中最基础的两个：`TableScanIterator` 与 `IndexRangeScanIterator`。

`sql/iterators/basic_row_iterators.h` 里 `TableScanIterator` 的类注释给出了它的定位：「从表头扫到表尾。这是最基本的表访问方式，使用 `rnd_init`、`ha_rnd_next` 和 `rnd_end`。**不使用任何索引**。」

`sql/iterators/basic_row_iterators.h` 里 `IndexScanIterator<Reverse>` 的类注释则给出另一个极端：「沿一个索引做一次完整的索引扫描。」它的模板参数 `Reverse` 决定方向：`IndexScanIterator<false>` 是正向（`ha_index_first` 起、`ha_index_next` 续），`IndexScanIterator<true>` 是反向（`ha_index_last` 起、`ha_index_prev` 续）。

`sql/range_optimizer/index_range_scan.cc` 里的 `IndexRangeScanIterator` 则是第三种形态：它不是「扫完一个索引」，而是「在索引上扫若干段区间」。它持有一个 `ranges`（`Bounds_checked_array<QUICK_RANGE *>`）数组，构造函数里把 `key_nr` 记进 `index`、把 `table()->key_info[index].key_part` 记进 `key_part_info`、把 `table()->file` 记进 `file`。

**这三者的共同点是：它们都不做行内容过滤，只做「取哪些行」的边界判定。** `TableScanIterator` 的边界判定是「表尾」，`IndexScanIterator` 的边界判定是「索引尾」，`IndexRangeScanIterator` 的边界判定是「`QUICK_RANGE` 的上下界」。真正的列值过滤在上一层（`FilterIterator`）完成。

## 怎么实现的

### `TableScanIterator::Init`：建立随机读游标

`sql/iterators/basic_row_iterators.cc` 里 `TableScanIterator::Init()` 的执行顺序是固定的四步：

1. `empty_record(table())` —— 清空记录缓冲区的内容。
2. 算 `const bool first_init = !table()->file->inited;`。源码注释写明理由：「只在 handler 第一次初始化时才尝试分配记录缓冲。」
3. `int error = table()->file->ha_rnd_init(true);` —— 向存储引擎请求一个随机读游标，出错则 `PrintError(error)` 并返回 `true`（表示初始化失败）。`ha_rnd_init` 的 `scan` 参数在 `sql/handler.cc` 的 `handler::ha_rnd_init` 注释里有明确定义：「if true: Initialize for random scans through `rnd_next()`；if false: Initialize for random reads through `rnd_pos()`」。这里传 `true`，即**扫描模式而非定位模式**。
4. 若 `first_init` 为真，则调 `set_record_buffer(table(), m_expected_rows)`；再把 `m_stored_rows = 0` 置零，返回 `false`。

`sql/handler.cc` 的 `handler::ha_rnd_init` 函数体还补了两件事：`assert(inited == NONE || (inited == RND && scan));` 保证重复初始化只在扫描模式下被允许；`inited = (result = rnd_init(scan)) ? NONE : RND;` 成功后把 handler 状态置为 `RND`。

`TableScanIterator` 的析构函数（`sql/iterators/basic_row_iterators.cc`）负责收尾：`if (table()->file != nullptr) { table()->file->ha_index_or_rnd_end(); }` —— 注意它用的是 `ha_index_or_rnd_end` 这个「索引或随机读都能关」的通用版本。

### `TableScanIterator::Read`：逐行取与两个特殊分支

`TableScanIterator::Read()` 用 `table()->file->ha_rnd_next(m_record)` 取行，整个函数体分成**两个分支**。

**分支一（`table()->is_union_or_table()` 为真）：** 一次 `ha_rnd_next` 成功即返回。循环体内只处理一个错误码：

```
while ((tmp = table()->file->ha_rnd_next(m_record))) {
  /*
   ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is
   reading and another deleting without locks.
  */
  if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed) continue;
  return HandleError(tmp);
}
if (m_examined_rows != nullptr) {
  ++*m_examined_rows;
}
```

即 `HA_ERR_RECORD_DELETED`（MyISAM 在「一个线程在读、另一个线程在无锁删除」时返回）被**跳过继续读**，而 `thd()->killed` 为真时不再跳过、直接进 `HandleError`。成功取到行后 `m_examined_rows` 自增——`m_examined_rows` 是构造时传入的指针，为 `nullptr` 时不计。

**分支二（否则，即 `INTERSECT`/`EXCEPT` 场景）：** 这一分支多了一套「重复行计数」逻辑，源码注释说明它只用于这两种集合运算：

- `m_remaining_dups == 0` 时先照常 `ha_rnd_next` 取一行（同样跳过 `HA_ERR_RECORD_DELETED`），然后读 `const ulonglong cnt = static_cast<ulonglong>(table()->set_counter()->val_int());` 拿到该行的重复计数。
- `table()->is_except()` 为真时：`is_distinct()` 为真（`EXCEPT DISTINCT`）则「任何大于一的计数恰好产出一行」，判据是 `if (cnt >= 1) break;`；否则（`EXCEPT ALL`）用 `m_remaining_dups = cnt;` 记录还要再产几次。
- `table()->is_except()` 为假时（即 `INTERSECT`）：`is_distinct()` 为真则 `if (cnt == 0) break;`（计数为零说明右侧没有该行，丢弃）；否则（`INTERSECT ALL`）用 `HalfCounter c(cnt); m_remaining_dups = std::min(c[0], c[1]);` 取左右两侧计数的较小值。
- `m_remaining_dups != 0` 时走 else 分支：`--m_remaining_dups;` 并 `break;` —— 把同一行再产一次。

`HalfCounter` 是 `sql/iterators/basic_row_iterators.h` 里定义的一个 `union`，把 64 位计数拆成两个 32 位使用。它的类注释写明用途是「为 `INTERSECT ALL` 挤进两个 32 位整数」，并明确写出了实现限制：「`INTERSECT ALL` 的结果表任何一行的重复数必须不超过 `uint32_t::max`。」

**行数上限：** 分支二末尾有 `if (++m_stored_rows > m_limit_rows) { return HandleError(HA_ERR_END_OF_FILE); }`。`m_limit_rows` 的来源在构造函数里：

```
m_limit_rows(table->set_counter() != nullptr ? table->m_limit_rows
                                             : HA_POS_ERROR)
```

即**只有表带了计数器（`set_counter() != nullptr`）时 `m_limit_rows` 才生效**，否则取 `HA_POS_ERROR`（表示无上限）。`sql/iterators/basic_row_iterators.h` 的成员注释解释了原因：「用于 `EXCEPT` 和 `INTERSECT` 专用：我们无法像 `UNION` 和单表那样在物化期间强制 limit，所以必须在扫描期间做。」

### `TableScanIterator::Read` 的错误收敛：走 `HandleError`

`TableScanIterator::Read()` 里的所有错误都交给基类 `TableRowIterator::HandleError(int error)`（`sql/iterators/basic_row_iterators.cc`）翻译成迭代器协议的三态：

```
int TableRowIterator::HandleError(int error) {
  if (thd()->killed) {
    thd()->send_kill_message();
    return 1;
  }

  if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
    m_table->set_no_row();
    return -1;
  } else {
    PrintError(error);
    return 1;
  }
}
```

三个要点：**(1)** `thd()->killed` 优先于一切，命中就发 kill 消息并返回 `1`；**(2)** `HA_ERR_END_OF_FILE` 与 `HA_ERR_KEY_NOT_FOUND` 都被视为 EOF，返回 `-1` 并调 `m_table->set_no_row()`；**(3)** 其余错误走 `PrintError(error)`（实现是 `m_table->file->print_error(error, MYF(0))`）并返回 `1`。

### `IndexRangeScanIterator::Init`：分叉的两条初始化路径

`sql/range_optimizer/index_range_scan.cc` 里 `IndexRangeScanIterator::Init()` 的第一处分叉在 `need_rows_in_rowid_order` 上：

```
if (!inited) {
  if (need_rows_in_rowid_order ? init_ror_merged_scan() : shared_init()) {
    return true;
  }
  inited = true;
} else {
  if (file->inited) file->ha_index_or_rnd_end();
}
```

- **首次初始化**：需要按 rowid 顺序返回行时走 `init_ror_merged_scan()`，否则走 `shared_init()`。`shared_init()` 的函数体做两件事：若 `column_bitmap.bitmap == nullptr` 则从 `mem_root` 分配一块 `table()->s->column_bitmap_size` 大小的位图并 `bitmap_init(&column_bitmap, bitmap, table()->s->fields)`；然后 `if (file->inited) file->ha_index_or_rnd_end();` 关掉旧游标。
- **重复初始化**（`Init()` 可被反复调用）：`if (file->inited) file->ha_index_or_rnd_end();` 关掉旧游标，随后 `shared_reset()` 重建。

两条路径之后都汇合到 `shared_reset()`。

### `shared_reset()`：建立 MRR 游标与缓冲

`sql/range_optimizer/index_range_scan.cc` 里 `IndexRangeScanIterator::shared_reset()` 的顺序是：

1. `last_range = nullptr; cur_range = ranges.begin();` —— 重置区间游标。
2. 调 `InitIndexRangeScan(table(), file, index, mrr_flags, in_ror_merged_scan, &column_bitmap)`。这个自由函数的函数体决定了两件事：

   - **keyread 的开关**：注释写明「如果索引是覆盖的就把 keyread 置真」——`if (!table->no_keyread && table->covering_keys.is_set(index)) table->set_keyread(true); else table->set_keyread(false);`。`sql/table.cc` 的 `TABLE::set_keyread(bool flag)` 实现是：`key_read = true` 时若表已创建则调 `file->ha_extra(HA_EXTRA_KEYREAD)`，置假时调 `file->ha_extra(HA_EXTRA_NO_KEYREAD)`。
   - **索引游标的建立**：`const bool sorted = (mrr_flags & HA_MRR_SORTED);` 决定是否要按序返回，然后 `if (int error = file->ha_index_init(index, sorted); error != 0) { (void)report_handler_error(table, error); return true; }`。`sql/handler.cc` 的 `handler::ha_index_init(uint idx, bool sorted)` 在 `index_init` 成功后把 `inited = INDEX;`，并把 `mrr_have_range = false; end_range = nullptr;`。

   该函数还有一段针对**多值索引**（`HA_MULTI_VALUED_KEY`）的处理：调 `file->ha_extra(HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER)`，并在 `table->s->primary_key != MAX_KEY` 时把主键的列加进 `read_set`，注释说明原因是「唯一性过滤器用 rowid 跳过重复行」。

3. **分配 MRR 缓冲**：`if (mrr_buf_size && !mrr_buf_desc)` 时用 `my_multi_malloc` 一次分配描述符与缓冲区。分配失败时的退避逻辑写在循环里：

   ```
   while (buf_size &&
          !my_multi_malloc(key_memory_IndexRangeScanIterator_mrr_buf_desc,
                           MYF(MY_WME), &mrr_buf_desc, sizeof(*mrr_buf_desc),
                           &mrange_buff, buf_size, NullS)) {
     /* Try to shrink the buffers until both are 0. */
     buf_size /= 2;
   }
   ```

   即**每次失败把缓冲区大小减半**，直到成功或减到 0；`mrr_buf_desc` 仍为空则 `table()->file->print_error(HA_ERR_OUT_OF_MEM, MYF(0))` 并返回 `true`。分配成功后填 `mrr_buf_desc->buffer` / `buffer_end` / `end_of_used_area` 三个字段。

4. **建立范围序列**：`RANGE_SEQ_IF seq_funcs = {quick_range_seq_init, quick_range_seq_next, nullptr};` 然后

   ```
   if (int error = file->multi_range_read_init(
           &seq_funcs, this, ranges.size(), mrr_flags,
           mrr_buf_desc ? mrr_buf_desc : &empty_buf);
       error != 0) {
     (void)report_handler_error(table(), error);
     return true;
   }
   ```

   没有缓冲时用 `HANDLER_BUFFER empty_buf;`（三个字段全置 `nullptr`）顶替。`sql/handler.cc` 的 `handler::multi_range_read_init` 实现是：`mrr_iter = seq_funcs->init(seq_init_param, n_ranges, mode); mrr_funcs = *seq_funcs; mrr_is_output_sorted = mode & HA_MRR_SORTED; mrr_have_range = false;`——**`sorted` 状态就来自 `mode` 里的 `HA_MRR_SORTED` 位**。

### `quick_range_seq_next`：区间 → handler 读区间 的翻译

`sql/range_optimizer/index_range_scan.cc` 里的 `quick_range_seq_next(range_seq_t rseq, KEY_MULTI_RANGE *range)` 是「区间序列回调」，把一个 `QUICK_RANGE` 翻译成 handler 能理解的 `key_range`：

- 起点：`start_key->flag = ((cur->flag & NEAR_MIN) ? HA_READ_AFTER_KEY : (cur->flag & EQ_RANGE) ? HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT);`
- 终点：`end_key->flag = (cur->flag & NEAR_MAX ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY);`。源码注释解释为什么终点也用 `HA_READ_AFTER_KEY`：「我们这里用 `HA_READ_AFTER_KEY` 是因为我们是在一个键前缀上读。我们想找到所有具有这个前缀的键。」
- `range->range_flag = cur->flag;`，然后 `ctx->cur++;`。

`quick_range_seq_init` 做的是把 `ranges.begin()`/`ranges.end()` 存进 `qr_traversal_ctx` 的 `first`/`cur`/`last` 三个字段，并返回 `&quick->qr_traversal_ctx`。

`quick_range_seq_next` 的注释还说明了遍历方向规则：「『下一个』指相对于当前一个的下一个区间，当第一个键部分是 ASC 排序时；或者是上一个区间——当键部分是 DESC 排序时。这是为了在多个区间覆盖 DESC 键部分时保持记录顺序正确。」

### `IndexRangeScanIterator::Read`：一次取一行

`sql/range_optimizer/index_range_scan.cc` 里 `IndexRangeScanIterator::Read()` 的函数体很短：

```
char *dummy;
int result = file->ha_multi_range_read_next(&dummy);
```

前后的准备与收尾是：

- **进入时**：若 `in_ror_merged_scan` 为真，先 `table()->column_bitmaps_set_no_signal(&column_bitmap, &column_bitmap);` —— 注释说「我们不需要发出位图变化的信号，因为对这个 `table()->file` 来说位图总是一样的」。
- **离开时**：若 `in_ror_merged_scan` 为真，先 `table()->column_bitmaps_set_no_signal(save_read_set, save_write_set);` 恢复入口处的位图；**并且在 `result == 0` 时调 `file->position(table()->record[0]);`** —— 把游标定位到刚取到的行上，这是 ROR 合并扫描能把行按 rowid 顺序归并的前提。
- **返回**：`if (result == 0) { if (m_examined_rows != nullptr) ++*m_examined_rows; return 0; }`，否则 `return HandleError(result);`。

`sql/handler.cc` 的 `handler::ha_multi_range_read_next` 在调用引擎的 `multi_range_read_next` 之外还做了两件事：若 `table->has_gcol()` 则 `m_update_generated_read_fields = true`，并在读到行后调 `update_generated_read_fields(table->record[0], table, active_index)` 更新生成列的值；最后调 `table->set_row_status_from_handler(result)`。

### 边界判定辅助：`row_in_ranges` 与 `cmp_next`/`cmp_prev`

`sql/range_optimizer/index_range_scan.cc` 里的 `IndexRangeScanIterator::row_in_ranges()` 用于判断「当前行是否会被这个区间扫描取到」。它的注释写明了算法与用途：「实现是在一组已排序的互不相交区间上做二分查找，不考虑区间的大小。」

函数体是一个标准二分：初值 `min = 0; max = ranges.size() - 1; mid = (max + min) / 2;`，循环条件是 `while (min != max)`，每轮用 `cmp_next(ranges[mid])` 判断「当前行值是否大于 `mid` 的上界」——为真则 `min = mid + 1`，否则 `max = mid`。收敛后 `res = ranges[mid];`，返回 `(!cmp_next(res) && !cmp_prev(res))`。

两个比较函数：

- `cmp_next(QUICK_RANGE *range_arg)`：注释是「比较找到的键是否超过最大值，返回 0 表示键 <= `range->max_key`」。函数体：`if (range_arg->flag & NO_MAX_RANGE) return 0;`（注释「键不会大于上界」）；否则 `cmp = key_cmp(key_part_info, range_arg->max_key, range_arg->max_length);`，`if (cmp < 0 || (cmp == 0 && !(range_arg->flag & NEAR_MAX))) return 0;`，否则返回 `1`。
- `cmp_prev(QUICK_RANGE *range_arg)`：注释是「返回 0 表示找到的键在区间内（找到的键 >= `range->min_key`）」。函数体：`if (range_arg->flag & NO_MIN_RANGE) return 0;`；否则 `cmp = key_cmp(key_part_info, range_arg->min_key, range_arg->min_length);`，`if (cmp > 0 || (cmp == 0 && !(range_arg->flag & NEAR_MIN))) return 0;`，否则返回 `1`。

**`row_in_ranges()` 的真实调用方是 `sql/range_optimizer/index_merge.cc` 里的 `IndexMergeIterator`。** 该文件中的调用点是：

```
/* skip row if it will be retrieved by clustered PK scan */
if (pk_quick_select && down_cast<IndexRangeScanIterator *>(
                           pk_quick_select.get()->real_iterator())
                           ->row_in_ranges()) {
  continue;
}
```

同一文件里的注释说明了语义：「用于在 `index_merge` quick select 中过滤掉聚集主键扫描会取到的行。」并且 `IndexMergeIterator::Read()` 上方的注释写明：「读取的行来自：(1) 存进 `Unique` 的 rowid；(2) 带聚集主键的 `IndexRangeScanIterator`（如果有）。这两组取到的行保证互不相交。」

### 记录缓冲：`set_record_buffer` 的尺寸规则

两个迭代器都会调 `set_record_buffer(table(), m_expected_rows)`，它的实现位于 `sql/sql_executor.cc`。与本页相关的三条硬规则：

- `if (expected_rows_to_fetch <= 1.0) return false;` —— 期望行数不超过 1 时**不建缓冲**。
- `if (!table->file->ha_is_record_buffer_wanted(&max_rows) || max_rows == 0) return false;` —— **只有存储引擎主动要缓冲才建**。
- `if (rows_in_buffer <= 1) return false;` —— 调整后仍不足 2 行则不建。

尺寸常量定义在 `sql/sql_executor.cc` 里：

- `static constexpr size_t MIN_RECORD_BUFFER_SIZE = 4 * 1024;` —— 4KB。注释说明「如果所有行（估算）能用更小的缓冲装下，就只分配需要的尺寸；否则 `set_record_buffer()` 会把尺寸调整到最小尺寸，以便小范围扫描用更小的缓冲」，并指出「这个值不应太高，基准测试显示缓冲过大会在高并发场景下损害性能」。
- `static constexpr size_t MAX_RECORD_BUFFER_SIZE = 128 * 1024;` —— 128KB。注释写明「更大的缓冲不会带来明显更好的性能，而过大的缓冲在高并发场景下已被观察到会损害性能」。
- `static constexpr double RECORD_BUFFER_FRACTION = 0.1f;` —— 缓冲只按估算返回行数的 10% 分配。注释解释了为什么不直接按「装得下全部行」来分配：「对小范围使用大记录缓冲已被观察到在高并发场景下损害性能。所以当范围不大时我们想把缓冲尺寸拉向最小缓冲尺寸，而当范围大、表扫描时又把它拉向最大缓冲尺寸。」

尺寸计算：`const size_t record_size = record_prefix_size(table);`（记录前缀大小，即从记录开头到 `read_set` 中最后一个被读列所需的空间），`const ha_rows min_rows = std::ceil(double{MIN_RECORD_BUFFER_SIZE} / record_size);`；若 `expected_rows < min_rows` 则 `rows_in_buffer = expected_rows`（只分配需要的），否则 `rows_in_buffer = std::ceil(rows_in_buffer * RECORD_BUFFER_FRACTION);` 再 `const ha_rows local_max_rows = (MAX_RECORD_BUFFER_SIZE / record_size); rows_in_buffer = std::clamp(rows_in_buffer, min_rows, local_max_rows);` 把结果夹在最小与最大行数之间。

`IndexRangeScanIterator::Init()` 里还有**两条跳过缓冲的特殊情况**，源码注释都给了理由：

- `bool skip_record_buffer = need_rows_in_rowid_order && Overlaps(table()->file->ha_table_flags(), HA_PRIMARY_KEY_REQUIRED_FOR_POSITION) && has_blob_primary_key(table());` —— 注释解释：「按 rowid 顺序取行会在更晚的阶段把主键加进 `read_set`。如果主键含 BLOB 成分，就不能使用记录缓冲，因为 BLOB 需要记录之外的存储空间。所以即使当前 `read_set` 看起来用记录缓冲没问题，也不要申请缓冲。」`has_blob_primary_key` 是同一文件里的静态辅助函数，用 `Overlaps(key_part.key_part_flag, HA_BLOB_PART)` 逐个键部分判断。
- `skip_record_buffer |= Overlaps(table()->key_info[index].flags, HA_MULTI_VALUED_KEY) && table()->key_read;` —— 注释解释：「跳过覆盖式多值索引范围扫描的记录缓冲。`Field_typed_array::key_cmp()` 的当前实现需要被索引表达式的生成列的值，而这个列在多值索引里不可用，所以覆盖扫描时存储引擎无法安全地在填充记录缓冲时求值区间末端条件。」

### 装配：优化器路径如何落到这两个迭代器

`sql/join_optimizer/access_path.cc` 的 `CreateIteratorFromAccessPath()` 按 `path->type` 分派：

- `case AccessPath::TABLE_SCAN:` → `NewIterator<TableScanIterator>(thd, mem_root, param.table, path->num_output_rows(), examined_rows);`
- `case AccessPath::INDEX_SCAN:` → 按 `param.reverse` 分派 `NewIterator<IndexScanIterator<true>>` 或 `NewIterator<IndexScanIterator<false>>`。
- `case AccessPath::INDEX_RANGE_SCAN:` → 三路分派：`param.geometry` 为真走 `NewIterator<GeometryIndexRangeScanIterator>`；`param.reverse` 为真走 `NewIterator<ReverseIndexRangeScanIterator>`；否则走 `NewIterator<IndexRangeScanIterator>`，并传入 `param.index`、`param.need_rows_in_rowid_order`、`param.reuse_handler`、`mem_root`、`param.mrr_flags`、`param.mrr_buf_size` 与 `Bounds_checked_array{param.ranges, param.num_ranges}`。

`examined_rows` 的来源也在同一函数里：`ha_rows *examined_rows = nullptr; if (path->count_examined_rows && join != nullptr) { examined_rows = &join->examined_rows; }`。

## 关键取舍

- **源码事实：全表扫描完全不用索引。** `sql/iterators/basic_row_iterators.h` 的 `TableScanIterator` 类注释写明「不使用任何索引」，实现上也只调 `ha_rnd_init`/`ha_rnd_next`，没有任何索引相关分支。收益是路径最短、不需要处理索引边界；代价是必须读完整张表（或直到 `m_limit_rows` 触发）才能结束。

- **源码事实：`Init()` 与 `Read()` 的错误处理分成两层。** 引擎错误码在 `TableRowIterator::HandleError` 一处翻译成 `-1/0/1` 三态，两个扫描迭代器只做各自的特例处理（`TableScanIterator::Read` 处理 `HA_ERR_RECORD_DELETED`，`IndexRangeScanIterator::Read` 处理 ROR 合并下的位图切换与 `file->position()`）。收益是新增错误码只需改一处；代价是「什么算 EOF」这个决定被固化在基类里（`HA_ERR_END_OF_FILE` 与 `HA_ERR_KEY_NOT_FOUND` 都被算作 EOF）。

- **源码事实：索引扫描的方向是编译期模板参数，不是运行时开关。** `IndexScanIterator<Reverse>` 在 `sql/iterators/basic_row_iterators.cc` 末尾显式实例化了两个特化：`template class IndexScanIterator<true>;` 与 `template class IndexScanIterator<false>;`。正反向的 `Read()` 也是两个显式特化（`IndexScanIterator<false>::Read()` 注释「Forward read.」，`IndexScanIterator<true>::Read()` 注释「Backward read.」）。收益是方向分支在编译期消掉；代价是每多一个方向就要多一个模板实例。

- **源码事实：`IndexRangeScanIterator` 的区间边界判定不在取行路径上。** 取行走 `file->ha_multi_range_read_next()`，边界由 `quick_range_seq_next` 交给引擎处理；`row_in_ranges()` 只在 `IndexMergeIterator` 需要「这行是不是会被聚集主键扫描取到」时作为**旁路二分查找**被调用一次。收益是热路径上没有额外的键比较；代价是多了一套独立的比较逻辑（`cmp_next`/`cmp_prev`）与引擎侧的区间边界判定并存，两者的语义必须靠 `NEAR_MIN`/`NEAR_MAX`/`NO_MIN_RANGE`/`NO_MAX_RANGE`/`EQ_RANGE` 这些 flag 对齐——`sql/range_optimizer/range_optimizer.cc` 里 `QUICK_RANGE` 的默认构造把 `flag` 初始化为 `NO_MIN_RANGE | NO_MAX_RANGE`，正是这套 flag 的起点。

- **源码事实：记录缓冲是有上限且刻意偏小的。** `MIN_RECORD_BUFFER_SIZE` 为 4KB、`MAX_RECORD_BUFFER_SIZE` 为 128KB、`RECORD_BUFFER_FRACTION` 为 0.1，三者都写在 `sql/sql_executor.cc` 里。源码注释两次点明原因：过大的缓冲在高并发场景下损害性能。收益是并发下内存占用可控；代价是小范围扫描时缓冲命中率低。

- **源码事实：`INTERSECT`/`EXCEPT` 的 limit 与重复计数只能在扫描期做。** `sql/iterators/basic_row_iterators.h` 的 `m_limit_rows` 成员注释写明：「我们无法像 `UNION` 和单表那样在物化期间强制 limit，所以必须在扫描期间做。」`HalfCounter` 的注释进一步写明 `INTERSECT ALL` 的实现限制：「结果表任何一行的重复数必须不超过 `uint32_t::max`。」收益是不需要为这两种集合运算再物化一层；代价是把 limit 语义和多值计数都塞进了扫描迭代器。

- **源码事实：ROR 合并扫描要显式保存/恢复 `read_set` 与 `write_set`。** `IndexRangeScanIterator::Read()` 入口保存 `table()->read_set`/`table()->write_set`，若 `in_ror_merged_scan` 则换成区间自己的 `column_bitmap`，离开时恢复；`InitIndexRangeScan` 里也做同样的保存/恢复，并在 `in_ror_merged_scan` 时额外调 `file->ha_extra(HA_EXTRA_KEYREAD_PRESERVE_FIELDS)`。`sql/range_optimizer/index_merge.cc` 里 `IndexMergeIterator` 对每个子迭代器做 `table()->covering_keys.set_bit(range_scan_it->index);` 后调 `child->Init()`，并用 `assert(table->key_read == true);` 断言「只做索引访问」生效。收益是多个区间扫描可以共享同一个 `TABLE` 对象而不互相污染读列集；代价是每次读行都要做两次位图切换。

- **架构推论：** `TableScanIterator` 与 `IndexRangeScanIterator` 之所以都长得「很薄」，是因为 MySQL 把「按什么顺序读」「读到哪一行算结束」这两件事拆给了两拨角色：迭代器负责「怎么调引擎」，优化器负责「调哪个索引、区间怎么切」（区间由 `sql/range_optimizer/range_optimizer.cc` 的 `QUICK_RANGE` 描述并在 `sql/join_optimizer/access_path.cc` 的 `AccessPath::INDEX_RANGE_SCAN` 分支传入）。代价是执行期无法改变扫描目标——想换路径只能重做计划。

- **架构推论：** 把 `row_in_ranges()` 这类「当前行是否在我的区间内」的判断留在迭代器上（而不是让 `IndexMergeIterator` 自己算），意味着区间语义只有一处定义（`cmp_next`/`cmp_prev`），但也意味着迭代器的公开面被 `IndexMergeIterator` 这个具体调用方撑大了一点——`sql/range_optimizer/index_merge.cc` 需要 `down_cast<IndexRangeScanIterator *>(...->real_iterator())` 才能拿到它。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::Init` | 全表扫描初始化：`empty_record` 起步、`ha_rnd_init(true)` 建扫描游标、首次初始化才 `set_record_buffer`、`m_stored_rows` 归零 |
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::Read` | 全表扫描读行：`ha_rnd_next` 取行、跳过 `HA_ERR_RECORD_DELETED`；`INTERSECT`/`EXCEPT` 分支按 `set_counter()` 计数决定重复产出；`m_stored_rows > m_limit_rows` 时按 EOF 处理 |
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::TableScanIterator` | 构造函数：记录 `m_expected_rows`/`m_examined_rows`，并按 `table->set_counter() != nullptr` 决定 `m_limit_rows` 取 `table->m_limit_rows` 还是 `HA_POS_ERROR` |
| `sql/iterators/basic_row_iterators.cc` | `TableScanIterator::~TableScanIterator` | 析构：用 `ha_index_or_rnd_end` 关闭扫描游标 |
| `sql/iterators/basic_row_iterators.cc` | `TableRowIterator::HandleError` | 错误翻译：`HA_ERR_END_OF_FILE` 与 `HA_ERR_KEY_NOT_FOUND` 视为 EOF 返回 `-1`，其余 `PrintError` 后返回 `1`，`killed` 优先 |
| `sql/iterators/basic_row_iterators.cc` | `IndexScanIterator<Reverse>::Init` | 整索引扫描初始化：覆盖索引时 `set_keyread(true)`、`ha_index_init(m_idx, m_use_order)`、首次初始化才 `set_record_buffer` |
| `sql/iterators/basic_row_iterators.cc` | `IndexScanIterator<false>::Read` | 正向整索引扫描：`ha_index_first` 起、`ha_index_next` 续 |
| `sql/iterators/basic_row_iterators.cc` | `IndexScanIterator<true>::Read` | 反向整索引扫描：`ha_index_last` 起、`ha_index_prev` 续 |
| `sql/iterators/basic_row_iterators.h` | `TableScanIterator` | 全表扫描迭代器声明：类注释写明「不使用任何索引」，成员含 `m_remaining_dups`、`m_limit_rows`、`m_stored_rows` |
| `sql/iterators/basic_row_iterators.h` | `HalfCounter` | 把 64 位重复计数拆成两个 32 位计数，供 `INTERSECT ALL` 取左右两侧较小值 |
| `sql/range_optimizer/index_range_scan.cc` | `IndexRangeScanIterator::Init` | 区间扫描初始化：按 `need_rows_in_rowid_order` 分派 `init_ror_merged_scan`/`shared_init`，重复 `Init()` 时先 `ha_index_or_rnd_end`；两种跳过记录缓冲的特殊情况（BLOB 主键、覆盖式多值索引）在此判定 |
| `sql/range_optimizer/index_range_scan.cc` | `IndexRangeScanIterator::Read` | 区间扫描读行：`ha_multi_range_read_next` 取行；ROR 合并下切换/恢复 `read_set`/`write_set` 并在成功时 `file->position(table()->record[0])` |
| `sql/range_optimizer/index_range_scan.cc` | `IndexRangeScanIterator::shared_reset` | 重建 MRR 游标：`InitIndexRangeScan` 建索引游标、MRR 缓冲分配失败时逐次减半、`multi_range_read_init` 注册区间序列回调 |
| `sql/range_optimizer/index_range_scan.cc` | `InitIndexRangeScan` | 按覆盖性设置 keyread、`ha_index_init(index, sorted)`；多值索引时开唯一性过滤器并把主键列加进 `read_set` |
| `sql/range_optimizer/index_range_scan.cc` | `quick_range_seq_next` | 把 `QUICK_RANGE` 翻译成 `KEY_MULTI_RANGE`：`NEAR_MIN`/`EQ_RANGE` 决定起点 flag，`NEAR_MAX` 决定终点 flag |
| `sql/range_optimizer/index_range_scan.cc` | `IndexRangeScanIterator::row_in_ranges` | 在已排序互不相交的区间数组上二分，判断当前行是否会被本扫描取到 |
| `sql/range_optimizer/index_range_scan.cc` | `IndexRangeScanIterator::cmp_next` / `cmp_prev` | 区间上下界比较：`NO_MAX_RANGE`/`NO_MIN_RANGE` 直接返回 0，其余用 `key_cmp` 比 `max_key`/`min_key` 并结合 `NEAR_MAX`/`NEAR_MIN` |
| `sql/range_optimizer/index_range_scan.cc` | `has_blob_primary_key` | 静态辅助：判断表的主键是否含 BLOB 成分（`HA_BLOB_PART`），用于判定能否使用记录缓冲 |
| `sql/range_optimizer/index_merge.cc` | `IndexMergeIterator` | `row_in_ranges()` 的真实调用方：跳过会被聚集主键扫描取到的行，保证两组行互不相交 |
| `sql/handler.cc` | `handler::ha_rnd_init` | 随机读游标入口：`scan` 参数区分 `rnd_next()` 扫描模式与 `rnd_pos()` 定位模式，成功后置 `inited = RND` |
| `sql/handler.cc` | `handler::ha_index_init` | 索引游标入口：`index_init` 成功后置 `inited = INDEX`，并清 `mrr_have_range`/`end_range` |
| `sql/handler.cc` | `handler::multi_range_read_init` | MRR 初始化：`mrr_is_output_sorted = mode & HA_MRR_SORTED`，`mrr_have_range = false` |
| `sql/handler.cc` | `handler::ha_multi_range_read_next` | MRR 取行包装：按 `table->has_gcol()` 更新生成列，调 `table->set_row_status_from_handler(result)` |
| `sql/sql_executor.cc` | `set_record_buffer` | 记录缓冲分配：`expected_rows_to_fetch <= 1.0` 不建、引擎不要则不建、行数不足 2 不建；按 `MIN_RECORD_BUFFER_SIZE`/`MAX_RECORD_BUFFER_SIZE`/`RECORD_BUFFER_FRACTION` 定尺寸 |
| `sql/table.cc` | `TABLE::set_keyread` | 切换 `key_read` 标志，并相应调 `ha_extra(HA_EXTRA_KEYREAD)` 或 `ha_extra(HA_EXTRA_NO_KEYREAD)` |
| `sql/range_optimizer/range_optimizer.cc` | `QUICK_RANGE::QUICK_RANGE` | 区间对象构造：默认 `flag` 为 `NO_MIN_RANGE | NO_MAX_RANGE`，另有带上下界与 flag 的重载 |
| `sql/join_optimizer/access_path.cc` | `CreateIteratorFromAccessPath` | 路径到迭代器的装配：`TABLE_SCAN`→`TableScanIterator`、`INDEX_SCAN`→`IndexScanIterator<Reverse>`、`INDEX_RANGE_SCAN`→`IndexRangeScanIterator`/`ReverseIndexRangeScanIterator`/`GeometryIndexRangeScanIterator` |

## 与主核 / AI 的关系

主核（`engine/db`）的扫描路径目前是固定形态：按表句柄顺序取行、逐行比对谓词，没有「同一张表的多种扫描策略」这层抽象，也没有把「扫描目标」与「扫描执行」分开。

**架构推论：** MySQL 这一层有三个结构值得主核借鉴。其一是**「扫描目标」与「扫描执行」的分离**：`TableScanIterator` / `IndexScanIterator` / `IndexRangeScanIterator` 三个迭代器形态对应 `AccessPath` 的三个类型，区间在优化期算好、执行期只负责按区间取行——主核若把「区间」也作为一等数据结构（而不是把谓词直接塞进扫描函数），就能在不重做计划的前提下复用同一套区间比较逻辑。其二是**「错误翻译集中在基类」**：`HandleError` 把 `HA_ERR_END_OF_FILE`/`HA_ERR_KEY_NOT_FOUND` 统一映射成 `-1`，主核若有多存储引擎，同样需要一个这样的收敛点，否则每个扫描实现都要自己判断「什么算扫完了」。其三是**「记录缓冲显式设上限」**：`set_record_buffer` 用 4KB/128KB/10% 三个常量把缓冲尺寸夹住，注释给出的理由是「过大的缓冲在高并发下有害」——主核在多模态场景下要缓存的记录比行宽得多，这条规则同样适用。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「同一条语义有多少种执行形态」以及「AI 该不该信任自己的成本估计」。**架构推论：** MySQL 的 `EXPLAIN` 能区分 `ALL`（全表扫描）与 `range`（区间扫描），而区间扫描的实际行数由 `m_examined_rows` 逐行累加得到——这给了 AI 一个可验证的信号：让模型自己写的 SQL 与数据库实际扫描的行数做对比，是发现「模型以为走索引、实际全表扫」这类问题的直接手段。主核若要让 AI 参与执行策略选择，应当把「每种扫描策略实际读了多少行」做成可查询的结构化对象。

## 参考

- [MySQL 源码：`sql/iterators/basic_row_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/basic_row_iterators.cc) —— `TableScanIterator::Init`/`Read`、`IndexScanIterator` 的正反向特化、`TableRowIterator::HandleError` 的完整实现。
- [MySQL 源码：`sql/iterators/basic_row_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/basic_row_iterators.h) —— `TableScanIterator` 的类注释与成员语义、`HalfCounter` 的实现限制。
- [MySQL 源码：`sql/range_optimizer/index_range_scan.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/range_optimizer/index_range_scan.cc) —— `IndexRangeScanIterator::Init`/`Read`、`shared_reset`、`quick_range_seq_next`、`row_in_ranges` 与区间比较函数。
- [MySQL 源码：`sql/range_optimizer/index_merge.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/range_optimizer/index_merge.cc) —— `row_in_ranges()` 的真实调用方与「两组行互不相交」的保证。
- [MySQL 源码：`sql/join_optimizer/access_path.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.cc) —— `AccessPath` 到各扫描迭代器的装配分派。
- [MySQL 源码：`sql/handler.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/handler.cc) —— `handler::ha_rnd_init`/`ha_index_init`/`multi_range_read_init`/`ha_multi_range_read_next` 的引擎侧语义。
- [MySQL 源码：`sql/sql_executor.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_executor.cc) —— `set_record_buffer` 与 `MIN_RECORD_BUFFER_SIZE`/`MAX_RECORD_BUFFER_SIZE`/`RECORD_BUFFER_FRACTION` 三个常量及其取舍说明。
- [MySQL 参考手册：EXPLAIN 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html) —— `ALL`/`index`/`range` 等访问类型与实际扫描形态的对应关系。
- [MySQL 参考手册：Index Merge 优化](https://dev.mysql.com/doc/refman/8.0/en/index-merge-optimization.html) —— `IndexMergeIterator` 组合多个区间扫描的语义。
- [MySQL 参考手册：InnoDB 物理结构](https://dev.mysql.com/doc/refman/8.0/en/innodb-physical-structure.html) —— 聚簇索引与二级索引的组织方式，解释区间扫描取到的是索引记录还是行。
