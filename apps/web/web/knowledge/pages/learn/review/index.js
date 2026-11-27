"use strict"
/**
 * 复习计划 —— SM-2 间隔重复
 *
 * 三个视图：due（到期）/ all（全部）/ done（已掌握）
 * 答题后按 0-5 评分，交给 shared/knowledge/review.js 的 SM-2 计算下次复习。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

const TABS = [
  { key: "due", label: "待复习" },
  { key: "all", label: "全部" },
  { key: "done", label: "已掌握" }
]

const QUALITY = [
  { q: 0, label: "完全忘了", color: "#E5484D" },
  { q: 1, label: "很模糊", color: "#F97316" },
  { q: 2, label: "有点印象", color: "#F5A524" },
  { q: 3, label: "想起来了", color: "#EAB308" },
  { q: 4, label: "比较熟练", color: "#84CC16" },
  { q: 5, label: "完全掌握", color: "#30A46C" }
]

module.exports = withTheme({
  data: {
    title: "复习计划",
    tabs: TABS,
    tab: "due",
    list: [],
    current: null,
    revealed: false,
    stats: { due: 0, all: 0, done: 0 },
    qualities: QUALITY,
    showAdd: false,
    form: { question: "", answer: "", domain: "算法" }
  },

  render: function (state) {
    const tabs = (state.tabs || TABS).map(function (t) {
      const button = ui.el("button", {
        class: "kh-tab" + (state.tab === t.key ? " is-active" : ""),
        text: t.label
      })
      button.addEventListener("click", function () {
        this.onSwitchTab({ currentTarget: { dataset: { key: t.key } } })
      }.bind(this))
      return button
    })

    const stats = state.stats || { due: 0, all: 0, done: 0 }
    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "复习计划", sub: "SM-2 间隔重复，到期才复习" }),
      ui.statGrid([
        { label: "待复习", value: String(stats.due) },
        { label: "全部卡片", value: String(stats.all) },
        { label: "已掌握", value: String(stats.done) }
      ]),
      ui.el("div", { class: "kh-tabs" }, tabs),
      this.renderCurrent(state)
    ]

    if (state.showAdd) children.push(this.renderAddForm(state))
    else {
      const open = ui.el("button", {
        class: "kh-btn primary",
        text: "新增卡片",
        onclick: function () { this.onOpenAdd() }.bind(this)
      })
      children.push(ui.el("div", { class: "kh-actions" }, [open]))
    }

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 当前卡片 */
  renderCurrent: function (state) {
    const cur = state.current
    if (!cur) return ui.panel({ title: "当前卡片", children: [ui.empty("这个视图没有卡片", "换个标签或新增一张")] })

    const children = [
      ui.el("div", { class: "kh-quiz-meta" }, [
        ui.pill(cur.domain || "通用", "accent"),
        ui.pill("第 " + (cur.repetitions + 1) + " 次复习"),
        ui.pill("下次 " + (cur.next || "—"))
      ]),
      ui.el("p", { class: "kh-quiz-stem", text: cur.question })
    ]

    if (state.revealed) {
      children.push(ui.el("p", { class: "reason", text: cur.answer }))
      const buttons = (state.qualities || QUALITY).map(function (q) {
        const button = ui.el("button", { class: "kh-btn", text: q.q + " · " + q.label })
        button.addEventListener("click", function () {
          this.onRate({ currentTarget: { dataset: { q: String(q.q) } } })
        }.bind(this))
        return button
      })
      children.push(ui.el("div", { class: "kh-quality-row" }, buttons))
    } else {
      const reveal = ui.el("button", {
        class: "kh-btn primary",
        text: "显示答案",
        onclick: function () { this.onReveal() }.bind(this)
      })
      children.push(ui.el("div", { class: "kh-actions" }, [reveal]))
    }

    return ui.panel({ title: "当前卡片", children: children })
  },

  /** 新增卡片表单 */
  renderAddForm: function (state) {
    const form = state.form || { question: "", answer: "", domain: "算法" }
    const question = ui.el("input", { class: "kh-input", placeholder: "问题" })
    question.value = form.question
    question.addEventListener("input", function (event) {
      this.onInput({ currentTarget: { dataset: { field: "question" } }, detail: { value: event.target.value } })
    }.bind(this))

    const answer = ui.el("textarea", { class: "kh-textarea", placeholder: "答案" })
    answer.value = form.answer
    answer.addEventListener("input", function (event) {
      this.onInput({ currentTarget: { dataset: { field: "answer" } }, detail: { value: event.target.value } })
    }.bind(this))

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseAdd() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSaveCard() }.bind(this) })
    ]

    return ui.panel({
      title: "新增卡片",
      children: [ui.el("div", { class: "kh-form" }, [question, answer]), ui.el("div", { class: "kh-actions" }, actions)]
    })
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    this.refresh()
    syncTheme(this)
  },

  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  refresh () {
    const all = stores.reviews.getAll()
    const due = stores.reviews.getDue()
    const done = stores.reviews.getDone()

    let list = all
    if (this.data.tab === "due") list = due
    else if (this.data.tab === "done") list = done

    const rows = list.map(function (r) {
      return {
        id: r.id,
        question: r.question,
        answer: r.answer,
        domain: r.domain,
        repetitions: r.repetitions,
        interval: r.interval,
        easiness: Math.round(r.easiness * 100) / 100,
        next: r.nextReviewDate ? r.nextReviewDate.slice(0, 10) : ""
      }
    })

    this.setData({
      list: rows,
      current: rows.length ? rows[0] : null,
      revealed: false,
      stats: { due: due.length, all: all.length, done: done.length }
    })
    syncTheme(this)
  },

  onSwitchTab (e) {
    this.setData({ tab: e.currentTarget.dataset.key })
    this.refresh()
  },

  onReveal () {
    this.setData({ revealed: true })
  },

  onRate (e) {
    const q = Number(e.currentTarget.dataset.q)
    const cur = this.data.current
    if (!cur) return
    stores.reviews.rateReview(cur.id, q)
    stores.daily.recordReview(1)
    this.refresh()
    platform.vibrateShort({ type: "light" })
  },

  onNextCard () {
    const list = this.data.list
    if (!list.length) return
    const idx = this.data.list.findIndex(function (i) { return i.id === this.data.current.id }, this)
    const next = list[(idx + 1) % list.length]
    this.setData({ current: next, revealed: false })
  },

  // ---- 新增卡片 ----
  onOpenAdd () {
    this.setData({ showAdd: true, form: { question: "", answer: "", domain: "算法" } })
  },

  onCloseAdd () {
    this.setData({ showAdd: false })
  },

  onInput (e) {
    const field = e.currentTarget.dataset.field
    const value = e.detail.value
    const patch = {}
    patch["form." + field] = value
    this.setData(patch)
  },

  onSaveCard () {
    const f = this.data.form
    if (!f.question.trim() || !f.answer.trim()) {
      platform.showToast({ title: "请填写问题与答案", icon: "none" })
      return
    }
    stores.reviews.addRecord({ question: f.question.trim(), answer: f.answer.trim(), domain: f.domain || "通用" })
    this.setData({ showAdd: false })
    this.refresh()
    platform.showToast({ title: "已加入复习队列", icon: "none" })
  },

  onDelete (e) {
    const id = e.currentTarget.dataset.id
    const self = this
    platform.showModal({
      title: "删除卡片",
      content: "确定从复习队列中移除？"
    }).then(function (res) {
      if (!res.confirm) return
      stores.reviews.remove(id)
      self.refresh()
    })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 复习计划", path: "/knowledge/pages/learn/review/index" }
  }
})
