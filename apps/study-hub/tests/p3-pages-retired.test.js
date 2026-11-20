"use strict"
/**
 * p3-pages-retired.test.js —— 旧三页的退役闸门（契约 §9.1 的 P3 行）
 *
 * P3 起 today / review / rounds 三个旧页**不再注册**，但**文件必须还在**：
 *   - 保留在仓库里作为历史（产品演进的痕迹，不是垃圾）
 *   - tests/web-dom.test.js 还要用 today.js 的 renderHealth 与 render
 *
 * 「还在」的三条证据（一处比一处更强）：
 *   1. 三个文件都在盘上
 *   2. 三个脚本**都还执行得动**（require 不抛错），且 today.js 的模块导出还在
 *   3. 加载 app.js 之后，三个页面不在 pages 里
 *   4. app.js 里写着那三句 delete（源码级对照，照 P2 的写法）
 *
 * 注意：review.js / rounds.js 是纯 IIFE，本来就没有 module.exports——
 * 它们的「活着」体现在「require 不抛错且能把自己注册进页面表」。
 * 只有 today.js 额外导出了 render / renderHealth（web-dom.test.js 要用）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const WEB = path.resolve(__dirname, "..", "web")

const stub = require("./helpers/dom-stub.js")
const dom = stub.createDocument()
global.document = dom.document
global.window = global

// 先加载 app.js：它把 el / getJson / pages 挂到 window.AlgoPlan 上。
// 旧页脚本自己要读 window.AlgoPlan.el，所以顺序不能反。
require("../web/app.js")

/** 三个退役页 */
const RETIRED = ["today", "review", "rounds"]

/** 清掉模块缓存再 require，让脚本重新执行一次（否则拿到的是 app.js 那次的结果） */
function reload(name) {
  const file = path.join(WEB, "pages", name + ".js")
  delete require.cache[require.resolve(file)]
  return require(file)
}

test("旧页的文件都还在仓库里（历史留存，不删文件）", function () {
  RETIRED.forEach(function (name) {
    const file = path.join(WEB, "pages", name + ".js")
    assert.ok(fs.existsSync(file), name + ".js 不见了——契约 §9.1 只删注册，不删文件")
  })
})

test("旧页的脚本都还执行得动：require 不抛错，且会把自己注册进页面表", function () {
  RETIRED.forEach(function (name) {
    // 换一张干净的页面表，验「脚本自己会注册」这件事没被破坏
    global.window.AlgoPlan.pages = {}
    let page
    assert.doesNotThrow(function () {
      page = reload(name)
    }, name + ".js require 就抛错了")
    assert.ok(global.window.AlgoPlan.pages[name], name + ".js 不再把自己注册进页面表")
    assert.strictEqual(
      typeof global.window.AlgoPlan.pages[name].render,
      "function",
      name + " 页缺少 render"
    )
    assert.ok(page !== null && page !== undefined, name + ".js 的 module.exports 是空的")
  })

  // today.js 额外导出了 renderHealth（tests/web-dom.test.js 要用）
  const today = reload("today")
  assert.strictEqual(typeof today.render, "function", "today.js 少了导出：render")
  assert.strictEqual(typeof today.renderHealth, "function", "today.js 少了导出：renderHealth")
})

test("加载 app.js 之后，旧页不再注册进 pages", function () {
  // 重跑一次 app.js 的注册逻辑：清缓存 → require → 它会先把旧页注册进来再删掉
  delete require.cache[require.resolve(path.join(WEB, "app.js"))]
  require(path.join(WEB, "app.js"))
  // 诊断页是 P3 的主页面，它自己 require 时注册（不经过 app.js）
  delete require.cache[require.resolve(path.join(WEB, "diagnose.js"))]
  require(path.join(WEB, "diagnose.js"))

  const pages = global.window.AlgoPlan.pages || {}
  RETIRED.forEach(function (name) {
    assert.strictEqual(pages[name], undefined, name + " 还注册在 pages 里")
  })
  // 又一个正面证据：诊断页确实注册上了——否则「删干净了」可能是假绿
  assert.ok(pages.diagnose, "诊断页没注册上，说明 delete 写过头了")
})

test("app.js 里写着那三句 delete（源码级对照，照 P2 的写法）", function () {
  const source = fs.readFileSync(path.join(WEB, "app.js"), "utf8")
  RETIRED.forEach(function (name) {
    assert.ok(
      source.indexOf("delete APP.pages." + name) !== -1,
      "app.js 里没写 delete APP.pages." + name
    )
  })
})
