"use strict"
/**
 * cjs-loader.js —— 浏览器侧的极小 CommonJS 加载器
 *
 * 目的：让计划一、二写好的 CommonJS 模块**一字不改**就能在浏览器里跑。
 *
 * 做法：fetch 源码 → new Function 包一层 → 把 require 指向本加载器。
 * 不改任何模块的写法，也不引入打包工具。
 *
 * 路径规则：以 / 开头的绝对路径直接用；相对路径（./ 与 ../）按当前模块所在目录解析。
 */

;(function (root) {
  "use strict"

  /** 已加载的模块缓存：绝对路径 → exports */
  const cache = {}

  /** 正在加载的模块（用于检测循环依赖） */
  const loading = {}

  /**
   * 把模块路径解析成绝对 URL 路径。
   * @param {string} fromDir 当前模块所在目录，如 "/shared/core"
   * @param {string} request 模块请求串，如 "./storage.js"
   * @returns {string} 绝对路径，如 "/shared/core/storage.js"
   */
  function resolvePath (fromDir, request) {
    if (request.charAt(0) === "/") return request
    const parts = (fromDir + "/" + request).split("/")
    const out = []
    for (let i = 0; i < parts.length; i++) {
      const p = parts[i]
      if (p === "" || p === ".") continue
      if (p === "..") { out.pop(); continue }
      out.push(p)
    }
    return "/" + out.join("/")
  }

  /**
   * 加载一个模块。
   * @param {string} absPath 绝对路径
   * @returns {Promise<object>} 模块 exports
   */
  function load (absPath) {
    if (cache[absPath]) return Promise.resolve(cache[absPath])
    if (loading[absPath]) return loading[absPath]

    const promise = fetch(absPath).then(function (res) {
      if (!res.ok) throw new Error("模块加载失败：" + absPath + "（HTTP " + res.status + "）")
      return res.text()
    }).then(function (src) {
      const module = { exports: {} }
      const dir = absPath.slice(0, absPath.lastIndexOf("/"))

      const localRequire = function (request) {
        const target = resolvePath(dir, request)
        // 同步 require 只支持已加载的模块 —— 模块按依赖顺序预载，这条约束由
        // browser.js 的 PRELOAD 顺序保证。
        if (cache[target]) return cache[target]
        throw new Error("同步 require 拿到未加载的模块：" + target + "（请检查预载顺序）")
      }

      const factory = new Function("require", "module", "exports", src + "\n//# sourceURL=" + absPath)
      factory(localRequire, module, module.exports)
      cache[absPath] = module.exports
      return module.exports
    })

    loading[absPath] = promise
    promise.then(function (exports) {
      cache[absPath] = exports
      delete loading[absPath]
    }, function () {
      delete loading[absPath]
    })
    return promise
  }

  root.CJS = { load: load, resolvePath: resolvePath }
})(typeof globalThis !== "undefined" ? globalThis : this)
