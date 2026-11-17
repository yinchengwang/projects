"use strict"
/**
 * pool.js —— 候选池与到期集合
 *
 * 候选池口径（契约 §1.4 / G41，冻结）：
 *   可学 = status === "verified" ∧（未完成 ∨ 已到期）
 *   draft / rejected / todo 一律不进池——供给流水线没验过的内容不拿去上课。
 *   同一单元不重复计数（做完了但到期的，进池且计 due）。
 *
 * 到期是双通道（契约 §6.4）：日期通道（next_review_at <= now）∨ 次数通道（units_since >= K）。
 *
 * 纯函数：不碰数据库。调用方把 units 表的行、unit_progress 表的行、review_cards 的行喂进来。
 */

const { TRACK_CODES } = require("../track/tracks.js")
const { isDue, isDueByCount } = require("../review/sm2.js")

/**
 * 把卡片对上单元 id：优先 unit_id，其次 problem_id（无前缀按算法轨补）。
 * @param {object} card
 * @returns {string}
 */
function resolveUnitId(card) {
  const src = card || {}
  const raw = src.unit_id === undefined || src.unit_id === null ? "" : String(src.unit_id)
  if (raw !== "") return raw
  const legacy = src.problem_id === undefined || src.problem_id === null ? "" : String(src.problem_id)
  if (legacy === "") return ""
  if (legacy.indexOf(":") >= 0) return legacy
  return "algo:" + legacy
}

/**
 * 算出到期单元集合。
 * @param {Array<object>} cards review_cards 表的行
 * @param {{[unitId: string]: object}} unitsById 单元 id → unit
 * @param {{nowIso?: string, k?: number}} opts
 * @returns {{dueUnitIds: Set<string>, dueByTrack: {[track: string]: number}, unknownUnitIds: string[]}}
 */
function collectDue(cards, unitsById, opts) {
  const options = opts || {}
  const nowIso = options.nowIso || new Date().toISOString()
  const k = options.k === undefined || options.k === null ? 5 : Number(options.k)
  const byId = unitsById || {}
  const dueUnitIds = new Set()
  const dueByTrack = {}
  const unknownUnitIds = []
  const seenUnknown = new Set()

  ;(cards || []).forEach(function (card) {
    const unitId = resolveUnitId(card)
    if (unitId === "") return
    const due = isDue(card, nowIso) || isDueByCount(card, k)
    if (!due) return
    const unit = byId[unitId]
    if (!unit) {
      // 卡指向的单元已经不在库里：记下来，别让它悄悄消失
      if (!seenUnknown.has(unitId)) {
        seenUnknown.add(unitId)
        unknownUnitIds.push(unitId)
      }
      return
    }
    // 契约 §6.0：**同一个单元只算一次**。Set 天然去重，但 dueByTrack 是按卡累加的，
    // 所以这里必须先查「是不是已经算过」，否则两张卡指向同一单元时会得到 2，
    // 与 collectPools().counts[track].due（按单元算）= 1 互相矛盾。
    if (dueUnitIds.has(unitId)) return
    dueUnitIds.add(unitId)
    const track = String(unit.track || "")
    dueByTrack[track] = (dueByTrack[track] || 0) + 1
  })

  return { dueUnitIds: dueUnitIds, dueByTrack: dueByTrack, unknownUnitIds: unknownUnitIds }
}

/**
 * 按轨建候选池并统计。
 * @param {{units: Array<object>, progressByUnit?: {[unitId: string]: string}, dueUnitIds?: Set<string>|string[], tracks?: string[]}} input
 * @returns {{pools: {[track: string]: Array<object>}, counts: {[track: string]: {candidates: number, due: number}}, dueUnitIds: Set<string>}}
 */
function collectPools(input) {
  const src = input || {}
  const units = Array.isArray(src.units) ? src.units : []
  const progressByUnit = src.progressByUnit || {}
  const dueUnitIds = src.dueUnitIds instanceof Set ? src.dueUnitIds : new Set(src.dueUnitIds || [])
  const tracks =
    Array.isArray(src.tracks) && src.tracks.length > 0 ? src.tracks : TRACK_CODES.slice()

  const pools = {}
  const counts = {}
  tracks.forEach(function (track) {
    pools[track] = []
    counts[track] = { candidates: 0, due: 0 }
  })

  units.forEach(function (unit) {
    if (!unit) return
    const track = String(unit.track || "")
    if (!Object.prototype.hasOwnProperty.call(pools, track)) return
    const id = String(unit.id || "")
    if (id === "") return
    if (String(unit.status || "") !== "verified") return
    const progress = String(progressByUnit[id] || "todo")
    const due = dueUnitIds.has(id)
    if (progress === "done" && !due) return
    pools[track].push(unit)
    counts[track].candidates += 1
    if (due) counts[track].due += 1
  })

  return { pools: pools, counts: counts, dueUnitIds: dueUnitIds }
}

module.exports = {
  collectDue: collectDue,
  collectPools: collectPools,
  resolveUnitId: resolveUnitId,
}
