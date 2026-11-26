/**
 * md2html.js —— 极简 Markdown → HTML 解析器
 *
 * 从 codetools.js 抽出，独立成模块。knowledge-hub 的图解系列（文章阅读器）
 * 与 programmer-workbench 的技术笔记共用这一个解析器，不引入工具箱依赖。
 *
 * 只覆盖笔记常用语法：标题、粗斜体、删除线、行内码、围栏代码块、
 * 引用、链接、列表、段落。不做完整 CommonMark 兼容。
 *
 * 注意：输出带 class 名的 HTML 只能用于浏览器。小程序 <rich-text> 不认
 * WXSS 类名，需经 md.js 改写为内联样式后再使用。
 */

/**
 * @param {string} md Markdown 原文
 * @returns {string} 带 class 的 HTML（md-code / md-inline-code）
 */
function md2html (md) {
  if (!md) return ''

  const escape = s => s.replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]))

  let html = escape(String(md))

  // 围栏代码块先占位，避免内部语法被后续规则误处理
  const codeBlocks = []
  html = html.replace(/```(\w*)\n?([\s\S]*?)```/g, function (_, lang, code) {
    const idx = codeBlocks.length
    const cls = lang ? ' class="md-code md-lang-' + lang + '"' : ' class="md-code"'
    codeBlocks.push('<pre' + cls + '><code>' + code.replace(/\n$/, '') + '</code></pre>')
    return '\u0000CODE' + idx + '\u0000'
  })

  html = html.replace(/`([^`]+)`/g, '<code class="md-inline-code">$1</code>')
  html = html.replace(/^###### (.*)$/gm, '<h6>$1</h6>')
  html = html.replace(/^##### (.*)$/gm, '<h5>$1</h5>')
  html = html.replace(/^#### (.*)$/gm, '<h4>$1</h4>')
  html = html.replace(/^### (.*)$/gm, '<h3>$1</h3>')
  html = html.replace(/^## (.*)$/gm, '<h2>$1</h2>')
  html = html.replace(/^# (.*)$/gm, '<h1>$1</h1>')
  html = html.replace(/^&gt;\s?(.*)$/gm, '<blockquote>$1</blockquote>')
  html = html.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
  html = html.replace(/\*(.+?)\*/g, '<em>$1</em>')
  html = html.replace(/~~(.+?)~~/g, '<del>$1</del>')
  html = html.replace(/\[(.+?)\]\((.+?)\)/g, '<a href="$2">$1</a>')
  html = html.replace(/^[-*] (.*)$/gm, '<li>$1</li>')
  html = html.replace(/(<li>[\s\S]*?<\/li>)/g, '<ul>$1</ul>')

  html = html.split(/\n\n+/).map(function (p) {
    if (!p.trim()) return ''
    if (p.charAt(0) === '<') return p
    if (p.indexOf('\u0000CODE') >= 0) return p
    return '<p>' + p + '</p>'
  }).join('\n')

  // 还原代码块
  html = html.replace(/\u0000CODE(\d+)\u0000/g, function (_, idx) {
    return codeBlocks[Number(idx)]
  })

  return html
}

module.exports = { md2html: md2html }
