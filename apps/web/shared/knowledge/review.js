/**
 * review.js —— SM-2 间隔重复算法（纯函数模块，配单测）
 *
 * 逐字保留原 Taro 版 reviewStore 的核心公式，抽为不依赖 wx 的纯函数，
 * 便于在 Node 下单测，也便于复习页 / stores 复用。
 *
 * 参考：SuperMemo-2。quality 为 0-5 的回忆质量评分。
 */

const DEFAULT_EASE = 2.5

/**
 * 构造一条复习记录，补齐 SM-2 所需字段（不被 input 覆盖）。
 * @param {{id?:string,question:string,answer:string,domain:string,
 *   easiness?:number,interval?:number,repetitions?:number,
 *   nextReviewDate?:string,lastReviewDate?:string,createdAt?:string}} input
 */
function createReview (input) {
  const nowIso = new Date().toISOString()
  return {
    id: input.id,
    question: input.question,
    answer: input.answer,
    domain: input.domain,
    easiness: input.easiness != null ? input.easiness : DEFAULT_EASE,
    interval: input.interval != null ? input.interval : 1,
    repetitions: input.repetitions != null ? input.repetitions : 0,
    nextReviewDate: input.nextReviewDate || nowIso,
    lastReviewDate: input.lastReviewDate || '',
    createdAt: input.createdAt || nowIso
  }
}

/**
 * SM-2 核心：根据本次回忆质量计算下次复习参数。
 * @param {{easiness:number,interval:number,repetitions:number,...}} record
 * @param {number} quality 0-5
 */
function calculateNextReview (record, quality) {
  let { easiness, interval, repetitions } = record

  // 更新难度因子（记忆容易度），下限 1.3
  easiness = Math.max(
    1.3,
    easiness + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02))
  )

  if (quality < 3) {
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

  // 计算下次复习日期
  const nextDate = new Date()
  nextDate.setDate(nextDate.getDate() + interval)

  return Object.assign({}, record, {
    easiness: easiness,
    interval: interval,
    repetitions: repetitions,
    nextReviewDate: nextDate.toISOString(),
    lastReviewDate: new Date().toISOString()
  })
}

/** 是否到期（nextReviewDate <= now） */
function isDue (record, nowIso) {
  const now = nowIso ? new Date(nowIso).getTime() : Date.now()
  return new Date(record.nextReviewDate).getTime() <= now
}

/** 过滤出到期记录 */
function filterDue (records, nowIso) {
  return (records || []).filter(function (r) { return isDue(r, nowIso) })
}

module.exports = {
  DEFAULT_EASE: DEFAULT_EASE,
  createReview: createReview,
  calculateNextReview: calculateNextReview,
  isDue: isDue,
  filterDue: filterDue
}
