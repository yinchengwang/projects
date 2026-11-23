---
project: postgres
feature: join-algorithms
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeNestloop.c
    symbols: [ExecNestLoop, ExecReScanNestLoop]
  - path: src/backend/executor/nodeMergejoin.c
    symbols: [ExecMergeJoin, MJCompare]
  - path: src/backend/executor/nodeHashjoin.c
    symbols: [ExecHashJoin, ExecHashJoinNewBatch]
---

# 连接执行算法

## 一句话职责

三个连接节点按各自算法把已选定的 outer/inner 子计划组合成连接结果，并维护重扫、标记与批次边界状态。

## 机制是什么

执行侧的连接节点只消费优化器给定的计划形状：Nested Loop 逐条 outer 驱动内表重扫；Merge Join 依赖两路已按连接键有序，用标记与回退对齐重复键组；Hash Join 先用内表建哈希表再逐条探测外表，内存不足时按批溢写。三者的接手范围是元组流、状态机、参数传递与溢写，不包含连接顺序枚举、连接算法选择或代价估算。

本机制明确不负责连接类型合法性判定、行可见性与最终投影语义。连接类型（inner、semi、anti、left、right、full）只改变节点内的空值补齐与提前终止分支；`joinqual` 决定是否算作匹配，`qual` 决定是否真的返回该结果。参数化路径里的参数值由 `nestParams` 按 `PARAM_EXEC` 槽写入，再通过 `chgParam` 通知子树失效，这属于执行期参数协议而非计划搜索。

## 怎么实现的

1. `ExecNestLoop` 是两段循环：`nl_NeedNewOuter` 为真时先从 outer 取一条元组，往 `econtext->ecxt_outertuple` 落位，清 `nl_MatchedOuter`，逐个把 `nestParams` 里 `OUTER_VAR` 变量求值写入 `ParamExecData`，把这些 paramno 加进 `innerPlan->chgParam`，再 `ExecReScan(innerPlan)`。
2. Nested Loop 的内层循环每次 `ExecProcNode(innerPlan)` 取一条内表元组。内表耗尽时置 `nl_NeedNewOuter`；如果此前没有匹配且连接类型是 left 或 anti，就用预先创建的 `nl_NullInnerTupleSlot` 组成空值补齐行并检查 `otherqual`。匹配分支里 `joinqual` 通过即置 `nl_MatchedOuter`，anti 连接直接跳过返回，`single_match`（semi 或 inner_unique）则在返回后强制换 outer。
3. `ExecReScanNestLoop` 只重扫 outer（且 `chgParam` 非空时留给首次 `ExecProcNode` 触发），代码注释明确要求它不得重扫 inner，因为内表通常是指索引扫描，运行期键值必须随新的 outer 元组重新绑定。
4. `ExecMergeJoin` 是显式状态机：`EXEC_MJ_INITIALIZE_OUTER/INITIALIZE_INNER` 取首个元组，`EXEC_MJ_SKIP_TEST` 在两侧不相等时按大小推进一侧，相等时 `ExecMarkPos(innerPlan)` 标记，随后 `JOINTUPLES` 逐条配对、`NEXTINNER` 推进内表，`NEXTOUTER` 取新外表后到 `TESTOUTER` 与已标记元组比较。
5. `MJCompare` 按 `mj_NumClauses` 逐个用 `ApplySortComparator` 比较已预求值的左右键，遇第一个非零即返回。若出现 NULL 对 NULL，或计划侧置了 `mj_ConstFalseJoin`，则把相等结果强制改成 +1，从而推进 inner 而不是判为匹配；内存分配切换到 `ecxt_per_tuple_memory`，避免比较函数泄漏留在长期上下文。
6. `ExecMergeJoin` 在 `TESTOUTER` 判定新 outer 仍等于标记元组时用 `ExecRestrPos(innerPlan)` 回到标记位置重扫这批内表重复键；`mj_SkipMarkRestore` 为真则跳过该动作，直接用当前内表位置。右/全连接时右路未匹配元组需要补空，`MJFillOuter`/`MJFillInner` 生成含空值槽的候选行并只做 `otherqual` 过滤。
7. `ExecHashJoin` 实为 `ExecHashJoinImpl(pstate, false)` 的并行无关外壳，内部状态机围绕 `hj_JoinState` 推进：`HJ_NEED_NEW_OUTER` 取外表并求哈希，`HJ_SCAN_BUCKET` 用 `ExecScanHashBucket` 在桶内游走，命中后检查 `joinqual`、置 `hj_MatchedOuter`、把 `HJTUPLE_MINTUPLE` 的头部匹配位标记上，再看 `single_match` 与 anti/right-anti 语义决定是否返回，`HJ_FILL_OUTER_TUPLE`、`HJ_FILL_INNER_TUPLES`、批次末端的 NULL 键元组补发都在同一循环内完成。
8. `ExecHashJoinNewBatch` 换批：先关掉上一批的外表临时文件；首批结束时关闭 skew 优化状态（`skewEnabled = false` 等）。随后从 `curbatch + 1` 起跳过内外两侧都为空的批次，但保留三类例外：外表连接必须处理只有外表的批、右/右反/全连接必须处理只有内表的批、以及在建表或开始外表扫描之后 `nbatch` 曾增长过时必须重扫该侧，因为其中元组可能要被重分配到更后面的批。真正选中的批会 `ExecHashTableReset`、回绕并重读该批内表文件边读边 `ExecHashTableInsert`、回绕外表文件，返回 true；扫到末尾返回 false。

## 关键取舍

- 源码事实：`ExecInitNestLoop` 在 `nestParams` 非空时把 `EXEC_FLAG_REWIND` 从 inner 子树的 eflags 中剔除，并在 `ExecNestLoop` 每次换 outer 时调用 `ExecReScan(innerPlan)`。放弃“内表缓存后回绕重放”换来按新参数重新走索引；若强行让内表支持 rewind，参数化索引扫描会拿到过期键值，或者需要额外缓存整表而失去参数化路径的意义。
- 源码事实：`MJCompare` 把 NULL 对 NULL 判成 +1 而不是 0，`ExecMergeJoin` 的标记/回退逻辑依赖这一点才能正确地推进内表。放弃“NULL 即相等”的直觉写法，是因为那会让重复键组边界判断出错，进而使 `TESTOUTER` 的回退分支把不匹配元组当成匹配重扫。
- 源码事实：hash join 的批数按 2 的幂增长，但 `nodeHashjoin.c` 头注释说明一旦某次加倍后出现“保留了全部或全部不保留元组”的批，就全局禁用继续增长，此后这些批只能超预算溢写。放弃继续加倍换取避免加倍后仍然切不开的批反复重写临时文件，代价是内存约束被突破，退化路径落到磁盘 I/O。
- 源码事实：`ExecHashJoinNewBatch` 在 `curbatch > 0` 时立即关闭上一批外表文件，并在跳过空批时顺手关闭其临时文件。放弃“保留所有批文件到最后统一清理”是为了尽早释放磁盘空间，代价是同一批不可能二次回放，只能依赖 `nbatch` 变化标记来决定哪些批必须重扫。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeNestloop.c` | `ExecNestLoop` | 双循环读外表、写 PARAM_EXEC、重扫内表并处理空值补齐与提前终止 |
| `src/backend/executor/nodeNestloop.c` | `ExecReScanNestLoop` | 只重扫 outer 并把状态重置为“需要新外表”，刻意不动 inner |
| `src/backend/executor/nodeMergejoin.c` | `ExecMergeJoin` | 推进 merge join 状态机、执行标记/回退与补空分支 |
| `src/backend/executor/nodeMergejoin.c` | `MJCompare` | 逐连接子句比较已预求值的排序键并处理 NULL 与常量假子句 |
| `src/backend/executor/nodeHashjoin.c` | `ExecHashJoin` | 并行无关的 hash join 状态机入口，按桶探测并维护补空与批次切换 |
| `src/backend/executor/nodeHashjoin.c` | `ExecHashJoinNewBatch` | 选择下一批、关闭旧批文件、重建哈希表并回绕外表文件 |

## 参考

- [PostgreSQL 文档：连接类型与连接计划](https://www.postgresql.org/docs/current/queries-table-expressions.html) —— 说明 inner/semi/anti/outer 连接语义，对应节点里的空值补齐与提前终止分支。
- [PostgreSQL 官方源码：Hash Join 实现](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeHashjoin.c) —— 支撑混合哈希连接、批次加倍与溢写退化路径的描述。
- [PostgreSQL 官方源码：Merge Join 状态机](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeMergejoin.c) —— 支撑标记/回退与多子句键比较流程。

## 与主核 / AI 的关系

暂无直接对应。主核若要实现连接算子，可复用“outer 驱动重扫 + 有序双路标记 + 哈希分批溢写”的骨架；但参数化键绑定、批次增长禁用条件和标记回退语义需要按主核的执行模型与内存预算重新设计。
