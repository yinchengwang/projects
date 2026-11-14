"use strict"
/**
 * router.test.js —— 路径参数匹配、方法匹配、JSON body 解析
 */

const test = require("node:test")
const assert = require("node:assert")
const { Readable } = require("node:stream")

const { compile, createRouter, readJsonBody, MAX_BODY_BYTES } = require("../server/router.js")

/** 造一个假的 IncomingMessage */
function fakeReq(text) {
  return Readable.from(text === undefined ? [] : [Buffer.from(text, "utf8")])
}

test("compile 匹配静态路径", function () {
  const match = compile("/api/bank")
  assert.deepStrictEqual(match("/api/bank"), {})
  assert.strictEqual(match("/api/bank/1"), null)
  assert.strictEqual(match("/api/banks"), null)
})

test("compile 提取路径参数", function () {
  const match = compile("/api/problem/:id")
  assert.deepStrictEqual(match("/api/problem/lc-00001"), { id: "lc-00001" })
  assert.strictEqual(match("/api/problem"), null)
  assert.strictEqual(match("/api/problem/"), null)
})

test("compile 支持多段路径参数", function () {
  const match = compile("/api/rounds/:id/items/:itemId")
  assert.deepStrictEqual(match("/api/rounds/r36/items/3"), { id: "r36", itemId: "3" })
})

test("compile 对非法百分号编码返回 null 而不是抛错", function () {
  const match = compile("/api/problem/:id")
  assert.strictEqual(match("/api/problem/%E4%"), null)
})

test("路由表：路径对方法不对返回 methodNotAllowed", function () {
  const router = createRouter()
  router.add("GET", "/api/bank", function () {})
  assert.strictEqual(router.find("GET", "/api/bank").route.pattern, "/api/bank")
  assert.strictEqual(router.find("POST", "/api/bank").methodNotAllowed, true)
  assert.strictEqual(router.find("GET", "/api/other"), null)
})

test("readJsonBody 解析 JSON", async function () {
  const body = await readJsonBody(fakeReq('{"a":1,"b":"x"}'))
  assert.deepStrictEqual(body, { a: 1, b: "x" })
})

test("readJsonBody 空 body 返回空对象", async function () {
  const body = await readJsonBody(fakeReq(""))
  assert.deepStrictEqual(body, {})
})

test("readJsonBody 非法 JSON 报 400", async function () {
  await assert.rejects(
    function () {
      return readJsonBody(fakeReq("not json"))
    },
    function (err) {
      return err.status === 400
    }
  )
})

test("readJsonBody 请求体超限报 413", async function () {
  const big = "x".repeat(MAX_BODY_BYTES + 1)
  await assert.rejects(
    function () {
      return readJsonBody(fakeReq(big))
    },
    function (err) {
      return err.status === 413
    }
  )
})

test("readJsonBody 解析结果不是对象时报 400", async function () {
  // 数字、字符串、null、数组都不是 JSON 对象；当成空对象会把
  // 「客户端 body 传错了」变成「服务端字段缺失」，所以直接报 400
  const notObjects = ["123", '"abc"', "null", "[1,2]"]
  for (const raw of notObjects) {
    await assert.rejects(
      function () {
        return readJsonBody(fakeReq(raw))
      },
      function (err) {
        return err.status === 400
      },
      "body 为 " + raw + " 时应报 400"
    )
  }
})
