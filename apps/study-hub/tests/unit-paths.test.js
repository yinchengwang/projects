"use strict"
/**
 * unit-paths.test.js —— core/paths.js 的路径解析与环境变量改名
 *
 * 钉住 G26 / G40：旧环境变量不再被读取，新目录辅助按约定拼路径。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const paths = require("../core/paths.js")
const main = require("../server/main.js")

function tmpDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix))
}

test("resolveLearningRoot 认 STUDY_HUB_LEARNING_ROOT", function () {
  const dir = tmpDir("study-hub-learning-")
  const got = paths.resolveLearningRoot({ STUDY_HUB_LEARNING_ROOT: dir })
  assert.equal(got.ok, true)
  assert.equal(got.root, path.resolve(dir))
  assert.equal(got.reason, "")
})

test("resolveLearningRoot 没设环境变量时进入纯本地模式", function () {
  const got = paths.resolveLearningRoot({})
  assert.equal(got.ok, false)
  assert.equal(got.root, null)
  assert.ok(got.reason.indexOf("STUDY_HUB_LEARNING_ROOT") !== -1, got.reason)
})

test("resolveLearningRoot 不再读旧的 ALGO_PLAN_LEARNING_ROOT", function () {
  const dir = tmpDir("study-hub-legacy-")
  const got = paths.resolveLearningRoot({ ALGO_PLAN_LEARNING_ROOT: dir })
  assert.equal(got.ok, false, "旧环境变量必须不再生效")
  assert.equal(got.root, null)
})

test("resolveLearningRoot 指向不存在的目录时给出中文原因", function () {
  const missing = path.join(os.tmpdir(), "study-hub-不存在的目录-" + Date.now())
  const got = paths.resolveLearningRoot({ STUDY_HUB_LEARNING_ROOT: missing })
  assert.equal(got.ok, false)
  assert.ok(got.reason.indexOf("不存在") !== -1, got.reason)
})

test("bankDir / unitsDir / grokkingDir 按约定拼路径", function () {
  const root = path.join(os.tmpdir(), "learning-root")
  assert.equal(paths.bankDir(root), path.join(root, "bank"))
  assert.equal(paths.unitsDir(root), path.join(root, "units"))
  assert.equal(paths.grokkingDir(root), path.join(root, "scaffold", "grokking"))
})

test("三个目录辅助在 learningRoot 为空时都返回 null", function () {
  assert.equal(paths.bankDir(null), null)
  assert.equal(paths.unitsDir(null), null)
  assert.equal(paths.grokkingDir(null), null)
})

test("localBankDir 默认指向工程仓内置题库", function () {
  const got = paths.localBankDir({})
  assert.equal(got, path.join(__dirname, "..", "bank"))
})

test("localBankDir 认 STUDY_HUB_LOCAL_BANK，不认旧名", function () {
  const dir = tmpDir("study-hub-local-bank-")
  assert.equal(paths.localBankDir({ STUDY_HUB_LOCAL_BANK: dir }), path.resolve(dir))
  assert.equal(
    paths.localBankDir({ ALGO_PLAN_LOCAL_BANK: dir }),
    path.join(__dirname, "..", "bank"),
    "旧环境变量必须不再生效"
  )
})

test("checkLegacyEnvNames 只提示旧变量存在，不读它的值", function () {
  const hints = main.checkLegacyEnvNames({
    ALGO_PLAN_LEARNING_ROOT: "/某个旧路径",
    ALGO_PLAN_LOCAL_BANK: "",
  })
  assert.equal(hints.length, 2)
  assert.ok(hints[0].indexOf("STUDY_HUB_LEARNING_ROOT") !== -1, hints[0])
  assert.ok(hints[1].indexOf("STUDY_HUB_LOCAL_BANK") !== -1, hints[1])
})

test("checkLegacyEnvNames 没有旧变量时返回空数组", function () {
  assert.deepEqual(main.checkLegacyEnvNames({}), [])
})
