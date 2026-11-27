"use strict"
/**
 * photos.test.js —— IndexedDB 图片存储层
 */

const test = require("node:test")
const assert = require("node:assert")

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

const photos = require("../shared/photos.js")

test.beforeEach(async function () {
  await photos.clear()
})

test("put 与 get：按 tripId + photoId 存取 Blob", async function () {
  const blob = { data: "假图片数据", size: 6 }
  await photos.put("t1", "p1", blob)
  const got = await photos.get("t1", "p1")
  assert.ok(got, "应能取回")
  assert.strictEqual(got.blob.data, "假图片数据")
})

test("get：键不存在返回 null", async function () {
  assert.strictEqual(await photos.get("t1", "不存在"), null)
})

test("put：同 tripId 下多个 photoId 互不覆盖", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t1", "p2", { data: "B" })
  assert.strictEqual((await photos.get("t1", "p1")).blob.data, "A")
  assert.strictEqual((await photos.get("t1", "p2")).blob.data, "B")
})

test("put：不同 tripId 下同名 photoId 互不覆盖", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  assert.strictEqual((await photos.get("t1", "p1")).blob.data, "A")
  assert.strictEqual((await photos.get("t2", "p1")).blob.data, "B")
})

test("list：列出某条记录的全部 photoId", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t1", "p2", { data: "B" })
  await photos.put("t2", "p1", { data: "C" })
  const list = await photos.list("t1")
  assert.strictEqual(list.length, 2)
  assert.ok(list.indexOf("p1") >= 0)
  assert.ok(list.indexOf("p2") >= 0)
})

test("delete：删掉指定照片", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.delete("t1", "p1")
  assert.strictEqual(await photos.get("t1", "p1"), null)
})

test("deleteAll：清掉某条记录的全部照片，不影响别的记录", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  await photos.deleteAll("t1")
  assert.strictEqual(await photos.get("t1", "p1"), null)
  assert.ok(await photos.get("t2", "p1"), "别的记录不该被删")
})

test("count：统计全部照片数", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  assert.strictEqual(await photos.count(), 2)
})

test("clear：清空全部", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.clear()
  assert.strictEqual(await photos.count(), 0)
})

test("isAvailable：IndexedDB 存在时返回 true", async function () {
  assert.strictEqual(await photos.isAvailable(), true)
})

test("IndexedDB 不可用时 isAvailable 返回 false，put reject 中文错误", async function () {
  const saved = global.indexedDB
  global.indexedDB = undefined
  // 重新 require 以便重新探测
  delete require.cache[require.resolve("../shared/photos.js")]
  const photos2 = require("../shared/photos.js")
  assert.strictEqual(await photos2.isAvailable(), false)
  await assert.rejects(
    function () { return photos2.put("t1", "p1", { data: "A" }) },
    function (err) {
      assert.ok(err && err.message.indexOf("IndexedDB") >= 0, "错误信息应含 IndexedDB，实际：" + (err && err.message))
      return true
    }
  )
  global.indexedDB = saved
})
