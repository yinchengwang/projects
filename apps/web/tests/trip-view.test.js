"use strict"
/**
 * trip-view.test.js —— trip 视图层
 *
 * 只断言「页面能加载、能渲染、路由表齐」，业务逻辑由 trip-store / trip-export 覆盖。
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")

const dom = require("./dom-stub.js")
dom.install()

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

// Node 下没有 localStorage，复用 wb-stub 的内存版替身
require("./wb-stub.js").installLocalStorage()

// Node 下没有 history，platform.navigateTo 会用到 —— 装个最小替身
if (typeof global.history === "undefined") {
  global.history = {
    pushState: function () {},
    replaceState: function () {},
    back: function () {}
  }
}

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")
const store = require("../shared/trip/store.js")

const PAGES = [
  "pages/index",
  "pages/new-trip",
  "pages/detail"
]

/** 加载页面模块并取回页面配置 */
function loadPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "trip", route + ".js")
  let config = null
  global.__TRIP_PAGE__ = function (o) { config = o }
  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__TRIP_PAGE__
  }
  if (!config) throw new Error("页面未调用 __TRIP_PAGE__(): " + route)
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

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

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
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 3 个页面", function () {
  const { ROUTES } = require("../web/trip/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 3)
})

test("列表页挂载后能读出已存的记录并分组", async function () {
  await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  const page = createPage("pages/index")
  const done = page.onMount()
  if (done && typeof done.then === "function") await done
  assert.strictEqual(page.state.total, 1)
  assert.strictEqual(page.state.groups.length, 1)
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("西湖") >= 0, "列表应渲染出标题")
  assert.ok(text.indexOf("杭州") >= 0, "列表应渲染出地点")
})

test("列表页空态给出提示", function () {
  const page = createPage("pages/index")
  page.refresh()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("还没有足迹") >= 0, "空态应提示还没有足迹")
})

test("详情页取不到记录时显示空态", async function () {
  const page = createPage("pages/detail")
  page.state.id = "不存在"
  await page.onMount()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("记录不存在或已删除") >= 0, "应显示空态")
})

test("详情页能读到记录并渲染日期与备注", async function () {
  const r = await store.add({
    title: "带备注",
    date: "2026-11-20",
    location: "杭州",
    notes: "写了点什么",
    tags: ["出差"]
  })
  const page = createPage("pages/detail")
  page.state.id = r.id
  await page.onMount()
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("2026年11月20日") >= 0, "应渲染出中文日期")
  assert.ok(text.indexOf("写了点什么") >= 0, "应渲染出备注")
  assert.ok(text.indexOf("出差") >= 0, "应渲染出标签")
})

test("新增页保存后写入数据层", async function () {
  const page = createPage("pages/new-trip")
  page.onMount()
  page.onInput("title", "新记录")
  page.onInput("date", "2026-11-21")
  page.onInput("tagsText", "出差，亲子")
  await page.onSave()
  const list = await store.list()
  assert.strictEqual(list.length, 1)
  assert.strictEqual(list[0].title, "新记录")
  assert.deepStrictEqual(list[0].tags, ["出差", "亲子"])
})

test("新增页缺标题时不写入", async function () {
  const page = createPage("pages/new-trip")
  page.onMount()
  page.onInput("date", "2026-11-21")
  await page.onSave()
  assert.strictEqual((await store.list()).length, 0)
})

test("新增页超过 9 张时拒绝继续添加", async function () {
  const page = createPage("pages/new-trip")
  page.onMount()
  page.setData({ files: new Array(9).fill({ path: "/x.png", size: 1, file: {} }) })
  await page.onAddPhotos()
  assert.strictEqual(page.state.files.length, 9)
})

test("详情页删除后数据层与照片都被清掉", async function () {
  const r = await store.add({
    title: "待删",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG" } }]
  })
  const page = createPage("pages/detail")
  page.state.id = r.id
  await page.onMount()

  // 直接确认删除（platform.showModal 在 Node 下走 DOM 桩，用测试钩子确认）
  const platform = require("../shared/platform.js")
  const pending = page.onDelete()
  platform.__testClickModal(true, "")
  await pending

  assert.strictEqual(await store.get(r.id), null, "记录应被删除")
  assert.strictEqual(await photos.get(r.id, "p1"), null, "照片应被删除")
})
