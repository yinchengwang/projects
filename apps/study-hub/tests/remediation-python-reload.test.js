"use strict"
const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")
const { tmpDir } = require("./helpers/tmp.js")
const { createApp } = require("../server/app.js")

test("切换 Python 后 loader 的 NumPy 警告随同一份单元映射更新", async function () {
  const root = tmpDir("study-hub-python-reload-")
  const cardDir = path.join(root, "scaffold", "grokking", "cnn")
  const unitsDir = path.join(root, "units", "ai")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# 神经网络\n\n测试卡\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")
  fs.writeFileSync(path.join(unitsDir, "cnn.assert.py"), "print('ok')\n", "utf8")
  fs.writeFileSync(path.join(unitsDir, "cnn.md"), [
    "---",
    "id: ai:cnn",
    "track: ai",
    "kind: card",
    "title: CNN",
    "difficulty: medium",
    "topics: []",
    "source: grokking",
    "demo: scaffold/grokking/cnn/main.py",
    "script: units/ai/cnn.assert.py",
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
  ].join("\n"), "utf8")

  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    localBank: null,
    env: {},
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    const before = await fetch(base + "/api/units/ai%3Acnn")
    assert.strictEqual(before.status, 200)

    const changed = await fetch(base + "/api/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ python_cmd: "py -3" }),
    })
    assert.strictEqual(changed.status, 200)
    const settings = await changed.json()
    assert.strictEqual(settings.env.python.cmd, "py")
    assert.strictEqual(settings.env.numpy.ok, true)
    assert.strictEqual(settings.env.numpy.version, "2.5.5")

    // reloadUnits 必须原地更新 unit-run 与 session 两组路由实际持有的 id→Unit 映射。
    // 若 units 仍是数组，或只更新了 supplyUnitsById，这个请求会 404。
    const completed = await fetch(base + "/api/units/ai%3Acnn/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: "{}",
    })
    assert.strictEqual(completed.status, 200)
    const completedBody = await completed.json()
    assert.strictEqual(completedBody.unitId, "ai:cnn")
    const env = await fetch(base + "/api/env")
    assert.strictEqual(env.status, 200)
    const envBody = await env.json()
    assert.strictEqual(envBody.numpy.available, true)
  } finally {
    await app.close()
    fs.rmSync(root, { recursive: true, force: true })
  }
})
