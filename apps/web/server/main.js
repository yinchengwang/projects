"use strict"
/**
 * main.js —— 命令行入口
 *
 * 用法：node apps/web/server/main.js
 * 环境变量：PORT（默认 5173）
 */

const fs = require("node:fs")
const path = require("node:path")

const { createApp } = require("./app.js")

/**
 * 读取 .env 文件，把键塞进 process.env。已存在的环境变量优先。
 * @param {string} [file] 默认读 apps/web/.env
 * @returns {object} 读到的键值对
 */
function loadDotEnv (file) {
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

function main () {
  loadDotEnv()
  const port = Number(process.env.PORT || 5173)
  const app = createApp()
  app.listen(port).then(function (p) {
    console.log("应用已启动：http://127.0.0.1:" + p)
  }).catch(function (err) {
    const code = err && err.code ? err.code : "未知错误"
    if (code === "EADDRINUSE") {
      console.error("启动失败：端口 " + port + " 已被占用（EADDRINUSE），请换一个端口")
    } else {
      console.error("启动失败：无法监听端口 " + port + "（" + code + "）")
    }
    process.exit(1)
  })
}

if (require.main === module) {
  main()
}

module.exports = { main: main, loadDotEnv: loadDotEnv }
