"use strict"
/**
 * unit-loader-a.test.js —— core/unit/loader-a.js
 *
 * loader A 读算法题 md（learning/bank 下的 md 与工程仓 bank 下的 md），产出 Unit。
 * 契约 §3.1 冻结了行为；这里把每一条都钉成断言。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const loaderA = require("../core/unit/loader-a.js")

/** 造一份算法题 md。 */
function makeMd(front, body) {
  return "---\n" + front + "\n---\n\n" + body
}

const FULL_FRONT = [
  "id: lc-00007",
  "source: leetcode",
  "title: 反转整数",
  "difficulty: medium",
  "topics: [数学, 字符串]",
  "tracks: [interview]",
  "status: in-progress",
].join("\n")

const FULL_BODY = [
  "## 题面",
  "",
  "给你一个整数 x，返回它反转后的整数。",
  "",
  "## 测试用例",
  "",
  "| input | expect |",
  "|---|---|",
  "| 123 | 321 |",
  "| -123 | -321 |",
  "",
  "## 参考解法",
  "",
  "```cpp",
  "int reverse(int x) { return 0; }",
  "```",
  "",
].join("\n")

const FULL = makeMd(FULL_FRONT, FULL_BODY)

test("id 带上 algo 前缀，track 恒为 algorithm", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.ok(res.unit)
  assert.equal(res.unit.id, "algo:lc-00007")
  assert.equal(res.unit.track, "algorithm")
  assert.equal(res.unit.kind, "problem")
})

test("id 已带前缀时不重复加前缀", function () {
  const front = FULL_FRONT.replace("id: lc-00007", "id: algo:lc-00007")
  const res = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), {
    relPath: "bank/interview/lc-00007.md",
  })
  assert.ok(res.unit)
  assert.equal(res.unit.id, "algo:lc-00007")
})

test("题面与用例齐备 → verified", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.status, "verified")
})

test("题面为空 → todo", function () {
  const md = makeMd(FULL_FRONT, ["## 题面", "", "", "## 测试用例", "", "| input | expect |", "|---|---|", "| 1 | 2 |", ""].join("\n"))
  const res = loaderA.parseUnitMarkdown(md, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.status, "todo")
})

test("题面有、用例为空 → draft", function () {
  const md = makeMd(FULL_FRONT, ["## 题面", "", "有题面。", ""].join("\n"))
  const res = loaderA.parseUnitMarkdown(md, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.status, "draft")
})

test("needs_fill 标志不参与状态判定", function () {
  const front = FULL_FRONT + "\nneeds_fill: true"
  const md = makeMd(front, FULL_BODY)
  const res = loaderA.parseUnitMarkdown(md, { relPath: "bank/interview/lc-00007.md" })
  // 题面与用例都在 → verified，哪怕 needs_fill 还是 true（存量标志已过时）
  assert.equal(res.unit.status, "verified")
})

test("用例转成 output 断言，cmp 恒为 exact", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.asserts.length, 2)
  assert.deepEqual(res.unit.asserts[0], { type: "output", input: "123", expect: "321", cmp: "exact" })
  assert.equal(res.unit.asserts[1].input, "-123")
})

test("带 tol 的用例：丢弃 tol 并留中文警告", function () {
  const body = [
    "## 题面",
    "",
    "题面",
    "",
    "## 测试用例",
    "",
    "| input | expect | tol |",
    "|---|---|---|",
    "| 1.5 | 1.5 | tol=1e-6 |",
    "",
  ].join("\n")
  const res = loaderA.parseUnitMarkdown(makeMd(FULL_FRONT, body), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.asserts.length, 1)
  assert.equal(res.unit.asserts[0].cmp, "exact")
  assert.equal("tol" in res.unit.asserts[0], false)
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("用例 1 带 tol 参数，v1 按精确比对处理") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("source 不在白名单 → unknown", function () {
  const front = FULL_FRONT.replace("source: leetcode", "source: 某个没见过的来源")
  const res = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.source, "unknown")
})

test("source 在白名单里原样保留", function () {
  const front = FULL_FRONT.replace("source: leetcode", "source: codeforces")
  const res = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.source, "codeforces")
})

test("difficulty 非法 → medium", function () {
  const front = FULL_FRONT.replace("difficulty: medium", "difficulty: 超难")
  const res = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.difficulty, "medium")
})

test("progress 取自 frontmatter 的 status；非法值 → todo", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.progress, "in-progress")
  const front = FULL_FRONT.replace("status: in-progress", "status: 瞎写的")
  const res2 = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res2.unit.progress, "todo")
})

test("topics 与 subtracks 各归其位", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.deepEqual(res.unit.topics, ["数学", "字符串"])
  assert.deepEqual(res.unit.subtracks, ["interview"])
})

test("entry 是 user + cpp，script 恒空", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.deepEqual(res.unit.entry, {
    demo: "",
    reference: "",
    graded: "user",
    script: "",
    language: "cpp",
  })
})

test("path 取 opts.relPath", function () {
  const a = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(a.unit.path, "bank/interview/lc-00007.md")
})

test("没给 relPath 时不产出单元，只给一条中文警告（G42）", function () {
  const b = loaderA.parseUnitMarkdown(FULL, {})
  assert.equal(b.unit, null)
  assert.equal(b.warnings.length, 1)
  assert.ok(b.warnings[0].indexOf("relPath") !== -1, b.warnings[0])
})

test("算法题不参与人工抽检", function () {
  const res = loaderA.parseUnitMarkdown(FULL, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit.reviewed, false)
  assert.equal(res.unit.reviewed_at, "")
})

test("解析不出 id → unit 为 null 且有中文警告", function () {
  const md = makeMd("title: 没 id 的题", FULL_BODY)
  const res = loaderA.parseUnitMarkdown(md, { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("算法题解析不出 id，跳过") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("id 非法（大小写不对）→ 跳过这一份，不抛异常", function () {
  // 契约 §3.1「unit 为 null 当且仅当解析不出 id」+ §3.4「任何单个文件解析失败只进 warnings，不中断整体加载」
  const front = FULL_FRONT.replace("id: lc-00007", "id: LC-00007")
  const res = loaderA.parseUnitMarkdown(makeMd(front, FULL_BODY), { relPath: "bank/interview/lc-00007.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("LC-00007") !== -1 && w.indexOf("解析失败") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})
