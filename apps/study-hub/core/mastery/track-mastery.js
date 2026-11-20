"use strict"
/**
 * track-mastery.js —— 跨轨掌握度三层聚合（P3 新建）
 *
 * 纯函数，无 IO。输入只有两样：**单元清单**（Unit[]）与**作答流水摘要**。
 * 输出三层掌握度、六根轴、弱强清单。数值口径见契约 §8.3.2。
 *
 * 三层：
 *   单元级   = mastery.js 的 masteryOf 公式（一个字不改）
 *   知识点级 = 该知识点下「练过的单元」的单元掌握度算术平均
 *   轨道级   = 该轨「练过的单元」的单元掌握度算术平均
 *
 * 为什么知识点与轨道都用「单元均值」而不是「合并流水」：
 * 契约 §8.3.1 明写 c / python / sql 三轴是「单元掌握度均值」，
 * 如果轨道级用合并流水，三轨轴与三根子轴就是两套口径，横向比较说不清。
 * 旧 /api/insights 仍用合并流水口径，那是另一条链路，两者不可混用。
 *
 * 「练过」的定义：掌握度 > 0（masteryOf 对 attempts=0 返回 0，
 * 所以掌握度为 0 的单元就是没练过的单元，不进任何均值）。
 */

const {
  MASTERY_THRESHOLD,
  masteryOfUnit,
  coverageOfUnits,
  diagnose,
  diagnoseLabel,
  diagnoseAction,
} = require("./mastery.js")
const { topicKey, trackOfUnit } = require("./expected.js")
const { TRACK_CODES, labelOf } = require("../track/tracks.js")

/**
 * 单元 id 归一：unit_id 优先，其次 problem_id；没有前缀的老 id 归算法轨。
 * @param {{unit_id?: string, problem_id?: string}} row
 * @returns {string} 归一后的单元 id；取不到返回 ""
 */
function unitIdOf(row) {
  const source = row || {}
  const raw = String(source.unit_id || source.problem_id || "").trim()
  if (raw === "") return ""
  if (raw.indexOf(":") === -1) return "algo:" + raw
  return raw
}

/**
 * 取一个单元的掌握度。没练过的（拿不到值或值为 0）返回 null。
 * @param {Object<string, number>} unitMastery
 * @param {string} unitId
 * @returns {number|null}
 */
function practicedValue(unitMastery, unitId) {
  const value = unitMastery ? unitMastery[unitId] : undefined
  if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) return null
  return value
}

/**
 * 四舍五入到 9 位小数（契约 §8.3.2「精度」）。
 *
 * 均值是浮点除法，`(0.8 + 0.4) / 2` 在 IEEE 754 下是 `0.6000000000000001`——
 * 这个噪声会一路漏进 HTTP 响应与前端显示，也会让「等值断言」变成假阴性。
 * 9 位小数远超掌握度需要的精度，且仍比常见测试容差（1e-9 量级的相邻档）更细，
 * 不改变任何口径。
 *
 * @param {number} value
 * @returns {number}
 */
function round9(value) {
  const n = Number(value)
  if (!Number.isFinite(n)) return 0
  return Math.round(n * 1000000000) / 1000000000
}

/**
 * 从作答流水派生单元级统计。
 * @param {Array<{id?: number, unit_id?: string, problem_id?: string, ts: string, verdict: string}>} attempts
 * @returns {Object<string, {attempts: number, firstPass: number, lapses: number, lastAt: string|null}>}
 */
function deriveUnitStats(attempts) {
  const groups = {}
  ;(attempts || []).forEach(function (row) {
    const id = unitIdOf(row)
    if (id === "") return
    if (!groups[id]) groups[id] = []
    groups[id].push(row)
  })

  const stats = {}
  Object.keys(groups).forEach(function (id) {
    // 按时间排序；时间相同按自增 id 排序——「第一次尝试」必须是真正的第一次
    const list = groups[id].slice().sort(function (a, b) {
      const ta = String(a.ts || "")
      const tb = String(b.ts || "")
      if (ta !== tb) return ta < tb ? -1 : 1
      return Number(a.id || 0) - Number(b.id || 0)
    })
    const stat = { attempts: list.length, firstPass: 0, lapses: 0, lastAt: null }
    list.forEach(function (row, index) {
      const passed = String(row.verdict || "") === "pass"
      if (index === 0 && passed) stat.firstPass = 1
      if (!passed) stat.lapses += 1
      const ts = String(row.ts || "")
      if (ts !== "") {
        const ms = Date.parse(ts)
        if (Number.isFinite(ms) && (stat.lastAt === null || ms > Date.parse(stat.lastAt))) {
          stat.lastAt = ts
        }
      }
    })
    stats[id] = stat
  })
  return stats
}

/**
 * 单元掌握度。没练过的单元不出现在结果里。
 * @param {Object<string, object>} unitStats
 * @param {string} nowIso
 * @returns {Object<string, number>}
 */
function computeUnitMastery(unitStats, nowIso) {
  const out = {}
  Object.keys(unitStats || {}).forEach(function (id) {
    const stat = unitStats[id]
    if (!stat || Number(stat.attempts || 0) <= 0) return
    out[id] = masteryOfUnit(id, stat, nowIso)
  })
  return out
}

/**
 * 知识点行。键是复合键 <track>:<topic>。
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @param {Object<string, {expected: number, source: string}>} expectedMap 键是复合键
 * @returns {Array<object>}
 */
function topicRowsFor(units, unitMastery, expectedMap) {
  const groups = {}
  ;(units || []).forEach(function (unit) {
    if (!unit || !unit.id) return
    const track = trackOfUnit(unit)
    const topics = Array.isArray(unit.topics) ? unit.topics : []
    topics.forEach(function (topic) {
      const key = topicKey(track, topic)
      if (key === "") return
      if (!groups[key]) {
        groups[key] = { id: key, track: track, topic: String(topic).trim(), sum: 0, count: 0 }
      }
      const value = practicedValue(unitMastery, unit.id)
      if (value === null) return
      groups[key].sum += value
      groups[key].count += 1
    })
  })

  const rows = Object.keys(groups)
    .map(function (key) {
      return groups[key]
    })
    // 一个练过的单元都没有的知识点不出现在明细里：掌握度与覆盖度都是 0，
    // 列出来只是噪声（learning 仓 689 道题的知识点全摆上去就是几十行 0）。
    // 这与 P3-4「从没练过的单元不进 weak」是同一条规矩的另一面。
    .filter(function (group) {
      return group.count > 0
    })
    .map(function (group) {
      const value = round9(group.sum / group.count)
      const info = (expectedMap && expectedMap[group.id]) || { expected: 0, source: "floor" }
      const expected = Number(info.expected) || 0
      const coverage = round9(coverageOfUnits(group.count, expected))
      const bucket = diagnose(value, coverage)
      return {
        id: group.id,
        track: group.track,
        topic: group.topic,
        mastery: value,
        coverage: coverage,
        bucket: bucket,
        label: diagnoseLabel(bucket),
        action: diagnoseAction(bucket),
        unitCount: group.count,
      }
    })
  rows.sort(function (a, b) {
    if (a.mastery !== b.mastery) return a.mastery - b.mastery
    if (a.id === b.id) return 0
    return a.id < b.id ? -1 : 1
  })
  return rows
}

/**
 * 轨道行。顺序固定：algorithm → database → ai。
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @returns {Array<object>}
 */
function trackRowsFor(units, unitMastery) {
  const list = Array.isArray(units) ? units : []
  return TRACK_CODES.map(function (code) {
    const all = list.filter(function (unit) {
      return unit && trackOfUnit(unit) === code
    })
    let sum = 0
    let count = 0
    all.forEach(function (unit) {
      const value = practicedValue(unitMastery, unit.id)
      if (value === null) return
      sum += value
      count += 1
    })
    const value = count > 0 ? round9(sum / count) : 0
    const coverage = round9(coverageOfUnits(count, all.length))
    const bucket = diagnose(value, coverage)
    return {
      track: code,
      label: labelOf(code),
      mastery: value,
      coverage: coverage,
      bucket: bucket,
      label2: diagnoseLabel(bucket),
      action: diagnoseAction(bucket),
      unitCount: all.length,
    }
  })
}

/**
 * 六根轴。顺序固定：algorithm → database → ai → c → python → sql。
 * @param {object[]} trackRows trackRowsFor 的返回值
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @returns {Array<{id: string, label: string, value: number}>}
 */
function radarAxesFor(trackRows, units, unitMastery) {
  const trackValue = {}
  ;(trackRows || []).forEach(function (row) {
    trackValue[String(row.track)] = Number(row.mastery) || 0
  })

  const practiced = (Array.isArray(units) ? units : []).filter(function (unit) {
    return unit && practicedValue(unitMastery, unit.id) !== null
  })

  function meanOf(list) {
    if (!list.length) return 0
    let sum = 0
    list.forEach(function (unit) {
      sum += practicedValue(unitMastery, unit.id)
    })
    return round9(sum / list.length)
  }

  // c 轴：算法轨里 subtracks 含 engineering 或 entry.language === "cpp"
  const cUnits = practiced.filter(function (unit) {
    if (trackOfUnit(unit) !== "algorithm") return false
    const subtracks = Array.isArray(unit.subtracks) ? unit.subtracks : []
    if (subtracks.indexOf("engineering") !== -1) return true
    return String((unit.entry && unit.entry.language) || "") === "cpp"
  })

  // python 轴：entry.language === "python"
  const pythonUnits = practiced.filter(function (unit) {
    return String((unit.entry && unit.entry.language) || "") === "python"
  })

  // sql 轴：topics 里含 "SQL"（去掉空白后小写比较，命中子串即算）
  const sqlUnits = practiced.filter(function (unit) {
    const topics = Array.isArray(unit.topics) ? unit.topics : []
    return topics.some(function (topic) {
      return String(topic || "").replace(/\s+/g, "").toLowerCase().indexOf("sql") !== -1
    })
  })

  return [
    { id: "algorithm", label: "算法", value: trackValue.algorithm || 0 },
    { id: "database", label: "数据库", value: trackValue.database || 0 },
    { id: "ai", label: "AI", value: trackValue.ai || 0 },
    { id: "c", label: "C/C++", value: meanOf(cUnits) },
    { id: "python", label: "Python", value: meanOf(pythonUnits) },
    { id: "sql", label: "SQL", value: meanOf(sqlUnits) },
  ]
}

/**
 * 练过的单元清单（弱强清单的公共底座）。
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @returns {Array<{unit_id: string, track: string, title: string, mastery: number, topic: string}>}
 */
function unitRows(units, unitMastery) {
  const rows = []
  ;(Array.isArray(units) ? units : []).forEach(function (unit) {
    if (!unit || !unit.id) return
    const value = practicedValue(unitMastery, unit.id)
    if (value === null) return
    const topics = Array.isArray(unit.topics) ? unit.topics : []
    rows.push({
      unit_id: String(unit.id),
      track: trackOfUnit(unit),
      title: String(unit.title === null || unit.title === undefined ? "" : unit.title),
      mastery: value,
      topic: topics.length > 0 ? String(topics[0]) : "",
    })
  })
  return rows
}

/**
 * 弱项清单：练过且掌握度 < 0.5，掌握度升序。
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @returns {Array<object>}
 */
function weakUnitsFor(units, unitMastery) {
  return unitRows(units, unitMastery)
    .filter(function (row) {
      return row.mastery < MASTERY_THRESHOLD
    })
    .sort(function (a, b) {
      if (a.mastery !== b.mastery) return a.mastery - b.mastery
      if (a.unit_id === b.unit_id) return 0
      return a.unit_id < b.unit_id ? -1 : 1
    })
}

/**
 * 强项清单：练过且掌握度 >= 0.5，掌握度降序。
 * @param {object[]} units
 * @param {Object<string, number>} unitMastery
 * @returns {Array<object>}
 */
function strongUnitsFor(units, unitMastery) {
  return unitRows(units, unitMastery)
    .filter(function (row) {
      return row.mastery >= MASTERY_THRESHOLD
    })
    .sort(function (a, b) {
      if (a.mastery !== b.mastery) return b.mastery - a.mastery
      if (a.unit_id === b.unit_id) return 0
      return a.unit_id < b.unit_id ? -1 : 1
    })
}

/**
 * 把三层掌握度物化进 mastery 表（整体覆盖，幂等）。
 * @param {object} store 带 replaceMastery 的 store
 * @param {Array<{scope: string, scope_id: string, level: number, updated_at: string}>} rows
 * @returns {void}
 */
function materializeMastery(store, rows) {
  if (!store || typeof store.replaceMastery !== "function") {
    throw new Error("materializeMastery 需要带 replaceMastery 的 store")
  }
  store.replaceMastery(rows || [])
}

module.exports = {
  round9: round9,
  unitIdOf: unitIdOf,
  deriveUnitStats: deriveUnitStats,
  computeUnitMastery: computeUnitMastery,
  topicRowsFor: topicRowsFor,
  trackRowsFor: trackRowsFor,
  radarAxesFor: radarAxesFor,
  unitRows: unitRows,
  weakUnitsFor: weakUnitsFor,
  strongUnitsFor: strongUnitsFor,
  materializeMastery: materializeMastery,
}
