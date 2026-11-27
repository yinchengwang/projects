"use strict"
/**
 * browser.js —— knowledge-hub 模块的浏览器入口
 *
 * 与 workbench 的 browser.js 同构：按依赖顺序预载 CommonJS 模块，再挂载页面。
 *
 * 预载顺序不能随意调：cjs-loader 的同步 require 只能拿到「已加载」的模块，
 * 所以被依赖者必须排在被依赖者前面。
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
    // ui.js / platform.js 虽然在外壳里也用 <script> 引过（挂到 window 上），
    // 但页面模块走的是 CommonJS require —— 那是 cjs-loader 的缓存，
    // 与 <script> 的全局挂载是两回事，必须在这里也预载一次。
    "/shared/ui.js",
    "/shared/platform.js",
    "/shared/knowledge/keys.js",
    "/shared/knowledge/seed-data.js",
    "/shared/knowledge/review.js",
    "/shared/knowledge/kh-constants.js",
    "/shared/knowledge/graph-layout.js",
    "/shared/knowledge/radar-layout.js",
    "/shared/knowledge/seed.js",
    "/shared/knowledge/stores.js",
    "/knowledge/nav.js",
    "/knowledge/theme-bridge.js",
    "/knowledge/md-block.js",
    "/knowledge/components/knowledge-graph.js",
    "/knowledge/components/radar-chart.js"
  ]

  /** 模块内路径 → 页面文件路径 */
  const PAGES = {
    "/pages/dashboard/index": "/knowledge/pages/dashboard/index.js",
    "/pages/learn/index": "/knowledge/pages/learn/index.js",
    "/pages/learn/quiz/index": "/knowledge/pages/learn/quiz/index.js",
    "/pages/learn/review/index": "/knowledge/pages/learn/review/index.js",
    "/pages/learn/deep/index": "/knowledge/pages/learn/deep/index.js",
    "/pages/learn/path/index": "/knowledge/pages/learn/path/index.js",
    "/pages/interview/index": "/knowledge/pages/interview/index.js",
    "/pages/interview/tracker/index": "/knowledge/pages/interview/tracker/index.js",
    "/pages/interview/bank/index": "/knowledge/pages/interview/bank/index.js",
    "/pages/interview/mock/index": "/knowledge/pages/interview/mock/index.js",
    "/pages/knowledge/index": "/knowledge/pages/knowledge/index.js",
    "/pages/knowledge/graph/index": "/knowledge/pages/knowledge/graph/index.js",
    "/pages/knowledge/excerpt/index": "/knowledge/pages/knowledge/excerpt/index.js",
    "/pages/knowledge/radar/index": "/knowledge/pages/knowledge/radar/index.js",
    "/pages/profile/index": "/knowledge/pages/profile/index.js",
    "/pages/plan/five-year/index": "/knowledge/pages/plan/five-year/index.js",
    "/pages/plan/roadmap/index": "/knowledge/pages/plan/roadmap/index.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    // children 是 HTMLCollection，没有 pop()；必须用 removeChild 逐个摘。
    while (rootEl.children.length) rootEl.removeChild(rootEl.children[0])
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
    // 画布进 DOM 后再绑定 —— getBoundingClientRect 需要元素已在文档流中
    if (typeof currentPage.attachCanvas === "function") currentPage.attachCanvas()
  }

  /**
   * 给页面补一个 setData：合并到 state 与 data 后重画。
   *
   * 为什么两份都要写：页面内部沿用小程序写法（handler 里读 this.data、
   * 改完调 this.setData），而 render 用的是 this.state。两个口径必须同步，
   * 否则浏览器里的事件处理器会读到过期值。
   *
   * @param {object} page 页面实例
   */
  function attach (page) {
    page.setData = function (patch) {
      Object.keys(patch || {}).forEach(function (key) {
        const parts = key.split(".")
        let nodeState = page.state
        let nodeData = page.data
        for (let i = 0; i < parts.length - 1; i++) {
          const part = parts[i]
          if (typeof nodeState[part] !== "object" || nodeState[part] === null) nodeState[part] = {}
          if (typeof nodeData[part] !== "object" || nodeData[part] === null) nodeData[part] = {}
          nodeState = nodeState[part]
          nodeData = nodeData[part]
        }
        const field = parts[parts.length - 1]
        nodeState[field] = patch[key]
        nodeData[field] = patch[key]
      })
      draw()
    }
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/knowledge/, "").replace(/\/$/, "") || "/pages/dashboard/index"
    const file = PAGES[key]
    if (!file) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) rootEl.appendChild(window.AlgoPlanUI.empty("页面不存在", key))
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      currentPage = page
      // 深拷贝一份 data：页面模块被 cjs-loader 缓存，直接复用会把上一页的
      // 表单态、展开态带到下一次访问。
      const initial = JSON.parse(JSON.stringify(page.data || {}))
      page.data = initial
      page.state = Object.assign({}, initial)
      attach(page)
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
      const rootEl = document.getElementById("app-main")
      if (rootEl) {
        rootEl.appendChild(window.AlgoPlanUI.empty("模块加载失败", String(err && err.message ? err.message : err)))
      }
      console.error("[knowledge] 加载失败", err)
    })
  }

  boot()
})()
