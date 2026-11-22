---
project: postgres
feature: local-buffer-pool
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/buffer/localbuf.c
    symbols: [LocalBufferAlloc, FlushLocalBuffer]
---

# 后端本地缓冲池

## 一句话职责

本地缓冲池为单个后端服务临时关系页，避免将无需跨进程共享的缓存状态放入共享池。

## 机制是什么

临时关系由后端本地缓冲管理。源码中的 LocalBuffer 描述符、页内存与映射表属于当前后端；缓冲编号位于本地命名空间，不是 shared buffer ID。命中时增加本地引用计数；缺页时选择本后端可复用的帧，脏页必要时写回后再换入目标页。由于这些结构不共享，后端之间无需为 LocalBuffer 获取共享缓冲表分区锁，也不会共享同一份临时页缓存。这个本地缓冲层只管理页缓存与写回，不覆盖临时表 SQL 生命周期、事务可见性或永久关系缓存。

## 怎么实现的

`LocalBufferAlloc` 以关系存储标识、fork 和块号查找本地映射。命中时增加本地引用计数并返回对应 buffer；缺页时按本后端可用的本地帧策略选取帧，若旧帧脏则调用 `FlushLocalBuffer` 写回，再替换映射并读入目标块。由于数据结构不共享，查找与替换不需要共享缓冲表分区锁；本地状态仍需遵守本地 pin 和脏页生命周期，避免正在使用的页被回收。

临时关系不参与永久关系的 checkpoint/WAL 持久化路径；这正是可简化同步成本的前提，而非通用“无需落盘”。临时页在需要时仍可能通过存储管理器读写临时文件。后端结束时其临时对象及本地缓存按各自清理生命周期回收。

## 关键取舍

- **后端私有而非共享缓存**：以每后端独立占用内存换取无跨进程 LocalBuffer 锁竞争；代价是同一临时页不会在后端间共享，内存预算随并发会话增加。
- **本地 Clock sweep 而非精确 LRU**：本地帧回收可用引用/使用计数近似老化，避免每次命中维护完整全序；若改用精确 LRU，淘汰顺序更贴近最近访问，但热路径需要更多链表更新。
- **临时关系页仍可写到临时文件**：本地缓冲不等于纯内存表；需要时可将页写回临时存储。其 I/O 与永久关系的 WAL 持久化要求不同；不能把“临时”误解为“永不落盘”。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/buffer/localbuf.c` | `LocalBufferAlloc` | 本地缓冲查找、分配与读入 |
| `src/backend/storage/buffer/localbuf.c` | `FlushLocalBuffer` | 本地脏页写回 |

## 参考

- [PostgreSQL 文档：临时表与 ON COMMIT 生命周期](https://www.postgresql.org/docs/current/sql-createtable.html)
- [PostgreSQL 文档：临时缓冲区配置](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-TEMP-BUFFERS)
- [PostgreSQL 源码：localbuf.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/buffer/localbuf.c)

## 与主核 / AI 的关系

临时页缓存可用于对照区分“共享数据缓存”和“会话局部工作集”，但不应直接推导其他引擎的临时数据持久性。
