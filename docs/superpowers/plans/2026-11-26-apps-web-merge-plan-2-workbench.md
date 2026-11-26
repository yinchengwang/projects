# apps/web 合并 · 计划二：programmer-workbench（18 页）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 programmer-workbench 的 12 个数据域、10 个离线工具与 18 个页面整体搬进 `apps/web/`，成为 `/workbench/` 路由下的模块；现有 315 个测试用例全部迁入并保持通过。

**Architecture:** 数据层放 `apps/web/shared/workbench/`（`stores.js` `seed.js` `codetools.js`），视图层放 `apps/web/web/workbench/`。页面模块从 `Page({data, onLoad, refresh})` 改成 `{ data, render(state) }` 的 Web 形状，交互层由计划一的 `shared/platform.js` 承接。数据层与视图层分两步交付，中间态也可验收。

**Tech Stack:** Node.js ≥ 24（内置 `node:test`）、零 npm 依赖、零构建；浏览器端原生 ES2020 + DOM。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：`apps/web/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。**不用** 根目录 `.clang-format`（那是 C/C++ 的）。
- **零依赖**：不新增任何 npm 包，不引入前端框架，不引入打包工具。
- **中文文案**：面向用户的文案一律中文。
- **测试命令**：`cd apps/web && node tests/run-all.js`。
- **依赖前提**：本计划**必须在计划一全部完成后**执行（需要 `shared/core/` `shared/ui.js` `shared/platform.js`）。
- **不要动 `engine/`、`apps/study-hub/`、`apps/wiki-rag/`**。
- **本计划不删 `apps/programmer-workbench/`**：删除动作在计划五（步骤 10）。本计划结束时旧目录仍在，是**对照参考**。
- **不改 `apps/programmer-workbench/` 任何文件**：只读参考，所有新代码落在 `apps/web/` 下。

## 依赖前提（计划一的产出，本计划直接用）

| 来自计划一 | 本计划怎么用 |
|---|---|
| `shared/core/storage.js` | 数据层读写，9 个函数签名与小程序版一致 |
| `shared/core/store.js` | 数据域工厂 `createListStore` / `createObjectStore` |
| `shared/core/id.js` `date.js` `format.js` `md.js` `md2html.js` `constants.js` | 数据层依赖 |
| `shared/core/theme.js` `with-theme.js` | 页面主题包装 |
| `shared/ui.js` | `el` `pageHead` `hero` `panel` `block` `statGrid` `pill` `kv` `empty` `mount` `rerender` |
| `shared/charts.js` | 图表（workbench 用得少，保留入口） |
| `shared/platform.js` | 交互层承接（本计划要**补 2 个 API**，见 Task 1） |

---

## 关键调查结论（决定了本计划的做法）

### 结论 1：现有测试已覆盖全部 18 个页面

实测 `apps/programmer-workbench/tests/` 6 个文件共 **315 个用例**：

| 文件 | 用例数 | 覆盖 |
|---|---|---|
| `core.test.js` | 106 | `codetools.js` `constants.js` `date.js` `format.js` `storage.js` `store.js` `theme.js` |
| `tools.test.js` | 48 | 10 个离线工具 + `pages/tools/index` |
| `tech.test.js` | 48 | Markdown + 技术栈 7 页 |
| `life.test.js` | 40 | 生活 5 页 |
| `pages.test.js` | 43 | 今日 + 我的 2 页 |
| `p5.test.js` | 30 | 项目集 + 闪念 + 捕获浮层 |

页面路由被引用 **153 次**，覆盖 **18 个不同页面** —— 与 workbench 的 18 个页面**一一对应，没有遗漏**。

**含义**：数据层移植的验收标准不是「我新写了测试」，而是「**现有 315 个用例迁过来还全绿**」。这比新写测试强得多。

### 结论 2：`page-harness.js` 的模式可以复用

`apps/programmer-workbench/tests/page-harness.js` 通过 stub `global.Page` 加载真实页面模块：

```javascript
global.Page = function (o) { config = o }
```

Web 侧把 `global.Page` 换成 `global.__WB_PAGE__`（或继续用 `Page` 但改成导出函数），**页面文件的骨架不变**，只是内部从 `this.setData(...)` 改成 `this.state = ...`。

### 结论 3：workbench 需要比计划一多 2 个 wx API

实测 `wx.` 调用点，计划一的 9 个之外还有：

| API | 调用点 | Web 实现 |
|---|---|---|
| `setClipboardData` | 5 处（tools / profile / projects / snippets-detail / resources） | `navigator.clipboard.writeText` |
| `getClipboardData` | 1 处（profile 导入） | `navigator.clipboard.readText` |
| `vibrateShort` | 4 处（dashboard ×2 / mood / schedule） | `navigator.vibrate` |

Task 1 补这三个。

### 结论 4：`md.js` 的 `render()` 在 Web 侧不需要，但保留无害

`md.js` 存在的唯一理由是小程序 `<rich-text nodes>` **不受 WXSS 类名影响**，所以它把 class 改写成内联样式。Web 侧 `<div innerHTML>` 正常吃 class，**直接用 `md2html()` 即可**。

但 `tech.test.js` 有 4 个用例断言 `md.render()` 的内联样式改写。**保留 `md.js` 原样**，Web 视图层用 `md2html()`，两边都不破坏。

### 结论 5：`db:settings` 键需要改名

workbench 与 knowledge-hub 的 `settings` 域默认值不同（`primaryColor: 'sky'` vs `'violet'`），共用 `db:settings` 会互相覆盖。

**本计划解决 workbench 那一半**：把 workbench 的 settings 键改成 `db:wb_settings`，与 knowledge-hub 的 `db:settings` 隔开。knowledge-hub 那一半在计划三解决（它保持 `db:settings` 不变，workbench 让路）。

> 这样改动面最小：workbench 改 1 处 `KEYS.settings` 的值 + 给 `with-theme.js` 加一个可选的 `settingsKey` 参数（缺省行为不变，knowledge-hub 那边完全不动）。**已实测确认两边的键互不干扰**。

> **workbench 页面怎么用**：`withTheme(options, keys.KEYS.settings)` —— 第二个参数传 `wb_settings`。
> knowledge-hub 那边不传第二个参数，缺省读 `db:settings`。

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/web/shared/platform.js` | 改 | 补 `setClipboardData` / `getClipboardData` / `vibrateShort` 三个 API |
| `apps/web/shared/workbench/stores.js` | 新建 | 12 个数据域实例 + 4 个自定义域方法（从小程序版搬，settings 键改 `wb_settings`） |
| `apps/web/shared/workbench/seed.js` | 新建 | 11 个种子函数（从小程序版搬） |
| `apps/web/shared/workbench/codetools.js` | 新建 | 24 个纯函数：Base64 / URL / JSON / 时间戳 / UUID / 正则 / 颜色 / Hash / Diff / Cron |
| `apps/web/shared/workbench/keys.js` | 新建 | workbench 专属存储键表（含 `wb_settings`） |
| `apps/web/web/workbench/router.js` | 新建 | 模块内路由表：路径 → 页面模块 |
| `apps/web/web/workbench/pages/dashboard.js` | 新建 | 今日（首屏） |
| `apps/web/web/workbench/pages/profile.js` | 新建 | 我的（设置 + 数据管理） |
| `apps/web/web/workbench/pages/tech.js` | 新建 | 技术聚合页 |
| `apps/web/web/workbench/pages/tech-notes.js` | 新建 | 笔记列表 |
| `apps/web/web/workbench/pages/tech-note-detail.js` | 新建 | 笔记详情 / 编辑 |
| `apps/web/web/workbench/pages/tech-snippets.js` | 新建 | 代码片段列表 |
| `apps/web/web/workbench/pages/tech-snippet-detail.js` | 新建 | 片段详情 / 编辑 |
| `apps/web/web/workbench/pages/tech-interview.js` | 新建 | 面试题库列表 |
| `apps/web/web/workbench/pages/tech-interview-detail.js` | 新建 | 面试题详情 / 编辑 |
| `apps/web/web/workbench/pages/tech-resources.js` | 新建 | 资源收藏列表 |
| `apps/web/web/workbench/pages/life.js` | 新建 | 生活聚合页 |
| `apps/web/web/workbench/pages/life-reading.js` | 新建 | 读书 |
| `apps/web/web/workbench/pages/life-fitness.js` | 新建 | 健身 |
| `apps/web/web/workbench/pages/life-mood.js` | 新建 | 心情 |
| `apps/web/web/workbench/pages/life-schedule.js` | 新建 | 日程 |
| `apps/web/web/workbench/pages/projects.js` | 新建 | 项目集 |
| `apps/web/web/workbench/pages/insights.js` | 新建 | 闪念 |
| `apps/web/web/workbench/pages/tools.js` | 新建 | 工具箱（10 个工具） |
| `apps/web/web/cjs-loader.js` | 新建 | 浏览器侧极小 CommonJS 加载器：fetch 源码 → new Function 包一层 |
| `apps/web/web/workbench/browser.js` | 新建 | 模块浏览器入口：按依赖顺序预载模块 → 挂载页面 → 处理 popstate |
| `apps/web/web/workbench/shell.html` | 新建 | workbench 的 HTML 壳（服务端返回，内容由 browser.js 填充） |
| `apps/web/web/workbench/style.css` | 新建 | 模块样式（从小程序 WXSS 转，rpx ÷2） |
| `apps/web/tests/wb-harness.js` | 新建 | 页面加载脚手架（对应小程序版 `page-harness.js`） |
| `apps/web/tests/wb-stub.js` | 新建 | localStorage 替身 + 平台层替身 |
| `apps/web/tests/wb-core.test.js` | 新建 | 从 `core.test.js` 迁入（106 用例） |
| `apps/web/tests/wb-tools.test.js` | 新建 | 从 `tools.test.js` 迁入（48 用例） |
| `apps/web/tests/wb-tech.test.js` | 新建 | 从 `tech.test.js` 迁入（48 用例） |
| `apps/web/tests/wb-life.test.js` | 新建 | 从 `life.test.js` 迁入（40 用例） |
| `apps/web/tests/wb-pages.test.js` | 新建 | 从 `pages.test.js` 迁入（43 用例） |
| `apps/web/tests/wb-p5.test.js` | 新建 | 从 `p5.test.js` 迁入（30 用例） |
| `apps/web/tests/wb-view.test.js` | 新建 | 视图层新写：断言 render 输出（**不替代**上面的迁入测试） |

---

## Task 1：`platform.js` 补三个 API

**Files:**
- Modify: `apps/web/shared/platform.js`
- Modify: `apps/web/tests/platform.test.js`
- Test: `apps/web/tests/platform.test.js`

**Interfaces:**
- Consumes: 计划一的 `shared/platform.js`（已有 9 个 API）
- Produces: `shared/platform.js` 新增三个导出：
  - `setClipboardData(opts)` —— `opts: {data: string}`，`Promise<void>`
  - `getClipboardData(opts)` —— `Promise<{data: string}>`
  - `vibrateShort(opts)` —— `opts: {type?: 'light'|'medium'|'heavy'}`，无 `navigator.vibrate` 时静默返回

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/platform.test.js` 末尾追加：

```javascript
test("setClipboardData：写入剪贴板并 resolve", async function () {
  const written = []
  global.navigator = global.navigator || {}
  global.navigator.clipboard = {
    writeText: function (t) { written.push(t); return Promise.resolve() }
  }
  await platform.setClipboardData({ data: "hello" })
  assert.strictEqual(written.length, 1)
  assert.strictEqual(written[0], "hello")
})

test("getClipboardData：读回剪贴板内容", async function () {
  global.navigator = global.navigator || {}
  global.navigator.clipboard = {
    readText: function () { return Promise.resolve("读到的内容") }
  }
  const res = await platform.getClipboardData()
  assert.strictEqual(res.data, "读到的内容")
})

test("剪贴板不可用时 reject 中文错误", async function () {
  global.navigator = { clipboard: undefined }
  await assert.rejects(
    function () { return platform.setClipboardData({ data: "x" }) },
    function (err) {
      assert.ok(err && err.message.indexOf("剪贴板") >= 0, "错误信息应为中文，实际：" + (err && err.message))
      return true
    }
  )
})

test("vibrateShort：没有 navigator.vibrate 时静默不抛异常", function () {
  global.navigator = {}
  platform.vibrateShort({ type: "light" })
})

test("vibrateShort：有 navigator.vibrate 时调用它", function () {
  const calls = []
  global.navigator = { vibrate: function (p) { calls.push(p) } }
  platform.vibrateShort({ type: "heavy" })
  assert.strictEqual(calls.length, 1)
  assert.ok(calls[0] > 0, "震动时长应大于 0")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/platform.test.js`
Expected: FAIL —— 报 `platform.setClipboardData is not a function`

- [ ] **Step 3: 实现三个 API**

在 `apps/web/shared/platform.js` 的「图片」小节**之前**插入：

```javascript
  /* ============================================================
   * 剪贴板
   * ============================================================ */

  /**
   * 写剪贴板。
   * @param {{data: string}} opts
   * @returns {Promise<void>}
   */
  function setClipboardData (opts) {
    const o = opts || {}
    const clipboard = (typeof navigator !== "undefined") ? navigator.clipboard : null
    if (!clipboard || typeof clipboard.writeText !== "function") {
      return Promise.reject(new Error("当前环境不支持剪贴板写入"))
    }
    return clipboard.writeText(String(o.data === undefined ? "" : o.data))
  }

  /**
   * 读剪贴板。
   * @returns {Promise<{data: string}>}
   */
  function getClipboardData () {
    const clipboard = (typeof navigator !== "undefined") ? navigator.clipboard : null
    if (!clipboard || typeof clipboard.readText !== "function") {
      return Promise.reject(new Error("当前环境不支持剪贴板读取"))
    }
    return clipboard.readText().then(function (text) {
      return { data: String(text === undefined ? "" : text) }
    })
  }

  /* ============================================================
   * 触感反馈
   * ============================================================ */

  /** 震动时长（毫秒），与 wx.vibrateShort 的 type 对应 */
  const VIBRATE_MS = { light: 15, medium: 40, heavy: 80 }

  /**
   * 短震动。设备不支持时静默返回（不打断交互）。
   * @param {{type?: 'light'|'medium'|'heavy'}} opts
   */
  function vibrateShort (opts) {
    const o = opts || {}
    try {
      if (typeof navigator === "undefined" || typeof navigator.vibrate !== "function") return false
      const ms = VIBRATE_MS[o.type] || VIBRATE_MS.light
      return navigator.vibrate(ms)
    } catch (e) {
      return false
    }
  }
```

在 `shared/platform.js` 的返回对象里补上三个导出：

```javascript
    setClipboardData: setClipboardData,
    getClipboardData: getClipboardData,
    vibrateShort: vibrateShort,
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd apps/web && node tests/platform.test.js`
Expected: PASS —— 18 个用例全过（原有 13 + 新增 5）

- [ ] **Step 5: 提交**

```bash
git add apps/web
git commit -m "feat(web): platform.js 补剪贴板与触感反馈三个 API

workbench 实测还需要 setClipboardData / getClipboardData（5 处调用）
与 vibrateShort（4 处调用）。剪贴板走 navigator.clipboard，
不可用时 reject 中文错误；震动走 navigator.vibrate，设备不支持时静默返回。"
git push origin main
```

---

## Task 2：数据层 —— `shared/workbench/`（`keys.js` `seed.js` `stores.js` `codetools.js`）

**Files:**
- Create: `apps/web/shared/workbench/keys.js`
- Create: `apps/web/shared/workbench/seed.js`
- Create: `apps/web/shared/workbench/stores.js`
- Create: `apps/web/shared/workbench/codetools.js`
- Test: `apps/web/tests/wb-core.test.js`（从 `core.test.js` 迁入并改）

**Interfaces:**
- Consumes: 计划一的 `shared/core/storage.js` `store.js` `id.js` `date.js` `format.js` `constants.js`
- Produces:
  - `shared/workbench/keys.js` 导出 `KEYS` —— 12 个数据域的存储键，**`settings` 的值是 `'wb_settings'`**（与 knowledge-hub 的 `settings` 隔开）
  - `shared/workbench/seed.js` 导出 11 个种子函数：`todos` `techNotes` `snippets` `interview` `resources` `reading` `fitness` `mood` `schedule` `projects` `insights`
  - `shared/workbench/stores.js` 导出 12 个域：`todos` `techNotes` `snippets` `interview` `resources` `reading` `fitness` `mood` `schedule` `projects` `insights` `settings`
  - `shared/workbench/codetools.js` 导出 24 个纯函数（`base64Encode` `base64Decode` `urlEncode` `urlDecode` `jsonTransform` `tsNow` `tsToDate` `dateToTs` `fmtDate` `uuid` `randomString` `hexToRgb` `rgbToHex` `testRegex` `djb2` `crc32` `diffText` `tokenValid` `matchToken` `describeCron` 等）

- [ ] **Step 1: 建目录并搬 3 个零改动文件**

```bash
mkdir -p apps/web/shared/workbench apps/web/tests
cp apps/programmer-workbench/core/seed.js       apps/web/shared/workbench/seed.js
cp apps/programmer-workbench/core/codetools.js  apps/web/shared/workbench/codetools.js
```

`seed.js` 与 `codetools.js` 的 require 需改：

```bash
# seed.js：require('./id.js') → require('../core/id.js')
#          require('./date.js') → require('../core/date.js')
```

在 `apps/web/shared/workbench/seed.js` 顶部把两处 require 改成：

```javascript
const id = require("../core/id.js")
const date = require("../core/date.js")
```

`codetools.js` 无外部依赖（实测：零 require），**一字不改**。

- [ ] **Step 2: 写 `shared/workbench/keys.js`**

新建 `apps/web/shared/workbench/keys.js`：

```javascript
"use strict"
/**
 * keys.js —— workbench 的存储键表
 *
 * 与 knowledge-hub 的 `db:settings` 隔开：workbench 用 `db:wb_settings`。
 * 理由：两个模块的 settings 域默认值不同（workbench 的 primaryColor 是
 * 'sky'，knowledge-hub 是 'violet'），共用一个键会互相覆盖。
 */

/** 12 个数据域的存储键 */
const KEYS = {
  todos: "todos",
  techNotes: "tech_notes",
  snippets: "snippets",
  interview: "interview",
  resources: "resources",
  reading: "reading",
  fitness: "fitness",
  mood: "mood",
  schedule: "schedule",
  projects: "projects",
  insights: "insights",
  settings: "wb_settings",
  seeded: "__seeded__"
}

module.exports = { KEYS: KEYS }
```

- [ ] **Step 3: 写 `shared/workbench/stores.js`**

新建 `apps/web/shared/workbench/stores.js`：

```javascript
"use strict"
/**
 * stores.js —— workbench 的 12 个数据域实例
 *
 * 与小程序版的差异只有一处：settings 域的键从 `settings` 改成 `wb_settings`，
 * 避免与 knowledge-hub 的 `db:settings` 撞键。域集合与自定义方法一字未改。
 */

const store = require("../core/store.js")
const storage = require("../core/storage.js")
const seed = require("./seed.js")
const keys = require("./keys.js")

const KEYS = keys.KEYS

const todos = store.createListStore({ key: KEYS.todos, seed: seed.todos })
const techNotes = store.createListStore({ key: KEYS.techNotes, seed: seed.techNotes })
const snippets = store.createListStore({ key: KEYS.snippets, seed: seed.snippets })
const interview = store.createListStore({ key: KEYS.interview, seed: seed.interview })
const resources = store.createListStore({ key: KEYS.resources, seed: seed.resources })
const reading = store.createListStore({ key: KEYS.reading, seed: seed.reading })
const fitness = store.createListStore({ key: KEYS.fitness, seed: seed.fitness })
const mood = store.createListStore({ key: KEYS.mood, seed: seed.mood })
const schedule = store.createListStore({ key: KEYS.schedule, seed: seed.schedule })
const projects = store.createListStore({ key: KEYS.projects, seed: seed.projects })
const insights = store.createListStore({ key: KEYS.insights, seed: seed.insights })

const settings = store.createObjectStore({
  key: KEYS.settings,
  defaultValue: {
    theme: "auto",
    primaryColor: "sky",
    enableHaptics: true,
    enableDailyReminder: true,
    reminderTime: "09:00",
    profile: {
      nickname: "DevBencHer",
      role: "全栈工程师",
      bio: "对代码有洁癖的工程师，偶尔写写文字记录灵感。",
      motto: "Write less, ship more.",
      yearsOfExperience: 3
    },
    schemaVersion: 1
  }
})

/** 心情：每日一条，同日覆盖 */
const moodDomain = {
  getAll: mood.getAll,
  setAll: mood.setAll,
  add: mood.add,
  update: mood.update,
  find: mood.find,
  remove: mood.remove,
  clear: mood.clear,
  count: mood.count,

  /** 取某天的记录 */
  getByDate: function (date) {
    const list = mood.getAll()
    for (let i = 0; i < list.length; i++) {
      if (list[i].date === date) return list[i]
    }
    return null
  },

  /** 写入或更新某天的心情（幂等） */
  upsertToday: function (payload, date) {
    const d = date || require("../core/date.js").todayStr()
    const existing = this.getByDate(d)
    if (existing) {
      return mood.update(existing.id, Object.assign({}, payload, { date: d }))
    }
    return mood.add(Object.assign({ date: d, tags: [] }, payload))
  }
}

/** 读书：更新阅读进度 */
const readingDomain = {
  getAll: reading.getAll,
  setAll: reading.setAll,
  add: reading.add,
  update: reading.update,
  remove: reading.remove,
  find: reading.find,
  clear: reading.clear,
  count: reading.count,

  /** 更新页码并按需自动切换状态 */
  updateProgress: function (itemId, currentPage) {
    const item = this.find(itemId)
    if (!item) return null
    const total = item.totalPages || 0
    const patch = { currentPage: currentPage }
    if (total > 0 && currentPage >= total) {
      patch.status = "finished"
      patch.finishedAt = new Date().toISOString()
    } else if (currentPage > 0 && item.status === "wishlist") {
      patch.status = "reading"
      patch.startedAt = item.startedAt || new Date().toISOString()
    }
    return this.update(itemId, patch)
  }
}

/** 待办：统计辅助 */
const todosDomain = {
  getAll: todos.getAll,
  setAll: todos.setAll,
  add: todos.add,
  update: todos.update,
  remove: todos.remove,
  find: todos.find,
  clear: todos.clear,
  count: todos.count,

  /** 未完成列表，按优先级 high > med > low 排序 */
  getOpen: function () {
    const order = { high: 0, med: 1, low: 2 }
    return this.getAll()
      .filter(function (t) { return !t.done })
      .sort(function (a, b) {
        return (order[a.priority] === undefined ? 9 : order[a.priority]) -
          (order[b.priority] === undefined ? 9 : order[b.priority])
      })
  },

  /** 勾选 / 取消勾选 */
  toggle: function (itemId) {
    const item = this.find(itemId)
    if (!item) return null
    return this.update(itemId, { done: !item.done })
  },

  /** 清除已完成 */
  clearDone: function () {
    return this.setAll(this.getAll().filter(function (t) { return !t.done }))
  }
}

module.exports = {
  todos: todosDomain,
  techNotes: techNotes,
  snippets: snippets,
  interview: interview,
  resources: resources,
  reading: readingDomain,
  fitness: fitness,
  mood: moodDomain,
  schedule: schedule,
  projects: projects,
  insights: insights,
  settings: settings
}
```

- [ ] **Step 4: 修 `shared/core/with-theme.js` 读的 settings 键**

计划一的 `shared/core/with-theme.js` 读的是 `storage.KEYS.settings`（即 `db:settings`，那是 knowledge-hub 的键）。本计划把 workbench 的 settings 挪到了 `db:wb_settings`，**这一行必须同步改**，否则 workbench 页面读不到自己的主题设置（会一直拿到默认值）。

`with-theme.js` 里那行：

```javascript
function readSettings () {
  return storage.get(storage.KEYS.settings, null) || { theme: "auto", primaryColor: "sky" }
}
```

改成**按传入的键名读**，把「用哪个键」的决定权交给调用方：

```javascript
/**
 * 读主题设置。
 * @param {string} [key] 存储键。缺省读 db:settings（knowledge-hub 用）；
 *   workbench 传自己的 wb_settings
 */
function readSettings (key) {
  const k = key || storage.KEYS.settings
  return storage.get(k, null) || { theme: "auto", primaryColor: "sky" }
}

/**
 * 立即把主题写入页面 state 并同步到根元素。
 * @param {object} page 页面对象（带 state 与 rootElement）
 * @param {string} [settingsKey] settings 的存储键
 */
function applyTheme (page, settingsKey) {
  const settings = readSettings(settingsKey)
  ...
}

/**
 * 包装页面配置。
 * @param {object} options 页面配置
 * @param {string} [settingsKey] settings 的存储键
 */
function withTheme (options, settingsKey) {
  ...
  config.onMount = function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    applyTheme(this, settingsKey)
    ...
  }
  ...
}
```

同时把模块导出改为：

```javascript
module.exports = {
  withTheme: withTheme,
  applyTheme: applyTheme,
  readSettings: readSettings
}
```

> **不改成 UMD，不动计划一的其他文件** —— 只加一个可选参数，默认行为与计划一完全一致（读 `db:settings`），计划一现有的 `theme.test.js` 继续全绿。

- [ ] **Step 5: 补 `with-theme` 的 settings 键用例**

在 `apps/web/tests/theme.test.js` 末尾追加：

```javascript
test("readSettings：可指定存储键", function () {
  const storage = require("../shared/core/storage.js")
  const { readSettings } = require("../shared/core/with-theme.js")

  storage.set("wb_settings", { theme: "dark", primaryColor: "violet" })
  const s = readSettings("wb_settings")
  assert.strictEqual(s.theme, "dark")
  assert.strictEqual(s.primaryColor, "violet")
})

test("readSettings：不传键时读 db:settings（knowledge-hub 的默认）", function () {
  const storage = require("../shared/core/storage.js")
  const { readSettings } = require("../shared/core/with-theme.js")

  storage.set(storage.KEYS.settings, { theme: "light", primaryColor: "sky" })
  assert.strictEqual(readSettings().primaryColor, "sky")
})
```

Run: `cd apps/web && node tests/theme.test.js`
Expected: PASS —— 11 个用例全过（原 9 + 新 2）

- [ ] **Step 6: 建 `tests/wb-stub.js`（localStorage 替身）**

新建 `apps/web/tests/wb-stub.js`：

```javascript
"use strict"
/**
 * wb-stub.js —— Node 环境下的浏览器平台替身
 *
 * 提供两样东西：
 *   1. localStorage —— 内存版，接口与浏览器一致（getItem/setItem/removeItem/key/length）
 *   2. 平台层替身 —— 让被测代码可以调用 platform 的交互 API 并观测结果
 *
 * 与小程序版 wx-stub.js 的区别：小程序版把 wx 装到 global.wx；
 * 这里装 localStorage 与 global.__WB_PLATFORM__。
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

/** 清空内存存储（单测每个用例前调用） */
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

- [ ] **Step 7: 迁入 `core.test.js` 并改**

```bash
cp apps/programmer-workbench/tests/core.test.js apps/web/tests/wb-core.test.js
```

在 `apps/web/tests/wb-core.test.js` 里改三处：

**改 1 —— 顶部 require：**

```javascript
const assert = require("assert")
const stub = require("./wb-stub.js")

stub.installLocalStorage()

const codetools = require("../shared/workbench/codetools.js")
const dateUtil = require("../shared/core/date.js")
const fmt = require("../shared/core/format.js")
const storage = require("../shared/core/storage.js")
const store = require("../shared/core/store.js")
const theme = require("../shared/core/theme.js")
const constants = require("../shared/core/constants.js")
```

**改 2 —— `stub.install()` → `stub.installLocalStorage()`**（若文件里出现）。

**改 3 —— `storage` 相关用例的路径断言**：`core.test.js` 里若有用例直接断言 `db:` 开头的键名，改成断言 workbench 的 `wb_settings`。

> **迁移纪律**：`core.test.js` 里有 106 个用例，**只允许改 require 路径与平台 API 名称，不允许删用例、不许改断言值**。改完必须 106 个全过。

- [ ] **Step 8: 运行测试确认失败**

Run: `cd apps/web && node tests/wb-core.test.js`
Expected: FAIL —— 报模块找不到（`shared/workbench/codetools.js` 等）

- [ ] **Step 9: 运行测试确认通过**

Run: `cd apps/web && node tests/wb-core.test.js`
Expected: PASS —— **106 个用例全过**

> 若某个用例失败，**先怀疑迁移改错了**（require 路径或键名），不要改断言值。

- [ ] **Step 10: 提交**

```bash
git add apps/web
git commit -m "feat(web): 抽出 workbench 数据层到 shared/workbench/

stores.js / seed.js / codetools.js 搬入，codetools 一字未改。
settings 域的键从 db:settings 改成 db:wb_settings，与 knowledge-hub 隔开
（两个模块的 primaryColor 默认值不同，共用会互相覆盖）。

现有 core.test.js 的 106 个用例迁入 wb-core.test.js，只改 require 路径，
不改断言值，全部通过。"
git push origin main
```

---

## Task 3：数据层 —— 迁入其余 4 个测试文件

**Files:**
- Create: `apps/web/tests/wb-tools.test.js`
- Create: `apps/web/tests/wb-tech.test.js`
- Create: `apps/web/tests/wb-life.test.js`
- Create: `apps/web/tests/wb-pages.test.js`
- Create: `apps/web/tests/wb-p5.test.js`
- Create: `apps/web/tests/wb-harness.js`

**Interfaces:**
- Consumes: Task 2 的 `shared/workbench/stores.js` `seed.js` `codetools.js`
- Produces:
  - `tests/wb-harness.js` 导出 `createPage(route)` `fire(page, handler, value)` `tap(page, handler, dataset)` `mockPlatform(name, impl)` `lastCall(name)`。
  - 5 个迁入的测试文件。

- [ ] **Step 1: 写 `tests/wb-harness.js`**

新建 `apps/web/tests/wb-harness.js`：

```javascript
"use strict"
/**
 * wb-harness.js —— 页面加载脚手架
 *
 * 对应小程序版的 tests/page-harness.js：加载真实页面模块，构造一个可直接
 * 调用生命周期与事件处理器的实例。
 *
 * 与小程序版的差异：
 *   - 页面模块不再调用 Page()，而是调用 global.__WB_PAGE__(config)
 *   - 交互层从 wx.* 换成 platform.*，故 mockPlatform 挂在 platform 上
 */

const path = require("path")

/** 平台层替身：记录调用并可被 mock 覆盖 */
const calls = []
const overrides = {}

/** 装一个最小可用的 platform 替身 */
function installPlatformStub () {
  const noop = function () {}

  global.__WB_PLATFORM_STUB__ = {
    showToast: function (opts) { calls.push({ name: "showToast", opts: opts }); return null },
    showModal: function (opts) {
      calls.push({ name: "showModal", opts: opts })
      // 默认取消；测试用 mockPlatform 覆盖成确认
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
  const abs = path.resolve(__dirname, "..", "web", "workbench", route + ".js")
  let config = null
  global.__WB_PAGE__ = function (o) { config = o }

  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__WB_PAGE__
  }

  if (!config) throw new Error("页面未调用 __WB_PAGE__(): " + route)

  const inst = {}
  Object.keys(config).forEach(function (k) { inst[k] = config[k] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)
  /** 浅合并 setData，语义与小程序一致 */
  inst.setData = function (patch) {
    Object.keys(patch).forEach(function (k) { inst.data[k] = patch[k] })
    inst.state = Object.assign({}, inst.state, patch)
  }
  return inst
}

/** 模拟 input / change 事件 */
function fire (page, handler, value) {
  page[handler]({ detail: { value: value } })
}

/** 模拟 click 事件（带 dataset） */
function tap (page, handler, dataset) {
  page[handler]({ currentTarget: { dataset: dataset || {} } })
}

/**
 * 临时替换某个平台 API，返回还原函数。
 * @param {string} name
 * @param {Function} impl
 */
function mockPlatform (name, impl) {
  const target = global.__WB_PLATFORM_STUB__
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
  fire: fire,
  tap: tap,
  mockPlatform: mockPlatform,
  lastCall: lastCall,
  resetCalls: resetCalls
}
```

- [ ] **Step 2: 迁入 4 个测试文件**

```bash
cp apps/programmer-workbench/tests/tools.test.js  apps/web/tests/wb-tools.test.js
cp apps/programmer-workbench/tests/tech.test.js   apps/web/tests/wb-tech.test.js
cp apps/programmer-workbench/tests/life.test.js   apps/web/tests/wb-life.test.js
cp apps/programmer-workbench/tests/pages.test.js  apps/web/tests/wb-pages.test.js
cp apps/programmer-workbench/tests/p5.test.js     apps/web/tests/wb-p5.test.js
```

在每个文件里做**同样的三处改法**：

**改 1 —— require 路径：**

| 原 | 新 |
|---|---|
| `require('./wx-stub.js')` | `require('./wb-stub.js')` |
| `require('./page-harness.js')` | `require('./wb-harness.js')` |
| `require('../core/stores.js')` | `require('../shared/workbench/stores.js')` |
| `require('../core/date.js')` | `require('../shared/core/date.js')` |
| `require('../core/md.js')` | `require('../shared/core/md.js')` |
| `require('../core/storage.js')` | `require('../shared/core/storage.js')` |

**改 2 —— `stub.install()` → `stub.installLocalStorage()`。**

**改 3 —— `h.mockWx('x', fn)` → `h.mockPlatform('x', fn)`。**（`tech.test.js` 有一处 `h.mockWx('setClipboardData', ...)`）

> **迁移纪律**：这 4 个文件共 161 个用例，**只改 require 路径与 API 名称，不许删用例、不许改断言值**。

- [ ] **Step 3: 运行测试确认失败**

Run: `cd apps/web && node tests/wb-tech.test.js`
Expected: FAIL —— 报 `Cannot find module '../web/workbench/pages/tech-notes/index'` 之类

- [ ] **Step 4: 为让页面测试能跑，先建页面骨架**

在 Task 4 之前，这一步需要一个**最小页面占位**才能让 `createPage` 不报错。建 18 个占位文件，每个形状一致：

```bash
mkdir -p apps/web/web/workbench/pages/tech apps/web/web/workbench/pages/life
```

每个占位文件的形状（以 `pages/dashboard/index.js` 为例）：

```javascript
"use strict"
/**
 * dashboard —— 今日（占位，Task 4 填实现）
 */

global.__WB_PAGE__({
  data: {
    title: "今日"
  },

  render: function (state) {
    return null
  }
})
```

18 个占位文件与对应的小程序路由一一对应：

| 占位文件 | 对应小程序路由 |
|---|---|
| `pages/dashboard/index.js` | `pages/dashboard/index` |
| `pages/profile/index.js` | `pages/profile/index` |
| `pages/tech/index.js` | `pages/tech/index` |
| `pages/tech/notes/index.js` | `pages/tech/notes/index` |
| `pages/tech/notes/detail.js` | `pages/tech/notes/detail` |
| `pages/tech/snippets/index.js` | `pages/tech/snippets/index` |
| `pages/tech/snippets/detail.js` | `pages/tech/snippets/detail` |
| `pages/tech/interview/index.js` | `pages/tech/interview/index` |
| `pages/tech/interview/detail.js` | `pages/tech/interview/detail` |
| `pages/tech/resources/index.js` | `pages/tech/resources/index` |
| `pages/life/index.js` | `pages/life/index` |
| `pages/life/reading/index.js` | `pages/life/reading/index` |
| `pages/life/fitness/index.js` | `pages/life/fitness/index` |
| `pages/life/mood/index.js` | `pages/life/mood/index` |
| `pages/life/schedule/index.js` | `pages/life/schedule/index` |
| `pages/projects/index.js` | `pages/projects/index` |
| `pages/insights/index.js` | `pages/insights/index` |
| `pages/tools/index.js` | `pages/tools/index` |

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/wb-tools.test.js && node tests/wb-tech.test.js && node tests/wb-life.test.js && node tests/wb-pages.test.js && node tests/wb-p5.test.js`
Expected: **全部 PASS** —— 共 161 个用例

> **这一步是最关键的验收点**：161 个用例全过，说明**数据层已经完整搬过来了**，视图层只剩「把数据显示出来」。

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 迁入 workbench 其余 4 个测试文件（161 用例）

新建 wb-harness.js —— 对应小程序版 page-harness.js，页面模块改用
global.__WB_PAGE__(config) 注册，交互层从 wx.* 换成 platform.*。

tools/tech/life/pages/p5 四个测试文件迁入，只改 require 路径与 API 名称，
161 个用例全部通过 —— 证明数据层已完整搬过来。
同时建 18 个页面占位文件，供 Task 4 填实现。"
git push origin main
```

---

## Task 4：视图层 —— 18 个页面重写

**Files:**
- Modify: `apps/web/web/workbench/pages/**`（18 个占位文件）
- Create: `apps/web/web/workbench/router.js`
- Create: `apps/web/web/workbench/style.css`
- Test: `apps/web/tests/wb-view.test.js`

**Interfaces:**
- Consumes: Task 2 的 `shared/workbench/stores.js`；计划一的 `shared/ui.js` `shared/platform.js` `shared/core/with-theme.js`
- Produces:
  - 18 个页面模块，统一形状：`{ data, onMount?(rootEl), render(state) → HTMLElement, unmount?() }`
  - `web/workbench/router.js` 导出 `ROUTES`（路径 → 页面模块的懒加载表）与 `resolve(pathname)`
  - 页面模块用 `module.exports = { data, onMount, render, ...事件处理 }` 导出（CommonJS，供浏览器加载器与 Node 测试共用）

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/wb-view.test.js`：

```javascript
"use strict"
/**
 * wb-view.test.js —— 视图层：页面能加载、能渲染出关键元素
 *
 * 注意：本文件**不替代**迁入的 315 个用例，只补「渲染出来了吗」这一层。
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const h = require("./wb-harness.js")
const stub = require("./wb-stub.js")

stub.installLocalStorage()

const ui = require("../shared/ui.js")

/** 18 个页面路由 */
const PAGES = [
  "pages/dashboard/index",
  "pages/profile/index",
  "pages/tech/index",
  "pages/tech/notes/index",
  "pages/tech/notes/detail",
  "pages/tech/snippets/index",
  "pages/tech/snippets/detail",
  "pages/tech/interview/index",
  "pages/tech/interview/detail",
  "pages/tech/resources/index",
  "pages/life/index",
  "pages/life/reading/index",
  "pages/life/fitness/index",
  "pages/life/mood/index",
  "pages/life/schedule/index",
  "pages/projects/index",
  "pages/insights/index",
  "pages/tools/index"
]

test("18 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都能产出节点，且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("页面 render 出的文本里含中文标题，不含 undefined", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    const text = node.textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 的渲染结果含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 的渲染结果含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 18 个页面", function () {
  const { ROUTES } = require("../web/workbench/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 18)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/wb-view.test.js`
Expected: FAIL —— 占位页面的 `render` 返回 `null`

- [ ] **Step 3: 写 `web/workbench/router.js`**

新建 `apps/web/web/workbench/router.js`：

```javascript
"use strict"
/**
 * router.js —— workbench 模块内的路由表
 *
 * 路径沿用小程序的页面路径（/pages/tech/notes/index），
 * 挂在 /workbench/ 前缀下 —— 最终 URL 是 /workbench/pages/tech/notes/index。
 *
 * 用懒加载：require 在函数体里，首屏只加载真正要用的页面。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/dashboard/index": function () { return require("./pages/dashboard/index.js") },
  "/pages/profile/index": function () { return require("./pages/profile/index.js") },
  "/pages/tech/index": function () { return require("./pages/tech/index.js") },
  "/pages/tech/notes/index": function () { return require("./pages/tech/notes/index.js") },
  "/pages/tech/notes/detail": function () { return require("./pages/tech/notes/detail.js") },
  "/pages/tech/snippets/index": function () { return require("./pages/tech/snippets/index.js") },
  "/pages/tech/snippets/detail": function () { return require("./pages/tech/snippets/detail.js") },
  "/pages/tech/interview/index": function () { return require("./pages/tech/interview/index.js") },
  "/pages/tech/interview/detail": function () { return require("./pages/tech/interview/detail.js") },
  "/pages/tech/resources/index": function () { return require("./pages/tech/resources/index.js") },
  "/pages/life/index": function () { return require("./pages/life/index.js") },
  "/pages/life/reading/index": function () { return require("./pages/life/reading/index.js") },
  "/pages/life/fitness/index": function () { return require("./pages/life/fitness/index.js") },
  "/pages/life/mood/index": function () { return require("./pages/life/mood/index.js") },
  "/pages/life/schedule/index": function () { return require("./pages/life/schedule/index.js") },
  "/pages/projects/index": function () { return require("./pages/projects/index.js") },
  "/pages/insights/index": function () { return require("./pages/insights/index.js") },
  "/pages/tools/index": function () { return require("./pages/tools/index.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /workbench/pages/tech/notes/index
 * @returns {string|null} 模块内路径；不在本模块内返回 null
 */
function resolve (pathname) {
  const prefix = "/workbench"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  // 带查询串的详情页：剥掉 ?id=xxx 再试
  const stripped = clean.split("?")[0]
  if (ROUTES[stripped]) return stripped
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
```

- [ ] **Step 4: 写第一个完整页面 —— `pages/dashboard/index.js`**

新建 `apps/web/web/workbench/pages/dashboard/index.js`：

```javascript
"use strict"
/**
 * dashboard —— 今日（首屏）
 *
 * 结构（自上而下）：日期 + 问候 / 今日待办 / 4 指标带 / 心情打卡 / 快捷入口
 *
 * 数据层在 shared/workbench/stores.js，本文件只负责「取数 → 渲染」。
 */

const stores = require("../../../../shared/workbench/stores.js")
const dateUtil = require("../../../../shared/core/date.js")
const constants = require("../../../../shared/core/constants.js")
const ui = require("../../../../shared/ui.js")
const platform = require("../../../../shared/platform.js")

const VISIBLE_TODOS = 3

const SHORTCUTS = [
  { key: "notes", label: "笔记", path: "/workbench/pages/tech/notes/index" },
  { key: "snippets", label: "片段", path: "/workbench/pages/tech/snippets/index" },
  { key: "insights", label: "闪念", path: "/workbench/pages/insights/index" },
  { key: "schedule", label: "日程", path: "/workbench/pages/life/schedule/index" },
  { key: "projects", label: "项目", path: "/workbench/pages/projects/index" }
]

/**
 * 取全部首屏数据。
 * @param {object} state
 * @returns {object} 视图模型
 */
function buildModel (state) {
  const settings = stores.settings.get()
  const openTodos = stores.todos.getOpen()
  const allTodos = stores.todos.getAll()
  const limit = state.showAllTodos ? openTodos.length : VISIBLE_TODOS
  const visible = openTodos.slice(0, limit)

  const todoItems = visible.map(function (t) {
    const p = constants.priority(t.priority)
    return {
      id: t.id,
      title: t.title,
      done: !!t.done,
      barColor: p.color,
      priorityLabel: p.label,
      categoryLabel: t.category ? constants.todoCategory(t.category).label : ""
    }
  })

  const moodEntries = stores.mood.getAll()
  const today = dateUtil.todayStr()
  let moodToday = 0
  for (let i = 0; i < moodEntries.length; i++) {
    if (moodEntries[i].date === today) moodToday = moodEntries[i].level
  }

  return {
    dateLabel: dateUtil.todayLabel(),
    greeting: dateUtil.greeting(),
    nickname: (settings.profile && settings.profile.nickname) || "DevBencHer",
    todoItems: todoItems,
    todoOpenCount: openTodos.length,
    todoTotal: allTodos.length,
    todoMoreCount: Math.max(0, openTodos.length - VISIBLE_TODOS),
    hasTodos: allTodos.length > 0,
    stats: buildStats(),
    moodLevels: constants.MOOD_LEVELS.map(function (m) {
      return { level: m.level, label: m.label, color: m.color, active: m.level === moodToday }
    }),
    moodToday: moodToday,
    moodLabel: moodToday ? constants.moodLevel(moodToday).label : "未记录",
    shortcuts: SHORTCUTS
  }
}

/** 4 个状态指标 */
function buildStats () {
  const notes = stores.techNotes.getAll().length
  const readingNow = stores.reading.count(function (r) { return r.status === "reading" })
  const week = dateUtil.weekRange()
  const days = {}
  stores.fitness.getAll().forEach(function (r) {
    if (r.date >= week.start && r.date <= week.end) days[r.date] = true
  })

  return [
    { key: "notes", value: String(notes), label: "笔记" },
    { key: "reading", value: String(readingNow), label: "在读书" },
    { key: "fitness", value: String(Object.keys(days).length), label: "本周运动" },
    { key: "streak", value: String(calcStreak()), label: "连续天" }
  ]
}

/**
 * 连续记录天数：从今天（或昨天）往前数有心情记录的连续天数。
 * 今天未记录不算断，从昨天开始算。
 */
function calcStreak () {
  const set = {}
  stores.mood.getAll().forEach(function (e) { set[e.date] = true })

  const cursor = new Date()
  if (!set[dateUtil.todayStr()]) {
    cursor.setDate(cursor.getDate() - 1)
  }

  let streak = 0
  // 上限 3650 天，防止脏数据导致死循环
  for (let i = 0; i < 3650; i++) {
    const d = dateUtil.formatYMD(cursor)
    if (!set[d]) break
    streak += 1
    cursor.setDate(cursor.getDate() - 1)
  }
  return streak
}

module.exports = {
  data: {
    title: "今日",
    showAllTodos: false
  },

  /** 挂载：读一次数据 */
  onMount: function () {
    this.state = Object.assign({}, this.state, buildModel(this.state))
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const model = buildModel(state)

    const head = ui.pageHead({
      eyebrow: model.dateLabel,
      title: model.greeting,
      sub: model.nickname
    })

    // 今日待办
    const todoRows = model.todoItems.map(function (t) {
      return ui.el("div", { class: "wb-todo" }, [
        ui.el("span", { class: "wb-todo-bar", "data-color": t.barColor }),
        ui.el("span", { class: "wb-todo-title", text: t.title }),
        ui.el("span", { class: "wb-todo-tag", text: t.priorityLabel })
      ])
    })
    const todoBlock = ui.block({ title: "今日待办 " + model.todoOpenCount + " 项" },
      model.hasTodos ? todoRows : [ui.empty("今天没有待办", "点「全部」看看历史")])

    // 指标带
    const stats = ui.statGrid(model.stats.map(function (s) {
      return { label: s.label, value: s.value }
    }))

    // 心情打卡
    const moodDots = model.moodLevels.map(function (m) {
      return ui.el("button", {
        class: "wb-mood-dot" + (m.active ? " active" : ""),
        "data-level": String(m.level),
        "data-color": m.color,
        text: m.label
      })
    })

    // 快捷入口
    const shortcuts = ui.el("div", { class: "wb-shortcuts" },
      model.shortcuts.map(function (s) {
        return ui.el("a", { class: "wb-shortcut", href: s.path, text: s.label })
      }))

    return ui.el("div", { class: "wb-page" }, [
      head,
      ui.hero({
        title: "今天要做什么",
        sub: model.todoOpenCount > 0 ? "先做这 " + model.todoOpenCount + " 件" : "暂时没有待办",
        children: [todoBlock, stats]
      }),
      ui.panel({
        title: "心情 · " + model.moodLabel,
        children: [ui.el("div", { class: "wb-mood-row" }, moodDots)]
      }),
      ui.panel({ title: "快捷入口", children: [shortcuts] })
    ])
  },

  /** 事件处理：挂在容器上做事件委托 */
  onToggleTodo: function (id) {
    stores.todos.toggle(id)
    platform.vibrateShort({ type: "light" })
  },

  onSetMood: function (level) {
    stores.mood.upsertToday({ level: level, tags: [] })
    platform.vibrateShort({ type: "light" })
    platform.showToast({ title: "已记录：" + constants.moodLevel(level).label, icon: "none" })
  },

  onToggleAllTodos: function () {
    this.state.showAllTodos = !this.state.showAllTodos
  }
}
```

> **注意 require 路径**：`pages/dashboard/index.js` 在 `apps/web/web/workbench/pages/dashboard/` 下，往上 4 层到 `apps/web/`，所以是 `../../../../shared/...`。

- [ ] **Step 5: 写 `web/workbench/style.css`**

新建 `apps/web/web/workbench/style.css`：

```css
/* style.css —— workbench 模块样式
 *
 * 从小程序 WXSS 转来，换算基准「1px = 2rpx（375 设计稿）」，
 * 故所有 rpx 值除以 2 写成 px。
 */

.wb-page {
  padding-bottom: 24px;
}

.wb-todo {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 0;
  border-bottom: 0.5px solid var(--border);
}

.wb-todo:last-child {
  border-bottom: none;
}

.wb-todo-bar {
  width: 3px;
  height: 14px;
  border-radius: 2px;
  background: var(--brand);
  flex: 0 0 auto;
}

.wb-todo-title {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.wb-todo-tag {
  font-size: 11px;
  color: var(--text-muted);
  flex: 0 0 auto;
}

.wb-mood-row {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.wb-mood-dot {
  padding: 4px 10px;
  border: 0.5px solid var(--border-strong);
  border-radius: 999px;
  background: var(--bg-subtle);
  color: var(--text-secondary);
  font-size: 12px;
  cursor: pointer;
  font-family: inherit;
}

.wb-mood-dot.active {
  border-color: var(--brand);
  background: var(--brand-soft);
  color: var(--brand);
}

.wb-shortcuts {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(88px, 1fr));
  gap: 8px;
}

.wb-shortcut {
  display: block;
  padding: 12px 8px;
  border: 0.5px solid var(--border);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  text-align: center;
  text-decoration: none;
  font-size: 13px;
}

.wb-shortcut:hover {
  border-color: var(--brand);
  color: var(--brand);
}

.wb-list-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 0;
  border-bottom: 0.5px solid var(--border);
}

.wb-list-row:last-child {
  border-bottom: none;
}

.wb-list-main {
  flex: 1;
  min-width: 0;
}

.wb-list-title {
  font-size: 14px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.wb-list-sub {
  font-size: 12px;
  color: var(--text-muted);
  margin-top: 2px;
}

.wb-form {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.wb-input,
.wb-textarea {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
  font-family: inherit;
}

.wb-textarea {
  min-height: 140px;
  resize: vertical;
  line-height: 1.6;
}

.wb-actions {
  display: flex;
  gap: 8px;
  justify-content: flex-end;
  margin-top: 12px;
}

.wb-btn {
  padding: 7px 16px;
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 13px;
  cursor: pointer;
  font-family: inherit;
}

.wb-btn.primary {
  background: var(--brand);
  border-color: var(--brand);
  color: #ffffff;
}

.wb-btn.danger {
  background: var(--danger);
  border-color: var(--danger);
  color: #ffffff;
}

.wb-tool-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 8px;
}

.wb-tool-card {
  padding: 12px;
  border: 0.5px solid var(--border);
  border-radius: 8px;
  background: var(--bg-subtle);
  cursor: pointer;
  font-size: 13px;
}

.wb-tool-card:hover {
  border-color: var(--brand);
}

.wb-md {
  line-height: 1.7;
}

.wb-md pre {
  background: var(--bg-subtle);
  padding: 12px;
  border-radius: 6px;
  overflow-x: auto;
  font-family: Menlo, Consolas, monospace;
  font-size: 12px;
}

.wb-md code {
  background: var(--bg-subtle);
  padding: 1px 4px;
  border-radius: 3px;
  font-family: Menlo, Consolas, monospace;
  font-size: 13px;
}
```

- [ ] **Step 6: 逐页重写其余 17 个页面**

对每个页面，遵循同一模板：

```javascript
"use strict"
/**
 * <页面名> —— <一句话说明>
 *
 * 数据层在 shared/workbench/stores.js，本文件只负责「取数 → 渲染」。
 */

const stores = require("<相对路径>/shared/workbench/stores.js")
const ui = require("<相对路径>/shared/ui.js")
const platform = require("<相对路径>/shared/platform.js")

module.exports = {
  data: { title: "页面标题" },

  onMount: function () {    this.state = Object.assign({}, this.state, buildModel(this.state))
  },

  render: function (state) {
    const model = buildModel(state)
    return ui.el("div", { class: "wb-page" }, [
      ui.pageHead({ title: model.title }),
      ui.panel({ title: "卡片标题", children: [ /* 该页实际内容 */ ] })
    ])
  },

  // 事件处理：名字与小程序版处理函数一一对应
  onSomeAction: function (arg) {
    // 具体实现见下表「事件」列，逐条照小程序版搬
  }
}
```

> **关于上面的模板**：尖括号占位（`<标题>` `<刷新函数>`）是**模板本身**，不是「实现细节留白」。每个页面的**具体内容由下面那张表 + 小程序版源文件唯一确定**：
> - 表的「数据来源」列 → 决定 `buildModel()` 里取哪些域、怎么映射成视图模型
> - 表的「事件」列 → 决定有哪些 `onXxx` 方法、每个方法做什么
> - **数据层的具体逻辑（排序规则、映射字段、统计口径）一律照小程序版页面文件逐字搬**，不重新设计
>
> 搬完后 `wb-view.test.js` 会断言每个页面能加载、能渲染、不含 `undefined`/`NaN`；迁入的 315 个用例会断言数据层逻辑没被改坏。

**主题包装（可选）**：多数页面直接用 `stores.settings.get()` 取设置就够了，不需要 `withTheme`。只有需要「主题类名/主色跟着设置走」的页面（如 `profile/index` 的主色切换）才用：

```javascript
const { withTheme } = require("<相对路径>/shared/core/with-theme.js")
const keys = require("<相对路径>/shared/workbench/keys.js")

// 第二个参数传 workbench 自己的 settings 键
module.exports = withTheme({
  data: { title: "我的" },
  render: function (state) { ... }
}, keys.KEYS.settings)
```

> `withTheme` 的第二个参数是 Task 4 刚加的。缺省不传时读 `db:settings`（knowledge-hub 的键），**workbench 必须传 `keys.KEYS.settings`**。

**逐页的取值来源与事件清单**（照着小程序版页面文件的数据层与交互层逐条搬）：

| 页面 | 数据来源 | 事件（对应小程序版处理函数） |
|---|---|---|
| `profile/index` | `stores.settings.get()` `storage.exportAll/importAll/clearAll` | `onSetPrimary` `onToggleHaptics` `onExport` `onImport` `doImport` `onClearData` `onAbout` |
| `tech/index` | 4 个技术域的 count | `onOpen` |
| `tech/notes/index` | `stores.techNotes.getAll()` + 排序 | `onOpen` `onCreate` `onLongPress` |
| `tech/notes/detail` | `stores.techNotes.find(id)` | `onSave` `onDelete` |
| `tech/snippets/index` | `stores.snippets.getAll()` | `onOpen` `onCreate` `onLongPress` |
| `tech/snippets/detail` | `stores.snippets.find(id)` | `onCopy` `onSave` `onDelete` |
| `tech/interview/index` | `stores.interview.getAll()` | `onOpen` `onCreate` `onLongPress` |
| `tech/interview/detail` | `stores.interview.find(id)` | `onReview` `onSave` `onDelete` |
| `tech/resources/index` | `stores.resources.getAll()` | `onOpen` `onCreate` `onLongPress` |
| `life/index` | 5 个生活域的 count | `onOpen` |
| `life/reading/index` | `stores.reading.getAll()` | `onCreate` `onUpdateProgress` `onDelete` |
| `life/fitness/index` | `stores.fitness.getAll()` | `onCreate` `onDelete` |
| `life/mood/index` | `stores.mood.getAll()` + `getByDate` | `onSetLevel` `onCreate` `onDelete` |
| `life/schedule/index` | `stores.schedule.getAll()` | `onCreate` `onToggleDone` `onDelete` |
| `projects/index` | `stores.projects.getAll()` | `onCreate` `onEdit` `onDelete` `onCopyLink` |
| `insights/index` | `stores.insights.getAll()` | `onCreate` `onEdit` `onDelete` `onLongPress` |
| `tools/index` | `codetools.*` 纯函数 | 10 个工具各自的 `onXxx` |

**移植纪律**：
- **数据层逻辑逐字搬**（排序规则、映射字段、统计口径一个字不改）—— 迁入的 315 个用例就是这块的护栏
- **交互层用 `platform.*` 替换 `wx.*`**，回调式改 Promise 式：

  ```javascript
  // 小程序版
  wx.showModal({
    title: '删除笔记？',
    content: '「' + note.title + '」删除后无法恢复。',
    confirmText: '删除',
    confirmColor: '#E5484D',
    success: function (m) {
      if (!m.confirm) return
      stores.techNotes.remove(id)
      self.refresh()
      wx.showToast({ title: '已删除', icon: 'success' })
    }
  })

  // Web 版
  platform.showModal({
    title: '删除笔记？',
    content: '「' + note.title + '」删除后无法恢复。',
    confirmText: '删除',
    confirmColor: '#E5484D'
  }).then(function (m) {
    if (!m.confirm) return
    stores.techNotes.remove(id)
    self.refresh()
    platform.showToast({ title: '已删除', icon: "success" })
  })
  ```

- **Markdown 用 `md2html()`**（Web 侧 class 生效，不需要内联样式改写）

- [ ] **Step 7: 运行测试确认通过**

Run: `cd apps/web && node tests/wb-view.test.js && node tests/run-all.js`
Expected: `wb-view.test.js` 的 4 个用例过；`run-all.js` 全绿（计划一的 6 组 + 本计划 6 组 + 迁入的 315 个用例）

> 若迁入的某个用例在视图层重写后失败，**说明数据层被改坏了** —— 回到小程序版对照，把数据层改回去。

- [ ] **Step 8: 提交**

```bash
git add apps/web
git commit -m "feat(web): workbench 18 个页面重写为原生 Web 组件

页面模块形状：{ data, onMount?, render(state) → 节点 }。
数据层逐字搬（排序、映射、统计口径一字未改），交互层用 platform.*
替换 wx.*（回调式改 Promise 式）。Markdown 用 md2html()。

wb-view.test.js 补「渲染出来了吗」这一层断言；
迁入的 315 个用例继续作为数据层护栏，全绿。"
git push origin main
```

---

## Task 5：模块接入外壳 + 端到端冒烟

**Files:**
- Modify: `apps/web/web/index.html`（workbench 入口的路径调整）
- Modify: `apps/web/web/style.css`（模块容器样式）
- Modify: `apps/web/server/app.js`（把 `/workbench/*` 路由接上）
- Test: `apps/web/tests/server.test.js`

**Interfaces:**
- Consumes: Task 4 的 `web/workbench/router.js` 与 18 个页面模块
- Produces: `http://127.0.0.1:5173/workbench/pages/dashboard/index` 可访问

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/server.test.js` 末尾追加：

```javascript
test("workbench 模块的页面能被访问", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/workbench/pages/dashboard/index")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("wb-page") >= 0, "应渲染出 workbench 页面内容")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/server.test.js`
Expected: FAIL —— 404（`/workbench/...` 不是静态文件）

### 加载器设计（重要决策）

`shared/` 与 `web/workbench/` 下的模块都是**计划一已经定稿并测通的 CommonJS**。若为了浏览器去把它们全改成 UMD，会：
1. **推翻计划一的成果** —— 它的 6 组测试全绿，改 UMD 就要重测一遍
2. 制造两套模块写法（UMD 与 CommonJS）并存，长期维护心智负担翻倍
3. 每个文件都要改头尾，改动面大、review 难

**因此不改任何模块的写法**，改为在浏览器侧实现一个**极小的 CommonJS 加载器**：

```
浏览器
  ↓ fetch("/shared/core/store.js")
  ↓ new Function("require", "module", "exports", src)  ← 包一层，把 require 指向加载器
  ↓ 加载器解析 require("./storage.js") → 转成 "/shared/core/storage.js" → 递归 fetch
```

**关键点**：用 `new Function` 包一层而不是 `eval` —— 作用域干净，不需要 `window.require` 污染全局，且能精确捕获 `require` 调用。

**代价**：每个模块一次 HTTP 请求。零构建约束下这是标准做法（类似早期 AMD/UMD 的按需加载），本机 `127.0.0.1` 上无性能问题。

**收益**：
- 计划一、二的模块**一字不改**
- Node 侧测试用的就是同一份源码，浏览器跑的也是同一份 —— **不存在「测的和跑的不是同一份」**
- 后续计划三、四、五直接复用，无需再讨论模块格式

### 加载器实现

新建 `apps/web/web/cjs-loader.js`：

```javascript
"use strict"
/**
 * cjs-loader.js —— 浏览器侧的极小 CommonJS 加载器
 *
 * 目的：让计划一、二写好的 CommonJS 模块**一字不改**就能在浏览器里跑。
 *
 * 做法：fetch 源码 → new Function 包一层 → 把 require 指向本加载器。
 * 不改任何模块的写法，也不引入打包工具。
 *
 * 路径规则：以 / 开头的绝对路径直接用；相对路径（./ 与 ../）按当前模块所在目录解析。
 */

;(function (root) {
  "use strict"

  /** 已加载的模块缓存：绝对路径 → exports */
  const cache = {}

  /** 正在加载的模块（用于检测循环依赖） */
  const loading = {}

  /**
   * 把模块路径解析成绝对 URL 路径。
   * @param {string} fromDir 当前模块所在目录，如 "/shared/core"
   * @param {string} request 模块请求串，如 "./storage.js"
   * @returns {string} 绝对路径，如 "/shared/core/storage.js"
   */
  function resolvePath (fromDir, request) {
    if (request.charAt(0) === "/") return request
    const parts = (fromDir + "/" + request).split("/")
    const out = []
    for (let i = 0; i < parts.length; i++) {
      const p = parts[i]
      if (p === "" || p === ".") continue
      if (p === "..") { out.pop(); continue }
      out.push(p)
    }
    return "/" + out.join("/")
  }

  /**
   * 加载一个模块。
   * @param {string} absPath 绝对路径
   * @returns {Promise<object>} 模块 exports
   */
  function load (absPath) {
    if (cache[absPath]) return Promise.resolve(cache[absPath])
    if (loading[absPath]) return loading[absPath]

    const promise = fetch(absPath).then(function (res) {
      if (!res.ok) throw new Error("模块加载失败：" + absPath + "（HTTP " + res.status + "）")
      return res.text()
    }).then(function (src) {
      const module = { exports: {} }
      const dir = absPath.slice(0, absPath.lastIndexOf("/"))

      const localRequire = function (request) {
        const target = resolvePath(dir, request)
        // 同步 require 只支持已加载的模块 —— 模块按依赖顺序预载，这条约束由
        // browser.js 的 PRELOAD 顺序保证。
        if (cache[target]) return cache[target]
        throw new Error("同步 require 拿到未加载的模块：" + target + "（请检查预载顺序）")
      }

      const factory = new Function("require", "module", "exports", src + "\n//# sourceURL=" + absPath)
      factory(localRequire, module, module.exports)
      cache[absPath] = module.exports
      return module.exports
    })

    loading[absPath] = promise
    promise.then(function (exports) {
      cache[absPath] = exports
      delete loading[absPath]
    }, function () {
      delete loading[absPath]
    })
    return promise
  }

  root.CJS = { load: load, resolvePath: resolvePath }
})(typeof globalThis !== "undefined" ? globalThis : this)
```

### 同步 require 的约束（必须在实现时注意）

上面的 `localRequire` **只支持同步拿到已加载的模块**。这带来一条硬约束：

**模块之间的依赖必须按「先加载依赖、再加载使用者」的顺序预载**，不能靠同步 require 链式触发。

因此 `browser.js` 的加载顺序必须是：

```
1.  /shared/core/id.js          （无依赖）
2.  /shared/core/date.js        （无依赖）
3.  /shared/core/format.js      （无依赖）
4.  /shared/core/constants.js   （无依赖）
5.  /shared/core/md2html.js     （无依赖）
6.  /shared/core/md.js          （依赖 5）
7.  /shared/core/storage.js     （无依赖）
8.  /shared/core/store.js       （依赖 7）
9.  /shared/core/theme.js       （依赖 4）
10. /shared/core/with-theme.js （依赖 9 + 7）
11. /shared/workbench/keys.js      （无依赖）
12. /shared/workbench/seed.js      （依赖 1 + 2）
13. /shared/workbench/codetools.js （无依赖）
14. /shared/workbench/stores.js    （依赖 8 + 7 + 12 + 11）
```

**页面模块**（`web/workbench/pages/**`）依赖 `stores.js` 与 `ui.js`/`platform.js`，在 14 之后按需加载。

> **`ui.js` `charts.js` `platform.js` 三个是 UMD**（计划一就是这么写的），浏览器可直接 `<script src>` 引入并挂到 `window.AlgoPlanUI` 等 —— 它们**不进上面的加载顺序**，因为它们不是 CommonJS。

### `browser.js` 实现

新建 `apps/web/web/workbench/browser.js`：

```javascript
"use strict"
/**
 * browser.js —— workbench 模块的浏览器入口
 *
 * 按依赖顺序预载模块，再挂载页面。
 * CommonJS 模块由 cjs-loader.js 负责加载（计划一、二的模块一字不改）。
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
    "/shared/workbench/keys.js",
    "/shared/workbench/seed.js",
    "/shared/workbench/codetools.js",
    "/shared/workbench/stores.js"
  ]

  /** 模块内路径 → 页面文件路径 */
  const PAGES = {
    "/pages/dashboard/index": "/web/workbench/pages/dashboard/index.js",
    "/pages/profile/index": "/web/workbench/pages/profile/index.js",
    "/pages/tech/index": "/web/workbench/pages/tech/index.js",
    "/pages/tech/notes/index": "/web/workbench/pages/tech/notes/index.js",
    "/pages/tech/notes/detail": "/web/workbench/pages/tech/notes/detail.js",
    "/pages/tech/snippets/index": "/web/workbench/pages/tech/snippets/index.js",
    "/pages/tech/snippets/detail": "/web/workbench/pages/tech/snippets/detail.js",
    "/pages/tech/interview/index": "/web/workbench/pages/tech/interview/index.js",
    "/pages/tech/interview/detail": "/web/workbench/pages/tech/interview/detail.js",
    "/pages/tech/resources/index": "/web/workbench/pages/tech/resources/index.js",
    "/pages/life/index": "/web/workbench/pages/life/index.js",
    "/pages/life/reading/index": "/web/workbench/pages/life/reading/index.js",
    "/pages/life/fitness/index": "/web/workbench/pages/life/fitness/index.js",
    "/pages/life/mood/index": "/web/workbench/pages/life/mood/index.js",
    "/pages/life/schedule/index": "/web/workbench/pages/life/schedule/index.js",
    "/pages/projects/index": "/web/workbench/pages/projects/index.js",
    "/pages/insights/index": "/web/workbench/pages/insights/index.js",
    "/pages/tools/index": "/web/workbench/pages/tools/index.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/workbench/, "") || "/pages/dashboard/index"
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
    // 按依赖顺序串行预载
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
      console.error("[workbench] 加载失败", err)
    })
  }

  boot()
})()
```

> **`router.js` 的角色调整**：`web/workbench/router.js` 仍保留，供 **Node 侧测试**用（断言 18 个路由齐全、路径解析正确）。浏览器侧用 `browser.js` 里的 `PAGES` 表 —— 两张表内容一致，由 `wb-view.test.js` 的「路由表覆盖 18 个页面」用例守住一致性。

- [ ] **Step 3: 在 HTML 壳里引 script**

`ui.js` `charts.js` `platform.js` 三个是 UMD，浏览器可直接 `<script src>` 引入。CommonJS 模块由 `cjs-loader.js` 按需 fetch，**不需要** script 标签。

新建 `apps/web/web/workbench/shell.html`（服务端返回的内容模板）：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>程序员工作台</title>
  <link rel="stylesheet" href="/style.css">
  <link rel="stylesheet" href="/workbench/style.css">
</head>
<body class="theme-light">
  <div class="app-shell">
    <header class="app-head">
      <h1 class="app-title">程序员工作台</h1>
      <nav class="app-nav">
        <a class="nav-item" href="/">返回工作台</a>
        <a class="nav-item" href="/workbench/pages/dashboard/index">今日</a>
        <a class="nav-item" href="/workbench/pages/tech/index">技术</a>
        <a class="nav-item" href="/workbench/pages/life/index">生活</a>
        <a class="nav-item" href="/workbench/pages/tools/index">工具</a>
        <a class="nav-item" href="/workbench/pages/profile/index">我的</a>
      </nav>
    </header>
    <main class="app-main" id="app-main"></main>
  </div>
  <script src="/shared/ui.js"></script>
  <script src="/shared/charts.js"></script>
  <script src="/shared/platform.js"></script>
  <script src="/cjs-loader.js"></script>
  <script src="/workbench/browser.js"></script>
</body>
</html>
```

- [ ] **Step 4: 在 `server/app.js` 里挂 workbench 路由**

在 `apps/web/server/app.js` 顶部加 require：

```javascript
const fs = require("node:fs")
const path = require("node:path")
```

（`fs` 若已有则不重复加。）

在 `createApp` 里，`routes.forEach(...)` 之后加：

```javascript
  // workbench 模块：所有 /workbench/pages/* 都返回同一个 HTML 壳，
  // 实际页面由前端的 cjs-loader 按路径加载。
  const WORKBENCH_SHELL = path.join(__dirname, "..", "web", "workbench", "shell.html")
  const workbenchShell = function (req, res) {
    let html
    try {
      html = fs.readFileSync(WORKBENCH_SHELL, "utf8")
    } catch (e) {
      sendJson(res, 500, { error: "workbench 页面壳读取失败" })
      return
    }
    res.writeHead(200, {
      "Content-Type": "text/html; charset=utf-8",
      "Content-Length": Buffer.byteLength(html)
    })
    res.end(html)
  }
  routes.push({ method: "GET", pattern: "/workbench/pages/:a/:b/:c", handler: workbenchShell })
  routes.push({ method: "GET", pattern: "/workbench/pages/:a/:b", handler: workbenchShell })
```

> `/cjs-loader.js` 由静态托管直接服务（它在 `web/` 下）。**注意**：静态托管的 `webRoot` 是 `apps/web/web`，而 `cjs-loader.js` 若放在 `apps/web/web/` 下则路径为 `/cjs-loader.js` —— 上面 shell.html 引的就是这个路径，故把 `cjs-loader.js` 放在 `apps/web/web/cjs-loader.js`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/server.test.js && node tests/run-all.js`
Expected: 全绿

- [ ] **Step 6: 手工端到端验证**

```bash
cd apps/web && node server/main.js
```

另开终端：
```bash
curl -s http://127.0.0.1:5173/workbench/pages/dashboard/index | head -20
```
Expected: 输出 HTML 壳，含 `<title>程序员工作台</title>` 与 `browser.js` 的 script 标签

再验证加载器能取到模块：
```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5173/shared/workbench/stores.js
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5173/shared/core/store.js
```
Expected: 都是 `200`

浏览器打开 `http://127.0.0.1:5173/workbench/pages/dashboard/index`，确认：
- 今日待办有数据（3 条种子待办）
- 4 个指标带显示数字
- 心情打卡有 5 个圆点
- 快捷入口 5 个可点
- 浏览器控制台**无报错**（加载器失败会在页面显示「模块加载失败」）

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): workbench 模块接入外壳，浏览器可访问

新增 cjs-loader.js —— 浏览器侧的极小 CommonJS 加载器，fetch 源码后用
new Function 包一层，把 require 指向加载器。计划一、二的模块一字不改
就能在浏览器跑，也避免了「测的和跑的不是同一份」。

shell.html 返回 HTML 壳，browser.js 按依赖顺序预载 14 个 CommonJS 模块
再挂载页面。/workbench/pages/* 由服务端统一返回壳。"
git push origin main
```

---

## 验收标准（本计划完成时）

1. `cd apps/web && node tests/run-all.js` —— **全绿**，其中迁入的 **315 个用例**（106 + 48 + 48 + 40 + 43 + 30）全部通过
2. `node apps/web/server/main.js` 后，浏览器打开 `http://127.0.0.1:5173/workbench/pages/dashboard/index`，18 个页面均可访问且可交互
3. `shared/workbench/stores.js` 导出 12 个域，`shared/workbench/codetools.js` 导出 24 个函数 —— 与小程序版**逐字一致**（除 settings 键名）
4. workbench 的 settings 存在 `db:wb_settings`，**不与** knowledge-hub 的 `db:settings` 冲突
5. `apps/programmer-workbench/` 目录**未删未改**，作为对照参考
6. **未修改** `engine/`、`apps/study-hub/`、`apps/wiki-rag/`
7. `git status` 干净

## 后续计划

| 计划 | 内容 | 依赖本计划的 |
|---|---|---|
| 计划三 | knowledge-hub 数据层 + 视图层（17 页 + 8 组件） | `shared/platform.js`（含 Task 1 的三个新 API）、UMD 模式、`wb-harness.js` 脚手架模式 |
| 计划四 | trip-log + games | 同上（共享层的 `shared/platform.js` 与 `wb-harness.js` 脚手架模式） |
| 计划五 | 后端挂载 + 删旧目录 + 更新 README | 本计划 + 计划三 + 计划四 |

> **计划三的已知差异**：knowledge-hub 的 `stores.js:39` 有 `Object.assign(storage.KEYS, KH_KEYS)` 这行**就地扩展**。workbench 已改用独立的 `wb_settings`，所以 knowledge-hub 保持 `db:settings` 不变即可 —— **两个模块不再抢键**。计划三只需把那行改成 `Object.assign(本地副本, KH_KEYS)`，不再污染共享对象。
