# `code` 顶层聚合与单数据库收敛实施计划

> **⚠️ 状态：Stage A 已完成；Stage B 部分已于 2026-11-16 作废。**
>
> Stage A（目录重构）已执行完毕并通过门禁。Stage B 的路线在 2026-11-16 发生翻转：
> 主核由 `engine/src/db` 改为 `engine/db`，冻结对象反过来变成 `engine/src/db`。
>
> **因此本文档 Stage B 起的全部内容（Task B0–B5、阶段编号、验收门）不再是当前执行依据**，
> 仅为历史记录保留。当前 Stage B 的执行依据是：
>
> - 设计：`docs/superpowers/specs/2026-11-15-code-umbrella-and-db-convergence-design.md`（§6 已按翻转后口径订正）
> - 实施设计：`docs/superpowers/plans/2026-11-16-db-capability-matrix.md`（B0–B5 的当前定义）
> - 分阶段产出：`2026-11-16-db-testtmp-inventory.md`（B0 数据盘点）、`2026-11-16-srcdb-asset-triage.md`（B1 资产审计）、`2026-11-16-stageb2-embedded-scope.md`（B2 服务形态决策）
>
> **编号冲突警告**：本文档 Task B3 是「执行旧数据批次迁移和无损校验」，
> 而当前 B3 是「选择并完成一个主核垂直切片」（首个切片见 `2026-11-16-db-result-null-api.md`）。
> 两者同名不同实，引用时以本文档之外的新文档为准。
>
> ---
>
> **执行要求（Stage A，已完成，仅供追溯）：**本计划是供后续执行的分阶段路线图；不得把“写计划”当作目录迁移、git 操作或 DB 变更授权。Stage A 必须完整验收后，才能启动 Stage B。代码任务使用 `superpowers:subagent-driven-development` 或 `superpowers:executing-plans`；跨仓提交必须逐仓处理。

**目标（历史口径，Stage A 部分仍然有效）：**先把 `C:\code` 建成四个自有仓的顶层 Git 聚合仓并更新所有相关 README；~~验收后再以 `engine/src/db` 为唯一主内核，冻结但保留 `engine/db`，并通过经过校验的逻辑导出/导入保全旧数据。~~（**已作废**：翻转后主核为 `engine/db`）

**架构（历史口径）：**Stage A 只重组工作区关系：`code` 直接挂载 `projects`、`learning`、`ai-docs`、`reference`，reference 内部上游仍按需拉取。~~Stage B 在目录阶段验收之后单独盘点能力与数据，逐项将确认需要的能力适配到 `engine/src/db`，验证数据迁移，再将 `engine/db` 标为冻结参考；不删除该目录。~~（**已作废**，见顶部状态说明）

**技术栈：**Git / submodule；Python 标准库一致性工具；Git Bash shell；CMake + CTest（`engine/src/db`，冻结前适用）；MinGW Make + shell 测试（`engine/db`，当前主核门禁）。

## 全局约束

- 四个自有仓代码必须先提交并推送到各自远程，验证远程可达后才允许删除本地 StillEvolving。
- `projects` 当前 15 个未跟踪 JSONB 源码/测试文件属于用户要求纳入并推送的内容；任何其他文件不得顺带暂存。
- `StillEvolving/m5-verify/` 是父仓未跟踪目录；用户已选择将它整体移到 `C:\code\m5-verify` 并加入 code 根 `.gitignore`。移动前须检查文件清单与目标不存在，移动后验证文件数/哈希一致；不提交到任何仓、不删除。
- projects 仓当前有三份总设计/计划/A0 审计文档；用户已批准将它们单独精确提交并推送。单独提交这些文档，不包含 15 个 JSONB 源码/测试。
- 不执行 `git submodule update --init --recursive`；reference 的 95 个上游仓必须保持按需拉取。
- 任何文件删除、目录删除、`.git` 指针删除、DB 文件覆盖或旧数据清理，必须先检查具体目标并取得该动作的明确授权；本计划不等同于执行授权。
- README 更新范围：`code/README.md`、四个自有仓 README、`projects/engine/README.md`、`projects/engine/db/README.md`；不改第三方上游仓 README。
- 历史计划/规格保持历史语境；仅更新仍作为当前操作指引、且迁移后失效的链接/命令。
- 所有对话、说明、注释与 commit message 使用简体中文；技术标识符保留原文。

## 文件与仓库边界

| 位置 | Stage A 职责 | Stage B 职责 |
|---|---|---|
| `C:\code`（code 仓） | `.gitmodules`、`repos.yaml`、README、共享 docs/scripts | 无 DB 源码；只记录阶段和仓关系 |
| `C:\code\projects` | 工程仓、README、路径适配、CMake/CTest | 唯一长期 DB 位于 `engine/src/db`；CMake/CTest 为主入口 |
| `C:\code\learning` | 学习仓 README 与跨仓链接 | 不迁移 DB 实现；作为数据来源仓之一只读盘点，不擅自改学习内容 |
| `C:\code\ai-docs` | README 与跨仓链接 | 不承担 DB 代码迁移 |
| `C:\code\reference` | README 关系与内部按需拉取说明 | 仅作为参考，不作为旧 DB 运行数据来源 |
| `projects/engine/db` | README 更新为冻结边界；目录和源文件完整保留 | 仅用于历史参考与获批的数据逻辑导出；不纳入默认构建/CI，不删除 |
| `projects/engine/src/db` | README 表述为目标主内核 | 逐项适配需要保留的功能，保持其现有 CMake/CTest 集成 |

---

# Stage A — 建立 code 顶层聚合仓

> **Stage A gate：**本阶段所有子仓远程同步、目录/README/脚本验证和 code 远程验证完成前，不得开始 Stage B。

## Task A0：保护现场并完成全仓远程完整性审计

**Files / 仓：**只读检查 `StillEvolving/`、`projects/`、`learning/`、`ai-docs/`、`reference/` 及已初始化嵌套子仓；不修改文件。

- [ ] **Step 1：建立仓库清单。** 逐仓记录工作树根、`.git` 是真目录还是指针、HEAD、origin URL、当前分支/Detached HEAD、`origin/<branch>...HEAD`、status、submodule pin。
- [ ] **Step 2：记录未提交内容。** 使用 `git -C <repo> status --short --untracked-files=all`；将每项归为明确代码、生成物、用户数据、未知。未知项停下询问，不暂存。
- [ ] **Step 3：审计 projects 的 15 个 JSONB 文件。** 文件精确范围为 `engine/db/src/modalities/doc/{json_ovf.c,json_ovf.h,json_parse.c,json_parse.h,json_path.c,json_path.h,json_scalar.c,jsonb.c,jsonb.h,jsonb_get.c}` 和 `engine/db/test/unit/{jsonb_test_common.h,test_json_ovf.c,test_json_path.c,test_jsonb.c,test_jsonb_crash.c}`。记录构建收集方式、SQL 接入现状和已知风险；不得宣称 SQL 功能已完成。
- [ ] **Step 4：保护 m5-verify。** 用户已选择将 `StillEvolving/m5-verify/` 整体迁到 `C:\code\m5-verify` 并加入 code 根 `.gitignore`。迁移前核对该目录完整清单、大小、目标目录不存在；只移动、不编辑、不提交；迁移后核对文件数与逐文件哈希一致。
- [ ] **Step 5：确认远程状态。** 对自有仓 fetch 后验证 ahead/behind；检查 gitlink 指向的每个 SHA 在对应远程可取。reference 未初始化的上游仓不递归拉取、不推送。
- [ ] **Step 6：记账但不改工作树。** 输出逐仓“干净且已远程 / 有明确待提交 / 未知需澄清”报告。只有用户批准的明确待提交代码进入后续 task。

**Gate A0：**没有未分类的用户文件；自有仓待提交项清单经过确认；所有要保留的提交有明确目标远程与分支。

## Task A1：验证并推送 projects 的 JSONB 在制品

**Files：**仅上述 15 个 JSONB 文件；测试产物不得加入提交。

- [ ] **Step 1：读取项目规则和构建脚本。** 以 `projects/CLAUDE.md`、`engine/db/Makefile`、`engine/db/test/run.sh`、`engine/db/tools/check_deps.sh` 为准，核实当前文件是否进入主源码目标和测试发现。
- [ ] **Step 2：检查测试前置对象。** 确认 `engine/db/build/*.o` 是否由当前源码构建生成；测试脚本会链接既有对象，因此若对象陈旧，先执行项目规定的 clean/all。
- [ ] **Step 3：运行全量严格构建。** Git Bash 中执行 `export PATH="/c/mingw64/bin:$PATH"`，进入 `/c/code/projects/engine/db`，执行 `mingw32-make clean && mingw32-make all`；记录 exit code 与 `-Werror` 结果。
- [ ] **Step 4：运行依赖方向门禁。** 执行 `sh tools/check_deps.sh`；期待 `✓ 依赖方向正确`。
- [ ] **Step 5：运行完整单元与集成测试。** 执行 `sh test/run.sh`；记录所有测试数量与失败，不以“JSONB 文件被发现”替代通过结果。运行产物 `build/`、`test/tmp/` 必须保持 ignored。
- [ ] **Step 6：阻断处理。** 若构建/测试失败，不提交、不推送；按失败分类提出修复或请求用户决定。对已观察到的契约问题（JSONB 键查找复杂度注释、路径语法注释、overflow 清理分支）先判定是否阻断功能或仅为说明差异，不在本 task 偷加无关修复。
- [ ] **Step 7：精确暂存。** `git -C /c/code/projects add` 仅列上述 15 个路径，然后检查 `git diff --cached --name-status` 必须逐项等于白名单；检查 `git status` 中其他工作区内容仍未暂存。
- [ ] **Step 8：提交、推送并验证。** 使用中文 commit message；推送 `origin main`；确认远程分支包含新 SHA，`origin/main...HEAD` 为 `0 0`。不得提交其他在制品。

**Gate A1：**JSONB 提交已由远程 `projects` 获取，完整构建、依赖门禁和测试结果被如实记录。

## Task A2：处理四个自有仓的其他远程差异

**仓库：**`projects`、`learning`、`ai-docs`、`reference`；reference 上游 third-party 另行排除。

- [ ] **Step 1：重新审计每仓。** A1 后重新读取 status、HEAD、remote、ahead/behind，避免依据过时快照提交。
- [ ] **Step 2：修复明确失效的当前 README 路径。** 只改 README 中指向旧 StillEvolving 布局的现行关系说明；历史路径映射、历史计划和历史复盘保持原文，除非该文仍被作为当前操作步骤执行。
- [ ] **Step 3：替换可相对化的硬编码路径。** 对 `.ai/.claude/settings.json` 中实际有效的绝对授权路径、spec-gen 脚本路径、learning 笔记路径逐一确认语义；路径按 `__file__` 或仓根推导，不用全局盲目字符串替换。
- [ ] **Step 4：检查影响范围。** 对改动的脚本跑其静态检查/单测；确认仅列出本 task 白名单文件，不带入未跟踪 DB 文件、构建产物或其他进行中改动。
- [ ] **Step 5：逐仓提交并推送。** 每个仓独立暂存、commit、push；确认各远程分支均可读取对应 SHA。干净仓不造空提交。

**Gate A2：**所有自有仓的代码和 README 修改均能从远程重新获取；嵌套第三方仓仍保持原 pin。

## Task A3：迁移 learning 与 ai-docs gitdir 到新工作区层级

**前置：**A0/A2 通过；用户明确同意执行相应文件/目录操作；目标路径不存在冲突。

- [ ] **Step 1：停止目录内服务。** 检查当前进程是否以待移动目录为 cwd；先说明 PID/命令行，取得同意后再停止。进程不存在则跳过。
- [ ] **Step 2：脱钩 learning 前先备份 gitdir 配置。** 记录 `.git` 指针、模块 config、所有嵌套模块 config 和 worktree 字段；不得先删指针。
- [ ] **Step 3：解除旧 core.worktree 并移动 gitdir。** 采用已核实的顺序：先清除旧 `core.worktree`，再移除指针文件，随后把整个真实 gitdir（含嵌套 modules）放入 learning 工作树。操作失败立即停止并按记录恢复。
- [ ] **Step 4：重算并验证嵌套 googletest 路径。** 目标 gitfile 从新工作树指向 `learning/.git/modules/third_part/googletest`；嵌套 config 的 worktree 必须解析到 `learning/third_part/googletest`。完成后执行两层 `git rev-parse --show-toplevel`、HEAD 和 status 检查。
- [ ] **Step 5：脱钩 ai-docs。** 按同样的 config 备份与验证流程处理；确认其工作树根、HEAD、origin 和 status。
- [ ] **Step 6：将两个工作树移动到 `C:\code` 平级。** 不复制、不清空原工作树；验证仓库历史、远程与嵌套关系不变。

**Gate A3：**learning、ai-docs 及 learning/google test 均能识别为独立正确工作树，HEAD 与远程 pin 一致。

## Task A4：迁移共享 docs、scripts 和关系清单

**Files：**`StillEvolving/docs/`、本地 `C:\code\docs/` 镜像、`StillEvolving/scripts/`、`repos.yaml`、`.gitattributes`、`LICENSE`。

- [ ] **Step 1：计算 docs 清单差异。** 对同名文件执行逐字节 diff；合并前把冲突列出。保留唯一权威的 StillEvolving docs 和本地独有的 README/内容，经用户确认后合并。
- [ ] **Step 2：迁移许可证和属性规则。** 仅当源文件与 code 远程初始文件兼容时复用；检查行尾规则（`.sh` LF、`.ps1` UTF-8 BOM + CRLF、Python LF）。
- [ ] **Step 3：迁移 scripts 并修路径。** fetch-reference 默认根改为 code 下的 `reference/`；测试 fixture 根路径同步；bootstrap 初始化 code 的四个 gitlink，但不得递归 reference 上游。
- [ ] **Step 4：升级 consistency checker。** 允许 reference 同时是直接 submodule 且拥有 `upstream` manifest；校验 optional/uninitialized reference 状态、`update=none` 配置、catalog/lockfile pin 一致性。
- [ ] **Step 5：更新脚本测试。** 增加新路径、新 submodule 结构、reference 未初始化与显式 bootstrap 初始化等测试；保留原有对危险递归行为的守卫。
- [ ] **Step 6：重写 `repos.yaml`。** 四个 `path` 均为 code 根下一层；reference 为 `fetch: submodule`，保留 `upstream` 块、catalog 与 manifest 路径、总数和 pin 字段；声明 `update: none` 与可选初始化语义。
- [ ] **Step 7：运行脚本测试。** 执行 Python unit tests 与 fetch shell tests，核对全部输出和退出码。未初始化上游不得因此自动下载。

## Task A5：建立 code Git 仓并登记四个 submodule

**前置：**A2、A3、A4 完成；README 更新 task A6 内容已准备且所有自有仓 HEAD 可从远程取得；远程 `code` 仓初始内容已检查。

- [ ] **Step 1：检查 code 远程初始提交。** 确认 LICENSE、README 与本地计划无冲突；记录远程分支/commit。
- [ ] **Step 2：初始化本地仓并保留工作树。** `git init -b main`、设置 origin、fetch 初始 main；如需对齐初始提交，使用保留工作树的方式。禁止 `reset --hard`。
- [ ] **Step 3：逐个登记既有仓。** 对 `projects`、`learning`、`ai-docs`、`reference` 执行 `git submodule add`；每项后立即检查 `.gitmodules` 与 index mode=160000，确认 gitlink SHA 正确。
- [ ] **Step 4：配置 reference 防护。** 将 `submodule.reference.update=none` 写入 `.gitmodules`；在临时副本验证裸 recursive clone/update 不获取 reference 内部上游。
- [ ] **Step 5：先登记 gitlinks，再 `git add -A`。** 检查 staged name/status，不允许子仓源码作为 code 普通文件被跟踪。

## Task A6：逐仓更新 README 并验收 Stage A

**Files：**按设计 §4 的七个 README 范围；每个文件提交到所属仓。

- [ ] **Step 1：更新 code README。** 写清四仓路径、角色、bootstrap、reference 的 `update=none` 与按需 fetch，列出 `repos.yaml` 和 `.gitmodules` 的权责。
- [ ] **Step 2：更新 projects 与 engine 入口。** `projects/README.md` 和 `engine/README.md` 指向 `engine/src/db` 主线；`engine/db/README.md` 标为冻结保留，不要写成已删除或默认主库。保留各自构建说明，但标注旧入口的维护状态。
- [ ] **Step 3：更新 learning、ai-docs、reference README。** 修正父仓名称、相对链接、reference submodule 身份和按需拉取说明；从各 README 独立打开链接，确认目标存在。
- [ ] **Step 4：提交并推送三份计划文档。** 将 `docs/superpowers/specs/2026-11-15-code-umbrella-and-db-convergence-design.md`、`docs/superpowers/plans/2026-11-15-code-umbrella-and-db-convergence.md` 与 `docs/superpowers/plans/2026-11-15-code-umbrella-and-db-convergence-stage-a0-audit.md` 单独精确暂存、中文 commit、推送 projects origin；确认远程包含新 SHA。此提交不得包含任何 JSONB 源码或测试。
- [ ] **Step 5：逐仓提交并推送 README。** 每个仓独立精确暂存 README；中文 commit message；推送后确认新 SHA 在各 origin 可达；随后更新 code gitlinks。
- [ ] **Step 6：运行最终目录门禁。** 跑 consistency checker、Python tests、shell fetch tests；验证嵌套 repo 和 reference bootstrap；做临时目录 clean clone smoke test。
- [ ] **Step 7：清理旧父仓前安全检查。** 输出 `StillEvolving` 剩余受跟踪/未跟踪文件；确认 docs/scripts/repos/license 均已迁移、m5-verify 已迁到 code 根并被 ignore、四个自有仓远程可恢复。取得用户对删除本地 StillEvolving 的单独明确授权后，才执行删除。
- [ ] **Step 8：提交并推送 code 聚合仓。** 只提交聚合 README/docs/scripts/repos/.gitmodules/gitlinks 和 `.gitignore`；验证 `git status`、submodule status、远程 SHA 与干净 clone。

### Stage A 最终 Gate

所有四个自有仓和其更新后的 README 均已推送；code 仓自身已推送；bootstrap 不会递归获取 reference 上游；clean clone 成功；所有待保留未跟踪内容（尤其 15 个 JSONB 文件、m5-verify）归属明确。通过后，才允许开 Stage B 详细任务。

---

# ~~Stage B — `engine/src/db` 唯一主内核，冻结保留 `engine/db`~~（已作废，2026-11-16）

> **⚠️ 本节起全部任务作废。** 路线已翻转为 `engine/db` 为唯一主核、`engine/src/db` 冻结保留。
> 当前 B0–B5 的定义见 `2026-11-16-db-capability-matrix.md`。以下内容仅作历史追溯，**不得**按此执行。
>
> 下方文字不改写，保留作废时刻的原貌，以便对照翻转前后差异。
>
> **硬依赖（历史）：**Stage A 最终 Gate 未通过时，不得执行 Stage B 的任何数据或代码迁移任务。Stage B 是后续独立评估与迁移阶段，不以本计划授权破坏性操作。

## Task B0：建立 DB 能力与数据资产基线

**Files：**`projects/engine/db` 与 `projects/engine/src/db` 源码、测试、CMake/Makefile、README/设计文件；不得修改。

- [ ] **Step 1：构建能力矩阵。** 每项标记为“实测完成/仅有实现未验证/默认未启用/明确未实现/仅设计”；分组覆盖 page/buffer、WAL/recovery、事务/MVCC/vacuum、heap/catalog/index、SQL/parser/executor、vector/graph/doc/FT/spatial/TS/blob/KV、RAG/distributed/protocol。
- [ ] **Step 2：确认主库当前实际 profile。** 阅读 `engine/cmake/ModalityProfiles.cmake`、`MultimodalConfig.cmake`、`engine/src/db/storage/CMakeLists.txt` 和 CI workflow，记录默认/显式启用的模态与已知编译限制。
- [ ] **Step 3：制定数据清查清单。** 对旧 `engine/db` 工作目录扫描数据库文件、WAL、测试输出、用户数据；逐项记录路径、大小、格式/来源、是否可重建、是否需要迁移。只读扫描，不读取密钥或上传数据。
- [ ] **Step 4：确认每类数据的语义映射。** 对关系表、blob 对象、graph/vector/FT/geo/TS/document 数据分别列出导出 API、目标 schema、无法表达的字段和目标 DB 是否支持；缺目标语义的项先停下，不忽略。
- [ ] **Step 5：提交迁移范围与容量报告。** 计算源备份、导出包、目标库三份空间的上限；请用户确认必须迁移的数据集和备份位置。

**Gate B0：**具名数据清单与目标映射获得用户批准；没有默认将 `build/`、`test/tmp/` 或真实业务数据视为可删除。

## Task B1：设计并验证逻辑导出格式（独立子设计/评审）

**Files（预计，需 B0 后精确到实际位置）：**旧 DB 导出 CLI/工具、导出 schema 文档、fixture tests；默认放项目脚本/工具目录，不嵌入两个数据库内核。

- [ ] **Step 1：为每种需迁移的模态定义可往返的中间 schema。** 至少规定版本、字符集、类型/null、主键、排序、对象分块、时间/坐标、索引重建规则和校验摘要。
- [ ] **Step 2：创建小型边界 fixture。** 覆盖空值、重复键语义、Unicode、超长数据、事务可见性、删除/更新、特殊浮点与二进制内容。
- [ ] **Step 3：写失败测试。** 测试导出覆盖所有 fixture，导入后逻辑内容相同；测试中断恢复不会重复插入或漏数据。
- [ ] **Step 4：实现最小导出器。** 只读旧 DB，通过公开逻辑 API 导出，附 source commit、源库标识、对象计数、逐对象 hash、错误清单；绝不修改源库。
- [ ] **Step 5：实现独立校验器。** 不复用导出器的序列化逻辑计算目标规范化内容摘要，避免同一 bug 同时写错和验错。
- [ ] **Step 6：运行 fixture round-trip。** 小型数据先经过 export → import → independent verify；任一项 mismatch 时保持失败并修复，不处理生产数据。

## Task B2：为 `engine/src/db` 明确迁移接入契约

**Files：**`engine/include/db/` 中适用公共 API、`engine/src/db` 对应 storage/index/parser/executor 子模块、`engine/test/db` 对应目录、各 CMakeLists。

- [ ] **Step 1：选出首个迁移垂直切片。** 从 B0 映射中选一个确有价值、主库缺口明确、能够写端到端测试的能力；优先让用户确认，不同时迁移多模态。
- [ ] **Step 2：先写主库可观察行为测试。** 在 `engine/test/db/<domain>/` 写 public API/SQL 行为测试，明确成功、错误、恢复和数据校验断言；运行确认失败原因是缺能力而非 fixture/构建故障。
- [ ] **Step 3：设计主库原生接入点。** 使用现有 catalog/heap/AM/storage registry/txn/WAL；禁止在新主库代码中 include `engine/db` 私有头、链接旧内核对象或复制第二套运行时状态。
- [ ] **Step 4：实现该垂直切片。** 每次只完善一个可独立测试的模块与它的构建接线；不新增主库之外的 Makefile 轨道。
- [ ] **Step 5：跑目标 profile 的 build + CTest。** 验证旧测试和新增测试；若 profile 未覆盖该模块，则将启用配置纳入任务并核实不会扩大默认依赖闭包。
- [ ] **Step 6：对比旧 DB 行为。** 对同一 fixture 从旧 DB 导出并与新 DB 查询/导入后内容比较；性能不能替代一致性和持久性断言。
- [ ] **Step 7：记录是否迁移下一项。** 若要迁移第二项，另建子任务和可拒绝的评审点；如果需复制 pager/WAL/事务底座，停止该方向并重新评估。

## Task B3：执行旧数据批次迁移和无损校验

**前置：**B1 round-trip 通过；B2 证明目标 schema/API 支持该批数据；原始数据备份目标已由用户确认。

- [ ] **Step 1：生成只读源快照。** 先正常 checkpoint/close 旧 DB；复制源文件与 WAL 到授权的只读备份位置；记录 SHA-256 和文件清单。不得对唯一原件做迁移操作。
- [ ] **Step 2：导出每一类已批准数据。** 产出版本化中间格式、manifest、计数和 hash；非零失败数必须阻断导入。
- [ ] **Step 3：建立全新目标 DB。** 使用主库新数据目录，不复用已有数据目录；记录 schema/profile/build commit。
- [ ] **Step 4：导入并保存可恢复进度。** 对每批记录源 ID、目标 ID 和状态；确保重跑幂等，避免断点续跑产生重复数据。
- [ ] **Step 5：独立核验。** 逐类型比较数量、主键集合、关键字段、内容 hash、blob 长度/sha、模态元数据；检查索引重建并做随机样本读回。
- [ ] **Step 6：形成差异报告。** 列出全部 mismatch、未映射字段、未支持模态与失败项。不得只报告汇总“成功”。
- [ ] **Step 7：用户审阅并决定未迁移项。** 未迁移数据必须逐项得到接受、补实现或继续保留旧格式备份的决定；不自动丢弃。
- [ ] **Step 8：保留双重可恢复源。** 原始 DB 备份与中间导出包都保留；本计划不执行清理。

## Task B4：将 `engine/db` 标记为冻结参考并从默认维护入口移除

**Files：**`projects/engine/db/README.md`、`projects/engine/README.md`、`projects/README.md`、`projects/CLAUDE.md`、CI/build orchestration 中明确列出旧独立 DB 的文件。

- [ ] **Step 1：检查全部旧主构建引用。** 搜索 Makefile 命令、CI workflow、README、CLAUDE/AGENTS 规则、脚本；列出现行开发入口。
- [ ] **Step 2：更新冻结告示。** 标明冻结日期、主内核路径、旧目录保留原因、数据迁移报告位置、允许的只读操作；不宣称旧项目已删除或全功能已迁走。
- [ ] **Step 3：从“默认受支持测试矩阵”中移除旧 DB。** 只移除自动常规维护入口；旧 Makefile、源码、测试、历史文档留在原位置，按需手动运行须另有批准。
- [ ] **Step 4：验证主库默认 build/test 不触发 engine/db。** 检查 CMake graph、CI 日志、`git status`；确保源目录仍完整存在且可审计。
- [ ] **Step 5：复核所有 README。** 重新逐份检查 §4 指定 README，确认文案之间一致，并清楚标明旧目录只是冻结参考。

## Task B5：收敛终验与状态登记

- [ ] **Step 1：运行主库 clean configure/build。** 在新的独立构建目录构建已批准的 profile，避免沿用旧 CMakeCache；保存完整日志。
- [ ] **Step 2：运行主库完整相关 CTest。** 记录 discovered/executed/passed/failed/skipped 数量；失败项如实归档。
- [ ] **Step 3：重复逻辑数据核验。** 对最终目标 DB 再跑独立验证器，确保迁移完成后未发生新写入差异。
- [ ] **Step 4：确认旧目录和备份保留。** 检查 `engine/db` git 跟踪文件数量与源提交；检查原始 DB 备份与 export package 的 SHA/manifest；不删除任何一项。
- [ ] **Step 5：更新阶段报告与主 README。** 只记录实际完成且有命令/报告证据的功能；注明未迁移能力与数据备份位置。
- [ ] **Step 6：等待用户审核。** 只有用户审阅迁移差异、未迁移列表和备份清单后，Stage B 才能标为完成。删除 `engine/db` 不属于本计划任何步骤。

## 全局验收门

### Stage A 通过条件

- 所有自有仓需要的提交均已推送，远程可取；code 有四条直接 submodule。
- reference 初始化策略防止无意递归抓取其内部上游。
- consistency、bootstrap/fetch 脚本测试通过；clean clone 验证成功。
- 所有自有仓根 README 与 `engine/README.md`、`engine/db/README.md` 更新并推送。
- projects 的 15 个 JSONB 文件不再是未跟踪：要么按用户决定已测试、提交、推送；若测试失败，Stage A gate 未通过，不能删除 StillEvolving。
- m5-verify 的归属已明确并按批准方案保留/迁移。

### Stage B 通过条件（历史口径，已作废）

- ~~只维护 `engine/src/db`；`engine/db` 处于冻结状态但目录、历史和原始数据备份保留。~~（翻转后：只维护 `engine/db`）
- 所有批准迁移的数据具有导出/导入 manifest、独立核验报告和用户确认。
- 所有批准适配能力有主核 public-behavior tests 和实际 build/test 证据。
- 不支持/未迁移项全部明确列出；没有以静默忽略换取“迁移完成”。

## 现在的执行边界

**Stage A 已完成。** Stage B 的执行依据已不在本文件——见 `2026-11-16-db-capability-matrix.md` 与 `2026-11-16-db-result-null-api.md`。
