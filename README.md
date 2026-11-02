# projects — C/C++ 算法工程实践

> StillEvolving 四仓体系中的**工程仓**：`engine/` 产出生产级作品。
> **学习轨（learning）与参考轨（reference）已切出为独立仓**，本仓不再承载学习笔记与第三方源码镜像。

[![CI](https://img.shields.io/badge/CI-github%20actions-blue)](.github/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Docs](https://img.shields.io/badge/docs-271-brightgreen)](docs/)

---

## 🎯 项目定位

本仓是 **C/C++ 工程能力** 的系统性实践，覆盖：

| 领域 | 说明 | 代码位置 |
|------|------|----------|
| **数据库内核** | 存储引擎、SQL 解析/执行、事务与 MVCC、Catalog | `engine/src/db/` |
| **向量数据库** | 27 个 ANN 索引实现 + 15 个传统索引实现 | `engine/src/db/index/` |
| **RAG 系统** | 检索增强生成、混合检索、Graph RAG、多模态分块 | `engine/src/db/executor/rag/` |
| **分布式系统** | 分片路由、Raft 共识、2PC 分布式事务 | `engine/src/db/{sharding,consensus,distributed}/` |
| **算法工程** | 生产级排序、K-Means、量化压缩 | `engine/src/algo-prod/` |

---

## 🏗️ 核心能力

### 1. 存储引擎（PostgreSQL 风格）

- **Buffer Pool**：Clock-Sweep 置换、脏页管理
- **WAL 日志**：写前日志、Redo 恢复、检查点
- **Access Method**：Heap 堆表、B+Tree / Hash / ART / Radix Tree / R-Tree 等索引
- **Catalog 系统**：表 / 列 / 索引元数据、OID 分配
- **多模态存储**：向量、文档、图、空间、时序、稀疏、列式、K-V、Blob 等并存

### 2. 向量索引（27 个 ANN 实现）

`engine/src/db/index/vector_index/` 下共 **34 个子模块**，其中 **27 个是独立的 ANN 索引算法实现**，
另有 7 个支撑模块（BM25 / delete / gpu / multimodal / persist / reranker / streaming）
与 3 个顶层编排文件（`hybrid_search.c` / `tiered_index.c` / `vector_index_selector.c`）。

| 类型 | 实现（均在 `vector_index/` 下） |
|------|------|
| 图索引 | HNSW 变体（`hnsw_pq` / `hnsw_sq` / `dist_hnsw`）、`nsw`、`ssg`、`scann`、`faiss_hnsw` |
| 倒排索引 | `ivf`、`ivf_flat`、`ivf_pq`、`ivf_hnsw`、`milvus_ivf`、`faiss_ivf_compat` |
| 树索引 | `kd_tree`、`ball_tree` |
| 哈希索引 | `lsh`、`lsh_multiprobe`、`itq`、`spectral_hash` |
| 量化 | `pq`、`sq`、`opq`、`rq`、`lvq`、`bq` |
| 磁盘索引 | `diskann`（Vamana + PQ，23 个源文件） |
| 其他 | `annoy` |

另有 **15 个传统索引实现**（同在 `engine/src/db/index/` 下）：
`art`、`bitmap`、`bplus_tree`、`brin`、`btree`、`fulltext`、`gin`、`gist`、`hash`、`heap`、
`hilbert`、`radix_tree`、`rtree`、`skip_list`、`ttree`。

### 3. RAG 检索增强生成

- **混合检索**：向量检索 + BM25 + RRF 融合（`engine/src/db/index/vector_index/hybrid_search.c`）
- **Graph RAG**：知识图谱 + 实体/关系抽取 + 子图检索（`engine/src/db/executor/rag/graphrag_*.c`）
- **多模态支持**：Markdown、PDF、代码、图片（`engine/include/rag/multimodal_chunk.h`）
- **智能分块**：递归字符、语义、代码感知分块（`engine/src/db/executor/rag/chunk_op.c`）
- **RAG 评估**：RAGAs 指标体系
- **向量 API / CLI**：API 见 `engine/src/db/api/vdb_api.c`，命令行用法见 `docs/arch/vdb-cli/README.md`

### 4. 分布式架构

- **分片策略**：Hash 分片、Range 分片、一致性 Hash（`engine/src/db/distributed/shard/`）
- **共识算法**：Raft Leader 选举、日志复制（`engine/src/db/consensus/raft*.c`）
- **分布式事务**：2PC 两阶段提交（`engine/src/db/distributed/txn/`）、Percolator（`engine/src/db/distributed/percolator/`）
- **高可用**：副本复制（`engine/src/db/replication/`）、全局时间戳（`engine/src/db/distributed/tso/`）

---

## 📁 项目结构

```
projects/
├── engine/                # 🎯 工程轨（生产级代码）
│   ├── src/
│   │   ├── db/            # 数据库内核（37 个子模块）
│   │   │   ├── index/     #   索引子系统：15 个传统索引 + vector_index/
│   │   │   ├── executor/  #   SQL 执行器（含 rag/ 子目录）
│   │   │   ├── storage/   #   存储层（24 个子模块：buffer/catalog/wal/vector/...）
│   │   │   ├── consensus/ #   Raft 共识
│   │   │   ├── distributed/#  分片 / 2PC / Percolator / TSO
│   │   │   ├── optimizer/ parser/ sql/ txn/ sharding/ replication/ graph/ ...
│   │   ├── algo-prod/    # 生产算法库（sort / Kmeans / quantization / heap ...）
│   │   ├── cpp/          # C++ 基础实现（trie）
│   │   ├── ds/           # 数据结构
│   │   ├── redis/        # Redis 核心移植（sds / adlist / skiplist / zmalloc）
│   │   └── sdk/          # 对外 SDK（aggregation / vectors / text / graph / ...）
│   ├── include/          # 公共头文件
│   ├── test/             # gtest 测试（8 个子目录，367 个 .c/.cpp 测试源文件）
│   ├── sdk/              # 多语言 SDK（go / python / tests）
│   ├── tools/            # benchmark（Python）+ mermaid2excalidraw
│   ├── docs/             # 引擎内文档（22 篇 + arch/ index/ superpowers/）
│   ├── cmake/  drivers/  proto/  scripts/  test_data/  third_part/
│   ├── Dockerfile  docker-compose.yml
│   └── CMakeLists.txt
│
├── apps/                  # 📱 微信小程序（4 个，零依赖零构建的原生小程序）
│   ├── knowledge-hub/         # Reading Radar
│   ├── programmer-workbench/  # DevBench
│   ├── games/                 # 游戏中心
│   ├── trip-log/
│   └── .archive/
│
├── docs/                  # 📄 项目级文档（271 篇 .md）
│   ├── arch/              #   架构设计（63 篇，含 modular-rag/ theory/ implementation/）
│   ├── storage/           #   存储设计（12 篇）
│   ├── analysis/          #   差距分析（27 篇）
│   ├── process/           #   流程与实施计划（106 篇）
│   ├── diagrams/          #   分层图（46 篇）
│   └── superpowers/       #   设计与规格（2 篇）
│
├── openspec/              # 📋 变更与规格
│   ├── specs/             #   23 个 spec 目录 + 1 篇根级 spec（learning/ 已迁出）
│   ├── changes/           #   8 个进行中变更
│   ├── archived/          #   3 个已归档变更
│   └── config.yaml
│
├── vendor/                # 📦 第三方依赖与共享 CMake
│   ├── third_part/        #   googletest / cmocka / sqlite3 / croaring
│   └── cmake/
│
├── dataset/               # 🗄️ 项目专属数据集（SIFT 等，大文件走 Git LFS）
├── scripts/               # 🛠️ 引用校验门禁与迁移脚本
├── archive/               # 📦 历史归档（37 个目录）
├── .ai/                   # 🤖 agent 规约（.agents / .claude / .opencode / .vscode）
├── .github/workflows/     # ci.yml + asan.yml
├── AGENTS.md  CLAUDE.md  skills-lock.json（38 个 skill）
└── CMakeLists.txt  LICENSE  README.md
```

---

## 🚀 快速开始

### 构建工程轨

```bash
# 构建只依赖这两个 submodule —— 单独浅拉取，不要加 --recursive
git submodule update --init --depth 1 \
    vendor/third_part/googletest engine/third_part/uthash

# 配置 & 构建
cmake -B build/engine -S engine -DBUILD_TESTING=ON
cmake --build build/engine --parallel 4

# 测试
ctest --test-dir build/engine --output-on-failure
ctest --test-dir build/engine -R <name> --output-on-failure   # 单个测试
```

> 上述命令与 [`.github/workflows/ci.yml`](.github/workflows/ci.yml) 一致，CI 在 Ubuntu 与
> Windows/MinGW 两套工具链上各跑一遍；根入口为 `cmake -B build/root -S . -DENGINEERING_BUILD=ON`。

### 使用 CLI

`engine/src/db/cli/` 编译出 `db_cli` 可执行程序（canonical SQL 路径）；
向量数据库对外 API 为 `engine/src/db/api/vdb_api.c`，用法见 `docs/arch/vdb-cli/README.md`。

---

## 🧪 测试覆盖

| 轨道 | 数量 | 核实方式 |
|------|------|----------|
| 工程轨测试源文件 | **367** | `git ls-files 'engine/test/**'` 下的 `.c` / `.cpp`（db 289、sdk 36、algo 13、smoke 13、index 8、kbase 7、self_made_cpp 1） |
| 工程轨 ctest 用例 | **835** | 已配置构建目录 `engine/build/base` 下 `ctest -N` 报告的用例总数（gtest 用例逐条展开） |
| RAG 相关测试源文件 | **4** | `engine/test/db/rag`（1）+ `engine/test/sdk/rag`（3） |

> 学习轨的测试与题解**已随学习轨迁出**，统计在 `../learning/` 仓，本仓不再重复计数。
> `engine/build/` 是本地构建产物目录（未纳入版本控制），上面的 835 来自该目录中最近一次配置；
> 重新配置后以 `ctest -N` 的实时输出为准。

---

## 📚 文档导航

| 文档 | 内容 |
|------|------|
| [engine/README.md](engine/README.md) | 工程轨入口：构建、测试、canonical 内容清单 |
| [engine/docs/](engine/docs/) | 引擎内文档：架构、B+Tree、MVCC、WAL、HNSW 深潜等（22 篇 + 3 个子目录） |
| [engine/docs/index/](engine/docs/index/) | 各索引实现的设计文档（art / btree / diskann / gin / hnsw / ivf_pq / ...） |
| [docs/](docs/) | 项目级文档总入口（按 arch / storage / analysis / process / diagrams 分区） |
| [docs/arch/](docs/arch/) | 架构总览、索引与 SQL 执行器设计、RAG 与 modular-rag 规范、理论推导 |
| [docs/storage/](docs/storage/) | 存储设计：向量 / 文档 / 图 / 空间 / 时序 / CCEH |
| [docs/diagrams/](docs/diagrams/) | L0–L6 分层图（storage / sql / rag 等） |
| [docs/analysis/](docs/analysis/) | 差距分析与研究报告 |
| [openspec/specs/](openspec/specs/) | 规格文档（23 个 spec 目录） |
| [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md) | 项目级 agent 规约 |

---

## 🚧 踩坑红线

> ### ⛔ 禁止 `git submodule update --init --recursive`（作用域是 `reference` 仓）
>
> **在 `reference` 仓内**（本仓相对路径 `../../reference/`，即 `C:\code\reference`）
> 执行 `git submodule update --init --recursive` 会一次性拉起其 **95 个第三方库 submodule**，
> 体积约 **25 GB / 53.6 万文件**，极易把磁盘与 CI 时间拖垮。
> 需要哪个库就用 `scripts/fetch-reference.sh --name <名字>` 按 catalog 单独拉取。
>
> **在本仓内递归初始化是安全的**：本仓 `.gitmodules` 只有 2 条
> （`vendor/third_part/googletest`、`engine/third_part/uthash`），都不含 reference。
> 但 CI 仍显式使用 `--depth 1` 浅拉取，避免不必要的下载。

> ### ⛔ 引用校验门禁：改文档必跑 `scripts/check-wiki-refs.py`
>
> 本仓 `scripts/check-wiki-refs.py` 与 reference 仓同名脚本**内容完全一致**（SHA256 相同，属已知重复副本）。
> 它是「引用正确性」的回归门禁：校验文档中 `路径:行号` 形式的第三方源码引用
> —— 文件是否存在、行号是否越界、目标行是否为空。
>
> 改动任何带 `路径:行号` 引用的文档后必须重跑，**不要手改行号绕过校验**。
> 子模块未初始化时脚本走模式 B（只校验零精确行号）。

> ### ⛔ 学习轨已切出为独立仓
>
> 学习笔记、脚手架、题解、进度**都在 `../learning/`**（`yinchengwang/learning`）；
> 学习轨的 spec（`openspec/specs/learning/`）已于 **2026-10-26** 迁出
> （本仓 `openspec/specs/` 下已无 `learning/` 目录）。
> 本仓不再新增学习轨内容，CI 也不再构建学习轨道。

---

## 📦 什么该放这个仓

本仓是四个仓中的**工程仓**（projects）。判断一份东西该放这里、还是放 `learning` / `ai-docs` / `reference`：

### 四仓关系速查

| 仓 | 获取方式 | 路径 | 角色 |
|---|---|---|---|
| StillEvolving | 父仓 | `../` | 只放四仓关系，不放业务代码 |
| projects | submodule | `./`（本仓） | 工程仓（引擎 + 小程序 + 项目级文档 + agent 规约） |
| learning | submodule | [`../learning/`](../learning/) | 学习仓（笔记 + 脚手架 + 题解 + 进度） |
| ai-docs | submodule | [`../ai-docs/`](../ai-docs/) | AI 工作文档仓（模板与方法论） |
| reference | 独立仓 + 清单拉取 | [`../../reference/`](../../reference/) | 第三方源码镜像 + 源码笔记（**不进 submodule 树**） |

### 归属判定表

| 内容 | 归属 |
|---|---|
| 可运行的工程代码、测试、构建脚本 | **projects**（本仓） |
| 项目级架构文档、流程文档 | **projects**（本仓） |
| **项目级** agent 规约（`.ai/`、`CLAUDE.md`、`AGENTS.md`、`openspec/`、`skills-lock.json`） | **projects**（本仓） |
| 项目专属数据（`dataset/`） | **projects**（本仓） |
| 第三方源码镜像 | reference |
| **跨项目**通用笔记、模板、方法论文档 | ai-docs |
| 学习笔记、课程进度、算法题解 | learning |
| 学习轨的 spec（原 `openspec/specs/learning/`） | learning（**已迁出** 2026-10-26） |

**判定口诀**：换个项目还用得上 → ai-docs；只对这个项目有意义 → 留在本仓。

> ⚠️ `.ai/`、`CLAUDE.md`、`AGENTS.md` 是项目级 agent 规约，**禁止搬走** —— 搬走会导致 agent 读不到规约。
>
> 完整目录规范见父仓 [`../docs/architecture.md`](../docs/architecture.md)；
> 机器可读版见父仓 [`../repos.yaml`](../repos.yaml) 的 `must_contain` 字段；
> 父仓入口见 [`../README.md`](../README.md)。

---

## 🔢 本文数字的核实方式

本文所有计数均来自本仓文件系统的实测，非估计值：

| 数字 | 核实命令 |
|---|---|
| 27 个 ANN 实现 / 34 个 vector_index 子模块 | `Get-ChildItem engine/src/db/index/vector_index` |
| 15 个传统索引实现 | `Get-ChildItem engine/src/db/index -Directory` |
| 37 个 db 子模块 / 24 个 storage 子模块 | `Get-ChildItem engine/src/db`、`engine/src/db/storage` |
| 367 个测试源文件 | `git ls-files 'engine/test/**'` 后按 `.c` / `.cpp` 计数 |
| 835 个 ctest 用例 | `ctest --test-dir engine/build/base -N` |
| 271 篇 `docs/` 文档（63+12+27+106+46+2+15） | `git ls-files 'docs/**'` 后按扩展名计数 |
| 23 个 spec 目录 / 8 个进行中变更 / 3 个已归档变更 | `Get-ChildItem openspec/{specs,changes,archived}` |
| 4 个小程序 | `Get-ChildItem apps -Directory` |
| 38 个 skill | `skills-lock.json` 的 `skills` 键计数 |
| 95 个第三方 submodule（reference 仓） | `Select-String reference/.gitmodules '^\[submodule '` |

---

## 📄 许可

MIT License - 详见 [LICENSE](LICENSE)
