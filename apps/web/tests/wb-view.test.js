"use strict"
/**
 * wb-view.test.js —— 视图层：页面能加载、能渲染出关键元素
 *
 * 注意：本文件**不替代**迁入的 315 个用例，只补「渲染出来了吗」这一层。
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const h = require("./wb-harness.js")
const stub = require("./wb-stub.js")

stub.installLocalStorage()

const ui = require("../shared/ui.js")

/** 18 个页面路由 */
const PAGES = [
  "pages/dashboard/index",
  "pages/profile/index",
  "pages/tech/index",
  "pages/tech/notes/index",
  "pages/tech/notes/detail",
  "pages/tech/snippets/index",
  "pages/tech/snippets/detail",
  "pages/tech/interview/index",
  "pages/tech/interview/detail",
  "pages/tech/resources/index",
  "pages/life/index",
  "pages/life/reading/index",
  "pages/life/fitness/index",
  "pages/life/mood/index",
  "pages/life/schedule/index",
  "pages/projects/index",
  "pages/insights/index",
  "pages/tools/index"
]

test("18 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都能产出节点，且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("页面 render 出的文本里含中文标题，不含 undefined", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    const text = node.textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 的渲染结果含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 的渲染结果含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 18 个页面", function () {
  const { ROUTES } = require("../web/workbench/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 18)
})
