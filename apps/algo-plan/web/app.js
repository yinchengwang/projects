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

/**
 * 全局状态：页面之间共享的当前题目 / 当前轮次。
 * language / draft / result 是 M1/M3 留下来的：代码草稿与上一次的判题结果
 * 按题号存着，切页签、切题再切回来都不丢。
 */
const state = Object.assign((window.AlgoPlan && window.AlgoPlan.state) || {}, {
  problemId: null,
  currentRoundId: null,
  // 今日页推的轮次条目 id：跳到轮次页时高亮那一行
  focusItemId: null,
  language: "cpp",
  draft: {},
  result: {},
})

/** 切页 */
function go(key) {
  activeTab = key
  renderTabs(activeTab)
  render()
}

/** 页面注册表 + 公共工具 */
window.AlgoPlan = Object.assign(window.AlgoPlan || {}, {
  el: el,
  getJson: getJson,
  postJson: postJson,
  patchJson: patchJson,
  state: state,
  pages: (window.AlgoPlan && window.AlgoPlan.pages) || {},
  go: go,
})

/** 顶栏的四个标签 */
const TABS = [
  { key: "today", label: "今日" },
  { key: "answer", label: "作答" },
  { key: "review", label: "复习" },
  { key: "rounds", label: "轮次" },
  { key: "diagnose", label: "诊断" },
]

let activeTab = "today"

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

/**
 * 渲染当前页。
 *
 * 页面先画到临时容器里，等画完再整体换上去：
 * 切页时上一次请求可能还没回来，晚到的旧页面不能往当前视图里插内容。
 */
async function render() {
  const seq = ++renderSeq
  const view = document.getElementById("view")
  if (!view) return
  const page = window.AlgoPlan.pages[activeTab]
  if (!page) {
    view.textContent = ""
    view.appendChild(el("div", { class: "panel", text: "页面没注册：" + activeTab }))
    return
  }
  const holder = document.createElement("div")
  try {
    await page.render(holder)
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
