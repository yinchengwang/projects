---
project: mysql-server
feature: page-directory
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/page/page0cur.cc
    symbols: [page_cur_search_with_match, page_cur_search_with_match_bytes]
---

# 页目录与记录定位

## 一句话职责

`storage/innobase/page/page0cur.cc` 的 `page_cur_search_with_match()` 与 `page_cur_search_with_match_bytes()` 负责把一个「待查元组 + 查找模式」在**单个索引页的内部**定位成一条具体记录：先用页目录（page directory）的槽位做二分查找把候选区间收敛到相邻两个槽位，再在低位槽所拥有的记录链上做线性查找，最后按查找模式把游标钉在「不小于目标」或「不大于目标」的那条记录上，并把「匹配了多少个字段（以及多少字节）」回传给调用方。

## 机制是什么

**它解决的是「一页之内怎么找记录」，不是「一页之内怎么比记录」。** 这两件事在 InnoDB 里被拆在两个文件：

- **定位**在本文件：`page_cur_search_with_match()` / `page_cur_search_with_match_bytes()`。它们的输入是页块、索引描述、待查元组与查找模式，输出是 `page_cur_t` 游标 + `up_match` / `low_match`（字节版另加 `up_bytes` / `low_bytes`）。
- **比较**在 `storage/innobase/rem/rem0cmp.cc`：字节版走 `cmp_dtuple_rec_with_match_bytes()`，普通版走 `dtuple_t::compare()`。本文件只负责「在哪比、比完往哪边挪」。

**页目录是什么。** 一个索引页除了记录数据区，还带一个「槽位数组」（`page_dir_slot_t`）。`storage/innobase/page/page0page.cc` 的 `page_dir_find_owner_slot()` 说明了槽位的语义：一个槽位「拥有」一段连续的记录，这条记录由槽位自己记录（`page_dir_slot_get_rec()`），拥有多少条记录由 `n_owned` 记录（`page_dir_slot_get_n_owned()`）。空页在 `page_create_low()` 里被初始化成 `page[PAGE_HEADER + PAGE_N_DIR_SLOTS + 1] = 2;`，也就是**两个槽位**（infimum 与 supremum 各一个）。目录槽位之间是有序的，因此目录本身就是一个可二分的有序数组。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责页内记录的定位：页目录槽位到记录的二分查找、以及记录链的遍历；**不负责记录头的变长字段解析（rem0rec）**、**不负责页的缓存（H 组）**。

## 怎么实现的

### 两个函数是同一套骨架的两份拷贝

`page_cur_search_with_match()` 与 `page_cur_search_with_match_bytes()` 的结构完全平行，差别只在两点：

1. **比较函数**：普通版调 `tuple->compare(mid_rec, index, offsets, &cur_matched_fields)`；字节版调 `cmp_dtuple_rec_with_match_bytes(tuple, mid_rec, index, offsets, &cur_matched_fields, &cur_matched_bytes)`。
2. **匹配度的合并方式**：普通版用 `cur_matched_fields = std::min(low_matched_fields, up_matched_fields);`；字节版用 `ut_pair_min(&cur_matched_fields, &cur_matched_bytes, low_matched_fields, low_matched_bytes, up_matched_fields, up_matched_bytes);`，即按「(字段数, 字节数)」这一对值取较小者。

字节版的入口在 `storage/innobase/btr/btr0cur.cc` 的 `btr_cur_search_to_nth_level()` 里，调用条件是 `height == 0 && btr_search_enabled && !dict_index_is_spatial(index)`，源码注释写明理由：「自适应哈希索引只在搜索叶页（height==0）时使用，R-tree 不用。我们只需要字节前缀比较来更新自适应哈希索引。」普通版的调用条件是同一个函数的 `else` 分支，源码注释是「搜索完整的索引字段」。同一个函数里还有第三处调用，在 `btr_cur_search_to_nth_level()` 的重试路径上，为已走过的每一层页重放一次 `page_cur_search_with_match()`。

### 步骤一：先试「上次插入点」捷径

`page_cur_search_with_match()` 在做二分之前先试一次捷径，判据是一串合取：

```
if (page_is_leaf(page) && (mode == PAGE_CUR_LE) &&
    !dict_index_is_spatial(index) &&
    (page_header_get_field(page, PAGE_N_DIRECTION) > 3) &&
    (page_header_get_ptr(page, PAGE_LAST_INSERT)) &&
    (page_header_get_field(page, PAGE_DIRECTION) == PAGE_RIGHT)) {
  if (page_cur_try_search_shortcut(block, index, tuple, iup_matched_fields,
                                   ilow_matched_fields, cursor)) {
    return;
  }
}
```

逐条对应：

- `page_is_leaf(page)` —— 只在叶页上试，因为叶页才有「顺序追加插入」这个模式。
- `mode == PAGE_CUR_LE` —— 只在「找不大于目标的位置」这一种模式上试。
- `!dict_index_is_spatial(index)` —— 排除 R-tree。
- `page_header_get_field(page, PAGE_N_DIRECTION) > 3` —— 方向计数器必须**大于 3**。这个计数器在 `page_cur_insert_rec_low()` 里维护：当插入位置不在页边界时置 `PAGE_DIRECTION` 为 `PAGE_RIGHT` 或 `PAGE_LEFT`，并把 `PAGE_N_DIRECTION` 加一；方向反转或回到 `PAGE_NO_DIRECTION` 时把它清零。因此「大于 3」意味着**连续 3 次以上同方向的顺序插入**。
- `page_header_get_ptr(page, PAGE_LAST_INSERT)` —— 页头必须留有「上次插入点」的记录指针；`page_cur_delete_rec()` 在删记录时把它清成 `nullptr`。
- `page_header_get_field(page, PAGE_DIRECTION) == PAGE_RIGHT` —— 上次插入方向必须是向右。

捷径本体是同文件的 `page_cur_try_search_shortcut()`：它取出 `PAGE_LAST_INSERT` 指向的记录 `rec`，用 `tuple->compare(rec, index, offsets, &low_match)` 判断目标是否不小于 `rec`（小于则放弃捷径）；再取 `page_rec_get_next_const(rec)` 得到 `next_rec`，若 `next_rec` 不是 supremum 且 `tuple->compare(next_rec, ...) >= 0`（目标不小于下一条）则放弃捷径；否则说明目标就落在 `rec` 与 `next_rec` 之间，直接 `page_cur_position(rec, block, cursor)` 定位并返回成功。字节版对应 `page_cur_try_search_shortcut_bytes()`，用 `cmp_dtuple_rec_with_match_bytes()` 做同样的两道夹逼判断。

**捷径失败时的回退路径（点名真实调用方）：** 捷径不成立时，函数体不 return，而是继续往下走页目录二分——`page_cur_search_with_match()` 落到 `page_dir_get_n_slots(page)` 那一行开始的标准二分；字节版 `page_cur_search_with_match_bytes()` 同理，落到它自己的 `page_dir_get_n_slots(page)` 二分。两者都不是跳到别的函数，而是**在同一个函数内继续执行二分分支**。

### 步骤二：R-tree 走另一条路

`page_cur_search_with_match()` 在捷径之后有一处分支：

```
if (dict_index_is_spatial(index) && mode > PAGE_CUR_LE) {
  if (mode == PAGE_CUR_RTREE_INSERT && page_is_leaf(page)) {
    mode = PAGE_CUR_LE;
  } else {
    rtr_cur_search_with_match(block, (dict_index_t *)index, tuple, mode,
                              cursor, rtr_info);
    return;
  }
}
```

也就是说：**空间索引且模式大于 `PAGE_CUR_LE` 时，本机制不参与定位**，而是转交 `rtr_cur_search_with_match()`（MBR 比较）并直接返回；唯一的例外是 `PAGE_CUR_RTREE_INSERT` 打在叶层时，模式被改写成 `PAGE_CUR_LE`，仍走本机制的普通二分。字节版 `page_cur_search_with_match_bytes()` **没有**这个 R-tree 分支。

### 步骤三：页目录上的二分查找

二分的主体是一个 `while (up - low > 1)` 循环，初始边界是：

```
low = 0;
up = page_dir_get_n_slots(page) - 1;
```

- `mid = (low + up) / 2;`
- `slot = page_dir_get_nth_slot(page, mid);`
- `mid_rec = page_dir_slot_get_rec(slot);`
- 取出 `mid_rec` 的字段偏移数组，然后比较。

比较结果决定区间如何收缩，共三个分支：

- **`cmp > 0`**（目标大于 `mid_rec`）：`low = mid;`，并把 `low_matched_fields` 更新为 `cur_matched_fields`。
- **`cmp != 0`（目标小于 `mid_rec`）**：`up = mid;`，并把 `up_matched_fields` 更新为 `cur_matched_fields`。
- **`cmp == 0`（目标等于 `mid_rec`）**：分模式。`mode == PAGE_CUR_G || mode == PAGE_CUR_LE` 时走 `low = mid;`（把命中点当作下界保留）；否则走 `up = mid;`。

循环条件 `up - low > 1` 的含义是**循环结束时上下界槽位相邻**，也就是「候选区间已经缩到一个槽位 owned 的记录链内」。

### 步骤四：槽位内的记录链线性查找

二分结束后取两个槽位各自的记录作为边界：

```
slot = page_dir_get_nth_slot(page, low);
low_rec = page_dir_slot_get_rec(slot);
slot = page_dir_get_nth_slot(page, up);
up_rec = page_dir_slot_get_rec(slot);
```

然后是线性扫描：

```
while (page_rec_get_next_const(low_rec) != up_rec) {
  mid_rec = page_rec_get_next_const(low_rec);
  ...
}
```

循环体沿**记录链**（`page_rec_get_next_const()`）逐条前进，比较逻辑与二分完全平行（`cmp > 0` 收 `low_rec`、`cmp < 0` 收 `up_rec`、`cmp == 0` 时 `PAGE_CUR_G` / `PAGE_CUR_LE` 收 `low_rec`，否则收 `up_rec`），只是变量从槽位下标换成了记录指针。

### 步骤五：落点与回传

```
if (mode <= PAGE_CUR_GE) {
  page_cur_position(up_rec, block, cursor);
} else {
  page_cur_position(low_rec, block, cursor);
}
```

落点只看 `mode <= PAGE_CUR_GE` 这一个判断：`PAGE_CUR_GE` 及更小的模式落在**上界**记录 `up_rec`，更大的模式落在**下界**记录 `low_rec`。字节版 `page_cur_search_with_match_bytes()` 的落点判断完全一致。

最后把匹配度回传：`page_cur_search_with_match()` 写 `*iup_matched_fields = up_matched_fields;` 与 `*ilow_matched_fields = low_matched_fields;`；字节版多写两个 `*iup_matched_bytes` / `*ilow_matched_bytes`。

### 「零匹配」的特殊处理

线性扫描里 `cmp == 0` 的分支有一段额外处理：当 `!cmp && !cur_matched_fields` 时，源码在 debug 构建下断言这条记录带 `REC_INFO_MIN_REC_FLAG`、且 `btr_page_get_prev(page, &mtr) == FIL_NULL`（即它是整棵树的最小记录），然后把 `cur_matched_fields` 置为 `dtuple_get_n_fields_cmp(tuple)`。两个函数都有这段逻辑。

### 错误分支与断言

- `page_cur_search_with_match()` 在 `UNIV_DEBUG` 下断言 `mode` 只能是 `PAGE_CUR_L` / `PAGE_CUR_LE` / `PAGE_CUR_G` / `PAGE_CUR_GE`，或者是空间索引（`dict_index_is_spatial(index)`）。
- `page_cur_search_with_match_bytes()` 在 `UNIV_DEBUG` 下断言 `mode` 只能是 `PAGE_CUR_L` / `PAGE_CUR_LE` / `PAGE_CUR_G` / `PAGE_CUR_GE`——**没有空间索引的豁免**。
- 两个函数都在 `PAGE_CUR_LE_OR_EXTENDS` 编译开关下断言 `mode != PAGE_CUR_LE_OR_EXTENDS`（源码注释说明原因：该模式依赖 `cmp_full_field` 报告匹配字节数，对非 latin1 字符集不成立）。`page_cur_rec_field_extends()` 是该开关下的辅助函数，判断记录的第 n 个字段是否「扩展」了元组的第 n 个字段。
- 两个函数都调 `ut_d(page_check_dir(page))` 做页目录一致性调试校验。
- `UNIV_ZIP_DEBUG` 下两者都断言 `!page_zip || page_zip_validate(page_zip, page, index)`。

### 目录槽位与记录的维护方（同一组文件内的邻接机制）

页目录本身不是本机制维护的，但它的读写规则决定了本机制能二分的前提，因此一并说明，符号均来自 `storage/innobase/page/page0page.cc` 与 `storage/innobase/page/page0cur.cc`：

- **插入时扩槽**：`page_cur_insert_rec_low()` 插入记录后递增拥有者记录的 `n_owned`；当 `n_owned == PAGE_DIR_SLOT_MAX_N_OWNED` 时调 `page_dir_split_slot()` 把槽位一分为二。`page_dir_split_slot()` 的函数体断言 `n_owned == PAGE_DIR_SLOT_MAX_N_OWNED + 1`（即触发时已超过上限），并把记录按 `n_owned / 2` 拆成两半。
- **删除时并槽**：`page_cur_delete_rec()` 递减拥有槽的 `n_owned`；当 `cur_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED` 时调 `page_dir_balance_slot()`。`page_dir_balance_slot()` 的函数体断言 `n_owned == PAGE_DIR_SLOT_MIN_N_OWNED - 1`，上邻槽 `n_owned` 大于 `PAGE_DIR_SLOT_MIN_N_OWNED` 时把一条记录转交下邻槽，否则调 `page_dir_delete_slot()` 合并两个槽位。
- **槽位数量的取值范围**：`page_dir_slot_check()` 的函数体断言首槽 `n_owned == 1`、末槽 `PAGE_DIR_SLOT_MIN_N_OWNED <= n_owned <= PAGE_DIR_SLOT_MAX_N_OWNED`、中间槽同样在 `[PAGE_DIR_SLOT_MIN_N_OWNED, PAGE_DIR_SLOT_MAX_N_OWNED]` 内。`PAGE_DIR_SLOT_MIN_N_OWNED` / `PAGE_DIR_SLOT_MAX_N_OWNED` 的具体数值定义在 InnoDB 头文件里，本次本地检出不包含这些头文件，因此本页不给出数值。

**架构推论：** 「`n_owned` 被维持在 `[PAGE_DIR_SLOT_MIN_N_OWNED, PAGE_DIR_SLOT_MAX_N_OWNED]` 区间」这一不变式，正是页目录能被当作有序数组二分的**前提**——二分要求「槽位 i 拥有的记录全部小于槽位 i+1 拥有的记录」，而 `n_owned` 的上下界保证了单个槽位不会无限膨胀（否则二分退化成链表遍历），也不会缩到 1（否则目录膨胀）。这条不变式由插入侧的 `page_dir_split_slot()` 与删除侧的 `page_dir_balance_slot()` 共同维护。

## 关键取舍

- **源码事实：定位被拆成「目录二分 + 槽内线性」两段，而不是全程二分。** 目录槽位是稀疏的——槽位只指向它拥有的第一条记录，同槽位内的其他记录要靠记录链走。收益是目录本身很小（每槽只存一条记录指针加一个 `n_owned`），二分阶段比较次数少；代价是槽内那一段是线性扫描，最坏情况要扫完槽内全部记录。

- **源码事实：匹配度（`up_match` / `low_match`）被作为出参一路带回，而不是只回「找到 / 没找到」。** 收益是调用方能区分「唯一键全等」与「仅前缀相同」；代价是调用方必须自己知道「唯一键字段数」与「索引总字段数」这两个不同的分母。

- **源码事实：两个函数各自持有一份完整拷贝，而不是共用一个带函数指针的公共实现。** `page_cur_search_with_match()` 与 `page_cur_search_with_match_bytes()` 的二分循环、线性循环、落点判断几乎逐行相同，只有比较调用与匹配度合并方式不同。收益是热路径上没有间接调用开销，且两个函数能各自使用不同的类型（`ulint` 对 `ulint` 与 `ulint`）；代价是任何算法改动必须同步改两处，两份实现容易漂移。

- **源码事实：字节版刻意不支持 R-tree，普通版支持。** `page_cur_search_with_match()` 有 `dict_index_is_spatial(index) && mode > PAGE_CUR_LE` 的转交分支，字节版没有。调用方 `btr_cur_search_to_nth_level()` 也用 `!dict_index_is_spatial(index)` 把字节版排除在 R-tree 之外。收益是 R-tree 的 MBR 语义不会被字节前缀比较污染；代价是空间索引拿不到字节级匹配度。

- **源码事实：捷径只在「连续同向顺序插入」这一种模式下生效，且阈值是方向计数大于 3。** 收益是追加型负载（自增主键）上页内定位几乎不需要二分；代价是这个优化只对写方向敏感，一旦写入方向反转（`PAGE_DIRECTION` 变成 `PAGE_LEFT`）或记录被删除（`PAGE_LAST_INSERT` 被清空），优化立刻失效并退回完整二分。

- **源码事实：记录的字段偏移数组可以走「索引级缓存」这条快路，但有前置条件。** `page_cur_search_with_match()` 的 `get_mid_rec_offsets` lambda 先取 `const ulint *const cached_offsets{index->rec_cache.offsets};`，只有当 `cached_offsets` 非空**且** `!page_cur_has_null(mid_rec, index)` 时才复用缓存，否则调 `rec_get_offsets()` 现算。`page_cur_has_null()` 的函数体扫描记录前面的 NULL 位图（`rec - (1 + REC_N_NEW_EXTRA_BYTES)` 起，按 `index->rec_cache.nullable_cols` 折算的字节数逐位检查），一旦发现相关列上有 NULL 就返回真。debug 构建下还有一组断言：缓存只在 `dict_table_is_comp(index->table)`、无虚拟列、无 instant 列、无行版本、非空间索引时可用。收益是省掉每条记录一次的偏移解析；代价是带 NULL 的记录完全用不上缓存，且这条快路只在普通版里有——字节版每次都调 `rec_get_offsets()`。

- **架构推论：** 把「匹配度」与「匹配字节数」做成接口的一部分，等于把「比较语义」的知识下沉到了存储引擎层，调用方不需要二次比较就能判断唯一性。对主核的启示是：索引接口若只返回「找到 / 没找到」，唯一约束去重就必须再比一次；若返回「匹配了几段键」，去重就是零额外比较的判断。

- **架构推论：** 本机制的可移植性被 `page_cur_t` 这一游标结构绑死在 InnoDB 的页模型上——它假定「页有槽位数组、槽位拥有记录链、记录链是单向链表」。主核若要做等价的页内定位，需要在自己的页模型上重建这套「稀疏有序索引 + 稠密链表」的组合，而不是照搬二分代码。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `storage/innobase/page/page0cur.cc` | `page_cur_search_with_match` | 主定位函数：先试 `page_cur_try_search_shortcut()` 捷径；空间索引且 `mode > PAGE_CUR_LE` 时转交 `rtr_cur_search_with_match()`；先在 `page_dir_get_n_slots(page)` 个槽位上二分（`mid = (low + up) / 2`，循环条件 `up - low > 1`），再在低位槽的记录链上线性扫（`page_rec_get_next_const(low_rec) != up_rec`）；`cmp == 0` 时 `PAGE_CUR_G` / `PAGE_CUR_LE` 收下界、否则收上界；最终按 `mode <= PAGE_CUR_GE` 落 `up_rec` 否则落 `low_rec`，回传 `up_matched_fields` / `low_matched_fields` |
| `storage/innobase/page/page0cur.cc` | `page_cur_search_with_match_bytes` | 字节级版本：骨架同上，比较走 `cmp_dtuple_rec_with_match_bytes()`，匹配度合并走 `ut_pair_min()`（按「字段数 + 字节数」成对取小），额外回传 `up_matched_bytes` / `low_matched_bytes`；没有 R-tree 转交分支，也没有 `index->rec_cache.offsets` 缓存快路 |
| `storage/innobase/page/page0cur.cc` | `page_cur_try_search_shortcut` | 捷径本体（仅普通版路径）：取 `PAGE_LAST_INSERT` 记录 `rec`，用 `tuple->compare()` 夹逼判断目标是否落在 `rec` 与 `page_rec_get_next_const(rec)` 之间，成立则 `page_cur_position(rec, block, cursor)` 并返回成功 |
| `storage/innobase/page/page0cur.cc` | `page_cur_try_search_shortcut_bytes` | 字节版捷径：用 `cmp_dtuple_rec_with_match_bytes()` 做同样的两道夹逼判断 |
| `storage/innobase/page/page0cur.cc` | `page_cur_has_null` | 判定记录在 `index->rec_cache.nullable_cols` 指定的列上是否含 NULL：扫描 `rec - (1 + REC_N_NEW_EXTRA_BYTES)` 起的 NULL 位图；为真则禁用缓存的 offsets 数组 |
| `storage/innobase/page/page0cur.cc` | `page_cur_rec_field_extends` | `PAGE_CUR_LE_OR_EXTENDS` 开关下的辅助函数：判断记录第 n 个字段是否比元组第 n 个字段更长（或等长）且前缀相同，成立的类型包括 `DATA_VARCHAR` / `DATA_CHAR` / `DATA_FIXBINARY` / `DATA_BINARY` / `DATA_BLOB` / `DATA_GEOMETRY` / `DATA_VARMYSQL` / `DATA_MYSQL` |
| `storage/innobase/page/page0cur.cc` | `page_cur_delete_rec` | 目录维护侧：删记录时先 `page_header_set_ptr(page, page_zip, PAGE_LAST_INSERT, nullptr)` 清掉捷径依据，再递减拥有槽 `n_owned`，当 `cur_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED` 时调 `page_dir_balance_slot()` |
| `storage/innobase/page/page0cur.cc` | `page_cur_insert_rec_low` | 目录维护侧：插入后递增拥有者记录的 `n_owned`，当 `n_owned == PAGE_DIR_SLOT_MAX_N_OWNED` 时调 `page_dir_split_slot()`；同一函数里还维护 `PAGE_DIRECTION` / `PAGE_N_DIRECTION`（捷径判据的来源） |
| `storage/innobase/page/page0page.cc` | `page_dir_find_owner_slot` | 槽位归属定位：沿记录链找到 `n_owned != 0` 的记录，再在槽位数组里线性找偏移等于它的槽位；找不到时报 `ER_IB_MSG_860` / `ER_IB_MSG_861` 并 `ut_error` |
| `storage/innobase/page/page0page.cc` | `page_dir_split_slot` | 槽位分裂：断言 `n_owned == PAGE_DIR_SLOT_MAX_N_OWNED + 1`，按 `n_owned / 2` 找中点记录，`page_dir_add_slot()` 加一个新槽并平分 `n_owned` |
| `storage/innobase/page/page0page.cc` | `page_dir_balance_slot` | 槽位平衡：断言 `n_owned == PAGE_DIR_SLOT_MIN_N_OWNED - 1`；上邻槽 `n_owned > PAGE_DIR_SLOT_MIN_N_OWNED` 时转交一条记录，否则 `page_dir_delete_slot()` 合并 |
| `storage/innobase/page/page0page.cc` | `page_dir_delete_slot` | 合并槽位：把被删槽的 `n_owned` 并入上邻槽，依次前移槽内容，最后把页头 `PAGE_N_DIR_SLOTS` 减一 |
| `storage/innobase/page/page0page.cc` | `page_dir_slot_check` | 槽位不变式校验：首槽 `n_owned == 1`；末槽与中间槽的 `n_owned` 落在 `[PAGE_DIR_SLOT_MIN_N_OWNED, PAGE_DIR_SLOT_MAX_N_OWNED]` |
| `storage/innobase/page/page0page.cc` | `page_create_low` | 初始目录形态：`page[PAGE_HEADER + PAGE_N_DIR_SLOTS + 1] = 2;`，即空页只有 infimum 与 supremum 两个槽位 |
| `storage/innobase/btr/btr0cur.cc` | `btr_cur_search_to_nth_level` | 本机制的真实调用方：叶层且 `btr_search_enabled` 且非空间索引时调 `page_cur_search_with_match_bytes()`（注释说明是为了更新自适应哈希索引），否则调 `page_cur_search_with_match()`；重试路径上为已走过的每一层页重放 `page_cur_search_with_match()` |
| `storage/innobase/rem/rem0cmp.cc` | `cmp_dtuple_rec_with_match_bytes` | 字节版定位所用的比较函数（比较语义本体，属 G05 记录比较） |

## 与主核 / AI 的关系

主核（`engine/db`）当前没有 InnoDB 这套「稀疏有序槽位数组 + 稠密记录链表」的页内结构：`engine/db/index/` 层是自研的多模态索引，页内记录组织没有独立的「页目录」这一层。

**架构推论：** InnoDB 用「目录槽位指向记录链上的第一条、槽位自己记 `n_owned`」这种稀疏索引，让页内定位不必为每条记录都付出索引结构成本——只有 `n_owned` 跨越 `[PAGE_DIR_SLOT_MIN_N_OWNED, PAGE_DIR_SLOT_MAX_N_OWNED]` 边界时才扩槽或并槽。主核若要给可变长记录做页内定位，这个「稀疏索引 + 密度上下界」的结构是可以直接借鉴的：它把索引空间开销与记录条数解耦，而不是为每条记录付一个固定索引项。

**架构推论：** 本机制把「匹配了多少个字段」作为出参而不是布尔值，对 AI 参与的写入路径有直接含义——AI 生成的 SQL 往往只带部分键值（只有前缀、缺唯一键全列），这类请求在 InnoDB 里不会「失败」，而是返回一个小于唯一键字段数的 `match` 计数，由上层（`row0ins.cc` 的 `row_ins_sec_index_entry_low` 一类调用方）据此判定为「唯一索引冲突」。主核要让 AI 处理唯一约束冲突，就需要让索引接口返回「键匹配到第几段」这一可解释信息，而不是笼统的「写入失败」。

**架构推论：** 快捷路径的判据（连续同向插入、方向计数大于 3、上次插入点存在）本质上是对**访问模式**的在线统计。这类「先按模式选快路、模式不成立就退回通用算法」的结构，是把 AI 生成的负载特征（往往偏追加、偏批量）显式纳入存储引擎决策的接口点；主核若要在 AI 参与下做存储层自适应，可以沿用同一形态：把负载特征作为**可观测状态**记录在页上，定位路径读该状态决定走哪条路，并保证快路失败时必然有明确的通用回退分支。

## 参考

- [MySQL 源码：`storage/innobase/page/page0cur.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/page/page0cur.cc) —— `page_cur_search_with_match()` 与 `page_cur_search_with_match_bytes()` 的完整实现，以及 `page_cur_try_search_shortcut()` / `page_cur_try_search_shortcut_bytes()` / `page_cur_has_null()` / `page_cur_rec_field_extends()` 与目录维护侧的 `page_cur_insert_rec_low()` / `page_cur_delete_rec()`。
- [MySQL 源码：`storage/innobase/page/page0page.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/page/page0page.cc) —— 页目录槽位结构一侧：`page_dir_find_owner_slot()` / `page_dir_split_slot()` / `page_dir_balance_slot()` / `page_dir_delete_slot()` / `page_dir_slot_check()` / `page_create_low()`。
- [MySQL 源码：`storage/innobase/btr/btr0cur.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/btr/btr0cur.cc) —— 调用方 `btr_cur_search_to_nth_level()`：叶层字节前缀比较与完整字段比较两条分支的划分，以及重试路径上对每层页重放 `page_cur_search_with_match()`。
- [MySQL 源码：`storage/innobase/rem/rem0cmp.cc`（mysql-8.0.40）](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.0.40/storage/innobase/rem/rem0cmp.cc) —— 字节版定位所依赖的比较函数 `cmp_dtuple_rec_with_match_bytes()` 的实现。
- [MySQL 参考手册：InnoDB Physical Structure（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-physical-structure.html) —— 页与表空间的物理结构，是理解「页目录 / 槽位」这一层前提的官方材料。
- [MySQL 参考手册：Introduction to InnoDB（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-introduction.html) —— InnoDB 的存储与索引组织背景。
- [MySQL 参考手册：InnoDB Indexes（Oracle 官方镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-indexes.html) —— InnoDB 索引类型与聚簇索引 / 二级索引的组织方式，说明页目录之上的 B-tree 层次。
