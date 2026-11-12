"use strict"
/**
 * insights.test.js —— 诊断数据装配 + GET /api/insights 接口
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { openDb, createStore } = require("../server/db.js")
const {
  buildInsights,
  buildSeries,
  addDays,
  normalizeDays,
  DEFAULT_DAYS,
  MAX_DAYS,
} = require("../server/insights.js")
const { createApp } = require("../server/app.js")

/** 固定时刻，断言可以写死数字 */
const NOW = "2026-11-11T00:00:00.000Z"

/** 假题库：三道题，一个知识点从没练过 */
const PROBLEMS = [
  { id: "lc-00001", topics: ["数组", "哈希表"] },
  { id: "lc-00002", topics: ["数组"] },
  { id: "lc-00003", topics: ["动态规划"] },
]

function fakeProvider() {
  return {
    list: async function () {
      return PROBLEMS
    },
  }
}

function freshStore() {
  return createStore(openDb(":memory:"))
}

/** 插一条作答流水 */
function addAttempt(store, problemId, ts, verdict) {
  store.insertAttempt({
    problem_id: problemId,
    ts: ts,
    language: "cpp",
    code: "int main(){}",
    verdict: verdict,
    passed_cases: verdict === "pass" ? 2 : 0,
    total_cases: 2,
    duration_ms: 100,
    mode: "solve",
  })
}

/**
 * 造一份固定流水：
 *   lc-00001 先过（首次通过）+1 次没通过 → 数组/哈希表 各 firstPass 1
 *   lc-00002 先错（首次不算通过）+1 次过   → 数组 firstPass 仍为 1
 * 于是：数组 attempts 4 / firstPass 1 / lapses 2；哈希表 attempts 2 / firstPass 1 / lapses 1
 */
function seedAttempts(store) {
  addAttempt(store, "lc-00001", "2026-11-10T08:00:00.000Z", "pass")
  addAttempt(store, "lc-00001", "2026-11-10T09:00:00.000Z", "wrong_answer")
  addAttempt(store, "lc-00002", "2026-11-09T08:00:00.000Z", "wrong_answer")
  addAttempt(store, "lc-00002", "2026-11-09T09:00:00.000Z", "pass")
}

/* ---------------- server/insights.js ---------------- */

test("buildInsights 把派生统计物化回 topics 表", async function () {
  const store = freshStore()
  seedAttempts(store)
  await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const rows = store.listTopics()
  assert.strictEqual(rows.length, 2)
  const array = rows.find(function (r) { return r.id === "数组" })
  assert.strictEqual(array.attempts, 4)
  assert.strictEqual(array.first_pass, 1)
  assert.strictEqual(array.lapses, 2)
  assert.strictEqual(array.last_at, "2026-11-10T09:00:00.000Z")
})

test("topics 表是物化视图：反复调接口不会把数字累加上去", async function () {
  const store = freshStore()
  seedAttempts(store)
  await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })
  const first = store.listTopics().map(function (r) { return r.attempts })
  await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })
  await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })
  const after = store.listTopics().map(function (r) { return r.attempts })
  assert.deepStrictEqual(after, first, "topics 表被累加了——replaceTopics 应该是整体覆盖")
})

test("buildInsights 返回的每个知识点带齐契约要求的字段", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  assert.ok(Array.isArray(insights.topics))
  assert.ok(insights.summary)
  assert.ok(Array.isArray(insights.series))

  const item = insights.topics.find(function (t) { return t.topic === "数组" })
  ;["topic", "mastery", "coverage", "bucket", "label", "attempts", "firstPass", "lapses", "lastAt"].forEach(
    function (key) {
      assert.ok(key in item, "缺少字段：" + key)
    }
  )
  assert.strictEqual(item.attempts, 4)
  assert.strictEqual(item.firstPass, 1)
  assert.strictEqual(item.lapses, 2)
  assert.strictEqual(item.problems, 2)
})

test("掌握度按公式算：首次通过率 + 遗忘因子", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const array = insights.topics.find(function (t) { return t.topic === "数组" })
  // 首次通过率 1/4 = 0.25；失误 2 次 → e^-1 = 0.367879
  // base = 0.7×0.25 + 0.3×0.367879 = 0.285364
  // 距上次练习 2026-11-10T09:00 → 15 小时 = 0.625 天 → decay = 0.5^(0.625/14)
  const expected = (0.7 * 0.25 + 0.3 * Math.exp(-1)) * Math.pow(0.5, 0.625 / 14)
  assert.ok(Math.abs(array.mastery - expected) < 1e-6, "实际 " + array.mastery + " 期望 " + expected)
})

test("覆盖度用练过的题目数除以期望题量", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const array = insights.topics.find(function (t) { return t.topic === "数组" })
  // 题库里「数组」只有 2 道题 < 下限 5 → 期望 5；练过 2 道 → 覆盖 0.4
  assert.strictEqual(array.expected, 5)
  assert.strictEqual(array.expectedSource, "floor")
  assert.ok(Math.abs(array.coverage - 0.4) < 1e-9, "实际 " + array.coverage)
})

test("settings 里的覆盖值能改期望题量", async function () {
  const store = freshStore()
  seedAttempts(store)
  store.setSetting("insights.expected_override.数组", "10")
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const array = insights.topics.find(function (t) { return t.topic === "数组" })
  assert.strictEqual(array.expected, 10)
  assert.strictEqual(array.expectedSource, "override")
  assert.ok(Math.abs(array.coverage - 0.2) < 1e-9, "实际 " + array.coverage)
})

test("四档诊断落到正确的档位", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const array = insights.topics.find(function (t) { return t.topic === "数组" })
  const hash = insights.topics.find(function (t) { return t.topic === "哈希表" })
  assert.strictEqual(array.bucket, "weak-sparse")
  assert.strictEqual(array.label, "薄弱且练得少")
  assert.strictEqual(hash.bucket, "strong-sparse")
  assert.strictEqual(hash.label, "偶然会，没练透")
})

test("topics 按处置优先级排序", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const buckets = insights.topics.map(function (t) { return t.bucket })
  const order = ["weak-dense", "weak-sparse", "strong-sparse", "strong-dense"]
  const indexes = buckets.map(function (b) { return order.indexOf(b) })
  for (let i = 1; i < indexes.length; i += 1) {
    assert.ok(indexes[i - 1] <= indexes[i], "顺序不对：" + buckets.join(","))
  }
})

test("summary 汇总各项数字", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })

  const s = insights.summary
  assert.strictEqual(s.topicCount, 2)
  assert.strictEqual(s.weakCount, 1)
  assert.strictEqual(s.strongCount, 1)
  // 4 条流水、2 道不同的题——按知识点加总会变成 6 和 3
  assert.strictEqual(s.totalAttempts, 4)
  assert.strictEqual(s.totalProblems, 2)
  assert.strictEqual(s.untouchedTopics, 1, "动态规划从没练过")
  assert.strictEqual(s.minExpectedQuestions, 5)
  assert.strictEqual(s.lastPracticeAt, "2026-11-10T09:00:00.000Z")
  assert.ok(s.avgMastery > 0 && s.avgMastery < 1)
})

test("每日题量曲线按 substr(ts,1,10) 分组，窗口内缺失的日期补 0", async function () {
  const store = freshStore()
  seedAttempts(store)
  const insights = await buildInsights({
    store: store,
    provider: fakeProvider(),
    nowIso: NOW,
    days: 4,
  })

  assert.strictEqual(insights.series.length, 4)
  const byDate = {}
  insights.series.forEach(function (row) { byDate[row.date] = row.count })
  assert.strictEqual(byDate["2026-11-08"], 0)
  assert.strictEqual(byDate["2026-11-09"], 2)
  assert.strictEqual(byDate["2026-11-10"], 2)
  assert.strictEqual(byDate["2026-11-11"], 0)
})

test("没有任何流水时返回空数组而不是抛错", async function () {
  const store = freshStore()
  const insights = await buildInsights({ store: store, provider: fakeProvider(), nowIso: NOW })
  assert.deepStrictEqual(insights.topics, [])
  assert.strictEqual(insights.summary.topicCount, 0)
  assert.strictEqual(insights.summary.avgMastery, 0)
  assert.ok(Array.isArray(insights.series))
})

test("addDays 跨月跨年都对", function () {
  assert.strictEqual(addDays("2026-11-11", -1), "2026-11-10")
  assert.strictEqual(addDays("2026-11-01", -1), "2026-10-31")
  assert.strictEqual(addDays("2026-12-31", 1), "2027-01-01")
  assert.strictEqual(addDays("2028-02-28", 1), "2028-02-29")
})

test("normalizeDays 夹住非法值", function () {
  assert.strictEqual(normalizeDays(undefined), DEFAULT_DAYS)
  assert.strictEqual(normalizeDays(""), DEFAULT_DAYS)
  assert.strictEqual(normalizeDays("abc"), DEFAULT_DAYS)
  assert.strictEqual(normalizeDays(-5), DEFAULT_DAYS)
  assert.strictEqual(normalizeDays(0), 0)
  assert.strictEqual(normalizeDays(7), 7)
  assert.strictEqual(normalizeDays(99999), MAX_DAYS)
})

test("buildSeries 在 days=0 时返回全部有数据的日期", function () {
  const series = buildSeries(
    [
      { date: "2026-11-01", count: 3 },
      { date: "2026-11-03", count: 1 },
    ],
    "2026-11-11",
    0
  )
  assert.deepStrictEqual(series, [
    { date: "2026-11-01", count: 3 },
    { date: "2026-11-03", count: 1 },
  ])
})

/* ---------------- store 新增的三个方法 ---------------- */

test("store.listAttemptSummaries 不带 code 列", function () {
  const store = freshStore()
  addAttempt(store, "lc-00001", "2026-11-10T08:00:00.000Z", "pass")
  const rows = store.listAttemptSummaries()
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].problem_id, "lc-00001")
  assert.strictEqual(rows[0].verdict, "pass")
  assert.strictEqual(rows[0].code, undefined)
})

test("store.countAttemptsByDay 按天分组", function () {
  const store = freshStore()
  addAttempt(store, "lc-00001", "2026-11-10T08:00:00.000Z", "pass")
  addAttempt(store, "lc-00002", "2026-11-10T20:00:00.000Z", "pass")
  addAttempt(store, "lc-00003", "2026-11-11T08:00:00.000Z", "wrong_answer")
  const rows = store.countAttemptsByDay()
  assert.deepStrictEqual(rows, [
    { date: "2026-11-10", count: 2 },
    { date: "2026-11-11", count: 1 },
  ])
})

test("store.replaceTopics 整体覆盖，不累加", function () {
  const store = freshStore()
  store.upsertTopic({ id: "数组", first_pass: 1, attempts: 1, last_at: null, lapses: 0 })
  store.replaceTopics([
    { id: "哈希表", first_pass: 2, attempts: 3, last_at: "2026-11-10T08:00:00.000Z", lapses: 1 },
  ])
  const rows = store.listTopics()
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].id, "哈希表")
  assert.strictEqual(rows[0].first_pass, 2)
})

/* ---------------- GET /api/insights ---------------- */

/** 造一个带题库的临时 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-insights-"))
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(
    path.join(dir, "lc-00001.md"),
    [
      "---",
      "id: lc-00001",
      "source: leetcode",
      "title: 两数之和",
      "difficulty: easy",
      "topics: [数组, 哈希表]",
      "tracks: [interview]",
      "status: todo",
      "needs_fill: false",
      "created: 2026-11-11",
      "updated: 2026-11-11",
      "---",
      "## 题面",
      "",
      "给定整数数组与目标值。",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

test("GET /api/insights 返回 topics / summary / series", async function () {
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning() })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/insights")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(Array.isArray(body.topics))
    assert.ok(body.summary)
    assert.ok(Array.isArray(body.series))
  } finally {
    await app.close()
  }
})

test("GET /api/insights?days=7 缩小曲线窗口", async function () {
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning() })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/insights?days=7")
    const body = await res.json()
    assert.strictEqual(body.series.length, 7)
    assert.strictEqual(body.summary.windowDays, 7)
  } finally {
    await app.close()
  }
})

test("GET /api/insights?days=abc 不炸，回默认窗口", async function () {
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning() })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/insights?days=abc")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.summary.windowDays, DEFAULT_DAYS)
  } finally {
    await app.close()
  }
})

test("GET /api/insights 在有流水时给出真实的知识点统计", async function () {
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning() })
  const port = await app.listen(0)
  try {
    app.store.insertAttempt({
      problem_id: "lc-00001",
      ts: new Date(Date.now() - 3600 * 1000).toISOString(),
      language: "cpp",
      code: "int main(){}",
      verdict: "pass",
      passed_cases: 2,
      total_cases: 2,
      duration_ms: 90,
      mode: "solve",
    })
    const body = await (await fetch("http://127.0.0.1:" + port + "/api/insights")).json()
    assert.strictEqual(body.topics.length, 2)
    const array = body.topics.find(function (t) { return t.topic === "数组" })
    const hash = body.topics.find(function (t) { return t.topic === "哈希表" })
    assert.strictEqual(array.attempts, 1)
    assert.strictEqual(array.firstPass, 1)
    assert.strictEqual(array.problems, 1)
    assert.strictEqual(hash.attempts, 1)
    assert.strictEqual(body.summary.totalAttempts, 1, "一次提交不能按知识点数两遍")
    assert.strictEqual(body.summary.totalProblems, 1)
  } finally {
    await app.close()
  }
})
