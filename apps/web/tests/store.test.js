"use strict"
/**
 * store.test.js —— 数据域工厂
 *
 * 注意：本文件先于 shared/core/storage.js 落地，故此处的 require 会失败，
 * Task 3 完成后本文件才可运行。这是预期的红→绿顺序。
 */

const test = require("node:test")
const assert = require("node:assert")

/**
 * Node 下没有 localStorage，先装一个内存版替身再 require。
 * storage.js 在模块加载时不碰 localStorage（只在函数体里访问），
 * 但 beforeEach 会调 clearAll()，所以必须在 require 之前装好。
 */
function makeLocalStorage () {
  const map = new Map()
  return {
    getItem: function (k) { return map.has(k) ? map.get(k) : null },
    setItem: function (k, v) { map.set(k, String(v)) },
    removeItem: function (k) { map.delete(k) },
    key: function (i) {
      const keys = Array.from(map.keys())
      return i < keys.length ? keys[i] : null
    },
    get length () { return map.size },
    clear: function () { map.clear() }
  }
}

global.localStorage = makeLocalStorage()

const store = require("../shared/core/store.js")
const storage = require("../shared/core/storage.js")

test.beforeEach(function () {
  global.localStorage = makeLocalStorage()
  storage.clearAll()
})

test("列表域：首次读取会写入种子数据", function () {
  const s = store.createListStore({ key: "test_seeded", seed: function () {
    return [{ id: "a", v: 1 }]
  } })
  assert.strictEqual(s.getAll().length, 1)
  // 落盘了
  assert.ok(storage.get("test_seeded", null) !== null)
})

test("列表域：add 生成 id 与 createdAt，并插到最前", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", v: 1 })
  s.add({ id: "x2", v: 2 })
  const all = s.getAll()
  assert.strictEqual(all[0].id, "x2", "新加的应排在最前")
  assert.ok(all[0].createdAt, "应自动补 createdAt")
})

test("列表域：update 按 id 局部更新，未命中返回 null", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", v: 1 })
  const updated = s.update("x1", { v: 9 })
  assert.strictEqual(updated.v, 9)
  assert.strictEqual(s.update("不存在", { v: 1 }), null)
})

test("列表域：remove 按 id 删除并返回是否真的删掉了", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1" })
  assert.strictEqual(s.remove("x1"), true)
  assert.strictEqual(s.remove("x1"), false, "重复删应返回 false")
})

test("列表域：find 与 count", function () {
  const s = store.createListStore({ key: "test_list" })
  s.add({ id: "x1", done: false })
  s.add({ id: "x2", done: true })
  assert.strictEqual(s.find("x1").id, "x1")
  assert.strictEqual(s.find("没有"), null)
  assert.strictEqual(s.count(), 2)
  assert.strictEqual(s.count(function (i) { return i.done }), 1)
})

test("对象域：get 返回默认值与已存值的合并", function () {
  const s = store.createObjectStore({ key: "test_obj", defaultValue: { a: 1, b: 2 } })
  assert.deepStrictEqual(s.get(), { a: 1, b: 2 })
  s.patch({ b: 5 })
  assert.deepStrictEqual(s.get(), { a: 1, b: 5 })
})

test("对象域：reset 恢复默认", function () {
  const s = store.createObjectStore({ key: "test_obj", defaultValue: { a: 1 } })
  s.patch({ a: 9 })
  s.reset()
  assert.deepStrictEqual(s.get(), { a: 1 })
})
