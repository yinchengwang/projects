---
project: postgres
feature: index-tuple-representation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/indextuple.c
    symbols: [index_form_tuple, index_deform_tuple, nocache_index_getattr]
---

# IndexTuple 编码

## 一句话职责

IndexTuple 将索引键值压成页内可存储的紧凑记录，并提供按属性取回 Datum 的解码路径。

## 机制是什么

索引页保存的不是 heap tuple 本身，而是 IndexTuple：固定头部 `IndexTupleData` 携带指向 heap 行的 TID 与 `t_info`，其后按 tuple descriptor 描述的顺序排列属性数据。`t_info` 编码元组尺寸及 NULL、变长等标志；存在 NULL 时，位图位于固定头后、数据区前。

`index_form_tuple` 和解码函数处理标准 IndexTuple 的构造与属性访问；它不负责 B-tree pivot/posting tuple 的专属语义，也不负责 heap tuple 的 MVCC header。属性对齐、NULL bitmap 与 varlena 布局遵循 tuple descriptor，不能按固定偏移假定所有列可直接访问。

## 怎么实现的

1. `index_form_tuple` 转到指定 memory context 的构造实现，先检查属性数上限，扫描 `isnull` 决定是否设置 `INDEX_NULL_MASK`，再用 `IndexInfoFindDataOffset` 算数据区起点。
2. `heap_compute_data_size` 按 descriptor、值和 NULL 数组计算数据长度；总尺寸加上头部后按 MAXALIGN 对齐并分配零初始化空间。
3. `heap_fill_tuple` 顺序写入属性值与可选 NULL bitmap。其产生的变长标记被映射为 IndexTuple 的 `INDEX_VAR_MASK`；TOAST_INDEX_HACK 构建下还处理外置属性，避免索引记录依赖外部存储引用。
4. 构造器检查尺寸能否装入 `t_info` 的 size 位域，随后写入 `t_info` 并返回当前上下文拥有的元组内存。超属性数或尺寸溢出会报错。
5. `index_deform_tuple` 根据 `t_info` 取得数据区和 NULL bitmap，按 descriptor 解出全部属性至 `Datum[]` / `isnull[]`。
6. 单属性访问通常经 `index_getattr` 宏复用 `attcacheoff`；偏移不可缓存且目标非 NULL 时进入 `nocache_index_getattr`，从已知缓存起点或首个 NULL 前位置逐属性计算对齐偏移。

## 关键取舍

- **源码事实：**可缓存属性偏移时直接访问；变长或 NULL 影响后续布局时才逐属性走 `nocache_index_getattr`。**架构推论：**以 descriptor 缓存加慢路径减少常见固定布局属性的重复计算；若所有访问都重新扫描前序列会更简单，但读取后部属性成本会随列数上升。
- **源码事实：**IndexTuple 头与可选 NULL bitmap 紧凑相邻，数据区按 tuple descriptor 对齐，构造器仅对最终尺寸做最大对齐。**架构推论：**这种按需位图与紧凑布局节省索引页空间，但要求访问者尊重 `t_info` 和 descriptor；固定列偏移布局更容易读取，却无法自然兼容 NULL 和变长属性。
- **源码事实：**TOAST_INDEX_HACK 下外部 datum 会去外部化，且可尝试压缩超目标的可压缩值。**架构推论：**索引元组不依赖 heap 外部 TOAST 指针的生命周期，代价是索引键可能需要物化/压缩数据并占更多页空间。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/indextuple.c` | `index_form_tuple` | 依据 descriptor、值和 NULL 数组创建标准索引元组。 |
| `src/backend/access/common/indextuple.c` | `index_deform_tuple` | 将完整 IndexTuple 按属性布局拆成 Datum 与 NULL 数组。 |
| `src/backend/access/common/indextuple.c` | `nocache_index_getattr` | 在偏移无法缓存时计算指定属性的真实数据地址。 |

## 参考

- [PostgreSQL 文档：索引类型](https://www.postgresql.org/docs/current/indexes-types.html) —— 介绍索引存储键并由访问方法执行搜索的概念边界。
- [PostgreSQL 官方源码：IndexTuple 编码](https://github.com/postgres/postgres/blob/master/src/backend/access/common/indextuple.c) —— 对照构造、对齐和属性解码实现。

## 与主核 / AI 的关系

暂无直接对应。索引编码只提供页内记录表示；不要与主核 heap tuple 的事务头或 MVCC 可见性结构混为一谈。
