> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Graph 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/graph/`（~3.4K 行）+ `engine/src/db/graph/`（~5.2K 行）+ `executor/graph/`（含 Cypher/gqlExec/traverse）

## 1. 实现现状盘点

### 1.1 模块清单

| 层 | 模块 | 关键文件 | 行数 |
|----|------|---------|------|
| 存储 | CSR + COO 混合 + 标签索引 | `storage/graph/graph_csr.c` | 625 |
| 存储 | 图引擎 | `storage/graph/graph_engine.c` | 740 |
| 存储 | 图遍历 | `storage/graph/graph_traverse.c` | 404 |
| 存储 | 图索引 | `storage/graph/graph_index.c` | 481 |
| 存储 | 图存储 | `storage/graph/graph_store.c` | 1120 |
| 算法 | 图算法库（P1-4） | `graph/graph_algorithms.c` | 1124 |
| 算法 | 算法统计/池 | `graph/graph_algo.c` | 974 |
| 属性 | 图属性 | `graph/graph_property.c` | 986 |
| 索引 | 图索引 | `graph/graph_index.c` | 679 |
| 查询 | Cypher | `graph/graph_cypher.c` | 1024 |
| SQL 集成 | 图 SQL 函数 | `graph/graph_sql_functions.c` | 424 |
| 执行 | 图执行 | `executor/graph/gqlExec.c`、`traverse/traverse.c`（709） | ~1100 |

### 1.2 测试覆盖

P1-4 图分析算法库 commit `0b4ff2090` 完成，含 dijkstra + connected_components；Cypher 修复 commit `a37a6e396`。具体测试文件未逐一统计（参照旧对比文档 + 提交记录）。

### 1.3 重要修正（相对 8 月 25 日旧对比文档）

旧文档称"4 个算法（BFS/DFS/最短路径/PageRank）"——**不准确**。`graph/graph_algorithms.c` 实际实现 7 个：BFS / DFS / BFS 最短路径 / Dijkstra / PageRank / 连通分量 / 统计（行 175/344/516/574/678/903/1055）。P1-4 新增 Dijkstra 与连通分量。但与 Memgraph MAGE 100+ 算法库仍有 90+ 差距。

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：CSR 全文件零锁——realloc 引发与 Vector 相同的 UAF 风险「确认·实现质量缺陷」**

`storage/graph/graph_csr.c`（625 行）`grep -E 'mutex|lock|spinlock|readwrite'` 零命中；`grow_vertices`（:46-53）与 `grow_edges`（:58-64）均直接 `realloc` 后修改结构体指针。`graph_traverse.c`（404 行）读取 `csr->offsets/edges/in_edges` 时无锁——并发「遍历中插入触发扩容」导致遍历线程读到已释放内存。

**缺陷 2：图引擎层无事务/快照「确认·实现质量缺陷」**

`storage/graph/graph_engine.c`（740 行）grep 无任何锁原语。顶点/边操作不入事务——与 Relational 同病（见 02-relational-gap 2.1）：写入过程中断不会回滚。

### 2.2 崩溃恢复

**缺陷 1：CSR 增量写入用 COO 缓冲但缺少崩溃后未 compact 的恢复路径「疑似·实现质量缺陷」**

`graph_csr.c:111-115` 用 COO 增量缓冲（默认容量 `GRAPH_CSR_COO_CAPACITY`），`:427` 的 `csr_compact` 触发 CSR 重建。`csr_open`（:131+）加载流程需核实：崩溃恢复时若 CSR 落后于 COO（未 compact），重启后是从哪个数据重建？未读 `csr_open` 全文，待运行时验证。

**缺陷 2：`graph_csr_save`（:206）持久化缺 fsync「疑似·实现质量缺陷」**

未读到 fsync/FlushFileBuffers 调用，与 Vector WAL 同样 fflush-only 的持久化语义缺口。需阅读 `:206` 后续行确认。

### 2.3 内存安全

**缺陷 1：`grow_edges` 失败时 `csr->edges` 可能指向已释放内存「确认·实现质量缺陷」**

`graph_csr.c:59-63`：`realloc` 失败返回 NULL 时未保留旧指针（实际上 realloc 失败原指针有效），但 `:60-62` 直接 return -1，旧指针仍可用——这一点实际安全（realloc 失败语义）。误判撤销。但缺少 `grow_edges` 失败后 `csr->edges_capacity` 是否更新的检查——若调用方在 realloc 失败后继续使用旧的 capacity 假设，可能再 overflow。需核实。建议小补 TODO 标记。

**正面证据**：`graph_csr_destroy`（:268）负责释放 vertices/edges/offsets/in_edges/labels/label_index/coo，路径完整；`graph_csr_create` 失败时的逐步回滚（:89-100）正确。

### 2.4 错误处理

**缺陷 1：算法库返回码处理不一致「确认·实现质量缺陷」**

`graph_algorithms.c` 7 个算法均返回 `int`（0=成功/-1=失败），但 `graph_path_result_clear` 等 `*_result_clear` 函数（:1085/1094/1106/1117）返回 void——调用方无法分辨"无结果"与"结果已部分清理"，容易重复释放。Cypher 层若不查 caller 可能在错误路径中重复 clear 同一结果。

**缺陷 2：Cypher 解析器对开放标准的覆盖「确认·功能缺失」**

`graph_cypher.c` 1024 行实现 Cypher 子集，与 openCypher TC（Neo4j/TigerGraph 跟踪）的核心测试套件相比：路径模式（变长模式 `*1..5`）、OPTIONAL MATCH、列表/模式推导、复合（UNWIND/CALL）-GQL 标准未达成。功能差距，非质量缺陷。

### 2.5 算法实现质量

**正面证据 1：CSR + COO 增量混合是 Neo4j 之外的合理工程取舍「确认」**

写热路径进 COO 缓冲（`graph_csr.c:111-115`），定期 `csr_compact`（:427）重建 CSR——相对 Neo4j CSStore 的 property chain 避免了"链长爆涨 + 部分重写"的复杂度，是工程上的正确简化。

**正面证据 2：7 个核心算法实现完成度尚可「确认」**

Dijkstra、BFS 最短路径、DFS、BFS、PageRank、连通分量、统计覆盖图分析最常用子集；P1-4 完成增量为正向项。

**缺陷 1：算法库规模与业界差距巨大「确认·功能缺失」**

自实现 7 vs Memgraph MAGE 100+、Neo4j GDS 60+、TigerGraph GSQL Algorithms 50+。社区发现（Louvain/Leiden）、中心性（betweenness/closeness/katz）、图嵌入（node2vec/GraphSAGE）、路径枚举——全部缺失。GQL/TC 子集达标但远低于业界。

**缺陷 2：PageRank 未确认对悬挂节点（dangling node）的处理「疑似·实现质量缺陷」**

PageRank 的标准实现需将悬挂节点（无出边）的 PageRank 质量按均匀分布返回全图（dangling mass redistribution）。`graph_pagerank_new`（:678-902）需阅读 ~225 行核实；未运行时验证。

### 2.6 API 设计

**正面证据**：Cypher + SQL 函数 + 图执行器三层组织清晰；与关系模态共享 SQL 表面（`graph_sql_functions.c`）。

**缺陷 1：无 GQL/ISO 标准兼容声明「确认·功能缺失」**

openCypher 子集实现但未声明合规等级。Neo4j 是 GQL TC 核心成员，自研版本与 GQL 标准不接轨，迁移成本高。

## 3. 业界标杆对比

| 维度 | 自实现 | Neo4j | TigerGraph | NebulaGraph | Memgraph |
|------|--------|-------|------------|-------------|----------|
| 存储 | CSR + COO | CSStore (native property chain) | CSR 压缩邻接 | CSR + Partition Raft | CSR + WAL |
| 查询语言 | Cypher 子集 | Cypher + GQL 草案 | GSQL | nGQL (Cypher 子集) | openCypher |
| 算法数 | 7 | 60+ (GDS) | 50+ | 40+ | 100+ (MAGE) |
| 分布式 | 单机 | Causal Cluster | 完整分布式 | Storage/Query 分离 | G-Alpha |
| 持久化 | CSR save + COO buffer | Transaction Log + Checkpoint | WAL + Checkpoint | RocksDB WAL + Checkpoint | AOF + RDB |
| 事务 | 无 ACID | 单写 ACID | 严格分布式 ACID | 单 Partition ACID | MVCC 快照隔离 |
| GraphRAG | 无 | 原生集成 | GraphML/AI Agent | 集成 | 原生 |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 3 | CSR 全文件零锁 `graph_csr.c`；图引擎无事务 `graph_engine.c` |
| 崩溃恢复 | 4 | CSR save fsync 待核 `graph_csr.c:206`；COO 未 compact 恢复路径待核 |
| 内存安全 | 6 | destroy 路径完整 `graph_csr.c:268`；create 失败回滚正确 `:89-100` |
| 错误处理 | 5 | 算法返回码与 void clear 不一致 `graph_algorithms.c:1085+`；Cypher 覆盖面有限 |
| 算法实现质量 | 4 | 7 vs 业界 50-100+；CSR+COO 工程合理；PageRank 悬挂节点处理待验 |
| API 设计 | 5 | Cypher+SQL 函数+执行器组织清晰；GQL 标准未声明 |

**实现质量缺陷清单（5 项确认 + 2 项疑似）**：
1. CSR/图引擎无锁 + realloc UAF 风险（并发）
2. 图操作无事务包裹（并发/崩溃）
3. 持久化 fsync 待核（疑似，崩溃）
4. CSR 崩溃后 COO 未 compact 恢复路径待核（疑似）
5. 算法错误码与 void clear 不一致（错误处理）
6. PageRank 悬挂节点处理待验（疑似）
7. Cypher/openCypher 标准覆盖度（功能缺失）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | CSR + 图引擎读写锁（RCU 或 copy-on-write） | 实现质量缺陷 | M |
| P0 | 图操作纳入事务（与 02 relational 共用 txn 系统） | 实现质量缺陷 | M |
| P1 | PageRank 悬挂节点处理复核 + 单元测试 | 实现质量缺陷 | S |
| P1 | 算法结果 clear 与错误码规范化 | 实现质量缺陷 | S |
| P1 | CSR save/fsync + 崩溃恢复路径明确化 | 实现质量缺陷 | M |
| P2 | Cypher/openCypher 核心测试套件覆盖（路径模式、OPTIONAL MATCH） | 功能缺失 | M |
| P2 | 算法库扩充（betweenness/closeness/Louvain） | 功能缺失 | L |
| P3 | GQL 标准合规声明 + 测试套件 | 功能缺失 | L |
