"use strict"
/**
 * fetch-cf-pages.js —— 用本机 Chrome 抓取 Codeforces 题面页
 *
 * 为什么要浏览器：
 *   codeforces.com 的网页版接口（/api/ 除外）挂在 Cloudflare 后面，
 *   匿名 HTTP 请求会被拦在 "Just a moment" 挑战页上（403）。
 *   挑战要跑 JS 才过，脚本直连过不去——只能借一个真浏览器。
 *   （/api/problemset.problems 这种纯接口是放行的，元数据就走它。）
 *
 * 怎么用浏览器但不引依赖：
 *   项目是零依赖零构建的，所以这里不用 puppeteer/playwright，
 *   而是直接说 Chrome DevTools Protocol：
 *     1. 起一个 Chrome，开 --remote-debugging-port
 *     2. 连浏览器级的 WebSocket，用 Target.createTarget 开标签页
 *     3. 用 Target.attachToTarget（flat 模式）拿到会话，
 *        之后 Page.navigate / Runtime.evaluate 都带 sessionId
 *   Node 22+ 自带 WebSocket，Windows / macOS / Linux 都能跑。
 *
 * 用法：
 *   node tools/fetch-cf-pages.js --out data/cf-pages --plan data/cf-plan.json
 *   node tools/fetch-cf-pages.js --only 4-A,26-B
 *   node tools/fetch-cf-pages.js --headless
 */

const fs = require("node:fs")
const path = require("node:path")
const os = require("node:os")
const { spawn } = require("node:child_process")

/** Chrome 可执行文件的位置（按平台找） */
function findChrome() {
  const candidates = [
    process.env.CHROME_PATH,
    process.env.ALGO_PLAN_CHROME,
    "C:/Program Files/Google/Chrome/Application/chrome.exe",
    "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ].filter(Boolean)
  for (let i = 0; i < candidates.length; i++) {
    try {
      if (fs.existsSync(candidates[i])) return candidates[i]
    } catch (e) {
      // 忽略
    }
  }
  return ""
}

/**
 * 睡一会儿。
 * @param {number} ms
 * @returns {Promise<void>}
 */
function sleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms)
  })
}

/**
 * 起一个带调试端口的 Chrome，返回 { proc, port }。
 *
 * 默认起的是**有界面**的真 Chrome——Cloudflare 的挑战本来就该让真浏览器过，
 * 没必要去伪装指纹（伪装反而更像在干坏事）。要无头就加 --headless。
 *
 * @param {number} port
 * @param {boolean} headless 是否无头
 * @returns {{proc: object, port: number}}
 */
function launchChrome(port, headless) {
  const exe = findChrome()
  if (exe === "") throw new Error("找不到 Chrome，可设 CHROME_PATH 环境变量指定")
  const userDataDir = path.join(os.tmpdir(), "study-hub-cf-chrome-" + port)
  const args = []
  if (headless) args.push("--headless=new")
  args.push(
    "--no-first-run",
    "--no-default-browser-check",
    "--remote-debugging-port=" + port,
    "--user-data-dir=" + userDataDir,
    "about:blank"
  )
  const proc = spawn(exe, args, { stdio: "ignore", detached: false })
  return { proc: proc, port: port }
}

/**
 * 等 Chrome 的调试端口起来。
 * @param {number} port
 * @param {number} timeoutMs
 * @returns {Promise<string>} 浏览器级 WebSocket 地址
 */
async function waitForDevTools(port, timeoutMs) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    try {
      const res = await fetch("http://127.0.0.1:" + port + "/json/version")
      if (res.ok) {
        const body = await res.json()
        if (body.webSocketDebuggerUrl) return body.webSocketDebuggerUrl
      }
    } catch (e) {
      // 还没起来
    }
    await sleep(300)
  }
  throw new Error("Chrome 调试端口没起来：" + port)
}

/**
 * 一个极简的 CDP 客户端：发命令、等回应。
 * @param {string} wsUrl
 * @returns {Promise<{send: function, close: function}>}
 */
function connect(wsUrl) {
  return new Promise(function (resolve, reject) {
    const ws = new WebSocket(wsUrl)
    const pending = new Map()
    let nextId = 1
    let closed = false

    const client = {
      send: function (method, params, sessionId, timeoutMs) {
        if (closed) return Promise.reject(new Error("CDP 连接已断开"))
        const id = nextId++
        const msg = { id: id, method: method, params: params || {} }
        if (sessionId) msg.sessionId = sessionId
        try {
          ws.send(JSON.stringify(msg))
        } catch (e) {
          return Promise.reject(new Error("CDP 发送失败：" + method))
        }
        return new Promise(function (res, rej) {
          const timer = setTimeout(function () {
            pending.delete(id)
            rej(new Error("CDP 命令超时：" + method))
          }, timeoutMs || 30000)
          pending.set(id, function (result, error) {
            clearTimeout(timer)
            pending.delete(id)
            if (error) rej(new Error(error.message || JSON.stringify(error)))
            else res(result)
          })
        })
      },
      close: function () {
        closed = true
        try {
          ws.close()
        } catch (e) {
          // 忽略
        }
      },
    }

    ws.addEventListener("open", function () {
      resolve(client)
    })

    ws.addEventListener("message", function (event) {
      let msg
      try {
        msg = JSON.parse(String(event.data))
      } catch (e) {
        return
      }
      if (msg.id && pending.has(msg.id)) {
        pending.get(msg.id)(msg.result, msg.error)
      }
    })

    ws.addEventListener("error", function () {
      if (!closed) reject(new Error("CDP 连接失败：" + wsUrl))
    })

    ws.addEventListener("close", function () {
      closed = true
      pending.forEach(function (fn) {
        fn(undefined, { message: "CDP 连接被关闭" })
      })
      pending.clear()
    })
  })
}

/**
 * 开一个标签页，返回它的 CDP 连接。
 * @param {number} port
 * @returns {Promise<{cdp: object, targetId: string}>}
 */
async function openTab(port) {
  // 新版 Chrome 只认 PUT 动词来开新标签页
  const res = await fetch("http://127.0.0.1:" + port + "/json/new?about:blank", { method: "PUT" })
  const target = await res.json()
  const cdp = await connect(target.webSocketDebuggerUrl)
  return { cdp: cdp, targetId: target.id }
}

/**
 * 在已经开好的标签页里访问一个地址，返回渲染完成后的 HTML。
 *
 * 复用同一个标签页一路 navigate 下去——标签页攒多了 Chrome 会变慢甚至拒绝新开。
 *
 * @param {object} cdp 标签页的 CDP 客户端
 * @param {string} url
 * @param {number} waitMs 等待挑战页自己刷新的时间
 * @returns {Promise<string>}
 */
async function fetchPage(cdp, url, waitMs) {
  await cdp.send("Page.enable", {}, null, 20000)
  await cdp.send("Runtime.enable", {}, null, 20000)
  await cdp.send("Page.navigate", { url: url }, null, 30000)

  // 挑战页会自己刷新，等它刷出题面来
  const deadline = Date.now() + waitMs
  while (Date.now() < deadline) {
    await sleep(1000)
    const check = await cdp.send(
      "Runtime.evaluate",
      {
        expression: "document.querySelector('.problem-statement') ? 'yes' : 'no'",
        returnByValue: true,
      },
      null,
      15000
    )
    if (check && check.result && check.result.value === "yes") {
      const out = await cdp.send(
        "Runtime.evaluate",
        { expression: "document.documentElement.outerHTML", returnByValue: true },
        null,
        30000
      )
      return (out && out.result && out.result.value) || ""
    }
  }
  // 没等到题面，把当前页面原样带回去（调用方能看出是挑战页）
  const out = await cdp.send(
    "Runtime.evaluate",
    { expression: "document.documentElement.outerHTML", returnByValue: true },
    null,
    20000
  )
  return (out && out.result && out.result.value) || ""
}

/**
 * 主流程。
 * @param {{plan?: string, out: string, only?: string[], wait?: number, port?: number, headless?: boolean}} opts
 * @returns {Promise<object>}
 */
async function run(opts) {
  const options = opts || {}
  const planFile = options.plan || path.join(__dirname, "..", "data", "cf-plan.json")
  const outDir = options.out || path.join(__dirname, "..", "data", "cf-pages")
  const waitMs = options.wait || 25000

  const plan = JSON.parse(fs.readFileSync(planFile, "utf8"))
  const only = options.only || []
  const todo = only.length
    ? plan.filter(function (item) {
        return only.indexOf(item.contestId + "-" + item.index) !== -1
      })
    : plan

  fs.mkdirSync(outDir, { recursive: true })

  const port = options.port || 9333
  const chrome = launchChrome(port, options.headless === true)
  const result = { ok: true, total: todo.length, saved: [], failed: [], warnings: [] }

  try {
    await waitForDevTools(port, 20000)
    const tab = await openTab(port)

    for (let i = 0; i < todo.length; i++) {
      const item = todo[i]
      const key = item.contestId + "-" + item.index
      const file = path.join(outDir, key + ".html")
      if (fs.existsSync(file) && fs.statSync(file).size > 2000) {
        result.saved.push(key)
        continue
      }
      const url =
        "https://codeforces.com/problemset/problem/" + item.contestId + "/" + encodeURIComponent(item.index)

      let html = ""
      let lastError = null
      // 失败重试一次：Cloudflare 偶尔会让某个标签页卡在挑战页
      for (let attempt = 0; attempt < 2; attempt++) {
        try {
          html = await fetchPage(tab.cdp, url, waitMs)
          lastError = null
          if (html && html.indexOf("problem-statement") !== -1) break
          lastError = new Error("页面里没有题面（可能卡在 Cloudflare 挑战页）")
        } catch (e) {
          lastError = e
          html = ""
        }
        await sleep(2500)
      }

      if (lastError || !html || html.indexOf("problem-statement") === -1) {
        result.failed.push(key)
        result.warnings.push(key + "：" + (lastError ? lastError.message : "没拿到题面"))
        continue
      }
      fs.writeFileSync(file, html, "utf8")
      result.saved.push(key)
      process.stdout.write("  ok " + key + " " + item.name + "\n")
      await sleep(800)
    }
    tab.cdp.close()
  } finally {
    try {
      chrome.proc.kill()
    } catch (e) {
      // 忽略
    }
  }

  return result
}

/**
 * 命令行入口。
 */
async function main() {
  const argv = process.argv.slice(2)
  const outArg = argv.indexOf("--out")
  const planArg = argv.indexOf("--plan")
  const waitArg = argv.indexOf("--wait")
  const onlyArg = argv.indexOf("--only")
  const headless = argv.indexOf("--headless") !== -1

  const only = onlyArg === -1 ? [] : String(argv[onlyArg + 1] || "").split(",").filter(Boolean)

  const result = await run({
    out: outArg === -1 ? "" : argv[outArg + 1],
    plan: planArg === -1 ? "" : argv[planArg + 1],
    only: only,
    headless: headless,
    wait: waitArg === -1 ? 25000 : Number(argv[waitArg + 1]),
  })

  console.log(
    "共 " + result.total + " 道，拿到 " + result.saved.length + " 道，失败 " + result.failed.length + " 道"
  )
  result.warnings.slice(0, 30).forEach(function (line) {
    console.log("  ! " + line)
  })
}

if (require.main === module) {
  main().catch(function (e) {
    console.error("抓取失败：" + (e && e.message ? e.message : e))
    process.exit(1)
  })
}

module.exports = {
  run: run,
  main: main,
  findChrome: findChrome,
  launchChrome: launchChrome,
  waitForDevTools: waitForDevTools,
  connect: connect,
  fetchPage: fetchPage,
}
