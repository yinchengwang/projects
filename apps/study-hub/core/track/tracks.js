"use strict"
/**
 * tracks.js —— 三条顶层学习轨的定义
 *
 * 这是「轨」的唯一定义处：调度器、掌握度、诊断、供给流水线都从这里取。
 * 权重与开关的运行期覆盖走 settings 表的 track.weight.<code> / track.enabled.<code>。
 *
 * 注意与 core/bank/problem.js 的 TRACKS 的区别：
 *   problem.js 的 TRACKS 是算法轨内部的 4 个子轨（fundamentals/interview/advanced/engineering）；
 *   这里的 TRACKS 是三条**顶层**轨（algorithm/database/ai）。
 */

/** 三条顶层轨。subtracks 只对算法轨有意义，其余轨恒为 []。 */
const TRACKS = [
  {
    code: "algorithm",
    label: "算法",
    idPrefix: "algo",
    subtracks: ["fundamentals", "interview", "advanced", "engineering"],
    weight: 0.4,
    enabled: true,
  },
  {
    code: "database",
    label: "数据库",
    idPrefix: "db",
    subtracks: [],
    weight: 0.3,
    enabled: true,
  },
  {
    code: "ai",
    label: "AI",
    idPrefix: "ai",
    subtracks: [],
    weight: 0.3,
    enabled: true,
  },
]

const TRACK_CODES = ["algorithm", "database", "ai"]

/** grokking 数据库卡目录名（实测清单，冻结） */
const GROKKING_DB_CARDS = [
  "anti_patterns",
  "backup_recovery",
  "cache_strategy",
  "cap_theorem",
  "clustering",
  "index_design",
  "lock_manager",
  "mvcc",
  "newsql",
  "normalization",
  "nosql",
  "query_optimization",
  "replication",
  "sharding",
  "sql_fundamentals",
  "transaction",
]

/** grokking AI 卡目录名（实测清单，冻结） */
const GROKKING_AI_CARDS = [
  "cnn",
  "decision_tree",
  "dimensionality_reduction",
  "gradient_boosting",
  "linear_regression",
  "logistic_regression",
  "ml_fundamentals",
  "neural_network",
  "nlp",
  "random_forest",
  "recommendation",
  "rnn",
  "svm",
]

/**
 * 按 code 取轨定义。
 * @param {string} code
 * @returns {object|null}
 */
function trackOf(code) {
  for (const t of TRACKS) {
    if (t.code === code) return t
  }
  return null
}

/**
 * 按 id 前缀取轨定义（algo / db / ai）。
 * @param {string} prefix
 * @returns {object|null}
 */
function trackOfIdPrefix(prefix) {
  for (const t of TRACKS) {
    if (t.idPrefix === prefix) return t
  }
  return null
}

/**
 * 轨的中文标签；未知 code 返回「未知轨道」。
 * @param {string} code
 * @returns {string}
 */
function labelOf(code) {
  const t = trackOf(code)
  return t ? t.label : "未知轨道"
}

/**
 * grokking 卡的轨道归属。
 * @param {string} stem 卡目录名
 * @returns {"database"|"ai"|null} 不属本轮三轨范围时返回 null
 */
function trackOfGrokkingCard(stem) {
  const name = String(stem === undefined || stem === null ? "" : stem)
  if (GROKKING_DB_CARDS.indexOf(name) !== -1) return "database"
  if (GROKKING_AI_CARDS.indexOf(name) !== -1) return "ai"
  return null
}

module.exports = {
  TRACKS: TRACKS,
  TRACK_CODES: TRACK_CODES,
  GROKKING_DB_CARDS: GROKKING_DB_CARDS,
  GROKKING_AI_CARDS: GROKKING_AI_CARDS,
  trackOf: trackOf,
  trackOfIdPrefix: trackOfIdPrefix,
  labelOf: labelOf,
  trackOfGrokkingCard: trackOfGrokkingCard,
}
