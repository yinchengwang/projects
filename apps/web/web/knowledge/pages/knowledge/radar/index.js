"use strict"
/**
 * 技术雷达 —— 知识点气泡图 / 技术栈雷达 / 能力差距
 */

const { withTheme } = require("../../../../../shared/core/with-theme.js")
const stores = require("../../../../../shared/knowledge/stores.js")
const khc = require("../../../../../shared/knowledge/kh-constants.js")
const seedData = require("../../../../../shared/knowledge/seed-data.js")
const radarLayout = require("../../../../../shared/knowledge/radar-layout.js")
const createRadar = require("../../../components/radar-chart.js")
const ui = require("../../../../../shared/ui.js")
const syncTheme = require("../../../theme-bridge.js").syncTheme

const MODES = [
  { key: "quadrant", label: "知识点" },
  { key: "stack", label: "技术栈" },
  { key: "gap", label: "能力差距" }
]

module.exports = withTheme({
  data: {
    title: "技术雷达",
    mode: "quadrant",
    modes: MODES,
    items: [],
    rows: [],
    gaps: [],
    quadrants: radarLayout.QUADRANT_ORDER,
    rings: radarLayout.RING_ORDER,
    activeQuadrant: "",
    activeRing: "",
    list: [],
    stats: null
  },

  render: function (state) {
    const modeTabs = (state.modes || MODES).map(function (m) {
      const button = ui.el("button", {
        class: "kh-tab" + (state.mode === m.key ? " is-active" : ""),
        text: m.label
      })
      button.addEventListener("click", function () {
        this.onMode({ currentTarget: { dataset: { mode: m.key } } })
      }.bind(this))
      return button
    })

    const children = [
      ui.pageHead({ eyebrow: "Reading Radar", title: "技术雷达", sub: "知识点分布、技术栈掌握度与能力差距" }),
      ui.el("div", { class: "kh-tabs" }, modeTabs)
    ]

    if (state.mode === "gap") {
      const gapRows = (state.gaps || []).map(function (g) {
        return ui.el("div", { class: "kh-list-row" }, [
          ui.el("div", { class: "kh-list-main" }, [
            ui.el("div", { class: "kh-list-title", text: g.label }),
            ui.progressBar(g.percent, { color: g.color })
          ]),
          ui.el("span", { class: "kh-list-sub", text: g.level + " / " + g.target + " · " + g.advice })
        ])
      })
      children.push(ui.panel({ title: "能力差距", children: gapRows.length ? gapRows : [ui.empty("暂无差距数据", "先去知识图谱打分")] }))
      return ui.el("div", { class: "kh-page" }, children)
    }

    const canvas = ui.el("canvas", { class: "kh-canvas" })
    this.__canvas = canvas
    this.__radar = createRadar.create({
      mode: state.mode === "stack" ? "stack" : "quadrant",
      items: state.items || [],
      rows: state.rows || [],
      isDark: !!state.isDark
    })

    children.push(ui.panel({
      title: state.mode === "stack" ? "技术栈掌握度" : "知识点分布",
      children: [ui.el("div", { class: "kh-canvas-wrap" }, [canvas])]
    }))

    if (state.mode === "stack") {
      const rowNodes = (state.rows || []).map(function (r) {
        return ui.el("div", { class: "kh-list-row" }, [
          ui.el("div", { class: "kh-list-main" }, [
            ui.el("div", { class: "kh-list-title", text: r.label }),
            ui.progressBar(r.percent, { color: r.color })
          ]),
          ui.el("span", { class: "kh-list-sub", text: r.answered + " / " + r.total })
        ])
      })
      children.push(ui.panel({ title: "7 个技术栈", children: rowNodes }))
      return ui.el("div", { class: "kh-page" }, children)
    }

    const quadrantChips = (state.quadrants || []).map(function (key) {
      const meta = radarLayout.QUADRANT_META[key]
      const button = ui.el("button", {
        class: "kh-chip" + (state.activeQuadrant === key ? " is-active" : ""),
        text: meta.label,
        "data-color": meta.color
      })
      button.addEventListener("click", function () {
        this.onPickQuadrant({ currentTarget: { dataset: { key: key } } })
      }.bind(this))
      return button
    })

    const listRows = (state.list || []).map(function (it) {
      return ui.el("div", { class: "kh-list-row" }, [
        ui.el("span", { class: "kh-dot", "data-color": it.color }),
        ui.el("div", { class: "kh-list-main" }, [
          ui.el("div", { class: "kh-list-title", text: it.title }),
          ui.el("div", { class: "kh-list-sub", text: it.quadrantLabel + " · " + it.ringLabel + " · " + it.desc })
        ])
      ])
    })

    children.push(ui.panel({
      title: "知识点 · " + (state.list || []).length + " 个",
      children: [ui.el("div", { class: "kh-chips" }, quadrantChips), ui.el("div", { class: "kh-list" }, listRows)]
    }))

    return ui.el("div", { class: "kh-page" }, children)
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
    const items = seedData.RADAR_ITEMS.map(function (it, i) {
      return {
        id: it.id,
        title: it.title,
        quadrant: it.quadrant,
        ring: it.ring,
        tags: it.tags || [],
        recommend: (i % 4) + 1,
        desc: it.desc
      }
    })

    const rows = khc.STACK_META.map(function (s) {
      const prog = stores.quizzes.getProgress(s.key)
      const items2 = items.filter(function (it) {
        return (it.tags || []).indexOf(s.key) >= 0
      })
      // 掌握度 = 已答题比例；若该栈无标签知识点，则用题库进度兜底
      const percent = prog.total ? Math.round(prog.answered / prog.total * 100) : (items2.length ? 20 : 0)
      return {
        key: s.key,
        label: s.label,
        color: s.color,
        percent: percent,
        total: prog.total,
        answered: prog.answered
      }
    })

    const gapRaw = stores.knowledge.getGaps()
    const gaps = Object.keys(gapRaw).map(function (k) {
      const g = gapRaw[k]
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
        urgent: gap > 20,
        advice: gap > 20 ? "建议优先安排专项练习" : (gap > 0 ? "持续积累中" : "已达标")
      }
    }).sort(function (a, b) { return b.gap - a.gap })

    const q = this.data.activeQuadrant
    const r = this.data.activeRing
    let list = items
    if (q) list = list.filter(function (it) { return it.quadrant === q })
    if (r) list = list.filter(function (it) { return it.ring === r })

    this.setData({
      items: items,
      rows: rows,
      gaps: gaps,
      stats: {
        items: items.length,
        stacks: rows.length,
        avgPercent: Math.round(rows.reduce(function (a, b) { return a + b.percent }, 0) / (rows.length || 1))
      },
      list: list.map(function (it) {
        const qm = radarLayout.QUADRANT_META[it.quadrant]
        const rm = radarLayout.RING_META[it.ring]
        return {
          id: it.id,
          title: it.title,
          desc: it.desc,
          quadrant: it.quadrant,
          quadrantLabel: qm.label,
          color: qm.color,
          ringLabel: rm.label
        }
      })
    })
  },

  onMode (e) {
    this.setData({ mode: e.currentTarget.dataset.mode })
  },

  onPickQuadrant (e) {
    const q = e.currentTarget.dataset.key
    this.setData({ activeQuadrant: this.data.activeQuadrant === q ? "" : q })
    this.refresh()
  },

  onPickRing (e) {
    const r = e.currentTarget.dataset.key
    this.setData({ activeRing: this.data.activeRing === r ? "" : r })
    this.refresh()
  },

  /** 画布挂进 DOM 后由 browser.js 调用 */
  attachCanvas () {
    if (this.__radar && this.__canvas) this.__radar.attachCanvas(this.__canvas)
  },

  onUnmount () {
    if (this.__radar) {
      this.__radar.destroy()
      this.__radar = null
    }
  },

  onShareAppMessage () {
    return { title: "Reading Radar · 技术雷达", path: "/knowledge/pages/knowledge/radar/index" }
  }
})
