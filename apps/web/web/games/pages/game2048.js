"use strict"
/**
 * game2048 —— 2048（Canvas 2d）
 *
 * 引擎在 shared/games/game2048.js（零 wx 依赖，纯逻辑）。
 * 与小程序版的差异只有「怎么拿到 canvas」：换成 getBoundingClientRect。
 * 绘制代码逐字保留。
 */

const g2048 = require("../../../shared/games/game2048.js")
const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const vibrate = require("../../../shared/games/vibrate.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const PAGE = {
  data: {
    score: 0,
    best: 0,
    status: "ready", // ready | playing | over
    hasWon: false,
    keepGoing: false,
    achievementToast: null
  },

  onMount: function () {
    this.__timer = null
    this.__canvas = null
    this.__ctx = null
    this.__size = null
    this.__unsubscribe = null
    this.__game = g2048.createInitialState(gameStorage.get2048BestScore())

    const self = this
    try {
      this.__unsubscribe = achievements.subscribe(function (a) {
        self.setData({ achievementToast: { title: a.name, desc: a.desc } })
      })
    } catch (e) {
      this.__unsubscribe = null
    }

    this.setData({ best: gameStorage.get2048BestScore() })
    this.bindKeyboard()
  },

  onUnmount: function () {
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
      button.addEventListener("click", function () { self.doMove(d) })
      return button
    })

    const startButton = ui.el("button", { class: "game-btn primary", text: "开始新局" })
    startButton.addEventListener("click", function () { self.onStart() })

    const children = [
      ui.el("div", { class: "game-canvas-wrap" }, [canvas]),
      ui.el("div", { class: "game-dirs" }, dirButtons),
      ui.el("div", { class: "game-actions" }, [startButton])
    ]

    if (state.hasWon && !state.keepGoing) {
      const keepButton = ui.el("button", { class: "game-btn", text: "继续挑战" })
      keepButton.addEventListener("click", function () { self.onKeepGoing() })
      const endButton = ui.el("button", { class: "game-btn", text: "结束本局" })
      endButton.addEventListener("click", function () { self.onEndWin() })
      children.push(ui.el("div", { class: "game-actions" }, [keepButton, endButton]))
    }

    return ui.el("div", { class: "games-page" }, [
      ui.pageHead({
        eyebrow: "2048",
        title: "得分 " + (state.score || 0),
        sub: "最高 " + (state.best || 0) + " 分 · " + statusText(state.status)
      }),
      ui.hero({
        title: state.hasWon ? "已合成 2048！" : "相同数字合并",
        sub: "方向按钮或键盘方向键控制",
        children: children
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
    if (width === 0) return

    const ctx = canvas.getContext("2d")
    if (!ctx) return
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 1
    canvas.width = width * dpr
    canvas.height = width * dpr
    if (typeof ctx.scale === "function") ctx.scale(dpr, dpr)
    this.__ctx = ctx
    this.__size = { w: width, h: width }
    this.draw()
  },

  /** 绘制（逐字照搬小程序版） */
  draw: function () {
    const ctx = this.__ctx
    const size = this.__size
    if (!ctx || !size) return
    const state = this.__game
    if (!state) return

    const cells = g2048.CONFIG.GRID_SIZE
    const cell = size.w / cells
    const gap = 8
    const pad = gap

    ctx.clearRect(0, 0, size.w, size.w)

    // 棋盘底色
    ctx.fillStyle = "#3c3a32"
    this.roundRect(ctx, 0, 0, size.w, size.w, 12)
    ctx.fill()

    const board = state.board
    for (let r = 0; r < cells; r++) {
      for (let c = 0; c < cells; c++) {
        const value = board[r][c]
        if (value === 0) continue

        const x = pad + c * cell + gap / 2
        const y = pad + r * cell + gap / 2
        const w = cell - gap
        const h = cell - gap

        ctx.fillStyle = g2048.getTileColor(value)
        this.roundRect(ctx, x, y, w, h, 8)
        ctx.fill()

        ctx.fillStyle = g2048.getTextColor(value)
        ctx.font = "bold " + (value >= 1024 ? 28 : value >= 128 ? 34 : 40) + "px sans-serif"
        ctx.textAlign = "center"
        ctx.textBaseline = "middle"
        ctx.fillText(String(value), x + w / 2, y + h / 2)
      }
    }
  },

  /** 圆角矩形路径（照搬小程序版，含 beginPath） */
  roundRect: function (ctx, x, y, w, h, r) {
    ctx.beginPath()
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
      const map = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" }
      const dir = map[event && event.key]
      if (!dir) return
      if (typeof event.preventDefault === "function") event.preventDefault()
      self.doMove(dir)
    }
    window.addEventListener("keydown", this.__keyHandler)
  },

  /** 开新局 */
  onStart: function () {
    this.__game = g2048.createInitialState(gameStorage.get2048BestScore())
    this.setData({ score: 0, status: "playing", hasWon: false, keepGoing: false })
    this.draw()
  },

  /** 达成 2048 后选择继续 */
  onKeepGoing: function () {
    g2048.keepGoing(this.__game)
    this.setData({ keepGoing: true })
  },

  /** 达成 2048 后选择结束本局 */
  onEndWin: function () {
    this.finishGame()
  },

  /** 执行移动 */
  doMove: function (direction) {
    if (this.data.status !== "playing") return
    if (this.__game.hasWon && !this.__game.keepGoing) return

    const result = g2048.move(this.__game, direction)
    if (!result.moved) return

    // 合并出 2048 / 4096 时评估成就
    if (result.mergedTile >= 2048) {
      achievements.evaluate({ type: "game2048.merge", tileValue: result.mergedTile })
    }

    this.setData({
      score: this.__game.score,
      best: Math.max(this.data.best, this.__game.bestScore),
      hasWon: this.__game.hasWon,
      keepGoing: this.__game.keepGoing
    })

    vibrate.trigger()
    this.draw()

    if (this.__game.isGameOver) this.finishGame()
  },

  /** 结束并持久化最高分 */
  finishGame: function () {
    gameStorage.update2048BestScore(this.__game.score)
    this.setData({ status: "over", best: gameStorage.get2048BestScore() })
    this.draw()
  }
}

/**
 * 状态文案。
 * @param {string} status
 * @returns {string}
 */
function statusText (status) {
  if (status === "playing") return "进行中"
  if (status === "over") return "已结束"
  return "准备好了吗？"
}

if (typeof global !== "undefined" && typeof global.__GAMES_PAGE__ === "function") global.__GAMES_PAGE__(PAGE)

module.exports = PAGE
