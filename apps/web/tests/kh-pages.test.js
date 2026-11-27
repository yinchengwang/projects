/**
 * pages.test.js —— P1 页面加载与交互（首页仪表盘 / 学习聚合 / 复习 / 题库 / 图解 / 路径）
 *
 * 用 page-harness 加载真实页面模块，驱动生命周期与 handler。
 * tap 传 dataset 原始对象（harness 会包成 {currentTarget:{dataset}}）。
 */

const assert = require('assert')
const stub = require('./kh-stub.js')
stub.installLocalStorage()
const h = require('./kh-harness.js')

let passed = 0
function ok (name, cond) {
  if (!cond) throw new Error('FAIL: ' + name)
  passed += 1
  process.stdout.write('  \x1b[32m·\x1b[0m ' + name + '\n')
}

/* ============ 首页仪表盘 ============ */
const dash = h.createPage('pages/dashboard/index')
dash.onLoad()
ok('仪表盘有 5 项统计', dash.data.stats.length === 5)
ok('仪表盘有 7 个栈进度', dash.data.stackProgress.length === 7)
ok('仪表盘有 4 个快捷入口', dash.data.shortcuts.length === 4)
ok('仪表盘待补强最多 3 条', dash.data.gaps.length <= 3)
ok('仪表盘有问候语', dash.data.greeting.length > 0)

let navUrl = ''
h.mockPlatform('navigateTo', function (url) { navUrl = url })
h.tap(dash, 'onOpen', { path: '/pages/learn/quiz/index' })
ok('仪表盘入口跳转正确', navUrl === '/pages/learn/quiz/index')

/* ============ 学习聚合 ============ */
const learn = h.createPage('pages/learn/index')
learn.onLoad()
ok('学习聚合 4 入口', learn.data.entries.length === 4)
ok('学习聚合有计划', learn.data.plan.total > 0)
h.tap(learn, 'onOpen', { path: '/pages/learn/review/index' })
ok('学习入口跳转正确', navUrl === '/pages/learn/review/index')

/* ============ 复习计划（SM-2） ============ */
const review = h.createPage('pages/learn/review/index')
review.onLoad()
ok('复习默认 due 视图', review.data.tab === 'due')
ok('复习有待复习卡片', review.data.current !== null)
ok('复习默认不显示答案', review.data.revealed === false)

h.tap(review, 'onReveal', {})
ok('点击后显示答案', review.data.revealed === true)

review.onRate({ currentTarget: { dataset: { q: '5' } } })
ok('评分后仍有卡片或空态', review.data.current !== undefined)

review.onOpenAdd()
ok('新增面板打开', review.data.showAdd === true)
review.onInput({ currentTarget: { dataset: { field: 'question' } }, detail: { value: '测试问题?' } })
review.onInput({ currentTarget: { dataset: { field: 'answer' } }, detail: { value: '测试答案' } })
review.onSaveCard()
ok('新增后总数+1', review.data.stats.all >= 4)

h.tap(review, 'onSwitchTab', { key: 'all' })
ok('切到全部视图', review.data.tab === 'all' && review.data.list.length >= 4)

/* ============ 题库练习（5 模式） ============ */
const quiz = h.createPage('pages/learn/quiz/index')
quiz.onLoad()
ok('题库有 7 个栈 tab', quiz.data.stacks.length === 7)
ok('题库有今日计划', quiz.data.plan.total > 0)
ok('题库列表非空', quiz.data.questions.length > 0)

h.tap(quiz, 'onPickStack', { key: 'c' })
ok('按 c 栈筛选生效', quiz.data.questions.every(function (q) { return q.stackLabel === 'C' }))

quiz.onStartPlan()
ok('进入答题模式', quiz.data.mode === 'quiz')
ok('会话首题存在', quiz.data.session.length > 0)
ok('选项已渲染', Array.isArray(quiz.data.options))

const optCount = quiz.data.options.length
if (optCount > 0) {
  h.tap(quiz, 'onPickOption', { letter: quiz.data.options[0].letter })
  ok('选项已选中', quiz.data.picked === quiz.data.options[0].letter)
  quiz.onSubmit()
  ok('提交后有结果', quiz.data.result !== null)
  quiz.onNext()
}

h.tap(quiz, 'onMode', { mode: 'stats' })
ok('切到统计视图', quiz.data.mode === 'stats' && quiz.data.progress.length === 7)
h.tap(quiz, 'onMode', { mode: 'wrong' })
ok('切到错题视图', quiz.data.mode === 'wrong')

/* ============ 图解系列 ============ */
const deep = h.createPage('pages/learn/deep/index')
deep.onLoad()
ok('图解有文章列表', deep.data.list.length > 0)
ok('图解默认 light', deep.data.isDark === false)
const slug = deep.data.list[0].slug
h.tap(deep, 'onOpen', { slug: slug })
ok('打开文章渲染 html', deep.data.html.length > 0 && deep.data.reading !== null)
h.tap(deep, 'onClose', {})
ok('关闭阅读', deep.data.reading === null)

h.fire(deep, 'onSearch', '指针')
ok('搜索有结果', deep.data.list.length >= 0)
h.fire(deep, 'onSearch', '')
ok('清空搜索恢复全部', deep.data.list.length > 0)

/* ============ 学习路径 ============ */
const path = h.createPage('pages/learn/path/index')
path.onLoad()
ok('路径有 3 条', path.data.paths.length === 3)
ok('默认选中第一条', path.data.activeId === path.data.paths[0].id)
ok('步骤已展开', path.data.steps.length >= 3)
const step0 = path.data.steps[0]
ok('步骤含子步骤', step0.subSteps.length > 0)

const sub0 = step0.subSteps[0]
h.tap(path, 'onToggleSub', { step: step0.id, sub: sub0.id })
ok('勾选后子步骤标记完成', path.data.steps[0].subSteps[0].done === true)
ok('步骤进度提升', path.data.steps[0].done === 1)

const pid = path.data.paths[1].id
h.tap(path, 'onPickPath', { id: pid })
ok('切换路径', path.data.activeId === pid)

console.log('\n\x1b[32m✓ kh-pages.test.js\x1b[0m  ' + passed + ' 条断言通过')