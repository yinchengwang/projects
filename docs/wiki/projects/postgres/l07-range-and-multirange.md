---
project: postgres
feature: range-and-multirange
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/rangetypes.c
    symbols: [range_in, range_out, range_contains]
  - path: src/backend/utils/adt/multirangetypes.c
    symbols: [make_multirange, multirange_contains_elem]
---

# 范围类型

## 一句话职责

范围类型以一对可包含或不包含的边界表示连续区间，多重范围则把多个互不重叠的区间装进一个规范化值。

## 机制是什么

`RangeType` 是 varlena 值：头部之后紧跟自身类型 OID，随后是按元素类型对齐的零到两个边界值，末字节是标志位（`RANGE_EMPTY`、`RANGE_LB_INC`、`RANGE_UB_INC`、`RANGE_LB_INF`、`RANGE_UB_INF`）。序列化时 `make_range` 会按子类型比较规则做规范化（如空区间统一表示、边界可取整），因此同一个数学区间只有一种规范二进制形式。

`MultirangeType` 保存 `multirangetypid`、`rangeCount`、每段的 flags 与偏移/长度条目，以及按子类型对齐的边界数据；构造时先排序再合并相邻或重叠的段，保证内部各段互不重叠且有序。范围机制负责区间表示与区间级谓词，不负责子类型的比较实现（由 typcache 中的 `rng_cmp_proc_finfo` 与 collation 提供），也不负责 GiST 索引页结构。

## 怎么实现的

1. `range_in` 用 `get_range_io_data` 缓存子类型的输入函数与 typioparam，`range_parse` 解析 `[`/`(`、边界文本、`]`/`)` 以及 `empty`、无穷边界，产出 flags 与两个边界字符串。
2. 对存在的边界调用子类型输入函数，构造 `RangeBound`（`val`、`infinite`、`inclusive`、`lower`），再交 `make_range` 做规范化与序列化；解析失败或子类型输入失败时按 escontext 返回 NULL 或报错。
3. `range_out` 反向：`range_deserialize` 把二进制拆成两个 `RangeBound` 与 empty 标志，按 flags 判断是否有界，调用子类型输出函数后由 `range_deparse` 拼成外部文本。
4. `range_contains` 取本范围的 typcache 后调 `range_contains_internal`：若被包含范围为空则恒真（空集是任何集合的子集），若容器为空则恒假；否则要求容器下界不大于对方下界、上界不小于对方上界，边界包含性由 `range_cmp_bounds` 一并处理。
5. `make_multirange` 先调 `multirange_canonicalize`：用 `range_compare` 对输入区间排序，丢弃空区间，再用 `range_adjacent_internal` 判定相接（无重叠的相邻区间）并 `range_union_internal` 合并，有重叠的同样合并，有间隙的则作为新段保留。
6. 规范化后 `multirange_size_estimate` 计算总尺寸并 `palloc0` 分配，写入类型 OID、段数与边界数据。`multirange_contains_elem` 在非空多重范围上调用 `multirange_bsearch_match`，以 `multirange_elem_bsearch_comparison` 对有序段二分查找，命中时要求元素落在该段的下界（含）与上界（含/不含）之内。

## 关键取舍

- **源码事实：**构造路径统一做规范化（空区间表示、边界包含性调整），`make_multirange` 依赖排序后合并相邻/重叠区间。**架构推论：**以构造期的一次排序与合并成本，换取等值比较与包含判断可基于规范表示直接比较，索引（尤其 GiST）无需处理大量等价写法；若保留未合并的原始段，每次谓词都要先做归并。
- **源码事实：**`range_contains_internal` 先判空：被包含区间为空返回 true，容器为空返回 false，随后比较边界。**架构推论：**这与集合论语义一致，使 `contains` 对空区间有确定答案；若把空区间当作普通边界对，会出现上下界比较反常结果。
- **源码事实：**`multirange_contains_elem` 走二分查找而非逐段线性扫描。**架构推论：**段数增大时查询代价按对数增长，前提是规范化保证段有序；若允许无序多重范围，二分前提不成立，只能线性扫描。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/rangetypes.c` | `range_in` | 解析边界文本、调用子类型输入并规范化为 RangeType。 |
| `src/backend/utils/adt/rangetypes.c` | `range_out` | 反序列化边界并调用子类型输出拼接外部文本。 |
| `src/backend/utils/adt/rangetypes.c` | `range_contains` | 取 typcache 后执行区间包含的边界比较。 |
| `src/backend/utils/adt/multirangetypes.c` | `make_multirange` | 排序合并输入区间并序列化带 flags/偏移的多重范围。 |
| `src/backend/utils/adt/multirangetypes.c` | `multirange_contains_elem` | 在有序段上二分判断元素是否落在任一段内。 |

## 参考

- [PostgreSQL 文档：范围类型](https://www.postgresql.org/docs/current/rangetypes.html) —— 说明范围/多重范围的边界包含性与空区间语义。
- [PostgreSQL 官方源码：多重范围实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/multirangetypes.c) —— 对照规范化合并与二分查找。

## 与主核 / AI 的关系

暂无直接对应。主核若引入区间类型，需要同时定义规范化规则与子类型比较契约，否则包含与索引谓词无法保持一致性。
