---
project: mysql-server
feature: record-compare
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/rem/rem0cmp.cc
    symbols: [cmp_dtuple_rec_with_match_low, cmp_dtuple_rec]
---

# 记录比较

## 一句话职责

InnoDB 把「两条记录谁的键值更大」这件事收敛在一个文件里：`storage/innobase/rem/rem0cmp.cc` 里的比较函数按字段类型分派（整数、浮点、定长/变长字节、字符集排序规则、DECIMAL、GIS MBR），并向上返回「在第几个字段上分出高下」，供 B 树定位、页目录二分、插入位置判定、重复键检测共用。

## 机制是什么

### 边界

- 负责：两条记录按键值的比较（定长与变长字段各自规则）以及比较函数的构造。
- 不负责：页目录的定位（G03 页目录与记录定位）、排序键的业务语义（C 组）。

### 排序规则的两条基线

源码文件头部注释给出了 InnoDB 的字母序规则：取两条记录**第一个不一致的字段**；该字段若在某个字节位置不同，就比较该位置的字符（源码注释：possibly after collating transformation，先做排序规则变换）；若字符都相同但长度不同，则可填充（paddable）的类型用填充字符补齐短的那个，不可填充的类型则**长者更大**；最后，**SQL NULL 比任何其他值都大**。

注意这两条在不同代码路径上并不统一，页面里逐条对照：

- `cmp_data` 里对 NULL 的处理是 `return ((len1 == UNIV_SQL_NULL) == is_asc ? -1 : 1);`，注释写明「We define the SQL null to be the smallest possible value of a field」——即升序时 NULL 最小。
- `cmp_dtuple_rec_with_match_bytes` 里同样是「SQL null is the smallest possible value of a field in the alphabetical order」。
- 而文件头注释声称 NULL 最大。二者矛盾。**架构推论：** 文件头注释是 1994 年遗留的旧描述，实际生效的语义由 `cmp_data` / `cmp_dtuple_rec_with_match_bytes` 两处注释与实现给出（NULL 最小）；页面上以实现为准。

### 三层结构

比较被分成三层，每层都可单独调用：

1. **记录级入口**：`cmp_dtuple_rec`、`cmp_rec_rec_simple`、`cmp_rec_rec_with_match`、`cmp_dtuple_is_prefix_of_rec`，负责两端的取数与遍历控制。
2. **字段级循环**：`cmp_dtuple_rec_with_match_low`（按字段粒度）与 `cmp_dtuple_rec_with_match_bytes`（按字节粒度），负责决定比哪些字段、往哪个方向比、报告第几个字段匹配上。
3. **单值比较**：`cmp_data` / `cmp_whole_field` / `innobase_mysql_cmp` / `cmp_decimal` / `cmp_geometry_field`，负责具体类型的比较规则与填充规则。

### 比较函数是怎么「构造」的

比较函数不预先构造，而是**在调用时按索引元数据现场决定方向**：

- 方向来自 `index->get_field(i)->is_ascending`（`cmp_dtuple_rec_with_match_low` 中），降序索引的比较结果在 `cmp_data` 出口处整体取反。
- `cmp_dtuple_rec_with_match_bytes` 同样取 `index->fields[cur_field].is_ascending`，但额外让 change buffer（insert buffer）索引固定升序：`const bool is_ascending = dict_index_is_ibuf(index) || index->fields[cur_field].is_ascending;`。
- `cmp_rec_rec_with_match` 里，insert buffer 索引直接伪造类型：`mtype = DATA_BINARY; prtype = 0; is_asc = true;`。
- 空间索引非叶子页的第 1 个字段不取字典信息，固定为 `mtype = DATA_SYS_CHILD; prtype = 0; is_asc = true;`——因为该页不含主键信息，只有 MBR 与子页号两个字段。

## 怎么实现的

### 入口一：`cmp_dtuple_rec` —— 一行转手给 `cmp_dtuple_rec_with_match_low`

```cpp
int cmp_dtuple_rec(const dtuple_t *dtuple, const rec_t *rec,
                   const dict_index_t *index, const ulint *offsets) {
  ulint matched_fields = 0;

  return (dtuple->compare(rec, index, offsets, &matched_fields));
}
```

`dtuple_t::compare` 的实现体是：

```cpp
int dtuple_t::compare(const rec_t *rec, const dict_index_t *index,
                      const ulint *offsets, ulint *matched_fields) const {
  return (cmp_dtuple_rec_with_match_low(this, rec, index, offsets, n_fields_cmp,
                                        matched_fields));
}
```

所以 `cmp_dtuple_rec` 的语义完全由 `cmp_dtuple_rec_with_match_low` 决定，比较的字段数是元组的 `n_fields_cmp`（比较字段数），不是总字段数。

### 入口二：`cmp_dtuple_rec_with_match_low` —— 字段粒度的核心

前置断言：`ut_ad(n_cmp > 0)`、`ut_ad(n_cmp <= dtuple_get_n_fields(dtuple))`、`ut_ad(*matched_fields <= n_cmp)`（`matched_fields` 等于 `DISABLE_MIN_REC_FLAG_CHECK` 时除外）。

**第一步：最小记录标志的处理。** 仅当 `*matched_fields == 0` 时进入：

```cpp
if (rec_info & REC_INFO_MIN_REC_FLAG) {
  return (!(tup_info & REC_INFO_MIN_REC_FLAG));
} else if (tup_info & REC_INFO_MIN_REC_FLAG) {
  return (-1);
}
```

`REC_INFO_MIN_REC_FLAG` 是 B 树最左节点指针记录（infimum node pointer）的标志位；注释明确它「比任何其他节点指针都小，与 ASC/DESC 标志无关」。若 `*matched_fields == DISABLE_MIN_REC_FLAG_CHECK`，则把这个检查整个跳过（把 `*matched_fields` 置 0 继续比字段）。

**第二步：逐字段比较。** 循环 `for (auto i = *matched_fields; i < n_cmp; ++i)`，每轮取元组第 `i` 个字段与记录第 `i` 个字段：

- 断言不允许碰到外存字段（`rec_offs_nth_extern`）与默认值字段（`rec_offs_nth_default`），注释说明理由：外存字段只出现在聚簇索引叶子页记录上，而此时主键字段应该已经分出高下。
- 多值索引字段（`dfield_is_multi_value`）且长度为 `UNIV_MULTI_VALUE_ARRAY_MARKER` 或 `UNIV_NO_INDEX_VALUE` 时，不走普通比较：长度为 `UNIV_NO_INDEX_VALUE` 直接返回 `ret = 1`；否则取 `multi_value_data *mv_data`，用 `mv_data->has(type, rec_b_ptr, rec_f_len)` 判断记录里的值是否落在多值集合内，命中返回 0，否则返回 1。
- 普通字段调用 `cmp_data(type->mtype, type->prtype, dict_index_is_ibuf(index) || index->get_field(i)->is_ascending, dtuple_b_ptr, dtuple_f_len, rec_b_ptr, rec_f_len)`。

**第三步：报告匹配位置。** 某字段比较出非零结果时，把 `*matched_fields = i` 并返回该结果；全部字段都比完则 `*matched_fields = n_cmp` 并返回 0（注释：元组与记录在公共字段上相等）。

`matched_fields` 是这套机制对外暴露的核心信息：调用方据此判断「前 k 个字段相同」，从而区分「不是重复键」「完全相同」「部分相同」。

### 单值比较：`cmp_data` —— 填充与字节比较

`cmp_data` 的分支顺序即语义顺序：

**NULL 优先。** `len1 == UNIV_SQL_NULL || len2 == UNIV_SQL_NULL` 时，若两者都是 NULL 返回 0，否则 `return ((len1 == UNIV_SQL_NULL) == is_asc ? -1 : 1);`。

**按类型选填充符 `pad`。**

| mtype | 条件 | pad |
|---|---|---|
| `DATA_FIXBINARY` / `DATA_BINARY` | 字符集不是 `DATA_MYSQL_BINARY_CHARSET_COLL` | `0x20`（空格） |
| `DATA_FIXBINARY` / `DATA_BINARY` | 字符集是 `DATA_MYSQL_BINARY_CHARSET_COLL` | `ULINT_UNDEFINED`（落到下面的分支） |
| `DATA_INT` / `DATA_SYS_CHILD` / `DATA_SYS` | — | `ULINT_UNDEFINED` |
| `DATA_POINT` / `DATA_VAR_POINT` / `DATA_GEOMETRY` | 且 `prtype & DATA_GIS_MBR` | 直接走 `cmp_whole_field` |
| `DATA_POINT` / `DATA_VAR_POINT` / `DATA_GEOMETRY` | 非 MBR | `ULINT_UNDEFINED` |
| `DATA_BLOB` | `prtype & DATA_BINARY_TYPE` | `ULINT_UNDEFINED` |
| 其它（default） | — | 直接走 `cmp_whole_field` |

**字节比较与长度比较。** 先把较短的一方截到公共长度 `len`，长的那方保留剩余长度（`len1` / `len2` 之一被置 0）。

在 `__i386__` / `__x86_64__` / `_M_IX86` / `_M_X64` 上，前 `4 + (len & 3)` 个字节用循环逐个比（`cmp = int(*data1++) - int(*data2++)`），注释解释这是为了避开 GCC 内建 `memcmp`（`repz cmpsb`）在 x86 上的慢路径，并引用了 GCC bug 43052；`len & 3` 用于把长度「量化」到 4 的倍数，因为注释说 x86-64 上 glibc 的 `memcmp` 在 `len=4` 时比 `len=3` 快。剩余部分再调 `memcmp(data1, data2, len)`。非 x86 架构直接走 `memcmp`。

公共前缀相等后，比较长度差 `cmp = (int)(len1 - len2)`：

- `cmp == 0` → 返回 0。
- `pad == ULINT_UNDEFINED` → 直接返回长度差（长者更大），这正是「不可填充的类型长者更大」这条规则的落点。
- 否则（有填充符）→ 进入填充比较循环：用 `mach_read_from_1(&data1[len])` 与 `pad` 逐字节相减，找出第一个非填充字节决定大小。

### 类型分派：`cmp_whole_field`

`cmp_whole_field` 按 `mtype` 分派：

- `DATA_DECIMAL` → `cmp_decimal`。
- `DATA_DOUBLE` → `mach_double_read` 后比较；`DATA_FLOAT` → `mach_float_read` 后比较。两者都在 `is_asc` 为假时取反。
- `DATA_VARCHAR` / `DATA_CHAR` → 固定用 `my_charset_latin1.coll->strnncollsp(&my_charset_latin1, a, a_length, b, b_length)`。
- `DATA_BLOB` 且 `prtype & DATA_BINARY_TYPE` → 先报 `ib::error(ER_IB_MSG_920) << "Comparing a binary BLOB using a character set collation!"`，并 `ut_d(ut_error)`（调试版断言，发布版继续往下走），然后 `[[fallthrough]]` 到 `DATA_VARMYSQL` / `DATA_MYSQL` 分支。
- `DATA_VARMYSQL` / `DATA_MYSQL` → `innobase_mysql_cmp(prtype, a, a_length, b, b_length)`。
- `DATA_POINT` / `DATA_VAR_POINT` / `DATA_GEOMETRY` → `cmp_geometry_field`。
- 其它 → `ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_921) << "Unknown data type number " << mtype;`（致命错误，进程终止）。

`innobase_mysql_cmp` 从 `dtype_get_charset_coll(prtype)` 取字符集编号，`get_charset(cs_num, MYF(MY_WME))` 拿到 `CHARSET_INFO`；若是 `MYSQL_TYPE_STRING` 且该排序规则的 `pad_attribute == NO_PAD`，先用 `cs->cset->lengthsp` 去掉尾部空格（注释说明这是为了对齐 `Field_string::val_str()` 的行为，因为内部索引比较不经过 Field 类），再调 `cs->coll->strnncollsp(cs, a, a_length, b, b_length)`。字符集找不到时 `ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_919) << "Unable to find charset-collation " << cs_num;`。

`cmp_decimal` 是一套手写的十进制文本比较：先剥掉前导空格；`'-'` 出现时翻转 `swap_flag`（`is_asc` 为真时 `swap_flag = 1`，否则 `-1`）；再跳过 `'+'` 与 `'0'` 前缀；之后先比剩余长度（长度不同则长的更大，按 `swap_flag` 取向），长度相同再逐字节比。

`cmp_geometry_field` 比较 MBR 的角点：用 `mach_double_read` 读 x 坐标，用 `mach_double_read(a + sizeof(double) * SPDIMS)` 读 y 坐标（`SPDIMS` 是空间维数常量）；先比左下角 `(xmin, ymin)`，再比右上角 `(xmax, ymax)`；任一长度小于 `sizeof(double)` 直接返回 0。

### 字节粒度变体：`cmp_dtuple_rec_with_match_bytes`

`cmp_dtuple_rec_with_match_bytes` 比较的字段数是 `dtuple_get_n_fields_cmp(dtuple)`，除了字段数还回报**字节数** `matched_bytes`，因此可以在一次比较后从上次停下的字节位置继续。

- `cur_bytes == 0` 时先处理 NULL：两边都是 NULL 则跳到下一个字段；只有一边是 NULL 时按升序/降序给出 `-1` / `1`。
- `mtype` 属于 `DATA_FIXBINARY` / `DATA_BINARY` / `DATA_INT` / `DATA_SYS_CHILD` / `DATA_SYS`（以及二进制 `DATA_BLOB`）时走**逐字节循环**：填充符由 `cmp_get_pad_char(type)` 给出，循环体内 `rec_byte` / `dtuple_byte` 缺字节时用填充符顶上，填充符是 `ULINT_UNDEFINED` 时表示该侧已到头，直接定序。
- 其余类型走 `cmp_data`，返回非零就定序，返回 0 就跳到下一个字段（并把 `cur_bytes` 归零）。

`cmp_get_pad_char` 的规则：`DATA_FIXBINARY` / `DATA_BINARY` 且是 `DATA_MYSQL_BINARY_CHARSET_COLL` 时返回 `ULINT_UNDEFINED`（注释：「Starting from 5.0.18, do not pad VARBINARY or BINARY columns」）；`DATA_CHAR` / `DATA_VARCHAR` / `DATA_MYSQL` / `DATA_VARMYSQL` 返回 `0x20`；`DATA_GEOMETRY` 返回 `ULINT_UNDEFINED`；`DATA_BLOB` 非二进制返回 `0x20`；其它返回 `ULINT_UNDEFINED`。

### 记录对记录：`cmp_rec_rec_simple` 与 `cmp_rec_rec_with_match`

`cmp_rec_rec_simple` 先比唯一键字段（`dict_index_get_n_unique(index)` 个）。全部相等时：若比较过程中没有遇到 NULL 字段、索引是唯一索引且传入了 `table`，则调用 `innobase_rec_to_mysql(table, rec1, index, offsets1)` 把出错行装进 MySQL 的行缓冲并返回 0——注释写明「Issue a duplicate key error if needed」，即错误由调用方报告。否则继续比剩余的非唯一字段，以维持完整的内部顺序。

`cmp_rec_rec_with_match` 用于比较两条 B 树记录，额外提供两个开关：

- `cmp_btree_recs` 为真时先做最小记录检查：`rec1` 带 `REC_INFO_MIN_REC_FLAG` 返回 `-1`，`rec2` 带该标志返回 `1`。
- `nulls_unequal` 为真且两边都是 NULL 时，返回 `-1` 并把 `*matched_fields` 设为当前字段下标——这是「NULL 与 NULL 不相等」的可选语义，与默认的「NULL 最小」不同。
- 比较到 `i == dict_index_get_n_unique_in_tree(index)` 时停止，因为节点指针记录不需要比 node-ptr 字段。
- 空间索引的第 0 个字段会被加上 `DATA_GIS_MBR` 标志（`prtype |= DATA_GIS_MBR`），从而走 MBR 比较。

### 其它辅助入口

- `cmp_dtuple_is_prefix_of_rec`：比较结果为 0 即视为「元组是记录的前缀」；若元组字段数超过记录字段数，调试版 `ut_d(ut_error)`，发布版返回 false。
- `cmp_dtuple_rec_with_gis` / `cmp_dtuple_rec_with_gis_internal`：GIS 专用，按 `page_cur_mode_t` 分派到 `cmp_gis_field`；`PAGE_CUR_MBR_EQUAL` 走 `cmp_geometry_field`，其余模式走 `rtree_key_cmp(mode, a, a_length, b, b_length, srs)`。
- `cmp_cols_are_equal`：判断两列能否直接比较。非二进制字符串类型要求字符集排序规则编号相同（`dtype_get_charset_coll(col1->prtype) == dtype_get_charset_coll(col2->prtype)`）；二进制字符串类型一律可比；`mtype` 不同则不可比；`DATA_INT` 且带符号性不同则不可比（注释：有符号整数的存储格式与无符号不同，正数要 OR `0x8000...`）。

### 真实调用方

| 调用方 | 位置 | 用法 |
|---|---|---|
| `btr_page_split_and_insert` | `storage/innobase/btr/btr0btr.cc` | `cmp_dtuple_rec(tuple, split_rec, cursor->index, *offsets) >= 0` 决定分裂记录的去向 |
| `btr_insert_into_right_sibling` 路径 | `storage/innobase/btr/btr0btr.cc` | `cmp_dtuple_rec(tuple, first_rec, cursor->index, *offsets) < 0` 判断是否需要转到右兄弟页 |
| `btr_page_split_and_insert` | `storage/innobase/btr/btr0btr.cc` | `insert_left = cmp_dtuple_rec(tuple, split_rec, cursor->index, *offsets) < 0;` 决定插入左半页还是右半页 |
| `btr_validate_index` | `storage/innobase/btr/btr0btr.cc` | `if (cmp_dtuple_rec(node_ptr_tuple, node_ptr, index, offsets))` 非零即报 `ER_IB_MSG_42`「Node ptrs differ on levels > 0」 |
| `row_ins_scan_sec_index_for_duplicate` | `storage/innobase/row/row0ins.cc` | `cmp = cmp_dtuple_rec(entry, rec, check_index, offsets);` 结果为 0 才继续加锁 |
| `row_ins_duplicate_online` | `storage/innobase/row/row0ins.cc` | 直接调 `cmp_dtuple_rec_with_match_low(entry, rec, index, offsets, n_uniq + 2, &fields)`，用返回的 `fields` 区分「不是重复」「完全相同」「重复键」三种结论 |
| `row_sel_...`（范围判定） | `storage/innobase/row/row0sel.cc` | `cmp_dtuple_rec(prebuilt->m_stop_tuple, rec, index, offsets)` 与 stop tuple 比较，决定该行是否还在范围内 |
| `row_vers_...` | `storage/innobase/row/row0vers.cc` | `0 == cmp_dtuple_rec(entry, sec_rec, sec_index, sec_offsets)` 判断二级索引记录是否匹配 |
| `page_cur_search_with_match` 及其自适应分支 | `storage/innobase/page/page0cur.cc` | 调 `cmp_dtuple_rec_with_match_bytes(tuple, rec, index, offsets, &low_match, &low_bytes)`，用返回的匹配字段数与匹配字节数做二分边界 |
| `page_cur_rec_field_extends` | `storage/innobase/page/page0cur.cc` | 调 `cmp_data_data(type->mtype, type->prtype, true, ...)` 只判等（注释说明 `is_ascending` 传常量 true，因为只关心是否相等） |

`row_ins_duplicate_online` 是最能说明「为什么需要 `matched_fields`」的例子——它比较 `n_uniq + 2` 个字段（主键字段加 `DB_TRX_ID`、`DB_ROLL_PTR`），然后：

- `fields < n_uniq` → 不是重复，返回 `DB_SUCCESS`；
- `fields == n_uniq + 2` → 记录与元组完全相同，返回 `DB_SUCCESS_LOCKED_REC`；
- 其余 → 返回 `DB_DUPLICATE_KEY`。

## 关键取舍

1. **同一套比较要同时服务「三值逻辑」与「索引有序性」。** `cmp_data` 把 SQL NULL 当作最小值，而 `cmp_rec_rec_with_match` 又提供 `nulls_unequal` 开关让两个 NULL 判为不等。取舍点在于：索引里的 NULL 必须有确定位置才能维持 B 树有序，所以默认取「最小」；而某些校验场景需要严格区分 NULL，取舍通过一个布尔开关完成，不改动默认路径。

2. **字段粒度 vs 字节粒度两套循环并存。** `cmp_dtuple_rec_with_match_low` 只回报字段下标，`cmp_dtuple_rec_with_match_bytes` 额外回报字节偏移。后者能在一次比较后从字节位置续比，页目录二分查找需要这个粒度；前者足够支撑重复键检测。代价是两套代码对填充、NULL、升降序的处理必须保持一致，靠的是两者都最终落到 `cmp_data` / `cmp_get_pad_char` 这两个共同的底层定义。

3. **字符集比较外包给 MySQL 层。** InnoDB 不自己实现排序规则，而是通过 `cs->coll->strnncollsp` 调用 SQL 层的比较函数；`DATA_VARCHAR` / `DATA_CHAR` 更是固定走 `my_charset_latin1` 的排序规则。取舍点是：避免在存储引擎里重复实现加权排序规则，代价是索引比较路径上多一层跨层调用。

4. **用 `is_asc` 在出口处取反来支持降序索引，而不是维护两套比较。** `cmp_data` 的出口统一是 `return (is_asc ? cmp : -cmp);`。好处是降序索引复用同一份代码；代价是降序路径上每次比较都多一次取反。

5. **x86 上的手写字节循环。** `4 + (len & 3)` 的循环上限不是经验值魔法数，而是来自「glibc 在 x86-64 上 `len=4` 的 `memcmp` 比 `len=3` 快」这一实测结论（源码注释原话），并配合 `len & 3` 把长度量化以命中该快路径。

6. **错误分级：调试断言 vs 运行时致命错误。** 类型不匹配这类「不该发生」的情况用 `ut_d(ut_error)`（仅调试版）加 `ib::error`；类型编号未知这种「一定错了」的情况用 `ib::fatal(ER_IB_MSG_921)` 直接终止；字符集找不到用 `ib::fatal(ER_IB_MSG_919)`。

7. **在 `cmp_data` 内部而非调用点处理 padding。** 短者补 `0x20` 的规则写在 `cmp_data` 里（`pad` 变量），而 `cmp_dtuple_rec_with_match_bytes` 的字节循环用 `cmp_get_pad_char` 取同一个填充符。两处填充符来源不同但取值一致：`cmp_data` 里 `DATA_CHAR` / `DATA_VARCHAR` 落到 `default` 分支即 `cmp_whole_field`（走排序规则，不涉及 pad），真正用 `pad = 0x20` 的只有非二进制字符集的 `DATA_FIXBINARY` / `DATA_BINARY`；`cmp_get_pad_char` 里对应地给 `DATA_CHAR` / `DATA_VARCHAR` / `DATA_MYSQL` / `DATA_VARMYSQL` 返回 `0x20`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_rec` | 记录级入口，转手给 `dtuple_t::compare` |
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_rec_with_match_low` | 字段粒度比较，含最小记录标志、多值字段、匹配字段数回报 |
| storage/innobase/rem/rem0cmp.cc | `dtuple_t::compare` | 以 `n_fields_cmp` 为比较字段数调用 `cmp_dtuple_rec_with_match_low` |
| storage/innobase/rem/rem0cmp.cc | `cmp_data` | 单值比较：NULL 语义、填充符选择、字节比较与长度比较 |
| storage/innobase/rem/rem0cmp.cc | `cmp_whole_field` | 按 mtype 分派到 DECIMAL / DOUBLE / FLOAT / 字符串 / GIS 比较 |
| storage/innobase/rem/rem0cmp.cc | `innobase_mysql_cmp` | 按字符集排序规则比较字符串，处理 NO_PAD 排序规则的尾空格裁剪 |
| storage/innobase/rem/rem0cmp.cc | `cmp_decimal` | 十进制文本比较：符号、前导零、长度、逐位比较 |
| storage/innobase/rem/rem0cmp.cc | `cmp_geometry_field` | 比较 MBR 的左下角与右上角坐标 |
| storage/innobase/rem/rem0cmp.cc | `cmp_get_pad_char` | 按类型给出填充字符（`0x20` 空格或 `ULINT_UNDEFINED`） |
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_rec_with_match_bytes` | 字节粒度比较，回报匹配字段数与匹配字节数 |
| storage/innobase/rem/rem0cmp.cc | `cmp_rec_rec_simple` | 记录对记录比较，唯一键相等时走 `innobase_rec_to_mysql` 报重复键 |
| storage/innobase/rem/rem0cmp.cc | `cmp_rec_rec_with_match` | B 树记录比较，支持 `nulls_unequal` 与 insert buffer / 空间索引特例 |
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_is_prefix_of_rec` | 判断元组是否为记录的前缀 |
| storage/innobase/rem/rem0cmp.cc | `cmp_cols_are_equal` | 判断两列是否可直接比较 |
| storage/innobase/rem/rem0cmp.cc | `cmp_data_data` | `cmp_data` 的对外薄封装 |
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_rec_with_gis` | GIS 比较入口，按 `page_cur_mode_t` 分派 |
| storage/innobase/rem/rem0cmp.cc | `cmp_dtuple_rec_with_gis_internal` | GIS 内部比较：先比 MBR，再比第二个字段 |
| storage/innobase/page/page0cur.cc | `page_cur_search_with_match` | 调 `cmp_dtuple_rec_with_match_bytes` 做二分定位 |
| storage/innobase/btr/btr0btr.cc | `btr_page_split_and_insert` | 用 `cmp_dtuple_rec` 决定分裂后记录落左页还是右页 |
| storage/innobase/row/row0ins.cc | `row_ins_duplicate_online` | 用 `cmp_dtuple_rec_with_match_low` 的 `matched_fields` 区分三种重复结论 |
| storage/innobase/row/row0sel.cc | `row_sel_...`（范围判定） | 与 `m_stop_tuple` 比较判断行是否落在范围内 |

## 与主核 / AI 的关系

本仓的主核在 `engine/db/`，与 MySQL 没有代码共享关系。**架构推论：** 主核的 B 树实现里若要做「记录比较」，可以借鉴本机制的两点设计：一是把比较拆成「字段级循环 + 单值比较」两层，让字段类型规则可以各自独立演进；二是让比较函数回报「在第几个字段上分出高下」，把「相等」与「前缀相同」这两种不同语义区分开——本机制里 `matched_fields` 就是这一信息的载体，`row_ins_duplicate_online` 靠它把一次比较的结论细分为三种。

至于 AI 侧：这一层是纯粹的确定性字节/数值比较，不含任何模型推理。把「记录比较」交给 AI 生成的代码没有收益，正确做法是把 InnoDB 这里的规则逐条映射成确定性的类型分派表。

## 参考

- [mysql-server 源码：storage/innobase/rem/rem0cmp.cc（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/storage/innobase/rem/rem0cmp.cc) —— 本页所有符号定义的出处。
- [mysql-server 源码：storage/innobase/rem/rem0cmp.cc（trunk）](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/rem/rem0cmp.cc) —— 最新主干版本，用于对照分支差异。
- [mysql-server 源码：storage/innobase/rem/rem0cmp.cc（8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/rem/rem0cmp.cc) —— 旧版本分支，可看出该文件的历史演化。
- [mysql-server 源码：storage/innobase/page/page0cur.cc（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/storage/innobase/page/page0cur.cc) —— `cmp_dtuple_rec_with_match_bytes` 的调用方，页目录二分定位的实现。
- [MySQL 官方文档：InnoDB 记录比较（Comparison of Records）](https://dev.mysql.com/doc/refman/8.4/en/innodb-comparison-records.html) —— 官方对 InnoDB 记录比较规则的说明。
- [MySQL 官方文档：InnoDB 物理结构（Physical Structure of an InnoDB Table）](https://dev.mysql.com/doc/refman/8.4/en/innodb-physical-structure.html) —— 记录在页内的物理布局背景。
