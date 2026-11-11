"use strict"
/**
 * run-all.js —— 一键跑全部测试
 *
 * 用法：node tests/run-all.js
 * 自动发现 tests/*.test.js，新增测试套件无需手动登记。
 */

const fs = require("node:fs")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const ROOT = path.resolve(__dirname, "..")

const suites = []
fs.readdirSync(path.join(ROOT, "tests")).forEach(function (file) {
  if (/^.+\.test\.js$/.test(file)) suites.push(file)
})

let failed = 0
suites.forEach(function (file) {
  process.stdout.write("\n▶ " + file + "\n")
  try {
    const out = execFileSync(process.execPath, [path.join(ROOT, "tests", file)], {
      cwd: ROOT,
      encoding: "utf8",
    })
    const lines = out.trim().split("\n")
    console.log(lines.slice(-3).join("\n"))
  } catch (e) {
    failed += 1
    const out = ((e.stdout || "") + (e.stderr || "")).trim()
    console.log(out.split("\n").slice(-20).join("\n"))
  }
})

console.log("\n" + (suites.length - failed) + " / " + suites.length + " 套件通过")
process.exit(failed === 0 ? 0 : 1)
