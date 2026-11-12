"use strict"
/**
 * guard.js —— 成本护栏：单次会话请求上限 + 每日请求上限
 *
 * 设计文档 §6.6：单次会话请求上限 + 每日请求上限，超了直接停并提示「今天用完了」。
 *
 * 计数落在 M1 已有的 settings 表里（G30），**不新增数据表**：
 *   llm.quota.session_limit         单次会话请求上限，默认 20
 *   llm.quota.daily_limit           每日请求上限，默认 100
 *   llm.quota.session.<sessionId>   本次会话已用次数
 *   llm.quota.daily.<YYYY-MM-DD>    今天已用次数
 */

const { today } = require("../bank/problem.js")

const DEFAULT_SESSION_LIMIT = 20
const DEFAULT_DAILY_LIMIT = 100
const SESSION_LIMIT_KEY = "llm.quota.session_limit"
const DAILY_LIMIT_KEY = "llm.quota.daily_limit"
const SESSION_PREFIX = "llm.quota.session."
const DAILY_PREFIX = "llm.quota.daily."

/** 会话计数键；没给 sessionId 就落到 default */
function sessionKey(sessionId) {
  return SESSION_PREFIX + (sessionId ? String(sessionId) : "default")
}

/** 每日计数键；没给日期就落到今天 */
function dailyKey(date) {
  return DAILY_PREFIX + (date ? String(date) : today())
}

/** 这个键是会话计数还是每日计数 */
function isSessionKey(key) {
  return String(key).indexOf(SESSION_PREFIX) === 0
}

/** 读这个键对应的上限；settings 表里没配就用默认值 */
function limitFor(store, key) {
  const session = isSessionKey(key)
  const settingKey = session ? SESSION_LIMIT_KEY : DAILY_LIMIT_KEY
  const fallback = session ? DEFAULT_SESSION_LIMIT : DEFAULT_DAILY_LIMIT
  const raw = store.getSetting(settingKey, String(fallback))
  const value = Number(raw)
  return Number.isFinite(value) && value >= 0 ? value : fallback
}

/** 读计数，脏值当 0 */
function readCount(store, key) {
  const raw = store.getSetting(key, "0")
  const value = Number(raw)
  return Number.isFinite(value) && value > 0 ? value : 0
}

/** 步长归一：非法值当 1 */
function normalizeStep(n) {
  if (n === undefined || n === null) return 1
  const value = Number(n)
  return Number.isFinite(value) && value > 0 ? value : 1
}

/**
 * 查配额。
 * @param {{getSetting: Function}} store
 * @param {string} key 会话键或每日键
 * @returns {{ok: boolean, used: number, limit: number}}
 */
function checkQuota(store, key) {
  const used = readCount(store, key)
  const limit = limitFor(store, key)
  return { ok: used < limit, used: used, limit: limit }
}

/**
 * 扣配额。
 * @param {{getSetting: Function, setSetting: Function}} store
 * @param {string} key
 * @param {number} [n] 本次扣几个，默认 1
 * @returns {{used: number, limit: number}}
 */
function consumeQuota(store, key, n) {
  const used = readCount(store, key) + normalizeStep(n)
  store.setSetting(key, String(used))
  return { used: used, limit: limitFor(store, key) }
}

/** 额度用完时给用户看的中文提示 */
function quotaMessage(session, daily) {
  if (!daily.ok) {
    return "今天用完了：LLM 每日请求次数已达上限（" + daily.limit + " 次）。明天再来，或者调高每日配额。"
  }
  return "本次会话的 LLM 请求次数已用完（上限 " + session.limit + " 次）。换个会话再来，或者调高单次会话配额。"
}

/**
 * 同时查会话与每日两个配额。
 * @returns {{ok: boolean, session: object, daily: object, message: string}}
 */
function checkAll(store, sessionId, date) {
  const session = checkQuota(store, sessionKey(sessionId))
  const daily = checkQuota(store, dailyKey(date))
  const ok = session.ok && daily.ok
  return {
    ok: ok,
    session: session,
    daily: daily,
    message: ok ? "" : quotaMessage(session, daily),
  }
}

/** 同时扣会话与每日两个计数 */
function consumeAll(store, sessionId, date) {
  return {
    session: consumeQuota(store, sessionKey(sessionId), 1),
    daily: consumeQuota(store, dailyKey(date), 1),
  }
}

/** 当前生效的两个上限 */
function limits(store) {
  return {
    sessionLimit: limitFor(store, sessionKey("probe")),
    dailyLimit: limitFor(store, dailyKey("probe")),
  }
}

/**
 * 启动时把环境变量里的配额写进 settings 表。
 * **只在键不存在时写**：设置页里改过的值优先，不被环境变量覆盖。
 * @param {{getSetting: Function, setSetting: Function}} store
 * @param {object} env
 */
function seedLimits(store, env) {
  const e = env || {}
  const session = Number(e.ALGO_PLAN_LLM_SESSION_LIMIT)
  if (Number.isFinite(session) && session > 0 && !store.getSetting(SESSION_LIMIT_KEY, "")) {
    store.setSetting(SESSION_LIMIT_KEY, String(Math.floor(session)))
  }
  const daily = Number(e.ALGO_PLAN_LLM_DAILY_LIMIT)
  if (Number.isFinite(daily) && daily > 0 && !store.getSetting(DAILY_LIMIT_KEY, "")) {
    store.setSetting(DAILY_LIMIT_KEY, String(Math.floor(daily)))
  }
}

module.exports = {
  checkQuota: checkQuota,
  consumeQuota: consumeQuota,
  sessionKey: sessionKey,
  dailyKey: dailyKey,
  checkAll: checkAll,
  consumeAll: consumeAll,
  limits: limits,
  seedLimits: seedLimits,
}
