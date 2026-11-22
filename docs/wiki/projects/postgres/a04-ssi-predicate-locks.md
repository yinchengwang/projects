---
project: postgres
feature: ssi-predicate-locks
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/lmgr/predicate.c
    symbols: [CheckForSerializableConflictOut, CheckForSerializableConflictIn, PredicateLockRelation]
---

# SSI 谓词锁与冲突检测

## 一句话职责

SSI 谓词锁记录可串行化事务读过的数据范围，并据此发现可能破坏串行化顺序的读写依赖。

## 机制是什么

PostgreSQL Serializable Snapshot Isolation (SSI) 以快照隔离执行事务，同时追踪读写冲突，识别危险结构并在必要时中止事务。谓词锁是 SIREAD 读痕迹，不像常规锁那样阻止写操作；它用于之后发现 rw-antidependency。锁粒度可从 tuple/page 提升到 relation，以限制跟踪开销。谓词锁也不能消除所有业务层重试：应用须处理 serialization failure。

该机制只为 SERIALIZABLE 事务提供串行化保证，不替代常规锁管理器的互斥/阻塞职责，也不负责一般 MVCC 元组可见性。

## 怎么实现的

读取路径通过 predicate lock 接口记录访问范围。`PredicateLockRelation` 对整关系登记 SIREAD 锁；更细粒度读由同一模块接口追踪。内存紧张或锁数量达到阈值时，粒度可向更粗对象合并，宁可增加保守冲突也避免无限增长。

读取侧调用 `CheckForSerializableConflictOut`：当表访问方法读到一条已被修改的元组时，它把创建或修改该版本的事务 XID 传进来，本函数判断对方是否也是可串行化事务且与本事务重叠（彼此看不到对方的写），若是就记下一条冲突出边。

写侧调用 `CheckForSerializableConflictIn`：本事务正在写入这条元组，函数检查其他可串行化事务是否已在关系、页或元组粒度上留下谓词锁，若有就记下一条冲突入边。两侧共同维护依赖图与冲突边，重点判断是否形成危险的连续读写反依赖结构；发现危险结构后，结合事务提交/可撤销状态选择牺牲者，避免可能产生不可串行化结果。

SIREAD 锁在事务提交后可能需要保留，直到与之并发的事务完成，因为后者稍后提交的写仍可能证明依赖关系。清理与代际状态使其生命周期长于单次语句。该过程是保守检测：可能中止本可序列化的事务；这是避免错误接受的代价。

## 关键取舍

SSI 不让读写冲突普遍阻塞，从而保留快照隔离的并发性，但支付冲突追踪和偶发回滚成本。若换成严格两阶段锁，冲突在执行时等待，长事务可能阻塞大量读写；SSI 通过事后依赖检测减少等待，但应用必须重试被中止事务，并接受保守误报。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/lmgr/predicate.c` | `PredicateLockRelation` | 关系粒度 SIREAD 登记 |
| `src/backend/storage/lmgr/predicate.c` | `CheckForSerializableConflictOut` | 读侧：读到他人修改版本时检查冲突出 |
| `src/backend/storage/lmgr/predicate.c` | `CheckForSerializableConflictIn` | 写侧：检查他人留下的谓词锁即冲突入 |

## 参考

- [PostgreSQL 文档：可串行化隔离级别](https://www.postgresql.org/docs/current/transaction-iso.html#XACT-SERIALIZABLE)
- [Ports & Grittner, Serializable Snapshot Isolation in PostgreSQL](https://doi.org/10.14778/2367502.2367523)

## 与主核 / AI 的关系

SSI 的冲突图与范围读痕迹适合作为正确性分析案例；不能将 SIREAD 当作阻塞式行锁或模型提示缓存。
