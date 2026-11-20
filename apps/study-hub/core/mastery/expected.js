"use strict"
/**
 * expected.js —— 「期望题量」口径：覆盖度的分母
 *
 * 期望题量 = 这个知识点要练多少单元，才算「练透」。
 *
 * 三级口径（自上而下）：
 *   ① settings 的 insights.expected_override.<track>:<topic> → 手工指定，直接用
 *      （旧的 insights.expected_override.<topic> 裸名键继续能读，作为兼容兜底）
 *   ② 题库里带这个复合键的单元数                        → 这个知识点一共就这么多单元
 *   ③ settings 的 insights.expected_min_questions（默认 5）→ 至少要练够这么多单元
 *
 * ②③ 合成：期望题量 = max(题库单元数, 下限常量)。
 * 下限存在的理由：题库里只有 1 个单元的冷门知识点，练 1 个就是 100% 覆盖，
 * 会被误判成「掌握」——下限把这个洞堵上。
 *
 * 复合键的形状是 `<track>:<topic>`，轨用**顶层轨代码**（algorithm / database / ai），
 * 不是单元 id 前缀（algo / db / ai）。见契约 §4.3 与 P3 计划「统计口径」第 6 节。
 *
 * 退化：题库不可用（纯本地模式 / learning 仓没连 / bank 目录不存在）
 * 或这个 topic 只出现在流水里时，题库计数为 0 → 全部走下限，不抛错。
 */

const { trackOfIdPrefix } = require("../track/tracks.js")

/** 期望题量的默认下限（单元） */
const DEFAULT_MIN_EXPECTED = 5

/** 下限常量的 settings 键 */
const SETTING_MIN = "insights.expected_min_questions"

/** 逐 topic 覆盖值的 settings 键前缀 */
const SETTING_OVERRIDE_PREFIX = "insights.expected_override."

/** 复合键的分隔符 */
const KEY_SEP = ":"

/** 裸知识点名默认归的顶层轨（历史数据全是算法轨） */
const DEFAULT_TRACK = "algorithm"

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
 * 拼复合键。
 * @param {string} track 顶层轨代码；空串按 algorithm
 * @param {string} topic 知识点名
 * @returns {string} "<track>:<topic>"；知识点为空返回 ""
 */
function topicKey(track, topic) {
  const name = String(topic === null || topic === undefined ? "" : topic).trim()
  if (name === "") return ""
  const code = String(track === null || track === undefined ? "" : track).trim()
  return (code === "" ? DEFAULT_TRACK : code) + KEY_SEP + name
}

/**
 * 拆复合键。裸名（没有分隔符）按算法轨处理。
 * @param {string} key
 * @returns {{track: string, topic: string}}
 */
function splitKey(key) {
  const raw = String(key === null || key === undefined ? "" : key)
  const index = raw.indexOf(KEY_SEP)
  if (index <= 0) return { track: DEFAULT_TRACK, topic: raw }
  return { track: raw.slice(0, index), topic: raw.slice(index + 1) }
}

/**
 * 由单元 id 的前缀推顶层轨。
 *   algo:xxx → algorithm；db:xxx → database；ai:xxx → ai
 *   没有前缀的老 id → algorithm
 * @param {string} unitId
 * @returns {string}
 */
function trackOfUnitId(unitId) {
  const raw = String(unitId === null || unitId === undefined ? "" : unitId).trim()
  if (raw === "") return DEFAULT_TRACK
  const index = raw.indexOf(KEY_SEP)
  if (index <= 0) return DEFAULT_TRACK
  const track = trackOfIdPrefix(raw.slice(0, index))
  return track && track.code ? String(track.code) : DEFAULT_TRACK
}

/**
 * 单元自己的轨：优先用 unit.track，没有就用 id 前缀推。
 * @param {{id?: string, track?: string}} unit
 * @returns {string}
 */
function trackOfUnit(unit) {
  const u = unit || {}
  const declared = String(u.track === null || u.track === undefined ? "" : u.track).trim()
  if (declared === "algorithm" || declared === "database" || declared === "ai") return declared
  return trackOfUnitId(u.id)
}

/**
 * 数出题库里每个 topic 有多少个单元。**键是复合键。**
 * @param {Array<{id: string, track?: string, topics: string[]}>} units
 * @returns {Object<string, number>}
 */
function bankTopicCounts(units) {
  const counts = {}
  ;(units || []).forEach(function (unit) {
    const track = trackOfUnit(unit)
    ;(unit.topics || []).forEach(function (topic) {
      const key = topicKey(track, topic)
      if (key === "") return
      counts[key] = (counts[key] || 0) + 1
    })
  })
  return counts
}

/**
 * 读一个覆盖值。正整数才算数，非法值当没写。
 * @param {Object<string, string>} settings
 * @param {string} name 覆盖值的名字（复合键或裸名）
 * @returns {number|null}
 */
function readOverride(settings, name) {
  const raw = (settings || {})[SETTING_OVERRIDE_PREFIX + name]
  if (raw === undefined || raw === null || String(raw).trim() === "") return null
  const value = Number(raw)
  if (!Number.isFinite(value) || value <= 0) return null
  return Math.floor(value)
}

/**
 * 题库计数：先查复合键，查不到再查裸名。
 * @param {Object<string, number>} bankCounts
 * @param {string} key 复合键
 * @param {string} bare 裸知识点名
 * @returns {number}
 */
function pickBankCount(bankCounts, key, bare) {
  const all = bankCounts || {}
  const composite = Number(all[key] || 0)
  if (Number.isFinite(composite) && composite > 0) return composite
  const legacy = Number(all[bare] || 0)
  return Number.isFinite(legacy) ? legacy : 0
}

/**
 * 算一个 topic 的期望题量。
 * @param {string} key 复合键 "<track>:<topic>"（裸名也认）
 * @param {Object<string, number>} bankCounts 键是复合键
 * @param {Object<string, string>} settings
 * @returns {{expected: number, source: "override"|"bank"|"floor"}}
 */
function expectedFor(key, bankCounts, settings) {
  const all = settings || {}
  const parts = splitKey(key)
  const bare = parts.topic

  // ① 覆盖值：复合键优先，裸名兜底
  const composite = readOverride(all, parts.track + KEY_SEP + bare)
  if (composite !== null) return { expected: composite, source: "override" }
  const legacy = readOverride(all, bare)
  if (legacy !== null) return { expected: legacy, source: "override" }

  // ②③ 题库单元数与下限取大
  const min = minExpectedFrom(all)
  const bank = pickBankCount(bankCounts, key, bare)
  const safeBank = Number.isFinite(bank) && bank > 0 ? Math.floor(bank) : 0
  if (safeBank >= min) return { expected: safeBank, source: "bank" }
  return { expected: min, source: "floor" }
}

/**
 * 给一批 topic 批量算期望题量。
 * @param {string[]} keys 复合键（裸名也认）
 * @param {Object<string, number>} bankCounts
 * @param {Object<string, string>} settings
 * @returns {Object<string, {expected: number, source: string}>}
 */
function buildExpectedMap(keys, bankCounts, settings) {
  const out = {}
  ;(keys || []).forEach(function (key) {
    const name = String(key)
    out[name] = expectedFor(name, bankCounts, settings)
  })
  return out
}

module.exports = {
  DEFAULT_MIN_EXPECTED: DEFAULT_MIN_EXPECTED,
  SETTING_MIN: SETTING_MIN,
  SETTING_OVERRIDE_PREFIX: SETTING_OVERRIDE_PREFIX,
  KEY_SEP: KEY_SEP,
  DEFAULT_TRACK: DEFAULT_TRACK,
  minExpectedFrom: minExpectedFrom,
  topicKey: topicKey,
  splitKey: splitKey,
  trackOfUnitId: trackOfUnitId,
  trackOfUnit: trackOfUnit,
  bankTopicCounts: bankTopicCounts,
  expectedFor: expectedFor,
  buildExpectedMap: buildExpectedMap,
}
