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

/** 样例用例最多暴露几条（契约 §8.1.1） */
const SAMPLE_CASE_COUNT = 2

/**
 * 把单元里的 output 断言整理成「样例用例」。
 *
 * 为什么从 asserts 派生而不是另存一份：断言集本来就是从用例派生的
 * （loader A 把 `problem.cases` 逐条转成 `output` 断言），两边各算一次口径迟早打架。
 * 从 asserts 取前 N 条，就天然保证「cases.sample 与 asserts 前 N 条同源同值」。
 *
 * @param {object} unit
 * @returns {{sample: Array<{input: string, expect: string}>, total: number}|null}
 *   不是 problem 类就返回 null（响应里不带 cases 字段）
 */
function buildCases(unit) {
  const src = unit || {}
  if (String(src.kind || "") !== "problem") return null
  const asserts = Array.isArray(src.asserts) ? src.asserts : []
  const cases = asserts.filter(function (item) {
    return item && String(item.type || "") === "output"
  })
  return {
    sample: cases.slice(0, SAMPLE_CASE_COUNT).map(function (item) {
      return {
        input: item.input === undefined || item.input === null ? "" : String(item.input),
        expect: item.expect === undefined || item.expect === null ? "" : String(item.expect),
      }
    }),
    total: cases.length,
  }
}

/**
 * 响应里的 `unit`：`kind === "problem"` 时把 `asserts` 裁到前 2 条。
 *
 * 为什么必须裁：`unit.asserts` 是全量的（loader A 把每条用例都转成一条 output 断言），
 * 不裁的话第 3 条用例的 `input`/`expect` 会从这里漏出去，「第 3 个用例不外泄」就是假保护。
 * 实测：3 条用例的题，响应全文里能搜到 `3,3,0,0,0`。
 *
 * 判题链路不受影响：`judgeUnit` 用的是**服务端内存里的单元索引**，不是这份 HTTP 响应。
 * 需要「一共几条断言」的地方读 `cases.total`，不要读 `unit.asserts.length`。
 * @param {object} unit
 * @returns {object} 裁剪后的副本（不改原对象）
 */
function trimUnit(unit) {
  const src = unit || {}
  if (String(src.kind || "") !== "problem") return src
  const asserts = Array.isArray(src.asserts) ? src.asserts : []
  return Object.assign({}, src, { asserts: asserts.slice(0, SAMPLE_CASE_COUNT) })
}

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
      const payload = {
        unit: trimUnit(unit),
        statement_html: toHtml(unit.statement || ""),
      }
      // 契约 §8.1.1：problem 类带 cases（sample 最多 2 条），card 类不带这个字段。
      // 旧路由 /api/problem/:id 下线前，这两条保护先搬到这里。
      const cases = buildCases(unit)
      if (cases) payload.cases = cases
      ctx.sendJson(res, 200, payload)
    },
  })

  return routes
}

module.exports = {
  buildUnitRoutes: buildUnitRoutes,
  buildCases: buildCases,
  trimUnit: trimUnit,
  SAMPLE_CASE_COUNT: SAMPLE_CASE_COUNT,
}
