---
project: postgres
feature: path-generation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/path/allpaths.c
    symbols: [set_rel_pathlist, set_base_rel_pathlists]
  - path: src/backend/optimizer/path/indxpath.c
    symbols: [create_index_paths]
  - path: src/backend/optimizer/path/tidpath.c
    symbols: [create_tidscan_paths]
---

# 访问路径生成

## 一句话职责

访问路径生成针对每个关系枚举可行的扫描实现，并把有价值的候选登记给成本比较阶段。

## 机制是什么

关系模型建立和大小估计完成后，路径生成根据 RTE 类型派发到普通表、外表、采样表、appendrel、子查询、函数和值等专门逻辑。普通表常考虑顺序扫描、并行顺序扫描、索引扫描与 TID 扫描；候选进入 `RelOptInfo.pathlist` 或并行 `partial_pathlist`，由 `set_cheapest` 标记成本领先路径。

本机制负责生成候选，不负责选定最终全查询计划（最终关系及上层排序/聚合另行比较），也不负责执行扫描。候选受谓词、安全性、索引能力、LATERAL 参数化和并行安全约束；没有可用 TID 路径时，普通顺序/索引路径仍可参加竞争，除 `CURRENT OF` 等执行器要求特例。

## 怎么实现的

1. `make_one_rel` 先完成 base rel 估算和并行标记，再由 `set_base_rel_pathlists` 遍历基础关系调用 `set_rel_pathlist`；appendrel 成员路径由父 appendrel 统一组合。
2. `set_rel_pathlist` 按 RTE 和继承标记选择专用函数，完成后运行 `set_rel_pathlist_hook`，合并合适的 partial path，调用 `set_cheapest`。
3. 普通表路径入口 `set_plain_rel_pathlist` 先调用 `create_tidscan_paths`；若发现 `CURRENT OF`，该 TID 路径是唯一允许路径并提前返回。否则再加入顺序扫描、符合条件的并行部分扫描及索引路径。
4. `create_index_paths` 遍历关系的 `IndexOptInfo`，先略过未证明可用的部分索引，再分别匹配限制条件、连接条件和等价类条件；构造普通/参数化 `IndexPath`，位图候选先收集，再择取 BitmapAnd/BitmapOr 组合为 BitmapHeapPath。
5. `create_tidscan_paths` 从条件识别 `ctid = 常量`、可用 OR/数组形式、范围条件和 `CURRENT OF`；连接等值还可能形成依赖外关系的参数化路径。`tidpath.c` 的范围扫描仅在 table AM 支持 TID 范围能力时加入。
6. 候选的路径键、required_outer、估计成本和并行属性附着在 Path；`add_path`/`add_partial_path` 依据支配关系过滤，随后上层连接搜索可组合存活候选。

## 关键取舍

- **源码事实：**索引位图路径会延迟组合，限制条件位图组合为每个关系挑选有前景的组合；连接位图按不同参数化集合产生候选。**架构推论：**这减少索引数增加时组合爆炸；若穷举每个索引子集，可能更全面但规划成本迅速增长。
- **源码事实：**普通访问路径可被 GUC 掩码控制，但 `CURRENT OF` 即使禁用 TID scan 仍必须使用 TID 路径，且因此阻止添加其他路径；其它不支持/不匹配的条件则不创建对应候选。**架构推论：**执行器能力约束优先于用户对一般算法候选的偏好；如果只按开关忽略特殊路径，会生成无法正确执行的计划。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/path/allpaths.c` | `set_rel_pathlist` | 按关系类型派发路径生成并运行扩展 hook |
| `src/backend/optimizer/path/allpaths.c` | `set_base_rel_pathlists` | 对基础关系逐一启动路径生成 |
| `src/backend/optimizer/path/indxpath.c` | `create_index_paths` | 匹配谓词与索引能力并建立索引/位图路径 |
| `src/backend/optimizer/path/tidpath.c` | `create_tidscan_paths` | 从 ctid/当前游标条件生成直接 TID 或范围路径 |

## 参考

- [PostgreSQL 文档：索引扫描与规划](https://www.postgresql.org/docs/current/indexes.html) —— 索引访问路径的用户可见基础
- [PostgreSQL 源码：indxpath.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/indxpath.c) —— 索引候选构造与筛选

## 与主核 / AI 的关系

可把扫描路径看作主核查询引擎选择物理访问方法的概念参照；并行与索引能力需按主核代码核对。暂无直接 AI 对应。
