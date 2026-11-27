"use strict"
/**
 * ui.js —— 公共界面构件
 *
 * 零依赖：在浏览器里挂到 window.AlgoPlanUI，在 Node 里可以 require 取纯构造器。
 *
 * 类名必须与 web/style.css 对齐，改这里就要同步改那里。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanUI = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /**
   * 极简 DOM 构造器。
   * @param {string} tag
   * @param {object} [attrs] 支持 class / text / on* 事件 / 其余走 setAttribute
   * @param {Array} [children]
   * @returns {object} DOM 元素
   */
  function el (tag, attrs, children) {
    const node = document.createElement(tag)
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        if (key === "class") node.className = attrs[key]
        else if (key === "text") node.textContent = attrs[key]
        else if (key.indexOf("on") === 0 && typeof attrs[key] === "function") {
          node.addEventListener(key.slice(2).toLowerCase(), attrs[key])
        } else node.setAttribute(key, attrs[key])
      })
    }
    ;(children || []).forEach(function (child) {
      if (!child) return
      node.appendChild(child)
    })
    return node
  }

  /**
   * 页头：眉标 → 标题 → 副标。每页的第一眼。
   * @param {{eyebrow?: string, title: string, sub?: string}} opts
   */
  function pageHead (opts) {
    const o = opts || {}
    const children = []
    if (o.eyebrow) children.push(el("span", { class: "eyebrow", text: o.eyebrow }))
    children.push(el("h2", { class: "page-title", text: o.title }))
    if (o.sub) children.push(el("p", { class: "page-sub", text: o.sub }))
    return el("section", { class: "page-head" }, children)
  }

  /**
   * 主卡：这一页「现在该做什么」的容器。
   * @param {{title?: string, sub?: string, children?: Array}} opts
   */
  function hero (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("h2", { text: o.title }))
    if (o.sub) children.push(el("p", { class: "reason", text: o.sub }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    return el("section", { class: "panel hero" }, children)
  }

  /**
   * 普通卡。
   * @param {{title?: string, children?: Array, className?: string}} opts
   */
  function panel (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("h2", { text: o.title }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    const cls = "panel" + (o.className ? " " + o.className : "")
    return el("section", { class: cls }, children)
  }

  /**
   * 卡内嵌套块。
   * @param {{title?: string, children?: Array}} opts
   */
  function block (opts) {
    const o = opts || {}
    const children = []
    if (o.title) children.push(el("p", { class: "block-title", text: o.title }))
    ;(o.children || []).forEach(function (child) { if (child) children.push(child) })
    return el("div", { class: "block" }, children)
  }

  /**
   * 数字块阵列。
   * @param {Array<{label: string, value: string|number, hint?: string, tone?: string}>} items
   */
  function statGrid (items) {
    const cells = (items || []).map(function (item) {
      const children = [
        el("span", { class: "stat-value", text: String(item.value) }),
        el("span", { class: "stat-label", text: String(item.label) })
      ]
      if (item.hint) children.push(el("span", { class: "stat-hint", text: item.hint }))
      return el("div", { class: "stat" + (item.tone ? " tone-" + item.tone : "") }, children)
    })
    return el("div", { class: "stat-grid" }, cells)
  }

  /**
   * 药丸徽章：状态用颜色 + 文字双承载。
   * @param {string} text
   * @param {string} [tone] "ok" | "bad" | "warn" | "accent"
   */
  function pill (text, tone) {
    return el("span", { class: "pill" + (tone ? " " + tone : ""), text: String(text) })
  }

  /**
   * 键值行：一行一个事实。
   * @param {Array<[string, string]>} pairs
   */
  function kv (pairs) {
    const rows = (pairs || []).map(function (pair) {
      return el("div", {}, [
        document.createTextNode(String(pair[0])),
        el("span", { text: String(pair[1] === undefined || pair[1] === null ? "" : pair[1]) })
      ])
    })
    return el("div", { class: "kv" }, rows)
  }

  /**
   * 空状态：说清「没有」以及「为什么」。
   * @param {string} title
   * @param {string} [hint]
   */
  function empty (title, hint) {
    const children = [el("strong", { text: String(title) })]
    if (hint) children.push(el("span", { text: String(hint) }))
    return el("div", { class: "empty" }, children)
  }

  /**
   * 进度条。
   * @param {number} percent 0..100，超出自动夹紧
   * @param {{color?: string, height?: number, label?: string}} [opts]
   * @returns {object} DOM 节点
   */
  function progressBar (percent, opts) {
    const o = opts || {}
    let p = Number(percent)
    if (!isFinite(p)) p = 0
    if (p < 0) p = 0
    if (p > 100) p = 100
    const rounded = Math.round(p)

    const fill = el("div", { class: "progress-fill" })
    // 浏览器里 style 是只读的 CSSStyleDeclaration，直接整体赋值无效；
    // 测试桩里没有 style 属性，先兜一个空对象，两种环境都成立。
    fill.style = fill.style || {}
    fill.style.width = rounded + "%"
    if (o.color) fill.style.background = o.color
    if (o.height) fill.style.height = o.height + "px"

    const label = el("span", { class: "progress-label", text: rounded + "%" })
    const children = [fill]
    if (o.label) children.unshift(el("span", { class: "progress-name", text: o.label }))

    const root = el("div", { class: "progress" }, children)
    if (o.height) {
      root.style = root.style || {}
      root.style.height = o.height + "px"
    }
    root.appendChild(label)
    return root
  }

  /**
   * 把页面挂到容器上。
   *
   * 页面形状：{ data, render(state) → 节点, on? }
   * 挂载后 page.state 是页面状态的浅拷贝，事件回调里改它再调 rerender 即可。
   *
   * @param {object} container 容器节点
   * @param {object} page 页面定义
   */
  function mount (container, page) {
    page.state = Object.assign({}, page.data)
    renderInto(container, page)
    return page
  }

  /**
   * 重新渲染：清空容器后用当前 state 重画。
   * @param {object} container
   * @param {object} page
   */
  function rerender (container, page) {
    renderInto(container, page)
    return page
  }

  function renderInto (container, page) {
    while (container.children && container.children.length) {
      container.children.pop()
    }
    const node = page.render(page.state)
    if (node) container.appendChild(node)
  }

  return {
    el: el,
    pageHead: pageHead,
    hero: hero,
    panel: panel,
    block: block,
    statGrid: statGrid,
    pill: pill,
    kv: kv,
    empty: empty,
    progressBar: progressBar,
    mount: mount,
    rerender: rerender
  }
})
