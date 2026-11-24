---
project: sqlite
page_type: overview
display_name: SQLite
repo: https://github.com/sqlite/sqlite.git
license: Public Domain
language: C
stars: 11104
stars_as_of: 2026-11-23
refs:
  - path: src/main.c
    symbols: [sqlite3_open, sqlite3_close]
  - path: src/tokenize.c
    symbols: [sqlite3GetToken]
  - path: src/parse.y
    symbols: [sqlite3Parser]
  - path: src/resolve.c
    symbols: [sqlite3ResolveExprNames]
  - path: src/expr.c
    symbols: [sqlite3ExprCodeTarget]
  - path: src/select.c
    symbols: [sqlite3Select]
  - path: src/vdbe.c
    symbols: [sqlite3VdbeExec]
  - path: src/btree.c
    symbols: [sqlite3BtreeOpen, sqlite3BtreeTableMoveto]
  - path: src/pager.c
    symbols: [sqlite3PagerGet, sqlite3PagerWrite]
  - path: src/wal.c
    symbols: [sqlite3WalOpen, walCheckpoint]
  - path: src/os.c
    symbols: [sqlite3OsRead, sqlite3OsWrite]
---

# SQLite

## 一句话职责

SQLite 是进程内嵌入、无需服务端的开源 SQL 数据库引擎，以单文件承载整个关系数据库。

## 机制是什么

SQLite 以库内嵌方式运行，调用方直接链接其代码。SQL 文本经分词、语法分析与名称解析后形成表达式树，查询编译阶段把语句转成虚拟机字节码程序；执行器逐条解释字节码，通过分页器把页读写落到文件，文件之上的页缓存管理脏页与回收。存储层是 B-tree，记录采用变长编码。事务以回滚日志或 WAL 实现，两种模式下的可见性规则不同。

本文库按经审阅的机制单元组织：**14 个机制领域、共 86 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 14 个领域按下面的顺序排列：A 连接与生命周期 → B 解析与语义解析 → C 表达式与代码生成 → D 查询编译 → E VDBE 执行 → F B-tree 与记录格式 → G Pager 与回滚日志 → H WAL → I 页缓存 → J VFS 与内存管理 → K 约束与索引维护 → L 函数 → M 事务与并发 → N DDL 与 PRAGMA。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

## 机制导航（86 篇 / 14 组）

### A-连接与生命周期（6 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-db-connection-open.md`](a01-db-connection-open.md) | 数据库连接与打开 |
| [`a02-db-close.md`](a02-db-close.md) | 连接关闭与僵尸连接 |
| [`a03-global-initialization.md`](a03-global-initialization.md) | 全局初始化与关闭 |
| [`a04-schema-cookie.md`](a04-schema-cookie.md) | 模式版本与缓存失效 |
| [`a05-busy-handler.md`](a05-busy-handler.md) | 锁冲突忙等待回调 |
| [`a06-connection-status-metrics.md`](a06-connection-status-metrics.md) | 连接与全局状态统计 |

### B-解析与名称解析（6 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-tokenizer.md`](b01-tokenizer.md) | 词法分析器 |
| [`b02-sql-parser.md`](b02-sql-parser.md) | 语法分析与语法树 |
| [`b03-name-resolution.md`](b03-name-resolution.md) | 名称解析 |
| [`b04-prepare-statement.md`](b04-prepare-statement.md) | 语句编译入口 |
| [`b05-schema-load.md`](b05-schema-load.md) | 模式表加载 |
| [`b06-reprepare.md`](b06-reprepare.md) | 模式变更后的语句重编译 |

### C-表达式与代码生成（6 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-expression-tree.md`](c01-expression-tree.md) | 表达式树 |
| [`c02-expression-codegen.md`](c02-expression-codegen.md) | 表达式代码生成 |
| [`c03-type-affinity.md`](c03-type-affinity.md) | 类型亲和性 |
| [`c04-subquery-and-in.md`](c04-subquery-and-in.md) | 子查询与 IN 条件 |
| [`c05-common-table-expression.md`](c05-common-table-expression.md) | 公用表表达式 |
| [`c06-virtual-machine-program-build.md`](c06-virtual-machine-program-build.md) | 虚拟机构建 |

### D-查询规划与执行（9 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-select-compile.md`](d01-select-compile.md) | SELECT 编译 |
| [`d02-where-loop-optimization.md`](d02-where-loop-optimization.md) | WHERE 条件的索引选择 |
| [`d03-join-planning.md`](d03-join-planning.md) | 连接顺序与外连接 |
| [`d04-materialization-and-co-routines.md`](d04-materialization-and-co-routines.md) | 物化与协程式子查询 |
| [`d05-group-by-and-aggregate.md`](d05-group-by-and-aggregate.md) | 分组与聚合 |
| [`d06-order-by-limit.md`](d06-order-by-limit.md) | 排序与 LIMIT |
| [`d07-delete-compile.md`](d07-delete-compile.md) | DELETE 编译 |
| [`d08-update-compile.md`](d08-update-compile.md) | UPDATE 编译 |
| [`d09-insert-compile.md`](d09-insert-compile.md) | INSERT 编译 |

### E-虚拟机与语句执行（5 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-vdbe-execution.md`](e01-vdbe-execution.md) | 虚拟机主循环 |
| [`e02-step-and-finalize.md`](e02-step-and-finalize.md) | step 与 finalize |
| [`e03-column-access.md`](e03-column-access.md) | 结果列读取 |
| [`e04-parameter-binding.md`](e04-parameter-binding.md) | 参数绑定 |
| [`e05-transaction-begin-in-vdbe.md`](e05-transaction-begin-in-vdbe.md) | 虚拟机内的事务与保存点 |

### F-Btree 存储引擎（11 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-btree-open-and-cursor.md`](f01-btree-open-and-cursor.md) | B-tree 打开与游标 |
| [`f02-btree-cell-access.md`](f02-btree-cell-access.md) | B-tree 单元与记录访问 |
| [`f03-btree-insert.md`](f03-btree-insert.md) | B-tree 插入与单元搬移 |
| [`f04-btree-delete-and-rebalance.md`](f04-btree-delete-and-rebalance.md) | B-tree 删除与页合并 |
| [`f05-btree-cursor-movement.md`](f05-btree-cursor-movement.md) | B-tree 游标移动 |
| [`f06-btree-search.md`](f06-btree-search.md) | B-tree 键定位 |
| [`f07-btree-transaction.md`](f07-btree-transaction.md) | B-tree 事务与提交 |
| [`f08-btree-integrity-check.md`](f08-btree-integrity-check.md) | B-tree 完整性检查 |
| [`f09-autovacuum.md`](f09-autovacuum.md) | 自动整理空闲页 |
| [`f10-sqlite-record-format.md`](f10-sqlite-record-format.md) | 记录格式与 varint |
| [`f11-sorter.md`](f11-sorter.md) | 外排排序器 |

### G-分页器与回滚日志（7 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-pager-open-and-roles.md`](g01-pager-open-and-roles.md) | 分页器与回滚模式切换 |
| [`g02-pager-page-cache-ref.md`](g02-pager-page-cache-ref.md) | 分页器的页获取与引用计数 |
| [`g03-rollback-journal.md`](g03-rollback-journal.md) | 回滚日志写入 |
| [`g04-page-writeback.md`](g04-page-writeback.md) | 脏页写回与同步 |
| [`g05-master-journal.md`](g05-master-journal.md) | 主日志与多文件原子提交 |
| [`g06-pager-locking.md`](g06-pager-locking.md) | 分页器文件锁 |
| [`g07-sqlite-file-header.md`](g07-sqlite-file-header.md) | 数据库文件头 |

### H-WAL 预写日志（5 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-wal-mode-and-index.md`](h01-wal-mode-and-index.md) | WAL 模式与索引文件 |
| [`h02-wal-frames.md`](h02-wal-frames.md) | WAL 帧写入与读取 |
| [`h03-wal-checkpoint.md`](h03-wal-checkpoint.md) | WAL 检查点 |
| [`h04-wal-readers.md`](h04-wal-readers.md) | WAL 读事务与快照 |
| [`h05-wal-savepoint-undo.md`](h05-wal-savepoint-undo.md) | WAL 保存点与撤销 |

### I-页缓存（4 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-pcache-lookup.md`](i01-pcache-lookup.md) | 页缓存查找与固定 |
| [`i02-pcache-dirty-tracking.md`](i02-pcache-dirty-tracking.md) | 脏页跟踪 |
| [`i03-pcache-eviction.md`](i03-pcache-eviction.md) | 页缓存淘汰与截断 |
| [`i04-pcache-size-policy.md`](i04-pcache-size-policy.md) | 页缓存容量策略 |

### J-OS 接口与内存管理（7 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-vfs-abstraction.md`](j01-vfs-abstraction.md) | VFS 抽象层 |
| [`j02-os-file-control.md`](j02-os-file-control.md) | 文件控制与共享内存 |
| [`j03-memory-allocator.md`](j03-memory-allocator.md) | 内存分配器 |
| [`j04-mem0-page-allocator.md`](j04-mem0-page-allocator.md) | 小内存块分配器 |
| [`j05-string-formatter.md`](j05-string-formatter.md) | 字符串格式化 |
| [`j06-hash-table.md`](j06-hash-table.md) | 哈希表 |
| [`j07-mutex-and-threading.md`](j07-mutex-and-threading.md) | 互斥与多线程 |

### K-写入路径与约束（5 篇）

| 页文件 | 标题 |
|---|---|
| [`k01-constraint-checks.md`](k01-constraint-checks.md) | 约束检查 |
| [`k02-foreign-keys.md`](k02-foreign-keys.md) | 外键约束 |
| [`k03-triggers.md`](k03-triggers.md) | 触发器 |
| [`k04-generated-columns.md`](k04-generated-columns.md) | 生成列 |
| [`k05-index-maintenance.md`](k05-index-maintenance.md) | 索引维护 |

### L-内置函数与日期时间（5 篇）

| 页文件 | 标题 |
|---|---|
| [`l01-builtin-functions.md`](l01-builtin-functions.md) | 内置标量与聚合函数 |
| [`l02-user-defined-functions.md`](l02-user-defined-functions.md) | 用户自定义函数 |
| [`l03-date-time-functions.md`](l03-date-time-functions.md) | 日期时间函数 |
| [`l04-json-functions.md`](l04-json-functions.md) | JSON 函数 |
| [`l05-like-and-glob-matching.md`](l05-like-and-glob-matching.md) | LIKE 与 GLOB 匹配 |

### M-事务与并发控制（4 篇）

| 页文件 | 标题 |
|---|---|
| [`m01-transaction-state.md`](m01-transaction-state.md) | 事务状态机 |
| [`m02-savepoints.md`](m02-savepoints.md) | 保存点 |
| [`m03-snapshot-isolation.md`](m03-snapshot-isolation.md) | 快照与并发读 |
| [`m04-unlock-notify.md`](m04-unlock-notify.md) | 解锁通知 |

### N-模式演进与附加能力（6 篇）

| 页文件 | 标题 |
|---|---|
| [`n01-alter-table.md`](n01-alter-table.md) | ALTER TABLE |
| [`n02-views.md`](n02-views.md) | 视图 |
| [`n03-attach-database.md`](n03-attach-database.md) | 附加数据库 |
| [`n04-load-extension.md`](n04-load-extension.md) | 可加载扩展 |
| [`n05-pragma-processing.md`](n05-pragma-processing.md) | PRAGMA 处理 |
| [`n06-returning-clause.md`](n06-returning-clause.md) | RETURNING 子句 |

## 关键取舍

SQLite 以单文件与进程内嵌入换取部署简单，代价是并发写受限、无服务端的多连接协调；同一时刻只能有一个写事务（WAL 模式下读不阻塞写）。整库复制以文件级操作为主，牺牲细粒度事务合并换取实现简单。

文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，每一页对应一个可独立验证的机制单元，而不是一个源文件。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `src/main.c` | sqlite3_open, sqlite3_close | 连接建立与关闭 |
| `src/tokenize.c` | sqlite3GetToken | 词法分析 |
| `src/parse.y` | sqlite3Parser | 语法分析 |
| `src/resolve.c` | sqlite3ResolveExprNames | 名称解析 |
| `src/expr.c` | sqlite3ExprCodeTarget | 表达式代码生成 |
| `src/select.c` | sqlite3Select | SELECT 编译 |
| `src/vdbe.c` | sqlite3VdbeExec | 字节码解释执行 |
| `src/btree.c` | sqlite3BtreeOpen, sqlite3BtreeTableMoveto | B-tree 游标 |
| `src/pager.c` | sqlite3PagerGet, sqlite3PagerWrite | 分页器读页与加写标记 |
| `src/wal.c` | sqlite3WalOpen, walCheckpoint | WAL 打开与检查点 |
| `src/os.c` | sqlite3OsRead, sqlite3OsWrite | VFS 文件读写 |

## 参考

- [SQLite 官方文档：文件格式](https://www.sqlite.org/fileformat.html)
- [SQLite 官方文档：WAL 模式](https://www.sqlite.org/wal.html)
- [SQLite 官方文档：隔离与并发](https://www.sqlite.org/isolation.html)
- [SQLite 官方文档：源码结构](https://www.sqlite.org/code.html)
- [SQLite 官方源码仓库](https://github.com/sqlite/sqlite)

## 与主核 / AI 的关系

本仓主数据库内核是从零实现的多模态数据库；SQLite 页面用于机制对照，比较时应明确分页器与存储层的分层边界、以及单文件嵌入式设计在并发与崩溃恢复上的取舍差异。AI 生成的导航不能代替对指定 checkout 源码及版本的核对。
