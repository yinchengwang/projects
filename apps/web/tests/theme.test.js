"use strict"
/**
 * theme.test.js —— 主题解析与 Web 侧应用
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

/** Node 下的 localStorage 内存桩，供 storage.js / with-theme.js 测试 */
const memory = new Map()
global.localStorage = {
  getItem: function (key) { return memory.has(key) ? memory.get(key) : null },
  setItem: function (key, value) { memory.set(key, String(value)) },
  removeItem: function (key) { memory.delete(key) },
  key: function (index) {
    const keys = Array.from(memory.keys())
    return index < keys.length ? keys[index] : null
  },
  clear: function () { memory.clear() },
  get length () { return memory.size }
}

/**
 * Node 下没有 window.matchMedia，装一个最小桩。
 * theme.onSystemThemeChange 依赖 window.matchMedia(...).addEventListener。
 */
function installWindowStub () {
  global.window = {
    matchMedia: function () {
      return {
        matches: false,
        addEventListener: function () {}
      }
    }
  }
}

installWindowStub()

const theme = require("../shared/core/theme.js")

test("resolve：显式 light/dark 直接返回", function () {
  assert.strictEqual(theme.resolve("light"), "light")
  assert.strictEqual(theme.resolve("dark"), "dark")
})

test("resolve：auto 落到系统主题", function () {
  assert.ok(theme.resolve("auto") === "light" || theme.resolve("auto") === "dark")
})

test("build：返回 themeClass / isDark / brand / brandSoft", function () {
  const t = theme.build({ theme: "light", primaryColor: "sky" })
  assert.strictEqual(t.themeClass, "theme-light")
  assert.strictEqual(t.isDark, false)
  assert.ok(typeof t.brand === "string" && t.brand.charAt(0) === "#")
  assert.ok(t.brandSoft.indexOf("rgba") === 0)
})

test("build：dark 模式返回 theme-dark", function () {
  const t = theme.build({ theme: "dark", primaryColor: "sky" })
  assert.strictEqual(t.themeClass, "theme-dark")
  assert.strictEqual(t.isDark, true)
})

test("build：缺设置时用默认值，不抛异常", function () {
  const t = theme.build(null)
  assert.ok(t.themeClass === "theme-light" || t.themeClass === "theme-dark")
})

test("hexToRgba：短 hex 与长 hex 都对", function () {
  assert.strictEqual(theme.hexToRgba("#0284C7", 0.5), "rgba(2,132,199,0.5)")
  assert.strictEqual(theme.hexToRgba("#fff", 1), "rgba(255,255,255,1)")
})

test("primaryOf：未知 key 回退到第一个主色", function () {
  // PRIMARY_COLORS 的形状是 {key, label, light, dark}，没有 color 字段
  assert.strictEqual(theme.primaryOf("不存在的颜色").key, "sky")
  assert.strictEqual(theme.primaryOf("sky").key, "sky")
  assert.strictEqual(theme.primaryOf("violet").key, "violet")
  assert.ok(theme.primaryOf("sky").light.charAt(0) === "#")
  assert.ok(theme.primaryOf("sky").dark.charAt(0) === "#")
})

test("applyThemeClass：把主题类名写到根元素上", function () {
  const root = document.createElement("div")
  theme.applyThemeClass(root, theme.build({ theme: "dark" }))
  assert.ok(root.className.indexOf("theme-dark") >= 0, "根元素类名应是：" + root.className)
})

test("onSystemThemeChange：注册成功返回 true", function () {
  assert.strictEqual(theme.onSystemThemeChange(function () {}), true)
})

test("readSettings：可指定存储键，缺省仍读 db:settings", function () {
  const storage = require("../shared/core/storage.js")
  const { readSettings } = require("../shared/core/with-theme.js")

  storage.set("wb_settings", { theme: "dark", primaryColor: "violet" })
  storage.set(storage.KEYS.settings, { theme: "light", primaryColor: "sky" })

  const workbench = readSettings("wb_settings")
  assert.strictEqual(workbench.theme, "dark")
  assert.strictEqual(workbench.primaryColor, "violet")

  const knowledge = readSettings()
  assert.strictEqual(knowledge.theme, "light")
  assert.strictEqual(knowledge.primaryColor, "sky")
})

test("withTheme：settingsKey 贯穿挂载与手动刷新", function () {
  const storage = require("../shared/core/storage.js")
  const { withTheme } = require("../shared/core/with-theme.js")
  storage.set("wb_settings", { theme: "dark", primaryColor: "violet" })

  const page = withTheme({ data: {}, render: function (state) { return state } }, "wb_settings")
  page.state = {}
  page.onMount(document.body)
  assert.strictEqual(page.state.isDark, true)
  assert.strictEqual(page.state.brand, theme.primaryOf("violet").dark)

  storage.set("wb_settings", { theme: "light", primaryColor: "sky" })
  page.refreshTheme()
  assert.strictEqual(page.state.isDark, false)
  assert.strictEqual(page.state.brand, theme.primaryOf("sky").light)
})
