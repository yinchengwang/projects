---
project: postgres
feature: aggregation-and-window
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeAgg.c
    symbols: [ExecAgg, build_hash_table, hash_agg_enter_spill_mode]
  - path: src/backend/executor/nodeWindowAgg.c
    symbols: [ExecWindowAgg, update_frameheadpos]
---

# 聚合与窗口

## 一句话职责

Agg 把输入行折叠为分组结果，WindowAgg 则保留逐行输出并按分区、排序和帧边界计算窗口值。

## 机制是什么

聚合节点按 `Agg` 策略执行 plain、sorted、hashed 或 mixed 分组；每个聚合函数的 transition state 在输入行到达时推进，组结束后再 finalize 与投影。Hash Agg 以分组键查 `TupleHashTable`，内存预算逼近限制时停止新建组，将后续新组输入分区写入逻辑磁带，逐批重建 hash table 并发出结果。

窗口节点要求输入已按分区键和 order key 排列，内部以 tuplestore 缓存当前分区，使函数可读取当前行之前/之后的行并访问 frame/group 边界。不同于 Agg，它不折叠行；每个输入行仍对应一个输出行。两者都不负责规划期的算子选择或排序保证生成，WindowAgg 也不负责根据 SQL 重新排序输入；上游计划必须兑现所需顺序。

## 怎么实现的

1. `ExecAgg` 按 `aggstrategy` 分派：`AGG_PLAIN/AGG_SORTED` 走 `agg_retrieve_direct`，`AGG_HASHED` 首先调用 `agg_fill_hash_table` 后走 hash table 输出，`AGG_MIXED` 同时按 phase 处理排序与哈希 grouping sets。
2. direct 路径按排序键比较相邻输入，遇到新组时 finalize 前一组并初始化下一组的 `AggStatePerGroupData`；plain 聚合只维护单组。每条输入按聚合 transition function 更新状态，组完成时调用 final/serialize 相关逻辑并由 projection 产出一行。
3. Hash Agg 的 `build_hash_table` 为每个 grouping set 创建 `TupleHashTable`，另配 metacxt、tuple context 和 per-tuple 临时 context。`agg_fill_hash_table` 对每条 child tuple 调 `lookup_hash_entries` 找组/建组，再 `advance_aggregates` 更新 transition state。
4. `hash_agg_check_limits` 只有在当前已至少有一个 hash group 后，且总内存超过 `hash_mem_limit` 或组数超过 `hash_ngroups_limit`，才进入 spill mode，确保极端情况仍能在内存中推进一个组。`hash_agg_enter_spill_mode` 设置 `hash_spill_mode`、重编表达式并创建 `LogicalTapeSet` 与每个 hash set 的 `HashAggSpill`。
5. spill mode 里已有 hash group 仍留在表中并继续累计；会新建组的 tuple 按 hash bits 写入 partition tape。内存表发完所有组后，`agg_refill_hash_table` 取一个 spilled batch，清空/重置表并重放其 tape，再逐组 finalize；这把同一分组的行聚到同一批次，而不要求整个输入表驻留内存。
6. `ExecWindowAgg` 首次调用时计算并固定 frame offsets，然后 `begin_partition` 初始化 partition 状态；`spool_tuples` 持续从 child 读入当前分区至当前行或边界所需位置，将行复制进 tuplestore，避免 child slot 被后续读取覆盖。
7. 对当前窗口行，节点从 `current_ptr` 读回 scan slot，更新 peer group 状态，执行普通 window function 与 window aggregate；前者按函数 API 读取 partition/frame，后者按 frame 中行推进 transition state。`update_frameheadpos` 按 ROWS 的位置偏移、RANGE 的排序键 in_range 比较或 GROUPS 的 peer group 边界推进独立 read pointer。
8. 每行处理后更新 frame/group auxiliary pointers 并 `tuplestore_trim` 释放不再需要的旧行；当分区到 EOF 就 `release_partition`，接着启动下一分区。runCondition 失败时可结束顶层窗口，也可在分区/上层节点仍需这些行时进入 pass-through 状态。

## 关键取舍

- 源码事实：Hash Agg 触发溢写后仍保留已存在的组，仅把会创建新组的 tuple 写到 spill partitions。若把全部输入都立刻写出，会重复写入已经有 transition state 的组；继续无界增加内存表则会突破 `hash_mem_limit`。代价是查询必须在内存阶段完成后重读分区，并可能递归产生后续批次。
- 源码事实：Hash Agg 只有在 hash table 已有至少一个 group 时才进入 spill mode，避免首个超大 group 无法被容纳时不断产生“零内存进展”。因此极端单组状态可能超过估算预算；这是以可终止性换取严格内存上限的退让。
- 源码事实：WindowAgg 需要 tuplestore 和多个读指针，让不同函数/边界各自前进；若只保留当前行会无法满足 FOLLOWING、GROUPS/RANGE peer 边界或窗口函数前瞻访问。代价是要缓存分区数据；`tuplestore_trim` 只在所有相关读指针都越过后才能回收。
- 源码事实：`update_frameheadpos` 对 ROWS 偏移可直接算位置，对 RANGE/GROUPS 则逐行比较 peer 或 order 值并可调用 `spool_tuples` 继续取数。保守地让复杂边界使用读指针推进，避免假设帧头只依赖行号；代价是某些窗口可能必须先物化较长前缀甚至整分区。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeAgg.c` | `ExecAgg` | 按 direct/hash/mixed 策略分派输入聚合与分组结果读取 |
| `src/backend/executor/nodeAgg.c` | `build_hash_table` | 为某个 grouping set 建立带聚合附加状态的 tuple hash table |
| `src/backend/executor/nodeAgg.c` | `hash_agg_enter_spill_mode` | 初始化 tape partitions 并切换到不再创建新 hash group 的模式 |
| `src/backend/executor/nodeWindowAgg.c` | `ExecWindowAgg` | 逐行推进分区 spool、窗口函数、聚合和输出流程 |
| `src/backend/executor/nodeWindowAgg.c` | `update_frameheadpos` | 根据 frame mode 和当前行位置更新 frame head 读指针 |

## 参考

- [PostgreSQL 文档：聚合函数](https://www.postgresql.org/docs/current/functions-aggregate.html) —— 说明聚合函数的分组结果语义。
- [PostgreSQL 文档：窗口函数教程](https://www.postgresql.org/docs/current/tutorial-window.html) —— 说明窗口函数对当前行、分区和排序的语义。
- [PostgreSQL 官方源码：Agg 执行节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeAgg.c) —— 支撑 Agg 状态推进、哈希分组和 spill mode 的流程。
- [PostgreSQL 官方源码：WindowAgg 执行节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeWindowAgg.c) —— 支撑 partition spool 与 frame pointer 推进细节。

## 与主核 / AI 的关系

暂无直接对应。主核若实现聚合/窗口，可借鉴“transition state + group lifecycle”与“分区 spool + 多读指针”的组合；哈希溢写和窗口缓存策略需按主核资源预算与函数调用接口单独实现。
