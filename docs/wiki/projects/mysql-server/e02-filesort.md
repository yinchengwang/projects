---
project: mysql-server
feature: filesort
page_type: feature
schema_version: 1
refs:
  - path: sql/filesort.cc
    symbols: [Filesort::make_sortorder, Filesort::clear_addon_fields]
  - path: sql/filesort_utils.cc
    symbols: [Filesort_buffer::allocate_block]
---

# 文件排序

## 一句话职责

`sql/filesort.cc` 的 `filesort()` 负责把「ORDER BY 的结果集」落到一个有序序列上：当排序键无法直接由索引顺序提供时，它把每一行按 `ORDER` 列表编码成**排序键（sort key）**，按会话变量 `thd->variables.sortbuff_size` 指定的内存预算装进 `Filesort_buffer`，装不下就把已排序的一段**溢写（spill）成磁盘上的 chunk**，最后用**归并（merge）**把多段 chunk 合流成有序输出；`sql/filesort_utils.cc` 的 `Filesort_buffer::allocate_block()` 负责这段内存缓冲区的分块分配与「装满即报错」的判定。这一层只管排序的落地形态，不决定最终返回多少行（那是 E03 的 LIMIT 截断），也不定义中间结果的字段结构（那是 E01 的临时表）。

## 机制是什么

**filesort 是 MySQL 的通用外部排序器，不是「用了临时表」的同义词。** 它的输出有三种可能的物理形态，且这个选择是在**排序执行时**而不是优化时做出的：

1. **全内存**：所有行装得进 `Filesort_buffer`，结果留在排序缓冲区里，读取时走内存迭代器。
2. **磁盘归并**：装不下，内存里的若干段被排序后溢写成磁盘 chunk，再多轮归并。
3. **优先级队列（Priority Queue，PQ）**：有 `LIMIT` 且行数估计足够小时，只保留「当前最小的 N 行」的堆，内存占用与 `LIMIT` 成正比而不是与结果集总行数成正比。

**排序键的编码方式**由 `sortlength()` 决定，它是整个机制的骨架。`sortlength()` 按 `ORDER` 列表逐项计算每段的字节数并累加为 `total_length`：

- 字符串类型走 `STRING_RESULT` 分支：先取 `item->max_length`，若字符集的 `pad_attribute == NO_PAD` 则标记 `is_varlen = true`（变长键）；随后用 `cs->coll->strnxfrmlen(cs, sortorder->length)` 算出含排序权重（sort weight）的实际字节数。**只有当 `sortorder->length < (10 << 20)`（源码注释写作 "10 MB"）时才调用 `strnxfrmlen`**，否则直接把长度设成 `0xFFFFFFFFu`，注释写明这是为了「不要在 `strnxfrmlen()` 里溢出」。
- 变长（`is_varlen`）且非变长字符串的键，其长度还要被 `max_sort_length_even` 夹住，其中 `max_sort_length_even = (thd->variables.max_sort_length + 1) & ~1`。源码注释解释了为什么要取偶数：`strnxfrm()` 需要偶数字节。
- 整数类 `INT_RESULT` 的键长度固定为 8 字节（`SIZEOF_LONG_LONG > 4` 时）或 4 字节。`ENUM`/`SET` 字段被显式改写成 `INT_RESULT`（"Sort enum and set fields as their underlying ints"），时间类型也被改写为 `INT_RESULT`。
- `DECIMAL_RESULT` 用 `my_decimal_get_binary_size()`，`REAL_RESULT` 用 `sizeof(double)`。
- 可空列额外占 1 字节做 NULL 标记（`AddWithSaturate(1u, &total_length)`）；变长键额外加 `VARLEN_PREFIX` 前缀长度。

**排序键之后要不要附带「附加列（addon fields）」，决定的是排序记录里放什么。** `Sort_param::decide_addon_fields()` 的判定顺序是：

1. `DBUG_EXECUTE_IF("filesort_force_sort_row_ids", ...)` 调试开关直接置为 `keep_rowid`。
2. 任一表 `pos_in_table_list->is_fulltext_searched()` 为真 → 置为 `fulltext_searched`（全文检索需要行定位符）。
3. `force_sort_rowids` 为真 → 置为 `keep_rowid`。
4. 否则调用 `Filesort::get_addon_fields()`，由它决定是否附带整行。

`Filesort::get_addon_fields()` 的判定细节：只统计 `bitmap_is_set(&table->read_set_internal, field->field_index())` 为真的字段（读集合里的列），累加 `field->max_packed_col_length()` 得到 `total_length`，其中 NULL 位图占 `(null_fields + 7) / 8` 字节；`packable_length` 只累加可空列或 `MYSQL_TYPE_STRING` / `MYSQL_TYPE_VARCHAR` / `MYSQL_TYPE_VAR_STRING` / 带 `BLOB_FLAG` 的列。判定「必须按行定位符排序」的函数是 `SortWillBeOnRowId()`：全文检索表直接返回 true；否则遍历读集合里的字段，**若字段类型是 `MYSQL_TYPE_BLOB` 且 `field->max_packed_col_length() > 70000u` 则返回 true**——源码注释说明这把 mediumblob/longblob 排除在附加列之外，因为把大 BLOB 拉进内存排序代价过高。

**去重（去重复行）是排序的一个可选项**，由 `m_remove_duplicates` 控制。`check_if_pq_applicable()` 里有一条明确的拒绝理由："duplicate removal not supported yet"。

## 怎么实现的

### 入口与整体流程

`filesort()` 由 `sql/iterators/sorting_iterator.cc` 的 `SortingIterator::DoSort()` 调用，是排序执行路径上的唯一入口。它的流程分五段。

**第一段：预算与前置检查。**

```
ulong memory_available = thd->variables.sortbuff_size;
...
if (!(s_length = filesort->sort_order_length())) return true;
```

排序预算就是会话变量 `sortbuff_size`，没有任何二次折扣。随后：

```
if (!param->using_addon_fields()) {
  for (TABLE *table : filesort->tables) {
    if (table->pos_in_table_list == nullptr ||
        (tables_to_get_rowid_for & table->pos_in_table_list->map())) {
      table->prepare_for_position();
    }
  }
}
```

不附带附加列（即按行定位符排序）时，必须先让表进入可定位状态。

**第二段：初始化排序参数。**

```
param->init_for_filesort(filesort, make_array(filesort->sortorder, s_length),
                         sortlength(thd, filesort->sortorder, s_length),
                         filesort->tables, max_rows,
                         filesort->m_remove_duplicates);
```

`Sort_param::init_for_filesort()` 内部先累加 `sum_ref_length`（每张可空表多 1 字节，每张表再加 `table->file->ref_length`），再调 `decide_addon_fields()` 定下附加列策略，然后按策略设置 `fixed_res_length`：带附加列时是 `m_addon_length`，不带时是 `sum_ref_length`，且此时要把 `sum_ref_length` 加进 `m_fixed_sort_length`（源码注释："The reference to the record is considered as an additional sorted field"）。之后追加变长键长度 `size_of_varlength_field`、若含 JSON 键则 `use_hash = true` 并追加 `sizeof(ulonglong)` 的哈希尾。

`Filesort::make_sortorder()` 在 `Filesort` 构造函数里被调用（`m_sort_order_length(make_sortorder(order, unwrap_rollup))`），负责把优化器产出的 `ORDER` 链表翻译成 `st_sort_field` 数组：

```
count = 0;
for (ord = order; ord; ord = ord->next) count++;
assert(count > 0);

const size_t sortorder_size = sizeof(*sortorder) * (count + 1);
if (sortorder == nullptr)
  sortorder = static_cast<st_sort_field *>((*THR_MALLOC)->Alloc(sortorder_size));
if (sortorder == nullptr) return 0; /* purecov: inspected */
memset(sortorder, 0, sortorder_size);
```

分配量是 `count + 1` 个元素（多出的一个作为哨兵），失败时返回 0——而 `filesort()` 里的 `if (!(s_length = filesort->sort_order_length())) return true;` 正是把这个 0 当作错误信号。逐项填充时：

```
Item *const item = ord->item[0], *const real_item = item->real_item();
pos->item = real_item;
if (unwrap_rollup) {
  pos->item = unwrap_rollup_group(pos->item);
}
pos->reverse = (ord->direction == ORDER_DESC);
```

排序方向直接编码成 `pos->reverse` 布尔位；`unwrap_rollup` 为真时把 `GROUP BY` 的 rollup 包装层剥掉，源码注释给出的理由是："If filesort runs before GROUP BY ... we cannot have any rollup NULLs (and they don't have any rollup state to query), so we need to remove the wrappers."

**第三段：PQ 决策与内存预算分配。**

```
if (check_if_pq_applicable(trace, param, fs_info, num_rows_estimate,
                           memory_available)) {
  ...
  filesort->using_pq = true;
  param->using_pq = true;
  param->m_addon_fields_status = Addon_fields_status::using_priority_queue;
} else {
  filesort->using_pq = false;
  param->using_pq = false;
  param->try_to_pack_addons();

  if (num_rows_estimate < MERGEBUFF2) num_rows_estimate = MERGEBUFF2;
  ha_rows keys =
      memory_available / (param->max_record_length() + sizeof(char *));
  param->max_rows_per_buffer =
      min(num_rows_estimate > 0 ? num_rows_estimate : 1, keys);

  fs_info->set_max_size(memory_available, param->max_record_length());
}
```

非 PQ 路径有两个值得注意的点。其一，`try_to_pack_addons()` 只在非 PQ 路径调用，源码注释写明原因："When sorting using priority queue, we cannot use packed addons."。其二，`num_rows_per_buffer` 的估算把每个排序键的指针也算进预算——除数是 `param->max_record_length() + sizeof(char *)`，即每行除了记录本身还要一个指针。

`check_if_pq_applicable()` 的完整拒绝与接受条件：

- `param->max_rows == HA_POS_ERROR`（没有 LIMIT）→ 拒绝，原因串 "not applicable (no LIMIT)"。
- `param->m_remove_duplicates` 为真 → 拒绝，原因串 "duplicate removal not supported yet"。
- `param->max_rows + 2 >= UINT_MAX` → 拒绝，原因串 "limit too large"。
- `param->max_record_length() >= 0xFFFFFFFFu` → 拒绝，原因串 "contains records of unbounded length"。
- `num_available_keys = memory_available / (param->max_record_length() + sizeof(char *))`，`param->max_rows_per_buffer = (uint)param->max_rows + 1`（源码注释："We need 1 extra record in the buffer, when using PQ."）。
- 若 `num_rows < num_available_keys`（源集整体装得下内存），再比较 `param->max_rows < num_rows / PQ_slowness`，其中 `const double PQ_slowness = 3.0;`，源码注释说明这个系数来自单元测试的实测——"PQ is roughly 3 times slower"。满足则采用 PQ，否则拒绝并记录原因串 "sort_is_cheaper"。
- 若源集装不下内存，则看 `param->max_rows_per_buffer < num_available_keys`，即「LIMIT 行数装得下」就采用 PQ。

PQ 路径上，分配失败是硬错误：

```
if (fs_info->preallocate_records(param->max_rows_per_buffer)) {
  my_error(ER_OUT_OF_SORTMEMORY, ME_FATALERROR);
  LogErr(ERROR_LEVEL, ER_SERVER_OUT_OF_SORTMEMORY);
  goto err;
}
```

**第四段：读行与溢写。**

`read_all_rows()` 是排序的执行主体，循环 `source_iterator->Read()` 逐行取数：

```
size_t key_length;
bool out_of_mem_or_error = alloc_and_make_sortkey(
    param, fs_info, tables, &key_length, &longest_addon_so_far);
if (out_of_mem_or_error) {
  if (thd->is_error()) {
    return HA_POS_ERROR;
  }
  // Out of room, so flush chunk to disk (if there's anything to flush).
  if (num_records_this_chunk > 0) {
    if (write_keys(param, fs_info, num_records_this_chunk, chunk_file,
                   tempfile)) {
      return HA_POS_ERROR;
    }
    num_records_this_chunk = 0;
    num_written_chunks++;
    fs_info->reset();
    ...
  }
  // If we're still out of memory after flushing to disk, give up.
  if (out_of_mem_or_error) {
    ...
    my_error(ER_OUT_OF_SORTMEMORY, ME_FATALERROR);
    LogErr(ERROR_LEVEL, ER_SERVER_OUT_OF_SORTMEMORY);
    return HA_POS_ERROR;
  }
}
```

**溢写是「先落盘、再试一次」的顺序**：内存放不下这一行时，把当前 chunk 排序后写到 `tempfile`、把 chunk 描述符写到 `chunk_file`，`fs_info->reset()` 释放内存，然后重试这一行；重试仍失败才报 `ER_OUT_OF_SORTMEMORY`。不按行定位符排序时，还要在取行后立刻记录行位置：

```
if (!param->using_addon_fields()) {
  for (TABLE *table : tables) {
    if (!can_call_position(table)) continue;
    if (table->pos_in_table_list == nullptr ||
        (tables_to_get_row_id_for & table->pos_in_table_list->map())) {
      table->file->position(table->record[0]);
    }
  }
}
```

`alloc_and_make_sortkey()` 里的重试逻辑值得单独看：它先按 `min_bytes`（初始为 1）向缓冲区要一段空间，若 `param->make_sortkey()` 产出的 `rec_sz` 超过拿到的空间，就把 `min_bytes` 抬到 `sort_key_buf.size() + 1` 再要一次——这样「行比预估的长」不会导致写越界。

`write_keys()` 负责落盘：

```
count = fs_info->sort_buffer(param, count, param->max_rows);

if (!my_b_inited(chunk_file) &&
    open_cached_file(chunk_file, mysql_tmpdir, TEMP_PREFIX, DISK_BUFFER_SIZE,
                     MYF(MY_WME)))
  return 1;
if (!my_b_inited(tempfile) &&
    open_cached_file(tempfile, mysql_tmpdir, TEMP_PREFIX, DISK_BUFFER_SIZE,
                     MYF(MY_WME)))
  return 1;
```

临时文件建在 `mysql_tmpdir` 下，前缀是 `TEMP_PREFIX`，并有一条硬约束：

```
// Check that we won't have more chunks than we can possibly keep in memory.
if (my_b_tell(chunk_file) + sizeof(Merge_chunk) > (ulonglong)UINT_MAX)
  return 1;
```

**第五段：归并与收尾。**

```
size_t num_chunks, num_initial_chunks;
if (my_b_inited(&chunk_file)) {
  num_chunks = static_cast<size_t>(my_b_tell(&chunk_file)) / sizeof(Merge_chunk);
} else {
  num_chunks = 0;
}
num_initial_chunks = num_chunks;

if (num_chunks == 0)  // The whole set is in memory
{
  ha_rows rows_in_chunk = param->using_pq ? pq.num_elements() : num_rows_found;
  if (save_index(param, rows_in_chunk, fs_info, sort_result)) goto err;
} else {
  ...
  fs_info->read_chunk_descriptors(&chunk_file, num_chunks);
  ...
  param->max_rows_per_buffer = static_cast<uint>(
      fs_info->max_size_in_bytes() / param->max_record_length());

  Bounds_checked_array<uchar> merge_buf = fs_info->get_contiguous_buffer();
  if (merge_buf.array() == nullptr) {
    my_error(ER_OUT_OF_SORTMEMORY, ME_FATALERROR);
    LogErr(ERROR_LEVEL, ER_SERVER_OUT_OF_SORTMEMORY);
    goto err;
  }
  if (merge_many_buff(thd, param, merge_buf, fs_info->merge_chunks,
                      &num_chunks, &tempfile))
    goto err;
  ...
  if (merge_index(thd, param, merge_buf,
                  Merge_chunk_array(fs_info->merge_chunks.begin(), num_chunks),
                  &tempfile, outfile))
    goto err;
  sort_result->found_records = num_rows_found;
}
```

`num_chunks == 0` 是「全在内存」的判定：没写过 chunk 描述符就说明从未溢写。`merge_many_buff()` 是多轮归并（其实现位于 `sql/merge_many_buff.cc`，不在本页 refs 的检出文件内），末轮由本文件的 `merge_index()` 完成：

```
static int merge_index(THD *thd, Sort_param *param, Sort_buffer sort_buffer,
                       Merge_chunk_array chunk_array, IO_CACHE *tempfile,
                       IO_CACHE *outfile) {
  if (merge_buffers(thd, param, tempfile, outfile, sort_buffer,
                    chunk_array.begin(), chunk_array, false))  // include_keys
    return 1;
  return 0;
}
```

`include_keys` 传 `false`，含义是末轮归并不再输出排序键，只输出附加列或行定位符（`merge_buffers()` 的注释写明："If false, the keys will be skipped (useful only for the output of the final merge, where we don't need to compare rows further)"）。

### 内存缓冲区：`Filesort_buffer::allocate_block()`

这是 `sql/filesort_utils.cc` 里的核心分配函数。`Filesort_buffer` 用**多个 block** 承载排序记录，每个 block 是一段 `my_malloc` 出来的连续内存，记录指针存在 `m_record_pointers` 里。

```
bool Filesort_buffer::allocate_block(size_t num_bytes) {
  size_t next_block_size;
  if (m_current_block_size == 0) {
    // First block.
    next_block_size = MIN_SORT_MEMORY;
  } else {
    next_block_size = m_current_block_size + m_current_block_size / 2;
  }
  ...
```

**block 尺寸的增长策略是「首块常量、之后每次放大一半」**：第一块取 `MIN_SORT_MEMORY`（该常量定义在本次稀疏检出未包含的头文件里，故此处不引用其数值），后续每块的尺寸是 `m_current_block_size + m_current_block_size / 2`。放大一半而非翻倍，是为了避免为了一条很长的行把内存占用顶到两倍。

```
  if (!m_blocks.empty() && m_blocks.back().get() == m_next_rec_ptr) {
    m_current_block_size = 0;
    m_next_rec_ptr = nullptr;
    m_current_block_end = nullptr;
    m_blocks.pop_back();
  }
```

若最后一个 block 一条记录都没用过（`m_blocks.back().get() == m_next_rec_ptr`），先把它释放掉再申请更大的——源码注释说这是安全的，因为没写过东西。注释也说明了释放动作放在 `next_block_size` 计算之后，因为计算要用到 `m_current_block_size`。

**剩余空间的计算是这一函数最精细的部分。** 它把三部分开销都算进预算：

```
  size_t space_used = m_current_block_size + m_space_used_other_blocks;
  space_used += m_record_pointers.capacity() * sizeof(m_record_pointers[0]);

  size_t space_left;
  if (space_used > m_max_size_in_bytes)
    space_left = 0;
  else
    space_left = m_max_size_in_bytes - space_used;
```

`m_max_size_in_bytes` 就是排序缓冲区的总上限（由 `fs_info->set_max_size(memory_available, param->max_record_length())` 设定），`space_used` 含当前 block、已释放的旧 block 的累计占用，以及记录指针数组的容量开销。

然后要为「填充这个新 block 时必然伴随的指针增长」预留空间：

```
  size_t min_num_rows_capacity =
      m_record_pointers.size() +
      space_left /
          AddWithSaturate(m_max_record_length, sizeof(m_record_pointers[0]));
  if (min_num_rows_capacity > m_record_pointers.capacity()) {
    space_left -= (min_num_rows_capacity - m_record_pointers.capacity()) *
                  sizeof(m_record_pointers[0]);
  }
```

源码注释坦承这个估算偏乐观："we take a best-case estimate based on maximum-size records. It's also impossible to say how capacity() will change since this is an implementation detail, so we don't take that into account. This means that, for smaller records, we could go above the maximum permitted total memory usage."

最终尺寸与「装不下」判定：

```
  next_block_size = min(max(next_block_size, num_bytes), space_left);
  if (next_block_size < num_bytes) {
    size_t excess_bytes =
        (m_record_pointers.capacity() - m_record_pointers.size()) *
        sizeof(m_record_pointers[0]);
    if (excess_bytes >= 32768) {
      size_t old_capacity = m_record_pointers.capacity();
      m_record_pointers.shrink_to_fit();
      if (m_record_pointers.capacity() < old_capacity) {
        return allocate_block(num_bytes);
      }
    }
    // We're full.
    return true;
  }

  return allocate_sized_block(next_block_size);
```

**返回 `true` 表示「缓冲区满了」**——注意这是**逆语义**，容易看反。装不下的处理是：先算指针数组里「已分配未使用」的空余量 `excess_bytes`，**若不小于 32768 字节（源码注释写作 "at least 32 kB unused in m_record_pointers"）**，就对指针数组 `shrink_to_fit()`，若容量确实下降了就递归调用 `allocate_block(num_bytes)` 再试一次；源码注释说明这条路径"几乎不会发生"，只在「先排了很多很短的行（堆出大量记录指针）然后要排很大的行」时才会触发。仍然装不下就返回 `true`。

`allocate_sized_block()` 是真正的裸分配：

```
bool Filesort_buffer::allocate_sized_block(size_t block_size) {
  unique_ptr_my_free<uchar[]> new_block((uchar *)my_malloc(
      key_memory_Filesort_buffer_sort_keys, block_size, MYF(0)));
  if (new_block == nullptr) {
    return true;
  }
  m_space_used_other_blocks += m_current_block_size;
  m_current_block_size = block_size;
  m_next_rec_ptr = new_block.get();
  m_current_block_end = new_block.get() + m_current_block_size;
  m_blocks.push_back(std::move(new_block));
  return false;
}
```

注意 `m_space_used_other_blocks += m_current_block_size;` 这一行：旧 block 的尺寸被累加进「其他 block 已用」，新 block 成为当前 block。这就是 `allocate_block()` 里 `space_used` 的两部分来源。

`Filesort_buffer::reset()` 在每次溢写后调用，负责回收内存但保留最后一块：

```
void Filesort_buffer::reset() {
  update_peak_memory_used();
  m_record_pointers.clear();
  if (m_blocks.size() >= 2) {
    // Free every block but the last.
    m_blocks.erase(m_blocks.begin(), m_blocks.end() - 1);
  }
  if (m_max_record_length > m_current_block_size) {
    free_sort_buffer();
  }
  ...
  m_space_used_other_blocks = 0;
}
```

`m_max_record_length > m_current_block_size` 这一条意味着：**如果新的最大行长已经超过当前 block 尺寸，连最后一块也一起释放**——因为按增长策略分配的下一块要按新行长重新起步，而旧块已经装不下任何新行了。

### 排序算法选择：`Filesort_buffer::sort_buffer()`

`sort_buffer()` 决定用哪种 STL 排序算法：

```
if (max_output_rows == 0) return max_output_rows;
if (num_input_rows <= 1) return num_input_rows;
if (param->max_compare_length() == 0) return num_input_rows;
...
const bool prefilter_nth_element =
    max_output_rows < num_input_rows / 2 && !param->m_remove_duplicates;
```

**`prefilter_nth_element` 是 LIMIT 场景的关键优化**：当 `max_output_rows` 小于输入行数的一半时，先用 `nth_element()` 把前 `max_output_rows` 个元素定位好，再只对这 `k` 个元素做完整排序。源码注释说这把复杂度从 O(n log n) 降到 O(n + k log k)，但因为 `nth_element()` **不是稳定排序**，所以只有在不需要去重时才能用（去重要求相同排序键的行保持原有相对顺序）。

算法分派：

```
  if (num_input_rows <= 100) {
    ... std::sort ...
  }
  param->m_sort_algorithm = Sort_param::FILESORT_ALG_STD_STABLE;
  ... std::stable_sort ...
```

**行数不超过 100 时用 `std::sort`，超过则用 `std::stable_sort`**。源码注释解释了原因："std::stable_sort has some extra overhead in allocating the temp buffer ... The cutover point where it starts to get faster than quicksort seems to be somewhere around 10 to 40 records. So we're a bit conservative, and stay with quicksort up to 100 records."

比较器也分两档：`key_len < 10` 用 `Mem_compare`（逐字节比较），否则用 `Mem_compare_longkey`（前 4 字节按大端整数比较，剩余部分 `memcmp`）。`key_len` 在不附带附加列时还要减去 `param->sum_ref_length`，因为行定位符被追加在记录尾部会干扰相等性判断（源码注释："This disturbs our equality comparisons, so we'll have to remove it."）。

变长键走独立分支，比较器是 `Mem_compare_varlen_key`，内部调用 `cmp_varlen_keys(param->local_sortorder, param->use_hash, s1, s2)`，且**变长键一律用 `std::sort`**（源码里的 TODO 写着 "Make more elaborate heuristics than just always picking std::sort"）。

### 结果读取：四种迭代器的选择

排序完成后，`sql/iterators/sorting_iterator.cc` 的 `SortingIterator::Init()` 按「结果在内存还是在磁盘」×「带附加列还是按行定位符」四个组合选迭代器：

- 磁盘 + 附加列 + 打包附加列 → `SortFileIterator<true>`
- 磁盘 + 附加列 + 未打包 → `SortFileIterator<false>`
- 磁盘 + 行定位符 → `SortFileIndirectIterator`
- 内存 + 附加列 + 打包 → `SortBufferIterator<true>`
- 内存 + 附加列 + 未打包 → `SortBufferIterator<false>`
- 内存 + 行定位符 → `SortBufferIndirectIterator`

`SortingIterator::CleanupAfterQuery()` 负责收尾：`m_fs_info.free_sort_buffer()`、`my_free(m_fs_info.merge_chunks.array())`、把 `merge_chunks` 置空、`m_fs_info.addon_fields = nullptr`。

### 「附加列策略」的推翻重来：`Filesort::clear_addon_fields()`

`Filesort::clear_addon_fields()` 是 `Filesort::clear_addon_fields()` 的公开入口，实现只有一行：

```
void Filesort::clear_addon_fields() { m_sort_param.clear_addon_fields(); }
```

它落到 `Sort_param::clear_addon_fields()`：

```
void Sort_param::clear_addon_fields() {
  m_addon_fields_status = Addon_fields_status::unknown_status;
  addon_fields = nullptr;
}
```

**语义是把「已经定好的附加列决策」作废**，让下一次 `decide_addon_fields()` 重新判一次（`decide_addon_fields()` 的第一行就是 `if (m_addon_fields_status != Addon_fields_status::unknown_status) { return; }`，即已决策则不重算）。

**真实的调用方是 `sql/sql_executor.cc` 的 `NewWeedoutAccessPathForTables()`**：

```
Filesort *filesort = qep_tabs[i].filesort;
if (filesort != nullptr) {
  if (filesort->m_sort_param.m_addon_fields_status !=
      Addon_fields_status::unknown_status) {
    // This can happen in the exceptional case that there's an extra
    // weedout added after-the-fact due to nonhierarchical weedouts
    // (see FindSubstructure for details). Note that our caller will
    // call FindTablesToGetRowidFor() if needed, which should overwrite
    // the previous (now wrong) decision there.
    filesort->clear_addon_fields();
  }
  filesort->m_force_sort_rowids = true;
  ...
}
```

**回退路径点名的三个函数**：`clear_addon_fields()` 的调用方是 `NewWeedoutAccessPathForTables()`；改判后的重决策入口是 `Sort_param::decide_addon_fields()`；强制按行定位符排序的开关是 `Filesort::m_force_sort_rowids`（由 `decide_addon_fields()` 的 `if (force_sort_rowids)` 分支消费，置 `Addon_fields_status::keep_rowid`）。这三者构成一条完整的「推翻已决策 → 强制改判 → 下次重新决策」的链路。

### 错误分支清单

`sql/filesort.cc` 中与排序内存相关的硬错误共 5 处 `my_error(ER_OUT_OF_SORTMEMORY, ME_FATALERROR)` + `LogErr(ERROR_LEVEL, ER_SERVER_OUT_OF_SORTMEMORY)`：

1. `filesort()` 的 PQ 路径：`fs_info->preallocate_records()` 失败。
2. `filesort()` 的归并路径：`fs_info->get_contiguous_buffer()` 返回空数组。
3. `read_all_rows()`：溢写后重试仍放不下该行。
4. `read_to_buffer()`：归并时读不出第一行完整记录（`count == 0`，源码注释 "Not even room for the first row."）。
5. `read_to_buffer()` 的变长键分支：`ix == 0`，即连一条完整记录都读不出来。

另有一类**被吞掉的警告**：`Filesort_error_handler` 的 `handle_condition()` 把 `ER_NOT_SUPPORTED_YET` 警告限制成每次排序只报一次——

```
if (*level == Sql_condition::SL_WARNING && sql_errno == ER_NOT_SUPPORTED_YET) {
  if (m_seen_not_supported) return true;
  m_seen_not_supported = true;
}
```

源码注释说明这个警告来自 `Json_wrapper::make_sort_key()`，即 JSON 值无法生成排序键时逐个报错，这里做了去重。

## 关键取舍

**取舍一：内存排序 vs 磁盘归并 vs 优先级队列，三选一且在运行时才定。**

`check_if_pq_applicable()` 之所以要等到运行期才做决定，源码注释给的理由是："We would have liked to do this ahead-of-time so that we can show it in EXPLAIN. However, we don't know the table->file->ref_length before sorting time, which makes it hard to make the decision if we're row IDs." 也就是说 `max_record_length()` 依赖 `table->file->ref_length`，而后者要等迭代器初始化后才知道。代价是 EXPLAIN 里看不到这个决策，只能在优化器 trace 的 `filesort_priority_queue_optimization` 节点里看到 `chosen` 与 `cause`。

**取舍二：PQ 的收益上限被一个硬编码系数限制。**

`const double PQ_slowness = 3.0;` 把 PQ 的比较开销按「是 qsort 的 3 倍」计。判定式 `param->max_rows < num_rows / PQ_slowness` 意味着：**只有当 LIMIT 行数小于源集行数的三分之一时，PQ 才被认为划算**。这个系数来自单元测试的实测（源码注释："Measurements (see unit test) indicate that PQ is roughly 3 times slower"），写死在代码里而非可配置。

**取舍三：附加列 vs 行定位符，用「行有多长」来分界。**

`SortWillBeOnRowId()` 的分界线是 `field->max_packed_col_length() > 70000u` 且类型为 `MYSQL_TYPE_BLOB`。源码注释解释了这个取舍的哲学：tinyblob（255 字节）与 blob（65535 字节）是「显式有界的」，JSON 与几何类型是「通常比较小」的，两者都放进附加列没问题；只有 mediumblob 和 longblob 是「用户声明了就期待排序不需要把它们全拉进内存」的，所以必须退回行定位符。

**取舍四：排序键长度的封顶，用两个不同的常量管两件事。**

**非变长的**字符串键（源码条件是 `!sortorder->is_varlen && is_string_type`）的长度被 `max_sort_length_even`（源自会话变量 `max_sort_length`）夹住，理由在源码注释里写得很清楚：PAD SPACE 排序规则的 BLOB 字段不是变长的（填充是排序键的一部分序列化进去的），因此需要无限空间，所以 `max_sort_length` 这条约束对它们仍然必须生效。而 `strnxfrmlen` 的调用另有 `10 << 20` 这道门槛，超过就把长度设成 `0xFFFFFFFFu`——源码注释说是 "so we don't get overflows in strnxfrmlen()"。**一个常量管「排序键实际要占多少内存」，另一个管「计算排序键长度时会不会溢出」。**

**取舍五：block 尺寸放大一半而非翻倍。**

`next_block_size = m_current_block_size + m_current_block_size / 2;` 对应的是「一条很长的行突然出现」的场景。翻倍会让峰值内存直接顶到两倍；放大一半则需要多几次重试，但内存曲线更平。

**取舍六：溢写优先于报错。**

`read_all_rows()` 的处理顺序是「先写盘、再重试、最后才报错」。这意味着排序缓冲区的硬上限是 `sortbuff_size` 而非「单条记录的上限」——再大的行也能排，只是要先溢写一段。

**取舍七：PQ 与去重、附加列打包三者互斥。**

`check_if_pq_applicable()` 拒绝去重（"duplicate removal not supported yet"），`filesort()` 只在非 PQ 路径调 `try_to_pack_addons()`（"When sorting using priority queue, we cannot use packed addons."），而 `try_to_pack_addons()` 自己还有一层启发式门槛——`m_packable_length < (10 + sz)` 时直接放弃打包并置 `Addon_fields_status::skip_heuristic`，源码注释写明理由："Heuristic: skip packing if potential savings are less than 10 bytes."

**取舍八：附加列决策可以推翻，但推翻要显式。**

`Filesort::clear_addon_fields()` 存在的唯一理由是处理「非层次 weedout 后补加 weedout」这种例外情况——`NewWeedoutAccessPathForTables()` 的注释明确说："This can happen in the exceptional case that there's an extra weedout added after-the-fact due to nonhierarchical weedouts". 作废决策之后必须紧跟一次 `m_force_sort_rowids = true`，否则重决策会得出同样的旧结论。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/filesort.cc` | `Filesort::make_sortorder` | 把优化器的 `ORDER` 链表翻译成 `st_sort_field` 数组：分配 `count + 1` 个元素、`memset` 清零、写入 `real_item()`、按 `unwrap_rollup` 剥掉 rollup 包装、用 `ord->direction == ORDER_DESC` 编码 `reverse` 位；失败返回 0 |
| `sql/filesort.cc` | `Filesort::clear_addon_fields` | 作废已定的附加列策略（`m_addon_fields_status` 置回 `unknown_status`、`addon_fields` 置空），使下次决策重算；调用方是 `sql/sql_executor.cc` 的 `NewWeedoutAccessPathForTables()` |
| `sql/filesort.cc` | `Sort_param::clear_addon_fields` | `Filesort::clear_addon_fields()` 的实际实现，把决策状态复位 |
| `sql/filesort.cc` | `Sort_param::decide_addon_fields` | 附加列策略的决策点：调试开关 → 全文检索表 → `force_sort_rowids` → 交给 `Filesort::get_addon_fields()` |
| `sql/filesort.cc` | `Sort_param::init_for_filesort` | 初始化排序参数：累加 `sum_ref_length`、定 `fixed_res_length`、追加变长键长度、JSON 键置 `use_hash` 并追加 `sizeof(ulonglong)` 哈希尾 |
| `sql/filesort.cc` | `Filesort::get_addon_fields` | 统计读集合内字段的 `max_packed_col_length()` 累计长度与可打包长度；NULL 位图占 `(null_fields + 7) / 8` 字节；`SortWillBeOnRowId()` 为真时以 `row_contains_blob` 状态返回空指针 |
| `sql/filesort.cc` | `SortWillBeOnRowId` | 判定是否必须按行定位符排序：全文检索表直接为真；`MYSQL_TYPE_BLOB` 且 `max_packed_col_length() > 70000u` 的字段触发为真 |
| `sql/filesort.cc` | `sortlength` | 计算排序键总长度：字符串走 `strnxfrmlen`（长度 ≥ `10 << 20` 时置 `0xFFFFFFFFu`）、整数 8 字节、十进制 `my_decimal_get_binary_size`、浮点 `sizeof(double)`；非变长字符串再被 `max_sort_length_even` 夹住；可空列加 1 字节 NULL 标记 |
| `sql/filesort.cc` | `filesort` | 排序主入口：取 `thd->variables.sortbuff_size` 作预算 → 决定 PQ 或普通排序 → `read_all_rows()` → 全内存走 `save_index()`、否则走 `merge_many_buff()` + `merge_index()`；收尾按 `keep_buffers` 决定是否释放缓冲 |
| `sql/filesort.cc` | `check_if_pq_applicable` | PQ 适用性判定：无 LIMIT / 要去重 / `max_rows + 2 >= UINT_MAX` / `max_record_length() >= 0xFFFFFFFFu` 均拒绝；源集装得下内存时要求 `max_rows < num_rows / PQ_slowness`（`PQ_slowness = 3.0`）；否则要求 `max_rows_per_buffer < num_available_keys` |
| `sql/filesort.cc` | `read_all_rows` | 逐行取数并生成排序键；内存放不下时先 `write_keys()` 溢写再重试，仍失败报 `ER_OUT_OF_SORTMEMORY` |
| `sql/filesort.cc` | `alloc_and_make_sortkey` | 单行的排序键生成与空间申请；键比申请到的空间长时把 `min_bytes` 抬到 `sort_key_buf.size() + 1` 重试 |
| `sql/filesort.cc` | `write_keys` | 把当前 chunk 排序后写入 `tempfile`、把 `Merge_chunk` 描述符写入 `chunk_file`；两个文件都在 `mysql_tmpdir` 下以 `TEMP_PREFIX` 建立；chunk 总数超过 `UINT_MAX` 字节时返回错误 |
| `sql/filesort.cc` | `save_index` | 全内存路径：调用 `sort_buffer()` 排序；带附加列时标记 `sorted_result_in_fsbuf` 并直接返回，否则把行定位符拷进 `sort_result->sorted_result` |
| `sql/filesort.cc` | `merge_buffers` | 归并实现：用 `Priority_queue` 维护 chunk 队列，按 `Merge_chunk_greater` 弹出最小 chunk 的行；`include_keys` 为假时只输出附加列/行定位符；每次进入递增 `thd->inc_status_sort_merge_passes()` |
| `sql/filesort.cc` | `merge_index` | 末轮归并入口，以 `include_keys = false` 调用 `merge_buffers()`，只写最终结果 |
| `sql/filesort.cc` | `read_to_buffer` | 从磁盘 chunk 读入归并缓冲区；变长键/打包附加列分支里逐条检查记录是否完整，完整记录数为 0 时报 `ER_OUT_OF_SORTMEMORY` |
| `sql/filesort.cc` | `Filesort_error_handler` | 排序期间的内部错误处理器，把 `ER_NOT_SUPPORTED_YET` 警告去重为每次排序只报一次 |
| `sql/filesort_utils.cc` | `Filesort_buffer::allocate_block` | 排序缓冲区的分块分配：首块取 `MIN_SORT_MEMORY`、之后每块放大一半；按 `m_max_size_in_bytes` 扣减已用空间并为记录指针增长预留；装不下时若指针数组空余 ≥ `32768` 字节则 `shrink_to_fit()` 后递归重试；返回 `true` 表示缓冲区已满 |
| `sql/filesort_utils.cc` | `Filesort_buffer::allocate_sized_block` | 按给定尺寸 `my_malloc` 一块内存（`key_memory_Filesort_buffer_sort_keys`），旧块尺寸累加进 `m_space_used_other_blocks`，新块成为当前块 |
| `sql/filesort_utils.cc` | `Filesort_buffer::sort_buffer` | 排序算法分派：行数 ≤ `100` 用 `std::sort`，否则用 `std::stable_sort`；`max_output_rows < num_input_rows / 2` 且不去重时先用 `nth_element()` 预筛；键长 < `10` 用 `Mem_compare`、否则用 `Mem_compare_longkey`；变长键走 `Mem_compare_varlen_key` 且固定用 `std::sort` |
| `sql/filesort_utils.cc` | `Filesort_buffer::reset` | 溢写后回收内存：清空记录指针、释放除最后一块外的所有块；`m_max_record_length > m_current_block_size` 时连最后一块一起释放 |
| `sql/filesort_utils.cc` | `Filesort_buffer::preallocate_records` | PQ 路径的预分配：`m_max_record_length == 0xFFFFFFFFu`（当作无穷大）时直接放弃；所需字节数加指针开销超过 `m_max_size_in_bytes` 时也放弃 |
| `sql/filesort_utils.cc` | `Filesort_buffer::get_contiguous_buffer` | 为归并准备一块连续的 `m_max_size_in_bytes` 内存，分配失败返回空数组 |
| `sql/iterators/sorting_iterator.cc` | `SortingIterator::DoSort` | 调用 `::filesort()` 执行排序，是排序机制的执行侧入口 |
| `sql/iterators/sorting_iterator.cc` | `SortingIterator::Init` | 按「磁盘/内存 × 附加列/行定位符 × 是否打包」六个组合选择 `SortFileIterator` / `SortFileIndirectIterator` / `SortBufferIterator` / `SortBufferIndirectIterator` |
| `sql/sql_executor.cc` | `NewWeedoutAccessPathForTables` | `Filesort::clear_addon_fields()` 的唯一调用方：后补 weedout 时作废已定的附加列策略并置 `m_force_sort_rowids = true` |

## 与主核 / AI 的关系

主核（`engine/db`）当前的排序能力远未展开到这一层——从本地检出的 `engine/db` 代码结构看，查询层只有扫描与过滤两类算子，**没有独立于执行算子的排序节点**。因此本节能给出的对照主要是「结构上缺什么」，而不是「已有什么不一样」。

**架构推论：** MySQL 这层最值得主核借鉴的是**「排序预算是一个会话变量，排序器只负责在这个预算内选择落地形态」**这条契约。`filesort()` 只从 `thd->variables.sortbuff_size` 取一个数，之后内存排序、磁盘归并、优先级队列三条路的分叉全部由这一个数加上运行时才知道的 `max_record_length()` 决定。主核若要加排序节点，把「排序预算」做成显式参数、并允许执行期按实际行长调整，比把排序策略写死在算子里更接近 MySQL 的做法。

**架构推论：** `SortWillBeOnRowId()` 用「单个字段的打包长度是否超过 `70000u`」作为「整行排序 vs 行定位符排序」的分界线，是一条**以数据宽度而非查询语义为准**的启发式。主核若在存储层有自己的行宽元数据，可以复用同一形态：把「排序时是否拉起整行」的决定权交给存储层的宽度信息，而不是由 SQL 层的算子硬判。

**架构推论：** `check_if_pq_applicable()` 把「是否用堆保留 top-N」写成一个纯数值判定（`max_rows < num_rows / 3.0`），并把系数 `PQ_slowness = 3.0` 作为可调的实测常数硬编码在函数里。主核若要做 top-N 排序，同样的「用一个实测常数把两种策略的成本比写进判定式」的做法是可以迁移的，且比让上层显式指定「用堆还是全排」更好用。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是**「同样的 ORDER BY 语义会以什么代价落地」**。**架构推论：** `filesort()` 的三个决策点——PQ 与否、附加列与否、是否打包附加列——每一个都会改变实际 I/O 量级：PQ 把内存占用从「结果集大小」降到「LIMIT 行数」，行定位符把「拉整行」降级为「拉行号」，磁盘归并把一次排序变成「N 次写盘 + N 轮归并读盘」。但这三个决策都不出现在 EXPLAIN 的常规输出里（PQ 决策因为依赖运行期才知道的 `ref_length` 而被源码注释明确排除在优化期之外）。这意味着 AI 若要回答「这条 ORDER BY 为什么慢」，仅凭计划树是不够的，得能读到优化器 trace 里的 `filesort_priority_queue_optimization` 节点（它带有 `chosen` 与 `cause` 字段）和 `filesort_summary` 节点（它带有 `sort_algorithm`、`sort_mode`、`num_initial_chunks_spilled_to_disk`、`peak_memory_used` 等字段）。主核若要让 AI 参与执行策略选择，应当把「排序器的三种落地形态与各自代价」做成可查询的结构化对象，而不是藏在执行期。

## 参考

- [MySQL 源码：`sql/filesort.cc`（上游 tag mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/sql/filesort.cc) —— 排序主流程：`filesort()`、`check_if_pq_applicable()`、`read_all_rows()`、`write_keys()`、`merge_buffers()`、`merge_index()`、`sortlength()`、`SortWillBeOnRowId()`、`Filesort::get_addon_fields()`、`Filesort::make_sortorder()`、`Filesort::clear_addon_fields()`。
- [MySQL 源码：`sql/filesort_utils.cc`（上游 tag mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/sql/filesort_utils.cc) —— 排序缓冲区的分配与排序算法：`Filesort_buffer::allocate_block()`、`allocate_sized_block()`、`reset()`、`preallocate_records()`、`sort_buffer()`、`get_contiguous_buffer()`。
- [MySQL 源码：`sql/filesort.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/filesort.h) —— `Filesort` 类声明：`sortorder`、`using_pq`、`m_remove_duplicates`、`m_force_sort_rowids`、`m_sort_param` 等成员与 `filesort()` 函数签名。
- [MySQL 源码：`sql/iterators/sorting_iterator.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/sorting_iterator.cc) —— `SortingIterator::DoSort()` 对 `filesort()` 的调用，以及 `SortingIterator::Init()` 对六种结果迭代器的选择。
- [MySQL 源码：`sql/sql_executor.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_executor.cc) —— `NewWeedoutAccessPathForTables()`，即 `Filesort::clear_addon_fields()` 的调用方，以及 `m_force_sort_rowids` 的置位点。
- [MySQL 参考手册：ORDER BY 优化](https://dev.mysql.com/doc/refman/8.0/en/order-by-optimization.html) —— 何时能用索引消除排序、filesort 的触发条件与代价说明。
- [MySQL 参考手册：服务器系统变量 `sort_buffer_size`](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_sort_buffer_size) —— 排序内存预算的会话变量定义与作用域。
- [MySQL 参考手册：服务器系统变量 `max_sort_length`](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_max_sort_length) —— 排序键长度上限，约束 `sortlength()` 中 `max_sort_length_even` 的取值。
- [MySQL 参考手册：InnoDB 存储引擎与 ORDER BY](https://dev.mysql.com/doc/refman/8.0/en/innodb-storage-engine.html) —— 存储引擎侧的索引有序性与 ORDER BY 的关系，即「何时不需要 filesort」的边界。
