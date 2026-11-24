---
project: mysql-server
feature: dd-data-dictionary
page_type: feature
schema_version: 1
refs:
  - path: sql/dd/dd_table.cc
    symbols: [fill_dd_columns_from_create_fields]
  - path: sql/dd/dictionary.h
    symbols: [is_dd_table_name, acquire_shared_table_mdl]
---

# 数据字典

## 一句话职责

数据字典负责 MySQL 8.0 之后表与列元数据的**持久化位置与读取路径**：把 DDL 解析结果（`Create_field` 列表、`KEY` 数组）翻译成事务化的数据字典表行，并提供「这个表名是不是字典表」「这条路径要不要先拿元数据锁」这两类判定原语。

## 机制是什么

MySQL 5.7 及以前，表定义存在 `.frm` 文件里，元数据不是事务性的。8.0 把这类元数据搬进 `mysql` 库下的字典表，使其可以随 DDL 一起提交或回滚。

本机制在源码里体现为两层：

1. **写入侧**：`sql/dd/dd_table.cc` 里的 `fill_dd_columns_from_create_fields` 一组函数，把服务端内存里的解析结果（`Create_field`、`KEY`、`FOREIGN_KEY`、`partition_info`）逐字段落到 `dd::Abstract_table` / `dd::Table` 对象上。这些对象最终由数据字典子系统写入字典表。
2. **判定侧**：`sql/dd/dictionary.h` 里的 `Dictionary` 抽象类与一组 MDL 包装函数。`Dictionary::is_dd_table_name` 回答「给定的 schema + 表名组合是不是字典表」，`dd::acquire_shared_table_mdl` 负责在访问某张表之前按需加共享元数据锁。

字典表本身放在 `mysql` 库内，官方文档说明这些表默认受保护，只有 debug 构建才能直接查；正常访问路径是 `INFORMATION_SCHEMA` 表与 `SHOW` 语句。

## 怎么实现的

### 写入侧：列定义如何落到字典对象

`fill_dd_columns_from_create_fields` 的实现（`sql/dd/dd_table.cc`）有明确的几步：

**第一步，准备一个能生成默认值的假表。** 函数先按 `max_pack_length(create_fields)` 算出缓冲区大小，再取 `std::max<size_t>(2, bufsize)`——下限是 2 字节，因为取默认值时会访问记录前导里的 leftover bits，`get_rec_buf()` 假设缓冲区至少两字节。缓冲区用 `my_malloc(key_memory_DD_default_values, bufsize, MYF(MY_WME))` 分配，分配失败直接返回 true。

随后构造一个栈上的 `TABLE table` 和 `TABLE_SHARE share`，`table.s = &share`、`table.in_use = thd`，并把 `table.s->db_low_byte_first` 设为 `file->low_byte_first()`。这两个对象只是为了把 `Create_field` 变成可用的 `Field`，**不落盘、不进表缓存**。

**第二步，临时改写 THD 的截断检查级别。** 函数内部定义了一个局部类 `Context_handler`，构造时把 `m_thd->check_for_truncated_fields` 设为 `CHECK_FIELD_WARN`，析构时 `my_free(m_buf)` 并还原原值。目的是让「默认值放不下时」走告警而不是报错。

**第三步，逐列填充。** 对每个 `Create_field` 调 `tab_obj->add_column()` 拿到 `dd::Column`，然后依次写入：

- 类型：`col_obj->set_type(dd::get_new_field_type(field.sql_type))`。`get_new_field_type` 是一个穷举 `switch`，把 `enum_field_types` 逐个映射到 `dd::enum_column_types`；`MYSQL_TYPE_NULL`、`MYSQL_TYPE_INVALID`、`MYSQL_TYPE_BOOL` 三个合并映射到 `TYPE_NULL`。映射表之外的值走 `LogErr(ERROR_LEVEL, ER_DD_FAILSAFE, "field type.")` 加 `assert(false)`，返回 `dd::enum_column_types::LONG`。
- 字符长度：`col_obj->set_char_length(field.max_display_width_in_bytes())`。
- 数值 scale / precision / datetime precision：分别由 `get_field_numeric_scale`、`get_field_numeric_precision`、`get_field_datetime_precision` 计算。这三个函数的返回约定是 `false` 表示「值已确定」、`true` 表示「该类型不适用」。
  - `get_field_numeric_scale`：`MYSQL_TYPE_NEWDECIMAL` / `MYSQL_TYPE_DECIMAL` 直接取 `field.decimals`；`MYSQL_TYPE_FLOAT` / `MYSQL_TYPE_DOUBLE` 仅在 `field.decimals != DECIMAL_NOT_SPECIFIED` 时取值，否则在 `INFORMATION_SCHEMA` 中显示为 NULL；整数类型断言 `field.decimals == 0`。
  - `get_field_numeric_precision` 的取值直接写在函数体里：`MYSQL_TYPE_TINY` 为 3、`SHORT` 为 5、`INT24` 为 7、`LONG` 为 10、`LONGLONG` 有符号 19、无符号 20。注释说明这些数字取自 `Field_XXX::max_display_length()` 减 1。
  - `get_field_datetime_precision`：`DATETIME` / `DATETIME2` / `TIMESTAMP` / `TIMESTAMP2` 用 `max_display_width_in_codepoints()` 减去 `MAX_DATETIME_WIDTH` 再减 1；`TIME` / `TIME2` 同理减去 `MAX_TIME_WIDTH` 再减 1。
- 可空、无符号、zerofill：`set_nullable(field.is_nullable)`、`set_unsigned(field.is_unsigned)`、`set_zerofill(field.is_zerofill)`。
- SRS id：`col_obj->set_srs_id(field.m_srid)`。
- hidden 类型：先 `assert(field.hidden != dd::Column::enum_hidden_type::HT_HIDDEN_SE)`——存储引擎内部专用的隐藏列类型不允许出现在数据字典里。
- 默认值与更新值：`Field::DEFAULT_NOW` 对应 `set_default_option(now_with_opt_decimals(field.decimals))`，`Field::ON_UPDATE_NOW` 对应 `set_update_option(...)`。`now_with_opt_decimals` 先写 `"CURRENT_TIMESTAMP"`，`decimals > 0` 时再 `append_parenthesized(decimals)`。
- 自增：`col_obj->set_auto_increment((field.auto_flags & Field::NEXT_NUMBER) != 0)`。
- 表达式默认值：`field.m_default_val_expr` 非空时，用 `print_expr(thd, &default_val_expr)` 把 `Item*` 打印成文本存入 `default_option`。缓冲区是固定 `char buffer[128]`。
- 生成列：`field.gcol_info` 非空时，`set_virtual(!field.stored_in_db)`，并用 `print_expr` 把表达式写成文本存进 `generation_expression`，同时用 `convert_and_print` 转成 UTF-8 存进 `generation_expression_utf8`。源码注释解释了为什么要做文本归一化：`'a||b'` 在 `PIPES_AS_CONCAT` 开与关时含义不同（`a OR b` 与 `CONCAT(a,b)`），用 `Item::print()` 得到自足文本后，`sql_mode` 后续变化不会影响该列。注释同样写在固定 `char buffer[128]` 里。
- 注释：`field.comment.str && field.comment.length` 时 `set_comment(...)`。
- 排序规则 id：`col_obj->set_collation_id(field.charset->number)`，并用 `set_is_explicit_collation(field.is_explicit_collation)` 记录是否显式指定。

**第四步，写列级选项。** 通过 `dd::Properties *col_options = &col_obj->options()` 写入若干键值对：

- `MYSQL_TYPE_BIT` 列写 `"treat_bit_as_char"`，值为 `field.treat_bit_as_char`。源码注释说明理由：存储引擎开始支持优化后的 BIT 存储后，仍要正确处理此变更之前创建的列。
- `MYSQL_TYPE_GEOMETRY` 列写 `"geom_type"`，值为 `field.geom_type`（这一段在函数体内出现了两次，两处写的是同一个键）。
- `field.field_storage_type() != HA_SM_DEFAULT` 时写 `"storage"`；`field.column_format() != COLUMN_FORMAT_TYPE_DEFAULT` 时写 `"column_format"`。
- `field.flags & NOT_SECONDARY_FLAG` 时写 `"not_secondary"`；`field.is_array` 为真时写 `"is_array"`。

**第五步，写 ENUM/SET 元素。** `field.interval` 非空时遍历 `field.interval->type_names`，对每个名字调 `col_obj->add_element()` 拿到 `dd::Column_type_element`，用 `dd::String_type(*pos, field.interval->type_lengths[i])` 拷贝名字——注释说明这里连结尾的 `'\0'` 一起拷贝，是为了兼容不同字符集的类型名。元素个数记在 `col_options->set("interval_count", i)`。

**第六步，写显示类型。** `col_obj->set_column_type_utf8(get_sql_type_by_create_field(&table, field))`。`get_sql_type_by_create_field` 内部用 `make_field(field, table->s)` 造一个 `Field`，`fld->init(table)`，然后 `fld->sql_type(type)` 写到 `char tmp[MAX_FIELD_WIDTH]` 缓冲上。

**第七步，写默认值。** 每列处理前先把缓冲区清零（`memset(buf, 0, bufsize)`），再调 `prepare_default_value(thd, buf, &table, field, col_obj)`，该函数返回 true 时整个函数返回 true。之后调 `prepare_default_value_string(buf, &table, field, col_obj, &def_val)` 得到字符串形式的默认值，存在 `def_val.ptr() != nullptr` 时写入 `col_obj->set_default_value_utf8(...)`。

`prepare_default_value_string` 里几处细节：

- 若 `col_obj->has_no_default()` 则给 `Field` 打上 `NO_DEFAULT_VALUE_FLAG`。
- 判定「有默认值」的条件是：类型不是 BLOB **或** 该 BLOB 有插入默认表达式表达式（`has_insert_default_general_value_expression()`），且没有 `NO_DEFAULT_VALUE_FLAG`，且 `auto_flags` 不含 `Field::NEXT_NUMBER`。
- 生成列（有 `gcol_info`）直接返回，不写默认值。
- `DEFAULT (表达式)` 走 `convert_and_print(&default_val_expr, def_value, system_charset_info)` 转 UTF-8。
- `DEFAULT NOW()` 写成字面量 `"CURRENT_TIMESTAMP"`，`f->decimals() > 0` 时追加括号参数。
- `MYSQL_TYPE_BIT` 的默认值被包成 `b'...'` 形式：先写 `'b'`、再写 `'\''`，然后用 `longlong2str(dec, tmp + 2, 2)` 写数值。
- BINARY / VARBINARY 的默认值统一转成十六进制字符串：`bin_to_hex_str(ptr, type.length() - 2, type2.c_ptr_safe(), strlen(type2.c_ptr_safe()))`，前缀 `0x`。源码注释说明这是因为 hex 串里可能有无法转成系统字符集的字节值，且用户原始写法在解析阶段已丢失，所以 I_S 与 `SHOW COLUMNS` 一律以 HEX 形式列出。

### 写入侧：索引与表级信息

同一文件里还有配套的索引/表级填充函数，它们和列填充在同一条 DDL 路径上：

- `fill_dd_index_elements_from_key_parts`：按 `KEY_PART_INFO` 数组逐列建立 `dd::Index_element`。列的 `column_key` 标记（`CK_PRIMARY` / `CK_UNIQUE` / `CK_MULTIPLE`）在这里决定；索引类型由 `dd_get_new_index_type` 判定——先看 `HA_FULLTEXT`，再看 `HA_SPATIAL`，然后 `HA_NOSAME` 时靠 `key->name == primary_key_name` 区分主键与唯一键，否则为 `IT_MULTIPLE`。
- `fill_dd_indexes_from_keyinfo`：为每个 `KEY` 建 `dd::Index`，写 `algorithm`、`visible`、`type`、`generated`、`engine` 等字段。索引选项里只存 `HA_PACK_KEY` 和 `HA_BINARY_PACK_KEY` 两个 flag——源码注释解释了为什么单独存这两个：其余 `KEY::flags` 位都能从 Index / Index_element / Column 的其他属性算出来，而这两个 flag 反映的是「是否施加某种优化」这一决策，不是键或键部分的固有属性，存下来是为了避免将来改动优化条件时出现二进制兼容问题。
- `is_candidate_primary_key`：判断某个唯一键能否被提升为主键。条件是必须带 `HA_NOSAME` 且不带 `HA_NULL_PART_KEY`，且不带 `HA_VIRTUAL_GEN_KEY`；然后对每个 key part 造 `Field` 调 `is_suitable_for_primary_key(key_part, table_field.get())`。
- `fill_dd_tablespace_id_or_name`：表空间元数据的落库分两路。`hton->alter_tablespace` 为真、不是临时表、且表空间名不以 `"innodb_file_per_table"` 开头时，走 `dd::cache::Dictionary_client::Auto_releaser` + `thd->dd_client()->acquire(tablespace_name, &ts_obj)` 查对象，拿到后存 `obj->set_tablespace_id(ts_obj->id())`；查不到则 `my_error(ER_TABLESPACE_MISSING_WITH_NAME, MYF(0), tablespace_name)` 返回 true。否则把表空间名作为字符串写进 `options->set("tablespace", tablespace_name)`。函数最后统一 `options->set("explicit_tablespace", true)`，注释说明这是为了让 `SHOW CREATE` 和 `CREATE LIKE` 忽略隐含的表空间（如 `innodb_system`）。该分支还 `assert` 当前线程已持有该表空间名的 `MDL_INTENTION_EXCLUSIVE` 及以上锁。
- `get_se_private_data`：从 `DD_properties` 里取出 `ID`、`SPACE_ID`、`DATA` 三项，分别写入 `tab_obj->set_se_private_id(se_id)`、`set_tablespace_id(space_id)`、`set_se_private_data(se_data)`；索引的私有数据用 `IDX` 加计数的键名逐个取出。取不到时 `my_error(ER_DD_METADATA_NOT_FOUND, MYF(0), tab_obj->name().c_str())`。

### 读取侧：字典表判定与元数据锁

`sql/dd/dictionary.h` 定义了 `dd::Dictionary` 抽象类，其中与本页直接相关的三个判定原语：

- `is_dd_table_name(schema_name, table_name)`：判断给定的 schema + 表名组合是否为字典表。`dd::get_dictionary()` 返回全局字典实例，调用方遍布 `sql/sql_table.cc`、`sql/handler.cc`、`sql/table.cc`、`sql/sql_insert.cc`。
- `is_dd_schema_name(schema_name)`：判断 schema 名是否为 `'mysql'`（字典表所在库）。
- `is_dd_table_access_allowed(is_dd_internal_thread, is_ddl_statement, schema_name, schema_length, table_name)`：判断给定线程类型能否访问该表。

这组判定被用在真实分支上。例如 `sql/handler.cc` 的建表后置更新里，只有在「不是临时表、不是 DD 表自身、且引擎支持 `HTON_SUPPORTS_ATOMIC_DDL`」时才调 `thd->dd_client()->update<dd::Table>(table_def)`——注释说明 DD 表本身在引擎里创建出来之前不能被写进 DD。`sql/table.cc` 的表类别判定里，`is_dd_table_name(MYSQL_SCHEMA_NAME.str, name.str)` 成立时返回 `TABLE_CATEGORY_DICTIONARY`。

`dd::acquire_shared_table_mdl` 是「按需加共享元数据锁」的入口，声明在 `dictionary.h`：

```
[[nodiscard]] bool acquire_shared_table_mdl(THD *thd, const char *schema_name,
                                            const char *table_name,
                                            bool no_wait,
                                            MDL_ticket **out_mdl_ticket);
```

语义由头文件注释给出：`no_wait` 为真时用 `try_acquire_lock()`，否则用 `acquire_lock()` 并采用 `thd->variables.lock_wait_timeout` 超时值。同一头文件里还有配套的 `has_shared_table_mdl` / `has_exclusive_table_mdl` 谓词，以及表空间版本 `acquire_shared_tablespace_mdl` / `acquire_exclusive_tablespace_mdl`（这两个带 `for_trx` 参数，`true` 表示 MDL duration 为 `MDL_TRANSACTION`，`false` 为 `MDL_EXPLICIT`）。

真实调用方之一在 `storage/innobase/dict/dict0dict.cc` 的 `dd_sdi_acquire_shared_mdl`：它拼出 `"SDI_" SPACE_ID_PF` 形式的表名（schema 固定为 `"dummy_sdi_db"`），然后调 `dd::acquire_shared_table_mdl(thd, db_buf, tbl_buf, false, sdi_mdl)`；返回非零、或 `*sdi_mdl == nullptr` 时，函数返回 `DB_LOCK_WAIT_TIMEOUT`。

### 读取侧：DD 客户端的获取与释放

`sql/dd/dd_table.cc` 里的一致性模式是 `dd::cache::Dictionary_client::Auto_releaser` + `client->acquire(...)`：

- `table_exists`：`Auto_releaser releaser(client)` 之后 `client->acquire(schema_name, name, &tab_obj)`，`acquire` 失败时错误已由字典子系统上报，函数返回 true；成功时 `*exists = (tab_obj != nullptr)`。
- `drop_table`：`thd->dd_client()->drop(&table_def)` 与 `thd->dd_client()->remove_table_dynamic_statistics(schema_name, name)` 用 `||` 串联。

## 关键取舍

**一，字典表和普通系统表分开。** 官方文档的表述是：字典表存放执行 SQL 查询所需的元数据，系统表存放时区、帮助信息这类辅助数据；两者的升级路径也不同——字典表由服务器自己管理升级，系统表升级要跑完整的 mysql_upgrade 流程。

**二，DD 表默认受保护。** 字典表默认不可直接访问，只有用 `-DWITH_DEBUG=1` 编译的 debug 版本，配合 `+d,skip_dd_table_access_check` 这个 debug 选项才能看到。官方文档明确警告：直接改写字典表可能导致实例不可用。

**三，用文本归一化而不是保存表达式树。** 生成列与表达式默认值都以文本形式落库。好处是后续 `sql_mode` 变化不会改变已存列的行为；代价是文本长度受固定缓冲限制——`fill_dd_columns_from_create_fields` 里生成列和默认值表达式都用了 `char buffer[128]`。

**四，尽量不存可推导的信息。** 索引 flag 只存 `HA_PACK_KEY` 与 `HA_BINARY_PACK_KEY` 两个，其余从其他属性推导。列的 `numeric_scale` 对整数类型统一写 0（源码注释说是为了简化 I_S 实现），而 FLOAT/DOUBLE 在用户没指定 `decimals` 时保持 NULL（`assert(col_obj->is_numeric_scale_null())`）。

**五，默认值区分「二进制形式」和「展示形式」。** `columns.default_values` 存二进制默认值，`columns.default_value_utf8` 存字符串形式，专供 I_S 使用；BINARY/VARBINARY 的默认值一律以 HEX 形式存放，代价是用户原始写法丢失。

**六，索引顺序交给存储引擎的 `index_flags()` 回答。** `fill_dd_indexes_from_keyinfo` 通过 `file->index_flags(...)` 拿 `HA_READ_ORDER` 来定 `Index_element` 的顺序类型。当 `file->get_table_share()` 返回空时，代码构造一个 `dummy_table_share` 顶上，并在结束时把 `file->change_table_ptr(nullptr, nullptr)` 复位。源码注释也记了这条路径的已知不完美：这里准备的 `keyinfo` 与表打开时准备的并不完全相同（`actual_flags`、`unused_key_parts`、`usable_key_parts`、`rec_per_key` 等成员可能不同），注释判断 `index_flags()` 的实现主要用 algorithm 与 flags 成员，所以能用。注释同时记录了备选方案（新增一个 handler API）和 TODO 标记（WL6599 评审期间由 dlenev 细化）。

**七，锁与元数据的顺序有断言保护。** `fill_dd_tablespace_id_or_name` 在走表空间 ID 分支前 `assert(thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::TABLESPACE, "", tablespace_name, MDL_INTENTION_EXCLUSIVE))`，把「先拿锁再读字典」的顺序固化到代码里。

**架构推论：** 从 `fill_dd_columns_from_create_fields` 造临时 `TABLE`/`TABLE_SHARE`、只为拿到 `Field` 的做法可以看出，DD 填充路径刻意不依赖表缓存——它要能在建表过程中就完成字典行的构造，而那时表还没有打开。同理，`fill_dd_tablespace_id_or_name` 通过 `thd->dd_client()->acquire()` 直接向字典客户端要对象，而不是走表缓存路径。

**架构推论：** `is_dd_table_name` 在 `sql/handler.cc`、`sql/sql_table.cc`、`sql/table.cc`、`sql/sql_insert.cc` 四处被反复调用，说明「这张表是不是字典表」是 DDL/DML 各路径都要先回答的前置判定；把判定做成 `Dictionary` 接口的一个方法而不是散落的字符串比较，是为了让这套判定在所有调用点保持一致。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/dd/dd_table.cc | fill_dd_columns_from_create_fields | 把 `Create_field` 列表翻译成 `dd::Column` 对象：类型、长度、scale/precision、默认值、生成列表达式、注释、排序规则 id、列级选项与 ENUM/SET 元素 |
| sql/dd/dd_table.cc | get_new_field_type | `enum_field_types` 到 `dd::enum_column_types` 的穷举映射，未覆盖值报 `ER_DD_FAILSAFE` |
| sql/dd/dd_table.cc | get_field_numeric_scale | 计算列的数值 scale，区分「适用且已确定」与「不适用」 |
| sql/dd/dd_table.cc | get_field_numeric_precision | 计算列的数值精度，函数体内写死 TINY/SHORT/INT24/LONG/LONGLONG 的取值 |
| sql/dd/dd_table.cc | get_field_datetime_precision | 计算日期时间列的小数秒精度，基于 `MAX_DATETIME_WIDTH` 与 `MAX_TIME_WIDTH` |
| sql/dd/dd_table.cc | prepare_default_value_string | 生成默认值的字符串形式，含 BIT 的 `b'...'` 包装与 BINARY/VARBINARY 的 HEX 化 |
| sql/dd/dd_table.cc | get_sql_type_by_create_field | 用临时 `Field` 调 `sql_type()` 取出列的展示类型 |
| sql/dd/dd_table.cc | fill_dd_index_elements_from_key_parts | 按 `KEY_PART_INFO` 建立 `dd::Index_element`，并决定列的 `column_key` 标记 |
| sql/dd/dd_table.cc | fill_dd_indexes_from_keyinfo | 按 `KEY` 数组建立 `dd::Index`，只把 `HA_PACK_KEY`/`HA_BINARY_PACK_KEY` 存进索引选项 |
| sql/dd/dd_table.cc | is_candidate_primary_key | 判断唯一键能否被提升为主键，逐 key part 调 `is_suitable_for_primary_key` |
| sql/dd/dd_table.cc | fill_dd_tablespace_id_or_name | 表空间元数据落库：可 alter 的引擎存 tablespace id，其余把表空间名写进 options |
| sql/dd/dd_table.cc | get_se_private_data | 从 `DD_properties` 取 `ID`/`SPACE_ID`/`DATA` 写入表与索引的 SE 私有数据 |
| sql/dd/dd_table.cc | table_exists | 通过 `Dictionary_client::acquire` 判断表是否存在 |
| sql/dd/dd_table.cc | drop_table | 删除字典表对象并清理动态统计信息 |
| sql/dd/dd_table.cc | invalid_tablespace_usage | 判定表是否非法地使用了保留表空间，命中 `ER_RESERVED_TABLESPACE_NAME` |
| sql/dd/dd_table.cc | engine_supports_provided_srs_id | 引擎不支持地理坐标系列时，拒绝非零且非投影的 SRS id |
| sql/dd/dictionary.h | is_dd_table_name | 判定给定的 schema + 表名组合是否为数据字典表 |
| sql/dd/dictionary.h | acquire_shared_table_mdl | 按需获取表的共享元数据锁，`no_wait` 为真走 `try_acquire_lock()`，否则按 `lock_wait_timeout` 等待 |
| sql/dd/dictionary.h | is_dd_table_access_allowed | 判定给定线程类型（DD 内部线程 / DDL 语句）能否访问该表 |
| sql/dd/dictionary.h | is_dd_schema_name | 判定 schema 名是否为字典表所在的 `'mysql'` 库 |
| sql/dd/dictionary.h | acquire_shared_tablespace_mdl | 表空间版本的共享锁入口，`for_trx` 决定 MDL duration 是 `MDL_TRANSACTION` 还是 `MDL_EXPLICIT` |
| storage/innobase/dict/dict0dict.cc | dd_sdi_acquire_shared_mdl | InnoDB 获取 SDI 相关共享元数据锁的真实调用方，失败返回 `DB_LOCK_WAIT_TIMEOUT` |
| sql/handler.cc | create_table | 建表后仅在非临时表、非 DD 表自身且引擎支持 `HTON_SUPPORTS_ATOMIC_DDL` 时更新 `dd::Table` |
| sql/table.cc | get_table_category | 依据 `is_dd_table_name` 把字典表归类为 `TABLE_CATEGORY_DICTIONARY` |

## 与主核 / AI 的关系

数据字典与本仓主核（`engine/db/`）在问题域上高度对应，但形态不同，值得对照着看：

- **主核的 `catalog/` 层是「一份可改的内存目录 + 写日志」**；MySQL 的字典是「一堆可被 SQL 查询的字典表 + 事务提交」。主核要自己实现崩溃恢复与并发可见性，MySQL 直接把这件事交给 InnoDB 的事务与回滚日志，因此字典表能被 DDL 原子地提交或回滚。
- **元数据锁的定位不同。** MySQL 里字典的读路径必须先拿 MDL（`acquire_shared_table_mdl`），主核的目录层没有独立的锁层。**架构推论：** 主核若要在 DDL 与 DML 之间建立互斥，需要自己引入一层等价物。
- **主核是「原生多模态」内核**，多模态的元数据（embedding 维度、向量索引参数）没有对应物。**架构推论：** 若主核要支持多模态的元数据持久化与并发 DDL，可参考 MySQL 字典的两条经验：一是用「事务化存储 + 统一缓存」替代分散的文件式元数据，二是把「是不是系统对象」的判定做成单一接口并在所有路径上强制调用（`is_dd_table_name`）。
- **对 AI 编码/检索的价值：** 字典的写入侧（`fill_dd_columns_from_create_fields`）是一份「DDL 语义 → 存储结构」的可执行规格。它把类型映射、数值精度、默认值归一化、生成列文本化这些规则集中在一处，是理解 MySQL 元数据语义最省事的入口。

## 参考

- [Chapter 16 MySQL Data Dictionary（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/data-dictionary.html)
- [16.1 Data Dictionary Schema（字典表清单与访问限制、字典表升级机制）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/data-dictionary-schema.html)
- [16.4 Dictionary Object Cache（字典对象缓存分区与容量上限）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/data-dictionary-object-cache.html)
- [16.6 Serialized Dictionary Information (SDI)（字典信息的序列化副本）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/serialized-dictionary-information.html)
- [mysql/mysql-server 源码：sql/dd/dd_table.cc（8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/dd/dd_table.cc)
- [mysql/mysql-server 源码：sql/dd/dictionary.h（8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/dd/dictionary.h)
