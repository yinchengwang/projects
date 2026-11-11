"use strict"
/**
 * import-bank.js —— 一次性把两路题库来源都导进 learning/bank/
 *
 * 对应设计文档 §3.5 的 B 路（复活死代码）与 C 路（存量题解）。
 *
 * 顺序固定：先目录（tools/import-catalog.js），再题解（tools/import-solutions.js）。
 * 先目录的理由：目录那边的元数据（题名、难度、标签、轨道）更全，题解那边只有代码；
 * 反过来写，「同 id 已存在就跳过」的规则会把目录那边的题挡住，参考解法就丢了。
 *
 * 用法：
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --dry-run
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-bank.js --force
 *
 * 本工具**绝不**碰 learning 仓的 git（不 commit、不 push）。
 */

const { resolveLearningRoot } = require("../core/paths.js")
const path = require("node:path")
const catalog = require("./import-catalog.js")
const solutions = require("./import-solutions.js")

/**
 * 主流程：先目录、后题解。
 * @param {{env?: object, dryRun?: boolean, force?: boolean, scriptsDir?: string, learningRoot?: string}} opts
 * @returns {{learningRoot: string|null, catalog: object, solutions: object}}
 */
function run(opts) {
  const options = opts || {}
  const learning = resolveLearningRoot(options.env || process.env, options.learningRoot)
  if (!learning.ok) {
    return {
      learningRoot: null,
      catalog: { written: [], skipped: [], errors: [learning.reason], warnings: [] },
      solutions: { written: [], skipped: [], errors: [], warnings: [] },
    }
  }
  const catalogResult = catalog.run(options)
  // 目录来源「将要写入」的 id 传给题解来源：
  // dry-run 时目录来源没落盘，不传的话跨来源的同 id 冲突看不出来，
  // dry-run 报出来的跳过条数会比真导入少（契约 G19）。
  const pendingIds = catalogResult.written.map(function (file) {
    return path.basename(file).replace(/\.md$/, "")
  })
  return {
    learningRoot: learning.root,
    catalog: catalogResult,
    solutions: solutions.run(Object.assign({}, options, { pendingIds: pendingIds })),
  }
}

/**
 * 命令行入口。
 */
function main() {
  const argv = process.argv.slice(2)
  const dryRun = argv.indexOf("--dry-run") !== -1
  const force = argv.indexOf("--force") !== -1
  const result = run({ dryRun: dryRun, force: force })

  const errors = result.catalog.errors.concat(result.solutions.errors)
  if (errors.length > 0) {
    console.error("导入失败：")
    errors.forEach(function (line) {
      console.error("  · " + line)
    })
    process.exit(1)
  }

  const tag = dryRun ? "（试运行）" : ""
  const warnings = result.catalog.warnings.concat(result.solutions.warnings)
  console.log(tag + "learning 仓：" + (result.learningRoot || "未接入"))
  console.log(
    tag +
      "目录来源：写入 " +
      result.catalog.written.length +
      " 道，跳过 " +
      result.catalog.skipped.length +
      " 道，告警 " +
      result.catalog.warnings.length +
      " 条"
  )
  console.log(
    tag +
      "题解来源：写入 " +
      result.solutions.written.length +
      " 道，跳过 " +
      result.solutions.skipped.length +
      " 道，告警 " +
      result.solutions.warnings.length +
      " 条"
  )
  console.log(tag + "合计写入 " + (result.catalog.written.length + result.solutions.written.length) + " 道题")
  console.log("")
  console.log("目录来源前 10 条：")
  result.catalog.written.slice(0, 10).forEach(function (line) {
    console.log("  + " + line)
  })
  console.log("题解来源：")
  result.solutions.written.forEach(function (line) {
    console.log("  + " + line)
  })
  if (warnings.length > 0) {
    console.log("")
    console.log("告警前 20 条（共 " + warnings.length + " 条）：")
    warnings.slice(0, 20).forEach(function (line) {
      console.log("  ! " + line)
    })
  }
  console.log("")
  console.log("提示：导入器不碰 learning 仓的 git，是否提交由你自己决定。")
}

if (require.main === module) {
  main()
}

module.exports = {
  run: run,
  main: main,
}
