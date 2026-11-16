# src/db 工程面资产分级清单（Stage B1）

盘点日期：2026-11-16。**只读**：读源码、CMake、测试文件。未修改代码、未运行构建或测试。

分级标准：
- **真实可用**：有实现且有测试覆盖其行为
- **真实未验证**：有实际实现，但本次未运行其测试 / 无独立行为测试
- **stub / 未接线**：关键路径是空实现、固定返回值或 TODO
- **深度耦合**：依赖 src/db 的私有存储/事务/向量内部类型，无法直接搬

## 一、结论速览

| 模块 | 分级 | 一句话 |
|---|---|---|
| `protocol/pg_wire_protocol.c` | **stub** | 报文编解码可参考；`pg_handle_query` 发空结果 |
| `protocol/mysql_wire_protocol.c` | **stub** | `mysql_handle_query` 直接 return 0 |
| `protocol/analytics_sql.c`、`sql_compat.c` | 真实未验证 | 纯字符串/方言映射，无内核依赖 |
| `sql/mmdb_sdk.c` | **stub** | 发表文 SQL，不读响应，返回空游标 |
| `api/rest_api.c`、`handlers.c` | **深度耦合** | HTTP 路由本身自包含，但业务直接绑 VectorAPI/BM25 |
| `api/vdb_api.c`、`api/vector_api.c` | **深度耦合** | 直连 vector WAL、checkpoint manager、私有查询计划类型 |
| `core/db_server.c` | **stub** | 有 socket bind/listen/accept，但 `server_execute_sql` 是空实现 |
| `executor/rag/*` | 混合 | 框架真实（`void*` 隔离）；检索/embedding/generate 是占位或 TODO |
| `distributed/percolator`、`mvcc_ts`、`tso`、`txn` | 真实未验证 | 纯内存逻辑（850/395/220 行），零 TODO，但有测试未运行 |
| `consensus/raft.c`、`raft_cluster.c` | 真实未验证 | 状态机与持久化真实（fopen/fwrite） |
| `consensus/raft_transport.c` | **stub** | `impl_send_request_vote` 返回伪造成功（`vote_granted = true`） |
| `distributed/query/distributed_query.c` | **stub** | `execute_fragment_simulation` |
| `sharding/migrate_manager.c` | **深度耦合** | 直接用 src/db KV 的 open/scan/put/delete |

## 二、逐项证据

### 2.1 协议层

**`pg_wire_protocol.c`（stub）**

```c
/* pg_wire_protocol.c:369-373 */
/* 调用执行器（这里先用 stub 实现） */
/* TODO: 接入实际的 SQL 执行器 */
pg_send_empty_query_response(conn);
```

`pg_handle_parse`(:390)、`pg_handle_execute`(:430) 同样 TODO，后者直接发 `"SELECT 0"`。头文件 `pg_wire_protocol.h` 只含 stdbool/stdint/stddef，连接体预留 `void *executor_data`(:193) —— 说明设计时已打算解耦，但从未接上。

**`mysql_wire_protocol.c`（stub）**

```c
/* mysql_wire_protocol.c:301-304 */
/* TODO: 集成实际的 SQL 执行引擎 */
/* 这里返回一个示例空结果集 */
return 0;
```

**可迁移部分**：两文件的消息编解码、长度/分帧校验逻辑可作为 codec 参考（`raft_transport_test.c` 显示该仓有编解码单测的传统）。但**执行链路必须新建**。

**`sql_compat.c` / `analytics_sql.c`（真实未验证）**：纯方言映射与字符串处理，`return 0` 多为正常的成功返回而非桩。这类不依赖内核，迁移成本最低。

### 2.2 SDK

**`sql/mmdb_sdk.c`（stub）**

```c
/* mmdb_sdk.c:221 */
socket_send(...)  /* 发明文 SQL */
/* mmdb_sdk.c:242 */
/* TODO: 实际接收查询结果 */
```

只 include `db/sql/mmdb_sdk.h`（仅 stdint/stdbool）+ POSIX socket。**没有实现任何 wire 协议**，事务 API 只是发 `"BEGIN"/"COMMIT"` 字符串（:754-764）。

### 2.3 REST / 向量 API（深度耦合）

`rest_api.c`、`handlers.c` 的路由层（`rest_api.c:296-529`）自包含，但业务处理直接持有 `VectorAPI *vector_api`(:100)、`vdb_handle_t *vdb`(:103)、BM25 映射(:106)，并调 `vector_api_create_collection`、`vector_api_insert` 等（`handlers.c:204/319/463/522/587`）。

`vdb_api.c` 直接调 `filter_parse`(:501)、`fusion_execute`(:646)，include 了 `storage/vector/vector_persist.h`(:12)。
`vector_api.c` 直接操作 `vector_wal_open`(:303)、`vector_wal_append`(:608)、`vector_wal_checkpoint`(:1057)、`checkpoint_manager_create`(:312)，字段是 `vector_wal_t *wal`(:64)、`CheckpointManager *checkpoint_mgr`(:65)。

公开头 `include/db/api/vector_api.h:21` 就直接 `#include <db/core/vector_query.h>`，把 `VectorQueryPlan` 等内部类型暴露给 API 使用者 —— 这是耦合无法通过"只换实现"绕开的证据。

**全目录 grep `execute_sql`/`sql_driver` 零命中**：REST 不是 SQL 面。

### 2.4 core/db_server.c（stub）

有真实 socket bind/listen/accept 与线程分派，但：

```c
/* db_server.c:344-350 */
int server_execute_sql(const char *sql, char *output, size_t output_len) {
    (void)sql; (void)output; (void)output_len;
    LOG_INFO("执行 SQL (stub): %s", sql);
    return 0;
}
```

**只能参考监听骨架**，不能构成可执行 SQL 的服务器。

### 2.5 RAG（框架真实，算子占位）

| 文件 | 状态 | 证据 |
|---|---|---|
| `rag_executor.h` | 真实 | 上下文里 Catalog/BufferPool/WAL 全是 `void*`(:101-109)；算子表是私有 init/execute/cleanup(:133-150)，与 executor 的 PlanState 零交集 |
| `embed_op.c` | **占位** | `TODO: 使用 libcurl 调用 Ollama API`(:356)；`processing_time_ms = 0`(:510) |
| `generate_op.c` | **占位** | `Simple: 关键词模拟生成`(:6)；`TODO: 使用 libcurl 调用 Ollama API`(:127,147) |
| `graphrag_hybrid_search.c` | **占位** | `当前使用模拟结果`(:100)；`TODO: 集成真实图遍历 API`(:150) |
| `retrieve_op.c` | **占位** | `hnsw_results_count = 0`(:115-122) |
| `rag_pipeline.c` | 混合 | 管道装配真实；`模拟检索（用于测试）`(:147) |

**可迁移部分**：算子框架与上下文契约（`void*` 隔离做得好），以及 pipeline 装配逻辑。**不可迁移**：任何声称"能检索/能生成"的部分。

### 2.6 分布式 / 共识

| 模块 | 行数 | TODO | 分级 |
|---|---|---|---|
| `distributed/percolator/*`（3 文件） | 850 | 0 | 真实未验证 |
| `distributed/mvcc_ts/mvcc_ts.c` | 395 | 0 | 真实未验证 |
| `distributed/tso/*`（3 文件） | 220 | 0 | 真实未验证 |
| `consensus/raft.c` + `raft_cluster.c` | — | — | 真实未验证；持久化用 `fopen/fwrite`(:634,670)，不碰 WAL |
| `consensus/raft_transport.c` | — | 多处 | **stub** |
| `distributed/query/distributed_query.c` | — | — | **stub**（`execute_fragment_simulation`:280） |
| `distributed/txn/distributed_txn.c` | — | 6 处 | 状态机真实，RPC 与状态持久化 TODO(:184/207/224/281/490/501) |

`raft_transport.c` 的关键证据：

```c
/* raft_transport.c:270-290 */
/* 暂时返回模拟响应 */
result->term = args->term;
result->vote_granted = true;
return 0;
```

**它报告"投票成功"但从未发包**。配套测试 `test/db/consensus/raft_transport_test.c` 只测编解码（`test_encode_decode_request_vote`），`raft_log_replication_test.cpp:6` 明写"用 in-process transport（直接函数调用）"。

### 2.7 sharding

`sharding/migrate_manager.c:7-8` include `db/storage/kv/kv.h`，直接调 `kv_open/kv_scan/kv_get/kv_put/kv_delete/kv_flush`(:320-508, 810-889)。而 `kv.h:10-12` 又拉入 `db/buffer.h`、`db/wal.h`、`db/lock.h`。**这是唯一的硬耦合点**，其余分片路由/负载均衡只 include 自己的头。

## 三、按迁移价值排序的建议

| 优先级 | 模块 | 理由 | 前置条件 |
|---|---|---|---|
| 1 | `distributed/percolator`、`mvcc_ts`、`tso` | 零 TODO、纯内存逻辑、不碰内核私有类型 | 先运行其 4 个测试确认通过 |
| 2 | `consensus/raft.c` 状态机 | 自持状态 + 文件持久化，可平移 | 先运行 5 个 raft 测试 |
| 3 | `sql_compat.c`、`analytics_sql.c` | 纯字符串映射 | 无 |
| 4 | RAG 算子框架 + pipeline 装配 | `void*` 边界清晰 | **不带** embed/generate/retrieve 的占位实现 |
| 5 | 协议 codec | 报文编解码可复用 | **执行链路须新建** |
| 不迁 | `pg/mysql_handle_query`、`mmdb_sdk`、`db_server`、`raft_transport`、`distributed_query`、REST/vector_api、`migrate_manager` | stub 或深度耦合 | 需要时在主核重新设计 |

## 四、未验证项说明

本清单判定"真实未验证"的模块（percolator/mvcc_ts/tso/raft）**没有在本次运行任何测试**。分级依据只有源码内容（零 TODO、有实际数据结构与算法）。要迁入主核前必须先在隔离构建目录运行其测试，确认测试本身不是空断言。

同理，本清单判定"stub"的模块依据是显式 TODO 注释与固定返回值，这是强证据；但"未验证"不等于"能用"。
