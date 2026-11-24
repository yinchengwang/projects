---
project: mysql-server
feature: limit-clause
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_resolver.cc
    symbols: [Query_block::resolve_limits]
  - path: sql/iterators/composite_iterators.h
    symbols: [LimitOffsetIterator]
  - path: sql/sql_lex.cc
    symbols: [Query_block::get_limit, Query_block::print_limit]
---

# LIMIT 与行数截断

## 一句话职责

LIMIT 与 OFFSET 子句的语义实现与提前终止：解析期把 LIMIT/OFFSET 表达式定型成无符号整数、判定半连接是否可用、算出「无限制」哨兵；优化期把限制翻译成执行计划上的 `LIMIT_OFFSET` 访问路径；执行期由 `LimitOffsetIterator` 在行流上跳过 OFFSET 行、在达到 LIMIT 行数后立刻停止取行并向上返回 EOF。

## 机制是什么

LIMIT 在 MySQL 里不是「执行完再截断」，而是**贯穿三个阶段的同一个约束**：

1. **解析与定型阶段**：`LIMIT`/`OFFSET` 后面跟的值先被语法分析接受为标识符（可含 `:` 前缀的变量名）、参数标记（prepared statement 的 `?`）或整数字面量三类，见 `sql/sql_yacc.yy` 的 `limit_option` 产生式。这三者最终都变成 `Item`（`Item_uint` 或 `Item_param`），挂在 `Query_block` 的 `select_limit`（LIMIT 值）与 `offset_limit`（OFFSET 值）两个成员上。
2. **语义定型阶段**：`Query_block::resolve_limits()`（`sql/sql_resolver.cc`）负责把这两个 `Item` 从「未定型的表达式」变成「可求值的无符号整数表达式」——`fix_fields` 解析名字，再在类型还是 `MYSQL_TYPE_INVALID` 时按 `Type_properties(MYSQL_TYPE_LONGLONG, true)` 传播类型并 `pin_data_type()` 钉死。
3. **执行阶段**：真正「截断」动作由 `LimitOffsetIterator`（`sql/iterators/composite_iterators.h`）完成，它挂在执行树的某一层，按 `Init()` / `Read()` 两步协议消费上游行流。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 LIMIT/OFFSET 的语义实现与提前终止；**不负责排序**（那是 E02 文件排序的职责）、**不负责分页游标的稳定性**（跨查询保持游标位置不属于服务端职责）。

**「无限制」用哨兵值表达，而不是用「有没有 LIMIT」这个布尔量表达。** `Query_block::get_limit()`（`sql/sql_lex.cc`）在没有任何限制时返回 `ha_rows{HA_POS_ERROR}`。`HA_POS_ERROR` 是 MySQL 表示「无上限 / 无此行」的既有常量；把它当哨兵用，意味着同一个 `ha_rows` 字段既承载「LIMIT 10」也承载「不限制」，下游不必再做一次「是否有上限」的分支。

**LIMIT 的来源不止一处。** `Query_block::get_limit()` 的函数体给出三条互斥的取值路径，顺序是：

1. `select_limit != nullptr` —— 显式写了 LIMIT，返回 `select_limit->val_uint()`；
2. 否则若 `m_use_select_limit` 为真 —— 返回 `thd->variables.select_limit`（会话变量），函数体注释写明这条路径「用于为 SELECT 语句实现 SQL_SELECT_LIMIT」；
3. 否则 —— 返回 `ha_rows{HA_POS_ERROR}`。

`m_use_select_limit` 的置位点在 `sql/sql_select.cc`：`if (!parameters->has_limit()) { parameters->m_use_select_limit = true; }`——**只有当语句本身没写 LIMIT 时，会话级 `select_limit` 变量才生效**。

**OFFSET 的取值路径更短。** `Query_block::get_offset()` 的函数体只有两条：非空时返回 `offset_limit->val_uint()`，否则返回 `ha_rows{0}`。

**LIMIT 值会被折算成「总共要读多少行」。** `Query_expression::set_limit()`（`sql/sql_lex.cc`）先取 `offset_limit_cnt = provider->get_offset(thd)` 与 `select_limit_cnt = provider->get_limit(thd)`，然后做一次带溢出保护的加法：若 `select_limit_cnt + offset_limit_cnt >= select_limit_cnt`（即没溢出）就 `select_limit_cnt += offset_limit_cnt`，否则置 `select_limit_cnt = HA_POS_ERROR`（视为无限制）。所以下游看到的 `select_limit_cnt` 是 **LIMIT + OFFSET** 的总和，而不是裸 LIMIT 值。

**LIMIT 会影响优化决策，不只是执行期截断。** `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 里：`m_select_limit = query_expression()->select_limit_cnt` 被注释标注为「用来判断我们是否可能需要扫全表」；而 `if (having_cond || calc_found_rows) m_select_limit = HA_POS_ERROR;` 说明**一旦带 HAVING 或需要算 found rows，LIMIT 就不再被当成「可能提前停」的信号**，因为这两种情形必须把输入读全。

**`LIMIT 0` 走的是「零行计划」而不是迭代器截断。** 同一函数里 `if (query_expression()->select_limit_cnt == 0 && !calc_found_rows) { zero_result_cause = "Zero limit"; best_rowcount = 0; create_access_paths_for_zero_rows(); goto setup_subq_exit; }`——直接生成零行访问路径，根本不会建立 `LimitOffsetIterator`。注意 `!calc_found_rows` 这个条件：`SQL_CALC_FOUND_ROWS` 与 `LIMIT 0` 同时出现时不能走这条路，因为仍需统计总行数。

**LIMIT 与半连接的相容性有明确判据。** `Query_block::is_row_count_valid_for_semi_join()`（`sql/sql_resolver.cc`）的函数体给出两条否决条件：OFFSET 存在且（不是常量 或 值不等于 0）则否决；LIMIT 存在且（不是常量 或 值等于 0）则否决。函数头注释写明「半连接只有在 OFFSET 为 0 且 select LIMIT 不为 0 时才可用」。

## 怎么实现的

### 语法侧：`limit_option` 的三类取值

`sql/sql_yacc.yy` 的 `limit_option` 产生式有四条分支：`ident`（含变量名，构造 `PTI_limit_option_ident`）、`param_marker`（构造 `PTI_limit_option_param_marker`）、`ULONGLONG_NUM`、`LONG_NUM`、`NUM`（后三者都构造 `Item_uint`）。

`limit_options` 产生式覆盖三种书写形式：`limit_option`（只有 LIMIT）、`limit_option ',' limit_option`（逗号形式，前者是 OFFSET，后者是 LIMIT，`is_offset_first = true`）、`limit_option OFFSET_SYM limit_option`（`LIMIT x OFFSET y` 形式）。`opt_simple_limit` 则只允许 `LIMIT limit_option` 一种（不带 OFFSET）。

### 上下文化：`PT_limit_clause::contextualize`

`sql/parse_tree_nodes.cc` 的 `PT_limit_clause::contextualize()` 函数体决定 itemize 的**先后顺序**：先在 `is_offset_first` 为真且 `opt_offset != nullptr` 时 itemize OFFSET；再 itemize LIMIT；最后在 `!is_offset_first` 且 `opt_offset != nullptr` 时 itemize OFFSET。然后把结果写回 query block：

```
pc->select->select_limit = limit_options.limit;
pc->select->offset_limit = limit_options.opt_offset;
pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
```

注意第三句：**带 LIMIT 的语句会被标记为 binlog unsafe**（`LEX::BINLOG_STMT_UNSAFE_LIMIT`），因为语句安全模式下带 LIMIT 的语句不能安全地在基于语句的复制中重放。

### 定型：`Query_block::resolve_limits`

`sql/sql_resolver.cc` 的 `Query_block::resolve_limits()` 函数体是一个完全对称的两段式结构——OFFSET 一段、LIMIT 一段，每段三步：

1. `fix_fields(thd, nullptr)`，失败返回真（错误）；
2. 若 `data_type() == MYSQL_TYPE_INVALID`，则 `propagate_type(thd, Type_properties(MYSQL_TYPE_LONGLONG, true))`，失败返回真；
3. `pin_data_type()`。

第二步的 `Type_properties(MYSQL_TYPE_LONGLONG, true)` 里第二个参数是 unsigned 标志——**LIMIT/OFFSET 的值域被定为 64 位无符号整数**。第三步 `pin_data_type()` 把类型钉死，避免后续优化阶段再改写。

函数头注释交代了它处理的语法变体：「OFFSET 和 LIMIT 子句可以挂在构成一个查询表达式的各个 query block 上。作用于整个查询表达式的 OFFSET 与 LIMIT 子句挂在 `fake_query_block` 上，因此我们可以用这个接口来解析它们。OFFSET 与 LIMIT 可以是无符号整数字面量值，也可以是参数。如果是参数，确保其类型是无符号整数。」

`resolve_limits()` 在 `sql/sql_resolver.cc` 里有两处调用点，都在解析流程内（同一文件的 `resolve_placeholder_tables` 一类的上层函数中）。

### 取值：`Query_block::get_limit` 与 `get_offset`

`sql/sql_lex.cc` 的 `Query_block::get_limit(const THD *thd)` 函数体就是前面列出的三条互斥路径。注意它**不是** `get_offset` 的对称形态——`get_offset(const THD *)` 只有两条路径（`offset_limit` 非空则 `val_uint()`，否则 `0`），且参数 `thd` 未被使用。

`get_limit` 的第二条路径会读 `thd->variables.select_limit`。这个会话变量在编译存储过程时被临时改写：`sql/sp.cc` 的 `sp_compile()` 函数体在解析前 `thd->variables.select_limit = HA_POS_ERROR;`，解析后 `thd->variables.select_limit = old_select_limit;` 恢复——**存储过程定义体里不继承调用方的 `select_limit`**。`sql/sql_call.cc` 的 `CALL` 执行路径也做同样的临时改写（`thd->variables.select_limit = HA_POS_ERROR;`）。

### 计划侧：`LIMIT_OFFSET` 访问路径

计划侧的载体是 `AccessPath::LIMIT_OFFSET`。构造函数 `NewLimitOffsetAccessPath()`（`sql/join_optimizer/access_path.h`）接受六个参数：`thd`、子访问路径 `child`、`limit`、`offset`、`count_all_rows`、`reject_multiple_rows`、`send_records_override`。函数体做的事：新建 `AccessPath`、置 `type = AccessPath::LIMIT_OFFSET`、继承 `child->immediate_update_delete_table`、把五个参数写进 `path->limit_offset()`、把 `path->ordering_state = child->ordering_state`，最后调 `EstimateLimitOffsetCost(path)`。

**`send_records_override` 的含义在 `sql/join_optimizer/access_path.h` 的成员注释里写明：「见 `Query_expression::send_records`。」** 它的用途是指定「被 OFFSET/LIMIT 跳过的行数往哪里累计」。`sql/join_optimizer/access_path.cc` 的 `CreateIteratorFromAccessPath()` 在 `case AccessPath::LIMIT_OFFSET:` 分支里的取值顺序是：若 `param.send_records_override != nullptr` 就用它；否则若 `join != nullptr` 就用 `&join->send_records`；否则保持 `nullptr`。然后：

```
iterator = NewIterator<LimitOffsetIterator>(
    thd, mem_root, std::move(job.children[0]), param.limit,
    param.offset, param.count_all_rows, param.reject_multiple_rows,
    send_records);
```

### 计划侧：谁在什么条件下挂 `LIMIT_OFFSET` 路径

`sql/join_optimizer/join_optimizer.cc`（超图优化器）里至少四处调用 `NewLimitOffsetAccessPath()`，条件各不相同：

- **`ApplyDistinctAndOrder()` 路径的 ORDER BY 分支**：先建 `AccessPath::SORT` 路径，其 `sort().limit` 字段被赋为 `push_limit_to_filesort ? limit_rows : HA_POS_ERROR`——即**能下推给 filesort 时 LIMIT 由排序节点自己执行**。随后源码注释写明：「如果我们有不能下推给 filesort 的 LIMIT 子句，或者有 OFFSET 子句，就需要在 SORT 节点之上加一个 LIMIT_OFFSET 路径。」判断条件是 `(limit_rows != HA_POS_ERROR && !push_limit_to_filesort) || offset_rows != 0`。
- **DISTINCT 且分组全为常量时**：`NewLimitOffsetAccessPath(thd, root_path, /*limit=*/1, /*offset=*/0, ...)`——分组列全是常量时只需一行，用 LIMIT 1 表达。
- **无 ORDER BY 的通用路径**：注释写明「应用 LIMIT 与 OFFSET，如果适用。如果 query block 是有序的，它们已经由 `ApplyDistinctAndOrder()` 应用了。」判断条件是 `join->order.order == nullptr && (query_expression->select_limit_cnt != HA_POS_ERROR || query_expression->offset_limit_cnt != 0)`。
- **`join_optimizer.cc` 的 `LIMIT 1` 去重路径**：在 `join_optimizer.cc` 的 DISTINCT 分支里 `NewRemoveDuplicatesAccessPath` 的兄弟位置也用 `NewLimitOffsetAccessPath(thd, path, /*limit=*/1, /*offset=*/0, ...)` 做去重降级。

传统优化器一侧，`sql/sql_executor.cc` 的 `attach_access_paths_for_having_and_limit()` 在末尾做同样的事，注释写明：「对于 select_count，`LIMIT 0` 的常见情形在 `JOIN::optimize()` 里处理了，但 `CALC_FOUND_ROWS` 不处理。OFFSET 在那里也没处理。」因此这里的判断条件是 `query_expression()->select_limit_cnt != HA_POS_ERROR || query_expression()->offset_limit_cnt != 0`，并传入 `calc_found_rows` 作为 `count_all_rows`。

`sql/sql_union.cc` 的集合运算侧另有一处，条件是 `streaming_allowed && (limit != HA_POS_ERROR || offset != 0) && (is_simple() || set_operation()->m_last_distinct == 0)`，并且这里**显式传了 `&send_records`**（其余调用点多数传 `send_records_override = nullptr`）。

### 执行侧：`LimitOffsetIterator`

`sql/iterators/composite_iterators.h` 的 `LimitOffsetIterator` 类注释只有两句：「处理 LIMIT 和/或 OFFSET；`Init()` 吃掉前 `offset` 行，`Read()` 在看到 `limit` 行后停止（包括被 offset 跳过的那些行）。」

构造函数的注释把七个参数写得很清楚，其中三个值得单独记：

- `limit`：「要读的最大行数，**包括被 offset 跳过的那些**。无限制时可以是 `HA_POS_ERROR`。」这解释了为什么 `LimitOffsetIterator` 的 `m_limit` 语义上装的是「LIMIT + OFFSET」的总和，与 `Query_expression::set_limit()` 的折算是自洽的。
- `count_all_rows`：「若为真，查询会跑到底以获得更准确的 skipped_rows 数字，因此你将得不到任何提前结束的性能收益。」
- `skipped_rows`：「若非空，每有一行被 offset 或 limit 跳过就递增它。」

构造函数体里有一条断言：`if (count_all_rows) { assert(m_skipped_rows != nullptr); }`。

私有成员里有一条设计说明：「注意：如果我们有 OFFSET，已见行数从 `m_limit` 开始计，这意味着 `Read()` 的快路径上不需要分别做 LIMIT 和 OFFSET 两个测试。」——这是把两个判断合并成一个的关键设计。

**`Init()`（`sql/iterators/composite_iterators.cc`）** 的函数体只有四步：`m_source->Init()` 失败就返回真；`m_offset > 0` 时置 `m_seen_rows = m_limit`、`m_needs_offset = true`；否则置 `m_seen_rows = 0`、`m_needs_offset = false`。

**`Read()`（`sql/iterators/composite_iterators.cc`）** 的执行顺序是：

1. 若 `m_seen_rows >= m_limit`，先判断是「撞到 LIMIT」还是「还需要跳 OFFSET」。若 `m_needs_offset` 为真，进入 OFFSET 跳过循环：从 `row_idx = 0` 到 `row_idx < m_offset`，每次调 `m_source->Read()`，非零就返回该错误码；成功后若 `m_skipped_rows != nullptr` 则 `++*m_skipped_rows`，并调 `m_source->UnlockRow()`。循环结束后置 `m_seen_rows = m_offset`、`m_needs_offset = false`，然后**继续往下走 LIMIT 判定**（源码注释写的是 "Fall through to LIMIT testing"）。
2. 再次判定 `m_seen_rows >= m_limit`——成立则是真的撞到 LIMIT（或是 OFFSET 刚结束就撞到 LIMIT），此时若 `m_count_all_rows` 为真，就 `while (m_source->Read() == 0) { ++*m_skipped_rows; }` 把剩下的行数完（注释说明「数到结尾或错误（若有错误则忽略）」），然后返回 `-1`（EOF）。
3. 正常路径：调 `m_source->Read()` 拿到 `result`。若 `m_reject_multiple_rows` 为真，则 `result != 0` 时 `++m_seen_rows` 并直接返回 `result`；读到行时检查 `m_seen_rows - m_offset > 0`，成立则调 `my_error(ER_SUBQUERY_NO_1_ROW, MYF(0))` 并返回 `1`。
4. 最后 `++m_seen_rows; return result;`。

**为什么 OFFSET 跳过放在 `Read()` 而不是 `Init()`？** 函数体里的注释给了两条理由：一是「performance schema 批量模式可能还没被 executor 在第一次 `Read()` 前设置好」，这样能保证 **(a)** 即使读的是 OFFSET 行也能拿到批量模式的性能收益，**(b)** 不会在 `Init()` 期间意外开启批量模式（例如通过 `NestedLoopIterator` 开启），因为 executor 若在第一次 `Read()` 前出错就还没准备好**关闭**它。

### 计划成本与行数估计

`sql/sql_optimizer.cc` 的 `EstimateRowAccesses()`（估算访问行数的地方）在 `case AccessPath::LIMIT_OFFSET:` 分支里做的事是：`rows += EstimateRowAccesses(param.child, num_evaluations, min(limit, static_cast<double>(param.limit)));` 然后 `return true`——**LIMIT 节点把「还允许读多少行」这个预算传给子树，子树的估算被这个预算夹住**。相邻的 `case AccessPath::AGGREGATE:` 分支注释则写明相反的取向：「假设聚合需要读完整个输入，不管 limit 如何。这对显式分组的查询可能过于悲观，但目前我们保守处理。」

`sql/join_optimizer/access_path.h` 的 `NewLimitOffsetAccessPath()` 在构造路径后调 `EstimateLimitOffsetCost(path)`（函数声明在 `access_path.h` 的 inline 函数体内）。

### `reject_multiple_rows`：标量子查询的基数检查

这个开关的来源链条可以完整追出来：

1. `sql/sql_resolver.cc` 的 `Query_block::transform_subquery_to_derived()` 有一个 `reject_multiple_rows` 参数，注释写明：「对于需要运行时基数检查的标量子查询：为真，否则为假。」函数体把它写进 `subs_query_expression->m_reject_multiple_rows = reject_multiple_rows;`。
2. 该函数的两个调用点：`sql/sql_resolver.cc` 里 `transform_subquery_to_derived(thd, &tl, subs_query_expression, subq, /*use_inner_join=*/false, needs_cardinality_check, subquery.m_join_condition, lifted_where)`——**这里传的就是 `needs_cardinality_check`**；另一处（半连接转派生表）显式传 `/*reject_multiple_rows*/ false`。
3. `sql/sql_executor.cc` 的派生表物化路径把它传给访问路径：`NewMaterializeAccessPath(..., query_expression->m_reject_multiple_rows)`；在 OFFSET 非零的分支里则是 `NewLimitOffsetAccessPath(thd, path, query_expression->select_limit_cnt, query_expression->offset_limit_cnt, /*count_all_rows=*/false, query_expression->m_reject_multiple_rows, /*send_records_override=*/nullptr)`——注释写明「LIMIT 由 MaterializeIterator 内部处理，但 OFFSET 不是。派生表的定义里不可能出现 `SQL_CALC_FOUND_ROWS`。」
4. 最终在 `sql/iterators/composite_iterators.cc` 的 `LimitOffsetIterator::Read()` 里生效：读到行后检查 `m_seen_rows - m_offset > 0`，成立则报 `ER_SUBQUERY_NO_1_ROW`。

物化侧还有一条平行的检查：`sql/iterators/composite_iterators.cc` 的 `MaterializeIterator::Init()` 在 `table()->is_union_or_table()` 分支里，`if (m_reject_multiple_rows && stored_rows > 1) { my_error(ER_SUBQUERY_NO_1_ROW, MYF(0)); return true; }`——**INTERSECT / EXCEPT 的结果行数多于一行时报同一个错**。

### `count_all_rows` 与 `SQL_CALC_FOUND_ROWS`

`calc_found_rows` 的判定在 `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 里：`calc_found_rows = m_select_limit != HA_POS_ERROR && (!query_expression()->is_set_operation() || query_block == set_operand_block);`。函数体上方的注释写明触发条件是「LIMIT 已设置」且「这是最外层的 query block（对 UNION 查询，是包含最终 UNION 求值上 LIMIT 的那个 block）」。

累计计数的落点在 `sql/sql_union.cc`：函数体注释写明「我们需要把计数累积到第一个 join 的 `send_records` 里，只要我们还支持 `SQL_CALC_FOUND_ROWS`，因为 `LimitOffsetIterator` 会用它来报告被 OFFSET 或 LIMIT 跳过的行数。」据此选择落点：简单查询用 `&first_query_block()->join->send_records`；已物化的集合运算用 `&query_term()->query_block()->join->send_records`；带 LIMIT 但没有 fake_query_block 的 UNION 用 `&send_records`。`sql/sql_union.cc` 的另一处注释解释了不装 `SQL_CALC_FOUND_ROWS` 时的行为：「这只是为了向后兼容；我们已经文档化过，没有 `SQL_CALC_FOUND_ROWS` 时返回实际返回的行数。」

### `LIMIT 1` 作为通用降级手段

`LIMIT_OFFSET` 访问路径在 MySQL 里不只服务于用户写的 LIMIT，还被大量用作「只需一行 / 只需少量行」的内部降级：

- `sql/sql_executor.cc` 的 `CreateWeedoutOrLimitAccessPath()`：confluent weedout（对所有字段去重的 weedout）可以退化成 `LIMIT 1`，函数体注释写明「这样一个 weedout 就可以丢掉 `WeedoutIterator` 的复杂度，直接插入一个 LIMIT 1」。
- `sql/sql_executor.cc` 的 hash join 构建侧：退化 semijoin / antijoin（没有 join 条件）只需要内层一行，因此套 `LIMIT 1`。
- `sql/sql_executor.cc` 的 slice 处理侧：整个 slice 都是 semijoin 时用 `LIMIT 1` 解决，注释写明「如果整个 slice 是外连接，我们用更迂回的方式解决了；见 `is_top_level_outer_join`」。
- `sql/item_subselect.cc` 的索引子查询侧：`JT_EQ_REF` 且带 `cond` 或 `having` 时套 `LIMIT 1`，注释解释「作为一个微优化，如果存在 filter 且索引是唯一的，我们加一个 LIMIT 1，这样任何加上的 filter 不会试图再读第二行（反正也不会有第二行）」。
- `sql/join_optimizer/join_optimizer.cc` 的 DISTINCT 侧：分组元素全为空时用 `LIMIT 1`。

### 回显：`Query_block::print_limit`

`sql/sql_lex.cc` 的 `Query_block::print_limit(const THD *thd, String *str, enum_query_type query_type)` 是把 LIMIT 子句重新打印成文本的函数（用于 EXPLAIN 与视图定义回显）。函数体有三段逻辑：

1. 取 `master_query_expression()` 的 `item`；若该 query expression 属于某个 `Item_subselect`，且它的 `global_parameters()` 就是当前 block，且子查询类型是 `EXISTS_SUBS` / `IN_SUBS` / `ALL_SUBS` 之一，则**直接 return 不打印**——因为这三个子查询类型的 LIMIT 是外层传下来的，不属于该子查询自己的语法。
2. 主条件是 `if (has_limit() && !m_internal_limit)`——**内部生成的 LIMIT（`m_internal_limit`）不回显**。
3. 打印格式：先追加 `" limit "`；若 `offset_limit` 非空，先打印 `offset_limit` 再追加一个逗号 `','`；最后打印 `select_limit`。即输出形态是 `limit <offset>,<limit>`。

`print_limit` 在 `sql/sql_lex.cc` 里有四处调用点：`Query_expression::print` 相关路径（`sql/sql_lex.cc` 中 `op->query_block()->print_limit(...)`）以及 `Query_block::print` 的三个分支。

## 关键取舍

- **源码事实：用 `HA_POS_ERROR` 一个哨兵值同时表达「LIMIT 10」和「不限制」。** `Query_block::get_limit()` 的第三条路径返回 `ha_rows{HA_POS_ERROR}`，而 `LimitOffsetIterator` 的 `limit` 参数注释写明「无限制时可以是 `HA_POS_ERROR`」。收益是整条链路上只有一个数值字段，下游不必额外维护「是否有上限」标志；代价是这个值在语义上是二义的——`HA_POS_ERROR` 在 handler 层是「无此行 / 出错」的标记，在 LIMIT 语义里被复用为「无上限」，读代码时必须靠上下文区分。

- **源码事实：`LIMIT + OFFSET` 的折算在 `Query_expression::set_limit()` 里做一次，并带溢出保护。** 加法若溢出（`select_limit_cnt + offset_limit_cnt >= select_limit_cnt` 不成立）就退化为 `HA_POS_ERROR`。收益是下游（`LimitOffsetIterator`、行数估计、成本模型）只需处理「总共读多少行」这一个数；代价是 `select_limit_cnt` 这个字段名与它的实际含义（已含 OFFSET）不再一致，`LimitOffsetIterator` 的 `limit` 参数注释「要读的最大行数，包括被 offset 跳过的那些」就是在补这个语义落差。

- **源码事实：OFFSET 的跳过放在 `Read()` 而不是 `Init()`，为的是 Performance Schema 批量模式的正确性与收益。** 源码注释给了两条并列理由：读到 OFFSET 行时批量模式已就绪（性能收益），且 `Init()` 期间不会意外开启批量模式（executor 尚未准备好关闭它）。这是「性能优化的时机约束压倒了 API 的直觉形态」的典型例子——直觉上 `Init()` 是做初始化工作的地方，但这里 `Init()` 只置两个标志位，真正的跳过动作推迟到第一次 `Read()`。

- **源码事实：把 LIMIT 与 OFFSET 合并成一次判断，而不是分别判断。** 私有成员的注释写明「如果有 OFFSET，已见行数从 `m_limit` 开始计，这意味着 `Read()` 的快路径上不需要分别做 LIMIT 和 OFFSET 两个测试」。收益是快路径只有一次整数比较；代价是 `m_seen_rows` 的含义随分支变化（有 OFFSET 时初值是 `m_limit`，无 OFFSET 时是 `0`），必须靠 `m_needs_offset` 这个布尔量辅助判断当前处于哪个阶段。

- **源码事实：`LIMIT 0` 走「零行计划」而不是迭代器截断，但 `SQL_CALC_FOUND_ROWS` 会挡住这条捷径。** `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 里 `if (query_expression()->select_limit_cnt == 0 && !calc_found_rows)` 直接 `create_access_paths_for_zero_rows()` 并跳到收尾。收益是完全不建执行树、不开表；代价是 `SQL_CALC_FOUND_ROWS` 场景必须放弃这条捷径，改为让迭代器跑到底——`sql/sql_executor.cc` 的 `attach_access_paths_for_having_and_limit()` 注释明确写着「对于 select_count，`LIMIT 0` 的常见情形在 `JOIN::optimize()` 里处理了，但 `CALC_FOUND_ROWS` 不处理。OFFSET 在那里也没处理」。

- **源码事实：LIMIT 会让优化器放弃「可能提前停」的假设，但 HAVING 与 `SQL_CALC_FOUND_ROWS` 会把这个假设拿掉。** `if (having_cond || calc_found_rows) m_select_limit = HA_POS_ERROR;`。收益是这三种情形下不会因为误判「读够 N 行就能停」而给出错误代价；代价是这三种情形的计划里 LIMIT 不再影响索引选择与行数估计。

- **源码事实：`LIMIT_OFFSET` 路径同时是「用户 LIMIT」和「内部只要几行」的通用载体。** 至少五处内部代码路径用它做降级（confluent weedout、退化 semijoin/antijoin、唯一索引子查询加 filter、整个 slice 是 semijoin、DISTINCT 分组全常量）。收益是降级只需复用一套已调优的迭代器；代价是 EXPLAIN 里出现 `LIMIT_OFFSET` 时无法一眼区分「用户写的 LIMIT」与「内部插入的 LIMIT 1」。

- **源码事实：标量子查询的基数检查有两处独立实现，报同一个错。** 一处在 `LimitOffsetIterator::Read()`（`m_seen_rows - m_offset > 0` 时报 `ER_SUBQUERY_NO_1_ROW`），一处在 `MaterializeIterator::Init()`（`stored_rows > 1` 时报同一个错，针对 INTERSECT/EXCEPT）。收益是无论走迭代器截断还是走物化回读，越界都能被同一错误码拦住；代价是同一个语义约束散落在两个迭代器里。

- **源码事实：LIMIT 会让语句在 binlog 中不安全。** `PT_limit_clause::contextualize()` 的最后一句是 `pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);`。收益是语句安全模式下不会误重放带 LIMIT 的语句；代价是带 LIMIT 的语句无法用于基于语句的安全复制场景。

- **架构推论：** LIMIT 在 MySQL 里被拆成「计划期的一个节点」而不是「执行器的一次后处理」，这是它能带来提前终止的根本原因——只有把 LIMIT 变成执行树上的一个节点，它才能挡在排序、物化、连接这些会放大行数的算子之前，从而真正少读数据。同一逻辑也解释了为什么 `LIMIT` 与 `ORDER BY` 组合时的顺序是「先排序后截断」：`sql/join_optimizer/join_optimizer.cc` 的注释明确写着 LIMIT_OFFSET 路径是加在 SORT 节点**之上**的。反过来说，如果 LIMIT 只是结果集末尾的裁剪，那么 `LIMIT 1` 就无法被用来做半连接、去重等优化——而源码里恰恰大量这么用。

- **架构推论：** 「LIMIT 计数」与「实际发送行数」被刻意分成两个量（`m_skipped_rows` 指向的 `send_records` 与结果集实际行数），且只在 `SQL_CALC_FOUND_ROWS` 存在时才需要精确对齐。`sql/sql_union.cc` 的注释说明了这一点，并顺带给出一条重构方向：「当我们不再需要 `SQL_CALC_FOUND_ROWS` 时，这里可以用一个局部变量。」这说明 MySQL 自己认为这套「借 JOIN 的 `send_records` 累加」的机制是临时的权宜之计。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_resolver.cc` | `Query_block::resolve_limits` | LIMIT/OFFSET 表达式的定型：`fix_fields` 后按 `Type_properties(MYSQL_TYPE_LONGLONG, true)` 传播类型并 `pin_data_type()`，决定 LIMIT 值域为 64 位无符号整数 |
| `sql/sql_resolver.cc` | `Query_block::is_row_count_valid_for_semi_join` | 半连接可用性判据：OFFSET 非 0 或 LIMIT 为 0 时否决 |
| `sql/sql_resolver.cc` | `Query_block::transform_subquery_to_derived` | 把 `reject_multiple_rows` 写进 `subs_query_expression->m_reject_multiple_rows`，是标量子查询基数检查的开关来源 |
| `sql/sql_resolver.cc` | `resolve_subquery` | 集合运算子查询的 LIMIT/OFFSET 处理：常量表值构造器按 `limit`/`offset` 与实际行数算出 `empty_rs` 并改写 `select_limit` |
| `sql/iterators/composite_iterators.h` | `LimitOffsetIterator` | LIMIT/OFFSET 的执行器形态：`limit` 含被 offset 跳过的行、无限制用 `HA_POS_ERROR`、`count_all_rows` 强制跑到底、`skipped_rows` 累计跳过行数；私有成员注释说明「已见行数从 `m_limit` 起步」使快路径只需一次判断 |
| `sql/iterators/composite_iterators.cc` | `LimitOffsetIterator::Init` | 初始化：源迭代器 `Init()` 失败即返回；有 OFFSET 时置 `m_seen_rows = m_limit`、`m_needs_offset = true`，否则置 0 与 false |
| `sql/iterators/composite_iterators.cc` | `LimitOffsetIterator::Read` | 截断实际执行：OFFSET 跳过循环放在 Read（批量模式原因）、跳过行逐行 `UnlockRow()`、`m_count_all_rows` 时读到底计数、`m_reject_multiple_rows` 时按 `m_seen_rows - m_offset > 0` 报 `ER_SUBQUERY_NO_1_ROW` |
| `sql/iterators/composite_iterators.cc` | `MaterializeIterator::Init` | 物化侧的平行基数检查：`m_reject_multiple_rows && stored_rows > 1` 时报 `ER_SUBQUERY_NO_1_ROW` |
| `sql/sql_lex.cc` | `Query_block::get_limit` | LIMIT 取值三路径：显式 `select_limit` → 会话变量 `thd->variables.select_limit`（仅当 `m_use_select_limit`）→ `ha_rows{HA_POS_ERROR}` |
| `sql/sql_lex.cc` | `Query_block::print_limit` | LIMIT 回显：EXISTS/IN/ALL 子查询不打印，`m_internal_limit` 的内部 LIMIT 不打印，输出格式为 `limit <offset>,<limit>` |
| `sql/sql_lex.cc` | `Query_block::get_offset` | OFFSET 取值：`offset_limit` 非空则 `val_uint()`，否则 `0` |
| `sql/sql_lex.cc` | `Query_expression::set_limit` | 把 LIMIT/OFFSET 折算成 `select_limit_cnt`（= LIMIT + OFFSET），加法溢出时退化为 `HA_POS_ERROR` |
| `sql/sql_lex.cc` | `Query_expression::has_any_limit` | 集合运算中检查是否有任一 query block 带 LIMIT |
| `sql/sql_lex.cc` | `Query_expression::is_mergeable` | 带 LIMIT 的查询表达式不可被合并进外层查询 |
| `sql/parse_tree_nodes.cc` | `PT_limit_clause::contextualize` | LIMIT/OFFSET 的上下文化：按 `is_offset_first` 决定 itemize 顺序，写入 `select_limit`/`offset_limit`，并置 `LEX::BINLOG_STMT_UNSAFE_LIMIT` |
| `sql/sql_yacc.yy` | `limit_option` / `limit_options` | LIMIT 的语法形态：`ident`、参数标记、三种整数字面量；`limit x,y` 与 `limit x offset y` 两种双值写法 |
| `sql/sql_optimizer.cc` | `JOIN::optimize` | LIMIT 对优化的影响：`m_select_limit` 作为「可能扫全表」的信号，`having_cond`/`calc_found_rows` 会把它重置为 `HA_POS_ERROR`，`LIMIT 0` 且非 `calc_found_rows` 时生成零行计划 |
| `sql/sql_optimizer.cc` | `EstimateRowAccesses` | `AccessPath::LIMIT_OFFSET` 分支把「剩余行数预算」夹给子树估算并终止继续下钻 |
| `sql/sql_executor.cc` | `attach_access_paths_for_having_and_limit` | 传统优化器侧挂 `LIMIT_OFFSET` 路径；注释说明 `LIMIT 0` 与 OFFSET 不在这里处理 |
| `sql/sql_executor.cc` | `CreateWeedoutOrLimitAccessPath` | confluent weedout 降级为 `LIMIT 1` |
| `sql/sql_executor.cc` | `ExecuteIteratorQuery` 侧的 hash join 构建 | 退化 semijoin/antijoin 只需内层一行，套 `LIMIT 1` |
| `sql/sql_executor.cc` | `create_access_paths_for_derived_table` | 派生表物化路径：OFFSET 非零时在物化路径之上再套 `LIMIT_OFFSET`，并传入 `m_reject_multiple_rows` |
| `sql/join_optimizer/access_path.h` | `NewLimitOffsetAccessPath` | `LIMIT_OFFSET` 访问路径的构造：写 `limit`/`offset`/`count_all_rows`/`reject_multiple_rows`/`send_records_override`，继承子路径的 ordering 与 immediate_update 属性，并调 `EstimateLimitOffsetCost` |
| `sql/join_optimizer/access_path.cc` | `CreateIteratorFromAccessPath` | `case AccessPath::LIMIT_OFFSET:` 分支装配 `LimitOffsetIterator`，`send_records` 取值顺序为 override → `join->send_records` → `nullptr` |
| `sql/join_optimizer/join_optimizer.cc` | `ApplyDistinctAndOrder` | ORDER BY 场景：LIMIT 能下推给 filesort 时由排序节点执行，否则在 SORT 路径之上加 `LIMIT_OFFSET`；OFFSET 非零时必加 |
| `sql/join_optimizer/join_optimizer.cc` | 超图优化器的 LIMIT 应用点 | 无 ORDER BY 时按 `select_limit_cnt`/`offset_limit_cnt` 挂 `LIMIT_OFFSET`；DISTINCT 分组全常量时用 `LIMIT 1` |
| `sql/sql_union.cc` | 集合运算的 LIMIT 路径 | 流式且非末位 DISTINCT 时在根路径上挂 `LIMIT_OFFSET`，并显式把 `&send_records` 作为累加落点 |
| `sql/sql_union.cc` | `Query_expression::execute` 发送循环 | 每发出一行 `++*send_records_ptr`；注释说明 `LimitOffsetIterator` 用 `send_records` 报告被 OFFSET/LIMIT 跳过的行数 |
| `sql/sql_select.cc` | `JOIN::prepare` 中的 `m_use_select_limit` 置位 | 只有语句本身没写 LIMIT 时，会话变量 `select_limit` 才生效 |
| `sql/sql_select.cc` | `JOIN::reset` | 重置执行状态时同步 `offset_limit_cnt` |
| `sql/sql_select.cc` | `JOIN::optimize` 中的 `m_select_limit` | `grouped` 时置 `HA_POS_ERROR`；排序表之后存在过滤条件时同样置 `HA_POS_ERROR`，即「必须排序全部行」 |
| `sql/sp.cc` | `sp_compile` | 编译存储过程定义体时临时把 `thd->variables.select_limit` 置为 `HA_POS_ERROR`，解析后恢复 |

## 与主核 / AI 的关系

主核（`engine/db`）的查询执行路径目前没有独立的「行数截断」层。LIMIT 在主核里若被实现，多半是查询结果组装阶段的一次性裁剪，而不是执行树上的一个节点。

**架构推论：** MySQL 把 LIMIT 做成执行树节点这一决定，对主核有直接可借鉴之处。差别不在「要不要支持 LIMIT」，而在「LIMIT 在管线的哪一环生效」。如果主核把 LIMIT 放在结果集组装的最末端，那么「`ORDER BY` + `LIMIT`」就必须先把全部结果排完再裁剪，而 MySQL 的做法（`sql/join_optimizer/join_optimizer.cc` 中 LIMIT_OFFSET 路径叠在 SORT 路径之上）虽然也在排序之后截断，但 LIMIT 节点本身可以下推给排序节点（`sort().limit` 字段），让 filesort 只排够 N 行就停——这个「排序节点自己知道上限」的能力，主核的排序器目前没有对应物。

**架构推论：** 更值得借鉴的是 `LimitOffsetIterator` 里那条「OFFSET 跳过放在 `Read()` 而非 `Init()`」的注释所揭示的约束意识：任何会改变执行代价的开关（本例是 Performance Schema 批量模式）都必须与「首次取行」这个时刻对齐，否则会在错误路径上留下未关闭的状态。主核若有类似的观测开关（性能采样、统计计数、调试钩子），应该明确它们在「初始化阶段」与「首次取行阶段」的语义差别。

**架构推论：** MySQL 把「内部只要几行」这件事统一表达成 `LIMIT 1` 访问路径（至少五处降级路径），使优化器与执行器共用一套「只要 N 行」的语义。这对 AI 场景有直接价值：模型生成 SQL 时经常写出等价但更啰嗦的写法（如「先取 100 行再过滤成 1 行」），MySQL 的内部降级说明「把「只要几行」的意图下推给引擎」是一个可被规划器识别的模式；主核若要支持 AI 参与执行策略选择，应当把这类意图表达成计划里的显式节点，而不是等 AI 生成恰好可优化的 SQL。

对 AI 场景而言，LIMIT 还牵涉一个 MySQL 明确拒绝承担的责任：**分页游标的稳定性不属于服务端职责**（见本机制条目的边界说明）。**架构推论：** 这意味着由模型驱动的分页场景需要应用层自己维护游标（例如用唯一键做 keyset pagination），而不是指望 `LIMIT`/`OFFSET` 在并发写入下给出稳定结果。主核若要让 AI 参与分页逻辑规划，应当把「游标稳定性由谁负责」这一边界写进给模型的契约里，否则模型会生成在并发下错位的分页代码。

## 参考

- [MySQL 源码：`sql/iterators/composite_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.h) —— `LimitOffsetIterator` 的类声明、构造参数注释与私有成员的计数设计说明。
- [MySQL 源码：`sql/iterators/composite_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.cc) —— `LimitOffsetIterator::Init()` / `Read()` 的实际执行顺序、OFFSET 跳过放在 Read 的原因注释与 `ER_SUBQUERY_NO_1_ROW` 分支。
- [MySQL 源码：`sql/sql_resolver.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_resolver.cc) —— `Query_block::resolve_limits` 的类型定型逻辑、半连接相容性判据与标量子查询到派生表的转换。
- [MySQL 源码：`sql/sql_lex.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_lex.cc) —— `Query_block::get_limit` / `get_offset` / `print_limit` 与 `Query_expression::set_limit` 的取值与折算。
- [MySQL 源码：`sql/join_optimizer/access_path.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.h) —— `NewLimitOffsetAccessPath` 与 `AccessPath::limit_offset` 各字段的含义。
- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_optimizer.cc) —— `JOIN::optimize()` 中 LIMIT 与 `calc_found_rows` 的交互、`Zero limit` 的零行计划分支。
- [MySQL 参考手册：SELECT 语句的 LIMIT 子句](https://dev.mysql.com/doc/refman/8.0/en/select.html) —— LIMIT/OFFSET 的语法形式与语义。
- [MySQL 参考手册：SQL_CALC_FOUND_ROWS 与 FOUND_ROWS()](https://dev.mysql.com/doc/refman/8.0/en/select.html) —— `SQL_CALC_FOUND_ROWS` 使查询跑到底以统计总行数的官方语义。
- [MySQL 参考手册：INNODB 存储引擎内部机制](https://dev.mysql.com/doc/refman/8.0/en/innodb-internals.html) —— 存储引擎侧的滚动与索引机制背景，用于理解 LIMIT 触发提前终止时读取层的行为。
