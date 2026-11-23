---
project: postgres
feature: heap-page-layout
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/page/bufpage.c
    symbols: [PageInit, PageAddItemExtended]
  - path: src/backend/access/heap/hio.c
    symbols: [RelationPutHeapTuple, RelationGetBufferForTuple]
---

# Heap 页与 line pointer

## 一句话职责

标准 page 层维护 header、line pointer、反向增长的 item 区和可用空间边界，使 TID 能以稳定槽号定位页内 item。

## 机制是什么

`PageHeaderData` 的 `pd_lower` 标出 header 与 line pointer array 的下界，`pd_upper` 是 item 存储区的上界，`pd_special` 划分 access method 专用尾部区域。`ItemIdData` 是 line pointer：TID 内的 offset 是其 1-based 槽号，槽记录 item 的字节偏移、长度与状态；因此 tuple 搬移后只要更新 ItemId 的位置，外部 TID 槽号仍可保持稳定。

本页只讲页内分配、定位与整理，不讲 tuple header 中 xmin/xmax、属性 null bitmap、字段对齐等 tuple 自身布局（那属于 c06）。通用 page API 不判断 MVCC 可见性，也不决定 heap 写入策略；heap I/O 层选择目标 buffer 并持排他锁后调用 page API 插入。空闲区是 `pd_lower` 与 `pd_upper` 间的当前连续区域，页上有总空闲但存在碎片时需整理后才可形成大块连续空间。

## 怎么实现的

1. `PageInit` 清零整页，按 special space 大小初始化 `pd_lower` 至 header 末端、`pd_upper`/`pd_special` 至尾部边界，并写入 page size 与 layout version。新页的 line pointer array 与 item 区因此从相反方向扩展。
2. `PageAddItemExtended` 先验证三条边界指针合法，再决定槽位：有指定 offset 时可覆盖未使用槽或在中间插入并移动 line pointer；未指定时优先扫描 free-line hint 找可复用的 unused 槽，否则追加新槽。
3. 插入路径计算 `MAXALIGN(size)` 与新增 ItemId 所需 lower 增量；若 `lower > upper` 返回 InvalidOffsetNumber。成功时设置 line pointer 为 normal、复制 item 到页尾方向的空闲区，并更新 `pd_lower`/`pd_upper`。heap flag 还限制可分配 line pointer 数。
4. 页被选择用于 heap 插入前，`RelationGetBufferForTuple` 优先复用 backend target block，再向 FSM 要候选；实际读入并取得 buffer exclusive lock 后重新计算 `PageGetHeapFreeSpace`。候选过时就记录真实 free space、再查 FSM，最终无可用页时扩展关系。
5. `RelationPutHeapTuple` 要求 caller 已持 buffer 排他锁，调用 `PageAddItem` 将 tuple 放进页，得到实际 offset 后回填 tuple 的 `t_self` 与页内 `t_ctid`（speculative token 除外）。插入失败在此路径是 PANIC，因为空间已在前置选择时验证过。
6. prune 后 `PageRepairFragmentation` 收集仍占存储的 ItemId，压缩 item bytes 并改写其偏移，保留逻辑槽号；尾部 unused line pointer 可截短以降低 array 占用。它要求 cleanup lock，避免并发访问时移动正在使用的 item。

## 关键取舍

- **TID 指向 line-pointer 槽，而不是 item 的字节地址。** 这多占一层槽表与间接寻址，但允许页面整理移动 tuple bytes 而不改变现存 TID 的槽身份；直接存 byte offset 的 TID 会在紧凑化后失效或需要全库索引重写。
- **空闲空间边界分开维护，item bytes 从页尾向前增长。** 新增槽扩展 lower，新增 item 下移 upper，只有两者之间仍有空间才能插入。放弃单向 append-only 布局，换取槽复用和 page 内压缩；代价是碎片整理与指针一致性校验更复杂。
- **page 层只报告插入失败，heap I/O 层负责候选验证与重试。** `RelationGetBufferForTuple` 在持 buffer 排他锁后重验空间，FSM 候选不足会修正记录并继续找页；`RelationPutHeapTuple` 则把失败视为不变量破坏而 PANIC。这样的分层避免通用 page primitive 依赖 relation/FSM 策略；反之若 PageAddItem 自己查找/扩展关系，就会把页格式 API 与锁、I/O、FSM 生命周期耦合。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/storage/page/bufpage.c` | `PageInit` | 清零 page 并初始化三条边界、页尺寸和格式版本 |
| `src/backend/storage/page/bufpage.c` | `PageAddItemExtended` | 选择/复用 line pointer，检测连续空间并写入 item、推进边界 |
| `src/backend/storage/page/bufpage.c` | `PageRepairFragmentation` | 在 cleanup lock 前提下压缩 item 空隙并更新 line pointer offsets |
| `src/backend/access/heap/hio.c` | `RelationPutHeapTuple` | 在已排他锁定的页上调用 PageAddItem，并写回实际 TID |
| `src/backend/access/heap/hio.c` | `RelationGetBufferForTuple` | 借助 target block/FSM 选页、锁页后核实空间并决定重试或扩展 |
| `src/include/storage/bufpage.h` | `PageHeaderData` | 定义 header 边界字段与 line pointer array 的页内起始处 |
| `src/include/storage/itemid.h` | `ItemIdData` | 定义槽中保存的 item offset、长度与 UNUSED/NORMAL/REDIRECT/DEAD 状态 |

## 与主核 / AI 的关系

暂无直接对应。页布局说明的是存储页内部寻址层，不能由 tuple schema 或行对象结构推导；分析 TID 时需同时区分 block number 与 line-pointer offset。

## 参考

- [PostgreSQL 官方源码：标准页操作](https://github.com/postgres/postgres/blob/master/src/backend/storage/page/bufpage.c) —— 支撑初始化、插入与碎片整理算法
- [PostgreSQL 官方源码：page header 定义](https://github.com/postgres/postgres/blob/master/src/include/storage/bufpage.h) —— 支撑 page boundary、header 与 line pointer array 语义
- [PostgreSQL 官方源码：heap I/O](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/hio.c) —— 支撑页选择、锁定和 tuple 插入的调用关系
