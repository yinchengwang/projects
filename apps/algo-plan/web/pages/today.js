"use strict";
/**
 * today.js —— 今日页
 *
 * 三块内容：
 *   1. 下一题卡片（GET /api/next）：什么动作、为什么、这一步大约几分钟
 *   2. 到期复习数
 *   3. 今日进度（GET /api/progress）：今天做了多少题、复习了多少、配额还剩多少
 *
 * 另外把运行状态（learning 仓连没连上）摆出来——写回不可用时要一眼看得出。
 */

(function () {
  const A = window.AlgoPlan
  const el = A.el

  /** 动作类型 → 中文 */
  const KIND_TEXT = {
    review: "到期复习",
    round: "推进轮次",
    explore: "主动探索",
  }

  /** 运行状态面板（M1 的健康面板搬到这里：learning 仓、题库规模、作答流水） */
  function renderHealth(health) {
    const learning = (health && health.learning) || {}
    const bank = (health && health.bank) || {}
    const badge = learning.ok
      ? el("span", { class: "badge ok", text: "已连接 learning 仓" })
      : el("span", { class: "badge warn", text: "纯本地模式" })
    const rows = [
      el("div", {}, [document.createTextNode("Node："), el("span", { text: (health && health.node) || "?" })]),
      el("div", {}, [document.createTextNode("题库题目数："), el("span", { text: String(bank.problemCount || 0) })]),
      el("div", {}, [document.createTextNode("作答流水条数："), el("span", { text: String((health && health.attempts) || 0) })]),
      el("div", {}, [document.createTextNode("learning 仓："), el("span", { text: learning.root || "（未连接）" })]),
    ]
    return el("section", { class: "panel" }, [
      el("h2", { text: "运行状态" }),
      el("div", { class: "kv" }, rows),
      el("p", { class: "muted", text: learning.reason || "" }),
      badge,
    ])
  }

  /** 下一题卡片 */
  function nextPanel(next) {
    const action = next && next.action
    if (!action) {
      return el("section", { class: "panel" }, [
        el("h2", { text: "下一步" }),
        el("p", { class: "muted", text: "现在没有该做的事：没有到期的复习卡片，也没有没做的轮次条目。" }),
      ])
    }
    const buttons = []
    if (action.problemId) {
      buttons.push(
        el("button", {
          class: "primary",
          text: action.kind === "review" ? "去复习这道" : "去作答",
          onclick: function () {
            A.state.problemId = action.problemId
            A.go(action.kind === "review" ? "review" : "answer")
          },
        })
      )
    }
    return el("section", { class: "panel" }, [
      el("h2", { text: "下一步" }),
      el("div", { class: "kv" }, [
        el("div", {}, [document.createTextNode("类型："), el("span", { text: KIND_TEXT[action.kind] || action.kind })]),
        el("div", {}, [document.createTextNode("题目："), el("span", { text: action.title })]),
        el("div", {}, [document.createTextNode("用时："), el("span", { text: "这一步大约 " + action.estimatedMinutes + " 分钟" })]),
        el("div", {}, [document.createTextNode("到期复习："), el("span", { text: action.reviewQueueLength + " 张" })]),
      ]),
      el("p", { class: "reason", text: action.reason }),
      el("div", { class: "row" }, buttons),
    ])
  }

  /** 今日进度 */
  function progressPanel(progress) {
    const quota = (progress && progress.quota) || {}
    const limit = Math.max(1, Number(quota.limitQuestions) || 1)
    const percent = Math.min(100, Math.round((Number(quota.questions) || 0) / limit * 100))
    const rows = [
      el("div", {}, [document.createTextNode("日期："), el("span", { text: (progress && progress.date) || "" })]),
      el("div", {}, [document.createTextNode("今天做题："), el("span", { text: ((progress && progress.solved) || 0) + " 道" })]),
      el("div", {}, [document.createTextNode("今天复习："), el("span", { text: ((progress && progress.reviewed) || 0) + " 张" })]),
      el("div", {}, [document.createTextNode("复习用时："), el("span", { text: ((progress && progress.reviewMinutes) || 0) + " 分钟" })]),
      el("div", {}, [
        document.createTextNode("复习配额："),
        el("span", {
          text:
            (quota.questions || 0) + "/" + (quota.limitQuestions || 0) + " 题，" +
            (quota.minutes || 0) + "/" + (quota.limitMinutes || 0) + " 分钟",
        }),
      ]),
    ]
    if (quota.exhausted) {
      rows.push(el("p", { class: "warn-text", text: "今天的复习配额用完了，接下来推新题。" }))
    }
    return el("section", { class: "panel" }, [
      el("h2", { text: "今日进度" }),
      el("div", { class: "kv" }, rows),
      el("div", { class: "bar" }, [el("div", { class: "fill", style: "width:" + percent + "%" })]),
    ])
  }

  A.pages.today = {
    title: "今日",
    render: async function (view) {
      view.textContent = ""
      view.appendChild(el("div", { class: "panel", text: "正在算今天该做什么…" }))
      let next = null
      let progress = null
      let health = null
      try {
        next = await A.getJson("/api/next")
        progress = await A.getJson("/api/progress")
        health = await A.getJson("/api/health")
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
      view.appendChild(nextPanel(next))
      view.appendChild(progressPanel(progress))
      view.appendChild(renderHealth(health))
    },
  }

  // Node（单测）下导出渲染函数，老测试直接用同一份实现
  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      render: A.pages.today.render,
      renderHealth: renderHealth,
    }
  }
})()
