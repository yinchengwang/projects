---
project: postgres
feature: tidstore
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/common/tidstore.c
    symbols: [TidStoreCreateLocal, TidStoreCreateShared]
---

# TID 集合存储

## 一句话职责

`TidStore` 把按堆块分组的 tuple offset 暂存为可查找、可迭代的 TID 集合，并提供本地或并行共享的存储形态。

## 机制是什么

`tidstore.c` 用基数树以块号为键，每个 `BlocktableEntry` 保存该块的 offset 集合：少量 offset 内联放在 `full_offsets`，较多时转为 `words[]` 位图，`nwords` 标记有效位图长度。VACUUM 的 dead-item 收集是核心用途；消费端能按 TID 查询，或按块迭代并取回 offset 列表。

本页只讲集合的表示、内存形态和迭代，不负责判定 tuple 是否死亡、扫描堆页或删除索引项。串行 lazy VACUUM 用本地实例；并行 VACUUM 通过 DSA 中的共享基数树让 leader/worker 访问同一集合。`max_bytes` 用于限制分配块/段尺寸并供调用方监控，不是结构自身强制的硬上限。

## 怎么实现的

1. `TidStoreCreateLocal` 在当前 memory context 中分配外层 `TidStore`，另建子 context 承载本地 radix tree；`insert_only` 选择 bump 或 alloc-set context。`max_bytes` 缩小 context 的最大分配块尺寸，但不会拒绝超过该值的累积数据。
2. `TidStoreCreateShared` 建立 DSA area，在其中创建共享 radix tree；树的 handle 与 DSA handle 可交给 worker。`TidStoreAttach` 为进程创建本地外壳并附着到已存在的树，树控制块中的 LWLock 由 radix tree 提供。
3. `TidStoreSetBlockOffsets` 要求 offset 严格递增；它替换指定块原有条目，不执行增量添加/删除。数量不超过 `NUM_FULL_OFFSETS` 时写入 `full_offsets`，否则按 `BITS_PER_BITMAPWORD` 打包位图，非法 offset 触发 ERROR。
4. 对共享实例的访问由调用方明确围住共享锁：`TidStoreLockExclusive`、`TidStoreLockShare` 和 `TidStoreUnlock` 转到 radix tree 锁；本地实例这些包装不做锁操作。迭代期间调用方必须持锁直到 `TidStoreEndIterate`，避免内部页被并发改写或释放。
5. `TidStoreBeginIterate` 按本地/共享类型建立树迭代器；`TidStoreIterateNext` 返回可复制结果，其中包含块号及内部页引用；`TidStoreGetBlockOffsets` 把该块 offsets 展开到调用者数组。容量不足时会返回所需数量，调用方可据此准备更大的数组。
6. 释放时区分所有权：共享 owner 用 `TidStoreDestroy` 释放树与 DSA；其他 backend 先 `TidStoreDetach`，只拆除本地附着资源。owner 必须保证不再有进程访问后才能销毁。

## 关键取舍

- **每块保存稀疏 offset 或稠密位图，而非所有 TID 一条独立记录。** 源码事实：小集合内联进 header，较大集合才分配 `words[]`；`TidStoreSetBlockOffsets` 依赖排序输入并整块替换。放弃的是任意逐项插入/删除的便利，换取 VACUUM 按块批量写入、展开和紧凑存储；若统一用每 TID 节点，稀疏情形下树节点与分配器开销会更高。
- **共享锁责任放在 caller，不把每个 API 包装成自动加锁。** 源码事实：迭代文档明确要求调用方在整个迭代期间持锁，`vacuumparallel.c` 的 worker 将共享 TidStore 附着后参与 dead-item 处理。代价是调用协议更容易误用；自动逐调用加锁不能保证 `IterateNext` 返回的内部页引用在后续 `GetBlockOffsets` 使用时仍然稳定。此处属于基于接口生命周期的架构解释。
- **`max_bytes` 是分配粒度提示与统计界限配合，而非强制容量。** `vacuumlazy.c` 在收集后查看 `TidStoreMemoryUsage` 并批次处理/重置；如果树本身硬拒绝插入，VACUUM 就必须另行处理部分页 offset 的失败与恢复。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/common/tidstore.c` | `TidStoreCreateLocal` | 创建以 memory context 为所有者的本地树，并按 insert-only 选择内存分配器 |
| `src/backend/access/common/tidstore.c` | `TidStoreCreateShared` | 在 DSA 中创建供并行 backend 共享的树，并据 max_bytes 调整 segment 尺寸 |
| `src/backend/access/heap/vacuumlazy.c` | `dead_items_add`、`lazy_vacuum_heap_rel` | 将每页收集的 dead offsets 批量写入 TidStore，再按块迭代执行 heap 清理 |
| `src/backend/commands/vacuumparallel.c` | `parallel_vacuum_reset_dead_items` | 回收并重建共享 TidStore，同时更新供 worker attach 的 DSA/tree handles |

## 与主核 / AI 的关系

暂无直接对应。主核实现可借鉴“按块组织、稀疏/稠密表示切换”的临时集合设计；但 TidStore 保存的是 VACUUM 生命周期内的 TID 工作集，不是持久索引、事务可见性结论或数据页本身。

## 参考

- [PostgreSQL 官方源码：TidStore 实现](https://github.com/postgres/postgres/blob/master/src/backend/access/common/tidstore.c) —— 支撑 radix tree、块内 offset 表示与本地/共享生命周期
- [PostgreSQL 官方源码：lazy VACUUM](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/vacuumlazy.c) —— 支撑 dead-item 工作集的写入、内存监测、迭代和重置
