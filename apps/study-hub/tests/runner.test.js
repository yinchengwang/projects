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

test("编译命令是固定的那一条，不带 -Wall / -Werror", function () {
  assert.deepStrictEqual(runner.CPP_COMPILE_ARGS, [
    "-std=c++17",
    "-O2",
    "main.cpp",
    "-o",
    "main",
  ])
  const joined = runner.CPP_COMPILE_ARGS.join(" ")
  assert.strictEqual(joined.indexOf("-Wall"), -1, "不该加 -Wall")
  assert.strictEqual(joined.indexOf("-Werror"), -1, "不该加 -Werror")
  assert.strictEqual(joined.indexOf("-std=c++17"), 0, "标准必须是 C++17")
})

test("Python 子进程确实拿到了 PYTHONIOENCODING=utf-8", async function () {
  const out = await runner.judge({
    problem: { id: "self-09998", cases: [{ input: "", expect: "utf-8" }] },
    code: "import os\nprint(os.environ.get('PYTHONIOENCODING'))",
    language: "python",
  })
  assert.strictEqual(
    out.verdict,
    "pass",
    "子进程没拿到 PYTHONIOENCODING=utf-8：" + out.message + " / " + out.compilerOutput
  )
})

test("程序狂喷输出不会把判题撑爆，也不会让服务崩掉", async function () {
  const out = await runner.judge({
    problem: { id: "self-09997", cases: [{ input: "", expect: "x" }] },
    code: ["import sys", "for i in range(200000):", "    sys.stdout.write('x' * 64)", ""].join(
      "\n"
    ),
    language: "python",
    timeoutMs: 5000,
  })
  // 200000 * 64 = 12.8MB，超过沙箱的 2MB 输出上限，应当被截断后判为答案错误而不是崩溃
  assert.strictEqual(out.verdict, "wrong_answer", out.message)
  assert.ok(out.failedCase, "要给出失败用例")
  assert.ok(
    out.failedCase.actual.length <= 2 * 1024 * 1024,
    "输出应当被截断在上限之内，实际 " + out.failedCase.actual.length
  )
})

test("600MB 级别的狂喷输出不会让服务进程崩掉", async function () {
  // 这条是复审发现的 P0 的原始复现：
  // 输出不被限制时，Buffer.concat().toString() 会撞上 V8 的字符串长度上限
  // 抛 ERR_STRING_TOO_LONG，而那行代码在 close 事件回调里 ——
  // 抛出去就是 uncaughtException，整个服务进程直接死，Promise 永远不 settle。
  const out = await runner.judge({
    problem: { id: "self-09996", cases: [{ input: "", expect: "x" }] },
    code: [
      "import sys",
      "chunk = 'x' * 1048576",
      "for i in range(600):",
      "    sys.stdout.write(chunk)",
      "",
    ].join("\n"),
    language: "python",
    timeoutMs: 60000,
  })
  assert.strictEqual(out.verdict, "wrong_answer", out.message)
  assert.ok(
    out.failedCase.actual.length <= 2 * 1024 * 1024,
    "输出必须被截断在 2MB 之内，实际 " + out.failedCase.actual.length
  )
})

test("子进程留下野进程时，判题不会永远挂着", async function () {
  // 场景：提交的代码自己拉起一个后台进程就退出了。那个后台进程继承了 stdout 管道，
  // 于是「close」事件要等它一起退出才会来——如果超时后杀进程树没杀干净，
  // 这个请求就会永远挂着，临时目录也永远删不掉。
  // 沙箱里有兜底：杀掉之后给一段宽限期，到点就按超时强行收摊。
  const timeoutMs = 1000
  const started = Date.now()
  const out = await runner.judge({
    problem: { id: "self-09994", cases: [{ input: "", expect: "x" }] },
    code: [
      "import subprocess, sys",
      "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])",
      "print('done')",
      "",
    ].join("\n"),
    language: "python",
    timeoutMs: timeoutMs,
  })
  const spent = Date.now() - started
  assert.ok(
    out.durationMs <= timeoutMs + 6000,
    "判题必须在合理时间内收摊，实际用了 " + out.durationMs + "ms（" + out.verdict + "）"
  )
  assert.ok(spent < 30000, "整段判题不该跑满 30 秒，实际 " + spent + "ms")
})

test("判完临时目录被删掉，不留编译垃圾", async function () {
  const problem = loadProblem(manifest.PROBLEMS.cppSum)
  // 用集合比而不是比数量：数量比较在并发跑时会假绿（这会儿泄漏一个、那会儿清掉一个）
  function sandboxes() {
    return fs.readdirSync(require("node:os").tmpdir()).filter(function (name) {
      return name.indexOf("study-hub-judge-") === 0
    })
  }
  const before = new Set(sandboxes())
  await runner.judge({
    problem: problem,
    code: loadCode(manifest.CODE.cppOk),
    language: "cpp",
  })
  const leaked = sandboxes().filter(function (name) {
    return !before.has(name)
  })
  assert.deepStrictEqual(leaked, [], "临时目录判完必须删掉")
})
