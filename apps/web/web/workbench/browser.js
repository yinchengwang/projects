"use strict"
/**
 * browser.js —— workbench 模块的浏览器入口
 *
 * 按依赖顺序预载模块，再挂载页面。
 * CommonJS 模块由 cjs-loader.js 负责加载（计划一、二的模块一字不改）。
 */

;(function () {
  "use strict"

  const CJS = window.CJS

  /** 按依赖顺序预载的模块路径 */
  const PRELOAD = [
    "/shared/core/id.js",
    "/shared/core/date.js",
    "/shared/core/format.js",
    "/shared/core/constants.js",
    "/shared/core/md2html.js",
    "/shared/core/md.js",
    "/shared/core/storage.js",
    "/shared/core/store.js",
    "/shared/core/theme.js",
    "/shared/core/with-theme.js",
    "/shared/workbench/keys.js",
    "/shared/workbench/seed.js",
    "/shared/workbench/codetools.js",
    "/shared/workbench/stores.js"
  ]

  /** 模块内路径 → 页面文件路径 */
  const PAGES = {
    "/pages/dashboard/index": "/workbench/pages/dashboard/index.js",
    "/pages/profile/index": "/workbench/pages/profile/index.js",
    "/pages/tech/index": "/workbench/pages/tech/index.js",
    "/pages/tech/notes/index": "/workbench/pages/tech/notes/index.js",
    "/pages/tech/notes/detail": "/workbench/pages/tech/notes/detail.js",
    "/pages/tech/snippets/index": "/workbench/pages/tech/snippets/index.js",
    "/pages/tech/snippets/detail": "/workbench/pages/tech/snippets/detail.js",
    "/pages/tech/interview/index": "/workbench/pages/tech/interview/index.js",
    "/pages/tech/interview/detail": "/workbench/pages/tech/interview/detail.js",
    "/pages/tech/resources/index": "/workbench/pages/tech/resources/index.js",
    "/pages/life/index": "/workbench/pages/life/index.js",
    "/pages/life/reading/index": "/workbench/pages/life/reading/index.js",
    "/pages/life/fitness/index": "/workbench/pages/life/fitness/index.js",
    "/pages/life/mood/index": "/workbench/pages/life/mood/index.js",
    "/pages/life/schedule/index": "/workbench/pages/life/schedule/index.js",
    "/pages/projects/index": "/workbench/pages/projects/index.js",
    "/pages/insights/index": "/workbench/pages/insights/index.js",
    "/pages/tools/index": "/workbench/pages/tools/index.js"
  }

  let currentPage = null

  /**
   * 读取查询串，形状与小程序 onLoad(query) 的参数一致。
   * @returns {object}
   */
  function readQuery () {
    const out = {}
    const search = window.location.search || ""
    if (!search) return out
    new URLSearchParams(search).forEach(function (value, key) { out[key] = value })
    return out
  }

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
  }

  /**
   * 给页面补一个 setData：合并到 state 后重画。
   * 页面模块沿用小程序写法（setData + render），这里用一处适配层衔接，
   * 页面与测试里的代码都不需要改。
   * @param {object} page 页面实例
   */
  function attach (page) {
    page.setData = function (patch) {
      Object.keys(patch || {}).forEach(function (key) {
        const parts = key.split(".")
        let node = page.state
        for (let i = 0; i < parts.length - 1; i++) {
          const part = parts[i]
          if (typeof node[part] !== "object" || node[part] === null) node[part] = {}
          node = node[part]
        }
        node[parts[parts.length - 1]] = patch[key]
      })
      draw()
    }
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/workbench/, "") || "/pages/dashboard/index"
    const file = PAGES[key]
    if (!file) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) rootEl.appendChild(window.AlgoPlanUI.empty("页面不存在", key))
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      currentPage = page
      currentPage.state = Object.assign({}, currentPage.data)
      attach(currentPage)
      // 生命周期与小程序一致：onMount 建好容器与主题，onLoad 初始化，onShow 每次进入刷新。
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
      if (typeof currentPage.onLoad === "function") currentPage.onLoad(readQuery())
      if (typeof currentPage.onShow === "function") currentPage.onShow()
      draw()
      return currentPage
    })
  }

  function boot () {
    // 按依赖顺序串行预载
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
      const rootEl = document.getElementById("app-main")
      if (rootEl) {
        rootEl.appendChild(window.AlgoPlanUI.empty("模块加载失败", String(err && err.message ? err.message : err)))
      }
      console.error("[workbench] 加载失败", err)
    })
  }

  boot()
})()
