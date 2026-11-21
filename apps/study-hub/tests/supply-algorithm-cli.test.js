"use strict"
/**
 * supply-algorithm-cli.test.js —— 算法轨 CLI：参数解析与退出码
 *
 * 退出码（契约 §10.1）：0 成功 / 2 LLM 关掉 / 1 参数错或路径解析不出来。
 * 全程不联网：LLM 注入假客户端。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const cli = require("../core/supply/algorithm-cli.js")

/** 内存里的 settings 仓储 */
function fakeStore() {
  const data = {}
  return {
    getSetting: function (key, fallback) {
      return data[key] === undefined ? fallback : data[key]
    },
    setSetting: function (key, value) {
      data[key] = String(value)
    },
  }
}

/** 造一个 learning 仓根：两道空骨架 */
function fakeLearningRoot() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-cli-"))
  const bank = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(bank, { recursive: true })
  ;["lc-00001", "lc-00002"].forEach(function (stem) {
    const lines = []
    lines.push("---")
    lines.push("id: " + stem)
    lines.push("source: leetcode")
    lines.push("source_id: 7")
    lines.push("title: 反转字符串")
    lines.push("difficulty: easy")
    lines.push("topics: [字符串]")
    lines.push("tracks: [fundamentals]")
    lines.push("status: todo")
    lines.push("---")
    lines.push("")
    lines.push("## 题面")
    lines.push("")
    lines.push("## 测试用例")
    lines.push("")
    lines.push("| input | expect |")
    lines.push("|---|---|")
    lines.push("")
    lines.push("## 参考解法")
    lines.push("")
    fs.writeFileSync(path.join(bank, stem + ".md"), lines.join("\n"), "utf8")
  })
  return root
}

/** 假客户端：记调用次数 */
function fakeClient() {
  return {
    name: "fake",
    model: "fake-model",
    calls: 0,
    async complete() {
      this.calls += 1
      return "{}"
    },
  }
}

/** 关掉的 LLM 客户端 */
function nullClient() {
  return { name: "null", model: "null", reason: "测试里关掉了", async complete() { return "" } }
}

/** 跑一条命令的公共参数 */
function base(root, extra) {
  return Object.assign(
    {
      argv: [],
      env: {},
      store: fakeStore(),
      learningRoot: root,
      localBank: path.join(root, "no-local-bank"),
      gitRunner: function () {
        return { ok: true, stdout: "", stderr: "", error: "" }
      },
    },
    extra || {}
  )
}

test("参数解析：选项各归各位", function () {
  const out = cli.parseAlgorithmArgs([
    "generate",
    "--track",
    "algorithm",
    "--batch",
    "--limit",
    "5",
    "--resume",
    "--retry-rejected",
    "--dry-run",
    "--session",
    "batch-20261113",
  ])
  assert.equal(out.error, "")
  assert.equal(out.options.command, "generate")
  assert.equal(out.options.track, "algorithm")
  assert.equal(out.options.batch, true)
  assert.equal(out.options.limit, 5)
  assert.equal(out.options.resume, true)
  assert.equal(out.options.retryRejected, true)
  assert.equal(out.options.dryRun, true)
  assert.equal(out.options.session, "batch-20261113")
})

test("参数解析：认出 --unit", function () {
  const out = cli.parseAlgorithmArgs(["verify", "--track", "algorithm", "--unit", "algo:lc-00007"])
  assert.equal(out.error, "")
  assert.equal(out.options.command, "verify")
  assert.equal(out.options.unit, "algo:lc-00007")
})

test("参数解析：坏参数要报错", function () {
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "--limit", "0"]).error, "")
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "--limit", "abc"]).error, "")
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "--limit"]).error, "")
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "--session", "a b"]).error, "")
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "--nope"]).error, "")
  assert.notEqual(cli.parseAlgorithmArgs(["generate", "extra"]).error, "")
})

test("LLM 关掉 → 退出码 2，并给中文提示", async function () {
  const root = fakeLearningRoot()
  try {
    const res = await cli.runAlgorithmCommand(base(root, { argv: ["generate", "--track", "algorithm"], client: nullClient() }))
    assert.equal(res.code, 2)
    assert.ok(res.output.indexOf("LLM 未启用") !== -1, res.output)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("参数错 → 退出码 1", async function () {
  const root = fakeLearningRoot()
  try {
    const res = await cli.runAlgorithmCommand(base(root, { argv: ["generate", "--limit", "0"], client: fakeClient() }))
    assert.equal(res.code, 1)
    assert.ok(res.output.indexOf("参数错") !== -1, res.output)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("子命令不认识 → 退出码 1", async function () {
  const root = fakeLearningRoot()
  try {
    const res = await cli.runAlgorithmCommand(base(root, { argv: ["review", "--track", "algorithm"], client: fakeClient() }))
    assert.equal(res.code, 1)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("learning 仓解析不出来 → 退出码 1", async function () {
  const res = await cli.runAlgorithmCommand({
    argv: ["plan", "--track", "algorithm"],
    env: {},
    store: fakeStore(),
    learningRoot: undefined,
    localBank: path.join(os.tmpdir(), "no-such-bank"),
  })
  assert.equal(res.code, 1)
  assert.ok(res.output.indexOf("解析不到 learning 仓") !== -1, res.output)
})

test("plan 打出缺口清单，退出码 0", async function () {
  const root = fakeLearningRoot()
  try {
    const res = await cli.runAlgorithmCommand(base(root, { argv: ["plan", "--track", "algorithm"], client: fakeClient() }))
    assert.equal(res.code, 0, res.output)
    assert.ok(res.output.indexOf("缺口清单") !== -1, res.output)
    assert.ok(res.output.indexOf("lc-00001") !== -1, res.output)
    assert.ok(fs.existsSync(path.join(root, "units", "algorithm", "plan.json")), "plan.json 要落地")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("report 打出覆盖报告，退出码 0", async function () {
  const root = fakeLearningRoot()
  try {
    const res = await cli.runAlgorithmCommand(base(root, { argv: ["report", "--track", "algorithm"], client: fakeClient() }))
    assert.equal(res.code, 0, res.output)
    assert.ok(res.output.indexOf("覆盖报告") !== -1, res.output)
    assert.ok(res.output.indexOf("还欠 2 题") !== -1, res.output)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("generate --dry-run 不调 LLM、不写单元", async function () {
  const root = fakeLearningRoot()
  try {
    const client = fakeClient()
    const res = await cli.runAlgorithmCommand(
      base(root, { argv: ["generate", "--track", "algorithm", "--dry-run"], client: client })
    )
    assert.equal(res.code, 0, res.output)
    assert.equal(client.calls, 0, "dry-run 不能调 LLM")
    assert.equal(fs.existsSync(path.join(root, "units", "algorithm", "lc-00001.md")), false)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})
