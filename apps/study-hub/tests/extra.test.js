"use strict"
/**
 * extra.test.js —— unit.extra 的形状与归一
 *
 * unit.extra 是 4 个附加元数据字段的嵌套袋（契约 §1.2 的 17 个冻结字段一个字不动）。
 * 四个键恒存在，形状不随来源漂移：loader A/B 是全缺省，loader C 是 frontmatter 里的值。
 */

const test = require("node:test")
const assert = require("node:assert/strict")

const extra = require("../core/unit/extra.js")
const { createUnit } = require("../core/unit/unit.js")

test("EXTRA_KEYS 是约定的四个键，顺序固定", function () {
  assert.deepStrictEqual(extra.EXTRA_KEYS, ["runs_local", "needs", "fallback", "timeout_s"])
})

test("RUNS_LOCAL_VALUES 是 yes / partial / no", function () {
  assert.deepStrictEqual(extra.RUNS_LOCAL_VALUES, ["yes", "partial", "no"])
})

test("makeExtra：不给输入时四个键都在，值全是缺省", function () {
  const e = extra.makeExtra()
  assert.strictEqual(e.runs_local, "no")
  assert.deepStrictEqual(e.needs, [])
  assert.strictEqual(e.fallback, "")
  assert.strictEqual(e.timeout_s, extra.DEFAULT_TIMEOUT_S)
})

test("makeExtra：runs_local 非法值落回 no（宁可说跑不了，也不误报能跑）", function () {
  assert.strictEqual(extra.makeExtra({ runs_local: "yes" }).runs_local, "yes")
  assert.strictEqual(extra.makeExtra({ runs_local: "PARTIAL" }).runs_local, "no")
  assert.strictEqual(extra.makeExtra({ runs_local: "也许" }).runs_local, "no")
  assert.strictEqual(extra.makeExtra({ runs_local: "" }).runs_local, "no")
})

test("makeExtra：needs 只留枚举内的值", function () {
  const e = extra.makeExtra({ needs: ["vram", "nvcc", "外星科技"] })
  assert.deepStrictEqual(e.needs, ["vram", "nvcc"])
})

test("makeExtra：timeout_s 夹到 [1, 600]", function () {
  assert.strictEqual(extra.makeExtra({ timeout_s: 300 }).timeout_s, 300)
  assert.strictEqual(extra.makeExtra({ timeout_s: 99999 }).timeout_s, extra.MAX_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: 0 }).timeout_s, extra.DEFAULT_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: -5 }).timeout_s, extra.DEFAULT_TIMEOUT_S)
  assert.strictEqual(extra.makeExtra({ timeout_s: "abc" }).timeout_s, extra.DEFAULT_TIMEOUT_S)
})

test("createUnit：产出的单元恒有 extra（loader A/B 走这条路）", function () {
  const unit = createUnit({
    id: "algo:lc-00001",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    entry: { graded: "user", script: "", language: "cpp" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "bank/interview/lc-00001.md",
  })
  assert.ok(unit.extra, "extra 必须在场")
  assert.deepStrictEqual(Object.keys(unit.extra).sort(), extra.EXTRA_KEYS.slice().sort())
  assert.strictEqual(unit.extra.runs_local, "no")
})

test("createUnit：传入 extra 时原样带上", function () {
  const unit = createUnit({
    id: "ai:probe",
    track: "ai",
    kind: "card",
    title: "探针",
    entry: { graded: "script", script: "units/ai/probe.assert.py", language: "python" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "units/ai/probe.md",
    extra: { runs_local: "yes", needs: ["vram"], fallback: "降级读文档", timeout_s: 120 },
  })
  assert.strictEqual(unit.extra.runs_local, "yes")
  assert.deepStrictEqual(unit.extra.needs, ["vram"])
  assert.strictEqual(unit.extra.fallback, "降级读文档")
  assert.strictEqual(unit.extra.timeout_s, 120)
})

test("createUnit：extra 里的非法值被归一，不让脏值进 Unit", function () {
  const unit = createUnit({
    id: "ai:probe2",
    track: "ai",
    kind: "card",
    title: "探针二",
    entry: { graded: "script", script: "units/ai/probe2.assert.py", language: "python" },
    asserts: [],
    topics: [],
    subtracks: [],
    path: "units/ai/probe2.md",
    extra: { runs_local: "胡说", needs: ["vram", "外星"], timeout_s: 999999 },
  })
  assert.strictEqual(unit.extra.runs_local, "no")
  assert.deepStrictEqual(unit.extra.needs, ["vram"])
  assert.strictEqual(unit.extra.timeout_s, extra.MAX_TIMEOUT_S)
})

test("normalizeExtra：对象不是 extra 就当全缺省", function () {
  const e = extra.normalizeExtra(undefined)
  assert.strictEqual(e.runs_local, "no")
  assert.deepStrictEqual(e.needs, [])
  assert.strictEqual(e.fallback, "")
  assert.strictEqual(e.timeout_s, extra.DEFAULT_TIMEOUT_S)
})
