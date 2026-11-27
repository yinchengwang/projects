/**
 * stores.js —— Knowledge Hub 14 个数据域实例
 *
 * 页面统一通过 `require('../../../shared/knowledge/stores.js').quizzes` 之类访问。
 * 简单列表/对象域用 shared/core/store.js 的工厂；复杂/嵌套/混合域（daily、
 * five_year_plan、interview_bank、quizzes、knowledge、learning_paths）
 * 在工厂之上包一层自定义方法，并剥离 UI/派生态（不进 db:）。
 *
 * 批量内容（题库/文章/雷达/五年配置/面试题库/摘抄）来自 seed-data.js，
 * 结构性种子（图谱/路径/项目/面试/复习）来自 seed.js。
 */

const store = require("../core/store.js")
const storage = require("../core/storage.js")
const seed = require("./seed.js")
const data = require("./seed-data.js")
const review = require("./review.js")
const khc = require("./kh-constants.js")
const dateUtil = require("../core/date.js")

// KH 使用 14 个 db: 键。
//
// 这里**不能**用 Object.assign(storage.KEYS, KH_KEYS) —— 那会就地修改共享的
// storage.KEYS 对象，合并成单一应用后会影响其它模块。改成操作本地副本。
const KH_KEYS = {
  daily: 'daily',
  excerpts: 'excerpts',
  five_year_plan: 'five_year_plan',
  interview_bank: 'interview_bank',
  interviews: 'interviews',
  knowledge: 'knowledge',
  learning_paths: 'learning_paths',
  mock_interviews: 'mock_interviews',
  notifications: 'notifications',
  study_projects: 'study_projects',
  quizzes: 'quizzes',
  reviews: 'reviews',
  settings: 'settings',
  user: 'user',
  seeded: '__seeded__'
}
const K = Object.assign({}, storage.KEYS, KH_KEYS)

/* ============================================================
 * 1. 摘抄 excerpts（列表）
 * ========================================================== */
const excerptsBase = store.createListStore({ key: K.excerpts, seed: function () {
  return data.INITIAL_EXCERPTS.map(function (e) { return Object.assign({}, e) })
} })
const excerpts = {
  getAll: excerptsBase.getAll, setAll: excerptsBase.setAll, add: excerptsBase.add,
  update: excerptsBase.update, remove: excerptsBase.remove, find: excerptsBase.find,
  clear: excerptsBase.clear, count: excerptsBase.count,
  addExcerpt: function (input) {
    return excerptsBase.add(Object.assign({ content: '', book: '', tags: [], note: '', favorite: false, status: 'reading' }, input))
  },
  toggleFavorite: function (id) {
    const it = excerptsBase.find(id)
    if (!it) return null
    return excerptsBase.update(id, { favorite: !it.favorite })
  },
  setStatus: function (id, status) {
    return excerptsBase.update(id, { status: status })
  },
  updateNote: function (id, note) {
    return excerptsBase.update(id, { note: note })
  },
  /** 派生：书籍 / 标签 / 年份 / 统计 */
  derive: function () {
    const list = excerptsBase.getAll()
    const books = {}, tags = {}, years = {}
    list.forEach(function (e) {
      if (e.book) books[e.book] = true
      ;(e.tags || []).forEach(function (t) { tags[t] = true })
      if (e.date) years[e.date] = true
    })
    const stats = { total: list.length, favorites: 0, reading: 0, mastered: 0, books: Object.keys(books).length }
    list.forEach(function (e) {
      if (e.favorite) stats.favorites += 1
      if (e.status === 'reading') stats.reading += 1
      if (e.status === 'mastered') stats.mastered += 1
    })
    return {
      books: Object.keys(books), tags: Object.keys(tags), years: Object.keys(years).sort(), stats: stats
    }
  }
}

/* ============================================================
 * 2. 知识图谱 knowledge（列表：节点）+ 静态边
 * ========================================================== */
const knowledgeBase = store.createListStore({ key: K.knowledge, seed: function () { return seed.KNOWLEDGE_NODES.map(function (n) { return Object.assign({}, n) }) } })
const knowledge = {
  getAll: knowledgeBase.getAll, setAll: knowledgeBase.setAll, add: knowledgeBase.add,
  update: knowledgeBase.update, remove: knowledgeBase.remove, find: knowledgeBase.find,
  clear: knowledgeBase.clear, count: knowledgeBase.count,
  links: seed.KNOWLEDGE_LINKS,
  addNode: function (input) {
    return knowledgeBase.add(Object.assign({ domain: '算法', level: 1 }, input))
  },
  updateNodeLevel: function (id, level) {
    const lv = Math.max(1, Math.min(4, level))
    return knowledgeBase.update(id, { level: lv })
  },
  /** 差距分析：每域平均 level * 20 作为当前掌握度（满分 100） */
  getGaps: function () {
    const nodes = knowledgeBase.getAll()
    const out = {}
    khc.KNOWLEDGE_DOMAINS.forEach(function (d) {
      const dn = nodes.filter(function (n) { return n.domain === d.key })
      const avg = dn.length ? Math.round(dn.reduce(function (a, n) { return a + n.level }, 0) / dn.length * 20) : 0
      out[d.key] = { level: avg, count: dn.length, target: khc.KNOWLEDGE_TARGETS[d.key] || 0 }
    })
    return out
  }
}

/* ============================================================
 * 3. 学习项目 study_projects（列表）
 * ========================================================== */
const studyProjectsBase = store.createListStore({ key: K.study_projects, seed: function () { return seed.STUDY_PROJECTS.map(function (p) { return Object.assign({}, p) }) } })
const studyProjects = {
  getAll: studyProjectsBase.getAll, setAll: studyProjectsBase.setAll, add: studyProjectsBase.add,
  update: studyProjectsBase.update, remove: studyProjectsBase.remove, find: studyProjectsBase.find,
  clear: studyProjectsBase.clear, count: studyProjectsBase.count,
  addProject: function (input) {
    return studyProjectsBase.add(Object.assign({
      name: '', description: '', status: '规划中', priority: '中', tags: [], knowledgeLinks: []
    }, input))
  },
  updateProject: function (id, patch) {
    return studyProjectsBase.update(id, Object.assign({}, patch, { updatedAt: new Date().toISOString() }))
  }
}

/* ============================================================
 * 4. 面试记录 interviews（列表）
 * ========================================================== */
const interviewsBase = store.createListStore({ key: K.interviews, seed: function () { return seed.INTERVIEW_COMPANIES.map(function (c) { return Object.assign({}, c) }) } })
const interviews = {
  getAll: interviewsBase.getAll, setAll: interviewsBase.setAll, add: interviewsBase.add,
  update: interviewsBase.update, remove: interviewsBase.remove, find: interviewsBase.find,
  clear: interviewsBase.clear, count: interviewsBase.count,
  addCompany: function (input) {
    return interviewsBase.add(Object.assign({ name: '', position: '', status: '投递', salary: '', notes: '', timeline: [] }, input))
  },
  updateStatus: function (id, status) {
    return interviewsBase.update(id, { status: status })
  },
  addInterviewRecord: function (companyId, record) {
    const c = interviewsBase.find(companyId)
    if (!c) return null
    const rec = Object.assign({ id: 'rec-' + Date.now(), date: dateUtil.todayStr() }, record)
    const timeline = (c.timeline || []).concat([rec])
    return interviewsBase.update(companyId, { timeline: timeline })
  },
  /** 派生统计 */
  derive: function () {
    const list = interviewsBase.getAll()
    const active = list.filter(function (c) { return c.status !== 'offer' && c.status !== '拒绝' }).length
    return { total: list.length, active: active }
  }
}

/* ============================================================
 * 5. 模拟面试 mock_interviews（列表）
 * ========================================================== */
const mockInterviewsBase = store.createListStore({ key: K.mock_interviews, seed: function () { return seed.MOCK_SESSIONS.map(function (s) { return Object.assign({}, s) }) } })
const mockInterviews = {
  getAll: mockInterviewsBase.getAll, setAll: mockInterviewsBase.setAll, add: mockInterviewsBase.add,
  update: mockInterviewsBase.update, remove: mockInterviewsBase.remove, find: mockInterviewsBase.find,
  clear: mockInterviewsBase.clear, count: mockInterviewsBase.count,
  addSession: function (input) {
    return mockInterviewsBase.add(Object.assign({ id: 'session-' + Date.now(), date: dateUtil.todayStr(), questions: [], totalScore: 0 }, input))
  },
  deleteSession: function (id) { return mockInterviewsBase.remove(id) },
  clearHistory: function () { return mockInterviewsBase.setAll([]) },
  getSessionById: function (id) { return mockInterviewsBase.find(id) }
}

/* ============================================================
 * 6. 学习路径 learning_paths（列表，进度存 completedSubSteps）
 * ========================================================== */
const learningPathsBase = store.createListStore({ key: K.learning_paths, seed: function () { return seed.LEARNING_PATHS.map(function (p) { return JSON.parse(JSON.stringify(p)) }) } })
const learningPaths = {
  getAll: learningPathsBase.getAll, setAll: learningPathsBase.setAll, add: learningPathsBase.add,
  update: learningPathsBase.update, remove: learningPathsBase.remove, find: learningPathsBase.find,
  clear: learningPathsBase.clear, count: learningPathsBase.count,
  getPathById: function (id) { return learningPathsBase.find(id) },
  /** 切换某子步骤完成态，key = stepId:subStepId */
  toggleSubStep: function (pathId, stepId, subStepId) {
    const p = learningPathsBase.find(pathId)
    if (!p) return null
    const done = Object.assign({}, p.completedSubSteps)
    const key = stepId + ':' + subStepId
    if (done[key]) delete done[key]; else done[key] = true
    return learningPathsBase.update(pathId, { completedSubSteps: done })
  },
  getStepProgress: function (pathId, stepId) {
    const p = learningPathsBase.find(pathId)
    if (!p) return { done: 0, total: 0 }
    const step = (p.steps || []).filter(function (s) { return s.id === stepId })[0]
    if (!step) return { done: 0, total: 0 }
    const total = (step.subSteps || []).length
    let done = 0
    ;(step.subSteps || []).forEach(function (s) {
      if (p.completedSubSteps[stepId + ':' + s.id]) done += 1
    })
    return { done: done, total: total, percent: total ? Math.round(done / total * 100) : 0 }
  },
  /** 整条路径进度（已完成子步骤 / 全部子步骤） */
  getPathProgress: function (pathId) {
    const p = learningPathsBase.find(pathId)
    if (!p) return { done: 0, total: 0, percent: 0 }
    let total = 0, done = 0
    ;(p.steps || []).forEach(function (s) {
      ;(s.subSteps || []).forEach(function (ss) {
        total += 1
        if (p.completedSubSteps[s.id + ':' + ss.id]) done += 1
      })
    })
    return { done: done, total: total, percent: total ? Math.round(done / total * 100) : 0 }
  },
  resetPath: function (pathId) {
    return learningPathsBase.update(pathId, { completedSubSteps: {} })
  }
}

/* ============================================================
 * 7. 复习记录 reviews（列表 + SM-2）
 * ========================================================== */
const reviewsBase = store.createListStore({ key: K.reviews, seed: function () { return seed.REVIEW_SEED.map(function (r) { return Object.assign({}, r) }) } })
const reviews = {
  getAll: reviewsBase.getAll, setAll: reviewsBase.setAll, add: reviewsBase.add,
  update: reviewsBase.update, remove: reviewsBase.remove, find: reviewsBase.find,
  clear: reviewsBase.clear, count: reviewsBase.count,
  addRecord: function (input) {
    return reviewsBase.add(review.createReview(input))
  },
  /** 对指定记录评分（SM-2），返回更新后的记录 */
  rateReview: function (id, quality) {
    const rec = reviewsBase.find(id)
    if (!rec) return null
    const updated = review.calculateNextReview(rec, quality)
    return reviewsBase.update(id, updated)
  },
  /** 到期（nextReviewDate <= now） */
  getDue: function () {
    return review.filterDue(reviewsBase.getAll())
  },
  /** 已掌握：repetitions>0 且下次在未来 */
  getDone: function () {
    const now = Date.now()
    return reviewsBase.getAll().filter(function (r) {
      return r.repetitions > 0 && new Date(r.nextReviewDate).getTime() > now
    })
  },
  /** 7 天内 */
  getUpcoming: function () {
    const now = Date.now()
    const week = now + 7 * 86400000
    return reviewsBase.getAll().filter(function (r) {
      const t = new Date(r.nextReviewDate).getTime()
      return t > now && t <= week
    })
  }
}

/* ============================================================
 * 8. 每日学习 daily（对象：snapshots + dailyLogs）
 * ========================================================== */
function readDaily () {
  return storage.get(K.daily, null) || { snapshots: [], dailyLogs: {} }
}
function writeDaily (obj) {
  storage.set(K.daily, obj)
  return obj
}
const daily = {
  get: function () { return readDaily() },
  addSnapshot: function (snapshot) {
    const d = readDaily()
    const list = d.snapshots || []
    const idx = list.findIndex(function (s) { return s.date === snapshot.date })
    if (idx >= 0) list[idx] = snapshot; else list.push(snapshot)
    d.snapshots = list
    return writeDaily(d)
  },
  getRecentSnapshots: function (n) {
    const list = (readDaily().snapshots || []).slice().sort(function (a, b) {
      return a.date < b.date ? -1 : 1
    })
    return list.slice(-(n || 14))
  },
  getDailyLog: function (dStr) {
    const logs = readDaily().dailyLogs || {}
    return logs[dStr] || null
  },
  addDailyLog: function (log) {
    const d = readDaily()
    d.dailyLogs = d.dailyLogs || {}
    d.dailyLogs[log.date] = log
    return writeDaily(d)
  },
  recordStudy: function (itemId, correct) {
    const dStr = dateUtil.todayStr()
    const d = readDaily()
    d.dailyLogs = d.dailyLogs || {}
    const log = d.dailyLogs[dStr] || { date: dStr, studied: [], correct: [], wrong: [], reviewCount: 0, studyMinutes: 0 }
    const studied = new Set(log.studied)
    studied.add(itemId)
    log.studied = Array.from(studied)
    if (correct) { if (log.correct.indexOf(itemId) < 0) log.correct.push(itemId) }
    else { if (log.wrong.indexOf(itemId) < 0) log.wrong.push(itemId) }
    d.dailyLogs[dStr] = log
    return writeDaily(d)
  },
  recordReview: function (count) {
    const dStr = dateUtil.todayStr()
    const d = readDaily()
    d.dailyLogs = d.dailyLogs || {}
    const log = d.dailyLogs[dStr] || { date: dStr, studied: [], correct: [], wrong: [], reviewCount: 0, studyMinutes: 0 }
    log.reviewCount += (count || 1)
    d.dailyLogs[dStr] = log
    return writeDaily(d)
  },
  recordStudyTime: function (minutes) {
    const dStr = dateUtil.todayStr()
    const d = readDaily()
    d.dailyLogs = d.dailyLogs || {}
    const log = d.dailyLogs[dStr] || { date: dStr, studied: [], correct: [], wrong: [], reviewCount: 0, studyMinutes: 0 }
    log.studyMinutes += (minutes || 0)
    d.dailyLogs[dStr] = log
    return writeDaily(d)
  },
  /** 近一年热力：每天强度 = studied.length + reviewCount */
  getHeatmapData: function () {
    const logs = readDaily().dailyLogs || {}
    return Object.keys(logs).map(function (k) {
      const l = logs[k]
      return { date: k, value: (l.studied ? l.studied.length : 0) + (l.reviewCount || 0) }
    })
  },
  getTrend: function (n) {
    const logs = readDaily().dailyLogs || {}
    const arr = Object.keys(logs).map(function (k) { return { date: k, value: (logs[k].studied ? logs[k].studied.length : 0) } })
    arr.sort(function (a, b) { return a.date < b.date ? -1 : 1 })
    return arr.slice(-(n || 14))
  }
}

/* ============================================================
 * 9. 五年计划 five_year_plan（对象：嵌套树）
 * ========================================================== */
function readPlan () {
  return storage.get(K.five_year_plan, null) || { data: {} }
}
function writePlan (obj) {
  storage.set(K.five_year_plan, obj)
  return obj
}
function ensureDay (plan, year, month, day) {
  plan.data = plan.data || {}
  plan.data[year] = plan.data[year] || {}
  plan.data[year][month] = plan.data[year][month] || {}
  plan.data[year][month][day] = plan.data[year][month][day] || { checks: {}, note: '', hasLearning: false }
  return plan.data[year][month][day]
}
const fiveYearPlan = {
  get: function () { return readPlan() },
  getDay: function (year, month, day) {
    const plan = readPlan()
    if (plan.data[year] && plan.data[year][month] && plan.data[year][month][day]) {
      return plan.data[year][month][day]
    }
    return null
  },
  setDay: function (year, month, day, payload) {
    const plan = readPlan()
    const cur = ensureDay(plan, year, month, day)
    const next = Object.assign({}, cur, payload, { checks: Object.assign({}, cur.checks, payload.checks) })
    plan.data[year][month][day] = next
    return writePlan(plan)
  },
  deleteDay: function (year, month, day) {
    const plan = readPlan()
    if (plan.data[year] && plan.data[year][month]) delete plan.data[year][month][day]
    return writePlan(plan)
  },
  toggleCheck: function (year, month, day, habitId) {
    const plan = readPlan()
    const cur = ensureDay(plan, year, month, day)
    cur.checks = cur.checks || {}
    cur.checks[habitId] = !cur.checks[habitId]
    plan.data[year][month][day] = cur
    return writePlan(plan)
  },
  toggleLearning: function (year, month, day) {
    const plan = readPlan()
    const cur = ensureDay(plan, year, month, day)
    cur.hasLearning = !cur.hasLearning
    plan.data[year][month][day] = cur
    return writePlan(plan)
  },
  countDayChecks: function (year, month, day) {
    const cur = this.getDay(year, month, day)
    const total = data.getAllItems(year).length
    const done = cur ? Object.keys(cur.checks || {}).filter(function (k) { return cur.checks[k] }).length : 0
    return { done: done, total: total }
  },
  getStreak: function () {
    const d = new Date()
    let streak = 0
    for (let i = 0; i < 365; i++) {
      const y = d.getFullYear(), m = d.getMonth() + 1, day = d.getDate()
      const c = this.countDayChecks(y, m, day)
      if (c.done > 0) { streak += 1; d.setDate(d.getDate() - 1) } else break
    }
    return streak
  },
  getTotalDays: function () {
    const plan = readPlan()
    let total = 0
    const data = plan.data || {}
    Object.keys(data).forEach(function (y) {
      Object.keys(data[y]).forEach(function (m) {
        Object.keys(data[y][m]).forEach(function (d) {
          const c = data[y][m][d].checks || {}
          if (Object.keys(c).some(function (k) { return c[k] })) total += 1
        })
      })
    })
    return total
  },
  getMonthPct: function (year, month) {
    const plan = readPlan()
    const days = (plan.data[year] && plan.data[year][month]) || {}
    const keys = Object.keys(days)
    if (!keys.length) return 0
    let doneDays = 0
    keys.forEach(function (d) {
      const c = days[d].checks || {}
      if (Object.keys(c).some(function (k) { return c[k] })) doneDays += 1
    })
    return Math.round(doneDays / keys.length * 100)
  }
}

/* ============================================================
 * 10. 面试题库浏览状态 interview_bank（对象：viewed / bookmarks）
 *     题目内容在 seed-data.INTERVIEW_BANK（静态）
 * ========================================================== */
const interviewBank = store.createObjectStore({
  key: K.interview_bank,
  defaultValue: { viewed: [], bookmarks: [] }
})
interviewBank.markViewed = function (id) {
  const s = interviewBank.get()
  if (s.viewed.indexOf(id) < 0) { s.viewed.push(id); interviewBank.patch({ viewed: s.viewed }) }
  return s
}
interviewBank.toggleBookmark = function (id) {
  const s = interviewBank.get()
  const i = s.bookmarks.indexOf(id)
  if (i >= 0) s.bookmarks.splice(i, 1); else s.bookmarks.push(id)
  interviewBank.patch({ bookmarks: s.bookmarks })
  return s
}
interviewBank.isViewed = function (id) { return interviewBank.get().viewed.indexOf(id) >= 0 }
interviewBank.isBookmarked = function (id) { return interviewBank.get().bookmarks.indexOf(id) >= 0 }
interviewBank.clearViewed = function () { return interviewBank.patch({ viewed: [] }) }

/* ============================================================
 * 11. 题库 quizzes（对象：answers + plans）
 * ========================================================== */
function judge (q, userAnswer) {
  const correct = String(q.answer == null ? '' : q.answer).trim()
  const user = String(userAnswer == null ? '' : userAnswer).trim()
  if (q.options && q.options.length) {
    return user === correct || user === correct.charAt(0)
  }
  if (typeof q.answer === 'boolean') {
    return String(q.answer) === user
  }
  return user === correct
}
function calcStreak (answers) {
  if (!answers.length) return 0
  const dates = {}
  answers.forEach(function (a) { dates[a.date] = true })
  const sorted = Object.keys(dates).sort().reverse()
  let streak = 0
  const today = new Date()
  for (let i = 0; i < sorted.length; i++) {
    const diff = Math.floor((today.getTime() - new Date(sorted[i]).getTime()) / 86400000)
    if (diff === streak) streak += 1; else break
  }
  return streak
}
const quizzes = store.createObjectStore({
  key: K.quizzes,
  defaultValue: { answers: [], plans: [] }
})
quizzes.getQuestionBank = function () { return data.QUIZ_BANK }
quizzes.getQuestionById = function (id) {
  const bank = data.QUIZ_BANK
  for (let i = 0; i < bank.length; i++) if (bank[i].id === id) return bank[i]
  return null
}
quizzes.searchQuestions = function (query) {
  const q = (query || '').trim().toLowerCase()
  if (!q) return data.QUIZ_BANK
  return data.QUIZ_BANK.filter(function (it) {
    return (it.stem || '').toLowerCase().indexOf(q) >= 0 ||
      (it.stack || '').toLowerCase().indexOf(q) >= 0 ||
      (it.scenario || '').toLowerCase().indexOf(q) >= 0
  })
}
quizzes.submitAnswer = function (answerRecord, timeSpent) {
  const s = quizzes.get()
  const q = quizzes.getQuestionById(answerRecord.questionId)
  const correct = q ? judge(q, answerRecord.userAnswer) : false
  const rec = {
    questionId: answerRecord.questionId,
    userAnswer: answerRecord.userAnswer,
    correct: correct,
    date: dateUtil.todayStr(),
    timestamp: Date.now(),
    timeSpent: timeSpent || 0
  }
  s.answers = s.answers.concat([rec])
  quizzes.patch({ answers: s.answers })
  return rec
}
quizzes.getStats = function () {
  const answers = quizzes.get().answers
  const total = answers.length
  let correct = 0
  answers.forEach(function (a) { if (a.correct) correct += 1 })
  const accuracy = total ? correct / total : 0
  return { totalAnswered: total, totalCorrect: correct, accuracy: accuracy, streak: calcStreak(answers) }
}
quizzes.getWrongBook = function () {
  const answers = quizzes.get().answers
  const map = {}
  answers.forEach(function (a) {
    if (!a.correct) {
      const ex = map[a.questionId]
      if (!ex || a.timestamp > ex.timestamp) map[a.questionId] = a
    }
  })
  return Object.keys(map).map(function (id) { return { questionId: id, question: quizzes.getQuestionById(id), lastAnswer: map[id] } })
}
quizzes.isWrongAnswer = function (qid) {
  const wrong = quizzes.getWrongBook().filter(function (w) { return w.questionId === qid })
  return wrong.length > 0
}
quizzes.getProgress = function (stack) {
  const bank = data.QUIZ_BANK.filter(function (q) { return !stack || q.stack === stack })
  const total = bank.length
  const answeredIds = {}
  quizzes.get().answers.forEach(function (a) { answeredIds[a.questionId] = a.correct })
  let answered = 0, correct = 0
  bank.forEach(function (q) {
    if (answeredIds[q.id] !== undefined) { answered += 1; if (answeredIds[q.id]) correct += 1 }
  })
  return { total: total, answered: answered, correct: correct }
}
quizzes.generateDailyPlan = function (dateStr) {
  const d = dateStr || dateUtil.todayStr()
  const daysSinceEpoch = Math.floor(Date.now() / 86400000)
  const stackIndex = daysSinceEpoch % khc.STACK_ORDER.length
  const stack = khc.STACK_ORDER[stackIndex]
  const pool = data.QUIZ_BANK.filter(function (q) { return q.stack === stack })
  const answered = {}
  quizzes.get().answers.forEach(function (a) { answered[a.questionId] = true })
  const fresh = pool.filter(function (q) { return !answered[q.id] })
  const chosen = (fresh.length >= 5 ? fresh : pool).slice(0, 10)
  const plan = { date: d, stack: stack, questionIds: chosen.map(function (q) { return q.id }), completed: 0 }
  const s = quizzes.get()
  const others = s.plans.filter(function (p) { return p.date !== d })
  s.plans = others.concat([plan])
  quizzes.patch({ plans: s.plans })
  return plan
}
quizzes.getTodayPlan = function () {
  const d = dateUtil.todayStr()
  const s = quizzes.get()
  let plan = s.plans.filter(function (p) { return p.date === d })[0]
  if (!plan) plan = quizzes.generateDailyPlan(d)
  return plan
}
quizzes.markPlanProgress = function (questionId) {
  const d = dateUtil.todayStr()
  const s = quizzes.get()
  s.plans = (s.plans || []).map(function (p) {
    if (p.date === d) {
      const done = p.completed || 0
      const total = p.questionIds ? p.questionIds.length : 0
      return Object.assign({}, p, { completed: Math.min(total, done + 1) })
    }
    return p
  })
  quizzes.patch({ plans: s.plans })
  return s.plans
}

/* ============================================================
 * 12. 设置 settings（对象）
 * ========================================================== */
const settings = store.createObjectStore({
  key: K.settings,
  defaultValue: {
    theme: 'auto',
    primaryColor: 'violet',
    notifications: { reviewReminder: true, interviewReminder: true },
    apiConfig: { apiKey: '', apiUrl: '', model: '' },
    profile: { nickname: 'Yinch', role: '后端开发', bio: 'Reading Radar 学习者' },
    schemaVersion: 1
  }
})

/* ============================================================
 * 13. 用户 user（对象）
 * ========================================================== */
function getInitial (name) {
  if (!name || !name.trim()) return '?'
  const t = name.trim()
  if (/^[a-zA-Z\s]/.test(t)) return t.charAt(0).toUpperCase()
  return t.charAt(0)
}
const user = store.createObjectStore({
  key: K.user,
  defaultValue: { name: 'Yinch', avatar: 'Y', joinDate: new Date().toISOString(), bio: 'Reading Radar 学习者' }
})
user.setName = function (name) {
  const u = user.get()
  const avatar = u.avatar && u.avatar !== getInitial(u.name) ? u.avatar : getInitial(name)
  return user.patch({ name: name, avatar: avatar })
}
user.setEmail = function (email) { return user.patch({ email: email }) }
user.setBio = function (bio) { return user.patch({ bio: bio }) }
user.setAvatar = function (avatar) { return user.patch({ avatar: avatar }) }
user.reset = function () {
  const u = user.get()
  return user.patch({ name: 'Yinch', avatar: 'Y', bio: 'Reading Radar 学习者', joinDate: u.joinDate })
}

/* ============================================================
 * 14. 通知 notifications（纯内存，不持久化；由其它域派生）
 * ========================================================== */
const notifications = {
  items: [],
  unreadCount: 0,
  setItems: function (items) {
    this.items = items || []
    this.unreadCount = this.items.filter(function (i) { return !i.read }).length
    return this.items
  },
  markRead: function (id) {
    this.items = this.items.map(function (i) { return i.id === id ? Object.assign({}, i, { read: true }) : i })
    this.unreadCount = this.items.filter(function (i) { return !i.read }).length
  },
  markAllRead: function () {
    this.items = this.items.map(function (i) { return Object.assign({}, i, { read: true }) })
    this.unreadCount = 0
  },
  /** 由复习 / 五年计划 / 面试派生今日通知 */
  build: function () {
    const items = []
    const due = reviews.getDue()
    if (due.length) {
      items.push({ id: 'n-review', type: 'review', title: '待复习', desc: '有 ' + due.length + ' 张卡片待复习', icon: '📚', iconBg: 'rgba(2,132,199,0.14)', read: false, time: Date.now() })
    }
    const streak = fiveYearPlan.getStreak()
    if (streak > 0) {
      items.push({ id: 'n-streak', type: 'streak', title: '连续打卡', desc: '已连续学习 ' + streak + ' 天', icon: '🔥', iconBg: 'rgba(245,165,36,0.14)', read: false, time: Date.now() })
    }
    return this.setItems(items)
  }
}

module.exports = {
  excerpts: excerpts,
  knowledge: knowledge,
  studyProjects: studyProjects,
  interviews: interviews,
  mockInterviews: mockInterviews,
  learningPaths: learningPaths,
  reviews: reviews,
  daily: daily,
  fiveYearPlan: fiveYearPlan,
  interviewBank: interviewBank,
  quizzes: quizzes,
  settings: settings,
  user: user,
  notifications: notifications
}
