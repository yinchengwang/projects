/**
 * p5.test.js —— 项目集 / 闪念 / 捕获浮层测试
 *
 * 用法：node tests/p5.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')

stub.installLocalStorage()

const h = require('./wb-harness.js')
const stores = require('../shared/workbench/stores.js')
const dateUtil = require('../shared/core/date.js')

const runner = require('./wb-runner.js').createRunner()
const test = function (name, fn) {
  runner.test(name, fn, function () {
    stub.reset()
    if (global.localStorage && typeof global.localStorage.clear === "function") global.localStorage.clear()
  })
}

function open (route, query) {
  const page = h.createPage(route)
  if (page.onLoad) page.onLoad(query || {})
  if (page.onShow) page.onShow()
  return page
}

// ==================== 项目集 ====================

console.log('\n\x1b[1m项目集\x1b[0m')

test('排序：进行中 → 开发中 → 已归档', () => {
  stores.projects.clear()
  stores.projects.add({ name: '归档', status: 'archived', stars: 0 })
  stores.projects.add({ name: '开发中', status: 'wip', stars: 0 })
  stores.projects.add({ name: '进行中', status: 'active', stars: 0 })
  const page = open('pages/projects/index')
  assert.deepStrictEqual(
    page.data.items.map(i => i.name),
    ['进行中', '开发中', '归档']
  )
})

test('技术栈最多展示 4 个', () => {
  stores.projects.clear()
  stores.projects.add({ name: 'P', status: 'active', techStack: ['a', 'b', 'c', 'd', 'e'], stars: 0 })
  const page = open('pages/projects/index')
  assert.strictEqual(page.data.items[0].techStack.length, 4)
})

test('亮点最多展示 2 条', () => {
  stores.projects.clear()
  stores.projects.add({ name: 'P', status: 'active', highlights: ['1', '2', '3'], stars: 0 })
  const page = open('pages/projects/index')
  assert.strictEqual(page.data.items[0].highlights.length, 2)
})

test('保存新项目写入 store', () => {
  stores.projects.clear()
  const page = open('pages/projects/index')
  page.onCreate()
  page.onFormName({ detail: { value: '新项目' } })
  page.onFormDesc({ detail: { value: '做点东西' } })
  page.onSave()
  const all = stores.projects.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].name, '新项目')
})

test('技术栈按逗号与顿号拆分', () => {
  stores.projects.clear()
  const page = open('pages/projects/index')
  page.onCreate()
  page.onFormName({ detail: { value: 'P' } })
  page.onFormStack({ detail: { value: 'TypeScript, Vue、Node' } })
  page.onSave()
  assert.deepStrictEqual(stores.projects.getAll()[0].techStack, ['TypeScript', 'Vue', 'Node'])
})

test('亮点按换行拆分并过滤空行', () => {
  stores.projects.clear()
  const page = open('pages/projects/index')
  page.onCreate()
  page.onFormName({ detail: { value: 'P' } })
  page.onFormHighlights({ detail: { value: '离线优先\n\n零依赖\n  \n' } })
  page.onSave()
  assert.deepStrictEqual(stores.projects.getAll()[0].highlights, ['离线优先', '零依赖'])
})

test('项目名必填', () => {
  stores.projects.clear()
  const page = open('pages/projects/index')
  page.onCreate()
  page.onSave()
  assert.strictEqual(stores.projects.getAll().length, 0)
  assert.strictEqual(page.data.editing, true, '应停留在面板')
})

test('编辑载入已有项目', () => {
  stores.projects.clear()
  const p = stores.projects.add({ name: '旧', description: 'D', status: 'wip', stars: 4, techStack: ['x'] })
  const page = open('pages/projects/index')
  page.openPanel(stores.projects.find(p.id))
  assert.strictEqual(page.data.isEdit, true)
  assert.strictEqual(page.data.formName, '旧')
  assert.strictEqual(page.data.formStatus, 'wip')
  assert.strictEqual(page.data.formStars, 4)
  assert.strictEqual(page.data.formStack, 'x')
})

test('编辑保存更新原项目', () => {
  stores.projects.clear()
  const p = stores.projects.add({ name: '旧', status: 'active', stars: 0 })
  const page = open('pages/projects/index')
  page.openPanel(stores.projects.find(p.id))
  page.onFormName({ detail: { value: '新' } })
  page.onSave()
  assert.strictEqual(stores.projects.getAll().length, 1)
  assert.strictEqual(stores.projects.find(p.id).name, '新')
})

test('复制链接写入剪贴板', () => {
  let copied = ''
  const restore = h.mockPlatform('setClipboardData', function (opts) {
    copied = opts.data
    return Promise.resolve()
  })
  try {
    const page = open('pages/projects/index')
    h.tap(page, 'onCopyLink', { url: 'https://example.com' })
    assert.strictEqual(copied, 'https://example.com')
  } finally {
    restore()
  }
})

// ==================== 闪念 ====================

console.log('\n\x1b[1m闪念\x1b[0m')

test('快速输入写入 store 并清空输入框', () => {
  stores.insights.clear()
  const page = open('pages/insights/index')
  page.onQuickInput({ detail: { value: '一个想法' } })
  page.onQuickAdd()
  const all = stores.insights.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].content, '一个想法')
  assert.strictEqual(page.data.quickText, '')
})

test('空输入不写入', () => {
  stores.insights.clear()
  const page = open('pages/insights/index')
  page.onQuickInput({ detail: { value: '   ' } })
  page.onQuickAdd()
  assert.strictEqual(stores.insights.getAll().length, 0)
})

test('置顶排在最前', () => {
  stores.insights.clear()
  stores.insights.add({ content: '普通', tags: [], pinned: false, archived: false })
  stores.insights.add({ content: '置顶', tags: [], pinned: true, archived: false })
  const page = open('pages/insights/index')
  assert.strictEqual(page.data.items[0].content, '置顶')
})

test('归档排在未归档之后', () => {
  stores.insights.clear()
  stores.insights.add({ content: '归档', tags: [], pinned: false, archived: true })
  stores.insights.add({ content: '正常', tags: [], pinned: false, archived: false })
  const page = open('pages/insights/index')
  assert.strictEqual(page.data.items[0].content, '正常')
  assert.strictEqual(page.data.items[1].content, '归档')
})

test('长按置顶切换', () => {
  stores.insights.clear()
  const i = stores.insights.add({ content: 'X', tags: [], pinned: false, archived: false })
  const restore = h.mockPlatform('showActionSheet', function () {
    return Promise.resolve({ tapIndex: 0 })
  })
  try {
    const page = open('pages/insights/index')
    return h.tap(page, 'onLongPress', { id: i.id }).then(function () {
      assert.strictEqual(stores.insights.find(i.id).pinned, true)
    })
  } finally {
    restore()
  }
})

test('长按归档切换', () => {
  stores.insights.clear()
  const i = stores.insights.add({ content: 'X', tags: [], pinned: false, archived: false })
  const restore = h.mockPlatform('showActionSheet', function () {
    return Promise.resolve({ tapIndex: 1 })
  })
  try {
    const page = open('pages/insights/index')
    return h.tap(page, 'onLongPress', { id: i.id }).then(function () {
      assert.strictEqual(stores.insights.find(i.id).archived, true)
    })
  } finally {
    restore()
  }
})

test('长按删除需二次确认', () => {
  stores.insights.clear()
  const i = stores.insights.add({ content: 'X', tags: [], pinned: false, archived: false })
  const restoreSheet = h.mockPlatform('showActionSheet', function () {
    return Promise.resolve({ tapIndex: 2 })
  })
  const restoreModal = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true })
  })
  const page = open('pages/insights/index')
  return h.tap(page, 'onLongPress', { id: i.id }).then(function () {
    assert.strictEqual(stores.insights.getAll().length, 0)
  }).then(function () {
    restoreSheet()
    restoreModal()
  }, function (err) {
    restoreSheet()
    restoreModal()
    throw err
  })
})

test('编辑面板保存内容与标签', () => {
  stores.insights.clear()
  const i = stores.insights.add({ content: '旧', tags: [], pinned: false, archived: false })
  const page = open('pages/insights/index')
  h.tap(page, 'onOpen', { id: i.id })
  assert.strictEqual(page.data.formContent, '旧')
  page.onFormContent({ detail: { value: '新内容' } })
  page.onFormTags({ detail: { value: 'arch, tip' } })
  page.onSave()
  const saved = stores.insights.find(i.id)
  assert.strictEqual(saved.content, '新内容')
  assert.deepStrictEqual(saved.tags, ['arch', 'tip'])
})

test('内容为空拒绝保存', () => {
  stores.insights.clear()
  const i = stores.insights.add({ content: '旧', tags: [], pinned: false, archived: false })
  const page = open('pages/insights/index')
  h.tap(page, 'onOpen', { id: i.id })
  page.onFormContent({ detail: { value: '  ' } })
  page.onSave()
  assert.strictEqual(stores.insights.find(i.id).content, '旧')
})

// ==================== 捕获浮层 ====================

console.log('\n\x1b[1m捕获浮层\x1b[0m')

test('选待办写入 todos 并派发事件', () => {
  stores.todos.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '买咖啡' })
  })
  try {
    const sheet = h.createComponent('components/capture-sheet/index')
    return h.tap(sheet, 'onPick', { type: 'todo' }).then(function () {
      const todos = stores.todos.getAll()
      assert.strictEqual(todos.length, 1)
      assert.strictEqual(todos[0].title, '买咖啡')
      assert.strictEqual(todos[0].done, false)
      assert.strictEqual(todos[0].dueDate, dateUtil.todayStr())

      const created = h.lastEvent(sheet, 'created')
      assert.strictEqual(created.detail.type, 'todo')
      assert.ok(h.lastEvent(sheet, 'close'), '应派发 close')
    })
  } finally {
    restore()
  }
})

test('选闪念写入 insights', () => {
  stores.insights.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '灵感' })
  })
  try {
    const sheet = h.createComponent('components/capture-sheet/index')
    return h.tap(sheet, 'onPick', { type: 'insight' }).then(function () {
      const all = stores.insights.getAll()
      assert.strictEqual(all.length, 1)
      assert.strictEqual(all[0].content, '灵感')
      assert.strictEqual(h.lastEvent(sheet, 'created').detail.type, 'insight')
    })
  } finally {
    restore()
  }
})

test('取消输入不写入', () => {
  stores.todos.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: false })
  })
  try {
    const sheet = h.createComponent('components/capture-sheet/index')
    h.tap(sheet, 'onPick', { type: 'todo' })
    assert.strictEqual(stores.todos.getAll().length, 0)
  } finally {
    restore()
  }
})

test('空内容不写入', () => {
  stores.insights.clear()
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '  ' })
  })
  try {
    const sheet = h.createComponent('components/capture-sheet/index')
    h.tap(sheet, 'onPick', { type: 'insight' })
    assert.strictEqual(stores.insights.getAll().length, 0)
  } finally {
    restore()
  }
})

test('选笔记跳转完整编辑器而非直接创建', () => {
  let navigated = ''
  const restore = h.mockPlatform('navigateTo', function (url) {
    navigated = url
  })
  try {
    stores.techNotes.clear()
    const sheet = h.createComponent('components/capture-sheet/index')
    return h.tap(sheet, 'onPick', { type: 'note' }).then(function () {
      assert.strictEqual(navigated, '/workbench/pages/tech/notes/detail')
      assert.strictEqual(stores.techNotes.getAll().length, 0, '不应直接创建空笔记')
    })
  } finally {
    restore()
  }
})

test('关闭派发 close 事件', () => {
  const sheet = h.createComponent('components/capture-sheet/index')
  sheet.onClose()
  assert.ok(h.lastEvent(sheet, 'close'))
})

test('点击面板内部不关闭', () => {
  const sheet = h.createComponent('components/capture-sheet/index')
  sheet.noop()
  assert.strictEqual(sheet.__events.length, 0)
})

// ==================== 页面接入 ====================

console.log('\n\x1b[1m捕获浮层接入\x1b[0m')

test('今日页可唤起并刷新', () => {
  const page = open('pages/dashboard/index')
  assert.strictEqual(page.data.captureVisible, false)
  page.onOpenCapture()
  assert.strictEqual(page.data.captureVisible, true)
  page.onCloseCapture()
  assert.strictEqual(page.data.captureVisible, false)
})

test('技术页可唤起', () => {
  const page = open('pages/tech/index')
  page.onOpenCapture()
  assert.strictEqual(page.data.captureVisible, true)
})

test('生活页可唤起', () => {
  const page = open('pages/life/index')
  page.onOpenCapture()
  assert.strictEqual(page.data.captureVisible, true)
})

test('捕获后今日页待办数更新', () => {
  stores.todos.clear()
  const page = open('pages/dashboard/index')
  const restore = h.mockPlatform('showModal', function () {
    return Promise.resolve({ confirm: true, content: '新任务' })
  })
  try {
    page.onCaptured()
    assert.strictEqual(page.data.todoOpenCount, 0)
    stores.todos.add({ title: '新任务', done: false, priority: 'med' })
    page.onCaptured()
    assert.strictEqual(page.data.todoOpenCount, 1)
  } finally {
    restore()
  }
})

// ==================== 结果 ====================

runner.report()
