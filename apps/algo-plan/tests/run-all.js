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

function main() {
  if (suites.length === 0) {
    // 空跑静默「通过」是最阴的一种假绿：一条断言都没跑，退出码却是 0
    // 这里用 process.exitCode 而不是 process.exit()：stderr 可能是异步管道，
    // process.exit() 会在管道把数据刷出去之前就把进程干掉，中文提示可能被截断。
    console.error("没有找到任何测试套件（tests/*.test.js），请先创建测试文件")
    process.exitCode = 1
    return
  }

  let failed = 0
  suites.forEach(function (file) {
    process.stdout.write("\n▶ " + file + "\n")
    try {
      const out = execFileSync(process.execPath, [path.join(ROOT, "tests", file)], {
        cwd: ROOT,
        encoding: "utf8",
      })
      const lines = out.trim().split("\n")
      // 保留 node:test 的收尾统计。只留 3 行会把 "ℹ pass N / ℹ fail 0" 正好切掉，
      // 看不出这套件到底跑了多少条断言
      console.log(lines.slice(-5).join("\n"))
      const counts = []
      lines.forEach(function (line) {
        const m = /^ℹ (tests|pass|fail)\s+(\d+)/.exec(line)
        if (m) counts.push(m[1] + " " + m[2])
      })
      if (counts.length > 0) console.log("断言统计：" + counts.join(" / "))
    } catch (e) {
      failed += 1
      const out = ((e.stdout || "") + (e.stderr || "")).trim()
      console.log(out.split("\n").slice(-20).join("\n"))
    }
  })

  console.log("\n" + (suites.length - failed) + " / " + suites.length + " 套件通过")
  // 用 process.exitCode 而不是 process.exit()：stderr 是异步管道时，
  // process.exit() 可能在管道把数据刷出去之前就把进程干掉，中文提示会被截断。
  // 置了 exitCode 之后进程自然退出，stderr 能被 flush 干净；退出码照样非 0。
  process.exitCode = failed === 0 ? 0 : 1
}

main()
