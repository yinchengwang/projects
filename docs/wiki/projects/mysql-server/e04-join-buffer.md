---
project: mysql-server
feature: join-buffer
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_optimizer.cc
    symbols: [setup_join_buffering, set_join_buffer_properties]
---

# 连接缓冲

## 一句话职责

`sql/sql_optimizer.cc` 的 `setup_join_buffering()` 负责在执行计划定型之后、逐表判定「这张表要不要用连接缓冲（join buffer）、用哪一种」：它按访问方式（`JT_ALL` / `JT_INDEX_SCAN` / `JT_RANGE` / `JT_INDEX_MERGE` 走 BNL，`JT_SYSTEM` / `JT_CONST` / `JT_REF` / `JT_EQ_REF` 走 BKA）、按半连接策略、按 `@@optimizer_switch` 的 `bnl` / `bka` 开关与 `BNL()` / `BKA()` 提示逐层排除，最后把结论写进 `tab->set_use_join_cache()`（取值来自 `sql/sql_join_buffer.h` 的 `JOIN_CACHE::enum_join_cache_type`：`ALG_NONE = 0`、`ALG_BNL = 1`、`ALG_BKA = 2`）。它不负责索引扫描路径本身（D03），也不负责连接顺序的决策（C02）。

## 机制是什么

**连接缓冲是「把内表整批物化进内存、再与外表行批量匹配」的连接执行形态**，目的与代价都很直接：

- **它要解决的问题**：当内表没有可用索引、只能全表扫描时，朴素的嵌套循环会把内表扫一遍又一遍（外表每出一行就重扫一次内表）。连接缓冲把内表（或外表的若干行）先攒进一块内存缓冲，于是内表只需被扫描「若干次」（次数由缓冲能装多少行决定），而不是「外表行数次」。
- **它付出的代价**：缓冲占用内存（预算是会话变量 `thd->variables.join_buff_size`），并且会**打乱行序**——缓冲里的行在恢复时按缓冲的物理顺序输出，而不是按索引顺序。源码在 `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 里明确写了这一点：一旦某张表用了连接缓冲，就把 `simple_order = simple_group = false`，理由是「连接缓冲会改变行的顺序，所以不能再依赖第一个非 const 表的行是有序的」。

**同一个页签下有两种缓冲算法**，由 `setup_join_buffering()` 依据内表访问方式二选一：

1. **BNL（Block Nested Loop，分块嵌套循环）**：适用于内表走**全表扫描类**访问（`JT_ALL`、`JT_INDEX_SCAN`、`JT_RANGE`、`JT_INDEX_MERGE`）。把内表的行分块读进缓冲，每读满一块就与外表的行做一次匹配。
2. **BKA（Batched Key Access，批量键访问）**：适用于内表走**等值查找类**访问（`JT_SYSTEM`、`JT_CONST`、`JT_REF`、`JT_EQ_REF`）。把**外表的行**攒进缓冲（`sql/iterators/bka_iterator.cc` 的 `BKAIterator::ReadOuterRows()` 就是攒外表的行），再用这些行的键值批量走 MRR（Multi-Range Read）去探内表索引。

注意两者的缓冲方向相反：BNL 缓冲内表，BKA 缓冲外表。

**「分块/分批」的粒度由内存预算决定**，预算就是 `thd->variables.join_buff_size`。`sql/sql_planner.cc` 的 `Optimize_table_order::calculate_scan_cost()` 在估算 BNL 成本时按这个预算算需要几个缓冲块：

```
const double buffer_count =
    1.0 + ((double)cache_record_length(join, idx) * prefix_rowcount /
           (double)thd->variables.join_buff_size);
```

`cache_record_length()` 累加的是从 `join->const_tables` 到当前表之前的每张表的 `used_fieldlength`（由 `calc_used_field_length()` 算出，源码注释说明此时不知道 Duplicate Weedout 是否可用，长度可能不准但可接受）。

## 怎么实现的

### 决策发生的位置

`setup_join_buffering()` 是 `sql/sql_optimizer.cc` 里的 `static bool` 函数，声明处带一句注释说明它在 `JOIN::optimize()` 中的位置。它的调用点在 `JOIN::optimize()` 里一个按 `const_tables` 到 `tables` 顺序的循环中：

```
for (uint i = const_tables; i < tables; i++) {
  JOIN_TAB *const tab = best_ref[i];
  if (!tab->position()) continue;
  if (setup_join_buffering(tab, this, no_jbuf_after)) return true;
  if (tab->use_join_cache() != JOIN_CACHE::ALG_NONE) simple_sort = false;
  ...
}
```

循环里紧跟的一句 `simple_sort = false` 配合循环后的 `simple_order = simple_group = false;`，就是前面说的「用了连接缓冲就不能再声称行序有序」。

`no_jbuf_after` 的来源在同一个函数更早处：如果第一个非 const 表带了 `FORCE INDEX FOR ORDER BY` 且存在 `ORDER BY`，或者带了 `FORCE INDEX FOR GROUP BY` 且存在分组，就把 `no_jbuf_after` 置为 `0`，注释写明理由是「对第一个表用 FORCE INDEX FOR ORDER BY / GROUP BY 意味着不能用连接缓冲」。

### 函数体：从「初判」到「最终定案」

`setup_join_buffering()` 的执行顺序如下。

**第一步，取两个开关。**

```
const bool bnl_on = hint_table_state(join->thd, tab->table_ref, BNL_HINT_ENUM,
                                     OPTIMIZER_SWITCH_BNL);
const bool bka_on = hint_table_state(join->thd, tab->table_ref, BKA_HINT_ENUM,
                                     OPTIMIZER_SWITCH_BKA);
```

`BNL()` / `NO_BNL()` / `BKA()` / `NO_BKA()` 四种表级提示在 `sql/sql_hints.yy` 的 `table_level_hint_type_on` / `table_level_hint_type_off` 产生规则里分别映射到 `BNL_HINT_ENUM` 与 `BKA_HINT_ENUM`（`NO_BNL` 与 `BNL` 共用同一个枚举值，区别在提示解析阶段）。开关本身来自 `@@optimizer_switch` 的 `bnl` / `bka` 项。

**第二步，算 MRR 标志位初值。**

```
const key_part_map keypart_map = make_prev_keypart_map(tab->ref().key_parts);
if (tab->ref().null_rejecting == keypart_map) {
  join_cache_flags |= HA_MRR_NO_NULL_ENDPOINTS;
}
```

只有当**所有** key part 都是 null-rejecting 时，才承诺 `HA_MRR_NO_NULL_ENDPOINTS`。源码注释解释了原因：这种情况下 `MultiRangeRowIterator` 自己就能把 NULL 值的键位全部剔除掉。

**第三步，先按「贪婪搜索阶段的初判」预置算法。**

```
if (!join->select_count)
  tab->set_use_join_cache(tab->position()->use_join_buffer
                              ? JOIN_CACHE::ALG_BNL
                              : JOIN_CACHE::ALG_NONE);
```

`use_join_buffer` 这个标志由 `sql/sql_planner.cc` 的 `Optimize_table_order::best_access_path()` 在选访问路径时写入 `POSITION`（`pos->use_join_buffer = best_uses_jbuf;`），而 `best_uses_jbuf` 只在「选中扫描类访问而不是 ref 访问」这一支被置真。同一个函数里，`disable_jbuf` 会在三种情况下被强制置真：调用方传入即为真、**当前表是连接序里的第一张非 const 表**（`idx == join->const_tables`）、以及剩余 LATERAL 派生表存在外部依赖。第三条在源码注释里被明确指回 `setup_join_buffering()`：「Reason 3: @see setup_join_buffering()」——即两处判定是配对的。

另外注意 `join->select_count` 这个条件：统计计数场景（`SELECT COUNT(*)` 一类）下不做这个预置。

**第四步，第一道否决：const 表。**

```
if (tableno == join->const_tables) {
  assert(tab->use_join_cache() == JOIN_CACHE::ALG_NONE);
  return false;
}
```

连接序里的第一张表永远不做连接缓冲——它是驱动表，没有可缓冲的「前一表」。

**第五步，第二道否决：开关与 `no_jbuf_after`。**

```
if (!(bnl_on || bka_on)) goto no_join_cache;
if (tab->use_quick == QS_DYNAMIC_RANGE) goto no_join_cache;
if (tableno > no_jbuf_after) goto no_join_cache;
```

第一条是 `@@optimizer_switch` 与提示共同关掉的情况；第二条是 `QS_DYNAMIC_RANGE`（动态范围扫描）的情况，源码在它上面留了一条 TODO 注释，疑问「为什么执行期代码看起来能处理『每条记录都重新检查范围』这种情况」；第三条是 `no_jbuf_after` 的作用——表序号大于 `no_jbuf_after` 之后就不允许再用连接缓冲。

**第六步，第三道否决：外连接嵌套的一致性。**

```
if (tab->first_inner() != NO_PLAN_IDX && tab->first_inner() != tab->idx() &&
    !join->best_ref[tab->first_inner()]->use_join_cache())
  goto no_join_cache;
if (tab->first_upper() != NO_PLAN_IDX &&
    !join->best_ref[tab->first_upper()]->use_join_cache())
  goto no_join_cache;
```

第一条：外连接嵌套里的内表，如果该嵌套的**第一个内表**不用连接缓冲，那么这个内表也不能用。第二条：该表所属的**上一层嵌套**的第一个内表不用连接缓冲时，本表也不能用。两条注释都写明「这个条件前面的优化器阶段没有处理」。

**第七步，第四道否决：表函数与半连接策略。**

表函数（`is_table_function()`）且是 dependent 的表直接排除。随后按 `tab_sj_strategy` 分支：

- `SJ_OPT_FIRST_MATCH`：只有当该表是**半连接的唯一内表**（`tab->is_single_inner_of_semi_join()`）时才允许用连接缓冲——但这条限制**对 secondary engine 放开了**，条件是 `current_thd->lex->m_sql_cmd != nullptr && current_thd->lex->m_sql_cmd->using_secondary_storage_engine()`。
- `SJ_OPT_LOOSE_SCAN`：由 LooseScan 处理的半连接嵌套不使用连接缓冲。
- `SJ_OPT_MATERIALIZE_LOOKUP` / `SJ_OPT_MATERIALIZE_SCAN`：物化策略下，**嵌套中的第一张表**（`tab->first_sj_inner() == tab->idx()`）不能用连接缓冲，理由写在注释里——物化策略复用的是被物化的第一张表的 `JOIN_TAB`，而连接序里的第一张表永远不用连接缓冲；物化嵌套里**其余的表**是允许用连接缓冲的。
- `SJ_OPT_DUPS_WEEDOUT`：源码注释写明「该策略允许与普通连接相同的连接缓冲」。

**第八步，第五道否决：外连接 + FirstMatch 的冲突。**

```
if (tab_sj_strategy == SJ_OPT_FIRST_MATCH &&
    tab->is_inner_table_of_outer_join())
  goto no_join_cache;
```

这条限制同样**对 secondary engine 放开**。源码注释给出了完整理由：外连接需要一个「match flag」来标记某行是否需要 NULL 补行，这个 flag 挂在第一个内表的 cache 上；FirstMatch 也需要一个 match flag，挂在半连接内表的 cache 上。**但一个 cache 上的行只有一个 match flag 位**——当「半连接内表」就是「第一个内表」时两者冲突。

**第九步，第六道否决：LATERAL 派生表依赖。**

```
if (join->deps_of_remaining_lateral_derived_tables &
    (tab->prefix_tables() & ~tab->added_tables())) {
  goto no_join_cache;
}
```

源码注释给了一个具体场景来说明为什么：计划形如 `t1-t2-dt`，其中 `dt` 是 LATERAL 且只依赖 `t1`。如果 `t2` 用了连接缓冲，就会先缓冲很多行 `t1`，然后读一行 `t2`，从缓存取 `t1` 的第 1 行，物化 `dt` 并发给客户端；再取 `t1` 的第 2 行，再次物化 `dt`——非常低效。所以禁止 `t2` 使用连接缓冲，这样「`t1` 的行变了」这个信号会在 `t1` 的操作符层级发出，频率低得多（`t1` 的一行可以服务 `t2` 的 N 行）。注释同时指出这还有一个副作用：`dt` 自己也被禁止使用连接缓冲。注释末尾写着「See similar code in best_access_path()」。

### 按访问方式定案：BNL 分支

```
switch (tab->type()) {
  case JT_ALL:
  case JT_INDEX_SCAN:
  case JT_RANGE:
  case JT_INDEX_MERGE:
    if (!bnl_on) { ... goto no_join_cache; }
    if (!join->select_count) tab->set_use_join_cache(JOIN_CACHE::ALG_BNL);
    return false;
```

这四种访问方式（全表扫描、索引扫描、范围扫描、index merge）一律走 BNL，但要求 `bnl_on` 为真。`join->select_count` 为真时同样不做设置。

### 按访问方式定案：BKA 分支

```
  case JT_SYSTEM:
  case JT_CONST:
  case JT_REF:
  case JT_EQ_REF:
    if (!bka_on) { ... goto no_join_cache; }
    if (tab->table_ref->uses_materialization()) goto no_join_cache;
    if (tab->has_guarded_conds()) goto no_join_cache;
    if (tab->table()->covering_keys.is_set(tab->ref().key))
      join_cache_flags |= HA_MRR_INDEX_ONLY;
    rows = tab->table()->file->multi_range_read_info(
        tab->ref().key, 10, 20, &bufsz, &join_cache_flags, &cost);
    if ((rows == HA_POS_ERROR) ||
        (join_cache_flags & HA_MRR_USE_DEFAULT_IMPL) ||
        (join_cache_flags & HA_MRR_NO_ASSOCIATION))
      goto no_join_cache;
    tab->set_use_join_cache(JOIN_CACHE::ALG_BKA);
    tab->join_cache_flags = join_cache_flags;
    return false;
```

这四种访问方式（系统表、常量表、ref 查找、eq_ref 查找）走 BKA，要求 `bka_on` 为真。三条额外否决：

- **可物化的派生表 / 视图**（`uses_materialization()`）：源码注释写明「它们还没有被实例化」，因此禁用 BKA。
- **带 guard 条件的表**（`has_guarded_conds()`）：源码注释指向 `Item_in_optimizer::val_int()`、`subselect_iterator_engine::exec()` 与 `Index_lookup::cond_guards`，说明当子查询可能把 ref 访问退化成「对 NULL 键的全表扫描」时不能用 BKA。同一段注释里还留了一条 `@todo`：这个判断本该在**成本估算之前**就做（即在做 `set_join_buffer_properties()` 那一步之前），但那时尚未建立 cond guard，所以只能退到这里补做。

随后向存储引擎询问 MRR 能否使用：

```
rows = tab->table()->file->multi_range_read_info(
    tab->ref().key, 10, 20, &bufsz, &join_cache_flags, &cost);
```

传进去的 `n_ranges` 是 `10`、`n_rows` 是 `20`（这是「假设有 10 个 range、每个 range 约 2 行」的估算输入）；`bufsz` 进函数前被初始化为 `4096`（局部变量 `uint bufsz = 4096;`），进函数后由引擎改写为建议的缓冲大小。

`sql/handler.cc` 的 `handler::multi_range_read_info()` 是默认实现，它做三件事：把 `*bufsz` 置 `0`（默认实现不需要缓冲）、往 `*flags` 里加上 `HA_MRR_USE_DEFAULT_IMPL` 与 `HA_MRR_SUPPORT_SORTED`、按是否 `HA_MRR_INDEX_ONLY` 给出成本（`index_scan_cost()` 或 `read_cost()`），返回 `0`。

InnoDB 走的是 DS-MRR：`DsMrr_impl::dsmrr_info()` 先调默认实现拿基准成本与标志，再调 `DsMrr_impl::choose_mrr_impl()` 决定用 DS-MRR 还是默认实现。`choose_mrr_impl()` 拒绝 DS-MRR 的条件包括：MRR 关闭且无提示强制、`*flags` 里带 `HA_MRR_INDEX_ONLY` 或 `HA_MRR_SORTED`（DS-MRR 不支持）、该键是聚簇主键、键使用部分列（`key_uses_partial_cols()`）、表是临时表。

`choose_mrr_impl()` 里还有一段 `OPTIMIZER_SWITCH_MRR_COST_BASED` 打开时的启发式（源码注释逐条列出）：存储引擎有内存缓冲时，DS-MRR 只在**表大小超过缓冲池**时才考虑；没有内存缓冲时阈值是 **100 MB**（代码里写死 `min_file_size = 100 * 1024 * 1024;`）；由于 DS-MRR 有初始化开销，**只有预计读取至少 50 条记录**时才考虑（代码里的判断是 `rows <= 50` 就放弃）。

DS-MRR 的缓冲大小由 `DsMrr_impl::get_disk_sweep_mrr_cost()` 细化：单条元素大小 `elem_size = h->ref_length + sizeof(void *) * !(flags & HA_MRR_NO_ASSOCIATION)`，缓冲能装 `max_buff_entries = *buffer_size / elem_size` 条；装不下连一个 rowid 都不行时直接返回失败。估算不足一轮缓冲时，把缓冲大小收缩为 `max(1.2 * rows_in_last_step, 100)` 条记录所需的大小（源码注释：加 20% 余量并保证至少 100 条记录）。

**回到 `setup_join_buffering()` 的三个否决条件**：MRR 返回 `HA_POS_ERROR`（MRR 扫描做不了）、标志里带 `HA_MRR_USE_DEFAULT_IMPL`（引擎选了默认 MRR 实现）、标志里带 `HA_MRR_NO_ASSOCIATION`——三者任一成立就放弃 BKA，转而走 `no_join_cache`。

### 回退路径：`no_join_cache` 与 `revise_cache_usage()`

所有被否决的分支都汇到函数末尾的 `no_join_cache:` 标签：

```
no_join_cache:
  revise_cache_usage(tab);
  tab->set_use_join_cache(JOIN_CACHE::ALG_NONE);
  return false;
```

**回退到 `revise_cache_usage()`**（定义在 `sql/sql_optimizer.cc`）。它的职责写在函数头的注释里：「Revise usage of join buffer for the specified table and the whole nest」——**放弃某张表的连接缓冲时，要连带把同一个嵌套里的内表一起放弃**。函数体分三种情形：

```
static void revise_cache_usage(JOIN_TAB *join_tab) {
  plan_idx first_inner = join_tab->first_inner();
  JOIN *const join = join_tab->join();
  if (first_inner != NO_PLAN_IDX) {
    plan_idx end_tab = join_tab->idx();
    for (first_inner = join_tab->first_inner(); first_inner != NO_PLAN_IDX;
         first_inner = join->best_ref[first_inner]->first_upper()) {
      for (plan_idx i = end_tab - 1; i >= first_inner; --i)
        join->best_ref[i]->set_use_join_cache(JOIN_CACHE::ALG_NONE);
      end_tab = first_inner;
    }
  } else if (join_tab->get_sj_strategy() == SJ_OPT_FIRST_MATCH) {
    plan_idx first_sj_inner = join_tab->first_sj_inner();
    for (plan_idx i = join_tab->idx() - 1; i >= first_sj_inner; --i) {
      JOIN_TAB *tab = join->best_ref[i];
      if (tab->first_sj_inner() == first_sj_inner)
        tab->set_use_join_cache(JOIN_CACHE::ALG_NONE);
    }
  } else
    join_tab->set_use_join_cache(JOIN_CACHE::ALG_NONE);
  assert(join->qep_tab == nullptr);
}
```

三种情形分别是：**外连接嵌套**（从本表往上逐层遍历嵌套，把每层内表全部清成 `ALG_NONE`）、**FirstMatch 半连接**（把同一半连接嵌套里其它表也清成 `ALG_NONE`）、**普通表**（只清自己）。

这条「全有或全无」的规则在 `setup_join_buffering()` 的文档注释里也被明确写出：「For a nested outer join/semi-join, currently, we either use join buffers for all inner tables or for none of them.」

### 从决策到执行

`setup_join_buffering()` 的结论落到 `tab->use_join_cache()` 之后，由 `sql/sql_select.cc` 的 `QEP_TAB::init_join_cache()` 翻译成执行器认识的操作类型：

```
switch (join_tab->use_join_cache()) {
  case JOIN_CACHE::ALG_BNL: op_type = QEP_TAB::OT_BNL; break;
  case JOIN_CACHE::ALG_BKA: op_type = QEP_TAB::OT_BKA; break;
  default: assert(0);
}
```

`sql/sql_executor.cc` 里有三个判定函数读这个 `op_type`：

- `UseHashJoin(QEP_TAB*)`：`return qep_tab->op_type == QEP_TAB::OT_BNL;`——**BNL 在执行器里被替换成 hash join**。`setup_join_buffering()` 的文档注释也印证了这一点：「Currently if blocked nested loop(BNL) is employed for join buffering, it is replaced by hash joins in the executor.」
- `UseBKA(QEP_TAB*)`：`op_type` 必须是 `OT_BKA`；并且如果这是一个外连接的 BKA、右侧不止一张表（`qep_tab->last_inner() != NO_PLAN_IDX && qep_tab->last_inner() != qep_tab->idx()`），就返回 false——源码注释说这种情形不是 left-deep 树，「we cannot handle at this point」。
- `QueryMixesOuterBKAAndBNL(JOIN*)`：注释解释了问题根源——「Having a non-BKA join on the right side of an outer BKA join causes problems for the matched-row signaling from MultiRangeRowIterator to BKAIterator」：行能找到，但没走 join filter，于是没被标记为已匹配，会多产生 NULL 行。命中时同时关掉 BKA 与 hash join。

BKA 的执行路径在 `sql/sql_executor.cc` 里是 `CreateBKAAccessPath()` 构造 `AccessPath::BKA_JOIN`，随后由 `sql/join_optimizer/access_path.cc` 在 `AccessPath::BKA_JOIN` 分支里 `NewIterator<BKAIterator>`，并把 `thd->variables.join_buff_size` 作为 `max_memory_available` 传给 `BKAIterator`。

`sql/iterators/bka_iterator.cc` 的 `BKAIterator::ReadOuterRows()` 展示了 BKA 的分批逻辑：外表的行被存进 `m_rows`；当「已用字节 + 当前行字节 + MRR 行缓冲需求」超过 `m_max_memory_available` 时，结束当前批次并把这一行留到下一批（`m_has_row_from_previous_batch`）。批次内 MRR 的行缓冲大小按 `mrr_buffer_size = m_mrr_bytes_needed_per_row * m_rows.size()` 估算，若预算不足则退到「至少能放一条内表记录」，预算有余时按 `std::min(mrr_buffer_size * 2 + 16384, m_max_memory_available - m_bytes_used)` 留出余量。外连接 / 反连接还需要 match flag，占 `BytesNeededForMatchFlags(rows)` 字节（每行一 bit）。

hash join 侧的内存预算同样用 `thd->variables.join_buff_size`，见 `sql/iterators/hash_join_iterator.cc` 与 `sql/join_optimizer/access_path.cc` 的 `AccessPath::HASH_JOIN` 分支；`sql/join_optimizer/join_optimizer.cc` 在估算 hash join 成本时用 `join_buff_size` 算出「缓冲区复用概率」：`buffer_usage = min(1.0, estimated_bytes_per_row * right_path->num_output_rows() / join_buff_size)`，复用概率为 `1.0 - buffer_usage * buffer_usage`。

### 一个必须点名的缺失符号

refs 里列出的 `set_join_buffer_properties` **在本仓库的这份源码检出里没有函数定义**。全仓检索该符号，唯一命中是 `sql/sql_optimizer.cc` 中 `setup_join_buffering()` 内 BKA 分支的一段 `@todo` 注释：

```
@todo: This choice to not use BKA should be done before making
cost estimates, e.g. in set_join_buffer_properties(). That
happens before cond guards are set up, so instead of doing the
check below, BKA should be disabled if
 - We are in an IN subquery, and
 - The IN predicate is not a top_level_item, and
 - The left_expr of the IN predicate may contain NULL values
   (left_expr->maybe_null)
```

因此本页只能断言：**源码注释声称「做成本估算（例如在 `set_join_buffer_properties()` 里）之前就应该决定不用 BKA」**；`set_join_buffer_properties` 本身的职责、签名与实现，在本次检出的文件集合中读不到，本页不对其行为作任何断言。

## 关键取舍

**取舍一：先排除、后定案的分层判定 vs. 一次性决策。** `setup_join_buffering()` 的文档注释写明了长期目标：「Long-term it is the goal that join buffering strategy is decided when the plan is selected.」现在之所以要等到计划定型后再判一次，是因为有些条件在更早的阶段无法判定——注释直接说「some join buffering decisions cannot currently be taken before this stage」。代价是同一件事要在两处各判一次：`sql/sql_planner.cc` 的 `best_access_path()` 里 `disable_jbuf` 判一次（第一条表、LATERAL 依赖、BNL 开关），`setup_join_buffering()` 里再判一次（LATERAL 依赖、BNL/BKA 开关等），两处通过源码注释「See similar code in best_access_path()」互相引用保持同步。

**取舍二：连接缓冲换取扫描次数，代价是内存与行序。** 用连接缓冲把内表的扫描次数从「外表行数次」降到「缓冲块数次」，代价是 `join_buff_size` 的内存占用与行序被打乱（`simple_order = simple_group = false`）。而且这个交换只在「缓冲装得下」时划算，所以成本估算里用 `buffer_count` 这个量来权衡：块数越多，省下的扫描越多。

**取舍三：宁可整层放弃，也不做半吊子的连接缓冲。** `revise_cache_usage()` 与 `setup_join_buffering()` 的六道外连接/半连接否决，说明了 MySQL 在这里的取舍是保守的：外连接的 NULL 补行需要 match flag，FirstMatch 也需要 match flag，而**一行在 cache 里只有一个 match flag 位**——与其引入复杂的 flag 复用逻辑，不如把整个嵌套的连接缓冲全部关掉。源码在 `setup_join_buffering()` 文档注释里把这个取舍写成了长期计划的前提：之所以现在还不能放宽，是因为优化器「只生成嵌套循环连接的计划，不生成 hash join的计划」，半连接策略的选择建立在 NLJ 的假设上；等「为 hash join 开发出成本模型、优化器也能生成 hash join 计划」之后，才可以放开更多场景。

**取舍四：对 secondary engine 放宽限制。** 两处否决（FirstMatch 下的多表半连接、FirstMatch + 外连接内表）都额外放开了 secondary engine。理由写在注释里：secondary engine 只认「FirstMatch」策略且只做表扫描，能解释的计划形态非常有限；MySQL 服务端则有多种半连接策略，且策略选择建立在 NLJ 假设上。

**取舍五：把 BKA 的可用性判定推到最后一刻。** 源码自己承认这是妥协（`@todo`）：BKA 需要 MRR 支持，而 MRR 的可用性又取决于存储引擎是否实现了 DS-MRR、是否命中 `choose_mrr_impl()` 的一长串否决条件。代价是**成本估算发生在决策之前**——`multi_range_read_info()` 返回的结果决定 BKA 能不能用，但成本已经算完了。这条 `@todo` 正是 `set_join_buffer_properties` 被引用的地方。

**取舍六：把「半连接策略」与「连接缓冲」互相绑死。** Materialize 策略下只有嵌套里的第一张表被禁用，其余表仍可用；LooseScan 下整个嵌套禁用；FirstMatch 下要求单内表且不能与外连接的 match flag 冲突；DUPS_WEEDOUT 则完全放开。这四档差异都源于一个共同事实：不同半连接策略对「内表行被访问几次」「匹配行如何记账」的要求不同。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/sql_optimizer.cc | `setup_join_buffering` | 核心决策函数：按开关、提示、`no_jbuf_after`、外连接/半连接嵌套、LATERAL 依赖与访问方式逐层排除，最终调 `tab->set_use_join_cache()` 定下 `ALG_NONE` / `ALG_BNL` / `ALG_BKA` |
| sql/sql_optimizer.cc | `set_join_buffer_properties` | 仅在 `setup_join_buffering()` 的 `@todo` 注释中被提及，本地检出中无函数定义，职责不可断言 |
| sql/sql_optimizer.cc | `revise_cache_usage` | `setup_join_buffering()` 的回退路径：放弃某表连接缓冲时，把同一外连接嵌套 / FirstMatch 半连接嵌套内的其它表一并清成 `ALG_NONE` |
| sql/sql_optimizer.cc | `JOIN::optimize` | 调用 `setup_join_buffering()` 的地方；据其结果把 `simple_order` / `simple_group` 置假 |
| sql/sql_join_buffer.h | `JOIN_CACHE::enum_join_cache_type` | 缓冲算法枚举：`ALG_NONE = 0`、`ALG_BNL = 1`、`ALG_BKA = 2` |
| sql/sql_planner.cc | `Optimize_table_order::best_access_path` | 计划阶段初判：写 `POSITION::use_join_buffer`，并计算 `disable_jbuf`（首表 / LATERAL 依赖 / BNL 开关） |
| sql/sql_planner.cc | `Optimize_table_order::calculate_scan_cost` | 用 `thd->variables.join_buff_size` 与 `cache_record_length()` 估算需要的缓冲块数 `buffer_count` |
| sql/sql_select.cc | `QEP_TAB::init_join_cache` | 把 `use_join_cache()` 翻译成执行器的 `QEP_TAB::OT_BNL` / `QEP_TAB::OT_BKA` |
| sql/sql_executor.cc | `UseHashJoin` | 判定 `op_type == OT_BNL`，即 BNL 在执行器中被替换成 hash join |
| sql/sql_executor.cc | `UseBKA` | 判定 BKA；外连接右侧不止一张表时返回 false（非 left-deep 树） |
| sql/sql_executor.cc | `QueryMixesOuterBKAAndBNL` | 检出「外 BKA 右侧混有非 BKA 连接」的冲突，命中时同时关闭 BKA 与 hash join |
| sql/sql_executor.cc | `CreateBKAAccessPath` | 构造 `AccessPath::BKA_JOIN`，填入 `mrr_length_per_rec` 与 `rec_per_key` |
| sql/iterators/bka_iterator.cc | `BKAIterator::ReadOuterRows` | BKA 的分批核心：攒外表行、按 `m_max_memory_available` 切批、决定 MRR 行缓冲大小 |
| sql/iterators/bka_iterator.cc | `MultiRangeRowIterator::MrrNextCallback` | 把缓冲中的外表行还原成 MRR range；用 `impossible_null_ref()` 跳过被 NULL 过滤掉的行 |
| sql/join_optimizer/access_path.cc | `AccessPath::BKA_JOIN` / `AccessPath::HASH_JOIN` 分支 | 实例化 `BKAIterator` / `HashJoinIterator`，把 `thd->variables.join_buff_size` 作为内存上限传入 |
| sql/handler.cc | `handler::multi_range_read_info` | 默认 MRR 实现：置 `HA_MRR_USE_DEFAULT_IMPL` 与 `HA_MRR_SUPPORT_SORTED`，返回 `0` |
| sql/handler.cc | `DsMrr_impl::choose_mrr_impl` | 决定用 DS-MRR 还是默认 MRR；含 100 MB 表大小阈值与「至少 50 条记录」的启发式 |
| sql/handler.cc | `DsMrr_impl::get_disk_sweep_mrr_cost` | 计算 DS-MRR 的缓冲大小与扫描轮数，缓冲放不下一个 rowid 即失败 |

## 与主核 / AI 的关系

**架构推论：** MySQL 服务端的连接缓冲与本仓 `engine/db/` 主核的多模态数据库内核没有直接对应关系——主核是自己从零写的存储 / 索引 / 查询内核，不复用 MySQL 的优化器与执行器，`engine/db/` 里不存在 BNL / BKA 这样的连接算法。但**「用内存预算换扫描次数」这一取舍形态在主核里是同构的**：主核的 `storage/heap.c` 与 `index/` 层同样面临「缓存多大、装不下怎么办」的问题。

**架构推论：** 从 MySQL 这份源码能直接提炼出三条对本仓有参考价值的设计模式，且都能对应到具体符号：

1. **决策与执行分离**：优化阶段只写标志（`POSITION::use_join_buffer`、`tab->use_join_cache()`），执行器再翻译成具体算子（`QEP_TAB::init_join_cache()`）。这让「计划」与「执行」可以各自演化，也便于解释输出（`EXPLAIN` 展示的是前者）。
2. **成对的一致性判定**：`best_access_path()` 与 `setup_join_buffering()` 判同一批条件，靠注释互指保持同步。**架构推论：** 主核若也把这类条件拆到多层判定，两处判定的同步成本会成为长期维护负担——这是可以直接借来做取舍参考的负面经验。
3. **否决优先于肯定**：连接缓冲的判定是「先列一堆否决条件，剩下来才允许用」，而不是「先判断鼓励条件，再排除」。**架构推论：** 这种结构让「为什么没用上」比「为什么用上了」更容易解释，也更容易在不改变既有行为的前提下逐条放宽。

**架构推论：** 对 AI 辅助的代码导航而言，本页这类页面的价值在于把「一个优化器开关如何影响执行形态」的因果链固化下来，避免 AI 只看到 `ALG_BNL` / `ALG_BKA` 两个枚举值却答不出它们各自的前置条件（例如「为什么这张表没用上 BKA」需要串起 `has_guarded_conds()`、`multi_range_read_info()` 的三个标志位、`choose_mrr_impl()` 的一长串否决条件）。这也解释了为什么 `refs` 要指向 `setup_join_buffering` 这类**决策函数**而不是只指向枚举定义。

## 参考

- MySQL 8.0 Reference Manual — Block Nested-Loop and Batched Key Access Join Algorithms: https://dev.mysql.com/doc/refman/8.0/en/join-algorithms-bnl-bka.html
- MySQL 8.0 Reference Manual — Optimizer Switch Flags and Hints: https://dev.mysql.com/doc/refman/8.0/en/optimizer-switch.html
- MySQL 8.0 Reference Manual — Controlling the Join Buffer Size with the join_buffer_size System Variable: https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html
- MySQL 8.0 Reference Manual — Switching Off the Block Nested-Loop and Batched Key Access Join Algorithms: https://dev.mysql.com/doc/refman/8.0/en/bnl-bka-optimization.html
- MySQL 源码：`sql/sql_optimizer.cc`（`setup_join_buffering` / `revise_cache_usage`）: https://github.com/mysql/mysql-server/blob/8.0/sql/sql_optimizer.cc
- MySQL 源码：`sql/sql_join_buffer.h`（`JOIN_CACHE::enum_join_cache_type`）: https://github.com/mysql/mysql-server/blob/8.0/sql/sql_join_buffer.h
- MySQL 源码：`sql/iterators/bka_iterator.cc`（`BKAIterator` / `MultiRangeRowIterator`）: https://github.com/mysql/mysql-server/blob/8.0/sql/iterators/bka_iterator.cc
- MySQL 源码：`sql/handler.cc`（`multi_range_read_info` / `DsMrr_impl::choose_mrr_impl`）: https://github.com/mysql/mysql-server/blob/8.0/sql/handler.cc
- MySQL 源码：`sql/sql_planner.cc`（`best_access_path` / `calculate_scan_cost`）: https://github.com/mysql/mysql-server/blob/8.0/sql/sql_planner.cc
