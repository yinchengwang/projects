"use strict"
/**
 * llm-web-bank.test.js —— 题库页与设置页的前端纯逻辑测试（**不联网、不碰 DOM**）
 *
 * 钉住：
 *   1. needs-fill 的题能从列表里挑出来；
 *   2. 造题与补全的请求体形状；
 *   3. 设置请求体只留四个白名单字段，**任何像密钥的字段一律丢掉**（G27）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const Llm = require("../web/llm.js")

test("needsFillList 只挑出缺题面或缺用例的题", function () {
  const list = [
    { id: "lc-00001", title: "正常的题", needsFill: false },
    { id: "lc-00002", title: "缺题面的题", needsFill: true },
    { id: "llm-0000000001", title: "缺用例的题", needsFill: true },
  ]
  const missing = Llm.needsFillList(list)
  assert.equal(missing.length, 2)
  assert.deepEqual(missing.map(function (p) { return p.id }), ["lc-00002", "llm-0000000001"])
  assert.deepEqual(Llm.needsFillList([]), [])
  assert.deepEqual(Llm.needsFillList(null), [])
})

test("造题请求体：轨道、知识点、难度、会话号", function () {
  assert.deepEqual(Llm.generateRequestBody("interview", ["哈希表"], "medium", "s1"), {
    track: "interview",
    topics: ["哈希表"],
    difficulty: "medium",
    sessionId: "s1",
  })
})

test("造题请求体：知识点不是数组就当空的，难度缺省 medium", function () {
  const body = Llm.generateRequestBody("interview", "哈希表", undefined, "s1")
  assert.deepEqual(body.topics, [])
  assert.equal(body.difficulty, "medium")
})

test("补全请求体：题号与会话号", function () {
  assert.deepEqual(Llm.fillRequestBody("lc-00002", "s1"), {
    problemId: "lc-00002",
    sessionId: "s1",
  })
})

test("设置请求体只留四个白名单字段，密钥字段一律丢掉（G27）", function () {
  const body = Llm.settingsRequestBody({
    enabled: true,
    provider: "ark",
    model: "doubao-seed-1-6-251015",
    baseUrl: "https://ark.cn-beijing.volces.com/api/v3",
    apiKey: "super-secret",
    key: "super-secret",
    token: "super-secret",
    ANTHROPIC_API_KEY: "super-secret",
  })
  assert.deepEqual(Object.keys(body).sort(), ["baseUrl", "enabled", "model", "provider"])
  assert.ok(!JSON.stringify(body).includes("super-secret"), "请求体里绝不能出现密钥")
})

test("设置请求体：没传的字段就不出现", function () {
  assert.deepEqual(Llm.settingsRequestBody({ enabled: false }), { enabled: false })
  assert.deepEqual(Llm.settingsRequestBody({}), {})
})

test("密钥来源说明是人话，而且不含密钥本身", function () {
  const 说明 = Llm.describeKeySource({
    effectiveProvider: "ark",
    effectiveModel: "doubao-seed-1-6-251015",
    keySource: "环境变量 ARK_API_KEY",
    reason: "",
  })
  assert.ok(说明.includes("ARK_API_KEY"), "说明里要点名密钥来自哪个变量：" + 说明)
  assert.ok(说明.includes("doubao-seed-1-6-251015"), "说明里要报当前模型：" + 说明)

  const 关闭 = Llm.describeKeySource({ effectiveProvider: "null", reason: "没有配置密钥" })
  assert.ok(关闭.includes("LLM 未启用"), "关掉时要说明未启用：" + 关闭)
  assert.ok(关闭.includes("没有配置密钥"), "关掉时要说明原因：" + 关闭)
})
