"use strict"
/**
 * 知识图谱 —— Canvas 关系图 + 节点列表（点选 / 调级）
 *
 * 坐标算法在 shared/knowledge/graph-layout.js（纯函数，已有单测），
 * 本页只负责取数、把 canvas 交给组件、以及渲染节点列表与详情。
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const graphLayout = require("../../../../../shared/knowledge/graph-layout.js")
const createGraph = require("../../../components/knowledge-graph.js")
const ui = require("../../../../../shared/ui.js")
const platform = require("../../../../../shared/platform.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

module.exports = withTheme({
  data: {
    title: "知识图谱",
    nodes: [],
    links: [],
    list: [],
    domains: khc.KNOWLEDGE_DOMAINS,
    activeDomain: "",
    selectedId: "",
    selected: null,
    related: []
  },

  render: function (state) {
    const domainChips = (state.domains || []).map(function (d) {
      const button = ui.el("button", {
        class: "kh-chip" + (state.activeDomain === d.key ? " is-active" : ""),
        text: d.label,
        "data-color": d.color
      })
      button.addEventListener("click", function () {
        this.onPickDomain({ currentTarget: { dataset: { key: d.key } } })
      }.bind(this))
      return button
    })

    const canvas = ui.el("canvas", { class: "kh-canvas" })
    this.__canvas = canvas
    this.__graph = createGraph.create({
      nodes: state.nodes || [],
      links: state.links || [],
      selectedId: state.selectedId || "",
      brand: state.brand || "#6366F1"
    })

    const rows = (state.list || []).map(function (n) {
      const button = ui.el("button", { class: "kh-list-row kh-row-btn" }, [
        ui.el("span", { class: "kh-dot", "data-color": n.color }),
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: n.name }),
          ui.el("div", { class: "kh-list-sub", text: n.domain + " · 掌握度 " + n.level })
        ])
      ])
      button.addEventListener("click", function () {
        this.onSelect({ currentTarget: { dataset: { id: n.id } } })
      }.bind(this))
      return button
    })

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "知识图谱", sub: "点节点看前置与后续，可调掌握度" }),
      ui.panel({
        title: "关系图",
        children: [ui.el("div", { class: "kh-canvas-wrap" }, [canvas])]
      }),
      ui.panel({
        title: "节点 · " + (state.list || []).length + " 个",
        children: [ui.el("div", { class: "kh-chips" }, domainChips), ui.el("div", { class: "kh-list" }, rows)]
      })
    ]

    if (state.selected) children.push(this.renderDetail(state))

    return ui.el("div", { class: "kh-page" }, children)
  },

  /** 选中节点详情 */
  renderDetail: function (state) {
    const sel = state.selected
    const relatedRows = (state.related || []).map(function (r) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: r.name }),
          ui.el("div", { class: "kh-list-sub", text: r.dir + " · " + r.domain })
        ])
      ])
    })

    const plus = ui.el("button", { class: "kh-btn", text: "掌握度 +1", onclick: function () { this.onAdjust({ currentTarget: { dataset: { delta: "1" } } }) }.bind(this) })
    const minus = ui.el("button", { class: "kh-btn", text: "掌握度 -1", onclick: function () { this.onAdjust({ currentTarget: { dataset: { delta: "-1" } } }) }.bind(this) })
    const close = ui.el("button", { class: "kh-btn", text: "取消选中", onclick: function () { this.onClose() }.bind(this) })

    return ui.panel({
      title: sel.name + " · " + sel.domain,
      children: [
        ui.progressBar(sel.level * 25, { color: sel.color, label: "掌握度 " + sel.level + " / 4" }),
        ui.el("div", { class: "kh-list" }, relatedRows.length ? relatedRows : [ui.empty("没有关联节点", "这个节点是孤立的")]),
        ui.el("div", { class: "kh-actions" }, [minus, plus, close])
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
    const nodes = stores.knowledge.getAll()
    const links = stores.knowledge.links
    const domain = this.data.activeDomain
    const filtered = domain ? nodes.filter(function (n) { return n.domain === domain }) : nodes

    const rows = filtered.map(function (n) {
      return {
        id: n.id,
        name: n.name,
        domain: n.domain,
        level: n.level,
        color: graphLayout.domainColor(n.domain)
      }
    })

    this.setData({
      nodes: nodes,
      links: links,
      selectedId: this.data.selectedId,
      list: rows,
      selected: this.data.selectedId
        ? rows.filter(function (n) { return n.id === this.data.selectedId }, this)[0] || null
        : null
    })

    this.syncRelated()
  },

  /** 找出与选中节点有前置关系（双向）的节点 */
  syncRelated () {
    const id = this.data.selectedId
    if (!id) {
      this.setData({ related: [] })
      return
    }
    const related = []
    this.data.links.forEach(function (l) {
      if (l.source === id && l.target) related.push({ id: l.target, dir: "后续" })
      if (l.target === id && l.source) related.push({ id: l.source, dir: "前置" })
    })
    const nodes = this.data.nodes
    this.setData({
      related: related.map(function (r) {
        const n = nodes.filter(function (x) { return x.id === r.id })[0]
        return {
          id: r.id,
          name: n ? n.name : r.id,
          domain: n ? n.domain : "",
          dir: r.dir,
          color: n ? graphLayout.domainColor(n.domain) : "#64748B"
        }
      })
    })
  },

  onPickDomain (e) {
    const key = e.currentTarget.dataset.key
    this.setData({ activeDomain: this.data.activeDomain === key ? "" : key })
    this.refresh()
  },

  onSelect (e) {
    this.setData({ selectedId: e.currentTarget.dataset.id })
    this.refresh()
  },

  onAdjust (e) {
    const delta = Number(e.currentTarget.dataset.delta)
    const id = this.data.selectedId
    if (!id) return
    const n = stores.knowledge.find(id)
    if (!n) return
    stores.knowledge.updateNodeLevel(id, n.level + delta)
    this.refresh()
    platform.showToast({ title: "掌握度 " + stores.knowledge.find(id).level, icon: "none" })
  },

  onClose () {
    this.setData({ selectedId: "" })
    this.refresh()
  },

  /**
   * 画布挂进 DOM 后由 browser.js 调用 —— getBoundingClientRect 需要元素已在文档流中。
   */
  attachCanvas () {
    if (this.__graph && this.__canvas) this.__graph.attachCanvas(this.__canvas)
  },

  onUnmount () {
    if (this.__graph) {
      this.__graph.destroy()
      this.__graph = null
    }
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 知识图谱", path: "/knowledge/pages/knowledge/graph/index" }
  }
})
