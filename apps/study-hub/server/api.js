"use strict"
/**
 * api.js —— HTTP 接口层
 *
 * M1 三个接口：健康检查、题库列表、单题详情。
 * 剩下的（推下一题、复习评分、轮次写回）属于 M4。
 *
 * P2 起，单题详情与提交判题搬到了单元路由：
 *   GET  /api/problem/:id  →  GET  /api/units/:id        （server/api-units.js）
 *   POST /api/attempt      →  POST /api/units/:id/judge  （server/api-unit-run.js）
 * 旧路由已下线。防作弊规则（设计文档 §7.2）随之搬到新路由：
 * 单题接口只返回前 2 个样例用例 + 用例总数，不返回全部用例、不返回参考解法。
 */

const path = require("node:path")

const { toSummary } = require("../core/bank/problem.js")
const { readJsonBody } = require("./router.js")
const { grade } = require("../core/review/sm2.js")
const { nextAction, quotaState, consumeQuota, dayKey } = require("../core/scheduler/next-action.js")
const { parseRounds, updateRoundItem, writeCorrection } = require("../core/sync/rounds.js")

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

      // P1 起这个接口废弃了，但**不删**：旧页面还能用，返回体里标一下。
      const deprecated = {
        deprecated: true,
        deprecatedNote: "这个接口已经废弃，P1 起请改用 /api/session/next。",
      }

      if (!action) {
        ctx.sendJson(res, 200, {
          action: null,
          reviewQueueLength: 0,
          deprecated: deprecated.deprecated,
          deprecatedNote: deprecated.deprecatedNote,
        })
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
          // 轮次分支才有：卡片不在算法题库里时，靠这两个字段去轮次页勾掉
          roundId: action.roundId || "",
          itemId: action.itemId || "",
        },
        reviewQueueLength: action.reviewQueueLength,
        deprecated: deprecated.deprecated,
        deprecatedNote: deprecated.deprecatedNote,
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
}
