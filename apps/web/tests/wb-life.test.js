/**
 * life.test.js —— 生活模块测试
 *
 * 覆盖：聚合页摘要、读书进度与自动完成、健身周汇总、心情打卡与 14 天色带、
 * 日程排序与完成切换。
 *
 * 用法：node tests/life.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')

stub.installLocalStorage()

const h = require('./wb-harness.js')
const stores = require('../shared/workbench/stores.js')
const dateUtil = require('../shared/core/date.js')

let pass = 0
let fail = 0
const failures = []

function test (name, fn) {
  try {
    stub.reset()
  if (global.localStorage && typeof global.localStorage.clear === "function") global.localStorage.clear()
    fn()
    pass++
    console.log('  \x1b[32mPASS\x1b[0m ' + name)
  } catch (e) {
    fail++
    failures.push({ name: name, error: e.message })
    console.log('  \x1b[31mFAIL\x1b[0m ' + name + ' -> ' + e.message)
  }
}

function open (route, query) {
  const page = h.createPage(route)
  if (page.onLoad) page.onLoad(query || {})
  if (page.onShow) page.onShow()
  return page
}

// ==================== 聚合页 ====================

console.log('\n\x1b[1m生活聚合页\x1b[0m')

test('4 个模块入口', () => {
  const page = open('pages/life/index')
  assert.strictEqual(page.data.entries.length, 4)
  assert.deepStrictEqual(
    page.data.entries.map(e => e.key),
    ['reading', 'fitness', 'mood', 'schedule']
  )
})

test('读书摘要显示在读与总数', () => {
  stores.reading.clear()
  stores.reading.add({ title: 'A', totalPages: 100, currentPage: 10, status: 'reading' })
  stores.reading.add({ title: 'B', totalPages: 100, currentPage: 0, status: 'wishlist' })
  const page = open('pages/life/index')
  const reading = page.data.entries.filter(e => e.key === 'reading')[0]
  assert.strictEqual(reading.count, 1)
  assert.strictEqual(reading.desc.indexOf('在读 1 本') >= 0, true)
})

test('健身摘要只统计本周', () => {
  stores.fitness.clear()
  stores.fitness.add({ date: dateUtil.todayStr(), type: 'cardio', name: '跑', durationMin: 30 })
  stores.fitness.add({ date: '2020-01-01', type: 'cardio', name: '旧', durationMin: 60 })
  const page = open('pages/life/index')
  const fitness = page.data.entries.filter(e => e.key === 'fitness')[0]
  assert.strictEqual(fitness.count, 1)
  assert.strictEqual(fitness.desc.indexOf('30 分钟') >= 0, true)
})

test('日程摘要只统计今天', () => {
  stores.schedule.clear()
  const today = dateUtil.todayStr()
  stores.schedule.add({ title: '今天的', startAt: today + 'T10:00:00', done: false })
  stores.schedule.add({ title: '明天的', startAt: '2099-01-01T10:00:00', done: false })
  const page = open('pages/life/index')
  const schedule = page.data.entries.filter(e => e.key === 'schedule')[0]
  assert.strictEqual(schedule.count, 1)
})

// ==================== 读书 ====================

console.log('\n\x1b[1m读书\x1b[0m')

test('排序：在读 → 想读 → 搁置 → 已读', () => {
  stores.reading.clear()
  stores.reading.add({ title: '已读', totalPages: 10, currentPage: 10, status: 'finished' })
  stores.reading.add({ title: '在读', totalPages: 10, currentPage: 5, status: 'reading' })
  stores.reading.add({ title: '想读', totalPages: 10, currentPage: 0, status: 'wishlist' })
  stores.reading.add({ title: '搁置', totalPages: 10, currentPage: 1, status: 'paused' })
  const page = open('pages/life/reading/index')
  assert.deepStrictEqual(
    page.data.items.map(i => i.title),
    ['在读', '想读', '搁置', '已读']
  )
})

test('进度百分比按页数计算', () => {
  stores.reading.clear()
  stores.reading.add({ title: 'A', totalPages: 200, currentPage: 50, status: 'reading' })
  const page = open('pages/life/reading/index')
  assert.strictEqual(page.data.items[0].percent, 25)
  assert.strictEqual(page.data.items[0].progressText, '50 / 200 页')
})

test('未设总页数时进度为 0 且提示', () => {
  stores.reading.clear()
  stores.reading.add({ title: 'A', totalPages: 0, currentPage: 0, status: 'reading' })
  const page = open('pages/life/reading/index')
  assert.strictEqual(page.data.items[0].percent, 0)
  assert.strictEqual(page.data.items[0].progressText, '未设总页数')
})

test('新建书默认在读状态', () => {
  const page = open('pages/life/reading/index')
  page.onCreate()
  assert.strictEqual(page.data.formStatus, 'reading')
  assert.strictEqual(page.data.isEdit, false)
})

test('保存新书写入 store', () => {
  stores.reading.clear()
  const page = open('pages/life/reading/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '新书' } })
  page.onFormTotal({ detail: { value: '300' } })
  page.onFormCurrent({ detail: { value: '30' } })
  page.onSave()
  const all = stores.reading.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].totalPages, 300)
  assert.strictEqual(all[0].currentPage, 30)
})

test('书名必填', () => {
  stores.reading.clear()
  const page = open('pages/life/reading/index')
  page.onCreate()
  page.onSave()
  assert.strictEqual(stores.reading.getAll().length, 0)
})

test('当前页超过总页数被夹取', () => {
  stores.reading.clear()
  const page = open('pages/life/reading/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '书' } })
  page.onFormTotal({ detail: { value: '100' } })
  page.onFormCurrent({ detail: { value: '999' } })
  page.onSave()
  assert.strictEqual(stores.reading.getAll()[0].currentPage, 100)
})

test('读满页数自动转已读', () => {
  stores.reading.clear()
  const page = open('pages/life/reading/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '书' } })
  page.onFormTotal({ detail: { value: '100' } })
  page.onFormCurrent({ detail: { value: '100' } })
  page.onSave()
  const book = stores.reading.getAll()[0]
  assert.strictEqual(book.status, 'finished')
  assert.strictEqual(typeof book.finishedAt, 'string')
})

test('编辑载入已有书', () => {
  stores.reading.clear()
  const b = stores.reading.add({ title: '旧', author: '作者', totalPages: 200, currentPage: 20, status: 'reading', rating: 4 })
  const page = open('pages/life/reading/index')
  page.openPanel(stores.reading.find(b.id))
  assert.strictEqual(page.data.isEdit, true)
  assert.strictEqual(page.data.formTitle, '旧')
  assert.strictEqual(page.data.formAuthor, '作者')
  assert.strictEqual(page.data.formRating, 4)
})

test('编辑保存更新原书', () => {
  stores.reading.clear()
  const b = stores.reading.add({ title: '旧', totalPages: 100, currentPage: 10, status: 'reading' })
  const page = open('pages/life/reading/index')
  page.openPanel(stores.reading.find(b.id))
  page.onFormTitle({ detail: { value: '新' } })
  page.onSave()
  assert.strictEqual(stores.reading.getAll().length, 1)
  assert.strictEqual(stores.reading.find(b.id).title, '新')
})

test('关闭面板不保存', () => {
  stores.reading.clear()
  const page = open('pages/life/reading/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '不保存' } })
  page.onClosePanel()
  assert.strictEqual(stores.reading.getAll().length, 0)
})

// ==================== 健身 ====================

console.log('\n\x1b[1m健身\x1b[0m')

test('周汇总统计次数时长与消耗', () => {
  stores.fitness.clear()
  const today = dateUtil.todayStr()
  stores.fitness.add({ date: today, type: 'cardio', name: '跑', durationMin: 30, calories: 200 })
  stores.fitness.add({ date: today, type: 'strength', name: '举铁', durationMin: 45, calories: 300 })
  const page = open('pages/life/fitness/index')
  assert.strictEqual(page.data.weekCount, 2)
  assert.strictEqual(page.data.weekMinutes, 75)
  assert.strictEqual(page.data.weekCalories, 500)
})

test('非本周记录不计入汇总', () => {
  stores.fitness.clear()
  stores.fitness.add({ date: '2020-01-01', type: 'cardio', name: '旧', durationMin: 60 })
  const page = open('pages/life/fitness/index')
  assert.strictEqual(page.data.weekCount, 0)
})

test('按日期倒序排列', () => {
  stores.fitness.clear()
  stores.fitness.add({ date: '2026-01-01', type: 'cardio', name: '旧', durationMin: 10 })
  stores.fitness.add({ date: '2026-12-01', type: 'cardio', name: '新', durationMin: 10 })
  const page = open('pages/life/fitness/index')
  assert.strictEqual(page.data.items[0].name, '新')
})

test('时长格式化为人类可读', () => {
  stores.fitness.clear()
  stores.fitness.add({ date: dateUtil.todayStr(), type: 'cardio', name: '跑', durationMin: 80 })
  const page = open('pages/life/fitness/index')
  assert.strictEqual(page.data.items[0].duration, '1 小时 20 分钟')
})

test('保存新记录写入 store', () => {
  stores.fitness.clear()
  const page = open('pages/life/fitness/index')
  page.onCreate()
  assert.strictEqual(page.data.formDate, dateUtil.todayStr(), '默认今天')
  page.onFormName({ detail: { value: '游泳' } })
  page.onFormDuration({ detail: { value: '40' } })
  page.onFormCalories({ detail: { value: '350' } })
  page.onSave()
  const all = stores.fitness.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].durationMin, 40)
})

test('名称必填', () => {
  stores.fitness.clear()
  const page = open('pages/life/fitness/index')
  page.onCreate()
  page.onFormDuration({ detail: { value: '30' } })
  page.onSave()
  assert.strictEqual(stores.fitness.getAll().length, 0)
})

test('时长必须大于 0', () => {
  stores.fitness.clear()
  const page = open('pages/life/fitness/index')
  page.onCreate()
  page.onFormName({ detail: { value: '跑' } })
  page.onFormDuration({ detail: { value: '0' } })
  page.onSave()
  assert.strictEqual(stores.fitness.getAll().length, 0)
})

// ==================== 心情 ====================

console.log('\n\x1b[1m心情\x1b[0m')

test('5 个等级圆点', () => {
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.levels.length, 5)
})

test('色带固定 14 天且末位是今天', () => {
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.strip.length, 14)
  assert.strictEqual(page.data.strip[13].isToday, true)
  assert.strictEqual(page.data.strip[13].date, dateUtil.todayStr())
})

test('点圆点记录今天', () => {
  stores.mood.clear()
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.todayLevel, 0)
  h.tap(page, 'onSetToday', { level: 5 })
  assert.strictEqual(page.data.todayLevel, 5)
  assert.strictEqual(page.data.todayLabel, '很好')
})

test('同日重复记录为幂等更新', () => {
  stores.mood.clear()
  const page = open('pages/life/mood/index')
  h.tap(page, 'onSetToday', { level: 2 })
  h.tap(page, 'onSetToday', { level: 4 })
  assert.strictEqual(stores.mood.getAll().length, 1)
  assert.strictEqual(stores.mood.getAll()[0].level, 4)
})

test('记录后色带对应格子上色', () => {
  stores.mood.clear()
  const page = open('pages/life/mood/index')
  h.tap(page, 'onSetToday', { level: 3 })
  assert.strictEqual(page.data.strip[13].level, 3)
  assert.strictEqual(/^#/.test(page.data.strip[13].color), true)
})

test('连续天数：今昨都有为 2', () => {
  stores.mood.clear()
  stores.mood.add({ date: dateUtil.todayStr(), level: 4, tags: [] })
  stores.mood.add({ date: dateUtil.daysAgo(1), level: 4, tags: [] })
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.streak, 2)
})

test('连续天数：断档只算到断点', () => {
  stores.mood.clear()
  stores.mood.add({ date: dateUtil.todayStr(), level: 4, tags: [] })
  stores.mood.add({ date: dateUtil.daysAgo(2), level: 4, tags: [] })
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.streak, 1)
})

test('历史列表按日期倒序', () => {
  stores.mood.clear()
  stores.mood.add({ date: '2026-01-01', level: 1, tags: [] })
  stores.mood.add({ date: '2026-12-01', level: 5, tags: [] })
  const page = open('pages/life/mood/index')
  assert.strictEqual(page.data.items[0].date, '2026-12-01')
})

test('编辑面板载入当天记录', () => {
  stores.mood.clear()
  const e = stores.mood.add({ date: dateUtil.todayStr(), level: 2, tags: ['work'], note: '累' })
  const page = open('pages/life/mood/index')
  page.openPanel(stores.mood.find(e.id))
  assert.strictEqual(page.data.formLevel, 2)
  assert.strictEqual(page.data.formTags, 'work')
  assert.strictEqual(page.data.formNote, '累')
})

test('保存面板修改备注与标签', () => {
  stores.mood.clear()
  const e = stores.mood.add({ date: dateUtil.todayStr(), level: 2, tags: [], note: '' })
  const page = open('pages/life/mood/index')
  page.openPanel(stores.mood.find(e.id))
  page.onFormLevel({ currentTarget: { dataset: { level: 5 } } })
  page.onFormTags({ detail: { value: 'work, code' } })
  page.onFormNote({ detail: { value: '很好' } })
  page.onSave()
  const saved = stores.mood.find(e.id)
  assert.strictEqual(saved.level, 5)
  assert.deepStrictEqual(saved.tags, ['work', 'code'])
  assert.strictEqual(saved.note, '很好')
})

// ==================== 日程 ====================

console.log('\n\x1b[1m日程\x1b[0m')

test('未完成在前，按时间升序', () => {
  stores.schedule.clear()
  stores.schedule.add({ title: '已完成', startAt: '2026-10-22T09:00:00', done: true })
  stores.schedule.add({ title: '晚', startAt: '2026-10-22T18:00:00', done: false })
  stores.schedule.add({ title: '早', startAt: '2026-10-22T08:00:00', done: false })
  const page = open('pages/life/schedule/index')
  assert.deepStrictEqual(
    page.data.items.map(i => i.title),
    ['早', '晚', '已完成']
  )
})

test('时间与日期标签正确', () => {
  stores.schedule.clear()
  stores.schedule.add({ title: '会议', startAt: dateUtil.todayStr() + 'T14:30:00', done: false })
  const page = open('pages/life/schedule/index')
  assert.strictEqual(page.data.items[0].timeLabel, '14:30')
  assert.strictEqual(page.data.items[0].dateLabel, '今天')
})

test('切换完成状态', () => {
  stores.schedule.clear()
  const e = stores.schedule.add({ title: 'X', startAt: '2026-10-22T10:00:00', done: false })
  const page = open('pages/life/schedule/index')
  h.tap(page, 'onToggleDone', { id: e.id })
  assert.strictEqual(stores.schedule.find(e.id).done, true)
  h.tap(page, 'onToggleDone', { id: e.id })
  assert.strictEqual(stores.schedule.find(e.id).done, false)
})

test('保存新日程拼接 ISO 时间', () => {
  stores.schedule.clear()
  const page = open('pages/life/schedule/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '周会' } })
  page.onFormDate({ detail: { value: '2026-11-01' } })
  page.onFormTime({ detail: { value: '09:30' } })
  page.onFormLocation({ detail: { value: '线上' } })
  page.onSave()
  const all = stores.schedule.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].startAt, '2026-11-01T09:30:00')
  assert.strictEqual(all[0].done, false)
})

test('标题必填', () => {
  stores.schedule.clear()
  const page = open('pages/life/schedule/index')
  page.onCreate()
  page.onSave()
  assert.strictEqual(stores.schedule.getAll().length, 0)
})

test('编辑载入已有日程的时间', () => {
  stores.schedule.clear()
  const e = stores.schedule.add({ title: 'T', startAt: '2026-12-25T20:15:00', done: false, location: '家' })
  const page = open('pages/life/schedule/index')
  page.openPanel(stores.schedule.find(e.id))
  assert.strictEqual(page.data.formDate, '2026-12-25')
  assert.strictEqual(page.data.formTime, '20:15')
  assert.strictEqual(page.data.formLocation, '家')
})

test('提醒开关可切换', () => {
  const page = open('pages/life/schedule/index')
  page.onCreate()
  assert.strictEqual(page.data.formReminder, true)
  page.onToggleReminder()
  assert.strictEqual(page.data.formReminder, false)
})

test('今天未完成数量正确', () => {
  stores.schedule.clear()
  const today = dateUtil.todayStr()
  stores.schedule.add({ title: 'a', startAt: today + 'T09:00:00', done: false })
  stores.schedule.add({ title: 'b', startAt: today + 'T10:00:00', done: true })
  const page = open('pages/life/schedule/index')
  assert.strictEqual(page.data.todayCount, 1)
  assert.strictEqual(page.data.openCount, 1)
})

// ==================== 结果 ====================

console.log('\n' + '='.repeat(52))
console.log('通过 ' + pass + ' 项，失败 ' + fail + ' 项')
if (fail > 0) {
  console.log('\n失败明细：')
  failures.forEach(f => console.log('  - ' + f.name + ': ' + f.error))
  process.exit(1)
}
console.log('全部通过')
