"use strict"
/**
 * supply-unit-file.test.js —— 单元文件读写 + 写回前查脏
 *
 * 三条纪律的回归闸门：字段顺序、只改目标行、写回前查脏。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const {
  FRONTMATTER_ORDER,
  renderUnitFile,
  readUnitFile,
  applyFrontmatterPatch,
  replaceSection,
  appendSectionNote,
  appendToSection,
  stateFilePath,
  markSelfWritten,
  isSelfWritten,
  checkWritable,
  patchUnitFile,
  writeFileChecked,
} = require("../core/supply/unit-file.js")

function makeRepo() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "supply-repo-"))
  execFileSync("git", ["init"], { cwd: dir, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: dir, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: dir, stdio: "ignore" })
  fs.writeFileSync(path.join(dir, "README.md"), "占位\n")
  execFileSync("git", ["add", "-A"], { cwd: dir, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: dir, stdio: "ignore" })
  return dir
}

const CARD_FIELDS = {
  id: "db:mvcc",
  track: "database",
  kind: "card",
  title: "MVCC（多版本并发控制）",
  difficulty: "medium",
  topics: ["事务", "并发"],
  source: "generated",
  language: "python",
  demo: "scaffold/grokking/mvcc/main.py",
  reference: "",
  graded: "script",
  script: "units/database/mvcc.assert.py",
  status: "draft",
  reviewed: false,
  reviewed_at: "",
  subtracks: [],
}

test("renderUnitFile 的 frontmatter 字段顺序与契约 §3.3 一致", function () {
  const text = renderUnitFile(CARD_FIELDS, { explain: "讲解正文", related: ["- 卡片目录：scaffold/grokking/mvcc"] })
  const keys = text.split(/^---$/m)[1].split(/\r?\n/).filter(function (line) { return line.trim() !== "" }).map(function (line) { return line.split(":")[0] })
  assert.deepStrictEqual(keys, FRONTMATTER_ORDER)
  assert.deepStrictEqual(FRONTMATTER_ORDER, [
    "id", "track", "kind", "title", "difficulty", "topics", "source", "language",
    "demo", "reference", "graded", "script", "status", "reviewed", "reviewed_at", "subtracks",
  ])
  assert.ok(text.indexOf("status: draft") !== -1)
  assert.ok(text.indexOf("topics: [事务, 并发]") !== -1)
  assert.ok(text.indexOf("## 讲解") !== -1)
  assert.ok(text.indexOf("## 关联") !== -1)
})

test("readUnitFile 读得出 frontmatter 与正文小节", function () {
  const repo = makeRepo()
  const file = path.join(repo, "mvcc.md")
  fs.writeFileSync(file, renderUnitFile(CARD_FIELDS, { explain: "讲解正文", related: ["- 卡片目录：x"] }))
  const result = readUnitFile(file)
  assert.strictEqual(result.fields.id, "db:mvcc")
  assert.strictEqual(result.fields.status, "draft")
  assert.deepStrictEqual(result.fields.topics, ["事务", "并发"])
  assert.ok(result.sections["讲解"].indexOf("讲解正文") !== -1)
})

test("applyFrontmatterPatch 只改目标行，其余逐字节不变", function () {
  const before = ["---", "id: db:mvcc", "track: database", "status: draft", "reviewed: false", "---", "", "## 讲解", "", "正文", ""].join("\n")
  const after = applyFrontmatterPatch(before, { status: "verified" })
  const lines = after.split("\n")
  assert.strictEqual(lines[3], "status: verified")
  assert.ok(after.indexOf("## 讲解") !== -1)
  assert.ok(after.indexOf("正文") !== -1)
})

test("applyFrontmatterPatch 不许把 CRLF 改成 LF", function () {
  const before = "---\r\nid: db:x\r\nstatus: draft\r\n---\r\n"
  assert.strictEqual(applyFrontmatterPatch(before, { status: "verified" }), "---\r\nid: db:x\r\nstatus: verified\r\n---\r\n")
})

test("applyFrontmatterPatch 能补一个不存在的键", function () {
  const after = applyFrontmatterPatch("---\nid: db:x\nstatus: draft\n---\n", { reviewed: true })
  assert.ok(after.indexOf("reviewed: true") !== -1)
  assert.ok(after.indexOf("status: draft") !== -1)
})

test("replaceSection 只替换目标小节", function () {
  const before = renderUnitFile(CARD_FIELDS, { explain: "旧正文", related: ["- 卡片目录：x"] })
  const after = replaceSection(before, "讲解", "新正文")
  assert.ok(after.indexOf("新正文") !== -1)
  assert.strictEqual(after.indexOf("旧正文"), -1)
  assert.ok(after.indexOf("status: draft") !== -1)
  assert.ok(after.indexOf("## 关联") !== -1)
})

test("appendSectionNote 在关联小节末尾追加一条", function () {
  const before = renderUnitFile(CARD_FIELDS, { explain: "正文", related: ["- 卡片目录：x"] })
  const after = appendSectionNote(before, "关联", "抽检意见：讲得清楚")
  assert.ok(after.indexOf("- 抽检意见：讲得清楚") !== -1)
  assert.ok(after.indexOf("- 卡片目录：x") !== -1)
})

test("appendToSection 在讲解小节末尾追加一段", function () {
  const before = renderUnitFile(CARD_FIELDS, { explain: "正文", related: ["- 卡片目录：x"] })
  const after = appendToSection(before, "讲解", "### 深挖 · 这个机制什么时候失效？\n\n失效场景有三个。")
  assert.ok(after.indexOf("### 深挖") !== -1)
  assert.ok(after.indexOf("失效场景有三个") !== -1)
  assert.ok(after.indexOf("## 关联") > after.indexOf("失效场景有三个"))
})

test("checkWritable：别人动过的文件阻塞写回", function () {
  const repo = makeRepo()
  fs.writeFileSync(path.join(repo, "别人改的.md"), "别人的内容\n")
  const check = checkWritable(repo, stateFilePath(repo))
  assert.strictEqual(check.ok, false)
  assert.ok(check.dirtyFiles.indexOf("别人改的.md") !== -1)
  assert.ok(check.reason.indexOf("别人改的.md") !== -1)
})

test("checkWritable：自己写过的未提交改动只告警不阻塞", function () {
  const repo = makeRepo()
  const file = path.join(repo, "units", "database", "mvcc.md")
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, "status: draft\n")
  const state = stateFilePath(repo)
  markSelfWritten(state, "units/database/mvcc.md", "status: draft\n")
  const check = checkWritable(repo, state)
  assert.strictEqual(check.ok, true)
  assert.deepStrictEqual(check.ownFiles, ["units/database/mvcc.md"])
  assert.strictEqual(isSelfWritten(state, repo, "units/database/mvcc.md"), true)
})

test("自己写过的文件被人改过之后，阻塞写回", function () {
  const repo = makeRepo()
  const file = path.join(repo, "units", "database", "mvcc.md")
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, "status: draft\n")
  const state = stateFilePath(repo)
  markSelfWritten(state, "units/database/mvcc.md", "status: draft\n")
  fs.writeFileSync(file, "status: draft\n\n被人加了一段\n")
  assert.strictEqual(isSelfWritten(state, repo, "units/database/mvcc.md"), false)
  assert.strictEqual(checkWritable(repo, state).ok, false)
})

test("patchUnitFile 只返回 diff，不自动提交", function () {
  const repo = makeRepo()
  const file = path.join(repo, "units", "database", "mvcc.md")
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, renderUnitFile(CARD_FIELDS, { explain: "正文", related: [] }))
  const state = stateFilePath(repo)
  markSelfWritten(state, "units/database/mvcc.md", fs.readFileSync(file, "utf8"))
  const result = patchUnitFile(file, { status: "verified" }, { repoRoot: repo, stateFile: state })
  assert.strictEqual(result.ok, true, result.reason)
  assert.strictEqual(result.changed, true)
  assert.ok(result.diff.indexOf("status: verified") !== -1)
  assert.ok(execFileSync("git", ["rev-parse", "HEAD"], { cwd: repo, encoding: "utf8" }).trim().length > 0)
})

test("writeFileChecked 写新文件不查脏，写已存在且被动过的文件要拦", function () {
  const repo = makeRepo()
  const state = stateFilePath(repo)
  const fresh = path.join(repo, "units", "database", "new.md")
  const created = writeFileChecked(fresh, "内容\n", { repoRoot: repo, stateFile: state })
  assert.strictEqual(created.ok, true, created.reason)
  assert.strictEqual(created.changed, true)
  assert.ok(fs.existsSync(fresh))
})
