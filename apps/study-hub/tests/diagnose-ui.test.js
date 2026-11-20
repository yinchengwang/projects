"use strict"
/**
 * diagnose-ui.test.js —— 诊断页的渲染层
 *
 * 用 tests/helpers/dom-stub.js 驱动真实的页面代码，验四件事：
 *   1. 页面层次还在（页头 / 主卡 / 静默卡 / 数字块 / 中文药丸）
 *   2. 三个视图标签能切，切完主卡与卡片跟着换
 *   3. 点一条轨会下钻
 *   4. 接口挂了也有中文提示，不白屏
 *
 * 桩没有 querySelector / innerHTML（契约 §9.4），所以这里只断言结构与中文文字；
 * SVG 的内容由 diagnose-e2e.test.js 那条「SVG 干净」负责。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")
const Charts = require("../web/charts.js")

// 必须先铺好 document，再 require 页面脚本
const dom = stub.createDocument()
global.document = dom.document

/** 假接口的数据：三个视图各一份 */
const PAYLOADS = {
  mastery: {
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
  radar: {
    view: "radar",
    axes: [
      { id: "algorithm", label: "算法", value: 0.31 },
      { id: "database", label: "数据库", value: 0.72 },
      { id: "ai", label: "AI", value: 0 },
      { id: "c", label: "C/C++", value: 0.31 },
      { id: "python", label: "Python", value: 0.72 },
      { id: "sql", label: "SQL", value: 0.12 },
    ],
    weak: [
      { unit_id: "algo:lc-00002", track: "algorithm", title: "合并区间", mastery: 0.12, topic: "数组" },
    ],
    strong: [
      { unit_id: "db:mvcc", track: "database", title: "MVCC", mastery: 0.72, topic: "事务" },
    ],
  },
  heatmap: {
    view: "heatmap",
    sessions: [
      { id: "s1", index: 1, started_at: "2026-11-09T10:00:00.000Z" },
      { id: "s2", index: 2, started_at: "2026-11-10T10:00:00.000Z" },
    ],
    cells: [
      { session_id: "s1", track: "algorithm", state: "pass", seq: 1 },
      { session_id: "s2", track: "algorithm", state: "wrong_answer", seq: 1 },
      { session_id: "s2", track: "database", state: "pending", seq: 2 },
    ],
  },
}

/** 最近一次请求的 URL——用来验「切标签会拉对应的视图」 */
let lastUrl = ""

/** 假 getJson：记下 URL，按视图名回数据 */
async function fakeGetJson(url) {
  lastUrl = String(url)
  const view = String(url).split("view=")[1] || "mastery"
  return PAYLOADS[view] || PAYLOADS.mastery
}

global.window = {
  StudyHub: {
    pages: {},
    el: require("../web/ui.js").el,
    getJson: fakeGetJson,
  },
  StudyHubCharts: Charts,
}

const Diagnose = require("../web/diagnose.js")
const PAGE = global.window.StudyHub.pages.diagnose

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
 * 在树里找出所有「class 里含这个词」的节点。
 * 药丸的 class 是 "pill warn" 这种带色调后缀的，光按 className 精确匹配找不到。
 */
function findAllByClassToken(node, token) {
  const out = []
  function walk(n) {
    if (!n) return
    const tokens = String(n.className || "").split(/\s+/)
    if (tokens.indexOf(token) !== -1) out.push(n)
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

/** 造一个页面容器 */
function mountView() {
  const node = dom.document.createElement("div")
  node.setAttribute("id", "view")
  return node
}

/** 等一拍：让点击触发的异步重绘跑完 */
function tick() {
  return new Promise(function (resolve) {
    setTimeout(resolve, 0)
  })
}

test("诊断页：页头 + 主卡 + 静默卡 + 数字块 + 中文药丸，四层都在", async function () {
  const view = mountView()
  await PAGE.render(view)

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

test("诊断页：三个视图标签都在，选中的那个带 aria-selected", async function () {
  const view = mountView()
  await PAGE.render(view)

  const tabs = findAllByClass(view, "diag-tab")
  assert.strictEqual(tabs.length, 3, "要有三个视图标签")
  assert.deepStrictEqual(
    tabs.map(function (tab) {
      return tab.textContent
    }),
    ["三轨总览", "六轴雷达", "会话热力图"]
  )
  assert.strictEqual(tabs[0].getAttribute("aria-selected"), "true", "默认选中三轨总览")
  assert.strictEqual(tabs[1].getAttribute("aria-selected"), "false")
  assert.strictEqual(tabs[2].getAttribute("aria-selected"), "false")
})

test("诊断页：切到雷达视图，主卡换成最短的那块板", async function () {
  const view = mountView()
  await PAGE.render(view)

  findAllByClass(view, "diag-tab")[1].dispatch("click")
  await tick()

  const hero = findByClass(view, "panel hero")
  assert.ok(hero, "切完还是要有主卡")
  assert.strictEqual(hero.children[0].textContent, "最短的那块板")
  assert.ok(lastUrl.indexOf("view=radar") !== -1, "要拉雷达的数据：" + lastUrl)
})

test("诊断页：点一条轨就下钻，下钻区出现那条轨的知识点", async function () {
  const view = mountView()
  await PAGE.render(view)

  const cards = findAllByClass(view, "diag-track")
  assert.strictEqual(cards.length, 2, "两条轨各一张卡")
  cards[1].dispatch("click")
  await tick()

  const drill = findByClass(view, "diag-drill")
  assert.ok(drill, "要有下钻区")
  const text = collectText(drill)
  assert.ok(text.indexOf("事务") !== -1, "下钻区要列出数据库轨的知识点：" + text)
  assert.ok(text.indexOf("数组") === -1, "下钻区不该混着别的轨：" + text)
})

test("诊断页：热力图视图有中文图例与表格孪生", async function () {
  const view = mountView()
  await PAGE.render(view)

  findAllByClass(view, "diag-tab")[2].dispatch("click")
  await tick()

  const legend = findByClass(view, "diag-heat-legend")
  assert.ok(legend, "热力图要有图例")
  const legendText = collectText(legend)
  assert.ok(legendText.indexOf("通过") !== -1, "图例要写中文：" + legendText)
  assert.ok(legendText.indexOf("未作答") !== -1, "图例要写中文：" + legendText)

  const table = findByClass(view, "chart-table")
  assert.ok(table, "热力图要有表格孪生")
  const tableText = collectText(table)
  assert.ok(tableText.indexOf("会话") !== -1, "表格要有表头：" + tableText)
  assert.ok(tableText.indexOf("答错") !== -1, "表格要逐格写结果：" + tableText)
})

test("诊断页：接口报错要有中文提示，不白屏", async function () {
  const saved = global.window.StudyHub
  global.window.StudyHub = {
    pages: {},
    el: require("../web/ui.js").el,
    getJson: async function () {
      throw new Error("接口炸了")
    },
  }
  try {
    const view = mountView()
    await PAGE.render(view)
    const text = collectText(view)
    assert.ok(text.indexOf("诊断数据加载失败") !== -1, "要给出中文提示：" + text)
    assert.ok(text.indexOf("接口炸了") !== -1, "要把原因写出来：" + text)
  } finally {
    global.window.StudyHub = saved
  }
})
