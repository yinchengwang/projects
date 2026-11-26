/**
 * md.js —— Markdown 渲染（供 rich-text 使用）
 *
 * 为什么需要这一层：小程序 <rich-text nodes="..."> 渲染的内容**不受 WXSS 类名影响**，
 * 只有内联 style 生效。codetools.md2html 输出的是带 class 的 HTML，这里把 class
 * 改写成内联样式，并按当前主题注入配色。
 */

const md2htmlModule = require('./md2html.js')
const md2html = md2htmlModule.md2html

/** 主题相关的内联样式 */
function styles (isDark) {
  const codeBg = isDark ? '#1D2430' : '#F1F3F5'
  const codeFg = isDark ? '#E6EAF0' : '#0F172A'
  const quoteBar = isDark ? '#33404F' : '#CBD5E1'

  return {
    code: 'display:block;margin:8px 0;padding:12px;border-radius:6px;background:' +
      codeBg + ';color:' + codeFg +
      ';font-family:Menlo,Consolas,monospace;font-size:12px;line-height:1.6;' +
      'white-space:pre-wrap;word-break:break-all;',
    inline: 'padding:1px 4px;border-radius:3px;background:' + codeBg +
      ';color:' + codeFg + ';font-family:Menlo,Consolas,monospace;font-size:13px;',
    quote: 'margin:8px 0;padding:4px 0 4px 10px;border-left:3px solid ' + quoteBar +
      ';color:' + (isDark ? '#94A3B8' : '#64748B') + ';',
    link: 'color:' + (isDark ? '#38BDF8' : '#0284C7') + ';text-decoration:none;'
  }
}

/**
 * 渲染 Markdown 为带内联样式的 HTML
 * @param {string} md
 * @param {boolean} isDark 当前是否深色主题
 * @returns {string} 可直接喂给 <rich-text nodes="...">
 */
function render (md, isDark) {
  if (!md) return ''
  const s = styles(!!isDark)

  let html = md2html(md)

  // 代码块：<pre class="md-code md-lang-js"> → <pre style="...">
  html = html.replace(/<pre class="md-code[^"]*">/g, '<pre style="' + s.code + '">')
  // 行内码
  html = html.replace(/<code class="md-inline-code">/g, '<code style="' + s.inline + '">')
  // 引用
  html = html.replace(/<blockquote>/g, '<blockquote style="' + s.quote + '">')
  // 链接
  html = html.replace(/<a href="/g, '<a style="' + s.link + '" href="')

  return html
}

/** 纯文本预览（去掉 Markdown 标记，用于列表摘要） */
function preview (md, len) {
  const max = len || 60
  if (!md) return ''
  const plain = String(md)
    .replace(/```[\s\S]*?```/g, ' [代码] ')
    .replace(/`([^`]+)`/g, '$1')
    .replace(/^#{1,6}\s+/gm, '')
    .replace(/^>\s?/gm, '')
    .replace(/^[-*]\s+/gm, '')
    .replace(/\*\*(.+?)\*\*/g, '$1')
    .replace(/\*(.+?)\*/g, '$1')
    .replace(/~~(.+?)~~/g, '$1')
    .replace(/\[(.+?)\]\((.+?)\)/g, '$1')
    .replace(/\s+/g, ' ')
    .trim()
  return plain.length > max ? plain.slice(0, max) + '…' : plain
}

module.exports = {
  render: render,
  preview: preview
}
