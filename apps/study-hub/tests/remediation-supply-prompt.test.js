"use strict"
/**
 * remediation-supply-prompt.test.js —— 断言脚本提示词闸门
 *
 * 实测教训：mvcc / index_design 的断言脚本只覆盖 demo 的一部分结论，
 * 变异测试因此抓不住被改坏的逻辑。模板必须明确要求「覆盖每个主要结论、
 * 检查具体内容」，否则新生成的脚本会重犯同一个错。
 *
 * 注意：algorithm 轨**没有** generate-assert 场景（只有 generate-unit 与
 * write-explain）——它的用例要求写在 generate-unit 模板里，所以本测试对
 * 算法轨查的是 generate-unit。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const prompts = require("../core/llm/prompts.js")

const CARD_TRACKS = ["database", "ai"]

const CTX = {
  title: "t",
  statement: "s",
  demo: "print(1)\n",
  readme: "",
  notes: "",
  question: "",
  topics: "x",
  difficulty: "medium",
}

test("database / ai 两轨的 generate-assert 模板都要求覆盖每个主要结论", function () {
  CARD_TRACKS.forEach(function (track) {
    const built = prompts.buildSupplyPrompt("generate-assert", track, CTX)
    const text = String(built.user || "")
    assert.ok(text.indexOf("覆盖") !== -1, track + " 模板要提「覆盖」")
    assert.ok(text.indexOf("每个") !== -1, track + " 模板要提「每个」")
  })
})

test("database / ai 模板要求断言具体内容，不许只断言「跑完了」", function () {
  CARD_TRACKS.forEach(function (track) {
    const built = prompts.buildSupplyPrompt("generate-assert", track, CTX)
    const text = String(built.user || "")
    assert.ok(text.indexOf("具体") !== -1, track + " 模板要提「具体」")
  })
})

test("database / ai 模板要求脚本能区分被测逻辑被改坏的情形", function () {
  CARD_TRACKS.forEach(function (track) {
    const built = prompts.buildSupplyPrompt("generate-assert", track, CTX)
    const text = String(built.user || "")
    assert.ok(text.indexOf("改坏") !== -1, track + " 模板要提「改坏」")
  })
})

test("algorithm 轨的 generate-unit 模板要求用例覆盖边界、期望输出具体", function () {
  const built = prompts.buildSupplyPrompt("generate-unit", "algorithm", CTX)
  const text = String(built.user || "")
  assert.ok(text.indexOf("边界") !== -1, "算法轨模板要提「边界」")
  assert.ok(text.indexOf("具体") !== -1, "算法轨模板要提「具体」")
})

test("algorithm 轨确实没有 generate-assert 场景（这是设计如此，不是缺陷）", function () {
  assert.strictEqual(prompts.hasSupplyTemplate("algorithm", "generate-assert"), false)
  assert.strictEqual(prompts.hasSupplyTemplate("algorithm", "generate-unit"), true)
  assert.strictEqual(prompts.hasSupplyTemplate("database", "generate-assert"), true)
  assert.strictEqual(prompts.hasSupplyTemplate("ai", "generate-assert"), true)
})
