"use strict"
/**
 * api-llm.js —— M6 的 LLM 接口
 *
 * 单独一个文件，**不动 M1 的 server/api.js**：M2–M5 也会往那个文件里加路由，
 * 分开放可以彻底避免抢同一个文件。
 *
 * 五个场景接口的入参与响应形状严格照接口契约 §5：
 *   POST /api/llm/explain       {problemId, level}                    -> {text, model}
 *   POST /api/llm/hint          {problemId, level}                    -> {text, model}
 *   POST /api/llm/grade-idea    {problemId, idea}                     -> {verdict, comment, model}
 *   POST /api/bank/llm-generate {track, topics[], difficulty}         -> {problem, model}
 *   POST /api/llm/fill          {problemId}                           -> {problem, model}
 *
 * 额外允许一个可选字段 sessionId：成本护栏按「单次会话」计数要用（G31）。
 * 不传就落到 default 桶。
 */

const { readJsonBody, HttpError } = require("./router.js")
const { writeLlmSettings } = require("../core/llm/service.js")

/**
 * 服务层用「带 status 的普通 Error」表达业务错误（400 参数不对 / 429 配额用完 /
 * 503 LLM 没开）。路由层只认 HttpError，所以这里转一道——不然它们统统会变成 500，
 * 前端就分不清「参数写错了」和「服务端炸了」。
 */
function toHttpError(err) {
  if (err instanceof HttpError) return err
  const status = Number(err && err.status)
  if (status >= 400 && status <= 599) {
    return new HttpError(status, String((err && err.message) || "请求失败"))
  }
  return err
}

/** 给 handler 包一层：业务错误转成 HttpError 再抛出去 */
function wrap(handler) {
  return async function (req, res, params, query) {
    try {
      return await handler(req, res, params, query)
    } catch (err) {
      throw toHttpError(err)
    }
  }
}

/**
 * 组装 M6 的路由表。
 * @param {{store: object, provider: object, llm: object, sendJson: Function, env?: object}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildLlmRoutes(ctx) {
  const routes = []

  /** 没装配 LLM 服务时统一回 503，不影响其它接口 */
  function needLlm(res) {
    if (ctx.llm) return true
    ctx.sendJson(res, 503, { error: "LLM 功能未装配" })
    return false
  }

  /** 按题号取题，取不到回 404 */
  async function loadProblem(res, body) {
    const id = String(body.problemId || "")
    const problem = await ctx.provider.get(id)
    if (!problem) {
      ctx.sendJson(res, 404, { error: "题目不存在", id: id })
      return null
    }
    return problem
  }

  routes.push({
    method: "POST",
    pattern: "/api/llm/explain",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const problem = await loadProblem(res, body)
      if (!problem) return
      const result = await ctx.llm.explain({
        problem: problem,
        level: String(body.level || "思路"),
        sessionId: body.sessionId,
      })
      ctx.sendJson(res, 200, { text: result.text, model: result.model })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/llm/hint",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const problem = await loadProblem(res, body)
      if (!problem) return
      const result = await ctx.llm.hint({
        problem: problem,
        level: String(body.level || "方向"),
        sessionId: body.sessionId,
      })
      ctx.sendJson(res, 200, { text: result.text, model: result.model })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/llm/grade-idea",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const problem = await loadProblem(res, body)
      if (!problem) return
      const result = await ctx.llm.gradeIdea({
        problem: problem,
        idea: String(body.idea || ""),
        sessionId: body.sessionId,
      })
      ctx.sendJson(res, 200, {
        verdict: result.verdict,
        comment: result.comment,
        model: result.model,
      })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/bank/llm-generate",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const topics = Array.isArray(body.topics) ? body.topics : []
      const result = await ctx.llm.generate({
        track: String(body.track || ""),
        topics: topics,
        difficulty: String(body.difficulty || "medium"),
        sessionId: body.sessionId,
      })
      ctx.sendJson(res, 200, { problem: result.problem, model: result.model })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/llm/fill",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const problem = await loadProblem(res, body)
      if (!problem) return
      const result = await ctx.llm.fill({
        problem: problem,
        sessionId: body.sessionId,
      })
      ctx.sendJson(res, 200, { problem: result.problem, model: result.model })
    },
  })

  // ---- LLM 设置（M6 新增的两个接口；**绝不接受也不返回密钥**） ----

  routes.push({
    method: "GET",
    pattern: "/api/llm/settings",
    handler: async function (req, res, params, query) {
      if (!needLlm(res)) return
      const sessionId = query.get("sessionId")
      ctx.sendJson(res, 200, ctx.llm.describe(sessionId || undefined))
    },
  })

  routes.push({
    method: "PUT",
    pattern: "/api/llm/settings",
    handler: async function (req, res) {
      if (!needLlm(res)) return
      const body = await readJsonBody(req)
      const patch = {}
      if (body.enabled !== undefined) {
        patch.enabled = body.enabled === true || body.enabled === 1 || body.enabled === "1"
      }
      if (body.provider !== undefined) patch.provider = String(body.provider)
      if (body.model !== undefined) patch.model = String(body.model)
      if (body.baseUrl !== undefined) patch.baseUrl = String(body.baseUrl)
      const settings = writeLlmSettings(ctx.store, patch)
      ctx.sendJson(res, 200, { ok: true, settings: settings })
    },
  })

  // 统一包一层：业务错误（400 / 429 / 503）原样透出状态码，别都变成 500。
  return routes.map(function (route) {
    return {
      method: route.method,
      pattern: route.pattern,
      handler: wrap(route.handler),
    }
  })
}

module.exports = { buildLlmRoutes: buildLlmRoutes }
