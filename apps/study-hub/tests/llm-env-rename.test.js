"use strict"
/**
 * llm-env-rename.test.js —— ALGO_PLAN_LLM_* → STUDY_HUB_LLM_*（契约 §13）
 *
 * P4 起**只读新的**环境变量名：旧的 ALGO_PLAN_LLM_* 一个都不再读，
 * 代码里**不保留兼容分支**（契约 G26）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const { createLlmClient } = require("../core/llm/client.js")
const { seedLimits, limits } = require("../core/llm/guard.js")

/** 一个最小的 settings 仓储（只实现 guard.js 用到的两个方法） */
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

test("总开关只认 STUDY_HUB_LLM_ENABLED", function () {
  const off = createLlmClient({ env: { ARK_API_KEY: "k", STUDY_HUB_LLM_ENABLED: "0" } })
  assert.strictEqual(off.name, "null")
  assert.ok(off.reason.indexOf("STUDY_HUB_LLM_ENABLED") !== -1, "提示里要点名新变量")
})

test("旧的 ALGO_PLAN_LLM_ENABLED 不再生效", function () {
  const client = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_ENABLED: "0" } })
  assert.strictEqual(client.name, "ark", "旧名字不再被读取，LLM 不该被关掉")
})

test("provider 只认 STUDY_HUB_LLM_PROVIDER", function () {
  const byNew = createLlmClient({ env: { ARK_API_KEY: "k", STUDY_HUB_LLM_PROVIDER: "anthropic" } })
  assert.strictEqual(byNew.name, "anthropic")
  const byOld = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_PROVIDER: "anthropic" } })
  assert.strictEqual(byOld.name, "ark", "旧名字不再被读取")
})

test("模型名只认 STUDY_HUB_LLM_MODEL", function () {
  const byNew = createLlmClient({ env: { ARK_API_KEY: "k", STUDY_HUB_LLM_MODEL: "my-model" } })
  assert.strictEqual(byNew.model, "my-model")
  const byOld = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_MODEL: "old-model" } })
  assert.strictEqual(byOld.model, "doubao-seed-1-6-251015", "旧名字不再被读取")
})

test("地址只认 STUDY_HUB_LLM_BASE_URL", function () {
  const byNew = createLlmClient({ env: { ARK_API_KEY: "k", STUDY_HUB_LLM_BASE_URL: "https://ark.test/api/v3" } })
  assert.strictEqual(byNew.baseUrl, "https://ark.test/api/v3")
  const byOld = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_BASE_URL: "https://old.test/api/v3" } })
  assert.strictEqual(byOld.baseUrl, "https://ark.cn-beijing.volces.com/api/v3", "旧名字不再被读取")
})

test("配额上限只认 STUDY_HUB_LLM_SESSION_LIMIT / _DAILY_LIMIT", function () {
  const store = makeStore()
  seedLimits(store, { STUDY_HUB_LLM_SESSION_LIMIT: "30", STUDY_HUB_LLM_DAILY_LIMIT: "200" })
  assert.deepStrictEqual(limits(store), { sessionLimit: 30, dailyLimit: 200 })
})

test("旧的配额环境变量不再生效", function () {
  const store = makeStore()
  seedLimits(store, { ALGO_PLAN_LLM_SESSION_LIMIT: "30", ALGO_PLAN_LLM_DAILY_LIMIT: "200" })
  assert.deepStrictEqual(limits(store), { sessionLimit: 20, dailyLimit: 100 }, "旧名字不再被读取")
})

test("旧名字一个都不许残留在源码里", function () {
  const root = path.join(__dirname, "..")
  ;["core/llm/client.js", "core/llm/guard.js"].forEach(function (rel) {
    const text = fs.readFileSync(path.join(root, rel), "utf8")
    assert.strictEqual(text.indexOf("ALGO_PLAN_LLM_"), -1, rel + " 里还残留旧环境变量名")
  })
})
