"use strict"
/**
 * main.js —— 命令行入口
 *
 * 用法：node server/main.js
 * 环境变量：PORT（默认 5173）、ALGO_PLAN_LEARNING_ROOT（可选）
 */

const fs = require("node:fs")
const path = require("node:path")

const { createApp } = require("./app.js")

/**
 * 读取 .env 文件，把里面的键塞进 process.env。
 *
 * 仓库惯例：本地密钥放在 .env 里（projects/.gitignore 已经忽略 .env 与 .env.local）。
 * 已经存在的环境变量优先，.env 不覆盖——命令行传的值永远最大。
 *
 * @param {string} [file] 默认读 apps/algo-plan/.env
 * @returns {object} 读到的键值对
 */
function loadDotEnv(file) {
  const target = file || path.join(__dirname, "..", ".env")
  let text
  try {
    text = fs.readFileSync(target, "utf8")
  } catch (err) {
    return {}
  }
  const loaded = {}
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim()
    if (line === "" || line.charAt(0) === "#") continue
    const eq = line.indexOf("=")
    if (eq === -1) continue
    const key = line.slice(0, eq).trim()
    if (key === "") continue
    let value = line.slice(eq + 1).trim()
    if (value.length >= 2 && value.charAt(0) === '"' && value.charAt(value.length - 1) === '"') {
      value = value.slice(1, -1)
    }
    loaded[key] = value
    if (process.env[key] === undefined) process.env[key] = value
  }
  return loaded
}

function checkNodeVersion() {
  const major = Number(process.versions.node.split(".")[0])
  if (major < 24) {
    console.error("需要 Node >= 24（用到 node:sqlite），当前是 " + process.version)
    process.exit(1)
  }
}

function main() {
  checkNodeVersion()
  // 本地密钥放在 apps/algo-plan/.env 里，命令行传的环境变量优先。
  loadDotEnv()
  const app = createApp()
  const port = Number(process.env.PORT || 5173)
  // listen 是否已经成功：用来区分「端口没起来」和「起来了但后面的启动输出炸了」
  let listenOk = false
  app
    .listen(port)
    .then(function (p) {
      listenOk = true
      console.log("算法能力计划 App 已启动：http://127.0.0.1:" + p)
      if (app.learning.ok) {
        console.log("learning 仓：" + app.learning.root)
      } else {
        console.log("未连接 learning 仓（纯本地模式）：" + app.learning.reason)
      }
      // M6：LLM 状态。没配密钥也照常起得来，只是 LLM 那部分用不了。
      const info = app.llm.describe()
      if (info.effectiveProvider === "null") {
        console.log("LLM 未启用：" + info.reason + "（其余功能照常可用）")
      } else {
        console.log("LLM 已就绪：" + info.effectiveProvider + " / " + info.effectiveModel)
      }
    })
    .catch(function (err) {
      const code = err && err.code ? err.code : "未知错误"
      if (listenOk) {
        // listen 已经成功了：异常来自上面的 .then 回调（比如读 app.learning 时炸了），
        // 跟端口一点关系都没有。这时候再 process.exit(1) 会把已经跑起来的服务杀掉，
        // 所以只记日志，让服务继续跑。
        console.error(
          "[algo-plan] 启动信息输出失败（服务已在端口 " + port + " 上运行）：",
          err
        )
        return
      }
      // listen 失败：最常见的是端口被占用，按错误码给不同的中文提示
      if (code === "EADDRINUSE") {
        console.error(
          "启动失败：端口 " + port + " 已被占用（EADDRINUSE），请换一个端口，或先关掉占用该端口的程序"
        )
      } else if (code === "EACCES") {
        console.error(
          "启动失败：没有权限监听端口 " + port + "（EACCES），请换一个 1024 以上的端口"
        )
      } else {
        console.error("启动失败：无法监听端口 " + port + "（" + code + "）")
      }
      if (!err || !err.code) {
        // 没有错误码的意外异常才补一份堆栈，方便定位
        console.error(err)
      }
      // 这里必须用 process.exit(1)：createApp() 已经打开了 SQLite 连接，
      // 改成 process.exitCode = 1 的话进程会因为连接还挂着而不退出。
      // stderr 是终端而不是管道时不会截断；真要管道场景再单独处理。
      process.exit(1)
    })
}

if (require.main === module) {
  main()
}

module.exports = { main: main, checkNodeVersion: checkNodeVersion, loadDotEnv: loadDotEnv }
