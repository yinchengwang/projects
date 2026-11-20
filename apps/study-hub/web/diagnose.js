"use strict"
/**
 * diagnose.js —— 诊断页（三视图）
 *
 * 三个视图看同一份数据：
 *   A 三轨总览：三条轨并列，点一条轨下钻它的知识点
 *   B 六轴雷达：六个维度一张雷达图 + 弱项 / 强项清单
 *   C 会话热力图：横轴会话序号、纵轴三轨、格子是那次的结果
 *
 * 规矩：
 *   - 三个视图是**页内标签**，不是路由（G29：不许在前端加路由）
 *   - 可测逻辑全是纯函数并导出（契约 §9.4：DOM 桩没有 querySelector / innerHTML）
 *   - 颜色只是辅助，每张图旁边都有中文表格孪生
 *   - 前端不另立档位口径：四档标签与处方由接口给
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory(require("./charts.js"))
  } else {
    root.AlgoPlanDiagnose = factory(root.StudyHubCharts || root.AlgoPlanCharts)
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function (Charts) {
  "use strict"

  if (!Charts) {
    throw new Error("诊断页依赖 charts.js，请确认 charts.js 在 diagnose.js 之前加载")
  }

  /* ---------------- 常量 ---------------- */

  /** 三个视图（页内标签，不是路由） */
  const VIEWS = ["mastery", "radar", "heatmap"]

  /** 视图的中文标签 */
  const VIEW_LABELS = { mastery: "三轨总览", radar: "六轴雷达", heatmap: "会话热力图" }

  /** 三轨的中文名（接口给了 label 就用接口的，这个只是兜底） */
  const TRACK_LABELS = { algorithm: "算法", database: "数据库", ai: "AI" }

  /** 三轨的固定顺序（热力图的纵轴用它） */
  const TRACK_CODES = ["algorithm", "database", "ai"]

  /** 四档 → 药丸色调。四档的中文标签由接口给，这里只映射颜色 */
  const BUCKET_TONE = {
    "weak-dense": "bad",
    "weak-sparse": "warn",
    "strong-sparse": "accent",
    "strong-dense": "ok",
  }

  /** 下钻明细最多列几行 */
  const MAX_TOPIC_ROWS = 12

  /** 弱项 / 强项清单最多列几行 */
  const MAX_WEAK_ROWS = 12

  const UI =
    (typeof globalThis !== "undefined" &&
      (globalThis.AlgoPlanUI ||
        (globalThis.StudyHub && globalThis.StudyHub.ui) ||
        (globalThis.AlgoPlan && globalThis.AlgoPlan.ui))) ||
    (typeof require === "function" ? require("./ui.js") : null)

  /* ---------------- 纯函数层 ---------------- */

  /** 0..1 → 整数百分比 */
  function percent(value) {
    const n = Number(value)
    if (!Number.isFinite(n) || n <= 0) return "0%"
    return String(Math.min(100, Math.round(n * 100))) + "%"
  }

  /** 轨名的中文兜底 */
  function trackLabelOf(track) {
    const key = String(track || "")
    return TRACK_LABELS[key] || key
  }

  /** 四档 → 条形颜色：弱档橙、强档绿、没诊断出来用蓝 */
  function bucketColor(bucket) {
    const theme = Charts.DEFAULT_THEME
    const key = String(bucket || "")
    if (key === "weak-dense" || key === "weak-sparse") return theme.series2
    if (key === "strong-dense" || key === "strong-sparse") return theme.series3
    return theme.series1
  }

  /**
   * 三轨并列的条形行。
   * @param {object} payload /api/diagnose?view=mastery 的响应
   * @returns {Array<object>}
   */
  function trackBarRows(payload) {
    const data = payload || {}
    const list = Array.isArray(data.tracks) ? data.tracks : []
    return list.map(function (item) {
      const track = String((item && item.track) || "")
      const mastery = Number((item && item.mastery) || 0)
      const coverage = Number((item && item.coverage) || 0)
      return {
        track: track,
        label: String((item && item.label) || trackLabelOf(track)),
        mastery: mastery,
        coverage: coverage,
        display: percent(mastery),
        coverageDisplay: percent(coverage),
        bucket: String((item && item.bucket) || ""),
        bucketLabel: String((item && item.label2) || ""),
        action: String((item && item.action) || ""),
        unitCount: Number((item && item.unitCount) || 0),
      }
    })
  }

  /**
   * 下钻明细：某一条轨的知识点，按掌握度从低到高。
   * @param {object} payload
   * @param {string} track 轨代码
   * @param {number} [limit] 最多几行
   * @returns {Array<object>}
   */
  function topicRowsForTrack(payload, track, limit) {
    const data = payload || {}
    const all = Array.isArray(data.topics) ? data.topics : []
    const max = Number(limit) > 0 ? Number(limit) : MAX_TOPIC_ROWS
    return all
      .filter(function (item) {
        return String((item && item.track) || "") === String(track || "")
      })
      .map(function (item) {
        const bucket = String((item && item.bucket) || "")
        const mastery = Number((item && item.mastery) || 0)
        const topic = String((item && item.topic) || "")
        return {
          key: String(track || "") + ":" + topic,
          track: String((item && item.track) || ""),
          topic: topic,
          mastery: mastery,
          display: percent(mastery),
          coverage: Number((item && item.coverage) || 0),
          coverageDisplay: percent((item && item.coverage) || 0),
          bucket: bucket,
          bucketLabel: String((item && item.label) || ""),
          action: String((item && item.action) || ""),
          color: bucketColor(bucket),
        }
      })
      .sort(function (a, b) {
        return a.mastery - b.mastery
      })
      .slice(0, max)
  }

  /**
   * 概览数字块：四个数。
   * @param {object} payload
   * @returns {Array<{label: string, value: string}>}
   */
  function summaryCards(payload) {
    const data = payload || {}
    const s = data.summary || {}
    return [
      { label: "练过单元", value: String(Number(s.unitCount) || 0) },
      { label: "练过知识点", value: String(Number(s.topicCount) || 0) },
      { label: "会话数", value: String(Number(s.sessionCount) || 0) },
      { label: "总提交数", value: String(Number(s.totalAttempts) || 0) },
    ]
  }

  /**
   * 雷达六轴。顺序由接口给（契约 §8.3.1 固定六轴顺序），前端不重排。
   * @param {object} payload
   * @returns {Array<object>}
   */
  function radarAxes(payload) {
    const data = payload || {}
    const list = Array.isArray(data.axes) ? data.axes : []
    return list.map(function (item) {
      const id = String((item && item.id) || "")
      const value = Number((item && item.value) || 0)
      return {
        id: id,
        label: String((item && item.label) || trackLabelOf(id)),
        value: value,
        display: percent(value),
      }
    })
  }

  /**
   * 一句建议。
   *
   * 接口的清单只给掌握度、没有覆盖度——算不出四档，所以前端不自己判档，
   * 只把掌握度写进句子里，给一句能执行的话。
   * @param {object} row
   * @param {boolean} weak true = 弱项，false = 强项
   * @returns {string}
   */
  function adviceFor(row, weak) {
    const item = row || {}
    const display = String(item.display || percent(item.mastery))
    if (weak) {
      return "掌握度 " + display + "——先补这个概念的笔记，别急着刷题"
    }
    return "掌握度 " + display + "——已掌握，不用主动排"
  }

  /**
   * 弱项清单。
   * @param {object} payload
   * @param {number} [limit]
   * @returns {Array<object>}
   */
  function weakRows(payload, limit) {
    const data = payload || {}
    const list = Array.isArray(data.weak) ? data.weak : []
    const max = Number(limit) > 0 ? Number(limit) : MAX_WEAK_ROWS
    return list.slice(0, max).map(function (item) {
      const track = String((item && item.track) || "")
      const mastery = Number((item && item.mastery) || 0)
      const row = {
        unit_id: String((item && item.unit_id) || ""),
        track: track,
        trackLabel: trackLabelOf(track),
        title: String((item && item.title) || ""),
        topic: String((item && item.topic) || ""),
        mastery: mastery,
        display: percent(mastery),
      }
      row.advice = adviceFor(row, true)
      return row
    })
  }

  /**
   * 强项清单。
   * @param {object} payload
   * @param {number} [limit]
   * @returns {Array<object>}
   */
  function strongRows(payload, limit) {
    const data = payload || {}
    const list = Array.isArray(data.strong) ? data.strong : []
    const max = Number(limit) > 0 ? Number(limit) : MAX_WEAK_ROWS
    return list.slice(0, max).map(function (item) {
      const track = String((item && item.track) || "")
      const mastery = Number((item && item.mastery) || 0)
      const row = {
        unit_id: String((item && item.unit_id) || ""),
        track: track,
        trackLabel: trackLabelOf(track),
        title: String((item && item.title) || ""),
        topic: String((item && item.topic) || ""),
        mastery: mastery,
        display: percent(mastery),
      }
      row.advice = adviceFor(row, false)
      return row
    })
  }

  /**
   * 热力图图例：六种状态。颜色只分组，状态看汉字与中文全称。
   * @returns {Array<{state: string, mark: string, label: string, color: string}>}
   */
  function legendItems() {
    const theme = Charts.DEFAULT_THEME
    return Charts.HEATMAP_STATE_ORDER.map(function (state) {
      const meta = Charts.HEATMAP_STATES[state] || { mark: "?", label: String(state) }
      return {
        state: state,
        mark: meta.mark,
        label: meta.label,
        color: Charts.heatmapStateColor(state, theme),
      }
    })
  }

  /**
   * 热力图按会话分组。
   * @param {object} payload
   * @returns {Array<{id: string, index: number, cells: Array<object>}>}
   */
  function heatmapRows(payload) {
    const data = payload || {}
    const sessions = Array.isArray(data.sessions) ? data.sessions : []
    const cells = Array.isArray(data.cells) ? data.cells : []
    return sessions.map(function (session, position) {
      const sid = String((session && session.id) || "")
      const index = Number(session && session.index) > 0 ? Number(session.index) : position + 1
      const mine = cells.filter(function (cell) {
        return String((cell && cell.session_id) || "") === sid
      })
      return {
        id: sid,
        index: index,
        startedAt: String((session && session.started_at) || ""),
        cells: mine.map(function (cell) {
          const state = String((cell && cell.state) || "pending")
          const meta = Charts.HEATMAP_STATES[state] || { mark: "?", label: state }
          const track = String((cell && cell.track) || "")
          return {
            session_id: sid,
            track: track,
            trackLabel: trackLabelOf(track),
            state: state,
            mark: meta.mark,
            label: meta.label,
            seq: Number((cell && cell.seq) || 0),
          }
        }),
      }
    })
  }

  /* ---------------- 视图模型 ---------------- */

  /**
   * 三轨总览的主卡文案：挑覆盖度大于 0 的轨里掌握度最低的那条。
   * @param {Array<object>} tracks
   * @returns {{title: string, text: string}}
   */
  function masteryHero(tracks) {
    const list = Array.isArray(tracks) ? tracks : []
    if (list.length === 0) {
      return { title: "还没有练习记录", text: "先去做几道题，这里才有得诊断。" }
    }
    let weakest = null
    list.forEach(function (item) {
      if (Number(item.coverage) <= 0) return
      if (!weakest || Number(item.mastery) < Number(weakest.mastery)) weakest = item
    })
    if (!weakest) {
      return { title: "还没有可诊断的轨", text: "先去做几道题，这里才有得诊断。" }
    }
    return {
      title: "先治这一条轨",
      text: weakest.label + " 掌握度 " + weakest.display + "——" + (weakest.action || "先补量"),
    }
  }

  /**
   * 雷达的主卡文案：六轴里最短的那块板。
   * @param {Array<object>} axes
   * @returns {{title: string, text: string}}
   */
  function radarHero(axes) {
    const list = Array.isArray(axes) ? axes : []
    let weakest = null
    list.forEach(function (item) {
      if (Number(item.value) <= 0) return
      if (!weakest || Number(item.value) < Number(weakest.value)) weakest = item
    })
    if (!weakest) {
      return { title: "还没有练习记录", text: "先去做几道题，这里才有得诊断。" }
    }
    return {
      title: "最短的那块板",
      text: weakest.label + " 掌握度 " + weakest.display + "——先补这个概念的笔记，别急着刷题",
    }
  }

  /**
   * 热力图的主卡文案：最近一次会话的结果。
   * @param {Array<object>} rows
   * @returns {{title: string, text: string}}
   */
  function heatmapHero(rows) {
    const list = Array.isArray(rows) ? rows : []
    if (list.length === 0) {
      return { title: "还没有会话记录", text: "先去做几道题，这里才有得诊断。" }
    }
    const last = list[list.length - 1]
    let passed = 0
    let answered = 0
    last.cells.forEach(function (cell) {
      if (cell.state === "pass") passed += 1
      if (cell.state !== "pending") answered += 1
    })
    return {
      title: "最近一次会话",
      text: "第 " + last.index + " 次会话：通过 " + passed + " 个，作答 " + answered + " 个。",
    }
  }

  /**
   * 三轨总览视图模型。
   * @param {object} payload
   * @returns {object}
   */
  function buildMasteryView(payload) {
    const data = payload || {}
    const tracks = trackBarRows(data)
    return {
      view: "mastery",
      title: "三轨总览",
      hero: masteryHero(tracks),
      tracks: tracks,
      drilldown: tracks.map(function (track) {
        return {
          track: track.track,
          label: track.label,
          rows: topicRowsForTrack(data, track.track),
        }
      }),
      cards: summaryCards(data),
      table: {
        columns: ["轨", "掌握度", "覆盖度", "诊断", "单元数"],
        rows: tracks.map(function (track) {
          return [
            track.label,
            track.display,
            track.coverageDisplay,
            track.bucketLabel || "未诊断",
            String(track.unitCount),
          ]
        }),
      },
    }
  }

  /**
   * 六轴雷达视图模型。
   * @param {object} payload
   * @returns {object}
   */
  function buildRadarView(payload) {
    const data = payload || {}
    const axes = radarAxes(data)
    return {
      view: "radar",
      title: "六轴雷达",
      hero: radarHero(axes),
      axes: axes,
      weak: weakRows(data),
      strong: strongRows(data),
      table: {
        columns: ["维度", "掌握度"],
        rows: axes.map(function (axis) {
          return [axis.label, axis.display]
        }),
      },
    }
  }

  /**
   * 会话热力图视图模型。
   * @param {object} payload
   * @returns {object}
   */
  function buildHeatmapView(payload) {
    const data = payload || {}
    const rows = heatmapRows(data)
    const tableRows = []
    rows.forEach(function (row) {
      row.cells.forEach(function (cell) {
        tableRows.push([String(row.index), cell.trackLabel, cell.label, String(cell.seq)])
      })
    })
    return {
      view: "heatmap",
      title: "会话热力图",
      hero: heatmapHero(rows),
      sessions: Array.isArray(data.sessions) ? data.sessions : [],
      sessionRows: rows,
      cells: Array.isArray(data.cells) ? data.cells : [],
      legend: legendItems(),
      table: {
        columns: ["会话", "轨", "结果", "序号"],
        rows: tableRows,
      },
    }
  }

  /**
   * 按视图名分发。名字不认识就回落到三轨总览。
   * @param {object} payload
   * @param {string} view
   * @returns {object}
   */
  function buildDiagnoseView(payload, view) {
    const name = String(view || "")
    if (name === "radar") return buildRadarView(payload)
    if (name === "heatmap") return buildHeatmapView(payload)
    return buildMasteryView(payload)
  }

  /**
   * 最弱的那条轨（覆盖度大于 0 的里面挑掌握度最低的）。
   * 下钻默认落在它身上。
   * @param {object} model 视图模型
   * @returns {string} 轨代码，空串表示没有
   */
  function weakestTrack(model) {
    const tracks = (model && model.tracks) || []
    let best = null
    tracks.forEach(function (item) {
      if (Number(item.coverage) <= 0) return
      if (!best || Number(item.mastery) < Number(best.mastery)) best = item
    })
    return best ? String(best.track) : ""
  }

  /* ---------------- 渲染层 ---------------- */

  /** 前端全局：P1 改名成 StudyHub，旧名也认一份（契约 §13） */
  function appGlobal() {
    if (typeof globalThis !== "undefined") {
      if (globalThis.StudyHub) return globalThis.StudyHub
      if (globalThis.AlgoPlan) return globalThis.AlgoPlan
    }
    if (typeof window !== "undefined") {
      if (window.StudyHub) return window.StudyHub
      if (window.AlgoPlan) return window.AlgoPlan
    }
    return null
  }

  /** 图表库全局：Task 6 同时挂了 StudyHubCharts 与 AlgoPlanCharts */
  function chartsGlobal() {
    if (typeof globalThis !== "undefined") {
      if (globalThis.StudyHubCharts) return globalThis.StudyHubCharts
      if (globalThis.AlgoPlanCharts) return globalThis.AlgoPlanCharts
    }
    if (typeof window !== "undefined") {
      if (window.StudyHubCharts) return window.StudyHubCharts
      if (window.AlgoPlanCharts) return window.AlgoPlanCharts
    }
    return null
  }

  /** 图表容器：SVG 字符串塞进 .chart-host（G8 的既有做法） */
  function chartHost(make, svg) {
    const host = make("div", { class: "chart-host" })
    host.innerHTML = String(svg || "")
    return host
  }

  /** 表格孪生：图看形状，表看数字——不靠颜色也能读 */
  function tableTwin(make, table) {
    const t = table || { columns: [], rows: [] }
    const wrap = make("details", { class: "chart-table" }, [])
    wrap.appendChild(make("summary", { text: "表格孪生（不靠颜色也能读）" }))
    const node = make("table", {}, [])
    const head = make("tr", {}, [])
    t.columns.forEach(function (name) {
      head.appendChild(make("th", { text: String(name) }))
    })
    node.appendChild(head)
    t.rows.forEach(function (row) {
      const tr = make("tr", {}, [])
      row.forEach(function (cell) {
        tr.appendChild(make("td", { text: String(cell) }))
      })
      node.appendChild(tr)
    })
    wrap.appendChild(node)
    return wrap
  }

  /** 页内切换条：三个视图标签（不是路由，G29） */
  function tabRow(make, active, onSelect) {
    const row = make("div", { class: "diag-tabs" }, [])
    VIEWS.forEach(function (name) {
      const selected = name === active
      row.appendChild(
        make(
          "button",
          {
            class: "diag-tab",
            type: "button",
            role: "tab",
            "aria-selected": selected ? "true" : "false",
            text: VIEW_LABELS[name],
            onclick: function () {
              if (typeof onSelect === "function") onSelect(name)
            },
          },
          []
        )
      )
    })
    return row
  }

  /** 三轨并列：一条轨一张卡，卡里两根条（掌握度 / 覆盖度） */
  function trackCards(make, C, model, opts) {
    const options = opts || {}
    const wrap = make("div", { class: "diag-tracks" }, [])
    model.tracks.forEach(function (track) {
      const selected = track.track === options.drillTrack
      const card = make(
        "div",
        {
          class: "diag-track",
          role: "button",
          tabindex: "0",
          "aria-pressed": selected ? "true" : "false",
          onclick: function () {
            if (typeof options.onSelectTrack === "function") options.onSelectTrack(track.track)
          },
        },
        []
      )
      const head = make("div", { class: "diag-track-head" }, [])
      head.appendChild(make("span", { class: "diag-track-name", text: track.label }))
      if (track.bucketLabel) {
        head.appendChild(UI.pill(track.bucketLabel, BUCKET_TONE[track.bucket]))
      }
      card.appendChild(head)
      card.appendChild(
        chartHost(
          make,
          C.barSvg({
            rows: [
              {
                label: "掌握度",
                value: track.mastery,
                display: track.display,
                color: C.DEFAULT_THEME.series1,
              },
              {
                label: "覆盖度",
                value: track.coverage,
                display: track.coverageDisplay,
                color: C.DEFAULT_THEME.series2,
              },
            ],
            max: 1,
            width: 320,
            labelWidth: 56,
            valueWidth: 48,
            title: track.label + " 的掌握度与覆盖度",
          })
        )
      )
      card.appendChild(make("p", { class: "diag-track-action", text: track.action || "" }))
      wrap.appendChild(card)
    })
    return wrap
  }

  /** 下钻明细：选中那条轨的知识点 */
  function drilldown(make, C, model, opts) {
    const options = opts || {}
    const selected = String(options.drillTrack || "")
    let target = null
    model.drilldown.forEach(function (item) {
      if (item.track === selected) target = item
    })
    if (!target) {
      return UI.empty("点上面任意一条轨", "选中之后这里会列出那条轨的知识点与处方。")
    }
    const wrap = make("div", { class: "diag-drill" }, [])
    wrap.appendChild(
      chartHost(
        make,
        C.barSvg({
          rows: target.rows.map(function (row) {
            return { label: row.topic, value: row.mastery, display: row.display, color: row.color }
          }),
          max: 1,
          width: 520,
          title: target.label + " 的知识点掌握度",
        })
      )
    )
    wrap.appendChild(topicList(make, target.rows))
    return wrap
  }

  /** 下钻的知识点行：名字 + 数字 + 档位药丸 + 处方 */
  function topicList(make, rows) {
    if (!rows.length) {
      return UI.empty("这条轨还没有知识点", "先做几道题，这里才有得诊断。")
    }
    const list = make("ul", { class: "diag-advice" }, [])
    rows.forEach(function (row) {
      const item = make("li", { class: "diag-topic" }, [])
      item.appendChild(make("span", { class: "diag-topic-name", text: row.topic }))
      item.appendChild(make("span", { class: "diag-topic-pct", text: row.display }))
      if (row.bucketLabel) {
        item.appendChild(UI.pill(row.bucketLabel, BUCKET_TONE[row.bucket]))
      }
      item.appendChild(make("span", { class: "diag-topic-action", text: row.action || "" }))
      list.appendChild(item)
    })
    return list
  }

  /** 清单行（弱项 / 强项）：名字 + 数字 + 轨名药丸 + 建议 */
  function listRows(make, rows, emptyTitle, emptyHint) {
    if (!rows.length) return UI.empty(emptyTitle, emptyHint)
    const list = make("ul", { class: "diag-advice" }, [])
    rows.forEach(function (row) {
      const item = make("li", { class: "diag-topic" }, [])
      item.appendChild(make("span", { class: "diag-topic-name", text: row.title || row.unit_id }))
      item.appendChild(make("span", { class: "diag-topic-pct", text: row.display }))
      item.appendChild(UI.pill(row.trackLabel, "accent"))
      item.appendChild(make("span", { class: "diag-topic-action", text: row.advice || "" }))
      list.appendChild(item)
    })
    return list
  }

  /** 热力图图例：颜色 + 汉字 + 中文全称，三重承载 */
  function legendRow(make, items) {
    const row = make("div", { class: "diag-heat-legend" }, [])
    items.forEach(function (item) {
      const cell = make("span", { class: "diag-heat-legend-item" }, [])
      cell.appendChild(make("i", { class: "diag-heat-swatch", style: "background:" + item.color }))
      cell.appendChild(make("span", { class: "diag-heat-mark", text: item.mark }))
      cell.appendChild(make("span", { class: "diag-heat-label", text: item.label }))
      row.appendChild(cell)
    })
    return row
  }

  /** 三轨总览卡 */
  function masteryPanel(make, C, model, opts) {
    return UI.panel({
      title: "三轨总览",
      className: "diag-panel",
      children: [
        UI.statGrid(model.cards),
        UI.block({
          title: "三轨并列（点一条轨看它的知识点）",
          children: [trackCards(make, C, model, opts)],
        }),
        UI.block({ title: "下钻明细", children: [drilldown(make, C, model, opts)] }),
        UI.block({ title: "表格孪生", children: [tableTwin(make, model.table)] }),
      ],
    })
  }

  /** 六轴雷达卡 */
  function radarPanel(make, C, model) {
    return UI.panel({
      title: "六轴雷达",
      className: "diag-panel",
      children: [
        UI.block({
          title: "六维掌握度",
          children: [
            chartHost(
              make,
              C.radarSvg({
                labels: model.axes.map(function (axis) {
                  return axis.label
                }),
                series: [
                  {
                    name: "掌握度",
                    values: model.axes.map(function (axis) {
                      return axis.value
                    }),
                  },
                ],
                size: 360,
                title: "六轴掌握度雷达图",
              })
            ),
          ],
        }),
        UI.block({
          title: "弱项清单",
          children: [listRows(make, model.weak, "没有弱项", "六轴都在一半以上，暂时不用补。")],
        }),
        UI.block({ title: "表格孪生", children: [tableTwin(make, model.table)] }),
      ],
    })
  }

  /** 会话热力图卡 */
  function heatmapPanel(make, C, model) {
    return UI.panel({
      title: "会话热力图",
      className: "diag-panel",
      children: [
        UI.block({
          title: "会话 × 轨道",
          children: [
            chartHost(
              make,
              C.heatmapSvg({
                sessions: model.sessions,
                cells: model.cells,
                tracks: TRACK_CODES,
                trackLabels: TRACK_LABELS,
                title: "会话热力图",
              })
            ),
            legendRow(make, model.legend),
          ],
        }),
        UI.block({ title: "表格孪生", children: [tableTwin(make, model.table)] }),
      ],
    })
  }

  /**
   * 把视图模型摆进 DOM。
   * @param {HTMLElement} container
   * @param {object} view 视图模型
   * @param {object} charts charts.js
   * @param {Function} el 造节点的函数
   * @param {object} [options] {activeView, drillTrack, onSelectView, onSelectTrack}
   */
  function mountDiagnose(container, view, charts, el, options) {
    const C = charts || Charts
    const make = el || (UI && UI.el)
    const opts = options || {}
    const model = view || buildMasteryView({})
    const active = VIEWS.indexOf(String(opts.activeView)) === -1 ? "mastery" : String(opts.activeView)
    const drill = String(opts.drillTrack || "") || weakestTrack(model)

    container.textContent = ""

    container.appendChild(
      UI.pageHead({
        eyebrow: "诊断",
        title: "能力诊断",
        sub: "三个视图看同一份数据：三轨总览比强弱，六轴雷达找短板，会话热力图看每一次的结果。",
      })
    )

    // 切换条是导航，不是内容——放静默卡
    container.appendChild(
      UI.panel({
        title: "视图",
        className: "quiet",
        children: [tabRow(make, active, opts.onSelectView)],
      })
    )

    // 主卡：现在该做什么
    container.appendChild(UI.hero({ title: model.hero.title, sub: model.hero.text }))

    if (model.view === "radar") {
      container.appendChild(radarPanel(make, C, model))
    } else if (model.view === "heatmap") {
      container.appendChild(heatmapPanel(make, C, model))
    } else {
      container.appendChild(
        masteryPanel(make, C, model, { drillTrack: drill, onSelectTrack: opts.onSelectTrack })
      )
    }
  }

  /**
   * 页面入口：拉数据 → 算视图 → 摆 DOM。
   *
   * 同一个页面实例里缓存已经拉过的视图：切标签不重复请求。
   * @param {HTMLElement} view 页面容器
   */
  async function renderPage(view) {
    const app = appGlobal()
    if (!app) return
    const make = typeof app.el === "function" ? app.el : UI && UI.el
    const cache = {}
    let active = "mastery"
    let drill = ""

    async function paint() {
      view.textContent = ""
      view.appendChild(make("div", { class: "diag-loading", text: "正在加载诊断数据…" }))
      try {
        if (!cache[active]) {
          cache[active] = await app.getJson("/api/diagnose?view=" + active)
        }
        const model = buildDiagnoseView(cache[active], active)
        view.textContent = ""
        mountDiagnose(view, model, chartsGlobal() || Charts, make, {
          activeView: active,
          drillTrack: drill,
          onSelectView: function (next) {
            if (next === active) return
            active = next
            paint()
          },
          onSelectTrack: function (next) {
            drill = next
            paint()
          },
        })
      } catch (err) {
        view.textContent = ""
        view.appendChild(
          UI.panel({
            title: "诊断数据加载失败",
            children: [make("p", { class: "error", text: String((err && err.message) || err) })],
          })
        )
      }
    }

    await paint()
  }

  /* ---------------- 注册进页面表 ---------------- */

  /** 把页面注册到前端全局的页面表里（P1 改名成 StudyHub，旧名也认一份） */
  function registerPage() {
    const app = appGlobal()
    if (!app || !app.pages) return null
    app.pages.diagnose = { title: "诊断", render: renderPage }
    return app
  }

  registerPage()

  return {
    VIEWS: VIEWS,
    VIEW_LABELS: VIEW_LABELS,
    TRACK_LABELS: TRACK_LABELS,
    BUCKET_TONE: BUCKET_TONE,
    percent: percent,
    trackBarRows: trackBarRows,
    topicRowsForTrack: topicRowsForTrack,
    summaryCards: summaryCards,
    radarAxes: radarAxes,
    weakRows: weakRows,
    strongRows: strongRows,
    legendItems: legendItems,
    adviceFor: adviceFor,
    weakestTrack: weakestTrack,
    buildMasteryView: buildMasteryView,
    buildRadarView: buildRadarView,
    buildHeatmapView: buildHeatmapView,
    buildDiagnoseView: buildDiagnoseView,
    mountDiagnose: mountDiagnose,
    renderPage: renderPage,
  }
})
