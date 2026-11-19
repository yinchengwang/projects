"use strict"
/**
 * p2-unit-page.test.js —— 单元学习页（#/unit/:id）的纯函数与层次闸门
 *
 * 这个页面的可测逻辑都抽成了纯函数（契约 §9.4）：
 *   buildUnitView / parseHtmlNodes / decodeEntities / splitDemoOutput
 * 渲染层用 tests/helpers/dom-stub.js 驱动，验证「左读右做」的层次。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
const unitPage = require("../web/pages/unit.js")

/** 造一张卡类单元 */
function cardUnit(over) {
  return Object.assign(
    {
      id: "db:mvcc",
      track: "database",
      kind: "card",
      title: "MVCC 与多版本并发控制",
      status: "verified",
      difficulty: "进阶",
      topics: ["事务", "隔离级别"],
      subtracks: ["mvcc"],
      entry: {
        demo: "scaffold/grokking/mvcc/main.py",
        reference: null,
        graded: "script",
        script: "units/database/mvcc.assert.py",
        language: "python",
      },
      asserts: [{ type: "exit", expect: 0 }],
      path: "units/database/mvcc.md",
      statement: "",
    },
    over || {}
  )
}

/** 造一道算法题 */
function problemUnit(over) {
  return Object.assign(
    {
      id: "algo:lc-00007",
      track: "algorithm",
      kind: "problem",
      title: "两数之和",
      status: "verified",
      difficulty: "简单",
      topics: ["数组"],
      subtracks: ["fundamentals"],
      entry: { demo: null, reference: null, graded: "user", script: null, language: "python" },
      asserts: [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }],
      path: "bank/algorithm/lc-00007.md",
      statement: "",
    },
    over || {}
  )
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
 * 按 class **词元**找节点。多 class 的元素（例如 `unit-col unit-col-read`）
 * 用 findAllByClass 的**精确匹配**找不到 —— 那个是给 G7 的 `"panel hero"` 用的。
 * @param {object} node
 * @param {string} token
 * @returns {object|null}
 */
function findByToken(node, token) {
  let hit = null
  function walk(n) {
    if (hit || !n) return
    const parts = String(n.className || "").split(/\s+/)
    if (parts.indexOf(token) !== -1) {
      hit = n
      return
    }
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return hit
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

/** 节点描述树的形状（只看元素标签） */
function shape(nodes) {
  const out = []
  ;(nodes || []).forEach(function (n) {
    if (!n || n.tag === "#text") return
    out.push(n.tag + "(" + shape(n.children) + ")")
  })
  return out.join("")
}

/** 节点描述树里的全部文字 */
function textOf(nodes) {
  let out = ""
  ;(nodes || []).forEach(function (n) {
    if (!n) return
    if (n.tag === "#text") {
      out += n.text
      return
    }
    out += textOf(n.children)
  })
  return out
}

/* ---------------- 纯函数 ---------------- */

test("buildUnitView：卡类单元给出中文的轨道 / 形态 / 状态", function () {
  const v = unitPage.buildUnitView(cardUnit(), "<p>正文</p>")
  assert.strictEqual(v.id, "db:mvcc")
  assert.strictEqual(v.trackLabel, "数据库")
  assert.strictEqual(v.kindLabel, "知识卡")
  assert.strictEqual(v.statusLabel, "已验证")
  assert.strictEqual(v.isProblem, false)
  assert.strictEqual(v.assertCount, 1)
  assert.strictEqual(v.demoPath, "scaffold/grokking/mvcc/main.py")
  assert.strictEqual(v.scriptPath, "units/database/mvcc.assert.py")
})

test("buildUnitView：算法题与知识卡分得开", function () {
  const v = unitPage.buildUnitView(problemUnit(), "<p>题面</p>")
  assert.strictEqual(v.kindLabel, "算法题")
  assert.strictEqual(v.isProblem, true)
  assert.strictEqual(v.graded, "user")
  assert.strictEqual(v.trackLabel, "算法")
})

test("buildUnitView：题面为空时说清没有正文", function () {
  const v = unitPage.buildUnitView(cardUnit(), "")
  assert.strictEqual(v.hasStatement, false)
  assert.strictEqual(v.statementNodes.length, 0)
})

test("parseHtmlNodes：标题、段落、列表都认得", function () {
  const html = "<h2>版本链</h2><p>每行数据有多个版本。</p><ul><li>写操作新增版本</li><li>读操作看快照</li></ul>"
  const nodes = unitPage.parseHtmlNodes(html)
  assert.strictEqual(shape(nodes), "h2()p()ul(li()li())")
  assert.strictEqual(textOf(nodes), "版本链每行数据有多个版本。写操作新增版本读操作看快照")
})

test("parseHtmlNodes：行内标签与有序列表都认得", function () {
  const html = "<p>看 <strong>版本链</strong> 与 <code>ReadView</code></p><ol><li>第一步</li></ol>"
  const nodes = unitPage.parseHtmlNodes(html)
  assert.strictEqual(shape(nodes), "p(strong()code())ol(li())")
  assert.ok(textOf(nodes).indexOf("版本链") !== -1)
})

test("parseHtmlNodes：HTML 实体解得开", function () {
  const nodes = unitPage.parseHtmlNodes("<p>a &amp; b &lt;c&gt; &#65;</p>")
  assert.strictEqual(textOf(nodes), "a & b <c> A")
})

test("parseHtmlNodes：认不出的标签也不丢内容", function () {
  const nodes = unitPage.parseHtmlNodes("<p>hello <blink>world</blink></p>")
  assert.strictEqual(shape(nodes), "p(blink())")
  assert.strictEqual(textOf(nodes), "hello world")
})

test("splitDemoOutput：按契约 §3.5 的定界行切出 demo 输出", function () {
  const stdout =
    "─── demo 输出开始 ───\nhello demo\n─── demo 输出结束 ───\n全部断言通过（1 条）\n"
  const split = unitPage.splitDemoOutput(stdout)
  assert.strictEqual(split.hasMarkers, true)
  assert.strictEqual(split.demoOutput, "hello demo")
  assert.ok(split.rest.indexOf("全部断言通过") !== -1)
  assert.strictEqual(split.rest.indexOf("demo 输出开始"), -1)
})

test("splitDemoOutput：没有定界行就原样返回，不炸", function () {
  const split = unitPage.splitDemoOutput("全部断言通过（1 条）")
  assert.strictEqual(split.hasMarkers, false)
  assert.strictEqual(split.demoOutput, "")
  assert.strictEqual(split.rest, "全部断言通过（1 条）")
})

/* ---------------- 渲染层 ---------------- */

test("renderUnit：左读右做两栏，一个页头一个主卡", function () {
  const unit = cardUnit()
  const html = "<h2>版本链</h2><p>每行数据有多个版本。</p>"
  const view = unitPage.buildUnitView(unit, html)
  const root = unitPage.renderUnit(view, unit, html)

  assert.strictEqual(findAllByClass(root, "page-head").length, 1, "单元页有且只有一个页头")
  assert.strictEqual(findAllByClass(root, "panel hero").length, 1, "主行动有且只有一个主卡")
  // 两栏的 class 是 `unit-col unit-col-read`（多 class，便于 CSS 复用公共样式）——
  // 必须按词元找，用 findByClass 的精确匹配永远找不到。
  assert.ok(findByToken(root, "unit-col-read"), "左栏（读）要在")
  assert.ok(findByToken(root, "unit-col-do"), "右栏（做）要在")
  assert.ok(findByClass(root, "stat-grid"), "数字要摆成数字块")
  assert.ok(findByClass(root, "panel quiet"), "元信息是背景信息，用静默卡")
})

test("renderUnit：题面正文进了 DOM（全程没用 innerHTML）", function () {
  const unit = cardUnit()
  const html = "<h2>版本链</h2><p>每行数据有多个版本。</p>"
  const view = unitPage.buildUnitView(unit, html)
  const root = unitPage.renderUnit(view, unit, html)
  const text = collectText(root)
  assert.ok(text.indexOf("版本链") !== -1, "题面标题要渲染出来")
  assert.ok(text.indexOf("每行数据有多个版本") !== -1, "题面正文要渲染出来")
})

test("renderUnit：卡片形态右栏有「运行断言」按钮", function () {
  const unit = cardUnit()
  const view = unitPage.buildUnitView(unit, "<p>正文</p>")
  const root = unitPage.renderUnit(view, unit, "<p>正文</p>")
  assert.ok(stub.findById(root, "unit-run"), "卡片要有运行断言的按钮")
  assert.ok(stub.findById(root, "unit-complete"), "要有完成本单元的按钮")
  assert.strictEqual(stub.findById(root, "unit-code"), null, "卡片不该有代码编辑区")
})

test("renderUnit：算法题形态右栏有代码编辑区与「提交判题」按钮", function () {
  const unit = problemUnit()
  const view = unitPage.buildUnitView(unit, "<p>题面</p>")
  const root = unitPage.renderUnit(view, unit, "<p>题面</p>")
  assert.ok(stub.findById(root, "unit-code"), "算法题要有代码编辑区")
  assert.ok(stub.findById(root, "unit-judge"), "算法题要有提交判题的按钮")
  assert.strictEqual(stub.findById(root, "unit-run"), null, "算法题不该有运行断言的按钮")
})

test("renderUnit：没正文时给出中文空态，不白屏", function () {
  const unit = cardUnit()
  const view = unitPage.buildUnitView(unit, "")
  const root = unitPage.renderUnit(view, unit, "")
  assert.ok(findByClass(root, "empty"), "没正文要给空态")
  assert.ok(collectText(root).indexOf("还没有正文") !== -1)
})

/* ---------------- web/llm.js 的 id 正则（契约 §9.2） ---------------- */

test("llm.js 的 id 正则认得三种新形态，也还认旧题号", function () {
  const llm = require("../web/llm.js")
  const saved = global.location
  function idOf(hash) {
    global.location = { hash: hash }
    try {
      return llm.findProblemIdInHash()
    } finally {
      global.location = saved
    }
  }
  assert.strictEqual(idOf("#/unit/algo:lc-00007"), "algo:lc-00007")
  assert.strictEqual(idOf("#/unit/db:mvcc"), "db:mvcc")
  assert.strictEqual(idOf("#/unit/ai:cnn"), "ai:cnn")
  assert.strictEqual(idOf("#/unit/db%3Amvcc"), "db:mvcc", "百分号编码的冒号也要认")
  assert.strictEqual(idOf("#/unit/lc-00001"), "lc-00001", "旧形态不能改")
})
