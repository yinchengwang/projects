"use strict"
/**
 * trip-games-boot.test.js —— 在 Node 里模拟浏览器启动 trip 与 games 模块
 *
 * 与 wb-boot / kh-boot 同一目的：Node 单测走原生 require，绕过了 cjs-loader；
 * 浏览器里页面模块的 require() 只能拿到「已预载」的模块。这一组补那条接缝。
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
 * @param {string} urlPath
 * @returns {string}
 */
function toDisk (urlPath) {
  const clean = String(urlPath).split("?")[0]
  if (clean.indexOf("/shared/") === 0) return path.join(SHARED, clean.slice("/shared/".length))
  return path.join(WEB, clean.replace(/^\//, ""))
}

/** 最小 2d 上下文 */
function makeCtx () {
  const noop = function () {}
  return {
    scale: noop, clearRect: noop, beginPath: noop, moveTo: noop, lineTo: noop, arc: noop,
    closePath: noop, fill: noop, stroke: noop, fillRect: noop, strokeRect: noop,
    save: noop, restore: noop, translate: noop, rotate: noop,
    measureText: function () { return { width: 10 } },
    fillText: noop, strokeText: noop,
    strokeStyle: "", fillStyle: "", lineWidth: 1, globalAlpha: 1, font: ""
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
    value: "",
    width: 0,
    height: 0,
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
    removeEventListener: function () {},
    getBoundingClientRect: function () { return { width: 320, height: 320, top: 0, left: 0 } },
    getContext: function () { return makeCtx() },
    focus: function () {}
  }
}

/**
 * 造浏览器沙箱并执行模块入口。
 * @param {string} module "trip" 或 "games"
 * @param {string} pathname 初始地址
 * @returns {object} { sandbox, appMain }
 */
function bootModule (module, pathname) {
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
    location: { pathname: pathname, search: "" },
    history: { pushState: function () {}, replaceState: function () {}, back: function () {} },
    addEventListener: function () {},
    matchMedia: function () { return { matches: false, addEventListener: function () {} } },
    URLSearchParams: URLSearchParams,
    URL: { createObjectURL: function () { return "blob:stub" }, revokeObjectURL: function () {} },
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

  run("/shared/ui.js")
  run("/shared/charts.js")
  run("/shared/platform.js")
  run("/cjs-loader.js")
  run("/" + module + "/browser.js")

  return { sandbox: sandbox, appMain: appMain }
}

/** trip 的 3 个页面 */
const TRIP_PAGES = ["index", "new-trip", "detail"]

/** games 的 5 个页面 */
const GAMES_PAGES = ["index", "snake", "game2048", "sudoku", "match3"]

test("trip：预载链能让首页挂载出内容", function () {
  const { appMain } = bootModule("trip", "/trip/pages/index")
  return new Promise(function (resolve) { setTimeout(resolve, 500) }).then(function () {
    const text = appMain.textContent
    assert.ok(text.indexOf("模块加载失败") < 0, "预载链失败：" + text.slice(0, 300))
    assert.ok(text.length > 0, "首页应渲染出内容")
    assert.ok(text.indexOf("undefined") < 0, "含 undefined：" + text.slice(0, 200))
  })
})

test("trip：3 个页面模块都能经 cjs-loader 加载并渲染", function () {
  const { sandbox } = bootModule("trip", "/trip/pages/index")
  return new Promise(function (resolve) { setTimeout(resolve, 500) }).then(function () {
    assert.ok(sandbox.CJS, "cjs-loader 应挂到 window.CJS")
    return TRIP_PAGES.reduce(function (chain, name) {
      return chain.then(function () {
        return sandbox.CJS.load("/trip/pages/" + name + ".js").then(function (page) {
          assert.strictEqual(typeof page.render, "function", name + " 应有 render()")
          page.data = JSON.parse(JSON.stringify(page.data || {}))
          page.state = Object.assign({}, page.data)
          const node = page.render(page.state)
          assert.ok(node, name + " 的 render 应返回节点")
          const text = node.textContent
          assert.ok(text.indexOf("undefined") < 0, name + " 含 undefined：" + text.slice(0, 120))
          assert.ok(text.indexOf("NaN") < 0, name + " 含 NaN：" + text.slice(0, 120))
        })
      })
    }, Promise.resolve())
  })
})

test("games：预载链能让首页挂载出内容", function () {
  const { appMain } = bootModule("games", "/games/pages/index")
  return new Promise(function (resolve) { setTimeout(resolve, 500) }).then(function () {
    const text = appMain.textContent
    assert.ok(text.indexOf("模块加载失败") < 0, "预载链失败：" + text.slice(0, 300))
    assert.ok(text.length > 0, "首页应渲染出内容")
    assert.ok(text.indexOf("undefined") < 0, "含 undefined：" + text.slice(0, 200))
  })
})

test("games：5 个页面模块都能经 cjs-loader 加载并渲染", function () {
  const { sandbox } = bootModule("games", "/games/pages/index")
  return new Promise(function (resolve) { setTimeout(resolve, 500) }).then(function () {
    assert.ok(sandbox.CJS, "cjs-loader 应挂到 window.CJS")
    return GAMES_PAGES.reduce(function (chain, name) {
      return chain.then(function () {
        return sandbox.CJS.load("/games/pages/" + name + ".js").then(function (page) {
          assert.strictEqual(typeof page.render, "function", name + " 应有 render()")
          page.data = JSON.parse(JSON.stringify(page.data || {}))
          page.state = Object.assign({}, page.data)
          const node = page.render(page.state)
          assert.ok(node, name + " 的 render 应返回节点")
          const text = node.textContent
          assert.ok(text.indexOf("undefined") < 0, name + " 含 undefined：" + text.slice(0, 120))
          assert.ok(text.indexOf("NaN") < 0, name + " 含 NaN：" + text.slice(0, 120))
        })
      })
    }, Promise.resolve())
  })
})
