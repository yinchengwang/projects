"use strict"
/**
 * algorithm-fill.js —— 算法轨生成侧：扫题库判缺口、渲染四件套、落盘三件套、只改 status 一行
 *
 * 本模块**只读** learning/bank/** 与工程仓 bank/**，**只写** learning/units/algorithm/（P5-1）。
 * 存量题库一个字节都不许动（G14 / P5-2）。
 *
 * 「缺什么」按**内容**判定，不看 frontmatter 的 needs_fill 标志（契约 §1.4：
 * 那个标志在存量里已经过时——51 道 CF 原题的 needs_fill 还是 true，但题面与用例其实都齐）。
 *
 * 四种形态：
 *   A 补全型：题面空（用例也空）        → LLM 产全套四件套
 *   B 补用例型：题面非空、用例空        → 题面原样保留，LLM 产用例 + 解法 + 负例
 *   C 补解型：题面非空、用例非空、解法空 → 题面与用例原样保留，LLM 只产解法 + 负例
 *   ready 已齐备：一个字都不改
 */

const fs = require("node:fs")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const { parseFrontmatter, splitSections, parseCases, parseSolution } = require("../bank/parse.js")
const { encodeCell } = require("../bank/case-codec.js")
const { normalizeTopics, normalizeTracks, DIFFICULTIES } = require("../bank/problem.js")

/** 题库里的小节名（题库文件与单元文件同名，沿用 parse.js 的叫法） */
const SECTION_STATEMENT = "题面"
const SECTION_CASES = "测试用例"
const SECTION_SOLUTION = "参考解法"

/** 四种形态 */
const SHAPES = ["A", "B", "C", "ready"]

/** 可能缺的三样 */
const MISSING_KEYS = ["statement", "cases", "solution"]

/** 产出目录（相对 learning 仓根） */
const UNITS_REL_DIR = "units/algorithm/"

/** 契约 §1.2 允许的 Unit.source 取值域 */
const UNIT_SOURCES = [
  "leetcode",
  "nowcoder",
  "self",
  "grokking",
  "scaffold",
  "generated",
  "codeforces",
  "unknown",
]

/** 题库里的 source 名 → Unit.source 的归一（存量题库用 "jz" 表示牛客） */
const SOURCE_ALIAS = { jz: "nowcoder" }

/**
 * C++ 源码归一：统一 LF、去掉末尾空行。
 *
 * md 里的 `## 参考解法` 代码块与 `<stem>.reference.cpp` **用同一份归一结果**，
 * 于是「两者逐字节一致」这件事可以直接比较（P5-6 / 契约 §3.3.1）。
 */
function canonicalCode(code) {
  return String(code === null || code === undefined ? "" : code)
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "\n")
    .replace(/\n+$/, "")
}

/**
 * 题面里的二级标题降一级（`## 输入` → `### 输入`）。
 *
 * loader C 用 `splitSections` 按 `^## ` 切正文，题面里留着二级标题会把题面截断。
 * 围栏代码块**不动**（里面的 `##` 是代码，不是标题）。
 */
function demoteHeadings(text) {
  const lines = String(text === null || text === undefined ? "" : text).split(/\r?\n/)
  let inFence = false
  return lines
    .map(function (line) {
      const trimmed = line.trim()
      if (trimmed.charAt(0) === "`" && trimmed.indexOf("```") === 0) {
        inFence = !inFence
        return line
      }
      if (inFence) return line
      if (/^##\s+\S/.test(line)) return "#" + line
      return line
    })
    .join("\n")
}

/** 把值压成一行（frontmatter 的值不能换行） */
function oneLine(text) {
  return String(text === null || text === undefined ? "" : text)
    .replace(/\r\n/g, "\n")
    .replace(/\n/g, " ")
    .trim()
}

/**
 * 列表项里的逗号会破坏 mini-YAML 的行内数组（`[a, b]` 靠逗号切分），
 * 所以写进 frontmatter 之前把半角逗号换成全角顿号。
 */
function safeListItem(text) {
  return oneLine(text).replace(/,/g, "、")
}

/**
 * 读一份题库 md，判出它属于哪种形态、缺什么。
 *
 * @param {string} absPath 题库文件的绝对路径
 * @param {"learning"|"local"} origin 来自 learning 仓还是工程仓
 * @param {string} contentRoot 相对路径的基准目录（learning 仓根 / 工程仓根）
 * @returns {object}
 */
function inspectBankFile(absPath, origin, contentRoot) {
  const rel = path.relative(contentRoot, absPath).split(path.sep).join("/")
  let text = ""
  try {
    text = fs.readFileSync(absPath, "utf8")
  } catch (err) {
    return { ok: false, stem: "", origin: origin, sourcePath: rel, warnings: ["读不到题库文件：" + rel] }
  }
  const src = text.replace(/\r\n/g, "\n")
  const fmMatch = /^---\n([\s\S]*?)\n---\n?/.exec(src)
  const fm = fmMatch ? parseFrontmatter(fmMatch[1]) : {}
  const body = fmMatch ? src.slice(fmMatch[0].length) : src
  const sections = splitSections(body)

  const statement = String(sections[SECTION_STATEMENT] || "").trim()
  const cases = parseCases(sections[SECTION_CASES] || "")
  const solution = parseSolution(sections[SECTION_SOLUTION] || "").code.trim()

  const hasStatement = statement !== ""
  const hasCases = cases.length > 0
  const hasSolution = solution !== ""

  let shape = "ready"
  if (!hasStatement) shape = "A"
  else if (!hasCases) shape = "B"
  else if (!hasSolution) shape = "C"

  const missing = []
  if (!hasStatement) missing.push("statement")
  if (!hasCases) missing.push("cases")
  if (!hasSolution) missing.push("solution")

  const stem = path.basename(absPath).replace(/\.md$/, "")
  const difficulty = DIFFICULTIES.indexOf(String(fm.difficulty)) !== -1 ? String(fm.difficulty) : "medium"
  const rawSource = oneLine(fm.source || "unknown")
  const aliased = SOURCE_ALIAS[rawSource] || rawSource
  const unitSource = UNIT_SOURCES.indexOf(aliased) !== -1 ? aliased : "unknown"

  return {
    ok: true,
    stem: stem,
    origin: origin,
    sourcePath: rel,
    unitId: "algo:" + stem,
    title: oneLine(fm.title || stem),
    difficulty: difficulty,
    topics: normalizeTopics(fm.topics),
    subtracks: normalizeTracks(fm.tracks),
    source: rawSource,
    unitSource: unitSource,
    sourceId: oneLine(fm.source_id === undefined ? "" : fm.source_id),
    url: oneLine(fm.url === undefined ? "" : fm.url),
    shape: shape,
    missing: missing,
    hasStatement: hasStatement,
    hasCases: hasCases,
    hasSolution: hasSolution,
    statement: statement,
    cases: cases.map(function (c) {
      return { input: c.input, expect: c.expect, tol: c.tol }
    }),
    solution: solution,
    warnings: [],
  }
}

/**
 * 递归列出目录下所有 .md。
 * @param {string} dir
 * @param {string} contentRoot
 * @param {"learning"|"local"} origin
 * @returns {Array<object>}
 */
function listBankFiles(dir, contentRoot, origin) {
  const acc = []
  let entries = []
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch (err) {
    return acc
  }
  entries.sort(function (a, b) {
    return a.name < b.name ? -1 : a.name > b.name ? 1 : 0
  })
  entries.forEach(function (entry) {
    const abs = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      acc.push.apply(acc, listBankFiles(abs, contentRoot, origin))
      return
    }
    if (!entry.isFile()) return
    if (!/\.md$/i.test(entry.name)) return
    const item = inspectBankFile(abs, origin, contentRoot)
    if (item.ok) acc.push(item)
  })
  return acc
}

/**
 * 统计：按形态、来源、难度、子轨、知识点、缺什么。
 * @param {Array<object>} items
 * @returns {object}
 */
function buildStats(items) {
  const stats = {
    total: items.length,
    byShape: { A: 0, B: 0, C: 0, ready: 0 },
    bySource: {},
    byDifficulty: {},
    bySubtrack: {},
    byTopic: {},
    byOrigin: { learning: 0, local: 0 },
    missing: { statement: 0, cases: 0, solution: 0 },
  }
  items.forEach(function (item) {
    stats.byShape[item.shape] += 1
    stats.bySource[item.source] = (stats.bySource[item.source] || 0) + 1
    stats.byDifficulty[item.difficulty] = (stats.byDifficulty[item.difficulty] || 0) + 1
    stats.byOrigin[item.origin] = (stats.byOrigin[item.origin] || 0) + 1
    item.subtracks.forEach(function (t) {
      stats.bySubtrack[t] = (stats.bySubtrack[t] || 0) + 1
    })
    item.topics.forEach(function (t) {
      stats.byTopic[t] = (stats.byTopic[t] || 0) + 1
    })
    item.missing.forEach(function (m) {
      stats.missing[m] += 1
    })
  })
  return stats
}

/**
 * 扫两个题库，产出缺口清单与统计。
 *
 * @param {{learningRoot?: string|null, localBank?: string|null, localContentRoot?: string|null}} opts
 * @returns {{items: Array<object>, stats: object, warnings: string[]}}
 */
function scanBank(opts) {
  const o = opts || {}
  const warnings = []
  const items = []

  if (o.learningRoot) {
    const dir = path.join(o.learningRoot, "bank")
    if (fs.existsSync(dir)) {
      items.push.apply(items, listBankFiles(dir, o.learningRoot, "learning"))
    } else {
      warnings.push("learning 仓里没有 bank 目录：" + dir)
    }
  }
  if (o.localBank && fs.existsSync(o.localBank)) {
    const localContentRoot = o.localContentRoot || path.dirname(o.localBank)
    items.push.apply(items, listBankFiles(o.localBank, localContentRoot, "local"))
  }

  // 同一个 stem 在两处都有时，先到先得（learning 优先），后者丢弃
  const seen = Object.create(null)
  const unique = []
  items.forEach(function (item) {
    if (seen[item.stem]) {
      warnings.push("题号重复，保留先出现的：" + item.stem + "（跳过 " + item.sourcePath + "）")
      return
    }
    seen[item.stem] = true
    unique.push(item)
  })
  unique.sort(function (a, b) {
    return a.stem < b.stem ? -1 : a.stem > b.stem ? 1 : 0
  })
  return { items: unique, stats: buildStats(unique), warnings: warnings }
}

/**
 * 渲染单元 md（契约 §3.3 + §3.3.1）。
 *
 * frontmatter 字段与 §3.3 的清单一一对应，另加 §3.3.1 允许的 negative / source_id / url。
 * 负例的**内容绝不进 md**（§10.1 冻结）——学习者只看得到正确解法。
 *
 * @param {object} draft
 * @returns {string}
 */
function renderUnitMarkdown(draft) {
  const d = draft || {}
  const stem = oneLine(d.stem || "")
  const topics = (d.topics || []).map(safeListItem).filter(function (t) { return t !== "" })
  const subtracks = (d.subtracks || []).map(safeListItem).filter(function (t) { return t !== "" })
  const lines = []

  lines.push("---")
  lines.push("id: algo:" + stem)
  lines.push("track: algorithm")
  lines.push("kind: problem")
  lines.push("title: " + oneLine(d.title || stem))
  lines.push("difficulty: " + (DIFFICULTIES.indexOf(String(d.difficulty)) !== -1 ? String(d.difficulty) : "medium"))
  lines.push("topics: [" + topics.join(", ") + "]")
  lines.push("source: " + (d.unitSource ? oneLine(d.unitSource) : d.source ? oneLine(d.source) : "generated"))
  if (d.sourceId) lines.push("source_id: " + oneLine(d.sourceId))
  if (d.url) lines.push("url: " + oneLine(d.url))
  lines.push("subtracks: [" + subtracks.join(", ") + "]")
  lines.push("progress: todo")
  lines.push("graded: user")
  lines.push("reference: " + UNITS_REL_DIR + stem + ".reference.cpp")
  lines.push("negative: " + UNITS_REL_DIR + stem + ".negative.cpp")
  lines.push("status: " + (d.status ? oneLine(d.status) : "draft"))
  lines.push("reviewed: false")
  lines.push('reviewed_at: ""')
  lines.push("---")
  lines.push("")
  lines.push("## " + SECTION_STATEMENT)
  lines.push("")
  lines.push(demoteHeadings(String(d.statement === undefined || d.statement === null ? "" : d.statement).trim()))
  lines.push("")
  lines.push("## " + SECTION_CASES)
  lines.push("")
  lines.push("| input | expect |")
  lines.push("|---|---|")
  ;(d.cases || []).forEach(function (c) {
    lines.push(
      "| " +
        encodeCell(String(c.input === undefined ? "" : c.input)) +
        " | " +
        encodeCell(String(c.expect === undefined ? "" : c.expect)) +
        " |"
    )
  })
  lines.push("")
  lines.push("## " + SECTION_SOLUTION)
  lines.push("")
  lines.push("```cpp")
  lines.push(canonicalCode(d.solution))
  lines.push("```")
  lines.push("")
  lines.push("## 关联")
  lines.push("")
  lines.push("- 原始题号：" + (d.sourceId ? oneLine(d.sourceId) : "无"))
  lines.push("- 来源平台：" + (d.source ? oneLine(d.source) : "unknown"))
  lines.push("- 原题链接：" + (d.url ? oneLine(d.url) : "无"))
  if (d.originPath) lines.push("- 原题库文件：" + oneLine(d.originPath))
  lines.push("")
  return lines.join("\n")
}

/**
 * 落盘三件套：md + reference.cpp + negative.cpp。
 *
 * @param {{unitsDir: string, stem: string, draft: object}} opts
 * @returns {{files: string[]}} 落盘的绝对路径
 */
function writeTriplet(opts) {
  const o = opts || {}
  const unitsDir = String(o.unitsDir || "")
  const stem = oneLine(o.stem || "")
  if (!unitsDir) throw new Error("writeTriplet 缺 unitsDir")
  if (!stem) throw new Error("writeTriplet 缺 stem")
  fs.mkdirSync(unitsDir, { recursive: true })
  const mdPath = path.join(unitsDir, stem + ".md")
  const refPath = path.join(unitsDir, stem + ".reference.cpp")
  const negPath = path.join(unitsDir, stem + ".negative.cpp")
  fs.writeFileSync(mdPath, renderUnitMarkdown(o.draft), "utf8")
  fs.writeFileSync(refPath, canonicalCode(o.draft.solution) + "\n", "utf8")
  fs.writeFileSync(negPath, canonicalCode(o.draft.negative) + "\n", "utf8")
  return { files: [mdPath, refPath, negPath] }
}

/**
 * 从单元 md 里抠出 `## 参考解法` 的代码块（归一后）。
 * @param {string} text
 * @returns {string}
 */
function extractSolutionBlock(text) {
  const sections = splitSections(String(text || ""))
  return canonicalCode(parseSolution(sections[SECTION_SOLUTION] || "").code)
}

/**
 * 只改 frontmatter 里 status 那一行，其余**逐字节**不变（G15 / P5-10）。
 *
 * 连行尾风格（CRLF / LF）都原样保留；frontmatter 里没有 status 行时补一行。
 *
 * @param {string} text
 * @param {string} status
 * @returns {string}
 */
function patchStatusLine(text, status) {
  const src = String(text === null || text === undefined ? "" : text)
  const fm = /^---[ \t]*\r?\n([\s\S]*?)\r?\n---[ \t]*(?:\r?\n|$)/.exec(src)
  if (!fm) throw new Error("单元文件缺少 frontmatter，改不了 status")
  // fm[0] 从开头的 --- 一直包到收尾的 ---（含它后面的换行）。
  // 所以「frontmatter 正文在整份文件里的起止」要自己算：
  //   起点 = fm[0] 里第一条换行之后
  //   终点 = fm.index + fm[0].length - 收尾 --- 及其换行的长度 - 前面那条换行
  const openLen = fm[0].indexOf("\n") + 1
  const bodyStart = fm.index + openLen
  const bodyEnd = bodyStart + fm[1].length
  const head = src.slice(0, bodyStart)
  const body = fm[1]
  const tail = src.slice(bodyEnd)
  const re = /(^|\r?\n)([ \t]*status[ \t]*:[ \t]*)([^\r\n]*)/
  if (re.test(body)) {
    const newBody = body.replace(re, function (all, pre, key) {
      return pre + key + status
    })
    return head + newBody + tail
  }
  const eol = body.indexOf("\r\n") !== -1 ? "\r\n" : "\n"
  return head + body + eol + "status: " + status + tail
}

/** porcelain -z 的一行里取路径（去掉前两列的状态标记） */
function statusEntryPath(entry) {
  const text = String(entry || "")
  if (text.length < 4) return text
  return text.slice(3)
}

/**
 * 真的跑一次 git status（`-z` 按 NUL 切分）。
 * @param {string} root
 * @returns {{ok: boolean, stdout: string, stderr: string, error: string}}
 */
function runGitStatus(root) {
  const res = spawnSync(
    "git",
    ["-c", "core.quotepath=false", "status", "--porcelain", "-z", "--untracked-files=all"],
    { cwd: root, encoding: "utf8", windowsHide: true, maxBuffer: 8 * 1024 * 1024 }
  )
  if (res.error) {
    return { ok: false, stdout: "", stderr: "", error: res.error.message || String(res.error) }
  }
  return {
    ok: res.status === 0,
    stdout: String(res.stdout || ""),
    stderr: String(res.stderr || ""),
    error: "",
  }
}

/**
 * 写 learning 仓之前查脏（G14 / P5-3）。
 *
 * 命令固定：`git -c core.quotepath=false status --porcelain -z --untracked-files=all`，
 * 输出按 NUL 切分。
 *
 * **放行范围**：`units/algorithm/` 下的改动是本计划自己的产物，允许；
 * 其余任何一处改动都**拦下来**并把清单报给用户（G14：有他人未提交改动就停手）。
 *
 * @param {{learningRoot?: string, allowPrefix?: string, gitRunner?: Function}} opts
 * @returns {{ok: boolean, entries: string[], blocked: string[], allowed: string[], reason: string, blockedBy: string}}
 */
function checkLearningClean(opts) {
  const o = opts || {}
  const root = String(o.learningRoot || "")
  if (!root) {
    return {
      ok: false,
      entries: [],
      blocked: [],
      allowed: [],
      reason: "没解析到 learning 仓根目录，无法查脏",
      blockedBy: "no-root",
    }
  }
  const runner = typeof o.gitRunner === "function" ? o.gitRunner : runGitStatus
  const res = runner(root)
  if (!res.ok) {
    return {
      ok: false,
      entries: [],
      blocked: [],
      allowed: [],
      reason: "查脏失败（git 跑不通）：" + (res.error || res.stderr || "未知原因"),
      blockedBy: "git-error",
    }
  }
  const entries = String(res.stdout || "")
    .split("\0")
    .filter(function (s) {
      return s !== ""
    })
  const allowPrefix = o.allowPrefix === undefined ? UNITS_REL_DIR : String(o.allowPrefix)
  const blocked = []
  const allowed = []
  entries.forEach(function (entry) {
    const p = statusEntryPath(entry).replace(/\\/g, "/")
    if (allowPrefix && p.indexOf(allowPrefix) === 0) allowed.push(entry)
    else blocked.push(entry)
  })
  if (blocked.length > 0) {
    return {
      ok: false,
      entries: entries,
      blocked: blocked,
      allowed: allowed,
      reason:
        "learning 仓有 " +
        blocked.length +
        " 处不是本计划产物的未提交改动，先提交或 stash 再跑：" +
        blocked.slice(0, 5).join("、"),
      blockedBy: "dirty",
    }
  }
  return { ok: true, entries: entries, blocked: [], allowed: allowed, reason: "", blockedBy: "" }
}

/**
 * 读 units/algorithm/ 下每个单元 md 的 status，返回 {stem: status}。
 *
 * 覆盖报告靠它判断「这道题还欠不欠」：
 * 存量题库是**只读**的，一个字节都不改（P5-1），所以**缺口不是靠题库变完整来消的**，
 * 而是靠 units/algorithm/ 下多了一份 status: verified 的单元——这就是契约 §12 的验收线。
 *
 * @param {string} unitsDir 单元目录
 * @returns {Object} {stem: status}
 */
function readUnitStatuses(unitsDir) {
  const out = {}
  const dir = String(unitsDir || "")
  if (dir === "" || !fs.existsSync(dir)) return out
  let names = []
  try {
    names = fs.readdirSync(dir)
  } catch (err) {
    return out
  }
  names.forEach(function (name) {
    if (!name.endsWith(".md")) return
    const stem = name.slice(0, name.length - 3)
    if (stem === "plan") return
    let text = ""
    try {
      text = fs.readFileSync(path.join(dir, name), "utf8")
    } catch (err) {
      return
    }
    const m = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(text)
    if (!m) return
    const fm = parseFrontmatter(m[1]) || {}
    const status = String(fm.status || "").trim()
    if (status !== "") out[stem] = status
  })
  return out
}

module.exports = {
  SHAPES: SHAPES,
  MISSING_KEYS: MISSING_KEYS,
  UNITS_REL_DIR: UNITS_REL_DIR,
  UNIT_SOURCES: UNIT_SOURCES,
  SOURCE_ALIAS: SOURCE_ALIAS,
  SECTION_STATEMENT: SECTION_STATEMENT,
  SECTION_CASES: SECTION_CASES,
  SECTION_SOLUTION: SECTION_SOLUTION,
  canonicalCode: canonicalCode,
  demoteHeadings: demoteHeadings,
  oneLine: oneLine,
  safeListItem: safeListItem,
  inspectBankFile: inspectBankFile,
  listBankFiles: listBankFiles,
  buildStats: buildStats,
  scanBank: scanBank,
  renderUnitMarkdown: renderUnitMarkdown,
  writeTriplet: writeTriplet,
  extractSolutionBlock: extractSolutionBlock,
  patchStatusLine: patchStatusLine,
  statusEntryPath: statusEntryPath,
  runGitStatus: runGitStatus,
  checkLearningClean: checkLearningClean,
  readUnitStatuses: readUnitStatuses,
}
