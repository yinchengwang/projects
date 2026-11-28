"use strict"
/**
 * series —— 每日一特性的领域系列页
 *
 * 地址栏带 seriesId，在 /api/aitimer/index 的 learn 集合里找到对应系列，
 * 列出系列下的全部文档（README 是系列首页，标「系列首页」）。
 */

const ui = require("../../../shared/ui.js")
const plainText = require("../md-block.js").plainText

const PAGE = {
  data: {
    title: "每日一特性",
    loading: false,
    error: "",
    seriesId: "",
    series: null,
    docs: []
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
    if (state.loading) {
      return ui.el("div", { class: "aitimer-page" }, [
        ui.pageHead({ eyebrow: "每日一特性", title: "加载中…" }),
        ui.panel({ children: [ui.empty("加载中…", "正在读取领域系列")] })
      ])
    }

    if (state.error) {
      return ui.el("div", { class: "aitimer-page" }, [
        ui.pageHead({ eyebrow: "每日一特性", title: "系列加载失败" }),
        ui.panel({ children: [ui.empty("系列加载失败", state.error)] })
      ])
    }

    const s = state.series
    if (!s) {
      return ui.el("div", { class: "aitimer-page" }, [
        ui.pageHead({ eyebrow: "每日一特性", title: "系列不存在" }),
        ui.panel({ children: [ui.empty("找不到该领域系列", "返回概览页重新选择")] }),
        ui.el("div", { class: "aitimer-nav" }, [
          ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/index", text: "← 返回产物概览" })
        ])
      ])
    }

    const rows = (state.docs || []).map(function (d) {
      return ui.el("a", { class: "aitimer-item", href: "/aitimer/pages/doc?id=" + encodeURIComponent(d.id) }, [
        ui.el("span", { class: "aitimer-item-date", text: d.date || "" }),
        ui.el("span", { class: "aitimer-item-main" }, [
          ui.el("span", { class: "aitimer-item-title", text: d.title || d.id }),
          ui.el("span", { class: "aitimer-item-meta" }, buildMeta(d))
        ])
      ])
    })

    return ui.el("div", { class: "aitimer-page" }, [
      ui.pageHead({
        eyebrow: "每日一特性",
        title: plainText(s.name || s.id),
        sub: plainText(s.desc || "")
      }),
      ui.panel({
        title: "系列文章 · " + (state.docs || []).length + " 篇",
        children: rows.length ? rows : [ui.empty("该系列还没有文章", "定时任务跑一次就会出现")]
      }),
      ui.el("div", { class: "aitimer-nav" }, [
        ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/index", text: "← 返回产物概览" })
      ])
    ])
  },

  /** 拉索引，找到 learn 集合里的对应系列 */
  refresh: function () {
    const self = this
    const seriesId = this.readSeriesId()
    if (!seriesId) {
      this.setData({ loading: false, error: "地址栏缺少 series id 参数" })
      return Promise.resolve(null)
    }
    this.setData({ loading: true, error: "", seriesId: seriesId })
    return fetch("/api/aitimer/index").then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status)
      return res.json()
    }).then(function (data) {
      if (!data || data.ok !== true) {
        throw new Error((data && data.error) || "接口未返回成功标记")
      }
      const series = findSeries(data.collections || [], seriesId)
      self.setData({
        loading: false,
        error: "",
        series: series,
        docs: series ? (series.docs || []) : []
      })
      return series
    }).catch(function (err) {
      self.setData({
        loading: false,
        error: err && err.message ? err.message : "未知错误"
      })
      return null
    })
  },

  /** 从地址栏查询串取系列 id */
  readSeriesId: function () {
    if (typeof window !== "undefined" && window.location && typeof window.location.search === "string") {
      const match = /[?&]id=([^&]*)/.exec(window.location.search)
      if (match) return decodeURIComponent(match[1])
    }
    return this.state.seriesId || ""
  }
}

/**
 * 在集合列表里找系列。
 * @param {Array} collections 集合数组
 * @param {string} seriesId 系列 id
 * @returns {object|null}
 */
function findSeries (collections, seriesId) {
  for (let i = 0; i < collections.length; i++) {
    const items = collections[i].docs || []
    for (let j = 0; j < items.length; j++) {
      const s = items[j]
      if (s && s.id === seriesId && Array.isArray(s.docs)) return s
    }
  }
  return null
}

/**
 * 组装一项文档的标签行：难度、状态、标签、系列首页标记。
 * @param {object} d 文档
 * @returns {Array} DOM 节点数组
 */
function buildMeta (d) {
  const out = []
  const meta = d.meta || {}
  if (d.isIndex) out.push(ui.pill("系列首页", "accent"))
  if (meta.difficulty) out.push(ui.pill(difficultyLabel(meta.difficulty)))
  if (meta.status) out.push(ui.pill(meta.status, statusTone(meta.status)))
  const tags = meta.tags || []
  tags.forEach(function (t) {
    out.push(ui.el("span", { class: "aitimer-item-desc", text: "#" + t }))
  })
  return out
}

/**
 * 难度中文化。
 * @param {string} v 原始难度
 * @returns {string}
 */
function difficultyLabel (v) {
  const map = { easy: "简单", medium: "中等", hard: "困难" }
  return map[String(v).toLowerCase()] || String(v)
}

/**
 * 状态配色。
 * @param {string} v 原始状态
 * @returns {string} tone
 */
function statusTone (v) {
  const s = String(v).toLowerCase()
  if (s === "done" || s === "已完成") return "ok"
  if (s === "todo" || s === "doing" || s === "进行中") return "warn"
  return ""
}

if (typeof global !== "undefined" && typeof global.__AITIMER_PAGE__ === "function") global.__AITIMER_PAGE__(PAGE)

module.exports = PAGE
