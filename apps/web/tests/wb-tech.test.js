/**
 * tech.test.js —— 技术栈模块测试
 *
 * 覆盖：聚合页计数、笔记列表排序与预览、笔记增删改与 Markdown 渲染、
 * 片段列表与详情、面试题库与复习计数、资源收藏面板。
 *
 * 用法：node tests/tech.test.js
 */

const assert = require('assert')
const stub = require('./wb-stub.js')

stub.installLocalStorage()

const h = require('./wb-harness.js')
const stores = require('../shared/workbench/stores.js')
const md = require('../shared/core/md.js')

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

/** 打开页面（onLoad + onShow） */
function open (route, query) {
  const page = h.createPage(route)
  if (page.onLoad) page.onLoad(query || {})
  if (page.onShow) page.onShow()
  return page
}

// ==================== Markdown ====================

console.log('\n\x1b[1mMarkdown 渲染\x1b[0m')

test('标题渲染为 h1', () => {
  const html = md.render('# 标题', false)
  assert.strictEqual(html.indexOf('<h1>标题</h1>') >= 0, true)
})

test('代码块改写为内联样式（rich-text 需要）', () => {
  const html = md.render('```js\nconst a = 1\n```', false)
  assert.strictEqual(html.indexOf('class="md-code') < 0, true, '不应残留 class')
  assert.strictEqual(html.indexOf('style=') >= 0, true)
})

test('深色与浅色代码底色不同', () => {
  const light = md.render('```\nx\n```', false)
  const dark = md.render('```\nx\n```', true)
  assert.notStrictEqual(light, dark)
})

test('行内码改写为内联样式', () => {
  const html = md.render('用 `code` 表示', false)
  assert.strictEqual(html.indexOf('class="md-inline-code"') < 0, true)
})

test('纯文本预览去掉 Markdown 标记', () => {
  const p = md.preview('## 标题\n\n**粗体** 和 `代码`', 60)
  assert.strictEqual(p.indexOf('#'), -1)
  assert.strictEqual(p.indexOf('**'), -1)
  assert.strictEqual(p.indexOf('粗体') >= 0, true)
})

test('预览超长截断并加省略号', () => {
  const p = md.preview('x'.repeat(100), 20)
  assert.strictEqual(p.length, 21)
  assert.strictEqual(p.slice(-1), '…')
})

test('预览把代码块替换为占位', () => {
  const p = md.preview('前\n```\ncode\n```\n后', 60)
  assert.strictEqual(p.indexOf('[代码]') >= 0, true)
})

test('空内容渲染为空串', () => {
  assert.strictEqual(md.render('', false), '')
  assert.strictEqual(md.preview('', 60), '')
})

// ==================== 聚合页 ====================

console.log('\n\x1b[1m技术栈聚合页\x1b[0m')

test('4 个入口', () => {
  const page = open('pages/tech/index')
  assert.strictEqual(page.data.entries.length, 4)
})

test('计数来自各域实际条数', () => {
  const page = open('pages/tech/index')
  const notes = page.data.entries.filter(e => e.key === 'notes')[0]
  assert.strictEqual(notes.count, stores.techNotes.getAll().length)
})

test('总数等于四项之和', () => {
  const page = open('pages/tech/index')
  const sum = page.data.entries.reduce((a, e) => a + e.count, 0)
  assert.strictEqual(page.data.total, sum)
})

// ==================== 笔记列表 ====================

console.log('\n\x1b[1m笔记列表\x1b[0m')

test('置顶笔记排在最前', () => {
  stores.techNotes.clear()
  stores.techNotes.add({ title: '普通', content: 'x', category: 'frontend', tags: [], pinned: false })
  stores.techNotes.add({ title: '置顶', content: 'x', category: 'frontend', tags: [], pinned: true })
  const page = open('pages/tech/notes/index')
  assert.strictEqual(page.data.items[0].title, '置顶')
})

test('列表带分类色与相对时间', () => {
  stores.techNotes.clear()
  stores.techNotes.add({ title: 'n', content: 'x', category: 'database', tags: ['sql'], pinned: false })
  const page = open('pages/tech/notes/index')
  const item = page.data.items[0]
  assert.strictEqual(item.categoryLabel, '数据库')
  assert.strictEqual(/^#/.test(item.color), true)
  assert.strictEqual(item.time, '刚刚')
})

test('列表摘要取正文前 60 字', () => {
  stores.techNotes.clear()
  stores.techNotes.add({ title: 'n', content: '正文内容', category: 'other', tags: [], pinned: false })
  const page = open('pages/tech/notes/index')
  assert.strictEqual(page.data.items[0].preview, '正文内容')
})

test('标签最多显示 3 个', () => {
  stores.techNotes.clear()
  stores.techNotes.add({ title: 'n', content: 'x', category: 'other', tags: ['a', 'b', 'c', 'd'], pinned: false })
  const page = open('pages/tech/notes/index')
  assert.strictEqual(page.data.items[0].tags.length, 3)
})

test('空列表时 count 为 0', () => {
  stores.techNotes.clear()
  const page = open('pages/tech/notes/index')
  assert.strictEqual(page.data.count, 0)
})

// ==================== 笔记详情 ====================

console.log('\n\x1b[1m笔记详情\x1b[0m')

test('新建模式带入默认内容', () => {
  const page = open('pages/tech/notes/detail', {})
  assert.strictEqual(page.data.isEdit, false)
  assert.strictEqual(page.data.content.length > 0, true)
})

test('编辑模式载入已有笔记', () => {
  stores.techNotes.clear()
  const note = stores.techNotes.add({ title: '原标题', content: '原内容', category: 'backend', tags: ['x'], pinned: false })
  const page = open('pages/tech/notes/detail', { id: note.id })
  assert.strictEqual(page.data.isEdit, true)
  assert.strictEqual(page.data.formTitle, '原标题')
  assert.strictEqual(page.data.category, 'backend')
  assert.strictEqual(page.data.tagsText, 'x')
})

test('保存新笔记写入 store', () => {
  stores.techNotes.clear()
  const page = open('pages/tech/notes/detail', {})
  page.onTitle({ detail: { value: '新笔记' } })
  page.onContent({ detail: { value: '内容' } })
  page.onSave()
  const all = stores.techNotes.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].title, '新笔记')
})

test('标题为空拒绝保存', () => {
  stores.techNotes.clear()
  const page = open('pages/tech/notes/detail', {})
  page.onContent({ detail: { value: '内容' } })
  page.onSave()
  assert.strictEqual(stores.techNotes.getAll().length, 0)
})

test('内容为空拒绝保存', () => {
  stores.techNotes.clear()
  const page = open('pages/tech/notes/detail', {})
  page.onTitle({ detail: { value: '标题' } })
  page.onContent({ detail: { value: '   ' } })
  page.onSave()
  assert.strictEqual(stores.techNotes.getAll().length, 0)
})

test('标签按逗号与空格分隔解析', () => {
  const page = open('pages/tech/notes/detail', {})
  const tags = page.parseTags(' react, hooks  #ts')
  assert.deepStrictEqual(tags, ['react', 'hooks', 'ts'])
})

test('标签解析去掉重复分隔符', () => {
  const page = open('pages/tech/notes/detail', {})
  assert.deepStrictEqual(page.parseTags('a,,b  c'), ['a', 'b', 'c'])
})

test('编辑保存更新原笔记而非新增', () => {
  stores.techNotes.clear()
  const note = stores.techNotes.add({ title: '旧', content: 'x', category: 'other', tags: [], pinned: false })
  const page = open('pages/tech/notes/detail', { id: note.id })
  page.onTitle({ detail: { value: '新标题' } })
  page.onSave()
  assert.strictEqual(stores.techNotes.getAll().length, 1)
  assert.strictEqual(stores.techNotes.find(note.id).title, '新标题')
})

test('切换预览生成 HTML', () => {
  const page = open('pages/tech/notes/detail', {})
  page.onContent({ detail: { value: '# 标题' } })
  page.onTogglePreview()
  assert.strictEqual(page.data.preview, true)
  assert.strictEqual(page.data.html.indexOf('<h1>') >= 0, true)
})

test('预览状态下改内容同步刷新 HTML', () => {
  const page = open('pages/tech/notes/detail', {})
  page.onTogglePreview()
  page.onContent({ detail: { value: '## 二级' } })
  assert.strictEqual(page.data.html.indexOf('<h2>') >= 0, true)
})

// ==================== 代码片段 ====================

console.log('\n\x1b[1m代码片段\x1b[0m')

test('收藏片段排在最前', () => {
  stores.snippets.clear()
  stores.snippets.add({ title: '普通', language: 'python', code: 'x', tags: [], favorite: false })
  stores.snippets.add({ title: '收藏', language: 'go', code: 'x', tags: [], favorite: true })
  const page = open('pages/tech/snippets/index')
  assert.strictEqual(page.data.items[0].title, '收藏')
})

test('列表显示语言标签与行数', () => {
  stores.snippets.clear()
  stores.snippets.add({ title: 's', language: 'typescript', code: 'a\nb\nc', tags: [], favorite: false })
  const page = open('pages/tech/snippets/index')
  assert.strictEqual(page.data.items[0].languageLabel, 'TypeScript')
  assert.strictEqual(page.data.items[0].lineCount, 3)
})

test('详情载入已有片段', () => {
  stores.snippets.clear()
  const s = stores.snippets.add({ title: 'T', language: 'rust', code: 'fn main(){}', tags: ['a'], favorite: true })
  const page = open('pages/tech/snippets/detail', { id: s.id })
  assert.strictEqual(page.data.formTitle, 'T')
  assert.strictEqual(page.data.language, 'rust')
  assert.strictEqual(page.data.favorite, true)
})

test('保存新片段写入 store', () => {
  stores.snippets.clear()
  const page = open('pages/tech/snippets/detail', {})
  page.onTitle({ detail: { value: '新片段' } })
  page.onCode({ detail: { value: 'const a = 1' } })
  page.onSave()
  assert.strictEqual(stores.snippets.getAll().length, 1)
})

test('代码为空拒绝保存', () => {
  stores.snippets.clear()
  const page = open('pages/tech/snippets/detail', {})
  page.onTitle({ detail: { value: '标题' } })
  page.onSave()
  assert.strictEqual(stores.snippets.getAll().length, 0)
})

test('行数随输入实时更新', () => {
  const page = open('pages/tech/snippets/detail', {})
  page.onCode({ detail: { value: 'a\nb\nc\nd' } })
  assert.strictEqual(page.data.lineCount, 4)
})

test('复制代码写入剪贴板', () => {
  let copied = ''
  const restore = h.mockPlatform('setClipboardData', function (opts) {
    copied = opts.data
    if (opts.success) opts.success()
    return Promise.resolve()
  })
  try {
    const page = open('pages/tech/snippets/detail', {})
    page.onCode({ detail: { value: 'copy me' } })
    page.onCopyCode()
    assert.strictEqual(copied, 'copy me')
  } finally {
    restore()
  }
})

// ==================== 面试题库 ====================

console.log('\n\x1b[1m面试题库\x1b[0m')

test('收藏题目排在最前', () => {
  stores.interview.clear()
  stores.interview.add({ question: '普通题', answer: 'a', category: 'backend', difficulty: 'med', starred: false, reviewCount: 0 })
  stores.interview.add({ question: '收藏题', answer: 'a', category: 'frontend', difficulty: 'hard', starred: true, reviewCount: 0 })
  const page = open('pages/tech/interview/index')
  assert.strictEqual(page.data.items[0].question, '收藏题')
})

test('列表显示难度与复习次数', () => {
  stores.interview.clear()
  stores.interview.add({ question: 'q', answer: 'a', category: 'frontend', difficulty: 'hard', starred: false, reviewCount: 3 })
  const page = open('pages/tech/interview/index')
  assert.strictEqual(page.data.items[0].difficultyLabel, '困难')
  assert.strictEqual(page.data.items[0].reviewCount, 3)
})

test('详情载入题目与答案', () => {
  stores.interview.clear()
  const q = stores.interview.add({ question: 'Q?', answer: 'A.', category: 'ai', difficulty: 'easy', starred: false, reviewCount: 1 })
  const page = open('pages/tech/interview/detail', { id: q.id })
  assert.strictEqual(page.data.question, 'Q?')
  assert.strictEqual(page.data.answer, 'A.')
  assert.strictEqual(page.data.reviewCount, 1)
})

test('标记已复习后计数 +1 并记录日期', () => {
  stores.interview.clear()
  const q = stores.interview.add({ question: 'Q', answer: 'A', category: 'other', difficulty: 'med', starred: false, reviewCount: 0 })
  const page = open('pages/tech/interview/detail', { id: q.id })
  page.onMarkReviewed()
  assert.strictEqual(page.data.reviewCount, 1)
  const saved = stores.interview.find(q.id)
  assert.strictEqual(saved.reviewCount, 1)
  assert.strictEqual(typeof saved.lastReviewed, 'string')
})

test('新建态标记复习被拒绝', () => {
  const page = open('pages/tech/interview/detail', {})
  page.onMarkReviewed()
  assert.strictEqual(page.data.reviewCount, 0)
})

test('保存新题写入 store', () => {
  stores.interview.clear()
  const page = open('pages/tech/interview/detail', {})
  page.onQuestion({ detail: { value: '新题' } })
  page.onAnswer({ detail: { value: '新答案' } })
  page.onSave()
  assert.strictEqual(stores.interview.getAll().length, 1)
  assert.strictEqual(stores.interview.getAll()[0].reviewCount, 0)
})

test('题目为空拒绝保存', () => {
  stores.interview.clear()
  const page = open('pages/tech/interview/detail', {})
  page.onAnswer({ detail: { value: '答案' } })
  page.onSave()
  assert.strictEqual(stores.interview.getAll().length, 0)
})

// ==================== 资源收藏 ====================

console.log('\n\x1b[1m资源收藏\x1b[0m')

test('未归档优先且按星级排序', () => {
  stores.resources.clear()
  stores.resources.add({ title: '一星', type: 'article', category: 'other', stars: 1, archived: false })
  stores.resources.add({ title: '五星', type: 'book', category: 'other', stars: 5, archived: false })
  stores.resources.add({ title: '归档五星', type: 'doc', category: 'other', stars: 5, archived: true })
  const page = open('pages/tech/resources/index')
  assert.strictEqual(page.data.items[0].title, '五星')
  assert.strictEqual(page.data.items[2].title, '归档五星')
})

test('新建打开空面板', () => {
  const page = open('pages/tech/resources/index')
  page.onCreate()
  assert.strictEqual(page.data.editing, true)
  assert.strictEqual(page.data.isEdit, false)
  assert.strictEqual(page.data.formTitle, '')
})

test('保存新资源写入 store', () => {
  stores.resources.clear()
  const page = open('pages/tech/resources/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '新资源' } })
  page.onFormStars({ currentTarget: { dataset: { star: 4 } } })
  page.onSave()
  const all = stores.resources.getAll()
  assert.strictEqual(all.length, 1)
  assert.strictEqual(all[0].stars, 4)
  assert.strictEqual(page.data.editing, false)
})

test('标题为空拒绝保存', () => {
  stores.resources.clear()
  const page = open('pages/tech/resources/index')
  page.onCreate()
  page.onSave()
  assert.strictEqual(stores.resources.getAll().length, 0)
  assert.strictEqual(page.data.editing, true, '应停留在面板')
})

test('编辑载入已有资源', () => {
  stores.resources.clear()
  const r = stores.resources.add({ title: '旧标题', type: 'video', category: 'ai', stars: 2, note: '备注', archived: false })
  const page = open('pages/tech/resources/index')
  page.openPanel(stores.resources.find(r.id))
  assert.strictEqual(page.data.isEdit, true)
  assert.strictEqual(page.data.formTitle, '旧标题')
  assert.strictEqual(page.data.formType, 'video')
  assert.strictEqual(page.data.formStars, 2)
})

test('编辑保存更新原资源', () => {
  stores.resources.clear()
  const r = stores.resources.add({ title: '旧', type: 'article', category: 'other', stars: 1, archived: false })
  const page = open('pages/tech/resources/index')
  page.openPanel(stores.resources.find(r.id))
  page.onFormTitle({ detail: { value: '新' } })
  page.onSave()
  assert.strictEqual(stores.resources.getAll().length, 1)
  assert.strictEqual(stores.resources.find(r.id).title, '新')
})

test('归档开关可切换', () => {
  const page = open('pages/tech/resources/index')
  page.onCreate()
  assert.strictEqual(page.data.formArchived, false)
  page.onToggleArchived()
  assert.strictEqual(page.data.formArchived, true)
})

test('关闭面板不保存', () => {
  stores.resources.clear()
  const page = open('pages/tech/resources/index')
  page.onCreate()
  page.onFormTitle({ detail: { value: '不保存' } })
  page.onClosePanel()
  assert.strictEqual(stores.resources.getAll().length, 0)
  assert.strictEqual(page.data.editing, false)
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
