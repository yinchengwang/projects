"use strict"
/**
 * dashboard —— 首页仪表盘
 *
 * 学习概览、今日任务、5 项统计、7 个技术栈进度、待补强的 3 个领域、4 个快捷入口。
 * 数据层在 shared/knowledge/stores.js，本文件只负责取数与渲染。
 */

const { withTheme } = require("../../../../shared/core/with-theme.js")
const stores = require("../../../../shared/knowledge/stores.js")
const date = require("../../../../shared/core/date.js")
const khc = require("../../../../shared/knowledge/kh-constants.js")
const ui = require("../../../../shared/ui.js")
const platform = require("../../../../shared/platform.js")

/**
 * 取首页数据。
 * @param {object} state 页面状态（只用到 isDark）
 * @returns {object} 视图模型
 */
function build (state) {
  const user = stores.user.get()
  const quizStats = stores.quizzes.getStats()
  const due = stores.reviews.getDue()
  const streak = stores.fiveYearPlan.getStreak()
  const plan = stores.quizzes.getTodayPlan()

  const gaps = stores.knowledge.getGaps()
  const gapRows = Object.keys(gaps).map(function (k) {
    const g = gaps[k]
    const meta = khc.KNOWLEDGE_DOMAINS.filter(function (d) { return d.key === k })[0]
    return {
      key: k,
      label: meta ? meta.label : k,
      color: meta ? meta.color : "#64748B",
      level: g.level,
      target: g.target,
      percent: g.target ? Math.min(100, Math.round(g.level / g.target * 100)) : 0
    }
  }).sort(function (a, b) { return a.percent - b.percent }).slice(0, 3)

  const stackProgress = khc.STACK_ORDER.map(function (s) {
    const meta = khc.stackMeta(s)
    const prog = stores.quizzes.getProgress(s)
    return {
      key: s,
      label: meta.label,
      icon: meta.icon,
      color: meta.color,
      total: prog.total,
      answered: prog.answered,
      percent: prog.total ? Math.round(prog.answered / prog.total * 100) : 0
    }
  })

  return {
    greeting: date.greeting() + "，" + (user.name || "同学"),
    todayLabel: date.todayLabel(),
    dueCount: due.length,
    streak: streak,
    plan: {
      stack: plan.stack,
      stackLabel: khc.stackMeta(plan.stack).label,
      total: plan.questionIds.length,
      completed: plan.completed || 0
    },
    planProgress: plan.questionIds.length
      ? Math.round((plan.completed || 0) / plan.questionIds.length * 100)
      : 0,
    stackProgress: stackProgress,
    gaps: gapRows,
    hasActivity: quizStats.totalAnswered > 0 || due.length > 0,
    stats: [
      { key: "answered", label: "累计答题", value: String(quizStats.totalAnswered), color: "#6366F1" },
      { key: "accuracy", label: "正确率", value: Math.round(quizStats.accuracy * 100) + "%", color: "#30A46C" },
      { key: "streak", label: "连续打卡", value: String(streak), color: "#F5A524" },
      { key: "due", label: "待复习", value: String(due.length), color: "#E5484D" },
      { key: "paths", label: "学习路径", value: String(stores.learningPaths.getAll().length), color: "#0284C7" }
    ],
    shortcuts: [
      { key: "quiz", symbol: "?", title: "题库练习", path: "/knowledge/pages/learn/quiz/index", color: "#6366F1" },
      { key: "review", symbol: "R", title: "复习计划", path: "/knowledge/pages/learn/review/index", color: "#0284C7" },
      { key: "deep", symbol: "D", title: "图解系列", path: "/knowledge/pages/learn/deep/index", color: "#30A46C" },
      { key: "path", symbol: "P", title: "学习路径", path: "/knowledge/pages/learn/path/index", color: "#7F77DD" }
    ]
  }
}

module.exports = withTheme({
  data: {
    title: "Reading Radar",
    greeting: "",
    todayLabel: "",
    stats: [],
    dueCount: 0,
    streak: 0,
    plan: null,
    planProgress: 0,
    stackProgress: [],
    gaps: [],
    shortcuts: [],
    hasActivity: false
  },

  render: function (state) {
    const model = build(state)

    const planPanel = ui.panel({
      title: "今日任务 · " + model.plan.stackLabel,
      children: [
        ui.el("p", {
          class: "reason",
          text: "今日计划 " + model.plan.total + " 题，已完成 " + model.plan.completed + " 题"
        }),
        ui.progressBar(model.planProgress, { color: "#6366F1", label: "今日进度" }),
        ui.el("div", { class: "kh-actions" }, [
          ui.el("button", {
            class: "kh-btn primary",
            text: "开始今日计划",
            onclick: function () { platform.navigateTo("/knowledge/pages/learn/quiz/index") }.bind(this)
          })
        ])
      ]
    })

    const stackRows = model.stackProgress.map(function (s) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("span", { class: "kh-stack-icon", text: s.icon, "data-color": s.color }),
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: s.label }),
          ui.progressBar(s.percent, { color: s.color })
        ]),
        ui.el("span", { class: "kh-list-sub", text: s.answered + " / " + s.total })
      ])
    })

    const gapRows = model.gaps.map(function (g) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: g.label }),
          ui.progressBar(g.percent, { color: g.color })
        ]),
        ui.el("span", { class: "kh-list-sub", text: g.level + " / " + g.target })
      ])
    })

    const shortcuts = model.shortcuts.map(function (s) {
      return ui.el("a", {
        class: "kh-entry",
        href: s.path,
        onclick: function (event) {
          if (event && typeof event.preventDefault === "function") event.preventDefault()
          this.onOpen({ currentTarget: { dataset: { path: s.path } } })
        }.bind(this)
      }, [
        ui.el("span", { class: "kh-entry-title", text: s.symbol + " · " + s.title }),
        ui.el("span", { class: "kh-entry-desc", text: "进入" + s.title })
      ])
    })

    const children = [
      ui.pageHead({ eyebrow: model.todayLabel, title: model.greeting, sub: "今天想学点什么？" }),
      ui.statGrid(model.stats.map(function (s) {
        return { label: s.label, value: s.value, hint: "" }
      })),
      planPanel,
      ui.panel({
        title: "技术栈进度",
        children: stackRows.length ? stackRows : [ui.empty("还没有答题记录", "从今日计划开始刷第一题")]
      }),
      ui.panel({
        title: "待补强",
        children: gapRows.length ? gapRows : [ui.empty("暂无知识节点", "去知识图谱里添加节点")]
      }),
      ui.panel({ title: "快捷入口", children: [ui.el("div", { class: "kh-entry-grid" }, shortcuts)] })
    ]

    return ui.el("div", { class: "kh-page" }, children)
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    this.refresh()
    this.refreshTheme()
  },

  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  /** 汇总首屏数据 */
  refresh () {
    this.setData(build(this.data))
  },

  onOpen (e) {
    const path = e.currentTarget.dataset.path
    if (!path) return
    platform.navigateTo(path)
  },

  onOpenPlan () {
    platform.navigateTo("/knowledge/pages/learn/quiz/index")
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 学习概览", path: "/knowledge/pages/dashboard/index" }
  }
})
