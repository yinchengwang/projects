---
project: mysql-server
feature: filter-and-aggregate
page_type: feature
schema_version: 1
refs:
  - path: sql/iterators/composite_iterators.cc
    symbols: [FilterIterator, AggregateIterator]
  - path: sql/iterators/window_iterators.cc
    symbols: [WindowIterator]
---

# 过滤与聚合迭代

## 一句话职责

过滤与聚合迭代负责在行流上施加「留下哪些行」的判定（`FilterIterator`）、把同组行折叠成分组结果（`AggregateIterator`，含 ROLLUP 多级输出）、以及在窗口语义下计算窗口函数（`WindowIterator` / `BufferingWindowIterator`）；它不负责条件表达式本身的求值算法，也不负责最终结果的排序与截断。

## 机制是什么

**这一层提供的是三类「行流变形器」：它们消费上游的行流，产出自己的行流。**

- **过滤**：`FilterIterator` 在 `sql/iterators/composite_iterators.cc` 里实现，是一个把行流按布尔条件筛掉的单子节点组合迭代器。它持有的核心成员只有一个 `Item *m_condition`——条件被建模成一个 `Item`，求值入口是 `Item::val_int()`。
- **分组聚合**：`AggregateIterator` 把「同组行相邻」当作前提（组内行在输入流里必须连续），逐组折叠为一行输出。它持有 `JOIN *m_join` 来取聚合函数列表 `m_join->sum_funcs`、分组字段 `m_join->group_fields`、ROLLUP 分组项 `m_join->rollup_group_items` 与 `m_join->rollup_sums`。
- **窗口函数**：`sql/iterators/window_iterators.cc` 里的 `WindowIterator` 与 `BufferingWindowIterator` 是同一族里按「是否需要缓冲」分出的两个形态。`WindowIterator` 构造函数里有 `assert(!m_window->needs_buffering())`，`BufferingWindowIterator` 构造函数里有 `assert(m_window->needs_buffering())`——**这是两个类唯一的结构性差别之一，选哪个由 `Window::needs_buffering()` 决定**。

**关键边界：这三个迭代器都把「语义计算」外包给别处。** 过滤的布尔判定落在 `Item::val_int()` 上；聚合的数值累加落在 `Item_sum::reset_and_add()` / `Item_sum::aggregator_add()` 上（ROLLUP 场景还多一层 `Item_rollup_sum_switcher`）；窗口函数的求值落在 `copy_funcs()` 按 `CFT_*` 标志位的分派上。**本层只负责「什么时候读一行、什么时候切一组、什么时候输出一行」这个调度问题。**

## 怎么实现的

### `FilterIterator::Read`：过滤的全部逻辑

`FilterIterator::Read()` 的函数体是一个 `for (;;)` 无限循环，循环体内依次做四件事：

1. `int err = m_source->Read();`，`err != 0` 直接返回（`1` 是错误，`-1` 是 EOF）。
2. `bool matched = m_condition->val_int();` —— 条件求值，**用的是 `val_int()` 而不是 `val_bool()`**：真假语义由 `Item` 自身的取值规则承担。
3. **先查中断、再查错误**，顺序是源码写死的：
   - `if (thd()->killed) { thd()->send_kill_message(); return 1; }`
   - `if (thd()->is_error()) return 1;`
   两者都返回 `1`（错误），**中断检查排在错误检查之前**。
4. `if (!matched) { m_source->UnlockRow(); continue; }` —— **不匹配的行被解锁后丢弃**，继续读下一行；匹配则 `return 0`。

由此得到过滤的两个可断言性质：

- **过滤是「读—判—丢」的流式过程，不物化任何东西。** 被丢弃的行只调用一次 `m_source->UnlockRow()`，不写临时表、不进缓冲。
- **解锁发生在丢弃路径上。** 这正是 `RowIterator::UnlockRow()` 存在的原因：读到了但没用它，就要释放行锁。`AggregateIterator::UnlockRow()` 是空实现（注释解释：多半是 HAVING 失败，想回退解锁进入聚合的所有行做不到，也不能解锁当前行，因为当前行属于另一个组）——**同一个协议里，过滤能解锁而聚合不能**，差别正来自聚合的「当前行属于下一组」这一事实。

`FilterIterator::Init()` 是一行转发：`return m_source->Init();`。

### 谁创建 `FilterIterator`：两条优化路径

`FilterIterator` 不是被谁「调用」的，而是被**装配**出来的——`AccessPath::FILTER` 落到迭代器时实例化：

- **hypergraph 优化器**：`sql/join_optimizer/access_path.cc` 的 `CreateIteratorFromAccessPath` 中 `case AccessPath::FILTER:` 分支执行 `iterator = NewIterator<FilterIterator>(thd, mem_root, std::move(job.children[0]), param.condition);`。同文件的 `case AccessPath::AGGREGATE:` 分支执行 `NewIterator<AggregateIterator>(...)`，并把 `param.rollup` 传进去；`case AccessPath::WINDOW:` 分支按 `param.needs_buffering` 决定实例化 `BufferingWindowIterator` 还是 `WindowIterator`。
- **旧优化器**：`sql/sql_executor.cc` 的 `PossiblyAttachFilter()` 把一组条件用 `CreateConjunction()` 合成一个 `Item`，再 `NewFilterAccessPath(thd, path, condition)` 造出过滤访问路径；该函数在 `JOIN::create_access_paths()` 里被用于挂 WHERE 条件与连接条件。`PossiblyAttachFilter()` 里有一条短路规则：若某个 `const_item()` 条件的 `val_int()` 为 `0`，且路径不含 `ContainsAnyMRRPaths(path)`，则直接返回 `NewZeroRowsAccessPath(thd, path, "Impossible filter")`——**恒假条件不生成过滤迭代器，而是生成「零行」路径**。若 `val_int()` 非零则该条件被跳过。

### `AggregateIterator`：四态状态机

`AggregateIterator` 的状态是一个匿名枚举 `m_state`，四个取值：`READING_FIRST_ROW`、`LAST_ROW_STARTED_NEW_GROUP`、`OUTPUTTING_ROLLUP_ROWS`、`DONE_OUTPUTTING_ROWS`。`Read()` 是一个以 `m_state` 为 `switch` 分支的函数机。

**构造函数**做了一件容易被忽略的事：

```
const size_t upper_data_length = ComputeRowSizeUpperBound(m_tables);
m_first_row_this_group.reserve(upper_data_length);
m_first_row_next_group.reserve(upper_data_length);
```

两个 `String` 缓冲按「一行的打包后长度上界」预留容量。**这是「行可以保存和恢复」这一设计的关键**：注释写明 AggregateIterator 需要能保存和恢复行，因为它直到看到**下一组的第一行**才知道当前组结束；它必须把那一行先存起来，然后把上一组的第一行恢复回去，输出行才会带上正确的分组值。

**「存一行 / 恢复一行」的实现走的是 `pack_rows` 基础设施**：`StoreFromTableBuffers(m_tables, &m_first_row_next_group)` 与 `LoadIntoTableBuffers(m_tables, ...)` 成对出现，分别出现在 `READING_FIRST_ROW`、EOF 处理、以及组切换三个分支里。

**`Init()` 的前置条件与复位动作**：

- `assert(!m_join->tmp_table_param.precomputed_group_by);` —— 分组已经被预计算（`precomputed_group_by`）的场合不走这个迭代器。
- `m_current_rollup_position = -1; SetRollupLevel(INT_MAX);` —— 复位残留的 ROLLUP 状态；`INT_MAX` 表示「比任何上层迭代器都高的层级」，`DONE_OUTPUTTING_ROWS` 分支再次用 `SetRollupLevel(INT_MAX)` 注释说明「Higher-level iterators up above should not activate any rollup」。
- 若 `m_first_row_next_group` 非空（迭代器之前执行过），先 `LoadIntoTableBuffers()` 恢复该行再清空它。注释给出理由：**下面若有 `EQRefIterator`，`Read()` 里恢复上一组的动作可能扰动了它的缓存**，所以必须复原。
- `m_output_slice` 的设定带条件：`if (!(m_join->implicit_grouping || m_join->group_optimized_away) && !thd()->lex->using_hypergraph_optimizer())` 才取 `m_join->get_ref_item_slice()`。注释坦承这是「a hack」：切片系统在 hypergraph 优化器里根本不用。

**`Read()` 的各分支要点**：

- **`READING_FIRST_ROW`**：读第一行。若读到 EOF：
  - 若 `m_join->grouped || m_join->group_optimized_away`，则 `SetRollupLevel(m_join->send_group_parts)` 后返回 `-1`。
  - 否则（**没有 GROUP BY**），必须输出一行：对 `*m_join->get_current_fields()` 里非 hidden 的项（以及 `Item::SUM_FUNC_ITEM` 类型中 `aggr_query_block == m_join->query_block` 的项）调 `no_rows_in_result()`；再 `m_join->clear_fields(&m_save_nullinfo)` 把表标记为全 NULL；对每个 `m_join->sum_funcs` 调 `clear()`；最后 `return 0`。**这是「无 GROUP BY 且输入零行仍要输出一行」的实现位置。**
  - 读到了第一行则 `(void)update_item_cache_if_changed(m_join->group_fields);` 建立分组字段的初始缓存，并 `StoreFromTableBuffers(m_tables, &m_first_row_next_group)` 存下这一行。
- **`LAST_ROW_STARTED_NEW_GROUP`**：`swap(m_first_row_this_group, m_first_row_next_group)`（用交换而非移动，因为想复用缓冲），再 `LoadIntoTableBuffers()` 恢复本组第一行；然后对每个聚合函数：ROLLUP 场景调 `Item_rollup_sum_switcher::reset_and_add_for_rollup(m_last_unchanged_group_item_idx)`，非 ROLLUP 场景调 `(*item)->reset_and_add()`。
  - 随后进入内层 `for (;;)` 循环继续读行：读到 EOF 时存下 `m_first_row_next_group`（为了迭代器被重执行时能恢复，见关联子查询场景）、恢复 `m_first_row_this_group`、若 `m_rollup && m_join->send_group_parts > 0` 则转入 `OUTPUTTING_ROLLUP_ROWS`（输出最终组与总计行），否则转入 `DONE_OUTPUTTING_ROWS`，返回 `0`。
  - 组未变时：`int first_changed_idx = update_item_cache_if_changed(m_join->group_fields);` 返回首个变化的分组字段下标（**`update_item_cache_if_changed` 的返回值语义是 `-1` 表示没变，否则是第一个变化的项的下标**，定义在 `sql/sql_executor.cc`）。若 `first_changed_idx >= 0` 则组切换：存下新行、恢复旧行、ROLLUP 场景下 `m_last_unchanged_group_item_idx = first_changed_idx + 1` 并按 `first_changed_idx < m_join->send_group_parts - 1` 决定是转 `OUTPUTTING_ROLLUP_ROWS` 还是 `LAST_ROW_STARTED_NEW_GROUP`。
  - 组未变则对每个聚合函数累加：ROLLUP 场景调 `Item_rollup_sum_switcher::aggregator_add_all()`，非 ROLLUP 场景调 `(*item)->aggregator_add()`。
- **`OUTPUTTING_ROLLUP_ROWS`**：`SetRollupLevel(m_current_rollup_position - 1)` 逐级回退；`m_current_rollup_position <= m_last_unchanged_group_item_idx` 时结束 ROLLUP 输出，按 `m_seen_eof` 决定转 `DONE_OUTPUTTING_ROWS` 还是 `LAST_ROW_STARTED_NEW_GROUP`。
- **`DONE_OUTPUTTING_ROWS`**：若 `m_save_nullinfo != 0` 则 `m_join->restore_fields(m_save_nullinfo)` 还原被置 NULL 的字段；`SetRollupLevel(INT_MAX)`；返回 `-1`。

**`SetRollupLevel(int level)`** 只有在 `m_rollup && m_current_rollup_position != level` 时才动作：设置位置后，遍历 `m_join->rollup_group_items` 调 `item->set_current_rollup_level(level)`，遍历 `m_join->rollup_sums` 调 `item->set_current_rollup_level(level)`。**这就是 ROLLUP 的「逐级把分组列置 NULL」的实现机制**——由迭代器统一驱动各 ROLLUP 包装项自己的层级状态。

`Item_rollup_sum_switcher::reset_and_add_for_rollup(int last_unchanged_group_item_idx)`（定义在 `sql/item_sum.cc`）的循环体是：`i >= last_unchanged_group_item_idx` 的子项调 `reset_and_add()`，否则调 `aggregator_add()`——**即「从这一级往上重新开始累加，这一级以下沿用已有累加值」**。

### `AggregateIterator` 的一个已知缺陷（源码自述）

`READING_FIRST_ROW`/`LAST_ROW_STARTED_NEW_GROUP` 分支的注释明确写着一处限制：

> NOTE: This does not save and restore FTS information, so evaluating MATCH() on these rows may give the wrong result.

并给出替代方案：「Storing the row ID and repositioning it with ha_rnd_pos() would, but we can't do the latter without disturbing ongoing scans. See bug #32565923.」以及两条现实出路：旧 join 优化器靠插入临时表或排序来正确恢复该信息；hypergraph join 优化器则为 MATCH 列加一个专门的 streaming 步骤。

### `WindowIterator`：不缓冲形态

`WindowIterator` 的 `Read()` 只有五步，顺序固定：

1. `SwitchSlice(m_join, m_input_slice);` —— 切到输入切片。
2. `int err = m_source->Read();` —— 读源行。
3. `SwitchSlice(m_join, m_output_slice);` —— 切到输出切片。
4. `err != 0` 直接返回。
5. `copy_funcs(m_temp_table_param, thd(), CFT_HAS_NO_WF)` —— 算不含窗口函数的表达式。
6. `m_window->check_partition_boundary();` —— 检查分区边界。
7. `copy_funcs(m_temp_table_param, thd(), CFT_WF)` —— 算窗口函数。
8. `if (m_window->is_last() && copy_funcs(m_temp_table_param, thd(), CFT_HAS_WF)) return 1;` —— 最后一行的收尾。

**关键点：`WindowIterator` 是「读一行、算一行、出一行」的直通形态**——它不缓存分区，也不做 ROLLUP 式的多级输出。`Init()` 除了转发 `m_source->Init()`，只做两件事：`m_window->reset_round();` 与记录 `m_input_slice = m_join->get_ref_item_slice();`。

### `BufferingWindowIterator`：缓冲形态

`BufferingWindowIterator` 是需要缓冲时的形态，`Read()` 的结构是「先吐已缓冲的行，再读新行」：

- `m_possibly_buffered_rows` 为真时，先调 `ReadBufferedRow(m_last_input_row_started_new_partition)`，若返回非 `-1` 就直接返回——**因为上一次 `Read()` 可能已经算好了多行，但一次只能返回一行**，剩下的留给下一次调用。
- 读到新行后，先在**输入切片**下 `copy_funcs(..., CFT_HAS_NO_WF)`（注释说明原因：此时还没有 LEAD 的数据，先把非窗口函数的值存下来，等行完全可算时再重新求值一次 `CFT_NON_WF` 得到正确值），再调 `buffer_windowing_record(thd(), m_temp_table_param, &new_partition)` 把行缓冲起来。
- `buffer_windowing_record()` 里有一个重要的分区切换协议：若 `w->partition_rowno()` 从 `0` 变成 `1` 且不是第一个分区，则 `*new_partition = true`，并把这一行以 `Window::FBC_FIRST_IN_NEXT_PARTITION` 这个特殊行号 `w->save_special_row(...)` 存进内存，**这一行暂不写帧缓冲**；`BufferingWindowIterator::Read()` 下一轮再用 `bring_back_frame_row(thd(), m_window, Window::FBC_FIRST_IN_NEXT_PARTITION, ...)` 取回来真正缓冲。
- `ReadBufferedRow(bool new_partition_or_eof)` 调 `process_buffered_windowing_record(...)`，若返回错误码 `1` 则返回 `1`；`thd()->killed` 命中则 `send_kill_message()` 并返回 `1`；`output_row_ready` 为真时置 `m_possibly_buffered_rows = true` 并返回 `0`，否则返回 `-1`。

### `process_buffered_windowing_record`：窗口执行的核心

这是窗口函数求值的调度中心（`sql/iterators/window_iterators.cc` 的匿名命名空间内）。它的关键结构决策：

- **三种互斥的执行策略**：`static_aggregate`（`w.static_aggregates()`）、`row_optimizable`（`w.optimizable_row_aggregates()`）、`range_optimizable`（`w.optimizable_range_aggregates()`），源码用 `assert((static_aggregate + row_optimizable + range_optimizable) <= 1);` 保证互斥。
  - **static（静态聚合）**：窗口帧是整个分区且没有 `ORDER BY` 时，聚合值只需在分区第一行算一次并保存，后续行直接复用。
  - **row_optimizable / range_optimizable（反转优化）**：帧滑动时，行离开帧就用「逆函数」把它的贡献减掉，行进入帧再用正常聚合函数加进来。注释指出该优化对整型数据在无溢出时结果相同；**对浮点数结果可能不同，所以默认不做**，除非会话变量 `windowing_use_high_precision` 打开。
- **帧边界的数值上限**：RANGE 帧的 `upper_limit = INT64_MAX`（因为 RANGE 帧要先缓冲整个分区才能确定帧尾）。ROWS 帧的边界计算中，若 `border < 0` 则 `border = INT64_MAX`（处理无符号值回绕）；`WBT_VALUE_FOLLOWING` 分支里若 `border > (std::numeric_limits<int64>::max() - current_row)`，则 `lower_within_limits = false` 且 `lower_limit = INT64_MAX`；若上下边界都越界则 `lower_limit = INT64_MAX; upper_limit = INT64_MAX - 1;`，注释说明「the window frame is empty」。
- **`lower_limit` 的下界恒为 1**：`WBT_VALUE_PRECEDING` 分支用 `lower_limit = std::max<int64>(current_row - border, 1);`，注释写明「Invariant: lower_limit >= 1 after initialization」。
- **缓冲够了才输出**：`if (!((lower_limit <= last_rowno_in_cache && upper_limit <= last_rowno_in_cache && !w.needs_partition_cardinality()) || new_partition_or_eof)) return false;` —— 需要分区基数的窗口函数必须缓冲整个分区。
- **`copy_funcs()` 的 `CFT_*` 分派标志**是窗口求值的动作分类：`CFT_WF_NON_FRAMING`（如 ROW_NUMBER / RANK / DENSE_RANK）、`CFT_WF_FRAMING`（如 SUM / AVG / MIN / MAX）、`CFT_WF_USES_ONLY_ONE_ROW`（FIRST_VALUE / LAST_VALUE / NTH_VALUE 等只看一行的）、`CFT_WF_NEEDS_PARTITION_CARDINALITY`（NTILE、LEAD/LAG）、以及 `CFT_HAS_NO_WF` / `CFT_WF` / `CFT_HAS_WF`。
- **peer set（同行集）**：`needs_peerset` 为真且当前行还没确定 peer set 时，循环向后扫描到 peer 集末尾，用 `w.set_last_rowno_in_peerset(...)` 记录。`w.in_new_order_by_peer_set()` 判断新行是否仍属同一 peer set。

### 帧缓冲的行存取

`bring_back_frame_row(thd, w, rowno, reason, fno)` 从帧缓冲临时表取回指定行并回填到输出表的缓冲：

- `do_fetch = w.row_has_fields_in_out_table() != rowno;` —— **若请求的行正是上次取过并已拷到输出表的那行，就不重新取**，这是避免重复拷贝的优化；注释说明即使 `do_fetch` 为假也要更新位置提示，因为 `reason` 与 `fno` 可能与上次不同。
- 取行靠 `read_frame_buffer_row(rowno, w, for_nth_value)`，它从 `w->m_frame_buffer_positions` 里挑**行号差最小**的已缓存位置做 `ha_rnd_pos()` 定位，再按需 `ha_rnd_next()` 前移。
- 取到后 `t->file->position(fb_rec)` 并把 `t->file->ref` 拷进 `w->m_frame_buffer_positions[static_cast<int>(reason) + fno].m_position`，同时记录 `m_rowno = rowno`——**位置提示数组按 `reason + fno` 索引**，`reason` 是 `Window_retrieve_cached_row_reason` 枚举。
- 回填用 `copy_fields(fb_info, thd, true)`，`reverse_copy = true` 表示反向拷贝（从帧缓冲拷回输入表缓冲）。

`buffer_record_somewhere(thd, w, rowno)` 负责写帧缓冲：`ha_write_row()` 之后 `w->set_frame_buffer_total_rows(w->frame_buffer_total_rows() + 1)`。**写失败时的降级路径**是这一段里最值得记的：若 `t->file->is_ignorable_error(error)`（重复键）则直接返回错误；否则调 `create_ondisk_from_heap(thd, t, error, /*insert_last_record=*/true, /*ignore_last_dup=*/true, &is_duplicate)` —— **即内存临时表溢出时转成磁盘表**。转盘后需要重建所有位置提示（注释说明「all hints pertain to the in-memory file, not the new on-disk one」），并用 `encode_innodb_position()` 按 `w->frame_buffer_partition_offset()` 编码位置。

## 关键取舍

**取舍一：过滤与聚合都做成「不物化」的流式算子，但聚合必须保存行。** 过滤完全流式（读—判—丢）；聚合则必须把「下一组的第一行」和「本组的第一行」各存一份，否则无法在组边界上回退输出正确的分组值。代价是 `AggregateIterator` 依赖 `pack_rows` 基础设施把行打包进 `String` 缓冲，**并且这个打包不保存 FTS 信息**——源码自己承认这会让 `MATCH()` 求值出错（bug #32565923）。**流式的收益是不写临时表；代价是聚合的分组边界靠「看到下一组的第一行」来判定，因此必须依赖输入已经按组相邻排列这一前提。**

**取舍二：`AggregateIterator` 假设「组内行相邻」，排序与分组的先后顺序交给上层。** 类注释把三种来源列了出来：之前排序过、扫描的索引天然给出组兼容的顺序、或者根本没有分组。**这个假设把「分组键有序性」的责任推给了优化器**，而不是让迭代器自己维护哈希表或排序。源码为此还要求 `!m_join->tmp_table_param.precomputed_group_by`——分组已经被预计算的场合另有实现。

**取舍三：ROLLUP 用「层级计数」而不是「重新聚合」实现。** `SetRollupLevel()` 统一驱动 `Item_rollup_group_item` 与 `Item_rollup_sum_switcher` 的层级状态，组切换时只记录「第一个未变化的分组项下标」`m_last_unchanged_group_item_idx`，随后逐级回退。代价是 ROLLUP 的语义被拆进两个地方：迭代器决定「回退到第几级」，`Item_rollup_sum_switcher::reset_and_add_for_rollup()` 决定「这一级往上重新累加、这一级以下沿用旧值」。

**取舍四：窗口函数的执行策略按「最难的那个窗口函数」选，而不是逐函数选。** 源码注释写明：因为策略是按窗口上「最难的」函数统一决定的，所以优化聚合路径里必须也能求非聚合函数（ROW_NUMBER、NTILE、FIRST_VALUE），于是朴素路径与优化路径里对这些函数有**重复的代码**。NTILE 更特殊——注释明确说它「needs two passes over the partition's rows since the cardinality is needed to compute it」，因此它归入 `CFT_WF_NEEDS_PARTITION_CARDINALITY` 一类，强制整个分区缓冲完。

**取舍五：浮点数不默认走反转优化。** 注释明确：对整型数据，在正常求值不溢出的前提下反转优化是安全的；对浮点数则「optimizing in this way may lead to different results, so it is not done by default, cf the session variable `windowing_use_high_precision`」。这是**用可配置开关换取数值结果可复现性**的取舍。

**取舍六：帧缓冲溢出从内存转磁盘，靠位置提示数组维持定位效率。** 转换会作废所有已缓存的位置提示（因为它们指向内存文件），必须重建并用 `encode_innodb_position()` 编码新位置。代价是转换点之后定位效率下降，收益是帧缓冲大小不再受内存限制。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/iterators/composite_iterators.cc` | `FilterIterator::Read` | 过滤执行：读源行、`m_condition->val_int()` 求值、先查 `thd()->killed` 再查 `thd()->is_error()`、不匹配时 `m_source->UnlockRow()` 后继续 |
| `sql/iterators/composite_iterators.cc` | `AggregateIterator::AggregateIterator` | 聚合迭代器构造：按 `ComputeRowSizeUpperBound(m_tables)` 为两个行打包缓冲 `m_first_row_this_group` / `m_first_row_next_group` 预留容量 |
| `sql/iterators/composite_iterators.cc` | `AggregateIterator::Init` | 聚合初始化：断言 `!precomputed_group_by`、复位 ROLLUP 层级为 `INT_MAX`、恢复上次遗留的 `m_first_row_next_group`、按 `implicit_grouping`/`group_optimized_away`/`using_hypergraph_optimizer` 决定是否取 `m_output_slice` |
| `sql/iterators/composite_iterators.cc` | `AggregateIterator::Read` | 聚合主状态机：`READING_FIRST_ROW` / `LAST_ROW_STARTED_NEW_GROUP` / `OUTPUTTING_ROLLUP_ROWS` / `DONE_OUTPUTTING_ROWS` 四态切换，含零输入行仍输出一行、组切换、`reset_and_add`/`aggregator_add` 累加、ROLLUP 层级推进 |
| `sql/iterators/composite_iterators.cc` | `AggregateIterator::SetRollupLevel` | ROLLUP 层级驱动：仅当 `m_rollup` 且层级变化时，向 `m_join->rollup_group_items` 调 `set_current_rollup_level`、向 `m_join->rollup_sums` 调 `set_current_rollup_level` |
| `sql/iterators/composite_iterators.cc` | `WindowIterator` | 窗口函数直通形态：构造函数断言 `!m_window->needs_buffering()` |
| `sql/iterators/window_iterators.cc` | `WindowIterator::Init` | 窗口初始化：转发 `m_source->Init()`、`m_window->reset_round()`、记录 `m_input_slice` |
| `sql/iterators/window_iterators.cc` | `WindowIterator::Read` | 窗口执行：输入切片读行 → 切输出切片 → `CFT_HAS_NO_WF` → `check_partition_boundary()` → `CFT_WF` → 末行 `CFT_HAS_WF` |
| `sql/iterators/window_iterators.cc` | `BufferingWindowIterator::Read` | 窗口缓冲形态：先吐上次遗留的缓冲行，再读新行并在输入切片下 `CFT_HAS_NO_WF`，用 `buffer_windowing_record` 缓冲，分区首行以 `FBC_FIRST_IN_NEXT_PARTITION` 延后处理 |
| `sql/iterators/window_iterators.cc` | `BufferingWindowIterator::ReadBufferedRow` | 缓冲行输出：调 `process_buffered_windowing_record`，查 `thd()->killed`，按 `output_row_ready` 置 `m_possibly_buffered_rows` 并返回 `0` 或 `-1` |
| `sql/iterators/window_iterators.cc` | `process_buffered_windowing_record` | 窗口求值调度中心：三种互斥策略（static / row_optimizable / range_optimizable）、帧边界 `lower_limit`/`upper_limit` 计算、peer set 确定、`CFT_WF_*` 分派 |
| `sql/iterators/window_iterators.cc` | `buffer_windowing_record` | 分区边界协议：`partition_rowno()` 从 0 变 1 时置 `*new_partition = true` 并以 `FBC_FIRST_IN_NEXT_PARTITION` 保存特殊行 |
| `sql/iterators/window_iterators.cc` | `buffer_record_somewhere` | 写帧缓冲：`ha_write_row` 后累加 `frame_buffer_total_rows`；不可忽略错误时 `create_ondisk_from_heap` 转磁盘并重建位置提示 |
| `sql/iterators/window_iterators.cc` | `bring_back_frame_row` | 从帧缓冲取行回填：`do_fetch` 复用判断、`read_frame_buffer_row` 定位、`copy_fields(..., true)` 反向拷贝、按 `reason + fno` 更新位置提示 |
| `sql/iterators/window_iterators.cc` | `read_frame_buffer_row` | 帧缓冲定位：从 `m_frame_buffer_positions` 选行号差最小者做 `ha_rnd_pos()`，再按需 `ha_rnd_next()` 前移 |
| `sql/iterators/window_iterators.cc` | `process_wfs_needing_partition_cardinality` | 需要分区基数的窗口函数（NTILE、LEAD/LAG）求值：`CFT_WF_NEEDS_PARTITION_CARDINALITY` 分派，LEAD/LAG 按 `current_row - ll.m_rowno` 回访帧缓冲 |
| `sql/iterators/window_iterators.cc` | `SwitchSlice` | 切片切换辅助函数：`slice_num != -1` 且 `join->ref_items[slice_num]` 非空时才 `join->set_ref_item_slice(slice_num)` |
| `sql/iterators/window_iterators.cc` | `reset_framing_wf_states` / `reset_non_framing_wf_state` | 窗口函数状态复位：遍历 `Func_ptr_array` 用 `Item::reset_wf_state` 做 POSTFIX 遍历，参数区分 framing 与非 framing |
| `sql/item_sum.cc` | `Item_rollup_sum_switcher::reset_and_add_for_rollup` | ROLLUP 聚合累加：下标 `>= last_unchanged_group_item_idx` 的子项调 `reset_and_add()`，其余调 `aggregator_add()` |
| `sql/item_sum.cc` | `Item_rollup_sum_switcher::clear` | ROLLUP 聚合清零：遍历 `m_num_levels` 个子项调 `child(i)->clear()` |
| `sql/sql_executor.cc` | `update_item_cache_if_changed` | 分组字段变更检测：遍历 `List<Cached_item>` 调 `cmp()`，返回 `-1` 表示没变，否则返回第一个变化的项的下标 |
| `sql/sql_executor.cc` | `PossiblyAttachFilter` | 旧优化器的过滤装配：常量条件 `val_int()` 为 0 且路径不含 `ContainsAnyMRRPaths` 时返回 `NewZeroRowsAccessPath(thd, path, "Impossible filter")`，否则 `CreateConjunction` 合成条件后 `NewFilterAccessPath` |
| `sql/sql_executor.cc` | `NewAggregateAccessPath` | 旧优化器的聚合装配入口：`JOIN::create_access_paths()` 中以 `rollup_state != RollupState::NONE` 作为 rollup 参数创建聚合访问路径 |
| `sql/join_optimizer/access_path.cc` | `CreateIteratorFromAccessPath` | hypergraph 优化器的迭代器装配：`AccessPath::FILTER` → `NewIterator<FilterIterator>`、`AccessPath::AGGREGATE` → `NewIterator<AggregateIterator>`（传 `param.rollup`）、`AccessPath::WINDOW` → 按 `param.needs_buffering` 选 `NewIterator<BufferingWindowIterator>` 或 `NewIterator<WindowIterator>` |

## 与主核 / AI 的关系

主核（`engine/db`）目前没有「过滤 / 聚合 / 窗口」这三类可组合的行流算子。其查询执行路径是固定形态，聚合语义直接落在执行引擎里，不存在「读一行—判—丢」或「攒够一组再输出」这种可插拔的算子形态，也没有窗口帧缓冲这种带降级路径（内存转磁盘）的执行结构。

**架构推论：** MySQL 这套设计里有三点值得主核借鉴。其一是**「过滤能解锁、聚合不能解锁」的协议细节**：它来自聚合「当前行属于下一组」这一事实，并被明确写进 `AggregateIterator::UnlockRow()` 的注释里。主核若也在流式算子上做行级解锁，必须同样回答「这一行归谁」——这不能靠通用协议默认，必须由算子自己给出。其二是**「分组相邻性作为前提、由优化器负责排序」的分工**：`AggregateIterator` 的类注释把「输入已按组兼容顺序」写成契约。这让聚合算子本身保持简单，但把正确性责任转移到了计划生成侧——主核若采用同一分工，就需要像 MySQL 那样在优化器与执行器之间把这层契约显式化。其三是**「帧缓冲溢出转磁盘」这一降级路径被做在算子内部**：写帧缓冲失败时由 `create_ondisk_from_heap` 完成转换、作废并重建位置提示。这让窗口函数在超大分区上仍可执行，代价是转换点之后的定位效率下降。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「同一个语义有多少种落地形态」。**架构推论：** MySQL 把窗口函数的执行策略做成了三选一的互斥判定（`static_aggregate` / `row_optimizable` / `range_optimizable`），而且**策略是按窗口上最难的那个函数统一决定的**，导致优化路径里必须重复实现非聚合函数的求值代码。对 AI 而言，这意味着「一条 SQL 的实际执行代价」并不只由 SQL 文本决定，还由「窗口上恰好有哪些函数」的组合决定；因此当 AI 需要解释或优化一条慢查询时，仅看语义是不够的，还需要拿到「本次选了哪种策略、为什么」。主核若要支持 AI 参与执行策略选择，应当把「策略选择依据」做成可查询的结构化信息，而不是只暴露最终的耗时数字。

## 参考

- [MySQL 源码：`sql/iterators/composite_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.cc) —— `FilterIterator::Read`、`AggregateIterator::Init`/`Read`/`SetRollupLevel` 的完整实现。
- [MySQL 源码：`sql/iterators/composite_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.h) —— `FilterIterator` 与 `AggregateIterator` 的类声明、状态枚举与成员注释（含「保存/恢复行」的设计说明）。
- [MySQL 源码：`sql/iterators/window_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/window_iterators.cc) —— `WindowIterator`、`BufferingWindowIterator` 与 `process_buffered_windowing_record` 的执行逻辑。
- [MySQL 源码：`sql/iterators/window_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/window_iterators.h) —— 两个窗口迭代器的类声明与 `needs_buffering()` 的分工。
- [MySQL 源码：`sql/join_optimizer/access_path.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.cc) —— `AccessPath::FILTER`/`AGGREGATE`/`WINDOW` 到迭代器实例的装配。
- [MySQL 源码：`sql/sql_executor.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_executor.cc) —— 旧优化器路径的 `PossiblyAttachFilter`、`update_item_cache_if_changed` 与聚合访问路径创建。
- [MySQL 源码：`sql/item_sum.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/item_sum.cc) —— `Item_rollup_sum_switcher` 的 ROLLUP 累加与清零实现。
- [MySQL 参考手册：GROUP BY 优化](https://dev.mysql.com/doc/refman/8.0/en/group-by.html) —— GROUP BY 的执行方式与「分组键有序性」的语义背景。
- [MySQL 参考手册：窗口函数语法](https://dev.mysql.com/doc/refman/8.0/en/window-function-syntax.html) —— 窗口帧边界（ROWS / RANGE、UNBOUNDED 与 CURRENT ROW）的定义。
- [MySQL 参考手册：窗口函数使用](https://dev.mysql.com/doc/refman/8.0/en/window-function-usage.html) —— OVER 子句与分区、排序、帧的整体语义。
