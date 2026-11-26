"use strict"
/**
 * storage.test.js —— 存储适配层（localStorage 后端）
 */

const test = require("node:test")
const assert = require("node:assert")

/** Node 下没有 localStorage，用一个内存版替身 */
function makeLocalStorage () {
  const map = new Map()
  return {
    getItem: function (k) { return map.has(k) ? map.get(k) : null },
    setItem: function (k, v) { map.set(k, String(v)) },
    removeItem: function (k) { map.delete(k) },
    key: function (i) { return Array.from(map.keys())[i] === undefined ? null : Array.from(map.keys())[i] },
    get length () { return map.size },
    _map: map
  }
}

global.localStorage = makeLocalStorage()

const storage = require("../shared/core/storage.js")

test.beforeEach(function () {
  global.localStorage = makeLocalStorage()
  storage.clearAll()
})

test("键名统一带 db: 前缀", function () {
  storage.set("foo", { a: 1 })
  assert.ok(global.localStorage._map.has("db:foo"), "实际键名：" + Array.from(global.localStorage._map.keys()))
})

test("set/get 往返：对象与数组都正确", function () {
  storage.set("obj", { a: 1, b: [2, 3] })
  storage.set("arr", [1, 2, 3])
  assert.deepStrictEqual(storage.get("obj", null), { a: 1, b: [2, 3] })
  assert.deepStrictEqual(storage.get("arr", null), [1, 2, 3])
})

test("get 读不到时返回默认值", function () {
  assert.strictEqual(storage.get("根本没有", "兜底"), "兜底")
  assert.strictEqual(storage.get("根本没有", null), null)
})

test("set 写入的值是 JSON 字符串（可被其它标签页读到）", function () {
  storage.set("x", { a: 1 })
  assert.strictEqual(global.localStorage._map.get("db:x"), '{"a":1}')
})

test("remove 删除单个键", function () {
  storage.set("x", 1)
  assert.strictEqual(storage.remove("x"), true)
  assert.strictEqual(storage.get("x", null), null)
})

test("clearAll 只清 db: 前缀的键，不动别的", function () {
  global.localStorage.setItem("别人的键", "别删我")
  storage.set("mine", 1)
  storage.clearAll()
  assert.strictEqual(global.localStorage.getItem("别人的键"), "别删我")
  assert.strictEqual(storage.get("mine", null), null)
})

test("exportAll 导出为 JSON 字符串，且不含 seeded 标记", function () {
  storage.set("a", 1)
  storage.set("b", { c: 2 })
  const raw = storage.exportAll()
  const parsed = JSON.parse(raw)
  assert.strictEqual(parsed.version, 1)
  assert.deepStrictEqual(parsed.data.a, 1)
  assert.deepStrictEqual(parsed.data.b, { c: 2 })
  assert.ok(parsed.exportedAt, "应带 exportedAt")
})

test("importAll 以 merge 模式只填空缺键", function () {
  storage.set("a", "原有")
  const r = storage.importAll(JSON.stringify({ data: { a: "导入的", b: "导入的" } }), "merge")
  assert.strictEqual(r.ok, true)
  assert.strictEqual(r.count, 2)
  assert.strictEqual(storage.get("a", null), "原有", "merge 不该覆盖已有键")
  assert.strictEqual(storage.get("b", null), "导入的")
})

test("importAll 以 overwrite 模式全量覆盖", function () {
  storage.set("a", "原有")
  storage.importAll(JSON.stringify({ data: { a: "导入的" } }), "overwrite")
  assert.strictEqual(storage.get("a", null), "导入的")
})

test("importAll 遇到坏 JSON 返回中文错误而不是抛异常", function () {
  const r = storage.importAll("{ 这不是 JSON", "merge")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0)
})

test("importAll 缺少 data 字段时返回中文错误", function () {
  const r = storage.importAll(JSON.stringify({ nope: 1 }), "merge")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error.indexOf("data") >= 0, "错误信息应指出 data 字段，实际：" + r.error)
})

test("usage 统计键数", function () {
  storage.set("a", 1)
  storage.set("b", 2)
  const u = storage.usage()
  assert.ok(u.keys >= 2)
})

test("存储写满时 set 返回 false 而不是抛异常", function () {
  const original = global.localStorage.setItem
  global.localStorage.setItem = function () { throw new Error("QuotaExceededError") }
  const ok = storage.set("big", { x: 1 })
  global.localStorage.setItem = original
  assert.strictEqual(ok, false, "写失败应返回 false")
})
