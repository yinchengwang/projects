"use strict"
/**
 * 图解系列 —— Markdown 长文阅读器
 *
 * 列表：按栈筛选 + 搜索；详情：就地展开全屏 panel 阅读（不增加页面数）。
 * Markdown 走 shared/core/md2html.js（Web 侧类名生效，不需要内联样式改写）。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const md = require("../../../../../shared/core/md.js")
const md2html = require("../../../../../shared/core/md2html.js").md2html
const seedData = require("../../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../../shared/ui.js")
const mdBlock = require("../../../md-block.js").mdBlock
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "图解系列",
    stacks: [],
    activeStack: "",
    query: "",
    list: [],
    reading: null,
    html: ""
  },

  render: function (state) {
    if (state.reading) return this.renderReading(state)

    const stackChips = (state.stacks || []).map(function (s) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.activeStack === s.key ? " is-active" : ""),
        text: s.label,
        "data-color": s.color
      })
      button.addEventListener("click", function () {
        this.onPickStack({ currentTarget: { dataset: { key: s.key } } })
      }.bind(this))
      return button
    })

    const search = ui.el("input", { class: "kh-input", type: "search", placeholder: "搜索标题或栈" })
    search.value = state.query || ""
    search.addEventListener("input", function (event) {
      this.onSearch({ detail: { value: event.target.value } })
    }.bind(this))

    const rows = (state.list || []).map(function (a) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: a.title }),
          ui.el("div", { class: "kh-list-sub", text: a.summary })
        ]),
        ui.pill(a.stackLabel, "accent")
      ])
      button.addEventListener("click", function () {
        this.onOpen({ currentTarget: { dataset: { slug: a.slug } } })
      }.bind(this))
      return button
    })

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "Reading Radar", title: "图解系列", sub: "按技术栈组织的 Markdown 长文" }),
      ui.panel({
        title: "文章 " + (state.list || []).length + " 篇",
        children: [
          ui.el("div", { class: "kh-chips" }, stackChips),
          search,
          ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("没有匹配的文章", "换个栈或清空搜索")])
        ]
      })
    ])
  },

  /** 阅读视图 */
  renderReading: function (state) {
    const close = ui.el("button", {
      class: "kh-btn",
      text: "返回列表",
      onclick: function () { this.onClose() }.bind(this)
    })
    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "图解系列", title: state.reading.title, sub: state.reading.difficulty || "" }),
      ui.panel({
        title: "正文",
        children: [
          mdBlock(state.html),
          ui.el("div", { class: "kh-actions" }, [close])
        ]
      })
    ])
  },

  onMount: function (rootElement) {
    this.rootElement = rootElement || this.rootElement
    if (!this.data.stacks.length) this.setData({ stacks: khc.STACK_META })
    this.refresh()
    syncTheme(this)
  },

  onLoad () {
    this.setData({ stacks: khc.STACK_META })
    this.refresh()
  },

  refresh () {
    const stack = this.data.activeStack
    const q = (this.data.query || "").trim().toLowerCase()

    let list = seedData.LEARN_DEEP
    if (stack) list = list.filter(function (a) { return a.stack === stack })
    if (q) {
      list = list.filter(function (a) {
        return a.title.toLowerCase().indexOf(q) >= 0 || a.stack.indexOf(q) >= 0
      })
    }

    this.setData({
      list: list.map(function (a) {
        const meta = khc.stackMeta(a.stack)
        return {
          slug: a.slug,
          title: a.title,
          stackLabel: meta.label,
          color: meta.color,
          summary: md.preview(a.md, 56)
        }
      })
    })
  },

  onPickStack (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ activeStack: this.data.activeStack === key ? "" : key })
    this.refresh()
  },

  onSearch (e) {
    this.setData({ query: e.detail.value })
    this.refresh()
  },

  onOpen (e) {
    const slug = e.currentTarget.dataset.slug
    let found = null
    for (let i = 0; i < seedData.LEARN_DEEP.length; i++) {
      if (seedData.LEARN_DEEP[i].slug === slug) found = seedData.LEARN_DEEP[i]
    }
    if (!found) return
    this.setData({
      reading: found,
      html: md2html(found.md)
    })
  },

  onClose () {
    this.setData({ reading: null, html: "" })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 图解系列", path: "/knowledge/pages/learn/deep/index" }
  }
})
