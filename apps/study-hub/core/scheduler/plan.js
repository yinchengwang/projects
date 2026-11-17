"use strict"
/**
 * plan.js —— 组装一次会话的学习计划
 *
 * 把「谁到期了 → 谁最该学 → 这次排几个 → 怎么交错」串成一步：
 *   1. 到期集合（日期通道 ∨ 次数通道）
 *   2. 环境门禁（缺 numpy 的单元不进池）
 *   3. 候选池（verified ∧（未完成 ∨ 到期））
 *   4. 轨内优先级（到期 > 当前轮次 > 新单元）
 *   5. 跨轨配额（最大余数法）
 *   6. 跨轨交错成一条队列
 *
 * 纯函数：不读数据库、不读盘、不读时钟（nowIso 由调用方给）。
 */

const { allocateQuota } = require("./quota.js")
const { buildQueue } = require("./queue.js")
const { prioritize } = require("./prioritize.js")
const { collectDue, collectPools } = require("./pool.js")

/**
 * 这个单元要不要 numpy。
 * AI 轨的 13 张卡全部 import numpy；数据库轨只有 clustering 这张要（契约 §14.1）。
 * @param {object} unit
 * @returns {string} 需要 numpy 时给一句中文说明；不需要则空串
 */
function numpyRequirement(unit) {
  const id = String((unit && unit.id) || "")
  const track = String((unit && unit.track) || "")
  if (track === "ai") return "AI 轨的卡依赖 numpy"
  if (id === "db:clustering") return "这张卡依赖 numpy"
  return ""
}

/**
 * 单元索引：按 id、按去前缀的 id、按标题各建一张表。
 * @param {{[unitId: string]: object}} unitsById
 * @returns {{byId: Map, byStem: Map, byTitle: Map}}
 */
function buildUnitIndex(unitsById) {
  const byId = new Map()
  const byStem = new Map()
  const byTitle = new Map()
  Object.keys(unitsById || {}).forEach(function (id) {
    const unit = unitsById[id]
    byId.set(String(id), unit)
    const text = String(id)
    const stem = text.indexOf(":") >= 0 ? text.slice(text.indexOf(":") + 1) : text
    if (!byStem.has(stem)) byStem.set(stem, unit)
    const title = String((unit && unit.title) || "")
    if (title && !byTitle.has(title)) byTitle.set(title, unit)
  })
  return { byId: byId, byStem: byStem, byTitle: byTitle }
}

/**
 * 轮次条目对上单元：先按 id，再按去前缀的 id，再按标题。
 * @param {object} item
 * @param {{byId: Map, byStem: Map, byTitle: Map}} index
 * @returns {object|null}
 */
function matchUnit(item, index) {
  if (!item) return null
  const id = String(item.id || "")
  if (id && index.byId.has(id)) return index.byId.get(id)
  if (id && index.byStem.has(id)) return index.byStem.get(id)
  const title = String(item.title || "")
  if (title && index.byTitle.has(title)) return index.byTitle.get(title)
  return null
}

/**
 * 当前轮次上下文：哪些单元在当前轮次里、排第几、哪些已经做完。
 * @param {Array<object>} rounds parseRounds 的 rounds
 * @param {{[unitId: string]: object}} unitsById
 * @returns {{currentRoundUnitIds: Set<string>, roundOrder: string[], doneUnitIds: Set<string>, roundId: string}}
 */
function roundContext(rounds, unitsById) {
  const out = {
    currentRoundUnitIds: new Set(),
    roundOrder: [],
    doneUnitIds: new Set(),
    roundId: "",
  }
  const list = Array.isArray(rounds) ? rounds : []
  if (list.length === 0) return out
  let round = null
  list.forEach(function (r) {
    if (!round && r && r.status === "active") round = r
  })
  if (!round) round = list[list.length - 1]
  if (!round) return out
  out.roundId = String(round.id || "")
  const index = buildUnitIndex(unitsById)
  ;(round.items || []).forEach(function (item) {
    const unit = matchUnit(item, index)
    if (!unit) return
    const unitId = String(unit.id)
    out.roundOrder.push(unitId)
    if (String(item.status) === "done") out.doneUnitIds.add(unitId)
    else out.currentRoundUnitIds.add(unitId)
  })
  return out
}

/**
 * 排一次会话的完整计划。
 * @param {{
 *   n: number,
 *   nowIso: string,
 *   k: number,
 *   units: Array<object>,
 *   progressByUnit?: {[unitId: string]: string},
 *   cards?: Array<object>,
 *   masteryByUnit?: {[unitId: string]: number},
 *   rounds?: Array<object>,
 *   tracks: Array<{code: string, label: string, weight: number, enabled: boolean}>,
 *   env?: {numpy?: {ok: boolean}}
 * }} input
 * @returns {object} 计划
 */
function buildPlan(input) {
  const src = input || {}
  const nowIso = src.nowIso || new Date().toISOString()
  const k = Number(src.k === undefined || src.k === null ? 5 : src.k)
  const n = Number(src.n === undefined || src.n === null ? 3 : src.n)
  const units = Array.isArray(src.units) ? src.units : []
  const progressByUnit = src.progressByUnit || {}
  const cards = Array.isArray(src.cards) ? src.cards : []
  const masteryByUnit = src.masteryByUnit || {}
  const tracks = Array.isArray(src.tracks) ? src.tracks : []
  const numpyOk = !!(src.env && src.env.numpy && src.env.numpy.ok)
  const warnings = []

  const unitsById = {}
  units.forEach(function (unit) {
    if (unit && unit.id) unitsById[String(unit.id)] = unit
  })

  // 1. 到期集合（日期通道 ∨ 次数通道）
  const due = collectDue(cards, unitsById, { nowIso: nowIso, k: k })
  if (due.unknownUnitIds.length > 0) {
    warnings.push("有 " + due.unknownUnitIds.length + " 张复习卡指向了已不存在的单元，已跳过")
  }

  // 2. 环境门禁：缺 numpy 的单元不进池
  const kept = []
  units.forEach(function (unit) {
    const need = numpyRequirement(unit)
    if (need && !numpyOk) {
      const title = String((unit && unit.title) || (unit && unit.id) || "")
      warnings.push("「" + title + "」" + need + "，环境缺失，这次跳过")
      return
    }
    kept.push(unit)
  })

  // 3. 候选池
  const raw = collectPools({
    units: kept,
    progressByUnit: progressByUnit,
    dueUnitIds: due.dueUnitIds,
  })

  // 4. 轮次上下文与已完成集合
  const round = roundContext(src.rounds, unitsById)
  const doneUnitIds = new Set()
  Object.keys(progressByUnit).forEach(function (id) {
    if (String(progressByUnit[id]) === "done") doneUnitIds.add(id)
  })
  round.doneUnitIds.forEach(function (id) {
    doneUnitIds.add(id)
  })

  // 5. 轨内优先级
  const pools = {}
  tracks.forEach(function (t) {
    const code = String(t.code)
    pools[code] = prioritize(raw.pools[code] || [], {
      dueUnitIds: due.dueUnitIds,
      currentRoundUnitIds: round.currentRoundUnitIds,
      roundOrder: round.roundOrder,
      masteryByUnit: masteryByUnit,
      doneUnitIds: doneUnitIds,
    })
  })

  // 6. 跨轨配额（关掉的轨不参与）
  const quotaInput = tracks
    .filter(function (t) {
      return t.enabled !== false
    })
    .map(function (t) {
      const code = String(t.code)
      const c = raw.counts[code] || { candidates: 0, due: 0 }
      return {
        track: code,
        weight: Number(t.weight || 0),
        due: c.due,
        candidates: c.candidates,
      }
    })
  const quota = allocateQuota({ n: n, tracks: quotaInput, minPerTrack: 0 })
  warnings.push.apply(warnings, quota.reasons)

  // 7. 跨轨交错成一条队列
  const queue = buildQueue({ quota: quota.quota, pools: pools })

  const quotaOut = {}
  tracks.forEach(function (t) {
    const code = String(t.code)
    quotaOut[code] = quota.quota[code] || 0
  })

  return {
    quota: quotaOut,
    demand: quota.demand,
    weights: quota.weights,
    needMore: quota.needMore,
    queue: queue,
    dueUnitIds: due.dueUnitIds,
    currentRoundUnitIds: round.currentRoundUnitIds,
    roundOrder: round.roundOrder,
    roundId: round.roundId,
    doneUnitIds: doneUnitIds,
    counts: raw.counts,
    warnings: warnings,
  }
}

module.exports = {
  buildPlan: buildPlan,
  numpyRequirement: numpyRequirement,
  roundContext: roundContext,
}
