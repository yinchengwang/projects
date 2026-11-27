"use strict"
/**
 * kh-routes.test.js —— knowledge 模块的服务端路由
 *
 * 单独成文件，不改 server.test.js —— 那个文件由多个模块共用，
 * 各模块的路由断言放在各自的测试文件里更不容易互相踩。
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")
const path = require("node:path")

const { createApp } = require("../server/app.js")

/** 真实静态根（壳与源码都在里面） */
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

test("knowledge 的两级与三级页面路径都返回 HTML 壳", async function (t) {
  const app = await withApp(t)

  const three = await request(app.server, "/knowledge/pages/knowledge/graph/index")
  assert.strictEqual(three.status, 200)
  assert.ok(three.body.indexOf("Reading Radar") >= 0, "knowledge 壳应含「Reading Radar」")
  assert.ok(three.body.indexOf("/knowledge/browser.js") >= 0, "壳应引入模块入口脚本")

  const two = await request(app.server, "/knowledge/pages/dashboard/index")
  assert.strictEqual(two.status, 200)
  assert.ok(two.body.indexOf("app-main") >= 0, "壳应含挂载点")
})

test("knowledge 模块入口 302 到首页", async function (t) {
  const app = await withApp(t)

  const entry = await request(app.server, "/knowledge/")
  assert.strictEqual(entry.status, 302)
  assert.strictEqual(entry.headers.location, "/knowledge/pages/dashboard/index")

  const bare = await request(app.server, "/knowledge")
  assert.strictEqual(bare.status, 302)
})

test("带扩展名的请求落到静态托管，不被 HTML 壳盖住", async function (t) {
  const app = await withApp(t)

  const browser = await request(app.server, "/knowledge/browser.js")
  assert.strictEqual(browser.status, 200)
  assert.ok(browser.body.indexOf("CJS.load") >= 0, "入口应是 CommonJS 加载器调用")

  const page = await request(app.server, "/knowledge/pages/learn/quiz/index.js")
  assert.strictEqual(page.status, 200)
  assert.ok(page.body.indexOf("module.exports") >= 0, "页面模块应是 CommonJS 源码")

  const css = await request(app.server, "/knowledge/style.css")
  assert.strictEqual(css.status, 200)
  assert.ok(css.body.indexOf(".kh-page") >= 0, "样式应被静态托管")
})

test("数据层与布局模块能被静态托管", async function (t) {
  const app = await withApp(t)

  const stores = await request(app.server, "/shared/knowledge/stores.js")
  assert.strictEqual(stores.status, 200)
  assert.ok(stores.body.indexOf("module.exports") >= 0)

  const graphLayout = await request(app.server, "/shared/knowledge/graph-layout.js")
  assert.strictEqual(graphLayout.status, 200)

  const radarLayout = await request(app.server, "/shared/knowledge/radar-layout.js")
  assert.strictEqual(radarLayout.status, 200)
})

test("不存在的 knowledge 页面路径仍返回壳（由前端提示页面不存在）", async function (t) {
  const app = await withApp(t)
  const r = await request(app.server, "/knowledge/pages/not-a-page")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("Reading Radar") >= 0, "未知页面路径仍返回模块壳")
})
