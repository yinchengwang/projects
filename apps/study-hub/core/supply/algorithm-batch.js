"use strict"
/**
 * algorithm-batch.js —— 算法轨批处理：配额、限速、重试、断点续跑、记账
 *
 * 一次 LLM 调用产**四件套**（题面 / 用例 / 参考解法 / 负例，P5-3），
 * 每题最多 1 + supply.batch.max_retries 次尝试。
 *
 * 三条铁律：
 *   1. **超预算就停**（P5-12）：会话或当日调用数到顶 → 立刻停，不硬撑。
 *   2. **幂等续跑**（P5-11）：单题状态落 settings 的 supply.batch.item.algo.<stem>，
 *      同一个批次重跑不重复花钱。
 *   3. **真实执行**（P5-7）：产物落盘后交给 algorithm-verify.js 真编译真执行，
 *      结论再写回单元 md 的 status 一行（P5-10）。
 *
 * 落盘顺序固定：draft → 校验 → 改成 verified / rejected。
 */

const fs = require("node:fs")
const path = require("node:path")

const { buildPrompt, parseResponse, SKELETON_NEEDS } = require("../llm/prompts.js")
const {
  scanBank,
  buildStats,
  writeTriplet,
  patchStatusLine,
  checkLearningClean,
  canonicalCode,
  readUnitStatuses,
} = require("./algorithm-fill.js")
const { verifyAlgorithmUnit } = require("./algorithm-verify.js")

/** settings 键名（契约 §5 冻结，一个字都不能改） */
const SETTING_KEYS = {
  dailyLimit: "supply.batch.daily_limit",
  sessionLimit: "supply.batch.session_limit",
  maxRetries: "supply.batch.max_retries",
  rateMs: "supply.batch.rate_ms",
  callsDailyPrefix: "supply.batch.calls.daily.",
  callsSessionPrefix: "supply.batch.calls.session.",
  itemPrefix: "supply.batch.item.algo.",
  runPrefix: "supply.batch.run.",
}

/** 单题状态（契约 §5 冻结） */
const ITEM_STATES = ["pending", "verified", "rejected", "failed"]

/** 批次汇总的 12 个字段（契约 §5 冻结，逐个对齐） */
const RUN_FIELDS = [
  "batchId",
  "track",
  "planned",
  "done",
  "verified",
  "rejected",
  "failed",
  "skipped",
  "llmCalls",
  "stopped",
  "startedAt",
  "updatedAt",
]

/**
 * 日期键：YYYY-MM-DD（每日调用计数用）。
 * @param {Date} date
 * @returns {string}
 */
function dateKey(date) {
  const d = date instanceof Date ? date : new Date(date)
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return y + "-" + m + "-" + day
}

/**
 * 批次号：默认 batch-<YYYYMMDD>（契约 §5）。
 * @param {Date} date
 * @returns {string}
 */
function makeBatchId(date) {
  const d = date instanceof Date ? date : new Date(date)
  const y = d.getFullYear()
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return "batch-" + y + m + day
}

/**
 * 读一个数字设置；非有限或负数回落默认值（契约 §5 的读取约定）。
 * @param {object} store
 * @param {string} key
 * @param {number} fallback
 * @returns {number}
 */
function readNumberSetting(store, key, fallback) {
  const raw = store.getSetting(key, String(fallback))
  const value = Number(raw)
  if (!Number.isFinite(value) || value < 0) return fallback
  return value
}

/** 参数优先，其次 settings，最后默认值 */
function pickNumber(value, store, key, fallback) {
  if (value === undefined || value === null || value === "") {
    return readNumberSetting(store, key, fallback)
  }
  const n = Number(value)
  if (!Number.isFinite(n) || n < 0) return fallback
  return n
}

/**
 * 读单题进度。
 * @param {object} store
 * @param {string} stem
 * @returns {{stem: string, state: string, attempts: number, reason: string, updatedAt: string}}
 */
function readItemState(store, stem) {
  const raw = store.getSetting(SETTING_KEYS.itemPrefix + stem, "")
  if (!raw) {
    return { stem: stem, state: "pending", attempts: 0, reason: "", updatedAt: "" }
  }
  try {
    const parsed = JSON.parse(raw)
    if (!parsed || typeof parsed !== "object") throw new Error("不是对象")
    return {
      stem: String(parsed.stem || stem),
      state: ITEM_STATES.indexOf(String(parsed.state)) !== -1 ? String(parsed.state) : "pending",
      attempts: Number(parsed.attempts) || 0,
      reason: String(parsed.reason || ""),
      updatedAt: String(parsed.updatedAt || ""),
    }
  } catch (err) {
    return { stem: stem, state: "pending", attempts: 0, reason: "进度记录坏了，按没做过处理", updatedAt: "" }
  }
}

/**
 * 写单题进度。
 * @param {object} store
 * @param {string} stem
 * @param {string} state
 * @param {number} attempts
 * @param {string} reason
 * @param {string} updatedAt
 */
function writeItemState(store, stem, state, attempts, reason, updatedAt) {
  store.setSetting(
    SETTING_KEYS.itemPrefix + stem,
    JSON.stringify({
      stem: stem,
      state: state,
      attempts: Number(attempts) || 0,
      reason: String(reason || ""),
      updatedAt: String(updatedAt || ""),
    })
  )
}

/**
 * 读批次汇总。
 * @param {object} store
 * @param {string} batchId
 * @returns {object|null}
 */
function readRunSummary(store, batchId) {
  const raw = store.getSetting(SETTING_KEYS.runPrefix + batchId, "")
  if (!raw) return null
  try {
    const parsed = JSON.parse(raw)
    if (!parsed || typeof parsed !== "object") return null
    return parsed
  } catch (err) {
    return null
  }
}

/**
 * 写批次汇总。**只写契约 §5 冻结的那 12 个字段**，别的一概不落。
 * @param {object} store
 * @param {object} summary
 */
function writeRunSummary(store, summary) {
  const s = summary || {}
  const out = {}
  RUN_FIELDS.forEach(function (key) {
    out[key] = s[key]
  })
  store.setSetting(SETTING_KEYS.runPrefix + String(s.batchId || ""), JSON.stringify(out))
}

/**
 * 算本轮要处理哪些题（**不调 LLM、不写盘**）。
 *
 * P4 的 `core/supply/plan.js` 用它取 `stats` 做算法轨缺口计数（耦合面第 2 处）。
 *
 * @param {{items?: Array, store: object, resume?: boolean, retryRejected?: boolean, limit?: number|null}} params
 * @returns {{items: Array, planned: Array, skipped: Array, stats: object, warnings: string[]}}
 */
function planBatch(params) {
  const o = params || {}
  const items = Array.isArray(o.items) ? o.items : []
  const store = o.store
  const resume = o.resume === true
  const retryRejected = o.retryRejected === true
  // units：{stem: status}，磁盘上 units/algorithm/ 下**已经有的**单元状态。
  // 存量题库是只读的，所以「这道题补过没有」只能看这里（P5-1 / P5-11）。
  const units = o.units && typeof o.units === "object" ? o.units : {}
  const planned = []
  const skipped = []

  items.forEach(function (item) {
    if (item.shape === "ready") {
      skipped.push({ stem: item.stem, why: "已齐备，不动" })
      return
    }
    // 磁盘上已经有 verified 的单元 → 不重复花钱（不管有没有 --resume）
    if (units[item.stem] === "verified") {
      skipped.push({ stem: item.stem, why: "已经有 verified 的单元" })
      return
    }
    if (units[item.stem] === "rejected" && !retryRejected) {
      skipped.push({ stem: item.stem, why: "已有 rejected 的单元（要 --retry-rejected 才重做）" })
      return
    }
    const state = readItemState(store, item.stem)
    if (resume) {
      if (state.state === "verified") {
        skipped.push({ stem: item.stem, why: "已 verified" })
        return
      }
      if (state.state === "rejected" && !retryRejected) {
        skipped.push({ stem: item.stem, why: "已 rejected（要 --retry-rejected 才重做）" })
        return
      }
    }
    planned.push(item)
  })

  const limit = o.limit === undefined || o.limit === null ? null : Number(o.limit)
  const limited = limit !== null && Number.isFinite(limit) && limit > 0 ? planned.slice(0, limit) : planned

  return { items: items, planned: limited, skipped: skipped, stats: buildStats(items), warnings: [] }
}

/** 默认的等待 */
function defaultSleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms)
  })
}

/**
 * 跑一批。
 *
 * @param {{
 *   items: Array, client: object, store: object,
 *   batchId?: string, learningRoot: string, unitsDir: string,
 *   dryRun?: boolean, resume?: boolean, retryRejected?: boolean, limit?: number|null,
 *   maxRetries?: number|null, sessionLimit?: number|null, dailyLimit?: number|null, rateMs?: number|null,
 *   now?: Function, clock?: Function, sleep?: Function, onProgress?: Function, gitRunner?: Function
 * }} params
 * @returns {Promise<object>} 批次汇总（持久化时只取契约 §5 的 12 个字段）
 */
async function runBatch(params) {
  const o = params || {}
  const store = o.store
  const now = typeof o.now === "function" ? o.now : function () {
    return new Date()
  }
  const clock = typeof o.clock === "function" ? o.clock : Date.now
  const sleep = typeof o.sleep === "function" ? o.sleep : defaultSleep
  const onProgress = typeof o.onProgress === "function" ? o.onProgress : function () {}

  const batchId = String(o.batchId || makeBatchId(now()))
  const maxRetries = pickNumber(o.maxRetries, store, SETTING_KEYS.maxRetries, 2)
  const sessionLimit = pickNumber(o.sessionLimit, store, SETTING_KEYS.sessionLimit, 800)
  const dailyLimit = pickNumber(o.dailyLimit, store, SETTING_KEYS.dailyLimit, 800)
  const rateMs = pickNumber(o.rateMs, store, SETTING_KEYS.rateMs, 1200)

  const plan = planBatch({
    items: o.items,
    store: store,
    resume: o.resume === true,
    retryRejected: o.retryRejected === true,
    limit: o.limit,
    // 磁盘上已经有的单元：verified 的不再花钱，rejected 的要 --retry-rejected 才重做
    units: readUnitStatuses(o.unitsDir),
  })

  const summary = {
    batchId: batchId,
    track: "algorithm",
    planned: plan.planned.length,
    done: 0,
    verified: 0,
    rejected: 0,
    failed: 0,
    skipped: plan.skipped.length,
    llmCalls: 0,
    stopped: false,
    startedAt: now().toISOString(),
    updatedAt: now().toISOString(),
    // —— reason 不落 settings（契约 §5 的 12 个字段里没有它），只给调用方看 ——
    reason: "",
  }

  // —— dry-run：只算计划，不调 LLM、不写文件 ——
  if (o.dryRun === true) {
    writeRunSummary(store, summary)
    return summary
  }

  // —— 写回前查脏（G14 / P5-3）——
  const clean = checkLearningClean({
    learningRoot: String(o.learningRoot || ""),
    gitRunner: o.gitRunner,
  })
  if (!clean.ok) {
    summary.stopped = true
    summary.reason = clean.reason
    summary.updatedAt = now().toISOString()
    writeRunSummary(store, summary)
    return summary
  }

  const sessionCountKey = SETTING_KEYS.callsSessionPrefix + batchId
  const dailyCountKey = SETTING_KEYS.callsDailyPrefix + dateKey(now())
  let sessionUsed = readNumberSetting(store, sessionCountKey, 0)
  let dailyUsed = readNumberSetting(store, dailyCountKey, 0)
  let lastCallStamp = 0
  const attemptsLimit = 1 + maxRetries

  for (let i = 0; i < plan.planned.length; i += 1) {
    const item = plan.planned[i]
    let attempts = 0
    let lastReason = ""
    let lastOutcome = "failed"
    let budgetStopped = false

    while (attempts < attemptsLimit) {
      attempts += 1

      // —— 预算检查（P5-12）：到顶就停，绝不烧穿 ——
      if (sessionUsed >= sessionLimit || dailyUsed >= dailyLimit) {
        budgetStopped = true
        summary.stopped = true
        summary.reason =
          (sessionUsed >= sessionLimit
            ? "达到单批次调用上限（" + sessionLimit + "）"
            : "达到当日调用上限（" + dailyLimit + "）") +
          "，还欠 " +
          (plan.planned.length - i) +
          " 个"
        break
      }

      // —— 限速：两次调用之间的最小间隔 ——
      if (lastCallStamp > 0) {
        const wait = rateMs - (clock() - lastCallStamp)
        if (wait > 0) await sleep(wait)
      }

      const prompt = buildPrompt("generate", {
        mode: "skeleton",
        shape: item.shape,
        stem: item.stem,
        title: item.title,
        difficulty: item.difficulty,
        topics: item.topics,
        url: item.url,
        statement: item.statement,
        cases: item.cases,
      })

      let raw = ""
      try {
        raw = await o.client.complete({ system: prompt.system, user: prompt.user, temperature: 0.6 })
      } catch (err) {
        raw = ""
        lastReason = "调 LLM 失败：" + (err && err.message ? err.message : String(err))
      }
      lastCallStamp = clock()
      sessionUsed += 1
      dailyUsed += 1
      summary.llmCalls += 1
      store.setSetting(sessionCountKey, String(sessionUsed))
      store.setSetting(dailyCountKey, String(dailyUsed))

      if (!raw) {
        lastOutcome = "failed"
        continue
      }

      const parsed = parseResponse(raw, "generate", {
        mode: "skeleton",
        need: SKELETON_NEEDS[item.shape] || SKELETON_NEEDS.A,
      })
      if (!parsed.ok) {
        lastReason = parsed.reason
        lastOutcome = "failed"
        continue
      }

      const draft = {
        stem: item.stem,
        title: item.title,
        difficulty: item.difficulty,
        topics: item.topics,
        subtracks: item.subtracks,
        source: item.source,
        unitSource: item.unitSource,
        sourceId: item.sourceId,
        url: item.url,
        originPath: item.sourcePath,
        // 题面与用例：**题库有的原样保留，缺的才用模型给的**（P5 的边界）
        statement: item.hasStatement ? item.statement : parsed.problem.statement,
        cases: item.hasCases ? item.cases : parsed.problem.cases,
        solution: parsed.problem.solution,
        negative: parsed.problem.negative,
        negativeNote: parsed.problem.negativeNote,
        status: "draft",
      }

      // —— 结构自检：main 函数、负例不能与解法相同 ——
      if (!/\bmain\s*\(/.test(draft.solution)) {
        lastReason = "参考解法里没有 main 函数"
        lastOutcome = "failed"
        continue
      }
      if (!/\bmain\s*\(/.test(draft.negative)) {
        lastReason = "负例里没有 main 函数"
        lastOutcome = "failed"
        continue
      }
      if (canonicalCode(draft.solution) === canonicalCode(draft.negative)) {
        lastReason = "负例与参考解法完全相同"
        lastOutcome = "failed"
        continue
      }

      // —— 落盘三件套（先落 draft）——
      writeTriplet({ unitsDir: String(o.unitsDir || ""), stem: item.stem, draft: draft })

      // —— 真编译真执行（P5-7）——
      const verdict = await verifyAlgorithmUnit({
        unitPath: path.join(String(o.unitsDir || ""), item.stem + ".md"),
        learningRoot: String(o.learningRoot || ""),
      })

      // —— 只改 status 那一行（P5-10 / G15）——
      const mdPath = path.join(String(o.unitsDir || ""), item.stem + ".md")
      fs.writeFileSync(mdPath, patchStatusLine(fs.readFileSync(mdPath, "utf8"), verdict.status), "utf8")

      if (verdict.ok) {
        lastOutcome = "verified"
        lastReason = verdict.reason
        break
      }
      lastOutcome = "rejected"
      lastReason = verdict.reason
      // 校验不合格：还有次数就换一次再试
    }

    if (budgetStopped) break

    summary.done += 1
    if (lastOutcome === "verified") summary.verified += 1
    else if (lastOutcome === "rejected") summary.rejected += 1
    else summary.failed += 1

    writeItemState(store, item.stem, lastOutcome, attempts, lastReason, now().toISOString())
    summary.updatedAt = now().toISOString()
    writeRunSummary(store, summary)
    onProgress({
      stem: item.stem,
      state: lastOutcome,
      reason: lastReason,
      index: i + 1,
      total: plan.planned.length,
    })
  }

  summary.updatedAt = now().toISOString()
  writeRunSummary(store, summary)
  return summary
}

/**
 * 覆盖报告的数据：按来源 / 难度 / 子轨 / 知识点统计**还欠什么**。
 *
 * @param {{items?: Array, store?: object, batchId?: string}} params
 * @returns {object}
 */
function buildCoverageReport(params) {
  const o = params || {}
  const items = Array.isArray(o.items) ? o.items : []
  const stats = buildStats(items)
  // units：{stem: status}，来自 algorithm-fill.js 的 readUnitStatuses(unitsDir)。
  // 存量题库是只读的，所以「欠不欠」看的是 units/algorithm/ 下有没有 verified 的单元。
  const units = o.units && typeof o.units === "object" ? o.units : {}
  const debts = items.filter(function (item) {
    if (item.shape === "ready") return false
    return units[item.stem] !== "verified"
  })
  const filled = items.filter(function (item) {
    if (item.shape === "ready") return false
    return units[item.stem] === "verified"
  })
  const missing = { statement: 0, cases: 0, solution: 0 }
  const debtBySource = {}
  const debtByDifficulty = {}
  const debtBySubtrack = {}
  const debtByTopic = {}

  debts.forEach(function (item) {
    item.missing.forEach(function (m) {
      missing[m] += 1
    })
    debtBySource[item.source] = (debtBySource[item.source] || 0) + 1
    debtByDifficulty[item.difficulty] = (debtByDifficulty[item.difficulty] || 0) + 1
    item.subtracks.forEach(function (t) {
      debtBySubtrack[t] = (debtBySubtrack[t] || 0) + 1
    })
    const topic = item.topics.length ? item.topics[0] : "未标注"
    debtByTopic[topic] = (debtByTopic[topic] || 0) + 1
  })

  const run = o.store && o.batchId ? readRunSummary(o.store, o.batchId) : null

  return {
    total: items.length,
    // 题库里本来就齐的（形态 ready）
    bankReady: stats.byShape.ready,
    // 本计划已经补出来的（units/algorithm/ 下 status 是 verified）
    filled: filled.length,
    // 不再欠的总数
    ready: stats.byShape.ready + filled.length,
    debt: debts.length,
    byShape: stats.byShape,
    bySource: stats.bySource,
    byDifficulty: stats.byDifficulty,
    bySubtrack: stats.bySubtrack,
    byTopic: stats.byTopic,
    missing: missing,
    debtBySource: debtBySource,
    debtByDifficulty: debtByDifficulty,
    debtBySubtrack: debtBySubtrack,
    debtByTopic: debtByTopic,
    run: run,
  }
}

/**
 * 把覆盖报告渲染成中文文本（给人看，打印到 stdout）。
 * @param {object} report
 * @returns {string}
 */
function renderCoverageReport(report) {
  const r = report || {}
  const lines = []
  lines.push("算法轨题库覆盖报告")
  lines.push(
    "  扫到 " +
      r.total +
      " 题：不欠 " +
      r.ready +
      " 题（题库本来就齐 " +
      r.bankReady +
      " + 本计划补出来 " +
      r.filled +
      "），还欠 " +
      r.debt +
      " 题"
  )
  lines.push("  形态：A 补全 " + r.byShape.A + " / B 补用例 " + r.byShape.B + " / C 补解 " + r.byShape.C)
  lines.push("  欠账：缺题面 " + r.missing.statement + "、缺用例 " + r.missing.cases + "、缺参考解法 " + r.missing.solution)
  lines.push("  按来源欠：" + formatCounts(r.debtBySource))
  lines.push("  按难度欠：" + formatCounts(r.debtByDifficulty))
  lines.push("  按子轨欠：" + formatCounts(r.debtBySubtrack))
  lines.push("  按知识点欠（top）：" + formatCounts(topCounts(r.debtByTopic, 10)))
  if (r.run) {
    lines.push(
      "  最近批次 " +
        r.run.batchId +
        "：计划 " +
        r.run.planned +
        "，做了 " +
        r.run.done +
        "，通过 " +
        r.run.verified +
        "，被拒 " +
        r.run.rejected +
        "，失败 " +
        r.run.failed +
        "，跳过 " +
        r.run.skipped +
        "，调用 " +
        r.run.llmCalls +
        " 次" +
        (r.run.stopped ? "（中途停了）" : "")
    )
  }
  return lines.join("\n")
}

/** 计数表排成「k v」的顿号串 */
function formatCounts(counts) {
  const keys = Object.keys(counts || {})
  if (keys.length === 0) return "无"
  keys.sort(function (a, b) {
    return counts[b] - counts[a]
  })
  return keys
    .map(function (k) {
      return k + " " + counts[k]
    })
    .join("、")
}

/** 取计数表的前 n 项 */
function topCounts(counts, n) {
  const out = {}
  const keys = Object.keys(counts || {}).sort(function (a, b) {
    return counts[b] - counts[a]
  })
  keys.slice(0, n).forEach(function (k) {
    out[k] = counts[k]
  })
  return out
}

module.exports = {
  SETTING_KEYS: SETTING_KEYS,
  ITEM_STATES: ITEM_STATES,
  RUN_FIELDS: RUN_FIELDS,
  dateKey: dateKey,
  makeBatchId: makeBatchId,
  readNumberSetting: readNumberSetting,
  readItemState: readItemState,
  writeItemState: writeItemState,
  readRunSummary: readRunSummary,
  writeRunSummary: writeRunSummary,
  planBatch: planBatch,
  runBatch: runBatch,
  buildCoverageReport: buildCoverageReport,
  renderCoverageReport: renderCoverageReport,
  scanBank: scanBank,
  buildStats: buildStats,
}
