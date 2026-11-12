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

const path = require("node:path")

const { toSummary } = require("../core/bank/problem.js")
const { readJsonBody } = require("./router.js")
const { judge, LANGUAGES, DEFAULT_CASE_TIMEOUT_MS } = require("../judge/runner.js")
const { detectCompiler, detectPython } = require("../judge/sandbox.js")
const { grade } = require("../core/review/sm2.js")
const { nextAction, quotaState, consumeQuota, dayKey } = require("../core/scheduler/next-action.js")
const { parseRounds, updateRoundItem, writeCorrection } = require("../core/sync/rounds.js")

/** 题面接口最多暴露几个样例用例 */
const SAMPLE_CASE_COUNT = 2

/** 作答模式 */
const ATTEMPT_MODES = ["solve", "review", "probe"]

/** settings 键：单用例超时（毫秒） */
const KEY_JUDGE_TIMEOUT = "judge.timeout_ms"

/** settings 键：探测出来的 Python 解释器（G17 的缓存位） */
const KEY_JUDGE_PYTHON = "judge.python.cmd"

/**
 * 读 learning/progress 下的轮次。没连上 learning 仓就返回空列表（纯本地模式降级）。
 * @param {{learning: object}} ctx
 * @returns {{rounds: object[], warnings: string[]}}
 */
function loadRounds(ctx) {
  if (!ctx.learning || !ctx.learning.ok || !ctx.learning.root) {
    return { rounds: [], warnings: [] }
  }
  return parseRounds(path.join(ctx.learning.root, "progress"))
}

/**
 * 剥掉轮次标题开头的轮次号前缀（"R33 Grokking 算法篇" → "Grokking 算法篇"）。
 *
 * 解析层（core/sync/rounds.js）的 round.title 保持一级标题**原文**，那是 git 里的
 * 真实标题；但接口层给界面看的是「这轮叫什么」，轮次号已经单独有 round.id 了，
 * 重复一遍反而占地方。M4 计划 Task 2 与 Task 5 的断言各执一词，这里在接口层
 * 统一成「标题不带轮次号」，两边同时成立。
 * @param {string} title
 * @returns {string}
 */
function stripRoundNumber(title) {
  return String(title === null || title === undefined ? "" : title)
    .replace(/^\s*R\d{1,4}\s+/, "")
    .trim()
}

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

  /* ---------- M4：闭环 ---------- */

  routes.push({
    method: "GET",
    pattern: "/api/next",
    handler: async function (req, res) {
      const nowIso = new Date().toISOString()
      const problems = await ctx.provider.list()
      const parsed = loadRounds(ctx)
      const action = nextAction({
        store: ctx.store,
        provider: ctx.provider,
        rounds: parsed.rounds,
        nowIso: nowIso,
        settings: ctx.store.allSettings(),
        problems: problems,
      })

      if (!action) {
        ctx.sendJson(res, 200, { action: null, reviewQueueLength: 0 })
        return
      }

      // 复习动作先把这次的耗时估算记下来，等评分时计入单日配额
      if (action.kind === "review") {
        ctx.store.setSetting("review.estimate." + action.problemId, String(action.estimatedMinutes))
      }

      ctx.sendJson(res, 200, {
        action: {
          kind: action.kind,
          problemId: action.problemId,
          title: action.title,
          estimatedMinutes: action.estimatedMinutes,
          reason: action.reason,
          reviewQueueLength: action.reviewQueueLength,
        },
        reviewQueueLength: action.reviewQueueLength,
      })
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/review/:id/grade",
    handler: async function (req, res, params) {
      const body = await readJsonBody(req)
      const quality = Number(body.quality)
      if (!Number.isInteger(quality) || quality < 0 || quality > 5) {
        ctx.sendJson(res, 400, { error: "quality 必须是 0~5 的整数" })
        return
      }

      const problemId = String(params.id)
      const nowIso = new Date().toISOString()
      const existing = ctx.store.getReviewCard(problemId)
      const card = existing || {
        problem_id: problemId,
        easiness: 2.5,
        interval_days: 0,
        repetitions: 0,
        last_quality: null,
        last_at: null,
        next_review_at: nowIso,
        lapses: 0,
      }
      const graded = grade(card, quality, nowIso)
      ctx.store.upsertReviewCard(graded)

      // 复习一道算一题；分钟数用 /api/next 记下来的估算
      const estimate = Number(ctx.store.getSetting("review.estimate." + problemId, "0")) || 0
      ctx.store.setSetting("review.estimate." + problemId, "0")
      consumeQuota(ctx.store, { questions: 1, minutes: estimate }, nowIso)

      ctx.sendJson(res, 200, {
        problemId: problemId,
        nextReviewAt: graded.next_review_at,
        intervalDays: graded.interval_days,
        easiness: graded.easiness,
        lapses: graded.lapses,
      })
    },
  })

  routes.push({
    method: "GET",
    pattern: "/api/rounds",
    handler: async function (req, res) {
      const parsed = loadRounds(ctx)
      ctx.sendJson(res, 200, {
        rounds: parsed.rounds.map(function (round) {
          return {
            id: round.id,
            file: round.file,
            title: stripRoundNumber(round.title),
            goal: round.goal,
            status: round.status,
            items: round.items.map(function (item) {
              return {
                raw: item.raw,
                id: item.id,
                title: item.title,
                status: item.status,
                doneDate: item.doneDate,
                commitSha: item.commitSha,
              }
            }),
          }
        }),
        warnings: parsed.warnings,
      })
    },
  })

  routes.push({
    method: "PATCH",
    pattern: "/api/rounds/:id/items/:itemId",
    handler: async function (req, res, params) {
      const body = await readJsonBody(req)
      const patch = {}
      if (body.status !== undefined && body.status !== null) patch.status = String(body.status)
      if (body.doneDate !== undefined && body.doneDate !== null) patch.doneDate = String(body.doneDate)
      if (body.commitSha !== undefined && body.commitSha !== null) patch.commitSha = String(body.commitSha)

      if (!ctx.learning || !ctx.learning.ok || !ctx.learning.root) {
        ctx.sendJson(res, 503, { error: "还没连上 learning 仓，写回不可用" })
        return
      }

      try {
        const result = updateRoundItem(
          path.join(ctx.learning.root, "progress"),
          params.id,
          params.itemId,
          patch
        )
        ctx.sendJson(res, 200, { diff: result.diff, warnings: result.warnings })
      } catch (err) {
        ctx.sendJson(res, err && err.status ? err.status : 500, {
          error: (err && err.message) || "写回失败",
          dirtyFiles: (err && err.dirtyFiles) || [],
        })
      }
    },
  })

  routes.push({
    method: "POST",
    pattern: "/api/correction",
    handler: async function (req, res) {
      const body = await readJsonBody(req)
      const problemId = String(body.problemId || "")
      const text = String(body.text || "")
      if (!problemId) {
        ctx.sendJson(res, 400, { error: "缺少 problemId" })
        return
      }
      if (!text.trim()) {
        ctx.sendJson(res, 400, { error: "订正内容不能为空" })
        return
      }
      if (!ctx.learning || !ctx.learning.ok || !ctx.learning.root) {
        ctx.sendJson(res, 503, { error: "还没连上 learning 仓，写回不可用" })
        return
      }

      const problem = await ctx.provider.get(problemId)
      if (!problem) {
        ctx.sendJson(res, 404, { error: "题目不存在：" + problemId })
        return
      }

      try {
        const result = writeCorrection(ctx.learning.root, problem, text)
        ctx.sendJson(res, 200, { file: result.file, diff: result.diff })
      } catch (err) {
        ctx.sendJson(res, err && err.status ? err.status : 500, {
          error: (err && err.message) || "写回失败",
          dirtyFiles: (err && err.dirtyFiles) || [],
        })
      }
    },
  })

  routes.push({
    method: "GET",
    pattern: "/api/progress",
    handler: async function (req, res) {
      const now = new Date()
      const startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate()).toISOString()
      const nowIso = now.toISOString()
      const quota = quotaState(ctx.store, nowIso, ctx.store.allSettings())
      ctx.sendJson(res, 200, {
        date: dayKey(nowIso),
        solved: ctx.store.countAttemptsSince(startOfDay),
        reviewed: quota.questions,
        reviewMinutes: quota.minutes,
        quota: {
          // 契约 §3 的字段名
          usedQuestions: quota.questions,
          usedMinutes: quota.minutes,
          limitQuestions: quota.limitQuestions,
          limitMinutes: quota.limitMinutes,
          // M4 计划里的字段名（一并给出，两套名字都在，省得调用方猜）
          questions: quota.questions,
          minutes: quota.minutes,
          exhausted: quota.exhausted,
          exhaustedBy: quota.exhaustedBy,
        },
      })
    },
  })

  /* ---------- M4：闭环结束 ---------- */

  return routes
}

module.exports = {
  buildRoutes: buildRoutes,
  SAMPLE_CASE_COUNT: SAMPLE_CASE_COUNT,
  ATTEMPT_MODES: ATTEMPT_MODES,
}
