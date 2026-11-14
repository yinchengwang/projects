"use strict"
/**
 * ui.js —— 公共界面构件
 *
 * 五页共用的几件套：页头、主卡、数字块、药丸、空态。
 * 层次靠这些构件统一起来，不靠每页各写各的样式。
 *
 * 零依赖：在浏览器里挂到 window.AlgoPlanUI，在 Node 里可以 require 取纯构造器。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanUI = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /** 极简 DOM 构造器——与 app.js 的 el 同签名 */
  function el(tag, attrs, children) {
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
   * @returns {HTMLElement}
   */
  function pageHead(opts) {
    const o = opts || {}
    const children = []
    if (o.eyebrow) {
      children.push(el("span", { class: "eyebrow", text: o.eyebrow }))
    }
    children.push(el("h2", { class: "page-title", text: o.title }))
    if (o.sub) {
      children.push(el("p", { class: "page-sub", text: o.sub }))
    }
    return el("section", { class: "page-head" }, children)
  }

  /**
   * 主卡：这一页「现在该做什么」的容器。
   * @param {{title?: string, sub?: string, children?: HTMLElement[]}} opts
   * @returns {HTMLElement}
   */
  function hero(opts) {
    const o = opts || {}
    const children = []
    if (o.title) {
      children.push(el("h2", { text: o.title }))
    }
    if (o.sub) {
      children.push(el("p", { class: "reason", text: o.sub }))
    }
    ;(o.children || []).forEach(function (child) {
      if (child) children.push(child)
    })
    return el("section", { class: "panel hero" }, children)
  }

  /**
   * 普通卡。
   * @param {{title?: string, children?: HTMLElement[], className?: string}} opts
   * @returns {HTMLElement}
   */
  function panel(opts) {
    const o = opts || {}
    const children = []
    if (o.title) {
      children.push(el("h2", { text: o.title }))
    }
    ;(o.children || []).forEach(function (child) {
      if (child) children.push(child)
    })
    const cls = "panel" + (o.className ? " " + o.className : "")
    return el("section", { class: cls }, children)
  }

  /**
   * 卡内嵌套块。
   * @param {{title?: string, children?: HTMLElement[]}} opts
   * @returns {HTMLElement}
   */
  function block(opts) {
    const o = opts || {}
    const children = []
    if (o.title) {
      children.push(el("p", { class: "block-title", text: o.title }))
    }
    ;(o.children || []).forEach(function (child) {
      if (child) children.push(child)
    })
    return el("div", { class: "block" }, children)
  }

  /**
   * 数字块阵列。
   * @param {Array<{label: string, value: string|number, hint?: string, tone?: string}>} items
   * @returns {HTMLElement}
   */
  function statGrid(items) {
    const cells = (items || []).map(function (item) {
      const children = [
        el("span", { class: "stat-value", text: String(item.value) }),
        el("span", { class: "stat-label", text: String(item.label) }),
      ]
      if (item.hint) {
        children.push(el("span", { class: "stat-hint", text: String(item.hint) }))
      }
      return el("div", { class: "stat" + (item.tone ? " tone-" + item.tone : "") }, children)
    })
    return el("div", { class: "stat-grid" }, cells)
  }

  /**
   * 药丸徽章：状态用颜色 + 文字双承载。
   * @param {string} text
   * @param {string} tone "ok" | "bad" | "warn" | "accent"，可空
   * @returns {HTMLElement}
   */
  function pill(text, tone) {
    return el("span", { class: "pill" + (tone ? " " + tone : ""), text: String(text) })
  }

  /**
   * 键值行：一行一个事实。
   * @param {Array<[string, string]>} pairs
   * @returns {HTMLElement}
   */
  function kv(pairs) {
    const rows = (pairs || []).map(function (pair) {
      return el("div", {}, [
        document.createTextNode(String(pair[0])),
        el("span", { text: String(pair[1] === undefined || pair[1] === null ? "" : pair[1]) }),
      ])
    })
    return el("div", { class: "kv" }, rows)
  }

  /**
   * 空状态：说清「没有」以及「为什么」。
   * @param {string} title
   * @param {string} hint
   * @returns {HTMLElement}
   */
  function empty(title, hint) {
    const children = [el("strong", { text: String(title) })]
    if (hint) {
      children.push(el("span", { text: String(hint) }))
    }
    return el("div", { class: "empty" }, children)
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
  }
})
