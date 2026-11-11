"use strict"
/**
 * import-solutions.js —— 把 learning 仓里的存量题解导入 learning/bank/
 *
 * 对应设计文档 §3.5 的 C 路（存量题解）。
 *
 * 用法：
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-solutions.js --dry-run
 *   ALGO_PLAN_LEARNING_ROOT=/path/to/learning node tools/import-solutions.js
 *
 * 扫描范围（相对 learning 根目录）：
 *   code-solutions/c/src —— 区间文件，靠 `// leetcode <题号>` 标记定位
 *   algo-c               —— 单题文件（文件名即题号）与专题目录
 * 命名不符合 leetcode_<题号> 约定的文件（interview_17.c、contest/、bit_operations/）一律跳过。
 *
 * 本工具**绝不**碰 learning 仓的 git（不 commit、不 push）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot } = require("../core/paths.js")
const { serializeProblemMarkdown } = require("../core/bank/parse.js")
const { createProblem } = require("../core/bank/problem.js")
const scan = require("../core/bank/solutions-scan.js")
const bankFiles = require("./bank-files.js")

/** 扫描范围（相对 learning 根目录） */
const SCAN_ROOTS = ["code-solutions/c/src", "algo-c"]

/** 递归列出目录下的 .c / .cpp 文件 */
function listSourceFiles(dir, out) {
  const result = out || []
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (e) {
    return result
  }
  entries.forEach(function (entry) {
    const abs = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      listSourceFiles(abs, result)
      return
    }
    if (entry.isFile() && /\.(c|cpp)$/i.test(entry.name)) result.push(abs)
  })
  return result
}

/** 收集 learning/algo-c 各专题目录下的 README.md（专题索引表） */
function listIndexFiles(algoRoot) {
  const out = []
  let entries
  try {
    entries = fs.readdirSync(algoRoot, { withFileTypes: true })
  } catch (e) {
    return out
  }
  entries.forEach(function (entry) {
    if (!entry.isDirectory()) return
    const file = path.join(algoRoot, entry.name, "README.md")
    if (fs.existsSync(file)) out.push(file)
  })
  return out
}

/**
 * 主流程。
 * @param {{env?: object, dryRun?: boolean, force?: boolean, scriptsDir?: string, learningRoot?: string}} opts
 * @returns {{written: string[], skipped: string[], errors: string[], warnings: string[]}}
 */
function run(opts) {
  const options = opts || {}
  const written = []
  const skipped = []
  const errors = []
  const warnings = []

  const learning = resolveLearningRoot(options.env || process.env, options.learningRoot)
  if (!learning.ok) {
    return { written: written, skipped: skipped, errors: [learning.reason], warnings: warnings }
  }

  // 1. 读专题索引表（README 里的「题号 / 题目 / 难度」）
  const algoRoot = path.join(learning.root, "algo-c")
  const indexFiles = listIndexFiles(algoRoot).map(function (file) {
    return { path: file, text: fs.readFileSync(file, "utf8") }
  })
  const index = scan.parseSolutionIndex(indexFiles)
  index.warnings.forEach(function (w) {
    warnings.push(w)
  })

  // 2. 收集候选文件：基名匹配 leetcode_<题号> 或 leetcode_<起>_<止>
  const candidates = []
  const rejected = []
  SCAN_ROOTS.forEach(function (rel) {
    listSourceFiles(path.join(learning.root, rel)).forEach(function (file) {
      const base = path.basename(file)
      if (scan.FILENAME_PATTERN.test(base) || scan.RANGE_FILENAME_PATTERN.test(base)) {
        candidates.push({ path: file, text: fs.readFileSync(file, "utf8") })
        return
      }
      rejected.push(file)
    })
  })
  rejected.forEach(function (file) {
    warnings.push("文件名不符合 leetcode_<题号> 约定，已跳过：" + path.relative(learning.root, file))
  })

  // 3. 解析成题目条目
  const parsed = scan.parseSolutionFiles(candidates, index.byId)
  parsed.warnings.forEach(function (w) {
    warnings.push(w)
  })

  // 4. 落盘（同一题号只写一份；与目录来源同 id 的题也只留一份）
  const bankDir = path.join(learning.root, "bank")
  const existing = bankFiles.indexExistingProblems(bankDir)
  // dry-run 时目录来源还没落盘，同 id 的冲突看不出来。
  // import-bank.js 把「另一路来源将要写入的 id」传进来，
  // 这样 dry-run 报出来的跳过条数跟真导入一致（契约 G19）。
  const pending = Object.create(null)
  ;(options.pendingIds || []).forEach(function (id) {
    pending[String(id)] = true
  })
  const seen = Object.create(null)
  parsed.items.forEach(function (item) {
    let id
    try {
      id = scan.solutionIdOf(item)
    } catch (e) {
      warnings.push("ID 生成失败，已跳过：" + item.title + "（" + e.message + "）")
      return
    }
    if (seen[id]) {
      skipped.push(id + "（同一题号有多个解法，保留先出现的：" + seen[id] + "）")
      return
    }
    seen[id] = item.file

    const topic = scan.topicOfSolutionFile(item.file) || scan.DEFAULT_SOLUTION_TOPIC
    const track = scan.trackOfSolutionTopic(topic)
    const meta = index.byId[Number(item.sourceId)] ? index.byId[Number(item.sourceId)] : null

    let problem
    try {
      problem = createProblem({
        id: id,
        source: item.source,
        title: item.title,
        // 查不到难度时默认 medium（存量题解本身没有难度信息）
        difficulty: meta ? meta.difficulty : "medium",
        topics: [topic],
        tracks: [track],
        status: "todo",
        // 题面与测试用例本来就缺
        needsFill: true,
        statement: "",
        cases: [],
        solution: item.code,
        solutionLang: item.lang,
      })
    } catch (e) {
      errors.push(id + " 字段非法：" + e.message)
      return
    }

    const existFile = existing[problem.id]
    if (existFile && !options.force) {
      skipped.push(existFile)
      return
    }
    if (pending[problem.id] && !options.force) {
      skipped.push(problem.id + "（另一路来源将要写入同一 id，按 G19 跳过）")
      return
    }
    // 覆盖时写回它自己那个文件，不在别的轨道另开一份
    const target = existFile || path.join(bankDir, problem.tracks[0], problem.id + ".md")
    if (!options.dryRun) {
      fs.mkdirSync(path.dirname(target), { recursive: true })
      fs.writeFileSync(target, serializeProblemMarkdown(problem), "utf8")
    }
    written.push(target)
  })

  return { written: written, skipped: skipped, errors: errors, warnings: warnings }
}

/**
 * 命令行入口。
 */
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
  if (result.warnings.length > 0) {
    console.log("告警 " + result.warnings.length + " 条：")
    result.warnings.slice(0, 20).forEach(function (line) {
      console.log("  ! " + line)
    })
    if (result.warnings.length > 20) {
      console.log("  （其余 " + (result.warnings.length - 20) + " 条省略）")
    }
  }
  console.log("提示：导入器不碰 learning 仓的 git，是否提交由你自己决定。")
}

if (require.main === module) {
  main()
}

module.exports = {
  run: run,
  main: main,
  SCAN_ROOTS: SCAN_ROOTS,
  listSourceFiles: listSourceFiles,
  listIndexFiles: listIndexFiles,
  bankFiles: bankFiles,
}
