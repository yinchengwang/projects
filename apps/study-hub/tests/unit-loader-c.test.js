"use strict"
/**
 * unit-loader-c.test.js —— core/unit/loader-c.js
 *
 * loader C 读 learning/units/<track>/*.md，产出 Unit。契约 §3.3 冻结了行为与校验文案。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const loaderC = require("../core/unit/loader-c.js")

/** 造一份单元文件。 */
function makeUnitFile(front, body) {
  return "---\n" + front + "\n---\n\n" + (body || "")
}

const GOOD_FRONT = [
  "id: db:mvcc",
  "track: database",
  "kind: card",
  "title: MVCC：版本链与 ReadView",
  "difficulty: medium",
  "topics: [事务, 并发]",
  "source: grokking",
  "language: python",
  "demo: scaffold/grokking/mvcc/main.py",
  "graded: script",
  "script: units/database/mvcc.assert.py",
  "status: verified",
  "reviewed: true",
  "reviewed_at: 2026-11-20",
].join("\n")

const GOOD_BODY = [
  "## 讲解",
  "",
  "MVCC 是多版本并发控制。",
  "",
  "## 关联",
  "",
  "- 轮次：R37 Grokking 数据库",
  "- 上游单元：`db:transaction`",
  "",
].join("\n")

test("正常单元解析成 Unit", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "database/mvcc.md" })
  assert.ok(res.unit)
  assert.equal(res.unit.id, "db:mvcc")
  assert.equal(res.unit.track, "database")
  assert.equal(res.unit.kind, "card")
  assert.equal(res.unit.title, "MVCC：版本链与 ReadView")
  assert.equal(res.unit.status, "verified")
  assert.equal(res.unit.difficulty, "medium")
  assert.equal(res.unit.source, "grokking")
  assert.deepEqual(res.unit.topics, ["事务", "并发"])
})

test("入口字段照 frontmatter 走", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "database/mvcc.md" })
  assert.deepEqual(res.unit.entry, {
    demo: "scaffold/grokking/mvcc/main.py",
    reference: "",
    graded: "script",
    script: "units/database/mvcc.assert.py",
    language: "python",
  })
})

test("reviewed / reviewed_at 读成布尔与字符串", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit.reviewed, true)
  assert.equal(res.unit.reviewed_at, "2026-11-20")
})

test("讲解进 statement，关联进 related", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit.statement, "MVCC 是多版本并发控制。")
  assert.ok(res.unit.related.indexOf("R37 Grokking 数据库") !== -1)
})

test("path 取 opts.path", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit.path, "units/database/mvcc.md")
})

test("id 非法 → unit 为 null 且警告文案固定", function () {
  const front = GOOD_FRONT.replace("id: db:mvcc", "id: 非法 ID")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(res.warnings.indexOf("单元 id 非法：非法 ID") !== -1, JSON.stringify(res.warnings))
})

test("id 前缀与 track 不一致 → unit 为 null", function () {
  const front = GOOD_FRONT.replace("id: db:mvcc", "id: algo:mvcc")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.indexOf("单元 id 前缀与 track 不一致：algo:mvcc / database") !== -1,
    JSON.stringify(res.warnings)
  )
})

test("kind 非法 → unit 为 null", function () {
  const front = GOOD_FRONT.replace("kind: card", "kind: 卡片")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(res.warnings.indexOf("单元 db:mvcc 的 kind 非法：卡片") !== -1, JSON.stringify(res.warnings))
})

test("graded 非法 → unit 为 null", function () {
  const front = GOOD_FRONT.replace("graded: script", "graded: 随机")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.indexOf("单元 db:mvcc 的 graded 非法：随机") !== -1,
    JSON.stringify(res.warnings)
  )
})

test("status 非法 → unit 为 null", function () {
  const front = GOOD_FRONT.replace("status: verified", "status: 差不多")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.indexOf("单元 db:mvcc 的 status 非法：差不多") !== -1,
    JSON.stringify(res.warnings)
  )
})

test("graded: script 却没写 script → unit 为 null", function () {
  const front = GOOD_FRONT.replace("script: units/database/mvcc.assert.py", "script: \"\"")
  const res = loaderC.parseUnitFile(makeUnitFile(front, GOOD_BODY), { path: "database/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.indexOf("单元 db:mvcc 声明 graded: script 但没写 script 路径") !== -1,
    JSON.stringify(res.warnings)
  )
})

test("放在与 track 不符的目录下 → unit 为 null", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), { path: "ai/mvcc.md" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.indexOf("单元 db:mvcc 放在 units/ai/ 下，与 track 不符") !== -1,
    JSON.stringify(res.warnings)
  )
})

test("缺 frontmatter → unit 为 null", function () {
  const res = loaderC.parseUnitFile("## 讲解\n\n没有 frontmatter。\n", { path: "database/x.md" })
  assert.equal(res.unit, null)
  assert.ok(res.warnings.indexOf("缺少 frontmatter") !== -1, JSON.stringify(res.warnings))
})

test("算法轨分支：题面进 statement，测试用例转断言", function () {
  const front = [
    "id: algo:lc-00007",
    "track: algorithm",
    "kind: problem",
    "title: 反转整数",
    "graded: user",
    "status: draft",
  ].join("\n")
  const body = [
    "## 题面",
    "",
    "给你一个整数 x，返回它反转后的整数。",
    "",
    "## 测试用例",
    "",
    "| input | expect |",
    "|---|---|",
    "| 123 | 321 |",
    "",
    "## 参考解法",
    "",
    "```cpp",
    "int reverse(int x) { return 0; }",
    "```",
    "",
  ].join("\n")
  const res = loaderC.parseUnitFile(makeUnitFile(front, body), { path: "algorithm/lc-00007.md" })
  assert.ok(res.unit)
  assert.equal(res.unit.statement, "给你一个整数 x，返回它反转后的整数。")
  assert.equal(res.unit.asserts.length, 1)
  assert.deepEqual(res.unit.asserts[0], { type: "output", input: "123", expect: "321", cmp: "exact" })
  // 算法轨的入口恒为 user + cpp，script 恒空
  assert.equal(res.unit.entry.graded, "user")
  assert.equal(res.unit.entry.script, "")
  assert.equal(res.unit.entry.language, "cpp")
})

test("未知小节被忽略，不报错", function () {
  const body = ["## 讲解", "", "正文", "", "## 某个没听过的小节", "", "随便写点。", ""].join("\n")
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, body), { path: "database/mvcc.md" })
  assert.ok(res.unit)
  assert.equal(res.unit.statement, "正文")
})

test("不传 path → 不产出单元，只给一条中文警告（G42）", function () {
  const res = loaderC.parseUnitFile(makeUnitFile(GOOD_FRONT, GOOD_BODY), {})
  assert.equal(res.unit, null)
  assert.equal(res.warnings.length, 1)
  assert.ok(res.warnings[0].indexOf("path") !== -1, res.warnings[0])
})

test("带 tol 的用例：丢弃 tol 并留中文警告", function () {
  const front = [
    "id: algo:lc-00007",
    "track: algorithm",
    "kind: problem",
    "title: 反转整数",
    "graded: user",
    "status: draft",
  ].join("\n")
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
  const res = loaderC.parseUnitFile(makeUnitFile(front, body), { path: "algorithm/lc-00007.md" })
  assert.ok(res.unit)
  assert.equal(res.unit.asserts.length, 1)
  assert.equal(res.unit.asserts[0].cmp, "exact")
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("用例 1 带 tol 参数，v1 按精确比对处理") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})
