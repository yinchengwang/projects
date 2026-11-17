# `code` 顶层聚合仓与数据库收敛总设计

> 状态：设计已获用户确认。Stage A（目录重构）已完成；Stage B（数据库收敛）进行中。
> 范围：先把 `C:\code` 整理为四个自有仓的顶层聚合仓；目录阶段验收后，再启动数据库收敛阶段。
> 明确约束：`engine/db` 为唯一主内核；`engine/src/db` 冻结保留，不在本设计中删除；旧数据必须逻辑迁移并验证不丢失。
>
> **口径变更（2026-11-16）**：Stage B0 能力与数据盘点后，主核由 `engine/src/db` 翻转为 `engine/db`（见 §2 决策 6）。本文档 §6 已按翻转后的口径订正；§4 的 README 契约表同步为新主核方向。翻转前的旧口径仅保留在 git 历史中，不作为当前指引。

## 1. 背景与目标

`C:\code` 当前不是 Git 仓库。`StillEvolving/` 是既有父聚合仓，包含 `projects`、`learning`、`ai-docs` 三个 submodule；`reference/` 是与其平级的独立仓，内部管理大量上游源码 submodule。此前已确定希望由新建的 `github.com/yinchengwang/code.git` 直接聚合四个自有仓，并避免 reference 的上游依赖被递归初始化。

工程仓 `projects` 内同时存在两套数据库实现：

- `engine/src/db`：由工程 CMake/CTest 管理的 PostgreSQL-inspired 模块化内核。
- `engine/db`：自带 Makefile、分层依赖门禁和独立测试的从零实现多模态内核。

两者在存储、WAL、事务、索引、SQL 和若干模态上重叠，但数据格式、API、依赖边界和构建体系互不兼容。本次的目标不是把两套源代码机械拼接，而是建立一个清楚的仓库拓扑，并在目录阶段之后收敛为一个长期维护的数据库内核。

## 2. 已确认的决策

1. `C:\code` 最终成为 code 顶层聚合仓；四个自有仓直接挂载为 submodule：`projects`、`learning`、`ai-docs`、`reference`。
2. `reference` 仍保留按 catalog 清单按需初始化内部上游源码的流程；在 code 层使用 `update = none` 防止普通递归初始化造成大规模下载，bootstrap 必须显式处理 reference。
3. 先完成目录重构并单独验收，再启动数据库收敛。两阶段不交错提交。
4. 任何删除旧父仓目录或 gitdir 前，先确保所有自有仓代码已提交并可从远程获取。`projects` 当前 15 个未跟踪 JSONB 源码/测试已由用户确认应纳入并推送，但仍须审查、验证、精确提交并确认远程 SHA。
5. 每个自有仓的根 README 以及受重构影响的项目入口 README 都要更新；不改第三方上游仓的 README。
6. Stage B0 完成能力与数据盘点后，用户最终选择**路线 B：以 `engine/db` 为唯一主数据库**。它具有已实测通过的单机存储/事务/索引核心；`engine/src/db` 的真实可用外围功能按需适配，不把 stub 当资产。
7. `engine/src/db` 冻结并保留目录、源码、CMake、tests 和历史；删除或恢复维护均需另行设计与用户批准。
8. 不要求旧 DB 的磁盘格式由其他内核直接读取；旧项目中用户确认需要保留的数据须逻辑导出、导入最终主核并验证无遗漏。测试/benchmark 数据须逐项确认来源和是否可重建；未经批准不清理。
9. 数据库能力迁移不以“源码目录存在”为完成标准，必须逐项经过构建/测试/API 调用路径验证。


## 3. 目标仓库结构

```text
C:\code\                         # code：关系、入口、共享引导和规范
├── .gitmodules                   # 四个自有仓 gitlink
├── repos.yaml                    # 仓角色、URL、路径、初始化策略
├── README.md                     # code 仓入口与 bootstrap
├── docs/                         # 目录规范、决策与计划
├── scripts/                      # bootstrap、清单按需拉取、一致性校验
├── projects/                     # submodule
├── learning/                     # submodule
├── ai-docs/                      # submodule
└── reference/                    # submodule；内部上游仍按需拉取
```

code 聚合仓只承载仓库关系、引导脚本、总览规范和共享决策，不复制各子仓的业务源码。`.gitmodules` 是 Git 原生 pin 的事实来源；`repos.yaml` 描述仓角色、拉取/更新策略和内容边界；一致性工具检查二者与实际目录状态的一致性。

### 3.1 迁移安全边界

- 先对 `projects`、`learning`、`ai-docs`、`reference` 及嵌套仓做逐仓只读状态审计：HEAD、远程跟踪、工作区未提交内容、gitlink pin、嵌套 gitdir。
- 明确属于仓代码的修改精确暂存；用途不明的文件先停下确认；不得把 ignored build/cache、临时文件或无关用户更改加入 commit。
- 对 `learning` 和 `ai-docs` 的 submodule gitdir 脱钩要验证主仓及嵌套 submodule 的 `rev-parse`、status、远程；路径不确定时停止，不猜路径。
- `projects` 已移动到 `C:\code\projects`。当前旧父仓显示 `D projects`，这是本地移动的结果；不得用 `reset --hard` 恢复或覆盖该工作树。
- projects 仍有 15 个未跟踪 JSONB 文件；先按用户授权审查与测试，提交时仅包含这批文件，不顺带提交其他修改。
- `StillEvolving/m5-verify/` 是未跟踪目录，处理方式必须单独明确；不得因删除旧父目录而无声丢弃。
- 根目录的临时物与 `_archive` 等按已确认的 ignore 策略保留在磁盘、不提交、不删除。docs 上移后是 code 仓受跟踪的权威文档，不应再按临时镜像忽略。
- reference 的内部上游 submodule 不属于 code 的四个自有仓，不为本次迁移而递归初始化或推送到第三方远程。

## 4. README 更新契约

README 修改按各自仓库责任撰写，避免复制同一长篇说明；code 根 README 是仓间关系的总入口，子仓 README 是各自独立入口。

| README | 必须表达的更新 |
|---|---|
| `code/README.md`（新增） | 四仓平级结构、各仓职责、clone/bootstrap、reference 防递归下载说明、权威关系文件 |
| `projects/README.md` | 工程仓位于 code 聚合下；数据库主线为 `engine/db`；`engine/src/db` 冻结保留、不得误当主库；reference 是直接 sibling submodule |
| `projects/engine/README.md` | 工程构建入口；唯一主数据库代码、构建和测试路径；冻结内核的位置与非默认维护属性 |
| `projects/engine/db/README.md` | 主内核的独立构建边界：自带 Makefile、分层依赖门禁、`-Werror` 门禁与测试协议；说明它与 `engine/src/db` 的分离历史、JSONB 等能力的真实接入状态（不宣称未接 SQL 的能力已实现） |
| `learning/README.md` | 父聚合仓改为 code；更新 `../projects`、`../ai-docs`、`../reference` 路径和 submodule 关系 |
| `ai-docs/README.md` | 聚合仓名称与跨仓链接更新；说明 code 是唯一关系总览入口 |
| `reference/README.md` | 从“与 StillEvolving 平级且非 submodule”改为 code 的直接 submodule；说明 `update=none` 和内部按需拉取语义 |

历史计划、规格和复盘文件保留当时语境，不批量替换历史中的 `StillEvolving` 字样。只修仍作为当前指引、且迁移后会失效的链接/命令；对于需要保留原始历史路径的文档，标注其为历史状态而不是篡改记录。

## 5. 阶段 A：目录重构

### 5.1 步骤边界

1. **远程完整性门**：核实四个自有仓和相关嵌套仓。将用户确认应保留的 `projects` JSONB 文件精确提交、运行规定验证、推送到 projects 远程，确认远程可获取提交。检查其他仓的改动；干净且远程同步的仓不造空提交。处理 m5-verify 的归属/保留决定。
2. **迁移 worktree 与 gitdir**：在不覆盖工作树的前提下移动 projects；将 learning/ai-docs 的 gitdir 从旧父仓安全脱钩，再调整嵌套 gitfile/worktree 相对路径。每一步后检查 HEAD、remote、status、nested submodule。
3. **上移 code 资产**：迁移 LICENSE、权威 docs、scripts、repos 清单及属性文件；合并已有 docs 镜像时先比较同名文件，不用盲目覆盖。
4. **重构关系定义与脚本**：将四个路径调整为 code 下平级；reference 同时是 submodule 与 manifest/upstream 管理对象；改 bootstrap、fetch-reference 和一致性检查，保留 `update=none` 的防护。
5. **更新所有自有仓与受影响项目入口 README**：按 §4 范围逐个仓分别提交，避免将多个子仓的文件误提交到另一个仓。
6. **建立 code Git 历史**：以已存在的 code 远程初始提交为基线，用保留工作树的方式接续；逐个登记现有仓库为 submodule，禁止将子仓文件误纳入 code 普通索引。
7. **退役本地 StillEvolving 并发布**：仅在确认其剩余受跟踪资产已迁入、子仓均已远程可恢复、code 状态清单审核通过后，才删除本地旧父仓目录；随后提交并推送 code 聚合仓。

### 5.2 阶段 A 验收

- 四个 submodule URL/path/pin 与 `repos.yaml` 一致；四仓及嵌套仓 HEAD 与预期远程可达。
- 对 reference 的普通 bootstrap 不递归拉取其内部上游；按需 fetch 的 dry-check 与现有 fetch 脚本测试通过。
- consistency checker、Python 单测、shell fetch 行为测试通过。
- `git clone --recurse-submodules` 行为已通过专门验证，不会因 reference 配置意外拉取所有上游。
- 四个仓根 README 与 `projects/engine/README.md`、`projects/engine/db/README.md` 都反映新边界；链接及命令对应当前布局。
- clean clone 能获得 code 自身文件和四个直接 submodule，不带临时目录或大型 reference 上游源码。
- 只有达到以上验收后才允许启动阶段 B。

## 6. 阶段 B：数据库收敛（另立详细计划）

阶段 B 的目标是停止双内核的日常开发、维护和验证负担，不是让两套格式兼容或把两个内核原样拼接。

### 6.1 主内核与冻结边界

> **路线翻转后（2026-11-16）**：主核是 `engine/db`，冻结的是 `engine/src/db`。以下三条按翻转后口径表述。

- `engine/db`：唯一持续开发的 DB 实现，自带 Makefile、L0–L4 分层依赖门禁、严格 `-Werror` 编译门禁与独立测试协议（`test/run.sh` + `tools/check_deps.sh` + `make check`）。新 DB 功能、唯一对外主 API、默认门禁都以它为准。
- `engine/src/db`：功能冻结；保留目录、源码、设计/计划、CMake 和现有测试作为参考资产，但从默认构建、CI、开发指引和“当前受支持能力”列表中标记为非主线。任何后续修改、恢复维护或删除都需用户另行批准。
- 迁移 `engine/src/db` 里的能力时，采用“在主核现有接口内重实现/适配”，禁止引入第二套 pager、WAL、MVCC、catalog 或长期并行 SPI；也不让主核链接两套 pager/WAL/MVCC/catalog/BTree。

### 6.2 能力盘点和迁移判据

逐项标记：

1. 主核（`engine/db`）已有且以构建、实际调用和测试证实的能力——不迁。
2. 主核有源码但构建未启用、实现含占位/TODO、或测试不足的能力——不能算完成；优先决定主核是否真正需要，再补主核能力和测试。
3. `engine/src/db` 有而主核缺失、且用户确认有价值的能力——制定小范围子设计，在主核现有存储、事务、索引、执行器或 API 边界内实现。
4. 只有计划/设计文档、没有接入代码或完成测试的能力——作为方案参考，不作为可迁移的现成功能。
5. 两边存在但接口/语义不同的能力——逐项比较事务、并发、持久化、恢复、索引一致性和错误语义，不能按目录名或 README 声明判断等价。

初步风险重点：`engine/src/db` 事务状态/vacuum 有 TODO、BTree 搜索/插入/扫描未完成、SQL WHERE 明确拒绝；其多模态目录存在但默认启用范围有限；JSONPath 与 JSONB SQL 类型不可混为一谈；主核 JSONB 代码虽有单测，但尚未接入 `db_exec` SQL 路径。具体状态必须在阶段 B 的设计与实测中重核。

### 6.3 旧数据无损迁移

“新格式不直接兼容旧格式”仅表示不承诺文件级原位打开，**不表示可以抛弃旧数据**。详细迁移计划必须：

1. 盘点旧项目所有数据库文件、目录、测试夹具与用户数据，区分可重建的 benchmark/test 临时数据和需保留数据；不得仅凭扩展名自动删库。
2. 为每类需要保留的数据定义逻辑导出 schema，包括列/类型/NULL、主键或对象 ID、关系、模态元数据及必要的语义字段。
3. 实现可重复、可恢复的导出与导入流程；记录源文件/源版本、导出数量、失败项、目标映射和运行日志。
4. 导入到主库支持的等价结构；不支持的模态/字段作为显式未迁移清单，不静默略过，也不得声称整体迁移完成。
5. 迁移前后核对每表/对象数量、关键字段、排序规范化的记录 hash、blob 长度/内容 hash、索引重建结果及失败记录。
6. 保留原始 DB 文件只读备份和导出包；直到用户查看核对报告并批准后，才允许另行清理。备份存放位置与容量需在执行计划中明确。

### 6.4 阶段 B 验收与冻结完成定义

- 需要保留的旧数据已导出、导入，所有范围内计数与校验值一致；未迁移项逐项列明并由用户决定。
- 主核新增/补齐功能在主核测试框架（`engine/db/test/run.sh` + `make check`）中验证；迁移的测试场景实际能抓到目标语义问题，而不是只转抄测试文件。
- 主核关键构建配置、测试、运行/恢复路径通过；失败必须记录，不以目录存在或源码能编译替代功能完成。
- `engine/src/db` 的冻结状态在项目、engine、db 三层 README/开发规约中一致；默认构建和测试入口不再把它呈现为受支持 DB 主线。
- `engine/src/db` 目录和历史仍在原位；本阶段**不删除**该目录，也不修改其远程历史。未来删除必须单独方案、数据确认与明确批准。

## 7. 变更分仓、推送与回滚

- 每个 README、脚本或代码修改提交到文件所属的自有仓；code 聚合仓只记录关系文件、共享 docs/scripts 和子仓 gitlink。
- 每个仓单独检查 status、精确暂存、提交、推送并确认远程包含提交 SHA；不使用父仓提交替代子仓远程推送。
- 每完成一个阶段，形成该阶段独立的提交集合和验收清单。阶段 A 失败时可恢复各 worktree/gitdir 位置；阶段 B 的导入失败时保留源文件和导出包，按批次重跑或回滚新库导入，不改写源库。
- 任何会删除、覆盖或改写旧 DB 文件、旧 `.git` gitdir、未跟踪文件的动作必须在执行前检查目标并取得明确授权；本设计不授权这些破坏性执行。

## 8. 本设计不包含的事项

- 不开始实际目录移动、gitdir 手术、提交或推送。
- 不清理 build 缓存、临时目录、`m5-verify/` 或 JSONB 文件；其归属按阶段 A 计划单独确认。
- 不承诺 `engine/db` 或 `engine/src/db` 的全部声明能力已正确实现。
- 不定义 JSONB、空间、全文或时序的具体主核实现细节；这些需基于差异盘点与测试另立子设计。
- 不删除 `engine/db` 或 `engine/src/db`，不自动删除旧数据库文件，不在阶段 A 开始阶段 B。
