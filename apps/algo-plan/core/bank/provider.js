"use strict"
/**
 * provider.js —— 题库 provider 接口 + FileProvider
 *
 * 接口（设计文档 §3.6）：
 *   list(filter)  -> Promise<Problem[]>
 *   get(id)       -> Promise<Problem|undefined>
 *   put(problem)  -> Promise<void>
 *
 * 现在只有 FileProvider（读写 learning/bank/*.md）。
 * 将来换 DbProvider（查 engine/db）只需再加一个实现，上层一行不用改。
 */

const fs = require("node:fs")
const path = require("node:path")

const { parseProblemMarkdown, serializeProblemMarkdown } = require("./parse.js")
const { validateProblem, toSummary } = require("./problem.js")

/**
 * 递归列出目录下所有 .md 文件。
 * @param {string} dir
 * @returns {string[]} 绝对路径
 */
function listMarkdownFiles(dir) {
  const out = []
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (e) {
    return out
  }
  entries.forEach(function (entry) {
    const abs = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      out.push.apply(out, listMarkdownFiles(abs))
    } else if (entry.isFile() && /\.md$/i.test(entry.name)) {
      out.push(abs)
    }
  })
  return out
}

/**
 * 题库 provider。
 */
class FileProvider {
  /**
   * @param {string|null} bankDir 题库根目录；null 表示纯本地模式（无题库）
   */
  constructor(bankDir) {
    this.bankDir = bankDir || null
    this.index = null
    this.warnings = []
  }

  /**
   * 扫描题库目录，建 id → 文件路径的索引。
   * @returns {{byId: Object<string,string>, files: string[], warnings: string[]}}
   */
  buildIndex() {
    const byId = {}
    const files = []
    const warnings = []
    if (!this.bankDir || !fs.existsSync(this.bankDir)) {
      this.index = { byId: byId, files: files, warnings: warnings }
      return this.index
    }
    listMarkdownFiles(this.bankDir).forEach(function (file) {
      files.push(file)
      let text
      try {
        text = fs.readFileSync(file, "utf8")
      } catch (e) {
        warnings.push("读取失败：" + file + "（" + e.message + "）")
        return
      }
      const parsed = parseProblemMarkdown(text)
      if (!parsed.problem || !parsed.problem.id) {
        warnings.push("缺少 id，已跳过：" + file)
        return
      }
      if (byId[parsed.problem.id]) {
        warnings.push("id 重复：" + parsed.problem.id + "（保留先出现的）")
        return
      }
      byId[parsed.problem.id] = file
    })
    this.index = { byId: byId, files: files, warnings: warnings }
    return this.index
  }

  /** 取索引（懒建一次） */
  getIndex() {
    if (!this.index) this.buildIndex()
    return this.index
  }

  /**
   * 列出题目。
   * @param {{track?: string, topic?: string, difficulty?: string, status?: string, idPrefix?: string}} filter
   * @returns {Promise<object[]>}
   */
  async list(filter) {
    const f = filter || {}
    if (!this.bankDir || !fs.existsSync(this.bankDir)) return []
    const index = this.getIndex()
    const out = []
    const files = index.files
    for (const file of files) {
      let problem
      try {
        problem = parseProblemMarkdown(fs.readFileSync(file, "utf8")).problem
      } catch (e) {
        continue
      }
      if (!problem || !problem.id) continue
      if (f.track && problem.tracks.indexOf(f.track) === -1) continue
      if (f.topic && problem.topics.indexOf(f.topic) === -1) continue
      if (f.difficulty && problem.difficulty !== f.difficulty) continue
      if (f.status && problem.status !== f.status) continue
      if (f.idPrefix && problem.id.indexOf(f.idPrefix + "-") !== 0) continue
      out.push(problem)
    }
    out.sort(function (a, b) {
      return a.id < b.id ? -1 : a.id > b.id ? 1 : 0
    })
    return out
  }

  /**
   * 取一道题。
   * @param {string} id
   * @returns {Promise<object|undefined>}
   */
  async get(id) {
    if (!this.bankDir || !fs.existsSync(this.bankDir)) return undefined
    const index = this.getIndex()
    const file = index.byId[id]
    if (!file) return undefined
    const parsed = parseProblemMarkdown(fs.readFileSync(file, "utf8"))
    return parsed.problem
  }

  /**
   * 写入一道题：learning/bank/<track>/<id>.md
   * @param {object} problem
   * @returns {Promise<string>} 写入的绝对路径
   */
  async put(problem) {
    if (!this.bankDir) {
      throw new Error("未连接 learning 仓，无法写入题库（纯本地模式只读 SQLite）")
    }
    const check = validateProblem(problem)
    if (!check.ok) {
      throw new Error("题目字段非法：" + check.errors.join("；"))
    }
    const track = problem.tracks[0] || "misc"
    const dir = path.join(this.bankDir, track)
    fs.mkdirSync(dir, { recursive: true })
    const file = path.join(dir, problem.id + ".md")
    fs.writeFileSync(file, serializeProblemMarkdown(problem), "utf8")
    this.index = null
    return file
  }

  /**
   * 题库概况：文件数、解析告警。
   * @returns {{fileCount: number, problemCount: number, warnings: string[]}}
   */
  stats() {
    const index = this.getIndex()
    return {
      fileCount: index.files.length,
      problemCount: Object.keys(index.byId).length,
      warnings: index.warnings,
    }
  }
}

/**
 * 一个只返回空集的 provider，用于纯本地模式。
 */
class NullProvider {
  async list() {
    return []
  }
  async get() {
    return undefined
  }
  async put() {
    throw new Error("纯本地模式：没有题库可写")
  }
  stats() {
    return { fileCount: 0, problemCount: 0, warnings: [] }
  }
}

module.exports = {
  FileProvider: FileProvider,
  NullProvider: NullProvider,
  listMarkdownFiles: listMarkdownFiles,
  toSummary: toSummary,
}
