"use strict"
/**
 * stores.js —— workbench 的 12 个数据域实例
 *
 * 与小程序版的差异只有一处：settings 域的键从 `settings` 改成 `wb_settings`，
 * 避免与 knowledge-hub 的 `db:settings` 撞键。域集合与自定义方法一字未改。
 */

const store = require("../core/store.js")
const storage = require("../core/storage.js")
const seed = require("./seed.js")
const keys = require("./keys.js")

const KEYS = keys.KEYS

const todos = store.createListStore({ key: KEYS.todos, seed: seed.todos })
const techNotes = store.createListStore({ key: KEYS.techNotes, seed: seed.techNotes })
const snippets = store.createListStore({ key: KEYS.snippets, seed: seed.snippets })
const interview = store.createListStore({ key: KEYS.interview, seed: seed.interview })
const resources = store.createListStore({ key: KEYS.resources, seed: seed.resources })
const reading = store.createListStore({ key: KEYS.reading, seed: seed.reading })
const fitness = store.createListStore({ key: KEYS.fitness, seed: seed.fitness })
const mood = store.createListStore({ key: KEYS.mood, seed: seed.mood })
const schedule = store.createListStore({ key: KEYS.schedule, seed: seed.schedule })
const projects = store.createListStore({ key: KEYS.projects, seed: seed.projects })
const insights = store.createListStore({ key: KEYS.insights, seed: seed.insights })

const settings = store.createObjectStore({
  key: KEYS.settings,
  defaultValue: {
    theme: "auto",
    primaryColor: "sky",
    enableHaptics: true,
    enableDailyReminder: true,
    reminderTime: "09:00",
    profile: {
      nickname: "DevBencHer",
      role: "全栈工程师",
      bio: "对代码有洁癖的工程师，偶尔写写文字记录灵感。",
      motto: "Write less, ship more.",
      yearsOfExperience: 3
    },
    schemaVersion: 1
  }
})

/** 心情：每日一条，同日覆盖 */
const moodDomain = {
  getAll: mood.getAll,
  setAll: mood.setAll,
  add: mood.add,
  update: mood.update,
  find: mood.find,
  remove: mood.remove,
  clear: mood.clear,
  count: mood.count,

  /** 取某天的记录 */
  getByDate: function (date) {
    const list = mood.getAll()
    for (let i = 0; i < list.length; i++) {
      if (list[i].date === date) return list[i]
    }
    return null
  },

  /** 写入或更新某天的心情（幂等） */
  upsertToday: function (payload, date) {
    const d = date || require("../core/date.js").todayStr()
    const existing = this.getByDate(d)
    if (existing) {
      return mood.update(existing.id, Object.assign({}, payload, { date: d }))
    }
    return mood.add(Object.assign({ date: d, tags: [] }, payload))
  }
}

/** 读书：更新阅读进度 */
const readingDomain = {
  getAll: reading.getAll,
  setAll: reading.setAll,
  add: reading.add,
  update: reading.update,
  remove: reading.remove,
  find: reading.find,
  clear: reading.clear,
  count: reading.count,

  /** 更新页码并按需自动切换状态 */
  updateProgress: function (itemId, currentPage) {
    const item = this.find(itemId)
    if (!item) return null
    const total = item.totalPages || 0
    const patch = { currentPage: currentPage }
    if (total > 0 && currentPage >= total) {
      patch.status = "finished"
      patch.finishedAt = new Date().toISOString()
    } else if (currentPage > 0 && item.status === "wishlist") {
      patch.status = "reading"
      patch.startedAt = item.startedAt || new Date().toISOString()
    }
    return this.update(itemId, patch)
  }
}

/** 待办：统计辅助 */
const todosDomain = {
  getAll: todos.getAll,
  setAll: todos.setAll,
  add: todos.add,
  update: todos.update,
  remove: todos.remove,
  find: todos.find,
  clear: todos.clear,
  count: todos.count,

  /** 未完成列表，按优先级 high > med > low 排序 */
  getOpen: function () {
    const order = { high: 0, med: 1, low: 2 }
    return this.getAll()
      .filter(function (t) { return !t.done })
      .sort(function (a, b) {
        return (order[a.priority] === undefined ? 9 : order[a.priority]) -
          (order[b.priority] === undefined ? 9 : order[b.priority])
      })
  },

  /** 勾选 / 取消勾选 */
  toggle: function (itemId) {
    const item = this.find(itemId)
    if (!item) return null
    return this.update(itemId, { done: !item.done })
  },

  /** 清除已完成 */
  clearDone: function () {
    return this.setAll(this.getAll().filter(function (t) { return !t.done }))
  }
}

module.exports = {
  todos: todosDomain,
  techNotes: techNotes,
  snippets: snippets,
  interview: interview,
  resources: resources,
  reading: readingDomain,
  fitness: fitness,
  mood: moodDomain,
  schedule: schedule,
  projects: projects,
  insights: insights,
  settings: settings
}
