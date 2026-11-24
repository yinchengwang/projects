---
project: mysql-server
feature: btr-insert-split
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/btr/btr0btr.cc
    symbols: [btr_page_split_and_insert, btr_page_reorganize]
---

# B-tree 插入与页分裂

## 一句话职责

在 InnoDB 的 B-tree 上把一条新记录落到正确的叶页：页内塞不下时把该页拆成上下两半、把节点指针挂到上一层，并把记录插入其中一半。

## 机制是什么

本页覆盖的边界是「插入记录 → 需要时就地分裂 → 记录最终落在某个半页」这一段，具体包括三件事：

1. **页内插入**：在已定位的叶页上把记录插到正确位置。页内插入由 `page_cur_tuple_insert` 承担（在 `storage/innobase/page/page0cur.cc`），本页只描述 B-tree 层面的决策与失败后的回退。
2. **页分裂**：当页内插入放不下时，`btr_page_split_and_insert` 决定分裂点、分配新页、把原页拆成两半、向父层插入节点指针，然后再插入记录。
3. **页重组**：分裂前若新页放不下、或分裂后插入仍失败，先尝试 `btr_page_reorganize` 把当前页记录重排成紧凑布局再重试插入。

不负责记录本身的编码（K 组），也不负责页的写回与缓冲池管理（H 组）。

## 怎么实现的

### 分裂的主流程

`btr_page_split_and_insert`（`storage/innobase/btr/btr0btr.cc`）的函数体按 8 个编号步骤组织，逐步如下：

**0. 空间索引先分流。** 函数开头判断 `dict_index_is_spatial(index)`，为真时直接 `return rtr_page_split_and_insert(...)`——空间索引的分裂由 R-tree 自己实现，不走本页描述的路径。

**1. 先试右兄弟页。** 在做任何分裂动作之前，先调用 `btr_insert_into_right_sibling` 尝试把记录直接插到同一层的右兄弟页。该函数要求当前游标记录的后继是 supremum（即游标已在本页末尾），且本页存在右兄弟（`btr_page_get_next` 不为 `FIL_NULL`）；条件不满足就返回 `nullptr` 交回主流程。成功时它还会通过 `btr_insert_on_non_leaf_level` 更新父层节点指针——因为记录插到了右兄弟页，父层该节点的分裂键随之改变。

**2. 决定分裂点。** 主流程按 `n_iterations` 分为两条路：

- 首次进入（`n_iterations == 0`）时，先试 `btr_page_get_split_rec_to_right`，再试 `btr_page_get_split_rec_to_left`，都不命中才走兜底分支。这两个判断都基于页头 `PAGE_LAST_INSERT` 指针与当前插入点的关系：`btr_page_get_split_rec_to_right` 在「上次插入点就是当前插入点」时判定为顺序插入模式，向右分裂时把 `next_next_rec` 作为分裂点（源码注释明确说「留一条记录在原页，这样顺序插入还能用自适应哈希索引」）；`btr_page_get_split_rec_to_left` 在「上次插入点的后继就是当前插入点」时判定为向左收敛，向左分裂。
- 兜底分支中，若 `page_get_n_recs(page) > 1`，分裂点取 `page_get_middle_rec(page)`；若页内只有一条记录，则用 `btr_page_tuple_smaller` 判断新记录是否比页内第一条用户记录更小——更小则把 supremum 之前的记录作为分裂点，否则 `split_rec` 置为 `nullptr`，含义是「新记录将成为上半页的第一条记录」。
- 重试路径（`n_iterations > 0`）改用 `btr_page_get_split_rec`，它按记录的 `rec_offs_size` 逐条累加，直到已包含记录的 `incl_data + page_dir_calc_reserved_space(n)` 不小于 `total_space / 2`；`total_space = page_get_data_size(page) + insert_size + page_dir_calc_reserved_space(total_n_recs)`，其中 `insert_size = rec_get_converted_size(cursor->index, tuple)`。

**3. 分配新页并建页。** 用 `btr_page_alloc(cursor->index, hint_page_no, direction, btr_page_get_level(page), mtr, mtr)` 分配新页。`hint_page_no` 与 `direction`（`FSP_UP` / `FSP_DOWN`）由第 2 步决定：向上分裂时 `hint_page_no = page_no + 1`，向下分裂时 `hint_page_no = page_no - 1`。分配失败直接 `return nullptr`。分配成功后用 `btr_page_create` 按 `btr_page_get_level(page)` 建页。

**4. 计算两条边界记录。** `first_rec` 是上半页的第一条记录，`move_limit` 是原页上会跑到上半页的第一条记录。当 `split_rec` 非空时两者都等于 `split_rec`，并用 `cmp_dtuple_rec(tuple, split_rec, cursor->index, *offsets) < 0` 决定 `insert_left`（新记录是否应落到左半页）。当 `split_rec` 为空且 `insert_left` 为真时，`first_rec` 取 infimum 的后继；`split_rec` 为空且 `insert_left` 为假时进入 `insert_empty` 标签：`buf` 按 `rec_get_converted_size(cursor->index, tuple)` 分配，`first_rec = rec_convert_dtuple_to_rec(buf, cursor->index, tuple)`。此时若页是压缩页（`new_page_zip` 非空）且 `n_iterations > 0`，源码直接把 `split_rec` 置空并 `goto insert_empty`——注释说明这是为了在压缩页已经分裂过一次后避免继续分裂，改为把记录放进一个空页。

**5. 先改树结构。** 调用 `btr_attach_half_pages` 挂接两半页。该函数先按 `direction` 决定上下半页（FSP_DOWN 时新页是下半页，FSP_UP 时原页是下半页），用 `dict_index_build_node_ptr` 构造上半页的节点指针，然后调用 `btr_insert_on_non_leaf_level(flags, index, level + 1, node_ptr_upper, ...)` 把节点指针插入上一层；源码注释指出这一步「may generate recursion leading to a split on the higher level」（会生成递归，进而在更高层触发分裂）。随后更新本层的页前后指针（`btr_page_set_prev` / `btr_page_set_next`）。

**6. 树结构改完后提前释放树锁。** 源码用 `btr_page_insert_fits` 判断插入是否会落在选中的那一半页上；该函数按 `cmp_dtuple_rec` 的结果决定要腾挪的记录区间（从 infimum 后继到 `split_rec`，或从 `split_rec` 到 supremum），然后逐条减去 `rec_offs_size`，直到 `total_data + page_dir_calc_reserved_space(total_n_recs) <= free_space` 才返回 true，其中 `free_space = page_get_free_space_of_empty(page_is_comp(page))`。当 `!srv_read_only_mode`、非 intrinsic 表、`insert_will_fit` 为真、`page_is_leaf(page)` 为真且非 online DDL 五个条件同时成立时，源码执行 `mtr->memo_release(dict_index_get_lock(cursor->index), MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK)` 释放树锁——源码注释明确说明根页的 latch 不能在此释放，原文为「We cannot release root block latch here, because it has segment header and already modified in most of cases.」

**7. 搬移记录。** `direction == FSP_DOWN`（向左分裂）时调用 `page_move_rec_list_start(new_block, block, move_limit, ...)`；否则调用 `page_move_rec_list_end(new_block, block, move_limit, ...)`。两者返回假时（源码注释：压缩新页失败）走兜底：`page_zip_copy_recs` 逐字节复制页、`page_delete_rec_list_end` / `page_delete_rec_list_start` 删除两侧记录、`lock_move_rec_list_start` / `lock_move_rec_list_end` 迁移锁位图、`btr_search_update_hash_on_move` 更新哈希索引。搬移完成后分别调用 `lock_update_split_left(right_block, left_block)` 或 `lock_update_split_right(right_block, left_block)` 更新锁表。

**8. 定位并插入，最后收尾。** 按 `insert_left` 选定 `insert_block`（左半页或右半页），用 `page_cur_search` 重新定位后调用 `page_cur_tuple_insert`。若插入返回空，则判断 `page_cur_get_page_zip(page_cursor)` 为真（压缩页，`page_cur_tuple_insert` 内部已尝试重组）或 `btr_page_reorganize` 返回假（重组失败），两者都走 `insert_failed` 标签：先 `ibuf_reset_free_bits(new_block)` 与 `ibuf_reset_free_bits(block)` 重置插入缓冲位图（仅当非聚簇索引且非临时表），然后 `n_iterations++` 并 `goto func_start` 重新走一遍分裂流程。成功路径在 `func_exit` 处：对非聚簇索引、非临时表的叶页调用 `ibuf_update_free_bits_for_two_pages_low(left_block, right_block, mtr)`，并 `MONITOR_INC(MONITOR_INDEX_SPLIT)`。

### 根页扩张

根页不能再向上分裂，`btr_root_raise_and_insert` 负责把树长高一层：先 `btr_page_alloc(index, 0, FSP_NO_DIR, level, mtr, mtr)` 分配新页（`level` 取自 `btr_page_get_level(root)`），把根页记录搬到新页（`page_copy_rec_list_end`），在根页插入指向新页的节点指针（`page_cur_tuple_insert`，源码用 `ut_a(node_ptr_rec)` 断言必然成功），最后调用 `btr_page_split_and_insert` 完成子页分裂。`storage/innobase/btr/btr0cur.cc` 中的 `btr_cur_pessimistic_insert` 依据 `dict_index_get_page(index) == btr_cur_get_block(cursor)->page.id.page_no()` 判断当前页是不是根页：是则调 `btr_root_raise_and_insert`，否则调 `btr_page_split_and_insert`。

### 页重组

`btr_page_reorganize` 是 `btr_page_reorganize_low` 的薄封装，传入 `recovery = false` 与压缩级别 `page_zip_level`。`btr_page_reorganize_low` 的做法是：把整页复制到临时块（`buf_frame_copy`），用 `page_create` 重建原页（注释说明全局页数据——segment header、next page 字段等——保持原样），用 `page_copy_rec_list_end_no_locks` 把记录拷回（先不拷锁位），若 `dict_index_is_sec_or_ibuf(index)` 且是叶页且非临时表则把 `page_get_max_trx_id(temp_page)` 拷回，最后用 `lock_move_reorganize_page` 迁移锁位图。成功与否的判据是 `data_size1 != data_size2 || max_ins_size1 != max_ins_size2`——两项（`page_get_data_size` 与 `page_get_max_insert_size_after_reorganize(page, 1)`）中任意一项变化就报错 `ER_IB_MSG_30` / `ER_IB_MSG_31` 并置 `success = false`。压缩页重组失败时（`page_zip_compress` 返回假）源码按字节把临时页拷回原页并 `goto func_exit`，返回 false。成功时写 redo：压缩页用 `MLOG_ZIP_PAGE_REORGANIZE`，非压缩页用 `MLOG_PAGE_REORGANIZE`，并在压缩页情形下把压缩级别 `z_level` 追加写入日志。

## 关键取舍

- **源码事实：**分裂点的选择有三套并存的启发式（`btr_page_get_split_rec_to_right` / `btr_page_get_split_rec_to_left` / 兜底的 `page_get_middle_rec`），都依赖页头 `PAGE_LAST_INSERT` 指针判断插入模式。**架构推论：**这三条路径针对的是顺序插入与单边收敛两种常见负载——顺序插入时若每次都平分页面，同一条键的后续插入会立刻再次触发分裂，而按收敛点分裂可以让同一批键集中落到一侧；启发式以页头一个指针的代价换取这种分布特性，代价是分裂位置不保证严格居中。
- **源码事实：**`btr_page_get_split_rec_to_right` 在向上分裂时把 `next_next_rec` 而非 `next_rec` 作为分裂点，源码注释写明「留一条记录在原页，这样顺序插入还能用自适应哈希索引，只需要检查右边的搜索位置」。**架构推论：**分裂点的选择被自适应哈希索引的正确性反向约束：留在原页的记录必须足以让哈希查找确认右侧搜索位置，这比纯粹的 50/50 均分多了一条约束。
- **源码事实：**树锁的释放发生在第 5 步结构改完之后、记录搬移之前，条件是 `!srv_read_only_mode`、非 intrinsic 表、`insert_will_fit` 为真、`page_is_leaf(page)` 为真且非 online DDL 五项同时成立（`insert_will_fit` 由 `btr_page_insert_fits` 计算）。**架构推论：**先改结构后搬记录、且把搬记录放到树锁之外，是为了缩短全局树锁的持有时间——搬移是纯数据搬运、不改树形，代价是搬运期间新页内容对并发读者尚不可见，读取方必须依赖页级 latch 与缓冲池的读一致性来获得正确视图。
- **源码事实：**插入失败时先尝试 `btr_page_reorganize` 再重试 `page_cur_tuple_insert`，两者都失败才 `goto func_start` 重新分裂；`n_iterations++` 后源码用 `ut_ad(n_iterations < 2 || buf_block_get_page_zip(insert_block))` 约束非压缩页最多重试一次。**架构推论：**重组的成本是整页复制 + 重建，而分裂的成本是分配新页 + 更新父层节点指针，二者的失效模式不同；先做低成本的重排、再做高成本的分裂，是按代价递增的顺序尝试。断言同时说明压缩页允许更多次重试，因为压缩页的重排本身带有压缩失败这一分支（`page_zip_compress` 返回假）。
- **源码事实：**`btr_page_split_and_insert` 的注释明确写「NOTE: the tree x-latch is released within this function」以及「the operation of this function must always succeed, we cannot reverse it: therefore enough free disk space (2 pages) must be guaranteed to be available before this function is called」。**架构推论：**分配新页失败时函数直接返回 `nullptr`（不尝试回退到重组路径），因为一旦树结构已被改动就无法撤销，所以空间预留必须在调用之前完成——这解释了为什么调用方 `btr_cur_pessimistic_insert` 在调用前有 `fil_space_release_free_extents` 这一释放动作与之配对。
- **源码事实：**压缩页在 `n_iterations > 0` 的重试路径上把 `split_rec` 置空并跳到 `insert_empty`，源码注释为「避免进一步分裂，改为把记录插入一个空页」。**架构推论：**压缩页每次重组都要重新压缩整页，失败率高，因此宁可让出一整个空页也不冒险第二次压缩。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `storage/innobase/btr/btr0btr.cc` | `btr_page_split_and_insert` | 分裂主流程：决定分裂点、分配新页、挂接上下半页、搬移记录并完成插入。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_reorganize` | 页重组入口，以 `recovery = false` 与 `page_zip_level` 调用 `btr_page_reorganize_low`。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_reorganize_low` | 重组实现：临时页备份、重建页、拷回记录与锁位图，成功后写 `MLOG_PAGE_REORGANIZE` / `MLOG_ZIP_PAGE_REORGANIZE`。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_get_split_rec_to_right` | 顺序插入模式下决定向右分裂点，保留一条记录在原页以支持自适应哈希索引。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_get_split_rec_to_left` | 判断插入是否向左收敛并给出向左分裂点。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_get_split_rec` | 重试路径上按记录大小与页目录预留空间累计计算分裂点。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_insert_fits` | 判断插入是否落在选中的那一半页上，用于决定能否提前释放树锁。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_tuple_smaller` | 判断新记录是否比页内第一条用户记录更小。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_insert_into_right_sibling` | 分裂前先尝试直接插入右兄弟页，并同步更新父层节点指针。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_attach_half_pages` | 挂接上下半页、构造并插入上层节点指针、维护本层页前后指针。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_insert_on_non_leaf_level` | 把节点指针插入非叶层；源码注释指出该调用会生成递归，进而在更高层触发分裂。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_root_raise_and_insert` | 根页扩张：把根记录搬到新页、在根插入节点指针后再分裂子页。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_alloc` | 按 `hint_page_no` 与 `direction` 分配分裂用的新页。 |
| `storage/innobase/btr/btr0btr.cc` | `btr_page_create` | 按指定层号创建索引页并写入 level 与 index id。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_pessimistic_insert` | 插入的实际调用方：判定是否根页后在 `btr_root_raise_and_insert` 与 `btr_page_split_and_insert` 之间二选一。 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_insert_if_possible` | 乐观插入路径：页内插入失败时调用 `btr_page_reorganize` 后重试。 |

## 与主核 / AI 的关系

主核 `engine/db/` 的 L1 存储层与 L2 索引层同样面临「记录放不下就得分裂页」的问题。可对照的设计点有三个：

- 主核的 `engine/db/index/btree.c` 若实现页分裂，其切分点选择是否也依赖写入模式（如是否记录上一次插入位置）决定页分布，还是固定按记录数或字节数平分。
- 主核的分裂失败回退路径是否也区分「重组重试」与「重新分裂」两级，以及各自的重试次数上限与断言。
- 主核若支持压缩页，需明确「重组时压缩失败」这一分支如何回退到原页内容。

**架构推论：**InnoDB 在这里把「页重组」做成独立于「页分裂」的机制、并在两者之间显式分级，这一分层值得主核参考；主核是否采用同等分层，取决于其页格式与日志协议是否支持整页重建后的一致性恢复。

## 参考

- [MySQL 8.0 Reference Manual：InnoDB 物理结构（页、记录与索引结构）](https://dev.mysql.com/doc/refman/8.0/en/innodb-physical-structure.html) —— 页级结构与 B-tree 索引的官方说明。
- [MySQL 官方源码：storage/innobase/btr/btr0btr.cc（8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/btr/btr0btr.cc) —— 本页全部符号的定义处。
- [MySQL 官方源码：storage/innobase/btr/btr0btr.cc（raw 文本）](https://raw.githubusercontent.com/mysql/mysql-server/8.0/storage/innobase/btr/btr0btr.cc) —— 可直接检索函数体的原始文本。
- [MySQL 官方源码：storage/innobase/btr/btr0cur.cc（8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/btr/btr0cur.cc) —— 调用方 `btr_cur_pessimistic_insert` 所在文件。
