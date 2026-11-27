"use strict"
/**
 * 知识 —— 3 个模块聚合入口（含能力差距概览）
 */

const { withTheme } = require("../../../../shared/core/with-theme.js")
const stores = require("../../../../shared/knowledge/stores.js")
const khc = require("../../../../shared/knowledge/kh-constants.js")
const seedData = require("../../../../shared/knowledge/seed-data.js")
const ui = require("../../../../shared/ui.js")
const nav = require("../../nav.js")

/**
 * 取知识聚合数据。
 * @returns {object} 视图模型
 */
function build () {
  const nodes = stores.knowledge.getAll()
  const excerpts = stores.excerpts.derive()
  const radar = seedData.RADAR_ITEMS

  const gaps = stores.knowledge.getGaps()
  const gapRows = Object.keys(gaps).map(function (k) {
    const g = gaps[k]
    const meta = khc.KNOWLEDGE_DOMAINS.filter(function (d) { return d.key === k })[0]
    const gap = Math.max(0, g.target - g.level)
    return {
      key: k,
      label: meta ? meta.label : k,
      color: meta ? meta.color : "#64748B",
      level: g.level,
      target: g.target,
      gap: gap,
      percent: g.target ? Math.min(100, Math.round(g.level / g.target * 100)) : 0,
      urgent: gap > 20
    }
  }).sort(function (a, b) { return b.gap - a.gap })

  return {
    stats: {
      nodes: nodes.length,
      links: stores.knowledge.links.length,
      excerpts: excerpts.stats.total,
      radar: radar.length
    },
    gaps: gapRows,
    entries: [
      {
        key: "graph", symbol: "G", title: "知识图谱", color: "#6366F1",
        desc: "节点关系与前置依赖", path: "/knowledge/pages/knowledge/graph/index", count: nodes.length
      },
      {
        key: "excerpt", symbol: "E", title: "摘抄管理", color: "#30A46C",
        desc: "书籍摘录 · 收藏与状态", path: "/knowledge/pages/knowledge/excerpt/index", count: excerpts.stats.total
      },
      {
        key: "radar", symbol: "◎", title: "技术雷达", color: "#F5A524",
        desc: "知识点分布与能力差距", path: "/knowledge/pages/knowledge/radar/index", count: radar.length
      }
    ]
  }
}

module.exports = withTheme({
  data: {
    title: "知识",
    entries: [],
    stats: null,
    gaps: []
  },

  render: function (state) {
    const model = build()
    const stats = model.stats || { nodes: 0, links: 0, excerpts: 0, radar: 0 }

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

    const gapRows = model.gaps.map(function (g) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: g.label + (g.urgent ? " · 待补强" : "") }),
          ui.progressBar(g.percent, { color: g.color })
        ]),
        ui.el("span", { class: "kh-list-sub", text: g.level + " / " + g.target + "（差 " + g.gap + "）" })
      ])
    })

    return ui.el("div", { class: "kh-page" }, [
      ui.pageHead({ eyebrow: "Reading Radar", title: "知识", sub: "图谱、摘抄、技术雷达与能力差距" }),
      ui.statGrid([
        { label: "知识节点", value: String(stats.nodes) },
        { label: "依赖关系", value: String(stats.links) },
        { label: "摘抄", value: String(stats.excerpts) },
        { label: "雷达知识点", value: String(stats.radar) }
      ]),
      ui.panel({ title: "三个模块", children: [ui.el("div", { class: "kh-entry-grid" }, cards)] }),
      ui.panel({ title: "能力差距", children: gapRows.length ? gapRows : [ui.empty("暂无差距数据", "先给知识节点打分")] })
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
    return { title: "Reading Radar · 知识", path: "/knowledge/pages/knowledge/index" }
  }
})
