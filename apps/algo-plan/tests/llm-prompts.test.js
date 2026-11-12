"use strict"
/**
 * llm-prompts.test.js —— 提示词纯函数测试（**不联网**）
 *
 * 钉住三件事：
 *   1. 五种 kind 的 buildPrompt 都返回 {system, user}，且是纯函数；
 *   2. 不主动泄题（G24）：非「代码」档禁止给代码；explain / hint 的提示词里
 *      **根本不会**出现参考解法（哪怕调用方硬塞了 context.solution）；
 *   3. parseResponse 对脏输入给出 ok:false 与中文原因，不抛异常。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const {
  buildPrompt,
  parseResponse,
  EXPLAIN_LEVELS,
  HINT_LEVELS,
  KINDS,
} = require("../core/llm/prompts.js")

/** 一道普通的题，带参考解法 */
const 题 = {
  id: "lc-00001",
  title: "两数之和",
  difficulty: "easy",
  topics: ["哈希表", "数组"],
  tracks: ["interview"],
  statement: "给定整数数组 nums 和整数 target，找出数组中和为 target 的两个整数的下标。",
  cases: [{ input: "[2,7,11,15], 9", expect: "[0,1]", tol: null }],
  solution: "int main(){ /* 参考答案 */ return 0; }",
  solutionLang: "cpp",
}

/** 禁止给代码时 system 里必须出现的句子 */
const NO_CODE = "绝不给出任何可编译的完整代码"
/** 允许给代码时 system 里必须出现的句子 */
const ALLOW_CODE = "本次允许给出完整代码"

test("档位枚举固定：讲解两档、求助四档", function () {
  assert.deepEqual(EXPLAIN_LEVELS, ["思路", "代码"])
  assert.deepEqual(HINT_LEVELS, ["方向", "思路", "伪码", "代码"])
  assert.deepEqual(KINDS, ["explain", "hint", "grade-idea", "generate", "fill"])
})

test("buildPrompt 对五种 kind 都返回非空的 system 与 user", function () {
  const ctx = {
    title: 题.title,
    difficulty: 题.difficulty,
    topics: 题.topics,
    statement: 题.statement,
    level: "思路",
    idea: "用哈希表把出现过的数存起来",
    track: "interview",
    cases: 题.cases,
    solution: 题.solution,
    solutionLang: 题.solutionLang,
  }
  for (const kind of KINDS) {
    const p = buildPrompt(kind, ctx)
    assert.equal(typeof p.system, "string", kind + " 的 system 必须是字符串")
    assert.equal(typeof p.user, "string", kind + " 的 user 必须是字符串")
    assert.ok(p.system.length > 0, kind + " 的 system 不该为空")
    assert.ok(p.user.length > 0, kind + " 的 user 不该为空")
  }
})

test("buildPrompt 是纯函数：同样输入永远同样输出", function () {
  const ctx = { title: "t", statement: "s", level: "思路", topics: ["a"], difficulty: "easy" }
  assert.deepEqual(buildPrompt("explain", ctx), buildPrompt("explain", ctx))
  assert.deepEqual(buildPrompt("generate", ctx), buildPrompt("generate", ctx))
})

test("不主动泄题：非『代码』档的 system 明确禁止给代码", function () {
  const 思路 = buildPrompt("explain", { title: "t", statement: "s", level: "思路" })
  assert.ok(思路.system.includes(NO_CODE), "讲解-思路 档应禁止给代码")

  const 方向 = buildPrompt("hint", { title: "t", statement: "s", level: "方向" })
  assert.ok(方向.system.includes(NO_CODE), "求助-方向 档应禁止给代码")

  const 伪码 = buildPrompt("hint", { title: "t", statement: "s", level: "伪码" })
  assert.ok(伪码.system.includes(NO_CODE), "求助-伪码 档应禁止给可编译代码")
})

test("不主动泄题：只有显式选『代码』档才放开给代码", function () {
  const 代码 = buildPrompt("explain", { title: "t", statement: "s", level: "代码" })
  assert.ok(代码.system.includes(ALLOW_CODE), "讲解-代码 档应放开给代码")
  assert.ok(!代码.system.includes(NO_CODE), "讲解-代码 档不该再出现禁止条款")

  const 求助代码 = buildPrompt("hint", { title: "t", statement: "s", level: "代码" })
  assert.ok(求助代码.system.includes(ALLOW_CODE), "求助-代码 档应放开给代码")
  assert.ok(!求助代码.system.includes(NO_CODE), "求助-代码 档不该再出现禁止条款")
})

test("不主动泄题：explain / hint 的提示词里绝不出现参考解法", function () {
  const 组合 = []
  for (const level of EXPLAIN_LEVELS) 组合.push(["explain", level])
  for (const level of HINT_LEVELS)    组合.push(["hint", level])
  for (const [kind, level] of 组合) {
    const p = buildPrompt(kind, {
      title: 题.title,
      difficulty: 题.difficulty,
      topics: 题.topics,
      statement: 题.statement,
      level: level,
      // 故意把参考解法塞进来，验证 buildPrompt 根本不读它
      solution: 题.solution,
      solutionLang: 题.solutionLang,
    })
    assert.ok(!p.user.includes("参考答案"), kind + "/" + level + " 的 user 不该出现参考解法")
    assert.ok(!p.user.includes("int main"), kind + "/" + level + " 的 user 不该出现参考解法代码")
    assert.ok(!p.system.includes("参考答案"), kind + "/" + level + " 的 system 不该出现参考解法")
  }
})

test("buildPrompt 的 grade-idea 会带上用户思路", function () {
  const p = buildPrompt("grade-idea", { title: "t", statement: "s", idea: "用两个指针" })
  assert.ok(p.user.includes("用两个指针"), "grade-idea 的 user 应带上用户思路")
  assert.ok(p.user.includes("verdict"), "grade-idea 的 user 应要求返回 JSON")
})

test("buildPrompt 的 generate 会要求 JSON 与题面自包含", function () {
  const p = buildPrompt("generate", { track: "interview", topics: ["哈希表"], difficulty: "easy" })
  assert.ok(p.user.includes("interview"), "generate 的 user 应带上轨道")
  assert.ok(p.user.includes("哈希表"), "generate 的 user 应带上知识点")
  assert.ok(p.user.includes("JSON"), "generate 的 user 应要求返回 JSON")
  assert.ok(p.user.includes("stdin"), "generate 的 user 应交代 stdin/stdout 判题契约")
})

test("buildPrompt 的 fill 会带上参考解法", function () {
  const p = buildPrompt("fill", {
    title: "t",
    statement: "",
    cases: [],
    solution: "int main(){return 0;}",
    solutionLang: "cpp",
  })
  assert.ok(p.user.includes("int main"), "fill 的 user 应带上参考解法")
  assert.ok(p.user.includes("JSON"), "fill 的 user 应要求返回 JSON")
})

test("parseResponse 解析讲解与求助：原样返回文本", function () {
  const r = parseResponse("  先用哈希表  ", "explain")
  assert.equal(r.ok, true)
  assert.equal(r.text, "先用哈希表")
})

test("parseResponse 对空回复给出 ok:false 与中文原因", function () {
  const r = parseResponse("", "explain")
  assert.equal(r.ok, false)
  assert.ok(r.reason.length > 0, "失败时必须给出原因")
})

test("parseResponse 解析 grade-idea：认 verdict 枚举", function () {
  const r = parseResponse('{"verdict":"correct","comment":"思路没问题"}', "grade-idea")
  assert.equal(r.ok, true)
  assert.equal(r.verdict, "correct")
  assert.equal(r.comment, "思路没问题")
})

test("parseResponse 能剥掉 ```json 围栏", function () {
  const r = parseResponse('```json\n{"verdict":"partial","comment":"漏了边界"}\n```', "grade-idea")
  assert.equal(r.ok, true)
  assert.equal(r.verdict, "partial")
  assert.equal(r.comment, "漏了边界")
})

test("parseResponse 解析 grade-idea：不是 JSON 就给 ok:false，不抛异常", function () {
  const r = parseResponse("我觉得挺对的", "grade-idea")
  assert.equal(r.ok, false)
  assert.ok(r.reason.length > 0)
})

test("parseResponse 解析 generate：产出题目骨架", function () {
  const r = parseResponse(JSON.stringify({
    title: "两数之和",
    statement: "给定数组…",
    difficulty: "easy",
    topics: ["哈希表"],
    cases: [{ input: "[2,7], 9", expect: "[0,1]" }],
    solution: "int main(){return 0;}",
    solutionLang: "cpp",
  }), "generate")
  assert.equal(r.ok, true)
  assert.equal(r.problem.title, "两数之和")
  assert.equal(r.problem.statement, "给定数组…")
  assert.equal(r.problem.cases.length, 1)
  assert.equal(r.problem.cases[0].input, "[2,7], 9")
  assert.equal(r.problem.solutionLang, "cpp")
})

test("parseResponse 解析 generate：缺测试用例就报 ok:false", function () {
  const r = parseResponse(JSON.stringify({ title: "t", statement: "s", solution: "c" }), "generate")
  assert.equal(r.ok, false)
  assert.ok(r.reason.includes("测试用例"), "原因里应点明缺测试用例")
})

test("parseResponse 解析 fill：产出补全补丁", function () {
  const r = parseResponse(JSON.stringify({
    statement: "补出来的题面",
    cases: [{ input: "1 2", expect: "3" }],
  }), "fill")
  assert.equal(r.ok, true)
  assert.equal(r.patch.statement, "补出来的题面")
  assert.equal(r.patch.cases.length, 1)
  assert.equal(r.patch.cases[0].expect, "3")
})

test("parseResponse 对未知 kind 返回 ok:false", function () {
  const r = parseResponse("随便", "不存在的种类")
  assert.equal(r.ok, false)
})
