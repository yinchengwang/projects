"use strict"
/**
 * api-insights.js —— M5 的诊断接口
 *
 * 单独一个文件，**不动 M1 的 server/api.js**：M3、M4 也要改那个文件，
 * 互相整体覆盖会丢接口。路由在 server/app.js 里跟 api.js 的路由合并。
 */

const { buildInsights, DEFAULT_DAYS } = require("./insights.js")

/**
 * 组装 M5 的路由。
 * @param {{store: object, provider: object, sendJson: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildInsightRoutes(ctx) {
  return [
    {
      method: "GET",
      pattern: "/api/insights",
      handler: async function (req, res, params, query) {
        const daysRaw = query.get("days")
        const days = daysRaw === null || daysRaw === "" ? DEFAULT_DAYS : Number(daysRaw)
        const insights = await buildInsights({
          store: ctx.store,
          provider: ctx.provider,
          days: days,
        })
        ctx.sendJson(res, 200, insights)
      },
    },
  ]
}

module.exports = { buildInsightRoutes: buildInsightRoutes }
