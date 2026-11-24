---
project: mysql-server
feature: temp-table
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_tmp_table.cc
    symbols: [create_tmp_table, create_tmp_table_from_fields]
---

# 内部临时表

## 一句话职责

在服务器层为中间结果建立一张**内部临时表**：推导每一列的字段类型、按 `group`/`distinct` 需求建立索引或唯一约束、挑选存储引擎（TempTable / MEMORY / InnoDB），并把上游算子产出的行回填进去。

## 机制是什么

内部临时表与用户用 `CREATE TEMPORARY TABLE` 建的临时表是两套东西。`sql/sql_tmp_table.cc` 里的临时表没有磁盘上的数据字典条目，也没有独立的 `TABLE_SHARE` 目录项：它由 `TABLE_SHARE` + 一个或多个 `TABLE` 对象表示，其中至多一个 `TABLE` 是写者，其余是读者。

生命周期被拆成两段，源码用两个计数器分别跟踪：

- `tmp_handler_count`：已经分配并锁定存储引擎的 `TABLE` 数量，在 `setup_tmp_table_handler()` 里自增。
- `tmp_open_count`：已经打开（内容已创建）的 `TABLE` 数量，在 `open_tmp_table()` 里自增。

打开临时表因此是两阶段操作：先分配并锁定存储引擎，再创建表内容。`instantiate_tmp_table()` 负责把这两步串起来。

字段推导的入口是 `create_tmp_field()`：它按 `Item` 的 `result_type()` 决定列的物理类型——`REAL_RESULT` 走 `Field_float`/`Field_double`，`INT_RESULT` 在 `item->max_length >= (MY_INT32_NUM_DECIMAL_DIGITS - 1)` 时用 `Field_longlong`、否则用 `Field_long`，`STRING_RESULT` 走 `item->make_string_field()`（但 DATE/TIME、GEOMETRY、JSON 因为 `result_type()` 同为 `STRING_RESULT` 而被单独走 `tmp_table_field_from_field_type()` 以保住类型），`DECIMAL_RESULT` 走 `Field_new_decimal::create_from_item()`。

## 怎么实现的

### 1. 字段与索引的推导

`create_tmp_table()` 的入口先处理两个互斥关系：`param->allow_group_via_temp_table` 为假时把 `group` 置空；`group` 非空时把 `distinct` 置假（两者不能同时用）。

随后遍历 `fields` 列表逐列建字段。对每个 `Item`：

- 若 `not_all_columns`（即没有 `TMP_TABLE_ALL_COLUMNS`）且该 `Item` 带聚合但不是 `SUM_FUNC_ITEM`，且它引用了子查询或外部表，则置 `param->using_outer_summary_function = true` 且不存这一列。
- 窗口函数同理：`param->m_window` 为空说明这是「窗口化之前的表」，不存；`param->m_window` 与该窗口函数所属窗口不等，说明是别的窗口的函数，也不存。
- 隐式分组（`is_implicitly_grouped()`）且只用到 `RAND_TABLE_BIT`/`INNER_TABLE_BIT` 的表达式，以及在优化期就能求值的常量项（`const_for_execution()` 且 `evaluate_during_optimization()`），直接跳过不落列。

聚合函数的参数列在这里被单独展开成独立字段，并通过 `sum_item->set_arg(thd, i, new (thd->mem_root) Item_field(new_field))` 把聚合函数的实参替换成指向临时表列的 `Item_field`。

唯一约束有两条路：

- **用索引**：在结果列上建 `KEY`，标志位为 `HA_NOSAME | HA_NULL_ARE_EQUAL`，名字为 `<auto_distinct_key>`；分组场景用 `HA_NOSAME`，名字为 `<group_key>`。
- **用哈希列**：加一个隐藏的 `Field_longlong(sizeof(ulonglong), false, "<hash_field>", true)`，再在它上面建单键索引，名字按场景是 `<hash_distinct_key>` 或 `<hash_group_key>`。

走哪条路由 `unique_constraint_via_hash_field` 决定，置位的判据有：

- `param->m_operation != Temp_table_param::TTP_UNION_OR_TABLE`（即 INTERSECT / EXCEPT）；
- 结果列里有 `BLOB_FLAG` 的列（BLOB 因长度问题强制走哈希）；
- `group` 分组项的 `max_char_length() > CONVERT_IF_BIGGER_TO_BLOB`，或该分组项 `is_non_deterministic()`（分组表达式会被求值两次，非确定性函数会导致两次结果不一致）；
- `param->group_parts > max_key_parts`、`param->group_length > max_key_length` 或 `param->group_length >= MAX_BLOB_WIDTH`；
- 建索引时单个键段 `store_length > max_key_part_length`；
- `distinct` 时 `distinct_key_length > max_key_length`，或 `(fieldnr - param->hidden_field_count) > max_key_parts`，或 `param->force_hash_field_for_unique`。

这里的三个上限不是常量写死的，而是 `get_max_key_and_part_length()` 取 HEAP 引擎与 InnoDB 引擎对应值的最小值：`max_key_length` 取 `HEAP_MAX_KEY_LENGTH` 与 `INNODB_MAX_KEY_LENGTH` 的较小者，`max_key_part_length` 取 `HEAP_MAX_KEY_PART_LENGTH` 与 `INNODB_MAX_KEY_PART_LENGTH` 的较小者，`max_key_parts` 取 `HEAP_MAX_KEY_PARTS` 与 `INNODB_MAX_KEY_PARTS` 的较小者。这些值由 `Cache_temp_engine_properties::init()` 在服务器启动时向各引擎索取并缓存；其中 `INNODB_MAX_KEY_PART_LENGTH` 在源码里直接硬编码为 `3072`，注释说明是为了让服务器侧与 InnoDB 侧保持一致，等 bug#20629014 修复后再去掉硬编码。之所以只取 HEAP 与 InnoDB 的最小值，是因为 HEAP 引擎在创建后仍可能转成磁盘引擎，键长度必须按最小的那个来校验。

INTERSECT / EXCEPT 还要额外加一个隐藏的 `<set counter>` 列（`Field_longlong(sizeof(ulonglong), false, "<set counter>", true)`），通过 `table->set_set_counter()` 登记。

### 2. 记录布局

`share->reclength` 累加每列的 `pack_length()`；可空列计入 `null_count`，BIT 列还要按 `field_length & 7` 累加 `total_uneven_bit_length`（非整字节的位要挤在 NULL 位图里）。`share->null_bytes` 按 `(hidden_null_pack_length + (null_count + total_uneven_bit_length + 7) / 8)` 计算；`share->reclength` 若算出来是 0 就强制置 1（源码注释称之为 "Dummy select"）。

`alloc_record_buffers()` 按 `ALIGN_SIZE(share->reclength + TMP_TABLE_UNIQUE_HASH_LENGTH + 1)` 分配 `rec_buff_length`，其中 `TMP_TABLE_UNIQUE_HASH_LENGTH` 是函数内定义的常量 `4`。它一次性分配 `alloc_length * 3 + share->null_bytes`，分别用作 `record[0]`、`record[1]` 和 `share->default_values`。

行数上限 `share->max_rows` 由 `alloc_record_buffers()` 依 `thd->variables.tmp_table_size` 推出：若 `tmp_table_size == ~(ulonglong)0`（无上限）则 `max_rows = ~(ha_rows)0`；否则按 `tmp_table_size` 与（仅当引擎是 HEAP 时）`thd->variables.max_heap_table_size` 取较小者再除以 `share->reclength`。结果再用 `std::max(share->max_rows, ha_rows(1))` 兜底。

是否用打包行格式由 `use_packed_rows` 决定，判据是：存在 BLOB 列，或 `string_total_length >= STRING_TOTAL_LENGTH_TO_PACK_ROWS`（宏，值 128）且（`share->reclength / string_total_length <= RATIO_TO_PACK_ROWS`（宏，值 2）或 `string_total_length / string_count >= AVG_STRING_LENGTH_TO_PACK_ROWS`（宏，值 64））。

各列的字节位置由 `relocate_field()` 安置：可空列的 NULL 位落在 NULL 位图里，BIT 列额外占位 `field_length & 7` 个比特。

### 3. 回填数据的顺序

真正往表里写行不是 `create_tmp_table()` 干的，而是通过 `param->items_to_copy`（`Func_ptr_array`）记录的表达式求值序列，在物化时逐列求值写入。`create_tmp_table()` 负责把这个数组排好序：`sort_copy_func()` 用 `std::stable_partition` 把「引用了同一个查询块里其他表达式的元素」稳定地移到数组末尾。

排序的理由写在 `sort_copy_func()` 的注释里：物化迭代器先切换到读临时表的 `REF_SLICE`，再 `copy_fields()` 拷贝连接输出，最后 `copy_funcs()` 求值表达式。由于 `REF_SLICE` 已经前移，`copy_funcs()` 求值时 `Item_ref` 可能指向临时表的列，所以求值顺序必须尊重依赖关系；而 `Query_block::fields` 里隐藏元素（GROUP BY / HAVING / ORDER BY 的元素，以及被 `split_sum_func()` 拆出来的聚合内层元素）排在前面，隐藏元素若通过别名引用 SELECT 列表的非隐藏元素，就必须把被引用者排到前面。

窗口函数还有一条额外规则：注释明确说明，带窗口函数的隐藏元素可能是聚合（由 `split_sum_func` 产出）**且**要被物化到更下一层的临时表，所以必须留在 `copy_func` 数组靠前位置；只有当它包含指向 SELECT 列表的别名引用时，排序才会把它移到末尾，而由于用的是 `stable_partition`，被别名的表达式会一起被移动，二者的相对顺序保持不变。

### 4. 存储引擎选择

引擎选择在 `setup_tmp_table_handler()` 里完成，入口参数是 `thd->variables.internal_tmp_mem_storage_engine`：

- 值为 `TMP_TABLE_TEMPTABLE` 时用 `temptable_hton`；
- 值为 `TMP_TABLE_MEMORY` 时用 `heap_hton`；
- `schema_table` 为真且内存引擎是 `TMP_TABLE_TEMPTABLE` 时会被改回 `TMP_TABLE_MEMORY`，理由是防止用户用 `CREATE TABLE ... LIKE information_schema.some` 的方式间接得到一张 TempTable 表。

改用 InnoDB（`innodb_hton`）的判据来自 `use_tmp_disk_storage_engine()`：

- `force_disk_table` 为真（调用方明确要求磁盘引擎）；
- `opt_initialize` 为真（服务器启动引导阶段）或 `thd->is_dd_system_thread()` 为真（此时 HEAP 引擎尚不可用）；
- 内存引擎是 `TMP_TABLE_MEMORY` 且 `share->blob_fields` 非零（源码注释：MEMORY 引擎不支持 BLOB）；
- `thd->variables.big_tables` 为真且 `select_options` 不含 `SELECT_SMALL_RESULT`（用户声明结果集很大，内存装不下）。

引擎用 `ha_lock_engine(nullptr, hton)` 锁定后赋给 `share->db_plugin`，然后 `get_new_handler()` 分配 handler 并 `share->tmp_handler_count++`。

### 5. 实例化与引擎降级

`instantiate_tmp_table()` 负责真正把表建出来：

- TempTable 引擎：调 `create_tmp_table_with_fallback()`；
- InnoDB 引擎：调 `create_tmp_table_with_fallback()`，随后 `empty_record(table)` 清掉随机数据再落盘；
- MEMORY 引擎：不做 `create()`，内容由 `open_tmp_table()` 创建。

`create_tmp_table_with_fallback()` 里的降级逻辑是：先在当前引擎上 `create()`，若返回 `HA_ERR_RECORD_FILE_FULL` 且当前引擎是 `temptable_hton`，就用 `get_new_handler(..., innodb_hton)` 换一个新 handler 再 `create()` 一次——即**内存 TempTable 装不下时回退到 InnoDB 磁盘临时表**，回退分支的调用方是 `instantiate_tmp_table()`（以及 `create_ondisk_from_heap()` 内部重建磁盘表时）。另外该函数对 InnoDB 有一条前置检查：`MYSQL_TYPE_STRING` 且 `key_length() > 1024` 的列直接报 `ER_TOO_LONG_KEY`（源码注释说明 InnoDB 的定长列长度上限是 1024）。

运行期「内存表写满」的降级由 `create_ondisk_from_heap()` 处理：只在错误码为 `HA_ERR_RECORD_FILE_FULL` 时继续，其它错误码直接按致命错误抛出（源码注释明确说不要把这类错误降级成 warning，以免影响 `INSERT IGNORE ... SELECT`）。它只接受 MEMORY 或 TempTable 两种内存引擎，其余引擎直接报错。降级过程是：新建 `TABLE_SHARE` 拷贝、锁定 `innodb_hton`、`create_tmp_table_with_fallback()` 建磁盘表、逐行 `ha_rnd_next` + `ha_write_row` 把旧表数据搬过去（用 `new_table.record[1]` 作中转缓冲），然后把原表句柄销毁、换成新的磁盘表句柄。

`create_ondisk_from_heap()` 的实际调用方（在本地检出的源码里可见）包括 `sql/iterators/composite_iterators.cc`、`sql/iterators/window_iterators.cc`、`sql/sql_executor.cc`、`sql/sql_show.cc`、`sql/sql_union.cc`、`sql/sql_update.cc`、`sql/item_sum.cc`——它们都是在写入临时表收到 `HA_ERR_RECORD_FILE_FULL` 时才触发降级。

如果表有多个克隆（递归 CTE 的场景），`create_ondisk_from_heap()` 会遍历 `Derived_refs_iterator` 把每个克隆都转成磁盘表；源码注释说明为什么要把写表 `wtable` 排在最前面先转：这样万一复制行时出错，可以干净地退回旧的 `TABLE_SHARE`，避免出现「A 已转换、B 未转换」的半转换状态。

### 6. `create_tmp_table_from_fields()` 的另一条路

`create_tmp_table_from_fields()` 不从 `Item` 列表推导字段，而是直接从 `List<Create_field>` 的列定义建表。它与 `create_tmp_table()` 的区别在源码注释里写明：它建的表没有分组、没有索引、没有 copy_funcs 数组，作用是「不经中间 Item 列表，直接为表函数创建结果表」。

- `is_virtual` 为真时：只分配一块记录缓冲（`share->rec_buff_length = ALIGN_SIZE(share->reclength + 1)`），没有存储引擎、没有键，仅供把数据读写到 `table->record[0]` 供外部容器（RB 树、哈希表等）保存；此时表建在 `thd->mem_root` 上。
- `is_virtual` 为假时：走 `alloc_record_buffers()` 分配缓冲，然后 `setup_tmp_table_handler()` 分配存储引擎。

它的实际调用方在本地检出里可见两处：`sql/item_sum.cc`（为 `Item_sum` 的内存树 `Unique` 计算 `tree_key_length = table->s->reclength - table->s->null_bytes`）和 `sql/rpl_utility.cc`（行事件格式转换时构造转换表）。

### 7. 关闭与释放

`close_tmp_table()` 负责关闭：先 `filesort_free_buffers(table, true)`，若表已创建则 `tmp_open_count` 自减；减到还有剩余就 `ha_close()`，减到 0 就 `ha_drop_table()` 删除表内容；随后销毁 handler 并把 `table->file` 置空；当 `tmp_handler_count` 自减到 0 时 `plugin_unlock(nullptr, share->db_plugin)` 释放插件锁。

`free_tmp_table()` 只能在未实例化的表上调用，它把 `TABLE_SHARE` 的引用计数减一；减到 0 时把 `share->mem_root` 移出来 `Clear()` 掉。

## 关键取舍

- **键 vs 哈希列**：能用索引表达的唯一性就用索引（`<auto_distinct_key>` / `<group_key>`），只有键太长、键段太多、含 BLOB 或调用方明确要求时才加 `<hash_field>`。代价是哈希列要额外占 `sizeof(ulonglong)` 字节并且唯一性判定从「索引比较」降级为「哈希值比较」。
- **键长度上限取 HEAP 与 InnoDB 的最小值**，而不是按实际选中的引擎算。代价是 InnoDB 表的可用键长被 MEMORY 引擎的能力拉低；收益是 HEAP 表后续转磁盘时索引仍然可用。
- **非确定性分组表达式强制走哈希列**：`group` 分组项会被求值两次（一次分组、一次取值），若函数非确定则两次结果不一致，因此不能靠索引做唯一性判定。
- **打包行格式用启发式判断**（`STRING_TOTAL_LENGTH_TO_PACK_ROWS` / `RATIO_TO_PACK_ROWS` / `AVG_STRING_LENGTH_TO_PACK_ROWS` 三个宏），而不是精确计算省下的字节数。源码注释里的理由是「为了减少占用，请求存储引擎打包变长字段」。
- **窗口函数列被"推迟存储"**：含窗口函数的表达式不写进窗口化表，而是等到最后一个窗口处理完才写。这避免了同一列被反复物化，但代价是需要多轮物化。
- **`sort_copy_func()` 用稳定分区而不是重排**：源码注释承认更彻底的方案（让别名引用自然地排在后面）实现困难，因为「有些代码依赖非隐藏元素排在最后」这一既有约定。
- **内存装不下时才降级到磁盘**：TempTable 在 `create()` 阶段返回 `HA_ERR_RECORD_FILE_FULL` 时才切 InnoDB；运行期写满则由 `create_ondisk_from_heap()` 搬运数据。代价是降级时要做一次全表行拷贝。
- **引擎降级只降不升**：`use_tmp_disk_storage_engine()` 一旦判定用 InnoDB 就不会退回内存引擎。
- **`create_tmp_table_from_fields()` 刻意做成"窄接口"**：不带分组、索引、copy_funcs，因为它的用途（表函数结果表、行事件转换表）本来就不需要这些。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/sql_tmp_table.cc | `create_tmp_table` | 主入口：推导字段、建立 group/distinct 键或哈希列、计算记录布局与行数上限、挑选并锁定存储引擎 |
| sql/sql_tmp_table.cc | `create_tmp_table_from_fields` | 从 `List<Create_field>` 列定义建表（可虚拟表或普通临时表），无分组/索引/copy_funcs |
| sql/sql_tmp_table.cc | `create_tmp_field` | 按 `Item` 的类型与 `result_type()` 决定临时表列的物理类型 |
| sql/sql_tmp_table.cc | `create_tmp_field_from_field` | 以已有 `Field` 为模板复制出新列（`org_field->new_field()`） |
| sql/sql_tmp_table.cc | `create_tmp_field_from_item` | 按 `result_type()` 新建列：`Field_float`/`Field_double`/`Field_long`/`Field_longlong`/`Field_new_decimal`/字符串列 |
| sql/sql_tmp_table.cc | `setup_tmp_table_handler` | 依据 `thd->variables.internal_tmp_mem_storage_engine` 与 `use_tmp_disk_storage_engine()` 选 TempTable/MEMORY/InnoDB，`ha_lock_engine()` 锁定插件并分配 handler |
| sql/sql_tmp_table.cc | `use_tmp_disk_storage_engine` | 判定是否必须用磁盘引擎：强制磁盘、引导期、含 BLOB、`big_tables` |
| sql/sql_tmp_table.cc | `instantiate_tmp_table` | 实例化：分配引擎 → `create_tmp_table_with_fallback()` → `open_tmp_table()` |
| sql/sql_tmp_table.cc | `create_tmp_table_with_fallback` | TempTable 内存不足（`HA_ERR_RECORD_FILE_FULL`）时换 InnoDB handler 重建；并对 InnoDB 的 `MYSQL_TYPE_STRING` 列做 1024 字节键长检查 |
| sql/sql_tmp_table.cc | `create_ondisk_from_heap` | 运行期内存表写满时把 MEMORY/TempTable 表转换为 InnoDB 磁盘表并搬运全部行 |
| sql/sql_tmp_table.cc | `alloc_record_buffers` | 按 `share->reclength` 分配 `record[0]/record[1]/default_values`，并依 `tmp_table_size` 与 `max_heap_table_size` 推出 `share->max_rows` |
| sql/sql_tmp_table.cc | `get_max_key_and_part_length` | 取 HEAP 与 InnoDB 的 `max_key_length`/`max_key_part_length`/`max_key_parts` 最小值作为唯一性判据 |
| sql/sql_tmp_table.cc | `Cache_temp_engine_properties` | 启动时向 HEAP/TempTable/InnoDB 索取键长能力并缓存（`INNODB_MAX_KEY_PART_LENGTH` 硬编码 3072） |
| sql/sql_tmp_table.cc | `sort_copy_func` | 用 `std::stable_partition` 把引用同块其他表达式的求值项移到 `items_to_copy` 末尾，保证求值依赖顺序 |
| sql/sql_tmp_table.cc | `relocate_field` | 把各列安置到记录缓冲的正确字节偏移，并分配 NULL 位图中的位 |
| sql/sql_tmp_table.cc | `register_hidden_field` | 在隐藏字段区头部插入 `<hash_field>` / `<set counter>` |
| sql/sql_tmp_table.cc | `setup_tmp_table_column_bitmaps` | 初始化临时表的 `def_read_set`/`tmp_set`/`cond_set` 列使用位图 |
| sql/sql_tmp_table.cc | `open_tmp_table` | 调 `ha_open()`（`HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE`），`tmp_open_count++` 并置 created 标志 |
| sql/sql_tmp_table.cc | `close_tmp_table` | 关闭表：`tmp_open_count` 自减，归零时 `ha_drop_table()`；`tmp_handler_count` 归零时 `plugin_unlock()` |
| sql/sql_tmp_table.cc | `free_tmp_table` | 引用计数归零时释放 `TABLE_SHARE` 的 mem_root |
| sql/sql_tmp_table.cc | `create_duplicate_weedout_tmp_table` | 半连接去重用：单列 `rowids` 的 VARBINARY 表，超长时改用 `<hash_field>` 唯一约束 |
| sql/sql_tmp_table.cc | `create_tmp_table_field_tmp_name` | 列为 InnoDB 临时表生成非空字段名（InnoDB 不接受空名字段） |
| sql/sql_tmp_table.cc | `set_real_row_type` | 依引擎计算 `TABLE_SHARE::real_row_type` |
| sql/sql_tmp_table.cc | `reposition_innodb_cursor` | 引擎转换后按 InnoDB 自增主键把游标重新定位到同一行 |
| sql/sql_tmp_table.cc | `trace_tmp_table` | 向 optimizer trace 输出临时表的列数、行长、键长、是否唯一约束、存储位置等信息 |

## 与主核 / AI 的关系

**架构推论：** 这一层是「执行计划」与「存储引擎」之间的适配层——它把优化器产出的 `Item` 表达式列表翻译成一张有物理列、有索引、有记录缓冲的表，因此主核如果要实现「中间结果物化」，需要复刻的是同一套转换：`Item` → 字段类型推导、依赖排序（`sort_copy_func` 对应的 `copy_funcs()` 求值顺序）、以及容量触顶时的存储降级（`create_ondisk_from_heap` 对应的内存表转磁盘表）。这三件事都不在存储引擎内部，而在引擎之上。

**架构推论：** 对 AI 辅助的数据库内核而言，`create_tmp_table()` 里的「先算字段类型、再算键长度可行性、必要时退化为哈希列」是一个可直接借鉴的决策树：唯一性约束的表达成本（索引 vs 哈希列）在建表期就要定下来，且必须按**可能用到的最弱引擎**来校验键长度，否则后续从内存转磁盘时索引会失效。

**架构推论：** `create_ondisk_from_heap()` 里「先转写表、再转读者表」的顺序（以及注释里解释的半转换状态问题），说明在多副本共享同一份物化数据的场景下（递归 CTE），转换过程必须把不一致状态压到最小、并且能干净回退。

## 参考

- [MySQL 8.0 Reference Manual：Internal Temporary Tables](https://dev.mysql.com/doc/refman/8.0/en/internal-temporary-tables.html)
- [MySQL 8.0 Reference Manual：internal_tmp_mem_storage_engine 系统变量](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_internal_tmp_mem_storage_engine)
- [mysql/mysql-server 源码：sql/sql_tmp_table.cc（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_tmp_table.cc)
- [mysql/mysql-server 源码：storage/temptable 引擎目录（mysql-8.0.40 tag）](https://github.com/mysql/mysql-server/tree/mysql-8.0.40/storage/temptable)
