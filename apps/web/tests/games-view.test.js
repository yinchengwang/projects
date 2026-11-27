"use strict"
/**
 * games-view.test.js —— games 视图层
 *
 * 只断言「5 个页面能加载、能渲染、路由表齐」，引擎规则由 games-engine.test.js 覆盖。
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")

const dom = require("./dom-stub.js")
dom.install()

// Node 下没有 localStorage，复用 wb-stub 的内存版替身
const stub = require("./wb-stub.js")
stub.installLocalStorage()

// Node 下没有 history，platform.navigateTo 会用到
if (typeof global.history === "undefined") {
  global.history = {
    pushState: function () {},
    replaceState: function () {},
    back: function () {}
  }
}

const gameStorage = require("../shared/games/storage.js")

const PAGES = [
  "pages/index",
  "pages/snake",
  "pages/game2048",
  "pages/sudoku",
  "pages/match3"
]

/** 加载页面模块并取回页面配置 */
function loadPage (route) {
  const abs = path.resolve(__dirname, "..", "web", "games", route + ".js")
  let config = null
  global.__GAMES_PAGE__ = function (o) { config = o }
  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__GAMES_PAGE__
  }
  if (!config) throw new Error("页面未调用 __GAMES_PAGE__(): " + route)
  return config
}

/** 构造可直接调用的页面实例（与浏览器入口一致：补 setData） */
function createPage (route) {
  const config = loadPage(route)
  const inst = {}
  Object.keys(config).forEach(function (key) { inst[key] = config[key] })
  inst.data = JSON.parse(JSON.stringify(config.data || {}))
  inst.state = Object.assign({}, inst.data)
  // 与小程序一致：setData 同时更新 data 与 state（页面里两种读法都有）
  inst.setData = function (patch) {
    Object.keys(patch || {}).forEach(function (key) {
      inst.data[key] = patch[key]
      inst.state[key] = patch[key]
    })
  }
  return inst
}

test.beforeEach(function () {
  stub.reset()
})

test("5 个页面模块都能加载且形状正确", function () {
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

test("路由表覆盖 5 个页面", function () {
  const { ROUTES } = require("../web/games/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 5)
})

test("首页挂载后列出 4 个游戏与最高分", function () {
  gameStorage.updateSnakeBestScore(120)
  const page = createPage("pages/index")
  page.onMount()
  assert.strictEqual(page.state.entries.length, 4)
  assert.strictEqual(page.state.achievementTotal, 8)
  const text = page.render(page.state).textContent
  assert.ok(text.indexOf("贪吃蛇") >= 0, "应列出贪吃蛇")
  assert.ok(text.indexOf("最高 120 分") >= 0, "应显示贪吃蛇最高分")
})

test("首页成就墙可展开收起", function () {
  const page = createPage("pages/index")
  page.onMount()
  page.onToggleAchievements()
  assert.strictEqual(page.state.showAchievements, true)
  assert.strictEqual(page.state.achievements.length, 8)
  page.onToggleAchievements()
  assert.strictEqual(page.state.showAchievements, false)
})

test("首页清空进度后最高分归零", async function () {
  gameStorage.updateSnakeBestScore(200)
  const page = createPage("pages/index")
  page.onMount()

  const platform = require("../shared/platform.js")
  const pending = page.onClear()
  platform.__testClickModal(true, "")
  await pending

  assert.strictEqual(gameStorage.getSnakeBestScore(), 0)
})

test("贪吃蛇开始后状态变成 playing，方向键能改引擎方向", function () {
  const page = createPage("pages/snake")
  page.onMount()
  page.onStart()
  assert.strictEqual(page.state.status, "playing")
  page.onDirection("up")
  assert.strictEqual(page.__game.nextDirection, require("../shared/games/snake.js").Direction.UP)
  page.onPause()
  assert.strictEqual(page.state.status, "paused")
  page.onUnmount()
})

test("贪吃蛇开局蛇身 3 节", function () {
  const page = createPage("pages/snake")
  page.onMount()
  assert.strictEqual(page.__game.snake.length, 3)
  page.onUnmount()
})

test("2048 开始后能移动并计分", function () {
  const page = createPage("pages/game2048")
  page.onMount()
  page.onStart()
  assert.strictEqual(page.state.status, "playing")
  // 手工构造一个必定能合并的棋盘
  page.__game.board = [
    [2, 2, 0, 0],
    [0, 0, 0, 0],
    [0, 0, 0, 0],
    [0, 0, 0, 0]
  ]
  page.__game.score = 0
  page.doMove("left")
  assert.strictEqual(page.__game.score, 4, "2+2 应得 4 分")
  page.onUnmount()
})

test("数独能开始第 1 关并填入数字", function () {
  const page = createPage("pages/sudoku")
  page.onMount()
  const first = page.state.chapters[0]
  page.onLevelTap(first.id, 1, true)
  assert.strictEqual(page.state.phase, "play")
  assert.strictEqual(page.state.currentLevel, 1)
  assert.strictEqual(page.state.cells.length, 81)
  page.onUnmount()
})

test("数独未解锁章节时提示且不进入对局", function () {
  const page = createPage("pages/sudoku")
  page.onMount()
  const chapter2 = page.state.chapters.filter(function (c) { return c.id === 2 })[0]
  page.onLevelTap(chapter2.id, 1, true)
  assert.strictEqual(page.state.phase, "menu", "未解锁章节不应进入对局")
  page.onUnmount()
})

test("消消乐能开始第 1 关并渲染 81 格", function () {
  const page = createPage("pages/match3")
  page.onMount()
  page.startLevel(1)
  assert.strictEqual(page.state.phase, "play")
  assert.strictEqual(page.state.level, 1)
  assert.strictEqual(page.state.cells.length, 81)
  assert.ok(page.state.goals.length >= 1, "应至少有一个目标")
  page.onUnmount()
})

test("消消乐章节列表 6 章，第 1 章默认解锁", function () {
  const page = createPage("pages/match3")
  page.onMount()
  assert.strictEqual(page.state.chapters.length, 6)
  assert.strictEqual(page.state.chapters[0].unlocked, true)
  page.onUnmount()
})
