"use strict"
/**
 * p2-asserts.test.js —— 四型断言校验器
 *
 * 判题只有一条路径：断言集 → 入口 → 沙箱执行 → 收集 → 逐条校验 → verdict。
 * 这一层就是「逐条校验」那一段，必须纯函数、可单测、不碰 IO。
 */

const test = require("node:test")
const assert = require("node:assert")

const asserts = require("../judge/asserts.js")

/** 造一次「跑完进程」的现场 */
function runOf(over) {
  return Object.assign(
    { stdout: "", stderr: "", exitCode: 0, timedOut: false, artifacts: {} },
    over || {}
  )
}

test("CMP_MODES 正好四档", function () {
  assert.deepStrictEqual(asserts.CMP_MODES, ["exact", "trim", "json", "lines"])
})

test("normalizeText 与 core/judge-spec/compare.js 的口径一致", function () {
  const compare = require("../core/judge-spec/compare.js")
  const samples = ["a\r\nb", "a  \nb\t", "  x  ", "", null]
  samples.forEach(function (s) {
    assert.strictEqual(asserts.normalizeText(s), compare.normalizeText(s), String(s))
  })
})

test("exact：归一化后相等即通过", function () {
  const r = asserts.compareByMode("a\r\nb  \n", "a\nb", "exact")
  assert.strictEqual(r.equal, true, r.reason)
})

test("trim：只去首尾空白，中间行的行尾空白不动", function () {
  // 夹具要能显出「trim 与 exact 的差别」，否则这条用例测不出档位语义：
  //   exact 会逐行去行尾空白 → "a  \nb" 与 "a\nb" 在 exact 下**相等**
  //   trim 只做整体 String.trim() → 内层的两个空格保住 → **不等**
  // （用单行 "  a  \n" vs "a" 是反的：String.trim() 会把行尾空白也去掉 → 反而相等。）
  const r = asserts.compareByMode("a  \nb", "a\nb", "trim")
  assert.strictEqual(r.equal, false, "trim 档不动中间行的行尾空白，两侧不等")
  assert.strictEqual(r.reason, "去掉首尾空白后仍与期望不一致")
  // 对照组：同一批输入在 exact 下相等（exact 会逐行去行尾空白）
  assert.strictEqual(asserts.compareByMode("a  \nb", "a\nb", "exact").equal, true, "exact 要相等")
})

test("json：键序无关、数组序有关", function () {
  assert.strictEqual(asserts.compareByMode('{"a":1,"b":2}', '{"b":2,"a":1}', "json").equal, true)
  assert.strictEqual(asserts.compareByMode("[1,2]", "[2,1]", "json").equal, false)
})

test("json：解析失败给出冻结文案", function () {
  const r = asserts.compareByMode("not json", "{}", "json")
  assert.strictEqual(r.equal, false)
  assert.strictEqual(r.reason, "期望是 JSON 但解析失败")
})

test("lines：行数必须一致", function () {
  assert.strictEqual(asserts.compareByMode("a\nb", "a\nb", "lines").equal, true)
  const r = asserts.compareByMode("a\nb", "a", "lines")
  assert.strictEqual(r.equal, false)
  assert.strictEqual(r.reason, "行数不一致：期望 1 行，实际 2 行")
})

test("validateAsserts：exit 型通过", function () {
  const out = asserts.validateAsserts([{ type: "exit", expect: 0 }], runOf({ exitCode: 0 }))
  assert.strictEqual(out.total, 1)
  assert.strictEqual(out.passed, 1)
  assert.deepStrictEqual(out.failures, [])
})

test("validateAsserts：exit 型非零退出给出冻结文案", function () {
  const out = asserts.validateAsserts([{ type: "exit", expect: 0 }], runOf({ exitCode: 3 }))
  assert.strictEqual(out.passed, 0)
  assert.strictEqual(out.failures.length, 1)
  assert.strictEqual(out.failures[0].reason, "期望退出码 0，实际 3")
  assert.strictEqual(out.failures[0].index, 0)
  assert.strictEqual(out.failures[0].type, "exit")
})

test("validateAsserts：exit 的 expect 不是 0 算非法断言", function () {
  const out = asserts.validateAsserts([{ type: "exit", expect: 1 }], runOf({ exitCode: 1 }))
  assert.strictEqual(out.passed, 0)
  assert.strictEqual(out.failures[0].reason, "断言非法：exit 类型的 expect 只能是 0，实际 1")
})

test("validateAsserts：contains 在 stdout+stderr 合并串里找", function () {
  const ok = asserts.validateAsserts([{ type: "contains", text: "警告" }], runOf({ stderr: "警告：有点问题" }))
  assert.strictEqual(ok.passed, 1)
  const bad = asserts.validateAsserts([{ type: "contains", text: "缺失" }], runOf({ stdout: "别的" }))
  assert.strictEqual(bad.failures[0].reason, "输出里没有找到：缺失")
})

test("validateAsserts：file 内容不一致给的是「产物」文案，不是「输出」", function () {
  // compareByMode 的 reason 是给 stdout 写的（「输出与期望不一致」），
  // 直接透传会让用户以为在说 stdout —— 而这条断言看的是产物文件。
  // 契约 §7.4 的冻结表：file 内容不一致 → 「产物内容与期望不一致」。
  const out = asserts.validateAsserts([{ type: "file", path: "out.txt", expect: "期望内容" }], {
    artifacts: { "out.txt": "实际内容" },
    exitCode: 0,
    stdout: "",
    stderr: "",
    timedOut: false,
  })
  assert.strictEqual(out.passed, 0)
  assert.strictEqual(out.failures[0].reason, "产物内容与期望不一致")
})

test("validateAsserts：file 内容一致时通过（别把「一致」也判成失败）", function () {
  const out = asserts.validateAsserts([{ type: "file", path: "out.txt", expect: "同一内容" }], {
    artifacts: { "out.txt": "同一内容" },
    exitCode: 0,
    stdout: "",
    stderr: "",
    timedOut: false,
  })
  assert.strictEqual(out.passed, 1, JSON.stringify(out.failures))
  assert.deepStrictEqual(out.failures, [])
})

test("lines：行数相同但某行不同 → 第 K 行不一致", function () {
  // 原 20 条用例只覆盖了「行数不一致」，**没覆盖「行数相同、内容不同」**
  // ——干跑实测：把 lines 的逐行比对改成 `if (false)`，测试仍 20/20 全绿。
  const r = asserts.compareByMode("a\nc", "a\nb", "lines")
  assert.strictEqual(r.equal, false, "第 2 行不同应当不等")
  assert.strictEqual(r.reason, "第 2 行与期望不一致")
})

test("validateAsserts：file 路径越界直接失败", function () {
  const out = asserts.validateAsserts([{ type: "file", path: "../etc/passwd", expect: "x" }], runOf())
  assert.strictEqual(out.failures[0].reason, "产物路径越界：../etc/passwd")
})

test("validateAsserts：file 绝对路径也算越界", function () {
  const out = asserts.validateAsserts([{ type: "file", path: "/etc/passwd", expect: "x" }], runOf())
  assert.strictEqual(out.failures[0].reason, "产物路径越界：/etc/passwd")
})

test("validateAsserts：file 不存在给出冻结文案", function () {
  const out = asserts.validateAsserts([{ type: "file", path: "out.txt", expect: "x" }], runOf())
  assert.strictEqual(out.failures[0].reason, "产物文件不存在：out.txt")
})

test("validateAsserts：file 从 artifacts 取内容比对", function () {
  const out = asserts.validateAsserts([{ type: "file", path: "out.txt", expect: "hello" }], runOf({
    artifacts: { "out.txt": "hello\n" },
  }))
  assert.strictEqual(out.passed, 1)
  assert.deepStrictEqual(out.failures, [])
})

test("validateAsserts：output 型按 cmp 比对 stdout", function () {
  const out = asserts.validateAsserts(
    [{ type: "output", input: "1 2", expect: "3", cmp: "exact" }],
    runOf({ stdout: "3\n" })
  )
  assert.strictEqual(out.passed, 1)
})

test("validateAsserts：output 型不匹配给出冻结文案", function () {
  const out = asserts.validateAsserts(
    [{ type: "output", expect: "3", cmp: "exact" }],
    runOf({ stdout: "4\n" })
  )
  assert.strictEqual(out.failures[0].reason, "输出与期望不一致")
})

test("validateAsserts：多条断言时 index 是断言集里的下标", function () {
  const out = asserts.validateAsserts(
    [
      { type: "exit", expect: 0 },
      { type: "contains", text: "没有的" },
      { type: "exit", expect: 0 },
    ],
    runOf({ exitCode: 0 })
  )
  assert.strictEqual(out.total, 3)
  assert.strictEqual(out.passed, 2)
  assert.strictEqual(out.failures.length, 1)
  assert.strictEqual(out.failures[0].index, 1)
})

test("validateAsserts：进程超时时全部断言都不通过", function () {
  const out = asserts.validateAsserts(
    [{ type: "exit", expect: 0 }, { type: "contains", text: "x" }],
    runOf({ timedOut: true })
  )
  assert.strictEqual(out.passed, 0)
  assert.strictEqual(out.total, 2)
  assert.strictEqual(out.failures.length, 2)
})

test("validateAsserts：空断言集返回 0/0", function () {
  const out = asserts.validateAsserts([], runOf())
  assert.deepStrictEqual(out, { passed: 0, total: 0, failures: [] })
})
