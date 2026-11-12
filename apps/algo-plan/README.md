# algo-plan · 算法能力提升计划 App

本机运行的算法学习助手：本地静态题库打底，LLM 做增强，判题走真实编译执行，
进度与错题写回 learning 仓进 git。

当前进度：**M6 LLM 增强**（讲解 / 分级求助 / 批改思路 / 现造新题 / 补全缺口 /
成本护栏 / 密钥只从环境变量读 / LLM 关掉后 app 照常可用）。
题库导入属于 M2（已完成）；判题属于 M3（已完成，真实编译执行）；
轮次与复习闭环属于 M4（已完成）；掌握度与诊断属于 M5（已完成）。

## 快速开始

需要 Node ≥ 24（用到内置 `node:sqlite`）。零 npm 依赖、零构建。

```bash
# 跑测试
node tests/run-all.js

# 接上 learning 仓并启动
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node server/main.js
# 打开 http://127.0.0.1:5173
```

没设 `ALGO_PLAN_LEARNING_ROOT` 也能启动——那是「纯本地模式」：只有 SQLite，
不与 learning 仓同步。

## 界面层次

界面是**单一暗色主题**（不做浅色、不做双主题）。层次靠四件事建立，不靠颜色堆：

| 层 | 构件 | 干什么 |
|---|---|---|
| 1 | 页头 `.page-head` | 每页第一眼：眉标 → 大标题 → 一句副标，说清「这是哪儿」 |
| 2 | 主卡 `.panel.hero` | 「现在该做什么」只有一个：下一步、该复习这道、当前轮次 |
| 3 | 普通卡 `.panel` / 嵌套块 `.block` | 细节与上下文：题面、样例、队列概况、轮次清单 |
| 4 | 静默卡 `.panel.quiet` | 背景信息：运行状态、仓库连接情况，不抢注意力 |

另有两条硬规矩：

- **数字摆成数字块**（`.stat-grid`），不要用一长串键值行——「今天做了 2 道 / 复习 1 张」
  应该一眼看完，而不是逐行读。
- **状态用中文文字 + 颜色双承载**（`.pill` / `.badge`）。档位、进度、判题结果都有中文说法，
  不靠一块颜色单独表意（色觉障碍、灰度打印都读得出来）。

公共构件集中在 `web/ui.js`：`pageHead` / `hero` / `panel` / `block` / `statGrid` / `pill` / `kv` / `empty`。
页面脚本从 `window.AlgoPlanUI` 取（Node 下 require 同一份文件——实现只有一份）。

层次有回归闸门：`tests/ui-hierarchy.test.js` 断言每页有且只有一个页头、
主行动在主卡里、状态是中文药丸。

## 环境变量

| 变量 | 说明 | 默认 |
|---|---|---|
| `ALGO_PLAN_LEARNING_ROOT` | learning 仓根目录。**不硬编码相对路径** | 无（未设＝纯本地模式） |
| `ALGO_PLAN_LOCAL_BANK` | 工程仓内置题库根（Codeforces 题在这） | `apps/algo-plan/bank` |
| `PORT` | 服务端口 | `5173` |
| `ARK_API_KEY` | 火山方舟密钥（**只从环境变量读**） | 无 |
| `ARK_BASE_URL` | 方舟地址 | `https://ark.cn-beijing.volces.com/api/v3` |
| `ANTHROPIC_API_KEY` | Anthropic 密钥（**只从环境变量读**） | 无 |
| `ALGO_PLAN_LLM_ENABLED` | LLM 总开关，`0`/`false`/`off` = 关 | 未设＝按密钥自动判断 |
| `ALGO_PLAN_LLM_PROVIDER` | `ark` / `anthropic` / `null` | 未设＝按密钥推断 |
| `ALGO_PLAN_LLM_MODEL` | 模型名 | 按 provider 取默认 |
| `ALGO_PLAN_LLM_BASE_URL` | 覆盖地址 | 按 provider 取默认 |
| `ALGO_PLAN_LLM_SESSION_LIMIT` | 单次会话请求上限 | `20` |
| `ALGO_PLAN_LLM_DAILY_LIMIT` | 每日请求上限 | `100` |

密钥还可以放在 `apps/algo-plan/.env` 里（`projects/.gitignore` 已经忽略它），
启动时自动读取；**已存在的环境变量优先，`.env` 不覆盖**。

## 目录结构

```
algo-plan/
├── core/            纯函数层：Problem 模型、markdown 解析、provider、路径解析
│   ├── bank/        legacy.js：死脚本内联清单的静态解析
│   │                solutions-scan.js：存量题解的静态扫描
│   ├── review/      SM-2 间隔重复与判题结果映射
│   ├── scheduler/   推下一题：候选动作优先级 + 单日配额
│   ├── sync/        learning 仓同步：轮次解析、勾选写回、错题订正、查脏
│   ├── mastery/     掌握度模型：mastery.js（公式与四档诊断）、expected.js（期望题量口径）、
│   │                topic-stats.js（从流水派生知识点统计）
│   └── judge-spec/  判题契约纯函数：用例归一化、输出比对
├── server/          组合根、路由、API、SQLite 存储、诊断数据装配（insights.js）
├── judge/           判题执行器：沙箱（临时目录/探测/杀进程树）、编排器（verdict）
├── bank/            工程仓内置题库（Codeforces 题，共 51 道）
├── web/             原生 JS 单页（无构建）：today / answer / review / rounds / diagnose 五页
│                    ui.js 是公共界面构件（页头 / 主卡 / 数字块 / 药丸 / 空态），
│                    charts.js 是手绘 SVG 的纯函数层（零第三方图表库）
├── tools/           seed-bank.js / import-catalog.js / import-solutions.js / import-bank.js
│                    import-codeforces.js / sync-links.js
├── fixtures/        样板题 + 判题 fixture（题面、用例、正例与三类负例）
└── tests/           node:test 套件，一个命令跑全部
```

## 两套题库根

应用同时认**两个**题库根，**前者优先**：

| 顺序 | 根目录 | 内容 | 性质 |
|---|---|---|---|
| 1 | `learning/bank/` | 人审题库（LeetCode、牛客、剑指 Offer……） | 在 git 里，改动要人审 |
| 2 | `apps/algo-plan/bank/` | 机器抓的 Codeforces 题 | 也在 git 里，但内容由导入器生成 |

- 同一个 id 出现在两个根里时，**以先出现的为准**（人审库优先），并在 `/api/health`
  的 `bank.warnings` 里记一笔。
- **写回永远写人审库**（`learning/bank`），不会把机器抓的题写回工程仓。
- 工程仓的 `bank/` 之所以不住 `data/` 下面：`projects/.gitignore` 有 `data/` 规则，
  放那儿就进不了 git，题目就丢了。

## 题库格式

题库放在 `learning/bank/<track>/<id>.md`，markdown + frontmatter，人和 agent 都能直接编辑：

````markdown
---
id: lc-00001
source: leetcode
title: 两数之和
difficulty: easy
topics: [哈希表, 数组]
tracks: [interview, fundamentals]
status: todo
needs_fill: false
created: 2026-11-11
updated: 2026-11-11
---
## 题面
...
## 测试用例
| input | expect |
|---|---|
| [2,7,11,15], 9 | [0,1] |
## 参考解法
```cpp
...
```
````

`id` 是 git 侧与 SQLite 侧的唯一连接键。人审题库用 `<前缀>-<5 位数字>`，
前缀取自 `lc` / `nc` / `jz` / `self` / `llm`；Codeforces 题用 `cf-<题号>-<字母>`。

存量题库里有一批题**只有元数据、没有题面**（`needs_fill: true`）。这类题在界面上
**不装死链、不显示空白**，而是明确给出状态：题号、源站、去原题的入口，以及
「题面待回填」的说明。

导入样板题：

```bash
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js --dry-run
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js
```

## 题库导入（M2）

题库有两路来源，都由 `tools/` 下的导入器负责。**导入器一律不碰 learning 仓的 git**：

| 工具 | 来源 | 产出 |
|---|---|---|
| `tools/import-catalog.js` | `projects/scripts/` 三个死脚本里的内联题目清单 | `learning/bank/<track>/<id>.md`（只有元数据） |
| `tools/import-solutions.js` | `learning/code-solutions/` 与 `learning/algo-c/` 里的存量题解 | 同上，多一段「## 参考解法」 |
| `tools/import-bank.js` | 上面两个，按「先目录、后题解」的顺序跑一遍 | 同上 |

```bash
# 先试运行，看要写哪些文件
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --dry-run

# 真导入
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js

# 已有同名文件时强制覆盖（默认是跳过并计入 skipped）
ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --force
```

导入的题**一律 `needs_fill: true`**：死脚本与存量题解里都只有元数据与代码，
没有题面和测试用例，补内容是后面的活。

导入器**绝不执行** `projects/scripts/` 里的死脚本——那三个脚本都指向已删除的
`practice-data.js`，而且里面有 6 处题号写成了未定义标识符（`offer22` / `offer18` /
`deleteNode` / `offer23` / `offer09` / `offer30`），跑起来必然 `ReferenceError`。
题目清单是**静态解析**出来的。

同 id 的冲突按 **id** 判断（不按路径）：同一个 id 在两路来源里算出来的轨道可能不同，
只看自己那条路径会漏判，会写出两份同 id 的文件。

### Codeforces 题源

`tools/import-codeforces.js` 从 Codeforces 抓题面，落到**工程仓**的 `bank/<track>/`
（不落 learning 仓）：

```bash
# 抓题面（Chrome DevTools Protocol 驱动，网页版挂在 Cloudflare 后面）
node tools/fetch-cf-pages.js

# 导入成题库文件
node tools/import-codeforces.js --dry-run
node tools/import-codeforces.js

# 已有文件时强制覆盖
node tools/import-codeforces.js --rewrite
```

要点：

- **测试数据不公开**，所以用例**只提取题面里的样例**（样例就是官方给的输入输出）。
- 每道题人工整理了一段**中文题意**放在英文原题面前，中文对照着看更省事。
- 题面里带「题目信息」小节：源站题号、难度、标签，以及**去原题**的链接。
- 抓下来的题**一样受「不自动 commit」约束**——导入器只写文件，不碰 git。

`tools/sync-links.js` 负责给存量题回填原题链接（有直达地址给「去原题」，
只有搜索地址退到「去源站搜索」，两个都没有就明说没有，不装死链）。

## 判题契约

提交给判题器的是**完整的 stdin→stdout 程序，必须自己写 `main()`**。

- 判题器不注入 Solution 包装、不自动生成 `main()`、不猜方法名。
- C++ 走 `g++ -std=c++17 -O2`（不加 `-Werror`、不加 `-Wall`），Python 走
  `python3` → `python` → `py -3` 依次探测（第一个能 `--version` 的胜出）。
- 单用例默认超时 2000ms，超时杀整棵进程树（Windows `taskkill /T /F`，POSIX 进程组 kill）。
- verdict 只有五个：`pass` / `wrong_answer` / `timeout` / `compile_error` / `runtime_error`。
- 单次运行的 stdout/stderr 各封顶 **2MB**，超了截断——不封顶的话，一个死循环打印的
  程序会把输出撑到几百 MB，拼字符串时撞上 V8 的长度上限抛 `ERR_STRING_TOO_LONG`，
  那行代码在事件回调里，抛出去就是 `uncaughtException`，**整个服务进程直接死**。
- 杀进程树之后还有 **2 秒宽限期**：到点还没结束就强行按超时收摊，
  免得请求永远挂着、临时目录永远删不掉。

**判题不是安全沙箱**：它防跑飞、死循环、爆内存、编译垃圾，**不防恶意代码**。
用户代码以当前用户权限在系统临时目录里跑，每题一个临时目录，判完即删。

## 掌握度与诊断（M5）

诊断页的每个数字都有明确的来源，口径定死，不随实现漂移。

### 掌握度

```
首次通过率 = firstPass / attempts            ← 反复重试通过不算掌握
遗忘因子   = 0.5 ^ (距上次练习天数 / 14)      ← 半衰期 14 天
掌握度     = (0.7 × 首次通过率 + 0.3 × e^(-失误次数/2)) × 遗忘因子
```

- `firstPass`：**第一次尝试就通过**的题目数。
- `attempts`：该知识点下所有题目的提交总次数。
- `lapses`：该知识点下**没通过**的提交次数。
- 四个数存在 SQLite 的 `topics` 表里（`first_pass` / `attempts` / `lapses` / `last_at`）。

### 覆盖度

```
覆盖度 = 练过的不同题目数 / 期望题量
```

### 期望题量（覆盖度的分母）

三级口径，自上而下：

| 优先级 | 来源 | 说明 |
|---|---|---|
| ① 覆盖值 | `settings` 表的 `insights.expected_override.<topic>` | 手工指定某个知识点的期望题量 |
| ② 题库题数 | 题库里带这个 topic 的题目数 | 「这个知识点一共就这么多题」 |
| ③ 下限常量 | `settings` 表的 `insights.expected_min_questions`，默认 **5** | 「至少练够 5 题才叫练透」 |

**②③ 的合成规则：`期望题量 = max(题库题数, 下限常量)`。**

下限存在的理由：只有 1 道题的冷门知识点，练 1 道就是 100% 覆盖，会被误判成「掌握」。
下限常量把这个洞堵上。

**可配置**（改完刷新诊断页立刻生效）：

| 键名 | 作用 | 默认 |
|---|---|---|
| `insights.expected_min_questions` | 所有知识点的期望题量下限 | `5` |
| `insights.expected_override.<topic>` | 单个知识点的期望题量（如 `insights.expected_override.数组`） | 无 |

### 四档诊断

掌握度 ≥ 0.5 算「高」，覆盖度 ≥ 0.5 算「高」，两个维度交叉出四档：

| 掌握度 | 覆盖度 | 档位 | 含义与处方 |
|---|---|---|---|
| 低 | 低 | 薄弱且练得少 | 轮次里优先排这类题——先补量 |
| 低 | 高 | 练过但总错 | 回头看概念，而不是继续刷题 |
| 高 | 低 | 偶然会，没练透 | 安排复习巩固 |
| 高 | 高 | 掌握 | 不再主动排 |

「练过但总错」这一档很重要——它区分「题做得少」和「概念没懂」，两者处方完全不同。

### 每日题量曲线

数据源是 `attempts` 表按 `substr(ts,1,10)` 分组的行数，也就是当天的提交次数。
默认最近 30 天，`GET /api/insights?days=N` 可调（`days=0` 表示全部）。
窗口内某天没提交会**补 0**，这样「哪天没练」在曲线上看得见。

### 诊断页

顶栏的「诊断」页有四块内容，全部是**手绘 SVG**（零第三方图表库）：

1. **掌握度雷达**——两条序列：掌握度、覆盖度，取练得最多的 6 个知识点；
2. **弱项榜**——只收「练过但总错」「薄弱且练得少」两档，条形末端标掌握度；
3. **每日题量曲线**——折线 + 面积，窗口内没提交的日期补 0；
4. **知识点覆盖**——每个知识点「练过 N / 期望 M 题」的覆盖条。

每张图都能展开成表格视图（表格孪生），方便核对数字。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 运行状态、learning 仓连接情况、题库概况 |
| GET | `/api/bank` | 题库列表，支持 `track` / `topic` / `difficulty` / `status` / `prefix` / `limit` |
| GET | `/api/problem/:id` | 单题详情。**只回前 2 个样例用例 + 用例总数，不回参考解法** |
| POST | `/api/llm/explain` | 讲解。入参 `{problemId, level}`，回 `{text, model}` |
| POST | `/api/llm/hint` | 分级求助。入参 `{problemId, level}`，回 `{text, model}` |
| POST | `/api/llm/grade-idea` | 批改思路。入参 `{problemId, idea}`，回 `{verdict, comment, model}` |
| POST | `/api/bank/llm-generate` | 现造新题并**落盘**。入参 `{track, topics, difficulty}`，回 `{problem, model}` |
| POST | `/api/llm/fill` | 补全缺口并**写回**。入参 `{problemId}`，回 `{problem, model}` |
| GET | `/api/llm/settings` | LLM 开关、provider、模型名、密钥来源、配额 |
| PUT | `/api/llm/settings` | 保存 LLM 开关、provider、模型名、地址。**不接受密钥** |
| POST | `/api/attempt` | 提交代码判题。入参 `{problemId, code, language, mode}`；响应 `{attemptId, verdict, passedCases, totalCases, durationMs, failedCase, message, compilerOutput}` |
| GET | `/api/next` | 下一个动作：复习 / 轮次推进 / 主动探索，带「这一步大约 N 分钟」。轮次卡片若不在题库里，`problemId` 为空而 `roundId` / `itemId` 有值——界面会把你带到轮次页去勾掉它 |
| POST | `/api/review/:id/grade` | 给复习卡片打分（`quality` 0~5），返回下一次复习时间、间隔、难度因子、遗忘次数 |
| GET | `/api/rounds` | learning/progress 下的轮次清单与解析告警 |
| PATCH | `/api/rounds/:id/items/:itemId` | 勾选写回（`status` / `doneDate` / `commitSha`），只改目标行，返回 diff |
| POST | `/api/correction` | 写错题订正到 `notes/practice/<id>-订正.md`，返回文件路径与 diff |
| GET | `/api/progress` | 今日进度与单日复习配额 |
| GET | `/api/insights` | 诊断数据。`?days=N` 调曲线窗口（默认 30，`0`＝全部）。返回 `{topics, summary, series}` |

`GET /api/insights` 的响应：

```json
{
  "topics": [{
    "topic": "数组",
    "mastery": 0.2836,
    "coverage": 0.4,
    "bucket": "weak-sparse",
    "label": "薄弱且练得少",
    "action": "轮次里优先排这类题——先补量",
    "attempts": 4,
    "firstPass": 1,
    "lapses": 2,
    "lastAt": "2026-11-10T11:00:00.000Z",
    "expected": 5,
    "expectedSource": "floor",
    "problems": 2
  }],
  "summary": {
    "topicCount": 2,
    "untouchedTopics": 1,
    "weakCount": 1,
    "strongCount": 1,
    "avgMastery": 0.4061,
    "avgCoverage": 0.3,
    "totalAttempts": 4,
    "totalProblems": 2,
    "lastPracticeAt": "2026-11-10T11:00:00.000Z",
    "minExpectedQuestions": 5,
    "windowDays": 30,
    "windowAttempts": 4
  },
  "series": [{ "date": "2026-11-01", "count": 0 }, { "date": "2026-11-02", "count": 2 }]
}
```

`summary.totalAttempts` / `totalProblems` **从原始流水算**，不按知识点加总——
一道题挂两个知识点时，按知识点加总会把同一次提交数两遍。
`topics` 的顺序是处置优先级（练过但总错 → 薄弱且练得少 → 偶然会没练透 → 掌握），
同一档内按掌握度升序。

## LLM 增强（M6）

LLM 是**可选增强**：不配密钥也能用，app 的排程、判题、复习、进度一项不少。
关掉之后点「讲解」或「求助」，会得到一句明确的中文提示，而不是报错或空白。

### 五个场景

| 场景 | 接口 | 前端入口 |
|---|---|---|
| 讲解 | `POST /api/llm/explain` | 作答页「讲思路」/「直接给代码」 |
| 分级求助 | `POST /api/llm/hint` | 作答页「求助」四档：方向 → 思路 → 伪码 → 代码 |
| 批改思路 | `POST /api/llm/grade-idea` | 作答页的思路输入框 |
| 现造新题 | `POST /api/bank/llm-generate` | 题库页「来一题新的」 |
| 补全缺口 | `POST /api/llm/fill` | 题库页缺题面的题后面的「让 LLM 补全」 |

**不主动泄题**：讲解默认只到思路层；「直接给代码」是另一个独立按钮，点之前要确认一次。
求助四档**逐级解锁**——用完「方向」才放开「思路」，解锁进度存在 `sessionStorage`，关掉浏览器就没了。

**产出全部可追溯**：现造的新题**落盘进 `learning/bank/<track>/<id>.md`**
（`source: llm`，题号是 `llm-<内容哈希>`），不是一次性玩具；
每次讲解都记下 **model + 提示词摘要**到 `settings` 表。

### 配置密钥

密钥**只从环境变量读**，不写进代码、不写进 settings 表、不进 git。
在本目录下建一个 `.env`（`projects/.gitignore` 已经忽略它）：

```
ARK_API_KEY=你的方舟密钥
```

或者：

```
ANTHROPIC_API_KEY=你的 Anthropic 密钥
```

启动时会自动读 `.env`；也可以直接用环境变量：

```bash
ARK_API_KEY=xxx node server/main.js
```

### provider 与模型

| provider | 默认地址 | 默认模型 |
|---|---|---|
| `ark` | `https://ark.cn-beijing.volces.com/api/v3` | `doubao-seed-1-6-251015` |
| `anthropic` | `https://api.anthropic.com` | `claude-opus-5` |

模型名要写**完整版本化 ID**：方舟写 `doubao-seed-1-6-251015` 这种，
直接写族名会返回 `404 InvalidEndpointOrModel.NotFound`。

设置页可以切 provider、改模型名、开关 LLM；**密钥不在设置页里填**——
设置页只显示「密钥来自哪个环境变量」。

### 成本护栏

- 单次会话最多 **20** 次请求；
- 每天最多 **100** 次请求；
- 超了**直接停**，提示「今天用完了」，不会继续烧钱；
- 计数落在 `settings` 表，键是 `llm.quota.session.*` 与 `llm.quota.daily.*`；
- 上限可以用设置页，或用 `ALGO_PLAN_LLM_SESSION_LIMIT` / `ALGO_PLAN_LLM_DAILY_LIMIT` 调。

### 离线自检

```bash
node tools/llm-check.js
```

只读不写：**不发任何网络请求**，只打印当前 provider、模型名、密钥来源、配额，
并对五个提示词做纯函数自检（同样输入两次构造必须一致）。

## 测试

```bash
node tests/run-all.js
```

| 套件 | 覆盖 |
|---|---|
| `smoke.test.js` | 静态页 200 / 404 / 越界 400 / 方法不对 405 |
| `router.test.js` | 路径参数匹配、方法匹配、JSON body 解析 |
| `problem.test.js` | ID 生成、topic 归一化、字段校验 |
| `parse.test.js` | frontmatter、用例表、代码块、往返等价 |
| `db.test.js` | 建表、配置、流水、复习卡片、知识点 |
| `provider.test.js` | 题库读写、过滤、坏文件容错 |
| `api.test.js` | 三个接口（健康 / 题库 / 单题）、纯本地模式降级、用例不外泄 |
| `e2e.test.js` | 导入 → 起服务 → 打接口 → 落盘 |
| `legacy.test.js` | 死脚本内联清单的静态解析：分类合并、题号提取、标题还原、标签归一化 |
| `solutions-scan.test.js` | 存量题解扫描：标记切分、文件名取号、专题识别、索引解析 |
| `import-catalog.test.js` | 目录导入器：落盘位置、needs_fill、坏题号跳过、冲突跳过、dry-run |
| `import-solutions.test.js` | 题解导入器：落盘位置、参考解法、索引题名难度、命名过滤、重复导入 |
| `m2-e2e.test.js` | 端到端：死脚本 / 题解 → 题库文件 → 读回来 |
| `judge-spec.test.js` | 用例归一化、输出比对、容差比较 |
| `sandbox.test.js` | 工具链探测、临时目录、喂 stdin、超时杀进程树 |
| `judge-fixtures.test.js` | fixture 自检：正例对、三类负例确实坏 |
| `runner.test.js` | 五类 verdict 端到端、判题契约反例、输出上限、宽限期兜底、临时目录清理 |
| `attempt.test.js` | `POST /api/attempt` 的形状、校验、流水落库 |
| `web-panel.test.js` | 结果面板数据整理（纯函数） |
| `web-dom.test.js` | 前端渲染路径（DOM 桩）：样例渲染、草稿不丢、渲染竞态 |
| `sm2.test.js` | SM-2 公式、easiness 下限、六档 quality 映射、到期与逾期排序 |
| `rounds-parse.test.js` | 7 种真实表头变体、缺列、坏行、告警 |
| `sync-write.test.js` | 查脏、只改目标行、缺列告警、脏仓拒绝、错题订正 |
| `next-action.test.js` | 三级优先级、两种配额耗尽、轮次挑选与掌握度排序 |
| `api-m4.test.js` | M4 五个接口的响应字段 |
| `pages.test.js` | 四个页面注册与评分映射 |
| `e2e-m4.test.js` | 下一题 → 写回 → 订正 → 评分，且不自动提交 |
| `mastery.test.js` | 掌握度公式、遗忘因子、四档诊断、处置优先级排序 |
| `expected.test.js` | 期望题量三级口径（覆盖值 / 题库题数 / 下限常量） |
| `topic-stats.test.js` | 从流水派生知识点统计（首次通过、失误次数、练过题数） |
| `insights.test.js` | 诊断装配与 `GET /api/insights`（含天数窗口的夹取与补 0） |
| `charts.test.js` | 手绘 SVG 纯函数：雷达 / 折线 / 条形，数值不出现 NaN |
| `diagnose.test.js` | 诊断页视图函数、静态资源接线、页面注册 |
| `diagnose-e2e.test.js` | 造流水 → 打接口 → 静态资源 → 前端 SVG 干净的完整闭环 |
| `llm-prompts.test.js` | 五个场景的提示词纯函数、防泄题（只有「代码」档才给代码） |
| `llm-client.test.js` | 三个客户端 + 假 fetch：provider 推断、密钥来源、错误处理 |
| `llm-guard.test.js` | 成本护栏：会话上限、每日上限、计数落 settings 表 |
| `llm-service.test.js` | 五个场景服务层、造题落盘、讲解留痕、护栏拦截、关闭态降级 |
| `llm-api.test.js` | 五个场景接口与两个设置接口的响应形状、错误状态码 |
| `llm-off.test.js` | 把 LLM 关掉，app 仍然完整可用（硬约束专测） |
| `llm-web.test.js` | 前端纯逻辑：求助四档逐级解锁、请求体形状 |
| `llm-web-bank.test.js` | 题库页与设置页：needs-fill 挑题、设置体只留白名单字段 |
| `llm-e2e.test.js` | 端到端：五个场景跑通、造题落盘、讲解留痕、护栏拦住第 21 次 |

## 写回 learning 仓的规矩

- **写回前查脏**：`git -c core.quotepath=false status --porcelain -z`（按 NUL 切分），
  工作区不干净就拒绝写回，并告诉你哪个文件被动过。
- **只改目标行**：读-改-写，整份轮次文件只有那一行会变。
- **绝不自动 commit**：写回只落在 learning 仓的工作区，界面把 diff 摆出来，
  提交与否由你决定。app 不会替你 `git add`，也不会替你 `git commit`。
- **错题订正**写到 `notes/practice/<题目ID>-订正.md`，frontmatter 键序按
  `notes/_meta/schema.md` 登记的 `correction` 惯例。

## 归属

- 服务端、判题器、前端、测试 → `projects/apps/algo-plan/`（本仓）
- 题目本体、轮次清单、错题订正 → `learning/bank/`、`learning/progress/`、`learning/notes/`（学习仓）
