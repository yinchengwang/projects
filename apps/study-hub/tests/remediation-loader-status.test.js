"use strict"
/**
 * remediation-loader-status.test.js —— 状态门槛闸门
 *
 * 补齐设计 §3.2：verified 只能由 loader C（verify 写回的单元文件）承载。
 * loader B 单独加载一张卡时，最多给 draft——不管断言脚本在不在。
 * 理由：脚本存在 ≠ 校验通过。mvcc / index_design 的脚本存在但变异测试判 rejected，
 * 若「脚本存在即 verified」，这两张会被绕过裁决放回学习队列。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const loaderB = require("../core/unit/loader-b.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一张卡目录；unitsDir 下可放断言脚本。 */
function makeCard(stem, opts) {
  const options = opts || {}
  const root = path.join(tmpDir("study-hub-status-"), stem)
  const dir = path.join(root, "card")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(path.join(dir, "README.md"), "# " + stem + "\n\n正文\n", "utf8")
  fs.writeFileSync(path.join(dir, "main.py"), "print('hi')\n", "utf8")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  if (options.withScript) {
    fs.writeFileSync(path.join(unitsDir, stem + ".assert.py"), "print('ok')\n", "utf8")
  }
  return { dir: dir, unitsDir: path.join(root, "units") }
}

test("断言脚本不存在 → draft（不是 verified）", function () {
  const card = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(res.unit)
  assert.equal(res.unit.status, "draft")
})

test("断言脚本存在但没有 loader C 单元文件 → 仍是 draft", function () {
  const card = makeCard("mvcc", { withScript: true })
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(res.unit)
  assert.equal(res.unit.status, "draft", "脚本存在不等于校验通过，不能给 verified")
})

test("status 仍然只能是四档里的合法值", function () {
  const card = makeCard("mvcc", { withScript: true })
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(["todo", "draft", "verified", "rejected"].indexOf(res.unit.status) !== -1)
})
