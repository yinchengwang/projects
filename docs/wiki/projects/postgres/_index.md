---
project: postgres
page_type: overview
display_name: PostgreSQL
repo: https://github.com/postgres/postgres.git
license: PostgreSQL
language: unknown
stars: 0
stars_as_of: 1970-01-01
refs:
  - path: src/backend/tcop/postgres.c
    symbols: [PostgresMain]
  - path: src/backend/optimizer/plan/planner.c
    symbols: [standard_planner]
  - path: src/backend/executor/execMain.c
    symbols: [ExecutorStart]
  - path: src/backend/access/transam/xlog.c
    symbols: [XLogInsertRecord, XLogFlush]
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended]
---

# PostgreSQL

## 一句话职责
PostgreSQL 是提供关系型 SQL、事务、并发控制、持久化与扩展接口的开源数据库管理系统。

## 机制是什么
PostgreSQL 后端以进程承载客户端会话；SQL 解析、规划和执行分层完成查询处理。MVCC、WAL、锁及存储管理协作提供事务一致性与持久化。本文库按经审阅的机制单元组织，避免把一个源码文件机械等同于一篇机制页。

## 怎么实现的
后端会话接收协议消息并执行 SQL；优化器生成 Plan，执行器驱动表/索引访问方法。事务快照与提交状态判定版本可见性，WAL 为变更记录和崩溃恢复提供依据。共享缓冲池缓存关系页，smgr/md 将逻辑块映射到关系文件。进程槽、等待事件、共享内存和复制设施支撑并发及高可用。以上为系统级导航，具体算法与边界以对应机制页为准。

## 关键取舍
PostgreSQL 以多进程会话和明确的子系统接口换取隔离与可扩展性，代价是机制路径往往跨越多个实现层。MVCC 减少读写互阻，但要求维护事务状态、快照及版本回收；WAL 支持恢复，但写出顺序须满足日志先于数据页的持久化约束。具体收益取决于工作负载和配置，本文不声称其在所有场景最优。

## 源码落点
| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/backend/tcop/postgres.c` | `PostgresMain` | 后端会话循环 |
| `src/backend/optimizer/plan/planner.c` | `standard_planner` | 查询规划入口 |
| `src/backend/executor/execMain.c` | `ExecutorStart`、`ExecutorRun` | 执行器生命周期 |
| `src/backend/access/transam/xlog.c` | `XLogInsertRecord`、`XLogFlush` | WAL 插入与刷写 |
| `src/backend/storage/buffer/bufmgr.c` | `ReadBufferExtended` | 共享缓冲读入 |
| `src/backend/storage/smgr/smgr.c` | `smgropen` | 关系物理存储句柄 |

## 151 个机制页

按 12 个机制领域组织。每页是一个独立的机制单元，标题、`feature` 与 `refs` 由 `postgres_mechanism_specs.py` 冻结，正文由 writer 逐页写并经 `wiki_lint` + `wiki_refs` 双门禁。

| 领域 | 页数 |
|---|---:|
| A · 存储与 IPC | 20 |
| B · 事务与 WAL | 19 |
| C · 堆表与元组 | 15 |
| D · 索引族 | 17 |
| E · 优化器 | 13 |
| F · 执行器 | 13 |
| G · 目录与缓存 | 7 |
| H · 内存与排序 | 5 |
| I · 复制与高可用 | 10 |
| J · 服务端与命令面 | 12 |
| K · SQL 处理与安全 | 11 |
| L · SQL 类型与内建函数 | 9 |
| **合计** | **151** |

### A · 存储与 IPC（20 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`a01-buffer-pool.md`](a01-buffer-pool.md) | `buffer-pool` | 共享缓冲池与替换 |
| [`a02-local-buffer-pool.md`](a02-local-buffer-pool.md) | `local-buffer-pool` | 后端本地缓冲池 |
| [`a03-procarray-snapshot.md`](a03-procarray-snapshot.md) | `procarray-snapshot` | 活跃事务集合与 MVCC 快照 |
| [`a04-ssi-predicate-locks.md`](a04-ssi-predicate-locks.md) | `ssi-predicate-locks` | SSI 谓词锁与冲突检测 |
| [`a05-lock-manager-deadlock.md`](a05-lock-manager-deadlock.md) | `lock-manager-deadlock` | 常规锁与死锁检测 |
| [`a06-fd-cache-file-io.md`](a06-fd-cache-file-io.md) | `fd-cache-file-io` | 虚拟文件描述符与文件 I/O |
| [`a07-backend-proc-slots.md`](a07-backend-proc-slots.md) | `backend-proc-slots` | 后端进程槽 |
| [`a08-smgr-md-storage.md`](a08-smgr-md-storage.md) | `smgr-md-storage` | 关系物理存储（smgr 与 md） |
| [`a09-wait-event-sets.md`](a09-wait-event-sets.md) | `wait-event-sets` | 等待事件集 |
| [`a10-lwlocks.md`](a10-lwlocks.md) | `lwlocks` | 轻量锁 LWLock |
| [`a11-read-stream-prefetch.md`](a11-read-stream-prefetch.md) | `read-stream-prefetch` | 扫描预读流 |
| [`a12-shared-memory-allocation.md`](a12-shared-memory-allocation.md) | `shared-memory-allocation` | 主共享内存分配 |
| [`a13-dynamic-shared-memory.md`](a13-dynamic-shared-memory.md) | `dynamic-shared-memory` | 动态共享内存 DSM |
| [`a14-shared-memory-message-queues.md`](a14-shared-memory-message-queues.md) | `shared-memory-message-queues` | 共享内存消息队列 |
| [`a15-aio-io-methods.md`](a15-aio-io-methods.md) | `aio-io-methods` | 异步 I/O 请求与执行后端 |
| [`a16-free-space-map.md`](a16-free-space-map.md) | `free-space-map` | 空闲空间映射 FSM |
| [`a17-temporary-buffiles.md`](a17-temporary-buffiles.md) | `temporary-buffiles` | 临时 BufFile |
| [`a18-process-signals-barriers.md`](a18-process-signals-barriers.md) | `process-signals-barriers` | 进程信号与屏障 |
| [`a19-parallel-query-dsm.md`](a19-parallel-query-dsm.md) | `parallel-query-dsm` | 并行查询 DSM 基础设施 |
| [`a20-shared-invalidation.md`](a20-shared-invalidation.md) | `shared-invalidation` | 共享失效消息传输原语 |

### B · 事务与 WAL（19 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`b01-wal-insertion-and-write.md`](b01-wal-insertion-and-write.md) | `wal-insertion-and-write` | WAL 记录插入与写出 |
| [`b02-checkpoint-and-wal-flush.md`](b02-checkpoint-and-wal-flush.md) | `checkpoint-and-wal-flush` | Checkpoint 与持久化屏障 |
| [`b03-wal-recovery-and-restore.md`](b03-wal-recovery-and-restore.md) | `wal-recovery-and-restore` | WAL 恢复与归档取回 |
| [`b04-wal-reader.md`](b04-wal-reader.md) | `wal-reader` | 通用 WAL Reader |
| [`b05-standby-recovery-conflict.md`](b05-standby-recovery-conflict.md) | `standby-recovery-conflict` | 热备恢复冲突 |
| [`b06-multixact-member-status.md`](b06-multixact-member-status.md) | `multixact-member-status` | MultiXact 成员与状态 |
| [`b07-two-phase-commit.md`](b07-two-phase-commit.md) | `two-phase-commit` | 两阶段提交 |
| [`b08-transaction-status-slru.md`](b08-transaction-status-slru.md) | `transaction-status-slru` | SLRU 状态页缓存实现 |
| [`b09-xid-commit-status.md`](b09-xid-commit-status.md) | `xid-commit-status` | XID 提交状态（CLOG/pg_xact） |
| [`b10-subtransaction-parent-map.md`](b10-subtransaction-parent-map.md) | `subtransaction-parent-map` | 子事务父映射 |
| [`b11-commit-timestamp.md`](b11-commit-timestamp.md) | `commit-timestamp` | 提交时间戳存储 |
| [`b12-xid-allocation.md`](b12-xid-allocation.md) | `xid-allocation` | XID 分配与防回卷 |
| [`b13-transaction-lifecycle-and-status.md`](b13-transaction-lifecycle-and-status.md) | `transaction-lifecycle-and-status` | 事务生命周期与状态接口 |
| [`b14-recovery-wal-prefetch.md`](b14-recovery-wal-prefetch.md) | `recovery-wal-prefetch` | Redo WAL 预取 |
| [`b15-wal-resource-manager.md`](b15-wal-resource-manager.md) | `wal-resource-manager` | WAL rmgr 分派 |
| [`b16-wal-rmgr-description.md`](b16-wal-rmgr-description.md) | `wal-rmgr-description` | WAL record 描述器 |
| [`b17-wal-archive-backup-interfaces.md`](b17-wal-archive-backup-interfaces.md) | `wal-archive-backup-interfaces` | 备份与归档接口 |
| [`b18-synchronous-wal-wait.md`](b18-synchronous-wal-wait.md) | `synchronous-wal-wait` | WAL/LSN 等待与唤醒 |
| [`b19-generic-xlog-api.md`](b19-generic-xlog-api.md) | `generic-xlog-api` | Generic WAL API |

### C · 堆表与元组（15 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`c01-table-access-method-heap.md`](c01-table-access-method-heap.md) | `table-access-method-heap` | Heap TableAM 访问 |
| [`c02-lazy-vacuum-and-pruning.md`](c02-lazy-vacuum-and-pruning.md) | `lazy-vacuum-and-pruning` | Lazy VACUUM 与页内剪枝 |
| [`c03-visibility-map.md`](c03-visibility-map.md) | `visibility-map` | Visibility Map |
| [`c04-heap-mvcc-visibility.md`](c04-heap-mvcc-visibility.md) | `heap-mvcc-visibility` | Heap tuple MVCC 可见性 |
| [`c05-heap-rewrite.md`](c05-heap-rewrite.md) | `heap-rewrite` | Heap 重写 |
| [`c06-tuple-descriptor-layout.md`](c06-tuple-descriptor-layout.md) | `tuple-descriptor-layout` | Tuple descriptor 与 tuple 编码 |
| [`c07-relation-options.md`](c07-relation-options.md) | `relation-options` | Relation options |
| [`c08-toast-storage-and-compression.md`](c08-toast-storage-and-compression.md) | `toast-storage-and-compression` | TOAST 外置与压缩 |
| [`c09-tidstore.md`](c09-tidstore.md) | `tidstore` | TID 集合存储 |
| [`c10-synchronized-sequential-scan.md`](c10-synchronized-sequential-scan.md) | `synchronized-sequential-scan` | 同步顺序扫描 |
| [`c11-tuple-attribute-mapping-conversion.md`](c11-tuple-attribute-mapping-conversion.md) | `tuple-attribute-mapping-conversion` | Tuple 属性映射与转换 |
| [`c12-heap-wal-redo.md`](c12-heap-wal-redo.md) | `heap-wal-redo` | Heap WAL redo |
| [`c13-large-object-storage.md`](c13-large-object-storage.md) | `large-object-storage` | 大对象存储 |
| [`c14-heap-page-layout.md`](c14-heap-page-layout.md) | `heap-page-layout` | Heap 页与 line pointer |
| [`c15-heap-index-fetch.md`](c15-heap-index-fetch.md) | `heap-index-fetch` | Heap 回表取行 |

### D · 索引族（17 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`d01-index-am-framework.md`](d01-index-am-framework.md) | `index-am-framework` | 通用 IndexAM 扫描框架 |
| [`d02-index-tuple-representation.md`](d02-index-tuple-representation.md) | `index-tuple-representation` | IndexTuple 编码 |
| [`d03-btree-scan-search.md`](d03-btree-scan-search.md) | `btree-scan-search` | B-tree 扫描与搜索 |
| [`d04-btree-insertion-split.md`](d04-btree-insertion-split.md) | `btree-insertion-split` | B-tree 插入与分裂 |
| [`d05-btree-scan-key-preprocessing.md`](d05-btree-scan-key-preprocessing.md) | `btree-scan-key-preprocessing` | B-tree 扫描键预处理 |
| [`d06-btree-page-maintenance.md`](d06-btree-page-maintenance.md) | `btree-page-maintenance` | B-tree 页维护与去重 |
| [`d07-btree-build.md`](d07-btree-build.md) | `btree-build` | B-tree 构建 |
| [`d08-gin-write-and-pending-list.md`](d08-gin-write-and-pending-list.md) | `gin-write-and-pending-list` | GIN 写入与 pending list |
| [`d09-gin-search.md`](d09-gin-search.md) | `gin-search` | GIN 搜索 |
| [`d10-gin-data-pages.md`](d10-gin-data-pages.md) | `gin-data-pages` | GIN 数据页 |
| [`d11-gist-search-insert.md`](d11-gist-search-insert.md) | `gist-search-insert` | GiST 搜索与插入 |
| [`d12-gist-build-and-page-split.md`](d12-gist-build-and-page-split.md) | `gist-build-and-page-split` | GiST 构建与页拆分 |
| [`d13-hash-index-storage-and-operations.md`](d13-hash-index-storage-and-operations.md) | `hash-index-storage-and-operations` | Hash 索引操作与溢出页 |
| [`d14-brin-core.md`](d14-brin-core.md) | `brin-core` | BRIN 扫描与维护 |
| [`d15-brin-opclasses-summarization.md`](d15-brin-opclasses-summarization.md) | `brin-opclasses-summarization` | BRIN 摘要算子类 |
| [`d16-spgist-access-maintenance.md`](d16-spgist-access-maintenance.md) | `spgist-access-maintenance` | SP-GiST 访问与维护 |
| [`d17-index-wal-redo.md`](d17-index-wal-redo.md) | `index-wal-redo` | 索引 WAL redo（合并篇） |

### E · 优化器（13 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`e01-planner-orchestration.md`](e01-planner-orchestration.md) | `planner-orchestration` | 查询规划编排 |
| [`e02-query-preprocessing.md`](e02-query-preprocessing.md) | `query-preprocessing` | 查询树预处理 |
| [`e03-subquery-transform.md`](e03-subquery-transform.md) | `subquery-transform` | 子查询与子链接转换 |
| [`e04-rel-metadata.md`](e04-rel-metadata.md) | `rel-metadata` | 关系建模与继承展开 |
| [`e05-path-generation.md`](e05-path-generation.md) | `path-generation` | 访问路径生成 |
| [`e06-join-search.md`](e06-join-search.md) | `join-search` | 连接枚举与算法候选 |
| [`e07-geqo.md`](e07-geqo.md) | `geqo` | 遗传查询优化 |
| [`e08-path-ranking.md`](e08-path-ranking.md) | `path-ranking` | 路径支配与最优选择 |
| [`e09-cost-cardinality.md`](e09-cost-cardinality.md) | `cost-cardinality` | 成本与基数估算 |
| [`e10-equivalence-and-order.md`](e10-equivalence-and-order.md) | `equivalence-and-order` | 等价类与排序传播 |
| [`e11-predicate-reasoning.md`](e11-predicate-reasoning.md) | `predicate-reasoning` | 谓词推理与连接简化 |
| [`e12-plan-construction.md`](e12-plan-construction.md) | `plan-construction` | Plan 树构造 |
| [`e13-plan-reference-fixing.md`](e13-plan-reference-fixing.md) | `plan-reference-fixing` | Plan 引用修整 |

### F · 执行器（13 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`f01-executor-lifecycle.md`](f01-executor-lifecycle.md) | `executor-lifecycle` | 执行器生命周期 |
| [`f02-executor-state.md`](f02-executor-state.md) | `executor-state` | 执行状态与资源生命周期 |
| [`f03-node-dispatch.md`](f03-node-dispatch.md) | `node-dispatch` | 节点初始化与驱动 |
| [`f04-expression-evaluation.md`](f04-expression-evaluation.md) | `expression-evaluation` | 表达式求值 |
| [`f05-tuple-slots.md`](f05-tuple-slots.md) | `tuple-slots` | TupleTableSlot 表示 |
| [`f06-scan-nodes.md`](f06-scan-nodes.md) | `scan-nodes` | 表与定位扫描节点 |
| [`f07-index-and-bitmap-scans.md`](f07-index-and-bitmap-scans.md) | `index-and-bitmap-scans` | 索引与位图扫描执行 |
| [`f08-join-algorithms.md`](f08-join-algorithms.md) | `join-algorithms` | 连接执行算法 |
| [`f09-aggregation-and-window.md`](f09-aggregation-and-window.md) | `aggregation-and-window` | 聚合与窗口 |
| [`f10-sort-and-materialization.md`](f10-sort-and-materialization.md) | `sort-and-materialization` | 排序与物化节点 |
| [`f11-append-and-subplans.md`](f11-append-and-subplans.md) | `append-and-subplans` | Append 族与子计划 |
| [`f12-parallel-execution.md`](f12-parallel-execution.md) | `parallel-execution` | 并行执行 |
| [`f13-modify-and-partition-routing.md`](f13-modify-and-partition-routing.md) | `modify-and-partition-routing` | 修改节点与分区路由 |

### G · 目录与缓存（7 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`g01-catalog-storage-bootstrap.md`](g01-catalog-storage-bootstrap.md) | `catalog-storage-bootstrap` | 系统目录持久化与引导 |
| [`g02-syscache.md`](g02-syscache.md) | `syscache` | 系统缓存 |
| [`g03-relcache.md`](g03-relcache.md) | `relcache` | 关系缓存生命周期 |
| [`g04-cache-invalidation-semantics.md`](g04-cache-invalidation-semantics.md) | `cache-invalidation-semantics` | 缓存失效语义 |
| [`g05-object-dependencies.md`](g05-object-dependencies.md) | `object-dependencies` | 对象依赖与删除 |
| [`g06-namespace-resolution.md`](g06-namespace-resolution.md) | `namespace-resolution` | 命名空间解析 |
| [`g07-acl-and-ownership.md`](g07-acl-and-ownership.md) | `acl-and-ownership` | 权限与属主 |

### H · 内存与排序（5 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`h01-memory-contexts.md`](h01-memory-contexts.md) | `memory-contexts` | 内存上下文体系 |
| [`h02-resource-owners.md`](h02-resource-owners.md) | `resource-owners` | 资源所有者与错误期回收 |
| [`h03-tuplesort.md`](h03-tuplesort.md) | `tuplesort` | 元组外部排序 |
| [`h04-logical-tapes.md`](h04-logical-tapes.md) | `logical-tapes` | 逻辑磁带 |
| [`h05-sort-primitives.md`](h05-sort-primitives.md) | `sort-primitives` | 排序基元 |

### I · 复制与高可用（10 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`i01-physical-streaming.md`](i01-physical-streaming.md) | `physical-streaming` | 物理流复制 |
| [`i02-replication-slots.md`](i02-replication-slots.md) | `replication-slots` | 复制槽 |
| [`i03-sync-replication.md`](i03-sync-replication.md) | `sync-replication` | 同步复制 |
| [`i04-logical-decoding.md`](i04-logical-decoding.md) | `logical-decoding` | 逻辑解码 |
| [`i05-logical-replication-apply.md`](i05-logical-replication-apply.md) | `logical-replication-apply` | 逻辑复制应用 |
| [`i06-reorder-buffer.md`](i06-reorder-buffer.md) | `reorder-buffer` | ReorderBuffer |
| [`i07-snapshot-build.md`](i07-snapshot-build.md) | `snapshot-build` | 逻辑解码快照构建 |
| [`i08-tablesync.md`](i08-tablesync.md) | `tablesync` | 表同步 |
| [`i09-origin-tracking.md`](i09-origin-tracking.md) | `origin-tracking` | 复制来源追踪 |
| [`i10-archive-and-base-backup.md`](i10-archive-and-base-backup.md) | `archive-and-base-backup` | 归档与基础备份 |

### J · 服务端与命令面（12 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`j01-postmaster-lifecycle.md`](j01-postmaster-lifecycle.md) | `postmaster-lifecycle` | postmaster 与子进程管理 |
| [`j02-backend-protocol-session.md`](j02-backend-protocol-session.md) | `backend-protocol-session` | 后端协议与会话循环 |
| [`j03-utility-command-execution.md`](j03-utility-command-execution.md) | `utility-command-execution` | utility 命令执行 |
| [`j04-ddl-object-commands.md`](j04-ddl-object-commands.md) | `ddl-object-commands` | 对象 DDL 命令域 |
| [`j05-copy-commands.md`](j05-copy-commands.md) | `copy-commands` | COPY 命令 |
| [`j06-background-worker-framework.md`](j06-background-worker-framework.md) | `background-worker-framework` | 后台 worker 框架 |
| [`j07-autovacuum-launcher.md`](j07-autovacuum-launcher.md) | `autovacuum-launcher` | autovacuum 调度 |
| [`j08-checkpointer-process.md`](j08-checkpointer-process.md) | `checkpointer-process` | checkpointer 进程 |
| [`j09-pgstat-activity.md`](j09-pgstat-activity.md) | `pgstat-activity` | 运行统计 |
| [`j10-error-reporting.md`](j10-error-reporting.md) | `error-reporting` | 错误上报 |
| [`j11-async-notify-listen.md`](j11-async-notify-listen.md) | `async-notify-listen` | LISTEN/NOTIFY |
| [`j12-session-and-guc.md`](j12-session-and-guc.md) | `session-and-guc` | GUC 与会话配置 |

### K · SQL 处理与安全（11 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`k01-parser-pipeline.md`](k01-parser-pipeline.md) | `parser-pipeline` | SQL 词法与语法解析 |
| [`k02-parse-analysis.md`](k02-parse-analysis.md) | `parse-analysis` | SQL 语义分析 |
| [`k03-rewrite-and-views.md`](k03-rewrite-and-views.md) | `rewrite-and-views` | 重写与视图展开 |
| [`k04-node-tree-utilities.md`](k04-node-tree-utilities.md) | `node-tree-utilities` | 节点树工具 |
| [`k05-tablesample.md`](k05-tablesample.md) | `tablesample` | TABLESAMPLE 抽样 |
| [`k06-client-auth-hba.md`](k06-client-auth-hba.md) | `client-auth-hba` | 客户端认证与 HBA |
| [`k07-auth-methods.md`](k07-auth-methods.md) | `auth-methods` | 认证方法 |
| [`k08-tls-transport.md`](k08-tls-transport.md) | `tls-transport` | TLS 传输安全 |
| [`k09-row-level-security.md`](k09-row-level-security.md) | `row-level-security` | 行级安全 |
| [`k10-authorization-checks.md`](k10-authorization-checks.md) | `authorization-checks` | 权限检查 |
| [`k11-event-triggers.md`](k11-event-triggers.md) | `event-triggers` | 事件触发器 |

### L · SQL 类型与内建函数（9 页）

| 页 | 机制 | 标题 |
|---|---|---|
| [`l01-type-system-and-typcache.md`](l01-type-system-and-typcache.md) | `type-system-and-typcache` | 类型系统与 typcache |
| [`l02-array-types.md`](l02-array-types.md) | `array-types` | 数组类型 |
| [`l03-numeric-type.md`](l03-numeric-type.md) | `numeric-type` | numeric 与 decimal |
| [`l04-date-time-types.md`](l04-date-time-types.md) | `date-time-types` | 日期时间类型 |
| [`l05-json-jsonb-types.md`](l05-json-jsonb-types.md) | `json-jsonb-types` | JSON/JSONB |
| [`l06-text-and-varlena.md`](l06-text-and-varlena.md) | `text-and-varlena` | text 与 varlena |
| [`l07-range-and-multirange.md`](l07-range-and-multirange.md) | `range-and-multirange` | 范围类型 |
| [`l08-network-types.md`](l08-network-types.md) | `network-types` | 网络类型 |
| [`l09-regex-and-fulltext.md`](l09-regex-and-fulltext.md) | `regex-and-fulltext` | 正则与全文 |

## 参考
- [PostgreSQL 官方文档：简介](https://www.postgresql.org/docs/current/intro-whatis.html)
- [PostgreSQL 官方文档：体系架构教程](https://www.postgresql.org/docs/current/tutorial-arch.html)
- [PostgreSQL 官方源码仓库](https://github.com/postgres/postgres)

## 与主核 / AI 的关系
本仓主数据库内核是从零实现的多模态数据库；PostgreSQL 页面用于机制对照，比较时应明确接口、事务语义、存储表示与恢复保证的差异。AI 生成的导航不能代替对指定 checkout 源码及版本的核对。
