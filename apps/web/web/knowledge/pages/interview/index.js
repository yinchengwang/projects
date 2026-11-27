"use strict"
/**
 * 面试 —— 3 个模块聚合入口
 */

const { withTheme } = require("../../../../shared/core/with-theme.js")
const stores = require("../../../../shared/knowledge/stores.js")
const khc = require("../../../../shared/knowledge/kh-constants.js")
const seedData = require("../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../shared/ui.js")
const nav = require("../../nav.js")

/**
 * 取面试聚合数据。
 * @returns {object} 视图模型
 */
function build () {
  const derive = stores.interviews.derive()
  const companies = stores.interviews.getAll()
  const mocks = stores.mockInterviews.getAll()
  const bank = seedData.INTERVIEW_BANK
  const bookmarkCount = stores.interviewBank.get().bookmarks.length

  const upcoming = companies
    .filter(function (c) { return c.status !== "offer" && c.status !== "拒绝" })
    .slice(0, 3)
    .map(function (c) {
      const meta = khc.interviewStatus(c.status)
      return {
        id: c.id,
        name: c.name,
        position: c.position,
        status: c.status,
        color: meta.color,
        progress: meta.progress
      }
    })

  return {
    stats: { companies: derive.total, active: derive.active, mocks: mocks.length, bookmarks: bookmarkCount },
    upcoming: upcoming,
    entries: [
      {
        key: "tracker", symbol: "T", title: "面试追踪", color: "#6366F1",
        desc: "公司 · 状态流转 · 时间线", path: "/knowledge/pages/interview/tracker/index", count: derive.total
      },
      {
        key: "bank", symbol: "Q", title: "面试题库", color: "#0284C7",
        desc: "算法 + 后端题 · 收藏与浏览记录", path: "/knowledge/pages/interview/bank/index",
        count: bank.algorithm.length + bank.interview.length
      },
      {
        key: "mock", symbol: "M", title: "模拟面试", color: "#7F77DD",
        desc: "场景答题 · 评分报告", path: "/knowledge/pages/interview/mock/index", count: mocks.length
      }
    ]
  }
}

module.exports = withTheme({
  data: {
    title: "面试",
    entries: [],
    stats: null,
    upcoming: []
  },

  render: function (state) {
    const model = build()
    const stats = model.stats || { companies: 0, active: 0, mocks: 0, bookmarks: 0 }

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

    const upcomingRows = model.upcoming.map(function (c) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: c.name }),
          ui.el("div", { class: "kh-list-sub", text: c.position })
        ]),
        ui.pill(c.status, "accent")
      ])
    })

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "Reading Radar", title: "面试", sub: "追踪进度、刷题库、做模拟面试" }),
      ui.statGrid([
        { label: "在投公司", value: String(stats.active) },
        { label: "全部公司", value: String(stats.companies) },
        { label: "模拟面试", value: String(stats.mocks) },
        { label: "题库收藏", value: String(stats.bookmarks) }
      ]),
      ui.panel({ title: "三个模块", children: [ui.el("div", { class: "kh-entry-grid" }, cards)] }),
      ui.panel({
        title: "进行中的面试",
        children: upcomingRows.length ? upcomingRows : [ui.empty("没有进行中的面试", "去面试追踪里加一家公司")]
      })
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

  onShareAppMessage () {
    return { title: "Reading Radar · 面试", path: "/knowledge/pages/interview/index" }
  }
})
