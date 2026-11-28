"use strict"
/**
 * aitimer-view.test.js —— aitimer 模块的视图层
 *
 * 覆盖三件事：
 * 1. 三个页面模块能加载、能渲染、渲染结果不含 undefined/NaN；
 * 2. md-block 的三道处理（wikilink 归一 / 危险协议剥离 / 纯文本提取）有效；
 * 3. 概览页的统计口径正确，目录缺失时给出空态。
 *
 * 页面要 fetch 接口，测试里把 global.fetch 换成桩。
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")

const dom = require("./dom-stub.js")
dom.install()

const mdBlockModule = require("../web/aitimer/md-block.js")
const md2html = require("../shared/core/md2html.js").md2html

const PAGES = ["pages/index", "pages/series", "pages/doc"]

/** 构造一份最小的索引数据 */
function makeIndex () {
  return {
    ok: true,
    exists: true,
    root: "/fake/ai-timer",
    collections: [
      {
        id: "briefing",
        name: "AI 简报",
        desc: "每日简报",
        docs: [
          { id: "briefing/2026-11-28", title: "日报 1128", date: "2026-11-28", size: 100, meta: { stack: "db" }, isIndex: false },
          { id: "briefing/2026-11-27", title: "日报 1127", date: "2026-11-27", size: 100, meta: {}, isIndex: false }
        ]
      },
      {
        id: "inspection",
        name: "代码仓巡检",
        desc: "每日巡检",
        docs: [
          { id: "inspection/2026-11-28", title: "巡检 1128", date: "2026-11-28", size: 100, meta: {}, isIndex: false }
        ]
      },
      {
        id: "learn",
        name: "每日一特性",
        desc: "深度系列",
        docs: [
          {
            id: "db",
            name: "数据库",
            desc: "**主线根基**：存储引擎",
            docs: [
              { id: "learn/db/index", title: "数据库系列", date: "2026-11-28", size: 10, meta: {}, isIndex: true },
              { id: "learn/db/pg-hot-update", title: "HOT 更新", date: "2026-11-26", size: 20, meta: { difficulty: "hard", status: "done", tags: ["theory"] }, isIndex: false }
            ]
          }
        ]
      }
    ]
  }
}

/**
 * 装一个假的 fetch。
 * @param {object} routes 路径 → 返回体
 * @returns {Function} 还原函数
 */
function stubFetch (routes) {
  const previous = global.fetch
  global.fetch = function (url) {
    const key = String(url).split("?")[0]
    if (routes[key] === undefined) {
      return Promise.resolve({ ok: false, status: 404, json: function () { return Promise.resolve({ ok: false, error: "未命中桩" }) } })
    }
    const body = routes[key]
    return Promise.resolve({
      ok: true,
      status: 200,
      json: function () { return Promise.resolve(body) }
    })
  }
  return function () {
    if (previous === undefined) delete global.fetch
    else global.fetch = previous
  }
}

/** 加载页面模块并取回页面配置 */
function loadPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "aitimer", route + ".js")
  let config = null
  global.__AITIMER_PAGE__ = function (o) { config = o }
  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__AITIMER_PAGE__
  }
  if (!config) throw new Error("页面未调用 __AITIMER_PAGE__(): " + route)
  return config
}

/** 构造可直接调用的页面实例（与浏览器入口一致：补 setData） */
function createPage (route) {
  const config = loadPage(route)
  const inst = {}
  Object.keys(config).forEach(function (key) { inst[key] = config[key] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)
  inst.setData = function (patch) {
    Object.keys(patch || {}).forEach(function (key) { inst.state[key] = patch[key] })
  }
  return inst
}

test("3 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都产出节点且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("渲染结果不含 undefined / NaN", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const text = page.render(page.state).textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 160))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 160))
  })
})

test("md-block：wikilink 被归一成普通文本", function () {
  assert.strictEqual(mdBlockModule.stripWikiLinks("见 [[pg-hot-update]] 的说明"), "见 pg-hot-update 的说明")
  assert.strictEqual(mdBlockModule.stripWikiLinks("没有 wikilink"), "没有 wikilink")
})

test("md-block：plainText 剥掉 Markdown 标记", function () {
  assert.strictEqual(mdBlockModule.plainText("**主线根基**：存储引擎"), "主线根基：存储引擎")
  assert.strictEqual(mdBlockModule.plainText("`行内码` 与 *斜体*"), "行内码 与 斜体")
  assert.strictEqual(mdBlockModule.plainText("[链接](https://x.dev)"), "链接")
  assert.strictEqual(mdBlockModule.plainText("[[wikilink]]"), "wikilink")
})

test("md-block：危险协议的 href 被剥离", function () {
  const node = mdBlockModule.mdBlock("[点我](javascript:alert(1))", md2html)
  // DOM 桩没有真正的 querySelectorAll，锚点不会被遍历 —— 这里验证不抛异常，
  // 真正的协议剥离在浏览器里由 hardenLinks 负责（见下条直接测 hardenLinks）。
  assert.ok(node)
  assert.strictEqual(node.className, "aitimer-md")
})

test("md-block：hardenLinks 剥掉 javascript: 并给外链补 target", function () {
  // 造一个带 querySelectorAll 的假容器，验证 hardenLinks 的判定逻辑
  const anchors = [
    { attrs: { href: "javascript:alert(1)" }, removed: [], set: {} },
    { attrs: { href: "https://example.com/a" }, removed: [], set: {} },
    { attrs: { href: "/relative/path" }, removed: [], set: {} }
  ]
  const container = {
    querySelectorAll: function () {
      return anchors.map(function (a) {
        return {
          getAttribute: function (k) { return a.attrs[k] === undefined ? null : a.attrs[k] },
          removeAttribute: function (k) { a.removed.push(k); delete a.attrs[k] },
          setAttribute: function (k, v) { a.set[k] = v }
        }
      })
    }
  }
  mdBlockModule.hardenLinks(container)

  assert.ok(anchors[0].removed.indexOf("href") >= 0, "javascript: 的 href 应被剥掉")
  assert.strictEqual(anchors[0].set.target, undefined, "危险链接不应补 target")
  assert.strictEqual(anchors[1].set.target, "_blank", "外链应开新标签")
  assert.strictEqual(anchors[1].set.rel, "noopener noreferrer")
  assert.strictEqual(anchors[2].set.target, undefined, "站内相对链接保持同窗口")
})

test("概览页：统计口径正确", async function (t) {
  const restore = stubFetch({ "/api/aitimer/index": makeIndex() })
  t.after(restore)

  const page = createPage("pages/index")
  await page.refresh()
  assert.strictEqual(page.state.stats.briefing, 2)
  assert.strictEqual(page.state.stats.inspection, 1)
  assert.strictEqual(page.state.stats.series, 1)
  assert.strictEqual(page.state.stats.articles, 2, "系列内的文档数应计入文章数")

  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("AI 简报") >= 0)
  assert.ok(text.indexOf("代码仓巡检") >= 0)
  assert.ok(text.indexOf("每日一特性") >= 0)
})

test("概览页：目录缺失时给空态而不是报错", async function (t) {
  const restore = stubFetch({
    "/api/aitimer/index": { ok: true, exists: false, root: "/x", collections: [] }
  })
  t.after(restore)

  const page = createPage("pages/index")
  await page.refresh()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("未找到 ai-timer 目录") >= 0, "应提示目录缺失，实际：" + text.slice(0, 160))
})

test("概览页：接口失败时显示错误文案", async function (t) {
  const restore = stubFetch({})
  t.after(restore)

  const page = createPage("pages/index")
  await page.refresh()
  assert.ok(page.state.error, "应记录错误")
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("产物加载失败") >= 0)
})

test("系列页：按系列 id 找到系列并列出文章", async function (t) {
  const restore = stubFetch({ "/api/aitimer/index": makeIndex() })
  t.after(restore)

  const page = createPage("pages/series")
  page.state.seriesId = "db"
  await page.refresh()
  assert.ok(page.state.series, "应找到系列")
  assert.strictEqual(page.state.docs.length, 2)

  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("数据库") >= 0)
  assert.ok(text.indexOf("HOT 更新") >= 0)
  assert.ok(text.indexOf("系列首页") >= 0, "README 应标「系列首页」")
  assert.ok(text.indexOf("困难") >= 0, "difficulty 应中文化")
})

test("系列页：系列名里的 Markdown 标记被剥掉", async function (t) {
  const restore = stubFetch({ "/api/aitimer/index": makeIndex() })
  t.after(restore)

  const page = createPage("pages/series")
  page.state.seriesId = "db"
  await page.refresh()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("**") < 0, "系列简介不该露出 Markdown 星号：" + text.slice(0, 160))
})

test("系列页：找不到系列时给出提示", async function (t) {
  const restore = stubFetch({ "/api/aitimer/index": makeIndex() })
  t.after(restore)

  const page = createPage("pages/series")
  page.state.seriesId = "no-such-series"
  await page.refresh()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("找不到该领域系列") >= 0)
})

test("文档页：渲染正文并算出前后篇", async function (t) {
  const restore = stubFetch({
    "/api/aitimer/index": makeIndex(),
    "/api/aitimer/doc": {
      ok: true,
      doc: {
        id: "learn/db/pg-hot-update",
        title: "HOT 更新",
        date: "2026-11-26",
        size: 2048,
        meta: { stack: "db", difficulty: "hard", status: "done", tags: ["theory"], links: ["https://example.com/a"] },
        body: "# HOT 更新\n\n正文里有 [[wikilink]] 与 [外链](https://example.com/b)。\n",
        path: "/x/y.md"
      }
    }
  })
  t.after(restore)

  const page = createPage("pages/doc")
  page.state.docId = "learn/db/pg-hot-update"
  await page.refresh()

  assert.ok(page.state.doc, "应读到文档")
  assert.strictEqual(page.state.collectionName, "每日一特性")
  assert.strictEqual(page.state.prev.id, "learn/db/index", "上一篇应是系列首页")
  assert.strictEqual(page.state.next, null, "该篇是系列末篇，没有下一篇")

  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("HOT 更新") >= 0)
  assert.ok(text.indexOf("2.0 KB") >= 0, "应显示体积：" + text.slice(0, 160))
  assert.ok(text.indexOf("https://example.com/a") >= 0, "参考链接应列出")
})

test("文档页：缺 id 时给出中文提示", async function () {
  const page = createPage("pages/doc")
  page.state.docId = ""
  await page.refresh()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("缺少文档 id") >= 0)
})
