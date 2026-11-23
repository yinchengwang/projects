---
project: postgres
feature: plan-reference-fixing
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/plan/setrefs.c
    symbols: [set_plan_references, fix_scan_expr]
  - path: src/backend/optimizer/plan/subselect.c
    symbols: [SS_finalize_plan]
---

# Plan 引用修整

## 一句话职责

引用修整把 planner 格式的 Var、Param 和子计划依赖改写为执行器能按 rangetable、子计划输出列及运行期参数槽解释的引用，并校验参数依赖闭包。

## 机制是什么

`create_plan` 之后，Plan 树仍可能使用原 Query 层级的 varno、scan 节点 base Var 和跨层表达式；子查询各自的 rangetable 也尚未合并。`set_plan_references` 作为最终结构处理 pass，将 rangetable 展平、调整扫描 Var 的索引，并把 join/upper 节点表达式中可由子计划输出提供的变量改为 `OUTER_VAR` / `INNER_VAR` 与相应 resno 引用。它还解析 PARAM_MULTIEXPR、AlternativeSubPlan、聚合替换及 operator function OID 等 executor 需求。

引用修整不重新选择 Path、不改成本或谓词语义（e08/e09/e11），也不负责 Path→Plan 节点创建（e12）。`SS_finalize_plan` 在 setrefs 前递归求 Plan 节点 `allParam`/`extParam` 集合，检查 PARAM_EXEC 的作用域和传递依赖；setrefs 则主要落实表达式引用表示、扁平 rangetable、依赖对象与 plan node ID。

## 怎么实现的

1. 顶层调用方先对普通子计划和 initplan 运行 `SS_finalize_plan`，再处理主 Plan，确保依赖子计划的 PARAM_EXEC 集合已计算；随后 `set_plan_references` 记录当前 flat rangetable 长度作为 rtoffset，并把本层 RTE、rowmark、appendrel 信息搬入 PlannerGlobal 的 executor 全局表。
2. `set_plan_refs` 递归遍历 Plan 节点，对 scan 节点调用 `fix_scan_expr`。普通 Var 的 `varno`/语法编号按 rtoffset 平移；参数节点由 `fix_param_node` 规范化；PlaceholderVar 在扫描层展开为其表达式，AlternativeSubPlan 根据估计执行次数选定一个实现。
3. 对 index-only、SubqueryScan、Append、MergeAppend、ForeignScan 等节点先做专用子计划引用处理；join 节点建立 child targetlist 的索引，`fix_join_expr` 将可匹配输出的表达式改为对应内部/外部 varno 和 resno。
4. upper 节点通过 `fix_upper_expr` 根据下层 indexed targetlist 匹配 Var、PlaceHolderVar 或可直接复用的非 Var 表达式；无法在子计划输出找到所需 Var 会报错，避免执行期引用到不存在的列。输出表达式不能复用下层项时则保留并递归修正其内部引用。
5. `SS_finalize_plan` 遍历目标列表、qual、节点特有表达式及左右子计划，汇总 PARAM_EXEC ID。它按 Gather rescan、NestLoop 参数、initPlan setParam 等作用域传播 `valid_params`，对不在作用域内的 Param 报错，最后填充每节点 `allParam` 与去除本节点可供给参数后的 `extParam`。
6. 修整期间同步收集关系 OID 与其他 PlanInvalItem 依赖、登记 plan node ID，并在引用已经统一后删除无用 SubqueryScan/Append/MergeAppend 节点；这些节点在此之前承担父子 var 编号隔离作用，过早消除会使 join/upper 引用无法匹配。

## 关键取舍

- **源码事实：**把 setrefs 放在 Plan 树完成后统一处理；早期转换虽可减少后续工作，却可能在父节点输出 targetlist 尚未定型时无法判定 Var 应映射到哪个子计划 resno。
- **源码事实：**upper 表达式优先复用子计划已有 targetlist 项，否则展开 PlaceholderVar 并递归修整；若缺少 Var 对应列则硬错误。盲目保留原始 base Var 会令执行器无法在 join 输出槽中找到该属性。
- **源码事实：**`SS_finalize_plan` 与 setrefs 分开且先处理子计划，因为 `extParam/allParam` 需要依赖子计划已定型的参数集；Planner caller `standard_planner` 在 `planner.c` 先对子计划再对主计划 finalize，随后才 set references。合并为一次前序遍历会在父节点处理时尚无子计划参数闭包。
- **源码事实：**先收集全局依赖和 node ID 再删除透明计划节点，以便 plan cache 失效和节点引用仍有完整来源；若在此 pass 前删去子查询/Append 层，跨 RTE 编号与 tlist 映射更难恢复。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/plan/setrefs.c` | `set_plan_references` | 建立全局执行 rangetable 并启动最终 Plan 引用/依赖修整 |
| `src/backend/optimizer/plan/setrefs.c` | `fix_scan_expr` | 转换 scan 层 Var/Param/PlaceholderVar 等表达式引用 |
| `src/backend/optimizer/plan/subselect.c` | `SS_finalize_plan` | 计算各节点参数依赖集并验证运行期参数作用域 |

## 与主核 / AI 的关系

暂无直接对应。引用定型可类比编译器把中间表示符号解析为最终操作数，但主核自身若存在执行计划引用体系需单独验证，不能映射成 PostgreSQL 的 varno/resno。

## 参考

- [PostgreSQL 官方源码：setrefs.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/setrefs.c) —— 支撑 rangetable 展平、Var 到子计划输出引用映射及节点依赖收集。
- [PostgreSQL 官方源码：subselect.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/subselect.c) —— 支撑 PARAM_EXEC 作用域校验和 allParam/extParam 计算。
- [PostgreSQL 官方源码：planner.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/planner.c) —— 支撑 finalize 与 setrefs 的真实调用顺序。
