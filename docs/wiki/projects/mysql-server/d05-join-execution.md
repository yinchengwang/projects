---
project: mysql-server
feature: join-execution
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_select.cc
    symbols: [JOIN::exec]
  - path: sql/iterators/composite_iterators.h
    symbols: [NestedLoopIterator]
  - path: sql/sql_executor.cc
    symbols: [JOIN::create_access_paths, attach_access_paths_for_having_and_limit]
---

# 连接执行

## 一句话职责

连接执行负责把「哪些表按什么顺序连起来」这个优化期决策（见 C02 计划、C03 索引选择）在执行期真正跑起来：按 `QEP_TAB` 序列构造一棵连接访问路径树，把内连接、外连接、半连接、反连接各自落成对应的连接算子，并在外侧行推进时驱动内侧行迭代器的重扫与停止；它不负责优化器如何选出连接顺序，也不负责单个表扫描算子内部的取行算法。

## 机制是什么

**这一层要解决的是「一棵连接计划，怎么变成一串内外表交替的行流」。**

MySQL 8.0 的连接执行不是一段手写的嵌套循环，而是三层结构：

1. **访问路径层**（`AccessPath` 树）：`sql/sql_executor.cc` 的 `JOIN::create_access_paths()` 把优化器留下的 `QEP_TAB` 数组变成一棵 `AccessPath` 树。连接节点是 `AccessPath::NESTED_LOOP_JOIN`，它带三个子字段：`outer`、`inner`、`join_type`。`join_type` 取自 `JoinType` 枚举，本页涉及的取值有 `JoinType::INNER`、`JoinType::OUTER`、`JoinType::SEMI`、`JoinType::ANTI`（`sql/join_optimizer/access_path.h` 的 `nested_loop_join` 联合成员默认初始化为 `JoinType::INNER`）。
2. **迭代器层**（`RowIterator` 树）：`sql/join_optimizer/access_path.cc` 的 `AccessPath::NESTED_LOOP_JOIN` 分支把两个子迭代器交给 `NewIterator<NestedLoopIterator>(thd, mem_root, std::move(job.children[0]), std::move(job.children[1]), param.join_type, param.pfs_batch_mode)`，产出 `sql/iterators/composite_iterators.h` 的 `NestedLoopIterator`。
3. **驱动层**：`sql/sql_union.cc` 的 `Query_expression::ExecuteIteratorQuery()` 在 `m_root_iterator->Init()` 之后用一个 for 循环反复 `m_root_iterator->Read()`，把每一行交给 `query_result->send_data(thd, *fields)`。

**连接的核心是「外侧一行 × 内侧一段」的推进语义，由四态状态机承担。** `NestedLoopIterator` 的私有枚举 `m_state` 有四个取值：`NEEDS_OUTER_ROW`、`READING_FIRST_INNER_ROW`、`READING_INNER_ROWS`、`END_OF_ROWS`。头文件注释把这个迭代器定位为「一个简单的嵌套循环连接，取两个迭代器（左/外与右/内）把它们连接起来……这当然会多次扫描内侧迭代器。它目前是我们仅有的连接形式。」

**不同连接类型的差异，集中在两处判断上：内侧读完时要不要补 NULL 行，以及内侧出现第一行时要不要停。**

- **内连接（`JoinType::INNER`）**：内侧读完直接回到外侧，不产出任何补充行。
- **外连接（`JoinType::OUTER`）**：若这个外侧行在内侧一行都没匹配上，连接器必须为它合成一行「全 NULL 的内侧」，否则该外侧行会在结果里彻底消失。`NestedLoopIterator::Read()` 的判据是 `(m_join_type == JoinType::OUTER && m_state == READING_FIRST_INNER_ROW)`——注意 `m_state == READING_FIRST_INNER_ROW` 这个附加条件：只有「还没产出过任何内侧行」才补 NULL 行，一旦已经产出过内侧行，内侧读完只是这个外侧行的匹配结束，直接推进外侧即可。
- **半连接（`JoinType::SEMI`）**：只关心「有没有匹配」，不关心匹配了几条。读到第一条内侧行就把 `m_state` 置回 `NEEDS_OUTER_ROW` 并返回这一行——外侧行保留，内侧行不进入结果集。
- **反连接（`JoinType::ANTI`）**：只关心「有没有不匹配」。读到第一条内侧行就说明这个外侧行**有**匹配，必须被丢弃：`NestedLoopIterator::Read()` 在 `m_join_type == JoinType::ANTI` 分支里把状态置回 `NEEDS_OUTER_ROW` 后 `continue`，**不返回这一行**。反过来，若内侧一行都没读到（`err == -1`），反连接必须为这个外侧行产出一行（语义上「没找到任何不匹配项」），所以反连接与外连接共享同一条补 NULL 行的分支。

**PSI 批量模式是连接器的一个专属优化点，且对半连接/反连接强制关闭。** `NestedLoopIterator` 的成员 `m_pfs_batch_mode` 决定是否给内侧迭代器开 Performance Schema 批量模式。头文件注释解释了这个优化的适用范围：「典型地只在整个连接最内层的表上使用（那里开启批量模式的收益最大，而关闭它的精度损失也最小）」。构造函数里有一条断言：`if (join_type == JoinType::ANTI || join_type == JoinType::SEMI) { assert(!pfs_batch_mode); }`，理由写在注释里——「批量模式对反连接或半连接没有意义，因为它们只应读取一行」。`sql/sql_executor.cc` 的 `CreateNestedLoopAccessPath()` 在 `join_type == JoinType::ANTI || join_type == JoinType::SEMI` 时同样把 `path->nested_loop_join().pfs_batch_mode` 强制置为 `false`，即在计划层就已经关掉，不依赖迭代器构造函数的断言来兜。

**这一层不负责判定「该不该用这条连接路径」，那在更早的阶段就已经定好了。** `sql/sql_executor.cc` 的 `UseHashJoin(qep_tab)` 的实现只有一行 `return qep_tab->op_type == QEP_TAB::OT_BNL;`——即「优化器把这段连接标成了 BNL，执行期就按 hash join 的形状处理」；`UseBKA(qep_tab)` 同理以 `qep_tab->op_type == QEP_TAB::OT_BKA` 为起点，再加一条限制：「如果外连接 BKA 的右侧包含多于一张表，我们就没有左深树，在这个阶段处理不了」，于是返回 `false`。

## 怎么实现的

### 入口：`JOIN::create_access_paths()`

`sql/sql_executor.cc` 的 `JOIN::create_access_paths()` 是连接执行的总入口，函数体只有四行：

```
assert(m_root_access_path == nullptr);
AccessPath *path = create_root_access_path_for_join();
path = attach_access_paths_for_having_and_limit(path);
path = attach_access_path_for_update_or_delete(path);
m_root_access_path = path;
```

三步的分工是明确的：**第一步造连接树，第二步在树顶挂 HAVING 与 LIMIT/OFFSET，第三步为 UPDATE/DELETE 挂目标表过滤**。`attach_access_path_for_update_or_delete()` 里有两条断言 `assert(command != SQLCOM_UPDATE); assert(command != SQLCOM_DELETE);`，说明 UPDATE/DELETE 是由另外的入口（`SQLCOM_UPDATE_MULTI` / `SQLCOM_DELETE_MULTI`）走的。

### 第一步：`create_root_access_path_for_join()` 与 `ConnectJoins()`

`JOIN::create_root_access_path_for_join()` 按四种情形分叉：

- `select_count` 为真 → 直接返回 `NewUnqualifiedCountAccessPath(thd)`，不建连接树。
- `query_block->is_table_value_constructor` 为真 → 返回 `NewTableValueConstructorAccessPath(thd)`，并把 `best_rowcount` 与 `path->set_num_output_rows()` 都设为 `query_block->row_value_list->size()`。
- `const_tables == primary_tables`（全是常量表）→ 返回 `NewFakeSingleRowAccessPath(thd, /*count_examined_rows=*/true)`，再用 `PossiblyAttachFilter(path, vector<Item *>{where_cond}, thd, &conditions_depend_on_outer_tables)` 挂上 `where_cond`。注释解释了为什么要造一行假行：「加一个假的单行来 join 进所有常量表（只有内连接的表在优化器里才会被提升为常量表）」。
- 其余情形 → 调 `ConnectJoins()` 递归构造整棵连接树。

`ConnectJoins()` 的调用参数是：`ConnectJoins(/*upper_first_idx=*/NO_PLAN_IDX, const_tables, primary_tables, qep_tab, thd, TOP_LEVEL, nullptr, &pending_invalidators, /*pending_join_conditions=*/nullptr, &unhandled_duplicates, &conditions_depend_on_outer_tables)`。

`ConnectJoins()` 的结构是**一个遍历表序列的 for 循环 + 对子结构的递归**。函数注释写明：「MySQL 的连接树一般是一棵内连接的左深树，所以我们可以从左边开始，对着下一张表做一次内连接，把结果再对着下一张表做内连接，等等。」但连接树并不只有内连接：注释接着说「某个表序列切片可以被指定为外连接（通过设置第一张表的 `first_inner()` 和 `last_inner()`），如果是这样，我们就把这个切片递归处理一次，把结果放到外连接的右（内）臂上。」

函数体里的主循环 `for (plan_idx i = first_idx; i < last_idx;)` 每轮做三件事：

1. **补发延期的 invalidator**：遍历 `pending_invalidators`，把 `table_index_to_invalidate < last_idx` 的那些用 `NewInvalidatorAccessPathForTable()` 挂到当前树上。注释说明这与 Lateral 派生表有关：「如果行经过它或它以任何其他方式改变状态，它就递增自己的『代际』计数器。这让 `MaterializeIterator` 能看出它的任何依赖是否改变，从而强制重新物化——这典型地用于 LATERAL 表」。
2. **识别子结构**：`FindSubstructure(qep_tabs, first_idx, i, last_idx, calling_context, &add_limit_1, &substructure_end, unhandled_duplicates)`。`FindSubstructure()` 的返回类型是 `enum class Substructure { NONE, OUTER_JOIN, SEMIJOIN, WEEDOUT }`。
3. **按子结构分派**。

### `FindSubstructure()`：识别外连接、半连接与 weedout

`FindSubstructure()` 里三种结构的识别条件都直接来自 `QEP_TAB` 上的标记：

- **外连接**：`qep_tab->last_inner() != NO_PLAN_IDX && qep_tab->last_inner() < last_idx`，外连接的结束位置是 `outer_join_end = qep_tab->last_inner() + 1`。
- **半连接**：遍历 `j` 从 `this_idx` 到 `last_idx`，一旦发现 `qep_tabs[j].firstmatch_return == this_idx - 1` 就判定为半连接，结束位置 `semijoin_end = j + 1`。函数注释解释了「first match」的含义：「如果表 N 对表 M 设了『first match』（即在表 N 看到一条未被过滤的记录时跳回表 M），那么我们需要递归处理从 `[M+1,N]` 的一个子切片，然后把它作为半连接的右侧。」
- **weedout**：起点是 `qep_tab->starts_weedout()`，终点是扫描到 `qep_tabs[j].check_weed_out_table == qep_tab->flush_weedout_table` 的那张表。

`FindSubstructure()` 里还有三条重要的冲突消解规则：

- **weedout 与外连接/半连接重叠时**：若 weedout 范围更大，则「weedout 会在更低的递归层被处理」，于是 `is_weedout = false`；否则若 weedout 是 *confluent* 的（`qep_tab->flush_weedout_table->is_confluent`），则改用 `*add_limit_1 = true`——注释写明理由：「confluent weedout 至多返回一行，所以可以把 weedout 换成一个 LIMIT 1」。若都不是，则调 `MarkUnhandledDuplicates()` 把这些表记进 `unhandled_duplicates`，由顶层在所有连接之后补一个 weedout。
- **半连接与外连接重叠时**：若 `semijoin_end == outer_join_end`，则 `*add_limit_1 = true; is_semijoin = false;`——注释解释：「名义上这些表应该被优化掉，但这不是该干的地方，所以我们用一个 LIMIT 1 解决它，然后把该切片当作普通外连接处理。」若 `semijoin_end > outer_join_end`，源码直接 `assert(false)`，注释说「join 优化器不应当产生这种情况」。
- **最外层优先**：`if (calling_context == DIRECTLY_UNDER_SEMIJOIN && this_idx == first_idx && semijoin_end == last_idx) { is_semijoin = false; }`，外连接同理。函数注释解释「外连接和半连接可以嵌套，所以需要小心，确保我们递归的是最外层的结构」。最后有一句断言 `assert(is_semijoin + is_outer_join + is_weedout <= 1);` 把「三者互斥」写成硬约束。

### 外连接与半连接的构造：`join_type` 的确定

`ConnectJoins()` 在 `substructure == Substructure::OUTER_JOIN || substructure == Substructure::SEMIJOIN` 的分支里确定 `join_type`：

- 若 `qep_tab->table()->reginfo.not_exists_optimize` 为真（即优化器把这段外连接判定为「可以转成反连接」），则 `join_type = (pending_conditions == nullptr) ? JoinType::ANTI : JoinType::OUTER;`。注释解释了为什么不能在已经位于另一个外连接内侧时启用反连接优化：「否则我们会让更上层的外连接在不该产生 NULL 行的地方产生 NULL 行。」
- 反连接标记还有一条特殊例外：源码扫描 `subtree_pending_conditions` 与 `subtree_pending_join_conditions`，把 `it->cond->item_name.ptr() == antijoin_null_cond` 且 `it->table_index_to_attach_to == int(i)` 的条件**删掉**，同时把 `join_type` 强行改回 `JoinType::ANTI`。注释解释这是 `NOT IN` 的场景：「这里一个 FALSE 条件被用来指定内侧行应当通过这个连接，但它们应当阻止 null-complemented 行的产生。」`antijoin_null_cond` 这个名字的字面量定义在 `sql/sql_optimizer.cc`：`const char *antijoin_null_cond = "<ANTIJOIN-NULL>";`。
- 其余情形：`join_type = substructure == Substructure::SEMIJOIN ? JoinType::SEMI : JoinType::OUTER;`。

`not_exists_optimize` 这个标记的设置点在 `sql/sql_optimizer.cc`：一处是检测 `fld->val->type() == Item::NULL_ITEM && !fld->item_field->field->is_nullable()` 的 `IS NULL` 谓词，注释给的例子是 `SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.a WHERE t2.a IS NULL;`；另一处是构造反连接时设 `lt->table()->reginfo.not_exists_optimize = true;`。

### `CreateNestedLoopAccessPath()`：把连接写成访问路径

`sql/sql_executor.cc` 的 `CreateNestedLoopAccessPath(THD *thd, AccessPath *outer, AccessPath *inner, JoinType join_type, bool pfs_batch_mode)` 是内连接、外连接、半连接、反连接共同的落地函数，函数体把四个参数原样塞进访问路径：

```
path->type = AccessPath::NESTED_LOOP_JOIN;
path->nested_loop_join().outer = outer;
path->nested_loop_join().inner = inner;
path->nested_loop_join().join_type = join_type;
```

随后按连接类型决定批量模式：`if (join_type == JoinType::ANTI || join_type == JoinType::SEMI) { path->nested_loop_join().pfs_batch_mode = false; } else { path->nested_loop_join().pfs_batch_mode = pfs_batch_mode; }`，注释写明「对反连接或半连接来说这作为优化没有意义」。

`pfs_batch_mode` 的来源是 `qep_tab->pfs_batch_update(qep_tab->join())`。`sql/sql_executor.cc` 的 `QEP_TAB::pfs_batch_update()` 写明它返回 `false` 的三种情形：

```
return !((join->qep_tab + join->primary_tables - 1) != this ||   // 1
         this->type() == JT_EQ_REF ||                             // 2
         this->type() == JT_CONST || this->type() == JT_SYSTEM ||
         (condition() && condition()->has_subquery()));           // 3
```

即：**不是最内层表**、**访问类型是 `JT_EQ_REF`/`JT_CONST`/`JT_SYSTEM`**、**条件里含子查询**——这三种情况下不开批量模式。

`ConnectJoins()` 在三处调用 `CreateNestedLoopAccessPath()`：外连接/半连接分支（`path = CreateNestedLoopAccessPath(thd, path, subtree_path, join_type, pfs_batch_mode)`）、weedout 分支（`JoinType::INNER`，`/*pfs_batch_mode=*/false`）、以及主循环里的内连接分支（`JoinType::INNER`，`qep_tab->pfs_batch_update(qep_tab->join())`）。

### `NestedLoopIterator::Init()` 与 `Read()`：状态机的实际推进

`sql/iterators/composite_iterators.cc` 的 `NestedLoopIterator::Init()` 是四行的初始化：

```
if (m_source_outer->Init()) return true;
m_state = NEEDS_OUTER_ROW;
if (m_pfs_batch_mode) { m_source_inner->EndPSIBatchModeIfStarted(); }
return false;
```

`NestedLoopIterator::Read()` 是本层最核心的函数。它的骨架是一个 `for (;;)` 无限循环，靠状态变量推进：

- **状态为 `NEEDS_OUTER_ROW`**：读外侧行。`m_source_outer->Read()` 返回 `1` 则直接 `return 1`（错误）；返回 `-1` 则置 `m_state = END_OF_ROWS` 并 `return -1`（结束）。成功读到外侧行后，若 `m_pfs_batch_mode` 则调 `m_source_inner->StartPSIBatchMode()`；接着调 `m_source_inner->SetNullRowFlag(false)`，源码注释解释了顺序原因：「`Init()` 可能读取了 NULL 行标志（例如在构建哈希表时），所以要在之前而不是之后 unset 掉它。」最后调 `m_source_inner->Init()`（内侧必须为每一个新的外侧行重新初始化），并置 `m_state = READING_FIRST_INNER_ROW`。
- **状态为 `READING_FIRST_INNER_ROW` 或 `READING_INNER_ROWS`**：读内侧行 `m_source_inner->Read()`。若 `err != 0 && m_pfs_batch_mode` 则先调 `m_source_inner->EndPSIBatchModeIfStarted()`。`err == 1` 返回 `1`；接着查 `thd()->killed`，命中则 `thd()->send_kill_message(); return 1;`。
- **内侧读完（`err == -1`）**：这是分叉点。若 `(m_join_type == JoinType::OUTER && m_state == READING_FIRST_INNER_ROW) || m_join_type == JoinType::ANTI`，则 `m_source_inner->SetNullRowFlag(true); m_state = NEEDS_OUTER_ROW; return 0;`——产出一行 NULL 补齐行。源码注释写明：「对这个外侧行来说内侧行已读完。如果我们是一个外连接且从未找到任何内侧行，就返回一个 null-complemented 行。否则跳过它，直接去读一个新的外侧行。」否则 `m_state = NEEDS_OUTER_ROW; continue;`。
- **找到内侧行**：若 `m_join_type == JoinType::ANTI`，源码注释是「反连接应该在内侧看到一行的瞬间就停止扫描内侧，而且不返回那一行」，于是 `m_state = NEEDS_OUTER_ROW; continue;`。若 `m_join_type == JoinType::SEMI`，注释是「半连接在第一行之后就应停止；普通连接（内连接和外连接）则应继续扫完剩下的」，于是 `m_state = NEEDS_OUTER_ROW;` 并返回 `0`。其余（内连接、外连接）置 `m_state = READING_INNER_ROWS;` 并返回 `0`。

`NestedLoopIterator::UnlockRow()` 则按状态条件转发：只有在 `m_state == READING_FIRST_INNER_ROW || m_state == READING_INNER_ROWS` 时才调 `m_source_inner->UnlockRow()`，注释解释了原因：「因为我们不知道是哪个条件否决了这一行，所以我们也无法知道能否同时解锁外侧行（它可能还被其他连接结果用到）。」

### 代价估算：`SetCostOnNestedLoopAccessPath()`

`sql/sql_executor.cc` 的 `SetCostOnNestedLoopAccessPath()` 给出连接节点的代价与输出行数。关键公式直接来自函数体：

```
double inner_expected_rows_before_filter =
    pos_inner->filter_effect > 0.0 ? (inner->num_output_rows() / pos_inner->filter_effect) : 0.0;
double joined_rows = outer->num_output_rows() * inner_expected_rows_before_filter;
path->set_num_output_rows(joined_rows * pos_inner->filter_effect);
path->cost = outer->cost + pos_inner->read_cost + cost_model.row_evaluate_cost(joined_rows);
```

两处保护：任一子节点的 `num_output_rows()` 为 `-1.0`（缺少代价信息）则直接返回；`pos_inner == nullptr` 也直接返回。函数注释说这段计算「镜像 `set_prefix_join_cost()`，尽管这个代价计算并不太合理」。

### 第二步：`attach_access_paths_for_having_and_limit()`

`sql/sql_executor.cc` 的 `JOIN::attach_access_paths_for_having_and_limit(AccessPath *path)` 在连接树顶挂两层东西：

- **HAVING**：`if (having_cond != nullptr)` 则 `path = NewFilterAccessPath(thd, path, having_cond);` 再 `CopyBasicProperties(*old_path, path);`。函数注释说明了「我们可以在没有 GROUP BY 的情况下也有 HAVING，虽然这不太有用」。代价估算被限制在超图优化器下：`if (thd->lex->using_hypergraph_optimizer())` 才调 `EstimateFilterCost(thd, path->num_output_rows(), having_cond, query_block)`，注释给出的理由是「在旧的优化器中我们不能调 `EstimateFilterCost()`，因为在重复执行预备语句时，条件可能包含对尚未销毁也未重新优化的子查询的引用」。
- **LIMIT/OFFSET**：判定条件是 `if (query_expression()->select_limit_cnt != HA_POS_ERROR || query_expression()->offset_limit_cnt != 0)`，满足则 `path = NewLimitOffsetAccessPath(thd, path, query_expression()->select_limit_cnt, query_expression()->offset_limit_cnt, calc_found_rows, false, /*send_records_override=*/nullptr);`。`HA_POS_ERROR` 是「无限制」的哨兵值。注释补充：「对于 `select_count`，LIMIT 0 在 `JOIN::optimize()` 里对常见情况已经处理了，但 `CALC_FOUND_ROWS` 不是。OFFSET 也没有在那里处理。」

同一个函数还被 `JOIN::create_access_paths_for_index_subquery()` 复用——该函数建好索引子查询的路径后调 `path = attach_access_paths_for_having_and_limit(path);` 再赋值给 `m_root_access_path`。

### 顶层驱动：`Query_expression::ExecuteIteratorQuery()`

`sql/sql_union.cc` 的 `Query_expression::ExecuteIteratorQuery()` 是根迭代器的驱动方。取行循环是：

```
if (m_root_iterator->Init()) { return true; }
PFSBatchMode pfs_batch_mode(m_root_iterator.get());
for (;;) {
  int error = m_root_iterator->Read();
  if (error > 0 || thd->is_error()) return true;   // 致命错误
  else if (error < 0) break;                        // 记录结束
  else if (thd->killed) { thd->send_kill_message(); return true; }
  ++*send_records_ptr;
  if (query_result->send_data(thd, *fields)) return true;
}
```

`send_records_ptr` 的选取分三种：`is_simple()` 时取 `&first_query_block()->join->send_records`；`set_operation()->m_is_materialized` 时取 `&query_term()->query_block()->join->send_records`；否则取 `&send_records`。注释解释了原因：「我们需要把找到的行数累积到第一个 join 的 `send_records` 里，只要我们还支持 `SQL_CALC_FOUND_ROWS`，因为 `LimitOffsetIterator` 会用它来报告被 OFFSET 或 LIMIT 跳过的行数。」

函数末尾通过一个 scope guard 做连接清理：

```
auto join_cleanup = create_scope_guard([this, thd] {
  for (Query_block *sl = first_query_block(); sl; sl = sl->next_query_block()) {
    JOIN *join = sl->join;
    join->join_free();
    thd->inc_examined_row_count(join->examined_rows);
  }
  ...
});
```

`join_free()` 定义在 `sql/sql_select.cc`，其函数注释说明这是「JOIN 执行后的部分清理：关闭索引或随机读（表游标），释放 quick select」。注释同时说明了它为什么不是完整清理：「如果一个 JOIN 是为子查询执行的，或者它含有子查询，我们不能做完整清理，只能做部分清理。」函数体里 `bool full = (!query_block->uncacheable && !thd->lex->is_explain());` 决定了走完整清理还是部分清理。

### 半连接与反连接的其他落地路径

半连接并不总是走 `NestedLoopIterator`。`sql/sql_executor.cc` 的 `CreateWeedoutOrLimitAccessPath()` 里，若 `weedout_table->is_confluent` 就返回 `NewLimitOffsetAccessPath(thd, path, /*limit=*/1, /*offset=*/0, ...)`，注释解释：「一个『confluent』weedout 是在所有字段上去重的。如果是，我们可以丢掉 `WeedoutIterator` 的复杂性，简单地插入一个 LIMIT 1。」

`ConnectJoins()` 中 `add_limit_1` 为真时，同样用 `NewLimitOffsetAccessPath(thd, subtree_path, /*limit=*/1, /*offset=*/0, /*count_all_rows=*/false, /*reject_multiple_rows=*/false, /*send_records_override=*/nullptr)` 包裹子树路径。源码里 `/*limit=*/1` 与 `/*offset=*/0` 这两个实参是硬编码的常量，不是来自配置。

**LooseScan 的半连接有一个专属迭代器。** `sql/iterators/composite_iterators.h` 的 `NestedLoopSemiJoinWithDuplicateRemovalIterator` 的类注释写明：「一个在语义上等价于『半连接 `NestedLoopIterator` 紧接着 `RemoveDuplicatesOnIndexIterator`』的迭代器。它用于在半连接内侧有多张表的查询中实现『loose scan』策略。」注释给出的解法是：「它从外侧取一行，从内侧取恰好一行，然后跳过外侧的行（**不扫内侧**）直到它的 keypart 发生变化。」`ConnectJoins()` 在 `qep_tabs[i - 1].do_loosescan() && qep_tabs[i - 1].match_tab != i - 1` 且子结构是 `SEMIJOIN` 时走 `NewNestedLoopSemiJoinWithDuplicateRemovalAccessPath()`；若是外连接，则改用 `remove_duplicates_loose_scan = true`，把去重推到连接与过滤之后。

**半连接还有一条完全不同的落地路径：物化。** `sql/sql_select.cc` 的 `JOIN::setup_semijoin_materialized_table(JOIN_TAB *tab, uint tableno, POSITION *inner_pos, POSITION *sjm_pos)` 为一个半连接物化嵌套建立执行结构：建临时表（`create_tmp_table(thd, &sjm_exec->table_param, emb_sj_nest->nested_join->sj_inner_exprs, nullptr, true /* distinct */, true /* save_sum_fields */, thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS, HA_POS_ERROR /* rows_limit */, name)`），表名由 `snprintf(buffer, sizeof(buffer) - 1, "<subquery%u>", emb_sj_nest->nested_join->query_block_id)` 生成。函数注释列出了它建立的三样东西：「物化临时表（含 `Table_ref` 对象）、临时表每列一个 `Item_field` 对象的列表、描述对该表做索引查找的 keyuse 数组（供 MaterializeLookup 用）」。断言限定了适用范围：`assert(inner_pos->sj_strategy == SJ_OPT_MATERIALIZE_LOOKUP || inner_pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN);`。

物化的表路径在 `sql/sql_executor.cc` 的 `GetTableAccessPath()` 里构造（`qep_tab->materialize_table == QEP_TAB::MATERIALIZE_SEMIJOIN` 分支）：它以 `ConnectJoins(/*upper_first_idx=*/NO_PLAN_IDX, join_start, join_end, qep_tabs, thd, TOP_LEVEL, ...)` 把半连接的内侧当作一个独立的「虚拟连接」来建树，注释写明「把这个子查询当作一个完全独立的连接来处理，即使这些表属于同一个 `JOIN` 对象」。该分支还额外加了一道 `IS NOT NULL` 过滤，注释说明原因：「由于物化的半连接基于对表的 ref 访问，而 ref 访问的 NULL = NULL（而 IN 表达式不应该这样），所以在这里移除含 NULL 的行。这对 IN 只是一个优化（因为等值传播会在另一侧过滤掉 NULL），但对 NOT IN 的正确性是必需的。」

`sql/sql_select.cc` 的 `JOIN::set_semijoin_info()` 负责把优化器选出的半连接策略落到 `JOIN_TAB` 上。它按 `pos->sj_strategy` 分派：`SJ_OPT_NONE` 跳过；`SJ_OPT_MATERIALIZE_LOOKUP`、`SJ_OPT_MATERIALIZE_SCAN`、`SJ_OPT_LOOSE_SCAN`、`SJ_OPT_DUPS_WEEDOUT`、`SJ_OPT_FIRST_MATCH` 五种都归为一类处理——把该范围内的表的 `first_sj_inner` 设为 `tableno`，`last_sj_inner` 设为 `last_sj_tab`；唯独 `SJ_OPT_DUPS_WEEDOUT` 例外，其 `last_sj_inner` 被设为 `NO_PLAN_IDX`，源码注释解释「范围可能以非内侧表结束，所以不能设置 `last_sj_inner_tab`」。

### 条件如何落到连接上：`SplitConditions()`

`sql/sql_executor.cc` 的 `SplitConditions()` 决定每个条件挂到连接的哪一侧。它的注释把 `QEP_TAB` 上的条件分成四类，其中最关键的是第 3 类：「像第 2 类那样位于左连接内侧（右边）的谓词。这些条件必须被移到连接的**上面**，因为它们也应该对连接可能生成的 NULL 补齐行做测试。例如 `t1 LEFT JOIN t2 WHERE t1.x + t2.x > 3`，这个条件会附在 `t2` 的 `QEP_TAB` 上，但需要附在连接之上，否则它会错误地保留那些 `t2` 没有产生（真实）行的行。」

函数体按条件包装类型分派：

- `Item_func_trig_cond::FOUND_MATCH` 类型的触发条件 → `predicates_above_join->push_back(PendingCondition{inner_cond, trig_cond->idx()})`，即推到连接之上。
- `Item_func_trig_cond::IS_NOT_NULL_COMPL` 类型 → 这是外连接的连接条件；如果内层还套着触发条件，则用 `GetInnermostCondition()` 取最内层条件再推上去（注释：「我们可能有条件嵌在多层触发条件里。我们要最内层的条件，因为到这里我们其实已经不在乎触发条件了」）。
- 没有触发条件的普通条件 → `predicates_below_join->push_back(item)`，即在连接之下立即求值。

`join_conditions` 参数为非空时（表示位于某个将被 hash join 执行的外连接内侧），条件被收集到这个向量，稍后挂到 hash join 迭代器上。`SplitConditions()` 的调用点在 `ConnectJoins()` 的主循环里：`SplitConditions(qep_tab->condition(), qep_tab, &predicates_below_join, &predicates_above_join, replace_with_hash_join ? pending_join_conditions : nullptr, semi_join_table_idx, left_tables);`。

### 顶层补 weedout

`create_root_access_path_for_join()` 在 `ConnectJoins()` 返回后检查 `unhandled_duplicates != 0`，若非零则 `path = NewWeedoutAccessPathForTables(thd, unhandled_duplicates, qep_tab, primary_tables, child);` 并 `CopyBasicProperties(*child, path);`。源码注释说明：「如果我们在 `ConnectJoins()` 期间不得不丢弃任何 weedout（即连接留下了一些本该去重却没有去重的表），现在在最末尾处理它们。」

## 关键取舍

- **源码事实：连接类型被压缩成四值枚举，判断集中在两处。** `JoinType` 的 `INNER`/`OUTER`/`SEMI`/`ANTI` 覆盖了 `NestedLoopIterator` 需要区分的全部语义：内侧读完要不要补 NULL 行、内侧出现第一行要不要停。收益是状态机本身很薄——`NestedLoopIterator::Read()` 里与连接类型相关的判断只有 `JoinType::ANTI`（找到内侧行就丢弃）、`JoinType::SEMI`（找到内侧行就停）、以及 `(JoinType::OUTER && m_state == READING_FIRST_INNER_ROW) || JoinType::ANTI`（补 NULL 行）三处。代价是「外连接 + 反连接」的组合无法直接表达，只能靠 `ConnectJoins()` 里那条 `antijoin_null_cond` 名字匹配的特例来单独处理。

- **源码事实：半连接与反连接强制关闭 PSI 批量模式。** 断言在 `NestedLoopIterator` 构造函数里，强制置位在 `CreateNestedLoopAccessPath()` 里，两处独立。理由写在构造函数注释里：「批量模式对反连接或半连接没有意义，因为它们只应读取一行。」收益是批量模式的计时误差不会影响这两种连接的结果正确性；代价是这两类连接的 Performance Schema 开销无法摊薄。

- **源码事实：`UnlockRow()` 放弃解锁外侧行。** `NestedLoopIterator::UnlockRow()` 只在 `READING_FIRST_INNER_ROW` 或 `READING_INNER_ROWS` 状态下解锁内侧，注释解释是因为「不知道是哪个条件否决了这一行」。代价是外侧行的锁在 `READ COMMITTED` 等隔离级别下可能被多保留一段时间。

- **源码事实：连接代价公式被源码自己标注为「不太合理」。** `SetCostOnNestedLoopAccessPath()` 的注释写明这段计算「镜像 `set_prefix_join_cost()`，尽管这个代价计算并不太合理」，函数体末尾那句 `cost_model.row_evaluate_cost(joined_rows)` 也没有更细的拆解。收益是代价模型足够简单，能与旧的 `POSITION` 代价体系并存；代价是嵌套循环连接的代价估计精度有限。

- **源码事实：内连接恒为左深树。** `ConnectJoins()` 在主循环的内连接分支里有一句 `assert(qep_tab->last_inner() == NO_PLAN_IDX);`，注释是「内连接总是左深的，所以我们可以随着找到它们就把表接上」。收益是内连接路径的构造不需要维护右子树；代价是右深形状的连接无法表达——`UseBKA()` 里那条「外连接 BKA 的右侧包含多于一张表 → 我们就没有左深树，在这个阶段处理不了」的返回 `false` 就是这条约束的直接后果。

- **源码事实：hash join 与 BKA 的选择完全由优化器的 `op_type` 决定。** `UseHashJoin()` 的实现只有 `return qep_tab->op_type == QEP_TAB::OT_BNL;` 一行；`UseBKA()` 在此之上再加一条左深约束。执行期不重新判断「哪种更快」。收益是优化期与执行期的决策一致，不存在两处标准打架；代价是优化器标记的方案在运行时条件变化时无法回退。

- **源码事实：条件位置靠触发条件（trigger condition）编码，而不是靠单独的结构字段。** `SplitConditions()` 通过识别 `Item_func_trig_cond` 的 `FOUND_MATCH` 与 `IS_NOT_NULL_COMPL` 两类触发条件来区分「该在连接之下求值」还是「该在连接之上求值」。函数注释的 `TODO` 直言：「优化器本该区分连接前与连接后的条件，而不是让我们在这里把它们理清楚。」收益是条件仍能挂在 `QEP_TAB` 上，不额外占用结构空间；代价是执行期必须对每个条件做一层包装解包。

- **源码事实：冲突情形被显式拒绝而不是静默处理。** `FindSubstructure()` 里对半连接与外连接的非法重叠直接 `assert(false)`，注释说「join 优化器不应当产生这种情况」；对半连接与外连接的重叠则用 `add_limit_1` + 视作普通外连接来消解。收益是计划异常会被立刻暴露而不是产生错误结果；代价是必须依赖优化器的不变式。

- **源码事实：`NOT IN` 的正确性靠一道显式的 `IS NOT NULL` 过滤兜底。** `GetTableAccessPath()` 的 `MATERIALIZE_SEMIJOIN` 分支给物化的内侧加上 `Item_func_isnotnull` 过滤，注释写明这对 IN 只是优化，「但对 NOT IN 的正确性是必需的」。收益是把「ref 访问的 NULL = NULL」与「IN 的 NULL 语义」之间的差异显式消掉；代价是每种半连接物化都要多挂一层过滤。

- **架构推论：** MySQL 把「连接顺序与访问方法」全部交给优化器、执行期只做忠实翻译，这条分界让执行期代码不必携带任何代价模型判断，因此 `ConnectJoins()` 的主循环只有结构分支、没有数值分支。代价是执行期无法在运行时改主意——一旦优化器选了 BNL，`UseHashJoin()` 就直接返回 `true`，即使运行时数据分布已经不同。

- **架构推论：** 状态机式（`m_state` 四态）而不是递归式的连接推进，是 MySQL 能把连接、weedout、物化统一进同一棵 `RowIterator` 树的前提。`NestedLoopIterator` 的类注释说它是「一个状态机，状态记录着是否需要读新的外侧行，以及是否已经从内侧迭代器见过任何行（如果没有，外连接需要合成一个新的 NULL 行）」。收益是连接器可以被任意嵌套在其他组合迭代器里；代价是状态机的正确性完全依赖 `m_state` 与两侧迭代器的调用时序严格对齐——例如 `SetNullRowFlag(false)` 必须在 `m_source_inner->Init()` **之前**调用，源码为此专门留了注释说明原因。

- **架构推论：** 把「条件该放在连接哪一侧」的判断放在执行期（`SplitConditions()`）而不是优化期，是把优化器的输出「降级」成可执行形态的代价。注释里的 `TODO` 承认这是权宜之计。收益是优化器不必重写一遍条件的归属；代价是执行期承担了本该由优化器完成的语义判定工作。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_executor.cc` | `JOIN::create_access_paths` | 连接执行总入口：造连接树 → 挂 HAVING/LIMIT → 挂 UPDATE/DELETE 目标表，结果存入 `m_root_access_path` |
| `sql/sql_executor.cc` | `JOIN::create_root_access_path_for_join` | 按 `select_count` / 表值构造 / 全常量表 / 一般连接四种情形分叉；一般连接调 `ConnectJoins()`；处理遗留 weedout 与尾部物化 |
| `sql/sql_executor.cc` | `attach_access_paths_for_having_and_limit` | 在连接树顶挂 `NewFilterAccessPath`（HAVING）与 `NewLimitOffsetAccessPath`（LIMIT/OFFSET），LIMIT 判定用 `HA_POS_ERROR` 哨兵 |
| `sql/sql_executor.cc` | `ConnectJoins` | 连接树构造主循环：识别子结构后递归构造子树，再按 `join_type` 合成 `NESTED_LOOP_JOIN`；处理 weedout、LooseScan、hash join/BKA 分支 |
| `sql/sql_executor.cc` | `FindSubstructure` | 识别 `Substructure::{NONE, OUTER_JOIN, SEMIJOIN, WEEDOUT}`；处理三者的冲突消解与 `add_limit_1` |
| `sql/sql_executor.cc` | `CreateNestedLoopAccessPath` | 写入 `AccessPath::NESTED_LOOP_JOIN`、`outer`/`inner`/`join_type`；对 `ANTI`/`SEMI` 强制关闭 `pfs_batch_mode` |
| `sql/sql_executor.cc` | `SplitConditions` | 按 `FOUND_MATCH` / `IS_NOT_NULL_COMPL` 触发条件把条件分到 `predicates_below_join` / `predicates_above_join` / `join_conditions` |
| `sql/sql_executor.cc` | `SetCostOnNestedLoopAccessPath` | 连接代价与输出行数：`joined_rows = outer->num_output_rows() * inner_expected_rows_before_filter`，代价为 `outer->cost + pos_inner->read_cost + row_evaluate_cost(joined_rows)` |
| `sql/sql_executor.cc` | `UseHashJoin` / `UseBKA` | 连接形态选择：`UseHashJoin` 判 `op_type == QEP_TAB::OT_BNL`；`UseBKA` 判 `op_type == QEP_TAB::OT_BKA` 并附加左深树约束 |
| `sql/sql_executor.cc` | `QueryMixesOuterBKAAndBNL` | 探测「外连接 BKA 右侧出现非 BKA 连接」的病态组合，命中则整条查询同时关闭 BKA 与 hash join |
| `sql/sql_executor.cc` | `QEP_TAB::pfs_batch_update` | 决定是否开 PSI 批量模式：非最内层表、`JT_EQ_REF`/`JT_CONST`/`JT_SYSTEM`、条件含子查询这三种情形返回 `false` |
| `sql/sql_executor.cc` | `CreateWeedoutOrLimitAccessPath` | confluent weedout 退化为 `LIMIT 1`；否则建 `NewWeedoutAccessPath` |
| `sql/sql_executor.cc` | `GetTableAccessPath` | 表级路径装配；`MATERIALIZE_SEMIJOIN` 分支把半连接内侧当独立连接建树并加 `IS NOT NULL` 过滤 |
| `sql/iterators/composite_iterators.h` | `NestedLoopIterator` | 连接迭代器：四态枚举 `NEEDS_OUTER_ROW`/`READING_FIRST_INNER_ROW`/`READING_INNER_ROWS`/`END_OF_ROWS`；构造时断言 `ANTI`/`SEMI` 不带 `pfs_batch_mode` |
| `sql/iterators/composite_iterators.h` | `NestedLoopSemiJoinWithDuplicateRemovalIterator` | LooseScan 专用：语义等价于半连接 `NestedLoopIterator` + `RemoveDuplicatesOnIndexIterator`，内侧只取一行后按 keypart 跳过外侧行 |
| `sql/iterators/composite_iterators.cc` | `NestedLoopIterator::Init` | 初始化外侧迭代器、状态置 `NEEDS_OUTER_ROW`、关闭可能残留的内侧批量模式 |
| `sql/iterators/composite_iterators.cc` | `NestedLoopIterator::Read` | 连接状态机实际推进：外侧 EOF → `END_OF_ROWS`；内侧 EOF 时按 `OUTER`/`ANTI` 决定是否补 NULL 行；`ANTI` 见到内侧行即丢弃；`SEMI` 见到首行即停 |
| `sql/iterators/composite_iterators.cc` | `NestedLoopIterator::UnlockRow` | 仅在 `READING_FIRST_INNER_ROW`/`READING_INNER_ROWS` 状态解锁内侧行 |
| `sql/join_optimizer/access_path.cc` | `CreateIteratorFromAccessPath` | 访问路径到迭代器树的装配；`NESTED_LOOP_JOIN` 分支产出 `NewIterator<NestedLoopIterator>` 并传入 `join_type` 与 `pfs_batch_mode` |
| `sql/join_optimizer/access_path.h` | `AccessPath::nested_loop_join` | 连接节点结构：`outer`、`inner`、`join_type`、`pfs_batch_mode` 等成员，默认 `join_type` 为 `JoinType::INNER` |
| `sql/sql_select.cc` | `JOIN::set_semijoin_info` | 按 `pos->sj_strategy`（`SJ_OPT_MATERIALIZE_LOOKUP`/`SJ_OPT_MATERIALIZE_SCAN`/`SJ_OPT_LOOSE_SCAN`/`SJ_OPT_DUPS_WEEDOUT`/`SJ_OPT_FIRST_MATCH`）把半连接范围标到 `JOIN_TAB` 的 `first_sj_inner`/`last_sj_inner` |
| `sql/sql_select.cc` | `JOIN::setup_semijoin_materialized_table` | 为半连接物化建立临时表（`create_tmp_table` 带 `distinct`）、`Table_ref` 与 keyuse 数组 |
| `sql/sql_select.cc` | `JOIN::join_free` | 连接执行后的部分清理：关表游标、释放 quick select；`full = (!query_block->uncacheable && !thd->lex->is_explain())` 决定清理力度 |
| `sql/sql_union.cc` | `Query_expression::ExecuteIteratorQuery` | 根迭代器驱动：`m_root_iterator->Init()` 后循环 `Read()`，`send_data` 出行，scope guard 里调 `join_free()` |
| `sql/sql_optimizer.cc` | `antijoin_null_cond` | 反连接标记条件的名字常量，字面量 `"<ANTIJOIN-NULL>"`；`ConnectJoins()` 据此把嵌套在外连接内的反连接单独识别出来 |

## 与主核 / AI 的关系

主核（`engine/db`）的连接执行目前是固定形态：单表扫描 → 过滤 → 两表嵌套循环，没有连接类型枚举，也没有外连接/半连接/反连接的语义分叉。

**架构推论：** MySQL 这层有三点值得主核借鉴。其一是**「连接类型 = 一个枚举 + 状态机里两处分支」的做法**：MySQL 把外连接补 NULL 行、半连接首行即停、反连接首行即丢这三件事，全部收敛到 `NestedLoopIterator::Read()` 的两个判断上，配合一个四态 `m_state`。这比「每种连接类型一个迭代器类」的实现量小得多，代价是 `JoinType` 一旦扩展，状态机就要跟着加分支——MySQL 用 `assert` 把非法组合挡在开发期。主核若要支持更多连接形态（如全外连接），同样的收敛思路可以省掉重复的迭代器类。

其二是**「条件该放在连接哪一侧」被显式建模**。`SplitConditions()` 用 `FOUND_MATCH` 与 `IS_NOT_NULL_COMPL` 两类触发条件，把「连接之下求值」与「连接之上求值」区分开，并专门处理了外连接内侧的谓词必须上移这一条（否则 `t1 LEFT JOIN t2 WHERE t1.x + t2.x > 3` 会错误保留 NULL 补齐行）。主核若在连接执行里直接把所有条件一律放在连接之下，外连接的 NULL 补齐行就会带着未检验的值流入结果。

其三是**「代价与结构分离」的边界**：MySQL 的执行期完全不做代价判断——`UseHashJoin()` 只读优化器写好的 `op_type`。这让执行期代码保持纯粹，但也意味着运行期无法因数据分布变化而改换连接算法。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「同一个语义有多少种落地形态」。**架构推论：** MySQL 的 `EXPLAIN` 输出里，外连接/半连接/反连接与 BNL/hash join/BKA 的组合会直接反映到计划树形态上；`opt_explain.cc` 里还有专门针对反连接的输出分支（`if (table->reginfo.not_exists_optimize && push_extra(ET_NOT_EXISTS))`）。这意味着 AI 若要判断「我写的这条 SQL 慢在哪一层」，必须能把计划树里的连接节点形态与 SQL 的连接语义对应起来。主核若要让 AI 参与执行策略选择，应当把「连接类型 + 连接算法 + 两侧实际行数」做成可查询的结构化对象。

## 参考

- [MySQL 源码：`sql/sql_executor.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_executor.cc) —— `JOIN::create_access_paths`、`ConnectJoins`、`FindSubstructure`、`CreateNestedLoopAccessPath`、`SplitConditions` 的实现。
- [MySQL 源码：`sql/iterators/composite_iterators.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.h) —— `NestedLoopIterator` 与 `NestedLoopSemiJoinWithDuplicateRemovalIterator` 的声明与状态枚举。
- [MySQL 源码：`sql/iterators/composite_iterators.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/iterators/composite_iterators.cc) —— 连接状态机 `NestedLoopIterator::Init`/`Read`/`UnlockRow` 的实际推进与错误分支。
- [MySQL 源码：`sql/join_optimizer/access_path.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.h) —— `AccessPath::NESTED_LOOP_JOIN` 与 `nested_loop_join` 联合成员的字段定义。
- [MySQL 源码：`sql/join_optimizer/access_path.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/access_path.cc) —— 访问路径到迭代器树的装配，含 `NESTED_LOOP_JOIN` 到 `NestedLoopIterator` 的分派。
- [MySQL 源码：`sql/sql_select.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_select.cc) —— `JOIN::set_semijoin_info`、`JOIN::setup_semijoin_materialized_table`、`JOIN::join_free` 的实现。
- [MySQL 源码：`sql/sql_union.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_union.cc) —— `Query_expression::ExecuteIteratorQuery` 的取行循环与 `send_records` 计数。
- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_optimizer.cc) —— `antijoin_null_cond` 常量与 `not_exists_optimize` 标记的设置点。
- [MySQL 参考手册：JOIN 类型与连接实现](https://dev.mysql.com/doc/refman/8.0/en/join-types.html) —— 内连接、外连接、交叉连接等连接类型的 SQL 语义。
- [MySQL 参考手册：子查询优化与半连接](https://dev.mysql.com/doc/refman/8.0/en/subquery-optimization.html) —— 半连接策略（物化、FirstMatch、LooseScan、 weedout）与 `IN`/`NOT IN` 的改写规则。
- [MySQL 参考手册：EXPLAIN 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html) —— 连接算子（BNL/hash join/BKA）在执行计划输出中的形态。
- [MySQL 参考手册：Performance Schema 事件语句表](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-events-statements.html) —— 连接执行的 Performance Schema 计时与批量模式所依托的事件机制。
