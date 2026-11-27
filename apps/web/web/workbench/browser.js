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
    "/pages/dashboard/index": "/web/workbench/pages/dashboard/index.js",
    "/pages/profile/index": "/web/workbench/pages/profile/index.js",
    "/pages/tech/index": "/web/workbench/pages/tech/index.js",
    "/pages/tech/notes/index": "/web/workbench/pages/tech/notes/index.js",
    "/pages/tech/notes/detail": "/web/workbench/pages/tech/notes/detail.js",
    "/pages/tech/snippets/index": "/web/workbench/pages/tech/snippets/index.js",
    "/pages/tech/snippets/detail": "/web/workbench/pages/tech/snippets/detail.js",
    "/pages/tech/interview/index": "/web/workbench/pages/tech/interview/index.js",
    "/pages/tech/interview/detail": "/web/workbench/pages/tech/interview/detail.js",
    "/pages/tech/resources/index": "/web/workbench/pages/tech/resources/index.js",
    "/pages/life/index": "/web/workbench/pages/life/index.js",
    "/pages/life/reading/index": "/web/workbench/pages/life/reading/index.js",
    "/pages/life/fitness/index": "/web/workbench/pages/life/fitness/index.js",
    "/pages/life/mood/index": "/web/workbench/pages/life/mood/index.js",
    "/pages/life/schedule/index": "/web/workbench/pages/life/schedule/index.js",
    "/pages/projects/index": "/web/workbench/pages/projects/index.js",
    "/pages/insights/index": "/web/workbench/pages/insights/index.js",
    "/pages/tools/index": "/web/workbench/pages/tools/index.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
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
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
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
