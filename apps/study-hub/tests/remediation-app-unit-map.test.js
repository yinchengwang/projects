"use strict"
const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")
const { createApp } = require("../server/app.js")

test("真实 app 的单元运行路由拿到 id→Unit 映射，而非数组", async function () {
  const root = fs.mkdtempSync(path.join(require("node:os").tmpdir(), "study-hub-reload-unit-map-"))
  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC\n\n测试说明\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")
  fs.writeFileSync(path.join(unitsDir, "mvcc.assert.py"), "print('ok')\n", "utf8")
  const unitFile = [
    "---",
    "id: db:mvcc",
    "track: database",
    "kind: card",
    "title: MVCC",
    "difficulty: medium",
    "topics: []",
    "source: grokking",
    "demo: scaffold/grokking/mvcc/main.py",
    "script: units/database/mvcc.assert.py",
    "graded: script",
    "status: verified",
    "reviewed: false",
    'reviewed_at: ""',
    "---",
    "",
    "## 讲解",
    "",
    "测试讲解正文。",
    "",
  ].join("\n")
  fs.writeFileSync(path.join(unitsDir, "mvcc.md"), unitFile, "utf8")
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    localBank: null,
    env: { STUDY_HUB_LEARNING_ROOT: root },
    llmClient: null,
  })
  const port = await app.listen(0)
  try {
    const unit = await fetch("http://127.0.0.1:" + port + "/api/units/db%3Amvcc")
    assert.strictEqual(unit.status, 200, await unit.text())
    const completed = await fetch("http://127.0.0.1:" + port + "/api/units/db%3Amvcc/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: "{}",
    })
    assert.strictEqual(completed.status, 200, await completed.text())
    const deepened = await fetch("http://127.0.0.1:" + port + "/api/units/db%3Amvcc/deepen", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ question: "这怎么工作？" }),
    })
    // llmClient=null 表示离线，deepen 有服务端错误路径（503），
    // 但如果 pickUnit 拿不到单元，返回的是 404。这里钉住应先过 ID 查找。
    assert.strictEqual(deepened.status, 503)
    const body = await deepened.json()
    assert.ok(String(body.error || "").indexOf("LLM 未启用") !== -1, JSON.stringify(body))
  } finally {
    await app.close()
    fs.rmSync(root, { recursive: true, force: true })
  }
})
