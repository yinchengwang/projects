"use strict"
/**
 * topic-stats.js —— 从作答流水派生每个「知识点」的统计量
 *
 * 纯函数，无 IO。口径对应设计文档 §4.3 的「首次通过率 = 首次尝试就过的比例」：
 *   attempts   = 该知识点下所有单元的提交总次数
 *   firstPass  = 该知识点下「第一次尝试就通过」的单元数（反复重试通过不算）
 *   lapses     = 该知识点下没通过的提交次数（失误次数）
 *   lastAt     = 该知识点下最后一次提交的时间
 *   unitCount  = 该知识点下练过的不同单元数（覆盖度的分子）
 *
 * P3 起知识点带轨：一条记录是「某轨的某个知识点」，所以每行给三份信息：
 *   id    = "algorithm:数组"（复合键，唯一；与 topics.id / mastery.scope_id 同形）
 *   track = "algorithm"（顶层轨代码）
 *   topic = "数组"（裸知识点名，给旧页面显示用）
 *
 * 单元 id 归一跟跨轨聚合用同一套规则（core/mastery/track-mastery.js 的 unitIdOf）：
 * unit_id 优先，其次 problem_id；没有前缀的老 id 归算法轨。
 * 单元对象的主键字段叫 id，喂给 unitIdOf 时包一层——规则只有一处。
 *
 * 题库里查不到知识点的单元不计入——没有知识点归属就没法诊断。
 */

const { unitIdOf } = require("./track-mastery.js")
const { topicKey, trackOfUnit } = require("./expected.js")

/** 一次作答视为「通过」的判定 */
function isPass(verdict) {
  return String(verdict || "") === "pass"
}

/**
 * 单元 id 归一。单元对象的主键字段是 id，unitIdOf 读的是 unit_id / problem_id——
 * 这里包一层喂给它，规则（无前缀归算法轨）仍然只有 unitIdOf 一处实现。
 * @param {{id?: string, unit_id?: string, problem_id?: string}} unit
 * @returns {string} 归一后的单元 id；取不到返回 ""
 */
function unitKeyOf(unit) {
  const source = unit || {}
  return unitIdOf({ unit_id: source.id || source.unit_id || source.problem_id })
}

/**
 * 派生知识点统计。
 * @param {Array<{id?: number, unit_id?: string, problem_id?: string, ts: string, verdict: string}>} attempts
 * @param {Array<{id?: string, unit_id?: string, track?: string, topics?: string[]}>} units
 * @returns {Array<{id: string, track: string, topic: string, attempts: number, firstPass: number, lapses: number, lastAt: string|null, unitCount: number}>}
 */
function deriveTopicStats(attempts, units) {
  // 单元 id → {track, topics}
  const unitsOfId = {}
  ;(units || []).forEach(function (unit) {
    const id = unitKeyOf(unit)
    if (id === "") return
    const topics = (unit.topics || []).map(String).filter(function (t) {
      return t !== ""
    })
    unitsOfId[id] = { track: trackOfUnit(unit), topics: topics }
  })

  // 单元 id → 该单元的历次作答
  const attemptsOfUnit = {}
  ;(attempts || []).forEach(function (attempt) {
    const id = unitIdOf(attempt)
    if (id === "") return
    if (!attemptsOfUnit[id]) attemptsOfUnit[id] = []
    attemptsOfUnit[id].push(attempt)
  })

  const stats = {}

  Object.keys(attemptsOfUnit).forEach(function (id) {
    const unit = unitsOfId[id]
    if (!unit || unit.topics.length === 0) return

    // 按时间排序；时间相同按自增 id 排序——「第一次尝试」必须是真正的第一次
    const list = attemptsOfUnit[id].slice().sort(function (a, b) {
      const ta = String(a.ts || "")
      const tb = String(b.ts || "")
      if (ta !== tb) return ta < tb ? -1 : 1
      return Number(a.id || 0) - Number(b.id || 0)
    })

    const firstPassed = isPass(list[0].verdict)
    let lapses = 0
    let lastAt = null
    let lastMs = -Infinity
    list.forEach(function (attempt) {
      if (!isPass(attempt.verdict)) lapses += 1
      const ts = String(attempt.ts || "")
      const ms = Date.parse(ts)
      if (ts !== "" && Number.isFinite(ms) && ms > lastMs) {
        lastMs = ms
        lastAt = ts
      }
    })

    unit.topics.forEach(function (topic) {
      const key = topicKey(unit.track, topic)
      if (key === "") return
      if (!stats[key]) {
        stats[key] = {
          id: key,
          track: unit.track,
          topic: topic,
          attempts: 0,
          firstPass: 0,
          lapses: 0,
          lastAt: null,
          unitCount: 0,
        }
      }
      const row = stats[key]
      row.attempts += list.length
      row.unitCount += 1
      if (firstPassed) row.firstPass += 1
      row.lapses += lapses
      if (lastAt !== null && (row.lastAt === null || Date.parse(lastAt) > Date.parse(row.lastAt))) {
        row.lastAt = lastAt
      }
    })
  })

  return Object.keys(stats)
    .map(function (key) {
      return stats[key]
    })
    .sort(function (a, b) {
      if (a.id === b.id) return 0
      return a.id < b.id ? -1 : 1
    })
}

module.exports = { deriveTopicStats: deriveTopicStats }
