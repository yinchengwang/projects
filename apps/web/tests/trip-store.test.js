"use strict"
/**
 * trip-store.test.js —— trip-log 的数据层
 *
 * trip-log 原先没有测试（云端逻辑没法在 Node 跑），本文件是新增的。
 */

const test = require("node:test")
const assert = require("node:assert")

// 复用 photos.test.js 的 IndexedDB 替身
const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

// Node 下没有 localStorage，复用 wb-stub 的内存版替身（不要各写一份）
require("./wb-stub.js").installLocalStorage()

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")
const store = require("../shared/trip/store.js")

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

test("add：写入元数据并返回带 id 的记录", async function () {
  const r = await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  assert.ok(r.id, "应生成 id")
  assert.strictEqual(r.title, "西湖")
  assert.strictEqual(r.location, "杭州")
  assert.strictEqual(r.photoIds.length, 0)
})

test("add：带照片时照片进 IndexedDB，元数据只记 photoId", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  assert.deepStrictEqual(r.photoIds, ["p1"])
  const row = await photos.get(r.id, "p1")
  assert.ok(row, "照片应在 IndexedDB 里")
  assert.strictEqual(row.blob.data, "IMG1")
})

test("list：按日期倒序", async function () {
  await store.add({ title: "早", date: "2026-01-01" })
  await store.add({ title: "晚", date: "2026-12-31" })
  const list = await store.list()
  assert.strictEqual(list.length, 2)
  assert.strictEqual(list[0].title, "晚")
})

test("get：按 id 取记录，取不到返回 null", async function () {
  const r = await store.add({ title: "X", date: "2026-11-20" })
  assert.strictEqual((await store.get(r.id)).title, "X")
  assert.strictEqual(await store.get("不存在"), null)
})

test("update：改字段并保留未改的字段", async function () {
  const r = await store.add({ title: "旧标题", date: "2026-11-20", location: "杭州" })
  const u = await store.update(r.id, { title: "新标题" })
  assert.strictEqual(u.title, "新标题")
  assert.strictEqual(u.location, "杭州", "未改的字段应保留")
})

test("update：改不存在的记录返回 null", async function () {
  assert.strictEqual(await store.update("不存在", { title: "X" }), null)
})

test("remove：删记录并连带删掉照片", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  assert.strictEqual(await photos.get(r.id, "p1") !== null, true)
  assert.strictEqual(await store.remove(r.id), true)
  assert.strictEqual(await store.get(r.id), null)
  assert.strictEqual(await photos.get(r.id, "p1"), null, "照片应一并删掉")
})

test("remove：删不存在的记录返回 false", async function () {
  assert.strictEqual(await store.remove("不存在"), false)
})

test("getPhotoUrls：返回照片列表", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const urls = await store.getPhotoUrls(r.id)
  assert.strictEqual(urls.length, 1)
  assert.strictEqual(urls[0].photoId, "p1")
})

test("clear：清掉全部记录与照片", async function () {
  await store.add({ title: "A", date: "2026-11-20", photos: [{ photoId: "p1", blob: { data: "X" } }] })
  await store.clear()
  assert.strictEqual((await store.list()).length, 0)
  assert.strictEqual(await photos.count(), 0)
})

test("listSync：同步读元数据，结果与 list 一致", async function () {
  await store.add({ title: "同步", date: "2026-11-20" })
  const sync = store.listSync()
  assert.strictEqual(sync.length, 1)
  assert.strictEqual(sync[0].title, "同步")
})
