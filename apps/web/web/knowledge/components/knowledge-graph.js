"use strict"
/**
 * knowledge-graph —— 知识图谱画布（Canvas 2d）
 *
 * 与小程序版的差异只有「怎么拿到 canvas」：
 *   小程序：wx.createSelectorQuery().in(this).select('#graph').fields({node:true,size:true})
 *   Web   ：getBoundingClientRect
 *
 * 绘制代码逐字保留 —— Canvas 2D API（clearRect / beginPath / arc / fill /
 * stroke / moveTo / lineTo）两边完全一致。
 * 坐标计算在 shared/knowledge/graph-layout.js（纯函数，已有单测）。
 *
 * 【为什么用工厂函数而不是 Component()】
 * 小程序的 Component() 依赖框架注入 this.setData / this.triggerEvent /
 * properties 观察者。本仓零构建、无框架，硬造一套不值当。
 * 改成工厂函数后组件是普通对象，页面直接持有实例并调 draw()。
 */

const layout = require("../../../shared/knowledge/graph-layout.js")

/**
 * 创建知识图谱组件实例。
 * @param {{nodes?: Array, links?: Array, selectedId?: string, brand?: string}} props
 * @returns {{data: object, setData: Function, draw: Function, attachCanvas: Function, destroy: Function}}
 */
function create (props) {
  const p = props || {}
  const inst = {
    data: {
      nodes: p.nodes || [],
      links: p.links || [],
      selectedId: p.selectedId || "",
      brand: p.brand || "#6366F1",
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
    // 用 px 画布 + dpr 放大，保证高清
    canvasEl.width = width * dpr
    canvasEl.height = height * dpr
    ctx.scale(dpr, dpr)

    inst.__ctx = ctx
    inst.setData({ ready: true, width: width, height: height })
    inst.draw()
  }

  /**
   * 绘制图谱。坐标由 graph-layout.js 算出，本方法只负责画。
   */
  inst.draw = function () {
    const ctx = inst.__ctx
    if (!ctx) return
    const nodes = inst.data.nodes
    const links = inst.data.links
    if (!nodes.length) return

    const w = inst.data.width
    const h = inst.data.height
    const geo = layout.layout(nodes, links, { width: w, height: h })

    ctx.clearRect(0, 0, w, h)

    // 连线（前置依赖）
    const pos = {}
    geo.points.forEach(function (pt) { pos[pt.id] = pt })
    ctx.strokeStyle = inst.data.brand
    ctx.globalAlpha = 0.35
    ctx.lineWidth = 1
    geo.edges.forEach(function (e) {
      const a = pos[e.from]
      const b = pos[e.to]
      ctx.beginPath()
      ctx.moveTo(a.x, a.y)
      ctx.lineTo(b.x, b.y)
      ctx.stroke()
    })

    // 节点
    ctx.globalAlpha = 1
    geo.points.forEach(function (pt) {
      const selected = pt.id === inst.data.selectedId
      ctx.beginPath()
      ctx.arc(pt.x, pt.y, pt.r, 0, Math.PI * 2)
      ctx.fillStyle = pt.color
      ctx.globalAlpha = selected ? 1 : 0.75
      ctx.fill()
      ctx.globalAlpha = 1
      if (selected) {
        ctx.lineWidth = 2
        ctx.strokeStyle = "#FFFFFF"
        ctx.stroke()
      }
    })
  }

  /** 释放资源 */
  inst.destroy = function () {
    inst.__ctx = null
  }

  return inst
}

module.exports = { create: create }
