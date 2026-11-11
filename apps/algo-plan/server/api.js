"use strict"
/**
 * api.js —— HTTP 接口层
 *
 * M1 只实现三个接口：健康检查、题库列表、单题详情。
 * 剩下的（推下一题、提交判题、复习评分、轮次写回）分别属于 M2–M4。
 *
 * 防作弊规则（设计文档 §7.2）：单题接口只返回前 2 个样例用例 + 用例总数，
 * 不返回全部用例、不返回参考解法。
 */

const { toSummary } = require("../core/bank/problem.js")

/** 题面接口最多暴露几个样例用例 */
const SAMPLE_CASE_COUNT = 2

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
        bank: stats,
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

  return routes
}

module.exports = { buildRoutes: buildRoutes, SAMPLE_CASE_COUNT: SAMPLE_CASE_COUNT }
