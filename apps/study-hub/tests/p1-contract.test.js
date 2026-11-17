"use strict"
/**
 * p1-contract.test.js —— 契约回写的回归测试
 *
 * Task 1 把 P1 期间对《接口契约》的补充写回了契约文件本身。
 * 这份测试把那批补充固化下来：契约文件在，就断言补充还在；
 * 契约文件不在（projects 仓被单独 checkout 的情形），整份测试跳过。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const CONTRACT = path.join(
  __dirname,
  "..",
  "..",
  "..",
  "..",
  "docs",
  "superpowers",
  "plans",
  "2026-11-12-study-hub-接口契约.md"
)

const hasContract = fs.existsSync(CONTRACT)
const contractText = hasContract ? fs.readFileSync(CONTRACT, "utf8") : ""
const skip = hasContract ? false : "契约文件不在（顶层聚合仓没一起 checkout），跳过契约回写断言"

/** Task 1 写进契约的各段标题——少一段就说明回写被覆盖了 */
const NEEDED = [
  "#### 4.3.1 P1 追加的 store 方法与归属裁决",
  "### 6.0 候选池与到期集合",
  "#### 8.2.1 P1 阶段",
  "#### 8.2.2 P1 新增 —— 会话设置",
  "#### 8.2.3 P1 阶段队列项",
  "window.AlgoPlan === window.StudyHub",
  "每期只把已交付的页面加进 TABS",
  "core/scheduler/pool.js",
  "web/style.css",
]

test("契约里补齐了 P1 的各段补充", { skip: skip }, function () {
  const missing = NEEDED.filter(function (needle) {
    return contractText.indexOf(needle) === -1
  })
  assert.deepStrictEqual(missing, [], "契约里少了：" + missing.join(" / "))
})

test("P1 的会话路由与设置路由都写进契约了", { skip: skip }, function () {
  const routes = [
    "/api/session/start",
    "/api/session/next",
    "/api/session/pause",
    "/api/session/submit",
    "/api/settings",
  ]
  const missing = routes.filter(function (route) {
    return contractText.indexOf(route) === -1
  })
  assert.deepStrictEqual(missing, [], "契约里少了路由：" + missing.join(" / "))
})

test("web/app.js 用别名保留 window.AlgoPlan", function () {
  const src = fs.readFileSync(path.join(__dirname, "..", "web", "app.js"), "utf8")
  assert.ok(src.indexOf("window.StudyHub") !== -1, "app.js 要定义 window.StudyHub")
  assert.ok(
    src.indexOf("window.AlgoPlan = window.StudyHub") !== -1,
    "app.js 要留别名 window.AlgoPlan，老页面脚本靠它继续工作"
  )
})

test("样式表仍然只有一套 CSS 变量（追加队列样式没动 :root）", function () {
  const css = fs.readFileSync(path.join(__dirname, "..", "web", "style.css"), "utf8")
  const roots = css.match(/:root\s*\{/g) || []
  assert.strictEqual(roots.length, 1, "应该只有一套 CSS 变量")
  assert.ok(/--bg:\s*#1/i.test(css), "底色要保持暗色")
  assert.ok(css.indexOf(".queue-item") !== -1, "队列样式要加上了")
})
