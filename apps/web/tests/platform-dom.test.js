"use strict"
/**
 * platform-dom.test.js —— 浮层在真实 DOM 上的行为
 *
 * 为什么单独测：dom-stub.js 的 children 是**数组**，有 indexOf/splice；
 * 而浏览器里 children 是 HTMLCollection —— 没有这两个方法。
 * 于是「浮层关不掉」这类 bug 在测试里是绿的，在浏览器里是坏的。
 *
 * 这套用例用一个 children 只读（没有 indexOf/splice）的桩，
 * 把浏览器的真实约束搬进测试。
 */

const test = require("node:test")
const assert = require("node:assert")

/** 装一个接近浏览器的 DOM 桩 */
function installDom () {
  const nodes = []
  function makeNode (tag) {
    const node = {
      tagName: String(tag).toUpperCase(),
      attributes: {},
      listeners: {},
      _text: "",
      _parent: null,
      _children: [],
      get className () { return this.attributes.class || "" },
      set className (v) { this.attributes.class = v },
      // 浏览器里就是 parentNode —— 代码用的是这个
      get parentNode () { return this._parent },
      get textContent () {
        if (this._children.length === 0) return this._text
        return this._children.map(function (c) { return c.textContent }).join("")
      },
      set textContent (v) {
        this._children = []
        this._text = String(v)
      },
      appendChild: function (child) {
        if (child._parent) child._parent.removeChild(child)
        this._children.push(child)
        child._parent = this
        return child
      },
      removeChild: function (child) {
        const i = this._children.indexOf(child)
        if (i >= 0) { this._children.splice(i, 1); child._parent = null }
        return child
      },
      setAttribute: function (k, v) { this.attributes[k] = String(v) },
      getAttribute: function (k) {
        return Object.prototype.hasOwnProperty.call(this.attributes, k) ? this.attributes[k] : null
      },
      addEventListener: function (type, fn) {
        if (!this.listeners[type]) this.listeners[type] = []
        this.listeners[type].push(fn)
      },
      fire: function (type, event) {
        (this.listeners[type] || []).forEach(function (fn) { fn(event || {}) })
      }
    }
    // 关键：children 暴露成只读视图 —— 跟浏览器一样，没有 indexOf / splice
    Object.defineProperty(node, "children", {
      get: function () {
        return {
          length: node._children.length,
          item: function (i) { return node._children[i] },
          forEach: function (fn) { node._children.forEach(fn) }
        }
      },
      configurable: true
    })
    nodes.push(node)
    return node
  }

  const body = makeNode("body")
  global.document = {
    createElement: makeNode,
    createTextNode: function (text) {
      const n = makeNode("#text")
      n._text = String(text)
      return n
    },
    body: body,
    documentElement: makeNode("html"),
    querySelector: function () { return null },
    addEventListener: function () {}
  }
  global.window = {
    addEventListener: function () {},
    matchMedia: function () { return { matches: false, addEventListener: function () {} } },
    setTimeout: setTimeout,
    clearTimeout: clearTimeout
  }
  return { body: body, makeNode: makeNode }
}

test("showToast 的提示条会自己消失（children 是 HTMLCollection 时）", async function () {
  const dom = installDom()
  const platform = require("../shared/platform.js")
  const node = platform.showToast({ title: "测试" })
  assert.ok(node, "应返回提示条节点")
  // 提示条挂在一个常驻的 host 容器里（host 会留在 body 上，这是设计）
  const host = dom.body._children[0]
  assert.strictEqual(host._children.length, 1, "提示条应已挂到 host 上")

  await new Promise(function (r) { setTimeout(r, 2200) })
  assert.strictEqual(host._children.length, 0,
    "提示条没有消失 —— 用的是 children.indexOf/splice，浏览器里会抛")
})

test("对话框关闭后遮罩会被移除", async function () {
  const dom = installDom()
  const platform = require("../shared/platform.js")
  const p = platform.showModal({ title: "测试", content: "内容" })
  await new Promise(function (r) { setTimeout(r, 20) })
  assert.ok(dom.body._children.length > 0, "遮罩应已挂上")

  // 找取消按钮并点它
  const mask = dom.body._children[0]
  const dialog = mask._children[0]
  const actions = dialog._children[dialog._children.length - 1]
  const cancel = actions._children[0]
  cancel.fire("click")

  const res = await p
  assert.strictEqual(res.confirm, false)
  assert.strictEqual(dom.body._children.length, 0,
    "遮罩没被移除 —— 整个应用会被一层看不见的盖子挡住")
})

test("对话框确认后遮罩也会被移除", async function () {
  const dom = installDom()
  const platform = require("../shared/platform.js")
  const p = platform.showModal({ title: "测试", editable: true })
  await new Promise(function (r) { setTimeout(r, 20) })

  const mask = dom.body._children[0]
  const dialog = mask._children[0]
  const actions = dialog._children[dialog._children.length - 1]
  const confirm = actions._children[1]
  confirm.fire("click")

  const res = await p
  assert.strictEqual(res.confirm, true)
  assert.strictEqual(dom.body._children.length, 0, "遮罩没被移除")
})
