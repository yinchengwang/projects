"use strict"
/**
 * capture-sheet.js —— 工作台全局捕获面板
 *
 * 用原生 DOM 实现待办 / 闪念 / 笔记快速捕获；不依赖小程序 Component。
 */

const stores = require("../../../../shared/workbench/stores.js")
const dateUtil = require("../../../../shared/core/date.js")
const platform = require("../../../../shared/platform.js")
const ui = require("../../../../shared/ui.js")

/**
 * 构造捕获浮层 DOM 并绑定事件。
 * @param {{onClose?: Function, onCreated?: Function}} props
 * @returns {{element: HTMLElement, destroy: Function}}
 */
function create (props) {
  const options = props || {}
  const backdrop = ui.el("div", { class: "wb-capture-backdrop" })
  const panel = ui.el("section", { class: "wb-capture-panel" }, [
    ui.el("div", { class: "wb-capture-handle" }),
    ui.el("h2", { class: "wb-capture-title", text: "快速捕获" }),
    ui.el("p", { class: "wb-muted", text: "先记下来，稍后再整理。" })
  ])

  function close () {
    if (options.onClose) options.onClose()
    destroy()
  }

  function pick (kind) {
    if (kind === "note") {
      close()
      return Promise.resolve(platform.navigateTo("/workbench/pages/tech/notes/detail"))
    }
    const isTodo = kind === "todo"
    return platform.showModal({
      title: isTodo ? "记一条待办" : "记一条闪念",
      editable: true,
      placeholderText: isTodo ? "要做什么？" : "此刻的想法…"
    }).then(function (result) {
      if (!result || !result.confirm) return
      const text = String(result.content || "").trim()
      if (!text) return
      if (isTodo) {
        stores.todos.add({ title: text, done: false, priority: "med", category: "work", dueDate: dateUtil.todayStr() })
      } else {
        stores.insights.add({ content: text, tags: [], pinned: false, archived: false })
      }
      if (options.onCreated) options.onCreated(kind)
      close()
      platform.showToast({ title: "已记下", icon: "success" })
    })
  }

  ;[
    { type: "todo", label: "待办", hint: "记下要完成的事情", symbol: "✓" },
    { type: "insight", label: "闪念", hint: "记录灵感与念头", symbol: "✦" },
    { type: "note", label: "笔记", hint: "打开完整笔记编辑器", symbol: "文" }
  ].forEach(function (item) {
    const button = ui.el("button", { class: "wb-capture-option" }, [
      ui.el("span", { class: "wb-capture-symbol", text: item.symbol }),
      ui.el("span", { class: "wb-capture-option-copy" }, [
        ui.el("strong", { text: item.label }),
        ui.el("small", { text: item.hint })
      ])
    ])
    button.addEventListener("click", function () { pick(item.type) })
    panel.appendChild(button)
  })

  const cancel = ui.el("button", { class: "btn wb-capture-cancel", text: "取消" })
  cancel.addEventListener("click", close)
  panel.appendChild(cancel)
  backdrop.appendChild(panel)
  backdrop.addEventListener("click", function (event) {
    if (event.target === backdrop) close()
  })

  const host = typeof document !== "undefined" ? document.body : null
  if (host) host.appendChild(backdrop)

  function destroy () {
    if (backdrop.parentNode && backdrop.parentNode.removeChild) backdrop.parentNode.removeChild(backdrop)
    else if (host && host.removeChild) {
      try { host.removeChild(backdrop) } catch (e) {}
    }
  }

  return { element: backdrop, destroy: destroy, pick: pick, close: close }
}

module.exports = { create: create }
