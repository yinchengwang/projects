"use strict"
/**
 * insights.js —— 诊断数据装配
 *
 * 数据来源（口径见 M5 计划「统计口径」一节）：
 *   - 知识点统计：attempts 表 + 题库 → 派生 → 幂等覆盖 topics 表 → 从 topics 表读
 *   - 覆盖度分母：core/mastery/expected.js 的期望题量口径
 *   - 每日题量曲线：attempts 表按 substr(ts,1,10) 分组
 *
 * 这一层只做装配，不含 HTTP 细节。
 */

const {
  MASTERY_THRESHOLD,
  diagnoseAction,
  rankTopics,
  sortByUrgency,
} = require("../core/mastery/mastery.js")
const { bankTopicCounts, buildExpectedMap, minExpectedFrom } = require("../core/mastery/expected.js")
const { deriveTopicStats } = require("../core/mastery/topic-stats.js")

/** 曲线的默认天数窗口 */
const DEFAULT_DAYS = 30

/** 曲线的最大天数窗口 */
const MAX_DAYS = 365

/**
 * 日期加减（UTC）。格式不对就原样返回。
 * @param {string} dateStr "YYYY-MM-DD"
 * @param {number} delta 天数偏移，可负
 * @returns {string}
 */
function addDays(dateStr, delta) {
  const parts = String(dateStr).split("-")
  const y = Number(parts[0])
  const m = Number(parts[1])
  const d = Number(parts[2])
  if (!Number.isFinite(y) || !Number.isFinite(m) || !Number.isFinite(d)) return String(dateStr)
  const ms = Date.UTC(y, m - 1, d) + delta * 86400000
  const dt = new Date(ms)
  const yy = dt.getUTCFullYear()
  const mm = String(dt.getUTCMonth() + 1).padStart(2, "0")
  const dd = String(dt.getUTCDate()).padStart(2, "0")
  return yy + "-" + mm + "-" + dd
}

/**
 * 把天数窗口夹到合法范围。非法值退回默认 30；0 表示「全部」。
 * @param {number|string|undefined} value
 * @returns {number}
 */
function normalizeDays(value) {
  if (value === undefined || value === null || value === "") return DEFAULT_DAYS
  const n = Number(value)
  if (!Number.isFinite(n) || n < 0) return DEFAULT_DAYS
  return Math.min(MAX_DAYS, Math.floor(n))
}

/**
 * 每日题量曲线：按天分组，窗口内没有数据的日期补 0。
 * @param {Array<{date: string, count: number}>} rows
 * @param {string} endDate 窗口最后一天（YYYY-MM-DD）
 * @param {number} days 窗口天数；0 表示全部（不补 0）
 * @returns {Array<{date: string, count: number}>}
 */
function buildSeries(rows, endDate, days) {
  const countMap = {}
  ;(rows || []).forEach(function (row) {
    countMap[String(row.date)] = Number(row.count || 0)
  })
  const dates = Object.keys(countMap).sort()
  const all = dates.map(function (date) {
    return { date: date, count: countMap[date] }
  })
  if (!days || days <= 0) return all
  if (!/^\d{4}-\d{2}-\d{2}$/.test(String(endDate || ""))) return all

  const out = []
  let cursor = addDays(String(endDate), -(days - 1))
  for (let i = 0; i < days; i += 1) {
    out.push({ date: cursor, count: countMap[cursor] || 0 })
    const next = addDays(cursor, 1)
    if (next === cursor) break
    cursor = next
  }
  return out
}

/**
 * 四舍五入到 4 位小数。
 */
function round4(value) {
  return Math.round(Number(value) * 10000) / 10000
}

/**
 * 汇总概览数字。
 *
 * totalAttempts / totalProblems 都从原始流水算，不能按知识点加总——
 * 一道题挂多个知识点时，按知识点加总会把同一次提交数两遍。
 *
 * @param {{topics: object[], stats: object[], series: object[], bankCounts: object,
 *          settings: object, attemptTotal: number, problemTotal: number}} input
 * @returns {object}
 */
function buildSummary(input) {
  const data = input || {}
  const topics = Array.isArray(data.topics) ? data.topics : []
  const stats = Array.isArray(data.stats) ? data.stats : []
  const series = Array.isArray(data.series) ? data.series : []
  const bankCounts = data.bankCounts || {}
  const settings = data.settings || {}

  let masterySum = 0
  let coverageSum = 0
  let weakCount = 0
  let strongCount = 0
  topics.forEach(function (item) {
    masterySum += item.mastery
    coverageSum += item.coverage
    // 强弱只看掌握度（覆盖度是另一维，四档诊断才把两者交叉）
    if (item.mastery < MASTERY_THRESHOLD) weakCount += 1
    else strongCount += 1
  })

  let lastPracticeAt = null
  stats.forEach(function (stat) {
    if (stat.lastAt && (lastPracticeAt === null || Date.parse(stat.lastAt) > Date.parse(lastPracticeAt))) {
      lastPracticeAt = stat.lastAt
    }
  })

  let windowAttempts = 0
  series.forEach(function (row) {
    windowAttempts += Number(row.count || 0)
  })

  const practiced = {}
  topics.forEach(function (item) {
    practiced[item.topic] = true
  })
  let untouchedTopics = 0
  Object.keys(bankCounts).forEach(function (topic) {
    if (!practiced[topic]) untouchedTopics += 1
  })

  const count = topics.length
  return {
    topicCount: count,
    untouchedTopics: untouchedTopics,
    weakCount: weakCount,
    strongCount: strongCount,
    avgMastery: count ? round4(masterySum / count) : 0,
    avgCoverage: count ? round4(coverageSum / count) : 0,
    totalAttempts: Number(data.attemptTotal) || 0,
    totalProblems: Number(data.problemTotal) || 0,
    lastPracticeAt: lastPracticeAt,
    minExpectedQuestions: minExpectedFrom(settings),
    windowDays: series.length,
    windowAttempts: windowAttempts,
  }
}

/**
 * 装配诊断数据。
 * @param {{store: object, provider: object, nowIso?: string, days?: number}} ctx
 * @returns {Promise<{topics: object[], summary: object, series: object[]}>}
 */
async function buildInsights(ctx) {
  const context = ctx || {}
  const nowIso = context.nowIso || new Date().toISOString()
  const days = normalizeDays(context.days)

  const problems = context.provider && context.provider.list ? await context.provider.list() : []
  const attempts = context.store.listAttemptSummaries()
  const derived = deriveTopicStats(attempts, problems)

  // topics 表是物化视图：整体覆盖，幂等
  context.store.replaceTopics(
    derived.map(function (row) {
      return {
        id: row.topic,
        first_pass: row.firstPass,
        attempts: row.attempts,
        last_at: row.lastAt,
        lapses: row.lapses,
      }
    })
  )

  // 掌握度只从 topics 表读
  const topicRows = context.store.listTopics()
  const problemsOfTopic = {}
  derived.forEach(function (row) {
    problemsOfTopic[row.topic] = row.problems
  })

  const stats = topicRows.map(function (row) {
    return {
      topic: row.id,
      attempts: row.attempts,
      firstPass: row.first_pass,
      lapses: row.lapses,
      lastAt: row.last_at,
      problems: problemsOfTopic[row.id] === undefined ? 0 : problemsOfTopic[row.id],
    }
  })

  const settings = context.store.allSettings()
  const bankCounts = bankTopicCounts(problems)
  const expectedMap = buildExpectedMap(
    stats.map(function (stat) {
      return stat.topic
    }),
    bankCounts,
    settings
  )
  const expectedCounts = {}
  Object.keys(expectedMap).forEach(function (topic) {
    expectedCounts[topic] = expectedMap[topic].expected
  })

  // rankTopics 给掌握度升序；sortByUrgency 改成「该先治哪个」的顺序
  const ranked = sortByUrgency(rankTopics(stats, nowIso, expectedCounts))

  const statOf = {}
  stats.forEach(function (stat) {
    statOf[stat.topic] = stat
  })

  const topics = ranked.map(function (item) {
    const stat = statOf[item.topic]
    const expected = expectedMap[item.topic]
    return {
      topic: item.topic,
      mastery: item.mastery,
      coverage: item.coverage,
      bucket: item.bucket,
      label: item.label,
      action: diagnoseAction(item.bucket),
      attempts: stat.attempts,
      firstPass: stat.firstPass,
      lapses: stat.lapses,
      lastAt: stat.lastAt,
      expected: expected.expected,
      expectedSource: expected.source,
      problems: stat.problems,
    }
  })

  const endDate = String(nowIso).slice(0, 10)
  const series = buildSeries(context.store.countAttemptsByDay(), endDate, days)

  // 总提交数与总练过题数从原始流水算——按知识点加总会把一道题数两遍
  const distinctProblems = {}
  attempts.forEach(function (row) {
    distinctProblems[String(row.problem_id)] = true
  })

  const summary = buildSummary({
    topics: topics,
    stats: stats,
    series: series,
    bankCounts: bankCounts,
    settings: settings,
    attemptTotal: attempts.length,
    problemTotal: Object.keys(distinctProblems).length,
  })

  return { topics: topics, summary: summary, series: series }
}

module.exports = {
  DEFAULT_DAYS: DEFAULT_DAYS,
  MAX_DAYS: MAX_DAYS,
  addDays: addDays,
  normalizeDays: normalizeDays,
  buildSeries: buildSeries,
  buildSummary: buildSummary,
  buildInsights: buildInsights,
}
