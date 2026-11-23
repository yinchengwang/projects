---
project: postgres
feature: subtransaction-parent-map
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/subtrans.c
    symbols: [SubTransSetParent, SubTransGetParent]
---

# 子事务父映射

## 一句话职责

`pg_subtrans` 为近期子事务保存其直接父 XID，使事务状态检查路径在内存缓存不足时能够沿关系恢复到祖先事务。

## 机制是什么

每个普通子事务的 XID 对应一个父 TransactionId 槽；`SubTransSetParent` 建立 subXID→parent XID 映射，`SubTransGetParent` 查回直接父项。存储按 XID 定位 SLRU 页，页内每项是 `TransactionId`，表内容不是 commit/abort 状态位，也不决定某事务当前是否仍运行。

该机制只负责子事务父映射，不负责 CLOG/pg_xact 提交状态（b09）、XID 分配，也不取代 ProcArray 的活跃事务集合或 MultiXact 成员关系。`SubTransGetTopmostTransaction` 可沿父映射迭代，但读取范围受 `TransactionXmin` 约束：更早映射可能已回收，返回中间祖先对特定活跃事务判断仍足够。

## 怎么实现的

1. `SubTransSetParent` 计算子 XID 所在页和 entry，验证 parent 合法且按分配顺序早于 child；获取该页 bank lock 并 `SimpleLruReadPage` 读入。
2. 页内映射必须从 invalid 变为所给 parent；重复写相同 parent 可接受，但从一个有效父 XID 改成另一有效值属于结构损坏，调试断言阻止覆盖。成功时标记 SLRU 页 dirty 并释放锁。
3. `SubTransGetParent` 确认请求 XID 不早于 `TransactionXmin`，非普通 XID（bootstrap/frozen）直接返回 invalid；普通 XID 通过只读 SLRU 页读取 entry，在释放 bank lock 后返回父 XID。
4. 子事务登记在分配/子事务路径建立映射；`ExtendSUBTRANS` 在页边界零化新页，启动恢复时 `StartupSUBTRANS` 将当前活跃 XID 范围的页清零，因为源码不要求 crash 后旧 pg_subtrans 映射本身持久有效。checkpoint 写脏页主要是把 I/O 移给 checkpointer，并非该状态的正确性前提。
5. 向上追祖先时，父 XID 按约定必须早于 child；发现 parent 不早于 child 会报错避免损坏映射导致无限循环。追溯到 `TransactionXmin` 前停止，容许返回最近可用的中间父项。

## 关键取舍

- **只保留直接父映射而非每个子事务重复存顶层 XID（源码事实）**：节省存储，查顶层需逐级读取父链；若每项存完整祖先信息，写入和维护成本将随层级变化且更新更多。
- **不把 crash 后 pg_subtrans 当作可靠历史（源码事实）**：startup 重置仍可能被引用的活动范围并随页扩展重新初始化；恢复时依赖其他恢复状态重建所需关系。强制持久化所有映射可减少启动重建，但需承受更重 WAL/同步开销，源码明确说明当前设计不假设跨崩溃有效。
- **用 TransactionXmin 划定可查询范围（源码事实）**：越界请求由断言拒绝，追顶层时遇到已回收历史可退回中间祖先；对于活跃状态检查足够，但不能用该结果做任意久远历史的完整父树审计。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/subtrans.c` | `SubTransSetParent` | 在对应 SLRU 页中只允许写入子事务的直接父 XID |
| `src/backend/access/transam/subtrans.c` | `SubTransGetParent` | 从近期映射页取回直接父项，过滤非普通 XID |

## 与主核 / AI 的关系

暂无直接对应。父链可帮助区分子事务与其顶层事务，但不能据此替代主核的事务树、状态查询或快照表示。

## 参考

- [PostgreSQL 官方源码：subtrans.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/subtrans.c) —— 支撑直接父映射、启动清零与有效查询范围。
- [PostgreSQL 官方源码：slru.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/slru.c) —— 支撑父映射页缓存的共享读写机制（具体通用算法见 b08）。
- [PostgreSQL 文档：事务 ID](https://www.postgresql.org/docs/current/transaction-id.html) —— 支撑 XID 与子事务背景概念。
