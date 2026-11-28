"use strict"
/**
 * md-block.js —— 把 Markdown 正文渲染成页面节点（aitimer 模块本地副本）
 *
 * 方案 A：不复用 knowledge 模块的 md-block.js，避免跨模块耦合与样式串味。
 * 这里复制同样的模式，但类名用 aitimer-md，样式定义在 aitimer/style.css。
 *
 * 正文进来先过三道处理（顺序有讲究）：
 * 1. wikilink 归一：[[pg-hot-update]] → pg-hot-update。md2html 不认识这语法，
 *    不处理的话会原样显示成 "[[pg-hot-update]]"。
 * 2. md2html 转 HTML：它只转义 & < >，正文里的标签会变文本，风险低。
 * 3. 链接加固：md2html 的 [文字](链接) 不校验 href，javascript: 伪协议会原样
 *    落进 DOM。插入 DOM 后遍历 a 元素，剥掉危险协议，剩下的补 target=_blank
 *    rel="noopener"。
 */

const ui = require("../../shared/ui.js")

/** 危险协议前缀：这些协议的 href 一律丢掉 */
const DANGEROUS_PROTOCOL = /^\s*(javascript|vbscript|data)\s*:/i

/**
 * 把 [[wikilink]] 归一成普通文本。
 *
 * @param {string} text 正文原文
 * @returns {string}
 */
function stripWikiLinks (text) {
  if (!text) return ""
  return String(text).replace(/\[\[([^\]]+)\]\]/g, "$1")
}

/**
 * 生成承载 Markdown 正文的节点。
 *
 * @param {string} md Markdown 原文
 * @param {object} [md2html] Markdown 解析器（md2html(md) → HTML 字符串）
 * @returns {object} DOM 节点（class="aitimer-md"）
 */
function mdBlock (md, md2html) {
  const node = ui.el("div", { class: "aitimer-md" })
  if (!md || typeof md2html !== "function") return node
  const html = md2html(stripWikiLinks(md))
  if (!html) return node
  node.innerHTML = html
  hardenLinks(node)
  return node
}

/**
 * 加固正文里的链接：剥掉危险协议，外链补 target/rel。
 *
 * @param {object} container 正文容器
 */
function hardenLinks (container) {
  const anchors = container.querySelectorAll ? container.querySelectorAll("a") : []
  Array.prototype.forEach.call(anchors, function (a) {
    const href = a.getAttribute("href") || ""
    if (DANGEROUS_PROTOCOL.test(href)) {
      a.removeAttribute("href")
      a.setAttribute("data-blocked-href", href)
      return
    }
    if (!href) return
    // 站内相对链接保持同窗口打开，只有外链才新开标签
    if (/^https?:\/\//i.test(href)) {
      a.setAttribute("target", "_blank")
      a.setAttribute("rel", "noopener noreferrer")
    }
  })
}

/**
 * 把一段 Markdown 摘成纯文本，用于简介、标题这类一行的场合。
 *
 * 服务端的系列简介取自 README 首段，里面会带 **粗体**、`行内码` 之类标记，
 * 直接塞进 textContent 会原样显示星号与反引号，这里先剥掉。
 *
 * @param {string} text 原文
 * @returns {string} 纯文本
 */
function plainText (text) {
  if (!text) return ""
  return String(text)
    .replace(/`([^`]+)`/g, "$1")
    .replace(/\*\*([^*]+)\*\*/g, "$1")
    .replace(/\*([^*]+)\*/g, "$1")
    .replace(/~~([^~]+)~~/g, "$1")
    .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
    .replace(/\[\[([^\]]+)\]\]/g, "$1")
    .trim()
}

module.exports = {
  mdBlock: mdBlock,
  stripWikiLinks: stripWikiLinks,
  hardenLinks: hardenLinks,
  plainText: plainText
}
