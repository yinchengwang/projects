"use strict"
/**
 * llm-client.test.js —— LLM 客户端测试（**注入假 fetch，全程不联网**）
 *
 * 钉住：
 *   1. 关闭态（NullClient）返回明确中文提示且不抛错；
 *   2. provider 与密钥的解析顺序：显式 > 环境变量 > 按密钥推断；
 *   3. 方舟走 OpenAI 兼容的 /chat/completions，Anthropic 走 /v1/messages；
 *   4. Anthropic **不发 temperature**（claude-opus-5 已移除采样参数，传了 400）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const {
  createLlmClient,
  ArkClient,
  AnthropicClient,
  NullClient,
  keySourceOf,
  ARK_DEFAULT_BASE_URL,
  ARK_DEFAULT_MODEL,
  ANTHROPIC_DEFAULT_BASE_URL,
  ANTHROPIC_DEFAULT_MODEL,
} = require("../core/llm/client.js")

/** 造一个假 fetch：记录请求，返回指定响应。测试全程不发起真实请求。 */
function makeFakeFetch(payload, status) {
  const records = []
  const fn = async function (url, options) {
    records.push({ url: url, options: options })
    const text = typeof payload === "string" ? payload : JSON.stringify(payload)
    const code = status === undefined ? 200 : status
    return {
      ok: code < 400,
      status: code,
      text: async function () { return text },
    }
  }
  fn.records = records
  return fn
}

test("NullClient 返回明确的中文提示，且不抛错", async function () {
  const client = new NullClient("没有配置密钥")
  assert.equal(client.name, "null")
  const text = await client.complete({ system: "s", user: "u" })
  assert.equal(typeof text, "string")
  assert.ok(text.includes("LLM 未启用"), "提示里应写明 LLM 未启用")
  assert.ok(text.includes("没有配置密钥"), "提示里应写明关闭原因")
  assert.ok(text.includes("不受影响"), "提示里应说明其余功能不受影响")
})

test("createLlmClient：没有任何配置时降级成 null 客户端", function () {
  const client = createLlmClient({ env: {} })
  assert.equal(client.name, "null")
})

test("createLlmClient：有 ARK_API_KEY 就用 ark", function () {
  const client = createLlmClient({ env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "ark")
  assert.equal(client.model, ARK_DEFAULT_MODEL)
})

test("createLlmClient：有 ANTHROPIC_API_KEY 就用 anthropic", function () {
  const client = createLlmClient({ env: { ANTHROPIC_API_KEY: "k" } })
  assert.equal(client.name, "anthropic")
  assert.equal(client.model, ANTHROPIC_DEFAULT_MODEL)
})

test("createLlmClient：显式 provider 优先于环境变量推断", function () {
  const client = createLlmClient({ provider: "anthropic", env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "anthropic")
})

test("createLlmClient：provider 指定了但没有密钥，降级成 null 而不是抛错", function () {
  const client = createLlmClient({ provider: "ark", env: {} })
  assert.equal(client.name, "null")
  assert.ok(client.reason.includes("API Key"), "关闭原因里应提到 API Key")
})

test("createLlmClient：ALGO_PLAN_LLM_ENABLED=0 强制关闭", function () {
  const client = createLlmClient({ env: { ARK_API_KEY: "k", ALGO_PLAN_LLM_ENABLED: "0" } })
  assert.equal(client.name, "null")
})

test("createLlmClient：enabled=false 显式关闭", function () {
  const client = createLlmClient({ enabled: false, env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "null")
})

test("createLlmClient：provider 写 null 强制关闭", function () {
  const client = createLlmClient({ provider: "null", env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "null")
})

test("createLlmClient：未知 provider 降级成 null 而不是抛错", function () {
  const client = createLlmClient({ provider: "gemini", env: { ARK_API_KEY: "k" } })
  assert.equal(client.name, "null")
  assert.ok(client.reason.includes("provider"), "关闭原因里应提到 provider")
})

test("createLlmClient：环境变量可以覆盖地址与模型", function () {
  const client = createLlmClient({
    env: { ARK_API_KEY: "k", ARK_BASE_URL: "https://ark.test/api/v3", ALGO_PLAN_LLM_MODEL: "my-model" },
  })
  assert.equal(client.name, "ark")
  assert.equal(client.baseUrl, "https://ark.test/api/v3")
  assert.equal(client.model, "my-model")
})

test("默认地址与模型名固定", function () {
  assert.equal(ARK_DEFAULT_BASE_URL, "https://ark.cn-beijing.volces.com/api/v3")
  assert.equal(ANTHROPIC_DEFAULT_BASE_URL, "https://api.anthropic.com")
  assert.equal(ARK_DEFAULT_MODEL, "doubao-seed-1-6-251015")
  assert.equal(ANTHROPIC_DEFAULT_MODEL, "claude-opus-5")
})

test("ArkClient：按方舟的 OpenAI 兼容形状发请求，读 choices[0].message.content", async function () {
  const fake = makeFakeFetch({ choices: [{ message: { content: "  答案  " } }] })
  const client = new ArkClient({ apiKey: "secret", fetchImpl: fake })
  const text = await client.complete({ system: "系统提示", user: "用户提示", temperature: 0.4 })
  assert.equal(text, "答案")
  assert.equal(fake.records.length, 1)
  const req = fake.records[0]
  assert.equal(req.url, ARK_DEFAULT_BASE_URL + "/chat/completions")
  assert.equal(req.options.method, "POST")
  assert.equal(req.options.headers.authorization, "Bearer secret")
  assert.equal(req.options.headers["content-type"], "application/json")
  const body = JSON.parse(req.options.body)
  assert.equal(body.model, ARK_DEFAULT_MODEL)
  assert.deepEqual(body.messages, [
    { role: "system", content: "系统提示" },
    { role: "user", content: "用户提示" },
  ])
  assert.equal(body.temperature, 0.4)
  assert.equal(body.stream, false)
})

test("ArkClient：没给 temperature 就不发这个字段", async function () {
  const fake = makeFakeFetch({ choices: [{ message: { content: "x" } }] })
  const client = new ArkClient({ apiKey: "secret", fetchImpl: fake })
  await client.complete({ system: "s", user: "u" })
  const body = JSON.parse(fake.records[0].options.body)
  assert.ok(!("temperature" in body), "没给 temperature 时不该发这个字段")
})

test("ArkClient：baseUrl 与 model 可以覆盖", async function () {
  const fake = makeFakeFetch({ choices: [{ message: { content: "x" } }] })
  const client = new ArkClient({
    apiKey: "secret",
    baseUrl: "https://example.test/v1/",
    model: "my-model",
    fetchImpl: fake,
  })
  await client.complete({ system: "s", user: "u" })
  const req = fake.records[0]
  assert.equal(req.url, "https://example.test/v1/chat/completions")
  assert.equal(JSON.parse(req.options.body).model, "my-model")
})

test("AnthropicClient：按 Messages API 形状发请求，读 content[] 里的 text", async function () {
  const fake = makeFakeFetch({
    content: [
      { type: "text", text: "第一段" },
      { type: "text", text: "第二段" },
    ],
  })
  const client = new AnthropicClient({ apiKey: "secret", fetchImpl: fake })
  const text = await client.complete({ system: "系统提示", user: "用户提示", temperature: 0.4 })
  assert.equal(text, "第一段\n第二段")
  const req = fake.records[0]
  assert.equal(req.url, ANTHROPIC_DEFAULT_BASE_URL + "/v1/messages")
  assert.equal(req.options.headers["x-api-key"], "secret")
  assert.equal(req.options.headers["anthropic-version"], "2023-06-01")
  const body = JSON.parse(req.options.body)
  assert.equal(body.model, ANTHROPIC_DEFAULT_MODEL)
  assert.equal(body.system, "系统提示")
  assert.deepEqual(body.messages, [{ role: "user", content: "用户提示" }])
  assert.ok(!("temperature" in body), "claude-opus-5 已移除采样参数，不能发 temperature")
})

test("AnthropicClient：忽略非 text 类型的 content 块", async function () {
  const fake = makeFakeFetch({
    content: [{ type: "thinking", thinking: "…" }, { type: "text", text: "正文" }],
  })
  const client = new AnthropicClient({ apiKey: "secret", fetchImpl: fake })
  const text = await client.complete({ system: "s", user: "u" })
  assert.equal(text, "正文")
})

test("客户端在接口报错时抛出带状态码的中文错误", async function () {
  const fake = makeFakeFetch({ error: { message: "InvalidEndpointOrModel.NotFound" } }, 404)
  const client = new ArkClient({ apiKey: "secret", fetchImpl: fake })
  await assert.rejects(
    function () { return client.complete({ system: "s", user: "u" }) },
    function (err) {
      assert.ok(err instanceof Error)
      assert.ok(err.message.includes("404"), "错误信息里应带状态码")
      assert.ok(err.message.includes("InvalidEndpointOrModel.NotFound"), "错误信息里应带上游返回的详情")
      return true
    }
  )
})

test("客户端在响应里没有文本时抛出中文错误", async function () {
  const fake = makeFakeFetch({ choices: [] })
  const client = new ArkClient({ apiKey: "secret", fetchImpl: fake })
  await assert.rejects(function () { return client.complete({ system: "s", user: "u" }) }, /没有返回文本/)
})

test("keySourceOf 说明密钥来源，绝不回显密钥本身", function () {
  assert.equal(keySourceOf("ark", { ARK_API_KEY: "secret" }), "环境变量 ARK_API_KEY")
  assert.equal(keySourceOf("anthropic", { ANTHROPIC_API_KEY: "secret" }), "环境变量 ANTHROPIC_API_KEY")
  assert.ok(keySourceOf("ark", {}).includes("未配置"))
  assert.ok(!keySourceOf("ark", { ARK_API_KEY: "secret" }).includes("secret"))
})
