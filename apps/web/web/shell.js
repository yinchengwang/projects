"use strict"
/**
 * shell.js —— 合并应用外壳的脚本
 *
 * 只做一件事：探测两个独立后端的在线状态，把结果显示在入口卡片上。
 */

;(function () {
  "use strict"

  /**
   * 更新一个后端入口的状态显示。
   * @param {string} id 状态元素的 id
   * @param {object} info checkBackends 返回的那一项
   */
  function paintState (id, info) {
    const node = document.getElementById(id)
    if (!node) return
    if (info.ok) {
      node.textContent = "运行中"
      node.className = "module-state ok"
      return
    }
    node.textContent = "未启动"
    node.className = "module-state off"
    node.setAttribute("title", info.hint || "")
  }

  fetch("/api/health")
    .then(function (res) { return res.json() })
    .then(function (data) {
      if (!data || !data.ok) return
      paintState("study-state", data.study || { ok: false })
      paintState("wiki-state", data.wiki || { ok: false })
    })
    .catch(function (err) {
      console.error("[外壳] 健康检查失败", err)
      paintState("study-state", { ok: false, hint: "健康检查请求失败" })
      paintState("wiki-state", { ok: false, hint: "健康检查请求失败" })
    })
})()
