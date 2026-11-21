"use strict"
/**
 * supply-algorithm-fill.test.js —— 生成侧纯函数：判缺口、渲染三件套、只改 status 一行
 *
 * 全程不联网、不写真实 learning 仓：题库与产出目录都在临时目录里。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const fill = require("../core/supply/algorithm-fill.js")
const { parseUnitFile } = require("../core/unit/loader-c.js")

/** 造一个临时目录，测试结束删掉 */
function tmpDir(tag) {
  return fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-fill-" + tag + "-"))
}

/** 一份空的题库骨架（形态 A：题面空、用例空） */
function bankSkeleton(id, extra) {
  const lines = []
  lines.push("---")
  lines.push("id: " + id)
  lines.push("source: leetcode")
  lines.push("source_id: 7")
  lines.push("title: 整数反转")
  lines.push("difficulty: medium")
  lines.push("topics: [数学]")
  lines.push("tracks: [fundamentals, interview]")
  lines.push("status: todo")
  lines.push("needs_fill: true")
  lines.push("url: https://leetcode.cn/problems/reverse-integer/")
  lines.push("created: 2026-01-05")
  lines.push("updated: 2026-01-05")
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
  if (extra) lines.push(extra)
  return lines.join("\n")
}

test("scanBank 认出四种形态", function () {
  const root = tmpDir("scan")
  try {
    const bank = path.join(root, "bank", "fundamentals")
    fs.mkdirSync(bank, { recursive: true })
    // A：题面空、用例空
    fs.writeFileSync(path.join(bank, "lc-00001.md"), bankSkeleton("lc-00001"), "utf8")
    // B：题面非空、用例空
    fs.writeFileSync(
      path.join(bank, "lc-00002.md"),
      bankSkeleton("lc-00002").replace("## 题面\n\n", "## 题面\n\n给你一个整数。\n\n"),
      "utf8"
    )
    // C：题面非空、用例非空、参考解法空
    fs.writeFileSync(
      path.join(bank, "lc-00003.md"),
      bankSkeleton("lc-00003")
        .replace("## 题面\n\n", "## 题面\n\n给你一个整数。\n\n")
        .replace("|---|---|", "|---|---|\n| 123 | 321 |"),
      "utf8"
    )
    // ready：三样齐
    fs.writeFileSync(
      path.join(bank, "lc-00004.md"),
      bankSkeleton("lc-00004")
        .replace("## 题面\n\n", "## 题面\n\n给你一个整数。\n\n")
        .replace("|---|---|", "|---|---|\n| 123 | 321 |")
        .replace("## 参考解法\n", "## 参考解法\n\n```cpp\nint main(){return 0;}\n```\n"),
      "utf8"
    )

    const out = fill.scanBank({ learningRoot: root, localBank: null })
    const byStem = {}
    out.items.forEach(function (item) { byStem[item.stem] = item })

    assert.equal(byStem["lc-00001"].shape, "A", "题面空的应是形态 A")
    assert.deepEqual(byStem["lc-00001"].missing, ["statement", "cases", "solution"])
    assert.equal(byStem["lc-00002"].shape, "B", "题面有、用例空的应是形态 B")
    assert.deepEqual(byStem["lc-00002"].missing, ["cases", "solution"])
    assert.equal(byStem["lc-00003"].shape, "C", "只缺参考解法的应是形态 C")
    assert.deepEqual(byStem["lc-00003"].missing, ["solution"])
    assert.equal(byStem["lc-00004"].shape, "ready", "三样齐的应是 ready")
    assert.deepEqual(byStem["lc-00004"].missing, [])

    assert.equal(out.stats.byShape.A, 1)
    assert.equal(out.stats.byShape.ready, 1)
    assert.equal(out.stats.bySource["leetcode"], 4)
    assert.equal(out.stats.bySubtrack["fundamentals"], 4)
    assert.equal(out.stats.missing.solution, 3)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("scanBank 保留题面与用例的原文（形态 C 原样带着走）", function () {
  const root = tmpDir("keep")
  try {
    const bank = path.join(root, "bank", "advanced")
    fs.mkdirSync(bank, { recursive: true })
    fs.writeFileSync(
      path.join(bank, "cf-1-c.md"),
      bankSkeleton("cf-1-c")
        .replace("## 题面\n\n", "## 题面\n\n古马戏团\n\n**输入**\n\n三行坐标。\n\n")
        .replace("|---|---|", "|---|---|\n| 0.0 0.0<br>1.0 1.0<br>0.0 1.0 | 1.0 |"),
      "utf8"
    )
    const out = fill.scanBank({ learningRoot: root, localBank: null })
    const item = out.items[0]
    assert.equal(item.shape, "C")
    assert.ok(item.statement.indexOf("古马戏团") !== -1, "题面原文必须保留")
    assert.equal(item.cases.length, 1)
    assert.equal(item.cases[0].input, "0.0 0.0\n1.0 1.0\n0.0 1.0", "<br> 应被解码回换行")
    assert.equal(item.cases[0].expect, "1.0")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("renderUnitMarkdown 产出契约 §3.3.1 规定的小节结构", function () {
  const text = fill.renderUnitMarkdown({
    stem: "lc-00007",
    title: "整数反转",
    difficulty: "medium",
    topics: ["数学"],
    subtracks: ["fundamentals", "interview"],
    source: "leetcode",
    sourceId: "7",
    url: "https://leetcode.cn/problems/reverse-integer/",
    statement: "给你一个整数。\n\n## 输入\n\n一个数。",
    cases: [{ input: "123", expect: "321" }],
    solution: "int main(){return 0;}",
    negative: "int main(){return 1;}",
    status: "draft",
  })
  assert.ok(text.indexOf("id: algo:lc-00007") !== -1, text)
  assert.ok(text.indexOf("track: algorithm") !== -1)
  assert.ok(text.indexOf("kind: problem") !== -1)
  assert.ok(text.indexOf("graded: user") !== -1)
  assert.ok(text.indexOf("reference: units/algorithm/lc-00007.reference.cpp") !== -1)
  assert.ok(text.indexOf("negative: units/algorithm/lc-00007.negative.cpp") !== -1)
  assert.ok(text.indexOf("status: draft") !== -1)
  assert.ok(text.indexOf("reviewed: false") !== -1)
  assert.ok(text.indexOf('reviewed_at: ""') !== -1, 'reviewed_at 要写成显式空串，不能写成 key:')
  assert.ok(text.indexOf("## 题面") !== -1)
  assert.ok(text.indexOf("## 测试用例") !== -1)
  assert.ok(text.indexOf("## 参考解法") !== -1)
  assert.ok(text.indexOf("## 关联") !== -1)
  // 题面里的二级标题必须降级，否则 loader C 切小节时会把题面截断
  assert.ok(text.indexOf("### 输入") !== -1, "题面里的 ## 要降成 ###")
  assert.ok(text.indexOf("\n## 输入\n") === -1, "题面里不能残留二级标题")
  // 负例的内容绝不进 md，只留路径
  assert.ok(text.indexOf("return 1;") === -1, "负例代码不能进单元 md")
  assert.ok(text.indexOf("| 123 | 321 |") !== -1, "用例要渲染成表格行")
})

test("canonicalCode 统一换行并去掉末尾空行", function () {
  assert.equal(fill.canonicalCode("a\r\nb\r\n"), "a\nb")
  assert.equal(fill.canonicalCode("a\n\n\n"), "a")
  assert.equal(fill.canonicalCode(""), "")
})

test("writeTriplet 落三件套，且 md 代码块与 .reference.cpp 逐字节一致", function () {
  const root = tmpDir("write")
  try {
    const unitsDir = path.join(root, "units", "algorithm")
    const solution = "int main(){\r\n  return 0;\r\n}\r\n"
    const out = fill.writeTriplet({
      unitsDir: unitsDir,
      stem: "lc-00007",
      draft: {
        stem: "lc-00007",
        title: "整数反转",
        difficulty: "medium",
        topics: ["数学"],
        subtracks: ["fundamentals"],
        source: "leetcode",
        sourceId: "7",
        url: "https://leetcode.cn/problems/reverse-integer/",
        statement: "给你一个整数。",
        cases: [{ input: "123", expect: "321" }],
        solution: solution,
        negative: "int main(){return 1;}",
        status: "draft",
      },
    })
    assert.equal(out.files.length, 3)
    const md = fs.readFileSync(path.join(unitsDir, "lc-00007.md"), "utf8")
    const ref = fs.readFileSync(path.join(unitsDir, "lc-00007.reference.cpp"), "utf8")
    const neg = fs.readFileSync(path.join(unitsDir, "lc-00007.negative.cpp"), "utf8")
    assert.equal(fill.extractSolutionBlock(md), fill.canonicalCode(ref), "md 代码块与 .reference.cpp 必须逐字节一致")
    assert.equal(ref, "int main(){\n  return 0;\n}\n", "cpp 文件用 LF，末尾一个换行")
    assert.ok(neg.indexOf("return 1;") !== -1)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("patchStatusLine 只改 status 那一行，其余逐字节不变", function () {
  const original = "---\nid: algo:lc-00007\nstatus: draft\nreviewed: false\n---\n\n## 题面\n\n正文\n"
  const patched = fill.patchStatusLine(original, "verified")
  assert.ok(patched.indexOf("status: verified") !== -1, patched)
  assert.ok(patched.indexOf("status: draft") === -1)
  assert.equal(
    patched,
    original.replace("status: draft", "status: verified"),
    "除了 status 那一行，其余必须逐字节不变"
  )
})

test("patchStatusLine 保留 CRLF 行尾", function () {
  const original = "---\r\nid: algo:x\r\nstatus: draft\r\n---\r\n正文\r\n"
  const patched = fill.patchStatusLine(original, "rejected")
  assert.ok(patched.indexOf("status: rejected\r\n") !== -1, JSON.stringify(patched))
  assert.ok(patched.indexOf("\n\n") === -1, "不能把 CRLF 换成 LF")
})

test("patchStatusLine 没有 status 行时补一行", function () {
  const original = "---\nid: algo:x\n---\n正文\n"
  const patched = fill.patchStatusLine(original, "verified")
  assert.ok(patched.indexOf("status: verified") !== -1, patched)
  assert.ok(patched.indexOf("正文") !== -1)
})

test("checkLearningClean：有未提交改动就拦下来", function () {
  const res = fill.checkLearningClean({
    learningRoot: "/tmp/whatever",
    gitRunner: function () {
      return { ok: true, stdout: "?? notes/x.md\0 M units/algorithm/a.md\0", stderr: "" }
    },
  })
  assert.equal(res.ok, false)
  assert.equal(res.entries.length, 2)
  assert.ok(res.reason.indexOf("未提交改动") !== -1, res.reason)
})

test("checkLearningClean：干净时放行", function () {
  const res = fill.checkLearningClean({
    learningRoot: "/tmp/whatever",
    gitRunner: function () { return { ok: true, stdout: "", stderr: "" } },
  })
  assert.equal(res.ok, true)
  assert.equal(res.entries.length, 0)
})

/* ---------- loader C 读算法轨单元（契约 §3.3.1，P0 已实现，这里钉住它的行为） ---------- */

test("loader C 读得懂 renderUnitMarkdown 产出的算法单元", function () {
  const md = fill.renderUnitMarkdown({
    stem: "lc-00007",
    title: "整数反转",
    difficulty: "hard",
    topics: ["数学", "字符串"],
    subtracks: ["fundamentals"],
    source: "leetcode",
    sourceId: "7",
    url: "https://leetcode.cn/problems/reverse-integer/",
    statement: "给你一个整数，把它反转。\n\n## 输入格式\n\n一个整数。",
    cases: [
      { input: "123", expect: "321" },
      { input: "-120", expect: "-21" },
    ],
    solution: "int main(){return 0;}",
    negative: "int main(){return 1;}",
    status: "draft",
  })
    // 路径必须落在 units/algorithm/ 下（ClearPath 的目录与 track 校验）
  const parsed = parseUnitFile(md, { path: "algorithm/lc-00007.md" })
  assert.equal(parsed.warnings.length, 0, "不该有警告：" + parsed.warnings.join(" / "))
  const unit = parsed.unit
  assert.ok(unit, "要产出单元")
  assert.equal(unit.id, "algo:lc-00007")
  assert.equal(unit.track, "algorithm")
  assert.equal(unit.kind, "problem")
  assert.equal(unit.difficulty, "hard")
  assert.equal(unit.status, "draft")
  // 题面：读的是 `## 题面`，不是 `## 讲解`
  assert.ok(unit.statement.indexOf("把它反转") !== -1, unit.statement)
  // 题面里降级过的三级标题原样留在正文里
  assert.ok(unit.statement.indexOf("### 输入格式") !== -1, unit.statement)
  // 测试用例表格 → output 断言，cmp 固定 exact
  assert.equal(unit.asserts.length, 2)
  assert.deepEqual(unit.asserts[0], { type: "output", input: "123", expect: "321", cmp: "exact" })
  assert.deepEqual(unit.asserts[1], { type: "output", input: "-120", expect: "-21", cmp: "exact" })
  // 算法轨的入口：graded user / script 空 / language cpp / reference 指向 cpp 文件
  assert.equal(unit.entry.graded, "user")
  assert.equal(unit.entry.script, "")
  assert.equal(unit.entry.language, "cpp")
  assert.equal(unit.entry.reference, "units/algorithm/lc-00007.reference.cpp")
  // 参考解法的代码块不进 Unit（给人看的镜像）
  assert.equal(unit.entry.demo, "")
  assert.ok(unit.topics.indexOf("数学") !== -1, JSON.stringify(unit.topics))
  assert.ok(unit.subtracks.indexOf("fundamentals") !== -1, JSON.stringify(unit.subtracks))
  assert.equal(unit.path, "units/algorithm/lc-00007.md")
  assert.ok(unit.related.indexOf("原题链接") !== -1, unit.related)
})

test("loader C 认算法轨的 negative 字段（不报非法）", function () {
  const md = fill.renderUnitMarkdown({
    stem: "cf-1-c",
    title: "古马戏团",
    statement: "题面。",
    cases: [{ input: "1", expect: "2" }],
    solution: "int main(){return 0;}",
    negative: "int main(){return 1;}",
    source: "codeforces",
    sourceId: "1",
    status: "draft",
  })
  const parsed = parseUnitFile(md, { path: "algorithm/cf-1-c.md" })
  assert.equal(parsed.warnings.length, 0, "negative / source_id / url 三个选填字段不该触发警告")
  assert.equal(parsed.unit.source, "codeforces")
})
