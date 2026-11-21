> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Relational 模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查（未运行新基准）
> 代码位置：`engine/src/db/sql/`（~19.3K 行，38 文件）+ `storage/rel/`（~1.2K 行）+ `executor/`（~8.2K 行）+ `txn/`（~0.7K 行）+ `storage/txn/`、`concurrency/`（MVCC 族）+ `index/btree/`

## 1. 实现现状盘点

### 1.1 模块清单

| 层 | 模块 | 关键文件 | 行数 |
|----|------|---------|------|
| 解析/执行 | Volcano 执行器 + 12 种物理算子 | `sql/executor.c`（918）、`sql/node*.c`（SeqScan/IndexScan/NestLoop/HashJoin/HashAgg/Sort/Limit/Gather/Window/ProjectSet/ModifyTable/RefreshMview） | ~5600 |
| 解析/执行 | 表达式求值 | `sql/expr.c`、`expr_interp.c` | 465 |
| 优化 | 优化器框架 + 代价模型 | `sql/optimizer.c`（360）、`cost.c`（247） | 607 |
| SQL 特性 | CTE / 窗口函数 / 物化视图 / 分区 | `sql/cte.c`（444）、`nodeWindow.c`（814）、`materialized_view.c`（928）、`partition.c`（530） | 2716 |
| 协议 | PG Wire / MySQL Wire / REST | `sql/pgwire.c`（1292）、`mysql_wire_protocol.h`、`rest_api.c`（956） | ~3000 |
| 事务 | MVCC 事务管理器 | `txn/txn.c`（668）、`storage/txn/`（mvcc/mvcc_wal/heap_visibility/txn_xact/vacuum）、`concurrency/`（mvcc_snapshot/mvcc_visibility/mvcc_version/mvcc_hot/mvcc_gc） | ~4000+ |
| 内存 | MemoryContext 体系 | `sql/memctx.c`（834） | 834 |
| 索引 | 内存 B-Tree | `index/btree/btree_insert.c` 等 | ~800 |
| 存储 | Heap AM / Relation | `storage/access/heap/heapam.c`、`storage/rel/` | ~1400 |
| 其他 | 并行 / JIT / 子模块 | `sql/parallel.c`（250）、`jit.c`（85 桩） | 335 |

### 1.2 测试覆盖

`test/db/sql/`（sql_integration、sql_parser、test_sql_storage_integration）、`test/db/storage/txn.cpp`（MVCC 模块隔离测试）、`test/db/protocol/`、`test/db/parser/`。据 memory 记录 SQL 执行器 P1-5 完成时 184+ 测试；MemoryContext 迁移后 252/252 PASS（Debug），Release 下 8 个 use-after-free 为存量问题。

### 1.3 重要事实修正（相对 2026-08-25 旧对比文档）

旧文档称"无 MVCC"——**不准确**。MVCC 模块族（xmin/xmax/CID/保存点/子事务/2PC 预提交态，`include/db/txn.h:1-80`）已完整实现，**但未接入执行路径**（见 2.1）。旧文档称"缺少窗口函数/CTE"——**已过时**，`nodeWindow.c`/`cte.c` 均已实现。

## 2. 代码级质量审查

### 2.1 并发正确性（评分依据：MVCC 孤岛化，DML 无事务包裹）

**缺陷 1：MVCC 可见性函数零调用方——整套机制是孤岛「确认·实现质量缺陷」**

- `storage/txn/heap_visibility.c:30` 定义 `heap_tuple_visible()`，全仓库无任何其他文件调用（grep 仅命中定义文件自身）
- `txn_begin(` 仅在 `txn/txn.c` 与 `storage/txn/txn_xact.c` 内部出现，SQL 执行器不开启事务
- `sql/nodeSeqscan.c:223-256` 的 `ExecSeqScan` 拉取元组后只做 `ExecQual`（WHERE）过滤，无任何可见性检查——已删除/未提交的行会被当作正常行返回
- `sql/nodeModifyTable.c:56-61` 调用 `heap_insert` 时 `cid` 传 0，且 `heap_insert` 内部 `(void)cid;`（`storage/access/heap/heapam.c:300`）——xmin/xmax 戳记根本不写入

结论：并发读写的正确性保证**不存在**，尽管代码库里有 4000+ 行的 MVCC 实现。

**缺陷 2：执行器全程无锁、无快照「确认·实现质量缺陷」**

`sql/executor.c` 与全部 `node*.c` 无任何锁原语；并行算子 `nodeGather.c` 的 Worker 与主线程共享 EState 无同步保护（`sql/parallel.c` 仅 250 行的 Worker Pool 框架）。PostgreSQL 的等价物是每算子共享状态分区 + barrier 同步。

### 2.2 崩溃恢复（评分依据：关系 DML 完全无 WAL 覆盖）

**缺陷 1：heap_insert/delete/update 不产生任何 WAL 记录「确认·实现质量缺陷」**

`storage/access/heap/heapam.c:294-382` 的 `heap_insert` 只做 `buf_dirty(buf)`（:375）——脏页最终刷盘，但无 redo 日志。全仓库 `wal_log_` 类调用只出现在 `storage/wal/`（WAL 本体）、`storage/kv/kv.c:671`（KV 模块）和 `storage/vector/vector_wal.c`（向量独立 WAL）。**关系模态是三大写路径中唯一没有 WAL 的**。崩溃后页面可能处于半写状态且无法重放。

对比：PostgreSQL `heap_insert` → `XLogInsert(XLOG_HEAP_INSERT)`；这是 8 月 25 日旧文档未发现的更深差距（旧文档只说"WAL 已支持"，实际只对 KV 生效）。

**缺陷 2：无 FSM（空闲空间映射），删除空间永不复用「确认·功能缺失」**

`heapam.c:305-364`：插入只尝试最后一页（`rd_nblocks - 1`），空间不足直接开新页——没有 PostgreSQL FSM/FSM 的空闲页回收查找。UPDATE/DELETE 释放的中间页空间永远无法被后续插入复用，表持续膨胀。

### 2.3 内存安全（评分依据：memctx 体系扎实，存量 UAF 待清）

**正面证据 1：MemoryContext 体系接近 PG 形态「确认」**

`sql/memctx.c`（834 行）+ P6 迁移（memory 记录：5859 处裸分配清理、252/252 测试 PASS）、资源析构回调、线程归属校验、Generation 追踪——这套机制显著优于一般 C 项目。

**缺陷 1：Release 构建 8 个存量 use-after-free「确认（引自 memory，代码位置未逐一复核）·实现质量缺陷」**

Debug 全绿但 Release 8 个 UAF（memory: p6-memory-context Task 14 记录），说明仍有依赖 Debug 分配器布局的代码。无 CI 常态化 ASAN/UBSAN（MinGW 无 libasan，Task 56 跳过）。

**缺陷 2：优化器/执行器错误路径的清理依赖手工配对「确认·实现质量缺陷」**

`sql/nodeSeqscan.c:160-176` 的 `ExecInitSeqScan` 失败路径 5 个资源逐一 free，手工配对无 goto-fail 模式——新增资源时极易漏一个（PostgreSQL 用 MemoryContextReset 统一回收，自研 memctx 尚未覆盖到执行器初始化路径）。

### 2.4 错误处理（评分依据：DML 错误静默吞没 + TID 伪造）

**缺陷 1：UPDATE/DELETE 构造的 TID 硬编码"块 0、偏移 24"「确认·实现质量缺陷（本模态最严重缺陷）」**

`sql/nodeModifyTable.c:70-75`（UPDATE）与 `:96-99`（DELETE）：

```c
/* 构造 TID（简化：假设元组在块 0） */
uint32_t block = 0;
uint16_t offset = 24;
```

槽位不携带真实元组物理位置，无论目标行在哪，更新/删除都作用于"块 0 偏移 24"。多行表上的 UPDATE/DELETE **会改错行**。这属于功能性错误而不只是质量问题——需要在扫描节点把 TID 装入 TupleTableSlot 一路传到 ModifyTable。

**缺陷 2：DML 失败静默吞没「确认·实现质量缺陷」**

`nodeModifyTable.c:62-64/85-87/106-108`：`heap_insert/heap_update/heap_delete` 返回非 0 时不报错、不中止、不计数，`:117` 照常 `mt_processed++`——上层把失败当成功。

**缺陷 3：优化器规则全为 TODO 骨架却宣称启用「确认·实现质量缺陷」**

`sql/optimizer.c:83-84` `enable_join_order = true` 时调用 `opt_join_order()`，但 `:199-220` 的实现是"递归遍历后原样返回计划"，TODO 注释承认未实现。同样原样返回的还有 `opt_constant_folding`（:228-248）、`opt_subquery_unnest`（:256-276）、`opt_agg_pushdown`（:284-304）、`opt_sort_elimination`（:312+）。配置开关制造了"已优化"的假象。

### 2.5 算法实现质量（评分依据：算子实现可用，优化器是空壳）

**正面证据 1：B-Tree 插入算法教科书级正确「确认」**

`index/btree/btree_insert.c:27-64` 的 `_btree_split_child` 严格按 CLRS 中位数分裂（`right->nkeys = t-1`、中位键上提），`:88-107` 下探前预分裂的 top-down 策略正确；`:112-137` 根分裂路径的失败清理完整。

**缺陷 1：B-Tree 是内存树，非磁盘页式 B+Tree「确认·功能缺失」**

节点为堆分配数组（`_btree_node_create`），无页面布局、无叶子链表（范围扫描需中序回溯）、value 内联存储而非堆表 TID。与 PostgreSQL nbtree（页式、叶子链、TOAST 协调）差距是架构级的；与 SQLite 的 cell/paging 层同样差距明显。另外 `_btree_insert_internal:118` 插入前全树 find 一次做重复检查——搜索成本翻倍。

**缺陷 2：代价模型只有骨架，选择率估算仅等值一种「确认·功能缺失」**

`sql/cost.c:205-216`：`estimate_selectivity` 只实现 `1/ndistinct`（等值），其余一律 0.5——无直方图、无 MCV、无相关性（PostgreSQL `selfuncs.c` 的核心）。`compute_agg_cost:146-159`/`compute_sort_cost:173-189` 公式正确但参数固定。join 顺序枚举未实现（见 2.4 缺陷 3），代价模型实际无决策可影响。

**正面证据 2：SQL 特性面远超旧文档记载「确认」**

窗口函数（`nodeWindow.c` 814 行）、CTE（`cte.c` 444 行）、物化视图（`materialized_view.c` 928 行）、分区（`partition.c` 530 行）、并行 Gather（`nodeGather.c` + `parallel.c`）、PG/MySQL 双 Wire 协议 + REST API——特性广度接近 mid-tier 商用产品。

### 2.6 API 设计（评分依据：对外协议面宽，对内契约断裂）

**缺陷 1：TupleTableSlot 不携带 TID，破坏存储-执行器契约「确认·实现质量缺陷」**

ModifyTable 伪造 TID（2.4 缺陷 1）的根源是扫描算子没有把物理行位置放进 slot——`ExecCopyTupleToSlot(slot, tuple, NULL)`（`nodeSeqscan.c:242`）第三参数（应为 tid）传 NULL。契约缺口导致上层只能猜。

**缺陷 2：MVCC API 与执行器 API 双轨平行，无绑定点「确认·实现质量缺陷」**

`txn.h` 的 `txn_begin/commit/rollback`（`txn/txn.c:296-392`）与 `executor.c` 的执行入口互不引用；`heap_visibility.c` 的接口签名（`heap_tuple_visible(xmin, xmax, status...)`）也从未被 heapam 扫描路径调用。两套体系各自测试全绿，合在一起不工作——这是"模块完成度"与"系统完成度"的差距。

**正面证据**：对外 API 面完整——PG Wire（`pgwire.c`）、MySQL Wire、REST（`rest_api.c`）、SDK（`mmdb_sdk.c` 1032 行），兼容策略清晰。

## 3. 业界标杆对比

| 维度 | 自实现 | PostgreSQL 17 | DuckDB | SQLite |
|------|--------|--------------|--------|--------|
| 物理算子 | 12 种（含 Window/ProjectSet/RefreshMview） | 15+ 种 | 20+ 种（向量化） | 少量（简单迭代） |
| 优化器 | 规则全 TODO 桩 + 骨架代价模型 | 动态规划 + GEQO + 自适应 | 代价 + 统计驱动 | 简单规则 |
| MVCC | 4000+ 行实现但零集成 | SSI 完整集成 heap/index | Read Committed | WAL 快照 |
| WAL | 仅 KV 模块接入 | 全 DML 覆盖 + PITR | ACP/Parquet | 全覆盖 |
| 并行 | Gather + Worker Pool 框架 | 并行扫描/连接/聚合 | 全向量化多线程 | 无 |
| SQL 特性 | 窗口/CTE/MView/分区 | SQL:2023 大部分 | SQL:2023 大部分 + 富分析函数 | SQL:2023 大部分 |
| 协议 | PG Wire + MySQL Wire + REST | 原生 + 扩展 | 嵌入式 + HTTP | 嵌入式 |
| B-Tree | 内存教科书版 | 页式 B+Tree + dedup | 压缩 ART/zone map | 页式 B-Tree + cell 层 |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 2 | MVCC 零调用方 `heap_visibility.c:30`；SeqScan 无可见性过滤 `nodeSeqscan.c:223-256`；DML 无事务包裹 `nodeModifyTable.c:56` |
| 崩溃恢复 | 2 | heap DML 无 WAL `heapam.c:294-382`（仅 `kv.c:671` 接入）；无 FSM `heapam.c:305-364` |
| 内存安全 | 6 | memctx 体系扎实（memory: 252/252）；Release 8 存量 UAF；初始化错误路径手工配对 `nodeSeqscan.c:160-176` |
| 错误处理 | 3 | TID 硬编码块 0 `nodeModifyTable.c:70-75,96-99`；DML 失败静默 `:62-64,117`；优化器假开关 `optimizer.c:83-84,199-220` |
| 算法实现质量 | 4 | B-Tree 分裂正确 `btree_insert.c:27-64`；但非页式；选择率仅等值 `cost.c:205-216`；join 枚举未实现 |
| API 设计 | 6 | 协议面宽（pgwire/mysql/rest）；但 slot 无 TID `nodeSeqscan.c:242`、MVCC 双轨无绑定 |

**实现质量缺陷清单（7 项确认）**：
1. UPDATE/DELETE 伪造 TID（块 0 偏移 24）——改错行（错误处理）
2. MVCC 全链路未集成（并发正确性）
3. 关系 DML 无 WAL（崩溃恢复）
4. DML 失败静默吞没 + processed 计数虚增（错误处理）
5. 优化器规则 TODO 桩 + 假开关（错误处理/算法）
6. 无 FSM 导致空间永不复用（功能缺失，存此备查）
7. slot-TID 契约断裂（API 设计）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 | 说明 |
|--------|------|------|--------|------|
| P0 | TID 管道：扫描算子填充 slot TID → ModifyTable 使用真实 TID | 实现质量缺陷 | M | UPDATE/DELETE 改错行是正确性炸弹 |
| P0 | MVCC 接入执行路径：SeqScan 可见性过滤 + heap_insert 戳 xmin + DML 事务包裹 | 实现质量缺陷 | L | 模块已就绪，缺的是接线 |
| P1 | heap DML 接入共享 WAL（扩展 wal_log_type_t） | 实现质量缺陷 | M | 崩溃安全 |
| P1 | DML 错误传播 + 中止语义（失败即停，计数真实） | 实现质量缺陷 | S | |
| P2 | 实现 join 顺序动态规划（≤10 表）或先摘掉假开关 | 功能缺失 | M | 摘开关是 S |
| P2 | 选择率估算扩展（直方图/MCV） | 功能缺失 | M | |
| P2 | B-Tree 页式化或至少叶子链 + FSM | 功能缺失 | L | |
| P3 | 执行器初始化错误路径统一 goto-fail / memctx 化 | 实现质量缺陷 | M | |
| P3 | Release UAF 清零 + CI ASAN（Linux 侧） | 实现质量缺陷 | M | |
