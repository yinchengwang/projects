"use strict";
/**
 * app.js —— 前端骨架：tab 调度 + 公共工具
 *
 * 原生 JS，无构建步骤。四个页面在 web/pages/ 下，各自注册到 window.AlgoPlan.pages。
 * index.html 里 app.js 先加载，所以页面脚本加载时公共工具已经就绪。
 *
 * M1/M3 的旧渲染逻辑（题库表格、原题入口、题面缺失说明、判题结果面板）
 * 搬到了 web/pages/answer.js 与 web/pages/today.js；本文件只留公共工具与 tab 调度，
 * 外加一层给老测试用的兼容视图（Node 下才装载）。
 */

// Node（单测）里没有 window：补一个替身，页面脚本就能走同一条加载路径
if (typeof window === "undefined" && typeof global !== "undefined") {
  global.window = global
}

/** 极简 DOM 构造器 */
function el(tag, attrs, children) {
  const node = document.createElement(tag)
  if (attrs) {
    Object.keys(attrs).forEach(function (key) {
      if (key === "class") node.className = attrs[key]
      else if (key === "text") node.textContent = attrs[key]
      else if (key.indexOf("on") === 0 && typeof attrs[key] === "function") {
        // 事件监听器：onclick → click
        node.addEventListener(key.slice(2).toLowerCase(), attrs[key])
      } else node.setAttribute(key, attrs[key])
    })
  }
  ;(children || []).forEach(function (child) {
    if (!child) return
    node.appendChild(child)
  })
  return node
}

/** 取 JSON，失败时抛出带中文说明的错误 */
async function getJson(url) {
  const res = await fetch(url)
  const body = await res.json()
  if (!res.ok) {
    throw new Error((body && body.error) || "请求失败：" + res.status)
  }
  return body
}

/** 发 JSON（POST / PATCH），失败时抛出带中文说明的错误 */
async function sendJson(url, method, body) {
  const res = await fetch(url, {
    method: method,
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
  const data = await res.json()
  if (!res.ok) {
    throw new Error((data && data.error) || "请求失败：" + res.status)
  }
  return data
}

function postJson(url, body) {
  return sendJson(url, "POST", body)
}

function patchJson(url, body) {
  return sendJson(url, "PATCH", body)
}

/** 发 JSON（PUT）——P1 的会话设置用它写回 */
function putJson(url, body) {
  return sendJson(url, "PUT", body)
}

/**
 * 全局状态：页面之间共享的当前题目 / 当前轮次。
 * language / draft / result 是 M1/M3 留下来的：代码草稿与上一次的判题结果
 * 按题号存着，切页签、切题再切回来都不丢。
 */
const state = Object.assign(
  (window.StudyHub && window.StudyHub.state) || (window.AlgoPlan && window.AlgoPlan.state) || {},
  {
    problemId: null,
    currentRoundId: null,
    // 今日页推的轮次条目 id：跳到轮次页时高亮那一行
    focusItemId: null,
    language: "cpp",
    draft: {},
    result: {},
    // P1：会话页记着当前会话的 id，切页回来不丢
    sessionId: null,
  }
)

/** 切页：改 hash，由 hashchange 驱动重绘 */
function go(key) {
  const hash = "#/" + String(key || "")
  const loc = typeof location !== "undefined" ? location : null
  if (loc) {
    if (loc.hash === hash) {
      // hash 没变就不会触发 hashchange，这里自己重绘一次
      activeTab = key
      renderTabs(activeTab)
      render()
      return
    }
    loc.hash = hash
    return
  }
  activeTab = key
  renderTabs(activeTab)
  render()
}


/**
 * 解析地址栏 hash。未知页面回落 session；unit 保留解码后的 id 参数。
 * @param {string} [hash]
 * @returns {{page: string, params: object, query: object}}
 */
function parseHash(hash) {
  const raw =
    hash === undefined || hash === null
      ? typeof location !== "undefined" && location && typeof location.hash === "string"
        ? location.hash
        : ""
      : String(hash)
  const text = raw.charAt(0) === "#" ? raw.slice(1) : raw
  const qIndex = text.indexOf("?")
  const pathPart = qIndex === -1 ? text : text.slice(0, qIndex)
  const queryPart = qIndex === -1 ? "" : text.slice(qIndex + 1)
  const query = {}
  new URLSearchParams(queryPart).forEach(function (value, key) {
    query[key] = value
  })
  const segments = pathPart.split("/").filter(function (part) {
    return part !== ""
  })
  const knownPages = ["session", "library", "diagnose", "supply", "settings"]
  const page = segments.length === 0 ? "session" : segments[0]
  if (page === "unit") {
    const params = {}
    if (segments.length > 1) params.id = decodeURIComponent(segments.slice(1).join("/"))
    return { page: "unit", params: params, query: query }
  }
  return {
    page: knownPages.indexOf(page) !== -1 ? page : "session",
    params: {},
    query: query,
  }
}


/**
 * 按 hash 定当前页。页没注册就不动（返回 false）。
 * @param {string} [hash]
 * @returns {boolean}
 */
function applyHash(hash) {
  const raw =
    hash === undefined || hash === null
      ? typeof location !== "undefined" && location && typeof location.hash === "string"
        ? location.hash
        : ""
      : String(hash)
  const text = raw.charAt(0) === "#" ? raw.slice(1) : raw
  const head =
    text
      .split("?")[0]
      .split("/")
      .filter(function (part) {
        return part !== ""
      })[0] || "session"
  if (!window.StudyHub.pages[head]) return false
  activeTab = head
  return true
}

/**
 * 读当前路由：地址栏 hash 解析出来的结果。
 * @returns {{page: string, params: object, query: object}}
 */
function readRoute() {
  const hash = (typeof location !== "undefined" && location && location.hash) || ""
  return parseHash(hash)
}


/** 页面注册表 + 公共工具 */
const APP = window.StudyHub || window.AlgoPlan || {}
window.StudyHub = Object.assign(APP, {
  el: el,
  getJson: getJson,
  postJson: postJson,
  patchJson: patchJson,
  putJson: putJson,
  parseHash: parseHash,
  state: state,
  pages:
    (window.StudyHub && window.StudyHub.pages) || (window.AlgoPlan && window.AlgoPlan.pages) || {},
  go: go,
  // 公共界面构件：ui.js 在浏览器里挂 window.AlgoPlanUI；Node 下没有，
  // 页面脚本会自己 require 一份——两边是同一份实现。
  ui: (typeof window !== "undefined" && window.AlgoPlanUI) || null,
})

// 别名：老页面脚本（today / answer / review / rounds）与 web/diagnose.js
// 靠 window.AlgoPlan 拿公共工具——P1 不改它们一行，靠这个别名继续工作。
window.AlgoPlan = window.StudyHub

/**
 * 顶栏的标签。
 *
 * P1 只上**已交付**的页面：本次学习（session）、诊断（diagnose）、设置（settings）。
 * 单元库（library）P2 交付、供给（supply）P4 交付——到那一期再把标签加进来，
 * 不提前挂一个点开是「页面没注册」的标签。
 *
 * 过渡期：today / answer / review / rounds 四个旧页仍注册在 pages 里（不删），
 * 只是从标签栏里摘掉了（不再有导航入口）。
 */
const TABS = [
  { key: "session", label: "本次学习" },
  { key: "library", label: "单元库" },
  { key: "diagnose", label: "诊断" },
  { key: "settings", label: "设置" },
]

let activeTab = "session"

/**
 * 渲染序号。每次 render() 自增，用来挡掉异步竞态：
 * 后发起的 render 序号更大，先发起的 await 回来后发现序号变了就直接放弃，
 * 不会把过期的面板追加到已经切走的视图里。
 */
let renderSeq = 0

/** 画标签栏 */
function renderTabs(active) {
  const nav = document.getElementById("tabs")
  if (!nav) return
  nav.textContent = ""
  TABS.forEach(function (tab) {
    nav.appendChild(
      el("button", {
        class: tab.key === active ? "active" : "",
        text: tab.label,
        onclick: function () {
          go(tab.key)
        },
      })
    )
  })
}

async function render() {
  const seq = ++renderSeq
  const view = document.getElementById("view")
  if (!view) return
  const route = readRoute()
  const tabKeys = TABS.map(function (t) {
    return t.key
  })
  if (tabKeys.indexOf(route.page) !== -1) {
    activeTab = route.page
    renderTabs(activeTab)
  }
  const page = APP.pages[route.page]
  if (!page) {
    view.textContent = ""
    view.appendChild(el("div", { class: "panel", text: "页面没注册：" + route.page }))
    return
  }
  const holder = document.createElement("div")
  try {
    // 第二个参数是路由参数：#/unit/:id 传 { id }，其余页传 {}（多传的参数被忽略）
    await page.render(holder, route.params)
    if (seq !== renderSeq) return
    view.textContent = ""
    view.appendChild(holder)
  } catch (err) {
    if (seq !== renderSeq) return
    view.textContent = ""
    view.appendChild(
      el("section", { class: "panel" }, [
        el("h2", { text: "出错了" }),
        el("p", { class: "error", text: String(err.message || err) }),
      ])
    )
  }
}

if (typeof document !== "undefined") {
  document.addEventListener("DOMContentLoaded", function () {
    // 先按 hash 定页（`#/` 默认就是本次学习），再画标签栏与页面
    applyHash()
    renderTabs(activeTab)
    render()
  })
}

// 地址栏 hash 变了就重绘：#/unit/db:mvcc 与 #/library?track=database 都走这条路
// （浏览器里才有 location / addEventListener；Node 桩里没有）
if (typeof window !== "undefined" && typeof window.addEventListener === "function") {
  window.addEventListener("hashchange", function () {
    applyHash()
    renderTabs(activeTab)
    render()
  })
}

// Node 下（测试用）可以 require 本文件取纯函数与渲染函数；浏览器里不会执行这一段
if (typeof module !== "undefined" && module.exports) {
  // 页面脚本在浏览器里靠 <script> 标签加载；Node 下手动 require 一次，
  // 老测试（题库行、样例用例、代码草稿）拿到的就是同一份实现
  const todayPage = require("./pages/today.js")
  const answerPage = require("./pages/answer.js")
  // P2 起 answer.js 不再注册（文件保留在仓库里作为历史，契约 §9.1）。
  // 旧测试要的 renderBank / renderPractice 等仍从 __test 拿得到。
  if (APP.pages) delete APP.pages.answer

  // P3 起旧三页不再注册（文件保留在仓库里作为历史，契约 §9.1）。
  // 文件与旧导出都留着：tests/web-dom.test.js 还要用 today.js 的 renderHealth。
  // 下面 renderLegacy 走的是 todayPage.render（模块引用），不受注册被删的影响。
  if (APP.pages) {
    delete APP.pages.today
    delete APP.pages.review
    delete APP.pages.rounds
  }

  /** 旧视图：today / bank / practice（M1、M3 的老测试仍在用这三个视图） */
  let legacyView = "today"

  /**
   * 旧视图的渲染：跟老 app.js 一样，今日视图先健康面板再题库表。
   * 画到临时容器再换上去，过期的请求不会往当前视图里插内容。
   */
  async function renderLegacy() {
    const seq = ++renderSeq
    const view = document.getElementById("view")
    if (!view) return
    const holder = document.createElement("div")
    try {
      if (legacyView === "practice") {
        await answerPage.render(holder)
      } else {
        if (legacyView === "today") await todayPage.render(holder)
        const bank = await getJson("/api/bank")
        if (seq !== renderSeq) return
        holder.appendChild(answerPage.renderBank(bank))
      }
      if (seq !== renderSeq) return
      view.textContent = ""
      view.appendChild(holder)
    } catch (err) {
      if (seq !== renderSeq) return
      view.textContent = ""
      view.appendChild(
        el("section", { class: "panel" }, [
          el("h2", { text: "加载失败" }),
          el("p", { class: "error", text: String(err.message || err) }),
        ])
      )
    }
  }

  module.exports = {
    describePanel: answerPage.describePanel,
    describeSourceLink: answerPage.describeSourceLink,
    truncate: answerPage.truncate,
    TONE_BY_VERDICT: answerPage.TONE_BY_VERDICT,
    PANEL_TEXT_LIMIT: answerPage.PANEL_TEXT_LIMIT,
    // P1：最小 hash 解析器、标签清单
    parseHash: parseHash,
    putJson: putJson,
    readRoute: readRoute,
    applyHash: applyHash,
    TABS: TABS,
    // 以下只给测试用：让 DOM 冒烟测试能驱动真实的渲染路径
    __test: {
      state: state,
      render: renderLegacy,
      setView: function (view) {
        legacyView = view
      },
      getView: function () {
        return legacyView
      },
      renderBank: answerPage.renderBank,
      renderPractice: async function () {
        const holder = document.createElement("div")
        await answerPage.renderPractice(holder, state.problemId)
        return holder
      },
      renderVerdictPanel: answerPage.renderVerdictPanel,
      renderMissingStatement: answerPage.renderMissingStatement,
      renderSourceLink: answerPage.renderSourceLink,
    },
  }
}
