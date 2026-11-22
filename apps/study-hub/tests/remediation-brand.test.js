"use strict"
/**
 * remediation-brand.test.js —— 品牌文案闸门
 *
 * 补齐设计 §6.2：应用已从「算法能力计划」改名 study-hub，
 * 浏览器标签页与页头还挂着旧名。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const WEB = path.resolve(__dirname, "..", "web")

test("index.html 的标题是学习中枢，不是「算法能力计划」", function () {
  const html = fs.readFileSync(path.join(WEB, "index.html"), "utf8")
  assert.ok(html.indexOf("算法能力计划") === -1, "还留着旧标题")
  assert.ok(html.indexOf("学习中枢") !== -1, "标题该用「学习中枢」")
})

test("index.html 的页头副标提到三轨", function () {
  const html = fs.readFileSync(path.join(WEB, "index.html"), "utf8")
  assert.ok(html.indexOf("算法") !== -1, "副标要提算法")
  assert.ok(html.indexOf("数据库") !== -1, "副标要提数据库")
  assert.ok(html.indexOf("AI") !== -1, "副标要提 AI")
})
