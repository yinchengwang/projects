"use strict"
/**
 * api.js —— HTTP 接口层
 *
 * M1 三个接口：健康检查、题库列表、单题详情。
 * M3 加一个：POST /api/attempt —— 提交代码 → 真实编译执行 → 记流水。
 * 剩下的（推下一题、复习评分、轮次写回）属于 M4。
 *
 * 防作弊规则（设计文档 §7.2）：单题接口只返回前 2 个样例用例 + 用例总数，
 * 不返回全部用例、不返回参考解法。
 */

const { toSummary } = require("../core/bank/problem.js")
const { readJsonBody } = require("./router.js")
const { judge, LANGUAGES, DEFAULT_CASE_TIMEOUT_MS } = require("../judge/runner.js")
const { detectCompiler, detectPython } = require("../judge/sandbox.js")

/** 题面接口最多暴露几个样例用例 */
const SAMPLE_CASE_COUNT = 2

/** 作答模式 */
const ATTEMPT_MODES = ["solve", "review", "probe"]

/** settings 键：单用例超时（毫秒） */
const KEY_JUDGE_TIMEOUT = "judge.timeout_ms"

/** settings 键：探测出来的 Python 解释器（G17 的缓存位） */
const KEY_JUDGE_PYTHON = "judge.python.cmd"

/**
 * 组装路由表。
 * @param {{store: object, provider: object, learning: object, sendJson: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildRoutes(ctx) {
  const routes = []

  routes.push({
    method: "GET",
    pattern: "/api/health",
    handler: async function (req, res) {
      const stats = ctx.provider.stats()
      ctx.sendJson(res, 200, {
        ok: true,
        node: process.version,
        learning: {
          ok: ctx.learning.ok,
          root: ctx.learning.root,
          reason: ctx.learning.reason,
        },
        bank: {
          fileCount: stats.fileCount,
          problemCount: stats.problemCount,
          warnings: stats.warnings,
          roots: ctx.provider.roots || [],
        },
        attempts: ctx.store.countAttempts(),
      })
    },
  })

  routes.push({
    method: "GET",
    pattern: "/api/bank",
    handler: async function (req, res, params, query) {
      const problems = await ctx.provider.list({
        track: query.get("track") || undefined,
        topic: query.get("topic") || undefined,
        difficulty: query.get("difficulty") || undefined,
        status: query.get("status") || undefined,
        idPrefix: query.get("prefix") || undefined,
      })
      const limitRaw = Number(query.get("limit") || 0)
      const limit = Number.isFinite(limitRaw) && limitRaw > 0 ? limitRaw : problems.length
      ctx.sendJson(res, 200, {
        total: problems.length,
        items: problems.slice(0, limit).map(toSummary),
      })
    },
  })

  routes.push({
    method: "GET",
    pattern: "/api/problem/:id",
    handler: async function (req, res, params) {
      const problem = await ctx.provider.get(params.id)
      if (!problem) {
        ctx.sendJson(res, 404, { error: "题目不存在", id: params.id })
        return
      }
      ctx.sendJson(res, 200, {
        id: problem.id,
        title: problem.title,
        source: problem.source,
        sourceId: problem.sourceId || "",
        url: problem.url || "",
        searchUrl: problem.searchUrl || "",
        difficulty: problem.difficulty,
        topics: problem.topics,
        tracks: problem.tracks,
        status: problem.status,
        needsFill: problem.needsFill,
        statement: problem.statement,
        cases: {
          sample: problem.cases.slice(0, SAMPLE_CASE_COUNT),
          total: problem.cases.length,
        },
      })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/attempt",
    handler: async function (req, res) {
      const body = await readJsonBody(req)

      const problemId = String(
        body.problemId === null || body.problemId === undefined ? "" : body.problemId
      ).trim()
      const code = String(body.code === null || body.code === undefined ? "" : body.code)
      const language = String(
        body.language === null || body.language === undefined ? "" : body.language
      ).trim().toLowerCase()
      const modeRaw =
        body.mode === null || body.mode === undefined || body.mode === ""
          ? "solve"
          : String(body.mode)

      if (problemId === "") {
        ctx.sendJson(res, 400, { error: "缺少 problemId" })
        return
      }
      if (ATTEMPT_MODES.indexOf(modeRaw) === -1) {
        ctx.sendJson(res, 400, { error: "mode 只能是 solve / review / probe", mode: modeRaw })
        return
      }
      if (LANGUAGES.indexOf(language) === -1) {
        ctx.sendJson(res, 400, { error: "不支持的语言：" + language, supported: LANGUAGES })
        return
      }
      if (code.trim() === "") {
        ctx.sendJson(res, 400, { error: "代码不能为空" })
        return
      }

      const problem = await ctx.provider.get(problemId)
      if (!problem) {
        ctx.sendJson(res, 404, { error: "题目不存在", id: problemId })
        return
      }
      if (!problem.cases || problem.cases.length === 0) {
        ctx.sendJson(res, 400, { error: "题目还没有测试用例，无法判题", id: problemId })
        return
      }

      // 单用例超时：默认 2000ms，可在 settings 里改
      const timeoutRaw = Number(
        ctx.store.getSetting(KEY_JUDGE_TIMEOUT, String(DEFAULT_CASE_TIMEOUT_MS))
      )
      const timeoutMs =
        Number.isFinite(timeoutRaw) && timeoutRaw > 0 ? timeoutRaw : DEFAULT_CASE_TIMEOUT_MS

      // 工具链：编译器的探测结果在进程内缓存，只有第一次付 spawn 的钱。
      // Python 解释器只在提交 Python 时才探测——提交 C++ 没必要为它多等几百毫秒。
      const toolchain = { compiler: detectCompiler(), python: null }
      if (language === "python") {
        // 先按 settings 里缓存的解释器试，不行再按 python3 → python → py -3 探测
        const override = ctx.store.getSetting(KEY_JUDGE_PYTHON)
        const python = detectPython(override || null)
        toolchain.python = python
        if (python) {
          // 无论有没有 override 都回写：override 配错了（比如解释器被删了），
          // 这里探测到的真身会把它顶掉，下次就不必再走一遍注定失败的探测。
          const text = python.cmd + (python.args.length ? " " + python.args.join(" ") : "")
          if (ctx.store.getSetting(KEY_JUDGE_PYTHON) !== text) {
            ctx.store.setSetting(KEY_JUDGE_PYTHON, text)
          }
        }
      }

      const result = await judge({
        problem: problem,
        code: code,
        language: language,
        timeoutMs: timeoutMs,
        toolchain: toolchain,
      })

      const attemptId = ctx.store.insertAttempt({
        problem_id: problemId,
        ts: new Date().toISOString(),
        language: language,
        code: code,
        verdict: result.verdict,
        passed_cases: result.passedCases,
        total_cases: result.totalCases,
        duration_ms: result.durationMs,
        mode: modeRaw,
      })

      ctx.sendJson(res, 200, {
        attemptId: attemptId,
        verdict: result.verdict,
        passedCases: result.passedCases,
        totalCases: result.totalCases,
        durationMs: result.durationMs,
        failedCase: result.failedCase,
        message: result.message,
        compilerOutput: result.compilerOutput,
      })
    },
  })

  return routes
}

module.exports = {
  buildRoutes: buildRoutes,
  SAMPLE_CASE_COUNT: SAMPLE_CASE_COUNT,
  ATTEMPT_MODES: ATTEMPT_MODES,
}
