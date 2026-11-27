"use strict"
/**
 * learn —— 学习模块聚合入口（4 个模块）
 */

const { withTheme } = require("../../../../shared/core/with-theme.js")
const stores = require("../../../../shared/knowledge/stores.js")
const khc = require("../../../../shared/knowledge/kh-constants.js")
const seedData = require("../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../shared/ui.js")
const nav = require("../../nav.js")

/**
 * 取学习聚合数据。
 * @returns {object} 视图模型
 */
function build () {
  const plan = stores.quizzes.getTodayPlan()
  const counts = {
    quiz: stores.quizzes.getQuestionBank().length,
    review: stores.reviews.getAll().length,
    deep: seedData.LEARN_DEEP.length,
    path: stores.learningPaths.getAll().length
  }

  const entries = [
    {
      key: "quiz", symbol: "?", title: "题库练习", color: "#6366F1",
      desc: "7 大技术栈 · 每日计划 · 错题本", path: "/knowledge/pages/learn/quiz/index", count: counts.quiz
    },
    {
      key: "review", symbol: "R", title: "复习计划", color: "#0284C7",
      desc: "SM-2 间隔重复 · 到期队列", path: "/knowledge/pages/learn/review/index", count: counts.review
    },
    {
      key: "deep", symbol: "D", title: "图解系列", color: "#30A46C",
      desc: "Markdown 长文 · 按栈分类", path: "/knowledge/pages/learn/deep/index", count: counts.deep
    },
    {
      key: "path", symbol: "P", title: "学习路径", color: "#7F77DD",
      desc: "分步进阶 · 子任务进度", path: "/knowledge/pages/learn/path/index", count: counts.path
    }
  ]

  let total = 0
  Object.keys(counts).forEach(function (k) { total += counts[k] })

  return {
    entries: entries,
    total: total,
    plan: {
      stack: plan.stack,
      stackLabel: khc.stackMeta(plan.stack).label,
      done: plan.completed || 0,
      total: plan.questionIds.length
    }
  }
}

module.exports = withTheme({
  data: {
    title: "学习",
    entries: [],
    total: 0,
    plan: null
  },

  render: function (state) {
    const model = build()
    const cards = model.entries.map(function (e) {
      return ui.el("a", {
        class: "kh-entry",
        href: e.path,
        onclick: function (event) {
          if (event && typeof event.preventDefault === "function") event.preventDefault()
          this.onOpen({ currentTarget: { dataset: { path: e.path } } })
        }.bind(this)
      }, [
        ui.el("span", { class: "kh-entry-title", text: e.symbol + " · " + e.title }),
        ui.el("span", { class: "kh-entry-desc", text: e.desc }),
        ui.pill(String(e.count) + " 项", "accent")
      ])
    })

    const plan = model.plan || { stackLabel: "", done: 0, total: 0 }

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "Reading Radar", title: "学习", sub: "题库、复习、图解与路径" }),
      ui.hero({
        title: "今日计划 · " + plan.stackLabel,
        sub: "已完成 " + plan.done + " / " + plan.total + " 题",
        children: [
          ui.progressBar(plan.total ? Math.round(plan.done / plan.total * 100) : 0, { color: "#6366F1", label: "今日进度" }),
          ui.el("div", { class: "kh-actions" }, [
            ui.el("button", {
              class: "kh-btn primary",
              text: "去刷题",
              onclick: function () { this.onOpenPlan() }.bind(this)
            })
          ])
        ]
      }),
      ui.panel({ title: "四个模块", children: [ui.el("div", { class: "kh-entry-grid" }, cards)] })
    ])
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

  refresh () {
    this.setData(build())
  },

  onOpen (e) {
    const path = e.currentTarget.dataset.path
    if (path) nav.go(path)
  },

  onOpenPlan () {
    nav.go("/knowledge/pages/learn/quiz/index")
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 学习", path: "/knowledge/pages/learn/index" }
  }
})
