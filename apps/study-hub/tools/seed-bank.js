"use strict"
/**
 * seed-bank.js —— 把 fixtures/seed-bank/ 里的样板题导入 learning 仓的 bank/
 *
 * 用法：
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js --dry-run
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/seed-bank.js --force
 *
 * 规则：
 *   - 已存在的文件默认不覆盖（--force 才覆盖）
 *   - 导入前先解析并校验题目，格式不合法的直接报错退出
 *   - 绝不自动 git commit
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot } = require("../core/paths.js")
const { parseProblemMarkdown } = require("../core/bank/parse.js")
const { validateProblem } = require("../core/bank/problem.js")

const FIXTURES_DIR = path.resolve(__dirname, "..", "fixtures", "seed-bank")

/**
 * 递归列出 fixtures 下所有 .md。
 */
function listFixtures(dir) {
  const out = []
  fs.readdirSync(dir, { withFileTypes: true }).forEach(function (entry) {
    const abs = path.join(dir, entry.name)
    if (entry.isDirectory()) out.push.apply(out, listFixtures(abs))
    else if (entry.isFile() && /\.md$/i.test(entry.name)) out.push(abs)
  })
  return out
}

/**
 * 主流程。
 * @param {{env?: object, dryRun?: boolean, force?: boolean, fixturesDir?: string}} opts
 * @returns {{written: string[], skipped: string[], errors: string[]}}
 */
function run(opts) {
  const options = opts || {}
  const learning = resolveLearningRoot(options.env || process.env)
  if (!learning.ok) {
    return { written: [], skipped: [], errors: [learning.reason] }
  }

  const bankDir = path.join(learning.root, "bank")
  const fixturesDir = options.fixturesDir || FIXTURES_DIR
  const written = []
  const skipped = []
  const errors = []

  listFixtures(fixturesDir).forEach(function (file) {
    const parsed = parseProblemMarkdown(fs.readFileSync(file, "utf8"))
    const problem = parsed.problem
    const check = validateProblem(problem)
    if (!check.ok) {
      errors.push(path.basename(file) + "：字段非法（" + check.errors.join("；") + "）")
      return
    }
    const track = problem.tracks[0] || "misc"
    const target = path.join(bankDir, track, problem.id + ".md")
    if (fs.existsSync(target) && !options.force) {
      skipped.push(target)
      return
    }
    if (!options.dryRun) {
      fs.mkdirSync(path.dirname(target), { recursive: true })
      fs.copyFileSync(file, target)
    }
    written.push(target)
  })

  return { written: written, skipped: skipped, errors: errors }
}

function main() {
  const argv = process.argv.slice(2)
  const dryRun = argv.indexOf("--dry-run") !== -1
  const force = argv.indexOf("--force") !== -1
  const result = run({ dryRun: dryRun, force: force })

  if (result.errors.length > 0) {
    console.error("导入失败：")
    result.errors.forEach(function (line) {
      console.error("  · " + line)
    })
    process.exit(1)
  }
  console.log((dryRun ? "（试运行）将要写入：" : "已写入：") + result.written.length + " 道题")
  result.written.forEach(function (line) {
    console.log("  + " + line)
  })
  if (result.skipped.length > 0) {
    console.log("已存在跳过：" + result.skipped.length + " 道（要覆盖加 --force）")
  }
}

if (require.main === module) {
  main()
}

module.exports = { run: run, listFixtures: listFixtures, FIXTURES_DIR: FIXTURES_DIR }
