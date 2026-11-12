"use strict"
/**
 * next-action.test.js —— 推下一题：优先级 / 单日配额 / 轮次挑选
 */

const test = require("node:test")
const assert = require("node:assert")

const { openDb, createStore } = require("../server/db.js")
const scheduler = require("../core/scheduler/next-action.js")

const NOW = "2026-11-11T09:00:00.000Z"

/** 本地日期键（独立实现，不复用被测模块的函数） */
function localDayKey(iso) {
  const d = new Date(iso)
  const m = String(d.getMonth() + 1).padStart(2, "0")
  const day = String(d.getDate()).padStart(2, "0")
  return d.getFullYear() + "-" + m + "-" + day
}

function freshStore() {
  const db = openDb(":memory:")
  return { db: db, store: createStore(db) }
}

function addCard(store, problemId, nextReviewAt) {
  store.upsertReviewCard({
    problem_id: problemId,
    easiness: 2.5,
    interval_days: 1,
    repetitions: 1,
    last_quality: 4,
    last_at: "2026-11-01T00:00:00.000Z",
    next_review_at: nextReviewAt,
    lapses: 0,
  })
}

function addAttempt(store, problemId, verdict, ts) {
  store.insertAttempt({
    problem_id: problemId,
    ts: ts || "2026-11-01T00:00:00.000Z",
    language: "cpp",
    code: "int main(){}",
    verdict: verdict,
    passed_cases: verdict === "pass" ? 2 : 0,
    total_cases: 2,
    duration_ms: 600000,
    mode: "solve",
  })
}

function makeItem(id, title, status) {
  return {
    raw: "| " + id + " | " + title + " | " + (status || "todo") + " |",
    id: id,
    title: title,
    status: status || "todo",
    doneDate: "",
    commitSha: "",
  }
}

function makeRound(id, startedAt, items) {
  return {
    id: id,
    file: "/fake/" + id + "-progress.md",
    title: id + " 轮次",
    goal: "把 " + id + " 刷完",
    status: "active",
    startedAt: startedAt,
    items: items,
  }
}

function makeCtx(store, extra) {
  return Object.assign(
    {
      store: store,
      provider: null,
      rounds: [],
      nowIso: NOW,
      settings: store.allSettings(),
      problems: [],
    },
    extra || {}
  )
}

const PROBLEMS = [
  {
    id: "lc-00001",
    title: "两数之和",
    difficulty: "medium",
    topics: ["哈希表"],
    tracks: ["interview"],
  },
  {
    id: "lc-00002",
    title: "三数之和",
    difficulty: "medium",
    topics: ["双指针"],
    tracks: ["interview"],
  },
]

test("有到期卡片时先给复习动作", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.ok(action)
  assert.strictEqual(action.kind, "review")
  assert.strictEqual(action.problemId, "lc-00001")
  assert.strictEqual(action.reviewQueueLength, 1)
  assert.ok(action.reason.indexOf("复习") !== -1, action.reason)
  db.close()
})

test("逾期越久的卡片排在前面", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-01T00:00:00.000Z")
  addCard(store, "lc-00002", "2026-11-11T00:00:00.000Z")
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.strictEqual(action.problemId, "lc-00001", "逾期十天的比刚到期的更该先做")
  db.close()
})

test("复习题数配额用满后让位给轮次推进", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  store.setSetting("review.used.questions." + localDayKey(NOW), "10")
  const rounds = [makeRound("r33", "2026-07-12", [makeItem("arrays", "数组技巧", "todo")])]
  const action = scheduler.nextAction(
    makeCtx(store, { rounds: rounds, problems: PROBLEMS })
  )
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.reviewQueueLength, 1, "队列长度照实报")
  db.close()
})

test("复习分钟配额用满后同样让位", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  store.setSetting("review.used.minutes." + localDayKey(NOW), "20")
  const rounds = [makeRound("r33", "2026-07-12", [makeItem("arrays", "数组技巧", "todo")])]
  const action = scheduler.nextAction(
    makeCtx(store, { rounds: rounds, problems: PROBLEMS })
  )
  assert.strictEqual(action.kind, "round")
  db.close()
})

test("配额上限可以在 settings 里调小", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  store.setSetting("review.daily_quota_questions", "0")
  const rounds = [makeRound("r33", "2026-07-12", [makeItem("arrays", "数组技巧", "todo")])]
  const action = scheduler.nextAction(
    makeCtx(store, { rounds: rounds, problems: PROBLEMS })
  )
  assert.strictEqual(action.kind, "round")
  db.close()
})

test("多个 active 轮次优先最近创建的", function () {
  const { db, store } = freshStore()
  const rounds = [
    makeRound("r30", "2026-05-01", [makeItem("a", "旧轮次的题", "todo")]),
    makeRound("r38", "2026-10-01", [makeItem("b", "新轮次的题", "todo")]),
  ]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: PROBLEMS }))
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.problemId, null, "没匹配到题库题目时 problemId 为空")
  assert.ok(action.reason.indexOf("r38") !== -1, action.reason)
  db.close()
})

test("手动指定当前轮次时按指定的来", function () {
  const { db, store } = freshStore()
  const rounds = [
    makeRound("r30", "2026-05-01", [makeItem("a", "旧轮次的题", "todo")]),
    makeRound("r38", "2026-10-01", [makeItem("b", "新轮次的题", "todo")]),
  ]
  const action = scheduler.nextAction(
    makeCtx(store, { rounds: rounds, problems: PROBLEMS, currentRoundId: "r30" })
  )
  assert.ok(action.reason.indexOf("r30") !== -1, action.reason)
  db.close()
})

test("轮次里掌握度最低的 topic 优先", function () {
  const { db, store } = freshStore()
  // 哈希表：首次尝试失败 → 掌握度低；双指针：首次通过 → 掌握度高
  addAttempt(store, "lc-00001", "wrong_answer")
  addAttempt(store, "lc-00002", "pass")
  const rounds = [
    makeRound("r33", "2026-07-12", [
      makeItem("two_sum", "三数之和", "todo"),
      makeItem("hash", "两数之和", "todo"),
    ]),
  ]
  const action = scheduler.nextAction(
    makeCtx(store, { rounds: rounds, problems: PROBLEMS })
  )
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.problemId, "lc-00001", "掌握度低的「哈希表」应排前面")
  db.close()
})

test("同一 topic 内按清单里的题号顺序", function () {
  const { db, store } = freshStore()
  const same = [
    { id: "lc-00001", title: "两数之和", difficulty: "medium", topics: ["哈希表"], tracks: ["interview"] },
    { id: "lc-00002", title: "三数之和", difficulty: "medium", topics: ["哈希表"], tracks: ["interview"] },
  ]
  const rounds = [
    makeRound("r33", "2026-07-12", [
      makeItem("first", "两数之和", "todo"),
      makeItem("second", "三数之和", "todo"),
    ]),
  ]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: same }))
  assert.strictEqual(action.problemId, "lc-00001", "同 topic 取清单里靠前的那题")
  db.close()
})

test("没有 active 轮次时退回主动探索", function () {
  const { db, store } = freshStore()
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.strictEqual(action.kind, "explore")
  assert.strictEqual(action.problemId, "lc-00001")
  db.close()
})

test("每次只给一个动作并带耗时估算", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.ok(action && !Array.isArray(action))
  assert.ok(action.estimatedMinutes > 0, "要有「这一步大约 N 分钟」")
  assert.strictEqual(action.title, "两数之和")
  assert.strictEqual(typeof action.reason, "string")
  db.close()
})

test("做过的题不会被当成探索目标", function () {
  const { db, store } = freshStore()
  addAttempt(store, "lc-00001", "pass")
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.strictEqual(action.problemId, "lc-00002")
  db.close()
})

test("没有任何可推的动作时返回 null", function () {
  const { db, store } = freshStore()
  const action = scheduler.nextAction(makeCtx(store, { problems: [] }))
  assert.strictEqual(action, null)
  db.close()
})

test("consumeQuota 把题数与分钟记到今天的键上", function () {
  const { db, store } = freshStore()
  const result = scheduler.consumeQuota(store, { questions: 1, minutes: 15 }, NOW)
  assert.strictEqual(result.questions, 1)
  assert.strictEqual(result.minutes, 15)
  assert.strictEqual(store.getSetting("review.used.questions." + localDayKey(NOW)), "1")
  assert.strictEqual(store.getSetting("review.used.minutes." + localDayKey(NOW)), "15")
  db.close()
})

test("quotaState 报出是哪一项配额用满了", function () {
  const { db, store } = freshStore()
  store.setSetting("review.used.questions." + localDayKey(NOW), "10")
  const state = scheduler.quotaState(store, NOW, store.allSettings())
  assert.strictEqual(state.exhausted, true)
  assert.strictEqual(state.exhaustedBy, "questions")
  assert.strictEqual(state.limitQuestions, 10)
  assert.strictEqual(state.limitMinutes, 20)
  db.close()
})

test("masteryByTopic 首次通过率低的主题分数更低", function () {
  const attempts = {
    "lc-00001": [{ problem_id: "lc-00001", verdict: "wrong_answer" }],
    "lc-00002": [{ problem_id: "lc-00002", verdict: "pass" }],
  }
  const mastery = scheduler.masteryByTopic(PROBLEMS, attempts)
  assert.ok(mastery["哈希表"] < mastery["双指针"], JSON.stringify(mastery))
})

/* ---------- 以下是计划之外的补充断言，把 G24 / G26 与边界钉死 ---------- */

test("G24 配额取先到者：题数与分钟同时到顶也算用完", function () {
  const { db, store } = freshStore()
  store.setSetting("review.used.questions." + localDayKey(NOW), "10")
  store.setSetting("review.used.minutes." + localDayKey(NOW), "20")
  const state = scheduler.quotaState(store, NOW, store.allSettings())
  assert.strictEqual(state.exhausted, true)
  assert.strictEqual(state.exhaustedBy, "questions")
  db.close()
})

test("G24 配额取先到者：分钟先到就停（题数还剩很多）", function () {
  const { db, store } = freshStore()
  store.setSetting("review.used.minutes." + localDayKey(NOW), "20")
  const state = scheduler.quotaState(store, NOW, store.allSettings())
  assert.strictEqual(state.exhausted, true)
  assert.strictEqual(state.exhaustedBy, "minutes")
  assert.strictEqual(state.questions, 0)
  db.close()
})

test("配额上限调成 0 时今天不推复习", function () {
  const { db, store } = freshStore()
  store.setSetting("review.daily_quota_questions", "0")
  store.setSetting("review.daily_quota_minutes", "0")
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  const state = scheduler.quotaState(store, NOW, store.allSettings())
  assert.strictEqual(state.limitQuestions, 0)
  assert.strictEqual(state.limitMinutes, 0)
  assert.strictEqual(state.exhausted, true)
  const action = scheduler.nextAction(makeCtx(store, { problems: PROBLEMS }))
  assert.strictEqual(action.kind, "explore", "配额为 0 时不给复习")
  db.close()
})

test("consumeQuota 累加多次消耗", function () {
  const { db, store } = freshStore()
  scheduler.consumeQuota(store, { questions: 1, minutes: 10 }, NOW)
  const second = scheduler.consumeQuota(store, { questions: 2, minutes: 5 }, NOW)
  assert.strictEqual(second.questions, 3)
  assert.strictEqual(second.minutes, 15)
  db.close()
})

test("G26 纯本地模式（没有轮次、题库为空）不抛错，只走复习分支", function () {
  const { db, store } = freshStore()
  addCard(store, "lc-00001", "2026-11-10T00:00:00.000Z")
  const action = scheduler.nextAction({
    store: store,
    provider: null,
    rounds: [],
    nowIso: NOW,
    settings: store.allSettings(),
  })
  assert.ok(action, "纯本地模式也要能推出动作")
  assert.strictEqual(action.kind, "review")
  assert.strictEqual(action.problem, null, "题库里没有这道题时 problem 为空")
  assert.strictEqual(action.title, "lc-00001", "没有题面时也要有标题可显示")
  db.close()
})

test("G26 纯本地模式既没有题库也没有轮次时返回 null 而不是抛错", function () {
  const { db, store } = freshStore()
  const action = scheduler.nextAction(makeCtx(store))
  assert.strictEqual(action, null)
  db.close()
})

test("轮次里的题都做完了就退回探索分支", function () {
  const { db, store } = freshStore()
  const rounds = [makeRound("r33", "2026-07-12", [makeItem("a", "两数之和", "done")])]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: PROBLEMS }))
  assert.strictEqual(action.kind, "explore")
  db.close()
})

test("dayKey 返回本地日期", function () {
  assert.strictEqual(scheduler.dayKey(NOW), localDayKey(NOW))
})

test("estimateMinutes 没有历史数据时按难度基准", function () {
  assert.strictEqual(scheduler.estimateMinutes({ difficulty: "easy" }, {}), 10)
  assert.strictEqual(scheduler.estimateMinutes({ difficulty: "medium" }, {}), 15)
  assert.strictEqual(scheduler.estimateMinutes({ difficulty: "hard" }, {}), 25)
  assert.strictEqual(scheduler.estimateMinutes(null, {}), 15)
})

test("estimateMinutes 取难度基准与历史平均用时的较大者", function () {
  // 历史平均 30 分钟（1800000 毫秒）比 medium 的基准 15 分钟长
  assert.strictEqual(scheduler.estimateMinutes({ difficulty: "medium" }, { medium: 1800000 }), 30)
  // 历史平均 5 分钟比基准短，取基准
  assert.strictEqual(scheduler.estimateMinutes({ difficulty: "medium" }, { medium: 300000 }), 15)
})

test("masteryByTopic 一个题都没练过的 topic 给 0.3", function () {
  const mastery = scheduler.masteryByTopic(PROBLEMS, {})
  assert.strictEqual(mastery["哈希表"], 0.3)
  assert.strictEqual(mastery["双指针"], 0.3)
})

test("轮次条目对不上题库时给出轮次号与条目号，但不硬塞一个题目 id", function () {
  const { db, store } = freshStore()
  // r32 的卡片是 Python 工具链，不在算法题库里
  const rounds = [makeRound("r32", "2026-07-12", [makeItem("virtual_env", "virtualenv", "todo")])]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: PROBLEMS }))
  assert.ok(action)
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.problemId, null, "题库里没有它，就别编一个题目 id")
  assert.strictEqual(action.roundId, "r32")
  assert.strictEqual(action.itemId, "virtual_env")
  db.close()
})

test("有能对上题库的轮次时，优先给对得上的那一轮", function () {
  const { db, store } = freshStore()
  const rounds = [
    // 最近创建的轮次，条目不在题库里
    makeRound("r32", "2026-07-20", [makeItem("virtual_env", "virtualenv", "todo")]),
    // 早一轮，但条目标题与题库标题对得上
    makeRound("r33", "2026-07-12", [makeItem("x", "两数之和", "todo")]),
  ]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: PROBLEMS }))
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.roundId, "r33")
  assert.strictEqual(action.problemId, "lc-00001")
  db.close()
})

test("一轮里既有对得上的也有对不上的，优先挑对得上的", function () {
  const { db, store } = freshStore()
  const rounds = [
    makeRound("r33", "2026-07-12", [
      makeItem("virtual_env", "virtualenv", "todo"),
      makeItem("x", "两数之和", "todo"),
    ]),
  ]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: PROBLEMS }))
  assert.strictEqual(action.problemId, "lc-00001")
  assert.strictEqual(action.itemId, "x")
  db.close()
})

test("题库为空时轮次照样推，只是没有 problemId", function () {
  const { db, store } = freshStore()
  const rounds = [makeRound("r32", "2026-07-12", [makeItem("virtual_env", "virtualenv", "todo")])]
  const action = scheduler.nextAction(makeCtx(store, { rounds: rounds, problems: [] }))
  assert.ok(action)
  assert.strictEqual(action.kind, "round")
  assert.strictEqual(action.problemId, null)
  db.close()
})
