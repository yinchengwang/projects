"use strict"
/**
 * pages.test.js —— 四个前端页面都注册进 window.AlgoPlan.pages
 *
 * 页面脚本本来在浏览器里跑，但「注册」这一步可以在 Node 里验：
 * 给一个最小的 window 替身，把页面脚本 require 进来。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const WEB = path.resolve(__dirname, "..", "web")
const PAGES = ["today", "answer", "review", "rounds"]

function stubWindow() {
  global.window = {
    AlgoPlan: {
      el: function () {
        return {}
      },
      getJson: function () {
        return Promise.resolve({})
      },
      postJson: function () {
        return Promise.resolve({})
      },
      patchJson: function () {
        return Promise.resolve({})
      },
      state: { problemId: null, currentRoundId: null },
      pages: {},
      go: function () {},
    },
  }
}

/**
 * 加载一个页面脚本。
 *
 * 必须先清掉模块缓存：Node 会缓存 require 过的模块，
 * 第二次 require 拿到的是缓存（页面脚本不会重新执行，也就不会重新注册），
 * 换一个 window 替身也就白换了。
 */
function loadPage(name) {
  const file = path.join(WEB, "pages", name + ".js")
  delete require.cache[require.resolve(file)]
  require(file)
}

test("四个页面脚本都注册进 window.AlgoPlan.pages", function () {
  stubWindow()
  PAGES.forEach(function (name) {
    loadPage(name)
  })
  PAGES.forEach(function (name) {
    const page = global.window.AlgoPlan.pages[name]
    assert.ok(page, name + " 页没有注册")
    assert.strictEqual(typeof page.render, "function", name + " 页缺少 render")
  })
})

test("index.html 引用了四个页面脚本", function () {
  const html = fs.readFileSync(path.join(WEB, "index.html"), "utf8")
  PAGES.forEach(function (name) {
    assert.ok(html.indexOf("/pages/" + name + ".js") !== -1, "index.html 里没有 " + name + ".js")
  })
})

test("复习页的四个直觉文案映射到 SM-2 的 quality", function () {
  stubWindow()
  loadPage("review")
  const buttons = global.window.AlgoPlan.pages.review.buttons
  assert.strictEqual(buttons.length, 4)
  assert.deepStrictEqual(
    buttons.map(function (b) {
      return b.quality
    }),
    [1, 2, 3, 5]
  )
  assert.deepStrictEqual(
    buttons.map(function (b) {
      return b.label
    }),
    ["完全忘了", "想很久", "有点印象", "秒答"]
  )
})
