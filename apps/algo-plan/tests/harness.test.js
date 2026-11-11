"use strict"
/**
 * harness.test.js —— 测试入口自检：run-all.js 自己别出岔子
 *
 * run-all.js 是「跑测试的测试」，它要是空跑还退出 0，
 * 整套回归就等于没有。这里把它复制到临时目录里单独跑。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync, spawnSync } = require("node:child_process")
const { tmpDir } = require("./helpers/tmp.js")

const RUN_ALL = path.join(__dirname, "run-all.js")

/** 建一个临时目录，把 run-all.js 复制进去，模拟「另一个工程」 */
function makeSandbox() {
  const dir = tmpDir("algo-plan-harness-")
  const testsDir = path.join(dir, "tests")
  fs.mkdirSync(testsDir)
  fs.copyFileSync(RUN_ALL, path.join(testsDir, "run-all.js"))
  return {
    dir: dir,
    testsDir: testsDir,
    cleanup: function () {
      fs.rmSync(dir, { recursive: true, force: true })
    },
  }
}

test("run-all.js 在没有测试套件时报错退出（退出码非 0）", function () {
  const sandbox = makeSandbox()
  try {
    const res = spawnSync(process.execPath, [path.join(sandbox.testsDir, "run-all.js")], {
      encoding: "utf8",
    })
    assert.notStrictEqual(res.status, 0, "空套件时退出码必须非 0，不能静默通过")
    assert.ok(
      (res.stderr || "").indexOf("没有找到任何测试套件") !== -1,
      "应该打印中文提示，实际输出：" + JSON.stringify(res.stderr)
    )
    // 提示必须完整送到：stderr 是异步管道时，process.exit() 会在管道把数据
    // 刷出去之前就把进程干掉，尾部会被截断。run-all.js 现在改用
    // process.exitCode 让进程自然退出，所以尾部也必须在。
    assert.ok(
      (res.stderr || "").indexOf("请先创建测试文件") !== -1,
      "中文提示的尾部不能被截断，实际输出：" + JSON.stringify(res.stderr)
    )
  } finally {
    sandbox.cleanup()
  }
})

test("run-all.js 的输出里能看到断言通过数", function () {
  const sandbox = makeSandbox()
  try {
    fs.writeFileSync(
      path.join(sandbox.testsDir, "fake.test.js"),
      'const test = require("node:test")\ntest("占位断言", function () {})\n',
      "utf8"
    )
    const out = execFileSync(process.execPath, [path.join(sandbox.testsDir, "run-all.js")], {
      encoding: "utf8",
    })
    assert.ok(out.indexOf("pass 1") !== -1, "输出里应能看到 pass 计数，实际输出：\n" + out)
    assert.ok(out.indexOf("1 / 1 套件通过") !== -1, "输出里应能看到套件通过数，实际输出：\n" + out)
  } finally {
    sandbox.cleanup()
  }
})
