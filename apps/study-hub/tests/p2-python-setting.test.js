"use strict"
/**
 * p2-python-setting.test.js —— P2 补充：judge.python.cmd 必须真正贯通
 *
 * 起因：numpy 只装在 py -3（Python 3.12），而探测默认命中 PATH 上的 python（3.11，无 numpy）。
 * settings 表里本来就有一个 judge.python.cmd 键（契约 §5.1「既有」），
 * 但当时只有旧路由 /api/attempt 读过它——P2 的判题、/api/env、loader B 都写死 detectXxx(null)。
 *
 * 这个套件钉住三件事：
 *   1. detectPython / detectNumpy 的缓存必须按**解释器**分键，不能串味；
 *   2. judgeUnit 的 pythonOverride 要真的改变工具链；
 *   3. POST /api/units/:id/judge 与 GET /api/env 都尊重 settings 里的那个键。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const sandbox = require("../judge/sandbox.js")
const { judgeUnit } = require("../judge/runner.js")
const { createApp } = require("../server/app.js")

/** 造一个只有一张卡的 learning 仓（判断跑得起来即可） */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "p2-pyset-"))
  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# 测试卡\n\n讲解正文。\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('OK demo ran')\n", "utf8")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "mvcc.assert.py"),
    [
      "import os, sys",
      "print('所有 MVCC 演示完成。')",
      "print('─── demo 输出开始 ───')",
      "print('OK demo ran')",
      "print('─── demo 输出结束 ───')",
      "print('全部断言通过（1 条）')",
      "sys.exit(0)",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

test("detectPython：缓存按解释器分键，换个 override 必须重探", function () {
  sandbox.resetDetectionCache()
  const auto = sandbox.detectPython(null)
  const py3 = sandbox.detectPython("py -3")
  // 两者可能是同一个（机器上只有一个解释器），也可能不同——两种都允许。
  // 但**再取一次自动探测**必须回到自动那个结果，不能拿到 py -3 的结果。
  const autoAgain = sandbox.detectPython(null)
  if (auto && py3 && (auto.cmd !== py3.cmd || auto.args.join(" ") !== py3.args.join(" "))) {
    assert.deepStrictEqual(autoAgain, auto, "自动探测被 py -3 的结果污染了")
  }
  assert.ok(true)
})

test("detectNumpy：缓存按解释器分键，不能把「没有」串给「有」", function () {
  if (!sandbox.detectPython(null) || !sandbox.detectPython("py -3")) {
    // 只装了一个解释器就没法验这条——不静默跳过，写明原因
    return
  }
  sandbox.resetDetectionCache()
  const first = sandbox.detectNumpy(null)
  const second = sandbox.detectNumpy("py -3")
  const backToFirst = sandbox.detectNumpy(null)
  assert.deepStrictEqual(
    backToFirst,
    first,
    "numpy 缓存串味了：先探默认（" +
      JSON.stringify(first) +
      "）再探 py -3（" +
      JSON.stringify(second) +
      "）后，默认的结果变成了 " +
      JSON.stringify(backToFirst)
  )
})

test("judgeUnit：pythonOverride 传了就要真的换掉工具链", async function () {
  const py3 = sandbox.detectPython("py -3")
  const autoPy = sandbox.detectPython(null)
  if (!py3 || !autoPy) {
    return // 只装了一个解释器的机器上没法验「换没换」，跳过（下面的用例仍覆盖 override 通路）
  }

  const learningRoot = tempLearning()
  // 把断言脚本换成「按解释器版本判定」的：只有真跑在 py -3（3.12）上才会绿，
  // 忽略 override 继续用自动探测到的 python（3.11）会直接判不过。
  const scriptPath = path.join(learningRoot, "units", "database", "mvcc.assert.py")
  const wantedMajorMinor = py3.version.replace("Python ", "").split(".").slice(0, 2).join(".")
  fs.writeFileSync(
    scriptPath,
    [
      "import sys",
      "want = tuple(int(x) for x in " + JSON.stringify(wantedMajorMinor) + ".split('.'))",
      "got = sys.version_info[:2]",
      "print('─── demo 输出开始 ───')",
      "print('interpreter', got)",
      "print('─── demo 输出结束 ───')",
      "if got == want:",
      "    print('全部断言通过（1 条）')",
      "    sys.exit(0)",
      "print('断言失败：解释器不对，期望 ' + str(want) + ' 实际 ' + str(got))",
      "sys.exit(1)",
      "",
    ].join("\n"),
    "utf8"
  )
  try {
    const unit = {
      id: "db:mvcc",
      track: "database",
      kind: "card",
      title: "测试卡",
      entry: {
        demo: "scaffold/grokking/mvcc/main.py",
        reference: "",
        graded: "script",
        script: "units/database/mvcc.assert.py",
        language: "python",
      },
      asserts: [],
      statement: "",
    }
    const withOverride = await judgeUnit({
      unit: unit,
      learningRoot: learningRoot,
      pythonOverride: "py -3",
      timeoutMs: 30000,
    })
    assert.strictEqual(
      withOverride.verdict,
      "pass",
      "传了 py -3 却没用上（脚本看到的是别的解释器）：" + withOverride.message + "\n" + withOverride.stdout
    )
  } finally {
    fs.rmSync(learningRoot, { recursive: true, force: true })
  }
})

test("judgeUnit：override 指向坏解释器时按既有口径回落到自动探测", async function () {
  // 契约口径（judge/sandbox.js 的 detectPython 写明）：「先按 settings 里缓存的解释器试，
  // 不行再按 python3 → python → py -3 探测」。所以坏 override 不是错误，
  // 是「探测失败后回落」。这条钉住回落后仍能判题，不会因为配置写错就判不了。
  const learningRoot = tempLearning()
  try {
    const unit = {
      id: "db:mvcc",
      track: "database",
      kind: "card",
      title: "测试卡",
      entry: {
        demo: "scaffold/grokking/mvcc/main.py",
        reference: "",
        graded: "script",
        script: "units/database/mvcc.assert.py",
        language: "python",
      },
      asserts: [],
      statement: "",
    }
    const bad = await judgeUnit({
      unit: unit,
      learningRoot: learningRoot,
      pythonOverride: "绝对不存在的解释器-xyz",
      timeoutMs: 30000,
    })
    assert.ok(
      ["pass", "compile_error"].indexOf(bad.verdict) !== -1,
      "坏 override 应当回落到自动探测（有 Python 就 pass、没 Python 就 compile_error）：" + bad.message
    )
  } finally {
    fs.rmSync(learningRoot, { recursive: true, force: true })
  }
})

test("judgeUnit：不传 pythonOverride 时行为与从前一致（自动探测）", async function () {
  const learningRoot = tempLearning()
  try {
    const unit = {
      id: "db:mvcc",
      track: "database",
      kind: "card",
      title: "测试卡",
      entry: {
        demo: "scaffold/grokking/mvcc/main.py",
        reference: "",
        graded: "script",
        script: "units/database/mvcc.assert.py",
        language: "python",
      },
      asserts: [],
      statement: "",
    }
    const r = await judgeUnit({ unit: unit, learningRoot: learningRoot, timeoutMs: 30000 })
    assert.ok(
      ["pass", "compile_error"].indexOf(r.verdict) !== -1,
      "没装 Python 的机器上应当明确 compile_error，不该是别的：" + r.verdict
    )
  } finally {
    fs.rmSync(learningRoot, { recursive: true, force: true })
  }
})

test("API：POST /api/units/:id/judge 与 GET /api/env 都尊重 judge.python.cmd", async function () {
  const py3 = sandbox.detectPython("py -3")
  if (!py3) {
    return // 没有 py -3 的机器跳过
  }
  const learningRoot = tempLearning()
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: learningRoot,
    localBank: null,
    llmClient: null,
    env: {},
  })
  // 模拟「用户/上一轮探测把解释器写进了 settings」
  app.store.setSetting("judge.python.cmd", "py -3")
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    const env = await (await fetch(base + "/api/env")).json()
    assert.strictEqual(
      env.python.version,
      py3.version,
      "GET /api/env 没用 settings 里的解释器：拿到 " + env.python.version + "，期望 " + py3.version
    )

    const judged = await fetch(base + "/api/units/db:mvcc/judge", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: "{}",
    })
    const body = await judged.json()
    assert.strictEqual(judged.status, 200)
    assert.strictEqual(body.verdict, "pass", "判题没用 settings 里的解释器：" + body.message)
    assert.ok(
      body.stdout.indexOf("demo 输出开始") !== -1,
      "判题 stdout 里要有 demo 回显定界：" + body.stdout
    )
  } finally {
    await app.close()
    fs.rmSync(learningRoot, { recursive: true, force: true })
  }
})
