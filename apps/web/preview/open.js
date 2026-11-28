"use strict"
/**
 * open.js —— 一键打开前端页面预览
 *
 * 用法：node apps/web/preview/open.js
 *
 * 做的事：
 * 1. 检查应用服务是否已在跑；在跑就直接开浏览器，没跑就起一个；
 * 2. 用系统默认浏览器打开 /preview.html。
 *
 * 预览页本身在 apps/web/web/preview.html，由应用服务托管 ——
 * 所以它与应用同源，iframe 里的页面能正常读 localStorage、调 /api/*。
 */

const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const http = require("node:http")
const { spawn } = require("node:child_process")

const REPO = path.resolve(__dirname, "..", "..", "..")
const PORT = Number(process.env.PORT || 5173)
const PREVIEW_URL = "http://127.0.0.1:" + PORT + "/preview.html"

/**
 * 探测服务是否已在跑。
 * @param {number} port
 * @returns {Promise<boolean>}
 */
function probe (port) {
  return new Promise(function (resolve) {
    const req = http.request(
      { host: "127.0.0.1", port: port, path: "/api/health", method: "GET", timeout: 1200 },
      function (res) { res.resume(); resolve(true) }
    )
    req.on("error", function () { resolve(false) })
    req.on("timeout", function () { req.destroy(); resolve(false) })
    req.end()
  })
}

/**
 * 用系统默认浏览器打开一个 URL。
 * @param {string} url
 */
function openBrowser (url) {
  const platform = process.platform
  let cmd = null
  let args = []
  if (platform === "win32") {
    cmd = "cmd"
    args = ["/c", "start", "", url]
  } else if (platform === "darwin") {
    cmd = "open"
    args = [url]
  } else {
    cmd = "xdg-open"
    args = [url]
  }
  try {
    spawn(cmd, args, { detached: true, stdio: "ignore" }).unref()
  } catch (e) {
    console.log("无法自动打开浏览器，请手动访问：" + url)
  }
}

async function main () {
  const alive = await probe(PORT)
  if (alive) {
    console.log("[预览] 服务已在跑（端口 " + PORT + "）")
  } else {
    console.log("[预览] 启动应用服务，端口 " + PORT)
    const child = spawn(process.execPath, [path.join(REPO, "apps", "web", "server", "main.js")], {
      cwd: REPO,
      env: Object.assign({}, process.env, { PORT: String(PORT) }),
      stdio: "inherit"
    })
    // 等服务就绪
    const deadline = Date.now() + 20000
    while (Date.now() < deadline) {
      await new Promise(function (r) { setTimeout(r, 300) })
      if (await probe(PORT)) break
    }
    if (!(await probe(PORT))) {
      console.error("[预览] 服务启动失败")
      process.exit(1)
    }
    console.log("[预览] 服务已就绪")
  }

  console.log("[预览] 打开 " + PREVIEW_URL)
  openBrowser(PREVIEW_URL)
  console.log("[预览] 关掉这个命令行窗口就会停掉服务（若由本脚本启动）")
}

main().catch(function (err) {
  console.error("[预览] 失败：", err)
  process.exit(1)
})
