"use strict"
/**
 * api-units.js —— 单元查询接口（P0 新增）
 *
 * 契约 §8.1：
 *   GET /api/units     → { total, items: [...] }
 *   GET /api/units/:id → { unit, statement_html }
 *
 * 列表走 units 表（物化视图，不每次扫盘）；详情走启动时载入的单元快照
 * （表里只有摘要字段，全字段的 Unit 与 statement 得从 loader 拿）。
 */

const { toHtml } = require("../core/unit/markdown.js")

/**
 * 组装路由表。
 * @param {{store: object, unitSnapshot: object, sendJson: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildUnitRoutes(ctx) {
  const routes = []
  const store = ctx.store
  const snapshot = ctx.unitSnapshot || { units: [], warnings: [], byId: {} }

  routes.push({
    method: "GET",
    pattern: "/api/units",
    handler: async function (req, res, params, query) {
      const filter = {}
      const track = query.get("track")
      if (track) filter.track = track
      const kind = query.get("kind")
      if (kind) filter.kind = kind
      const status = query.get("status")
      if (status) filter.status = status

      let rows = store.listUnits(filter)

      // q 是标题的模糊搜索。放在 SQL 之外做：表里没有别名字段可搜，
      // 而 units 的规模（几千行）内存过滤完全够快。
      const q = query.get("q")
      if (q) {
        const needle = String(q).toLowerCase()
        rows = rows.filter(function (row) {
          return String(row.title).toLowerCase().indexOf(needle) !== -1
        })
      }

      const total = rows.length

      // limit 非正整数 → 按不限制处理（返回全部匹配项）
      const limitRaw = query.get("limit")
      const limit = limitRaw === null ? NaN : Number(limitRaw)
      if (Number.isFinite(limit) && limit > 0) {
        rows = rows.slice(0, limit)
      }

      ctx.sendJson(res, 200, { total: total, items: rows })
    },
  })

  routes.push({
    method: "GET",
    pattern: "/api/units/:id",
    handler: async function (req, res, params) {
      const id = String(params.id || "")
      const unit = snapshot.byId[id]
      if (!unit) {
        ctx.sendJson(res, 404, { error: "单元不存在", id: id })
        return
      }
      ctx.sendJson(res, 200, {
        unit: unit,
        statement_html: toHtml(unit.statement || ""),
      })
    },
  })

  return routes
}

module.exports = { buildUnitRoutes: buildUnitRoutes }
