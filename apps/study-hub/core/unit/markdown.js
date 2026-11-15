"use strict"
/**
 * markdown.js —— 极简 Markdown → HTML
 *
 * 零依赖铁律：不引 Markdown 库（契约 §8.1）。
 * 支持的语法就那几样：标题、段落、无序/有序列表、粗体、行内代码、代码块、表格、引用。
 * **未知语法原样输出为文本**——不报错、不丢内容。
 *
 * 输出是给单元学习页左栏用的**片段**，不含 <html>/<body> 外壳。
 */

/** 行内代码的占位哨兵：先抽出行内代码，转义完再放回去，免得代码里的 * 和 _ 被当成强调。 */
const SENTINEL = "\u0000"

/**
 * 引用块的行判定：`>` 后**必须**有空白（GFM 要求；`>5 秒` 这类写法是正文，不是引用）。
 *
 * 同一条规则同时用于三处——判定块的起点、收集引用体、剥掉前缀。
 * **必须共用这一个常量**：历史上这里抄了三份，改了其中一处就会让 `toHtml` 死循环
 * （判定说「这是引用」，收集循环却不认，`i` 不前进）。
 */
const QUOTE_LINE = /^>\s/

/**
 * HTML 转义。
 * @param {*} text
 * @returns {string}
 */
function escapeHtml(text) {
  return String(text === undefined || text === null ? "" : text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;")
}

/**
 * 行内渲染：行内代码、粗体、斜体。
 * @param {string} text
 * @returns {string}
 */
function inline(text) {
  const codes = []
  let out = String(text === undefined || text === null ? "" : text).replace(/`([^`]+)`/g, function (_, code) {
    codes.push(code)
    return SENTINEL + (codes.length - 1) + SENTINEL
  })
  out = escapeHtml(out)
  out = out.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
  out = out.replace(/(^|[^*])\*([^*\n]+)\*/g, "$1<em>$2</em>")
  out = out.replace(/\u0000(\d+)\u0000/g, function (_, index) {
    return "<code>" + escapeHtml(codes[Number(index)]) + "</code>"
  })
  return out
}

/**
 * 判断一行是不是表格分隔行（|---|---| 或 --- | ---）。
 * @param {string} line
 * @returns {boolean}
 */
function isTableSeparator(line) {
  const text = String(line === undefined || line === null ? "" : line).trim()
  if (text.indexOf("-") === -1) return false
  return /^\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?$/.test(text)
}

/**
 * 判断当前位置是不是表格的表头行（当前行有 |，且下一行是分隔行）。
 * @param {string} line
 * @param {string} next
 * @returns {boolean}
 */
function isTableStart(line, next) {
  if (String(line === undefined || line === null ? "" : line).indexOf("|") === -1) return false
  return isTableSeparator(next)
}

/**
 * 判断一行是不是某个块级语法的起点，并给出块的种类。
 *
 * 这是块类型清单的**唯一来源**：
 *   - 分派链（`toHtml` 里的 if 链）靠它决定这一段渲染成什么；
 *   - 段落扫描靠它判断段落什么时候结束。
 * 将来加块类型只改这一处——漏改段落扫描就会出现「某个块被当成段落」。
 *
 * 判定顺序就是分派链的顺序：代码块 → 标题 → 表格 → 引用 → 列表。
 * 表格要看下一行（分隔行），所以要 `next`。
 *
 * @param {string} line 当前行
 * @param {string} next 下一行
 * @returns {"code"|"heading"|"table"|"quote"|"list"|null} 不是块级起点时返回 null
 */
function isBlockStart(line, next) {
  const text = String(line === undefined || line === null ? "" : line)
  if (/^```/.test(text)) return "code"
  if (/^(#{1,6})\s+(.*)$/.test(text)) return "heading"
  if (isTableStart(text, next)) return "table"
  if (QUOTE_LINE.test(text)) return "quote"
  if (/^\s*[-*]\s+/.test(text) || /^\s*\d+\.\s+/.test(text)) return "list"
  return null
}

/**
 * 切一行表格为单元格数组。单元格里写 `\|` 表示竖线本身。
 * @param {string} line
 * @returns {string[]}
 */
function splitTableRow(line) {
  let body = String(line === undefined || line === null ? "" : line).trim()
  if (body.charAt(0) === "|") body = body.slice(1)
  if (body.length > 0 && body.charAt(body.length - 1) === "|") body = body.slice(0, -1)
  const cells = []
  let buf = ""
  for (let i = 0; i < body.length; i++) {
    const ch = body.charAt(i)
    if (ch === "\\" && body.charAt(i + 1) === "|") {
      buf += "|"
      i += 1
      continue
    }
    if (ch === "|") {
      cells.push(buf.trim())
      buf = ""
      continue
    }
    buf += ch
  }
  cells.push(buf.trim())
  return cells
}

/**
 * 表格渲染。
 * @param {string[][]} rows 第一行是表头
 * @returns {string}
 */
function renderTable(rows) {
  if (!rows || rows.length === 0) return ""
  let out = "<table><thead><tr>"
  for (const cell of rows[0]) {
    out += "<th>" + inline(cell) + "</th>"
  }
  out += "</tr></thead><tbody>"
  for (const row of rows.slice(1)) {
    out += "<tr>"
    for (const cell of row) {
      out += "<td>" + inline(cell) + "</td>"
    }
    out += "</tr>"
  }
  out += "</tbody></table>"
  return out
}

/**
 * Markdown → HTML。
 * @param {string} markdown
 * @returns {string}
 */
function toHtml(markdown) {
  const lines = String(markdown === undefined || markdown === null ? "" : markdown).split(/\r?\n/)
  const out = []
  let i = 0

  while (i < lines.length) {
    const line = lines[i]
    // 块级判定统一走 isBlockStart（块类型清单只有那一份）。
    const block = isBlockStart(line, lines[i + 1])

    // 代码块
    if (block === "code") {
      const body = []
      i += 1
      while (i < lines.length && !/^```/.test(lines[i])) {
        body.push(lines[i])
        i += 1
      }
      i += 1 // 跳过收尾的 ```
      out.push("<pre><code>" + escapeHtml(body.join("\n")) + "</code></pre>")
      continue
    }

    // 标题
    if (block === "heading") {
      const head = /^(#{1,6})\s+(.*)$/.exec(line)
      const level = head[1].length
      out.push("<h" + level + ">" + inline(head[2]) + "</h" + level + ">")
      i += 1
      continue
    }

    // 表格
    if (block === "table") {
      const rows = [splitTableRow(line)]
      i += 2
      while (i < lines.length && lines[i].trim() !== "" && lines[i].indexOf("|") !== -1) {
        rows.push(splitTableRow(lines[i]))
        i += 1
      }
      out.push(renderTable(rows))
      continue
    }

    // 引用
    if (block === "quote") {
      const body = []
      while (i < lines.length && QUOTE_LINE.test(lines[i])) {
        body.push(lines[i].replace(QUOTE_LINE, ""))
        i += 1
      }
      out.push("<blockquote>" + toHtml(body.join("\n")) + "</blockquote>")
      continue
    }

    // 列表
    if (block === "list") {
      const ordered = /^\s*\d+\.\s+/.test(line)
      const items = []
      while (i < lines.length) {
        const m = ordered
          ? /^\s*\d+\.\s+(.*)$/.exec(lines[i])
          : /^\s*[-*]\s+(.*)$/.exec(lines[i])
        if (!m) break
        items.push(m[1])
        i += 1
      }
      const tag = ordered ? "ol" : "ul"
      let html = "<" + tag + ">"
      for (const item of items) {
        html += "<li>" + inline(item) + "</li>"
      }
      html += "</" + tag + ">"
      out.push(html)
      continue
    }

    // 空行
    if (line.trim() === "") {
      i += 1
      continue
    }

    // 段落：连续的非特殊非空行合成一段
    const para = []
    while (
      i < lines.length &&
      lines[i].trim() !== "" &&
      !isBlockStart(lines[i], lines[i + 1])
    ) {
      para.push(lines[i])
      i += 1
    }
    if (para.length > 0) {
      out.push("<p>" + inline(para.join("\n")).replace(/\n/g, "<br>") + "</p>")
    } else {
      i += 1
    }
  }

  return out.join("\n")
}

module.exports = {
  escapeHtml: escapeHtml,
  inline: inline,
  isTableSeparator: isTableSeparator,
  splitTableRow: splitTableRow,
  toHtml: toHtml,
}
