"use strict"
/**
 * llm-e2e.test.js —— M6 端到端验收（**注入假 client，全程不联网**）
 *
 * 走一遍完整链路：起服务 → 读题库 → 讲解 → 求助 → 批改思路 → 造题（落盘）→ 补全（写回）。
 * 验 M6 的四条硬线：
 *   1. 五个场景都跑通，响应形状与接口契约 §5 一致；
 *   2. 造出来的新题真的落在 learning/bank/ 里，文件里写着 source: llm；
 *   3. 讲解记下了 model 与提示词摘要（可追溯）；
 *   4. 成本护栏真的拦得住：单次会话第 21 次请求被挡住并提示。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")

/** 造一个带一道题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-e2e-"))
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
      "needs_fill: true",
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

/** 发一个 POST 并把状态码与 JSON 一起返回 */
async function post(base, p, body) {
  const res = await fetch(base + p, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  })
  return { status: res.status, body: await res.json() }
}

test("端到端：五个场景跑通，造题落盘，讲解留痕", async function () {
  const root = tempLearning()
  const client = makeFakeClient()
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    llmClient: client,
    env: {},
    // 只用这个临时 learning 仓，不加载工程仓内置的 Codeforces 题库——
    // 否则题库里会多出 51 道 CF 题，题数断言就不准了。
    localBank: null,
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    // 1. 题库读得到
    const bank = await (await fetch(base + "/api/bank")).json()
    assert.equal(bank.total, 1)

    // 2. 讲解
    const explain = await post(base, "/api/llm/explain", {
      problemId: "lc-00001",
      level: "思路",
      sessionId: "e2e",
    })
    assert.equal(explain.status, 200)
    assert.deepEqual(Object.keys(explain.body).sort(), ["model", "text"])
    assert.equal(explain.body.model, "fake-model")

    // 3. 求助
    const hint = await post(base, "/api/llm/hint", {
      problemId: "lc-00001",
      level: "方向",
      sessionId: "e2e",
    })
    assert.equal(hint.status, 200)
    assert.deepEqual(Object.keys(hint.body).sort(), ["model", "text"])

    // 4. 批改思路
    const grade = await post(base, "/api/llm/grade-idea", {
      problemId: "lc-00001",
      idea: "用哈希表",
      sessionId: "e2e",
    })
    assert.equal(grade.status, 200)
    assert.deepEqual(Object.keys(grade.body).sort(), ["comment", "model", "verdict"])
    assert.equal(grade.body.verdict, "correct")

    // 5. 造题并落盘
    const gen = await post(base, "/api/bank/llm-generate", {
      track: "interview",
      topics: ["双指针"],
      difficulty: "medium",
      sessionId: "e2e",
    })
    assert.equal(gen.status, 200)
    assert.deepEqual(Object.keys(gen.body).sort(), ["model", "problem"])
    assert.equal(gen.body.problem.source, "llm")
    assert.ok(gen.body.problem.id.startsWith("llm-"), "题号应是 llm- 前缀：" + gen.body.problem.id)
    const 新题文件 = path.join(root, "bank", "interview", gen.body.problem.id + ".md")
    assert.ok(fs.existsSync(新题文件), "新题应落盘进 learning/bank：" + 新题文件)
    const 文件内容 = fs.readFileSync(新题文件, "utf8")
    assert.ok(文件内容.includes("source: llm"), "落盘文件里 source 应是 llm")
    assert.ok(文件内容.includes(gen.body.problem.id), "落盘文件里应带着题号")

    // 6. 补全并写回
    const fill = await post(base, "/api/llm/fill", {
      problemId: "lc-00001",
      sessionId: "e2e",
    })
    assert.equal(fill.status, 200)
    assert.equal(fill.body.problem.needsFill, false)
    const 补全后的文件 = fs.readFileSync(path.join(root, "bank", "interview", "lc-00001.md"), "utf8")
    // 原始文件写的是 needs_fill: true，写回后变成 false —— 这才证明文件真被重写了
    assert.ok(补全后的文件.includes("needs_fill: false"), "写回后 needs_fill 应从 true 变成 false")
    assert.ok(!补全后的文件.includes("needs_fill: true"), "写回后不该还留着 needs_fill: true")

    // 7. 讲解留痕：model + 提示词摘要
    const trace = JSON.parse(app.store.getSetting("llm.trace.lc-00001", "[]"))
    assert.ok(trace.length >= 3, "讲解、求助、批改都应留痕，实际 " + trace.length + " 条")
    const 讲解记录 = trace.filter(function (t) { return t.kind === "explain" })[0]
    assert.ok(讲解记录, "讲解应有记录")
    assert.equal(讲解记录.model, "fake-model")
    assert.ok(讲解记录.prompt.digest.length > 0, "记录里应有提示词摘要")
    assert.ok(讲解记录.prompt.length > 0, "记录里应有提示词长度")

    // 8. 配额计数落 settings 表
    assert.equal(app.store.getSetting("llm.quota.session.e2e"), "5")
    const 全部设置 = app.store.allSettings()
    const 每日键 = Object.keys(全部设置).filter(function (k) {
      return k.indexOf("llm.quota.daily.") === 0
    })
    assert.equal(每日键.length, 1, "应正好有一个每日计数键")
    assert.equal(全部设置[每日键[0]], "5")
  } finally {
    await app.close()
  }
})

test("端到端：单次会话第 21 次请求被护栏挡住，提示会话额度用完", async function () {
  const root = tempLearning()
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    llmClient: makeFakeClient(),
    env: {},
    localBank: null,
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    for (let i = 0; i < 20; i += 1) {
      const r = await post(base, "/api/llm/explain", {
        problemId: "lc-00001",
        level: "思路",
        sessionId: "cap",
      })
      assert.equal(r.status, 200, "第 " + (i + 1) + " 次应当成功")
    }
    const 第21次 = await post(base, "/api/llm/explain", {
      problemId: "lc-00001",
      level: "思路",
      sessionId: "cap",
    })
    assert.equal(第21次.status, 429, "第 21 次应被护栏挡住")
    assert.ok(第21次.body.error.includes("本次会话"), "提示里应说明是会话额度用完：" + 第21次.body.error)
  } finally {
    await app.close()
  }
})

test("端到端：造出来的题不会从单题接口泄露参考解法（G14）", async function () {
  const root = tempLearning()
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    llmClient: makeFakeClient(),
    env: {},
    localBank: null,
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    const gen = await post(base, "/api/bank/llm-generate", {
      track: "interview",
      difficulty: "medium",
      sessionId: "g14",
    })
    const res = await fetch(base + "/api/problem/" + gen.body.problem.id)
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.solution, undefined, "单题接口不许返回参考解法")
    assert.equal(body.cases.total, 1)
    assert.ok(body.cases.sample.length <= 2, "样例用例最多给 2 个")
  } finally {
    await app.close()
  }
})
