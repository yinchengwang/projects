"use strict"
/**
 * diagnose-e2e.test.js —— 诊断的端到端验收
 *
 * 链路：造作答流水 → 起真服务 → 打 /api/insights → 校验响应形状 →
 * 校验静态资源取得到 → 把真实响应喂给前端的视图函数，确认画出来的 SVG 干净。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const Charts = require("../web/charts.js")
const { buildDiagnoseView } = require("../web/diagnose.js")

/** 三道题：数组挂两个知识点，动态规划单独一个 */
const BANK = [
  {
    file: "lc-00001.md",
    id: "lc-00001",
    title: "两数之和",
    topics: "[数组, 哈希表]",
  },
  {
    file: "lc-00002.md",
    id: "lc-00002",
    title: "合并区间",
    topics: "[数组]",
  },
  {
    file: "lc-00003.md",
    id: "lc-00003",
    title: "爬楼梯",
    topics: "[动态规划]",
  },
]

/** 造一个带题库的临时 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-diag-e2e-"))
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  BANK.forEach(function (item) {
    const body = [
      "---",
      "id: " + item.id,
      "source: leetcode",
      "title: " + item.title,
      "difficulty: easy",
      "topics: " + item.topics,
      "tracks: [interview]",
      "status: todo",
      "needs_fill: false",
      "created: 2026-11-11",
      "updated: 2026-11-11",
      "---",
      "## 题面",
      "",
      "题目正文。",
      "",
    ].join("\n")
    fs.writeFileSync(path.join(dir, item.file), body, "utf8")
  })
  return root
}

/** 起服务并塞四条流水。 */
const NOW = Date.now()

/** 几小时之前的时间戳 */
function isoAgo(hours) {
  return new Date(NOW - hours * 3600 * 1000).toISOString()
}

async function startServer() {
  // localBank: null —— 不加载工程仓内置的 Codeforces 题库，只用这个临时 learning 仓。
  // 否则「没练过的知识点」会连 CF 那 51 道题的一起算进来，数字就不确定了。
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning(), localBank: null })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port

  function add(problemId, hoursAgo, verdict) {
    app.store.insertAttempt({
      problem_id: problemId,
      ts: isoAgo(hoursAgo),
      language: "cpp",
      code: "int main(){}",
      verdict: verdict,
      passed_cases: verdict === "pass" ? 2 : 0,
      total_cases: 2,
      duration_ms: 90,
      mode: "solve",
    })
  }

  // lc-00001：先过、再错一次
  add("lc-00001", 4, "pass")
  add("lc-00001", 3, "wrong_answer")
  // lc-00002：先错、再过
  add("lc-00002", 2, "wrong_answer")
  add("lc-00002", 1, "pass")

  return { app: app, base: base }
}

/* ---------------- 接口 ---------------- */

test("端到端：GET /api/insights 返回契约要求的三个键", async function () {
  const { app, base } = await startServer()
  try {
    const res = await fetch(base + "/api/insights")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(Array.isArray(body.topics))
    assert.ok(body.summary)
    assert.ok(Array.isArray(body.series))
  } finally {
    await app.close()
  }
})

test("端到端：知识点统计与作答流水对得上", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/insights")).json()
    assert.strictEqual(body.topics.length, 2, "没练过的动态规划不该出现在 topics 里")

    const array = body.topics.find(function (t) {
      return t.topic === "数组"
    })
    assert.strictEqual(array.attempts, 4)
    assert.strictEqual(array.firstPass, 1, "lc-00002 首次没通过，不算首次通过")
    assert.strictEqual(array.lapses, 2)
    assert.strictEqual(array.unitCount, 2)
    assert.strictEqual(array.expected, 5, "题库只有 2 道，按下限取 5")
    assert.strictEqual(array.expectedSource, "floor")
    assert.ok(Math.abs(array.coverage - 0.4) < 1e-9, "实际 " + array.coverage)
    assert.strictEqual(array.bucket, "weak-sparse")
    assert.strictEqual(array.label, "薄弱且练得少")

    const hash = body.topics.find(function (t) {
      return t.topic === "哈希表"
    })
    assert.strictEqual(hash.attempts, 2)
    assert.strictEqual(hash.firstPass, 1)
    assert.strictEqual(hash.lapses, 1)
    assert.strictEqual(hash.unitCount, 1)
    assert.ok(Math.abs(hash.coverage - 0.2) < 1e-9, "实际 " + hash.coverage)
    assert.strictEqual(hash.bucket, "strong-sparse")
    assert.strictEqual(hash.label, "偶然会，没练透")

    // 掌握度不会因为一道题挂两个知识点而翻倍
    assert.ok(hash.mastery > array.mastery, "哈希表掌握度应该高于数组")
  } finally {
    await app.close()
  }
})

test("端到端：summary 的总数按原始流水算，不按知识点加总", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/insights")).json()
    assert.strictEqual(body.summary.totalAttempts, 4)
    assert.strictEqual(body.summary.totalProblems, 2)
    assert.strictEqual(body.summary.untouchedTopics, 1, "动态规划没练过")
    assert.strictEqual(body.summary.topicCount, 2)
  } finally {
    await app.close()
  }
})

test("端到端：每日题量曲线按天分组，窗口内缺失的日期补 0", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/insights")).json()
    assert.strictEqual(body.series.length, 30)
    const total = body.series.reduce(function (sum, row) {
      return sum + row.count
    }, 0)
    assert.strictEqual(total, 4, "四条流水都要在曲线里")
    body.series.forEach(function (row) {
      assert.ok(row.count >= 0)
      assert.ok(/^\d{4}-\d{2}-\d{2}$/.test(row.date))
    })
    assert.strictEqual(body.summary.windowAttempts, 4)

    // ?days=7 缩小窗口
    const short = await (await fetch(base + "/api/insights?days=7")).json()
    assert.strictEqual(short.series.length, 7)
    assert.strictEqual(short.summary.windowDays, 7)
  } finally {
    await app.close()
  }
})

/* ---------------- P3：GET /api/diagnose 三视图 ---------------- */

test("端到端：GET /api/diagnose?view=mastery 返回三轨与知识点，形状照契约", async function () {
  const { app, base } = await startServer()
  try {
    const res = await fetch(base + "/api/diagnose?view=mastery")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.view, "mastery")
    assert.deepStrictEqual(
      body.tracks.map(function (track) {
        return track.track
      }),
      ["algorithm", "database", "ai"],
      "三轨顺序固定"
    )
    assert.ok(Array.isArray(body.topics))
    assert.ok(body.summary)
    assert.strictEqual(typeof body.summary.unitCount, "number")
  } finally {
    await app.close()
  }
})

test("端到端：GET /api/diagnose?view=radar 返回固定顺序的六个维度", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/diagnose?view=radar")).json()
    assert.strictEqual(body.view, "radar")
    assert.deepStrictEqual(
      body.axes.map(function (axis) {
        return axis.id
      }),
      ["algorithm", "database", "ai", "c", "python", "sql"]
    )
    assert.ok(Array.isArray(body.weak))
    assert.ok(Array.isArray(body.strong))
  } finally {
    await app.close()
  }
})

test("端到端：GET /api/diagnose?view=heatmap 返回会话与格子", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/diagnose?view=heatmap")).json()
    assert.strictEqual(body.view, "heatmap")
    assert.ok(Array.isArray(body.sessions))
    assert.ok(Array.isArray(body.cells))
  } finally {
    await app.close()
  }
})

test("端到端：视图名不认识就返回 400 与中文错误", async function () {
  const { app, base } = await startServer()
  try {
    const res = await fetch(base + "/api/diagnose?view=bogus")
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.strictEqual(body.error, "诊断视图只能是 mastery / radar / heatmap")
  } finally {
    await app.close()
  }
})

test("端到端：数值都在 0~1，没练过的知识点不出现", async function () {
  const { app, base } = await startServer()
  try {
    const body = await (await fetch(base + "/api/diagnose?view=mastery")).json()
    body.tracks.forEach(function (track) {
      assert.ok(track.mastery >= 0 && track.mastery <= 1, "掌握度要在 0~1：" + track.mastery)
      assert.ok(track.coverage >= 0 && track.coverage <= 1, "覆盖度要在 0~1：" + track.coverage)
      if (track.coverage > 0) {
        assert.ok(track.mastery > 0, "练过的轨掌握度不能是 0：" + track.track)
      }
    })
    body.topics.forEach(function (topic) {
      assert.ok(topic.mastery >= 0 && topic.mastery <= 1, "知识点掌握度要在 0~1")
      assert.ok(topic.coverage > 0, "出现在 topics 里的知识点一定是练过的：" + topic.topic)
      assert.ok(topic.label, "知识点要带四档的中文标签")
      assert.ok(topic.action, "知识点要带处方")
    })
    const names = body.topics.map(function (topic) {
      return topic.topic
    })
    assert.ok(names.indexOf("动态规划") === -1, "没练过的动态规划不该出现在 topics 里")
  } finally {
    await app.close()
  }
})

/* ---------------- 静态资源 ---------------- */

test("端到端：charts.js / diagnose.js / diagnose.css 都取得到", async function () {
  const { app, base } = await startServer()
  try {
    const charts = await fetch(base + "/charts.js")
    assert.strictEqual(charts.status, 200)
    assert.ok((await charts.text()).indexOf("radarSvg") !== -1)

    const diagnose = await fetch(base + "/diagnose.js")
    assert.strictEqual(diagnose.status, 200)
    assert.ok((await diagnose.text()).indexOf("buildDiagnoseView") !== -1)

    const css = await fetch(base + "/diagnose.css")
    assert.strictEqual(css.status, 200)
    assert.ok((await css.text()).indexOf(".diag-panel") !== -1)
  } finally {
    await app.close()
  }
})

test("端到端：首页引用了诊断页的资源", async function () {
  const { app, base } = await startServer()
  try {
    const html = await (await fetch(base + "/")).text()
    assert.ok(html.indexOf('src="/charts.js"') !== -1)
    assert.ok(html.indexOf('src="/diagnose.js"') !== -1)
    assert.ok(html.indexOf('href="/diagnose.css"') !== -1)
    assert.ok(html.indexOf('src="/charts.js"') < html.indexOf('src="/diagnose.js"'))
  } finally {
    await app.close()
  }
})

/* ---------------- 前端闭环 ---------------- */

test("端到端：真实响应喂给前端，出来的 SVG 干净", async function () {
  const { app, base } = await startServer()
  try {
    const radarBody = await (await fetch(base + "/api/diagnose?view=radar")).json()
    const radarView = buildDiagnoseView(radarBody, "radar")
    const radar = Charts.radarSvg({
      labels: radarView.axes.map(function (axis) {
        return axis.label
      }),
      series: [
        {
          name: "掌握度",
          values: radarView.axes.map(function (axis) {
            return axis.value
          }),
        },
      ],
      title: radarView.title,
    })

    const heatBody = await (await fetch(base + "/api/diagnose?view=heatmap")).json()
    const heatView = buildDiagnoseView(heatBody, "heatmap")
    const heat = Charts.heatmapSvg({
      sessions: heatView.sessions,
      cells: heatView.cells,
      tracks: ["algorithm", "database", "ai"],
      title: heatView.title,
    })

    const masteryBody = await (await fetch(base + "/api/diagnose?view=mastery")).json()
    const masteryView = buildDiagnoseView(masteryBody, "mastery")
    const bars = Charts.barSvg({
      rows: masteryView.tracks.map(function (track) {
        return { label: track.label, value: track.mastery, display: track.display }
      }),
      max: 1,
      title: masteryView.title,
    })

    ;[radar, heat, bars].forEach(function (svg) {
      assert.ok(svg.indexOf("NaN") === -1, "SVG 里出现了 NaN")
      assert.ok(svg.indexOf("undefined") === -1, "SVG 里出现了 undefined")
      assert.ok(svg.indexOf("Infinity") === -1, "SVG 里出现了 Infinity")
    })

    assert.ok(radar.indexOf("<title>") !== -1, "雷达图要有标题")
    assert.ok(
      heat.indexOf("还没有会话记录") !== -1 || heat.indexOf("heatmap-cell") !== -1,
      "热力图要么是空态文案，要么有格子"
    )
  } finally {
    await app.close()
  }
})

/* ---------------- 文档同步 ---------------- */

test("README 写清楚了诊断接口与期望题量口径", function () {
  const readme = fs.readFileSync(path.join(__dirname, "..", "README.md"), "utf8")
  assert.ok(readme.indexOf("/api/insights") !== -1, "README 里没有写 /api/insights")
  assert.ok(readme.indexOf("期望题量") !== -1, "README 里没有写期望题量口径")
  assert.ok(readme.indexOf("insights.expected_min_questions") !== -1, "README 里没有写可配置的键名")
  assert.ok(readme.indexOf("/api/diagnose") !== -1, "README 里没有写 /api/diagnose")
  assert.ok(readme.indexOf("三轨总览") !== -1, "README 里没有写诊断页的三个视图")
})
