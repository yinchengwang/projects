"use strict"
/**
 * runs-local-filter.test.js —— units 表的 runs_local 列与过滤
 *
 * runs_local 标的是「这个单元本机跑不跑得起来」。它必须能被筛出来，
 * 否则界面上看不见等于没标。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const path = require("node:path")
const { DatabaseSync } = require("node:sqlite")

const { openDb, createStore, migrate } = require("../server/db.js")
const { tmpDir } = require("./helpers/tmp.js")

function freshStore() {
  const dir = tmpDir("study-hub-runs-local-")
  const db = openDb(path.join(dir, "app.db"))
  return { db: db, store: createStore(db) }
}

function makeLegacyUnitsDb(file) {
  const db = new DatabaseSync(file)
  db.exec(`CREATE TABLE units (
    id TEXT PRIMARY KEY,
    track TEXT NOT NULL,
    kind TEXT NOT NULL,
    title TEXT NOT NULL DEFAULT '',
    path TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'todo',
    topics TEXT NOT NULL DEFAULT '',
    difficulty TEXT NOT NULL DEFAULT 'medium',
    subtracks TEXT NOT NULL DEFAULT '',
    updated TEXT NOT NULL DEFAULT ''
  )`)
  db.prepare(
    "INSERT INTO units(id, track, kind, title, path, status, topics, difficulty, subtracks, updated) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
  ).run("ai:x", "ai", "card", "X", "p", "draft", "[]", "medium", "[]", "")
  db.close()
}

test("units 表有 runs_local 列", function () {
  const { db, store } = freshStore()
  store.replaceUnits([
    {
      id: "ai:probe",
      track: "ai",
      kind: "card",
      title: "探针",
      path: "units/ai/probe.md",
      status: "draft",
      topics: JSON.stringify(["infra:显存"]),
      difficulty: "medium",
      subtracks: JSON.stringify([]),
      runs_local: "yes",
      updated: "2026-11-26T00:00:00.000Z",
    },
  ])
  const rows = store.listUnits({})
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].runs_local, "yes")
  db.close()
})

test("listUnits 能按 runs_local 过滤", function () {
  const { db, store } = freshStore()
  store.replaceUnits([
    { id: "ai:a", track: "ai", kind: "card", title: "A", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "yes", updated: "" },
    { id: "ai:b", track: "ai", kind: "card", title: "B", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "no", updated: "" },
    { id: "ai:c", track: "ai", kind: "card", title: "C", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "partial", updated: "" },
    { id: "ai:d", track: "ai", kind: "card", title: "D", path: "p", status: "draft", topics: "[]", difficulty: "medium", subtracks: "[]", runs_local: "", updated: "" },
  ])
  assert.strictEqual(store.listUnits({ runs_local: "yes" }).length, 1)
  assert.strictEqual(store.listUnits({ runs_local: "no" }).length, 1)
  assert.strictEqual(store.listUnits({ runs_local: "partial" }).length, 1)
  assert.strictEqual(store.listUnits({ runs_local: "" }).length, 1)
  assert.strictEqual(store.listUnits({}).length, 4)
  db.close()
})

test("老库迁移后 runs_local 列存在且缺省为空串", function () {
  const dir = tmpDir("study-hub-runs-local-migrate-")
  const file = path.join(dir, "app.db")
  makeLegacyUnitsDb(file)
  const db = openDb(file)
  const store = createStore(db)
  const columns = db.prepare("PRAGMA table_info(units)").all().map(function (row) {
    return String(row.name)
  })
  assert.ok(columns.includes("runs_local"), columns.join(","))
  const rows = store.listUnits({})
  assert.strictEqual(rows.length, 1)
  assert.strictEqual(rows[0].runs_local, "")
  db.close()
})

test("迁移是幂等的：第二次 openDb 不再加列、不报警告", function () {
  const dir = tmpDir("study-hub-runs-local-idem-")
  const file = path.join(dir, "app.db")
  const first = openDb(file)
  first.close()
  const second = openDb(file)
  const again = migrate(second)
  assert.deepStrictEqual(again.applied, [], "第二次不该再加列")
  assert.deepStrictEqual(again.warnings, [], "第二次不该有警告")
  second.close()
})
