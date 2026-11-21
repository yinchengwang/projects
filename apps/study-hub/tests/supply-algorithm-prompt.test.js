"use strict"
/**
 * supply-algorithm-prompt.test.js —— 四件套提示词：构造是纯函数、解析认负例、老调用不受影响
 *
 * 全程不联网：只测提示词组装与响应解析。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const prompts = require("../core/llm/prompts.js")

/** 形态 A 的上下文：题面与用例都缺 */
function ctxA() {
  return {
    mode: "skeleton",
    shape: "A",
    stem: "lc-00007",
    title: "整数反转",
    difficulty: "medium",
    topics: ["数学"],
    url: "https://leetcode.cn/problems/reverse-integer/",
    statement: "",
    cases: [],
  }
}

test("skeleton 提示词是纯函数：两次组装完全一致", function () {
  const one = prompts.buildPrompt("generate", ctxA())
  const two = prompts.buildPrompt("generate", ctxA())
  assert.deepEqual(one, two)
})

test("skeleton 提示词要说清四件套，尤其是负例", function () {
  const p = prompts.buildPrompt("generate", ctxA())
  assert.ok(p.system.indexOf("负例") !== -1, p.system)
  assert.ok(p.user.indexOf("负例") !== -1, p.user)
  assert.ok(p.user.indexOf("negative") !== -1, "JSON 模板里要有 negative 字段")
  assert.ok(p.user.indexOf("negativeNote") !== -1, "JSON 模板里要有 negativeNote 字段")
  assert.ok(p.user.indexOf("lc-00007") !== -1, "题号要进提示词")
  assert.ok(p.user.indexOf("main") !== -1, "要说明判题契约（自带 main 函数）")
})

test("形态 C 的提示词明说「题面与用例原样保留」", function () {
  const p = prompts.buildPrompt(
    "generate",
    Object.assign(ctxA(), {
      shape: "C",
      statement: "给你一个 32 位有符号整数。",
      cases: [{ input: "123", expect: "321" }],
    })
  )
  assert.ok(p.user.indexOf("原样保留") !== -1, p.user)
  assert.ok(p.user.indexOf("给你一个 32 位有符号整数。") !== -1, "题面原文要进提示词")
  assert.ok(p.user.indexOf("123 → 321") !== -1, "用例原文要进提示词")
})

test("没有 mode 时，老的 generate 提示词一个字都不变", function () {
  const p = prompts.buildPrompt("generate", { track: "advanced", topics: ["数学"], difficulty: "hard" })
  assert.ok(p.user.indexOf("请出一道新的算法练习题。") !== -1, p.user)
  assert.ok(p.user.indexOf("负例") === -1, "老提示词不该出现负例")
  assert.equal(p.system, prompts.buildPrompt("generate", { track: "advanced" }).system)
})

test("skeleton 解析：四件套齐了就能解析出来", function () {
  const raw = JSON.stringify({
    title: "整数反转",
    statement: "给你一个整数。",
    difficulty: "medium",
    topics: ["数学"],
    cases: [{ input: "123", expect: "321" }],
    solution: "int main(){std::cout<<321;}",
    negative: "int main(){std::cout<<123;}",
    negativeNote: "负例原样输出了输入",
  })
  const out = prompts.parseResponse(raw, "generate", { mode: "skeleton", need: prompts.SKELETON_NEEDS.A })
  assert.equal(out.ok, true, out.reason)
  assert.equal(out.problem.statement, "给你一个整数。")
  assert.equal(out.problem.cases.length, 1)
  assert.equal(out.problem.cases[0].input, "123")
  assert.equal(out.problem.cases[0].expect, "321")
  assert.equal(out.problem.solution, "int main(){std::cout<<321;}")
  assert.equal(out.problem.negative, "int main(){std::cout<<123;}")
  assert.equal(out.problem.negativeNote, "负例原样输出了输入")
})

test("skeleton 解析：缺负例就判不合格，并给出中文理由", function () {
  const raw = JSON.stringify({
    statement: "给你一个整数。",
    cases: [{ input: "123", expect: "321" }],
    solution: "int main(){std::cout<<321;}",
  })
  const out = prompts.parseResponse(raw, "generate", { mode: "skeleton", need: prompts.SKELETON_NEEDS.A })
  assert.equal(out.ok, false)
  assert.ok(out.reason.indexOf("负例") !== -1, out.reason)
})

test("skeleton 解析：形态 C 只要解法与负例", function () {
  const raw = JSON.stringify({
    solution: "int main(){std::cout<<321;}",
    negative: "int main(){std::cout<<123;}",
  })
  const out = prompts.parseResponse(raw, "generate", { mode: "skeleton", need: prompts.SKELETON_NEEDS.C })
  assert.equal(out.ok, true, out.reason)
  assert.equal(out.problem.solution, "int main(){std::cout<<321;}")
  assert.equal(out.problem.negative, "int main(){std::cout<<123;}")
})

test("老的两参 parseResponse 仍然工作，负例字段缺省是空串", function () {
  const raw = JSON.stringify({
    title: "两数之和",
    statement: "给你两个数。",
    cases: [{ input: "1 2", expect: "3" }],
    solution: "int main(){std::cout<<3;}",
  })
  const out = prompts.parseResponse(raw, "generate")
  assert.equal(out.ok, true, out.reason)
  assert.equal(out.problem.title, "两数之和")
  assert.equal(out.problem.statement, "给你两个数。")
  assert.equal(out.problem.cases.length, 1)
  assert.equal(out.problem.cases[0].input, "1 2")
  assert.equal(out.problem.solution, "int main(){std::cout<<3;}")
  assert.equal(out.problem.negative, "", "老路径的负例字段是空串")
})
