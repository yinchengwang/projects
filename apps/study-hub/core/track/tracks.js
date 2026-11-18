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

// 由 TRACKS 派生：加了新轨却忘了改这份清单，是最容易犯的漂移（G41）。
const TRACK_CODES = TRACKS.map(function (t) {
  return t.code
})

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
/**
 * grokking 卡的清单（接口契约 §14.1 冻结）。
 * database 16 张 + ai 13 张 = 29 张，是本轮三轨的卡片范围。
 */
const GROKKING_CARDS = {
  database: [
    "anti_patterns", "backup_recovery", "cache_strategy", "cap_theorem",
    "clustering", "index_design", "lock_manager", "mvcc", "newsql",
    "normalization", "nosql", "query_optimization", "replication",
    "sharding", "sql_fundamentals", "transaction",
  ],
  ai: [
    "cnn", "decision_tree", "dimensionality_reduction", "gradient_boosting",
    "linear_regression", "logistic_regression", "ml_fundamentals",
    "neural_network", "nlp", "random_forest", "recommendation", "rnn", "svm",
  ],
}

/**
 * 依赖 numpy 的卡：clustering + 全部 13 张 AI 卡（契约 §14.1）。
 * 缺 numpy 时这些卡读得懂、跑不动。
 */
const NUMPY_CARDS = [
  "clustering", "cnn", "decision_tree", "dimensionality_reduction",
  "gradient_boosting", "linear_regression", "logistic_regression",
  "ml_fundamentals", "neural_network", "nlp", "random_forest",
  "recommendation", "rnn", "svm",
]

/**
 * 这张卡要不要 numpy。
 * @param {string} stem
 * @returns {boolean}
 */
function cardNeedsNumpy(stem) {
  return NUMPY_CARDS.indexOf(String(stem === null || stem === undefined ? "" : stem)) !== -1
}

function trackOfGrokkingCard(stem) {
  const s = String(stem === null || stem === undefined ? "" : stem)
  if (GROKKING_CARDS.database.indexOf(s) !== -1) return "database"
  if (GROKKING_CARDS.ai.indexOf(s) !== -1) return "ai"
  return null
}

module.exports = {
  TRACKS: TRACKS,
  TRACK_CODES: TRACK_CODES,
  GROKKING_DB_CARDS: GROKKING_DB_CARDS,
  GROKKING_CARDS: GROKKING_CARDS,
  NUMPY_CARDS: NUMPY_CARDS,
  cardNeedsNumpy: cardNeedsNumpy,
  GROKKING_AI_CARDS: GROKKING_AI_CARDS,
  trackOf: trackOf,
  trackOfIdPrefix: trackOfIdPrefix,
  labelOf: labelOf,
  trackOfGrokkingCard: trackOfGrokkingCard,
}
