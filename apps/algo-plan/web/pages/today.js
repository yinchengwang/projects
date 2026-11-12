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

  /**
   * 公共界面构件。浏览器里 ui.js 先加载挂 window.AlgoPlanUI；
   * Node 下没有全局挂载，就 require 同一份文件——实现只有一份。
   */
  const UI =
    (typeof window !== "undefined" && (window.AlgoPlanUI || (window.AlgoPlan && window.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

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
    return el("section", { class: "panel quiet" }, [
      el("h2", { text: "运行状态" }),
      el("div", { class: "kv" }, rows),
      el("p", { class: "muted", text: learning.reason || "" }),
      badge,
    ])
  }

  /** 下一题卡片——这一页唯一要做的事，放在主卡里 */
  function nextPanel(next) {
    const action = next && next.action
    if (!action) {
      return el("section", { class: "panel hero" }, [
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
    } else if (action.roundId) {
      // 轮次卡片不一定在算法题库里（Grokking 卡片、Python 工具链……）。
      // 这种卡片的正确去处是轮次页——把它勾掉，而不是跳去一个空的作答页。
      buttons.push(
        el("button", {
          class: "primary",
          text: "去轮次页勾掉它",
          onclick: function () {
            A.state.currentRoundId = action.roundId
            A.state.focusItemId = action.itemId
            A.go("rounds")
          },
        })
      )
    }
    const label = action.problemId ? "题目：" : "卡片："
    const extra = action.problemId
      ? null
      : el("p", {
          class: "muted",
          text: "这张卡片不在算法题库里——它在轮次清单上，去轮次页把它勾掉。",
        })
    return el("section", { class: "panel hero" }, [
      el("h2", { text: "下一步" }),
      el("div", { class: "kv" }, [
        el("div", {}, [document.createTextNode("类型："), el("span", { text: KIND_TEXT[action.kind] || action.kind })]),
        el("div", {}, [document.createTextNode(label), el("span", { text: action.title })]),
        el("div", {}, [document.createTextNode("用时："), el("span", { text: "这一步大约 " + action.estimatedMinutes + " 分钟" })]),
        el("div", {}, [document.createTextNode("到期复习："), el("span", { text: action.reviewQueueLength + " 张" })]),
      ]),
      el("p", { class: "reason", text: action.reason }),
      extra,
      el("div", { class: "row" }, buttons),
    ])
  }

  /** 今日进度——四个数摆成数字块，比键值行更容易一眼看完 */
  function progressPanel(progress) {
    const quota = (progress && progress.quota) || {}
    const limit = Math.max(1, Number(quota.limitQuestions) || 1)
    const percent = Math.min(100, Math.round((Number(quota.questions) || 0) / limit * 100))
    const statItems = [
      { label: "今天做题", value: ((progress && progress.solved) || 0) + " 道" },
      { label: "今天复习", value: ((progress && progress.reviewed) || 0) + " 张" },
      { label: "复习用时", value: ((progress && progress.reviewMinutes) || 0) + " 分钟" },
      {
        label: "复习配额",
        value:
          (quota.questions || 0) + "/" + (quota.limitQuestions || 0) + " 题",
        hint:
          (quota.minutes || 0) + "/" + (quota.limitMinutes || 0) + " 分钟",
        tone: quota.exhausted ? "warn" : "",
      },
    ]
    const stats = UI
      ? UI.statGrid(statItems)
      : el("div", { class: "kv" }, statItems.map(function (item) {
          return el("div", {}, [
            document.createTextNode(item.label + "："),
            el("span", { text: String(item.value) }),
          ])
        }))
    const children = [
      el("h2", { text: "今日进度" }),
      stats,
      el("div", { class: "bar" }, [el("div", { class: "fill", style: "width:" + percent + "%" })]),
    ]
    if (quota.exhausted) {
      children.push(el("p", { class: "warn-text", text: "今天的复习配额用完了，接下来推新题。" }))
    }
    return el("section", { class: "panel" }, children)
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
      if (UI) {
        view.appendChild(
          UI.pageHead({
            eyebrow: "今日",
            title: "今天该做什么",
            sub: "下一步由到期复习、轮次清单和掌握度共同决定——做完记得在轮次页勾掉。",
          })
        )
      }
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
