"use strict"
/**
 * service.js —— LLM 服务层：把提示词、客户端、成本护栏、题库串成五个场景
 *
 * 五个场景（设计文档 §6.2）：讲解 / 分级求助 / 批改思路 / 现造新题 / 补全缺口。
 *
 * 两条纪律（设计文档 §6.3，本文件是落地的地方）：
 *   1. 不主动泄题（G24）—— 讲解只到思路层；只有显式选「代码」档才给代码。
 *      这里构造上下文时**只挑安全字段**，参考解法根本不进提示词。
 *   2. 产出全部可追溯（G25）—— 现造的新题落盘进 learning/bank/（source: llm，
 *      id 用 M1 的 makeLlmId）；每次请求都记 model + prompt 摘要到 settings 表。
 *
 * LLM 关着时（client.name === "null"）：explain / hint / gradeIdea 返回明确的中文提示，
 * generate / fill 抛 503。两种情况都不崩，其余功能不受影响（G23）。
 */

const crypto = require("node:crypto")

const { buildPrompt, parseResponse, EXPLAIN_LEVELS, HINT_LEVELS } = require("./prompts.js")
const { NullClient, keySourceOf } = require("./client.js")
const { checkAll, consumeAll, limits, sessionKey, dailyKey } = require("./guard.js")
const {
  makeLlmId,
  createProblem,
  normalizeTopics,
  TRACKS,
  DIFFICULTIES,
} = require("../bank/problem.js")

/** 每道题最多留多少条调用记录 */
const MAX_TRACE_PER_PROBLEM = 20

/** settings 表里存 LLM 设置的键；**绝不**存密钥（G27） */
const LLM_SETTING_KEYS = {
  enabled: "llm.enabled",
  provider: "llm.provider",
  model: "llm.model",
  baseUrl: "llm.base_url",
}

/** 业务错误：400 参数不对 */
function badRequest(message) {
  const err = new Error(message)
  err.status = 400
  return err
}

/** 配额用完：429 */
function quotaError(message) {
  const err = new Error(message)
  err.status = 429
  err.quota = true
  return err
}

/** LLM 没开：503 */
function serviceUnavailable(message) {
  const err = new Error(message)
  err.status = 503
  return err
}

/** 日期格式化成 YYYY-MM-DD（与 M1 的 today() 保持一致） */
function dateStr(date) {
  const d = date instanceof Date ? date : new Date(date)
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return y + "-" + m + "-" + day
}

/** 读 LLM 设置 */
function readLlmSettings(store) {
  const enabledRaw = String(store.getSetting(LLM_SETTING_KEYS.enabled, "1")).trim().toLowerCase()
  return {
    enabled: !(enabledRaw === "0" || enabledRaw === "false" || enabledRaw === "off"),
    provider: String(store.getSetting(LLM_SETTING_KEYS.provider, "") || ""),
    model: String(store.getSetting(LLM_SETTING_KEYS.model, "") || ""),
    baseUrl: String(store.getSetting(LLM_SETTING_KEYS.baseUrl, "") || ""),
  }
}

/**
 * 写 LLM 设置。**只认这四个字段，密钥永远不写进来**（G27）。
 * @param {{getSetting: Function, setSetting: Function}} store
 * @param {{enabled?: boolean, provider?: string, model?: string, baseUrl?: string}} patch
 */
function writeLlmSettings(store, patch) {
  const p = patch || {}
  if (p.enabled !== undefined) {
    store.setSetting(LLM_SETTING_KEYS.enabled, p.enabled ? "1" : "0")
  }
  if (p.provider !== undefined) {
    store.setSetting(LLM_SETTING_KEYS.provider, String(p.provider))
  }
  if (p.model !== undefined) {
    store.setSetting(LLM_SETTING_KEYS.model, String(p.model))
  }
  if (p.baseUrl !== undefined) {
    store.setSetting(LLM_SETTING_KEYS.baseUrl, String(p.baseUrl))
  }
  return readLlmSettings(store)
}

/**
 * 造一个服务层。
 * @param {{client?: object, getClient?: Function, store: object, provider: object,
 *          env?: object, now?: Function}} opts
 *   client：固定客户端（测试用）；getClient：每次请求都调一次拿当前客户端（设置页改完立即生效）。
 * @returns {object} 服务层
 */
function createLlmService(opts) {
  const o = opts || {}
  const store = o.store
  const provider = o.provider
  const env = o.env || process.env
  const fixedClient = o.client || null
  const getClient = typeof o.getClient === "function"
    ? o.getClient
    : function () { return fixedClient }
  const now = typeof o.now === "function" ? o.now : function () { return new Date() }

  /** 取当前客户端；没装配就用关闭态客户端兜底 */
  function currentClient() {
    const client = getClient()
    if (!client) return new NullClient("LLM 服务没有装配客户端")
    return client
  }

  /** 关闭态的统一提示语 */
  function offMessage(client) {
    const reason = (client && client.reason) || "LLM 未启用"
    return "【LLM 未启用】" + reason + "。App 的其余功能（排程、判题、复习、进度）不受影响。"
  }

  /** 防泄题第一道闸：只挑安全字段，参考解法根本不进来（G24） */
  function safeContext(problem, level) {
    return {
      title: problem.title,
      difficulty: problem.difficulty,
      topics: problem.topics,
      statement: problem.statement,
      level: level,
    }
  }

  /** 提示词摘要：只留指纹、长度和开头一小截，不整段存（G25） */
  function summarizePrompt(prompt) {
    const full = String((prompt && prompt.system) || "") + "\n" + String((prompt && prompt.user) || "")
    const digest = crypto.createHash("sha1").update(full).digest("hex").slice(0, 10)
    return {
      digest: digest,
      length: full.length,
      head: full.replace(/\s+/g, " ").slice(0, 120),
    }
  }

  /** 记一次调用：model + prompt 摘要 + 档位，落 settings 表（G25） */
  function recordTrace(problemId, kind, level, model, prompt) {
    if (!problemId) return
    const key = "llm.trace." + problemId
    let list = []
    const raw = store.getSetting(key, "")
    if (raw) {
      try {
        const parsed = JSON.parse(raw)
        if (Array.isArray(parsed)) list = parsed
      } catch (err) {
        list = []
      }
    }
    list.push({
      ts: now().toISOString(),
      kind: kind,
      level: level || "",
      model: model || "",
      prompt: summarizePrompt(prompt),
    })
    while (list.length > MAX_TRACE_PER_PROBLEM) list.shift()
    store.setSetting(key, JSON.stringify(list))
  }

  /** 扣配额前先查；超了直接停并提示（G31） */
  function ensureQuota(sessionId) {
    const quota = checkAll(store, sessionId, dateStr(now()))
    if (!quota.ok) throw quotaError(quota.message)
    return quota
  }

  /**
   * 讲解：先思路后代码。
   * @param {{problem: object, level?: string, sessionId?: string}} req
   * @returns {Promise<{text: string, model: string}>}
   */
  async function explain(req) {
    const r = req || {}
    const problem = r.problem
    if (!problem) throw badRequest("缺少题目")
    const level = EXPLAIN_LEVELS.indexOf(r.level) !== -1 ? r.level : "思路"
    const client = currentClient()
    if (client.name === "null") {
      return { text: offMessage(client), model: client.model || "null" }
    }
    ensureQuota(r.sessionId)
    const prompt = buildPrompt("explain", safeContext(problem, level))
    const raw = await client.complete({ system: prompt.system, user: prompt.user, temperature: 0.4 })
    const parsed = parseResponse(raw, "explain")
    if (!parsed.ok) throw new Error("讲解失败：" + parsed.reason)
    consumeAll(store, r.sessionId, dateStr(now()))
    recordTrace(problem.id, "explain", level, client.model, prompt)
    return { text: parsed.text, model: client.model }
  }

  /**
   * 分级求助：方向 → 思路 → 伪码 → 代码，逐级解锁。
   * @param {{problem: object, level?: string, sessionId?: string}} req
   * @returns {Promise<{text: string, model: string}>}
   */
  async function hint(req) {
    const r = req || {}
    const problem = r.problem
    if (!problem) throw badRequest("缺少题目")
    if (HINT_LEVELS.indexOf(r.level) === -1) {
      throw badRequest("求助档位非法：" + String(r.level))
    }
    const level = r.level
    const client = currentClient()
    if (client.name === "null") {
      return { text: offMessage(client), model: client.model || "null" }
    }
    ensureQuota(r.sessionId)
    const prompt = buildPrompt("hint", safeContext(problem, level))
    const raw = await client.complete({ system: prompt.system, user: prompt.user, temperature: 0.3 })
    const parsed = parseResponse(raw, "hint")
    if (!parsed.ok) throw new Error("求助失败：" + parsed.reason)
    consumeAll(store, r.sessionId, dateStr(now()))
    recordTrace(problem.id, "hint", level, client.model, prompt)
    return { text: parsed.text, model: client.model }
  }

  /**
   * 批改思路：只判断思路对不对，不给完整代码。
   * @param {{problem: object, idea: string, sessionId?: string}} req
   * @returns {Promise<{verdict: string, comment: string, model: string}>}
   */
  async function gradeIdea(req) {
    const r = req || {}
    const problem = r.problem
    if (!problem) throw badRequest("缺少题目")
    const idea = String(r.idea || "").trim()
    if (idea === "") throw badRequest("思路内容不能为空")
    const client = currentClient()
    if (client.name === "null") {
      return { verdict: "unknown", comment: offMessage(client), model: client.model || "null" }
    }
    ensureQuota(r.sessionId)
    const prompt = buildPrompt("grade-idea", {
      title: problem.title,
      difficulty: problem.difficulty,
      topics: problem.topics,
      statement: problem.statement,
      idea: idea,
    })
    const raw = await client.complete({ system: prompt.system, user: prompt.user, temperature: 0.2 })
    const parsed = parseResponse(raw, "grade-idea")
    consumeAll(store, r.sessionId, dateStr(now()))
    recordTrace(problem.id, "grade-idea", "", client.model, prompt)
    if (!parsed.ok) {
      return {
        verdict: "unknown",
        comment: "模型没给出可判定的结论：" + parsed.reason,
        model: client.model,
      }
    }
    return { verdict: parsed.verdict, comment: parsed.comment, model: client.model }
  }

  /**
   * 现造新题：真的落盘进 learning/bank/（G25）。
   * @param {{track: string, topics?: string[], difficulty?: string, sessionId?: string}} req
   * @returns {Promise<{problem: object, model: string, file: string}>}
   */
  async function generate(req) {
    const r = req || {}
    const track = String(r.track || "").trim()
    if (TRACKS.indexOf(track) === -1) {
      throw badRequest("轨道非法：" + (track || "（空）"))
    }
    const difficulty = DIFFICULTIES.indexOf(String(r.difficulty)) !== -1
      ? String(r.difficulty)
      : "medium"
    const topics = normalizeTopics(r.topics)
    const client = currentClient()
    if (client.name === "null") throw serviceUnavailable(offMessage(client))
    ensureQuota(r.sessionId)

    const prompt = buildPrompt("generate", {
      track: track,
      topics: topics,
      difficulty: difficulty,
    })
    const raw = await client.complete({ system: prompt.system, user: prompt.user, temperature: 0.8 })
    const parsed = parseResponse(raw, "generate")
    if (!parsed.ok) throw new Error("造题失败：" + parsed.reason)

    const id = makeLlmId(parsed.problem.statement)
    const existing = await provider.get(id)
    if (existing) {
      throw badRequest("这道题已经生成过了（题号 " + id + "），换个知识点再试。")
    }

    let finalTopics = parsed.problem.topics.length ? parsed.problem.topics : topics
    if (finalTopics.length === 0) finalTopics = ["未分类"]

    const day = dateStr(now())
    let problem
    try {
      problem = createProblem({
        id: id,
        source: "llm",
        title: parsed.problem.title,
        difficulty: parsed.problem.difficulty || difficulty,
        topics: finalTopics,
        tracks: [track],
        status: "todo",
        needsFill: false,
        created: day,
        updated: day,
        statement: parsed.problem.statement,
        cases: parsed.problem.cases,
        solution: parsed.problem.solution,
        solutionLang: parsed.problem.solutionLang,
        extraSections: [],
      })
    } catch (err) {
      throw badRequest("模型造的题不合规：" + err.message)
    }

    const file = await provider.put(problem)
    consumeAll(store, r.sessionId, day)
    recordTrace(id, "generate", track, client.model, prompt)
    return { problem: problem, model: client.model, file: file }
  }

  /**
   * 补全缺口：把缺题面或缺用例的题补齐，写回题库。
   * **只补缺的部分**：原来有的题面与用例一律保留（G25：补缺口，不是重写）。
   * @param {{problem: object, sessionId?: string}} req
   * @returns {Promise<{problem: object, model: string, file: string}>}
   */
  async function fill(req) {
    const r = req || {}
    const problem = r.problem
    if (!problem) throw badRequest("缺少题目")
    const client = currentClient()
    if (client.name === "null") throw serviceUnavailable(offMessage(client))
    ensureQuota(r.sessionId)

    const prompt = buildPrompt("fill", {
      title: problem.title,
      difficulty: problem.difficulty,
      topics: problem.topics,
      statement: problem.statement,
      cases: problem.cases,
      solution: problem.solution,
      solutionLang: problem.solutionLang,
    })
    const raw = await client.complete({ system: prompt.system, user: prompt.user, temperature: 0.4 })
    const parsed = parseResponse(raw, "fill")
    if (!parsed.ok) throw new Error("补全失败：" + parsed.reason)

    const statement = String(problem.statement || "").trim() !== ""
      ? problem.statement
      : parsed.patch.statement
    const cases = Array.isArray(problem.cases) && problem.cases.length > 0
      ? problem.cases
      : parsed.patch.cases
    const solution = parsed.patch.solution || problem.solution
    const solutionLang = parsed.patch.solutionLang || problem.solutionLang

    const day = dateStr(now())
    let updated
    try {
      updated = createProblem({
        id: problem.id,
        source: problem.source,
        title: problem.title,
        difficulty: problem.difficulty,
        topics: problem.topics,
        tracks: problem.tracks,
        status: problem.status,
        needsFill: false,
        created: problem.created,
        updated: day,
        statement: statement,
        cases: cases,
        solution: solution,
        solutionLang: solutionLang,
        extraSections: problem.extraSections || [],
      })
    } catch (err) {
      throw badRequest("补全后题目不合规：" + err.message)
    }

    const file = await provider.put(updated)
    consumeAll(store, r.sessionId, day)
    recordTrace(problem.id, "fill", "", client.model, prompt)
    return { problem: updated, model: client.model, file: file }
  }

  /**
   * 当前 LLM 状态，给设置页与前端看。
   * **只报密钥来自哪个环境变量，绝不回显密钥**（G27）。
   */
  function describe(sessionId) {
    const client = currentClient()
    const s = readLlmSettings(store)
    const l = limits(store)
    const usedSession = Number(store.getSetting(sessionKey(sessionId), "0")) || 0
    const usedDaily = Number(store.getSetting(dailyKey(dateStr(now())), "0")) || 0
    return {
      enabled: s.enabled,
      provider: s.provider,
      model: s.model,
      baseUrl: s.baseUrl,
      effectiveProvider: client.name,
      effectiveModel: client.model,
      keySource: keySourceOf(s.provider || client.name, env),
      reason: client.name === "null" ? String(client.reason || "") : "",
      // 轨道与难度直接取 M1 冻结的常量，前端不自己硬编码一份——两边会漂。
      tracks: TRACKS.slice(),
      difficulties: DIFFICULTIES.slice(),
      quota: {
        sessionLimit: l.sessionLimit,
        dailyLimit: l.dailyLimit,
        usedSession: usedSession,
        usedDaily: usedDaily,
      },
    }
  }

  return {
    explain: explain,
    hint: hint,
    gradeIdea: gradeIdea,
    generate: generate,
    fill: fill,
    describe: describe,
    currentClient: currentClient,
  }
}

module.exports = {
  createLlmService: createLlmService,
  readLlmSettings: readLlmSettings,
  writeLlmSettings: writeLlmSettings,
  LLM_SETTING_KEYS: LLM_SETTING_KEYS,
}
