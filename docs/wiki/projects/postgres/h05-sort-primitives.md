---
project: postgres
feature: sort-primitives
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/sort/sortsupport.c
    symbols: [PrepareSortSupportFromOrderingOp, FinishSortSupportFunction, comparison_shim]
  - path: src/backend/utils/sort/tuplesort.c
    symbols: [tuplesort_heap_insert, tuplesort_heap_delete_top]
---

# 排序基元

## 一句话职责

排序基元提供排序所需的比较器装配与堆维护操作，使上层排序只需按三值比较结果驱动数组排序或归并。

## 机制是什么

`sortsupport.c` 把 btree opclass 的比较能力装配成 `SortSupport`：优先调用 opfamily 提供的 sort support 函数；若其没有安装比较器，则用 `comparison_shim` 把传统 btree comparator（返回 int 的三值函数）包装成统一的 `comparator(Datum, Datum, SortSupport)`。`SortSupport` 还携带 `ssup_reverse` 与 `ssup_nulls_first`，由内联的 `ApplySortComparator` 统一处理 NULL 次序与降序反转。

`tuplesort.c` 的堆操作是这一契约的消费者：`tuplesort_heap_insert` 通过 `COMPARETUP` 上滤插入，`tuplesort_heap_delete_top` 删除堆顶并下滤，二者共同维护“堆顶始终是当前最小项”的不变式，供限界排序与多路归并使用。它们不负责定义类型序、不处理 NULL 语义，也不实现具体排序算法的主体（数组排序走 `qsort_*`／radix，归并走堆加磁带）。

## 怎么实现的

1. `PrepareSortSupportFromOrderingOp` 用 `get_ordering_op_properties` 从排序操作符取 opfamily、opcintype 与比较类型；`COMPARE_GT` 时置 `ssup_reverse`，随后交给 `FinishSortSupportFunction`。
2. `FinishSortSupportFunction` 查 `BTSORTSUPPORT_PROC`；存在则调用该函数，由 opclass 自行安装 `comparator` 并可选用缩写键能力。若调用后 `comparator` 仍为空，则查 `BTORDER_PROC` 得到传统比较函数并走 shim。
3. `PrepareSortSupportComparisonShim` 在 `ssup_cxt` 上分配 `SortShimExtra`，用 `fmgr_info_cxt` 缓存函数查找信息，一次性初始化可复用 `fcinfo`（2 参数、指定 collation），把 `ssup_extra` 指向它并令 `comparator = comparison_shim`，避免每次比较重复建调用信息。
4. `comparison_shim` 每次比较只写入两个 Datum 参数、重置 `isnull`，随后 `FunctionCallInvoke` 调用旧式比较函数；若返回 NULL 则报错，否则把结果转成 int 返回。因此旧比较函数缺少 NULL 处理不影响使用——NULL 次序已在调用它之前由 `ApplySortComparator` 决定。
5. `ApplySortComparator` 按 `NULLS FIRST/LAST` 直接给出两个 NULL 组合的结果；两侧非 NULL 时才调用 `comparator`，并在非 DESC 列上反转符号，使上层得到统一的“负/零/正”语义。
6. `tuplesort_heap_insert` 在数组尾部放置新元组，沿父链上滤：只要 `COMPARETUP` 表明新项小于父项就下移父项，符合 Knuth 的上滤过程；函数断言数组还有空位，要求调用者事先确保容量。
7. `tuplesort_heap_delete_top` 递减元素数，若堆已空直接返回；否则把最后一个元组 `tuplesort_heap_replace_top` 到堆顶并下滤，选出较小的子节点上升，从而在 O(log n) 内恢复堆序。限界排序收尾时反复调用它把最大项逐个移到数组尾部，最终得到正向有序数组。

## 关键取舍

- **源码事实：**只有 opfamily 未安装 sort support comparator 时才退回到 shim 包装传统比较函数；shim 复用同一个 `fcinfo` 而不是每次构造。**架构推论：**以一层间接调用换取所有 opclass 都能参与排序，复用调用信息可降低每次比较的固定开销；若要求所有类型都必须提供原生 sortsupport，就得改造大量既有 opclass。
- **源码事实：**`ApplySortComparator` 在调用 comparator 前处理 NULL 与 reverse，注释明确指出 reverse 由它负责。**架构推论：**把方向与 NULL 次序集中到一处，避免每个比较函数重复实现；若下推到各 opclass，任何遗漏都会造成排序方向错误且难以统一排查。
- **源码事实：**堆操作以索引运算实现上滤/下滤，注释标注对应 Knuth 5.2.3 过程；限界排序依靠反复 delete-top 把堆转成有序数组。**架构推论：**堆路径只保留 M 个候选元组即可维持前 K 结果，内存随 K 而非输入规模增长；改用全量排序则内存与输入规模成正比。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/sort/sortsupport.c` | `PrepareSortSupportFromOrderingOp` | 由排序操作符解析 opfamily 并设置 reverse 与比较器。 |
| `src/backend/utils/sort/sortsupport.c` | `FinishSortSupportFunction` | 优先使用 opclass sortsupport，缺失时退回传统比较函数。 |
| `src/backend/utils/sort/sortsupport.c` | `comparison_shim` | 复用 fcinfo 调用旧式比较函数并转换为 int 结果。 |
| `src/backend/utils/sort/tuplesort.c` | `tuplesort_heap_insert` | 上滤插入新元组并维持堆顶最小不变式。 |
| `src/backend/utils/sort/tuplesort.c` | `tuplesort_heap_delete_top` | 移除堆顶并下滤恢复堆序，支撑限界排序与归并。 |

## 参考

- [PostgreSQL 文档：操作符类与排序支持](https://www.postgresql.org/docs/current/indexes-opclass.html) —— 说明 opclass 中排序相关支持函数的角色。
- [PostgreSQL 官方源码：排序支持装配](https://github.com/postgres/postgres/blob/master/src/backend/utils/sort/sortsupport.c) —— 对照 sortsupport 查找与 shim 回退路径。

## 与主核 / AI 的关系

暂无直接对应。可借鉴“统一三值比较契约 + 方向/NULL 集中处理”的做法；主核实现时需自行定义类型序与比较器回退策略。
