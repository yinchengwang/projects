"use strict"
/**
 * 我的 —— 资料 / 主题 / 数据管理
 *
 * 小程序版用 wx.setClipboardData / wx.getFileSystemManager 做数据备份，
 * Web 侧统一走 storage.js 的 exportAll / importAll / clearAll + platform 剪贴板。
 */

const { withTheme } = require("../../../../shared/core/with-theme.js")
const stores = require("../../../../shared/knowledge/stores.js")
const storage = require("../../../../shared/core/storage.js")
const constants = require("../../../../shared/core/constants.js")
const ui = require("../../../../shared/ui.js")
const platform = require("../../../../shared/platform.js")
const syncTheme = require("../../theme-bridge.js").syncTheme

const THEME_MODES = constants.THEME_MODES
const PRIMARY_COLORS = constants.PRIMARY_COLORS

module.exports = withTheme({
  data: {
    title: "我的",
    user: null,
    settings: null,
    themeModes: THEME_MODES,
    primaryColors: PRIMARY_COLORS,
    usage: { keys: 0, currentSize: 0 },
    showEdit: false,
    form: { name: "", bio: "", email: "" },
    importText: ""
  },

  render: function (state) {
    const user = state.user || { name: "Yinch", bio: "", avatar: "Y" }
    const settings = state.settings || { theme: "auto", primaryColor: "violet" }

    const themeButtons = (state.themeModes || THEME_MODES).map(function (t) {
      const button = ui.el("button", {
        class: "kh-chip" + (settings.theme === t.key ? " is-active" : ""),
        text: t.label
      })
      button.addEventListener("click", function () {
        this.onPickTheme({ currentTarget: { dataset: { key: t.key } } })
      }.bind(this))
      return button
    })

    const colorButtons = (state.primaryColors || PRIMARY_COLORS).map(function (c) {
      const button = ui.el("button", {
        class: "kh-chip" + (settings.primaryColor === c.key ? " is-active" : ""),
        text: c.label,
        "data-color": c.light
      })
      button.addEventListener("click", function () {
        this.onPickPrimary({ currentTarget: { dataset: { key: c.key } } })
      }.bind(this))
      return button
    })

    const usage = state.usage || { keys: 0, currentSize: 0 }

    const actions = [
      ui.el("button", { class: "kh-btn", text: "编辑资料", onclick: function () { this.onOpenEdit() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "导出备份", onclick: function () { this.onExport() }.bind(this) }),
      ui.el("button", { class: "kh-btn", text: "粘贴备份并导入", onclick: function () { this.onImport() }.bind(this) }),
      ui.el("button", { class: "kh-btn danger", text: "清空全部数据", onclick: function () { this.onClear() }.bind(this) })
    ]

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "我的", sub: "资料、外观与本机数据管理" }),
      ui.panel({
        title: "资料",
        children: [
          ui.el("div", { class: "kh-profile" }, [
            ui.el("div", { class: "kh-avatar", text: user.avatar || "Y" }),
            ui.el("div", {}, [
              ui.el("div", { class: "kh-list-title", text: user.name || "Yinch" }),
              ui.el("div", { class: "kh-list-sub", text: user.bio || "Reading Radar 学习者" })
            ])
          ]),
          ui.el("p", { class: "kh-list-sub", text: "本机存储占用 " + usage.currentSize + " KB · " + usage.keys + " 个键" })
        ]
      }),
      ui.panel({ title: "主题模式", children: [ui.el("div", { class: "kh-chips" }, themeButtons)] }),
      ui.panel({ title: "主色", children: [ui.el("div", { class: "kh-chips" }, colorButtons)] }),
      ui.panel({ title: "数据管理", children: [ui.el("div", { class: "kh-actions" }, actions)] })
    ]

    if (state.showEdit) children.push(this.renderEditForm(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 资料编辑表单 */
  renderEditForm: function (state) {
    const form = state.form || { name: "", bio: "", email: "" }
    const fields = [
      { key: "name", label: "昵称" },
      { key: "email", label: "邮箱" },
      { key: "bio", label: "简介" }
    ]
    const inputs = fields.map(function (f) {
      const node = ui.el("input", { class: "kh-input", placeholder: f.label })
      node.value = form[f.key] || ""
      node.addEventListener("input", function (event) {
        this.onEditInput({ currentTarget: { dataset: { field: f.key } }, detail: { value: event.target.value } })
      }.bind(this))
      return node
    })

    const actions = [
      ui.el("button", { class: "kh-btn", text: "取消", onclick: function () { this.onCloseEdit() }.bind(this) }),
      ui.el("button", { class: "kh-btn primary", text: "保存", onclick: function () { this.onSaveProfile() }.bind(this) })
    ]

    return ui.panel({ title: "编辑资料", children: [ui.el("div", { class: "kh-form" }, inputs), ui.el("div", { class: "kh-actions" }, actions)] })
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
    const user = stores.user.get()
    const settings = stores.settings.get()
    this.setData({
      user: user,
      settings: settings,
      usage: storage.usage()
    })
  },

  /* ---- 资料 ---- */
  onOpenEdit () {
    const u = this.data.user
    this.setData({ showEdit: true, form: { name: u.name || "", bio: u.bio || "", email: u.email || "" } })
  },

  onCloseEdit () {
    this.setData({ showEdit: false })
  },

  onEditInput (e) {
    const patch = {}
    patch["form." + e.currentTarget.dataset.field] = e.detail.value
    this.setData(patch)
  },

  onSaveProfile () {
    const f = this.data.form
    if (f.name.trim()) stores.user.setName(f.name.trim())
    if (f.email.trim()) stores.user.setEmail(f.email.trim())
    stores.user.setBio(f.bio.trim())
    this.setData({ showEdit: false })
    this.refresh()
    platform.showToast({ title: "已保存", icon: "none" })
  },

  /* ---- 主题 ---- */
  onPickTheme (e) {
    stores.settings.patch({ theme: e.currentTarget.dataset.key })
    this.refresh()
    syncTheme(this)
  },

  onPickPrimary (e) {
    stores.settings.patch({ primaryColor: e.currentTarget.dataset.key })
    this.refresh()
    syncTheme(this)
  },

  /* ---- 数据 ---- */
  onExport () {
    const json = storage.exportAll()
    platform.setClipboardData({ data: json }).then(function () {
      platform.showToast({ title: "备份已复制到剪贴板", icon: "none" })
    })
  },

  onImportInput (e) {
    this.setData({ importText: e.detail.value })
  },

  onImport () {
    const self = this
    platform.getClipboardData().then(function (res) {
      const text = String((res && res.data) || "").trim()
      if (!text) {
        platform.showToast({ title: "剪贴板为空", icon: "none" })
        return
      }
      self.doImport(text)
    })
  },

  /**
   * 执行导入（merge）。
   * @param {string} text 备份 JSON
   */
  doImport (text) {
    const res = storage.importAll(text, "merge")
    if (!res.ok) {
      platform.showToast({ title: res.error || "导入失败", icon: "none" })
      return
    }
    this.setData({ importText: "" })
    this.refresh()
    platform.showToast({ title: "已导入 " + res.count + " 项", icon: "none" })
  },

  onClear () {
    const self = this
    platform.showModal({
      title: "清空数据",
      content: "将删除本应用全部本地数据，不可恢复"
    }).then(function (res) {
      if (!res.confirm) return
      storage.clearAll()
      self.refresh()
      syncTheme(self)
      platform.showToast({ title: "已清空", icon: "none" })
    })
  },

  onShareAppMessage () {
    return { title: "Reading Radar", path: "/knowledge/pages/profile/index" }
  }
})
