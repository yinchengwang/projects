---
project: postgres
feature: node-dispatch
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/execProcnode.c
    symbols: [ExecInitNode, ExecProcNodeFirst, MultiExecProcNode]
---

# 节点初始化与驱动

## 一句话职责

执行器分派器把计划节点类型映射到运行期 PlanState，并以按需拉取 tuple slot 的方式递归驱动计划树。

## 机制是什么

计划树保存优化后的逻辑操作；执行期的 `PlanState` 树与其结构对应，但加入每个节点的 callback、输入子树、结果 slot、表达式上下文、参数变化和 instrumentation 状态。`ExecInitNode` 按 `Plan` 的 node tag 选择特定 `ExecInit...` 构造器，节点构造器再初始化自身子计划；初始化完成后分派器记录节点的真实 `ExecProcNode` 实现。

普通 tuple-producing 节点通过 `ExecProcNode` 一次产生一个 `TupleTableSlot`，父节点决定何时再向子节点请求下一行；到达 end-of-stream 时返回空 slot。Hash、bitmap index/AND/OR 等需要一次生成整体结构而非逐行返回的节点走 `MultiExecProcNode`，返回 `Node *` 且由 caller 校验类型。这里负责通用分派/树驱动，不负责各节点的连接、扫描或聚合算法，也不负责 SQL 计划的选择。

## 怎么实现的

1. `ExecInitNode(NULL, ...)` 直接返回 NULL，作为叶节点/可选子树的终止条件；非空节点先做 stack-depth 检查，再按 `nodeTag` 分派到对应初始化函数。未知 tag 报 ERROR，避免错误的运行期类型被误当成合法节点。
2. 节点初始化器为自身分配具体 `PlanState` 子类、绑定共享 `EState`，并按需创建表达式上下文、slot、relation、qual 和 projection。父节点初始化器递归调用 `ExecInitNode` 构建子树，因此 PlanState 的 left/right/专属 child links 对应计划树的输入关系。
3. 返回后，分派器通过 `ExecSetExecProcNode` 保存真实 callback 到 `ExecProcNodeReal`，先将公开 callback 指向 `ExecProcNodeFirst`；initPlan 与 instrumentation 也在这里挂到结果 PlanState。
4. 节点第一次由 `ExecProcNode` 调用时，`ExecProcNodeFirst` 执行一次 stack-depth 检查，然后根据是否有 instrumentation 将 callback 改为 `ExecProcNodeInstr` 或直接改为真实方法。后续逐 tuple 调用免去 first-call 包装；如果 callback 运行期改变，通过 setter 重新安装 wrapper。
5. 头文件中的 `ExecProcNode` inline wrapper 先检查 `chgParam`。有参数变化就调用 `ExecReScan` 让节点重置自身状态，再调用节点 callback 取 slot；上层通常由 ExecutePlan 重复拉取，直到 slot empty 或用户/portal fetch 限制到达。
6. `MultiExecProcNode` 另做 stack-depth 与 interrupt 检查，同样处理 `chgParam` 后按 state tag 分派到支持 MultiExec 的 hash/bitmap 实现。它不做通用 per-node instrumentation 计数，因为返回整体对象而非逐 tuple；相应实现自行处理统计。

## 关键取舍

- **首调做一次安全检查并随后直接换成真实 callback，而非每次都经过通用 switch。** 源码事实：`ExecProcNodeFirst` 第一次运行后会改写 `ExecProcNode`，没有 instrumentation 时直接指向 `ExecProcNodeReal`。放弃每行都做 node-tag 分派与堆栈检查，换取热路径开销更低；约束是 callback 生命周期内必须遵守通过 `ExecSetExecProcNode` 更新的约定，不能私自覆盖字段并期望 instrumentation wrapper 自动修复。
- **分开逐行拉取与 MultiExec 整体返回，而非强迫所有节点用 slot 协议。** Hash build 或 bitmap 生成的结果是表/集合，不是一个 tuple 序列；若逐行编码，会增加中间对象与往返协议。代价是 MultiExec 调用方需要知道返回 Node 的具体类型，分派器无法统一计数结果行数。
- **参数变化由 wrapper 统一触发 ReScan，而非每个父节点单独追踪 child 是否过期。** `ExecProcNode` 和 `MultiExecProcNode` 均在 dispatch 前检查 `chgParam`；放弃父节点对全部依赖参数的重复判断，换取节点自有 rescan 逻辑处理状态更新。若错误漏传 `chgParam`，callback 仍会沿用过期执行状态，因此参数传播仍是计划节点协议的一部分。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/execProcnode.c` | `ExecInitNode` | 按 Plan node tag 选择具体初始化器并递归形成 PlanState 树 |
| `src/backend/executor/execProcnode.c` | `ExecProcNodeFirst` | 首次拉取时检查调用栈并将 callback 变为 instrumentation 或直接执行方法 |
| `src/backend/executor/execProcnode.c` | `MultiExecProcNode` | 分派只返回整体结构的 hash/bitmap 节点并处理参数重扫 |
| `src/include/executor/executor.h` | `ExecProcNode` | 每次拉取前检查 changed Params，再调用 PlanState 上的节点方法 |
| `src/include/nodes/execnodes.h` | `PlanState` | 保存 Plan/EState、真实/包装 callback、子树与结果 slot 等执行期状态 |

## 与主核 / AI 的关系

暂无直接对应。实现 plan executor 时可复用按节点状态对象分派的拉取模型；应将逐行 slot 与整体构建结果分开建模，并明确参数变化后的 rescan 责任。

## 参考

- [PostgreSQL 官方源码：执行节点分派](https://github.com/postgres/postgres/blob/master/src/backend/executor/execProcnode.c) —— 支撑初始化、首调 callback 安装及 MultiExec 分派
- [PostgreSQL 官方源码：PlanState 与 ExecProcNode 接口](https://github.com/postgres/postgres/blob/master/src/include/executor/executor.h) —— 支撑 pull 调用与参数变化重扫协议
- [PostgreSQL 官方源码：PlanState 结构](https://github.com/postgres/postgres/blob/master/src/include/nodes/execnodes.h) —— 支撑运行态树与 callback/slot 字段
