---
project: postgres
feature: parallel-execution
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeGather.c
    symbols: [ExecInitGather, gather_getnext]
  - path: src/backend/executor/nodeGatherMerge.c
    symbols: [ExecGatherMerge]
  - path: src/backend/executor/execParallel.c
    symbols: [ExecInitParallelPlan, ParallelQueryMain]
---

# 并行执行

## 一句话职责

Gather 与 GatherMerge 管理并行计划 worker 的启动和元组汇聚，execParallel 则把计划、参数和执行环境交给 worker 并运行其计划副本。

## 机制是什么

并行计划的 worker 执行 partial plan，各自输出 tuple 到 leader 可读取的 tuple queue；Gather 从多个队列和可选的 leader 本地执行路径取行，不保证跨 worker 的全局排序。GatherMerge 要求每路 child 输出满足相同排序规则，以 binary heap 合并队列头，保持顺序输出。若 worker 数量未能按请求启动，Gather 可在 leader 本地执行 child，计划仍能完成。

本文讨论执行节点的协调协议，不讲 DSM/DSA 的共享内存分配、布局与生命周期（见 A19）；此处只说明执行器如何序列化计划、传参、建立读取器、投递任务和等待结果。它不负责选择哪些算子可并行，也不负责 SQL 排序语义生成或 worker 进程启动策略本身；后者由 parallel context/runtime 协议承接。

## 怎么实现的

1. `ExecInitGather` 建立 `GatherState` 与 child `PlanState`，初始化时不创建大共享段或启动 worker；真正进入 `ExecGather` 首次取行时才按 `num_workers`、`es_use_parallel_mode` 调用 `ExecInitParallelPlan`。
2. `ExecInitParallelPlan` 先执行需要传给 worker 的 initplan 参数，序列化 `PlannedStmt`，创建 `ParallelContext`，遍历 planstate 让 parallel-aware 节点估算共享需求并初始化其共享状态；随后建立 DSM/toc、序列化 query text 与 `ParamListInfo`，设置 tuple queues、BufferUsage/WalUsage 及必要的 instrumentation。
3. Gather 调 `LaunchParallelWorkers` 后记录实际启动数，为 worker 建 `TupleQueueReader`。worker 不足甚至为零时会设置 `need_to_scan_locally`；leader 可继续运行自己的 child 副本。执行器不会假定请求数等于实际启动数。
4. `gather_getnext` 轮询各 worker queue，取到 tuple 就放进 funnel slot 返回；轮询一圈都没结果且 leader 还需扫描时返回空指示给上层，由 `ExecGather` 主循环随后从 child 本地取行。没有本地工作时，leader 通过 latch 等待 queue 新数据或 worker 完成事件。
5. `ExecGatherMerge` 同样惰性初始化 worker，但由 `gather_merge_getnext` 首次从 leader/每个 worker 取首 tuple 建 heap；每次输出 heap 顶端 participant 的 tuple，再只推进该 participant 并更新 heap。worker tuple buffer 可用 nowait 预取，但全局顺序仍由堆顶比较器决定。
6. `ParallelQueryMain` 在 worker 中从 toc 取固定状态、receiver、serialized plan 和参数信息，挂接 DSA area，调用 `ExecutorStart`，恢复 `PARAM_EXEC` 值，调用 `ExecParallelInitializeWorker` 初始化并行感知节点，再按 tuple bound 执行 `ExecutorRun`，把结果送到队列。
7. worker 结束时向共享统计区汇报 buffer/WAL 使用与执行器 instrumentation；leader 在队列耗尽或节点关闭时 `ExecParallelFinish` 等待并收尾 worker，再 `ExecParallelCleanup` 释放 parallel context。Rescan 则先关闭旧 worker，再在下一次执行时重建或 reinitialize DSM 共享状态。

## 关键取舍

- 源码事实：worker 与 leader 只在 Gather 节点首次取数时启动，而不在 `ExecInitGather` 时启动。这避免计划最终未被消费时创建进程和大共享段；代价是首行延迟包含 worker 初始化/启动时间。
- 源码事实：worker 启动数可少于请求数或为零，Gather 会转用 leader 本地 child。相比将 worker 不足当成查询失败，这使并行计划具备可回退执行能力；代价是实际并行度和 EXPLAIN 中的 worker 数可能低于计划预期。
- 源码事实：Gather 轮询 worker queue，轮空时若 leader 可参与就暂时回到本地执行，而不阻塞等待任一具体 worker；GatherMerge 则必须知道每个 participant 当前最小头部才能安全输出下一行，因此更强的等待/预取要求换取有序结果。
- 源码事实：`ExecInitParallelPlan` 先把 initplan 输出物化为可序列化参数，再把 PlannedStmt 和参数副本发送给 worker；worker 在独立 executor 中运行同一计划，而不是让 leader 把每个 tuple 当任务远程派发。这样保留并行计划树的执行语义，但要求计划节点与参数状态明确区分本地状态和可共享/可恢复状态。
- 源码事实：A19 负责 DSM 基础分配与同步原语；这里的 `ExecInitParallelPlan` 仅使用 ParallelContext/toc/DSA 来建立执行会话。把两页混写会把执行协作策略与底层共享内存机制混为同一层。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeGather.c` | `ExecInitGather` | 创建 Gather 状态并初始化 child，延迟 worker 实际启动到首次执行 |
| `src/backend/executor/nodeGather.c` | `gather_getnext` | 在 worker tuple queues 与 leader 本地 child 之间拉取结果 |
| `src/backend/executor/nodeGatherMerge.c` | `ExecGatherMerge` | 首次启动 worker 并建立有序汇聚所需状态 |
| `src/backend/executor/execParallel.c` | `ExecInitParallelPlan` | 序列化计划与参数、估算执行共享状态并创建 worker 通信上下文 |
| `src/backend/executor/execParallel.c` | `ParallelQueryMain` | worker 恢复计划/参数状态、初始化 worker 节点并执行计划 |

## 参考

- [PostgreSQL 文档：并行查询](https://www.postgresql.org/docs/current/parallel-query.html) —— 说明 parallel plan、leader/worker 参与及结果汇聚。
- [PostgreSQL 文档：并行查询如何工作](https://www.postgresql.org/docs/current/how-parallel-query-works.html) —— 说明 Gather/GatherMerge 与 worker 数量、leader 工作的关系。
- [PostgreSQL 官方源码：Gather 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeGather.c) —— 支撑队列读取与 leader fallback 流程。
- [PostgreSQL 官方源码：并行计划执行器](https://github.com/postgres/postgres/blob/master/src/backend/executor/execParallel.c) —— 支撑计划序列化、worker 初始化和执行过程。

## 与主核 / AI 的关系

暂无直接对应。主核若采用并行执行，可复用“worker 执行 partial plan、leader funnel 行流、顺序汇聚用堆”的分层；DSM 分配与 worker 协议应仍与具体算子节点职责分开。
