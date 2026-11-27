"use strict"
/**
 * new-trip —— 新增记录（含选图）
 *
 * 表单 + 照片。照片用 platform.chooseMedia 选（浏览器里是 <input type=file>），
 * 存进 IndexedDB（shared/photos.js），页面里用 objectURL 预览。
 *
 * IndexedDB 不可用时（隐私模式等）降级为「只存元数据」，并在页面上明示。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const photos = require("../../../shared/photos.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const MAX_PHOTOS = 9
const MAX_PHOTO_BYTES = 5 * 1024 * 1024

const PAGE = {
  data: {
    title: "新增记录",
    form: {
      title: "",
      date: "",
      weather: "",
      location: "",
      companions: "",
      tagsText: "",
      notes: ""
    },
    files: [],
    saving: false,
    photoReady: true
  },

  onMount: function () {
    const self = this
    this.setData({
      form: Object.assign({}, this.state.form, { date: format.todayISO() })
    })
    photos.isAvailable().then(function (ok) {
      if (!ok) self.setData({ photoReady: false })
    })
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const self = this
    const form = state.form || {}

    const field = function (label, name, value, placeholder) {
      const input = ui.el("input", {
        class: "trip-input",
        "data-field": name,
        placeholder: placeholder || "",
        value: value || ""
      })
      // 输入只改 state，不重绘 —— 否则每敲一个字就丢焦点
      input.addEventListener("input", function (event) {
        self.onInput(name, event.target.value)
      })
      return ui.el("label", { class: "trip-field" }, [
        ui.el("span", { class: "trip-field-label", text: label }),
        input
      ])
    }

    const notesInput = ui.el("textarea", {
      class: "trip-textarea",
      "data-field": "notes",
      placeholder: "随便写点什么",
      text: form.notes || ""
    })
    notesInput.addEventListener("input", function (event) {
      self.onInput("notes", event.target.value)
    })

    const photoNodes = (state.files || []).map(function (f, i) {
      const del = ui.el("button", {
        class: "trip-photo-del",
        "data-index": String(i),
        text: "×"
      })
      del.addEventListener("click", function () { self.onRemovePhoto(i) })
      return ui.el("div", { class: "trip-photo" }, [
        ui.el("img", { class: "trip-photo-img", src: f.path }),
        del
      ])
    })

    const pickButton = ui.el("button", { class: "trip-btn", text: "添加照片" })
    pickButton.addEventListener("click", function () { self.onAddPhotos() })

    const cancelLink = ui.el("a", { class: "trip-btn", href: "/trip/pages/index", text: "取消" })
    const saveButton = ui.el("button", {
      class: "trip-btn primary",
      text: state.saving ? "保存中…" : "保存"
    })
    saveButton.addEventListener("click", function () { self.onSave() })

    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({ title: "新增记录", sub: "标题与日期必填，照片可选" }),
      ui.panel({
        title: "基本信息",
        children: [
          ui.el("div", { class: "trip-form" }, [
            field("标题", "title", form.title, "去了哪儿"),
            field("日期", "date", form.date, "2026-11-20"),
            field("地点", "location", form.location, "杭州 · 西湖"),
            field("天气", "weather", form.weather, "晴"),
            field("同行", "companions", form.companions, "谁一起"),
            field("标签", "tagsText", form.tagsText, "逗号分隔，如：出差,亲子"),
            ui.el("label", { class: "trip-field" }, [
              ui.el("span", { class: "trip-field-label", text: "备注" }),
              notesInput
            ])
          ])
        ]
      }),
      ui.panel({
        title: "照片（最多 " + MAX_PHOTOS + " 张，每张不超过 5MB）",
        children: [
          state.photoReady
            ? ui.el("div", { class: "trip-photos" }, photoNodes)
            : ui.empty("当前浏览器不支持本地图片存储", "记录仍可保存，只是不带照片"),
          state.photoReady ? pickButton : null
        ].filter(Boolean)
      }),
      ui.el("div", { class: "trip-actions" }, [cancelLink, saveButton])
    ])
  },

  /**
   * 表单输入：只更新 state，不重绘（避免输入框丢焦点）。
   * @param {string} field
   * @param {string} value
   */
  onInput: function (field, value) {
    this.state.form = Object.assign({}, this.state.form)
    this.state.form[field] = value
  },

  /** 选图 */
  onAddPhotos: function () {
    const self = this
    const current = this.state.files || []
    const room = MAX_PHOTOS - current.length
    if (room <= 0) {
      platform.showToast({ title: "最多 " + MAX_PHOTOS + " 张", icon: "none" })
      return Promise.resolve()
    }
    return platform.chooseMedia({ count: room, mediaType: ["image"] }).then(function (list) {
      const accepted = []
      let oversize = 0
      ;(list || []).forEach(function (f) {
        if (accepted.length >= room) return
        if (f.size > MAX_PHOTO_BYTES) {
          oversize += 1
          return
        }
        accepted.push({ path: f.path, size: f.size, file: f.file })
      })
      if (oversize > 0) {
        platform.showToast({ title: "有 " + oversize + " 张超过 5MB，已跳过", icon: "none" })
      }
      if (accepted.length > 0) self.setData({ files: current.concat(accepted) })
    }).catch(function (err) {
      platform.showToast({ title: (err && err.message) || "选择照片失败", icon: "none" })
    })
  },

  /** 移除一张照片 */
  onRemovePhoto: function (index) {
    const files = (this.state.files || []).slice()
    const removed = files.splice(index, 1)[0]
    if (removed && removed.path && typeof URL !== "undefined" && typeof URL.revokeObjectURL === "function") {
      URL.revokeObjectURL(removed.path)
    }
    this.setData({ files: files })
  },

  /** 保存 */
  onSave: function () {
    const self = this
    const form = this.state.form || {}
    const title = String(form.title || "").trim()
    const date = String(form.date || "").trim()
    if (!title) {
      platform.showToast({ title: "请填写标题", icon: "none" })
      return Promise.resolve()
    }
    if (!date) {
      platform.showToast({ title: "请填写日期", icon: "none" })
      return Promise.resolve()
    }
    this.setData({ saving: true })

    const tags = String(form.tagsText || "").split(/[,，]/)
      .map(function (s) { return s.trim() })
      .filter(Boolean)
    const photoList = (this.state.files || []).map(function (f, i) {
      return { photoId: "p" + i + "-" + Date.now(), blob: f.file }
    })

    return store.add({
      title: title,
      date: date,
      weather: String(form.weather || "").trim(),
      location: String(form.location || "").trim(),
      companions: String(form.companions || "").trim(),
      tags: tags,
      notes: String(form.notes || "").trim(),
      photos: photoList
    }).then(function () {
      platform.showToast({ title: "已保存", icon: "success" })
      platform.navigateTo("/trip/pages/index")
    }).catch(function (err) {
      self.setData({ saving: false })
      platform.showToast({ title: "保存失败：" + (err && err.message ? err.message : "未知错误"), icon: "none" })
    })
  }
}

if (typeof global !== "undefined" && typeof global.__TRIP_PAGE__ === "function") global.__TRIP_PAGE__(PAGE)

module.exports = PAGE
