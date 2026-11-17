"use strict"
/**
 * api-session.js —— P1 的会话生命周期接口 + 会话设置
 *
 * 七条路由（契约 §8.2 / §8.2.2）：
 *   POST /api/session/start    建会话（或接着上次没做完的）
 *   GET  /api/session          当前会话 + 队列 + 下一个 + 「上次还有 N 个没做完」
 *   GET  /api/session/next     下一个单元 + 中文理由
 *   POST /api/session/pause    关掉会话（队列留着，下次接着做）
 *   POST /api/session/submit   记完成（**P1 只排不判**：verdict 恒为 null）
 *   GET  /api/settings         会话长度 / 轨权重 / 次数阈值 / 环境自检
 *   PUT  /api/settings         同上的写入口
 */

const path = require("node:path")
const crypto = require("node:crypto")
const { spawnSync } = require("node:child_process")

const { readJsonBody } = require("./router.js")
const { detectPython } = require("../judge/sandbox.js")
const { buildPlan } = require("../core/scheduler/plan.js")
const { gradeWithCount, bumpUnitsSince } = require("../core/review/sm2.js")
const { resolveUnitId } = require("../core/scheduler/pool.js")
const { TRACKS, TRACK_CODES } = require("../core/track/tracks.js")
const { parseRounds } = require("../core/sync/rounds.js")

/* ---------- settings 键 ---------- */

const KEY_SESSION_COUNT = "session.default_count"
const KEY_SESSION_MIN = "session.min_count"
const KEY_SESSION_MAX = "session.max_count"
const KEY_COUNT_K = "review.count_trigger_k"
const KEY_PYTHON = "judge.python.cmd"

const DEFAULT_SESSION_COUNT = 3
const DEFAULT_SESSION_MIN = 1
const DEFAULT_SESSION_MAX = 10
const DEFAULT_COUNT_K = 5

/**
 * P1 提交时给 SM-2 的质量。
 * P1 没有判题信号（verdict 恒为 null），所以按「看过一遍」记 3 分：
 * 不吹（不记 5）也不贬（不记 1）。P2 接入判题后改用真实判定折算出的质量。
 */
const P1_REVIEW_QUALITY = 3

/* ---------- 小工具 ---------- */

function weightKey(code) {
  return "track.weight." + code
}

function enabledKey(code) {
  return "track.enabled." + code
}

function readInt(store, key, fallback) {
  const raw = Number(store.getSetting(key, String(fallback)))
  if (!Number.isInteger(raw)) return fallback
  return raw
}

function readNumber(store, key, fallback) {
  const raw = Number(store.getSetting(key, String(fallback)))
  if (!Number.isFinite(raw) || raw < 0) return fallback
  return raw
}

function readBool(store, key, fallback) {
  const raw = String(store.getSetting(key, fallback ? "1" : "0"))
  return raw === "1" || raw.toLowerCase() === "true"
}

/**
 * 三条轨的权重与开关（settings 优先于 tracks.js 的默认值）。
 * @param {object} store
 * @returns {Array<{code: string, label: string, weight: number, enabled: boolean}>}
 */
function trackSettings(store) {
  return TRACKS.map(function (t) {
    return {
      code: String(t.code),
      label: String(t.label),
      weight: readNumber(store, weightKey(t.code), t.weight),
      enabled: readBool(store, enabledKey(t.code), t.enabled !== false),
    }
  })
}

/* ---------- 环境自检 ---------- */

/**
 * 探测 Python 与 numpy。
 * spawn 与 python 都可注入——测试用假实现，不碰真环境。
 * @param {{spawn?: Function, python?: object|null, override?: string|null}} opts
 * @returns {{python: {ok: boolean, detail: string}, numpy: {ok: boolean, detail: string}}}
 */
function probeEnv(opts) {
  const options = opts || {}
  const spawn = options.spawn || spawnSync
  const python =
    options.python !== undefined ? options.python : detectPython(options.override || null)
  if (!python) {
    return {
      python: { ok: false, detail: "没找到 Python 解释器（python3 / python / py -3）" },
      numpy: { ok: false, detail: "没有 Python 解释器，依赖 numpy 的单元这次跳过" },
    }
  }
  const cmd = String(python.cmd)
  const args = Array.isArray(python.args) ? python.args.slice() : []
  let numpyOk = false
  let numpyDetail = ""
  try {
    const res = spawn(cmd, args.concat(["-c", "import numpy"]), {
      encoding: "utf8",
      timeout: 5000,
    })
    numpyOk = !!(res && res.status === 0)
    numpyDetail = numpyOk
      ? "numpy 可用（" + cmd + "）"
      : "缺 numpy：" + String((res && res.stderr) || "导入失败").trim().split("\n")[0]
  } catch (err) {
    numpyOk = false
    numpyDetail = "numpy 探测失败：" + String(err && err.message ? err.message : err)
  }
  return {
    python: { ok: true, detail: cmd + (args.length ? " " + args.join(" ") : "") },
    numpy: { ok: numpyOk, detail: numpyDetail },
  }
}

/**
 * 进程内缓存的环境探测（探一次就够）。
 * @param {object} ctx
 * @returns {Function}
 */
function createEnvProbe(ctx) {
  let cached = null
  return function () {
    if (!cached) {
      cached = probeEnv({ override: ctx.store.getSetting(KEY_PYTHON) || null })
    }
    return cached
  }
}

/* ---------- 会话上下文 ---------- */

/**
 * 读轮次清单（没连 learning 仓就空数组）。
 * @param {object} ctx
 * @returns {Array<object>}
 */
function loadRounds(ctx) {
  if (!ctx.learning || !ctx.learning.ok || !ctx.learning.root) return []
  try {
    const parsed = parseRounds(path.join(ctx.learning.root, "progress"))
    return parsed.rounds || []
  } catch (err) {
    return []
  }
}

/**
 * 把 store 里的数据凑成 buildPlan 的入参。
 * @param {object} ctx
 * @param {{n: number, nowIso: string, env: object}} opts
 * @returns {object}
 */
function buildPlanInput(ctx, opts) {
  const store = ctx.store
  const progressByUnit = {}
  store.listUnitProgress().forEach(function (row) {
    progressByUnit[String(row.unit_id)] = String(row.progress)
  })
  const masteryByUnit = {}
  store.listMastery("unit").forEach(function (row) {
    masteryByUnit[String(row.scope_id)] = Number(row.level || 0)
  })
  return {
    n: opts.n,
    nowIso: opts.nowIso,
    k: readInt(store, KEY_COUNT_K, DEFAULT_COUNT_K),
    units: store.listUnits(),
    progressByUnit: progressByUnit,
    cards: store.listReviewCards(),
    masteryByUnit: masteryByUnit,
    rounds: loadRounds(ctx),
    tracks: trackSettings(store),
    env: opts.env,
  }
}

function newSessionId() {
  return "s-" + Date.now().toString(36) + "-" + crypto.randomBytes(4).toString("hex")
}

/**
 * 下一个该做的单元：优先 current，其次第一个 pending。
 * @param {Array<object>} queue
 * @returns {object|null}
 */
function pickNext(queue) {
  let current = null
  let firstPending = null
  ;(queue || []).forEach(function (item) {
    if (!current && item.state === "current") current = item
    if (!firstPending && item.state === "pending") firstPending = item
  })
  return current || firstPending || null
}

/**
 * 队列里还没做完的项数。
 * @param {Array<object>} queue
 * @returns {number}
 */
function unfinishedCount(queue) {
  let n = 0
  ;(queue || []).forEach(function (item) {
    if (item.state === "pending" || item.state === "current") n += 1
  })
  return n
}

/**
 * 队列项 → 给前端看的行。
 *
 * 冻结形状是 `{unit_id, seq, state}`；这里**额外**补 `title` 与 `track`
 * （契约 §8.2.3）——首页要显示「当前会话的这几个单元」，光有 id 没法给人看。
 * 单元不在 units 表里时 title 回落成 unit_id、track 给空串。
 * @param {object} store
 * @param {string} sessionId
 * @returns {Array<{unit_id: string, seq: number, state: string, title: string, track: string}>}
 */
function queueRows(store, sessionId) {
  return store.listQueue(sessionId).map(function (item) {
    const unit = store.getUnit(item.unit_id) || {}
    return {
      unit_id: String(item.unit_id),
      seq: Number(item.seq),
      state: String(item.state),
      title: String(unit.title || item.unit_id),
      track: String(unit.track || ""),
    }
  })
}

/**
 * 找「还能接着做」的会话：最近一个还有没做完的项的会话。
 * @param {object} ctx
 * @returns {{session: object, queue: Array<object>}|null}
 */
function findResumable(ctx) {
  const sessions = ctx.store.listSessions(20)
  for (let i = 0; i < sessions.length; i += 1) {
    const queue = ctx.store.listQueue(sessions[i].id)
    if (unfinishedCount(queue) > 0) {
      return { session: sessions[i], queue: queue }
    }
  }
  return null
}

/**
 * 卡片对上单元 id（优先 unit_id，其次 problem_id）。
 * @param {Array<object>} cards
 * @param {string} unitId
 * @returns {object|null}
 */
function findCardByUnit(cards, unitId) {
  let found = null
  ;(cards || []).forEach(function (card) {
    if (!found && resolveUnitId(card) === String(unitId)) found = card
  })
  return found
}

/**
 * 新建卡时的 problem_id。
 *
 * `review_cards.problem_id` 是主键，而算法轨的老卡用的是**不带前缀**的
 * `lc-00001`——如果新卡用带前缀的 `algo:lc-00001` 当主键，同一个单元就会
 * 出现两行卡（老行 + 新行），复习调度会重复计数。
 * 所以：算法轨沿用旧约定（去掉 algo: 前缀），其它轨用完整单元 id。
 * @param {string} unitId
 * @returns {string}
 */
function cardProblemId(unitId) {
  const text = String(unitId || "")
  const idx = text.indexOf(":")
  if (idx < 0) return text
  const prefix = text.slice(0, idx)
  const stem = text.slice(idx + 1)
  return prefix === "algorithm" ? stem : text
}

/**
 * 设置页要的那一份数据。
 * @param {object} ctx
 * @param {object} env
 * @returns {object}
 */
function settingsPayload(ctx, env) {
  const store = ctx.store
  const unitCounts = {}
  store.listUnits().forEach(function (unit) {
    const track = String(unit.track || "")
    unitCounts[track] = (unitCounts[track] || 0) + 1
  })
  return {
    session: {
      default_count: readInt(store, KEY_SESSION_COUNT, DEFAULT_SESSION_COUNT),
      min_count: readInt(store, KEY_SESSION_MIN, DEFAULT_SESSION_MIN),
      max_count: readInt(store, KEY_SESSION_MAX, DEFAULT_SESSION_MAX),
    },
    tracks: trackSettings(store).map(function (t) {
      return {
        code: t.code,
        label: t.label,
        weight: t.weight,
        enabled: t.enabled,
        unit_count: unitCounts[t.code] || 0,
      }
    }),
    review: { count_trigger_k: readInt(store, KEY_COUNT_K, DEFAULT_COUNT_K) },
    env: env,
    warnings: [],
  }
}

/**
 * 组装路由表。
 * @param {{store: object, learning: object, sendJson: Function, envProbe?: Function}} ctx
 * @returns {Array<{method: string, pattern: string, handler: Function}>}
 */
function buildRoutes(ctx) {
  const routes = []
  const envProbe = ctx.envProbe || createEnvProbe(ctx)
  const nowIso = function () {
    return new Date().toISOString()
  }

  /* ---------- POST /api/session/start ---------- */

  routes.push({
    method: "POST",
    pattern: "/api/session/start",
    handler: async function (req, res) {
      const body = await readJsonBody(req)
      const store = ctx.store
      const min = readInt(store, KEY_SESSION_MIN, DEFAULT_SESSION_MIN)
      const max = readInt(store, KEY_SESSION_MAX, DEFAULT_SESSION_MAX)
      let n = readInt(store, KEY_SESSION_COUNT, DEFAULT_SESSION_COUNT)
      if (body.n !== undefined && body.n !== null && body.n !== "") {
        n = Number(body.n)
        if (!Number.isInteger(n) || n < min || n > max) {
          ctx.sendJson(res, 400, { error: "会话长度必须是 1~10 的整数" })
          return
        }
      }

      // 接着上次没做完的（reset 为 true 才重排）
      if (body.reset !== true) {
        const resumable = findResumable(ctx)
        if (resumable) {
          ctx.sendJson(res, 200, {
            session: resumable.session,
            queue: queueRows(store, resumable.session.id),
            quota: {},
            warnings: ["接着上次没做完的会话继续"],
          })
          return
        }
      }

      const startedAt = nowIso()
      const plan = buildPlan(
        buildPlanInput(ctx, { n: n, nowIso: startedAt, env: envProbe() })
      )
      if (plan.queue.length === 0) {
        const warnings = plan.warnings.slice()
        if (warnings.join(" ").indexOf("没有可学的单元，建议去供给流水线补内容") === -1) {
          warnings.push("没有可学的单元，建议去供给流水线补内容")
        }
        ctx.sendJson(res, 200, {
          session: null,
          queue: [],
          quota: plan.quota,
          warnings: warnings,
        })
        return
      }

      const sessionId = newSessionId()
      const tracks = Object.keys(plan.quota).filter(function (code) {
        return plan.quota[code] > 0
      })
      store.insertSession({
        id: sessionId,
        started_at: startedAt,
        planned_count: plan.queue.length,
        done_count: 0,
        tracks: tracks,
      })
      store.replaceQueue(
        sessionId,
        plan.queue.map(function (item, index) {
          return {
            unit_id: String(item.unit.id),
            seq: item.seq,
            state: index === 0 ? "current" : "pending",
          }
        })
      )
      ctx.sendJson(res, 200, {
        session: store.getSession(sessionId),
        queue: queueRows(store, sessionId),
        quota: plan.quota,
        warnings: plan.warnings,
      })
    },
  })

  /* ---------- GET /api/session ---------- */

  routes.push({
    method: "GET",
    pattern: "/api/session",
    handler: async function (req, res) {
      const sessions = ctx.store.listSessions(1)
      const session = sessions.length > 0 ? sessions[0] : null
      if (!session) {
        ctx.sendJson(res, 200, {
          session: null,
          queue: [],
          next: null,
          notice: "",
          warnings: [],
        })
        return
      }
      const queue = queueRows(ctx.store, session.id)
      const next = pickNext(queue)
      const left = unfinishedCount(queue)
      ctx.sendJson(res, 200, {
        session: session,
        queue: queue,
        next: next ? { unit_id: next.unit_id, seq: next.seq, total: queue.length } : null,
        notice: left > 0 ? "上次还有 " + left + " 个没做完" : "",
        warnings: [],
      })
    },
  })

  /* ---------- GET /api/session/next ---------- */

  routes.push({
    method: "GET",
    pattern: "/api/session/next",
    handler: async function (req, res) {
      const sessions = ctx.store.listSessions(1)
      const session = sessions.length > 0 ? sessions[0] : null
      if (!session) {
        ctx.sendJson(res, 200, { unit: null, seq: 0, total: 0, reason: "还没有会话" })
        return
      }
      const queue = queueRows(ctx.store, session.id)
      const next = pickNext(queue)
      if (!next) {
        ctx.sendJson(res, 200, {
          unit: null,
          seq: 0,
          total: queue.length,
          reason: "这一轮的单元都做完了",
        })
        return
      }
      const unit = ctx.store.getUnit(next.unit_id) || null
      const plan = buildPlan(
        buildPlanInput(ctx, { n: 1, nowIso: nowIso(), env: envProbe() })
      )
      let reason = "新单元，先把掌握度补上来"
      if (plan.dueUnitIds.has(next.unit_id)) reason = "到期复习，先把这张过一遍"
      else if (plan.currentRoundUnitIds.has(next.unit_id)) reason = "当前轮次里的下一个"
      ctx.sendJson(res, 200, {
        unit: unit,
        seq: next.seq,
        total: queue.length,
        reason: reason,
      })
    },
  })

  /* ---------- POST /api/session/pause ---------- */

  routes.push({
    method: "POST",
    pattern: "/api/session/pause",
    handler: async function (req, res) {
      const store = ctx.store
      const open = store.getLatestOpenSession()
      const resumable = findResumable(ctx)
      const target = open || (resumable ? resumable.session : null)
      if (!target) {
        ctx.sendJson(res, 200, { session: null, queue: [] })
        return
      }
      store.updateSession(target.id, { ended_at: nowIso() })
      ctx.sendJson(res, 200, {
        session: store.getSession(target.id),
        queue: queueRows(store, target.id),
      })
    },
  })

  /* ---------- POST /api/session/submit ---------- */

  routes.push({
    method: "POST",
    pattern: "/api/session/submit",
    handler: async function (req, res) {
      const body = await readJsonBody(req)
      const unitId = String(
        body.unit_id === undefined || body.unit_id === null ? "" : body.unit_id
      ).trim()
      if (unitId === "") {
        ctx.sendJson(res, 400, { error: "缺少 unit_id" })
        return
      }
      const store = ctx.store
      const sessions = store.listSessions(1)
      const session = sessions.length > 0 ? sessions[0] : null
      if (!session) {
        ctx.sendJson(res, 400, { error: "这个单元不在当前会话里" })
        return
      }
      const queue = queueRows(store, session.id)
      let item = null
      queue.forEach(function (q) {
        if (!item && q.unit_id === unitId) item = q
      })
      if (!item) {
        ctx.sendJson(res, 400, { error: "这个单元不在当前会话里" })
        return
      }

      const at = nowIso()

      // 重复提交：不再重复记账，原样回一个结果
      if (item.state === "done") {
        const next = pickNext(queue)
        ctx.sendJson(res, 200, {
          ok: true,
          judged: false,
          verdict: null,
          passed: 0,
          total: 0,
          message: "这个单元已经完成过了",
          failed: null,
          stdout: "",
          next: next ? { unit_id: next.unit_id, seq: next.seq, total: queue.length } : null,
          session: store.getSession(session.id),
          due: [],
          warnings: [],
        })
        return
      }

      // 1. 队列项置 done，done_count 跟着走
      store.updateQueueItem(session.id, unitId, { state: "done" })
      store.updateSession(session.id, { done_count: store.countQueueDone(session.id) })

      // 2. 单元进度置 done
      store.upsertUnitProgress(unitId, "done", at)

      // 3. 所有复习卡 units_since += 1（任何轨完成一个单元都算）
      const k = readInt(store, KEY_COUNT_K, DEFAULT_COUNT_K)
      const cards = store.listReviewCards()
      const newlyDue = []
      cards.forEach(function (card) {
        const before = Number(card.units_since || 0)
        if (before + 1 >= k && before < k) {
          const id = resolveUnitId(card)
          if (id) newlyDue.push(id)
        }
      })
      bumpUnitsSince(cards, 1).forEach(function (card) {
        store.upsertReviewCard({
          problem_id: String(card.problem_id),
          unit_id: card.unit_id === undefined || card.unit_id === null ? null : String(card.unit_id),
          easiness: Number(card.easiness),
          interval_days: Number(card.interval_days),
          repetitions: Number(card.repetitions),
          last_quality:
            card.last_quality === null || card.last_quality === undefined
              ? null
              : Number(card.last_quality),
          last_at: card.last_at === null || card.last_at === undefined ? null : String(card.last_at),
          next_review_at: String(card.next_review_at),
          lapses: Number(card.lapses || 0),
          units_since: Number(card.units_since || 0),
        })
      })

      // 4. 被提交的单元自己的卡：跑一次 SM-2 更新，units_since 归零
      const own = findCardByUnit(cards, unitId)
      const ownCard = own || {
        problem_id: cardProblemId(unitId),
        unit_id: unitId,
        easiness: 2.5,
        interval_days: 0,
        repetitions: 0,
        last_quality: null,
        last_at: null,
        next_review_at: at,
        lapses: 0,
        units_since: 0,
      }
      const graded = gradeWithCount(ownCard, P1_REVIEW_QUALITY, { nowIso: at })
      store.upsertReviewCard({
        problem_id: String(graded.problem_id),
        unit_id: String(graded.unit_id),
        easiness: Number(graded.easiness),
        interval_days: Number(graded.interval_days),
        repetitions: Number(graded.repetitions),
        last_quality: Number(graded.last_quality),
        last_at: String(graded.last_at),
        next_review_at: String(graded.next_review_at),
        lapses: Number(graded.lapses || 0),
        units_since: Number(graded.units_since || 0),
      })

      // 5. 下一个 pending 置 current
      const afterDone = store.listQueue(session.id)
      const nextItem = pickNext(afterDone)
      if (nextItem && nextItem.state === "pending") {
        store.updateQueueItem(session.id, nextItem.unit_id, { state: "current" })
      }

      // 6. 队列空了就把会话关掉
      const finalQueue = store.listQueue(session.id)
      if (unfinishedCount(finalQueue) === 0) {
        store.updateSession(session.id, { ended_at: at })
      }

      const next = pickNext(finalQueue)
      const unit = store.getUnit(unitId)
      ctx.sendJson(res, 200, {
        ok: true,
        judged: false,
        verdict: null,
        passed: 0,
        total: 0,
        message: "「" + String((unit && unit.title) || unitId) + "」已记为完成",
        failed: null,
        stdout: "",
        next: next ? { unit_id: next.unit_id, seq: next.seq, total: finalQueue.length } : null,
        session: store.getSession(session.id),
        due: newlyDue,
        warnings: [],
      })
    },
  })

  /* ---------- GET /api/settings ---------- */

  routes.push({
    method: "GET",
    pattern: "/api/settings",
    handler: async function (req, res) {
      ctx.sendJson(res, 200, settingsPayload(ctx, envProbe()))
    },
  })

  /* ---------- PUT /api/settings ---------- */

  routes.push({
    method: "PUT",
    pattern: "/api/settings",
    handler: async function (req, res) {
      const body = await readJsonBody(req)
      const store = ctx.store

      // **先校验完再写**：这个接口一次性改四组设置，任何一组不合法就整单拒绝，
      // 不能「前几组已经写进去、后面的组报 400」—— 那会让设置页显示的值与实际生效的值
      // 对不上（实测：{session_count:7, track_weights:{algorithm:-1}} 返回 400，
      // session.default_count 却已经被改成 7）。
      const writes = []

      if (body.session_count !== undefined && body.session_count !== null) {
        const n = Number(body.session_count)
        if (!Number.isInteger(n) || n < DEFAULT_SESSION_MIN || n > DEFAULT_SESSION_MAX) {
          ctx.sendJson(res, 400, { error: "会话长度必须是 1~10 的整数" })
          return
        }
        writes.push([KEY_SESSION_COUNT, String(n)])
      }

      if (body.track_weights !== undefined && body.track_weights !== null) {
        const weights = body.track_weights
        if (typeof weights !== "object" || Array.isArray(weights)) {
          ctx.sendJson(res, 400, { error: "轨权重必须是不小于 0 的数字" })
          return
        }
        const codes = Object.keys(weights)
        for (let i = 0; i < codes.length; i += 1) {
          const value = Number(weights[codes[i]])
          if (!Number.isFinite(value) || value < 0) {
            ctx.sendJson(res, 400, { error: "轨权重必须是不小于 0 的数字" })
            return
          }
          // 未知轨代码一律拒绝：静默接受会在 settings 表里留下没人读的键
          if (TRACK_CODES.indexOf(codes[i]) === -1) {
            ctx.sendJson(res, 400, { error: "未知的轨道代码：" + codes[i] })
            return
          }
        }
        codes.forEach(function (code) {
          writes.push([weightKey(code), String(Number(weights[code]))])
        })
      }

      if (body.track_enabled !== undefined && body.track_enabled !== null) {
        const enabled = body.track_enabled
        if (typeof enabled !== "object" || Array.isArray(enabled)) {
          ctx.sendJson(res, 400, { error: "轨开关必须是布尔值" })
          return
        }
        const codes = Object.keys(enabled)
        for (let i = 0; i < codes.length; i += 1) {
          if (typeof enabled[codes[i]] !== "boolean") {
            ctx.sendJson(res, 400, { error: "轨开关必须是布尔值" })
            return
          }
          if (TRACK_CODES.indexOf(codes[i]) === -1) {
            ctx.sendJson(res, 400, { error: "未知的轨道代码：" + codes[i] })
            return
          }
        }
        codes.forEach(function (code) {
          writes.push([enabledKey(code), enabled[code] ? "1" : "0"])
        })
      }

      if (body.count_trigger_k !== undefined && body.count_trigger_k !== null) {
        const k = Number(body.count_trigger_k)
        if (!Number.isInteger(k) || k < 1) {
          ctx.sendJson(res, 400, { error: "次数阈值 K 必须是大于等于 1 的整数" })
          return
        }
        writes.push([KEY_COUNT_K, String(k)])
      }

      // 全部校验通过，才真正落盘
      writes.forEach(function (pair) {
        store.setSetting(pair[0], pair[1])
      })

      ctx.sendJson(res, 200, settingsPayload(ctx, envProbe()))
    },
  })

  return routes
}

module.exports = {
  buildRoutes: buildRoutes,
  probeEnv: probeEnv,
  settingsPayload: settingsPayload,
}
