---
project: postgres
feature: node-tree-utilities
page_type: feature
schema_version: 1
refs:
  - path: src/backend/nodes/nodeFuncs.c
    symbols: [exprType, exprTypmod, exprCollation]
  - path: src/backend/nodes/equalfuncs.c
    symbols: [equal]
  - path: src/backend/nodes/copyfuncs.c
    symbols: [copyObjectImpl]
---

# 节点树工具

## 一句话职责

节点树工具提供对表达式与查询树按节点类型分派查询、比较、复制和遍历的通用基础设施。

## 机制是什么

PostgreSQL 的表达式与查询是多态节点树：同一个“表达式”可能是 `Var`、`Const`、`OpExpr`、`FuncExpr` 或 `SubLink`。`exprType`/`exprTypmod`/`exprCollation` 用 `switch (nodeTag(expr))` 把三种类型属性从对应节点的字段中提取出来，调用者无需知道具体节点种类。`equal` 判断两棵树结构等价，`copyObjectImpl` 产生深拷贝。此外 `nodeFuncs.c` 还承载 `expression_tree_walker`/`query_tree_walker` 与 `expression_tree_mutator` 家族，让上层只写回调就能遍历整棵树。

边界：这些工具不定义语义，不校验类型合法性，也不做优化。它们依赖节点种类齐全的 switch 分支，遇到未处理的 tag 直接 `elog(ERROR)`；不保证未解析即 raw parse tree 的语义，例如对标量 `SubLink` 取类型时会检查子查询是否已变换。

## 怎么实现的

1. `exprType` 先处理 NULL 返回 `InvalidOid`，再按 `nodeTag` 分支：`Var` 取 `vartype`、`Const` 取 `consttype`、`Param` 取 `paramtype`；`GroupingFunc` 与 `BoolExpr`、`ScalarArrayOpExpr` 直接返回 `BOOLOID`/`INT4OID` 这类固定结果类型。
2. 对标量或数组 `SubLink`，`exprType` 取子查询首目标列的表达式类型，数组形式再经 `get_promoted_array_type` 提升为数组类型；若子查询尚未变换则 `elog(ERROR)`，防止从 raw 树误取类型。
3. `exprTypmod` 对 `NullIfExpr` 取第一参数 typmod、对 `CollateExpr` 透传到参数；对 `FuncExpr` 先用 `exprIsLengthCoercion` 判断是否为长度强制函数，是则用其第二个 `int4` 常量参数作为结果 typmod，否则返回 -1 表示不可知。`CaseExpr` 只有在 default 与各分支类型、typmod 全部一致时才返回该 typmod。
4. `exprCollation` 平行地从各节点取结果 collation；注释特别指出 `inputcollid` 是函数输入的统一 collation，可能不同于结果 collation，例如可排序与不可排序类型互换时两者不同。布尔结果节点（`BoolExpr`、`ScalarArrayOpExpr`）返回 `InvalidOid`。
5. `equal` 先做指针相等、NULL、`nodeTag` 相等三项快速判定，再经 `check_stack_depth` 后按 tag 分派到生成的 `equalfuncs.switch.c`；`List` 分支单独实现，先比 `type`/`length`，再逐元素递归。`Const` 比较走 `_equalConst`：先比类型/typmod/collation 等标量字段，若 `constisnull` 则所有同类型的 NULL 常量视作相等，否则用 `datumIsEqual`。
6. `copyObjectImpl` 对 NULL 返回 NULL，`check_stack_depth` 后按 tag 分派到 `copyfuncs.switch.c`；`List` 用 `list_copy_deep` 深拷贝，`IntList`/`OidList`/`XidList` 只需 `list_copy` 浅拷贝，因为元素本身是值而非指针。
7. 比较与复制都刻意不复制也不比较 `location` 与 `CoercionForm`，源码中把对应宏定义为空操作，使仅因写法位置不同的等价表达式被视为相等。遍历/替换则通过 `expression_tree_walker_impl` 与 `expression_tree_mutator_impl` 按 tag 递归子节点，回调返回 true 即可提前中止。

## 关键取舍

- **源码事实：**`location` 与 `CoercionForm` 在 `equal` 中被显式忽略，`Const` 的所有 NULL 值按类型视为相等。**架构推论：**这让“结构等价”与源码位置解耦，便于把不同来源的等价表达式匹配起来；若比较位置，同一子句的不同书写位置将被判为不等，影响常量表达式缓存与去重。
- **源码事实：**`copyObjectImpl` 对 `IntList`/`OidList`/`XidList` 只做浅拷贝，其余 `List` 用 `list_copy_deep`。**架构推论：**按元素是否为指针区分拷贝深度，避免为标量元素做无意义递归；若一律深拷贝，开销与内存都会上升，若一律浅拷贝则共享节点树会被后续原地改写污染。
- **源码事实：**所有属性提取都依赖穷举 tag 的 switch，遇到未知 tag 直接报错。**架构推论：**放弃对第三方自定义节点的透明支持（`ExtensibleNode` 另有专门的比较/复制钩子），换取编译期可见的完整覆盖；若改为反射式字段查找，通用性提高但每节点访问都会付出查找成本。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/nodes/nodeFuncs.c` | `exprType` | 按节点种类返回结果类型 OID，含 SubLink 首列提升 |
| `src/backend/nodes/nodeFuncs.c` | `exprTypmod` | 返回可确定的 typmod，含长度强制函数识别 |
| `src/backend/nodes/nodeFuncs.c` | `exprCollation` | 返回结果 collation，并区分输入 collation |
| `src/backend/nodes/equalfuncs.c` | `equal` | 结构等价比较入口，忽略位置与强制形式字段 |
| `src/backend/nodes/copyfuncs.c` | `copyObjectImpl` | 深拷贝节点树/列表，对标量列表走浅拷贝 |

## 参考

- [PostgreSQL 文档：查询树](https://www.postgresql.org/docs/current/querytree.html) —— 查询树与节点表示在实现中的位置
- [PostgreSQL 源码：nodeFuncs.c](https://github.com/postgres/postgres/blob/master/src/backend/nodes/nodeFuncs.c) —— 表达式属性提取与遍历实现
- [PostgreSQL 源码头文件：nodes.h](https://github.com/postgres/postgres/blob/master/src/include/nodes/nodes.h) —— 节点标签与 `copyObject`/`equal` 的对外声明

## 与主核 / AI 的关系

“多态节点 + 按 tag 分派”的组织方式可作主核 AST/表达式表示的对照；walkers/mutators 的回调约定也值得借鉴。暂无直接 AI 对应。
