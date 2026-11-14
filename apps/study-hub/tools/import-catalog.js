"use strict"
/**
 * import-catalog.js —— 把三个死脚本里的内联题目清单导入 learning/bank/
 *
 * 对应设计文档 §3.5 的 B 路（复活死代码）。
 *
 * 用法：
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/import-catalog.js --dry-run
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/import-catalog.js
 *   STUDY_HUB_LEARNING_ROOT=/path/to/learning node tools/import-catalog.js --force
 *
 * 死脚本**绝不执行**，只做静态解析（原因见 core/bank/legacy.js 的注释）。
 * 本工具**绝不**碰 learning 仓的 git（不 commit、不 push）。
 */

const fs = require("node:fs")
const path = require("node:path")

const { resolveLearningRoot } = require("../core/paths.js")
const { serializeProblemMarkdown } = require("../core/bank/parse.js")
const { createProblem } = require("../core/bank/problem.js")
const legacy = require("../core/bank/legacy.js")
const bankFiles = require("./bank-files.js")

/** 要扫的三个死脚本，顺序固定 */
const SCRIPT_FILES = ["gen-practice-data.js", "expand-practice.js", "split-practice-data.js"]

/** 死脚本默认在 projects/scripts/ */
const DEFAULT_SCRIPTS_DIR = path.resolve(__dirname, "..", "..", "..", "scripts")

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

  const scriptsDir = options.scriptsDir || DEFAULT_SCRIPTS_DIR
  const sources = []
  SCRIPT_FILES.forEach(function (name) {
    const file = path.join(scriptsDir, name)
    if (!fs.existsSync(file)) {
      warnings.push("死脚本不存在，已跳过：" + file)
      return
    }
    sources.push({ name: name, file: file })
  })
  if (sources.length === 0) {
    errors.push("死脚本目录里一个脚本都没找到：" + scriptsDir)
    return { written: written, skipped: skipped, errors: errors, warnings: warnings }
  }

  // 1. 逐个脚本静态解析
  const categories = []
  sources.forEach(function (src) {
    const parsed = legacy.parseLegacyCategories(fs.readFileSync(src.file, "utf8"))
    parsed.warnings.forEach(function (w) {
      warnings.push(src.name + "：" + w)
    })
    parsed.categories.forEach(function (cat) {
      categories.push({ slug: cat.slug, title: cat.title, items: cat.items })
    })
  })
  if (categories.length === 0) {
    errors.push("三个死脚本里没解析出任何分类")
    return { written: written, skipped: skipped, errors: errors, warnings: warnings }
  }

  // 2. 同一道题出现在多个分类 → 轨道取并集、标签取并集，只写一个文件
  const byId = Object.create(null)
  const order = []
  const unknownSlugs = Object.create(null)
  categories.forEach(function (cat) {
    const track = legacy.mapCategoryToTrack(cat.slug)
    if (!Object.prototype.hasOwnProperty.call(legacy.CATEGORY_TRACK_MAP, cat.slug)) {
      unknownSlugs[cat.slug] = true
    }
    cat.items.forEach(function (item) {
      let id
      try {
        id = legacy.legacyIdOf(item)
      } catch (e) {
        warnings.push("ID 生成失败，已跳过：" + item.title + "（" + e.message + "）")
        return
      }
      const exist = byId[id]
      if (!exist) {
        byId[id] = {
          id: id,
          source: item.platform,
          // 死脚本里存的是源站真实题号（LC 题号 / 剑指 Offer 号），别丢——
          // 题库自己的 id 是导入顺序号，对不上源站，原题链接全靠这个字段回填。
          sourceId: item.problemId,
          title: item.title,
          difficulty: item.difficulty,
          tracks: [track],
          topics: legacy.normalizeLegacyTopics(item.tags),
        }
        order.push(id)
        return
      }
      if (exist.tracks.indexOf(track) === -1) exist.tracks.push(track)
      item.tags.forEach(function (tag) {
        const topic = legacy.normalizeLegacyTopic(tag)
        if (topic !== "" && exist.topics.indexOf(topic) === -1) exist.topics.push(topic)
      })
    })
  })
  Object.keys(unknownSlugs).forEach(function (slug) {
    warnings.push("分类 slug 不在映射表里，已落到兜底轨道 " + legacy.FALLBACK_TRACK + "：" + slug)
  })

  // 3. 落盘（冲突按 id 判断：已存在的同 id 文件一律跳过，--force 才覆盖）
  const bankDir = path.join(learning.root, "bank")
  const existing = bankFiles.indexExistingProblems(bankDir)
  order.forEach(function (id) {
    const data = byId[id]
    let problem
    try {
      problem = createProblem({
        id: data.id,
        source: data.source,
        sourceId: data.sourceId,
        title: data.title,
        // 实测死脚本里只有 easy / medium，没有 hard
        difficulty: data.difficulty === "easy" ? "easy" : "medium",
        topics: data.topics.length > 0 ? data.topics : ["未分类"],
        tracks: data.tracks,
        status: "todo",
        // 死脚本只有元数据，没有题面与测试用例
        needsFill: true,
        statement: "",
        cases: [],
        solution: "",
        solutionLang: "",
      })
    } catch (e) {
      errors.push(data.id + " 字段非法：" + e.message)
      return
    }
    const existFile = existing[problem.id]
    if (existFile && !options.force) {
      skipped.push(existFile)
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
  SCRIPT_FILES: SCRIPT_FILES,
  DEFAULT_SCRIPTS_DIR: DEFAULT_SCRIPTS_DIR,
  bankFiles: bankFiles,
}
