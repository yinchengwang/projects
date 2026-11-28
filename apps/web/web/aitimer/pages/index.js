"use strict"
/**
 * index —— AI 定时任务产物概览
 *
 * 一次拉 /api/aitimer/index，把三个任务（AI 简报 / 代码仓巡检 / 每日一特性）
 * 的产物分三张卡列出来。
 *
 * 契约要点：
 * - collections 里每个集合的 docs 是文档数组；
 * - 但 learn 集合的 docs 元素是**系列**（{ id, name, desc, docs }），
 *   所以判断「这一项是文档还是系列」看有没有子 docs 字段。
 */

const ui = require("../../../shared/ui.js")
const plainText = require("../md-block.js").plainText

/** 概览页每个集合最多列出多少条 */
const BRIEF_LIMIT = 8

const PAGE = {
  data: {
    title: "AI 定时任务",
    loading: false,
    error: "",
    exists: true,
    root: "",
    stats: { briefing: 0, inspection: 0, series: 0, articles: 0 },
    collections: []
  },

  onMount: function () {
    this.refresh()
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const head = ui.pageHead({
      eyebrow: "AI 定时任务",
      title: "AI 定时任务产物",
      sub: "代码仓巡检每天 07:30、数据库与 AI 简报每天 08:30、计算机基础深度写作每天 18:00 各产出一批产物"
    })

    if (state.loading) {
      return ui.el("div", { class: "aitimer-page" }, [
        head,
        ui.panel({ children: [ui.empty("加载中…", "正在读取 ai-timer 目录")] })
      ])
    }

    if (state.error) {
      return ui.el("div", { class: "aitimer-page" }, [
        head,
        ui.panel({ children: [ui.empty("产物加载失败", state.error)] })
      ])
    }

    if (!state.exists) {
      return ui.el("div", { class: "aitimer-page" }, [
        head,
        ui.panel({
          children: [
            ui.empty(
              "未找到 ai-timer 目录",
              "该目录被 .gitignore 忽略，克隆仓库后需先运行定时任务"
            )
          ]
        })
      ])
    }

    const stats = state.stats || {}
    const panels = (state.collections || []).map(function (c) {
      return renderCollection(c)
    })

    return ui.el("div", { class: "aitimer-page" }, [
      head,
      ui.statGrid([
        { label: "简报篇数", value: stats.briefing || 0 },
        { label: "巡检篇数", value: stats.inspection || 0 },
        { label: "深度系列数", value: stats.series || 0 },
        { label: "深度文章数", value: stats.articles || 0 }
      ]),
      ui.el("div", {}, panels)
    ])
  },

  /** 拉一次索引并整成页面状态 */
  refresh: function () {
    const self = this
    this.setData({ loading: true, error: "" })
    return fetch("/api/aitimer/index").then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status)
      return res.json()
    }).then(function (data) {
      if (!data || data.ok !== true) {
        throw new Error((data && data.error) || "接口未返回成功标记")
      }
      self.setData(buildModel(data))
      return data
    }).catch(function (err) {
      self.setData({
        loading: false,
        error: err && err.message ? err.message : "未知错误"
      })
      return null
    })
  }
}

/**
 * 渲染单个集合卡片。
 *
 * 简报/巡检是文档列表；learn 是系列网格（系列里再列文档）。
 * @param {object} c 集合
 * @returns {object} DOM 节点
 */
function renderCollection (c) {
  const items = c.docs || []
  const isSeries = items.length > 0 && isSeriesEntry(items[0])

  if (!items.length) {
    return ui.panel({ title: c.name || c.id, children: [ui.empty("暂无产物", "定时任务跑过一次后就会出现")] })
  }

  if (isSeries) {
    const cards = items.map(function (s) {
      const count = (s.docs || []).length
      return ui.el("a", { class: "aitimer-series-card", href: "/aitimer/pages/series?id=" + encodeURIComponent(s.id) }, [
        ui.el("div", { class: "aitimer-series-card-head" }, [
          ui.el("span", { class: "aitimer-series-name", text: plainText(s.name || s.id) }),
          ui.pill(count + " 篇", "accent")
        ]),
        ui.el("span", { class: "aitimer-series-desc", text: plainText(s.desc || "") })
      ])
    })
    return ui.panel({
      title: c.name || c.id,
      children: [
        c.desc ? ui.el("p", { class: "aitimer-note", text: c.desc }) : null,
        ui.el("div", { class: "aitimer-series-grid" }, cards)
      ].filter(Boolean)
    })
  }

  const rows = items.slice(0, BRIEF_LIMIT).map(function (d) {
    return ui.el("a", { class: "aitimer-item", href: "/aitimer/pages/doc?id=" + encodeURIComponent(d.id) }, [
      ui.el("span", { class: "aitimer-item-date", text: d.date || "" }),
      ui.el("span", { class: "aitimer-item-main" }, [
        ui.el("span", { class: "aitimer-item-title", text: d.title || d.id })
      ])
    ])
  })

  const children = []
  if (c.desc) children.push(ui.el("p", { class: "aitimer-note", text: c.desc }))
  children.push(ui.el("div", {}, rows))
  if (items.length > BRIEF_LIMIT) {
    children.push(ui.el("p", { class: "aitimer-note", text: "仅显示最近 " + BRIEF_LIMIT + " 篇，共 " + items.length + " 篇" }))
  }

  return ui.panel({ title: c.name || c.id, children: children })
}

/**
 * 判断一项是不是「系列」（learn 集合的元素）。
 * @param {object} entry 集合元素
 * @returns {boolean}
 */
function isSeriesEntry (entry) {
  return !!(entry && Array.isArray(entry.docs) && entry.name !== undefined)
}

/**
 * 把接口返回整成页面状态。
 * @param {object} data /api/aitimer/index 的返回
 * @returns {object} 状态补丁
 */
function buildModel (data) {
  const collections = data.collections || []
  const stats = { briefing: 0, inspection: 0, series: 0, articles: 0 }

  collections.forEach(function (c) {
    const items = c.docs || []
    if (isSeriesEntry(items[0])) {
      stats.series += items.length
      items.forEach(function (s) { stats.articles += (s.docs || []).length })
      return
    }
    if (c.id === "inspection") stats.inspection = items.length
    else stats.briefing += items.length
  })

  return {
    loading: false,
    error: "",
    exists: data.exists !== false,
    root: data.root || "",
    stats: stats,
    collections: collections
  }
}

if (typeof global !== "undefined" && typeof global.__AITIMER_PAGE__ === "function") global.__AITIMER_PAGE__(PAGE)

module.exports = PAGE
