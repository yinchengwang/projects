# projects — C/C++ 算法工程实践

> code 四仓体系中的**工程仓**：`engine/` 产出生产级作品。
> **学习轨（learning）与源码笔记轨（reference）由 code 顶层聚合仓中的两个直接 submodule 提供**，本仓不承载学习笔记与第三方源码镜像。

[![CI](https://img.shields.io/badge/CI-github%20actions-blue)](.github/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Docs](https://img.shields.io/badge/docs-285-brightgreen)](docs/)

---

## 🎯 项目定位

本仓是 **C/C++ 工程能力** 的系统性实践，覆盖：

| 领域 | 说明 | 代码位置 |
|------|------|----------|
| **数据库内核** | 存储引擎、SQL 解析/执行、事务与 MVCC、Catalog | `engine/db/`（唯一内核） |
| **向量数据库** | HNSW 向量索引与 KNN 检索（HNSW 之外还有精确 KNN 对照） | `engine/db/src/modalities/vector/` |
| **嵌入式 SDK** | 只依赖 SQLite 的多模态 SDK：内存上下文、读写锁、精确扁平向量索引、全文、图、时序、跨模态检索 | `engine/src/sdk/` |
| **分布式系统** | 分片路由、Raft 共识、2PC 分布式事务（SDK 侧，真实可用但未做集群验证） | `engine/src/sdk/core/{sharding,replication,txn}.c` |
| **算法工程** | 生产级排序、K-Means、量化压缩 | `engine/src/algo-prod/` |

---

## 🏗️ 核心能力

> **阅读提示**：本仓曾经有两套数据库内核。并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）
> 已于 **2026-11-18 退役** —— 源码、公开头、测试、构建接线全部删除，
> 那 27 个 ANN 索引实现、15 个传统索引、RAG 执行器与分布式模块随之一起消失。
> 历史分析见 `docs/analysis/archive/`。
> **现在唯一长期维护的内核是下节的 `engine/db/`，外加一条独立的嵌入式 SDK（`engine/src/sdk/`）。**

### 1. 嵌入式 SDK（`engine/src/sdk/`）

**只依赖 SQLite 一个外部库**的多模态 SDK：

- **内存上下文**：AllocSet 分配器（`palloc` / `pfree` / reset / delete），父子层级、线程归属校验、限额
- **向量**：精确扁平索引（召回率 100%，L2 平方距离），metadata filter 谓词下推
- **全文**：FTS5 + BM25，混合检索与 RRF 多通道融合
- **图 / 时序 / 文本 / 跨模态**：各模态 API + SQL 表接入
- **工程能力**：ACID 事务、备份/恢复、Raft 复制、一致性哈希分片、命名空间配额、运行时指标

> SDK 的内存上下文、读写锁、向量索引、SQL 适配层都是**自持**的 —— 它不链接主核，
> 也不依赖任何已退役的代码。

### 2. 多模态数据库主内核（`engine/db/`）

本仓的**唯一数据库内核**，一个**从零手写**的多模态数据库内核。它用 Makefile 构建、
**不入 CMake 图**（有意的例外：自带 L0–L4 分层门禁、`-Werror` 编译门禁与独立测试协议）。

**主核门禁**（改完必跑，`-Werror` 零警告是硬要求）：

```bash
export PATH="/c/mingw64/bin:$PATH"
cd engine/db
mingw32-make clean && mingw32-make all   # 严格编译
sh tools/check_deps.sh                   # L0–L4 依赖方向
sh test/run.sh                           # unit + integration
mingw32-make check                       # 全套
```

| 项 | 值 | 核实方式 |
|---|-----|----------|
| C 源码规模 | **389 个 `.c`/`.h`，121,738 行**（`src/` + `bench/` + `test/`，不含 `build/`） | `Get-ChildItem -Recurse -Include '*.c','*.h' engine/db/src,engine/db/bench,engine/db/test` |
| 测试 | **105 个测试**（unit 69 + integration 36），**全绿 / 0 FAIL** | `sh test/run.sh` |
| 依赖方向门禁 | `sh tools/check_deps.sh` → `✓ 依赖方向正确`（S9① 已阻断 query→模态，S9② 待 C-2/C-3 落地后启用） | 该脚本 |
| 计划文档 | `engine/db/docs/` 下 **22 篇**（`PLAN-v0.1` ~ `PLAN-v1.6` + `DESIGN.md` + `MODALITIES.md` + `MM-PROFILES.md`） | `Get-ChildItem engine/db/docs` |

**强制分层（L0 → L4，`tools/check_deps.sh` 门禁）**：

```
L0 common/     不依赖任何层
L1 storage/ wal/    只依赖 common
L2 index/ txn/ catalog/   禁依赖 query / spi / modalities
L3 query/ spi/    禁依赖 modalities
L4 modalities/   模态之间禁互相 include（graph/ 与 vector/ 各自独立）
```

**模态进度**：关系（v0.x）+ Blob（v0.9）+ 图（v0.11）+ 向量（v1.0）已落地；
v1.1（向量化执行 + 规则 CBO）已完成；**v1.2（内核收尾：copy-out → 细粒度 latch → 页内 compact → checkpoint）七个步骤全部完成**；
⚠️ **v1.2 唯一的未达标项是 A3（吞吐）**：细粒度 latch vs 大锁基线实测 **0.78× – 1.01×**（判据 ≥ 2×），
瓶颈已定位为缓冲池的池级互斥量 `pg->mu` —— 详见 `engine/db/docs/PLAN-v1.2.md` §3 步骤 7；
v1.3 ~ v1.6 依次为全文检索、地理空间、时序、文档/JSON 四模态（计划已生成，未执行）。
另有 `mingw32-make bench` 跑 vec_bench / graph_bench / hnswlib_ref 对照基准。

---

## 📁 项目结构

```
projects/
├── engine/                # 🎯 工程轨（生产级代码）
│   ├── src/
│   │   ├── sdk/           # 嵌入式 SDK（core / vectors / graph / text / timeseries
│   │   │                  #   / extra / aggregation），只依赖 SQLite
│   │   ├── algo-prod/     # 生产算法库（sort / Kmeans / quantization / distance）
│   │   ├── cpp/           # C++ 基础实现（trie）
│   │   ├── ds/            # 数据结构
│   │   └── redis/         # Redis 核心移植（sds / adlist / skiplist / zmalloc）
│   ├── db/                # 🎯 唯一数据库内核（Makefile 构建，L0–L4 分层门禁，不入 CMake 图）
│   ├── include/           # 公共头文件（algo-prod/ ds/ sdk/ ...）
│   ├── test/              # gtest 测试（5 个子目录，56 个 .c/.cpp 测试源文件）
│   ├── sdk/               # 多语言 SDK 绑定（go / python / tests）
│   ├── tools/             # benchmark（Python）+ mermaid2excalidraw
│   ├── docs/              # 引擎内文档（44 篇）
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
│   ├── analysis/archive/  #   已归档的差距分析（27 篇，分析对象已退役）
│   ├── process/           #   流程与实施计划（106 篇）
│   ├── diagrams/          #   分层图（46 篇）
│   └── superpowers/       #   设计与规格 / 计划（16 篇）
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

主核的交互入口是 `engine/db/src/main.c` 的 REPL（`mingw32-make` 产物，见 `engine/db/README.md`）。

> 原先并列的 `engine/src/db/cli/`（`db_cli`）与其向量 API 已随冻结轨于 **2026-11-18 删除**，
> 相关设计文档的「已退役」说明见 `docs/arch/vdb-cli/README.md`。

---

## 🧪 测试覆盖

| 轨道 | 数量 | 核实方式 |
|------|------|----------|
| 工程轨测试源文件 | **56** | `engine/test/` 下的 `.c` / `.cpp`（algo 13、sdk 36、kbase 7，含 ds 与 self_made_cpp 子目录） |
| 工程轨 ctest 用例 | **76** | `ctest --test-dir build/engine -N` 报告的用例总数（gtest 用例逐条展开） |
| 独立内核测试 | **105** | `engine/db/test/`（unit 69 + integration 36），由 `sh test/run.sh` 驱动，**105 通过 / 0 失败** |

> 学习轨的测试与题解**已随学习轨迁出**，统计在 `../learning/` 仓，本仓不再重复计数。
> `build/engine` 是本地构建产物目录（未纳入版本控制），上面的 76 来自该目录最近一次配置；
> 重新配置后以 `ctest -N` 的实时输出为准。
> 冻结轨退役后用例数从 835 降到 76 —— 少掉的绝大部分是 `engine/test/db/`（289 个源文件）与被删的
> `smoke/`、`index/` 目录，**不是覆盖丢失**：它们测的内核已经不存在了。

---

## 📚 文档导航

| 文档 | 内容 |
|------|------|
| [engine/README.md](engine/README.md) | 工程轨入口：构建、测试、canonical 内容清单 |
| [engine/docs/](engine/docs/) | 引擎内文档：架构、B+Tree、MVCC、WAL、HNSW 深潜等（22 篇 + 3 个子目录） |
| [engine/docs/index/](engine/docs/index/) | 各索引实现的设计文档（art / btree / diskann / gin / hnsw / ivf_pq / ...） |
| [docs/](docs/) | 项目级文档总入口（按 arch / storage / analysis / process / diagrams 分区） |
| [docs/arch/](docs/arch/) | 架构总览、索引与 SQL 执行器设计、RAG 与 modular-rag 规范、理论推导（**多数对应已退役的旧内核，详见各篇顶部说明**） |
| [docs/storage/](docs/storage/) | 存储设计：向量 / 文档 / 图 / 空间 / 时序 / CCEH |
| [docs/diagrams/](docs/diagrams/) | L0–L6 分层图（storage / sql / rag 等） |
| [docs/analysis/archive/](docs/analysis/archive/) | **已归档**的差距分析与研究报告（分析对象已退役，仅作历史参考） |
| [openspec/specs/](openspec/specs/) | 规格文档（23 个 spec 目录） |
| [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md) | 项目级 agent 规约 |

---

## 🚧 踩坑红线

> ### ⛔ 禁止 `git submodule update --init --recursive`（作用域是 `reference` 仓）
>
> **在 `reference` 仓内**（本仓相对路径 `../reference/`，即 `C:\code\reference`）
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

> ### ⛔ `engine/db/` 的分层门禁与构建方式（勿用 CMake 构建，勿跨层 include）
>
> `engine/db/` 是独立的多模态内核，**不在 CMake 构建图内**（根 `CMakeLists.txt` 只 `add_subdirectory(engine)`，
> 而 `engine/CMakeLists.txt` 只引入 `src/` `tools/` `test/`）。
> 它用自带 `Makefile` 构建，Windows 下必须 `SHELL := sh`（Git Bash），
> 依赖 `find`/`rm`，且编译门禁是 `-Wall -Wextra -Werror`（**零警告**，比工程轨更严）。
>
> 分层由 `engine/db/tools/check_deps.sh` 强制：
> **L0 `common/`** 不依赖任何层 → **L1 `storage/` `wal/`** 只依赖 common →
> **L2 `index/` `txn/` `catalog/`** 禁依赖 query/spi/modalities →
> **L3 `query/` `spi/`** 禁依赖 modalities → **L4 `modalities/`** 模态之间禁互相 include。
> 改动后必须跑 `sh tools/check_deps.sh`（期望输出 `✓ 依赖方向正确`）与 `sh test/run.sh`（期望 0 FAIL）。
>
> ⚠️ 向量模态的适配器住在 `modalities/vector/vector_am.{h,c}` 而**不是** `spi/vector_am.{h,c}` ——
> 因为 `spi/`（L3）不许依赖 `modalities/`（L4）。`engine/db/docs/MODALITIES.md` 中仍有旧路径表述，修复中。
>
> ⚠️ 向量页页头是 **20B**，不是 8B：8B 会静默压坏 `H_LSN`(12) 与 `H_CKSUM`(16)，只坏每页第一个向量且不报错。
> `engine/db/docs/PLAN-v1.0.md` 与 `MODALITIES.md` 的存储布局小节仍有 8B 表述，修复中。

---

## 📦 什么该放这个仓

本仓是四个仓中的**工程仓**（projects）。判断一份东西该放这里、还是放 `learning` / `ai-docs` / `reference`：

### 四仓关系速查

| 仓 | 获取方式 | 路径 | 角色 |
|---|---|---|---|
| code | 父仓 | `../` | 只放四仓关系，不放业务代码 |
| projects | submodule | `./`（本仓） | 工程仓（引擎 + 小程序 + 项目级文档 + agent 规约） |
| learning | submodule | [`../learning/`](../learning/) | 学习仓（笔记 + 脚手架 + 题解 + 进度） |
| ai-docs | submodule | [`../ai-docs/`](../ai-docs/) | AI 工作文档仓（模板与方法论） |
| reference | submodule + `update=none` | [`../reference/`](../reference/) | 第三方源码镜像 + 源码笔记（内部上游按需拉取） |

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
| 56 个测试源文件 | `git ls-files 'engine/test/**'` 后按 `.c` / `.cpp` 计数 |
| 76 个 ctest 用例 | `ctest --test-dir build/engine -N` |
| 285 篇 `docs/` 文档（63+12+27+106+46+16+15） | `git ls-files 'docs/**'` 后按扩展名计数 |
| 23 个 spec 目录 / 8 个进行中变更 / 3 个已归档变更 | `Get-ChildItem openspec/{specs,changes,archived}` |
| 4 个小程序 | `Get-ChildItem apps -Directory` |
| 38 个 skill | `skills-lock.json` 的 `skills` 键计数 |
| 95 个第三方 submodule（reference 仓） | `Select-String reference/.gitmodules '^\[submodule '` |
| 独立内核 389 个 `.c`/`.h`、121,738 行 | `Get-ChildItem -Recurse -Include '*.c','*.h' engine/db/src,engine/db/bench,engine/db/test` |
| 独立内核 105 个测试（unit 69 + integration 36） | `sh test/run.sh` |
| 独立内核 22 篇计划与设计文档 | `Get-ChildItem engine/db/docs -Filter '*.md'` |

---

## 📄 许可

MIT License - 详见 [LICENSE](LICENSE)
