# apps/web 合并 · 计划五：收尾（后端导航 + 删旧目录 + 更新 README）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把合并应用的导航补全（study-hub 与 wiki-rag 以独立端口接入）、删除 4 个旧小程序目录、更新 `apps/README.md` 与 `AGENTS.md`，完成整个合并。

**Architecture:** 合并应用 `apps/web` 独占 5173 端口，承载 workbench / knowledge / trip / games 四个前端模块。study-hub（5174）与 wiki-rag（8000）**保持独立端口**，合并应用的外壳用普通链接指过去 —— 这是实测后确定的方案（见「关键设计裁决」）。

**Tech Stack:** Node.js ≥ 24、零 npm 依赖、零构建。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：`apps/web/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。
- **零依赖**：不新增任何 npm 包。
- **中文文案**：面向用户的文案一律中文。
- **测试命令**：`cd apps/web && node tests/run-all.js`。
- **依赖前提**：本计划**必须在计划一、二、三、四全部完成后**执行。
- **不改 `engine/`、`apps/study-hub/`、`apps/wiki-rag/`**：本计划只**链接**它们，不改它们的代码。
- **端口约定**（本机）：
  | 端口 | 服务 | 启动方式 |
  |---|---|---|
  | 5173 | 合并应用 `apps/web` | `node apps/web/server/main.js` |
  | 5174 | study-hub | `PORT=5174 node apps/study-hub/server/main.js` |
  | 8000 | wiki-rag | `python -m server.main`（在 `apps/wiki-rag` 下） |

---

## 关键设计裁决：为什么后端不反代

**这是实施规划阶段实测得出的结论，与设计文档 §3.2 的原设想不同。**

设计文档原设想把 study-hub 挂到 `/study/*`、wiki-rag 反代到 `/wiki/*`。实测发现**两个应用的前端都硬编码了绝对路径**：

| 应用 | 绝对路径处数 | 具体 |
|---|---|---|
| study-hub | **35 处** `fetch("/api/xxx")` + 若干 `script src="/ui.js"` | 分布在 `web/app.js` `web/llm.js` `web/diagnose.js` `web/pages/*.js` 共 10 个文件 |
| wiki-rag | **4 处** | `index.html` 的 `/style.css`、`/app.js`；`app.js` 的 `/api/health`、`/api/projects` |

挂到子路径后，这些绝对路径**不会**跟着走：
- 前端 `fetch("/api/session")` 会打到合并应用的 `/api/session`（不存在）→ 404
- `script src="/ui.js"` 会打到合并应用的 `/ui.js`（不存在）→ 白屏
- 静态资源还会在根路径撞名：合并应用的 `style.css` 与两个后端的 `style.css` 同名

三条路可选：

| 方案 | 代价 |
|---|---|
| A. 反代 + 改写 HTML/JS 里的绝对路径 | 改写逻辑对别人的路径有假设，以后对方改了路径就碎 |
| B. 改 study-hub 支持前缀 | 改 35 处别人的代码，违反「不改 study-hub」约束 |
| C. **保持独立端口，外壳用链接指过去** | 从合并应用点进去开新标签页，不是同一个 SPA |

**选 C**。理由：
1. `apps/study-hub/` 与 `apps/wiki-rag/` **一行不改** —— 它们有固定层级路径假设（设计文档 §2.5），是自包含应用
2. 两个后端本来就是**独立进程**（Node / Python），本来就得分开启动
3. 合并应用的价值在于**把 4 个小程序的 43 个页面收进一个入口**，后端本来就该独立

**代价要说清楚**：点「学习中枢」会打开新标签页 `http://127.0.0.1:5174/`，视觉上不是同一个 SPA。这是**有意识的取舍**，不是偷懒。

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/web/web/index.html` | 改 | 外壳导航补上 study-hub 与 wiki-rag 的独立端口链接 |
| `apps/web/web/shell.js` | 新建 | 外壳脚本：探测两个后端的在线状态并显示 |
| `apps/web/web/style.css` | 改 | 外链样式（区分「本页模块」与「独立服务」两类入口） |
| `apps/web/server/health.js` | 新建 | 探测 study-hub(5174) 与 wiki-rag(8000) 是否在线 |
| `apps/web/server/app.js` | 改 | 注册 `/api/health` 路由，返回两个后端的在线状态 |
| `apps/web/tests/health.test.js` | 新建 | 健康检查测试 |
| `apps/web/tests/server.test.js` | 改 | 补一条：外壳含三个后端入口 |
| `apps/README.md` | 改 | 全面重写：描述合并后的架构与启动方式 |
| `AGENTS.md` | 改 | 更新「前端说明（已迁移）」小节 |
| `CLAUDE.md` | 改 | 更新「项目布局」小节 |
| `apps/games/` | 删 | 旧小程序（37 文件 / 144 KB） |
| `apps/knowledge-hub/` | 删 | 旧小程序（132 文件 / 324 KB） |
| `apps/programmer-workbench/` | 删 | 旧小程序（131 文件 / 388 KB） |
| `apps/trip-log/` | 删 | 旧小程序（43 文件 / 1.7 MB，含 node_modules） |

---

## Task 1：后端在线状态探测

**Files:**
- Create: `apps/web/server/health.js`
- Create: `apps/web/tests/health.test.js`
- Modify: `apps/web/server/app.js`

**Interfaces:**
- Consumes: 计划一的 `server/app.js`（路由注册机制）、`server/router.js`
- Produces:
  - `server/health.js` 导出 `probe(url, timeoutMs)` → `Promise<{ok: boolean, status?: number, error?: string}>`
  - `server/health.js` 导出 `checkBackends()` → `Promise<{study: object, wiki: object}>`
  - `server/app.js` 注册路由 `GET /api/health` → `{ ok, study: {...}, wiki: {...} }`

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/health.test.js`：

```javascript
"use strict"
/**
 * health.test.js —— 后端在线状态探测
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")

const { probe, checkBackends } = require("../server/health.js")

/** 起一个临时 HTTP 服务，用来测「在线」的情况 */
function startServer (handler) {
  return new Promise(function (resolve) {
    const server = http.createServer(handler)
    server.listen(0, "127.0.0.1", function () {
      resolve({ server: server, port: server.address().port })
    })
  })
}

test("probe：服务在线时 ok=true", async function () {
  const s = await startServer(function (req, res) {
    res.writeHead(200, { "Content-Type": "application/json" })
    res.end(JSON.stringify({ status: "ok" }))
  })
  try {
    const r = await probe("http://127.0.0.1:" + s.port + "/api/health")
    assert.strictEqual(r.ok, true)
    assert.strictEqual(r.status, 200)
  } finally {
    s.server.close()
  }
})

test("probe：服务未启动时 ok=false 且带中文说明", async function () {
  // 找一个没人监听的端口
  const s = await startServer(function () {})
  const port = s.port
  s.server.close()

  const r = await probe("http://127.0.0.1:" + port + "/api/health", 500)
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0, "应有错误说明")
})

test("probe：超时返回 ok=false，不挂住", async function () {
  const s = await startServer(function () { /* 故意不响应 */ })
  try {
    const r = await probe("http://127.0.0.1:" + s.port + "/", 300)
    assert.strictEqual(r.ok, false)
    assert.ok(r.error.indexOf("超时") >= 0 || r.error.indexOf("连接") >= 0,
      "错误说明应提到超时或连接，实际：" + r.error)
  } finally {
    s.server.close()
  }
})

test("checkBackends：返回 study 与 wiki 两项", async function () {
  const r = await checkBackends()
  assert.ok(r.study, "应有 study 项")
  assert.ok(r.wiki, "应有 wiki 项")
  assert.strictEqual(typeof r.study.ok, "boolean")
  assert.strictEqual(typeof r.wiki.ok, "boolean")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/health.test.js`
Expected: FAIL —— 报 `Cannot find module '../server/health.js'`

- [ ] **Step 3: 写 `server/health.js`**

新建 `apps/web/server/health.js`：

```javascript
"use strict"
/**
 * health.js —— 后端在线状态探测
 *
 * study-hub(5174) 与 wiki-rag(8000) 是**独立进程**，可能没启动。
 * 合并应用的外壳要能告诉用户「哪个还没起来」，而不是让用户点进去才发现。
 *
 * 零依赖：用 node:http 发一个 GET，不引第三方 HTTP 客户端。
 */

const http = require("node:http")

/** study-hub 的地址（独立端口） */
const STUDY_URL = process.env.STUDY_HUB_URL || "http://127.0.0.1:5174"

/** wiki-rag 的地址（独立端口，Python） */
const WIKI_URL = process.env.WIKI_RAG_URL || "http://127.0.0.1:8000"

/**
 * 探测一个 URL 是否可达。
 * @param {string} url
 * @param {number} [timeoutMs] 超时毫秒数，默认 1500
 * @returns {Promise<{ok: boolean, status?: number, error?: string}>}
 */
function probe (url, timeoutMs) {
  const timeout = timeoutMs === undefined ? 1500 : timeoutMs
  return new Promise(function (resolve) {
    let settled = false
    const done = function (result) {
      if (settled) return
      settled = true
      resolve(result)
    }

    let parsed
    try {
      parsed = new URL(url)
    } catch (e) {
      done({ ok: false, error: "地址不合法：" + url })
      return
    }

    const req = http.request(
      {
        host: parsed.hostname,
        port: parsed.port || 80,
        path: parsed.pathname + parsed.search,
        method: "GET",
        timeout: timeout
      },
      function (res) {
        // 不管状态码是多少，能连上就说明服务在
        res.resume()
        done({ ok: true, status: res.statusCode })
      }
    )

    req.on("timeout", function () {
      req.destroy()
      done({ ok: false, error: "连接超时（" + timeout + "ms 内没响应）" })
    })
    req.on("error", function (err) {
      done({ ok: false, error: "连接失败：" + (err && err.code ? err.code : "未知错误") })
    })
    req.end()
  })
}

/**
 * 探测两个后端。
 * @returns {Promise<{study: object, wiki: object}>}
 */
function checkBackends () {
  return Promise.all([
    probe(STUDY_URL + "/api/health"),
    probe(WIKI_URL + "/api/health")
  ]).then(function (results) {
    return {
      study: {
        ok: results[0].ok,
        status: results[0].status,
        error: results[0].error,
        url: STUDY_URL,
        hint: results[0].ok ? "" : "学习中枢未启动，请运行：PORT=5174 node apps/study-hub/server/main.js"
      },
      wiki: {
        ok: results[1].ok,
        status: results[1].status,
        error: results[1].error,
        url: WIKI_URL,
        hint: results[1].ok ? "" : "Wiki 检索未启动，请运行：cd apps/wiki-rag && python -m server.main"
      }
    }
  })
}

module.exports = { STUDY_URL: STUDY_URL, WIKI_URL: WIKI_URL, probe: probe, checkBackends: checkBackends }
```

- [ ] **Step 4: 在 `app.js` 注册 `/api/health` 路由**

在 `apps/web/server/app.js` 顶部加：

```javascript
const { checkBackends } = require("./health.js")
```

在 `createApp` 里，`routes.forEach(...)` 之后加：

```javascript
  // 后端在线状态：study-hub 与 wiki-rag 是独立进程，外壳要用它提示「还没启动」
  routes.push({
    method: "GET",
    pattern: "/api/health",
    handler: function (req, res) {
      checkBackends().then(function (result) {
        sendJson(res, 200, { ok: true, study: result.study, wiki: result.wiki })
      }).catch(function (err) {
        sendJson(res, 200, { ok: false, error: (err && err.message) || "探测失败" })
      })
    }
  })
```

> **注意**：这个 `/api/health` 是合并应用**自己**的路由，与 study-hub 的 `/api/health` 不是一回事 —— 后者在 5174 端口上。合并应用的 `/api/health` 只探测它，不提供它自己的功能。

- [ ] **Step 5: 运行测试确认通过**

Run:
```bash
cd apps/web && node tests/health.test.js && node tests/run-all.js
```
Expected: PASS —— health 4 个用例过；全量测试全绿

> `checkBackends()` 在测试环境会探测 5174 与 8000 —— 这两个端口大概率没服务，返回 `ok: false` 是**预期**的，不影响断言（断言只看 `typeof ok === "boolean"`）。

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 新增后端在线状态探测

study-hub(5174) 与 wiki-rag(8000) 是独立进程，可能没启动。
合并应用的外壳要能提前告诉用户「哪个还没起来」，
而不是让用户点进去才发现 404 或白屏。

server/health.js 用 node:http 探测，零依赖；
/api/health 返回两个后端的在线状态与中文启动提示。"
git push origin main
```

---

## Task 2：外壳导航补全

**Files:**
- Modify: `apps/web/web/index.html`
- Modify: `apps/web/web/style.css`
- Modify: `apps/web/tests/server.test.js`

**Interfaces:**
- Consumes: Task 1 的 `/api/health`
- Produces: 外壳含 **6 个模块入口**（4 个本页模块 + 2 个独立服务）

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/server.test.js` 末尾追加：

```javascript
test("外壳：六个模块入口齐全，含两个独立服务", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  const html = r.body
  // 四个本页模块
  ["/workbench/", "/knowledge/", "/trip/", "/games/"].forEach(function (href) {
    assert.ok(html.indexOf(href) >= 0, "外壳应含入口：" + href)
  })
  // 两个独立服务：study-hub 与 wiki-rag 走各自端口
  assert.ok(html.indexOf("5174") >= 0, "应含 study-hub 的独立端口链接")
  assert.ok(html.indexOf("8000") >= 0, "应含 wiki-rag 的独立端口链接")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/server.test.js`
Expected: FAIL —— 外壳还没有两个独立服务的入口

- [ ] **Step 3: 改 `web/index.html`**

把 `apps/web/web/index.html` 的 `<nav>` 改成：

```html
      <nav class="app-nav" id="app-nav">
        <a class="nav-item" href="/workbench/">程序员工作台</a>
        <a class="nav-item" href="/knowledge/">知识中心</a>
        <a class="nav-item" href="/trip/">足迹</a>
        <a class="nav-item" href="/games/">游戏</a>
      </nav>
```

把 `<main class="app-main" id="app-main">` 里的内容改成：

```html
    <main class="app-main" id="app-main">
      <section class="panel hero">
        <h2>选择模块</h2>
        <p class="reason">下面四个模块在本页内切换。</p>
        <div class="module-grid" id="module-grid">
          <a class="module-card" href="/workbench/">
            <span class="module-title">程序员工作台</span>
            <span class="module-desc">待办、技术笔记、生活记录与 10 个离线工具</span>
          </a>
          <a class="module-card" href="/knowledge/">
            <span class="module-title">知识中心</span>
            <span class="module-desc">题库复习、面试追踪、知识图谱与五年计划</span>
          </a>
          <a class="module-card" href="/trip/">
            <span class="module-title">足迹</span>
            <span class="module-desc">记录走过的地方与照片</span>
          </a>
          <a class="module-card" href="/games/">
            <span class="module-title">游戏</span>
            <span class="module-desc">贪吃蛇、2048、数独、消消乐</span>
          </a>
        </div>
      </section>

      <section class="panel">
        <h2>独立服务</h2>
        <p class="reason">下面两个是独立进程，有各自的端口与界面，点开会开新标签页。</p>
        <div class="module-grid" id="service-grid">
          <a class="module-card external" id="study-link" href="http://127.0.0.1:5174/" target="_blank" rel="noopener">
            <span class="module-title">学习中枢 <span class="module-state" id="study-state">检测中…</span></span>
            <span class="module-desc">算法 / 数据库 / AI 三轨，判题与间隔复习</span>
          </a>
          <a class="module-card external" id="wiki-link" href="http://127.0.0.1:8000/" target="_blank" rel="noopener">
            <span class="module-title">Wiki 检索 <span class="module-state" id="wiki-state">检测中…</span></span>
            <span class="module-desc">本地 wiki 语料的混合检索与抽取式问答</span>
          </a>
        </div>
      </section>
    </main>
```

在 `</body>` 前加：

```html
  <script src="/shell.js"></script>
```

> **文件名叫 `shell.js` 而不是 `app.js`** —— `server/app.js` 是组合根，`web/app.js` 会跟它混淆。
> 而且 `study-hub/web/app.js` 与 `wiki-rag/web/app.js` 也叫 app.js，虽在不同端口不冲突，但同名容易看错。

新建 `apps/web/web/shell.js`：

```javascript
"use strict"
/**
 * shell.js —— 合并应用外壳的脚本
 *
 * 只做一件事：探测两个独立后端的在线状态，把结果显示在入口卡片上。
 */

;(function () {
  "use strict"

  /**
   * 更新一个后端入口的状态显示。
   * @param {string} id 状态元素的 id
   * @param {object} info checkBackends 返回的那一项
   */
  function paintState (id, info) {
    const node = document.getElementById(id)
    if (!node) return
    if (info.ok) {
      node.textContent = "运行中"
      node.className = "module-state ok"
    } else {
      node.textContent = "未启动"
      node.className = "module-state off"
      node.setAttribute("title", info.hint || "")
    }
  }

  fetch("/api/health")
    .then(function (res) { return res.json() })
    .then(function (data) {
      if (!data || !data.ok) return
      paintState("study-state", data.study || { ok: false })
      paintState("wiki-state", data.wiki || { ok: false })
    })
    .catch(function (err) {
      console.error("[app] 健康检查失败", err)
      paintState("study-state", { ok: false, hint: "健康检查请求失败" })
      paintState("wiki-state", { ok: false, hint: "健康检查请求失败" })
    })
})()
```

- [ ] **Step 4: 在 `web/style.css` 加模块卡片样式**

在 `apps/web/web/style.css` 的「卡」小节之后插入：

```css
/* 模块入口卡片 */
.module-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 10px;
  margin-top: 12px;
}

.module-card {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 14px;
  border: 0.5px solid var(--border);
  border-radius: 10px;
  background: var(--bg-subtle);
  text-decoration: none;
}

.module-card:hover {
  border-color: var(--brand);
}

.module-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
}

.module-desc {
  font-size: 12px;
  color: var(--text-muted);
}

.module-state {
  font-size: 11px;
  font-weight: 400;
  padding: 1px 6px;
  border-radius: 999px;
  background: var(--bg-primary);
  color: var(--text-muted);
}

.module-state.ok {
  background: rgba(48, 164, 108, 0.14);
  color: var(--ok);
}

.module-state.off {
  background: rgba(245, 165, 36, 0.16);
  color: var(--warn);
}

.module-card.external {
  border-style: dashed;
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/server.test.js && node tests/run-all.js`
Expected: 全绿

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 外壳导航补全六个模块入口

四个本页模块（workbench/knowledge/trip/games）走 SPA 路由；
study-hub(5174) 与 wiki-rag(8000) 是独立进程，用普通链接指过去，
打开新标签页。入口卡片上显示各后端的在线状态（运行中/未启动），
未启动时鼠标悬停给出中文启动命令。"
git push origin main
```

---

## Task 3：删旧小程序目录（严格前置）

**Files:**
- Delete: `apps/games/`（37 文件 / 144 KB）
- Delete: `apps/knowledge-hub/`（132 文件 / 324 KB）
- Delete: `apps/programmer-workbench/`（131 文件 / 388 KB）
- Delete: `apps/trip-log/`（43 文件 / 1.7 MB，含 node_modules）

**Interfaces:**
- Consumes: 计划一、二、三、四的**全部迁入测试**
- Produces: `apps/` 下只剩 `web/` `study-hub/` `wiki-rag/` `README.md`

- [ ] **Step 1: 核对前置条件 —— 562 个用例必须全绿**

Run:
```bash
cd apps/web && node tests/run-all.js
```

Expected: **全部通过**，其中包含：

| 模块 | 迁入的用例 | 来自 |
|---|---|---|
| programmer-workbench | 315 | 计划二 |
| knowledge-hub | 181 | 计划三 |
| games | 51 | 计划四 |
| trip-log | 15（新增） | 计划四 |
| **合计** | **562** | |

> **任何一个失败都不许往下走** —— 删了旧目录就没法对照了。先修测试。

- [ ] **Step 2: 核对新代码确实覆盖了旧代码的功能**

逐项对照，确认没有功能遗漏：

```bash
# 旧目录还在（删之前），列出页面清单做对照
ls apps/programmer-workbench/pages/**/index.js 2>/dev/null | wc -l
ls apps/knowledge-hub/pages/**/index.js 2>/dev/null | wc -l
ls apps/games/pages/*/index.js 2>/dev/null | wc -l
ls apps/trip-log/pages/*/*.js 2>/dev/null | wc -l
```

Expected: 分别是 **18 / 17 / 5 / 3**，与新代码的页面数一致。

再核对新代码：

```bash
ls apps/web/web/workbench/pages/ 2>/dev/null
ls apps/web/web/knowledge/pages/ 2>/dev/null
ls apps/web/web/games/pages/ 2>/dev/null
ls apps/web/web/trip/pages/ 2>/dev/null
```

**逐个打开页面确认可用**（浏览器手工验证）：
- workbench 的 18 个页面都能打开、工具箱 10 个工具可用
- knowledge-hub 的 17 个页面都能打开、**知识图谱与技术雷达的 Canvas 有内容**
- games 的 4 个游戏可玩、**贪吃蛇的 Canvas 有内容**
- trip 的新增 → 存照片 → 详情页看到照片 → 删除，闭环可用

> **Canvas 页面必须实际看一眼** —— 页面能加载不等于画对了。

- [ ] **Step 3: 删目录**

```bash
git rm -r apps/games apps/knowledge-hub apps/programmer-workbench apps/trip-log
```

> `git rm` 而非 `rm` —— 让 git 记录删除，误删可回滚。
>
> `apps/trip-log/` 里有 `node_modules/`（腾讯云 SDK），`git rm -r` 只会删 git 跟踪的文件；`node_modules` 本来就在 `.gitignore` 里，不在 git 索引中，**需要手动删目录**：
>
> ```bash
> rm -rf apps/trip-log/node_modules
> ```
>
> 若 `git rm -r` 报「pathspec did not match」，说明 `node_modules` 里有些文件被 git 跟踪了（说明 `.gitignore` 规则被覆盖），用：
>
> ```bash
> rm -rf apps/trip-log
> git add -A apps/trip-log
> ```

- [ ] **Step 4: 确认目录确实没了**

```bash
ls apps/
```
Expected: 只有 `README.md` `web` `study-hub` `wiki-rag`（可能还有 `.archive`）

```bash
git status --short
```
Expected: 只显示 4 个目录的删除

- [ ] **Step 5: 跑一遍全量测试确认没删错东西**

Run:
```bash
cd apps/web && node tests/run-all.js
```
Expected: 全绿

> 如果这里挂了，说明新代码**依赖**了旧目录里的某个文件 —— 那就说明移植不彻底，**恢复目录**（`git checkout -- apps/xxx`）再查。

- [ ] **Step 6: 提交**

```bash
git commit -m "refactor(apps): 删除 4 个旧小程序目录

workbench(18 页) / knowledge-hub(17 页 + 2 组件) / trip-log(3 页) /
games(5 页) 已整体迁入 apps/web/，562 个测试用例全部迁入并通过。

删除前逐页手工验证过：43 个页面都能打开，工具箱 10 个工具可用，
知识图谱与技术雷达的 Canvas 有内容，trip 的图片存取闭环可用。

study-hub 与 wiki-rag 原地保留（独立端口，路径有固定层级假设）。"
git push origin main
```

---

## Task 4：更新文档

**Files:**
- Modify: `apps/README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: 计划一～四的实际产出
- Produces: 与新架构一致的文档

- [ ] **Step 1: 重写 `apps/README.md`**

用下面这份内容**整体替换** `apps/README.md`：

````markdown
# 应用集合

本目录是仓库内**所有本机应用**的 canonical 位置。

合并后的形态：一个入口 + 两个独立服务。

```
apps/
├── web/           合并应用（Node 服务，5173）
│   ├── server/    组合根：路由、静态托管、健康检查
│   ├── shared/    共用层：core / ui / charts / platform / photos + 各模块数据层
│   └── web/       各模块前端（workbench / knowledge / trip / games）
├── study-hub/     学习中枢（Node，5174，独立进程）
└── wiki-rag/      Wiki 检索（Python FastAPI，8000，独立进程）
```

---

## 合并应用 · `web/`（5173）

**一个入口，四个模块。** 原先的四个微信小程序（workbench / knowledge-hub / trip-log / games）
已整体移植为原生 Web 页面，共 43 个页面。

### 启动

```bash
node apps/web/server/main.js
# 打开 http://127.0.0.1:5173
```

需要 Node ≥ 24。零 npm 依赖、零构建。

### 模块

| 模块 | 路径 | 页面数 | 说明 |
|---|---|---|---|
| 程序员工作台 | `/workbench/` | 18 | 待办、技术笔记、代码片段、面试题库、资源、读书、健身、心情、日程、项目集、闪念 + 10 个离线工具 |
| 知识中心 | `/knowledge/` | 17 | SM-2 间隔复习、题库练习与错题本、面试追踪与模拟面试、知识图谱（Canvas）、技术雷达（Canvas）、五年计划 |
| 足迹 | `/trip/` | 3 | 记录走过的地方与照片（照片存 IndexedDB） |
| 游戏 | `/games/` | 5 | 贪吃蛇、2048、数独、消消乐 |

### 数据存哪

| 数据 | 存储 | 说明 |
|---|---|---|
| workbench / knowledge / trip 的业务数据 | `localStorage`，`db:` 前缀 | 按数据域分键 |
| games 的存档 | `localStorage`，`games_` 前缀 | 与上面不冲突 |
| trip 的照片 | **IndexedDB** | 单条记录最多 9 张 × 5MB，超出 localStorage 配额 |

全部数据只在本机，不上传任何服务器。

### 目录

```
web/
├── server/          组合根：main / app / router / static / health
├── shared/
│   ├── core/        通用层：storage / store / id / date / format / md / constants / theme
│   ├── ui.js        构件层（页头 / 主卡 / 数字块 / 药丸 / 进度条 / 空态）
│   ├── charts.js    手绘 SVG 图表层
│   ├── platform.js  平台适配层（承接 wx 的 9 个交互 API）
│   ├── photos.js    IndexedDB 图片存储层
│   ├── workbench/   workbench 数据层（12 个数据域 + 10 个工具）
│   ├── knowledge/   knowledge-hub 数据层（14 个数据域 + 图谱/雷达布局）
│   ├── trip/        trip 数据层 + 导出导入
│   └── games/       4 个游戏引擎 + 存档层
├── web/             各模块前端 + cjs-loader.js（浏览器侧 CommonJS 加载器）
└── tests/           全部测试，一条命令跑完
```

### 测试

```bash
cd apps/web && node tests/run-all.js
```

---

## 学习中枢 · `study-hub/`（5174）

算法 / 数据库 / AI 三轨合一的学习助手。本机运行，判题走真实编译执行。

**独立进程**，不与合并应用耦合。

```bash
PORT=5174 node apps/study-hub/server/main.js
# 打开 http://127.0.0.1:5174
```

需要 Node ≥ 24。设 `STUDY_HUB_LEARNING_ROOT` 指向 learning 仓可开启同步；不设则纯本地模式。

```bash
cd apps/study-hub && node tests/run-all.js
```

---

## Wiki 检索 · `wiki-rag/`（8000）

本地 wiki 语料的三栏混合检索工作台。答案为抽取式，只拼接检索到的原文块。

**独立进程**（Python）。

```powershell
cd apps\wiki-rag
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python tools/ingest.py --project postgres   # 建索引
python -m server.main
# 打开 http://127.0.0.1:8000
```

```powershell
python -m pytest tests -q
```

---

## 三个服务的关系

合并应用的外壳是**导航入口**；study-hub 与 wiki-rag 各自独立启动、独立端口。

原因是这两个应用的前端硬编码了绝对路径（study-hub 有 35 处 `fetch("/api/...")`），
挂到子路径下会全部失效。保持独立端口，它们一行都不用改。

外壳首页会显示两个后端的在线状态（运行中 / 未启动），未启动时鼠标悬停给出启动命令。
````

- [ ] **Step 2: 更新 `AGENTS.md` 的「前端说明（已迁移）」小节**

在 `AGENTS.md` 里找到 `## 前端说明（已迁移）` 那一节，**整节替换**为：

```markdown
## 前端说明（已合并）

前端已从微信小程序重写为**本机 Web 应用**（2026-11-26 完成合并）。

原先的 4 个微信小程序（programmer-workbench / knowledge-hub / trip-log / games）已整体
移植为原生 Web 页面，合并进 `apps/web/`：

| 模块 | 路径 | 页面数 |
|---|---|---|
| 程序员工作台 | `/workbench/` | 18 |
| 知识中心 | `/knowledge/` | 17 |
| 足迹 | `/trip/` | 3 |
| 游戏 | `/games/` | 5 |

`apps/study-hub/`（学习中枢，5174）与 `apps/wiki-rag/`（Wiki 检索，8000）是**独立进程**，
保持原地不动 —— 它们的前端硬编码了绝对路径，挂到子路径下会失效。

```bash
node apps/web/server/main.js              # 合并应用，5173
PORT=5174 node apps/study-hub/server/main.js   # 学习中枢，5174
cd apps/wiki-rag && python -m server.main      # Wiki 检索，8000
```

合并应用零 npm 依赖、零构建。测试：`cd apps/web && node tests/run-all.js`。
详见 [`apps/README.md`](apps/README.md)。
```

同时，删掉 `AGENTS.md` 里「前端说明（已迁移）」小节中那张**旧的表格**（列 `knowledge-hub` / `games` / `programmer-workbench` / `trip-log` 与「位置」的那张），因为它已被新内容取代。

> **核对**：`AGENTS.md` 里凡是提到 `apps/knowledge-hub/` `apps/games/` `apps/programmer-workbench/` `apps/trip-log/` 这些**已删除目录**的地方，都要一并更新或删除。用这条命令找：
>
> ```bash
> grep -n "apps/knowledge-hub\|apps/games\|apps/programmer-workbench\|apps/trip-log" AGENTS.md CLAUDE.md
> ```

- [ ] **Step 3: 更新 `CLAUDE.md` 的「项目布局」小节**

`CLAUDE.md` 的「项目布局」小节里有个目录树代码块（`engine/` 那段，代码块从 `engine/` 开头到 `test-results/` 那行结束）。**在这个代码块内部补上 `apps/`** —— 插在 `engine/` 那一段的后面、`docs/` 那行之前：

````markdown
apps/                # 前端应用（微信小程序已全部重写为 Web 应用）
├── web/             合并应用（Node 服务，5173）：4 个模块共 43 个页面
├── study-hub/       学习中枢（Node，5174，独立进程）
└── wiki-rag/        Wiki 检索（Python FastAPI，8000，独立进程）
````

再在「子项目规则」小节里，把 `### 前端子项目（apps/）` 整节替换为：

````markdown
### 前端子项目（apps/）

前端已从微信小程序重写为**本机 Web 应用**（2026-11-26 完成合并）。

`apps/web/` 是合并应用：组合根 + 共用层 + 4 个模块前端（43 个页面）。
零 npm 依赖、零构建；浏览器侧用 `web/cjs-loader.js` 加载 CommonJS 模块。

`apps/study-hub/` 与 `apps/wiki-rag/` 是**独立进程**（5174 / 8000），原地保留。

```bash
node apps/web/server/main.js
cd apps/web && node tests/run-all.js
```

详见 [`apps/README.md`](apps/README.md)。
````

- [ ] **Step 4: 检查全仓还有没有指向已删目录的引用**

```bash
grep -rn "apps/knowledge-hub\|apps/games\|apps/programmer-workbench\|apps/trip-log" \
  --include="*.md" --include="*.js" --include="*.json" --include="*.yml" . \
  | grep -v "^./docs/superpowers/" \
  | grep -v "^./archive/"
```

Expected: **无输出**（`docs/superpowers/` 下的历史计划与本文件除外 —— 那是历史记录，不改）

若有输出，逐个判断：是真的还指向旧代码（要改），还是只是在描述历史（可保留但要确认）。

- [ ] **Step 5: 提交**

```bash
git add apps/README.md AGENTS.md CLAUDE.md
git commit -m "docs(apps): 更新为合并后的应用架构说明

apps/README.md 全面重写：合并应用（5173）的四个模块、数据存储位置、
目录结构与测试命令；study-hub（5174）与 wiki-rag（8000）作为独立进程说明。

AGENTS.md 的「前端说明」与 CLAUDE.md 的「项目布局」「子项目规则」同步更新，
删除所有指向已删小程序目录的表述。"
git push origin main
```

---

## Task 5：端到端验收

**Files:**
- 无（只验证）

**Interfaces:**
- Consumes: 全部五个计划
- Produces: 合并完成的验收记录

- [ ] **Step 1: 三个服务都能起来**

```bash
# 终端 1
node apps/web/server/main.js
# 终端 2
PORT=5174 node apps/study-hub/server/main.js
# 终端 3（apps/wiki-rag 下）
python -m server.main
```

三个都不报「端口被占用」。

- [ ] **Step 2: 合并应用的首页显示两个后端「运行中」**

浏览器打开 `http://127.0.0.1:5173/`：
- 四个模块卡片可点，进对应模块
- 「学习中枢」与「Wiki 检索」显示**运行中**

- [ ] **Step 3: 停掉一个后端，确认状态变成「未启动」**

Ctrl+C 停掉 wiki-rag，刷新首页：
- 「Wiki 检索」变成**未启动**
- 鼠标悬停显示启动命令
- **其余模块完全不受影响**（首页、workbench、knowledge、trip、games 都正常）

再把 wiki-rag 起回来，状态恢复「运行中」。

- [ ] **Step 4: 全量测试 + 两个后端的测试**

```bash
cd apps/web && node tests/run-all.js
cd apps/study-hub && node tests/run-all.js
cd apps/wiki-rag && python -m pytest tests -q
```
Expected: 三处全绿

- [ ] **Step 5: 检查工作区干净**

```bash
git status
```
Expected: 只有 4 个旧目录的删除 + 文档改动，没有游离文件

> 特别检查 `apps/trip-log/node_modules` 有没有被误提交 —— `.gitignore` 里应该忽略了它。

- [ ] **Step 6: 提交（如有遗留改动）**

```bash
git add -A
git commit -m "chore(apps): 合并收尾"   # 若前面各步已提交完，这步可跳过
git push origin main
```

---

## 验收标准（整个合并完成时）

1. `cd apps/web && node tests/run-all.js` —— **全绿**，含 **562 个迁入用例**
2. `cd apps/study-hub && node tests/run-all.js` —— **全绿**（原地未受影响）
3. `cd apps/wiki-rag && python -m pytest tests -q` —— **全绿**（原地未受影响）
4. `apps/` 下只剩 `web/` `study-hub/` `wiki-rag/` `README.md` —— **4 个旧小程序目录已删除**
5. 浏览器打开 `http://127.0.0.1:5173/`：
   - 四个模块卡片可点，43 个页面都能打开、能交互
   - 知识图谱与技术雷达的 Canvas **有内容**
   - 贪吃蛇的 Canvas **有内容**，方向键能控制
   - trip 的「新增 → 存照片 → 详情页看到照片 → 删除」闭环可用
6. study-hub 停掉时，合并应用**其余部分完全正常**，首页显示「未启动」+ 中文启动命令
7. `apps/README.md` `AGENTS.md` `CLAUDE.md` 与新架构一致，**全仓无指向已删目录的引用**
8. `git status` 干净，`engine/` **未被改动**

## 合并前后的对照

| | 合并前 | 合并后 |
|---|---|---|
| 前端入口 | 4 个微信小程序（微信开发者工具逐个导入） | 1 个 Web 应用（浏览器打开 5173） |
| 页面数 | 43 | 43（同一批） |
| 前端运行时 | 小程序框架 | 原生 ES2020 + DOM + Canvas |
| 构建步骤 | 无 | 无 |
| npm 依赖 | trip-log 要装腾讯云 SDK | **零** |
| 数据位置 | 各小程序独立的 wx.storage | localStorage（`db:` / `games_` 前缀）+ IndexedDB（trip 照片） |
| 云端能力 | trip-log 的腾讯云（本就未激活） | **无**（用 JSON 导出导入补偿） |
| 后端 | 无 | study-hub(5174) + wiki-rag(8000)，独立进程 |
| 测试 | 小程序 4 套（562 个用例） | 合并为 1 套，`node tests/run-all.js` |
