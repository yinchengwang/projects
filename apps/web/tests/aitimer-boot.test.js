"use strict"
/**
 * aitimer-boot.test.js —— aitimer 模块的浏览器加载链
 *
 * 这个测试模拟浏览器侧 cjs-loader 的行为（fetch 源码 → new Function 包装 →
 * require 指向本地加载器），跑一遍 browser.js 声明的 PRELOAD 与三张页面。
 *
 * 存在的理由：cjs-loader 的同步 require **只认已载入缓存的模块**。
 * 页面里 require 了某个模块、而 browser.js 的 PRELOAD 漏了它，
 * 在浏览器里会抛「同步 require 拿到未加载的模块」，但 Node 下 require 直接读盘，
 * 单测根本发现不了 —— 必须模拟浏览器加载链才测得出来。
 *
 * 与其它模块同构（knowledge 的 md-block.js 也要预载）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const ROOT = path.resolve(__dirname, "..")
const WEB_ROOT = path.join(ROOT, "web")

/**
 * 把 URL 路径映射到磁盘文件。
 * /shared/* 在 apps/web/shared/，其余在 apps/web/web/。
 * @param {string} urlPath
 * @returns {string}
 */
function diskPath (urlPath) {
  if (urlPath.indexOf("/shared/") === 0) {
    return path.join(ROOT, urlPath.replace(/^\//, ""))
  }
  return path.join(WEB_ROOT, urlPath.replace(/^\//, ""))
}

/**
 * 解析模块请求路径（与 cjs-loader 同规则）。
 * @param {string} fromDir
 * @param {string} request
 * @returns {string}
 */
function resolvePath (fromDir, request) {
  if (request.charAt(0) === "/") return request
  const parts = (fromDir + "/" + request).split("/")
  const out = []
  for (let i = 0; i < parts.length; i += 1) {
    const p = parts[i]
    if (p === "" || p === ".") continue
    if (p === "..") { out.pop(); continue }
    out.push(p)
  }
  return "/" + out.join("/")
}

/**
 * 造一个模拟 cjs-loader 的加载器。
 * @returns {{load: Function, cache: object}}
 */
function makeLoader () {
  const cache = {}
  function load (absPath) {
    if (cache[absPath]) return Promise.resolve(cache[absPath])
    const src = fs.readFileSync(diskPath(absPath), "utf8")
    return Promise.resolve().then(function () {
      const module = { exports: {} }
      const dir = absPath.slice(0, absPath.lastIndexOf("/"))
      const localRequire = function (request) {
        const target = resolvePath(dir, request)
        if (cache[target]) return cache[target]
        throw new Error("同步 require 拿到未加载的模块：" + target + "（请检查预载顺序）")
      }
      const factory = new Function("require", "module", "exports", src + "\n//# sourceURL=" + absPath)
      factory(localRequire, module, module.exports)
      cache[absPath] = module.exports
      return module.exports
    })
  }
  return { load: load, cache: cache }
}

/**
 * 从 browser.js 源码里读出 PRELOAD 与 PAGES 两个数组。
 * @param {string} rel 模块内相对路径，如 "aitimer/browser.js"
 * @returns {{preload: string[], pages: object}}
 */
function readManifest (rel) {
  const src = fs.readFileSync(path.join(WEB_ROOT, rel), "utf8")

  const preloadMatch = /const PRELOAD = \[([\s\S]*?)\]/.exec(src)
  assert.ok(preloadMatch, rel + " 里应能读出 PRELOAD")
  const preload = (preloadMatch[1].match(/"([^"]+)"/g) || []).map(function (s) {
    return s.slice(1, -1)
  })

  const pagesMatch = /const PAGES = \{([\s\S]*?)\n  \}/.exec(src)
  assert.ok(pagesMatch, rel + " 里应能读出 PAGES")
  const pages = {}
  const entryRe = /"([^"]+)":\s*"([^"]+)"/g
  let m = entryRe.exec(pagesMatch[1])
  while (m) {
    pages[m[1]] = m[2]
    m = entryRe.exec(pagesMatch[1])
  }

  return { preload: preload, pages: pages }
}

test("aitimer：browser.js 的 PRELOAD 覆盖全部页面依赖", async function () {
  const manifest = readManifest("aitimer/browser.js")
  const loader = makeLoader()

  // 预载
  for (const p of manifest.preload) {
    await loader.load(p)
  }

  // 三张页面都要能加载：页面里 require 的每个模块都得已在缓存里
  for (const key of Object.keys(manifest.pages)) {
    const file = manifest.pages[key]
    const page = await loader.load(file)
    assert.strictEqual(typeof page.render, "function", key + " 应导出 render()")
  }
})

test("aitimer：页面里 require 的模块都在 PRELOAD 里", function () {
  const manifest = readManifest("aitimer/browser.js")
  const preloaded = manifest.preload

  // 扫页面源码里所有 require("...") 的请求串，换算成 URL 路径后核对
  Object.keys(manifest.pages).forEach(function (key) {
    const file = manifest.pages[key]
    const src = fs.readFileSync(diskPath(file), "utf8")
    const dir = file.slice(0, file.lastIndexOf("/"))
    const reqRe = /require\("([^"]+)"\)/g
    let m = reqRe.exec(src)
    while (m) {
      const target = resolvePath(dir, m[1])
      assert.ok(
        preloaded.indexOf(target) >= 0,
        key + " require 了 " + target + "，但 browser.js 的 PRELOAD 里没有 —— 浏览器里会加载失败"
      )
      m = reqRe.exec(src)
    }
  })
})

test("aitimer：页面壳引入了 browser.js 与模块样式", function () {
  const html = fs.readFileSync(path.join(WEB_ROOT, "aitimer", "shell.html"), "utf8")
  assert.ok(html.indexOf("/aitimer/browser.js") >= 0, "壳应引入 browser.js")
  assert.ok(html.indexOf("/aitimer/style.css") >= 0, "壳应引入模块样式")
  assert.ok(html.indexOf("/cjs-loader.js") >= 0, "壳应引入 cjs-loader")
})
