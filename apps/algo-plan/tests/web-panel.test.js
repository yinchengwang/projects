"use strict"
/**
 * web-panel.test.js —— 结果面板的数据整理（纯函数，不碰 DOM）
 *
 * web/app.js 在 Node 下也能 require：文件末尾的 document / module 都做了存在性判断。
 */

const test = require("node:test")
const assert = require("node:assert")

const panel = require("../web/app.js")

test("五种 verdict 都有对应的面板色调", function () {
  const verdicts = ["pass", "wrong_answer", "timeout", "compile_error", "runtime_error"]
  verdicts.forEach(function (v) {
    assert.ok(panel.TONE_BY_VERDICT[v], v + " 缺色调")
    assert.ok(["ok", "bad", "warn"].indexOf(panel.TONE_BY_VERDICT[v]) !== -1, v + " 色调不合法")
  })
})

test("describePanel 给出用例数与耗时", function () {
  const p = panel.describePanel({
    verdict: "pass",
    passedCases: 3,
    totalCases: 3,
    durationMs: 123,
    message: "通过：全部 3 个用例通过",
    failedCase: null,
    compilerOutput: "",
  })
  assert.strictEqual(p.verdict, "pass")
  assert.strictEqual(p.tone, "ok")
  assert.strictEqual(p.title, "通过：全部 3 个用例通过")
  assert.deepStrictEqual(p.rows, [
    { label: "用例", value: "3 / 3 通过" },
    { label: "耗时", value: "123 ms" },
  ])
  assert.strictEqual(p.failed, null)
  assert.strictEqual(p.compilerOutput, "")
})

test("describePanel 把失败用例的期望与实际都带出来", function () {
  const p = panel.describePanel({
    verdict: "wrong_answer",
    passedCases: 0,
    totalCases: 3,
    durationMs: 20,
    message: "答案错误：第 1 个用例不通过：输出与期望不一致",
    failedCase: { index: 1, input: "4 2 7 11 15 9", expected: "0 1", actual: "0 0" },
    compilerOutput: "",
  })
  assert.strictEqual(p.tone, "bad")
  assert.strictEqual(p.failed.index, 1)
  assert.strictEqual(p.failed.expected, "0 1")
  assert.strictEqual(p.failed.actual, "0 0")
})

test("describePanel 带出编译错误原文", function () {
  const p = panel.describePanel({
    verdict: "compile_error",
    passedCases: 0,
    totalCases: 3,
    durationMs: 50,
    message: "编译失败：第 4 行：'undefined_function_call' was not declared in this scope",
    failedCase: null,
    compilerOutput: "main.cpp:4:5: error: 'undefined_function_call' was not declared in this scope",
  })
  assert.strictEqual(p.tone, "bad")
  assert.ok(p.compilerOutput.indexOf("main.cpp:4:5") !== -1, p.compilerOutput)
})

test("describePanel 对空结果不炸", function () {
  const p = panel.describePanel(null)
  assert.strictEqual(p.verdict, "")
  assert.strictEqual(p.tone, "")
  assert.strictEqual(p.failed, null)
  assert.deepStrictEqual(p.rows.length, 2)
})

test("超长输出会被截断并注明", function () {
  const long = "x".repeat(1000)
  const longer = "y".repeat(2000)
  const p = panel.describePanel({
    verdict: "wrong_answer",
    passedCases: 0,
    totalCases: 1,
    durationMs: 1,
    message: "答案错误",
    failedCase: { index: 1, input: "a", expected: "b", actual: long },
    compilerOutput: longer,
  })
  // 用例输出上限 400 字符，编译原文上限 1200 字符（PANEL_TEXT_LIMIT 的 3 倍）
  assert.ok(p.failed.actual.length < long.length, "实际输出应被截断")
  assert.ok(p.failed.actual.indexOf("已截断") !== -1, p.failed.actual)
  assert.ok(p.compilerOutput.length < longer.length, "编译原文应被截断")
  assert.ok(p.compilerOutput.indexOf("已截断") !== -1)
})

test("truncate 不动短文本", function () {
  assert.strictEqual(panel.truncate("abc", 10), "abc")
})
