---
project: mysql-server
feature: compact-record
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/rem/rem0rec.cc
    symbols: [rec_get_offsets, rec_convert_dtuple_to_rec_comp]
---

# 紧凑记录格式

## 一句话职责

`storage/innobase/rem/rem0rec.cc` 里的紧凑（COMPACT/DYNAMIC）记录机制负责把一条逻辑元组（`dtuple_t`）**编码成 InnoDB 索引页里的物理记录**，并把物理记录**解码回字段偏移数组**：写入侧由 `rec_convert_dtuple_to_rec_comp()` 负责记录头的构造、NULL 位图的置位、变长字段长度数组的编码与字段值的落盘；读取侧由 `rec_get_offsets()` 负责从记录头反推出「第 i 个字段的值在记录里的偏移与长度」，供页内比较、字段取值、复制、打印等上层使用。

## 机制是什么

### 物理记录长什么样

`rem0rec.cc` 文件头部有一段专门描述格式的注释块，把 InnoDB 的物理记录分成「OLD STYLE」（REDUNDANT 行格式）与「NEW STYLE」（COMPACT/DYNAMIC 行格式）两套。紧凑格式的注释原文描述如下（自上而下是记录的内存布局，越靠下地址越高）：

```
| length of the last non-null variable-length field of data:
  if the maximum length is 255, one byte; otherwise,
  0xxxxxxx (one byte, length=0..127), or 1exxxxxxxxxxxxxx (two bytes,
  length=128..16383, extern storage flag) |
...
| length of first variable-length field of data |
| SQL-null flags (1 bit per nullable field), padded to full bytes |
| 1 or 2 bytes to indicate number of fields in the record if the table
  where the record resides has undergone an instant ADD COLUMN
  before this record gets inserted; If no instant ADD COLUMN ever
  happened, here should be no byte; So parsing this optional number
  requires the index or table information |
| 4 bits used to delete mark a record, and mark a predefined
  minimum record in alphabetical order |
| 4 bits giving the number of records owned by this record
  (this term is explained in page0page.h) |
| 13 bits giving the order number of this record in the
  heap of the index page |
| 3 bits record type: 000=conventional, 001=node pointer (inside B-tree),
  010=infimum, 011=supremum, 1xx=reserved |
| two bytes giving a relative pointer to the next record in the page |
ORIGIN of the record
| first field of data |
...
| last field of data |
```

由此可以提炼出紧凑记录的四段式结构：

1. **变长字段长度数组（lens）**：位于记录头一侧，**逆序**存放——注释里写明「The offsets of the data fields are stored in an inverted order because then the offset of the first fields are near the origin, giving maybe a better processor cache hit rate in searches」。
2. **NULL 位图（nulls）**：每个可空字段占 1 位，**按整字节对齐补齐**（padded to full bytes）。
3. **记录头（header）**：固定 4+4+13 位描述删除标记、所属记录数、页内顺序号，加 3 位记录类型（conventional / node pointer / infimum / supremum），再加 2 字节指向页内下一条记录的相对指针。
4. **字段数据区**：从 ORIGIN 开始依次落盘各字段的实际字节。

**关键区别：紧凑格式不再为每个字段存偏移。** 定长字段的长度由索引元数据（`dict_field_t::fixed_len`）直接给出，只有变长字段才需要在长度数组里占位——这正是「紧凑」二字的来源。

### 三个「长度」的分工

紧凑记录里有三处长度信息，容易混淆，源码把它们分得很清楚：

| 长度 | 存在哪 | 谁来读 |
|---|---|---|
| 字段的**声明长度**（定长字段的字节数） | 索引元数据 `dict_field_t::fixed_len` | `rec_convert_dtuple_to_rec_comp()` 的 `store_field` lambda |
| 字段的**实际长度**（变长字段） | 记录头里的长度数组 | 同上，编码；`rec_get_n_extern_new()` 解码 |
| 字段的**NULL 标志** | 记录头里的 NULL 位图 | 同上，`store_field` 置位；`rec_get_n_extern_new()` 解码 |

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责变长记录的字段偏移数组、NULL 位图与字段值的编码；**不负责记录的比较语义（rem0cmp）**、**不负责页的目录组织（G03）**。

## 怎么实现的

### 写入侧：`rec_convert_dtuple_to_rec_comp()`

这是本机制的核心写入函数，签名是：

```
static inline Rec_instant_state rec_convert_dtuple_to_rec_comp(
    rec_t *rec, const dict_index_t *index, const dfield_t *fields,
    ulint n_fields, const dtuple_t *v_entry, ulint status, bool temp,
    uint8_t rec_version);
```

它返回 `Rec_instant_state`（`REC_IS_SIMPLE` / `REC_IS_VERSIONED` / `REC_IS_INSTANT`），告诉调用方这条记录属于哪种「instant / version」形态。函数体按下面的顺序推进。

#### 第 1 步：算出可空字段数

```
ulint n_null = 0;
if (n_fields != 0) {
  n_null = get_nullable_fields_for_rec(index, n_fields, rec_version);
}
```

`get_nullable_fields_for_rec()` 是同文件的静态函数，三条分支决定了 `n_null` 的取值：

- 版本无效（`!is_valid_row_version(rec_version)`，源码注释写明「Possible for temp record for redundant format」）→ 把 `rec_version` 置 0，取 `index->get_nullable_in_version(rec_version)`。
- 表有行版本（`index->has_row_versions()`）→ 取 `index->get_nullable_in_version(rec_version)`。
- 表只有 instant 列（`index->has_instant_cols()`）→ 取 `index->get_n_nullable_before(n_fields)`。
- 两者都没有 → 直接取 `index->n_nullable`。

**这四条分支的意义：NULL 位图的位数是「按记录版本/列数动态算出来的」，不是常量。** 同一张表在不同时间写入的记录，可空字段数可以不同。

#### 第 2 步：定位 NULL 位图与长度数组

非临时记录走这一支：

```
nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
```

即 NULL 位图的起点是「记录 ORIGIN 往回退 `REC_N_NEW_EXTRA_BYTES + 1` 字节」——`REC_N_NEW_EXTRA_BYTES` 是紧凑记录头的固定字节数（该常量定义在 `rem0rec.h`，本仓库的稀疏检出未包含该头文件，故此处不引用其数值）。

紧跟着：

```
byte *lens = nullptr;
if (n_fields != 0) {
  lens = nulls - UT_BITS_IN_BYTES(n_null);
  /* clear the SQL-null flags */
  memset(lens + 1, 0, nulls - lens);
}
```

`UT_BITS_IN_BYTES(n_null)` 就是 NULL 位图占用的字节数（位图按字节补齐）。`lens` 落在 NULL 位图的**更低地址**一侧，也就是长度数组在记录头里位于 NULL 位图之前——与文件头注释里的布局顺序一致。`memset` 清掉的是 NULL 标志位区（`lens + 1` 到 `nulls` 之间）。

临时记录（`temp == true`）走另一支：`nulls = rec - 1;`，且 `n_node_ptr_field = ULINT_UNDEFINED;`。若表不是 COMPACT 格式，还要用 `get_extra_bytes_for_temp_redundant()` 额外回退指针。

#### 第 3 步：按 status 分派记录形态

`switch (UNIV_EXPECT(status, REC_STATUS_ORDINARY))` 的四个分支：

- **`REC_STATUS_ORDINARY`**：普通叶层记录，`n_node_ptr_field = ULINT_UNDEFINED`。若索引带 instant 列或行版本（`index->has_instant_cols_or_row_versions()`），还要处理两件事：`is_store_version(index, n_fields)` 为真时用 `rec_set_instant_row_version_new()` 写入当前行版本并把 `nulls` 再退 1 字节（版本号占 1 字节）；否则若 `index->is_tuple_instant_format(n_fields)` 为真，用 `rec_set_n_fields(rec, n_fields)` 写入字段数，并按 `rec_set_n_fields()` 返回的 `n_fields_len` 把 `nulls` 继续下移。
- **`REC_STATUS_NODE_PTR`**：非叶层的节点指针记录。断言字段数等于 `dict_index_get_n_unique_in_tree_nonleaf(index) + 1`，`n_node_ptr_field = n_fields - 1`（最后一个字段是子页号），`n_null` 被改写为 `index->get_nullable_before_instant_add_drop()`。
- **`REC_STATUS_INFIMUM` / `REC_STATUS_SUPREMUM`**：伪记录，断言 `n_fields == 1`，`n_node_ptr_field = ULINT_UNDEFINED`。
- **`default`**：`ut_error;`——非法 status 直接触发断言失败。

#### 第 4 步：逐字段编码（`store_field` lambda）

`store_field` 是本机制真正的「编码器」，签名 `auto store_field = [&](const dfield_t *field, uint32_t pos)`。它按下面的顺序工作：

**（1）处理 NULL。**

```
if (!(dtype_get_prtype(type) & DATA_NOT_NULL)) {
  /* It's a nullable field */
  ut_ad(n_null--);

  if (UNIV_UNLIKELY(!(byte)null_mask)) {
    (nulls)--;
    null_mask = 1;
  }

  ut_ad(*nulls < null_mask);

  /* Set the null bit if field is NULL */
  if (dfield_is_null(field)) {
    *nulls |= null_mask;
    null_mask <<= 1;
    return;
  }

  /* Set null_mask to point to the next field bit in the null bitmap */
  null_mask <<= 1;
}
```

要点有三个：

- 位图是**从低位往高位**消耗的：`null_mask` 初值 1，每处理一个可空字段左移一位；当一个字节被占满（`!(byte)null_mask`）时，`nulls` 指针**下移一个字节**（地址更低）并把 `null_mask` 重置为 1。
- **NULL 字段不写长度、不写数据**，直接 `return`——所以 NULL 字段在记录里只占 NULL 位图里的 1 个比特。
- 非 NULL 的可空字段同样要推进 `null_mask`，但会继续往下走写长度和数据的流程。

**（2）确定字段是定长还是变长。**

```
const dict_field_t *ifield = index->get_physical_field(pos);
uint32_t fixed_len = ifield->fixed_len;
dict_col_t *col = ifield->col;

if (temp && fixed_len && !col->get_fixed_size(temp)) {
  fixed_len = 0;
}
```

定长字段由 `ifield->fixed_len` 决定；临时记录场景下若该列在临时文件里不是定长（`!col->get_fixed_size(temp)`），则强制降级为变长处理。

**（3）编码长度。** 三条互斥分支：

```
if (fixed_len) {
  /* 定长字段：长度数组里不占位 */
} else if (dfield_is_ext(field)) {
  /* 外部存储字段 */
  *lens = (byte)(len >> 8) | 0xc0;
  lens--;
  *lens = (byte)len;
  lens--;
} else {
  /* 普通变长字段 */
  if (len < 128 ||
      !DATA_BIG_LEN_MTYPE(dtype_get_len(type), dtype_get_mtype(type))) {
    *lens = (byte)len;
    lens--;
  } else {
    ut_ad(len < 16384);
    *lens = (byte)(len >> 8) | 0x80;
    lens--;
    *lens = (byte)len;
    lens--;
  }
}
```

**这三个魔数都能追到源码常量或断言：**

| 数值 | 含义 | 出处 |
|---|---|---|
| `0xc0` | 外部存储标记：两字节长度的高字节最高两位为 `11` | `rec_convert_dtuple_to_rec_comp()` 的 `store_field` 分支；文件头注释记作 `1exxxxxxxxxxxxxx` |
| `0x80` | 两字节长度标记：高字节最高位为 `1` | 同上；文件头注释记作 `1exxxxxxxxxxxxxx` |
| `128` | 单字节 / 双字节长度的分界：`len < 128` 走单字节 | `store_field` 的条件判断；文件头注释写明 `length=0..127` |
| `16384` | 双字节长度的上界：`ut_ad(len < 16384)` | `store_field` 的断言；文件头注释写明 `length=128..16383` |

**（4）落盘字段值。**

```
if (len > 0) memcpy(end, dfield_get_data(field), len);
end += len;
```

字段数据从 `end`（初始等于 `rec`，即 ORIGIN）起顺序写入。

#### 第 5 步：字段遍历的两种分支

函数体在 `store_field` 之外还有一个外层的遍历决策：

```
if (index->has_instant_cols_or_row_versions()) {
  ut_ad(index->is_clustered());
  size_t n_total_fields = index->get_n_total_fields();
  ...
  for (size_t i = 0; i < n_total_fields && n_fields_stored < n_fields; i++) {
    ...
    dict_field_t *phy_dict_field = index->get_physical_field(i);
    if (!phy_dict_field->col->is_visible_in_version(rec_version)) {
      continue;
    }
    logical_i = (phy_dict_field - index->fields);
    ...
  }
} else {
  for (size_t i = 0; i < n_fields; i++) { ... }
}
```

- **带 instant 列 / 行版本**时，按**物理顺序**遍历索引的全部字段，用 `col->is_visible_in_version(rec_version)` 跳过当前版本不可见的列，并把物理位置反查回逻辑下标 `logical_i = (phy_dict_field - index->fields)` 去取 `fields[logical_i]` 的值。
- **不带 instant 列 / 行版本**时，直接按下标 `0..n_fields-1` 顺序遍历。

两个分支里都有一条 node pointer 的特殊处理：`if (logical_i == n_node_ptr_field)` 时断言该字段 `DATA_NOT_NULL` 且 `len == REC_NODE_PTR_SIZE`，然后**直接 memcpy 而不走 `store_field`**——节点指针是定长的 8 字节子页号，不进长度数组。

#### 第 6 步：虚拟列的编码（仅临时记录）

只有 `num_v != 0`（`v_entry` 非空）时才走这一段：

```
byte *ptr = end;
ptr += 2;   /* reserve 2 bytes for writing length */

for (ulint col_no = 0; col_no < num_v; col_no++) {
  const dict_v_col_t *col = dict_table_get_nth_v_col(index->table, col_no);
  if (col->m_col.ord_part || !dict_table_is_comp(index->table)) {
    ulint pos = col_no;
    pos += REC_MAX_N_FIELDS;
    ptr += mach_write_compressed(ptr, pos);
    ...
    ptr += mach_write_compressed(ptr, flen);
    ...
  }
}

mach_write_to_2(end, ptr - end);
```

要点：

- 虚拟列数据放在所有普通字段之后，前面预留 **2 字节**写这段的总长度（`mach_write_to_2(end, ptr - end)`）。
- 只有「有序部分」的虚拟列（`col->m_col.ord_part`）或非 COMPACT 表的虚拟列才被记录。
- 列位置编码为 `pos = col_no + REC_MAX_N_FIELDS`，即**用加上 `REC_MAX_N_FIELDS` 的偏移量与普通字段号区分开**。
- 长度用 `mach_write_compressed()` 变长编码；非 NULL 时还要用 `std::min(flen, static_cast<ulint>(DICT_MAX_FIELD_LEN_BY_FORMAT(index->table)))` 截断。

### 写入侧的配套：`rec_get_converted_size_comp_prefix_low()`

写之前必须先知道要占多少字节。同文件的 `rec_get_converted_size_comp_prefix_low()` 负责算这件事，它和 `rec_convert_dtuple_to_rec_comp()` **共用同一套编码规则**：

- 头部大小：`extra_size += temp ? UT_BITS_IN_BYTES(n_null) : REC_N_NEW_EXTRA_BYTES + UT_BITS_IN_BYTES(n_null);`
- 每个字段的长度数组开销：定长字段 `fixed_len` 非零则 **0 字节**；外部存储字段 `extra_size += 2;`；普通变长字段按 `if (len < 128 || !DATA_BIG_COL(col)) extra_size++; else extra_size += 2;`。
- 数据区大小：`data_size += len;`（NULL 字段不计入）。

对比可见：**长度数组里的「定长字段不占位」这条规则，是写路径和算大小路径共享的关键。** `rec_convert_dtuple_to_rec_new()` 正是靠这个函数先算出 `extra_size`，再把 `rec` 指针定位到 `buf + extra_size`，然后才调用 `rec_convert_dtuple_to_rec_comp()`。

### 读取侧：`rec_get_offsets()`

`rec_get_offsets()` 是本机制的读取入口，在 `rem0rec.cc` 里被多处调用。它的定义位于 `rem0rec.h`（本仓库的稀疏检出未包含该头文件），因此这里只描述 `rem0rec.cc` 中**可读到的调用契约**：

```
const ulint *rec_get_offsets(const rec_t *rec, const dict_index_t *index,
                              ulint *offsets, ulint n_fields,
                              Location_location location,
                              mem_heap_t **heap);
```

在 `rem0rec.cc` 中的四个调用点，展示了它的四种典型用法：

1. **`rec_convert_dtuple_to_rec()`（调试校验）**：`offsets = rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);`，随后 `ut_ad(rec_validate(rec, offsets))`，并逐字段比对「dtuple 里是不是外部存储」与「offsets 里是不是外部存储」是否一致（`!dfield_is_ext(dtuple_get_nth_field(dtuple, i)) == !rec_offs_nth_extern(index, offsets, i)`）。
2. **`rec_copy_prefix_to_dtuple()`**：`rec_get_offsets(rec, index, offsets, n_fields, UT_LOCATION_HERE, &heap)`，然后用 `rec_get_nth_field_instant(rec, offsets, i, index, &len)` 逐字段取值，`len == UNIV_SQL_NULL` 时置 NULL，否则 `dfield_set_data(field, mem_heap_dup(heap, data, len), len)`。
3. **`rec_get_trx_id()`**：`offsets = rec_get_offsets(rec, index, offsets, trx_id_col + 1, UT_LOCATION_HERE, &heap);` —— 注意第三个参数是**要解析的字段数**（`trx_id_col + 1`），说明 `rec_get_offsets()` 支持**只解析前 n 个字段**，这正是「记录前缀拷贝」这一优化能成立的前提。
4. **`rec_print()` / `operator<<`**：`rec_get_offsets(rec, index, offsets_, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap)`，解析全部字段。

**`rec_get_offsets()` 输出的 `offsets` 数组被上层当作「记录的事实描述」使用**，这一约定在 `rem0rec.cc` 的注释里被反复标注（`@param[in] offsets Array returned by rec_get_offsets()`）。典型消费方式：

- `rec_offs_n_fields(offsets)` 取字段数；
- `rec_offs_data_size(offsets)` 取数据区字节数；
- `rec_offs_comp(offsets)` 区分紧凑 / REDUNDANT；
- `rec_offs_nth_extern(index, offsets, i)` 判断某字段是否外部存储；
- `rec_offs_nth_default(nullptr, offsets, i)` 判断某字段是否为 instant ADD COLUMN 的默认值（`rec_print()` 的 std::ostream 版本用它在输出里打 `DEFAULT`）。

### 读取侧的配套：`rec_get_n_extern_new()`

`rec_get_n_extern_new()` 演示了**如何纯手工解析紧凑记录头**，是理解编码规则的最佳样本：

```
nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
null_mask = 1;
...
do {
  const dict_field_t *field = index->get_field(i);
  const dict_col_t *col = field->col;
  if (!(col->prtype & DATA_NOT_NULL)) {
    /* nullable field => read the null flag */
    if (UNIV_UNLIKELY(!(byte)null_mask)) {
      nulls--;
      null_mask = 1;
    }
    if (*nulls & null_mask) {
      null_mask <<= 1;
      /* No length is stored for NULL fields. */
      continue;
    }
    null_mask <<= 1;
  }

  if (UNIV_UNLIKELY(!field->fixed_len)) {
    /* Variable-length field: read the length */
    len = *lens--;
    if (DATA_BIG_COL(col)) {
      if (len & 0x80) {
        /* 1exxxxxxx xxxxxxxx */
        if (len & 0x40) {
          n_extern++;
        }
        lens--;
      }
    }
  }
} while (++i < n);
```

这段代码是**编码规则的镜像**：NULL 字段不读长度；定长字段不读长度；变长字段读一字节，若该字节最高位为 1（`len & 0x80`）说明是两字节长度，需要再 `lens--` 读一字节；若两字节高字节的次高位也为 1（`len & 0x40`）则该字段是**外部存储**，`n_extern++`。

注意 `rec_get_n_extern_new()` 用的是 `index->n_nullable`（索引级的可空列总数），而写入侧 `rec_convert_dtuple_to_rec_comp()` 用的是 `get_nullable_fields_for_rec()` 算出的 `n_null`——两者在带行版本/instant 列的表上会不一致，这正是写入侧要按版本动态算 `n_null` 的原因。

### 一致性校验：`rec_validate()`

`rec_validate()` 是紧凑记录的自检入口，它把 `rec_get_offsets()` 的输出当输入：

```
n_fields = static_cast<uint16_t>(rec_offs_n_fields(offsets));

if ((n_fields == 0) || (n_fields > REC_MAX_N_FIELDS)) {
  ib::error(ER_IB_MSG_925) << "Record has " << n_fields << " fields";
  return false;
}

for (i = 0; i < n_fields; i++) {
  rec_get_nth_field_offs(nullptr, offsets, i, &len);
  switch (len) {
    case UNIV_SQL_NULL: ...
    case UNIV_SQL_ADD_COL_DEFAULT: /* INSTANT ADD Column */ ++n_defaults; break;
    case UNIV_SQL_INSTANT_DROP_COL: /* INSTANT DROP Column */ ++n_inst_dropped; break;
    default:
      /* Normal field with non null value */
      ut_a(n_defaults == 0);
      if (len >= UNIV_PAGE_SIZE) {
        ib::error(ER_IB_MSG_926) << "Record field " << i << " len " << len;
        return false;
      }
      len_sum += len;
      break;
  }
}

if (len_sum != rec_offs_data_size(offsets)) {
  ib::error(ER_IB_MSG_927) << "Record len should be " << len_sum << ", len "
                           << rec_offs_data_size(offsets);
  return false;
}
```

三个可追溯的上限/判据：`n_fields > REC_MAX_N_FIELDS` 走错误码 `ER_IB_MSG_925`；单个字段 `len >= UNIV_PAGE_SIZE` 走 `ER_IB_MSG_926`；累加长度与 `rec_offs_data_size(offsets)` 不一致走 `ER_IB_MSG_927`。

### 临时记录的旁路：`rec_serialize_dtuple()` / `rec_deserialize_init_offsets()`

同文件还提供了临时文件（排序建索引时的临时表）专用的两个入口：

```
void rec_serialize_dtuple(rec_t *rec, const dict_index_t *index,
                           const dfield_t *fields, ulint n_fields,
                           const dtuple_t *v_entry, uint8_t rec_version) {
  rec_convert_dtuple_to_rec_comp(rec, index, fields, n_fields, v_entry,
                                 REC_STATUS_ORDINARY, true, rec_version);
}

void rec_deserialize_init_offsets(const rec_t *rec, const dict_index_t *index,
                                  ulint *offsets) {
  rec_init_offsets_comp_ordinary(rec, true, index, offsets);
}
```

**写用 `temp = true` 复用同一个编码器，读用 `rec_init_offsets_comp_ordinary()` 单独解析**——这就是「编码规则只有一份、解码有两条路径」的现状。

## 关键取舍

### 取舍一：定长字段不占长度数组，换取紧凑性

代价是**长度信息分裂成了两处**：定长字段的长度在索引元数据里，变长字段的长度在记录头里。解析时必须先知道「哪些字段是定长」，这让紧凑记录的自描述能力变弱——`rec_get_n_extern_new()` 因此需要 `index->get_field(i)` 才能解析，`rec_get_offsets()` 也必须传入 `index`。

收益是每条记录省下「每个定长字段 1~2 字节」的偏移开销。对于主键 + 少量定长列 + 少量变长列的典型表结构，节省是可观的。

### 取舍二：长度数组逆序存放，换取缓存命中率

文件头注释明确写出了动机：「The offsets of the data fields are stored in an inverted order because then the offset of the first fields are near the origin, giving maybe a better processor cache hit rate in searches.」

代价是**读取方向与写入方向相反**：`store_field` 里长度数组是 `lens--` 递减写入（因为 `lens` 初始在 NULL 位图的低地址侧），而 `rec_get_n_extern_new()` 也是 `lens--` 递减读取。两者同向，但与字段数据区（`end += len` 递增）方向相反。

### 取舍三：NULL 字段不写任何数据，只占 1 个比特

代价是**NULL 字段的长度信息完全不存在**——`store_field` 里 NULL 分支直接 `return`，`rec_get_n_extern_new()` 里对应分支 `continue`。这让 NULL 与「零长度字符串」在物理上无法区分，必须靠位图才能还原。

收益是稀疏表（大量 NULL）的记录体积可以非常小。

### 取舍四：长度用「1 字节 / 2 字节」可变编码，换取自适应

`len < 128` 用 1 字节，否则用 2 字节，上界由 `ut_ad(len < 16384)` 断言守住。代价是**解析变长字段时必须先判断类型**（`DATA_BIG_COL(col)`）才知道该读 1 字节还是 2 字节，且这个判断依赖列的最大长度而非当前值的实际长度。

这直接导致了 `rec_convert_dtuple_to_rec_comp()` 与 `rec_get_converted_size_comp_prefix_low()` 两处必须用**完全一致**的条件（一个用 `DATA_BIG_LEN_MTYPE(dtype_get_len(type), dtype_get_mtype(type))`，一个用 `DATA_BIG_COL(col)`）——两处条件若不一致，算出来的大小和实际写入的大小就对不上。

### 取舍五：外部存储字段复用两字节长度并用 `0xc0` 标记

代价是**外部存储与「长度 128~16383 的普通变长字段」共用同一套两字节编码**，必须靠 `0xc0` 里的次高位区分。`rec_get_n_extern_new()` 里的 `if (len & 0x80) { if (len & 0x40) { n_extern++; } lens--; }` 就是这套区分逻辑。

### 取舍六：instant 列 / 行版本让 NULL 位图宽度变成动态量

`get_nullable_fields_for_rec()` 的四条分支说明：同一张表在不同时间点写入的记录，其 NULL 位图的宽度**不相同**——`index->get_nullable_in_version(rec_version)` 按版本取，`index->get_n_nullable_before(n_fields)` 按字段数取，`index->n_nullable` 才是全表固定值。这增加了编码与解码的复杂度（写入侧要按 `rec_version` 动态算，读取侧若只用 `index->n_nullable` 就会算错），但换来了「加列 / 改版本不需要重写历史行」的能力。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/rem/rem0rec.cc | `rec_convert_dtuple_to_rec_comp` | 紧凑记录的编码器：构造记录头、置 NULL 位图、编码变长字段长度数组、落盘字段值、处理虚拟列 |
| storage/innobase/rem/rem0rec.cc | `rec_get_offsets` | 紧凑记录的解码入口：返回「字段数 / 数据区大小 / 是否外部存储 / 每字段偏移」构成的 offsets 数组（本文件内为调用点，定义在 `rem0rec.h`） |
| storage/innobase/rem/rem0rec.cc | `rec_get_converted_size_comp_prefix_low` | 编码前的配套尺寸计算：与编码器共用同一套长度编码规则，算出头部长度与数据区大小 |
| storage/innobase/rem/rem0rec.cc | `rec_convert_dtuple_to_rec_new` | 编码器的调用方：先算 `extra_size` 定位 ORIGIN，再调 `rec_convert_dtuple_to_rec_comp`，最后按返回的 `Rec_instant_state` 设置记录标志 |
| storage/innobase/rem/rem0rec.cc | `rec_convert_dtuple_to_rec` | 对外入口：按 `dict_table_is_comp(index->table)` 分派到紧凑或 REDUNDANT 路径，并在调试构建下回读校验 |
| storage/innobase/rem/rem0rec.cc | `get_nullable_fields_for_rec` | 按记录版本 / instant 列状态算出这条记录的可空字段数，即 NULL 位图的宽度 |
| storage/innobase/rem/rem0rec.cc | `rec_get_n_extern_new` | 紧凑记录头的纯手工解析示例：演示 NULL 位图与两字节长度的解码规则，统计外部存储字段数 |
| storage/innobase/rem/rem0rec.cc | `rec_copy_prefix_to_buf` | 记录前缀拷贝：定位 NULL 位图与长度数组后逐字段累加长度，得出前缀字节数 |
| storage/innobase/rem/rem0rec.cc | `rec_copy_prefix_to_dtuple` | 把记录前 n 个字段还原成 dtuple，经由 `rec_get_offsets()` + `rec_get_nth_field_instant()` |
| storage/innobase/rem/rem0rec.cc | `rec_validate` | 紧凑记录一致性自检：字段数上限、字段长度上限、累加长度与 `rec_offs_data_size()` 比对 |
| storage/innobase/rem/rem0rec.cc | `rec_serialize_dtuple` | 临时文件记录序列化，调用编码器的 `temp = true` 分支 |
| storage/innobase/rem/rem0rec.cc | `rec_deserialize_init_offsets` | 临时文件记录偏移初始化，走 `rec_init_offsets_comp_ordinary()` |
| storage/innobase/rem/rem0rec.cc | `rec_get_trx_id` | 读取 DB_TRX_ID：演示 `rec_get_offsets()` 的「只解析前 `trx_id_col + 1` 个字段」用法 |
| storage/innobase/rem/rem0rec.cc | `rec_print` / `rec_print_new` / `rec_print_comp` | 记录打印：以 `rec_get_offsets()` 的输出为输入，逐字段区分 `DEFAULT` / `NULL` / 普通值 / 外部存储 |

## 与主核 / AI 的关系

**架构推论：** 本页描述的紧凑记录格式是 InnoDB 的**存储引擎私有格式**，位于 `storage/innobase/` 之下，与 Server 层（`sql/` 目录）之间没有直接的数据结构耦合——Server 层只通过 handler 接口（`sql/handler.cc`）收发逻辑行。**架构推论：** 若主核（`engine/db/`）要实现一个兼容 InnoDB 的存储层，需要复刻的是本机制的**编码规则**（定长字段不占长度数组、NULL 字段只占 1 比特、长度 1/2 字节可变编码、`0x80` / `0xc0` 标记位、逆序长度数组），而不是它的函数结构。

**架构推论：** 对 AI 检索场景而言，本机制的特点是「**记录自描述能力弱**」——要解释一条紧凑记录，必须同时给出索引元数据（哪些字段定长、哪些可空）才能完整解码。这意味着任何基于紧凑记录原文的检索都必须把「记录字节 + 索引 schema」当成一个整体，否则解出来的字段边界是错的。

**架构推论：** 反过来，紧凑格式的「定长字段不占长度数组」这一条，对 AI 友好的按列访问不友好——取第 k 个字段的值必须从头顺序累加前面所有字段的长度，无法随机访问。这与 G03 页的页目录二分查找不同：页目录能定位到**记录**，但不能定位到**记录内的字段**。

## 参考

- [InnoDB Row Formats（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-row-format.html) —— COMPACT / DYNAMIC 行格式的官方说明。
- [The Physical Structure of an InnoDB Index（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-physical-structure.html) —— InnoDB 索引页与记录的物理结构。
- [storage/innobase/rem/rem0rec.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/rem/rem0rec.cc) —— 本页全部符号的权威来源。
- [storage/innobase/rem/rem0rec.cc（raw 文本，GitHub）](https://raw.githubusercontent.com/mysql/mysql-server/8.0/storage/innobase/rem/rem0rec.cc) —— 便于直接检索的纯文本版本。
