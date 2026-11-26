"use strict"
/**
 * script-timeout.test.js —— 断言脚本的超时第二档
 *
 * AI Infra 实验真实耗时 60–600 秒，而用户提交代码的超时上限必须留在 60 秒。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const sandbox = require("../judge/sandbox.js")

test("MAX_TIMEOUT_MS 仍然是 60000（用户代码的上限不动）", function () {
  assert.strictEqual(sandbox.MAX_TIMEOUT_MS, 60000)
})

test("MAX_SCRIPT_TIMEOUT_MS 是 600000（脚本 10 分钟）", function () {
  assert.strictEqual(sandbox.MAX_SCRIPT_TIMEOUT_MS, 600000)
})

test("clampTimeoutMs 不给上限时行为与从前一致", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(1500), 1500)
  assert.strictEqual(sandbox.clampTimeoutMs(999999), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(0), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs(-5), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs("abc"), sandbox.DEFAULT_TIMEOUT_MS)
})

test("clampTimeoutMs 给上限时按上限夹", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(300000, 600000), 300000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, 600000), 600000)
})

test("clampTimeoutMs 的上限非法时退回默认上限", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(999999, 0), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, "abc"), 60000)
  assert.strictEqual(sandbox.clampTimeoutMs(999999, -1), 60000)
})
