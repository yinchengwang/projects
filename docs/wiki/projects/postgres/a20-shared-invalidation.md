---
project: postgres
feature: shared-invalidation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/sinvaladt.c
    symbols: [SIInsertDataEntries, SIGetDataEntries, SharedInvalBackendInit]
---

# 共享失效消息传输原语

## 一句话职责

共享失效管理器在后端之间传递有界的失效消息流，并跟踪各接收者的读取进度。

## 机制是什么

`sinvaladt.c` 管理共享内存中的失效消息环形缓冲区、消息序号以及每个参与后端的读取游标。生产者写入消息并标记接收方有数据；后端按自身游标批量取出消息。空间回收依据参与回收计算的有效接收者进度：`sendOnly` 后端与已进入 reset 状态的后端不作为普通接收游标；reset 后端需先丢弃本地可失效状态，再重新同步。若环形缓冲区覆盖了其未读内容，后端会被标记为需要整体 reset。

本页只讨论传输原语和队列管理，不展开消息如何使 syscache/relcache 失效，也不重复缓存失效语义。消息传递不是可靠的无限队列：环容量固定，超前接收端可能丢失细粒度历史并被要求清空本地可失效状态。真正的失效解释与本地缓存动作属于上层消费者。

## 怎么实现的

共享段 `SISeg` 保存 `minMsgNum`、原子 `maxMsgNum`、固定容量环形消息数组，以及 `ProcState` 数组和活跃 ProcNumber 索引。全局消息号映射到槽位时取模。每个进程状态包含 PID、下条待读消息号、reset/signaled/hasMessages 标志和 sendOnly 属性；新后端通过 `SharedInvalBackendInit` 在写锁下登记，并从当前 `maxMsgNum` 起步，因此加入前的历史消息不需要补读。退出钩子则将槽标 inactive 并从密集索引中移除。

`SIInsertDataEntries` 将任意长度批次切成不超过 `WRITE_QUANTUM` 的片段，持有 `SInvalWriteLock` 串行写入。写入槽位后以带内存屏障的原子写发布新的 `maxMsgNum`，然后标记活跃接收者 `hasMessages`。`SIGetDataEntries` 先用该标记快速跳过空读，再在 `SInvalReadLock` 共享模式下只更新当前后端自己的游标并复制消息；共享模式读锁在这里允许多个后端并行读，但不代表任意代码可用它修改他人状态。

写者可与读者并行，因为写锁保护生产者及队列整理，读锁保护各自的消费状态。`SICleanupQueue` 在需要整理全局游标时升级为独占读锁，计算最慢接收端以推进 `minMsgNum` 并回收槽位。若队列压力上升，会对落后后端发送 `PROCSIG_CATCHUP_INTERRUPT`；无法追上的后端最终可能进入 reset 状态并被排除在最小游标之外，稍后取消息时收到 reset 返回值，必须自行丢弃不再可信的失效状态。

`maxMsgNum` 与消息内容的发布/读取有明确内存顺序要求：消息必须先可见，再让读者看到新上界。消息号接近回绕阈值时，所有相关计数同步减去与环长度整除的偏移，避免移动环内消息。队列管理只保证传输状态一致，不保证消费者已经执行了消息代表的语义动作。

## 关键取舍

固定容量环与每后端游标提供了低分配、低拷贝的广播传输；代价是慢消费者最终可能被 reset，而不能无限保留历史。若换成无界队列，慢后端会持续消耗内存；若每条消息单独投递给每个后端，写者成本会随参与者数量成倍增长。共享环把写入成本集中为一次，但需要复杂的最慢游标回收、追赶中断与整体重置恢复策略。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/sinvaladt.c` | `SharedInvalBackendInit` | 登记后端并初始化消费游标 |
| `src/backend/storage/ipc/sinvaladt.c` | `SIInsertDataEntries` | 分批写入环形消息并发布新上界 |
| `src/backend/storage/ipc/sinvaladt.c` | `SIGetDataEntries` | 读取当前后端尚未消费的消息 |
| `src/backend/storage/ipc/sinvaladt.c` | `SICleanupQueue` | 回收空间、追赶或 reset 落后进程 |

## 参考

- [PostgreSQL 官方文档：系统目录](https://www.postgresql.org/docs/current/catalogs.html)
- [PostgreSQL 源码 API 文档：共享失效队列管理](https://doxygen.postgresql.org/sinvaladt_8c.html)

## 与主核 / AI 的关系

主核用此原语传播需要后端响应的失效通知；传输队列负责哪些消息仍可读取以及何时要求 reset，上层消费者负责消息含义。AI 不应把“已经写入共享环”描述成“所有缓存已完成失效”，也不应把 reset 说成单条消息重放。
