# 阶段 0A 实现计划：catalog 扩容到 270 并建立全局项目状态账本

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. 本计划按阶段 0A 的独立交付执行；后续阶段仍以 `docs/wiki/project-roadmap.md` 为唯一总路线入口。

**Goal:** 将 wiki 项目 catalog 从 152 项扩充到经审阅、真实 metadata 核实的 270 项（数据库 / 数据基础设施 150 项、AI 120 项），并建立 canonical repo、wiki slug、alias、源码锚点、阶段状态和唯一下一动作的机器可读账本。该计划只是全局主任务的阶段 0A，绝不代表“270 项 wiki 收集”已完成；最终完成必须在总路线中逐项完成全部项目的机制 wiki 和验收。

**Architecture:** 保留 `catalog-projects.yml` 作为项目发现与分类清单，新增 `wiki-project-status.yml` 作为生产状态及名称映射来源；实现标准库解析 / 校验 / 报告 CLI。候选项目先做代表性与重复审阅，再用已有 `fill_stars.py` 按缓存和每轮最多 55 次 API 请求取得真实仓库元数据；任何无法核实的条目不得伪装成完成。

**Tech Stack:** Python 3 标准库、现有 `wiki_catalog.py` / `fill_stars.py` / `catalog_additions.py`、GitHub REST API、`unittest`；保持无 YAML 第三方依赖。

## Global Constraints

- catalog 的 8 个字段保持现契约：`name` / `model` / `tier` / `repo` / `license` / `language` / `stars` / `stars_as_of`。
- 270 项目标分配固定为数据库与数据基础设施 150 项、AI 120 项；当前 96 + 56，缺口分别 54、64。
- 每一条新项目必须核实 canonical repo、分类、tier 理由、stars、license、language 和取数时间；不编造数据、不使用 `1970-01-01` 占位通过。
- 按 canonical repository 去重；catalog 名、canonical wiki slug、上游 repo key 与 aliases 必须显式登记。
- 区分项目阶段 `catalog` / `planned` / `source-ready` / `inventoried` / `pages-written` / `gates-pass` / `indexed` / `blocked` / `excluded-with-reason`；blocked 与 excluded 不得算完成。
- 修改前检查 `git status --short`；不要暂存或覆盖当前无关工作；提交只包含本计划文件范围。
- 每个 commit 后立即 `git push origin main`。不自动 stage 无关工作，不自动修改其它仓。
- 任何临时下载、候选文件和 API 响应如需落盘，只能置于任务专用 `%TEMP%\code-wiki-catalog-270\`，完毕清理并确认不存在；不把临时物放入仓库。
- 完成前运行目标测试、catalog 校验、`git status --short`、`git ls-files -d` 与 `python scripts/audit_workspace.py`；对 WARN 要处置或解释。

---

## Files and Responsibilities

| 文件 | 操作 | 职责 |
|---|---|---|
| `scripts/wiki/catalog_additions.py` | 修改或替换 | 只保存经审阅、真实取数的新增候选；不得把候选仓库 metadata 填成推测值 |
| `scripts/wiki/apply_catalog_additions.py` | 新建 | 读取现 catalog 与 additions，经校验后幂等合并；支持 `--dry-run`，仅在全量校验通过后写 catalog |
| `scripts/wiki/wiki_project_status.py` | 新建 | 解析 / 验证项目状态表，检查 canonical slug、aliases 与 repo 冲突，输出项目启动信息和待办列表 |
| `scripts/wiki/test_catalog_additions.py` | 修改 | 测试扩容候选完整性、repo 去重、真实 metadata 日期和值、existing 优先行为 |
| `scripts/wiki/test_apply_catalog_additions.py` | 新建 | 测试 additions 合并幂等、dry-run 不写、失败原子性、排序与 270 / 分类配额校验 |
| `scripts/wiki/test_wiki_project_status.py` | 新建 | 测试状态表 schema、别名冲突、canonical repo 重复、下一动作字段与状态转换合法性 |
| `scripts/wiki/wiki_project_status.py` | 新建 | CLI：`validate` / `next` / `show <slug>`，为下一阶段提供不靠记忆的执行入口 |
| `docs/wiki/catalog-projects.yml` | 修改 | 只写入全部通过 metadata / 分类 / 去重校验的 270 条正式条目 |
| `docs/wiki/wiki-project-status.yml` | 新建 | 270 项逐条生产状态、canonical slug、repo key、wiki state、source ref、阻塞原因、下一动作、alias |
| `docs/wiki/project-roadmap.md` | 修改 | 更新当前状态、阶段 0A 出口、下一步固定为阶段 0B 与阶段 1 的确定顺序 |
| `docs/wiki/project-catalog-review.md` | 新建 | 记录原 152 项审阅、118 个候选纳入/拒绝理由、每个分类配额核验及来源日期 |
| `.github/workflows/wiki.yml` | 后续修改（本任务中在校验 CLI 成型后接入） | 增加 catalog/status 门禁，不执行 GitHub API 请求 |

> `.github/workflows/wiki.yml` 如果需改，必须新增纯本地校验命令；绝不让每次 CI run 查询 stars 或依赖外部 rate limit。

---

## Task 1：冻结当前输入与候选差额

**Files:**
- Read: `docs/wiki/catalog-projects.yml`
- Read: `docs/wiki/project-roadmap.md`
- Test: 现有 `scripts/wiki/test_wiki_catalog.py`

**进度（2026-11-27）**：完整 catalog 基线 152 项（DB 96 / AI 56）已解析；别名 `mysql` → `mysql-server`、`sqlite3` → `sqlite` 已登记；候选集合按用户确认的广义基础设施范围冻结为 **DB 54 / Agent 24 / RAG 20 / LLM 20 = 118**，表内 name 与规范化 repo URL 均唯一，且与当前 catalog 零 name/repo 冲突。已完成的替换处置：HAWQ 归档→carbondata、ZincSearch 归档→bleve、HSQLDB 错误指向→derby、consul→memcached、dbt/go-ycsb 移出、aphrodite-engine 归并 sonar、NeMo 重定向语音仓→lmcache。约 20 条已取得 GitHub API 事实记录；其余待逐条取数。**Task 1 的候选部分已完成，Task 2 的整批 metadata 尚未完成。** 见 [`project-catalog-review.md`](../../wiki/project-catalog-review.md)。

- [ ] **Step 1: 记录基线、检查脏工作树**

Run:

```powershell
cd C:\code\projects
git status --short
py -3 -c "import sys; sys.path.insert(0, 'scripts/wiki'); from wiki_catalog import parse_catalog; p=parse_catalog(open('docs/wiki/catalog-projects.yml', encoding='utf-8').read()); print('total=',len(p)); print('db=',sum(x['model'] not in ('ai/agent','ai/rag','ai/llm') for x in p)); print('ai=',sum(x['model'] in ('ai/agent','ai/rag','ai/llm') for x in p))"
```

Expected: `total=152` / `db=96` / `ai=56`。记录未跟踪或修改文件，不在本任务内处理。

- [ ] **Step 2: 检查现有 catalog schema 测试**

Run: `py -3 -m unittest discover -s scripts/wiki -p "test_wiki_catalog.py" -v`

Expected: 全部通过；若基线失败，停止扩容，先报告既有失败。

- [ ] **Step 3: 建立候选审阅矩阵，不调用网络**

创建 `docs/wiki/project-catalog-review.md`，逐分类列出：目标数、现有数、缺口、候选名、canonical repo、tier 依据、与现有条目去重结果、候选处置及评审备注。数据库类与 AI 类分别 subtotal；把数据库已完成别名 `mysql-server` ↔ `mysql`、`sqlite` ↔ `sqlite3` 固定记录。

Expected: 候选数恰好 118（DB 54 / AI 64）；分类明细加总等于各自缺口；候选只有名称、repo 和分类等已知事实，尚未被标成正式 catalog 条目。

- [ ] **Step 4: 审阅候选而非仅从旧清单机械复制**

逐项核实：官方 repo 是否 canonical、是否存续、是否属于该类别、是否与现有 canonical repo 重复、tier 是否符合已批准定义。若候选被拒绝，写理由并找经审阅的替补，保持 54/64 的分类总缺口；不得默默减少目标。

Expected: `project-catalog-review.md` 所有候选都有明确 `include` / `exclude` 结论和依据；无未解释重复。

- [ ] **Step 5: 提交候选审阅矩阵的独立 checkpoint**

```powershell
git add docs/wiki/project-catalog-review.md
git commit -m "docs(wiki): 建立 270 项项目清单扩充审阅矩阵"
git push origin main
```

只 stage 该文件，不得包括已有未跟踪工作。

---

## Task 2：取得 118 个候选的真实仓库 metadata

**Files:**
- Modify: `scripts/wiki/catalog_additions.py`
- Use: `scripts/wiki/fill_stars.py`
- Test: `scripts/wiki/test_catalog_additions.py`

- [ ] **Step 1: 写 metadata 真实性测试**

扩展 `test_catalog_additions.py`，对正式 ADDITIONS 断言：条目数等于 118；每条恰好有 8 个 schema 字段；repo 唯一；stars 是非负整数（允许真实 0）；language / license 不是空占位（若 GitHub 返回 null，按明确可审计的 `NOASSERTION` / `unknown` 规则登记并标来源，不得把缺失与真实值混淆）；`stars_as_of` 为实际抓取日期；model/tier 都在已批准枚举内；与现有 catalog 名称和 canonical repo 均无冲突。

Expected: 现有不足 118 条时此测试先 FAIL，待数据取完后才可变 PASS。

- [ ] **Step 2: 准备认证与分批计划**

Run:

```powershell
Get-ChildItem Env:GITHUB_TOKEN,Env:GH_TOKEN -ErrorAction SilentlyContinue | Select-Object Name
py -3 scripts/wiki/fill_stars.py --dry-run
```

若使用 token，不输出 token 值。脚本上限为每轮 55 次；每轮记录开始 / 结束时间、成功、失败、剩余候选和缓存路径。token 未设置时允许低于上限运行，但遇 rate limit 立即保存进度、下次重试；不得为了满配额反复试错。

- [ ] **Step 3: 用 `fill_stars.py` 取数并审阅异常**

按已批准候选分两轮取数（最多 55 + 55；若从未取数、但 catalog 缓存中已含有有效候选缓存，可用缓存降低 API 次数）。每条保存 GitHub REST `GET /repos/{owner}/{repo}` 的真实 `stargazers_count`、`language`、`license.spdx_id`、规范 `html_url` 与抓取日期。审核 archive / 404 / moved / license null / language null；旧缓存记录需核查其 owner/repo 与当前 canonical URL 对齐。

Expected: 每个正式候选有足以重现的 API 状态记录；不可取得 metadata 的项目不能被塞入正式 270，须替换为另一条经审阅候选；API 失败时不写假值。

- [ ] **Step 4: 将取数记录写入 `catalog_additions.py`**

`ADDITIONS` 每个条目为八字段 dict；来源注释放在模块文档或 review 表，记录 API endpoint、抓取日和失败替换关系。确保模型分类只用已登记键，AI 细分为 `ai/agent` / `ai/rag` / `ai/llm`。

- [ ] **Step 5: 测试所有 additions 数据**

Run: `py -3 -m unittest discover -s scripts/wiki -p "test_catalog_additions.py" -v`

Expected: 所有候选 schema 合格；数据库新增 54、AI 新增 64，合计 118；无正式候选留 1970 日期、空 repo 或重复 canonical repo。

- [ ] **Step 6: 记录取数 checkpoint**

```powershell
git add scripts/wiki/catalog_additions.py scripts/wiki/test_catalog_additions.py docs/wiki/project-catalog-review.md
git commit -m "data(wiki): 核实 118 个项目候选的仓库与元数据"
git push origin main
```

如果取数和审阅跨多个自然日，按轮次拆分提交、每次提交后立即推送，并持续更新 review 表。

---

## Task 3：实现 catalog 合并与 270 配额校验 CLI

**Files:**
- Create: `scripts/wiki/apply_catalog_additions.py`
- Create: `scripts/wiki/test_apply_catalog_additions.py`
- Modify: `scripts/wiki/wiki_catalog.py`（仅在需要 canonical repo/category/tier 全表校验时）

- [ ] **Step 1: 写合并器失败测试**

使用临时目录与临时 catalog 字符串（仅在测试夹具创建，不提交临时文件）覆盖：

- additions 空列表时无变化；重复运行相同 additions 结果一致；existing 条目优先。
- name 重复但 repo 不同、repo 重复但 name 不同均阻止写入。
- catalog 结构非法、schema 缺字段、stars/date 非法、model 未登记、分类配额不正确时，目标文件字节保持不变。
- `--dry-run` 输出合并统计但不写文件。
- 仅当总数恰好 270、数据库类 150、AI 类 120、且每类细分配额满足 `project-catalog-review.md` 中冻结的分类表时允许正式写入。
- 稳定排序不得因 locale 或文件系统遍历顺序改变。

- [ ] **Step 2: 运行单测确认失败**

Run: `py -3 -m unittest discover -s scripts/wiki -p "test_apply_catalog_additions.py" -v`

Expected: 因 `apply_catalog_additions.py` 尚不存在而失败。

- [ ] **Step 3: 实现只读 dry-run 与原子写入**

导出：

```python
merge_catalog(existing: list[dict], additions: list[dict]) -> list[dict]
validate_catalog_target(entries: list[dict], expected_total=270) -> list[str]
```

CLI：`py -3 scripts/wiki/apply_catalog_additions.py --dry-run` 与 `--write`。默认仅 dry-run；`--write` 前先完整解析、校验、检查重复 name/repo、检查固定配额，全部通过后写同目录临时文件再原子替换 catalog。写入失败时保留原文件。工具不调用网络，API 抓取只由 fill_stars 负责。

- [ ] **Step 4: 运行合并器测试**

Run: `py -3 -m unittest discover -s scripts/wiki -p "test_apply_catalog_additions.py" -v`

Expected: PASS，失败场景不改变原文件。

- [ ] **Step 5: 运行现有 catalog 与 additions 测试**

Run: `py -3 -m unittest discover -s scripts/wiki -p "test_*catalog*.py" -v`

Expected: 所有旧契约与新合并行为一致。

- [ ] **Step 6: 提交工具与测试**

```powershell
git add scripts/wiki/apply_catalog_additions.py scripts/wiki/test_apply_catalog_additions.py scripts/wiki/wiki_catalog.py
git commit -m "feat(wiki): 增加 catalog 幂等合并与 270 项配额校验"
git push origin main
```

若 `wiki_catalog.py` 未修改，不将其加入 stage。

---

## Task 4：建立 270 项状态表、别名映射与下一步 CLI

**Files:**
- Create: `scripts/wiki/wiki_project_status.py`
- Create: `scripts/wiki/test_wiki_project_status.py`
- Create: `docs/wiki/wiki-project-status.yml`
- Modify: `docs/wiki/catalog-projects.yml`（catalog 合并后读取生成状态表）
- Modify: `docs/wiki/project-roadmap.md`

- [ ] **Step 1: 编写状态模型测试**

最低字段：`catalog_name`、`canonical_slug`、`repo_key`、`repo_url`、`model`、`tier`、`status`、`source_ref`、`aliases`、`blocked_reason`、`next_action`。状态枚举：`catalog` / `planned` / `source-ready` / `inventoried` / `pages-written` / `gates-pass` / `indexed` / `blocked` / `excluded-with-reason`。测试必须拒绝：重复 slug、重复 canonical URL（除明确 alias 绑定同一主记录）、alias 同时指向两个主项目、未知 status、blocked 无原因、非 blocked 状态却残留阻塞原因、`indexed` 无 source_ref 或 gates 记录。

- [ ] **Step 2: 定义现有四库 alias 与已完成状态**

固化：

```text
postgres   -> canonical_slug=postgres,     repo_key=postgres,     status=indexed（其余仓库 metadata 保持现状）
redis      -> canonical_slug=redis,         repo_key=redis,        status=gates-pass（RAG 完成后升级 indexed）
mysql      -> canonical_slug=mysql-server,  repo_key=mysql-server, status=gates-pass（RAG 完成后升级 indexed）
sqlite3    -> canonical_slug=sqlite,        repo_key=sqlite,      status=gates-pass（RAG 完成后升级 indexed）
```

数据库现有 refs 已通过，不把未 RAG 入库的项目写成 `indexed`。status 表应确保 catalog 名对应 repo metadata；别名不是新项目，不重复计数。

- [ ] **Step 3: 实现校验与下一项目 CLI**

导出：

```python
load_status(path) -> dict
validate_status(status, catalog) -> list[str]
next_action(status) -> dict | None
```

CLI：

```text
py -3 scripts/wiki/wiki_project_status.py validate
py -3 scripts/wiki/wiki_project_status.py next
py -3 scripts/wiki/wiki_project_status.py show langgraph
```

`next` 按 roadmap 的波次顺序取第一个非终态项目和具体未完成阶段，输出 catalog 名、canonical slug、下一动作、源码 repo、需要的 refs 根路径；若下一步是 catalog review，不要跳到 langgraph。

- [ ] **Step 4: 写完整状态表并校验 270 条**

扩容后逐 catalog 条目建立状态项。初始 270 行全部在 `catalog` 或 `planned`，已有四库按上一步回填状态；全部名称唯一、alias 一致、canonical URL 无冲突。langgraph 置为阶段 1 的下一 `planned` 项。

- [ ] **Step 5: 运行状态表与 CLI 测试**

Run:

```powershell
py -3 -m unittest discover -s scripts/wiki -p "test_wiki_project_status.py" -v
py -3 scripts/wiki/wiki_project_status.py validate
py -3 scripts/wiki/wiki_project_status.py next
```

Expected: 全部测试通过；validate 输出 270 projects / 0 errors；next 首先指出尚未完成的阶段 0A / 0B；完成阶段 0B 后才输出 langgraph。

- [ ] **Step 6: 更新 roadmap 单一入口**

将 `project-roadmap.md` 当前状态改为 catalog 270 / DB 150 / AI 120 已审核；批次台账为 CAT-270 complete、RAG-DB-4 pending；「下次从这里开始」写成唯一明确的下一阶段。所有阶段状态只能在一处维护，具体报告用链接，不复制另一张状态表。

---

## Task 5：CI 门禁、整体验收与阶段交接

**Files:**
- Modify: `.github/workflows/wiki.yml`
- Modify: `scripts/wiki/test_wiki_catalog.py`（如需锁定分类 / repo 唯一性契约）
- Modify: `docs/wiki/project-roadmap.md`

- [ ] **Step 1: 在 CI 增加 catalog / status 门禁**

增加无网络 job 或扩展现有 `wiki-tests`：

```yaml
- name: 项目清单与状态表门禁
  run: |
    python scripts/wiki/apply_catalog_additions.py --check
    python scripts/wiki/wiki_project_status.py validate
```

`--check` 必须只做本地 schema、重复项、配额、status/alias 一致性检查；不重新取 GitHub metadata，不写文件。

- [ ] **Step 2: 运行全套 wiki 测试与现有门禁**

Run:

```powershell
$env:PYTHONIOENCODING='utf-8'
py -3 -m unittest discover -s scripts/wiki -p "test_*.py"
py -3 scripts/wiki/wiki_lint.py docs/wiki/projects --quiet
foreach ($lib in 'postgres','redis','sqlite','mysql-server') {
  py -3 scripts/wiki/wiki_manifest_lint.py "docs/wiki/projects/$lib/_manifest.yml"
  py -3 scripts/wiki/wiki_refs.py --repo-root ".cache/repos/$lib" "docs/wiki/projects/$lib" --quiet
}
py -3 scripts/wiki/apply_catalog_additions.py --check
py -3 scripts/wiki/wiki_project_status.py validate
```

Expected: 所有命令 exit 0；工装测试无新增失败；四库 refs 仍 0 失败。

- [ ] **Step 3: 检查审计与工作树**

```powershell
python scripts/audit_workspace.py
git status --short
git ls-files -d
```

Expected: 没有任务临时目录残留；WARN 有解释；没有误删路径；无关用户修改保持未 stage。

- [ ] **Step 4: 汇总与阶段交接**

在 `project-roadmap.md` 台账登记 CAT-270 结果、分类分布、取数日期、测试与 CI 状态；设置唯一下一步为阶段 0B：三个既有数据库的 RAG ingest。另将阶段 1 的首个源码项目明确固定为 langgraph，写入 `wiki_project_status.yml` 的 `planned` 项与 `next_action`。

- [ ] **Step 5: 分路径提交并即时推送**

每个提交只含本次任务相关文件；按照仓纪律，每个 commit 后立即 `git push origin main`。不使用 `git add -A`。

---

## 全路线阶段状态同步（后续每次更新）

本计划只负责 CAT-270 / 全局状态账本的实现。执行后的跨阶段状态以 `docs/wiki/project-roadmap.md` 为唯一总入口，后续更新必须包括：

1. **总阶段**：0A、0B、1、2A–2G、3A–3H、4 每项状态、阻塞和完成证据。
2. **项目注册表**：270 项逐条状态与 source SHA；每次结束项目立即升级状态。
3. **统一台账**：每批机制页数、门禁、审阅、CI、RAG blocks，失败项与下一动作。
4. **下一个执行入口**：必须给 `wiki_project_status.py next` 与本 roadmap 的同一个结果；不允许说“试点结束，之后再定”。
5. **暂停恢复**：重新开始时先 `git status --short`、读 roadmap「当前状态」/「下一步」、跑状态 CLI；不依赖对话记忆。
