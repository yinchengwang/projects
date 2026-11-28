/**
 * pages.test.js —— 今日页与我的页交互测试
 *
 * 用法：node tests/pages.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')
const dom = require('./dom-stub.js')
dom.install()

stub.installLocalStorage()

const h = require('./wb-harness.js')
const stores = require('../shared/workbench/stores.js')
const storage = require('../shared/core/storage.js')

const runner = require('./wb-runner.js').createRunner()
const test = function (name, fn) {
  runner.test(name, fn, function () {
    stub.reset()
    if (global.localStorage && typeof global.localStorage.clear === 'function') global.localStorage.clear()
  })
}

/** 打开今日页（onLoad + onShow，与真实进入一致） */
function openDashboard () {
  const page = h.createPage('pages/dashboard/index')
  page.onMount(document.body)
  page.onLoad()
  page.onShow()
  return page
}

function openProfile () {
  const page = h.createPage('pages/profile/index')
  page.onMount(document.body)
  page.onLoad()
  page.onShow()
  return page
}

// ==================== 今日页 ====================

console.log('\n\x1b[1m今日页 · 首屏数据\x1b[0m')

test('日期与问候语非空', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.dateLabel.length > 0, true)
  assert.strictEqual(page.data.greeting.length > 0, true)
})

test('昵称来自设置', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.nickname, 'DevBencHer')
})

test('待办最多显示 3 条', () => {
  stores.todos.add({ title: 'a', done: false, priority: 'high' })
  stores.todos.add({ title: 'b', done: false, priority: 'med' })
  stores.todos.add({ title: 'c', done: false, priority: 'low' })
  stores.todos.add({ title: 'd', done: false, priority: 'med' })
  const page = openDashboard()
  assert.strictEqual(page.data.todoItems.length, 3)
})

test('未完成数超过 3 时显示"还有 N 项"', () => {
  const page = openDashboard()
  const open = page.data.todoOpenCount
  assert.strictEqual(page.data.todoMoreCount, Math.max(0, open - 3))
})

test('待办按优先级排序：高在前', () => {
  stores.todos.clear()
  stores.todos.add({ title: 'low', done: false, priority: 'low' })
  stores.todos.add({ title: 'high', done: false, priority: 'high' })
  stores.todos.add({ title: 'med', done: false, priority: 'med' })
  const page = openDashboard()
  assert.strictEqual(page.data.todoItems[0].title, 'high')
  assert.strictEqual(page.data.todoItems[1].title, 'med')
  assert.strictEqual(page.data.todoItems[2].title, 'low')
})

test('已完成待办不出现在待办列表', () => {
  stores.todos.clear()
  stores.todos.add({ title: 'done-item', done: true, priority: 'high' })
  const page = openDashboard()
  assert.strictEqual(page.data.todoOpenCount, 0)
  assert.strictEqual(page.data.todoItems.length, 0)
})

test('4 个指标全部存在', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.stats.length, 4)
  const labels = page.data.stats.map(s => s.label)
  assert.deepStrictEqual(labels, ['笔记', '在读书', '本周运动', '连续天'])
})

test('笔记指标等于实际条数', () => {
  const page = openDashboard()
  const notes = page.data.stats.filter(s => s.key === 'notes')[0]
  assert.strictEqual(notes.value, String(stores.techNotes.getAll().length))
})

test('本周运动只统计本周记录', () => {
  stores.fitness.clear()
  const dateUtil = require('../shared/core/date.js')
  stores.fitness.add({ date: dateUtil.todayStr(), type: 'cardio', name: '跑步', durationMin: 30 })
  stores.fitness.add({ date: '2020-01-01', type: 'cardio', name: '旧记录', durationMin: 30 })
  const page = openDashboard()
  const fit = page.data.stats.filter(s => s.key === 'fitness')[0]
  assert.strictEqual(fit.value, '1')
})

test('心情 5 个圆点', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.moodLevels.length, 5)
})

test('种子数据里今天已有心情，圆点被选中', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.moodToday, 4)
  const active = page.data.moodLevels.filter(m => m.active)
  assert.strictEqual(active.length, 1)
  assert.strictEqual(active[0].level, 4)
})

test('5 个快捷入口', () => {
  const page = openDashboard()
  assert.strictEqual(page.data.shortcuts.length, 5)
})

// ==================== 今日页 · 交互 ====================

console.log('\n\x1b[1m今日页 · 交互\x1b[0m')

test('勾选待办后未完成数减 1', () => {
  stores.todos.clear()
  const item = stores.todos.add({ title: '任务', done: false, priority: 'med' })
  const page = openDashboard()
  assert.strictEqual(page.data.todoOpenCount, 1)
  h.tap(page, 'onToggleTodo', { id: item.id })
  assert.strictEqual(page.data.todoOpenCount, 0)
  assert.strictEqual(stores.todos.find(item.id).done, true)
})

test('再次勾选可取消完成', () => {
  stores.todos.clear()
  const item = stores.todos.add({ title: '任务', done: false, priority: 'med' })
  const page = openDashboard()
  h.tap(page, 'onToggleTodo', { id: item.id })
  h.tap(page, 'onToggleTodo', { id: item.id })
  assert.strictEqual(stores.todos.find(item.id).done, false)
})

test('优先级循环：高 → 中 → 低 → 高', () => {
  stores.todos.clear()
  const item = stores.todos.add({ title: '任务', done: false, priority: 'high' })
  const page = openDashboard()

  h.tap(page, 'onCyclePriority', { id: item.id })
  assert.strictEqual(stores.todos.find(item.id).priority, 'med')
  h.tap(page, 'onCyclePriority', { id: item.id })
  assert.strictEqual(stores.todos.find(item.id).priority, 'low')
  h.tap(page, 'onCyclePriority', { id: item.id })
  assert.strictEqual(stores.todos.find(item.id).priority, 'high')
})

test('添加待办写入 store 并展开列表', () => {
  stores.todos.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '  新任务  ' })
  })
  try {
    const page = openDashboard()
    page.onAddTodo()
    return new Promise(function (resolve) { setImmediate(resolve) }).then(function () {
      const all = stores.todos.getAll()
      assert.strictEqual(all.length, 1)
      assert.strictEqual(all[0].title, '新任务', '首尾空格应被裁掉')
      assert.strictEqual(page.data.showAllTodos, true)
    })
  } finally {
    restore()
  }
})

test('取消添加不写入', () => {
  stores.todos.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: false })
  })
  try {
    const page = openDashboard()
    page.onAddTodo()
    assert.strictEqual(stores.todos.getAll().length, 0)
  } finally {
    restore()
  }
})

test('空内容不写入', () => {
  stores.todos.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '   ' })
  })
  try {
    const page = openDashboard()
    page.onAddTodo()
    assert.strictEqual(stores.todos.getAll().length, 0)
  } finally {
    restore()
  }
})

test('展开全部待办后显示超过 3 条', () => {
  stores.todos.clear()
  for (let i = 0; i < 5; i++) {
    stores.todos.add({ title: 't' + i, done: false, priority: 'med' })
  }
  const page = openDashboard()
  assert.strictEqual(page.data.todoItems.length, 3)
  page.onToggleAllTodos()
  assert.strictEqual(page.data.todoItems.length, 5)
  page.onToggleAllTodos()
  assert.strictEqual(page.data.todoItems.length, 3)
})

test('打卡心情后当日记录更新', () => {
  stores.mood.clear()
  const page = openDashboard()
  assert.strictEqual(page.data.moodToday, 0)
  h.tap(page, 'onSetMood', { level: 5 })
  assert.strictEqual(page.data.moodToday, 5)
  assert.strictEqual(page.data.moodLabel, '很好')
})

test('同日重复打卡为幂等更新，不产生两条', () => {
  stores.mood.clear()
  const page = openDashboard()
  h.tap(page, 'onSetMood', { level: 2 })
  h.tap(page, 'onSetMood', { level: 5 })
  assert.strictEqual(stores.mood.getAll().length, 1)
  assert.strictEqual(stores.mood.getAll()[0].level, 5)
})

test('连续天数：今天有记录时为 1', () => {
  stores.mood.clear()
  const page = openDashboard()
  h.tap(page, 'onSetMood', { level: 3 })
  const streak = page.data.stats.filter(s => s.key === 'streak')[0]
  assert.strictEqual(streak.value, '1')
})

test('连续天数：今昨都有记录时为 2', () => {
  const dateUtil = require('../shared/core/date.js')
  stores.mood.clear()
  stores.mood.add({ date: dateUtil.todayStr(), level: 4, tags: [] })
  stores.mood.add({ date: dateUtil.daysAgo(1), level: 4, tags: [] })
  const page = openDashboard()
  const streak = page.data.stats.filter(s => s.key === 'streak')[0]
  assert.strictEqual(streak.value, '2')
})

test('连续天数：断档后重新计数', () => {
  const dateUtil = require('../shared/core/date.js')
  stores.mood.clear()
  stores.mood.add({ date: dateUtil.todayStr(), level: 4, tags: [] })
  stores.mood.add({ date: dateUtil.daysAgo(2), level: 4, tags: [] })
  const page = openDashboard()
  const streak = page.data.stats.filter(s => s.key === 'streak')[0]
  assert.strictEqual(streak.value, '1')
})

// ==================== 我的页 ====================

console.log('\n\x1b[1m我的页 · 数据\x1b[0m')

test('11 个统计项', () => {
  const page = openProfile()
  assert.strictEqual(page.data.stats.length, 11)
})

test('主题三选，默认跟随系统', () => {
  const page = openProfile()
  assert.strictEqual(page.data.themes.length, 3)
  const active = page.data.themes.filter(t => t.active)
  assert.strictEqual(active.length, 1)
  assert.strictEqual(active[0].key, 'auto')
})

test('主色五选', () => {
  const page = openProfile()
  assert.strictEqual(page.data.colors.length, 5)
})

test('头像首字母大写', () => {
  const page = openProfile()
  assert.strictEqual(page.data.initial, 'D')
})

test('显示本机占用', () => {
  const page = openProfile()
  assert.strictEqual(/KB/.test(page.data.usageText), true)
})

// ==================== 我的页 · 外观 ====================

console.log('\n\x1b[1m我的页 · 外观\x1b[0m')

test('切换深色后 themeClass 变化', () => {
  const page = openProfile()
  h.tap(page, 'onSetTheme', { key: 'dark' })
  assert.strictEqual(stores.settings.get().theme, 'dark')
  assert.strictEqual(page.data.themeClass, 'theme-dark')
  assert.strictEqual(page.data.isDark, true)
})

test('切换浅色后 themeClass 还原', () => {
  const page = openProfile()
  h.tap(page, 'onSetTheme', { key: 'dark' })
  h.tap(page, 'onSetTheme', { key: 'light' })
  assert.strictEqual(page.data.themeClass, 'theme-light')
})

test('切换主色后 brand 变化', () => {
  const page = openProfile()
  h.tap(page, 'onSetPrimary', { key: 'violet' })
  assert.strictEqual(stores.settings.get().primaryColor, 'violet')
  assert.strictEqual(page.data.brand, '#4F46E5')
})

test('深色下主色取深色版色值', () => {
  const page = openProfile()
  h.tap(page, 'onSetTheme', { key: 'dark' })
  h.tap(page, 'onSetPrimary', { key: 'violet' })
  assert.strictEqual(page.data.brand, '#818CF8')
})

test('振动开关持久化', () => {
  const page = openProfile()
  h.fire(page, 'onToggleHaptics', false)
  assert.strictEqual(stores.settings.get().enableHaptics, false)
  assert.strictEqual(page.data.haptics, false)
})

// ==================== 我的页 · 资料 ====================

console.log('\n\x1b[1m我的页 · 资料\x1b[0m')

test('编辑面板带入当前资料', () => {
  const page = openProfile()
  page.onToggleEdit()
  assert.strictEqual(page.data.editing, true)
  assert.strictEqual(page.data.editForm.nickname, 'DevBencHer')
})

test('保存资料后昵称更新', () => {
  const page = openProfile()
  page.onToggleEdit()
  page.data.editForm.nickname = '新昵称'
  page.onSaveProfile()
  assert.strictEqual(stores.settings.get().profile.nickname, '新昵称')
  assert.strictEqual(page.data.editing, false)
})

test('昵称为空时拒绝保存', () => {
  const page = openProfile()
  page.onToggleEdit()
  page.data.editForm.nickname = '   '
  page.onSaveProfile()
  assert.strictEqual(stores.settings.get().profile.nickname, 'DevBencHer')
  assert.strictEqual(page.data.editing, true, '应停留在编辑态')
})

test('保存后其他字段一并持久化', () => {
  const page = openProfile()
  page.onToggleEdit()
  page.data.editForm.nickname = 'N'
  page.data.editForm.role = '架构师'
  page.data.editForm.motto = '少写多交付'
  page.onSaveProfile()
  const p = stores.settings.get().profile
  assert.strictEqual(p.role, '架构师')
  assert.strictEqual(p.motto, '少写多交付')
})

// ==================== 我的页 · 数据管理 ====================

console.log('\n\x1b[1m我的页 · 数据管理\x1b[0m')

test('导出写入剪贴板且为合法 JSON', () => {
  let copied = ''
  const restore = h.mockPlatform('setClipboardData', function (opts) {
    copied = opts.data
    return Promise.resolve()
  })
  const restoreModal = h.mockPlatform('showModal', function () {})
  try {
    const page = openProfile()
    page.onExport()
    const parsed = JSON.parse(copied)
    assert.strictEqual(typeof parsed.exportedAt, 'string')
    assert.strictEqual(Array.isArray(parsed.data.todos), true)
  } finally {
    restore()
    restoreModal()
  }
})

test('导出的备份可被恢复', () => {
  // 先触发种子写入，否则导出的是空备份
  stores.todos.getAll()
  const backup = storage.exportAll()
  const before = stores.todos.getAll().length
  assert.strictEqual(before > 0, true)

  stores.todos.clear()
  assert.strictEqual(stores.todos.getAll().length, 0)

  const result = storage.importAll(backup, 'overwrite')
  assert.strictEqual(result.ok, true)
  assert.strictEqual(stores.todos.getAll().length, before)
})

test('恢复 merge 模式不覆盖已有数据', () => {
  stores.todos.clear()
  stores.todos.add({ title: '保留我', done: false, priority: 'med' })
  const backup = JSON.stringify({ data: { todos: [{ id: 'x', title: '外部' }] } })
  storage.importAll(backup, 'merge')
  assert.strictEqual(stores.todos.getAll()[0].title, '保留我')
})

test('清空数据后重新写入种子', () => {
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true })
  })
  try {
    const page = openProfile()
    stores.todos.clear()
    assert.strictEqual(stores.todos.getAll().length, 0)
    page.onClearData()
    return Promise.resolve().then(function () {
      assert.strictEqual(stores.todos.getAll().length > 0, true, '应重新写入种子数据')
    })
  } finally {
    restore()
  }
})

test('取消清空时数据保留', () => {
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: false })
  })
  try {
    const page = openProfile()
    const before = stores.todos.getAll().length
    page.onClearData()
    assert.strictEqual(stores.todos.getAll().length, before)
  } finally {
    restore()
  }
})

// ==================== 结果 ====================

runner.report()
