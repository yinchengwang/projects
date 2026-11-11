"use strict"
/**
 * runner.test.js —— 判题编排器：五类 verdict 端到端（真编译、真执行）
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")

const { parseProblemMarkdown } = require("../core/bank/parse.js")
const runner = require("../judge/runner.js")
const manifest = require("../fixtures/judge/manifest.js")

function loadProblem(file) {
  return parseProblemMarkdown(fs.readFileSync(file, "utf8")).problem
}

function loadCode(file) {
  return fs.readFileSync(file, "utf8")
}

test("verdict 只有五个，且与契约逐字一致", function () {
  assert.deepStrictEqual(runner.VERDICTS, [
    "pass",
    "wrong_answer",
    "timeout",
    "compile_error",
    "runtime_error",
  ])
})

test("五类 verdict 都备了中文面板文案", function () {
  runner.VERDICTS.forEach(function (v) {
    const panel = runner.VERDICT_PANEL[v]
    assert.ok(panel, "缺少 " + v + " 的面板文案")
    assert.ok(panel.title && panel.title.length > 0, v + " 缺标题")
    assert.ok(panel.hint && panel.hint.length > 0, v + " 缺提示")
    assert.ok(["ok", "bad", "warn"].indexOf(panel.tone) !== -1, v + " 的色调不合法")
  })
})

test("parseCompilerError 能取出行号、列号与原因", function () {
  const out = runner.parseCompilerError(
    "main.cpp:4:5: error: 'undefined_function_call' was not declared in this scope\n    4 |     undefined_function_call();\n"
  )
  assert.strictEqual(out.line, 4)
  assert.strictEqual(out.column, 5)
  assert.ok(out.text.indexOf("undefined_function_call") !== -1, out.text)
})

test("parseCompilerError 对缺 main 的链接期错误给中文说明", function () {
  const out = runner.parseCompilerError(
    "C:/mingw64/bin/ld.exe: undefined reference to `main'\ncollect2.exe: error: ld returned 1 exit status\n"
  )
  assert.strictEqual(out.line, null)
  assert.ok(out.text.indexOf("main") !== -1, out.text)
  assert.ok(out.text.indexOf("自己写 main()") !== -1, "要提醒用户自己写 main()：" + out.text)
})

test("C++ 正例判为 pass", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppOk),
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "pass")
  assert.strictEqual(out.passedCases, 3)
  assert.strictEqual(out.totalCases, 3)
  assert.strictEqual(out.failedCase, null)
  assert.strictEqual(out.compilerOutput, "")
  assert.ok(out.message.indexOf("通过") !== -1, out.message)
  assert.ok(out.durationMs >= 0)
})

test("C++ 答案错误：报告第几个用例挂的、期望 vs 实际", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppWrongAnswer),
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "wrong_answer")
  assert.strictEqual(out.passedCases, 0)
  assert.strictEqual(out.totalCases, 3)
  assert.ok(out.failedCase, "必须给出失败用例")
  assert.strictEqual(out.failedCase.index, 1)
  assert.strictEqual(out.failedCase.expected, "0 1")
  assert.ok(out.failedCase.actual.indexOf("0 0") !== -1, out.failedCase.actual)
  assert.ok(out.failedCase.input.indexOf("2 7 11 15") !== -1, out.failedCase.input)
  assert.ok(out.message.indexOf("第 1 个用例") !== -1, out.message)
})

test("C++ 编译错误：给出编译器原文与错误行号", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppCompileError),
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "compile_error")
  assert.strictEqual(out.passedCases, 0)
  assert.ok(out.compilerOutput.indexOf("error:") !== -1, out.compilerOutput)
  // fixture 里 undefined_function_call() 在第 4 行，行号必须解析对
  assert.ok(out.message.indexOf("第 4 行") !== -1, "要给出正确的错误行号：" + out.message)
  assert.ok(out.message.indexOf("undefined_function_call") !== -1, out.message)
})

test("判题契约：class Solution 没有 main() 一律编译失败，不注入包装", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: [
      "class Solution {",
      "public:",
      "    int twoSum(int a, int b) { return a + b; }",
      "};",
    ].join("\n"),
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "compile_error")
  assert.ok(out.message.indexOf("main") !== -1, "面板要说明缺 main：" + out.message)
})

test("C++ 死循环判为 timeout，并说明超时上限", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppTimeout),
    language: "cpp",
    timeoutMs: 800,
  })
  assert.strictEqual(out.verdict, "timeout")
  assert.ok(out.message.indexOf("超时") !== -1, out.message)
  assert.ok(out.message.indexOf("800") !== -1, "要说明超时上限：" + out.message)
})

test("C++ 运行时异常判为 runtime_error，并带上错误输出", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppRuntimeError),
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "runtime_error")
  assert.strictEqual(out.passedCases, 0)
  assert.ok(out.compilerOutput.length > 0, "必须带 stderr 原文")
})

test("Python 正例判为 pass（含中文用例）", async function () {
  const problem = loadProblem(manifest.PROBLEMS.pythonReverse)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.pythonOk),
    language: "python",
  })
  assert.strictEqual(out.verdict, "pass")
  assert.strictEqual(out.passedCases, 2)
  assert.strictEqual(out.totalCases, 2)
})

test("Python 答案错误判为 wrong_answer", async function () {
  const problem = loadProblem(manifest.PROBLEMS.pythonReverse)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.pythonWrongAnswer),
    language: "python",
  })
  assert.strictEqual(out.verdict, "wrong_answer")
  assert.strictEqual(out.failedCase.index, 1)
  assert.strictEqual(out.failedCase.expected, "olleh")
  assert.strictEqual(out.failedCase.actual.trim(), "hello")
})

test("Python 运行时异常判为 runtime_error，并带上 traceback", async function () {
  const problem = loadProblem(manifest.PROBLEMS.pythonReverse)
  const out = await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.pythonRuntimeError),
    language: "python",
  })
  assert.strictEqual(out.verdict, "runtime_error")
  assert.ok(out.compilerOutput.indexOf("ValueError") !== -1, out.compilerOutput)
})

test("不认识的语言直接报编译错误，不 spawn 任何东西", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const out = await runner.judge({
    problem: problem,
    code: "print(1)",
    language: "rust",
  })
  assert.strictEqual(out.verdict, "compile_error")
  assert.ok(out.message.indexOf("rust") !== -1, out.message)
})

test("没有测试用例的题目给出明确说明", async function () {
  const out = await runner.judge({
    problem: { id: "self-09999", cases: [] },
    code: "int main(){}",
    language: "cpp",
  })
  assert.strictEqual(out.verdict, "wrong_answer")
  assert.ok(out.message.indexOf("没有测试用例") !== -1, out.message)
})

test("判完临时目录被删掉，不留编译垃圾", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  const before = fs.readdirSync(require("node:os").tmpdir()).filter(function (name) {
    return name.indexOf("algo-plan-judge-") === 0
  }).length
  await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppOk),
    language: "cpp",
  })
  const after = fs.readdirSync(require("node:os").tmpdir()).filter(function (name) {
    return name.indexOf("algo-plan-judge-") === 0
  }).length
  assert.strictEqual(after, before, "临时目录判完必须删掉")
})
