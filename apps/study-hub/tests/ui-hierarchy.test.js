"use strict"
/**
 * ui-hierarchy.test.js —— 界面层次的回归闸门
 *
 * 界面重做（保持暗色、重做层次）之后，层次靠三件事保证：
 *   1. 每页最上面有且只有一个页头（眉标 + 大标题 + 副标）
 *   2. 「现在该做什么」在主卡里（.panel.hero），不是普通卡
 *   3. 关键状态用中文文字承载（药丸 / 徽章），不靠颜色单独表意
 *
 * 这里用 tests/helpers/dom-stub.js 驱动真实的页面渲染代码，
 * 断言渲染出来的树里有这些层次——纯 CSS 的样子测不了，但结构能测。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")

// 页面脚本把自己注册进 window.AlgoPlan.pages——统一从那里取 render，
// 不依赖各页面脚本自己导出了什么
const todayPage = require("../web/pages/today.js")
require("../web/pages/answer.js")
require("../web/pages/review.js")
require("../web/pages/rounds.js")
const PAGES = global.window.AlgoPlan.pages

/** 铺一个假的 fetch：按路径前缀返回预设响应 */
function stubFetch(routes) {
  global.fetch = async function (url) {
    const key = Object.keys(routes).find(function (prefix) {
      return String(url).indexOf(prefix) === 0
    })
    if (!key) {
      return { ok: false, status: 404, json: async function () { return { error: "没有这条路由" } } }
    }
    const body = routes[key]
    return { ok: true, status: 200, json: async function () { return body } }
  }
}

/** 在树里找出所有 class 精确匹配的节点 */
function findAllByClass(node, className) {
  const out = []
  function walk(n) {
    if (!n) return
    if (n.className === className) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

/** 在树里找出第一个 class 精确匹配的节点 */
function findByClass(node, className) {
  return findAllByClass(node, className)[0] || null
}

/** 把树里所有文本拼起来 */
function collectText(node) {
  let out = ""
  function walk(n) {
    if (!n) return
    if (n.textContent) out += " " + n.textContent
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

/** 造一个空容器 */
function mountView() {
  const node = dom.document.createElement("div")
  node.setAttribute("id", "view")
  return node
}

/** 一份能喂饱各页的接口响应 */
const COMMON_ROUTES = {
  "/api/next": {
    action: {
      kind: "round",
      problemId: "lc-00001",
      title: "两数之和",
      estimatedMinutes: 15,
      reason: "轮次 r33 还剩 11 张卡片没做",
      reviewQueueLength: 3,
      roundId: "r33",
      itemId: "two_sum",
    },
    reviewQueueLength: 3,
  },
  "/api/progress": {
    date: "2026-11-12",
    solved: 2,
    reviewed: 1,
    reviewMinutes: 5,
    quota: { questions: 1, minutes: 5, limitQuestions: 10, limitMinutes: 20, exhausted: false },
  },
  "/api/health": {
    ok: true,
    node: "v24.0.0",
    learning: { ok: true, root: "/tmp/learning", reason: "" },
    bank: { problemCount: 740, fileCount: 740, warnings: [] },
    attempts: 12,
  },
  "/api/rounds": {
    rounds: [
      {
        id: "r33",
        title: "Grokking 算法篇 — 学习进度",
        status: "active",
        goal: "Grokking 算法篇 16 张卡片",
        file: "/tmp/learning/progress/r33-progress.md",
        items: [
          { id: "two_sum", title: "两数之和", status: "done", doneDate: "2026-11-11" },
          { id: "arrays", title: "数组技巧", status: "todo", doneDate: "" },
        ],
      },
    ],
    warnings: [],
  },
  "/api/bank": { total: 1, items: [] },
  "/api/problem/": {
    id: "lc-00001",
    title: "两数之和",
    statement: "给定整数数组与目标值。",
    cases: { sample: [{ input: "2 7 11 15", expect: "0 1" }], total: 5 },
  },
  "/api/insights": {
    topics: [
      {
        topic: "数组",
        mastery: 0.2836,
        coverage: 0.4,
        bucket: "weak-sparse",
        label: "薄弱且练得少",
        action: "轮次里优先排这类题——先补量",
        attempts: 4,
        firstPass: 1,
        lapses: 2,
        lastAt: "2026-11-10T09:00:00.000Z",
        expected: 5,
        expectedSource: "floor",
        problems: 2,
      },
    ],
    summary: {
      topicCount: 1,
      untouchedTopics: 0,
      weakCount: 1,
      strongCount: 0,
      avgMastery: 0.2836,
      avgCoverage: 0.4,
      totalAttempts: 4,
      totalProblems: 2,
      lastPracticeAt: "2026-11-10T09:00:00.000Z",
      minExpectedQuestions: 5,
      windowDays: 30,
      windowAttempts: 4,
    },
    series: [{ date: "2026-11-11", count: 4 }],
  },
}

/* ---------------- 各页的层次 ---------------- */

test("今日页：页头 + 主卡 + 数字块 + 静默卡，四层都在", async function () {
  stubFetch(COMMON_ROUTES)
  const view = mountView()
  await PAGES.today.render(view)

  const heads = findAllByClass(view, "page-head")
  assert.strictEqual(heads.length, 1, "今日页有且只有一个页头")
  assert.ok(findByClass(heads[0], "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(heads[0], "page-title"), "页头里要有大标题")
  assert.ok(findByClass(heads[0], "page-sub"), "页头里要有副标")

  assert.ok(findByClass(view, "panel hero"), "「下一步」要在主卡里")
  assert.ok(findByClass(view, "stat-grid"), "今日进度要摆成数字块")
  assert.ok(findByClass(view, "panel quiet"), "运行状态是背景信息，用静默卡")
})

test("复习页：页头 + 主卡 + 数字块", async function () {
  // 复习页要有一张真的到期卡片才会渲染主卡
  const routes = Object.assign({}, COMMON_ROUTES)
  routes["/api/next"] = {
    action: {
      kind: "review",
      problemId: "lc-00001",
      title: "两数之和",
      estimatedMinutes: 5,
      reason: "上次做的是 14 天前，该复习了",
      reviewQueueLength: 3,
    },
    reviewQueueLength: 3,
  }
  stubFetch(routes)
  const view = mountView()
  await PAGES.review.render(view)

  assert.strictEqual(findAllByClass(view, "page-head").length, 1, "复习页有且只有一个页头")
  assert.ok(findByClass(view, "panel hero"), "「该复习这道」要在主卡里")
  assert.ok(findByClass(view, "stat-grid"), "队列概况要摆成数字块")
})

test("轮次页：页头 + 当前轮次进主卡 + 状态是中文药丸", async function () {
  stubFetch(COMMON_ROUTES)
  const A = global.window.AlgoPlan
  A.state.currentRoundId = "r33"
  const view = mountView()
  await PAGES.rounds.render(view)

  assert.strictEqual(findAllByClass(view, "page-head").length, 1, "轮次页有且只有一个页头")
  assert.ok(findByClass(view, "panel hero"), "当前轮次要在主卡里")

  const pills = findAllByClass(view, "pill")
  assert.ok(pills.length > 0, "轮次状态要用药丸承载，不能只靠颜色")
  const texts = pills.map(function (p) { return p.textContent })
  assert.ok(
    texts.join(" ").indexOf("待完成") !== -1,
    "状态要有中文文字：" + texts.join(",")
  )
})

test("作答页：页头在题目之上", async function () {
  stubFetch(COMMON_ROUTES)
  const A = global.window.AlgoPlan
  A.state.problemId = "lc-00001"
  const view = mountView()
  await PAGES.answer.render(view)

  assert.strictEqual(findAllByClass(view, "page-head").length, 1, "作答页有且只有一个页头")
})

test("保持暗色单主题：没有浅色/双主题的开关", function () {
  const fs = require("node:fs")
  const path = require("node:path")
  const html = fs.readFileSync(path.join(__dirname, "..", "web", "index.html"), "utf8")
  const css = fs.readFileSync(path.join(__dirname, "..", "web", "style.css"), "utf8")

  // 没有 theme 切换按钮、没有 data-theme 属性切换
  assert.ok(html.indexOf("data-theme") === -1, "不该有主题切换属性")
  assert.ok(html.indexOf("theme-toggle") === -1, "不该有主题切换按钮")

  // 样式表里只有一个 :root 定义（一个变量集 = 一套主题）
  const roots = css.match(/:root\s*\{/g) || []
  assert.strictEqual(roots.length, 1, "应该只有一套 CSS 变量")

  // 底色仍是暗色
  assert.ok(/--bg:\s*#1/i.test(css), "底色要保持暗色")
})

/* ---------------- 层次构件本身 ---------------- */

test("公共构件：页头 / 主卡 / 数字块 / 空态 都造得出层次类", function () {
  const UI = require("../web/ui.js")

  const head = UI.pageHead({ eyebrow: "今日", title: "今天该做什么", sub: "一句话说明" })
  assert.strictEqual(head.className, "page-head")
  assert.ok(findByClass(head, "eyebrow"))
  assert.ok(findByClass(head, "page-title"))
  assert.ok(findByClass(head, "page-sub"))

  const hero = UI.hero({ title: "下一步" })
  assert.strictEqual(hero.className, "panel hero")

  const grid = UI.statGrid([{ label: "今天做题", value: "2 道", tone: "accent" }])
  assert.strictEqual(grid.className, "stat-grid")
  const cell = findByClass(grid, "stat tone-accent")
  assert.ok(cell, "数字块要有格子")
  assert.ok(findByClass(cell, "stat-value"))
  assert.ok(findByClass(cell, "stat-label"))

  const hole = UI.empty("没有内容", "因为还没开始")
  assert.strictEqual(hole.className, "empty")
  assert.ok(collectText(hole).indexOf("没有内容") !== -1)
})

test("页头只有一个：五页各自渲染都不许出现两个页头", async function () {
  stubFetch(COMMON_ROUTES)
  const A = global.window.AlgoPlan
  A.state.problemId = "lc-00001"

  const views = [
    { name: "今日", render: PAGES.today.render },
    { name: "作答", render: PAGES.answer.render },
    { name: "复习", render: PAGES.review.render },
    { name: "轮次", render: PAGES.rounds.render },
  ]

  for (const item of views) {
    const view = mountView()
    await item.render(view)
    const heads = findAllByClass(view, "page-head")
    assert.strictEqual(heads.length, 1, item.name + "页出现了 " + heads.length + " 个页头")
  }
})
