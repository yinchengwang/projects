"use strict"
/**
 * supply-algorithm-audit.test.js —— 交付前审计：把 Task 9/10 验收自查表里机器能查的部分钉住
 *
 * 纯文件检查，不联网、不编译、不写盘：三件套成对、status、负例不外泄、
 * md 代码块与 cpp 逐字节一致、密钥不外泄、plan.json 不混进产物。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const audit = require("../core/supply/audit.js")
const fill = require("../core/supply/algorithm-fill.js")

const REF = [
  "#include <iostream>",
  "int main() {",
  '  std::cout << "ok" << "\\n";',
  "  return 0;",
  "}",
].join("\n")

const REF_OTHER = [
  "#include <iostream>",
  "int main() {",
  '  std::cout << "other" << "\\n";',
  "  return 0;",
  "}",
].join("\n")

const NEG = [
  "#include <iostream>",
  "int main() {",
  '  std::cout << "WRONG_LINE_MARKER" << "\\n";',
  "  return 0;",
  "}",
].join("\n")

function tmpUnits(tag) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-audit-" + tag + "-"))
  return { root: root, unitsDir: path.join(root, "units", "algorithm") }
}

function writeUnit(unitsDir, stem, opts) {
  const o = opts || {}
  fill.writeTriplet({
    unitsDir: unitsDir,
    stem: stem,
    draft: {
      stem: stem,
      title: "审计用单元",
      difficulty: "easy",
      topics: ["字符串"],
      subtracks: ["fundamentals"],
      source: "leetcode",
      unitSource: "leetcode",
      sourceId: "7",
      statement: "给你一个字符串。",
      cases: [{ input: "1", expect: "1" }],
      solution: o.solution === undefined ? REF : o.solution,
      negative: o.negative === undefined ? NEG : o.negative,
      status: o.status === undefined ? "verified" : o.status,
    },
  })
}

test("三件套齐全 + status verified → 审计通过", function () {
  const t = tmpUnits("ok")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    writeUnit(t.unitsDir, "lc-00002")
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, true, JSON.stringify(res.problems))
    assert.equal(res.counts.md, 2)
    assert.equal(res.counts.reference, 2)
    assert.equal(res.counts.negative, 2)
    assert.equal(res.counts.verified, 2)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("缺負例文件 → 报出来（三件套不成对）", function () {
  const t = tmpUnits("orphan")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    fs.rmSync(path.join(t.unitsDir, "lc-00001.negative.cpp"))
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) {
      return p.kind === "missing-file" && p.stem === "lc-00001"
    })
    assert.ok(hit, JSON.stringify(res.problems))
    assert.ok(hit.detail.indexOf("negative") !== -1, hit.detail)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("status 还是 draft → 报出来（没经过真实校验）", function () {
  const t = tmpUnits("draft")
  try {
    writeUnit(t.unitsDir, "lc-00001", { status: "draft" })
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) { return p.kind === "status" })
    assert.ok(hit, JSON.stringify(res.problems))
    assert.ok(hit.detail.indexOf("draft") !== -1, hit.detail)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("md 混进负例代码 → 报出来（P5-5）", function () {
  const t = tmpUnits("leak")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    const file = path.join(t.unitsDir, "lc-00001.md")
    // 整行放进 md：判据是「负例独有的整行出现在 md 里」
    const leakLine = '  std::cout << "WRONG_LINE_MARKER" << "\\n";'
    fs.writeFileSync(file, fs.readFileSync(file, "utf8").replace("## 关联", "## 关联\n\n" + leakLine), "utf8")
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) { return p.kind === "negative-leak" })
    assert.ok(hit, JSON.stringify(res.problems))
    assert.ok(hit.detail.indexOf("WRONG_LINE_MARKER") !== -1, hit.detail)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("md 代码块与 .reference.cpp 不一致 → 报出来（P5-6）", function () {
  const t = tmpUnits("drift")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    fs.writeFileSync(path.join(t.unitsDir, "lc-00001.reference.cpp"), REF_OTHER, "utf8")
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) { return p.kind === "reference-drift" })
    assert.ok(hit, JSON.stringify(res.problems))
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("plan.json 混在产物目录里 → 单独记为「清单不是产物」", function () {
  const t = tmpUnits("planjson")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    fs.writeFileSync(path.join(t.unitsDir, "plan.json"), '{"track":"algorithm"}', "utf8")
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) { return p.kind === "plan-json" })
    assert.ok(hit, JSON.stringify(res.problems))
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("密钥字符串出现在单元文件里 → 报出来（G9）", function () {
  const t = tmpUnits("secret")
  try {
    writeUnit(t.unitsDir, "lc-00001")
    const file = path.join(t.unitsDir, "lc-00001.md")
    fs.writeFileSync(file, fs.readFileSync(file, "utf8").replace("## 关联", "## 关联\n\nARK_API_KEY=sk-abcdef123456"), "utf8")
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, false)
    const hit = res.problems.find(function (p) { return p.kind === "secret" })
    assert.ok(hit, JSON.stringify(res.problems))
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("空目录 → 通过但计数为 0（还没跑全量时的正常状态）", function () {
  const t = tmpUnits("empty")
  try {
    fs.mkdirSync(t.unitsDir, { recursive: true })
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    assert.equal(res.ok, true, JSON.stringify(res.problems))
    assert.equal(res.counts.md, 0)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})

test("目录不存在 → 不抛错，计数为 0 并给一句提醒", function () {
  const res = audit.auditAlgorithmUnits({ unitsDir: "/no/such/dir", learningRoot: "/tmp" })
  assert.equal(res.ok, true)
  assert.equal(res.counts.md, 0)
  assert.ok(res.notes.length > 0, JSON.stringify(res.notes))
})

test("renderAuditReport 是纯函数：中文、含关键数字", function () {
  const t = tmpUnits("render")
  try {
    writeUnit(t.unitsDir, "lc-00001", { status: "draft" })
    const res = audit.auditAlgorithmUnits({ unitsDir: t.unitsDir, learningRoot: t.root })
    const text = audit.renderAuditReport(res)
    assert.ok(text.indexOf("算法轨交付审计") !== -1, text)
    assert.ok(text.indexOf("md 1") !== -1, text)
    assert.ok(text.indexOf("lc-00001") !== -1, text)
  } finally {
    fs.rmSync(t.root, { recursive: true, force: true })
  }
})
