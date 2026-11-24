---
project: mysql-server
page_type: overview
display_name: MySQL Server
repo: https://github.com/mysql/mysql-server.git
license: GPL-2.0
language: C++
stars: 12522
stars_as_of: 2026-11-23
refs:
  - path: sql/sql_parse.cc
    symbols: [do_command, dispatch_command]
  - path: sql/sql_class.cc
    symbols: [THD::sql_parser]
  - path: sql/sql_optimizer.cc
    symbols: [JOIN::optimize, JOIN::make_join_plan]
  - path: sql/sql_select.cc
    symbols: [Sql_cmd_dml::execute, JOIN::exec]
  - path: sql/handler.cc
    symbols: [ha_open, handler::ha_write_row]
  - path: storage/innobase/btr/btr0cur.cc
    symbols: [btr_cur_search_to_nth_level]
  - path: storage/innobase/buf/buf0buf.cc
    symbols: [buf_page_get_gen, buf_pool_get]
  - path: storage/innobase/trx/trx0trx.cc
    symbols: [trx_commit_for_mysql]
  - path: sql/log_event.cc
    symbols: [Log_event::write]
  - path: sql/rpl_replica.cc
    symbols: [exec_relay_log_event, start_slave]
---

# MySQL Server

## 一句话职责

MySQL Server 是以 InnoDB 为默认存储引擎的开源关系型数据库管理系统，兼作客户端-服务器协议的服务端。

## 机制是什么

MySQL Server 以前台进程承载连接，命令经分派后进入解析、计划与执行三层。解析阶段由词法与文法分析产出解析树，再转换为可求值的 Item；优化器在候选索引与连接顺序之间按代价取舍，产出访问路径；执行器把访问路径落到行迭代器上驱动取行。InnoDB 侧以 B-tree 组织聚簇索引与二级索引，缓冲池缓存页，redo 日志保证崩溃恢复，MVCC 与行锁共同支撑事务隔离。binlog 记录变更并支撑复制，GTID 提供复制定位。

本文库按经审阅的机制单元组织：**14 个机制领域、共 67 篇机制页**，每个机制页对应一个可独立理解、可独立验证的机制单元，而不是一个源文件。

## 怎么实现的

下表的 14 个领域按下面的顺序排列：A 连接与命令分派 → B 解析与词法 → C 优化器与执行计划 → D 执行器与行迭代 → E 临时表与排序 → F 存储引擎接口 → G InnoDB 索引与页 → H InnoDB 缓冲池 → I InnoDB 日志与恢复 → J InnoDB 事务与锁 → K InnoDB 行操作与可见性 → L 二进制日志 → M 复制与 GTID → N 权限与认证。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

## 机制导航（67 篇 / 14 组）

### A-连接与命令分派（5 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-connection-dispatch.md`](a01-connection-dispatch.md) | 连接与命令分派 |
| [`a02-thd-lifecycle.md`](a02-thd-lifecycle.md) | THD 线程对象生命周期 |
| [`a03-prepare-execute-protocol.md`](a03-prepare-execute-protocol.md) | 预处理语句的两阶段执行 |
| [`a04-change-db.md`](a04-change-db.md) | USE 与当前库切换 |
| [`a05-error-diagnostics.md`](a05-error-diagnostics.md) | 错误码与诊断信息 |

### B-解析与词法（5 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-lexer.md`](b01-lexer.md) | 词法分析器 |
| [`b02-grammar-parser.md`](b02-grammar-parser.md) | 语法分析器 |
| [`b03-parse-tree-items.md`](b03-parse-tree-items.md) | 解析树到 Item 的转换 |
| [`b04-name-resolution.md`](b04-name-resolution.md) | 名称解析与列绑定 |
| [`b05-parse-tree-hints.md`](b05-parse-tree-hints.md) | 优化器提示的解析 |

### C-优化器与执行计划（6 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-join-optimize.md`](c01-join-optimize.md) | JOIN 的整体优化 |
| [`c02-join-plan.md`](c02-join-plan.md) | 连接计划与表访问顺序 |
| [`c03-index-selection.md`](c03-index-selection.md) | 索引与范围访问选择 |
| [`c04-subquery-transform.md`](c04-subquery-transform.md) | 子查询改写 |
| [`c05-derived-table-materialization.md`](c05-derived-table-materialization.md) | 派生表与 CTE 的物化 |
| [`c06-partition-pruning.md`](c06-partition-pruning.md) | 分区裁剪 |

### D-执行器与行迭代（5 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-query-execution.md`](d01-query-execution.md) | 查询执行入口 |
| [`d02-row-iterator.md`](d02-row-iterator.md) | 行迭代器框架 |
| [`d03-table-and-index-scan.md`](d03-table-and-index-scan.md) | 全表扫描与索引扫描 |
| [`d04-filter-and-aggregate.md`](d04-filter-and-aggregate.md) | 过滤与聚合迭代 |
| [`d05-join-execution.md`](d05-join-execution.md) | 连接执行 |

### E-临时表与排序（4 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-temp-table.md`](e01-temp-table.md) | 内部临时表 |
| [`e02-filesort.md`](e02-filesort.md) | 文件排序 |
| [`e03-limit-clause.md`](e03-limit-clause.md) | LIMIT 与行数截断 |
| [`e04-join-buffer.md`](e04-join-buffer.md) | 连接缓冲 |

### F-存储引擎接口（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-handler-api.md`](f01-handler-api.md) | handler 存储引擎接口 |
| [`f02-table-open-cache.md`](f02-table-open-cache.md) | 表打开与表缓存 |
| [`f03-metadata-lock.md`](f03-metadata-lock.md) | 元数据锁 |
| [`f04-dd-data-dictionary.md`](f04-dd-data-dictionary.md) | 数据字典 |

### G-InnoDB 索引与页（5 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-btr-cursor-search.md`](g01-btr-cursor-search.md) | B-tree 游标定位 |
| [`g02-btr-insert-split.md`](g02-btr-insert-split.md) | B-tree 插入与页分裂 |
| [`g03-page-directory.md`](g03-page-directory.md) | 页目录与记录定位 |
| [`g04-compact-record.md`](g04-compact-record.md) | 紧凑记录格式 |
| [`g05-record-compare.md`](g05-record-compare.md) | 记录比较 |

### H-InnoDB 缓冲池（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-buf-page-get.md`](h01-buf-page-get.md) | 缓冲池取页 |
| [`h02-buf-flush.md`](h02-buf-flush.md) | 脏页刷新 |
| [`h03-buf-lru-eviction.md`](h03-buf-lru-eviction.md) | LRU 淘汰 |
| [`h04-buf-pool-instance.md`](h04-buf-pool-instance.md) | 缓冲池多实例 |

### I-InnoDB 日志与恢复（4 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-redo-log-write.md`](i01-redo-log-write.md) | redo 日志写入 |
| [`i02-log-checkpoint.md`](i02-log-checkpoint.md) | 检查点 |
| [`i03-crash-recovery.md`](i03-crash-recovery.md) | 崩溃恢复 |
| [`i04-file-io.md`](i04-file-io.md) | 文件 I/O 封装 |

### J-InnoDB 事务与锁（6 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-trx-lifecycle.md`](j01-trx-lifecycle.md) | 事务对象生命周期 |
| [`j02-row-lock.md`](j02-row-lock.md) | 行锁与锁等待 |
| [`j03-lock-latch.md`](j03-lock-latch.md) | 闩锁 |
| [`j04-read-view.md`](j04-read-view.md) | 读视图 |
| [`j05-undo-log.md`](j05-undo-log.md) | undo 日志 |
| [`j06-group-commit.md`](j06-group-commit.md) | 组提交 |

### K-InnoDB 行操作与可见性（4 篇）

| 页文件 | 标题 |
|---|---|
| [`k01-row-insert.md`](k01-row-insert.md) | 插入行 |
| [`k02-row-update.md`](k02-row-update.md) | 更新行 |
| [`k03-row-delete.md`](k03-row-delete.md) | 删除行 |
| [`k04-mvcc-visibility.md`](k04-mvcc-visibility.md) | MVCC 可见性判定 |

### L-二进制日志（5 篇）

| 页文件 | 标题 |
|---|---|
| [`l01-binlog-event-write.md`](l01-binlog-event-write.md) | binlog 事件写入 |
| [`l02-binlog-statement-vs-row.md`](l02-binlog-statement-vs-row.md) | 语句格式与行格式 |
| [`l03-binlog-cache-flush.md`](l03-binlog-cache-flush.md) | binlog 缓存与刷盘 |
| [`l04-binlog-dump-source.md`](l04-binlog-dump-source.md) | binlog 发送端 |
| [`l05-binlog-rotation.md`](l05-binlog-rotation.md) | binlog 轮转与清理 |

### M-复制与 GTID（6 篇）

| 页文件 | 标题 |
|---|---|
| [`m01-replica-io-thread.md`](m01-replica-io-thread.md) | 从库 IO 线程 |
| [`m02-replica-applier.md`](m02-replica-applier.md) | 从库应用线程 |
| [`m03-gtid-set.md`](m03-gtid-set.md) | GTID 集合 |
| [`m04-gtid-allocation.md`](m04-gtid-allocation.md) | GTID 分配与持久化 |
| [`m05-replication-filter.md`](m05-replication-filter.md) | 复制过滤规则 |
| [`m06-write-set-tracking.md`](m06-write-set-tracking.md) | 写集跟踪 |

### N-权限与认证（4 篇）

| 页文件 | 标题 |
|---|---|
| [`n01-authentication.md`](n01-authentication.md) | 认证插件 |
| [`n02-privilege-check.md`](n02-privilege-check.md) | 权限判定 |
| [`n03-role-model.md`](n03-role-model.md) | 角色模型 |
| [`n04-perfschema.md`](n04-perfschema.md) | 性能监控 |

## 关键取舍

MySQL 用 handler 接口把 server 层与存储引擎解耦，代价是引擎必须适配一套约定良好的回调面，InnoDB 之外的引擎实现成本不低。优化器以代价模型换取可预测的查询计划，但统计信息陈旧时可能选错路径。

文档层面沿用与 PostgreSQL 相同的取舍：**机制粒度优先于文件粒度**，每一页对应一个可独立验证的机制单元，而不是一个源文件。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `sql/sql_parse.cc` | do_command, dispatch_command | 命令分派 |
| `sql/sql_class.cc` | THD::sql_parser | 会话线程对象 |
| `sql/sql_optimizer.cc` | JOIN::optimize, JOIN::make_join_plan | 优化器编排与连接计划 |
| `sql/sql_select.cc` | Sql_cmd_dml::execute, JOIN::exec | 查询执行入口与连接执行 |
| `sql/handler.cc` | ha_open, handler::ha_write_row | 存储引擎接口 |
| `storage/innobase/btr/btr0cur.cc` | btr_cur_search_to_nth_level | B-tree 游标定位 |
| `storage/innobase/buf/buf0buf.cc` | buf_page_get_gen, buf_pool_get | 缓冲池取页 |
| `storage/innobase/trx/trx0trx.cc` | trx_commit_for_mysql | 事务提交 |
| `sql/log_event.cc` | Log_event::write | binlog 事件写入 |
| `sql/rpl_replica.cc` | exec_relay_log_event, start_slave | 复制应用 |

## 参考

- [MySQL 官方文档：8.0 参考手册](https://dev.mysql.com/doc/refman/8.0/en/)
- [MySQL 官方文档：InnoDB 内部机制](https://dev.mysql.com/doc/refman/8.0/en/innodb-internal-architecture.html)
- [MySQL 官方文档：复制](https://dev.mysql.com/doc/refman/8.0/en/replication.html)
- [MySQL 官方源码仓库](https://github.com/mysql/mysql-server)

## 与主核 / AI 的关系

本仓主数据库内核是从零实现的多模态数据库；MySQL 页面用于机制对照，比较时应明确 SQL 层与存储层的接口边界、锁模型与日志语义——MySQL 的 binlog 与 InnoDB redo 处在不同层次，不能混谈。AI 生成的导航不能代替对指定 checkout 源码及版本的核对。
