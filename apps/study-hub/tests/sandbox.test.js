"use strict"
/**
 * sandbox.test.js —— 判题沙箱：工具链探测、临时目录、喂 stdin、超时杀整棵进程树
 *
 * 这个套件**真的**会编译、真的会跑子进程、真的会在 Windows 上 taskkill 进程树。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const sandbox = require("../judge/sandbox.js")
const { tmpDir } = require("./helpers/tmp.js")

test("detectCompiler 找到本机的 g++ 并给出版本", function () {
  const compiler = sandbox.detectCompiler()
  assert.ok(compiler, "本机应能找到 g++")
  assert.strictEqual(compiler.cmd, "g++")
  assert.ok(compiler.version.indexOf("g++") !== -1, compiler.version)
})

test("detectPython 选到的是**真的能跑**的解释器（不是 Store 占位符）", async function () {
  const python = sandbox.detectPython()
  assert.ok(python, "本机应能找到可用的 Python")
  assert.ok(
    ["python3", "python", "py"].indexOf(python.cmd) !== -1,
    "探测顺序只能是 python3 / python / py，实际是 " + python.cmd
  )
  const run = await sandbox.runCommand(python.cmd, python.args.concat(["-c", "print(6 * 7)"]), {})
  assert.strictEqual(run.code, 0)
  assert.strictEqual(run.stdout.trim(), "42", "选中的解释器必须能真的执行代码")
})

test("detectPython 的版本号形如 Python 3.x", function () {
  const python = sandbox.detectPython()
  assert.ok(/^Python 3\./.test(python.version), python.version)
})

test("detectPython 支持用户指定解释器 py -3", function () {
  const python = sandbox.detectPython("py -3")
  assert.strictEqual(python.cmd, "py")
  assert.deepStrictEqual(python.args, ["-3"])
})

test("detectPython 指定的解释器不能用时回落到标准顺序", function () {
  const python = sandbox.detectPython("根本不存在的解释器-xyz")
  assert.ok(python, "应回落到 python3 / python / py -3")
  assert.ok(["python3", "python", "py"].indexOf(python.cmd) !== -1, python.cmd)
})

test("createSandbox 建出临时目录，cleanup 后目录消失", function () {
  const box = sandbox.createSandbox()
  assert.ok(fs.existsSync(box.dir), "临时目录应存在")
  assert.ok(box.dir.indexOf("study-hub-judge-") !== -1, box.dir)
  box.cleanup()
  assert.strictEqual(fs.existsSync(box.dir), false, "cleanup 后目录应被删掉")
})

test("cleanup 调两次不会报错", function () {
  const box = sandbox.createSandbox()
  box.cleanup()
  box.cleanup()
})

test("runCommand 捕获 stdout 与退出码", async function () {
  const run = await sandbox.runCommand(process.execPath, ["-e", "console.log('hello')"], {})
  assert.strictEqual(run.code, 0)
  assert.strictEqual(run.stdout.trim(), "hello")
  assert.strictEqual(run.timedOut, false)
  assert.ok(run.durationMs >= 0)
})

test("runCommand 把 input 喂进 stdin", async function () {
  const run = await sandbox.runCommand(
    process.execPath,
    ["-e", "process.stdin.on('data', function (d) { process.stdout.write('got:' + d) })"],
    { input: "abc" }
  )
  assert.strictEqual(run.code, 0)
  assert.ok(run.stdout.indexOf("got:abc") !== -1, run.stdout)
})

test("runCommand 捕获非零退出码", async function () {
  const run = await sandbox.runCommand(process.execPath, ["-e", "process.exit(3)"], {})
  assert.strictEqual(run.code, 3)
  assert.strictEqual(run.timedOut, false)
})

test("runCommand 传入 env 后子进程能看到", async function () {
  const run = await sandbox.runCommand(
    process.execPath,
    ["-e", "process.stdout.write(String(process.env.ALGO_PLAN_TEST_ENV || ''))"],
    { env: Object.assign({}, process.env, { ALGO_PLAN_TEST_ENV: "看得见" }) }
  )
  assert.strictEqual(run.stdout, "看得见")
})

test("runCommand 超时会掐断子进程并标记 timedOut", async function () {
  const run = await sandbox.runCommand(process.execPath, ["-e", "setInterval(function () {}, 1000)"], {
    timeoutMs: 600,
  })
  assert.strictEqual(run.timedOut, true)
  assert.ok(run.durationMs >= 600, "耗时 " + run.durationMs + " 应不少于 600")
})

test("超时后整棵进程树都被杀掉（孙进程也停）", async function () {
  const dir = tmpDir("study-hub-tree-")
  const log = path.join(dir, "grandchild.log")
  const parent = path.join(dir, "parent.js")
  const grandchild = path.join(dir, "grandchild.js")

  // 孙进程：每 100ms 往日志里追加一行，只要它活着，日志就在长
  fs.writeFileSync(
    grandchild,
    [
      '"use strict"',
      'const fs = require("node:fs")',
      'setInterval(function () { fs.appendFileSync(process.argv[2], "x\\n") }, 100)',
    ].join("\n"),
    "utf8"
  )
  // 父进程：拉起孙进程，然后自己永远活着
  fs.writeFileSync(
    parent,
    [
      '"use strict"',
      'const { spawn } = require("node:child_process")',
      'spawn(process.execPath, [process.argv[2], process.argv[3]], { stdio: "ignore", windowsHide: true })',
      'setInterval(function () {}, 1000)',
    ].join("\n"),
    "utf8"
  )
  fs.writeFileSync(log, "", "utf8")

  const run = await sandbox.runCommand(process.execPath, [parent, grandchild, log], {
    cwd: dir,
    timeoutMs: 1200,
  })
  assert.strictEqual(run.timedOut, true)

  const size1 = fs.statSync(log).size
  await new Promise(function (resolve) {
    setTimeout(resolve, 700)
  })
  const size2 = fs.statSync(log).size
  assert.strictEqual(size2, size1, "孙进程还在写日志，说明整棵进程树没被杀干净")
})
