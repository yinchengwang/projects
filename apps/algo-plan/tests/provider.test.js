"use strict"
/**
 * provider.test.js —— FileProvider 读写 learning/bank
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { FileProvider } = require("../core/bank/provider.js")
const { createProblem } = require("../core/bank/problem.js")
const { serializeProblemMarkdown } = require("../core/bank/parse.js")

function tempBank() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-bank-"))
  return dir
}

function sampleProblem(id, overrides) {
  return createProblem(
    Object.assign(
      {
        id: id,
        source: "leetcode",
        title: "题目 " + id,
        difficulty: "easy",
        topics: ["数组"],
        tracks: ["interview"],
        statement: "题面 " + id,
        cases: [{ input: "1", expect: "1", tol: null }],
        solution: "int main(){}",
        solutionLang: "cpp",
      },
      overrides || {}
    )
  )
}

test("bank 目录不存在时 list 返回空数组而不是报错", async function () {
  const provider = new FileProvider(path.join(tempBank(), "not-exist"))
  assert.deepStrictEqual(await provider.list(), [])
  assert.strictEqual(await provider.get("lc-00001"), undefined)
})

test("bank 目录为 null（纯本地模式）时 list 返回空数组", async function () {
  const provider = new FileProvider(null)
  assert.deepStrictEqual(await provider.list(), [])
})

test("put 写入 learning/bank/<track>/<id>.md 并能读回", async function () {
  const bank = tempBank()
  const provider = new FileProvider(bank)
  const file = await provider.put(sampleProblem("lc-00001"))
  assert.strictEqual(file, path.join(bank, "interview", "lc-00001.md"))
  assert.ok(fs.existsSync(file))

  const got = await provider.get("lc-00001")
  assert.strictEqual(got.title, "题目 lc-00001")
  assert.strictEqual(got.cases.length, 1)
})

test("put 拒绝字段非法的题目", async function () {
  const provider = new FileProvider(tempBank())
  await assert.rejects(function () {
    return provider.put({ id: "bad" })
  })
})

test("list 支持按 track / topic / difficulty 过滤", async function () {
  const bank = tempBank()
  const provider = new FileProvider(bank)
  await provider.put(sampleProblem("lc-00001", { tracks: ["interview"], topics: ["数组"] }))
  await provider.put(sampleProblem("lc-00002", { tracks: ["advanced"], topics: ["图论"], difficulty: "hard" }))

  assert.strictEqual((await provider.list({})).length, 2)
  assert.strictEqual((await provider.list({ track: "advanced" })).length, 1)
  assert.strictEqual((await provider.list({ topic: "数组" })).length, 1)
  assert.strictEqual((await provider.list({ difficulty: "hard" })).length, 1)
  assert.strictEqual((await provider.list({ track: "advanced" }))[0].id, "lc-00002")
})

test("list 结果按 id 排序", async function () {
  const bank = tempBank()
  const provider = new FileProvider(bank)
  await provider.put(sampleProblem("lc-00003"))
  await provider.put(sampleProblem("lc-00001"))
  await provider.put(sampleProblem("lc-00002"))
  const ids = (await provider.list({})).map(function (p) {
    return p.id
  })
  assert.deepStrictEqual(ids, ["lc-00001", "lc-00002", "lc-00003"])
})

test("缺 id 的文件被跳过并计入告警，不影响其它题", async function () {
  const bank = tempBank()
  fs.mkdirSync(path.join(bank, "interview"), { recursive: true })
  fs.writeFileSync(path.join(bank, "interview", "broken.md"), "## 题面\n\n没有 frontmatter", "utf8")
  const provider = new FileProvider(bank)
  await provider.put(sampleProblem("lc-00001"))
  const list = await provider.list({})
  assert.strictEqual(list.length, 1)
  const stats = provider.stats()
  assert.strictEqual(stats.problemCount, 1)
  assert.ok(
    stats.warnings.some(function (w) {
      return w.indexOf("broken.md") !== -1
    }),
    stats.warnings.join("|")
  )
})

test("写出的文件能再次解析回来（往返一致）", async function () {
  const bank = tempBank()
  const provider = new FileProvider(bank)
  const original = sampleProblem("lc-00001")
  await provider.put(original)
  const text = fs.readFileSync(path.join(bank, "interview", "lc-00001.md"), "utf8")
  const { parseProblemMarkdown } = require("../core/bank/parse.js")
  const parsed = parseProblemMarkdown(text).problem
  assert.strictEqual(parsed.id, original.id)
  assert.strictEqual(parsed.title, original.title)
  assert.deepStrictEqual(parsed.topics, original.topics)
  assert.strictEqual(parsed.solution, original.solution)
})

test("serializeProblemMarkdown 产出 frontmatter", function () {
  const text = serializeProblemMarkdown(sampleProblem("lc-00001"))
  assert.ok(text.indexOf("---\nid: lc-00001") === 0, text.slice(0, 40))
  assert.ok(text.indexOf("needs_fill: false") !== -1)
})
