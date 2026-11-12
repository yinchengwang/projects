"use strict";
/**
 * rounds.js —— 轮次页：清单可视化 + 勾选写回 + 看 diff
 *
 * 勾选走 PATCH /api/rounds/:id/items/:itemId，写回只改 learning 仓里那一行，
 * **app 绝不自动 commit** —— diff 摆出来，提交与否由用户决定。
 *
 * 层次（跟 style.css 的四件套对齐）：
 *   页头（.page-head）→ 当前轮次进主卡（.panel.hero）→ 其余轮次是普通卡（.panel）
 *   状态用中文药丸承载，不靠颜色单独表意。
 */

(function () {
  const A = window.AlgoPlan
  const el = A.el

  /**
   * 公共界面构件。浏览器里 ui.js 先加载挂 window.AlgoPlanUI；
   * Node 下没有全局挂载，就 require 同一份文件——实现只有一份。
   */
  const UI =
    (typeof window !== "undefined" && (window.AlgoPlanUI || (window.AlgoPlan && window.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** 今天的日期（写完成时间用） */
  function today() {
    const now = new Date()
    const m = String(now.getMonth() + 1).padStart(2, "0")
    const d = String(now.getDate()).padStart(2, "0")
    return now.getFullYear() + "-" + m + "-" + d
  }

  /** 状态药丸：颜色 + 中文文字双承载，不能只剩一块颜色 */
  function statusPill(text, tone) {
    if (UI) return UI.pill(text, tone)
    return el("span", { class: "badge" + (tone ? " " + tone : ""), text: text })
  }

  /** 一行条目 */
  function itemRow(round, item, out, focus) {
    const box = el("input", { type: "checkbox", class: "item-check" })
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
    return el("tr", { class: focus ? "focus-row" : "" }, [
      el("td", {}, [box]),
      el("td", { text: item.id }),
      el("td", { text: item.title }),
      el("td", {}, [statusPill(item.status === "done" ? "已完成" : "待完成", item.status === "done" ? "ok" : "")]),
      el("td", { text: item.doneDate || "" }),
    ])
  }

  /** 一个轮次：当前轮次进主卡，其余进普通卡 */
  function roundPanel(round, focusItemId, isCurrent) {
    const out = el("div", { class: "result" })
    const rows = round.items.map(function (item) {
      return itemRow(round, item, out, focusItemId && item.id === focusItemId)
    })
    const doneCount = round.items.filter(function (item) {
      return item.status === "done"
    }).length
    const total = round.items.length
    const percent = total === 0 ? 0 : Math.round((doneCount / total) * 100)

    // 头部信息摆成数字块：一眼看完这轮走到哪儿了
    const statItems = [
      { label: "轮次号", value: round.id },
      {
        label: "状态",
        value: round.status === "done" ? "已完成" : "进行中",
        tone: round.status === "done" ? "ok" : "accent",
      },
      { label: "已完成条目", value: doneCount + "/" + total, hint: percent + "%" },
    ]
    const stats = UI
      ? UI.statGrid(statItems)
      : el("div", { class: "kv" }, statItems.map(function (item) {
          return el("div", {}, [
            document.createTextNode(item.label + "："),
            el("span", { text: String(item.value) }),
          ])
        }))

    const goalText = round.goal || "（没写目标）"
    const goal = UI
      ? UI.kv([["目标", goalText]])
      : el("div", { class: "kv" }, [
          el("div", {}, [document.createTextNode("目标："), el("span", { text: goalText })]),
        ])

    const body = [
      stats,
      el("div", { class: "bar" }, [el("div", { class: "fill", style: "width:" + percent + "%" })]),
      goal,
      total === 0
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
    ]

    const title = round.title || round.id
    if (UI) {
      return isCurrent
        ? UI.hero({ title: title, children: body })
        : UI.panel({ title: title, children: body })
    }
    return el("section", { class: isCurrent ? "panel hero" : "panel" }, [
      el("h2", { text: title }),
    ].concat(body))
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

      if (UI) {
        view.appendChild(
          UI.pageHead({
            eyebrow: "轮次",
            title: "轮次清单",
            sub: "勾选会把结果写回 learning 仓对应的那一行——不自动提交，提交与否由你决定。",
          })
        )
      }

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
        const title = "没有解析到轮次"
        const hint =
          "learning/progress/ 下没解析到轮次文件。learning/progress/ 下没有轮次文件，或文件里没有清单表格。"
        view.appendChild(
          UI
            ? UI.empty(title, hint)
            : el("section", { class: "panel" }, [
                el("h2", { text: title }),
                el("p", { class: "muted", text: hint }),
              ])
        )
        return
      }
      // 今日页跳过来时带着 currentRoundId：把那一轮排到最前，条目也高亮
      const currentId = String(A.state.currentRoundId || "")
      const ordered = rounds.slice().sort(function (a, b) {
        if (a.id === currentId) return -1
        if (b.id === currentId) return 1
        return 0
      })
      if (currentId) {
        view.appendChild(
          el("section", { class: "panel quiet" }, [
            el("p", { class: "muted", text: "当前轮次：" + currentId + "（今日页推的那一轮，已排在最前）" }),
          ])
        )
      }
      // 主卡放「现在该推进的那一轮」：有当前轮次就是它，没有就取清单里的第一轮
      const hasCurrent = currentId && ordered.some(function (round) { return round.id === currentId })
      const heroId = hasCurrent ? currentId : ordered[0].id
      ordered.forEach(function (round) {
        view.appendChild(roundPanel(round, A.state.focusItemId, round.id === heroId))
      })
    },
  }
})()
