"use strict"
/**
 * p1-db-session.test.js —— sessions / session_queue 落库 + P1 数据迁移
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")
const { DatabaseSync } = require("node:sqlite")

const { openDb, createStore, migrateUnitIds } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")

function freshStore() {
  const db = openDb(":memory:")
  return { db: db, store: createStore(db) }
}

test("insertSession / getSession：tracks 存进去读出来还是数组", function () {
  const { store } = freshStore()
  store.insertSession({
    id: "s-1",
    started_at: "2026-01-01T00:00:00.000Z",
    planned_count: 3,
    done_count: 0,
    tracks: ["algorithm", "database"],
  })
  const s = store.getSession("s-1")
  assert.strictEqual(s.id, "s-1")
  assert.strictEqual(s.planned_count, 3)
  assert.deepStrictEqual(s.tracks, ["algorithm", "database"])
  assert.strictEqual(s.ended_at, null)
})

test("getLatestOpenSession：只认 ended_at 为空的最新一条", function () {
  const { store } = freshStore()
  store.insertSession({
    id: "s-old",
    started_at: "2026-01-01T00:00:00.000Z",
    planned_count: 3,
    done_count: 0,
    tracks: [],
  })
  store.insertSession({
    id: "s-new",
    started_at: "2026-01-02T00:00:00.000Z",
    planned_count: 3,
    done_count: 0,
    tracks: [],
  })
  store.updateSession("s-new", { ended_at: "2026-01-02T01:00:00.000Z" })
  const open = store.getLatestOpenSession()
  assert.strictEqual(open.id, "s-old")
})

test("updateSession：改 done_count 与 ended_at", function () {
  const { store } = freshStore()
  store.insertSession({
    id: "s-1",
    started_at: "2026-01-01T00:00:00.000Z",
    planned_count: 3,
    done_count: 0,
    tracks: [],
  })
  store.updateSession("s-1", { done_count: 2, ended_at: "2026-01-01T02:00:00.000Z" })
  const s = store.getSession("s-1")
  assert.strictEqual(s.done_count, 2)
  assert.strictEqual(s.ended_at, "2026-01-01T02:00:00.000Z")
})

test("listSessions：按 started_at 倒序", function () {
  const { store } = freshStore()
  store.insertSession({ id: "s-1", started_at: "2026-01-01T00:00:00.000Z", tracks: [] })
  store.insertSession({ id: "s-2", started_at: "2026-01-02T00:00:00.000Z", tracks: [] })
  const list = store.listSessions()
  assert.deepStrictEqual(
    list.map(function (s) {
      return s.id
    }),
    ["s-2", "s-1"]
  )
})

test("replaceQueue / listQueue：按 seq 升序", function () {
  const { store } = freshStore()
  store.replaceQueue("s-1", [
    { unit_id: "algo:lc-00001", seq: 1, state: "pending" },
    { unit_id: "db:mvcc", seq: 2, state: "pending" },
  ])
  const queue = store.listQueue("s-1")
  assert.strictEqual(queue.length, 2)
  assert.strictEqual(queue[0].unit_id, "algo:lc-00001")
  assert.strictEqual(queue[1].seq, 2)
})

test("replaceQueue 是整体覆盖：再排一次不留旧项", function () {
  const { store } = freshStore()
  store.replaceQueue("s-1", [{ unit_id: "algo:lc-00001", seq: 1, state: "pending" }])
  store.replaceQueue("s-1", [{ unit_id: "db:mvcc", seq: 1, state: "pending" }])
  const queue = store.listQueue("s-1")
  assert.strictEqual(queue.length, 1)
  assert.strictEqual(queue[0].unit_id, "db:mvcc")
})

test("updateQueueItem：改 state 与 seq", function () {
  const { store } = freshStore()
  store.replaceQueue("s-1", [{ unit_id: "algo:lc-00001", seq: 1, state: "pending" }])
  store.updateQueueItem("s-1", "algo:lc-00001", { state: "done" })
  assert.strictEqual(store.listQueue("s-1")[0].state, "done")
  store.updateQueueItem("s-1", "algo:lc-00001", { seq: 5 })
  assert.strictEqual(store.listQueue("s-1")[0].seq, 5)
})

test("countQueueDone：只数 done", function () {
  const { store } = freshStore()
  store.replaceQueue("s-1", [
    { unit_id: "algo:lc-00001", seq: 1, state: "done" },
    { unit_id: "algo:lc-00002", seq: 2, state: "pending" },
    { unit_id: "db:mvcc", seq: 3, state: "skipped" },
  ])
  assert.strictEqual(store.countQueueDone("s-1"), 1)
})

test("upsertReviewCard：带 unit_id 与 units_since 时都写进去", function () {
  const { store } = freshStore()
  store.upsertReviewCard({
    problem_id: "lc-00007",
    unit_id: "algo:lc-00007",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 1,
    last_quality: 4,
    last_at: "2026-01-01T00:00:00.000Z",
    next_review_at: "2026-01-07T00:00:00.000Z",
    lapses: 0,
    units_since: 3,
  })
  const card = store.getReviewCard("lc-00007")
  assert.strictEqual(card.unit_id, "algo:lc-00007")
  assert.strictEqual(Number(card.units_since), 3)
})

test("upsertReviewCard：缺省 unit_id / units_since 时保留旧值，不清零", function () {
  const { store } = freshStore()
  store.upsertReviewCard({
    problem_id: "lc-00007",
    unit_id: "algo:lc-00007",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 1,
    last_quality: 4,
    last_at: "2026-01-01T00:00:00.000Z",
    next_review_at: "2026-01-07T00:00:00.000Z",
    lapses: 0,
    units_since: 3,
  })
  store.upsertReviewCard({
    problem_id: "lc-00007",
    easiness: 2.5,
    interval_days: 6,
    repetitions: 2,
    last_quality: 5,
    last_at: "2026-01-07T00:00:00.000Z",
    next_review_at: "2026-01-22T00:00:00.000Z",
    lapses: 0,
  })
  const card = store.getReviewCard("lc-00007")
  assert.strictEqual(card.unit_id, "algo:lc-00007")
  assert.strictEqual(Number(card.units_since), 3)
})

test("listReviewCards / listDueCardsByCount：次数通道的候选", function () {
  const { store } = freshStore()
  store.upsertReviewCard({
    problem_id: "lc-00001",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 0,
    next_review_at: "2099-01-01T00:00:00.000Z",
    lapses: 0,
    units_since: 5,
  })
  store.upsertReviewCard({
    problem_id: "lc-00002",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 0,
    next_review_at: "2099-01-01T00:00:00.000Z",
    lapses: 0,
    units_since: 1,
  })
  assert.strictEqual(store.listReviewCards().length, 2)
  const due = store.listDueCardsByCount(5)
  assert.strictEqual(due.length, 1)
  assert.strictEqual(due[0].problem_id, "lc-00001")
})

test("insertAttempt：缺省 unit_id 为 null、track 为 algorithm", function () {
  const { store } = freshStore()
  store.insertAttempt({
    problem_id: "lc-00007",
    ts: "2026-01-01T00:00:00.000Z",
    language: "cpp",
    code: "",
    verdict: "pass",
    passed_cases: 1,
    total_cases: 1,
    duration_ms: 10,
    mode: "solve",
  })
  const row = store.listAttemptsByProblem("lc-00007")[0]
  assert.strictEqual(row.unit_id, null)
  assert.strictEqual(row.track, "algorithm")
})

test("migrateUnitIds：给老行补 unit_id，跑两次结果一致", function () {
  const db = new DatabaseSync(":memory:")
  db.exec(
    "CREATE TABLE attempts (id INTEGER PRIMARY KEY, problem_id TEXT NOT NULL, ts TEXT NOT NULL, language TEXT NOT NULL, code TEXT NOT NULL, verdict TEXT NOT NULL, passed_cases INTEGER NOT NULL DEFAULT 0, total_cases INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0, mode TEXT NOT NULL DEFAULT 'solve')"
  )
  db.exec(
    "CREATE TABLE review_cards (problem_id TEXT PRIMARY KEY, easiness REAL NOT NULL DEFAULT 2.5, interval_days INTEGER NOT NULL DEFAULT 1, repetitions INTEGER NOT NULL DEFAULT 0, last_quality INTEGER, last_at TEXT, next_review_at TEXT NOT NULL, lapses INTEGER NOT NULL DEFAULT 0)"
  )
  db.prepare("INSERT INTO attempts(problem_id, ts, language, code, verdict) VALUES(?,?,?,?,?)").run(
    "lc-00007",
    "2026-01-01T00:00:00.000Z",
    "cpp",
    "",
    "pass"
  )
  db.prepare("INSERT INTO review_cards(problem_id, next_review_at) VALUES(?,?)").run(
    "lc-00007",
    "2026-01-01T00:00:00.000Z"
  )
  migrateUnitIds(db)
  const attempt = db.prepare("SELECT unit_id FROM attempts WHERE problem_id = ?").get("lc-00007")
  assert.strictEqual(attempt.unit_id, "algo:lc-00007")
  const card = db.prepare("SELECT unit_id FROM review_cards WHERE problem_id = ?").get("lc-00007")
  assert.strictEqual(card.unit_id, "algo:lc-00007")
  migrateUnitIds(db)
  const again = db.prepare("SELECT unit_id FROM attempts WHERE problem_id = ?").get("lc-00007")
  assert.strictEqual(again.unit_id, "algo:lc-00007")
  db.close()
})

test("openDb 必须跑 migrateUnitIds：老库里没 unit_id 的行，打开后要补上", function () {
  // 上一条是直接调 migrateUnitIds —— 它抓不住「openDb 忘了调它」这个变异体
  // （干跑实测：把 openDb 里那行删掉，13 条用例全绿）。
  // 这条走 openDb 的真实路径：造一个「P0 之前」的老库文件，openDb 之后检查回填。
  const file = path.join(tmpDir("study-hub-p1-"), "legacy-noid.db")
  const raw = new DatabaseSync(file)
  raw.exec(
    "CREATE TABLE attempts (id INTEGER PRIMARY KEY, problem_id TEXT NOT NULL, ts TEXT NOT NULL, language TEXT NOT NULL, code TEXT NOT NULL, verdict TEXT NOT NULL, passed_cases INTEGER NOT NULL DEFAULT 0, total_cases INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0, mode TEXT NOT NULL DEFAULT 'solve')"
  )
  raw.prepare("INSERT INTO attempts(problem_id, ts, language, code, verdict) VALUES(?,?,?,?,?)").run(
    "lc-00007",
    "2026-01-01T00:00:00.000Z",
    "cpp",
    "",
    "pass"
  )
  raw.close()

  const db = openDb(file)
  const row = db.prepare("SELECT unit_id FROM attempts WHERE problem_id = ?").get("lc-00007")
  assert.strictEqual(row.unit_id, "algo:lc-00007", "openDb 必须把老行的 unit_id 补齐")
  // 带冒号的 problem_id 不该被二次加前缀
  db.prepare("INSERT INTO attempts(problem_id, ts, language, code, verdict) VALUES(?,?,?,?,?)").run(
    "db:mvcc",
    "2026-01-01T00:00:00.000Z",
    "cpp",
    "",
    "pass"
  )
  migrateUnitIds(db)
  const prefixed = db.prepare("SELECT unit_id FROM attempts WHERE problem_id = ? AND unit_id IS NOT NULL").get("db:mvcc")
  assert.strictEqual(prefixed, undefined, "带冒号的 problem_id 不该被回填成 algo:db:mvcc")
  db.close()
})
