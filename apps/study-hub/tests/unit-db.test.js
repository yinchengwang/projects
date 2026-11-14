"use strict"
/**
 * unit-db.test.js —— server/db.js 的 schema 增补
 *
 * 契约 §4 的铁律：**纯加法**。既有列一个都不改名、不删、不改语义；
 * P0 一条数据都不改，只做「列不存在就加上」，而且迁移必须幂等。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")
const { DatabaseSync } = require("node:sqlite")

const { openDb, createStore, migrate } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 取某张表的列名列表。 */
function columnsOf(db, table) {
  return db
    .prepare("PRAGMA table_info(" + table + ")")
    .all()
    .map(function (row) {
      return String(row.name)
    })
}

/** 造一个「P0 之前」的旧库：四张表，没有新列。 */
function makeLegacyDb(file) {
  const db = new DatabaseSync(file)
  db.exec(`CREATE TABLE attempts (
      id INTEGER PRIMARY KEY, problem_id TEXT NOT NULL, ts TEXT NOT NULL,
      language TEXT NOT NULL, code TEXT NOT NULL, verdict TEXT NOT NULL,
      passed_cases INTEGER NOT NULL DEFAULT 0, total_cases INTEGER NOT NULL DEFAULT 0,
      duration_ms INTEGER NOT NULL DEFAULT 0, mode TEXT NOT NULL DEFAULT 'solve')`)
  db.exec(`CREATE TABLE review_cards (
      problem_id TEXT PRIMARY KEY, easiness REAL NOT NULL DEFAULT 2.5,
      interval_days INTEGER NOT NULL DEFAULT 1, repetitions INTEGER NOT NULL DEFAULT 0,
      last_quality INTEGER, last_at TEXT, next_review_at TEXT NOT NULL,
      lapses INTEGER NOT NULL DEFAULT 0)`)
  db.exec(`CREATE TABLE topics (
      id TEXT PRIMARY KEY, first_pass INTEGER NOT NULL DEFAULT 0,
      attempts INTEGER NOT NULL DEFAULT 0, last_at TEXT, lapses INTEGER NOT NULL DEFAULT 0)`)
  db.exec(`CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`)
  db.prepare("INSERT INTO attempts(problem_id, ts, language, code, verdict) VALUES(?,?,?,?,?)").run(
    "lc-00001",
    "2026-11-01T00:00:00.000Z",
    "cpp",
    "int main(){}",
    "pass"
  )
  db.prepare("INSERT INTO topics(id, first_pass, attempts) VALUES(?,?,?)").run("数组", 1, 1)
  db.close()
}

test("新库既有新列，也有 5 张新表", function () {
  const db = openDb(":memory:")
  const attempts = columnsOf(db, "attempts")
  assert.ok(attempts.indexOf("unit_id") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("track") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("assert_total") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("assert_passed") !== -1, attempts.join(","))

  const cards = columnsOf(db, "review_cards")
  assert.ok(cards.indexOf("unit_id") !== -1, cards.join(","))
  assert.ok(cards.indexOf("units_since") !== -1, cards.join(","))

  const topics = columnsOf(db, "topics")
  assert.ok(topics.indexOf("track") !== -1, topics.join(","))

  ;["units", "sessions", "session_queue", "mastery", "unit_progress"].forEach(function (table) {
    const rows = db.prepare("SELECT COUNT(*) AS n FROM " + table).get()
    assert.equal(Number(rows.n), 0, table)
  })
  db.close()
})

test("migrate 幂等：同一个库跑两次，第二次什么都不做", function () {
  const db = openDb(":memory:")
  const second = migrate(db)
  assert.deepEqual(second.applied, [])
  db.close()
})

test("老库被补齐新列，老数据一条不少", function () {
  const file = path.join(tmpDir("study-hub-db-"), "legacy.db")
  makeLegacyDb(file)
  const db = openDb(file)
  const attempts = columnsOf(db, "attempts")
  assert.ok(attempts.indexOf("unit_id") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("track") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("assert_total") !== -1, attempts.join(","))
  assert.ok(attempts.indexOf("assert_passed") !== -1, attempts.join(","))
  const rows = db.prepare("SELECT COUNT(*) AS n FROM attempts").get()
  assert.equal(Number(rows.n), 1)
  const topics = db.prepare("SELECT COUNT(*) AS n FROM topics").get()
  assert.equal(Number(topics.n), 1)
  db.close()
})

test("老库的既有列名与数据都没被改", function () {
  const file = path.join(tmpDir("study-hub-db-"), "legacy2.db")
  makeLegacyDb(file)
  const db = openDb(file)
  const row = db.prepare("SELECT problem_id, verdict, mode FROM attempts").get()
  assert.equal(String(row.problem_id), "lc-00001")
  assert.equal(String(row.verdict), "pass")
  assert.equal(String(row.mode), "solve")
  db.close()
})

test("既有方法完全不受影响，新列取默认值", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  const id = store.insertAttempt({
    problem_id: "lc-00001",
    ts: "2026-11-01T00:00:00.000Z",
    language: "cpp",
    code: "int main(){}",
    verdict: "pass",
    passed_cases: 2,
    total_cases: 2,
  })
  assert.ok(id > 0)
  const row = db.prepare("SELECT unit_id, track, assert_total, assert_passed FROM attempts WHERE id = ?").get(id)
  assert.equal(row.unit_id, null)
  assert.equal(String(row.track), "algorithm")
  assert.equal(Number(row.assert_total), 0)
  assert.equal(Number(row.assert_passed), 0)
  assert.equal(store.countAttempts(), 1)
  assert.equal(store.listAttemptsByProblem("lc-00001").length, 1)
  db.close()
})

test("units 表：upsert / get / list / count / replace", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  const row = {
    id: "algo:lc-00007",
    track: "algorithm",
    kind: "problem",
    title: "反转整数",
    path: "bank/interview/lc-00007.md",
    status: "verified",
    topics: JSON.stringify(["数学"]),
    difficulty: "medium",
    subtracks: JSON.stringify(["interview"]),
    updated: "2026-11-12T00:00:00.000Z",
  }
  store.upsertUnit(row)
  assert.equal(store.countUnits(), 1)
  const got = store.getUnit("algo:lc-00007")
  assert.equal(got.title, "反转整数")
  // listUnits 返回的 topics / subtracks 已经从 JSON 字符串解析回数组
  assert.deepEqual(got.topics, ["数学"])
  assert.deepEqual(got.subtracks, ["interview"])

  store.upsertUnit(Object.assign({}, row, { title: "改过的标题", status: "draft" }))
  assert.equal(store.countUnits(), 1)
  assert.equal(store.getUnit("algo:lc-00007").title, "改过的标题")
  assert.equal(store.getUnit("algo:lc-00007").status, "draft")

  assert.equal(store.listUnits({ track: "algorithm" }).length, 1)
  assert.equal(store.listUnits({ track: "database" }).length, 0)
  assert.equal(store.listUnits({ status: "draft" }).length, 1)
  assert.equal(store.listUnits({ kind: "problem" }).length, 1)

  store.replaceUnits([])
  assert.equal(store.countUnits(), 0)
  db.close()
})

test("sessions 表：插入 / 改 / 取 / 列表 / 取未关闭的", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  store.insertSession({
    id: "s1",
    started_at: "2026-11-12T01:00:00.000Z",
    planned_count: 3,
    done_count: 0,
    tracks: "algorithm,database",
  })
  store.insertSession({
    id: "s2",
    started_at: "2026-11-12T02:00:00.000Z",
    planned_count: 3,
    done_count: 3,
    tracks: "algorithm",
  })
  assert.equal(store.listSessions().length, 2)
  assert.equal(store.listSessions()[0].id, "s2")
  assert.equal(store.getSession("s1").planned_count, 3)

  store.updateSession("s1", { done_count: 2, ended_at: "2026-11-12T03:00:00.000Z" })
  assert.equal(store.getSession("s1").done_count, 2)
  assert.equal(store.getSession("s1").ended_at, "2026-11-12T03:00:00.000Z")

  // s1 刚被关闭（ended_at 有值），s2 还没结束 → 最新的一条**未关闭**会话是 s2
  const open = store.getLatestOpenSession()
  assert.ok(open)
  assert.equal(open.id, "s2")

  // 把 s2 也关掉 → 已经没有未关闭的会话了（契约 §4.3：没有就返回 undefined）
  store.updateSession("s2", { ended_at: "2026-11-12T04:00:00.000Z" })
  assert.equal(store.getLatestOpenSession(), undefined)
  db.close()
})

test("session_queue 表：整体覆盖 / 列出 / 改状态 / 数完成数", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  store.replaceQueue("s1", [
    { unit_id: "algo:lc-00007", seq: 0, state: "pending" },
    { unit_id: "db:mvcc", seq: 1, state: "current" },
  ])
  const items = store.listQueue("s1")
  assert.equal(items.length, 2)
  assert.equal(items[0].unit_id, "algo:lc-00007")
  assert.equal(items[1].seq, 1)

  store.updateQueueItem("s1", "algo:lc-00007", { state: "done" })
  assert.equal(store.listQueue("s1")[0].state, "done")
  assert.equal(store.countQueueDone("s1"), 1)

  // 整体覆盖：再来一次只剩一行的队列
  store.replaceQueue("s1", [{ unit_id: "ai:cnn", seq: 0, state: "pending" }])
  assert.equal(store.listQueue("s1").length, 1)
  assert.equal(store.countQueueDone("s1"), 0)
  db.close()
})

test("mastery 表：按 scope 存与取", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  store.upsertMastery("unit", "algo:lc-00007", 0.8, "2026-11-12T00:00:00.000Z")
  store.upsertMastery("track", "algorithm", 0.62, "2026-11-12T00:00:00.000Z")
  assert.equal(store.listMastery().length, 2)
  assert.equal(store.listMastery("track").length, 1)
  assert.equal(store.listMastery("track")[0].scope_id, "algorithm")
  // 同一个 scope 再写一次是覆盖，不是追加
  store.upsertMastery("track", "algorithm", 0.7, "2026-11-12T01:00:00.000Z")
  assert.equal(store.listMastery("track").length, 1)
  assert.equal(store.listMastery("track")[0].level, 0.7)
  db.close()
})

test("unit_progress 表：存与取", function () {
  const db = openDb(":memory:")
  const store = createStore(db)
  store.upsertUnitProgress("algo:lc-00007", "in-progress", "2026-11-12T00:00:00.000Z")
  store.upsertUnitProgress("db:mvcc", "done", "2026-11-12T01:00:00.000Z")
  const all = store.listUnitProgress()
  assert.equal(all.length, 2)
  assert.equal(all[0].unit_id, "algo:lc-00007")
  assert.equal(all[0].progress, "in-progress")
  store.upsertUnitProgress("algo:lc-00007", "done")
  assert.equal(store.listUnitProgress().length, 2)
  assert.equal(store.listUnitProgress()[0].progress, "done")
  db.close()
})
