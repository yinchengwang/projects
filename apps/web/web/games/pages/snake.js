"use strict"
/**
 * snake —— 贪吃蛇（Canvas 2d）
 *
 * 引擎在 shared/games/snake.js（零 wx 依赖，纯逻辑）。
 * 本文件负责：状态管理、Canvas 绘制、方向控制、定时循环。
 *
 * 与小程序版的差异只有「怎么拿到 canvas」：
 *   小程序：wx.createSelectorQuery().select('#snake-canvas').fields({node:true,size:true})
 *   Web   ：getBoundingClientRect + canvas.getContext('2d')
 * 绘制代码逐字保留 —— Canvas 2D API 两边完全一致。
 */

const snake = require("../../../shared/games/snake.js")
const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const vibrate = require("../../../shared/games/vibrate.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const PAGE = {
  data: {
    score: 0,
    best: 0,
    status: "ready", // ready | playing | paused | over
    gridWidth: snake.SNAKE_CONFIG.GRID_WIDTH,
    gridHeight: snake.SNAKE_CONFIG.GRID_HEIGHT,
    achievementToast: null
  },

  onMount: function () {
    // 游戏状态与页面状态分开放：this.state 是渲染用，this.__game 是引擎状态
    this.__game = snake.createInitialState()
    this.__timer = null
    this.__canvas = null
    this.__ctx = null
    this.__size = null
    this.__unsubscribe = null

    const self = this
    try {
      this.__unsubscribe = achievements.subscribe(function (a) {
        self.setData({ achievementToast: { title: a.name, desc: a.desc } })
      })
    } catch (e) {
      this.__unsubscribe = null
    }

    this.setData({ best: gameStorage.getSnakeBestScore() })
    this.bindKeyboard()
  },

  onUnmount: function () {
    this.stopLoop()
    if (typeof this.__unsubscribe === "function") this.__unsubscribe()
    if (this.__keyHandler && typeof window !== "undefined" && typeof window.removeEventListener === "function") {
      window.removeEventListener("keydown", this.__keyHandler)
    }
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const self = this
    const canvas = ui.el("canvas")
    this.__canvas = canvas

    const dirButtons = ["up", "down", "left", "right"].map(function (d) {
      const label = { up: "↑", down: "↓", left: "←", right: "→" }[d]
      const button = ui.el("button", { class: "game-dir", text: label })
      button.addEventListener("click", function () { self.onDirection(d) })
      return button
    })

    const startButton = ui.el("button", {
      class: "game-btn primary",
      text: state.status === "playing" ? "暂停" : (state.status === "paused" ? "继续" : "开始")
    })
    startButton.addEventListener("click", function () {
      if (state.status === "playing") self.onPause()
      else if (state.status === "paused") self.onResume()
      else self.onStart()
    })

    const restartButton = ui.el("button", { class: "game-btn", text: "重开" })
    restartButton.addEventListener("click", function () { self.onStart() })

    return ui.el("div", { class: "games-page" }, [
      ui.pageHead({
        eyebrow: "贪吃蛇",
        title: "得分 " + (state.score || 0),
        sub: "最高 " + (state.best || 0) + " 分 · " + (state.status === "playing" ? "进行中" : statusText(state.status))
      }),
      ui.hero({
        title: "方向按钮或键盘方向键控制",
        sub: "吃到食物长一节，速度会越来越快",
        children: [
          ui.el("div", { class: "game-canvas-wrap" }, [canvas]),
          ui.el("div", { class: "game-dirs" }, dirButtons),
          ui.el("div", { class: "game-actions" }, [startButton, restartButton])
        ]
      }),
      state.achievementToast
        ? ui.panel({ title: "成就解锁", children: [ui.el("p", { text: state.achievementToast.title || "已解锁新成就" })] })
        : null
    ].filter(Boolean))
  },

  /** 画布进 DOM 后调用（浏览器入口负责时机） */
  attachCanvas: function () {
    if (!this.__canvas) return
    const canvas = this.__canvas
    if (typeof canvas.getContext !== "function") return
    const rect = typeof canvas.getBoundingClientRect === "function"
      ? canvas.getBoundingClientRect()
      : { width: canvas.clientWidth, height: canvas.clientHeight }
    const width = Math.round((rect && rect.width) || canvas.clientWidth || 320)
    const height = Math.round((rect && rect.height) || canvas.clientHeight || 240)
    if (width === 0 || height === 0) return

    const ctx = canvas.getContext("2d")
    if (!ctx) return
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 1
    canvas.width = width * dpr
    canvas.height = height * dpr
    if (typeof ctx.scale === "function") ctx.scale(dpr, dpr)
    this.__ctx = ctx
    this.__size = { w: width, h: height }
    this.draw()
  },

  /** 绘制（逐字照搬小程序版） */
  draw: function () {
    const ctx = this.__ctx
    const size = this.__size
    if (!ctx || !size) return
    const state = this.__game
    if (!state) return

    const cols = snake.SNAKE_CONFIG.GRID_WIDTH
    const rows = snake.SNAKE_CONFIG.GRID_HEIGHT
    const cell = Math.min(size.w / cols, size.h / rows)
    const offsetX = (size.w - cell * cols) / 2
    const offsetY = (size.h - cell * rows) / 2

    const grid = snake.getGrid(state)

    ctx.clearRect(0, 0, size.w, size.h)

    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const x = offsetX + c * cell
        const y = offsetY + r * cell
        const v = grid[r][c]
        if (v === 0) continue

        ctx.beginPath()
        ctx.fillStyle = v === 2 ? "#ff6b6b" : v === 3 ? "#51cf66" : "#38d9a9"
        const inset = v === 3 ? 1 : 2
        this.roundRect(ctx, x + inset, y + inset, cell - inset * 2, cell - inset * 2, v === 3 ? 6 : 4)
        ctx.fill()
      }
    }
  },

  /** 圆角矩形路径（照搬小程序版） */
  roundRect: function (ctx, x, y, w, h, r) {
    ctx.moveTo(x + r, y)
    ctx.lineTo(x + w - r, y)
    ctx.quadraticCurveTo(x + w, y, x + w, y + r)
    ctx.lineTo(x + w, y + h - r)
    ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
    ctx.lineTo(x + r, y + h)
    ctx.quadraticCurveTo(x, y + h, x, y + h - r)
    ctx.lineTo(x, y + r)
    ctx.quadraticCurveTo(x, y, x + r, y)
    ctx.closePath()
  },

  /** 键盘方向控制 */
  bindKeyboard: function () {
    if (typeof window === "undefined" || typeof window.addEventListener !== "function") return
    const self = this
    this.__keyHandler = function (event) {
      const key = event && event.key
      const map = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" }
      const dir = map[key]
      if (!dir) return
      if (typeof event.preventDefault === "function") event.preventDefault()
      self.onDirection(dir)
    }
    window.addEventListener("keydown", this.__keyHandler)
  },

  /** 开始 / 重新开始 */
  onStart: function () {
    this.__game = snake.createInitialState()
    this.setData({ status: "playing", score: 0 })
    this.startLoop()
    this.draw()
  },

  /** 暂停 */
  onPause: function () {
    if (this.data.status !== "playing") return
    this.stopLoop()
    this.setData({ status: "paused" })
  },

  /** 继续 */
  onResume: function () {
    if (this.data.status !== "paused") return
    this.setData({ status: "playing" })
    this.startLoop()
  },

  /** 方向按钮 / 键盘 */
  onDirection: function (dir) {
    if (this.data.status !== "playing") return
    const D = snake.Direction
    const map = { up: D.UP, down: D.DOWN, left: D.LEFT, right: D.RIGHT }
    const d = map[dir]
    if (d !== undefined) snake.setDirection(this.__game, d)
  },

  /** 启动定时循环 */
  startLoop: function () {
    const self = this
    this.stopLoop()
    this.__timer = setInterval(function () { self.step() }, this.__game.speed)
  },

  /** 停止定时循环 */
  stopLoop: function () {
    if (this.__timer) {
      clearInterval(this.__timer)
      this.__timer = null
    }
  },

  /** 单步推进（照搬小程序版） */
  step: function () {
    const self = this
    snake.tick(this.__game)

    if (this.__game.score > 0 && this.__game.score % 10 === 0) {
      // 每吃一个食物触发一次成就评估
      achievements.evaluate({ type: "snake.eat", score: this.__game.score })
      vibrate.trigger()
    }

    if (this.__game.isGameOver) {
      this.stopLoop()
      this.onGameOver()
      return
    }

    // 速度随得分提升，同步重启定时器
    this.stopLoop()
    this.__timer = setInterval(function () { self.step() }, this.__game.speed)

    this.setData({ score: this.__game.score })
    this.draw()
  },

  /** 游戏结束结算 */
  onGameOver: function () {
    const score = this.__game.score
    gameStorage.updateSnakeBestScore(score)
    achievements.evaluate({ type: "snake.gameOver", score })

    this.setData({ status: "over", score: score, best: gameStorage.getSnakeBestScore() })
    this.draw()
    platform.vibrateLong()
    platform.showToast({ title: "游戏结束，得分 " + score, icon: "none" })
  }
}

/**
 * 状态文案。
 * @param {string} status
 * @returns {string}
 */
function statusText (status) {
  if (status === "paused") return "已暂停"
  if (status === "over") return "已结束"
  return "准备好了吗？"
}

if (typeof global !== "undefined" && typeof global.__GAMES_PAGE__ === "function") global.__GAMES_PAGE__(PAGE)

module.exports = PAGE
