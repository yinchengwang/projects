"use strict"
/**
 * diagnose.test.js —— 诊断页三视图的纯函数层
 *
 * 渲染层要真 DOM（Node 里没有），所以这里只测纯函数：
 * 视图数据怎么算、怎么排、截断到几条、hero 说哪句话。
 */

const test = require("node:test")
const assert = require("node:assert")

const Charts = require("../web/charts.js")

/**
 * 页面脚本在浏览器里跑，但「注册进页面表」这一步可以在 Node 里验：
 * 给一个最小的 window 替身，把页面脚本 require 进来。
 * 顺序很重要：先搭好替身，再 require——脚本加载时就会注册自己。
 *
 * P1 把前端全局改名成 StudyHub（契约 §13），旧名 AlgoPlan 也留一份替身，
 * 顺便验一下「认新名」这件事。
 */
global.window = {
  StudyHub: {
    pages: {},
    el: function (tag, attrs, children) {
      return { tag: tag, attrs: attrs, children: children }
    },
    getJson: async function () {
      return { view: "mastery", tracks: [], topics: [], summary: {} }
    },
  },
  AlgoPlan: { pages: {} },
  StudyHubCharts: Charts,
  AlgoPlanCharts: Charts,
}

const Diagnose = require("../web/diagnose.js")

/** 一份固定的 mastery 视图数据：三轨 + 知识点 */
const MASTERY_PAYLOAD = {
  view: "mastery",
  tracks: [
    {
      track: "algorithm",
      label: "算法",
      mastery: 0.57,
      coverage: 0.4,
      bucket: "weak-dense",
      label2: "练过但总错",
      action: "先回去看概念笔记，别急着刷题——量够了，理解没到位",
      unitCount: 51,
    },
    {
      track: "database",
      label: "数据库",
      mastery: 0.97,
      coverage: 0.2,
      bucket: "strong-sparse",
      label2: "偶然会，没练透",
      action: "安排复习巩固——会做，但还没练透",
      unitCount: 12,
    },
    {
      track: "ai",
      label: "AI",
      mastery: 0,
      coverage: 0,
      bucket: "weak-sparse",
      label2: "薄弱且练得少",
      action: "轮次里优先排这类题——先补量",
      unitCount: 8,
    },
  ],
  topics: [
    {
      track: "algorithm",
      topic: "数组",
      mastery: 0.57,
      coverage: 0.4,
      bucket: "weak-dense",
      label: "练过但总错",
      action: "先回去看概念笔记",
    },
    {
      track: "algorithm",
      topic: "SQL",
      mastery: 0.18,
      coverage: 0.2,
      bucket: "weak-sparse",
      label: "薄弱且练得少",
      action: "先补量",
    },
    {
      track: "database",
      topic: "事务",
      mastery: 0.97,
      coverage: 0.2,
      bucket: "strong-sparse",
      label: "偶然会，没练透",
      action: "安排复习巩固",
    },
  ],
  summary: { unitCount: 3, topicCount: 3, sessionCount: 5, totalAttempts: 12 },
}

/** 一份固定的 radar 视图数据：六轴 + 弱项 + 强项 */
const RADAR_PAYLOAD = {
  view: "radar",
  axes: [
    { id: "algorithm", label: "算法", value: 0.57 },
    { id: "database", label: "数据库", value: 0.97 },
    { id: "ai", label: "AI", value: 0 },
    { id: "c", label: "C/C++", value: 0.57 },
    { id: "python", label: "Python", value: 0.97 },
    { id: "sql", label: "SQL", value: 0.18 },
  ],
  weak: [
    { unit_id: "algo:lc-00002", track: "algorithm", title: "合并区间", mastery: 0.18, topic: "数组" },
  ],
  strong: [
    { unit_id: "db:mvcc", track: "database", title: "MVCC", mastery: 0.97, topic: "事务" },
    { unit_id: "algo:lc-00001", track: "algorithm", title: "两数之和", mastery: 0.96, topic: "数组" },
  ],
}

/** 一份固定的 heatmap 视图数据：两个会话 + 三个格子 */
const HEATMAP_PAYLOAD = {
  view: "heatmap",
  sessions: [
    { id: "s1", index: 1, started_at: "2026-11-09T10:00:00.000Z" },
    { id: "s2", index: 2, started_at: "2026-11-10T10:00:00.000Z" },
  ],
  cells: [
    { session_id: "s1", track: "algorithm", state: "pass", seq: 1 },
    { session_id: "s1", track: "database", state: "pending", seq: 1 },
    { session_id: "s2", track: "algorithm", state: "wrong_answer", seq: 1 },
  ],
}

/* ---------------- 三轨总览 ---------------- */

test("三轨并列：trackBarRows 三行、顺序固定、百分比按掌握度", function () {
  const rows = Diagnose.trackBarRows(MASTERY_PAYLOAD)
  assert.strictEqual(rows.length, 3)
  assert.deepStrictEqual(
    rows.map(function (r) {
      return r.track
    }),
    ["algorithm", "database", "ai"]
  )
  assert.strictEqual(rows[0].label, "算法")
  assert.strictEqual(rows[0].display, "57%")
  assert.strictEqual(rows[1].display, "97%")
})

test("下钻明细只给选中那条轨的知识点，按掌握度从低到高", function () {
  const rows = Diagnose.topicRowsForTrack(MASTERY_PAYLOAD, "algorithm")
  assert.strictEqual(rows.length, 2)
  assert.deepStrictEqual(
    rows.map(function (r) {
      return r.topic
    }),
    ["SQL", "数组"]
  )
  assert.strictEqual(rows[0].display, "18%")
})

test("下钻的条形颜色按档位给：弱档橙、强档绿", function () {
  const algo = Diagnose.topicRowsForTrack(MASTERY_PAYLOAD, "algorithm")
  assert.strictEqual(algo[0].color, Charts.DEFAULT_THEME.series2, "薄弱且练得少是橙")
  const db = Diagnose.topicRowsForTrack(MASTERY_PAYLOAD, "database")
  assert.strictEqual(db[0].color, Charts.DEFAULT_THEME.series3, "偶然会没练透是绿")
})

test("概览数字块给四个数", function () {
  const cards = Diagnose.summaryCards(MASTERY_PAYLOAD)
  assert.deepStrictEqual(
    cards.map(function (c) {
      return c.label
    }),
    ["练过单元", "练过知识点", "会话数", "总提交数"]
  )
  assert.strictEqual(cards[0].value, "3")
  assert.strictEqual(cards[3].value, "12")
})

test("hero 挑覆盖度大于 0 的轨里掌握度最低的那条", function () {
  const view = Diagnose.buildMasteryView(MASTERY_PAYLOAD)
  assert.strictEqual(view.hero.title, "先治这一条轨")
  assert.ok(view.hero.text.indexOf("算法") === 0, view.hero.text)
  assert.ok(view.hero.text.indexOf("57%") !== -1, view.hero.text)
})

test("一条轨都没练过时 hero 给中文空态", function () {
  const view = Diagnose.buildMasteryView({ view: "mastery", tracks: [], topics: [], summary: {} })
  assert.strictEqual(view.hero.title, "还没有练习记录")
  assert.ok(view.hero.text.indexOf("先去做几道题") !== -1, view.hero.text)
})

test("buildMasteryView 的形状：view / tracks / drilldown / table", function () {
  const view = Diagnose.buildMasteryView(MASTERY_PAYLOAD)
  assert.strictEqual(view.view, "mastery")
  assert.strictEqual(view.tracks.length, 3)
  assert.strictEqual(view.drilldown.length, 3)
  assert.deepStrictEqual(
    view.drilldown.map(function (d) {
      return d.track
    }),
    ["algorithm", "database", "ai"]
  )
  assert.deepStrictEqual(view.table.columns, ["轨", "掌握度", "覆盖度", "诊断", "单元数"])
  assert.strictEqual(view.table.rows.length, 3)
})

/* ---------------- 六轴雷达 ---------------- */

test("雷达六轴：顺序与百分比", function () {
  const axes = Diagnose.radarAxes(RADAR_PAYLOAD)
  assert.deepStrictEqual(
    axes.map(function (a) {
      return a.id
    }),
    ["algorithm", "database", "ai", "c", "python", "sql"]
  )
  assert.strictEqual(axes[0].display, "57%")
  assert.strictEqual(axes[2].display, "0%")
})

test("弱项清单带上轨名与中文建议", function () {
  const rows = Diagnose.weakRows(RADAR_PAYLOAD)
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].unit_id, "algo:lc-00002")
  assert.strictEqual(rows[0].trackLabel, "算法")
  assert.strictEqual(rows[0].title, "合并区间")
  assert.ok(rows[0].advice.indexOf("18%") !== -1, rows[0].advice)
  assert.ok(rows[0].advice.indexOf("笔记") !== -1, rows[0].advice)
})

test("强项清单带上轨名", function () {
  const rows = Diagnose.strongRows(RADAR_PAYLOAD)
  assert.strictEqual(rows.length, 2)
  assert.deepStrictEqual(
    rows.map(function (r) {
      return r.unit_id
    }),
    ["db:mvcc", "algo:lc-00001"]
  )
  assert.strictEqual(rows[0].trackLabel, "数据库")
})

test("雷达 hero 挑最短的那块板", function () {
  const view = Diagnose.buildRadarView(RADAR_PAYLOAD)
  assert.strictEqual(view.hero.title, "最短的那块板")
  assert.ok(view.hero.text.indexOf("SQL") === 0, view.hero.text)
  assert.ok(view.hero.text.indexOf("18%") !== -1, view.hero.text)
})

test("buildRadarView 的形状", function () {
  const view = Diagnose.buildRadarView(RADAR_PAYLOAD)
  assert.strictEqual(view.view, "radar")
  assert.strictEqual(view.axes.length, 6)
  assert.strictEqual(view.weak.length, 1)
  assert.strictEqual(view.strong.length, 2)
  assert.deepStrictEqual(view.table.columns, ["维度", "掌握度"])
})

/* ---------------- 会话热力图 ---------------- */

test("热力图按会话分组，格子带汉字标记与中文全称", function () {
  const view = Diagnose.buildHeatmapView(HEATMAP_PAYLOAD)
  assert.strictEqual(view.sessionRows.length, 2)
  const first = view.sessionRows[0]
  assert.strictEqual(first.id, "s1")
  assert.strictEqual(first.index, 1)
  assert.strictEqual(first.cells.length, 2)
  assert.strictEqual(first.cells[0].mark, "通")
  assert.strictEqual(first.cells[0].label, "通过")
  assert.strictEqual(first.cells[0].trackLabel, "算法")
  assert.strictEqual(first.cells[1].mark, "待")
})

test("热力图图例六项，颜色只分组", function () {
  const legend = Diagnose.legendItems()
  assert.strictEqual(legend.length, 6)
  assert.strictEqual(legend[0].mark, "通")
  assert.strictEqual(legend[0].label, "通过")
  assert.strictEqual(legend[0].color, Charts.DEFAULT_THEME.series3)
  assert.strictEqual(legend[1].color, Charts.DEFAULT_THEME.series2)
  assert.strictEqual(legend[5].color, Charts.DEFAULT_THEME.track)
})

test("热力图 hero 说最近一次会话的通过数", function () {
  const view = Diagnose.buildHeatmapView(HEATMAP_PAYLOAD)
  assert.strictEqual(view.hero.title, "最近一次会话")
  assert.ok(view.hero.text.indexOf("第 2 次会话") !== -1, view.hero.text)
  assert.ok(view.hero.text.indexOf("通过 0 个") !== -1, view.hero.text)
})

test("热力图的表格孪生逐格列出", function () {
  const view = Diagnose.buildHeatmapView(HEATMAP_PAYLOAD)
  assert.deepStrictEqual(view.table.columns, ["会话", "轨", "结果", "序号"])
  assert.strictEqual(view.table.rows.length, 3)
  assert.deepStrictEqual(view.table.rows[0], ["1", "算法", "通过", "1"])
})

/* ---------------- 分发与注册 ---------------- */

test("buildDiagnoseView 按视图名分发，非法名回落到总览", function () {
  assert.strictEqual(Diagnose.buildDiagnoseView(MASTERY_PAYLOAD, "mastery").view, "mastery")
  assert.strictEqual(Diagnose.buildDiagnoseView(RADAR_PAYLOAD, "radar").view, "radar")
  assert.strictEqual(Diagnose.buildDiagnoseView(HEATMAP_PAYLOAD, "heatmap").view, "heatmap")
  assert.strictEqual(Diagnose.buildDiagnoseView(MASTERY_PAYLOAD, "bogus").view, "mastery")
})

test("页面注册进 window.StudyHub.pages.diagnose", function () {
  assert.ok(global.window.StudyHub.pages.diagnose, "要注册进页面表")
  assert.strictEqual(global.window.StudyHub.pages.diagnose.title, "诊断")
  assert.strictEqual(typeof global.window.StudyHub.pages.diagnose.render, "function")
})
