"use strict"
/**
 * 面试追踪 —— 公司列表 / 状态流转 / 面试时间线
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const date = require("../../../../../shared/core/date.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "面试追踪",
    statuses: khc.INTERVIEW_STATUSES,
    recTypes: khc.INTERVIEW_TYPES,
    list: [],
    current: null,
    showAdd: false,
    showRecord: false,
    form: { name: "", position: "", salary: "", notes: "" },
    recForm: { type: "一面", content: "" }
  },

  render: function (state) {
    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "面试追踪", sub: "公司状态流转与面试时间线" })
    ]

    if (state.current) {
      children.push(this.renderDetail(state))
    } else {
      const rows = (state.list || []).map(function (c) {
        const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
          ui.el("div", { class: "kh-list-main" }, [
            ui.el("div", { class: "kh-list-title", text: c.name + " · " + c.position }),
            ui.el("div", { class: "kh-list-sub", text: c.timelineCount + " 条记录 · " + c.updated })
          ]),
          ui.pill(c.status, "accent")
        ])
        button.addEventListener("click", function () {
          this.onOpen({ currentTarget: { dataset: { id: c.id } } })
        }.bind(this))
        return button
      })

      const add = ui.el("button", {
        class: "kh-btn primary",
        text: "添加公司",
        onclick: function () { this.onOpenAdd() }.bind(this)
      })

      children.push(ui.panel({
        title: "公司列表 · " + (state.list || []).length + " 家",
        children: [
          ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("还没有投递记录", "添加第一家公司")]),
          ui.el("div", { class: "kh-actions" }, [add])
        ]
      }))
    }

    if (state.showAdd) children.push(this.renderAddForm(state))
    if (state.showRecord) children.push(this.renderRecordForm(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 公司详情 */
  renderDetail: function (state) {
    const c = state.current
    const statusButtons = (state.statuses || []).map(function (s) {
      const button = ui.el("button", {
        class: "kh-chip" + (c.status === s.key ? " is-active" : ""),
        text: s.label
      })
      button.addEventListener("click", function () {
        this.onChangeStatus({ currentTarget: { dataset: { status: s.key } } })
      }.bind(this))
      return button
    })

    const timeline = (c.timeline || []).map(function (r) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: r.typeLabel + " · " + r.date }),
          ui.el("div", { class: "kh-list-sub", text: r.content })
        ])
      ])
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "关闭", onclick: function () { this.onCloseDetail() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "记录一场", onclick: function () { this.onOpenRecord() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "复制面经", onclick: function () { this.onShareCompany() }.bind(this) }),
      ui.el("button", { class: "kh-btn danger", text: "删除", onclick: function () { this.onDelete() }.bind(this) })
    ]

    return ui.panel({
      title: c.name + " · " + c.position,
      children: [
        ui.el("div", { class: "kh-chips" }, statusButtons),
        ui.el("p", { class: "reason", text: "薪资 " + (c.salary || "—") + " · " + (c.notes || "无备注") }),
        ui.el("div", { class: "kh-list" }, timeline.length ? timeline : [ui.empty("还没有面试记录", "点「记录一场」补上")]),
        ui.el("div", { class: "kh-actions" }, actions)
      ]
    })
  },

  /** 新增公司表单 */
  renderAddForm: function (state) {
    const form = state.form || { name: "", position: "", salary: "", notes: "" }
    const fields = [
      { key: "name", label: "公司名" },
      { key: "position", label: "岗位" },
      { key: "salary", label: "薪资范围" },
      { key: "notes", label: "备注" }
    ]
    const inputs = fields.map(function (f) {
      const input = ui.el("input", { class: "kh-input", placeholder: f.label })
      input.value = form[f.key] || ""
      input.addEventListener("input", function (event) {
        this.onAddInput({ currentTarget: { dataset: { field: f.key } }, detail: { value: event.target.value } })
      }.bind(this))
      return input
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseAdd() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSaveCompany() }.bind(this) })
    ]

    return ui.panel({ title: "添加公司", children: [ui.el("div", { class: "kh-form" }, inputs), ui.el("div", { class: "kh-actions" }, actions)] })
  },

  /** 新增面试记录表单 */
  renderRecordForm: function (state) {
    const recForm = state.recForm || { type: "一面", content: "" }
    const typeButtons = (state.recTypes || []).map(function (t) {
      const button = ui.el("button", {
        class: "kh-chip" + (recForm.type === t.key ? " is-active" : ""),
        text: t.label
      })
      button.addEventListener("click", function () {
        this.onPickRecType({ currentTarget: { dataset: { type: t.key } } })
      }.bind(this))
      return button
    })

    const content = ui.el("textarea", { class: "kh-textarea", placeholder: "面试内容" })
    content.value = recForm.content || ""
    content.addEventListener("input", function (event) {
      this.onRecInput({ currentTarget: { dataset: { field: "content" } }, detail: { value: event.target.value } })
    }.bind(this))

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseRecord() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存记录", onclick: function () { this.onSaveRecord() }.bind(this) })
    ]

    return ui.panel({
      title: "记录一场面试",
      children: [ui.el("div", { class: "kh-chips" }, typeButtons), content, ui.el("div", { class: "kh-actions" }, actions)]
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
    const keep = this.data.current ? stores.interviews.find(this.data.current.id) : null
    const list = stores.interviews.getAll().map(function (c) {
      const meta = khc.interviewStatus(c.status)
      return {
        id: c.id,
        name: c.name,
        position: c.position,
        status: c.status,
        statusColor: meta.color,
        progress: meta.progress,
        salary: c.salary,
        notes: c.notes,
        timelineCount: (c.timeline || []).length,
        updated: date.formatRelative(c.createdAt)
      }
    })
    this.setData({ list: list, current: keep })
  },

  onOpen (e) {
    const id = e.currentTarget.dataset.id
    const c = stores.interviews.find(id)
    if (!c) return
    this.setData({
      current: {
        id: c.id,
        name: c.name,
        position: c.position,
        status: c.status,
        statusColor: khc.interviewStatus(c.status).color,
        salary: c.salary,
        notes: c.notes,
        timeline: (c.timeline || []).slice().reverse().map(function (r) {
          const meta = khc.INTERVIEW_TYPES.filter(function (t) { return t.key === r.type })[0]
          return {
            id: r.id,
            type: r.type,
            typeLabel: meta ? meta.label : r.type,
            content: r.content,
            date: r.date
          }
        })
      }
    })
  },

  onCloseDetail () {
    this.setData({ current: null })
  },

  /** 生成 markdown 面经并复制到剪贴板 */
  onShareCompany () {
    const c = this.data.current
    if (!c) return
    const full = stores.interviews.find(c.id)
    const lines = ["# " + full.name + " · " + full.position, ""]
    ;(full.timeline || []).forEach(function (r) {
      lines.push("## " + r.type + "（" + r.date + "）")
      lines.push(r.content)
      lines.push("")
    })
    if (full.notes) {
      lines.push("## 备注")
      lines.push(full.notes)
    }
    platform.setClipboardData({ data: lines.join("\n") }).then(function () {
      platform.showToast({ title: "面经已复制", icon: "none" })
    })
  },

  onChangeStatus (e) {
    const c = this.data.current
    if (!c) return
    stores.interviews.updateStatus(c.id, e.currentTarget.dataset.status)
    this.refresh()
  },

  // ---- 新增公司 ----
  onOpenAdd () {
    this.setData({ showAdd: true, form: { name: "", position: "", salary: "", notes: "" } })
  },

  onCloseAdd () {
    this.setData({ showAdd: false })
  },

  onAddInput (e) {
    const patch = {}
    patch["form." + e.currentTarget.dataset.field] = e.detail.value
    this.setData(patch)
  },

  onSaveCompany () {
    const f = this.data.form
    if (!f.name.trim()) {
      platform.showToast({ title: "请填写公司名", icon: "none" })
      return
    }
    stores.interviews.addCompany({
      name: f.name.trim(),
      position: f.position.trim(),
      salary: f.salary.trim(),
      notes: f.notes.trim(),
      status: "投递"
    })
    this.setData({ showAdd: false })
    this.refresh()
    platform.showToast({ title: "已添加", icon: "none" })
  },

  // ---- 添加面试记录 ----
  onOpenRecord () {
    this.setData({ showRecord: true, recForm: { type: "一面", content: "" } })
  },

  onCloseRecord () {
    this.setData({ showRecord: false })
  },

  onRecInput (e) {
    const patch = {}
    patch["recForm." + e.currentTarget.dataset.field] = e.detail.value
    this.setData(patch)
  },

  onPickRecType (e) {
    this.setData({ "recForm.type": e.currentTarget.dataset.type })
  },

  onSaveRecord () {
    const c = this.data.current
    const f = this.data.recForm
    if (!c || !f.content.trim()) {
      platform.showToast({ title: "请填写面试内容", icon: "none" })
      return
    }
    stores.interviews.addInterviewRecord(c.id, { type: f.type, content: f.content.trim(), date: date.todayStr() })
    this.setData({ showRecord: false })
    this.refresh()
    platform.showToast({ title: "已记录", icon: "none" })
  },

  onDelete () {
    const c = this.data.current
    if (!c) return
    const self = this
    platform.showModal({
      title: "删除公司",
      content: "将同时删除该公司的全部面试记录"
    }).then(function (res) {
      if (!res.confirm) return
      stores.interviews.remove(c.id)
      self.setData({ current: null })
      self.refresh()
    })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 面试追踪", path: "/knowledge/pages/interview/tracker/index" }
  }
})
