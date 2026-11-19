"use strict"
/**
 * p2-library-page.test.js —— 单元库页（#/library）的纯函数与层次闸门
 *
 * 可测逻辑抽成纯函数（契约 §9.4）：buildLibraryView。
 * 渲染层用 tests/helpers/dom-stub.js 驱动。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
const libraryPage = require("../web/pages/library.js")

/** 一份能喂饱单元库页的接口响应 */
function payload(items) {
  return {
    total: (items || []).length,
    items: items || [],
  }
}

const SAMPLE = [
  {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC 与多版本并发控制",
    status: "verified",
    difficulty: "进阶",
    topics: ["事务"],
    subtracks: ["mvcc"],
  },
  {
    id: "algo:lc-00007",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    status: "todo",
    difficulty: "简单",
    topics: ["数组"],
    subtracks: [],
  },
  {
    id: "ai:cnn",
    track: "ai",
    kind: "card",
    title: "卷积神经网络",
    status: "draft",
    difficulty: "进阶",
    topics: ["深度学习"],
    subtracks: [],
  },
]

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

/* ---------------- 纯函数 ---------------- */

test("buildLibraryView：算出各轨数量与总数", function () {
  const v = libraryPage.buildLibraryView(payload(SAMPLE), { track: "", q: "" })
  assert.strictEqual(v.count, 3)
  assert.strictEqual(v.total, 3)
  assert.strictEqual(v.trackCounts.algorithm, 1)
  assert.strictEqual(v.trackCounts.database, 1)
  assert.strictEqual(v.trackCounts.ai, 1)
  assert.strictEqual(v.isEmpty, false)
})

test("buildLibraryView：过滤条件原样带出（前端不二次过滤）", function () {
  const v = libraryPage.buildLibraryView(payload(SAMPLE), { track: "database", q: "mvcc" })
  assert.strictEqual(v.filter.track, "database")
  assert.strictEqual(v.filter.q, "mvcc")
  assert.strictEqual(v.hasFilter, true)
  assert.strictEqual(v.count, 3, "服务端已经过滤过，前端不再动它")
})

test("buildLibraryView：每一行带上中文标签", function () {
  const v = libraryPage.buildLibraryView(payload(SAMPLE), { track: "", q: "" })
  const row = v.rows[0]
  assert.strictEqual(row.id, "db:mvcc")
  assert.strictEqual(row.trackLabel, "数据库")
  assert.strictEqual(row.kindLabel, "知识卡")
  assert.strictEqual(row.statusLabel, "已验证")
  assert.strictEqual(row.href, "#/unit/db:mvcc", "链接用契约 §9.2 的原样冒号形态")
})

test("buildLibraryView：空库时给中文提示，不白屏", function () {
  const v = libraryPage.buildLibraryView(payload([]), { track: "", q: "" })
  assert.strictEqual(v.isEmpty, true)
  assert.ok(v.emptyHint.indexOf("单元库") !== -1, v.emptyHint)
})

/* ---------------- 渲染层 ---------------- */

test("renderLibrary：一个页头一个主卡，数字块与静默卡都在", function () {
  const view = libraryPage.buildLibraryView(payload(SAMPLE), { track: "", q: "" })
  const root = libraryPage.renderLibrary(view, payload(SAMPLE), { track: "", q: "" })

  assert.strictEqual(findAllByClass(root, "page-head").length, 1, "单元库页有且只有一个页头")
  assert.strictEqual(findAllByClass(root, "panel hero").length, 1, "主行动有且只有一个主卡")
  assert.ok(findByClass(root, "stat-grid"), "各轨数量要摆成数字块")
  assert.ok(findByClass(root, "panel quiet"), "说明是背景信息，用静默卡")
})

test("renderLibrary：轨切换按钮与搜索框都在", function () {
  const view = libraryPage.buildLibraryView(payload(SAMPLE), { track: "database", q: "mvcc" })
  const root = libraryPage.renderLibrary(view, payload(SAMPLE), { track: "database", q: "mvcc" })

  assert.ok(stub.findById(root, "library-track-algorithm"), "要有算法轨的按钮")
  assert.ok(stub.findById(root, "library-track-database"), "要有数据库轨的按钮")
  assert.ok(stub.findById(root, "library-track-ai"), "要有 AI 轨的按钮")
  const input = stub.findById(root, "library-q")
  assert.ok(input, "要有搜索框")
  assert.strictEqual(input.value, "mvcc", "搜索框里要回填当前的关键词")
})

test("renderLibrary：每个单元一行，行上有打开按钮", function () {
  const view = libraryPage.buildLibraryView(payload(SAMPLE), { track: "", q: "" })
  const root = libraryPage.renderLibrary(view, payload(SAMPLE), { track: "", q: "" })
  const rows = findAllByClass(root, "unit-row")
  assert.strictEqual(rows.length, 3)
  const text = collectText(rows[0])
  assert.ok(text.indexOf("MVCC") !== -1, "行里要有标题")
  assert.ok(text.indexOf("数据库") !== -1, "行里要有轨道的中文名")
})

test("renderLibrary：空库时给空态，不白屏", function () {
  const view = libraryPage.buildLibraryView(payload([]), { track: "", q: "" })
  const root = libraryPage.renderLibrary(view, payload([]), { track: "", q: "" })
  assert.ok(findByClass(root, "empty"), "空库要给空态")
  assert.ok(collectText(root).indexOf("单元库") !== -1)
})
