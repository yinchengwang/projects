"use strict"
/**
 * api-diagnose.test.js —— GET /api/diagnose 的三个视图
 *
 * 契约 §8.3 冻结接口与错误码，§8.3.1 冻结响应形状，§8.3.2 冻结统计口径。
 * 全程不联网：直接调路由的 handler，用假 res 接 JSON。
 */

const test = require("node:test")
const assert = require("node:assert")

const { openDb, createStore } = require("../server/db.js")
const { buildDiagnoseRoutes } = require("../server/api-diagnose.js")

/** 固定时刻，断言能写死数字 */
const NOW = "2026-11-11T00:00:00.000Z"

/**
 * 单元清单：算法 3 个（1 个没练过）、数据库 1 个、AI 1 个。
 * 覆盖六轴的四种判定：cpp / python / engineering 子轨 / SQL 知识点。
 */
const UNITS = [
  { id: "algo:lc-00001", track: "algorithm", title: "两数之和", topics: ["数组"], subtracks: ["interview"], entry: { language: "cpp" } },
  { id: "algo:lc-00002", track: "algorithm", title: "合并区间", topics: ["数组", "SQL"], subtracks: ["engineering"], entry: { language: "cpp" } },
  { id: "algo:lc-00003", track: "algorithm", title: "爬楼梯", topics: [], subtracks: [], entry: { language: "cpp" } },
  { id: "db:mvcc", track: "database", title: "MVCC", topics: ["事务"], subtracks: [], entry: { language: "python" } },
  { id: "ai:cnn", track: "ai", title: "卷积神经网络", topics: [], subtracks: [], entry: { language: "python" } },
]

/** 单元级统计（测试里独立算一遍掌握度，公式与 core/mastery/mastery.js 一致） */
const STATS = {
  "algo:lc-00001": { attempts: 1, firstPass: 1, lapses: 0, lastAt: "2026-11-10T08:00:00.000Z" },
  "algo:lc-00002": { attempts: 1, firstPass: 0, lapses: 1, lastAt: "2026-11-10T09:00:00.000Z" },
  "db:mvcc": { attempts: 1, firstPass: 1, lapses: 0, lastAt: "2026-11-10T10:00:00.000Z" },
}

/** 掌握度：0.7×首次通过率 + 0.3×e^(-失误/2)，再乘遗忘因子 */
function masteryOf(stat) {
  const days = (Date.parse(NOW) - Date.parse(stat.lastAt)) / 86400000
  const base = 0.7 * (stat.firstPass / stat.attempts) + 0.3 * Math.exp(-stat.lapses / 2)
  return base * Math.pow(0.5, days / 14)
}

const M_ALGO_1 = masteryOf(STATS["algo:lc-00001"])
const M_ALGO_2 = masteryOf(STATS["algo:lc-00002"])
const M_DB = masteryOf(STATS["db:mvcc"])
/** 算法轨：练过的两个单元的均值 */
const M_TRACK_ALGO = (M_ALGO_1 + M_ALGO_2) / 2

function freshStore() {
  return createStore(openDb(":memory:"))
}

/** 插一条作答流水。problem_id 与 unit_id 都写，两个口径都能查到 */
function addAttempt(store, unitId, ts, verdict) {
  store.insertAttempt({
    problem_id: unitId,
    unit_id: unitId,
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
 *   algo:lc-00001 一次通过   → 强
 *   algo:lc-00002 一次没通过 → 弱
 *   db:mvcc       一次通过   → 强
 *   algo:lc-00003 / ai:cnn 没练过
 */
function seed(store) {
  addAttempt(store, "algo:lc-00001", "2026-11-10T08:00:00.000Z", "pass")
  addAttempt(store, "algo:lc-00002", "2026-11-10T09:00:00.000Z", "wrong_answer")
  addAttempt(store, "db:mvcc", "2026-11-10T10:00:00.000Z", "pass")
}

/** 假 res：把 writeHead / end 记下来 */
function fakeRes() {
  const res = { status: 0, body: null }
  res.writeHead = function (status) {
    res.status = status
  }
  res.end = function (text) {
    res.body = text ? JSON.parse(text) : null
  }
  return res
}

/** 回 JSON——与 server/app.js 的 sendJson 同一个写法 */
function sendJson(res, status, body) {
  const text = JSON.stringify(body)
  res.writeHead(status, { "content-type": "application/json; charset=utf-8" })
  res.end(text)
}

/** 调一次 /api/diagnose */
async function call(store, units, view) {
  const routes = buildDiagnoseRoutes({
    store: store,
    provider: null,
    sendJson: sendJson,
    units: units,
    nowIso: NOW,
  })
  const route = routes[0]
  assert.strictEqual(route.method, "GET")
  assert.strictEqual(route.pattern, "/api/diagnose")
  const res = fakeRes()
  const query = new URLSearchParams(view === undefined || view === null ? "" : "view=" + view)
  await route.handler({}, res, {}, query)
  return res
}

/* ---------------- view=mastery ---------------- */

test("view 缺省时返回 mastery 视图", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, undefined)
  assert.strictEqual(res.status, 200)
  assert.strictEqual(res.body.view, "mastery")
  assert.ok(Array.isArray(res.body.tracks))
  assert.ok(Array.isArray(res.body.topics))
  assert.ok(res.body.summary)
})

test("view 不在三个视图里返回 400 与中文错误", async function () {
  const store = freshStore()
  await assert.rejects(
    function () {
      return call(store, UNITS, "bogus")
    },
    function (err) {
      assert.strictEqual(err.status, 400)
      assert.strictEqual(err.message, "诊断视图只能是 mastery / radar / heatmap")
      return true
    }
  )
})

test("mastery 视图的 tracks 三行，顺序固定 algorithm → database → ai", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "mastery")

  assert.strictEqual(res.body.tracks.length, 3)
  assert.deepStrictEqual(
    res.body.tracks.map(function (t) {
      return t.track
    }),
    ["algorithm", "database", "ai"]
  )
  assert.deepStrictEqual(
    res.body.tracks.map(function (t) {
      return t.label
    }),
    ["算法", "数据库", "AI"]
  )
})

test("轨道掌握度是练过单元的单元掌握度均值", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "mastery")

  const algo = res.body.tracks[0]
  assert.ok(Math.abs(algo.mastery - M_TRACK_ALGO) < 1e-9, "实际 " + algo.mastery + " 期望 " + M_TRACK_ALGO)
  const db = res.body.tracks[1]
  assert.ok(Math.abs(db.mastery - M_DB) < 1e-9, "实际 " + db.mastery + " 期望 " + M_DB)
})

test("没练过的轨道掌握度是 0，不抛错", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "mastery")

  const ai = res.body.tracks[2]
  assert.strictEqual(ai.track, "ai")
  assert.strictEqual(ai.mastery, 0)
  assert.strictEqual(ai.unitCount, 1, "该轨一共 1 个单元（没练过也计数）")
})

test("mastery 视图的知识点行就是契约冻结的七个字段", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "mastery")

  const array = res.body.topics.filter(function (t) {
    return t.track === "algorithm" && t.topic === "数组"
  })[0]
  assert.ok(array, "知识点行带轨与裸知识点名")
  assert.strictEqual(array.track, "algorithm")
  assert.strictEqual(array.topic, "数组")
  assert.ok(Math.abs(array.mastery - M_TRACK_ALGO) < 1e-9)
  assert.ok(array.coverage > 0, "练过的知识点覆盖度大于 0")
  assert.ok(array.label, "知识点行要带四档的中文标签")
  assert.ok(array.action, "知识点行要带处方")

  // 契约 §8.3.1 冻结的就是这七个字段——不多不少
  assert.deepStrictEqual(
    Object.keys(array).sort(),
    ["action", "bucket", "coverage", "label", "mastery", "topic", "track"]
  )
})

test("三层掌握度物化进 mastery 表", async function () {
  const store = freshStore()
  seed(store)
  await call(store, UNITS, "mastery")

  const units = store.listMastery("unit")
  const topics = store.listMastery("topic")
  const tracks = store.listMastery("track")
  assert.strictEqual(units.length, 3, "练过的三个单元")
  assert.strictEqual(topics.length, 3, "algorithm:数组 / algorithm:SQL / database:事务")
  assert.strictEqual(tracks.length, 3)
  const algo = tracks.filter(function (row) {
    return row.scope_id === "algorithm"
  })[0]
  assert.ok(Math.abs(algo.level - M_TRACK_ALGO) < 1e-9)
})

/* ---------------- view=radar ---------------- */

test("radar 视图的六根轴顺序固定", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  assert.strictEqual(res.body.view, "radar")
  assert.deepStrictEqual(
    res.body.axes.map(function (a) {
      return a.id
    }),
    ["algorithm", "database", "ai", "c", "python", "sql"]
  )
  assert.deepStrictEqual(
    res.body.axes.map(function (a) {
      return a.label
    }),
    ["算法", "数据库", "AI", "C/C++", "Python", "SQL"]
  )
})

test("radar 视图的 weak 只收练过且掌握度低于 0.5 的单元", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  const ids = res.body.weak.map(function (u) {
    return u.unit_id
  })
  assert.deepStrictEqual(ids, ["algo:lc-00002"])
  assert.strictEqual(res.body.weak[0].track, "algorithm")
  assert.strictEqual(res.body.weak[0].title, "合并区间")
})

test("radar 视图的 strong 只收练过且掌握度不低于 0.5 的单元", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  const ids = res.body.strong.map(function (u) {
    return u.unit_id
  })
  assert.deepStrictEqual(ids, ["db:mvcc", "algo:lc-00001"], "按掌握度降序")
  assert.ok(ids.indexOf("algo:lc-00003") === -1, "没练过的单元不进清单")
  assert.ok(ids.indexOf("ai:cnn") === -1, "没练过的单元不进清单")
})

test("radar 的 c 轴取算法轨里 cpp 或 engineering 子轨的单元均值", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  const c = res.body.axes.filter(function (a) {
    return a.id === "c"
  })[0]
  assert.ok(Math.abs(c.value - M_TRACK_ALGO) < 1e-9, "实际 " + c.value)
})

test("radar 的 python 轴取 entry.language 为 python 的单元均值", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  const py = res.body.axes.filter(function (a) {
    return a.id === "python"
  })[0]
  assert.ok(Math.abs(py.value - M_DB) < 1e-9, "实际 " + py.value)
})

test("radar 的 sql 轴取知识点含 SQL 的单元均值（大小写不敏感）", async function () {
  const store = freshStore()
  seed(store)
  const res = await call(store, UNITS, "radar")

  const sql = res.body.axes.filter(function (a) {
    return a.id === "sql"
  })[0]
  assert.ok(Math.abs(sql.value - M_ALGO_2) < 1e-9, "实际 " + sql.value)
})

/* ---------------- view=heatmap ---------------- */

test("heatmap 视图给出按时间升序的会话与每个队列项的格子", async function () {
  const store = freshStore()
  seed(store)
  store.insertSession({
    id: "s1",
    started_at: "2026-11-09T10:00:00.000Z",
    planned_count: 2,
    done_count: 1,
    tracks: "algorithm",
  })
  store.replaceQueue("s1", [
    { unit_id: "algo:lc-00001", seq: 1, state: "done" },
    { unit_id: "algo:lc-00003", seq: 2, state: "pending" },
  ])
  store.insertSession({
    id: "s2",
    started_at: "2026-11-10T10:00:00.000Z",
    planned_count: 1,
    done_count: 1,
    tracks: "algorithm",
  })
  store.replaceQueue("s2", [{ unit_id: "algo:lc-00002", seq: 1, state: "done" }])

  const res = await call(store, UNITS, "heatmap")
  assert.strictEqual(res.body.view, "heatmap")
  assert.deepStrictEqual(
    res.body.sessions.map(function (s) {
      return s.id
    }),
    ["s1", "s2"],
    "按时间升序"
  )
  assert.deepStrictEqual(
    res.body.sessions.map(function (s) {
      return s.index
    }),
    [1, 2],
    "序号从 1 开始"
  )
  assert.strictEqual(res.body.cells.length, 3)
  const first = res.body.cells[0]
  assert.strictEqual(first.session_id, "s1")
  assert.strictEqual(first.track, "algorithm")
  assert.strictEqual(first.state, "pass")
  assert.strictEqual(first.seq, 1)
  assert.strictEqual(res.body.cells[1].state, "pending", "没作答过的单元是 pending")
  assert.strictEqual(res.body.cells[2].state, "wrong_answer")
})

/* ---------------- 单元清单的两种形状 ---------------- */

test("ctx.units 传快照对象（loadUnits 的返回值）也认，不静默退回旧题库", async function () {
  const store = freshStore()
  seed(store)

  // server/app.js 传进来的是 loadUnitsIndex() 的产物：{units, warnings, byId}
  const routes = buildDiagnoseRoutes({
    store: store,
    provider: null,
    sendJson: sendJson,
    units: { units: UNITS, warnings: [], byId: {} },
    nowIso: NOW,
  })
  const res = fakeRes()
  await routes[0].handler({}, res, {}, new URLSearchParams("view=mastery"))

  assert.strictEqual(res.status, 200)
  // 三条轨迹的 unitCount 来自完整 Unit 清单；退回 provider.list() 的话这里会是 0
  assert.strictEqual(res.body.tracks[0].unitCount, 3, "算法轨全部单元数")
  const array = res.body.topics.filter(function (t) {
    return t.topic === "数组"
  })[0]
  assert.ok(array, "快照对象里的 topics 数组要用上，否则知识点明细全空")
  assert.ok(Math.abs(array.mastery - M_TRACK_ALGO) < 1e-9)
})
