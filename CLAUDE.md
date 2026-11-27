# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目语言规范

请严格遵守以下规则：
1. 所有对话、解释、建议必须使用**简体中文**。
2. 代码注释必须使用中文。
3. 生成的 Commit Message 必须使用中文。
4. 严禁出现大段未翻译的英文技术名词（保留专业术语如 API、SDK 除外）。

## 任务交付铁律

**每次任务执行完成后，必须给用户一个清晰的总结。**

总结必须包含以下要素：
1. **完成了什么** — 本次任务产出的具体内容（文件、修改、功能）
2. **变更范围** — 涉及哪些文件/模块的增删改
3. **验证方式** — 如何确认改动正确（编译、测试、运行等）
4. **后续建议** — 如有未竟事宜或可优化点，一并指出

总结应在任务结束时输出，避免仅返回"完成了"而无细节。

## OpenSpec 变更管理铁律

**每次提出新需求后必须更新 tasks.md，同时 tasks.md 更新后，其余相关变更文档也要同步适配。**

1. **tasks.md 是任务源**：所有变更任务必须记录在 tasks.md 中
2. **任务更新触发联动**：当 tasks.md 中的任务列表发生变化时，以下文档必须同步检查并适配：
   - `proposal.md` - 检查 What Changes 部分是否需要补充
   - `design.md` - 检查设计决策是否需要调整
   - `specs/**/*.md` - 检查规格定义是否需要扩展
3. **任务完成必须更新状态**：每完成一个任务后，必须将 tasks.md 中对应任务的状态从 `- [ ]` 更新为 `- [x]`

**检查清单**：修改 tasks.md 后，逐一确认以下文档是否需要更新：
- [ ] proposal.md - 变更范围是否覆盖新任务？
- [ ] design.md - 设计决策是否需要补充？
- [ ] specs/ - 是否有新的能力规格需要定义？
- [ ] 其他相关文档

### OPSX 变更操作纪律

1. **回退纪律**：OPSX 变更修改出现问题需要回退时，只回退当前变更修改的代码，不要去动别的代码
2. **提交纪律**：每次 OPSX 变更里提交代码时，只提交变更相关的代码，不相关的不要混在一起提交
3. **归档纪律**：变更归档时，只提交变更相关的代码，不相关的不要混在一起提交

## 提交推送纪律

1. **目标分支**：所有代码统一推送到 GitHub 远程仓库（`origin`）的 `main` 分支。
2. **即时推送**：每次 commit 完成后，必须立即执行 `git push`，不得累积多个 commit 后批量推送。
3. **远程仓库**：仅使用 `origin`（GitHub: `https://github.com/yinchengwang/projects.git`），不再使用 Gitee。

## 项目概述

C/C++ 算法与数据结构练习项目。CMake 3.20+、C11、C++17。无运行时依赖。

[AGENTS.md](AGENTS.md) 涵盖了构建命令、测试结构、库列表、代码风格和编译器标志，请先阅读。

## 项目布局

仓库采用单工程轨架构：`engine/` 为唯一的工程轨道。学习轨（`learning/`）与参考轨（`reference/`）已切出为独立仓，根目录仅保留共享基础设施与必要配置文件。

```
engine/         # 工程轨道（默认构建）
├── src/             # 库代码（algo-prod/cpp/ds/redis/sdk）
├── db/              # 独立多模态数据库内核（自带 Makefile，不入 CMake 图，见下）
├── include/         # 公共头文件
├── test/            # gtest 测试
├── sdk/             # 多语言 SDK
├── tools/           # 工程专用工具
├── cmake/           # 工程 CMake 工具
├── test_data/       # 测试夹具
├── Dockerfile / docker-compose.yml
└── CMakeLists.txt

docs/                # 架构文档（arch/ storage/ analysis/ process/ diagrams/）
scripts/             # 共享脚本
openspec/            # OpenSpec 变更管理
archive/             # 归档
vendor/third_part/  # 第三方依赖（googletest/uthash/sqlite3/cmocka）
vendor/cmake/        # 共享 CMake 工具（ProjectUtils.cmake）
build/               # 编译产物（build/engine）
test-results/        # 测试/运行产物（logs/coverage/test dbs）
```

## 根目录准入规则

**严禁在根目录放置任何新文件，除非属于以下白名单：**

| 文件/目录 | 类型 | 说明 |
|-----------|------|------|
| `.clang-format` | 配置文件 | 代码风格 |
| `.git*` | Git 内部 | git 配置和属性 |
| `.ai/` | AI 配置 | 收敛后的 AI 工具配置（`.ai/.claude/`、`.ai/.agents/`、`.ai/.opencode/`、`.ai/.vscode/`） |
| `.obsidianignore` | Obsidian 配置 | |
| `.github/` | CI/CD | GitHub Actions |
| `.remember/` | 记忆系统 | 日程/上下文 |
| `.playwright-mcp/` | MCP 配置 | Playwright MCP（仅保留 `.gitkeep`，运行时产物不入库） |
| `AGENTS.md` | 项目配置 | Agent 说明 |
| `CLAUDE.md` | 项目配置 | 本文件 |
| `CMakeLists.txt` | 构建配置 | 双轨调度器 |
| `README.md` | 项目文档 | 入口文档 |
| `skills-lock.json` | 能力锁定 | 已安装技能 |

**绝对禁止出现在根目录的内容：**
- ❌ 任何 `.o` `.obj` `.so` `.a` `.dll` 等编译产物
- ❌ 任何 `.md` 报告文件（`*report.md`、`*result.md`）
- ❌ 任何 `.c` `.cpp` `.h` 源码文件
- ❌ 任何测试数据目录（除非用户明确要求）
- ❌ 任何调试脚本（`*debug*.sh`、`test_*.py` 等）

**文件归宿速查表：**

| 你想放的文件类型 | 应该放哪里 |
|-----------------|-----------|
| 源码 `.c` `.h` | `engine/src/<库>/` |
| 测试代码 `.cpp` | `engine/test/<库>/` |
| 编译产物 `.o` | 已在 `build/engine/` |
| 报告/文档 `.md` | `docs/` 或 `archive/` |
| 测试数据 | `engine/test_data/` 或 `test-results/<项目>/` |
| 临时调试文件 | 用完即删，或 `archive/` |

**测试产物清理规则：**
- 测试产生的临时文件（日志、数据库文件、中间数据）应在测试完成后删除
- `test-results/` 目录只保留必要的测试报告和覆盖率数据
- 不再需要的测试产物执行清理，保持目录整洁

**规则违反处理：** 任何在根目录发现禁止文件，应立即提醒用户并建议正确的归档位置。

## 根目录瘦身纪律

1. **新业务内容**必须放在 `engine/` 对应目录，不得创建新顶层业务目录
2. **编译产物**统一进入 `build/<项目或轨道>/`，测试产物统一进入 `test-results/<项目或轨道>/`，禁止产物污染源码目录
3. **旧顶层兼容入口已在 Phase 2 中删除**，所有内容均通过 canonical 路径访问
4. **旧顶层真实代码**在迁移到对应轨道后不再保留双副本

## 外部源码（Git 子模块）

```
vendor/third_part/googletest/  # GoogleTest（vendored，CMake 自动包含，无需系统安装）
vendor/third_part/uthash/      # uthash 哈希表头文件（engine/third_part/uthash 为子模块）
vendor/third_part/sqlite3/     # SQLite amalgamation
vendor/third_part/cmocka-1.1.7/# cmocka（仅测试用）
```

> 原 `reference/open-source/` 下的开源源码镜像（faiss/redis/postgres/pgvector/
> sqlite3/elasticsearch/chroma/milvus/mysql/neo4j/openGauss/ann-benchmarks）
> 已随参考轨整体迁出至独立仓 `yinchengwang/reference`。

## CMake 构建架构

根 `CMakeLists.txt` 是工程调度器，按 `ENGINEERING_BUILD` 开关调度；学习轨已切出独立仓，不再有 `LEARNING_BUILD`。

- **`cmake/ProjectUtils.cmake`**（位于 `engine/cmake/`）提供两个辅助函数：
  - `add_project_test(name VAR)` — 通配 `CMAKE_CURRENT_SOURCE_DIR` 下的 `*.c`/`*.cpp`，创建 gtest 可执行文件，自动发现测试用例
  - `add_project_library(name VAR)` — 同上通配模式，创建静态库
- `project_includes` 是 INTERFACE 库，携带 `engine/include/` 目录作为头文件搜索路径
- **CMake 输出目录铁律**：
  - `RUNTIME_OUTPUT_DIRECTORY` 必须使用 `${CMAKE_BINARY_DIR}`，**禁止使用** `${CMAKE_CURRENT_SOURCE_DIR}`（后者会把二进制输出到源码目录）
  - `add_project_test()` 已内置此约束，新增测试应优先使用此函数
  - 手动设置 `RUNTIME_OUTPUT_DIRECTORY` 时必须显式检查值，不得引入源码路径
- 测试二进制和所有编译产物输出到 `build/<项目或轨道>/`，**不再输出到源码目录**
- 测试运行时产物（日志、数据库、覆盖率）输出到 `test-results/<项目或轨道>/`
- `all_tests` 是 `add_custom_target` 包装的 ctest

## 库间依赖关系

```
index ──→ algo ──→ (无外部依赖)
                └── project_includes (仅头文件)

self_made ──→ project_includes
self_made_cpp ──→ project_includes
leetcode ──→ project_includes
interview ──→ project_includes

主二进制 (AlgorithmPractice) ──→ project_includes + self_made + leetcode + Threads::Threads
```

## 数据库存储引擎

> **本仓只有一套数据库内核**：`engine/db/`（从零手写的多模态数据库内核）。
> 它自带 `Makefile`，**不在 CMake 构建图内**（根 `CMakeLists.txt` 只引入 `engine/`，
> `engine/CMakeLists.txt` 只引入 `src/` `tools/` `test/`，均不含 `db/`）——
> 这是有意的例外：它有自己的分层门禁、编译门禁与测试协议。
>
> 原先并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，
> 源码、公开头、测试与构建接线（含 `cmake/ModalityProfiles.cmake`、`MultimodalConfig.cmake`）
> 全部删除。那个位置**现在没有内核** —— 不要按旧文档或旧路径去找它。
> 历史分析文档已归档到 `docs/analysis/archive/`。

主核的分层、纪律与构建命令见「子项目规则 › 主数据库内核（`engine/db/`）」。
嵌入式 SDK（`engine/src/sdk/`）是主核能力的独立实现，**只依赖 SQLite 一个外部库**。

## 添加代码的约定

- **新数据结构/算法** → 在 `engine/src/<库>/` 下添加 `.c`/`.h`，在 `engine/include/<库>/` 下添加公共头文件
- **新测试** → 在 `engine/test/<库>/` 下添加 `.cpp`（学习轨题解测试已随独立仓迁出）
- **新独立项目** → 在 `engine/` 下创建直属目录及自带 `CMakeLists.txt`（例外：`engine/db/` 自带 `Makefile`，不入 CMake 图）
- **新库** → 创建 `engine/src/<库名>/CMakeLists.txt`（推荐用 `add_project_library()`），在 `engine/include/<库名>/` 放头文件

## 测试约定

- 测试框架为 GoogleTest（vendored 在 `vendor/third_part/googletest/`）
- 测试文件统一用 C++（`.cpp`），即使测试的是 C 代码 —— 通过 `extern "C"` 引入 C 头文件
- 单独运行某个测试：`ctest --test-dir build/engineering -R <名称> --output-on-failure`
- 测试二进制和产物统一输出到 `build/<项目或轨道>/` 和 `test-results/<项目或轨道>/`，**不再输出到源码目录**

## 子项目规则

### 主数据库内核（`engine/db/`）

`engine/db/` 是本仓**唯一长期维护的数据库主内核**，一个**从零手写的多模态数据库内核**（2026-11-16 路线翻转后确认）。
它**自带 `Makefile`，不在 CMake 构建图内**（根 `CMakeLists.txt` 只引入 `engine/`，
`engine/CMakeLists.txt` 只引入 `src/` `tools/` `test/`，均不含 `db/`）——这是有意的例外，
因为它有自己独立的分层门禁、编译门禁与测试协议。

并列的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役** —— 源码、公开头、
测试与构建接线（`cmake/ModalityProfiles.cmake` / `MultimodalConfig.cmake` / `mm_smoke.cmake` 等）全部删除。
**本仓现在只有一个数据库内核** —— `engine/db/`。

嵌入式 SDK（`engine/src/sdk/`）是另一条独立线：它不链接主核，自己也只依赖 SQLite 一个外部库
（内存上下文、读写锁、向量索引、SQL 引擎适配层都已自持）。

**构建与测试（需 MinGW 工具链 + Git Bash）：**

```bash
export PATH="/c/mingw64/bin:$PATH"   # Windows 下 Makefile 强制 SHELL := sh（依赖 find/rm）
cd engine/db
mingw32-make clean && mingw32-make all   # -Wall -Wextra -Werror，零警告
sh test/run.sh                           # 105 个测试，期望 0 FAIL
sh tools/check_deps.sh                   # 依赖方向门禁，期望 ✓ 依赖方向正确
mingw32-make bench                       # vec_bench / graph_bench / hnswlib_ref 对照基准
```

**分层铁律（由 `tools/check_deps.sh` 强制，改动后必跑）：**

```
L0 common/                      不依赖任何层
L1 storage/  wal/               只依赖 common
L2 index/  txn/  catalog/       禁依赖 query / spi / modalities
L3 query/  spi/                 禁依赖 modalities
L4 modalities/                  模态之间禁互相 include
```

**专属纪律：**

- 编译门禁是 `-Werror`（比工程轨更严），**零警告**是硬要求
- 新文件不超过 800 行；`index/btree.c` 与 `catalog/catalog.c` 的例外**已在 v1.2 步骤 7 消除**（两者均已拆分）；v1.3 账上的历史超限（`src/storage/heap.c`、`src/db.c`、`src/storage/pager.c`）**已全部消除**，现在没有超限文件
- 公开 API 前缀 `db_<模态>_<动作>`；内部错误码 `E_*`，公开层映射为 `DB_E_*`
- 新入口必须成对 `latch_lock()` / `latch_unlock()`；xid 必须显式传参，不得藏进共享对象
- 注释用中文，坑用 `M<层>-<序号>` 编号（汇总在 `engine/db/docs/MODALITIES.md`）
- 新测试放 `engine/db/test/unit/` 或 `engine/db/test/integration/`，命名 `test_*.c` 即被 `test/run.sh` 自动收集

### 前端子项目（`apps/`）

前端已从微信小程序重写为**本机 Web 应用**（2026-11-26 完成合并）。

`apps/web/` 是合并应用：组合根（`server/`）+ 共用层（`shared/`）+ 4 个模块前端（`web/`），
共 43 个页面（workbench 18 / knowledge 17 / trip 3 / games 5）。
零 npm 依赖、零构建（原生 ES2020 + DOM + Canvas），浏览器侧用 `web/cjs-loader.js` 加载
CommonJS 模块 —— 测试与浏览器跑的是同一份源码。

`apps/study-hub/`（5174）与 `apps/wiki-rag/`（8000）是**独立进程**，原地保留。

```bash
node apps/web/server/main.js
cd apps/web && node tests/run-all.js
```

详见 [`apps/README.md`](apps/README.md)。
