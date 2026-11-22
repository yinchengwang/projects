---
project: postgres
feature: backend-proc-slots
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/lmgr/proc.c
    symbols: [InitProcess, InitProcessPhase2, ProcSleep]
---

# 后端进程槽

## 一句话职责

Proc 子系统为每个数据库进程准备共享的进程状态槽，并把进程纳入锁等待与唤醒协议。

## 机制是什么

进程槽是共享内存中的 `PGPROC` 状态记录，锁管理器借它追踪进程身份、等待状态、等待队列链接、持有/等待的轻量锁等信息。初始化流程把当前后端与一条可供共享同步设施识别的记录关联；`ProcSleep` 在等待常规锁时使用该状态与队列协议睡眠，唤醒后恢复执行。

它不是进程创建器、事务快照集合或 SQL 锁模式定义者。postmaster/后台工作者框架负责进程生命周期；procarray 维护 MVCC 活跃事务集合；lock manager 决定常规锁冲突与排队规则；LWLock 则保护共享内存结构并提供低层同步。PGPROC 是这些机制间的共享进程状态载体之一，而非其全部语义。

## 怎么实现的

启动初始化分阶段进行。`InitProcess` 为当前进程取得可用的 `PGPROC` 槽，建立本地 `MyProc` 引用和基本标识，并初始化用于等待/唤醒、持锁追踪及错误恢复的状态。部分场景需要在更晚阶段由 `InitProcessPhase2` 才进入完整的共享处理状态，避免启动早期就参加某些同步域。

锁等待路径在持有必要的锁管理器保护时，将 PGPROC 加入目标等待队列并记录等待锁/模式，再释放队列保护后睡眠。`ProcSleep` 循环处理唤醒与中断检查，醒来后需重新确认等待结果，避免把“被唤醒”误判为已经取得锁。死锁检测、取消或退出等路径可改变等待状态，队列操作与状态标志必须在相应同步保护下保持一致。正常路径移除队列节点并清除等待信息；异常路径也需保证后续重用槽不会继承旧等待状态。

PGPROC 槽来自共享进程数组及其辅助结构，容量规划和槽分配由初始化配置与进程类型共同决定。槽会被后续进程复用，因此其清理、重置与引用有效期是安全性的关键。进程槽本身不保证上层事务已结束，也不负责回收该进程持有的所有锁语义。

## 关键取舍

- 共享槽让其他进程可观察等待者并协作唤醒，代价是状态更新必须遵守严格的锁与内存可见性协议。若改成仅用私有状态，跨进程锁等待队列无法高效协调。
- 初始化分阶段可避开启动期间不适用的共享同步环节；若所有初始化一次性完成，启动路径的依赖顺序更难控制。
- 使用槽复用减少动态共享内存分配，但要求完整重置状态，否则新进程会受到前任残留状态污染。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/lmgr/proc.c` | `InitProcess` | 绑定进程到共享 PGPROC 槽 |
| `src/backend/storage/lmgr/proc.c` | `InitProcessPhase2` | 完成延后阶段的进程状态初始化 |
| `src/backend/storage/lmgr/proc.c` | `ProcSleep` | 睡眠并处理 heavyweight lock 等待结果/唤醒；加入锁等待队列由 `JoinWaitQueue` 完成 |
| `src/include/storage/proc.h` | `PGPROC` | 共享进程状态结构定义 |

## 参考

- [PostgreSQL 官方文档：显式锁定](https://www.postgresql.org/docs/current/explicit-locking.html)
- [PostgreSQL 官方文档：监控锁等待](https://www.postgresql.org/docs/current/monitoring-stats.html#MONITORING-PG-LOCKS-VIEW)
- [PostgreSQL 源码：proc.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/lmgr/proc.c)

## 与主核 / AI 的关系

多进程主核可借鉴“共享进程状态槽+受保护等待队列”的结构，但必须自行定义进程代际、故障回收和唤醒规则。该机制不等价于业务任务调度器或 AI 作业队列。
