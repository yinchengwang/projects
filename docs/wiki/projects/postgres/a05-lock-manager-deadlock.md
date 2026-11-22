---
project: postgres
feature: lock-manager-deadlock
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/lmgr/lock.c
    symbols: [LockAcquireExtended, LockRelease]
  - path: src/backend/storage/lmgr/lmgr.c
    symbols: [LockRelationOid, UnlockRelationOid]
  - path: src/backend/storage/lmgr/deadlock.c
    symbols: [DeadLockCheck]
---

# 常规锁与死锁检测

## 一句话职责

常规锁管理器协调事务对数据库对象的互斥访问，并在等待关系形成环时检测和打破死锁。

## 机制是什么

锁管理器以 LockTag 标识锁对象，以锁模式及冲突矩阵判定请求能否立即授予；同一事务可持有对象锁，并在冲突时进入等待队列。关系级 SQL 锁由 `lmgr.c` 包装调用底层锁接口。它不同于 buffer content lock、轻量锁以及 SSI SIREAD 谓词锁：这里主要管理事务级对象锁及其等待，不保护单个页内存操作，也不检测 SSI 的 rw 依赖。

## 怎么实现的

`LockAcquireExtended` 根据 LockTag 在共享锁表定位或建立 LOCK 项，并按进程本地持有状态与共享 LOCK/PROCLOCK 状态处理重复请求、模式计数和授予。若现有持有模式与请求模式冲突，请求者进入等待队列并通过进程等待机制睡眠；释放或授予变更会唤醒可运行等待者。快速路径用于常见局部锁持有，必要时再转入共享哈希表，降低热点锁表竞争。

关系操作入口 `LockRelationOid` 将 OID 和模式转换为关系锁请求；`UnlockRelationOid` 释放对应持有。锁的持有期限依锁类别及事务/会话语义决定，不应把所有锁都简化成语句结束自动释放。

等待期间触发死锁检查。`DeadLockCheck` 基于等待图搜索环；若能通过重排等待队列解除环则尝试调整，否则选择当前事务作为牺牲者并报告死锁，使其回滚释放锁。检测不是每次加锁都全局扫描：只对等待及检查时机运行，以控制成本。检测最终只解决锁等待图中的环，不涵盖应用层逻辑循环或 SSI 冲突图。

## 关键取舍

采用冲突矩阵和等待队列实现共享对象锁，允许冲突事务等待，保持传统锁语义。若所有冲突都立即失败，重试量会增加；若永不检测等待环，参与死锁的事务可能永久等待。按需死锁搜索而非每次锁操作扫描全图，降低常态成本，代价是死锁持续到检测触发。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/lmgr/lock.c` | `LockAcquireExtended` | 申请、授予或排队 |
| `src/backend/storage/lmgr/lock.c` | `LockRelease` | 释放锁与唤醒等待者 |
| `src/backend/storage/lmgr/lmgr.c` | `LockRelationOid` | 关系锁上层入口 |
| `src/backend/storage/lmgr/lmgr.c` | `UnlockRelationOid` | 关系锁释放入口 |
| `src/backend/storage/lmgr/deadlock.c` | `DeadLockCheck` | 等待图死锁检测与修复 |

## 参考

- [PostgreSQL 文档：显式锁定](https://www.postgresql.org/docs/current/explicit-locking.html)
- [PostgreSQL 文档：锁管理器监控视图](https://www.postgresql.org/docs/current/monitoring-locks.html)

## 与主核 / AI 的关系

常规锁等待图与 SSI 依赖图是不同机制；诊断时可用锁视图观察等待，但不能据此推断所有事务级冲突。
