"use strict"
/**
 * db.test.js —— SQLite schema 与仓储函数
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { openDb, createStore } = require("../server/db.js")

function freshStore() {
  const db = openDb(":memory:")
  return { db: db, store: createStore(db) }
}

test("openDb 建出四张表", function () {
  const db = openDb(":memory:")
  const rows = db.prepare("SELECT name FROM sqlite_master WHERE type='table'").all()
  const names = rows.map(function (r) {
    return r.name
  })
  assert.ok(names.indexOf("attempts") !== -1, names.join(","))
  assert.ok(names.indexOf("review_cards") !== -1)
  assert.ok(names.indexOf("topics") !== -1)
  assert.ok(names.indexOf("settings") !== -1)
  db.close()
})

test("openDb 在文件路径下自动建目录", function () {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-db-"))
  const file = path.join(dir, "nested", "app.db")
  const db = openDb(file)
  assert.ok(fs.existsSync(file), "应创建数据库文件")
  db.close()
})

test("settings 读写与覆盖", function () {
  const { store } = freshStore()
  assert.strictEqual(store.getSetting("review.daily_quota_questions", "10"), "10")
  store.setSetting("review.daily_quota_questions", "20")
  assert.strictEqual(store.getSetting("review.daily_quota_questions"), "20")
  store.setSetting("review.daily_quota_questions", "15")
  assert.strictEqual(store.getSetting("review.daily_quota_questions"), "15")
  assert.deepStrictEqual(Object.keys(store.allSettings()), ["review.daily_quota_questions"])
})

test("attempts 写入后能按题号查回", function () {
  const { store } = freshStore()
  const id = store.insertAttempt({
    problem_id: "lc-00001",
    ts: "2026-11-11T10:00:00.000Z",
    language: "cpp",
    code: "int main(){}",
    verdict: "pass",
    passed_cases: 2,
    total_cases: 2,
    duration_ms: 120,
    mode: "solve",
  })
  assert.ok(id > 0, "应返回自增 id")
  assert.strictEqual(store.countAttempts(), 1)
  const rows = store.listAttemptsByProblem("lc-00001")
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].verdict, "pass")
  assert.strictEqual(rows[0].passed_cases, 2)
})

test("review_cards upsert 后覆盖旧值", function () {
  const { store } = freshStore()
  store.upsertReviewCard({
    problem_id: "lc-00001",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 1,
    last_quality: 5,
    last_at: "2026-11-11T10:00:00.000Z",
    next_review_at: "2026-11-12T10:00:00.000Z",
    lapses: 0,
  })
  store.upsertReviewCard({
    problem_id: "lc-00001",
    easiness: 2.6,
    interval_days: 6,
    repetitions: 2,
    last_quality: 4,
    last_at: "2026-11-12T10:00:00.000Z",
    next_review_at: "2026-11-18T10:00:00.000Z",
    lapses: 0,
  })
  assert.strictEqual(store.countReviewCards(), 1)
  const card = store.getReviewCard("lc-00001")
  assert.strictEqual(card.interval_days, 6)
  assert.strictEqual(card.easiness, 2.6)
})

test("listDueCards 只返回已到期的卡片", function () {
  const { store } = freshStore()
  store.upsertReviewCard({
    problem_id: "due",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 0,
    last_quality: null,
    last_at: null,
    next_review_at: "2026-11-01T00:00:00.000Z",
    lapses: 0,
  })
  store.upsertReviewCard({
    problem_id: "later",
    easiness: 2.5,
    interval_days: 6,
    repetitions: 1,
    last_quality: 4,
    last_at: null,
    next_review_at: "2027-01-01T00:00:00.000Z",
    lapses: 0,
  })
  const due = store.listDueCards("2026-12-01T00:00:00.000Z")
  assert.strictEqual(due.length, 1)
  assert.strictEqual(due[0].problem_id, "due")
})

test("topics 累加而不是覆盖", function () {
  const { store } = freshStore()
  store.upsertTopic({ id: "数组", first_pass: 1, attempts: 1, last_at: "2026-11-11T00:00:00.000Z", lapses: 0 })
  store.upsertTopic({ id: "数组", first_pass: 0, attempts: 1, last_at: "2026-11-12T00:00:00.000Z", lapses: 1 })
  const topics = store.listTopics()
  assert.strictEqual(topics.length, 1)
  assert.strictEqual(topics[0].first_pass, 1)
  assert.strictEqual(topics[0].attempts, 2)
  assert.strictEqual(topics[0].lapses, 1)
})
