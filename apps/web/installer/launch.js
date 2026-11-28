"use strict"
/**
 * launch.js —— 安装后的启动器
 *
 * 干三件事：
 *   1. 看服务是不是已经在跑 —— 在跑就直接开浏览器，不重复起
 *   2. 没跑就拉起服务（后台、隐藏窗口），并把 AI 简报的数据目录指对
 *   3. 服务就绪后用默认浏览器打开首页
 *
 * 运行信息写在 runtime.json 里，stop.js 靠它停服务。
 *
 * 用法：
 *   node launch.js          启动（幂等）
 *   node launch.js --stop   停止
 */

const fs = require("node:fs")
const http = require("node:http")
const path = require("node:path")
const { spawn, execFileSync } = require("node:child_process")

const HOME = __dirname
const RUNTIME = path.join(HOME, "runtime.json")
const APP_DIR = path.join(HOME, "app")
const AI_TIMER_DIR = path.join(HOME, "ai-timer")
const PORT_FILE = path.join(HOME, "port.txt")

/** 端口候选：先读上次用的，再从默认往后找 */
const DEFAULT_PORT = 5173

/**
 * 不能用的端口。
 *
 * 5174 是 study-hub、8000 是 wiki-rag —— 这两个是独立进程，
 * 本应用不能占。而且 /api/health 会去探测它们：本应用要是也跑在 5174 上，
 * 探测就变成自己探自己，接口会无限递归（实测卡 17 秒）。
 *
 * @param {number} port
 * @returns {boolean}
 */
function reserved (port) {
  return port === 5174 || port === 8000
}

/**
 * 读 runtime.json。
 * @returns {object|null}
 */
function readRuntime () {
  try {
    return JSON.parse(fs.readFileSync(RUNTIME, "utf8"))
  } catch (e) {
    return null
  }
}

/**
 * 某个 pid 还活着吗。
 * @param {number} pid
 * @returns {boolean}
 */
function pidAlive (pid) {
  if (!pid) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (e) {
    return false
  }
}

/**
 * 问服务一句「你活着吗」。
 *
 * 探的是首页而不是 /api/health —— 后者要去探测 study-hub 与 wiki-rag，
 * 那两个没起的时候要等 1.5 秒超时，不适合当就绪信号。
 *
 * @param {number} port
 * @returns {Promise<boolean>}
 */
function ping (port) {
  return new Promise(function (resolve) {
    const req = http.request(
      { host: "127.0.0.1", port: port, path: "/", method: "GET", timeout: 1200 },
      function (res) { res.resume(); resolve(true) }
    )
    req.on("error", function () { resolve(false) })
    req.on("timeout", function () { req.destroy(); resolve(false) })
    req.end()
  })
}

/**
 * 找一个没人用的端口。
 *
 * 探针：listen 成功说明端口是空的；报错说明被占了，换下一个再试。
 *
 * @param {number} start
 * @returns {Promise<number>}
 */
function freePort (start) {
  return new Promise(function (resolve) {
    const net = require("node:net")
    const tryPort = function (p) {
      if (reserved(p)) { tryPort(p + 1); return }
      const srv = net.createServer()
      srv.once("error", function () {
        // 被占了 —— 往后找一个
        if (p >= start + 100) { resolve(p); return }
        tryPort(p + 1)
      })
      srv.once("listening", function () {
        // 找到了 —— 让出端口给服务进程
        srv.close(function () { resolve(p) })
      })
      srv.listen(p, "127.0.0.1")
    }
    tryPort(start)
  })
}

/**
 * 用默认浏览器打开一个地址。
 * @param {string} url
 */
function openBrowser (url) {
  try {
    if (process.platform === "win32") {
      execFileSync("cmd", ["/c", "start", "", url], { stdio: "ignore", timeout: 10000 })
    } else if (process.platform === "darwin") {
      execFileSync("open", [url], { stdio: "ignore" })
    } else {
      execFileSync("xdg-open", [url], { stdio: "ignore" })
    }
  } catch (e) {
    console.log("打不开浏览器，请手动访问：" + url)
  }
}

/**
 * 弹个提示框。
 *
 * 启动器是被 start.vbs 无窗口拉起来的 —— 出错的话没人看得见，
 * 所以得自己弹一下。
 *
 * @param {string} text
 */
function notify (text) {
  const msg = String(text).replace(/"/g, "'").replace(/\r?\n/g, " ")
  try {
    execFileSync("mshta.exe", [
      "javascript:alert(\"" + encodeURIComponent(msg).replace(/"/g, "%22") + "\");close()"
    ], { stdio: "ignore", timeout: 15000 })
  } catch (e) {
    // 连提示都弹不出来就算了，日志里有
  }
}

/**
 * 启动服务。
 * @returns {Promise<number>} 端口
 */
async function startServer () {
  let start = DEFAULT_PORT
  try {
    const saved = parseInt(fs.readFileSync(PORT_FILE, "utf8").trim(), 10)
    if (saved > 0) start = saved
  } catch (e) { /* 没写过就用默认 */ }

  const port = await freePort(start)
  const nodeExe = path.join(HOME, "node.exe")
  const entry = path.join(APP_DIR, "server", "main.js")

  const child = spawn(nodeExe, [entry], {
    cwd: APP_DIR,
    env: Object.assign({}, process.env, {
      PORT: String(port),
      AI_TIMER_ROOT: AI_TIMER_DIR
    }),
    detached: true,
    stdio: "ignore",
    windowsHide: true
  })
  child.unref()

  fs.writeFileSync(PORT_FILE, String(port), "utf8")
  fs.writeFileSync(RUNTIME, JSON.stringify({ pid: child.pid, port: port, startedAt: Date.now() }), "utf8")

  // 等服务就绪
  const deadline = Date.now() + 20000
  while (Date.now() < deadline) {
    await new Promise(function (r) { setTimeout(r, 300) })
    if (await ping(port)) return port
  }
  throw new Error("服务没能在 20 秒内就绪")
}

/**
 * 停服务。
 * @returns {boolean} 是否真的停了
 */
function stopServer () {
  const rt = readRuntime()
  if (!rt || !rt.pid) return false
  let stopped = false
  try {
    process.kill(rt.pid)
    stopped = true
  } catch (e) { /* 进程可能已经没了 */ }
  fs.rmSync(RUNTIME, { force: true })
  return stopped
}

async function main () {
  if (process.argv.indexOf("--stop") >= 0) {
    const ok = stopServer()
    console.log(ok ? "已停止" : "服务本来就没在跑")
    return
  }

  const rt = readRuntime()
  if (rt && pidAlive(rt.pid) && (await ping(rt.port))) {
    console.log("服务已在跑（端口 " + rt.port + "）")
    openBrowser("http://127.0.0.1:" + rt.port + "/")
    return
  }

  console.log("启动服务…")
  const port = await startServer()
  console.log("就绪：http://127.0.0.1:" + port + "/")
  openBrowser("http://127.0.0.1:" + port + "/")
}

if (require.main === module) {
  main().catch(function (err) {
    const msg = err && err.message ? err.message : String(err)
    try {
      fs.appendFileSync(path.join(HOME, "launcher.log"), "[" + new Date().toISOString() + "] " + msg + "\n")
    } catch (e) { /* 日志写不进去就算了 */ }
    notify("工作台启动失败：\n" + msg)
    process.exit(1)
  })
}

module.exports = { stopServer: stopServer, ping: ping, RUNTIME: RUNTIME }
