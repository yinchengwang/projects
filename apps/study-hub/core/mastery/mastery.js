"use strict"
/**
 * mastery.js —— 掌握度模型与四档诊断
 *
 * 纯函数，无 IO。公式照设计文档 §4.3，一个字不许改：
 *   首次通过率 = firstPass / attempts        （反复重试通过不算掌握）
 *   遗忘因子   = 0.5 ^ (距上次练习天数 / 14)  （半衰期 14 天）
 *   掌握度     = (0.7 × 首次通过率 + 0.3 × e^(-失误次数/2)) × 遗忘因子
 *
 * 覆盖度是第二个维度：练过的不同题目数 / 期望题量。
 * 两个维度交叉出四档诊断，用来区分「题做得少」与「概念没懂」——
 * 这两者的处方完全不同（前者多做题，后者先读笔记）。
 */

/** 遗忘半衰期（天） */
const HALF_LIFE_DAYS = 14

/** 掌握度「高 / 低」的分界 */
const MASTERY_THRESHOLD = 0.5

/** 覆盖度「高 / 低」的分界 */
const COVERAGE_THRESHOLD = 0.5

/** 四档诊断的中文标签 */
const LABELS = {
  "weak-sparse": "薄弱且练得少",
  "weak-dense": "练过但总错",
  "strong-sparse": "偶然会，没练透",
  "strong-dense": "掌握",
}

/** 四档诊断对应的 app 动作（设计文档 §4.3 的「app 的动作」列） */
const ACTIONS = {
  "weak-sparse": "轮次里优先排这类题——先补量",
  "weak-dense": "先回去看概念笔记，别急着刷题——量够了，理解没到位",
  "strong-sparse": "安排复习巩固——会做，但还没练透",
  "strong-dense": "已掌握，不再主动排",
}

/** 处置优先级：越靠前越该先治 */
const URGENCY_ORDER = ["weak-dense", "weak-sparse", "strong-sparse", "strong-dense"]

/**
 * 把任意输入夹到 [0, 1]。
 * @param {*} value
 * @returns {number}
 */
function clamp01(value) {
  const n = Number(value)
  if (!Number.isFinite(n)) return 0
  if (n < 0) return 0
  if (n > 1) return 1
  return n
}

/**
 * 掌握度。
 * @param {{attempts:number, firstPass:number, lapses:number, lastAt:string|null}} stat
 * @param {string} nowIso 当前时刻（ISO8601）
 * @returns {number} 0~1
 */
function masteryOf(stat, nowIso) {
  const s = stat || {}
  const attempts = Number(s.attempts || 0)
  if (!Number.isFinite(attempts) || attempts <= 0) return 0

  const firstPass = Number(s.firstPass || 0)
  const lapses = Number(s.lapses || 0)

  // 首次通过率：反复重试通过不算掌握，所以分母是总提交次数
  const firstPassRate = clamp01(firstPass / attempts)
  // 失误惩罚：失误越多，这一项越接近 0
  const lapseFactor = Math.exp(-(Number.isFinite(lapses) ? lapses : 0) / 2)
  const base = 0.7 * firstPassRate + 0.3 * lapseFactor

  // 遗忘因子：距上次练习的天数
  let days = 0
  if (s.lastAt) {
    const then = new Date(s.lastAt).getTime()
    const now = new Date(nowIso).getTime()
    if (Number.isFinite(then) && Number.isFinite(now) && now > then) {
      days = (now - then) / 86400000
    }
  }
  const decay = Math.pow(0.5, days / HALF_LIFE_DAYS)

  return clamp01(base * decay)
}

/**
 * 覆盖度：练过的不同题目数 / 期望题量。
 * stat 里没有 problems 字段时退回用 attempts（老数据兼容）。
 * @param {{problems?:number, attempts?:number}} stat
 * @param {number} expectedCount 期望题量
 * @returns {number} 0~1
 */
function coverageOf(stat, expectedCount) {
  const s = stat || {}
  const expected = Number(expectedCount)
  if (!Number.isFinite(expected) || expected <= 0) return 0
  const practiced =
    s.problems === undefined || s.problems === null ? Number(s.attempts || 0) : Number(s.problems)
  const safe = Number.isFinite(practiced) ? practiced : 0
  return clamp01(safe / expected)
}

/**
 * 四档诊断。
 * @param {number} mastery 0~1
 * @param {number} coverage 0~1
 * @returns {"weak-sparse"|"weak-dense"|"strong-sparse"|"strong-dense"}
 */
function diagnose(mastery, coverage) {
  const strong = Number(mastery) >= MASTERY_THRESHOLD
  const dense = Number(coverage) >= COVERAGE_THRESHOLD
  if (!strong && !dense) return "weak-sparse"
  if (!strong && dense) return "weak-dense"
  if (strong && !dense) return "strong-sparse"
  return "strong-dense"
}

/**
 * 档位的中文标签。
 * @param {string} bucket
 * @returns {string}
 */
function diagnoseLabel(bucket) {
  return LABELS[bucket] || "未知档位"
}

/**
 * 档位对应的 app 动作。
 * @param {string} bucket
 * @returns {string}
 */
function diagnoseAction(bucket) {
  return ACTIONS[bucket] || "暂无建议动作"
}

/**
 * 给知识点排序：掌握度低的在前。排程器挑题也用这个顺序。
 * @param {object[]} stats 每项至少有 topic / attempts / firstPass / lapses / lastAt
 * @param {string} nowIso
 * @param {Object<string, number>} expectedCounts topic → 期望题量
 * @returns {Array<{topic:string, mastery:number, coverage:number, bucket:string, label:string}>}
 */
function rankTopics(stats, nowIso, expectedCounts) {
  const expected = expectedCounts || {}
  const rows = (stats || []).map(function (s) {
    const topic = s.topic
    const mastery = masteryOf(s, nowIso)
    const coverage = coverageOf(s, expected[topic])
    const bucket = diagnose(mastery, coverage)
    return {
      topic: topic,
      mastery: mastery,
      coverage: coverage,
      bucket: bucket,
      label: diagnoseLabel(bucket),
    }
  })
  rows.sort(function (a, b) {
    if (a.mastery !== b.mastery) return a.mastery - b.mastery
    if (a.topic === b.topic) return 0
    return a.topic < b.topic ? -1 : 1
  })
  return rows
}

/**
 * 按「该先治哪个」排序：练过但总错 > 薄弱且练得少 > 偶然会没练透 > 掌握。
 * 不改原数组。
 * @param {object[]} items 每项至少有 topic / bucket / mastery
 * @returns {object[]}
 */
function sortByUrgency(items) {
  return (items || []).slice().sort(function (a, b) {
    const ia = URGENCY_ORDER.indexOf(a.bucket)
    const ib = URGENCY_ORDER.indexOf(b.bucket)
    const pa = ia === -1 ? URGENCY_ORDER.length : ia
    const pb = ib === -1 ? URGENCY_ORDER.length : ib
    if (pa !== pb) return pa - pb
    if (a.mastery !== b.mastery) return a.mastery - b.mastery
    if (a.topic === b.topic) return 0
    return String(a.topic) < String(b.topic) ? -1 : 1
  })
}

module.exports = {
  HALF_LIFE_DAYS: HALF_LIFE_DAYS,
  MASTERY_THRESHOLD: MASTERY_THRESHOLD,
  COVERAGE_THRESHOLD: COVERAGE_THRESHOLD,
  LABELS: LABELS,
  ACTIONS: ACTIONS,
  URGENCY_ORDER: URGENCY_ORDER,
  masteryOf: masteryOf,
  coverageOf: coverageOf,
  diagnose: diagnose,
  diagnoseLabel: diagnoseLabel,
  diagnoseAction: diagnoseAction,
  rankTopics: rankTopics,
  sortByUrgency: sortByUrgency,
}
