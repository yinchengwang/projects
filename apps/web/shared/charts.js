"use strict"
/**
 * charts.js —— 手绘 SVG 图表层（零第三方图表库）
 *
 * 纯函数：输入数据 → 输出 SVG 字符串。不碰 DOM，方便单测。
 * 浏览器挂到 window.AlgoPlanCharts，Node 可 require。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanCharts = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /** XML 转义，防注入 */
  function esc (s) {
    return String(s === undefined || s === null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
  }

  /**
   * 折线图。
   * @param {{points: Array<{x: string|number, y: number}>, width?: number, height?: number, color?: string, title?: string}} opts
   * @returns {string} SVG 字符串
   */
  function lineChart (opts) {
    const o = opts || {}
    const points = o.points || []
    const w = o.width || 480
    const h = o.height || 200
    const pad = 32
    const color = o.color || "#0284C7"

    if (points.length === 0) {
      return svgWrap(w, h, '<text x="' + (w / 2) + '" y="' + (h / 2) +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">暂无数据</text>')
    }

    const ys = points.map(function (p) { return Number(p.y) || 0 })
    const min = Math.min.apply(null, ys)
    const max = Math.max.apply(null, ys)
    const span = max - min || 1
    const stepX = points.length > 1 ? (w - pad * 2) / (points.length - 1) : 0

    const coords = points.map(function (p, i) {
      const x = pad + stepX * i
      const y = h - pad - ((Number(p.y) || 0) - min) / span * (h - pad * 2)
      return { x: x, y: y, p: p }
    })

    const path = coords.map(function (c, i) {
      return (i === 0 ? "M" : "L") + c.x.toFixed(1) + " " + c.y.toFixed(1)
    }).join(" ")

    const dots = coords.map(function (c) {
      return '<circle cx="' + c.x.toFixed(1) + '" cy="' + c.y.toFixed(1) +
        '" r="3" fill="' + color + '"><title>' + esc(c.p.x) + "：" + esc(c.p.y) + "</title></circle>"
    }).join("")

    const labels = coords.map(function (c) {
      return '<text x="' + c.x.toFixed(1) + '" y="' + (h - 10) +
        '" text-anchor="middle" fill="#94A3B8" font-size="10">' + esc(c.p.x) + "</text>"
    }).join("")

    return svgWrap(w, h,
      '<path d="' + path + '" fill="none" stroke="' + color + '" stroke-width="2"/>' +
      dots + labels
    )
  }

  /**
   * 柱状图。
   * @param {{bars: Array<{label: string, value: number}>, width?: number, height?: number, color?: string}} opts
   * @returns {string} SVG 字符串
   */
  function barChart (opts) {
    const o = opts || {}
    const bars = o.bars || []
    const w = o.width || 480
    const h = o.height || 200
    const pad = 32
    const color = o.color || "#0284C7"

    if (bars.length === 0) {
      return svgWrap(w, h, '<text x="' + (w / 2) + '" y="' + (h / 2) +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">暂无数据</text>')
    }

    const max = Math.max.apply(null, bars.map(function (b) { return Number(b.value) || 0 })) || 1
    const slot = (w - pad * 2) / bars.length
    const bw = Math.max(4, slot * 0.6)

    const rects = bars.map(function (b, i) {
      const v = Number(b.value) || 0
      const bh = (v / max) * (h - pad * 2)
      const x = pad + slot * i + (slot - bw) / 2
      const y = h - pad - bh
      return '<rect x="' + x.toFixed(1) + '" y="' + y.toFixed(1) +
        '" width="' + bw.toFixed(1) + '" height="' + Math.max(0, bh).toFixed(1) +
        '" fill="' + color + '" rx="3"><title>' + esc(b.label) + "：" + esc(v) + "</title></rect>" +
        '<text x="' + (x + bw / 2).toFixed(1) + '" y="' + (h - 10) +
        '" text-anchor="middle" fill="#94A3B8" font-size="10">' + esc(b.label) + "</text>"
    }).join("")

    return svgWrap(w, h, rects)
  }

  /**
   * 雷达图（技术雷达用）。
   * @param {{axes: Array<{label: string, value: number, max?: number}>, width?: number, height?: number, color?: string}} opts
   * @returns {string} SVG 字符串
   */
  function radarChart (opts) {
    const o = opts || {}
    const axes = o.axes || []
    const size = o.size || 220
    const cx = size / 2
    const cy = size / 2
    const r = size / 2 - 30
    const color = o.color || "#0284C7"

    if (axes.length < 3) {
      return svgWrap(size, size, '<text x="' + cx + '" y="' + cy +
        '" text-anchor="middle" fill="#94A3B8" font-size="13">至少需要 3 个维度</text>')
    }

    function pointAt (i, ratio) {
      const angle = -Math.PI / 2 + (Math.PI * 2 * i) / axes.length
      return { x: cx + Math.cos(angle) * r * ratio, y: cy + Math.sin(angle) * r * ratio }
    }

    const rings = [0.25, 0.5, 0.75, 1].map(function (ratio) {
      const pts = axes.map(function (_, i) { return pointAt(i, ratio) })
      const d = pts.map(function (p, i) {
        return (i === 0 ? "M" : "L") + p.x.toFixed(1) + " " + p.y.toFixed(1)
      }).join(" ") + " Z"
      return '<path d="' + d + '" fill="none" stroke="#CBD5E1" stroke-width="1"/>'
    }).join("")

    const spokes = axes.map(function (_, i) {
      const p = pointAt(i, 1)
      return '<line x1="' + cx + '" y1="' + cy + '" x2="' + p.x.toFixed(1) +
        '" y2="' + p.y.toFixed(1) + '" stroke="#CBD5E1" stroke-width="1"/>'
    }).join("")

    const poly = axes.map(function (a, i) {
      const max = a.max || 100
      const ratio = Math.max(0, Math.min(1, (Number(a.value) || 0) / max))
      const p = pointAt(i, ratio)
      return p.x.toFixed(1) + "," + p.y.toFixed(1)
    }).join(" ")

    const labels = axes.map(function (a, i) {
      const p = pointAt(i, 1.18)
      return '<text x="' + p.x.toFixed(1) + '" y="' + p.y.toFixed(1) +
        '" text-anchor="middle" fill="#64748B" font-size="11">' + esc(a.label) + "</text>"
    }).join("")

    return svgWrap(size, size,
      rings + spokes +
      '<polygon points="' + poly + '" fill="' + color + '" fill-opacity="0.18" stroke="' + color + '" stroke-width="2"/>' +
      labels
    )
  }

  /**
   * 热力图（打卡用）。
   * @param {{cells: Array<{x: number, y: number, value: number}>, width?: number, height?: number, cell?: number}} opts
   * @returns {string} SVG 字符串
   */
  function heatmap (opts) {
    const o = opts || {}
    const cells = o.cells || []
    const cell = o.cell || 12
    const w = o.width || 480
    const h = o.height || 200

    const max = cells.reduce(function (m, c) {
      return Math.max(m, Number(c.value) || 0)
    }, 0) || 1

    const rects = cells.map(function (c) {
      const ratio = (Number(c.value) || 0) / max
      const alpha = ratio === 0 ? 0.05 : 0.15 + ratio * 0.6
      return '<rect x="' + (c.x * cell) + '" y="' + (c.y * cell) +
        '" width="' + (cell - 1) + '" height="' + (cell - 1) + '" rx="2"' +
        ' fill="#0284C7" fill-opacity="' + alpha.toFixed(2) + '"/>'
    }).join("")

    return svgWrap(w, h, rects)
  }

  /** SVG 外壳 */
  function svgWrap (w, h, inner) {
    return '<svg xmlns="http://www.w3.org/2000/svg" width="' + w + '" height="' + h +
      '" viewBox="0 0 ' + w + " " + h + '">' + inner + "</svg>"
  }

  return {
    lineChart: lineChart,
    barChart: barChart,
    radarChart: radarChart,
    heatmap: heatmap,
    esc: esc
  }
})
