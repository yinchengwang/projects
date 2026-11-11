"use strict"
/**
 * app.js —— 前端逻辑（M1：只渲染「今日」页的骨架与题库概览）
 *
 * 原生 JS，无构建步骤。后续里程碑在这里接作答 / 复习 / 轮次 / 诊断页。
 */

/** 极简 DOM 构造器 */
function el(tag, attrs, children) {
  const node = document.createElement(tag)
  if (attrs) {
    Object.keys(attrs).forEach(function (key) {
      if (key === "class") node.className = attrs[key]
      else if (key === "text") node.textContent = attrs[key]
      else node.setAttribute(key, attrs[key])
    })
  }
  ;(children || []).forEach(function (child) {
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

/** 渲染健康状态面板 */
function renderHealth(health) {
  const learning = health.learning || {}
  const bank = health.bank || {}
  const badge = learning.ok
    ? el("span", { class: "badge ok", text: "已连接 learning 仓" })
    : el("span", { class: "badge warn", text: "纯本地模式" })
  const rows = [
    el("div", {}, [document.createTextNode("Node："), el("span", { text: health.node || "?" })]),
    el("div", {}, [document.createTextNode("题库题目数："), el("span", { text: String(bank.problemCount || 0) })]),
    el("div", {}, [document.createTextNode("作答流水条数："), el("span", { text: String(health.attempts || 0) })]),
    el("div", {}, [document.createTextNode("learning 仓："), el("span", { text: learning.root || "（未连接）" })]),
  ]
  return el("section", { class: "panel" }, [
    el("h2", { text: "运行状态" }),
    el("div", { class: "kv" }, rows),
    el("p", { class: "muted", text: learning.reason || "" }),
    badge,
  ])
}

/** 渲染题库列表 */
function renderBank(bank) {
  const items = (bank && bank.items) || []
  const rows = items.map(function (item) {
    const topics = (item.topics || []).map(function (t) {
      return el("span", { class: "badge", text: t })
    })
    return el("tr", {}, [
      el("td", { text: item.id }),
      el("td", { text: item.title }),
      el("td", { text: item.difficulty }),
      el("td", {}, topics),
      el("td", {}, [
        el("span", { class: item.needsFill ? "badge warn" : "badge", text: item.needsFill ? "待补全" : item.status }),
      ]),
    ])
  })
  return el("section", { class: "panel" }, [
    el("h2", { text: "题库（" + (bank ? bank.total : 0) + "）" }),
    items.length === 0
      ? el("p", { class: "muted", text: "题库还是空的。设置 ALGO_PLAN_LEARNING_ROOT 后运行 node tools/seed-bank.js 导入样板题。" })
      : el("table", {}, [
          el("thead", {}, [
            el("tr", {}, [
              el("th", { text: "ID" }),
              el("th", { text: "标题" }),
              el("th", { text: "难度" }),
              el("th", { text: "知识点" }),
              el("th", { text: "状态" }),
            ]),
          ]),
          el("tbody", {}, rows),
        ]),
  ])
}

/** 主渲染 */
async function render() {
  const view = document.getElementById("view")
  view.textContent = ""
  try {
    const health = await getJson("/api/health")
    const bank = await getJson("/api/bank")
    view.appendChild(renderHealth(health))
    view.appendChild(renderBank(bank))
  } catch (err) {
    view.appendChild(
      el("section", { class: "panel" }, [
        el("h2", { text: "加载失败" }),
        el("p", { class: "error", text: String(err.message || err) }),
      ])
    )
  }
}

document.addEventListener("DOMContentLoaded", function () {
  render()
})
