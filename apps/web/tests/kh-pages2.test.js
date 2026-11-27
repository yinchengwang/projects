/**
 * pages2.test.js —— P2/P3/P4 页面交互（面试 / 知识 / 我的 / 计划）
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

// showModal 在测试中需要立即确认
h.mockPlatform('showModal', function () { return Promise.resolve({ confirm: true, content: '' }) })

/* ============ 面试聚合 ============ */
const iv = h.createPage('pages/interview/index')
iv.onLoad()
ok('面试聚合 3 入口', iv.data.entries.length === 3)
ok('面试统计有公司数', iv.data.stats.companies === 2)

/* ============ 面试追踪 ============ */
const tr = h.createPage('pages/interview/tracker/index')
tr.onLoad()
ok('追踪列表 2 家公司', tr.data.list.length === 2)

const cid = tr.data.list[0].id
h.tap(tr, 'onOpen', { id: cid })
ok('打开详情', tr.data.current !== null)
ok('详情有时间线', Array.isArray(tr.data.current.timeline))

h.tap(tr, 'onChangeStatus', { status: 'offer' })
ok('状态改为 offer', tr.data.current.status === 'offer')

// 添加面试记录
h.tap(tr, 'onOpenRecord', {})
ok('记录面板打开', tr.data.showRecord === true)
h.tap(tr, 'onPickRecType', { type: '二面' })
ok('轮次已选', tr.data.recForm.type === '二面')
tr.onRecInput({ currentTarget: { dataset: { field: 'content' } }, detail: { value: '系统设计' } })
tr.onSaveRecord()
ok('记录已保存', tr.data.current.timeline.length >= 2)

// 添加公司
tr.onOpenAdd()
tr.onAddInput({ currentTarget: { dataset: { field: 'name' } }, detail: { value: '腾讯' } })
tr.onSaveCompany()
ok('公司+1', tr.data.list.length === 3)

/* ============ 面试题库 ============ */
const bank = h.createPage('pages/interview/bank/index')
bank.onLoad()
ok('题库有分类', bank.data.categories.length > 0)
ok('题库有题目', bank.data.list.length > 0)

h.tap(bank, 'onSwitchBank', { key: 'interview' })
ok('切到后端题库', bank.data.bank === 'interview' && bank.data.list.length > 0)

const qid = bank.data.list[0].id
h.tap(bank, 'onOpen', { id: qid })
ok('打开题目渲染 html', bank.data.html.length > 0)
ok('打开即标记已读', bank.data.viewedCount >= 1)

h.tap(bank, 'onToggleBookmark', { id: qid })
ok('收藏生效', bank.data.bookmarkCount >= 1)
h.tap(bank, 'onClose', {})
ok('关闭题目', bank.data.current === null)

/* ============ 模拟面试 ============ */
const mock = h.createPage('pages/interview/mock/index')
mock.onLoad()
ok('模拟面试有历史', mock.data.history.length === 1)

h.tap(mock, 'onPick', { field: 'role', value: '算法工程师' })
ok('岗位已选', mock.data.config.role === '算法工程师')
h.tap(mock, 'onPick', { field: 'count', value: '2', type: 'number' })
ok('题数已选', mock.data.config.count === 2)

mock.onStart()
ok('进入答题', mock.data.step === 'interview')
ok('已生成题目', mock.data.questions.length === 2)
ok('倒计时启动', mock.data.remaining === mock.data.config.minutes * 60)
mock.stopTimer()

mock.onAnswer({ detail: { value: '首先介绍限流，然后说明令牌桶的原理，1. 2. 3. 具体实现如下' } })
ok('答案已记录', mock.data.answers[0].length > 0)
mock.onNext()
ok('进入第 2 题', mock.data.index === 1)
mock.onAnswer({ detail: { value: '短答案' } })
mock.onFinish()
ok('出报告', mock.data.step === 'result' && mock.data.result !== null)
ok('报告有评分', typeof mock.data.result.score === 'number')
ok('历史 +1', mock.data.history.length === 2)

/* ============ 知识聚合 ============ */
const kn = h.createPage('pages/knowledge/index')
kn.onLoad()
ok('知识聚合 3 入口', kn.data.entries.length === 3)
ok('知识聚合有差距', kn.data.gaps.length === 5)

/* ============ 知识图谱 ============ */
const g = h.createPage('pages/knowledge/graph/index')
g.onLoad()
ok('图谱 14 节点', g.data.nodes.length === 14)
h.tap(g, 'onSelect', { id: 'cpp-2' })
ok('选中节点', g.data.selectedId === 'cpp-2')
ok('有关联关系', g.data.related.length === 2)
h.tap(g, 'onAdjust', { delta: '1' })
ok('等级+1', g.data.selected.level === 3)
h.tap(g, 'onClose', {})
ok('取消选中', g.data.selectedId === '')

/* ============ 摘抄管理 ============ */
const ex = h.createPage('pages/knowledge/excerpt/index')
ex.onLoad()
ok('摘抄 4 条', ex.data.list.length === 4)
ok('摘抄有书籍', ex.data.books.length > 0)

h.tap(ex, 'onOpen', { id: ex.data.list[0].id })
ok('打开摘抄', ex.data.current !== null)
const favBefore = ex.data.current.favorite
h.tap(ex, 'onToggleFav', { id: ex.data.current.id })
ok('收藏取反', ex.data.current.favorite === !favBefore)

h.tap(ex, 'onPickBook', { book: ex.data.books[0] })
ok('按书籍筛选', ex.data.list.length > 0 && ex.data.list.every(function (x) { return x.book === ex.data.books[0] }))

h.fire(ex, 'onSearch', '')
h.tap(ex, 'onClose', {})
ex.onOpenAdd()
ex.onAddInput({ currentTarget: { dataset: { field: 'content' } }, detail: { value: '新摘抄内容' } })
ex.onAddInput({ currentTarget: { dataset: { field: 'book' } }, detail: { value: '测试书' } })
ex.onSave()
ok('摘抄 +1', ex.data.stats.total === 5)

/* ============ 技术雷达 ============ */
const r = h.createPage('pages/knowledge/radar/index')
r.onLoad()
ok('雷达 18 知识点', r.data.items.length === 18)
ok('雷达 7 栈', r.data.rows.length === 7)
ok('雷达 5 差距域', r.data.gaps.length === 5)
h.tap(r, 'onMode', { mode: 'stack' })
ok('切到栈视图', r.data.mode === 'stack')
h.tap(r, 'onMode', { mode: 'gap' })
ok('切到差距视图', r.data.mode === 'gap')
h.tap(r, 'onMode', { mode: 'quadrant' })
h.tap(r, 'onPickQuadrant', { key: 'language' })
ok('按象限筛选', r.data.list.length > 0 && r.data.list.every(function (x) { return x.quadrant === 'language' }))

/* ============ 我的 ============ */
const pf = h.createPage('pages/profile/index')
pf.onLoad()
ok('我的有用户', pf.data.user.name === 'Yinch')
ok('我的有主题模式', pf.data.themeModes.length === 3)
ok('我的有主色', pf.data.primaryColors.length === 5)

h.tap(pf, 'onPickTheme', { key: 'dark' })
ok('主题切深色', pf.data.settings.theme === 'dark' && pf.data.isDark === true)
h.tap(pf, 'onPickTheme', { key: 'light' })
ok('主题切浅色', pf.data.isDark === false)
h.tap(pf, 'onPickPrimary', { key: 'teal' })
ok('主色已换', pf.data.settings.primaryColor === 'teal')

pf.onOpenEdit()
pf.onEditInput({ currentTarget: { dataset: { field: 'name' } }, detail: { value: '新名字' } })
pf.onSaveProfile()
ok('资料已保存', pf.data.user.name === '新名字')

/* ============ 五年计划 ============ */
const fy = h.createPage('pages/plan/five-year/index')
fy.onLoad()
ok('五年计划有年份', fy.data.years.length === 5)
ok('月历已生成', fy.data.days.length >= 4)
ok('习惯项非空', fy.data.habits.length > 0)

h.tap(fy, 'onQuickCheck', { habit: fy.data.habits[0].id })
ok('快速打卡生效', fy.data.totalDays >= 1)

const firstDayCell = fy.data.days[0].filter(function (c) { return c })[0]
h.tap(fy, 'onOpenDay', { day: firstDayCell.day })
ok('打开某天', fy.data.showEdit === true && fy.data.current === firstDayCell.day)
fy.onEditInput({ detail: { value: '今天学了排序' } })
fy.onSaveDay()
ok('当日记录已存', fy.data.showEdit === false)

h.tap(fy, 'onShiftMonth', { delta: '1' })
ok('月份已切换', fy.data.month >= 1 && fy.data.month <= 12)

/* ============ 项目路线 ============ */
const rm = h.createPage('pages/plan/roadmap/index')
rm.onLoad()
ok('路线 3 个项目', rm.data.list.length === 3)
ok('统计有进行中', rm.data.stats.doing === 1)

h.tap(rm, 'onPickFilter', { status: '进行中' })
ok('按状态筛选', rm.data.list.length === 1)
h.tap(rm, 'onPickFilter', { status: '进行中' })

h.tap(rm, 'onOpen', { id: rm.data.list[0].id })
ok('打开项目', rm.data.current !== null)
h.tap(rm, 'onChangeStatus', { status: '已完成' })
ok('状态已改', rm.data.current.status === '已完成')

rm.onOpenAdd()
rm.onAddInput({ currentTarget: { dataset: { field: 'name' } }, detail: { value: '新项目' } })
h.tap(rm, 'onPickForm', { field: 'priority', value: '高' })
rm.onSave()
ok('项目 +1', rm.data.stats.total === 4)

console.log('\n\x1b[32m✓ kh-pages2.test.js\x1b[0m  ' + passed + ' 条断言通过')