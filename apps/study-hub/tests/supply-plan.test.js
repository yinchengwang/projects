"use strict"
/** supply-plan.test.js —— 供给缺口扫描 */
const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { scanGaps, writePlanFiles, missingOf, GAP_KINDS, EMPTY_EXPLAIN } = require("../core/supply/plan.js")

function cardUnit(patch) {
  const p = patch || {}
  return Object.assign({
    id: "db:mvcc", track: "database", kind: "card", title: "MVCC", statement: "# MVCC\n\n## 要点\n版本链\n",
    entry: { demo: "scaffold/grokking/mvcc/main.py", reference: "", graded: "script", script: "units/database/mvcc.assert.py", language: "python" },
    asserts: [], topics: [], difficulty: "medium", source: "grokking", status: "verified", progress: "todo", reviewed: false, reviewed_at: "", subtracks: [], path: "scaffold/grokking/mvcc/",
  }, p)
}
function problemUnit(patch) {
  const p = patch || {}
  return Object.assign({
    id: "algo:lc-00007", track: "algorithm", kind: "problem", title: "某道题", statement: "题面在这里",
    entry: { demo: "", reference: "", graded: "user", script: "", language: "cpp" },
    asserts: [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }], topics: [], difficulty: "medium", source: "leetcode", status: "verified", progress: "todo", reviewed: false, reviewed_at: "", subtracks: [], path: "bank/fundamentals/lc-00007.md",
  }, p)
}
function makeRoot(files) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-plan-"))
  Object.keys(files || {}).forEach(function (rel) { const abs = path.join(root, rel.split("/").join(path.sep)); fs.mkdirSync(path.dirname(abs), { recursive: true }); fs.writeFileSync(abs, files[rel], "utf8") })
  return root
}

test("缺口种类与契约 §8.4 一致", function () { assert.deepStrictEqual(GAP_KINDS, ["asserts", "statement", "cases", "solution"]) })
test("卡缺断言脚本 → missing 含 asserts", function () { const root = makeRoot({ "scaffold/grokking/mvcc/main.py": "print(1)\n" }); assert.deepStrictEqual(missingOf(cardUnit(), { learningRoot: root }), ["asserts"]) })
test("卡有断言脚本但讲解还是占位符 → missing 含 statement", function () { const root = makeRoot({ "scaffold/grokking/mvcc/main.py": "print(1)\n", "units/database/mvcc.assert.py": "print(1)\n" }); assert.deepStrictEqual(missingOf(cardUnit({ statement: EMPTY_EXPLAIN }), { learningRoot: root }), ["statement"]) })
test("卡齐了 → 没有缺口", function () { const root = makeRoot({ "scaffold/grokking/mvcc/main.py": "print(1)\n", "units/database/mvcc.assert.py": "print(1)\n" }); assert.deepStrictEqual(missingOf(cardUnit(), { learningRoot: root }), []) })
test("算法题：题面空 → statement；用例空 → cases；参考解法空 → solution", function () { const root = makeRoot({}); const missing = missingOf(problemUnit({ statement: "", asserts: [] }), { learningRoot: root, contentRoots: [root] }); assert.ok(missing.indexOf("statement") !== -1); assert.ok(missing.indexOf("cases") !== -1); assert.ok(missing.indexOf("solution") !== -1) })
test("算法题：读得到原文件且参考解法非空 → 不报 solution", function () { const root = makeRoot({ "bank/fundamentals/lc-00007.md": "---\nid: lc-00007\n---\n\n## 题面\n\n题面\n\n## 测试用例\n\n| input | expect |\n|---|---|\n| 1 2 | 3 |\n\n## 参考解法\n\n```cpp\nint main(){}\n```\n" }); assert.deepStrictEqual(missingOf(problemUnit(), { learningRoot: root, contentRoots: [root] }), []) })
test("scanGaps 的轨道计数正确", function () { const root = makeRoot({ "scaffold/grokking/mvcc/main.py": "print(1)\n", "units/database/mvcc.assert.py": "print(1)\n" }); const result = scanGaps({ units: [cardUnit({ reviewed: true }), cardUnit({ id: "db:transaction", status: "draft", entry: { demo: "scaffold/grokking/transaction/main.py", script: "units/database/transaction.assert.py" } }), problemUnit({ status: "todo" })], learningRoot: root, contentRoots: [root] }); const db = result.tracks.filter(function (t) { return t.track === "database" })[0]; assert.strictEqual(db.total, 2); assert.strictEqual(db.verified, 1); assert.strictEqual(db.draft, 1); assert.strictEqual(db.reviewed, 1); assert.strictEqual(result.tracks.length, 3) })
test("scanGaps 的 gaps 按 id 升序，且能按轨过滤", function () { const root = makeRoot({}); const units = [cardUnit({ id: "db:zeta" }), cardUnit({ id: "db:alpha" }), problemUnit({ statement: "" })]; const all = scanGaps({ units: units, learningRoot: root, contentRoots: [root] }); assert.deepStrictEqual(all.gaps.map(function (g) { return g.id }), ["algo:lc-00007", "db:alpha", "db:zeta"]); const onlyDb = scanGaps({ units: units, learningRoot: root, contentRoots: [root], track: "database" }); assert.strictEqual(onlyDb.tracks.length, 1); assert.deepStrictEqual(onlyDb.gaps.map(function (g) { return g.id }), ["db:alpha", "db:zeta"]) })
test("writePlanFiles 把缺口清单写进 learning/units/<track>/plan.json", function () { const root = makeRoot({ "scaffold/grokking/mvcc/main.py": "print(1)\n" }); const unitsDir = path.join(root, "units"); const result = scanGaps({ units: [cardUnit()], learningRoot: root, contentRoots: [root] }); const written = writePlanFiles({ result: result, unitsDir: unitsDir, repoRoot: null, stateFile: null }); assert.deepStrictEqual(written.blocked, []); const parsed = JSON.parse(fs.readFileSync(path.join(unitsDir, "database", "plan.json"), "utf8")); assert.strictEqual(parsed.track, "database"); assert.strictEqual(parsed.gaps.length, 1); assert.deepStrictEqual(parsed.gaps[0].missing, ["asserts"]) })
