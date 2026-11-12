;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory(require("./charts.js"))
  } else {
    root.AlgoPlanDiagnose = factory(root.AlgoPlanCharts)
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function (Charts) {
  "use strict"

  if (!Charts) {
    throw new Error("诊断页依赖 charts.js，请确认 charts.js 在 diagnose.js 之前加载")
  }

  /**
   * diagnose.js —— 诊断页（掌握度雷达 / 弱项榜 / 每日题量曲线 / 知识点覆盖）
   *
   * 零第三方依赖：图表由 web/charts.js 手绘 SVG 生成。
   * 同一份文件既能被 node:test 直接 require，也能在浏览器里挂到 window.AlgoPlanDiagnose。
   *
   * 分工：
   *   纯函数（buildDiagnoseView 等）——算数据、排序、截断，可单测
   *   渲染层（mountDiagnose / showDiagnose）——只做 DOM 装配，无业务分支
   */

  /** 雷达图最多放几个知识点（轴太多就看不清了） */
  const MAX_RADAR_TOPICS = 6

  /** 弱项榜最多列几行 */
  const MAX_WEAK_ROWS = 8

  /** 知识点覆盖最多列几行 */
  const MAX_COVERAGE_ROWS = 10

  /** 处置优先级：练过但总错 > 薄弱且练得少 > 偶然会没练透 > 掌握 */
  const URGENCY_ORDER = ["weak-dense", "weak-sparse", "strong-sparse", "strong-dense"]

  /** 两个弱档 */
  const WEAK_BUCKETS = ["weak-dense", "weak-sparse"]

  /** 百分比格式化，跟 charts.formatPercent 同口径 */
  function percent(value) {
    return Charts.formatPercent(value)
  }

  /** 四个档位各有多少个知识点 */
  function bucketCounts(topics) {
    const counts = {
      "weak-dense": 0,
      "weak-sparse": 0,
      "strong-sparse": 0,
      "strong-dense": 0,
    }
    ;(topics || []).forEach(function (item) {
      if (item && Object.prototype.hasOwnProperty.call(counts, item.bucket)) {
        counts[item.bucket] += 1
      }
    })
    return counts
  }

  /** 雷达图图例：两条序列，颜色取自 charts 的主题色 */
  function legendItems() {
    return [
      { name: "掌握度", color: Charts.DEFAULT_THEME.series1 },
      { name: "覆盖度", color: Charts.DEFAULT_THEME.series2 },
    ]
  }

  /** 概览卡片 */
  function summaryCards(summary) {
    const s = summary || {}
    return [
      { label: "知识点", value: String(Number(s.topicCount) || 0) },
      { label: "平均掌握度", value: percent(s.avgMastery) },
      { label: "平均覆盖度", value: percent(s.avgCoverage) },
      { label: "薄弱知识点", value: String(Number(s.weakCount) || 0) },
      { label: "总提交数", value: String(Number(s.totalAttempts) || 0) },
      { label: "练过题数", value: String(Number(s.totalProblems) || 0) },
      { label: "未涉及的知识点", value: String(Number(s.untouchedTopics) || 0) },
      { label: "期望题量下限", value: String(Number(s.minExpectedQuestions) || 0) },
    ]
  }

  /**
   * 雷达图要展示的知识点：练得最多的几个（提交次数降序，同次数按名字排）。
   * 选「练得最多的」是因为雷达看的是主战场的长相，弱项由弱项榜负责。
   */
  function radarTopics(topics, max) {
    const list = Array.isArray(topics) ? topics.slice() : []
    const limit = Math.max(0, Math.floor(Number(max) || MAX_RADAR_TOPICS))
    list.sort(function (a, b) {
      const da = Number(a.attempts) || 0
      const db = Number(b.attempts) || 0
      if (da !== db) return db - da
      return String(a.topic) < String(b.topic) ? -1 : 1
    })
    return list.slice(0, limit).map(function (item) {
      return {
        topic: String(item.topic),
        mastery: Charts.clamp01(item.mastery),
        coverage: Charts.clamp01(item.coverage),
        attempts: Number(item.attempts) || 0,
      }
    })
  }

  /** 弱项榜：只收两个弱档，按处置优先级排 */
  function weakRows(topics, limit) {
    const list = Array.isArray(topics) ? topics.slice() : []
    const cap = Math.max(0, Math.floor(Number(limit) || MAX_WEAK_ROWS))
    const rows = list.filter(function (item) {
      return item && WEAK_BUCKETS.indexOf(item.bucket) !== -1
    })
    rows.sort(function (a, b) {
      const ia = URGENCY_ORDER.indexOf(a.bucket)
      const ib = URGENCY_ORDER.indexOf(b.bucket)
      if (ia !== ib) return ia - ib
      const ma = Number(a.mastery) || 0
      const mb = Number(b.mastery) || 0
      if (ma !== mb) return ma - mb
      return String(a.topic) < String(b.topic) ? -1 : 1
    })
    return rows.slice(0, cap).map(function (item) {
      return {
        topic: String(item.topic),
        mastery: Charts.clamp01(item.mastery),
        coverage: Charts.clamp01(item.coverage),
        label: String(item.label || ""),
        action: String(item.action || ""),
        display: percent(item.mastery),
      }
    })
  }

  /** 知识点覆盖：按覆盖度从低到高排 */
  function coverageRows(topics, limit) {
    const list = Array.isArray(topics) ? topics.slice() : []
    const cap = Math.max(0, Math.floor(Number(limit) || MAX_COVERAGE_ROWS))
    list.sort(function (a, b) {
      const ca = Number(a.coverage) || 0
      const cb = Number(b.coverage) || 0
      if (ca !== cb) return ca - cb
      return String(a.topic) < String(b.topic) ? -1 : 1
    })
    return list.slice(0, cap).map(function (item) {
      const problems = Number(item.problems) || 0
      const expected = Number(item.expected) || 0
      return {
        topic: String(item.topic),
        coverage: Charts.clamp01(item.coverage),
        expected: expected,
        problems: problems,
        display: percent(item.coverage),
        detail: "练了 " + problems + " / 期望 " + expected + " 题",
      }
    })
  }

  /**
   * 算整个诊断页要显示的数据。纯函数，渲染层只负责把它摆出来。
   * @param {{topics?: object[], summary?: object, series?: object[]}} data
   * @returns {object}
   */
  function buildDiagnoseView(data) {
    const payload = data || {}
    const topics = Array.isArray(payload.topics) ? payload.topics : []
    const series = Array.isArray(payload.series) ? payload.series : []
    const radar = radarTopics(topics, MAX_RADAR_TOPICS)
    const weak = weakRows(topics, MAX_WEAK_ROWS)
    const coverage = coverageRows(topics, MAX_COVERAGE_ROWS)

    return {
      summary: summaryCards(payload.summary),
      buckets: bucketCounts(topics),
      radar: {
        title: "掌握度雷达（练得最多的 " + radar.length + " 个知识点）",
        labels: radar.map(function (row) {
          return row.topic
        }),
        series: [
          { name: "掌握度", values: radar.map(function (row) { return row.mastery }) },
          { name: "覆盖度", values: radar.map(function (row) { return row.coverage }) },
        ],
        rows: radar,
        table: {
          columns: ["知识点", "掌握度", "覆盖度", "提交数"],
          rows: radar.map(function (row) {
            return [row.topic, percent(row.mastery), percent(row.coverage), String(row.attempts)]
          }),
        },
      },
      weak: {
        title: "弱项榜",
        rows: weak,
        table: {
          columns: ["知识点", "掌握度", "诊断", "建议"],
          rows: weak.map(function (row) {
            return [row.topic, row.display, row.label, row.action]
          }),
        },
      },
      curve: {
        title: "每日题量曲线（近 " + series.length + " 天）",
        points: series.map(function (row) {
          return { date: String(row.date), count: Number(row.count) || 0 }
        }),
        table: {
          columns: ["日期", "题量"],
          rows: series.map(function (row) {
            return [String(row.date), String(Number(row.count) || 0)]
          }),
        },
      },
      coverage: {
        title: "知识点覆盖",
        rows: coverage,
        table: {
          columns: ["知识点", "覆盖度", "练过 / 期望"],
          rows: coverage.map(function (row) {
            return [row.topic, row.display, row.detail]
          }),
        },
      },
    }
  }

  /* ---------------- 渲染层 ---------------- */

  /** 极简 DOM 构造器——与 M4 的 A.el 同签名，用不上 A.el 时的兜底 */
  function dEl(tag, attrs, children) {
    const node = document.createElement(tag)
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        if (key === "class") node.className = attrs[key]
        else if (key === "text") node.textContent = attrs[key]
        else node.setAttribute(key, attrs[key])
      })
    }
    ;(children || []).forEach(function (child) {
      node.appendChild(child)
    })
    return node
  }

  /** 放一张图（SVG 字符串由 web/charts.js 生成，标签已转义） */
  function chartHost(el, svg) {
    const host = el("div", { class: "chart-host" })
    host.innerHTML = svg
    return host
  }

  /** 表格孪生：每张图都能展开成表格看 */
  function tableTwin(el, table) {
    const data = table || {}
    const head = el(
      "tr",
      {},
      (data.columns || []).map(function (name) {
        return el("th", { text: name })
      })
    )
    const body = (data.rows || []).map(function (row) {
      return el(
        "tr",
        {},
        row.map(function (cell) {
          return el("td", { text: String(cell) })
        })
      )
    })
    return el("details", { class: "chart-table" }, [
      el("summary", { text: "表格视图" }),
      el("table", {}, [
        el("thead", {}, [head]),
        el("tbody", {}, body),
      ]),
    ])
  }

  /**
   * 把视图数据摆进 DOM。
   * @param {Element} container 容器（一般是 #view）
   * @param {object} view buildDiagnoseView 的返回值
   * @param {object} [charts] web/charts.js
   * @param {Function} [el] DOM 构造器 el(tag, attrs, children)，默认用 M4 的 A.el
   */
  function mountDiagnose(container, view, charts, el) {
    const C = charts || Charts
    const make =
      el ||
      (typeof window !== "undefined" && window.AlgoPlan && window.AlgoPlan.el
        ? window.AlgoPlan.el
        : dEl)
    container.textContent = ""

    // 概览
    const cards = view.summary.map(function (card) {
      return make("div", { class: "diag-card" }, [
        make("div", { text: card.label }),
        make("span", { text: card.value }),
      ])
    })
    container.appendChild(
      make("section", { class: "diag-panel" }, [
        make("h2", { text: "诊断概览" }),
        make("div", { class: "diag-cards" }, cards),
      ])
    )

    // 掌握度雷达
    const legend = legendItems().map(function (item) {
      const dot = make("i", {})
      dot.style.background = item.color
      return make("span", {}, [dot, document.createTextNode(item.name)])
    })
    container.appendChild(
      make("section", { class: "diag-panel" }, [
        make("h2", { text: view.radar.title }),
        make("div", { class: "diag-legend" }, legend),
        chartHost(
          make,
          C.radarSvg({
            labels: view.radar.labels,
            series: view.radar.series,
            title: view.radar.title,
          })
        ),
        tableTwin(make, view.radar.table),
      ])
    )

    // 弱项榜
    const weakBars = view.weak.rows.map(function (row) {
      return { label: row.topic, value: row.mastery, display: row.display }
    })
    const weakAdvice = view.weak.rows.map(function (row) {
      return make("li", {}, [
        make("span", { class: "badge", text: row.label }),
        make("span", { class: "diag-topic", text: row.topic }),
        make("span", { class: "muted", text: row.action }),
      ])
    })
    container.appendChild(
      make("section", { class: "diag-panel" }, [
        make("h2", { text: view.weak.title }),
        weakBars.length === 0
          ? make("p", { class: "diag-empty", text: "目前没有薄弱知识点——保持住。" })
          : chartHost(make, C.barSvg({ rows: weakBars, title: view.weak.title })),
        weakAdvice.length === 0
          ? null
          : make("ul", { class: "diag-advice" }, weakAdvice),
        tableTwin(make, view.weak.table),
      ])
    )

    // 每日题量曲线
    container.appendChild(
      make("section", { class: "diag-panel" }, [
        make("h2", { text: view.curve.title }),
        chartHost(make, C.lineSvg({ points: view.curve.points, title: view.curve.title })),
        tableTwin(make, view.curve.table),
      ])
    )

    // 知识点覆盖
    const coverageBars = view.coverage.rows.map(function (row) {
      return { label: row.topic, value: row.coverage, display: row.display }
    })
    container.appendChild(
      make("section", { class: "diag-panel" }, [
        make("h2", { text: view.coverage.title }),
        coverageBars.length === 0
          ? make("p", { class: "diag-empty", text: "还没有练过任何题目。" })
          : chartHost(make, C.barSvg({ rows: coverageBars, title: view.coverage.title })),
        tableTwin(make, view.coverage.table),
      ])
    )
  }

  /**
   * M4 的页面入口：拉数据并渲染诊断页。
   * @param {Element} view M4 传进来的 #view
   * @returns {Promise<void>}
   */
  async function renderPage(view) {
    const api = typeof window !== "undefined" ? window.AlgoPlan : null
    const C = typeof window !== "undefined" && window.AlgoPlanCharts ? window.AlgoPlanCharts : Charts
    if (!api) return
    const make = typeof api.el === "function" ? api.el : dEl
    view.textContent = ""
    view.appendChild(make("div", { class: "panel muted", text: "正在加载诊断数据…" }))
    try {
      const body = await api.getJson("/api/insights")
      const model = buildDiagnoseView(body)
      view.textContent = ""
      mountDiagnose(view, model, C, make)
    } catch (err) {
      view.textContent = ""
      view.appendChild(
        make("section", { class: "panel" }, [
          make("h2", { text: "诊断数据加载失败" }),
          make("p", { class: "error", text: String((err && err.message) || err) }),
        ])
      )
    }
  }

  /* 注册进 M4 的页面表——浏览器里 window.AlgoPlan 一定在，Node 测试里用替身 */
  if (typeof window !== "undefined" && window.AlgoPlan && window.AlgoPlan.pages) {
    window.AlgoPlan.pages.diagnose = {
      title: "诊断",
      render: renderPage,
    }
  }

  return {
    MAX_RADAR_TOPICS: MAX_RADAR_TOPICS,
    MAX_WEAK_ROWS: MAX_WEAK_ROWS,
    MAX_COVERAGE_ROWS: MAX_COVERAGE_ROWS,
    URGENCY_ORDER: URGENCY_ORDER,
    WEAK_BUCKETS: WEAK_BUCKETS,
    percent: percent,
    bucketCounts: bucketCounts,
    legendItems: legendItems,
    summaryCards: summaryCards,
    radarTopics: radarTopics,
    weakRows: weakRows,
    coverageRows: coverageRows,
    buildDiagnoseView: buildDiagnoseView,
    mountDiagnose: mountDiagnose,
    renderPage: renderPage,
  }
})
