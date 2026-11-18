# 多模态数据库工程组织设计：主核形态 + 冻结轨退役

> 状态：设计稿，待用户批准
> 日期：2026-11-18
> 范围：`projects` 仓 —— 主核 `engine/db/` 的工程组织形态，以及冻结轨 `engine/src/db/` 的退役
> 所有「现状」数字均为 **2026-11-18 实测**，核实命令见附录 A

---

## 0. 一句话

**主核保持「一个工程 + 编译期模态裁剪」（不加仓、不加进程、不加插件）；八模态在「SPI 适配器 + 公开 API + REPL 族 + 模态文档」四件套上补全对称（为此给 SPI 加第四契约 `TableOpener`）；文档模态功能补全（接进 SQL）；冻结轨 `engine/src/db/` 全删（173,719 行），SDK 去冻结化后保留。**

---

## 1. 问题陈述

`projects` 仓当前有**两个数据库内核**：

| | 体量 | 状态 | CI 覆盖 |
|---|---|---|---|
| **主核** `engine/db/` | 247 文件 / **50,867 行** | **唯一在维护**（2026-11-16 路线翻转） | **零** ❌ |
| **冻结轨** `engine/src/db/` | 524 文件 / **173,719 行** | 已冻结，保留作历史参考 | **在 CI 里被构建** ✅ |

冻结轨体量是主核的 3.4 倍，且主核一行 CI 都没有。这带来三个具体问题：

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

**关键数字**：模态可见的底座接口面 = **13 个头文件 / 约 1,313 行**，占 51k 行代码的 **2.6%**。
`pager_internal.h` / `heap_internal.h` / `heap_am_internal.h` / `blob_hdr.h` —— 模态一个都没碰。

### 2.2 主核的体量分布

| 部分 | 行数 | 占比 |
|---|---|---|
| 共享底座 `common`+`storage`+`wal`+`index`+`txn`+`catalog`+`query`+`spi` | **19,066** | 37% |
| 六个模态 `ft` 8,850 / `ts` 5,580 / `geo` 3,244 / `vector` 3,088 / `doc` 2,829 / `graph` 2,056 | **25,647** | 50% |
| 各模态公开 API + REPL（`db_ft.c` / `repl_geo.c` …） | ~6,154 | 12% |

### 2.3 「公共代码抽到单独目录」这个诉求的现状

**已经满足，且是 deep module 形态**：底座 19,066 行对外只暴露 1,313 行头文件；
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
**该清单漏了 `index/`（46,805 行 / 27 个 ANN + 15 个传统索引）** —— 它只盘了「工程面」。

**③ 用户裁决（2026-11-18）**：冻结轨**全删**，含索引动物园与分布式。

### 2.7 冻结轨的依赖分层（实测，本轮补做）

| 子树 | 行数 | 是否依赖冻结内核 | 依赖证据 |
|---|---|---|---|
| `index/` | **46,805** | **否** | 只用自己的头 + `core/log.h` + `index/heap/heap_vector_store.h` |
| `distributed`+`consensus`+`sharding` | **6,684** | **否** | + `db/shard_routing.h` |
| `graph/` | 4,910 | **是** | `db/core/func_registry.h`、`db/core/log.h` |
| `tools/` | 2,390 | **是** | `db/` 顶层（CLI/server） |
| `protocol/` | 2,329 | 部分 | `sql_compat.c`/`analytics_sql.c` 独立，另两个是 stub |
| 其余内核本体 | **110,601** | — | storage/sql/parser/executor/core/api/cli/optimizer/vectorized/yang/… （= 173,719 − 上述各行） |

### 2.8 SDK 的真实依赖（推翻了一个直觉）

`engine/src/sdk/` 35 文件 / 7,196 行，7 个模块。**6 个是 SQLite 后端**（`core/sqlite_backend.c`、`text/text_fts5.c`）：

| 模块 | 后端 | 依赖冻结内核？ |
|---|---|---|
| `core`（14） | SQLite | 否 |
| `text`（3） | SQLite FTS5 | 否 |
| `graph`（3） | SQLite | 否 |
| `timeseries`（3） | SQLite | 否 |
| `aggregation`（1） | SQLite | 否 |
| `extra`（8） | SQLite + vectors | 间接 |
| **`vectors`（3）** | **faiss_hnsw** | **是** —— `vectors.c` 有 22 处 `faiss_hnsw_*` / `vector_index_selector_*` 引用 |

`src/sdk/vectors/index_flat.c` 是个 **dummy**：`int mmdb_index_flat_dummy(void) { return 0; }` —— 正是为此预留的占位。

### 2.9 测试面的依赖分类（实测）

| 目录 | 文件数 | 依赖冻结内核 | 独立 | 处置 |
|---|---|---|---|---|
| `engine/test/db/` | 289 | 289 | 0 | **删** |
| `engine/test/smoke/` | 13 | 13 | 0 | **删**（全是旧轨 profile 冒烟 `mm_*_smoke`） |
| `engine/test/sdk/` | 36 | 7 | 29 | 7 个随 HNSW 改造处理，29 个留 |
| `engine/test/index/` | 8 | 0 | 8 | 留 |
| `engine/test/kbase/` | 7 | 0 | 7 | 留 |
| `engine/test/algo/`、`self_made_cpp/` | 14 | 0 | 14 | 留 |

### 2.10 文档引用面

**912 处** `src/db/` 引用：`docs/analysis` 869 + `docs/arch` 32 + `docs/storage` 8 + `openspec/specs` 3。
其中**带精确行号的引用实测为 0** → `scripts/check-wiki-refs.py` 不会因行号越界失败，但会产生死链。

### 2.11 八模态的对称性缺口（实测）

八模态在四条线上的对齐情况：

| 模态 | 实现 | SPI 适配器 | 公开 API | REPL 族 | SQL 入口 |
|---|---|---|---|---|---|
| 0 关系 | `storage/heap*`（**L1，被 L2 依赖**） | `spi/heap_am*` ❌ | 内嵌在 db.c ❌ | SQL | catalog 真表 |
| 1 blob | `storage/blob*` ❌ | `spi/blob_am*` ❌ | `db_blob.c` ✓ | BLOB ✓ | blob_frames/blob_objects |
| 2 图 | `modalities/graph/` ✓ | `graph_am.c` ✓ | `db_graph.c` ✓ | GRAPH ✓ | graph_nodes/graph_edges |
| 3 向量 | `modalities/vector/` ✓ | `vector_am.c` + `hnsw_am.c` ✓ | `db_vec.c` ✓ | VEC ✓ | vec_items |
| 4 全文 | `modalities/ft/` ✓ | `ft_am.c` ✓ | `db_ft.c` ✓ | FT ✓ | ft_docs |
| 5 空间 | `modalities/geo/` ✓ | `geo_am.c` ✓ | `db_geo.c` ✓ | GEO ✓ | geo_points |
| 6 时序 | `modalities/ts/` ✓ | `ts_am.c` ✓ | `db_ts.c` ✓ | TS ✓ | ts_readings/ts_partitions |
| 7 **文档** | `modalities/doc/` ✓ | **无**（寄生模态，设计如此） | **无** ❌ | **无** ❌ | **未接入** ❌ |

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
| 步骤 3 heap 列类型 + 大文档外置 | 🟡 **部分** | `T_JSON = 6`（`record.h:53`）+ 变长区判定（`record.h:64`）已定义；`json_ovf.c` 页链已实现；**但 heap 记录编解码是否真的走 `T_JSON` 未验证** |
| 步骤 4 路径索引（写时维护） | ❌ 未做 | 无 `CREATE INDEX ... (json '$.a.b')` 路径 |
| 步骤 5 执行器接线 + `json_match` | ❌ 未做 | `include/db.h` 里**零个** `db_json_*`；`src/query/` 里零个 json |
| 步骤 6 MVCC + 崩溃 + 反向验证 + 收官复盘 | ❌ 未做 | — |

---

## 3. 判决

| # | 判决 | 理由 |
|---|---|---|
| **D1** | 主核保持**单仓单二进制**，用**编译期模态裁剪**实现「模态可单独构建/单独部署」 | 模态共享同一套运行时（pager/WAL/xid/缓冲池），进程内耦合无法靠目录结构切断；§2.4 的三项阻断决定了多进程当前不可行 |
| **D2** | **不做**插件化（`.so` 动态加载） | 要把 C 头文件 SPI 升级成稳定 ABI 契约（结构体布局 / 函数指针表 / 内存所有权跨模块 / CRT 差异），成本远大于「发版便利」的收益；且 `spi/storage_am.h` 151 行含大量 inline 与宏，ABI 易碎 |
| **D3** | **不重构目录**：公共代码就是现有的 L0–L3（19,066 行），`check_deps.sh` 就是它的门禁 | §2.1/§2.3：接口面已收敛到 1,313 行薄契约、零 internal 泄漏 |
| **D4** | **删除** `engine/src/db/` 全树（524 文件 / 173,719 行）+ `engine/test/db/`（289）+ `engine/include/db/`（~300 头） | 用户裁决 2026-11-18。§2.2：冻结轨体量是主核 3.4 倍且无人维护 |
| **D5** | **保留 SDK，去冻结化**：摘掉 `faiss_hnsw`/`mm_vector_index`/`sql_engine` 三个链接依赖；`sdk/vectors` 的 HNSW 后端改为 `index_flat.c` 实现真扁平索引 | §2.8：SDK 6/7 模块是 SQLite 后端，与冻结内核无关；`index_flat.c` 本就是为此预留的占位 |
| **D6** | **主核补 CI** | 主核 50,867 行 / 94 个测试文件当前**零 CI 覆盖**，而冻结轨在 CI 里 |
| **D7** | 编译期裁剪用**单一 `MM` 变量**（不是 14 个独立布尔开关） | 2026-10-27 设计明列的反目标：「宏组合爆炸」 |
| **D8** | **方案 C（多进程 / 多仓）留预备**，seam 固定为 §2.1 的 1,313 行头文件 | 真要拆的那天按这条缝切即可。现在拆只会先付进程间协调成本，且会堵死已排期的 v1.7 跨模态联合查询 |
| **D9** | **八模态对称化**：8 个模态统一为「SPI 适配器 + 公开 API + REPL 族 + 模态文档」四件套，且**适配器一律在 `src/modalities/<m>/` 下** | §2.1：模态可见的底座接口面已是 1,313 行薄契约，**边界够干净到可以谈对称**；对称化让「八模态」从文档说法变成结构事实 |
| **D10** | **给 SPI 加第四个契约 `TableOpener`**：关系模态的适配器进 L4，执行器不再 include 任何模态头 | 关系模态是唯一被执行器直接调用的模态（`exec_scan.c` 的 `heap_am_build`）。它是「底座型模态」，实现横跨 L1–L3，搬不进 L4 —— 只有把「表打开」抽象成契约才能真正对称。**这也是 v1.7 跨模态联合查询的前置** |
| **D11** | **文档模态功能补全**：执行 `PLAN-v1.6` 步骤 3~6，把 JSON 接进 SQL（`json_match` + 路径索引） | §2.11：JSONB 编解码/路径/溢出链已实现且测试全绿（2,829 行），但 `db.h` 里零个 `db_json_*`、无路径索引、未接执行器 —— 八模态里唯一功能未闭环的 |

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

**裁剪点只有三处**，不做满地 `#ifdef`（2026-10-27 设计的反目标）：

| # | 位置 | 做法 |
|---|---|---|
| 1 | 源文件级 | Makefile 的 `SRCS` 过滤 —— 裁掉源文件即裁掉该模态的注册代码 |
| 2 | 实例结构体级 | `src/db_internal.h` 的 per-modality 字段与 `#include` 用 `#if DB_HAS_<MOD>` 包裹 |
| 3 | 实例初始化级 | `src/db.c` 里 `db_vec_open` / `db_graph_open` / … 的调用点用 `#if DB_HAS_<MOD>` 包裹 |

**不需要**旧轨那套「注册表反转 + 生成 `mm_builtin_engines.c`」：
主核的注册本来就是**分散在每个模态自己的公开 API 文件里**（`db_vec.c` 注册 `vec_items`、
`db_graph.c` 注册 `graph_nodes`/`graph_edges`），裁掉源文件即裁掉注册。

### 4.3 每模态 smoke 目标（独立可用的机械证明）

`make MM=vector` 额外产出 `build/smoke_vector.exe`，**只链接**该模态 + L0–L3 底座 + 模态自己的公开 API。

它存在的唯一意义是**链接期证明**：如果这个模态偷偷依赖了别的模态，链接会失败。
内容：初始化 → 建一个最小对象 → 写一条 → 读回 → 断言 → 关闭。

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

`.github/workflows/ci.yml` 现有 4 个 job **全是 CMake 工程轨**，主核一次都没跑过。
新增 job `Build & Test Main Kernel (engine/db)`：

```yaml
- name: Main Kernel — deps + build + test (MinGW/GCC)
  run: |
    cd engine/db
    mingw32-make clean && mingw32-make all      # -Wall -Wextra -Werror 零警告
    sh tools/check_deps.sh                        # 期望 ✓ 依赖方向正确
    sh test/run.sh                                # 期望 0 FAIL
```

Linux runner 上另跑一遍（`make all` + `sh test/run.sh`），因为 `check_deps.sh` 与 `run.sh` 都是 POSIX sh。
**失败即红灯** —— 主核从此不再是「无门禁区」。

### 4.6 阶段与顺序

| 阶段 | 内容 | 前置 |
|---|---|---|
| **P0** | 主核 CI（§4.5） | 无 —— **先做**，因为它给后面所有改动兜底 |
| **P1** | `MM` 编译期开关 + 闭包校验（§4.2） | P0 |
| **P2** | 每模态 smoke 目标 + `measure_build.sh`（§4.3/§4.4） | P1 |
| **P3** | D8 预备：把 seam（1,313 行头文件）整理成可发布的头文件包 | P1–P2 完成后再评估 |

> 冻结轨退役（§5）是**独立工作流 W-A**，不属于本表的阶段序列 —— 它与 P0/P1/P2 的交叉点只有一条：
> **W-A 必须在 P0 之后执行**（§9）。八模态对称化（§6）是**独立工作流 W-C**，与 P1 的先后见 §9。

---

## 5. 设计 B：冻结轨退役

### 5.1 删除清单

| # | 对象 | 规模 | 依据 |
|---|---|---|---|
| D-1 | `engine/src/db/` | 524 文件 / 173,719 行 | 用户裁决；§2.7 依赖分层已查实 |
| D-2 | `engine/test/db/` | 289 文件（≈587 条 ctest 用例） | 测的都是被删的内核 |
| D-3 | `engine/test/smoke/` | 13 文件 | 全是旧轨 profile 冒烟（`mm_*_smoke`） |
| D-4 | `engine/include/db/` | ~300 公共头 | 冻结内核的公开 API |
| D-5 | `engine/include/rag/`、`engine/include/tree_index/` | 3 + 9 头 | 分别对应 `executor/rag/` 与 `src/db/index/` 的传统索引 |
| D-6 | `engine/include/sdk/` 里的**过期注释**（`sdk/impl/mmdb_memctx.h:116`） | 1 处 | 实测：SDK 公共头**零处**引用冻结内核类型；唯一命中是一段声称「本头文件已通过 `#include "db/sql/memctx.h"` 自动暴露其声明」的过期注释，实际并无此 include → 订正注释即可，**SDK 公共头整体保留** |
| D-7 | `engine/cmake/ModalityProfiles.cmake`、`engine/cmake/MultimodalConfig.cmake` | 2 文件 | 旧轨的 profile 机制；主核用 Makefile，不进 CMake 图 |
| D-8 | `engine/tools/benchmark/c_api/` | 目录 | 链 `mm_vector_index`/`faiss_hnsw` |
| D-9 | `engine/test/sdk/` 中依赖 HNSW 的 7 个文件 | 7 文件 | §2.9 |

### 5.2 保留清单（**不动**）

| 对象 | 说明 |
|---|---|
| `engine/src/{algo-prod,cpp,ds,redis}/` | 与冻结内核无关 |
| `engine/src/sdk/` | 7 个模块，去冻结化后保留（D5） |
| `engine/test/{algo,index,kbase,ds,smoke→见D-3,self_made_cpp}/` | §2.9 独立部分 |
| `engine/test/sdk/` 的 29 个独立文件 | |
| `engine/db/`（主核） | 本设计的主体 |

### 5.3 必须同步修的构建点

| # | 位置 | 动作 |
|---|---|---|
| B-1 | `engine/src/CMakeLists.txt:21` | 删 `add_subdirectory(db)` |
| B-2 | `engine/test/CMakeLists.txt:27-28` | 删 `add_subdirectory(db)` 与 `add_subdirectory(db/consensus)` |
| B-3 | `engine/CMakeLists.txt:84` | 删 `ENABLE_FROZEN_SRC_DB_TESTS` 选项 |
| B-4 | `engine/CMakeLists.txt:59-60, 155` | 删 `ModalityProfiles.cmake` / `MultimodalConfig.cmake` 的 include 与 `multimodal_config_gen` 链接 |
| B-5 | `engine/src/sdk/CMakeLists.txt:112` | `target_link_libraries(mmsdk PUBLIC sqlite3 faiss_hnsw mm_vector_index sql_engine)` → `target_link_libraries(mmsdk PUBLIC sqlite3)` |
| B-6 | `engine/src/sdk/CMakeLists.txt:103` | 删掉为 `faiss_hnsw.h` 等 db 模块头准备的 `../` include 路径 |
| B-7 | `engine/src/sdk/vectors/vectors.c` | HNSW 后端改为调用 `index_flat.c` 的扁平索引；`vector_index_selector` 调用改为本地决策 |
| B-8 | `engine/src/sdk/vectors/index_flat.c` | 把 dummy 换成真实现（插入 / 精确 KNN / 过滤后候选） |

**B-7/B-8 的降级语义必须写进 SDK 文档**：SDK 的向量检索从「HNSW 近似」降级为「暴力精确 KNN」——
复杂度从 O(log N) 变 O(N)。这是**能力降级**，必须在 `engine/include/sdk/mmdb_vectors.h` 的注释与
`engine/docs/` 里写明，不能默默改。

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
grep -rn "src/db/" engine/ --include=CMakeLists.txt --include=*.c --include=*.h   # 零命中

# 2) CMake 轨仍可构建、测试通过
cmake -B build/engine -S engine -DBUILD_TESTING=ON
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure

# 3) SDK 不再有 undefined symbol
nm -u build/engine/src/sdk/libmmsdk.a | grep -Ei "faiss|hnsw|sql_engine|memctx_"   # 零命中

# 4) 引用校验门禁
python scripts/check-wiki-refs.py

# 5) 主核不受影响
cd engine/db && sh tools/check_deps.sh && sh test/run.sh
```

---

## 6. 设计 C：八模态对称化与功能补全

### 6.1 「对称」的定义（先定义，再补全）

**模态的契约面 = 四件套**，位置统一：

| 件 | 统一位置 | 关系 | blob | 图 | 向量 | 全文 | 空间 | 时序 | 文档 |
|---|---|---|---|---|---|---|---|---|---|
| ① SPI 适配器 | `src/modalities/<m>/` | `relational/heap_am.c` **← 新建** | `blob/blob_am.c` **← 搬** | ✓ | ✓ | ✓ | ✓ | ✓ | **无**（寄生模态，设计如此） |
| ② 公开 API | `src/db_<m>.c` | SQL 路径（无独立文件，见 S4） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | **`db_json.c` 新建** |
| ③ REPL 族 | `src/repl_<m>.c` | SQL | BLOB ✓ | GRAPH ✓ | VEC ✓ | FT ✓ | GEO ✓ | TS ✓ | **`repl_json.c` 新建** |
| ④ 模态文档 | `MODALITIES.md` 对应节 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓（补契约面小节） |

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
/* 打开一张「catalog 真表」并构造它的 StorageAM。
 * 关系模态是当前唯一使用者，但它住在 L4 —— 执行器只认这个接口。 */
typedef struct {
    /* 构造表 tid 的 AM。ctx_out 指向调用方提供的不透明缓冲区
     * （≥ SPI_AM_CTX_MAX 字节），由适配器写入其私有状态。 */
    int  (*build)(void *inst_ctx, u32 tid, StorageAM *am_out,
                  void *ctx_out, u32 ctx_cap);
    void *inst_ctx;              /* 实例级上下文，由组合根持有 */
} TableOpener;
```

- **不透明 ctx 缓冲区**：`SPI_AM_CTX_MAX = 256`（实测 `HeapAmCtx` ≈ 184 字节，附录 C）。
  `TableCtx.am_ctx` 从 `HeapAmCtx` 改成 `u8 am_ctx_buf[SPI_AM_CTX_MAX]` →
  **执行器不再 include 任何模态头**。
- **组合根（`db.c`）持有**：实例级保存一个 `TableOpener`，指向关系模态提供的实现。
- **门禁加强**：`check_deps.sh` 增加一条 —— `src/query/` 下**禁止出现任何模态名**
  （`heap|blob|graph|vec|ft|geo|ts|json` 的 include）。可机械验证，与 PLAN-v1.6 的 A7 审计同款思路，但提前到 C 阶段做。

**收益**：
1. 执行器真正模态无关（`grep -iE "heap|blob|graph|vector|json" src/query/` 零命中）
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
| S9 | 门禁加强 | `tools/check_deps.sh` | ① `src/query/` 禁含模态名；② 模态适配器必须在 `src/modalities/` 下 | 可机械验证 |

**S3 + S6 做完后的 `spi/` 目录**（只剩**纯契约 + 机制**）：

```
src/spi/
├── storage_am.h      存储 AM 契约（v0.9 定稿）
├── index_am.h        索引 AM 契约（v0.9 定稿）
├── pushdown.h        谓词下推契约（v0.9 定稿）
├── table_opener.h    表打开契约（D10 新增）
└── registry.{c,h}    虚拟表注册表（实例级机制）
```

### 6.4 文档模态的功能补全（`PLAN-v1.6` 步骤 3~6）

| 步骤 | 动作 | 验收 |
|---|---|---|
| 步骤 3 收尾 | 验证 heap 记录编解码真的走 `T_JSON`；大文档外置接 `PT_JSON_OVF` 页链（`ext_fn` 换成沿链读，契约一行不改） | A2：只取路径字段时**不读全文**（页读计数对比） |
| 步骤 4 | 路径索引：`CREATE INDEX ON t (json '$.a.b')`；**写时同步维护**（UPDATE 整替换 → 删旧键插新键，用既有 B+tree 能力） | A4：索引项计数对账，旧键不残留 |
| 步骤 5 | 执行器接线 + `json_match(doc, '$.a.b = 5')`；**走/不走索引两路结果集全等** | A3 + A7 |
| 步骤 6 | MVCC（RR 快照下文档不撕裂）+ 崩溃恢复 + **反向验证 2 处**（标量映射把 null 映成 0；UPDATE 漏删旧键）+ 收官复盘 | A5 / A6 / A8 |

**A7 审计清单（先定义后实现）**：执行器允许的改动落点**只有两处** ——

1. **通用值层的路径变体**：`value.h` / `record.h` 承载 JSON 值（`T_JSON` 已有，需补 `DbValue` 的 JSON 变体或明确走外部值通道）
2. **求值入口**：`exec_scan.c` 的谓词求值处识别 `json_match(...)` 并委托给 `modalities/doc/json_match.c`

其余任何 `if (strcmp(col_type, "json"))` 形态的模态特判 = **零容忍**。

### 6.5 `MM` 开关与 smoke 覆盖 8 个模态

profile 补全（`engine/db/tools/mm_profiles.mk` 为唯一真相源）：

| profile | 打开的模态 | 闭包自动含 | 产物 |
|---|---|---|---|
| `MM=relational` | 关系 | 底座 L0–L3 | `db.exe` + `smoke_relational` |
| `MM=blob` | blob | 底座 L0–L1（+ 实例编排所需的 wal） | `db.exe` + `smoke_blob` |
| `MM=graph` / `vector` / `ft` / `geo` / `ts` | 各 1 个 | 底座 L0–L3 | 对应 smoke |
| `MM=doc` | 文档 | 底座 + **关系**（寄生模态必须依赖它的宿主） | `db.exe` + `smoke_doc` |
| `MM=full` | 全部 8 个 | — | 全部 8 个 smoke |

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
| **C-4** | PLAN-v1.6 步骤 3~6：文档模态接 SQL（含 S7 / S8） | C-1（执行器已脱钩，改动面小） |
| **C-5** | S9 门禁加强 + `MM` 开关扩到 8 个 profile + 8 个 smoke | C-1~C-4 |

---

## 7. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| **ctest 用例数从 ~835 掉到 ~250** | 高 | 这不是回归 —— 是删掉了 289 个测冻结内核的测试文件。**必须采改造前基线**（失败用例名集合，跑两次取交集），只比「集合不新增」而非绝对值（2026-10-27 设计的教训） |
| **SDK 向量检索降级为暴力 KNN** | 中 | 写进 SDK 文档与 changelog；不静默改 |
| **`mmsdk` 摘链接后出现 undefined symbol** | 中 | 链接期立刻暴露；`nm -u` 逐个查 |
| **912 处文档死链** | 中 | §5.4 分类处置；`check-wiki-refs.py` 必跑 |
| **`db_internal.h` 裁剪后实例结构体变小** | 中 | 每模态的 `db_<m>_open/close` 都要在 `#if` 两侧对称；P1 的验收要覆盖「裁掉的模态的公开 API 不存在于符号表」 |
| **P0~P2 与冻结轨退役并行导致分支混乱** | 中 | **次序**：先 W-B 的 P0（CI 兜底）→ 再 W-A（删除，趁 CI 已就位，能立刻验证主核没受影响）→ 再 W-B 的 P1/P2 → 最后文档大改 |
| **删除后无法回滚** | 低 | git 历史保留；删除前打 tag `pre-frozen-track-removal` |
| **`engine/include/db/` 下有 SDK 仍在用的头** | 低 | §2.8 已证明 SDK 只 include `algo-prod/distance/distance.h` 与 `roaring_bitmap.h`；B-6 的 include 路径调整后用编译验证 |
| **`TableOpener` 改造动执行器，可能引入回归** | 高 | 改造前先跑 `sh test/run.sh` 存基线；改造后**用例集合必须完全一致**（不新增、不减少）；S2 的验收 = 「执行器零模态名」门禁通过 **且** 全部既有用例仍绿 |
| **`SPI_AM_CTX_MAX = 256` 未来不够用** | 低 | 契约头里写死上限并在编译期校验（超限即编译错）；真需要时改这一个常量，代价可控 |
| **文档模态接 SQL 时执行器改动越界** | 中 | A7 审计清单**先定义后实现**：允许落点只有「值层路径变体」与「求值入口」两处；`grep -i json src/query/` 的每一处命中都要在清单里登记 |
| **`json_match` 的 NULL 语义（路径不存在 vs JSON null vs 标量不匹配）** | 中 | 映射规则全表用例（A1/A2）；反向验证注入 1 = 把 null 映成 INT 0，让 NULL 语义测试变红 |
| **八模态对称化与 v1.6 的执行器改动撞车** | 中 | **必须合并做**（C-4 依赖 C-1）；分两次动执行器 = 两次回归成本 + 两次审计 |

---

## 8. 不做项

1. **不做**多进程 / 多服务 / 微服务化（D8 留预备）。
2. **不做**插件化（`.so`）—— D2 已否决，理由见 §3。
3. **不做**跨模态事务（v1.7+ 议题，本次不动）。
4. **不做**把冻结轨的 percolator / raft / RAG 算子框架**迁移**到主核 —— 用户裁决是全删；triage 文档保留其分级结论供将来需要时取用。
5. **不重构**主核的**底座**目录结构与已有 SPI 契约（D3）—— §6 只动模态归属与新增第四契约，不碰 L0–L3 的形状。
6. **不改**主核的公开 API 语义 —— `db_json_*` 是**新增**，不是改动既有函数。
7. **不为对称而造空壳** —— 关系模态不新建 `db_rel.c`（它的对外接口就是 SQL），文档模态不新建 `doc_am.c`（寄生模态无虚拟表）。
8. **不做** JSON Schema 校验、多值索引、部分更新、JSON 聚合函数、MongoDB 查询语法 —— 沿用 `PLAN-v1.6` §1.1 的「不做表」，不重复决策。

---

## 9. 交付物与实施计划拆分

本设计包含三条工作流，实施计划拆成三份。**交叉点**：
W-A（删除 524 个文件）动的是整个仓库的构建面，必须先有 W-B 的 P0（主核 CI）兜底，
否则删除之后没有任何自动化手段能证明「主核没被碰坏」。

### 计划 W-A：冻结轨退役（§5，风险最高、依赖最多）

0. **前置**：W-B 的 P0（主核 CI）已合入 —— 有了红灯才敢删
1. 打 tag `pre-frozen-track-removal`
2. 采改造前 ctest 基线（失败用例名集合，跑两次取交集）
3. D-1 ~ D-9 删除
4. B-1 ~ B-8 构建与 SDK 修复
5. §5.4 文档处置
6. §5.5 验收

### 计划 W-B：主核工程组织（§4）

1. **P0**：主核 CI ← **W-A 的前置**
2. P1：`MM` 编译期开关 + 闭包校验
3. P2：每模态 smoke + `measure_build.sh`
4. 写 `docs/arch/db/multimodal-build-baseline.md`（实测基线）

### 计划 W-C：八模态对称化与功能补全（§6）

1. **C-1**：S1 + S2 —— SPI 第四契约 + 执行器一次性脱钩（**风险最高的一步，先做**）
2. **C-2**：S3 + S6 —— 关系模态适配器 + B+tree 适配器归位
3. **C-3**：S5 —— blob 搬进 `modalities/blob/`
4. **C-4**：PLAN-v1.6 步骤 3~6 —— 文档模态接 SQL（含 S7 / S8）
5. **C-5**：S9 门禁加强 + `MM` 扩到 8 个 profile + 8 个 smoke

**执行顺序**：

```
W-B 的 P0（CI 兜底）
  → W-A 全部（冻结轨退役）
  → W-C 的 C-1（执行器脱钩，此时 CI 已是绿的，能立刻发现回归）
  → W-C 的 C-2 / C-3 / C-4
  → W-B 的 P1 / P2（MM 开关与 smoke，此时 8 个模态已就位，profile 才是真的 8 个）
  → 文档大改（§5.4 与 §6 的 MODALITIES.md 补写，放在 W-A 末尾，与删除同批提交）
```

> **W-C 的 C-1 必须在 W-B 的 P1 之前**：P1 的 `MM` 开关要靠 `#if DB_HAS_<MOD>` 裁剪 `db_internal.h` 与 `db.c`；
> 而 C-2 正在改这两个文件的结构。先脱钩、后裁剪，避免同一批文件被两条工作流同时改。

---

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

## 附录 C：`SPI_AM_CTX_MAX = 256` 的取值依据

`HeapAmCtx`（`spi/heap_am.h:36-50`）的字段与估算尺寸（64 位平台）：

| 字段 | 类型 | 字节 |
|---|---|---|
| `heap` | `Heap *` | 8 |
| `cols` | `const ColDesc *` | 8 |
| `ncols` | `int` | 4（+4 对齐填充） |
| `idxs` | `const IndexInfo *` | 8 |
| `nidx` | `int` | 4（+4 对齐填充） |
| `layout` | `RecLayout` | 142 → 144（对齐后） |

**合计 ≈ 184 字节** → `SPI_AM_CTX_MAX = 256` 留 72 字节余量（可容纳一个 `Graph*` 或两个 `u32` 级别的附加状态）。

`RecLayout` 的构成（`storage/record.h:113-125`）：7 个 `u16`（14 字节）+ 2 个 `u16[REC_MAX_COLS]`。
`REC_MAX_COLS` = 32 → 2 × 32 × 2 = 128 字节 → 合计 142 字节。

> 该常量必须写在 `spi/table_opener.h` 里，并在编译期校验适配器的 ctx 不超过它（超限即编译错），
> 而不是等到运行时才发现栈溢出。
