"use strict"
/**
 * core/scheduler/next-action.js —— 推下一题
 *
 * 候选动作的优先级（设计文档 §4.1）：
 *   1. 到期复习：next_review_at <= now 的卡片，逾期越久越靠前
 *   2. 轮次推进：多个 active 轮次优先最近创建的；轮次内掌握度最低的 topic 优先，
 *                同一 topic 内按清单里的题号顺序
 *   3. 主动探索：没有 active 轮次时的兜底，挑没做过的题
 *
 * 单日配额「10 题 **或** 20 分钟，取先到者」：任一顶到就不再推复习。
 * 配额上限在 settings 表（review.daily_quota_questions / review.daily_quota_minutes），
 * 用量记在同表的日期作用域键上（review.used.questions.<日期> / review.used.minutes.<日期>）。
 *
 * 纯本地模式（G26）可降级：没连上 learning 仓时 rounds 为空，走复习/探索分支，
 * 本模块自身不碰文件系统（除轮次文件的修改时间兜底），不抛错。
 */

const fs = require("node:fs")
const { sortByOverdue } = require("../review/sm2.js")

/** 各难度的基准耗时（分钟） */
const BASE_MINUTES = { easy: 10, medium: 15, hard: 25 }

/** 单日配额默认值 */
const DEFAULT_QUOTA_QUESTIONS = 10
const DEFAULT_QUOTA_MINUTES = 20

/** 配额用量键前缀（后面拼本地日期） */
const KEY_USED_QUESTIONS = "review.used.questions."
const KEY_USED_MINUTES = "review.used.minutes."

/** 配额上限的设置键 */
const KEY_LIMIT_QUESTIONS = "review.daily_quota_questions"
const KEY_LIMIT_MINUTES = "review.daily_quota_minutes"

/**
 * 本地日期键 YYYY-MM-DD（配额按本地日计）。
 * @param {string} nowIso
 * @returns {string}
 */
function dayKey(nowIso) {
  const d = new Date(nowIso)
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return d.getFullYear() + "-" + m + "-" + day
}

/**
 * 读一个非负数字设置：空值/非法值退回默认值，0 是合法值（配额上限调成 0 意为「今天不复习」）。
 * @param {*} raw
 * @param {number} fallback
 * @returns {number}
 */
function readLimit(raw, fallback) {
  if (raw === null || raw === undefined || raw === "") return fallback
  const n = Number(raw)
  if (!Number.isFinite(n) || n < 0) return fallback
  return n
}

/**
 * 读已用配额：非法值当 0。
 */
function readUsed(store, key) {
  if (!store || typeof store.getSetting !== "function") return 0
  const n = Number(store.getSetting(key, "0"))
  if (!Number.isFinite(n) || n <= 0) return 0
  return n
}

/**
 * 读今天的配额状态。
 * @param {object} store M1 的 store
 * @param {string} nowIso
 * @param {object} settings settings 表的全量键值（不传就自己读 store）
 * @returns {{date: string, questions: number, minutes: number,
 *            limitQuestions: number, limitMinutes: number,
 *            exhausted: boolean, exhaustedBy: string|null}}
 */
function quotaState(store, nowIso, settings) {
  let conf = settings
  if (!conf) {
    conf =
      store && typeof store.allSettings === "function" ? store.allSettings() : {}
  }
  const date = dayKey(nowIso)
  const limitQuestions = readLimit(conf[KEY_LIMIT_QUESTIONS], DEFAULT_QUOTA_QUESTIONS)
  const limitMinutes = readLimit(conf[KEY_LIMIT_MINUTES], DEFAULT_QUOTA_MINUTES)
  const questions = readUsed(store, KEY_USED_QUESTIONS + date)
  const minutes = readUsed(store, KEY_USED_MINUTES + date)
  // G24：题数与分钟「取先到者」，任一顶到即视为配额用完
  const byQuestions = questions >= limitQuestions
  const byMinutes = minutes >= limitMinutes
  return {
    date: date,
    questions: questions,
    minutes: minutes,
    limitQuestions: limitQuestions,
    limitMinutes: limitMinutes,
    exhausted: byQuestions || byMinutes,
    exhaustedBy: byQuestions ? "questions" : byMinutes ? "minutes" : null,
  }
}

/**
 * 记一次配额消耗。
 * @param {object} store M1 的 store
 * @param {{questions?: number, minutes?: number}} usage 本次用量
 * @param {string} nowIso
 * @returns {{questions: number, minutes: number}} 记完之后的当日累计用量
 */
function consumeQuota(store, usage, nowIso) {
  if (!store || typeof store.setSetting !== "function") {
    throw new Error("缺少 store，无法记账单日配额")
  }
  const date = dayKey(nowIso)
  const add = usage || {}
  const questions =
    readUsed(store, KEY_USED_QUESTIONS + date) + Number(add.questions || 0)
  const minutes = readUsed(store, KEY_USED_MINUTES + date) + Number(add.minutes || 0)
  store.setSetting(KEY_USED_QUESTIONS + date, String(questions))
  store.setSetting(KEY_USED_MINUTES + date, String(minutes))
  return { questions: questions, minutes: minutes }
}

/**
 * 估算这一步要花多少分钟：有该难度的历史平均用时就用它，和难度基准取较大者。
 * @param {object|null} problem
 * @param {object} avgByDifficulty 难度 → 历史平均用时（毫秒）
 * @returns {number}
 */
function estimateMinutes(problem, avgByDifficulty) {
  const difficulty = (problem && problem.difficulty) || "medium"
  const base = BASE_MINUTES[difficulty] || BASE_MINUTES.medium
  const avg = avgByDifficulty ? avgByDifficulty[difficulty] : null
  if (avg === null || avg === undefined || Number.isNaN(Number(avg))) return base
  const historical = Math.max(1, Math.round(Number(avg) / 60000))
  return Math.max(base, historical)
}

/**
 * 按题目把尝试记录分组（rows 按时间升序，第一条就是首次尝试）。
 * @param {object[]} attempts
 * @returns {Object<string, object[]>}
 */
function groupAttemptsByProblem(attempts) {
  const out = Object.create(null)
  ;(attempts || []).forEach(function (row) {
    if (!row) return
    const id = String(row.problem_id)
    if (!out[id]) out[id] = []
    out[id].push(row)
  })
  return out
}

/**
 * 各难度的历史平均用时（毫秒）。
 * @param {object[]} problems
 * @param {Object<string, object[]>} attemptsByProblem
 * @returns {Object<string, number>}
 */
function averageMsByDifficulty(problems, attemptsByProblem) {
  const sums = Object.create(null)
  const counts = Object.create(null)
  ;(problems || []).forEach(function (p) {
    if (!p) return
    const rows = attemptsByProblem[String(p.id)] || []
    rows.forEach(function (row) {
      const ms = Number(row.duration_ms || 0)
      if (!ms) return
      const d = p.difficulty || "medium"
      sums[d] = (sums[d] || 0) + ms
      counts[d] = (counts[d] || 0) + 1
    })
  })
  const out = Object.create(null)
  Object.keys(sums).forEach(function (d) {
    out[d] = sums[d] / counts[d]
  })
  return out
}

/**
 * 主题掌握度近似值。
 *
 *   首次通过率 = 首次尝试就过的比例
 *   掌握度     = 0.7 × 首次通过率 + 0.3 × e^(-失误次数/2)
 *
 * M4 先不含遗忘因子（0.5 ^ (距上次练习天数/14)）——那需要每个 topic 的上次练习时间，
 * M5 落地后整体换成 core/mastery/mastery.js 的 masteryOf()，调用方接口不变。
 * 一个题都没练过的 topic 给 0.3（只剩 e^0 那一项），也就是最该补的短板。
 *
 * 返回值用无原型对象：topic 名是用户数据，可能叫 "constructor"、"toString"，
 * 用普通对象会被 Object.prototype 上的同名键污染。
 *
 * @param {object[]} problems
 * @param {Object<string, object[]>} attemptsByProblem
 * @returns {Object<string, number>}
 */
function masteryByTopic(problems, attemptsByProblem) {
  const topics = Object.create(null)
  ;(problems || []).forEach(function (p) {
    if (!p) return
    ;(p.topics || []).forEach(function (t) {
      if (!topics[t]) topics[t] = { ids: [], firstPass: 0, firstTotal: 0, lapses: 0 }
      topics[t].ids.push(String(p.id))
    })
  })

  const out = Object.create(null)
  Object.keys(topics).forEach(function (t) {
    const stat = topics[t]
    stat.ids.forEach(function (id) {
      const rows = (attemptsByProblem && attemptsByProblem[id]) || []
      if (rows.length === 0) return
      const first = rows[0]
      stat.firstTotal += 1
      if (String(first.verdict) === "pass") stat.firstPass += 1
      else stat.lapses += 1
    })
    if (stat.firstTotal === 0) {
      out[t] = 0.3
    } else {
      const rate = stat.firstPass / stat.firstTotal
      out[t] = 0.7 * rate + 0.3 * Math.exp(-stat.lapses / 2)
    }
  })
  return out
}

/**
 * 题库索引：按 ID 与标题各建一张表。
 * @param {object[]} problems
 * @returns {{byId: Map<string,object>, byTitle: Map<string,object>}}
 */
function buildProblemIndex(problems) {
  const byId = new Map()
  const byTitle = new Map()
  ;(problems || []).forEach(function (p) {
    if (!p) return
    if (p.id) byId.set(String(p.id), p)
    if (p.title) byTitle.set(String(p.title), p)
  })
  return { byId: byId, byTitle: byTitle }
}

/**
 * 轮次条目 → 题库题目：先按 ID 精确匹配，再按标题精确匹配；都匹配不上返回 null。
 * @param {object} item
 * @param {{byId: Map<string,object>, byTitle: Map<string,object>}} index
 * @returns {object|null}
 */
function matchProblem(item, index) {
  if (!item) return null
  if (index.byId.has(String(item.id))) return index.byId.get(String(item.id))
  if (item.title && index.byTitle.has(String(item.title))) return index.byTitle.get(String(item.title))
  return null
}

/**
 * 条目所属的 topic：优先取匹配到的题目的第一个 topic；匹配不上就用条目标题当临时 topic。
 * @param {object} item
 * @param {{byId: Map<string,object>, byTitle: Map<string,object>}} index
 * @returns {string}
 */
function topicOf(item, index) {
  const problem = matchProblem(item, index)
  if (problem && problem.topics && problem.topics.length > 0) return problem.topics[0]
  return String((item && item.title) || (item && item.id) || "")
}

/**
 * 轮次的「最近创建」排序键：开始时间优先，没有就用文件修改时间。
 * @param {object} round
 * @returns {string}
 */
function roundCreatedKey(round) {
  if (round && round.startedAt) return String(round.startedAt)
  try {
    return fs.statSync(round.file).mtime.toISOString()
  } catch (e) {
    return ""
  }
}

/**
 * 候选轮次：手动指定 > 最近创建的 active 轮次（按新到旧排）。
 *
 * 返回**列表**而不是单个——调用方要逐个试，因为一个轮次里可能全是
 * 「题库里根本没有」的卡片（Python 工具链、设计模式……），那种轮次推给
 * 用户也点不开题面，不该占掉「下一个动作」。
 * @param {object[]} rounds
 * @param {string} currentRoundId
 * @returns {object[]}
 */
function pickRounds(rounds, currentRoundId) {
  const list = rounds || []
  if (currentRoundId) {
    const wanted = String(currentRoundId).toLowerCase()
    const hit = list.find(function (r) {
      return r && String(r.id).toLowerCase() === wanted
    })
    if (hit) return [hit]
    // 手动指定的轮次不存在：退回自动挑选，不装作找到了
  }
  const active = list.filter(function (r) {
    return r && r.status === "active"
  })
  active.sort(function (a, b) {
    const ka = roundCreatedKey(a)
    const kb = roundCreatedKey(b)
    if (ka !== kb) return ka < kb ? 1 : -1
    return String(a.id).toLowerCase() < String(b.id).toLowerCase() ? -1 : 1
  })
  return active
}

/**
 * 选当前轮次：手动指定 > 最近创建的 active 轮次。
 * @param {object[]} rounds
 * @param {string} currentRoundId
 * @returns {object|null}
 */
function pickRound(rounds, currentRoundId) {
  const candidates = pickRounds(rounds, currentRoundId)
  return candidates.length ? candidates[0] : null
}

/**
 * 轮次里挑一题：掌握度最低的 topic 优先，同 topic 内按清单原始顺序（题号顺序）。
 *
 * 先在「题库里能对上号」的待办条目里挑（按 id 或标题匹配）；一个都匹配不上时
 * 才退回全部待办条目——题库为空（纯本地模式）时仍要给得出动作。
 * @param {object} round
 * @param {{byId: Map<string,object>, byTitle: Map<string,object>}} index
 * @param {Object<string, number>} mastery
 * @returns {{item: object, problem: object|null, topic: string}|null}
 */
function pickRoundItem(round, index, mastery) {
  const items = (round && round.items) || []
  const pending = []
  items.forEach(function (item, order) {
    if (!item) return
    if (item.status === "done") return
    pending.push({ item: item, order: order })
  })
  if (pending.length === 0) return null

  // 轮次里混着不在算法题库里的卡片（Python 工具链、设计模式……），
  // 推一个打不开题面的条目不叫「下一个动作」——有对得上的就先给对得上的。
  const matched = pending.filter(function (p) {
    return !!matchProblem(p.item, index)
  })
  const pool = matched.length > 0 ? matched : pending

  pool.sort(function (a, b) {
    const ma = mastery[topicOf(a.item, index)]
    const mb = mastery[topicOf(b.item, index)]
    const va = ma === undefined ? 0.3 : ma
    const vb = mb === undefined ? 0.3 : mb
    if (va !== vb) return va - vb
    return a.order - b.order
  })

  const chosen = pool[0]
  return {
    item: chosen.item,
    problem: matchProblem(chosen.item, index),
    topic: topicOf(chosen.item, index),
  }
}

/**
 * 主动探索：挑一道没做过的题（一个尝试都没有的）。
 * @param {object[]} problems
 * @param {Object<string, object[]>} attemptsByProblem
 * @returns {object|null}
 */
function pickExplore(problems, attemptsByProblem) {
  const untouched = []
  ;(problems || []).forEach(function (p) {
    if (!p) return
    const rows = (attemptsByProblem && attemptsByProblem[String(p.id)]) || []
    if (rows.length === 0) untouched.push(p)
  })
  if (untouched.length === 0) return null
  untouched.sort(function (a, b) {
    return String(a.id) < String(b.id) ? -1 : 1
  })
  return untouched[0]
}

/**
 * 组装「轮次推进」这个动作。
 * @param {object} round
 * @param {{item: object, problem: object|null, topic: string}} picked
 * @param {object[]} queue 到期复习队列
 * @param {object} quota 配额状态
 * @param {Object<string, number>} avgMs 各难度的平均用时
 * @returns {object} 动作
 */
function roundAction(round, picked, queue, quota, avgMs) {
  const left = (round.items || []).filter(function (it) {
    return it && it.status !== "done"
  }).length
  const quotaNote =
    quota.exhausted && queue.length > 0
      ? "今天的复习配额用完了（已复习 " +
        quota.questions +
        " 题 / " +
        quota.minutes +
        " 分钟），先推进轮次。"
      : ""
  return {
    kind: "round",
    problemId: picked.problem ? String(picked.problem.id) : null,
    problem: picked.problem,
    title: picked.problem
      ? picked.problem.title
      : String(picked.item.title || picked.item.id),
    // 轮次里的卡片不一定在算法题库里（Grokking 卡片、Python 工具链……）。
    // 带上轮次号与条目号，前端才知道「去轮次页把它勾掉」，而不是跳去一个空的作答页。
    roundId: round.id ? String(round.id) : "",
    itemId: picked.item && picked.item.id ? String(picked.item.id) : "",
    estimatedMinutes: estimateMinutes(picked.problem, avgMs),
    reason:
      quotaNote +
      "轮次 " +
      round.id +
      " 还剩 " +
      left +
      " 张卡片没做，先补「" +
      picked.topic +
      "」这个短板",
    reviewQueueLength: queue.length,
  }
}

/**
 * 推下一个动作。
 *
 * @param {{store:object, provider:object, rounds:object[], nowIso:string,
 *          settings:object, problems?:object[], currentRoundId?:string}} ctx
 *        problems 是题库快照（M1 的 provider.list() 是异步的，由调用方先 await 好再塞进来）；
 *        currentRoundId 对应设计文档 §4.1「今日页顶部可手动切换当前轮次」。
 * @returns {{kind:string, problemId:string|null, problem:object|null, title:string,
 *            estimatedMinutes:number, reason:string, reviewQueueLength:number}|null}
 */
function nextAction(ctx) {
  const context = ctx || {}
  const store = context.store
  const nowIso = context.nowIso || new Date().toISOString()
  const settings = context.settings || {}
  const problems = context.problems || []

  const attempts =
    store && typeof store.listAttempts === "function" ? store.listAttempts() : []
  const attemptsByProblem = groupAttemptsByProblem(attempts)
  const index = buildProblemIndex(problems)
  const avgMs = averageMsByDifficulty(problems, attemptsByProblem)
  const mastery = masteryByTopic(problems, attemptsByProblem)

  /* ---- 1. 到期复习 ---- */
  const dueCards =
    store && typeof store.listDueCards === "function" ? store.listDueCards(nowIso) : []
  const queue = sortByOverdue(dueCards, nowIso)
  const quota = quotaState(store, nowIso, settings)

  if (queue.length > 0 && !quota.exhausted) {
    const card = queue[0]
    const problem = index.byId.get(String(card.problem_id)) || null
    const overdueDays = Math.max(
      0,
      Math.floor((Date.parse(nowIso) - Date.parse(card.next_review_at)) / 86400000)
    )
    return {
      kind: "review",
      problemId: String(card.problem_id),
      problem: problem,
      title: problem ? problem.title : String(card.problem_id),
      estimatedMinutes: estimateMinutes(problem, avgMs),
      reason:
        "到期复习：还有 " +
        queue.length +
        " 张卡片到期了，先捡逾期最久的那张（已经逾期 " +
        overdueDays +
        " 天）",
      reviewQueueLength: queue.length,
    }
  }

  /* ---- 2. 轮次推进 ---- */
  // 逐个候选轮次试：优先给「题库里对得上号」的那一轮。
  // 真实 learning 仓里 r32（Python 工具链）、r35~r38（设计模式、并发、数据库、机器学习）
  // 的卡片都不在算法题库里，直接推它们等于推一个打不开题面的条目。
  const candidates = pickRounds(context.rounds || [], context.currentRoundId)
  let fallback = null
  for (let i = 0; i < candidates.length; i += 1) {
    const round = candidates[i]
    const picked = pickRoundItem(round, index, mastery)
    if (!picked) continue
    if (!picked.problem) {
      // 这个轮次没有对得上的题，留作兜底（题库为空时它就是唯一的选择）
      if (!fallback) fallback = { round: round, picked: picked }
      continue
    }
    return roundAction(round, picked, queue, quota, avgMs)
  }
  if (fallback) {
    return roundAction(fallback.round, fallback.picked, queue, quota, avgMs)
  }

  /* ---- 3. 主动探索 ---- */
  const explore = pickExplore(problems, attemptsByProblem)
  if (explore) {
    return {
      kind: "explore",
      problemId: String(explore.id),
      problem: explore,
      title: explore.title,
      estimatedMinutes: estimateMinutes(explore, avgMs),
      reason: "没有进行中的轮次了，从题库里挑一道没做过的题",
      reviewQueueLength: queue.length,
    }
  }

  return null
}

module.exports = {
  BASE_MINUTES: BASE_MINUTES,
  DEFAULT_QUOTA_QUESTIONS: DEFAULT_QUOTA_QUESTIONS,
  DEFAULT_QUOTA_MINUTES: DEFAULT_QUOTA_MINUTES,
  nextAction: nextAction,
  quotaState: quotaState,
  consumeQuota: consumeQuota,
  estimateMinutes: estimateMinutes,
  masteryByTopic: masteryByTopic,
  dayKey: dayKey,
}
