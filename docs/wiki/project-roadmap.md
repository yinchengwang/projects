# 开源数据库、AI 与 AI Infra Wiki 总路线图

> **本文件是 wiki 收集任务的唯一持续进度入口。** 每次结束一个批次，先更新本文件的「当前状态」「批次台账」与「下一步」，再开始下一批；不要另开一份互不关联的进度记录。
>
> **本轮状态（2026-11-28）：** 24/270 项目的机制 wiki 已完成（postgres、redis、sqlite、mysql-server、langchain、llama-index、haystack、lightrag、graphrag、ragflow、ragas、dify、langgraph、autogen、crewai、metagpt、openai-agents、pydantic-ai、smolagents、mcp-python-sdk、browser-use、openhands、goose、open-interpreter），共 1094 篇机制页（DB 四项目 368 + AI 类 726）。**阶段 2A（Agent 图执行与状态机）已收官**（六个框架共 227 篇）；**阶段 2B（工具使用与软件工程 Agent）已收官**（六个项目共 184 篇）；**阶段 2C（RAG / 索引编排框架）进行中**：langchain 37、llama-index 33、haystack 35、lightrag 42、graphrag 31、ragas 35、ragflow 49、dify 53。全部逐篇 lint + refs 全 PASS，但 RAG 未入库，账本状态为 `gates-pass`。候选矩阵已按用户确认的“广义基础设施范围”冻结为 118 个唯一 canonical repo：数据库 54、Agent 24、RAG 20、LLM 20，与现有 152 项 catalog 静态零 name/repo 冲突；约 26 条已有真实 GitHub API 事实，其余尚未核验。项目状态账本 `wiki-project-status.yml` 已建立：270 条在册，4 项 `indexed`、20 项 `gates-pass`、246 项 `planned`。
>
> **下次从这里开始：** 阶段 2C 已收官（八个 RAG 项目共 356 篇）。下一步进入阶段 2D（LLM runtime / serving / kernel 基础设施）。catalog 已达 270 条（候选 118 条全部并入，另补 tigerbeetle / sqlite-vector 两条替补，剔除 db-bench / pklite 两条失效项）；18 条待补真实 metadata（GitHub API 限流未恢复）。项目收集的终点是 270 项全部通过 wiki 验收。

---

## 1. 目标、范围与不可变口径

### 1.1 最终目标

在 `projects/docs/wiki/` 下建立一套可追溯、可校验、能持续扩展的开源技术机制 wiki，覆盖下列全部 **270 个项目清单名额**：

- **数据库 / 数据基础设施：150 项**：关系型、键值、列式、文档、图、搜索、流、时序、向量、分布式 SQL、嵌入式、扩展、基准测试等类别。
- **AI 相关：120 项**：Agent、RAG、LLM（含模型推理、服务和训练基础设施）。
- 每一个被纳入的项目都走完：**来源与分层核实 → 机制盘点 → manifest → 源码引用校验 → 机制页面 → 概览页 → CI → RAG 索引**。

`270` 是项目目录的覆盖目标，不是机制页总数。每个项目的机制页数只能来自该项目独立盘点，**不承诺固定总页数、不用源文件数乘系数估算、不为达成数字制造主题**。

### 1.2 当前真实基线（2026-11-26）

| 指标 | 当前实测 | 目标 / 差额 |
|---|---:|---:|
| 项目 catalog | 152 项 | 270 项；还需评审、核实并补入 118 项 |
| 数据库与数据基础设施分类 | 96 项 | 150 项；缺 54 项 |
| AI / Agent / RAG / LLM 分类 | 56 项 | 120 项；缺 64 项 |
| 已完成机制 wiki 的项目 | 24 个：postgres、redis、sqlite、mysql-server、langchain、llama-index、haystack、lightrag、graphrag、ragflow、ragas、dify、langgraph、autogen、crewai、metagpt、openai-agents、pydantic-ai、smolagents、mcp-python-sdk、browser-use、openhands、goose、open-interpreter | 24/270 项目收集完成 |
| 已完成机制页 | 1094 篇（368 + langgraph 40 + autogen 39 + crewai 43 + metagpt 31 + openai-agents 35 + pydantic-ai 39 + smolagents 30 + mcp-python-sdk 29 + browser-use 25 + openhands 23 + goose 36 + open-interpreter 41 + langchain 37 + llama-index 33 + haystack 35 + lightrag 42 + graphrag 31 + ragas 35 + ragflow 49 + dify 53） | 逐项目盘点后汇总，不设外推总量 |
| 已完成概览页 | 24 篇 | 每个完成项目 1 篇 |
| wiki-rag 已入库 | 仅 postgres | 现有八库全部入库，然后每批新项目增量入库 |
| `ai/agent` / `ai/rag` / `ai/llm` 机制语料 | 726 页（langgraph 40 + autogen 39 + crewai 43 + metagpt 31 + openai-agents 35 + pydantic-ai 39 + smolagents 30 + mcp-python-sdk 29 + browser-use 25 + openhands 23 + goose 36 + open-interpreter 41 + langchain 37 + llama-index 33 + haystack 35 + lightrag 42 + graphrag 31 + ragas 35 + ragflow 49 + dify 53） | 全类别纳入，按阶段门槛推进 |

当前 152 项按 `model` 分类的分布：

| 分类 | 项数 | 分类 | 项数 |
|---|---:|---|---:|
| relational | 7 | key-value | 9 |
| columnar | 8 | document | 7 |
| graph | 7 | search | 8 |
| stream | 7 | time-series | 8 |
| vector | 9 | distributed-sql | 6 |
| embedded | 4 | extension | 11 |
| benchmark | 5 | ai/agent | 23 |
| ai/rag | 21 | ai/llm | 12 |

**数据库分类小计 96，AI 小计 56，合计 152。** 设计文档中旧的「15 个分类」是历史口径；以当前 catalog 的 16 个分类键为执行口径。若扩充时决定增加分类键，必须先在本文件登记分类定义与目标分配，再写入 catalog。

### 1.3 项目名、源码锚点和质量边界

1. **项目键与 wiki 目录 slug 分离记录。** 已存在的映射：catalog `mysql` → wiki `mysql-server`；catalog `sqlite3` → wiki `sqlite`。数据库项目以后的 repo/root、manifest `project`、catalog 名和 overview `stars_key` 要在扩容/盘点登记中使用同一张 alias 表，不能靠人记忆猜。
2. **源码版本可追溯。** 每个项目规格必须记录上游仓库 URL 与完整源码 commit SHA（tarball 等非 Git checkout 还要记录下载 URL、校验信息与限制）；修订 refs 或重新生成页面时使用同一锚点，版本升级须登记差异。
3. **refs 门禁证明有限。** 现有 `wiki_refs.py` 是文件存在 + 符号文本词边界命中，不证明命中的是定义，也不证明符号说明了该机制。定义位置和机制边界必须通过源码审阅；AI / Python 试点不得把文本命中宣称为完整语义校验。
4. **写作验收**沿用 `wiki-schema.md`：六个必含章节、条件章节标题、路径 + 符号引用（不写行号）、至少两个外部参考、项目内 feature 唯一；不降低门禁换通过率。
5. **CI 扩展方式**按项目矩阵隔离，不把 270 个项目一次性全量克隆塞进一个 job；新矩阵的源码 pin、缓存键、超时和失败可见性必须可审计。
6. **清单数据不编造。** 未取得的 stars / license / language 必须显式留阻塞或排除，不能用占位值冒充已核实。扩充后的 tier 必须有明确口径和取数日期。
7. **RAG 索引不代替语料验收。** 先通过 lint、manifest、refs 与抽检，才允许入索引；每批索引状态与已入库页数写入台账。

---

## 2. 已完成资产与欠账

### 2.1 数据库语料基线

| 项目目录 | 机制页 | 概览页 | 当前已知验证 |
|---|---:|---:|---|
| postgres | 151 | 1 | lint / refs / manifest PASS；全项目 152 页 |
| redis | 63 | 1 | lint / refs / manifest PASS；全项目 64 页 |
| sqlite | 86 | 1 | lint / refs / manifest PASS；全项目 87 页 |
| mysql-server | 67 | 1 | lint / refs / manifest PASS；全项目 68 页；完整上游 Git 历史的获取限制见 §8 |
| **合计** | **367** | **4** | 现有四库全量门禁通过 |

已存在的生产工装：catalog 解析与填数、通用 manifest 生成、manifest/lint/refs 门禁、源码缓存、项目概览生成、wiki-rag 分库 ingest。工装不是全自动写作器：机制边界和正文仍需要源码证据与独立审阅。

### 2.2 名称别名对账（后续必须固化为机器可读映射）

| catalog 项目名 | 当前 wiki 目录 | 状态 |
|---|---|---|
| postgres | postgres | 已完成 |
| redis | redis | 已完成 |
| mysql | mysql-server | 已完成；alias |
| sqlite3 | sqlite | 已完成；alias |

**剩余风险**：当前 catalog 只有项目元数据，并没有 `wiki_status` / alias / source pin 字段；已完成项目识别依赖目录与手工映射。阶段 0A 必须建立确定性的项目登记/对账方案，避免将别名误当成缺页而重复生产，或误将同名不同仓库合并。

---

## 3. 统一生产流水线与阶段门

每个项目都按同一条流水线执行；只有当前阶段过门，才能进入下一阶段：

1. **登记**：catalog 名、canonical wiki slug、分类、tier、repo URL、license、language、stars 与日期、源码 ref、状态。
2. **取得源码**：选择完整 Git checkout、partial clone 或有明确信息边界的归档下载；失败时登记阻塞，不能默认为源码齐备。
3. **盘点定义**：建立相关源码子树与定义索引；按业务/系统机制簇识别边界，明确“负责什么、不负责什么”。
4. **产出规格**：`<project>_mechanism_specs.py`（或等效、经批准的语言无关格式）提供 `GROUP_ORDER` 与 `MECHANISM_SPECS`；各条具有唯一 feature、标题、page_file、boundary、refs。
5. **核验 manifest**：重生成与规格一致；manifest lint 全绿；逐 refs 检查文件和符号；对词边界门禁可能误判的动态语言、宏、重载等做定义级人工复核。
6. **写机制页**：按主题组分片，页级 lint/refs 通过后合并；每个主题组至少一位非作者审阅者抽检机制边界、实现证据、取舍与来源。
7. **概览与索引**：每库生成 1 篇 `_index.md`，包含所有机制页导航、真实来源信息；禁止只注册清单却没有覆盖状态。
8. **目录级验收**：该项目 lint / refs / manifest 均 0 失败；契约测试确认 manifest 页、实体页、overview 一致；抽检报告有结论。
9. **CI 与 RAG**：CI 对新项目的页面树、manifest、refs 与工具测试可运行；RAG 逐库 ingest，验证文档数量、块 ID 唯一、引用回源和问答可用。
10. **状态收口**：更新本 roadmap 台账、项目 alias/status 索引、批次报告；明确下一项目/组和启动命令。只在本步后才算完成。

**批次调度规则**：同批项目可以并行，不同 writer 不可写同一项目；按源码树/语言/机制范式分批，先暴露共性工装问题，再提升并发；一次通过率和独立抽检合格才准扩大批量。项目数量不是质量指标。

---

## 4. 全局阶段图

```text
阶段 0A：审阅 / 扩充 catalog 到 270，建立唯一项目与状态账本
  ↓
阶段 1：langgraph Python 多包仓 AI 样板，验证跨语言 wiki 流水线
  ↓
阶段 2：按范式批次完成 120 个 AI / Agent / RAG / LLM 项目的机制 wiki
  ↓
阶段 3：按存储与架构类别完成 150 个数据库 / 数据基础设施项目机制 wiki
  ↓
阶段 4：全量 270 项门禁、抽检、对比页、全局导航与 RAG 索引
  ↓
终态：270 项均有状态；所有纳入项目机制 wiki / overview / 源码引用 / CI 验收通过；全量 RAG 索引可回源
```

**顺序原则：** catalog → wiki 机制语料 → 项目验收 → RAG 全量入库。RAG 索引不再作为“项目开写前”的主线门槛；个别批次可增量索引验证，但最终全量 ingest 必须在 270 项 wiki 机制页完成后收口。现有三库的阶段 0B ingest 可在主线并行，但不得替代 270 项 wiki 收集，也不能让“先做索引演示”再次让主任务停住。

### 阶段 0A｜清单扩充与项目状态账本（先做）

**目标**：从 152 扩到设计目标 270。目标分配为数据库/数据基础设施 150（当前 96，补 54），AI 120（当前 56，补 64）。具体实现入口：[`2026-11-26-wiki-catalog-270-implementation.md`](../superpowers/plans/2026-11-26-wiki-catalog-270-implementation.md)。

**工作包**：

- 候选审阅矩阵（118 个候选已冻结，metadata 部分核验）：[`project-catalog-review.md`](project-catalog-review.md)
- 项目状态账本（270 条，机器可读）：[`wiki-project-status.yml`](wiki-project-status.yml)
- 阶段 0A 实施步骤：[`2026-11-26-wiki-catalog-270-implementation.md`](../superpowers/plans/2026-11-26-wiki-catalog-270-implementation.md)

**账本口径（2026-11-27）**：270 条全部在册；`indexed` 4 项（postgres、redis、sqlite、mysql-server，别名分别回指 catalog 的 `sqlite3`、`mysql`）；其余 266 项为 `planned`。账本由 `scripts/wiki/gen_project_status.py` 从 catalog 与候选矩阵生成，`scripts/wiki/wiki_project_status.py` 校验 schema／阶段／slug／repo／别名冲突。

- 0A.1 盘点旧候选目录和 reference 镜像清单，按 canonical repo 去重，检查 catalog 当前条目是否仍活跃、官方 repo 是否变更、分类和 tier 是否合理。**状态：已完成一轮；`apache-hawq` 归档→carbondata，`zincsearch` 归档→bleve，`hsqldb` 错误指向→derby，`consul`→memcached，`dbt`/`go-ycsb` 移出，`aphrodite-engine` 归并为 `sonar`，NeMo 重定向至语音仓→lmcache。**
- 0A.2 补 118 个候选：数据库类别缺口 54、AI 缺口 64。**状态：候选集合已冻结为 DB 54 / Agent 24 / RAG 20 / LLM 20，共 118，表内唯一且与现有 catalog 零冲突。**
- 0A.3 取数使用 `fill_stars.py`，优先 `GITHUB_TOKEN` 或 `GH_TOKEN`；若无 token，分批使用未认证速率额度并依赖缓存，不能因 60 次/小时限制而放弃台账更新。每次最多 55 条的脚本限制保持。
- 0A.4 写入机器可读项目状态/alias 登记：区分 `catalog`、`planned`、`source-ready`、`inventoried`、`pages-written`、`gates-pass`、`indexed`、`blocked`、`excluded-with-reason`；至少能按项目名查询当前阶段与下一动作。不得把项目状态塞进冻结的 wiki 页面 frontmatter；优先用独立项目注册表和本文件链接。**状态：已完成 `wiki-project-status.yml`（270 条）+ 解析／校验／生成三个脚本与两组测试。**
- 0A.5 处理 catalog 名与 canonical slug 的别名；明确每项只对应一个 canonical repo 和唯一 wiki slug；补充 CI 与 ingest 使用的 slug 映射。
- 0A.6 通过 catalog schema、重复 repo/name 检查、分类配额检查、stars/date 来源复核后锁定 270 版本；未通过核实的候选放在待审队列，不以虚假值补满 270。

**阶段出口**：catalog 恰好 270 项；数据库 150、AI 120；所有记录必填字段验证通过；每项有可追溯 repo URL、分类、tier 与 metadata 取数时间；已有四个 wiki 项目正确 alias，不生成重复目录；下一批 T1 队列可由状态账本直接导出。

**依赖 / 阻塞**：GitHub API token 有利但非绝对前提。token 缺失只降低取数吞吐；如果项目来源或 metadata 无法证实，该候选不得进入正式 270 清单。

### 阶段 0B｜现有数据库 RAG 索引补齐（并行支线，不是主线门槛）

**目标**：并行补齐现有四库的可检索基线，但该项不作为 270 项机制 wiki 主线的前置门槛，也不能替代完整项目语料收集。

- 依次运行 `python apps/wiki-rag/tools/ingest.py --project redis`、`--project sqlite`、`--project mysql-server`；postgres 不重建。
- 每库 ingest 前确保 refs 源码 checkout 与页面所依据版本一致；按数据库路由配置别名（`mysql-server` 对应 catalog `mysql`，`sqlite` 对应 catalog `sqlite3`）。
- 检查 `metadata.json` 页数、块数、门禁退出码；运行 app 冒烟脚本，检查项目列表含 4 库、问答引用块都能回源、块 ID 唯一。
- 本阶段不做 LLM 化；保持抽取式生成与后端来源引用。

**出口**：wiki-rag 可列出四库；四库各自 metadata 中 `wiki_lint=0`、`wiki_refs=0`；端到端 smoke 的引用回源缺失为 0；报告记录四库 pages / blocks。

### 阶段 1｜langgraph Python 多包仓全链路样板

**目标**：按用户选择，langgraph 是首个 AI / Agent wiki 项目；用它验证 Python、monorepo、多包、异步函数和 Agent 机制的全流程，不在完成样板后停止。

**源码事实锚点（只用于规划边界）**：上游仓为 `https://github.com/langchain-ai/langgraph`；API 现状显示它是多包仓，包含 `libs/langgraph/`、`libs/checkpoint/`、`libs/checkpoint-postgres/`、`libs/checkpoint-sqlite/`、`libs/prebuilt/`、`libs/sdk-py/`、`libs/sdk-js/` 等。每个源码 checkout 必须在正式规格中固定完整 commit SHA，不能用 `main` 浮动引用。

**工作包**：

- 1.1 固定本批源码锚点，记录 license、default branch、repo scale、packages、主要语言和 workspace 边界；明确 wiki 项目只覆盖 `langgraph` 主仓一次，`langgraph` 仓内的子包按机制职责决定是否独立页面，不把每个 package 自动当成独立项目。
- 1.2 建 Python 定义索引：现有 `dump_defs.py` 的正则能命中简单 Python `def` / 缩进 `def`，但文件说明只承诺 C/C++；先加带测试的 Python / `async def` / 类方法 / 装饰器与同名方法样例，决定是把通用脚本改名/扩展，还是新增 `dump_python_defs.py`。定义索引要能给出文件路径和行，不能把简单词命中当定义证据。
- 1.3 检查 refs 校验语言支持：现有 `wiki_refs.CODE_EXTS` 包含 `.py` / `.ts`，但缺 `.tsx` / `.jsx`，应加测试并补齐需要的扩展名；测试 Unicode、下划线和 Python 方法名。不能假定 TSX 覆盖所有项目，也不能对 Python 文件要求错误的 `src/` 前缀。
- 1.4 扩展通用 manifest 生成与源码路径校验，让 Python monorepo 可用 `libs/langgraph/langgraph/...` 等真实仓库相对路径；把源码根 / 允许路径前缀从单纯 `src-prefix` 参数提升为可复用 layout 配置或经过测试的多前缀配置。验证仓外路径逃逸、路径穿越与空前缀失败。
- 1.5 盘点机制簇并双人审阅：graph construction / state schema & reducers / Pregel execution / branching & scheduling / checkpoint & persistence / interrupts & resume / streaming / subgraphs / retry & error handling / functional API / prebuilt agents & tool execution / remote runtime & SDK；最终机制数量以源码盘点为准，不承诺此清单每项必成独立页。
- 1.6 为每条机制建立 `boundary` 与同路径真实定义符号 refs；从每个子包挑引用代表页，抽检定义、调用关系及版本锚点；以官方文档、源码、设计/论文作为参考来源。
- 1.7 完成 langgraph manifest、机制正文、overview、独立审阅、目录门禁、项目级 refs、契约一致性测试和 CI matrix 项。
- 1.8 `wiki-rag` ingest langgraph；用涉及 durable execution、checkpoint、interrupt 等的问题验证检索与引用回源；只在整条链路通过后才关闭样板。

**出口**：langgraph 在项目注册表从 `planned` 到 `indexed`；manifest 与页面一一对应；项目目录 lint / refs / manifest 0 失败；抽检报告 PASS；CI 对 langgraph 可独立运行；RAG 数据可追溯回该版本原文。

**进度（2026-11-27）**：1.1–1.7 已完成。源码锚点 `3e3db42c5329dd404669ccd923c005c814bee3a3`（`langgraph` 主包，`libs/langgraph`）；新增 `scripts/wiki/dump_python_defs.py`（`ast` 解析，覆盖 `async def`、类方法、嵌套定义）与 8 项测试；`gen_manifest.py` 用 `--src-prefix libs` 支持 monorepo 路径；机制清单 `scripts/wiki/langgraph_mechanism_specs.py` 盘点出 **9 组 40 篇**（A 图构建 4 / B 通道 6 / C Pregel 循环 7 / D 调度并发 3 / E 检查点 4 / F 通道读写 4 / G 流式 4 / H 中断超时校验 4 / I 运行时上下文 4）；40 篇正文 + `_index.md` 已写完，逐篇 `wiki_lint` 与 `wiki_refs` 全 PASS，全目录 41 页 0 失败，manifest 门禁 0 失败。**1.8（RAG ingest）尚未开始**，因此账本状态为 `gates-pass` 而非 `indexed`。

### 阶段 2｜AI / Agent / RAG / LLM 120 项全覆盖

**目标**：为全部 270 项（数据库 150、AI 120）完成统一流水线。逐库机制 wiki 总篇数等于盘点结果之和，不设固定估算。成功完成“270 项 wiki 收集”要求每个批准纳入项目都有机制页、overview、源码引用与 CI 验收通过；无法完成的要保留可核验证据与明确 `blocked` / `excluded-with-reason` 状态，不能把它们算成已完成，也不能以一个试点代替余下项目。全量 RAG 入库是语料验收之后的闭环，不是 wiki 主任务的替代物。

**顺序按技术范式，不按单纯 star 排名。langgraph 是范式样板，不是唯一 AI 项目。**

| 波次 | 范式 | 当前 catalog 中的示例（非新增清单） | 波次出口 |
|---|---|---|---|
| 2A | Agent 图执行与状态机 | **已完成（收官）langgraph 40、autogen 39、crewai 43、metagpt 31、openai-agents 35、pydantic-ai 39，共 227 篇** | 比较图 / 消息 / 团队 / SOP / 能力系统等机制；页数由逐库盘点决定 |
| 2B | Agent 工具使用与软件工程 Agent | **已收官**：smolagents（30 篇）、mcp-python-sdk（29 篇）、browser-use（25 篇）、openhands（23 篇）、goose（36 篇）、open-interpreter（41 篇），共 184 篇 | 工具调用、沙箱 / 浏览器 / MCP 边界有源码证据；不按产品功能列表切页 |
| 2C | RAG / 索引编排框架 | **已收官：langchain（37 篇）、llama-index（33 篇）、haystack（35 篇）、lightrag（42 篇）、graphrag（31 篇）、ragas（35 篇）、ragflow（49 篇）、dify（53 篇）** | ingestion、chunk、retrieval、rerank、evaluation 与应用编排分别建机制图 |
| 2D | 面向应用的 RAG 产品与知识平台 | open-webui、anythingllm、fastgpt、private-gpt、qanything、kotaemon、onyx、r2r、verba 等 | 项目间避免把相同功能词误当不同机制；保留各自真实架构差异 |
| 2E | 开源模型运行时与本地推理 | llama.cpp、ollama、vllm、sglang、lmdeploy、tgi | 模型加载、内存布局、批处理、缓存、调度、量化与 API 服务链路 |
| 2F | 训练 / 优化 / 推理 kernel 基础设施 | deepspeed、megatron-lm、llama-factory、transformer-engine、triton-server 等 | 区分训练并行、内存优化、kernel / engine 和 serving；不把库 README 当机制页 |
| 2G | AI 全局对比与索引复核 | 各 AI 波次 | 只比较已通过独立页的真实机制；`_compare/` 是跨项目页，仍需符合 schema / refs 规则 |

每个波次先用 1–2 个已登记项目验证方法，再依据指标扩大到 4–8 个不同项目；通过率门槛不等于省略复核。停止条件：同类系统性 refs 错误达到 3 个、抽检机制证据不足、源码 checkout 不可靠或项目归类出现争议时，暂停相关范式并修正工装/设计。

**出口**：120 个 AI catalog 项目各自有 `indexed`、`blocked`、`excluded-with-reason` 明确状态；所有 `indexed` 项目过项目门禁、审阅和 RAG 引用回源；对阻塞项有具体负责人 / 外部条件 / 复查条件；不会因 langgraph 样板结束就把 AI 轨记成完成。

### 阶段 3｜数据库与数据基础设施剩余项目全覆盖

**目标**：当前数据库类 96 项 + 阶段 0A 新增 54 项 = 150 项。每一个项目都要完成机制 wiki 和验收；不能以“状态已登记”代替文章和页面完成。现有四库保留，不重复写。

按共性与仓库形态分波次，每个波次先查目录结构/语言/源码获取难点：

| 波次 | 范式簇 | 当前已登记项目示例 |
|---|---|---|
| 3A | 关系型 / 嵌入式 SQL | mysql-family、DuckDB、Firebird、MariaDB、H2 等；已完成 postgres / mysql-server / sqlite 不重复写 |
| 3B | 分布式 SQL / HTAP / 调度 | CockroachDB、TiDB、YugabyteDB、OceanBase、Trino、Presto 等 |
| 3C | 键值、嵌入式存储与 LSM | Redis、RocksDB、FoundationDB、etcd、LMDB、Badger、Dragonfly 等；redis 已完成 |
| 3D | 列式、湖仓与数据框架 | ClickHouse、Doris、StarRocks、DataFusion、Delta Lake、Iceberg、Arrow 等 |
| 3E | 图数据库 / 图执行 | Neo4j、ArangoDB、Memgraph、NebulaGraph、Kuzu 等 |
| 3F | 搜索、时序与流处理 | Elasticsearch、OpenSearch、Kafka、Flink、NATS、InfluxDB、Prometheus 等 |
| 3G | 向量索引与向量数据库 | FAISS、Milvus、Qdrant、Chroma、Weaviate、Vespa、LanceDB 等 |
| 3H | 扩展、接口与基准 | pgvector、HNSWlib、DuckDB extensions、ANN-Benchmarks、VectorDBBench 等 |

**出口**：数据库 / data infra 的 150 项全部有状态；已入库项目都具备固定源码锚点、机制规格、manifest、概览、门禁、抽检、CI 和 RAG 状态；未覆盖项须明示阻塞/排除，不藏在估算数字里。

### 阶段 4｜全量门禁、对比、导航与 RAG 闭环

- 阶段 2/3 的所有批准纳入项目机制 wiki 与 overview 均已完成，所有项目的 manifest / lint / refs / 独立抽检与 CI 验收通过；阻塞 / 排除项明列证据，不计为已写完。
- 为有直接对照意义的项目组建立 `_compare/` 机制对照：至少包含 Agent 范式、RAG pipeline、LLM runtime/serving、关系型 / 分布式 SQL、向量检索、流处理等。
- 对比页必须基于已验收的项目页与源码锚点；列清比较维度、边界和版本，不能复制 overview 作为对比。
- 生成全局导航：按 model 分类、tier、机制组链接到项目 overview；明确数量统计只计去重 canonical repo，不将 alias 重算。
- 最后对所有已验收项目一次性全量 ingest wiki-rag，检查项目筛选、块 ID 唯一、引用回源、无结果 abstain 与对比问答。
- 最终报告逐项核对 270 catalog：`indexed` / `blocked` / `excluded-with-reason` 总和必须为 270；数据库 150、AI 120 分项准确；`blocked` 和 `excluded` 不能统计为已完成 wiki。

---

## 5. 每个项目的交付契约

每个项目（canonical repo）最小交付集合：

```text
docs/wiki/projects/<canonical-slug>/
├── _manifest.yml             # 机制清单（机制正文唯一生成输入）
├── _index.md                 # 项目概览与 manifest 全量导航
├── <group-prefix>-*.md       # 盘点出的机制页
└── （可选）_compare/         # 仅用于跨项目比较，不混入单库 manifest

scripts/wiki/<project>_mechanism_specs.py  # 固定版本的机制规格
projects/.cache/repos/<repo-key>/          # 本地缓存，不入库
```

进入 `indexed` 的验收条件：

- catalog/alias/slug 关系唯一、repo URL 可访问、源码锚点完整。
- `gen_manifest.py` 或已认可替代工具成功生成 manifest，refs 文件和符号正确。
- `wiki_manifest_lint.py`、该项目 `wiki_lint.py`、该项目 `wiki_refs.py` 都 exit 0。
- 所有页面 frontmatter 与 manifest 的 `project`、`feature`、`refs` 一致；每条 feature 在项目范围内唯一。
- 独立抽检覆盖所有机制组；每页可说明机制边界、核心实现链路、关键取舍和引用证据；不以只跑门禁代替内容审阅。
- `_index.md` 元数据与 catalog 中的 stars/date、license、language 对齐，不使用缺失占位值假装通过。
- CI 能在该项目的 source checkout 上执行 refs；按项目/语言设定缓存与超时，单项目失败可识别。
- RAG ingest 后 pages 与目录页数相符；块 ID 无重复；实际问答引用均能回到块库原文。

一个项目若因上游删除/重命名、禁止获取、不可复现源码、metadata 不可核验等无法完成，要登记为 `blocked` 或 `excluded-with-reason`，写明证据与复查时间。**这不等于完成，也不允许把整个波次静默跳过。**

---

## 6. 阶段启动与收尾检查表

### 开始一批时

1. 打开本文件「当前状态」和「下一步」，只从那里确定启动项目/任务。
2. 查询项目状态账本；确认项目没被标记为 `indexed`，查清 alias 与 canonical repo。
3. 确认目标 checkout 的准确绝对路径、source ref、源码缓存归属；所有临时产物列出准确位置和清理时点，依仓库临时文件规则执行。
4. 检查 `git status --short`，不得覆盖或暂存无关改动；写入前建立具体文件作用域。
5. 为这一波列出具体目标、文件范围、门禁和停止条件；同项目只允许一个 writer 修改。

### 完成一批时

1. 运行该项目 lint、manifest、refs、契约和抽检；保留命令与结果。
2. 如改动 wiki 工装，运行 `py -3 -m unittest discover -s scripts/wiki -p "test_*.py"`。
3. 将通过门禁的项目 ingest RAG，检查索引元数据、块唯一和引用回源。
4. 更新本路线图的状态与台账，写明 repo SHA、页数、块数、审阅、门禁、CI run、RAG 状态。
5. 明确写入**唯一下一步**：批次 ID、项目 slug、从哪一步开始、准确命令或需要的用户条件。
6. 确认临时目录清理、`git status --short`、`git ls-files -d`、`python scripts/audit_workspace.py`；每个 WARN 有处置或解释。
7. 按仓库纪律提交；每个 commit 后立即 `git push origin main`。推送前只 stage 本任务路径。

---

## 7. 批次台账（每批追加，不覆盖已完成记录）

| 批次 | 阶段 | 项目 / 范围 | 状态 | 源码 ref | 机制页 | lint / refs / manifest | 抽检 | CI | RAG | 备注 / 下一个动作 |
|---|---|---|---|---|---:|---|---|---|---|---|
| DB-BASE | 基线 | postgres、redis、sqlite、mysql-server | 完成 | 各库规格模块与 P4 记录 | 367 | 四库 PASS；全树 371 页 0 fail | 见 P3/P4 报告 | wiki workflow 已接入四库 | 仅 postgres 已入库，另外三库待 0B | 先完成 0A 清单，再开始 0B |
| AI-LG-01 | 阶段 1 | langgraph | 未开始 | 待阶段 1.1 固定 SHA | — | — | — | — | — | 0B 完成后开工：先定锚点、盘点多包结构、加 Python 定义索引测试 |
| CAT-270 | 阶段 0A | 数据库/AI catalog 扩容与 alias 状态表 | 未开始 | N/A | N/A | schema / duplicate / quota 检查待跑 | 候选评审待做 | CI 校验待接 | 不适用 | 当前 152 项；数据库 +54、AI +64 |
| RAG-DB-4 | 阶段 0B | redis、sqlite、mysql-server ingest | 未开始 | 复用各项目冻结源码锚点 | 214 | 页面门禁已过，索引门禁待跑 | 已有抽检记录 | 不适用 | postgres only → 目标四库 | 按命令逐库 `tools/ingest.py --project <slug>`，每库验证后再继续 |

> 台账中 367 是机制页数；全项目 371 是 lint 覆盖数（367 机制页 + 4 概览页）。不同数字有不同口径，更新时勿混用。

---

## 8. 风险、限制与明确非目标

| 风险 / 限制 | 影响 | 处置规则 |
|---|---|---|
| catalog 扩容历史中断在 152 / 270 | 缺 118 项；原 P4 首批三库完成后未排后续批次 | 阶段 0A 一次建立 270 目标清单和后续状态台账，避免再变成临时试点 |
| 现有 `wiki_refs.py` 只做词边界命中 | 动态语言会把注释、调用点当定义 | 用定义索引 + 审阅补足；必要时分语言写具体 tests，不将 refs exit 0 等价于机制语义正确 |
| AI / Python monorepo 的包树与生成器尚未经过真正样板 | 现有 C/C++ 经验可能不适用 | langgraph 阶段先验证多包路径、类/异步/装饰器定义与超长源码文件 |
| TSX / JSX 扩展覆盖有缺口 | TS agent 项目相关 `.tsx` / `.jsx` 文件可能漏扫 | 阶段 1 加扩展回归测试；按真实项目树确认语言覆盖，不做无测试正则扩张 |
| GitHub 未认证 API 有速率限制 | 118 个扩展项目 metadata 需分批实取 | 优先设置 `GITHUB_TOKEN` 或 `GH_TOKEN`；本机当前检查未发现两者，申请/注入属于用户环境动作；缓存断点续跑 |
| MySQL 完整 Git 历史尚未取回 | 当前 refs 可按已有全量源码树核验，无法证明任意历史版本 | 继续标明 tarball/本地仓边界；网络恢复时独立补历史，不阻塞已声明版本的页面 |
| `wiki-rag` 数据目录当前只有 postgres | 用户无法在演示应用查另外三库 | 阶段 0B 最先补齐，不等 AI 页面 |
| 清单名称 / 目录别名映射是手工配置 | 重复生产或 overview 元数据对错仓 | 阶段 0A 纳入机器可读 alias/status 表与冲突测试 |
| 所有项目的 CI checkout 资源成本 | 270 项 full clone 的 CI 耗时与缓存膨胀 | 按项目分层矩阵、限制并发、partial clone/cache、明确 source pins；避免每次页面修改都重新下载所有仓 |
| “270”并不自动保证“业内优秀” | 目录数可能有重复、过时或代表性弱的项目 | 每个候选须有分类/代表性/tier 理由；审阅阶段允许明确淘汰并补入经核实候选，绝不为数量留死仓 |
| 多库机制页总量未知 | 无法对页数、token 或完成日期给精确承诺 | 报告盘点结果和工作量区间，不能把旧 11,000 页外推当承诺 |

**不在这条 wiki 生产线中直接实现的事情**：不做 LLM 生成内容流水线替代机制审阅；不因 catalog 达 270 自动声称所有项目内容完成；不将 wiki-rag 接入数据库主核；不把抽取式 RAG 的工作冒充为 LLM 问答质量验收。

AI Infra 学习轨（`learning/units/ai/` + `apps/study-hub/`）是另一个 deliverable：共享 AI 领域和部分来源，但**不混入这 270 个源码 wiki 项目/机制页计数**。其 P0 状态应沿其自己的计划维护，在本 wiki 完成阶段 1 后建立清晰交叉链接即可。

---

## 9. 相关资料与执行入口

- wiki 契约：`docs/wiki/wiki-schema.md`
- 当前 catalog：`docs/wiki/catalog-projects.yml`
- 四库 P4 进度：`docs/wiki/p4-progress.md`
- 工装实现计划：`../superpowers/plans/2026-11-22-multimodal-wiki-p0-toolbox.md`
- 整体设计与 270 目标：`../../../ai-docs/methodology/planning/2026-11-22-multimodal-wiki-and-rag-design.md`
- catalog 与 manifest 计划：`../../../ai-docs/methodology/planning/2026-11-22-multimodal-wiki-p1-catalog-and-manifest.md`
- 多库流水线计划：`../../../ai-docs/methodology/planning/2026-11-23-multimodal-wiki-p4-other-libraries.md`
- RAG 应用计划：`../../../ai-docs/methodology/planning/2026-11-23-multimodal-wiki-p5-rag-app.md`
- AI Infra 学习轨（独立任务）：`../superpowers/plans/2026-11-26-ai-infra-learning-track-p0.md`
- 本地 wiki 工装测试：`py -3 -m unittest discover -s scripts/wiki -p "test_*.py"`
- 全树结构门禁：`py -3 scripts/wiki/wiki_lint.py docs/wiki/projects --quiet`
- manifest 门禁：`py -3 scripts/wiki/wiki_manifest_lint.py docs/wiki/projects/<slug>/_manifest.yml`
- 项目 refs 门禁：`py -3 scripts/wiki/wiki_refs.py --repo-root .cache/repos/<repo-key> docs/wiki/projects/<slug> --quiet`
- 项目 RAG ingest：`py -3 apps/wiki-rag/tools/ingest.py --project <slug>`

> **执行约定**：本文件记录全局阶段与下一步；每个实现阶段的具体修改、文件作用域、测试与提交拆成独立实现计划 / PR。不要把清单扩容、manifest 生成器、所有机制页、CI 与 RAG 大改揉成一个无法审阅的大提交。
