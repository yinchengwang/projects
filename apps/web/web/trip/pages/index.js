"use strict"
/**
 * index —— 足迹列表
 *
 * 按月份分组展示全部记录。数据层在 shared/trip/store.js。
 *
 * 列表只需元数据，用 store.listSync() 同步读 —— 渲染不必等 IndexedDB。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const ui = require("../../../shared/ui.js")

const PAGE = {
  data: {
    title: "足迹",
    groups: [],
    total: 0,
    hint: ""
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
    const groups = (state.groups || []).map(function (g) {
      const rows = (g.items || []).map(function (t) {
        return ui.el("a", { class: "trip-row", href: "/trip/pages/detail?id=" + t.id }, [
          ui.el("span", { class: "trip-row-date", text: t.dateShort }),
          ui.el("span", { class: "trip-row-title", text: t.title }),
          ui.el("span", { class: "trip-row-loc", text: t.location })
        ])
      })
      return ui.panel({ title: g.label, children: rows })
    })

    const total = state.total || 0
    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({
        eyebrow: "足迹",
        title: "共 " + total + " 条记录",
        sub: "记录走过的地方"
      }),
      ui.hero({
        title: "添加一条记录",
        sub: "标题与日期必填，照片可选",
        children: [
          ui.el("a", { class: "trip-btn primary", href: "/trip/pages/new-trip", text: "新增记录" })
        ]
      }),
      state.hint ? ui.panel({ children: [ui.empty(state.hint, "照片存在本机 IndexedDB 里")] }) : null,
      total === 0
        ? ui.panel({ children: [ui.empty("还没有足迹", "点上面的按钮记下第一条")] })
        : ui.el("div", {}, groups)
    ].filter(Boolean))
  },

  /** 读元数据并按月份分组 */
  refresh: function () {
    let records = []
    let hint = ""
    try {
      records = store.listSync()
    } catch (e) {
      hint = "记录读取失败：" + (e && e.message ? e.message : "未知错误")
    }
    this.setData(buildModel(records, hint))
  }
}

/**
 * 取数据并按月份分组。
 * @param {Array} records 记录数组（已按日期倒序）
 * @param {string} hint 提示文案
 * @returns {{groups: Array, total: number, hint: string}}
 */
function buildModel (records, hint) {
  const byKey = {}
  const order = []
  ;(records || []).forEach(function (r) {
    const key = format.groupKey(r.date)
    if (!byKey[key]) {
      byKey[key] = { key: key, label: format.groupLabel(r.date), items: [] }
      order.push(key)
    }
    byKey[key].items.push({
      id: r.id,
      title: r.title || "未命名",
      location: r.location || "",
      dateShort: format.ymdShort(r.date)
    })
  })
  return {
    groups: order.map(function (k) { return byKey[k] }),
    total: (records || []).length,
    hint: hint || ""
  }
}

if (typeof global !== "undefined" && typeof global.__TRIP_PAGE__ === "function") global.__TRIP_PAGE__(PAGE)

module.exports = PAGE
