"use strict"
/**
 * parse.js —— markdown ↔ Problem 双向转换
 *
 * 纯函数，无 IO。题库文件格式见设计文档 §3.2：
 *   frontmatter（id/source/source_id/title/difficulty/topics/tracks/status/needs_fill/url/search_url/created/updated）
 *   + 「## 题面」「## 测试用例」「## 参考解法」三个正文小节。
 *
 * frontmatter 用手写的 mini-YAML 解析器——零依赖是硬约束（设计文档 §1.9）。
 */

const { normalizeTopics, normalizeTracks, today } = require("./problem.js")
const { encodeCell, decodeCell, splitRow } = require("./case-codec.js")

/** 已知的正文小节标题 */
const SECTION_STATEMENT = "题面"
const SECTION_CASES = "测试用例"
const SECTION_SOLUTION = "参考解法"
const KNOWN_SECTIONS = [SECTION_STATEMENT, SECTION_CASES, SECTION_SOLUTION]

/**
 * 去掉一层引号。
 */
function unquote(value) {
  const text = String(value).trim()
  if (text.length >= 2) {
    const first = text.charAt(0)
    const last = text.charAt(text.length - 1)
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      return text.slice(1, -1)
    }
  }
  return text
}

/**
 * 解析 frontmatter 片段（不含首尾 --- 分隔线）。
 *
 * 支持：key: value、key: [a, b]、key: []、块列表（- item 形式）、引号包裹的值。
 * 不支持：嵌套 map、多行标量（| 与 >）——题库格式用不上。
 *
 * @param {string} text
 * @returns {object}
 */
function parseFrontmatter(text) {
  const lines = String(text || "").split(/\r?\n/)
  const out = {}
  let i = 0
  while (i < lines.length) {
    const line = lines[i]
    const trimmed = line.trim()
    if (trimmed === "" || trimmed.charAt(0) === "#") {
      i += 1
      continue
    }
    const m = /^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$/.exec(line)
    if (!m) {
      i += 1
      continue
    }
    const key = m[1]
    const raw = m[2].trim()
    if (raw === "") {
      // 块列表：下面连续的 "- xxx"
      const items = []
      i += 1
      while (i < lines.length && /^\s*-\s+/.test(lines[i])) {
        items.push(unquote(lines[i].replace(/^\s*-\s+/, "")))
        i += 1
      }
      out[key] = items
      continue
    }
    if (raw.charAt(0) === "[" && raw.charAt(raw.length - 1) === "]") {
      const inner = raw.slice(1, -1).trim()
      if (inner === "") {
        out[key] = []
      } else {
        out[key] = inner.split(",").map(function (item) {
          return unquote(item)
        })
      }
      i += 1
      continue
    }
    out[key] = unquote(raw)
    i += 1
  }
  return out
}

/**
 * 序列化 frontmatter（不含首尾 --- 分隔线）。
 * @param {object} obj
 * @returns {string}
 */
function serializeFrontmatter(obj) {
  const lines = []
  Object.keys(obj).forEach(function (key) {
    const value = obj[key]
    if (Array.isArray(value)) {
      lines.push(key + ": [" + value.join(", ") + "]")
    } else if (typeof value === "boolean") {
      lines.push(key + ": " + (value ? "true" : "false"))
    } else {
      lines.push(key + ": " + value)
    }
  })
  return lines.join("\n")
}

/**
 * 按「## 标题」切分正文。
 * @param {string} body
 * @returns {Object<string,string>} 标题 → 内容
 */
function splitSections(body) {
  const sections = {}
  let current = ""
  let buf = []
  String(body || "")
    .split(/\r?\n/)
    .forEach(function (line) {
      const m = /^##\s+(.*)$/.exec(line)
      if (m) {
        if (current !== "") sections[current] = buf.join("\n")
        current = m[1].trim()
        buf = []
      } else {
        buf.push(line)
      }
    })
  if (current !== "") sections[current] = buf.join("\n")
  return sections
}

/**
 * 解析「## 测试用例」里的 markdown 表格。
 *
 * 表头形如 `| input | expect |`，数据行第三格可带 `tol=1e-6` 容差标记。
 * 认不出列的行直接跳过，不报错。
 *
 * 单元格里的换行写成 <br>、竖线写成 \|（见 case-codec.js），
 * 所以 Codeforces 那种多行样例也能塞进一行表格里。
 *
 * @param {string} text
 * @returns {Array<{input: string, expect: string, tol: number|null}>}
 */
function parseCases(text) {
  const cases = []
  let headerSeen = false
  String(text || "")
    .split(/\r?\n/)
    .forEach(function (line) {
      const trimmed = line.trim()
      if (trimmed.charAt(0) !== "|") return
      const body = trimmed.charAt(trimmed.length - 1) === "|" ? trimmed.slice(1, -1) : trimmed.slice(1)
      const cells = splitRow(body)
      if (!headerSeen) {
        headerSeen = true
        return
      }
      // 分隔行 |---|---| 跳过
      if (
        cells.every(function (cell) {
          return cell === "" || /^-+:?$/.test(cell)
        })
      ) {
        return
      }
      if (cells.length < 2) return
      let tol = null
      if (cells.length >= 3) {
        const m = /tol\s*=\s*([0-9.eE+-]+)/.exec(cells[2])
        if (m) {
          const value = Number(m[1])
          if (!Number.isNaN(value)) tol = value
        }
      }
      cases.push({ input: decodeCell(cells[0]), expect: decodeCell(cells[1]), tol: tol })
    })
  return cases
}

/**
 * 解析「## 参考解法」里的代码块。
 * @param {string} text
 * @returns {{code: string, lang: string}}
 */
function parseSolution(text) {
  const fence = /```([A-Za-z0-9+#_-]*)\n([\s\S]*?)```/.exec(String(text || ""))
  if (!fence) return { code: "", lang: "" }
  return {
    code: fence[2].replace(/\n+$/, ""),
    lang: fence[1] || "",
  }
}

/**
 * markdown → Problem。
 * @param {string} text
 * @returns {{problem: object|null, warnings: string[]}}
 */
function parseProblemMarkdown(text) {
  const warnings = []
  const src = String(text || "").replace(/\r\n/g, "\n")

  let body = src
  let fm = {}
  const match = /^---\n([\s\S]*?)\n---\n?/.exec(src)
  if (match) {
    fm = parseFrontmatter(match[1])
    body = src.slice(match[0].length)
  } else {
    warnings.push("缺少 frontmatter")
  }

  const sections = splitSections(body)
  const extraSections = []
  Object.keys(sections).forEach(function (title) {
    if (KNOWN_SECTIONS.indexOf(title) === -1) {
      extraSections.push({ title: title, content: sections[title].trim() })
    }
  })

  const needsFillRaw = fm.needs_fill
  const solution = parseSolution(sections[SECTION_SOLUTION] || "")
  const problem = {
    id: fm.id === undefined ? "" : String(fm.id),
    source: fm.source === undefined ? "" : String(fm.source),
    sourceId: fm.source_id === undefined ? "" : String(fm.source_id),
    title: fm.title === undefined ? "" : String(fm.title),
    difficulty: fm.difficulty === undefined ? "medium" : String(fm.difficulty),
    topics: normalizeTopics(fm.topics),
    tracks: normalizeTracks(fm.tracks),
    status: fm.status === undefined ? "todo" : String(fm.status),
    needsFill: needsFillRaw === true || needsFillRaw === "true",
    created: fm.created === undefined ? today() : String(fm.created),
    updated: fm.updated === undefined ? today() : String(fm.updated),
    url: fm.url === undefined ? "" : String(fm.url),
    searchUrl: fm.search_url === undefined ? "" : String(fm.search_url),
    statement: (sections[SECTION_STATEMENT] || "").trim(),
    cases: parseCases(sections[SECTION_CASES] || ""),
    solution: solution.code,
    solutionLang: solution.lang,
    extraSections: extraSections,
  }

  if (!fm.id) warnings.push("frontmatter 缺 id")
  if (!fm.title) warnings.push("frontmatter 缺 title")

  return { problem: problem, warnings: warnings }
}

/**
 * Problem → markdown。
 * @param {object} problem
 * @returns {string}
 */
function serializeProblemMarkdown(problem) {
  // 按固定顺序拼 frontmatter；source_id 与 url 为空就整行省掉——
  // 空值写进去会被 mini-YAML 解析成块列表（空数组），文件里也平白多两行噪声。
  const pairs = [
    ["id", problem.id],
    ["source", problem.source],
    ["source_id", problem.sourceId],
    ["title", problem.title],
    ["difficulty", problem.difficulty],
    ["topics", problem.topics],
    ["tracks", problem.tracks],
    ["status", problem.status],
    ["needs_fill", problem.needsFill ? "true" : "false"],
    ["url", problem.url],
    ["search_url", problem.searchUrl],
    ["created", problem.created],
    ["updated", problem.updated],
  ]
  const fields = {}
  pairs.forEach(function (pair) {
    const value = pair[1]
    if (value === undefined || value === null) return
    if (typeof value === "string" && value === "") return
    fields[pair[0]] = value
  })
  const frontmatter = serializeFrontmatter(fields)

  const lines = []
  lines.push("---")
  lines.push(frontmatter)
  lines.push("---")
  lines.push("")
  lines.push("## " + SECTION_STATEMENT)
  lines.push("")
  lines.push(problem.statement || "")
  lines.push("")
  lines.push("## " + SECTION_CASES)
  lines.push("")
  lines.push("| input | expect |")
  lines.push("|---|---|")
  ;(problem.cases || []).forEach(function (item) {
    const tol = item.tol === null || item.tol === undefined ? "" : " tol=" + item.tol
    lines.push("| " + encodeCell(item.input) + " | " + encodeCell(item.expect) + " |" + tol)
  })
  lines.push("")
  lines.push("## " + SECTION_SOLUTION)
  lines.push("")
  if (problem.solution) {
    lines.push("```" + (problem.solutionLang || "cpp"))
    lines.push(problem.solution)
    lines.push("```")
  }
  lines.push("")

  ;(problem.extraSections || []).forEach(function (section) {
    lines.push("## " + section.title)
    lines.push("")
    lines.push(section.content || "")
    lines.push("")
  })

  return lines.join("\n")
}

module.exports = {
  SECTION_STATEMENT: SECTION_STATEMENT,
  SECTION_CASES: SECTION_CASES,
  SECTION_SOLUTION: SECTION_SOLUTION,
  parseFrontmatter: parseFrontmatter,
  serializeFrontmatter: serializeFrontmatter,
  splitSections: splitSections,
  parseCases: parseCases,
  parseSolution: parseSolution,
  parseProblemMarkdown: parseProblemMarkdown,
  serializeProblemMarkdown: serializeProblemMarkdown,
}
