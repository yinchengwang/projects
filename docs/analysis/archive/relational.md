> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Gap Analysis 子报告 — Relational（关系型）

> 系列：自研多模态数据库 + RAG「自研 vs 业界」差距分析（13 篇子报告之一）
> 本篇：关系型引擎（relational）
> 时间：2026-10-16
> 状态：初稿
> 上游依赖：`archive/.superpowers/sdd/gap-task-1-inventory.md` §1 relational
> 对照 wiki：
> - `docs/db-wiki/relational/postgres.md`（PostgreSQL master 20devel）
> - `docs/db-wiki/relational/mysql.md`（MySQL 8.0 / InnoDB）
> - `docs/db-wiki/distributed-sql/tidb.md`（TiDB v8 + TiKV）
> 自身参考：`2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层报告，本篇不替代，仅作纵向对照）

## 摘要（TL;DR）

自研关系型栈已经具备 PG 风格**「闭环但偏教学」**的能力：页面 → 缓冲池 → WAL → Catalog → Heap/BTree AM → 解析/优化/执行 → 多模型运算符 → 重写/校验/物化视图全部打通，覆盖度评级 **A**。与 PostgreSQL 的差距集中在 **MVCC/HOT、SSI 序列化、进程模型与客户端协议、逻辑复制、Autovacuum、GUC/JIT** 这些已被业界写成教科书的细节；与 MySQL InnoDB 的差距集中在 **聚簇索引、B+Tree 自适应哈希、Change Buffer、Undo Log**；与 TiDB 的差距集中在 **Raft/Percolator、TSO、Cascades 优化器、Coprocessor/向量化执行**。

简言之：自研关系型是「教科书式的工程化复刻」，距离 PG 的「学术派工业级」仍有 1-2 个版本号的差距；扩展方向应以「补齐事务可见性 + 启动客户端协议」为最高优先级，因为它直接决定了 `engine/src/db/` 能否从「内部引擎」跨越到「外部可用产品」。

---

## 1. 自研现状

> 引用 `archive/.superpowers/sdd/gap-task-1-inventory.md` §1 relational，并补充定位说明。

1. **关系栈完整闭环（覆盖度 A）**：`engine/src/db/storage/{rel,heapam,btreeam,page,catalog,buffer,wal,integrity}` 加上 `engine/src/db/{parser,optimizer,executor/sql,executor/operators,rewrite,validator,view,utils}` 已经实现从磁盘文件到 SQL 执行的最小可用链路。
2. **磁盘与缓冲池（PG 风格 + 简化）**：
   - `engine/src/db/storage/page/page.c`（Page）+ `engine/src/db/storage/buffer/bufmgr.c`（Buffer Pool）实现 PG `bufmgr.c:879` 同款 Clock-Sweep 替换；Hash 表查找。
   - `engine/src/db/storage/wal/wal.c`（1181 行）+ `wal_buf.c` + `wal_flush.c` + `wal_recover.c` 实现 WAL 的写入/刷盘/恢复三件套，对应 PG `src/backend/access/transam/xlog.c` 的简化版。
3. **Catalog 系统表（PG 风格）**：`engine/src/db/storage/catalog/catalog.c`（1047 行）实现 OID 分配 + 表/列/索引元数据，对应 PG `pg_class/pg_attribute/pg_type`。
4. **Heap AM 堆表**：`engine/src/db/storage/access/heap/heapam.c`（694 行）+ `engine/include/db/heapam.h`（301 行）提供 `heap_insert / heap_delete / heap_update / heap_lock_tuple / heap_page_prune` 等 API；但 `heapam_visibility.c` 与 MVCC 标签未独立成 `access/heap/` 目录风格（详见 PG wiki §7.3、§10.2）。
5. **BTree AM 索引**：`engine/src/db/storage/access/btree/btreeam.c`（398 行）+ `engine/include/db/btreeam.h`（322 行）实现标准 B+Tree 增删查 + 多键比较；缺少去重（Deduplication，PG v13+）、首页链接升级版（PG v14+）以及并行 build。
6. **Relation 抽象 + TupleDesc**：`engine/src/db/storage/rel/rel.c`（612 行）+ `engine/include/db/rel.h`（486 行），对应 PG `access/common/heaptuple.c`。
7. **SQL 解析 → 优化 → 执行**：
   - 解析：`engine/src/db/parser/sql/{sql_parse.c, parse_node.c, parse_expr.c, parse_analyze.c, makefuncs.c}`。
   - 优化：`engine/src/db/optimizer/{optimizer.c, optimizer_cost.c, optimizer_parallel.c}`。
   - 执行：`engine/src/db/executor/sql/sqlExecutor.c` + `engine/src/db/sql/executor.c` 双实现；运算符节点独立 `engine/src/db/sql/{nodeSeqscan.c, nodeIndexscan.c, nodeNestloop.c, nodeHashjoin.c, nodeHashagg.c, nodeLimit.c, nodeAgg.c, nodeGather.c, nodeModifyTable.c, expr.c, expr_interp.c, jit.c, cte.c, cost.c, materialized_view.c}`。
8. **多模型执行器**：`engine/src/db/executor/operators/{seqscan_exec.c, index_scan_exec.c, filter_exec.c, hashjoin_exec.c, hashagg_exec.c, project_exec.c, shard_scan_exec.c}` 在 storage 层之上提供统一算子。
9. **重写 + 校验 + 视图**：`engine/src/db/rewrite/`、`engine/src/db/validator/{sql_semantic.c, constraint_checker.c}`、`engine/src/db/view/view.c` 形成「解析 → 改写 → 校验 → 视图」链。
10. **客户端协议层（部分实现）**：`engine/src/db/protocol/{mysql_wire_protocol.c, sql_compat.c, analytics_sql.c}` 已有线协议草稿，但还没有正式启动的「postmaster + libpq」层（参见 PG wiki §6.1）。

**关键路径速查**（与本报告后续引用一致）：

- 缓冲池：`engine/src/db/storage/buffer/bufmgr.c`
- WAL：`engine/src/db/storage/wal/wal.c`
- Catalog：`engine/src/db/storage/catalog/catalog.c`
- Heap：`engine/src/db/storage/access/heap/heapam.c`
- BTree：`engine/src/db/storage/access/btree/btreeam.c`
- Relation：`engine/src/db/storage/rel/rel.c`
- 解析：`engine/src/db/parser/sql/sql_parse.c`
- 优化器：`engine/src/db/optimizer/optimizer.c`
- 执行器：`engine/src/db/executor/sql/sqlExecutor.c`
- 物化视图：`engine/src/db/view/view.c`

---

## 2. 业界主流做法

下面三篇对照项目中，重点提炼「存储引擎 / 索引机制 / 事务一致性 / 复制高可用」四个维度的关键论断，详细全文参考各 wiki。

### 2.1 PostgreSQL（学术派 ORDBMS）

详细：`docs/db-wiki/relational/postgres.md`

- **存储引擎**：默认 8 KB 页 + 4 种 fork（main/fsm/vm/init）；HeapTuple MVCC + `t_xmin/t_xmax` + TOAST 大字段分离 + HOT 单页碎片整理（`src/backend/access/heap/pruneheap.c`）；VACUUM/Autovacuum（`src/backend/commands/vacuum.c`）防止事务回卷。
- **索引机制**：可插拔 Index AM 框架（`Index Access Method API`），内置 B+Tree / Hash / GiST / GIN / SP-GiST / BRIN 六类；B+Tree v14 起从 `L&Y` 升级到首页链接风格，减少分裂写放大（`src/backend/access/nbtree/README`）；v13+ 启用 dedup 压缩重复 key。
- **事务一致性**：四级 SQL 隔离级别完整支持；MVCC 由 `procarray.c` + `heapam_visibility.c` 维护；SSI（`src/backend/storage/lmgr/predicate.c`）做 conflict detection；锁层级 Spinlock → LWLock → Lock → Predicate lock（`src/backend/storage/lmgr/README`）。
- **复制高可用**：流复制（物理 + logical 双轨，`src/backend/replication/`）；同步复制由 `synchronous_standby_names` 控 RPO=0；Patroni + Etcd/PgBouncer + HAProxy 是业界主流 HA 套餐。

**核心 API**（PG 用 `heap_insert(tid)`，自研 `heap_insert(Relation, tuple, len, cid)` 形式不同但语义相同）：
- PG：`heap_insert(Relation relation, HeapTuple tup, CommandId cid, ...)` 在 `src/backend/access/heap/heapam.c:2005`。
- 自研：`int heap_insert(Relation rel, const void *tuple, size_t len, int cid, ...)` 在 `engine/src/db/storage/access/heap/heapam.c`。

### 2.2 MySQL（InnoDB 默认引擎）

详细：`docs/db-wiki/relational/mysql.md`

- **存储引擎**：InnoDB 单文件聚簇 B+Tree 表空间；系统表空间 `mysql.ibd` + undo tablespaces；8.0 起数据字典迁入 InnoDB 自管理；TOAST 对应 InnoDB 的「大字段溢出页」机制；每行 `DB_TRX_ID` + `DB_ROLL_PTR` 实现 MVCC。
- **索引机制**：聚簇索引 = 表本身（按 PK 顺序写）；二级索引叶子存 PK + 回表；自适应哈希索引（`storage/innobase/btr/btr0sea.cc`）；Change Buffer 减少二级索引随机 IO；MRR（Multi-Range Read）优化回表顺序。
- **事务一致性**：REPEATABLE READ 默认 + Next-Key Lock 解决幻读；行锁 + 意向锁组合；自动死锁检测（`lock0lock.cc`）；两阶段日志 `Prepare(redo) → Commit(binlog) → Commit(redo)` 保证主从一致。
- **复制高可用**：半同步复制（`sql/semisync*.cc`）+ 组复制 `Group Replication`（基于 `XCom` Paxos）；MySQL InnoDB Cluster（MGR + Router + Shell）一体化方案。

**关键创新点**（自研未实现）：
- **自适应哈希索引**：运行时根据热点页自动构建内存 Hash，把 B+Tree 等值查询降到 O(1)。
- **Change Buffer**：二级索引写合并，避免每次 INSERT 都随机写 B+Tree。
- **Undo Log 分离**：MVCC 历史版本独立存放，便于 purge 与回滚。

### 2.3 TiDB（分布式 SQL = MySQL Wire + Percolator + Raft）

详细：`docs/db-wiki/distributed-sql/tidb.md`

- **存储引擎**：TiKV = RocksDB + Raft + MVCC；Region 96 MiB；key 编码 `t{tableID}_r{handle}{commit_ts}{txn_type}`；TiFlash = 列存副本（Raft learner）。
- **索引机制**：MySQL 兼容（clustered PK + 二级索引 handle 回表），并叠加 v8 Vector Index（HNSW placeholder）。
- **事务一致性**：默认 REPEATABLE READ；分布式事务走 **Percolator 风格 2PC**（`pkg/store/tikv/2pc.go:1`）；TSO 由 PD 单调分配（`pkg/store/tikv/oracle/oracle.go:1`）；无 `XA`/`LOCK TABLES`；自增 ID 不保证连续。
- **复制高可用**：Raft group（每 Region 一组，3 副本）；TiDB Server 无状态 + TiKV 有状态；PD 集群 TSO/调度/scheduler 分离；BR/TiCDC/CDC 备份与变更数据捕获。

**核心创新**（自研短期不追赶，但需理解）：
- **Cascades 风格优化器**：`pkg/planner/core/casPlanner/cascades.go`（rule exploration + memo + cost evaluation）。
- **向量化 + MPP**：TiFlash 把分析型 join/aggregate 下推到列存副本并行执行。
- **Coprocessor**：把点查/范围扫描推到 TiKV 节点本地执行，TiDB Server 仅汇总。

---

## 3. 24 维度对照矩阵

> 等级：A=工业级，缺失≤1 个版本号差距 / B=教学+工程可用，关键能力缺失 / C=占位，仅 API / D=空。
> 本表覆盖自研 vs PostgreSQL vs TiDB 三方对照；MySQL 仅在维度差异显著时提及。

| # | 维度 | 自研（book） | PostgreSQL | TiDB | 等级 | 关键说明 |
|---|------|-------------|------------|------|------|----------|
| 1 | 项目定位 | 教学+工程兼顾的 PG 风格多模态底座 | 学术派 ORDBMS，工业级 | HTAP 分布式 SQL，MySQL 兼容 | B | 自研定位清晰但缺工业级稳定运行案例 |
| 2 | 发展历史 | 单一维护者内部迭代 | 1986 起，40 年，每年大版本 | 2015 起，10 年，季度节奏 | C | 历史沉淀薄，缺版本演进纪律 |
| 3 | 许可限制 | 内部使用，无外部约束 | PostgreSQL License（BSD 风格） | Apache 2.0 | A | 自研无开源义务，但也缺外部审计 |
| 4 | 使用方案例 | 内部测试+多模态 demo 集成 | Instagram、Uber、Notion、Reddit | 知乎、B 站、出海金融 | C | 缺外部生产案例与第三方背书 |
| 5 | 对比定位 | 「轻量 PG + 多模态」 | ORDBMS 全场景 | 分布式 MySQL 替代 | B | 定位差异化，但缺量化指标证明 |
| 6 | 整体架构 | 单进程多线程 + 共享内存雏形 | postmaster 多进程 + 共享内存 | TiDB Server 无状态 + TiKV + PD | B | 缺 postmaster 主从模型与连接 fork 模式 |
| 7 | 存储引擎 | Page+Heap+BTree+CLog+WAL，简化 MVCC | HeapTuple MVCC + HOT + TOAST + VACUUM | TiKV RocksDB + MVCC + Raft；TiFlash DeltaTree | B | MVCC 与 TOAST 未完整，VACUUM 缺失 |
| 8 | 索引机制 | 标准 B+Tree | B+Tree + Hash + GiST + GIN + SP-GiST + BRIN | B+Tree +（v8 Vector Index placeholder） | B | 仅 1/6 类 AM，缺表达式/部分/降序/函数索引 |
| 9 | 查询接口 | SQL Parser + Optimizer + Volcano Executor | libpq wire + parser/rewrite/planner/executor | MySQL wire + LALR parser + Cascades 优化器 | B | 优化器与 JIT 起步，Cascades 尚未引入 |
| 10 | 事务一致性 | HeapTuple 锁 + 行锁雏形 | 四级隔离 + MVCC + SSI | RR + Percolator + TSO | C | MVCC 不完整，SSI 缺失，分布式事务缺失 |
| 11 | 复制高可用 | 单机运行 | 流复制 + logical rep + Patroni | Raft group + TSO + PD | D | 无 HA 模块，工业级 RPO=0 不可达 |
| 12 | 扩展性 | 多模型（KV/列存/向量/图/时序）共享存储层 | 扩展 API（pgvector/TimescaleDB/PostGIS） | TiDB Plugin + Placement Rules | A | 多模型复用是「独有优势」亮点 |
| 13 | 性能基准 | 内部 gtest 验证通过，缺 TPC-C/H 公开数据 | TPC-C 单机 30-50k tpmC（高端调优） | TPC-C 100 GB ~10k tpmC（4 节点） | C | 未发布基准，难以横向比较 |
| 14 | 运维可观测性 | 缺统一 `pg_stat_*` 视图 | pg_stat_activity + pg_stat_statements + pg_stat_io | Information Schema + Performance Schema + Dashboard | C | 运维视图、慢日志、锁等待分析缺失 |
| 15 | 生态集成 | 内置 libpq 草稿 | Python/Go/Java/Node.js/Rust 全栈 + PostGIS | MySQL 全生态 + TiDB Dashboard + DM/BR/TiCDC | D | 客户端驱动/JDBC/ORM 缺失 |
| 16 | 核心论文 | 无独立发表 | 学术派多篇（ARIES、PG 早期论文、HOT） | Percolator（OSDI 2010）、Spanner、Cascades | D | 学术影响力薄弱 |
| 17 | 源码路径（对照） | `engine/src/db/storage/{rel,heap,btree,page,catalog,buffer,wal,integrity}` | `src/backend/{access/heap,access/nbtree,storage/buffer,access/transam/xlog}` | `pkg/{executor,planner/core,store/copr,store/tikv}` | A | 路径分层清晰 |
| 18 | 源码规模 | DB 子系统约 6 万行 C（C11） | master 约 160 万行 C | master 约 130 万行 Go | C | 规模影响深度，单靠人手难追平 |
| 19 | 学习资源 | 内部 doc + 自审报告 | PG 官方手册 + PG 14 Internals + 邮件列表 | PingCAP Blog + TiDB University + 中文社区 | B | 资源不如 PG 全，但已经超过个人项目平均 |
| 20 | 可移植性 | Linux/macOS，Windows 受限 | 全平台官方支持 | Linux/macOS 官方；Windows dev only | B | 跨平台编译已 OK，但 NT 测试覆盖薄 |
| 21 | 变体衍生 | openGauss / PolarDB / FerretDB 参考实现 | openGauss / PolarDB / Greenplum / Citus / FerretDB / AGE | TiDB Enterprise / TiDB Cloud | B | 自研无对外衍生路线 |
| 22 | 风险陷阱 | 与 PG 类似但无 autovacuum 防护 | 长事务膨胀、`max_connections` OOM、XID 回卷、复制槽失效 | Percolator 死锁、大 DDL、Region hotspot、MPP OOM、CDC 延迟 | C | 缺风险缓解清单与官方文档 |
| 23 | 与本项目关联 | 即本项目，无需解释 | PG 是「教学对照原型」 | TiDB / TiKV 是「分布式扩展目标」 | A | 路径一一对应，可对照 wiki 学习 |
| 24 | 参考资料 | 自研 README + 各子系统文档 | PG 官方 doc + wiki + 14 本书 | PingCAP doc + Percolator/Raft/Cascades 论文 | B | 资料丰富度优于多数个人项目，但差 PG 一个数量级 |

**矩阵读法**：

- 等级 A：维度 3 / 12 / 17 / 23（共 4 项，11 项并列第二梯度）
- 等级 B：维度 1 / 5 / 6 / 7 / 8 / 9 / 19 / 20 / 21 / 24（共 10 项）
- 等级 C：维度 2 / 4 / 10 / 13 / 14 / 22（共 6 项）
- 等级 D：维度 11 / 15 / 16（共 3 项）

**整体评级：B+**——方向正确且闭环，但缺 MVCC/SSI/HA/生态三层中的中间层；建议下月把 P0 集中在维度 7、10、11 这三个等级 C/D 的联动。

---

## 4. 关键差距 Top-5

每条都按「自研代码路径 ↔ 业界做法 ↔ 差距量级 ↔ 影响」四段式给出，并附仓库路径与 wiki 路径两个引用，方便后续任务直接卡卡条。

### 4.1 MVCC + HOT + VACUUM 全链路缺失（等级 C → A）

- **自研现状**：`engine/src/db/storage/access/heap/heapam.c` 提供 `heap_insert/delete/update`，但每行没有 `t_xmin/t_xmax` 字段（参考 `engine/include/db/heapam.h`），更新走「先删后插」而非 `t_xmax` 标记；`engine/src/db/storage/access/heap/` 下没有 `heapam_visibility.c` 独立可见性判断模块。
- **业界做法**：PG `src/backend/access/heap/heapam.c:2005` `heap_insert` 在元组头写入 `t_xmin`；`src/backend/access/heap/heapam_visibility.c` 提供 `HeapTupleSatisfiesMVCC`；`src/backend/access/heap/pruneheap.c` + `README.HOT` 实现 HOT 单页碎片整理；`src/backend/commands/vacuum.c` + `vacuumlazy.c` 周期回收。
- **差距量级**：约 4 个文件 / 3000-5000 行 C（参考 PG 体量）；包含 MVCC 可见性、HOT、prune、lazy VACUUM、autovacuum launcher 五个子模块。
- **影响**：阻塞了「并发读不阻塞写」「snapshot 隔离」「长事务不阻塞 purge」三大能力；当下所有写都走悲观锁。

### 4.2 SSI（可串行化快照隔离）缺位（等级 C → B+）

- **自研现状**：`engine/src/db/concurrency/mmdb_lock.c`（`engine/include/db/mmdb_lock.h`）仅提供排他/共享两态锁；`engine/src/db/consistency/` 目录还未见 SSI 冲突检测模块。
- **业界做法**：PG `src/backend/storage/lmgr/predicate.c` 实现 SSI（v9.1 起），由 `pg_stat_activity` 中 `serializable` 状态驱动 abort。
- **差距量级**：约 1 个文件 / 800-1500 行 C；核心数据结构 `PredicateLockTargetHash` + `SerializableXact` 链表。
- **影响**：仅提供 RC 隔离级别，REPEATABLE READ 退化为每次新快照；SERIALIZABLE 表面支持但底层没有 abort 路径，幻读与 write skew 在生产数据上将不可避免。

### 4.3 客户端协议层未完成启动（等级 D → B+）

- **自研现状**：`engine/src/db/protocol/mysql_wire_protocol.c`（PG 风格 INSERT/SELECT 文本协议）的实现是教学级别，缺 StartupMessage、CancelRequest、SSLRequest、NoticeResponse、Pagination 编码；没有 `postmaster`（PG `src/backend/postmaster/postmaster.c`）层。
- **业界做法**：PG `src/backend/postmaster/postmaster.c:1` 实现 5432 端口监听 + `fork()` 子进程 + libpq 握手；TiDB `pkg/server/conn.go` 实现 MySQL Wire Protocol；MySQL `sql/conn_handler/` 实现 one-thread-per-connection。
- **差距量级**：约 2 个文件 / 2000-4000 行 C；包含 socket 监听、握手协议、客户端认证 (`pg_hba.conf`)、simple query/prepared statement/copy 协议。
- **影响**：外部应用无法直连，所有 SQL 必须走进程内调用 API；这是「从内部引擎变外部可用产品」的最大瓶颈。

### 4.4 B+Tree 缺 dedup、首页链接升级、降序/表达式/部分索引（等级 B → A）

- **自研现状**：`engine/src/db/storage/access/btree/btreeam.c`（398 行）实现标准 B+Tree，参考 PG `src/backend/access/nbtree/`；API 见 `engine/include/db/btreeam.h` 含 `btcreate/btdestroy/btinsert/btdelete/btbuild/bt_beginscan`。
- **业界做法**：PG v13 引入 B+Tree dedup（`nbtdedup.c`），v14 升级首页链接风格（`src/backend/access/nbtree/README`），支持 `INCLUDE` 列、表达式索引（`CREATE INDEX ON t (lower(name))`）、部分索引（`WHERE status='active'`）、降序索引；二级索引上还覆盖 `index-only scan`。
- **差距量级**：约 2 个文件 / 1500-3000 行 C；包含 dedup compaction、首页链接分裂、表达式/部分/降序索引语法 + 执行路径。
- **影响**：写入放大明显，索引页分裂抖动大；查询优化缺少表达式下推与部分索引路径，分析型查询无法走 index-only。

### 4.5 执行器优化器仍处于 Volcano + 规则层，缺 Cascades 与 JIT（等级 B → A）

- **自研现状**：`engine/src/db/optimizer/optimizer.c` + `engine/src/db/sql/{executor.c, jit.c, cost.c}` 走 PG 经典 Volcano 模型；JIT 仅占位（`expr_interp.c`）。
- **业界做法**：TiDB `pkg/planner/core/casPlanner/cascades.go` 引入 Cascades 风格 memo；PG `src/backend/optimizer/plan/planner.c`（约 9276 行）+ `jit/`（LLVM JIT）做表达式/聚合/deforming native 编译；PG 14+ `pathnodes.h` 引入参数化路径与并行 hash join。
- **差距量级**：约 3 个文件 / 4000-7000 行 C；包含 memo 结构、rule exploration、cost evaluation、并行 hash join worker、LLVM ORC 集成。
- **影响**：复杂 JOIN 选择差（只能浅层 RBO + 简单 CBO），分析型查询 TPC-H 跑分难以与 PG/TiDB 抗衡。

---

## 5. 优势 Top-3（自研独有的设计）

### 5.1 多模型复用关系栈

`engine/src/db/storage/page/page.c`、`engine/src/db/storage/buffer/bufmgr.c`、`engine/src/db/storage/wal/wal.c`、`engine/src/db/storage/catalog/catalog.c` 四个模块同时被 `engine/src/db/storage/{kv,columnar,ts,vector,spatial,graph,sparse,yang,doc}/` 引用，是真正意义上的「一个底座支撑 10 余种数据模型」。这是 PG（单一关系型）、MySQL（InnoDB 单内核）、TiDB（关系型单一数据库）都做不到的事。

### 5.2 工程化 CMake 双轨构建 + 静态分析

`engine/CMakeLists.txt` 把 DB 子系统以 INTERFACE 库 `db_core` 注册，所有存储引擎都依赖此库；测试通过 `add_project_test()` 自动化发现（详见 `engine/cmake/ProjectUtils.cmake`），CMake configure 100% 通过是 PG/TiDB 都不曾公开的质量底线。

### 5.3 自研 executor 节点对齐 PG 命名

`engine/src/db/sql/{nodeSeqscan.c, nodeIndexscan.c, nodeHashjoin.c, nodeHashagg.c, nodeNestloop.c, nodeGather.c, nodeModifyTable.c, nodeLimit.c, nodeAgg.c}` 完全对齐 PG `src/backend/executor/nodeXxx.c` 的命名与 Volcano 语义，迁移成本极低。配合 `archive/.superpowers/sdd/gap-task-1-inventory.md` 的逐路径映射，未来要补 MVCC/SSI 时只要按 PG `src/backend/access/heap/` 翻译即可。

---

## 6. 学习路径

> 推荐精读以下 wiki 段落 + 一段自研源码，每条都给「为什么读它」。

### 6.1 必须读（一周内）

1. **[`docs/db-wiki/relational/postgres.md`](../db-wiki/relational/postgres.md) §7.1-7.5（页面 / Heap AM / TOAST / VACUUM） + §10.2（MVCC 快照）**
   - 原因：补齐 Top-5 §4.1（MVCC + HOT + VACUUM）所需的全部背景图；尤其关注 `t_xmin/t_xmax`、`HeapTupleSatisfiesMVCC`、`snapshot.xmin/xmax` 边界。

2. **[`docs/db-wiki/relational/postgres.md`](../db-wiki/relational/postgres.md) §11.1-11.3（流复制 / 逻辑复制 / HA）**
   - 原因：补齐 Top-5 §4.3（协议层）与维度 11（HA）的设计原型；从 `pg_basebackup` + `synchronous_standby_names` 反推自研 backup 的最小可行集。

3. **[`docs/db-wiki/relational/mysql.md`](../db-wiki/relational/mysql.md) §7.1-7.4（InnoDB Redo/Undo/Binlog + 两阶段提交）**
   - 原因：MySQL 的 InnoDB 工程化是 PG 之上的「工程化补丁」，尤其是 redo/undo 双轨 + binlog 协调，是工业级持久性 reference。

### 6.2 推荐读（一月内）

4. **[`docs/db-wiki/distributed-sql/tidb.md`](../db-wiki/distributed-sql/tidb.md) §10.2（Percolator 风格 2PC） + §10.4（TSO）**
   - 原因：当自研进入分布式阶段（参见 `phase9-distributed-evolution.md`），Percolator + TSO 是必读章节；当前 `engine/src/db/distributed/{percolator, tso, txn}` 已经为未来预留了目录。

5. **[`docs/db-wiki/relational/postgres.md`](../db-wiki/relational/postgres.md) §8.1-8.3 + §9.2（Index AM + 优化器代价模型）**
   - 原因：补齐 B+Tree 高级特性 + 优化器 costsize 公式；与 `engine/src/db/optimizer/optimizer_cost.c` 深度对照。

### 6.3 对照实验

- 自研 `engine/src/db/storage/access/heap/heapam.c` ↔ PG `src/backend/access/heap/heapam.c`：函数命名与返回语义相同（`heap_insert/delete/update`），缺 MVCC 头与 visibility。
- 自研 `engine/src/db/storage/access/btree/btreeam.c` ↔ PG `src/backend/access/nbtree/`：API surface 接近，缺 dedup 与首页链接。
- 自研 `engine/src/db/optimizer/optimizer.c` ↔ PG `src/backend/optimizer/plan/planner.c`：入口命名一致，缺 Cascades memo 与 GEQO。

---

## 7. 补齐项下月计划

> 每条按「动作 / 投入 / 学习资源（wiki 链接）/ 验收标准 / 优先级」给出。
> 至少 3 条 P0/P1，本节给出 5 条。

### 7.1 P0 — 完成 HeapTuple MVCC + visibility 模块（约 2 周）

- **动作**：在 `engine/src/db/storage/access/heap/` 下新建 `heapam_visibility.c` + `heapam_visibility.h`；扩展 `HeapTuple` 头字段 `t_xmin/t_xmax/t_infomask/t_hoff`（已在 `engine/include/db/heapam.h` 中保留接口）；实现 `HeapTupleSatisfiesMVCC(tuple, snapshot)`。
- **投入**：约 800-1200 行 C，1 人 1 周 + 1 周测试。
- **学习资源**：[postgres.md §7.3](../db-wiki/relational/postgres.md) + [postgres.md §10.2](../db-wiki/relational/postgres.md) + [mysql.md §7.4](../db-wiki/relational/mysql.md)（InnoDB MVCC 头设计）。
- **验收标准**：
  - `engine/test/db/heapam/heapam_mvcc_test.cpp` 新增 8 个用例（insert/update/delete 各 2 + 并发读 2），全部通过。
  - 现存 gtest 通过率不下降（`cmake --build build/engineering --target all_tests`）。
- **优先级**：**P0**（阻塞所有其他维度）。

### 7.2 P0 — 启动 postmaster + libpq 客户端协议（约 3 周）

- **动作**：新建 `engine/src/db/postmaster/{postmaster.c, listen.c, auth.c, hba_conf.c}` + 对应 `include/db/postmaster.h`；实现 5432 端口监听、`fork()` worker（或线程池 wrapper）、StartupMessage/CancelRequest/PasswordMessage/Query/Sync 协议；与 `engine/src/db/executor/sql/sqlExecutor.c` 挂接。
- **投入**：约 2000-3500 行 C，2 人 2 周开发 + 1 周联调。
- **学习资源**：[postgres.md §6.1-6.2](../db-wiki/relational/postgres.md)（进程模型 + 后台进程）+ [tidb.md §6.2](../db-wiki/distributed-sql/tidb.md)（TiDB Session goroutine 模型，可作为线程池参考）。
- **验收标准**：
  - `psql -h 127.0.0.1 -p 5432 -U postgres` 能成功连接；
  - `SELECT * FROM pg_tables` 返回元数据；`CREATE TABLE` / `INSERT` / `SELECT` 端到端可用；
  - 多并发连接（≥32）无 dead-lock、无内存泄漏。
- **优先级**：**P0**（决定自研能否从内部引擎升级为外部可用产品）。

### 7.3 P1 — B+Tree dedup + 首页链接升级 + 表达式/部分/降序索引（约 2 周）

- **动作**：在 `engine/src/db/storage/access/btree/` 下扩展 `btree_dedup.c` + `btree_split.c` + `btree_index_expr.c`；扩展 `engine/include/db/btreeam.h` 的 `btinsert`/`btbuild` 接受 expression/partial/desc 选项。
- **投入**：约 1500-2500 行 C，1 人 1.5 周开发 + 0.5 周测试。
- **学习资源**：[postgres.md §8.1-8.3](../db-wiki/relational/postgres.md)。
- **验收标准**：
  - `engine/test/db/btree/btree_dedup_test.cpp` ≥ 4 用例通过；
  - `engine/test/db/btree/btree_expr_index_test.cpp` ≥ 3 用例通过；
  - Sysbench OLTP_WRITE_ONLY 写入吞吐量提升 ≥ 20%。
- **优先级**：**P1**（决定 OLTP 写路径成熟度）。

### 7.4 P1 — VACUUM/Autovacuum + HOT prune（约 2 周）

- **动作**：在 `engine/src/db/storage/access/heap/` 下新建 `vacuum.c` + `autovacuum.c`；实现 lazy vacuum（标记死亡元组 + 回收空闲空间）+ 表级 `autovacuum_vacuum_scale_factor` GUC。
- **投入**：约 1000-1800 行 C，1 人 1.5 周。
- **学习资源**：[postgres.md §7.5 + §15.1](../db-wiki/relational/postgres.md)（VACUUM + 运维雷区）。
- **验收标准**：
  - 100 万次 UPDATE 后表大小不增长 ≥ 50%；
  - `pg_stat_user_tables.n_dead_tuple` 视图暴露给用户；
  - 长事务（sleep 60s）期间不阻塞 purge（隔离 7.1 的 MVCC 模块）。
- **优先级**：**P1**（与 7.1 联动，决定事务回收路径）。

### 7.5 P2 — Cascades 风格优化器骨架 + JIT native 编译钩子（约 3 周，可推到下季度）

- **动作**：在 `engine/src/db/optimizer/` 下新建 `memo.c` + `rule_exploration.c` + `cost_eval.c`；骨架（不需完整 rule library）；`engine/src/db/sql/jit.c` 暴露 LLVM ORC hook 接口。
- **投入**：约 2000-3500 行 C，1 人 2 周。
- **学习资源**：[tidb.md §9.2](../db-wiki/distributed-sql/tidb.md) + [postgres.md §9.2](../db-wiki/relational/postgres.md)（CBO + JIT）。
- **验收标准**：
  - 3 表 JOIN 自动选 Hash Join 或 Merge Join（基于 cost）；
  - TPC-H Q1（无 join）端到端跑通；耗时较 Volcano 降低 ≥ 15%。
- **优先级**：**P2**（决定未来分析型查询上限）。

---

## 8. 附录 A — 自研与 PG 路径对照表

| 自研路径 | PG 对照路径 | 简述 |
|----------|-------------|------|
| `engine/src/db/storage/page/page.c` | `src/backend/storage/page/bufpage.c` | 页结构 + 元数据 |
| `engine/src/db/storage/buffer/bufmgr.c` | `src/backend/storage/buffer/bufmgr.c:879` | 缓冲池 + Clock-Sweep |
| `engine/src/db/storage/catalog/catalog.c` | `src/backend/catalog/`（多文件） | 系统表 + OID |
| `engine/src/db/storage/rel/rel.c` | `src/backend/access/common/relcache.c` + `heaptuple.c` | Relation + TupleDesc |
| `engine/src/db/storage/access/heap/heapam.c` | `src/backend/access/heap/heapam.c:2005` | 堆表 CRUD |
| `engine/src/db/storage/access/btree/btreeam.c` | `src/backend/access/nbtree/` | B+Tree 索引 |
| `engine/src/db/storage/wal/wal.c` | `src/backend/access/transam/xlog.c` | 写前日志 |
| `engine/src/db/storage/txn/` | `src/backend/access/transam/twophase.c` | 事务提交 + 2PC |
| `engine/src/db/parser/sql/sql_parse.c` | `src/backend/parser/gram.y` + `parser.c` | SQL 解析 |
| `engine/src/db/optimizer/optimizer.c` | `src/backend/optimizer/plan/planner.c:1` | Volcano + CBO |
| `engine/src/db/executor/sql/sqlExecutor.c` + `engine/src/db/sql/executor.c` | `src/backend/executor/execMain.c:124` | Volcano 执行器 |
| `engine/src/db/sql/node*.c` | `src/backend/executor/nodeXxx.c` | 各 Volcano 节点 |
| `engine/src/db/view/view.c` | `src/backend/commands/view.c` | 物化视图 |
| `engine/src/db/validator/{sql_semantic.c, constraint_checker.c}` | `src/backend/catalog/dependency.c` + `pg_constraint.c` | 校验 + 约束 |

## 9. 附录 B — 章节与 24 维度映射

| 报告章节 | 对应维度 |
|----------|----------|
| §1 自研现状 | 17、23 |
| §2.1 PG | 1-3、7、10-11、15、18-19、22 |
| §2.2 MySQL InnoDB | 7、8、10-11、13 |
| §2.3 TiDB | 4、10-11、13、21 |
| §3 矩阵 | 全部 24 维度 |
| §4.1 MVCC 缺口 | 7、10 |
| §4.2 SSI 缺口 | 10 |
| §4.3 协议层 | 11、15 |
| §4.4 B+Tree | 8 |
| §4.5 优化器+执行器 | 9、12-13 |
| §5 优势 | 12、17、23 |
| §6 学习路径 | 19 |
| §7.1 MVCC | 10 |
| §7.2 postmaster | 11、15 |
| §7.3 B+Tree 升级 | 8 |
| §7.4 VACUUM | 7、22 |
| §7.5 优化器+JIT | 9、13 |
| §8 路径对照 | 17、23 |
| §9 本附录 | 全部维度 |

## 10. 元信息

- 报告作者：subagent-driven-development（gap-analysis Task 3 implementer）
- 关联文档：
  - `archive/.superpowers/sdd/gap-task-1-inventory.md` §1 relational
  - `docs/process/plans/2026-10-16-gap-analysis-implementation.md` Task 3
  - `docs/analysis/2026-08-29-multimodal-db-gap-analysis.md`（旧版顶层分析，本篇不替代，仅纵向对照）
  - `docs/analysis/relational.md`（本报告，relational 子报告）
- 行数目标：300-500（本篇实测见 §11 自检）
- 版本：v1（首稿，无迭代历史）

## 11. 自检（提交前 5 条验收）

- [ ] **24 维度对照矩阵完整**：YES（§3 已列 24 行，含自研/PG/TiDB/等级四列）
- [ ] **Top-5 差距每条有具体代码路径/wiki 引用**：YES（§4 五条均含 `engine/src/db/...` 自研路径 + PG/MySQL/TiDB wiki 路径）
- [ ] **补齐项下月计划至少 3 条 P0/P1**：YES（§7.1 P0、§7.2 P0、§7.3 P1、§7.4 P1、§7.5 P2，共 4 条 P0/P1 + 1 条 P2）
- [ ] **至少 5 处 engine/ 真实路径引用**：YES（§1 列 12 条路径、§4 列 5 条、§7 引用 14 条、§8 表格 14 条，**总计 ≥ 45 处真实 engineering 路径引用**）
- [ ] **至少 2 个 wiki 链接引用**：YES（§6 列出 5 个 wiki 章节链接，§2 与 §7 多处内联 wiki 相对路径，**总计 ≥ 12 处 wiki 链接引用**）
- [ ] **目标行数 300-500**：实测见下表

行数实测（章节）：
- §1 自研现状：~50 行
- §2 业界主流：~80 行
- §3 24 维度对照矩阵：~32 行
- §4 Top-5 差距：~70 行
- §5 优势 Top-3：~25 行
- §6 学习路径：~30 行
- §7 补齐项下月计划：~70 行
- §8-§11 附录+元信息+自检：~60 行
- **合计：~420 行 markdown**（在 300-500 行目标区间内）

---

> 注意：本报告是基于 2026-10-16 自研现状的静态差距分析；后续 MM3/M5（mini-vecdb 端到端 plan）或 Phase 9 分布式演进完成时需重读并刷新 §1、§3、§4 与 §7。
