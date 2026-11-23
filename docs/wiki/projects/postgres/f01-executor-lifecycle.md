---
project: postgres
feature: executor-lifecycle
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/execMain.c
    symbols: [ExecutorStart, ExecutorRun, ExecutorFinish, ExecutorEnd]
---

# 执行器生命周期

## 一句话职责

执行器生命周期把已规划语句包装为可初始化、分批拉取结果、完成语句级副作用并最终释放的 `QueryDesc` 执行过程。

## 机制是什么

`QueryDesc` 是 executor 的入口载体，保存 planned statement、操作类型、输入 snapshot、结果接收器以及运行期产生的 `EState`、plan state 和输出描述符。`ExecutorStart` 完成权限与只读约束检查、建立 `EState`、登记 snapshot 并初始化 PlanState 树；`ExecutorRun` 可多次调用，每次按方向和 tuple count 拉取输出；`ExecutorFinish` 收尾仍需执行的修改节点并触发 AFTER trigger；`ExecutorEnd` 关闭节点/关系、释放 slot pins 与 snapshot 引用，清空 QueryDesc 的运行期指针。

四阶段是调用者负责遵循的协议：`ExecutorEnd` 要求 `ExecutorStart` 已成功，普通执行还要求先调用 `ExecutorFinish`；EXPLAIN-only 是 Finish 例外。Run 的 count 只限制顶层取出的 tuple 数，不保证底层修改节点只处理同样数量。本页不讲 SQL 解析/优化，也不讲节点内部扫描算法；它描述这些节点的生命周期边界与 QueryDesc 上下文。

## 怎么实现的

1. `ExecutorStart` 先交给 hook 或标准实现。标准实现验证 QueryDesc 尚未启动且 active snapshot 与传入快照相同；只读事务或 parallel mode 下检查计划是否有不允许的写操作。随后创建 `EState`，进入 per-query memory context，装载参数、命令 ID、source text/query environment、instrumentation 与 registered snapshot。
2. `InitPlan` 对 range-table permission info 做权限检查、初始化 range table 与行标记，执行初始分区裁剪并通过 `ExecInitNode` 构建与 Plan 平行的 PlanState 树；结果 tuple descriptor 与 `planstate` 回填到 QueryDesc。权限失败会在计划真正执行前报错。
3. `ExecutorRun` 每次进入 EState 的 query context，启动 receiver/instrumentation，按方向设置执行状态后由 `ExecutePlan` 重复调用根 PlanState 的 `ExecProcNode`。收到空 slot 或达到 count 即结束本次；Run 之间累计 `es_total_processed`，如允许 backward scan 则保留节点状态，否则可 shutdown 节点。
4. 若单次请求完整执行计划且计划需要 parallel mode，ExecutePlan 进入 parallel mode；若已经部分执行或请求有 count 上限，则以非并行方式运行。`NoMovement` 方向只启动/关闭 receiver，不拉取节点 tuple。
5. 最后一次取数后，调用者仍须执行 `ExecutorFinish`：它让辅助 ModifyTable 节点跑到完成并触发 queued AFTER triggers（若未 skip），再标记 `es_finished`。这是独立阶段，使 EXPLAIN ANALYZE 可将 trigger 等尾部工作计入运行时间。
6. `ExecutorEnd` 检查 Finish 已完成（EXPLAIN-only 除外），调用 `ExecEndPlan` 释放节点资源、tuple table 引用与打开关系，注销 EState 注册的 snapshots，再经 `FreeExecutorState` 删除 query context，并把 QueryDesc 的 estate/planstate 等字段置空。

## 关键取舍

- **把 Finish 与 End 分开，而不是在 Run 结束时立刻触发所有副作用。** 源码注释说明 Finish 需要执行 AFTER trigger，分开后 EXPLAIN ANALYZE 能把这些工作纳入统计；代价是调用者要维护多一步协议，并且 End 用断言检查该步骤。`pquery.c` 的 portal 执行路径是实际 caller：结束 portal 执行时依次调用 Finish 与 End。
- **Run 支持分批与方向控制，但有限制时禁用 parallel mode。** 源码事实是 `ExecutePlan` 根据 `already_executed` 和 `numberTuples` 决定并行模式；部分执行后再并行执行难以正确协调。放弃在所有 fetch batch 上并行启动/继续 worker 的能力，换取 portal/游标按批取行的稳定状态语义。
- **query descriptor 保存快照借用关系，EState 注册自己的引用。** Start 要求调用者已 push 对应 active snapshot，并用 `RegisterSnapshot` 在 EState 生命周期内持有引用；End 再注销。这避免 QueryDesc 生命周期和事务外部 snapshot owner 的释放时机耦合。错误调用顺序或 snapshot 不匹配会触发断言/执行错误，不由 executor 静默修复。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/execMain.c` | `ExecutorStart` | 提供可 hook 的启动入口并转入标准初始化 |
| `src/backend/executor/execMain.c` | `ExecutorRun` | 提供可 hook 的执行入口，允许按 fetch count/方向分批驱动计划 |
| `src/backend/executor/execMain.c` | `ExecutorFinish` | 驱动尾部修改节点与语句级 AFTER-trigger 收尾 |
| `src/backend/executor/execMain.c` | `ExecutorEnd` | 清理计划、snapshot 与 EState，并重置 QueryDesc 运行期字段 |
| `src/backend/executor/execMain.c` | `ExecutePlan` | 根节点拉取循环、顶层 count 限制及 parallel mode 选择 |
| `src/backend/tcop/pquery.c` | `PortalRun` | 真实调用方，管理 portal 分次 Run 与完成后的 Finish/End 次序 |

## 与主核 / AI 的关系

暂无直接对应。理解执行过程时要区分单次 `ExecutorRun` 的 fetch 边界与整条语句的 Finish/End 生命周期；到达当前批次上限不代表修改节点或语句级 trigger 工作已全部收尾。

## 参考

- [PostgreSQL 官方源码：executor 生命周期](https://github.com/postgres/postgres/blob/master/src/backend/executor/execMain.c) —— 支撑 QueryDesc 初始化、拉取、收尾和释放顺序
- [PostgreSQL 官方源码：portal 执行协议](https://github.com/postgres/postgres/blob/master/src/backend/tcop/pquery.c) —— 支撑门户调用方如何分批驱动 executor
