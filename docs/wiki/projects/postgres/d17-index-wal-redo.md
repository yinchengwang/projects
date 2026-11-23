---
project: postgres
feature: index-wal-redo
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtxlog.c
    symbols: [btree_redo, btree_xlog_startup, btree_xlog_cleanup]
  - path: src/backend/access/hash/hash_xlog.c
    symbols: [hash_redo, hash_mask]
  - path: src/backend/access/spgist/spgxlog.c
    symbols: [spg_redo, spg_xlog_startup, spg_xlog_cleanup]
---

# 索引 WAL redo（合并篇）

## 一句话职责

索引 WAL redo 按资源管理器与操作码重放已记录的页变更，使崩溃恢复及备库页面回到一致状态。

## 机制是什么

WAL record 描述索引操作及涉及的 block、数据片段或初始化标志。每个索引资源管理器的 redo dispatcher 按 info 操作码分派具体动作；页面从 WAL 的 block 引用读取或初始化，按记录内容重建、更新 tuple、opaque 状态及链接，最后设置 record end LSN 并标脏。`XLogReadBufferForRedo` 等接口负责依据页面 LSN 决定是否需要重做。

redo 重放的是主库已决定的物理变更，不重新运行 SQL、opclass 算法或索引选择策略。它不等于完整事务提交协议，也不负责生成原始 WAL。页面锁在恢复中通常可逐页获取而非复现主路径的跨页锁耦合；Hot Standby 需要保护读者时，特定记录仍会取 cleanup lock 或执行 recovery conflict 处理。

## 怎么实现的

1. 资源管理器先由 WAL 系统初始化记录解析态；B-tree `btree_redo`、Hash `hash_redo`、SP-GiST `spg_redo` 各自屏蔽通用 info bits，再 switch 到按操作类型划分的处理函数。
2. 处理函数解析主记录及 block data，调用 `XLogReadBufferForRedo` 或 `XLogInitBufferForRedo`。需要初始化页时依本 AM helper 建页头和 opaque；已有页只有在 redo 判断需要时才应用 delta，完整页映像恢复也可能还要补充未包含在映像里的 opaque 标志。
3. B-tree 按 insert/split/newroot 等记录恢复索引 tuple、兄弟链、高键和根元数据；插入父 downlink 时清除孩子页 incomplete-split 标记。`btree_xlog_split` 从 WAL 中分离左右页 tuple 数据重建页面。
4. Hash 按 record 重放 bucket/overflow 页、bitmap 和 metapage。split allocation 更新 bucket 元数据及进行中 flags；overflow add/squeeze 更新前后链接和 bitmap。Hot Standby 对需要的清理操作取得 bucket cleanup lock，避免扫描跨越结构变更。
5. SP-GiST `spg_redo` 分派 add leaf、move leafs、add node、pick split、vacuum 等操作。移动/分裂时 redo 先安装新目标 tuple，再把旧位置替换为有效 redirect，随后按记录更新 parent downlink，避免中间出现悬空链接。
6. 各页修改后设置 WAL end LSN 并标脏；B-tree 与 SP-GiST 对每条记录在专用操作 memory context 中分配临时对象，dispatch 结束后 reset，startup 创建 context，cleanup 删除。
7. B-tree `btree_mask`、Hash `hash_mask`、SP-GiST `spg_mask` 处理页面一致性测试前的非确定字段或未记录 hint，避免把合法差异当作 WAL redo 不一致。

## 关键取舍

- 源码事实：B-tree 与 SP-GiST 各用按操作生命周期管理的 `opCtx`，redo 完成后 reset；相较将临时对象散落在长期 context，可避免恢复过程按 WAL 记录数累积内存，代价是初始化/清理路径必须正确配对。
- 源码事实：SP-GiST redo 在 move/split 时先写新目标再写 redirect，Hash 和 B-tree 也按各自 WAL 操作顺序修复多页关系。redo 顺序保证崩溃恢复可重放；若只记录最终页而不记录操作顺序，partial-page replay 可能留下指向未准备目标的链接。
- 源码事实：恢复期间通常不重现 primary 上多页锁耦合；但 Hash 在 Hot Standby 针对会改变扫描内容的记录仍取 cleanup lock，B-tree 在 vacuum redo 对待删除页取得 cleanup lock。完全省略读者协调会让备库扫描重复或漏读，而每种记录都复刻 primary 全锁序会增加恢复开销且无并发写者收益。
- 源码事实：页 mask 会忽略 checksum/LSN、未使用空间及允许未 WAL 化的 hint 字段；否则一致性检查会把合法的非决定性差异误报为 redo 错误。mask 不能掩盖 tuple 与结构链接等实质数据差异。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtxlog.c` | `btree_redo` | 解析 B-tree WAL info 并分派具体页面重做操作 |
| `src/backend/access/nbtree/nbtxlog.c` | `btree_xlog_startup` | 创建 B-tree redo 临时内存 context |
| `src/backend/access/nbtree/nbtxlog.c` | `btree_xlog_cleanup` | 删除 B-tree redo context 并清空全局指针 |
| `src/backend/access/hash/hash_xlog.c` | `hash_redo` | 分派 Hash 初始化、插入、分裂、清理等 WAL 操作 |
| `src/backend/access/hash/hash_xlog.c` | `hash_mask` | 一致性检查前屏蔽合法变化的 hint、空闲区和 line pointer 标志 |
| `src/backend/access/spgist/spgxlog.c` | `spg_redo` | 分派 SP-GiST tuple、split、vacuum 等记录重放 |
| `src/backend/access/spgist/spgxlog.c` | `spg_xlog_startup` | 创建 SP-GiST redo 临时内存 context |
| `src/backend/access/spgist/spgxlog.c` | `spg_xlog_cleanup` | 释放 SP-GiST redo context |

## 参考

- [PostgreSQL 文档：可靠性与 WAL](https://www.postgresql.org/docs/current/wal-reliability.html) —— 说明 WAL 与崩溃恢复的关系。
- [PostgreSQL 官方源码：B-tree redo](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtxlog.c) —— 支撑 B-tree record 分派与页面恢复流程。
- [PostgreSQL 官方源码：Hash redo](https://github.com/postgres/postgres/blob/master/src/backend/access/hash/hash_xlog.c) —— 支撑 Hash 多页操作重放及页面 mask。
- [PostgreSQL 官方源码：SP-GiST redo](https://github.com/postgres/postgres/blob/master/src/backend/access/spgist/spgxlog.c) —— 支撑 redirect 顺序、页面重建和 context 生命周期。

## 与主核 / AI 的关系

主核 WAL redo 可参考“记录类型分派、页面 LSN 幂等判断、操作顺序恢复和非决定性字段屏蔽”的通用骨架；B-tree/Hash/SP-GiST 的页语义与恢复细节必须分别实现，不能跨 AM 混用。
