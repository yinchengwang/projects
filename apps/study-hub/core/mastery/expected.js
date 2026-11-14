"use strict"
/**
 * expected.js —— 「期望题量」口径：覆盖度的分母
 *
 * 期望题量 = 这个知识点要练多少题，才算「练透」。
 *
 * 三级口径（自上而下）：
 *   ① settings 的 insights.expected_override.<topic> → 手工指定，直接用
 *   ② 题库里带这个 topic 的题目数                       → 这个知识点一共就这么多题
 *   ③ settings 的 insights.expected_min_questions（默认 5）→ 至少要练够这么多题
 *
 * ②③ 合成：期望题量 = max(题库题数, 下限常量)。
 * 下限存在的理由：题库里只有 1 道题的冷门知识点，练 1 道就是 100% 覆盖，
 * 会被误判成「掌握」——下限把这个洞堵上。
 *
 * 退化：题库不可用（纯本地模式 / learning 仓没连 / bank 目录不存在）
 * 或这个 topic 只出现在流水里时，题库题数为 0 → 全部走下限，不抛错。
 */

/** 期望题量的默认下限（题） */
const DEFAULT_MIN_EXPECTED = 5

/** 下限常量的 settings 键 */
const SETTING_MIN = "insights.expected_min_questions"

/** 逐 topic 覆盖值的 settings 键前缀 */
const SETTING_OVERRIDE_PREFIX = "insights.expected_override."

/**
 * 读下限常量。非法值退回默认 5。
 * @param {Object<string, string>} settings
 * @returns {number}
 */
function minExpectedFrom(settings) {
  const raw = (settings || {})[SETTING_MIN]
  if (raw === undefined || raw === null || String(raw).trim() === "") return DEFAULT_MIN_EXPECTED
  const value = Number(raw)
  if (!Number.isFinite(value) || value <= 0) return DEFAULT_MIN_EXPECTED
  return Math.floor(value)
}

/**
 * 数出题库里每个 topic 有多少题。
 * @param {Array<{id: string, topics: string[]}>} problems
 * @returns {Object<string, number>}
 */
function bankTopicCounts(problems) {
  const counts = {}
  ;(problems || []).forEach(function (problem) {
    ;(problem.topics || []).forEach(function (topic) {
      const key = String(topic)
      if (key === "") return
      counts[key] = (counts[key] || 0) + 1
    })
  })
  return counts
}

/**
 * 算一个 topic 的期望题量。
 * @param {string} topic
 * @param {Object<string, number>} bankCounts
 * @param {Object<string, string>} settings
 * @returns {{expected: number, source: "override"|"bank"|"floor"}}
 */
function expectedFor(topic, bankCounts, settings) {
  const all = settings || {}
  const overrideRaw = all[SETTING_OVERRIDE_PREFIX + topic]
  if (overrideRaw !== undefined && overrideRaw !== null && String(overrideRaw).trim() !== "") {
    const value = Number(overrideRaw)
    if (Number.isFinite(value) && value > 0) {
      return { expected: Math.floor(value), source: "override" }
    }
  }

  const min = minExpectedFrom(all)
  const bank = Number((bankCounts || {})[topic] || 0)
  const safeBank = Number.isFinite(bank) && bank > 0 ? Math.floor(bank) : 0
  if (safeBank >= min) return { expected: safeBank, source: "bank" }
  return { expected: min, source: "floor" }
}

/**
 * 给一批 topic 批量算期望题量。
 * @param {string[]} topics
 * @param {Object<string, number>} bankCounts
 * @param {Object<string, string>} settings
 * @returns {Object<string, {expected: number, source: string}>}
 */
function buildExpectedMap(topics, bankCounts, settings) {
  const out = {}
  ;(topics || []).forEach(function (topic) {
    const key = String(topic)
    out[key] = expectedFor(key, bankCounts, settings)
  })
  return out
}

module.exports = {
  DEFAULT_MIN_EXPECTED: DEFAULT_MIN_EXPECTED,
  SETTING_MIN: SETTING_MIN,
  SETTING_OVERRIDE_PREFIX: SETTING_OVERRIDE_PREFIX,
  minExpectedFrom: minExpectedFrom,
  bankTopicCounts: bankTopicCounts,
  expectedFor: expectedFor,
  buildExpectedMap: buildExpectedMap,
}
