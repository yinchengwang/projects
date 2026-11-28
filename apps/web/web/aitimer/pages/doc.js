"use strict"
/**
 * doc —— 产物文档阅读页
 *
 * 地址栏带 docId，先取正文 /api/aitimer/doc，再取索引 /api/aitimer/index
 * 算出「上一篇/下一篇」（同集合内按索引给出的顺序前后各取一篇）。
 *
 * 正文渲染走本模块的 md-block.js：wikilink 归一 + 链接加固。
 */

const ui = require("../../../shared/ui.js")
const md2html = require("../../../shared/core/md2html.js").md2html
const mdBlock = require("../md-block.js").mdBlock
const plainText = require("../md-block.js").plainText

const PAGE = {
  data: {
    title: "文档",
    loading: false,
    error: "",
    docId: "",
    doc: null,
    collectionName: "",
    prev: null,
    next: null
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
        ui.pageHead({ eyebrow: state.collectionName || "AI 定时任务", title: "加载中…" }),
        ui.panel({ children: [ui.empty("加载中…", "正在读取文档正文")] })
      ])
    }

    if (state.error) {
      return ui.el("div", { class: "aitimer-page" }, [
        ui.pageHead({ eyebrow: state.collectionName || "AI 定时任务", title: "文档加载失败" }),
        ui.panel({ children: [ui.empty("文档加载失败", state.error)] }),
        ui.el("div", { class: "aitimer-nav" }, [
          ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/index", text: "← 返回产物概览" })
        ])
      ])
    }

    const doc = state.doc
    if (!doc) {
      return ui.el("div", { class: "aitimer-page" }, [
        ui.pageHead({ eyebrow: state.collectionName || "AI 定时任务", title: "文档不存在" }),
        ui.panel({ children: [ui.empty("找不到这篇文档", "它可能已被移动或删除")] }),
        ui.el("div", { class: "aitimer-nav" }, [
          ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/index", text: "← 返回产物概览" })
        ])
      ])
    }

    const meta = doc.meta || {}
    const tags = meta.tags || []
    const links = meta.links || []

    const metaPairs = [
      ["技术栈", meta.stack || "未标注"],
      ["难度", meta.difficulty || "未标注"],
      ["状态", meta.status || "未标注"],
      ["创建时间", meta.created || "未标注"],
      ["更新时间", meta.updated || "未标注"],
      ["标签", tags.length ? tags.join("、") : "无"]
    ]
    if (meta.ref && meta.ref.length) metaPairs.push(["参考", meta.ref.join("、")])

    return ui.el("div", { class: "aitimer-page" }, [
      ui.pageHead({
        eyebrow: state.collectionName || "AI 定时任务",
        title: plainText(doc.title || doc.id),
        sub: doc.date ? doc.date + " · " + formatSize(doc.size) : formatSize(doc.size)
      }),
      ui.panel({ title: "元信息", children: [ui.kv(metaPairs)] }),
      ui.panel({
        title: "正文",
        children: [
          mdBlock(doc.body, md2html),
          ui.el("p", { class: "aitimer-note", text: "注：解析器不支持 Markdown 表格与 wikilink，表格会显示为原始竖线文本。" })
        ]
      }),
      links.length ? renderLinks(links) : null,
      ui.el("div", { class: "aitimer-nav" }, [
        state.prev
          ? ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/doc?id=" + encodeURIComponent(state.prev.id), text: "← 上一篇：" + plainText(state.prev.title) })
          : ui.el("span", { class: "aitimer-nav-empty", text: "已是第一篇" }),
        state.next
          ? ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/doc?id=" + encodeURIComponent(state.next.id), text: "下一篇：" + plainText(state.next.title) + " →" })
          : ui.el("span", { class: "aitimer-nav-empty", text: "已是最后一篇" })
      ].filter(Boolean)),
      ui.el("div", { class: "aitimer-nav" }, [
        ui.el("a", { class: "aitimer-nav-link", href: "/aitimer/pages/index", text: "← 返回产物概览" }),
        ui.el("a", {
          class: "aitimer-nav-link",
          href: "/api/aitimer/raw?id=" + encodeURIComponent(state.docId),
          target: "_blank",
          rel: "noopener",
          text: "查看源文件（原始 Markdown）"
        })
      ])
    ].filter(Boolean))
  },

  /** 拉正文 + 索引，算出前后篇 */
  refresh: function () {
    const self = this
    const docId = this.readDocId()
    if (!docId) {
      this.setData({ loading: false, error: "地址栏缺少文档 id 参数" })
      return Promise.resolve(null)
    }
    this.setData({ loading: true, error: "", docId: docId })

    // 索引用于算前后篇与所属集合名，失败不影响正文阅读
    const indexPromise = fetch("/api/aitimer/index").then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status)
      return res.json()
    }).then(function (data) {
      if (!data || data.ok !== true) throw new Error((data && data.error) || "接口未返回成功标记")
      return data
    })

    return fetch("/api/aitimer/doc?id=" + encodeURIComponent(docId)).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status)
      return res.json()
    }).then(function (data) {
      if (!data || data.ok !== true) {
        throw new Error((data && data.error) || "接口未返回成功标记")
      }
      const doc = data.doc || null
      self.setData({ loading: false, error: "", doc: doc })
      return indexPromise.then(function (index) {
        const nav = locate(index, docId)
        self.setData({
          collectionName: nav.collectionName,
          prev: nav.prev,
          next: nav.next
        })
        return nav
      }).catch(function () {
        self.setData({ collectionName: "", prev: null, next: null })
        return null
      })
    }).catch(function (err) {
      self.setData({
        loading: false,
        error: err && err.message ? err.message : "未知错误"
      })
      return null
    })
  },

  /** 从地址栏查询串取文档 id */
  readDocId: function () {
    if (typeof window !== "undefined" && window.location && typeof window.location.search === "string") {
      const match = /[?&]id=([^&]*)/.exec(window.location.search)
      if (match) return decodeURIComponent(match[1])
    }
    return this.state.docId || ""
  }
}

/**
 * 在索引里定位 docId：返回所属集合名与前后篇。
 *
 * 顺序规则：
 * - 扁平集合（简报 / 巡检）：按 docs 数组顺序前后取一篇；
 * - learn 集合：先在系列层面找（系列自身也是可点的文档），
 *   找不到再进系列内部找，此时同系列内前后取一篇。
 *
 * @param {object} index /api/aitimer/index 的返回
 * @param {string} docId 当前文档 id
 * @returns {{collectionName: string, prev: object|null, next: object|null}}
 */
function locate (index, docId) {
  const collections = (index && index.collections) || []
  for (let i = 0; i < collections.length; i++) {
    const c = collections[i]
    const items = c.docs || []
    // 扁平集合
    const flat = items.findIndex(function (d) { return d && d.id === docId })
    if (flat >= 0) {
      return {
        collectionName: c.name || c.id,
        prev: flat > 0 ? briefDoc(items[flat - 1]) : null,
        next: flat < items.length - 1 ? briefDoc(items[flat + 1]) : null
      }
    }
    // 系列集合：系列自身是文档，系列内还有文档
    for (let j = 0; j < items.length; j++) {
      const s = items[j]
      if (!s || !Array.isArray(s.docs)) continue
      if (s.id === docId) {
        return { collectionName: c.name || c.id, prev: null, next: briefDoc(s.docs[0]) }
      }
      const inner = s.docs.findIndex(function (d) { return d && d.id === docId })
      if (inner >= 0) {
        return {
          collectionName: c.name || c.id,
          prev: inner > 0 ? briefDoc(s.docs[inner - 1]) : null,
          next: inner < s.docs.length - 1 ? briefDoc(s.docs[inner + 1]) : null
        }
      }
    }
  }
  return { collectionName: "", prev: null, next: null }
}

/**
 * 取出前后篇导航要用的最小信息。
 * @param {object} d 文档
 * @returns {object|null}
 */
function briefDoc (d) {
  if (!d) return null
  return { id: d.id, title: d.title || d.id }
}

/**
 * 把字节数写成人类可读。
 * @param {number} size 字节数
 * @returns {string}
 */
function formatSize (size) {
  const n = Number(size)
  if (!isFinite(n) || n <= 0) return "大小未知"
  if (n < 1024) return n + " B"
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB"
  return (n / 1024 / 1024).toFixed(1) + " MB"
}

/**
 * 参考链接面板。
 * @param {Array} links 外链数组
 * @returns {object} DOM 节点
 */
function renderLinks (links) {
  const rows = links.map(function (href) {
    return ui.el("a", {
      class: "aitimer-link",
      href: String(href),
      target: "_blank",
      rel: "noopener noreferrer",
      text: String(href)
    })
  })
  return ui.panel({ title: "参考链接", children: [ui.el("div", { class: "aitimer-links" }, rows)] })
}

if (typeof global !== "undefined" && typeof global.__AITIMER_PAGE__ === "function") global.__AITIMER_PAGE__(PAGE)

module.exports = PAGE
