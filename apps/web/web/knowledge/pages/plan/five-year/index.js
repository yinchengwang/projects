"use strict"
/**
 * 五年计划 —— 年份切换 / 月历网格 / 习惯打卡 / 连续天数
 *
 * 月历是纯 DOM 网格（非 canvas），原版即可直接复刻。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const date = require("../../../../../shared/core/date.js")
const seedData = require("../../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

const WEEK = ["日", "一", "二", "三", "四", "五", "六"]

module.exports = withTheme({
  data: {
    title: "五年计划",
    years: seedData.ALL_YEARS,
    year: seedData.ALL_YEARS[0],
    month: 1,
    weeks: WEEK,
    days: [],
    habits: [],
    config: null,
    streak: 0,
    totalDays: 0,
    monthPct: 0,
    current: null,
    showEdit: false,
    editForm: { note: "", hasLearning: false },
    projects: []
  },

  render: function (state) {
    const year = state.year
    const month = state.month
    const config = state.config || { theme: "", core: "", actions: [], extras: [], habits: [] }

    const shiftYear = [
      ui.el("button", { class: "kh-btn", text: "上一年", onclick: function () { this.onShiftYear({ currentTarget: { dataset: { delta: "-1" } } }) }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "下一年", onclick: function () { this.onShiftYear({ currentTarget: { dataset: { delta: "1" } } }) }.bind(this) })
    ]
    const shiftMonth = [
      ui.el("button", { class: "kh-btn", text: "上月", onclick: function () { this.onShiftMonth({ currentTarget: { dataset: { delta: "-1" } } }) }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "下月", onclick: function () { this.onShiftMonth({ currentTarget: { dataset: { delta: "1" } } }) }.bind(this) })
    ]

    const habitButtons = (state.habits || []).map(function (h) {
      const button = ui.el("button", { class: "kh-chip", text: h.icon + " " + h.label })
      button.addEventListener("click", function () {
        this.onQuickCheck({ currentTarget: { dataset: { habit: h.id } } })
      }.bind(this))
      return button
    })

    const weekHead = (state.weeks || WEEK).map(function (w) {
      return ui.el("span", { class: "kh-week-cell", text: w })
    })

    const calendarRows = (state.days || []).map(function (row) {
      const cells = row.map(function (cell) {
        if (!cell) return ui.el("span", { class: "kh-day-cell is-blank" })
        const button = ui.el("button", {
          class: "kh-day-cell"
            + (cell.isToday ? " is-today" : "")
            + (cell.hasLearning ? " has-learning" : ""),
          text: String(cell.day)
        })
        button.addEventListener("click", function () {
          this.onOpenDay({ currentTarget: { dataset: { day: String(cell.day) } } })
        }.bind(this))
        return button
      })
      return ui.el("div", { class: "kh-week-row" }, cells)
    })

    const projectRows = (state.projects || []).map(function (p) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: p.name }),
          ui.el("div", { class: "kh-list-sub", text: p.desc })
        ])
      ])
    })

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "五年计划", sub: year + " 年 · " + month + " 月" }),
      ui.statGrid([
        { label: "连续打卡", value: String(state.streak) },
        { label: "本月完成度", value: state.monthPct + "%" },
        { label: "累计打卡天", value: String(state.totalDays) }
      ]),
      ui.panel({
        title: year + " · " + (config.theme || ""),
        children: [
          ui.el("p", { class: "reason", text: config.core || "" }),
          ui.el("div", { class: "kh-actions" }, shiftYear),
          ui.el("div", { class: "kh-actions" }, shiftMonth),
          ui.el("div", { class: "kh-chips" }, habitButtons)
        ]
      }),
      ui.panel({ title: "月历", children: [ui.el("div", { class: "kh-week-head" }, weekHead)].concat(calendarRows) }),
      ui.panel({
        title: year + " 核心项目",
        children: projectRows.length ? projectRows : [ui.empty("这一年没有配置项目", "去 seed-data.js 补一条")]
      })
    ]

    if (state.showEdit) children.push(this.renderDayEditor(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 当日编辑面板 */
  renderDayEditor: function (state) {
    const form = state.editForm || { note: "", hasLearning: false }
    const note = ui.el("textarea", { class: "kh-textarea", placeholder: "今天学了什么" })
    note.value = form.note || ""
    note.addEventListener("input", function (event) {
      this.onEditInput({ detail: { value: event.target.value } })
    }.bind(this))

    const actions = [
      ui.el("button", { class: "kh-btn", text: "关闭", onclick: function () { this.onCloseEdit() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: form.hasLearning ? "取消学习标记" : "标记为学习", onclick: function () { this.onToggleLearning() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSaveDay() }.bind(this) }),
      ui.el("button", { class: "kh-btn danger", text: "删除当天", onclick: function () { this.onDeleteDay() }.bind(this) })
    ]

    return ui.panel({
      title: state.month + " 月 " + state.current + " 日",
      children: [note, ui.el("div", { class: "kh-actions" }, actions)]
    })
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    if (this.data.month === 1 && !this.__monthInit) {
      const now = new Date()
      this.setData({ year: now.getFullYear(), month: now.getMonth() + 1 })
    }
    this.__monthInit = true
    this.refresh()
    syncTheme(this)
  },

  onLoad () {
    const now = new Date()
    this.setData({ year: now.getFullYear(), month: now.getMonth() + 1 })
    this.__monthInit = true
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const year = this.data.year
    const month = this.data.month
    const config = seedData.getYearConfig(year)

    // 生成月历网格（补齐前后空位）
    const first = new Date(year, month - 1, 1)
    const daysInMonth = new Date(year, month, 0).getDate()
    const lead = first.getDay()
    const cells = []
    for (let i = 0; i < lead; i++) cells.push(null)
    for (let d = 1; d <= daysInMonth; d++) {
      const checks = stores.fiveYearPlan.getDay(year, month, d)
      const done = checks ? Object.keys(checks.checks || {}).filter(function (k) { return checks.checks[k] }).length : 0
      const total = config ? config.habits.length : 0
      cells.push({
        day: d,
        done: done,
        total: total,
        ratio: total ? done / total : 0,
        fillPct: total ? Math.round(done / total * 100) : 0,
        hasLearning: checks ? !!checks.hasLearning : false,
        isToday: date.formatYMD(new Date(year, month - 1, d)) === date.todayStr()
      })
    }
    while (cells.length % 7 !== 0) cells.push(null)

    const days = []
    for (let i = 0; i < cells.length; i += 7) days.push(cells.slice(i, i + 7))

    this.setData({
      days: days,
      config: config,
      habits: config ? config.habits : [],
      streak: stores.fiveYearPlan.getStreak(),
      totalDays: stores.fiveYearPlan.getTotalDays(),
      monthPct: stores.fiveYearPlan.getMonthPct(year, month),
      projects: seedData.CORE_PROJECTS.filter(function (p) { return p.year === year })
    })
  },

  onShiftYear (e) {
    const delta = Number(e.currentTarget.dataset.delta)
    this.setData({ year: this.data.year + delta })
    this.refresh()
  },

  onShiftMonth (e) {
    const delta = Number(e.currentTarget.dataset.delta)
    let m = this.data.month + delta
    let y = this.data.year
    if (m < 1) { m = 12; y -= 1 }
    if (m > 12) { m = 1; y += 1 }
    this.setData({ month: m, year: y })
    this.refresh()
  },

  /** 今日快速打卡 */
  onQuickCheck (e) {
    const ds = e.currentTarget.dataset
    const now = new Date()
    stores.fiveYearPlan.toggleCheck(this.data.year, this.data.month, now.getDate(), ds.habit)
    stores.fiveYearPlan.toggleLearning(this.data.year, this.data.month, now.getDate())
    this.refresh()
  },

  onOpenDay (e) {
    const day = Number(e.currentTarget.dataset.day)
    const checks = stores.fiveYearPlan.getDay(this.data.year, this.data.month, day)
    this.setData({
      current: day,
      showEdit: true,
      editForm: {
        note: checks ? checks.note : "",
        hasLearning: checks ? !!checks.hasLearning : false
      }
    })
  },

  onCloseEdit () {
    this.setData({ showEdit: false, current: null })
  },

  onEditInput (e) {
    this.setData({ "editForm.note": e.detail.value })
  },

  onToggleLearning () {
    stores.fiveYearPlan.toggleLearning(this.data.year, this.data.month, this.data.current)
    const checks = stores.fiveYearPlan.getDay(this.data.year, this.data.month, this.data.current)
    this.setData({ "editForm.hasLearning": checks ? !!checks.hasLearning : false })
  },

  onSaveDay () {
    stores.fiveYearPlan.setDay(this.data.year, this.data.month, this.data.current, {
      note: this.data.editForm.note
    })
    this.setData({ showEdit: false, current: null })
    this.refresh()
  },

  onDeleteDay () {
    stores.fiveYearPlan.deleteDay(this.data.year, this.data.month, this.data.current)
    this.setData({ showEdit: false, current: null })
    this.refresh()
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 五年计划", path: "/knowledge/pages/plan/five-year/index" }
  }
})
