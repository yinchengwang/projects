---
project: postgres
feature: sort-and-materialization
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/nodeSort.c
    symbols: [ExecSort, ExecInitSort]
  - path: src/backend/executor/nodeIncrementalSort.c
    symbols: [ExecIncrementalSort]
  - path: src/backend/executor/nodeMaterial.c
    symbols: [ExecMaterial, ExecMaterialMarkPos]
---

# 排序与物化节点

## 一句话职责

Sort 与 IncrementalSort 把子计划元组组织为所需顺序，Material 则缓存元组流以支持回退、重扫或标记恢复。

## 机制是什么

`Sort` 节点负责把 child 元组完整交给 `tuplesort` 后再按执行方向读出；它定义执行器何时初始化、填充、完成和重扫排序状态，不负责外部排序的 run 生成与归并算法。`IncrementalSort` 假设 child 已有排序键前缀，只对同一前缀组内未排序后缀排序，并以混合状态机平衡小组启动开销与大组排序成本。

`Material` 采用惰性 tuplestore：顺序向前且游标到达缓存末尾时继续从 child 读取并缓存；只有回退、重扫或 mark/restore 需求出现时才从 store 回读。它不计算排序键，也不选择算法或分配永久结果集；超内存缓存的存储策略由 tuplestore 承接。`Memoize` 虽在本组范围里是相关执行状态，但采用参数键 hash cache/LRU、容量不足驱逐且不 spill；本页将它与 Material 的顺序缓存区分。

## 怎么实现的

1. `ExecInitSort` 由 eflags 决定 `randomAccess`：若调用方可能 rewind/backward/mark，就要求随机访问输出；同时屏蔽传给 child 的这些标记，因为 Sort 会替 child 吸收回退需求。单列单键可走 Datum sort，其余 tuple sort。
2. `ExecSort` 首次调用把 child 强制设为 forward，按 Datum 或 tuple 逐个送入 `tuplesort_putdatum/tuplesort_puttupleslot`，结束时 `tuplesort_performsort` 并恢复原方向；`sort_Done` 置位后每次调用只从 tuplesort 取一个 slot。bounded 与 randomAccess 选项由节点状态传给排序实现。
3. IncrementalSort 状态有 `INCSORT_LOADFULLSORT/READFULLSORT/LOADPREFIXSORT/READPREFIXSORT` 等阶段。初始 fullsort 先累积至少 `DEFAULT_MIN_GROUP_SIZE`（源码常量 32）条，不在小样本上反复比较前缀；随后识别相同 presorted prefix 的组，组内用 `prefixsort_state` 只排剩余 key。若预读样本足以认为较大组用 prefix sort 划算，就切换，否则对收集的批次按全部 key fullsort。
4. 切换模式时 `group_pivot` 暂存跨批边界的第一条下一组 tuple，避免消费一次就丢失；`fullsort_state` 和 `prefixsort_state` 复用并 reset，每个批次完成时记录独立 instrumentation。bounded LIMIT 会把剩余 bound 传入 tuplesort，并相应调低需要探测的最小组规模。
5. `ExecMaterial` 在首次执行且 `eflags != 0` 时按需创建 `tuplestore_begin_heap`；`EXEC_FLAG_MARK` 会分配第二 read pointer。到 store EOF 时才调用 child，把 child slot 存入 store 后返回；反向读则从 store 读取，不会重新向前 child 拉取。
6. `ExecMaterialMarkPos` 把活动 read pointer 0 复制到 mark pointer 1，并尝试 `tuplestore_trim`；`ExecMaterialRestrPos` 反向复制 mark 到 active。`ExecReScanMaterial` 若 child 参数没变且 store 支持 rewind，可直接 `tuplestore_rescan`；child `chgParam` 改变或未请求 rewind 时清空旧 store、重扫并重新物化。
7. 对照 Memoize，`nodeMemoize.c` 中 cache key 命中回放对应参数结果，未命中执行 child 并记录结果；缓存填满时 LRU 驱逐而非写临时文件。半途未读完的条目默认不完整，semi/unique caller 可通过 single-row 标记使其提前完整；单次 scan 本身无法再容纳新 tuple 时该 entry 转 bypass，下一次 rescan 才重新尝试缓存。

## 关键取舍

- 源码事实：Sort 把 backward/mark/rewind 能力集中在排序结果上，child 不必支持这些能力；代价是第一次返回前需消费全部 child 元组并完成排序。若流式排序边读边出，不能保证后续更小键值不破坏已输出顺序。
- 源码事实：IncrementalSort 对小组先使用 fullsort 再决定是否按前缀分组；源码以 `DEFAULT_MIN_GROUP_SIZE` 过滤过多微小 group sort 的开销。全部输入统一做整表 Sort 会失去早期返回和局部内存优势；无条件给每组单独 sort 又可能让许多小组启动开销高于排序本身。
- 源码事实：Material 只在游标回读时使用已缓存数据，在纯 forward-only 情况下可不创建 tuplestore；需要反向/mark 才物化并维护 read pointer。始终物化会增加内存/临时文件写入，而从不物化则不能满足 rescan/restore。
- 源码事实：Memoize 的策略与 Material 不同，缓存用完时驱逐 LRU，不把旧参数结果写磁盘；若当前填充 entry 本身超预算，则丢弃它并进入 bypass。源码注释指出这是为了避免没有可驱逐空间时继续保存无收益数据，代价是该参数后续重复出现仍要重新执行 child。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/nodeSort.c` | `ExecSort` | 首次填充并完成排序，之后按方向逐项读取结果 |
| `src/backend/executor/nodeSort.c` | `ExecInitSort` | 根据调用方 eflags 配置随机访问能力并约束 child 的执行标记 |
| `src/backend/executor/nodeIncrementalSort.c` | `ExecIncrementalSort` | 在 fullsort 与 prefix-group sort 间切换并逐批输出 |
| `src/backend/executor/nodeMaterial.c` | `ExecMaterial` | 在 child EOF 边界惰性写入 tuplestore 并提供重读结果 |
| `src/backend/executor/nodeMaterial.c` | `ExecMaterialMarkPos` | 保存活动读指针位置并尝试回收 mark 之前的存储 |

## 参考

- [PostgreSQL 文档：排序输出](https://www.postgresql.org/docs/current/queries-order.html) —— 说明查询排序结果语义。
- [PostgreSQL 文档：EXPLAIN 与执行计划节点](https://www.postgresql.org/docs/current/using-explain.html) —— 介绍 Sort、Materialize 等执行计划节点。
- [PostgreSQL 官方源码：Sort 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeSort.c) —— 支撑节点初始化、child 读取及排序结果输出流程。
- [PostgreSQL 官方源码：IncrementalSort 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeIncrementalSort.c) —— 支撑 fullsort/prefixsort 状态切换。
- [PostgreSQL 官方源码：Material 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeMaterial.c) —— 支撑 tuplestore 惰性填充与位置标记。
- [PostgreSQL 官方源码：Memoize 节点](https://github.com/postgres/postgres/blob/master/src/backend/executor/nodeMemoize.c) —— 支撑参数缓存、LRU 驱逐与 bypass 语义。

## 与主核 / AI 的关系

暂无直接对应。主核可区分“顺序重读的 Material”“有序组织的 Sort”和“按参数复用的 Memoize”，避免把三类缓存统一成不区分访问模式的存储层。
