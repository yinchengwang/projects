"use strict"
/**
 * p1-api-session.test.js —— P1 的会话接口
 *
 * 用真实的 SQLite（内存库）+ 真的 HTTP 服务打接口，不联网、不碰真 Python。
 */

const test = require("node:test")
const assert = require("node:assert")

const { openDb, createStore } = require("../server/db.js")
const { buildRoutes } = require("../server/api-session.js")
const { createServer, sendJson } = require("../server/app.js")

// `topics` / `subtracks` 与 store.listUnits() 读出来的形状一致（数组）。
// P1 的排程只用 id / track / kind / title / status 五个字段，这两个字段不参与判断。
const DEFAULT_UNITS = [
  {
    id: "algo:lc-00001",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    path: "bank/interview/lc-00001.md",
    status: "verified",
    topics: ["哈希表"],
    difficulty: "easy",
    subtracks: ["interview"],
  },
  {
    id: "algo:lc-00002",
    track: "algorithm",
    kind: "problem",
    title: "反转链表",
    path: "bank/interview/lc-00002.md",
    status: "verified",
    topics: ["链表"],
    difficulty: "easy",
    subtracks: ["interview"],
  },
  {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC：版本链与 ReadView",
    path: "scaffold/grokking/mvcc/",
    status: "verified",
    topics: ["事务"],
    difficulty: "medium",
    subtracks: [],
  },
  {
    id: "ai:cnn",
    track: "ai",
    kind: "card",
    title: "卷积神经网络",
    path: "scaffold/grokking/cnn/",
    status: "verified",
    topics: ["深度学习"],
    difficulty: "medium",
    subtracks: [],
  },
]

function probeOk() {
  return {
    python: { ok: true, detail: "python3（测试桩）" },
    numpy: { ok: true, detail: "numpy 可用（测试桩）" },
  }
}

function probeNoNumpy() {
  return {
    python: { ok: true, detail: "python3（测试桩）" },
    numpy: { ok: false, detail: "缺 numpy（测试桩）" },
  }
}

async function withServer(fn, opts) {
  const options = opts || {}
  const db = openDb(":memory:")
  const store = createStore(db)
  const units = options.units === undefined ? DEFAULT_UNITS : options.units
  if (units.length > 0) store.replaceUnits(units)
  const routes = buildRoutes({
    store: store,
    learning: { ok: false, root: "", reason: "测试不连 learning 仓" },
    sendJson: sendJson,
    envProbe: options.envProbe || probeOk,
  })
  const server = createServer({ routes: routes })
  const port = await server.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, store)
  } finally {
    await server.close()
    db.close()
  }
}

function postJson(base, url, body) {
  return fetch(base + url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
}

test("POST /api/session/start 建会话、排队列、给配额", async function () {
  await withServer(async function (base) {
    const res = await postJson(base, "/api/session/start", { n: 3 })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.session, "要有会话")
    assert.strictEqual(body.session.planned_count, body.queue.length)
    assert.ok(body.queue.length > 0, "队列不能空")
    assert.ok(body.quota, "要给配额")
    assert.deepStrictEqual(
      body.queue.map(function (item) {
        return item.seq
      }),
      body.queue.map(function (_item, index) {
        return index + 1
      })
    )
    // 第一个是 current，其余是 pending
    assert.strictEqual(body.queue[0].state, "current")
  })
})

test("POST /api/session/start 拒绝非法会话长度", async function () {
  await withServer(async function (base) {
    const res = await postJson(base, "/api/session/start", { n: 0 })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.strictEqual(body.error, "会话长度必须是 1~10 的整数")
  })
})

test("候选池为空时仍返回 200，并提示去补内容", async function () {
  await withServer(
    async function (base) {
      const res = await postJson(base, "/api/session/start", { n: 3 })
      assert.strictEqual(res.status, 200)
      const body = await res.json()
      assert.strictEqual(body.session, null)
      assert.ok(
        body.warnings.join(" ").indexOf("没有可学的单元，建议去供给流水线补内容") !== -1,
        body.warnings.join("|")
      )
    },
    { units: [] }
  )
})

test("缺 numpy 时 AI 轨被跳过", async function () {
  await withServer(
    async function (base) {
      const res = await postJson(base, "/api/session/start", { n: 3 })
      const body = await res.json()
      const ids = body.queue.map(function (item) {
        return item.unit_id
      })
      assert.ok(ids.indexOf("ai:cnn") === -1, "缺 numpy 时不该排 AI 轨：" + ids.join(","))
      assert.ok(ids.indexOf("algo:lc-00001") !== -1, ids.join(","))
    },
    { envProbe: probeNoNumpy }
  )
})

test("GET /api/session 给出 notice：上次还有 N 个没做完", async function () {
  await withServer(async function (base) {
    await postJson(base, "/api/session/start", { n: 3 })
    const res = await fetch(base + "/api/session")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.session, "要有会话")
    assert.ok(body.queue.length > 0)
    assert.strictEqual(body.notice, "上次还有 " + body.queue.length + " 个没做完")
    assert.ok(body.next, "要给出下一个单元")
    assert.strictEqual(body.next.unit_id, body.queue[0].unit_id)
  })
})

test("GET /api/session/next 给出下一个单元与中文理由", async function () {
  await withServer(async function (base) {
    await postJson(base, "/api/session/start", { n: 3 })
    const res = await fetch(base + "/api/session/next")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.unit, "要有单元")
    assert.strictEqual(body.unit.id, body.unit.id)
    assert.ok(body.reason.length > 0, "要给中文理由")
    assert.ok(body.seq >= 1)
  })
})

test("POST /api/session/submit 缺 unit_id 与不在队列里各报一个中文错", async function () {
  await withServer(async function (base) {
    await postJson(base, "/api/session/start", { n: 3 })
    const missing = await postJson(base, "/api/session/submit", {})
    assert.strictEqual(missing.status, 400)
    assert.strictEqual((await missing.json()).error, "缺少 unit_id")

    const notIn = await postJson(base, "/api/session/submit", { unit_id: "db:不存在的单元" })
    assert.strictEqual(notIn.status, 400)
    assert.strictEqual((await notIn.json()).error, "这个单元不在当前会话里")
  })
})

test("POST /api/session/submit 只排不判：verdict 为 null，队列项变 done，进度记 done", async function () {
  await withServer(async function (base, store) {
    const started = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const unitId = started.queue[0].unit_id

    // 先给这个单元建一张复习卡，用来验证 units_since 的自增
    store.upsertReviewCard({
      problem_id: unitId,
      unit_id: unitId,
      easiness: 2.5,
      interval_days: 1,
      repetitions: 1,
      last_quality: 4,
      last_at: "2026-01-01T00:00:00.000Z",
      next_review_at: "2099-01-01T00:00:00.000Z",
      lapses: 0,
      units_since: 0,
    })

    const res = await postJson(base, "/api/session/submit", { unit_id: unitId })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.ok, true)
    assert.strictEqual(body.judged, false)
    assert.strictEqual(body.verdict, null)
    assert.strictEqual(body.passed, 0)
    assert.strictEqual(body.total, 0)
    assert.ok(body.message.indexOf("已记为完成") !== -1, body.message)
    assert.ok(body.session, "要回会话")
    assert.ok(Array.isArray(body.due))

    const queue = store.listQueue(body.session.id)
    const item = queue.filter(function (q) {
      return q.unit_id === unitId
    })[0]
    assert.strictEqual(item.state, "done")
    assert.strictEqual(store.getSession(body.session.id).done_count, 1)

    const progress = store.listUnitProgress().filter(function (row) {
      return row.unit_id === unitId
    })[0]
    assert.strictEqual(progress.progress, "done")

    // 被提交的单元自己的卡：跑过一次 SM-2，units_since 归零
    const own = store.getReviewCard(unitId)
    assert.ok(own, "被提交的单元自己的卡要更新")
    assert.strictEqual(Number(own.units_since), 0, "触发后 units_since 归零")
    assert.strictEqual(Number(own.repetitions), 2, "SM-2 记一次重复")
  })
})

test("POST /api/session/submit 后所有复习卡的 units_since 自增 1", async function () {
  await withServer(async function (base, store) {
    const started = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const unitId = started.queue[0].unit_id
    store.upsertReviewCard({
      problem_id: "lc-99999",
      unit_id: "algo:lc-99999",
      easiness: 2.5,
      interval_days: 1,
      repetitions: 1,
      last_quality: 4,
      last_at: "2026-01-01T00:00:00.000Z",
      next_review_at: "2099-01-01T00:00:00.000Z",
      lapses: 0,
      units_since: 2,
    })
    await postJson(base, "/api/session/submit", { unit_id: unitId })
    const card = store.getReviewCard("lc-99999")
    assert.strictEqual(Number(card.units_since), 3)
  })
})

test("POST /api/session/pause 关掉会话但队列还在", async function () {
  await withServer(async function (base, store) {
    const started = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const res = await postJson(base, "/api/session/pause", {})
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.session.ended_at, "要写上结束时间")
    assert.ok(body.queue.length > 0, "队列不能丢")
    assert.strictEqual(store.listQueue(started.session.id).length, body.queue.length)
  })
})

test("再次 start 不带 reset 时接着上次的会话", async function () {
  await withServer(async function (base) {
    const first = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    await postJson(base, "/api/session/pause", {})
    const second = await (await postJson(base, "/api/session/start", {})).json()
    assert.strictEqual(second.session.id, first.session.id)
    assert.strictEqual(second.queue.length, first.queue.length)
  })
})

test("start 带 reset 时重排一个新会话", async function () {
  await withServer(async function (base) {
    const first = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const second = await (await postJson(base, "/api/session/start", { reset: true })).json()
    assert.notStrictEqual(second.session.id, first.session.id)
  })
})

test("GET /api/settings 给出会话、轨权重、次数阈值与环境自检", async function () {
  await withServer(async function (base) {
    const res = await fetch(base + "/api/settings")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.session.default_count, 3)
    assert.strictEqual(body.session.min_count, 1)
    assert.strictEqual(body.session.max_count, 10)
    assert.strictEqual(body.review.count_trigger_k, 5)
    assert.strictEqual(body.tracks.length, 3)
    assert.strictEqual(body.tracks[0].code, "algorithm")
    assert.strictEqual(body.tracks[0].weight, 0.4)
    assert.strictEqual(body.tracks[0].enabled, true)
    assert.ok(typeof body.tracks[0].unit_count === "number")
    assert.ok(body.env.numpy, "要给环境自检结果")
  })
})

test("PUT /api/settings 改会话长度、轨权重、轨开关与次数阈值", async function () {
  await withServer(async function (base) {
    const res = await fetch(base + "/api/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        session_count: 5,
        track_weights: { algorithm: 0.6, database: 0.2, ai: 0.2 },
        track_enabled: { ai: false },
        count_trigger_k: 8,
      }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.session.default_count, 5)
    assert.strictEqual(body.review.count_trigger_k, 8)
    const ai = body.tracks.filter(function (t) {
      return t.code === "ai"
    })[0]
    assert.strictEqual(ai.enabled, false)
    assert.strictEqual(ai.weight, 0.2)
  })
})

test("PUT /api/settings 拒绝非法值，四条错各给一句中文", async function () {
  await withServer(async function (base) {
    const cases = [
      [{ session_count: 99 }, "会话长度必须是 1~10 的整数"],
      [{ track_weights: { algorithm: -1 } }, "轨权重必须是不小于 0 的数字"],
      [{ track_enabled: { ai: "yes" } }, "轨开关必须是布尔值"],
      [{ count_trigger_k: 0 }, "次数阈值 K 必须是大于等于 1 的整数"],
    ]
    for (let i = 0; i < cases.length; i += 1) {
      const res = await fetch(base + "/api/settings", {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(cases[i][0]),
      })
      assert.strictEqual(res.status, 400, JSON.stringify(cases[i][0]))
      const body = await res.json()
      assert.strictEqual(body.error, cases[i][1])
    }
  })
})

test("重复提交同一个单元：不重复记账，给中文说明", async function () {
  // submit 里有「队列项已是 done 就直接返回」的分支（幂等），原 15 条用例**未覆盖**。
  // 干跑实测：把那条分支的 verdict: null 改成 "pass" 依然全绿（变异体存活）。
  await withServer(async function (base, store) {
    const started = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const unitId = started.queue[0].unit_id

    const first = await (await postJson(base, "/api/session/submit", { unit_id: unitId })).json()
    assert.strictEqual(first.judged, false)
    assert.strictEqual(first.verdict, null)

    const second = await (await postJson(base, "/api/session/submit", { unit_id: unitId })).json()
    assert.strictEqual(second.ok, true)
    assert.strictEqual(second.judged, false, "重复提交仍然不判题")
    assert.strictEqual(second.verdict, null, "重复提交也不许冒出 verdict")
    assert.ok(second.message.indexOf("已经完成过") !== -1, second.message)
    // 不重复记账：done_count 还是 1
    assert.strictEqual(store.getSession(started.session.id).done_count, 1)
  })
})

test("submit 时刚跨过次数阈值的卡会被列进 due", async function () {
  // `newlyDue` 的收集逻辑原 15 条用例只断言了 `Array.isArray(body.due)`（恒真）。
  // 干跑实测：把 `if (id) newlyDue.push(id)` 删掉依然全绿（变异体存活）。
  await withServer(async function (base, store) {
    const started = await (await postJson(base, "/api/session/start", { n: 3 })).json()
    const unitId = started.queue[0].unit_id

    // 默认 K = 5；这张卡已有 4，提交一次就跨过阈值 → 必须出现在 due 里
    store.upsertReviewCard({
      problem_id: "lc-08888",
      unit_id: "algo:lc-08888",
      easiness: 2.5,
      interval_days: 1,
      repetitions: 1,
      last_quality: 4,
      last_at: "2026-01-01T00:00:00.000Z",
      next_review_at: "2099-01-01T00:00:00.000Z",
      lapses: 0,
      units_since: 4,
    })

    const body = await (await postJson(base, "/api/session/submit", { unit_id: unitId })).json()
    assert.ok(
      body.due.indexOf("algo:lc-08888") !== -1,
      "刚跨过 K 的卡要进 due：" + JSON.stringify(body.due)
    )
  })
})
