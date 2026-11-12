"use strict"
/**
 * llm-guard.test.js —— 成本护栏测试（**不联网、不起服务**）
 *
 * 钉住：
 *   1. 单次会话上限 20 次、每日上限 100 次（G31）；
 *   2. 超了 ok 变 false，且每日超限时提示里含「今天用完了」；
 *   3. 计数真的落在 settings 表里（这里用一个最小的假 store）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const {
  checkQuota,
  consumeQuota,
  sessionKey,
  dailyKey,
  checkAll,
  consumeAll,
  limits,
  seedLimits,
} = require("../core/llm/guard.js")

/** 最小假 store：只实现 settings 表的两个方法，接口与 M1 冻结的一致 */
function makeStore(initial) {
  const data = Object.assign({}, initial || {})
  return {
    data: data,
    getSetting: function (key, fallback) {
      if (Object.prototype.hasOwnProperty.call(data, key)) return data[key]
      return fallback === undefined ? null : fallback
    },
    setSetting: function (key, value) { data[key] = String(value) },
  }
}

test("默认上限：单次会话 20 次、每日 100 次", function () {
  const store = makeStore()
  const l = limits(store)
  assert.equal(l.sessionLimit, 20)
  assert.equal(l.dailyLimit, 100)
})

test("checkQuota：新会话还没用就是 ok，用满了就 false", function () {
  const store = makeStore()
  const key = sessionKey("s1")
  const before = checkQuota(store, key)
  assert.equal(before.ok, true)
  assert.equal(before.used, 0)
  assert.equal(before.limit, 20)

  consumeQuota(store, key, 20)
  const after = checkQuota(store, key)
  assert.equal(after.ok, false)
  assert.equal(after.used, 20)
})

test("consumeQuota：计数累加，并写回 settings 表", function () {
  const store = makeStore()
  const key = sessionKey("s2")
  consumeQuota(store, key)
  consumeQuota(store, key)
  assert.equal(store.getSetting(key), "2")
  const r = consumeQuota(store, key, 3)
  assert.equal(r.used, 5)
  assert.equal(r.limit, 20)
})

test("上限可以在 settings 表里调，调完立即生效", function () {
  const store = makeStore({ "llm.quota.session_limit": "2" })
  const key = sessionKey("s3")
  consumeQuota(store, key)
  consumeQuota(store, key)
  assert.equal(checkQuota(store, key).ok, false)
  assert.equal(checkQuota(store, key).limit, 2)
})

test("sessionKey 与 dailyKey 的键名固定", function () {
  assert.equal(sessionKey("abc"), "llm.quota.session.abc")
  assert.equal(sessionKey(""), "llm.quota.session.default")
  assert.equal(sessionKey(undefined), "llm.quota.session.default")
  assert.ok(dailyKey("2026-11-11").startsWith("llm.quota.daily."))
  assert.equal(dailyKey("2026-11-11"), "llm.quota.daily.2026-11-11")
})

test("checkAll：两个额度都没超就 ok，message 为空", function () {
  const store = makeStore()
  const r = checkAll(store, "s4", "2026-11-11")
  assert.equal(r.ok, true)
  assert.equal(r.message, "")
  assert.equal(r.session.limit, 20)
  assert.equal(r.daily.limit, 100)
})

test("checkAll：会话额度用完时提示会话上限", function () {
  const store = makeStore()
  consumeQuota(store, sessionKey("s5"), 20)
  const r = checkAll(store, "s5", "2026-11-11")
  assert.equal(r.ok, false)
  assert.ok(r.message.includes("本次会话"), "提示里应说明是会话额度用完：" + r.message)
  assert.ok(!r.message.includes("今天用完了"), "会话额度用完不该说『今天用完了』")
})

test("checkAll：每日额度用完时提示『今天用完了』", function () {
  const store = makeStore()
  consumeQuota(store, dailyKey("2026-11-11"), 100)
  const r = checkAll(store, "s6", "2026-11-11")
  assert.equal(r.ok, false)
  assert.ok(r.message.includes("今天用完了"), "提示里应包含『今天用完了』：" + r.message)
  assert.equal(r.daily.ok, false)
})

test("consumeAll：会话与每日两个计数同时扣", function () {
  const store = makeStore()
  consumeAll(store, "s7", "2026-11-11")
  assert.equal(store.getSetting(sessionKey("s7")), "1")
  assert.equal(store.getSetting(dailyKey("2026-11-11")), "1")
})

test("seedLimits：环境变量里的配额写进 settings 表，但不覆盖已有的值", function () {
  const store = makeStore({ "llm.quota.session_limit": "7" })
  seedLimits(store, { ALGO_PLAN_LLM_SESSION_LIMIT: "30", ALGO_PLAN_LLM_DAILY_LIMIT: "200" })
  assert.equal(store.getSetting("llm.quota.session_limit"), "7", "已有的值不该被环境变量覆盖")
  assert.equal(store.getSetting("llm.quota.daily_limit"), "200", "没有的值应从环境变量写入")
})
