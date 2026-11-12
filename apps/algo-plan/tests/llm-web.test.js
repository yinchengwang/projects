"use strict"
/**
 * llm-web.test.js —— 前端纯逻辑测试（**不联网、不碰 DOM**）
 *
 * web/llm.js 里的「纯逻辑区」不依赖 document，可以直接在 Node 里 require 进来测。
 * 钉住：
 *   1. 四档求助的顺序与「逐级解锁」规则；
 *   2. 讲解默认只到思路层，代码那一档必须显式传；
 *   3. 请求体一定带题号与会话号（成本护栏要用会话号）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const Llm = require("../web/llm.js")

test("web/llm.js 在 Node 里 require 不会崩，也没有碰 DOM", function () {
  assert.ok(Llm, "模块应当导出对象")
  assert.equal(typeof Llm.hintLevelIndex, "function")
})

test("求助四档固定：方向 → 思路 → 伪码 → 代码", function () {
  assert.deepEqual(Llm.HINT_LEVELS, ["方向", "思路", "伪码", "代码"])
})

test("讲解两档固定：先思路，后代码", function () {
  assert.deepEqual(Llm.EXPLAIN_LEVELS, ["思路", "代码"])
})

test("一开始只有「方向」那一档能用", function () {
  assert.equal(Llm.isHintLevelUnlocked(0, "方向"), true)
  assert.equal(Llm.isHintLevelUnlocked(0, "思路"), false)
  assert.equal(Llm.isHintLevelUnlocked(0, "伪码"), false)
  assert.equal(Llm.isHintLevelUnlocked(0, "代码"), false)
})

test("解锁只前进不回退：方向 → 思路 → 伪码 → 代码", function () {
  var idx = 0
  idx = Llm.nextUnlockedIndex(idx, "方向")
  assert.equal(idx, 1, "用过方向之后，思路这一档应解锁")
  assert.equal(Llm.isHintLevelUnlocked(idx, "思路"), true)
  assert.equal(Llm.isHintLevelUnlocked(idx, "伪码"), false)

  idx = Llm.nextUnlockedIndex(idx, "思路")
  assert.equal(idx, 2)
  assert.equal(Llm.isHintLevelUnlocked(idx, "伪码"), true)
  assert.equal(Llm.isHintLevelUnlocked(idx, "代码"), false)

  idx = Llm.nextUnlockedIndex(idx, "伪码")
  assert.equal(idx, 3)
  assert.equal(Llm.isHintLevelUnlocked(idx, "代码"), true)

  idx = Llm.nextUnlockedIndex(idx, "代码")
  assert.equal(idx, 3, "最高档用完也不该越界")

  idx = Llm.nextUnlockedIndex(idx, "方向")
  assert.equal(idx, 3, "解锁进度不该回退")
})

test("档位下标：认不出来的返回 -1", function () {
  assert.equal(Llm.hintLevelIndex("方向"), 0)
  assert.equal(Llm.hintLevelIndex("代码"), 3)
  assert.equal(Llm.hintLevelIndex("神级"), -1)
  assert.equal(Llm.isHintLevelUnlocked(3, "神级"), false)
})

test("讲解默认只到思路层，要代码必须显式传", function () {
  assert.equal(Llm.explainRequestBody("lc-00001", undefined, "s1").level, "思路")
  assert.equal(Llm.explainRequestBody("lc-00001", "思路", "s1").level, "思路")
  assert.equal(Llm.explainRequestBody("lc-00001", "代码", "s1").level, "代码")
})

test("请求体带题号与档位", function () {
  assert.deepEqual(Llm.explainRequestBody("lc-00001", "思路", "s1"), {
    problemId: "lc-00001",
    level: "思路",
    sessionId: "s1",
  })
  assert.deepEqual(Llm.hintRequestBody("lc-00001", "伪码", "s1"), {
    problemId: "lc-00001",
    level: "伪码",
    sessionId: "s1",
  })
})

test("解锁进度按题号分别记，互不影响", function () {
  Llm.resetUnlocks()
  Llm.setUnlocked("lc-00001", 2)
  assert.equal(Llm.getUnlocked("lc-00001"), 2)
  assert.equal(Llm.getUnlocked("lc-00002"), 0)
  Llm.resetUnlocks()
  assert.equal(Llm.getUnlocked("lc-00001"), 0)
})

test("会话号是稳定的（同一个会话里不变）", function () {
  const a = Llm.sessionId()
  const b = Llm.sessionId()
  assert.equal(a, b)
  assert.ok(a.length > 0)
})
