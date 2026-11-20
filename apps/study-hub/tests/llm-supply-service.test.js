"use strict"
/**
 * llm-supply-service.test.js —— 供给服务层：四个场景 + 落盘
 * 全程不联网：所有用例都注入假客户端。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { createLlmService } = require("../core/llm/service.js")
const { stateFilePath } = require("../core/supply/unit-file.js")

const OK_SCRIPT = [
  "import os", "import shutil", "import subprocess", "import sys", "import tempfile",
  "DEMO = os.environ.get('STUDY_HUB_DEMO') or os.path.join(",
  "    os.path.dirname(os.path.abspath(__file__)), 'main.py')",
  "def decode(raw):", "    for enc in ('utf-8', 'gbk'):", "        try:",
  "            return raw.decode(enc)", "        except UnicodeDecodeError:", "            continue",
  "    return raw.decode('utf-8', errors='replace')", "def main():",
  "    sandbox = tempfile.mkdtemp(prefix='study-hub-assert-')", "    try:",
  "        target = os.path.join(sandbox, os.path.basename(DEMO))", "        shutil.copy2(DEMO, target)",
  "        proc = subprocess.run([sys.executable, target], capture_output=True, timeout=120)",
  "    finally:", "        shutil.rmtree(sandbox, ignore_errors=True)",
  "    return 0 if proc.returncode == 0 else 1", "if __name__ == '__main__':", "    sys.exit(main())",
].join("\n")

function makeStore() {
  const map = new Map()
  return {
    getSetting: function (key, fallback) { return map.has(key) ? map.get(key) : fallback },
    setSetting: function (key, value) { map.set(key, String(value)) },
  }
}

function fakeClient(reply) {
  const calls = []
  return {
    calls: calls, name: "fake", model: "fake-model",
    complete: async function (req) { calls.push(req); return typeof reply === "function" ? reply(req) : reply },
  }
}

function makeFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-svc-"))
  execFileSync("git", ["init"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: root, stdio: "ignore" })
  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "main.py"), "def begin():\n    return 1\n")
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC\n")
  fs.writeFileSync(path.join(cardDir, "NOTES.md"), "## 要点\n版本链\n")
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: root, stdio: "ignore" })
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(unitsDir, { recursive: true })
  return { root: root, unitsDir: unitsDir }
}

function mvccUnit() {
  return {
    id: "db:mvcc", track: "database", kind: "card", title: "MVCC（多版本并发控制）",
    statement: "# MVCC\n\n## 要点\n版本链\n", entry: {
      demo: "scaffold/grokking/mvcc/main.py", reference: "", graded: "script",
      script: "units/database/mvcc.assert.py", language: "python",
    }, asserts: [], topics: [], difficulty: "medium", source: "grokking", status: "todo",
    progress: "todo", reviewed: false, reviewed_at: "", subtracks: [], path: "scaffold/grokking/mvcc/",
  }
}

function makeService(fixture, client, store) {
  return createLlmService({
    client: client, store: store || makeStore(), provider: {}, env: {},
    learningRoot: fixture.root, unitsDir: fixture.unitsDir, stateFile: stateFilePath(fixture.root),
  })
}

test("generateAssert：写断言脚本，并建出 status: draft 的单元文件", async function () {
  const fixture = makeFixture()
  const service = makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT, topics: ["事务"] })))
  const result = await service.generateAssert({ unit: mvccUnit(), batchId: "b1" })
  assert.strictEqual(result.topics[0], "事务")
  const scriptText = fs.readFileSync(path.join(fixture.unitsDir, "database", "mvcc.assert.py"), "utf8")
  assert.ok(scriptText.indexOf("STUDY_HUB_DEMO") !== -1)
  const unitText = fs.readFileSync(path.join(fixture.unitsDir, "database", "mvcc.md"), "utf8")
  assert.ok(unitText.indexOf("status: draft") !== -1)
  assert.ok(unitText.indexOf("topics: [事务]") !== -1)
  assert.ok(unitText.indexOf("graded: script") !== -1)
})

test("generateAssert：提示词里带上 demo 源码与卡片说明", async function () {
  const fixture = makeFixture()
  const client = fakeClient(JSON.stringify({ assert_script: OK_SCRIPT }))
  await makeService(fixture, client).generateAssert({ unit: mvccUnit(), batchId: "b1" })
  assert.ok(client.calls[0].user.indexOf("def begin()") !== -1)
  assert.ok(client.calls[0].user.indexOf("# MVCC") !== -1)
  assert.ok(client.calls[0].user.indexOf("版本链") !== -1)
})

test("generateAssert：LLM 关掉时抛 503", async function () {
  const fixture = makeFixture()
  const service = makeService(fixture, { name: "null", model: "null", reason: "没有配置 API Key", complete: async function () { return "" } })
  await assert.rejects(function () { return service.generateAssert({ unit: mvccUnit(), batchId: "b1" }) }, function (err) { return err.status === 503 })
})

test("generateAssert：配额超了抛 429", async function () {
  const fixture = makeFixture()
  const store = makeStore()
  store.setSetting("llm.quota.session_limit", "0")
  const service = makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT })), store)
  await assert.rejects(function () { return service.generateAssert({ unit: mvccUnit(), batchId: "b1" }) }, function (err) { return err.status === 429 })
})

test("writeExplain：把讲解正文写进「## 讲解」小节", async function () {
  const fixture = makeFixture()
  const unit = mvccUnit()
  await makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT }))).generateAssert({ unit: unit, batchId: "b1" })
  const result = await makeService(fixture, fakeClient("# 讲解\n\nMVCC 用版本链让读不阻塞写。")).writeExplain({ unit: unit, batchId: "b1" })
  const text = fs.readFileSync(result.file, "utf8")
  assert.ok(result.explain.indexOf("版本链") !== -1)
  assert.ok(text.indexOf("MVCC 用版本链让读不阻塞写") !== -1)
  assert.ok(text.indexOf("status: draft") !== -1)
})

test("deepen：把深挖段落追加到讲解小节末尾", async function () {
  const fixture = makeFixture()
  const unit = mvccUnit()
  await makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT }))).generateAssert({ unit: unit, batchId: "b1" })
  const result = await makeService(fixture, fakeClient("失效场景有三个：写冲突、长事务、undo 空间不足。")).deepen({ unit: unit, batchId: "b1", question: "这个机制什么时候失效？" })
  const text = fs.readFileSync(result.file, "utf8")
  assert.ok(result.text.indexOf("失效场景") !== -1)
  assert.ok(text.indexOf("### 深挖") !== -1)
  assert.ok(text.indexOf("这个机制什么时候失效？") !== -1)
})

test("generateUnit：写单元文件 + 参考解法文件", async function () {
  const fixture = makeFixture()
  const unit = { id: "algo:llm-abc123", track: "algorithm", kind: "problem", title: "占位", statement: "", entry: { demo: "", reference: "", graded: "user", script: "", language: "python" }, asserts: [], topics: [], difficulty: "medium", source: "generated", status: "todo", progress: "todo", reviewed: false, reviewed_at: "", subtracks: [], path: "" }
  const result = await makeService(fixture, fakeClient(JSON.stringify({ title: "两数之和", statement: "给定数组与目标值。", difficulty: "easy", topics: ["哈希表"], cases: [{ input: "2 7 11 15", expect: "0 1" }], solution: "print('0 1')", solutionLang: "python" }))).generateUnit({ unit: unit, batchId: "b1" })
  const text = fs.readFileSync(result.file, "utf8")
  assert.ok(text.indexOf("kind: problem") !== -1)
  assert.ok(text.indexOf("status: draft") !== -1)
  assert.ok(text.indexOf("给定数组与目标值") !== -1)
  assert.ok(fs.existsSync(result.referenceFile))
  assert.ok(text.indexOf("reference: units/algorithm/llm-abc123.reference.py") !== -1)
})

test("算法轨不生成断言脚本：报 400", async function () {
  const fixture = makeFixture()
  const unit = mvccUnit(); unit.track = "algorithm"
  await assert.rejects(function () { return makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT }))).generateAssert({ unit: unit, batchId: "b1" }) }, function (err) { return err.status === 400 })
})

test("每次调用都记一条 trace", async function () {
  const fixture = makeFixture(); const store = makeStore(); const unit = mvccUnit()
  await makeService(fixture, fakeClient(JSON.stringify({ assert_script: OK_SCRIPT })), store).generateAssert({ unit: unit, batchId: "b1" })
  assert.ok(store.getSetting("llm.trace.db:mvcc", "").indexOf("generate-assert") !== -1)
})
