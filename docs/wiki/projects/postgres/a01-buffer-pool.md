---
project: postgres
feature: buffer-pool
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended, BufferAlloc, FlushBuffer]
  - path: src/backend/storage/buffer/freelist.c
    symbols: [StrategyGetBuffer, StrategySyncStart]
---

# 共享缓冲池与替换

## 一句话职责

共享缓冲池把关系页缓存为全实例可协作访问的共享帧，并在需要新帧时选择可复用的受害者。

## 机制是什么

PostgreSQL 的 shared buffers 是共享内存中的固定页帧集合，缓冲描述符记录页标识、引用计数、状态位及内容锁等管理状态。缓冲表负责将逻辑页身份映射到描述符。命中时增加 pin，避免该帧在使用期间被回收；内容一致性由 buffer content lock 保护，而 pin 与内容锁是不同约束。

未命中时 `BufferAlloc` 协调缓冲表映射和候选帧分配：候选帧必须可安全复用；旧页若脏，先写回，再更新页身份并安排读入。脏页可因替换而写出，也可由检查点等路径写出，缓冲池自身不定义持久化协议。该机制不负责事务可见性判断、WAL 语义或磁盘文件布局；它为上层提供页缓存与固定生命周期接口，下游由存储管理器执行物理 I/O。

## 怎么实现的

`ReadBufferExtended` 是通常的同步读入入口；它先处理特殊块号与本地临时关系分流，常规共享关系通过 `BufferAlloc` 查找或分配帧。缓冲表分区锁保护映射查找与更新；并发未命中时，先取得候选帧并在更新映射前再次检查，以免两个后端同时为同一逻辑页建立不同帧。若目标页的 I/O 正在进行，后到者会等待 I/O 完成，而批量/异步读路径由 `StartReadBuffers`、`WaitReadBuffers` 等步骤协调；页面有效后调用者取得 pin。

分配策略在 `freelist.c`：`StrategyGetBuffer` 先按访问策略 ring 尝试复用适合该扫描的帧；ring 可限制大扫描污染全局缓存的范围。不能从 ring 取得时，经共享 clock sweep 状态推进候选描述符。候选的 pin/锁状态若表明仍被使用则跳过；成功回收需原子地占有其状态并将其计数减至可用。它不是精确 LRU，避免每次访问都更新全局排序结构。

对永久关系的脏页，`FlushBuffer` 在读取页面 LSN 后先确保相应 WAL 刷到要求位置，再经存储管理器写出页面；unlogged 等非永久关系不适用这条 WAL-before-data 等待。成功写出后清除脏状态。`StrategySyncStart` 为 bgwriter 的同步扫描提供起始位置及近期分配计数，不意味着它通用地指挥检查点，也不承诺某一帧必然被回收。访问者释放 pin 后帧才可能成为替换候选；内容锁负责读写页内容的互斥。

## 关键取舍

采用固定共享帧和近似 clock sweep，换取多进程共享缓存及较低元数据维护成本，放弃严格按最近访问顺序淘汰。若换成精确 LRU，每次命中都要争用并更新全局队列，热点访问并发可能显著下降。访问策略 ring 只是一种缓冲池污染控制提示，不是独立缓存，也不改变页一致性或持久化语义。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/buffer/bufmgr.c` | `ReadBufferExtended` | 缓冲读取入口 |
| `src/backend/storage/buffer/bufmgr.c` | `BufferAlloc` | 命中查询及帧分配协调 |
| `src/backend/storage/buffer/bufmgr.c` | `FlushBuffer` | 脏页写出 |
| `src/backend/storage/buffer/freelist.c` | `StrategyGetBuffer` | ring 与 clock sweep 取候选 |
| `src/backend/storage/buffer/freelist.c` | `StrategySyncStart` | sweep 进度统计 |

## 参考

- [PostgreSQL 文档：资源消耗与共享缓冲区](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-SHARED-BUFFERS)
- [PostgreSQL 官方源码：shared buffer 与写回实现](https://github.com/postgres/postgres/blob/master/src/backend/storage/buffer/bufmgr.c)

## 与主核 / AI 的关系

共享缓冲池适合作为页缓存与并发 pin/内容锁机制的参照；不能直接等同于事务快照或数据库完整缓存策略。
