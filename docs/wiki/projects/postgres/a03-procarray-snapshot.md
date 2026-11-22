---
project: postgres
feature: procarray-snapshot
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/procarray.c
    symbols: [ProcArrayAdd, ProcArrayRemove, GetSnapshotData]
---

# 活跃事务集合与 MVCC 快照

## 一句话职责

ProcArray 为并发事务提供活跃 XID 集合，使快照能够判断哪些事务在快照时仍未结束。

## 机制是什么

ProcArray 维护活动后端及其事务状态的共享视图。MVCC snapshot 通常以 `xmin`、`xmax` 和 `xip` 集合描述可见性边界：低于 xmin 的事务已不可能仍活动；达到或超过 xmax 的新事务不属于快照；夹在边界内且出现在 xip 的事务在取快照时仍运行。快照随后由 heap tuple 可见性规则结合元组 xmin/xmax 判定版本可见性。ProcArray 并不保存每条元组的版本，也不决定 SQL 隔离级别语义。

## 怎么实现的

事务进入可见活动集合时由 `ProcArrayAdd` 发布其进程状态；结束时 `ProcArrayRemove` 从集合移除并协助维护最新已分配/已结束事务边界。`GetSnapshotData` 在共享同步保护下读取活跃事务信息，计算快照上下界并拷贝仍运行 XID 到快照的 `xip`，同时处理子事务相关状态及并发更新的一致性要求。读取时的共享锁保护集合快照，不等于整个事务持锁。

快照获取可能是高频路径，源码通过共享数组/辅助状态减少遍历代价，并在不同平台能力下采用相应同步策略。获取的 Snapshot 对象具有调用者管理的生命周期；语句级与事务级快照由上层选择和持有。事务提交状态最终还要结合提交状态存储和元组提示位等机制，不能只凭 ProcArray 判断历史 XID 的结果。

## 关键取舍

用活跃事务集合而非逐行维护可见性目录，使读取元组时可由版本 XID 与快照做局部判断。若每次取快照扫描完整事务历史，成本会随数据库运行时间增长；若每行读取都查询全局活跃状态，则热路径同步将不可接受。ProcArray 快照依然需复制/读取活动集合，其规模及并发协调是明确成本。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/ipc/procarray.c` | `ProcArrayAdd` | 发布活动事务 |
| `src/backend/storage/ipc/procarray.c` | `ProcArrayRemove` | 移除已结束事务 |
| `src/backend/storage/ipc/procarray.c` | `GetSnapshotData` | 构造 MVCC snapshot |

## 参考

- [PostgreSQL 文档：并发控制介绍](https://www.postgresql.org/docs/current/mvcc-intro.html)
- [PostgreSQL 文档：事务隔离](https://www.postgresql.org/docs/current/transaction-iso.html)

## 与主核 / AI 的关系

ProcArray 提供的是事务活动集合，不是 AI 缓存或通用事件日志；可作为理解 MVCC 快照边界的实现参照。
