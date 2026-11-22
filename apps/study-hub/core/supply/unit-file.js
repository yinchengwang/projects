"use strict"
/**
 * unit-file.js —— 单元文件（learning/units/<track>/<stem>.md）的读写与「写回前查脏」
 *
 * 三条纪律都在这里落地：
 *   1. 写回只改目标行（G15）：frontmatter 只替换目标键所在的那一行，
 *      正文小节只替换目标小节内的行，其余行**逐字节**保持原样（含行尾 \r\n / \n）。
 *   2. 写回前查脏（G14）：别人动过的文件一律阻塞写回；
 *      **本流水线自己写过的**未提交改动只告警不阻塞——靠一张「自己写过」清单
 *      （路径 → 写入时的内容哈希）比对，内容被人改过就不再算自己写的。
 *   3. 不自动 commit（G13）：这里只返回 diff，绝不碰 git。
 *
 * 「自己写过」清单放在 app 自己的 data/ 目录（projects/.gitignore 已忽略 data/），
 * 不往 learning 仓里塞任何文件。
 */

const fs = require("node:fs")
const path = require("node:path")
const crypto = require("node:crypto")

const { parseFrontmatter, splitSections } = require("../bank/parse.js")
const { assertCleanWorktree, makeDiff, splitLines } = require("../sync/rounds.js")

/** loader C 的 frontmatter 字段顺序（契约 §3.3，逐字照抄） */
const FRONTMATTER_ORDER = [
  "id",
  "track",
  "kind",
  "title",
  "difficulty",
  "topics",
  "source",
  "language",
  "demo",
  "reference",
  "graded",
  "script",
  "status",
  "reviewed",
  "reviewed_at",
  "subtracks",
]

/** 「自己写过」清单的文件名 */
const SELF_WRITTEN_FILE = "supply-self-written.json"

/**
 * 单元文件里「讲解还没生成」的占位符。
 *
 * 与 `core/supply/plan.js` 的同名常量必须一致——plan 靠它判断「缺讲解正文」这个缺口。
 * 由本文件导出、plan.js 与 verify.js 共用，别再各写一份。
 */
const EMPTY_EXPLAIN = "（讲解正文还没生成）"

/** 单元的短名：id 形如 db:mvcc，取冒号后面那段 */
function stemOf(unit) {
  const id = String((unit && unit.id) || "")
  const at = id.indexOf(":")
  return at === -1 ? id : id.slice(at + 1)
}

/** 单元文件路径：learning/units/<track>/<stem>.md */
function unitFilePath(unitsDir, unit) {
  return path.join(unitsDir, unit.track, stemOf(unit) + ".md")
}

/** 断言脚本路径：learning/units/<track>/<stem>.assert.py */
function assertFilePath(unitsDir, unit) {
  return path.join(unitsDir, unit.track, stemOf(unit) + ".assert.py")
}

/** 把值写成 frontmatter 里的一行值 */
function formatFieldValue(value) {
  if (typeof value === "boolean") return value ? "true" : "false"
  if (Array.isArray(value)) {
    return "[" + value.map(function (item) { return String(item) }).join(", ") + "]"
  }
  const text = String(value === null || value === undefined ? "" : value)
  if (text.indexOf("\n") !== -1) {
    throw new Error("frontmatter 的值不能带换行：" + text.slice(0, 30))
  }
  return text
}

/**
 * 渲染一个单元文件。
 * @param {object} fields frontmatter 字段（键取自 FRONTMATTER_ORDER）
 * @param {{explain?: string, related?: string[]}} opts
 * @returns {string}
 */
function renderUnitFile(fields, opts) {
  const o = opts || {}
  const f = fields || {}
  const lines = ["---"]
  FRONTMATTER_ORDER.forEach(function (key) {
    let value = f[key]
    if (value === undefined || value === null) {
      if (key === "topics" || key === "subtracks") value = []
      else if (key === "reviewed") value = false
      else if (key === "difficulty") value = "medium"
      else if (key === "status") value = "draft"
      else if (key === "kind") value = "card"
      else if (key === "graded") value = "script"
      else value = ""
    }
    lines.push(key + ": " + formatFieldValue(value))
  })
  lines.push("---")
  lines.push("")
  lines.push("## 讲解")
  lines.push("")
  lines.push(String(o.explain || "").trim() || "（讲解正文还没生成）")
  lines.push("")
  lines.push("## 关联")
  lines.push("")
  const related = Array.isArray(o.related) ? o.related : []
  related.forEach(function (item) { lines.push(String(item)) })
  lines.push("")
  return lines.join("\n")
}

/**
 * 读一个单元文件。
 * @param {string} absPath
 * @returns {{text: string, fields: object, body: string, sections: object}}
 */
function readUnitFile(absPath) {
  const text = fs.readFileSync(absPath, "utf8")
  const parts = text.split(/^---$/m)
  const fields = parts.length >= 3 ? parseFrontmatter(parts[1]) : parseFrontmatter(text)
  const body = parts.length >= 3 ? parts.slice(2).join("---") : text
  return {
    text: text,
    fields: fields,
    body: body,
    sections: splitSections(body),
  }
}

/**
 * 只改 frontmatter 里指定的那几行，其余行逐字节不变。
 * @param {string} text
 * @param {object} patch
 * @returns {string}
 */
function applyFrontmatterPatch(text, patch) {
  const lines = splitLines(String(text === null || text === undefined ? "" : text))
  if (lines.length === 0) throw new Error("单元文件是空的")
  if (lines[0].text.trim() !== "---") throw new Error("单元文件缺 frontmatter 起始行")
  let end = -1
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].text.trim() === "---") { end = i; break }
  }
  if (end === -1) throw new Error("单元文件缺 frontmatter 结束行")

  const keys = Object.keys(patch || {})
  for (const key of keys) {
    const value = formatFieldValue(patch[key])
    let hit = -1
    for (let i = 1; i < end; i++) {
      const m = /^([A-Za-z_][A-Za-z0-9_]*)\s*:/.exec(lines[i].text)
      if (m && m[1] === key) { hit = i; break }
    }
    if (hit !== -1) {
      lines[hit] = { text: key + ": " + value, eol: lines[hit].eol }
    } else {
      const eol = end > 1 ? (lines[end - 1].eol || "\n") : "\n"
      lines.splice(end, 0, { text: key + ": " + value, eol: eol })
      end += 1
    }
  }
  return lines.map(function (line) { return line.text + line.eol }).join("")
}

/** 找到「## <name>」小节的头行下标；找不到返回 -1 */
function findSection(lines, name) {
  for (let i = 0; i < lines.length; i++) {
    const m = /^##\s+(.*)$/.exec(lines[i].text)
    if (m && m[1].trim() === name) return i
  }
  return -1
}

/** 找到从 start+1 开始、下一处「## 」小节头的行下标；找不到返回末尾 */
function sectionEnd(lines, start) {
  for (let i = start + 1; i < lines.length; i++) {
    if (/^##\s+/.test(lines[i].text)) return i
  }
  return lines.length
}

/** 行数组 → 字符串（逐字节还原） */
function joinLines(lines) {
  return lines.map(function (line) { return line.text + line.eol }).join("")
}

/**
 * 替换目标小节的正文，其余行逐字节不变。
 * @param {string} text
 * @param {string} name 小节名（如「讲解」）
 * @param {string} content
 * @returns {string}
 */
function replaceSection(text, name, content) {
  const lines = splitLines(String(text === null || text === undefined ? "" : text))
  const start = findSection(lines, name)
  if (start === -1) throw new Error("单元文件里没有「## " + name + "」小节")
  const end = sectionEnd(lines, start)
  const eol = lines[start].eol || "\n"
  const body = String(content === null || content === undefined ? "" : content).trim()
  const bodyLines = body === "" ? [] : body.split(/\r\n|\n/)
  const inserted = [{ text: "", eol: eol }]
    .concat(bodyLines.map(function (t) { return { text: t, eol: eol } }))
    .concat([{ text: "", eol: eol }])
  if (end >= lines.length && inserted.length > 0) {
    const last = lines[lines.length - 1]
    inserted[inserted.length - 1] = { text: inserted[inserted.length - 1].text, eol: last ? (last.eol || "") : eol }
  }
  return joinLines(lines.slice(0, start + 1).concat(inserted, lines.slice(end)))
}

/**
 * 在目标小节末尾追加一条列表项。
 * @param {string} text
 * @param {string} name
 * @param {string} note
 * @returns {string}
 */
function appendSectionNote(text, name, note) {
  const src = String(text === null || text === undefined ? "" : text)
  const lines = splitLines(src)
  const start = findSection(lines, name)
  const bullet = "- " + String(note === null || note === undefined ? "" : note).trim()
  if (start === -1) {
    const prefix = src === "" || src.slice(-1) === "\n" ? src : src + "\n"
    return prefix + "\n## " + name + "\n\n" + bullet + "\n"
  }
  const end = sectionEnd(lines, start)
  let insertAt = 0
  for (let i = 0; i < end; i++) insertAt += lines[i].text.length + lines[i].eol.length
  const head = src.slice(0, insertAt)
  const tail = src.slice(insertAt)
  const eol = lines[start].eol || "\n"
  const inserted = tail === "" ? bullet + eol : bullet + eol + eol
  return head + inserted + tail
}

/**
 * 在目标小节末尾追加一段正文，其余行逐字节不变。
 * @param {string} text
 * @param {string} name
 * @param {string} content
 * @returns {string}
 */
function appendToSection(text, name, content) {
  const src = String(text === null || text === undefined ? "" : text)
  const lines = splitLines(src)
  const start = findSection(lines, name)
  if (start === -1) throw new Error("单元文件里没有「## " + name + "」小节")
  const end = sectionEnd(lines, start)
  let insertAt = 0
  for (let i = 0; i < end; i++) insertAt += lines[i].text.length + lines[i].eol.length
  const body = String(content === null || content === undefined ? "" : content).trim()
  if (body === "") return src
  const eol = lines[start].eol || "\n"
  const head = src.slice(0, insertAt)
  const tail = src.slice(insertAt)
  const glue = head === "" || head.slice(-1) === "\n" ? "" : eol
  const tailer = tail === "" ? eol : eol + eol
  return head + glue + body + tailer + tail
}

/** 「自己写过」清单的路径 */
function stateFilePath(appRoot) {
  return path.join(appRoot, "data", SELF_WRITTEN_FILE)
}

/** 内容哈希 */
function sha1(text) {
  return crypto.createHash("sha1").update(String(text), "utf8").digest("hex")
}

/** 路径统一成正斜杠（与 git 一致） */
function relPosix(root, abs) {
  return path.relative(root, abs).replace(/\\/g, "/")
}

/**
 * 读「自己写过」清单。
 * @param {string} stateFile
 * @returns {{files: object}}
 */
function readSelfWritten(stateFile) {
  if (!stateFile || !fs.existsSync(stateFile)) return { files: {} }
  try {
    const parsed = JSON.parse(fs.readFileSync(stateFile, "utf8"))
    if (parsed && typeof parsed.files === "object" && parsed.files) return { files: parsed.files }
    return { files: {} }
  } catch (err) {
    return { files: {} }
  }
}

/**
 * 记一笔「这个文件是我们自己写的」+ 写入时的内容哈希。
 * @param {string} stateFile
 * @param {string} rel 相对 learning 仓根的路径（正斜杠）
 * @param {string} content 写入时的内容
 */
function markSelfWritten(stateFile, rel, content) {
  if (!stateFile) return
  const state = readSelfWritten(stateFile)
  state.files[rel] = sha1(content)
  fs.mkdirSync(path.dirname(stateFile), { recursive: true })
  fs.writeFileSync(stateFile, JSON.stringify(state, null, 2) + "\n", "utf8")
}

/**
 * P4 完全自有的生成物：永远是「自己写的」，不阻塞写回。
 * 目前只有 `units/<track>/plan.json`——它是 `plan` 子命令每次重写的报告，
 * 人不会手改它；内容每次都变，哈希比对必然失配，所以单独豁免。
 * @param {string} rel
 * @returns {boolean}
 */
function isOwnedArtifact(rel) {
  return /^units\/[a-z]+\/plan\.json$/.test(String(rel || ""))
}

/**
 * 这个文件还是我们自己写的吗（内容没被人动过）。
 * @param {string} stateFile
 * @param {string} repoRoot
 * @param {string} rel
 * @returns {boolean}
 */
function isSelfWritten(stateFile, repoRoot, rel) {
  if (isOwnedArtifact(rel)) return true
  const state = readSelfWritten(stateFile)
  const recorded = state.files[rel]
  if (!recorded) return false
  const abs = path.join(repoRoot, rel.split("/").join(path.sep))
  if (!fs.existsSync(abs)) return false
  return sha1(fs.readFileSync(abs, "utf8")) === recorded
}

/**
 * 写回前查脏。
 * @param {string} repoRoot learning 仓根
 * @param {string} stateFile
 * @returns {{ok: boolean, dirtyFiles: string[], ownFiles: string[], reason: string}}
 */
function checkWritable(repoRoot, stateFile) {
  const status = assertCleanWorktree(repoRoot)
  if (status.ok) return { ok: true, dirtyFiles: [], ownFiles: [], reason: "" }
  if (status.reason) {
    return { ok: false, dirtyFiles: [], ownFiles: [], reason: status.reason }
  }
  const ownFiles = []
  const others = []
  const stateRel = stateFile ? relPosix(repoRoot, stateFile) : ""
  status.dirtyFiles.forEach(function (rel) {
    // 自己的状态清单本身就是写回副产物，不参与「别人动过」判定。
    // 否则第一次 markSelfWritten 后，下一次写回会被它自己的 JSON 反向拦住。
    if (stateRel && rel === stateRel) return
    if (isSelfWritten(stateFile, repoRoot, rel)) ownFiles.push(rel)
    else others.push(rel)
  })
  if (others.length > 0) {
    return {
      ok: false,
      dirtyFiles: others,
      ownFiles: ownFiles,
      reason:
        "learning 仓工作区不干净，这些文件被别处改过，写回已阻塞：" +
        others.join("、") +
        "（先提交或撤销这些改动再试）",
    }
  }
  return { ok: true, dirtyFiles: [], ownFiles: ownFiles, reason: "" }
}

/**
 * 写一个文件：已存在且被别人动过就拦；写完记一笔「自己写的」。
 * @param {string} absPath
 * @param {string} content
 * @param {{repoRoot?: string, stateFile?: string}} opts
 * @returns {{ok: boolean, changed: boolean, reason: string, diff: string}}
 */
function writeFileChecked(absPath, content, opts) {
  const o = opts || {}
  const text = String(content === null || content === undefined ? "" : content)
  const exists = fs.existsSync(absPath)
  const before = exists ? fs.readFileSync(absPath, "utf8") : null
  if (before === text) return { ok: true, changed: false, reason: "", diff: "" }
  if (exists && o.repoRoot) {
    const check = checkWritable(o.repoRoot, o.stateFile)
    if (!check.ok) return { ok: false, changed: false, reason: check.reason, diff: "" }
  }
  fs.mkdirSync(path.dirname(absPath), { recursive: true })
  fs.writeFileSync(absPath, text, "utf8")
  const rel = o.repoRoot ? relPosix(o.repoRoot, absPath) : absPath
  if (o.repoRoot && o.stateFile) markSelfWritten(o.stateFile, rel, text)
  return {
    ok: true,
    changed: true,
    reason: "",
    diff: makeDiff(rel, before === null ? "" : before, text),
  }
}

/**
 * 改一个单元文件的 frontmatter：只改目标行，写完记一笔「自己写的」。
 * @param {string} absPath
 * @param {object} patch
 * @param {{repoRoot?: string, stateFile?: string}} opts
 * @returns {{ok: boolean, changed: boolean, reason: string, diff: string}}
 */
function patchUnitFile(absPath, patch, opts) {
  const o = opts || {}
  if (!fs.existsSync(absPath)) {
    return { ok: false, changed: false, reason: "单元文件不存在：" + absPath, diff: "" }
  }
  const before = fs.readFileSync(absPath, "utf8")
  let after
  try {
    after = applyFrontmatterPatch(before, patch)
  } catch (err) {
    return { ok: false, changed: false, reason: String((err && err.message) || err), diff: "" }
  }
  if (after === before) return { ok: true, changed: false, reason: "", diff: "" }
  if (o.repoRoot) {
    const check = checkWritable(o.repoRoot, o.stateFile)
    if (!check.ok) return { ok: false, changed: false, reason: check.reason, diff: "" }
  }
  fs.writeFileSync(absPath, after, "utf8")
  const rel = o.repoRoot ? relPosix(o.repoRoot, absPath) : absPath
  if (o.repoRoot && o.stateFile) markSelfWritten(o.stateFile, rel, after)
  return {
    ok: true,
    changed: true,
    reason: "",
    diff: makeDiff(rel, before, after),
  }
}

module.exports = {
  FRONTMATTER_ORDER: FRONTMATTER_ORDER,
  SELF_WRITTEN_FILE: SELF_WRITTEN_FILE,
  EMPTY_EXPLAIN: EMPTY_EXPLAIN,
  formatFieldValue: formatFieldValue,
  stemOf: stemOf,
  unitFilePath: unitFilePath,
  assertFilePath: assertFilePath,
  renderUnitFile: renderUnitFile,
  readUnitFile: readUnitFile,
  applyFrontmatterPatch: applyFrontmatterPatch,
  replaceSection: replaceSection,
  appendSectionNote: appendSectionNote,
  appendToSection: appendToSection,
  stateFilePath: stateFilePath,
  readSelfWritten: readSelfWritten,
  markSelfWritten: markSelfWritten,
  isSelfWritten: isSelfWritten,
  isOwnedArtifact: isOwnedArtifact,
  checkWritable: checkWritable,
  writeFileChecked: writeFileChecked,
  patchUnitFile: patchUnitFile,
}
