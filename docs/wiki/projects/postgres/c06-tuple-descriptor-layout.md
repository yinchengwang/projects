---
project: postgres
feature: tuple-descriptor-layout
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/heaptuple.c
    symbols: [heap_compute_data_size, heap_fill_tuple, heap_deform_tuple]
  - path: src/backend/access/common/tupdesc.c
    symbols: [CreateTupleDescCopy, FreeTupleDesc, equalTupleDescs]
---

# Tuple descriptor 与 tuple 编码

## 一句话职责

TupleDesc 把列的类型、长度、对齐与约束元数据组织成可复用描述符，heap tuple 编解码器据此把 Datum 数组转成带 null bitmap 的紧凑数据区并还原。

## 机制是什么

`TupleDescData` 保存 `natts`、复合类型标识、typmod、引用计数、版本、约束和属性数组；每列的 `FormData_pg_attribute` 含 `atttypid`、`attlen`、`attbyval`、`attalign`、storage 等信息。`CompactAttribute` 是面向热路径的紧凑副本，保存长度、by-value、对齐字节数、是否可 pack、missing/drop 标志及空值约束状态；修改完整属性后必须重新填充 compact 版本，并在使用前 `TupleDescFinalize`。

数据区按属性顺序排列，固定长度值按 attalign 对齐；varlena 可能使用短头形式而省略常规对齐，展开值在形成 tuple 时会被 flatten。null bitmap 仅在 tuple 含 NULL 时出现，每个属性对应一位；tuple header 的 `t_hoff` 指向对齐后的用户数据起点，`t_infomask` 标出 null、变长或外部值等实际布局特征。此处不讲 page line pointer、页内空闲空间偏移或元组在页中的安放管理。

## 怎么实现的

1. 描述符创建或复制时，以 `natts` 分配 `TupleDescData` 与 `FormData_pg_attribute` 数组；`CreateTupleDescCopy` 平拷属性后清除不属于该轻量副本的 defaults/constraints 等字段，逐列重建 `CompactAttribute` 并 finalize。带约束的深拷贝由另一接口处理。
2. `heap_compute_data_size` 按列遍历 Datum/isnull：NULL 不占数据区；packable varlena 可按短头大小估算且无需通常对齐；展开对象按 flatten 后尺寸计算；其余值根据 `attalignby`、`attlen` 计算填充与长度，并用溢出检查防止 tuple 超过允许分配大小。
3. tuple 构造者为 header、可能存在的 null bitmap 和数据区计算空间，并将数据区预先清零；`heap_fill_tuple` 顺序处理每个值，维护 null bitmap 的位掩码，写入 by-value 或复制 by-reference 内容，并设置 `HEAP_HASNULL`、`HEAP_HASVARWIDTH`、`HEAP_HASEXTERNAL` 等提示位。对 varlena 可按 storage 元数据转换为 short header，expanded value 则复制 flatten 内容。
4. `TupleDesc` 的缓存偏移 `attcacheoff` 只对可确定的前缀属性使用；NULL、missing 与变长值可能使后续偏移不能固定。`TupleDescFinalize` 计算首个不可缓存 offset/首个非保证属性，形成/读取期间可使用 `CompactAttribute` 的预计算字段避开重复访问宽属性结构。
5. `heap_deform_tuple` 先比较 tuple 实际属性数与 descriptor 数，避免写出调用方数组；优先直接读取缓存偏移前缀，再按对齐及实际长度推进变长区间，遇 null bitmap 位时返回 null，descriptor 比 tuple 多出的尾列按 missing value 或 NULL 补足。
6. `FreeTupleDesc` 要求非正引用计数并释放约束、默认值表达式、missing 值等子结构；引用计数型描述符经 `Incr/DecrTupleDescRefCount` 与 `ResourceOwner` 配对，引用归零后才释放。`equalTupleDescs` 除列数/类型/version 外逐列比较布局与属性语义，版本变化也会使描述符不相等。

## 关键取舍

- 源码事实：TupleDesc 同时保存完整 `FormData_pg_attribute` 与小型 `CompactAttribute`，源码要求改变完整属性后刷新紧凑副本。架构推论：以重复元数据换取 tuple 形成/解构热路径少读、更紧凑的工作集；只保留完整属性可减少副本维护，但每列处理需做更多地址计算并触及更大的结构。
- 源码事实：`attcacheoff` 仅在确定属性前缀有效；遇到 NULL、变长、missing 等因素会从较早位置扫描并重算。架构推论：选择对稳定前缀快速解码，同时不强行维护每种 tuple 的完整偏移索引；若为所有 tuple 保存 offset 表，空间成本会随属性数增加，且 tuple 内容变化仍需对应维护。
- 源码事实：null bitmap 只在 tuple 有 NULL 时设置，属性数据按对齐规则紧密编码；NULL 不写入数据区。架构推论：通过条件性 bitmap 与省略 null 数据节省常见 tuple 空间，代价是读取时必须结合 header/null 位恢复属性位置；固定宽度槽位会让随机定位简单，却会浪费大量 NULL 与短 varlena 空间。
- 源码事实：`equalTupleDescs` 把 `tdtypmod`、`tdrefcount` 排除比较，但会检查 descriptor version、attlen/attbyval/attalign 等。架构推论：相等性针对可安全重用的行类型/layout，而不是内存对象完全相同；若把引用计数也纳入，就会让纯生命周期差异阻止描述符复用。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/heaptuple.c` | `heap_compute_data_size` | 按 NULL、packing、expanded value 与属性对齐规则计算 tuple 数据区长度并检查溢出。 |
| `src/backend/access/common/heaptuple.c` | `heap_fill_tuple` | 写入 null bitmap 与每个属性的物理表示，并更新 tuple 内容标志位。 |
| `src/backend/access/common/heaptuple.c` | `heap_deform_tuple` | 依据 descriptor 缓存偏移、bitmap 和属性长度把物理 tuple 还原为 Datum/null 数组。 |
| `src/backend/access/common/tupdesc.c` | `CreateTupleDescCopy` | 复制属性元数据但清理约束/默认值类字段，再重建 compact 属性与版本缓存。 |
| `src/backend/access/common/tupdesc.c` | `FreeTupleDesc` | 释放 descriptor 及其约束、默认值和 missing value 子对象。 |
| `src/backend/access/common/tupdesc.c` | `equalTupleDescs` | 判断两个 descriptor 的属性、物理布局及相关约束/version 是否兼容。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL 关系列描述符及这种 tuple data area 编码的直接对应物。可以参考用紧凑元数据支撑热路径、区分逻辑 schema 与物理编码的设计；不要将 PostgreSQL 的 varlena 对齐、null bitmap 或行类型版本规则套用于主核对象。

## 参考

- [PostgreSQL 文档：数据库物理存储布局](https://www.postgresql.org/docs/current/storage-page-layout.html) —— 说明 heap tuple header 与 tuple data 的物理组织。
- [PostgreSQL 官方源码：tuple descriptor 定义](https://github.com/postgres/postgres/blob/master/src/include/access/tupdesc.h) —— 支撑 TupleDescData、CompactAttribute 与引用生命周期字段。
