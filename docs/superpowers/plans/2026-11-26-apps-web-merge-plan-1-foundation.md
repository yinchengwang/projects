# apps/web 合并 · 计划一：地基（骨架 + 共享层 + 平台适配层）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `apps/web/` 搭起单一 Node 服务的骨架，把 4 个小程序 `core/` 里逐字节相同的 10 个公共模块抽成一份 `shared/core/`，并实现承接 9 个 wx 交互 API 的 `shared/platform.js`。

**Architecture:** `apps/web/server/` 是组合根，只做路由分发与静态托管；`apps/web/shared/` 放浏览器与 Node 双端可用的共用层（UMD 模块，浏览器挂 `window.AlgoPlanShared`，Node 可 `require`）。存储抽象层 `shared/core/storage.js` 把 `wx.setStorageSync` 换成 `localStorage`，对外 9 个函数签名不变，因此上层 30+ 个调用方零改动。

**Tech Stack:** Node.js ≥ 24（内置 `node:test`、`node:sqlite`）、零 npm 依赖、零构建；浏览器端原生 ES2020 + DOM。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：`apps/web/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。**不用** 根目录 `.clang-format`（那是 C/C++ 的）。
- **零依赖**：不新增任何 npm 包，不引入前端框架，不引入打包工具。
- **中文文案**：面向用户的文案一律中文（沿用 `study-hub` 的 G4 约定）。
- **测试命令**：`cd apps/web && node tests/run-all.js`（自动发现 `tests/*.test.js`）。
- **单套件命令**：`cd apps/web && node tests/<name>.test.js`。
- **本机事实**（实测 2026-11-26，勿凭记忆改）：Node v24.17.0；Python 3.11.15；`apps/web` 目录当前不存在。
- **端口**：`apps/web` 服务监听 `127.0.0.1:5173`。`study-hub` 后续挂载时改用内部端口 `5174`（本计划不使用）。
- **不要动 `engine/`**：本计划与后续计划均不修改 `engine/` 任何文件。
- **不要动 `apps/study-hub/` 与 `apps/wiki-rag/`**：原地保留（路径有固定层级假设，见设计文档 §2.5）。
- **本计划不删任何旧小程序目录**：删除动作在计划五（步骤 10）。

## 已知隐患（留给计划二/三处理）

**`db:settings` 键会被两个模块抢用。** 实测：小程序版 `knowledge-hub/core/stores.js:39` 有一行 `Object.assign(storage.KEYS, KH_KEYS)`，在加载时**就地扩展**共享的 `storage.KEYS` 对象；而 workbench 与 knowledge-hub 的 `settings` 域默认值还不一样：

| 模块 | `primaryColor` 默认 | `profile.nickname` 默认 |
|---|---|---|
| programmer-workbench | `'sky'` | `'DevBencHer'` |
| knowledge-hub | `'violet'` | `'Yinch'` |

合并成一个应用后，两个模块若都用 `db:settings`，后加载的会覆盖先加载的。

**本计划的处理**：`shared/core/storage.js` 导出的 `KEYS` 是**同一个对象引用**（与小程序版行为一致，`Object.assign` 扩展对调用方可见），但 `KEYS.settings` 目前只有一个。`shared/core/with-theme.js` 读的是 `storage.KEYS.settings`。

**计划二/三必须处理**：给每个模块的 settings 一个独立键（如 `db:wb_settings` / `db:kh_settings`），或给 `storage.KEYS` 加命名空间前缀。本计划不预先决定方案，留给计划二/三在各自的 Task 里定。

**注意**：`with-theme.js` 读 `storage.KEYS.settings` 这一行在计划二/三 改键名时需要同步改。

## 依赖关系

本计划是后续四个计划的地基，四者的依赖关系：

```
计划一（地基）  ← 本文件
   ├── 计划二：workbench 数据层 + 视图层   （依赖 Task 1/2/3/4）
   ├── 计划三：knowledge-hub 数据层 + 视图层（依赖 Task 1/2/3/4）
   ├── 计划四：trip-log + games             （依赖 Task 1/2/3/4）
   └── 计划五：后端挂载 + 删除旧目录         （依赖计划二/三/四）
```

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/web/server/main.js` | 新建 | 入口：读 `.env`、起 HTTP 服务、监听 5173 |
| `apps/web/server/app.js` | 新建 | 组合根：建路由表、挂静态目录、错误兜底 |
| `apps/web/server/router.js` | 新建 | 极简路由：方法匹配 + 路径参数（零依赖，参照 `study-hub/server/router.js`） |
| `apps/web/server/static.js` | 新建 | 静态文件托管：路径解析 + 越界拒绝 + MIME |
| `apps/web/web/index.html` | 新建 | 统一外壳：模块导航 + 路由出口 |
| `apps/web/web/style.css` | 新建 | 全局样式 + 主题变量（`.theme-light` / `.theme-dark`） |
| `apps/web/shared/core/storage.js` | 新建 | 存储封装，底层换成 `localStorage`（对外 9 个函数签名不变） |
| `apps/web/shared/core/store.js` | 新建 | 数据域工厂（从 `knowledge-hub/core/store.js` 原样搬） |
| `apps/web/shared/core/id.js` | 新建 | 轻量唯一 ID（原样搬） |
| `apps/web/shared/core/date.js` | 新建 | 日期工具（原样搬） |
| `apps/web/shared/core/format.js` | 新建 | 格式化工具（原样搬） |
| `apps/web/shared/core/md.js` | 新建 | Markdown 渲染（原样搬） |
| `apps/web/shared/core/md2html.js` | 新建 | Markdown → HTML（原样搬） |
| `apps/web/shared/core/constants.js` | 新建 | 分类/优先级/状态常量（原样搬） |
| `apps/web/shared/core/theme.js` | 新建 | 主题解析（改：wx 导航栏同步 → CSS 变量） |
| `apps/web/shared/core/with-theme.js` | 新建 | 页面主题包装器（改：注入方式换成 Web） |
| `apps/web/shared/ui.js` | 新建 | 构件层（从 `study-hub/web/ui.js` 搬，UMD） |
| `apps/web/shared/charts.js` | 新建 | 手绘 SVG 图表层（从 `study-hub/web/charts.js` 搬，UMD） |
| `apps/web/shared/platform.js` | 新建 | 平台适配层：9 个 wx 交互 API 的 Web 实现 |
| `apps/web/tests/run-all.js` | 新建 | 一键跑全部测试 |
| `apps/web/tests/dom-stub.js` | 新建 | Node 下的最小 DOM stub（供 platform/ui 测试用） |
| `apps/web/tests/storage.test.js` | 新建 | storage 适配层测试 |
| `apps/web/tests/store.test.js` | 新建 | 数据域工厂测试 |
| `apps/web/tests/platform.test.js` | 新建 | 平台适配层测试 |
| `apps/web/tests/server.test.js` | 新建 | 组合根路由与静态托管测试 |

**搬 vs 改的原则**：能从 `apps/knowledge-hub/core/` **原样搬**的就原样搬（10 个公共文件里 8 个是零改动的），只有 `storage.js`（换底层）、`theme.js`（wx 导航栏 → CSS 变量）、`with-theme.js`（注入方式）需要改。搬过来的文件只改 `require` 路径，不改逻辑。

**为什么 `shared/` 放在 `apps/web/` 而不是 `apps/shared/`**：外壳、路由、静态资源都在 `apps/web/`，共用层与它们同生命周期、同部署单元，放一起便于整体删除或整体搬迁。`apps/study-hub/` 与 `apps/wiki-rag/` 原地不动。

---

## Task 1：`apps/web/server/` 骨架

**Files:**
- Create: `apps/web/server/router.js`
- Create: `apps/web/server/static.js`
- Create: `apps/web/server/app.js`
- Create: `apps/web/server/main.js`
- Create: `apps/web/web/index.html`
- Create: `apps/web/web/style.css`
- Create: `apps/web/tests/server.test.js`
- Create: `apps/web/tests/run-all.js`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces:
  - `server/router.js` 导出 `createRouter()`，返回 `{ add(method, pattern, handler), find(method, urlPath), routes }`。`find` 返回 `{route, params}` / `{methodNotAllowed, allowed}` / `null`。
  - `server/static.js` 导出 `createStaticHandler(opts)`，opts：`{ webRoot }`，返回 `(req, res) => boolean`（命中并已响应返回 `true`）。
  - `server/app.js` 导出 `createApp(opts)`，opts：`{ routes?, webRoot? }`，返回 `{ server, listen(port), close() }`。
  - `server/main.js` 导出 `main()`、`loadDotEnv(file?)`，直接 `node server/main.js` 可启动。
  - 静态根目录常量 `WEB_DIR` = `apps/web/web`。

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/server.test.js`：

```javascript
"use strict"
/**
 * server.test.js —— 组合根的路由分发与静态托管
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")
const path = require("node:path")
const fs = require("node:fs")
const os = require("node:os")

const { createApp } = require("../server/app.js")

/** 起一个临时静态目录，避免往源码树写文件 */
function makeTempWebRoot () {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "appsweb-test-"))
  fs.writeFileSync(path.join(dir, "index.html"), "<!doctype html><title>外壳</title>")
  fs.mkdirSync(path.join(dir, "shared"))
  fs.writeFileSync(path.join(dir, "shared", "a.js"), "window.__A = 1")
  return dir
}

/** 请求一次并返回 {status, body} */
function request (server, urlPath) {
  return new Promise(function (resolve, reject) {
    const addr = server.address()
    const req = http.request(
      { host: "127.0.0.1", port: addr.port, path: urlPath, method: "GET" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
}

test("组合根：命中路由时走 handler", async function (t) {
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/ping", handler: function (req, res) {
      res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" })
      res.end(JSON.stringify({ ok: true, pong: true }))
    } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/api/ping")
  assert.strictEqual(r.status, 200)
  assert.deepStrictEqual(JSON.parse(r.body), { ok: true, pong: true })
})

test("组合根：路径参数能被解析出来", async function (t) {
  let got = null
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/items/:id", handler: function (req, res, params) {
      got = params.id
      res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" })
      res.end("{}")
    } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  await request(app.server, "/api/items/abc123")
  assert.strictEqual(got, "abc123")
})

test("组合根：路径不存在时返回 404 中文提示", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/api/nothing-here")
  assert.strictEqual(r.status, 404)
  assert.ok(r.body.indexOf("未找到") >= 0, "响应体应含中文提示，实际是：" + r.body)
})

test("组合根：静态文件能被托管，根路径回落到 index.html", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("外壳") >= 0)

  const r2 = await request(app.server, "/shared/a.js")
  assert.strictEqual(r2.status, 200)
  assert.ok(r2.body.indexOf("__A") >= 0)
})

test("组合根：静态路径越界被拒绝", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/../package.json")
  assert.ok(r.status === 403 || r.status === 404, "越界请求应被拒绝，实际状态码：" + r.status)
})

test("组合根：同一路径方法不匹配时返回 405 并带 Allow 头", async function (t) {
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/only-get", handler: function (req, res) { res.end("{}") } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await new Promise(function (resolve, reject) {
    const addr = app.server.address()
    const req = http.request(
      { host: "127.0.0.1", port: addr.port, path: "/api/only-get", method: "POST" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, headers: res.headers, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
  assert.strictEqual(r.status, 405)
  assert.ok(r.headers.allow && r.headers.allow.indexOf("GET") >= 0, "405 响应必须带 Allow 头")
})
```

新建 `apps/web/tests/run-all.js`：

```javascript
"use strict"
/**
 * run-all.js —— 一键跑全部测试
 *
 * 用法：cd apps/web && node tests/run-all.js
 *
 * 自动发现 tests/*.test.js，新增测试套件只需放文件，无需手动登记。
 */

const fs = require("fs")
const path = require("path")
const { execFileSync } = require("child_process")

const ROOT = path.resolve(__dirname, "..")

const SUITES = []
fs.readdirSync(path.join(ROOT, "tests")).forEach(function (f) {
  if (/^.+\.test\.js$/.test(f)) {
    SUITES.push({ name: f.replace(/\.js$/, ""), file: "tests/" + f })
  }
})

let failed = 0
const summary = []

SUITES.forEach(function (suite) {
  process.stdout.write("\n\x1b[1m▶ " + suite.name + "\x1b[0m\n")
  try {
    const out = execFileSync(process.execPath, [path.join(ROOT, suite.file)], {
      cwd: ROOT,
      encoding: "utf8"
    })
    console.log(out.trim().split("\n").slice(-3).join("\n"))
    summary.push({ name: suite.name, ok: true })
  } catch (e) {
    console.log(((e.stdout || "") + (e.stderr || "")).trim().split("\n").slice(-16).join("\n"))
    summary.push({ name: suite.name, ok: false })
    failed += 1
  }
})

console.log("\n" + "=".repeat(52))
summary.forEach(function (s) {
  console.log((s.ok ? "\x1b[32m✓\x1b[0m " : "\x1b[31m✗\x1b[0m ") + s.name)
})

if (failed > 0) {
  console.log("\n\x1b[31m" + failed + " 组失败\x1b[0m")
  process.exit(1)
}
console.log("\n\x1b[32m全部 " + SUITES.length + " 组通过\x1b[0m")
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/server.test.js`
Expected: FAIL —— 报 `Cannot find module '../server/app.js'`

- [ ] **Step 3: 写 `server/router.js`**

新建 `apps/web/server/router.js`：

```javascript
"use strict"
/**
 * router.js —— 极简路由：方法匹配 + 路径参数
 *
 * 零依赖。路径参数用 :name 段式，例如 "/api/items/:id"。
 * 路由匹配只认路径段，不认正则通配。
 */

/** 带 HTTP 状态码的错误，供 handler 抛出 */
class HttpError extends Error {
  constructor (status, message) {
    super(message)
    this.status = status
  }
}

/**
 * 把 "/api/items/:id" 编成匹配函数。
 * @param {string} pattern
 * @returns {(urlPath: string) => (object|null)} 命中返回参数对象，不命中返回 null
 */
function compile (pattern) {
  const segs = pattern.split("/").filter(function (s) { return s !== "" })
  return function (urlPath) {
    const parts = urlPath.split("/").filter(function (s) { return s !== "" })
    if (parts.length !== segs.length) return null
    const params = {}
    for (let i = 0; i < segs.length; i += 1) {
      const seg = segs[i]
      const raw = parts[i]
      if (seg.charAt(0) === ":") {
        let value
        try {
          value = decodeURIComponent(raw)
        } catch (e) {
          return null
        }
        if (value === "") return null
        params[seg.slice(1)] = value
      } else if (seg !== raw) {
        return null
      }
    }
    return params
  }
}

/**
 * 创建路由表。
 * @returns {{add: Function, find: Function, routes: Array}}
 */
function createRouter () {
  const routes = []

  function add (method, pattern, handler) {
    routes.push({ method: method, pattern: pattern, match: compile(pattern), handler: handler })
  }

  /**
   * @returns {{route: object, params: object}|{methodNotAllowed: true, allowed: string[]}|null}
   */
  function find (method, urlPath) {
    let pathMatched = false
    const allowed = []
    for (const route of routes) {
      const params = route.match(urlPath)
      if (params === null) continue
      pathMatched = true
      if (route.method === method) return { route: route, params: params }
      if (allowed.indexOf(route.method) < 0) allowed.push(route.method)
    }
    if (pathMatched) return { methodNotAllowed: true, allowed: allowed }
    return null
  }

  return { add: add, find: find, routes: routes }
}

module.exports = { HttpError: HttpError, compile: compile, createRouter: createRouter }
```

- [ ] **Step 4: 写 `server/static.js`**

新建 `apps/web/server/static.js`：

```javascript
"use strict"
/**
 * static.js —— 静态文件托管
 *
 * 只服务 webRoot 内的文件。路径越界（../）一律拒绝。
 */

const fs = require("fs")
const path = require("path")

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
  ".woff2": "font/woff2",
  ".md": "text/markdown; charset=utf-8"
}

/**
 * 把 URL 路径映射到 webRoot 内的文件；越界或编码非法一律拒绝。
 * @param {string} urlPath 已经切掉查询串的路径
 * @param {string} webRoot 静态根目录
 * @returns {string|null} 绝对路径，不合法返回 null
 */
function resolveStatic (urlPath, webRoot) {
  const base = path.resolve(webRoot)
  let clean
  try {
    clean = decodeURIComponent(urlPath)
  } catch (e) {
    return null
  }
  const rel = clean === "/" ? "index.html" : clean.replace(/^\/+/, "")
  const abs = path.resolve(base, rel)
  if (abs !== base && abs.indexOf(base + path.sep) !== 0) return null
  return abs
}

/**
 * 创建静态文件处理器。
 * @param {{webRoot: string}} opts
 * @returns {(req: object, res: object) => boolean} 命中并已响应返回 true
 */
function createStaticHandler (opts) {
  const options = opts || {}
  const webRoot = options.webRoot

  return function serveStatic (req, res) {
    if (req.method !== "GET" && req.method !== "HEAD") return false
    const rawUrl = req.url || "/"
    const rawPath = rawUrl.split("?")[0]
    const abs = resolveStatic(rawPath, webRoot)
    if (abs === null) {
      res.writeHead(403, { "Content-Type": "text/plain; charset=utf-8" })
      res.end("禁止访问")
      return true
    }
    let stat
    try {
      stat = fs.statSync(abs)
    } catch (e) {
      return false
    }
    if (stat.isDirectory()) return false
    const ext = path.extname(abs).toLowerCase()
    const headers = {
      "Content-Type": MIME[ext] || "application/octet-stream",
      "Content-Length": String(stat.size)
    }
    res.writeHead(200, headers)
    if (req.method === "HEAD") {
      res.end()
      return true
    }
    res.end(fs.readFileSync(abs))
    return true
  }
}

module.exports = { MIME: MIME, resolveStatic: resolveStatic, createStaticHandler: createStaticHandler }
```

- [ ] **Step 5: 写 `server/app.js`**

新建 `apps/web/server/app.js`：

```javascript
"use strict"
/**
 * app.js —— 组合根
 *
 * 只做三件事：路由分发、静态托管、错误兜底。
 * 各业务模块（study-hub、wiki-rag 反代、各前端模块）由后续计划挂进来。
 */

const http = require("node:http")
const path = require("node:path")

const { createRouter, HttpError } = require("./router.js")
const { createStaticHandler } = require("./static.js")

const ROOT = path.resolve(__dirname, "..")
const WEB_DIR = path.join(ROOT, "web")

/** 发 JSON 响应 */
function sendJson (res, status, body, extraHeaders) {
  const text = JSON.stringify(body)
  const headers = {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(text)
  }
  if (extraHeaders) {
    Object.keys(extraHeaders).forEach(function (k) { headers[k] = extraHeaders[k] })
  }
  res.writeHead(status, headers)
  res.end(text)
}

/**
 * 创建 HTTP 服务。
 * @param {{routes?: Array, webRoot?: string}} opts
 * @returns {{server: object, listen: Function, close: Function}}
 */
function createApp (opts) {
  const options = opts || {}
  const router = createRouter()
  const webRoot = options.webRoot || WEB_DIR
  const routes = options.routes || []
  routes.forEach(function (route) {
    router.add(route.method, route.pattern, route.handler)
  })
  const serveStatic = createStaticHandler({ webRoot: webRoot })

  const server = http.createServer(function (req, res) {
    const rawUrl = req.url || "/"
    const rawPath = rawUrl.split("?")[0]
    const query = new URLSearchParams(rawUrl.indexOf("?") === -1 ? "" : rawUrl.slice(rawUrl.indexOf("?") + 1))
    const found = router.find(req.method, rawPath)

    if (found && found.route) {
      Promise.resolve()
        .then(function () {
          return found.route.handler(req, res, found.params, query)
        })
        .catch(function (err) {
          if (res.headersSent || res.writableEnded || res.destroyed) {
            console.error("[apps/web] 接口 " + req.method + " " + rawPath + " 处理异常（响应已发出）：", err)
            if (!res.writableEnded && !res.destroyed) res.end()
            return
          }
          const isHttpError = err instanceof HttpError
          const status = isHttpError && err.status >= 400 && err.status <= 599 ? err.status : 500
          if (!isHttpError) {
            console.error("[apps/web] 接口 " + req.method + " " + rawPath + " 处理异常：", err)
          }
          sendJson(res, status, { error: isHttpError ? err.message : "服务端出错" })
        })
      return
    }

    if (found && found.methodNotAllowed) {
      const allow = (found.allowed && found.allowed.length) ? found.allowed.join(", ") : "GET"
      sendJson(res, 405, { error: "方法不允许", method: req.method }, { Allow: allow })
      return
    }

    if (serveStatic(req, res)) return

    sendJson(res, 404, { error: "未找到该路径：" + rawPath })
  })

  return {
    server: server,
    listen: function (port) {
      return new Promise(function (resolve, reject) {
        server.listen(port, "127.0.0.1", function () { resolve(server.address().port) })
        server.on("error", reject)
      })
    },
    close: function () {
      return new Promise(function (resolve) { server.close(function () { resolve() }) })
    }
  }
}

module.exports = { ROOT: ROOT, WEB_DIR: WEB_DIR, createApp: createApp, sendJson: sendJson }
```

- [ ] **Step 6: 写 `server/main.js`**

新建 `apps/web/server/main.js`：

```javascript
"use strict"
/**
 * main.js —— 命令行入口
 *
 * 用法：node apps/web/server/main.js
 * 环境变量：PORT（默认 5173）
 */

const fs = require("node:fs")
const path = require("node:path")

const { createApp } = require("./app.js")

/**
 * 读取 .env 文件，把键塞进 process.env。已存在的环境变量优先。
 * @param {string} [file] 默认读 apps/web/.env
 * @returns {object} 读到的键值对
 */
function loadDotEnv (file) {
  const target = file || path.join(__dirname, "..", ".env")
  let text
  try {
    text = fs.readFileSync(target, "utf8")
  } catch (err) {
    return {}
  }
  const loaded = {}
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim()
    if (line === "" || line.charAt(0) === "#") continue
    const eq = line.indexOf("=")
    if (eq === -1) continue
    const key = line.slice(0, eq).trim()
    if (key === "") continue
    let value = line.slice(eq + 1).trim()
    if (value.length >= 2 && value.charAt(0) === '"' && value.charAt(value.length - 1) === '"') {
      value = value.slice(1, -1)
    }
    loaded[key] = value
    if (process.env[key] === undefined) process.env[key] = value
  }
  return loaded
}

function main () {
  loadDotEnv()
  const port = Number(process.env.PORT || 5173)
  const app = createApp()
  app.listen(port).then(function (p) {
    console.log("应用已启动：http://127.0.0.1:" + p)
  }).catch(function (err) {
    const code = err && err.code ? err.code : "未知错误"
    if (code === "EADDRINUSE") {
      console.error("启动失败：端口 " + port + " 已被占用（EADDRINUSE），请换一个端口")
    } else {
      console.error("启动失败：无法监听端口 " + port + "（" + code + "）")
    }
    process.exit(1)
  })
}

if (require.main === module) {
  main()
}

module.exports = { main: main, loadDotEnv: loadDotEnv }
```

- [ ] **Step 7: 写 `web/index.html` 与 `web/style.css`**

新建 `apps/web/web/index.html`：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>工作台</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body class="theme-light">
  <div class="app-shell">
    <header class="app-head">
      <h1 class="app-title">工作台</h1>
      <nav class="app-nav" id="app-nav">
        <a class="nav-item" href="/workbench/">程序员工作台</a>
        <a class="nav-item" href="/knowledge/">知识中心</a>
        <a class="nav-item" href="/trip/">足迹</a>
        <a class="nav-item" href="/games/">游戏</a>
        <a class="nav-item" href="/study/">学习中枢</a>
        <a class="nav-item" href="/wiki/">Wiki 检索</a>
      </nav>
    </header>
    <main class="app-main" id="app-main">
      <section class="panel hero">
        <h2>选择模块</h2>
        <p class="reason">各模块独立加载，互不影响。</p>
      </section>
    </main>
  </div>
</body>
</html>
```

新建 `apps/web/web/style.css`：

```css
/* style.css —— 全局样式与主题变量
 *
 * 主题靠 class 承载变量（不用 [data-theme]，兼容性更好）：
 *   .theme-light { --bg-primary: #F7F8FA; ... }
 *   .theme-dark  { --bg-primary: #0B0F17; ... }
 * 小程序的 WXSS 变量名保持不变，迁过来的样式可直接用。
 */

:root {
  --brand: #0284C7;
  --brand-soft: rgba(2, 132, 199, 0.12);
}

.theme-light {
  --bg-primary: #F7F8FA;
  --bg-surface: #FFFFFF;
  --bg-subtle: #F1F3F5;
  --text-primary: #0F172A;
  --text-secondary: #475569;
  --text-muted: #94A3B8;
  --border: rgba(15, 23, 42, 0.08);
  --border-strong: rgba(15, 23, 42, 0.16);
  --danger: #E5484D;
  --ok: #30A46C;
  --warn: #F5A524;
}

.theme-dark {
  --bg-primary: #0B0F17;
  --bg-surface: #151A23;
  --bg-subtle: #1D2430;
  --text-primary: #E6EAF0;
  --text-secondary: #A8B3C4;
  --text-muted: #64748B;
  --border: rgba(230, 234, 240, 0.10);
  --border-strong: rgba(230, 234, 240, 0.20);
  --danger: #E5484D;
  --ok: #30A46C;
  --warn: #F5A524;
}

* {
  box-sizing: border-box;
}

html,
body {
  margin: 0;
  padding: 0;
}

body {
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
    "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
  font-size: 14px;
  line-height: 1.6;
}

.app-shell {
  max-width: 1080px;
  margin: 0 auto;
  padding: 0 24px 64px;
}

.app-head {
  display: flex;
  align-items: baseline;
  gap: 24px;
  flex-wrap: wrap;
  padding: 24px 0 16px;
  border-bottom: 0.5px solid var(--border);
}

.app-title {
  margin: 0;
  font-size: 20px;
  font-weight: 600;
}

.app-nav {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}

.nav-item {
  color: var(--text-secondary);
  text-decoration: none;
  font-size: 14px;
  padding: 4px 0;
  border-bottom: 2px solid transparent;
}

.nav-item:hover {
  color: var(--text-primary);
}

.nav-item.active {
  color: var(--brand);
  border-bottom-color: var(--brand);
}

.app-main {
  padding-top: 24px;
}

/* 页头：眉标 → 标题 → 副标 */
.page-head {
  margin-bottom: 20px;
}

.eyebrow {
  display: block;
  font-size: 12px;
  color: var(--text-muted);
  letter-spacing: 0.04em;
}

.page-title {
  margin: 4px 0;
  font-size: 22px;
  font-weight: 600;
}

.page-sub {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
}

/* 卡 */
.panel {
  background: var(--bg-surface);
  border: 0.5px solid var(--border);
  border-radius: 12px;
  padding: 16px;
  margin-bottom: 16px;
}

.panel h2 {
  margin: 0 0 8px;
  font-size: 16px;
  font-weight: 600;
}

.panel.hero {
  border-color: var(--border-strong);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04);
}

.reason {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
}

/* 卡内嵌套块 */
.block {
  background: var(--bg-subtle);
  border-radius: 8px;
  padding: 12px;
  margin-top: 12px;
}

.block-title {
  margin: 0 0 6px;
  font-size: 13px;
  font-weight: 600;
  color: var(--text-secondary);
}

/* 数字块阵列 */
.stat-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
  gap: 12px;
  margin-top: 12px;
}

.stat {
  background: var(--bg-subtle);
  border-radius: 8px;
  padding: 12px;
  text-align: center;
}

.stat-value {
  display: block;
  font-size: 22px;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
}

.stat-label {
  display: block;
  font-size: 12px;
  color: var(--text-secondary);
  margin-top: 2px;
}

.stat-hint {
  display: block;
  font-size: 11px;
  color: var(--text-muted);
  margin-top: 2px;
}

/* 药丸：状态用颜色 + 文字双承载 */
.pill {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 999px;
  background: var(--bg-subtle);
  color: var(--text-secondary);
  font-size: 12px;
  line-height: 18px;
}

.pill.ok {
  background: rgba(48, 164, 108, 0.12);
  color: var(--ok);
}

.pill.bad {
  background: rgba(229, 72, 77, 0.12);
  color: var(--danger);
}

.pill.warn {
  background: rgba(245, 165, 36, 0.14);
  color: var(--warn);
}

.pill.accent {
  background: var(--brand-soft);
  color: var(--brand);
}

/* 键值行 */
.kv > div {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  padding: 5px 0;
  border-bottom: 0.5px solid var(--border);
  font-size: 13px;
}

.kv > div:last-child {
  border-bottom: none;
}

.kv > div > span:first-child {
  color: var(--text-secondary);
}

/* 空状态 */
.empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 48px 24px;
  color: var(--text-muted);
  text-align: center;
}

.empty strong {
  color: var(--text-secondary);
  font-size: 14px;
}

/* 平台层浮层：toast / modal / action sheet / loading */
.pl-toast-host {
  position: fixed;
  left: 0;
  right: 0;
  bottom: 40px;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  pointer-events: none;
  z-index: 9000;
}

.pl-toast {
  background: rgba(15, 23, 42, 0.88);
  color: #ffffff;
  padding: 8px 16px;
  border-radius: 8px;
  font-size: 13px;
  max-width: 80vw;
}

.pl-mask {
  position: fixed;
  inset: 0;
  background: rgba(15, 23, 42, 0.45);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9100;
}

.pl-dialog {
  background: var(--bg-surface);
  border-radius: 12px;
  padding: 20px;
  width: min(360px, 88vw);
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.24);
}

.pl-dialog-title {
  margin: 0 0 8px;
  font-size: 16px;
  font-weight: 600;
}

.pl-dialog-content {
  margin: 0 0 16px;
  color: var(--text-secondary);
  font-size: 14px;
  white-space: pre-wrap;
}

.pl-dialog-input {
  width: 100%;
  padding: 8px 10px;
  margin-bottom: 16px;
  border: 1px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
  font-family: inherit;
}

.pl-dialog-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.pl-btn {
  padding: 7px 16px;
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 13px;
  cursor: pointer;
  font-family: inherit;
}

.pl-btn.primary {
  background: var(--brand);
  border-color: var(--brand);
  color: #ffffff;
}

.pl-btn.danger {
  background: var(--danger);
  border-color: var(--danger);
  color: #ffffff;
}

.pl-sheet-list {
  display: flex;
  flex-direction: column;
  gap: 2px;
  margin-bottom: 16px;
}

.pl-sheet-item {
  padding: 10px 12px;
  border-radius: 8px;
  cursor: pointer;
  font-size: 14px;
}

.pl-sheet-item:hover {
  background: var(--bg-subtle);
}

.pl-loading {
  background: var(--bg-surface);
  border-radius: 10px;
  padding: 18px 26px;
  font-size: 14px;
  color: var(--text-secondary);
}

/* 图片预览 */
.pl-lightbox {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.9);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9200;
  cursor: zoom-out;
}

.pl-lightbox img {
  max-width: 92vw;
  max-height: 92vh;
  object-fit: contain;
}
```

- [ ] **Step 8: 运行测试确认通过**

Run: `cd apps/web && node tests/server.test.js`
Expected: PASS —— 6 个用例全过（`#pass 6`）

- [ ] **Step 9: 提交**

```bash
git add apps/web
git commit -m "feat(web): 搭起 apps/web 组合根骨架

单一 Node 服务：路由分发 + 静态托管 + 中文错误兜底。
路由用零依赖实现，路径参数支持 :name 段式，405 响应带 Allow 头。
静态托管拒绝 ../ 越界。
统一外壳含六个模块导航入口与明暗两套主题变量。"
git push origin main
```

---

## Task 2：`shared/core/` 抽取（8 个零改动文件）

**Files:**
- Create: `apps/web/shared/core/id.js`
- Create: `apps/web/shared/core/date.js`
- Create: `apps/web/shared/core/format.js`
- Create: `apps/web/shared/core/md2html.js`
- Create: `apps/web/shared/core/md.js`
- Create: `apps/web/shared/core/constants.js`
- Create: `apps/web/shared/core/store.js`
- Create: `apps/web/tests/store.test.js`

**Interfaces:**
- Consumes: Task 1 的 `apps/web/` 目录结构
- Produces:
  - `shared/core/id.js` 导出 `genId()` → `string`
  - `shared/core/date.js` 导出 `todayStr()`、`formatRelative(ts)` 等（与源文件一致）
  - `shared/core/format.js` 导出现有格式化函数
  - `shared/core/md2html.js` 导出 `md2html(md)` → `string`
  - `shared/core/md.js` 导出 `render(md, isDark)`、`preview(md, len)` → `string`
  - `shared/core/constants.js` 导出 `TECH_CATEGORIES`、`PRIORITIES`、`techCategory(key)`、`priority(key)`、`PRIMARY_COLORS`、`primaryOf(key)` 等
  - `shared/core/store.js` 导出 `createListStore(options)`、`createObjectStore(options)`

- [ ] **Step 1: 确认源文件零 wx 依赖**

Run:
```bash
grep -n "wx\." apps/knowledge-hub/core/{id,date,format,md2html,md,constants,store}.js
```
Expected: **无输出**（这 7 个文件都不碰 wx，可原样搬）

- [ ] **Step 2: 搬文件并改 require 路径**

Run:
```bash
mkdir -p apps/web/shared/core apps/web/tests
cp apps/knowledge-hub/core/id.js       apps/web/shared/core/id.js
cp apps/knowledge-hub/core/date.js      apps/web/shared/core/date.js
cp apps/knowledge-hub/core/format.js    apps/web/shared/core/format.js
cp apps/knowledge-hub/core/md2html.js   apps/web/shared/core/md2html.js
cp apps/knowledge-hub/core/md.js        apps/web/shared/core/md.js
cp apps/knowledge-hub/core/constants.js apps/web/shared/core/constants.js
cp apps/knowledge-hub/core/store.js     apps/web/shared/core/store.js
```

这 7 个文件内部只用 `./xxx.js` 相对路径引用同目录文件，**require 路径无需改动**（`md.js` 依赖 `md2html.js`、`store.js` 依赖 `storage.js` 与 `id.js`）。

> `store.js` 依赖 `storage.js`——Task 3 才创建。因此 Task 2 结束时 `store.js` 的 `require('./storage.js')` 暂时指向不存在的文件。

- [ ] **Step 3: 写失败测试**

新建 `apps/web/tests/store.test.js`：

```javascript
"use strict"
/**
 * store.test.js —— 数据域工厂
 *
 * 注意：本文件先于 shared/core/storage.js 落地，故此处的 require 会失败，
 * Task 3 完成后本文件才可运行。这是预期的红→绿顺序。
 */

const test = require("node:test")
const assert = require("node:assert")

const store = require("../shared/core/store.js")
const storage = require("../shared/core/storage.js")

test.beforeEach(function () {
  storage.clearAll()
})

test("列表域：首次读取会写入种子数据", function () {
  const s = store.createListStore({ key: "test_seeded", seed: function () {
    return [{ id: "a", v: 1 }]
  } })
  assert.strictEqual(s.getAll().length, 1)
  // 落盘了
  assert.ok(storage.get("test_seeded", null) !== null)
})

test("列表域：add 生成 id 与 createdAt，并插到最前", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", v: 1 })
  s.add({ id: "x2", v: 2 })
  const all = s.getAll()
  assert.strictEqual(all[0].id, "x2", "新加的应排在最前")
  assert.ok(all[0].createdAt, "应自动补 createdAt")
})

test("列表域：update 按 id 局部更新，未命中返回 null", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", v: 1 })
  const updated = s.update("x1", { v: 9 })
  assert.strictEqual(updated.v, 9)
  assert.strictEqual(s.update("不存在", { v: 1 }), null)
})

test("列表域：remove 按 id 删除并返回是否真的删掉了", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1" })
  assert.strictEqual(s.remove("x1"), true)
  assert.strictEqual(s.remove("x1"), false, "重复删应返回 false")
})

test("列表域：find 与 count", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", done: false })
  s.add({ id: "x2", done: true })
  assert.strictEqual(s.find("x1").id, "x1")
  assert.strictEqual(s.find("没有"), null)
  assert.strictEqual(s.count(), 2)
  assert.strictEqual(s.count(function (i) { return i.done }), 1)
})

test("对象域：get 返回默认值与已存值的合并", function () {
  const s = store.createObjectStore({ key: "test_obj", defaultValue: { a: 1, b: 2 } })
  assert.deepStrictEqual(s.get(), { a: 1, b: 2 })
  s.patch({ b: 5 })
  assert.deepStrictEqual(s.get(), { a: 1, b: 5 })
})

test("对象域：reset 恢复默认", function () {
  const s = store.createObjectStore({ key: "test_obj", defaultValue: { a: 1 } })
  s.patch({ a: 9 })
  s.reset()
  assert.deepStrictEqual(s.get(), { a: 1 })
})
```

- [ ] **Step 4: 运行测试确认失败**

Run: `cd apps/web && node tests/store.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/core/storage.js'`

- [ ] **Step 5: 提交（文件已搬，测试为红）**

```bash
git add apps/web
git commit -m "refactor(web): 抽出 shared/core 的 7 个零改动公共模块

id/date/format/md2html/md/constants/store 这 7 个文件实测零 wx 依赖，
逻辑一字不改直接搬入 shared/core/。store.test.js 先落为红，
等 Task 3 的 storage.js 就位后转绿。"
git push origin main
```

---

## Task 3：`shared/core/storage.js` 适配到 `localStorage`

**Files:**
- Create: `apps/web/shared/core/storage.js`
- Create: `apps/web/tests/storage.test.js`
- Modify: `apps/web/tests/store.test.js`（无需改动，Task 2 已写好）

**Interfaces:**
- Consumes: Task 2 的 `shared/core/store.js`（它 `require('./storage.js')`）
- Produces: `shared/core/storage.js` 导出 `PREFIX`（`'db:'`）、`KEYS`（对象）、`get(key, defaultValue)`、`set(key, value)`、`remove(key)`、`clearAll()`、`exportAll()`、`importAll(json, mode)`、`usage()`。
  - **与小程序版的关键差异**：`get` 在键不存在时返回 `defaultValue`（小程序版对缺失键返回 `''` 再被 `get` 归一为 `defaultValue`，语义一致）；`usage()` 的 `currentSize` 用字符数估算（localStorage 无 `getStorageInfoSync`）。

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/storage.test.js`：

```javascript
"use strict"
/**
 * storage.test.js —— 存储适配层（localStorage 后端）
 */

const test = require("node:test")
const assert = require("node:assert")

/** Node 下没有 localStorage，用一个内存版替身 */
function makeLocalStorage () {
  const map = new Map()
  return {
    getItem: function (k) { return map.has(k) ? map.get(k) : null },
    setItem: function (k, v) { map.set(k, String(v)) },
    removeItem: function (k) { map.delete(k) },
    key: function (i) { return Array.from(map.keys())[i] === undefined ? null : Array.from(map.keys())[i] },
    get length () { return map.size },
    _map: map
  }
}

global.localStorage = makeLocalStorage()

const storage = require("../shared/core/storage.js")

test.beforeEach(function () {
  global.localStorage = makeLocalStorage()
  storage.clearAll()
})

test("键名统一带 db: 前缀", function () {
  storage.set("foo", { a: 1 })
  assert.ok(global.localStorage._map.has("db:foo"), "实际键名：" + Array.from(global.localStorage._map.keys()))
})

test("set/get 往返：对象与数组都正确", function () {
  storage.set("obj", { a: 1, b: [2, 3] })
  storage.set("arr", [1, 2, 3])
  assert.deepStrictEqual(storage.get("obj", null), { a: 1, b: [2, 3] })
  assert.deepStrictEqual(storage.get("arr", null), [1, 2, 3])
})

test("get 读不到时返回默认值", function () {
  assert.strictEqual(storage.get("根本没有", "兜底"), "兜底")
  assert.strictEqual(storage.get("根本没有", null), null)
})

test("set 写入的值是 JSON 字符串（可被其它标签页读到）", function () {
  storage.set("x", { a: 1 })
  assert.strictEqual(global.localStorage._map.get("db:x"), '{"a":1}')
})

test("remove 删除单个键", function () {
  storage.set("x", 1)
  assert.strictEqual(storage.remove("x"), true)
  assert.strictEqual(storage.get("x", null), null)
})

test("clearAll 只清 db: 前缀的键，不动别的", function () {
  global.localStorage.setItem("别人的键", "别删我")
  storage.set("mine", 1)
  storage.clearAll()
  assert.strictEqual(global.localStorage.getItem("别人的键"), "别删我")
  assert.strictEqual(storage.get("mine", null), null)
})

test("exportAll 导出为 JSON 字符串，且不含 seeded 标记", function () {
  storage.set("a", 1)
  storage.set("b", { c: 2 })
  const raw = storage.exportAll()
  const parsed = JSON.parse(raw)
  assert.strictEqual(parsed.version, 1)
  assert.deepStrictEqual(parsed.data.a, 1)
  assert.deepStrictEqual(parsed.data.b, { c: 2 })
  assert.ok(parsed.exportedAt, "应带 exportedAt")
})

test("importAll 以 merge 模式只填空缺键", function () {
  storage.set("a", "原有")
  const r = storage.importAll(JSON.stringify({ data: { a: "导入的", b: "导入的" } }), "merge")
  assert.strictEqual(r.ok, true)
  assert.strictEqual(r.count, 2)
  assert.strictEqual(storage.get("a", null), "原有", "merge 不该覆盖已有键")
  assert.strictEqual(storage.get("b", null), "导入的")
})

test("importAll 以 overwrite 模式全量覆盖", function () {
  storage.set("a", "原有")
  storage.importAll(JSON.stringify({ data: { a: "导入的" } }), "overwrite")
  assert.strictEqual(storage.get("a", null), "导入的")
})

test("importAll 遇到坏 JSON 返回中文错误而不是抛异常", function () {
  const r = storage.importAll("{ 这不是 JSON", "merge")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0)
})

test("importAll 缺少 data 字段时返回中文错误", function () {
  const r = storage.importAll(JSON.stringify({ nope: 1 }), "merge")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error.indexOf("data") >= 0, "错误信息应指出 data 字段，实际：" + r.error)
})

test("usage 统计键数", function () {
  storage.set("a", 1)
  storage.set("b", 2)
  const u = storage.usage()
  assert.ok(u.keys >= 2)
})

test("存储写满时 set 返回 false 而不是抛异常", function () {
  const original = global.localStorage.setItem
  global.localStorage.setItem = function () { throw new Error("QuotaExceededError") }
  const ok = storage.set("big", { x: 1 })
  global.localStorage.setItem = original
  assert.strictEqual(ok, false, "写失败应返回 false")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/storage.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/core/storage.js'`

- [ ] **Step 3: 写 `shared/core/storage.js`**

新建 `apps/web/shared/core/storage.js`：

```javascript
"use strict"
/**
 * storage.js —— 本地存储封装（localStorage 后端）
 *
 * 前缀 `db:`，与小程序版同键名，便于已有数据直接沿用。
 *
 * 与小程序版的差异只有一处：底层从 `wx.setStorageSync` 换成 `localStorage`。
 * 对外的 9 个函数签名与语义完全不变，因此上层 store/stores/seed 等
 * 30 余个调用方零改动。
 *
 * localStorage 只能存字符串，故一律 JSON.stringify / JSON.parse。
 * 键不存在时 getItem 返回 null（小程序版返回 ''），本模块统一归一为 null。
 */

/** 存储键前缀 */
const PREFIX = "db:"

/**
 * 数据域的存储键。
 *
 * 注意：knowledge-hub 会在加载时 Object.assign 扩展这个对象（加 daily、
 * excerpts 等 14 个键）。本模块导出的是**同一个对象引用**，扩展对调用方可见。
 */
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
  settings: "settings",
  seeded: "__seeded__"
}

function fullKey (key) {
  return PREFIX + key
}

/**
 * 读取
 * @param {string} key
 * @param {*} defaultValue 读不到时返回
 * @returns {*}
 */
function get (key, defaultValue) {
  try {
    const raw = localStorage.getItem(fullKey(key))
    if (raw === null || raw === undefined || raw === "") return defaultValue
    try {
      return JSON.parse(raw)
    } catch (e) {
      return defaultValue
    }
  } catch (e) {
    return defaultValue
  }
}

/**
 * 写入
 * @param {string} key
 * @param {*} value
 * @returns {boolean} 写成功返回 true，配额不足等失败返回 false
 */
function set (key, value) {
  try {
    localStorage.setItem(fullKey(key), JSON.stringify(value))
    return true
  } catch (e) {
    console.warn("[storage] 写入失败", key, e)
    return false
  }
}

/**
 * 删除单个键
 * @param {string} key
 * @returns {boolean}
 */
function remove (key) {
  try {
    localStorage.removeItem(fullKey(key))
    return true
  } catch (e) {
    return false
  }
}

/** 列出本应用的全部键（不含前缀） */
function keys () {
  const out = []
  try {
    for (let i = 0; i < localStorage.length; i += 1) {
      const k = localStorage.key(i)
      if (k !== null && k.indexOf(PREFIX) === 0) out.push(k.slice(PREFIX.length))
    }
  } catch (e) {
    console.warn("[storage] 列举键失败", e)
  }
  return out
}

/**
 * 清空本应用全部数据（只清 db: 前缀的键）
 * @returns {boolean}
 */
function clearAll () {
  try {
    keys().forEach(function (k) { localStorage.removeItem(fullKey(k)) })
    return true
  } catch (e) {
    console.warn("[storage] 清空失败", e)
    return false
  }
}

/**
 * 导出全部数据为 JSON 字符串
 * @returns {string} { exportedAt, version, data: { key: value } }
 */
function exportAll () {
  const data = {}
  try {
    keys().forEach(function (shortKey) {
      if (shortKey === KEYS.seeded) return
      const raw = localStorage.getItem(fullKey(shortKey))
      if (raw === null || raw === undefined || raw === "") return
      try {
        data[shortKey] = JSON.parse(raw)
      } catch (e) {
        data[shortKey] = raw
      }
    })
  } catch (e) {
    console.warn("[storage] 导出失败", e)
  }
  return JSON.stringify({
    exportedAt: new Date().toISOString(),
    version: 1,
    data: data
  }, null, 2)
}

/**
 * 从 JSON 导入
 * @param {string} json
 * @param {'merge'|'overwrite'} mode merge 只填空缺键，overwrite 全量覆盖
 * @returns {{ok:boolean, count:number, error?:string}}
 */
function importAll (json, mode) {
  const m = mode || "merge"
  try {
    const parsed = JSON.parse(json)
    const data = parsed && parsed.data
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return { ok: false, count: 0, error: "备份格式不正确：缺少 data 字段" }
    }
    let count = 0
    Object.keys(data).forEach(function (k) {
      if (m === "overwrite") {
        set(k, data[k])
      } else {
        const existing = get(k, null)
        if (existing === null) set(k, data[k])
      }
      count += 1
    })
    return { ok: true, count: count }
  } catch (err) {
    return { ok: false, count: 0, error: (err && err.message) || String(err) }
  }
}

/**
 * 统计占用。
 * localStorage 没有 getStorageInfoSync，容量按字符数估算（与小程序版的 KB 口径不同，
 * 但只用于展示，不参与任何判定）。
 * @returns {{keys:number, currentSize:number}}
 */
function usage () {
  try {
    const list = keys()
    let size = 0
    list.forEach(function (k) {
      const raw = localStorage.getItem(fullKey(k))
      if (raw !== null) size += raw.length
    })
    return { keys: list.length, currentSize: Math.ceil(size / 1024) }
  } catch (e) {
    return { keys: 0, currentSize: 0 }
  }
}

module.exports = {
  PREFIX: PREFIX,
  KEYS: KEYS,
  get: get,
  set: set,
  remove: remove,
  keys: keys,
  clearAll: clearAll,
  exportAll: exportAll,
  importAll: importAll,
  usage: usage
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cd apps/web && node tests/storage.test.js && node tests/store.test.js`
Expected: 两个都 PASS —— `storage.test.js` 12 个用例过，`store.test.js` 7 个用例过

> `store.test.js` 里的 `storage.clearAll()` 现在会真的清 localStorage，不再报错。

- [ ] **Step 5: 提交**

```bash
git add apps/web
git commit -m "feat(web): storage 适配到 localStorage

底层从 wx.setStorageSync 换成 localStorage，对外 9 个函数签名与语义不变，
上层 store/stores/seed 等调用方零改动。键名沿用 db: 前缀，
键不存在时统一归一为 null（小程序版返回空串）。
写配额不足时返回 false 而不是抛异常。"
git push origin main
```

---

## Task 4：`shared/ui.js` 与 `shared/charts.js`

**Files:**
- Create: `apps/web/shared/ui.js`
- Create: `apps/web/shared/charts.js`
- Create: `apps/web/tests/dom-stub.js`
- Create: `apps/web/tests/ui.test.js`

**Interfaces:**
- Consumes: Task 1 的 `apps/web/web/style.css`（构件类名必须与 CSS 对齐）
- Produces:
  - `shared/ui.js`（UMD）：浏览器挂 `window.AlgoPlanUI`，Node 可 `require`。导出 `el(tag, attrs, children)`、`pageHead(opts)`、`hero(opts)`、`panel(opts)`、`block(opts)`、`statGrid(items)`、`pill(text, tone)`、`kv(pairs)`、`empty(title, hint)`。
  - `shared/charts.js`（UMD）：导出 `lineChart(opts)`、`barChart(opts)`、`radarChart(opts)`、`heatmap(opts)` 等纯函数（返回 SVG 字符串）。
  - `shared/ui.js` 新增 `mount(root, page)`：把 `{ data, render(state) }` 页面挂到容器上。

- [ ] **Step 1: 写失败测试与 DOM stub**

新建 `apps/web/tests/dom-stub.js`：

```javascript
"use strict"
/**
 * dom-stub.js —— Node 下的最小 DOM 替身
 *
 * 只实现构件层用到的 API：createElement / createTextNode / appendChild /
 * setAttribute / addEventListener / textContent / className / innerHTML。
 * 够用来断言渲染结构，不做完整 DOM 模拟。
 */

/**
 * 安装 DOM 替身到 global。
 * @returns {object} { nodes: 创建过的元素数组 }
 */
function install () {
  const nodes = []

  function makeNode (tag) {
    const node = {
      tagName: String(tag).toUpperCase(),
      children: [],
      attributes: {},
      listeners: {},
      _text: "",
      get className () { return this.attributes.class || "" },
      set className (v) { this.attributes.class = v },
      get textContent () {
        if (this.children.length === 0) return this._text
        return this.children.map(function (c) { return c.textContent }).join("")
      },
      set textContent (v) {
        this.children = []
        this._text = String(v)
      },
      appendChild: function (child) {
        this.children.push(child)
        return child
      },
      setAttribute: function (k, v) { this.attributes[k] = String(v) },
      getAttribute: function (k) {
        return Object.prototype.hasOwnProperty.call(this.attributes, k) ? this.attributes[k] : null
      },
      addEventListener: function (type, fn) {
        if (!this.listeners[type]) this.listeners[type] = []
        this.listeners[type].push(fn)
      },
      /** 触发已注册的事件监听器 */
      fire: function (type, event) {
        const list = this.listeners[type] || []
        list.forEach(function (fn) { fn(event || {}) })
      }
    }
    nodes.push(node)
    return node
  }

  global.document = {
    createElement: makeNode,
    createTextNode: function (text) {
      return { tagName: "#text", children: [], _text: String(text), textContent: String(text) }
    },
    body: makeNode("body"),
    documentElement: makeNode("html")
  }

  return { nodes: nodes }
}

module.exports = { install: install }
```

新建 `apps/web/tests/ui.test.js`：

```javascript
"use strict"
/**
 * ui.test.js —— 构件层
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const ui = require("../shared/ui.js")

test("el：设置 class 与 text", function () {
  const node = ui.el("div", { class: "panel", text: "内容" })
  assert.strictEqual(node.tagName, "DIV")
  assert.strictEqual(node.className, "panel")
  assert.strictEqual(node.textContent, "内容")
})

test("el：on 前缀的属性注册为事件监听器", function () {
  let hit = 0
  const node = ui.el("button", { onclick: function () { hit += 1 } })
  node.fire("click")
  assert.strictEqual(hit, 1)
})

test("el：children 按顺序追加", function () {
  const node = ui.el("div", {}, [ui.el("span", { text: "A" }), ui.el("span", { text: "B" })])
  assert.strictEqual(node.children.length, 2)
  assert.strictEqual(node.textContent, "AB")
})

test("pageHead：眉标 → 标题 → 副标的顺序", function () {
  const node = ui.pageHead({ eyebrow: "眉标", title: "标题", sub: "副标" })
  assert.strictEqual(node.className, "page-head")
  assert.strictEqual(node.children.length, 3)
  assert.strictEqual(node.children[0].textContent, "眉标")
  assert.strictEqual(node.children[1].textContent, "标题")
  assert.strictEqual(node.children[2].textContent, "副标")
})

test("hero：主卡带 hero 类名", function () {
  const node = ui.hero({ title: "现在该做什么", sub: "下一步" })
  assert.strictEqual(node.className, "panel hero")
})

test("panel：可带自定义类名", function () {
  assert.strictEqual(ui.panel({ title: "T" }).className, "panel")
  assert.strictEqual(ui.panel({ title: "T", className: "quiet" }).className, "panel quiet")
})

test("statGrid：数字块的数量与内容", function () {
  const node = ui.statGrid([
    { label: "今天做了", value: 2, hint: "道题" },
    { label: "复习", value: 1 }
  ])
  assert.strictEqual(node.className, "stat-grid")
  assert.strictEqual(node.children.length, 2)
  assert.ok(node.textContent.indexOf("今天做了") >= 0)
  assert.ok(node.textContent.indexOf("2") >= 0)
})

test("pill：状态用颜色 + 文字双承载", function () {
  const node = ui.pill("已完成", "ok")
  assert.strictEqual(node.className, "pill ok")
  assert.strictEqual(node.textContent, "已完成")
})

test("kv：每行一个事实", function () {
  const node = ui.kv([["键", "值"], ["键2", 42]])
  assert.strictEqual(node.className, "kv")
  assert.strictEqual(node.children.length, 2)
  assert.ok(node.textContent.indexOf("键2") >= 0)
  assert.ok(node.textContent.indexOf("42") >= 0)
})

test("empty：说清「没有」以及「为什么」", function () {
  const node = ui.empty("还没有记录", "点上面的按钮新建一条")
  assert.strictEqual(node.className, "empty")
  assert.ok(node.textContent.indexOf("还没有记录") >= 0)
  assert.ok(node.textContent.indexOf("点上面的按钮新建一条") >= 0)
})

test("mount：把页面的 render 结果挂到容器上", function () {
  const container = { children: [], appendChild: function (c) { this.children.push(c) } }
  const page = {
    data: { name: "甲" },
    render: function (state) { return ui.panel({ title: state.name }) }
  }
  ui.mount(container, page)
  assert.strictEqual(container.children.length, 1)
  assert.ok(container.children[0].textContent.indexOf("甲") >= 0)
})

test("mount：state 变化后重新渲染", function () {
  const container = { children: [], appendChild: function (c) { this.children.push(c) } }
  const page = {
    data: { name: "甲" },
    render: function (state) { return ui.panel({ title: state.name }) }
  }
  ui.mount(container, page)
  page.state.name = "乙"
  ui.rerender(container, page)
  assert.ok(container.children[0].textContent.indexOf("乙") >= 0)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/ui.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/ui.js'`

- [ ] **Step 3: 写 `shared/ui.js`**

新建 `apps/web/shared/ui.js`：

```javascript
"use strict"
/**
 * ui.js —— 公共界面构件
 *
 * 零依赖：在浏览器里挂到 window.AlgoPlanUI，在 Node 里可以 require 取纯构造器。
 *
 * 类名必须与 web/style.css 对齐，改这里就要同步改那里。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanUI = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /**
   * 极简 DOM 构造器。
   * @param {string} tag
   * @param {object} [attrs] 支持 class / text / on* 事件 / 其余走 setAttribute
   * @param {Array} [children]
   * @returns {object} DOM 元素
   */
  function el (tag, attrs, children) {
    const node = document.createElement(tag)
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        if (key === "class") node.className = attrs[key]
        else if (key === "text") node.textContent = attrs[key]
        else if (key.indexOf("on") === 0 && typeof attrs[key] === "function") {
          node.addEventListener(key.slice(2).toLowerCase(), attrs[key])
        } else node.setAttribute(key, attrs[key])
      })
    }
    ;(children || []).forEach(function (child) {
      if (!child) return
      node.appendChild(child)
    })
    return node
  }

  /**
   * 页头：眉标 → 标题 → 副标。每页的第一眼。
   * @param {{eyebrow?: string, title: string, sub?: string}} opts
   */
  function pageHead (opts) {
    const o = opts || {}
    const children = []
    if (o.eyebrow) children.push(el("span", { class: "eyebrow", text: o.eyebrow }))
    children.push(el("h2", { class: "page-title", text: o.title }))
    if (o.sub) children.push(el("p", { class: "page-sub", text: o.sub }))
    return el("section", { class: "page-head" }, children)
  }

  /**
   * 主卡：这一页「现在该做什么」的容器。
   * @param {{title?: string, sub?: string, children?: Array}} opts
   */
  function hero (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("h2", { text: o.title }))
    if (o.sub) children.push(el("p", { class: "reason", text: o.sub }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    return el("section", { class: "panel hero" }, children)
  }

  /**
   * 普通卡。
   * @param {{title?: string, children?: Array, className?: string}} opts
   */
  function panel (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("h2", { text: o.title }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    const cls = "panel" + (o.className ? " " + o.className : "")
    return el("section", { class: cls }, children)
  }

  /**
   * 卡内嵌套块。
   * @param {{title?: string, children?: Array}} opts
   */
  function block (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("p", { class: "block-title", text: o.title }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    return el("div", { class: "block" }, children)
  }

  /**
   * 数字块阵列。
   * @param {Array<{label: string, value: string|number, hint?: string, tone?: string}>} items
   */
  function statGrid (items) {
    const cells = (items || []).map(function (item) {
      const children = [
        el("span", { class: "stat-value", text: String(item.value) }),
        el("span", { class: "stat-label", text: String(item.label) })
      ]
      if (item.hint) children.push(el("span", { class: "stat-hint", text: item.hint }))
      return el("div", { class: "stat" + (item.tone ? " tone-" + item.tone : "") }, children)
    })
    return el("div", { class: "stat-grid" }, cells)
  }

  /**
   * 药丸徽章：状态用颜色 + 文字双承载。
   * @param {string} text
   * @param {string} [tone] "ok" | "bad" | "warn" | "accent"
   */
  function pill (text, tone) {
    return el("span", { class: "pill" + (tone ? " " + tone : ""), text: String(text) })
  }

  /**
   * 键值行：一行一个事实。
   * @param {Array<[string, string]>} pairs
   */
  function kv (pairs) {
    const rows = (pairs || []).map(function (pair) {
      return el("div", {}, [
        document.createTextNode(String(pair[0])),
        el("span", { text: String(pair[1] === undefined || pair[1] === null ? "" : pair[1]) })
      ])
    })
    return el("div", { class: "kv" }, rows)
  }

  /**
   * 空状态：说清「没有」以及「为什么」。
   * @param {string} title
   * @param {string} [hint]
   */
  function empty (title, hint) {
    const children = [el("strong", { text: String(title) })]
    if (hint) children.push(el("span", { text: String(hint) }))
    return el("div", { class: "empty" }, children)
  }

  /**
   * 把页面挂到容器上。
   *
   * 页面形状：{ data, render(state) → 节点, on? }
   * 挂载后 page.state 是页面状态的浅拷贝，事件回调里改它再调 rerender 即可。
   *
   * @param {object} container 容器节点
   * @param {object} page 页面定义
   */
  function mount (container, page) {
    page.state = Object.assign({}, page.data)
    renderInto(container, page)
    return page
  }

  /**
   * 重新渲染：清空容器后用当前 state 重画。
   * @param {object} container
   * @param {object} page
   */
  function rerender (container, page) {
    renderInto(container, page)
    return page
  }

  function renderInto (container, page) {
    while (container.children && container.children.length) {
      container.children.pop()
    }
    const node = page.render(page.state)
    if (node) container.appendChild(node)
  }

  return {
    el: el,
    pageHead: pageHead,
    hero: hero,
    panel: panel,
    block: block,
    statGrid: statGrid,
    pill: pill,
    kv: kv,
    empty: empty,
    mount: mount,
    rerender: rerender
  }
})
```

- [ ] **Step 4: 写 `shared/charts.js`**

新建 `apps/web/shared/charts.js`（从 `apps/study-hub/web/charts.js` 搬，改为 UMD 导出）：

```javascript
"use strict"
/**
 * charts.js —— 手绘 SVG 图表层（零第三方图表库）
 *
 * 纯函数：输入数据 → 输出 SVG 字符串。不碰 DOM，方便单测。
 * 浏览器挂到 window.AlgoPlanCharts，Node 可 require。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanCharts = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /** XML 转义，防注入 */
  function esc (s) {
    return String(s === undefined || s === null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
  }

  /**
   * 折线图。
   * @param {{points: Array<{x: string|number, y: number}>, width?: number, height?: number, color?: string, title?: string}} opts
   * @returns {string} SVG 字符串
   */
  function lineChart (opts) {
    const o = opts || {}
    const points = o.points || []
    const w = o.width || 480
    const h = o.height || 200
    const pad = 32
    const color = o.color || "#0284C7"

    if (points.length === 0) {
      return svgWrap(w, h, '<text x="' + (w / 2) + '" y="' + (h / 2) +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">暂无数据</text>')
    }

    const ys = points.map(function (p) { return Number(p.y) || 0 })
    const min = Math.min.apply(null, ys)
    const max = Math.max.apply(null, ys)
    const span = max - min || 1
    const stepX = points.length > 1 ? (w - pad * 2) / (points.length - 1) : 0

    const coords = points.map(function (p, i) {
      const x = pad + stepX * i
      const y = h - pad - ((Number(p.y) || 0) - min) / span * (h - pad * 2)
      return { x: x, y: y, p: p }
    })

    const path = coords.map(function (c, i) {
      return (i === 0 ? "M" : "L") + c.x.toFixed(1) + " " + c.y.toFixed(1)
    }).join(" ")

    const dots = coords.map(function (c) {
      return '<circle cx="' + c.x.toFixed(1) + '" cy="' + c.y.toFixed(1) +
        '" r="3" fill="' + color + '"><title>' + esc(c.p.x) + "：" + esc(c.p.y) + "</title></circle>"
    }).join("")

    const labels = coords.map(function (c) {
      return '<text x="' + c.x.toFixed(1) + '" y="' + (h - 10) +
        '" text-anchor="middle" fill="#94A3B8" font-size="10">' + esc(c.p.x) + "</text>"
    }).join("")

    return svgWrap(w, h,
      '<path d="' + path + '" fill="none" stroke="' + color + '" stroke-width="2"/>' +
      dots + labels
    )
  }

  /**
   * 柱状图。
   * @param {{bars: Array<{label: string, value: number}>, width?: number, height?: number, color?: string}} opts
   * @returns {string} SVG 字符串
   */
  function barChart (opts) {
    const o = opts || {}
    const bars = o.bars || []
    const w = o.width || 480
    const h = o.height || 200
    const pad = 32
    const color = o.color || "#0284C7"

    if (bars.length === 0) {
      return svgWrap(w, h, '<text x="' + (w / 2) + '" y="' + (h / 2) +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">暂无数据</text>')
    }

    const max = Math.max.apply(null, bars.map(function (b) { return Number(b.value) || 0 })) || 1
    const slot = (w - pad * 2) / bars.length
    const bw = Math.max(4, slot * 0.6)

    const rects = bars.map(function (b, i) {
      const v = Number(b.value) || 0
      const bh = (v / max) * (h - pad * 2)
      const x = pad + slot * i + (slot - bw) / 2
      const y = h - pad - bh
      return '<rect x="' + x.toFixed(1) + '" y="' + y.toFixed(1) +
        '" width="' + bw.toFixed(1) + '" height="' + Math.max(0, bh).toFixed(1) +
        '" fill="' + color + '" rx="3"><title>' + esc(b.label) + "：" + esc(v) + "</title></rect>" +
        '<text x="' + (x + bw / 2).toFixed(1) + '" y="' + (h - 10) +
        '" text-anchor="middle" fill="#94A3B8" font-size="10">' + esc(b.label) + "</text>"
    }).join("")

    return svgWrap(w, h, rects)
  }

  /**
   * 雷达图（技术雷达用）。
   * @param {{axes: Array<{label: string, value: number, max?: number}>, width?: number, height?: number, color?: string}} opts
   * @returns {string} SVG 字符串
   */
  function radarChart (opts) {
    const o = opts || {}
    const axes = o.axes || []
    const size = o.size || 220
    const cx = size / 2
    const cy = size / 2
    const r = size / 2 - 30
    const color = o.color || "#0284C7"

    if (axes.length < 3) {
      return svgWrap(size, size, '<text x="' + cx + '" y="' + cy +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">至少需要 3 个维度</text>')
    }

    function pointAt (i, ratio) {
      const angle = -Math.PI / 2 + (Math.PI * 2 * i) / axes.length
      return { x: cx + Math.cos(angle) * r * ratio, y: cy + Math.sin(angle) * r * ratio }
    }

    const rings = [0.25, 0.5, 0.75, 1].map(function (ratio) {
      const pts = axes.map(function (_, i) { return pointAt(i, ratio) })
      const d = pts.map(function (p, i) {
        return (i === 0 ? "M" : "L") + p.x.toFixed(1) + " " + p.y.toFixed(1)
      }).join(" ") + " Z"
      return '<path d="' + d + '" fill="none" stroke="#CBD5E1" stroke-width="1"/>'
    }).join("")

    const spokes = axes.map(function (_, i) {
      const p = pointAt(i, 1)
      return '<line x1="' + cx + '" y1="' + cy + '" x2="' + p.x.toFixed(1) +
        '" y2="' + p.y.toFixed(1) + '" stroke="#CBD5E1" stroke-width="1"/>'
    }).join("")

    const poly = axes.map(function (a, i) {
      const max = a.max || 100
      const ratio = Math.max(0, Math.min(1, (Number(a.value) || 0) / max))
      const p = pointAt(i, ratio)
      return p.x.toFixed(1) + "," + p.y.toFixed(1)
    }).join(" ")

    const labels = axes.map(function (a, i) {
      const p = pointAt(i, 1.18)
      return '<text x="' + p.x.toFixed(1) + '" y="' + p.y.toFixed(1) +
        '" text-anchor="middle" fill="#64748B" font-size="11">' + esc(a.label) + "</text>"
    }).join("")

    return svgWrap(size, size,
      rings + spokes +
      '<polygon points="' + poly + '" fill="' + color + '" fill-opacity="0.18" stroke="' + color + '" stroke-width="2"/>' +
      labels
    )
  }

  /**
   * 热力图（打卡用）。
   * @param {{cells: Array<{x: number, y: number, value: number}>, width?: number, height?: number, cell?: number}} opts
   * @returns {string} SVG 字符串
   */
  function heatmap (opts) {
    const o = opts || {}
    const cells = o.cells || []
    const cell = o.cell || 12
    const w = o.width || 480
    const h = o.height || 200

    const max = cells.reduce(function (m, c) {
      return Math.max(m, Number(c.value) || 0)
    }, 0) || 1

    const rects = cells.map(function (c) {
      const ratio = (Number(c.value) || 0) / max
      const alpha = ratio === 0 ? 0.05 : 0.15 + ratio * 0.6
      return '<rect x="' + (c.x * cell) + '" y="' + (c.y * cell) +
        '" width="' + (cell - 1) + '" height="' + (cell - 1) + '" rx="2"' +
        ' fill="#0284C7" fill-opacity="' + alpha.toFixed(2) + '"/>'
    }).join("")

    return svgWrap(w, h, rects)
  }

  /** SVG 外壳 */
  function svgWrap (w, h, inner) {
    return '<svg xmlns="http://www.w3.org/2000/svg" width="' + w + '" height="' + h +
      '" viewBox="0 0 ' + w + " " + h + '">' + inner + "</svg>"
  }

  return {
    lineChart: lineChart,
    barChart: barChart,
    radarChart: radarChart,
    heatmap: heatmap,
    esc: esc
  }
})
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/ui.test.js`
Expected: PASS —— 13 个用例全过

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 抽出 shared/ui.js 构件层与 shared/charts.js 图表层

ui.js 从 study-hub/web/ui.js 搬来并新增 mount/rerender，页面统一形状
{ data, render(state) }，改状态后重绘即可。charts.js 是零依赖的
手绘 SVG 纯函数层（折线/柱状/雷达/热力），不碰 DOM 便于单测。
附带 dom-stub.js —— Node 下测试构件层的最小 DOM 替身。"
git push origin main
```

---

## Task 5：`shared/platform.js` 平台适配层

**Files:**
- Create: `apps/web/shared/platform.js`
- Create: `apps/web/tests/platform.test.js`
- Modify: `apps/web/tests/dom-stub.js`（补 `querySelector` / `body.appendChild` 支持）

**Interfaces:**
- Consumes: Task 1 的 `web/style.css`（浮层类名）、Task 4 的 `dom-stub.js`
- Produces: `shared/platform.js`（UMD，浏览器挂 `window.AlgoPlanPlatform`，Node 可 `require`）导出：
  - `showToast(opts)` —— `opts: {title: string, icon?: 'none'|'success'|'error', duration?: number}`
  - `showModal(opts)` —— `opts: {title?, content?, editable?: boolean, confirmText?, cancelText?, confirmColor?}`，`Promise<{confirm: boolean, content?: string}>`
  - `showActionSheet(opts)` —— `opts: {itemList: string[], itemColor?}`，`Promise<{tapIndex: number}>`
  - `navigateTo(url)` / `navigateBack()` / `switchTab(url)`
  - `showLoading(opts)` / `hideLoading()`
  - `chooseMedia(opts)` —— `opts: {count?, mediaType?}`，`Promise<Array<{path, size, file}>>`
  - `previewImage(opts)` —— `opts: {current, urls}`
  - `stopPullDownRefresh()` —— Web 无此手势，空实现

- [ ] **Step 1: 补 DOM stub 的能力**

在 `apps/web/tests/dom-stub.js` 的 `install()` 里，把 `global.document` 改为带 `querySelector` 的版本：

```javascript
  const byId = {}

  global.document = {
    createElement: makeNode,
    createTextNode: function (text) {
      return { tagName: "#text", children: [], _text: String(text), textContent: String(text) }
    },
    body: makeNode("body"),
    documentElement: makeNode("html"),
    /** 桩：按 id 找已创建的元素。platform.js 会用它挂浮层。 */
    querySelector: function (sel) {
      if (sel.charAt(0) === "#") return byId[sel.slice(1)] || null
      return null
    },
    /** 记录 id，方便 querySelector 找到 */
    _register: function (node, id) { byId[id] = node; return node }
  }
```

同时把 `makeNode` 的返回值加上 `id` 字段与 `setAttribute` 对 `id` 的登记：

```javascript
      setAttribute: function (k, v) {
        this.attributes[k] = String(v)
        if (k === "id" && global.document && global.document._register) {
          global.document._register(this, String(v))
        }
      },
```

- [ ] **Step 2: 写失败测试**

新建 `apps/web/tests/platform.test.js`：

```javascript
"use strict"
/**
 * platform.test.js —— 平台适配层
 *
 * 验收标准：页面代码把 wx.xxx 换成 platform.xxx 之后，行为不变。
 * 所以这里逐个断言 9 个 API 的签名与返回值形状。
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const platform = require("../shared/platform.js")

test("showToast：把提示挂到页面上并返回", function () {
  const r = platform.showToast({ title: "已保存", icon: "success" })
  assert.ok(r !== undefined)
})

test("showActionSheet：返回被点选的序号", async function () {
  const p = platform.showActionSheet({ itemList: ["置顶", "删除"] })
  // 桩环境下模拟用户点第二项
  platform.__testClickSheet(1)
  const res = await p
  assert.strictEqual(res.tapIndex, 1)
})

test("showActionSheet：取消返回 -1", async function () {
  const p = platform.showActionSheet({ itemList: ["A", "B"] })
  platform.__testClickSheet(-1)
  const res = await p
  assert.strictEqual(res.tapIndex, -1)
})

test("showModal：确认返回 confirm=true", async function () {
  const p = platform.showModal({ title: "删除笔记？", content: "删除后无法恢复。" })
  platform.__testClickModal(true, "")
  const res = await p
  assert.strictEqual(res.confirm, true)
  assert.strictEqual(res.content, "")
})

test("showModal：取消返回 confirm=false", async function () {
  const p = platform.showModal({ title: "删除笔记？", content: "删除后无法恢复。" })
  platform.__testClickModal(false, "")
  const res = await p
  assert.strictEqual(res.confirm, false)
})

test("showModal：editable 模式带回输入内容", async function () {
  const p = platform.showModal({ title: "记一笔", editable: true })
  platform.__testClickModal(true, "今天把存储层写完了")
  const res = await p
  assert.strictEqual(res.confirm, true)
  assert.strictEqual(res.content, "今天把存储层写完了")
})

test("showModal：editable 模式取消时不带内容", async function () {
  const p = platform.showModal({ title: "记一笔", editable: true })
  platform.__testClickModal(false, "忽略")
  const res = await p
  assert.strictEqual(res.confirm, false)
  assert.strictEqual(res.content, "")
})

test("navigateTo：pushState 到目标路径", function () {
  const before = global.history.length
  platform.navigateTo("/workbench/tech/")
  assert.strictEqual(global.location.pathname, "/workbench/tech/")
  assert.ok(global.history.length >= before)
})

test("navigateBack：回退一步", function () {
  platform.navigateTo("/a/")
  platform.navigateTo("/b/")
  platform.navigateBack()
  assert.strictEqual(global.location.pathname, "/a/")
})

test("switchTab：替换当前路径而不是压栈", function () {
  platform.navigateTo("/a/")
  platform.switchTab("/knowledge/")
  assert.strictEqual(global.location.pathname, "/knowledge/")
})

test("showLoading / hideLoading：成对使用不抛异常", function () {
  platform.showLoading({ title: "保存中…" })
  platform.hideLoading()
})

test("previewImage：不抛异常", function () {
  platform.previewImage({ current: "blob:1", urls: ["blob:1", "blob:2"] })
})

test("stopPullDownRefresh：Web 无此手势，空实现不抛异常", function () {
  platform.stopPullDownRefresh()
})

test("chooseMedia：没有文件选择器时 reject 中文错误", async function () {
  await assert.rejects(
    function () { return platform.chooseMedia({ count: 3 }) },
    function (err) {
      assert.ok(err && err.message.indexOf("文件") >= 0, "错误信息应为中文，实际：" + (err && err.message))
      return true
    }
  )
})
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cd apps/web && node tests/platform.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/platform.js'`

- [ ] **Step 4: 写 `shared/platform.js`**

新建 `apps/web/shared/platform.js`：

```javascript
"use strict"
/**
 * platform.js —— 平台适配层
 *
 * 把小程序 wx 的 9 个交互 API 用同名同签名承接，让页面代码只需把
 * `wx.` 换成 `platform.` 就能在浏览器里跑。
 *
 * 零依赖。浏览器挂到 window.AlgoPlanPlatform，Node 可 require（供单测）。
 *
 * 为什么值得单独一层：35 个页面共 193 处 wx 交互调用，只用了 9 个 API。
 * 在这里实现一次，比每个页面各写一遍便宜得多，也让「交互层」有唯一去处。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanPlatform = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /* ============================================================
   * 运行环境探测
   * ============================================================ */

  const isBrowser = typeof document !== "undefined" && typeof document.createElement === "function"

  /** 浮层宿主节点（懒建） */
  let toastHost = null

  /** 当前挂着的遮罩（loading / modal / sheet 共用一个） */
  let activeMask = null

  /** 当前弹层里用于测试的钩子 */
  const testHooks = {
    clickSheet: null,
    clickModal: null
  }

  function ensureToastHost () {
    if (!isBrowser) return null
    if (toastHost) return toastHost
    toastHost = document.createElement("div")
    toastHost.className = "pl-toast-host"
    if (document.body) document.body.appendChild(toastHost)
    return toastHost
  }

  function el (tag, cls, text) {
    const node = document.createElement(tag)
    if (cls) node.className = cls
    if (text !== undefined && text !== null) node.textContent = String(text)
    return node
  }

  /* ============================================================
   * 轻提示
   * ============================================================ */

  /**
   * 轻提示。
   * @param {{title: string, icon?: 'none'|'success'|'error', duration?: number}} opts
   */
  function showToast (opts) {
    const o = opts || {}
    const host = ensureToastHost()
    if (!host) return null
    const node = el("div", "pl-toast", o.title || "")
    host.appendChild(node)
    const duration = o.duration === undefined ? 2000 : o.duration
    setTimeout(function () {
      const idx = host.children.indexOf(node)
      if (idx >= 0) host.children.splice(idx, 1)
    }, duration)
    return node
  }

  /* ============================================================
   * 动作菜单
   * ============================================================ */

  /**
   * 动作菜单。
   * @param {{itemList: string[], itemColor?: string}} opts
   * @returns {Promise<{tapIndex: number}>} tapIndex 为 -1 表示取消
   */
  function showActionSheet (opts) {
    const o = opts || {}
    const items = o.itemList || []
    return new Promise(function (resolve) {
      const mask = buildMask()
      const dialog = el("div", "pl-dialog")
      const list = el("div", "pl-sheet-list")

      items.forEach(function (label, index) {
        const item = el("div", "pl-sheet-item", label)
        if (o.itemColor) item.style = { color: o.itemColor }
        item.addEventListener("click", function () {
          closeMask(mask)
          resolve({ tapIndex: index })
        })
        list.appendChild(item)
      })

      dialog.appendChild(list)
      const actions = el("div", "pl-dialog-actions")
      const cancel = el("button", "pl-btn", "取消")
      cancel.addEventListener("click", function () {
        closeMask(mask)
        resolve({ tapIndex: -1 })
      })
      actions.appendChild(cancel)
      dialog.appendChild(actions)
      mask.appendChild(dialog)

      // 测试钩子：桩环境下模拟点击
      testHooks.clickSheet = function (index) {
        closeMask(mask)
        resolve({ tapIndex: index })
      }
    })
  }

  /* ============================================================
   * 对话框
   * ============================================================ */

  /**
   * 对话框。editable=true 时兼作输入框。
   * @param {{title?: string, content?: string, editable?: boolean, confirmText?: string, cancelText?: string, confirmColor?: string}} opts
   * @returns {Promise<{confirm: boolean, content: string}>}
   */
  function showModal (opts) {
    const o = opts || {}
    return new Promise(function (resolve) {
      const mask = buildMask()
      const dialog = el("div", "pl-dialog")

      if (o.title) dialog.appendChild(el("h3", "pl-dialog-title", o.title))
      if (o.content) dialog.appendChild(el("p", "pl-dialog-content", o.content))

      let input = null
      if (o.editable) {
        input = el("input", "pl-dialog-input")
        input.setAttribute("type", "text")
        dialog.appendChild(input)
      }

      const actions = el("div", "pl-dialog-actions")
      const cancel = el("button", "pl-btn", o.cancelText || "取消")
      const confirm = el("button", "pl-btn primary", o.confirmText || "确定")
      if (o.confirmColor) confirm.setAttribute("data-color", o.confirmColor)

      cancel.addEventListener("click", function () {
        closeMask(mask)
        resolve({ confirm: false, content: "" })
      })
      confirm.addEventListener("click", function () {
        const text = input ? (input.value || "") : ""
        closeMask(mask)
        resolve({ confirm: true, content: text })
      })

      actions.appendChild(cancel)
      actions.appendChild(confirm)
      dialog.appendChild(actions)
      mask.appendChild(dialog)

      if (input) setTimeout(function () { if (input.focus) input.focus() }, 0)

      testHooks.clickModal = function (confirmed, content) {
        closeMask(mask)
        resolve({ confirm: !!confirmed, content: confirmed ? String(content || "") : "" })
      }
    })
  }

  /* ============================================================
   * 遮罩
   * ============================================================ */

  function buildMask () {
    const mask = el("div", "pl-mask")
    if (document.body) document.body.appendChild(mask)
    activeMask = mask
    return mask
  }

  function closeMask (mask) {
    if (mask && document.body) {
      const idx = document.body.children.indexOf(mask)
      if (idx >= 0) document.body.children.splice(idx, 1)
    }
    if (activeMask === mask) activeMask = null
  }

  /* ============================================================
   * 路由
   * ============================================================ */

  /**
   * 压栈跳转。
   * @param {string} url
   */
  function navigateTo (url) {
    if (!isBrowser) return
    global.history.pushState({ url: url }, "", url)
  }

  /** 回退一步 */
  function navigateBack () {
    if (!isBrowser) return
    global.history.back()
  }

  /**
   * 切换主标签：替换当前记录，不压栈。
   * @param {string} url
   */
  function switchTab (url) {
    if (!isBrowser) return
    global.history.replaceState({ url: url }, "", url)
  }

  /* ============================================================
   * 加载遮罩
   * ============================================================ */

  /**
   * 显示加载遮罩。
   * @param {{title?: string, mask?: boolean}} opts
   */
  function showLoading (opts) {
    const o = opts || {}
    const mask = buildMask()
    mask.appendChild(el("div", "pl-loading", o.title || "加载中…"))
    return mask
  }

  /** 隐藏加载遮罩 */
  function hideLoading () {
    if (activeMask) closeMask(activeMask)
  }

  /* ============================================================
   * 图片
   * ============================================================ */

  /**
   * 选图。浏览器里用 <input type="file"> 触发。
   * @param {{count?: number, mediaType?: string[]}} opts
   * @returns {Promise<Array<{path: string, size: number, file: object}>>}
   */
  function chooseMedia (opts) {
    const o = opts || {}
    if (!isBrowser || typeof document.createElement !== "function") {
      return Promise.reject(new Error("当前环境不支持选择文件"))
    }
    return new Promise(function (resolve, reject) {
      const input = document.createElement("input")
      // Node 测试环境下 createElement 存在但没有 click —— 必须先探测再往下走，
      // 否则会抛英文的 "input.click is not a function"，违反「文案一律中文」。
      if (typeof input.click !== "function") {
        reject(new Error("当前环境不支持选择文件"))
        return
      }
      input.setAttribute("type", "file")
      input.setAttribute("accept", "image/*")
      if (o.count && o.count > 1) input.setAttribute("multiple", "multiple")
      input.addEventListener("change", function () {
        const files = input.files ? Array.prototype.slice.call(input.files) : []
        resolve(files.map(function (file) {
          return { path: URL.createObjectURL(file), size: file.size, file: file }
        }))
      })
      input.click()
    })
  }

  /**
   * 图片预览。
   * @param {{current: string, urls: string[]}} opts
   */
  function previewImage (opts) {
    const o = opts || {}
    if (!isBrowser) return null
    const urls = o.urls || []
    if (urls.length === 0) return null
    const box = el("div", "pl-lightbox")
    const img = document.createElement("img")
    img.setAttribute("src", o.current || urls[0])
    box.appendChild(img)
    box.addEventListener("click", function () {
      closeMask(box)
    })
    if (document.body) document.body.appendChild(box)
    activeMask = box
    return box
  }

  /** Web 无下拉刷新手势，空实现 */
  function stopPullDownRefresh () {}

  /* ============================================================
   * 测试钩子（仅供单测模拟用户操作）
   * ============================================================ */

  /**
   * 模拟点击动作菜单的第几项。
   * @param {number} index -1 表示取消
   */
  function __testClickSheet (index) {
    if (testHooks.clickSheet) testHooks.clickSheet(index)
  }

  /**
   * 模拟确认/取消对话框。
   * @param {boolean} confirmed
   * @param {string} content 输入框内容
   */
  function __testClickModal (confirmed, content) {
    if (testHooks.clickModal) testHooks.clickModal(confirmed, content)
  }

  return {
    showToast: showToast,
    showModal: showModal,
    showActionSheet: showActionSheet,
    navigateTo: navigateTo,
    navigateBack: navigateBack,
    switchTab: switchTab,
    showLoading: showLoading,
    hideLoading: hideLoading,
    chooseMedia: chooseMedia,
    previewImage: previewImage,
    stopPullDownRefresh: stopPullDownRefresh,
    __testClickSheet: __testClickSheet,
    __testClickModal: __testClickModal
  }
})
```

- [ ] **Step 5: 运行测试确认失败（补 Node 侧 history/location 桩）**

`platform.test.js` 的路由用例需要 `global.history` 与 `global.location`。在 `apps/web/tests/platform.test.js` 顶部、`require("../shared/platform.js")` **之前**插入：

```javascript
/** Node 下没有 history/location，用一个可回退的桩 */
function installHistoryStub () {
  const stack = []
  let current = "/"
  global.history = {
    pushState: function (state, title, url) { stack.push(current); current = url },
    replaceState: function (state, title, url) { current = url },
    back: function () { const prev = stack.pop(); if (prev !== undefined) current = prev },
    get length () { return stack.length + 1 }
  }
  Object.defineProperty(global, "location", {
    configurable: true,
    get: function () { return { pathname: current, href: "http://127.0.0.1" + current } }
  })
}

installHistoryStub()
```

- [ ] **Step 6: 运行测试确认通过**

Run: `cd apps/web && node tests/platform.test.js`
Expected: PASS —— 13 个用例全过

> 若 `navigateBack` 用例失败，检查 `installHistoryStub` 是否在 `require` 之前调用 —— `platform.js` 在模块加载时就会读 `global.history`。

- [ ] **Step 7: 跑全量测试**

Run: `cd apps/web && node tests/run-all.js`
Expected: 4 组全过（`server.test.js`、`storage.test.js`、`store.test.js`、`ui.test.js`、`platform.test.js` —— 共 5 组）

- [ ] **Step 8: 提交**

```bash
git add apps/web
git commit -m "feat(web): 新增 shared/platform.js 平台适配层

承接 9 个 wx 交互 API，同名同签名，页面代码把 wx. 换成 platform. 即可。
showToast / showModal / showActionSheet 用 DOM 浮层实现，
showModal 的 editable 模式对应 <dialog> + 内嵌 input，
导航用 History API。零依赖，浏览器挂 window.AlgoPlanPlatform。"
git push origin main
```

---

## Task 6：`shared/core/theme.js` 与 `with-theme.js` 适配

**Files:**
- Create: `apps/web/shared/core/theme.js`
- Create: `apps/web/shared/core/with-theme.js`
- Create: `apps/web/tests/theme.test.js`

**Interfaces:**
- Consumes: Task 3 的 `shared/core/storage.js`（读 `db:settings`）、Task 1 的 `web/style.css`（`.theme-light` / `.theme-dark`）
- Produces:
  - `shared/core/theme.js` 导出 `systemTheme()`、`resolve(mode)`、`build(settings)`、`hexToRgba(hex, alpha)`、`primaryOf(key)`、`applyThemeClass(rootEl, themeData)`、`onSystemThemeChange(cb)`。
  - `shared/core/with-theme.js` 导出 `withTheme(options)`、`applyTheme(page)`。
  - **`build()` 的返回形状与小程序版一致**：`{themeClass, isDark, brand, brandSoft, navFront, navBg}` —— 页面代码零改动。

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/theme.test.js`：

```javascript
"use strict"
/**
 * theme.test.js —— 主题解析与 Web 侧应用
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const theme = require("../shared/core/theme.js")

test("resolve：显式 light/dark 直接返回", function () {
  assert.strictEqual(theme.resolve("light"), "light")
  assert.strictEqual(theme.resolve("dark"), "dark")
})

test("resolve：auto 落到系统主题", function () {
  assert.ok(theme.resolve("auto") === "light" || theme.resolve("auto") === "dark")
})

test("build：返回 themeClass / isDark / brand / brandSoft", function () {
  const t = theme.build({ theme: "light", primaryColor: "sky" })
  assert.strictEqual(t.themeClass, "theme-light")
  assert.strictEqual(t.isDark, false)
  assert.ok(typeof t.brand === "string" && t.brand.charAt(0) === "#")
  assert.ok(t.brandSoft.indexOf("rgba") === 0)
})

test("build：dark 模式返回 theme-dark", function () {
  const t = theme.build({ theme: "dark", primaryColor: "sky" })
  assert.strictEqual(t.themeClass, "theme-dark")
  assert.strictEqual(t.isDark, true)
})

test("build：缺设置时用默认值，不抛异常", function () {
  const t = theme.build(null)
  assert.ok(t.themeClass === "theme-light" || t.themeClass === "theme-dark")
})

test("hexToRgba：短 hex 与长 hex 都对", function () {
  assert.strictEqual(theme.hexToRgba("#0284C7", 0.5), "rgba(2,132,199,0.5)")
  assert.strictEqual(theme.hexToRgba("#fff", 1), "rgba(255,255,255,1)")
})

test("primaryOf：未知 key 回退到第一个主色", function () {
  // PRIMARY_COLORS 的形状是 {key, label, light, dark}，没有 color 字段
  assert.strictEqual(theme.primaryOf("不存在的颜色").key, "sky")
  assert.strictEqual(theme.primaryOf("sky").key, "sky")
  assert.strictEqual(theme.primaryOf("violet").key, "violet")
  assert.ok(theme.primaryOf("sky").light.charAt(0) === "#")
  assert.ok(theme.primaryOf("sky").dark.charAt(0) === "#")
})

test("applyThemeClass：把主题类名写到根元素上", function () {
  const root = document.createElement("div")
  theme.applyThemeClass(root, theme.build({ theme: "dark" }))
  assert.ok(root.className.indexOf("theme-dark") >= 0, "根元素类名应是：" + root.className)
})

test("onSystemThemeChange：注册成功返回 true", function () {
  assert.strictEqual(theme.onSystemThemeChange(function () {}), true)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/theme.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/core/theme.js'`

- [ ] **Step 3: 写 `shared/core/theme.js`**

新建 `apps/web/shared/core/theme.js`：

```javascript
"use strict"
/**
 * theme.js —— 主题解析
 *
 * 主题靠 class 承载 CSS 变量（不用 [data-theme]，兼容性更好）：
 *   .theme-light { --bg-primary: #F7F8FA; ... }
 *   .theme-dark  { --bg-primary: #0B0F17; ... }
 *
 * 与小程序版的差异：小程序版用 wx.setNavigationBarColor / setTabBarStyle
 * 同步原生导航栏；Web 没有原生导航栏，改为把主题类名写到根元素上，
 * 由 CSS 变量自然生效。`build()` 的返回形状保持不变，页面代码零改动。
 */

const constants = require("./constants.js")

/** 读取系统主题 */
function systemTheme () {
  try {
    if (typeof window !== "undefined" && typeof window.matchMedia === "function") {
      return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light"
    }
    return "light"
  } catch (e) {
    return "light"
  }
}

/**
 * 把设置里的 mode 解析成实际主题
 * @param {'light'|'dark'|'auto'} mode
 * @returns {'light'|'dark'}
 */
function resolve (mode) {
  if (mode === "light" || mode === "dark") return mode
  return systemTheme()
}

/**
 * HEX → rgba 字符串
 * @param {string} hex
 * @param {number} alpha
 * @returns {string}
 */
function hexToRgba (hex, alpha) {
  const h = String(hex).replace("#", "")
  const full = h.length === 3 ? h.split("").map(function (c) { return c + c }).join("") : h
  const r = parseInt(full.slice(0, 2), 16)
  const g = parseInt(full.slice(2, 4), 16)
  const b = parseInt(full.slice(4, 6), 16)
  return "rgba(" + r + "," + g + "," + b + "," + alpha + ")"
}

/**
 * 取主色定义
 * @param {string} key
 * @returns {{key: string, label: string, light: string, dark: string}}
 */
function primaryOf (key) {
  for (let i = 0; i < constants.PRIMARY_COLORS.length; i++) {
    if (constants.PRIMARY_COLORS[i].key === key) return constants.PRIMARY_COLORS[i]
  }
  return constants.PRIMARY_COLORS[0]
}

/**
 * 根据设置计算主题数据。
 * @param {object} settings settings.get() 的结果
 * @returns {{themeClass: string, isDark: boolean, brand: string, brandSoft: string, navFront: string, navBg: string}}
 */
function build (settings) {
  const mode = (settings && settings.theme) || "auto"
  const theme = resolve(mode)
  const isDark = theme === "dark"
  const primary = primaryOf((settings && settings.primaryColor) || "sky")
  const brand = isDark ? primary.dark : primary.light

  return {
    themeClass: isDark ? "theme-dark" : "theme-light",
    isDark: isDark,
    brand: brand,
    brandSoft: hexToRgba(brand, isDark ? 0.18 : 0.12),
    navFront: isDark ? "#ffffff" : "#000000",
    navBg: isDark ? "#0B0F17" : "#F7F8FA"
  }
}

/**
 * 把主题类名与主色变量写到根元素上。
 * @param {object} rootEl 根元素（通常是 document.body）
 * @param {object} themeData build() 的结果
 */
function applyThemeClass (rootEl, themeData) {
  if (!rootEl || !themeData) return
  const classes = String(rootEl.className || "").split(/\s+/)
    .filter(function (c) { return c !== "theme-light" && c !== "theme-dark" })
  classes.push(themeData.themeClass)
  rootEl.className = classes.join(" ")
  if (rootEl.style) {
    rootEl.style.setProperty = rootEl.style.setProperty || function () {}
    rootEl.style.setProperty("--brand", themeData.brand)
    rootEl.style.setProperty("--brand-soft", themeData.brandSoft)
  }
}

/**
 * 订阅系统主题变化（仅 auto 模式需要）。
 * @param {Function} callback
 * @returns {boolean} 注册成功返回 true
 */
function onSystemThemeChange (callback) {
  try {
    if (typeof window !== "undefined" && typeof window.matchMedia === "function") {
      const mq = window.matchMedia("(prefers-color-scheme: dark)")
      if (typeof mq.addEventListener === "function") {
        mq.addEventListener("change", function () {
          callback(mq.matches ? "dark" : "light")
        })
        return true
      }
    }
    return false
  } catch (e) {
    return false
  }
}

module.exports = {
  systemTheme: systemTheme,
  resolve: resolve,
  build: build,
  hexToRgba: hexToRgba,
  primaryOf: primaryOf,
  applyThemeClass: applyThemeClass,
  onSystemThemeChange: onSystemThemeChange
}
```

- [ ] **Step 4: 写 `shared/core/with-theme.js`**

新建 `apps/web/shared/core/with-theme.js`：

```javascript
"use strict"
/**
 * with-theme.js —— 页面主题包装器
 *
 * 小程序版用高阶函数包装 Page 配置（因为 Behavior 只对 Component 生效）。
 * Web 版同样用包装器统一注入主题，页面形状：
 *
 *   const { withTheme } = require('../../core/with-theme.js')
 *   export default withTheme({
 *     data: { ... },
 *     render(state) { ... }
 *   })
 *
 * 包装后页面自动获得：
 *   - state.themeClass / isDark / brand / brandSoft
 *   - rootElement：承载主题类名的根节点
 *   - refreshTheme()：手动刷新主题（如设置页切换后立即生效）
 */

const theme = require("./theme.js")
const storage = require("./storage.js")

/** 读当前设置 */
function readSettings () {
  return storage.get(storage.KEYS.settings, null) || { theme: "auto", primaryColor: "sky" }
}

/**
 * 立即把主题写入页面 state 并同步到根元素。
 * @param {object} page 页面对象（带 state 与 rootElement）
 * @returns {object} 主题数据
 */
function applyTheme (page) {
  const settings = readSettings()
  const data = theme.build(settings)
  page.state = page.state || {}
  page.state.themeClass = data.themeClass
  page.state.isDark = data.isDark
  page.state.brand = data.brand
  page.state.brandSoft = data.brandSoft
  theme.applyThemeClass(page.rootElement || (typeof document !== "undefined" ? document.body : null), data)
  return data
}

/**
 * 包装页面配置。
 * @param {object} options 页面配置
 * @returns {object} 包装后的配置
 */
function withTheme (options) {
  const config = Object.assign({}, options)

  config.data = Object.assign({
    themeClass: "theme-light",
    isDark: false,
    brand: "#0284C7",
    brandSoft: "rgba(2,132,199,0.12)"
  }, options.data)

  const userOnMount = options.onMount
  const userOnUnmount = options.onUnmount

  config.onMount = function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    applyTheme(this)
    this.__bindSystemTheme()
    if (userOnMount) userOnMount.call(this, rootElement)
  }

  config.onUnmount = function () {
    if (userOnUnmount) userOnUnmount.call(this)
  }

  /** 仅在 auto 模式下响应系统主题切换 */
  config.__bindSystemTheme = function () {
    if (this.__themeBound) return
    this.__themeBound = true
    const self = this
    theme.onSystemThemeChange(function () {
      const settings = readSettings()
      if (settings.theme !== "auto") return
      applyTheme(self)
    })
  }

  /** 供页面手动刷新主题 */
  config.refreshTheme = function () {
    return applyTheme(this)
  }

  return config
}

module.exports = {
  withTheme: withTheme,
  applyTheme: applyTheme
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/theme.test.js`
Expected: PASS —— 9 个用例全过

- [ ] **Step 6: 跑全量并提交**

Run: `cd apps/web && node tests/run-all.js`
Expected: 6 组全过

```bash
git add apps/web
git commit -m "feat(web): theme.js 与 with-theme.js 适配到 Web

小程序版用 wx.setNavigationBarColor / setTabBarStyle 同步原生导航栏；
Web 没有原生导航栏，改为把主题类名写到根元素上，由 CSS 变量生效。
build() 的返回形状保持不变，页面代码零改动。"
git push origin main
```

---

## Task 7：端到端冒烟 —— 浏览器能打开外壳

**Files:**
- Modify: `apps/web/web/index.html`（加一个「各模块尚未接入」的说明）
- Test: `apps/web/tests/server.test.js`（补一条：外壳 HTML 含六个导航项）

**Interfaces:**
- Consumes: Task 1–6 的全部产出
- Produces: 可运行的 `node apps/web/server/main.js`；`GET /` 返回 200 且含六个模块入口。

- [ ] **Step 1: 补一条失败测试**

在 `apps/web/tests/server.test.js` 末尾追加：

```javascript
test("外壳：首页含六个模块导航入口", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  const expected = ["/workbench/", "/knowledge/", "/trip/", "/games/", "/study/", "/wiki/"]
  expected.forEach(function (href) {
    assert.ok(r.body.indexOf(href) >= 0, "外壳应含入口：" + href)
  })
})
```

- [ ] **Step 2: 运行测试确认失败或通过**

Run: `cd apps/web && node tests/server.test.js`
Expected: PASS —— Task 1 已写入六个入口，此用例应直接通过（这是回归护栏，不是新功能）

- [ ] **Step 3: 手工起服务确认**

Run（后台起，几秒后停）:
```bash
cd apps/web && node server/main.js
```

另开一个终端:
```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:5173/
```
Expected: `200`

```bash
curl -s http://127.0.0.1:5173/ | head -20
```
Expected: 输出 HTML，含 `<title>工作台</title>` 与六个 `.nav-item`

- [ ] **Step 4: 停掉服务并提交**

```bash
git add apps/web
git commit -m "test(web): 补外壳导航入口的回归断言

端到端冒烟：node apps/web/server/main.js 起得来，GET / 返回 200
且含六个模块入口。后续计划挂载各模块时，这条断言会守住外壳不被改坏。"
git push origin main
```

---

## 验收标准（本计划完成时）

1. `cd apps/web && node tests/run-all.js` —— **6 组全通过**
2. `node apps/web/server/main.js` 启动，`http://127.0.0.1:5173/` 返回 200 且显示六个模块导航
3. `apps/web/shared/core/` 含 10 个公共模块（8 个零改动 + `storage.js` + `theme.js` + `with-theme.js`），`storage.js` 的 9 个函数签名与小程序版一致
4. `apps/web/shared/platform.js` 导出 9 个 API，与 wx 同名同签名
5. `apps/web/shared/ui.js` 与 `charts.js` 可在浏览器与 Node 双端 `require`
6. **未删除任何旧小程序目录**，`apps/knowledge-hub/` 等 4 个目录原样保留
7. **未修改** `apps/study-hub/`、`apps/wiki-rag/`、`engine/` 任何文件
8. `git status` 干净（无游离改动）

## 后续计划

本计划完成后按序执行：

| 计划 | 内容 | 依赖本计划的 |
|---|---|---|
| 计划二 | workbench 数据层 + 视图层（18 页） | Task 1/2/3/4/5/6 |
| 计划三 | knowledge-hub 数据层 + 视图层（17 页 + 8 组件） | Task 1/2/3/4/5/6 |
| 计划四 | trip-log（3 页 + IndexedDB）+ games（5 页） | Task 1/2/3/4/5 |
| 计划五 | study-hub 挂载 + wiki-rag 反代 + 删除旧小程序目录 + 更新 README | 计划二/三/四 |
