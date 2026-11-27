"use strict"
/**
 * kh-boot.test.js —— 在 Node 里模拟浏览器启动 knowledge 模块
 *
 * 为什么需要这一组：
 *   Node 单测走的是原生 require，绕过了 cjs-loader；而浏览器里页面模块的
 *   `require()` 只能拿到「已预载」的模块，否则抛「同步 require 拿到未加载的模块」。
 *   所以「单测全绿」并不等于「浏览器能跑」—— 这一组补的就是这条接缝。
 *
 * 做法：装最小 DOM → fetch 桩从磁盘读文件 → 按 shell.html 的顺序执行
 *       ui.js / platform.js / charts.js / cjs-loader.js / knowledge/browser.js。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")
const vm = require("node:vm")

const WEB = path.join(__dirname, "..", "web")
const SHARED = path.join(__dirname, "..", "shared")

/**
 * URL 路径 → 磁盘路径。
 * /shared/* 在 apps/web/shared，其余在 apps/web/web。
 * @param {string} urlPath
 * @returns {string}
 */
function toDisk (urlPath) {
  const clean = String(urlPath).split("?")[0]
  if (clean.indexOf("/shared/") === 0) return path.join(SHARED, clean.slice("/shared/".length))
  return path.join(WEB, clean.replace(/^\//, ""))
}

/** 最小 2d 上下文：draw 只要不抛异常即可 */
function makeCtx () {
  const noop = function () {}
  return {
    scale: noop, clearRect: noop, beginPath: noop, moveTo: noop, lineTo: noop, arc: noop,
    closePath: noop, fill: noop, stroke: noop,
    strokeStyle: "", fillStyle: "", lineWidth: 1, globalAlpha: 1
  }
}

/** 最小 DOM 节点 */
function makeNode (tag) {
  return {
    tagName: String(tag).toUpperCase(),
    children: [],
    attributes: {},
    listeners: {},
    style: {},
    className: "",
    _text: "",
    get textContent () {
      return this.children.length ? this.children.map(function (c) { return c.textContent }).join("") : this._text
    },
    set textContent (v) { this.children = []; this._text = String(v) },
    appendChild: function (child) { this.children.push(child); return child },
    removeChild: function (child) {
      const i = this.children.indexOf(child)
      if (i >= 0) this.children.splice(i, 1)
      return child
    },
    setAttribute: function (k, v) { this.attributes[k] = String(v) },
    getAttribute: function (k) { return this.attributes[k] === undefined ? null : this.attributes[k] },
    addEventListener: function (type, fn) {
      if (!this.listeners[type]) this.listeners[type] = []
      this.listeners[type].push(fn)
    },
    getBoundingClientRect: function () { return { width: 320, height: 240, top: 0, left: 0 } },
    getContext: function () { return makeCtx() }
  }
}

/**
 * 造一个浏览器沙箱并执行模块入口。
 * @returns {object} { sandbox, appMain }
 */
function bootModule () {
  const appMain = makeNode("main")
  const body = makeNode("body")

  const documentStub = {
    body: body,
    documentElement: makeNode("html"),
    createElement: makeNode,
    createTextNode: function (t) { return { tagName: "#text", children: [], textContent: String(t) } },
    getElementById: function (id) { return id === "app-main" ? appMain : null },
    querySelector: function () { return null },
    addEventListener: function () {}
  }

  const memory = new Map()
  const localStorageStub = {
    getItem: function (k) { return memory.has(k) ? memory.get(k) : null },
    setItem: function (k, v) { memory.set(k, String(v)) },
    removeItem: function (k) { memory.delete(k) },
    clear: function () { memory.clear() },
    key: function (i) { const keys = Array.from(memory.keys()); return i < keys.length ? keys[i] : null },
    get length () { return memory.size }
  }

  const sandbox = {
    console: console,
    setTimeout: setTimeout,
    clearTimeout: clearTimeout,
    setInterval: setInterval,
    clearInterval: clearInterval,
    document: documentStub,
    localStorage: localStorageStub,
    navigator: {},
    devicePixelRatio: 2,
    location: { pathname: "/knowledge/pages/dashboard/index" },
    history: { pushState: function () {}, replaceState: function () {}, back: function () {} },
    addEventListener: function () {},
    matchMedia: function () { return { matches: false, addEventListener: function () {} } },
    URLSearchParams: URLSearchParams,
    JSON: JSON,
    Date: Date,
    Math: Math,
    Promise: Promise,
    Map: Map,
    Set: Set,
    Object: Object,
    Array: Array,
    String: String,
    Number: Number,
    Boolean: Boolean,
    RegExp: RegExp,
    Error: Error,
    isNaN: isNaN,
    parseInt: parseInt,
    parseFloat: parseFloat,
    encodeURIComponent: encodeURIComponent,
    decodeURIComponent: decodeURIComponent,
    fetch: function (url) {
      const abs = toDisk(url)
      if (!fs.existsSync(abs)) {
        return Promise.resolve({ ok: false, status: 404, text: function () { return Promise.resolve("") } })
      }
      const text = fs.readFileSync(abs, "utf8")
      return Promise.resolve({ ok: true, status: 200, text: function () { return Promise.resolve(text) } })
    }
  }
  sandbox.window = sandbox
  sandbox.globalThis = sandbox
  sandbox.self = sandbox

  const context = vm.createContext(sandbox)
  const run = function (urlPath) {
    vm.runInContext(fs.readFileSync(toDisk(urlPath), "utf8"), context, { filename: urlPath })
  }

  // 顺序与 web/knowledge/shell.html 的 <script> 一致
  run("/shared/ui.js")
  run("/shared/charts.js")
  run("/shared/platform.js")
  run("/cjs-loader.js")
  run("/knowledge/browser.js")

  return { sandbox: sandbox, appMain: appMain }
}

/** 17 个页面路由（含 .js 后缀，与 browser.js 的 PAGES 对齐） */
const PAGES = [
  "pages/dashboard/index",
  "pages/learn/index",
  "pages/learn/quiz/index",
  "pages/learn/review/index",
  "pages/learn/deep/index",
  "pages/learn/path/index",
  "pages/interview/index",
  "pages/interview/tracker/index",
  "pages/interview/bank/index",
  "pages/interview/mock/index",
  "pages/knowledge/index",
  "pages/knowledge/graph/index",
  "pages/knowledge/excerpt/index",
  "pages/knowledge/radar/index",
  "pages/profile/index",
  "pages/plan/five-year/index",
  "pages/plan/roadmap/index"
]

test("browser.js 的预载链能让首页挂载出内容", function () {
  const { appMain } = bootModule()
  return new Promise(function (resolve) { setTimeout(resolve, 300) }).then(function () {
    // 预载是串行 Promise 链，300ms 足够；失败时页面会渲染「模块加载失败」
    const text = appMain.textContent
    assert.ok(text.indexOf("模块加载失败") < 0, "预载链失败：" + text.slice(0, 200))
    assert.ok(text.length > 0, "首页应渲染出内容")
  })
})

test("17 个页面模块都能经 cjs-loader 加载并渲染", function () {
  const { sandbox } = bootModule()
  return new Promise(function (resolve) { setTimeout(resolve, 300) }).then(function () {
    const CJS = sandbox.CJS
    assert.ok(CJS, "cjs-loader 应挂到 window.CJS")

    return PAGES.reduce(function (chain, route) {
      return chain.then(function () {
        return CJS.load("/knowledge/" + route + ".js").then(function (page) {
          assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
          // browser.js 每次加载都会深拷贝 data，这里照做以免上一页的状态串场
          page.data = JSON.parse(JSON.stringify(page.data || {}))
          page.state = Object.assign({}, page.data)
          const node = page.render(page.state)
          assert.ok(node, route + " 的 render 应返回节点，且不因预载缺项抛错")
          const text = node.textContent
          assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
          assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
          // 两个 Canvas 页面还会走一次 attachCanvas
          if (typeof page.attachCanvas === "function") page.attachCanvas()
        })
      })
    }, Promise.resolve())
  })
})

test("两个 Canvas 组件在 cjs-loader 路径下也能拿到布局模块", function () {
  const { sandbox } = bootModule()
  return new Promise(function (resolve) { setTimeout(resolve, 300) }).then(function () {
    return sandbox.CJS.load("/knowledge/components/knowledge-graph.js").then(function (mod) {
      assert.strictEqual(typeof mod.create, "function")
      const inst = mod.create({ nodes: [{ id: "a", name: "A", domain: "算法", level: 1 }], links: [] })
      assert.strictEqual(typeof inst.draw, "function")
      inst.destroy()
      return sandbox.CJS.load("/knowledge/components/radar-chart.js")
    }).then(function (mod) {
      assert.strictEqual(typeof mod.create, "function")
      const inst = mod.create({ mode: "stack", rows: [{ key: "c", label: "C", color: "#5C6BC0", percent: 50 }] })
      assert.strictEqual(typeof inst.draw, "function")
      inst.destroy()
    })
  })
})
