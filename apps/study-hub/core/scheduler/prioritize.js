"use strict"
/**
 * prioritize.js —— 轨内优先级
 *
 * 一轨内部的排序键（从高到低）：
 *   1. 到期复习的单元（dueUnitIds）
 *   2. 当前轮次里未完成的单元（按 roundOrder 的下标）
 *   3. 其余新单元（掌握度升序——越弱越先；再按 id 升序）
 *   4. 已完成的单元排到最后（仍可被选，可能要重练）
 *
 * 纯函数：不碰数据库，同输入必同输出。
 */

/** 排位：数字越小越靠前 */
const RANK_DUE = 0
const RANK_ROUND = 1
const RANK_NEW = 2
const RANK_DONE = 3

/**
 * 把 Set 或数组统一成 Set。
 * @param {Set<string>|string[]|null|undefined} value
 * @returns {Set<string>}
 */
function toSet(value) {
  if (!value) return new Set()
  if (value instanceof Set) return value
  if (Array.isArray(value)) {
    const out = new Set()
    value.forEach(function (item) {
      out.add(String(item))
    })
    return out
  }
  return new Set()
}

/**
 * 读掌握度；没有记录按 0 算（0 = 越弱越先）。
 * @param {{[unitId: string]: number}} mastery
 * @param {string} id
 * @returns {number}
 */
function levelOf(mastery, id) {
  const raw = Number(mastery[id])
  if (!Number.isFinite(raw)) return 0
  return raw
}

/**
 * 给一轨的候选单元排出优先级顺序。
 * @param {Array<object>} units
 * @param {{dueUnitIds?: Set<string>|string[], currentRoundUnitIds?: Set<string>|string[], roundOrder?: string[], masteryByUnit?: {[unitId: string]: number}, doneUnitIds?: Set<string>|string[]}} ctx
 * @returns {Array<object>}
 */
function prioritize(units, ctx) {
  const c = ctx || {}
  const due = toSet(c.dueUnitIds)
  const round = toSet(c.currentRoundUnitIds)
  const done = toSet(c.doneUnitIds)
  const order = Array.isArray(c.roundOrder) ? c.roundOrder : []
  const mastery = c.masteryByUnit || {}

  const orderIndex = {}
  order.forEach(function (id, index) {
    orderIndex[String(id)] = index
  })

  const list = (units || []).slice()
  list.sort(function (a, b) {
    const ra = rankOf(a)
    const rb = rankOf(b)
    if (ra !== rb) return ra - rb
    if (ra === RANK_ROUND) {
      const ia = orderIndex[String(a.id)]
      const ib = orderIndex[String(b.id)]
      const va = ia === undefined ? Number.MAX_SAFE_INTEGER : ia
      const vb = ib === undefined ? Number.MAX_SAFE_INTEGER : ib
      if (va !== vb) return va - vb
    }
    if (ra === RANK_NEW) {
      const ma = levelOf(mastery, a.id)
      const mb = levelOf(mastery, b.id)
      if (ma !== mb) return ma - mb
    }
    const ida = String(a.id)
    const idb = String(b.id)
    if (ida < idb) return -1
    if (ida > idb) return 1
    return 0
  })
  return list

  /**
   * 一个单元的排位。
   * @param {object} unit
   * @returns {number}
   */
  function rankOf(unit) {
    const id = String(unit && unit.id)
    if (due.has(id)) return RANK_DUE
    if (done.has(id)) return RANK_DONE
    if (round.has(id)) return RANK_ROUND
    return RANK_NEW
  }
}

module.exports = { prioritize: prioritize }
