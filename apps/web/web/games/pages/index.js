"use strict"
/**
 * index —— 游戏中心首页
 *
 * 4 个游戏 + 各自最高分 / 星数 + 成就墙 + 清空进度。
 * 汇总数据直接取 storage.getOverview()，不逐个游戏自己算。
 */

const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

/** 4 个游戏的入口定义 */
const ENTRIES = [
  { key: "snake", label: "贪吃蛇", emoji: "🐍", desc: "经典贪吃蛇 · 越吃越快", path: "/games/pages/snake" },
  { key: "game2048", label: "2048", emoji: "🔢", desc: "数字合成 · 挑战 2048", path: "/games/pages/game2048" },
  { key: "sudoku", label: "数独", emoji: "🔲", desc: "五章关卡 · 逻辑推理", path: "/games/pages/sudoku" },
  { key: "match3", label: "消消乐", emoji: "💎", desc: "六章 50 关 · 特效消除", path: "/games/pages/match3" }
]

const PAGE = {
  data: {
    title: "游戏中心",
    entries: [],
    showAchievements: false,
    achievements: [],
    unlockedCount: 0,
    achievementTotal: 0
  },

  onMount: function () {
    gameStorage.migrate()
    this.refresh()
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const self = this

    const cards = (state.entries || []).map(function (e) {
      return ui.el("a", { class: "game-entry", href: e.path }, [
        ui.el("span", { class: "game-entry-label", text: e.emoji + " " + e.label }),
        ui.el("span", { class: "game-entry-desc", text: e.desc }),
        ui.el("span", { class: "game-entry-best", text: e.best })
      ])
    })

    const toggle = ui.el("button", {
      class: "game-btn",
      text: state.showAchievements ? "收起成就墙" : "成就墙 · " + (state.unlockedCount || 0) + "/" + (state.achievementTotal || 0)
    })
    toggle.addEventListener("click", function () { self.onToggleAchievements() })

    const clearButton = ui.el("button", { class: "game-btn", text: "清空全部进度" })
    clearButton.addEventListener("click", function () { self.onClear() })

    const achievementNodes = (state.achievements || []).map(function (a) {
      return ui.el("div", { class: "game-achievement" + (a.unlocked ? " is-unlocked" : "") }, [
        ui.el("span", { class: "game-achievement-emoji", text: a.emoji }),
        ui.el("span", { class: "game-achievement-name", text: a.name }),
        ui.el("span", { class: "game-achievement-desc", text: a.desc })
      ])
    })

    return ui.el("div", { class: "games-page" }, [
      ui.pageHead({ eyebrow: "游戏中心", title: "挑一个玩玩", sub: "进度都存在本机" }),
      ui.el("div", { class: "game-grid" }, cards),
      ui.panel({
        title: "成就",
        children: [
          ui.el("div", { class: "game-actions" }, [toggle, clearButton]),
          state.showAchievements
            ? ui.el("div", { class: "game-achievements" }, achievementNodes)
            : ui.empty("成就墙收起中", "点上面的按钮展开全部 8 项成就")
        ]
      })
    ])
  },

  /** 读汇总数据 */
  refresh: function () {
    let overview = { snakeBest: 0, g2048Best: 0, sudokuStars: 0, match3Stars: 0, achievementUnlocked: 0 }
    try {
      overview = gameStorage.getOverview()
    } catch (e) {
      // 读不到就用全 0，页面仍可显示
    }
    const bestMap = {
      snake: "最高 " + overview.snakeBest + " 分",
      game2048: "最高 " + overview.g2048Best + " 分",
      sudoku: "已获 " + overview.sudokuStars + " 星",
      match3: "已获 " + overview.match3Stars + " 星"
    }
    this.setData({
      entries: ENTRIES.map(function (e) {
        return { key: e.key, label: e.label, emoji: e.emoji, desc: e.desc, path: e.path, best: bestMap[e.key] }
      }),
      unlockedCount: overview.achievementUnlocked,
      achievementTotal: achievements.ALL_IDS.length
    })
  },

  /** 展开 / 收起成就墙 */
  onToggleAchievements: function () {
    if (this.state.showAchievements) {
      this.setData({ showAchievements: false, achievements: [] })
      return
    }
    let list = []
    try {
      list = achievements.getAchievementList()
    } catch (e) {
      list = []
    }
    this.setData({ showAchievements: true, achievements: list })
  },

  /** 清空全部游戏存档 */
  onClear: function () {
    const self = this
    return platform.showModal({
      title: "清空所有数据",
      content: "将删除全部最高分、关卡进度与成就，且不可恢复。确认继续？",
      confirmText: "确认清空",
      confirmColor: "#e53e3e"
    }).then(function (r) {
      if (!r.confirm) return null
      gameStorage.resetAll()
      self.setData({ showAchievements: false, achievements: [] })
      self.refresh()
      platform.showToast({ title: "已清空", icon: "success" })
      return null
    })
  }
}

if (typeof global !== "undefined" && typeof global.__GAMES_PAGE__ === "function") global.__GAMES_PAGE__(PAGE)

module.exports = PAGE
