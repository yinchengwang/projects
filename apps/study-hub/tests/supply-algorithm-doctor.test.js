"use strict"
/**
 * supply-algorithm-doctor.test.js —— 环境自检：工具链、路径、配额、密钥
 *
 * 不联网、不编译（只查命令在不在）、不写真实 learning 仓。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const doctor = require("../core/supply/doctor.js")

/** 造一个带 bank 的临时 learning 仓 */
function makeRepo() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-doctor-"))
  const bank = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(bank, { recursive: true })
  fs.writeFileSync(
    path.join(bank, "lc-00001.md"),
    [
      "---", "id: lc-00001", "source: leetcode", "title: 空骨架", "difficulty: easy",
      "topics: [字符串]", "tracks: [fundamentals]", "status: todo", "---", "",
      "## 题面", "", "", "## 测试用例", "", "| input | expect |", "|---|---|", "", "## 参考解法", "",
    ].join("\n"),
    "utf8"
  )
  return root
}

test("环境齐全 → ok，且报出 Node / g++ / git / 题库 / 配额", function () {
  const root = makeRepo()
  try {
    const res = doctor.runDoctor({ learningRoot: root, store: null, env: { ARK_API_KEY: "x" } })
    const names = res.items.map(function (it) { return it.name })
    for (const name of ["Node.js", "g++ 编译器", "git", "learning 仓", "题库扫描", "产物目录", "批处理配额", "LLM 密钥"]) {
      assert.ok(names.indexOf(name) !== -1, "缺这一项：" + name + " 实际有 " + names.join("、"))
    }
    const node = res.items.find(function (it) { return it.name === "Node.js" })
    assert.equal(node.status, "ok", node.detail)
    const scan = res.items.find(function (it) { return it.name === "题库扫描" })
    assert.equal(scan.status, "ok", scan.detail)
    assert.ok(scan.detail.indexOf("共 1 题") !== -1, scan.detail)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("没配密钥 → 密钥项是 warn 不是 bad（离线命令照常可用）", function () {
  const root = makeRepo()
  try {
    const res = doctor.runDoctor({ learningRoot: root, env: {} })
    const key = res.items.find(function (it) { return it.name === "LLM 密钥" })
    assert.equal(key.status, "warn", key.detail)
    assert.ok(key.detail.indexOf("generate 跑不了") !== -1, key.detail)
    assert.equal(res.ok, true, "密钥缺失不该让整体变 bad")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("密钥只在 dotenv 里 → 仍然报「没配」并说明 .env 在", function () {
  const root = makeRepo()
  try {
    const res = doctor.runDoctor({ learningRoot: root, env: {}, hasDotEnv: true })
    const key = res.items.find(function (it) { return it.name === "LLM 密钥" })
    assert.equal(key.status, "warn")
    assert.ok(key.detail.indexOf(".env 在") !== -1, key.detail)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("learning 仓没解析到 → bad，并说写回不可用", function () {
  const res = doctor.runDoctor({ learningRoot: null, env: {} })
  const repo = res.items.find(function (it) { return it.name === "learning 仓" })
  assert.equal(repo.status, "bad", repo.detail)
  assert.ok(repo.detail.indexOf("写回不可用") !== -1, repo.detail)
  assert.equal(res.ok, false)
})

test("learning 目录不存在 → bad", function () {
  const res = doctor.runDoctor({ learningRoot: "/no/such/learning", env: {} })
  const repo = res.items.find(function (it) { return it.name === "learning 仓" })
  assert.equal(repo.status, "bad")
})

test("配额按 settings 读出来（与 runBatch 同一口径）", function () {
  const store = {
    getSetting: function (key, fallback) {
      if (key === "supply.batch.session_limit") return "10"
      if (key === "supply.batch.rate_ms") return "50"
      return fallback
    },
  }
  const res = doctor.runDoctor({ learningRoot: null, store: store, env: {} })
  const quot = res.items.find(function (it) { return it.name === "批处理配额" })
  assert.equal(quot.status, "ok", quot.detail)
  assert.ok(quot.detail.indexOf("单批 10 次") !== -1, quot.detail)
  assert.ok(quot.detail.indexOf("50 毫秒") !== -1, quot.detail)
})

test("renderDoctor 打出 ✅/… 两种记号与中文结论", function () {
  const root = makeRepo()
  try {
    const res = doctor.runDoctor({ learningRoot: root, env: {} })
    const text = doctor.renderDoctor(res)
    assert.ok(text.indexOf("算法轨环境自检") !== -1, text)
    assert.ok(text.indexOf("✅") !== -1, text)
    assert.ok(text.indexOf("…") !== -1, text)
    assert.ok(text.indexOf("环境就绪") !== -1, text)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("有 bad 项时结论改成「先解决再跑 generate」", function () {
  const res = doctor.runDoctor({ learningRoot: null, env: {} })
  const text = doctor.renderDoctor(res)
    assert.ok(text.indexOf("挡住了") !== -1, text)
})

test("产物目录写成临时目录以外的地方也能报 ok（可写探测不留痕）", function () {
  const root = makeRepo()
  try {
    const res = doctor.runDoctor({ learningRoot: root, env: {} })
    const dir = res.items.find(function (it) { return it.name === "产物目录" })
    assert.equal(dir.status, "ok", dir.detail)
    const unitsDir = path.join(root, "units", "algorithm")
    assert.equal(fs.existsSync(path.join(unitsDir, ".doctor-write-probe")), false, "探测文件必须删掉")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})
