"use strict"
/**
 * supply-algorithm-batch.test.js —— 批处理：配额、限速、重试、续跑、记账
 *
 * LLM 一律注入**假客户端**（G12：测试全程不联网）。
 * 真编译执行的部分由 `core/supply/algorithm-verify.js` 承担（那里自己 skip 无 g++ 的机器）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const batch = require("../core/supply/algorithm-batch.js")
const fill = require("../core/supply/algorithm-fill.js")
const { detectCompiler } = require("../judge/sandbox.js")

/** 没有 g++ 就 skip 并说清原因 */
const SKIP = detectCompiler() ? false : "本机没有 g++，无法做真实编译执行"

/** 正确的参考解法 */
const REF_CODE = [
  "#include <iostream>",
  "#include <string>",
  "#include <algorithm>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  "  std::reverse(s.begin(), s.end());",
  '  std::cout << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 负例：原样输出 */
const NEG_CODE = [
  "#include <iostream>",
  "#include <string>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  '  std::cout << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 内存里的 settings 仓储（不碰真 SQLite） */
function fakeStore(initial) {
  const data = Object.assign({}, initial || {})
  return {
    data: data,
    getSetting: function (key, fallback) {
      return data[key] === undefined ? fallback : data[key]
    },
    setSetting: function (key, value) {
      data[key] = String(value)
    },
  }
}

/** 假 git status：报干净（真实 git 路径由端到端测试覆盖） */
function fakeGit() {
  return { ok: true, stdout: "", stderr: "", error: "" }
}

/** 造一个 learning 仓根：bank/fundamentals 下 n 道空骨架 */
function fakeLearningRoot(stems) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-batch-"))
  const bank = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(bank, { recursive: true })
  stems.forEach(function (stem) {
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
    lines.push("needs_fill: true")
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

/** 假客户端：按调用次数给不同的回复 */
function fakeClient(responder) {
  return {
    name: "fake",
    model: "fake-model",
    calls: 0,
    async complete(req) {
      this.calls += 1
      return responder(req, this.calls)
    },
  }
}

/** 默认回复：一份能过的四件套 */
function goodAnswer(req) {
  return JSON.stringify({
    title: "反转字符串",
    statement: "给你一个字符串，把它反过来输出。",
    difficulty: "easy",
    topics: ["字符串"],
    cases: [{ input: "123", expect: "321" }],
    solution: REF_CODE,
    negative: NEG_CODE,
    negativeNote: "负例原样输出了输入",
  })
}

test("dry-run 只算计划，不调 LLM、不写文件", function () {
  const root = fakeLearningRoot(["lc-00001", "lc-00002"])
  try {
    const client = fakeClient(goodAnswer)
    const store = fakeStore()
    return batch
      .runBatch({
        items: fill.scanBank({ learningRoot: root }).items,
        client: client,
        store: store,
        learningRoot: root,
        unitsDir: path.join(root, "units", "algorithm"),
        gitRunner: fakeGit,
        dryRun: true,
      })
      .then(function (summary) {
        assert.equal(summary.planned, 2)
        assert.equal(summary.llmCalls, 0, "dry-run 不能调 LLM")
        assert.equal(client.calls, 0)
        assert.equal(fs.existsSync(path.join(root, "units")), false, "dry-run 不能写文件")
      })
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("跑通一批：产物落盘、status 改对、账记得住", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001", "lc-00002"])
  try {
    const client = fakeClient(goodAnswer)
    const store = fakeStore()
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: client,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(summary.planned, 2)
    assert.equal(summary.done, 2)
    assert.equal(summary.verified, 2)
    assert.equal(summary.rejected, 0)
    assert.equal(summary.failed, 0)
    assert.equal(summary.stopped, false)
    assert.equal(summary.llmCalls, 2)
    assert.equal(client.calls, 2)

    const unitsDir = path.join(root, "units", "algorithm")
    assert.ok(fs.existsSync(path.join(unitsDir, "lc-00001.md")))
    assert.ok(fs.existsSync(path.join(unitsDir, "lc-00001.reference.cpp")))
    assert.ok(fs.existsSync(path.join(unitsDir, "lc-00001.negative.cpp")))
    const md = fs.readFileSync(path.join(unitsDir, "lc-00001.md"), "utf8")
    assert.ok(md.indexOf("status: verified") !== -1, md.slice(0, 400))

    // 记账：批次汇总 + 单题进度 + 调用计数
    const run = JSON.parse(store.getSetting("supply.batch.run.batch-test", "{}"))
    assert.equal(run.batchId, "batch-test")
    assert.equal(run.track, "algorithm")
    assert.equal(run.verified, 2)
    assert.equal(run.llmCalls, 2)
    assert.equal(store.getSetting("supply.batch.calls.session.batch-test", "0"), "2")
    const item = JSON.parse(store.getSetting("supply.batch.item.algo.lc-00001", "{}"))
    assert.equal(item.state, "verified")
    assert.equal(item.attempts, 1)
    assert.equal(item.stem, "lc-00001")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("--resume 跳过已 verified 的题，不重复花钱", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001", "lc-00002"])
  try {
    const store = fakeStore()
    const first = fakeClient(goodAnswer)
    await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: first,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(first.calls, 2)

    const second = fakeClient(goodAnswer)
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: second,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
      resume: true,
    })
    assert.equal(summary.planned, 0, "已 verified 的题不该再排进本轮")
    assert.equal(summary.skipped, 2)
    assert.equal(second.calls, 0, "续跑不能再花钱")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("磁盘上已经有 verified 单元的题，不带 --resume 也不再花钱", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001"])
  try {
    const first = fakeClient(goodAnswer)
    await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: first,
      store: fakeStore(),
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(first.calls, 1)

    // 换一个空账本——模拟 settings 丢了，磁盘上的单元还在
    const second = fakeClient(goodAnswer)
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: second,
      store: fakeStore(),
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test-2",
      rateMs: 0,
    })
    assert.equal(summary.planned, 0, "磁盘上已经有 verified 的单元，不该再排进来")
    assert.equal(second.calls, 0, "不能再花钱")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("预算到顶就停，剩下的不跑", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001", "lc-00002", "lc-00003"])
  try {
    const client = fakeClient(goodAnswer)
    const store = fakeStore()
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: client,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
      sessionLimit: 1,
    })
    assert.equal(summary.stopped, true, "到预算必须停")
    assert.equal(summary.done, 1)
    assert.equal(summary.planned, 3)
    assert.equal(client.calls, 1)
    assert.ok(String(summary.reason).indexOf("还欠 2 个") !== -1, summary.reason)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("第一次返回坏 JSON，第二次重来：attempts 记 2", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001"])
  try {
    const client = fakeClient(function (req, call) {
      return call === 1 ? "这不是 JSON" : goodAnswer(req)
    })
    const store = fakeStore()
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: client,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(client.calls, 2)
    assert.equal(summary.verified, 1)
    const item = JSON.parse(store.getSetting("supply.batch.item.algo.lc-00001", "{}"))
    assert.equal(item.attempts, 2)
    assert.equal(item.state, "verified")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("负例编译不过 → rejected，单题状态记 rejected", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001"])
  try {
    const client = fakeClient(function (req) {
      const obj = JSON.parse(goodAnswer(req))
      obj.negative = "int main() { 这不是合法的 C++ }"
      return JSON.stringify(obj)
    })
    const store = fakeStore()
    const summary = await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: client,
      store: store,
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(summary.rejected, 1)
    const item = JSON.parse(store.getSetting("supply.batch.item.algo.lc-00001", "{}"))
    assert.equal(item.state, "rejected")
    assert.ok(item.reason.indexOf("负例编译失败") !== -1, item.reason)
    const md = fs.readFileSync(path.join(root, "units", "algorithm", "lc-00001.md"), "utf8")
    assert.ok(md.indexOf("status: rejected") !== -1)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("存量题库一个字节都不许动", { skip: SKIP }, async function () {
  const root = fakeLearningRoot(["lc-00001"])
  try {
    const bankFile = path.join(root, "bank", "fundamentals", "lc-00001.md")
    const before = fs.readFileSync(bankFile, "utf8")
    const client = fakeClient(goodAnswer)
    await batch.runBatch({
      items: fill.scanBank({ learningRoot: root }).items,
      client: client,
      store: fakeStore(),
      learningRoot: root,
      unitsDir: path.join(root, "units", "algorithm"),
      gitRunner: fakeGit,
      batchId: "batch-test",
      rateMs: 0,
    })
    assert.equal(fs.readFileSync(bankFile, "utf8"), before, "存量题库不许改")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})
