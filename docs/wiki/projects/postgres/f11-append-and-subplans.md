---
project: postgres
feature: append-and-subplans
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeAppend.c
    symbols: [ExecAppend, choose_next_subplan_for_leader]
  - path: src/backend/executor/nodeMergeAppend.c
    symbols: [ExecMergeAppend]
  - path: src/backend/executor/nodeSubplan.c
    symbols: [ExecSubPlan, ExecSetParamPlan]
---

# Append 族与子计划

## 一句话职责

Append 族汇合多个子计划的行流，SubPlan 与 initplan 则在表达式求值点执行子查询并传回标量、布尔或参数结果。

## 机制是什么

普通 Append 按顺序耗尽一个 child 后切到下一个；运行期分区裁剪可先过滤无效 child，异步 child 可与同步 child 重叠推进。MergeAppend 不拼接而是按排序键从各 child 的当前头部取最小/最大项，保持合并结果有序。并行感知 Append 通过共享 finished 标志和锁把 partial subplan 分配给 worker/leader。

SubPlan 是表达式内嵌的计划执行入口：相关子计划在每次表达式求值时按 `parParam` 设置参数并重扫，未相关且可哈希的子计划则预建结果哈希表。initplan 不经 `ExecSubPlan` 逐次调用，而由 `ExecSetParamPlan` 在需要其输出参数时执行一次并写入 `PARAM_EXEC`；CTE 子计划由 CTE scan/tuplestore 协议消费，不能冒充普通 SubPlan。这些节点不负责规划期把子查询拉平、改写成 join、生成计划或判断分区裁剪表达式的语义。

## 怎么实现的

1. `ExecAppend` 首次调用时根据 `as_valid_subplans` 选同步子计划，并启动 async child 请求；同步计划逐个 `ExecProcNode` 拉取，child 返回 tuple 即原 slot 直接向上传递，child EOF 后选下一个。若异步请求尚未完成，就等待/轮询事件后再归并结果。
2. 并行感知 Append 的 leader 通过 `choose_next_subplan_for_leader` 获取 `ParallelAppendState.pa_lock` 排他锁，按分配顺序寻找未完成子计划；选择非 partial 计划即把它标为 finished，partial 计划则只在某 worker 执行完时标记完成，允许多 worker 合作跑同一 partial child。
3. `ExecMergeAppend` 首次从每个有效 child 各取一个 tuple，child 编号加入 `binaryheap`；之后只推进堆顶对应 child 并替换/移除该项。异步 child 在首次阶段可并发发请求，但合并期间最多对当前堆顶有一个未完成请求，因为必须知道其下一项才能决定全局最小输出。
4. `ExecSubPlan` 先强制正向扫描，然后按 `useHashTable` 分到 `ExecHashSubPlan` 或 `ExecScanSubPlan`。扫描式路径把 `parParam` 加到 child `chgParam` 后调用 `ExecReScan`，逐行按 EXISTS、ANY/ALL、EXPR、ARRAY 等 SubLink 类型累积结果；EXPR/MULTIEXPR/ROWCOMPARE 多行会触发 cardinality violation。
5. 哈希式 SubPlan 不允许直接相关参数；首次或 stale 时执行 `buildSubPlanHash` 建主表及含 NULL 的辅助表。LHS 无 NULL 时先精确 probe，若未匹配且右侧有 NULL 行，再用 partial match 判定 SQL 三值逻辑的 UNKNOWN；空子计划立即返回 false。
6. `ExecSetParamPlan` 校验 initplan 未相关且非 CTE/ANY/ALL，切换到 per-query memory context 正向执行。EXISTS/ARRAY/EXPR 等结果转存到 `ParamExecData` 的 `setParam`；by-reference 值先复制 heap tuple 或数组，避免 child slot 生命周期结束后悬空。
7. 子计划结束与重扫通过 `ExecReScanSubPlan` 等清理/失效路径重置哈希表或标记 stale；CTE 不可从 `ExecSubPlan`/`ExecSetParamPlan` 进入，executor 对这种误用显式报错。

## 关键取舍

- 源码事实：Append 直接向上传递 child slot，不额外复制成统一 tuple；初始化时仅为 end-of-stream 建结果 slot。这样避免每条行都做投影/拷贝，代价是父节点必须遵守 slot 由当前 child 所有的生命周期约定。
- 源码事实：MergeAppend 用 binary heap 而不是每次线性扫描所有 child 头部；每次只需替换堆顶，适合多路归并，但需要为每个 child 保存当前 slot 和堆状态，初始化也必须先拉到每路首行。
- 源码事实：`choose_next_subplan_for_leader` 持 `pa_lock` 串行领取子计划，并以 `pa_finished` 区分普通计划和 partial 计划完成时机。相较每个 worker 各自从头跑全套子计划，这避免非 partial 输入重复扫描；代价是领取时需竞争共享锁，且某些非 partial 计划只由一个 worker 执行。
- 源码事实：SubPlan 哈希只允许无直接相关参数的子计划；相关子计划走 scan/rescan 以便每轮使用新参数。把相关结果强行哈希缓存需键化全部参数并处理失效/NULL 部分匹配，复杂度高于按需重扫；扫描法的代价是相关外层行多时会重复执行 child。
- 源码事实：initplan 以 `execPlan` 延迟标记和 `PARAM_EXEC` 保存结果，第一次取参数时才执行；若每次表达式读取都重跑 initplan 会重复工作，但惰性执行意味着异常或副作用也推迟到参数实际消费时发生。CTE 被明确排除在这两条接口外，避免其共享结果生命周期被误当作 scalar subquery。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeAppend.c` | `ExecAppend` | 按有效子计划顺序取行，并协调同步/异步结果 |
| `src/backend/executor/nodeAppend.c` | `choose_next_subplan_for_leader` | 持共享锁分配并标记并行 Append 的 leader 子计划 |
| `src/backend/executor/nodeMergeAppend.c` | `ExecMergeAppend` | 初始化各路首行并通过堆顶推进有序多路归并 |
| `src/backend/executor/nodeSubplan.c` | `ExecSubPlan` | 根据相关性与 useHashTable 选择扫描或哈希子计划执行 |
| `src/backend/executor/nodeSubplan.c` | `ExecSetParamPlan` | 执行延迟 initplan 并将结果写入 PARAM_EXEC 参数槽 |

## 参考

- [PostgreSQL 文档：UNION 与集合运算](https://www.postgresql.org/docs/current/queries-union.html) —— 说明多输入行流合并及排序相关语义。
- [PostgreSQL 文档：子查询表达式](https://www.postgresql.org/docs/current/functions-subquery.html) —— 说明 EXISTS/ANY/ALL 等子查询结果语义。
- [PostgreSQL 文档：WITH 查询](https://www.postgresql.org/docs/current/queries-with.html) —— 说明 CTE 的查询语义及与普通子计划的边界。
- [PostgreSQL 官方源码：Append 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeAppend.c) —— 支撑同步、异步和并行子计划推进。
- [PostgreSQL 官方源码：SubPlan 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeSubplan.c) —— 支撑哈希/扫描 SubPlan 与 initplan 参数填充。

## 与主核 / AI 的关系

暂无直接对应。主核可分别采用串接、优先堆归并、参数化子查询执行与延迟参数填充等执行形态；共享子计划分配和 SQL 三值逻辑必须由自身执行器协议承接。
