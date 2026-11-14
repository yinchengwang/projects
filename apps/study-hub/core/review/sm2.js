"use strict"
/**
 * sm2.js —— SM-2 间隔重复（公式逐字搬自 projects/apps/knowledge-hub/core/review.js）
 *
 * 公式来源：knowledge-hub/core/review.js 的 calculateNextReview(record, quality)。
 * 逐字搬运的部分（G23）：
 *   easiness = max(1.3, easiness + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02)))
 *   quality < 3        → repetitions = 0, interval = 1
 *   quality >= 3 且 repetitions === 0 → interval = 1
 *   quality >= 3 且 repetitions === 1 → interval = 6
 *   否则                → interval = round(interval * easiness)
 *
 * 唯一的一处刻意改动：原版用 `nextDate.setDate(nextDate.getDate() + interval)`
 * （本地日历日），本模块用 `setUTCDate`（UTC 日历日）——日历日语义相同，
 * 但不随机器时区/夏令时漂移，测试可断言确定的日期。
 *
 * 卡片字段用 M1 的 review_cards 表形状（snake_case），因为它要直接进 SQLite。
 */

const DEFAULT_EASE = 2.5
const MIN_EASE = 1.3
const QUALITY_MIN = 0
const QUALITY_MAX = 5

/**
 * 把卡片补成 SM-2 需要的完整形状。
 * @param {object} card
 * @returns {object}
 */
function normalizeCard(card) {
  const src = card || {}
  return {
    problem_id: String(src.problem_id || ""),
    easiness:
      src.easiness === null || src.easiness === undefined ? DEFAULT_EASE : Number(src.easiness),
    interval_days:
      src.interval_days === null || src.interval_days === undefined ? 1 : Number(src.interval_days),
    repetitions:
      src.repetitions === null || src.repetitions === undefined ? 0 : Number(src.repetitions),
    last_quality:
      src.last_quality === null || src.last_quality === undefined ? null : Number(src.last_quality),
    last_at: src.last_at === null || src.last_at === undefined ? null : String(src.last_at),
    next_review_at: src.next_review_at ? String(src.next_review_at) : new Date().toISOString(),
    lapses: src.lapses === null || src.lapses === undefined ? 0 : Number(src.lapses),
  }
}

/**
 * SM-2 核心：按回忆质量算下一次的间隔与难度因子。
 * @param {object} card 卡片（snake_case）
 * @param {number} quality 0~5 的整数
 * @param {string} nowIso 本次复习时间（ISO）；不传用当前时间
 * @returns {object} 新卡片（snake_case）
 */
function grade(card, quality, nowIso) {
  const q = Number(quality)
  if (!Number.isInteger(q) || q < QUALITY_MIN || q > QUALITY_MAX) {
    throw new Error("评分 quality 必须是 0~5 的整数，收到：" + String(quality))
  }
  const now = nowIso ? new Date(nowIso).toISOString() : new Date().toISOString()
  const current = normalizeCard(card)

  // 更新难度因子（记忆容易度），下限 1.3
  let easiness = Math.max(
    MIN_EASE,
    current.easiness + (0.1 - (5 - q) * (0.08 + (5 - q) * 0.02))
  )

  let interval = current.interval_days
  let repetitions = current.repetitions

  if (q < 3) {
    // 质量低于 3，判定未记住：重置间隔与重复计数
    repetitions = 0
    interval = 1
  } else {
    // 质量 >= 3，增加间隔
    if (repetitions === 0) {
      interval = 1
    } else if (repetitions === 1) {
      interval = 6
    } else {
      interval = Math.round(interval * easiness)
    }
    repetitions += 1
  }

  // 下次复习日期：日历日 + interval 天（UTC 日历日，避免时区漂移）
  const nextDate = new Date(now)
  nextDate.setUTCDate(nextDate.getUTCDate() + interval)

  return {
    problem_id: current.problem_id,
    easiness: easiness,
    interval_days: interval,
    repetitions: repetitions,
    last_quality: q,
    last_at: now,
    next_review_at: nextDate.toISOString(),
    lapses: current.lapses + (q < 3 ? 1 : 0),
  }
}

/**
 * 判题结果 → SM-2 quality（设计文档 §4.2 的表格）。
 * @param {{passed?:boolean, durationMs?:number, medianMs?:number|null,
 *          usedOldSolution?:boolean, copiedReference?:boolean, failedFirst?:boolean}} outcome
 * @returns {number} 0~5
 */
function qualityFromOutcome(outcome) {
  const o = outcome || {}
  if (o.copiedReference === true) return 0
  // 「失败过一次、后来自己改对」是 2 档：答对了，但记忆还不牢。
  // 这条判据必须排在 passed 之前——failedFirst 的场合 passed 通常也是 true，
  // 若先判 passed 就会掉到 5 档，把「折腾过」和「一次过」混为一谈。
  if (o.failedFirst === true) return o.passed === true ? 2 : 1
  if (o.passed !== true) return 1
  if (o.usedOldSolution === true) return 3
  if (
    typeof o.durationMs === "number" &&
    typeof o.medianMs === "number" &&
    o.durationMs > o.medianMs
  ) {
    return 4
  }
  return 5
}

/**
 * 是否到期（next_review_at <= now）。
 * @param {object} card
 * @param {string} nowIso
 * @returns {boolean}
 */
function isDue(card, nowIso) {
  const current = normalizeCard(card)
  const now = nowIso ? new Date(nowIso).getTime() : Date.now()
  return new Date(current.next_review_at).getTime() <= now
}

/**
 * 逾期毫秒数（越大越久没复习）。
 * @param {object} card
 * @param {string} nowIso
 * @returns {number}
 */
function overdueMs(card, nowIso) {
  const current = normalizeCard(card)
  const now = nowIso ? new Date(nowIso).getTime() : Date.now()
  return now - new Date(current.next_review_at).getTime()
}

/**
 * 按逾期时长倒序排到期卡片（逾期最久的排最前）。不改动原数组。
 * @param {object[]} cards
 * @param {string} nowIso
 * @returns {object[]}
 */
function sortByOverdue(cards, nowIso) {
  return (cards || []).slice().sort(function (a, b) {
    const diff = overdueMs(b, nowIso) - overdueMs(a, nowIso)
    if (diff !== 0) return diff
    return String(a.problem_id).localeCompare(String(b.problem_id))
  })
}

module.exports = {
  DEFAULT_EASE: DEFAULT_EASE,
  MIN_EASE: MIN_EASE,
  grade: grade,
  qualityFromOutcome: qualityFromOutcome,
  isDue: isDue,
  sortByOverdue: sortByOverdue,
}
