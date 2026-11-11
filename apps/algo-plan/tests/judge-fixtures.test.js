"use strict"
/**
 * judge-fixtures.test.js —— 判题 fixture 自检
 *
 * 这套测试保证 fixture 本身没坏：
 *   - 题面文件是合法的题库 markdown，用例齐全，参考解法与 code/ 里的正例逐字一致
 *   - 三类负例**真的**坏：编译错误真的编不过、死循环真的跑不完、运行时异常真的非零退出
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const { parseProblemMarkdown } = require("../core/bank/parse.js")
const { validateProblem } = require("../core/bank/problem.js")
const sandbox = require("../judge/sandbox.js")
const manifest = require("../fixtures/judge/manifest.js")
const { tmpDir } = require("./helpers/tmp.js")

function read(file) {
  return fs.readFileSync(file, "utf8")
}

test("fixture 清单里的文件全都存在", function () {
  const all = Object.keys(manifest.PROBLEMS)
    .map(function (k) {
      return manifest.PROBLEMS[k]
    })
    .concat(
      Object.keys(manifest.CODE).map(function (k) {
        return manifest.CODE[k]
      })
    )
  all.forEach(function (file) {
    assert.ok(fs.existsSync(file), "缺文件：" + file)
  })
})

test("self-00010 是合法的题库题目且用例齐全", function () {
  const parsed = parseProblemMarkdown(read(manifest.PROBLEMS.cppSum))
  const problem = parsed.problem
  assert.deepStrictEqual(parsed.warnings, [])
  assert.strictEqual(problem.id, "self-00010")
  assert.strictEqual(problem.title, "两数之和（裸程序契约正例）")
  assert.ok(validateProblem(problem).ok, "题目字段应合法")
  assert.strictEqual(problem.cases.length, 3)
  assert.strictEqual(problem.solutionLang, "cpp")
  assert.ok(problem.statement.indexOf("main") !== -1, "题面要写清是裸程序契约")
})

test("self-00011 是合法的题库题目且用例齐全", function () {
  const parsed = parseProblemMarkdown(read(manifest.PROBLEMS.pythonReverse))
  const problem = parsed.problem
  assert.deepStrictEqual(parsed.warnings, [])
  assert.strictEqual(problem.id, "self-00011")
  assert.strictEqual(problem.cases.length, 2)
  assert.strictEqual(problem.solutionLang, "python")
  assert.strictEqual(problem.cases[1].expect, "字练", "第二个用例必须是中文，验证 UTF-8 处理")
})

test("题库里的参考解法与 code/ 里的正例逐字一致", function () {
  const cpp = parseProblemMarkdown(read(manifest.PROBLEMS.cppSum)).problem
  const py = parseProblemMarkdown(read(manifest.PROBLEMS.pythonReverse)).problem
  assert.strictEqual(cpp.solution.trim(), read(manifest.CODE.cppOk).trim())
  assert.strictEqual(py.solution.trim(), read(manifest.CODE.pythonOk).trim())
})

test("负例：cpp-compile-error 真的编不过", async function () {
  const dir = tmpDir("algo-plan-fx-")
  const source = path.join(dir, "main.cpp")
  fs.copyFileSync(manifest.CODE.cppCompileError, source)
  const run = await sandbox.runCommand("g++", ["-std=c++17", "-O2", "main.cpp", "-o", "main"], {
    cwd: dir,
    timeoutMs: 10000,
  })
  assert.notStrictEqual(run.code, 0, "编译错误 fixture 必须编不过")
  assert.ok(run.stderr.indexOf("error:") !== -1, run.stderr)
})

test("负例：cpp-timeout 真的停不下来", async function () {
  const dir = tmpDir("algo-plan-fx-")
  const source = path.join(dir, "main.cpp")
  fs.copyFileSync(manifest.CODE.cppTimeout, source)
  const compile = await sandbox.runCommand("g++", ["-std=c++17", "-O2", "main.cpp", "-o", "main"], {
    cwd: dir,
    timeoutMs: 10000,
  })
  assert.strictEqual(compile.code, 0, "超时 fixture 本身必须能编译过：" + compile.stderr)
  // 必须用绝对路径：spawn 的可执行文件是相对**父进程** cwd 解析的，cwd 选项只影响子进程自己的工作目录
  const binary = path.join(dir, fs.existsSync(path.join(dir, "main.exe")) ? "main.exe" : "main")
  const run = await sandbox.runCommand(binary, [], { cwd: dir, timeoutMs: 800 })
  assert.strictEqual(run.timedOut, true, "死循环 fixture 必须跑不完")
})

test("负例：cpp-runtime-error 真的非零退出", async function () {
  const dir = tmpDir("algo-plan-fx-")
  const source = path.join(dir, "main.cpp")
  fs.copyFileSync(manifest.CODE.cppRuntimeError, source)
  const compile = await sandbox.runCommand("g++", ["-std=c++17", "-O2", "main.cpp", "-o", "main"], {
    cwd: dir,
    timeoutMs: 10000,
  })
  assert.strictEqual(compile.code, 0, compile.stderr)
  // 绝对路径，见上面那条注释
  const binary = path.join(dir, fs.existsSync(path.join(dir, "main.exe")) ? "main.exe" : "main")
  const run = await sandbox.runCommand(binary, [], { cwd: dir, timeoutMs: 5000 })
  assert.notStrictEqual(run.code, 0, "运行时异常 fixture 必须非零退出")
  assert.ok(run.stderr.length > 0, "必须留下错误输出")
})

test("负例：python-runtime-error 真的非零退出并留下 traceback", async function () {
  const python = sandbox.detectPython()
  assert.ok(python)
  const dir = tmpDir("algo-plan-fx-")
  const source = path.join(dir, "main.py")
  fs.copyFileSync(manifest.CODE.pythonRuntimeError, source)
  const run = await sandbox.runCommand(python.cmd, python.args.concat(["main.py"]), {
    cwd: dir,
    timeoutMs: 5000,
    env: Object.assign({}, process.env, { PYTHONIOENCODING: "utf-8" }),
  })
  assert.strictEqual(run.code, 1)
  assert.ok(run.stderr.indexOf("ValueError") !== -1, run.stderr)
})
