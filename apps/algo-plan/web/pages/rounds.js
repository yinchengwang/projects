"use strict";
/**
 * rounds.js —— 轮次页：清单可视化 + 勾选写回 + 看 diff
 *
 * 勾选走 PATCH /api/rounds/:id/items/:itemId，写回只改 learning 仓里那一行，
 * **app 绝不自动 commit** —— diff 摆出来，提交与否由用户决定。
 */

(function () {
  const A = window.AlgoPlan
  const el = A.el

  /** 今天的日期（写完成时间用） */
  function today() {
    const now = new Date()
    const m = String(now.getMonth() + 1).padStart(2, "0")
    const d = String(now.getDate()).padStart(2, "0")
    return now.getFullYear() + "-" + m + "-" + d
  }

  /** 一行条目 */
  function itemRow(round, item, out) {
    const box = el("input", { type: "checkbox" })
    box.checked = item.status === "done"
    box.addEventListener("change", async function () {
      const done = box.checked
      box.disabled = true
      out.textContent = "写回中…"
      try {
        const body = await A.patchJson(
          "/api/rounds/" + encodeURIComponent(round.id) + "/items/" + encodeURIComponent(item.id),
          { status: done ? "done" : "todo", doneDate: done ? today() : "" }
        )
        out.textContent = ""
        out.appendChild(el("p", { class: "ok-text", text: "已写回 learning 仓（没有自动提交，提交与否由你决定）。" }))
        out.appendChild(el("pre", { class: "diff", text: body.diff || "（没有变化）" }))
        if (body.warnings && body.warnings.length > 0) {
          out.appendChild(el("ul", {}, body.warnings.map(function (w) { return el("li", { class: "warn-text", text: w }) })))
        }
      } catch (err) {
        out.textContent = ""
        out.appendChild(el("p", { class: "error", text: String(err.message || err) }))
        box.checked = !done
      } finally {
        box.disabled = false
      }
    })
    return el("tr", {}, [
      el("td", {}, [box]),
      el("td", { text: item.id }),
      el("td", { text: item.title }),
      el("td", {}, [el("span", { class: item.status === "done" ? "badge ok" : "badge", text: item.status === "done" ? "已完成" : "待完成" })]),
      el("td", { text: item.doneDate || "" }),
    ])
  }

  /** 一个轮次 */
  function roundPanel(round) {
    const out = el("div", { class: "result" })
    const rows = round.items.map(function (item) {
      return itemRow(round, item, out)
    })
    const doneCount = round.items.filter(function (item) {
      return item.status === "done"
    }).length

    return el("section", { class: "panel" }, [
      el("h2", { text: round.title || round.id }),
      el("div", { class: "kv" }, [
        el("div", {}, [document.createTextNode("轮次："), el("span", { text: round.id })]),
        el("div", {}, [document.createTextNode("状态："), el("span", { class: round.status === "done" ? "badge ok" : "badge", text: round.status === "done" ? "已完成" : "进行中" })]),
        el("div", {}, [document.createTextNode("进度："), el("span", { text: doneCount + "/" + round.items.length })]),
        el("div", {}, [document.createTextNode("目标："), el("span", { text: round.goal || "（没写目标）" })]),
      ]),
      round.items.length === 0
        ? el("p", { class: "muted", text: "这个轮次没解析出清单条目。" })
        : el("table", {}, [
            el("thead", {}, [
              el("tr", {}, [
                el("th", { text: "" }),
                el("th", { text: "ID" }),
                el("th", { text: "名称" }),
                el("th", { text: "状态" }),
                el("th", { text: "完成时间" }),
              ]),
            ]),
            el("tbody", {}, rows),
          ]),
      out,
    ])
  }

  A.pages.rounds = {
    title: "轮次",
    render: async function (view) {
      view.textContent = ""
      view.appendChild(el("div", { class: "panel", text: "加载中…" }))
      let data = null
      try {
        data = await A.getJson("/api/rounds")
      } catch (err) {
        view.textContent = ""
        view.appendChild(
          el("section", { class: "panel" }, [
            el("h2", { text: "加载失败" }),
            el("p", { class: "error", text: String(err.message || err) }),
          ])
        )
        return
      }
      view.textContent = ""

      if (data.warnings && data.warnings.length > 0) {
        view.appendChild(
          el("section", { class: "panel warn-panel" }, [
            el("h2", { text: "解析告警（" + data.warnings.length + "）" }),
            el("ul", {}, data.warnings.map(function (w) { return el("li", { text: w }) })),
          ])
        )
      }

      const rounds = data.rounds || []
      if (rounds.length === 0) {
        view.appendChild(
          el("section", { class: "panel" }, [
            el("h2", { text: "轮次" }),
            el("p", { class: "muted", text: "learning/progress/ 下没解析到轮次文件。" }),
          ])
        )
        return
      }
      rounds.forEach(function (round) {
        view.appendChild(roundPanel(round))
      })
    },
  }
})()
