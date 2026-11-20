"use strict"
/**
 * llm-supply-guard.test.js —— 离线批处理的成本护栏
 *
 * 口径（契约 §10 + G11）：供给批处理**复用会话桶**，把「会话 id」换成**批次 id**；
 * 每日上限沿用不变。一批用满就停并提示。
 */

const test = require("node:test")
const assert = require("node:assert")

const { DEFAULT_BATCH_ID, batchKey, checkBatch, consumeBatch } = require("../core/llm/guard.js")

/** 一个最小的 settings 仓储 */
function makeStore() {
  const map = new Map()
  return {
    getSetting: function (key, fallback) {
      return map.has(key) ? map.get(key) : fallback
    },
    setSetting: function (key, value) {
      map.set(key, String(value))
    },
  }
}

test("批次桶沿用会话桶的键名", function () {
  assert.strictEqual(batchKey("supply-2026-11-12"), "llm.quota.session.supply-2026-11-12")
})

test("没给批次 id 时落到默认批次", function () {
  assert.strictEqual(DEFAULT_BATCH_ID, "supply")
  assert.strictEqual(batchKey(""), "llm.quota.session.supply")
  assert.strictEqual(batchKey(null), "llm.quota.session.supply")
})

test("批次配额：批次桶 + 每日桶一起查、一起扣", function () {
  const store = makeStore()
  const before = checkBatch(store, "b1", "2026-11-12")
  assert.strictEqual(before.ok, true)
  assert.strictEqual(before.batch.used, 0)
  assert.strictEqual(before.daily.used, 0)

  consumeBatch(store, "b1", "2026-11-12")
  const after = checkBatch(store, "b1", "2026-11-12")
  assert.strictEqual(after.batch.used, 1)
  assert.strictEqual(after.daily.used, 1)
})

test("批次桶用满就停，提示语要说清是批次", function () {
  const store = makeStore()
  store.setSetting("llm.quota.session_limit", "2")
  consumeBatch(store, "b1", "2026-11-12")
  consumeBatch(store, "b1", "2026-11-12")
  const quota = checkBatch(store, "b1", "2026-11-12")
  assert.strictEqual(quota.ok, false)
  assert.ok(quota.message.indexOf("批次") !== -1, "提示里要说批次：" + quota.message)
})

test("不同批次各算各的，每日上限共享", function () {
  const store = makeStore()
  consumeBatch(store, "b1", "2026-11-12")
  const other = checkBatch(store, "b2", "2026-11-12")
  assert.strictEqual(other.ok, true)
  assert.strictEqual(other.batch.used, 0)
  assert.strictEqual(other.daily.used, 1)
})

test("每日上限沿用 100，提示语照旧", function () {
  const store = makeStore()
  store.setSetting("llm.quota.daily_limit", "3")
  consumeBatch(store, "b1", "2026-11-12")
  consumeBatch(store, "b2", "2026-11-12")
  consumeBatch(store, "b3", "2026-11-12")
  const quota = checkBatch(store, "b4", "2026-11-12")
  assert.strictEqual(quota.ok, false)
  assert.ok(quota.message.indexOf("今天用完了") !== -1, "每日上限的提示语要沿用：" + quota.message)
})
