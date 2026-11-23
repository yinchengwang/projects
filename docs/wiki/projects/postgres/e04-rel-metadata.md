---
project: postgres
feature: rel-metadata
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/util/plancat.c
    symbols: [get_relation_info, get_relation_statistics]
  - path: src/backend/optimizer/util/inherit.c
    symbols: [expand_inherited_rtentry]
  - path: src/backend/optimizer/util/relnode.c
    symbols: [build_simple_rel]
---

# 关系建模与继承展开

## 一句话职责

关系建模把查询引用的 RTE 转成带目录元数据、统计估计和继承成员关系的优化器关系对象。

## 机制是什么

`RelOptInfo` 是规划期的关系模型：基础关系具有属性范围、行列估计、索引和统计对象等信息，子关系则作为 appendrel 成员关联父关系。`build_simple_rel` 依据 RTE 类型初始化模型，真实关系再由 `get_relation_info` 装载表、索引、统计、外键及分区信息；继承/分区父项由 `expand_inherited_rtentry` 添加子 RTE、`AppendRelInfo` 和对应关系模型。

此机制提供估算与结构，不生成具体访问路径，也不负责运行期分区裁剪。分区父项与传统继承成员在规划表示上不同：分区父表本身可不扫描，传统继承会把父表作为成员之一；路径生成阶段再为存活成员分别估算、生成候选。

## 怎么实现的

1. `query_planner` 根据 jointree 找到实际参与查询的 RTE，并由 `build_simple_rel` 在 `PlannerInfo.simple_rel_array` 建立 `RelOptInfo`；视图等未进入 jointree 的 RTE 不会仅因在 rangetable 中就建基础关系。
2. 普通关系调用 `get_relation_info`，用已有关系锁打开 relcache，记录属性边界、表空间、NOT NULL 属性和大小估计；读取有效索引描述为 `IndexOptInfo`，统计对象元数据放入 `statlist`，同时收集外键与 FDW/分区所需字段。
3. 继承关系由 `expand_inherited_rtentry` 扫描成员并按父关系锁模式取得子关系锁；为子项扩充 PlannerInfo 数组，创建 child RTE 和 `AppendRelInfo`，随后调用 `build_simple_rel` 建子模型。
4. 分区表通过分区目录先按父关系限制裁剪存活分区，再递归展开存活分区并记录 `live_parts`、`part_rels`；分区已被并发 detach/drop 时 `try_table_open` 失败则将其从存活集合移除。
5. 子项在 `build_simple_rel` 中继承父关系的 nulling/lateral 信息，经 `apply_child_basequals` 翻译父限制条件；翻译后若化简为 false/NULL，立即标记 dummy，供后续裁剪跳过。模型及目录数据留在本次规划生命周期内，不需跨后端锁保护。

## 关键取舍

- **源码事实：**传统继承父项的物理大小留给 appendrel 汇总；分区父项递归加入存活子项并以父关系承载整体。**架构推论：**避免父子页数重复计入成本，同时保留成员层级；把每个子项都当独立 SQL 引用会破坏继承语义和统一输出列映射。
- **源码事实：**对子项翻译后的限制若为常量 false/NULL，会立即标为 dummy；遇到已裁剪掉或无法打开的分区也不继续建模。**架构推论：**尽早排除空成员可减少后续统计与候选路径工作；若延迟到最终路径选择才判断，所有上游关系组合会承受无效搜索成本。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/util/plancat.c` | `get_relation_info` | 把关系目录、索引、统计与存储估计装入 `RelOptInfo` |
| `src/backend/optimizer/util/plancat.c` | `get_relation_statistics` | 建立可用扩展统计对象的规划期描述 |
| `src/backend/optimizer/util/inherit.c` | `expand_inherited_rtentry` | 扩展继承/分区 RTE 并建立子关系及映射 |
| `src/backend/optimizer/util/relnode.c` | `build_simple_rel` | 初始化基础/append 成员 `RelOptInfo` 并装载关系信息 |

## 参考

- [PostgreSQL 文档：继承](https://www.postgresql.org/docs/current/ddl-inherit.html) —— 传统继承的查询语义
- [PostgreSQL 源码：inherit.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/util/inherit.c) —— 子关系与分区展开

## 与主核 / AI 的关系

`RelOptInfo` 与继承展开可作为主核关系目录建模的概念对照；数据结构与裁剪阶段须按主核实现确认。暂无直接 AI 对应。
