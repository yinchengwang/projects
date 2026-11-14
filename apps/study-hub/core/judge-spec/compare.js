"use strict"
/**
 * compare.js —— 输出比对规则
 *
 * 纯函数，无 IO。规则照设计文档 §5.3：
 *   默认：去首尾空白 + 忽略末尾空行后精确匹配
 *   tol != null：数值容差比较
 *
 * 「归一化」的确切定义（面板上要能说清楚，所以写死在这里）：
 *   1. \r\n 与 \r 统一成 \n
 *   2. 每行去掉行尾的空格与制表符
 *   3. 整个文本去掉首尾空白（等价于 trim）——末尾的空行因此被忽略
 */

/**
 * 归一化文本。
 * @param {string} text
 * @returns {string}
 */
function normalizeText(text) {
  if (text === null || text === undefined) return ""
  return String(text)
    .replace(/\r\n/g, "\n")
    .split("\n")
    .map(function (line) {
      return line.replace(/[ \t\r]+$/, "")
    })
    .join("\n")
    .trim()
}

/**
 * 严格数值解析：不是合法数字就返回 null。
 * 特意不用 Number()——Number("") 是 0，会把「程序什么都没输出」误当成数字 0。
 * @param {string} text
 * @returns {number|null}
 */
function toNumber(text) {
  const s = String(text === null || text === undefined ? "" : text).trim()
  if (s === "") return null
  if (!/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(s)) return null
  const value = Number(s)
  return Number.isFinite(value) ? value : null
}

/**
 * 比对实际输出与期望输出。
 * @param {string} actual 程序 stdout
 * @param {string} expected 用例里的期望输出
 * @param {number|string|null|undefined} tol 数值容差；null/undefined 表示精确匹配
 * @returns {{equal: boolean, reason: string}} reason 是中文说明
 */
function compareOutput(actual, expected, tol) {
  const a = normalizeText(actual)
  const e = normalizeText(expected)

  const hasTol = tol !== null && tol !== undefined && tol !== ""
  if (hasTol) {
    const av = toNumber(a)
    const ev = toNumber(e)
    if (av !== null && ev !== null) {
      const diff = Math.abs(av - ev)
      if (diff <= Number(tol)) {
        return { equal: true, reason: "数值偏差 " + diff + "，在容差 " + tol + " 之内" }
      }
      return { equal: false, reason: "数值偏差 " + diff + "，超过容差 " + tol }
    }
    if (ev !== null) {
      return { equal: false, reason: "期望是数值，但实际输出不是数值，无法按容差比较" }
    }
  }

  if (a === e) {
    return { equal: true, reason: "输出一致（已忽略首尾空白与末尾空行）" }
  }
  return { equal: false, reason: "输出与期望不一致" }
}

module.exports = {
  compareOutput: compareOutput,
  normalizeText: normalizeText,
  toNumber: toNumber,
}
