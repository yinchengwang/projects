"use strict"
/**
 * 摘抄管理 —— 搜索 / 按书籍·标签筛选 / 收藏 / 状态流转
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
    title: "摘抄管理",
    statuses: khc.EXCERPT_STATUS,
    query: "",
    filterBook: "",
    filterTag: "",
    books: [],
    tags: [],
    list: [],
    stats: null,
    current: null,
    showAdd: false,
    form: { content: "", book: "", tags: "", note: "" }
  },

  render: function (state) {
    const stats = state.stats || { total: 0, favorites: 0, reading: 0, mastered: 0, books: 0 }

    const bookChips = (state.books || []).map(function (b) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.filterBook === b ? " is-active" : ""),
        text: b
      })
      button.addEventListener("click", function () {
        this.onPickBook({ currentTarget: { dataset: { book: b } } })
      }.bind(this))
      return button
    })

    const search = ui.el("input", { class: "kh-input", type: "search", placeholder: "搜索内容或书名" })
    search.value = state.query || ""
    search.addEventListener("input", function (event) {
      this.onSearch({ detail: { value: event.target.value } })
    }.bind(this))

    const rows = (state.list || []).map(function (e) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: e.content }),
          ui.el("div", { class: "kh-list-sub", text: (e.book || "未归类") + " · " + e.statusLabel + " · " + e.date })
        ]),
        e.favorite ? ui.pill("已收藏", "warn") : ui.pill(e.statusLabel, "accent")
      ])
      button.addEventListener("click", function () {
        this.onOpen({ currentTarget: { dataset: { id: e.id } } })
      }.bind(this))
      return button
    })

    const add = ui.el("button", { class: "kh-btn primary", text: "新增摘抄", onclick: function () { this.onOpenAdd() }.bind(this) })

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "摘抄管理", sub: "书籍摘录、收藏与状态流转" }),
      ui.statGrid([
        { label: "全部", value: String(stats.total) },
        { label: "已收藏", value: String(stats.favorites) },
        { label: "在读", value: String(stats.reading) },
        { label: "精读", value: String(stats.mastered) }
      ]),
      ui.panel({
        title: "摘抄 · " + (state.list || []).length + " 条",
        children: [
          ui.el("div", { class: "kh-chips" }, bookChips),
          search,
          ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("没有匹配的摘抄", "换个书名或清空搜索")]),
          ui.el("div", { class: "kh-actions" }, [add])
        ]
      })
    ]

    if (state.current) children.push(this.renderDetail(state))
    if (state.showAdd) children.push(this.renderAddForm(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 详情 */
  renderDetail: function (state) {
    const c = state.current
    const fav = ui.el("button", {
      class: "kh-btn" + (c.favorite ? " primary" : ""),
      text: c.favorite ? "取消收藏" : "收藏",
      onclick: function () { this.onToggleFav({ currentTarget: { dataset: { id: c.id } } }) }.bind(this)
    })
    const cycle = ui.el("button", {
      class: "kh-btn",
      text: "切换状态（当前：" + c.statusLabel + "）",
      onclick: function () { this.onCycleStatus({ currentTarget: { dataset: { id: c.id } } }) }.bind(this)
    })
    const copy = ui.el("button", { class: "kh-btn", text: "复制", onclick: function () { this.onCopy() }.bind(this) })
    const del = ui.el("button", { class: "kh-btn danger", text: "删除", onclick: function () { this.onDelete({ currentTarget: { dataset: { id: c.id } } }) }.bind(this) })
    const close = ui.el("button", { class: "kh-btn", text: "关闭", onclick: function () { this.onClose() }.bind(this) })

    return ui.panel({
      title: c.book || "未归类",
      children: [
        ui.el("p", { class: "kh-quiz-stem", text: c.content }),
        c.note ? ui.el("p", { class: "reason", text: "笔记：" + c.note }) : null,
        ui.el("div", { class: "kh-actions" }, [fav, cycle, copy, del, close])
      ].filter(Boolean)
    })
  },

  /** 新增表单 */
  renderAddForm: function (state) {
    const form = state.form || { content: "", book: "", tags: "", note: "" }
    const fields = [
      { key: "content", label: "摘抄内容", long: true },
      { key: "book", label: "书名" },
      { key: "tags", label: "标签（逗号分隔）" },
      { key: "note", label: "笔记" }
    ]
    const inputs = fields.map(function (f) {
      const node = f.long
        ? ui.el("textarea", { class: "kh-textarea", placeholder: f.label })
        : ui.el("input", { class: "kh-input", placeholder: f.label })
      node.value = form[f.key] || ""
      node.addEventListener("input", function (event) {
        this.onAddInput({ currentTarget: { dataset: { field: f.key } }, detail: { value: event.target.value } })
      }.bind(this))
      return node
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseAdd() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSave() }.bind(this) })
    ]

    return ui.panel({ title: "新增摘抄", children: [ui.el("div", { class: "kh-form" }, inputs), ui.el("div", { class: "kh-actions" }, actions)] })
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
    const der = stores.excerpts.derive()
    const all = stores.excerpts.getAll()
    const q = (this.data.query || "").trim().toLowerCase()
    const fb = this.data.filterBook
    const ft = this.data.filterTag

    let list = all
    if (fb) list = list.filter(function (e) { return e.book === fb })
    if (ft) list = list.filter(function (e) { return (e.tags || []).indexOf(ft) >= 0 })
    if (q) {
      list = list.filter(function (e) {
        return (e.content || "").toLowerCase().indexOf(q) >= 0 ||
          (e.book || "").toLowerCase().indexOf(q) >= 0
      })
    }

    const keep = this.data.current ? stores.excerpts.find(this.data.current.id) : null

    this.setData({
      books: der.books,
      tags: der.tags,
      stats: der.stats,
      list: list.map(function (e) {
        const meta = khc.excerptStatus(e.status)
        return {
          id: e.id,
          content: e.content,
          book: e.book,
          note: e.note,
          favorite: e.favorite,
          status: e.status,
          statusLabel: meta.label,
          statusColor: meta.color,
          tags: e.tags || [],
          date: date.formatRelative(e.createdAt)
        }
      }),
      current: keep ? this.toDetail(keep) : null
    })
  },

  toDetail (e) {
    const meta = khc.excerptStatus(e.status)
    return {
      id: e.id,
      content: e.content,
      book: e.book,
      note: e.note,
      favorite: e.favorite,
      status: e.status,
      statusLabel: meta.label,
      statusColor: meta.color,
      tags: e.tags || [],
      date: e.createdAt ? e.createdAt.slice(0, 10) : ""
    }
  },

  onSearch (e) {
    this.setData({ query: e.detail.value })
    this.refresh()
  },

  onPickBook (e) {
    const b = e.currentTarget.dataset.book
    this.setData({ filterBook: this.data.filterBook === b ? "" : b })
    this.refresh()
  },

  onPickTag (e) {
    const t = e.currentTarget.dataset.tag
    this.setData({ filterTag: this.data.filterTag === t ? "" : t })
    this.refresh()
  },

  onOpen (e) {
    const ex = stores.excerpts.find(e.currentTarget.dataset.id)
    if (ex) this.setData({ current: this.toDetail(ex) })
  },

  onClose () {
    this.setData({ current: null })
  },

  onToggleFav (e) {
    const id = (e.currentTarget.dataset.id) || (this.data.current && this.data.current.id)
    if (!id) return
    stores.excerpts.toggleFavorite(id)
    this.refresh()
  },

  onCycleStatus (e) {
    const id = (e.currentTarget.dataset.id) || (this.data.current && this.data.current.id)
    if (!id) return
    const cur = stores.excerpts.find(id)
    const keys = khc.EXCERPT_STATUS.map(function (s) { return s.key })
    const next = keys[(keys.indexOf(cur.status) + 1) % keys.length]
    stores.excerpts.setStatus(id, next)
    this.refresh()
  },

  onCopy () {
    const c = this.data.current
    if (!c) return
    platform.setClipboardData({ data: c.content }).then(function () {
      platform.showToast({ title: "已复制", icon: "none" })
    })
  },

  onDelete (e) {
    const id = (e.currentTarget.dataset.id) || (this.data.current && this.data.current.id)
    if (!id) return
    const self = this
    platform.showModal({
      title: "删除摘抄",
      content: "确定删除这条摘抄？"
    }).then(function (res) {
      if (!res.confirm) return
      stores.excerpts.remove(id)
      self.setData({ current: null })
      self.refresh()
    })
  },

  // ---- 新增 ----
  onOpenAdd () {
    this.setData({ showAdd: true, form: { content: "", book: "", tags: "", note: "" } })
  },

  onCloseAdd () {
    this.setData({ showAdd: false })
  },

  onAddInput (e) {
    const patch = {}
    patch["form." + e.currentTarget.dataset.field] = e.detail.value
    this.setData(patch)
  },

  onSave () {
    const f = this.data.form
    if (!f.content.trim()) {
      platform.showToast({ title: "请填写摘抄内容", icon: "none" })
      return
    }
    stores.excerpts.addExcerpt({
      content: f.content.trim(),
      book: f.book.trim(),
      tags: f.tags.split(",").map(function (t) { return t.trim() }).filter(Boolean),
      note: f.note.trim(),
      status: "reading"
    })
    this.setData({ showAdd: false })
    this.refresh()
    platform.showToast({ title: "已添加", icon: "none" })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 摘抄管理", path: "/knowledge/pages/knowledge/excerpt/index" }
  }
})
