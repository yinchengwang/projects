"use strict"
/**
 * 面试题库 —— 算法 / 后端两类，答案用 md2html 渲染
 *
 * 打开即记录「已浏览」，可收藏；题库内容来自 seed-data.INTERVIEW_BANK（静态）。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const md2html = require("../../../../../shared/core/md2html.js").md2html
const seedData = require("../../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../../shared/ui.js")
const mdBlock = require("../../../md-block.js").mdBlock
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "面试题库",
    banks: [
      { key: "algorithm", label: "算法" },
      { key: "interview", label: "后端" }
    ],
    bank: "algorithm",
    categories: [],
    category: "",
    query: "",
    list: [],
    current: null,
    html: "",
    showBookmarks: false,
    viewedCount: 0,
    bookmarkCount: 0
  },

  render: function (state) {
    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "面试题库", sub: "算法与后端两类，共 " + (state.list || []).length + " 题" })
    ]

    const bankTabs = (state.banks || []).map(function (b) {
      const button = ui.el("button", {
        class: "kh-tab" + (state.bank === b.key ? " is-active" : ""),
        text: b.label
      })
      button.addEventListener("click", function () {
        this.onSwitchBank({ currentTarget: { dataset: { key: b.key } } })
      }.bind(this))
      return button
    })
    children.push(ui.el("div", { class: "kh-tabs" }, bankTabs))

    const categoryChips = (state.categories || []).map(function (cat) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.category === cat ? " is-active" : ""),
        text: cat
      })
      button.addEventListener("click", function () {
        this.onPickCategory({ currentTarget: { dataset: { cat: cat } } })
      }.bind(this))
      return button
    })

    const search = ui.el("input", { class: "kh-input", type: "search", placeholder: "搜索题目" })
    search.value = state.query || ""
    search.addEventListener("input", function (event) {
      this.onSearch({ detail: { value: event.target.value } })
    }.bind(this))

    const bookmarkToggle = ui.el("button", {
      class: "kh-btn" + (state.showBookmarks ? " primary" : ""),
      text: "只看收藏（" + (state.bookmarkCount || 0) + "）",
      onclick: function () { this.onToggleBookmarks() }.bind(this)
    })

    const rows = (state.list || []).map(function (q) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: q.question }),
          ui.el("div", { class: "kh-list-sub", text: q.category + " · " + q.difficulty + (q.viewed ? " · 已读" : "") })
        ]),
        q.bookmarked ? ui.pill("已收藏", "warn") : ui.pill("未收藏")
      ])
      button.addEventListener("click", function () {
        this.onOpen({ currentTarget: { dataset: { id: q.id } } })
      }.bind(this))
      return button
    })

    children.push(ui.panel({
      title: "题目列表 · 已浏览 " + (state.viewedCount || 0) + " 题",
      children: [
        ui.el("div", { class: "kh-chips" }, categoryChips),
        search,
        ui.el("div", { class: "kh-actions" }, [bookmarkToggle]),
        ui.el("div", { class: "kh-list" }, rows.length ? rows : [ui.empty("没有匹配的题目", "清空搜索或切换分类")])
      ]
    }))

    if (state.current) children.push(this.renderDetail(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 题目详情 */
  renderDetail: function (state) {
    const cur = state.current
    const bookmark = ui.el("button", {
      class: "kh-btn" + (cur.bookmarked ? " primary" : ""),
      text: cur.bookmarked ? "取消收藏" : "收藏",
      onclick: function () { this.onToggleBookmark({ currentTarget: { dataset: { id: cur.id } } }) }.bind(this)
    })
    const close = ui.el("button", { class: "kh-btn", text: "关闭", onclick: function () { this.onClose() }.bind(this) })

    return ui.panel({
      title: cur.question,
      children: [
        ui.el("div", { class: "kh-quiz-meta" }, [
          ui.pill(cur.category, "accent"),
          ui.pill(cur.difficulty, "warn")
        ]),
        mdBlock(state.html),
        ui.el("div", { class: "kh-actions" }, [bookmark, close])
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
    const source = seedData.INTERVIEW_BANK[this.data.bank]
    const categories = []
    source.forEach(function (q) {
      if (categories.indexOf(q.category) < 0) categories.push(q.category)
    })

    const q = (this.data.query || "").trim().toLowerCase()
    const state = stores.interviewBank.get()
    let list = source
    if (this.data.showBookmarks) list = list.filter(function (item) { return state.bookmarks.indexOf(item.id) >= 0 })
    if (this.data.category) list = list.filter(function (item) { return item.category === this.data.category }, this)
    if (q) list = list.filter(function (item) { return item.question.toLowerCase().indexOf(q) >= 0 })

    this.setData({
      categories: categories,
      viewedCount: state.viewed.length,
      bookmarkCount: state.bookmarks.length,
      list: list.map(function (item) {
        return {
          id: item.id,
          question: item.question,
          category: item.category,
          difficulty: item.difficulty,
          viewed: state.viewed.indexOf(item.id) >= 0,
          bookmarked: state.bookmarks.indexOf(item.id) >= 0
        }
      })
    })
  },

  onSwitchBank (e) {
    this.setData({ bank: e.currentTarget.dataset.key, category: "" })
    this.refresh()
  },

  onPickCategory (e) {
    const cat = e.currentTarget.dataset.cat
    this.setData({ category: this.data.category === cat ? "" : cat })
    this.refresh()
  },

  onSearch (e) {
    this.setData({ query: e.detail.value })
    this.refresh()
  },

  onToggleBookmarks () {
    this.setData({ showBookmarks: !this.data.showBookmarks })
    this.refresh()
  },

  onOpen (e) {
    const id = e.currentTarget.dataset.id
    const source = seedData.INTERVIEW_BANK[this.data.bank]
    let found = null
    for (let i = 0; i < source.length; i++) {
      if (source[i].id === id) found = source[i]
    }
    if (!found) return
    stores.interviewBank.markViewed(id)
    this.setData({
      current: {
        id: found.id,
        question: found.question,
        category: found.category,
        difficulty: found.difficulty,
        bookmarked: stores.interviewBank.isBookmarked(id)
      },
      html: md2html(found.answer)
    })
    this.refresh()
  },

  onToggleBookmark (e) {
    const id = e.currentTarget.dataset.id
    stores.interviewBank.toggleBookmark(id)
    this.refresh()
    const cur = this.data.current
    if (cur && cur.id === id) this.setData({ "current.bookmarked": stores.interviewBank.isBookmarked(id) })
  },

  onClose () {
    this.setData({ current: null, html: "" })
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 面试题库", path: "/knowledge/pages/interview/bank/index" }
  }
})
