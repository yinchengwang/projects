"use strict"
/**
 * gg-routes.test.js —— trip 与 games 两个模块的服务端路由
 *
 * 单独成文件，不改 server.test.js —— 那个文件由多个模块共用，
 * 各模块的路由断言放在各自的测试文件里更不容易互相踩。
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")
const path = require("node:path")

const { createApp } = require("../server/app.js")

/** 真实静态根（这两个模块的壳与源码都在里面） */
const WEB_ROOT = path.resolve(__dirname, "..", "web")

/** 请求一次并返回 {status, headers, body} */
function request (server, urlPath) {
  return new Promise(function (resolve, reject) {
    const addr = server.address()
    const req = http.request(
      { host: "127.0.0.1", port: addr.port, path: urlPath, method: "GET" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, headers: res.headers, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
}

/** 起一个 app 并在用例结束后关掉 */
async function withApp (t) {
  const app = createApp({ webRoot: WEB_ROOT, routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })
  return app
}

test("trip 与 games 的页面路径返回各自的 HTML 壳", async function (t) {
  const app = await withApp(t)

  const trip = await request(app.server, "/trip/pages/index")
  assert.strictEqual(trip.status, 200)
  assert.ok(trip.body.indexOf("足迹") >= 0, "trip 壳应含「足迹」")
  assert.ok(trip.body.indexOf("/trip/browser.js") >= 0, "trip 壳应引入模块入口脚本")

  const games = await request(app.server, "/games/pages/index")
  assert.strictEqual(games.status, 200)
  assert.ok(games.body.indexOf("游戏") >= 0, "games 壳应含「游戏」")
  assert.ok(games.body.indexOf("/games/browser.js") >= 0, "games 壳应引入模块入口脚本")
})

test("trip 与 games 的模块入口 302 到各自首页", async function (t) {
  const app = await withApp(t)

  const trip = await request(app.server, "/trip/")
  assert.strictEqual(trip.status, 302)
  assert.strictEqual(trip.headers.location, "/trip/pages/index")

  const games = await request(app.server, "/games/")
  assert.strictEqual(games.status, 302)
  assert.strictEqual(games.headers.location, "/games/pages/index")
})

test("带扩展名的请求落到静态托管，不被 HTML 壳盖住", async function (t) {
  const app = await withApp(t)

  const tripBrowser = await request(app.server, "/trip/browser.js")
  assert.strictEqual(tripBrowser.status, 200)
  assert.ok(tripBrowser.body.indexOf("CJS.load") >= 0, "trip 入口应是 CommonJS 加载器调用")

  const gamesBrowser = await request(app.server, "/games/browser.js")
  assert.strictEqual(gamesBrowser.status, 200)
  assert.ok(gamesBrowser.body.indexOf("CJS.load") >= 0, "games 入口应是 CommonJS 加载器调用")

  const tripPage = await request(app.server, "/trip/pages/index.js")
  assert.strictEqual(tripPage.status, 200)
  assert.ok(tripPage.body.indexOf("module.exports") >= 0, "页面模块应是 CommonJS 源码")

  const gamesPage = await request(app.server, "/games/pages/snake.js")
  assert.strictEqual(gamesPage.status, 200)
  assert.ok(gamesPage.body.indexOf("module.exports") >= 0, "页面模块应是 CommonJS 源码")

  const css = await request(app.server, "/trip/style.css")
  assert.strictEqual(css.status, 200)
  assert.ok(css.body.indexOf(".trip-page") >= 0, "样式应被静态托管")
})

test("/shared/photos.js 能被静态托管", async function (t) {
  const app = await withApp(t)
  const r = await request(app.server, "/shared/photos.js")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("module.exports") >= 0, "photos.js 应被静态托管")
})

test("不存在的 trip 页面路径仍返回壳（由前端提示页面不存在）", async function (t) {
  const app = await withApp(t)
  const r = await request(app.server, "/trip/pages/not-a-page")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("足迹") >= 0, "未知页面路径仍返回模块壳")
})
