"use strict"
/**
 * unit-model.test.js —— core/track/tracks.js 与 core/unit/unit.js
 *
 * 钉住契约 §1（Unit 模型）与 §14（轨道定义）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const tracks = require("../core/track/tracks.js")
const unit = require("../core/unit/unit.js")

// ---------- core/track/tracks.js ----------

test("TRACKS 恰好三条顶层轨，权重 0.4 / 0.3 / 0.3", function () {
  assert.equal(tracks.TRACKS.length, 3)
  assert.deepEqual(tracks.TRACK_CODES, ["algorithm", "database", "ai"])
  const algo = tracks.trackOf("algorithm")
  assert.equal(algo.label, "算法")
  assert.equal(algo.idPrefix, "algo")
  assert.equal(algo.weight, 0.4)
  assert.deepEqual(algo.subtracks, ["fundamentals", "interview", "advanced", "engineering"])
  assert.equal(tracks.trackOf("database").weight, 0.3)
  assert.equal(tracks.trackOf("ai").weight, 0.3)
})

test("只有算法轨有子轨，另两条恒为空数组", function () {
  assert.deepEqual(tracks.trackOf("database").subtracks, [])
  assert.deepEqual(tracks.trackOf("ai").subtracks, [])
})

test("trackOf 认得三条轨，未知 code 返回 null", function () {
  assert.equal(tracks.trackOf("algorithm").code, "algorithm")
  assert.equal(tracks.trackOf("database").code, "database")
  assert.equal(tracks.trackOf("ai").code, "ai")
  assert.equal(tracks.trackOf("network"), null)
  assert.equal(tracks.trackOf(""), null)
})

test("trackOfIdPrefix 认得三个 id 前缀", function () {
  assert.equal(tracks.trackOfIdPrefix("algo").code, "algorithm")
  assert.equal(tracks.trackOfIdPrefix("db").code, "database")
  assert.equal(tracks.trackOfIdPrefix("ai").code, "ai")
  assert.equal(tracks.trackOfIdPrefix("xx"), null)
})

test("labelOf 给中文标签，未知 code 返回「未知轨道」", function () {
  assert.equal(tracks.labelOf("algorithm"), "算法")
  assert.equal(tracks.labelOf("database"), "数据库")
  assert.equal(tracks.labelOf("ai"), "AI")
  assert.equal(tracks.labelOf("network"), "未知轨道")
})

test("grokking 卡清单是冻结的 16 + 13", function () {
  assert.equal(tracks.GROKKING_DB_CARDS.length, 16)
  assert.equal(tracks.GROKKING_AI_CARDS.length, 13)
  assert.ok(tracks.GROKKING_DB_CARDS.indexOf("mvcc") !== -1)
  assert.ok(tracks.GROKKING_DB_CARDS.indexOf("transaction") !== -1)
  assert.ok(tracks.GROKKING_DB_CARDS.indexOf("index_design") !== -1)
  assert.ok(tracks.GROKKING_DB_CARDS.indexOf("sql_fundamentals") !== -1)
  assert.ok(tracks.GROKKING_AI_CARDS.indexOf("cnn") !== -1)
  assert.ok(tracks.GROKKING_AI_CARDS.indexOf("svm") !== -1)
})

test("trackOfGrokkingCard 分得出 database / ai / null", function () {
  assert.equal(tracks.trackOfGrokkingCard("mvcc"), "database")
  assert.equal(tracks.trackOfGrokkingCard("cnn"), "ai")
  assert.equal(tracks.trackOfGrokkingCard("btree"), null)
  assert.equal(tracks.trackOfGrokkingCard(""), null)
})

// ---------- core/unit/unit.js ----------

test("normalizeList 去空、去重、保序", function () {
  assert.deepEqual(unit.normalizeList([" 事务 ", "并发", "事务", "", null, undefined]), ["事务", "并发"])
  assert.deepEqual(unit.normalizeList("单个"), ["单个"])
  assert.deepEqual(unit.normalizeList(null), [])
})

test("normalizeUnitId 按轨补前缀，已带前缀的原样返回", function () {
  assert.equal(unit.normalizeUnitId("lc-00007", "algorithm"), "algo:lc-00007")
  assert.equal(unit.normalizeUnitId("mvcc", "database"), "db:mvcc")
  assert.equal(unit.normalizeUnitId("cnn", "ai"), "ai:cnn")
  assert.equal(unit.normalizeUnitId("algo:lc-00007", "algorithm"), "algo:lc-00007")
  assert.equal(unit.normalizeUnitId("", "algorithm"), "")
})

test("ID_PATTERN 只认三种 id 形态", function () {
  assert.ok(unit.ID_PATTERN.test("algo:lc-00007"))
  assert.ok(unit.ID_PATTERN.test("db:mvcc"))
  assert.ok(unit.ID_PATTERN.test("ai:cnn"))
  assert.ok(unit.ID_PATTERN.test("algo:cf-1234-a"))
  assert.ok(!unit.ID_PATTERN.test("lc-00007"))
  assert.ok(!unit.ID_PATTERN.test("db:MVCC"))
  assert.ok(!unit.ID_PATTERN.test("xx:mvcc"))
  assert.ok(!unit.ID_PATTERN.test("db:"))
})

test("toBool 只把 true 与字符串 'true' 当真", function () {
  assert.equal(unit.toBool(true), true)
  assert.equal(unit.toBool("true"), true)
  assert.equal(unit.toBool("false"), false)
  assert.equal(unit.toBool(false), false)
  assert.equal(unit.toBool(undefined), false)
})

function validUnit(over) {
  return Object.assign(
    {
      id: "db:mvcc",
      track: "database",
      kind: "card",
      title: "MVCC",
      statement: "讲解",
      entry: { demo: "scaffold/grokking/mvcc/main.py", reference: "", graded: "script", script: "units/database/mvcc.assert.py", language: "python" },
      asserts: [],
      topics: ["事务"],
      difficulty: "medium",
      source: "grokking",
      status: "verified",
      progress: "todo",
      reviewed: false,
      reviewed_at: "",
      subtracks: [],
      path: "scaffold/grokking/mvcc/",
    },
    over || {}
  )
}

test("validateUnit 对合规单元返回空数组", function () {
  assert.deepEqual(unit.validateUnit(validUnit()), [])
})

test("validateUnit 报出具体的中文错误", function () {
  const errors = unit.validateUnit(validUnit({ id: "mvcc", title: "" }))
  assert.equal(errors.length, 3)
  assert.ok(errors[0].indexOf("id 非法") !== -1, errors[0])
  assert.ok(errors[1].indexOf("前缀") !== -1, errors[1])
  assert.ok(errors[2].indexOf("title") !== -1, errors[2])
})

test("validateUnit 抓到 graded:script 却没有 script 路径", function () {
  const errors = unit.validateUnit(
    validUnit({ entry: { demo: "", reference: "", graded: "script", script: "", language: "python" } })
  )
  assert.equal(errors.length, 1)
  assert.ok(errors[0].indexOf("entry.script") !== -1, errors[0])
})

test("createUnit 填齐默认值并保留 related", function () {
  const got = unit.createUnit({
    id: "algo:lc-00007",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    entry: { graded: "user" },
    path: "bank/interview/lc-00007.md",
    related: "轮次：R01",
  })
  assert.equal(got.difficulty, "medium")
  assert.equal(got.source, "unknown")
  assert.equal(got.status, "todo")
  assert.equal(got.progress, "todo")
  assert.equal(got.reviewed, false)
  assert.equal(got.reviewed_at, "")
  assert.deepEqual(got.topics, [])
  assert.deepEqual(got.subtracks, [])
  assert.deepEqual(got.asserts, [])
  assert.equal(got.entry.demo, "")
  assert.equal(got.entry.script, "")
  assert.equal(got.related, "轮次：R01")
  // 契约 §1.2：related 是可选附加字段，缺省必须是 ""（不是 undefined），键必须存在
  const bare = unit.createUnit({
    id: "algo:lc-00007",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    entry: { graded: "user" },
    path: "bank/interview/lc-00007.md",
  })
  assert.ok(Object.prototype.hasOwnProperty.call(bare, "related"))
  assert.equal(bare.related, "")
})

test("createUnit 字段非法时抛中文错误", function () {
  assert.throws(
    function () {
      unit.createUnit({
        id: "bad",
        track: "algorithm",
        kind: "problem",
        title: "x",
        entry: { graded: "user" },
        path: "bank/interview/lc-00007.md",
      })
    },
    /单元字段非法/
  )
})

test("validateUnit 要求 path 非空（契约 §1.2 必填，G42）", function () {
  const errors = unit.validateUnit(validUnit({ path: "" }))
  assert.equal(errors.length, 1)
  assert.ok(errors[0].indexOf("path") !== -1, errors[0])
})

test("validateUnit 要求 id 前缀与 track 一致（G41）", function () {
  const errors = unit.validateUnit(validUnit({ id: "algo:mvcc" }))
  assert.equal(errors.length, 1)
  assert.ok(errors[0].indexOf("前缀") !== -1, errors[0])
})

test("normalizeUnitId 未知轨道返回空串，不静默兜底成 algo", function () {
  assert.equal(unit.normalizeUnitId("lc-00007", "network"), "")
})

test("createUnit 缺 path 时抛中文错误", function () {
  assert.throws(
    function () {
      unit.createUnit({ id: "algo:lc-00007", track: "algorithm", kind: "problem", title: "x", entry: { graded: "user" } })
    },
    /单元字段非法/
  )
})

test("toUnitSummary 只给列表页要的字段", function () {
  const got = unit.toUnitSummary(validUnit())
  assert.deepEqual(Object.keys(got).sort(), [
    "difficulty",
    "id",
    "kind",
    "status",
    "subtracks",
    "title",
    "topics",
    "track",
  ])
  assert.equal(got.id, "db:mvcc")
  assert.equal(got.title, "MVCC")
})
