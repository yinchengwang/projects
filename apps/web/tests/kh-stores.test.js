/**
 * stores.test.js —— 14 个数据域逻辑验证
 *
 * 覆盖：种子加载、SM-2 评分、quiz 判分/统计/错题本/每日计划、知识图谱差距、
 * 五年计划打卡/连续天数、学习路径进度、摘抄派生、面试题库浏览状态。
 */

const assert = require('assert')
const stub = require('./kh-stub.js')
stub.installLocalStorage()

const stores = require('../shared/knowledge/stores.js')
const review = require('../shared/knowledge/review.js')

let passed = 0
function ok (name, cond) {
  if (!cond) throw new Error('FAIL: ' + name)
  passed += 1
  process.stdout.write('  \x1b[32m·\x1b[0m ' + name + '\n')
}

// ---- 种子加载 ----
ok('excerpts 种子 4 条', stores.excerpts.getAll().length === 4)
ok('knowledge 节点 14', stores.knowledge.getAll().length === 14)
ok('knowledge 边 9', stores.knowledge.links.length === 9)
ok('studyProjects 3 条', stores.studyProjects.getAll().length === 3)
ok('interviews 2 家', stores.interviews.getAll().length === 2)
ok('mockInterviews 1 条', stores.mockInterviews.getAll().length === 1)
ok('learningPaths 3 条', stores.learningPaths.getAll().length === 3)
ok('reviews 3 条', stores.reviews.getAll().length === 3)
ok('settings 默认 violet', stores.settings.get().primaryColor === 'violet')
ok('user 默认 Yinch', stores.user.get().name === 'Yinch')

// ---- SM-2 ----
const r = stores.reviews.rateReview(stores.reviews.getAll()[0].id, 5)
ok('rateReview 后 repetitions=1', r.repetitions === 1)
ok('rateReview 后 interval=1', r.interval === 1)
ok('getDue 仍为数组', Array.isArray(stores.reviews.getDue()))

const r2 = stores.reviews.addRecord({ question: '测试题?', answer: '答', domain: '算法' })
ok('addRecord 默认 easiness', r2.easiness === 2.5)
ok('addRecord 默认 interval', r2.interval === 1)

// ---- quiz 判分 / 统计 ----
const bank = stores.quizzes.getQuestionBank()
const choiceQ = bank.filter(function (q) { return q.type === '选择题' })[0]
const res = stores.quizzes.submitAnswer({ questionId: choiceQ.id, userAnswer: choiceQ.answer })
ok('选择题判分正确', res.correct === true)
const wrongQ = bank.filter(function (q) { return q.type === '选择题' })[1]
const wrong = stores.quizzes.submitAnswer({ questionId: wrongQ.id, userAnswer: 'Z' })
ok('错误选择判分错误', wrong.correct === false)
const stats = stores.quizzes.getStats()
ok('统计总数 2', stats.totalAnswered === 2)
ok('统计正确 1', stats.totalCorrect === 1)
const book = stores.quizzes.getWrongBook()
ok('错题本 1 条', book.length === 1 && book[0].questionId === wrongQ.id)
ok('isWrongAnswer 命中', stores.quizzes.isWrongAnswer(wrongQ.id) === true)

const plan = stores.quizzes.generateDailyPlan()
ok('每日计划含 stack', khc().STACK_ORDER.indexOf(plan.stack) >= 0)
ok('每日计划有题目', plan.questionIds.length > 0)
// 与实现一致：用本地日期（todayStr），不用 UTC —— 东八区跨 UTC 午夜时会误判
ok('getTodayPlan 返回计划', stores.quizzes.getTodayPlan().date === require('../shared/core/date.js').todayStr())

function khc () { return require('../shared/knowledge/kh-constants.js') }

// ---- 知识图谱差距 ----
const gaps = stores.knowledge.getGaps()
ok('gaps 含 5 域', Object.keys(gaps).length === 5)
ok('算法域有 count', gaps['算法'].count === 4)

// ---- 五年计划 ----
const dateUtil = require('../shared/core/date.js')
const todayStr = dateUtil.todayStr()
const y = parseInt(todayStr.slice(0, 4), 10)
const m = parseInt(todayStr.slice(5, 7), 10)
const d = parseInt(todayStr.slice(8, 10), 10)
stores.fiveYearPlan.toggleCheck(y, m, d, 'h-algo')
const cc = stores.fiveYearPlan.countDayChecks(y, m, d)
ok('打卡后 done=1', cc.done === 1)
ok('打卡 total>0', cc.total > 0)
stores.fiveYearPlan.toggleLearning(y, m, d)
ok('toggleLearning 生效', stores.fiveYearPlan.getDay(y, m, d).hasLearning === true)
const streak = stores.fiveYearPlan.getStreak()
ok('连续打卡天数>=1', streak >= 1)

// ---- 学习路径进度 ----
const path = stores.learningPaths.getAll()[0]
const step = path.steps[0]
const ss = step.subSteps[0]
stores.learningPaths.toggleSubStep(path.id, step.id, ss.id)
const sp = stores.learningPaths.getStepProgress(path.id, step.id)
ok('子步骤进度 done=1', sp.done === 1)
const pp = stores.learningPaths.getPathProgress(path.id)
ok('路径进度总子步骤>0', pp.total > 0 && pp.done === 1)
stores.learningPaths.resetPath(path.id)
ok('重置后 done=0', stores.learningPaths.getPathProgress(path.id).done === 0)

// ---- 摘抄派生 ----
const der = stores.excerpts.derive()
ok('摘抄派生 books>0', der.books.length > 0)
ok('摘抄统计 total=4', der.stats.total === 4)
const exId = stores.excerpts.getAll()[0].id
const favBefore = stores.excerpts.find(exId).favorite
stores.excerpts.toggleFavorite(exId)
ok('toggleFavorite 取反', stores.excerpts.find(exId).favorite === !favBefore)

// ---- 面试题库浏览状态 ----
ok('初始未浏览', stores.interviewBank.isViewed('x') === false)
stores.interviewBank.markViewed('x')
ok('markViewed 生效', stores.interviewBank.isViewed('x') === true)
stores.interviewBank.toggleBookmark('x')
ok('toggleBookmark 生效', stores.interviewBank.isBookmarked('x') === true)

// ---- 面试记录 ----
const companies = stores.interviews.getAll()
stores.interviews.updateStatus(companies[0].id, 'offer')
ok('updateStatus 生效', stores.interviews.find(companies[0].id).status === 'offer')
stores.interviews.addInterviewRecord(companies[0].id, { type: 'HR', content: '谈薪' })
ok('addInterviewRecord 生效', stores.interviews.find(companies[0].id).timeline.length === companies[0].timeline.length + 1)

// ---- 每日学习热力 ----
stores.daily.recordStudy('q-1', true)
stores.daily.recordReview(2)
const heat = stores.daily.getHeatmapData()
ok('热力数据含今天', heat.some(function (h) { return h.value >= 3 }))

console.log('\n\x1b[32m✓ kh-stores.test.js\x1b[0m  ' + passed + ' 条断言通过')
