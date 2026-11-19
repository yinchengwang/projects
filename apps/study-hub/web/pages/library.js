"use strict"
/**
 * library.js —— 单元库页：按轨浏览、搜索全部单元
 *
 * 数据来自 GET /api/units?track=&q=（契约 §8.1）。
 * **过滤在服务端做**：前端只把过滤条件原样带回去，不二次过滤——
 * 两边各过滤一次，口径迟早打架。
 *
 * 分工：
 *   纯函数（buildLibraryView）——算各轨数量、整理行数据，可单测
 *   渲染层（renderLibrary / renderPage）——只做 DOM 装配
 */

;(function () {
  /** 全局对象：P1 起是 window.StudyHub；过渡期兼容 window.AlgoPlan */
  const A =
    (typeof window !== "undefined" && (window.StudyHub || window.AlgoPlan)) ||
    (typeof globalThis !== "undefined" && (globalThis.StudyHub || globalThis.AlgoPlan)) ||
    {}

  const el = A.el || (typeof require === "function" ? require("../ui.js").el : null)

  /** 公共界面构件。浏览器里 ui.js 先加载；Node 下 require 同一份文件——实现只有一份。 */
  const UI =
    (typeof window !== "undefined" &&
      (window.AlgoPlanUI || (window.StudyHub && window.StudyHub.ui) || (window.AlgoPlan && window.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("../ui.js") : null)

  /** 轨道 → 中文名 */
  const TRACK_LABEL = { algorithm: "算法", database: "数据库", ai: "AI" }

  /** 轨切换按钮：全部 + 三条轨 */
  const TRACK_FILTERS = [
    { key: "", label: "全部" },
    { key: "algorithm", label: "算法" },
    { key: "database", label: "数据库" },
    { key: "ai", label: "AI" },
  ]

  /** 单元状态 → 中文名 */
  const STATUS_LABEL = { todo: "待学", draft: "草稿", verified: "已验证", rejected: "已驳回" }

  /** 单元状态 → 药丸色调 */
  const STATUS_TONE = { todo: "", draft: "warn", verified: "ok", rejected: "bad" }

  /**
   * 改地址栏 hash（浏览器里触发 hashchange，页面自己重绘）。
   * @param {string} hash
   */
  function gotoHash(hash) {
    const loc = typeof location !== "undefined" ? location : null
    if (loc) {
      loc.hash = hash
      return
    }
    if (typeof globalThis !== "undefined" && globalThis.location) {
      globalThis.location.hash = hash
    }
  }

  /**
   * 从地址栏读当前的过滤条件。
   * 优先用 web/app.js 的 parseHash；它不在就自己把 query 段拆出来，
   * **不许静默丢掉筛选**（丢了的话单元库页会显示全部，用户以为筛选没生效）。
   * @returns {{track: string, q: string}}
   */
  function readFilter() {
    const hash = (typeof location !== "undefined" && location && location.hash) || ""
    let query = {}
    if (A && typeof A.parseHash === "function") {
      const route = A.parseHash(hash)
      query = (route && route.query) || {}
    } else {
      const qIndex = hash.indexOf("?")
      const queryPart = qIndex === -1 ? "" : hash.slice(qIndex + 1)
      queryPart.split("&").forEach(function (pair) {
        if (!pair) return
        const i = pair.indexOf("=")
        const key = i === -1 ? pair : pair.slice(0, i)
        const value = i === -1 ? "" : pair.slice(i + 1)
        if (!key) return
        query[decodeURIComponent(key)] = decodeURIComponent(String(value).replace(/\+/g, " "))
      })
    }
    return { track: String(query.track || ""), q: String(query.q || "") }
  }

  /**
   * 把接口响应整理成页面要用的视图数据（纯函数）。
   * @param {{total?: number, items?: Array}} payload
   * @param {{track?: string, q?: string}} filter
   * @returns {object}
   */
  function buildLibraryView(payload, filter) {
    const p = payload || {}
    const f = filter || {}
    const items = Array.isArray(p.items) ? p.items : []
    const track = String(f.track || "")
    const q = String(f.q || "")

    const trackCounts = { algorithm: 0, database: 0, ai: 0 }
    const rows = items.map(function (it) {
      const itemTrack = String((it && it.track) || "")
      if (Object.prototype.hasOwnProperty.call(trackCounts, itemTrack)) {
        trackCounts[itemTrack] += 1
      }
      const status = String((it && it.status) || "todo")
      const id = String((it && it.id) || "")
      return {
        id: id,
        title: String((it && it.title) || "未命名单元"),
        track: itemTrack,
        trackLabel: TRACK_LABEL[itemTrack] || "未知轨道",
        kind: String((it && it.kind) || ""),
        kindLabel: String((it && it.kind) || "") === "problem" ? "算法题" : "知识卡",
        status: status,
        statusLabel: STATUS_LABEL[status] || status,
        statusTone: STATUS_TONE[status] || "",
        difficulty: String((it && it.difficulty) || ""),
        topics: Array.isArray(it && it.topics) ? it.topics.map(String) : [],
        // 冒号保持原样：契约 §9.2 的示例就是 #/unit/db:mvcc，
        // encodeURIComponent 会把它编码成 %3A，地址栏里不那么好读，
        // 而 web/llm.js 的 id 正则与契约的 hash 表都以原样冒号为准。
        href: "#/unit/" + encodeURIComponent(id).replace(/%3A/g, ":"),
      }
    })

    const isEmpty = rows.length === 0
    return {
      total: Number(p.total === undefined || p.total === null ? rows.length : p.total),
      count: rows.length,
      rows: rows,
      trackCounts: trackCounts,
      filter: { track: track, q: q },
      hasFilter: track !== "" || q !== "",
      isEmpty: isEmpty,
      emptyHint: hasFilterText(track, q)
        ? "没有匹配的单元。换个关键词，或把轨道切回「全部」。"
        : "单元库还是空的。去供给流水线补内容，补完这里就有货了。",
    }
  }

  /**
   * 有没有过滤条件（给空态文案用）。
   * @param {string} track
   * @param {string} q
   * @returns {boolean}
   */
  function hasFilterText(track, q) {
    return String(track || "") !== "" || String(q || "") !== ""
  }

  /**
   * 渲染单元库页。
   * @param {object} view buildLibraryView 的产物
   * @param {object} payload 接口响应
   * @param {object} filter 当前过滤条件
   * @returns {HTMLElement}
   */
  function renderLibrary(view, payload, filter) {
    const v = view || {}
    const f = (filter && filter.track !== undefined ? filter : v.filter) || { track: "", q: "" }

    const head = UI.pageHead({
      eyebrow: "单元库",
      title: "单元库",
      sub:
        "共 " +
        String(v.total || 0) +
        " 个单元" +
        (v.hasFilter ? "（当前有筛选：轨道 " + (f.track || "全部") + "，关键词 " + (f.q || "无") + "）" : ""),
    })

    // ── 主卡：找单元 ──
    const trackButtons = TRACK_FILTERS.map(function (t) {
      const active = String(f.track || "") === t.key
      return el("button", {
        class: "btn" + (active ? " active" : ""),
        id: "library-track-" + (t.key || "all"),
        text: t.label,
        onclick: function () {
          gotoHash("#/library" + (t.key ? "?track=" + encodeURIComponent(t.key) : ""))
        },
      })
    })

    const search = el("input", { id: "library-q", type: "text", placeholder: "搜标题、编号、知识点" })
    search.value = String(f.q || "")

    const heroKids = [
      el("div", { class: "row" }, trackButtons),
      el("div", { class: "row" }, [
        search,
        el("button", {
          class: "btn",
          id: "library-search",
          text: "搜索",
          onclick: function () {
            const q = String(search.value || "").trim()
            const query = []
            if (f.track) query.push("track=" + encodeURIComponent(f.track))
            if (q) query.push("q=" + encodeURIComponent(q))
            gotoHash("#/library" + (query.length ? "?" + query.join("&") : ""))
          },
        }),
      ]),
      UI.statGrid([
        { label: "当前条数", value: String(v.count || 0), tone: "accent" },
        { label: "算法", value: String((v.trackCounts && v.trackCounts.algorithm) || 0) },
        { label: "数据库", value: String((v.trackCounts && v.trackCounts.database) || 0) },
        { label: "AI", value: String((v.trackCounts && v.trackCounts.ai) || 0) },
      ]),
    ]

    // ── 普通卡：单元列表 ──
    const listKids = []
    if (v.isEmpty) {
      listKids.push(UI.empty("没有可显示的单元", String(v.emptyHint || "")))
    } else {
      ;(v.rows || []).forEach(function (row) {
        listKids.push(
          el("div", { class: "unit-row" }, [
            el("div", { class: "unit-row-main" }, [
              el("span", { class: "unit-row-title", text: row.title }),
              el("span", { class: "muted", text: row.id }),
              row.difficulty ? el("span", { class: "faint", text: "难度 " + row.difficulty }) : null,
            ]),
            el("div", { class: "unit-row-tags" }, [
              UI.pill(row.trackLabel, ""),
              UI.pill(row.kindLabel, ""),
              UI.pill(row.statusLabel, row.statusTone),
            ]),
            el("button", {
              class: "btn",
              text: "打开",
              onclick: function () {
                gotoHash(row.href)
              },
            }),
          ])
        )
      })
    }

    // ── 静默卡：说明 ──
    const quiet = UI.panel({
      title: "说明",
      className: "quiet",
      children: [
        el("p", {
          class: "muted",
          text: "筛选由服务端做（GET /api/units?track=&q=）。点「打开」会跳到单元页，左栏读、右栏做。",
        }),
      ],
    })

    return el("div", { class: "library-page" }, [
      head,
      UI.hero({ title: "找单元", sub: "按轨道筛，或者直接搜标题、编号、知识点。", children: heroKids }),
      UI.panel({ title: "单元列表", children: listKids }),
      quiet,
    ])
  }

  /**
   * 页面入口：拉单元库 → 建视图 → 画。
   * @param {HTMLElement} holder
   * @param {object} [params] 路由参数（单元库页不用，用的是 hash 上的 query）
   */
  async function renderPage(holder, params) {
    const filter = readFilter()
    holder.textContent = ""
    holder.appendChild(el("div", { class: "panel", text: "正在打开单元库…" }))

    const query = []
    if (filter.track) query.push("track=" + encodeURIComponent(filter.track))
    if (filter.q) query.push("q=" + encodeURIComponent(filter.q))
    const url = "/api/units" + (query.length ? "?" + query.join("&") : "")

    let data = null
    try {
      data = await A.getJson(url)
    } catch (err) {
      holder.textContent = ""
      holder.appendChild(UI.pageHead({ eyebrow: "单元库", title: "单元库没打开", sub: "下面是具体原因。" }))
      holder.appendChild(
        el("section", { class: "panel" }, [
          el("h2", { text: "加载失败" }),
          el("p", { class: "error", text: String((err && err.message) || err) }),
        ])
      )
      return
    }

    const view = buildLibraryView(data, filter)
    holder.textContent = ""
    holder.appendChild(renderLibrary(view, data, filter))
  }

  if (A && A.pages) {
    A.pages.library = { title: "单元库", render: renderPage }
  }

  if (typeof module !== "undefined" && module.exports) {
    module.exports = {
      buildLibraryView: buildLibraryView,
      renderLibrary: renderLibrary,
      readFilter: readFilter,
      render: renderPage,
      TRACK_LABEL: TRACK_LABEL,
      TRACK_FILTERS: TRACK_FILTERS,
      STATUS_LABEL: STATUS_LABEL,
    }
  }
})()
