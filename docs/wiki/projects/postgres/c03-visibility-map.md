---
project: postgres
feature: visibility-map
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/heap/visibilitymap.c
    symbols: [visibilitymap_set, visibilitymap_clear, visibilitymap_pin]
---

# Visibility Map

## 一句话职责

Visibility Map 为每个堆页保存保守的全可见与全冻结摘要，供 VACUUM 和 index-only scan 决定哪些堆页可免于检查。

## 机制是什么

VM 位于关系独立的 visibility map fork，按堆块号映射到 VM 页中的紧凑位对。`VISIBILITYMAP_ALL_VISIBLE` 表示该堆页元组已知对所有事务可见，`VISIBILITYMAP_ALL_FROZEN` 表示其中元组均已冻结；all-frozen 必须同时满足 all-visible。位清零只表示未知或不再保证，不证明页上必然有不可见元组，因此它是“设位可信、清位不作结论”的保守摘要。

VM 本身不执行 MVCC 可见性判定，也不替代堆页的 `PD_ALL_VISIBLE` 提示。index-only scan 可根据 VM 信号跳过 heap fetch；VACUUM 可据此跳页或记录冻结进度。位更新必须与改变堆页事实的操作相协调：设位要在堆页锁保护下与相应 heap/WAL 修改同一关键区间完成，清位则必须确保该更新操作的 WAL 重放也会清掉 VM 位。

## 怎么实现的

1. `HEAPBLK_TO_MAPBLOCK`、`HEAPBLK_TO_MAPBYTE`、`HEAPBLK_TO_OFFSET` 将堆块号转成 VM fork 的页、字节与位对位置；每页摘要由两个状态位组成。VM 页直接以 `PageGetContents` 存位图，不建立普通 line pointer 项。
2. 修改者先呼叫 `visibilitymap_pin` 取得覆盖目标 heap block 的 VM buffer pin；如果原 pin 已指向同一 VM 页就复用，否则释放旧 pin 并读取/必要时扩展 fork。读取用 `RBM_ZERO_ON_ERROR`，坏页可安全归零，因为清除摘要比错误地保留设位安全。
3. `visibilitymap_pin` 不要求持有堆页锁，因为读取 VM 页可能触发 I/O。调用者随后锁住堆 buffer 并确认页状态；`visibilitymap_set` 需要 VM buffer 已 pin 且排他锁定，调用方同时持有堆页 pin/排他锁，并在关键区段中更新摘要与写 WAL。
4. 清位路径要求传入正确且排他锁定的 VM buffer，不做 I/O；`visibilitymap_clear` 对请求的状态位做按位清除并标记 VM buffer dirty。清除 all-visible 时必须同时清 all-frozen，避免留下非法状态。heap 插入/更新/删除等操作把 VM 清除与主页变更纳入同一 WAL 语义。
5. `visibilitymap_get_status` 为读取复用正确 pin 或读取已有 VM 页，单字节读取不加 VM 页锁，结果可能短暂陈旧；一致性依赖调用方与 heap/index 页的锁顺序。VM fork 扩页则在 buffer 管理路径创建零页并广播 smgr cache invalidation。

## 关键取舍

- 源码事实：VM 只保证 set bit 为真，clear bit 允许“实际全可见但摘要未知”；损坏 VM 页读取时采用 `ZERO_ON_ERROR`。架构推论：以可能多做 heap I/O 换取不因过时或损坏摘要漏读、不正确跳过；若把清位解释为精确“不可见”，就需要每次改动都维护更昂贵的反向事实。
- 源码事实：pin VM 页与锁堆页分两阶段，`visibilitymap_set` 要求两边相关保护并进入 WAL critical section；读取/扩展 VM 可能 I/O，故不能持堆页锁做第一步。架构推论：此顺序在避免锁跨 I/O 与维护原子语义之间折中；反过来持 heap lock 再取 VM 页可能造成长时间锁占用及锁序风险，拆开设位则会出现摘要与页事实不匹配的窗口。
- 源码事实：`visibilitymap_get_status` 不锁 VM 页，允许读到瞬时陈旧位；源码对 index-only scan 的安全性依赖插入先清 VM、再发布 index TID 的锁同步，以及删除事务提交与快照建立的屏障。架构推论：无锁读降低 VM 热页竞争，但仅在调用方遵守这些写入顺序时成立；若其他路径绕开排序，读到旧 all-visible 就可能跳过必要 heap fetch。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/heap/visibilitymap.c` | `visibilitymap_set` | 在调用方已 pin/排他锁住 VM 页且持有堆页保护时设置摘要位，并返回修改前状态。 |
| `src/backend/access/heap/visibilitymap.c` | `visibilitymap_clear` | 对既有 VM buffer 清指定摘要位，不做 I/O，供改变堆页可见性事实的写入路径同步撤销保证。 |
| `src/backend/access/heap/visibilitymap.c` | `visibilitymap_pin` | 取到覆盖目标 heap block 的 VM 页 pin，允许同页复用并在跨 VM 页时释放旧 pin。 |

## 与主核 / AI 的关系

主核没有按 heap block 记录 all-visible/all-frozen 位的直接同构结构。可以借鉴 VM 将昂贵检查结果做成保守摘要、读取时只以“可信置位”跳过工作；不能把该提示类比为主核的事务快照、对象可见性或持久化状态。

## 参考

- [PostgreSQL 文档：Visibility Map](https://www.postgresql.org/docs/current/storage-vm.html) —— 说明 VM 的 all-visible/all-frozen 位与其在 index-only scan 中的用途。
- [PostgreSQL 官方源码：Visibility Map 实现](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/visibilitymap.c) —— 支撑位图映射、pin/锁顺序与 WAL 协作事实。
