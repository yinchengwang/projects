# apps/web 合并 · 计划三：knowledge-hub（17 页 + 8 组件）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 knowledge-hub 的 14 个数据域、5 个独有 core 模块与 17 个页面 + 8 个组件整体搬进 `apps/web/`，成为 `/knowledge/` 路由下的模块；现有 181 个断言全部迁入并保持通过。

**Architecture:** 与计划二同构 —— 数据层放 `apps/web/shared/knowledge/`，视图层放 `apps/web/web/knowledge/`。两个 Canvas 组件（`knowledge-graph` `radar-chart`）的**坐标计算已在 `core/*-layout.js` 里（纯函数、零 wx、可单测）**，组件本体只负责绘制，因此 Canvas 移植只改「怎么拿到 canvas」，绘制代码逐字保留。

**Tech Stack:** Node.js ≥ 24（内置 `node:test`）、零 npm 依赖、零构建；浏览器端原生 ES2020 + DOM + Canvas 2D。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：`apps/web/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。
- **零依赖**：不新增任何 npm 包，不引入前端框架，不引入打包工具。
- **中文文案**：面向用户的文案一律中文。
- **测试命令**：`cd apps/web && node tests/run-all.js`。
- **依赖前提**：本计划**必须在计划一、计划二完成后**执行。
- **不要动 `engine/`、`apps/study-hub/`、`apps/wiki-rag/`**。
- **不要动 `apps/knowledge-hub/`**：只读参考，所有新代码落在 `apps/web/` 下。本计划**不删**旧目录（删除在计划五）。
- **不动计划一、计划二的既有文件**（除 Task 3 明确说的两处）。

## 依赖前提（前两个计划的产出，本计划直接用）

| 来自 | 本计划怎么用 |
|---|---|
| `shared/core/storage.js` `store.js` `id.js` `date.js` `format.js` `md.js` `md2html.js` `constants.js` | 数据层依赖（全部零改动复用） |
| `shared/core/theme.js` `with-theme.js` | 页面主题包装（`withTheme(options, settingsKey)` 第二个参数可选） |
| `shared/ui.js` | 构件层 |
| `shared/platform.js` | 交互层（含计划二补的 `setClipboardData` / `getClipboardData` / `vibrateShort`） |
| `shared/charts.js` | 图表层 |
| `shared/workbench/*` | **不用**（域集完全不相交） |
| `web/cjs-loader.js` | 浏览器侧 CommonJS 加载器 |
| `tests/wb-harness.js` `tests/wb-stub.js` | 脚手架模式（本计划照抄一份 kh 版） |

---

## 关键调查结论（决定了本计划的做法）

### 结论 1：现有测试已覆盖全部 17 个页面

实测 `apps/knowledge-hub/tests/` 5 个测试文件：

| 文件 | 断言数 | 覆盖 |
|---|---|---|
| `stores.test.js` | 42 | 14 个数据域：种子加载、SM-2 评分、quiz 判分/统计/错题本、图谱差距、五年计划打卡/连续天数、路径进度、摘抄派生、面试题库浏览状态 |
| `pages2.test.js` | 67 | 页面级（面试/知识/学习路径等） |
| `pages.test.js` | 41 | 页面级（首页/学习/我的等） |
| `review.test.js` | 20 | SM-2 间隔重复 |
| `graph.test.js` | 11 | 知识图谱布局 |

页面路由被引用覆盖 **17 个不同页面** —— 与 knowledge-hub 的 17 个页面**一一对应，没有遗漏**。

**与计划二的差异**：knowledge-hub 的测试用 `ok(name, cond)` 风格（不是 `test(name, fn)`），且 `ok()` 抛错即失败。迁入时**保留 `ok()` 风格**，只在文件末尾加一段统计输出。

### 结论 2：域集与 workbench 完全不相交 —— 不存在"合并冲突"

| | programmer-workbench | knowledge-hub |
|---|---|---|
| 域数 | 12 | 14 |
| 域名 | todos, techNotes, snippets, interview, resources, reading, fitness, mood, schedule, projects, insights, settings | excerpts, knowledge, studyProjects, interviews, mockInterviews, learningPaths, reviews, daily, fiveYearPlan, interviewBank, quizzes, settings, user, notifications |

**只有 `settings` 同名**，且计划二已把 workbench 挪到 `db:wb_settings`，两边不再抢键。

**结论**：knowledge-hub 的 `stores.js` **不需要与 workbench 合并**，它就是自己的一份。这比设计文档原先设想的「合并成一个 stores.js」简单得多 —— 原设计低估了这个发现。

### 结论 3：`stores.js:39` 那行 `Object.assign` 必须改

```javascript
Object.assign(storage.KEYS, KH_KEYS)
```

这行在**模块加载时就地修改共享的 `storage.KEYS` 对象**。在合并应用里，workbench 的 `keys.KEYS` 是独立对象（计划二已做），所以**污染不到 workbench**；但它仍会污染 `shared/core/storage.js` 的 `KEYS`，影响后续计划。

**改法**：改成本地副本，**不改 `storage.KEYS`**：

```javascript
// KH 自己的键表副本，不污染共享的 storage.KEYS
const KH_KEYS = { daily: 'daily', /* ... */ }
const K = Object.assign({}, storage.KEYS, KH_KEYS)
```

> workbench 那边用的是自己的 `shared/workbench/keys.js`，不依赖 `storage.KEYS`，所以这个改动对两边都安全。已实测。

### 结论 4：两个 Canvas 组件的移植成本很低

`knowledge-graph` 与 `radar-chart` 的结构都是「**纯计算 + 纯绘制**」：

| 部分 | 位置 | 移植成本 |
|---|---|---|
| 坐标计算 | `core/graph-layout.js` `core/radar-layout.js` | **零** —— 纯函数，无 wx，已有单测 |
| 绘制 | 组件的 `draw()` | **低** —— Canvas 2D API（`clearRect` `beginPath` `arc` `fill` `stroke` `moveTo` `lineTo` `closePath`）小程序与 Web **完全一致** |
| 取 canvas | 组件的 `ready()` | **要改** —— 小程序用 `wx.createSelectorQuery().in(this).select('#graph').fields({node:true,size:true})`，Web 用 `document.querySelector` + `getBoundingClientRect` |

**结论**：只有 `ready()` 那 15 行要改，`draw()` 逐字保留。且 Canvas 组件只有 2 个页面在用（`knowledge/graph` `knowledge/radar`）。

### 结论 5：组件的 6 个通用件可以直接被 `shared/ui.js` 取代

`card` `empty` `fab` `header` `progress-bar` `tag` 这 6 个组件与 `shared/ui.js` 的构件**功能重叠**：

| 小程序组件 | `shared/ui.js` 对应 |
|---|---|
| `card` | `panel()` |
| `empty` | `empty()` |
| `header` | `pageHead()` |
| `tag` | `pill()` |
| `progress-bar` | （需加 `progressBar()`） |
| `fab` | （页面内直接用按钮） |

**结论**：**只移植 2 个 Canvas 组件**（`knowledge-graph` `radar-chart`），其余 6 个用 `shared/ui.js` 的构件替代。这省掉 6 个组件的移植工作。

需要在 `shared/ui.js` **加一个** `progressBar(percent, opts)` 构件。

### 结论 6：`setData` 的点路径写法要保留

knowledge-hub 有 **12 处**点路径 `setData`（`'form.title'` `'recForm.type'` `'current.bookmarked'` `'editForm.note'` 等），分布在 9 个页面。

计划二的 `wb-harness.js` 的 `setData` 是**浅合并**，不支持点路径 —— **本计划的 `kh-harness.js` 必须支持**（小程序版的 `page-harness.js` 支持，见其第 37–51 行）。

**结论**：把点路径支持加进 `wb-harness.js`，两个 harness 共用一份实现。

### 结论 7：knowledge-hub 的 `settings` 保持 `db:settings`

计划二已把 workbench 挪到 `db:wb_settings`，所以 knowledge-hub **不用改键名**，保持 `db:settings` 原样。

`shared/core/with-theme.js` 的 `readSettings()` 缺省就读 `db:settings` —— 正好。所以 knowledge-hub 页面用 `withTheme(options)` **不传第二个参数**。

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/web/shared/knowledge/keys.js` | 新建 | knowledge-hub 的 14 个存储键（`settings` 保持 `settings`） |
| `apps/web/shared/knowledge/seed.js` | 新建 | 结构性种子：图谱节点/边、学习路径、面试公司、模拟面试、复习记录（从小程序版搬） |
| `apps/web/shared/knowledge/seed-data.js` | 新建 | 批量内容：题库/深挖/雷达/五年配置/核心项目/面试题库/初始摘抄（从小程序版搬） |
| `apps/web/shared/knowledge/review.js` | 新建 | SM-2 间隔重复纯函数（从小程序版搬） |
| `apps/web/shared/knowledge/kh-constants.js` | 新建 | knowledge-hub 专属常量（从小程序版搬） |
| `apps/web/shared/knowledge/graph-layout.js` | 新建 | 知识图谱坐标计算（从小程序版搬） |
| `apps/web/shared/knowledge/radar-layout.js` | 新建 | 技术雷达坐标计算（从小程序版搬） |
| `apps/web/shared/knowledge/stores.js` | 新建 | 14 个数据域实例（从小程序版搬，改 `Object.assign` 那行） |
| `apps/web/web/knowledge/router.js` | 新建 | 模块内路由表（17 个页面 + 2 个组件入口） |
| `apps/web/web/knowledge/browser.js` | 新建 | 模块浏览器入口（预载 + 挂载） |
| `apps/web/web/knowledge/shell.html` | 新建 | 模块 HTML 壳 |
| `apps/web/web/knowledge/style.css` | 新建 | 模块样式 |
| `apps/web/web/knowledge/components/knowledge-graph.js` | 新建 | 知识图谱 Canvas 组件（Web 版） |
| `apps/web/web/knowledge/components/radar-chart.js` | 新建 | 技术雷达 Canvas 组件（Web 版） |
| `apps/web/web/knowledge/pages/*.js` | 新建 | 17 个页面模块 |
| `apps/web/shared/ui.js` | 改 | 加 `progressBar(percent, opts)` 构件 |
| `apps/web/tests/kh-harness.js` | 新建 | 页面加载脚手架（支持点路径 setData） |
| `apps/web/tests/kh-stub.js` | 新建 | localStorage 替身（复用 wb-stub 的形状） |
| `apps/web/tests/kh-stores.test.js` | 新建 | 从 `stores.test.js` 迁入（42 断言） |
| `apps/web/tests/kh-review.test.js` | 新建 | 从 `review.test.js` 迁入（20 断言） |
| `apps/web/tests/kh-graph.test.js` | 新建 | 从 `graph.test.js` 迁入（11 断言） |
| `apps/web/tests/kh-pages.test.js` | 新建 | 从 `pages.test.js` 迁入（41 断言） |
| `apps/web/tests/kh-pages2.test.js` | 新建 | 从 `pages2.test.js` 迁入（67 断言） |
| `apps/web/tests/kh-view.test.js` | 新建 | 视图层新写：渲染断言 + Canvas 组件断言 |

---

## Task 1：`platform.js` 补 `setClipboardData` 之外的缺口（核实后确认无需补）

**Files:**
- 无（先核实，再决定）

**Interfaces:**
- Consumes: 计划二的 `shared/platform.js`
- Produces: 无

> 这个 Task 的目的是**核实**而不是盲写。knowledge-hub 可能需要计划二没覆盖的 wx API。

- [ ] **Step 1: 扫描 knowledge-hub 全部 wx 调用点**

Run:
```bash
grep -ohrE "wx\.[a-zA-Z]+" apps/knowledge-hub/pages apps/knowledge-hub/components apps/knowledge-hub/core \
  | sort | uniq -c | sort -rn
```

Expected: 得到一份去重后的 wx API 清单及调用次数。

- [ ] **Step 2: 逐个核对计划二的 `platform.js` 是否已覆盖**

对 Step 1 得到的每个 API，比对 `apps/web/shared/platform.js` 的导出：

| wx API | 计划二是否已实现 |
|---|---|
| `getStorageSync` / `setStorageSync` / `removeStorageSync` / `getStorageInfoSync` | ✅ `shared/core/storage.js` 已不用 wx |
| `getSystemInfoSync` / `onThemeChange` | ✅ `shared/core/theme.js` 已改用 CSS 变量 + matchMedia |
| `setNavigationBarColor` / `setTabBarStyle` | ✅ 不再需要（Web 无原生导航栏） |
| `vibrateShort` | ✅ 计划二 Task 1 补了 |
| `showToast` / `showModal` / `showActionSheet` | ✅ 计划一 |
| `navigateTo` / `navigateBack` / `switchTab` | ✅ 计划一 |
| `showLoading` / `hideLoading` | ✅ 计划一 |
| `chooseMedia` / `previewImage` | ✅ 计划一 |
| `stopPullDownRefresh` | ✅ 计划一 |
| `setClipboardData` / `getClipboardData` | ✅ 计划二 Task 1 补了 |

**若 Step 1 发现清单里有计划二没覆盖的 API**（例如 `wx.createSelectorQuery`），在本 Task 里补进 `platform.js`，并照计划二 Task 1 的格式加测试。

`wx.createSelectorQuery` 的处理：knowledge-hub 的两个 Canvas 组件用它取 canvas 节点。**在 Web 侧直接用 `document.querySelector` + `getBoundingClientRect`，不走 platform** —— 组件自己改就行，不需要在 platform 里加。

- [ ] **Step 3: 记录结论并提交（若无改动则跳过提交）**

若 Step 2 发现缺口，补完后：

```bash
git add apps/web
git commit -m "feat(web): platform.js 补 knowledge-hub 需要的 API"
git push origin main
```

若无需改动，在本 Task 的执行记录里写明「knowledge-hub 的 wx 调用面已全部被 platform.js 覆盖」，**不产生 commit**。

---

## Task 2：数据层 —— `shared/knowledge/`（7 个模块）

**Files:**
- Create: `apps/web/shared/knowledge/keys.js`
- Create: `apps/web/shared/knowledge/seed.js`
- Create: `apps/web/shared/knowledge/seed-data.js`
- Create: `apps/web/shared/knowledge/review.js`
- Create: `apps/web/shared/knowledge/kh-constants.js`
- Create: `apps/web/shared/knowledge/graph-layout.js`
- Create: `apps/web/shared/knowledge/radar-layout.js`
- Create: `apps/web/shared/knowledge/stores.js`

**Interfaces:**
- Consumes: 计划一的 `shared/core/storage.js` `store.js` `id.js` `date.js` `format.js` `constants.js`
- Produces:
  - `shared/knowledge/keys.js` 导出 `KEYS`（14 个存储键）
  - `shared/knowledge/seed.js` 导出 `KNOWLEDGE_NODES` `KNOWLEDGE_LINKS` `STUDY_PROJECTS` `INTERVIEW_COMPANIES` `MOCK_SESSIONS` `LEARNING_PATHS` `REVIEW_SEED`
  - `shared/knowledge/seed-data.js` 导出 `STACKS` `QUIZ_BANK` `LEARN_DEEP` `RADAR_ITEMS` `FIVE_YEAR_CONFIG` `ALL_YEARS` `CORE_PROJECTS` `getYearConfig` `getAllItems` `getGroupedItems` `INTERVIEW_BANK` `INITIAL_EXCERPTS`
  - `shared/knowledge/review.js` 导出 `createReview` `calculateNextReview` `isDue` `filterDue`
  - `shared/knowledge/kh-constants.js` 导出 knowledge-hub 专属常量
  - `shared/knowledge/graph-layout.js` 导出 `DOMAIN_COLORS` `domainColor` `layout`
  - `shared/knowledge/radar-layout.js` 导出 `QUADRANT_META` `RING_META` `QUADRANT_ORDER` `RING_ORDER` `quadrantBubbles` `resolveCollisions` `stackPolygon`
  - `shared/knowledge/stores.js` 导出 14 个域：`excerpts` `knowledge` `studyProjects` `interviews` `mockInterviews` `learningPaths` `reviews` `daily` `fiveYearPlan` `interviewBank` `quizzes` `settings` `user` `notifications`

- [ ] **Step 1: 建目录并搬 5 个零改动文件**

```bash
mkdir -p apps/web/shared/knowledge
cp apps/knowledge-hub/core/seed-data.js   apps/web/shared/knowledge/seed-data.js
cp apps/knowledge-hub/core/review.js       apps/web/shared/knowledge/review.js
cp apps/knowledge-hub/core/kh-constants.js apps/web/shared/knowledge/kh-constants.js
cp apps/knowledge-hub/core/graph-layout.js apps/web/shared/knowledge/graph-layout.js
cp apps/knowledge-hub/core/radar-layout.js apps/web/shared/knowledge/radar-layout.js
```

这 5 个文件的 require 需改（实测依赖如下）：

| 文件 | 原 require | 改为 |
|---|---|---|
| `seed-data.js` | 无（纯数据） | 不改 |
| `review.js` | 无 | 不改 |
| `kh-constants.js` | 需核实 | 按实际 |
| `graph-layout.js` | 需核实 | 按实际 |
| `radar-layout.js` | 需核实 | 按实际 |

核实命令：
```bash
grep -n "require(" apps/knowledge-hub/core/{kh-constants,graph-layout,radar-layout}.js
```

凡指向 `./xxx.js` 的，改为 `../core/xxx.js`（core 层已在 `shared/core/`）。

- [ ] **Step 2: 搬 `seed.js` 并改 require**

```bash
cp apps/knowledge-hub/core/seed.js apps/web/shared/knowledge/seed.js
```

`seed.js` 的 require 从 `./id.js` `./date.js` 改为 `../core/id.js` `../core/date.js`：

```javascript
const id = require("../core/id.js")
const date = require("../core/date.js")
```

- [ ] **Step 3: 写 `shared/knowledge/keys.js`**

新建 `apps/web/shared/knowledge/keys.js`：

```javascript
"use strict"
/**
 * keys.js —— knowledge-hub 的存储键表
 *
 * settings 保持 `settings`（即 db:settings）—— programmer-workbench 已改用
 * `db:wb_settings`，两个模块不再抢同一个键。
 */

const KEYS = {
  daily: "daily",
  excerpts: "excerpts",
  five_year_plan: "five_year_plan",
  interview_bank: "interview_bank",
  interviews: "interviews",
  knowledge: "knowledge",
  learning_paths: "learning_paths",
  mock_interviews: "mock_interviews",
  notifications: "notifications",
  study_projects: "study_projects",
  quizzes: "quizzes",
  reviews: "reviews",
  settings: "settings",
  user: "user",
  seeded: "__seeded__"
}

module.exports = { KEYS: KEYS }
```

- [ ] **Step 4: 写 `shared/knowledge/stores.js`**

从 `apps/knowledge-hub/core/stores.js` 搬（697 行），**只改三处**：

**改 1 —— require 路径：**

```javascript
const store = require("../core/store.js")
const storage = require("../core/storage.js")
const seed = require("./seed.js")
const data = require("./seed-data.js")
const review = require("./review.js")
const khc = require("./kh-constants.js")
const dateUtil = require("../core/date.js")
```

**改 2 —— `Object.assign(storage.KEYS, KH_KEYS)` 改成本地副本：**

原代码（`stores.js` 第 39–41 行，**三行变一行**）：
```javascript
// KH 使用 14 个 db: 键，扩展 storage.KEYS（KH 自己的 core 副本，不影响 PWB）
const KH_KEYS = { daily: 'daily', /* ... */ }
Object.assign(storage.KEYS, KH_KEYS)

const K = storage.KEYS
```

改成（**注意：原来的 `const K = storage.KEYS` 那行要一起删掉**，否则会报 `Identifier 'K' has already been declared`）：
```javascript
// KH 使用 14 个 db: 键。
//
// 这里**不能**用 Object.assign(storage.KEYS, KH_KEYS) —— 那会就地修改共享的
// storage.KEYS 对象，合并成单一应用后会影响其它模块。改成操作本地副本。
const KH_KEYS = { daily: 'daily', /* ... */ }

const K = Object.assign({}, storage.KEYS, KH_KEYS)
```

> `K` 从此是 knowledge-hub 自己的键表，与 `storage.KEYS`（共享的 12 个基础键）**不再同一引用**。domain 里凡是用 `K.xxx` 的都不受影响。
>
> **改完自检**：`grep -n "storage.KEYS" apps/web/shared/knowledge/stores.js` 应该只剩 `const K = Object.assign({}, storage.KEYS, KH_KEYS)` 这一处。

**改 3 —— `require('./date.js')`（在 `stores.js` 内部动态 require 处）：**

搜 `stores.js` 里的 `require('./date.js')`，改成 `require('../core/date.js')`。

- [ ] **Step 5: 跑一次冒烟，确认 14 个域都能加载**

```bash
cd apps/web && node -e "
const m = new Map();
global.localStorage = {
  getItem: k => m.has(k) ? m.get(k) : null,
  setItem: (k,v) => m.set(k, String(v)),
  removeItem: k => m.delete(k),
  key: i => { const a=[...m.keys()]; return a[i]===undefined?null:a[i] },
  get length () { return m.size }
};
const stores = require('./shared/knowledge/stores.js');
const DOMAINS = ['excerpts','knowledge','studyProjects','interviews','mockInterviews','learningPaths','reviews','daily','fiveYearPlan','interviewBank','quizzes','settings','user','notifications'];
const missing = DOMAINS.filter(d => !stores[d]);
if (missing.length) { console.error('缺少域:', missing); process.exit(1); }
console.log('14 个域齐全');
console.log('excerpts 种子', stores.excerpts.getAll().length, '条');
console.log('knowledge 节点', stores.knowledge.getAll().length, '个');
console.log('settings.primaryColor', stores.settings.get().primaryColor);
"
```

Expected:
```
14 个域齐全
excerpts 种子 4 条
knowledge 节点 14 个
settings.primaryColor violet
```

> `primaryColor` 必须是 `violet`（knowledge-hub 的默认值）—— 若显示 `sky`，说明读到了 workbench 的设置，键搞混了。

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 抽出 knowledge-hub 数据层到 shared/knowledge/

seed-data / review / kh-constants / graph-layout / radar-layout 五个文件
零改动搬入（坐标计算已是纯函数，无 wx 依赖）。

stores.js 搬入，改三处：require 路径、动态 require 的 date.js 路径、
以及把 Object.assign(storage.KEYS, KH_KEYS) 改成本地副本 ——
原写法会就地修改共享的 storage.KEYS，合并成单一应用后会影响其它模块。

keys.js 独立建表，settings 保持 db:settings（workbench 已改用 wb_settings）。"
git push origin main
```

---

## Task 3：迁入 5 个测试文件（181 个断言）

**Files:**
- Create: `apps/web/tests/kh-stub.js`
- Create: `apps/web/tests/kh-harness.js`
- Modify: `apps/web/tests/wb-harness.js`（加点路径 setData 支持）
- Create: `apps/web/tests/kh-stores.test.js`
- Create: `apps/web/tests/kh-review.test.js`
- Create: `apps/web/tests/kh-graph.test.js`
- Create: `apps/web/tests/kh-pages.test.js`
- Create: `apps/web/tests/kh-pages2.test.js`

**Interfaces:**
- Consumes: Task 2 的 `shared/knowledge/*`
- Produces:
  - `tests/kh-harness.js` 导出 `createPage(route)` `createComponent(route, props)` `fire` `tap` `mockPlatform` `lastEvent` `lastCall` `resetCalls`
  - `tests/kh-stub.js` 导出 `installLocalStorage` `reset` `raw` `dbKeys`
  - 5 个迁入的测试文件

- [ ] **Step 1: 给 `wb-harness.js` 加点路径 setData 支持**

knowledge-hub 有 12 处点路径 `setData`（`'form.title'` `'current.bookmarked'` 等）。计划二的 `wb-harness.js` 只有浅合并，**必须补上**，否则 knowledge-hub 的页面测试跑不了。

**先给 `wb-harness.js` 加一个可复用的实现**（两个 harness 共用）：

在 `apps/web/tests/wb-harness.js` 里，把 `setData` 换成：

```javascript
  /**
   * 浅合并 setData，支持 'a.b' 形式的路径（与小程序一致）。
   * 未知中间层自动补对象。
   */
  inst.setData = function (patch) {
    Object.keys(patch).forEach(function (k) {
      if (k.indexOf(".") < 0) {
        inst.data[k] = patch[k]
        inst.state = Object.assign({}, inst.state, patch)
        return
      }
      const parts = k.split(".")
      let node = inst.data
      for (let i = 0; i < parts.length - 1; i++) {
        if (typeof node[parts[i]] !== "object" || node[parts[i]] === null) node[parts[i]] = {}
        node = node[parts[i]]
      }
      node[parts[parts.length - 1]] = patch[k]
      inst.state = Object.assign({}, inst.state, patch)
    })
  }
```

> `inst.state` 也要同步更新 —— Web 页面用 `this.state` 而不是 `this.data`。

- [ ] **Step 2: 建 `tests/kh-stub.js`**

新建 `apps/web/tests/kh-stub.js`：

```javascript
"use strict"
/**
 * kh-stub.js —— Node 环境下的浏览器平台替身
 *
 * 与 wb-stub.js 同构，独立一份便于各模块单独演进。
 */

/** 内存版 localStorage */
const memory = new Map()

function installLocalStorage () {
  global.localStorage = {
    getItem: function (k) { return memory.has(k) ? memory.get(k) : null },
    setItem: function (k, v) { memory.set(k, String(v)) },
    removeItem: function (k) { memory.delete(k) },
    clear: function () { memory.clear() },
    key: function (i) {
      const keys = Array.from(memory.keys())
      return i < keys.length ? keys[i] : null
    },
    get length () { return memory.size }
  }
}

/** 清空内存存储 */
function reset () {
  memory.clear()
}

/** 直接读内存，便于断言 */
function raw (key) {
  return memory.has(key) ? memory.get(key) : undefined
}

/** 读取全部 db: 前缀的键 */
function dbKeys () {
  return Array.from(memory.keys()).filter(function (k) {
    return k.indexOf("db:") === 0
  })
}

module.exports = {
  installLocalStorage: installLocalStorage,
  reset: reset,
  raw: raw,
  dbKeys: dbKeys,
  memory: memory
}
```

- [ ] **Step 3: 建 `tests/kh-harness.js`**

新建 `apps/web/tests/kh-harness.js`：

```javascript
"use strict"
/**
 * kh-harness.js —— knowledge-hub 页面加载脚手架
 *
 * 对应小程序版 tests/page-harness.js：
 *   - 加载真实页面模块，构造可直接调用生命周期的实例
 *   - setData 支持 'a.b' 点路径（knowledge-hub 有 12 处这种写法）
 *   - 交互层从 wx.* 换成 platform.*
 */

const path = require("path")

/** 平台层调用记录 */
const calls = []

/** 最小可用的 platform 替身 */
function installPlatformStub () {
  const noop = function () {}

  global.__KH_PLATFORM_STUB__ = {
    showToast: function (opts) { calls.push({ name: "showToast", opts: opts }); return null },
    showModal: function (opts) {
      calls.push({ name: "showModal", opts: opts })
      return Promise.resolve({ confirm: false, content: "" })
    },
    showActionSheet: function (opts) {
      calls.push({ name: "showActionSheet", opts: opts })
      return Promise.resolve({ tapIndex: -1 })
    },
    navigateTo: function (url) { calls.push({ name: "navigateTo", url: url }) },
    navigateBack: function () { calls.push({ name: "navigateBack" }) },
    switchTab: function (url) { calls.push({ name: "switchTab", url: url }) },
    showLoading: function (opts) { calls.push({ name: "showLoading", opts: opts }) },
    hideLoading: function () { calls.push({ name: "hideLoading" }) },
    setClipboardData: function (opts) {
      calls.push({ name: "setClipboardData", opts: opts })
      return Promise.resolve()
    },
    getClipboardData: function () {
      calls.push({ name: "getClipboardData" })
      return Promise.resolve({ data: "" })
    },
    vibrateShort: function (opts) { calls.push({ name: "vibrateShort", opts: opts }) },
    chooseMedia: function () { return Promise.reject(new Error("当前环境不支持选择文件")) },
    previewImage: function (opts) { calls.push({ name: "previewImage", opts: opts }) },
    stopPullDownRefresh: noop
  }
}

installPlatformStub()

/**
 * 加载页面并返回可调用实例。
 * @param {string} route 形如 "pages/dashboard/index"
 */
function createPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "knowledge", route + ".js")
  let config = null
  global.__KH_PAGE__ = function (o) { config = o }

  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__KH_PAGE__
  }

  if (!config) throw new Error("页面未调用 __KH_PAGE__(): " + route)

  const inst = {}
  Object.keys(config).forEach(function (k) { inst[k] = config[k] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)

  /**
   * 浅合并 setData，支持 'a.b' 路径（与小程序一致）。
   */
  inst.setData = function (patch) {
    Object.keys(patch).forEach(function (k) {
      if (k.indexOf(".") < 0) {
        inst.data[k] = patch[k]
        return
      }
      const parts = k.split(".")
      let node = inst.data
      for (let i = 0; i < parts.length - 1; i++) {
        if (typeof node[parts[i]] !== "object" || node[parts[i]] === null) node[parts[i]] = {}
        node = node[parts[i]]
      }
      node[parts[parts.length - 1]] = patch[k]
    })
    inst.state = Object.assign({}, inst.state, patch)
  }
  return inst
}

/**
 * 加载组件并返回可调用实例。
 *
 * 组件是**工厂函数**形态（create(props)），不是小程序的 Component() ——
 * 小程序那套依赖框架注入，Web 侧没有，硬造不值当。
 *
 * @param {string} route 形如 "components/knowledge-graph"
 * @param {object} [props] 组件属性
 */
function createComponent (route, props) {
  const abs = path.resolve(__dirname, "..", "web", "knowledge", route + ".js")
  delete require.cache[require.resolve(abs)]
  const mod = require(abs)
  if (!mod || typeof mod.create !== "function") {
    throw new Error("组件未导出 create(): " + route)
  }
  return mod.create(props || {})
}

/** 取某个事件的最后一次触发 */
function lastEvent (inst, name) {
  for (let i = inst.__events.length - 1; i >= 0; i--) {
    if (inst.__events[i].name === name) return inst.__events[i]
  }
  return null
}

/** 模拟 input / change 事件 */
function fire (page, handler, value) {
  page[handler]({ detail: { value: value } })
}

/** 模拟 click 事件（带 dataset） */
function tap (page, handler, dataset) {
  page[handler]({ currentTarget: { dataset: dataset || {} } })
}

/** 临时替换某个平台 API，返回还原函数 */
function mockPlatform (name, impl) {
  const target = global.__KH_PLATFORM_STUB__
  const original = target[name]
  target[name] = impl
  return function () { target[name] = original }
}

/** 取某个平台 API 的最后一次调用 */
function lastCall (name) {
  for (let i = calls.length - 1; i >= 0; i--) {
    if (calls[i].name === name) return calls[i]
  }
  return null
}

/** 清空调用记录 */
function resetCalls () {
  calls.length = 0
}

module.exports = {
  createPage: createPage,
  createComponent: createComponent,
  lastEvent: lastEvent,
  fire: fire,
  tap: tap,
  mockPlatform: mockPlatform,
  lastCall: lastCall,
  resetCalls: resetCalls
}
```

- [ ] **Step 4: 迁入 5 个测试文件**

```bash
cp apps/knowledge-hub/tests/stores.test.js  apps/web/tests/kh-stores.test.js
cp apps/knowledge-hub/tests/review.test.js  apps/web/tests/kh-review.test.js
cp apps/knowledge-hub/tests/graph.test.js   apps/web/tests/kh-graph.test.js
cp apps/knowledge-hub/tests/pages.test.js   apps/web/tests/kh-pages.test.js
cp apps/knowledge-hub/tests/pages2.test.js  apps/web/tests/kh-pages2.test.js
```

每个文件做**同样的三处改法**：

**改 1 —— require 路径：**

| 原 | 新 |
|---|---|
| `require('./wx-stub.js')` | `require('./kh-stub.js')` |
| `require('./page-harness.js')` | `require('./kh-harness.js')` |
| `require('../core/stores.js')` | `require('../shared/knowledge/stores.js')` |
| `require('../core/review.js')` | `require('../shared/knowledge/review.js')` |
| `require('../core/graph-layout.js')` | `require('../shared/knowledge/graph-layout.js')` |
| `require('../core/radar-layout.js')` | `require('../shared/knowledge/radar-layout.js')` |
| `require('../core/kh-constants.js')` | `require('../shared/knowledge/kh-constants.js')` |
| `require('../core/date.js')` | `require('../shared/core/date.js')` |
| `require('../core/storage.js')` | `require('../shared/core/storage.js')` |
| `require('../core/seed-data.js')` | `require('../shared/knowledge/seed-data.js')` |

**改 2 —— `stub.install()` → `stub.installLocalStorage()`。**

**改 3 —— `h.mockWx('x', fn)` → `h.mockPlatform('x', fn)`。**（若有）

> **迁移纪律**：5 个文件共 181 个断言，**只改 require 路径与 API 名称，不许删断言、不许改断言值**。
>
> **保留 `ok(name, cond)` 风格** —— knowledge-hub 的测试是自制的 `ok()` 计数风格，不是 `node:test`。不要改写成 `node:test`，改了就得重排断言顺序。

- [ ] **Step 5: 运行测试确认失败**

Run:
```bash
cd apps/web && node tests/kh-stores.test.js
```
Expected: FAIL —— 报模块找不到，或页面模块找不到（`web/knowledge/pages/...` 还不存在）

- [ ] **Step 6: 建 17 个页面占位文件**

建 `apps/web/web/knowledge/pages/` 及子目录，每个占位文件：

```javascript
"use strict"
/**
 * <页面名> —— 占位，Task 5 填实现
 */

global.__KH_PAGE__({
  data: {
    title: "<页面名>"
  },

  render: function (state) {
    return null
  }
})
```

17 个占位文件与小程序路由一一对应：

| 占位文件 | 对应小程序路由 |
|---|---|
| `pages/dashboard/index.js` | `pages/dashboard/index` |
| `pages/learn/index.js` | `pages/learn/index` |
| `pages/learn/quiz/index.js` | `pages/learn/quiz/index` |
| `pages/learn/review/index.js` | `pages/learn/review/index` |
| `pages/learn/deep/index.js` | `pages/learn/deep/index` |
| `pages/learn/path/index.js` | `pages/learn/path/index` |
| `pages/interview/index.js` | `pages/interview/index` |
| `pages/interview/tracker/index.js` | `pages/interview/tracker/index` |
| `pages/interview/bank/index.js` | `pages/interview/bank/index` |
| `pages/interview/mock/index.js` | `pages/interview/mock/index` |
| `pages/knowledge/index.js` | `pages/knowledge/index` |
| `pages/knowledge/graph/index.js` | `pages/knowledge/graph/index` |
| `pages/knowledge/excerpt/index.js` | `pages/knowledge/excerpt/index` |
| `pages/knowledge/radar/index.js` | `pages/knowledge/radar/index` |
| `pages/profile/index.js` | `pages/profile/index` |
| `pages/plan/five-year/index.js` | `pages/plan/five-year/index` |
| `pages/plan/roadmap/index.js` | `pages/plan/roadmap/index` |

目录创建命令：
```bash
mkdir -p apps/web/web/knowledge/pages/{learn/{quiz,review,deep,path},interview/{tracker,bank,mock},knowledge/{graph,excerpt,radar},plan/{five-year,roadmap}}
```

- [ ] **Step 7: 运行测试确认通过**

Run:
```bash
cd apps/web && node tests/kh-stores.test.js && node tests/kh-review.test.js && node tests/kh-graph.test.js && node tests/kh-pages.test.js && node tests/kh-pages2.test.js
```
Expected: **全部 PASS** —— 共 181 个断言

> **这一步是最关键的验收点**：181 个断言全过，说明**数据层已完整搬过来**。
>
> 若某个断言失败，**先怀疑迁移改错了**（require 路径、键名、`ok()` 风格被改动），不要改断言值。

- [ ] **Step 8: 提交**

```bash
git add apps/web
git commit -m "test(web): 迁入 knowledge-hub 的 5 个测试文件（181 断言）

新建 kh-harness.js —— 支持点路径 setData（knowledge-hub 有 12 处
'form.title' 这种写法），同时给 wb-harness.js 补上同样的支持。

stores/review/graph/pages/pages2 五个文件迁入，只改 require 路径与
API 名称，保留原有的 ok() 计数风格，181 个断言全部通过。"
git push origin main
```

---

## Task 4：`shared/ui.js` 加 `progressBar` 构件

**Files:**
- Modify: `apps/web/shared/ui.js`
- Modify: `apps/web/tests/ui.test.js`

**Interfaces:**
- Consumes: 计划一的 `shared/ui.js`
- Produces: `shared/ui.js` 新增导出 `progressBar(percent, opts)`
  - `opts: { color?: string, height?: number, label?: string }`
  - 返回 DOM 节点，类名 `progress` / `progress-fill` / `progress-label`

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/ui.test.js` 末尾追加：

```javascript
test("progressBar：渲染出进度条与填充", function () {
  const node = ui.progressBar(40, { color: "#6366F1" })
  assert.strictEqual(node.className, "progress")
  assert.ok(node.textContent.indexOf("40%") >= 0, "应含百分比文本，实际：" + node.textContent)
})

test("progressBar：percent 会被夹到 0..100", function () {
  const node = ui.progressBar(150)
  assert.ok(node.textContent.indexOf("100%") >= 0, "超出应夹到 100，实际：" + node.textContent)
  const node2 = ui.progressBar(-20)
  assert.ok(node2.textContent.indexOf("0%") >= 0, "负数应夹到 0，实际：" + node2.textContent)
})

test("progressBar：可传高度与标签", function () {
  const node = ui.progressBar(30, { height: 12, label: "掌握度" })
  assert.ok(node.textContent.indexOf("掌握度") >= 0)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/ui.test.js`
Expected: FAIL —— 报 `ui.progressBar is not a function`

- [ ] **Step 3: 实现 `progressBar`**

在 `apps/web/shared/ui.js` 的 `empty` 函数之后、返回对象之前，插入：

```javascript
  /**
   * 进度条。
   * @param {number} percent 0..100，超出自动夹紧
   * @param {{color?: string, height?: number, label?: string}} [opts]
   * @returns {object} DOM 节点
   */
  function progressBar (percent, opts) {
    const o = opts || {}
    let p = Number(percent)
    if (!isFinite(p)) p = 0
    if (p < 0) p = 0
    if (p > 100) p = 100
    const rounded = Math.round(p)

    const fill = el("div", { class: "progress-fill" })
    fill.style = { width: rounded + "%" }
    if (o.color) fill.style.background = o.color
    if (o.height) fill.style.height = o.height + "px"

    const label = el("span", { class: "progress-label", text: rounded + "%" })
    const children = [fill]
    if (o.label) children.unshift(el("span", { class: "progress-name", text: o.label }))

    const root = el("div", { class: "progress" }, children)
    if (o.height) root.style = { height: o.height + "px" }
    root.appendChild(label)
    return root
  }
```

在返回对象里补上：

```javascript
    progressBar: progressBar,
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd apps/web && node tests/ui.test.js`
Expected: PASS —— 16 个用例全过（原 13 + 新 3）

- [ ] **Step 5: 在 `web/style.css` 里加进度条样式**

在 `apps/web/web/style.css` 的「药丸」小节之后插入：

```css
/* 进度条 */
.progress {
  position: relative;
  display: flex;
  align-items: center;
  gap: 8px;
  height: 8px;
  border-radius: 999px;
  background: var(--bg-subtle);
  overflow: visible;
}

.progress-fill {
  height: 100%;
  border-radius: 999px;
  background: var(--brand);
  transition: width 0.2s ease;
}

.progress-name {
  font-size: 12px;
  color: var(--text-secondary);
  flex: 0 0 auto;
}

.progress-label {
  font-size: 11px;
  color: var(--text-muted);
  font-variant-numeric: tabular-nums;
  margin-left: auto;
}
```

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): shared/ui.js 加 progressBar 构件

knowledge-hub 的 progress-bar 组件与 ui.js 的构件重叠，本计划不移植
那个组件，改用新增的 progressBar 构件。percent 自动夹到 0..100。"
git push origin main
```

---

## Task 5：视图层 —— 17 个页面 + 2 个 Canvas 组件

**Files:**
- Modify: `apps/web/web/knowledge/pages/**`（17 个占位文件）
- Create: `apps/web/web/knowledge/components/knowledge-graph.js`
- Create: `apps/web/web/knowledge/components/radar-chart.js`
- Create: `apps/web/web/knowledge/router.js`
- Create: `apps/web/web/knowledge/style.css`
- Test: `apps/web/tests/kh-view.test.js`

**Interfaces:**
- Consumes: Task 2 的 `shared/knowledge/stores.js`；计划一的 `shared/ui.js` `shared/platform.js` `shared/charts.js`
- Produces:
  - 17 个页面模块：`{ data, onMount?(rootEl), render(state) → HTMLElement }`，用 `global.__KH_PAGE__({...})` 注册
  - 2 个 Canvas 组件：**工厂函数形态**，模块导出 `create(props)`，返回 `{ data, setData, draw, attachCanvas, destroy }`
  - `web/knowledge/router.js` 导出 `ROUTES`（17 条）与 `resolve(pathname)`

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/kh-view.test.js`：

```javascript
"use strict"
/**
 * kh-view.test.js —— 视图层：页面能加载、能渲染，Canvas 组件能画
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const h = require("./kh-harness.js")
const stub = require("./kh-stub.js")

stub.installLocalStorage()

const PAGES = [
  "pages/dashboard/index",
  "pages/learn/index",
  "pages/learn/quiz/index",
  "pages/learn/review/index",
  "pages/learn/deep/index",
  "pages/learn/path/index",
  "pages/interview/index",
  "pages/interview/tracker/index",
  "pages/interview/bank/index",
  "pages/interview/mock/index",
  "pages/knowledge/index",
  "pages/knowledge/graph/index",
  "pages/knowledge/excerpt/index",
  "pages/knowledge/radar/index",
  "pages/profile/index",
  "pages/plan/five-year/index",
  "pages/plan/roadmap/index"
]

test("17 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都产出节点，且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("渲染结果不含 undefined / NaN", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const text = page.render(page.state).textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 17 个页面", function () {
  const { ROUTES } = require("../web/knowledge/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 17)
})

test("知识图谱组件：布局计算产出坐标", function () {
  const { layout } = require("../shared/knowledge/graph-layout.js")
  // 节点形状：{id, name, domain, level}；domain 是中文领域名（决定列与配色）
  const nodes = [
    { id: "a", name: "A", domain: "算法", level: 1 },
    { id: "b", name: "B", domain: "系统设计", level: 2 }
  ]
  // 链接形状：{source, target}（不是 from/to —— 布局内部才转成 from/to）
  const links = [{ source: "a", target: "b" }]
  const geo = layout(nodes, links, { width: 300, height: 200 })
  assert.strictEqual(geo.points.length, 2)
  assert.strictEqual(geo.edges.length, 1)
  geo.points.forEach(function (p) {
    assert.strictEqual(typeof p.x, "number")
    assert.strictEqual(typeof p.y, "number")
    assert.ok(p.x >= 0 && p.x <= 300, "x 应在画布内，实际 " + p.x)
    assert.ok(p.y >= 0 && p.y <= 200, "y 应在画布内，实际 " + p.y)
  })
})

test("技术雷达组件：象限气泡与栈多边形都能算出来", function () {
  const radar = require("../shared/knowledge/radar-layout.js")
  // 象限气泡的项形状：{id, title, quadrant, ring, recommend}
  const items = [{ id: "x", title: "C++", quadrant: "language", ring: "basic", recommend: 2 }]
  const geo = radar.quadrantBubbles(items, { width: 300, height: 300 })
  assert.strictEqual(geo.points.length, 1)
  assert.ok(geo.points[0].x >= 0 && geo.points[0].x <= 300)
  // 栈多边形的行形状：{key, label, color, percent}
  const stack = radar.stackPolygon(
    [{ key: "lang", label: "语言", color: "#0284C7", percent: 70 }],
    { width: 300, height: 300 }
  )
  assert.ok(Array.isArray(stack.axes))
  assert.ok(stack.axes.length >= 1)
  assert.ok(Array.isArray(stack.points))
})

test("两个 Canvas 组件都能加载，且 draw 方法存在", function () {
  const graph = h.createComponent("components/knowledge-graph")
  assert.strictEqual(typeof graph.draw, "function")
  const radar = h.createComponent("components/radar-chart")
  assert.strictEqual(typeof radar.draw, "function")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/kh-view.test.js`
Expected: FAIL —— 占位页面的 `render` 返回 `null`，且 `web/knowledge/router.js` 不存在

- [ ] **Step 3: 写两个 Canvas 组件（Web 版）**

新建 `apps/web/web/knowledge/components/knowledge-graph.js`：

```javascript
"use strict"
/**
 * knowledge-graph —— 知识图谱画布（Canvas 2d）
 *
 * 与小程序版的差异只有「怎么拿到 canvas」：
 *   小程序：wx.createSelectorQuery().in(this).select('#graph').fields({node:true,size:true})
 *   Web   ：getBoundingClientRect
 *
 * 绘制代码逐字保留 —— Canvas 2D API（clearRect / beginPath / arc / fill /
 * stroke / moveTo / lineTo）两边完全一致。
 * 坐标计算在 shared/knowledge/graph-layout.js（纯函数，已有单测）。
 *
 * 【为什么用工厂函数而不是 Component()】
 * 小程序的 Component() 依赖框架注入 this.setData / this.triggerEvent /
 * properties 观察者。本仓零构建、无框架，硬造一套不值当。
 * 改成工厂函数后组件是普通对象，页面直接持有实例并调 draw()。
 */

const layout = require("../../../shared/knowledge/graph-layout.js")

/**
 * 创建知识图谱组件实例。
 * @param {{nodes?: Array, links?: Array, selectedId?: string, brand?: string}} props
 * @returns {{data: object, setData: Function, draw: Function, attachCanvas: Function, destroy: Function}}
 */
function create (props) {
  const p = props || {}
  const inst = {
    data: {
      nodes: p.nodes || [],
      links: p.links || [],
      selectedId: p.selectedId || "",
      brand: p.brand || "#6366F1",
      ready: false,
      width: 0,
      height: 0
    },
    setData: function (patch) {
      Object.keys(patch).forEach(function (k) { inst.data[k] = patch[k] })
    }
  }

  /**
   * 绑定 canvas 并首次绘制。
   * @param {object} canvasEl HTMLCanvasElement
   */
  inst.attachCanvas = function (canvasEl) {
    if (!canvasEl || typeof canvasEl.getContext !== "function") return
    const rect = canvasEl.getBoundingClientRect()
    const width = Math.round(rect.width || canvasEl.clientWidth || 0)
    const height = Math.round(rect.height || canvasEl.clientHeight || 0)
    if (width === 0 || height === 0) return

    const ctx = canvasEl.getContext("2d")
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 2
    // 用 px 画布 + dpr 放大，保证高清
    canvasEl.width = width * dpr
    canvasEl.height = height * dpr
    ctx.scale(dpr, dpr)

    inst.__ctx = ctx
    inst.setData({ ready: true, width: width, height: height })
    inst.draw()
  }

  /**
   * 绘制图谱。坐标由 graph-layout.js 算出，本方法只负责画。
   */
  inst.draw = function () {
    const ctx = inst.__ctx
    if (!ctx) return
    const nodes = inst.data.nodes
    const links = inst.data.links
    if (!nodes.length) return

    const w = inst.data.width
    const h = inst.data.height
    const geo = layout.layout(nodes, links, { width: w, height: h })

    ctx.clearRect(0, 0, w, h)

    // 连线（前置依赖）
    const pos = {}
    geo.points.forEach(function (pt) { pos[pt.id] = pt })
    ctx.strokeStyle = inst.data.brand
    ctx.globalAlpha = 0.35
    ctx.lineWidth = 1
    geo.edges.forEach(function (e) {
      const a = pos[e.from]
      const b = pos[e.to]
      ctx.beginPath()
      ctx.moveTo(a.x, a.y)
      ctx.lineTo(b.x, b.y)
      ctx.stroke()
    })

    // 节点
    ctx.globalAlpha = 1
    geo.points.forEach(function (pt) {
      const selected = pt.id === inst.data.selectedId
      ctx.beginPath()
      ctx.arc(pt.x, pt.y, pt.r, 0, Math.PI * 2)
      ctx.fillStyle = pt.color
      ctx.globalAlpha = selected ? 1 : 0.75
      ctx.fill()
      ctx.globalAlpha = 1
      if (selected) {
        ctx.lineWidth = 2
        ctx.strokeStyle = "#FFFFFF"
        ctx.stroke()
      }
    })
  }

  /** 释放资源 */
  inst.destroy = function () {
    inst.__ctx = null
  }

  return inst
}

module.exports = { create: create }
```

新建 `apps/web/web/knowledge/components/radar-chart.js`：

```javascript
"use strict"
/**
 * radar-chart —— 技术雷达画布（Canvas 2d）
 *
 * 与小程序版的差异只有「怎么拿到 canvas」，绘制代码逐字保留。
 * 坐标计算在 shared/knowledge/radar-layout.js（纯函数，已有单测）。
 *
 * 组件形态同 knowledge-graph.js：工厂函数 create(props)。
 */

const layout = require("../../../shared/knowledge/radar-layout.js")

/**
 * 创建技术雷达组件实例。
 * @param {{mode?: string, items?: Array, rows?: Array, isDark?: boolean}} props
 * @returns {{data: object, setData: Function, draw: Function, attachCanvas: Function, destroy: Function}}
 */
function create (props) {
  const p = props || {}
  const inst = {
    data: {
      mode: p.mode || "quadrant",
      items: p.items || [],
      rows: p.rows || [],
      isDark: !!p.isDark,
      ready: false,
      width: 0,
      height: 0
    },
    setData: function (patch) {
      Object.keys(patch).forEach(function (k) { inst.data[k] = patch[k] })
    }
  }

  /**
   * 绑定 canvas 并首次绘制。
   * @param {object} canvasEl HTMLCanvasElement
   */
  inst.attachCanvas = function (canvasEl) {
    if (!canvasEl || typeof canvasEl.getContext !== "function") return
    const rect = canvasEl.getBoundingClientRect()
    const width = Math.round(rect.width || canvasEl.clientWidth || 0)
    const height = Math.round(rect.height || canvasEl.clientHeight || 0)
    if (width === 0 || height === 0) return

    const ctx = canvasEl.getContext("2d")
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 2
    canvasEl.width = width * dpr
    canvasEl.height = height * dpr
    ctx.scale(dpr, dpr)

    inst.__ctx = ctx
    inst.setData({ ready: true, width: width, height: height })
    inst.draw()
  }

  /** 绘制。坐标由 radar-layout.js 算出，本方法只负责画。 */
  inst.draw = function () {
    const ctx = inst.__ctx
    if (!ctx) return
    const w = inst.data.width
    const h = inst.data.height
    ctx.clearRect(0, 0, w, h)

    if (inst.data.mode === "stack") inst.drawStack(ctx, w, h)
    else inst.drawQuadrant(ctx, w, h)
  }

  /** 象限气泡图 */
  inst.drawQuadrant = function (ctx, w, h) {
    const items = inst.data.items
    if (!items.length) return
    const geo = layout.quadrantBubbles(items, { width: w, height: h })

    // 象限轴线
    ctx.strokeStyle = inst.data.isDark ? "#33404F" : "#E4E7EC"
    ctx.lineWidth = 1
    for (let i = 0; i < 4; i++) {
      const a = (i / 4) * Math.PI * 2
      ctx.beginPath()
      ctx.moveTo(geo.cx, geo.cy)
      ctx.lineTo(geo.cx + Math.cos(a) * geo.radius, geo.cy + Math.sin(a) * geo.radius)
      ctx.stroke()
    }

    // 碰撞推开
    const adjusted = layout.resolveCollisions(geo.points, { minGap: 24, rounds: 8 })

    // 气泡
    adjusted.forEach(function (p, i) {
      const src = geo.points[i]
      ctx.beginPath()
      ctx.arc(p.x, p.y, src.r, 0, Math.PI * 2)
      ctx.fillStyle = src.color
      ctx.globalAlpha = 0.7
      ctx.fill()
      ctx.globalAlpha = 1
    })
  }

  /** 栈雷达多边形 */
  inst.drawStack = function (ctx, w, h) {
    const rows = inst.data.rows
    if (!rows.length) return
    const geo = layout.stackPolygon(rows, { width: w, height: h })

    // 网格环
    ctx.strokeStyle = inst.data.isDark ? "#232B38" : "#E4E7EC"
    ctx.lineWidth = 1
    ;[0.25, 0.5, 0.75, 1].forEach(function (ratio) {
      ctx.beginPath()
      geo.axes.forEach(function (ax, i) {
        const angle = (i / geo.axes.length) * Math.PI * 2 - Math.PI / 2
        const rr = geo.radius * ratio
        const x = geo.cx + Math.cos(angle) * rr
        const y = geo.cy + Math.sin(angle) * rr
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
      })
      ctx.closePath()
      ctx.stroke()
    })

    // 轴线
    ctx.beginPath()
    geo.axes.forEach(function (ax, i) {
      if (i === 0) ctx.moveTo(geo.cx, geo.cy); else ctx.lineTo(ax.x, ax.y)
    })
    ctx.stroke()

    // 数据多边形
    ctx.beginPath()
    geo.points.forEach(function (p, i) {
      if (i === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y)
    })
    ctx.closePath()
    ctx.fillStyle = "rgba(99, 102, 241, 0.24)"
    ctx.fill()
    ctx.strokeStyle = "#6366F1"
    ctx.lineWidth = 2
    ctx.stroke()
  }

  /** 释放资源 */
  inst.destroy = function () {
    inst.__ctx = null
  }

  return inst
}

module.exports = { create: create }
```

- [ ] **Step 4: 写 `web/knowledge/router.js`**

新建 `apps/web/web/knowledge/router.js`：

```javascript
"use strict"
/**
 * router.js —— knowledge-hub 模块内的路由表
 *
 * 路径沿用小程序的页面路径，挂在 /knowledge/ 前缀下。
 * 用懒加载：require 在函数体里，首屏只加载真正要用的页面。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/dashboard/index": function () { return require("./pages/dashboard/index.js") },
  "/pages/learn/index": function () { return require("./pages/learn/index.js") },
  "/pages/learn/quiz/index": function () { return require("./pages/learn/quiz/index.js") },
  "/pages/learn/review/index": function () { return require("./pages/learn/review/index.js") },
  "/pages/learn/deep/index": function () { return require("./pages/learn/deep/index.js") },
  "/pages/learn/path/index": function () { return require("./pages/learn/path/index.js") },
  "/pages/interview/index": function () { return require("./pages/interview/index.js") },
  "/pages/interview/tracker/index": function () { return require("./pages/interview/tracker/index.js") },
  "/pages/interview/bank/index": function () { return require("./pages/interview/bank/index.js") },
  "/pages/interview/mock/index": function () { return require("./pages/interview/mock/index.js") },
  "/pages/knowledge/index": function () { return require("./pages/knowledge/index.js") },
  "/pages/knowledge/graph/index": function () { return require("./pages/knowledge/graph/index.js") },
  "/pages/knowledge/excerpt/index": function () { return require("./pages/knowledge/excerpt/index.js") },
  "/pages/knowledge/radar/index": function () { return require("./pages/knowledge/radar/index.js") },
  "/pages/profile/index": function () { return require("./pages/profile/index.js") },
  "/pages/plan/five-year/index": function () { return require("./pages/plan/five-year/index.js") },
  "/pages/plan/roadmap/index": function () { return require("./pages/plan/roadmap/index.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /knowledge/pages/learn/quiz/index
 * @returns {string|null}
 */
function resolve (pathname) {
  const prefix = "/knowledge"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  const stripped = clean.split("?")[0]
  if (ROUTES[stripped]) return stripped
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
```

- [ ] **Step 5: 写第一个完整页面 —— `pages/dashboard/index.js`**

新建 `apps/web/web/knowledge/pages/dashboard/index.js`：

```javascript
"use strict"
/**
 * dashboard —— 首页（Reading Radar 首屏）
 *
 * 数据层在 shared/knowledge/stores.js，本文件只负责「取数 → 渲染」。
 */

const stores = require("../../../../shared/knowledge/stores.js")
const ui = require("../../../../shared/ui.js")

/** 首页的 4 个入口 */
const ENTRIES = [
  { key: "learn", label: "学习", desc: "题库、复习与深挖", path: "/knowledge/pages/learn/index" },
  { key: "interview", label: "面试", desc: "题库、追踪与模拟", path: "/knowledge/pages/interview/index" },
  { key: "knowledge", label: "知识", desc: "图谱、摘抄与雷达", path: "/knowledge/pages/knowledge/index" },
  { key: "plan", label: "计划", desc: "五年计划与路线图", path: "/knowledge/pages/plan/roadmap/index" }
]

/**
 * 取首页数据。
 * @returns {object} 视图模型
 */
function buildModel () {
  const user = stores.user.get()
  const dueCount = stores.reviews.getDue().length
  const excerpts = stores.excerpts.getAll().length
  const gaps = stores.knowledge.getGaps().length
  const streak = stores.daily.getTrend(7)

  return {
    nickname: user.name || "Yinch",
    entries: ENTRIES,
    stats: [
      { label: "待复习", value: String(dueCount), tone: dueCount > 0 ? "warn" : "" },
      { label: "摘抄", value: String(excerpts) },
      { label: "知识缺口", value: String(gaps) },
      { label: "近 7 天活跃", value: String(streak.length) }
    ]
  }
}

global.__KH_PAGE__({
  data: {
    title: "Reading Radar"
  },

  /** 挂载：读一次数据 */
  onMount: function () {
    this.state = Object.assign({}, this.state, buildModel())
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const model = buildModel()

    const entries = model.entries.map(function (e) {
      return ui.el("a", { class: "kh-entry", href: e.path }, [
        ui.el("span", { class: "kh-entry-title", text: e.label }),
        ui.el("span", { class: "kh-entry-desc", text: e.desc })
      ])
    })

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({
        eyebrow: "Reading Radar",
        title: model.nickname,
        sub: "今天想学点什么？"
      }),
      ui.hero({
        title: "最近要做的",
        sub: "按「复习 → 学习 → 记录」的顺序",
        children: [
          ui.statGrid(model.stats),
          ui.el("div", { class: "kh-entry-grid" }, entries)
        ]
      })
    ])
  }
})
```

> **注意 require 路径**：`pages/dashboard/index.js` 在 `apps/web/web/knowledge/pages/dashboard/` 下，往上 4 层到 `apps/web/`，所以是 `../../../../shared/...`。

- [ ] **Step 6: 写 `web/knowledge/style.css`**

新建 `apps/web/web/knowledge/style.css`：

```css
/* style.css —— knowledge-hub 模块样式
 *
 * 从小程序 WXSS 转来，换算基准「1px = 2rpx（375 设计稿）」。
 */

.kh-page {
  padding-bottom: 24px;
}

.kh-entry-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
  gap: 8px;
  margin-top: 12px;
}

.kh-entry {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 12px;
  border: 0.5px solid var(--border);
  border-radius: 8px;
  background: var(--bg-subtle);
  text-decoration: none;
}

.kh-entry:hover {
  border-color: var(--brand);
}

.kh-entry-title {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-primary);
}

.kh-entry-desc {
  font-size: 12px;
  color: var(--text-muted);
}

.kh-list-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 0;
  border-bottom: 0.5px solid var(--border);
}

.kh-list-row:last-child {
  border-bottom: none;
}

.kh-list-main {
  flex: 1;
  min-width: 0;
}

.kh-list-title {
  font-size: 14px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.kh-list-sub {
  font-size: 12px;
  color: var(--text-muted);
  margin-top: 2px;
}

.kh-form {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.kh-input,
.kh-textarea {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
  font-family: inherit;
}

.kh-textarea {
  min-height: 120px;
  resize: vertical;
  line-height: 1.6;
}

.kh-actions {
  display: flex;
  gap: 8px;
  justify-content: flex-end;
  margin-top: 12px;
}

.kh-btn {
  padding: 7px 16px;
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 13px;
  cursor: pointer;
  font-family: inherit;
}

.kh-btn.primary {
  background: var(--brand);
  border-color: var(--brand);
  color: #ffffff;
}

.kh-btn.danger {
  background: var(--danger);
  border-color: var(--danger);
  color: #ffffff;
}

.kh-canvas-wrap {
  position: relative;
  width: 100%;
  height: 180px;
}

.kh-canvas-wrap canvas {
  width: 100%;
  height: 100%;
  display: block;
}

.kh-md {
  line-height: 1.7;
}

.kh-md pre {
  background: var(--bg-subtle);
  padding: 12px;
  border-radius: 6px;
  overflow-x: auto;
  font-family: Menlo, Consolas, monospace;
  font-size: 12px;
}

.kh-md code {
  background: var(--bg-subtle);
  padding: 1px 4px;
  border-radius: 3px;
  font-family: Menlo, Consolas, monospace;
  font-size: 13px;
}
```

- [ ] **Step 7: 逐页重写其余 16 个页面**

照小程序版页面文件的数据层与交互层逐条搬。**逐页对照表**：

| 页面 | 数据来源 | 事件（对应小程序版处理函数） |
|---|---|---|
| `learn/index` | `stores.learningPaths` `stores.quizzes` 计数 | `onOpen` `onStartQuiz` |
| `learn/quiz/index` | `stores.quizzes.getQuestionBank/searchQuestions/submitAnswer/getStats/getWrongBook` | `onAnswer` `onSubmit` `onNext` `onChangeBatch` `onShowWrongBook` |
| `learn/review/index` | `stores.reviews.getDue/getDone/getUpcoming` `review.createReview` | `onAdd` `onRate` `onDelete` |
| `learn/deep/index` | `seedData.LEARN_DEEP` | `onOpen` |
| `learn/path/index` | `stores.learningPaths.getAll` `getStepProgress` `getPathProgress` | `onOpen` `onToggleSubStep` `onReset` |
| `interview/index` | `stores.interviews` `stores.mockInterviews` 计数 | `onOpen` |
| `interview/tracker/index` | `stores.interviews.getAll` `addInterviewRecord` | `onAddCompany` `onAddRecord` `onDelete` `onCopy` |
| `interview/bank/index` | `stores.interviewBank.get` `seedData.INTERVIEW_BANK` | `onOpen` `onBookmark` `onRate` |
| `interview/mock/index` | `stores.mockInterviews` `stores.quizzes.getQuestionBank` | `onStart` `onSubmit` `onDelete` `onClear` |
| `knowledge/index` | `stores.knowledge.getAll` `stores.excerpts` 计数 | `onOpen` |
| `knowledge/graph/index` | `stores.knowledge.getAll` `stores.knowledge.links` | `onSelect` |
| `knowledge/excerpt/index` | `stores.excerpts.getAll` `derive` | `onAdd` `onCopy` `onDelete` `onToggleFavorite` |
| `knowledge/radar/index` | `seedData.RADAR_ITEMS` `stores.knowledge.getAll` | `onSwitchMode` |
| `profile/index` | `stores.user.get` `stores.settings.get` `storage.exportAll/importAll/clearAll` | `onSave` `onExport` `onImport` `doImport` `onClearData` `onSetPrimary` |
| `plan/five-year/index` | `stores.fiveYearPlan.get/getDay/setDay/toggleCheck/toggleLearning/getStreak/getTotalDays/getMonthPct` | `onEditNote` `onToggleCheck` `onToggleLearning` `onDelete` |
| `plan/roadmap/index` | `stores.studyProjects.getAll` | `onAdd` `onEdit` `onDelete` |

**移植纪律**：
- **数据层逻辑逐字搬**（排序、映射、统计口径一字不改）—— 迁入的 181 个断言就是这块的护栏
- **交互层用 `platform.*` 替换 `wx.*`**，回调式改 Promise 式：

  ```javascript
  // 小程序版
  wx.showModal({
    title: '删除摘抄？',
    content: '删除后无法恢复。',
    confirmText: '删除',
    confirmColor: '#E5484D',
    success: function (m) {
      if (!m.confirm) return
      stores.excerpts.remove(id)
      self.refresh()
      wx.showToast({ title: '已删除', icon: 'success' })
    }
  })

  // Web 版
  platform.showModal({
    title: '删除摘抄？',
    content: '删除后无法恢复。',
    confirmText: '删除',
    confirmColor: '#E5484D'
  }).then(function (m) {
    if (!m.confirm) return
    stores.excerpts.remove(id)
    self.refresh()
    platform.showToast({ title: '已删除', icon: "success" })
  })
  ```

- **点路径 setData 改成直接改 state**：小程序版 `this.setData({ 'form.title': v })`，Web 版改成 `this.state.form.title = v`（或在 `render` 里从 state 现取）。
- **Markdown 用 `md2html()`**（Web 侧 class 生效，不需要内联样式改写）
- **Canvas 页面的组件接法**（以 `knowledge/graph/index.js` 为例）：

  组件是工厂函数，页面在 `render` 里建 canvas、渲染后把 canvas 交给组件实例：

  ```javascript
  const createGraph = require("../../components/knowledge-graph.js")

  global.__KH_PAGE__({
    data: { title: "知识图谱" },

    onMount: function () {
      this.state = Object.assign({}, this.state, buildModel(this.state))
    },

    render: function (state) {
      const model = buildModel(state)
      const canvas = ui.el("canvas")
      // 组件实例：渲染后由 browser.js 调 attachCanvas 绑定画布
      this.__graph = createGraph.create({
        nodes: model.nodes,
        links: model.links,
        selectedId: model.selectedId,
        brand: model.brand
      })
      this.__canvas = canvas
      return ui.el("div", { class: "kh-page" }, [
        ui.pageHead({ title: "知识图谱" }),
        ui.panel({
          title: "图谱",
          children: [ui.el("div", { class: "kh-canvas-wrap" }, [canvas])]
        })
      ])
    },

    /** 画布挂进 DOM 后由 browser.js 调用 */
    attachCanvas: function () {
      if (this.__graph && this.__canvas) this.__graph.attachCanvas(this.__canvas)
    }
  })
  ```

  > **为什么用工厂函数而不是小程序的 `Component()`**：小程序的 `Component()` 依赖框架注入 `global.Component`、`this.setData`、`this.triggerEvent`、properties 观察者。本仓零构建、无框架，硬造一套不值当。改成工厂函数后组件是普通对象：`{ data, setData, draw, attachCanvas, destroy }`，页面直接持有实例。
  >
  > `attachCanvas` 必须在 canvas **进 DOM 之后**调 —— `getBoundingClientRect()` 需要元素已在文档流中，宽度才是对的。所以 `browser.js` 渲染完要额外调一次页面的 `attachCanvas()`。

- [ ] **Step 8: 运行测试确认通过**

Run:
```bash
cd apps/web && node tests/kh-view.test.js && node tests/run-all.js
```
Expected: `kh-view.test.js` 的 7 个用例过；`run-all.js` 全绿（计划一的 6 组 + 计划二的 6 组 + 本计划的 5 组 + 迁入的 181 个断言）

> 若迁入的某个断言在视图层重写后失败，**说明数据层被改坏了** —— 回到小程序版对照，把数据层改回去。

- [ ] **Step 9: 提交**

```bash
git add apps/web
git commit -m "feat(web): knowledge-hub 17 个页面与 2 个 Canvas 组件重写

页面模块形状：{ data, onMount?, render(state) → 节点 }，用
global.__KH_PAGE__ 注册。数据层逐字搬，交互层用 platform.* 替换 wx.*
（回调式改 Promise 式）。点路径 setData 改成直接改 state。

两个 Canvas 组件改成工厂函数（create(props)）—— 小程序的 Component()
依赖框架注入，Web 侧没有那套机制，硬造不值当。绘制代码逐字保留，
只把「怎么拿到 canvas」从 createSelectorQuery 换成
getBoundingClientRect。坐标计算在 shared/knowledge/*-layout.js，
纯函数、已有单测。

6 个通用组件（card/empty/fab/header/progress-bar/tag）不移植，
改用 shared/ui.js 的构件（新增 progressBar）。"
git push origin main
```

---

## Task 6：模块接入外壳 + 端到端冒烟

**Files:**
- Create: `apps/web/web/knowledge/shell.html`
- Create: `apps/web/web/knowledge/browser.js`
- Modify: `apps/web/server/app.js`
- Modify: `apps/web/tests/server.test.js`

**Interfaces:**
- Consumes: Task 5 的 `web/knowledge/router.js` 与页面模块
- Produces: `http://127.0.0.1:5173/knowledge/pages/dashboard/index` 可访问

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/server.test.js` 末尾追加：

```javascript
test("knowledge-hub 模块的页面能被访问", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/knowledge/pages/dashboard/index")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("knowledge-hub") >= 0 || r.body.indexOf("Reading Radar") >= 0,
    "应返回 knowledge 模块的 HTML 壳")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/server.test.js`
Expected: FAIL —— 404

- [ ] **Step 3: 写 `web/knowledge/shell.html`**

新建 `apps/web/web/knowledge/shell.html`：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Reading Radar · 知识中心</title>
  <link rel="stylesheet" href="/style.css">
  <link rel="stylesheet" href="/workbench/style.css">
  <link rel="stylesheet" href="/knowledge/style.css">
</head>
<body class="theme-light">
  <div class="app-shell">
    <header class="app-head">
      <h1 class="app-title">Reading Radar</h1>
      <nav class="app-nav">
        <a class="nav-item" href="/">返回工作台</a>
        <a class="nav-item" href="/knowledge/pages/dashboard/index">首页</a>
        <a class="nav-item" href="/knowledge/pages/learn/index">学习</a>
        <a class="nav-item" href="/knowledge/pages/interview/index">面试</a>
        <a class="nav-item" href="/knowledge/pages/knowledge/index">知识</a>
        <a class="nav-item" href="/knowledge/pages/profile/index">我的</a>
      </nav>
    </header>
    <main class="app-main" id="app-main"></main>
  </div>
  <script src="/shared/ui.js"></script>
  <script src="/shared/charts.js"></script>
  <script src="/shared/platform.js"></script>
  <script src="/cjs-loader.js"></script>
  <script src="/knowledge/browser.js"></script>
</body>
</html>
```

- [ ] **Step 4: 写 `web/knowledge/browser.js`**

新建 `apps/web/web/knowledge/browser.js`：

```javascript
"use strict"
/**
 * browser.js —— knowledge-hub 模块的浏览器入口
 *
 * 与 workbench 的 browser.js 同构：按依赖顺序预载 CommonJS 模块，再挂载页面。
 */

;(function () {
  "use strict"

  const CJS = window.CJS

  /** 按依赖顺序预载的模块路径 */
  const PRELOAD = [
    "/shared/core/id.js",
    "/shared/core/date.js",
    "/shared/core/format.js",
    "/shared/core/constants.js",
    "/shared/core/md2html.js",
    "/shared/core/md.js",
    "/shared/core/storage.js",
    "/shared/core/store.js",
    "/shared/core/theme.js",
    "/shared/core/with-theme.js",
    "/shared/knowledge/keys.js",
    "/shared/knowledge/seed-data.js",
    "/shared/knowledge/review.js",
    "/shared/knowledge/kh-constants.js",
    "/shared/knowledge/graph-layout.js",
    "/shared/knowledge/radar-layout.js",
    "/shared/knowledge/seed.js",
    "/shared/knowledge/stores.js"
  ]

  /** 模块内路径 → 页面文件路径 */
  const PAGES = {
    "/pages/dashboard/index": "/web/knowledge/pages/dashboard/index.js",
    "/pages/learn/index": "/web/knowledge/pages/learn/index.js",
    "/pages/learn/quiz/index": "/web/knowledge/pages/learn/quiz/index.js",
    "/pages/learn/review/index": "/web/knowledge/pages/learn/review/index.js",
    "/pages/learn/deep/index": "/web/knowledge/pages/learn/deep/index.js",
    "/pages/learn/path/index": "/web/knowledge/pages/learn/path/index.js",
    "/pages/interview/index": "/web/knowledge/pages/interview/index.js",
    "/pages/interview/tracker/index": "/web/knowledge/pages/interview/tracker/index.js",
    "/pages/interview/bank/index": "/web/knowledge/pages/interview/bank/index.js",
    "/pages/interview/mock/index": "/web/knowledge/pages/interview/mock/index.js",
    "/pages/knowledge/index": "/web/knowledge/pages/knowledge/index.js",
    "/pages/knowledge/graph/index": "/web/knowledge/pages/knowledge/graph/index.js",
    "/pages/knowledge/excerpt/index": "/web/knowledge/pages/knowledge/excerpt/index.js",
    "/pages/knowledge/radar/index": "/web/knowledge/pages/knowledge/radar/index.js",
    "/pages/profile/index": "/web/knowledge/pages/profile/index.js",
    "/pages/plan/five-year/index": "/web/knowledge/pages/plan/five-year/index.js",
    "/pages/plan/roadmap/index": "/web/knowledge/pages/plan/roadmap/index.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
    // 画布进 DOM 后再绑定 —— getBoundingClientRect 需要元素已在文档流中
    if (typeof currentPage.attachCanvas === "function") currentPage.attachCanvas()
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/knowledge/, "") || "/pages/dashboard/index"
    const file = PAGES[key]
    if (!file) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) rootEl.appendChild(window.AlgoPlanUI.empty("页面不存在", key))
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      currentPage = page
      currentPage.state = Object.assign({}, currentPage.data)
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
      draw()
      return currentPage
    })
  }

  function boot () {
    let chain = Promise.resolve()
    PRELOAD.forEach(function (p) {
      chain = chain.then(function () { return CJS.load(p) })
    })
    chain.then(function () {
      window.addEventListener("popstate", function () {
        loadPage(window.location.pathname)
      })
      return loadPage(window.location.pathname)
    }).catch(function (err) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) {
        rootEl.appendChild(window.AlgoPlanUI.empty("模块加载失败", String(err && err.message ? err.message : err)))
      }
      console.error("[knowledge] 加载失败", err)
    })
  }

  boot()
})()
```

- [ ] **Step 5: 在 `server/app.js` 里挂 knowledge 路由**

在 `apps/web/server/app.js` 里，`workbenchShell` 那段之后加：

```javascript
  // knowledge-hub 模块：同理返回 HTML 壳
  const KNOWLEDGE_SHELL = path.join(__dirname, "..", "web", "knowledge", "shell.html")
  const knowledgeShell = function (req, res) {
    let html
    try {
      html = fs.readFileSync(KNOWLEDGE_SHELL, "utf8")
    } catch (e) {
      sendJson(res, 500, { error: "knowledge 模块页面壳读取失败" })
      return
    }
    res.writeHead(200, {
      "Content-Type": "text/html; charset=utf-8",
      "Content-Length": Buffer.byteLength(html)
    })
    res.end(html)
  }
  routes.push({ method: "GET", pattern: "/knowledge/pages/:a/:b/:c", handler: knowledgeShell })
  routes.push({ method: "GET", pattern: "/knowledge/pages/:a/:b", handler: knowledgeShell })
```

- [ ] **Step 6: 运行测试确认通过**

Run: `cd apps/web && node tests/server.test.js && node tests/run-all.js`
Expected: 全绿

- [ ] **Step 7: 手工端到端验证**

```bash
cd apps/web && node server/main.js
```

```bash
curl -s http://127.0.0.1:5173/knowledge/pages/dashboard/index | head -20
```
Expected: HTML 壳，含 `<title>Reading Radar · 知识中心</title>`

```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5173/shared/knowledge/stores.js
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5173/shared/knowledge/graph-layout.js
```
Expected: 都是 `200`

浏览器打开 `http://127.0.0.1:5173/knowledge/pages/dashboard/index`，确认：
- 首页 4 个入口卡片可点
- 4 个指标显示数字
- 打开 `knowledge/pages/knowledge/graph/index`，**知识图谱画出来了**（Canvas 有内容）
- 打开 `knowledge/pages/knowledge/radar/index`，**技术雷达画出来了**
- 浏览器控制台**无报错**

- [ ] **Step 8: 提交**

```bash
git add apps/web
git commit -m "feat(web): knowledge-hub 模块接入外壳，浏览器可访问

shell.html 返回 HTML 壳，browser.js 按依赖顺序预载 18 个 CommonJS 模块
再挂载页面。两个 Canvas 页面（知识图谱 / 技术雷达）用 getBoundingClientRect
取画布尺寸。/knowledge/pages/* 由服务端统一返回壳。"
git push origin main
```

---

## 验收标准（本计划完成时）

1. `cd apps/web && node tests/run-all.js` —— **全绿**，其中迁入的 **181 个断言**（42 + 20 + 11 + 41 + 67）全部通过
2. `node apps/web/server/main.js` 后，`http://127.0.0.1:5173/knowledge/pages/dashboard/index` 可访问，17 个页面均可交互
3. **两个 Canvas 组件正常绘制** —— 知识图谱与技术雷达在浏览器里看得到内容
4. `shared/knowledge/stores.js` 导出 14 个域；`storage.KEYS` **未被污染**（`Object.assign` 已改成本地副本）
5. knowledge-hub 的 settings 仍在 `db:settings`，workbench 在 `db:wb_settings` —— **互不干扰**
6. `apps/knowledge-hub/` 目录**未删未改**，作为对照参考
7. **未修改** `engine/`、`apps/study-hub/`、`apps/wiki-rag/`
8. `git status` 干净

## 后续计划

| 计划 | 内容 | 依赖本计划的 |
|---|---|---|
| 计划四 | trip-log（3 页 + IndexedDB 图片）+ games（5 页） | `shared/platform.js`、`cjs-loader.js`、工厂函数式组件模式、`kb-harness.js` 脚手架模式 |
| 计划五 | study-hub 挂载 + wiki-rag 反代 + 删旧目录 + 更新 README | 计划二 + 计划三 + 计划四 |
