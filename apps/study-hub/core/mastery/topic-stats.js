"use strict"
/**
 * topic-stats.js —— 从作答流水派生每个知识点的统计量
 *
 * 纯函数，无 IO。口径对应设计文档 §4.3 的「首次通过率 = 首次尝试就过的比例」：
 *   attempts   = 该知识点下所有题目的提交总次数
 *   firstPass  = 该知识点下「第一次尝试就通过」的题目数（反复重试通过不算）
 *   lapses     = 该知识点下没通过的提交次数（失误次数）
 *   lastAt     = 该知识点下最后一次提交的时间
 *   problems   = 该知识点下练过的不同题目数（覆盖度的分子）
 *
 * 题库里查不到知识点的题目不计入——没有知识点归属就没法诊断。
 */

/** 一次作答视为「通过」的判定 */
function isPass(verdict) {
  return String(verdict || "") === "pass"
}

/**
 * 派生知识点统计。
 * @param {Array<{id: number, problem_id: string, ts: string, verdict: string}>} attempts
 * @param {Array<{id: string, topics: string[]}>} problems
 * @returns {Array<{topic: string, attempts: number, firstPass: number, lapses: number, lastAt: string|null, problems: number}>}
 */
function deriveTopicStats(attempts, problems) {
  // 题目 → 知识点列表
  const topicsOfProblem = {}
  ;(problems || []).forEach(function (problem) {
    topicsOfProblem[String(problem.id)] = (problem.topics || []).map(String).filter(function (t) {
      return t !== ""
    })
  })

  // 题目 → 该题的历次作答
  const attemptsOfProblem = {}
  ;(attempts || []).forEach(function (attempt) {
    const pid = String(attempt.problem_id)
    if (!attemptsOfProblem[pid]) attemptsOfProblem[pid] = []
    attemptsOfProblem[pid].push(attempt)
  })

  const stats = {}

  Object.keys(attemptsOfProblem).forEach(function (pid) {
    const topics = topicsOfProblem[pid]
    if (!topics || topics.length === 0) return

    // 按时间排序；时间相同按自增 id 排序——「第一次尝试」必须是真正的第一次
    const list = attemptsOfProblem[pid].slice().sort(function (a, b) {
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

    topics.forEach(function (topic) {
      if (!stats[topic]) {
        stats[topic] = {
          topic: topic,
          attempts: 0,
          firstPass: 0,
          lapses: 0,
          lastAt: null,
          problems: 0,
        }
      }
      const row = stats[topic]
      row.attempts += list.length
      row.problems += 1
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
      if (a.topic === b.topic) return 0
      return a.topic < b.topic ? -1 : 1
    })
}

module.exports = { deriveTopicStats: deriveTopicStats }
