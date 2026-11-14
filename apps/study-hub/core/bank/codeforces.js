"use strict"
/**
 * codeforces.js —— Codeforces 题面页 HTML → 结构化题目（纯函数，无 IO、无网络）
 *
 * 题面页的结构（2026-11 实测）：
 *   <div class="problem-statement">
 *     <div class="header">            题号、标题、时间/内存限制
 *     <div>                            题面正文（若干 <p>）
 *     <div class="input-specification">
 *     <div class="output-specification">
 *     <div class="sample-tests">
 *       <div class="sample-test">
 *         <div class="input"><pre>…</pre></div>
 *         <div class="output"><pre>…</pre></div>
 *
 * 只做「样例」这一层：Codeforces 的完整测试数据要登录才能下 zip（403），
 * 公开能拿到的只有题面里写着的 Examples。
 *
 * 数学公式在 CF 页面上是 <span class="tex-span">…</span>（渲染后的 MathJax 结果），
 * 直接去标签留文字就行，不用去解 TeX。
 */

/** HTML 实体表（只列题面里真会出现的） */
const ENTITIES = {
  "&lt;": "<",
  "&gt;": ">",
  "&amp;": "&",
  "&quot;": '"',
  "&apos;": "'",
  "&#39;": "'",
  "&nbsp;": " ",
  "&times;": "×",
  "&minus;": "−",
  "&le;": "≤",
  "&ge;": "≥",
  "&ne;": "≠",
  "&hellip;": "…",
  "&mdash;": "—",
  "&ndash;": "–",
  "&deg;": "°",
  "&plusmn;": "±",
  "&infin;": "∞",
  "&sum;": "∑",
  "&prod;": "∏",
  "&rarr;": "→",
  "&larr;": "←",
}

/**
 * 解 HTML 实体。
 * @param {string} text
 * @returns {string}
 */
function decodeEntities(text) {
  let s = String(text === null || text === undefined ? "" : text)
  s = s.replace(/&#(\d+);/g, function (all, code) {
    return String.fromCharCode(Number(code))
  })
  s = s.replace(/&[a-zA-Z]+;/g, function (all) {
    return Object.prototype.hasOwnProperty.call(ENTITIES, all) ? ENTITIES[all] : all
  })
  return s
}

/**
 * HTML → 纯文本。段落之间留空行，<br> 换行。
 * @param {string} html
 * @returns {string}
 */
function htmlToText(html) {
  let s = String(html === null || html === undefined ? "" : html)
  s = s.replace(/<\s*br\s*\/?\s*>/gi, "\n")
  s = s.replace(/<\s*\/\s*(p|div|li|tr|h[1-6])\s*>/gi, "\n\n")
  s = s.replace(/<[^>]+>/g, "")
  s = decodeEntities(s)
  // 连续三行以上空行压成两行
  s = s.replace(/\n{3,}/g, "\n\n")
  return s.trim()
}

/**
 * 从 html 里找下一个 <div ...> 的位置与内容起点。
 * @param {string} html
 * @param {number} from
 * @returns {{start: number, contentStart: number}|null}
 */
function nextDiv(html, from) {
  const re = /<div\b[^>]*>/gi
  re.lastIndex = Math.max(0, from)
  const m = re.exec(html)
  if (!m) return null
  return { start: m.index, contentStart: m.index + m[0].length }
}

/**
 * 从 contentStart 处开始算 div 配对，返回外层 div 的结束位置（</div> 的下标）。
 * @param {string} html
 * @param {number} contentStart
 * @returns {number} 找不到返回 -1
 */
function divBlockEnd(html, contentStart) {
  const re = /<div\b[^>]*>|<\/div>/gi
  re.lastIndex = contentStart
  let depth = 1
  let m
  while ((m = re.exec(html))) {
    if (m[0].toLowerCase() === "</div>") {
      depth -= 1
      if (depth === 0) return m.index
    } else {
      depth += 1
    }
  }
  return -1
}

/**
 * 取出所有 class 里带某个词的 div 的内容。
 * @param {string} html
 * @param {string} word class 名里要找的词
 * @returns {string[]} 各个 div 的内部 HTML
 */
function divsByClass(html, word) {
  const out = []
  const re = new RegExp('<div\\b[^>]*class="[^"]*\\b' + word + '\\b[^"]*"[^>]*>', "gi")
  let m
  while ((m = re.exec(html))) {
    const contentStart = m.index + m[0].length
    const end = divBlockEnd(html, contentStart)
    out.push(html.slice(contentStart, end === -1 ? html.length : end))
    if (end !== -1) re.lastIndex = end
  }
  return out
}

/**
 * 取第一个 class 里带某个词的 div 的内容。
 * @param {string} html
 * @param {string} word
 * @returns {string} 找不到返回 ""
 */
function firstDivByClass(html, word) {
  const list = divsByClass(html, word)
  return list.length > 0 ? list[0] : ""
}

/**
 * 取 <pre> 里的文本。
 * @param {string} html
 * @returns {string[]}
 */
function preTexts(html) {
  const out = []
  const re = /<pre[^>]*>([\s\S]*?)<\/pre>/gi
  let m
  while ((m = re.exec(html))) {
    // CF 的 <pre> 里用 <br /> 分行
    let inner = m[1].replace(/<\s*br\s*\/?\s*>/gi, "\n")
    inner = inner.replace(/<[^>]+>/g, "")
    inner = decodeEntities(inner)
    out.push(inner.replace(/\n+$/, ""))
  }
  return out
}

/**
 * 取「属性名 + 值」里的值，比如 time-limit 块的 "1 second"。
 * 写法是 <div class="property-title">time limit per test</div>1 second，
 * 所以只取 property-title 之后、下一个标签之前的纯文本。
 * @param {string} html
 * @param {string} word
 * @returns {string}
 */
function propertyValue(html, word) {
  const re = new RegExp(
    '<div[^>]*class="[^"]*\\b' + word + '\\b[^"]*"[^>]*>([\\s\\S]*?)<div[^>]*class="[^"]*\\bproperty-title\\b[^"]*"[^>]*>([\\s\\S]*?)</div>([\\s\\S]*?)<div',
    "i"
  )
  const m = re.exec(html)
  if (!m) return ""
  return htmlToText(m[3]).split("\n")[0].trim()
}

/**
 * 解析一张 Codeforces 题面页。
 * @param {string} html
 * @returns {{title: string, timeLimit: string, memoryLimit: string, statement: string, samples: Array<{input: string, output: string}>}}
 */
function parseProblemPage(html) {
  const src = String(html === null || html === undefined ? "" : html)
  const block = firstDivByClass(src, "problem-statement")
  if (block === "") {
    return { title: "", timeLimit: "", memoryLimit: "", statement: "", samples: [] }
  }

  // 标题：<div class="title">A. Theatre Square</div>
  const titleMatch = /<div[^>]*class="[^"]*\btitle\b[^"]*"[^>]*>([\s\S]*?)<\/div>/i.exec(block)
  const title = titleMatch ? htmlToText(titleMatch[1]) : ""

  const timeLimit = propertyValue(block, "time-limit")
  const memoryLimit = propertyValue(block, "memory-limit")

  // 题面正文：header 之后、input-specification 之前的那一个无 class 的 div
  let statement = ""
  const headerIdx = /<div\b[^>]*class="[^"]*\bheader\b[^"]*"[^>]*>/i.exec(block)
  if (headerIdx) {
    const headerEnd = divBlockEnd(block, headerIdx.index + headerIdx[0].length)
    const body = nextDiv(block, headerEnd === -1 ? headerIdx.index : headerEnd)
    if (body) {
      const bodyEnd = divBlockEnd(block, body.contentStart)
      statement = htmlToText(block.slice(body.contentStart, bodyEnd === -1 ? block.length : bodyEnd))
    }
  }

  // 样例：每个 sample-test 块里，第一个 <pre> 是输入，第二个是输出
  const samples = []
  divsByClass(block, "sample-test").forEach(function (sampleBlock) {
    const pres = preTexts(sampleBlock)
    if (pres.length >= 2) {
      samples.push({ input: pres[0], output: pres[1] })
    }
  })

  return {
    title: title,
    timeLimit: timeLimit,
    memoryLimit: memoryLimit,
    statement: statement,
    samples: samples,
  }
}

/**
 * 从题面正文里切出「输入 / 输出」两段说明，拼成题面正文的一部分。
 * @param {string} html 题面页整页 HTML
 * @returns {string}
 */
function extractSpec(html) {
  // 去掉 section-title（就是 "Input" / "Output" 那行标题），正文里我们自己加标题
  const clean = function (block) {
    return block.replace(
      /<div[^>]*class="[^"]*\bsection-title\b[^"]*"[^>]*>[\s\S]*?<\/div>/gi,
      ""
    )
  }
  const input = htmlToText(clean(firstDivByClass(html, "input-specification")))
  const output = htmlToText(clean(firstDivByClass(html, "output-specification")))
  const parts = []
  if (input) parts.push("**输入**\n\n" + input)
  if (output) parts.push("**输出**\n\n" + output)
  return parts.join("\n\n")
}

module.exports = {
  ENTITIES: ENTITIES,
  decodeEntities: decodeEntities,
  htmlToText: htmlToText,
  nextDiv: nextDiv,
  divBlockEnd: divBlockEnd,
  divsByClass: divsByClass,
  firstDivByClass: firstDivByClass,
  preTexts: preTexts,
  parseProblemPage: parseProblemPage,
  extractSpec: extractSpec,
}
