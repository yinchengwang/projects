"use strict"
/**
 * dom-stub.js —— Node 下的最小 DOM 替身
 *
 * 只实现构件层用到的 API：createElement / createTextNode / appendChild /
 * setAttribute / addEventListener / textContent / className / innerHTML。
 * 够用来断言渲染结构，不做完整 DOM 模拟。
 */

/**
 * 安装 DOM 替身到 global。
 * @returns {object} { nodes: 创建过的元素数组 }
 */
function install () {
  const nodes = []
  const byId = {}

  function makeNode (tag) {
    const node = {
      tagName: String(tag).toUpperCase(),
      children: [],
      attributes: {},
      listeners: {},
      _text: "",
      get className () { return this.attributes.class || "" },
      set className (v) { this.attributes.class = v },
      get textContent () {
        if (this.children.length === 0) return this._text
        return this.children.map(function (c) { return c.textContent }).join("")
      },
      set textContent (v) {
        this.children = []
        this._text = String(v)
      },
      appendChild: function (child) {
        this.children.push(child)
        return child
      },
      removeChild: function (child) {
        const i = this.children.indexOf(child)
        if (i >= 0) this.children.splice(i, 1)
        return child
      },
      setAttribute: function (k, v) {
        this.attributes[k] = String(v)
        if (k === "id" && global.document && global.document._register) {
          global.document._register(this, String(v))
        }
      },
      getAttribute: function (k) {
        return Object.prototype.hasOwnProperty.call(this.attributes, k) ? this.attributes[k] : null
      },
      addEventListener: function (type, fn) {
        if (!this.listeners[type]) this.listeners[type] = []
        this.listeners[type].push(fn)
      },
      /** 触发已注册的事件监听器 */
      fire: function (type, event) {
        const list = this.listeners[type] || []
        list.forEach(function (fn) { fn(event || {}) })
      }
    }
    nodes.push(node)
    return node
  }

  global.document = {
    createElement: makeNode,
    createTextNode: function (text) {
      return { tagName: "#text", children: [], _text: String(text), textContent: String(text) }
    },
    body: makeNode("body"),
    documentElement: makeNode("html"),
    /** 桩：按 id 找已创建的元素。platform.js 会用它挂浮层。 */
    querySelector: function (sel) {
      if (sel.charAt(0) === "#") return byId[sel.slice(1)] || null
      return null
    },
    /** 记录 id，方便 querySelector 找到 */
    _register: function (node, id) { byId[id] = node; return node }
  }

  return { nodes: nodes }
}

module.exports = { install: install }
