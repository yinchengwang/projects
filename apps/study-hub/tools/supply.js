"use strict"
/**
 * supply.js —— 供给流水线 CLI（契约 §10）
 *
 *   node tools/supply.js plan    [--track <t>] [--limit <n>]
 *   node tools/supply.js generate [--track <t>] [--limit <n>] [--session <batchId>]
 *   node tools/supply.js verify  [--all | --track <t> | --unit <id>]
 *   node tools/supply.js review  [--sample <n>] [--track <t>]
 *   node tools/supply.js report  [--track <t>]
 *
 * 三条纪律：
 *   1. **离线批处理**（G30）：不依赖 HTTP 服务。自己开 SQLite、自己读 learning 仓
 *      （路径一律经 core/paths.js 的 resolveLearningRoot 解析，绝不硬编码）。
 *   2. **不自动提交**（G13）：写回 learning 仓只打印 diff，绝不执行 git commit / git push。
 *   3. **密钥只从环境变量读**（G9）：本 CLI **不接受** --api-key 这类参数。
 *
 * LLM 关掉时（G10）：只有 generate 受影响（退出码 2 + 中文提示）；
 * plan / verify / review / report 都不调 LLM，照常可用。
 */

const fs = require("node:fs")
const path = require("node:path")
const readline = require("node:readline/promises")

const { resolveLearningRoot, localBankDir, resolvePythonOverride } = require("../core/paths.js")
const { TRACK_CODES, labelOf } = require("../core/track/tracks.js")
const { loadUnits } = require("../core/unit/index.js")
const { createLlmClient } = require("../core/llm/client.js")
const { createLlmService, readLlmSettings } = require("../core/llm/service.js")
const { FileProvider } = require("../core/bank/provider.js")
const { openDb, createStore } = require("../server/db.js")
const { scanGaps, writePlanFiles, missingOf } = require("../core/supply/plan.js")
const { verifyBatch } = require("../core/supply/verify.js")
const { stateFilePath } = require("../core/supply/unit-file.js")
const { pickReviewTargets, applyReview, todayStamp } = require("../core/supply/review.js")
// P5：算法轨批处理与专线 CLI（只在 --track algorithm 时分派）
const { runAlgorithmCommand, parseAlgorithmArgs } = require("../core/supply/algorithm-cli.js")

/** app 根目录（projects/apps/study-hub） */
const APP_ROOT = path.resolve(__dirname, "..")

/** 五个子命令（契约 §10） */
const COMMANDS = ["plan", "generate", "verify", "review", "report"]

/** 退出码（契约 §10.1） */
const EXIT_OK = 0
const EXIT_USAGE = 1
const EXIT_LLM_OFF = 2

/** 每个子命令允许的选项 */
const ALLOWED_OPTIONS = {
  plan: ["track", "limit"],
  generate: ["track", "limit", "session"],
  verify: ["all", "track", "unit"],
  review: ["sample", "track"],
  report: ["track"],
}

/** 布尔开关（不带值） */
const BOOLEAN_OPTIONS = ["all"]

/** 需要取值的选项 */
const VALUE_OPTIONS = ["track", "limit", "session", "unit", "sample"]

/** 不许出现在命令行上的词（密钥纪律，G9） */
const SECRET_WORDS = ["key", "token", "secret", "password", "passwd"]

/** 缺口种类的中文名 */
const GAP_LABELS = {
  asserts: "缺断言脚本",
  statement: "缺讲解正文",
  cases: "缺测试用例",
  solution: "缺参考解法",
}

/** 单元状态的中文名 */
const STATUS_LABELS = {
  todo: "待补内容",
  draft: "待校验",
  verified: "已验证",
  rejected: "已打回",
  skipped: "已跳过",
}

const USAGE = [
  "供给流水线 CLI（契约 §10）",
  "",
  "用法：node tools/supply.js <子命令> [选项]",
  "",
  "  plan     [--track <轨道>] [--limit <n>]        扫描缺口清单，写 learning/units/<轨道>/plan.json",
  "  generate [--track <轨道>] [--limit <n>] [--session <批次号>]   调 LLM 补断言脚本与讲解正文",
  "  verify   [--all | --track <轨道> | --unit <单元id>]          跑执行器反向校验与变异测试",
  "  review   [--sample <n>] [--track <轨道>]      抽检并写回 frontmatter",
  "  report   [--track <轨道>]                    打印每轨计数与欠账",
  "",
  "退出码：0 正常；1 参数错或 learning 仓路径解析不出来；2 LLM 关掉",
].join("\n")

/**
 * 解析命令行参数。
 * @param {string[]} argv
 * @returns {{command: string, options: object, error: string}}
 */
function parseArgs(argv) {
  const args = Array.isArray(argv) ? argv.slice() : []
  const options = {}
  let command = ""
  let error = ""
  let i = 0

  while (i < args.length) {
    const arg = String(args[i])
    if (arg === "--help" || arg === "-h") {
      return { command: "help", options: options, error: "" }
    }
    if (arg.slice(0, 2) === "--") {
      const eq = arg.indexOf("=")
      const name = eq === -1 ? arg.slice(2) : arg.slice(2, eq)
      if (name === "") {
        error = "选项名是空的"
        break
      }
      const lower = name.toLowerCase()
      const isSecret = SECRET_WORDS.some(function (word) {
        return lower.indexOf(word) !== -1
      })
      if (isSecret) {
        error = "不接受把密钥写在命令行上：" + arg + "（密钥只从环境变量或 .env 里读）"
        break
      }
      if (BOOLEAN_OPTIONS.indexOf(name) !== -1) {
        options[name] = true
        i += 1
        continue
      }
      if (VALUE_OPTIONS.indexOf(name) === -1) {
        error = "不认识的选项：" + arg
        break
      }
      let value = eq === -1 ? "" : arg.slice(eq + 1)
      if (eq === -1) {
        i += 1
        if (i >= args.length) {
          error = "选项 " + arg + " 后面缺值"
          break
        }
        value = String(args[i])
      }
      options[name] = value
      i += 1
      continue
    }
    if (command === "") {
      command = arg
      i += 1
      continue
    }
    error = "多了一个位置参数：" + arg
    break
  }

  if (error !== "") return { command: "", options: options, error: error }
  if (command === "") {
    return { command: "", options: options, error: "少了一个子命令（可用：" + COMMANDS.join(" / ") + "）" }
  }
  if (COMMANDS.indexOf(command) === -1) {
    return {
      command: "",
      options: options,
      error: "不认识的子命令：" + command + "（可用：" + COMMANDS.join(" / ") + "）",
    }
  }
  const allowed = ALLOWED_OPTIONS[command] || []
  Object.keys(options).forEach(function (name) {
    if (error !== "") return
    if (allowed.indexOf(name) === -1) {
      error =
        "「" + command + "」不接受选项 --" + name +
        "（可用：" + allowed.map(function (n) { return "--" + n }).join(" / ") + "）"
    }
  })
  if (error !== "") return { command: "", options: options, error: error }
  return { command: command, options: options, error: "" }
}

/**
 * 解析正整数选项。
 * @param {string|undefined} raw
 * @param {number} fallback 缺省值
 * @returns {number|null} null = 不合法
 */
function parseCount(raw, fallback) {
  if (raw === undefined || raw === null || String(raw) === "") return fallback
  const text = String(raw).trim()
  if (!/^[0-9]+$/.test(text)) return null
  const n = Number(text)
  if (n <= 0) return null
  return n
}

/**
 * 截一段文字出来给人看。
 * @param {string} text
 * @param {number} limit
 * @returns {string}
 */
function preview(text, limit) {
  const s = String(text || "").trim()
  const n = Number(limit) > 0 ? Number(limit) : 400
  if (s.length <= n) return s
  return s.slice(0, n) + "……（后面还有 " + (s.length - n) + " 字）"
}

/**
 * 统计每种缺口各有几个。
 * @param {object[]} gaps
 * @returns {object}
 */
function countKinds(gaps) {
  const counts = {}
  ;(Array.isArray(gaps) ? gaps : []).forEach(function (gap) {
    ;(gap.missing || []).forEach(function (kind) {
      counts[kind] = (counts[kind] || 0) + 1
    })
  })
  return counts
}

/**
 * 默认的人工抽检问询：终端里问人。
 * @param {object} unit
 * @returns {Promise<{passed: boolean, note: string}|null>} null = 人要退出
 */
async function defaultAsk(unit) {
  const rl = readline.createInterface({ input: process.stdin, output: process.stderr })
  try {
    const answer = await rl.question(
      "单元 " + String(unit.id || "") + "（" + String(unit.title || "") + "）通过吗？输入 y 通过，n 打回，q 退出："
    )
    const a = String(answer || "").trim().toLowerCase()
    if (a === "q" || a === "quit") return null
    const passed = a === "" || a === "y" || a === "yes"
    const note = await rl.question("抽检意见（可留空）：")
    return { passed: passed, note: String(note || "").trim() }
  } finally {
    rl.close()
  }
}

/**
 * 读单元的断言脚本。
 * @param {string} learningRoot
 * @param {object} unit
 * @returns {{file: string, text: string}|null}
 */
function readScriptText(learningRoot, unit) {
  const rel = String((unit.entry && unit.entry.script) || "")
  if (rel === "") return null
  const file = path.join(learningRoot, rel.split("/").join(path.sep))
  if (!fs.existsSync(file)) return null
  return { file: file, text: fs.readFileSync(file, "utf8") }
}

/**
 * 挑要补断言脚本的单元：卡类、还缺断言脚本的。
 * @param {object[]} units
 * @param {{track?: string, limit?: number, learningRoot: string|null, contentRoots: string[]}} opts
 * @returns {object[]}
 */
function pickGenerateTargets(units, opts) {
  const o = opts || {}
  const only = o.track ? String(o.track) : ""
  const list = (Array.isArray(units) ? units : []).filter(function (unit) {
    if (only !== "" && String(unit.track || "") !== only) return false
    if (String(unit.kind || "") !== "card") return false
    const missing = missingOf(unit, {
      learningRoot: o.learningRoot || null,
      contentRoots: o.contentRoots || [],
      warnings: [],
    })
    return missing.indexOf("asserts") !== -1
  })
  const n = Number(o.limit) > 0 ? Number(o.limit) : list.length
  return list.slice(0, n)
}

/**
 * 跑一次 CLI。
 * @param {string[]} argv
 * @param {{env?: object, client?: object|null, learningRoot?: string|null, localBank?: string|null,
 *          dbPath?: string, stateFile?: string|null, ask?: Function, today?: Function}} ctx
 * @returns {Promise<{code: number, stdout: string, stderr: string}>}
 */
async function runCli(argv, ctx) {
  const c = ctx || {}
  const env = c.env || process.env
  const out = []
  const err = []

  function say(text) {
    out.push(String(text))
  }
  function warn(text) {
    err.push(String(text))
  }
  function done(code) {
    return {
      code: code,
      stdout: out.length ? out.join("\n") + "\n" : "",
      stderr: err.length ? err.join("\n") + "\n" : "",
    }
  }

  const parsed = parseArgs(argv)
  // P5：算法轨参数由专线解析；保持 P4 的五个子命令与旧参数不变。
  if (Array.isArray(argv) && argv.indexOf("--track") !== -1) {
    const trackAt = argv.indexOf("--track")
    if (String(argv[trackAt + 1] || "") === "algorithm") {
      const algorithm = await runAlgorithmCommand({
        argv: argv,
        env: env,
        store: c.store,
        client: c.client,
        learningRoot: c.learningRoot,
        localBank: c.localBank,
        gitRunner: c.gitRunner,
      })
      return { code: algorithm.code, stdout: algorithm.output + (algorithm.output ? "\n" : ""), stderr: "" }
    }
  }
  if (parsed.error !== "") {
    warn(parsed.error)
    warn(USAGE)
    return done(EXIT_USAGE)
  }
  if (parsed.command === "help") {
    say(USAGE)
    return done(EXIT_OK)
  }

  const command = parsed.command
  const options = parsed.options

  // ---- 轨道与选项校验 ----
  const track = options.track ? String(options.track) : ""
  if (track !== "" && TRACK_CODES.indexOf(track) === -1) {
    warn("不认识的轨道：" + track + "（可用：" + TRACK_CODES.join(" / ") + "）")
    return done(EXIT_USAGE)
  }
  const limit = parseCount(options.limit, 0)
  if (limit === null) {
    warn("--limit 必须是正整数")
    return done(EXIT_USAGE)
  }
  const sample = parseCount(options.sample, 5)
  if (sample === null) {
    warn("--sample 必须是正整数")
    return done(EXIT_USAGE)
  }
  // ---- 解析 learning 仓（G16：解析不到就降级，不抛错） ----
  const learning = resolveLearningRoot(env, c.learningRoot === undefined ? null : c.learningRoot)
  const localBank = c.localBank === null ? null : localBankDir(env, c.localBank || null)
  const unitsDir = learning.root ? path.join(learning.root, "units") : null
  const grokkingDir = learning.root ? path.join(learning.root, "scaffold", "grokking") : null
  const contentRoots = []
  if (learning.root) contentRoots.push(learning.root)
  if (localBank) contentRoots.push(localBank)
  const stateFile = c.stateFile || stateFilePath(APP_ROOT)

  /**
   * 当前该用哪个 Python 解释器。
   *
   * 优先级与判题、环境自检完全一致：settings > STUDY_HUB_PYTHON > 自动探测。
   * 离线 CLI 也必须遵守——否则 `verify --track ai` 会按自动探测落到没有 numpy 的
   * python（3.11），把 13 张 AI 卡全判成「环境缺失跳过」，而服务里判题却能跑。
   * @returns {string|null}
   */
  function pythonOverrideNow() {
    if (c.store && typeof c.store.getSetting === "function") {
      const raw = c.store.getSetting("judge.python.cmd")
      if (raw !== undefined && raw !== null && String(raw).trim() !== "") {
        return String(raw).trim()
      }
    }
    return resolvePythonOverride(env) || null
  }
  const pythonOverride = pythonOverrideNow()

  let units = []
  let loadWarnings = []
  try {
    const loaded = loadUnits({
      learningRoot: learning.root,
      localBank: localBank,
      unitsDir: unitsDir,
      grokkingDir: grokkingDir,
      // numpy 探测与判题用同一个解释器，否则 loader 的警告与判题结论会矛盾
      pythonOverride: pythonOverride,
    })
    units = loaded.units || []
    loadWarnings = loaded.warnings || []
  } catch (e) {
    warn("读单元列表失败：" + String((e && e.message) || e))
  }
  if (!learning.ok) {
    warn(learning.reason || "还没连上 learning 仓，进入纯本地模式")
  }

  const scan = scanGaps({
    units: units,
    learningRoot: learning.root,
    contentRoots: contentRoots,
    track: track,
  })

  // ---- plan ----
  if (command === "plan") {
    say("供给缺口清单（" + todayStamp() + "）")
    scan.tracks.forEach(function (item) {
      const code = String(item.track || "")
      say("")
      say(labelOf(code) + "（" + code + "）：共 " + item.total + " 个单元 —— 已验证 " + item.verified
        + "、待补 " + (item.todo + item.draft) + "、已打回 " + item.rejected + "、已抽检 " + item.reviewed)
      const gaps = scan.gaps.filter(function (gap) {
        return String(gap.track) === code
      })
      if (gaps.length === 0) {
        say("  没有缺口")
        return
      }
      const shown = limit > 0 ? gaps.slice(0, limit) : gaps
      say("  缺口 " + gaps.length + " 个：")
      shown.forEach(function (gap) {
        const kinds = (gap.missing || []).map(function (kind) {
          return GAP_LABELS[kind] || kind
        })
        say("    " + gap.id + "  " + gap.title + "  缺：" + kinds.join("、"))
      })
      if (shown.length < gaps.length) {
        say("    ……还有 " + (gaps.length - shown.length) + " 个没列出来")
      }
    })
    if (scan.warnings.length > 0) {
      say("")
      say("提醒：")
      scan.warnings.forEach(function (text) {
        say("  " + text)
      })
    }
    loadWarnings.forEach(function (text) {
      warn("加载单元时的提醒：" + text)
    })
    if (!unitsDir) {
      warn("还没连上 learning 仓，plan.json 写不了（缺口清单只打在屏幕上）")
    } else {
      const written = writePlanFiles({
        result: scan,
        unitsDir: unitsDir,
        repoRoot: learning.root,
        stateFile: stateFile,
      })
      written.written.forEach(function (code) {
        say("已写入：" + path.join(unitsDir, code, "plan.json"))
      })
      written.blocked.forEach(function (code) {
        warn("缺口清单没写进 " + path.join(unitsDir, code, "plan.json") + "：" + written.reason)
      })
    }
    return done(EXIT_OK)
  }

  // ---- report ----
  if (command === "report") {
    say("供给报告（" + todayStamp() + "）")
    say("learning 仓：" + (learning.root ? learning.root : "还没连上 learning 仓（纯本地模式）"))
    scan.tracks.forEach(function (item) {
      say(labelOf(String(item.track || "")) + "：共 " + item.total + " 个单元 —— todo " + item.todo
        + " / draft " + item.draft + " / verified " + item.verified
        + " / rejected " + item.rejected + " / 已抽检 " + item.reviewed)
    })
    const kinds = countKinds(scan.gaps)
    const names = Object.keys(kinds)
    say("欠账：" + (names.length === 0
      ? "没有"
      : names.map(function (kind) {
          return (GAP_LABELS[kind] || kind) + " " + kinds[kind] + " 个"
        }).join("、")))
    loadWarnings.forEach(function (text) {
      warn("加载单元时的提醒：" + text)
    })
    return done(EXIT_OK)
  }

  // ---- generate ----
  if (command === "generate") {
    if (!learning.root || !unitsDir) {
      warn("还没连上 learning 仓，写回不可用：" + (learning.reason || "解析不出 learning 仓路径"))
      return done(EXIT_USAGE)
    }
    const db = openDb(c.dbPath || path.join(APP_ROOT, "data", "app.db"))
    const store = createStore(db)
    try {
      const fixedClient = c.client || null
      const service = createLlmService({
        client: fixedClient,
        getClient: fixedClient
          ? undefined
          : function () {
              const s = readLlmSettings(store)
              return createLlmClient({
                provider: s.provider || undefined,
                model: s.model || undefined,
                baseUrl: s.baseUrl || undefined,
                enabled: s.enabled,
                env: env,
              })
            },
        store: store,
        provider: new FileProvider(contentRoots),
        env: env,
        learningRoot: learning.root,
        unitsDir: unitsDir,
        stateFile: stateFile,
      })
      const client = service.currentClient()
      if (client.name === "null") {
        warn("LLM 没开（" + String(client.reason || "未启用") + "），generate 用不了。")
        warn("plan / verify / review / report 都不调 LLM，照常可用。")
        return done(EXIT_LLM_OFF)
      }
      if (track === "algorithm") {
        // P4 的边界只有 DB/AI 两轨（边界口径第 1 条），算法轨的批量生成是 P5 的活。
        // 这里**不报「参数错」**——这一期只是没有活干，正常退出即可。
        // verify 不拦：算法单元由 core/supply/verify.js 逐个跳过（理由「算法题由 P5 校验」）。
        say("算法轨的批量生成是 P5 的活（--track algorithm 不在本期范围），没有要补的单元")
        return done(EXIT_OK)
      }
      const batchId = String(options.session || "supply")
      const targets = pickGenerateTargets(units, {
        track: track,
        limit: limit > 0 ? limit : 10,
        learningRoot: learning.root,
        contentRoots: contentRoots,
      })
      if (targets.length === 0) {
        say("没有要补的单元（卡类的断言脚本都齐了）")
        return done(EXIT_OK)
      }
      say("本次要补 " + targets.length + " 个单元（批次号：" + batchId + "）")
      let ok = 0
      let failed = 0
      for (const unit of targets) {
        say("—— " + unit.id + " " + unit.title + " ——")
        try {
          const asserted = await service.generateAssert({ unit: unit, batchId: batchId })
          say("  断言脚本已生成：" + path.relative(learning.root, asserted.file))
          const explained = await service.writeExplain({ unit: unit, batchId: batchId })
          say("  讲解正文已写入：" + path.relative(learning.root, explained.file))
          ok += 1
        } catch (e) {
          const status = e && e.status
          if (status === 503) {
            warn("LLM 中途断了（" + String((e && e.message) || e) + "），剩下的单元不再处理")
            break
          }
          if (status === 429) {
            warn("配额用完了（" + String((e && e.message) || e) + "），剩下的单元不再处理")
            break
          }
          warn("  单元 " + unit.id + " 生成失败：" + String((e && e.message) || e))
          failed += 1
        }
      }
      say("")
      say("本次生成：" + ok + " 个成功，" + failed + " 个失败")
      return done(EXIT_OK)
    } finally {
      db.close()
    }
  }

  // ---- verify ----
  if (command === "verify") {
    const hasAll = options.all === true
    const unitId = options.unit ? String(options.unit) : ""
    if (!hasAll && unitId === "" && track === "") {
      warn("verify 至少要一个范围：--all / --track <轨道> / --unit <单元id>")
      return done(EXIT_USAGE)
    }
    let targets = units
    if (unitId !== "") {
      targets = targets.filter(function (unit) {
        return String(unit.id) === unitId
      })
      if (targets.length === 0) {
        warn("找不到单元：" + unitId)
        return done(EXIT_USAGE)
      }
    }
    if (track !== "") {
      targets = targets.filter(function (unit) {
        return String(unit.track) === track
      })
    }
    if (!learning.root) {
      warn("还没连上 learning 仓，没有单元可校验")
      return done(EXIT_OK)
    }
    const batch = await verifyBatch({
      units: targets,
      learningRoot: learning.root,
      unitsDir: unitsDir,
      repoRoot: learning.root,
      stateFile: stateFile,
      // 与判题、环境自检同一口径：否则 CLI 校验 AI 卡会落到没 numpy 的解释器上
      pythonOverride: pythonOverride,
    })
    say("校验了 " + batch.counts.total + " 个单元：通过 " + batch.counts.verified
      + "，打回 " + batch.counts.rejected + "，跳过 " + batch.counts.skipped)
    batch.results.forEach(function (item) {
      say("  " + item.id + "  " + (STATUS_LABELS[item.status] || item.status) + "  " + item.reason)
    })
    if (batch.diffs.length > 0) {
      say("")
      say("下面是改动（自己看着提交，CLI 绝不替你提交）：")
      batch.diffs.forEach(function (diff) {
        say(diff)
      })
    }
    return done(EXIT_OK)
  }

  // ---- review ----
  if (command === "review") {
    if (!learning.root || !unitsDir) {
      warn("还没连上 learning 仓，写回不可用：" + (learning.reason || "解析不出 learning 仓路径"))
      return done(EXIT_USAGE)
    }
    const ask = typeof c.ask === "function" ? c.ask : defaultAsk
    const targets = pickReviewTargets(units, { sample: sample, track: track })
    if (targets.length === 0) {
      say("没有待抽检的单元（都被人看过了）")
      return done(EXIT_OK)
    }
    say("待抽检单元 " + targets.length + " 个（输入 y 通过，n 打回，q 提前退出）：")
    let written = 0
    let blocked = 0
    for (const unit of targets) {
      say("")
      say("—— " + unit.id + " " + unit.title + " ——")
      say("题面：" + preview(unit.statement, 400))
      const script = readScriptText(learning.root, unit)
      if (script) {
        say("断言脚本：" + path.relative(learning.root, script.file))
        say(preview(script.text, 400))
      } else {
        say("断言脚本：还没有")
      }
      const answer = await ask(unit)
      if (!answer) {
        say("提前退出，已经抽检过的单元保留")
        break
      }
      const applied = applyReview({
        unit: unit,
        passed: answer.passed === true,
        note: String(answer.note || ""),
        unitsDir: unitsDir,
        repoRoot: learning.root,
        stateFile: stateFile,
        today: todayStamp(),
      })
      if (!applied.ok) {
        warn("写回被阻塞：" + applied.reason)
        blocked += 1
        continue
      }
      say("已写回 " + applied.unit.id + "：reviewed: true，reviewed_at: " + applied.unit.reviewed_at
        + "，status: " + applied.unit.status)
      if (applied.diff) {
        say("（下面是 diff，自己看着提交）")
        say(applied.diff)
      }
      written += 1
    }
    say("")
    say("本次抽检：写回 " + written + " 个，没写成 " + blocked + " 个")
    return done(EXIT_OK)
  }

  warn("没实现的子命令：" + command)
  return done(EXIT_USAGE)
}

/**
 * 命令行入口。
 * @param {string[]} argv
 * @returns {Promise<number>}
 */
async function main(argv) {
  const res = await runCli(argv, {})
  if (res.stdout) process.stdout.write(res.stdout)
  if (res.stderr) process.stderr.write(res.stderr)
  return res.code
}

module.exports = {
  COMMANDS: COMMANDS,
  EXIT_OK: EXIT_OK,
  EXIT_USAGE: EXIT_USAGE,
  EXIT_LLM_OFF: EXIT_LLM_OFF,
  USAGE: USAGE,
  parseArgs: parseArgs,
  parseAlgorithmArgs: parseAlgorithmArgs,
  runAlgorithmCommand: runAlgorithmCommand,
  pickGenerateTargets: pickGenerateTargets,
  runCli: runCli,
  main: main,
}

if (require.main === module) {
  main(process.argv.slice(2)).then(function (code) {
    process.exitCode = code
  })
}
