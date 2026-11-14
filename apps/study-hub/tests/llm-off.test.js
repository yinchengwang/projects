"use strict"
/**
 * llm-off.test.js —— 「把 LLM 关掉，app 仍然完整可用」的专项测试（**不联网**）
 *
 * 这是设计文档 §6.1 的硬要求（G23），本文件是它的验收证据。
 * 覆盖：
 *   1. 没有密钥 / 环境变量关掉 / 设置页关掉，三种关法都降级成 NullClient；
 *   2. 关掉后 app 起得来，页面、题库、单题接口一个都不崩；
 *   3. LLM 接口返回明确的中文提示（200）或 503 中文错误，**不返回 500**；
 *   4. 关掉时题库一个文件都不多写（造题、补全都不落盘）。
 *
 * 联网纪律（G28）：本文件里的 fetch 只打本进程自己 listen(0) 起的本地回环地址，
 * 绝不请求任何外部域名；LLM 客户端一律走关闭态（NullClient），不发请求。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const { createLlmClient, NullClient } = require("../core/llm/client.js")

/** 造一个带一道题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-off-"))
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

/** 数一数题库目录里有多少个 md 文件 */
function countBankFiles(root) {
  const dir = path.join(root, "bank")
  let total = 0
  for (const name of fs.readdirSync(dir)) {
    const sub = path.join(dir, name)
    if (fs.statSync(sub).isDirectory()) {
      total += fs.readdirSync(sub).filter(function (f) { return f.endsWith(".md") }).length
    }
  }
  return total
}

/** 题库里所有 md 文件的「路径 -> 内容」快照，用来证明文件没被动过 */
function snapshotBank(root) {
  const dir = path.join(root, "bank")
  const snap = {}
  for (const name of fs.readdirSync(dir)) {
    const sub = path.join(dir, name)
    if (!fs.statSync(sub).isDirectory()) continue
    for (const f of fs.readdirSync(sub)) {
      if (!f.endsWith(".md")) continue
      snap[path.join(name, f)] = fs.readFileSync(path.join(sub, f), "utf8")
    }
  }
  return snap
}

/**
 * 起一个**没有 LLM** 的 app：env 里没有任何密钥。
 * localBank 显式传 null：不加载工程仓内置题库，题库计数才是确定的（测试隔离用）。
 */
async function withOffApp(fn) {
  const root = tempLearning()
  const app = createApp({ dbPath: ":memory:", learningRoot: root, env: {}, localBank: null })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, app, root)
  } finally {
    await app.close()
  }
}

/** 发一个 LLM 场景接口的请求 */
function post(base, url, body) {
  return fetch(base + url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  })
}

test("没有密钥时降级成 NullClient，返回明确的中文提示", async function () {
  const client = createLlmClient({ env: {} })
  assert.equal(client.name, "null")
  assert.ok(client instanceof NullClient)
  const text = await client.complete({ system: "s", user: "u" })
  assert.ok(text.includes("LLM 未启用"), "提示里应写明 LLM 未启用：" + text)
  assert.ok(text.includes("不受影响"), "提示里应说明其余功能不受影响：" + text)
})

test("三种关法都降级成 NullClient：没密钥、环境变量关、设置页关", function () {
  assert.equal(createLlmClient({ env: {} }).name, "null")
  assert.equal(createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_ENABLED: "0" } }).name, "null")
  assert.equal(createLlmClient({ enabled: false, env: { ARK_API_KEY: "k" } }).name, "null")
})

test("三种关法分别是三种原因，且都说明其余功能不受影响", async function () {
  const noKey = createLlmClient({ env: {} })
  const byEnv = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_ENABLED: "0" } })
  const bySetting = createLlmClient({ enabled: false, env: { ARK_API_KEY: "k" } })
  const all = [noKey, byEnv, bySetting]
  all.forEach(function (c) {
    assert.ok(c instanceof NullClient, "三种关法都该是 NullClient")
    assert.ok(String(c.reason).length > 0, "每种关法都要说清楚为什么关掉了")
  })
  const texts = await Promise.all(all.map(function (c) { return c.complete({ system: "s", user: "u" }) }))
  texts.forEach(function (t) {
    assert.ok(t.includes("LLM 未启用"), "提示里应写明 LLM 未启用：" + t)
    assert.ok(t.includes("不受影响"), "提示里应说明其余功能不受影响：" + t)
  })
  // 三种关法的理由要能区分开，别都糊成同一句
  assert.notEqual(noKey.reason, byEnv.reason)
  assert.notEqual(byEnv.reason, bySetting.reason)
})

test("有密钥且开关开着时**不会**降级（证明上面的降级不是无脑降级）", function () {
  const client = createLlmClient({ env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "ark", "有密钥、开关开着时应该建出真客户端")
})

test("LLM 关着时 app 照常起得来：页面、题库、单题接口全都不崩", async function () {
  await withOffApp(async function (base) {
    const page = await fetch(base + "/")
    assert.equal(page.status, 200, "首页应该照常能打开")

    const health = await fetch(base + "/api/health")
    assert.equal(health.status, 200)
    const healthBody = await health.json()
    assert.equal(healthBody.ok, true)
    assert.equal(healthBody.bank.problemCount, 1)

    const bank = await fetch(base + "/api/bank")
    assert.equal(bank.status, 200)
    const bankBody = await bank.json()
    assert.equal(bankBody.total, 1)

    const one = await fetch(base + "/api/problem/lc-00001")
    assert.equal(one.status, 200)
    const oneBody = await one.json()
    assert.equal(oneBody.id, "lc-00001")
  })
})

test("LLM 关着时，讲解与求助返回明确的中文提示，不返回 500", async function () {
  await withOffApp(async function (base) {
    const explain = await post(base, "/api/llm/explain", { problemId: "lc-00001", level: "思路" })
    assert.equal(explain.status, 200)
    const explainBody = await explain.json()
    assert.ok(explainBody.text.includes("LLM 未启用"), "应返回明确的中文提示：" + explainBody.text)
    assert.ok(explainBody.text.includes("不受影响"), "应说明其余功能不受影响：" + explainBody.text)

    const hint = await post(base, "/api/llm/hint", { problemId: "lc-00001", level: "方向" })
    assert.equal(hint.status, 200)
    const hintBody = await hint.json()
    assert.ok(hintBody.text.includes("LLM 未启用"))
  })
})

test("LLM 关着时，批改思路返回 unknown 与中文提示，不返回 500", async function () {
  await withOffApp(async function (base) {
    const res = await post(base, "/api/llm/grade-idea", { problemId: "lc-00001", idea: "用哈希表" })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.verdict, "unknown")
    assert.ok(body.comment.includes("LLM 未启用"))
  })
})

test("LLM 关着时，造题与补全返回 503 中文错误，题库一个文件都不多写", async function () {
  await withOffApp(async function (base, app, root) {
    const before = countBankFiles(root)
    const snapBefore = snapshotBank(root)

    const gen = await post(base, "/api/bank/llm-generate", { track: "interview", difficulty: "medium" })
    assert.equal(gen.status, 503, "应返回 503，不是 500")
    const genBody = await gen.json()
    assert.ok(typeof genBody.error === "string" && genBody.error.length > 0, "应给出中文错误说明")
    assert.ok(genBody.error.includes("LLM 未启用"), "错误说明应写明 LLM 未启用：" + genBody.error)

    const fill = await post(base, "/api/llm/fill", { problemId: "lc-00001" })
    assert.equal(fill.status, 503, "应返回 503，不是 500")
    const fillBody = await fill.json()
    assert.ok(typeof fillBody.error === "string" && fillBody.error.length > 0, "应给出中文错误说明")
    assert.ok(fillBody.error.includes("LLM 未启用"), "错误说明应写明 LLM 未启用：" + fillBody.error)

    assert.equal(countBankFiles(root), before, "LLM 关着时题库不该多出文件")
    assert.deepEqual(snapshotBank(root), snapBefore, "LLM 关着时已有题库文件不该被改动")
  })
})

test("设置页把 LLM 关掉之后，所有 LLM 接口依然是中文提示，不是 500", async function () {
  await withOffApp(async function (base) {
    const put = await fetch(base + "/api/llm/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ enabled: false }),
    })
    assert.equal(put.status, 200)

    const info = await fetch(base + "/api/llm/settings")
    const infoBody = await info.json()
    assert.equal(infoBody.enabled, false)
    assert.equal(infoBody.effectiveProvider, "null")
    assert.ok(infoBody.reason.length > 0, "应说明为什么没启用")

    const res = await post(base, "/api/llm/explain", { problemId: "lc-00001", level: "思路" })
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.ok(body.text.includes("LLM 未启用"))
  })
})

test("LLM 关着时，GET /api/llm/settings 也能正常返回（200，不泄露密钥）", async function () {
  await withOffApp(async function (base) {
    const res = await fetch(base + "/api/llm/settings")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.effectiveProvider, "null")
    assert.ok(body.reason.length > 0, "应说明为什么没启用")
    assert.equal(body.quota.sessionLimit, 20)
    assert.equal(body.quota.dailyLimit, 100)
    assert.ok(!JSON.stringify(body).includes("apiKey"), "响应里不许出现密钥字段")
  })
})

test("LLM 关着时，五个场景接口都不返回 500", async function () {
  await withOffApp(async function (base) {
    const cases = [
      { url: "/api/llm/explain", body: { problemId: "lc-00001", level: "思路" } },
      { url: "/api/llm/hint", body: { problemId: "lc-00001", level: "方向" } },
      { url: "/api/llm/grade-idea", body: { problemId: "lc-00001", idea: "用哈希表" } },
      { url: "/api/bank/llm-generate", body: { track: "interview", difficulty: "medium" } },
      { url: "/api/llm/fill", body: { problemId: "lc-00001" } },
    ]
    for (const c of cases) {
      const res = await post(base, c.url, c.body)
      assert.notEqual(res.status, 500, c.url + " 不该返回 500")
      assert.ok(res.status === 200 || res.status === 503, c.url + " 只该返回 200 或 503，实际 " + res.status)
    }
  })
})

test("LLM 关着时，题库读取与 health 都不受任何影响", async function () {
  await withOffApp(async function (base) {
    const first = await fetch(base + "/api/bank")
    const second = await fetch(base + "/api/bank")
    assert.equal(first.status, 200)
    assert.equal(second.status, 200)
    const a = await first.json()
    const b = await second.json()
    assert.deepEqual(a, b, "题库内容不该因为 LLM 关着而变化")
  })
})

test("LLM 关着时，单题接口仍不泄露参考解法（G14 不受 LLM 开关影响）", async function () {
  await withOffApp(async function (base) {
    const res = await fetch(base + "/api/problem/lc-00001")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.solution, undefined, "单题接口不许返回参考解法")
    assert.equal(body.cases.total, 1)
    assert.equal(body.cases.sample.length, 1)
  })
})
