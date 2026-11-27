"use strict"
/**
 * 项目路线 —— 学习项目追踪（study_projects）
 *
 * 注意：这里的 project 是「学习项目」，与 programmer-workbench 的
 * 作品集 projects 同名异义，故存储键为 db:study_projects。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "项目路线",
    statuses: khc.PROJECT_STATUSES,
    priorities: khc.PROJECT_PRIORITIES,
    filter: "",
    list: [],
    stats: null,
    current: null,
    showAdd: false,
    form: { name: "", description: "", status: "规划中", priority: "中", tags: "" }
  },

  render: function (state) {
    const stats = state.stats || { total: 0, doing: 0, done: 0 }

    const filterChips = (state.statuses || []).map(function (s) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.filter === s.key ? " is-active" : ""),
        text: s.label,
        "data-color": s.color
      })
      button.addEventListener("click", function () {
        this.onPickFilter({ currentTarget: { dataset: { status: s.key } } })
      }.bind(this))
      return button
    })

    const rows = (state.list || []).map(function (p) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: p.name }),
          ui.el("div", { class: "kh-list-sub", text: p.description })
        ]),
        ui.pill(p.statusLabel, "accent"),
        ui.pill(p.priorityLabel, "warn")
      ])
      button.addEventListener("click", function () {
        this.onOpen({ currentTarget: { dataset: { id: p.id } } })
      }.bind(this))
      return button
    })

    const add = ui.el("button", { class: "kh-btn primary", text: "新增项目", onclick: function () { this.onOpenAdd() }.bind(this) })

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "项目路线", sub: "学习项目的状态与优先级" }),
      ui.statGrid([
        { label: "全部", value: String(stats.total) },
        { label: "进行中", value: String(stats.doing) },
        { label: "已完成", value: String(stats.done) }
      ]),
      ui.panel({
        title: "学习项目 · " + (state.list || []).length + " 个",
        children: [
          ui.el("div", { class: "kh-chips" }, filterChips),
          ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("没有匹配的项目", "换个状态筛选或新增")]),
          ui.el("div", { class: "kh-actions" }, [add])
        ]
      })
    ]

    if (state.current) children.push(this.renderDetail(state))
    if (state.showAdd) children.push(this.renderAddForm(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 项目详情 */
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

    const links = (c.knowledgeLinks || []).map(function (n) {
      return ui.pill(n.name, "accent")
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "关闭", onclick: function () { this.onClose() }.bind(this) }),
      ui.el("button", { class: "kh-btn danger", text: "删除", onclick: function () { this.onDelete({ currentTarget: { dataset: { id: c.id } } }) }.bind(this) })
    ]

    return ui.panel({
      title: c.name,
      children: [
        ui.el("p", { class: "reason", text: c.description }),
        ui.el("div", { class: "kh-chips" }, statusButtons),
        ui.el("div", { class: "kh-chips" }, links),
        ui.el("p", { class: "kh-list-sub", text: "创建 " + (c.createdAt || "—") + " · 更新 " + (c.updatedAt || "—") }),
        ui.el("div", { class: "kh-actions" }, actions)
      ]
    })
  },

  /** 新增表单 */
  renderAddForm: function (state) {
    const form = state.form || { name: "", description: "", status: "规划中", priority: "中", tags: "" }

    const name = ui.el("input", { class: "kh-input", placeholder: "项目名" })
    name.value = form.name
    name.addEventListener("input", function (event) {
      this.onAddInput({ currentTarget: { dataset: { field: "name" } }, detail: { value: event.target.value } })
    }.bind(this))

    const desc = ui.el("textarea", { class: "kh-textarea", placeholder: "描述" })
    desc.value = form.description
    desc.addEventListener("input", function (event) {
      this.onAddInput({ currentTarget: { dataset: { field: "description" } }, detail: { value: event.target.value } })
    }.bind(this))

    const tags = ui.el("input", { class: "kh-input", placeholder: "标签（逗号分隔）" })
    tags.value = form.tags
    tags.addEventListener("input", function (event) {
      this.onAddInput({ currentTarget: { dataset: { field: "tags" } }, detail: { value: event.target.value } })
    }.bind(this))

    const priorityButtons = (state.priorities || []).map(function (p) {
      const button = ui.el("button", {
        class: "kh-chip" + (form.priority === p.key ? " is-active" : ""),
        text: p.label
      })
      button.addEventListener("click", function () {
        this.onPickForm({ currentTarget: { dataset: { field: "priority", value: p.key } } })
      }.bind(this))
      return button
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseAdd() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSave() }.bind(this) })
    ]

    return ui.panel({
      title: "新增学习项目",
      children: [
        ui.el("div", { class: "kh-form" }, [name, desc, tags]),
        ui.el("div", { class: "kh-chips" }, priorityButtons),
        ui.el("div", { class: "kh-actions" }, actions)
      ]
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
    const all = stores.studyProjects.getAll()
    const filter = this.data.filter
    const list = (filter ? all.filter(function (p) { return p.status === filter }) : all)
      .map(function (p) {
        const st = khc.projectStatus(p.status)
        const pr = khc.projectPriority(p.priority)
        return {
          id: p.id,
          name: p.name,
          description: p.description,
          status: p.status,
          statusLabel: st.label,
          statusColor: st.color,
          priority: p.priority,
          priorityLabel: pr.label,
          priorityColor: pr.color,
          tags: p.tags || [],
          knowledgeLinks: p.knowledgeLinks || []
        }
      })

    const stats = {
      total: all.length,
      doing: all.filter(function (p) { return p.status === "进行中" }).length,
      done: all.filter(function (p) { return p.status === "已完成" }).length
    }

    const keep = this.data.current ? stores.studyProjects.find(this.data.current.id) : null
    this.setData({ list: list, stats: stats, current: keep ? this.toDetail(keep) : null })
  },

  toDetail (p) {
    const st = khc.projectStatus(p.status)
    const pr = khc.projectPriority(p.priority)
    const nodes = stores.knowledge.getAll()
    return {
      id: p.id,
      name: p.name,
      description: p.description,
      status: p.status,
      statusLabel: st.label,
      statusColor: st.color,
      priority: p.priority,
      priorityLabel: pr.label,
      priorityColor: pr.color,
      tags: p.tags || [],
      knowledgeLinks: (p.knowledgeLinks || []).map(function (kid) {
        const n = nodes.filter(function (x) { return x.id === kid })[0]
        return { id: kid, name: n ? n.name : kid }
      }),
      createdAt: p.createdAt ? p.createdAt.slice(0, 10) : "",
      updatedAt: p.updatedAt ? p.updatedAt.slice(0, 10) : ""
    }
  },

  onPickFilter (e) {
    const s = e.currentTarget.dataset.status
    this.setData({ filter: this.data.filter === s ? "" : s })
    this.refresh()
  },

  onOpen (e) {
    const p = stores.studyProjects.find(e.currentTarget.dataset.id)
    if (p) this.setData({ current: this.toDetail(p) })
  },

  onClose () {
    this.setData({ current: null })
  },

  onChangeStatus (e) {
    const id = (e.currentTarget.dataset.id) || (this.data.current && this.data.current.id)
    const status = e.currentTarget.dataset.status
    if (!id) return
    stores.studyProjects.updateProject(id, { status: status })
    this.refresh()
  },

  onDelete (e) {
    const id = (e.currentTarget.dataset.id) || (this.data.current && this.data.current.id)
    if (!id) return
    const self = this
    platform.showModal({
      title: "删除项目",
      content: "确定删除这个学习项目？"
    }).then(function (res) {
      if (!res.confirm) return
      stores.studyProjects.remove(id)
      self.setData({ current: null })
      self.refresh()
    })
  },

  onOpenAdd () {
    this.setData({ showAdd: true, form: { name: "", description: "", status: "规划中", priority: "中", tags: "" } })
  },

  onCloseAdd () {
    this.setData({ showAdd: false })
  },

  onAddInput (e) {
    const patch = {}
    patch["form." + e.currentTarget.dataset.field] = e.detail.value
    this.setData(patch)
  },

  onPickForm (e) {
    const ds = e.currentTarget.dataset
    const patch = {}
    patch["form." + ds.field] = ds.value
    this.setData(patch)
  },

  onSave () {
    const f = this.data.form
    if (!f.name.trim()) {
      platform.showToast({ title: "请填写项目名", icon: "none" })
      return
    }
    stores.studyProjects.addProject({
      name: f.name.trim(),
      description: f.description.trim(),
      status: f.status,
      priority: f.priority,
      tags: f.tags.split(",").map(function (t) { return t.trim() }).filter(Boolean)
    })
    this.setData({ showAdd: false })
    this.refresh()
    platform.showToast({ title: "已添加", icon: "none" })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 项目路线", path: "/knowledge/pages/plan/roadmap/index" }
  }
})
