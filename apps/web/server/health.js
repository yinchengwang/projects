"use strict"
/**
 * health.js —— 后端在线状态探测
 *
 * study-hub(5174) 与 wiki-rag(8000) 是**独立进程**，可能没启动。
 * 合并应用的外壳要能告诉用户「哪个还没起来」，而不是让用户点进去才发现。
 *
 * 零依赖：用 node:http 发一个 GET，不引第三方 HTTP 客户端。
 */

const http = require("node:http")

/** study-hub 的地址（独立端口） */
const STUDY_URL = process.env.STUDY_HUB_URL || "http://127.0.0.1:5174"

/** wiki-rag 的地址（独立端口，Python） */
const WIKI_URL = process.env.WIKI_RAG_URL || "http://127.0.0.1:8000"

/**
 * 探测一个 URL 是否可达。
 *
 * selfPort 是本服务自己监听的端口 —— 探到自己的话就跳过：
 * 本应用若跑在 5174 上，探测 study-hub(5174) 就变成自己探自己，
 * /api/health 会无限递归（实测卡 17 秒，socket 也会被打爆）。
 *
 * @param {string} url
 * @param {number} [timeoutMs] 超时毫秒数，默认 1500
 * @param {number} [selfPort] 本服务自己的端口
 * @returns {Promise<{ok: boolean, status?: number, error?: string}>}
 */
function probe (url, timeoutMs, selfPort) {
  const timeout = timeoutMs === undefined ? 1500 : timeoutMs
  return new Promise(function (resolve) {
    let settled = false
    const done = function (result) {
      if (settled) return
      settled = true
      resolve(result)
    }

    let parsed
    try {
      parsed = new URL(url)
    } catch (e) {
      done({ ok: false, error: "地址不合法：" + url })
      return
    }

    if (selfPort && parsed.port && Number(parsed.port) === Number(selfPort)) {
      done({ ok: false, error: "目标端口就是本服务，已跳过探测" })
      return
    }

    const req = http.request(
      {
        host: parsed.hostname,
        port: parsed.port || 80,
        path: parsed.pathname + parsed.search,
        method: "GET",
        timeout: timeout
      },
      function (res) {
        // 不管状态码是多少，能连上就说明服务在
        res.resume()
        done({ ok: true, status: res.statusCode })
      }
    )

    req.on("timeout", function () {
      req.destroy()
      done({ ok: false, error: "连接超时（" + timeout + "ms 内没响应）" })
    })
    req.on("error", function (err) {
      done({ ok: false, error: "连接失败：" + (err && err.code ? err.code : "未知错误") })
    })
    req.end()
  })
}

/**
 * 探测两个后端。
 *
 * @param {number} [selfPort] 本服务自己的端口 —— 用来跳过探到自己的情况
 * @returns {Promise<{study: object, wiki: object}>}
 */
function checkBackends (selfPort) {
  return Promise.all([
    probe(STUDY_URL + "/api/health", undefined, selfPort),
    probe(WIKI_URL + "/api/health", undefined, selfPort)
  ]).then(function (results) {
    return {
      study: {
        ok: results[0].ok,
        status: results[0].status,
        error: results[0].error,
        url: STUDY_URL,
        hint: results[0].ok ? "" : "学习中枢未启动，请运行：PORT=5174 node apps/study-hub/server/main.js"
      },
      wiki: {
        ok: results[1].ok,
        status: results[1].status,
        error: results[1].error,
        url: WIKI_URL,
        hint: results[1].ok ? "" : "Wiki 检索未启动，请运行：cd apps/wiki-rag && python -m server.main"
      }
    }
  })
}

module.exports = {
  STUDY_URL: STUDY_URL,
  WIKI_URL: WIKI_URL,
  probe: probe,
  checkBackends: checkBackends
}
