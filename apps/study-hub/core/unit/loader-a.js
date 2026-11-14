"use strict"
/**
 * loader-a.js —— 算法题 md → Unit
 *
 * 读 learning/bank 下的 md 与工程仓 bank 下的 md，产出与 DB/AI 卡**完全同构**的 Unit。
 * 契约 §3.1 冻结了本文件的行为。
 *
 * 铁律：**只读**。存量题库文件一个字都不改（G32）——id 的规范化只发生在内存层（G33）。
 */

const { parseProblemMarkdown } = require("../bank/parse")
// Unit 的枚举只有 core/unit/unit.js 一份（G41）：这里引用，不另抄。
const { createUnit, PROGRESSES, DIFFICULTIES } = require("./unit")

/** 来源白名单；不在表内的一律记 unknown。 */
const SOURCE_WHITELIST = [
  "leetcode",
  "nowcoder",
  "self",
  "grokking",
  "scaffold",
  "generated",
  "codeforces",
]

/**
 * 按内容判定单元状态（契约 §1.4 的 loader A 规则）。
 *
 * 刻意**不看** frontmatter 的 needs_fill —— 那个标志在存量里已经过时：
 * 51 道 CF 原题的 needs_fill 仍是 true，可题面与用例其实都齐。
 *
 * @param {object} problem
 * @returns {"todo"|"draft"|"verified"}
 */
function statusOf(problem) {
  const hasStatement = typeof problem.statement === "string" && problem.statement.trim() !== ""
  const hasCases = Array.isArray(problem.cases) && problem.cases.length > 0
  if (!hasStatement) return "todo"
  if (!hasCases) return "draft"
  return "verified"
}

/**
 * 来源归一化：不在白名单里 → unknown。
 * @param {*} raw
 * @returns {string}
 */
function normalizeSource(raw) {
  const value = String(raw === undefined || raw === null ? "" : raw).trim()
  return SOURCE_WHITELIST.indexOf(value) !== -1 ? value : "unknown"
}

/**
 * 学习进度归一化：不在枚举里 → todo。
 * @param {*} raw
 * @returns {string}
 */
function progressOf(raw) {
  const value = String(raw === undefined || raw === null ? "" : raw).trim()
  return PROGRESSES.indexOf(value) !== -1 ? value : "todo"
}

/**
 * 难度归一化：不在枚举里 → medium。
 * @param {*} raw
 * @returns {string}
 */
function difficultyOf(raw) {
  const value = String(raw === undefined || raw === null ? "" : raw).trim()
  return DIFFICULTIES.indexOf(value) !== -1 ? value : "medium"
}

/**
 * 解析一份算法题 md，产出 Unit。
 *
 * @param {string} text 文件全文
 * @param {object} opts { track?: "algorithm", relPath: string }
 * @returns {{unit: object|null, warnings: string[]}}
 */
function parseUnitMarkdown(text, opts) {
  const options = opts || {}
  const warnings = []

  const parsed = parseProblemMarkdown(text)
  ;(parsed.warnings || []).forEach(function (w) {
    warnings.push(w)
  })

  const problem = parsed.problem
  if (!problem || !problem.id) {
    warnings.push("算法题解析不出 id，跳过")
    return { unit: null, warnings: warnings }
  }

  // G42：Unit.path 必填。调用方必须给出文件相对内容根的路径（loader 不自己拼路径），
  // 缺了就只给一条警告，不产出单元——空的 path 会让下游找不到文件。
  if (!String(options.relPath || "").trim()) {
    warnings.push("算法题 " + problem.id + " 缺少 relPath，无法产出单元")
    return { unit: null, warnings: warnings }
  }

  // 用例 → output 断言。tol 在 v1 一律丢弃（契约 §3.1.1）。
  const asserts = []
  ;(problem.cases || []).forEach(function (item, index) {
    if (item.tol !== null && item.tol !== undefined) {
      warnings.push("用例 " + (index + 1) + " 带 tol 参数，v1 按精确比对处理")
    }
    asserts.push({
      type: "output",
      input: item.input === undefined || item.input === null ? "" : String(item.input),
      expect: item.expect === undefined || item.expect === null ? "" : String(item.expect),
      cmp: "exact",
    })
  })

  // 契约 §3.1：「unit 为 null 当且仅当解析不出 id」；契约 §3.4：「任何单个文件解析失败只进 warnings，不中断整体加载」。
  // 所以 createUnit 抛错（id 非法等）时，**这一份题被跳过**，而不是让整个 app 起不来。
  let unit = null
  try {
    unit = createUnit({
      id: "algo:" + problem.id,
      track: "algorithm",
      kind: "problem",
      title: problem.title || problem.id,
      statement: problem.statement || "",
      entry: {
        demo: "",
        reference: "",
        graded: "user",
        script: "",
        language: "cpp",
      },
      asserts: asserts,
      topics: problem.topics || [],
      difficulty: difficultyOf(problem.difficulty),
      source: normalizeSource(problem.source),
      status: statusOf(problem),
      progress: progressOf(problem.status),
      reviewed: false,
      reviewed_at: "",
      subtracks: problem.tracks || [],
      path: options.relPath === undefined || options.relPath === null ? "" : String(options.relPath),
    })
  } catch (err) {
    warnings.push("算法题 " + problem.id + " 解析失败：" + (err && err.message ? err.message : String(err)))
    return { unit: null, warnings: warnings }
  }

  return { unit: unit, warnings: warnings }
}

module.exports = {
  SOURCE_WHITELIST: SOURCE_WHITELIST,
  statusOf: statusOf,
  parseUnitMarkdown: parseUnitMarkdown,
}
