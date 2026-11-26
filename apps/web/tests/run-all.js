"use strict"
/**
 * run-all.js —— 一键跑全部测试
 *
 * 用法：cd apps/web && node tests/run-all.js
 *
 * 自动发现 tests/*.test.js，新增测试套件只需放文件，无需手动登记。
 */

const fs = require("fs")
const path = require("path")
const { execFileSync } = require("child_process")

const ROOT = path.resolve(__dirname, "..")

const SUITES = []
fs.readdirSync(path.join(ROOT, "tests")).forEach(function (f) {
  if (/^.+\.test\.js$/.test(f)) {
    SUITES.push({ name: f.replace(/\.js$/, ""), file: "tests/" + f })
  }
})

let failed = 0
const summary = []

SUITES.forEach(function (suite) {
  process.stdout.write("\n\x1b[1m▶ " + suite.name + "\x1b[0m\n")
  try {
    const out = execFileSync(process.execPath, [path.join(ROOT, suite.file)], {
      cwd: ROOT,
      encoding: "utf8"
    })
    console.log(out.trim().split("\n").slice(-3).join("\n"))
    summary.push({ name: suite.name, ok: true })
  } catch (e) {
    console.log(((e.stdout || "") + (e.stderr || "")).trim().split("\n").slice(-16).join("\n"))
    summary.push({ name: suite.name, ok: false })
    failed += 1
  }
})

console.log("\n" + "=".repeat(52))
summary.forEach(function (s) {
  console.log((s.ok ? "\x1b[32m✓\x1b[0m " : "\x1b[31m✗\x1b[0m ") + s.name)
})

if (failed > 0) {
  console.log("\n\x1b[31m" + failed + " 组失败\x1b[0m")
  process.exit(1)
}
console.log("\n\x1b[32m全部 " + SUITES.length + " 组通过\x1b[0m")
