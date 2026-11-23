---
project: postgres
feature: scan-nodes
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeSeqscan.c
    symbols: [ExecInitSeqScan, SeqNext]
  - path: src/backend/executor/nodeTidscan.c
    symbols: [ExecInitTidScan, TidNext]
  - path: src/backend/executor/nodeSamplescan.c
    symbols: [ExecInitSampleScan, tablesample_getnext]
---

# 表与定位扫描节点

## 一句话职责

这三类扫描节点把「从哪里取下一行」按访问模型分派出去：顺序扫描按物理顺序推进，按 TID 扫描按查询给出的行标识精确取行，抽样扫描按采样方法给出的节奏取行。

## 机制是什么

执行器的扫描节点共享同一副骨架：`ScanState` 状态、一个「取下一行」的访问函数、一个「重检」函数，外层由 `ExecScan`/`ExecScanExtended` 统一施加 qual 过滤与投影。三个节点的差别只在于访问模型：

- **顺序扫描**没有任何定位信息，从关系的一端按页顺序取行，定位交给 table AM 的扫描描述符。
- **按 TID 扫描**的定位信息由查询给出：`ctid = 某值`、`ctid = ANY(数组)`、或 `WHERE CURRENT OF`（游标当前行）。节点先把这些表达式求值成一组 TID，再逐个按 TID 取行。
- **抽样扫描**的定位信息由采样方法给出：方法决定先挑哪些块、再挑块内哪些元组，节点只负责驱动这个循环。

需要区分开的是 TID **区间**扫描（`ctid >= ...` 这类比较）：规划器为它生成的是另一种节点类型（`src/backend/executor/nodeTidrangescan.c`），走的是「先定块范围再顺序扫」的模型，不在本页展开；本页讲的是按 TID 精确取行的那一类。

本页只讲**节点侧**的取行流程。它**不负责**：一行是否对本事务可见的判定规则本身（快照与 heap AM 的可见性检查）、索引如何定位到行（索引 AM 与索引扫描节点另页）、采样方法内部如何算样本（system/bernoulli 的抽样算法属于 TABLESAMPLE 那页）、以及 qual 求值与投影的具体实现。

## 怎么实现的

1. **共同骨架**。`ExecScan` 从节点状态里取 `es_epq_active`、`qual`、`ps_ProjInfo`，转交 `ExecScanExtended`（`src/include/executor/execScan.h`）：先 `ResetExprContext`，然后循环调 `ExecScanFetch` 取行；slot 为空即扫描结束；若有 qual 则 `ExecQual` 过滤，不过则重置表达式上下文再取一行；有投影则 `ExecProject` 后返回结果 slot。`ExecScanFetch` 另外处理 EvalPlanQual 重检：EPQ 激活时它不访问数据，而是返回重检用的替代行，并调用节点各自的 recheck 回调判断该行是否仍满足访问方法条件。
2. **顺序扫描的初始化**。`ExecInitSeqScan` 建 `SeqScanState`，`ExecAssignExprContext` 挂表达式上下文，`ExecOpenScanRelation` 打开关系，`ExecInitScanTupleSlot` 用 `table_slot_callbacks(relation)` 取得该关系对应的 slot ops 并置 `TTS_FLAG_OBEYS_NOT_NULL_CONSTRAINTS`，`ExecInitResultTypeTL` 与 `ExecAssignScanProjectionInfo` 决定输出类型与是否需要投影，`ExecInitQual` 编译过滤条件。最后按 qual 与投影是否存在，从 `ExecSeqScan`、`ExecSeqScanWithQual`、`ExecSeqScanWithProject`、`ExecSeqScanWithQualProject` 四个变体里挑一个挂到 `ExecProcNode` 上。
3. **顺序扫描取行**。`SeqNext` 在 `ss_currentScanDesc` 为空时才惰性建扫描：只读关系走 `SO_HINT_REL_READ_ONLY`，开了 I/O instrumentation 就加 `SO_SCAN_INSTRUMENT`，然后 `table_beginscan`。此后每次调用 `table_scan_getnextslot(scandesc, direction, slot)`，取到就返回 slot，取不到返回 NULL。方向取自 `estate->es_direction`。`SeqRecheck` 恒返回 true：顺序扫描不向 AM 传任何 scan key，没有需要重检的访问方法条件。
4. **TID 扫描的初始化**。`ExecInitTidScan` 建状态后调 `TidExprListCreate`，把 `node->tidquals` 里的表达式分三类处理：`OpExpr` 形态（`ctid = expr`）编译成 `ExprState`；`ScalarArrayOpExpr` 形态（`ctid = ANY(...)`）同样编译但标记 `isarray`；`CurrentOfExpr`（`WHERE CURRENT OF`）没法编译，原样存进 `TidExpr` 并置 `tss_isCurrentOf`。识别 CTID 变量靠 `IsCTIDVar`——只比对 `varattno == SelfItemPointerAttributeNumber`，源码注释说明扫描 qual 里的 Var 必然属于本表，别的表的 CTID 到这里已变成 Param。
5. **TID 列表的求值**。`TidListEval` 在首次取行时才跑（注释：开扫描不是免费的，比如 heap 要统计表大小，节点也可能根本不执行）。它同样惰性 `table_beginscan_tid`，然后逐个表达式求值：单值表达式取一个 TID，数组表达式用 `deconstruct_array_builtin` 拆开；求值为 NULL 的直接跳过；`table_tuple_tid_valid` 判为无效的 TID 也静默丢弃——注释解释这是安全的，因为扫描期间持有 AccessShareLock，不会有人把要访问的块截断掉。全部求完后用 `qsort` 排序并用 `qunique` 去重：去重是为实现跨列表的 OR 语义，排序顺带让访问顺序贴近物理顺序。
6. **TID 扫描取行**。`TidNext` 用 `tss_TidPtr` 在列表上前后移动，方向同样来自 `estate->es_direction`；`WHERE CURRENT OF` 时先用 `table_tuple_get_latest_tid` 修正 TID（注释：游标取到的行此后可能已被更新，应当取按本快照算来最新的版本），再 `table_tuple_fetch_row_version` 按快照取行；取不到就推进指针继续，循环内 `CHECK_FOR_INTERRUPTS()`。列表耗尽时返回 `ExecClearTuple(slot)` 表示结束。`TidRecheck` 在非 CURRENT OF 情况下用 `bsearch` 在 TID 列表里查该行的 `tts_tid` 是否被提及。
7. **抽样扫描的初始化**。`ExecInitSampleScan` 把 `TableSampleClause` 拆开：`tsc->args` 用 `ExecInitExprList` 编译成参数表达式，`tsc->repeatable` 用 `ExecInitExpr` 编译；没有 REPEATABLE 时取一个随机种子 `pg_prng_uint32(&pg_global_prng_state)` 并固定下来（注释：种子不应在重扫之间变化）。随后 `GetTsmRoutine(tsc->tsmhandler)` 取得采样方法，`tsm->InitSampleScan` 若非空则调用。此刻**不开扫描**——`begun` 置 false，因为参数可能还求不出值。
8. **抽样扫描的取行循环**。`SampleNext` 首次调用触发 `tablesample_init`：逐个求值参数（NULL 参数报 `ERRCODE_INVALID_TABLESAMPLE_ARGUMENT`），求 REPEATABLE 的值并用 `hashfloat8` 转成种子（注释：这样 `REPEATABLE(0)` 给出与机器无关的结果，便于回归测试）；然后设默认 `use_bulkread = true`、`use_pagemode = true`，调 `tsm->BeginSampleScan`；`allow_sync` 取 `tsm->NextSampleBlock == NULL`——方法没有块级回调时才允许同步扫描；最后 `table_beginscan_sampling` 建扫描（已存在则用 `table_rescan_set_params` 更新参数）。`tablesample_getnext` 是取行主循环：先 `ExecClearTuple`，`done` 为真直接返回 NULL；没有当前块就 `table_scan_sample_next_block` 选块，返回 false 表示关系取完、置 `done`；块内用 `table_scan_sample_next_tuple` 逐个取，返回 false 表示本页取完、回到选块。取到可见行就返回 slot 并累加 `donetuples`。
9. **节点与 AM 的分工**。抽样扫描中挑块与挑块内元组由 table AM 代为调用 TSM 回调完成：`src/backend/access/heap/heapam_handler.c` 的 `heapam_scan_sample_next_block` 在 `tsm->NextSampleBlock` 非空时用它选块，否则退化为顺序推进块号并按 `SO_ALLOW_SYNC` 报告扫描位置；`heapam_scan_sample_next_tuple` 逐个问 `tsm->NextSampleTuple` 要块内偏移号，再判可见性并把元组存进 slot。
10. **重扫**。`ExecReScanSeqScan` 调 `table_rescan` 并复位扫描状态；`ExecReScanTidScan` 释放 TID 列表、把指针复位到 -1，下一轮会重新求值；`ExecReScanSampleScan` 只复位 `begun`/`done`/`haveblock`/`donetuples`，让 `tablesample_init` 重跑——种子不变，因此重扫得到的是同一批样本。

## 关键取舍

- **顺序扫描惰性开扫描。** 源码事实：`SeqNext` 里 `scandesc == NULL` 时才 `table_beginscan`，注释写明开扫描不是免费的（heap 要统计表大小），节点也可能根本不执行。放弃在 `ExecInitSeqScan` 阶段一次性建扫描，换取计划未被执行或走 EPQ 路径时不做无用功；代价是取第一行时多一次判断，扫描描述符的生命周期也从 init 阶段推迟到首次取行。
- **把「要不要过滤/投影」固化进 `ExecProcNode` 变体。** 源码事实：`ExecInitSeqScan` 按 qual 与 `ps_ProjInfo` 是否为空挑四个变体，变体用常量 NULL 调用 `ExecScanExtended`，让编译器把对应分支消掉。放弃「一个 `ExecProcNode` 靠运行时判断 qual/projInfo」的统一写法，换取热路径上没有多余的 NULL 检查；代价是节点代码里出现四份几乎相同的包装。
- **TID 列表排序去重。** 源码事实：`TidListEval` 对多于一个 TID 的情况用 `qsort` + `qunique`，注释说明去重是为 OR 语义、排序顺带让访问更高效。放弃「不去重、按求值顺序逐个访问」，换取重复 TID 只访问一次且访问顺序贴近物理顺序；代价是每次求值列表要付一次排序开销，元素多时是 O(n log n)。
- **抽样扫描把参数求值推迟到首次取行。** 源码事实：`ExecInitSampleScan` 只编译表达式与算种子，`tablesample_init` 在首次取行时才求值参数并建扫描。放弃在 init 阶段建扫描（那时参数可能依赖尚未求值的 Param），换来采样参数可以是运行时值；代价是节点状态多一个 `begun` 标志，且每条重扫路径都必须显式复位它。
- **采样方法没有块级回调时退化为顺序扫块。** 源码事实：`allow_sync = (tsm->NextSampleBlock == NULL)`，而 `heapam_scan_sample_next_block` 在 `tsm->NextSampleBlock` 为空时按 `rs_cblock + 1` 顺序推进并在到达末尾时绕回起点；bernoulli 方法正是这种形态（其 `TsmRoutine` 里 `NextSampleBlock` 为 NULL）。放弃「没有块级回调就报错」，换来任何采样方法都能接入；代价是这类方法无法跳块，I/O 量与全表扫描同量级。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeSeqscan.c` | `ExecInitSeqScan` | 建状态、开关系、按关系取 slot ops、编译 qual，并按 qual/投影是否存在挑选 `ExecProcNode` 变体 |
| `src/backend/executor/nodeSeqscan.c` | `SeqNext` | 惰性开扫描并用 `table_scan_getnextslot` 逐行取元组 |
| `src/backend/executor/nodeTidscan.c` | `ExecInitTidScan` | 建状态并把 `tidquals` 分类编译成 TID 表达式列表（含 `CURRENT OF` 特例） |
| `src/backend/executor/nodeTidscan.c` | `TidNext` | 在求值好的 TID 列表上前后推进，按快照逐个取行，取尽后清空 slot |
| `src/backend/executor/nodeSamplescan.c` | `ExecInitSampleScan` | 编译采样参数与 REPEATABLE、固定随机种子、取得 TSM 回调，但推迟开扫描 |
| `src/backend/executor/nodeSamplescan.c` | `tablesample_getnext` | 抽样扫描的取行主循环：选块、块内取元组、关系取完即结束 |

## 与主核 / AI 的关系

主核的执行层（`engine/db/src/query/exec.c`）走的是批量扫描路径：一次把一批行读进 `BatchCtx` 再逐条处理，没有 PostgreSQL 这种「节点一次交一行」的拉取协议，也没有按访问模型分派扫描节点。若主核后续需要支持按行标识定位或抽样读取，可借鉴这里的三点：把「定位信息」在取行前一次性解析成列表、把块级与行级抽样拆成两个回调、以及在方法缺少块级回调时给出明确退化路径而不是报错。

## 参考

- [PostgreSQL 官方源码：顺序扫描节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeSeqscan.c) —— 支撑惰性开扫描与四个 `ExecProcNode` 变体的选择逻辑
- [PostgreSQL 官方源码：TID 扫描节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeTidscan.c) —— 支撑 TID 表达式分类、列表排序去重与按快照取行
- [PostgreSQL 官方源码：抽样扫描节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeSamplescan.c) —— 支撑参数延迟求值、种子固定与选块/取行主循环
- [PostgreSQL 官方源码：扫描节点的统一外壳](https://github.com/postgres/postgres/blob/master/src/include/executor/execScan.h) —— 支撑 qual 过滤、投影与 EPQ 重检的公共路径
