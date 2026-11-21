"use strict"
/**
 * algorithm-cli.js —— 算法轨的 CLI 分支
 *
 * P4 的 `tools/supply.js` 在 `--track algorithm` 时把整段 argv 交给这里（耦合面第 3 处，≤ 15 行）。
 *
 * 子命令：plan / generate / verify / report（review 是 P4 的，P5 不碰）。
 *
 * 退出码（契约 §10.1 冻结）：
 *   0 成功（含「跑完了但有题被拒」——结果打在输出里）
 *   2 LLM 关掉（503 语义，输出中文提示）
 *   1 参数错 / learning 仓路径解析不出来
 *
 * LLM 关掉时 plan 与 report **照常可用**（G10）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot, localBankDir } = require("../paths.js")
const fill = require("./algorithm-fill.js")
const batch = require("./algorithm-batch.js")
const { verifyAlgorithmUnit, verifyAlgorithmUnits } = require("./algorithm-verify.js")
const { openDb, createStore } = require("../../server/db.js")

/** 算法轨接管的子命令 */
const COMMANDS = ["plan", "generate", "verify", "report"]

/** 中文用法 */
const USAGE = [
  "算法轨批量补题（--track algorithm）：",
  "  node tools/supply.js plan    --track algorithm [--limit <n>]",
  "  node tools/supply.js generate --track algorithm [--batch] [--limit <n>] [--resume]",
  "                               [--retry-rejected] [--dry-run] [--session <batchId>]",
  "  node tools/supply.js verify  --track algorithm [--unit <id>]",
  "  node tools/supply.js report  --track algorithm",
].join("\n")

/**
 * 解析算法轨的命令行参数。
 *
 * @param {string[]} argv
 * @returns {{options: object, error: string}}
 */
function parseAlgorithmArgs(argv) {
  const args = Array.isArray(argv) ? argv.slice() : []
  const options = {
    command: "",
    track: "",
    batch: false,
    limit: null,
    resume: false,
    retryRejected: false,
    dryRun: false,
    session: "",
    unit: "",
  }
  let error = ""
  let i = 0
  while (i < args.length && error === "") {
    const arg = String(args[i])
    if (arg === "--batch") {
      options.batch = true
      i += 1
      continue
    }
    if (arg === "--resume") {
      options.resume = true
      i += 1
      continue
    }
    if (arg === "--retry-rejected") {
      options.retryRejected = true
      i += 1
      continue
    }
    if (arg === "--dry-run") {
      options.dryRun = true
      i += 1
      continue
    }
    if (arg === "--track" || arg === "--limit" || arg === "--session" || arg === "--unit") {
      if (i + 1 >= args.length) {
        error = "选项 " + arg + " 后面缺值"
        break
      }
      const value = String(args[i + 1])
      if (arg === "--track") options.track = value
      if (arg === "--limit") {
        const n = Number(value)
        if (!Number.isFinite(n) || n <= 0 || Math.floor(n) !== n) {
          error = "--limit 要一个正整数，收到：" + value
        } else {
          options.limit = n
        }
      }
      if (arg === "--session") {
        if (!/^[A-Za-z0-9._-]+$/.test(value)) {
          error = "--session 只能含字母、数字、点、下划线与连字符，收到：" + value
        } else {
          options.session = value
        }
      }
      if (arg === "--unit") options.unit = value
      i += 2
      continue
    }
    if (arg.charAt(0) === "-") {
      error = "不认识的选项：" + arg
      break
    }
    if (options.command === "") {
      options.command = arg
      i += 1
      continue
    }
    error = "多了一个位置参数：" + arg
  }
  return { options: options, error: error }
}

/**
 * 跑算法轨的命令。
 *
 * @param {{
 *   argv: string[], env?: object, store: object, client?: object,
 *   learningRoot?: string, localBank?: string, gitRunner?: Function, write?: Function
 * }} params
 * @returns {Promise<{code: number, output: string}>}
 */
async function runAlgorithmCommand(params) {
  const o = params || {}
  const argv = Array.isArray(o.argv) ? o.argv : []
  const env = o.env || process.env
  const store = o.store || createStore(openDb(o.dbPath || ":memory:"))
  const lines = []
  const say = function (text) {
    lines.push(String(text))
  }

  const parsed = parseAlgorithmArgs(argv)
  if (parsed.error) {
    say("参数错：" + parsed.error)
    say(USAGE)
    return { code: 1, output: lines.join("\n") }
  }
  const options = parsed.options
  if (COMMANDS.indexOf(options.command) === -1) {
    say("子命令非法：" + (options.command || "（空）") + "。算法轨只接 plan / generate / verify / report。")
    say(USAGE)
    return { code: 1, output: lines.join("\n") }
  }
  if (options.track && options.track !== "algorithm") {
    say("算法轨分支只接 --track algorithm，收到：" + options.track)
    return { code: 1, output: lines.join("\n") }
  }

  // —— learning 仓路径（G16：不硬编码，解析不到就降级不抛错）——
  const learning = resolveLearningRoot(env, o.learningRoot)
  if (!learning.ok) {
    say("解析不到 learning 仓：" + (learning.reason || "未知原因"))
    say("用环境变量 STUDY_HUB_LEARNING_ROOT 指到 learning 仓根目录。")
    return { code: 1, output: lines.join("\n") }
  }
  const learningRoot = learning.root
  const unitsDir = path.join(learningRoot, "units", "algorithm")
  const localBank = localBankDir(env, o.localBank)

  // ——— plan ———
  if (options.command === "plan") {
    const scan = fill.scanBank({ learningRoot: learningRoot, localBank: localBank })
    // 磁盘上已经补过的题不再算缺口（存量题库是只读的，缺口靠 units/algorithm/ 消）
    const units = fill.readUnitStatuses(unitsDir)
    const plan = batch.planBatch({ items: scan.items, store: store, limit: options.limit, units: units })
    say("算法轨缺口清单（共 " + plan.stats.total + " 题）：")
    plan.items.forEach(function (item) {
      if (item.shape === "ready") return
      if (units[item.stem] === "verified") return
      say("  " + item.stem + "  形态 " + item.shape + "  缺 " + item.missing.join("/") + "  来源 " + item.source)
    })
    say("待补 " + plan.planned.length + " 题；跳过 " + plan.skipped.length + " 题")
    const clean = fill.checkLearningClean({ learningRoot: learningRoot, gitRunner: o.gitRunner })
    if (clean.ok) {
      fs.mkdirSync(unitsDir, { recursive: true })
      const jsonPath = path.join(unitsDir, "plan.json")
      fs.writeFileSync(
        jsonPath,
        JSON.stringify(
          {
            track: "algorithm",
            total: plan.stats.total,
            planned: plan.planned.length,
            skipped: plan.skipped.length,
            byShape: plan.stats.byShape,
            missing: plan.stats.missing,
            items: plan.items.map(function (item) {
              return {
                stem: item.stem,
                shape: item.shape,
                missing: item.missing,
                source: item.source,
                difficulty: item.difficulty,
                subtracks: item.subtracks,
                topics: item.topics,
                originPath: item.sourcePath,
              }
            }),
          },
          null,
          2
        ),
        "utf8"
      )
      say("缺口清单已写入：" + jsonPath)
    } else {
      say("没写 plan.json（learning 仓有未提交改动）：" + clean.reason)
    }
    return { code: 0, output: lines.join("\n") }
  }

  // ——— generate ———
  if (options.command === "generate") {
    if (options.dryRun) {
      const scan = fill.scanBank({ learningRoot: learningRoot, localBank: localBank })
      const plan = batch.planBatch({ items: scan.items, store: store, limit: options.limit, units: fill.readUnitStatuses(unitsDir) })
      say("算法轨 dry-run：计划 " + plan.planned.length + " 题，跳过 " + plan.skipped.length + " 题")
      plan.planned.forEach(function (item) { say("  " + item.stem + "  形态 " + item.shape + "  缺 " + item.missing.join("/")) })
      return { code: 0, output: lines.join("\n") }
    }
    const client = o.client
    if (!client || typeof client.complete !== "function" || client.name === "null") {
      say("【LLM 未启用】批量补题需要 LLM 才能生成内容。")
      say("请配置环境变量 ARK_API_KEY 或 ANTHROPIC_API_KEY 后重试。")
      say("App 的其余功能（排程、判题、复习、进度）不受影响。")
      return { code: 2, output: lines.join("\n") }
    }
    const scan = fill.scanBank({ learningRoot: learningRoot, localBank: localBank })
    const batchId = options.session || batch.makeBatchId(new Date())
    const summary = await batch.runBatch({
      items: scan.items,
      client: client,
      store: store,
      batchId: batchId,
      learningRoot: learningRoot,
      unitsDir: unitsDir,
      dryRun: options.dryRun,
      resume: options.resume,
      retryRejected: options.retryRejected,
      limit: options.limit,
      gitRunner: o.gitRunner,
      onProgress: function (p) {
        say("  " + p.stem + "  " + p.state + "（" + p.index + "/" + p.total + "）" + (p.reason ? "  " + p.reason : ""))
      },
    })
    say("批次 " + summary.batchId + "：计划 " + summary.planned + " 题，做了 " + summary.done + " 题")
    say("  通过 " + summary.verified + "，被拒 " + summary.rejected + "，失败 " + summary.failed + "，跳过 " + summary.skipped)
    say("  LLM 调用 " + summary.llmCalls + " 次")
    if (summary.stopped) {
      say(summary.reason + "。用 --resume 接着跑。")
    }
    return { code: 0, output: lines.join("\n") }
  }

  // ——— verify ———
  if (options.command === "verify") {
    if (options.unit) {
      const stem = options.unit.indexOf("algo:") === 0 ? options.unit.slice(5) : options.unit
      const unitPath = path.join(unitsDir, stem + ".md")
      if (!fs.existsSync(unitPath)) {
        say("单元不存在：" + options.unit)
        return { code: 1, output: lines.join("\n") }
      }
      const res = await verifyAlgorithmUnit({ unitPath: unitPath, learningRoot: learningRoot })
      say(options.unit + " → " + res.status + "：" + res.reason)
      return { code: 0, output: lines.join("\n") }
    }
    const summary = await verifyAlgorithmUnits({ unitsDir: unitsDir, learningRoot: learningRoot })
    say("校验了 " + summary.checked + " 个单元：通过 " + summary.verified + "，被拒 " + summary.rejected)
    summary.results.forEach(function (r) {
      if (r.status === "verified") return
      say("  " + r.unit + "  " + r.status + "：" + r.reason)
    })
    return { code: 0, output: lines.join("\n") }
  }

  // ——— report ———
  const scan = fill.scanBank({ learningRoot: learningRoot, localBank: localBank })
  const report = batch.buildCoverageReport({
    items: scan.items,
    store: store,
    batchId: options.session,
    units: fill.readUnitStatuses(unitsDir),
  })
  say(batch.renderCoverageReport(report))
  return { code: 0, output: lines.join("\n") }
}

module.exports = {
  COMMANDS: COMMANDS,
  USAGE: USAGE,
  parseAlgorithmArgs: parseAlgorithmArgs,
  runAlgorithmCommand: runAlgorithmCommand,
}
