/**
 * 今日 —— 首屏
 *
 * 结构（自上而下，拇指热区优先）：
 *   日期 + 问候 / 今日待办（可勾选、可加、优先级可循环）/ 4 指标带 / 心情打卡 / 快捷入口
 *
 * 设计要点：无装饰性大卡，待办直接置顶；一屏内回答"今天什么状态"。
 */

const { withTheme } = require('../../../../shared/core/with-theme.js')
const stores = require('../../../../shared/workbench/stores.js')
const dateUtil = require('../../../../shared/core/date.js')
const constants = require('../../../../shared/core/constants.js')
const platform = require('../../../../shared/platform.js')
const keys = require('../../../../shared/workbench/keys.js')
const ui = require('../../../../shared/ui.js')

const VISIBLE_TODOS = 3

const SHORTCUTS = [
  { key: 'notes', label: '笔记', path: '/pages/tech/notes/index' },
  { key: 'snippets', label: '片段', path: '/pages/tech/snippets/index' },
  { key: 'insights', label: '闪念', path: '/pages/insights/index' },
  { key: 'schedule', label: '日程', path: '/pages/life/schedule/index' },
  { key: 'projects', label: '项目', path: '/pages/projects/index' }
]

module.exports = withTheme({
  data: {
    title: '今日',

    dateLabel: '',
    greeting: '',
    nickname: '',

    todoItems: [],
    todoOpenCount: 0,
    todoTotal: 0,
    todoMoreCount: 0,
    showAllTodos: false,
    hasTodos: false,

    stats: [],
    moodLevels: [],
    moodToday: 0,
    moodLabel: '',

    shortcuts: SHORTCUTS,
    captureVisible: false
  },

  render: function (state) {
    const children = []
    children.push(ui.pageHead({ eyebrow: state.dateLabel, title: state.greeting + '，' + (state.nickname || '开发者'), sub: '今天的工作与生活，一眼掌握。' }))
    children.push(ui.statGrid((state.stats || []).map(function (item) {
      return { label: item.label, value: item.value, tone: item.highlight ? 'accent' : '' }
    })))

    const todoNodes = (state.todoItems || []).map(function (item) {
      const toggle = ui.el('button', { class: 'wb-check' + (item.done ? ' is-done' : ''), 'data-id': item.id, text: item.done ? '✓' : '○' })
      toggle.addEventListener('click', function () { this.onToggleTodo({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      const priority = ui.el('button', { class: 'wb-priority', text: item.priorityLabel || '一般', 'data-id': item.id, 'data-priority': item.priorityKey })
      priority.addEventListener('click', function () { this.onCyclePriority({ currentTarget: { dataset: { id: item.id } } }) }.bind(this))
      return ui.el('div', { class: 'wb-todo-row' }, [toggle, ui.el('span', { class: item.done ? 'is-done' : '', text: item.title }), priority])
    }.bind(this))
    if (!todoNodes.length) todoNodes.push(ui.empty('今天还没有待办', '添加一项小任务，开始今天的节奏。'))
    const todoActions = ui.el('div', { class: 'wb-actions' }, [
      ui.el('button', { class: 'btn primary', text: '添加待办', onclick: function () { this.onAddTodo() }.bind(this) }),
      ui.el('button', { class: 'btn', text: state.showAllTodos ? '收起' : '展开全部', onclick: function () { this.onToggleAllTodos() }.bind(this) })
    ])
    children.push(ui.panel({ title: '今日待办 · ' + state.todoOpenCount + ' 项未完成', children: [ui.el('div', { class: 'wb-todos' }, todoNodes), todoActions] }))

    const moodNodes = (state.moodLevels || []).map(function (item) {
      const button = ui.el('button', { class: 'wb-mood' + (item.active ? ' is-active' : ''), 'data-level': item.level, text: item.label })
      button.addEventListener('click', function () { this.onSetMood({ currentTarget: { dataset: { level: item.level } } }) }.bind(this))
      return button
    }.bind(this))
    children.push(ui.panel({ title: '此刻心情 · ' + state.moodLabel, children: [ui.el('div', { class: 'wb-moods' }, moodNodes)] }))

    const shortcutNodes = (state.shortcuts || []).map(function (item) {
      const button = ui.el('button', { class: 'wb-shortcut', text: item.label })
      button.addEventListener('click', function () { this.onGoShortcut({ currentTarget: { dataset: { path: item.path } } }) }.bind(this))
      return button
    }.bind(this))
    children.push(ui.panel({ title: '快捷入口', children: [ui.el('div', { class: 'wb-shortcuts' }, shortcutNodes)] }))
    return ui.el('div', { class: 'wb-page wb-dashboard' }, children)
  },

  onMount: function (rootEl) {
    this.rootElement = rootEl || this.rootElement
    this.refresh()
  },

  onLoad () {
    this.refresh()
  },

  onShow () {
    this.refresh()
  },

  /** 汇总所有首屏数据 */
  refresh () {
    const settings = stores.settings.get()
    const openTodos = stores.todos.getOpen()
    const allTodos = stores.todos.getAll()
    const limit = this.data.showAllTodos ? openTodos.length : VISIBLE_TODOS
    const visible = openTodos.slice(0, limit)

    const todoItems = visible.map(function (t) {
      const p = constants.priority(t.priority)
      return {
        id: t.id,
        title: t.title,
        done: !!t.done,
        barColor: p.color,
        priorityLabel: p.label,
        priorityKey: t.priority,
        categoryLabel: t.category ? constants.todoCategory(t.category).label : ''
      }
    })

    const moodEntries = stores.mood.getAll()
    const today = dateUtil.todayStr()
    let moodToday = 0
    for (let i = 0; i < moodEntries.length; i++) {
      if (moodEntries[i].date === today) moodToday = moodEntries[i].level
    }

    this.setData({
      dateLabel: dateUtil.todayLabel(),
      greeting: dateUtil.greeting(),
      nickname: settings.profile.nickname || 'DevBencHer',

      todoItems: todoItems,
      todoOpenCount: openTodos.length,
      todoTotal: allTodos.length,
      todoMoreCount: Math.max(0, openTodos.length - VISIBLE_TODOS),
      hasTodos: allTodos.length > 0,

      stats: this.buildStats(),
      moodLevels: this.buildMoodLevels(moodToday),
      moodToday: moodToday,
      moodLabel: moodToday ? constants.moodLevel(moodToday).label : '未记录'
    })
  },

  /** 4 个状态指标 */
  buildStats () {
    const notes = stores.techNotes.getAll().length
    const readingNow = stores.reading.count(function (r) { return r.status === 'reading' })
    const week = dateUtil.weekRange()
    const fitness = stores.fitness.getAll()
    const days = {}
    fitness.forEach(function (r) {
      if (r.date >= week.start && r.date <= week.end) days[r.date] = true
    })

    return [
      { key: 'notes', value: String(notes), label: '笔记', highlight: false },
      { key: 'reading', value: String(readingNow), label: '在读书', highlight: false },
      { key: 'fitness', value: String(Object.keys(days).length), label: '本周运动', highlight: false },
      { key: 'streak', value: String(this.calcStreak()), label: '连续天', highlight: true }
    ]
  },

  /**
   * 连续记录天数：从今天（或昨天）往前数有心情记录的连续天数
   * 今天未记录不算断，从昨天开始算
   */
  calcStreak () {
    const entries = stores.mood.getAll()
    const set = {}
    entries.forEach(function (e) { set[e.date] = true })

    const cursor = new Date()
    if (!set[dateUtil.todayStr()]) {
      cursor.setDate(cursor.getDate() - 1)
    }

    let streak = 0
    // 上限 3650 天，防止脏数据导致死循环
    for (let i = 0; i < 3650; i++) {
      const d = dateUtil.formatYMD(cursor)
      if (!set[d]) break
      streak += 1
      cursor.setDate(cursor.getDate() - 1)
    }
    return streak
  },

  /** 5 级心情圆点 */
  buildMoodLevels (current) {
    return constants.MOOD_LEVELS.map(function (m) {
      return {
        level: m.level,
        // 带表情：MOOD_LEVELS 里本来就有 emoji，之前谁都没渲染出来
        label: m.emoji + " " + m.label,
        color: m.color,
        active: m.level === current
      }
    })
  },

  // ==================== 待办 ====================

  onToggleTodo (e) {
    const id = e.currentTarget.dataset.id
    stores.todos.toggle(id)
    this.refresh()
    platform.vibrateShort({ type: 'light' })
  },

  /** 点优先级圆点循环切换 高 → 中 → 低 */
  onCyclePriority (e) {
    const id = e.currentTarget.dataset.id
    const item = stores.todos.find(id)
    if (!item) return
    const order = ['high', 'med', 'low']
    const next = order[(order.indexOf(item.priority) + 1) % order.length]
    stores.todos.update(id, { priority: next })
    this.refresh()
    platform.showToast({ title: '优先级：' + constants.priority(next).label, icon: 'none' })
  },

  onAddTodo () {
    const self = this
    platform.showModal({
      title: '添加待办',
      editable: true,
      placeholderText: '要做什么？'
    }).then(function (res) {
      if (!res.confirm) return
      const title = (res.content || '').trim()
      if (!title) return
      stores.todos.add({
        title: title,
        done: false,
        priority: 'med',
        category: 'work',
        dueDate: dateUtil.todayStr()
      })
      self.setData({ showAllTodos: true })
      self.refresh()
    })
  },

  onToggleAllTodos () {
    this.setData({ showAllTodos: !this.data.showAllTodos })
    this.refresh()
  },

  // ==================== 心情 ====================

  onSetMood (e) {
    const level = Number(e.currentTarget.dataset.level)
    stores.mood.upsertToday({ level: level, tags: [] })
    this.refresh()
    platform.vibrateShort({ type: 'light' })
    platform.showToast({ title: '已记录：' + constants.moodLevel(level).label, icon: 'none' })
  },

  // ==================== 导航 ====================

  onGoShortcut (e) {
    const path = e.currentTarget.dataset.path
    if (path) platform.navigateTo(path)
  },

  onGoProfile () {
    platform.switchTab('/workbench/pages/profile/index')
  },

  // ==================== 捕获浮层 ====================

  onOpenCapture () {
    this.setData({ captureVisible: true })
  },

  onCloseCapture () {
    this.setData({ captureVisible: false })
  },

  onCaptured () {
    this.refresh()
  },

  onShareAppMessage () {
    return { title: 'DevBench · 程序员个人工作台', path: '/pages/dashboard/index' }
  }
}, require('../../../../shared/workbench/keys.js').KEYS.settings)
