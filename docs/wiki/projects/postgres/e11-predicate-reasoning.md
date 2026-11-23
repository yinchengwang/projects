---
project: postgres
feature: predicate-reasoning
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/util/predtest.c
    symbols: [predicate_implied_by, predicate_refuted_by]
  - path: src/backend/optimizer/plan/analyzejoins.c
    symbols: [remove_useless_outer_joins]
  - path: src/backend/optimizer/prep/prepqual.c
    symbols: [canonicalize_qual]
---

# 谓词推理与连接简化

## 一句话职责

谓词推理尝试证明条件蕴含、矛盾或可冗余，从而判断部分索引适用性、过滤不可达分支并安全简化某些外连接。

## 机制是什么

`predtest.c` 对 AND/OR 表达式递归执行蕴含与反证检查，可选择 strong/weak 语义以分别处理 CHECK 约束和查询条件；原子谓词通过简单表达式规则及可证明的操作符序推理。它不是完整一阶逻辑求解器：遇到不支持或无法证明的形式就返回未证明，调用者必须保守保留原条件。谓词中的非 immutable 表达式不能作为稳定索引谓词证明依据。

不同 caller 将证明结果用于不同安全目标：索引路径检查 query quals 是否蕴含部分索引 predicate；`analyzejoins.c` 通过内连接唯一性证明尝试移除不增加行数的 LEFT JOIN；`canonicalize_qual` 则在顶层 WHERE/JOIN ON/CHECK 的布尔结构中提取重复子句。此页负责逻辑推理与其连接简化调用，不负责等式归一/EC/pathkeys（e10），也不负责部分索引候选生成或最终 Plan 构造。

## 怎么实现的

1. `canonicalize_qual` 假定常量折叠已完成且 AND/OR 已扁平化，只在顶层 AND/OR 结构寻找 OR-of-AND 重复项，按 WHERE 与 CHECK 对 NULL 的不同真值语义简化常量。它不强制转成理论上的 CNF/DNF，以免逻辑展开制造大量冗余子句。
2. `predicate_implied_by`/`predicate_refuted_by` 将顶层 List 视作 AND，对表达式分类为 AND、OR 或 atom，递归按组合逻辑规则交错分解两边；strong/weak implication/refutation 区分“不为真”与“必须为假”等三值逻辑要求。
3. 原子蕴含先处理相同表达式、布尔常量关系和严格函数推出 `IS NOT NULL` 等规则，再查 operator family 的证明关系；反证有单独实现，处理 `IS NULL`/`IS NOT NULL`、strictness 与反向操作符，不能简单地把“不蕴含”当成“已反证”。
4. 部分索引 caller 仅在 quals 不含 mutable function 时调用蕴含证明；index scan plan 构造还可证明额外 scan clause 已由 stripped index quals 覆盖后从 qpqual 移除。若 proof 无把握则保留 clause 作执行期 recheck。
5. `remove_useless_outer_joins` 遍历 SpecialJoinInfo，要求目标是对单一 base relation 的 LEFT JOIN、内侧不被上层需要，且 join quals 能证明内侧对外侧唯一；成立才从 jointree 移除该 join 并清理其关系引用。证明不成立就保留原连接。

## 关键取舍

- **源码事实：**蕴含/反证只使用可证明的递归与操作符规则，不试图做任意公式求解；无法证明返回 false，caller 保留原谓词。若把“未证明”当成“成立”，可能漏掉部分索引必需过滤或错误删除行。
- **源码事实：**谓词 predicate 必须 immutable，避免规划时和执行时函数结果变化造成错误证明；查询 clause 的可变性也由相应 caller 约束。若允许 volatile 条件参与部分索引适用证明，执行时真假变化会使索引漏行。
- **源码事实：**`canonicalize_qual` 只提取局部重复项而不做一般 CNF/DNF 展开；覆盖通用布尔等价变换理论更完整，却可能造成指数级表达式膨胀和更多后续规划工作。
- **源码事实：**外连接移除只覆盖可证明内侧唯一、单关系右侧且属性/PlaceHolderVar 不需在连接上方的 LEFT JOIN；更广泛的外连接变换需要额外处理 NULL 扩展、变量需求和 jointree 范围，保守限制避免改变 SQL 语义。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/util/predtest.c` | `predicate_implied_by` | 递归证明条件集合是否推出目标谓词 |
| `src/backend/optimizer/util/predtest.c` | `predicate_refuted_by` | 独立证明条件是否使目标谓词必假/不成立 |
| `src/backend/optimizer/plan/analyzejoins.c` | `remove_useless_outer_joins` | 迭代删除可证明不改变输出行数的外连接 |
| `src/backend/optimizer/prep/prepqual.c` | `canonicalize_qual` | 简化顶层 WHERE/ON/CHECK 中可提取的重复 OR 子句 |

## 与主核 / AI 的关系

暂无直接对应。规则化谓词证明可作为优化器安全性设计的概念参照，但主核若做谓词化简须自行核对 NULL、volatile 与外连接语义。

## 参考

- [PostgreSQL 官方源码：predtest.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/util/predtest.c) —— 支撑蕴含/反证递归、strong/weak 规则及操作符证明。
- [PostgreSQL 官方源码：analyzejoins.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/analyzejoins.c) —— 支撑外连接唯一性检查与 jointree 移除条件。
- [PostgreSQL 官方源码：prepqual.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/prep/prepqual.c) —— 支撑顶层布尔条件的重复子句简化。
