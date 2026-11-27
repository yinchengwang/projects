"use strict"
/**
 * trip-export.test.js —— 导出导入往返
 */

const test = require("node:test")
const assert = require("node:assert")

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

// Node 下没有 localStorage，复用 wb-stub 的内存版替身（不要各写一份）
require("./wb-stub.js").installLocalStorage()

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")
const store = require("../shared/trip/store.js")
const exporter = require("../shared/trip/export.js")

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

test("exportAll：导出成 JSON 字符串", async function () {
  await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  const json = await exporter.exportAll()
  const parsed = JSON.parse(json)
  assert.strictEqual(parsed.version, 1)
  assert.strictEqual(parsed.data.length, 1)
  assert.strictEqual(parsed.data[0].title, "西湖")
  assert.ok(parsed.exportedAt)
})

test("exportAll：照片被 base64 嵌进去", async function () {
  await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const parsed = JSON.parse(await exporter.exportAll())
  assert.strictEqual(parsed.data[0].photos.length, 1)
  assert.strictEqual(parsed.data[0].photos[0].photoId, "p1")
  assert.ok(parsed.data[0].photos[0].base64 !== undefined)
})

test("importAll：导出再导入，数据一致", async function () {
  await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  const json = await exporter.exportAll()

  storage.clearAll()
  await photos.clear()
  assert.strictEqual((await store.list()).length, 0)

  const r = await exporter.importAll(json, "overwrite")
  assert.strictEqual(r.ok, true)
  assert.strictEqual(r.count, 1)

  const list = await store.list()
  assert.strictEqual(list.length, 1)
  assert.strictEqual(list[0].title, "西湖")
  assert.strictEqual(list[0].location, "杭州")
})

test("importAll：照片也能往返", async function () {
  await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const json = await exporter.exportAll()

  storage.clearAll()
  await photos.clear()

  await exporter.importAll(json, "overwrite")
  const list = await store.list()
  assert.strictEqual(list[0].photoIds.length, 1)
  const row = await photos.get(list[0].id, "p1")
  assert.ok(row, "照片应恢复")
})

test("importAll：坏 JSON 返回中文错误不抛异常", async function () {
  const r = await exporter.importAll("{ 这不是 JSON", "overwrite")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0)
})

test("importAll：缺 data 数组返回中文错误", async function () {
  const r = await exporter.importAll(JSON.stringify({ nope: 1 }), "overwrite")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error.indexOf("data") >= 0, "错误应指出 data，实际：" + r.error)
})
