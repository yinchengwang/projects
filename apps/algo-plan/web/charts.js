;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanCharts = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /**
   * charts.js —— 手绘 SVG 图表的纯函数层
   *
   * 零第三方依赖（契约 G22）：雷达图、折线 + 面积图、水平条形图全部手写 SVG。
   * 同一份文件既能被 node:test 直接 require，也能在浏览器里挂到 window.AlgoPlanCharts。
   *
   * 配色：dataviz 暗色分类色 slot 1 蓝 #3987e5（掌握度）、slot 2 橙 #d95926（覆盖度），
   * 在应用面板色 #1d2026 上跑过 validate_palette.js，六项检查全部 PASS。
   * 文字一律用文本色，不穿数据色；状态（弱/强）用文字徽章表达，不靠颜色单独承载。
   */

  /** 图表主题色（暗色面板） */
  const DEFAULT_THEME = {
    surface: "#1d2026",
    grid: "#2c2c2a",
    axis: "#383835",
    track: "#24272d",
    text: "#e8e8e6",
    muted: "#898781",
    series1: "#3987e5",
    series2: "#d95926",
  }

  /** 转义 XML 特殊字符 */
  function escapeXml(value) {
    return String(value === null || value === undefined ? "" : value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&apos;")
  }

  /** 数字转字符串；非法数一律当 0，SVG 里绝不允许出现 NaN */
  function num(value) {
    const n = Number(value)
    if (!Number.isFinite(n)) return "0"
    return String(Math.round(n * 100) / 100)
  }

  /** 把值夹到 0..1 */
  function clamp01(value) {
    const n = Number(value)
    if (!Number.isFinite(n)) return 0
    if (n < 0) return 0
    if (n > 1) return 1
    return n
  }

  /** 0..1 转百分数字符串 */
  function formatPercent(value, digits) {
    const n = Number(value)
    const d = typeof digits === "number" && digits >= 0 ? Math.floor(digits) : 0
    if (!Number.isFinite(n)) return "0%"
    return (n * 100).toFixed(d) + "%"
  }

  /* ---------------- 雷达图 ---------------- */

  /** 雷达图的几何框架：圆心与半径 */
  function radarFrame(size, padding) {
    const s = Number(size) > 0 ? Number(size) : 360
    const p = Number(padding) >= 0 ? Number(padding) : 64
    return { center: s / 2, radius: Math.max(0, s / 2 - p) }
  }

  /**
   * 雷达图的数据点坐标：从正上方开始，顺时针均分。
   * @param {number[]} values 每个维度的值，取值 0..1
   * @param {number} size 画布边长（正方形）
   * @param {{padding?: number}} [options]
   * @returns {Array<{x: number, y: number}>}
   */
  function radarPoints(values, size, options) {
    const opts = options || {}
    const list = Array.isArray(values) ? values : []
    const n = list.length
    if (n === 0) return []
    const frame = radarFrame(size, opts.padding)
    return list.map(function (raw, index) {
      const value = clamp01(raw)
      const angle = -Math.PI / 2 + (2 * Math.PI * index) / n
      return {
        x: Math.round((frame.center + frame.radius * value * Math.cos(angle)) * 100) / 100,
        y: Math.round((frame.center + frame.radius * value * Math.sin(angle)) * 100) / 100,
      }
    })
  }

  /**
   * 雷达图每条轴的外端坐标与文字对齐方式。
   * @param {string[]} labels 维度名
   * @param {number} size 画布边长
   * @param {{padding?: number}} [options]
   * @returns {Array<{label: string, x: number, y: number, anchor: string, baseline: string}>}
   */
  function radarAxes(labels, size, options) {
    const opts = options || {}
    const list = Array.isArray(labels) ? labels : []
    const n = list.length
    if (n === 0) return []
    const frame = radarFrame(size, opts.padding)
    return list.map(function (label, index) {
      const angle = -Math.PI / 2 + (2 * Math.PI * index) / n
      const x = frame.center + frame.radius * Math.cos(angle)
      const y = frame.center + frame.radius * Math.sin(angle)
      let anchor = "middle"
      if (x > frame.center + 0.5) anchor = "start"
      else if (x < frame.center - 0.5) anchor = "end"
      let baseline = "middle"
      if (y > frame.center + 0.5) baseline = "hanging"
      else if (y < frame.center - 0.5) baseline = "auto"
      return {
        label: String(label === null || label === undefined ? "" : label),
        x: Math.round(x * 100) / 100,
        y: Math.round(y * 100) / 100,
        anchor: anchor,
        baseline: baseline,
      }
    })
  }

  /**
   * 雷达图的同心网格环（hairline 实线）。
   * @param {number} count 维度个数
   * @param {number} size 画布边长
   * @param {{padding?: number, rings?: number}} [options]
   * @returns {Array<{level: number, points: Array<{x: number, y: number}>}>}
   */
  function radarGrid(count, size, options) {
    const opts = options || {}
    const n = Math.floor(Number(count) || 0)
    if (n <= 0) return []
    const rings = Math.max(1, Math.floor(Number(opts.rings) || 4))
    const out = []
    for (let i = 1; i <= rings; i += 1) {
      const level = i / rings
      const filler = []
      for (let j = 0; j < n; j += 1) filler.push(level)
      out.push({ level: level, points: radarPoints(filler, size, opts) })
    }
    return out
  }

  /** 多边形路径 */
  function polygonPath(points) {
    const list = Array.isArray(points) ? points : []
    if (list.length === 0) return ""
    return (
      "M " +
      list
        .map(function (point) {
          return num(point.x) + "," + num(point.y)
        })
        .join(" L ") +
      " Z"
    )
  }

  /** SVG polygon 的 points 属性 */
  function pointsAttr(points) {
    const list = Array.isArray(points) ? points : []
    return list
      .map(function (point) {
        return num(point.x) + "," + num(point.y)
      })
      .join(" ")
  }

  /**
   * 雷达图 SVG。最多两条系列（掌握度 / 覆盖度）。
   * @param {{labels: string[], series: Array<{name: string, values: number[]}>,
   *          size?: number, padding?: number, title?: string, theme?: object}} options
   * @returns {string}
   */
  function radarSvg(options) {
    const opts = options || {}
    const theme = Object.assign({}, DEFAULT_THEME, opts.theme || {})
    const labels = Array.isArray(opts.labels) ? opts.labels : []
    const rawSeries = Array.isArray(opts.series) ? opts.series : []
    const size = Number(opts.size) > 0 ? Number(opts.size) : 360
    const padding = Number(opts.padding) >= 0 ? Number(opts.padding) : 64
    const title = String(opts.title || "掌握度雷达图")
    const count = labels.length
    const parts = []

    parts.push(
      '<svg class="chart chart-radar" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' +
        num(size) +
        " " +
        num(size) +
        '" role="img" aria-label="' +
        escapeXml(title) +
        '">'
    )
    parts.push("<title>" + escapeXml(title) + "</title>")

    if (count === 0 || rawSeries.length === 0) {
      parts.push(
        '<text class="chart-empty" x="' +
          num(size / 2) +
          '" y="' +
          num(size / 2) +
          '" text-anchor="middle" dominant-baseline="middle" fill="' +
          theme.muted +
          '" font-size="13">暂无数据</text>'
      )
      parts.push("</svg>")
      return parts.join("")
    }

    // 网格环
    radarGrid(count, size, { padding: padding, rings: 4 }).forEach(function (ring) {
      parts.push(
        '<polygon class="chart-grid" points="' +
          pointsAttr(ring.points) +
          '" fill="none" stroke="' +
          theme.grid +
          '" stroke-width="1"/>'
      )
    })

    // 轴线
    const frame = radarFrame(size, padding)
    radarAxes(labels, size, { padding: padding }).forEach(function (axis) {
      parts.push(
        '<line class="chart-spoke" x1="' +
          num(frame.center) +
          '" y1="' +
          num(frame.center) +
          '" x2="' +
          num(axis.x) +
          '" y2="' +
          num(axis.y) +
          '" stroke="' +
          theme.axis +
          '" stroke-width="1"/>'
      )
    })

    // 数据多边形：面积 10% 透明 + 2px 描边 + 顶点 8px 标记（2px 表面色描边环）
    const series = rawSeries.slice(0, 2).map(function (item) {
      const values = Array.isArray(item && item.values) ? item.values.slice() : []
      while (values.length < count) values.push(0)
      return { name: item && item.name ? String(item.name) : "", values: values }
    })
    series.forEach(function (item, index) {
      const color = index === 0 ? theme.series1 : theme.series2
      const points = radarPoints(item.values, size, { padding: padding })
      parts.push(
        '<polygon class="chart-series" data-series="' +
          escapeXml(item.name) +
          '" points="' +
          pointsAttr(points) +
          '" fill="' +
          color +
          '" fill-opacity="0.1" stroke="' +
          color +
          '" stroke-width="2" stroke-linejoin="round"/>'
      )
      points.forEach(function (point) {
        parts.push(
          '<circle class="chart-point" cx="' +
            num(point.x) +
            '" cy="' +
            num(point.y) +
            '" r="4" fill="' +
            color +
            '" stroke="' +
            theme.surface +
            '" stroke-width="2"/>'
        )
      })
    })

    // 维度标签
    radarAxes(labels, size, { padding: padding }).forEach(function (axis) {
      parts.push(
        '<text class="chart-label" x="' +
          num(axis.x) +
          '" y="' +
          num(axis.y) +
          '" text-anchor="' +
          axis.anchor +
          '" dominant-baseline="' +
          axis.baseline +
          '" fill="' +
          theme.muted +
          '" font-size="12">' +
          escapeXml(axis.label) +
          "</text>"
      )
    })

    parts.push("</svg>")
    return parts.join("")
  }

  /* ---------------- 折线 + 面积 ---------------- */

  /** 把上界取整到整齐的刻度 */
  function niceScale(max, count) {
    const top = Math.max(1, Number(max) || 1)
    const n = Math.max(1, Math.floor(Number(count) || 1))
    const step = Math.max(1, Math.ceil(top / n))
    return { step: step, max: step * n }
  }

  /**
   * 折线图的点坐标。
   * @param {number[]} values 原始数值（可以是题量这种非 0..1 的数）
   * @param {number} width 画布宽
   * @param {number} height 画布高
   * @param {{padding?: {top: number, right: number, bottom: number, left: number}, max?: number}} [options]
   * @returns {Array<{x: number, y: number}>}
   */
  function linePoints(values, width, height, options) {
    const opts = options || {}
    const list = Array.isArray(values) ? values : []
    if (list.length === 0) return []
    const pad = Object.assign({ top: 16, right: 20, bottom: 28, left: 36 }, opts.padding || {})
    const w = Number(width) > 0 ? Number(width) : 600
    const h = Number(height) > 0 ? Number(height) : 220
    let max = Number(opts.max)
    if (!Number.isFinite(max) || max <= 0) {
      max = 0
      list.forEach(function (raw) {
        const value = Number(raw)
        if (Number.isFinite(value) && value > max) max = value
      })
      if (max <= 0) max = 1
    }
    const innerW = Math.max(0, w - pad.left - pad.right)
    const innerH = Math.max(0, h - pad.top - pad.bottom)
    const step = list.length > 1 ? innerW / (list.length - 1) : 0
    return list.map(function (raw, index) {
      const value = Number(raw)
      const safe = Number.isFinite(value) && value > 0 ? value : 0
      const x = list.length > 1 ? pad.left + step * index : pad.left + innerW / 2
      const y = pad.top + innerH - (safe / max) * innerH
      return {
        x: Math.round(x * 100) / 100,
        y: Math.round(y * 100) / 100,
      }
    })
  }

  /** 折线路径（不闭合） */
  function linePath(points) {
    const list = Array.isArray(points) ? points : []
    if (list.length === 0) return ""
    return (
      "M " +
      list
        .map(function (point) {
          return num(point.x) + "," + num(point.y)
        })
        .join(" L ")
    )
  }

  /** 面积路径（闭合到基线） */
  function areaPath(points, baselineY) {
    const list = Array.isArray(points) ? points : []
    if (list.length === 0) return ""
    const first = list[0]
    const last = list[list.length - 1]
    return (
      "M " +
      num(first.x) +
      "," +
      num(baselineY) +
      " L " +
      list
        .map(function (point) {
          return num(point.x) + "," + num(point.y)
        })
        .join(" L ") +
      " L " +
      num(last.x) +
      "," +
      num(baselineY) +
      " Z"
    )
  }

  /**
   * 稀疏挑几个下标做直接标注——首尾一定保留，中间均分。
   * @param {number} count 总点数
   * @param {number} max 最多标几个
   * @returns {number[]}
   */
  function sparseIndexes(count, max) {
    const n = Math.floor(Number(count) || 0)
    const limit = Math.floor(Number(max) || 0)
    if (n <= 0 || limit <= 0) return []
    if (n <= limit) {
      const all = []
      for (let i = 0; i < n; i += 1) all.push(i)
      return all
    }
    if (limit === 1) return [n - 1]
    const out = []
    for (let i = 0; i < limit; i += 1) {
      out.push(Math.round((i * (n - 1)) / (limit - 1)))
    }
    return out
  }

  /**
   * 折线 + 面积图（每日题量曲线）。单序列，不需要图例。
   * @param {{points: Array<{date: string, count: number}>, width?: number, height?: number,
   *          padding?: object, title?: string, color?: string, theme?: object}} options
   * @returns {string}
   */
  function lineSvg(options) {
    const opts = options || {}
    const theme = Object.assign({}, DEFAULT_THEME, opts.theme || {})
    const width = Number(opts.width) > 0 ? Number(opts.width) : 640
    const height = Number(opts.height) > 0 ? Number(opts.height) : 220
    const pad = Object.assign({ top: 16, right: 20, bottom: 28, left: 36 }, opts.padding || {})
    const rows = Array.isArray(opts.points) ? opts.points : []
    const title = String(opts.title || "每日题量曲线")
    const color = opts.color || theme.series1
    const parts = []

    parts.push(
      '<svg class="chart chart-line" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' +
        num(width) +
        " " +
        num(height) +
        '" role="img" aria-label="' +
        escapeXml(title) +
        '">'
    )
    parts.push("<title>" + escapeXml(title) + "</title>")

    if (rows.length === 0) {
      parts.push(
        '<text class="chart-empty" x="' +
          num(width / 2) +
          '" y="' +
          num(height / 2) +
          '" text-anchor="middle" dominant-baseline="middle" fill="' +
          theme.muted +
          '" font-size="13">暂无数据</text>'
      )
      parts.push("</svg>")
      return parts.join("")
    }

    const values = rows.map(function (row) {
      const value = Number(row && row.count)
      return Number.isFinite(value) && value > 0 ? value : 0
    })
    const scale = niceScale(
      values.reduce(function (acc, value) {
        return value > acc ? value : acc
      }, 0),
      3
    )
    const innerW = Math.max(0, width - pad.left - pad.right)
    const innerH = Math.max(0, height - pad.top - pad.bottom)
    const baselineY = pad.top + innerH
    const points = linePoints(values, width, height, { padding: pad, max: scale.max })

    // hairline 实线网格 + y 轴刻度
    for (let tick = 0; tick <= scale.max; tick += scale.step) {
      const y = baselineY - (tick / scale.max) * innerH
      parts.push(
        '<line class="chart-grid" x1="' +
          num(pad.left) +
          '" y1="' +
          num(y) +
          '" x2="' +
          num(width - pad.right) +
          '" y2="' +
          num(y) +
          '" stroke="' +
          theme.grid +
          '" stroke-width="1"/>'
      )
      parts.push(
        '<text class="chart-tick" x="' +
          num(pad.left - 6) +
          '" y="' +
          num(y) +
          '" text-anchor="end" dominant-baseline="middle" fill="' +
          theme.muted +
          '" font-size="11">' +
          num(tick) +
          "</text>"
      )
    }

    // 面积（10% 透明）+ 折线（2px）
    parts.push(
      '<path class="chart-area" d="' +
        areaPath(points, baselineY) +
        '" fill="' +
        color +
        '" fill-opacity="0.1" stroke="none"/>'
    )
    parts.push(
      '<path class="chart-line" d="' +
        linePath(points) +
        '" fill="none" stroke="' +
        color +
        '" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>'
    )

    // 末点标记：8px 直径 + 2px 表面色描边环
    const last = points[points.length - 1]
    parts.push(
      '<circle class="chart-point" cx="' +
        num(last.x) +
        '" cy="' +
        num(last.y) +
        '" r="4" fill="' +
        color +
        '" stroke="' +
        theme.surface +
        '" stroke-width="2"/>'
    )

    // x 轴日期：稀疏标注，不给每个点都写数字
    const labelIndexes = sparseIndexes(rows.length, 4)
    labelIndexes.forEach(function (index) {
      const row = rows[index]
      const point = points[index]
      let anchor = "middle"
      if (index === 0) anchor = "start"
      else if (index === rows.length - 1) anchor = "end"
      parts.push(
        '<text class="chart-tick" x="' +
          num(point.x) +
          '" y="' +
          num(height - 8) +
          '" text-anchor="' +
          anchor +
          '" fill="' +
          theme.muted +
          '" font-size="11">' +
          escapeXml(String(row && row.date ? row.date : "")) +
          "</text>"
      )
    })

    parts.push("</svg>")
    return parts.join("")
  }

  /* ---------------- 水平条形 ---------------- */

  /**
   * 水平条形图的行布局。
   * @param {Array<{label: string, value: number, display?: string}>} rows
   * @param {{width?: number, rowHeight?: number, labelWidth?: number, valueWidth?: number,
   *          barHeight?: number, max?: number}} [options]
   * @returns {Array<{label: string, value: number, display: string, x: number, y: number,
   *          width: number, height: number, baselineX: number, trackWidth: number}>}
   */
  function barRows(rows, options) {
    const opts = options || {}
    const list = Array.isArray(rows) ? rows : []
    const width = Number(opts.width) > 0 ? Number(opts.width) : 520
    const rowHeight = Number(opts.rowHeight) > 0 ? Number(opts.rowHeight) : 30
    const labelWidth = Number(opts.labelWidth) >= 0 ? Number(opts.labelWidth) : 96
    const valueWidth = Number(opts.valueWidth) >= 0 ? Number(opts.valueWidth) : 56
    const barHeight = Number(opts.barHeight) > 0 ? Number(opts.barHeight) : 16
    const trackWidth = Math.max(0, width - labelWidth - valueWidth)
    let max = Number(opts.max)
    if (!Number.isFinite(max) || max <= 0) {
      max = 0
      list.forEach(function (row) {
        const value = Number(row && row.value)
        if (Number.isFinite(value) && value > max) max = value
      })
    }
    if (max <= 0) max = 1
    return list.map(function (row, index) {
      const value = Number(row && row.value)
      const safe = Number.isFinite(value) && value > 0 ? value : 0
      const w = trackWidth * (safe / max)
      return {
        label: String(row && row.label !== null && row.label !== undefined ? row.label : ""),
        value: safe,
        display: String(
          row && row.display !== null && row.display !== undefined ? row.display : num(safe)
        ),
        x: labelWidth,
        y: Math.round((index * rowHeight + (rowHeight - barHeight) / 2) * 100) / 100,
        width: Math.round(w * 100) / 100,
        height: barHeight,
        baselineX: labelWidth,
        trackWidth: trackWidth,
      }
    })
  }

  /** 水平条：基线端方角，数据端 4px 圆角 */
  function barPath(x, y, width, height, radius) {
    const w = Math.max(0, Number(width) || 0)
    const h = Math.max(0, Number(height) || 0)
    const r = Math.max(0, Math.min(Number(radius) || 0, w, h / 2))
    if (r === 0) {
      return "M " + num(x) + "," + num(y) + " h " + num(w) + " v " + num(h) + " h " + num(-w) + " Z"
    }
    return [
      "M " + num(x) + "," + num(y),
      "H " + num(x + w - r),
      "A " + num(r) + "," + num(r) + " 0 0 1 " + num(x + w) + "," + num(y + r),
      "V " + num(y + h - r),
      "A " + num(r) + "," + num(r) + " 0 0 1 " + num(x + w - r) + "," + num(y + h),
      "H " + num(x),
      "Z",
    ].join(" ")
  }

  /**
   * 水平条形图（弱项榜 / 知识点覆盖）。单序列，不按值分色。
   * @param {{rows: Array<{label: string, value: number, display?: string}>, width?: number,
   *          height?: number, rowHeight?: number, labelWidth?: number, valueWidth?: number,
   *          barHeight?: number, max?: number, title?: string, color?: string, theme?: object}} options
   * @returns {string}
   */
  function barSvg(options) {
    const opts = options || {}
    const theme = Object.assign({}, DEFAULT_THEME, opts.theme || {})
    const width = Number(opts.width) > 0 ? Number(opts.width) : 520
    const rowHeight = Number(opts.rowHeight) > 0 ? Number(opts.rowHeight) : 30
    const labelWidth = Number(opts.labelWidth) >= 0 ? Number(opts.labelWidth) : 96
    const valueWidth = Number(opts.valueWidth) >= 0 ? Number(opts.valueWidth) : 56
    const barHeight = Number(opts.barHeight) > 0 ? Number(opts.barHeight) : 16
    const rows = Array.isArray(opts.rows) ? opts.rows : []
    const title = String(opts.title || "条形图")
    const color = opts.color || theme.series1
    const layout = barRows(rows, {
      width: width,
      rowHeight: rowHeight,
      labelWidth: labelWidth,
      valueWidth: valueWidth,
      barHeight: barHeight,
      max: opts.max,
    })
    const height = Number(opts.height) > 0 ? Number(opts.height) : layout.length * rowHeight + 8
    const parts = []

    parts.push(
      '<svg class="chart chart-bar" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' +
        num(width) +
        " " +
        num(height) +
        '" role="img" aria-label="' +
        escapeXml(title) +
        '">'
    )
    parts.push("<title>" + escapeXml(title) + "</title>")

    if (layout.length === 0) {
      parts.push(
        '<text class="chart-empty" x="' +
          num(width / 2) +
          '" y="' +
          num(height / 2) +
          '" text-anchor="middle" dominant-baseline="middle" fill="' +
          theme.muted +
          '" font-size="13">暂无数据</text>'
      )
      parts.push("</svg>")
      return parts.join("")
    }

    layout.forEach(function (row) {
      // 轨道：说明这一行最多能有多长
      parts.push(
        '<rect class="chart-track" x="' +
          num(row.baselineX) +
          '" y="' +
          num(row.y) +
          '" width="' +
          num(row.trackWidth) +
          '" height="' +
          num(row.height) +
          '" rx="4" fill="' +
          theme.track +
          '"/>'
      )
      // 数据条
      if (row.width > 0) {
        parts.push(
          '<path class="chart-bar" d="' +
            barPath(row.x, row.y, row.width, row.height, 4) +
            '" fill="' +
            color +
            '"/>'
        )
      }
      // 维度名
      parts.push(
        '<text class="chart-label" x="' +
          num(row.x - 8) +
          '" y="' +
          num(row.y + row.height / 2) +
          '" text-anchor="end" dominant-baseline="middle" fill="' +
          theme.muted +
          '" font-size="12">' +
          escapeXml(row.label) +
          "</text>"
      )
      // 数值标在条的末端外侧
      parts.push(
        '<text class="chart-value" x="' +
          num(row.x + row.width + 8) +
          '" y="' +
          num(row.y + row.height / 2) +
          '" text-anchor="start" dominant-baseline="middle" fill="' +
          theme.text +
          '" font-size="12">' +
          escapeXml(row.display) +
          "</text>"
      )
    })

    parts.push("</svg>")
    return parts.join("")
  }

  return {
    DEFAULT_THEME: DEFAULT_THEME,
    escapeXml: escapeXml,
    num: num,
    clamp01: clamp01,
    formatPercent: formatPercent,
    radarFrame: radarFrame,
    radarPoints: radarPoints,
    radarAxes: radarAxes,
    radarGrid: radarGrid,
    polygonPath: polygonPath,
    pointsAttr: pointsAttr,
    radarSvg: radarSvg,
    niceScale: niceScale,
    linePoints: linePoints,
    linePath: linePath,
    areaPath: areaPath,
    sparseIndexes: sparseIndexes,
    lineSvg: lineSvg,
    barRows: barRows,
    barPath: barPath,
    barSvg: barSvg,
  }
})
