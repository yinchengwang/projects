"use strict"
/**
 * cases.js —— 判题用例的归一化
 *
 * 纯函数，无 IO。题库里的用例长这样（core/bank/parse.js 解析出来的）：
 *   [{input: "[2,7,11,15], 9", expect: "[0,1]", tol: null}]
 * 归一化之后补上从 1 开始的 index——面板上「第几个用例挂的」用的就是它。
 */

/**
 * 归一化容差：非有限数、负数、空串一律当作「没有容差」。
 * @param {*} value
 * @returns {number|null}
 */
function normalizeTol(value) {
  if (value === null || value === undefined || value === "") return null
  const tol = Number(value)
  if (!Number.isFinite(tol) || tol < 0) return null
  return tol
}

/**
 * 归一化一组用例。
 * 规则：
 *   - 非对象条目跳过
 *   - input / expect 转字符串并去首尾空白
 *   - input 与 expect 都空的行是表格里的空行，跳过且不占号
 *   - tol 归一化成 number 或 null
 *   - index 从 1 开始连续编号
 * @param {Array<{input?: string, expect?: string, tol?: number|string|null}>} cases
 * @returns {Array<{index: number, expect: string, input: string, tol: number|null}>}
 */
function normalizeCases(cases) {
  const list = Array.isArray(cases) ? cases : []
  const out = []
  list.forEach(function (raw) {
    if (!raw || typeof raw !== "object") return
    const input = raw.input === null || raw.input === undefined ? "" : String(raw.input).trim()
    const expect = raw.expect === null || raw.expect === undefined ? "" : String(raw.expect).trim()
    if (input === "" && expect === "") return
    out.push({
      index: out.length + 1,
      expect: expect,
      input: input,
      tol: normalizeTol(raw.tol),
    })
  })
  return out
}

module.exports = { normalizeCases: normalizeCases, normalizeTol: normalizeTol }
