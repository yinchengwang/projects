/**
 * review.test.js —— SM-2 算法单测
 *
 * 覆盖：默认值、首次/二次/三次复习的间隔递进、低分重置、easiness 下限、
 * 到期判断与过滤。
 */

const assert = require('assert')
const review = require('../shared/knowledge/review.js')

let passed = 0
function ok (name, cond) {
  if (!cond) throw new Error('FAIL: ' + name)
  passed += 1
  process.stdout.write('  \x1b[32m·\x1b[0m ' + name + '\n')
}

// ---- createReview 默认字段 ----
const r0 = review.createReview({ id: 'a', question: 'q', answer: 'a', domain: 'x' })
ok('默认 easiness = 2.5', r0.easiness === 2.5)
ok('默认 interval = 1', r0.interval === 1)
ok('默认 repetitions = 0', r0.repetitions === 0)
ok('nextReviewDate 为 ISO 字符串', typeof r0.nextReviewDate === 'string' && !isNaN(Date.parse(r0.nextReviewDate)))
ok('lastReviewDate 初始为空', r0.lastReviewDate === '')

// ---- 首次高质量复习：repetitions 0->1，interval 保持 1 ----
let r = review.calculateNextReview({ easiness: 2.5, interval: 1, repetitions: 0 }, 5)
ok('q5: repetitions 0->1', r.repetitions === 1)
ok('q5: 首次 interval 保持 1', r.interval === 1)
ok('q5: easiness 上升', r.easiness > 2.5)

// ---- 第二次高质量：interval 跳到 6 ----
r = review.calculateNextReview({ easiness: 2.6, interval: 1, repetitions: 1 }, 4)
ok('q4: repetitions 1->2', r.repetitions === 2)
ok('q4: 第二次 interval = 6', r.interval === 6)

// ---- 第三次高质量：interval = round(prev * ease) ----
r = review.calculateNextReview({ easiness: 2.6, interval: 6, repetitions: 2 }, 5)
ok('q5: repetitions 2->3', r.repetitions === 3)
ok('q5: 第三次 interval = round(6 * ease)', r.interval === Math.round(6 * r.easiness))

// ---- 低分（<3）重置 ----
r = review.calculateNextReview({ easiness: 2.8, interval: 30, repetitions: 5 }, 1)
ok('q1: repetitions 重置为 0', r.repetitions === 0)
ok('q1: interval 重置为 1', r.interval === 1)

// ---- easiness 下限 1.3 ----
r = review.calculateNextReview({ easiness: 1.3, interval: 10, repetitions: 3 }, 0)
ok('easiness 不低于 1.3', r.easiness === 1.3)

// ---- 计算后的记录保留原始字段 ----
const before = { id: 'x', question: 'Q', answer: 'A', domain: 'D', easiness: 2.5, interval: 1, repetitions: 0, nextReviewDate: new Date().toISOString(), lastReviewDate: '' }
const after = review.calculateNextReview(before, 5)
ok('保留 id', after.id === 'x')
ok('保留 question/answer/domain', after.question === 'Q' && after.answer === 'A' && after.domain === 'D')

// ---- 到期判断 ----
const past = new Date(Date.now() - 86400000).toISOString()
const future = new Date(Date.now() + 86400000).toISOString()
ok('过去的日期已到期', review.isDue({ nextReviewDate: past }) === true)
ok('未来的日期未到期', review.isDue({ nextReviewDate: future }) === false)
ok('filterDue 只保留到期项', review.filterDue([
  { nextReviewDate: past }, { nextReviewDate: future }, { nextReviewDate: past }
]).length === 2)

console.log('\n\x1b[32m✓ kh-review.test.js\x1b[0m  ' + passed + ' 条断言通过')
