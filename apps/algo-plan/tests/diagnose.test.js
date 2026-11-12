"use strict"
/**
 * diagnose.test.js —— 诊断页的纯函数层
 *
 * 渲染层（mountDiagnose / showDiagnose）要真实 DOM，node 里没有，
 * 所以这里只测纯函数：视图数据怎么算、怎么排、截断到几条。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const Charts = require("../web/charts.js")

/**
 * 页面脚本在浏览器里跑，但「注册进 window.AlgoPlan.pages」这一步可以在 Node 里验：
 * 给一个最小的 window 替身，把页面脚本 require 进来。
 * 顺序很重要：先搭好替身，再 require——脚本加载时就会注册自己。
 */
global.window = {
  AlgoPlan: {
    pages: {},
    el: function (tag, attrs, children) {
      return { tag: tag, attrs: attrs, children: children }
    },
    getJson: async function () {
      return { topics: [], summary: {}, series: [] }
    },
  },
  AlgoPlanCharts: Charts,
}

const Diagnose = require("../web/diagnose.js")

/** 一份固定数据：覆盖四个档位 */
const TOPICS = [
  {
    topic: "数组",
    mastery: 0.28,
    coverage: 0.4,
    bucket: "weak-sparse",
    label: "薄弱且练得少",
    action: "轮次里优先排这类题——先补量",
    attempts: 6,
    expected: 5,
    problems: 2,
  },
  {
    topic: "哈希表",
    mastery: 0.62,
    coverage: 0.8,
    bucket: "strong-dense",
    label: "掌握",
    action: "保持手感",
    attempts: 5,
    expected: 5,
    problems: 4,
  },
  {
    topic: "动态规划",
    mastery: 0.45,
    coverage: 0.2,
    bucket: "weak-dense",
    label: "练过但总错",
    action: "回头复盘错题",
    attempts: 4,
    expected: 5,
    problems: 1,
  },
  {
    topic: "二分查找",
    mastery: 0.7,
    coverage: 0.5,
    bucket: "strong-sparse",
    label: "偶然会，没练透",
    action: "多练同类型题",
    attempts: 2,
    expected: 5,
    problems: 2,
  },
]

const SERIES = [
  { date: "2026-11-01", count: 0 },
  { date: "2026-11-02", count: 3 },
  { date: "2026-11-03", count: 1 },
]

/* ---------------- 基础 ---------------- */

test("percent 与 charts.formatPercent 同口径", function () {
  assert.strictEqual(Diagnose.percent(0.42), Charts.formatPercent(0.42))
  assert.strictEqual(Diagnose.percent(0.42), "42%")
  assert.strictEqual(Diagnose.percent(undefined), "0%")
})

test("bucketCounts 数出四个档位各有多少个知识点", function () {
  const counts = Diagnose.bucketCounts(TOPICS)
  assert.strictEqual(counts["weak-sparse"], 1)
  assert.strictEqual(counts["strong-dense"], 1)
  assert.strictEqual(counts["weak-dense"], 1)
  assert.strictEqual(counts["strong-sparse"], 1)
})

test("bucketCounts 遇到不认识的档位不炸", function () {
  const counts = Diagnose.bucketCounts([{ bucket: "不存在的档" }, null])
  assert.strictEqual(counts["weak-dense"], 0)
})

test("legendItems 的颜色跟 charts 的主题色一致", function () {
  const items = Diagnose.legendItems()
  assert.strictEqual(items.length, 2)
  assert.strictEqual(items[0].name, "掌握度")
  assert.strictEqual(items[0].color, Charts.DEFAULT_THEME.series1)
  assert.strictEqual(items[1].name, "覆盖度")
  assert.strictEqual(items[1].color, Charts.DEFAULT_THEME.series2)
})

test("summaryCards 把概览数字变成标签 + 数值", function () {
  const cards = Diagnose.summaryCards({
    topicCount: 4,
    avgMastery: 0.5,
    avgCoverage: 0.4,
    weakCount: 2,
    totalAttempts: 17,
    totalProblems: 9,
    untouchedTopics: 3,
    minExpectedQuestions: 5,
  })
  const byLabel = {}
  cards.forEach(function (card) {
    byLabel[card.label] = card.value
  })
  assert.strictEqual(byLabel["知识点"], "4")
  assert.strictEqual(byLabel["平均掌握度"], "50%")
  assert.strictEqual(byLabel["平均覆盖度"], "40%")
  assert.strictEqual(byLabel["薄弱知识点"], "2")
  assert.strictEqual(byLabel["总提交数"], "17")
  assert.strictEqual(byLabel["练过题数"], "9")
  assert.strictEqual(byLabel["未涉及的知识点"], "3")
  assert.strictEqual(byLabel["期望题量下限"], "5")
})

/* ---------------- 选数据 ---------------- */

test("radarTopics 选提交次数最多的几个知识点", function () {
  const rows = Diagnose.radarTopics(TOPICS, 2)
  assert.strictEqual(rows.length, 2)
  assert.strictEqual(rows[0].topic, "数组")
  assert.strictEqual(rows[1].topic, "哈希表")
})

test("radarTopics 提交次数相同时按知识点名排，顺序稳定", function () {
  const rows = Diagnose.radarTopics(
    [
      { topic: "乙", mastery: 0.1, coverage: 0.1, attempts: 3 },
      { topic: "甲", mastery: 0.1, coverage: 0.1, attempts: 3 },
    ],
    5
  )
  const names = rows.map(function (row) {
    return row.topic
  })
  // 中文按码位排（跟 Array.prototype.sort 一致），不按拼音硬编码顺序
  assert.deepStrictEqual(names, names.slice().sort())
  assert.strictEqual(names.length, 2)
})

test("radarTopics 的值被夹到 0..1", function () {
  const rows = Diagnose.radarTopics([{ topic: "x", mastery: 1.5, coverage: -1, attempts: 1 }], 5)
  assert.strictEqual(rows[0].mastery, 1)
  assert.strictEqual(rows[0].coverage, 0)
})

test("weakRows 只收两个弱档，并按处置优先级排", function () {
  const rows = Diagnose.weakRows(TOPICS, 10)
  assert.strictEqual(rows.length, 2)
  assert.strictEqual(rows[0].topic, "动态规划", "练过但总错排最前")
  assert.strictEqual(rows[0].label, "练过但总错")
  assert.strictEqual(rows[1].topic, "数组")
  assert.strictEqual(rows[1].display, "28%")
})

test("weakRows 超过上限就截断", function () {
  const many = []
  for (let i = 0; i < 12; i += 1) {
    many.push({
      topic: "t" + i,
      mastery: 0.1,
      coverage: 0.1,
      bucket: "weak-sparse",
      label: "薄弱且练得少",
      action: "",
      attempts: 1,
    })
  }
  assert.strictEqual(Diagnose.weakRows(many, 8).length, 8)
})

test("coverageRows 按覆盖度从低到高排，并带上「练了几 / 期望几」", function () {
  const rows = Diagnose.coverageRows(TOPICS, 10)
  assert.strictEqual(rows.length, 4)
  assert.strictEqual(rows[0].topic, "动态规划")
  assert.strictEqual(rows[0].coverage, 0.2)
  assert.strictEqual(rows[0].detail, "练了 1 / 期望 5 题")
  assert.strictEqual(rows[3].topic, "哈希表")
})

/* ---------------- 视图 ---------------- */

test("buildDiagnoseView 出四块内容：雷达、弱项榜、曲线、覆盖", function () {
  const view = Diagnose.buildDiagnoseView({
    topics: TOPICS,
    summary: { topicCount: 4, avgMastery: 0.5 },
    series: SERIES,
  })

  assert.strictEqual(view.radar.labels.length, 4)
  assert.strictEqual(view.radar.series.length, 2)
  assert.strictEqual(view.radar.series[0].name, "掌握度")
  assert.strictEqual(view.radar.series[1].name, "覆盖度")
  assert.strictEqual(view.radar.series[0].values.length, 4)

  assert.strictEqual(view.weak.rows.length, 2)
  assert.strictEqual(view.curve.points.length, 3)
  assert.strictEqual(view.curve.points[1].count, 3)
  assert.strictEqual(view.coverage.rows.length, 4)

  // 每块都要有表格孪生
  ;[view.radar, view.weak, view.curve, view.coverage].forEach(function (block) {
    assert.ok(Array.isArray(block.table.columns), "缺表格列")
    assert.ok(Array.isArray(block.table.rows), "缺表格行")
    assert.strictEqual(block.table.rows.length, block.table.rows.length)
  })
})

test("buildDiagnoseView 表格孪生的行数跟图表行数对得上", function () {
  const view = Diagnose.buildDiagnoseView({ topics: TOPICS, summary: {}, series: SERIES })
  assert.strictEqual(view.radar.table.rows.length, view.radar.labels.length)
  assert.strictEqual(view.weak.table.rows.length, view.weak.rows.length)
  assert.strictEqual(view.curve.table.rows.length, view.curve.points.length)
  assert.strictEqual(view.coverage.table.rows.length, view.coverage.rows.length)
})

test("buildDiagnoseView 空数据不炸", function () {
  const view = Diagnose.buildDiagnoseView({})
  assert.deepStrictEqual(view.radar.labels, [])
  assert.deepStrictEqual(view.weak.rows, [])
  assert.deepStrictEqual(view.curve.points, [])
  assert.deepStrictEqual(view.coverage.rows, [])
  assert.strictEqual(view.summary.length, Diagnose.summaryCards({}).length)
})

/* ---------------- 静态资源接线 ---------------- */

test("index.html 引入 charts.js / diagnose.js / diagnose.css", function () {
  const html = fs.readFileSync(path.join(__dirname, "..", "web", "index.html"), "utf8")
  assert.ok(html.indexOf('src="/charts.js"') !== -1, "少 charts.js")
  assert.ok(html.indexOf('src="/diagnose.js"') !== -1, "少 diagnose.js")
  assert.ok(html.indexOf('href="/diagnose.css"') !== -1, "少 diagnose.css")
  assert.ok(
    html.indexOf('src="/charts.js"') < html.indexOf('src="/diagnose.js"'),
    "charts.js 必须先加载"
  )
})

test("diagnose.css 存在且定义了诊断页要用的类名", function () {
  const css = fs.readFileSync(path.join(__dirname, "..", "web", "diagnose.css"), "utf8")
  assert.ok(css.indexOf(".diag-panel") !== -1)
  assert.ok(css.indexOf(".diag-card") !== -1)
  assert.ok(css.indexOf(".chart") !== -1)
})

/* ---------------- 页面注册 ---------------- */

test("页面脚本注册进 window.AlgoPlan.pages", function () {
  const page = global.window.AlgoPlan.pages.diagnose
  assert.ok(page, "诊断页没有注册")
  assert.strictEqual(page.title, "诊断")
  assert.strictEqual(typeof page.render, "function", "诊断页缺少 render")
})

test("web/app.js 的 TABS 里有「诊断」这一项", function () {
  const app = fs.readFileSync(path.join(__dirname, "..", "web", "app.js"), "utf8")
  assert.ok(
    /\{\s*key:\s*"diagnose"\s*,\s*label:\s*"诊断"\s*\}/.test(app),
    "TABS 里没有诊断页"
  )
})
