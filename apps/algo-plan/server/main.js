"use strict"
/**
 * main.js —— 命令行入口
 *
 * 用法：node server/main.js
 * 环境变量：PORT（默认 5173）、ALGO_PLAN_LEARNING_ROOT（可选）
 */

const { createApp } = require("./app.js")

function checkNodeVersion() {
  const major = Number(process.versions.node.split(".")[0])
  if (major < 24) {
    console.error("需要 Node >= 24（用到 node:sqlite），当前是 " + process.version)
    process.exit(1)
  }
}

function main() {
  checkNodeVersion()
  const app = createApp()
  const port = Number(process.env.PORT || 5173)
  app.listen(port).then(function (p) {
    console.log("算法能力计划 App 已启动：http://127.0.0.1:" + p)
    if (app.learning.ok) {
      console.log("learning 仓：" + app.learning.root)
    } else {
      console.log("未连接 learning 仓（纯本地模式）：" + app.learning.reason)
    }
  })
}

if (require.main === module) {
  main()
}

module.exports = { main: main, checkNodeVersion: checkNodeVersion }
