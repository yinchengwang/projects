"use strict"
/**
 * plan.js —— 供给缺口扫描
 *
 * 回答两个问题：
 *   1. 每轨现在有多少能学的（todo/draft/verified/rejected）、多少抽检过（reviewed）；
 *   2. 每个单元还欠什么（断言脚本 / 讲解正文 / 测试用例 / 参考解法）。
 *
 * 缺口种类与契约 §8.4 的 missing 取值域一一对应：
 *   "asserts"   —— 断言脚本不存在（卡）
 *   "statement" —— 讲解正文/题面为空（卡 / 题）
 *   "cases"     —— 测试用例为空（题）
 *   "solution"  —— 参考解法为空（题）
 *
 * 卡与题的判据不同：
 *   卡（kind=card）  断言在**独立脚本**里，判据是脚本文件在不在；
 *   题（kind=problem）断言就是测试用例，判据是 unit.asserts 空不空。
 */

const fs = require("node:fs")
const path = require("node:path")

const { TRACK_CODES } = require("../track/tracks.js")
const { parseProblemMarkdown } = require("../bank/parse.js")
const { stemOf } = require("./unit-file.js")

/** 缺口种类（契约 §8.4） */
const GAP_KINDS = ["asserts", "statement", "cases", "solution"]

/** loader C 单元文件里「讲解还没生成」的占位符 */
const EMPTY_EXPLAIN = "（讲解正文还没生成）"

/**
 * 单元文件相对 learning 仓根的路径 → 绝对路径。
 * @param {string} root
 * @param {string} rel
 * @returns {string}
 */
function absOf(root, rel) {
  return path.join(root, String(rel).split("/").join(path.sep))
}

/**
 * 断言脚本在不在。
 * @param {object} unit
 * @param {string|null} learningRoot
 * @returns {boolean}
 */
function hasAssertScript(unit, learningRoot) {
  const script = String((unit.entry && unit.entry.script) || "")
  if (script === "" || !learningRoot) return false
  return fs.existsSync(absOf(learningRoot, script))
}

/**
 * 单元文件里的「## 讲解」段是不是真有正文。
 *
 * 单元文件落在 `learning/units/<track>/<stem>.md`（与 `unit-file.js` 的 `unitFilePath` 一致）——
 * **注意中间那层 `units/`**，少写它就会永远读不到文件、把每张卡都报成缺讲解。
 * 读不到文件（还没生成单元文件）→ 返回 false（讲解算缺口）。
 * 读到占位符或空段 → false。
 * @param {object} unit
 * @param {string|null} learningRoot
 * @returns {boolean}
 */
function hasRealExplain(unit, learningRoot) {
  if (!learningRoot) return false
  const file = path.join(learningRoot, "units", String(unit.track || ""), stemOf(unit) + ".md")
  if (!fs.existsSync(file)) return false
  let text = ""
  try {
    text = fs.readFileSync(file, "utf8")
  } catch (e) {
    return false
  }
  const m = /##\s*讲解\s*\r?\n([\s\S]*?)(?:\r?\n##\s|$)/.exec(text)
  if (!m) return false
  const body = String(m[1] || "").trim()
  return body !== "" && body !== EMPTY_EXPLAIN
}

/**
 * 读一道题的参考解法。
 * @param {object} unit
 * @param {string[]} contentRoots
 * @returns {string|null} 解法正文；null 表示**读不到原文件，判断不了**
 */
function readSolution(unit, contentRoots) {
  const list = Array.isArray(contentRoots) ? contentRoots : []
  for (const root of list) {
    if (!root) continue
    const abs = absOf(root, unit.path)
    if (!fs.existsSync(abs)) continue
    const parsed = parseProblemMarkdown(fs.readFileSync(abs, "utf8"))
    return String((parsed.problem && parsed.problem.solution) || "").trim()
  }
  return null
}

/**
 * 一个单元还欠什么。
 * @param {object} unit
 * @param {{learningRoot?: string|null, contentRoots?: string[], warnings?: string[]}} opts
 * @returns {string[]} 缺口种类（契约 §8.4 的取值域）
 */
function missingOf(unit, opts) {
  const o = opts || {}
  const warnings = Array.isArray(o.warnings) ? o.warnings : []
  const missing = []
  const kind = String(unit.kind || "")
  const statement = String(unit.statement || "").trim()
  if (kind === "card") {
    if (!hasAssertScript(unit, o.learningRoot || null)) missing.push("asserts")
    if (statement === "" || statement === EMPTY_EXPLAIN) missing.push("statement")
    // 单元文件里的「## 讲解」段是占位符 → 讲解还没生成，也算缺口。
    // loader B 给的 statement 是**教材原文**（README + NOTES），不是学习讲解，
    // 所以必须看单元文件那一段，不能只看 unit.statement。
    if (missing.indexOf("statement") === -1 && !hasRealExplain(unit, o.learningRoot || null)) {
      missing.push("statement")
    }
    return missing
  }
  if (statement === "") missing.push("statement")
  if (!Array.isArray(unit.asserts) || unit.asserts.length === 0) missing.push("cases")
  const solution = readSolution(unit, o.contentRoots)
  if (solution === "") {
    missing.push("solution")
  } else if (solution === null) {
    const reference = String((unit.entry && unit.entry.reference) || "")
    if (reference === "") {
      // 没有原文件、也没有独立 reference 路径时，按缺参考解法处理。
      // 若有 reference 字段，说明解法在另一处，交给 loader/下游判断。
      missing.push("solution")
    }
  }
  return missing
}

/**
 * 扫描三轨的缺口。
 * @param {{units: object[], learningRoot?: string|null, contentRoots?: string[], track?: string|null}} opts
 * @returns {{tracks: object[], gaps: object[], warnings: string[]}}
 */
function scanGaps(opts) {
  const o = opts || {}
  const units = Array.isArray(o.units) ? o.units : []
  const learningRoot = o.learningRoot || null
  const contentRoots = Array.isArray(o.contentRoots) ? o.contentRoots : []
  const only = o.track ? String(o.track) : ""
  const warnings = []
  const gaps = []
  const counts = {}

  function bucket(code) {
    if (!counts[code]) {
      counts[code] = {
        track: code,
        total: 0,
        todo: 0,
        draft: 0,
        verified: 0,
        rejected: 0,
        reviewed: 0,
      }
    }
    return counts[code]
  }
  TRACK_CODES.forEach(bucket)

  units.forEach(function (unit) {
    const code = String(unit.track || "")
    if (!counts[code]) {
      bucket(code)
      warnings.push("出现了未知轨道：" + code)
    }
    const box = bucket(code)
    box.total += 1
    const status = String(unit.status || "todo")
    if (status === "todo" || status === "draft" || status === "verified" || status === "rejected") {
      box[status] += 1
    } else {
      box.todo += 1
      warnings.push("单元 " + String(unit.id) + " 的 status 不认识：" + status + "，按 todo 算")
    }
    if (unit.reviewed === true) box.reviewed += 1
    if (only && code !== only) return
    const missing = missingOf(unit, {
      learningRoot: learningRoot,
      contentRoots: contentRoots,
      warnings: warnings,
    })
    if (missing.length > 0) {
      gaps.push({
        id: String(unit.id),
        track: code,
        kind: String(unit.kind || ""),
        title: String(unit.title || ""),
        missing: missing,
      })
    }
    // P5 追加：算法轨覆盖报告——题库是只读的，欠账由 units/algorithm/ 的 verified 抵扣。
    if (code === "algorithm" && (!unit.kind || unit.kind === "problem") && o.store) {
      const { buildCoverageReport } = require("./algorithm-batch.js")
      const { scanBank, readUnitStatuses } = require("./algorithm-fill.js")
      const scan = scanBank({ learningRoot: learningRoot, localBank: null })
      const report = buildCoverageReport({ items: scan.items, store: o.store, units: readUnitStatuses(path.join(learningRoot || "", "units", "algorithm")) })
      gaps.push({ track: "algorithm", total: report.total, ready: report.ready, debt: report.debt, missing: report.missing })
    }
  })

  const tracks = TRACK_CODES.filter(function (code) {
    return !only || code === only
  }).map(function (code) {
    return counts[code]
  })

  gaps.sort(function (a, b) {
    return a.id < b.id ? -1 : a.id > b.id ? 1 : 0
  })

  return { tracks: tracks, gaps: gaps, warnings: warnings }
}

/**
 * 把缺口清单写进 learning/units/<track>/plan.json（契约 §10 的 plan 子命令）。
 * @param {{result: object, unitsDir: string, repoRoot: string|null, stateFile: string|null}} opts
 * @returns {{written: string[], blocked: string[], reason: string}}
 */
function writePlanFiles(opts) {
  const o = opts || {}
  const result = o.result || { tracks: [], gaps: [] }
  const unitsDir = o.unitsDir || ""
  const written = []
  const blocked = []
  let reason = ""
  if (unitsDir === "") {
    return { written: written, blocked: blocked, reason: "还没连上 learning 仓，plan.json 写不了" }
  }
  const tracks = Array.isArray(result.tracks) ? result.tracks : []
  tracks.forEach(function (track) {
    const code = String(track.track || "")
    const dir = path.join(unitsDir, code)
    const file = path.join(dir, "plan.json")
    const gaps = (result.gaps || []).filter(function (gap) {
      return String(gap.track) === code
    })
    const payload = {
      track: code,
      generated_at: new Date().toISOString().slice(0, 10),
      total: track.total,
      todo: track.todo,
      draft: track.draft,
      verified: track.verified,
      rejected: track.rejected,
      reviewed: track.reviewed,
      gaps: gaps,
    }
    const content = JSON.stringify(payload, null, 2) + "\n"
    fs.mkdirSync(dir, { recursive: true })
    if (o.repoRoot) {
      const { writeFileChecked } = require("./unit-file.js")
      const res = writeFileChecked(file, content, {
        repoRoot: o.repoRoot,
        stateFile: o.stateFile || null,
      })
      if (!res.ok) {
        blocked.push(code)
        if (!reason) reason = res.reason
        return
      }
    } else {
      fs.writeFileSync(file, content, "utf8")
    }
    written.push(code)
  })
  return { written: written, blocked: blocked, reason: reason }
}

module.exports = {
  GAP_KINDS: GAP_KINDS,
  EMPTY_EXPLAIN: EMPTY_EXPLAIN,
  missingOf: missingOf,
  scanGaps: scanGaps,
  writePlanFiles: writePlanFiles,
}
