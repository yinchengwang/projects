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
const reviewPage = require("../web/pages/review.js")
const roundsPage = require("../web/pages/rounds.js")
const unitPage = require("../web/pages/unit.js")
const libraryPage = require("../web/pages/library.js")
const diagnosePage = require("../web/diagnose.js")

/**
 * 在册的页面表。
 *
 * P3 起 today / review / rounds 三页**不再注册**（契约 §9.1：文件保留在仓库里作为历史，
 * 只是不再注册）。所以这里手工拼一张表：诊断页取它自己的 render（`require` 时已注册进
 * APP.pages.diagnose，但那份注册在 app.js 的 delete 之前，取不到就退回模块导出），
 * 其余在册页面从 APP.pages 取。
 */
const PAGES = Object.assign({}, global.window.AlgoPlan.pages, {
  diagnose: global.window.AlgoPlan.pages.diagnose || { title: "诊断", render: diagnosePage.render },
})
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

/**
 * 在树里找出所有 class 里含有这个词的节点。
 *
 * findAllByClass 是**精确匹配 className**；
 * 单元页的左右两栏是 "unit-col unit-col-read" / "unit-col unit-col-do"，
 * 拿 "unit-col" 去精确匹配一个都找不到——所以补一个按词匹配的。
 * @param {object} node
 * @param {string} token
 * @returns {Array<object>}
 */
function findAllByClassToken(node, token) {
  const out = []
  function walk(n) {
    if (!n) return
    const cls = String(n.className || "").split(/\s+/)
    if (cls.indexOf(token) !== -1) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
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
  "/api/diagnose": {
    view: "mastery",
    tracks: [
      {
        track: "algorithm",
        label: "算法",
        mastery: 0.31,
        coverage: 0.4,
        bucket: "weak-sparse",
        label2: "薄弱且练得少",
        action: "轮次里优先排这类题——先补量",
        unitCount: 51,
      },
      {
        track: "database",
        label: "数据库",
        mastery: 0.72,
        coverage: 0.2,
        bucket: "strong-sparse",
        label2: "偶然会，没练透",
        action: "安排复习巩固——会做，但还没练透",
        unitCount: 12,
      },
    ],
    topics: [
      {
        track: "algorithm",
        topic: "数组",
        mastery: 0.31,
        coverage: 0.4,
        bucket: "weak-sparse",
        label: "薄弱且练得少",
        action: "先补量",
      },
      {
        track: "database",
        topic: "事务",
        mastery: 0.72,
        coverage: 0.2,
        bucket: "strong-sparse",
        label: "偶然会，没练透",
        action: "安排复习巩固",
      },
    ],
    summary: { unitCount: 3, topicCount: 2, sessionCount: 4, totalAttempts: 9 },
  },
}

test("诊断页：页头 + 主卡 + 静默卡 + 数字块 + 中文药丸，四层都在", async function () {
  stubFetch(COMMON_ROUTES)
  const view = mountView()
  await PAGES.diagnose.render(view)

  const heads = findAllByClass(view, "page-head")
  assert.strictEqual(heads.length, 1, "诊断页有且只有一个页头")
  assert.ok(findByClass(heads[0], "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(heads[0], "page-title"), "页头里要有大标题")
  assert.ok(findByClass(heads[0], "page-sub"), "页头里要有副标")

  assert.ok(findByClass(view, "panel hero"), "「先治哪条轨」要在主卡里")
  assert.ok(findByClass(view, "stat-grid"), "概览要摆成数字块")
  assert.ok(findByClass(view, "panel quiet"), "视图切换是背景信息，用静默卡")

  const pills = findAllByClassToken(view, "pill")
  assert.ok(pills.length > 0, "诊断结论要用药丸承载，不能只靠颜色")
  const texts = pills
    .map(function (p) {
      return p.textContent
    })
    .join(" ")
  assert.ok(texts.indexOf("薄弱且练得少") !== -1, "药丸要带中文：" + texts)
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

test("页头只有一个：在册的页面各自渲染都不许出现两个页头", async function () {
  stubFetch(COMMON_ROUTES)

  const views = [
    { name: "诊断", render: PAGES.diagnose.render },
  ]

  for (const item of views) {
    const view = mountView()
    await item.render(view)
    const heads = findAllByClass(view, "page-head")
    assert.strictEqual(heads.length, 1, item.name + "页出现了 " + heads.length + " 个页头")
  }
})

/* ---------------- P2：单元页与单元库页 ---------------- */

/** 一张数据库卡：左栏讲解，右栏跑 demo + 看断言结果 */
const P2_CARD = {
  id: "db:mvcc",
  track: "database",
  kind: "card",
  title: "MVCC 与多版本并发控制",
  status: "verified",
  difficulty: "进阶",
  topics: ["事务"],
  subtracks: ["mvcc"],
  entry: {
    demo: "scaffold/grokking/mvcc/main.py",
    graded: "script",
    script: "units/database/mvcc.assert.py",
    language: "python",
  },
  asserts: [],
  statement_html:
    "<h2>MVCC</h2><p>多版本并发控制靠<strong>快照</strong>读。</p><ul><li>读不阻塞写</li></ul>",
}

/** 一道算法题：左栏题面，右栏代码编辑区 + 判题结果 */
const P2_PROBLEM = {
  id: "algo:lc-00007",
  track: "algorithm",
  kind: "problem",
  title: "两数之和",
  status: "todo",
  difficulty: "简单",
  topics: ["哈希表"],
  subtracks: [],
  entry: { graded: "user", language: "cpp" },
  asserts: [],
  statement_html: "<h2>题面</h2><p>给定整数数组与目标值，返回两个数的下标。</p>",
}

/** 判题结果：与 POST /api/units/:id/judge 的响应同形 */
const P2_RESULT = {
  verdict: "pass",
  message: "全部断言通过（2 条）",
  stdout: "─── demo 输出开始 ───\nhello\n─── demo 输出结束 ───",
  stderr: "",
  passed: 2,
  total: 2,
  failed: null,
  durationMs: 42,
}

test("单元页（知识卡）：左栏是讲解，右栏是「运行断言」与判题结果区", function () {
  const root = unitPage.renderUnit(
    unitPage.buildUnitView(P2_CARD, P2_CARD.statement_html),
    P2_CARD,
    P2_CARD.statement_html
  )

  assert.strictEqual(findAllByClass(root, "page-head").length, 1, "单元页有且只有一个页头")
  assert.ok(findByClass(root, "panel hero"), "右栏「动手做」要在主卡里")

  const cols = findAllByClassToken(root, "unit-col")
  assert.strictEqual(cols.length, 2, "左栏读、右栏做，一共两栏")
  const readCol = findAllByClassToken(root, "unit-col-read")[0]
  const doCol = findAllByClassToken(root, "unit-col-do")[0]
  assert.ok(readCol, "要有左栏（读）")
  assert.ok(doCol, "要有右栏（做）")

  // 左栏：讲解正文渲染成真节点（不是 innerHTML）
  const readText = collectText(readCol)
  assert.ok(readText.indexOf("多版本并发控制") !== -1, "左栏要有讲解正文：" + readText)
  assert.ok(readText.indexOf("快照") !== -1, "讲解里的 strong 也要渲染出来")

  // 右栏：跑 demo 的按钮 + 判题结果区
  assert.ok(stub.findById(doCol, "unit-run"), "右栏要有「运行断言」按钮")
  assert.ok(stub.findById(doCol, "unit-rerun"), "右栏要有「重新运行」按钮")
  assert.ok(stub.findById(doCol, "unit-result"), "右栏要有判题结果区")
  assert.strictEqual(stub.findById(doCol, "unit-code"), null, "知识卡右栏不该有代码编辑区")
})

test("单元页（算法题）：左栏是题面，右栏是代码编辑区 + 判题结果区", function () {
  const root = unitPage.renderUnit(
    unitPage.buildUnitView(P2_PROBLEM, P2_PROBLEM.statement_html),
    P2_PROBLEM,
    P2_PROBLEM.statement_html
  )

  assert.strictEqual(findAllByClass(root, "page-head").length, 1)
  const readCol = findAllByClassToken(root, "unit-col-read")[0]
  const doCol = findAllByClassToken(root, "unit-col-do")[0]

  const readText = collectText(readCol)
  assert.ok(readText.indexOf("给定整数数组") !== -1, "左栏要有题面：" + readText)

  const editor = stub.findById(doCol, "unit-code")
  assert.ok(editor, "右栏要有代码编辑区")
  assert.strictEqual(editor.tagName, "textarea", "代码编辑区是 textarea")
  assert.ok(stub.findById(doCol, "unit-judge"), "右栏要有「提交判题」按钮")
  assert.ok(stub.findById(doCol, "unit-result"), "右栏要有判题结果区")
  assert.strictEqual(stub.findById(doCol, "unit-run"), null, "算法题右栏不该有「运行断言」按钮")
})

test("单元页：判题结果按五类 verdict 上色，中文文案", function () {
  const root = unitPage.renderUnit(
    unitPage.buildUnitView(P2_CARD, P2_CARD.statement_html),
    P2_CARD,
    P2_CARD.statement_html
  )
  const box = stub.findById(root, "unit-result")
  assert.ok(box, "结果区要有 unit-result 的 id")
  unitPage.paintResult(box, P2_RESULT)
  const text = collectText(box)
  assert.ok(text.indexOf("通过") !== -1, "结果区要有中文结论：" + text)
  assert.ok(text.indexOf("2/2") !== -1, "结果区要显示通过条数：" + text)
  assert.ok(text.indexOf("demo 输出") !== -1, "结果区要显示 demo 输出：" + text)
})

test("单元页：demo 输出被拆成展示区", function () {
  const parts = unitPage.splitDemoOutput(P2_RESULT.stdout)
  assert.strictEqual(parts.hasMarkers, true)
  assert.strictEqual(parts.demoOutput, "hello")
})

test("单元库页：页头、主卡、数字块、静默卡齐全", function () {
  const payload = { total: 1, items: [P2_CARD] }
  const filter = { track: "", q: "" }
  const root = libraryPage.renderLibrary(
    libraryPage.buildLibraryView(payload, filter),
    payload,
    filter
  )
  assert.strictEqual(findAllByClass(root, "page-head").length, 1)
  assert.strictEqual(findAllByClass(root, "panel hero").length, 1)
  assert.ok(findByClass(root, "stat-grid"))
  assert.ok(findByClass(root, "panel quiet"))
})
