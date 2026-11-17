"use strict"
/**
 * p1-quota.test.js —— 跨轨配额（最大余数法）
 *
 * 契约 §6.1：需求度 → 有效权重 → 最大余数法 → 候选回收 → 空池兜底。
 * 这里只验算法本身，不碰数据库。
 */

const test = require("node:test")
const assert = require("node:assert")

const { allocateQuota } = require("../core/scheduler/quota.js")

test("默认权重 0.4 / 0.3 / 0.3、会话长度 3：三条轨各分一个", function () {
  const r = allocateQuota({
    n: 3,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 20 },
      { track: "database", weight: 0.3, due: 0, candidates: 20 },
      { track: "ai", weight: 0.3, due: 0, candidates: 20 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 1, database: 1, ai: 1 })
  assert.strictEqual(r.needMore, false)
  assert.deepStrictEqual(r.demand, { algorithm: 1, database: 1, ai: 1 })
})

test("复习积压加权：到期多的轨需求度更高，分到的名额更多", function () {
  const r = allocateQuota({
    n: 4,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 20 },
      { track: "database", weight: 0.3, due: 10, candidates: 20 },
      { track: "ai", weight: 0.3, due: 0, candidates: 20 },
    ],
  })
  assert.strictEqual(r.demand.algorithm, 1)
  assert.strictEqual(r.demand.database, 2)
  assert.strictEqual(r.demand.ai, 1)
  assert.strictEqual(r.weights.database, 0.6)
  // w' = 0.4 / 0.6 / 0.3 → 精确值 1.2308 / 1.8462 / 0.9231
  // 取整后 1 / 1 / 0，余下两个名额按小数从大到小给 ai、database
  assert.deepStrictEqual(r.quota, { algorithm: 1, database: 2, ai: 1 })
})

test("候选不够的名额回收给还有候选的轨", function () {
  // 夹具要让「回收」真的发生：database 权重最大（该分到 2 个），但只有 1 个候选。
  // 压到 1 之后，多出来的那一个按有效权重分给还有候选的别的轨。
  const r = allocateQuota({
    n: 3,
    tracks: [
      { track: "algorithm", weight: 0.1, due: 0, candidates: 10 },
      { track: "database", weight: 0.8, due: 0, candidates: 1 },
      { track: "ai", weight: 0.1, due: 0, candidates: 10 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 1, database: 1, ai: 1 })
  assert.ok(r.reasons.join(" ").indexOf("数据库轨") !== -1, "回收原因要写中文轨名：" + r.reasons.join("|"))
})

test("所有轨都没候选：needMore 为 true，并给出中文提示", function () {
  const r = allocateQuota({
    n: 3,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 0 },
      { track: "database", weight: 0.3, due: 0, candidates: 0 },
      { track: "ai", weight: 0.3, due: 0, candidates: 0 },
    ],
  })
  assert.strictEqual(r.needMore, true)
  assert.deepStrictEqual(r.quota, { algorithm: 0, database: 0, ai: 0 })
  assert.ok(
    r.reasons.join(" ").indexOf("没有可学的单元，建议去供给流水线补内容") !== -1,
    "空池要提示去补内容：" + r.reasons.join("|")
  )
})

test("候选总数不够会话长度：只排得下现有的，needMore 为 true", function () {
  const r = allocateQuota({
    n: 5,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 2 },
      { track: "database", weight: 0.3, due: 0, candidates: 0 },
      { track: "ai", weight: 0.3, due: 0, candidates: 0 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 2, database: 0, ai: 0 })
  assert.strictEqual(r.needMore, true)
  assert.ok(r.reasons.join(" ").indexOf("不够") !== -1, "要说明排不满：" + r.reasons.join("|"))
})

test("权重全为 0：各轨等分，余数按声明顺序补给靠前的轨", function () {
  const r = allocateQuota({
    n: 4,
    tracks: [
      { track: "algorithm", weight: 0, due: 0, candidates: 10 },
      { track: "database", weight: 0, due: 0, candidates: 10 },
      { track: "ai", weight: 0, due: 0, candidates: 10 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 2, database: 1, ai: 1 })
})

test("minPerTrack 保底：名额不够时按声明顺序每轨先给一个", function () {
  const r = allocateQuota({
    n: 2,
    minPerTrack: 1,
    tracks: [
      { track: "algorithm", weight: 0.9, due: 0, candidates: 10 },
      { track: "database", weight: 0.1, due: 0, candidates: 10 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 1, database: 1 })
})

test("minPerTrack 保底：三条轨、名额不够时保底只给前 N 条，配额和仍等于会话长度", function () {
  // 两轨的夹具抓不住「保底分支被整个跳过」——两轨按权重分也正好是 1/1。
  // 三轨且权重悬殊时，不保底就会得到 2/0/0，保底才得到 1/1/0。
  // 并且「配额和恒等于 n」这个不变式在这里会被检验（保底不到位的常见后果是超出 n）。
  const r = allocateQuota({
    n: 2,
    minPerTrack: 1,
    tracks: [
      { track: "algorithm", weight: 0.9, due: 0, candidates: 10 },
      { track: "database", weight: 0.05, due: 0, candidates: 10 },
      { track: "ai", weight: 0.05, due: 0, candidates: 10 },
    ],
  })
  assert.deepStrictEqual(r.quota, { algorithm: 1, database: 1, ai: 0 })
  const sum = Object.keys(r.quota).reduce(function (acc, k) {
    return acc + r.quota[k]
  }, 0)
  assert.strictEqual(sum, 2, "配额和必须等于会话长度：" + JSON.stringify(r.quota))
})

test("minPerTrack 保底：名额够时先保底再把剩下的按权重分", function () {
  const r = allocateQuota({
    n: 5,
    minPerTrack: 1,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 10 },
      { track: "database", weight: 0.3, due: 0, candidates: 10 },
      { track: "ai", weight: 0.3, due: 0, candidates: 10 },
    ],
  })
  // 先各保底 1（用掉 3），剩下 2 个按 0.4/0.3/0.3 分 → 各轨 +0/+1/+1（算法余数最大 +1）
  assert.deepStrictEqual(r.quota, { algorithm: 2, database: 2, ai: 1 })
})

test("同输入必同输出", function () {
  const input = {
    n: 7,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 3, candidates: 30 },
      { track: "database", weight: 0.3, due: 7, candidates: 30 },
      { track: "ai", weight: 0.3, due: 1, candidates: 30 },
    ],
  }
  assert.deepStrictEqual(allocateQuota(input), allocateQuota(input))
})

test("没有轨道：needMore 为 true", function () {
  const r = allocateQuota({ n: 3, tracks: [] })
  assert.strictEqual(r.needMore, true)
  assert.deepStrictEqual(r.quota, {})
})

test("minPerTrack ≥ 2 且名额够保底时，剩下的名额不许丢（配额和恒等于 n）", function () {
  // 历史 bug：名额不够保底时旧实现「每轨只给 1 个」，把剩下的名额直接丢掉——
  // 三轨各 10 个候选、n=4、minPerTrack=2 时只排出 3 个，还谎称「可学的单元不够」。
  // 契约 §6.1「保底」的真实含义是「先保底，再按余数法分剩下的」。
  const r = allocateQuota({
    n: 4,
    minPerTrack: 2,
    tracks: [
      { track: "algorithm", weight: 0.4, due: 0, candidates: 10 },
      { track: "database", weight: 0.3, due: 0, candidates: 10 },
      { track: "ai", weight: 0.3, due: 0, candidates: 10 },
    ],
  })
  const sum = Object.keys(r.quota).reduce(function (acc, k) {
    return acc + r.quota[k]
  }, 0)
  assert.strictEqual(sum, 4, "候选有 30 个，绝不能只排 3 个：" + JSON.stringify(r.quota))
  assert.deepStrictEqual(r.quota, { algorithm: 2, database: 2, ai: 0 })
  assert.deepStrictEqual(r.reasons, [], "名额够排满时不该报「不够」：" + JSON.stringify(r.reasons))
})

test("配额恒等于 min(n, Σ候选)，且每轨配额不超候选数（不变式）", function () {
  // 穷举一批输入，只验两条不变式，不验具体分配值：
  //   1. Σquota === min(n, Σcandidates)  —— 名额不许丢，也不许凭空多出来
  //   2. quota[t] ≤ candidates[t]        —— 不许超过候选数
  const W = [0, 0.1, 0.4, 0.9]
  const C = [0, 1, 2, 5, 20]
  const N = [1, 2, 3, 5, 7]
  const MP = [0, 1, 2, 3]
  const names = ["algorithm", "database", "ai"]
  let cases = 0
  N.forEach(function (n) {
    MP.forEach(function (mp) {
      W.forEach(function (w0) {
        C.forEach(function (c0) {
          C.forEach(function (c1) {
            C.forEach(function (c2) {
              cases += 1
              const r = allocateQuota({
                n: n,
                minPerTrack: mp,
                tracks: [
                  { track: "algorithm", weight: w0, due: 0, candidates: c0 },
                  { track: "database", weight: w0, due: 0, candidates: c1 },
                  { track: "ai", weight: w0, due: 0, candidates: c2 },
                ],
              })
              const sum = names.reduce(function (acc, t) {
                return acc + (r.quota[t] || 0)
              }, 0)
              const expect = Math.min(n, c0 + c1 + c2)
              assert.strictEqual(
                sum,
                expect,
                "n=" + n + " mp=" + mp + " C=[" + c0 + "," + c1 + "," + c2 + "] → " + JSON.stringify(r.quota)
              )
              assert.ok((r.quota.algorithm || 0) <= c0, "algorithm 超候选")
              assert.ok((r.quota.database || 0) <= c1, "database 超候选")
              assert.ok((r.quota.ai || 0) <= c2, "ai 超候选")
            })
          })
        })
      })
    })
  })
  assert.ok(cases > 0)
})
