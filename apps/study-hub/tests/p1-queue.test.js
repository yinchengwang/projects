"use strict"
/**
 * p1-queue.test.js —— 队列成形与跨轨交错
 *
 * 轨内顺序由调用方给，本函数不重排；跨轨 round-robin；seq 从 1 开始连续。
 */

const test = require("node:test")
const assert = require("node:assert")

const { buildQueue } = require("../core/scheduler/queue.js")

function u(id) {
  return { id: id, track: id.split(":")[0], kind: "problem", title: id, status: "verified" }
}

function ids(queue) {
  return queue.map(function (item) {
    return item.unit.id
  })
}

function trackOf(id) {
  return id.split(":")[0]
}

function maxStreak(list) {
  let best = 0
  let streak = 0
  let prev = null
  list.forEach(function (id) {
    const t = trackOf(id)
    streak = t === prev ? streak + 1 : 1
    prev = t
    if (streak > best) best = streak
  })
  return best
}

test("seq 从 1 开始连续", function () {
  const queue = buildQueue({
    quota: { algorithm: 2, database: 1 },
    pools: {
      algorithm: [u("algo:lc-00001"), u("algo:lc-00002")],
      database: [u("db:mvcc")],
    },
  })
  assert.deepStrictEqual(
    queue.map(function (item) {
      return item.seq
    }),
    [1, 2, 3]
  )
})

test("跨轨 round-robin 交错：多轨都有货时必然逐条换轨", function () {
  // round-robin 的游标总指向上一条轨的下一个位置，所以多轨都有货时 streak 恒为 1
  // ——这是「不需要连续同轨上限」的直接证据，比「不超过配额两倍」这条弱断言强得多
  // （后者对一个恒为 1 的 streak 永远成立，变异体「删掉上限」根本抓不住）。
  const queue = buildQueue({
    quota: { algorithm: 3, database: 2 },
    pools: {
      algorithm: [u("algo:lc-00001"), u("algo:lc-00002"), u("algo:lc-00003")],
      database: [u("db:mvcc"), u("db:transaction")],
    },
  })
  const list = ids(queue)
  assert.strictEqual(list.length, 5)
  // 精确断言整个交错序列（不只断言长度与「混进来了」）
  assert.deepStrictEqual(list, ["algo:lc-00001", "db:mvcc", "algo:lc-00002", "db:transaction", "algo:lc-00003"])
  assert.strictEqual(maxStreak(list), 1, "多轨都有货时不该出现连续同轨：" + list.join(","))
})

test("轨内顺序原样保留，不重排", function () {
  const queue = buildQueue({
    quota: { algorithm: 2 },
    pools: { algorithm: [u("algo:lc-00009"), u("algo:lc-00001")] },
  })
  assert.deepStrictEqual(ids(queue), ["algo:lc-00009", "algo:lc-00001"])
})

test("只有一条轨有名额时，全部排这一轨", function () {
  const queue = buildQueue({
    quota: { algorithm: 3, database: 0, ai: 0 },
    pools: {
      algorithm: [u("algo:lc-00001"), u("algo:lc-00002"), u("algo:lc-00003")],
      database: [],
      ai: [],
    },
  })
  assert.deepStrictEqual(ids(queue), ["algo:lc-00001", "algo:lc-00002", "algo:lc-00003"])
})

test("池子比配额短时只取现有的", function () {
  const queue = buildQueue({
    quota: { algorithm: 3 },
    pools: { algorithm: [u("algo:lc-00001")] },
  })
  assert.deepStrictEqual(ids(queue), ["algo:lc-00001"])
})

test("配额为 0 的轨不排", function () {
  const queue = buildQueue({
    quota: { algorithm: 1, database: 0 },
    pools: { algorithm: [u("algo:lc-00001")], database: [u("db:mvcc")] },
  })
  assert.deepStrictEqual(ids(queue), ["algo:lc-00001"])
})

test("配额用完的轨即使池子里还有货，也不许再排（游标可能停在它上面）", function () {
  // 上一条用例抓不住「取数时忘了检查剩余配额」——因为 database 那个单元在
  // alive 判断阶段就被排除了，游标根本走不到它。
  // 这条让游标**恰好停在一条配额已用完、但池子里还有货的轨上**：
  //   第 1 条取 algorithm（配额用尽）→ 游标转向 database → 第 2 条取 database
  //   → 游标又回到 algorithm，此时它配额已为 0 但 pool 里还留着 algo:lc-00002。
  // 忘了检查剩余配额，就会把 algo:lc-00002 违规排进来（algorithm 出现 2 次）。
  const queue = buildQueue({
    quota: { algorithm: 1, database: 2 },
    pools: {
      algorithm: [u("algo:lc-00001"), u("algo:lc-00002")],
      database: [u("db:mvcc"), u("db:transaction")],
    },
  })
  const list = ids(queue)
  assert.strictEqual(list.length, 3, "一共只有 3 个名额：" + list.join(","))
  assert.strictEqual(
    list.filter(function (id) {
      return trackOf(id) === "algo"
    }).length,
    1,
    "algorithm 配额是 1，不许排第 2 个：" + list.join(",")
  )
})
// 注：`alive` 过滤里的 `s.left > 0` 与内层取数的 `s.left <= 0` 是一处冗余——
// 下面这条用例证明真正的守门人是**内层取数**（去掉它，用例立刻红）；
// 去掉 alive 里的 left 判断不会改变任何输出（被内层拦住了），属于等价变异体，不必为它补测试。

test("空输入返回空数组", function () {
  assert.deepStrictEqual(buildQueue({ quota: {}, pools: {} }), [])
})
