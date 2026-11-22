"use strict"
/**
 * remediation-supply-web.test.js —— 供给页语义闸门
 *
 * 补齐设计 §6.2：缺口是「还没补齐的东西」，抽检是「人看过之后给个结论」。
 * 缺口项上不该再挂「通过 / 打回」按钮——那是把两个动作混成一件事。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document / window 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global
require("../web/app.js")
const supply = require("../web/pages/supply.js")

/** 收集树里所有按钮的文案（按 tagName 找，按钮的 class 是 "primary" 之类） */
function buttonLabels(nodes) {
  const out = []
  function walk(n) {
    if (!n) return
    // dom-stub 的 tagName 是 createElement 传进来的原样（小写 "button"），
    // 所以这里统一按小写比，不要写死大写。
    if (String(n.tagName || "").toLowerCase() === "button") {
      out.push(String(n.textContent || ""))
    }
    ;(n.children || []).forEach(walk)
  }
  ;(nodes || []).forEach(walk)
  return out
}

/** 把树里所有文本拼起来 */
function collectText(nodes) {
  let out = ""
  function walk(n) {
    if (!n) return
    if (n.textContent) out += " " + String(n.textContent)
    ;(n.children || []).forEach(walk)
  }
  ;(nodes || []).forEach(walk)
  return out
}

test("缺口清单里没有「通过 / 打回」按钮", function () {
  const nodes = supply.buildSupplyView({
    tracks: [],
    gaps: [{ id: "db:mvcc", track: "database", title: "MVCC", missing: ["asserts"] }],
    warnings: [],
    reviewable: [],
  })
  const text = collectText(nodes)
  assert.ok(text.indexOf("缺断言脚本") !== -1, "缺口要显示缺什么：" + text)
  const labels = buttonLabels(nodes)
  assert.strictEqual(labels.indexOf("通过"), -1, "缺口清单里不该有通过按钮：" + labels.join("/"))
  assert.strictEqual(labels.indexOf("打回"), -1, "缺口清单里不该有打回按钮：" + labels.join("/"))
})

test("待抽检清单里有「通过 / 打回」按钮", function () {
  const nodes = supply.buildSupplyView({
    tracks: [],
    gaps: [],
    warnings: [],
    reviewable: [{ id: "db:mvcc", track: "database", title: "MVCC", status: "verified" }],
  })
  const labels = buttonLabels(nodes)
  assert.ok(labels.indexOf("通过") !== -1, "待抽检要有通过按钮：" + labels.join("/"))
  assert.ok(labels.indexOf("打回") !== -1, "待抽检要有打回按钮：" + labels.join("/"))
})

test("既有缺口又有待抽检时，按钮只来自待抽检那一块", function () {
  const nodes = supply.buildSupplyView({
    tracks: [],
    gaps: [{ id: "db:mvcc", track: "database", title: "MVCC", missing: ["asserts", "statement"] }],
    warnings: [],
    reviewable: [{ id: "db:transaction", track: "database", title: "事务", status: "verified" }],
  })
  const labels = buttonLabels(nodes)
  // 缺口那一块**一个按钮都没有**——这是本用例的核心断言。
  // 待抽检清单自身 2 个（通过 / 打回），主卡另有 2 个快捷（抽检第一个 / 打回第一个），
  // 那两个的文案与「通过 / 打回」不同，所以不会混淆。
  assert.strictEqual(labels.filter(function (l) { return l === "通过" }).length, 1, labels.join("/"))
  assert.strictEqual(labels.filter(function (l) { return l === "打回" }).length, 1, labels.join("/"))
  // 主卡的快捷入口只指向 reviewable[0]，不带「缺口」字样
  labels.forEach(function (label) {
    assert.strictEqual(label.indexOf("缺口"), -1, "按钮文案里不该再出现「缺口」：" + labels.join("/"))
  })
})

test("buildSupplyView 是纯函数：给空数据也不炸", function () {
  assert.ok(supply.buildSupplyView({}).length > 0)
  assert.ok(supply.buildSupplyView(null).length > 0)
})
