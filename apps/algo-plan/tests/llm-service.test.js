"use strict"
/**
 * llm-service.test.js —— LLM 服务层测试（**注入假 client，不联网**）
 *
 * 钉住：
 *   1. 五个场景的行为与返回形状；
 *   2. 不主动泄题：service 构造的提示词里没有参考解法；
 *   3. 产出可追溯：造的新题真的落盘进 bank（source=llm，id 走 makeLlmId），
 *      讲解记下 model + prompt 摘要；
 *   4. 成本护栏：超配额直接停并提示「今天用完了」；
 *   5. LLM 关着：explain/hint/gradeIdea 返回明确中文提示，generate/fill 报 503，都不崩。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const { createLlmService } = require("../core/llm/service.js")

/** 最小假 store：只实现 settings 表的两个方法 */
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

/** 假题库 provider：把题存在内存 Map 里 */
function makeProvider() {
  const files = new Map()
  return {
    files: files,
    list: async function () { return Array.from(files.values()) },
    get: async function (id) { return files.get(id) },
    put: async function (problem) {
      files.set(problem.id, problem)
      return "learning/bank/" + (problem.tracks[0] || "misc") + "/" + problem.id + ".md"
    },
    stats: async function () {
      return { fileCount: files.size, problemCount: files.size, warnings: [] }
    },
  }
}

/** 假客户端：记录每次请求，返回预置回复 */
function makeClient(reply, name) {
  const calls = []
  return {
    name: name || "ark",
    model: "fake-model",
    calls: calls,
    complete: async function (req) {
      calls.push(req)
      return typeof reply === "function" ? reply(req) : reply
    },
  }
}

/** 一道普通题 */
function makeProblem() {
  return {
    id: "lc-00001",
    source: "leetcode",
    title: "两数之和",
    difficulty: "easy",
    topics: ["哈希表"],
    tracks: ["interview"],
    status: "todo",
    needsFill: false,
    created: "2026-11-01",
    updated: "2026-11-01",
    statement: "给定整数数组 nums 和整数 target，找出和为 target 的两个整数的下标。",
    cases: [{ input: "[2,7,11,15], 9", expect: "[0,1]", tol: null }],
    solution: "int main(){ /* 参考答案 */ return 0; }",
    solutionLang: "cpp",
    extraSections: [],
  }
}

/** 造一个服务层 */
function makeService(reply, options) {
  const o = options || {}
  const store = o.store || makeStore()
  const provider = o.provider || makeProvider()
  const client = o.client || makeClient(reply)
  const service = createLlmService({
    client: client,
    store: store,
    provider: provider,
    env: o.env || {},
    now: o.now || function () { return new Date("2026-11-11T08:00:00Z") },
  })
  return { service: service, store: store, provider: provider, client: client }
}

test("explain：返回文本，并记下 model 与 prompt 摘要", async function () {
  const s = makeService("先扫一遍数组，用哈希表存见过的数。")
  const result = await s.service.explain({ problem: makeProblem(), level: "思路", sessionId: "s1" })
  assert.equal(result.text, "先扫一遍数组，用哈希表存见过的数。")
  assert.equal(result.model, "fake-model")
  const trace = JSON.parse(s.store.getSetting("llm.trace.lc-00001", "[]"))
  assert.equal(trace.length, 1)
  assert.equal(trace[0].kind, "explain")
  assert.equal(trace[0].level, "思路")
  assert.equal(trace[0].model, "fake-model")
  assert.ok(trace[0].prompt.digest.length > 0, "trace 里应记 prompt 摘要")
  assert.ok(trace[0].prompt.length > 0)
})

test("不主动泄题：service 发出去的提示词里没有参考解法", async function () {
  const s = makeService("随便讲讲")
  await s.service.explain({ problem: makeProblem(), level: "思路", sessionId: "s1" })
  await s.service.hint({ problem: makeProblem(), level: "方向", sessionId: "s1" })
  assert.equal(s.client.calls.length, 2)
  for (const call of s.client.calls) {
    assert.ok(!call.user.includes("参考答案"), "提示词里不该出现参考解法")
    assert.ok(!call.user.includes("int main"), "提示词里不该出现参考解法代码")
    assert.ok(!call.system.includes("参考答案"), "系统提示里不该出现参考解法")
  }
})

test("只有显式选『代码』档才放开给代码", async function () {
  const s = makeService("讲思路的正文")
  await s.service.explain({ problem: makeProblem(), level: "思路", sessionId: "s1" })
  const 思路档 = s.client.calls[0]
  assert.ok(思路档.system.includes("绝不给出任何可编译的完整代码"))

  await s.service.explain({ problem: makeProblem(), level: "代码", sessionId: "s1" })
  const 代码档 = s.client.calls[1]
  assert.ok(代码档.system.includes("本次允许给出完整代码"))
})

test("hint：档位非法就报错，合法就返回文本", async function () {
  const s = makeService("提示内容")
  const r = await s.service.hint({ problem: makeProblem(), level: "伪码", sessionId: "s1" })
  assert.equal(r.text, "提示内容")
  await assert.rejects(
    function () { return s.service.hint({ problem: makeProblem(), level: "神级", sessionId: "s1" }) },
    function (err) { return err.status === 400 }
  )
})

test("gradeIdea：解析出 verdict 与 comment", async function () {
  const s = makeService('{"verdict":"partial","comment":"思路对，但漏了重复元素"}')
  const r = await s.service.gradeIdea({ problem: makeProblem(), idea: "用两个指针", sessionId: "s1" })
  assert.equal(r.verdict, "partial")
  assert.equal(r.comment, "思路对，但漏了重复元素")
  assert.equal(r.model, "fake-model")
})

test("gradeIdea：模型不给 JSON 时返回 unknown，不崩", async function () {
  const s = makeService("看起来挺有道理的")
  const r = await s.service.gradeIdea({ problem: makeProblem(), idea: "用两个指针", sessionId: "s1" })
  assert.equal(r.verdict, "unknown")
  assert.ok(r.comment.length > 0)
})

test("gradeIdea：思路为空就报 400", async function () {
  const s = makeService("{}")
  await assert.rejects(
    function () { return s.service.gradeIdea({ problem: makeProblem(), idea: "  ", sessionId: "s1" }) },
    function (err) { return err.status === 400 }
  )
})

test("generate：新题落盘进 bank，source 是 llm，id 走 makeLlmId", async function () {
  const s = makeService(JSON.stringify({
    title: "三数之和",
    statement: "给一个数组，找出所有和为 0 的三元组。",
    difficulty: "medium",
    topics: ["双指针"],
    cases: [
      { input: "[-1,0,1,2,-1,-4]", expect: "[[-1,-1,2],[-1,0,1]]" },
      { input: "[0,0,0]", expect: "[[0,0,0]]" },
    ],
    solution: "int main(){ return 0; }",
    solutionLang: "cpp",
  }))
  const r = await s.service.generate({ track: "interview", topics: ["双指针"], difficulty: "medium", sessionId: "s1" })
  assert.equal(r.problem.source, "llm")
  assert.ok(r.problem.id.startsWith("llm-"), "id 应是 llm- 前缀：" + r.problem.id)
  assert.deepEqual(r.problem.tracks, ["interview"])
  assert.equal(r.problem.needsFill, false)
  assert.equal(s.provider.files.size, 1, "新题应真的落进题库")
  assert.ok(s.provider.files.has(r.problem.id))
})

test("generate：同一句题面再生成一次要报错，不覆盖已有文件", async function () {
  const 回复 = JSON.stringify({
    title: "三数之和",
    statement: "给一个数组，找出所有和为 0 的三元组。",
    difficulty: "medium",
    topics: ["双指针"],
    cases: [{ input: "[0]", expect: "[]" }],
    solution: "int main(){ return 0; }",
  })
  const s = makeService(回复)
  await s.service.generate({ track: "interview", difficulty: "medium", sessionId: "s1" })
  await assert.rejects(
    function () { return s.service.generate({ track: "interview", difficulty: "medium", sessionId: "s1" }) },
    function (err) { return err.status === 400 && err.message.includes("已经生成过") }
  )
  assert.equal(s.provider.files.size, 1, "重复生成不该多出文件")
})

test("generate：轨道非法就报 400", async function () {
  const s = makeService("{}")
  await assert.rejects(
    function () { return s.service.generate({ track: "不存在", difficulty: "easy", sessionId: "s1" }) },
    function (err) { return err.status === 400 }
  )
})

test("fill：补上缺失的题面与用例，needsFill 变 false 并落盘", async function () {
  const 题 = makeProblem()
  题.statement = ""
  题.cases = []
  题.needsFill = true
  const s = makeService(JSON.stringify({
    statement: "补出来的题面：从数组里找两个数。",
    cases: [{ input: "[2,7], 9", expect: "[0,1]" }],
  }))
  const r = await s.service.fill({ problem: 题, sessionId: "s1" })
  assert.equal(r.problem.statement, "补出来的题面：从数组里找两个数。")
  assert.equal(r.problem.cases.length, 1)
  assert.equal(r.problem.needsFill, false)
  assert.equal(s.provider.files.get(题.id).statement, r.problem.statement, "补全结果应落盘")
})

test("fill：本来就有题面与用例时保留原来的，只补缺口", async function () {
  const 题 = makeProblem()
  题.needsFill = true
  const s = makeService(JSON.stringify({
    statement: "模型另写的题面",
    cases: [{ input: "999", expect: "0" }],
  }))
  const r = await s.service.fill({ problem: 题, sessionId: "s1" })
  assert.equal(r.problem.statement, 题.statement, "已有的题面不该被改写")
  assert.equal(r.problem.cases.length, 1)
  assert.equal(r.problem.cases[0].input, "[2,7,11,15], 9", "已有的用例不该被改写")
})

test("成本护栏：每日额度用完时直接停，提示『今天用完了』", async function () {
  const store = makeStore({ "llm.quota.daily_limit": "0" })
  const s = makeService("随便", { store: store })
  await assert.rejects(
    function () { return s.service.explain({ problem: makeProblem(), level: "思路", sessionId: "s1" }) },
    function (err) {
      assert.equal(err.status, 429)
      assert.ok(err.message.includes("今天用完了"), "提示里应包含『今天用完了』：" + err.message)
      return true
    }
  )
})

test("成本护栏：会话额度用完时直接停", async function () {
  const store = makeStore({ "llm.quota.session_limit": "0" })
  const s = makeService("随便", { store: store })
  await assert.rejects(
    function () { return s.service.hint({ problem: makeProblem(), level: "方向", sessionId: "s1" }) },
    function (err) { return err.status === 429 }
  )
})

test("LLM 关着：explain / hint / gradeIdea 返回明确中文提示，不抛错", async function () {
  const client = makeClient("不会用到", "null")
  client.reason = "没有配置密钥"
  const s = makeService(null, { client: client })
  const e = await s.service.explain({ problem: makeProblem(), level: "思路", sessionId: "s1" })
  assert.ok(e.text.includes("LLM 未启用"), "提示里应写明 LLM 未启用：" + e.text)
  assert.ok(e.text.includes("不受影响"))

  const h = await s.service.hint({ problem: makeProblem(), level: "方向", sessionId: "s1" })
  assert.ok(h.text.includes("LLM 未启用"))

  const g = await s.service.gradeIdea({ problem: makeProblem(), idea: "用哈希表", sessionId: "s1" })
  assert.equal(g.verdict, "unknown")
  assert.ok(g.comment.includes("LLM 未启用"))
  assert.equal(client.calls.length, 0, "LLM 关着时不该发起任何请求")
})

test("LLM 关着：generate / fill 报 503 并且不写任何文件", async function () {
  const client = makeClient("不会用到", "null")
  client.reason = "没有配置密钥"
  const s = makeService(null, { client: client })
  await assert.rejects(
    function () { return s.service.generate({ track: "interview", difficulty: "easy", sessionId: "s1" }) },
    function (err) { return err.status === 503 }
  )
  await assert.rejects(
    function () { return s.service.fill({ problem: makeProblem(), sessionId: "s1" }) },
    function (err) { return err.status === 503 }
  )
  assert.equal(s.provider.files.size, 0, "LLM 关着时不该写题库")
})

test("describe：报告当前开关、provider、密钥来源与配额，不泄露密钥", async function () {
  const s = makeService("x", { env: { ARK_API_KEY: "super-secret" } })
  const d = s.service.describe("s1")
  assert.equal(d.effectiveProvider, "ark")
  assert.equal(d.keySource, "环境变量 ARK_API_KEY")
  assert.ok(!JSON.stringify(d).includes("super-secret"), "describe 里绝不能出现密钥本身")
  assert.equal(d.quota.sessionLimit, 20)
  assert.equal(d.quota.dailyLimit, 100)
})
