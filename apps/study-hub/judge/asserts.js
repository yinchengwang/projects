"use strict"
/**
 * asserts.js —— 断言校验器
 *
 * 判题只有一条路径（设计文档 §5.1）：
 *   断言集 → 选入口 → 沙箱执行 → 收集 stdout/exit/产物 → **逐条校验** → verdict
 * 本文件就是「逐条校验」那一段：纯函数、无 IO、可单测。
 *
 * 四种断言（接口契约 §2）：
 *   output   stdout 比对（cmp: exact / trim / json / lines）
 *   exit     进程退出码
 *   contains stdout+stderr 合并串里含某个子串
 *   file     沙箱里的产物文件内容比对
 *
 * 归一化口径沿用 core/judge-spec/compare.js 的 normalizeText（逐字相同）。
 */

const { normalizeText } = require("../core/judge-spec/compare.js")

/** cmp 的四档 */
const CMP_MODES = ["exact", "trim", "json", "lines"]

/**
 * 严格 JSON 解析：失败返回 null（不抛）。
 * @param {string} text
 * @returns {object|number|string|boolean|null|undefined}
 */
function tryParseJson(text) {
  try {
    return JSON.parse(String(text === null || text === undefined ? "" : text))
  } catch (e) {
    return undefined
  }
}

/**
 * 深度相等：对象键序无关、数组元素序有关。
 * @param {*} a
 * @param {*} b
 * @returns {boolean}
 */
function deepEqual(a, b) {
  if (a === b) return true
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b)) return false
    if (a.length !== b.length) return false
    for (let i = 0; i < a.length; i += 1) {
      if (!deepEqual(a[i], b[i])) return false
    }
    return true
  }
  if (a && b && typeof a === "object" && typeof b === "object") {
    const ka = Object.keys(a).sort()
    const kb = Object.keys(b).sort()
    if (ka.length !== kb.length) return false
    for (let i = 0; i < ka.length; i += 1) {
      if (ka[i] !== kb[i]) return false
      if (!deepEqual(a[ka[i]], b[ka[i]])) return false
    }
    return true
  }
  return false
}

/**
 * 按 cmp 档位比对两段文本。
 * @param {string} actual
 * @param {string} expect
 * @param {"exact"|"trim"|"json"|"lines"} cmp
 * @returns {{equal: boolean, reason: string}}
 */
function compareByMode(actual, expect, cmp) {
  const mode = CMP_MODES.indexOf(cmp) === -1 ? "exact" : cmp
  const a = actual === null || actual === undefined ? "" : String(actual)
  const e = expect === null || expect === undefined ? "" : String(expect)

  if (mode === "trim") {
    return a.trim() === e.trim()
      ? { equal: true, reason: "去掉首尾空白后一致" }
      : { equal: false, reason: "去掉首尾空白后仍与期望不一致" }
  }

  if (mode === "json") {
    const av = tryParseJson(a)
    const ev = tryParseJson(e)
    if (av === undefined || ev === undefined) {
      return { equal: false, reason: "期望是 JSON 但解析失败" }
    }
    return deepEqual(av, ev)
      ? { equal: true, reason: "JSON 结构一致" }
      : { equal: false, reason: "JSON 结构与期望不一致" }
  }

  if (mode === "lines") {
    const al = a.split("\n")
    const el = e.split("\n")
    if (al.length !== el.length) {
      return {
        equal: false,
        reason: "行数不一致：期望 " + el.length + " 行，实际 " + al.length + " 行",
      }
    }
    for (let i = 0; i < al.length; i += 1) {
      if (normalizeText(al[i]) !== normalizeText(el[i])) {
        return { equal: false, reason: "第 " + (i + 1) + " 行与期望不一致" }
      }
    }
    return { equal: true, reason: "逐行一致" }
  }

  return normalizeText(a) === normalizeText(e)
    ? { equal: true, reason: "输出一致（已忽略首尾空白与末尾空行）" }
    : { equal: false, reason: "输出与期望不一致" }
}

/**
 * 产物路径是否越界：含 `..` 段，或以 / 或 \ 开头。
 * @param {string} relPath
 * @returns {boolean}
 */
function isOutOfBounds(relPath) {
  const text = String(relPath === null || relPath === undefined ? "" : relPath)
  if (text === "") return true
  if (text.charAt(0) === "/" || text.charAt(0) === "\\") return true
  return text.split(/[\\/]+/).some(function (seg) {
    return seg === ".."
  })
}

/**
 * 校验一条断言。
 * @param {object} a 断言
 * @param {object} run 跑完进程的现场
 * @returns {{ok: boolean, reason: string}}
 */
function validateOne(a, run) {
  const r = run || {}

  if (a.type === "exit") {
    if (Number(a.expect) !== 0) {
      return { ok: false, reason: "断言非法：exit 类型的 expect 只能是 0，实际 " + String(a.expect) }
    }
    const code = r.exitCode === null || r.exitCode === undefined ? null : Number(r.exitCode)
    if (code === 0) return { ok: true, reason: "退出码 0" }
    return { ok: false, reason: "期望退出码 0，实际 " + String(code === null ? "无" : code) }
  }

  if (a.type === "output") {
    const cmp = compareByMode(r.stdout, a.expect, a.cmp || "exact")
    return { ok: cmp.equal, reason: cmp.reason }
  }

  if (a.type === "contains") {
    const merged = String(r.stdout || "") + String(r.stderr || "")
    const text = String(a.text === null || a.text === undefined ? "" : a.text)
    if (merged.indexOf(text) !== -1) return { ok: true, reason: "输出里找到了：" + text }
    return { ok: false, reason: "输出里没有找到：" + text }
  }

  if (a.type === "file") {
    const p = String(a.path === null || a.path === undefined ? "" : a.path)
    if (isOutOfBounds(p)) return { ok: false, reason: "产物路径越界：" + p }
    const artifacts = r.artifacts || {}
    if (!Object.prototype.hasOwnProperty.call(artifacts, p)) {
      return { ok: false, reason: "产物文件不存在：" + p }
    }
    const cmp = compareByMode(artifacts[p], a.expect, a.cmp || "exact")
    return { ok: cmp.equal, reason: cmp.reason }
  }

  return { ok: false, reason: "未知断言类型：" + String(a.type) }
}

/**
 * 逐条校验断言集。
 * @param {Array<object>} asserts 断言集
 * @param {{stdout?: string, stderr?: string, exitCode?: number|null, timedOut?: boolean, artifacts?: object}} run
 * @returns {{passed: number, total: number, failures: Array<{index: number, type: string, reason: string}>}}
 */
function validateAsserts(asserts, run) {
  const list = Array.isArray(asserts) ? asserts : []
  const r = Object.assign({ stdout: "", stderr: "", exitCode: 0, timedOut: false, artifacts: {} }, run || {})
  const failures = []
  let passed = 0

  for (let i = 0; i < list.length; i += 1) {
    const a = list[i] || {}
    if (r.timedOut) {
      failures.push({ index: i, type: String(a.type || ""), reason: "进程超时，未执行到断言校验" })
      continue
    }
    const one = validateOne(a, r)
    if (one.ok) passed += 1
    else failures.push({ index: i, type: String(a.type || ""), reason: one.reason })
  }

  return { passed: passed, total: list.length, failures: failures }
}

module.exports = {
  CMP_MODES: CMP_MODES,
  normalizeText: normalizeText,
  compareByMode: compareByMode,
  validateAsserts: validateAsserts,
  isOutOfBounds: isOutOfBounds,
}
