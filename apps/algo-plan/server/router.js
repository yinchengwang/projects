"use strict"
/**
 * router.js —— 极简路由：方法匹配 + 路径参数 + JSON body 解析
 *
 * 零依赖。路径参数用 :name 段式，例如 "/api/problem/:id"。
 * 路由匹配只认路径段，不认正则通配——本机工具用不上那套。
 */

/** 请求体上限 1MB，防止本地误传超大 body 把内存吃满 */
const MAX_BODY_BYTES = 1024 * 1024

/** 带 HTTP 状态码的错误，供 handler 抛出 */
class HttpError extends Error {
  constructor(status, message) {
    super(message)
    this.status = status
  }
}

/**
 * 把 "/api/problem/:id" 编成匹配函数。
 * @param {string} pattern
 * @returns {(urlPath: string) => (object|null)} 命中返回参数对象，不命中返回 null
 */
function compile(pattern) {
  const segs = pattern.split("/").filter(function (s) {
    return s !== ""
  })
  return function (urlPath) {
    const parts = urlPath.split("/").filter(function (s) {
      return s !== ""
    })
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
 * add(method, pattern, handler) 注册；find(method, urlPath) 查。
 */
function createRouter() {
  const routes = []

  function add(method, pattern, handler) {
    routes.push({
      method: method,
      pattern: pattern,
      match: compile(pattern),
      handler: handler,
    })
  }

  /**
   * @returns {{route: object, params: object}|{methodNotAllowed: true}|null}
   *   route 命中；methodNotAllowed 表示路径对但方法不对；null 表示没这个路径
   */
  function find(method, urlPath) {
    let pathMatched = false
    for (const route of routes) {
      const params = route.match(urlPath)
      if (params === null) continue
      pathMatched = true
      if (route.method === method) {
        return { route: route, params: params }
      }
    }
    if (pathMatched) return { methodNotAllowed: true }
    return null
  }

  return { add: add, find: find, routes: routes }
}

/**
 * 读请求体并解析成 JSON。空 body 返回 {}，非法 JSON 抛 HttpError(400)。
 * @param {import("node:http").IncomingMessage} req
 * @returns {Promise<object>}
 */
function readJsonBody(req) {
  return new Promise(function (resolve, reject) {
    let size = 0
    const chunks = []
    req.on("data", function (chunk) {
      size += chunk.length
      if (size > MAX_BODY_BYTES) {
        reject(new HttpError(413, "请求体过大（上限 1MB）"))
        req.destroy()
        return
      }
      chunks.push(chunk)
    })
    req.on("end", function () {
      const raw = Buffer.concat(chunks).toString("utf8").trim()
      if (raw === "") {
        resolve({})
        return
      }
      try {
        resolve(JSON.parse(raw))
      } catch (e) {
        reject(new HttpError(400, "请求体不是合法 JSON"))
      }
    })
    req.on("error", reject)
  })
}

module.exports = {
  MAX_BODY_BYTES: MAX_BODY_BYTES,
  HttpError: HttpError,
  compile: compile,
  createRouter: createRouter,
  readJsonBody: readJsonBody,
}
