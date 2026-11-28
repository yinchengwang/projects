"use strict"
/**
 * browser.js —— aitimer 模块的浏览器入口
 *
 * 与 trip 模块的 browser.js 同构：按依赖顺序预载 CommonJS 模块，再挂载页面。
 *
 * 注意 PAGES 里的路径是 **URL 路径**（静态根是 apps/web/web/），
 * 所以是 "/aitimer/pages/index.js"，不是文件系统里的 "web/aitimer/..."。
 */

;(function () {
  "use strict"

  const CJS = window.CJS

  /**
   * 按依赖顺序预载的模块路径。
   *
   * ui.js / platform.js 也在其中：页面模块用 require() 取它们，
   * 而 cjs-loader 的同步 require 只认已载入缓存的模块。
   *
   * md-block.js 同样必须预载 —— index / series / doc 三个页面都用
   * require("../md-block.js") 取 plainText 与 mdBlock。
   */
  const PRELOAD = [
    "/shared/ui.js",
    "/shared/platform.js",
    "/shared/core/md2html.js",
    "/aitimer/md-block.js"
  ]

  /** 模块内路径 → 页面文件路径（URL 路径） */
  const PAGES = {
    "/pages/index": "/aitimer/pages/index.js",
    "/pages/series": "/aitimer/pages/series.js",
    "/pages/doc": "/aitimer/pages/doc.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.removeChild(rootEl.children[0])
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
  }

  /**
   * 给页面补一个 setData：合并到 state 后重画。
   * 页面内部继续沿用小程序写法（setData + render），这里一处适配即可。
   * @param {object} page
   */
  function attach (page) {
    page.setData = function (patch) {
      Object.keys(patch || {}).forEach(function (key) {
        page.state[key] = patch[key]
      })
      draw()
    }
  }

  function showError (title, detail) {
    const rootEl = document.getElementById("app-main")
    if (rootEl && window.AlgoPlanUI) rootEl.appendChild(window.AlgoPlanUI.empty(title, detail))
  }

  /**
   * 把地址栏路径归一成模块内路径。
   * @param {string} pathname
   * @returns {string}
   */
  function normalizeKey (pathname) {
    const key = pathname.replace(/^\/aitimer/, "").split("?")[0] || "/pages/index"
    if (PAGES[key]) return key
    if (/\/index$/.test(key) && PAGES[key.replace(/\/index$/, "")]) return key.replace(/\/index$/, "")
    return key
  }

  function loadPage (pathname) {
    const key = normalizeKey(pathname)
    const file = PAGES[key]
    if (!file) {
      showError("页面不存在", key)
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      if (currentPage && typeof currentPage.onUnmount === "function") currentPage.onUnmount()
      currentPage = page
      currentPage.state = Object.assign({}, currentPage.data)
      attach(currentPage)
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
      draw()
      return currentPage
    })
  }

  function boot () {
    let chain = Promise.resolve()
    PRELOAD.forEach(function (p) {
      chain = chain.then(function () { return CJS.load(p) })
    })
    chain.then(function () {
      window.addEventListener("popstate", function () {
        loadPage(window.location.pathname)
      })
      return loadPage(window.location.pathname)
    }).catch(function (err) {
      showError("模块加载失败", String(err && err.message ? err.message : err))
      console.error("[aitimer] 加载失败", err)
    })
  }

  boot()
})()
