---
project: mysql-server
feature: btr-cursor-search
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/btr/btr0cur.cc
    symbols: [btr_cur_search_to_nth_level, btr_cur_search_to_nth_level_with_no_latch]
---

# B-tree 游标定位

## 一句话职责

`storage/innobase/btr/btr0cur.cc` 的 `btr_cur_search_to_nth_level()` 负责把一个「待查元组 + 查找模式」在 InnoDB 的 B-tree 上定位成一颗游标（`btr_cur_t`）：从根页开始逐层下降，每层用页内二分查找把候选记录收敛到一条，再顺着这条记录的 node pointer 走到下一层，直到到达调用方指定的层（`level == 0` 即叶层）；同时它还负责在下降路径上加锁与释放锁、在叶层按模式钉住正确的页与记录、把「匹配了多少个字段」的结果（`up_match` / `low_match`）交回给调用方。

## 机制是什么

**它是一个「带方向控制的树下降」，不是一次纯粹的比较。** 同一个 `btr_cur_search_to_nth_level()` 要同时满足四类互相牵制的需求：

1. **定位**——把游标放到「按 B-tree 顺序，tuple 应该插入/查找的位置」。函数头的注释写明这一点：`PAGE_CUR_LE` 模式下 `cursor->up_match` 与 `cursor->low_match` 都有意义值；`PAGE_CUR_GE` 模式下只有 `up_match` 有意义值；`PAGE_CUR_LE` 时游标停在「插入 tuple 应该发生的地方」，InnoDB 紧接着就在游标之后插入。

2. **页级匹配的方向控制**——`mode` 决定在**非叶层**用什么比较模式、在**叶层**用什么比较模式。函数体里有一段显式的模式改写：

   ```
   switch (mode) {
     case PAGE_CUR_GE:
       page_mode = PAGE_CUR_L;
       break;
     case PAGE_CUR_G:
       page_mode = PAGE_CUR_LE;
       break;
     default:
       page_mode = mode;
       break;
   }
   ```

   源码注释解释：非叶层用这些改写后的模式，好让我们落到正确的叶子上；到了那个叶子上，我们用**原始** `mode`。函数体在 `height == 0`（已经到叶层）时执行 `page_mode = mode;` 把模式改回来。

3. **树结构的写意图**——`latch_mode` 之外还携带一组「操作意图」标志（`BTR_INSERT` / `BTR_DELETE` / `BTR_DELETE_MARK` / `BTR_ESTIMATE`），以及树锁意图（`BTR_LATCH_FOR_INSERT` / `BTR_LATCH_FOR_DELETE`）。这些标志决定下降过程中要不要把上层页的锁一直攥着，以及要不要在页放不下时把插入/删除**缓冲到 change buffer**。

4. **锁的进退**——下降的每一层按 `rw_latch` 加页锁，函数体必须在合适的时机释放上层页锁，否则并发会被自己阻塞死。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责在 B-tree 上按比较函数定位游标位置（自适应哈希索引除外）、以及页级匹配的方向控制；**不负责页内记录比较（rem0cmp）**、**不负责缓冲池的页获取（H 组）**。

## 怎么实现的

### 主函数 `btr_cur_search_to_nth_level()` 的骨架

函数体从 `page_t *page = nullptr;` 开始，声明了本机制要用的全部状态：`buf_block_t *tree_blocks[BTR_MAX_LEVELS]` 与 `ulint tree_savepoints[BTR_MAX_LEVELS]`（下降路径上每一层的页块与 mtr savepoint）、`n_blocks` / `n_releases`（已加锁层数与已释放层数）、`up_match` / `up_bytes` / `low_match` / `low_bytes`（匹配字段数与字节数）。

进入下降之前，函数体先做了四组参数解析：

**（1）操作类型。** 函数体把 `latch_mode` 里的 `BTR_INSERT | BTR_DELETE | BTR_DELETE_MARK` 三者拆开，映射成内部的 `btr_op` 枚举：`BTR_INSERT_OP`（`BTR_INSERT`）、`BTR_INSERT_IGNORE_UNIQUE_OP`（`BTR_INSERT` 且带 `BTR_IGNORE_SEC_UNIQUE`）、`BTR_DELETE_OP`（`BTR_DELETE`）、`BTR_DELMARK_OP`（`BTR_DELETE_MARK`）、`BTR_NO_OP`（都没带）。`switch` 的 `default` 分支是 `ut_error`——源码注释写明「`BTR_INSERT`、`BTR_DELETE`、`BTR_DELETE_MARK` 一次只能指定一个」。

**（2）操作前置约束。** 紧接着四条断言：change buffer 树上的操作不能被缓冲（`btr_op == BTR_NO_OP || !dict_index_is_ibuf(index)`）；聚簇索引上的操作不能被缓冲；临时表的操作不能被缓冲；空间索引的操作不能被缓冲。

**（3）树锁意图与标志剥离。** `lock_intention = btr_cur_get_and_clear_intention(&latch_mode);` 把 `BTR_LATCH_FOR_INSERT` / `BTR_LATCH_FOR_DELETE` 摘出来变成 `BTR_INTENTION_INSERT` / `BTR_INTENTION_DELETE`，两者都没有或都有时是 `BTR_INTENTION_BOTH`；然后 `latch_mode = BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);` 把与加锁模式无关的标志全部剥掉，只留纯加锁模式。

**（4）游标初始化。** `cursor->flag = BTR_CUR_BINARY;` 与 `cursor->index = index;`——注意这个 flag 的含义：本次是**二分查找**下来的，不是自适应哈希索引命中的。`up_match` / `up_bytes` / `low_match` / `low_bytes` 四个字段先用 `UNIV_MEM_INVALID` 置为无效。

### 第一步：先试自适应哈希索引，失败才走树下降

函数体在做任何取页动作之前，先判断能不能走 AHI 快路径：

```
if (rw_lock_get_writer(btr_get_search_latch(index)) == RW_LOCK_NOT_LOCKED &&
    latch_mode <= BTR_MODIFY_LEAF && index->search_info->last_hash_succ &&
    !index->disable_ahi && !estimate
    && mode != PAGE_CUR_LE_OR_EXTENDS
    && !dict_index_is_spatial(index)
    && UNIV_LIKELY(btr_search_enabled) && !modify_external &&
    btr_search_guess_on_hash(tuple, mode, latch_mode, cursor,
                             has_search_latch, mtr)) {
  btr_cur_n_sea++;
  return;
}
btr_cur_n_non_sea++;
```

这段的判据是**一串合取**，逐条都有出处：`rw_lock_get_writer(btr_get_search_latch(index)) == RW_LOCK_NOT_LOCKED` 表示没有写者等在搜索闩上；`index->search_info->last_hash_succ` 表示上一次 AHI 猜测成功过；`!index->disable_ahi` 是索引级的 AHI 开关；`!estimate` 表示统计估算路径不走 AHI；`!dict_index_is_spatial(index)` 因为 R-tree 的 MBR 比较与 AHI 不兼容；`btr_search_enabled` 是全局开关。

命中则 `btr_cur_n_sea++` 并**直接 return**——走 AHI 就完全不进树下降。未命中则 `btr_cur_n_non_sea++`。这两个计数器是 InnoDB monitor 的 `btr_cur_n_sea` / `btr_cur_n_non_sea` 统计项，源码注释写明「`btr_cur_search_to_nth_level()` 中沿 B-tree 下降的搜索次数」与「成功的自适应哈希索引查找次数」。

**这就是本机制边界里说的「自适应哈希索引除外」的落点：AHI 命中时，定位根本不走这个文件里的树下降逻辑。**

### 第二步：释放搜索闩、拿树锁、定根页

AHI 未命中后，函数体先释放搜索闩以遵守加锁顺序：

```
if (has_search_latch) {
  rw_lock_s_unlock(btr_get_search_latch(index));
}
savepoint = mtr_set_savepoint(mtr);
```

然后是一个按 `latch_mode` 分支的 `switch`，决定拿哪种树锁：

- `BTR_MODIFY_TREE`：先判 `lock_intention == BTR_INTENTION_DELETE && trx_sys->rseg_history_len.load() > BTR_CUR_FINE_HISTORY_LENGTH && buf_get_n_pending_read_ios()`，三者同时成立才 `mtr_x_lock(dict_index_get_lock(index), ...)`；否则（空间索引且 `lock_intention <= BTR_INTENTION_BOTH` 时）也拿 X 锁；其余情况 `mtr_sx_lock(...)`。`BTR_CUR_FINE_HISTORY_LENGTH` 是文件内定义的 `constexpr uint32_t`，值为 `100000`，源码注释说明「实验上历史链表长度从大约 100000 开始明显影响吞吐」。
- `BTR_CONT_MODIFY_TREE` / `BTR_CONT_SEARCH_TREE`：什么都不做（锁由调用方持有），只有 R-tree 的 `BTR_CONT_MODIFY_TREE` 会把 `upper_rw_latch` 设为 `RW_X_LATCH`，其余是 `RW_NO_LATCH`。
- `default`（普通搜索）：调用方已通过 `BTR_ALREADY_S_LATCHED` 标记拿了锁时只做断言；否则 `mtr_s_lock(...)` 拿 S 锁（`BTR_MODIFY_EXTERNAL` 时拿 SX 锁），`upper_rw_latch = RW_S_LATCH`；`srv_read_only_mode` 时不拿锁，`upper_rw_latch = RW_NO_LATCH`。

接着 `root_leaf_rw_latch = btr_cur_latch_for_root_leaf(latch_mode);`——这个辅助函数把加锁模式映射成「根页兼作叶页时该用什么锁」：`BTR_SEARCH_LEAF` / `BTR_SEARCH_TREE` / `BTR_SEARCH_PREV` → `RW_S_LATCH`，`BTR_MODIFY_LEAF` / `BTR_MODIFY_TREE` / `BTR_MODIFY_PREV` → `RW_X_LATCH`，`BTR_CONT_MODIFY_TREE` / `BTR_CONT_SEARCH_TREE` / `BTR_NO_LATCHES` → `RW_NO_LATCH`（注释：根页应已被加锁）。

最后定位起点：`page_id_t page_id(space, dict_index_get_page(index));`——**从根页开始**。如果 `root_leaf_rw_latch == RW_X_LATCH`，还要把 `node_ptr_max_size` 从默认的 `UNIV_PAGE_SIZE / 2` 换成 `dict_index_node_ptr_max_size(index)`。

### 第三步：主循环 `search_loop`

主循环标号 `search_loop:` 的结构是「取页 → 判断层级 → 页内比较 → 决定下一层」：

**取页。** 每次迭代都调 `buf_page_get_gen(page_id, page_size, rw_latch, ..., fetch, {file, line}, mtr)`。这里有三条与本机制直接相关的细节：

- 根页那一轮传的是 `index->search_info->root_guess` 作为 hint——**上一次下降时记下的根页块**（函数体在识别出根节点时执行 `index->search_info->root_guess = block;`），这是避免重复哈希查找根页的优化。
- `rw_latch` 的取值随高度和模式变化：非叶层一般取 `upper_rw_latch`；`BTR_MODIFY_EXTERNAL` 且是根页时取 `RW_SX_LATCH`；叶层且 `latch_mode <= BTR_MODIFY_LEAF` 时 `rw_latch = latch_mode`（即 `BTR_SEARCH_LEAF` / `BTR_MODIFY_LEAF` 直接当锁类型用）。
- `fetch` 的取值：当 `btr_op != BTR_NO_OP` 且 `ibuf_should_try(index, btr_op != BTR_INSERT_OP)` 成立时，`fetch` 变成 `Page_fetch::IF_IN_POOL`（删除则是 `Page_fetch::IF_IN_POOL_OR_WATCH`），也就是**先试 change buffer，页不在缓冲池就不去读盘**。

**取页失败的分支（change buffer 路径）。** `block == nullptr` 时函数体断言 `height == 0`，然后按 `btr_op` 分四种情况：

- `BTR_INSERT_OP` / `BTR_INSERT_IGNORE_UNIQUE_OP`：调 `ibuf_insert(IBUF_OP_INSERT, tuple, index, page_id, page_size, cursor->thr)`，成功则置 `cursor->flag = BTR_CUR_INSERT_TO_IBUF` 并 `goto func_exit`——**插入被缓冲了，本层搜索到此结束**。
- `BTR_DELMARK_OP`：调 `ibuf_insert(IBUF_OP_DELETE_MARK, ...)`，成功则置 `cursor->flag = BTR_CUR_DEL_MARK_IBUF` 并 `goto func_exit`。
- `BTR_DELETE_OP`：先调 `row_purge_poss_sec(cursor->purge_node, index, tuple)`，返回假说明「这条记录还不能 purge」，置 `cursor->flag = BTR_CUR_DELETE_REF`；返回真则调 `ibuf_insert(IBUF_OP_DELETE, ...)`，成功置 `cursor->flag = BTR_CUR_DELETE_IBUF`。两条路径最后都 `buf_pool_watch_unset(page_id);` 并 `goto func_exit`。
- `default`：`ut_error`。

缓冲失败时，函数体把 `fetch = cursor->m_fetch_mode;` 恢复成正常取值，然后 `goto retry_page_get;` **重新取页**——这次是真的从磁盘读。

**页内比较。** 这是本机制与 rem0cmp 的接口点。函数体有三个互斥分支：

```
if (dict_index_is_spatial(index) && page_mode >= PAGE_CUR_CONTAIN) {
  found = rtr_cur_search_with_match(block, index, tuple, page_mode,
                                    page_cursor, cursor->rtr_info);
  ...
} else if (height == 0 && btr_search_enabled &&
           !dict_index_is_spatial(index)) {
  page_cur_search_with_match_bytes(block, index, tuple, page_mode, &up_match,
                                   &up_bytes, &low_match, &low_bytes,
                                   page_cursor);
} else {
  up_bytes = low_bytes = 0;
  page_cur_search_with_match(block, index, tuple, page_mode, &up_match,
                             &low_match, page_cursor,
                             need_path ? cursor->rtr_info : nullptr);
}
```

三个分支的选择依据是明确的：

- **R-tree 且模式 ≥ `PAGE_CUR_CONTAIN`**：走 `rtr_cur_search_with_match()`，按 MBR 语义定位，并维护父路径栈 `cursor->rtr_info`。
- **叶层 + AHI 开启 + 非空间索引**：走 `page_cur_search_with_match_bytes()`——注释写明「自适应哈希索引只在搜索叶页时使用（`height == 0`），但 R-tree 不用。我们只需要字节前缀比较来更新自适应哈希索引」。这一支会额外产出 `up_bytes` / `low_bytes`。
- **其余（普通非叶层、空间索引的叶层插入等）**：走 `page_cur_search_with_match()`，比较全部索引字段，并把 `up_bytes = low_bytes = 0;` 显式清零。

页内二分查找本身在 `storage/innobase/page/page0cur.cc` 的 `page_cur_search_with_match()` 里：先在页目录槽位上二分（`low = 0; up = page_dir_get_n_slots(page) - 1;`，循环条件 `up - low > 1`），再在上下限槽位所属的记录链上线性扫。方向由 `mode` 决定：命中时 `mode == PAGE_CUR_G || mode == PAGE_CUR_LE` 走 `low_slot_match`（往下界收），否则走 `up_slot_match`（往上界收）；最终 `if (mode <= PAGE_CUR_GE) page_cur_position(up_rec, block, cursor); else page_cur_position(low_rec, block, cursor);`。

**写意图与「重试」。** 这是本机制最复杂的部分。下降到非目标层时，函数体拿到 node pointer 后要做三件事：

**（1）取 node pointer 的字段偏移。** `offsets = rec_get_offsets(node_ptr, index, offsets, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);`。

**（2）判断是否需要「改变意图后重来」。** 条件是 `latch_mode == BTR_MODIFY_TREE && btr_cur_need_opposite_intention(page, lock_intention, node_ptr)`。`btr_cur_need_opposite_intention()` 的函数体按意图分三种：`BTR_INTENTION_DELETE` 时，若该记录是页内第一条且页有前驱，或该记录是页内最后一条且页有后继，则返回真；`BTR_INTENTION_INSERT` 时，若该记录是页内最后一条且页有后继，则返回真；`BTR_INTENTION_BOTH` 恒返回假。源码注释说明原因：悲观删除页首记录会在上层引发 node pointer 的插入。

命中后函数体走 `need_opposite_intention:` 标签：释放所有已加锁的块、把 `lock_intention` 改成 `BTR_INTENTION_BOTH`、`page_id.reset(space, dict_index_get_page(index));` 回到根页、`height = ULINT_UNDEFINED;`、`n_blocks = 0; n_releases = 0;`，然后 `goto search_loop;`——**整棵树重来一遍，但这次用更保守的意图**。

叶层也有一条同样的重试路径：函数体在 `level != height` 的 `else` 分支里判断 `!dict_index_is_spatial(index) && latch_mode == BTR_MODIFY_TREE && lock_intention == BTR_INTENTION_INSERT && mach_read_from_4(page + FIL_PAGE_NEXT) != FIL_NULL && page_rec_is_last(page_cur_get_rec(page_cursor), page)`，命中则先释放三个已加锁的叶页，再 `goto need_opposite_intention;`。源码注释写明原因：`btr_insert_into_right_sibling()` 可能在上一层删掉 node pointer。

**（3）判断是否还攥着上层页锁。** 条件是 `!detected_same_key_root && latch_mode == BTR_MODIFY_TREE && !btr_cur_will_modify_tree(index, page, lock_intention, node_ptr, node_ptr_max_size, page_size, mtr) && !rtree_parent_modified`。`btr_cur_will_modify_tree()` 的函数体很长，但逻辑是「这次操作会不会改动树结构」：

- 删除侧（`lock_intention <= BTR_INTENTION_BOTH`）：当 `lock_intention == BTR_INTENTION_BOTH` 时先算 `max_nodes_deleted`——`level > 7` 时取 `64`（源码注释标了 `TODO: adjust this practical limit`），`level > 0` 时取 `(ulint)1 << (level - 1)`；然后若 `page_get_n_recs(page) <= max_nodes_deleted * 2 || page_rec_is_first(rec, page)` 就判定会改树。再判两条「距离」条件：`page_rec_distance_is_at_most(page_get_infimum_rec(page), rec, max_nodes_deleted)` 与 `page_rec_distance_is_at_most(rec, page_get_supremum_rec(page), max_nodes_deleted)`。删除侧的 `margin` 在 `BTR_INTENTION_BOTH` 下是 `rec_size * max_nodes_deleted`，在 `BTR_INTENTION_DELETE` 下是 `rec_size`；最后判 `page_get_data_size(page) < margin + BTR_CUR_PAGE_COMPRESS_LIMIT(index)`，或该页既无后继也无前驱。
- 插入侧（`lock_intention >= BTR_INTENTION_BOTH`）：`LIMIT_OPTIMISTIC_INSERT_DEBUG(page_get_n_recs(page), return (true);)` 是调试期的条数上限；然后 `ulint max_size = page_get_max_insert_size_after_reorganize(page, 2);`，判 `max_size < BTR_CUR_PAGE_REORGANIZE_LIMIT + rec_size || max_size < rec_size * 2`。`BTR_CUR_PAGE_REORGANIZE_LIMIT` 是文件内宏 `#define BTR_CUR_PAGE_REORGANIZE_LIMIT (UNIV_PAGE_SIZE / 32)`。压缩页另有一条判 `page_zip_empty_size(index->n_fields, page_size.physical()) < rec_size * 2 + page_get_data_size(page) + page_dir_calc_reserved_space(page_get_n_recs(page) + 2) + 1`。

判定为「不会改树」时，函数体才释放上层块（`mtr_release_block_at_savepoint(mtr, tree_savepoints[n_releases], tree_blocks[n_releases]);`），但**根页永远不释放**——循环体里 `if (n_releases == 0) { continue; }`，源码注释写明「我们应该不释放根页，以固定到同一个块」。

**（4）同键根检测。** 在 `lock_intention == BTR_INTENTION_BOTH` 且索引非唯一、`latch_mode == BTR_MODIFY_TREE`、且 `up_match >= rec_offs_n_fields(offsets) - 1 || low_match >= rec_offs_n_fields(offsets) - 1` 时，函数体用 `cmp_rec_rec_with_match()` 拿当前 node pointer 与该页的第一条记录、最后一条记录逐字段比较，命中 `matched_fields >= rec_offs_n_fields(offsets) - 1` 就置 `detected_same_key_root = true`。源码注释解释：首尾记录或同键值时，`BTR_CONT_MODIFY_TREE` 下可能选中另一页，所以父页不能释放，否则会与另一个同键搜索死锁。

### 第四步：向左重试（`BTR_SEARCH_PREV` / `BTR_MODIFY_PREV`）

这是本机制里唯一的「向上回退重跑」路径。当加锁模式是 `BTR_SEARCH_PREV` 或 `BTR_MODIFY_PREV` 时，函数体在下降过程中检查「当前 node pointer 是不是所在页的最左记录」——判据是 `btr_page_get_prev(page, mtr) != FIL_NULL && page_rec_is_first(node_ptr, page)`。若是，则记 `leftmost_from_level = height + 1;`。

**降到叶层且 `leftmost_from_level > 0` 时**，函数体做一次「回到左边界那一层重来」：分配 `prev_tree_blocks` / `prev_tree_savepoints`（大小都是 `leftmost_from_level`），把 `page_id` 重置回 `leftmost_from_level` 那一层对应的页号，释放那之后的块，`n_blocks -= (leftmost_from_level - 1);`，`height = leftmost_from_level;`，然后**重放一遍页内比较**（对保留下来的每一层块重新调 `page_cur_search_with_match()` 并累加 `up_match` / `low_match`），最后 `goto search_loop;`。

源码注释说明动机：`BTR_SEARCH_PREV` 与 `BTR_MODIFY_PREV` 会去 latch 叶页的前驱页，所以如果游标落在页首就必须把前驱页一起拿住。

### 第五步：到达目标层，收尾

`level == height` 时退出下降。函数体按 `level != 0` 分两路：

- **`level != 0`（停在非叶层）**：若 `upper_rw_latch == RW_NO_LATCH`，按模式取子页——`BTR_CONT_MODIFY_TREE` 用 `btr_block_get(page_id, page_size, RW_X_LATCH, ...)`，`BTR_CONT_SEARCH_TREE` 用 `RW_SX_LATCH`，并调 `btr_assert_not_corrupted(child_block, index);`。若 `page_mode <= PAGE_CUR_LE`，把 `cursor->low_match = low_match; cursor->up_match = up_match;` 交回调用方。
- **`level == 0`（停在叶层）**：把四个匹配计数全部交回：`cursor->low_match = low_match; cursor->low_bytes = low_bytes; cursor->up_match = up_match; cursor->up_bytes = up_bytes;`。然后若 `btr_search_enabled && !index->disable_ahi`，调 `btr_search_info_update(cursor);`——**用这次搜索的结果去更新 AHI 的统计**（源码注释明确说这里是「对 `btr_search_enabled` 的脏读」，真正建哈希索引前会在 `btr_search_build_page_hash_index()` 里再加锁复查一次）。

叶层还有一条顺序性的收尾：函数体在 `height == 0` 分支里按 `latch_mode` 决定是否释放树锁与上层块——`BTR_MODIFY_TREE` / `BTR_CONT_MODIFY_TREE` / `BTR_CONT_SEARCH_TREE` 三个模式**不释放**（树锁还要用），其余模式调 `mtr_release_s_latch_at_savepoint(mtr, savepoint, dict_index_get_lock(index));` 释放树 S 锁，并逐层 `mtr_release_block_at_savepoint()` 释放上层块。`BTR_MODIFY_EXTERNAL` 时根页要保留（源码注释：「keep latch of root page」）。

`func_exit:` 标签处做三件收尾：`mem_heap_free(heap)` 释放临时堆；`retrying_for_search_prev` 时 `ut::free(prev_tree_blocks); ut::free(prev_tree_savepoints);`；`has_search_latch` 时把搜索闩**重新拿回来**（`rw_lock_s_lock(btr_get_search_latch(index), UT_LOCATION_HERE);`）——这与函数开头「释放搜索闩以遵守加锁顺序」形成一对。

### 第六步：估算路径（`BTR_ESTIMATE`）

`btr_estimate_n_rows_in_range_low()` 用两次带 `BTR_ESTIMATE` 的搜索来估算区间行数。函数体里两次调用分别是：

```
btr_cur_search_to_nth_level(index, 0, tuple1, mode1,
                            BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, 0,
                            __FILE__, __LINE__, &mtr);
```

和

```
btr_cur_search_to_nth_level(index, 0, tuple2, mode2,
                            BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, 0,
                            __FILE__, __LINE__, &mtr);
```

`BTR_ESTIMATE` 在本机制里有两个作用：一是**绕开 AHI**（`!estimate` 是 AHI 快路径的判据之一）；二是让函数体在页内比较之后调 `btr_cur_add_path_info(cursor, height, root_height);`——把下降路径记进 `cursor->path_arr`，供后续估算用。

`btr_cur_add_path_info()` 的函数体：若 `root_height >= BTR_PATH_ARRAY_N_SLOTS - 1` 就直接返回空路径（把 `slot->nth_rec` 置 `ULINT_UNDEFINED`）；否则在 `height == 0` 时给 `cursor->path_arr + root_height + 1` 标记路径结束，再把当前记录的页号 `page_no`、页内记录数 `n_recs`、页层级 `page_level`、记录序号 `nth_rec` 填进 `cursor->path_arr + (root_height - height)` 这个槽位。

### 兄弟函数 `btr_cur_search_to_nth_level_with_no_latch()`

这是同一个机制的「无锁版」，函数体比主函数短得多，差别集中在四处：

1. **前置断言不同**：`ut_ad(index->table->is_intrinsic());`——**只允许内部临时表（intrinsic table）使用**，因为这类表不共享、不需要加锁。
2. **没有树锁、没有 change buffer、没有意图判定**：没有 `lock_intention`、没有 `btr_op`、没有 `ibuf_insert` 分支、没有 `btr_cur_need_opposite_intention` / `btr_cur_will_modify_tree` 的重试。
3. **加锁模式固定为 `RW_NO_LATCH`**：`rw_latch = RW_NO_LATCH;` 后直接调 `buf_page_get_gen(page_id, page_size, rw_latch, nullptr, fetch, {file, line}, mtr, mark_dirty);`——注意它多传了 `mark_dirty` 参数。
4. **比较永远用完整字段**：`page_cur_search_with_match(block, index, tuple, page_mode, &up_match, &low_match, page_cursor, nullptr);`——没有 `_bytes` 变体，也没有 R-tree 分支。

它的循环是一个 `while (!at_desired_level)`，退出条件是 `height == level`；下降时同样用 `btr_node_ptr_get_child_page_no(node_ptr, offsets)` 取下一层页号。函数体在叶层也会把 `page_mode = mode;` 切回原始模式，语义与主函数一致。

**这个兄弟函数的真实调用方**（本地检出中可见）：

- `storage/innobase/row/row0ins.cc` 的 `row_ins_sec_index_entry_low()`：表是 intrinsic 时走 `btr_cur_search_to_nth_level_with_no_latch(index, 0, entry, PAGE_CUR_LE, &cursor, __FILE__, __LINE__, &mtr);`，否则走 `btr_cur_search_to_nth_level(index, 0, entry, PAGE_CUR_LE, search_mode, &cursor, 0, __FILE__, __LINE__, &mtr);`。
- `storage/innobase/row/row0ins.cc` 里唯一二级索引的重复键扫描之后，会**再搜一次**来重新定位：`btr_cur_search_to_nth_level_with_no_latch(...)` 或 `btr_cur_search_to_nth_level(...)`——源码注释写明「现在我们没有发现重复项，并且已经用 s 锁锁住了必要的记录来防止其他事务插入重复项。让我们重新定位游标并继续插入」。
- `storage/innobase/row/row0mysql.cc` 的 `row_mysql_convert_row_to_innobase()`：表是 intrinsic 时用无锁版。
- `storage/innobase/btr/btr0btr.cc` 的 `btr_insert_on_non_leaf_level()`：非空间索引且表是 intrinsic 时用无锁版，否则用 `btr_cur_search_to_nth_level(index, level, tuple, PAGE_CUR_LE, BTR_CONT_MODIFY_TREE, ...)`；空间索引用 `PAGE_CUR_RTREE_INSERT` 模式。

### 真实调用方清单

本地检出中实际调用 `btr_cur_search_to_nth_level()` 的地方：

| 调用点 | 加锁模式 / 查找模式 | 用途 |
|---|---|---|
| `storage/innobase/row/row0ins.cc` `row_ins_sec_index_entry_low()` | `search_mode` + `PAGE_CUR_LE`；R-tree 用 `PAGE_CUR_RTREE_INSERT` | 唯一二级索引插入前定位 |
| `storage/innobase/row/row0log.cc` `row_log_table_low()` | `has_index_lock ? BTR_MODIFY_TREE : BTR_MODIFY_LEAF` + `PAGE_CUR_LE` | 在线 DDL 的行日志回放；乐观删除失败后**回退到 `BTR_MODIFY_TREE` 重新搜一次** |
| `storage/innobase/dict/dict0dict.cc` `DDTableBuffer::get()` | `BTR_SEARCH_LEAF` + `PAGE_CUR_LE` | 读动态表元数据 |
| `storage/innobase/btr/btr0btr.cc` `btr_page_get_father_node_ptr_func()` | `BTR_CONT_MODIFY_TREE` 或 `BTR_CONT_SEARCH_TREE` + `PAGE_CUR_LE` | 向上一层找父页的 node pointer |
| `storage/innobase/btr/btr0btr.cc` `btr_insert_on_non_leaf_level()` | `BTR_CONT_MODIFY_TREE` + `PAGE_CUR_LE` | 向非叶层插入 |
| `storage/innobase/btr/btr0cur.cc` `btr_estimate_n_rows_in_range_low()` | `BTR_SEARCH_LEAF \| BTR_ESTIMATE` | 区间行数估算的两次边界搜索 |

**读路径不直接调本函数**，而是经由 `btr_pcur` 定位：`storage/innobase/row/row0sel.cc` 的 `row_sel_open_pcur()` 调 `plan->pcur.open_no_init(index, plan->tuple, plan->mode, BTR_SEARCH_LEAF, 0, &mtr, ...)`，同文件的 `row_sel_test_other_conds()` 则调 `plan->clust_pcur.open_no_init(index, plan->clust_ref, PAGE_CUR_LE, ...)` 打开聚簇索引游标——两者都由 `btr_pcur` 内部转成本机制。

### 错误分支与降级路径

本机制的错误面不靠返回码，而靠**游标 flag** 与**断言**：

- `cursor->flag` 的取值就是「这次定位发生了什么」：`BTR_CUR_BINARY`（树下降）、`BTR_CUR_HASH`（AHI 命中，由 `btr_search_guess_on_hash()` 设置）、`BTR_CUR_HASH_NOT_ATTEMPTED`（AHI 尝试但未命中）、`BTR_CUR_INSERT_TO_IBUF`（插入已进 change buffer）、`BTR_CUR_DEL_MARK_IBUF`（删除标记已进 change buffer）、`BTR_CUR_DELETE_REF`（记录还不能 purge）、`BTR_CUR_DELETE_IBUF`（purge 已进 change buffer）。调用方正是靠读这些 flag 决定后续行为——`row_ins_sec_index_entry_low()` 的函数体里就是 `if (cursor.flag == BTR_CUR_INSERT_TO_IBUF) { /* 插入在搜索期间被缓冲了，到此为止 */ goto func_exit; }`。
- **AHI 命中后校验失败会退回树下降**：`btr_search_guess_on_hash()` 内部调 `btr_search_check_guess()` 校验猜测的位置，`btr_search_check_guess()` 的函数体在 `index->space != block->page.id.space() || index->id != btr_page_get_index_id(block->frame) || !btr_search_check_guess(...)` 时返回 `false`——`btr_search_guess_on_hash()` 因此返回 `false`，本机制就继续走 `btr_cur_n_non_sea++` 之后的树下降路径。
- **空间索引的父 MBR 需要更新时会退回重做**：`search_mode == PAGE_CUR_RTREE_INSERT && cursor->rtr_info->mbr_adj` 时，若当前是 `BTR_MODIFY_LEAF` 就 `goto func_exit;`（源码注释：「父 MBR 需要更新，应该用 `BTR_MODIFY_TREE` 重试」）；若已是 `BTR_MODIFY_TREE` 则置 `rtree_parent_modified = true`。
- **`ut_error` 是不可恢复的硬失败点**：`btr_cur_get_and_clear_intention()` 之后的操作类型 `switch` 的 `default`、`btr_cur_latch_for_root_leaf()` 的末尾、`btr_cur_need_opposite_intention()` 的 `BTR_INTENTION_BOTH` 之外的分支、以及取页失败分支的 `default`，都是 `ut_error`。

## 关键取舍

- **源码事实：AHI 与树下降是「先试快的，失败再走准的」的两级结构，而不是二选一。** 本机制的函数体在下降前先试 `btr_search_guess_on_hash()`，命中就直接 `return`。收益是重复访问热点行时省掉整棵树的下降（这就是 InnoDB 的 AHI 存在的理由）；代价是每次搜索都要多付一次哈希查找与一次猜测校验（`btr_search_check_guess()` 会额外比较游标记录的前后邻居记录），而且 AHI 猜测必须被**验证**过——`btr_search_guess_on_hash()` 的函数体在 `index->space != block->page.id.space() || index->id != btr_page_get_index_id(block->frame)` 时直接放弃猜测。

- **源码事实：非叶层用「放宽」的比较模式、叶层用「精确」的比较模式，这是一套刻意的方向控制。** `PAGE_CUR_GE` 在非叶层被改写成 `PAGE_CUR_L`，`PAGE_CUR_G` 被改写成 `PAGE_CUR_LE`，到了叶层再 `page_mode = mode;` 改回来。收益是中间层只保证「落到包含目标区间的子树」，最后一层才做精确判定，中间层的比较开销更小；代价是调用方必须自己区分「中间层的匹配数」和「叶层的匹配数」——函数体因此在 `level != 0` 时只在 `page_mode <= PAGE_CUR_LE` 的条件下才把 `up_match` / `low_match` 交回调用方。

- **源码事实：匹配度是「匹配了几个字段」而不是「匹配了还是没匹配」。** `up_match` / `low_match` 是字段计数，调用方用它区分「完全相同的唯一键」与「只有前缀相同的键」。`storage/innobase/row/row0log.cc` 的 `row_log_table_low()` 用 `cursor.low_match >= dict_index_get_n_unique(index)` 判定「命中了一条匹配记录」，再用 `cursor.low_match == dict_index_get_n_fields(index)` 区分「完全相同」与「唯一键相同但主键不同」。`storage/innobase/dict/dict0dict.cc` 的 `DDTableBuffer::get()` 用 `cursor.low_match == dtuple_get_n_fields(m_search_tuple)` 判定命中。收益是唯一索引的重复键检查不需要二次比较；代价是调用方必须知道「唯一键字段数」与「索引总字段数」这两个不同的分母（`dict_index_get_n_unique(index)` 与 `dict_index_get_n_fields(index)`）。

- **源码事实：写路径的「保守重试」会把一次树下降变成两遍。** `btr_cur_need_opposite_intention()` 命中时，函数体释放所有块、把意图改成 `BTR_INTENTION_BOTH`、回到根页重来。收益是不用在一开始就拿最重的锁，避免长事务把整棵索引树锁死；代价是悲观删除页首/页尾记录时，搜索开销翻倍。

- **源码事实：上层页锁的释放条件是「这次操作会不会改树结构」，而不是「层数有多深」。** `btr_cur_will_modify_tree()` 的判据包含页内记录数、记录在页内的位置距离、页数据大小与 `BTR_CUR_PAGE_COMPRESS_LIMIT(index)`、以及 `page_get_max_insert_size_after_reorganize(page, 2)` 与 `BTR_CUR_PAGE_REORGANIZE_LIMIT`（`UNIV_PAGE_SIZE / 32`）的比较。收益是大多数不触发分裂的插入不必一路攥着父页锁，并发度更高；代价是这套判据本身相当复杂，且带一个明写的 `TODO: adjust this practical limit`（`level > 7` 时把 `max_nodes_deleted` 取成 `64`）。

- **源码事实：根页永远不释放。** 释放上层块的循环体里第一句就是 `if (n_releases == 0) { continue; }`，源码注释写明「我们应该不释放根页，以固定到同一个块」。根页还兼作段头（`seg_header`）所在，`height == level && latch_mode == BTR_MODIFY_TREE` 时会专门用 `mtr_block_sx_latch_at_savepoint()` 把它重新 SX 加锁，注释写明「它含有 seg_header」。

- **源码事实：统计估算路径宁可多花代价也不走 AHI。** `BTR_ESTIMATE` 是 AHI 快路径判据里的一个否定项，且估算路径反而多记一份下降路径（`btr_cur_add_path_info()`）。收益是估算读不污染 AHI 的热度统计（否则估算查询会把 AHI 的命中率统计带偏）；代价是估算每次都要完整下降两遍。

- **源码事实：有一个专门为「内部临时表」准备的无锁双胞胎。** `btr_cur_search_to_nth_level_with_no_latch()` 断言 `index->table->is_intrinsic()`，全程 `RW_NO_LATCH`，取页时多传一个 `mark_dirty` 参数。收益是 intrinsic 表（引擎内部临时表，不共享）省掉全部加锁开销；代价是这个函数**完全不提供正确性保护**——任何非 intrinsic 表误用它，`ut_ad` 在 debug 构建下会立刻崩，release 构建下则是静默的数据竞争。

- **架构推论：** 本机制把「定位」与「加锁」耦在同一个函数里（`latch_mode` 是 `btr_cur_search_to_nth_level()` 的入参），意味着**定位策略的任何变化都要重新审视加锁策略**。AHI 快路径的判据里塞了 `latch_mode <= BTR_MODIFY_LEAF`、`!modify_external`、`!dict_index_is_spatial(index)` 三条加锁/索引形态相关的限制，就是因为「先定位再决定锁」在这个结构里做不到。收益是加锁顺序与页锁生命周期都被集中在一处管理，不容易出现「拿了页锁却没释放」；代价是无法复用「定位」这一层逻辑到别的锁模型下。

- **架构推论：** 下降路径上保存的 `tree_blocks` / `tree_savepoints` 数组与 `n_blocks` / `n_releases` 两个游标，本质上是一个**显式的「已持有资源栈」**。释放上层块时不是简单地「释放除根页外的所有块」，而是从 `n_releases` 指针逐个 `mtr_release_block_at_savepoint()`——因为 mtr 的 savepoint 是栈式的，不能乱序释放。这个结构与主核里常见的「用 RAII guard 持有锁」是两种不同的资源管理风格：InnoDB 这套必须与 mtr 的回滚语义严格对齐。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_search_to_nth_level` | 主入口：解析 `latch_mode` 与操作类型、先试 AHI（命中则 `btr_cur_n_sea++` 后直接返回）、拿树锁、从根页逐层下降、页内二分、把 `up_match` / `low_match` / `up_bytes` / `low_bytes` 交回调用方；写意图触发时改意图后从根页重来 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_search_to_nth_level_with_no_latch` | 无锁双胞胎：断言 `index->table->is_intrinsic()`，`rw_latch` 恒为 `RW_NO_LATCH`，取页时传 `mark_dirty`，只用 `page_cur_search_with_match()` 比较完整字段，无 change buffer、无意图重试 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_latch_leaves` | 叶层加锁：`BTR_SEARCH_LEAF` / `BTR_MODIFY_LEAF` / `BTR_SEARCH_TREE` 只 latch 当前页；`BTR_MODIFY_TREE` 从左到右依次 X latch 左邻居、本页、右邻居；`BTR_SEARCH_PREV` / `BTR_MODIFY_PREV` 额外 latch 左邻居并写入 `cursor->left_block` |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_get_and_clear_intention` | 从 `latch_mode` 摘出 `BTR_LATCH_FOR_INSERT` / `BTR_LATCH_FOR_DELETE`，映射为 `BTR_INTENTION_INSERT` / `BTR_INTENTION_DELETE`，其余为 `BTR_INTENTION_BOTH`，并把标志从 `latch_mode` 中清掉 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_latch_for_root_leaf` | 把加锁模式映射为「根页兼叶页」的锁类型：`BTR_SEARCH_*` → `RW_S_LATCH`，`BTR_MODIFY_*` → `RW_X_LATCH`，`BTR_CONT_*` / `BTR_NO_LATCHES` → `RW_NO_LATCH` |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_need_opposite_intention` | 判断是否需要把写意图改保守后重搜：删除意图下页首/页尾记录、插入意图下页尾记录返回真，`BTR_INTENTION_BOTH` 恒假 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_will_modify_tree` | 判断本次操作是否会改树结构，决定能否释放上层页锁：删除侧用 `max_nodes_deleted`（`level > 7` 取 `64`，否则 `1 << (level - 1)`）与 `BTR_CUR_PAGE_COMPRESS_LIMIT(index)`；插入侧用 `page_get_max_insert_size_after_reorganize(page, 2)` 与 `BTR_CUR_PAGE_REORGANIZE_LIMIT`（`UNIV_PAGE_SIZE / 32`） |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_add_path_info` | 估算路径专用：把下降路径写进 `cursor->path_arr`（页号、页内记录数、页层级、记录序号）；根高度超过 `BTR_PATH_ARRAY_N_SLOTS - 1` 时直接返回空路径 |
| `storage/innobase/btr/btr0cur.cc` | `btr_estimate_n_rows_in_range_low` | 用两次 `BTR_SEARCH_LEAF \| BTR_ESTIMATE` 的搜索确定区间左右边界，再据此估算行数 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_optimistic_latch_leaves` | 游标位置恢复时的乐观加锁：按 `modify_clock` 校验块未被改动，`BTR_SEARCH_PREV` / `BTR_MODIFY_PREV` 额外取左邻居块 |
| `storage/innobase/btr/btr0sea.cc` | `btr_search_guess_on_hash` | AHI 快路径本体：`info->n_hash_potential == 0` 直接返回假；`BTR_SEARCH_BUILD_LIMIT`（`constexpr uint32_t`，值 `100`）控制建索引的热度门槛；成功后置 `cursor->flag = BTR_CUR_HASH` |
| `storage/innobase/btr/btr0sea.cc` | `btr_search_check_guess` | 校验 AHI 猜测的位置：按 `PAGE_CUR_GE` / `PAGE_CUR_LE` / `PAGE_CUR_G` / `PAGE_CUR_L` 比较游标记录及其前后邻居，校验不过返回假从而退回树下降 |
| `storage/innobase/page/page0cur.cc` | `page_cur_search_with_match` | 页内二分查找（本机制的比对实现）：先在页目录槽位上二分，再在槽位所属记录链上线性扫；`mode == PAGE_CUR_G \|\| mode == PAGE_CUR_LE` 时命中后往下界收，否则往上界收；最终按 `mode <= PAGE_CUR_GE` 决定落在 `up_rec` 还是 `low_rec` |
| `storage/innobase/page/page0cur.cc` | `page_cur_search_with_match_bytes` | 字节级前缀比较版本，供叶层 AHI 更新使用（产出 `up_bytes` / `low_bytes`） |
| `storage/innobase/row/row0ins.cc` | `row_ins_sec_index_entry_low` | 唯一二级索引插入的调用方：表是 intrinsic 时走无锁版，否则走 `btr_cur_search_to_nth_level(... PAGE_CUR_LE ...)`；扫到重复键后重新定位游标再插入 |
| `storage/innobase/row/row0log.cc` | `row_log_table_low` | 在线 DDL 行日志回放的调用方：先用 `BTR_MODIFY_LEAF` 乐观删除，失败后回退到 `BTR_MODIFY_TREE` 重新调用 `btr_cur_search_to_nth_level` 再删 |
| `storage/innobase/dict/dict0dict.cc` | `DDTableBuffer::get` | 读动态表元数据的调用方：`BTR_SEARCH_LEAF` + `PAGE_CUR_LE`，用 `cursor.low_match == dtuple_get_n_fields(m_search_tuple)` 判定命中 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_get_father_node_ptr_func` | 向上一层找父页 node pointer 的调用方：`BTR_CONT_MODIFY_TREE` / `BTR_CONT_SEARCH_TREE` + `PAGE_CUR_LE`，intrinsic 表走无锁版 |
| `storage/innobase/btr/btr0btr.cc` | `btr_insert_on_non_leaf_level` | 向非叶层插入的调用方：`BTR_CONT_MODIFY_TREE` + `PAGE_CUR_LE`，空间索引用 `PAGE_CUR_RTREE_INSERT` |
| `storage/innobase/row/row0sel.cc` | `row_sel_open_pcur` / `row_sel_try_search_shortcut` | 读路径：经由 `plan->pcur.open_no_init(index, plan->tuple, plan->mode, BTR_SEARCH_LEAF, ...)` 间接进入本机制；同文件 `row_sel_try_search_shortcut()` 的函数体先 `rw_lock_s_lock(btr_get_search_latch(index), UT_LOCATION_HERE);` 拿搜索闩（并在 `rw_lock_get_writer(...) == RW_X_WAIT` 时先释放再重拿，注释写明「一个 x-latch 请求在等待时，先放开 s-latch 一会儿」），再调 `row_sel_try_search_shortcut()` 走 AHI 快捷路径 |

## 与主核 / AI 的关系

主核（`engine/db`）当前没有 InnoDB 这套「树游标 + 加锁模式 + 意图」的组合结构：主核的 `engine/db/index/` 层是自研的多模态索引，定位路径上还没有 InnoDB 这种「页目录二分 + node pointer 逐层下降 + 页锁栈」的形态。

**架构推论：** InnoDB 这套机制里最值得主核借鉴的是**「匹配度」而非「匹配与否」的返回值设计**。`up_match` / `low_match` 是「匹配了几个字段」的计数，让调用方能区分「唯一键全等」与「仅前缀相同」。主核的索引接口若只返回「找到 / 没找到」，那么唯一约束的去重就必须再比一次；把匹配度做进接口返回值，可以让「唯一索引冲突检查」变成零额外比较的判断。

**架构推论：** `btr_cur_need_opposite_intention()` 与 `btr_cur_will_modify_tree()` 这类「先乐观、失败再保守」的策略，对 AI 参与写入路径有直接含义：AI 生成的 SQL 往往携带不完整的键值（只有前缀、没有唯一键全列），这类请求最容易触发「唯一索引冲突」。如果主核要让 AI 处理这类冲突，就需要在索引层显式建模「这次写入会不会改动树结构」这一判据，让 AI 拿到的是「可重试」与「不可重试」的明确区分，而不是笼统的写入失败。

**架构推论：** 本机制把「定位」与「加锁」耦在一个函数的入参里（`latch_mode`），这决定了 AI 只能观测到「定位成功 / 定位失败」和最终落在哪条记录上，看不到「为什么选了这个加锁模式」。如果主核要让 AI 参与并发控制（例如让模型决定是否升级为悲观路径），就需要把「定位」与「加锁强度」拆成两个可分开的决策点，否则模型无法在「已定位但锁太弱」时介入。

## 参考

- [MySQL 源码：`storage/innobase/btr/btr0cur.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/btr/btr0cur.cc) —— `btr_cur_search_to_nth_level()` 与 `btr_cur_search_to_nth_level_with_no_latch()` 的完整实现，以及 `btr_cur_latch_leaves` / `btr_cur_get_and_clear_intention` / `btr_cur_latch_for_root_leaf` / `btr_cur_need_opposite_intention` / `btr_cur_will_modify_tree` / `btr_cur_add_path_info` 等辅助函数。
- [MySQL 源码：`storage/innobase/btr/btr0sea.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/btr/btr0sea.cc) —— 自适应哈希索引一侧：`btr_search_guess_on_hash()` 的快路径判据与 `btr_search_check_guess()` 的猜测校验，以及 `BTR_SEARCH_BUILD_LIMIT` 的取值。
- [MySQL 源码：`storage/innobase/page/page0cur.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/page/page0cur.cc) —— 页内比对的实现：`page_cur_search_with_match()` 的页目录二分与线性扫描，以及 `page_cur_search_with_match_bytes()` 的字节前缀比较。
- [MySQL 源码：`storage/innobase/btr/btr0btr.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/btr/btr0btr.cc) —— 调用方 `btr_page_get_father_node_ptr_func()` 与 `btr_insert_on_non_leaf_level()`，展示 `BTR_CONT_MODIFY_TREE` / `BTR_CONT_SEARCH_TREE` 两种「续下降」模式。
- [MySQL 源码：`storage/innobase/row/row0ins.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/row/row0ins.cc) —— 调用方 `row_ins_sec_index_entry_low()`：`PAGE_CUR_LE` 的用法、intrinsic 表走无锁版的分支，以及唯一索引重复键扫描后重新定位游标。
- [MySQL 参考手册：Introduction to InnoDB](https://dev.mysql.com/doc/refman/8.0/en/innodb-introduction.html) —— InnoDB 的存储与索引组织背景，说明 B-tree 索引在 InnoDB 中的地位。
- [MySQL 参考手册：InnoDB Physical Structure](https://dev.mysql.com/doc/refman/8.0/en/innodb-physical-structure.html) —— 页与表空间的物理结构，是理解本机制「页」这一层前提的官方材料。
- [MySQL 参考手册：InnoDB Indexes](https://dev.mysql.com/doc/refman/8.0/en/innodb-indexes.html) —— InnoDB 索引类型与使用方式，含聚簇索引与二级索引的组织方式。
