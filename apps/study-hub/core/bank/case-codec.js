"use strict"
/**
 * case-codec.js —— 测试用例单元格的编码与解码（纯函数，无 IO）
 *
 * 用例放在 markdown 表格里，一行一格。但 Codeforces 的样例常常是多行的：
 *
 *     | 3 3 | 4 |
 *     | 1 2 3 | 4 |
 *     | 4 5 6 | 4 |
 *
 * 一个换行就会把表格撑破——第二行成了新的一行用例，表格整个错位。
 * 竖线也一样：样例里出现 `|` 时，表格会多切出一列。
 *
 * 编码规则（写进表格前编码，读表格时解码）：
 *   \n → <br>      换行（<br> 在 markdown 表格里正好渲染成软换行）
 *   |  → \|        竖线（表格分隔符，先转义）
 *   \  → \\        反斜杠（必须最先转义，否则会和上面两条打架）
 *
 * 反过来解的时候先认 <br>，再逐字符解 \\ 与 \|，正好还原原文。
 */

const BR_RE = /<\s*br\s*\/?\s*>/gi

/**
 * 编码一个表格单元格。
 * @param {string} text 原始文本（可以含换行、竖线、反斜杠）
 * @returns {string}
 */
function encodeCell(text) {
  let s = String(text === null || text === undefined ? "" : text)
  s = s.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
  s = s.replace(/\\/g, "\\\\")
  s = s.replace(/\|/g, "\\|")
  s = s.replace(/\n/g, "<br>")
  return s
}

/**
 * 解码一个表格单元格。
 * @param {string} text 表格里读到的单元格文本
 * @returns {string}
 */
function decodeCell(text) {
  let s = String(text === null || text === undefined ? "" : text)
  s = s.replace(BR_RE, "\n")
  let out = ""
  for (let i = 0; i < s.length; i++) {
    const ch = s.charAt(i)
    if (ch === "\\" && i + 1 < s.length) {
      const next = s.charAt(i + 1)
      if (next === "|" || next === "\\") {
        out += next
        i += 1
        continue
      }
    }
    out += ch
  }
  return out
}

/**
 * 按未被转义的竖线切一行表格。
 * @param {string} body 已经去掉首尾竖线的行内容
 * @returns {string[]} 各单元格原文（未解码）
 */
function splitRow(body) {
  const cells = []
  let buf = ""
  const src = String(body === null || body === undefined ? "" : body)
  for (let i = 0; i < src.length; i++) {
    const ch = src.charAt(i)
    if (ch === "\\" && src.charAt(i + 1) === "|") {
      buf += "\\|"
      i += 1
      continue
    }
    if (ch === "|") {
      cells.push(buf)
      buf = ""
      continue
    }
    buf += ch
  }
  cells.push(buf)
  return cells.map(function (cell) {
    return cell.trim()
  })
}

module.exports = {
  encodeCell: encodeCell,
  decodeCell: decodeCell,
  splitRow: splitRow,
}
