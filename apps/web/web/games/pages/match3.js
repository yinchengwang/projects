"use strict"
/**
 * match3 —— 消消乐
 *
 * 章节选择 → 9x9 棋盘两段式点选交换 → 连锁消除 → 目标达成。
 * 引擎在 shared/games/match3.js（零 wx 依赖，纯逻辑）。
 *
 * 与小程序版的差异：连锁过程由 setTimeout 逐帧动画改成**同步循环结算**
 * （结论完全一致，只是没有动画间隔），避免 Web 侧定时器泄漏与竞态。
 */

const match3 = require("../../../shared/games/match3.js")
const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const vibrate = require("../../../shared/games/vibrate.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const PAGE = {
  data: {
    title: "消消乐",
    phase: "menu", // menu | play
    chapters: [],
    totalStars: 0,
    level: 1,
    chapter: 1,
    cells: [],
    moves: 0,
    maxMoves: 0,
    score: 0,
    goals: [],
    status: "playing", // playing | won | lost
    selected: "",
    stars: 0,
    chapterName: "",
    levelTypeName: "",
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
    const children = [
      ui.pageHead({
        eyebrow: "消消乐",
        title: state.phase === "play"
          ? "第 " + state.level + " 关 · " + (state.chapterName || "")
          : "已获 " + (state.totalStars || 0) + " 星",
        sub: state.phase === "play"
          ? (state.levelTypeName || "普通关") + " · 得分 " + (state.score || 0) + " · 剩余 " + (state.moves || 0) + " 步"
          : "六章 50 关，星数足够才解锁下一章"
      })
    ]

    children.push(state.phase === "play" ? this.renderBoard(state) : this.renderChapters(state))

    if (state.achievementToast) {
      children.push(ui.panel({ title: "成就解锁", children: [ui.el("p", { text: state.achievementToast.title || "已解锁新成就" })] }))
    }
    return ui.el("div", { class: "games-page" }, children.filter(Boolean))
  },

  /** 棋盘 */
  renderBoard: function (state) {
    const self = this
    const grid = ui.el("div", { class: "game-match3-grid" })
    ;(state.cells || []).forEach(function (cell) {
      const button = ui.el("button", {
        class: "game-match3-cell" + (state.selected === cell.pos ? " selected" : "") + (cell.hasGem ? "" : " empty"),
        text: cell.elementEmoji || cell.obstacleEmoji || ""
      })
      if (cell.bg && button.style !== undefined) button.style.background = cell.bg
      button.addEventListener("click", function () { self.onCellTap(cell.pos) })
      grid.appendChild(button)
    })

    const goalNodes = (state.goals || []).map(function (g) {
      return ui.pill(g.emoji + " " + g.current + "/" + g.target, g.done ? "ok" : "")
    })

    const retryButton = ui.el("button", { class: "game-btn primary", text: "重玩本关" })
    retryButton.addEventListener("click", function () { self.onRetry() })
    const backButton = ui.el("button", { class: "game-btn", text: "返回章节" })
    backButton.addEventListener("click", function () { self.onBack() })

    return ui.panel({
      title: state.status === "won" ? "通关！获得 " + state.stars + " 星" : (state.status === "lost" ? "本关失败" : "目标"),
      children: [
        ui.el("div", { class: "game-goals" }, goalNodes),
        grid,
        ui.el("div", { class: "game-actions" }, [retryButton, backButton])
      ]
    })
  },

  /** 章节列表 */
  renderChapters: function (state) {
    const self = this
    const blocks = (state.chapters || []).map(function (ch) {
      const button = ui.el("button", {
        class: "game-btn" + (ch.unlocked ? " primary" : ""),
        text: ch.name + " " + ch.typeName + " " + ch.levelsText + " · " + ch.stars + "/" + ch.maxStars + " 星" + (ch.unlocked ? "" : "（未解锁）")
      })
      button.addEventListener("click", function () { self.onChapterTap(ch.id) })
      return ui.el("div", { class: "game-chapter" }, [button])
    })
    return blocks.length
      ? ui.panel({ title: "章节", children: blocks })
      : ui.panel({ children: [ui.empty("还没有章节数据", "刷新试试")] })
  },

  /** 加载章节 + 解锁 + 星数 */
  loadChapters: function () {
    let chapters = []
    let totalStars = 0
    try {
      const unlocked = gameStorage.getMatch3UnlockedChapters()
      const earned = gameStorage.getMatch3EarnedStars()
      totalStars = gameStorage.getMatch3TotalStars()

      chapters = match3.CHAPTERS.map(function (c) {
        let stars = 0
        let cleared = 0
        for (let lv = c.levels[0]; lv <= c.levels[1]; lv++) {
          const s = earned[lv] || 0
          stars += s
          if (s > 0) cleared++
        }
        const typeInfo = match3.LEVEL_TYPE_INFO[c.type]
        return {
          id: c.id,
          name: c.name,
          type: c.type,
          typeName: typeInfo.name,
          description: c.description,
          levelsText: c.levels[0] + "-" + c.levels[1],
          unlocked: match3.isChapterUnlocked(c, totalStars, unlocked),
          stars: stars,
          maxStars: (c.levels[1] - c.levels[0] + 1) * 3,
          cleared: cleared,
          totalLevels: c.levels[1] - c.levels[0] + 1
        }
      })
    } catch (e) {
      chapters = []
    }
    this.setData({ chapters: chapters, totalStars: totalStars })
  },

  /** 进入章节（选第 1 关开始） */
  onChapterTap: function (chapterId) {
    const chapter = (this.state.chapters || []).filter(function (c) { return c.id === chapterId })[0]
    if (!chapter) return
    if (!chapter.unlocked) {
      platform.showToast({ title: "星数不足，章节未解锁", icon: "none" })
      return
    }
    this.startLevel(chapter.levelsText.split("-").map(Number)[0])
  },

  /** 开始关卡 */
  startLevel: function (level) {
    this.__game = match3.createGameState(
      level,
      gameStorage.getMatch3EarnedStars(),
      gameStorage.getMatch3UnlockedChapters()
    )

    const config = match3.getLevelConfig(level)
    const chapter = match3.getChapterByLevel(level)
    const typeInfo = match3.LEVEL_TYPE_INFO[config.type]

    this.setData({
      phase: "play",
      level: level,
      chapter: config.chapter,
      chapterName: chapter ? chapter.name : "",
      levelTypeName: typeInfo.name,
      cells: this.buildCells(),
      moves: this.__game.moves,
      maxMoves: this.__game.maxMoves,
      score: this.__game.score,
      goals: this.buildGoals(),
      status: "playing",
      selected: ""
    })
  },

  /**
   * 把棋盘拍平成 81 格。
   * @returns {Array}
   */
  buildCells: function () {
    const cells = []
    if (!this.__game) return cells
    const size = match3.BOARD_SIZE
    for (let r = 0; r < size; r++) {
      for (let c = 0; c < size; c++) {
        const cell = this.__game.board[r][c]
        const gem = cell.gem
        const gemColors = gem === null ? null : match3.GEM_COLORS[gem]
        const obstacle = cell.obstacle ? match3.getObstacleRenderInfo(cell) : null
        const element = cell.element ? match3.ELEMENT_INFO[cell.element] : null

        cells.push({
          row: r,
          col: c,
          pos: r + "," + c,
          hasGem: gem !== null,
          bg: gemColors ? gemColors.primary : "",
          obstacleEmoji: obstacle ? obstacle.emoji : "",
          obstacleHp: obstacle ? obstacle.hp : 0,
          elementEmoji: element ? element.emoji : ""
        })
      }
    }
    return cells
  },

  /** 目标进度（含 emoji） */
  buildGoals: function () {
    if (!this.__game) return []
    return this.__game.goals.map(function (g) {
      return {
        type: g.type,
        current: Math.min(g.current, g.target),
        target: g.target,
        emoji: g.type === "score" ? "⭐" : ((match3.ELEMENT_INFO[g.type] || {}).emoji || "❓"),
        done: g.current >= g.target
      }
    })
  },

  /** 两段式选择 → 交换 */
  onCellTap: function (pos) {
    if (this.state.status !== "playing" || !this.__game) return

    if (!this.state.selected) {
      this.setData({ selected: pos })
      return
    }
    if (this.state.selected === pos) {
      this.setData({ selected: "" })
      return
    }
    if (!match3.isValidSwap(this.__game.board, this.state.selected, pos)) {
      platform.showToast({ title: "只能交换相邻宝石", icon: "none" })
      this.setData({ selected: "" })
      return
    }

    const from = this.state.selected
    this.setData({ selected: "" })
    this.attemptSwap(from, pos)
  },

  /** 尝试交换：无匹配则回弹 */
  attemptSwap: function (pos1, pos2) {
    match3.doSwap(this.__game.board, pos1, pos2)
    const matches = match3.findMatches(this.__game.board)

    if (matches.length === 0) {
      // 无匹配 → 回弹（不放动画延时，直接换回来）
      match3.doSwap(this.__game.board, pos1, pos2)
      this.setData({ cells: this.buildCells() })
      return
    }
    this.resolveMatches(matches)
  },

  /**
   * 连锁结算主循环（同步，结论与小程序版一致）。
   * @param {Array} matches 首轮匹配
   */
  resolveMatches: function (matches) {
    let combo = 0
    let current = matches

    for (let guard = 0; guard < 64 && current.length > 0; guard++) {
      combo++

      const positions = new Set()
      const shape = match3.analyzeMatchShape(current)
      current.forEach(function (m) {
        m.cells.forEach(function (p) { positions.add(p) })
      })

      if (combo > 1) platform.vibrateShort({ type: "light" })

      // 5 连 → 彩虹；交叉 → 炸弹；4 连 → 直线
      let special = null
      let specialPos = null
      if (shape.line5.length > 0) {
        special = match3.SpecialType.RAINBOW
        specialPos = shape.line5[0]
      } else if (shape.intersections.length > 0) {
        special = match3.SpecialType.BOMB
        specialPos = shape.intersections[0]
      } else if (shape.line4.length > 0) {
        special = match3.SpecialType.ROW_CLEAR
        specialPos = shape.line4[0]
      }

      if (special && specialPos) {
        match3.triggerSpecial(this.__game.board, specialPos, special).forEach(function (p) { positions.add(p) })
      }

      this.applyRemoval(positions)
      this.__game.score += match3.calculateScore(positions.size, combo)

      // 下落 + 补充
      match3.dropGems(this.__game.board)
      current = match3.findMatches(this.__game.board)
    }

    this.__game.moves--
    this.afterTurn()
  },

  /**
   * 执行消除：伤害障碍物、收集特殊元素。
   * @param {Set<string>} positions 被消除的位置集合
   */
  applyRemoval: function (positions) {
    const board = this.__game.board
    const self = this

    positions.forEach(function (pos) {
      const p = pos.split(",").map(Number)
      const cell = board[p[0]][p[1]]

      // 特殊元素收集
      if (cell.element) {
        self.__game.collected[cell.element] = (self.__game.collected[cell.element] || 0) + 1
        cell.element = null
      }

      // 障碍物伤害
      if (cell.obstacle) {
        match3.damageObstacle(cell, 1)
        if (match3.isObstacleDestroyed(cell)) match3.clearObstacle(cell)
        return // 障碍未破则宝石不消
      }

      // 特效宝石触发
      if (cell.special) {
        match3.triggerSpecial(board, pos, cell.special).forEach(function (p2) {
          const q = p2.split(",").map(Number)
          if (board[q[0]][q[1]].gem !== null) board[q[0]][q[1]].gem = null
        })
      }

      cell.gem = null
      cell.special = null
    })

    // 同步目标进度
    this.__game.goals.forEach(function (g) {
      if (g.type === "score") g.current = self.__game.score
      else g.current = self.__game.collected[g.type] || 0
    })
  },

  /** 一回合结束后的结算 */
  afterTurn: function () {
    const snailResult = match3.updateSnails(this.__game)
    const board = this.__game.board
    snailResult.snailRemoved.forEach(function (pos) {
      const p = pos.split(",").map(Number)
      if (p[0] >= 0 && p[0] < match3.BOARD_SIZE) board[p[0]][p[1]].gem = null
    })

    const goalsDone = match3.checkGoalsComplete(this.__game)
    const movesOut = this.__game.moves <= 0

    this.setData({
      cells: this.buildCells(),
      moves: Math.max(0, this.__game.moves),
      score: this.__game.score,
      goals: this.buildGoals()
    })
    vibrate.trigger()

    if (snailResult.gameOver || movesOut) {
      this.finish("lost")
      return
    }
    if (goalsDone) {
      this.finish("won")
      return
    }
  },

  /** 结束本关 */
  finish: function (result) {
    if (result === "won") {
      const stars = match3.calculateStars(
        this.__game.score,
        match3.getLevelConfig(this.__game.level).targetScore,
        Math.max(0, this.__game.moves),
        this.__game.maxMoves
      )
      this.__game.status = "won"
      this.__game.stars = stars

      gameStorage.setMatch3Stars(this.__game.level, stars)
      achievements.evaluate({ type: "match3.complete", stars: stars })

      // 每章最后一关解锁下一章
      const chapter = match3.getChapterByLevel(this.__game.level)
      if (chapter && this.__game.level === chapter.levels[1] && chapter.id < match3.CHAPTERS.length) {
        gameStorage.unlockMatch3Chapter(chapter.id + 1)
      }
      this.loadChapters()
      this.setData({ status: "won", stars: stars })
      return
    }
    this.__game.status = "lost"
    this.setData({ status: "lost", stars: 0 })
  },

  /** 重玩本关 */
  onRetry: function () {
    this.startLevel(this.state.level)
  },

  /** 返回章节 */
  onBack: function () {
    this.setData({ phase: "menu" })
    this.loadChapters()
  }
}

if (typeof global !== "undefined" && typeof global.__GAMES_PAGE__ === "function") global.__GAMES_PAGE__(PAGE)

module.exports = PAGE
