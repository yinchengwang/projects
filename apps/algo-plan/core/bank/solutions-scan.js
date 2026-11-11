"use strict"
/**
 * solutions-scan.js —— 存量题解的静态扫描（纯函数层，无 IO）
 *
 * 对应设计文档 §3.5 的「C 路：存量题解」。
 *
 * 两种命名形态，各有各的取题号办法：
 *   1. 区间文件 learning/code-solutions/c/src/{c,cpp}/leetcode_<起>_<止>.{c,cpp}
 *      —— 靠 `// leetcode <题号>` 注释定位，文件里可能有多道题；
 *   2. 单题文件 learning/algo-c 任意子目录下的 leetcode_<题号>.{c,cpp}
 *      —— 文件名就是题号，整个文件是一道题的解法。
 *
 * 收录范围 = 文件基名匹配 FILENAME_PATTERN 或 RANGE_FILENAME_PATTERN 的 .c/.cpp 文件。
 * 其余文件（interview_17.c 的面经章节号、contest/ 的竞赛题、bit_operations/ 的演示代码）一律跳过。
 */

const { makeId } = require("./problem.js")

/** 标记注释：// leetcode 56 / // leetcode 1 hash map */
const MARKER_PATTERN = /^[ \t]*\/\/[ \t]*leetcode[ \t]+(\d+)[ \t]*(.*)$/gm

/** 单题文件名：leetcode_1455.cpp */
const FILENAME_PATTERN = /^leetcode_(\d+)\.(c|cpp)$/

/** 区间文件名：leetcode_1_100.c */
const RANGE_FILENAME_PATTERN = /^leetcode_\d+_\d+\.(c|cpp)$/

/** 中文难度 → 难度枚举 */
const DIFFICULTY_BY_CN = {
  "简单": "easy",
  "中等": "medium",
  "困难": "hard",
}

/** algo-c 专题目录 → 标签 */
const SOLUTION_TOPIC_BY_DIR = {
  sliding_window: "滑动窗口",
  prefix_sum: "前缀和",
}

/** 没有专题信息时的默认标签 */
const DEFAULT_SOLUTION_TOPIC = "未分类"

/** 没有专题信息时的默认轨道 */
const DEFAULT_SOLUTION_TRACK = "fundamentals"

/** 标签 → 轨道 */
const SOLUTION_TRACK_BY_TOPIC = {
  "滑动窗口": "interview",
  "前缀和": "interview",
}

/**
 * 按 `// leetcode <题号>` 标记把一个区间文件切成若干段。
 * 每段保留自己的标记注释；第一段额外带上文件头的 #include。
 * @param {string} text
 * @returns {Array<{id: number, rest: string, code: string}>}
 */
function splitByMarker(text) {
  const src = String(text === undefined || text === null ? "" : text)
  const re = new RegExp(MARKER_PATTERN.source, "gm")
  const marks = []
  let m
  while ((m = re.exec(src))) {
    marks.push({
      id: Number(m[1]),
      rest: String(m[2] || "").trim(),
      start: m.index,
    })
  }
  const out = []
  marks.forEach(function (mark, i) {
    const end = i + 1 < marks.length ? marks[i + 1].start : src.length
    let code = src.slice(mark.start, end).replace(/\s+$/, "")
    if (i === 0) {
      // 文件头的 #include 属于第一段
      const head = src.slice(0, mark.start).split(/\r?\n/)
      const includes = head.filter(function (line) {
        return /^[ \t]*#include/.test(line)
      })
      if (includes.length > 0) code = includes.join("\n") + "\n\n" + code
    }
    out.push({ id: mark.id, rest: mark.rest, code: code })
  })
  return out
}

/**
 * 从文件名取题号。不是 leetcode_<题号>.<ext> 就返回 null。
 * @param {string} name
 * @returns {number|null}
 */
function idFromFileName(name) {
  const m = FILENAME_PATTERN.exec(String(name === undefined || name === null ? "" : name))
  return m ? Number(m[1]) : null
}

/**
 * 语言：.c → c，.cpp → cpp。
 * @param {string} filePath
 * @returns {string}
 */
function langOf(filePath) {
  const m = /\.(c|cpp)$/i.exec(String(filePath === undefined || filePath === null ? "" : filePath))
  return m ? m[1].toLowerCase() : ""
}

/**
 * 从路径里认出 algo-c 的专题目录。
 * 规则：路径里有 "problems" 目录，它的上一级目录名若在 SOLUTION_TOPIC_BY_DIR 里，就是专题。
 * @param {string} filePath
 * @returns {string} 标签；认不出返回空串
 */
function topicOfSolutionFile(filePath) {
  const parts = String(filePath === undefined || filePath === null ? "" : filePath).split(/[\\/]+/)
  const idx = parts.lastIndexOf("problems")
  if (idx <= 0) return ""
  const dir = parts[idx - 1]
  if (Object.prototype.hasOwnProperty.call(SOLUTION_TOPIC_BY_DIR, dir)) {
    return SOLUTION_TOPIC_BY_DIR[dir]
  }
  return ""
}

/**
 * 标签 → 轨道。认不出的标签落到默认轨道。
 * @param {string} topic
 * @returns {string}
 */
function trackOfSolutionTopic(topic) {
  const key = String(topic === undefined || topic === null ? "" : topic)
  if (Object.prototype.hasOwnProperty.call(SOLUTION_TRACK_BY_TOPIC, key)) {
    return SOLUTION_TRACK_BY_TOPIC[key]
  }
  return DEFAULT_SOLUTION_TRACK
}

/**
 * 解析 README 里的「题号 / 题目 / 难度」表。
 * @param {Array<{path: string, text: string}>} files
 * @returns {{byId: Object, warnings: string[]}}
 */
function parseSolutionIndex(files) {
  const byId = Object.create(null)
  const warnings = []
  ;(files || []).forEach(function (file) {
    const text = String((file && file.text) || "")
    const path = String((file && file.path) || "")
    text.split(/\r?\n/).forEach(function (line) {
      const m = /^\|\s*(\d+)\s*\|\s*(?:\[([^\]]*)\]\([^)]*\)|([^|]*?))\s*\|\s*([^|]+?)\s*\|/.exec(line)
      if (!m) return
      const id = Number(m[1])
      const title = String(m[2] !== undefined ? m[2] : m[3]).trim()
      const cn = String(m[4]).trim()
      const difficulty = DIFFICULTY_BY_CN[cn]
      if (!difficulty) {
        warnings.push("难度写法不认识：" + cn + "（" + path + "）")
        return
      }
      if (byId[id]) {
        warnings.push("题号在索引里重复：" + id + "（" + path + "）")
        return
      }
      byId[id] = { title: title, difficulty: difficulty }
    })
  })
  return { byId: byId, warnings: warnings }
}

/**
 * 扫描题解文件 → 题目条目（契约 §1 钉死的形状）。
 * @param {Array<{path: string, text: string}>} files
 * @param {Object} [index] parseSolutionIndex 产出的 byId，用来取中文题名
 * @returns {{items: Array<{source, sourceId, title, lang, code, file}>, warnings: string[]}}
 */
function parseSolutionFiles(files, index) {
  const items = []
  const warnings = []
  const byId = index || Object.create(null)
  ;(files || []).forEach(function (file) {
    const path = String((file && file.path) || "")
    const text = String((file && file.text) || "")
    const base = path.split(/[\\/]+/).pop()

    const single = FILENAME_PATTERN.exec(base)
    if (single) {
      const id = Number(single[1])
      const meta = byId[id] ? byId[id] : null
      items.push({
        source: "leetcode",
        sourceId: String(id),
        title: meta ? meta.title : "LeetCode " + id,
        lang: single[2].toLowerCase(),
        code: text.replace(/\s+$/, ""),
        file: path,
      })
      return
    }

    if (RANGE_FILENAME_PATTERN.test(base)) {
      const blocks = splitByMarker(text)
      if (blocks.length === 0) {
        warnings.push("区间文件里没有找到 // leetcode 标记，已跳过：" + path)
        return
      }
      blocks.forEach(function (block) {
        const meta = byId[block.id] ? byId[block.id] : null
        const baseTitle = meta ? meta.title : "LeetCode " + block.id
        items.push({
          source: "leetcode",
          sourceId: String(block.id),
          title: block.rest ? baseTitle + "（" + block.rest + "）" : baseTitle,
          lang: langOf(path),
          code: block.code,
          file: path,
        })
      })
      return
    }

    warnings.push("文件名不符合 leetcode_<题号> 约定，已跳过：" + path)
  })
  return { items: items, warnings: warnings }
}

/**
 * 题解条目 → 稳定 ID（用 M1 的 makeId）。
 * @param {{source: string, sourceId: string}} item
 * @returns {string}
 */
function solutionIdOf(item) {
  if (!item) throw new Error("条目为空")
  return makeId(String(item.source || ""), Number(item.sourceId))
}

module.exports = {
  MARKER_PATTERN: MARKER_PATTERN,
  FILENAME_PATTERN: FILENAME_PATTERN,
  RANGE_FILENAME_PATTERN: RANGE_FILENAME_PATTERN,
  DIFFICULTY_BY_CN: DIFFICULTY_BY_CN,
  SOLUTION_TOPIC_BY_DIR: SOLUTION_TOPIC_BY_DIR,
  DEFAULT_SOLUTION_TOPIC: DEFAULT_SOLUTION_TOPIC,
  DEFAULT_SOLUTION_TRACK: DEFAULT_SOLUTION_TRACK,
  SOLUTION_TRACK_BY_TOPIC: SOLUTION_TRACK_BY_TOPIC,
  splitByMarker: splitByMarker,
  idFromFileName: idFromFileName,
  langOf: langOf,
  topicOfSolutionFile: topicOfSolutionFile,
  trackOfSolutionTopic: trackOfSolutionTopic,
  parseSolutionIndex: parseSolutionIndex,
  parseSolutionFiles: parseSolutionFiles,
  solutionIdOf: solutionIdOf,
}
