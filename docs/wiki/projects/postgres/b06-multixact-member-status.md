---
project: postgres
feature: multixact-member-status
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/multixact.c
    symbols: [MultiXactIdCreate, GetMultiXactIdMembers, MultiXactIdIsRunning]
---

# MultiXact 成员与状态

## 一句话职责

MultiXact 把多个事务 ID 及其 tuple lock/update 状态编码为一个可存入 tuple xmax 的共享成员集合，并回答成员是否仍运行。

## 机制是什么

单个 tuple 可能同时被多个事务持有共享锁，或需要记录锁持有者与更新者的状态；一个 xmax 不能只表达单一事务 ID。MultiXact 为成员集合分配 MXID，成员包含 `xid` 与 `MultiXactStatus`，分别记 tuple 锁模式和更新/删除状态。持久表示拆成 offset SLRU（每 MXID 的成员起点）及 member SLRU（成员 XID 与状态位），另有本地缓存以减少重复 I/O。

此机制管理 MultiXact 创建、成员读取、是否仍有运行成员及老化维护；不替代 XID 的提交状态查询（b09）或 CLOG/SLRU 通用缓存（b08），也不提供 heavyweight lock manager 的对象锁等待。已创建的 MultiXact 不可原地追加成员：扩展会生成新 MXID，避免等待旧 MXID 的读者遇到成员集合并发变更。

## 怎么实现的

1. `MultiXactIdCreate` 将两组 xid/status 包为 `MultiXactMember` 数组后调用 `MultiXactIdCreateFromMembers`。后者先按成员集合查本地缓存并排序；相同集合可复用已有 MXID，同时拒绝多于一个 update member 的非法集合。
2. 未命中时 `GetNewMultiXactId` 在 `MultiXactGenLock` 下分配 MXID 与成员 offset，并确保 SLRU 页已扩展。`MultiXactIdCreateFromMembers` 标记 checkpoint 延迟，写 `XLOG_MULTIXACT_CREATE_ID` WAL，再由 `RecordNewMultiXact` 将 MXID 起点、下一 MXID offset 和成员/状态写进各自 SLRU，结束关键区后填充本地 cache。
3. `GetMultiXactIdMembers` 先查缓存并建立当前事务 `OldestVisibleMXactId`，再校验请求 MXID 位于 `[oldestMultiXactId,nextMXact)` 有效区间。它从 offset 页取当前与下一个 MXID 的成员偏移，差值确定成员数，再逐页从 member SLRU 读出 XID 和状态位。
4. 每次跨 SLRU 页时按新页取对应 bank lock；不同时持有两个 bank 锁，而是释放前一页锁后再获取下一页锁，防止锁序交叉。无效/越界 offset、零成员或不合理成员计数触发数据损坏错误。
5. `MultiXactIdIsRunning` 取成员后先检查是否为本事务活跃子事务，再逐成员调用 `TransactionIdIsInProgress`；只要任一成员仍运行即返回 true，否则 false。旧已提交的 update member 是否保留与“是否仍运行”不同：这里判断的是成员运行态，不把提交状态直接当成活跃。
6. per-backend `OldestMemberMXactId` 与 `OldestVisibleMXactId` 水位保护可能仍被事务引用的 SLRU 范围；维护路径据此推进 truncation horizon，回收不再可见的 offset/member 段。

## 关键取舍

- **成员集合只追加为新 MXID，不原地扩展（源码事实）**：`MultiXactIdExpand` 读旧成员后筛除已无意义成员并建新集合，避免另一代码路径等待原 MXID 期间成员悄悄变更。替代为原地更新需为每次读取/等待增加成员版本同步与竞争控制。
- **offset 与 member 分区存储（源码事实）**：offset 表按 MXID 定位成员区间，member 区只顺序保存成员记录；取成员需要访问两类 SLRU，增加一次元数据查找，但避免每个 MXID 预留固定大小成员槽，成员数量可变。
- **本地缓存按集合复用 MXID（源码事实）**：缓存命中可减少重复 WAL、SLRU 写与 MXID 消耗；缓存不是持久真相，未命中仍要写入日志/SLRU。若为发现全局重复定义而扫描磁盘，会增加创建热路径开销，源码注释说明边缘情况下不值得。
- **读取路径用水位保护而非长期持有全局截断锁（源码事实）**：记录 oldest visible 水位后，截断方可安全确定页是否仍可能被读取；代价是维护额外 per-backend 状态并在事务结束清理。无保护直接允许并发截断可让成员读取访问已删除页。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/multixact.c` | `MultiXactIdCreate` | 将两名事务成员与状态交给集合创建路径 |
| `src/backend/access/transam/multixact.c` | `GetMultiXactIdMembers` | 校验有效 MXID 范围并从 offset/member SLRU 重建成员集合 |
| `src/backend/access/transam/multixact.c` | `MultiXactIdIsRunning` | 检查成员是否属于当前事务或 procarray 中仍运行的事务 |

## 与主核 / AI 的关系

暂无直接对应。多持有者 tuple 状态集合可作概念参照，但不可与普通锁表或主核事务状态表混为一谈。

## 参考

- [PostgreSQL 文档：多事务](https://www.postgresql.org/docs/current/routine-vacuuming.html#VACUUM-FOR-MULTIXACT-WRAPAROUND) —— 支撑 MultiXact 与 wraparound/维护关系。
- [PostgreSQL 官方源码：multixact.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/multixact.c) —— 支撑成员创建、offset/member 存储、读取和运行状态判断。
- [PostgreSQL 官方源码：multixact.h](https://github.com/postgres/postgres/blob/master/src/include/access/multixact.h) —— 支撑成员状态枚举和持久化记录结构。
