---
project: postgres
feature: equivalence-and-order
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/path/equivclass.c
    symbols: [process_equivalence, generate_base_implied_equalities]
  - path: src/backend/optimizer/path/pathkeys.c
    symbols: [make_canonical_pathkey, truncate_useless_pathkeys]
---

# 等价类与排序传播

## 一句话职责

等价类把可证明相等的表达式归一为共享成员集合，再将这种关系传播为可用于连接、索引顺序与排序消除的 PathKey 属性。

## 机制是什么

`EquivalenceClass`（EC）由操作符族、collation、成员表达式及来源条件等组成；mergejoinable 等式只有在表达式/数据类型匹配、操作符族一致且安全级别允许立即执行时才可并入。完成合并后，规划器生成基础约束和跨关系连接的 implied equalities，不必在每个 join 顺序上重复使用原始相等条件。常量成员会产生 `member = const` 限制，普通多关系成员在关系可用时产生连接等式。

`PathKey` 用 EC、排序 opfamily、比较方向和 NULL 顺序描述路径输出顺序；同一 canonical EC 可表明两个排序项在相等意义上冗余，常量受限的 EC 也可能令排序键无区分力。此页负责等式归一、pathkey 传递及冗余顺序消除，不负责候选路径支配/最终 cheapest 选择（e08）或谓词一般逻辑证明（e11）。EC 合并完成后才可创建 canonical PathKey，避免已有路径键指向非 canonical 成员。

## 怎么实现的

1. `process_equivalence` 仅检查适合 merge 的等式；若条件带非零 security level 且表达式非 leakproof，则拒绝吸收，避免将安全策略允许延迟的谓词提前执行。表达式先按所需类型/collation canonicalize，再查找匹配 opfamily 与 collation 的 EC。
2. 两端已在同一 EC 时记录 source；分别位于不同 EC 时，在允许合并阶段将成员、来源、派生子句和 relids 并到目标 EC，将被并 EC 通过 `ec_merged` 指向目标而不立即释放，以免悬空 PathKey；只有找到一侧时新增另一成员，均未命中则新建双成员 EC。
3. `generate_base_implied_equalities` 标记 `ec_merging_done`，然后按是否含常量生成基础限制等式；无常量 EC 对同一基础关系的成员生成足够连接它们的链式等式，并把相关 EC 索引登记到 base rel。缺少跨类型 equality operator 时标记 `ec_broken`，后续回退到原始 source RestrictInfo。
4. `make_canonical_pathkey` 要求 EC 合并完成，追随 `ec_merged` 到 canonical EC，再以 EC/opfamily/cmp type/NULL 顺序查找或创建共享对象。PathKey 列表可用指针身份快速比较。
5. `truncate_useless_pathkeys` 检查查询 ORDER BY、窗口、setop、GROUP BY、DISTINCT 和 merge join 哪些需要前缀或成员顺序；只保留被这些需求使用的键。常量 EC 或重复 EC 项由 `pathkey_is_redundant` 判为不增加排序区分度。

## 关键取舍

- **源码事实：**EC 用普通成员列表合并，注释指出若性能成为瓶颈可考虑更优 UNION-FIND；当前简单列表减少结构复杂性，但查找/合并可能随成员数量线性增长。若换复杂并查集，需额外维护 canonical 指针和 PathKey/关系索引一致性。
- **源码事实：**等价类吸收后生成最少化的基础等式，而不是每个成员两两连接；无常量情形按同一 base rel 成员链式连等。跨类型 operator 不足会破坏推导不变量，标记 `ec_broken` 并回退原始条件，而不是丢弃语义约束。
- **源码事实：**`truncate_useless_pathkeys` 对 ORDER BY 等要求顺序前缀，对 GROUP BY/DISTINCT 则可利用不要求相同位置排列的已排序键；保留无用 pathkey 会增加比较/排序工作，过度截断则会丢失上层免 Sort 或 merge join 的机会。
- **源码事实：**EC 常量只在同一 JoinDomain 中匹配，且带安全延迟的非 leakproof 条件不会形成 EC；放宽作用域可能跨外连接或安全边界错误传播等式，代价远大于少一次优化机会。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/path/equivclass.c` | `process_equivalence` | 检查等式可吸收条件并创建/扩充/合并等价类 |
| `src/backend/optimizer/path/equivclass.c` | `generate_base_implied_equalities` | 固定 EC 合并状态并生成基础限制等式、登记关系索引 |
| `src/backend/optimizer/path/pathkeys.c` | `make_canonical_pathkey` | 将排序需求映射到唯一 canonical EC/opfamily/方向/null 顺序键 |
| `src/backend/optimizer/path/pathkeys.c` | `truncate_useless_pathkeys` | 按上层排序、分组、去重、merge join 需求截去无用键 |

## 与主核 / AI 的关系

暂无直接对应。等价类与排序属性可类比查询优化中的表达式规范化和物理属性传播；不能据此推断主核使用同类 opfamily 或 pathkey 表示。

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 提供优化器及物理路径选择的整体背景。
- [PostgreSQL 官方源码：equivclass.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/equivclass.c) —— 支撑等价类成员归一、派生等式及失效回退。
- [PostgreSQL 官方源码：pathkeys.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/pathkeys.c) —— 支撑 canonical pathkey 和有用排序前缀处理。
