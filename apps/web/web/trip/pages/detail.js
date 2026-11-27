"use strict"
/**
 * detail —— 足迹详情
 *
 * 读记录 + 照片，照片用 objectURL 预览（用完 revoke），删除时连带删照片。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const PAGE = {
  data: {
    title: "详情",
    id: "",
    trip: null,
    photos: [],
    loading: false
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const self = this
    const t = state.trip
    if (!t) {
      return ui.el("div", { class: "trip-page" }, [
        ui.pageHead({ title: "详情" }),
        ui.panel({ children: [ui.empty("记录不存在或已删除", "返回列表看看别的")] }),
        ui.el("div", { class: "trip-actions" }, [
          ui.el("a", { class: "trip-btn", href: "/trip/pages/index", text: "返回列表" })
        ])
      ])
    }

    const photoNodes = (state.photos || []).map(function (p) {
      const img = ui.el("img", { class: "trip-photo-img", src: p.url })
      img.addEventListener("click", function () { self.onPreview(p.url) })
      return img
    })

    const backLink = ui.el("a", { class: "trip-btn", href: "/trip/pages/index", text: "返回" })
    const deleteButton = ui.el("button", { class: "trip-btn danger", text: "删除" })
    deleteButton.addEventListener("click", function () { self.onDelete() })

    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({ eyebrow: t.dateText, title: t.title, sub: t.location }),
      ui.panel({
        title: "记录",
        children: [
          ui.kv([
            ["日期", t.dateText],
            ["地点", t.location || "未填"],
            ["天气", t.weather || "未填"],
            ["同行", t.companions || "未填"],
            ["标签", t.tagsText || "无"]
          ]),
          t.notes ? ui.block({ title: "备注", children: [ui.el("p", { text: t.notes })] }) : null
        ].filter(Boolean)
      }),
      photoNodes.length
        ? ui.panel({ title: "照片 · " + photoNodes.length + " 张", children: [ui.el("div", { class: "trip-photos" }, photoNodes)] })
        : null,
      ui.el("div", { class: "trip-actions" }, [backLink, deleteButton])
    ].filter(Boolean))
  },

  /** 挂载：从查询串取 id，再读记录与照片 */
  onMount: function () {
    const self = this
    const recordId = this.readId()
    if (!recordId) return
    this.setData({ id: recordId, loading: true })
    return store.get(recordId).then(function (t) {
      if (!t) {
        self.setData({ trip: null, photos: [], loading: false })
        return null
      }
      self.setData({
        loading: false,
        trip: {
          id: t.id,
          title: t.title || "未命名",
          dateText: format.ymdPretty(t.date),
          location: t.location || "",
          weather: t.weather || "",
          companions: t.companions || "",
          notes: t.notes || "",
          tagsText: (t.tags || []).join(" ")
        }
      })
      return store.getPhotoUrls(recordId).then(function (urls) {
        self.setData({ photos: urls })
      })
    })
  },

  /** 从地址栏查询串取记录 id */
  readId: function () {
    if (typeof window !== "undefined" && window.location && typeof window.location.search === "string") {
      const match = /[?&]id=([^&]+)/.exec(window.location.search)
      if (match) return decodeURIComponent(match[1])
    }
    return this.state.id || ""
  },

  /** 预览照片 */
  onPreview: function (url) {
    const urls = (this.state.photos || []).map(function (p) { return p.url }).filter(Boolean)
    return platform.previewImage({ current: url, urls: urls })
  },

  /** 删除（含照片） */
  onDelete: function () {
    const self = this
    return platform.showModal({
      title: "删除记录",
      content: "确定要删除这条足迹吗？照片也会一并删除。",
      confirmText: "删除",
      confirmColor: "#d33"
    }).then(function (r) {
      if (!r.confirm) return null
      return store.remove(self.state.id).then(function () {
        platform.showToast({ title: "已删除", icon: "success" })
        platform.navigateTo("/trip/pages/index")
      })
    })
  }
}

if (typeof global !== "undefined" && typeof global.__TRIP_PAGE__ === "function") global.__TRIP_PAGE__(PAGE)

module.exports = PAGE
