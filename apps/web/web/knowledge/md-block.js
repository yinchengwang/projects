"use strict"
/**
 * md-block.js —— 把 Markdown 正文渲染成页面节点
 *
 * shared/core/md2html.js 输出的是带 class 的 HTML 字符串，Web 侧 CSS 直接生效，
 * 所以只需要把它塞进容器，不需要小程序那套内联样式改写。
 *
 * 注意：Node 测试桩里没有真正的 HTML 解析器，innerHTML 只是普通属性；
 * 这里不做能力探测分支，赋值即可 —— 生产与测试两侧都安全。
 */

const ui = require("../../shared/ui.js")

/**
 * 生成承载 Markdown 正文的节点。
 * @param {string} html md2html() 的输出
 * @returns {object} DOM 节点（class="kh-md"）
 */
function mdBlock (html) {
  const node = ui.el("div", { class: "kh-md" })
  const safe = html === undefined || html === null ? "" : String(html)
  if (safe) node.innerHTML = safe
  return node
}

module.exports = { mdBlock: mdBlock }
