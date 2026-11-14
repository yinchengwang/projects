"use strict";
/**
 * review.js —— 复习页：到期卡片 + 直觉评分
 *
 * 直觉文案 → SM-2 quality 的映射（设计文档 §4.2 的六档里，界面只暴露四档）：
 *   完全忘了 → 1（没记住，重置间隔）
 *   想很久   → 2（想很久才想起，重置间隔）
 *   有点印象 → 3（想起来了，间隔不变）
 *   秒答     → 5（一次过而且快，间隔按 easiness 拉长）
 * 剩下两档——0 = 直接抄参考答案、4 = 一次通过但偏慢——由判题流程自动判定，不做按钮。
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

  /** 四个直觉评分按钮 */
  const BUTTONS = [
    { label: "完全忘了", quality: 1 },
    { label: "想很久", quality: 2 },
    { label: "有点印象", quality: 3 },
    { label: "秒答", quality: 5 },
  ]

  /** 队列概况 */
  function queuePanel(next) {
    const count = (next && next.reviewQueueLength) || 0
    const stats = UI
      ? UI.statGrid([
          { label: "到期卡片", value: count + " 张", tone: count > 0 ? "accent" : "" },
          { label: "单日配额", value: "10 题", hint: "或 20 分钟，取先到者" },
        ])
      : el("div", { class: "kv" }, [
          el("div", {}, [document.createTextNode("队列长度："), el("span", { text: count + " 张" })]),
          el("div", {}, [
            document.createTextNode("单日配额："),
            el("span", { text: "10 题 或 20 分钟，取先到者" }),
          ]),
        ])
    return el("section", { class: "panel" }, [
      el("h2", { text: "到期复习" }),
      stats,
    ])
  }

  /** 到期卡片 + 四个评分按钮 */
  function cardPanel(action) {
    const out = el("div", { class: "result" })
    const buttons = BUTTONS.map(function (item) {
      return el("button", {
        text: item.label,
        onclick: async function () {
          out.textContent = "记录中…"
          try {
            const body = await A.postJson("/api/review/" + encodeURIComponent(action.problemId) + "/grade", {
              quality: item.quality,
            })
            out.textContent = ""
            out.appendChild(
              el("p", {
                class: "ok-text",
                text:
                  "记下了（" + item.label + "）。下次复习：" +
                  String(body.nextReviewAt).slice(0, 10) +
                  "，间隔 " + body.intervalDays + " 天。",
              })
            )
            out.appendChild(el("p", { class: "muted", text: "点「换一张」继续。" }))
          } catch (err) {
            out.textContent = ""
            out.appendChild(el("p", { class: "error", text: String(err.message || err) }))
          }
        },
      })
    })

    return el("section", { class: "panel hero" }, [
      el("h2", { text: "该复习这道" }),
      el("div", { class: "kv" }, [
        el("div", {}, [document.createTextNode("题目："), el("span", { text: action.title })]),
        el("div", {}, [document.createTextNode("用时："), el("span", { text: "这一步大约 " + action.estimatedMinutes + " 分钟" })]),
      ]),
      el("p", { class: "reason", text: action.reason }),
      el("div", { class: "row" }, buttons),
      out,
    ])
  }

  A.pages.review = {
    title: "复习",
    buttons: BUTTONS,
    render: async function (view) {
      view.textContent = ""
      view.appendChild(el("div", { class: "panel", text: "加载中…" }))
      let next = null
      try {
        next = await A.getJson("/api/next")
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
            eyebrow: "复习",
            title: "间隔复习",
            sub: "按 SM-2 排期：答得越顺，间隔拉得越长。给四个直觉档，间隔算法自己算。",
          })
        )
      }
      const action = next && next.action
      view.appendChild(queuePanel(next))
      if (action && action.kind === "review") {
        view.appendChild(cardPanel(action))
        view.appendChild(
          el("div", { class: "row" }, [
            el("button", {
              text: "换一张",
              onclick: function () {
                A.go("review")
              },
            }),
          ])
        )
      } else {
        const hint =
          "没有到期的卡片，或者今天的复习配额已经用完。回到「今日」页做别的吧。"
        view.appendChild(
          UI
            ? UI.empty("暂时没有要复习的卡片", hint)
            : el("section", { class: "panel" }, [
                el("h2", { text: "暂时没有要复习的卡片" }),
                el("p", { class: "muted", text: hint }),
              ])
        )
      }
    },
  }
})()
