"use strict"
/**
 * api-diagnose.js —— GET /api/diagnose：一个接口，三个视图
 *
 * 视图（契约 §8.3）：
 *   view=mastery（缺省）—— 三轨并列 + 知识点明细（下钻用）
 *   view=radar          —— 六轴雷达 + 弱项清单 + 强项清单
 *   view=heatmap        —— 会话热力图
 *
 * 数值全部来自 core/mastery/track-mastery.js（纯函数），这一层只做装配与 HTTP。
 * 优雅降级（G17）：单元清单拿不到就交空视图，绝不 500。
 */

const { HttpError } = require("./router.js")
const { TRACK_CODES } = require("../core/track/tracks.js")
const {
  bankTopicCounts,
  buildExpectedMap,
  trackOfUnit,
  trackOfUnitId,
} = require("../core/mastery/expected.js")
const tm = require("../core/mastery/track-mastery.js")

/** 三个视图名 */
const VIEWS = ["mastery", "radar", "heatmap"]

/** 热力图最多回看多少个会话 */
const HEATMAP_SESSIONS = 20

/** 单元级结论词（G20 冻结的五类）。热力图的格子只有这五种 + pending */
const VERDICTS = ["pass", "wrong_answer", "timeout", "compile_error", "runtime_error"]

/**
 * 取单元清单。
 *
 * 优先用 ctx.units——那是 server/app.js 启动时 loadUnits 出来的那一份，
 * 带 entry.language（六轴的 c / python 两轴要看它），而 units 表只存摘要字段（契约 §4.2）。
 * 没有就退回 ctx.provider.list()（旧题库清单，没有 entry → c / python 两轴为 0，
 * 属于 G17 的优雅降级，不是错误）。
 * @param {{units?: object[], provider?: object}} ctx
 * @returns {Promise<object[]>}
 */
async function unitsOf(ctx) {
  if (Array.isArray(ctx.units)) return ctx.units
  if (ctx.provider && typeof ctx.provider.list === "function") {
    try {
      const list = await ctx.provider.list()
      return Array.isArray(list) ? list : []
    } catch (err) {
      // 题库不可用（纯本地模式 / learning 仓没连）——空视图，不报错
      return []
    }
  }
  return []
}

/**
 * 算三层掌握度（纯装配；数值口径全在 core/mastery/track-mastery.js 里）。
 * @param {{store: object}} ctx
 * @param {object[]} units
 * @param {string} nowIso
 * @returns {object}
 */
function buildModel(ctx, units, nowIso) {
  const store = ctx.store
  const attempts = store.listAttemptSummaries()
  const unitStats = tm.deriveUnitStats(attempts)
  const unitMastery = tm.computeUnitMastery(unitStats, nowIso)
  const bankCounts = bankTopicCounts(units)
  const expectedMap = buildExpectedMap(Object.keys(bankCounts), bankCounts, store.allSettings())
  return {
    attempts: attempts,
    unitStats: unitStats,
    unitMastery: unitMastery,
    expectedMap: expectedMap,
    trackRows: tm.trackRowsFor(units, unitMastery),
    topicRows: tm.topicRowsFor(units, unitMastery, expectedMap),
  }
}

/**
 * 把三层掌握度物化进 mastery 表（整体覆盖，幂等——Task 4 的 replaceMastery）。
 * @param {{store: object}} ctx
 * @param {object} model
 * @param {string} nowIso
 * @returns {void}
 */
function materialize(ctx, model, nowIso) {
  const rows = []
  Object.keys(model.unitMastery).forEach(function (unitId) {
    rows.push({
      scope: "unit",
      scope_id: unitId,
      level: model.unitMastery[unitId],
      updated_at: nowIso,
    })
  })
  model.topicRows.forEach(function (row) {
    rows.push({ scope: "topic", scope_id: row.id, level: row.mastery, updated_at: nowIso })
  })
  model.trackRows.forEach(function (row) {
    rows.push({ scope: "track", scope_id: row.track, level: row.mastery, updated_at: nowIso })
  })
  tm.materializeMastery(ctx.store, rows)
}

/**
 * view=mastery：三轨并列 + 知识点明细。
 *
 * summary 四个数的口径（契约 §8.3.1 只给了字段名，这里定死）：
 *   unitCount     = 练过的单元数（有过作答记录的单元）
 *   topicCount    = 练过的知识点数（至少有一个单元练过）
 *   sessionCount  = 会话总数
 *   totalAttempts = 总提交数
 *
 * @param {{store: object}} ctx
 * @param {object} model
 * @param {object[]} units
 * @returns {object}
 */
function masteryView(ctx, model, units) {
  const store = ctx.store
  const sessions = typeof store.listSessions === "function" ? store.listSessions() : []
  const practicedTopics = model.topicRows.filter(function (row) {
    return row.unitCount > 0
  })
  return {
    view: "mastery",
    tracks: model.trackRows,
    // 契约 §8.3.1 的 topics 行就这七个字段——内部算用的 id / unitCount 不外传
    topics: model.topicRows.map(function (row) {
      return {
        track: row.track,
        topic: row.topic,
        mastery: row.mastery,
        coverage: row.coverage,
        bucket: row.bucket,
        label: row.label,
        action: row.action,
      }
    }),
    summary: {
      unitCount: Object.keys(model.unitMastery).length,
      topicCount: practicedTopics.length,
      sessionCount: sessions.length,
      totalAttempts: model.attempts.length,
    },
  }
}

/**
 * view=radar：六轴 + 弱项 + 强项。
 *
 * 契约 §8.3.1：algorithm / database / ai 三根轴取自 mastery 表的 scope='track'。
 * 所以这里先读表；读不到（表是空的）才退回本次算出来的轨道值。
 * @param {{store: object}} ctx
 * @param {object} model
 * @param {object[]} units
 * @returns {object}
 */
function radarView(ctx, model, units) {
  const levels = {}
  if (typeof ctx.store.listMastery === "function") {
    ;(ctx.store.listMastery("track") || []).forEach(function (row) {
      levels[String(row.scope_id)] = Number(row.level) || 0
    })
  }
  const trackRows = TRACK_CODES.map(function (code) {
    const fromTable = levels[code]
    const row = model.trackRows.filter(function (item) {
      return item.track === code
    })[0]
    const value = typeof fromTable === "number" ? fromTable : row ? row.mastery : 0
    return { track: code, mastery: value }
  })
  return {
    view: "radar",
    axes: tm.radarAxesFor(trackRows, units, model.unitMastery),
    weak: tm.weakUnitsFor(units, model.unitMastery),
    strong: tm.strongUnitsFor(units, model.unitMastery),
  }
}

/**
 * 某单元的作答流水。新的 listAttemptsByUnit 没有时退回旧的 listAttemptsByProblem
 * （把单元 id 的前缀去掉——旧流水存的是不带前缀的题号）。
 * @param {object} store
 * @param {string} unitId
 * @returns {object[]}
 */
function attemptsOfUnit(store, unitId) {
  if (!unitId) return []
  if (typeof store.listAttemptsByUnit === "function") {
    const rows = store.listAttemptsByUnit(unitId)
    return Array.isArray(rows) ? rows : []
  }
  if (typeof store.listAttemptsByProblem === "function") {
    const index = unitId.indexOf(":")
    const bare = index === -1 ? unitId : unitId.slice(index + 1)
    const rows = store.listAttemptsByProblem(bare)
    return Array.isArray(rows) ? rows : []
  }
  return []
}

/**
 * 格子的状态：该单元最近一次作答的结论；没作答过就是 pending。
 * 队列里还是 pending / skipped 的项一律 pending。
 * @param {{state?: string}} item 队列项
 * @param {object[]} attempts 该单元的作答流水
 * @returns {string} 六种之一
 */
function cellState(item, attempts) {
  const state = String((item && item.state) || "")
  if (state === "pending" || state === "skipped") return "pending"
  const list = (attempts || []).slice().sort(function (a, b) {
    const ta = String(a.ts || "")
    const tb = String(b.ts || "")
    if (ta !== tb) return ta < tb ? -1 : 1
    return Number(a.id || 0) - Number(b.id || 0)
  })
  if (!list.length) return "pending"
  const verdict = String(list[list.length - 1].verdict || "")
  return VERDICTS.indexOf(verdict) === -1 ? "pending" : verdict
}

/**
 * view=heatmap：会话热力图。
 *
 * 横轴是会话序号（最近 HEATMAP_SESSIONS 个会话，按时间升序，序号从 1 开始）；
 * 纵轴是三轨；格子是那个单元的最近一次作答结论。
 * @param {{store: object}} ctx
 * @param {object[]} units
 * @returns {object}
 */
function heatmapView(ctx, units) {
  const store = ctx.store
  const trackOfId = {}
  ;(units || []).forEach(function (unit) {
    if (unit && unit.id) trackOfId[String(unit.id)] = trackOfUnit(unit)
  })

  const recent =
    typeof store.listSessions === "function" ? store.listSessions(HEATMAP_SESSIONS) || [] : []
  // listSessions 是 started_at DESC；排成升序，序号从 1 开始
  const ordered = recent.slice().sort(function (a, b) {
    const ta = String(a.started_at || "")
    const tb = String(b.started_at || "")
    if (ta !== tb) return ta < tb ? -1 : 1
    return String(a.id) < String(b.id) ? -1 : 1
  })

  const sessions = []
  const cells = []
  ordered.forEach(function (session, index) {
    sessions.push({ id: session.id, index: index + 1, started_at: session.started_at })
    const queue = typeof store.listQueue === "function" ? store.listQueue(session.id) || [] : []
    queue.forEach(function (item) {
      const unitId = String(item.unit_id || "")
      cells.push({
        session_id: session.id,
        track: trackOfId[unitId] || trackOfUnitId(unitId),
        state: cellState(item, attemptsOfUnit(store, unitId)),
        seq: Number(item.seq) || 0,
      })
    })
  })
  return { view: "heatmap", sessions: sessions, cells: cells }
}

/**
 * 组装 P3 的诊断路由。
 * @param {{store: object, provider?: object, sendJson: Function, units?: object[], nowIso?: string}} ctx
 *   units：启动时 loadUnits 的结果（可选，缺省退回 provider.list()）
 *   nowIso：当前时刻（可选，测试用；缺省取系统时间）
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildDiagnoseRoutes(ctx) {
  return [
    {
      method: "GET",
      pattern: "/api/diagnose",
      handler: async function (req, res, params, query) {
        const raw = query.get("view")
        const view = raw === null || String(raw).trim() === "" ? "mastery" : String(raw).trim()
        if (VIEWS.indexOf(view) === -1) {
          throw new HttpError(400, "诊断视图只能是 mastery / radar / heatmap")
        }
        const nowIso = ctx.nowIso || new Date().toISOString()
        const units = await unitsOf(ctx)
        const model = buildModel(ctx, units, nowIso)
        // 先物化再读表：radar 的三轨轴取自 mastery 表的 scope='track'
        materialize(ctx, model, nowIso)

        let body
        if (view === "radar") body = radarView(ctx, model, units)
        else if (view === "heatmap") body = heatmapView(ctx, units)
        else body = masteryView(ctx, model, units)
        ctx.sendJson(res, 200, body)
      },
    },
  ]
}

module.exports = {
  VIEWS: VIEWS,
  HEATMAP_SESSIONS: HEATMAP_SESSIONS,
  VERDICTS: VERDICTS,
  buildDiagnoseRoutes: buildDiagnoseRoutes,
}
