# 多模态数据库工程组织设计：主核形态 + 冻结轨退役

> 状态：设计稿，待用户批准
> 日期：2026-11-18
> 范围：`projects` 仓 —— 主核 `engine/db/` 的工程组织形态，以及冻结轨 `engine/src/db/` 的退役
> 所有「现状」数字均为 **2026-11-18 实测**，核实命令见附录 A

---

## 0. 一句话

**主核保持「一个工程 + 编译期模态裁剪」（不加仓、不加进程、不加插件）；八模态在「SPI 适配器 + 公开 API + REPL 族 + 模态文档」四件套上补全对称（为此给 SPI 加第四契约 `TableOpener`）；文档模态功能补全（接进 SQL）；冻结轨 `engine/src/db/` 全删（209,905 行），SDK 去冻结化（含 memctx 移植）后保留。**

---

## 1. 问题陈述

`projects` 仓当前有**两个数据库内核**：

| | 体量 | 状态 | CI 覆盖 |
|---|---|---|---|
| **主核** `engine/db/` | 247 个 `.c`/`.h` 文件 / **68,030 行**（`.c` 52,393） | **唯一在维护**（2026-11-16 路线翻转） | **零** ❌ |
| **冻结轨** `engine/src/db/` | **524 个 `.c`/`.h` 文件 / 209,905 行**（连非代码共 676 个文件） | 已冻结，保留作历史参考 | **在 CI 里被构建** ✅ |

冻结轨体量是主核的 3 倍多（209,905 行 vs 主核 68,030 行），且主核一行 CI 都没有。这带来四个具体问题：

1. **读者不知道主核在哪** —— 两套内核、两套构建体系（Makefile vs CMake）、两套测试协议、两套目录约定。
2. **CI 门禁打在了不维护的代码上** —— 冻结轨的测试默认还是关的（`ENABLE_FROZEN_SRC_DB_TESTS=OFF`），但它的**库**仍在被构建。
3. **主核的多模态组织方式没有「可独立验证」的机制** —— 目前「模态之间不许互相 include」由 `check_deps.sh` 守着，但**没有任何机械证据证明「某个模态可以单独构建、单独部署」**。

第 3 点引出本设计的主体：**多模态数据库应该以什么工程形态存在**，以及**「八模态」如何在结构上成为事实**。

4. **「八模态」目前只是文档说法，不是结构事实** —— 实测（§2.11）：关系模态的实现与适配器在 `storage/` 与 `spi/`，
   blob 的实现在 `storage/`、适配器在 `spi/`，文档模态**没有公开 API、没有 REPL 族、没接入 SQL**。
   八个模态在「实现目录 / SPI 适配器 / 公开 API / REPL 族 / SQL 入口」五条线上**只齐了 6 个**。

---

## 2. 取证（实测，不是估计）

### 2.1 主核的模态耦合面

每个模态实际 include 的底座头文件（`src/modalities/*/` 全量扫描）：

| 模态 | 引用的底座头 | internal 头泄漏 |
|---|---|---|
| ft | `common/{types,error,config,loghook}` + `storage/{page,pager,value}` + `spi/{storage_am,pushdown}` | **0** |
| ts | 同 ft + `storage/record` + `spi/storage_am` | **0** |
| geo | 同 ft + `storage/record` | **0** |
| vector | + `storage/gstore` + `spi/index_am` | **0** |
| graph | + `common/crc` + `storage/gstore` | **0** |
| doc | `common/{config,error,types}` + `storage/{page,pager,value}` | **0** |

**关键数字**：模态可见的底座接口面 = **13 个头文件 / 1,934 行**（全行口径），占 `src/` 68,030 行的 **2.8%**。
`pager_internal.h` / `heap_internal.h` / `heap_am_internal.h` / `blob_hdr.h` —— 模态一个都没碰。

### 2.2 主核的体量分布

> **口径（2026-11-18 实测）**：统计 `engine/db/src/` 下 **`.c` 文件的物理行数**（不含 `.h`）。
> **全文所有主核体量数字都用这个口径**，避免"同一张表里两种算法"。
> 头文件的量单列在 §2.1。冻结轨的行数用 **`.c` + `.h` 全行**口径（§2.7 的表）。

| 部分 | 行数 | 占比 |
|---|---|---|
| 共享底座 `common`+`storage`+`wal`+`index`+`txn`+`catalog`+`query`+`spi` | **18,108** | 35% |
| 六个模态 `ft` 9,487 / `ts` 5,558 / `geo` 3,310 / `vector` 3,103 / `doc` 3,061 / `graph` 2,279 | **26,798** | 51% |
| 各模态公开 API + REPL（`db_ft.c` / `repl_geo.c` / `main.c` …，顶层 18 个 `.c`） | **7,487** | 14% |
| **合计（`.c` 口径）** | **52,393** | 100% |

> ⚠️ **（复审修正）第三行原来写 6,722，少了 765 行** —— 顶层 18 个 `.c` 文件实测 7,487 行
> （`db_ft.c` 796 / `db.c` 766 / `db_vec.c` 694 / `db_geo.c` 615 / `db_graph.c` 616 / `db_ts.c` 479 /
> `db_stmt.c` 446 / `repl_geo.c` 333 / `db_graph_walk.c` 316 / `db_ckpt.c` 283 / `repl_ts.c` 268 /
> `repl_vec.c` 242 / `db_instance.c` 232 / `db_blob.c` 196 / `repl_ft.c` 166 / `db_result.c` 116 / `main.c` 670）。
> 原值会让三行加起来（51,634）对不上全文的 52,393 —— **表格必须自己加得起来**。

### 2.3 「公共代码抽到单独目录」这个诉求的现状

**已经满足，且是 deep module 形态**：底座 18,109 行对外只暴露 1,934 行头文件；
`tools/check_deps.sh` 强制 L0→L4 单向依赖（L4 模态之间禁互相 include），当前输出 `✓ 依赖方向正确`。

**再抽一个目录是纯改名，零收益。**

### 2.4 多进程部署的阻断项（实测）

| 阻断项 | 证据 | 后果 |
|---|---|---|
| **无文件锁** | `flock` / `fcntl` / `LockFile` 全仓**零命中** | 两个进程同开一个库文件 → 各自缓冲池缓存同一页 → 互相覆盖，**静默损坏** |
| **全局 latch 是进程内互斥量** | `common/latch.c`（pthread） | 跨进程完全无效 |
| **`next_xid` 是进程内计数器** | `txn/txn.c:26` | 两进程各自分配 → 撞号 → 未提交数据被判已提交（坑 M0-3） |
| **WAL 各自追加** | 无共享日志段 | 日志交错 → 恢复时序错乱 |

### 2.5 跨模态事务本来就不存在

`docs/MODALITIES.md` §1.4：ft / vector / graph / ts 全是「锁 + WAL，**语句级提交**」，不参与 BEGIN/COMMIT；
blob 是 xid=0 的不可变对象；只有 heap + JSON 是 MVCC。

**所以「一个库文件」目前真正承载的价值只有三样**：同一份 catalog、同一套缓冲池/WAL/崩溃恢复、将来的 v1.7 跨模态联合查询。

### 2.6 已有机制的复用点

**① `docs/superpowers/specs/2026-10-27-per-modality-build-isolation-design.md`（同一问题，已设计过）**

它选的也是「一个工程 + 编译期裁剪」，机制是：`MMDB_MODALITY` profile 单入口 → 配置期闭包校验 →
注册表反转（生成 `mm_builtin_engines.c`，不用弱符号）→ 每模态 smoke 可执行文件做链接期独立证明 →
依赖白名单登记 → `measure_build.sh` 量化编译单元数。

它做在**冻结轨**上。**主核的底子比旧轨好得多**：旧轨要解决的「487 个 `.c` 中 421 个无守卫」、
「`db_core ↔ db_storage` 循环」、「`core/` 反向依赖全部模态」—— **主核一个都没有**。
搬过去是**减法**。

**② `docs/superpowers/plans/2026-11-16-srcdb-asset-triage.md`（冻结轨逐模块分级）**

已判定：stub 不迁（pg/mysql 协议、`db_server`、`raft_transport`、`mmdb_sdk`、`distributed_query`、REST/`vector_api`）；
值得迁的是 `percolator`/`mvcc_ts`/`tso`/`raft` 状态机、`sql_compat`/`analytics_sql`、RAG 算子框架、协议 codec。
**该清单漏了 `index/`（56,647 行 / 27 个 ANN + 15 个传统索引）** —— 它只盘了「工程面」。

**③ 用户裁决（2026-11-18）**：冻结轨**全删**，含索引动物园与分布式。

### 2.7 冻结轨的依赖分层（实测，本轮补做）

> **口径**：`.c` + `.h` 的物理行数，全树合计 **524 个 `.c`/`.h` 文件 / 209,905 行**。
> （该目录下另有 **142 个 `.txt`**、8 个 `.md`、1 个 `.y`、1 个 `.l` ——
> 连非代码文件一起数是 **676 个文件 / 216,627 行**。**「524 文件」指的是 `.c`+`.h` 的代码文件数**，
> 别与「676 个文件」混用 —— 两个数都对，口径不同。）

| 子树 | 行数 | 是否依赖冻结内核**实现** | 依赖证据 |
|---|---|---|---|
| `index/` | **56,647** | **否** | 只用自己的头 + 7 个冻结**公开头**：`db/core/log.h`、`db/index/{art/art.h,btree/btree.h,heap/heap_vector_store.h,hilbert.h,shared/tree_page.h,vector_index/vector_index_selector.h}` |
| `distributed`+`consensus`+`sharding` | **8,136** | **否** | + `db/shard_routing.h`、`db/distributed/{distributed_query,rpc,rpc_stream}.h`、`db/distributed/txn/distributed_txn.h`、`db/sharding/sharding.h`、`db/core/log.h` |
| `graph/` | 5,850 | **是** | `db/core/func_registry.h`、`db/core/log.h` |
| `tools/` | 2,922 | **是** | `db/` 顶层（CLI/server） |
| `protocol/` | 2,847 | 部分 | `sql_compat.c`/`analytics_sql.c` 独立，另两个是 stub |
| 其余内核本体 | **133,505** | — | storage/sql/parser/executor/core/api/cli/optimizer/vectorized/yang/… （= 209,905 − 上述各行） |

> **措辞修正（2026-11-18 复审）**：`index/` 与 `distributed/` 不依赖冻结内核的**实现**，但**依赖它的公开头**
> （`engine/include/db/` 下的头）。因此 D-1 删 `engine/src/db/` 时，这些子树**暂时还能编译**；
> 但 D-4 删 `engine/include/db/` 时会**一起断** —— 两者必须同批删除，或先把这些公开头一并迁走。
> 这也说明 D-1 与 D-4 **不能拆成两批**（否则中间态编译不过）。

### 2.8 SDK 的真实依赖（推翻了一个直觉）

`engine/src/sdk/` 35 文件 / 7,196 行，7 个模块。**6 个是 SQLite 后端**（`core/sqlite_backend.c`、`text/text_fts5.c`）：

| 模块 | 后端 | 依赖冻结内核？ |
|---|---|---|
| `core`（14） | SQLite | **是** —— memctx（见下） |
| `text`（3） | SQLite FTS5 | **是** —— memctx |
| `graph`（3） | SQLite | 否 |
| `timeseries`（3） | SQLite | 否 |
| `aggregation`（1） | SQLite | 否 |
| `extra`（8） | SQLite + vectors | 间接（memctx + vectors） |
| **`vectors`（3）** | **faiss_hnsw** | **是** —— `vectors.c` 有 22 处 `faiss_hnsw_*` / `vector_index_selector_*` 引用 |

`src/sdk/vectors/index_flat.c` 是个 **dummy**：`int mmdb_index_flat_dummy(void) { return 0; }` —— 正是为此预留的占位。

> **（复审重大修正）SDK 对冻结内核有**第二条**依赖线：memctx。**
> 原文只查了 `faiss_hnsw` / `mm_vector_index` / `sql_engine` 三个链接目标，漏掉了
> **`MemoryContext` 类型与实现都来自冻结内核**（`include/db/sql/memctx.h` + `src/db/sql/memctx.c`）。
> 6 个 SDK 源文件 include `sdk/impl/mmdb_memctx.h`，而它 **真实 include** `db/sql/memctx.h`。
> 详见 §5.6。**这是 D-5 / B-5 摘链接时的真正拦路虎** —— 不是 `faiss_hnsw`，是 memctx。

### 2.9 测试面的依赖分类（实测）

> **口径**：下表「文件数」列数的是**源码文件**（`.c`/`.cpp`），不是目录下的全部文件 ——
> 差异不小：全部文件 / 代码文件（连 `.h`）/ 源码，三者对每个目录分别是
> `db/` 357 / 294 / **289**、`smoke/` 15 / 14 / **13**、`sdk/` 40 / 37 / **36**、
> `index/` 15 / 8 / **8**、`kbase/` 10 / 7 / **7**、`algo/`+`self_made_cpp/` 27 / 14 / **14**。
> 下表用的是**源码数**（即 `.c`/`.cpp`）；写「删 289 个」指的是删 289 个源码文件。

| 目录 | 文件数 | 依赖冻结内核 | 独立 | 处置 |
|---|---|---|---|---|
| `engine/test/db/` | 289 | 289 | 0 | **删** |
| `engine/test/smoke/` | 13 | 13 | 0 | **删**（全是旧轨 profile 冒烟 `mm_*_smoke`） |
| `engine/test/sdk/` | 36 | 7 | 29 | **拆两类**：HNSW **直接**依赖 2 个 + memctx **直接**依赖 5 个（第 6 个间接依赖，合计保留 6 个，见下表） |
| `engine/test/index/` | **8 个 `.cpp`** | **8 个全部**（源码 include `<index/...>` **且**链接目标指向冻结库） | 0 | **全删**（整个 `engine/test/index/` 目录，见下表） |
| `engine/test/kbase/` | 7 | 0 | 7 | 留 —— 但**全部被注释掉**，零注册目标（无害但也无意义） |
| `engine/test/algo/`、`self_made_cpp/` | 14 | 0 | 14 | 留 |

**`engine/test/sdk/` 的 7 个依赖文件必须拆成两类**（复审修正：原文把两类问题混成一条动作）：

| 文件 | 真依赖 | 处置 |
|---|---|---|
| `hnsw/hnsw_filter_test.cpp` | HNSW | **删**（或随 B-7 改造保留） |
| `integration/selector_integration_test.cpp` | `vector_index_selector` | **删**（同上） |
| `memory/mmdb_memctx_test.cpp` | **memctx（间接）** | **保留** —— 它不直接 include `db/sql/memctx.h`，而是经 `sdk/impl/mmdb_memctx.h:25` 间接依赖 ⇒ **移植后同样编译不过** |
| `memory/mmdb_root_context_test.cpp` | **memctx** | **保留** |
| `memory/mmdb_request_scope_test.cpp` | **memctx** | **保留** |
| `memory/sdk_core_memctx_test.cpp` | **memctx** | **保留** |
| `memory/sdk_vectors_memctx_test.cpp` | **memctx** | **保留** |
| `memory/memory_leak_detection_test.cpp` | **memctx** | **保留** |

> **复审修正**：原文写「依赖 HNSW 的 7 个文件」。实测这 7 个里只有 **2 个**依赖 HNSW，
> 另外 **5 个**依赖的是 memctx（`include/db/sql/memctx.h`）。memctx 移植进 SDK 后（§5.6），
> 这 5 个测试**从「待删」变成「移植是否成功的验收依据」** —— 删掉等于自己拆掉证据。

> ⚠️ **（复审核实）口径澄清** —— 上面的「5 个」是**直接** include `db/sql/memctx.h` 的个数
> （`memory_leak_detection_test` / `mmdb_request_scope_test` / `mmdb_root_context_test` /
> `sdk_core_memctx_test` / `sdk_vectors_memctx_test`）；
> 加上表里第 6 个 `mmdb_memctx_test.cpp`（**间接**依赖，经 `sdk/impl/mmdb_memctx.h:25`），
> **实际要保留的是 6 个**。摘要行写「memctx 依赖 5 个」指的是**直接依赖**数，别当成总数。

> ⚠️ **（复审核实）HNSW 那 2 个也核实过**：`hnsw/hnsw_filter_test.cpp` include
> `<db/index/vector_index/faiss_hnsw/faiss_hnsw.h>`；`integration/selector_integration_test.cpp` include
> `"db/index/vector_index/vector_index_selector.h"`。
> （另 3 个文件文本里出现过 `hnsw` 字样，但**只在注释/字符串里**，没有 include 冻结头 ⇒ 不算依赖。）

> ⚠️ **（复审修正）原文写「测试源码确实零依赖冻结内核（grep 验证）」—— 这是错的。**
> 逐文件 grep 的结果是：**8 个 `.cpp` 全部 include `<index/...>`**：
>
> | 文件 | include |
> |---|---|
> | `block/brin/brin_test.cpp` | `<index/block/brin/brin.h>` |
> | `diskann/test_config.cpp` | `<index/vector_index/diskann/diskann.h>` |
> | `diskann/test_fresh.cpp` | `<index/vector_index/diskann/diskann.h>` + `..._fresh.h` |
> | `diskann/test_merge.cpp` | `<index/vector_index/diskann/diskann.h>` + `..._merge.h` |
> | `diskann/test_partition.cpp` | `<index/vector_index/diskann/diskann_partition.h>` |
> | `diskann/test_spann.cpp` | `<index/vector_index/diskann/diskann.h>` + `..._spann.h` |
> | `inverted/bitmap/bitmap_test.cpp` | `<index/inverted/bitmap/bitmap.h>` |
> | `inverted/gin/gin_test.cpp` | `<index/inverted/gin/gin.h>` |
>
> ⇒ 这 8 个文件的源码**依赖**冻结内核，**且**链接目标也指向冻结库 ——
> **两层都依赖**，所以是「**全删**」，不是原文写的「留 5 / 删 3」。
> （原表格自己列的 4 个子目录也全是「删」，与「留 5」自相矛盾。）
>
> ⚠️ **另一层容易漏的事实**：整个 `engine/test/index/` **根本没注册进 CMake** ——
> `engine/test/CMakeLists.txt` 里**没有** `add_subdirectory(index)`；
> 而 `engine/test/index/CMakeLists.txt` 自己只有 `add_subdirectory(inverted)` + `add_subdirectory(block)`，
> 连 `diskann` 都没被列进去。
> ⇒ **这棵树从来没被构建过，贡献 0 条 ctest 用例** ⇒ 删它对 ctest 用例数**零影响**。
> ⇒ 这也修正了 R-06 的判据：**ctest 用例数差值里不应计入 `engine/test/index/`**。

| 子目录 | 链接目标 | 判定 |
|---|---|---|
| `block/brin/` | `mm_vector_index` | **删**（随 D-1 一起失效） |
| `inverted/bitmap/` | `mm_vector_index` | **删** |
| `inverted/gin/` | `mm_vector_index` | **删** |
| `diskann/` | `diskann_index` | **删** —— 且该目标名**本就不存在**（真名是 `diskann`，`src/db/index/vector_index/diskann/CMakeLists.txt:22`），这个测试**现在就是坏的** |
| `block/CMakeLists.txt`、`inverted/CMakeLists.txt` | 仅 `add_subdirectory` | 随子目录删 |

> **教训**：判断「某个测试目录能不能留」必须查**两层** —— ① 源码 include ② CMake `target_link_libraries`。
> 只查 ① 会漏掉「源码干净但链接目标没了」这一类。

### 2.10 文档引用面

**912 处** `src/db/` 引用：`docs/analysis` 869 + `docs/arch` 32 + `docs/storage` 8 + `openspec/specs` 3。
其中**带精确行号的引用实测为 0** → `scripts/check-wiki-refs.py` 不会因行号越界失败，但会产生死链。

### 2.11 八模态的对称性缺口（实测）

八模态在**五条线**（实现 / SPI 适配器 / 公开 API / REPL 族 / SQL 入口）上的对齐情况：

| 模态 | 实现 | SPI 适配器 | 公开 API | REPL 族 | SQL 入口 |
|---|---|---|---|---|---|
| 0 关系 | `storage/heap*`（**L1，被 L2 依赖**） | `spi/heap_am*` ❌ | **—**（对外接口即 SQL，见下注） | SQL | catalog 真表 |
| 1 blob | `storage/blob*` ❌ | `spi/blob_am*` ❌ | `db_blob.c` ✓ | BLOB ✓ | blob_frames/blob_objects |
| 2 图 | `modalities/graph/` ✓ | `graph_am.c` ✓ | `db_graph.c` ✓ | GRAPH ✓ | graph_nodes/graph_edges |
| 3 向量 | `modalities/vector/` ✓ | `vector_am.c` + `hnsw_am.c` ✓ | `db_vec.c` ✓ | VEC ✓ | vec_items |
| 4 全文 | `modalities/ft/` ✓ | `ft_am.c` ✓ | `db_ft.c` ✓ | FT ✓ | ft_docs |
| 5 空间 | `modalities/geo/` ✓ | `geo_am.c` ✓ | `db_geo.c` ✓ | GEO ✓ | geo_points |
| 6 时序 | `modalities/ts/` ✓ | `ts_am.c` ✓ | `db_ts.c` ✓ | TS ✓ | ts_readings/ts_partitions |
| 7 **文档** | `modalities/doc/` ✓ | **无**（寄生模态，设计如此） | **无** ❌ | **无** ❌ | **未接入** ❌ |

> **关系模态的「公开 API」标记为「—」而非「❌」**：它的对外接口就是 SQL 本身
> （`db_exec` + DDL/DML），没有独立的 `db_rel_*` 函数族是**角色决定的，不是缺口**。
> 原文标 ❌ 会与 §6.3 的 S4「不为了对称而造空壳」自相矛盾 —— 同一事实，一处当缺陷、一处当设计。

**三条实测结论**：

**① 关系模态的适配器搬不进 L4 —— 被执行器卡住**

```
query/exec_internal.h:5    #include "spi/heap_am.h"     → HeapAmCtx 类型
query/exec_internal.h:80   HeapAmCtx am_ctx;            → TableCtx 字段
query/exec_scan.c:222,286  heap_am_build(...)           → 2 处调用
```

其余 6 个模态的适配器能在 L4，是因为**执行器从不直接调用**它们（只拿 `StorageAM*`）。
关系模态的表不走注册表（catalog 真表），执行器必须**主动构造**它的 AM。

**② 关系模态的实现也搬不进 L4 —— 被 L2 卡住**

```
catalog/catalog.h:8   #include "storage/heap.h"   →  Heap *catalog_heap(Catalog*, u32)
txn/vacuum.h:6        #include "storage/heap.h"   →  int vacuum_table(Heap *h, ...)
```

L2 直接持有 `Heap*` → 搬到 L4 就是 `L2 → L4` 反向依赖，门禁必红。

**③ blob 零反向依赖 → 可完整搬进 `modalities/blob/`**

全仓只有 `spi/blob_am.h` 依赖 `storage/blob.h`，而适配器一起搬 → 无残留。

**补充**：执行器是全仓唯一直接认识模态的地方，而 `spi/heap_am.h` 的注释自己承认 ——
*"如果关系模态直接被执行器特殊处理（现在是这个状态），那'模态'就只是'blob 那一块'，SPI 是假的"*。

### 2.12 文档模态的功能缺口（对照 `PLAN-v1.6`）

| PLAN-v1.6 步骤 | 状态 | 证据 |
|---|---|---|
| 步骤 1 JSON parser + JSONB 编解码 | ✅ 已完成 | `json_parse.c` / `jsonb.c` / `json_scalar.c` / `jsonb_get.c`；`test_jsonb.c`、`test_jsonb_crash.c` 全绿 |
| 步骤 2 路径表达式 + 标量映射 | ✅ 已完成 | `json_path.c`；`test_json_path.c` 全绿 |
| 步骤 3 heap 列类型 + 大文档外置 | 🟡 **收尾** | **heap 记录编解码已真的走 `T_JSON`**（复审实测）：`record.c:20` `fixed_len()` 判 `T_JSON` 返回 0（变长）、`record.c:40` 类型白名单含 `T_JSON`、`record.c:113-121` 变长区与 `T_TEXT` 共用字节区并按 `cols[i].type` 区分。**只剩「大文档外置接 `PT_JSON_OVF` 页链」一件事** |
| 步骤 4 路径索引（写时维护） | ❌ 未做 | 无 `CREATE INDEX ... (json '$.a.b')` 路径；且 **parser 不支持路径字面量**（`parser_stmt.c:92` 是 `p_expect_ident(index_col)`） |
| 步骤 5 执行器接线 + `json_match` | ❌ 未做 | `include/db.h` 里**零个** `db_json_*`；`src/query/` 里零个 json。**且 parser 没有通用函数调用节点**（`parser.h:48-52` 只有 `EX_LIT/EX_COL/EX_BIN/EX_UN/EX_AGG`） |
| 步骤 6 MVCC + 崩溃 + 反向验证 + 收官复盘 | ❌ 未做 | — |

> **复审实测的两条正面结论**：
> ① **步骤 3 比原判更接近完成** —— 原文标「heap 记录编解码是否真的走 `T_JSON` 未验证」，
>    实测三处（`record.c` 的 `fixed_len` / 类型白名单 / 变长区）**都已处理 `T_JSON`**。
> ② **页类型隔离是对的**：`json_ovf.h:82` 自定义 `#define PT_JSON_OVF 0x1Bu`，
>    **没有**进 `common/config.h` 的页类型枚举（该枚举止于 `PT_FT_CATALOG = 0x14`）。
>    这正是「模态自持页类型、不污染 L0」的正面样板，应在 §6.4 明说并保持。

### 2.13 计划编写期的复核发现（2026-11-18）

写三份实施计划（W-A 冻结轨退役 / W-B 主核工程组织 / W-C 八模态对称化）时逐文件核对源码，
发现下列与本设计文档**不一致**或**未记录**的地方。每一条都已落到对应计划里；
本节记录**证据**与**结论**，供复审逐条对照。

| # | 发现 | 证据 | 结论 | 落在 |
|---|---|---|---|---|
| R-01 | **`faiss_hnsw.h` 这个头本身住在冻结轨的源码树里** | `engine/src/db/index/vector_index/faiss_hnsw/faiss_hnsw.h` | §2.8 只说「`vectors.c` 依赖 faiss_hnsw」。实情更糟：它 include 的是一个**将被整棵删掉的头** | W-A Task 3 |
| R-02 | **`vector_index_selector.h` 在 `engine/include/db/`，实现却在冻结轨** | 头与实现分属两侧 | 删哪一边都不够 ⇒ 头与实现**必须一起**移植 | W-A Task 3 |
| R-03 | **`memctx.h` 依赖 `db/parser/sql/parsenodes.h`** | `engine/include/db/sql/memctx.h` include 它 | 该头**也在删除清单里** ⇒ 移植时不能保留（它实际只用 114 个 `NodeTag` 值里的 2 个） | W-A Task 2 |
| R-04 | **blob 与 graph 的 REPL 不在 `repl_<m>.c` 里** | `handle_blob` / `handle_graph` 是 `src/main.c` 内的 `static` 函数 | §6.1 的「③ REPL 族 = `src/repl_<m>.c`」对这两个模态**不成立** ⇒ 对称化动作清单要加两条：搬成 `repl_blob.c` / `repl_graph.c` | §6.1（已补） |
| R-05 | **§5.6 的「5 个 memctx 测试」是笔误** | `engine/test/sdk/memory/` 实有 **6** 个 `.cpp` | §2.9 的 6 行清单是对的；§5.6 的两处「5 个」已改正 | §5.6（已改） |
| R-06 | **doc 模态的完整计数是 3,787 行，不是 2,829** | `src/modalities/doc/` 10 个文件（.c + .h） | 2,829 是早期**部分计数**。⚠️ §2.2 该表的合计口径与本次测量**不同**，故**不做加减**（避免改出一组自相矛盾的总和）—— 权威数字由 W-B 的 `tools/measure_build.sh` 产出 | W-B Task B5 |
| R-07 | **`engine/test/index/` 的源码 include `<index/...>`** | 这些路径只能经 `engine/include/db/` 解析 | 删掉 `engine/include/db/` ⇒ 这 8 个文件**连编译都过不了**，不只是链接目标消失。**（复审核实：8 个 `.cpp` 全部 include `<index/...>`，§2.9 原写「源码零依赖」是错的，已订正）** | W-A Task 4 |
| R-08 | **`ALLOCSET_PRESET_SMALL高频` 是混写标识符** | 全仓仅 **2 处**：`memctx.h:58`（枚举定义）+ `memctx.c:543`（case） | 移植时**逐字保留** —— 改名要连带改测试，且属"顺手优化相邻代码" | W-A Task 2 |
| R-09 | **`SQL_MAX_COLS` 是 32，注释仍写 16** | `query/parser.h:36`、`query/exec_internal.h:69-72` | 既有文档债，**本轮不改**（精准修改纪律），但新代码按 32 写 | W-C Task C1 |
| R-10 | **裁剪点是五处不是四处** | `src/main.c` 直接调用 `handle_vec` / `handle_ft` / `handle_geo` / `handle_ts` 与 `static` 的 `handle_blob` / `handle_graph` | §4.2 补第 5 处（REPL 入口级）；且 `db_internal.h` 的**模态函数声明**也要守卫 —— 否则 `db_ft_recovery_hook` 的注册点链接失败 | §4.2（已补） |
| R-11 | **`storage/gstore*` 是 graph 与 vector **共用**的底座** | `vector_am.c:125` 调 `gstore_cursor_init(vstore_meta(...))` | 它**不属于 graph**，不能搬进 `modalities/graph/`，必须留在 L1 | W-C Task C3 |
| R-12 | **`test/run.sh` 硬编码了 `build/`** | `OBJS=$(find build -name '*.o' ...)` | 裁剪 profile 的对象在 `build_mm_<m>/` ⇒ `run.sh` 必须读 `OBJ_DIR` 环境变量 | W-B Task B3 |
| R-13 | **`make bench` 只在 `MM=full` 下可用** | bench 目标链 `vec_bench` / `graph_bench` / `ft_bench` / `geo_bench` / `ts_bench` | 裁剪 profile 下要**显式拒绝**（给出可读信息），而不是让它链接失败 | W-B Task B2 |
| R-14 | **主核测试是 95 个文件（64 unit + 31 integration）** | `test/run.sh` 的 glob 实测 | `run.sh` 头部注释写的「93 个测试」也已过期 ⇒ 所有基线数字按 95 记 | W-A / W-B 基线 |
| R-15 | **`SQL_MAX_COLS` 定义在 `query/parser.h`，而 `spi/` 要用它** | 依赖方向是 L3→L3 但**方向反了** | 挪到 `spi/table_opener.h` 或 `common/types.h`，`query/parser.h` 反过来 include | W-C Task C1 |
| R-16 | **`db.c` 766 行 / `main.c` 670 行 / `db_instance.c` 232 行** | 实测 | 都在 800 行硬规则内，但 `db.c` 已接近上限 ⇒ C-1 往里加 opener 安装点时**不要**再堆代码 | W-C Task C1 |
| **R-17** | **仓库自己的文档里，主核的测试数与超限文件清单都已过期** | `CLAUDE.md`「主数据库内核」节写「`sh test/run.sh` — **55 个测试文件**，73 处断言」；同节把 `src/storage/heap.c`、`src/db.c`、`src/storage/pager.c` 列为「既有超限文件」 | 实测：`test/unit/` **64** 个 + `test/integration/` **31** 个 = **95** 个测试文件（`run.sh` 的 glob 就是 `test/unit/test_*.c test/integration/test_*.c`）；三个「超限文件」实测 **614 / 766 / 795** 行，**全部 ≤ 800**，早已不超限 | W-A Task 5 |
| **R-18** | **`test/run.sh` 的头部注释写着「实测 93 个测试里没有任何两个共用同一个 .db 路径」** | `test/run.sh:15` | 实测是 **95** 个。这条注释是**并发安全性的证据**（"各测试互不干扰"），数字错一点不影响结论，但会让复核者对不上 | W-A Task 5 |
| **R-19** | **`memctx` 的行数被记小了** | `engine/include/db/sql/memctx.h` 实测 **503 行**、`engine/src/db/sql/memctx.c` 实测 **834 行** | 本文档 §5.6 原写「742 行 `memctx.c` + 354 行 `memctx.h`」「其余 352 行」—— 三个数字都偏小。**移植的工作量估计按偏小的数给的**，已订正 | §5.6（已改） |
| **R-20** | **本文档的体量数字是「早快照」，且口径未标注** | §2.2 的表、§2.7 的冻结轨分层表、§2.1 的「1,313 行 / 2.6%」 | 三处都**没写统计口径**，读者无从判断"行数"是含头文件、含注释、含空行还是只算 `.c`。本文档 §2.2 现在把口径写死为「`.c` 文件的物理行数」、§2.7 写死为「`.c` + `.h` 全行」，并按 2026-11-18 实测值重算 | §2.1 / §2.2 / §2.7（已改） |
| **R-21** | **冻结轨的体量比本文档记录的还大** | `engine/src/db/` 实测 **524 文件 / 209,905 行**（`.c` + `.h`）；`index/` 子树 **56,647 行**（原记 46,805） | 「冻结轨是主核 3 倍多」这个**结论方向不变**，但绝对值全线偏小。删除的**风险评估**（§7）用的是偏小的数 ⇒ 实际删除量更大 | §2.7 / D-1 / D-4（已改） |

---

## 3. 判决

| # | 判决 | 理由 |
|---|---|---|
| **D1** | 主核保持**单仓单二进制**，用**编译期模态裁剪**实现「模态可单独构建/单独部署」 | 模态共享同一套运行时（pager/WAL/xid/缓冲池），进程内耦合无法靠目录结构切断；§2.4 的**四项**阻断决定了多进程当前不可行 |
| **D2** | **不做**插件化（`.so` 动态加载） | 要把 C 头文件 SPI 升级成稳定 ABI 契约（结构体布局 / 函数指针表 / 内存所有权跨模块 / CRT 差异），成本远大于「发版便利」的收益；且 `spi/storage_am.h` 151 行含 11 个宏与 14 个函数指针字段，ABI 易碎 |
| **D3** | **不重构 L1–L3 的公共机制**（pager / WAL / btree / txn / catalog / query 的内部形状与已有 SPI 三契约一律不动）；**模态归属的搬迁由 D9 管辖** | §2.1/§2.3：接口面已收敛到 1,934 行薄契约、零 internal 泄漏。**（复审修正）** 原文只写「不重构目录」，但 `storage/` 就是 L1 底座，把 `blob.c` 搬走属底座改动 —— 两个判决会在实施时打架，故收窄为「不动 L1–L3 的**机制与形状**」 |
| **D4** | **删除** `engine/src/db/` 全树（524 文件 / 209,905 行）+ `engine/test/db/`（289）+ `engine/include/db/`（**409 头**） | 用户裁决 2026-11-18。§2.2：冻结轨体量是主核 3 倍多（209,905 行 vs 主核 68,030 行）且无人维护。**D-1 与 D-4 必须同批删**（§2.7：`index/`、`distributed/` 依赖 `include/db/` 的公开头，拆批会中间态编译不过） |
| **D5** | **保留 SDK，去冻结化**：摘掉 `faiss_hnsw`/`mm_vector_index`/`sql_engine` 三个链接依赖；`sdk/vectors` 的 HNSW 后端改为 `index_flat.c` 实现真扁平索引；**并把 memctx 移植进 SDK 内部**（§5.6） | §2.8：SDK 6/7 模块是 SQLite 后端，与冻结内核无关；`index_flat.c` 本就是为此预留的占位。**（复审修正）** 原文漏了 memctx —— SDK 的 `MemoryContext` 类型**真的**来自 `include/db/sql/memctx.h`，实现**真的**在 `src/db/sql/memctx.c`，不处理则 `mmsdk` 链接必失败 |
| **D6** | **主核补 CI** | 主核 52,393 行 `.c`（含 `.h` 是 68,030 行 / 247 文件）/ 95 个测试文件（64 unit + 31 integration）当前**零 CI 覆盖**，而冻结轨在 CI 里 |
| **D7** | 编译期裁剪用**单一 `MM` 变量**（不是 14 个独立布尔开关） | 2026-10-27 设计明列的反目标：「宏组合爆炸」 |
| **D8** | **方案 C（多进程 / 多仓）留预备**，seam 固定为 §2.1 的 1,934 行头文件 | 真要拆的那天按这条缝切即可。现在拆只会先付进程间协调成本，且会堵死已排期的 v1.7 跨模态联合查询 |
| **D9** | **八模态对称化**：8 个模态统一为「SPI 适配器 + 公开 API + REPL 族 + 模态文档」四件套，且**适配器一律在 `src/modalities/<m>/` 下** | §2.1：模态可见的底座接口面已是 1,934 行薄契约，**边界够干净到可以谈对称**；对称化让「八模态」从文档说法变成结构事实 |
| **D10** | **给 SPI 加第四个契约 `TableOpener`**：关系模态的适配器进 L4，执行器不再 include 任何模态头 | 关系模态是唯一被执行器直接调用的模态（`exec_scan.c` 的 `heap_am_build`）。它是「底座型模态」，实现横跨 L1–L3，搬不进 L4 —— 只有把「表打开」抽象成契约才能真正对称。**这也是 v1.7 跨模态联合查询的前置** |
| **D11** | **文档模态功能补全**：执行 `PLAN-v1.6` 步骤 3~6，把 JSON 接进 SQL（`json_match` + 路径索引） | §2.12：JSONB 编解码/路径/溢出链已实现且测试全绿（3,787 行），但 `db.h` 里零个 `db_json_*`、无路径索引、未接执行器 —— 八模态里唯一功能未闭环的 |

### 3.1 为什么 D1 不是「凑合」而是正解

多模态数据库的价值命题是**「一个库、一套事务、一条查询路径跨多种数据模型」**。
把它拆成多进程/多仓，等于提前放弃这个命题，换来的是：

- N 份构建与测试基础设施
- SPI 变更要跨仓协调版本（每次改 `storage_am.h` = N 个仓的 PR，临时不可编译状态）
- §2.4 的进程间协调成本
- 跨模态事务（当前本来就没有）变得**永久不可能**

而「单独部署」这个诉求，编译期裁剪已经能满足 90%：**产物里只有选中的模态，功能上就是一个单模态数据库**。
剩下 10%（底座 19k 行无法从产物剔除、不能独立扩缩容）才是 D8 的预备内容。

---

## 4. 设计 A：主核工程组织

### 4.1 目录不动

```
engine/db/
├── src/
│   ├── common/ storage/ wal/            L0–L1  底座
│   ├── index/ txn/ catalog/             L2
│   ├── query/ spi/                      L3     SPI = 唯一扩展点
│   ├── modalities/{doc,ft,geo,graph,ts,vector}/   L4
│   └── db_*.c repl_*.c                  各模态公开 API + REPL
└── tools/check_deps.sh                  分层门禁
```

### 4.2 `MM` 编译期模态开关

**单一入口**：`make MM=relational` / `blob` / `graph` / `vector` / `ft` / `geo` / `ts` / `doc` / `full`（默认）。

> **完整的 8 个 profile 列表与依赖闭包规则见 §6.5** —— 本节讲机制，§6.5 讲 8 模态的取值。

- profile 定义写在**一个**文件 `engine/db/tools/mm_profiles.mk`（唯一真相源），
  每个 profile 显式列出它打开的模态集合，每个模态声明自己的**依赖**（由闭包校验器算出实际集合，不手工指定）。
- Makefile 从 profile 推导 `-DDB_HAS_<MOD>=0/1` 并**裁剪源文件**：
  未选中的模态，其 `src/modalities/<m>/`、`src/db_<m>.c`、`src/repl_<m>.c` 不进 `SRCS`。
- **闭包校验在配置期做**：profile 声明的模态集合必须对其依赖闭包封闭，否则 `make` 直接报错并打印差集。
  （不允许「选了 vector 却没编它的依赖」这种半配置状态。）

**裁剪点只有五处**，不做满地 `#ifdef`（2026-10-27 设计的反目标）：

| # | 位置 | 做法 |
|---|---|---|
| 1 | 源文件级 | Makefile 的 `SRCS` 过滤 —— 裁掉源文件即裁掉该模态的注册代码 |
| 2 | 实例结构体级 | `src/db_internal.h` 的 per-modality 字段与 `#include` 用 `#if DB_HAS_<MOD>` 包裹 |
| 3 | 实例初始化级 | `src/db.c` 里 `db_vec_open` / `db_graph_open` / … 的调用点用 `#if DB_HAS_<MOD>` 包裹 |
| 4 | **实例释放级** | `src/db_instance.c` 里的 `db_<m>_close` / `blob_store_destroy` / `graph_destroy` 调用点 —— **复审补入** |
| 5 | **REPL 入口级** | `src/main.c` 里的 `handle_<m>` 派发与 `#include "repl_<m>.h"` —— **计划编写期补入** |

> **（计划编写期补入的第 5 处）**：`main.c` 是命令行 REPL 的前端，**直接**调用各模态的
> `handle_*`：外部的 `handle_vec` / `handle_ft` / `handle_geo` / `handle_ts`
> （定义在 `repl_*.c`），以及**定义在 `main.c` 自己内部**的 `handle_blob` / `handle_graph`。
> 不裁它 ⇒ 裁剪出来的二进制链接不过（`repl_vec.h` 被裁掉了，但 `main.c` 还在 include 它）。
> ⚠️ `handle_blob` / `handle_graph` 是 `static` 函数 —— 整个函数体连同它的 static 辅助函数
> 要一起包进 `#if`，否则 `-Werror` 的 `-Wunused-function` 会报「未使用的静态函数」。

> **（计划编写期补入的第 2 处补充）**：`db_internal.h` 里除了字段与 `#include`，
> 还有**模态专属的函数声明**（`db_vec_open` / `db_ft_open` / `db_ts_open` / `db_geo_open` /
> `db_graph_register` / `db_ft_recovery_hook` / `GeoPtEntry` / `GeoTab` / `FtReplaySet` …）。
> 它们也必须用 `#if DB_HAS_<MOD>` 包起来 ——
> 不包不会编译失败（声明不存在的函数是合法的），但会留下误导性残留；
> 且 `db_ft_recovery_hook` 的**注册点**（`db.c` 里的 `recovery_set_hook(db_ft_recovery_hook, inst)`）
> 不守卫就会**链接失败**。

> **复审补入的第 4 处（重要）**：模态的生命周期有**两半** —— 打开在 `db.c`，**关闭在 `db_instance.c`**，
> 而且 `db_instance.c` 有**两条关闭路径**（正常关闭 + 错误回滚）。实测该文件里的模态调用点共 **11 处**（复审订正：原写 10 处，漏了 L187 `graph_destroy`）：
>
> ```
> L55   db_vec_close(inst)          L125  blob_store_destroy(inst->blobs)
> L67   db_ft_close(inst)           L126  graph_destroy(inst->graph)
> L78   db_geo_close(inst)          L186  blob_store_destroy(inst->blobs)   ← 错误路径
> L90   db_ts_close(inst)           L187  graph_destroy(inst->graph)        ← 错误路径
>                                    L207  db_ft_close(inst)
>                                    L213  db_geo_close(inst)
>                                    L219  db_ts_close(inst)
> ```
>
> 只裁 `db.c` 而不裁 `db_instance.c` 的后果有二：
> ① 裁掉 vector 后 `db_vec_close` 未声明 → **编译失败**；
> ② 若用 `#if` 只包住正常路径而漏掉错误路径 → **运行期重复释放 / 泄漏**。
> 验收必须同时覆盖两条路径。

**不需要**旧轨那套「注册表反转 + 生成 `mm_builtin_engines.c`」：
主核的注册本来就是**分散在每个模态自己的公开 API 文件里**（`db_vec.c` 注册 `vec_items`、
`db_graph.c` 注册 `graph_nodes`/`graph_edges`），裁掉源文件即裁掉注册。

### 4.3 每模态 smoke 目标（独立可用的机械证明）

`make smoke` 产出 8 个可执行文件（`build/smoke_<m>.exe`），每个**只链接**：L0–L3 底座
+ 该模态自己的源文件 + **只打开该模态的组合根**（`db.o` / `db_instance.o`，按模态单独编译一份）。

它存在的唯一意义是**链接期证明**：如果这个模态偷偷依赖了别的模态，链接会失败。
内容：初始化 → 建一个最小对象 → 写一条 → 读回 → 断言 → 关闭。

> **（计划编写期修正）为什么要单独编译一份根**：组合根 `db.c` / `db_instance.c` 调用各模态的
> open/close。若直接用 full 构建里的根对象文件，链接器会把**所有**模态的符号都拖进来 ⇒
> 证明作废。所以每个模态单独编一份根（`-DDB_HAS_<MOD>` 只开它自己）——
> 多出的代价是 8 × 2 个小文件的重编。
>
> **（计划编写期修正）smoke 的产出方式**：`make smoke`（在 `MM=full` 下）产出全部 8 个；
> `make MM=vector` 产出 `build_mm_vector/smoke_vector.exe`。
> 两种都要有 —— 前者证明"8 个模态各自独立"，后者证明"这个裁剪配置是能用的"。

`make MM=full` 时产出**全部 8 个** smoke（完整列表见 §6.5）：
`smoke_relational` / `smoke_blob` / `smoke_graph` / `smoke_vector` / `smoke_ft` /
`smoke_geo` / `smoke_ts` / `smoke_doc`。

> `smoke_doc` 与其他七个不同：文档模态是**寄生模态**，没有独立存储与虚拟表，
> 它的 smoke 验的是「JSONB 编解码 + 路径提取 + 标量映射」，**不验 SQL 路径**（除非 §6.4 的 C-4 已完成）。
> 这一点必须写在 smoke 的注释里，否则会让人误以为文档模态已可从 SQL 访问。

### 4.4 量化脚本

`engine/db/tools/measure_build.sh`：给定 `MM`，打印「编译单元数 / 产物大小 / 模态依赖闭包」。
**门禁值在改造前先实测一次**并写入 `docs/arch/db/multimodal-build-baseline.md`，后续以实测值为准（门禁 = 实测值 × 1.2）。
理由：2026-10-27 设计的教训 —— 估算数字不作验收判据。

### 4.5 CI（P0，最高优先级）

`.github/workflows/ci.yml` 现有 5 个 job **全是 CMake 工程轨**，主核一次都没跑过。
新增**两个** job（Linux + Windows）—— 主核是 POSIX sh + Makefile 工程，
**Linux 用 `make`，Windows 用 `mingw32-make`，不能混用**（复审修正：原文把 `mingw32-make` 写进了 ubuntu job，会 `command not found`）。

```yaml
# ---- 新增 job 1：Linux ----
  build-main-kernel-linux:
    name: Build & Test Main Kernel (Linux/GCC)
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Build main kernel
        run: |
          cd engine/db
          make clean && make all          # -Wall -Wextra -Werror，零警告
      - name: Dependency gate
        run: sh engine/db/tools/check_deps.sh    # 期望 ✓ 依赖方向正确
      - name: Unit & integration tests
        run: sh engine/db/test/run.sh            # 期望 0 FAIL

# ---- 新增 job 2：Windows ----
  build-main-kernel-windows:
    name: Build & Test Main Kernel (Windows/MinGW)
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with: { msystem: MINGW64 }
      - name: Install toolchain
        run: pacman -S --needed --noconfirm mingw-w64-x86_64-make mingw-w64-x86_64-gcc
      - name: Build main kernel
        shell: msys2 {0}
        run: |
          cd engine/db
          mingw32-make clean && mingw32-make all
      - name: Dependency gate + tests
        shell: msys2 {0}
        run: |
          sh engine/db/tools/check_deps.sh
          sh engine/db/test/run.sh
```

**失败即红灯** —— 主核从此不再是「无门禁区」。

> **本地执行注意**（复审补入）：Windows 上 `sh` **不在 PATH**，
> 但 Git 自带 `C:\Program Files\Git\usr\bin\sh.exe`；`mingw32-make` 与 `gcc` 在 `C:\mingw64\bin`（在 PATH）。
> 本地跑门禁必须**显式调用绝对路径**或先注入 PATH，否则会以为「脚本坏了」：
>
> ```powershell
> cd C:\code\projects\engine\db
> & "C:\Program Files\Git\usr\bin\sh.exe" tools/check_deps.sh
> & "C:\Program Files\Git\usr\bin\sh.exe" test/run.sh
> mingw32-make clean; mingw32-make all
> ```

### 4.6 阶段与顺序

| 阶段 | 内容 | 前置 |
|---|---|---|
| **P0** | 主核 CI（§4.5） | 无 —— **先做**，因为它给后面所有改动兜底 |
| **P1** | `MM` 编译期开关 + 闭包校验（§4.2） | P0 |
| **P2** | 每模态 smoke 目标 + `measure_build.sh`（§4.3/§4.4） | P1 |
| **P3** | D8 预备：把 seam（1,934 行头文件）整理成可发布的头文件包 | P1–P2 完成后再评估 |

> 冻结轨退役（§5）是**独立工作流 W-A**，不属于本表的阶段序列 —— 它与 P0/P1/P2 的交叉点只有一条：
> **W-A 必须在 P0 之后执行**（§9）。八模态对称化（§6）是**独立工作流 W-C**，与 P1 的先后见 §9。

---

## 5. 设计 B：冻结轨退役

### 5.1 删除清单

| # | 对象 | 规模 | 依据 |
|---|---|---|---|
| D-1 | `engine/src/db/` | 524 文件 / 209,905 行 | 用户裁决；§2.7 依赖分层已查实 |
| D-2 | `engine/test/db/` | 289 文件（≈587 条 ctest 用例） | 测的都是被删的内核 |
| D-3 | `engine/test/smoke/` | 13 文件 | 全是旧轨 profile 冒烟（`mm_*_smoke`） |
| D-4 | `engine/include/db/` | **409** 个 `.h` | 冻结内核的公开 API。**必须与 D-1 同批删**（§2.7：`index/`、`distributed/` 依赖这里的头） |
| D-5 | `engine/include/rag/`、`engine/include/tree_index/` | **2 + 8** 头 | 分别对应 `executor/rag/` 与 `src/db/index/` 的传统索引 |
| D-6 | `engine/include/sdk/impl/mmdb_memctx.h` 的 **include 与类型依赖** | 1 头 + 6 源文件 + 5 测试 | **（复审重大修正）** 原文判断「SDK 公共头零处引用冻结内核类型，只需订正注释」—— **实测相反**：<br>`include/sdk/impl/mmdb_memctx.h:25` **真实存在** `#include "db/sql/memctx.h"`，<br>L32 `typedef MemoryContext mmdb_memctx_t;` 直接 typedef 自冻结内核类型。<br>6 个 SDK 源文件 include 它；`palloc`/`pfree`/`AllocSetContextCreate`/`MemoryContextDelete` 的**实现体**在 `src/db/sql/memctx.c`（834 行）；`mmdb_mem_register_resource` **只在该文件定义**（L783）而 `sdk/core/mmdb.c:199` 在调用。<br>→ **处置见 §5.6「SDK 内存上下文去冻结化」**（把 memctx 移植进 `sdk/core/`），**不是订正注释** |
| D-7 | `engine/cmake/ModalityProfiles.cmake`、`engine/cmake/MultimodalConfig.cmake` | 2 文件 | 旧轨的 profile 机制；主核用 Makefile，不进 CMake 图 |
| D-8 | `engine/tools/benchmark/c_api/` | 目录 | 链 `mm_vector_index`/`faiss_hnsw`；**删了必须同时删 `tools/benchmark/CMakeLists.txt:5` 的 `add_subdirectory(c_api)`**（见 B-9） |
| D-9a | `engine/test/sdk/hnsw/hnsw_filter_test.cpp`、`engine/test/sdk/integration/selector_integration_test.cpp` | 2 文件 | 依赖 HNSW / `vector_index_selector`（随 B-7 改造或删除） |
| D-10 | `engine/test/index/{block/brin,inverted/bitmap,inverted/gin,diskann}/` | **4 个索引类型目录 / 8 个 `.cpp` 测试文件** | 链接目标 `mm_vector_index` / `diskann_index` 随 D-1 消失（§2.9）。`diskann/` 那个**现在就是坏的**（目标名应为 `diskann`）。⚠️ **口径**：这里的「4」是**索引类型目录数**（brin / bitmap / gin / diskann），而实际要删的是这 4 个目录下的 **8 个 `.cpp` 文件** —— 两个数不矛盾，但**别混用**。且整棵树未注册进 CMake ⇒ 删它对 ctest 用例数零影响（§2.9） |

> **D-9 的编号拆分（复审修正）**：原文 D-9 是「`engine/test/sdk/` 中依赖 HNSW 的 7 个文件」。
> 实测这 7 个里只有 **2 个**依赖 HNSW（→ D-9a），另外 **5 个**依赖的是 **memctx** ——
> 它们**不在删除清单里**，而是 §5.6 memctx 移植的**验收依据**（见 §5.2 保留清单）。
> 原文把它们混进 D-9，会在实施时把移植的证据一起删掉。

### 5.2 保留清单（**不动**）

| 对象 | 说明 |
|---|---|
| `engine/src/{algo-prod,cpp,ds,redis}/` | 与冻结内核无关 |
| `engine/src/sdk/` | 7 个模块，去冻结化后保留（D5） |
| `engine/test/{algo,self_made_cpp}/` | §2.9 独立部分 |
| `engine/test/kbase/` | 7 文件 —— **全部被注释掉**（`test/kbase/CMakeLists.txt` L4–L43 无一行生效），零注册目标。留着无害，但也无意义；本设计**不动它**（不属本设计范围） |
| `engine/test/sdk/memory/` 的 6 个 memctx 测试 | §5.6 移植后是验收依据 |
| `engine/test/sdk/` 其余 29 个独立文件 | |
| `engine/db/`（主核） | 本设计的主体 |

### 5.3 必须同步修的构建点

| # | 位置 | 动作 |
|---|---|---|
| B-1 | `engine/src/CMakeLists.txt:21` | 删 `add_subdirectory(db)` |
| B-2 | `engine/test/CMakeLists.txt:17-19` | 删 `if(ENABLE_FROZEN_SRC_DB_TESTS)` 整块（含 `add_subdirectory(db)` 与 `add_subdirectory(db/consensus)`）—— **（复审修正行号：不是 L27-28，L27-28 是 `add_subdirectory(kbase)` / `add_subdirectory(sdk)`）** |
| B-3 | `engine/CMakeLists.txt:84` | 删 `ENABLE_FROZEN_SRC_DB_TESTS` 选项 |
| B-4 | `engine/CMakeLists.txt:59-60, 155` | 删 `ModalityProfiles.cmake` / `MultimodalConfig.cmake` 的 include 与 `multimodal_config_gen` 链接 |
| B-5 | `engine/src/sdk/CMakeLists.txt:112` | `target_link_libraries(mmsdk PUBLIC sqlite3 faiss_hnsw mm_vector_index sql_engine)` → `target_link_libraries(mmsdk PUBLIC sqlite3)`。**注意：这一步必须与 §5.6 的 memctx 移植同批做** —— 否则 `mmsdk` 立刻出现 undefined symbol |
| B-6 | `engine/src/sdk/CMakeLists.txt:103` | 删掉为 `faiss_hnsw.h` 等 db 模块头准备的 `../` include 路径 |
| B-7 | `engine/src/sdk/vectors/vectors.c` | HNSW 后端改为调用 `index_flat.c` 的扁平索引；`vector_index_selector` 调用改为本地决策 |
| B-8 | `engine/src/sdk/vectors/index_flat.c` | 把 dummy 换成真实现（插入 / 精确 KNN / 过滤后候选） |
| **B-9** | `engine/tools/benchmark/CMakeLists.txt:5` | **（复审补入）** 删 `add_subdirectory(c_api)` —— 否则 D-8 删目录后 **CMake 配置期 fatal error** |
| **B-10** | `engine/test/CMakeLists.txt:23` | **（复审补入）** 删 `add_subdirectory(smoke)` —— 否则 D-3 删目录后**同样 fatal** |
| **B-11** | `engine/test/CMakeLists.txt:32-68` 的 `_test_targets` 列表 | **（复审补入）** 27 个目标名里约 20 个指向冻结轨。`if(TARGET ${_t})` 守卫会**静默跳过**（不报错也不提示）—— 必须**显式清理列表**，否则残留目标名会长期伪装成「已清理」 |
| **B-12** | `engine/test/index/{block,inverted}/CMakeLists.txt` 及 4 个子目录 | **（复审补入）** **整个 `engine/test/index/` 目录删除**（D-10 的 4 个索引类型目录 + 8 个 `.cpp` + 各级 `CMakeLists.txt`）。⚠️ **（复审修正）** 这棵树**没注册进 CMake**（`engine/test/CMakeLists.txt` 无 `add_subdirectory(index)`）⇒ 删它**不影响任何 ctest 用例数** ⇒ **R-06 的差值判据里不应计入它** |
| **B-13** | `engine/src/db/index/vector_index/CMakeLists.txt` 等 | **（复审补入）** 随 D-1 整树删除，无需单独处理；但要确认 `engine/src/db/CMakeLists.txt:180` 的 `if(MMDB_ENABLE_RELATIONAL AND TARGET sql_engine AND TARGET db_executor)` 分支不残留在别处 |
| **B-14** | `engine/src/sdk/core/memctx.{c,h}` + `engine/include/sdk/impl/mmdb_memctx.h` | **（复审补入）** memctx 移植 —— 见 §5.6 |

**B-7/B-8 的降级语义必须写进 SDK 文档**：SDK 的向量检索从「HNSW 近似」降级为「暴力精确 KNN」——
复杂度从 O(log N) 变 O(N)。这是**能力降级**，必须在 `engine/include/sdk/mmdb_vectors.h` 的注释与
`engine/docs/` 里写明，不能默默改。

**B-9 ~ B-12 的共同特征**：这些是**「删了目录但构建文件还引用它」**类故障 ——
CMake 在**配置期**就报错（好），而 `_test_targets` 里的残留目标名是**静默跳过**（坏，最难查）。
验收必须包含「`cmake` 配置成功」+「`ctest -N` 列出的用例数与基线一致」两条。

### 5.4 文档处置（912 处引用）

| 类别 | 处数 | 处置 |
|---|---|---|
| `docs/analysis/`（差距分析报告） | 869 | **整批归档**：在每篇顶部加「本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考」的横幅；文件移入 `docs/analysis/archive/` |
| `docs/arch/` | 32 | 逐条改：指向冻结内核的章节删除或改指主核 |
| `docs/storage/` | 8 | 同上 |
| `openspec/specs/` | 3 | 同上 |
| `README.md`（仓级） | — | 「项目定位」表删掉冻结内核行；「核心能力」§1–§4 整节重写为主核口径；「项目结构」树更新；「测试覆盖」表更新；「踩坑红线」删掉冻结内核相关条目 |
| `CLAUDE.md` | — | 「数据库存储引擎」「多模态存储引擎」「GUC」「initdb / pg_ctl / db_server」「API 使用示例」五节描述的全是冻结内核 → **整节重写为主核口径**；「库间依赖关系」表删掉 `db/storage → db_catalog` 那组 |
| `AGENTS.md` | — | 「库」清单里的 `index`（向量索引 hnsw/ivf/diskann/BM25 等）指向冻结轨 → 改写 |
| `engine/README.md` | — | 同仓级 README |

### 5.5 退役的验收判据

```bash
# 1) 冻结轨彻底消失
test ! -e engine/src/db && test ! -e engine/test/db && test ! -e engine/include/db
# 只扫构建与源码面，且**排除 engine/db/**（主核自己的 README 里合法提到 src/db/ 作为历史说明）
grep -rn "src/db/" engine/src engine/test engine/include engine/tools engine/cmake \
     --include=CMakeLists.txt --include=*.c --include=*.h --include=*.cpp   # 零命中

# 2) CMake 轨仍可构建、测试通过
cmake -B build/engine -S engine -DBUILD_TESTING=ON     # 配置期不得有 fatal error
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure
ctest --test-dir build/engine -N | tail -1               # 用例数须与基线一致（见 §7）

# 3) SDK 不再有 undefined symbol
nm -u build/engine/src/sdk/libmmsdk.a | grep -Ei "faiss|hnsw|sql_engine|memctx_"   # 零命中

# 4) 引用校验门禁
python scripts/check-wiki-refs.py

# 5) 主核不受影响
cd engine/db && sh tools/check_deps.sh && sh test/run.sh
```

> **（复审修正）第 1 条的 grep 范围**：原文写 `grep -rn "src/db/" engine/` —— 这会**误报**：
> `engine/db/README.md:9` 与 L626 各有一处，**合法地**提到 `engine/src/db/` 作为冻结轨的历史说明。
> 故必须限定扫描目录（排除 `engine/db/`）或限定文件类型。

### 5.6 SDK 内存上下文去冻结化（D-6 / B-14 的处置）

**问题**（复审实测，是原文的重大遗漏）：

```
engine/include/sdk/impl/mmdb_memctx.h:25   #include "db/sql/memctx.h"     ← 真实存在
engine/include/sdk/impl/mmdb_memctx.h:32   typedef MemoryContext mmdb_memctx_t;
```

| 层 | 依赖 |
|---|---|
| SDK 源文件（6 个） | `core/collection.c`、`core/mmdb.c`、`core/mmdb_memctx.c`、`extra/xquery.c`、`graph/graph.c`、`text/text.c` |
| 类型 | `MemoryContext` 来自 `include/db/sql/memctx.h`（冻结内核公开头） |
| 实现 | `palloc` / `pfree` / `AllocSetContextCreate` / `MemoryContextReset` / `MemoryContextDelete` → `src/db/sql/memctx.c`（834 行） |
| 独有符号 | `mmdb_mem_register_resource` / `mmdb_mem_unregister_resource` **只在** `src/db/sql/memctx.c:783,816` 定义；`src/sdk/core/mmdb.c:199` 在调用 |
| 测试 | `engine/test/sdk/memory/` 下 **5 个**（见 §2.9） |

**结论**：不处理它，D-4 删掉 `include/db/` 后 `mmsdk` **立刻链接失败**（5+ 个 undefined symbol），
B-5 摘链接这步走不下去。

**方案选择**：

| 方案 | 工作量 | 评价 |
|---|---|---|
| **A. 把 memctx 移植进 `sdk/core/`**（**采纳**） | 834 行 `memctx.c` + 503 行 `memctx.h` 迁入 SDK | 见下方可行性论证 |
| B. 主核提供等价分配器 | 主核是 Makefile 工程、无 `.a` 产物 | 跨构建体系，CMake 侧链不上 → **不可行** |
| C. 删掉 SDK 的 memctx 层 | 删 6 个文件的 memctx 调用 + 6 个测试 | 破坏 SDK 公开 API —— 这是「砍功能」而非「去冻结化」，且 §2.9 的 5 个测试就是验收依据 → **不采纳** |

**方案 A 的可行性论证（关键）**：`memctx.h` 对冻结内核的依赖**只有一个** ——
`#include "db/parser/sql/parsenodes.h"`，而它需要那个文件的**唯一**原因是
`MemoryContextData.type` 字段用了 `NodeTag` 枚举里的 **2 个值**（`T_MemoryContext` / `T_AllocSetContext`）。
实测：`parsenodes.h` 的 `NodeTag` 共 **114 项**，而 `memctx.h` 只用到 2 项。

→ **移植代价极小**：在 SDK 内定义一个只含这 2 个值的本地枚举（或改用 `u16` + 2 个常量），
`memctx.h` 的**其余 501 行与 `memctx.c` 的 834 行可近乎原样搬运**，无行为变更。

**移植清单**：

| # | 动作 |
|---|---|
| M-1 | `include/db/sql/memctx.h` → **`engine/include/sdk/impl/memctx.h`**（去 `parsenodes.h` 依赖，`NodeTag` 换本地 2 值枚举）—— **（复审订正）** 原文写 `engine/src/sdk/core/memctx.h`（把头放进 .c 目录，与 SDK 其它实现级头的落法不一致）；W-A 用的 `sdk/impl/` 才是对的 |
| M-2 | `src/db/sql/memctx.c` → `engine/src/sdk/core/memctx.c`（`<windows.h>` / `<pthread.h>` 双线程 ID 保留原样） |
| M-3 | `include/sdk/impl/mmdb_memctx.h` 的 `#include "db/sql/memctx.h"` → `#include "core/memctx.h"`；同步订正 L116 那段过期注释（原文说「已通过 `#include "db/sql/memctx.h"` 自动暴露声明」—— 引用路径变了） |
| M-4 | `mmdb_mem_register_resource` 的声明与定义一并迁入 SDK |
| M-5 | `src/sdk/CMakeLists.txt` 把 `memctx.c` 加进 `mmsdk` 的源文件列表 |
| M-6 | `engine/test/sdk/memory/` 的 6 个测试**保留**，作为 M-1~M-5 的验收依据 |

**验收**：`nm -u libmmsdk.a | grep memctx` 零命中（§5.5 第 3 条）+ 6 个 memctx 测试全绿。

> **这一步的独立价值**：移植后 SDK **彻底不依赖**冻结内核 —— 冻结轨才真的可以整棵拔掉，
> 而不只是「让 CMake 不再链接它」。

---

## 6. 设计 C：八模态对称化与功能补全

### 6.1 「对称」的定义（先定义，再补全）

**模态的契约面 = 四件套**，位置统一：

| 件 | 统一位置 | 关系 | blob | 图 | 向量 | 全文 | 空间 | 时序 | 文档 |
|---|---|---|---|---|---|---|---|---|---|
| ① SPI 适配器 | `src/modalities/<m>/` | `relational/heap_am.c` **← 搬**（S3） | `blob/blob_am.c` **← 搬**（S5） | ✓ | ✓ | ✓ | ✓ | ✓ | **无**（寄生模态，设计如此） |
| ② 公开 API | `src/db_<m>.c` | SQL 路径（无独立文件，见 S4） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | **`db_json.c` 新建** |
| ③ REPL 族 | `src/repl_<m>.c` | SQL | BLOB ⚠ | GRAPH ⚠ | VEC ✓ | FT ✓ | GEO ✓ | TS ✓ | **`repl_json.c` 新建** |
| ④ 模态文档 | `MODALITIES.md` 对应节 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓（补契约面小节） |

> **（计划编写期发现）blob 与 graph 的 REPL 是两个例外**：它们的 REPL 实现**不在** `repl_<m>.c` 里，
> 而是**直接写在 `src/main.c` 内部**（`handle_blob` 与 `handle_graph` 两个 `static` 函数）。
> 其余五个模态的 REPL 才是独立的 `repl_<m>.c`（`repl_vec.c` / `repl_ft.c` / `repl_geo.c` / `repl_ts.c`）。
>
> ⇒ **对称化的动作清单要加两条**：把 `handle_blob` / `handle_graph` 从 `main.c` 里**搬出去**
> 成 `src/repl_blob.c` / `src/repl_graph.c`。
> ⚠️ 这两条与 §4.2 的第 5 处裁剪点直接相关 —— 搬完之后 `main.c` 里就只剩**派发**，
> 裁剪点只需包住派发分支（`static` 辅助函数随文件一起搬走，不会有 `-Wunused-function` 问题）。

**实现目录不在对称范围内** —— 它由**分层**决定，不由「模态身份」决定：

- **关系模态 = 底座型模态**：实现横跨 L1（`storage/heap*`）/ L2（`catalog`、`txn/vacuum`、`index/btree`）/ L3（`query`）。
  §2.11② 已证：`catalog.h:8` 与 `vacuum.h:6` 都 include `storage/heap.h` → **L2 直接持有 `Heap*`**。
  搬到 L4 = `L2 → L4` 反向依赖 = 门禁必红。**这是角色决定的，不是历史遗留。**
- **其余 7 个 = 插件型模态**：实现整体在 L4 `modalities/<m>/`。
- **blob 例外中的例外**：§2.11③ 已证零反向依赖 → **可完整搬进 `modalities/blob/`**。

> **文档模态没有 ① 号件，是设计而非缺陷**：它是**寄生模态** —— 不建存储、做 heap 上的列类型、没有虚拟表，
> 因此没有 `StorageAM`。它的对应物是**值层谓词求值器** `modalities/doc/json_match.c`，走 `spi/pushdown.h` 的下推契约。

### 6.2 SPI 的第四个契约：`TableOpener`（关系模态进入 L4 的钥匙）

**问题**：其余 6 个模态的适配器能在 L4，是因为执行器**从不直接调用**它们。
关系模态是例外 —— 它的表不走注册表（catalog 真表），执行器必须主动构造它的 AM（§2.11①）。

把适配器留在 `spi/`（L3）能骗过门禁（L3→L3 合法），但**语义上执行器仍然直接认识 heap**。

**判决 D10：新增 `src/spi/table_opener.h`**

```c
/* 不透明 ctx 缓冲区的上限。 */
/*
 * 打开器私有状态缓冲的字节上限。
 *
 * ⚠️ **（复审修正）原值 256 不够，且名字有两义** ——
 * 关系模态的私有状态实测是 `Heap*`(8) + `HeapAmCtx`(184) + `StorageAM`(152) = **344 字节** > 256。
 * 原值只按 `HeapAmCtx` 单件定的，漏算了 `Heap*` 与 `StorageAM`。
 * ⇒ 值提到 **512**；名字从 `SPI_AM_CTX_MAX` 改成 `SPI_OPENER_PRIV_MAX`
 *   —— 原名里的 "CTX" 在别处被当成"执行器的上下文缓冲"，一个名字两种含义。
 */
#define SPI_OPENER_PRIV_MAX 512

/* 打开器的产物。调用方持有，必须活到本次查询结束。 */
typedef struct {
    StorageAM      *am;        /* 这张表的 StorageAM */
    const ColDesc  *cols;      /* 列定义（指向调用方的 cols_buf） */
    int             ncols;
    const IndexInfo *idxs;      /* 索引列表（指向调用方的 idxs_buf） */
    int             nidx;
    u32             tid;       /* catalog 表 id；虚拟表为 0 */
    bool            is_virtual;
} TableOpenResult;

typedef struct TableOpener TableOpener;

struct TableOpener {
    const char *kind;          /* "catalog" / "virtual" —— 仅诊断用 */

    /*
     * 按表名打开一张表。
     *
     * owner      : 打开器的宿主（catalog / SpiRegistry / ...）
     * out        : 产物（调用方持有）
     * cols_buf   : 调用方给的列定义缓冲（至少 SQL_MAX_COLS 项 ColDesc）
     * idxs_buf   : 调用方给的索引列表缓冲（至少 SQL_MAX_COLS 项 IndexInfo）
     * priv       : 调用方给的私有状态缓冲
     * priv_bytes : priv 的字节数
     *
     * out->cols / out->idxs 指向 cols_buf / idxs_buf（不是打开器自己的内存）——
     * 这样调用方「自持副本」的纪律（v1.2 步骤 1）继续成立。
     *
     * 返回 OK；表不存在返回 E_NOTFOUND。
     */
    int (*open)(const TableOpener *op, void *owner, const char *name,
                TableOpenResult *out,
                ColDesc *cols_buf, IndexInfo *idxs_buf,
                void *priv, size_t priv_bytes);

    /* 按表 id 打开（只有 catalog opener 支持；虚拟表返回 E_NOTSUP）。 */
    int (*open_by_id)(const TableOpener *op, void *owner, u32 tid,
                      TableOpenResult *out,
                      ColDesc *cols_buf, IndexInfo *idxs_buf,
                      void *priv, size_t priv_bytes);

    /* 私有状态需要多少字节。 */
    size_t priv_bytes;
};
```

> **（计划编写期修正）契约比初稿多了三样东西**，都是为了补上初稿**装不下**的东西：
>
> | 初稿 | 缺什么 | 为什么初稿装不下 |
> |---|---|---|
> | 只返回 `StorageAM` | **列定义 + 索引列表** | `catalog_table_cols` 的签名是「**写进调用方给的数组**」，它不返回指针 ⇒ 列定义本来就必须落在调用方的缓冲里 |
> | 只按 `tid` 打开 | **按表名打开** | 执行器的主路径是 `resolve_table(name)`；`tid` 路径只有 UPDATE / DELETE 用 |
> | 固定 `ctx_out` | **`priv` + `priv_bytes`** | 关系模态的 per-query 私有状态是 `HeapAmCtx`(≈184 B) **加** 一个 `StorageAM` —— 一个 `ctx_out` 装不下两样 |
>
> **不透明 ctx 缓冲区**：`SPI_OPENER_PRIV_MAX = 512`（实测 `HeapAmCtx` ≈ 184 字节，附录 C）。
`TableCtx.am_ctx` 从 `HeapAmCtx` 改成 `u8 am_ctx_buf[SPI_OPENER_PRIV_MAX]` →
  **执行器不再 include 任何模态头**。
  - **（复审修正）接口里不传 `ctx_cap`**：上限是编译期常量，运行时再传一个 cap 没有意义，
    反而给误用留了空间（适配器可以收到一个更小的 cap 然后写超界）。
正确做法是**编译期校验**：`_Static_assert(sizeof(HeapAmCtx) <= SPI_OPENER_PRIV_MAX, "...")`
    —— 超限直接编译错，而不是运行期才发现。接口更小，且不可误用。
- **两个实现**：
  - `rel_table_opener`（关系模态提供，在 `src/modalities/relational/rel_opener.c`）——
    `catalog → heap → heap_am_build`；
  - `spi_table_opener`（SPI 注册表提供，在 `src/spi/table_opener.c`）——
    `名字 → 已注册的 StorageAM`。
  执行器只按顺序问这两个，**谁先答上就用谁**（顺序 = catalog 优先，与旧实现一致）。
- **组合根（`db.c`）持有**：实例级保存这两个 `TableOpener` 指针，装进 `Db` 结构体。
- **门禁加强**：`check_deps.sh` 增加一条 —— `src/query/` 下**禁止 include 任何模态头**
  （`heap|blob|graph|vec|ft|geo|ts|json` 的 `#include` 行）。可机械验证，
  与 PLAN-v1.6 的 A7 审计同款思路，但提前到 C 阶段做。

> **（复审修正）门禁的写法**：原文写「`src/query/` 下禁止出现任何模态名」——
> 这会**第一天就红**：`src/query/` 当前有 **26 处** `heap|blob|graph|vector|json` 命中，
> 其中绝大多数是**注释**（如 `exec.c:18`「执行器里没有一行 blob 特判」这句自己就在告发自己，
> `exec_internal.h:42` 整段在解释「为什么关系模态要单独放 heap / am_ctx / am_storage」）。
> 门禁必须**只查 `#include` 行**，否则噪声淹没信号。

**收益**：

1. 执行器真正模态无关 —— 验收命令：`grep -rnE '#include\s+"(heap|blob|graph|vector|vec|ft|geo|ts|json)[^"]*"' engine/db/src/query/` **零命中**
2. 关系模态与其它 7 个同层（L4）→ 八模态结构对称
3. **这是 v1.7「跨模态联合查询」的前置** —— 联合查询要同时打开不同模态的表，必须先有统一的「表打开」抽象

**代价**：执行器 3 处改动（1 个 include + 1 个结构体字段 + 2 个调用点）+ 1 个新契约头（约 40 行）+ 门禁 1 条。
**必须与 C-4（文档模态接 SQL）合并做** —— 两者都要动执行器，分两次动是浪费。

### 6.3 结构对称化的动作清单

| # | 动作 | 从 | 到 | 门禁 / 影响 |
|---|---|---|---|---|
| S1 | SPI 第四契约 | — | 新增 `src/spi/table_opener.h` | L3 新增头，无依赖 |
| S2 | 执行器去模态化 | `query/exec_internal.h`、`query/exec_scan.c` | 改用 `TableOpener` | **必须在 S3 之前** |
| S3 | 关系模态适配器 | `src/spi/heap_am.{c,h}`、`heap_am_batch.c`、`heap_am_internal.h` | `src/modalities/relational/` | 依赖 S2 |
| S4 | 关系模态的「公开 API」归位 | — | **不新建 `db_rel.c`** —— 关系模态的对外接口就是 SQL（`db_exec` + DDL/DML），没有独立的 `db_rel_*` 函数族。**不为了对称而造空壳 API** | — |
| S5 | blob 实现 + 适配器 | `src/storage/blob*`、`src/spi/blob_am.*` | `src/modalities/blob/` | 零反向依赖 |
| S6 | B+tree 适配器归位 | `src/spi/btree_am.{c,h}` | `src/modalities/relational/` | B+tree 是关系模态的索引结构；全仓仅 `btree_am.c` 自己 include 它 |
| S7 | 文档模态公开 API | — | 新增 `src/db_json.c` + `include/db.h` 的 `db_json_*` | 依赖 C-4 |
| S8 | 文档模态 REPL 族 | — | 新增 `src/repl_json.c` + `DOC` 动词族 | 依赖 C-4 |
| S9 | 门禁加强 | `tools/check_deps.sh` | ① `src/query/` 禁 include 模态头（**只查 `#include` 行**）；② 模态适配器必须在 `src/modalities/` 下 | 可机械验证 |
| **S10** | **blob / graph 的 REPL 归位**（**计划编写期补入**） | `src/main.c` 内的 `static handle_blob` / `static handle_graph` | `src/repl_blob.c` / `src/repl_graph.c` | 见 §6.1 的备注：这两个模态的 REPL **原本就不在** `repl_<m>.c` 里 ⇒ 对称性缺这两条。搬完之后 `main.c` 里只剩**派发**，§4.2 的第 5 处裁剪点也变干净 |

**S3 + S6 做完后的 `spi/` 目录**（只剩**纯契约 + 机制**）：

```
src/spi/
├── storage_am.h      存储 AM 契约（v0.9 定稿）
├── index_am.h        索引 AM 契约（v0.9 定稿）
├── pushdown.h        谓词下推契约（v0.9 定稿）
├── table_opener.h    表打开契约（D10 新增）
├── table_opener.c    注册表侧的 opener 实现（纯机制，不含任何模态）
└── registry.{c,h}    虚拟表注册表（实例级机制）
```

> **（计划编写期修正）`spi/` 里不能有任何模态的东西** —— 这条由门禁守住
> （`forbid src/spi heap_am btree_am blob_am`）。
> 关系模态与 blob 的适配器搬进 `src/modalities/` 后，`spi/` 只剩契约与机制。

### 6.4 文档模态的功能补全（`PLAN-v1.6` 步骤 3~6）

| 步骤 | 动作 | 验收 |
|---|---|---|
| 步骤 3 收尾 | **heap 记录编解码已真的走 `T_JSON`**（§2.12 实测已达成，无需重做）；只剩**大文档外置接 `PT_JSON_OVF` 页链**（`ext_fn` 换成沿链读，`DetoastFn` 契约一行不改） | A2：只取路径字段时**不读全文**（页读计数对比） |
| 步骤 4 | 路径索引：`CREATE INDEX ON t (json '$.a.b')`；**写时同步维护**（UPDATE 整替换 → 删旧键插新键，用既有 B+tree 能力） | A4：索引项计数对账，旧键不残留 |
| 步骤 5 | 执行器接线 + `json_match(doc, '$.a.b = 5')`；**走/不走索引两路结果集全等** | A3 + A7 |
| 步骤 6 | MVCC（RR 快照下文档不撕裂）+ 崩溃恢复 + **反向验证 2 处**（标量映射把 null 映成 0；UPDATE 漏删旧键）+ 收官复盘 | A5 / A6 / A8 |

**A7 审计清单（先定义后实现）**：执行器允许的改动落点 —— **（复审修正）原文写「只有两处」是错的**。

> **为什么必须扩**：实测 `src/query/` 的 parser 目前**根本表达不了** `json_match(...)` 与
> `json '$.a.b'`：① `parser.h:48-52` 的表达式节点只有 `EX_LIT/EX_COL/EX_BIN/EX_UN/EX_AGG`
> —— `EX_AGG` 是**聚合专用**，没有通用函数调用节点；② `parser_stmt.c:27-34` 的类型名只认
> `INT/TEXT/REAL/BOOL`，`JSON` 直接落到 `p_fail("不支持的类型")`；
> ③ `parser_stmt.c:92` 的 `CREATE INDEX` 列名是 `p_expect_ident(index_col)`，
> 解析不了 `json '$.a.b'` 这种带路径字面量的写法。
>
> 三处缺口**全在 `src/query/`** —— 恰是 A7 说「只许两处」的那个目录。
> 若不提前把清单写清，执行者在步骤 5 撞墙后必然**私自扩大改动面**，
> 而那正是 A7 想防的事。所以清单必须**先定义完整**。

**A7 允许的落点（五处）**：

| # | 落点 | 内容 | 边界 |
|---|---|---|---|
| 1 | **通用值层** | `value.h` / `record.h` 承载 JSON 值（`T_JSON` 已有，需补 `DbValue` 的 JSON 变体或明确走外部值通道） | 只加类型分支，**不加** `if (type == T_JSON)` 之外的模态判断 |
| 2 | **求值入口** | `exec_scan.c` 的谓词求值处识别 `json_match(...)` 并委托给 `modalities/doc/json_match.c` | 委托，**不实现** JSON 语义 |
| 3 | **lexer** | 认识 `JSON` 类型名与 `json` 路径字面量（`'$.a.b'` 的词法） | 纯词法，**不含**语义 |
| 4 | **parser 表达式** | 新增通用函数调用节点（`EX_FUNC`）+ `json_match` 的参数解析 | 只做**通用**函数调用能力，**不**为 `json_match` 特判 |
| 5 | **parser DDL** | `CREATE INDEX` 支持 `json '$.a.b'` 的路径列 | 只扩展 DDL 语法，**不**加模态分支 |

**落点之外零容忍**：任何 `if (strcmp(col_type, "json"))` 形态的模态特判 = **零容忍**。
落点 3/4/5 的设计原则是「**加通用能力，不加模态分支**」——
`EX_FUNC` 是通用函数调用节点（未来 `geo_contains(...)` 也能用），
`JSON` 类型名是通用类型名，不是「为了 JSON 才加的」。

**门禁配合**（S9）：`src/query/` 禁 include 模态头这条门禁，**恰好能机械验证落点 1/2/3/4/5 合规** ——
因为合规的实现方式就是「不 include `modalities/doc/` 的任何头，只走 `spi/` 契约」。
这比人工审计可靠得多。

> **正面样板（保持）**：页类型 `PT_JSON_OVF`（`json_ovf.h:82` 自定义为 `0x1Bu`）
> **不进** `common/config.h` 的公共页类型枚举 —— 模态自持页类型，不污染 L0。
> 这个做法应写进 `MODALITIES.md` 并保持。

### 6.5 `MM` 开关与 smoke 覆盖 8 个模态

profile 补全（`engine/db/tools/mm_profiles.mk` 为唯一真相源）：

| profile | 打开的模态 | 闭包自动含 | 产物 |
|---|---|---|---|
| `MM=relational` | 关系 | 底座 L0–L3 | `db.exe` + `smoke_relational` |
| `MM=blob` | blob | **底座 L0–L3**（见下方复审修正） | `db.exe` + `smoke_blob` |
| `MM=graph` / `vector` / `ft` / `geo` / `ts` | 各 1 个 | 底座 L0–L3 | 对应 smoke |
| `MM=doc` | 文档 | 底座 + **关系**（寄生模态必须依赖它的宿主） | `db.exe` + `smoke_doc` |
| `MM=full` | 全部 8 个 | — | 全部 8 个 smoke |

> **（复审修正）`MM=blob` 的闭包是 L0–L3，不是 L0–L1**。
> 原文写「底座 L0–L1（+ 实例编排所需的 wal）」—— **做不到**：
> 组合根 [db.c](projects/engine/db/src/db.c) 固定 include 了 `query/exec.h`、`query/parser.h`、
> `catalog/catalog.h`、`txn/{txn,mvcc}.h`、`wal/recovery.h`。
> 主核的 `db.exe` 无论如何都要 L0–L3（关系模态是底座型模态，其执行器路径不可裁）。
>
> 正确表述：**所有 profile 的闭包都含 L0–L3**，`MM` 裁的是**模态**（8 个里的哪几个），
> 不是底座层数。原文把「blob 只含 L0–L1」与「关系只含底座」描述成不同档，是错的。

**依赖闭包由声明算出，不手工指定** —— 每个模态在 `mm_profiles.mk` 里声明它需要谁，
Makefile 的闭包校验器算出实际集合；差集非空直接报错。

> **闭包机制第一次真正起作用的就是 `doc`**：文档模态是寄生模态，JSON 存 heap 行里，
> 所以 `MM=doc` 必须带上关系模态。这条依赖是**声明出来的**，不是手工塞进 profile 的 ——
> 漏声明时闭包校验器会报错并打印差集。

> **不要把 blob 塞进 `relational` 的闭包**：外部值机制是**泛型**的（`storage/value.h` 存 `DetoastFn` 函数指针），
> 关系模态的 heap 路径**零处**引用 blob（实测：`storage/heap.c` 只 include `heap_internal.h` + `record.h`）。
> blob 走自己的 `blob_frames` 虚表路径，与关系模态无依赖关系。

**smoke 的语义**（每模态一个）：初始化 → 建最小对象 → 写一条 → 读回 → 断言 → 关闭。
它的意义是**链接期证明**：若某模态偷偷依赖了别的模态，链接失败。

> `smoke_doc` 与其他七个不同：文档模态是**寄生模态**，没有独立存储与虚拟表，
> 它的 smoke 验的是「JSONB 编解码 + 路径提取 + 标量映射」，**不验 SQL 路径**（除非 C-4 已完成）。
> 这一点必须写在 smoke 的注释里，否则会让人误以为文档模态已可从 SQL 访问。

### 6.6 设计 C 的执行顺序

| 切片 | 内容 | 前置 |
|---|---|---|
| **C-1** | S1 + S2：SPI 第四契约 + 执行器去模态化（**执行器一次性脱钩**） | 无 |
| **C-2** | S3 + S6：关系模态适配器 + B+tree 适配器搬进 `modalities/relational/` | C-1 |
| **C-3** | S5：blob 搬进 `modalities/blob/` | 无（可与 C-1/C-2 并行） |
| **C-4** | PLAN-v1.6 步骤 3~6：文档模态接 SQL（含 S7 / S8 + **A7 的落点 3/4/5**） | C-1（执行器已脱钩，改动面小） |
| **C-5** | S9 门禁加强 + `MM` 开关扩到 8 个 profile + 8 个 smoke | C-1~C-4 |

> **C-4 的工作量修正（复审）**：原文把步骤 4/5 当成「接线」，
> 实际还包含 **A7 落点 3/4/5**（lexer 认 JSON 与路径字面量、parser 加通用函数调用节点 `EX_FUNC`、
> parser DDL 支持路径列）—— 这是**三处 parser 能力扩展**，不是「接线」。
> 它们是**通用能力**（未来 `geo_contains(...)` 也受益），但工作量必须单列，不能藏在「接线」二字里。

---

## 7. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| **ctest 用例数从 ~835 掉到 ~250** | 高 | 这不是回归 —— 是删掉了 294 个测冻结内核的测试文件。**必须采改造前基线**（失败用例名集合，跑两次取交集），只比「集合不新增」而非绝对值（2026-10-27 设计的教训） |
| **SDK 向量检索降级为暴力 KNN** | 中 | 写进 SDK 文档与 changelog；不静默改 |
| **`mmsdk` 摘链接后出现 undefined symbol** | 中 | 链接期立刻暴露；`nm -u` 逐个查 |
| **912 处文档死链** | 中 | §5.4 分类处置；`check-wiki-refs.py` 必跑 |
| **`db_internal.h` 裁剪后实例结构体变小** | 中 | 每模态的 `db_<m>_open/close` 都要在 `#if` 两侧对称；P1 的验收要覆盖「裁掉的模态的公开 API 不存在于符号表」 |
| **P0~P2 与冻结轨退役并行导致分支混乱** | 中 | **次序**：先 W-B 的 P0（CI 兜底）→ 再 W-A（删除，趁 CI 已就位，能立刻验证主核没受影响）→ 再 W-B 的 P1/P2 → 最后文档大改 |
| **删除后无法回滚** | 低 | git 历史保留；删除前打 tag `pre-frozen-track-removal` |
| **`engine/include/db/` 下有 SDK 仍在用的头** | 低 | §2.8 已证明 SDK 只 include `algo-prod/distance/distance.h` 与 `roaring_bitmap.h`；B-6 的 include 路径调整后用编译验证 |
| **`TableOpener` 改造动执行器，可能引入回归** | 高 | 改造前先跑 `sh test/run.sh` 存基线；改造后**用例集合必须完全一致**（不新增、不减少）；S2 的验收 = 「执行器零模态名」门禁通过 **且** 全部既有用例仍绿 |
| **`SPI_OPENER_PRIV_MAX = 512` 未来不够用** | 低 | 契约头里写死上限并在编译期校验（超限即编译错）；真需要时改这一个常量，代价可控 |
| **文档模态接 SQL 时执行器改动越界** | 中 | A7 审计清单**先定义后实现**：允许落点**五处**（值层路径变体 / 求值入口 / lexer / parser 表达式 / parser DDL），见 §6.4；`grep -i json src/query/` 的每一处命中都要在清单里登记 |
| **`json_match` 的 NULL 语义（路径不存在 vs JSON null vs 标量不匹配）** | 中 | 映射规则全表用例（A1/A2）；反向验证注入 1 = 把 null 映成 INT 0，让 NULL 语义测试变红 |
| **八模态对称化与 v1.6 的执行器改动撞车** | 中 | **必须合并做**（C-4 依赖 C-1）；分两次动执行器 = 两次回归成本 + 两次审计 |
| **（复审）SDK memctx 移植遗漏 → `mmsdk` 链接失败** | **高** | §5.6 方案 A：`memctx.{c,h}` 移植进 `sdk/core/`（`NodeTag` 只用 2/114 项 → 成本极低）；**必须与 B-5 同批做**，否则 W-A 第一步就卡住 |
| **（复审）parser 缺通用函数调用节点 → `json_match` 无法表达** | **高** | A7 落点 3/4/5 提前定义（§6.4）；新增 `EX_FUNC` 是**通用**能力而非 JSON 特判 |
| **（复审）`db_instance.c` 的关闭路径漏裁 → 重复释放 / 泄漏** | 中 | 裁剪点第 4 处（§4.2）；验收必须同时覆盖正常关闭与错误回滚**两条**路径 |
| **（复审）`_test_targets` 残留目标名静默跳过** | 低 | B-11 显式清理；验收加 `ctest -N` 用例数与基线比对 |
| **（复审）`test/index/{brin,bitmap,gin,diskann}` 链接目标消失** | 低 | B-12 / D-10 一并删除（`diskann` 那个**现在就是坏的**，目标名应为 `diskann`） |

---

## 8. 不做项

1. **不做**多进程 / 多服务 / 微服务化（D8 留预备）。
2. **不做**插件化（`.so`）—— D2 已否决，理由见 §3。
3. **不做**跨模态事务（v1.7+ 议题，本次不动）。
4. **不做**把冻结轨的 percolator / raft / RAG 算子框架**迁移**到主核 —— 用户裁决是全删；triage 文档保留其分级结论供将来需要时取用。
5. **不重构**主核 **L1–L3 的公共机制**（pager / WAL / btree / txn / catalog / query 的内部形状与已有 SPI 三契约一律不动）—— D3 已收窄为这句话。§6 只动**模态归属**与新增第四契约。
   - **（复审补入的唯一豁免）**：`catalog` 的索引元数据**增加一种「表达式索引」形态**（`IndexInfo` 多三个字段 + 落盘格式多一段）——
     这是路径索引（PLAN-v1.6 步骤 4，「B+tree 既有能力、零新机制」）的**必要条件**。
     **豁免范围严格限定在这三处**：不改 `IndexInfo` 之外的任何 catalog 结构、不改 catalog 的其它落盘格式、不动 catalog 的锁与恢复逻辑。
     没有这条豁免，D11（文档模态功能补全）与「不重构 catalog」不可兼得。
6. **不改**主核的公开 API 语义 —— `db_json_*` 是**新增**，不是改动既有函数。
7. **不为对称而造空壳** —— 关系模态不新建 `db_rel.c`（它的对外接口就是 SQL），文档模态不新建 `doc_am.c`（寄生模态无虚拟表）。
8. **不做** JSON Schema 校验、多值索引、部分更新、JSON 聚合函数、MongoDB 查询语法 —— 沿用 `PLAN-v1.6` §1.1 的「不做表」，不重复决策。
9. **不做**跨构建体系的分配器复用（§5.6 方案 B）—— 主核是 Makefile 工程、无 `.a` 产物，CMake 侧链不上；方案 B 天然不可行。
10. **不修** `engine/test/kbase/` 的注释掉状态 —— 零注册目标，无害；不属本设计范围（避免顺手改相邻代码）。

---

## 9. 交付物与实施计划拆分

本设计包含三条工作流，实施计划拆成三份。**交叉点**：
W-A（删除 524 个文件）动的是整个仓库的构建面，必须先有 W-B 的 P0（主核 CI）兜底，
否则删除之后没有任何自动化手段能证明「主核没被碰坏」。

| 计划 | 计划文件 | Task 数 | 勾选项数 |
|---|---|---|---|
| W-A 冻结轨退役 | `docs/superpowers/plans/2026-11-18-frozen-track-retirement.md` | 6 | 58 |
| W-B 主核工程组织 | `docs/superpowers/plans/2026-11-18-main-kernel-build-organization.md` | 7 | 47 |
| W-C 八模态对称化 | `docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md` | 8 | 50 |
| W-D 主核欠账清偿 | `docs/superpowers/plans/2026-11-19-main-kernel-debts.md` | 8 | 41 （**全部完成**，见 §10.7） |

> ⚠️ **（复审修正）本表的 Task 数与勾选数按实测重填（2026-11-18 末轮）** ——
> 原值 `6/51`、`7/44`、`7/47` 是三轮复审**之前**的数。复审往三份计划里加了 C0（开工前处理脏文件）
> 与 B1.4（`mm_profiles_test.sh`）等步骤，所以 W-C 变成 **8 个 Task**、三份计划的勾选项也各增了 3~7 个。
> 这些数会随着后续编辑继续变 —— **它们是参考值，不是验收判据**。

### 计划 W-A：冻结轨退役（§5，风险最高、依赖最多）

> 计划文件：`docs/superpowers/plans/2026-11-18-frozen-track-retirement.md`

0. **前置**：W-B 的 P0（主核 CI）已合入 —— 有了红灯才敢删
1. 打 tag `pre-frozen-track-removal`
2. 采改造前 ctest 基线（失败用例名集合，跑两次取交集）+ **记录 `ctest -N` 的用例总数**
   （供 B-11 清理 `_test_targets` 后比对 —— 复审补入）
3. **§5.6 的 memctx 移植（M-1 ~ M-6）** ← **必须先做**，否则 B-5 摘链接时 `mmsdk` 立刻链接失败
4. D-1 ~ D-10 删除（**D-1 与 D-4 同批**，见 §2.7）
5. B-1 ~ B-14 构建与 SDK 修复
6. §5.4 文档处置
7. §5.5 验收

> **步骤 3 必须在步骤 5 之前**：memctx 移植与 B-5（摘 `sql_engine` 链接）是同一件事的两面。
> 顺序反了会得到一个「链接失败」的中间态，且失败原因被淹没在大量删除改动里。

### 计划 W-B：主核工程组织（§4）

> 计划文件：`docs/superpowers/plans/2026-11-18-main-kernel-build-organization.md`

1. **P0**：主核 CI（Linux + Windows 两个 job）← **W-A 的前置**
2. P1：`MM` 编译期开关 + 闭包校验（**五处裁剪点**，§4.2）
3. P2：每模态 smoke + `measure_build.sh`
4. 写 `docs/arch/db/multimodal-build-baseline.md`（实测基线）

### 计划 W-C：八模态对称化与功能补全（§6）

> 计划文件：`docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md`

1. **C-1**：S1 + S2 —— SPI 第四契约 + 执行器一次性脱钩（**风险最高的一步，先做**）
2. **C-2**：S3 + S6 —— 关系模态适配器 + B+tree 适配器归位
3. **C-3**：S5 —— blob 搬进 `modalities/blob/`
4. **C-4**：PLAN-v1.6 步骤 3~6 —— 文档模态接 SQL（含 S7 / S8 + **A7 落点 3/4/5 的 parser 能力扩展**）
5. **C-5**：S9 门禁加强 + `MM` 扩到 8 个 profile + 8 个 smoke
6. **C-6**：S10 —— blob / graph 的 REPL 从 `main.c` 搬进 `repl_blob.c` / `repl_graph.c`（**计划编写期补入**）

> **（复审补入）编号对照** —— 本节用 `C-1 … C-6`（按「动作组」编号），
> W-C 计划文件里用 `Task C1 … Task C7`（按「可独立提交的批次」编号）。两者不是一一对应：

| 本节 | W-C 计划的 Task | 内容 |
|---|---|---|
| C-1 | **C1** | S1 + S2 —— SPI 第四契约 + 执行器脱钩 |
| C-2 | **C2** | S3 + S6 —— 关系模态 + B+tree 适配器归位 |
| C-3 | **C3** | S5 —— blob 归位 |
| C-6 | **C4** | S10 —— blob / graph 的 REPL 归位 |
| C-4 | **C5** | PLAN-v1.6 步骤 3~6 —— 文档模态接 SQL（含 S7 / S8） |
| C-5 | **C6 + C7** | S9 门禁 + 8 profile + 8 smoke；收官复盘 |

**执行顺序**：

```
W-B 的 P0（CI 兜底）
  → W-A 全部（冻结轨退役，含 §5.6 的 memctx 移植）
  → W-C 的 C-1（执行器脱钩，此时 CI 已是绿的，能立刻发现回归）
  → W-C 的 C-2 / C-3 / C-4 / C-5
  → W-B 的 P1 / P2（MM 开关与 smoke —— 此时 8 个模态已就位且**文件归属已定**，`tools/mm_profiles.mk` 一次建对，不需要二次同步）
  → W-C 的 C-6（8 profile + 8 smoke 重跑 + 收官复盘）
  → 文档大改（§5.4 与 §6 的 MODALITIES.md 补写，放在 W-A 末尾，与删除同批提交）
```

> **（复审修正）这段顺序原来是四份互斥的说法** —— 设计 §9、W-B 表头、
> W-C 表头、W-B B1.1 注释各说一套。**本块是唯一权威。**

> **规则一**：W-C 的 C-1 ~ C-5 **全部**必须在 W-B 的 P1 **之前**完成。
> 理由：P1 的 B1 会**第一次创建** `tools/mm_profiles.mk` ——
> 那时所有模态的文件已在最终位置（C-2/C-3 搬完了、C-4/C-5 建好了
> `src/db_json.c` / `src/repl_json.c`），**一次写对**，
> 不存在「先写旧的、之后再同步」的第二步。
> 若顺序反了，B1 写下的 `MM_FILES_blob := src/storage/blob.c …`
> 在 C-3 搬家后全部失效 ⇒ `MM=vector` 裁不掉 blob ⇒ P1 的验收必失败。

> **规则二**：W-C 的 C-1 必须在 W-B 的 P1 之前（它与 P1 改同一批文件）：
> P1 的 `MM` 开关要靠 `#if DB_HAS_<MOD>` 裁剪 `db_internal.h`、`db.c` **与 `db_instance.c`**；
> 而 C-2 正在改这些文件的结构。先脱钩、后裁剪，避免同一批文件被两条工作流同时改。

> **规则三**：W-C 的 C-6 在 P1/P2 **之后** ——
> 它做的是「门禁 + 8 profile + 8 smoke **重跑**」，需要 P1 建好的 `MM` 开关已就位。
> C-6 对 `mm_profiles.mk` 的动作是**核对/校准**，不是"补同步"。

> **规则四**：W-A 的**文档大改**（§5.4）放在 W-A 末尾，与删除同批提交。

> **规则五**：`src/db_internal.h` 被 **W-C 的 C1** 与 **W-B 的 B3** 连续改**同一块** ——
> C1 改它的 include 块（去掉模态头），B3 再在**同一块**加 `#if DB_HAS_*` 守卫。
> ⇒ **B3 必须在 C1 之后**，且 B3 的做法是「**在 C1 的结果上原位包 `#if`**」，
> **不是**把 C1 之前的版本整段抄回去 —— 整段覆盖会把 C1 的脱钩改没。
> ⇒ 两个计划里凡是涉及 `db_internal.h` 的步骤，都必须写明
> 「**基于上一条工作流的结果改，不回退**」。

> **规则六**：**开工前先处理与本轮无关的脏工作区**。
> 当前工作区有 4 个与本轮无关的改动（`engine/db/src/db_ft.c`、
> `engine/db/src/modalities/ft/ft_index.c`、`engine/db/src/modalities/ft/ft_index.h`、
> `engine/db/test/integration/test_ft_reclaim_boot.c`）——
> 三份计划里都有 `git add engine/db/` 这类宽范围命令，会把它们卷进提交。
> ⇒ **开工第一步**：先把这 4 个文件单独提交，或 `git stash push` 收起来；
> 之后所有 `git add` 一律**逐文件列路径**。
> **规则七（2026-11-19 补入）**：本设计新增了 **W-D「主核欠账清偿」** 工作流（§10）。
> 它排在 **W-C 的 C-6 / C-7 之后、文档大改之前**：
>
> ```
> W-B P0 → W-A 全部 → W-C C1 → W-C C2~C5 → W-B P1/P2 → W-C C6/C7
>   → 【W-D 全部】（主核欠账清偿）
>   → 文档大改
> ```
>
> **为什么排在最后**：W-D 里的每一项都要改主核的**实现代码**（不是接线、不是搬家），
> 而 W-C 的 C1/C2 会重排 `db_internal.h` / `db.c` / `db_instance.c` 与模态目录结构。
> 先做 W-D ⇒ 同一批文件被两条工作流连续改 ⇒ 冲突与回归。
> 先做 W-C ⇒ W-D 面对的是**已经就位**的结构，改一处就测一处，不必再为搬家让路。
>
> ⚠️ **但 W-D 里的 D-0（ft 开机页回收的提交）不受这条约束** ——
> 那 4 个文件（`db_ft.c` / `ft_index.c` / `ft_index.h` / `test_ft_reclaim_boot.c`）
> **是当前工作区里已经写好的实现**，它必须**先提交**，
> 否则 W-B/W-A/W-C 的任何一次 `git add` 都可能把它卷进去（规则六已经写了这条）。
> ⇒ **D-0 是所有工作流的第 0 步**，见 §10.2。
>
> **（2026-11-20 补记）W-D 已全部完成**：11 项欠账对应 8 个 Task 的 41 个勾选项
> 全部打勾，门禁 95 → **105 个测试 105 通过**。逐项结果与「计划想错的五处」
> 见 §10.7 —— 那一节比本节更值得读，因为它记的是**实测推翻了什么**。

### 9.1 本地执行环境的前提（复审补入）

三条工作流里有两处依赖 POSIX `sh`，**Windows 本地 `sh` 不在 PATH**：

| 用途 | 命令 | 备注 |
|---|---|---|
| 分层门禁 | `& "C:\Program Files\Git\usr\bin\sh.exe" engine/db/tools/check_deps.sh` | Git 自带 sh |
| 主核测试 | `& "C:\Program Files\Git\usr\bin\sh.exe" engine/db/test/run.sh` | 同上 |
| 主核构建 | `mingw32-make clean; mingw32-make all` | `C:\mingw64\bin` 在 PATH |
| CMake 轨基线 | `cmake -B build/engine -S engine -DBUILD_TESTING=ON` | **`build/engine/CMakeCache.txt` 已存在**（2026-11-03），但必须先 `cmake --build` 到最新，否则采到的是旧基线 |

> **基线采集的坑**：`build/engine` 里的 CMakeCache 是 **2026-11-03** 的，两周前。
> 直接 `ctest` 采到的基线是陈的。**必须先 `cmake --build build/engine --parallel 4` 再采。**

---

## 10. 主核欠账台账与裁决（2026-11-19 补入）

### 10.1 这份台账解决什么问题

有一份「未完成欠账清单」被递进来，共 18 项技术欠账 + 4 项 study-hub 路线图 + 3 项已作废。
第一个要回答的问题是：**这些欠账属于主核（`engine/db/`）还是冻结轨（`engine/src/db/`）？**

> **结论：一项都不是冻结轨的。** 18 项技术欠账引用的源文件**全部只存在于主核**，
> 冻结轨里一个都没有；它们引用的 `PLAN-v0.6` ~ `PLAN-v1.6` 文档也全部在 `engine/db/docs/` 下，
> 讲的是主核自己的事。

实测依据（`engine/db/` 命中 / `engine/src/db/` 命中）：

| 欠账引用的文件 | 主核 | 冻结轨 |
|---|---|---|
| `src/db_ft.c` | ✓ | ✗ |
| `src/modalities/ft/ft_segment.c` / `ft_segment_build.c` / `ft_merge.c` | ✓ | ✗ |
| `src/modalities/ft/ft_tomb.c` / `ft_recover.c` | ✓ | ✗ |
| `src/wal/wal.h` / `wal.c` | ✓ | ✗ |
| `src/modalities/geo/geo_wal.c` / `geo_state.c` / `rtree_knn.c` | ✓ | ✗ |
| `src/modalities/ts/ts_am.c` / `ts_wal.c` | ✓ | ✗ |
| `bench/ft_bench_a4.c` | ✓ | ✗ |

⇒ **「冻结轨要删，所以这些欠账不用管」这个推理不成立** —— 它们从一开始就在主核上。
⇒ 相反：**W-A 删掉冻结轨之后，主核就是唯一的数据库内核**，
这 18 项欠账的**权重不降反升**（没有第二个实现可以对照了）。

### 10.2 裁决结果总表

裁决分四档：**做**（进 W-D 计划）、**已完**（实测已修，不必做）、**降级**（现状可接受，记为已知代价）、
**待裁决**（需产品/架构决策，agent 不得自行决定）。

| # | 欠账 | 裁决 | 依据（实测） |
|---|---|---|---|
| 1 | FT 开机页回收 | **做**（D-0，唯一例外：先做） | 实现已在工作区，4 个文件、+85 行、集成测试 3 项通过 |
| 2 | FT 精确段页数上界 | **做**（D-2） | `ft_merge.c:126-127` 确为保守估算 `post_bytes/(post_cap-35)+1` |
| 3 | C7 物理读盘判据 | **做**（D-3） | `ft_bench_a4.c:769-783` 确有 `[SKIP]` 空判据分支 |
| 4 | WAL 格式前向兼容 | **做**（D-1，优先级最高） | `wal.c:202 wal_open` 直接 `fopen`，无 magic/version 校验 |
| 5 | FT 按值撤销歧义 | **做 → 实测歧义不存在**（D-4 已执行，撞出 D-4b） | `ft_index.c:479` 对重复删除**直接 return**，不写第二条记录 ⇒ 无法撤错；但撞出运行时 ROLLBACK 不撤 FT 的真缺陷（见 §10.7） |
| 6 | geo WAL 只 redo 不 undo | **待裁决** | `geo_wal.c:51` 显式 `xid=0`；改法涉及事务模型，agent 不得自决 |
| 7 | geo 无第二份行存 | **待裁决** | `geo_state.h:58-60` 坑 M5-16；结构级，需先定 checkpoint 方案 |
| 8 | geo WAL 放大 4 倍 | **待裁决**（与 6/7 同批） | `geo_wal.c:56-59` 确认整页镜像 |
| 9 | geo 开库扫描 O(npages) | **降级**（记为已知代价） | `geo_state.h:39-46` 坑 M5-9 已如实记账；且**有现成解法**（见 10.3） |
| 10 | R-tree E_NOSPACE 保守误报 | **已完** | `rtree_knn.c:262-280` 坑 M5-11 **已于 2026-11-04 修**（commit `e48c4b1b8`），改成事后精确判 |
| 11 | geo KNN 二次全树扫描 | **做**（D-5） | `db_geo_query.c:140-152` 注释自认「为什么要扫两遍树」 |
| 12 | reindex 后 row_id 复用 | **降级** | `geo_am.c:377` `geo_am_reindex` 返回 `E_NOTSUP`；`db_geo.c:178` `dg_fix_high_water` 已修水位线 |
| 13 | ts 旧裸写页只标记不回收 | **做 → 已完成**（D-6.2，`55567631e`） | `ts_seal.c` 坑 M6-42 结清；空闲栈精确 +3 页、幂等、复用页全零 |
| 14 | ts 时间窗口封窗未接 | **做 → 已完成**（D-6.1，`d6fb60c2e`） | 真实 SQL 路径原来走全表扫描 + 行过滤（行数对、IO 一页没省）；接进 `ts_prune_scan` 后 3 分区实测 6 页 → 4/0 页 |
| 15 | ts 全局空闲栈只在内存 | **做 → 已完成**（D-6.3，`db9fc4838`） | **做法换了**：原方案（写进 `PT_TS_META`）违反分层铁律，改成 `PT_FREE` 页内标记 + 开机重建（见 §10.7） |
| 16 | ts 整页镜像 WAL 放大 300 倍 | **待裁决**（与 6/7/8 同批） | `ts_wal.c:52` 确认 `memcpy(buf, page, PAGE_SIZE)` |
| 17 | ts A8 IO 统一 benchmark | **做**（D-3 的一部分） | `bench/` 22 个文件，只有 `ft_bench_a4/a5` 有 logical/physical 指标，`ts_bench.c` **一个都没有** |
| 18 | group commit | **做 → 已完成**（D-7，`3381c22fb`） | `wal_group.c` + `wal_internal.h`；模拟 2 ms 盘 4 并发实测 100 事务 → **50 次**（省 50%） |

**做 = 11 项**（#1 #2 #3 #4 #5 #11 #13 #14 #15 #17 #18）
**已完 = 1 项**（#10）
**降级 = 2 项**（#9 #12）
**待裁决 = 4 项**（#6 #7 #8 #16）

> **执行结果（2026-11-20 补记）**：11 项「做」**全部完成**，见 §10.7。
> 其中两项在动手时被实测推翻了原来的判断：
> **#5 的歧义不存在**（D-4 改成 D-4b）、**#15 的原方案违反分层铁律**（D-6.3 换方案）。

### 10.3 逐项说明（只说「为什么」与「为什么不是别的结论」）

#### 已完 / 降级的三项 —— 清单与代码不符，必须先纠正

**#10 R-tree E_NOSPACE 保守误报 —— 已经修了，清单是旧的。**
`rtree_knn.c:262-280` 有一段完整的注释块，说明第一版确实用「树里的总点数」当 `max_out`
会保守误报 `E_NOSPACE`（坑 M5-11），**已在 2026-11-04 改成「事后精确判」**：
先把活点全找出来（`nhit = min(k, 活点数)`），再判 `nhit > max_out` 才报错。
`git log -S "M5-11"` 确认修复已进 `e48c4b1b8`。
⇒ **不做**。若照清单派 agent，会去「修」一个已经修好的地方。

**#12 reindex 后 row_id 复用 —— 前提不成立。**
`geo_am.c:377` 的 `geo_am_reindex` 直接 `return E_NOTSUP`（地理模态没有可重建的索引 ——
`geo_points` 是 R-tree 的投影而不是它的索引）。所以「reindex 后 row_id 被复用」
这件事今天**不可能发生**。而真正相关的那个问题（reindex 让条目数与最大行号分家）
**已经被处理**：`db_geo.c:178 dg_fix_high_water` 在开库时把 `n_total` 修正成 `max_id + 1`，
保证下次发号不发重复。
⇒ **不做**。等 `geo_am_reindex` 真正实现那天，再把 generation 方案一起带上。

**#9 geo 开库扫描 O(npages) —— 记为已知代价，不必现在改。**
`geo_state.h:39-46` 已经把这条如实记账（坑 M5-9），而且**给出了解法方向但当时被判定为
不可行**：「模态不许碰 catalog，而『页号 → 模态』的登记处正好在 catalog 手里」。
**这个判断现在不成立了** —— W-C 的 C1 引入 `TableOpener` 契约之后，
页号的归属关系由 db 层持有、模态通过 `TableOpener` 拿到自己的入口，
「让 db 层给模态发一个页号」这条路的**阻力已经消失**。
⇒ 但它**依赖 W-C 先落地**（`TableOpener` 到位才有地方挂），所以不排进 W-D，
而是**在 §9 的规则七里标注为 W-C 的后续项**。硬塞进 W-D 会造成 W-D 依赖一个还不存在的契约。

#### 待裁决的四项 —— 都要先写方案，再动代码

**#6 geo WAL 只 redo 不 undo / #7 geo 无第二份行存 / #8 geo WAL 放大 4 倍 / #16 ts WAL 放大 300 倍。**
这四项的共同点是：**它们不是「修一个 bug」，是「改一套设计」**。

- #6 的实际形态比清单描述的更彻底：`geo_wal.c:51` 写的是
  「xid 显式传 0（系统事务）—— geo 的公开 API 不带事务参数，模态层也拿不到事务上下文，
  所以这里没有『当前事务』可言」。也就是说 geo **根本没有事务边界**，
  要补 undo 就得先把 geo 接入事务模型（公开 API 加事务参数 → 全链改）。
- #7 与 #6 是同一件事的两面：没有第二份行存 ⇒ WAL 是唯一恢复路径 ⇒ WAL 只能 redo。
- #8 / #16 是 WAL 记录的**载荷粒度**问题（整页镜像 vs delta），
  但改它必须同时决定 redo 幂等性、页版本号、undo 语义 —— 与 #6 是同一批设计。

⇒ 这四项**必须打包成一个「WAL 记录格式 v2」的设计决策**，
由人（架构 owner）拍板选哪条路（页级 delta / 节点级 logical / 只记变更 slot / 压缩 payload），
再由 agent 落地。**agent 不得自行决定** —— 这不是实现细节，是数据可靠性策略。

#### 做的十一项 —— 为什么它们可以现在就做

共同点：**改动面窄、有明确的机械判据、不需要新的架构决策**。

- **#4 WAL 格式前向兼容（D-1，优先级最高）** ——
  这是唯一一个「不做就可能**静默损坏数据**」的欠账：
  `wal.c:202 wal_open` 今天是直接 `fopen(path, "r+b")`，
  一个格式不兼容的 WAL 文件被打开后，**恢复逻辑会按自己的格式去解析它**，
  最坏情况不是报错，而是**恢复出错误的数据**。
  改动面小（加文件头 + 打开时校验），收益是「把静默损坏变成显式报错」。

- **#1 FT 开机页回收（D-0）** ——
  实现已经在工作区（`db_ft.c` +66 行、`ft_index.c` +4 行、`ft_index.h` +15 行、
  新增 `test_ft_reclaim_boot.c` 177 行），集成测试 3 项验收通过。
  缺的只是「审查 diff + clean build + 全量门禁 + 单独提交」四步。
  ⇒ **它是第 0 步**，因为 §9 规则六要求先把这 4 个文件处置掉。

- **#2 FT 精确段页数上界（D-2）** ——
  `ft_merge.c:126-127` 今天的公式是「倒排字节 ÷ (每页容量 − 每页最多浪费的字节) + 1」，
  字典同理，是**宁多勿少**的保守上界 ⇒ 前几轮合并浪费页、文件比理论值大。
  改成读真实的 `FPH_USED` 与字典 slot 使用情况后，可以给出更紧的上界。
  判据明确：① 新上界 **不小于**真实需要；② 合并**不得**出现 `E_NOSPACE` 保守误报；
  ③ 现有 FT 全量测试保持通过；④ 记录新旧上界的差值。

- **#3 C7 物理读盘判据 + #17 ts A8 统一 benchmark（D-3）** ——
  这两项都是**测量口径**问题，不是内核行为问题，改动只在 `bench/` 下，风险最低。
  C7 今天是 `[SKIP]` 空判据（`0 <= 0`），因为帧池 4096 帧装得下整个库（3335 页）。
  办法很直接：给 bench 加显式帧池参数，让帧池小于数据页数，让判据真的有区分力。

- **#5 FT 按值撤销歧义（D-4）—— 歧义不存在，但撞出另一个真缺陷**

  **原判断**：`ft_tomb.h:80` 的契约是 `ft_tomb_page_remove(pg, page_id, doc_id, lsn)`
  —— 按 doc_id 找。若同一 doc_id 被不同事务重复删除，恢复期按值撤销
  理论上可能撤销掉错误事务的那条记录。

  **实测推翻**：`ft_index.c:479` 对**重复删除直接 return**：

  ```c
  if (ft_tomb_is(ix, doc_id))
      return OK;        /* 已经删过 ⇒ 不再写第二条记录 */
  ```

  于是「同一 doc_id 有两条 tombstone，撤销时分不清该撤哪条」这个前提**不成立**：
  第二条记录根本不存在。实测数据也印证：37 条 tombstone → 重复删一次 → 41（+4，
  是 4 个不同 doc_id）→ 再重复删 → **41（+0）**。

  ⇒ 所以「按值撤销」不会撤错 —— 因为**没有第二条可撤**。D-4 原定的改法取消。

  **但确认过程撞出一个真的缺陷（D-4b）**：运行时 `ROLLBACK` 不撤 FT 删除标记。
  实测：事务里删一条 → ROLLBACK → **同一会话里查到 0 条**（应为 1 条）；
  关库重开之后才恢复正常。
  根因：WAL 的 undo 只服务**崩溃恢复**，管不到当前会话的内存集合与 tombstone 页；
  而删除标记是**立即写页**的，于是「回滚了、但页上还留着删除标记」。
  ⇒ 补上连接作用域的 `db_ft_txn_{track,clear,rollback}` + `db_ft_delete_wired`（已提交 `1168c4284`）。

- **#11 geo KNN 二次全树扫描（D-5）** ——
  `db_geo_query.c:140-152` 的注释自己承认「为什么要扫两遍树」：
  `rtree_knn` 只给 row_id 不给距离（坑 M5-13），为了填距离字段要再扫一遍树捞坐标。
  ⇒ 候选项里直接带 row_id / MBR / 坐标 / 距离即可消除第二遍。
  判据：KNN 结果与 brute-force 一致 + 物理/逻辑页读减少。

- **#13 #14 #15 ts 三项（D-6）—— 已全部完成，且三条腿的诊断都被修正过**

  三项确实是同一个病根的三条腿，但**动手时发现三条腿的说法各有偏差**：

  - **#14（D-6.1）诊断偏了一半**。原判断是「`ts_am_set_time_range` 零调用方，
    缺一条线」。实测更严重：真实查询路径 `ts_am_mat_readings` 走的是
    `ts_store_scan`（**全表扫描**）+ `ts_am_in_parts`（逐行过滤）。
    能**整分区跳过**的 `ts_prune_scan` 只被 `bench/ts_bench_io.c` 直调过，
    真实 SQL 路径从来没接它。
    ⇒ 「时间谓词参与分区裁剪」这句在**行数上成立、在 IO 上不成立**：
    3 个分区只要 1 个时，行数对（300），但页读是 6/6 —— 一页没省。
    而且 T1–T4 那套判据测的是**直调** `ts_prune_scan`，绕开了真实查询路径，
    所以它们证明不了这件事。

  - **#13（D-6.2）要说清「谁收」**。原判断只写「旧裸写页只标记不回收」。
    实测确认 **TTL 收不了它们**：TTL 只收「目录项**现在**指着的那些页」，
    而封窗分区在目录里指的是列式头部页 ⇒ `release_column_part()` 永远碰不到
    那批旧裸写页（它们的位置只记在 `TSC_OLDFIRST` / `TSC_OLDPAGES`）。
    ⇒ 必须由封窗流程自己做，且顺序是「先清标记（落盘）→ 再回收页」：
    反过来崩在中间 ⇒ 标记还在、页已属别人 ⇒ 下次回收把**别人的页**清零。

  - **#15（D-6.3）原方案不可行**。原判断写「把栈元数据登记到 `PT_TS_META` 页」，
    但 `pager_free.c` 在 `src/storage/`（L1）、`PT_TS_META` 属于 `src/modalities/ts/`（L4），
    `tools/check_deps.sh` 明令禁止这个方向；而且 pager 自己没有自有持久化位置
    （第 0 页是 `PT_META`，归 catalog 管）。
    ⇒ 换成 **`PT_FREE` 页内标记 + 开机重建**：`pager_free()` 先把页类型改成
    `PT_FREE`，开库时在 WAL 重放**之后**扫页空间重建栈。
    选它的理由是「唯一不新增失败模式」—— 正确性完全建立在已经存在的
    「先写日志、后刷页」规则上，不需要新格式、不需要迁移、不引入新的 WAL 顺序命题。

### 10.4 与既有三份计划的关系

| | W-A | W-B | W-C | **W-D** |
|---|---|---|---|---|
| 性质 | 删代码 | 加护栏 + 加开关 | 加结构 + 加功能 | **补正确性与测量** |
| 改主核实现吗 | 否（只动 `engine/src/`、`engine/include/`、SDK） | 是（`Makefile` / `db_internal.h` / `db.c` / `db_instance.c` / `main.c`） | 是（`spi/` / `query/` / 模态目录 / REPL） | **是**（`ft/` / `geo/` / `ts/` / `wal/` / `bench/`） |
| 与 W-C 的文件重叠 | 无 | 与 C1 重叠（`db_internal.h` 等） | — | **无**（W-D 只改模态实现与 bench，不碰组合根） |

⇒ **W-D 不与 W-C 抢文件**，这正是它能独立成一个工作流的原因。
唯一需要注意的是 #11（D-5）会改 `db_geo.c` / `rtree_knn.c`，
而 W-C 的 C-2 会动 `src/spi/` 与 `src/modalities/relational/` —— 两者不重叠，但**执行顺序**仍按规则七。

### 10.5 这份台账的「不做」清单（防止后来者重复劳动）

以下事项**明确不做**，后来者不要再派 agent：

| 事项 | 原因 |
|---|---|
| #10 R-tree E_NOSPACE 保守误报 | 已修（`e48c4b1b8`） |
| #12 reindex 后 row_id 复用 | `geo_am_reindex` 是 `E_NOTSUP`，前提不成立；水位线已由 `dg_fix_high_water` 修 |
| #6 #7 #8 #16 四项 WAL 设计 | 待人工裁决，不由 agent 自行开工 |
| study-hub P2 / P3 / P4 / P5 | 与本设计无关（是研究账本/路线图，不是 db 内核的债） |
| 22c-monitoring-deployment | 已作废（C 语言微服务载体已删） |
| gap-analysis-roadmap 的 P0/P1/P2 | 已降级为历史参考（全部针对冻结轨） |
| Raft 迁移 | B2 决策明确「分布式：不做」；要推翻须单独决策 |

### 10.6 附：#1 的实现已核实（工作区当前状态）

> 下面是 2026-11-19 实测的工作区状态，**不是**清单里的转述。

```
 M engine/db/src/db_ft.c                     +66
 M engine/db/src/modalities/ft/ft_index.c     +4
 M engine/db/src/modalities/ft/ft_index.h    +15
?? engine/db/test/integration/test_ft_reclaim_boot.c   (177 行)
```

实现要点（`db_ft.c` 的 diff 已逐行读过）：
- 位置在 `ft_index_recover()` **之后** —— 顺序反了会把「还在用的页」判成垃圾
  （垃圾页被发给新段 ⇒ 两份数据互相踩且不报错），这是最坏的一种错误；
- 判据由段层给（`ft_segment_reclaimable_pages`），装池由索引层做（`ft_index_seg_pool` + `ft_pool_add`），
  两层各做自己懂的；
- 回收上限取 `npages / 2`（一个库不可能有一半以上页是垃圾），多出来的页下一轮再收；
- 分配失败就这轮不收 —— 池空着只浪费空间，不影响正确性。

⇒ 实现方向与本设计的「两层不互相掺」原则一致。后续四步（审查 → clean build →
全量门禁 → 单独提交）已完成，见 §10.7 的 D-0 一行。


### 10.7 执行结果（2026-11-20 补入）

> 这一节是**事后**写的，不是计划。它只记「实际发生了什么」，
> 尤其是**计划想错的地方** —— 那些才是下一次最该看的东西。

#### 逐项结果

| Task | 提交 | 结果 |
|---|---|---|
| D-0 ft 开机页回收 | `ace3a31d7` | 4 文件单独提交；门禁当时 95/95 |
| D-1 WAL 文件头 | `5d1ae2d4a` + `ea26666d8` | 实现把 `wal.c` 推到 889 行（破 800）⇒ 拆两提交，读写挪进 `wal_hdr.{h,c}` |
| D-2 FT 精确页数上界 | `c2def3080` | **做法改了**：不用 `FPH_USED` 累加，改成逐词模拟写入器分页 |
| D-3 测量口径 | `07ad8872` | C7 变三个子判据；新增 `bench/ts_bench_io.c` |
| D-4 tombstone 撤销 | `1168c4284` | **歧义不存在**；撞出 D-4b（运行时 ROLLBACK 不撤 FT） |
| D-5 geo KNN | `d66de13ef` | 页读 **739 → 11**（省 98.5%），结果逐位一致 |
| D-6.1 ts 时间窗 | `d6fb60c2e` | 真实 SQL 路径 6 页 → **4/0 页** |
| D-6.2 旧页回收 | `55567631e` | 空闲栈精确 +3 页；幂等；复用页全零；标记落盘 |
| D-6.3 空闲栈落盘 | `db9fc4838` | **方案换了**（见下）；13 页 vs 18 页证明跨会话复用 |
| D-7 group commit | `3381c22fb` | 模拟 2 ms 盘 4 并发：100 事务 → **50 次 fsync**（省 50%） |

全量门禁从 95 → **105 个测试 105 通过**
（新增 `test_wal_header`、`test_ft_merge_pages`、`test_ft_tomb_txn`、`test_geo_knn_points`、
`test_ts_page_prune`、`test_ts_page_reclaim`、`test_pager_free_rebuild`、
`test_ts_reuse_across_session`、`test_wal_group`、`test_wal_group_mt`）。

#### 五处「计划想错了」—— 这是本节的重点

**① D-4 的歧义不存在（最彻底的一次推翻）**

计划说「同一 doc_id 被不同事务重复删除，按值撤销可能撤错」。
但 `ft_index.c:479` 对重复删除**直接 return**：

```c
if (ft_tomb_is(ix, doc_id))
    return OK;        /* 已经删过 ⇒ 不写第二条 */
```

⇒ 第二条记录根本不存在，也就无从「撤错」。实测：37 → 41（+4，四个不同 doc_id）→
再重复删 → **41（+0）**。

这次推翻是**好事**：省掉一次改磁盘格式的工作。但它同时撞出一个真缺陷（D-4b）：
运行时 `ROLLBACK` 不撤 FT 删除标记 —— WAL 的 undo 只服务崩溃恢复，管不到
当前会话的内存集合；而删除标记是立即写页的。

**② D-6.1 的诊断偏了一半**

计划说「`ts_am_set_time_range` 零调用方，缺一条线」。实际更严重：
真实查询路径走的是 `ts_store_scan`（**全表扫描**）+ 逐行过滤；
能整分区跳过的 `ts_prune_scan` **只被 bench 直调过**。

所以在改之前，「时间谓词参与分区裁剪」这句在**行数上成立、在 IO 上不成立**。
而 T1–T4 那套判据测的是直调 `ts_prune_scan`，绕开了真实路径 ⇒ 它们证明不了这件事。

⇒ 教训：**「函数写好了」不等于「路径接上了」**。判据必须从用户入口打进去。

**③ D-6.3 的原方案违反分层铁律**

计划说「把栈元数据登记到 `PT_TS_META` 页」。但：
- `pager_free.c` 在 `src/storage/`（L1），`PT_TS_META` 属于 `src/modalities/ts/`（L4）；
  `tools/check_deps.sh` 明令禁止这个方向。
- pager 自己没有自有持久化位置（第 0 页是 `PT_META`，归 catalog 管）。

⇒ 三方案权衡（页内标记+开机重建 / 页 0 槽位+db 层钩子 / 维持内存栈不做），
选**页内标记**：唯一不新增失败模式的 —— 正确性完全建立在已经存在的
「先写日志、后刷页」规则上。

顺手记一个撞号事故：新页类型第一次取了 `0x15`，与 `PT_RTREE` 冲突 ⇒
`test_geo_crash` 三条 `db_open` 全失败。是既有测试当场抓住的。
页类型是**全局命名空间**（坑 M6-27），最终取 `0x1C`。

**④ D-7 拆文件是被行数上限逼的**

`wal.c` 原本 793 行（硬上限 800）—— 一行都加不进去。
⇒ 新增 `wal_internal.h`（`struct Wal`，只在 wal/ 内可见）+ `wal_group.c`（协议）；
`wal.c` 793 → 672 行。与 `storage/pager_internal.h` 同一个做法。

**⑤ 判据不标定 = 没判据（两次，同一种毛病）**

| 次数 | 判据 | 变异/实测结果 | 修法 |
|---|---|---|---|
| 第 1 次（D-6.3） | 文件页数 `B*4 < A*7`（1.75 倍，「给足余量」） | 关掉重建 ⇒ 13→18 页 ⇒ **没红** | 按实测收紧到 1.15 倍 |
| 第 2 次（D-7） | 多线程 fsync `< 事务数/2` | 原速 100/100 ⇒ 红（**但机器太快，不是缺陷**） | 分三段：原速如实记录 / 模拟 2 ms 盘 / 单线程对照 |

⇒ **「给足余量」是同义词「抓不住回归」**。判据要按实测定标，
并且必须在同一台机器上可复现。

D-7 里还发现一个更细的坑：4 并发攒批有**结构性下限**（第 1 轮只能覆盖发起者
自己，第 2 轮覆盖所有已到达者 ⇒ 每群 4 个事务共 2 次 fsync），所以省 50% 是
这个并发度下的上限，不是我一开始拍脑袋的 25% 或 75%。

#### 一个结构性的边界（不修，但必须写明）

D-6.1 之后双边界范围 `time >= a AND time < b` **仍然只下推一边**：

```c
typedef int (*PushFn)(StorageAM *am, const Predicate *p, AmScan **out);
```

`PushFn` 一次只能带**一个** `Predicate`，而双边界是两个谓词；执行器挑
「选择性最好」的那一个下推，另一边由执行器完整求值。
⇒ 行数永远精确，但分区裁剪可能只裁一边。

要彻底解决必须让 `PushFn` 携带**谓词合取** —— 那是 `src/spi/pushdown.h` 的契约变更，
属于 W-C 的地盘（Global Constraint 5 明令 W-D 不碰）。所以这一条**记在这里**，
留给 W-C 之后的 SPI 演进，不假装已经修好。


## 附录 A：核实命令

```powershell
# 主核体量与模态分布
Get-ChildItem engine/db/src/modalities -Directory | ForEach-Object {
  $f = Get-ChildItem $_.FullName -Recurse -File | Where-Object { $_.Extension -in '.c','.h' }
  $l = 0; foreach ($x in $f) { $l += (Get-Content $x.FullName | Measure-Object -Line).Lines }
  "{0,-8} 文件={1,-4} 行数={2}" -f $_.Name, $f.Count, $l
}

# 模态 → 底座接口面（13 个头，无 internal 泄漏）
Select-String -Path "engine/db/src/modalities/*/*.c","engine/db/src/modalities/*/*.h" `
  -Pattern '#include\s+"(storage|common|spi)/([^"]+)"' -AllMatches

# 无文件锁（多进程阻断项）
Select-String -Path "engine/db/src/**/*.c" -Pattern "flock|fcntl|LockFile|lockf"   # 零命中

# 冻结轨体量
Get-ChildItem engine/src/db -Recurse -File | Where-Object { $_.Extension -in '.c','.h','.cpp' } | Measure-Object

# SDK 对冻结内核的真实依赖
Select-String -Path "engine/src/sdk/**/*.c" -Pattern "faiss_hnsw_|vector_index_|sql_engine"

# 文档引用面
Select-String -Path "docs/**/*.md","openspec/**/*.md","engine/docs/**/*.md" -Pattern "src/db/"

# ---- 2026-11-18 复审新增的核实命令 ----

# B1: SDK 对冻结内核 memctx 的真实依赖（原文漏判的那条）
Select-String -Path "engine/include/sdk/impl/mmdb_memctx.h" -Pattern '#include'
Select-String -Path "engine/src/sdk/**/*.c" -Pattern 'palloc|pfree|AllocSetContextCreate|MemoryContextDelete|MemoryContextReset'
#   → 实现体在 engine/src/db/sql/memctx.c；mmdb_mem_register_resource 只在该文件定义

# memctx.h 对 parsenodes.h 的依赖有多深（决定移植成本）
Select-String -Path "engine/include/db/sql/memctx.h" -Pattern 'NodeTag|parsenodes'
#   → NodeTag 共 114 项，memctx 只用 T_MemoryContext / T_AllocSetContext 两项

# B2: 测试的【链接目标】依赖（只看源码 include 会漏）
Select-String -Path "engine/test/index/**/CMakeLists.txt" -Pattern 'target_link_libraries' -Context 0,8
#   → brin/bitmap/gin 链 mm_vector_index；diskann 链 diskann_index（该目标名不存在）

# B3: parser 能否表达 json_match(...) 与 json '$.a.b'
Select-String -Path "engine/db/src/query/parser.h"     -Pattern 'EX_[A-Z]+'      # 无通用函数调用节点
Select-String -Path "engine/db/src/query/parser_stmt.c" -Pattern '"INT"|"TEXT"|"REAL"|"JSON"'  # 无 JSON 类型名
Select-String -Path "engine/db/src/query/parser_stmt.c" -Pattern 'index_col'      # 只支持标识符，无路径字面量

# B4: 执行器与模态的真实耦合面（只查 include，注释不算）
Select-String -Path "engine/db/src/query/*.c","engine/db/src/query/*.h" -Pattern '#include\s+"(heap|blob|graph|vector|json)'
#   → 只看 include：零命中即为达标（直接查名字会有 26 处注释命中）

# B5: db_instance.c 的模态关闭点（两条路径：正常 + 错误回滚）
Select-String -Path "engine/db/src/db_instance.c" -Pattern 'db_vec_close|db_ft_close|db_geo_close|db_ts_close|blob_store_destroy|graph_destroy'

# B6: 页类型隔离（模态自持页类型，不污染 L0）
Select-String -Path "engine/db/src/modalities/doc/json_ovf.h" -Pattern 'PT_JSON_OVF'
#   → 自定义为 0x1Bu，不在 common/config.h 的公共枚举里（该枚举止于 PT_FT_CATALOG = 0x14）
```

## 附录 B：相关既有文档

| 文档 | 关系 |
|---|---|
| `docs/superpowers/specs/2026-10-27-per-modality-build-isolation-design.md` | 同一问题的旧轨版本；§4 的机制来源 |
| `docs/superpowers/plans/2026-10-27-per-modality-build-isolation.md` | 上述设计的实施计划 |
| `docs/superpowers/plans/2026-11-16-srcdb-asset-triage.md` | 冻结轨逐模块分级（§2.6） |
| `docs/superpowers/specs/2026-11-15-code-umbrella-and-db-convergence-design.md` | 2026-11-16 路线翻转的来源 |
| `engine/db/docs/DESIGN.md` | 主核整体设计（D1–D10、SPI 三件套） |
| `engine/db/docs/MODALITIES.md` | 八模态实现方案（§2.5 跨模态事务语义、§6.1 对称性定义的出处） |
| `engine/db/docs/PLAN-v1.6.md` | 文档模态的完整实现计划（§6.4 的执行依据） |
| `engine/db/README.md` | 主核七条硬规则（§4.3 的边界来源） |

## 附录 C：`SPI_OPENER_PRIV_MAX = 512` 的取值依据

`HeapAmCtx`（`spi/heap_am.h:36-50`）的字段与估算尺寸（64 位平台）：

| 字段 | 类型 | 字节 |
|---|---|---|
| `heap` | `Heap *` | 8 |
| `cols` | `const ColDesc *` | 8 |
| `ncols` | `int` | 4（+4 对齐填充） |
| `idxs` | `const IndexInfo *` | 8 |
| `nidx` | `int` | 4（+4 对齐填充） |
| `layout` | `RecLayout` | 142 → 144（对齐后） |

**合计 ≈ 184 字节**。⚠️ **（复审修正）这个依据不够** —— 关系模态的私有状态实际是 `Heap*`(8) + `HeapAmCtx`(184) + `StorageAM`(152) = **344 字节**，原来的 256 装不下 ⇒ 常量已提到 **512**（留 168 字节余量）。

`RecLayout` 的构成（`storage/record.h:113-125`）：7 个 `u16`（14 字节）+ 2 个 `u16[REC_MAX_COLS]`。
`REC_MAX_COLS` = 32 → 2 × 32 × 2 = 128 字节 → 合计 142 字节。

> 该常量必须写在 `spi/table_opener.h` 里，并在编译期校验适配器的 ctx 不超过它（超限即编译错），
> 而不是等到运行时才发现栈溢出。
>
> **（复审）契约接口不传 `ctx_cap`** —— 上限是编译期常量，运行时再传 cap 没有意义，
> 反而让适配器有机会收到一个更小的 cap 然后写超界。改为在适配器侧静态断言：
>
> ```c
> /* modalities/relational/heap_am.c */
> _Static_assert(sizeof(HeapAmCtx) <= SPI_OPENER_PRIV_MAX,
"HeapAmCtx 超过 SPI_OPENER_PRIV_MAX，需调大常量或压缩 ctx");
> ```
>
> 这让「ctx 超限」从**运行期内存损坏**降级为**编译期错误**。
