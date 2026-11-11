"use strict"
/**
 * router.js —— 极简路由：方法匹配 + 路径参数 + JSON body 解析
 *
 * 零依赖。路径参数用 :name 段式，例如 "/api/problem/:id"。
 * 路由匹配只认路径段，不认正则通配——本机工具用不上那套。
 */

/** 请求体上限 1MB，防止本地误传超大 body 把内存吃满 */
const MAX_BODY_BYTES = 1024 * 1024

/** 超限后仍继续丢弃的字节上限：超过这个量就放弃收，直接断开连接 */
const DRAIN_LIMIT_BYTES = MAX_BODY_BYTES * 8

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
 * 读请求体并解析成 JSON。
 * - 空 body 返回 {}
 * - 非法 JSON 抛 HttpError(400)
 * - body 超过 1MB 抛 HttpError(413)
 * - body 解析出来不是 JSON 对象（数字/字符串/null/数组）抛 HttpError(400)
 * @param {import("node:http").IncomingMessage} req
 * @returns {Promise<object>} 恒定 resolve 一个 JSON 对象
 */
function readJsonBody(req) {
  return new Promise(function (resolve, reject) {
    let size = 0
    let chunks = []
    let settled = false

    /** 超限后用来丢弃剩余数据的回调：只算字节数，不缓存 */
    function onDrain(chunk) {
      size += chunk.length
      if (size > DRAIN_LIMIT_BYTES) {
        // 病态大 body：读得够多了，直接断连接（此时 413 响应早已写完）
        req.destroy()
      }
    }

    function onData(chunk) {
      size += chunk.length
      if (size > MAX_BODY_BYTES) {
        // 超限：只 reject，**不要** req.destroy()。
        // 销毁 socket 之后 app.js 的 413 响应就写不出去了，
        // 客户端只会收到连接重置（UND_ERR_SOCKET），看不到这句中文提示。
        settled = true
        chunks = []
        req.removeListener("data", onData)
        // 把剩下的 body 读干净并丢弃：socket 收空，连接收尾时才不会回 RST 把 413 响应打掉
        req.on("data", onDrain)
        req.resume()
        reject(new HttpError(413, "请求体过大（上限 1MB）"))
        return
      }
      chunks.push(chunk)
    }

    req.on("data", onData)
    req.on("end", function () {
      if (settled) return
      settled = true
      const raw = Buffer.concat(chunks).toString("utf8").trim()
      if (raw === "") {
        resolve({})
        return
      }
      let value
      try {
        value = JSON.parse(raw)
      } catch (e) {
        reject(new HttpError(400, "请求体不是合法 JSON"))
        return
      }
      // 只接受 JSON 对象：数字、字符串、null、数组都不是对象。
      // 悄悄当成空对象会把「客户端 body 传错了」变成「服务端字段缺失」，更难排查，
      // 所以直接报 400，让调用方明确知道 body 的形状不对。
      if (value === null || typeof value !== "object" || Array.isArray(value)) {
        reject(new HttpError(400, "请求体必须是 JSON 对象"))
        return
      }
      resolve(value)
    })
    req.on("error", function (err) {
      if (settled) return
      settled = true
      reject(err)
    })
  })
}

module.exports = {
  MAX_BODY_BYTES: MAX_BODY_BYTES,
  HttpError: HttpError,
  compile: compile,
  createRouter: createRouter,
  readJsonBody: readJsonBody,
}
