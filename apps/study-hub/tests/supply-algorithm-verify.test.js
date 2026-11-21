"use strict"
/**
 * supply-algorithm-verify.test.js —— 校验侧：真的 g++ 编译执行
 *
 * 本机没有 g++ 时**明确 skip 并说明原因**，不静默跳过（设计 §9）。
 * 全程不联网。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const fill = require("../core/supply/algorithm-fill.js")
const verify = require("../core/supply/algorithm-verify.js")
const { detectCompiler } = require("../judge/sandbox.js")

/** 没有 g++ 就整份 skip，并说清原因 */
const SKIP = detectCompiler() ? false : "本机没有 g++，无法做真实编译执行"

/** 正确的参考解法：把输入反转后输出 */
const REF_CODE = [
  "#include <iostream>",
  "#include <string>",
  "#include <algorithm>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  "  std::reverse(s.begin(), s.end());",
  '  std::cout << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 负例：原样输出（逻辑错了，但能编译能跑） */
const NEG_CODE = [
  "#include <iostream>",
  "#include <string>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  '  std::cout << s << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 编译不过的负例 */
const BAD_NEG_CODE = "int main() { 这不是合法的 C++ }"

/** 负例：结果对、但代码与参考解法不一样（能骗过所有用例——正是要抓住的那种） */
const PASSING_NEG_CODE = [
  "#include <iostream>",
  "#include <string>",
  "int main() {",
  "  std::string s;",
  "  if (!(std::cin >> s)) return 0;",
  "  std::string out;",
  "  for (int i = (int)s.size() - 1; i >= 0; --i) out += s[i];",
  '  std::cout << out << "\\n";',
  "  return 0;",
  "}",
].join("\n")

const CASES = [
  { input: "123", expect: "321" },
  { input: "456", expect: "654" },
]

/** 造一个单元三件套，返回单元 md 的路径 */
function makeUnit(root, stem, solution, negative, cases) {
  const unitsDir = path.join(root, "units", "algorithm")
  fill.writeTriplet({
    unitsDir: unitsDir,
    stem: stem,
    draft: {
      stem: stem,
      title: "字符串反转",
      difficulty: "easy",
      topics: ["字符串"],
      subtracks: ["fundamentals"],
      source: "leetcode",
      unitSource: "leetcode",
      sourceId: "7",
      statement: "给你一个字符串，把它反过来输出。",
      cases: cases || CASES,
      solution: solution,
      negative: negative,
      status: "draft",
    },
  })
  return { unitsDir: unitsDir, unitPath: path.join(unitsDir, stem + ".md") }
}

test("参考解法全 AC 且负例被拒 → verified", { skip: SKIP }, async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const unit = makeUnit(root, "lc-00007", REF_CODE, NEG_CODE)
    const res = await verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root })
    assert.equal(res.status, "verified", res.reason)
    assert.equal(res.ok, true)
    assert.equal(res.reference.verdict, "pass")
    assert.equal(res.reference.passedCases, CASES.length)
    assert.equal(res.negative.verdict, "wrong_answer")
    assert.ok(res.reason.indexOf("负例被拒绝（wrong_answer）") !== -1, res.reason)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("负例全过 → rejected，并说清「用例太弱」", { skip: SKIP }, async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    // 负例的输出与参考解法一样（换个写法实现），骗过了所有用例
    const unit = makeUnit(root, "lc-00008", REF_CODE, PASSING_NEG_CODE)
    const res = await verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root })
    assert.equal(res.status, "rejected", res.reason)
    assert.equal(res.reason, "负例竟然全部通过，说明测试用例太弱，挡不住错解")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("负例编译失败 → rejected，并说清「负例本身无效」", { skip: SKIP }, async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const unit = makeUnit(root, "lc-00009", REF_CODE, BAD_NEG_CODE)
    const res = await verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root })
    assert.equal(res.status, "rejected", res.reason)
    assert.equal(res.reason, "负例编译失败，负例本身无效")
    assert.equal(res.negative.verdict, "compile_error")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("参考解法不过 → rejected，理由里写明是哪种 verdict", { skip: SKIP }, async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const wrong = REF_CODE.replace("std::reverse(s.begin(), s.end());", "")
    const unit = makeUnit(root, "lc-00010", wrong, NEG_CODE)
    const res = await verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root })
    assert.equal(res.status, "rejected", res.reason)
    assert.ok(res.reason.indexOf("wrong_answer") !== -1, res.reason)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("md 代码块与 .reference.cpp 不一致 → rejected", function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const unit = makeUnit(root, "lc-00011", REF_CODE, NEG_CODE)
    // 偷偷改掉 .reference.cpp，md 里的代码块不动
    fs.writeFileSync(path.join(unit.unitsDir, "lc-00011.reference.cpp"), REF_CODE + "\n// 多了一行\n", "utf8")
    return verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root }).then(function (res) {
      assert.equal(res.status, "rejected", res.reason)
      assert.equal(res.reason, "参考解法文件与单元文件里的代码块不一致")
    })
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("没有负例文件 → rejected", function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const unit = makeUnit(root, "lc-00012", REF_CODE, NEG_CODE)
    fs.rmSync(path.join(unit.unitsDir, "lc-00012.negative.cpp"))
    return verify.verifyAlgorithmUnit({ unitPath: unit.unitPath, learningRoot: root }).then(function (res) {
      assert.equal(res.status, "rejected", res.reason)
      assert.equal(res.reason, "负例文件不存在：units/algorithm/lc-00012.negative.cpp")
    })
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("verifyAlgorithmUnits 批量校验，并只改 status 那一行", { skip: SKIP }, async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-verify-"))
  try {
    const good = makeUnit(root, "lc-00013", REF_CODE, NEG_CODE)
    const weak = makeUnit(root, "lc-00014", REF_CODE, PASSING_NEG_CODE)
    const before = fs.readFileSync(good.unitPath, "utf8")
    const summary = await verify.verifyAlgorithmUnits({
      unitsDir: good.unitsDir,
      learningRoot: root,
    })
    assert.equal(summary.checked, 2)
    assert.equal(summary.verified, 1)
    assert.equal(summary.rejected, 1)
    const after = fs.readFileSync(good.unitPath, "utf8")
    assert.equal(after, before.replace("status: draft", "status: verified"), "只改 status 那一行")
    assert.ok(fs.readFileSync(weak.unitPath, "utf8").indexOf("status: rejected") !== -1)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})
