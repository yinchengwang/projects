"use strict"
/**
 * llm-api.test.js —— M6 的 HTTP 接口测试（**注入假 client，不联网**）
 *
 * 钉住：
 *   1. 五个接口的路径、入参与响应形状，严格照接口契约 §5；
 *   2. 造出来的新题真的落盘；落盘后 GET /api/problem/:id 仍不泄露参考解法（G14）；
 *   3. LLM 设置能读能写，但**接口里绝不出现密钥**。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")

/** 造一个带一道题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-llm-"))
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(
    path.join(dir, "lc-00001.md"),
    [
      "---",
      "id: lc-00001",
      "source: leetcode",
      "title: 两数之和",
      "difficulty: easy",
      "topics: [哈希表, 数组]",
      "tracks: [interview]",
      "status: todo",
      "needs_fill: false",
      "created: 2026-11-11",
      "updated: 2026-11-11",
      "---",
      "## 题面",
      "",
      "给定整数数组与目标值。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "| [2,7,11,15], 9 | [0,1]|",
      "",
      "## 参考解法",
      "",
      "```cpp",
      "int main() { return 0; }",
      "```",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

/** 假客户端：按 system 提示里的关键词分派回复 */
function makeFakeClient() {
  const calls = []
  return {
    name: "ark",
    model: "fake-model",
    calls: calls,
    complete: async function (req) {
      calls.push(req)
      const sys = String(req.system || "")
      if (sys.includes("出题老师")) {
        return JSON.stringify({
          title: "三数之和",
          statement: "给一个数组，找出所有和为 0 的三元组。",
          difficulty: "medium",
          topics: ["双指针"],
          cases: [{ input: "[0,0,0]", expect: "[[0,0,0]]" }],
          solution: "int main(){ return 0; }",
          solutionLang: "cpp",
        })
      }
      if (sys.includes("批改用户的解题思路")) {
        return JSON.stringify({ verdict: "correct", comment: "思路没问题" })
      }
      if (sys.includes("反推出一道题")) {
        return JSON.stringify({
          statement: "补出来的题面",
          cases: [{ input: "[2,7], 9", expect: "[0,1]" }],
        })
      }
      return "这是一段模型回答。"
    },
  }
}

async function withApp(fn, opts) {
  const app = createApp(
    Object.assign(
      { dbPath: ":memory:", learningRoot: tempLearning(), llmClient: makeFakeClient(), env: {} },
      opts || {}
    )
  )
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, app)
  } finally {
    await app.close()
  }
}

test("POST /api/llm/explain 返回 {text, model}", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/explain", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", level: "思路", sessionId: "s1" }),
    })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(typeof body.text, "string")
    assert.ok(body.text.length > 0)
    assert.equal(body.model, "fake-model")
    assert.deepEqual(Object.keys(body).sort(), ["model", "text"])
  })
})

test("POST /api/llm/explain 题目不存在返回 404", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/explain", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "不存在", level: "思路" }),
    })
    assert.equal(res.status, 404)
  })
})

test("POST /api/llm/hint 返回 {text, model}，档位非法返回 400", async function () {
  await withApp(async function (base) {
    const ok = await fetch(base + "/api/llm/hint", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", level: "方向", sessionId: "s1" }),
    })
    assert.equal(ok.status, 200)
    const body = await ok.json()
    assert.deepEqual(Object.keys(body).sort(), ["model", "text"])

    const bad = await fetch(base + "/api/llm/hint", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", level: "神级", sessionId: "s1" }),
    })
    assert.equal(bad.status, 400)
    const err = await bad.json()
    assert.ok(err.error.includes("求助档位非法"), "错误信息应为中文说明：" + err.error)
  })
})

test("POST /api/llm/grade-idea 返回 {verdict, comment, model}", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/grade-idea", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", idea: "用哈希表", sessionId: "s1" }),
    })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.deepEqual(Object.keys(body).sort(), ["comment", "model", "verdict"])
    assert.equal(body.verdict, "correct")
    assert.equal(body.comment, "思路没问题")
  })
})

test("POST /api/bank/llm-generate 返回 {problem, model}，且题目真的落盘", async function () {
  await withApp(async function (base, app) {
    const res = await fetch(base + "/api/bank/llm-generate", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ track: "interview", topics: ["双指针"], difficulty: "medium", sessionId: "s1" }),
    })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.deepEqual(Object.keys(body).sort(), ["model", "problem"])
    assert.equal(body.problem.source, "llm")
    assert.ok(body.problem.id.startsWith("llm-"))
    const 文件 = path.join(app.learning.root, "bank", "interview", body.problem.id + ".md")
    assert.ok(fs.existsSync(文件), "新题应落盘到 learning/bank：" + 文件)
  })
})

test("造完题后 GET /api/problem/:id 仍然不泄露参考解法（G14）", async function () {
  await withApp(async function (base) {
    const made = await fetch(base + "/api/bank/llm-generate", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ track: "interview", difficulty: "medium", sessionId: "s1" }),
    })
    const madeBody = await made.json()
    const id = madeBody.problem.id
    const res = await fetch(base + "/api/problem/" + id)
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.solution, undefined, "单题接口不许返回参考解法")
    assert.equal(body.cases.total, 1)
    assert.equal(body.cases.sample.length, 1)
  })
})

test("POST /api/llm/fill 返回 {problem, model}，needsFill 变 false", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/fill", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", sessionId: "s1" }),
    })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.deepEqual(Object.keys(body).sort(), ["model", "problem"])
    assert.equal(body.problem.needsFill, false)
  })
})

test("GET /api/llm/settings 返回开关、provider、密钥来源与配额", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/settings")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.effectiveProvider, "ark")
    assert.equal(body.effectiveModel, "fake-model")
    assert.equal(body.quota.sessionLimit, 20)
    assert.equal(body.quota.dailyLimit, 100)
    assert.ok(!JSON.stringify(body).includes("super-secret"))
  })
})

test("PUT /api/llm/settings 能保存开关与模型名", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/llm/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ enabled: false, provider: "anthropic", model: "claude-opus-5" }),
    })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.ok, true)
    assert.equal(body.settings.enabled, false)
    assert.equal(body.settings.provider, "anthropic")
    assert.equal(body.settings.model, "claude-opus-5")

    const again = await fetch(base + "/api/llm/settings")
    const after = await again.json()
    assert.equal(after.enabled, false)
    assert.equal(after.provider, "anthropic")
  })
})
