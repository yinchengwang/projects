"use strict"
/**
 * sudoku —— 数独
 *
 * 章节选择 → 9x9 棋盘 → 数字键盘填入。
 * 引擎在 shared/games/sudoku.js（零 wx 依赖，纯逻辑）。
 *
 * 与小程序版的差异：wx:for 的拍平数组改由 DOM 直接生成；
 * 其余交互逻辑（解锁检查、评星、成就、进度持久化）逐字沿用。
 */

const sudoku = require("../../../shared/games/sudoku.js")
const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const vibrate = require("../../../shared/games/vibrate.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

/** 每章 10 关 */
const LEVELS_PER_CHAPTER = 10

const PAGE = {
  data: {
    title: "数独",
    phase: "menu", // menu | play
    chapters: [],
    currentChapter: 0,
    currentLevel: 0,
    status: "ready", // ready | playing | won
    cells: [],
    cursorRow: 0,
    cursorCol: 0,
    selectedNumber: 1,
    emptyCount: 0,
    hintCount: 3,
    difficultyName: "",
    achievementToast: null
  },

  onMount: function () {
    this.__game = null
    this.__unsubscribe = null
    const self = this
    try {
      this.__unsubscribe = achievements.subscribe(function (a) {
        self.setData({ achievementToast: { title: a.name, desc: a.desc } })
      })
    } catch (e) {
      this.__unsubscribe = null
    }
    this.loadChapters()
  },

  onUnmount: function () {
    if (typeof this.__unsubscribe === "function") this.__unsubscribe()
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const self = this
    const children = [
      ui.pageHead({
        eyebrow: "数独",
        title: state.phase === "play" ? "第 " + state.currentLevel + " 关" : "挑一个章节",
        sub: state.phase === "play"
          ? (state.difficultyName || "简单") + " · 空格 " + (state.emptyCount || 0) + " · 提示 " + (state.hintCount || 0) + " 次"
          : "每章 10 关，拿到星数才能解锁下一章"
      })
    ]

    if (state.phase === "play") {
      children.push(this.renderBoard(state))
      children.push(ui.panel({ title: "数字", children: [this.renderKeypad(state)] }))
    } else {
      children.push(this.renderChapters(state))
    }

    if (state.achievementToast) {
      children.push(ui.panel({ title: "成就解锁", children: [ui.el("p", { text: state.achievementToast.title || "已解锁新成就" })] }))
    }
    return ui.el("div", { class: "games-page" }, children.filter(Boolean))
  },

  /** 9x9 棋盘 */
  renderBoard: function (state) {
    const self = this
    const grid = ui.el("div", { class: "game-sudoku-grid" })
    ;(state.cells || []).forEach(function (cell) {
      const classes = ["game-sudoku-cell"]
      if (cell.isGiven) classes.push("fixed")
      if (cell.isConflict) classes.push("error")
      if (cell.row === state.cursorRow && cell.col === state.cursorCol) classes.push("selected")
      const button = ui.el("button", {
        class: classes.join(" "),
        text: cell.value ? String(cell.value) : ""
      })
      button.addEventListener("click", function () { self.onCellTap(cell.row, cell.col) })
      grid.appendChild(button)
    })

    const keypadPanel = ui.el("div", { class: "game-actions" }, [])
    const backButton = ui.el("button", { class: "game-btn", text: "返回章节" })
    backButton.addEventListener("click", function () { self.onBack() })
    keypadPanel.appendChild(backButton)

    const hintButton = ui.el("button", { class: "game-btn", text: "提示" })
    hintButton.addEventListener("click", function () { self.onHint() })
    keypadPanel.appendChild(hintButton)

    const eraseButton = ui.el("button", { class: "game-btn", text: "擦除" })
    eraseButton.addEventListener("click", function () { self.onErase() })
    keypadPanel.appendChild(eraseButton)

    return ui.panel({ title: "棋盘", children: [grid, keypadPanel] })
  },

  /** 1-9 数字键盘 */
  renderKeypad: function (state) {
    const self = this
    const keys = []
    for (let n = 1; n <= 9; n++) {
      const button = ui.el("button", { class: "game-dir", text: String(n) })
      button.addEventListener("click", function () { self.onNumberTap(n) })
      keys.push(button)
    }
    return ui.el("div", { class: "game-dirs game-keypad" }, keys)
  },

  /** 章节与关卡列表 */
  renderChapters: function (state) {
    const self = this
    const blocks = (state.chapters || []).map(function (ch) {
      const levelButtons = (ch.levels || []).map(function (lv) {
        const button = ui.el("button", {
          class: "game-dir",
          text: lv.stars > 0 ? String(lv.stars) + "★" : String(lv.id)
        })
        button.addEventListener("click", function () {
          self.onLevelTap(ch.id, lv.id, lv.unlocked)
        })
        return button
      })
      const head = ui.el("p", { class: "block-title", text: "第 " + ch.id + " 章 · " + ch.totalStars + "/" + ch.maxStars + " 星" + (ch.unlocked ? "" : "（未解锁）") })
      return ui.el("div", { class: "game-chapter" }, [head, ui.el("div", { class: "game-dirs" }, levelButtons)])
    })
    return blocks.length
      ? ui.panel({ title: "章节", children: blocks })
      : ui.panel({ children: [ui.empty("还没有章节数据", "刷新试试")] })
  },

  /** 加载章节与星数进度 */
  loadChapters: function () {
    let chapters = []
    try {
      const saved = gameStorage.getSudokuData()
      chapters = (saved.chapters || []).map(function (ch) {
        const starKeys = Object.keys(ch.stars || {})
        const total = starKeys.reduce(function (sum, k) { return sum + (ch.stars[k] || 0) }, 0)
        return {
          id: ch.id,
          unlocked: !!ch.unlocked,
          totalStars: total,
          maxStars: LEVELS_PER_CHAPTER * 3,
          levels: PAGE.buildLevels(ch)
        }
      })
    } catch (e) {
      chapters = []
    }
    this.setData({ chapters: chapters })
  },

  /** 生成该章节的 10 个关卡格子 */
  buildLevels: function (chapter) {
    const levels = []
    const stars = chapter.stars || {}
    for (let i = 0; i < LEVELS_PER_CHAPTER; i++) {
      const levelId = i + 1
      levels.push({
        id: levelId,
        stars: stars[levelId] || 0,
        // 未解锁的关卡：前一关拿到过星才开放
        unlocked: i === 0 || (stars[i] || 0) > 0
      })
    }
    return levels
  },

  /** 开始某一关 */
  onLevelTap: function (chapterId, level, unlocked) {
    const chapter = (this.state.chapters || []).filter(function (c) { return c.id === chapterId })[0]
    if (!chapter) return
    if (!chapter.unlocked) {
      platform.showToast({ title: "请先完成上一章节", icon: "none" })
      return
    }
    if (unlocked === false) {
      platform.showToast({ title: "请先完成前一关", icon: "none" })
      return
    }

    // 难度随关卡递进：1-3 简单 / 4-6 中等 / 7-10 困难
    const difficulty = level <= 3 ? 0 : level <= 6 ? 1 : 2
    this.__game = sudoku.createInitialState(difficulty)

    this.setData({
      phase: "play",
      status: "playing",
      currentChapter: chapterId,
      currentLevel: level,
      difficultyName: sudoku.SUDOKU_CONFIG.DIFFICULTIES[difficulty].name,
      cells: this.buildCells(),
      cursorRow: this.__game.cursorRow,
      cursorCol: this.__game.cursorCol,
      selectedNumber: this.__game.selectedNumber,
      emptyCount: this.__game.emptyCount,
      hintCount: this.__game.hintCount
    })
  },

  /**
   * 把 9x9 棋盘拍平成 81 个格子。
   * @returns {Array}
   */
  buildCells: function () {
    const cells = []
    if (!this.__game) return cells
    for (let r = 0; r < 9; r++) {
      for (let c = 0; c < 9; c++) {
        const cell = this.__game.board[r][c]
        cells.push({
          row: r,
          col: c,
          value: cell.value,
          isGiven: cell.isGiven,
          isConflict: cell.isConflict
        })
      }
    }
    return cells
  },

  /** 选择格子 */
  onCellTap: function (row, col) {
    if (this.state.status !== "playing" || !this.__game) return
    this.__game.cursorRow = row
    this.__game.cursorCol = col
    this.setData({ cursorRow: row, cursorCol: col })
  },

  /** 选择数字并填入 */
  onNumberTap: function (num) {
    if (this.state.status !== "playing" || !this.__game) return
    sudoku.selectNumber(this.__game, num)
    sudoku.placeNumber(this.__game, num)
    vibrate.trigger()
    this.syncState()
  },

  /** 擦除 */
  onErase: function () {
    if (this.state.status !== "playing" || !this.__game) return
    sudoku.eraseCell(this.__game)
    this.syncState()
  },

  /** 提示 */
  onHint: function () {
    if (this.state.status !== "playing" || !this.__game) return
    const ok = sudoku.hint(this.__game)
    if (!ok) {
      platform.showToast({ title: "提示次数已用完", icon: "none" })
      return
    }
    this.syncState()
  },

  /** 同步引擎状态到视图 */
  syncState: function () {
    this.setData({
      cells: this.buildCells(),
      emptyCount: this.__game.emptyCount,
      hintCount: this.__game.hintCount,
      cursorRow: this.__game.cursorRow,
      cursorCol: this.__game.cursorCol
    })
    if (this.__game.isGameOver) this.onWin()
  },

  /** 通关结算：按剩余提示次数评星 */
  onWin: function () {
    this.setData({ status: "won" })

    const stars = this.calcStars()
    gameStorage.setSudokuChapterStars(this.state.currentChapter, this.state.currentLevel, stars)
    achievements.evaluate({ type: "sudoku.complete", hintCount: this.__game.hintCount })

    // 本章最后一关拿到星 → 解锁下一章
    if (this.state.currentLevel === LEVELS_PER_CHAPTER) {
      gameStorage.unlockSudokuChapter(this.state.currentChapter + 1)
    }

    this.loadChapters()
    platform.vibrateShort({ type: "medium" })
  },

  /** 星级：0 次提示 3 星，1 次 2 星，2 次以上 1 星 */
  calcStars: function () {
    if (!this.__game) return 1
    if (this.__game.hintCount >= 3) return 3
    if (this.__game.hintCount >= 2) return 2
    return 1
  },

  /** 返回章节列表 */
  onBack: function () {
    this.setData({ phase: "menu", currentLevel: 0 })
    this.loadChapters()
  }
}

if (typeof global !== "undefined" && typeof global.__GAMES_PAGE__ === "function") global.__GAMES_PAGE__(PAGE)

module.exports = PAGE
