"use strict"
/**
 * ui.test.js —— 构件层
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const ui = require("../shared/ui.js")

test("el：设置 class 与 text", function () {
  const node = ui.el("div", { class: "panel", text: "内容" })
  assert.strictEqual(node.tagName, "DIV")
  assert.strictEqual(node.className, "panel")
  assert.strictEqual(node.textContent, "内容")
})

test("el：on 前缀的属性注册为事件监听器", function () {
  let hit = 0
  const node = ui.el("button", { onclick: function () { hit += 1 } })
  node.fire("click")
  assert.strictEqual(hit, 1)
})

test("el：children 按顺序追加", function () {
  const node = ui.el("div", {}, [ui.el("span", { text: "A" }), ui.el("span", { text: "B" })])
  assert.strictEqual(node.children.length, 2)
  assert.strictEqual(node.textContent, "AB")
})

test("pageHead：眉标 → 标题 → 副标的顺序", function () {
  const node = ui.pageHead({ eyebrow: "眉标", title: "标题", sub: "副标" })
  assert.strictEqual(node.className, "page-head")
  assert.strictEqual(node.children.length, 3)
  assert.strictEqual(node.children[0].textContent, "眉标")
  assert.strictEqual(node.children[1].textContent, "标题")
  assert.strictEqual(node.children[2].textContent, "副标")
})

test("hero：主卡带 hero 类名", function () {
  const node = ui.hero({ title: "现在该做什么", sub: "下一步" })
  assert.strictEqual(node.className, "panel hero")
})

test("panel：可带自定义类名", function () {
  assert.strictEqual(ui.panel({ title: "T" }).className, "panel")
  assert.strictEqual(ui.panel({ title: "T", className: "quiet" }).className, "panel quiet")
})

test("statGrid：数字块的数量与内容", function () {
  const node = ui.statGrid([
    { label: "今天做了", value: 2, hint: "道题" },
    { label: "复习", value: 1 }
  ])
  assert.strictEqual(node.className, "stat-grid")
  assert.strictEqual(node.children.length, 2)
  assert.ok(node.textContent.indexOf("今天做了") >= 0)
  assert.ok(node.textContent.indexOf("2") >= 0)
})

test("pill：状态用颜色 + 文字双承载", function () {
  const node = ui.pill("已完成", "ok")
  assert.strictEqual(node.className, "pill ok")
  assert.strictEqual(node.textContent, "已完成")
})

test("kv：每行一个事实", function () {
  const node = ui.kv([["键", "值"], ["键2", 42]])
  assert.strictEqual(node.className, "kv")
  assert.strictEqual(node.children.length, 2)
  assert.ok(node.textContent.indexOf("键2") >= 0)
  assert.ok(node.textContent.indexOf("42") >= 0)
})

test("empty：说清「没有」以及「为什么」", function () {
  const node = ui.empty("还没有记录", "点上面的按钮新建一条")
  assert.strictEqual(node.className, "empty")
  assert.ok(node.textContent.indexOf("还没有记录") >= 0)
  assert.ok(node.textContent.indexOf("点上面的按钮新建一条") >= 0)
})

test("mount：把页面的 render 结果挂到容器上", function () {
  const container = { children: [], appendChild: function (c) { this.children.push(c) } }
  const page = {
    data: { name: "甲" },
    render: function (state) { return ui.panel({ title: state.name }) }
  }
  ui.mount(container, page)
  assert.strictEqual(container.children.length, 1)
  assert.ok(container.children[0].textContent.indexOf("甲") >= 0)
})

test("mount：state 变化后重新渲染", function () {
  const container = { children: [], appendChild: function (c) { this.children.push(c) } }
  const page = {
    data: { name: "甲" },
    render: function (state) { return ui.panel({ title: state.name }) }
  }
  ui.mount(container, page)
  page.state.name = "乙"
  ui.rerender(container, page)
  assert.ok(container.children[0].textContent.indexOf("乙") >= 0)
})
