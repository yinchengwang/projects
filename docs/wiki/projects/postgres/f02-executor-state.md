---
project: postgres
feature: executor-state
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/execUtils.c
    symbols: [CreateExecutorState, FreeExecutorState, CreateExprContext, FreeExprContext]
  - path: src/backend/executor/execTuples.c
    symbols: [ExecResetTupleTable]
---

# 执行状态与资源生命周期

## 一句话职责

`EState` 汇总一次 executor invocation 的共享运行状态，`ExprContext` 与 tuple table 为节点提供可重置的表达式工作区和带资源所有权的行槽。

## 机制是什么

`CreateExecutorState` 建立以当前 memory context 为父级的 query context，并在其中分配 `EState`。它保存当前方向/snapshot、range table 与 Relation 缓存、参数、tuple table、ExprContext 列表、plan/subplan state、累计处理计数及 instrumentation 等。`ExecutorStart` 将 snapshot 引用注册到 EState；PlanState 节点指向同一个 EState，但各自拥有需要表达式计算时才创建的 `ExprContext`。

一个 ExprContext 有 query-lifespan context 和 per-tuple context：函数缓存等长期数据可放前者，短期表达式结果放后者，通常每次输出 tuple 前 reset。TupleTableSlot 通过 ops 管理内容：slot 可以持 buffer pin 或拥有 palloc tuple，释放需先清空内容并调用 slot release，再释放 descriptor 引用。该页讲运行状态与这些资源容器的分配/重置/释放，不描述 executor 生命周期入口（f01）或 slot 变体的表示细节（f05）。

## 怎么实现的

1. `CreateExecutorState` 创建 per-query AllocSet context，在其中零初始化 `EState` 字段；状态对象和整棵计划执行期工作数据都由 `es_query_cxt` 管辖。调用者可在任意非待删除 context 中调用 `FreeExecutorState`，但不能在这个 query context 本身中调用。
2. `CreateExprContext` 在 query context 分配 `ExprContext`，另在其下创建 `ecxt_per_tuple_memory`，并把 context 链到 `estate->es_exprcontexts`。`ecxt_scantuple`、`ecxt_innertuple`、`ecxt_outertuple` 为 Var 求值绑定输入 slot；params、aggregate 值、CASE/domain 临时值也经此共享。
3. 节点或执行器按需取得 ExprContext；每次处理新 tuple 调用 reset 路径清理 per-tuple memory，重扫时 `ReScanExprContext` 先运行 shutdown callbacks，再 reset 内存，从而取消仍未完成的 set-returning 函数等状态。
4. `FreeExprContext` 先调用 `ShutdownExprContext`（commit cleanup 会运行 callback，错误清理只释放内存），删除 per-tuple context，从 EState 的 `es_exprcontexts` 列表脱链，再释放 ExprContext 对象。`FreeExecutorState` 循环处理遗留 contexts、释放 JIT context/partition directory，最后整体删除 query context。
5. `ExecResetTupleTable` 对 table 中每个 slot 先 `ExecClearTuple` 再调用 `tts_ops->release`，确保释放 buffer pin 或 owned tuple；随后 ReleaseTupleDesc。`shouldFree` 为真时另外释放 slot 的 Datum/null arrays（非固定 descriptor 情形）、slot 和 list；为假则保留 slot 对象供重用。
6. 错误中止路径可能直接回收 query context，但 `FreeExecutorState` 仍显式关闭 ExprContext，确保注册的非内存 callback 资源也被处理。相对地，tuple table slot 的 buffer pins/descriptor 引用必须由 `ExecResetTupleTable` 或节点 End 路径先清理；query context 自动回收内存并不等价于释放这些外部资源。

## 关键取舍

- **把大部分 query-lifespan 内存统一放进一个 context，而非每个对象单独释放。** 源码事实：EState 本身也在 `es_query_cxt` 中，最终 `MemoryContextDelete` 一次回收工作数据。放弃精细逐项 free 的复杂度，代价是必须把外部资源（pin、关系锁/引用、函数 callback、JIT context）显式挂到正确 shutdown 路径，不能指望 allocator 清理它们。
- **为每个 ExprContext 单设 per-tuple context，而非将短期 Datum 结果留在 EState query context。** 这样每个 tuple 后可 reset 大量 pass-by-reference 临时结果而不触碰长寿命缓存；代价是为每个上下文增加 memory-context 元数据和 reset 操作。真实 caller 是 `ExecutorRun` 中每次根节点拉取前的 `ResetPerTupleExprContext`，它防止结果内存随着 tuple 数增长。
- **tuple table reset 将“释放资源”和“释放 slot 内存”分开。** `shouldFree=false` 仍会 clear/release 内容、放掉 descriptor 引用，但保留 slot 以便 EPQ 或节点复用；若每次只置空内容而不调 ops release，buffer-backed slot 的 pin 会泄漏。反过来每次都释放 slot 会令重用场景频繁重新分配。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/execUtils.c` | `CreateExecutorState` | 创建 query context，并在其中初始化一次 executor invocation 的 EState |
| `src/backend/executor/execUtils.c` | `FreeExecutorState` | 收尾残留表达式上下文、JIT/partition 资源并删除整个 query context |
| `src/backend/executor/execUtils.c` | `CreateExprContext` | 为节点创建 query-lifespan ExprContext 与独立 per-tuple workspace，并登记给 EState |
| `src/backend/executor/execUtils.c` | `FreeExprContext` | 执行 shutdown callback、释放 per-tuple context 并从 EState 脱链 |
| `src/backend/executor/execTuples.c` | `ExecResetTupleTable` | 逐 slot 清理数据、pin 与 descriptor，可选择保留或销毁 slot 本身 |
| `src/backend/executor/execMain.c` | `ExecutePlan` | 每次输出 tuple 拉取前 reset per-tuple expression context |

## 与主核 / AI 的关系

暂无直接对应。设计查询执行状态时应把 query context 与 per-tuple 临时结果分开，并为 buffer pin、descriptor 引用等非内存资源定义显式 release 路径。

## 参考

- [PostgreSQL 官方源码：EState 与 ExprContext 管理](https://github.com/postgres/postgres/blob/master/src/backend/executor/execUtils.c) —— 支撑上下文分层、重扫 callback 与释放顺序
- [PostgreSQL 官方源码：TupleTableSlot 资源重置](https://github.com/postgres/postgres/blob/master/src/backend/executor/execTuples.c) —— 支撑 slot 内容释放与 descriptor 引用处理
- [PostgreSQL 官方源码：EState/ExprContext 结构](https://github.com/postgres/postgres/blob/master/src/include/nodes/execnodes.h) —— 支撑运行状态与 tuple/param 上下文字段
