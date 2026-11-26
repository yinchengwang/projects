"use strict"
/**
 * router.js —— 极简路由：方法匹配 + 路径参数
 *
 * 零依赖。路径参数用 :name 段式，例如 "/api/items/:id"。
 * 路由匹配只认路径段，不认正则通配。
 */

/** 带 HTTP 状态码的错误，供 handler 抛出 */
class HttpError extends Error {
  constructor (status, message) {
    super(message)
    this.status = status
  }
}

/**
 * 把 "/api/items/:id" 编成匹配函数。
 * @param {string} pattern
 * @returns {(urlPath: string) => (object|null)} 命中返回参数对象，不命中返回 null
 */
function compile (pattern) {
  const segs = pattern.split("/").filter(function (s) { return s !== "" })
  return function (urlPath) {
    const parts = urlPath.split("/").filter(function (s) { return s !== "" })
    if (parts.length !== segs.length) return null
    const params = {}
    for (let i = 0; i < segs.length; i += 1) {
      const seg = segs[i]
      const raw = parts[i]
      if (seg.charAt(0) === ":") {
        let value
        try {
          value = decodeURIComponent(raw)
        } catch (e) {
          return null
        }
        if (value === "") return null
        params[seg.slice(1)] = value
      } else if (seg !== raw) {
        return null
      }
    }
    return params
  }
}

/**
 * 创建路由表。
 * @returns {{add: Function, find: Function, routes: Array}}
 */
function createRouter () {
  const routes = []

  function add (method, pattern, handler) {
    routes.push({ method: method, pattern: pattern, match: compile(pattern), handler: handler })
  }

  /**
   * @returns {{route: object, params: object}|{methodNotAllowed: true, allowed: string[]}|null}
   */
  function find (method, urlPath) {
    let pathMatched = false
    const allowed = []
    for (const route of routes) {
      const params = route.match(urlPath)
      if (params === null) continue
      pathMatched = true
      if (route.method === method) return { route: route, params: params }
      if (allowed.indexOf(route.method) < 0) allowed.push(route.method)
    }
    if (pathMatched) return { methodNotAllowed: true, allowed: allowed }
    return null
  }

  return { add: add, find: find, routes: routes }
}

module.exports = { HttpError: HttpError, compile: compile, createRouter: createRouter }
