"use strict"
/**
 * kh-view.test.js —— 视图层：17 个页面能加载、能渲染，两个 Canvas 组件能加载
 *
 * 注意：本文件**不替代**迁入的 181 个断言，只补「渲染出来了吗」这一层。
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const h = require("./kh-harness.js")
const stub = require("./kh-stub.js")

stub.installLocalStorage()

/** 17 个页面路由 */
const PAGES = [
  "pages/dashboard/index",
  "pages/learn/index",
  "pages/learn/quiz/index",
  "pages/learn/review/index",
  "pages/learn/deep/index",
  "pages/learn/path/index",
  "pages/interview/index",
  "pages/interview/tracker/index",
  "pages/interview/bank/index",
  "pages/interview/mock/index",
  "pages/knowledge/index",
  "pages/knowledge/graph/index",
  "pages/knowledge/excerpt/index",
  "pages/knowledge/radar/index",
  "pages/profile/index",
  "pages/plan/five-year/index",
  "pages/plan/roadmap/index"
]

function openPage (route) {
  const page = h.createPage(route)
  page.onLoad()
  page.state = Object.assign({}, page.data)
  return page
}

test("17 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = h.createPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都产出节点，且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = openPage(route)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("渲染结果不含 undefined / NaN", function () {
  PAGES.forEach(function (route) {
    const page = openPage(route)
    const text = page.render(page.state).textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 17 个页面", function () {
  const router = require("../web/knowledge/router.js")
  assert.strictEqual(Object.keys(router.ROUTES).length, 17)
  assert.strictEqual(router.resolve("/knowledge/pages/learn/quiz/index"), "/pages/learn/quiz/index")
  assert.strictEqual(router.resolve("/workbench/pages/dashboard/index"), null)
  assert.strictEqual(router.resolve("/knowledge/pages/nope/index"), null)
})

test("知识图谱组件：布局计算产出坐标", function () {
  const gl = require("../shared/knowledge/graph-layout.js")
  // 节点形状：{id, name, domain, level}；domain 是中文领域名（决定列与配色）
  const nodes = [
    { id: "a", name: "A", domain: "算法", level: 1 },
    { id: "b", name: "B", domain: "系统设计", level: 2 }
  ]
  // 链接形状：{source, target}（不是 from/to —— 布局内部才转成 from/to）
  const links = [{ source: "a", target: "b" }]
  const geo = gl.layout(nodes, links, { width: 300, height: 200 })
  assert.strictEqual(geo.points.length, 2)
  assert.strictEqual(geo.edges.length, 1)
  geo.points.forEach(function (p) {
    assert.strictEqual(typeof p.x, "number")
    assert.strictEqual(typeof p.y, "number")
    assert.ok(p.x >= 0 && p.x <= 300, "x 应在画布内，实际 " + p.x)
    assert.ok(p.y >= 0 && p.y <= 200, "y 应在画布内，实际 " + p.y)
  })
})

test("技术雷达组件：象限气泡与栈多边形都能算出来", function () {
  const radar = require("../shared/knowledge/radar-layout.js")
  // 象限气泡的项形状：{id, title, quadrant, ring, recommend}
  const items = [{ id: "x", title: "C++", quadrant: "language", ring: "basic", recommend: 2 }]
  const geo = radar.quadrantBubbles(items, { width: 300, height: 300 })
  assert.strictEqual(geo.points.length, 1)
  assert.ok(geo.points[0].x >= 0 && geo.points[0].x <= 300)
  // 栈多边形的行形状：{key, label, color, percent}
  const stack = radar.stackPolygon(
    [{ key: "lang", label: "语言", color: "#0284C7", percent: 70 }],
    { width: 300, height: 300 }
  )
  assert.ok(Array.isArray(stack.axes))
  assert.ok(stack.axes.length >= 1)
  assert.ok(Array.isArray(stack.points))
})

test("两个 Canvas 组件都能加载，且 draw 方法存在", function () {
  const graph = h.createComponent("components/knowledge-graph")
  assert.strictEqual(typeof graph.draw, "function")
  assert.strictEqual(typeof graph.attachCanvas, "function")
  assert.strictEqual(typeof graph.destroy, "function")
  const radar = h.createComponent("components/radar-chart")
  assert.strictEqual(typeof radar.draw, "function")
  assert.strictEqual(typeof radar.attachCanvas, "function")
  assert.strictEqual(typeof radar.destroy, "function")
})

test("Canvas 组件在没有 2d 上下文的环境下 draw 不抛异常", function () {
  const graph = h.createComponent("components/knowledge-graph", {
    nodes: [{ id: "a", name: "A", domain: "算法", level: 1 }],
    links: []
  })
  // Node 的 dom-stub 里 canvas 没有 getContext，attachCanvas 应静默返回
  const fakeCanvas = { getContext: undefined }
  graph.attachCanvas(fakeCanvas)
  graph.draw()
  graph.destroy()
})

test("两个 Canvas 页面的 render 里建了 canvas 且暴露 attachCanvas", function () {
  ;["pages/knowledge/graph/index", "pages/knowledge/radar/index"].forEach(function (route) {
    const page = openPage(route)
    page.render(page.state)
    assert.strictEqual(typeof page.attachCanvas, "function", route + " 应暴露 attachCanvas()")
    page.attachCanvas() // 桩环境下静默返回
  })
})
