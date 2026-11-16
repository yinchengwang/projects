# Stage B2 决策：继续嵌入式，不建网络服务层

决策日期：2026-11-16。用户选择 **继续嵌入式**：`engine/db` 保持 C API + REPL，不新建 socket 监听、会话管理、认证层。

## 一、决策含义

| 项 | 结论 |
|---|---|
| 服务形态 | 嵌入式库 + REPL，进程内使用 |
| 网络协议 | **不做**：Wire protocol handler、监听器、连接管理均不建设 |
| 认证 / ACL | **不做** |
| 跨进程文件互斥 | **不做**（单进程独占假设保持） |
| 并发模型 | 保持现状：语句级域锁 + 模态全局 recursive mutex |
| 硬上限 | `TXN_MAX_ACTIVE=64`、`SNAP_MAX_XIP=128` 保持为已知限制，不在本阶段扩张 |
| 分布式 | **不做**：DESIGN.md:24 已列为非目标 |

因此 `src/db` 里的 `pg_wire_protocol`、`mysql_wire_protocol`、`mmdb_sdk`、`core/db_server`、`rest_api`、`vdb_api`、`vector_api` **全部不迁移**。它们的价值只剩「报文编解码参考」和「分布式逻辑承载者」。

## 二、本阶段实际可做的事

Route B 下唯一主核是 `engine/db`。在「不做服务」前提下，可迁移的是**不依赖网络、不依赖 src/db 私有存储**的纯逻辑能力：

| 候选 | 价值 | 前置 |
|---|---|---|
| `distributed/percolator` + `mvcc_ts` + `tso` | 提供 2PC / 时间戳 / 多版本能力，可服务未来多节点或本地事务增强 | 先跑 `distributed_txn_test` |
| `consensus/raft.c` + `raft_cluster.c` | Raft 状态机与日志复制逻辑 | 先跑 `raft_*` 五个测试 |
| `sql_compat.c` / `analytics_sql.c` | 方言映射，无内核依赖 | 直接评估 |
| RAG 算子框架（`void*` 契约 + pipeline 装配） | 框架清晰，可做本地检索编排 | **不带** embed/generate/retrieve 的占位实现 |

**不建议迁**：协议 codec（没有服务就不需要）、raft_transport（stub）、distributed_query（模拟）、migrate_manager（旧 KV 耦合）。

## 三、`engine/db` 自身在嵌入式场景下的已知限制

这些都是继续使用嵌入式形态时必须诚实记录的边界，不能因为「选了它做主核」就当作没有：

| 限制 | 证据 | 影响 |
|---|---|---|
| SQL 子集窄 | `query/parser.c:9` 只支持 `SELECT ... FROM name [WHERE] [LIMIT]`；无 JOIN / ORDER BY | 复杂查询不支持 |
| 单条 INSERT 最多 16 行 | `parser.h:11` `SQL_MAX_ROWS=16`，`parser_stmt.c:208` 强制检查 | 批量插入需多条语句 |
| 单表最多 16 列 | `parser.h:10` `SQL_MAX_COLS=16` | 宽表受限 |
| **注意：结果集行数不受 `SQL_MAX_ROWS` 限制** | `exec.c:105` 用 `b_grow()` 动态翻倍扩容；`SQL_MAX_ROWS` 只约束 INSERT 的 `vals[][]` | 我先前把 16 当作结果集上限是错的，此处更正 |
| 结果集全字符串化 | `query/exec.h:50-51`，`value.c:346-347` 把 NULL 写成字面 `"NULL"` | **NULL 与字符串 `"NULL"` 无法区分**，是真实的接口缺陷 |
| 外部值走 detoast | `exec.c:117-133` 单列物化，不截断 | 大 blob 列输出成本高 |

`engine/db` 的 `db_result_value` 对越界/空指针返回空字符串而非 NULL（`db_result.c:49-55`），调用方无法区分「空串」与「越界」。这在嵌入式使用中同样是需要注意的。

## 四、下一步（B3 的首个垂直切片建议）

在「不做服务」前提下，真正的缺口是**对外数据接口的保真度**。建议首个切片选：

**NULL 语义保真**：在 `include/db.h` 增加 `int db_result_is_null(const Result*, int row, int col)`，让调用方能区分 NULL 与字面 `"NULL"` 字符串。

理由：
- 这是嵌入式场景下**每个调用方都会碰到**的缺陷，且不需要迁任何 src/db 代码
- 改动面小（结果集构造时记录 null 位图 + 一个访问器），符合「单文件不超过 800 行」与 `-Werror` 门禁
- 可用 TDD：先写测试断言「列值为 NULL 时 `db_result_is_null` 返回 1，且 `db_result_value` 仍返回 "NULL"（保持向后兼容）」，观察 RED，再实现

如果用户认为该切片太小，备选是 **INSERT 批量的 16 行上限** 或 **Raft 状态机迁入**。前者是主核能力扩张（有回归风险），后者是跨目录迁移（需先跑通五个 raft 测试）。

本文件是决策记录，不授权任何代码改动。
