"use strict"
/**
 * remediation-env-example.test.js —— .env.example 安全与可用性闸门
 *
 * 约束：模板只给键名和注释，不塞真实密钥；每个关键键都要能被 server/main.js 读取。
 * 这条测试是防回归：绝不能出现看起来像真的 API key 的值，哪怕它是测试值。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const APP = path.resolve(__dirname, "..")
const TEMPLATE = path.join(APP, ".env.example")
const README = path.join(APP, "README.md")

const REQUIRED_KEYS = [
  "STUDY_HUB_LEARNING_ROOT",
  "STUDY_HUB_PYTHON",
  "PORT",
  "ARK_API_KEY",
  "ARK_BASE_URL",
  "ANTHROPIC_API_KEY",
  "STUDY_HUB_LLM_ENABLED",
  "STUDY_HUB_LLM_PROVIDER",
  "STUDY_HUB_LLM_MODEL",
  "STUDY_HUB_LLM_BASE_URL",
  "STUDY_HUB_LLM_SESSION_LIMIT",
  "STUDY_HUB_LLM_DAILY_LIMIT",
]

test(".env.example 覆盖所有关键配置键", function () {
  const text = fs.readFileSync(TEMPLATE, "utf8")
  REQUIRED_KEYS.forEach(function (key) {
    assert.ok(
      new RegExp("^#?\\s*" + key + "=", "m").test(text),
      "模板缺少 " + key
    )
  })
})

test(".env.example 不含看起来像真实密钥的值", function () {
  const text = fs.readFileSync(TEMPLATE, "utf8")
  const secretRows = text.split(/\r?\n/).filter(function (line) {
    return /^(ARK_API_KEY|ANTHROPIC_API_KEY)=/.test(line.trim())
  })
  assert.ok(secretRows.length >= 2, "至少要有两种密钥键的空白行")
  secretRows.forEach(function (line) {
    assert.match(line.trim(), /^(ARK_API_KEY|ANTHROPIC_API_KEY)=\s*$/)
  })
  assert.doesNotMatch(text, /sk-[A-Za-z0-9]{12,}|ark-[A-Za-z0-9]{12,}/i)
})

test("README 指向 .env.example 并说明环境变量优先", function () {
  const text = fs.readFileSync(README, "utf8")
  assert.ok(text.indexOf(".env.example") !== -1, "README 没链接模板")
  assert.ok(text.indexOf("环境变量优先") !== -1, "README 没说明覆盖顺序")
})

test(".env 被忽略而 .env.example 可跟踪", function () {
  const { execFileSync } = require("node:child_process")
  const root = path.resolve(APP, "..", "..")
  const ignoredOutput = execFileSync("git", ["check-ignore", "-v", "apps/study-hub/.env"], {
    cwd: root,
    encoding: "utf8",
  })
  assert.ok(ignoredOutput.indexOf(".env") !== -1, ignoredOutput)
  assert.throws(function () {
    execFileSync("git", ["check-ignore", "-q", "apps/study-hub/.env.example"], { cwd: root })
  })
})
