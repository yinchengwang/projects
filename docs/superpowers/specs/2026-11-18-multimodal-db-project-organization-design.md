# 多模态数据库工程组织设计：主核形态 + 冻结轨退役

> 状态：设计稿，待用户批准
> 日期：2026-11-18
> 范围：`projects` 仓 —— 主核 `engine/db/` 的工程组织形态，以及冻结轨 `engine/src/db/` 的退役
> 所有「现状」数字均为 **2026-11-18 实测**，核实命令见附录 A

---

## 0. 一句话

**主核保持「一个工程 + 编译期模态裁剪」（不加仓、不加进程、不加插件），冻结轨 `engine/src/db/` 全删（173,719 行），SDK 去冻结化后保留。**

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

第 3 点引出本设计的主体：**多模态数据库应该以什么工程形态存在**。

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

**单一入口**：`make MM=vector` / `MM=ft` / `MM=geo` / `MM=ts` / `MM=graph` / `MM=full`（默认）。

- profile 定义写在**一个**文件 `engine/db/tools/mm_profiles.mk`（唯一真相源），
  每个 profile 显式列出它打开的模态集合，并声明每个模态的**闭包依赖**（当前除底座外无跨模态依赖）。
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

`make MM=full` 时产出全部 7 个 smoke：`smoke_base`（关系模态 + blob，即内核基座）、`smoke_vector`、
`smoke_ft`、`smoke_geo`、`smoke_ts`、`smoke_graph`、`smoke_doc`。

> `smoke_doc` 与其他六个不同：文档模态**尚未接入 SQL**（`engine/db/README.md` 顶部状态校正），
> 它的 smoke 只验证 JSONB 编解码 / 路径求值 / 溢出链，**不验证 SQL 路径**。这一点必须在 smoke 的注释里写明，
> 否则会让人误以为文档模态已可从 SQL 访问。

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
> **W-A 必须在 P0 之后执行**（§8）。

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

## 6. 风险

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

---

## 7. 不做项

1. **不做**多进程 / 多服务 / 微服务化（D8 留预备）。
2. **不做**插件化（`.so`）—— D2 已否决，理由见 §3。
3. **不做**跨模态事务（v1.7+ 议题，本次不动）。
4. **不做**把冻结轨的 percolator / raft / RAG 算子框架**迁移**到主核 —— 用户裁决是全删；triage 文档保留其分级结论供将来需要时取用。
5. **不重构**主核的目录结构与 SPI 契约（D3）。
6. **不改**主核的公开 API 语义。

---

## 8. 交付物与实施计划拆分

本设计包含两条**相互独立**的工作流，实施计划拆成两份。**两者有一个交叉点**：
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

**执行顺序**：W-B 的 P0 → W-A 全部 → W-B 的 P1/P2 → 文档大改（§5.4 放在 W-A 末尾，与删除同批提交）。

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
| `engine/db/docs/MODALITIES.md` | 八模态实现方案（§2.5 跨模态事务语义的出处） |
| `engine/db/README.md` | 主核七条硬规则（§4.3 的边界来源） |
