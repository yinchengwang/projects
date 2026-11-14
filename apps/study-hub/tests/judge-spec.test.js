"use strict"
/**
 * judge-spec.test.js —— 判题契约的纯函数层：用例归一化 + 输出比对
 */

const test = require("node:test")
const assert = require("node:assert")

const cases = require("../core/judge-spec/cases.js")
const compare = require("../core/judge-spec/compare.js")

test("normalizeCases 的 index 从 1 开始", function () {
  const out = cases.normalizeCases([
    { input: "1 2", expect: "3", tol: null },
    { input: "4 5", expect: "9", tol: null },
  ])
  assert.strictEqual(out.length, 2)
  assert.deepStrictEqual(
    out.map(function (c) {
      return c.index
    }),
    [1, 2]
  )
  assert.deepStrictEqual(Object.keys(out[0]).sort(), ["expect", "index", "input", "tol"])
})

test("normalizeCases 跳过输入与期望都空的行", function () {
  const out = cases.normalizeCases([
    { input: "", expect: "", tol: null },
    { input: "1", expect: "1", tol: null },
    { input: "  ", expect: "", tol: null },
  ])
  assert.strictEqual(out.length, 1)
  assert.strictEqual(out[0].index, 1, "跳过的空行不占号")
})

test("normalizeCases 去掉输入与期望的首尾空白", function () {
  const out = cases.normalizeCases([{ input: "  3 2 4 6  ", expect: "  1 2 ", tol: null }])
  assert.strictEqual(out[0].input, "3 2 4 6")
  assert.strictEqual(out[0].expect, "1 2")
})

test("normalizeCases 归一化容差", function () {
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: "1e-6" }])[0].tol, 1e-6)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: 0.5 }])[0].tol, 0.5)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: null }])[0].tol, null)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b" }])[0].tol, null)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: "" }])[0].tol, null)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: -1 }])[0].tol, null)
  assert.strictEqual(cases.normalizeCases([{ input: "a", expect: "b", tol: "abc" }])[0].tol, null)
})

test("normalizeCases 容忍非数组输入", function () {
  assert.deepStrictEqual(cases.normalizeCases(null), [])
  assert.deepStrictEqual(cases.normalizeCases(undefined), [])
  assert.deepStrictEqual(cases.normalizeCases("不是数组"), [])
})

test("compareOutput 完全一致时相等", function () {
  const r = compare.compareOutput("0 1", "0 1", null)
  assert.strictEqual(r.equal, true)
  assert.ok(r.reason.length > 0, "必须给出中文说明")
})

test("compareOutput 忽略行尾空白与末尾空行", function () {
  assert.strictEqual(compare.compareOutput("0 1   \n\n\n", "0 1", null).equal, true)
  assert.strictEqual(compare.compareOutput("0 1\n", "0 1\n\n", null).equal, true)
  assert.strictEqual(compare.compareOutput("0 1", "0 1\n", null).equal, true)
})

test("compareOutput 统一 CRLF 与 LF", function () {
  assert.strictEqual(compare.compareOutput("0 1\r\n", "0 1\n", null).equal, true)
  assert.strictEqual(compare.compareOutput("a\r\nb\r\n", "a\nb", null).equal, true)
})

test("compareOutput 去掉首尾空白", function () {
  assert.strictEqual(compare.compareOutput("   0 1   ", "0 1", null).equal, true)
  assert.strictEqual(compare.compareOutput("\n\n0 1\n\n", "0 1", null).equal, true)
})

test("compareOutput 内容不同则不等", function () {
  const r = compare.compareOutput("0 2", "0 1", null)
  assert.strictEqual(r.equal, false)
  assert.ok(r.reason.indexOf("不一致") !== -1, r.reason)
})

test("compareOutput 顺序不同则不等", function () {
  assert.strictEqual(compare.compareOutput("1 0", "0 1", null).equal, false)
  assert.strictEqual(compare.compareOutput("a\nb", "b\na", null).equal, false)
})

test("compareOutput 容差之内相等", function () {
  const r = compare.compareOutput("3.0000000001", "3", 1e-6)
  assert.strictEqual(r.equal, true)
  assert.ok(r.reason.indexOf("容差") !== -1, r.reason)
})

test("compareOutput 超出容差则不等", function () {
  const r = compare.compareOutput("3.1", "3", 1e-6)
  assert.strictEqual(r.equal, false)
  assert.ok(r.reason.indexOf("容差") !== -1, r.reason)
})

test("compareOutput 期望是数值但实际不是数值则不等", function () {
  const r = compare.compareOutput("abc", "3", 1e-6)
  assert.strictEqual(r.equal, false)
  assert.ok(r.reason.indexOf("不是数值") !== -1, r.reason)
})

test("compareOutput 期望不是数值时退化成精确匹配", function () {
  assert.strictEqual(compare.compareOutput("abc", "abc", 1e-6).equal, true)
  assert.strictEqual(compare.compareOutput("abc", "abd", 1e-6).equal, false)
})

test("toNumber 不把空串当数字 0", function () {
  assert.strictEqual(compare.toNumber(""), null)
  assert.strictEqual(compare.toNumber("   "), null)
  assert.strictEqual(compare.toNumber("abc"), null)
  assert.strictEqual(compare.toNumber("3"), 3)
  assert.strictEqual(compare.toNumber("-2.5"), -2.5)
  assert.strictEqual(compare.toNumber("1e-6"), 1e-6)
})
