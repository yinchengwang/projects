> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# 多模态数据库差距深度分析（2026-08）

> 实现质量级差距分析，**取代** `docs/multimodal-db-comparison-2026.md` 的"差距分析"部分（功能级参考仍保留）
> 设计文档：`docs/process/specs/2026-08-27-multimodal-gap-analysis-design.md`
> 审查范围：22 个领域（8 已注册模态深挖 + 9 未注册引擎核对 + 5 缺失模态调研）
> 审查方式：全部为静态代码审查，未运行新基准

## 1. 执行摘要

### 1.1 总体结论

自研多模态数据库的 8 个已注册数据模型在**功能广度**上接近 mid-tier 商用产品（21+ 向量索引、PG/MySQL 双协议、CTE/窗口/物化视图/分区、Cypher 子集、YANG 解析、BM25 + JSONPath + 嵌套文档、TimescaleDB 风格连续聚合、H3/S2 之外合理的 R-Tree+Hilbert 双重空间索引）。但在**实现质量**层面存在三类系统性缺口：

1. **并发原语缺失**：5 个核心模态（Vector / Timeseries / Document / Graph / Spatial）各自复制同一份有竞态窗口与写者饥饿缺陷的自旋读写锁，且默认 `use_lock=false`；faiss_hnsw 与 CSR 加减边时直接 realloc + 无锁，与并发搜索共制造 UAF 风险。
2. **WAL 覆盖严重不对称**：只有 KV 模态接入共享 WAL；Vector 有独立 WAL 但缺 fsync；Relational / Timeseries / Spatial / Tree 写路径完全无 redo log，崩溃后不可恢复。
3. **执行器与存储层契约断裂**：关系模态的 `nodeModifyTable.c` 用硬编码 TID（块 0、偏移 24）替代真实物理位置——UPDATE/DELETE 会改错行；MVCC 模块（4000+ 行）零调用方，未接入 SeqScan/MVCC 可见性过滤；MemContext 体系覆盖到部分路径但执行器初始化仍依赖手工 free 配对。

### 1.2 最严重的正确性风险 Top 3

| 排名 | 缺陷 | 模态 | 严重性 |
|------|------|------|--------|
| 1 | UPDATE/DELETE 构造 TID 硬编码块 0 偏移 24，会改错行 | Relational | 静默数据损坏 |
| 2 | faiss_hnsw add 触发 realloc 与 search 并发 → UAF | Vector | 内存安全崩溃 |
| 3 | WAL 写入返回值被忽略 + 无 fsync → 持久化层假承诺 | KV/Vector | 崩溃后数据丢失无痕迹 |

### 1.3 投入产出最高的改进 Top 3

| 排名 | 改进 | 工作量 | 影响 |
|------|------|--------|------|
| 1 | 抽取公共并发原语库（pthread_rwlock 包装或自研 RCU） | M | 一次性修 5 个模态的 rwlock bug + 5 个 UAF 风险 |
| 2 | 关系模态 DML 接入共享 WAL + 真实 TID 管道 | L | 关系模态首次具备崩溃安全 + UPDATE/DELETE 不再改错行 |
| 3 | 集成 OSS 填补外围空白：MinIO（Blob）+ Lucene/Tantivy（全文）+ ClickHouse（可观测） | M | 用集成而非自研覆盖 3 个缺失模态 |

## 2. 22 领域差距总矩阵

### 2.1 8 个深挖模态

| 模态 | 并发 | 崩溃 | 内存 | 错误处理 | 算法 | API | 总分 | 最大风险 |
|------|------|------|------|---------|------|------|------|---------|
| Vector | 3 | 4 | 4 | 3 | 7 | 5 | 4.3 | faiss_hnsw UAF + rwlock 竞态 |
| Relational | 2 | 2 | 6 | 3 | 4 | 6 | 3.8 | TID 伪造 + MVCC 孤岛 + 无 WAL |
| Graph | 3 | 4 | 6 | 5 | 4 | 5 | 4.5 | CSR 无锁 + 算法库薄 |
| KV | 2 | 4 | 6 | 4 | 4 | 5 | 4.2 | 并发 put 丢更新 + WAL 失败吞 |
| Timeseries | 3 | 3 | 5 | 4 | 5 | 5 | 4.2 | 复刻 rwlock bug + 热路径未压缩 |
| Document | 3 | 3 | 5 | 5 | 5 | 5 | 4.3 | 第三个模态复制 rwlock bug |
| Spatial | 3 | 3 | 5 | 5 | 5 | 4 | 4.2 | R-Tree 无锁 + 仅平面 |
| Tree/Yang | 4 | 3 | 5 | 4 | 4 | 5 | 4.2 | XML parser 否认必要属性/namespace |

### 2.2 9 个未注册引擎

| 引擎 | 定位 | 完成度(0-10) | 注册建议 |
|------|------|--------------|---------|
| RDF/SPARQL | 语义图谱 | 7 | **MODEL_RDF = 10**，与 Graph 共享存储 |
| 稀疏向量+BM25 | 混合检索 | 7 | **MODEL_SPARSE = 11**，与 Vector 同源 |
| Column Family | KV 多命名空间 | 8 | 不独立注册（KV 扩展） |
| 流引擎 | 流处理 | 5 | 复用 MODEL_STREAM |
| 列存引擎 | 列式 OLAP | 6 | 不独立注册，作为存储格式 |
| 时空引擎（st） | 移动对象 | 4 | **MODEL_SPATIOTEMPORAL = 12** |
| 物化视图（mmview） | 预计算 | 4 | 不独立注册，Relational 子系统 |
| 账本（ledger） | 不可变审计 | 3 | 不注册，语义层 |
| CDC 订阅 | 变更捕获 | 5 | 不独立注册，WAL 扩展 |

### 2.3 5 个缺失模态

| 模态 | 业界代表 | 契合度(0-10) | 建议 |
|------|---------|-------------|------|
| 全文搜索引擎 | Elasticsearch / Tantivy | 7 | 集成 Lucene/Tantivy 增强 Document |
| 宽表 / Wide-Column | Cassandra / TiKV | 5 | 阶段化（CF → LSM → 分布式） |
| 对象 Blob | S3 / MinIO | 8 | **不自研**，集成 MinIO + BLOB 外键 |
| 可观测日志 | Loki / ClickHouse | 6 | 集成 ClickHouse + 标签索引 |
| 多模态 AI 原生 | LanceDB / Qdrant / Vespa | **9** | **优先投入**，与 Vector/RAG 互补 |

## 3. 跨模态共性 Gap

以下 6 类缺陷**至少在 3 个深挖模态中出现**，是系统性问题而非局部 bug：

### 3.1 自研有缺陷的自旋读写锁被跨模态复制（5 个模态）

`vector_engine.c:1557`、`ts_engine.c:745-770`、`doc_engine.c:750-770` 三个模态复刻同一份 `simple_rwlock_t`（readers/writers_waiting/writer_active 结构），存在：
- 读者与写者同时持有的竞态窗口
- `writers_waiting` 永不检查 → 写者饥饿
- `timeout_ms` 参数被忽略
- `use_lock=false` 是默认

**Graph 与 Spatial 模态甚至连这份错误实现都未复用**——`graph_csr.c`、`rtree.c` 直接零锁，更糟。**抽取公共并发原语库（pthread_rwlock 包装 / RCU）一次性覆盖全部 5 个模态**。

### 3.2 WAL 覆盖严重不对称（4 个模态无 redo）

只有 **KV** 接入共享 WAL（`kv.c:443-447,671`），Vector 有独立 WAL 但 `SYNC 模式无 fsync`（`vector_wal.c:282-289`）；**Relational / Timeseries / Spatial / Tree** 写路径无任何 redo log。崩溃后已修改脏页无法重放。

### 3.3 持久化 fsync 缺失（3 个模态）

`vector_wal.c:282-289` SYNC 模式只 `fflush`，`vector_wal.c:404` checkpoint 也只 `fflush`——只防进程崩溃不防系统崩溃。KV `wal_write_*` 调用（`kv.c:445,459`）返回值被忽略，WAL 失败静默吞。**统一 fsync 策略 + 失败回滚**。

### 3.4 锁默认关闭（5 个模态）

`vector_engine.c:219`、`ts_engine.c:133`、`doc_engine.c:114` 均 `use_lock = false` 默认；`graph_csr.c`/`rtree.c` 无任何并发原语。**默认应是有锁（或至少读写分离的 RCU），无锁应是 opt-in**。

### 3.5 错误路径清理不一致（4 个模态）

- Vector：`table_drop` 空操作返回成功（`vector_engine.c:351`）
- KV：`wal_write_*` 返回值忽略（`kv.c:445,459`）
- Document：`doc_engine_free_results` 与 stage 自由 free 所有权分裂（疑似）
- Yang：`goto fail` 多处依赖人工审计（`yang_model.c:352,388`）

**统一错误码扩展 + 错误路径资源回收工具**（PostgreSQL 风格的 `ERRCODE_*` + MemoryContext 统一释放）。

### 3.6 跨模态集成测试稀疏

单模态内部测试 252/252 PASS（memory 记录），但跨模态混合场景（Vector + RAG + Graph + mm_storage）端到端集成测试覆盖度低。**建议新增跨模态测试基线**——优先是 Graph + Vector + RAG、Relational + MVCC + WAL、Vector + faiss_hnsw 并发。

## 4. 已关闭 Gap 备忘（相对 8 月 25 日旧对比文档）

经本次静态审查核实，**已关闭或大幅进展**的 Gap：

| 旧文档 Gap | 当前状态 | 证据 |
|-----------|---------|------|
| GPU 加速（Vector） | ✓ 已落地 | commit `719c06d9c`（P2-1）；`index/vector_index/gpu/`（gpu_hnsw.c/gpu_ivf.c/gpu_ivf_pq.c/simd.c） |
| 图算法库（P1-4） | ✓ 7 个算法（从 4 → 7） | commit `0b4ff2090`；`graph/graph_algorithms.c` 新增 Dijkstra + 连通分量 |
| Cypher 修复 | ✓ | commit `a37a6e396` |
| Column Family（P3-4） | ✓ | commit `bb2c77fbb`；`cf_engine.c/cf_row.c/cf_column.c` |
| Yang/NETCONF 注册（P3-3） | ✓ | commit `beac0aee1`；`netconf_server.c` 631 行 + Yang parser |
| 无物化视图（Timeseries） | 旧文档误判 | `ts_continuous_agg.c` 514 行 + `ts_mview.c` 348 行 |
| 无聚合管道（Document） | 旧文档误判 | `doc_pipeline.c` 1372 行（match/group/sort/limit/skip/project）+ `doc_agg.c` 705 行 |
| 缺同义词（Document） | 旧文档误判 | `doc_fts.c:97-175` DocSynonyms 系统 |
| 无独立 KV Value 限制（VecPage 16KB） | 旧文档误判 | `include/db/kv.h:34` `KV_MAX_VALUE_SIZE = 1MB` |
| 无 MVCC（Relational） | 旧文档误判 | `txn.h` 完整 xmin/xmax/CID/保存点/2PC；但**仍未集成到执行路径**——见 03-relational 卷 |

## 5. 优先级路线图（按正确性风险 > 投入产出）

### 5.1 P0（立即修复——正确性炸弹）

| 项目 | 工作量 | 涉及模态 |
|------|--------|---------|
| 关系模态 TID 管道修复 | M | Relational |
| 抽取统一并发原语库 | M | Vector / Timeseries / Document / Graph / Spatial（5 模态） |
| faiss_hnsw 加 RCU/快照 + search 加读锁 | M | Vector |
| 关系模态 DML 接入共享 WAL | L | Relational |
| WAL 失败处理 + fsync 统一 | M | KV / Vector |

### 5.2 P1（短期补齐——主要功能缺口）

| 项目 | 工作量 | 涉及模态 |
|------|--------|---------|
| MVCC 集成到执行路径（SeqScan 可见性 + heap_insert 戳 xmin + DML 事务包裹） | L | Relational |
| CSR/R-Tree 读写锁或 RCU | M | Graph / Spatial |
| 优化器 join 顺序动态规划 或摘掉假开关 | M | Relational |
| DML 错误传播 + 中止语义 | S | Relational |
| PageRank 悬挂节点处理复核 + 单元测试 | S | Graph |
| 热路径增量压缩 | M | Timeseries |
| XML parser 升级（属性/命名空间）+ datastore 持久化 | M | Tree |

### 5.3 P2（中期扩展——功能面补齐）

| 项目 | 工作量 | 涉及模态 |
|------|--------|---------|
| 集成 MinIO（Blob）+ Lucene/Tantivy（全文）+ ClickHouse（可观测） | M × 3 | 新增能力 |
| 多模态 AI 原生（NamedVector + 对象内嵌 Blob + 跨模态检索） | L | Vector + RAG |
| B+Tree 页式化 + FSM | L | Relational |
| Cypher/openCypher 核心测试套件覆盖 | M | Graph |
| 图算法库扩充（betweenness/closeness/Louvain） | L | Graph |
| ST_* PostGIS 兼容子集 | L | Spatial |
| KV CAS/Watch/Multi + KV_FULL 错误码 + B+Tree 分裂 | M | KV |
| 跨模态端到端集成测试基线 | M | 全部 |

### 5.4 P3（长期生态——非关键路径）

| 项目 | 工作量 | 涉及模态 |
|------|--------|---------|
| DiskANN 持久化 + GQL 标准合规 | L | Vector / Graph |
| 分布式分片 + Raft 一致性 | XL | 全部 |
| YANG/NETCONF 1.1 + SSH transport | L | Tree |
| PostgreSQL ltree 兼容层 | L | Tree |

## 6. 分卷目录

| 卷 | 主题 | 文件 |
|----|------|------|
| 00 | 总览（本文） | `README.md` |
| 01 | Vector 深挖 | `01-vector-gap.md` |
| 02 | Relational 深挖 | `02-relational-gap.md` |
| 03 | Graph 深挖 | `03-graph-gap.md` |
| 04 | KV 深挖 | `04-kv-gap.md` |
| 05 | Timeseries 深挖 | `05-timeseries-gap.md` |
| 06 | Document 深挖 | `06-document-gap.md` |
| 07 | Spatial 深挖 | `07-spatial-gap.md` |
| 08 | Tree/Yang 深挖 | `08-tree-gap.md` |
| 09 | 9 个未注册引擎核对 | `09-unregistered-engines.md` |
| 10 | 5 个缺失模态调研 | `10-missing-modalities.md` |

---

## 7. 审查方法论说明

- 所有深挖卷的结论基于代码静态审查，每个缺陷均附 `file:line` 证据
- 6 维质量审查标准：并发正确性 / 崩溃恢复 / 内存安全 / 错误处理 / 算法实现质量 / API 设计
- 缺口二分标注：「功能缺失」（工程量问题）与「实现质量缺陷」（正确性风险）
- 评分口径：0-10 分（10=业界标杆，5=可用但明显落后，≤2=缺失或有正确性风险）
- 结论区分「确认」（代码可证）与「疑似」（需运行时验证）
- 未运行新的性能基准；业界对比引用 `reference/` 子模块源码与既有 `docs/multimodal-db-comparison-2026.md` 数据

**重要约定**：本报告取代 `docs/multimodal-db-comparison-2026.md` 的"差距分析"部分；该文档保留作为功能级参考。`docs/multimodal-gap-fill-plan-2026.md` 仍可作为历史路线图参考，但 P0-P3 任务状态需要根据本次发现的"实现质量缺陷"重排优先级（尤其 TID 管道修复、统一并发原语库两个 P0）。
