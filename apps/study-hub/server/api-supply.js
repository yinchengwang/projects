"use strict"
/**
 * api-supply.js —— P4 的供给接口（契约 §8.4）
 *
 *   GET  /api/supply/status      → {tracks, gaps, warnings}
 *   POST /api/supply/review/:id  → {ok: true, unit: {id, status, reviewed, reviewed_at}}
 *
 * 纯本地模式（没连上 learning 仓）：
 *   - status → 200 + {tracks: [], gaps: [], warnings: ["还没连上 learning 仓…"]}（G17，不许白屏）
 *   - review → 503 {"error":"还没连上 learning 仓，写回不可用"}
 *
 * 路由在独立文件里，与 server/api.js 零冲突；由 server/app.js 合并进同一张路由表。
 */

const { readJsonBody } = require("./router.js")
const { scanGaps } = require("../core/supply/plan.js")
const { applyReview } = require("../core/supply/review.js")

/** 纯本地模式时的提醒 */
const LOCAL_WARNING = "还没连上 learning 仓，供给状态只能看本地数据，写回不可用"

/**
 * 组装供给路由。
 * @param {{store?: object, provider?: object, learning?: object, units?: object[],
 *          unitsById?: object, unitsDir?: string|null, stateFile?: string|null,
 *          contentRoots?: string[], env?: object, sendJson: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildSupplyRoutes(ctx) {
  const c = ctx || {}
  return [
    {
      method: "GET",
      pattern: "/api/supply/status",
      handler: async function (req, res, params, query) {
        const learning = c.learning || { root: null, ok: false, reason: "" }
        if (!learning.ok || !learning.root) {
          c.sendJson(res, 200, { tracks: [], gaps: [], warnings: [LOCAL_WARNING] })
          return
        }
        const track = query.get("track") || ""
        const result = scanGaps({
          units: Array.isArray(c.units) ? c.units : [],
          learningRoot: learning.root,
          contentRoots: Array.isArray(c.contentRoots) ? c.contentRoots : [],
          track: track,
        })
        c.sendJson(res, 200, {
          tracks: result.tracks,
          gaps: result.gaps,
          warnings: result.warnings,
        })
      },
    },
    {
      method: "POST",
      pattern: "/api/supply/review/:id",
      handler: async function (req, res, params, query) {
        const learning = c.learning || { root: null, ok: false, reason: "" }
        if (!learning.ok || !learning.root) {
          c.sendJson(res, 503, { error: "还没连上 learning 仓，写回不可用" })
          return
        }
        const body = await readJsonBody(req)
        if (!body || typeof body.passed !== "boolean") {
          c.sendJson(res, 400, { error: "passed 必须是布尔值" })
          return
        }
        const id = String(params.id || "")
        const byId = c.unitsById || {}
        const unit = byId[id]
        if (!unit) {
          c.sendJson(res, 404, { error: "单元不存在", id: id })
          return
        }
        const applied = applyReview({
          unit: unit,
          passed: body.passed,
          note: String(body.note || ""),
          unitsDir: c.unitsDir || "",
          repoRoot: learning.root,
          stateFile: c.stateFile || null,
          today: typeof c.today === "function" ? c.today() : undefined,
        })
        if (!applied.ok) {
          const status = applied.code === "blocked" ? 409 : applied.code === "missing" ? 404 : 500
          c.sendJson(res, status, { error: applied.reason })
          return
        }
        c.sendJson(res, 200, { ok: true, unit: applied.unit })
      },
    },
  ]
}

module.exports = { buildSupplyRoutes: buildSupplyRoutes, LOCAL_WARNING: LOCAL_WARNING }
