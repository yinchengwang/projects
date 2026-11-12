"use strict"
/**
 * client.js —— LLM 客户端：ArkClient / AnthropicClient / NullClient
 *
 * 统一接口（接口契约 §5）：
 *   client.complete({system, user, temperature}) -> Promise<string>
 *   client.name -> "ark" | "anthropic" | "null"
 *
 * 零 npm 依赖，用 Node 24 内置的全局 fetch。
 *
 * 调用形状的来源（实地勘察出来的，不是凭记忆写的）：
 *   方舟：arkcli 自带示例 C:/Users/yinch/.claude/skills/arkcli-code-example/assets/demo-code/python.py
 *         base https://ark.cn-beijing.volces.com/api/v3，
 *         POST {base}/chat/completions，头 Authorization: Bearer $ARK_API_KEY，
 *         请求体 {model, messages:[{role,content}], temperature, max_tokens, stream:false}，
 *         读 choices[0].message.content。
 *         模型名必须是完整版本化 ID 或 ep- 接入点 ID，族名会 404。
 *   Anthropic：claude-api skill 的 curl/examples.md
 *         POST https://api.anthropic.com/v1/messages，
 *         头 x-api-key 与 anthropic-version: 2023-06-01，
 *         请求体 {model, max_tokens, system, messages:[{role:"user",content}]}，
 *         读 content[] 里 type=="text" 的 text。
 *         claude-opus-5 与 claude-sonnet-5 **已移除 temperature/top_p/top_k**，
 *         传了返回 400，所以本实现**不转发 temperature**。
 *
 * 密钥只从环境变量读（ARK_API_KEY / ANTHROPIC_API_KEY），
 * **绝不**写进 settings 表、**绝不**写进代码、**绝不**进 git（G27）。
 */

const ARK_DEFAULT_BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
const ARK_DEFAULT_MODEL = "doubao-seed-1-6-251015"
const ANTHROPIC_DEFAULT_BASE_URL = "https://api.anthropic.com"
const ANTHROPIC_DEFAULT_MODEL = "claude-opus-5"
const ARK_MAX_TOKENS = 4096
const ANTHROPIC_MAX_TOKENS = 16000
const ANTHROPIC_VERSION = "2023-06-01"

/**
 * 关闭态客户端：不联网、不抛错，返回明确的中文提示。
 * 这是「把 LLM 关掉，app 仍然完整可用」（G23）的最后一档兜底。
 */
class NullClient {
  constructor(reason) {
    this.name = "null"
    this.model = "null"
    this.reason = reason || "LLM 未启用"
  }

  async complete() {
    return "【LLM 未启用】" + this.reason + "。App 的其余功能（排程、判题、复习、进度）不受影响。"
  }
}

/** 火山方舟客户端：OpenAI 兼容的 /chat/completions */
class ArkClient {
  constructor(opts) {
    const o = opts || {}
    this.name = "ark"
    this.apiKey = o.apiKey || ""
    this.baseUrl = stripTrailingSlash(o.baseUrl || ARK_DEFAULT_BASE_URL)
    this.model = o.model || ARK_DEFAULT_MODEL
    this.maxTokens = o.maxTokens || ARK_MAX_TOKENS
    this.fetchImpl = o.fetchImpl || globalThis.fetch
  }

  async complete(request) {
    const req = request || {}
    const body = {
      model: this.model,
      messages: [
        { role: "system", content: String(req.system || "") },
        { role: "user", content: String(req.user || "") },
      ],
      max_tokens: this.maxTokens,
      stream: false,
    }
    if (typeof req.temperature === "number" && Number.isFinite(req.temperature)) {
      body.temperature = req.temperature
    }
    const res = await this.fetchImpl(this.baseUrl + "/chat/completions", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        authorization: "Bearer " + this.apiKey,
      },
      body: JSON.stringify(body),
    })
    const payload = await readBody(res)
    if (!res.ok) {
      throw new Error("方舟接口返回 " + res.status + "：" + errorText(payload))
    }
    const message = payload && payload.choices && payload.choices[0] ? payload.choices[0].message : null
    const content = message ? message.content : null
    if (typeof content !== "string" || content.trim() === "") {
      throw new Error("方舟接口没有返回文本内容")
    }
    return content.trim()
  }
}

/** Anthropic 客户端：Messages API */
class AnthropicClient {
  constructor(opts) {
    const o = opts || {}
    this.name = "anthropic"
    this.apiKey = o.apiKey || ""
    this.baseUrl = stripTrailingSlash(o.baseUrl || ANTHROPIC_DEFAULT_BASE_URL)
    this.model = o.model || ANTHROPIC_DEFAULT_MODEL
    this.maxTokens = o.maxTokens || ANTHROPIC_MAX_TOKENS
    this.fetchImpl = o.fetchImpl || globalThis.fetch
  }

  async complete(request) {
    const req = request || {}
    // 故意不转发 temperature：claude-opus-5 / claude-sonnet-5 已移除采样参数，传了会 400
    const body = {
      model: this.model,
      max_tokens: this.maxTokens,
      system: String(req.system || ""),
      messages: [{ role: "user", content: String(req.user || "") }],
    }
    const res = await this.fetchImpl(this.baseUrl + "/v1/messages", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-api-key": this.apiKey,
        "anthropic-version": ANTHROPIC_VERSION,
      },
      body: JSON.stringify(body),
    })
    const payload = await readBody(res)
    if (!res.ok) {
      throw new Error("Anthropic 接口返回 " + res.status + "：" + errorText(payload))
    }
    const blocks = payload && Array.isArray(payload.content) ? payload.content : []
    const text = blocks
      .filter(function (b) { return b && b.type === "text" && typeof b.text === "string" })
      .map(function (b) { return b.text })
      .join("\n")
      .trim()
    if (text === "") {
      throw new Error("Anthropic 接口没有返回文本内容")
    }
    return text
  }
}

/** 去掉地址结尾多余的斜杠 */
function stripTrailingSlash(url) {
  return String(url).replace(/\/+$/, "")
}

/** 读响应体：解析成 JSON；解析不了就包成 {raw}；读失败返回 null */
async function readBody(res) {
  let text = ""
  try {
    text = await res.text()
  } catch (err) {
    return null
  }
  if (!text) return null
  try {
    return JSON.parse(text)
  } catch (err) {
    return { raw: text }
  }
}

/** 从错误响应里抠出一句能给人看的详情 */
function errorText(payload) {
  if (payload && payload.error && payload.error.message) return String(payload.error.message)
  if (payload && payload.message) return String(payload.message)
  if (payload && payload.raw) return String(payload.raw)
  return "没有返回错误详情"
}

/** 总开关：环境变量 ALGO_PLAN_LLM_ENABLED，0/false/off/no 表示关 */
function readEnabledFlag(env) {
  const raw = env.ALGO_PLAN_LLM_ENABLED
  if (raw === undefined || raw === null || String(raw).trim() === "") return true
  const text = String(raw).trim().toLowerCase()
  if (text === "0" || text === "false" || text === "off" || text === "no") return false
  return true
}

/** 归一 provider 名字；认不出来的原样返回，由调用方降级 */
function normalizeProvider(value) {
  if (value === undefined || value === null) return ""
  const text = String(value).trim().toLowerCase()
  if (text === "") return ""
  if (text === "ark" || text === "volcengine" || text === "volc" || text === "doubao") return "ark"
  if (text === "anthropic" || text === "claude") return "anthropic"
  if (text === "null" || text === "none" || text === "off" || text === "false" || text === "disabled") return "null"
  return text
}

/** 地址：显式 > ALGO_PLAN_LLM_BASE_URL > ARK_BASE_URL > 内置默认 */
function resolveBaseUrl(o, env, provider) {
  if (o.baseUrl) return o.baseUrl
  if (env.ALGO_PLAN_LLM_BASE_URL) return env.ALGO_PLAN_LLM_BASE_URL
  if (provider === "ark" && env.ARK_BASE_URL) return env.ARK_BASE_URL
  if (provider === "ark") return ARK_DEFAULT_BASE_URL
  return ANTHROPIC_DEFAULT_BASE_URL
}

/** 模型：显式 > ALGO_PLAN_LLM_MODEL > 内置默认 */
function resolveModel(o, env, provider) {
  if (o.model) return o.model
  if (env.ALGO_PLAN_LLM_MODEL) return env.ALGO_PLAN_LLM_MODEL
  return provider === "ark" ? ARK_DEFAULT_MODEL : ANTHROPIC_DEFAULT_MODEL
}

/**
 * 密钥来源：显式传入 > 当前 provider 对应的环境变量 > 另一个 provider 的环境变量。
 * 兜底那一档只在「provider 是显式点名」时启用：即便手上只有 ARK_API_KEY，
 * 显式点名 anthropic 也照样按 anthropic 建客户端（真跑起来鉴权会失败，
 * 但那是用户配置问题，不该在这里悄悄降级成 null 客户端）。
 * 密钥本身只来自环境变量，绝不写进代码、日志或错误信息（G27）。
 */
function resolveApiKey(o, env, provider, providerIsExplicit) {
  if (o.apiKey) return o.apiKey
  const own = provider === "ark" ? env.ARK_API_KEY : env.ANTHROPIC_API_KEY
  if (own) return own
  if (!providerIsExplicit) return ""
  const other = provider === "ark" ? env.ANTHROPIC_API_KEY : env.ARK_API_KEY
  return other || ""
}

/**
 * 造一个 LLM 客户端。
 * @param {{provider?: string, apiKey?: string, baseUrl?: string, model?: string,
 *          env?: object, fetchImpl?: Function, enabled?: boolean}} opts
 *   provider / apiKey / baseUrl / model 的含义与接口契约 §5 一致；
 *   env / fetchImpl / enabled 是 M6 为可测试性与总开关新增的字段。
 * @returns {{complete: Function, name: string}}
 */
function createLlmClient(opts) {
  const o = opts || {}
  const env = o.env || process.env
  const fetchImpl = o.fetchImpl || globalThis.fetch

  // 1. 总开关
  if (o.enabled === false) return new NullClient("已在设置里关闭 LLM")
  if (readEnabledFlag(env) === false) return new NullClient("环境变量 ALGO_PLAN_LLM_ENABLED 把 LLM 关了")

  // 2. provider：显式 > 环境变量 > 按密钥推断
  const providerFromOption = normalizeProvider(o.provider)
  const providerFromEnv = normalizeProvider(env.ALGO_PLAN_LLM_PROVIDER)
  const providerIsExplicit = Boolean(providerFromOption || providerFromEnv)
  let provider = providerFromOption || providerFromEnv
  if (!provider) {
    if (env.ARK_API_KEY) provider = "ark"
    else if (env.ANTHROPIC_API_KEY) provider = "anthropic"
  }
  if (provider === "null") return new NullClient("provider 设为 null")
  if (!provider) return new NullClient("没有配置 provider，也没有找到 ARK_API_KEY 或 ANTHROPIC_API_KEY")
  if (provider !== "ark" && provider !== "anthropic") {
    return new NullClient("未知的 LLM provider：" + provider)
  }

  // 3. 密钥：只从环境变量读
  const apiKey = resolveApiKey(o, env, provider, providerIsExplicit)
  if (!apiKey) {
    return new NullClient("provider 是 " + provider + "，但没有配置对应的 API Key")
  }

  // 4. 地址与模型
  const common = {
    apiKey: apiKey,
    baseUrl: resolveBaseUrl(o, env, provider),
    model: resolveModel(o, env, provider),
    fetchImpl: fetchImpl,
  }
  return provider === "ark" ? new ArkClient(common) : new AnthropicClient(common)
}

/**
 * 说明密钥来自哪里。**只说变量名，绝不回显密钥本身**（G27）。
 * @param {string} provider
 * @param {object} env
 * @returns {string}
 */
function keySourceOf(provider, env) {
  const e = env || process.env
  if (provider === "ark") {
    return e.ARK_API_KEY ? "环境变量 ARK_API_KEY" : "未配置（需要环境变量 ARK_API_KEY）"
  }
  if (provider === "anthropic") {
    return e.ANTHROPIC_API_KEY ? "环境变量 ANTHROPIC_API_KEY" : "未配置（需要环境变量 ANTHROPIC_API_KEY）"
  }
  return "未配置"
}

module.exports = {
  createLlmClient: createLlmClient,
  ArkClient: ArkClient,
  AnthropicClient: AnthropicClient,
  NullClient: NullClient,
  keySourceOf: keySourceOf,
  ARK_DEFAULT_BASE_URL: ARK_DEFAULT_BASE_URL,
  ARK_DEFAULT_MODEL: ARK_DEFAULT_MODEL,
  ANTHROPIC_DEFAULT_BASE_URL: ANTHROPIC_DEFAULT_BASE_URL,
  ANTHROPIC_DEFAULT_MODEL: ANTHROPIC_DEFAULT_MODEL,
}
