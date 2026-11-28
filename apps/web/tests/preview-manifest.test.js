"use strict"
/**
 * preview-manifest.test.js —— 页面清单的防走样测试
 *
 * 预览页（/preview.html）的目录树来自 web/preview-manifest.js，
 * 那是从 server/preview-manifest.js 同步出来的静态副本。
 * 这里守住两道：
 *   1. 静态副本与源文件一致（同步没漏跑）
 *   2. 清单覆盖了各模块 browser.js 里 PAGES 映射的全部页面
 *      —— 新加了页面却忘了登记进预览，测试会挂
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const WEB = path.resolve(__dirname, "..")

const { buildManifest } = require(path.join(WEB, "server", "preview-manifest.js"))
const { render } = require(path.join(WEB, "preview", "sync.js"))

/** 各模块的 browser.js */
const MODULES = [
  "web/workbench/browser.js",
  "web/knowledge/browser.js",
  "web/aitimer/browser.js",
  "web/trip/browser.js",
  "web/games/browser.js"
]

/**
 * 从 browser.js 里抠出 PAGES 映射的页面路径。
 * @param {string} source browser.js 源码
 * @returns {string[]} 形如 /workbench/pages/dashboard/index
 */
function parsePages (source) {
  const block = /const PAGES = \{([\s\S]*?)\n\s*\}/.exec(source)
  if (!block) return []
  const out = []
  const re = /"(\/[^"]+?\.js)"/g
  let m
  while ((m = re.exec(block[1]))) {
    out.push(m[1].replace(/\.js$/, ""))
  }
  return out
}

/**
 * 清单里登记的全部页面路径。
 * @returns {string[]}
 */
function listedPaths () {
  const out = []
  buildManifest().forEach(function (m) {
    m.pages.forEach(function (p) { out.push(p.path) })
  })
  return out
}

test("preview：静态副本与源文件一致（同步没漏跑）", function () {
  const expected = render()
  const actual = fs.readFileSync(path.join(WEB, "web", "preview-manifest.js"), "utf8")
  assert.strictEqual(
    actual,
    expected,
    "web/preview-manifest.js 与 server/preview-manifest.js 不一致，跑 node apps/web/preview/sync.js"
  )
})

test("preview：清单覆盖各模块 browser.js 的全部页面", function () {
  const listed = listedPaths()
  MODULES.forEach(function (rel) {
    const source = fs.readFileSync(path.join(WEB, rel), "utf8")
    const pages = parsePages(source)
    assert.ok(pages.length, rel + " 里没解析出 PAGES 映射")
    pages.forEach(function (p) {
      assert.ok(
        listed.indexOf(p) >= 0,
        "页面没登记进预览清单：" + p + "（" + rel + "）—— 新加页面要同步到 server/preview-manifest.js"
      )
    })
  })
})

test("preview：清单里没有重复路径", function () {
  const seen = {}
  listedPaths().forEach(function (p) {
    assert.ok(!seen[p], "清单里路径重复：" + p)
    seen[p] = true
  })
})

test("preview：清单指向的页面文件都真实存在", function () {
  buildManifest().forEach(function (m) {
    m.pages.forEach(function (p) {
      const rel = p.path === "/" ? "web/index.html" : "web" + p.path + ".js"
      assert.ok(
        fs.existsSync(path.join(WEB, rel)),
        "清单指向的页面文件不存在：" + rel
      )
    })
  })
})

test("preview：详情页都配了 id 来源", function () {
  const keys = {
    "/workbench/pages/tech/notes/detail": "db:tech_notes",
    "/workbench/pages/tech/snippets/detail": "db:snippets",
    "/workbench/pages/tech/interview/detail": "db:interview",
    "/trip/pages/detail": "db:trips"
  }
  const html = fs.readFileSync(path.join(WEB, "web", "preview.html"), "utf8")
  Object.keys(keys).forEach(function (pagePath) {
    assert.ok(
      html.indexOf('"' + pagePath + '": "' + keys[pagePath] + '"') >= 0,
      "preview.html 里没给 " + pagePath + " 配 id 来源 " + keys[pagePath]
    )
  })
})

test("preview：清单里的详情页都登记了 detail 标记", function () {
  buildManifest().forEach(function (m) {
    m.pages.forEach(function (p) {
      if (/\/detail$/.test(p.path)) {
        assert.ok(p.detail, p.path + " 是详情页，得标 detail 才能解析真实 id")
      }
    })
  })
})
