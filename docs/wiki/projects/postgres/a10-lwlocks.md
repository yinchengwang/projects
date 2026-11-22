---
project: postgres
feature: lwlocks
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/lmgr/lwlock.c
    symbols: [LWLockAcquire, LWLockRelease, LWLockInitialize]
---

# 轻量锁 LWLock

## 一句话职责

LWLock 为共享内存中的短临界区提供共享/排他同步，并在竞争时协调等待者入队与唤醒。

## 机制是什么

LWLock 针对数据库内部共享结构的并发保护，支持共享模式与排他模式，使用原子状态和等待队列控制竞争。无竞争时尽量通过原子操作快速取得；竞争时等待者登记并睡眠，释放者按状态唤醒适当等待者。

它不是 SQL 层表锁，也不维护事务锁兼容矩阵；后者由常规 lock manager 提供，并依赖 LWLock 保护自己的共享状态。LWLock 也不是条件变量或自旋锁的通用替代品：它有特定的共享/排他持有规则、等待队列和 tranche 诊断归类。短临界区是预期用途，不能在持锁期间执行可能长期阻塞的工作。

## 怎么实现的

`LWLockInitialize` 初始化锁状态、等待队列和 tranche 标识。获取时 `LWLockAcquire` 先尝试原子修改状态：共享模式在无排他者且未被排他等待策略阻挡时增加共享持有计数；排他模式要求当前锁无持有者。失败后，等待者在保护队列的自旋锁下登记自己的 PGPROC 状态与请求模式，随后释放队列保护并睡眠。

释放者 `LWLockRelease` 原子更新持有状态；当锁达到可唤醒条件时，操作等待队列并唤醒符合策略的进程。被唤醒者仍需重新竞争并验证状态，而不是把唤醒当成锁已授予。共享/排他冲突与等待者排队共同避免同时进入不兼容临界区；自旋锁只保护短暂的队列及状态转换，并非持有整个用户临界区。

锁可附带 tranche 用于统计与诊断；共享内存规划者负责分配锁结构及初始化。锁持有者通常需在错误路径释放锁，某些 API 维护本地持锁追踪以支持检查。LWLock 不自动检测所有业务级死锁，也不回滚临界区内已修改的数据。

## 关键取舍

- 快速路径使用原子操作而非每次都进通用锁管理器，降低内部共享结构访问开销；代价是状态机和内存序协议复杂。
- 竞争时排队睡眠而不是无限自旋，避免高竞争下浪费 CPU；低竞争场景则通过快速路径避免系统调用成本。
- 以有限模式（共享/排他）换取较小同步原语；若改用通用事务锁，内部热点会承担更多元数据与兼容检查开销；若只用自旋锁，持锁临界区稍长时会消耗 CPU。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/lmgr/lwlock.c` | `LWLockInitialize` | 初始化锁状态及 tranche |
| `src/backend/storage/lmgr/lwlock.c` | `LWLockAcquire` | 获取共享或排他锁，必要时排队等待 |
| `src/backend/storage/lmgr/lwlock.c` | `LWLockRelease` | 释放锁并在适当时唤醒等待者 |
| `src/include/storage/lwlock.h` | `LWLock` | 锁结构与接口声明 |

## 参考

- [PostgreSQL 官方文档：锁与并发控制](https://www.postgresql.org/docs/current/explicit-locking.html)
- [PostgreSQL 官方文档：LWLock 等待事件](https://www.postgresql.org/docs/current/monitoring-stats.html#WAIT-EVENT-TABLE)
- [PostgreSQL 源码：lwlock.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/lmgr/lwlock.c)

## 与主核 / AI 的关系

多模态主核中共享目录、缓冲池元数据等短临界区可采用类似的读共享/写排他锁；需区分管理锁与数据锁、避免长时间持锁，也要明确异常释放和等待公平性。该原语不限制 AI 算法，但可用于保护其共享索引元数据。
