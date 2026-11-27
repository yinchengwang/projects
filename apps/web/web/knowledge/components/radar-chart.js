"use strict"
/**
 * radar-chart —— 技术雷达画布（Canvas 2d）
 *
 * 与小程序版的差异只有「怎么拿到 canvas」，绘制代码逐字保留。
 * 坐标计算在 shared/knowledge/radar-layout.js（纯函数，已有单测）。
 *
 * 组件形态同 knowledge-graph.js：工厂函数 create(props)。
 */

const layout = require("../../../shared/knowledge/radar-layout.js")

/**
 * 创建技术雷达组件实例。
 * @param {{mode?: string, items?: Array, rows?: Array, isDark?: boolean}} props
 * @returns {{data: object, setData: Function, draw: Function, attachCanvas: Function, destroy: Function}}
 */
function create (props) {
  const p = props || {}
  const inst = {
    data: {
      mode: p.mode || "quadrant",
      items: p.items || [],
      rows: p.rows || [],
      isDark: !!p.isDark,
      ready: false,
      width: 0,
      height: 0
    },
    setData: function (patch) {
      Object.keys(patch).forEach(function (k) { inst.data[k] = patch[k] })
    }
  }

  /**
   * 绑定 canvas 并首次绘制。
   * @param {object} canvasEl HTMLCanvasElement
   */
  inst.attachCanvas = function (canvasEl) {
    if (!canvasEl || typeof canvasEl.getContext !== "function") return
    const rect = canvasEl.getBoundingClientRect()
    const width = Math.round(rect.width || canvasEl.clientWidth || 0)
    const height = Math.round(rect.height || canvasEl.clientHeight || 0)
    if (width === 0 || height === 0) return

    const ctx = canvasEl.getContext("2d")
    if (!ctx) return
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 2
    canvasEl.width = width * dpr
    canvasEl.height = height * dpr
    ctx.scale(dpr, dpr)

    inst.__ctx = ctx
    inst.setData({ ready: true, width: width, height: height })
    inst.draw()
  }

  /** 绘制。坐标由 radar-layout.js 算出，本方法只负责画。 */
  inst.draw = function () {
    const ctx = inst.__ctx
    if (!ctx) return
    const w = inst.data.width
    const h = inst.data.height
    ctx.clearRect(0, 0, w, h)

    if (inst.data.mode === "stack") inst.drawStack(ctx, w, h)
    else inst.drawQuadrant(ctx, w, h)
  }

  /** 象限气泡图 */
  inst.drawQuadrant = function (ctx, w, h) {
    const items = inst.data.items
    if (!items.length) return
    const geo = layout.quadrantBubbles(items, { width: w, height: h })

    // 象限轴线
    ctx.strokeStyle = inst.data.isDark ? "#33404F" : "#E4E7EC"
    ctx.lineWidth = 1
    for (let i = 0; i < 4; i++) {
      const a = (i / 4) * Math.PI * 2
      ctx.beginPath()
      ctx.moveTo(geo.cx, geo.cy)
      ctx.lineTo(geo.cx + Math.cos(a) * geo.radius, geo.cy + Math.sin(a) * geo.radius)
      ctx.stroke()
    }

    // 碰撞推开
    const adjusted = layout.resolveCollisions(geo.points, { minGap: 24, rounds: 8 })

    // 气泡
    adjusted.forEach(function (p, i) {
      const src = geo.points[i]
      ctx.beginPath()
      ctx.arc(p.x, p.y, src.r, 0, Math.PI * 2)
      ctx.fillStyle = src.color
      ctx.globalAlpha = 0.7
      ctx.fill()
      ctx.globalAlpha = 1
    })
  }

  /** 栈雷达多边形 */
  inst.drawStack = function (ctx, w, h) {
    const rows = inst.data.rows
    if (!rows.length) return
    const geo = layout.stackPolygon(rows, { width: w, height: h })

    // 网格环
    ctx.strokeStyle = inst.data.isDark ? "#232B38" : "#E4E7EC"
    ctx.lineWidth = 1
    ;[0.25, 0.5, 0.75, 1].forEach(function (ratio) {
      ctx.beginPath()
      geo.axes.forEach(function (ax, i) {
        const angle = (i / geo.axes.length) * Math.PI * 2 - Math.PI / 2
        const rr = geo.radius * ratio
        const x = geo.cx + Math.cos(angle) * rr
        const y = geo.cy + Math.sin(angle) * rr
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
      })
      ctx.closePath()
      ctx.stroke()
    })

    // 轴线
    ctx.beginPath()
    geo.axes.forEach(function (ax, i) {
      if (i === 0) ctx.moveTo(geo.cx, geo.cy); else ctx.lineTo(ax.x, ax.y)
    })
    ctx.stroke()

    // 数据多边形
    ctx.beginPath()
    geo.points.forEach(function (p, i) {
      if (i === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y)
    })
    ctx.closePath()
    ctx.fillStyle = "rgba(99, 102, 241, 0.24)"
    ctx.fill()
    ctx.strokeStyle = "#6366F1"
    ctx.lineWidth = 2
    ctx.stroke()
  }

  /** 释放资源 */
  inst.destroy = function () {
    inst.__ctx = null
  }

  return inst
}

module.exports = { create: create }
